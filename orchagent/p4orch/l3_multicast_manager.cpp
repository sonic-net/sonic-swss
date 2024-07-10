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
extern sai_l2mc_api_t* sai_l2mc_api;
extern sai_l2mc_group_api_t* sai_l2mc_group_api;
extern sai_neighbor_api_t* sai_neighbor_api;
extern sai_next_hop_api_t* sai_next_hop_api;
extern sai_router_interface_api_t* sai_router_intfs_api;
extern sai_bridge_api_t* sai_bridge_api;
extern sai_switch_api_t* sai_switch_api;

extern PortsOrch* gPortsOrch;

namespace p4orch {

namespace {

// Placeholder values  to enable creation of next hop objects and neighbor
// entries.  The link local IP address is effectively a don't care for our use
// case.  The default neighbor MAC address will be ignored, except in the case
// where we re-write the destination MAC.  When we do re-write the MAC address,
// the value will be provided by the P4 action.
constexpr char* kLinkLocalIpv4Address = "169.254.0.1";
constexpr char* kNeighborMacAddress = "00:00:00:00:00:01";

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

  bool use_vlan = multicast_router_interface_entry.action ==
                      p4orch::kMulticastSetSrcMacAndVlanId ||
                  multicast_router_interface_entry.action ==
                      p4orch::kMulticastSetSrcMacAndDstMacAndVlanId;

  std::vector<sai_attribute_t> attrs;
  sai_attribute_t attr;
  // Map all P4 router interfaces to default VRF as virtual router is mandatory
  // parameter for creation of router interfaces in SAI.
  attr.id = SAI_ROUTER_INTERFACE_ATTR_VIRTUAL_ROUTER_ID;
  attr.value.oid = gVirtualRouterId;
  attrs.push_back(attr);

  attr.id = SAI_ROUTER_INTERFACE_ATTR_TYPE;
  if (use_vlan) {
    attr.value.s32 = SAI_ROUTER_INTERFACE_TYPE_SUB_PORT;
  } else {
    attr.value.s32 = SAI_ROUTER_INTERFACE_TYPE_PORT;
  }
  attrs.push_back(attr);
  if (port.m_type != Port::PHY) {
    // If we need to support LAG, VLAN, or other types, we can make this a
    // case statement like:
    // https://source.corp.google.com/h/nss/codesearch/+/master:third_party/
    // sonic-swss/orchagent/p4orch/router_interface_manager.cpp;l=90
    LOG_ERROR_AND_RETURN(ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
                         << "Unexpected port type: " << port.m_type);
  }

  if (use_vlan) {
    attr.id = SAI_ROUTER_INTERFACE_ATTR_OUTER_VLAN_ID;
    attr.value.u16 = multicast_router_interface_entry.vlan_id;
    attrs.push_back(attr);
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

std::vector<sai_attribute_t> prepareNeighborEntrySaiAttrs(
    const swss::MacAddress& dst_mac) {
  std::vector<sai_attribute_t> attrs;
  sai_attribute_t attr;

  attr.id = SAI_NEIGHBOR_ENTRY_ATTR_DST_MAC_ADDRESS;
  memcpy(attr.value.mac, dst_mac.getMac(), sizeof(sai_mac_t));
  attrs.push_back(attr);

  attr.id = SAI_NEIGHBOR_ENTRY_ATTR_NO_HOST_ROUTE;
  attr.value.booldata = true;
  attrs.push_back(attr);

  return attrs;
}

sai_neighbor_entry_t prepareSaiNeighborEntry(const sai_object_id_t rif_oid) {
  sai_neighbor_entry_t neigh_entry;
  neigh_entry.switch_id = gSwitchId;
  neigh_entry.rif_id = rif_oid;
  // IP address is required, but we don't care what's value is as long as it is
  // consistent with the next hop object we create.
  swss::IpAddress link_local_ip = swss::IpAddress(kLinkLocalIpv4Address);
  swss::copy(neigh_entry.ip_address, link_local_ip);
  return neigh_entry;
}

// Create the vector of SAI attributes for creating a new next hop object.
std::vector<sai_attribute_t> prepareNextHopSaiAttrs(
    const P4MulticastRouterInterfaceEntry& multicast_router_interface_entry,
    const sai_object_id_t rif_oid) {
  std::vector<sai_attribute_t> attrs;
  sai_attribute_t attr;

  attr.id = SAI_NEXT_HOP_ATTR_TYPE;
  attr.value.s32 = SAI_NEXT_HOP_TYPE_IPMC;
  attrs.push_back(attr);

  attr.id = SAI_NEXT_HOP_ATTR_ROUTER_INTERFACE_ID;
  attr.value.oid = rif_oid;
  attrs.push_back(attr);

  // IP address is required, but we don't care what's value is as long as it is
  // consistent with the neighbor entry we create.
  swss::IpAddress link_local_ip = swss::IpAddress(kLinkLocalIpv4Address);
  attr.id = SAI_NEXT_HOP_ATTR_IP;
  swss::copy(attr.value.ipaddr, link_local_ip);
  attrs.push_back(attr);

  attr.id = SAI_NEXT_HOP_ATTR_DISABLE_SRC_MAC_REWRITE;
  attr.value.booldata = false;  // All actions write the source MAC.
  attrs.push_back(attr);

  attr.id = SAI_NEXT_HOP_ATTR_DISABLE_DST_MAC_REWRITE;
  // Only the kMulticastSetSrcMacAndDstMacAndVlanId writes dst mac.
  attr.value.booldata = multicast_router_interface_entry.action !=
                        p4orch::kMulticastSetSrcMacAndDstMacAndVlanId;
  attrs.push_back(attr);

  bool write_vlan = multicast_router_interface_entry.action ==
                        p4orch::kMulticastSetSrcMacAndVlanId ||
                    multicast_router_interface_entry.action ==
                        p4orch::kMulticastSetSrcMacAndDstMacAndVlanId ||
                    // In P4, this action is expected to write the internal
                    // VLAN value (not provided from P4).
                    multicast_router_interface_entry.action ==
                        p4orch::kMulticastSetSrcMac;

  attr.id = SAI_NEXT_HOP_ATTR_DISABLE_VLAN_REWRITE;
  attr.value.booldata = !write_vlan;
  attrs.push_back(attr);

  return attrs;
}

// Create the vector of SAI attributes for creating a new bridge port object.
ReturnCodeOr<std::vector<sai_attribute_t>> prepareBridgePortSaiAttrs(
    const P4MulticastRouterInterfaceEntry& entry) {
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

  attr.id = SAI_BRIDGE_PORT_ATTR_ADMIN_STATE;
  attr.value.booldata = true;
  attrs.push_back(attr);

  attr.id = SAI_BRIDGE_PORT_ATTR_FDB_LEARNING_MODE;
  attr.value.s32 = SAI_BRIDGE_PORT_FDB_LEARNING_MODE_DISABLE;
  attrs.push_back(attr);

  return attrs;
}

// Create the vector of SAI attributes for creating a new multicast group
// member object.
std::vector<sai_attribute_t> prepareMulticastGroupMemberSaiAttrs(
    const sai_object_id_t multicast_group_oid, const sai_object_id_t rif_oid,
    const sai_object_id_t next_hop_oid) {
  std::vector<sai_attribute_t> attrs;
  sai_attribute_t attr;

  attr.id = SAI_IPMC_GROUP_MEMBER_ATTR_IPMC_GROUP_ID;
  attr.value.oid = multicast_group_oid;
  attrs.push_back(attr);

  if (next_hop_oid == SAI_NULL_OBJECT_ID) {
    attr.id = SAI_IPMC_GROUP_MEMBER_ATTR_IPMC_OUTPUT_ID;
    attr.value.oid = rif_oid;
    attrs.push_back(attr);
  } else {
    attr.id = SAI_IPMC_GROUP_MEMBER_ATTR_NEXT_HOP;
    attr.value.oid = next_hop_oid;
    attrs.push_back(attr);
  }

  return attrs;
}

// Create the vector of SAI attributes for creating a new L2 multicast group
// member object.
std::vector<sai_attribute_t> prepareL2MulticastGroupMemberSaiAttrs(
    const sai_object_id_t multicast_group_oid,
    const sai_object_id_t bridge_port_oid) {
  std::vector<sai_attribute_t> attrs;
  sai_attribute_t attr;

  attr.id = SAI_L2MC_GROUP_MEMBER_ATTR_L2MC_GROUP_ID;
  attr.value.oid = multicast_group_oid;
  attrs.push_back(attr);

  attr.id = SAI_L2MC_GROUP_MEMBER_ATTR_L2MC_OUTPUT_ID;
  attr.value.oid = bridge_port_oid;
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
    auto group_entry_or = deserializeMulticastGroupEntry(
        key, attributes);

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
    auto* group_entry_ptr = getMulticastGroupEntry(
      group_entry.multicast_group_id);
    bool update = group_entry_ptr != nullptr;

    if (prev_op == "") {
      prev_op = operation;
      prev_update = update;
    }
    // Process the entries if the operation type changes.
    if (operation != prev_op || update != prev_update) {
      status = processMulticastGroupEntries(
          multicast_group_entry_list, tuple_list, prev_op, prev_update);
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
    auto rc = processMulticastGroupEntries(
        multicast_group_entry_list, tuple_list, prev_op, prev_update);
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
  router_interface_entry.src_mac = swss::MacAddress("00:00:00:00:00:00");
  router_interface_entry.dst_mac = swss::MacAddress(kNeighborMacAddress);
  router_interface_entry.vlan_id = 0;

  for (const auto& it : attributes) {
    const auto& field = fvField(it);
    const auto& value = fvValue(it);
    // Note: The kSetSrcMac action is deprecated.  This action is used
    // for the original IP multicast implementation approach where multicast
    // replicas are output to a multicast RIF.  The new IP multicast actions
    // will continue to create a RIF, but they will also create a next hop and
    // neighbor entry.  The next hop object becomes the output target for a
    // multicast replica.
    if (field == p4orch::kAction) {
      if (value == p4orch::kSetMulticastSrcMac ||
          value == p4orch::kMulticastSetSrcMac ||
          value == p4orch::kMulticastSetSrcMacAndVlanId ||
          value == p4orch::kMulticastSetSrcMacAndDstMacAndVlanId ||
          value == p4orch::kMulticastSetSrcMacAndPreserveIngressVlanId ||
          value == p4orch::kL2MulticastPassthrough ||
          value == p4orch::kMulticastL2Passthrough) {
        router_interface_entry.action = value;
      } else {
        return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
               << "Unexpected action " << QuotedVar(value) << " in "
               << APP_P4RT_MULTICAST_ROUTER_INTERFACE_TABLE_NAME;
      }
    } else if (field == prependParamField(p4orch::kSrcMac)) {
      router_interface_entry.src_mac = swss::MacAddress(value);
      router_interface_entry.has_src_mac = true;
    } else if (field == prependParamField(p4orch::kDstMac)) {
      router_interface_entry.dst_mac = swss::MacAddress(value);
      router_interface_entry.has_dst_mac = true;
    } else if (field == prependParamField(p4orch::kVlanId)) {
      try {
        router_interface_entry.vlan_id =
            static_cast<uint16_t>(std::stoul(value, 0, /*base=*/16));
        router_interface_entry.has_vlan_id = true;
      } catch (std::exception& ex) {
        return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
               << "Invalid Vlan ID " << QuotedVar(value) << " of field "
               << QuotedVar(field);
      }
    } else if (field == p4orch::kMulticastMetadata) {
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
  P4RT:REPLICATION_IP_MULTICAST_TABLE:"0x1" {
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
                   << " is missing: "
                   << QuotedVar(value);
          }

          if (replica_map.find(p4orch::kMulticastReplicaInstance) !=
              replica_map.end()) {
            instance = replica_map.at(p4orch::kMulticastReplicaInstance);
          } else {
            return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
                   << "Failed to deserialize multicast replica, "
                   << p4orch::kMulticastReplicaInstance
                   << " is missing: "
                   << QuotedVar(value);
          }

          if (port_name.empty() || instance.empty()) {
            return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
                   << "Invalid multicast group table replica "
                   << QuotedVar(value)
                   << " for key "
                   << QuotedVar(key);
          }

          P4Replica replica = P4Replica(group_entry.multicast_group_id,
                                        port_name,
                                        instance);
	  if (group_entry.replica_keys.find(replica.key) !=
              group_entry.replica_keys.end()) {
            // Duplicate replica invalid
            return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
                   << "Duplicate multicast group table replica "
                   << QuotedVar(field)
                   << " for key "
                   << QuotedVar(key);
          }
          group_entry.replicas.push_back(replica);
	  group_entry.replica_keys.insert(replica.key);
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

std::string L3MulticastManager::verifyMulticastGroupState(
    const std::string& key,
    const std::vector<swss::FieldValueTuple>& tuple) {
  auto app_db_entry_or = deserializeMulticastGroupEntry(
      key, tuple);
  if (!app_db_entry_or.ok()) {
    ReturnCode status = app_db_entry_or.status();
    std::stringstream msg;
    msg << "Unable to deserialize key " << QuotedVar(key) << ": "
        << status.message();
    return msg.str();
  }
  auto& app_db_entry = *app_db_entry_or;

  auto* group_entry_ptr = getMulticastGroupEntry(
      app_db_entry.multicast_group_id);
  if (group_entry_ptr == nullptr) {
    std::stringstream msg;
    msg << "No entry found with key " << QuotedVar(key);
    return msg.str();
  }

  std::string cache_result = verifyMulticastGroupStateCache(
      app_db_entry, group_entry_ptr);
  std::string asic_db_result = verifyMulticastGroupStateAsicDb(
      group_entry_ptr);
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

ReturnCodeOr<bool> L3MulticastManager::validateReplicas(
    const P4MulticastGroupEntry& entry) {
  SWSS_LOG_ENTER();
  // To figure out if we're dealing with L2 or IP multicast groups, we have to
  // check each replica against the `multicast_router_interface_table` entry's
  // action.  If it's `multicast_l2_passthrough`, it's an L2 group.  Otherwise,
  // it's an IP group.
  int ipmc_count = 0;
  int ipmc_rif_oid_count = 0;
  int l2_count = 0;
  int l2_bridge_port_oid_count = 0;
  for (auto& replica : entry.replicas) {
    auto table_key = KeyGenerator::generateMulticastRouterInterfaceKey(
        replica.port, replica.instance);
    auto* router_interface_entry_ptr =
        getMulticastRouterInterfaceEntry(table_key);

    if (router_interface_entry_ptr == nullptr) {
      return ReturnCode(StatusCode::SWSS_RC_NOT_FOUND)
             << "No corresponding "
             << APP_P4RT_MULTICAST_ROUTER_INTERFACE_TABLE_NAME
             << " entry found for multicast group "
             << QuotedVar(replica.multicast_group_id) << " replica "
             << QuotedVar(replica.key);
    } else if (router_interface_entry_ptr->action !=
                   p4orch::kL2MulticastPassthrough &&
               router_interface_entry_ptr->action !=
                   p4orch::kMulticastL2Passthrough) {
      ipmc_count++;
      if (getRifOid(replica) != SAI_NULL_OBJECT_ID) {
        ipmc_rif_oid_count++;
      }
    } else if (router_interface_entry_ptr->action ==
                   p4orch::kL2MulticastPassthrough ||
               router_interface_entry_ptr->action ==
                   p4orch::kMulticastL2Passthrough) {
      l2_count++;
      if (getBridgePortOid(replica) != SAI_NULL_OBJECT_ID) {
        l2_bridge_port_oid_count++;
      }
    }
  }

  // No mixing and matching group member types allowed.
  if (ipmc_count != 0 && l2_count != 0) {
    return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
           << "Multicast group " << QuotedVar(entry.multicast_group_id)
           << " has a mix of IPMC (" << ipmc_count << ") and L2 " << "("
           << l2_count << ") replicas.";
  }
  // All members must have an associated entry.
  if ((ipmc_count + l2_count) != static_cast<int>(entry.replicas.size())) {
    return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
           << "Multicast group " << QuotedVar(entry.multicast_group_id)
           << " has replicas missing associated entries in table "
           << APP_P4RT_MULTICAST_ROUTER_INTERFACE_TABLE_NAME << " (missing "
           << (entry.replicas.size() - l2_count - ipmc_count) << ")";
  }

  if (ipmc_count != 0) {
    // Verify have RIF OID for each replica.
    if (ipmc_count != ipmc_rif_oid_count) {
      return ReturnCode(StatusCode::SWSS_RC_NOT_FOUND)
             << "Multicast group " << QuotedVar(entry.multicast_group_id)
             << " has " << (ipmc_count - ipmc_rif_oid_count)
             << " replicas that do not have an associated RIF programmed yet";
    }
    return true;  // IPMC group
  } else {
    // Verify have Bridge port OID for each replica.
    if (l2_count != l2_bridge_port_oid_count) {
      return ReturnCode(StatusCode::SWSS_RC_NOT_FOUND)
             << "Multicast group " << QuotedVar(entry.multicast_group_id)
             << " has " << (l2_count - l2_bridge_port_oid_count)
             << " replicas that do not have an associated bridge port "
             << "programmed yet";
    }
    return false;  // L2 group
  }
}

ReturnCode L3MulticastManager::validateSetMulticastGroupEntry(
    const P4MulticastGroupEntry& multicast_group_entry) {

  auto* group_entry_ptr = getMulticastGroupEntry(
      multicast_group_entry.multicast_group_id);

  ASSIGN_OR_RETURN(bool is_ipmc, validateReplicas(multicast_group_entry));
  sai_object_type_t sai_group_type = SAI_OBJECT_TYPE_IPMC_GROUP;
  sai_object_type_t sai_group_member_type = SAI_OBJECT_TYPE_IPMC_GROUP_MEMBER;
  if (!is_ipmc) {
    sai_group_type = SAI_OBJECT_TYPE_L2MC_GROUP;
    sai_group_member_type = SAI_OBJECT_TYPE_L2MC_GROUP_MEMBER;
  }

  bool is_update_operation = group_entry_ptr != nullptr;
  if (is_update_operation) {
    // Confirm multicast group had SAI object ID.
    if (!m_p4OidMapper->existsOID(sai_group_type,
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
      bool member_exists_in_mapper =
          m_p4OidMapper->existsOID(sai_group_member_type, replica.key);
      if (group_entry_ptr->replica_keys.find(replica.key) ==
          group_entry_ptr->replica_keys.end()) {  // Add member.
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

  auto* group_entry_ptr = getMulticastGroupEntry(
      multicast_group_entry.multicast_group_id);

  // Can't delete what isn't there.
  if (group_entry_ptr == nullptr) {
    return ReturnCode(StatusCode::SWSS_RC_NOT_FOUND)
        << "Multicast group entry does not exist for group "
        << multicast_group_entry.multicast_group_id;
  }

  ASSIGN_OR_RETURN(bool is_ipmc, validateReplicas(*group_entry_ptr));
  sai_object_type_t sai_group_type = SAI_OBJECT_TYPE_IPMC_GROUP;
  sai_object_type_t sai_group_member_type = SAI_OBJECT_TYPE_IPMC_GROUP_MEMBER;
  if (!is_ipmc) {
    sai_group_type = SAI_OBJECT_TYPE_L2MC_GROUP;
    sai_group_member_type = SAI_OBJECT_TYPE_L2MC_GROUP_MEMBER;
  }

  // Confirm the multicast object ID exists in central mapper.
  if (!m_p4OidMapper->existsOID(sai_group_type,
                                group_entry_ptr->multicast_group_id)) {
    return ReturnCode(StatusCode::SWSS_RC_NOT_FOUND)
        << "Multicast group does not exist in central mapper: "
        << QuotedVar(group_entry_ptr->multicast_group_id);
  }

  // Confirm members had member OIDs.
  for (auto& replica : group_entry_ptr->replicas) {
    if (!m_p4OidMapper->existsOID(sai_group_member_type, replica.key)) {
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
  // Confirm the RIF object ID exists in central mapper.
  if (getRifOid(router_interface_entry_ptr) == SAI_OBJECT_TYPE_NULL) {
    return ReturnCode(StatusCode::SWSS_RC_NOT_FOUND)
           << "RIF was not assigned before updating multicast router "
              "interface "
              "entry with keys "
           << QuotedVar(multicast_router_interface_entry.multicast_replica_port)
           << " and "
           << QuotedVar(
                  multicast_router_interface_entry.multicast_replica_instance);
  }

  if (router_interface_entry_ptr->action == p4orch::kMulticastSetSrcMac ||
      router_interface_entry_ptr->action ==
          p4orch::kMulticastSetSrcMacAndVlanId ||
      router_interface_entry_ptr->action ==
          p4orch::kMulticastSetSrcMacAndDstMacAndVlanId ||
      router_interface_entry_ptr->action ==
          p4orch::kMulticastSetSrcMacAndPreserveIngressVlanId) {
    // Confirm the next hop object ID exists in central mapper.
    if (getNextHopOid(router_interface_entry_ptr) == SAI_OBJECT_TYPE_NULL) {
      return ReturnCode(StatusCode::SWSS_RC_NOT_FOUND)
             << "Next hop was not assigned before updating multicast router "
                "interface entry with key "
             << QuotedVar(multicast_router_interface_entry
                              .multicast_router_interface_entry_key);
    }
  }

  return ReturnCode();
}

ReturnCode L3MulticastManager::validateL2MulticastRouterInterfaceEntry(
    const P4MulticastRouterInterfaceEntry& multicast_router_interface_entry,
    const P4MulticastRouterInterfaceEntry* router_interface_entry_ptr) {
  // Confirm bridge port ID exists in central mapper.
  sai_object_id_t bridge_port_oid =
      getBridgePortOid(router_interface_entry_ptr);
  if (bridge_port_oid == SAI_NULL_OBJECT_ID) {
    return ReturnCode(StatusCode::SWSS_RC_NOT_FOUND)
           << "Multicast router interface entry exists in manager but bridge "
           << "port for "
           << QuotedVar(router_interface_entry_ptr->multicast_replica_port)
           << " does not exist in the centralized map";
  }
  return ReturnCode();
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

  // Confirm src_mac is populated.
  bool need_src_mac =
      multicast_router_interface_entry.action == p4orch::kSetMulticastSrcMac ||
      multicast_router_interface_entry.action == p4orch::kMulticastSetSrcMac ||
      multicast_router_interface_entry.action ==
          p4orch::kMulticastSetSrcMacAndVlanId ||
      multicast_router_interface_entry.action ==
          p4orch::kMulticastSetSrcMacAndDstMacAndVlanId ||
      multicast_router_interface_entry.action ==
          p4orch::kMulticastSetSrcMacAndPreserveIngressVlanId;
  bool need_dst_mac = multicast_router_interface_entry.action ==
                      p4orch::kMulticastSetSrcMacAndDstMacAndVlanId;
  bool need_vlan_id = multicast_router_interface_entry.action ==
                          p4orch::kMulticastSetSrcMacAndVlanId ||
                      multicast_router_interface_entry.action ==
                          p4orch::kMulticastSetSrcMacAndDstMacAndVlanId;

  if (need_src_mac && !multicast_router_interface_entry.has_src_mac) {
    return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
           << "Multicast router interface entry did not specify a src mac.";
  }

  // Confirm dst_mac is populated.
  if (need_dst_mac && !multicast_router_interface_entry.has_dst_mac) {
    return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
           << "Multicast router interface entry did not specify a dst mac.";
  }

  // Confirm vlan_id is populated.
  if (need_vlan_id && !multicast_router_interface_entry.has_vlan_id) {
    return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
           << "Multicast router interface entry did not specify a Vlan ID.";
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
            p4orch::kL2MulticastPassthrough ||
        multicast_router_interface_entry.action ==
            p4orch::kMulticastL2Passthrough) {
      return validateL2MulticastRouterInterfaceEntry(
          multicast_router_interface_entry, router_interface_entry_ptr);
    } else {
      return validateL3SetMulticastRouterInterfaceEntry(
          multicast_router_interface_entry, router_interface_entry_ptr);
    }
  }
  // No additional validation required for add operation.
  return ReturnCode();
}

ReturnCode L3MulticastManager::validateL3DelMulticastRouterInterfaceEntry(
    const P4MulticastRouterInterfaceEntry& multicast_router_interface_entry,
    const P4MulticastRouterInterfaceEntry* router_interface_entry_ptr) {
  // Confirm we have a reference to the RIF object ID.
  if (getRifOid(router_interface_entry_ptr) == SAI_OBJECT_TYPE_NULL) {
    return ReturnCode(StatusCode::SWSS_RC_NOT_FOUND)
           << "RIF was not assigned before updating multicast router "
              "interface "
              "entry with keys "
           << QuotedVar(multicast_router_interface_entry.multicast_replica_port)
           << " and "
           << QuotedVar(
                  multicast_router_interface_entry.multicast_replica_instance);
  }

  if (router_interface_entry_ptr->action == p4orch::kMulticastSetSrcMac ||
      router_interface_entry_ptr->action ==
          p4orch::kMulticastSetSrcMacAndVlanId ||
      router_interface_entry_ptr->action ==
          p4orch::kMulticastSetSrcMacAndDstMacAndVlanId ||
      router_interface_entry_ptr->action ==
          p4orch::kMulticastSetSrcMacAndPreserveIngressVlanId) {
    // Confirm the next hop object ID exists in central mapper.
    if (getNextHopOid(router_interface_entry_ptr) == SAI_OBJECT_TYPE_NULL) {
      return ReturnCode(StatusCode::SWSS_RC_NOT_FOUND)
             << "Next hop was not assigned before updating multicast router "
                "interface entry with key "
             << QuotedVar(multicast_router_interface_entry
                              .multicast_router_interface_entry_key);
    }
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

  if (router_interface_entry_ptr->action == p4orch::kL2MulticastPassthrough ||
      router_interface_entry_ptr->action == p4orch::kMulticastL2Passthrough) {
    return validateL2MulticastRouterInterfaceEntry(
        multicast_router_interface_entry, router_interface_entry_ptr);
  } else {
    return validateL3DelMulticastRouterInterfaceEntry(
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
  ASSIGN_OR_RETURN(std::vector<sai_attribute_t> attrs,
                   prepareBridgePortSaiAttrs(entry));

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
    P4MulticastRouterInterfaceEntry& entry, sai_object_id_t* rif_oid) {
  SWSS_LOG_ENTER();

  // Confirm we haven't already created a RIF for this.
  if (m_p4OidMapper->existsOID(SAI_OBJECT_TYPE_ROUTER_INTERFACE,
                               entry.multicast_router_interface_entry_key)) {
    RETURN_INTERNAL_ERROR_AND_RAISE_CRITICAL(
        "Router interface to be used by multicast router interface table "
        << QuotedVar(entry.multicast_router_interface_entry_key)
        << " already exists in the centralized map");
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
        << "table: "
        << QuotedVar(entry.multicast_router_interface_entry_key).c_str());
  }
  return ReturnCode();
}

ReturnCode L3MulticastManager::createNeighborEntry(
    P4MulticastRouterInterfaceEntry& entry, const sai_object_id_t rif_oid) {
  SWSS_LOG_ENTER();

  std::vector<sai_attribute_t> attrs =
      prepareNeighborEntrySaiAttrs(entry.dst_mac);

  entry.sai_neighbor_entry = prepareSaiNeighborEntry(rif_oid);

  auto sai_status = sai_neighbor_api->create_neighbor_entry(
      &entry.sai_neighbor_entry, (uint32_t)attrs.size(), attrs.data());
  if (sai_status != SAI_STATUS_SUCCESS) {
    LOG_ERROR_AND_RETURN(
        ReturnCode(sai_status)
        << "Failed to create neighbor entry multicast router interface "
        << "table: "
        << QuotedVar(entry.multicast_router_interface_entry_key).c_str());
  }
  return ReturnCode();
}

ReturnCode L3MulticastManager::createNextHop(
    P4MulticastRouterInterfaceEntry& entry, const sai_object_id_t rif_oid,
    sai_object_id_t* next_hop_oid) {
  SWSS_LOG_ENTER();

  // Confirm we haven't already created a next hop for this.
  if (m_p4OidMapper->existsOID(SAI_OBJECT_TYPE_NEXT_HOP,
                               entry.multicast_router_interface_entry_key)) {
    RETURN_INTERNAL_ERROR_AND_RAISE_CRITICAL(
        "Next hop to be used by multicast router interface table "
        << QuotedVar(entry.multicast_router_interface_entry_key)
        << " already exists in the centralized map");
  }

  RETURN_IF_ERROR(createNeighborEntry(entry, rif_oid));

  // Create next hop SAI object.
  std::vector<sai_attribute_t> attrs = prepareNextHopSaiAttrs(entry, rif_oid);
  auto sai_status = sai_next_hop_api->create_next_hop(
      next_hop_oid, gSwitchId, (uint32_t)attrs.size(), attrs.data());
  if (sai_status != SAI_STATUS_SUCCESS) {
    // Back-out creation of neighbor entry.
    sai_status_t del_status =
        sai_neighbor_api->remove_neighbor_entry(&entry.sai_neighbor_entry);

    if (del_status != SAI_STATUS_SUCCESS) {
      // All kinds of bad.  The delete failed, and we have to leave a
      // dangling neighbor entry.
      std::stringstream err_msg;
      err_msg << "Next hop creation failed, and we were "
              << "unable to backout creation of the neighbor entry."
              << QuotedVar(entry.multicast_router_interface_entry_key);
      SWSS_LOG_ERROR("%s", err_msg.str().c_str());
      SWSS_RAISE_CRITICAL_STATE(err_msg.str());
    }

    LOG_ERROR_AND_RETURN(
        ReturnCode(sai_status)
        << "Failed to create next hop for multicast router interface "
        << "table: "
        << QuotedVar(entry.multicast_router_interface_entry_key).c_str());
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

ReturnCode L3MulticastManager::deleteNextHop(
    P4MulticastRouterInterfaceEntry* entry,
    const sai_object_id_t next_hop_oid) {
  SWSS_LOG_ENTER();
  // Confirm we have a next hop to be deleted.
  if (!m_p4OidMapper->existsOID(SAI_OBJECT_TYPE_NEXT_HOP,
                                entry->multicast_router_interface_entry_key)) {
    LOG_ERROR_AND_RETURN(
        ReturnCode(StatusCode::SWSS_RC_INTERNAL)
        << "Next hop to be deleted by multicast router interface table "
        << QuotedVar(entry->multicast_router_interface_entry_key)
        << " does not exist in the centralized map");
  }
  auto sai_status = sai_next_hop_api->remove_next_hop(next_hop_oid);
  if (sai_status != SAI_STATUS_SUCCESS) {
    LOG_ERROR_AND_RETURN(
        ReturnCode(sai_status)
        << "Failed to remove next hop for multicast router interface "
        << "table: "
        << QuotedVar(entry->multicast_router_interface_entry_key).c_str());
  }

  // Erase OID from mapper.  We do this here in case neighbor entry delete fails
  // and we have to re-add the next hop.
  m_p4OidMapper->eraseOID(SAI_OBJECT_TYPE_NEXT_HOP,
                          entry->multicast_router_interface_entry_key);

  sai_status_t del_status =
      sai_neighbor_api->remove_neighbor_entry(&entry->sai_neighbor_entry);
  if (del_status != SAI_STATUS_SUCCESS) {
    // Attempt to re-add next hop just deleted.
    sai_object_id_t rif_oid = getRifOid(entry);
    sai_object_id_t new_next_hop_oid = SAI_NULL_OBJECT_ID;
    std::vector<sai_attribute_t> attrs =
        prepareNextHopSaiAttrs(*entry, rif_oid);
    auto add_status = sai_next_hop_api->create_next_hop(
        &new_next_hop_oid, gSwitchId, (uint32_t)attrs.size(), attrs.data());
    if (add_status != SAI_STATUS_SUCCESS) {
      // All kinds of bad.  The create failed, and we couldn't restore the
      // previous system state.
      std::stringstream err_msg;
      err_msg << "Neighbor entry delete failed, and we were unable to re-add "
              << "the next hop object that had been removed for "
              << QuotedVar(entry->multicast_router_interface_entry_key);
      SWSS_LOG_ERROR("%s", err_msg.str().c_str());
      SWSS_RAISE_CRITICAL_STATE(err_msg.str());
    } else {
      // Re-add was successful.
      m_p4OidMapper->setOID(SAI_OBJECT_TYPE_NEXT_HOP,
                            entry->multicast_router_interface_entry_key,
                            new_next_hop_oid);
    }
    // Return original error.
    return ReturnCode(del_status)
           << "Failed to remove neighbor entry for multicast router interface "
           << "table: "
           << QuotedVar(entry->multicast_router_interface_entry_key).c_str();
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
  if (m_p4OidMapper->existsOID(
        SAI_OBJECT_TYPE_IPMC_GROUP, entry.multicast_group_id)) {
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
  if (m_p4OidMapper->existsOID(
        SAI_OBJECT_TYPE_IPMC_GROUP_MEMBER, replica.key)) {
    LOG_ERROR_AND_RETURN(ReturnCode(StatusCode::SWSS_RC_INTERNAL)
        << "Multicast group member to be added "
        << QuotedVar(replica.key).c_str()
        << " already exists in the centralized map");
  }

  if (rif_oid == SAI_NULL_OBJECT_ID) {
    LOG_ERROR_AND_RETURN(ReturnCode(StatusCode::SWSS_RC_UNAVAIL)
        << "Multicast group member "
        << QuotedVar(replica.key).c_str()
        << " cannot be added because there is no associated RIF available");
  }

  // Ok to be null, since some actions do not allocate a next hop.
  sai_object_id_t next_hop_oid = getNextHopOid(replica);

  // Create Multicast group member SAI object.
  std::vector<sai_attribute_t> attrs =
      prepareMulticastGroupMemberSaiAttrs(group_oid, rif_oid, next_hop_oid);

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

ReturnCode L3MulticastManager::createL2MulticastGroup(
    P4MulticastGroupEntry& entry, sai_object_id_t* mcast_group_oid) {
  SWSS_LOG_ENTER();
  // Confirm we haven't already created a multicast group for this.
  if (m_p4OidMapper->existsOID(SAI_OBJECT_TYPE_L2MC_GROUP,
                               entry.multicast_group_id)) {
    LOG_ERROR_AND_RETURN(ReturnCode(StatusCode::SWSS_RC_INTERNAL)
                         << "L2 multicast group to be added with group ID "
                         << QuotedVar(entry.multicast_group_id).c_str()
                         << " already exists in the centralized map");
  }

  // Create L2 multicast group SAI object.
  // There are no required attributes to create a group.
  std::vector<sai_attribute_t> attrs;
  auto sai_status = sai_l2mc_group_api->create_l2mc_group(
      mcast_group_oid, gSwitchId, (uint32_t)attrs.size(), attrs.data());
  if (sai_status != SAI_STATUS_SUCCESS) {
    LOG_ERROR_AND_RETURN(ReturnCode(sai_status)
                         << "Failed to create L2 multicast group for group ID: "
                         << QuotedVar(entry.multicast_group_id).c_str());
  }
  return ReturnCode();
}

ReturnCode L3MulticastManager::createL2MulticastGroupMember(
    const P4Replica& replica, const sai_object_id_t group_oid,
    const sai_object_id_t bridge_port_oid,
    sai_object_id_t* mcast_group_member_oid) {
  SWSS_LOG_ENTER();
  // Confirm we haven't already created a L2 multicast group member for this.
  if (m_p4OidMapper->existsOID(SAI_OBJECT_TYPE_L2MC_GROUP_MEMBER,
                               replica.key)) {
    LOG_ERROR_AND_RETURN(ReturnCode(StatusCode::SWSS_RC_INTERNAL)
                         << "L2 multicast group member to be added "
                         << QuotedVar(replica.key).c_str()
                         << " already exists in the centralized map");
  }

  if (bridge_port_oid == SAI_NULL_OBJECT_ID) {
    LOG_ERROR_AND_RETURN(ReturnCode(StatusCode::SWSS_RC_UNAVAIL)
                         << "L2 multicast group member "
                         << QuotedVar(replica.key).c_str()
                         << " cannot be added because there is no associated "
                            "bridge port available");
  }

  // Create L2 multicast group member SAI object.
  std::vector<sai_attribute_t> attrs =
      prepareL2MulticastGroupMemberSaiAttrs(group_oid, bridge_port_oid);

  auto sai_status = sai_l2mc_group_api->create_l2mc_group_member(
      mcast_group_member_oid, gSwitchId, (uint32_t)attrs.size(), attrs.data());
  if (sai_status != SAI_STATUS_SUCCESS) {
    LOG_ERROR_AND_RETURN(ReturnCode(sai_status)
                         << "Failed to create L2 multicast group member for: "
                         << QuotedVar(replica.key).c_str());
  }
  return ReturnCode();
}

ReturnCode L3MulticastManager::deleteL2MulticastGroup(
    const std::string& multicast_group_id, sai_object_id_t mcast_group_oid) {
  SWSS_LOG_ENTER();
  // Confirm we have a multicast group to delete.
  if (!m_p4OidMapper->existsOID(SAI_OBJECT_TYPE_L2MC_GROUP,
                                multicast_group_id)) {
    LOG_ERROR_AND_RETURN(ReturnCode(StatusCode::SWSS_RC_INTERNAL)
                         << "L2 multicast group to be deleted with group ID "
                         << QuotedVar(multicast_group_id).c_str()
                         << " does not exist in the centralized map");
  }
  auto sai_status = sai_l2mc_group_api->remove_l2mc_group(mcast_group_oid);
  if (sai_status != SAI_STATUS_SUCCESS) {
    LOG_ERROR_AND_RETURN(ReturnCode(sai_status)
                         << "Failed to delete L2 multicast group with ID: "
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
    if (entry.action == p4orch::kL2MulticastPassthrough ||
        entry.action == p4orch::kMulticastL2Passthrough) {
      statuses[i] = addL2MulticastRouterInterfaceEntry(entry);
    } else {
      statuses[i] = addL3MulticastRouterInterfaceEntry(entry);
    }
    if (!statuses[i].ok()) {
      break;
    }
  }
  return statuses;
}

ReturnCode L3MulticastManager::addL3MulticastRouterInterfaceEntry(
    P4MulticastRouterInterfaceEntry& entry) {
  // We no longer share RIFs, so adding a new entry requires allocating a RIF.
  SWSS_LOG_ENTER();

  sai_object_id_t rif_oid = SAI_NULL_OBJECT_ID;
  RETURN_IF_ERROR(createRouterInterface(entry, &rif_oid));

  // Need to set RIF in mapper in case have to back out.
  m_p4OidMapper->setOID(SAI_OBJECT_TYPE_ROUTER_INTERFACE,
                        entry.multicast_router_interface_entry_key, rif_oid);

  // For re-factoring purposes, only the new actions will setup the next hop.
  if (entry.action != p4orch::kSetMulticastSrcMac) {
    sai_object_id_t next_hop_oid = SAI_NULL_OBJECT_ID;
    ReturnCode nh_status = createNextHop(entry, rif_oid, &next_hop_oid);
    if (!nh_status.ok()) {
      ReturnCode del_status = deleteRouterInterface(
          entry.multicast_router_interface_entry_key, rif_oid);
      m_p4OidMapper->eraseOID(SAI_OBJECT_TYPE_ROUTER_INTERFACE,
                              entry.multicast_router_interface_entry_key);
      if (!del_status.ok()) {
        // All kinds of bad.  The delete failed, and we have to leave a
        // dangling allocated RIF
        std::stringstream err_msg;
        err_msg << "Next hop creation failed, and we were "
                << "unable to backout creation of the RIF for "
                << QuotedVar(entry.multicast_router_interface_entry_key);
        SWSS_LOG_ERROR("%s", err_msg.str().c_str());
        SWSS_RAISE_CRITICAL_STATE(err_msg.str());
      }

      // Return original failure.
      return nh_status;
    }
    m_p4OidMapper->setOID(SAI_OBJECT_TYPE_NEXT_HOP,
                          entry.multicast_router_interface_entry_key,
                          next_hop_oid);
  }

  // Update internal state.
  gPortsOrch->increasePortRefCount(entry.multicast_replica_port);
  m_multicastRouterInterfaceTable[entry.multicast_router_interface_entry_key] =
      entry;
  return ReturnCode();
}

ReturnCode L3MulticastManager::addL2MulticastRouterInterfaceEntry(
    P4MulticastRouterInterfaceEntry& entry) {
  // We cannot share bridge ports among replicas in the same L2 multicast group,
  // so we have to allocate a unique bridge port object, even if the same port
  // is used (but a different instance).
  SWSS_LOG_ENTER();

  sai_object_id_t bridge_port_oid = SAI_NULL_OBJECT_ID;
  RETURN_IF_ERROR(createBridgePort(entry, &bridge_port_oid));
  gPortsOrch->increasePortRefCount(entry.multicast_replica_port);
  m_p4OidMapper->setOID(SAI_OBJECT_TYPE_BRIDGE_PORT,
                        entry.multicast_router_interface_entry_key,
                        bridge_port_oid);
  m_multicastRouterInterfaceTable[entry.multicast_router_interface_entry_key] =
      entry;
  return ReturnCode();
}

ReturnCode L3MulticastManager::setDstMac(
    const swss::MacAddress& new_dst_mac,
    P4MulticastRouterInterfaceEntry* existing_entry) {
  SWSS_LOG_ENTER();

  sai_attribute_t attr;
  attr.id = SAI_NEIGHBOR_ENTRY_ATTR_DST_MAC_ADDRESS;
  memcpy(attr.value.mac, new_dst_mac.getMac(), sizeof(sai_mac_t));

  sai_status_t update_status = sai_neighbor_api->set_neighbor_entry_attribute(
      &existing_entry->sai_neighbor_entry, &attr);
  if (update_status != SAI_STATUS_SUCCESS) {
    return ReturnCode(update_status)
           << "Unable to update Dst MAC from "
           << QuotedVar(existing_entry->dst_mac.to_string()) << " to "
           << QuotedVar(new_dst_mac.to_string()) << " for entry "
           << QuotedVar(existing_entry->multicast_router_interface_entry_key);
  }

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

    // Since action kMulticastL2Passthrough, used to setup L2 multicast bridge
    // ports, does not have any parameters, there is nothing to update.
    if (old_entry_ptr->action == p4orch::kL2MulticastPassthrough ||
        old_entry_ptr->action == p4orch::kMulticastL2Passthrough) {
      statuses[i] = ReturnCode();
      continue;
    }

    // VLAN ID is a "create only" attribute.  It cannot be modified without
    // deleting the RIF.
    if (old_entry_ptr->vlan_id != entry.vlan_id) {
      statuses[i] = ReturnCode(StatusCode::SWSS_RC_UNIMPLEMENTED)
                    << "VLAN ID cannot be updated from '"
                    << old_entry_ptr->vlan_id << "' to '" << entry.vlan_id
                    << "' for entry "
                    << QuotedVar(entry.multicast_router_interface_entry_key);
      break;
    }

    // Dst MAC is part of the neighbor entry.
    if (old_entry_ptr->dst_mac != entry.dst_mac) {
      statuses[i] = setDstMac(entry.dst_mac, old_entry_ptr);
      if (!statuses[i].ok()) {
        break;
      }
    }

    // No change to src mac means there is nothing else to do.
    if (old_entry_ptr->src_mac == entry.src_mac) {
      SWSS_LOG_INFO(
          "No update required for %s because the src mac did not change",
          QuotedVar(entry.multicast_router_interface_entry_key).c_str());

      // Replace table with new entry if Dst MAC changed.
      if (old_entry_ptr->dst_mac != entry.dst_mac) {
        entry.sai_neighbor_entry = std::move(old_entry_ptr->sai_neighbor_entry);
        m_multicastRouterInterfaceTable.erase(
            old_entry_ptr->multicast_router_interface_entry_key);
        m_multicastRouterInterfaceTable
            [entry.multicast_router_interface_entry_key] = entry;
      }
      statuses[i] = ReturnCode();
      continue;
    }

    // Confirm RIF OID was assigned (for the old entry).
    sai_object_id_t old_rif_oid = getRifOid(old_entry_ptr);
    if (old_rif_oid == SAI_NULL_OBJECT_ID) {
      std::stringstream err_msg;
      err_msg << "Multicast router interface entry is missing a RIF oid "
              << QuotedVar(old_entry_ptr->multicast_router_interface_entry_key);
      SWSS_LOG_ERROR("%s", err_msg.str().c_str());
      SWSS_RAISE_CRITICAL_STATE(err_msg.str());
      statuses[i] = ReturnCode(StatusCode::SWSS_RC_INTERNAL) << err_msg.str();
      break;
    }

    // Update the MAC address.
    sai_attribute_t new_mac_attr;
    new_mac_attr.id = SAI_ROUTER_INTERFACE_ATTR_SRC_MAC_ADDRESS;
    memcpy(new_mac_attr.value.mac, entry.src_mac.getMac(), sizeof(sai_mac_t));

    sai_status_t new_mac_status =
        sai_router_intfs_api->set_router_interface_attribute(old_rif_oid,
                                                             &new_mac_attr);
    if (new_mac_status != SAI_STATUS_SUCCESS) {
      std::stringstream err_msg;
      err_msg << "Unable to update Src MAC address from "
              << QuotedVar(old_entry_ptr->src_mac.to_string()) << " to "
              << QuotedVar(entry.src_mac.to_string());
      SWSS_LOG_ERROR("%s", err_msg.str().c_str());
      statuses[i] = ReturnCode(new_mac_status) << err_msg.str();

      // Restore Dst MAC if it was changed.
      if (old_entry_ptr->dst_mac != entry.dst_mac) {
        ReturnCode restore_status =
            setDstMac(old_entry_ptr->dst_mac, old_entry_ptr);
        if (!restore_status.ok()) {
          std::stringstream err_msg;
          err_msg << "Unable to restore Dst MAC address back to "
                  << QuotedVar(old_entry_ptr->dst_mac.to_string())
                  << " after failure to update Src MAC address";
          SWSS_RAISE_CRITICAL_STATE(err_msg.str());
          SWSS_LOG_ERROR("%s", err_msg.str().c_str());
        }
      }

      break;
    }

    // Replace table with new entry.
    entry.sai_neighbor_entry = std::move(old_entry_ptr->sai_neighbor_entry);
    m_multicastRouterInterfaceTable.erase(
        old_entry_ptr->multicast_router_interface_entry_key);
    m_multicastRouterInterfaceTable[entry
                                        .multicast_router_interface_entry_key] =
        entry;
    statuses[i] = ReturnCode();
  }  // for entries
  return statuses;
}

ReturnCode L3MulticastManager::deleteL2MulticastRouterInterfaceEntry(
    const P4MulticastRouterInterfaceEntry* entry) {
  SWSS_LOG_ENTER();
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

  // To delete the entry, no more multicast group members can reference it.
  uint32_t bridge_port_ref_count = 1;
  m_p4OidMapper->getRefCount(SAI_OBJECT_TYPE_BRIDGE_PORT,
                             entry->multicast_router_interface_entry_key,
                             &bridge_port_ref_count);
  if (bridge_port_ref_count > 0) {
    return ReturnCode(StatusCode::SWSS_RC_IN_USE)
           << "Entry " << QuotedVar(entry->multicast_router_interface_entry_key)
           << " cannot be deleted, because it is still used by L2 multicast "
           << "group members";
  }

  RETURN_IF_ERROR(
      deleteBridgePort(entry->multicast_replica_port, bridge_port_oid));

  m_p4OidMapper->eraseOID(SAI_OBJECT_TYPE_BRIDGE_PORT,
                          entry->multicast_router_interface_entry_key);
  gPortsOrch->decreasePortRefCount(entry->multicast_replica_port);

  // Finally, remove the P4MulticastRouterInterfaceEntry.
  m_multicastRouterInterfaceTable.erase(
      entry->multicast_router_interface_entry_key);
  return ReturnCode();
}

ReturnCode L3MulticastManager::deleteL3MulticastRouterInterfaceEntry(
    P4MulticastRouterInterfaceEntry* entry) {
  SWSS_LOG_ENTER();
  // RIFs are no longer shared by multiple table entries, so confirm entry is
  // no longer referenced by multicast replicas.

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

  // To delete the entry, no more multicast group members can reference it.
  uint32_t rif_ref_count = 1;
  m_p4OidMapper->getRefCount(SAI_OBJECT_TYPE_ROUTER_INTERFACE,
                             entry->multicast_router_interface_entry_key,
                             &rif_ref_count);
  if (rif_ref_count > 0) {
    return ReturnCode(StatusCode::SWSS_RC_IN_USE)
           << "Entry " << QuotedVar(entry->multicast_router_interface_entry_key)
           << " cannot be deleted, because it is still used by IP multicast "
           << "group members";
  }

  // For re-factoring purposes, only the new actions will setup the next hop.
  if (entry->action != p4orch::kSetMulticastSrcMac) {
    sai_object_id_t next_hop_oid = getNextHopOid(entry);
    RETURN_IF_ERROR(deleteNextHop(entry, next_hop_oid));
    // deleteNextHop deletes next hop OID from oid mapper.
  }

  // Delete the RIF.
  // Attempt to delete RIF at SAI layer before adjusting internal maps, in
  // case there is an error.
  ReturnCode del_rif_rc = deleteRouterInterface(
      entry->multicast_router_interface_entry_key, rif_oid);

  if (!del_rif_rc.ok()) {
    if (entry->action != p4orch::kSetMulticastSrcMac) {
      // Try to restore next hop
      sai_object_id_t new_next_hop_oid = SAI_NULL_OBJECT_ID;
      ReturnCode nh_status = createNextHop(*entry, rif_oid, &new_next_hop_oid);
      if (!nh_status.ok()) {
        // All kinds of bad.  The create failed, and we couldn't restore the
        // previous system state.
        std::stringstream err_msg;
        err_msg
            << "Router interface delete failed, and we were unable to re-add "
            << "the next hop object that had been removed for "
            << QuotedVar(entry->multicast_router_interface_entry_key);
        SWSS_LOG_ERROR("%s", err_msg.str().c_str());
        SWSS_RAISE_CRITICAL_STATE(err_msg.str());
      } else {
        // Re-add was successful.
        m_p4OidMapper->setOID(SAI_OBJECT_TYPE_NEXT_HOP,
                              entry->multicast_router_interface_entry_key,
                              new_next_hop_oid);
      }
    }
    return del_rif_rc;
  }

  m_p4OidMapper->eraseOID(SAI_OBJECT_TYPE_ROUTER_INTERFACE,
                          entry->multicast_router_interface_entry_key);
  gPortsOrch->decreasePortRefCount(entry->multicast_replica_port);

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

    if (old_entry_ptr->action == p4orch::kL2MulticastPassthrough ||
        old_entry_ptr->action == p4orch::kMulticastL2Passthrough) {
      statuses[i] = deleteL2MulticastRouterInterfaceEntry(old_entry_ptr);
    } else {
      statuses[i] = deleteL3MulticastRouterInterfaceEntry(old_entry_ptr);
    }
    if (!statuses[i].ok()) {
      break;
    }
  }
  return statuses;
}

ReturnCode L3MulticastManager::addIpMulticastGroupEntry(
    P4MulticastGroupEntry& entry) {
  SWSS_LOG_ENTER();

  // Create the multicast group.
  sai_object_id_t mcast_group_oid = SAI_NULL_OBJECT_ID;
  RETURN_IF_ERROR(createMulticastGroup(entry, &mcast_group_oid));

  // Update internal book-keeping for new multicast group.
  m_p4OidMapper->setOID(SAI_OBJECT_TYPE_IPMC_GROUP, entry.multicast_group_id,
                        mcast_group_oid);

  // Next, create the group members.  If there's a failure, back out.
  // Instead of updating internal state as members are created, wait until all
  // members have been created to simplify back-out.
  std::unordered_map<std::string, sai_object_id_t> created_member_map;
  for (auto& replica : entry.replicas) {
    sai_object_id_t rif_oid = getRifOid(replica);

    // Create the group member.
    sai_object_id_t mcast_group_member_oid;
    ReturnCode create_member_status = createMulticastGroupMember(
        replica, mcast_group_oid, rif_oid, &mcast_group_member_oid);

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
          return create_member_status;
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
        m_p4OidMapper->eraseOID(SAI_OBJECT_TYPE_IPMC_GROUP,
                                entry.multicast_group_id);
      }
      // Stop trying to create replicas.  Return the first error.
      return create_member_status;
    }
    // We successfully created a group member.
    created_member_map[replica.key] = mcast_group_member_oid;
    // We defer additional book-keeping until all replicas are created.
  }  // for replica

  // Finish with book keeping.

  // Update state for created group members.
  for (auto& created_members : created_member_map) {
    m_p4OidMapper->setOID(SAI_OBJECT_TYPE_IPMC_GROUP_MEMBER,
                          created_members.first, created_members.second);
  }

  // Group members reference multicast_router_interface entries.
  for (auto& replica : entry.replicas) {
    const std::string router_interface_key =
        KeyGenerator::generateMulticastRouterInterfaceKey(replica.port,
                                                          replica.instance);
    m_p4OidMapper->increaseRefCount(SAI_OBJECT_TYPE_ROUTER_INTERFACE,
                                    router_interface_key);
  }

  // Update internal state.
  m_multicastGroupEntryTable[entry.multicast_group_id] = entry;
  return ReturnCode();
}

ReturnCode L3MulticastManager::activateL2MulticastGroup(
    const sai_object_id_t l2mc_group_oid) {
  SWSS_LOG_ENTER();

  sai_l2mc_entry_t sai_entry;

  sai_entry.switch_id = gSwitchId;

  sai_attribute_t attr;
  attr.id = SAI_SWITCH_ATTR_DEFAULT_VLAN_ID;

  sai_status_t status =
      sai_switch_api->get_switch_attribute(gSwitchId,
                                           /*attr_count=*/1, &attr);
  if (status != SAI_STATUS_SUCCESS) {
    LOG_ERROR_AND_RETURN(ReturnCode(status)
                         << "Unable to fetch default VLAN OID for l2mc entry");
  }
  sai_entry.bv_id = attr.value.oid;

  sai_entry.type = SAI_L2MC_ENTRY_TYPE_XG;

  sai_entry.destination.addr_family = SAI_IP_ADDR_FAMILY_IPV4;
  swss::IpAddress ipv4 = swss::IpAddress("224.1.1.1");
  sai_ip_address_t sai_ipv4_address;
  copy(sai_ipv4_address, ipv4);
  sai_entry.destination.addr.ip4 = sai_ipv4_address.addr.ip4;

  sai_entry.source.addr_family = SAI_IP_ADDR_FAMILY_IPV4;
  sai_entry.source.addr.ip4 = 0;

  std::vector<sai_attribute_t> attrs;

  attr.id = SAI_L2MC_ENTRY_ATTR_PACKET_ACTION;
  attr.value.s32 = SAI_PACKET_ACTION_FORWARD;
  attrs.push_back(attr);

  attr.id = SAI_L2MC_ENTRY_ATTR_OUTPUT_GROUP_ID;
  attr.value.oid = l2mc_group_oid;
  attrs.push_back(attr);

  status = sai_l2mc_api->create_l2mc_entry(&sai_entry, (uint32_t)attrs.size(),
                                           attrs.data());
  if (status != SAI_STATUS_SUCCESS) {
    std::stringstream ss;
    ss << "0x" << std::hex << l2mc_group_oid;
    LOG_ERROR_AND_RETURN(ReturnCode(status)
                         << "Failed to create L2 multicast entry for OID: "
                         << QuotedVar(ss.str()).c_str());
  }

  // Immediately remove the entry.  It was just needed to force creation of
  // the SAI L2 multicast group object.
  sai_status_t l2mc_entry_remove_status =
      sai_l2mc_api->remove_l2mc_entry(&sai_entry);
  // If this fails, we have to go critical.
  if (l2mc_entry_remove_status != SAI_STATUS_SUCCESS) {
    std::stringstream err_msg;
    err_msg << "Temporary L2 multicast entry could not be removed for "
            << "multicast group OID 0x" << std::hex << l2mc_group_oid;
    SWSS_LOG_ERROR("%s", err_msg.str().c_str());
    SWSS_RAISE_CRITICAL_STATE(err_msg.str());
    return l2mc_entry_remove_status;
  }

  return ReturnCode();
}

ReturnCode L3MulticastManager::addL2MulticastGroupEntry(
    P4MulticastGroupEntry& entry) {
  SWSS_LOG_ENTER();

  // Create the multicast group.
  sai_object_id_t mcast_group_oid = SAI_NULL_OBJECT_ID;
  RETURN_IF_ERROR(createL2MulticastGroup(entry, &mcast_group_oid));

  ReturnCode l2mc_entry_status = activateL2MulticastGroup(mcast_group_oid);

  // If we couldn't create the l2mc entry or we failed to remove it, back out
  // creation of the l2mc group.
  if (!l2mc_entry_status.ok()) {
    // Delete L2 multicast group just created
    auto sai_del_status =
        sai_l2mc_group_api->remove_l2mc_group(mcast_group_oid);
    if (sai_del_status != SAI_STATUS_SUCCESS) {
      // All kinds of bad.  The delete failed, and we have to leave a
      // dangling allocated multicast group.
      std::stringstream err_msg;
      err_msg << "Failed to create or remove L2 multicast entry, and we were "
              << "unable to backout creation of the multicast group "
              << QuotedVar(entry.multicast_group_id);
      SWSS_LOG_ERROR("%s", err_msg.str().c_str());
      SWSS_RAISE_CRITICAL_STATE(err_msg.str());
    }
    // Return original error.
    return l2mc_entry_status;
  }

  // Update internal book-keeping for new multicast group.
  m_p4OidMapper->setOID(SAI_OBJECT_TYPE_L2MC_GROUP, entry.multicast_group_id,
                        mcast_group_oid);

  // Next, create the group members.  If there's a failure, back out.
  // Instead of updating internal state as members are created, wait until all
  // members have been created to simplify back-out.
  std::unordered_map<std::string, sai_object_id_t> created_member_map;
  std::unordered_map<std::string, sai_object_id_t> member_bridge_port_map;
  for (auto& replica : entry.replicas) {
    sai_object_id_t bridge_port_oid = getBridgePortOid(replica);

    // Create the group member.
    sai_object_id_t mcast_group_member_oid;
    ReturnCode create_member_status = createL2MulticastGroupMember(
        replica, mcast_group_oid, bridge_port_oid, &mcast_group_member_oid);

    if (!create_member_status.ok()) {
      // Back out creation of members that were just created.
      for (auto& created_members : created_member_map) {
        sai_status_t member_delete_status =
            sai_l2mc_group_api->remove_l2mc_group_member(
                created_members.second);
        if (member_delete_status != SAI_STATUS_SUCCESS) {
          // All kinds of bad.  The delete failed, and we have to leave a
          // dangling allocated multicast group.  We are going to need outside
          // help to repair this.  Leave the create status as the original
          // failure code returned.
          std::stringstream err_msg;
          err_msg << "L2 multicast group member creation failed, and we were "
                  << "unable to backout creation of previous members.";
          SWSS_LOG_ERROR("%s", err_msg.str().c_str());
          SWSS_RAISE_CRITICAL_STATE(err_msg.str());
          // Cannot proceed to try to back out creation of group.
          return create_member_status;
        }
      }

      // Back out multicast group creation.
      ReturnCode backout_status =
          deleteL2MulticastGroup(entry.multicast_group_id, mcast_group_oid);

      if (!backout_status.ok()) {
        // All kinds of bad.  Since the delete failed, we should leave
        // the bookkeeping in place, but we are going to need outside help to
        // repair this.  Leave the create status as the failure code returned.
        std::stringstream err_msg;
        err_msg << "L2 multicast group member creation failed, and we were "
                << "unable to backout creation of the multicast group.";
        SWSS_LOG_ERROR("%s", err_msg.str().c_str());
        SWSS_RAISE_CRITICAL_STATE(err_msg.str());
      } else {
        // Back out multicast group state.
        m_p4OidMapper->eraseOID(SAI_OBJECT_TYPE_L2MC_GROUP,
                                entry.multicast_group_id);
      }
      // Stop trying to create replicas.  Return the first error.
      return create_member_status;
    }
    // We successfully created a group member.
    created_member_map[replica.key] = mcast_group_member_oid;
    member_bridge_port_map[replica.key] = bridge_port_oid;
    // We defer additional book-keeping until all replicas are created.
  }  // for replica

  // Finish with book keeping.

  // Update state for created group members.
  // Group members reference multicast_router_interface entries.
  for (auto& replica : entry.replicas) {
    const std::string router_interface_key =
        KeyGenerator::generateMulticastRouterInterfaceKey(replica.port,
                                                          replica.instance);
    m_p4OidMapper->increaseRefCount(SAI_OBJECT_TYPE_BRIDGE_PORT,
                                    router_interface_key);
  }
  for (auto& created_members : created_member_map) {
    m_p4OidMapper->setOID(SAI_OBJECT_TYPE_L2MC_GROUP_MEMBER,
                          created_members.first, created_members.second);
  }

  // Update internal state.
  m_multicastGroupEntryTable[entry.multicast_group_id] = entry;
  return ReturnCode();
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

    if (entry.replicas.size() == 0) {
      // We cannot create a group with no members, since there is no way to
      // determine if it is IP or L2 multicast.
      statuses[i] = ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
                    << "Multicast group " << QuotedVar(entry.multicast_group_id)
                    << "specified no replicas";
      break;
    }

    // To avoid needing to back-out later, confirm RIF or bridge port OIDs
    // exist up front.
    ReturnCodeOr<bool> is_ipmc_or = validateReplicas(entry);
    if (!is_ipmc_or.ok()) {
      statuses[i] = is_ipmc_or.status();
      break;
    }
    bool is_ipmc = *is_ipmc_or;
    if (is_ipmc) {
      statuses[i] = addIpMulticastGroupEntry(entry);
    } else {
      statuses[i] = addL2MulticastGroupEntry(entry);
    }
    if (!statuses[i].ok()) {
      break;
    }
  }  // for i
  return statuses;
}

ReturnCode L3MulticastManager::updateIpMulticastGroupEntry(
    P4MulticastGroupEntry& entry, P4MulticastGroupEntry* old_entry) {
  SWSS_LOG_ENTER();

  // Fetch the group OID.
  sai_object_id_t old_group_oid = SAI_NULL_OBJECT_ID;
  if (!m_p4OidMapper->getOID(SAI_OBJECT_TYPE_IPMC_GROUP,
                             entry.multicast_group_id, &old_group_oid)) {
    std::stringstream err_msg;
    err_msg << "Unable to fetch multicast group oid for group "
            << entry.multicast_group_id;
    SWSS_LOG_ERROR("%s", err_msg.str().c_str());
    SWSS_RAISE_CRITICAL_STATE(err_msg.str());
    return ReturnCode(StatusCode::SWSS_RC_INTERNAL) << err_msg.str();
  }

  std::vector<P4Replica> replicas_to_add;
  for (auto& replica : entry.replicas) {
    // New replica is not part of existing replicas.
    if (old_entry->replica_keys.find(replica.key) ==
        old_entry->replica_keys.end()) {
      replicas_to_add.push_back(replica);
    }
  }

  std::vector<P4Replica> replicas_to_delete;
  for (auto& replica : old_entry->replicas) {
    // Existing replica is not part of new replicas.
    if (entry.replica_keys.find(replica.key) == entry.replica_keys.end()) {
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
    replica_rif_map[replica.key] = old_rif_oid;

    // Fetch the member OID.
    sai_object_id_t old_group_member_oid = SAI_NULL_OBJECT_ID;
    m_p4OidMapper->getOID(SAI_OBJECT_TYPE_IPMC_GROUP_MEMBER, replica.key,
                          &old_group_member_oid);

    // Delete group member
    sai_status_t member_delete_status =
        sai_ipmc_group_api->remove_ipmc_group_member(old_group_member_oid);
    if (member_delete_status != SAI_STATUS_SUCCESS) {
      // Attempt to re-add deleted group members.
      ReturnCode restore_status =
          restoreDeletedGroupMembers(deleted_replicas, replica_rif_map,
                                     old_group_oid, replica.key, old_entry);
      if (!restore_status.ok()) {
        SWSS_LOG_ERROR("%s", restore_status.message().c_str());
        SWSS_RAISE_CRITICAL_STATE(restore_status.message());
      }
      // We still return the original failure when we successfully back
      // out changes.
      return member_delete_status;
    }
    // Update internal state to reflect successful delete.
    m_p4OidMapper->eraseOID(SAI_OBJECT_TYPE_IPMC_GROUP_MEMBER, replica.key);
    const std::string router_interface_key =
        KeyGenerator::generateMulticastRouterInterfaceKey(replica.port,
                                                          replica.instance);
    m_p4OidMapper->decreaseRefCount(SAI_OBJECT_TYPE_ROUTER_INTERFACE,
                                    router_interface_key);
    deleted_replicas.push_back(replica);
  }  // for replica (to delete)

  // Second add new replicas.
  std::vector<P4Replica> added_replicas;

  for (auto& replica : replicas_to_add) {
    // Fetch the RIF used by the member.
    sai_object_id_t new_rif_oid = getRifOid(replica);
    replica_rif_map[replica.key] = new_rif_oid;

    // Create the group member.
    sai_object_id_t mcast_group_member_oid;
    ReturnCode create_member_status = createMulticastGroupMember(
        replica, old_group_oid, new_rif_oid, &mcast_group_member_oid);

    if (!create_member_status.ok()) {
      // Backout members added.
      for (auto& added_replica : added_replicas) {
	sai_object_id_t added_member_oid = SAI_NULL_OBJECT_ID;
        m_p4OidMapper->getOID(SAI_OBJECT_TYPE_IPMC_GROUP_MEMBER,
                              added_replica.key, &added_member_oid);
        sai_status_t member_delete_status =
		sai_ipmc_group_api->remove_ipmc_group_member(added_member_oid);
        if (member_delete_status != SAI_STATUS_SUCCESS) {
          // All kinds of bad
          std::stringstream err_msg;
          err_msg << "Cannot revert to previous state, because added replica "
                  << QuotedVar(added_replica.key) << " cannot be deleted";
          SWSS_LOG_ERROR("%s", err_msg.str().c_str());
          SWSS_RAISE_CRITICAL_STATE(err_msg.str());
          return create_member_status;
        }
        // Update state based on successful removal.
        m_p4OidMapper->eraseOID(SAI_OBJECT_TYPE_IPMC_GROUP_MEMBER,
                                added_replica.key);
	const std::string added_router_interface_key =
            KeyGenerator::generateMulticastRouterInterfaceKey(
                added_replica.port, added_replica.instance);
	m_p4OidMapper->decreaseRefCount(SAI_OBJECT_TYPE_ROUTER_INTERFACE,
                                        added_router_interface_key);
      }

      // Attempt to re-add deleted group members.
      ReturnCode restore_status =
          restoreDeletedGroupMembers(deleted_replicas, replica_rif_map,
                                     old_group_oid, replica.key, old_entry);
      if (!restore_status.ok()) {
        SWSS_LOG_ERROR("%s", restore_status.message().c_str());
        SWSS_RAISE_CRITICAL_STATE(restore_status.message());
      }
      // We still return the original failure when we successfully back
      // out changes.
      return create_member_status;
    }

    // Update internal state to reflect successful add.
    m_p4OidMapper->setOID(SAI_OBJECT_TYPE_IPMC_GROUP_MEMBER, replica.key,
                          mcast_group_member_oid);
    const std::string router_interface_key =
        KeyGenerator::generateMulticastRouterInterfaceKey(replica.port,
                                                          replica.instance);
    m_p4OidMapper->increaseRefCount(SAI_OBJECT_TYPE_ROUTER_INTERFACE,
                                    router_interface_key);
    added_replicas.push_back(replica);

  }  // for replica (to add)

  // Final bookkeeping.
  // Since we updated the original entry in place, we need to replace the
  // replicas and metadata with the new state.
  old_entry->replicas.clear();
  old_entry->replicas.insert(old_entry->replicas.end(), entry.replicas.begin(),
                             entry.replicas.end());
  old_entry->replica_keys.clear();
  old_entry->replica_keys.insert(entry.replica_keys.begin(),
                                 entry.replica_keys.end());
  old_entry->controller_metadata = entry.controller_metadata;
  old_entry->multicast_metadata = entry.multicast_metadata;
  return ReturnCode();
}

ReturnCode L3MulticastManager::updateL2MulticastGroupEntry(
    P4MulticastGroupEntry& entry, P4MulticastGroupEntry* old_entry) {
  SWSS_LOG_ENTER();

  // Fetch the group OID.
  sai_object_id_t old_group_oid = SAI_NULL_OBJECT_ID;
  if (!m_p4OidMapper->getOID(SAI_OBJECT_TYPE_L2MC_GROUP,
                             entry.multicast_group_id, &old_group_oid)) {
    std::stringstream err_msg;
    err_msg << "Unable to fetch L2 multicast group oid for group "
            << entry.multicast_group_id;
    SWSS_LOG_ERROR("%s", err_msg.str().c_str());
    SWSS_RAISE_CRITICAL_STATE(err_msg.str());
    return ReturnCode(StatusCode::SWSS_RC_INTERNAL) << err_msg.str();
  }

  std::vector<P4Replica> replicas_to_add;
  for (auto& replica : entry.replicas) {
    // New replica is not part of existing replicas.
    if (old_entry->replica_keys.find(replica.key) ==
        old_entry->replica_keys.end()) {
      replicas_to_add.push_back(replica);
    }
  }

  std::vector<P4Replica> replicas_to_delete;
  for (auto& replica : old_entry->replicas) {
    // Existing replica is not part of new replicas.
    if (entry.replica_keys.find(replica.key) == entry.replica_keys.end()) {
      replicas_to_delete.push_back(replica);
    }
  }

  // Replicas in both old and new entries can be left untouched (no-op).

  // First, delete replicas.
  std::vector<P4Replica> deleted_replicas;
  std::unordered_map<std::string, sai_object_id_t> replica_bridge_port_map;

  for (auto& replica : replicas_to_delete) {
    // Fetch the bridge port used by the member.
    sai_object_id_t old_bridge_port_oid = getBridgePortOid(replica);
    replica_bridge_port_map[replica.key] = old_bridge_port_oid;

    // Fetch the member OID.
    sai_object_id_t old_group_member_oid = SAI_NULL_OBJECT_ID;
    m_p4OidMapper->getOID(SAI_OBJECT_TYPE_L2MC_GROUP_MEMBER, replica.key,
                          &old_group_member_oid);

    // Delete group member
    sai_status_t member_delete_status =
        sai_l2mc_group_api->remove_l2mc_group_member(old_group_member_oid);
    if (member_delete_status != SAI_STATUS_SUCCESS) {
      // Attempt to re-add deleted group members.
      ReturnCode restore_status = restoreDeletedL2GroupMembers(
          deleted_replicas, replica_bridge_port_map, old_group_oid, replica.key,
          old_entry);
      if (!restore_status.ok()) {
        SWSS_LOG_ERROR("%s", restore_status.message().c_str());
        SWSS_RAISE_CRITICAL_STATE(restore_status.message());
      }
      // We still return the original failure when we successfully back
      // out changes.
      return member_delete_status;
    }
    // Update internal state to reflect successful delete.
    m_p4OidMapper->eraseOID(SAI_OBJECT_TYPE_L2MC_GROUP_MEMBER, replica.key);
    const std::string router_interface_key =
        KeyGenerator::generateMulticastRouterInterfaceKey(replica.port,
                                                          replica.instance);
    m_p4OidMapper->decreaseRefCount(SAI_OBJECT_TYPE_BRIDGE_PORT,
                                    router_interface_key);
    deleted_replicas.push_back(replica);
  }  // for replica (to delete)

  // Second add new replicas.
  std::vector<P4Replica> added_replicas;

  for (auto& replica : replicas_to_add) {
    // Fetch the brige port used by the member.
    sai_object_id_t new_bridge_port_oid = getBridgePortOid(replica);
    replica_bridge_port_map[replica.key] = new_bridge_port_oid;

    // Create the group member.
    sai_object_id_t mcast_group_member_oid;
    ReturnCode create_member_status = createL2MulticastGroupMember(
        replica, old_group_oid, new_bridge_port_oid, &mcast_group_member_oid);

    if (!create_member_status.ok()) {
      // Backout members added.
      for (auto& added_replica : added_replicas) {
	sai_object_id_t added_member_oid = SAI_NULL_OBJECT_ID;
        m_p4OidMapper->getOID(SAI_OBJECT_TYPE_L2MC_GROUP_MEMBER,
                              added_replica.key, &added_member_oid);
        sai_status_t member_delete_status =
            sai_l2mc_group_api->remove_l2mc_group_member(added_member_oid);
        if (member_delete_status != SAI_STATUS_SUCCESS) {
          // All kinds of bad
          std::stringstream err_msg;
          err_msg << "Cannot revert to previous state, because added replica "
                  << QuotedVar(added_replica.key) << " cannot be deleted";
          SWSS_LOG_ERROR("%s", err_msg.str().c_str());
          SWSS_RAISE_CRITICAL_STATE(err_msg.str());
          return create_member_status;
        }
        // Update state based on successful removal.
        m_p4OidMapper->eraseOID(SAI_OBJECT_TYPE_L2MC_GROUP_MEMBER,
                                added_replica.key);
	const std::string added_router_interface_key =
            KeyGenerator::generateMulticastRouterInterfaceKey(
                added_replica.port, added_replica.instance);
	m_p4OidMapper->decreaseRefCount(SAI_OBJECT_TYPE_BRIDGE_PORT,
                                        added_router_interface_key);
      }

      // Attempt to re-add deleted group members.
      ReturnCode restore_status = restoreDeletedL2GroupMembers(
          deleted_replicas, replica_bridge_port_map, old_group_oid, replica.key,
          old_entry);
      if (!restore_status.ok()) {
        SWSS_LOG_ERROR("%s", restore_status.message().c_str());
        SWSS_RAISE_CRITICAL_STATE(restore_status.message());
      }
      // We still return the original failure when we successfully back
      // out changes.
      return create_member_status;
    }

    // Update internal state to reflect successful add.
    m_p4OidMapper->setOID(SAI_OBJECT_TYPE_L2MC_GROUP_MEMBER, replica.key,
                          mcast_group_member_oid);
    const std::string router_interface_key =
        KeyGenerator::generateMulticastRouterInterfaceKey(replica.port,
                                                          replica.instance);
     m_p4OidMapper->increaseRefCount(SAI_OBJECT_TYPE_BRIDGE_PORT,
                                    router_interface_key);
    added_replicas.push_back(replica);

  }  // for replica (to add)

  // Final bookkeeping.
  // Since we updated the original entry in place, we need to replace the
  // replicas and metadata with the new state.
  old_entry->replicas.clear();
  old_entry->replicas.insert(old_entry->replicas.end(), entry.replicas.begin(),
                             entry.replicas.end());
  old_entry->replica_keys.clear();
  old_entry->replica_keys.insert(entry.replica_keys.begin(),
                                 entry.replica_keys.end());
  old_entry->controller_metadata = entry.controller_metadata;
  old_entry->multicast_metadata = entry.multicast_metadata;
  return ReturnCode();
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
    auto *old_entry_ptr = getMulticastGroupEntry(entry.multicast_group_id);
    if (old_entry_ptr == nullptr) {
      statuses[i] = ReturnCode(StatusCode::SWSS_RC_UNKNOWN)
                    << "Multicast group entry is not known "
                    << QuotedVar(entry.multicast_group_id);
      break;
    }

    // To avoid needing to back-out later, confirm RIF or bridge port OIDs
    // exist up front.
    ReturnCodeOr<bool> is_ipmc_or = validateReplicas(entry);
    ReturnCodeOr<bool> is_ipmc_old_or = validateReplicas(*old_entry_ptr);
    if (!is_ipmc_or.ok()) {
      statuses[i] = is_ipmc_or.status();
      break;
    }
    if (!is_ipmc_old_or.ok()) {
      statuses[i] = is_ipmc_old_or.status();
      break;
    }
    bool is_ipmc = *is_ipmc_or;
    bool is_ipmc_old = *is_ipmc_old_or;
    // Check that the update operation does not switch between IP and L2.
    // This is not supported, because it requires changing the SAI group from/to
    // IPMC and L2 multicast group types.
    if (is_ipmc != is_ipmc_old) {
      statuses[i] = ReturnCode(StatusCode::SWSS_RC_UNIMPLEMENTED)
                    << "Updating multicast group "
                    << QuotedVar(entry.multicast_group_id)
                    << " cannot switch between IP and L2 type for its replicas";
      break;
    }

    if (is_ipmc) {
      statuses[i] = updateIpMulticastGroupEntry(entry, old_entry_ptr);
    } else {
      statuses[i] = updateL2MulticastGroupEntry(entry, old_entry_ptr);
    }
    if (!statuses[i].ok()) {
      break;
    }
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
        deleted_replica, group_oid, restore_rif_oid,
        &restore_group_member_oid);
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
    const std::string router_interface_key =
        KeyGenerator::generateMulticastRouterInterfaceKey(
            deleted_replica.port, deleted_replica.instance);
    m_p4OidMapper->increaseRefCount(SAI_OBJECT_TYPE_ROUTER_INTERFACE,
                                    router_interface_key);
  }
  return ReturnCode();
}

ReturnCode L3MulticastManager::restoreDeletedL2GroupMembers(
    const std::vector<P4Replica>& deleted_replicas,
    const std::unordered_map<std::string, sai_object_id_t>&
        replica_bridge_port_map,
    const sai_object_id_t group_oid, const std::string& error_message,
    P4MulticastGroupEntry* old_entry) {
  // Attempt to re-add deleted group members.
  for (auto& deleted_replica : deleted_replicas) {
    sai_object_id_t restore_bridge_port_oid =
        replica_bridge_port_map.at(deleted_replica.key);
    sai_object_id_t restore_group_member_oid = SAI_NULL_OBJECT_ID;
    auto create_status = createL2MulticastGroupMember(
        deleted_replica, group_oid, restore_bridge_port_oid,
        &restore_group_member_oid);
    if (!create_status.ok()) {
      // All kinds of bad.  We couldn't restore a L2 multicast group member,
      // which leaves us in an inconsistent state with what the controller
      // expects.  Leave the overall return code as original failure.
      return ReturnCode(StatusCode::SWSS_RC_INTERNAL)
             << "Unable to restore deleted L2 multicast group member  "
             << QuotedVar(deleted_replica.key)
             << " after group member delete failed on "
             << QuotedVar(error_message);
    }
    // If we successfully added the group member back, update internal
    // state.
    m_p4OidMapper->setOID(SAI_OBJECT_TYPE_L2MC_GROUP_MEMBER,
                          deleted_replica.key, restore_group_member_oid);
    const std::string router_interface_key =
        KeyGenerator::generateMulticastRouterInterfaceKey(
            deleted_replica.port, deleted_replica.instance);
    m_p4OidMapper->increaseRefCount(SAI_OBJECT_TYPE_BRIDGE_PORT,
                                    router_interface_key);
  }
  return ReturnCode();
}

ReturnCode L3MulticastManager::deleteIpMulticastGroupEntry(
    P4MulticastGroupEntry& entry) {
  SWSS_LOG_ENTER();
  // Before deleting the group, confirm there are no routes still using the
  // multicast group.
  uint32_t route_entry_ref_count = 1;
  if (!m_p4OidMapper->getRefCount(SAI_OBJECT_TYPE_IPMC_GROUP,
                                  entry.multicast_group_id,
                                  &route_entry_ref_count)) {
    std::stringstream err_msg;
    err_msg << "Unable to fetch reference count for multicast group "
            << QuotedVar(entry.multicast_group_id);
    SWSS_LOG_ERROR("%s", err_msg.str().c_str());
    SWSS_RAISE_CRITICAL_STATE(err_msg.str());
    return ReturnCode(StatusCode::SWSS_RC_INTERNAL) << err_msg.str();
  }
  if (route_entry_ref_count != 0) {
    return ReturnCode(StatusCode::SWSS_RC_IN_USE)
           << "Multicast group " << QuotedVar(entry.multicast_group_id)
           << " cannot be deleted because route entries are still "
           << "referencing it.";
  }

  // Fetch the group OID.
  // There's no need to check the return code of getOID, since getRefCount
  // above already checked that the key exists.
  sai_object_id_t old_group_oid = SAI_NULL_OBJECT_ID;
  m_p4OidMapper->getOID(SAI_OBJECT_TYPE_IPMC_GROUP, entry.multicast_group_id,
                        &old_group_oid);

  // Delete group members.  Do internal state-book keeping as go along, since
  // re-allocation does not necessarily result in the same OID.
  std::vector<P4Replica> deleted_replicas;
  std::unordered_map<std::string, sai_object_id_t> replica_rif_map;
  for (auto& replica : entry.replicas) {
    // Fetch the RIF used by the member.
    sai_object_id_t old_rif_oid = getRifOid(replica);
    replica_rif_map[replica.key] = old_rif_oid;

    // Fetch the member OID.
    sai_object_id_t old_group_member_oid = SAI_NULL_OBJECT_ID;
    if (!m_p4OidMapper->getOID(SAI_OBJECT_TYPE_IPMC_GROUP_MEMBER, replica.key,
                               &old_group_member_oid)) {
      std::stringstream err_msg;
      err_msg << "Cannot find oid associated with group member to delete "
              << QuotedVar(replica.key);
      SWSS_LOG_ERROR("%s", err_msg.str().c_str());
      SWSS_RAISE_CRITICAL_STATE(err_msg.str());
      return ReturnCode(StatusCode::SWSS_RC_INTERNAL) << err_msg.str();
    }

    // Delete group member
    sai_status_t member_delete_status =
        sai_ipmc_group_api->remove_ipmc_group_member(old_group_member_oid);
    if (member_delete_status != SAI_STATUS_SUCCESS) {
      // Attempt to re-add deleted group members.
      ReturnCode restore_status =
          restoreDeletedGroupMembers(deleted_replicas, replica_rif_map,
                                     old_group_oid, replica.key, &entry);
      if (!restore_status.ok()) {
        SWSS_LOG_ERROR("%s", restore_status.message().c_str());
        SWSS_RAISE_CRITICAL_STATE(restore_status.message());
      }
      // We still return the original failure when we successfully back
      // out changes.
      return member_delete_status;
    }

    // Update internal state to reflect successful delete.
    m_p4OidMapper->eraseOID(SAI_OBJECT_TYPE_IPMC_GROUP_MEMBER, replica.key);
    const std::string router_interface_key =
        KeyGenerator::generateMulticastRouterInterfaceKey(replica.port,
                                                          replica.instance);
    m_p4OidMapper->decreaseRefCount(SAI_OBJECT_TYPE_ROUTER_INTERFACE,
                                    router_interface_key);
    deleted_replicas.push_back(replica);
  }  // for replicas

  // Now delete the multicast group.
  sai_status_t group_delete_status =
      sai_ipmc_group_api->remove_ipmc_group(old_group_oid);
  if (group_delete_status != SAI_STATUS_SUCCESS) {
    SWSS_LOG_ERROR("Failed to delete multicast group %s",
                   QuotedVar(entry.multicast_group_id).c_str());
    // On group removal failure, attempt to put the group members back.
    ReturnCode restore_status = restoreDeletedGroupMembers(
        deleted_replicas, replica_rif_map, old_group_oid,
        entry.multicast_group_id, &entry);
    if (!restore_status.ok()) {
      SWSS_LOG_ERROR("%s", restore_status.message().c_str());
      SWSS_RAISE_CRITICAL_STATE(restore_status.message());
    }
    return group_delete_status;
  }

  // Do internal bookkeping to remove the multicast group.
  m_p4OidMapper->eraseOID(SAI_OBJECT_TYPE_IPMC_GROUP, entry.multicast_group_id);
  m_multicastGroupEntryTable.erase(entry.multicast_group_id);
  return ReturnCode();
}

ReturnCode L3MulticastManager::deleteL2MulticastGroupEntry(
    P4MulticastGroupEntry& entry) {
  SWSS_LOG_ENTER();

  // Before deleting the group, confirm there are no routes still using the
  // multicast group.
  uint32_t l2_group_ref_count = 1;
  if (!m_p4OidMapper->getRefCount(SAI_OBJECT_TYPE_L2MC_GROUP,
                                  entry.multicast_group_id,
                                  &l2_group_ref_count)) {
    std::stringstream err_msg;
    err_msg << "Unable to fetch reference count for L2 multicast group "
            << QuotedVar(entry.multicast_group_id);
    SWSS_LOG_ERROR("%s", err_msg.str().c_str());
    SWSS_RAISE_CRITICAL_STATE(err_msg.str());
    return ReturnCode(StatusCode::SWSS_RC_INTERNAL) << err_msg.str();
  }
  if (l2_group_ref_count != 0) {
    return ReturnCode(StatusCode::SWSS_RC_IN_USE)
           << "L2 multicast group " << QuotedVar(entry.multicast_group_id)
           << " cannot be deleted because entries are still referencing it.";
  }

  // Fetch the group OID.
  // There's no need to check the return code of getOID, since getRefCount
  // above already checked that the key exists.
  sai_object_id_t old_group_oid = SAI_NULL_OBJECT_ID;
  m_p4OidMapper->getOID(SAI_OBJECT_TYPE_L2MC_GROUP, entry.multicast_group_id,
                        &old_group_oid);

  // Delete group members.  Do internal state-book keeping as go along, since
  // re-allocation does not necessarily result in the same OID.
  std::vector<P4Replica> deleted_replicas;
  std::unordered_map<std::string, sai_object_id_t> replica_bridge_port_map;
  for (auto& replica : entry.replicas) {
    // Fetch the bridge port used by the member.
    sai_object_id_t old_bridge_port_oid = getBridgePortOid(replica);
    replica_bridge_port_map[replica.key] = old_bridge_port_oid;

    // Fetch the member OID.
    sai_object_id_t old_group_member_oid = SAI_NULL_OBJECT_ID;
    if (!m_p4OidMapper->getOID(SAI_OBJECT_TYPE_L2MC_GROUP_MEMBER, replica.key,
                               &old_group_member_oid)) {
      std::stringstream err_msg;
      err_msg << "Cannot find oid associated with L2 group member to delete "
              << QuotedVar(replica.key);
      SWSS_LOG_ERROR("%s", err_msg.str().c_str());
      SWSS_RAISE_CRITICAL_STATE(err_msg.str());
      return ReturnCode(StatusCode::SWSS_RC_INTERNAL) << err_msg.str();
    }

    // Delete group member
    sai_status_t member_delete_status =
        sai_l2mc_group_api->remove_l2mc_group_member(old_group_member_oid);
    if (member_delete_status != SAI_STATUS_SUCCESS) {
      // Attempt to re-add deleted group members.
      ReturnCode restore_status = restoreDeletedL2GroupMembers(
          deleted_replicas, replica_bridge_port_map, old_group_oid, replica.key,
          &entry);
      if (!restore_status.ok()) {
        SWSS_LOG_ERROR("%s", restore_status.message().c_str());
        SWSS_RAISE_CRITICAL_STATE(restore_status.message());
      }
      // We still return the original failure when we successfully back
      // out changes.
      return member_delete_status;
    }

    // Update internal state to reflect successful delete.
    m_p4OidMapper->eraseOID(SAI_OBJECT_TYPE_L2MC_GROUP_MEMBER, replica.key);
    const std::string router_interface_key =
        KeyGenerator::generateMulticastRouterInterfaceKey(replica.port,
                                                          replica.instance);
    m_p4OidMapper->decreaseRefCount(SAI_OBJECT_TYPE_BRIDGE_PORT,
                                    router_interface_key);
    deleted_replicas.push_back(replica);
  }  // for replicas

  // Now delete the L2 multicast group.
  sai_status_t group_delete_status =
      sai_l2mc_group_api->remove_l2mc_group(old_group_oid);
  if (group_delete_status != SAI_STATUS_SUCCESS) {
    SWSS_LOG_ERROR("Failed to delete L2 multicast group %s",
                   QuotedVar(entry.multicast_group_id).c_str());
    // On group removal failure, attempt to put the L2 group members back.
    ReturnCode restore_status = restoreDeletedL2GroupMembers(
        deleted_replicas, replica_bridge_port_map, old_group_oid,
        entry.multicast_group_id, &entry);
    if (!restore_status.ok()) {
      SWSS_LOG_ERROR("%s", restore_status.message().c_str());
      SWSS_RAISE_CRITICAL_STATE(restore_status.message());
    }
    return group_delete_status;
  }

  // Do internal bookkeping to remove the multicast group.
  m_p4OidMapper->eraseOID(SAI_OBJECT_TYPE_L2MC_GROUP, entry.multicast_group_id);
  m_multicastGroupEntryTable.erase(entry.multicast_group_id);
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
    auto *old_entry_ptr = getMulticastGroupEntry(entry.multicast_group_id);
    if (old_entry_ptr == nullptr) {
      statuses[i] = ReturnCode(StatusCode::SWSS_RC_UNKNOWN)
                    << "Multicast group entry is not known "
                    << QuotedVar(entry.multicast_group_id);
      break;
    }

    // To avoid needing to back-out later, confirm RIF or bridge port OIDs
    // exist up front.
    ReturnCodeOr<bool> is_ipmc_or = validateReplicas(*old_entry_ptr);
    if (!is_ipmc_or.ok()) {
      statuses[i] = is_ipmc_or.status();
      break;
    }
    bool is_ipmc = *is_ipmc_or;
    if (is_ipmc) {
      statuses[i] = deleteIpMulticastGroupEntry(*old_entry_ptr);
    } else {
      statuses[i] = deleteL2MulticastGroupEntry(*old_entry_ptr);
    }
    if (!statuses[i].ok()) {
      break;
    }
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
  if (multicast_router_interface_entry->dst_mac.to_string() !=
      app_db_entry.dst_mac.to_string()) {
    std::stringstream msg;
    msg << "Dst MAC " << QuotedVar(app_db_entry.dst_mac.to_string())
        << " does not match internal cache "
        << QuotedVar(multicast_router_interface_entry->dst_mac.to_string())
        << " in l3 multicast manager.";
    return msg.str();
  }
  if (multicast_router_interface_entry->vlan_id != app_db_entry.vlan_id) {
    std::stringstream msg;
    msg << "Vlan ID '" << app_db_entry.vlan_id
        << "' does not match internal cache '"
        << multicast_router_interface_entry->vlan_id
        << "' in l3 multicast manager.";
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

  if (multicast_router_interface_entry->action !=
          p4orch::kL2MulticastPassthrough &&
      multicast_router_interface_entry->action !=
          p4orch::kMulticastL2Passthrough) {
    sai_object_id_t rif_oid = getRifOid(multicast_router_interface_entry);
    std::string rif_str = m_p4OidMapper->verifyOIDMapping(
        SAI_OBJECT_TYPE_ROUTER_INTERFACE,
        multicast_router_interface_entry->multicast_router_interface_entry_key,
        rif_oid);
    if (!rif_str.empty()) {
      return rif_str;
    }
  }
  if (multicast_router_interface_entry->action == p4orch::kMulticastSetSrcMac ||
      multicast_router_interface_entry->action ==
          p4orch::kMulticastSetSrcMacAndVlanId ||
      multicast_router_interface_entry->action ==
          p4orch::kMulticastSetSrcMacAndDstMacAndVlanId ||
      multicast_router_interface_entry->action ==
          p4orch::kMulticastSetSrcMacAndPreserveIngressVlanId) {
    sai_object_id_t nh_oid = getNextHopOid(multicast_router_interface_entry);
    std::string nh_str = m_p4OidMapper->verifyOIDMapping(
        SAI_OBJECT_TYPE_NEXT_HOP,
        multicast_router_interface_entry->multicast_router_interface_entry_key,
        nh_oid);
    if (!nh_str.empty()) {
      return nh_str;
    }
  }

  return "";
}

std::string L3MulticastManager::verifyMulticastRouterInterfaceStateAsicDb(
    const P4MulticastRouterInterfaceEntry* multicast_router_interface_entry) {
  if (multicast_router_interface_entry->action ==
          p4orch::kL2MulticastPassthrough ||
      multicast_router_interface_entry->action ==
          p4orch::kMulticastL2Passthrough) {
    return verifyL2MulticastRouterInterfaceStateAsicDb(
        multicast_router_interface_entry);
  } else {
    return verifyL3MulticastRouterInterfaceStateAsicDb(
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
  sai_object_id_t rif_oid = getRifOid(multicast_router_interface_entry);

  swss::DBConnector db("ASIC_DB", 0);
  swss::Table table(&db, "ASIC_STATE");
  std::string key =
      sai_serialize_object_type(SAI_OBJECT_TYPE_ROUTER_INTERFACE) + ":" +
      sai_serialize_object_id(rif_oid);
  std::vector<swss::FieldValueTuple> values;
  if (!table.get(key, values)) {
    return std::string("ASIC DB key not found ") + key;
  }

  std::string rif_str =
      verifyAttrs(values, exp, std::vector<swss::FieldValueTuple>{},
                  /*allow_unknown=*/false);
  if (!rif_str.empty()) {
    return rif_str;
  }

  // Legacy action doesn't set a next hop.
  if (multicast_router_interface_entry->action == p4orch::kSetMulticastSrcMac) {
    return "";
  }

  auto nh_attrs =
      prepareNextHopSaiAttrs(*multicast_router_interface_entry, rif_oid);
  std::vector<swss::FieldValueTuple> nh_exp =
      saimeta::SaiAttributeList::serialize_attr_list(
          SAI_OBJECT_TYPE_NEXT_HOP, (uint32_t)nh_attrs.size(), nh_attrs.data(),
          /*countOnly=*/false);

  sai_object_id_t nh_oid = getNextHopOid(multicast_router_interface_entry);

  std::string nh_key = sai_serialize_object_type(SAI_OBJECT_TYPE_NEXT_HOP) +
                       ":" + sai_serialize_object_id(nh_oid);
  std::vector<swss::FieldValueTuple> nh_values;
  if (!table.get(nh_key, nh_values)) {
    return std::string("ASIC DB key not found ") + nh_key;
  }

  std::string nh_str =
      verifyAttrs(nh_values, nh_exp, std::vector<swss::FieldValueTuple>{},
                  /*allow_unknown=*/false);
  if (!nh_str.empty()) {
    return nh_str;
  }

  auto neigh_attrs =
      prepareNeighborEntrySaiAttrs(multicast_router_interface_entry->dst_mac);
  std::vector<swss::FieldValueTuple> neigh_exp =
      saimeta::SaiAttributeList::serialize_attr_list(
          SAI_OBJECT_TYPE_NEIGHBOR_ENTRY, (uint32_t)neigh_attrs.size(),
          neigh_attrs.data(),
          /*countOnly=*/false);
  sai_neighbor_entry_t neigh_entry = prepareSaiNeighborEntry(rif_oid);
  std::string neigh_key =
      sai_serialize_object_type(SAI_OBJECT_TYPE_NEIGHBOR_ENTRY) + ":" +
      sai_serialize_neighbor_entry(neigh_entry);
  std::vector<swss::FieldValueTuple> neigh_values;
  if (!table.get(neigh_key, neigh_values)) {
    return std::string("ASIC DB key not found ") + neigh_key;
  }

  return verifyAttrs(neigh_values, neigh_exp,
                     std::vector<swss::FieldValueTuple>{},
                     /*allow_unknown=*/false);
}

std::string L3MulticastManager::verifyL2MulticastRouterInterfaceStateAsicDb(
    const P4MulticastRouterInterfaceEntry* multicast_router_interface_entry) {
  auto attrs_or = prepareBridgePortSaiAttrs(*multicast_router_interface_entry);
  if (!attrs_or.ok()) {
    return std::string("Failed to get multicast router interface SAI attrs: ") +
           attrs_or.status().message();
  }
  std::vector<sai_attribute_t> attrs = *attrs_or;
  std::vector<swss::FieldValueTuple> exp =
      saimeta::SaiAttributeList::serialize_attr_list(
          SAI_OBJECT_TYPE_BRIDGE_PORT, (uint32_t)attrs.size(), attrs.data(),
          /*countOnly=*/false);

  sai_object_id_t bridge_port_oid =
      getBridgePortOid(multicast_router_interface_entry);

  swss::DBConnector db("ASIC_DB", 0);
  swss::Table table(&db, "ASIC_STATE");
  std::string key = sai_serialize_object_type(SAI_OBJECT_TYPE_BRIDGE_PORT) +
                    ":" + sai_serialize_object_id(bridge_port_oid);
  std::vector<swss::FieldValueTuple> values;
  if (!table.get(key, values)) {
    return std::string("ASIC DB key not found ") + key;
  }

  return verifyAttrs(values, exp, std::vector<swss::FieldValueTuple>{},
                     /*allow_unknown=*/false);
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
  if (app_db_entry.replicas.size() != multicast_group_entry->replicas.size() ||
      app_db_entry.replica_keys.size() !=
          multicast_group_entry->replica_keys.size()) {
    std::stringstream msg;
    msg << "Multicast group ID " << QuotedVar(app_db_entry.multicast_group_id)
        << " has a different number of replicas than internal cache.";
    return msg.str();
  }
  for (auto& replica : app_db_entry.replicas) {
    // Check we have the P4Replica object.
    if (multicast_group_entry->replica_keys.find(replica.key) ==
        multicast_group_entry->replica_keys.end()) {
      std::stringstream msg;
      msg << "Replica " << QuotedVar(replica.key)
          << " is missing from internal cache for multicast group "
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

  auto is_ipmc_or = validateReplicas(*multicast_group_entry);
  if (!is_ipmc_or.ok()) {
    std::stringstream msg;
    msg << "Unable to determine multicast group type for "
        << QuotedVar(multicast_group_entry->multicast_group_id);
    return msg.str();
  }

  return "";
}

std::string L3MulticastManager::verifyIpMulticastGroupStateAsicDb(
    const P4MulticastGroupEntry* multicast_group_entry) {
  // Confirm group settings.
  swss::DBConnector db("ASIC_DB", 0);
  swss::Table table(&db, "ASIC_STATE");

  sai_object_id_t ipmc_group_oid = SAI_NULL_OBJECT_ID;
  m_p4OidMapper->getOID(SAI_OBJECT_TYPE_IPMC_GROUP,
                        multicast_group_entry->multicast_group_id,
                        &ipmc_group_oid);
  std::string key = sai_serialize_object_type(SAI_OBJECT_TYPE_IPMC_GROUP) +
                    ":" + sai_serialize_object_id(ipmc_group_oid);
  std::vector<swss::FieldValueTuple> values;
  if (!table.get(key, values)) {
    return std::string("ASIC DB key not found ") + key;
  }
  // There are no IPMC group attributes to verify.  The attributes that do
  // exist are read-only attributes related to how many group members there are.
  // We check group members and their attributes below.

  // Confirm group member settings.
  for (auto& replica : multicast_group_entry->replicas) {
    // Confirm have RIF for each replica.
    sai_object_id_t rif_oid = getRifOid(replica);

    sai_object_id_t group_member_oid = SAI_NULL_OBJECT_ID;
    m_p4OidMapper->getOID(SAI_OBJECT_TYPE_IPMC_GROUP_MEMBER, replica.key,
                          &group_member_oid);

    // Ok to be null, since some actions do not allocate a next hop.
    sai_object_id_t next_hop_oid = getNextHopOid(replica);

    auto member_attrs = prepareMulticastGroupMemberSaiAttrs(
        ipmc_group_oid, rif_oid, next_hop_oid);
    std::vector<swss::FieldValueTuple> exp =
        saimeta::SaiAttributeList::serialize_attr_list(
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

std::string L3MulticastManager::verifyL2MulticastGroupStateAsicDb(
    const P4MulticastGroupEntry* multicast_group_entry) {
  // Confirm group settings.
  swss::DBConnector db("ASIC_DB", 0);
  swss::Table table(&db, "ASIC_STATE");

  sai_object_id_t l2mc_group_oid = SAI_NULL_OBJECT_ID;
  m_p4OidMapper->getOID(SAI_OBJECT_TYPE_L2MC_GROUP,
                        multicast_group_entry->multicast_group_id,
                        &l2mc_group_oid);
  std::string key = sai_serialize_object_type(SAI_OBJECT_TYPE_L2MC_GROUP) +
                    ":" + sai_serialize_object_id(l2mc_group_oid);
  std::vector<swss::FieldValueTuple> values;
  if (!table.get(key, values)) {
    return std::string("ASIC DB key not found ") + key;
  }
  // There are no L2MC group attributes to verify.  The attributes that do
  // exist are read-only attributes related to how many group members there are.
  // We check group members and their attributes below.

  // Confirm group member settings.
  for (auto& replica : multicast_group_entry->replicas) {
    // Confirm have RIF for each replica.
    sai_object_id_t bridge_port_oid = getBridgePortOid(replica);

    sai_object_id_t group_member_oid = SAI_NULL_OBJECT_ID;
    m_p4OidMapper->getOID(SAI_OBJECT_TYPE_L2MC_GROUP_MEMBER, replica.key,
                          &group_member_oid);

    auto member_attrs =
        prepareL2MulticastGroupMemberSaiAttrs(l2mc_group_oid, bridge_port_oid);
    std::vector<swss::FieldValueTuple> exp =
        saimeta::SaiAttributeList::serialize_attr_list(
            SAI_OBJECT_TYPE_L2MC_GROUP_MEMBER, (uint32_t)member_attrs.size(),
            member_attrs.data(), /*countOnly=*/false);
    key = sai_serialize_object_type(SAI_OBJECT_TYPE_L2MC_GROUP_MEMBER) + ":" +
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

std::string L3MulticastManager::verifyMulticastGroupStateAsicDb(
    const P4MulticastGroupEntry* multicast_group_entry) {
  auto is_ipmc_or = validateReplicas(*multicast_group_entry);
  if (!is_ipmc_or.ok()) {
    std::stringstream msg;
    msg << "Unable to determine multicast group type for "
        << QuotedVar(multicast_group_entry->multicast_group_id);
    return msg.str();
  }
  bool is_ipmc = *is_ipmc_or;
  if (is_ipmc) {
    return verifyIpMulticastGroupStateAsicDb(multicast_group_entry);
  } else {
    return verifyL2MulticastGroupStateAsicDb(multicast_group_entry);
  }
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
  sai_object_id_t rif_oid = SAI_NULL_OBJECT_ID;
  m_p4OidMapper->getOID(
      SAI_OBJECT_TYPE_ROUTER_INTERFACE,
      multicast_router_interface_entry->multicast_router_interface_entry_key,
      &rif_oid);
  return rif_oid;
}

sai_object_id_t L3MulticastManager::getNextHopOid(
    const P4MulticastRouterInterfaceEntry* multicast_router_interface_entry) {
  sai_object_id_t next_hop_oid = SAI_NULL_OBJECT_ID;
  m_p4OidMapper->getOID(
      SAI_OBJECT_TYPE_NEXT_HOP,
      multicast_router_interface_entry->multicast_router_interface_entry_key,
      &next_hop_oid);
  return next_hop_oid;
}

// A bridge port is associated with an egress port.
sai_object_id_t L3MulticastManager::getBridgePortOid(
    const P4MulticastRouterInterfaceEntry* multicast_router_interface_entry) {
  sai_object_id_t bridge_port_oid = SAI_NULL_OBJECT_ID;
  m_p4OidMapper->getOID(
      SAI_OBJECT_TYPE_BRIDGE_PORT,
      multicast_router_interface_entry->multicast_router_interface_entry_key,
      &bridge_port_oid);
  return bridge_port_oid;
}

// A RIF is associated with an egress port and Ethernet src mac value.
sai_object_id_t L3MulticastManager::getRifOid(const P4Replica& replica) {

  // Get router interface entry for out port and egress instance.
  const std::string router_interface_key =
      KeyGenerator::generateMulticastRouterInterfaceKey(
          replica.port, replica.instance);
  auto* router_interface_entry_ptr =
      getMulticastRouterInterfaceEntry(router_interface_key);
  if (router_interface_entry_ptr == nullptr) {
    return SAI_NULL_OBJECT_ID;
  }
  // Use that to generate RIF key.
  std::string rif_key = KeyGenerator::generateMulticastRouterInterfaceKey(
      router_interface_entry_ptr->multicast_replica_port,
      router_interface_entry_ptr->multicast_replica_instance);
  sai_object_id_t rif_oid = SAI_NULL_OBJECT_ID;
  m_p4OidMapper->getOID(SAI_OBJECT_TYPE_ROUTER_INTERFACE, rif_key, &rif_oid);
  return rif_oid;
}

sai_object_id_t L3MulticastManager::getNextHopOid(const P4Replica& replica) {
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
  std::string rif_key = KeyGenerator::generateMulticastRouterInterfaceKey(
      router_interface_entry_ptr->multicast_replica_port,
      router_interface_entry_ptr->multicast_replica_instance);
  sai_object_id_t next_hop_oid = SAI_NULL_OBJECT_ID;
  m_p4OidMapper->getOID(SAI_OBJECT_TYPE_NEXT_HOP, rif_key, &next_hop_oid);
  return next_hop_oid;
}

// A Bridge port is associated with an egress port.
sai_object_id_t L3MulticastManager::getBridgePortOid(const P4Replica& replica) {
  const std::string router_interface_key =
      KeyGenerator::generateMulticastRouterInterfaceKey(replica.port,
                                                        replica.instance);
  sai_object_id_t bridge_port_oid = SAI_NULL_OBJECT_ID;
  m_p4OidMapper->getOID(SAI_OBJECT_TYPE_BRIDGE_PORT, router_interface_key,
                        &bridge_port_oid);
  return bridge_port_oid;
}

}  // namespace p4orch
