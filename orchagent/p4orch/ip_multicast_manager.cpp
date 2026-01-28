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
extern sai_rpf_group_api_t* sai_rpf_group_api;

extern PortsOrch* gPortsOrch;

namespace p4orch {

namespace {}  // namespace

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

}  // namespace p4orch
