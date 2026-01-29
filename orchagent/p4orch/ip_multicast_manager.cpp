#include "p4orch/ip_multicast_manager.h"

#include <memory>
#include <nlohmann/json.hpp>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "SaiAttributeList.h"
#include "converter.h"
#include "crmorch.h"
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
extern sai_ipmc_api_t* sai_ipmc_api;
extern sai_router_interface_api_t* sai_router_intfs_api;
extern sai_rpf_group_api_t* sai_rpf_group_api;

extern CrmOrch* gCrmOrch;
extern PortsOrch* gPortsOrch;

namespace p4orch {

namespace {

constexpr char* kRifMemberMacAddress = "00:00:00:00:00:01";

void fillStatusArrayWithNotExecuted(std::vector<ReturnCode>& array,
                                    size_t startIndex) {
  for (size_t i = startIndex; i < array.size(); ++i) {
    array[i] = ReturnCode(StatusCode::SWSS_RC_NOT_EXECUTED);
  }
}

std::vector<sai_attribute_t> prepareIpmcSaiAttrs(
    const sai_object_id_t multicast_group_oid,
    const sai_object_id_t rpf_group_oid) {
  std::vector<sai_attribute_t> attrs;
  sai_attribute_t attr;

  attr.id = SAI_IPMC_ENTRY_ATTR_PACKET_ACTION;
  attr.value.s32 = SAI_PACKET_ACTION_FORWARD;
  attrs.push_back(attr);

  attr.id = SAI_IPMC_ENTRY_ATTR_OUTPUT_GROUP_ID;
  attr.value.oid = multicast_group_oid;
  attrs.push_back(attr);

  // We have nothing to set this to, but it is a mandatory attribute for
  // entry creation.
  attr.id = SAI_IPMC_ENTRY_ATTR_RPF_GROUP_ID;
  attr.value.oid = rpf_group_oid;
  attrs.push_back(attr);

  // TODO: Add with counter support.
  // attr.id = SAI_IPMC_ENTRY_ATTR_COUNTER_ID;
  // attr.value.oid = group_counter_oid;
  // attrs.push_back(attr);

  return attrs;
}

}  // namespace

IpMulticastManager::IpMulticastManager(P4OidMapper* mapper, VRFOrch* vrfOrch,
                                       ResponsePublisherInterface* publisher)
    : m_p4OidMapper(mapper), m_vrfOrch(vrfOrch) {
  SWSS_LOG_ENTER();
  assert(publisher != nullptr);
  m_publisher = publisher;
}

ReturnCode IpMulticastManager::getSaiObject(const std::string& json_key,
                                            sai_object_type_t& object_type,
                                            std::string& object_key) {
  return StatusCode::SWSS_RC_UNIMPLEMENTED;
}

void IpMulticastManager::enqueue(const std::string& table_name,
                                 const swss::KeyOpFieldsValuesTuple& entry) {
  m_entries.push_back(entry);
}

ReturnCode IpMulticastManager::drain() {
  SWSS_LOG_ENTER();
  return ReturnCode(StatusCode::SWSS_RC_SUCCESS)
         << "IpMulticastManager::drain is not implemented yet";
}

void IpMulticastManager::drainWithNotExecuted() {
  drainMgmtWithNotExecuted(m_entries, m_publisher);
}

std::string IpMulticastManager::verifyState(
    const std::string& key, const std::vector<swss::FieldValueTuple>& tuples) {
  SWSS_LOG_ENTER();
  return "IpMulticastManager::verifyState is not implemented yet";
}

ReturnCodeOr<P4IpMulticastEntry>
IpMulticastManager::deserializeIpMulticastEntry(
    const std::string& key,
    const std::vector<swss::FieldValueTuple>& attributes,
    const std::string& table_name) {
  SWSS_LOG_ENTER();
  P4IpMulticastEntry ip_multicast_entry = {};
  try {
    nlohmann::json j = nlohmann::json::parse(key);
    ip_multicast_entry.vrf_id = j[prependMatchField(p4orch::kVrfId)];

    std::string ip_dst;
    if (table_name == APP_P4RT_IPV4_MULTICAST_TABLE_NAME) {
      if (j.find(prependMatchField(p4orch::kIpv4Dst)) != j.end()) {
        ip_dst = j[prependMatchField(p4orch::kIpv4Dst)];
      }
    } else {
      if (j.find(prependMatchField(p4orch::kIpv6Dst)) != j.end()) {
        ip_dst = j[prependMatchField(p4orch::kIpv6Dst)];
      }
    }
    try {
      ip_multicast_entry.ip_dst = swss::IpAddress(ip_dst);
    } catch (std::exception& ex) {
      return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
             << "Invalid IP address " << QuotedVar(ip_dst);
    }
  } catch (std::exception& ex) {
    return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
           << "Failed to deserialize IP multicast table key";
  }

  ip_multicast_entry.ip_multicast_entry_key =
      KeyGenerator::generateIpMulticastKey(ip_multicast_entry.vrf_id,
                                           ip_multicast_entry.ip_dst);
  for (const auto& it : attributes) {
    const auto& field = fvField(it);
    const auto& value = fvValue(it);
    if (field == p4orch::kAction) {
      ip_multicast_entry.action = value;
    } else if (field == prependParamField(p4orch::kMulticastGroupId)) {
      ip_multicast_entry.multicast_group_id = value;
    } else if (field == p4orch::kControllerMetadata) {
      ip_multicast_entry.controller_metadata = value;
    } else {
      return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
             << "Unexpected field " << QuotedVar(field) << " in " << table_name;
    }
  }
  return ip_multicast_entry;
}

P4IpMulticastEntry* IpMulticastManager::getIpMulticastEntry(
    const std::string& ip_multicast_entry_key) {
  SWSS_LOG_ENTER();
  if (m_ipMulticastTable.find(ip_multicast_entry_key) ==
      m_ipMulticastTable.end()) {
    return nullptr;
  }
  return &m_ipMulticastTable[ip_multicast_entry_key];
}

ReturnCode IpMulticastManager::createRouterInterfaceForDefaultRpfGroupMember() {
  SWSS_LOG_ENTER();
  rif_for_rpf_group_member_oid_ = SAI_NULL_OBJECT_ID;

  std::vector<sai_attribute_t> attrs;
  sai_attribute_t attr;

  // Map all P4 router interfaces to default VRF as virtual router is mandatory
  // parameter for creation of router interfaces in SAI.
  attr.id = SAI_ROUTER_INTERFACE_ATTR_VIRTUAL_ROUTER_ID;
  attr.value.oid = gVirtualRouterId;
  attrs.push_back(attr);

  // Find an available port.
  auto& all_ports_map = gPortsOrch->getAllPorts();
  Port* p = nullptr;
  for (auto& kv : all_ports_map) {
    if (kv.second.m_type == Port::PHY) {
      p = &kv.second;
      break;
    }
  }

  if (p == nullptr) {
    LOG_ERROR_AND_RETURN(ReturnCode(StatusCode::SWSS_RC_UNAVAIL)
                         << "Unable to find port for RPF group member");
  }

  attr.id = SAI_ROUTER_INTERFACE_ATTR_TYPE;
  attr.value.s32 = SAI_ROUTER_INTERFACE_TYPE_PORT;
  attrs.push_back(attr);

  attr.id = SAI_ROUTER_INTERFACE_ATTR_PORT_ID;
  attr.value.oid = p->m_port_id;
  attrs.push_back(attr);

  attr.id = SAI_ROUTER_INTERFACE_ATTR_MTU;
  attr.value.u32 = p->m_mtu;
  attrs.push_back(attr);

  attr.id = SAI_ROUTER_INTERFACE_ATTR_SRC_MAC_ADDRESS;
  memcpy(attr.value.mac, swss::MacAddress(kRifMemberMacAddress).getMac(),
         sizeof(sai_mac_t));
  attrs.push_back(attr);

  attr.id = SAI_ROUTER_INTERFACE_ATTR_V4_MCAST_ENABLE;
  attr.value.booldata = true;
  attrs.push_back(attr);

  attr.id = SAI_ROUTER_INTERFACE_ATTR_V6_MCAST_ENABLE;
  attr.value.booldata = true;
  attrs.push_back(attr);

  sai_status_t status = sai_router_intfs_api->create_router_interface(
      &rif_for_rpf_group_member_oid_, gSwitchId, (uint32_t)attrs.size(),
      attrs.data());

  if (status != SAI_STATUS_SUCCESS) {
    LOG_ERROR_AND_RETURN(ReturnCode(status)
                         << "Unable to create RIF for group member prior to "
                         << "creating IPMC entries");
  }
  return ReturnCode();
}

ReturnCode IpMulticastManager::createDefaultRpfGroupMember() {
  SWSS_LOG_ENTER();
  unused_rpf_group_member_oid_ = SAI_NULL_OBJECT_ID;

  std::vector<sai_attribute_t> attrs;
  sai_attribute_t attr;

  attr.id = SAI_RPF_GROUP_MEMBER_ATTR_RPF_GROUP_ID;
  attr.value.oid = ipmc_rpf_group_oid_;
  attrs.push_back(attr);

  attr.id = SAI_RPF_GROUP_MEMBER_ATTR_RPF_INTERFACE_ID;
  attr.value.oid = rif_for_rpf_group_member_oid_;
  attrs.push_back(attr);

  sai_status_t status = sai_rpf_group_api->create_rpf_group_member(
      &unused_rpf_group_member_oid_, gSwitchId, (uint32_t)attrs.size(),
      attrs.data());

  if (status != SAI_STATUS_SUCCESS) {
    LOG_ERROR_AND_RETURN(ReturnCode(status)
                         << "Unable to create RPF group member prior to "
                         << "creating IPMC entries");
  }
  return ReturnCode();
}

ReturnCode IpMulticastManager::createDefaultRpfGroup() {
  SWSS_LOG_ENTER();

  // Instead of backing out previous object creation if there is a failure,
  // allow this function to be called more than once.  This requires us to
  // check which objects have been created.

  if (ipmc_rpf_group_oid_ == SAI_NULL_OBJECT_ID) {
    ipmc_rpf_group_oid_ = SAI_NULL_OBJECT_ID;
    std::vector<sai_attribute_t> attrs;
    // No attributes are needed for RPF group creation.
    sai_status_t status = sai_rpf_group_api->create_rpf_group(
        &ipmc_rpf_group_oid_, gSwitchId, (uint32_t)attrs.size(), attrs.data());

    if (status != SAI_STATUS_SUCCESS) {
      LOG_ERROR_AND_RETURN(ReturnCode(status)
                           << "Unable to create RPF group prior to creating"
                           << "IPMC entries");
    }
  }

  // We need to have at least one RPF group member, which
  // requires us to allocate a RIF.
  if (rif_for_rpf_group_member_oid_ == SAI_NULL_OBJECT_ID) {
    ReturnCode status = createRouterInterfaceForDefaultRpfGroupMember();
    if (!status.ok()) {
      return status;
    }
  }

  if (unused_rpf_group_member_oid_ == SAI_NULL_OBJECT_ID) {
    ReturnCode status = createDefaultRpfGroupMember();
    if (!status.ok()) {
      return status;
    }
  }

  return ReturnCode();
}

sai_ipmc_entry_t IpMulticastManager::prepareSaiIpmcEntry(
    const P4IpMulticastEntry& ip_multicast_entry) const {
  sai_ipmc_entry_t sai_entry;
  sai_entry.switch_id = gSwitchId;
  sai_entry.vr_id = m_vrfOrch->getVRFid(ip_multicast_entry.vrf_id);
  sai_entry.type = SAI_IPMC_ENTRY_TYPE_XG;

  sai_ip_address_t sai_address;
  copy(sai_address, ip_multicast_entry.ip_dst);
  if (sai_address.addr_family == SAI_IP_ADDR_FAMILY_IPV4) {
    sai_entry.destination.addr_family = SAI_IP_ADDR_FAMILY_IPV4;
    sai_entry.destination.addr.ip4 = sai_address.addr.ip4;
    sai_entry.source.addr_family = SAI_IP_ADDR_FAMILY_IPV4;
    sai_entry.source.addr.ip4 = 0;
  } else {
    sai_entry.destination.addr_family = SAI_IP_ADDR_FAMILY_IPV6;
    memcpy(&sai_entry.destination.addr.ip6, &sai_address.addr.ip6,
           sizeof(sai_ip6_t));
    sai_entry.source.addr_family = SAI_IP_ADDR_FAMILY_IPV6;
    memset(&sai_entry.source.addr.ip6, 0, sizeof(sai_ip6_t));
  }
  return sai_entry;
}

std::vector<ReturnCode> IpMulticastManager::createIpMulticastEntries(
    const std::vector<P4IpMulticastEntry>& ip_multicast_entries) {
  SWSS_LOG_ENTER();
  std::vector<ReturnCode> statuses(ip_multicast_entries.size());
  fillStatusArrayWithNotExecuted(statuses, 0);

  // Before the first entry add, we have to create a RPF group.
  // Ideally, the RPF group would be empty, there has
  // to be at least one RPF group member.
  if (ip_multicast_entries.size() > 0 &&
      (ipmc_rpf_group_oid_ == SAI_NULL_OBJECT_ID ||
       unused_rpf_group_member_oid_ == SAI_NULL_OBJECT_ID ||
       rif_for_rpf_group_member_oid_ == SAI_NULL_OBJECT_ID)) {
    ReturnCode status = createDefaultRpfGroup();
    if (!status.ok()) {
      statuses[0] = status;
      return statuses;
    }
    SWSS_LOG_INFO("Successfully created default RPF group with oid: %ld",
                  ipmc_rpf_group_oid_);
  }

  for (size_t i = 0; i < ip_multicast_entries.size(); ++i) {
    const auto& ip_multicast_entry = ip_multicast_entries[i];

    sai_ipmc_entry_t sai_entry = prepareSaiIpmcEntry(ip_multicast_entry);

    // Fetch the multicast group OID.
    sai_object_id_t group_oid = SAI_NULL_OBJECT_ID;
    if (!m_p4OidMapper->getOID(SAI_OBJECT_TYPE_IPMC_GROUP,
                               ip_multicast_entry.multicast_group_id,
                               &group_oid)) {
      statuses[i] = ReturnCode(StatusCode::SWSS_RC_NOT_FOUND)
                    << "Multicast group ID "
                    << QuotedVar(ip_multicast_entry.multicast_group_id)
                    << " has not been created yet.";
      break;
    }

    std::vector<sai_attribute_t> attrs =
        prepareIpmcSaiAttrs(group_oid, ipmc_rpf_group_oid_);

    statuses[i] = sai_ipmc_api->create_ipmc_entry(
        &sai_entry, (uint32_t)attrs.size(), attrs.data());
    if (statuses[i] != SAI_STATUS_SUCCESS) {
      break;
    }

    // Bookkeeping
    m_ipMulticastTable[ip_multicast_entry.ip_multicast_entry_key] =
        ip_multicast_entry;
    m_ipMulticastTable[ip_multicast_entry.ip_multicast_entry_key]
        .sai_ipmc_entry = sai_entry;
    m_p4OidMapper->setDummyOID(SAI_OBJECT_TYPE_IPMC_ENTRY,
                               ip_multicast_entry.ip_multicast_entry_key);
    gCrmOrch->incCrmResUsedCounter(CrmResourceType::CRM_IPMC_ENTRY);
    m_vrfOrch->increaseVrfRefCount(ip_multicast_entry.vrf_id);
    m_p4OidMapper->increaseRefCount(SAI_OBJECT_TYPE_IPMC_GROUP,
                                    ip_multicast_entry.multicast_group_id);
    statuses[i] = ReturnCode();
  }
  return statuses;
}

std::vector<ReturnCode> IpMulticastManager::updateIpMulticastEntries(
    const std::vector<P4IpMulticastEntry>& ip_multicast_entries) {
  SWSS_LOG_ENTER();
  std::vector<ReturnCode> statuses(ip_multicast_entries.size());
  fillStatusArrayWithNotExecuted(statuses, 0);

  for (size_t i = 0; i < ip_multicast_entries.size(); ++i) {
    const auto& ip_multicast_entry = ip_multicast_entries[i];
    auto* old_ip_multicast_entry_ptr =
        getIpMulticastEntry(ip_multicast_entry.ip_multicast_entry_key);

    if (old_ip_multicast_entry_ptr == nullptr) {
      statuses[i] = ReturnCode(StatusCode::SWSS_RC_INTERNAL)
                    << "Unable to find IP multicast entry to update "
                    << QuotedVar(ip_multicast_entry.ip_multicast_entry_key);
      break;
    }
    // No change means nothing to do.
    if (old_ip_multicast_entry_ptr->action == ip_multicast_entry.action &&
        old_ip_multicast_entry_ptr->multicast_group_id ==
            ip_multicast_entry.multicast_group_id) {
      statuses[i] = ReturnCode()
                    << "Entry "
                    << QuotedVar(ip_multicast_entry.ip_multicast_entry_key)
                    << " is already assigned to multicast_group_id "
                    << QuotedVar(ip_multicast_entry.multicast_group_id);
      continue;
    }

    // Fetch the multicast group OID.
    sai_object_id_t group_oid = SAI_NULL_OBJECT_ID;
    if (!m_p4OidMapper->getOID(SAI_OBJECT_TYPE_IPMC_GROUP,
                               ip_multicast_entry.multicast_group_id,
                               &group_oid)) {
      statuses[i] = ReturnCode(StatusCode::SWSS_RC_NOT_FOUND)
                    << "Unknown multicast group ID "
                    << QuotedVar(ip_multicast_entry.multicast_group_id);
      break;
    }

    // Update the multicast group OID attribute.
    sai_attribute_t update_attr;
    update_attr.id = SAI_IPMC_ENTRY_ATTR_OUTPUT_GROUP_ID;
    update_attr.value.oid = group_oid;
    statuses[i] = sai_ipmc_api->set_ipmc_entry_attribute(
        &old_ip_multicast_entry_ptr->sai_ipmc_entry, &update_attr);
    if (statuses[i] != SAI_STATUS_SUCCESS) {
      break;
    }

    // TODO: Add with counter support.
    // attr.id = SAI_IPMC_ENTRY_ATTR_COUNTER_ID;
    // attr.value.oid = group_counter_oid;

    // Bookkeeping
    m_p4OidMapper->decreaseRefCount(
        SAI_OBJECT_TYPE_IPMC_GROUP,
        old_ip_multicast_entry_ptr->multicast_group_id);
    m_p4OidMapper->increaseRefCount(SAI_OBJECT_TYPE_IPMC_GROUP,
                                    ip_multicast_entry.multicast_group_id);
    // We update the old entry object rather than updating maps.
    old_ip_multicast_entry_ptr->multicast_group_id =
        ip_multicast_entry.multicast_group_id;
    old_ip_multicast_entry_ptr->controller_metadata =
        ip_multicast_entry.controller_metadata;

    statuses[i] = ReturnCode();
  }
  return statuses;
}

std::vector<ReturnCode> IpMulticastManager::deleteIpMulticastEntries(
    const std::vector<P4IpMulticastEntry>& ip_multicast_entries) {
  SWSS_LOG_ENTER();
  std::vector<ReturnCode> statuses(ip_multicast_entries.size());
  fillStatusArrayWithNotExecuted(statuses, 0);

  for (size_t i = 0; i < ip_multicast_entries.size(); ++i) {
    const auto& ip_multicast_entry = ip_multicast_entries[i];

    auto* ip_multicast_entry_ptr =
        getIpMulticastEntry(ip_multicast_entry.ip_multicast_entry_key);
    if (ip_multicast_entry_ptr == nullptr) {
      statuses[i] = ReturnCode(StatusCode::SWSS_RC_NOT_FOUND)
                    << "IP multicast entry "
                    << QuotedVar(ip_multicast_entry.ip_multicast_entry_key)
                    << " does not exist in the internal cache";
      break;
    }

    // Remove the entry
    statuses[i] = sai_ipmc_api->remove_ipmc_entry(
        &ip_multicast_entry_ptr->sai_ipmc_entry);
    if (statuses[i] != SAI_STATUS_SUCCESS) {
      break;
    }

    // Bookkeeping
    m_p4OidMapper->decreaseRefCount(SAI_OBJECT_TYPE_IPMC_GROUP,
                                    ip_multicast_entry_ptr->multicast_group_id);
    m_p4OidMapper->eraseOID(SAI_OBJECT_TYPE_IPMC_ENTRY,
                            ip_multicast_entry.ip_multicast_entry_key);
    gCrmOrch->decCrmResUsedCounter(CrmResourceType::CRM_IPMC_ENTRY);
    m_vrfOrch->decreaseVrfRefCount(ip_multicast_entry.vrf_id);
    m_ipMulticastTable.erase(ip_multicast_entry.ip_multicast_entry_key);

    statuses[i] = ReturnCode();
  }
  return statuses;
}

}  // namespace p4orch
