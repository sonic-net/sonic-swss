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
  // Have it return success to avoid problems in p4orch unit tests.
  return ReturnCode(StatusCode::SWSS_RC_SUCCESS)
         << "L3MulticastManager::drain is not implemented";
}

ReturnCodeOr<P4MulticastRouterInterfaceEntry>
L3MulticastManager::deserializeMulticastRouterInterfaceEntry(
    const std::string& key,
    const std::vector<swss::FieldValueTuple>& attributes,
    const std::string& table_name) {
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
               << table_name;
      }
    } else if (field == prependParamField(p4orch::kSrcMac)) {
      router_interface_entry.src_mac = swss::MacAddress(value);
    } else if (field == prependParamField(p4orch::kMulticastMetadata)) {
      router_interface_entry.multicast_metadata = value;
    } else if (field != p4orch::kControllerMetadata) {
      return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
             << "Unexpected field " << QuotedVar(field) << " in " << table_name;
    }
  }
  return router_interface_entry;
}

ReturnCodeOr<P4MulticastReplicationEntry>
L3MulticastManager::deserializeMulticastReplicationEntry(
    const std::string& key,
    const std::vector<swss::FieldValueTuple>& attributes,
    const std::string& table_name) {
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
             << "Unexpected field " << QuotedVar(field) << " in " << table_name;
    }
  }
  return replication_entry;
}

void L3MulticastManager::drainWithNotExecuted() {
  drainMgmtWithNotExecuted(m_entries, m_publisher);
}

std::string L3MulticastManager::verifyState(
    const std::string& key, const std::vector<swss::FieldValueTuple>& tuple) {
  return "L3MulticastManager::verifyState is not implemented";
}

/*
std::string L3MulticastManager::verifyMulticastRouterInterfaceState(
    const std::string& key,
    const std::vector<swss::FieldValueTuple>& tuple) {
  return "L3MulticastManager::verifyMulticastRouterInterfaceState is not "
         "implemented";
}

std::string L3MulticastManager::verifyMulticastReplicationState(
    const std::string& key,
    const std::vector<swss::FieldValueTuple>& tuple) {
  return "L3MulticastManager::verifyMulticastReplicationState is not "
         "implemented";
}
*/

ReturnCode L3MulticastManager::validateMulticastRouterInterfaceEntry(
    const P4MulticastRouterInterfaceEntry& multicast_router_interface_entry,
    const std::string& operation) {
  return ReturnCode(StatusCode::SWSS_RC_UNIMPLEMENTED)
         << "L3MulticastManager::verifyMulticastRouterInterfaceState is not "
         << "implemented";
}

ReturnCode L3MulticastManager::validateMulticastReplicationEntry(
    const P4MulticastReplicationEntry& multicast_replication_entry,
    const std::string& operation) {
  return ReturnCode(StatusCode::SWSS_RC_UNIMPLEMENTED)
         << "L3MulticastManager::validateMulticastReplicationEntry is not "
         << "implemented";
}

ReturnCode L3MulticastManager::processMulticastRouterInterfaceEntries(
    std::vector<P4MulticastRouterInterfaceEntry>& entries,
    const std::deque<swss::KeyOpFieldsValuesTuple>& tuple_list,
    const std::string& op, bool update) {
  return ReturnCode(StatusCode::SWSS_RC_UNIMPLEMENTED)
         << "L3MulticastManager::processMulticastRouterInterfaceEntries is not "
         << "implemented";
}

ReturnCode L3MulticastManager::createRouterInterface(
    const std::string& rif_key, P4MulticastRouterInterfaceEntry& entry,
    sai_object_id_t* rif_oid) {
  return ReturnCode(StatusCode::SWSS_RC_UNIMPLEMENTED)
         << "L3MulticastManager::createRouterInterface is not implemented";
}

ReturnCode L3MulticastManager::deleteRouterInterface(const std::string& rif_key,
                                                     sai_object_id_t rif_oid) {
  return ReturnCode(StatusCode::SWSS_RC_UNIMPLEMENTED)
         << "L3MulticastManager::deleteRouterInterface is not implemented";
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
  std::vector<ReturnCode> rv;
  rv.push_back(ReturnCode(StatusCode::SWSS_RC_UNIMPLEMENTED)
               << "L3MulticastManager::addMulticastRouterInterfaceEntries is "
               << "not implemented");
  return rv;
}

std::vector<ReturnCode>
L3MulticastManager::updateMulticastRouterInterfaceEntries(
    std::vector<P4MulticastRouterInterfaceEntry>& entries) {
  std::vector<ReturnCode> rv;
  rv.push_back(ReturnCode(StatusCode::SWSS_RC_UNIMPLEMENTED)
               << "L3MulticastManager::updateMulticastRouterInterfaceEntries "
               << "is not implemented");
  return rv;
}

std::vector<ReturnCode>
L3MulticastManager::deleteMulticastRouterInterfaceEntries(
    const std::vector<P4MulticastRouterInterfaceEntry>& entries) {
  std::vector<ReturnCode> rv;
  rv.push_back(ReturnCode(StatusCode::SWSS_RC_UNIMPLEMENTED)
               << "L3MulticastManager::deleteMulticastRouterInterfaceEntries "
               << "is not implemented");
  return rv;
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
  return "L3MulticastManager::verifyMulticastRouterInterfaceStateCache is "
         "not implemented";
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
