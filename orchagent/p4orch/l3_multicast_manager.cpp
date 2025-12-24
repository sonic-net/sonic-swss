#include "p4orch/l3_multicast_manager.h"

#include <memory>
#include <nlohmann/json.hpp>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "SaiAttributeList.h"
#include "converter.h"
#include "dbconnector.h"
#include "ipaddress.h"
#include "logger.h"
#include "p4orch/p4oidmapper.h"
#include "p4orch/p4orch_util.h"
#include "portsorch.h"
#include "sai_serialize.h"
#include "swssnet.h"
#include "table.h"
#include "vrforch.h"

extern "C" {
#include "sai.h"
}

using ::p4orch::kTableKeyDelimiter;

extern sai_object_id_t gSwitchId;
extern sai_object_id_t gVirtualRouterId;
extern sai_ipmc_group_api_t* sai_ipmc_group_api;
extern sai_router_interface_api_t* sai_router_intfs_api;

extern PortsOrch* gPortsOrch;

namespace p4orch {

namespace {

void fillStatusArrayWithNotExecuted(std::vector<ReturnCode>& array,
                                    size_t startIndex) {
  for (size_t i = startIndex; i < array.size(); ++i) {
    array[i] = ReturnCode(StatusCode::SWSS_RC_NOT_EXECUTED);
  }
}

// Create the vector of SAI attributes for creating a new RIF object.
ReturnCodeOr<std::vector<sai_attribute_t>> prepareRifSaiAttrs(
    const P4MulticastRouterInterfaceEntry& multicast_router_interface_entry) {
  Port port;
  if (!gPortsOrch->getPort(
          multicast_router_interface_entry.multicast_replica_port, port)) {
    LOG_ERROR_AND_RETURN(
        ReturnCode(StatusCode::SWSS_RC_NOT_FOUND)
        << "Failed to get port info for multicast_replica_port "
        << QuotedVar(multicast_router_interface_entry.multicast_replica_port));
  }

  std::vector<sai_attribute_t> attrs;
  sai_attribute_t attr;
  // Map all P4 router interfaces to default VRF as virtual router is mandatory
  // parameter for creation of router interfaces in SAI.
  attr.id = SAI_ROUTER_INTERFACE_ATTR_VIRTUAL_ROUTER_ID;
  attr.value.oid = gVirtualRouterId;
  attrs.push_back(attr);

  attr.id = SAI_ROUTER_INTERFACE_ATTR_TYPE;
  attr.value.s32 = SAI_ROUTER_INTERFACE_TYPE_PORT;
  attrs.push_back(attr);
  if (port.m_type != Port::PHY) {
    // If we need to support LAG, VLAN, or other types, we can make this a
    // case statement like:
    // https://source.corp.google.com/h/nss/codesearch/+/master:third_party/
    // sonic-swss/orchagent/p4orch/router_interface_manager.cpp;l=90
    LOG_ERROR_AND_RETURN(ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
                         << "Unexpected port type: " << port.m_type);
  }

  attr.id = SAI_ROUTER_INTERFACE_ATTR_PORT_ID;
  attr.value.oid = port.m_port_id;
  attrs.push_back(attr);

  attr.id = SAI_ROUTER_INTERFACE_ATTR_MTU;
  attr.value.u32 = port.m_mtu;
  attrs.push_back(attr);

  attr.id = SAI_ROUTER_INTERFACE_ATTR_SRC_MAC_ADDRESS;
  memcpy(attr.value.mac, multicast_router_interface_entry.src_mac.getMac(),
         sizeof(sai_mac_t));
  attrs.push_back(attr);

  attr.id = SAI_ROUTER_INTERFACE_ATTR_V4_MCAST_ENABLE;
  attr.value.booldata = true;
  attrs.push_back(attr);

  attr.id = SAI_ROUTER_INTERFACE_ATTR_V6_MCAST_ENABLE;
  attr.value.booldata = true;
  attrs.push_back(attr);

  return attrs;
}

}  // namespace

L3MulticastManager::L3MulticastManager(P4OidMapper* mapper, VRFOrch* vrfOrch,
                                       ResponsePublisherInterface* publisher)
    : m_p4OidMapper(mapper), m_vrfOrch(vrfOrch) {
  SWSS_LOG_ENTER();
  assert(publisher != nullptr);
  m_publisher = publisher;
}

ReturnCode L3MulticastManager::getSaiObject(const std::string& json_key,
                                            sai_object_type_t& object_type,
                                            std::string& object_key) {
  return StatusCode::SWSS_RC_UNIMPLEMENTED;
}

// Since we subscribe to two table types, this function handles table entries
// of two different types.
void L3MulticastManager::enqueue(const std::string& table_name,
                                 const swss::KeyOpFieldsValuesTuple& entry) {
  m_entries.push_back(entry);
}

ReturnCode L3MulticastManager::drain() {
  SWSS_LOG_ENTER();
  // This manager subscribes to two tables.  We will drain pending entries
  // based on the table.  A table-specific drain function will process entries
  // associated with each table.
  std::string prev_table;

  // Pending tuples (unverified) to process for a given table.
  std::deque<swss::KeyOpFieldsValuesTuple> tuple_list;
  ReturnCode status;

  while (!m_entries.empty()) {
    auto key_op_fvs_tuple = m_entries.front();
    m_entries.pop_front();
    std::string table_name;
    std::string key;
    parseP4RTKey(kfvKey(key_op_fvs_tuple), &table_name, &key);

    if (prev_table == "") {
      prev_table = table_name;
    }

    // We have moved on to a different table, so drain the previous entries.
    if (table_name != prev_table) {
      if (prev_table == APP_P4RT_MULTICAST_ROUTER_INTERFACE_TABLE_NAME) {
        // This drain function will drain unexecuted entries upon failure.
        status = drainMulticastRouterInterfaceEntries(tuple_list);
      } else if (prev_table == APP_P4RT_REPLICATION_L2_MULTICAST_TABLE_NAME) {
        // This drain function will drain unexecuted entries upon failure.
        status = drainMulticastReplicationEntries(tuple_list);
      } else {
        status = ReturnCode(StatusCode::SWSS_RC_NOT_EXECUTED)
                 << "Unexpected table " << QuotedVar(prev_table);
        // Drain tuples associated with unknown table as not executed.
        drainMgmtWithNotExecuted(tuple_list, m_publisher);
      }
      prev_table = table_name;
    }
    if (!status.ok()) {
      // The entry we popped has not been processed yet.
      // Return SWSS_RC_NOT_EXECUTED.
      m_publisher->publish(APP_P4RT_TABLE_NAME, kfvKey(key_op_fvs_tuple),
                           kfvFieldsValues(key_op_fvs_tuple),
                           ReturnCode(StatusCode::SWSS_RC_NOT_EXECUTED),
                           /*replace=*/true);
      break;
    } else {
      tuple_list.push_back(key_op_fvs_tuple);
    }
  }  // while

  // If no failure, process any pending entries associated with the table.
  if (status.ok() && !tuple_list.empty()) {
    if (prev_table == APP_P4RT_MULTICAST_ROUTER_INTERFACE_TABLE_NAME) {
      status = drainMulticastRouterInterfaceEntries(tuple_list);
    } else if (prev_table == APP_P4RT_REPLICATION_L2_MULTICAST_TABLE_NAME) {
      status = drainMulticastReplicationEntries(tuple_list);
    } else {
      status = ReturnCode(StatusCode::SWSS_RC_NOT_EXECUTED)
               << "Unexpected table " << QuotedVar(prev_table);
      // Drain tuples associated with unknown table as not executed.
      drainMgmtWithNotExecuted(tuple_list, m_publisher);
    }
  }
  drainWithNotExecuted();  // drain the main queue
  return status;
}

// Drain entries associated with the multicast router interface table.
ReturnCode L3MulticastManager::drainMulticastRouterInterfaceEntries(
    std::deque<swss::KeyOpFieldsValuesTuple>& router_interface_tuples) {
  SWSS_LOG_ENTER();

  ReturnCode status;
  std::vector<P4MulticastRouterInterfaceEntry>
      multicast_router_interface_entry_list;
  std::deque<swss::KeyOpFieldsValuesTuple> tuple_list;

  std::string prev_op;
  bool prev_update = false;

  while (!router_interface_tuples.empty()) {
    auto key_op_fvs_tuple = router_interface_tuples.front();
    router_interface_tuples.pop_front();
    std::string table_name;
    std::string key;
    parseP4RTKey(kfvKey(key_op_fvs_tuple), &table_name, &key);
    const std::vector<swss::FieldValueTuple>& attributes =
        kfvFieldsValues(key_op_fvs_tuple);

    // Form entry object
    auto router_interface_entry_or =
        deserializeMulticastRouterInterfaceEntry(key, attributes);

    if (!router_interface_entry_or.ok()) {
      status = router_interface_entry_or.status();
      SWSS_LOG_ERROR("Unable to deserialize APP DB entry with key %s: %s",
                     QuotedVar(table_name + ":" + key).c_str(),
                     status.message().c_str());
      m_publisher->publish(APP_P4RT_TABLE_NAME, kfvKey(key_op_fvs_tuple),
                           kfvFieldsValues(key_op_fvs_tuple), status,
                           /*replace=*/true);
      break;
    }
    auto& router_interface_entry = *router_interface_entry_or;

    // Validate entry
    const std::string& operation = kfvOp(key_op_fvs_tuple);
    status = validateMulticastRouterInterfaceEntry(router_interface_entry,
                                                   operation);
    if (!status.ok()) {
      SWSS_LOG_ERROR(
          "Validation failed for router interface APP DB entry with key %s: %s",
          QuotedVar(table_name + ":" + key).c_str(), status.message().c_str());
      m_publisher->publish(APP_P4RT_TABLE_NAME, kfvKey(key_op_fvs_tuple),
                           kfvFieldsValues(key_op_fvs_tuple), status,
                           /*replace=*/true);
      break;
    }

    // Now, start processing batch of entries.
    auto* router_interface_entry_ptr = getMulticastRouterInterfaceEntry(
        router_interface_entry.multicast_router_interface_entry_key);
    bool update = router_interface_entry_ptr != nullptr;

    if (prev_op == "") {
      prev_op = operation;
      prev_update = update;
    }
    // Process the entries if the operation type changes.
    if (operation != prev_op || update != prev_update) {
      status = processMulticastRouterInterfaceEntries(
          multicast_router_interface_entry_list, tuple_list, prev_op,
          prev_update);
      multicast_router_interface_entry_list.clear();
      tuple_list.clear();
      prev_op = operation;
      prev_update = update;
    }

    if (!status.ok()) {
      // Return SWSS_RC_NOT_EXECUTED if failure has occured.
      m_publisher->publish(APP_P4RT_TABLE_NAME, kfvKey(key_op_fvs_tuple),
                           kfvFieldsValues(key_op_fvs_tuple),
                           ReturnCode(StatusCode::SWSS_RC_NOT_EXECUTED),
                           /*replace=*/true);
      break;
    } else {
      multicast_router_interface_entry_list.push_back(router_interface_entry);
      tuple_list.push_back(key_op_fvs_tuple);
    }
  }  // while

  // Process any pending entries.
  if (!multicast_router_interface_entry_list.empty()) {
    auto rc = processMulticastRouterInterfaceEntries(
        multicast_router_interface_entry_list, tuple_list, prev_op,
        prev_update);
    if (!rc.ok()) {
      status = rc;
    }
  }
  drainMgmtWithNotExecuted(router_interface_tuples, m_publisher);
  return status;
}

// Drain entries associated with the multicast replication table, and those
// only.
ReturnCode L3MulticastManager::drainMulticastReplicationEntries(
    std::deque<swss::KeyOpFieldsValuesTuple>& replication_tuples) {
  return ReturnCode(StatusCode::SWSS_RC_UNIMPLEMENTED)
         << "L3MulticastManager::drainMulticastReplicationEntries is not "
         << "implemented yet";
}

ReturnCodeOr<P4MulticastRouterInterfaceEntry>
L3MulticastManager::deserializeMulticastRouterInterfaceEntry(
    const std::string& key,
    const std::vector<swss::FieldValueTuple>& attributes) {
  SWSS_LOG_ENTER();

  P4MulticastRouterInterfaceEntry router_interface_entry = {};
  try {
    nlohmann::json j = nlohmann::json::parse(key);
    router_interface_entry.multicast_replica_port =
        j[prependMatchField(p4orch::kMulticastReplicaPort)];
    router_interface_entry.multicast_replica_instance =
        j[prependMatchField(p4orch::kMulticastReplicaInstance)];
  } catch (std::exception& ex) {
    return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
           << "Failed to deserialize multicast router interface table key";
  }

  router_interface_entry.multicast_router_interface_entry_key =
      KeyGenerator::generateMulticastRouterInterfaceKey(
          router_interface_entry.multicast_replica_port,
          router_interface_entry.multicast_replica_instance);

  for (const auto& it : attributes) {
    const auto& field = fvField(it);
    const auto& value = fvValue(it);
    if (field == p4orch::kAction) {
      if (value != p4orch::kSetSrcMac) {
        return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
               << "Unexpected action " << QuotedVar(value) << " in "
               << APP_P4RT_MULTICAST_ROUTER_INTERFACE_TABLE_NAME;
      }
    } else if (field == prependParamField(p4orch::kSrcMac)) {
      router_interface_entry.src_mac = swss::MacAddress(value);
    } else if (field == prependParamField(p4orch::kMulticastMetadata)) {
      router_interface_entry.multicast_metadata = value;
    } else if (field != p4orch::kControllerMetadata) {
      return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
             << "Unexpected field " << QuotedVar(field) << " in "
             << APP_P4RT_MULTICAST_ROUTER_INTERFACE_TABLE_NAME;
    }
  }
  return router_interface_entry;
}

ReturnCodeOr<P4MulticastReplicationEntry>
L3MulticastManager::deserializeMulticastReplicationEntry(
    const std::string& key,
    const std::vector<swss::FieldValueTuple>& attributes) {
  SWSS_LOG_ENTER();
  P4MulticastReplicationEntry replication_entry = {};
  try {
    nlohmann::json j = nlohmann::json::parse(key);
    replication_entry.multicast_group_id =
        j[prependMatchField(p4orch::kMulticastGroupId)];
    replication_entry.multicast_replica_port =
        j[prependMatchField(p4orch::kMulticastReplicaPort)];
    replication_entry.multicast_replica_instance =
        j[prependMatchField(p4orch::kMulticastReplicaInstance)];
  } catch (std::exception& ex) {
    return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
           << "Failed to deserialize multicast replication table key";
  }

  replication_entry.multicast_replication_key =
      KeyGenerator::generateMulticastReplicationKey(
          replication_entry.multicast_group_id,
          replication_entry.multicast_replica_port,
          replication_entry.multicast_replica_instance);

  for (const auto& it : attributes) {
    const auto& field = fvField(it);
    const auto& value = fvValue(it);
    if (field == p4orch::kAction) {
      // This table has no actions.
    } else if (field == prependParamField(p4orch::kMulticastMetadata)) {
      replication_entry.multicast_metadata = value;
    } else if (field != p4orch::kControllerMetadata) {
      return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
             << "Unexpected field " << QuotedVar(field) << " in "
             << APP_P4RT_REPLICATION_L2_MULTICAST_TABLE_NAME;
    }
  }
  return replication_entry;
}

void L3MulticastManager::drainWithNotExecuted() {
  drainMgmtWithNotExecuted(m_entries, m_publisher);
}

std::string L3MulticastManager::verifyState(
    const std::string& key, const std::vector<swss::FieldValueTuple>& tuple) {
  SWSS_LOG_ENTER();

  auto pos = key.find_first_of(kTableKeyDelimiter);
  if (pos == std::string::npos) {
    return std::string("Invalid key, missing delimiter: ") + key;
  }
  std::string p4rt_table = key.substr(0, pos);
  std::string p4rt_key = key.substr(pos + 1);
  if (p4rt_table != APP_P4RT_TABLE_NAME) {
    return std::string("Invalid key, unexpected P4RT table: ") + key;
  }
  std::string table_name;
  std::string key_content;
  parseP4RTKey(p4rt_key, &table_name, &key_content);
  if (table_name == APP_P4RT_MULTICAST_ROUTER_INTERFACE_TABLE_NAME) {
    return verifyMulticastRouterInterfaceState(key_content, tuple);
  } else if (table_name == APP_P4RT_REPLICATION_L2_MULTICAST_TABLE_NAME) {
    return verifyMulticastReplicationState(key_content, tuple);
  } else {
    return std::string("Invalid key, unexpected table name: ") + key;
  }
}

std::string L3MulticastManager::verifyMulticastRouterInterfaceState(
    const std::string& key,
    const std::vector<swss::FieldValueTuple>& tuple) {
  auto app_db_entry_or = deserializeMulticastRouterInterfaceEntry(
      key, tuple);
  if (!app_db_entry_or.ok()) {
    ReturnCode status = app_db_entry_or.status();
    std::stringstream msg;
    msg << "Unable to deserialize key " << QuotedVar(key) << ": "
        << status.message();
    return msg.str();
  }
  auto& app_db_entry = *app_db_entry_or;

  const std::string router_interface_entry_key =
      KeyGenerator::generateMulticastRouterInterfaceKey(
          app_db_entry.multicast_replica_port,
          app_db_entry.multicast_replica_instance);
  auto* router_interface_entry_ptr =
      getMulticastRouterInterfaceEntry(router_interface_entry_key);
  if (router_interface_entry_ptr == nullptr) {
    std::stringstream msg;
    msg << "No entry found with key " << QuotedVar(key);
    return msg.str();
  }

  std::string cache_result = verifyMulticastRouterInterfaceStateCache(
      app_db_entry, router_interface_entry_ptr);
  std::string asic_db_result = verifyMulticastRouterInterfaceStateAsicDb(
      router_interface_entry_ptr);
  if (cache_result.empty()) {
    return asic_db_result;
  }
  if (asic_db_result.empty()) {
    return cache_result;
  }
  return cache_result + "; " + asic_db_result;
}

std::string L3MulticastManager::verifyMulticastReplicationState(
    const std::string& key,
    const std::vector<swss::FieldValueTuple>& tuple) {
  return "L3MulticastManager::verifyMulticastReplicationState is not "
         "implemented";
}

ReturnCode L3MulticastManager::validateMulticastRouterInterfaceEntry(
    const P4MulticastRouterInterfaceEntry& multicast_router_interface_entry,
    const std::string& operation) {
  // Confirm match fields are populated.
  if (multicast_router_interface_entry.multicast_replica_port.empty()) {
    return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
           << "No match field entry multicast_replica_port provided";
  }
  if (multicast_router_interface_entry.multicast_replica_instance.empty()) {
    return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
           << "No match field entry multicast_replica_instance provided";
  }

  if (operation == SET_COMMAND) {
    return validateSetMulticastRouterInterfaceEntry(
        multicast_router_interface_entry);
  } else if (operation == DEL_COMMAND) {
    return validateDelMulticastRouterInterfaceEntry(
        multicast_router_interface_entry);
  }
  return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
         << "Unknown operation type " << QuotedVar(operation);
}

ReturnCode L3MulticastManager::validateMulticastReplicationEntry(
    const P4MulticastReplicationEntry& multicast_replication_entry,
    const std::string& operation) {
  return ReturnCode(StatusCode::SWSS_RC_UNIMPLEMENTED)
         << "L3MulticastManager::validateMulticastReplicationEntry is not "
         << "implemented";
}

ReturnCode L3MulticastManager::validateSetMulticastRouterInterfaceEntry(
    const P4MulticastRouterInterfaceEntry& multicast_router_interface_entry) {
  auto* router_interface_entry_ptr = getMulticastRouterInterfaceEntry(
      multicast_router_interface_entry.multicast_router_interface_entry_key);

  bool is_update_operation = router_interface_entry_ptr != nullptr;
  if (is_update_operation) {
    // Confirm RIF had SAI object ID.
    if (router_interface_entry_ptr->router_interface_oid ==
        SAI_OBJECT_TYPE_NULL) {
      return ReturnCode(StatusCode::SWSS_RC_NOT_FOUND)
             << "RIF was not assigned before updating multicast router "
                "interface "
                "entry with keys "
             << QuotedVar(
                    multicast_router_interface_entry.multicast_replica_port)
             << " and "
             << QuotedVar(multicast_router_interface_entry
                              .multicast_replica_instance);
    }

    // Confirm we have a reference to the RIF object ID.
    if (m_rifOidToRouterInterfaceEntries.find(
            router_interface_entry_ptr->router_interface_oid) ==
        m_rifOidToRouterInterfaceEntries.end()) {
      return ReturnCode(StatusCode::SWSS_RC_NOT_FOUND)
             << "Expected RIF OID is missing from map: "
             << router_interface_entry_ptr->router_interface_oid;
    }

    // Confirm the RIF object ID exists in central mapper.
    std::string rif_key = KeyGenerator::generateMulticastRouterInterfaceRifKey(
        router_interface_entry_ptr->multicast_replica_port,
        router_interface_entry_ptr->src_mac);
    bool exist_in_mapper =
        m_p4OidMapper->existsOID(SAI_OBJECT_TYPE_ROUTER_INTERFACE, rif_key);
    if (!exist_in_mapper) {
      return ReturnCode(StatusCode::SWSS_RC_NOT_FOUND)
             << "Multicast router interface entry exists in manager but RIF "
                "does "
                "not exist in the centralized map";
    }
  }
  // No additional validation required for add operation.
  return ReturnCode();
}

ReturnCode L3MulticastManager::validateDelMulticastRouterInterfaceEntry(
    const P4MulticastRouterInterfaceEntry& multicast_router_interface_entry) {
  auto* router_interface_entry_ptr = getMulticastRouterInterfaceEntry(
      multicast_router_interface_entry.multicast_router_interface_entry_key);

  // Can't delete what isn't there.
  if (router_interface_entry_ptr == nullptr) {
    return ReturnCode(StatusCode::SWSS_RC_NOT_FOUND)
           << "Multicast router interface entry exists does not exist";
  }

  // Confirm we have a reference to the RIF object ID.
  if (m_rifOidToRouterInterfaceEntries.find(
          router_interface_entry_ptr->router_interface_oid) ==
      m_rifOidToRouterInterfaceEntries.end()) {
    return ReturnCode(StatusCode::SWSS_RC_NOT_FOUND)
           << "Expected RIF OID is missing from map: "
           << router_interface_entry_ptr->router_interface_oid;
  }

  // Confirm the RIF object ID exists in central mapper.
  std::string rif_key = KeyGenerator::generateMulticastRouterInterfaceRifKey(
      multicast_router_interface_entry.multicast_replica_port,
      multicast_router_interface_entry.src_mac);
  if (!m_p4OidMapper->existsOID(SAI_OBJECT_TYPE_ROUTER_INTERFACE, rif_key)) {
    RETURN_INTERNAL_ERROR_AND_RAISE_CRITICAL(
        "Multicast router interface entry does not exist in the central map");
  }
  return ReturnCode();
}

ReturnCode L3MulticastManager::processMulticastRouterInterfaceEntries(
    std::vector<P4MulticastRouterInterfaceEntry>& entries,
    const std::deque<swss::KeyOpFieldsValuesTuple>& tuple_list,
    const std::string& op, bool update) {
  SWSS_LOG_ENTER();

  ReturnCode status;
  std::vector<ReturnCode> statuses;
  // In syncd, bulk SAI calls use mode SAI_BULK_OP_ERROR_MODE_STOP_ON_ERROR.
  if (op == SET_COMMAND) {
    if (!update) {
      statuses = addMulticastRouterInterfaceEntries(entries);
    } else {
      statuses = updateMulticastRouterInterfaceEntries(entries);
    }
  } else {
    statuses = deleteMulticastRouterInterfaceEntries(entries);
  }
  // Check status of each entry.
  for (size_t i = 0; i < entries.size(); ++i) {
    m_publisher->publish(APP_P4RT_TABLE_NAME, kfvKey(tuple_list[i]),
                         kfvFieldsValues(tuple_list[i]), statuses[i],
                         /*replace=*/true);
    if (status.ok() && !statuses[i].ok()) {
      status = statuses[i];
    }
  }
  return status;
}

ReturnCode L3MulticastManager::createRouterInterface(
    const std::string& rif_key, P4MulticastRouterInterfaceEntry& entry,
    sai_object_id_t* rif_oid) {
  SWSS_LOG_ENTER();

  // Confirm we haven't already created a RIF for this.
  if (m_p4OidMapper->existsOID(SAI_OBJECT_TYPE_ROUTER_INTERFACE, rif_key)) {
    RETURN_INTERNAL_ERROR_AND_RAISE_CRITICAL(
        "Router interface to be used by multicast router interface table "
        << QuotedVar(rif_key) << " already exists in the centralized map");
  }

  // Create RIF SAI object.
  ASSIGN_OR_RETURN(std::vector<sai_attribute_t> attrs,
                   prepareRifSaiAttrs(entry));
  auto sai_status = sai_router_intfs_api->create_router_interface(
      rif_oid, gSwitchId, (uint32_t)attrs.size(), attrs.data());
  if (sai_status != SAI_STATUS_SUCCESS) {
    LOG_ERROR_AND_RETURN(
        ReturnCode(sai_status)
        << "Failed to create router interface for multicast router interface "
        << "table: " << QuotedVar(rif_key).c_str());
  }
  return ReturnCode();
}

ReturnCode L3MulticastManager::deleteRouterInterface(const std::string& rif_key,
                                                     sai_object_id_t rif_oid) {
  SWSS_LOG_ENTER();
  // Confirm we have a RIF to be deleted.
  if (!m_p4OidMapper->existsOID(SAI_OBJECT_TYPE_ROUTER_INTERFACE, rif_key)) {
    LOG_ERROR_AND_RETURN(
        ReturnCode(StatusCode::SWSS_RC_INTERNAL)
        << "Router interface to be deleted by multicast router interface table "
        << QuotedVar(rif_key) << " does not exist in the centralized map");
  }
  auto sai_status = sai_router_intfs_api->remove_router_interface(rif_oid);
  if (sai_status != SAI_STATUS_SUCCESS) {
    LOG_ERROR_AND_RETURN(
        ReturnCode(sai_status)
        << "Failed to remove router interface for multicast router interface "
        << "table: " << QuotedVar(rif_key).c_str());
  }
  return ReturnCode();
}

ReturnCode L3MulticastManager::createMulticastGroup(
    P4MulticastReplicationEntry& entry, sai_object_id_t* mcast_group_oid) {
  return ReturnCode(StatusCode::SWSS_RC_UNIMPLEMENTED)
         << "L3MulticastManager::createMulticastGroup is not implemented";
}

ReturnCode L3MulticastManager::createMulticastGroupMember(
    const P4MulticastReplicationEntry& entry, const sai_object_id_t rif_oid,
    sai_object_id_t* mcast_group_member_oid) {
  return ReturnCode(StatusCode::SWSS_RC_UNIMPLEMENTED)
         << "L3MulticastManager::createMulticastGroupMember is not implemented";
}

ReturnCode L3MulticastManager::deleteMulticastGroup(
    const std::string& multicast_group_id, sai_object_id_t mcast_group_oid) {
  return ReturnCode(StatusCode::SWSS_RC_UNIMPLEMENTED)
         << "L3MulticastManager::deleteMulticastGroup is not implemented";
}

std::vector<ReturnCode> L3MulticastManager::addMulticastRouterInterfaceEntries(
    std::vector<P4MulticastRouterInterfaceEntry>& entries) {
  // There are two cases for add:
  // 1. The new entry (multicast_replica_port, multicast_replica_instance) will
  //    need a new RIF allocated.
  // 2. The new entry will be able to use an existing RIF.
  // Recall that RIFs are created based on multicast_replica_port and Ethernet
  // src mac, and src mac is the action parameter associated with a table entry.
  SWSS_LOG_ENTER();

  std::vector<ReturnCode> statuses(entries.size());
  fillStatusArrayWithNotExecuted(statuses, 0);
  for (size_t i = 0; i < entries.size(); ++i) {
    auto& entry = entries[i];

    sai_object_id_t rif_oid = getRifOid(&entry);
    if (rif_oid == SAI_NULL_OBJECT_ID) {
      std::string rif_key =
          KeyGenerator::generateMulticastRouterInterfaceRifKey(
              entry.multicast_replica_port, entry.src_mac);

      ReturnCode create_status =
          createRouterInterface(rif_key, entry, &rif_oid);
      statuses[i] = create_status;
      if (!create_status.ok()) {
        break;
      }

      gPortsOrch->increasePortRefCount(entry.multicast_replica_port);
      m_p4OidMapper->setOID(SAI_OBJECT_TYPE_ROUTER_INTERFACE, rif_key, rif_oid);
      m_rifOids[rif_key] = rif_oid;
      m_rifOidToMulticastGroupMembers[rif_oid] = {};
    }

    // Operations done regardless of whether RIF was created or not.
    // Set the entry RIF.
    entry.router_interface_oid = rif_oid;

    // Update internal state.
    m_multicastRouterInterfaceTable[entry
                                        .multicast_router_interface_entry_key] =
        entry;
    m_rifOidToRouterInterfaceEntries[rif_oid].push_back(entry);

    statuses[i] = ReturnCode();
  }  // for i
  return statuses;
}

std::vector<ReturnCode>
L3MulticastManager::updateMulticastRouterInterfaceEntries(
    std::vector<P4MulticastRouterInterfaceEntry>& entries) {
  SWSS_LOG_ENTER();
  std::vector<ReturnCode> statuses(entries.size());
  fillStatusArrayWithNotExecuted(statuses, 0);

  for (size_t i = 0; i < entries.size(); ++i) {
    auto& entry = entries[i];
    auto* old_entry_ptr = getMulticastRouterInterfaceEntry(
        entry.multicast_router_interface_entry_key);
    if (old_entry_ptr == nullptr) {
      statuses[i] = ReturnCode(StatusCode::SWSS_RC_INTERNAL)
                    << "Multicast router interface entry is missing "
                    << QuotedVar(entry.multicast_router_interface_entry_key);
      break;
    }

    // No change to src mac means there is nothing to do.
    if (old_entry_ptr->src_mac == entry.src_mac) {
      SWSS_LOG_INFO(
          "No update required for %s because the src mac did not change",
          QuotedVar(entry.multicast_router_interface_entry_key).c_str());
      statuses[i] = ReturnCode();
      continue;
    }

    // Confirm RIF OID was assigned (for the old entry).
    sai_object_id_t old_rif_oid = getRifOid(old_entry_ptr);
    std::string old_rif_key =
        KeyGenerator::generateMulticastRouterInterfaceRifKey(
            old_entry_ptr->multicast_replica_port, old_entry_ptr->src_mac);
    if (old_rif_oid == SAI_NULL_OBJECT_ID) {
      statuses[i] =
          ReturnCode(StatusCode::SWSS_RC_INTERNAL)
          << "Multicast router interface entry is missing a RIF oid "
          << QuotedVar(old_entry_ptr->multicast_router_interface_entry_key);
      break;
    }

    // Fetch the vector P4MulticastRouterInterfaceEntry associated with the RIF.
    if (m_rifOidToRouterInterfaceEntries.find(old_rif_oid) ==
        m_rifOidToRouterInterfaceEntries.end()) {
      statuses[i] =
          ReturnCode(StatusCode::SWSS_RC_INTERNAL)
          << "RIF oid " << old_rif_oid << " missing from map for "
          << QuotedVar(old_entry_ptr->multicast_router_interface_entry_key);
      break;
    }
    auto& old_entries_for_rif = m_rifOidToRouterInterfaceEntries[old_rif_oid];
    auto old_entry_with_rif = std::find_if(
        old_entries_for_rif.begin(), old_entries_for_rif.end(),
        [&](const P4MulticastRouterInterfaceEntry& x) {
          return x.multicast_router_interface_entry_key ==
                 old_entry_ptr->multicast_router_interface_entry_key;
        });
    if ((old_entry_with_rif == old_entries_for_rif.end()) ||
        (m_multicastRouterInterfaceTable.find(
             old_entry_ptr->multicast_router_interface_entry_key) ==
         m_multicastRouterInterfaceTable.end())) {
      statuses[i] =
          ReturnCode(StatusCode::SWSS_RC_INTERNAL)
          << "Unable to find entry "
          << QuotedVar(old_entry_ptr->multicast_router_interface_entry_key)
          << " in map";
      break;
    }

    // If we will delete the RIF, confirm there are no more multicast group
    // members using it.
    if (old_entries_for_rif.size() == 1) {
      if (m_rifOidToMulticastGroupMembers.find(old_rif_oid) !=
          m_rifOidToMulticastGroupMembers.end()) {
        if (m_rifOidToMulticastGroupMembers[old_rif_oid].size() > 0) {
          statuses[i] = ReturnCode(StatusCode::SWSS_RC_IN_USE)
                        << "RIF oid " << old_rif_oid << " cannot be deleted, "
                        << "because it is still used by multicast group "
                        << "members";
          break;
        }
      }
    }

    if (m_rifOidToMulticastGroupMembers.find(old_rif_oid) !=
        m_rifOidToMulticastGroupMembers.end()) {
      SWSS_LOG_INFO("Old RIF OID %ld was used by %lu multicast group members",
                    old_rif_oid,
                    m_rifOidToMulticastGroupMembers[old_rif_oid].size());
    }

    // Check if new RIF already exists.
    // If it doesn't exist, we will have to create one.
    bool created_new_rif = false;
    std::string rif_key = KeyGenerator::generateMulticastRouterInterfaceRifKey(
        entry.multicast_replica_port, entry.src_mac);

    sai_object_id_t new_rif_oid = getRifOid(&entry);
    // We create a new RIF instead of updating an existing RIF's src mac
    // attribute, in case multiple router interface entry tables references
    // the same RIF.
    if (new_rif_oid == SAI_NULL_OBJECT_ID) {
      ReturnCode create_status =
          createRouterInterface(rif_key, entry, &new_rif_oid);
      statuses[i] = create_status;
      if (!create_status.ok()) {
        break;
      }
      created_new_rif = true;
      // Internal book-keeping is done after all SAI calls have been performed.
    }

    // If this entry was the last one associated with the old RIF, we can
    // remove that interface.
    if (old_entries_for_rif.size() == 1) {
      ReturnCode delete_status =
          deleteRouterInterface(old_rif_key, old_rif_oid);
      statuses[i] = delete_status;
      if (!delete_status.ok()) {
        break;
      }

      m_p4OidMapper->eraseOID(SAI_OBJECT_TYPE_ROUTER_INTERFACE, old_rif_key);
      gPortsOrch->decreasePortRefCount(old_entry_ptr->multicast_replica_port);

      // Since old RIF no longer in use, delete from maps.
      old_entries_for_rif.erase(old_entry_with_rif);
      m_rifOidToRouterInterfaceEntries.erase(old_rif_oid);
      m_rifOidToMulticastGroupMembers.erase(old_rif_oid);
      m_rifOids.erase(old_rif_key);
    } else {
      old_entries_for_rif.erase(old_entry_with_rif);
    }

    // Always done book keeping.
    entry.router_interface_oid = new_rif_oid;
    m_multicastRouterInterfaceTable.erase(
        old_entry_ptr->multicast_router_interface_entry_key);
    // We removed the old P4MulticastRouterInterfaceEntry from the RIF to
    // entries vector in the block above.
    m_multicastRouterInterfaceTable[entry
                                        .multicast_router_interface_entry_key] =
        entry;
    m_rifOidToRouterInterfaceEntries[new_rif_oid].push_back(entry);
    m_rifOidToMulticastGroupMembers[new_rif_oid] = {};

    // Do RIF creation internal accounting at the end to avoid having to back
    // out on delete failure.
    if (created_new_rif) {
      gPortsOrch->increasePortRefCount(entry.multicast_replica_port);
      m_p4OidMapper->setOID(SAI_OBJECT_TYPE_ROUTER_INTERFACE, rif_key,
                            new_rif_oid);
      m_rifOids[rif_key] = new_rif_oid;
    }
    statuses[i] = ReturnCode();
  }  // for entries
  return statuses;
}

std::vector<ReturnCode>
L3MulticastManager::deleteMulticastRouterInterfaceEntries(
    const std::vector<P4MulticastRouterInterfaceEntry>& entries) {
  SWSS_LOG_ENTER();
  std::vector<ReturnCode> statuses(entries.size());
  fillStatusArrayWithNotExecuted(statuses, 0);

  // There are two cases for removal:
  // 1. This entry is the last one associated with the RIF.  In such a case,
  //    delete the RIF and clear it from appropriate maps.
  // 2. There will still be other entries associated with the RIF.  In such a
  //    case, only remove the current entry from being associated with the RIF.
  for (size_t i = 0; i < entries.size(); ++i) {
    auto& entry = entries[i];
    if (m_multicastRouterInterfaceTable.find(
            entry.multicast_router_interface_entry_key) ==
        m_multicastRouterInterfaceTable.end()) {
      statuses[i] = ReturnCode(StatusCode::SWSS_RC_UNKNOWN)
                    << "Multicast router interface entry is not known "
                    << QuotedVar(entry.multicast_router_interface_entry_key);
      break;
    }

    // Confirm RIF OID was assigned.
    sai_object_id_t rif_oid = getRifOid(&entry);
    if (rif_oid == SAI_NULL_OBJECT_ID) {
      statuses[i] = ReturnCode(StatusCode::SWSS_RC_INTERNAL)
                    << "Multicast router interface entry is missing a RIF oid "
                    << QuotedVar(entry.multicast_router_interface_entry_key);
      break;
    }

    // Confirm there are no more multicast group members using the RIF.
    if (m_rifOidToMulticastGroupMembers.find(rif_oid) !=
        m_rifOidToMulticastGroupMembers.end()) {
      if (m_rifOidToMulticastGroupMembers[rif_oid].size() > 0) {
        statuses[i] = ReturnCode(StatusCode::SWSS_RC_IN_USE)
                      << "RIF oid " << rif_oid << " cannot be deleted, because "
                      << "it is still used by multicast group members.";
        break;
      }
    }

    // Confirm there is at least one P4MulticastRouterInterfaceEntry associated
    // with the RIF.
    if (m_rifOidToRouterInterfaceEntries.find(rif_oid) ==
        m_rifOidToRouterInterfaceEntries.end()) {
      statuses[i] = ReturnCode(StatusCode::SWSS_RC_INTERNAL)
                    << "RIF oid " << rif_oid << " missing from map for "
                    << QuotedVar(entry.multicast_router_interface_entry_key);
      break;
    }
    auto& entries_for_rif = m_rifOidToRouterInterfaceEntries[rif_oid];
    auto entry_with_rif =
        std::find_if(entries_for_rif.begin(), entries_for_rif.end(),
                     [&](const P4MulticastRouterInterfaceEntry& x) {
                       return x.multicast_router_interface_entry_key ==
                              entry.multicast_router_interface_entry_key;
                     });
    if ((entry_with_rif == entries_for_rif.end()) ||
        (m_multicastRouterInterfaceTable.find(
             entry.multicast_router_interface_entry_key) ==
         m_multicastRouterInterfaceTable.end())) {
      statuses[i] = ReturnCode(StatusCode::SWSS_RC_INTERNAL)
                    << "Unable to find entry "
                    << QuotedVar(entry.multicast_router_interface_entry_key)
                    << " in map";
      break;
    }
    std::string rif_key = KeyGenerator::generateMulticastRouterInterfaceRifKey(
        entry.multicast_replica_port, entry.src_mac);

    // If this is the last entry, delete the RIF.
    // Attempt to delete RIF at SAI layer before adjusting internal maps, in
    // case there is an error.
    if (entries_for_rif.size() == 1) {
      ReturnCode delete_status = deleteRouterInterface(rif_key, rif_oid);
      statuses[i] = delete_status;
      if (!delete_status.ok()) {
        break;
      }

      m_p4OidMapper->eraseOID(SAI_OBJECT_TYPE_ROUTER_INTERFACE, rif_key);
      gPortsOrch->decreasePortRefCount(entry.multicast_replica_port);

      // Delete entry from list.
      entries_for_rif.erase(entry_with_rif);
      // Since RIF no longer in use, delete from maps.
      m_rifOidToRouterInterfaceEntries.erase(rif_oid);
      m_rifOidToMulticastGroupMembers.erase(rif_oid);
      m_rifOids.erase(rif_key);
    } else {
      // Delete entry from list.
      entries_for_rif.erase(entry_with_rif);
    }

    // Finally, remove the entry P4MulticastRouterInterfaceEntry.
    m_multicastRouterInterfaceTable.erase(
        entry.multicast_router_interface_entry_key);
    statuses[i] = ReturnCode();
  }  // for i
  return statuses;
}

std::vector<ReturnCode> L3MulticastManager::addMulticastReplicationEntries(
    std::vector<P4MulticastReplicationEntry>& entries) {
  std::vector<ReturnCode> rv;
  rv.push_back(ReturnCode(StatusCode::SWSS_RC_UNIMPLEMENTED)
               << "L3MulticastManager::addMulticastReplicationEntries is not "
               << "implemented");
  return rv;
}

std::vector<ReturnCode> L3MulticastManager::deleteMulticastReplicationEntries(
    const std::vector<P4MulticastReplicationEntry>& entries) {
  std::vector<ReturnCode> rv;
  rv.push_back(ReturnCode(StatusCode::SWSS_RC_UNIMPLEMENTED)
               << "L3MulticastManager::deleteMulticastReplicationEntries is "
               << "not implemented");
  return rv;
}

std::string L3MulticastManager::verifyMulticastRouterInterfaceStateCache(
    const P4MulticastRouterInterfaceEntry& app_db_entry,
    const P4MulticastRouterInterfaceEntry* multicast_router_interface_entry) {
  const std::string router_interface_entry_key =
      KeyGenerator::generateMulticastRouterInterfaceKey(
          app_db_entry.multicast_replica_port,
          app_db_entry.multicast_replica_instance);

  ReturnCode status = validateMulticastRouterInterfaceEntry(app_db_entry,
                                                            SET_COMMAND);
  if (!status.ok()) {
    std::stringstream msg;
    msg << "Validation failed for multicast router interface DB entry with key "
        << QuotedVar(router_interface_entry_key) << ": " << status.message();
    return msg.str();
  }
  if (multicast_router_interface_entry->multicast_router_interface_entry_key !=
      app_db_entry.multicast_router_interface_entry_key) {
    std::stringstream msg;
    msg << "Multicast router interface interface entry key "
        << QuotedVar(app_db_entry.multicast_router_interface_entry_key)
        << " does not match internal cache "
        << QuotedVar(multicast_router_interface_entry->multicast_router_interface_entry_key)
        << " in l3 multicast manager.";
    return msg.str();
  }
  if (multicast_router_interface_entry->multicast_replica_port !=
      app_db_entry.multicast_replica_port) {
    std::stringstream msg;
    msg << "Output port name " << QuotedVar(app_db_entry.multicast_replica_port)
        << " does not match internal cache "
        << QuotedVar(multicast_router_interface_entry->multicast_replica_port)
        << " in l3 multicast manager.";
    return msg.str();
  }
  if (multicast_router_interface_entry->multicast_replica_instance !=
      app_db_entry.multicast_replica_instance) {
    std::stringstream msg;
    msg << "Egress instance "
        << QuotedVar(app_db_entry.multicast_replica_instance)
        << " does not match internal cache "
        << QuotedVar(
               multicast_router_interface_entry->multicast_replica_instance)
        << " in l3 multicast manager.";
    return msg.str();
  }
  if (multicast_router_interface_entry->src_mac.to_string() !=
      app_db_entry.src_mac.to_string()) {
    std::stringstream msg;
    msg << "Src MAC " << QuotedVar(app_db_entry.src_mac.to_string())
        << " does not match internal cache "
        << QuotedVar(multicast_router_interface_entry->src_mac.to_string())
        << " in l3 multicast manager.";
    return msg.str();
  }
  if (multicast_router_interface_entry->multicast_metadata !=
      app_db_entry.multicast_metadata) {
    std::stringstream msg;
    msg << "Multicast metadata " << QuotedVar(app_db_entry.multicast_metadata)
        << " does not match internal cache "
        << QuotedVar(multicast_router_interface_entry->multicast_metadata)
        << " in l3 multicast manager.";
    return msg.str();
  }
  std::string rif_key = KeyGenerator::generateMulticastRouterInterfaceRifKey(
      multicast_router_interface_entry->multicast_replica_port,
      multicast_router_interface_entry->src_mac);
  return m_p4OidMapper->verifyOIDMapping(
      SAI_OBJECT_TYPE_ROUTER_INTERFACE, rif_key,
      multicast_router_interface_entry->router_interface_oid);
}

std::string L3MulticastManager::verifyMulticastRouterInterfaceStateAsicDb(
    const P4MulticastRouterInterfaceEntry* multicast_router_interface_entry) {
  auto attrs_or = prepareRifSaiAttrs(*multicast_router_interface_entry);
  if (!attrs_or.ok()) {
    return std::string("Failed to get multicast router interface SAI attrs: ") +
           attrs_or.status().message();
  }
  std::vector<sai_attribute_t> attrs = *attrs_or;
  std::vector<swss::FieldValueTuple> exp =
      saimeta::SaiAttributeList::serialize_attr_list(
          SAI_OBJECT_TYPE_ROUTER_INTERFACE, (uint32_t)attrs.size(),
          attrs.data(), /*countOnly=*/false);

  swss::DBConnector db("ASIC_DB", 0);
  swss::Table table(&db, "ASIC_STATE");
  std::string key =
      sai_serialize_object_type(SAI_OBJECT_TYPE_ROUTER_INTERFACE) + ":" +
      sai_serialize_object_id(
          multicast_router_interface_entry->router_interface_oid);
  std::vector<swss::FieldValueTuple> values;
  if (!table.get(key, values)) {
    return std::string("ASIC DB key not found ") + key;
  }

  return verifyAttrs(values, exp, std::vector<swss::FieldValueTuple>{},
                     /*allow_unknown=*/false);
}

std::string L3MulticastManager::verifyMulticastReplicationStateCache(
    const P4MulticastReplicationEntry& app_db_entry,
    const P4MulticastReplicationEntry* multicast_replication_entry) {
  return "L3MulticastManager::verifyMulticastReplicationStateCache is not "
         "implemented";
}

std::string L3MulticastManager::verifyMulticastReplicationStateAsicDb(
    const P4MulticastReplicationEntry* multicast_replication_entry) {
  return "L3MulticastManager::verifyMulticastReplicationStateAsicDb is not "
         "implemented";
}

P4MulticastRouterInterfaceEntry*
L3MulticastManager::getMulticastRouterInterfaceEntry(
    const std::string& multicast_router_interface_entry_key) {
  SWSS_LOG_ENTER();
  if (m_multicastRouterInterfaceTable.find(
          multicast_router_interface_entry_key) ==
      m_multicastRouterInterfaceTable.end()) {
    return nullptr;
  }
  return &m_multicastRouterInterfaceTable[multicast_router_interface_entry_key];
}

P4MulticastReplicationEntry* L3MulticastManager::getMulticastReplicationEntry(
    const std::string& multicast_replication_key) {
  SWSS_LOG_ENTER();
  if (m_multicastReplicationTable.find(multicast_replication_key) ==
      m_multicastReplicationTable.end()) {
    return nullptr;
  }
  return &m_multicastReplicationTable[multicast_replication_key];
}

// A RIF is associated with an egress port and Ethernet src mac value.
sai_object_id_t L3MulticastManager::getRifOid(
    const P4MulticastRouterInterfaceEntry* multicast_router_interface_entry) {
  std::string rif_key = KeyGenerator::generateMulticastRouterInterfaceRifKey(
      multicast_router_interface_entry->multicast_replica_port,
      multicast_router_interface_entry->src_mac);
  if (m_rifOids.find(rif_key) == m_rifOids.end()) {
    return SAI_NULL_OBJECT_ID;
  }
  return m_rifOids[rif_key];
}

}  // namespace p4orch
