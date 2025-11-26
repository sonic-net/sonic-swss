#include "p4orch/l3_multicast_manager.h"

#include <inttypes.h>

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
    const sai_object_id_t multicast_group_oid, const sai_object_id_t rif_oid) {
  std::vector<sai_attribute_t> attrs;
  sai_attribute_t attr;

  attr.id = SAI_IPMC_GROUP_MEMBER_ATTR_IPMC_GROUP_ID;
  attr.value.oid = multicast_group_oid;
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
      } else if (prev_table == APP_P4RT_REPLICATION_IP_MULTICAST_TABLE_NAME) {
        // This drain function will drain unexecuted entries upon failure.
        status = drainMulticastGroupEntries(tuple_list);
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
    } else if (prev_table == APP_P4RT_REPLICATION_IP_MULTICAST_TABLE_NAME) {
      status = drainMulticastGroupEntries(tuple_list);
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
ReturnCode L3MulticastManager::drainMulticastGroupEntries(
    std::deque<swss::KeyOpFieldsValuesTuple>& group_entry_tuples) {
  SWSS_LOG_ENTER();
  ReturnCode status;
  std::vector<P4MulticastGroupEntry> multicast_group_entry_list;
  std::deque<swss::KeyOpFieldsValuesTuple> tuple_list;

  std::string prev_op;
  bool prev_update = false;

  while (!group_entry_tuples.empty()) {
    auto key_op_fvs_tuple = group_entry_tuples.front();
    group_entry_tuples.pop_front();
    std::string table_name;
    std::string key;
    parseP4RTKey(kfvKey(key_op_fvs_tuple), &table_name, &key);
    const std::vector<swss::FieldValueTuple>& attributes =
        kfvFieldsValues(key_op_fvs_tuple);

    // Form entry object
    auto group_entry_or = deserializeMulticastGroupEntry(key, attributes);

    if (!group_entry_or.ok()) {
      status = group_entry_or.status();
      SWSS_LOG_ERROR("Unable to deserialize APP DB entry with key %s: %s",
                     QuotedVar(table_name + ":" + key).c_str(),
                     status.message().c_str());
      m_publisher->publish(APP_P4RT_TABLE_NAME, kfvKey(key_op_fvs_tuple),
                           kfvFieldsValues(key_op_fvs_tuple), status,
                           /*replace=*/true);
      break;
    }
    auto& group_entry = *group_entry_or;

    // Validate entry
    const std::string& operation = kfvOp(key_op_fvs_tuple);
    status = validateMulticastGroupEntry(group_entry, operation);
    if (!status.ok()) {
      SWSS_LOG_ERROR(
          "Validation failed for APP DB group entry with key  %s: %s",
          QuotedVar(table_name + ":" + key).c_str(), status.message().c_str());
      m_publisher->publish(APP_P4RT_TABLE_NAME, kfvKey(key_op_fvs_tuple),
                           kfvFieldsValues(key_op_fvs_tuple), status,
                           /*replace=*/true);
      break;
    }

    // Now, start processing batch of entries.
    auto* group_entry_ptr =
        getMulticastGroupEntry(group_entry.multicast_group_id);
    bool update = group_entry_ptr != nullptr;

    if (prev_op == "") {
      prev_op = operation;
      prev_update = update;
    }
    // Process the entries if the operation type changes.
    if (operation != prev_op || update != prev_update) {
      status = processMulticastGroupEntries(multicast_group_entry_list,
                                            tuple_list, prev_op, prev_update);
      multicast_group_entry_list.clear();
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
      multicast_group_entry_list.push_back(group_entry);
      tuple_list.push_back(key_op_fvs_tuple);
    }
  }  // while

  // Process any pending entries.
  if (!multicast_group_entry_list.empty()) {
    auto rc = processMulticastGroupEntries(multicast_group_entry_list,
                                           tuple_list, prev_op, prev_update);
    if (!rc.ok()) {
      status = rc;
    }
  }

  drainMgmtWithNotExecuted(group_entry_tuples, m_publisher);
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
      if (value == p4orch::kSetMulticastSrcMac || value == p4orch::kNoAction) {
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

/*
  P4RT:REPLICATION_MULTICAST_TABLE:"0x1" {
      "replicas": [
         {
            "multicast_replica_instance": "0x0",
            "multicast_replica_port": "Ethernet1"
         },
         ...
      ],
      "controller_metadata" = "...",  // optional
      "multicast_metadata" = "...",   // optional
  }
 */
ReturnCodeOr<P4MulticastGroupEntry>
L3MulticastManager::deserializeMulticastGroupEntry(
    const std::string& key,
    const std::vector<swss::FieldValueTuple>& attributes) {
  SWSS_LOG_ENTER();

  P4MulticastGroupEntry group_entry = {};
  group_entry.multicast_group_id = key;

  for (const auto& it : attributes) {
    const auto& field = fvField(it);
    const auto& value = fvValue(it);
    if (field == p4orch::kAction) {
      // This table has no actions.
    } else if (field == p4orch::kMulticastMetadata) {
      group_entry.multicast_metadata = value;
    } else if (field == p4orch::kControllerMetadata) {
      group_entry.controller_metadata = value;
    } else if (field == p4orch::kReplicas) {
      try {
        nlohmann::json replica_json = nlohmann::json::parse(value);
        if (!replica_json.is_array()) {
          return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
                 << "Invalid L3 multicast replicas " << QuotedVar(value)
                 << ", expecting an array.";
        }

        for (auto& replica_map : replica_json) {
          std::string port_name;
          std::string instance;

          if (replica_map.find(p4orch::kMulticastReplicaPort) !=
              replica_map.end()) {
            port_name = replica_map.at(p4orch::kMulticastReplicaPort);
          } else {
            return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
                   << "Failed to deserialize multicast replica, "
                   << p4orch::kMulticastReplicaPort
                   << " is missing: " << QuotedVar(value);
          }

          if (replica_map.find(p4orch::kMulticastReplicaInstance) !=
              replica_map.end()) {
            instance = replica_map.at(p4orch::kMulticastReplicaInstance);
          } else {
            return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
                   << "Failed to deserialize multicast replica, "
                   << p4orch::kMulticastReplicaInstance
                   << " is missing: " << QuotedVar(value);
          }

          if (port_name.empty() || instance.empty()) {
            return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
                   << "Invalid multicast group table replica "
                   << QuotedVar(value) << " for key " << QuotedVar(key);
          }

          P4Replica replica =
              P4Replica(group_entry.multicast_group_id, port_name, instance);
          if (group_entry.member_oids.find(replica.key) !=
              group_entry.member_oids.end()) {
            // Duplicate replica invalid
            return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
                   << "Duplicate multicast group table replica "
                   << QuotedVar(field) << " for key " << QuotedVar(key);
          }
          group_entry.replicas.push_back(replica);
          group_entry.member_oids[replica.key] = SAI_NULL_OBJECT_ID;
        }  // for replica_map
      } catch (std::exception& ex) {
        return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
               << "Failed to deserialize multicast replication table replicas "
               << value;
      }
    } else {
      return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
             << "Unexpected field " << QuotedVar(field) << " in "
             << APP_P4RT_REPLICATION_IP_MULTICAST_TABLE_NAME;
    }
  }
  return group_entry;
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
  } else if (table_name == APP_P4RT_REPLICATION_IP_MULTICAST_TABLE_NAME) {
    return verifyMulticastGroupState(key_content, tuple);
  } else {
    return std::string("Invalid key, unexpected table name: ") + key;
  }
}

// LINT.IfChange(verify_state_ritf_cache)
std::string L3MulticastManager::verifyMulticastRouterInterfaceState(
    const std::string& key, const std::vector<swss::FieldValueTuple>& tuple) {
  auto app_db_entry_or = deserializeMulticastRouterInterfaceEntry(key, tuple);
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
  std::string asic_db_result =
      verifyMulticastRouterInterfaceStateAsicDb(router_interface_entry_ptr);
  if (cache_result.empty()) {
    return asic_db_result;
  }
  if (asic_db_result.empty()) {
    return cache_result;
  }
  return cache_result + "; " + asic_db_result;
}
// LINT.ThenChange()

// LINT.IfChange(verify_state_group_cache)
std::string L3MulticastManager::verifyMulticastGroupState(
    const std::string& key, const std::vector<swss::FieldValueTuple>& tuple) {
  auto app_db_entry_or = deserializeMulticastGroupEntry(key, tuple);
  if (!app_db_entry_or.ok()) {
    ReturnCode status = app_db_entry_or.status();
    std::stringstream msg;
    msg << "Unable to deserialize key " << QuotedVar(key) << ": "
        << status.message();
    return msg.str();
  }
  auto& app_db_entry = *app_db_entry_or;

  auto* group_entry_ptr =
      getMulticastGroupEntry(app_db_entry.multicast_group_id);
  if (group_entry_ptr == nullptr) {
    std::stringstream msg;
    msg << "No entry found with key " << QuotedVar(key);
    return msg.str();
  }

  std::string cache_result =
      verifyMulticastGroupStateCache(app_db_entry, group_entry_ptr);
  std::string asic_db_result = verifyMulticastGroupStateAsicDb(group_entry_ptr);
  if (cache_result.empty()) {
    return asic_db_result;
  }
  if (asic_db_result.empty()) {
    return cache_result;
  }
  return cache_result + "; " + asic_db_result;
}
// LINT.ThenChange()

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

ReturnCode L3MulticastManager::validateMulticastGroupEntry(
    const P4MulticastGroupEntry& multicast_group_entry,
    const std::string& operation) {
  // Multicast group ID is required.
  if (multicast_group_entry.multicast_group_id.empty()) {
    return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
           << "No match field entry multicast_group_id provided";
  }
  if (operation == SET_COMMAND) {
    return validateSetMulticastGroupEntry(multicast_group_entry);
  } else if (operation == DEL_COMMAND) {
    return validateDelMulticastGroupEntry(multicast_group_entry);
  }
  return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
         << "Unknown operation type " << QuotedVar(operation);
}

ReturnCode L3MulticastManager::validateSetMulticastGroupEntry(
    const P4MulticastGroupEntry& multicast_group_entry) {
  auto* group_entry_ptr =
      getMulticastGroupEntry(multicast_group_entry.multicast_group_id);

  // Check that all replicas have a RIF object available.
  for (auto& replica : multicast_group_entry.replicas) {
    sai_object_id_t rif_oid = getRifOid(replica);
    if (rif_oid == SAI_NULL_OBJECT_ID) {
      return ReturnCode(StatusCode::SWSS_RC_NOT_FOUND)
             << "Multicast group member " << QuotedVar(replica.key)
             << " does not have an associated RIF available yet";
    }
  }

  bool is_update_operation = group_entry_ptr != nullptr;
  if (is_update_operation) {
    // Confirm multicast group had SAI object ID.
    if (group_entry_ptr->multicast_group_oid == SAI_OBJECT_TYPE_NULL) {
      return ReturnCode(StatusCode::SWSS_RC_NOT_FOUND)
             << "Multicast group OID was not assigned before updating replicas "
                "in "
                "multicast group "
             << QuotedVar(multicast_group_entry.multicast_group_id);
    }

    // Confirm we have references to the multicast group in internal maps.
    if (!m_p4OidMapper->existsOID(SAI_OBJECT_TYPE_IPMC_GROUP,
                                  group_entry_ptr->multicast_group_id)) {
      return ReturnCode(StatusCode::SWSS_RC_NOT_FOUND)
             << "Expected multicast group is missing from oid map: "
             << group_entry_ptr->multicast_group_id;
    }

    // Confirm we have references to the multicast group members also.
    // An update operation may add or delete members.
    // For add, confirm the member did not have an oid.
    // For update, confirm the member had an oid.
    for (auto& replica : multicast_group_entry.replicas) {
      bool member_exists_in_mapper = m_p4OidMapper->existsOID(
          SAI_OBJECT_TYPE_IPMC_GROUP_MEMBER, replica.key);
      if (group_entry_ptr->member_oids.find(replica.key) ==
          group_entry_ptr->member_oids.end()) {  // Add member.
        if (member_exists_in_mapper) {
          LOG_ERROR_AND_RETURN(ReturnCode(StatusCode::SWSS_RC_INTERNAL)
                               << "Multicast group member to add "
                               << QuotedVar(replica.key)
                               << " already exists in the central mapper.");
        }
      } else {  // Update member.
        if (!member_exists_in_mapper) {
          LOG_ERROR_AND_RETURN(ReturnCode(StatusCode::SWSS_RC_INTERNAL)
                               << "Multicast group member to delete "
                               << QuotedVar(replica.key)
                               << " does not exist in the central mapper.");
        }
      }
    }
  }
  // No additional validation required for add operation.
  return ReturnCode();
}

ReturnCode L3MulticastManager::validateDelMulticastGroupEntry(
    const P4MulticastGroupEntry& multicast_group_entry) {
  auto* group_entry_ptr =
      getMulticastGroupEntry(multicast_group_entry.multicast_group_id);

  // Can't delete what isn't there.
  if (group_entry_ptr == nullptr) {
    return ReturnCode(StatusCode::SWSS_RC_NOT_FOUND)
           << "Multicast group entry does not exist for group "
           << multicast_group_entry.multicast_group_id;
  }

  // Confirm multicast group had SAI object ID.
  if (group_entry_ptr->multicast_group_oid == SAI_OBJECT_TYPE_NULL) {
    return ReturnCode(StatusCode::SWSS_RC_NOT_FOUND)
           << "Multicast group OID was not assigned before deleting: "
           << QuotedVar(multicast_group_entry.multicast_group_id);
  }

  // Confirm the multicast object ID exists in central mapper.
  if (!m_p4OidMapper->existsOID(SAI_OBJECT_TYPE_IPMC_GROUP,
                                group_entry_ptr->multicast_group_id)) {
    return ReturnCode(StatusCode::SWSS_RC_NOT_FOUND)
           << "Multicast group does not exist in central mapper: "
           << QuotedVar(group_entry_ptr->multicast_group_id);
  }

  // Confirm members had member OIDs.
  for (auto& replica : group_entry_ptr->replicas) {
    if (group_entry_ptr->member_oids.find(replica.key) ==
        group_entry_ptr->member_oids.end()) {
      return ReturnCode(StatusCode::SWSS_RC_NOT_FOUND)
             << "Multicast group member OID was not assigned before deleting: "
             << QuotedVar(replica.key);
    }
    if (!m_p4OidMapper->existsOID(SAI_OBJECT_TYPE_IPMC_GROUP_MEMBER,
                                  replica.key)) {
      return ReturnCode(StatusCode::SWSS_RC_NOT_FOUND)
             << "Multicast group member does not exist in the central mapper: "
             << QuotedVar(replica.key);
    }
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
        p4orch::kSetMulticastSrcMac) {
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

  if (router_interface_entry_ptr->action == p4orch::kSetMulticastSrcMac) {
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

ReturnCode L3MulticastManager::processMulticastGroupEntries(
    std::vector<P4MulticastGroupEntry>& entries,
    const std::deque<swss::KeyOpFieldsValuesTuple>& tuple_list,
    const std::string& op, bool update) {
  SWSS_LOG_ENTER();
  ReturnCode status;

  std::vector<ReturnCode> statuses;
  // In syncd, bulk SAI calls use mode SAI_BULK_OP_ERROR_MODE_STOP_ON_ERROR.
  if (op == SET_COMMAND) {
    if (!update) {
      statuses = addMulticastGroupEntries(entries);
    } else {
      statuses = updateMulticastGroupEntries(entries);
    }
  } else {
    statuses = deleteMulticastGroupEntries(entries);
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

ReturnCode L3MulticastManager::deleteBridgePort(
    const std::string& port, sai_object_id_t bridge_port_oid) {
  SWSS_LOG_ENTER();
  auto sai_status = sai_bridge_api->remove_bridge_port(bridge_port_oid);
  if (sai_status != SAI_STATUS_SUCCESS) {
    LOG_ERROR_AND_RETURN(ReturnCode(sai_status)
                         << "Failed to remove bridge port for "
                         << QuotedVar(port).c_str());
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
    P4MulticastGroupEntry& entry, sai_object_id_t* mcast_group_oid) {
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
    const P4Replica& replica, const sai_object_id_t group_oid,
    const sai_object_id_t rif_oid, sai_object_id_t* mcast_group_member_oid) {
  SWSS_LOG_ENTER();
  // Confirm we haven't already created a multicast group member for this.
  if (m_p4OidMapper->existsOID(SAI_OBJECT_TYPE_IPMC_GROUP_MEMBER,
                               replica.key)) {
    LOG_ERROR_AND_RETURN(ReturnCode(StatusCode::SWSS_RC_INTERNAL)
                         << "Multicast group member to be added "
                         << QuotedVar(replica.key).c_str()
                         << " already exists in the centralized map");
  }

  if (rif_oid == SAI_NULL_OBJECT_ID) {
    LOG_ERROR_AND_RETURN(
        ReturnCode(StatusCode::SWSS_RC_UNAVAIL)
        << "Multicast group member " << QuotedVar(replica.key).c_str()
        << " cannot be added because there is no associated RIF available");
  }

  // Create Multicast group member SAI object.
  std::vector<sai_attribute_t> attrs =
      prepareMulticastGroupMemberSaiAttrs(group_oid, rif_oid);

  auto sai_status = sai_ipmc_group_api->create_ipmc_group_member(
      mcast_group_member_oid, gSwitchId, (uint32_t)attrs.size(), attrs.data());
  if (sai_status != SAI_STATUS_SUCCESS) {
    LOG_ERROR_AND_RETURN(ReturnCode(sai_status)
                         << "Failed to create multicast group member for: "
                         << QuotedVar(replica.key).c_str());
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
    if (entry.action == p4orch::kSetMulticastSrcMac) {
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
  m_p4OidMapper->increaseRefCount(SAI_OBJECT_TYPE_BRIDGE_PORT,
                                  entry.multicast_replica_port);
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
      std::stringstream err_msg;
      err_msg << "Multicast router interface entry is missing "
              << QuotedVar(entry.multicast_router_interface_entry_key);
      SWSS_LOG_ERROR("%s", err_msg.str().c_str());
      SWSS_RAISE_CRITICAL_STATE(err_msg.str());
      statuses[i] = ReturnCode(StatusCode::SWSS_RC_INTERNAL) << err_msg.str();
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
      std::stringstream err_msg;
      err_msg << "Multicast router interface entry is missing a RIF oid "
              << QuotedVar(old_entry_ptr->multicast_router_interface_entry_key);
      SWSS_LOG_ERROR("%s", err_msg.str().c_str());
      SWSS_RAISE_CRITICAL_STATE(err_msg.str());
      statuses[i] = ReturnCode(StatusCode::SWSS_RC_INTERNAL) << err_msg.str();
      break;
    }

    // Fetch the vector P4MulticastRouterInterfaceEntry associated with the RIF.
    if (m_rifOidToRouterInterfaceEntries.find(old_rif_oid) ==
        m_rifOidToRouterInterfaceEntries.end()) {
      std::stringstream err_msg;
      err_msg << "RIF oid " << old_rif_oid << " missing from map for "
              << QuotedVar(old_entry_ptr->multicast_router_interface_entry_key);
      SWSS_LOG_ERROR("%s", err_msg.str().c_str());
      SWSS_RAISE_CRITICAL_STATE(err_msg.str());
      statuses[i] = ReturnCode(StatusCode::SWSS_RC_INTERNAL) << err_msg.str();
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
      std::stringstream err_msg;
      err_msg << "Unable to find entry "
              << QuotedVar(old_entry_ptr->multicast_router_interface_entry_key)
              << " in map";
      SWSS_LOG_ERROR("%s", err_msg.str().c_str());
      SWSS_RAISE_CRITICAL_STATE(err_msg.str());
      statuses[i] = ReturnCode(StatusCode::SWSS_RC_INTERNAL) << err_msg.str();
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

    // The design does not expect to update
    // multicast group members that were using the original RIF, and this was
    // confirmed to not be a valid use case from the SFE perspective.
    if (m_rifOidToMulticastGroupMembers.find(old_rif_oid) !=
        m_rifOidToMulticastGroupMembers.end()) {
      SWSS_LOG_INFO("Old RIF OID %" PRIu64
                    " was used by %u multicast group members",
                    old_rif_oid,
                    static_cast<unsigned int>(
                        m_rifOidToMulticastGroupMembers[old_rif_oid].size()));
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

ReturnCode L3MulticastManager::deleteL2MulticastRouterInterfaceEntry(
    const P4MulticastRouterInterfaceEntry* entry) {
  SWSS_LOG_ENTER();

  // There are two cases for removal:
  // 1. This entry is the last one associated with the bridge port.  In such a
  //    case, delete the bridge port and clear it from appropriate maps.
  // 2. There will still be other entries associated with the bridge port.  In
  //    such a case, only remove the current entry from being associated with
  //    the brige port.

  // Confirm bridge port OID was assigned.
  sai_object_id_t bridge_port_oid = getBridgePortOid(entry);
  if (bridge_port_oid == SAI_NULL_OBJECT_ID) {
    std::stringstream err_msg;
    err_msg << "Multicast router interface entry is missing a bridge port oid "
            << QuotedVar(entry->multicast_router_interface_entry_key);
    SWSS_LOG_ERROR("%s", err_msg.str().c_str());
    SWSS_RAISE_CRITICAL_STATE(err_msg.str());
    return ReturnCode(StatusCode::SWSS_RC_INTERNAL) << err_msg.str();
  }

  uint32_t bridge_port_ref_count = 2;
  if (!m_p4OidMapper->getRefCount(SAI_OBJECT_TYPE_BRIDGE_PORT,
                                  entry->multicast_replica_port,
                                  &bridge_port_ref_count)) {
    std::stringstream err_msg;
    err_msg << "Unable to fetch reference count for bridge port "
            << entry->multicast_replica_port;
    SWSS_LOG_ERROR("%s", err_msg.str().c_str());
    SWSS_RAISE_CRITICAL_STATE(err_msg.str());
    return ReturnCode(StatusCode::SWSS_RC_INTERNAL) << err_msg.str();
  }

  // Only delete the bridge port if there will be no more entries associated
  // with it after this entry's deletion.
  if (bridge_port_ref_count <= 1) {
    RETURN_IF_ERROR(
        deleteBridgePort(entry->multicast_replica_port, bridge_port_oid));
  }

  // To successfully erase an OID when it is no longer used, the ref count must
  // be 0.
  m_p4OidMapper->decreaseRefCount(SAI_OBJECT_TYPE_BRIDGE_PORT,
                                  entry->multicast_replica_port);
  if (bridge_port_ref_count <= 1) {
    m_p4OidMapper->eraseOID(SAI_OBJECT_TYPE_BRIDGE_PORT,
                            entry->multicast_replica_port);
    gPortsOrch->decreasePortRefCount(entry->multicast_replica_port);
  }

  // Finally, remove the P4MulticastRouterInterfaceEntry.
  m_multicastRouterInterfaceTable.erase(
      entry->multicast_router_interface_entry_key);
  return ReturnCode();
}

ReturnCode L3MulticastManager::deleteL3MulticastRouterInterfaceEntry(
    const P4MulticastRouterInterfaceEntry* entry) {
  SWSS_LOG_ENTER();
  // There are two cases for removal:
  // 1. This entry is the last one associated with the RIF.  In such a case,
  //    delete the RIF and clear it from appropriate maps.
  // 2. There will still be other entries associated with the RIF.  In such a
  //    case, only remove the current entry from being associated with the RIF.

  // Confirm RIF OID was assigned.
  sai_object_id_t rif_oid = getRifOid(entry);
  if (rif_oid == SAI_NULL_OBJECT_ID) {
    std::stringstream err_msg;
    err_msg << "Multicast router interface entry is missing a RIF oid "
            << QuotedVar(entry->multicast_router_interface_entry_key);
    SWSS_LOG_ERROR("%s", err_msg.str().c_str());
    SWSS_RAISE_CRITICAL_STATE(err_msg.str());
    return ReturnCode(StatusCode::SWSS_RC_INTERNAL) << err_msg.str();
  }

  // Confirm there are no more multicast group members using the RIF.
  if (m_rifOidToMulticastGroupMembers.find(rif_oid) !=
      m_rifOidToMulticastGroupMembers.end()) {
    if (m_rifOidToMulticastGroupMembers[rif_oid].size() > 0) {
      return ReturnCode(StatusCode::SWSS_RC_IN_USE)
             << "RIF oid " << rif_oid << " cannot be deleted, because "
             << "it is still used by multicast group members.";
    }
  }

  // Confirm there is at least one P4MulticastRouterInterfaceEntry associated
  // with the RIF.
  if (m_rifOidToRouterInterfaceEntries.find(rif_oid) ==
      m_rifOidToRouterInterfaceEntries.end()) {
    std::stringstream err_msg;
    err_msg << "RIF oid " << rif_oid << " missing from map for "
            << QuotedVar(entry->multicast_router_interface_entry_key);
    SWSS_LOG_ERROR("%s", err_msg.str().c_str());
    SWSS_RAISE_CRITICAL_STATE(err_msg.str());
    return ReturnCode(StatusCode::SWSS_RC_INTERNAL) << err_msg.str();
  }
  auto& entries_for_rif = m_rifOidToRouterInterfaceEntries[rif_oid];
  auto entry_with_rif =
      std::find_if(entries_for_rif.begin(), entries_for_rif.end(),
                   [&](const P4MulticastRouterInterfaceEntry& x) {
                     return x.multicast_router_interface_entry_key ==
                            entry->multicast_router_interface_entry_key;
                   });
  if ((entry_with_rif == entries_for_rif.end()) ||
      (m_multicastRouterInterfaceTable.find(
           entry->multicast_router_interface_entry_key) ==
       m_multicastRouterInterfaceTable.end())) {
    std::stringstream err_msg;
    err_msg << "Unable to find entry "
            << QuotedVar(entry->multicast_router_interface_entry_key)
            << " in map";
    SWSS_LOG_ERROR("%s", err_msg.str().c_str());
    SWSS_RAISE_CRITICAL_STATE(err_msg.str());
    return ReturnCode(StatusCode::SWSS_RC_INTERNAL) << err_msg.str();
  }
  std::string rif_key = KeyGenerator::generateMulticastRouterInterfaceRifKey(
      entry->multicast_replica_port, entry->src_mac);

  // If this is the last entry, delete the RIF.
  // Attempt to delete RIF at SAI layer before adjusting internal maps, in
  // case there is an error.
  if (entries_for_rif.size() == 1) {
    RETURN_IF_ERROR(deleteRouterInterface(rif_key, rif_oid));

    m_p4OidMapper->eraseOID(SAI_OBJECT_TYPE_ROUTER_INTERFACE, rif_key);
    gPortsOrch->decreasePortRefCount(entry->multicast_replica_port);

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
      entry->multicast_router_interface_entry_key);

  return ReturnCode();
}

std::vector<ReturnCode>
L3MulticastManager::deleteMulticastRouterInterfaceEntries(
    const std::vector<P4MulticastRouterInterfaceEntry>& entries) {
  SWSS_LOG_ENTER();
  std::vector<ReturnCode> statuses(entries.size());
  fillStatusArrayWithNotExecuted(statuses, 0);

  for (size_t i = 0; i < entries.size(); ++i) {
    auto& entry = entries[i];

    // Cannot assume that the src mac will be set on delete operation.
    auto* old_entry_ptr = getMulticastRouterInterfaceEntry(
        entry.multicast_router_interface_entry_key);
    if (old_entry_ptr == nullptr) {
      statuses[i] = ReturnCode(StatusCode::SWSS_RC_UNKNOWN)
                    << "Multicast router interface entry is not known "
                    << QuotedVar(entry.multicast_router_interface_entry_key);
      break;
    }

    if (old_entry_ptr->action == p4orch::kSetMulticastSrcMac) {
      statuses[i] = deleteL3MulticastRouterInterfaceEntry(old_entry_ptr);
    } else {
      statuses[i] = deleteL2MulticastRouterInterfaceEntry(old_entry_ptr);
    }
    if (!statuses[i].ok()) {
      break;
    }
  }
  return statuses;
}

std::vector<ReturnCode> L3MulticastManager::addMulticastGroupEntries(
    std::vector<P4MulticastGroupEntry>& entries) {
  // An add operation creates the multicast group OID, since the multicast
  // group did not exist before.
  SWSS_LOG_ENTER();

  std::vector<ReturnCode> statuses(entries.size());
  fillStatusArrayWithNotExecuted(statuses, 0);
  for (size_t i = 0; i < entries.size(); ++i) {
    auto& entry = entries[i];

    // To avoid needing to back-out later, confirm RIF OIDs exist up front.
    for (auto& replica : entry.replicas) {
      sai_object_id_t rif_oid = getRifOid(replica);
      if (rif_oid == SAI_NULL_OBJECT_ID) {
        statuses[i] = ReturnCode(StatusCode::SWSS_RC_UNAVAIL)
                      << "Cannot add group member " << QuotedVar(replica.key)
                      << "because associated RIF has not be created.";
        return statuses;
      }
    }

    // Create the multicast group.
    sai_object_id_t mcast_group_oid = SAI_NULL_OBJECT_ID;
    ReturnCode create_status = createMulticastGroup(entry, &mcast_group_oid);
    statuses[i] = create_status;
    if (!create_status.ok()) {
      SWSS_LOG_ERROR("Unable to create multicast group for %s",
                     entry.multicast_group_id.c_str());
      break;
    }
    // Update internal book-keeping for new multicast group.
    m_p4OidMapper->setOID(SAI_OBJECT_TYPE_IPMC_GROUP, entry.multicast_group_id,
                          mcast_group_oid);
    // The group OID needs to be associated with the entry to be able to create
    // the group member.
    entry.multicast_group_oid = mcast_group_oid;

    // Next, create the group members.  If there's a failure, back out.
    // Instead of updating internal state as members are created, wait until all
    // members have been created to simplify back-out.
    std::unordered_map<std::string, sai_object_id_t> created_member_map;
    std::unordered_map<std::string, sai_object_id_t> member_rif_map;
    for (auto& replica : entry.replicas) {
      sai_object_id_t rif_oid = getRifOid(replica);

      // Create the group member.
      sai_object_id_t mcast_group_member_oid;
      ReturnCode create_member_status = createMulticastGroupMember(
          replica, mcast_group_oid, rif_oid, &mcast_group_member_oid);
      statuses[i] = create_member_status;

      if (!create_member_status.ok()) {
        // Back out creation of members that were just created.
        for (auto& created_members : created_member_map) {
          sai_status_t member_delete_status =
              sai_ipmc_group_api->remove_ipmc_group_member(
                  created_members.second);
          if (member_delete_status != SAI_STATUS_SUCCESS) {
            // All kinds of bad.  The delete failed, and we have to leave a
            // dangling allocated multicast group.  We are going to need outside
            // help to repair this.  Leave the create status as the original
            // failure code returned.
            std::stringstream err_msg;
            err_msg << "Multicast group member creation failed, and we were "
                    << "unable to backout creation of previous members.";
            SWSS_LOG_ERROR("%s", err_msg.str().c_str());
            SWSS_RAISE_CRITICAL_STATE(err_msg.str());
            // Cannot proceed to try to back out creation of group.
            return statuses;
          }
        }

        // Back out multicast group creation.
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
          // Back out multicast group state.
          entry.multicast_group_oid = SAI_NULL_OBJECT_ID;
          m_p4OidMapper->eraseOID(SAI_OBJECT_TYPE_IPMC_GROUP,
                                  entry.multicast_group_id);
        }
        return statuses;  // Stop trying to create replicas.
      }
      // We successfully created a group member.
      created_member_map[replica.key] = mcast_group_member_oid;
      member_rif_map[replica.key] = rif_oid;
      // We defer additional book-keeping until all replicas are created.
    }  // for replica

    // Finish with book keeping.

    // Update state for created group members.
    for (auto& created_members : created_member_map) {
      m_p4OidMapper->setOID(SAI_OBJECT_TYPE_IPMC_GROUP_MEMBER,
                            created_members.first, created_members.second);
      entry.member_oids[created_members.first] = created_members.second;
      auto rif_oid = member_rif_map.at(created_members.first);
      m_rifOidToMulticastGroupMembers[rif_oid].insert(created_members.first);
    }

    // Update internal state.
    m_multicastGroupEntryTable[entry.multicast_group_id] = entry;
    statuses[i] = ReturnCode();
  }  // for i
  return statuses;
}

std::vector<ReturnCode> L3MulticastManager::updateMulticastGroupEntries(
    std::vector<P4MulticastGroupEntry>& entries) {
  // An update operation has to figure out what replicas associated with the
  // multicast group have been added, deleted, or left unchanged.
  // Replicas will be deleted before new ones are added, to avoid the
  // possibility of resource exhaustion.

  SWSS_LOG_ENTER();
  std::vector<ReturnCode> statuses(entries.size());
  fillStatusArrayWithNotExecuted(statuses, 0);

  for (size_t i = 0; i < entries.size(); ++i) {
    auto& entry = entries[i];

    // Confirm old entry exists.
    auto* old_entry_ptr = getMulticastGroupEntry(entry.multicast_group_id);
    if (old_entry_ptr == nullptr) {
      statuses[i] = ReturnCode(StatusCode::SWSS_RC_UNKNOWN)
                    << "Multicast group entry is not known "
                    << QuotedVar(entry.multicast_group_id);
      break;
    }

    // Fetch the group OID.
    sai_object_id_t old_group_oid = SAI_NULL_OBJECT_ID;
    if (!m_p4OidMapper->getOID(SAI_OBJECT_TYPE_IPMC_GROUP,
                               old_entry_ptr->multicast_group_id,
                               &old_group_oid)) {
      std::stringstream err_msg;
      err_msg << "Unable to fetch multicast group oid for group "
              << old_entry_ptr->multicast_group_id;
      SWSS_LOG_ERROR("%s", err_msg.str().c_str());
      SWSS_RAISE_CRITICAL_STATE(err_msg.str());
      statuses[i] = ReturnCode(StatusCode::SWSS_RC_INTERNAL) << err_msg.str();
      break;
    }

    std::vector<P4Replica> replicas_to_add;
    for (auto& replica : entry.replicas) {
      // New replica is not part of existing replicas.
      if (old_entry_ptr->member_oids.find(replica.key) ==
          old_entry_ptr->member_oids.end()) {
        replicas_to_add.push_back(replica);
      }
    }

    std::vector<P4Replica> replicas_to_delete;
    for (auto& replica : old_entry_ptr->replicas) {
      // Existing replica is not part of new replicas.
      if (entry.member_oids.find(replica.key) == entry.member_oids.end()) {
        replicas_to_delete.push_back(replica);
      }
    }

    // Replicas in both old and new entries can be left untouched (no-op).

    // First, delete replicas.
    std::vector<P4Replica> deleted_replicas;
    std::unordered_map<std::string, sai_object_id_t> replica_rif_map;

    for (auto& replica : replicas_to_delete) {
      // Fetch the RIF used by the member.
      sai_object_id_t old_rif_oid = getRifOid(replica);
      if (old_rif_oid == SAI_NULL_OBJECT_ID) {
        std::stringstream err_msg;
        err_msg << "Cannot find RIF oid associated with group member to delete "
                << QuotedVar(replica.key);
        SWSS_LOG_ERROR("%s", err_msg.str().c_str());
        SWSS_RAISE_CRITICAL_STATE(err_msg.str());
        statuses[i] = ReturnCode(StatusCode::SWSS_RC_INTERNAL) << err_msg.str();
        return statuses;
      }
      replica_rif_map[replica.key] = old_rif_oid;

      // Fetch the member OID.
      if (old_entry_ptr->member_oids.find(replica.key) ==
          old_entry_ptr->member_oids.end()) {
        std::stringstream err_msg;
        err_msg << "Cannot find oid associated with group member to delete "
                << QuotedVar(replica.key);
        SWSS_LOG_ERROR("%s", err_msg.str().c_str());
        SWSS_RAISE_CRITICAL_STATE(err_msg.str());
        statuses[i] = ReturnCode(StatusCode::SWSS_RC_INTERNAL) << err_msg.str();
        return statuses;
      }
      sai_object_id_t old_group_member_oid =
          old_entry_ptr->member_oids.at(replica.key);

      // Delete group member
      sai_status_t member_delete_status =
          sai_ipmc_group_api->remove_ipmc_group_member(old_group_member_oid);
      if (member_delete_status != SAI_STATUS_SUCCESS) {
        statuses[i] = member_delete_status;

        // Attempt to re-add deleted group members.
        ReturnCode restore_status = restoreDeletedGroupMembers(
            deleted_replicas, replica_rif_map, old_group_oid, replica.key,
            old_entry_ptr);
        if (!restore_status.ok()) {
          SWSS_LOG_ERROR("%s", restore_status.message().c_str());
          SWSS_RAISE_CRITICAL_STATE(restore_status.message());
        }
        // We still return the original failure when we successfully back
        // out changes.
        return statuses;
      }
      // Update internal state to reflect successful delete.
      m_p4OidMapper->eraseOID(SAI_OBJECT_TYPE_IPMC_GROUP_MEMBER, replica.key);
      old_entry_ptr->member_oids.erase(replica.key);
      m_rifOidToMulticastGroupMembers[old_rif_oid].erase(replica.key);
      deleted_replicas.push_back(replica);
    }  // for replica (to delete)

    // Second add new replicas.
    std::vector<P4Replica> added_replicas;

    for (auto& replica : replicas_to_add) {
      // Fetch the RIF used by the member.
      sai_object_id_t new_rif_oid = getRifOid(replica);
      if (new_rif_oid == SAI_NULL_OBJECT_ID) {
        std::stringstream err_msg;
        err_msg << "Cannot find RIF oid associated with group member to add "
                << QuotedVar(replica.key);
        SWSS_LOG_ERROR("%s", err_msg.str().c_str());
        SWSS_RAISE_CRITICAL_STATE(err_msg.str());
        statuses[i] = ReturnCode(StatusCode::SWSS_RC_INTERNAL) << err_msg.str();
        return statuses;
      }
      replica_rif_map[replica.key] = new_rif_oid;

      // Create the group member.
      sai_object_id_t mcast_group_member_oid;
      ReturnCode create_member_status = createMulticastGroupMember(
          replica, old_group_oid, new_rif_oid, &mcast_group_member_oid);

      if (!create_member_status.ok()) {
        statuses[i] = create_member_status;

        // Backout members added.
        for (auto& added_replica : added_replicas) {
          sai_status_t member_delete_status =
              sai_ipmc_group_api->remove_ipmc_group_member(
                  old_entry_ptr->member_oids[added_replica.key]);
          if (member_delete_status != SAI_STATUS_SUCCESS) {
            // All kinds of bad
            std::stringstream err_msg;
            err_msg << "Cannot revert to previous state, because added replica "
                    << QuotedVar(added_replica.key) << " cannot be deleted";
            SWSS_LOG_ERROR("%s", err_msg.str().c_str());
            SWSS_RAISE_CRITICAL_STATE(err_msg.str());
            return statuses;
          }
          // Update state based on successful removal.
          m_p4OidMapper->eraseOID(SAI_OBJECT_TYPE_IPMC_GROUP_MEMBER,
                                  added_replica.key);
          old_entry_ptr->member_oids.erase(added_replica.key);
          m_rifOidToMulticastGroupMembers[replica_rif_map.at(added_replica.key)]
              .erase(added_replica.key);
        }

        // Attempt to re-add deleted group members.
        ReturnCode restore_status = restoreDeletedGroupMembers(
            deleted_replicas, replica_rif_map, old_group_oid, replica.key,
            old_entry_ptr);
        if (!restore_status.ok()) {
          SWSS_LOG_ERROR("%s", restore_status.message().c_str());
          SWSS_RAISE_CRITICAL_STATE(restore_status.message());
        }
        // We still return the original failure when we successfully back
        // out changes.
        return statuses;
      }

      // Update internal state to reflect successful add.
      m_p4OidMapper->setOID(SAI_OBJECT_TYPE_IPMC_GROUP_MEMBER, replica.key,
                            mcast_group_member_oid);
      old_entry_ptr->member_oids[replica.key] = mcast_group_member_oid;
      m_rifOidToMulticastGroupMembers[new_rif_oid].insert(replica.key);
      added_replicas.push_back(replica);

    }  // for replica (to add)

    // Final bookkeeping.
    // Since we updated the original entry in place, we need to replace the
    // replicas and metadata with the new state.
    old_entry_ptr->replicas.clear();
    old_entry_ptr->replicas.insert(old_entry_ptr->replicas.end(),
                                   entry.replicas.begin(),
                                   entry.replicas.end());
    old_entry_ptr->controller_metadata = entry.controller_metadata;
    old_entry_ptr->multicast_metadata = entry.multicast_metadata;
    statuses[i] = ReturnCode();
  }  // for i
  return statuses;
}

ReturnCode L3MulticastManager::restoreDeletedGroupMembers(
    const std::vector<P4Replica>& deleted_replicas,
    const std::unordered_map<std::string, sai_object_id_t>& replica_rif_map,
    const sai_object_id_t group_oid, const std::string& error_message,
    P4MulticastGroupEntry* old_entry) {
  // Attempt to re-add deleted group members.
  for (auto& deleted_replica : deleted_replicas) {
    sai_object_id_t restore_rif_oid = replica_rif_map.at(deleted_replica.key);
    sai_object_id_t restore_group_member_oid = SAI_NULL_OBJECT_ID;
    auto create_status = createMulticastGroupMember(
        deleted_replica, group_oid, restore_rif_oid, &restore_group_member_oid);
    if (!create_status.ok()) {
      // All kinds of bad.  We couldn't restore a multicast group member,
      // which leaves us in an inconsistent state with what the controller
      // expects.  Leave the overall return code as original failure.
      return ReturnCode(StatusCode::SWSS_RC_INTERNAL)
             << "Unable to restore deleted multicast group member  "
             << QuotedVar(deleted_replica.key)
             << " after group member delete failed on "
             << QuotedVar(error_message);
    }
    // If we successfully added the group member back, update internal
    // state.
    m_p4OidMapper->setOID(SAI_OBJECT_TYPE_IPMC_GROUP_MEMBER,
                          deleted_replica.key, restore_group_member_oid);
    old_entry->member_oids[deleted_replica.key] = restore_group_member_oid;
    m_rifOidToMulticastGroupMembers[restore_rif_oid].insert(
        deleted_replica.key);
  }
  return ReturnCode();
}

std::vector<ReturnCode> L3MulticastManager::deleteMulticastGroupEntries(
    const std::vector<P4MulticastGroupEntry>& entries) {
  // When we delete a multicast group entry, we first delete all its members
  // and then the group object.
  SWSS_LOG_ENTER();

  std::vector<ReturnCode> statuses(entries.size());
  fillStatusArrayWithNotExecuted(statuses, 0);
  for (size_t i = 0; i < entries.size(); ++i) {
    auto& entry = entries[i];

    // Confirm entry exists
    auto* old_entry_ptr = getMulticastGroupEntry(entry.multicast_group_id);
    if (old_entry_ptr == nullptr) {
      statuses[i] = ReturnCode(StatusCode::SWSS_RC_UNKNOWN)
                    << "Multicast group entry is not known "
                    << QuotedVar(entry.multicast_group_id);
      break;
    }

    // Before deleting the group, confirm there are no routes still using the
    // multicast group.
    uint32_t route_entry_ref_count = 1;
    if (!m_p4OidMapper->getRefCount(SAI_OBJECT_TYPE_IPMC_GROUP,
                                    old_entry_ptr->multicast_group_id,
                                    &route_entry_ref_count)) {
      std::stringstream err_msg;
      err_msg << "Unable to fetch reference count for multicast group "
              << old_entry_ptr->multicast_group_id;
      SWSS_LOG_ERROR("%s", err_msg.str().c_str());
      SWSS_RAISE_CRITICAL_STATE(err_msg.str());
      statuses[i] = ReturnCode(StatusCode::SWSS_RC_INTERNAL) << err_msg.str();
      break;
    }
    if (route_entry_ref_count != 0) {
      statuses[i] = ReturnCode(StatusCode::SWSS_RC_IN_USE)
                    << "Multicast group " << old_entry_ptr->multicast_group_id
                    << " cannot be deleted because route entries are still "
                    << "referencing it.";
      break;
    }

    // Fetch the group OID.
    // There's no need to check the return code of getOID, since getRefCount
    // above already checked that the key exists.
    sai_object_id_t old_group_oid = SAI_NULL_OBJECT_ID;
    m_p4OidMapper->getOID(SAI_OBJECT_TYPE_IPMC_GROUP,
                          old_entry_ptr->multicast_group_id, &old_group_oid);

    // Delete group members.  Do internal state-book keeping as go along, since
    // re-allocation does not necessarily result in the same OID.
    std::vector<P4Replica> deleted_replicas;
    std::unordered_map<std::string, sai_object_id_t> replica_rif_map;
    for (auto& replica : old_entry_ptr->replicas) {
      // Fetch the RIF used by the member.
      sai_object_id_t old_rif_oid = getRifOid(replica);
      if (old_rif_oid == SAI_NULL_OBJECT_ID) {
        std::stringstream err_msg;
        err_msg << "Cannot find RIF oid associated with group member to delete "
                << QuotedVar(replica.key);
        SWSS_LOG_ERROR("%s", err_msg.str().c_str());
        SWSS_RAISE_CRITICAL_STATE(err_msg.str());
        statuses[i] = ReturnCode(StatusCode::SWSS_RC_INTERNAL) << err_msg.str();
        return statuses;
      }
      replica_rif_map[replica.key] = old_rif_oid;

      // Fetch the member OID.
      if (old_entry_ptr->member_oids.find(replica.key) ==
          old_entry_ptr->member_oids.end()) {
        std::stringstream err_msg;
        err_msg << "Cannot find oid associated with group member to delete "
                << QuotedVar(replica.key);
        SWSS_LOG_ERROR("%s", err_msg.str().c_str());
        SWSS_RAISE_CRITICAL_STATE(err_msg.str());
        statuses[i] = ReturnCode(StatusCode::SWSS_RC_INTERNAL) << err_msg.str();
        return statuses;
      }
      sai_object_id_t old_group_member_oid =
          old_entry_ptr->member_oids.at(replica.key);

      // Delete group member
      sai_status_t member_delete_status =
          sai_ipmc_group_api->remove_ipmc_group_member(old_group_member_oid);
      if (member_delete_status != SAI_STATUS_SUCCESS) {
        statuses[i] = member_delete_status;

        // Attempt to re-add deleted group members.
        ReturnCode restore_status = restoreDeletedGroupMembers(
            deleted_replicas, replica_rif_map, old_group_oid, replica.key,
            old_entry_ptr);
        if (!restore_status.ok()) {
          SWSS_LOG_ERROR("%s", restore_status.message().c_str());
          SWSS_RAISE_CRITICAL_STATE(restore_status.message());
        }
        // We still return the original failure when we successfully back
        // out changes.
        return statuses;
      }

      // Update internal state to reflect successful delete.
      m_p4OidMapper->eraseOID(SAI_OBJECT_TYPE_IPMC_GROUP_MEMBER, replica.key);
      old_entry_ptr->member_oids.erase(replica.key);
      m_rifOidToMulticastGroupMembers[old_rif_oid].erase(replica.key);
      deleted_replicas.push_back(replica);
    }  // for replicas

    // Now delete the multicast group.
    sai_status_t group_delete_status =
        sai_ipmc_group_api->remove_ipmc_group(old_group_oid);
    if (group_delete_status != SAI_STATUS_SUCCESS) {
      SWSS_LOG_ERROR("Failed to delete multicast group %s",
                     QuotedVar(old_entry_ptr->multicast_group_id).c_str());
      statuses[i] = group_delete_status;
      // On group removal failure, attempt to put the group members back.
      ReturnCode restore_status = restoreDeletedGroupMembers(
          deleted_replicas, replica_rif_map, old_group_oid,
          old_entry_ptr->multicast_group_id, old_entry_ptr);
      if (!restore_status.ok()) {
        SWSS_LOG_ERROR("%s", restore_status.message().c_str());
        SWSS_RAISE_CRITICAL_STATE(restore_status.message());
      }
      return statuses;
    }

    // Do internal bookkeping to remove the multicast group.
    m_p4OidMapper->eraseOID(SAI_OBJECT_TYPE_IPMC_GROUP,
                            old_entry_ptr->multicast_group_id);
    m_multicastGroupEntryTable.erase(old_entry_ptr->multicast_group_id);
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

  ReturnCode status =
      validateMulticastRouterInterfaceEntry(app_db_entry, SET_COMMAND);
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
        << QuotedVar(multicast_router_interface_entry
                         ->multicast_router_interface_entry_key)
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

  if (multicast_router_interface_entry->action == p4orch::kSetMulticastSrcMac) {
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
  if (multicast_router_interface_entry->action == p4orch::kSetMulticastSrcMac) {
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

std::string L3MulticastManager::verifyMulticastGroupStateCache(
    const P4MulticastGroupEntry& app_db_entry,
    const P4MulticastGroupEntry* multicast_group_entry) {
  ReturnCode status = validateMulticastGroupEntry(app_db_entry, SET_COMMAND);
  if (!status.ok()) {
    std::stringstream msg;
    msg << "Validation failed for multicast group DB entry with key "
        << QuotedVar(app_db_entry.multicast_group_id) << ": "
        << status.message();
    return msg.str();
  }
  if (multicast_group_entry->multicast_group_id !=
      app_db_entry.multicast_group_id) {
    std::stringstream msg;
    msg << "Multicast group ID " << QuotedVar(app_db_entry.multicast_group_id)
        << " does not match internal cache "
        << QuotedVar(multicast_group_entry->multicast_group_id)
        << " in l3 multicast manager for group entry.";
    return msg.str();
  }

  // Check replicas
  if (app_db_entry.replicas.size() != multicast_group_entry->replicas.size()) {
    std::stringstream msg;
    msg << "Multicast group ID " << QuotedVar(app_db_entry.multicast_group_id)
        << " has a different number of replicas than internal cache.";
    return msg.str();
  }
  std::unordered_set<std::string> replica_keys;
  for (auto& replica : multicast_group_entry->replicas) {
    replica_keys.insert(replica.key);
  }
  for (auto& replica : app_db_entry.replicas) {
    // Check we have the P4Replica object.
    if (replica_keys.find(replica.key) == replica_keys.end()) {
      std::stringstream msg;
      msg << "Replica " << QuotedVar(replica.key)
          << " is missing from internal cache for multicast group "
          << QuotedVar(multicast_group_entry->multicast_group_id)
          << " in l3 multicast manager for group entry.";
      return msg.str();
    }
    // Check we have the replica in the member_oids map.
    if (multicast_group_entry->member_oids.find(replica.key) ==
        multicast_group_entry->member_oids.end()) {
      std::stringstream msg;
      msg << "Replica " << QuotedVar(replica.key)
          << " is missing from internal member oid map for multicast group "
          << QuotedVar(multicast_group_entry->multicast_group_id)
          << " in l3 multicast manager for group entry.";
      return msg.str();
    }
  }

  if (multicast_group_entry->multicast_metadata !=
      app_db_entry.multicast_metadata) {
    std::stringstream msg;
    msg << "Multicast metadata " << QuotedVar(app_db_entry.multicast_metadata)
        << " does not match internal cache "
        << QuotedVar(multicast_group_entry->multicast_metadata)
        << " in l3 multicast manager for group entry.";
    return msg.str();
  }
  if (multicast_group_entry->controller_metadata !=
      app_db_entry.controller_metadata) {
    std::stringstream msg;
    msg << "Controller metadata " << QuotedVar(app_db_entry.controller_metadata)
        << " does not match internal cache "
        << QuotedVar(multicast_group_entry->controller_metadata)
        << " in l3 multicast manager for group entry.";
    return msg.str();
  }

  std::string group_msg = m_p4OidMapper->verifyOIDMapping(
      SAI_OBJECT_TYPE_IPMC_GROUP, multicast_group_entry->multicast_group_id,
      multicast_group_entry->multicast_group_oid);
  if (!group_msg.empty()) {
    return group_msg;
  }

  // Check group member OIDs for replicas.
  for (auto& kv : multicast_group_entry->member_oids) {
    std::string group_member_msg = m_p4OidMapper->verifyOIDMapping(
        SAI_OBJECT_TYPE_IPMC_GROUP_MEMBER, kv.first, kv.second);
    if (!group_member_msg.empty()) {
      return group_member_msg;
    }
  }
  return "";
}

std::string L3MulticastManager::verifyMulticastGroupStateAsicDb(
    const P4MulticastGroupEntry* multicast_group_entry) {
  // Confirm group settings.
  std::vector<sai_attribute_t> group_attrs;  // no required attributes
  std::vector<swss::FieldValueTuple> exp =
      saimeta::SaiAttributeList::serialize_attr_list(
          SAI_OBJECT_TYPE_IPMC_GROUP, (uint32_t)group_attrs.size(),
          group_attrs.data(), /*countOnly=*/false);

  swss::DBConnector db("ASIC_DB", 0);
  swss::Table table(&db, "ASIC_STATE");
  std::string key =
      sai_serialize_object_type(SAI_OBJECT_TYPE_IPMC_GROUP) + ":" +
      sai_serialize_object_id(multicast_group_entry->multicast_group_oid);
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
  for (auto& replica : multicast_group_entry->replicas) {
    // Confirm have RIF for each replica.
    sai_object_id_t rif_oid = getRifOid(replica);
    if (rif_oid == SAI_NULL_OBJECT_ID) {
      std::stringstream msg;
      msg << "Unable to find RIF associated with replica "
          << QuotedVar(replica.key) << " for multicast group "
          << QuotedVar(multicast_group_entry->multicast_group_id);
      return msg.str();
    }

    sai_object_id_t group_member_oid = SAI_NULL_OBJECT_ID;
    if (multicast_group_entry->member_oids.find(replica.key) !=
        multicast_group_entry->member_oids.end()) {
      group_member_oid = multicast_group_entry->member_oids.at(replica.key);
    }

    auto member_attrs = prepareMulticastGroupMemberSaiAttrs(
        multicast_group_entry->multicast_group_oid, rif_oid);
    exp = saimeta::SaiAttributeList::serialize_attr_list(
        SAI_OBJECT_TYPE_IPMC_GROUP_MEMBER, (uint32_t)member_attrs.size(),
        member_attrs.data(), /*countOnly=*/false);
    key = sai_serialize_object_type(SAI_OBJECT_TYPE_IPMC_GROUP_MEMBER) + ":" +
          sai_serialize_object_id(group_member_oid);
    values.clear();
    if (!table.get(key, values)) {
      return std::string("ASIC DB key not found ") + key;
    }
    std::string group_member_msg =
        verifyAttrs(values, exp, std::vector<swss::FieldValueTuple>{},
                    /*allow_unknown=*/false);
    if (!group_member_msg.empty()) {
      return group_member_msg;
    }
  }
  return "";
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

P4MulticastGroupEntry* L3MulticastManager::getMulticastGroupEntry(
    const std::string& multicast_group_id) {
  SWSS_LOG_ENTER();
  if (m_multicastGroupEntryTable.find(multicast_group_id) ==
      m_multicastGroupEntryTable.end()) {
    return nullptr;
  }
  return &m_multicastGroupEntryTable[multicast_group_id];
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
sai_object_id_t L3MulticastManager::getRifOid(const P4Replica& replica) {
  // Get router interface entry for out port and egress instance.
  const std::string router_interface_key =
      KeyGenerator::generateMulticastRouterInterfaceKey(replica.port,
                                                        replica.instance);
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
