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
