#include "p4orch/ip_multicast_manager.h"

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

  std::vector<P4IpMulticastEntry> ip_multicast_list;
  std::vector<swss::KeyOpFieldsValuesTuple> tuple_list;
  std::unordered_set<std::string> ip_multicast_entry_list;

  ReturnCode status;
  std::string prev_op;
  bool prev_update = false;
  while (!m_entries.empty()) {
    auto key_op_fvs_tuple = m_entries.front();
    m_entries.pop_front();
    std::string table_name;
    std::string key;
    parseP4RTKey(kfvKey(key_op_fvs_tuple), &table_name, &key);
    const std::vector<swss::FieldValueTuple>& attributes =
        kfvFieldsValues(key_op_fvs_tuple);

    auto ip_multicast_entry_or =
        deserializeIpMulticastEntry(key, attributes, table_name);
    if (!ip_multicast_entry_or.ok()) {
      status = ip_multicast_entry_or.status();
      SWSS_LOG_ERROR("Unable to deserialize APP DB entry with key %s: %s",
                     QuotedVar(table_name + ":" + key).c_str(),
                     status.message().c_str());
      m_publisher->publish(APP_P4RT_TABLE_NAME, kfvKey(key_op_fvs_tuple),
                           kfvFieldsValues(key_op_fvs_tuple), status,
                           /*replace=*/true);
      break;
    }
    auto& ip_multicast_entry = *ip_multicast_entry_or;

    // A single batch should not modify the same entry more than once.
    if (ip_multicast_entry_list.count(
            ip_multicast_entry.ip_multicast_entry_key) != 0) {
      status = ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
               << "IP multicast entry has been included in the same batch";
      SWSS_LOG_ERROR(
          "%s: %s", status.message().c_str(),
          QuotedVar(ip_multicast_entry.ip_multicast_entry_key).c_str());
      m_publisher->publish(APP_P4RT_TABLE_NAME, kfvKey(key_op_fvs_tuple),
                           kfvFieldsValues(key_op_fvs_tuple), status,
                           /*replace=*/true);
      break;
    }

    const std::string& operation = kfvOp(key_op_fvs_tuple);
    status = validateIpMulticastEntry(ip_multicast_entry, operation);
    if (!status.ok()) {
      SWSS_LOG_ERROR(
          "Validation failed for IP multicast APP DB entry with key  %s: %s",
          QuotedVar(table_name + ":" + key).c_str(), status.message().c_str());
      m_publisher->publish(APP_P4RT_TABLE_NAME, kfvKey(key_op_fvs_tuple),
                           kfvFieldsValues(key_op_fvs_tuple), status,
                           /*replace=*/true);
      break;
    }
    ip_multicast_entry_list.insert(ip_multicast_entry.ip_multicast_entry_key);

    auto* old_ip_multicast_entry_ptr =
        getIpMulticastEntry(ip_multicast_entry.ip_multicast_entry_key);
    bool update = (old_ip_multicast_entry_ptr != nullptr);
    if (prev_op == "") {
      prev_op = operation;
      prev_update = update;
    }
    // Process the entries if the operation type changes.
    if (operation != prev_op || update != prev_update) {
      status = processIpMulticastEntries(ip_multicast_list, tuple_list, prev_op,
                                         prev_update);
      ip_multicast_list.clear();
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
      ip_multicast_list.push_back(ip_multicast_entry);
      tuple_list.push_back(key_op_fvs_tuple);
    }
  }

  if (!ip_multicast_list.empty()) {
    ReturnCode rc = processIpMulticastEntries(ip_multicast_list, tuple_list,
                                              prev_op, prev_update);
    if (!rc.ok()) {
      status = rc;
    }
  }
  drainWithNotExecuted();
  return status;
}

void IpMulticastManager::drainWithNotExecuted() {
  drainMgmtWithNotExecuted(m_entries, m_publisher);
}

ReturnCode IpMulticastManager::processIpMulticastEntries(
    const std::vector<P4IpMulticastEntry>& ip_multicast_entries,
    const std::vector<swss::KeyOpFieldsValuesTuple>& tuple_list,
    const std::string& op, bool update) {
  SWSS_LOG_ENTER();

  ReturnCode status;
  std::vector<ReturnCode> statuses;
  // In syncd, bulk SAI calls use mode SAI_BULK_OP_ERROR_MODE_STOP_ON_ERROR.
  if (op == SET_COMMAND) {
    if (!update) {
      statuses = createIpMulticastEntries(ip_multicast_entries);
    } else {
      statuses = updateIpMulticastEntries(ip_multicast_entries);
    }
  } else {
    statuses = deleteIpMulticastEntries(ip_multicast_entries);
  }
  for (size_t i = 0; i < ip_multicast_entries.size(); ++i) {
    m_publisher->publish(APP_P4RT_TABLE_NAME, kfvKey(tuple_list[i]),
                         kfvFieldsValues(tuple_list[i]), statuses[i],
                         /*replace=*/true);
    if (status.ok() && !statuses[i].ok()) {
      status = statuses[i];
    }
  }
  return status;
}

std::string IpMulticastManager::verifyState(
    const std::string& key, const std::vector<swss::FieldValueTuple>& tuples) {
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
  if (table_name != APP_P4RT_IPV4_MULTICAST_TABLE_NAME &&
      table_name != APP_P4RT_IPV6_MULTICAST_TABLE_NAME) {
    return std::string("Invalid key, unexpected table name: ") + key;
  }

  ReturnCode status;
  auto app_db_entry_or =
      deserializeIpMulticastEntry(key_content, tuples, table_name);
  if (!app_db_entry_or.ok()) {
    status = app_db_entry_or.status();
    std::stringstream msg;
    msg << "Unable to deserialize key " << QuotedVar(key) << ": "
        << status.message();
    return msg.str();
  }
  auto& app_db_entry = *app_db_entry_or;

  auto* ip_multicast_entry =
      getIpMulticastEntry(app_db_entry.ip_multicast_entry_key);
  if (ip_multicast_entry == nullptr) {
    std::stringstream msg;
    msg << "No entry found with key " << QuotedVar(key);
    return msg.str();
  }

  std::string cache_result = verifyStateCache(app_db_entry, ip_multicast_entry);
  std::string asic_db_result = verifyStateAsicDb(ip_multicast_entry);
  if (cache_result.empty()) {
    return asic_db_result;
  }
  if (asic_db_result.empty()) {
    return cache_result;
  }
  return cache_result + "; " + asic_db_result;
}

// LINT.IfChange(verify_state_cache)
std::string IpMulticastManager::verifyStateCache(
    const P4IpMulticastEntry& app_db_entry,
    const P4IpMulticastEntry* ip_multicast_entry) {
  ReturnCode status = validateIpMulticastEntry(app_db_entry, SET_COMMAND);
  if (!status.ok()) {
    std::stringstream msg;
    msg << "Validation failed for IP multicast DB entry with key "
        << QuotedVar(app_db_entry.ip_multicast_entry_key) << ": "
        << status.message();
    return msg.str();
  }
  if (ip_multicast_entry->ip_multicast_entry_key !=
      app_db_entry.ip_multicast_entry_key) {
    std::stringstream msg;
    msg << "IP multicast entry "
        << QuotedVar(app_db_entry.ip_multicast_entry_key)
        << " does not match internal cache "
        << QuotedVar(ip_multicast_entry->ip_multicast_entry_key)
        << " in IP multicast manager.";
    return msg.str();
  }
  if (ip_multicast_entry->vrf_id != app_db_entry.vrf_id) {
    std::stringstream msg;
    msg << "IP multicast entry "
        << QuotedVar(app_db_entry.ip_multicast_entry_key) << " with VRF "
        << QuotedVar(app_db_entry.vrf_id) << " does not match internal cache "
        << QuotedVar(ip_multicast_entry->vrf_id) << " in IP multicast manager.";
    return msg.str();
  }
  if (ip_multicast_entry->ip_dst.to_string() !=
      app_db_entry.ip_dst.to_string()) {
    std::stringstream msg;
    msg << "IP multicast entry "
        << QuotedVar(app_db_entry.ip_multicast_entry_key)
        << " with IP destination address "
        << QuotedVar(app_db_entry.ip_dst.to_string())
        << " does not match internal cache "
        << QuotedVar(ip_multicast_entry->ip_dst.to_string())
        << " in IP multicast manager.";
    return msg.str();
  }
  if (ip_multicast_entry->action != app_db_entry.action) {
    std::stringstream msg;
    msg << "IP multicast entry "
        << QuotedVar(app_db_entry.ip_multicast_entry_key) << " with action "
        << QuotedVar(app_db_entry.action) << " does not match internal cache "
        << QuotedVar(ip_multicast_entry->action) << " in IP multicast manager.";
    return msg.str();
  }
  if (ip_multicast_entry->multicast_group_id !=
      app_db_entry.multicast_group_id) {
    std::stringstream msg;
    msg << "IP multicast entry "
        << QuotedVar(app_db_entry.ip_multicast_entry_key)
        << " with multicast group ID "
        << QuotedVar(app_db_entry.multicast_group_id)
        << " does not match internal cache "
        << QuotedVar(ip_multicast_entry->multicast_group_id)
        << " in IP multicast manager.";
    return msg.str();
  }
  if (ip_multicast_entry->controller_metadata !=
      app_db_entry.controller_metadata) {
    std::stringstream msg;
    msg << "IP multicast entry "
        << QuotedVar(app_db_entry.ip_multicast_entry_key)
        << " with controller metadata "
        << QuotedVar(app_db_entry.controller_metadata)
        << " does not match internal cache "
        << QuotedVar(ip_multicast_entry->controller_metadata)
        << " in IP multicast manager.";
    return msg.str();
  }
  return "";
}
// LINT.ThenChange()

std::string IpMulticastManager::verifyStateAsicDb(
    const P4IpMulticastEntry* ip_multicast_entry) {
  std::vector<sai_attribute_t> exp_attrs;
  sai_attribute_t attr;

  attr.id = SAI_IPMC_ENTRY_ATTR_PACKET_ACTION;
  attr.value.s32 = SAI_PACKET_ACTION_FORWARD;
  exp_attrs.push_back(attr);

  attr.id = SAI_IPMC_ENTRY_ATTR_OUTPUT_GROUP_ID;
  attr.value.oid = SAI_NULL_OBJECT_ID;
  m_p4OidMapper->getOID(SAI_OBJECT_TYPE_IPMC_GROUP,
                        ip_multicast_entry->multicast_group_id,
                        &attr.value.oid);
  exp_attrs.push_back(attr);

  // TODO: Add with counter support.
  // attr.id = SAI_IPMC_ENTRY_ATTR_COUNTER_ID;
  // attr.value.oid = group_counter_oid;
  // attrs.push_back(attr);

  std::vector<swss::FieldValueTuple> exp =
      saimeta::SaiAttributeList::serialize_attr_list(
          SAI_OBJECT_TYPE_IPMC_ENTRY, (uint32_t)exp_attrs.size(),
          exp_attrs.data(), /*countOnly=*/false);

  swss::DBConnector db("ASIC_DB", 0);
  swss::Table table(&db, "ASIC_STATE");
  std::string key =
      sai_serialize_object_type(SAI_OBJECT_TYPE_IPMC_ENTRY) + ":" +
      sai_serialize_ipmc_entry(prepareSaiIpmcEntry(*ip_multicast_entry));

  std::vector<swss::FieldValueTuple> values;
  if (!table.get(key, values)) {
    return std::string("ASIC DB key not found ") + key;
  }

  return verifyAttrs(values, exp, /*opt=*/std::vector<swss::FieldValueTuple>{},
                     /*allow_unknown=*/false);
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

// Performs IP multicast entry validation.
ReturnCode IpMulticastManager::validateIpMulticastEntry(
    const P4IpMulticastEntry& ip_multicast_entry,
    const std::string& operation) {
  SWSS_LOG_ENTER();

  if (!ip_multicast_entry.vrf_id.empty() &&
      !m_vrfOrch->isVRFexists(ip_multicast_entry.vrf_id)) {
    LOG_ERROR_AND_RETURN(ReturnCode(StatusCode::SWSS_RC_NOT_FOUND)
                         << "No VRF found with name "
                         << QuotedVar(ip_multicast_entry.vrf_id));
  }

  if (operation == SET_COMMAND) {
    return validateSetIpMulticastEntry(ip_multicast_entry);
  } else if (operation == DEL_COMMAND) {
    return validateDelIpMulticastEntry(ip_multicast_entry);
  }
  return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
         << "Unknown operation type " << QuotedVar(operation);
}

// Performs IP multicast entry validation for SET command.
ReturnCode IpMulticastManager::validateSetIpMulticastEntry(
    const P4IpMulticastEntry& ip_multicast_entry) {
  SWSS_LOG_ENTER();

  if (!ip_multicast_entry.action.empty() &&
      ip_multicast_entry.action != p4orch::kSetMulticastGroupId) {
    return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
           << "Unsupported action " << QuotedVar(ip_multicast_entry.action);
  }

  if (ip_multicast_entry.multicast_group_id.empty()) {
    return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
           << "The multicast_group_id is missing for "
           << QuotedVar(ip_multicast_entry.ip_multicast_entry_key);
  } else {
    if (!m_p4OidMapper->existsOID(SAI_OBJECT_TYPE_IPMC_GROUP,
                                  ip_multicast_entry.multicast_group_id)) {
      return ReturnCode(StatusCode::SWSS_RC_NOT_FOUND)
             << "No multicast group ID found for "
             << QuotedVar(ip_multicast_entry.multicast_group_id);
    }
  }

  auto* ip_multicast_entry_ptr =
      getIpMulticastEntry(ip_multicast_entry.ip_multicast_entry_key);
  bool is_update = ip_multicast_entry_ptr != nullptr;
  bool exist_in_mapper = m_p4OidMapper->existsOID(
      SAI_OBJECT_TYPE_IPMC_ENTRY, ip_multicast_entry.ip_multicast_entry_key);

  if (is_update && !exist_in_mapper) {
    return ReturnCode(StatusCode::SWSS_RC_INTERNAL)
           << "IP multicast entry exists in manager but does not exist in the "
              "centralized map";
  } else if (!is_update && exist_in_mapper) {
    return ReturnCode(StatusCode::SWSS_RC_INTERNAL)
           << "IP multicast entry does not exist in manager but does not exist "
              "in the centralized map";
  }
  return ReturnCode();
}

// Performs IP multicast entry validation for DEL command.
ReturnCode IpMulticastManager::validateDelIpMulticastEntry(
    const P4IpMulticastEntry& ip_multicast_entry) {
  SWSS_LOG_ENTER();
  auto* ip_multicast_entry_ptr =
      getIpMulticastEntry(ip_multicast_entry.ip_multicast_entry_key);
  if (ip_multicast_entry_ptr == nullptr) {
    return ReturnCode(StatusCode::SWSS_RC_NOT_FOUND)
           << "IP multicast entry does not exist: "
           << QuotedVar(ip_multicast_entry.ip_multicast_entry_key);
  }

  if (!m_p4OidMapper->existsOID(SAI_OBJECT_TYPE_IPMC_ENTRY,
                                ip_multicast_entry.ip_multicast_entry_key)) {
    RETURN_INTERNAL_ERROR_AND_RAISE_CRITICAL(
        "IP multicast entry does not exist in the centralized map");
  }
  return ReturnCode();
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
