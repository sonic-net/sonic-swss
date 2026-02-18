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
extern sai_bridge_api_t* sai_bridge_api;

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

// Create the vector of SAI attributes for creating a new multicast group
// member object.
std::vector<sai_attribute_t> prepareMulticastGroupMemberSaiAttrs(
    const P4MulticastReplicationEntry& multicast_replication_entry,
    const sai_object_id_t rif_oid) {
  std::vector<sai_attribute_t> attrs;
  sai_attribute_t attr;

  attr.id = SAI_IPMC_GROUP_MEMBER_ATTR_IPMC_GROUP_ID;
  attr.value.oid = multicast_replication_entry.multicast_group_oid;
  attrs.push_back(attr);

  attr.id = SAI_IPMC_GROUP_MEMBER_ATTR_IPMC_OUTPUT_ID;
  attr.value.oid = rif_oid;
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
  SWSS_LOG_ENTER();
  ReturnCode status;
  std::vector<P4MulticastReplicationEntry> multicast_replication_entry_list;
  std::deque<swss::KeyOpFieldsValuesTuple> tuple_list;

  std::string prev_op;
  bool prev_update = false;

  while (!replication_tuples.empty()) {
    auto key_op_fvs_tuple = replication_tuples.front();
    replication_tuples.pop_front();
    std::string table_name;
    std::string key;
    parseP4RTKey(kfvKey(key_op_fvs_tuple), &table_name, &key);
    const std::vector<swss::FieldValueTuple>& attributes =
        kfvFieldsValues(key_op_fvs_tuple);

    // Form entry object
    auto replication_entry_or =
        deserializeMulticastReplicationEntry(key, attributes);

    if (!replication_entry_or.ok()) {
      status = replication_entry_or.status();
      SWSS_LOG_ERROR("Unable to deserialize APP DB entry with key %s: %s",
                     QuotedVar(table_name + ":" + key).c_str(),
                     status.message().c_str());
      m_publisher->publish(APP_P4RT_TABLE_NAME, kfvKey(key_op_fvs_tuple),
                           kfvFieldsValues(key_op_fvs_tuple), status,
                           /*replace=*/true);
      break;
    }
    auto& replication_entry = *replication_entry_or;

    // Validate entry
    const std::string& operation = kfvOp(key_op_fvs_tuple);
    status = validateMulticastReplicationEntry(replication_entry, operation);
    if (!status.ok()) {
      SWSS_LOG_ERROR(
          "Validation failed for replication APP DB entry with key  %s: %s",
          QuotedVar(table_name + ":" + key).c_str(), status.message().c_str());
      m_publisher->publish(APP_P4RT_TABLE_NAME, kfvKey(key_op_fvs_tuple),
                           kfvFieldsValues(key_op_fvs_tuple), status,
                           /*replace=*/true);
      break;
    }

    // Now, start processing batch of entries.
    auto* replication_entry_ptr = getMulticastReplicationEntry(
        replication_entry.multicast_replication_key);
    bool update = replication_entry_ptr != nullptr;

    if (prev_op == "") {
      prev_op = operation;
      prev_update = update;
    }
    // Process the entries if the operation type changes.
    if (operation != prev_op || update != prev_update) {
      status = processMulticastReplicationEntries(
          multicast_replication_entry_list, tuple_list, prev_op, prev_update);
      multicast_replication_entry_list.clear();
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
      multicast_replication_entry_list.push_back(replication_entry);
      tuple_list.push_back(key_op_fvs_tuple);
    }
  }  // while

  // Process any pending entries.
  if (!multicast_replication_entry_list.empty()) {
    auto rc = processMulticastReplicationEntries(
        multicast_replication_entry_list, tuple_list, prev_op, prev_update);
    if (!rc.ok()) {
      status = rc;
    }
  }

  drainMgmtWithNotExecuted(replication_tuples, m_publisher);
  return status;
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
      if (value == p4orch::kSetSrcMac || value == p4orch::kNoAction) {
        router_interface_entry.action = value;
      } else {
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
  auto app_db_entry_or = deserializeMulticastReplicationEntry(key, tuple);
  if (!app_db_entry_or.ok()) {
    ReturnCode status = app_db_entry_or.status();
    std::stringstream msg;
    msg << "Unable to deserialize key " << QuotedVar(key) << ": "
        << status.message();
    return msg.str();
  }
  auto& app_db_entry = *app_db_entry_or;

  const std::string replication_entry_key =
      KeyGenerator::generateMulticastReplicationKey(
          app_db_entry.multicast_group_id, app_db_entry.multicast_replica_port,
          app_db_entry.multicast_replica_instance);
  auto* replication_entry_ptr =
      getMulticastReplicationEntry(replication_entry_key);
  if (replication_entry_ptr == nullptr) {
    std::stringstream msg;
    msg << "No entry found with key " << QuotedVar(key);
    return msg.str();
  }

  std::string cache_result =
      verifyMulticastReplicationStateCache(app_db_entry, replication_entry_ptr);
  std::string asic_db_result =
      verifyMulticastReplicationStateAsicDb(replication_entry_ptr);
  if (cache_result.empty()) {
    return asic_db_result;
  }
  if (asic_db_result.empty()) {
    return cache_result;
  }
  return cache_result + "; " + asic_db_result;
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
  // Confirm match fields are populated.
  if (multicast_replication_entry.multicast_group_id.empty()) {
    return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
           << "No match field entry multicast_group_id provided";
  }
  if (multicast_replication_entry.multicast_replica_port.empty()) {
    return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
           << "No match field entry multicast_replica_port provided";
  }
  if (multicast_replication_entry.multicast_replica_instance.empty()) {
    return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
           << "No match field entry multicast_replica_instance provided";
  }

  if (operation == SET_COMMAND) {
    return validateSetMulticastReplicationEntry(multicast_replication_entry);
  } else if (operation == DEL_COMMAND) {
    return validateDelMulticastReplicationEntry(multicast_replication_entry);
  }
  return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
         << "Unknown operation type " << QuotedVar(operation);
}

ReturnCode L3MulticastManager::validateSetMulticastReplicationEntry(
    const P4MulticastReplicationEntry& multicast_replication_entry) {
  auto* replication_entry_ptr = getMulticastReplicationEntry(
      multicast_replication_entry.multicast_replication_key);

  sai_object_id_t rif_oid = getRifOid(&multicast_replication_entry);
  if (rif_oid == SAI_NULL_OBJECT_ID) {
    return ReturnCode(StatusCode::SWSS_RC_NOT_FOUND)
           << "Multicast group member "
           << QuotedVar(multicast_replication_entry.multicast_replication_key)
           << " cannot be created, since there is associated RIF available yet";
  }

  bool is_update_operation = replication_entry_ptr != nullptr;
  if (is_update_operation) {
    // Confirm multicast group had SAI object ID.
    if (replication_entry_ptr->multicast_group_oid == SAI_OBJECT_TYPE_NULL) {
      return ReturnCode(StatusCode::SWSS_RC_NOT_FOUND)
             << "Multicast group OID was not assigned before updating "
                "multicast "
                "replication entry with keys "
             << QuotedVar(multicast_replication_entry.multicast_group_id)
             << ", "
             << QuotedVar(multicast_replication_entry.multicast_replica_port)
             << ", and "
             << QuotedVar(
                    multicast_replication_entry.multicast_replica_instance);
    }

    // Confirm multicast group member had SAI object ID.
    if (replication_entry_ptr->multicast_group_member_oid ==
        SAI_OBJECT_TYPE_NULL) {
      return ReturnCode(StatusCode::SWSS_RC_NOT_FOUND)
             << "Multicast group member OID was not assigned before updating "
                "multicast replication entry with keys "
             << QuotedVar(multicast_replication_entry.multicast_group_id)
             << ", "
             << QuotedVar(multicast_replication_entry.multicast_replica_port)
             << ", and "
             << QuotedVar(
                    multicast_replication_entry.multicast_replica_instance);
    }

    // Confirm we have references to the multicast group in internal maps.
    if (!m_p4OidMapper->existsOID(SAI_OBJECT_TYPE_IPMC_GROUP,
                                  replication_entry_ptr->multicast_group_id)) {
      return ReturnCode(StatusCode::SWSS_RC_NOT_FOUND)
             << "Expected multicast group is missing from oid map: "
             << replication_entry_ptr->multicast_replication_key;
    }
    if (m_multicastGroupMembers.find(
            replication_entry_ptr->multicast_group_id) ==
        m_multicastGroupMembers.end()) {
      return ReturnCode(StatusCode::SWSS_RC_NOT_FOUND)
             << "Expected multicast group is missing from entry map: "
             << replication_entry_ptr->multicast_group_id;
    }

    // Confirm the multicast object ID exists in central mapper.
    bool exist_in_mapper = m_p4OidMapper->existsOID(
        SAI_OBJECT_TYPE_IPMC_GROUP, replication_entry_ptr->multicast_group_id);
    if (!exist_in_mapper) {
      return ReturnCode(StatusCode::SWSS_RC_NOT_FOUND)
             << "Multicast replication entry exists in manager but multicast "
                "group"
                " OID does not exist in the centralized map";
    }
    // Confirm the multicast member object ID exists in central mapper.
    exist_in_mapper = m_p4OidMapper->existsOID(
        SAI_OBJECT_TYPE_IPMC_GROUP_MEMBER,
        replication_entry_ptr->multicast_replication_key);
    if (!exist_in_mapper) {
      return ReturnCode(StatusCode::SWSS_RC_NOT_FOUND)
             << "Multicast replication entry exists in manager but multicast "
                "group"
                " OID does not exist in the centralized map";
    }
  }
  // No additional validation required for add operation.
  return ReturnCode();
}

ReturnCode L3MulticastManager::validateDelMulticastReplicationEntry(
    const P4MulticastReplicationEntry& multicast_replication_entry) {
  auto* replication_entry_ptr = getMulticastReplicationEntry(
      multicast_replication_entry.multicast_replication_key);

  // Can't delete what isn't there.
  if (replication_entry_ptr == nullptr) {
    return ReturnCode(StatusCode::SWSS_RC_NOT_FOUND)
           << "Multicast replication entry does not exist";
  }

  // Confirm multicast group had SAI object ID.
  if (replication_entry_ptr->multicast_group_oid == SAI_OBJECT_TYPE_NULL) {
    return ReturnCode(StatusCode::SWSS_RC_NOT_FOUND)
           << "Multicast group OID was not assigned before updating multicast "
              "replication entry with keys "
           << QuotedVar(multicast_replication_entry.multicast_group_id) << ", "
           << QuotedVar(multicast_replication_entry.multicast_replica_port)
           << ", and "
           << QuotedVar(multicast_replication_entry.multicast_replica_instance);
  }

  if (replication_entry_ptr->multicast_group_member_oid ==
      SAI_OBJECT_TYPE_NULL) {
    return ReturnCode(StatusCode::SWSS_RC_NOT_FOUND)
           << "Multicast group member OID was not assigned before deleting "
              "multicast replication entry with keys "
           << QuotedVar(multicast_replication_entry.multicast_group_id) << ", "
           << QuotedVar(multicast_replication_entry.multicast_replica_port)
           << ", and "
           << QuotedVar(multicast_replication_entry.multicast_replica_instance);
  }

  // Confirm the multicast object ID exists in central mapper.
  bool exist_in_mapper = m_p4OidMapper->existsOID(
      SAI_OBJECT_TYPE_IPMC_GROUP, replication_entry_ptr->multicast_group_id);
  if (!exist_in_mapper) {
    return ReturnCode(StatusCode::SWSS_RC_NOT_FOUND)
           << "Multicast replication entry exists in manager but multicast "
              "group"
              " OID does not exist in the centralized map";
  }
  // Confirm the multicast member object ID exists in central mapper.
  exist_in_mapper = m_p4OidMapper->existsOID(
      SAI_OBJECT_TYPE_IPMC_GROUP_MEMBER,
      replication_entry_ptr->multicast_replication_key);
  if (!exist_in_mapper) {
    return ReturnCode(StatusCode::SWSS_RC_NOT_FOUND)
           << "Multicast replication entry exists in manager but multicast "
              "group"
              " OID does not exist in the centralized map";
  }
  return ReturnCode();
}

ReturnCode L3MulticastManager::validateL3SetMulticastRouterInterfaceEntry(
    const P4MulticastRouterInterfaceEntry& multicast_router_interface_entry,
    const P4MulticastRouterInterfaceEntry* router_interface_entry_ptr) {
  // Confirm RIF had SAI object ID.
  if (router_interface_entry_ptr->router_interface_oid ==
      SAI_OBJECT_TYPE_NULL) {
    return ReturnCode(StatusCode::SWSS_RC_NOT_FOUND)
           << "RIF was not assigned before updating multicast router "
              "interface "
              "entry with keys "
	   << QuotedVar(multicast_router_interface_entry.multicast_replica_port)
           << " and "
           << QuotedVar(
                  multicast_router_interface_entry.multicast_replica_instance);
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
  return ReturnCode();
}

ReturnCode L3MulticastManager::validateL2SetMulticastRouterInterfaceEntry(
    const P4MulticastRouterInterfaceEntry& multicast_router_interface_entry,
    const P4MulticastRouterInterfaceEntry* router_interface_entry_ptr) {
  // TODO: This needs to be implemented.
  return ReturnCode(StatusCode::SWSS_RC_UNIMPLEMENTED);
}

ReturnCode L3MulticastManager::validateSetMulticastRouterInterfaceEntry(
    const P4MulticastRouterInterfaceEntry& multicast_router_interface_entry) {
  auto* router_interface_entry_ptr = getMulticastRouterInterfaceEntry(
      multicast_router_interface_entry.multicast_router_interface_entry_key);

  // Confirm action is populated.
  if (multicast_router_interface_entry.action.empty()) {
    return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
           << "Multicast router interface entry did not specify an action.";
  }

  bool is_update_operation = router_interface_entry_ptr != nullptr;
  if (is_update_operation) {
    // Confirm action did not change.
    if (multicast_router_interface_entry.action !=
        router_interface_entry_ptr->action) {
      return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
             << "Multicast router interface entry with key "
	     << QuotedVar(multicast_router_interface_entry
                              .multicast_router_interface_entry_key)
             << " cannot change action from "
             << QuotedVar(router_interface_entry_ptr->action) << " to "
             << QuotedVar(multicast_router_interface_entry.action);
    }

    if (multicast_router_interface_entry.action ==
        p4orch::kSetSrcMac) {
      return validateL3SetMulticastRouterInterfaceEntry(
          multicast_router_interface_entry, router_interface_entry_ptr);
    } else {
      return validateL2SetMulticastRouterInterfaceEntry(
          multicast_router_interface_entry, router_interface_entry_ptr);
    }
  }
  // No additional validation required for add operation.
  return ReturnCode();
}

ReturnCode L3MulticastManager::validateL2DelMulticastRouterInterfaceEntry(
    const P4MulticastRouterInterfaceEntry& multicast_router_interface_entry,
    const P4MulticastRouterInterfaceEntry* router_interface_entry_ptr) {
  // TODO: This needs to be implemented.
  return ReturnCode(StatusCode::SWSS_RC_UNIMPLEMENTED);
}

ReturnCode L3MulticastManager::validateL3DelMulticastRouterInterfaceEntry(
    const P4MulticastRouterInterfaceEntry& multicast_router_interface_entry,
    const P4MulticastRouterInterfaceEntry* router_interface_entry_ptr) {

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
      router_interface_entry_ptr
          ->src_mac);  // No attributes provided on delete.
  if (!m_p4OidMapper->existsOID(SAI_OBJECT_TYPE_ROUTER_INTERFACE, rif_key)) {
    RETURN_INTERNAL_ERROR_AND_RAISE_CRITICAL(
        "Multicast router interface entry does not exist in the central map");
  }

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

  if (router_interface_entry_ptr->action == p4orch::kSetSrcMac) {
    return validateL3DelMulticastRouterInterfaceEntry(
        multicast_router_interface_entry, router_interface_entry_ptr);
  } else {
    return validateL2DelMulticastRouterInterfaceEntry(
        multicast_router_interface_entry, router_interface_entry_ptr);
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

ReturnCode L3MulticastManager::processMulticastReplicationEntries(
    std::vector<P4MulticastReplicationEntry>& entries,
    const std::deque<swss::KeyOpFieldsValuesTuple>& tuple_list,
    const std::string& op, bool update) {
  SWSS_LOG_ENTER();
  ReturnCode status;

  std::vector<ReturnCode> statuses;
  // In syncd, bulk SAI calls use mode SAI_BULK_OP_ERROR_MODE_STOP_ON_ERROR.
  if (op == SET_COMMAND) {
    if (!update) {
      statuses = addMulticastReplicationEntries(entries);
    } else {
      statuses = updateMulticastReplicationEntries(entries);
    }
  } else {
    statuses = deleteMulticastReplicationEntries(entries);
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

ReturnCode L3MulticastManager::createBridgePort(
    P4MulticastRouterInterfaceEntry& entry, sai_object_id_t* bridge_port_oid) {
  SWSS_LOG_ENTER();

  Port port;
  if (!gPortsOrch->getPort(entry.multicast_replica_port, port)) {
    LOG_ERROR_AND_RETURN(ReturnCode(StatusCode::SWSS_RC_NOT_FOUND)
                         << "Unable to find port object "
                         << QuotedVar(entry.multicast_replica_port)
                         << " to create bridge port");
  }

  std::vector<sai_attribute_t> attrs;
  sai_attribute_t attr;

  attr.id = SAI_BRIDGE_PORT_ATTR_TYPE;
  attr.value.s32 = SAI_BRIDGE_PORT_TYPE_PORT;
  attrs.push_back(attr);

  attr.id = SAI_BRIDGE_PORT_ATTR_PORT_ID;
  attr.value.oid = port.m_port_id;
  attrs.push_back(attr);

  sai_status_t status = sai_bridge_api->create_bridge_port(
      bridge_port_oid, gSwitchId, (uint32_t)attrs.size(), attrs.data());

  if (status != SAI_STATUS_SUCCESS) {
    LOG_ERROR_AND_RETURN(
        ReturnCode(status)
        << "Failed to create bridge port for L2 multicast on port "
        << QuotedVar(entry.multicast_replica_port));
  }

  return ReturnCode();
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
  SWSS_LOG_ENTER();
  // Confirm we haven't already created a multicast group for this.
  if (m_p4OidMapper->existsOID(SAI_OBJECT_TYPE_IPMC_GROUP,
                               entry.multicast_group_id)) {
    LOG_ERROR_AND_RETURN(ReturnCode(StatusCode::SWSS_RC_INTERNAL)
                         << "Multicast group to be added with group ID "
                         << QuotedVar(entry.multicast_group_id).c_str()
                         << " already exists in the centralized map");
  }

  // Create Multicast group SAI object.
  // There are no required attributes to create a group.
  std::vector<sai_attribute_t> attrs;
  auto sai_status = sai_ipmc_group_api->create_ipmc_group(
      mcast_group_oid, gSwitchId, (uint32_t)attrs.size(), attrs.data());
  if (sai_status != SAI_STATUS_SUCCESS) {
    LOG_ERROR_AND_RETURN(ReturnCode(sai_status)
                         << "Failed to create multicast group for group ID: "
                         << QuotedVar(entry.multicast_group_id).c_str());
  }
  return ReturnCode();
}

ReturnCode L3MulticastManager::createMulticastGroupMember(
    const P4MulticastReplicationEntry& entry, const sai_object_id_t rif_oid,
    sai_object_id_t* mcast_group_member_oid) {
  SWSS_LOG_ENTER();
  // Confirm we haven't already created a multicast group member for this.
  if (m_p4OidMapper->existsOID(SAI_OBJECT_TYPE_IPMC_GROUP_MEMBER,
                               entry.multicast_replication_key)) {
    LOG_ERROR_AND_RETURN(ReturnCode(StatusCode::SWSS_RC_INTERNAL)
                         << "Multicast group member to be added with key "
                         << QuotedVar(entry.multicast_replication_key).c_str()
                         << " already exists in the centralized map");
  }

  if (rif_oid == SAI_NULL_OBJECT_ID) {
    LOG_ERROR_AND_RETURN(
        ReturnCode(StatusCode::SWSS_RC_UNAVAIL)
        << "Multicast group member with key "
        << QuotedVar(entry.multicast_replication_key).c_str()
        << " cannot be added because there is no associated RIF available");
  }

  // Create Multicast group member SAI object.
  std::vector<sai_attribute_t> attrs =
      prepareMulticastGroupMemberSaiAttrs(entry, rif_oid);

  auto sai_status = sai_ipmc_group_api->create_ipmc_group_member(
      mcast_group_member_oid, gSwitchId, (uint32_t)attrs.size(), attrs.data());
  if (sai_status != SAI_STATUS_SUCCESS) {
    LOG_ERROR_AND_RETURN(ReturnCode(sai_status)
                         << "Failed to create multicast group member for: "
                         << QuotedVar(entry.multicast_replication_key).c_str());
  }
  return ReturnCode();
}

ReturnCode L3MulticastManager::deleteMulticastGroup(
    const std::string& multicast_group_id, sai_object_id_t mcast_group_oid) {
  SWSS_LOG_ENTER();
  // Confirm we have a multicast group to delete.
  if (!m_p4OidMapper->existsOID(SAI_OBJECT_TYPE_IPMC_GROUP,
                                multicast_group_id)) {
    LOG_ERROR_AND_RETURN(ReturnCode(StatusCode::SWSS_RC_INTERNAL)
                         << "Multicast group to be deleted with group ID "
                         << QuotedVar(multicast_group_id).c_str()
                         << " does not exist in the centralized map");
  }
  auto sai_status = sai_ipmc_group_api->remove_ipmc_group(mcast_group_oid);
  if (sai_status != SAI_STATUS_SUCCESS) {
    LOG_ERROR_AND_RETURN(ReturnCode(sai_status)
                         << "Failed to delete multicast group with ID: "
                         << QuotedVar(multicast_group_id).c_str());
  }
  return ReturnCode();
}

std::vector<ReturnCode> L3MulticastManager::addMulticastRouterInterfaceEntries(
    std::vector<P4MulticastRouterInterfaceEntry>& entries) {
  SWSS_LOG_ENTER();
  std::vector<ReturnCode> statuses(entries.size());
  fillStatusArrayWithNotExecuted(statuses, 0);

  for (size_t i = 0; i < entries.size(); ++i) {
    auto& entry = entries[i];
    if (entry.action == p4orch::kSetSrcMac) {
      statuses[i] = addL3MulticastRouterInterfaceEntry(entry);
    } else {
      statuses[i] = addL2MulticastRouterInterfaceEntry(entry);
    }
    if (!statuses[i].ok()) {
      break;
    }
  }
  return statuses;
}

ReturnCode L3MulticastManager::addL3MulticastRouterInterfaceEntry(
    P4MulticastRouterInterfaceEntry& entry) { 
  // There are two cases for add:
  // 1. The new entry (multicast_replica_port, multicast_replica_instance) will
  //    need a new RIF allocated.
  // 2. The new entry will be able to use an existing RIF.
  // Recall that RIFs are created based on multicast_replica_port and Ethernet
  // src mac, and src mac is the action parameter associated with a table entry.
  SWSS_LOG_ENTER();

  sai_object_id_t rif_oid = getRifOid(&entry);
  if (rif_oid == SAI_NULL_OBJECT_ID) {
    std::string rif_key = KeyGenerator::generateMulticastRouterInterfaceRifKey(
        entry.multicast_replica_port, entry.src_mac);

    RETURN_IF_ERROR(createRouterInterface(rif_key, entry, &rif_oid));

    gPortsOrch->increasePortRefCount(entry.multicast_replica_port);
    m_p4OidMapper->setOID(SAI_OBJECT_TYPE_ROUTER_INTERFACE, rif_key, rif_oid);
    m_rifOids[rif_key] = rif_oid;
    m_rifOidToMulticastGroupMembers[rif_oid] = {};
  }

  // Operations done regardless of whether RIF was created or not.
  // Set the entry RIF.
  entry.router_interface_oid = rif_oid;

  // Update internal state.
  m_multicastRouterInterfaceTable[entry.multicast_router_interface_entry_key] =
      entry;
  m_rifOidToRouterInterfaceEntries[rif_oid].push_back(entry);
  return ReturnCode();
}

ReturnCode L3MulticastManager::addL2MulticastRouterInterfaceEntry(
    P4MulticastRouterInterfaceEntry& entry) {
  // There are two cases for add:
  // 1. The new entry (multicast_replica_port, multicast_replica_instance) will
  //    need a new bridge port allocated.
  // 2. The new entry will be able to use an existing bridge port.
  // Recall that bridge ports depend only on the multicast_replica_port.
  SWSS_LOG_ENTER();

  sai_object_id_t bridge_port_oid = getBridgePortOid(&entry);
  if (bridge_port_oid == SAI_NULL_OBJECT_ID) {
    RETURN_IF_ERROR(createBridgePort(entry, &bridge_port_oid));
    gPortsOrch->increasePortRefCount(entry.multicast_replica_port);
    m_p4OidMapper->setOID(SAI_OBJECT_TYPE_BRIDGE_PORT,
                          entry.multicast_replica_port, bridge_port_oid);
  }

  // Operations done regardless of whether bridge port was created or not.
  // Set the entry bridge port.
  m_multicastRouterInterfaceTable[entry.multicast_router_interface_entry_key] =
      entry;
  return ReturnCode();
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
  // There are two cases for add:
  // 1. This is the first occurrence of the multicast group ID, which requires
  //    the creation of a multicast group OID.
  // 2. The multicast group ID already exists, so we can reference the
  //    existing multicast group OID.
  // Once we have a reference to the multicast group OID, we can add a
  // multicast group member.
  SWSS_LOG_ENTER();

  std::vector<ReturnCode> statuses(entries.size());
  fillStatusArrayWithNotExecuted(statuses, 0);
  for (size_t i = 0; i < entries.size(); ++i) {
    auto& entry = entries[i];
    bool created_group = false;

    sai_object_id_t rif_oid = getRifOid(&entry);
    if (rif_oid == SAI_NULL_OBJECT_ID) {
      statuses[i] = ReturnCode(StatusCode::SWSS_RC_UNAVAIL)
                    << "Cannot add group member "
                    << QuotedVar(entry.multicast_replication_key)
                    << "because associated RIF has not be created.";
      break;
    }

    sai_object_id_t mcast_group_oid = SAI_NULL_OBJECT_ID;
    if (!m_p4OidMapper->getOID(SAI_OBJECT_TYPE_IPMC_GROUP,
                               entry.multicast_group_id, &mcast_group_oid)) {
      // Create the multicast group.
      ReturnCode create_status = createMulticastGroup(entry, &mcast_group_oid);
      statuses[i] = create_status;
      if (!create_status.ok()) {
        break;
      }
      created_group = true;

      // Update internal book-keeping for new multicast group.
      m_p4OidMapper->setOID(SAI_OBJECT_TYPE_IPMC_GROUP,
                            entry.multicast_group_id, mcast_group_oid);
    }
    // The group OID needs to be associated with the entry to be able to create
    // the group member.
    entry.multicast_group_oid = mcast_group_oid;

    // Create the group member.
    sai_object_id_t mcast_group_member_oid;
    ReturnCode create_status =
        createMulticastGroupMember(entry, rif_oid, &mcast_group_member_oid);
    statuses[i] = create_status;

    if (!create_status.ok()) {
      // On group member create failure, attempt to back out creation of the
      // multicast group if one was just created.
      entry.multicast_group_oid = SAI_NULL_OBJECT_ID;
      if (created_group) {
        ReturnCode backout_status =
            deleteMulticastGroup(entry.multicast_group_id, mcast_group_oid);
        if (!backout_status.ok()) {
          // All kinds of bad.  Since the delete failed, we should leave
          // the bookkeeping in place, but we are going to need outside help to
          // repair this.  Leave the create status as the failure code returned.
          std::stringstream err_msg;
          err_msg << "Multicast group member creation failed, and we were "
                  << "unable to backout creation of the multicast group.";
          SWSS_LOG_ERROR("%s", err_msg.str().c_str());
          SWSS_RAISE_CRITICAL_STATE(err_msg.str());
        } else {
          m_p4OidMapper->eraseOID(SAI_OBJECT_TYPE_IPMC_GROUP,
                                  entry.multicast_group_id);
        }
      }
      break;
    }
    m_p4OidMapper->setOID(SAI_OBJECT_TYPE_IPMC_GROUP_MEMBER,
                          entry.multicast_replication_key,
                          mcast_group_member_oid);

    // Finish with book keeping.

    // Operations done regardless of whether multicast group was created or not.
    // Set entry OIDs.
    // The group OID was set above prior to group member creation.
    entry.multicast_group_member_oid = mcast_group_member_oid;

    // Update internal state.
    m_multicastReplicationTable[entry.multicast_replication_key] = entry;
    m_multicastGroupMembers[entry.multicast_group_id].insert(
        entry.multicast_replication_key);
    m_rifOidToMulticastGroupMembers[rif_oid].insert(
        entry.multicast_replication_key);

    statuses[i] = ReturnCode();
  }  // for i
  return statuses;
}

std::vector<ReturnCode> L3MulticastManager::updateMulticastReplicationEntries(
    std::vector<P4MulticastReplicationEntry>& entries) {
  // There is nothing extra to do for update operations, since the table
  // key itself (group_id, multicast_replica_port, multicast_replica_instance)
  // encodes the information needed to add a multicast group and multicast group
  // member.  Validation has previously occurred that also checked internal
  // maps.
  SWSS_LOG_ENTER();

  std::vector<ReturnCode> statuses(entries.size());
  for (size_t i = 0; i < entries.size(); ++i) {
    auto& entry = entries[i];
    statuses[i] = ReturnCode(StatusCode::SWSS_RC_SUCCESS)
                  << "Update of replication entry "
                  << QuotedVar(entry.multicast_replication_key)
                  << " is a no-op";
  }  // for i
  return statuses;
}

std::vector<ReturnCode> L3MulticastManager::deleteMulticastReplicationEntries(
    const std::vector<P4MulticastReplicationEntry>& entries) {
  // There are two cases for removal:
  // 1. This entry is the last one associated with the multicast group.  In
  //    such a case, delete the multicast group and clear it from appropriate
  //    maps.
  // 2. There will still be other group members associated with the multicast
  //    group.  In such a case, only remove the member from being associated
  //    with the group.
  SWSS_LOG_ENTER();

  std::vector<ReturnCode> statuses(entries.size());
  fillStatusArrayWithNotExecuted(statuses, 0);
  for (size_t i = 0; i < entries.size(); ++i) {
    auto& entry = entries[i];

    // Confirm entry exists
    auto* old_entry_ptr =
        getMulticastReplicationEntry(entry.multicast_replication_key);
    if (old_entry_ptr == nullptr) {
      statuses[i] = ReturnCode(StatusCode::SWSS_RC_UNKNOWN)
                    << "Multicast replication entry is not known "
                    << QuotedVar(entry.multicast_replication_key);
      break;
    }

    // Fetch the RIF the member is associated with.
    sai_object_id_t old_rif_oid = getRifOid(old_entry_ptr);
    if (old_rif_oid == SAI_NULL_OBJECT_ID) {
      statuses[i] =
          ReturnCode(StatusCode::SWSS_RC_INTERNAL)
          << "Cannot find RIF oid associated with group member to delete "
          << QuotedVar(old_entry_ptr->multicast_replication_key);
      break;
    }

    // Confirm the old entry had OIDs assigned.
    sai_object_id_t old_group_oid = SAI_NULL_OBJECT_ID;
    m_p4OidMapper->getOID(SAI_OBJECT_TYPE_IPMC_GROUP,
                          old_entry_ptr->multicast_group_id, &old_group_oid);
    sai_object_id_t old_group_member_oid = SAI_NULL_OBJECT_ID;
    m_p4OidMapper->getOID(SAI_OBJECT_TYPE_IPMC_GROUP_MEMBER,
                          old_entry_ptr->multicast_replication_key,
                          &old_group_member_oid);
    if (old_group_oid == SAI_NULL_OBJECT_ID ||
        old_group_member_oid == SAI_NULL_OBJECT_ID) {
      statuses[i] = ReturnCode(StatusCode::SWSS_RC_INTERNAL)
                    << "Multicast replication entry is missing a multicast "
                    << "group OID or a multicast group member OID "
                    << QuotedVar(entry.multicast_replication_key);
      break;
    }

    // Fetch group members associated with multicast group
    if (m_multicastGroupMembers.find(old_entry_ptr->multicast_group_id) ==
        m_multicastGroupMembers.end()) {
      statuses[i] = ReturnCode(StatusCode::SWSS_RC_INTERNAL)
                    << "Cannot find members associated with multicast group "
                    << " id " << old_entry_ptr->multicast_group_id;
      break;
    }
    auto& group_members_set =
        m_multicastGroupMembers[old_entry_ptr->multicast_group_id];
    auto member_cnt = group_members_set.size();
    if (group_members_set.count(old_entry_ptr->multicast_replication_key) !=
        1) {
      statuses[i] = ReturnCode(StatusCode::SWSS_RC_INTERNAL)
                    << "Member " << old_entry_ptr->multicast_replication_key
                    << " was not associated with multicast group id "
                    << old_entry_ptr->multicast_group_id;
      break;
    }

    // If we will delete the multicast group, confirm no more L3 routes use
    // this group id before deleting it.
    // We do this check before any SAI calls to avoid having to undo operations.
    if (member_cnt == 1) {
      // Set to non-zero to avoid deletion in case of failure.
      uint32_t route_entry_ref_count = 1;
      if (!m_p4OidMapper->getRefCount(SAI_OBJECT_TYPE_IPMC_GROUP,
                                      old_entry_ptr->multicast_group_id,
                                      &route_entry_ref_count)) {
        statuses[i] = ReturnCode(StatusCode::SWSS_RC_INTERNAL)
                      << "Unable to fetch reference count for multicast "
                      << "group " << old_entry_ptr->multicast_group_id;
        break;
      }

      if (route_entry_ref_count != 0) {
        statuses[i] = ReturnCode(StatusCode::SWSS_RC_IN_USE)
                      << "Member " << old_entry_ptr->multicast_replication_key
                      << " cannot be deleted because route entries are still "
                      << "referencing multicast group "
                      << old_entry_ptr->multicast_group_id;
        break;
      }
    }

    // Delete group member
    sai_status_t member_delete_status =
        sai_ipmc_group_api->remove_ipmc_group_member(
            old_entry_ptr->multicast_group_member_oid);
    if (member_delete_status != SAI_STATUS_SUCCESS) {
      statuses[i] = member_delete_status;
      break;
    }

    // Delete group, if necessary.
    if (member_cnt == 1) {
      sai_status_t group_delete_status = sai_ipmc_group_api->remove_ipmc_group(
          old_entry_ptr->multicast_group_oid);
      if (group_delete_status != SAI_STATUS_SUCCESS) {
        statuses[i] = group_delete_status;
        // On group removal failure, attempt to put the group member back.
        sai_object_id_t re_add_rif_oid = getRifOid(old_entry_ptr);
        std::vector<sai_attribute_t> re_add_attrs =
            prepareMulticastGroupMemberSaiAttrs(*old_entry_ptr, re_add_rif_oid);
        sai_status_t re_add_status =
            sai_ipmc_group_api->create_ipmc_group_member(
                &old_entry_ptr->multicast_group_member_oid, gSwitchId,
                (uint32_t)re_add_attrs.size(), re_add_attrs.data());

        if (re_add_status != SAI_STATUS_SUCCESS) {
          // All kinds of bad.  We couldn't restore the multicast group object,
          // which leaves us in an inconsistent state with what the controller
          // expects.
          std::stringstream err_msg;
          err_msg << "Unable to backout removal of multicast group member for "
                  << QuotedVar(old_entry_ptr->multicast_replication_key)
                  << " after group delete failed";
          SWSS_LOG_ERROR("%s", err_msg.str().c_str());
          SWSS_RAISE_CRITICAL_STATE(err_msg.str());
        } else {
          // Update group member OID, in case it changed.
          m_p4OidMapper->eraseOID(SAI_OBJECT_TYPE_IPMC_GROUP_MEMBER,
                                  old_entry_ptr->multicast_replication_key);
          m_p4OidMapper->setOID(SAI_OBJECT_TYPE_IPMC_GROUP_MEMBER,
                                old_entry_ptr->multicast_replication_key,
                                old_entry_ptr->multicast_group_member_oid);
        }
        break;
      }
    }

    // Do internal bookkeeping.
    m_p4OidMapper->eraseOID(SAI_OBJECT_TYPE_IPMC_GROUP_MEMBER,
                            old_entry_ptr->multicast_replication_key);
    group_members_set.erase(old_entry_ptr->multicast_replication_key);
    m_rifOidToMulticastGroupMembers[old_rif_oid].erase(
        entry.multicast_replication_key);
    if (member_cnt == 1) {
      m_p4OidMapper->eraseOID(SAI_OBJECT_TYPE_IPMC_GROUP,
                              old_entry_ptr->multicast_group_id);
      m_multicastGroupMembers.erase(old_entry_ptr->multicast_group_id);
    }
    m_multicastReplicationTable.erase(old_entry_ptr->multicast_replication_key);

    statuses[i] = ReturnCode();
  }  // for i
  return statuses;
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
  // Note: action is checked for differences in the
  // validateMulticastRouterInterfaceEntry function.
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

  if (multicast_router_interface_entry->action == p4orch::kSetSrcMac) {
    std::string rif_key = KeyGenerator::generateMulticastRouterInterfaceRifKey(
        multicast_router_interface_entry->multicast_replica_port,
        multicast_router_interface_entry->src_mac);
    return m_p4OidMapper->verifyOIDMapping(
        SAI_OBJECT_TYPE_ROUTER_INTERFACE, rif_key,
        multicast_router_interface_entry->router_interface_oid);
  } else {
    return "SWSS_RC_UNIMPLEMENTED";
  }
}

std::string L3MulticastManager::verifyMulticastRouterInterfaceStateAsicDb(
    const P4MulticastRouterInterfaceEntry* multicast_router_interface_entry) {
  if (multicast_router_interface_entry->action == p4orch::kSetSrcMac) {
    return verifyL3MulticastRouterInterfaceStateAsicDb(
        multicast_router_interface_entry);
  } else {
    return verifyL2MulticastRouterInterfaceStateAsicDb(
        multicast_router_interface_entry);
  }
}

std::string L3MulticastManager::verifyL3MulticastRouterInterfaceStateAsicDb(
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

std::string L3MulticastManager::verifyL2MulticastRouterInterfaceStateAsicDb(
    const P4MulticastRouterInterfaceEntry* multicast_router_interface_entry) {
  // TODO: This needs to be implemented.
  return "SWSS_RC_UNIMPLEMENTED";
}

std::string L3MulticastManager::verifyMulticastReplicationStateCache(
    const P4MulticastReplicationEntry& app_db_entry,
    const P4MulticastReplicationEntry* multicast_replication_entry) {
  const std::string replication_entry_key =
      KeyGenerator::generateMulticastReplicationKey(
          app_db_entry.multicast_group_id, app_db_entry.multicast_replica_port,
          app_db_entry.multicast_replica_instance);

  ReturnCode status =
      validateMulticastReplicationEntry(app_db_entry, SET_COMMAND);
  if (!status.ok()) {
    std::stringstream msg;
    msg << "Validation failed for multicast replication DB entry with key "
        << QuotedVar(replication_entry_key) << ": " << status.message();
    return msg.str();
  }
  if (multicast_replication_entry->multicast_replication_key !=
      app_db_entry.multicast_replication_key) {
    std::stringstream msg;
    msg << "Multicast replication entry key "
        << QuotedVar(app_db_entry.multicast_replication_key)
        << " does not match internal cache "
        << QuotedVar(multicast_replication_entry->multicast_replication_key)
        << " in l3 multicast manager for replication entry.";
    return msg.str();
  }
  if (multicast_replication_entry->multicast_group_id !=
      app_db_entry.multicast_group_id) {
    std::stringstream msg;
    msg << "Multicast group ID " << QuotedVar(app_db_entry.multicast_group_id)
        << " does not match internal cache "
        << QuotedVar(multicast_replication_entry->multicast_group_id)
        << " in l3 multicast manager for replication entry.";
    return msg.str();
  }
  if (multicast_replication_entry->multicast_replica_port !=
      app_db_entry.multicast_replica_port) {
    std::stringstream msg;
    msg << "Output port name " << QuotedVar(app_db_entry.multicast_replica_port)
        << " does not match internal cache "
        << QuotedVar(multicast_replication_entry->multicast_replica_port)
        << " in l3 multicast manager for replication entry.";
    return msg.str();
  }
  if (multicast_replication_entry->multicast_replica_instance !=
      app_db_entry.multicast_replica_instance) {
    std::stringstream msg;
    msg << "Egress instance "
        << QuotedVar(app_db_entry.multicast_replica_instance)
        << " does not match internal cache "
        << QuotedVar(multicast_replication_entry->multicast_replica_instance)
        << " in l3 multicast manager for replication entry.";
    return msg.str();
  }
  if (multicast_replication_entry->multicast_metadata !=
      app_db_entry.multicast_metadata) {
    std::stringstream msg;
    msg << "Multicast metadata " << QuotedVar(app_db_entry.multicast_metadata)
        << " does not match internal cache "
        << QuotedVar(multicast_replication_entry->multicast_metadata)
        << " in l3 multicast manager for replication entry.";
    return msg.str();
  }
  std::string group_msg = m_p4OidMapper->verifyOIDMapping(
      SAI_OBJECT_TYPE_IPMC_GROUP,
      multicast_replication_entry->multicast_group_id,
      multicast_replication_entry->multicast_group_oid);
  if (!group_msg.empty()) {
    return group_msg;
  }
  return m_p4OidMapper->verifyOIDMapping(
      SAI_OBJECT_TYPE_IPMC_GROUP_MEMBER,
      multicast_replication_entry->multicast_replication_key,
      multicast_replication_entry->multicast_group_member_oid);
}

std::string L3MulticastManager::verifyMulticastReplicationStateAsicDb(
    const P4MulticastReplicationEntry* multicast_replication_entry) {
  // Confirm have RIF.
  sai_object_id_t rif_oid = getRifOid(multicast_replication_entry);
  if (rif_oid == SAI_NULL_OBJECT_ID) {
    std::stringstream msg;
    msg << "Unable to find RIF associated with multicast entry "
        << QuotedVar(multicast_replication_entry->multicast_replication_key);
    return msg.str();
  }

  // Confirm group settings.
  std::vector<sai_attribute_t> group_attrs;  // no required attributes
  std::vector<swss::FieldValueTuple> exp =
      saimeta::SaiAttributeList::serialize_attr_list(
          SAI_OBJECT_TYPE_IPMC_GROUP_MEMBER, (uint32_t)group_attrs.size(),
          group_attrs.data(), /*countOnly=*/false);

  swss::DBConnector db("ASIC_DB", 0);
  swss::Table table(&db, "ASIC_STATE");
  std::string key =
      sai_serialize_object_type(SAI_OBJECT_TYPE_IPMC_GROUP) + ":" +
      sai_serialize_object_id(multicast_replication_entry->multicast_group_oid);
  std::vector<swss::FieldValueTuple> values;
  if (!table.get(key, values)) {
    return std::string("ASIC DB key not found ") + key;
  }
  std::string group_msg =
      verifyAttrs(values, exp, std::vector<swss::FieldValueTuple>{},
                  /*allow_unknown=*/false);
  if (!group_msg.empty()) {
    return group_msg;
  }

  // Confirm group member settings.
  auto member_attrs = prepareMulticastGroupMemberSaiAttrs(
      *multicast_replication_entry, rif_oid);
  exp = saimeta::SaiAttributeList::serialize_attr_list(
      SAI_OBJECT_TYPE_IPMC_GROUP_MEMBER, (uint32_t)member_attrs.size(),
      member_attrs.data(), /*countOnly=*/false);
  key = sai_serialize_object_type(SAI_OBJECT_TYPE_IPMC_GROUP_MEMBER) + ":" +
        sai_serialize_object_id(
            multicast_replication_entry->multicast_group_member_oid);
  values.clear();
  if (!table.get(key, values)) {
    return std::string("ASIC DB key not found ") + key;
  }
  return verifyAttrs(values, exp, std::vector<swss::FieldValueTuple>{},
                     /*allow_unknown=*/false);
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

// A bridge port is associated with an egress port.
sai_object_id_t L3MulticastManager::getBridgePortOid(
    const P4MulticastRouterInterfaceEntry* multicast_router_interface_entry) {
  sai_object_id_t bridge_port_oid = SAI_NULL_OBJECT_ID;
  m_p4OidMapper->getOID(
      SAI_OBJECT_TYPE_BRIDGE_PORT,
      multicast_router_interface_entry->multicast_replica_port,
      &bridge_port_oid);
  return bridge_port_oid;
}

// A RIF is associated with an egress port and Ethernet src mac value.
sai_object_id_t L3MulticastManager::getRifOid(
    const P4MulticastReplicationEntry* multicast_replication_entry) {
  // Get router interface entry for out port and egress instance.
  const std::string router_interface_key =
      KeyGenerator::generateMulticastRouterInterfaceKey(
          multicast_replication_entry->multicast_replica_port,
          multicast_replication_entry->multicast_replica_instance);
  auto* router_interface_entry_ptr =
      getMulticastRouterInterfaceEntry(router_interface_key);
  if (router_interface_entry_ptr == nullptr) {
    return SAI_NULL_OBJECT_ID;
  }
  // Use that to generate RIF key.
  std::string rif_key = KeyGenerator::generateMulticastRouterInterfaceRifKey(
      router_interface_entry_ptr->multicast_replica_port,
      router_interface_entry_ptr->src_mac);
  if (m_rifOids.find(rif_key) == m_rifOids.end()) {
    return SAI_NULL_OBJECT_ID;
  }
  return m_rifOids[rif_key];
}

}  // namespace p4orch
