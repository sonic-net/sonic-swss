#include "p4orch/tunnel_decap_group_manager.h"

#include <map>
#include <nlohmann/json.hpp>
#include <sstream>
#include <string>
#include <vector>

#include "SaiAttributeList.h"
#include "crmorch.h"
#include "dbconnector.h"
#include "ipprefix.h"
#include "logger.h"
#include "p4orch/p4orch_util.h"
#include "sai_serialize.h"
#include "swssnet.h"
#include "table.h"
#include "tokenize.h"
extern "C" {
#include "sai.h"
}

using ::p4orch::kTableKeyDelimiter;

extern sai_object_id_t gSwitchId;
extern sai_tunnel_api_t* sai_tunnel_api;
extern sai_object_id_t gUnderlayIfId;
static sai_object_id_t dummyTunnelId = SAI_NULL_OBJECT_ID;

namespace {

// Create dummy tunnel.
sai_object_id_t create_dummy_tunnel(void) {
  sai_attribute_t attr;
  std::vector<sai_attribute_t> tunnel_attrs;

  attr.id = SAI_TUNNEL_ATTR_TYPE;
  attr.value.s32 = SAI_TUNNEL_TYPE_IPINIP;
  tunnel_attrs.push_back(attr);

  attr.id = SAI_TUNNEL_ATTR_OVERLAY_INTERFACE;
  attr.value.oid = gUnderlayIfId;
  tunnel_attrs.push_back(attr);

  attr.id = SAI_TUNNEL_ATTR_UNDERLAY_INTERFACE;
  attr.value.oid = gUnderlayIfId;
  tunnel_attrs.push_back(attr);

  sai_object_id_t tunnel_id = SAI_NULL_OBJECT_ID;
  sai_status_t status = sai_tunnel_api->create_tunnel(
      &tunnel_id, gSwitchId, static_cast<uint32_t>(tunnel_attrs.size()),
      tunnel_attrs.data());
  CHECK_ERROR_AND_LOG(status, "Failed to create dummy tunnel.");

  return tunnel_id;
}

std::vector<sai_attribute_t> prepareSaiAttrs(
    const Ipv6TunnelTermTableEntry& ipv6_tunnel_term_entry) {
  std::vector<sai_attribute_t> attrs;
  sai_attribute_t attr;

  // Decapsulate IP-in-IP encapsulated packets only.
  attr.id = SAI_TUNNEL_TERM_TABLE_ENTRY_ATTR_TUNNEL_TYPE;
  attr.value.s32 = SAI_TUNNEL_TYPE_IPINIP;
  attrs.push_back(attr);

  // We use a wildcard match on src IP and dest IP.
  attr.id = SAI_TUNNEL_TERM_TABLE_ENTRY_ATTR_TYPE;
  attr.value.s32 = SAI_TUNNEL_TERM_TABLE_ENTRY_TYPE_MP2MP;
  attrs.push_back(attr);

  // Match on destination IP.
  attr.id = SAI_TUNNEL_TERM_TABLE_ENTRY_ATTR_DST_IP;
  swss::copy(attr.value.ipaddr, ipv6_tunnel_term_entry.dst_ipv6_ip);
  attrs.push_back(attr);

  // Match on destination MASK.
  attr.id = SAI_TUNNEL_TERM_TABLE_ENTRY_ATTR_DST_IP_MASK;
  swss::copy(attr.value.ipaddr, ipv6_tunnel_term_entry.dst_ipv6_mask);
  attrs.push_back(attr);

  // Set the VRF for routing of the inner packet.
  attr.id = SAI_TUNNEL_TERM_TABLE_ENTRY_ATTR_VR_ID;
  attr.value.oid = ipv6_tunnel_term_entry.vrf_oid;
  attrs.push_back(attr);

  if (dummyTunnelId == SAI_NULL_OBJECT_ID)
    dummyTunnelId = create_dummy_tunnel();

  // Currently specifying a tunnel object is mendatory in SAI,
  // but it is unclear for what purpose. Our use case should
  // technically not require it.
  // As a workaround, we use a dummy object ID here.
  attr.id = SAI_TUNNEL_TERM_TABLE_ENTRY_ATTR_ACTION_TUNNEL_ID;
  attr.value.oid = dummyTunnelId;
  attrs.push_back(attr);

  return attrs;
}

}  // namespace

TunnelDecapGroupManager::TunnelDecapGroupManager(
    P4OidMapper* p4oidMapper, VRFOrch* vrfOrch,
    ResponsePublisherInterface* publisher) {
  SWSS_LOG_ENTER();

  assert(p4oidMapper != nullptr);
  m_p4OidMapper = p4oidMapper;
  assert(vrfOrch != nullptr);
  m_vrfOrch = vrfOrch;
  assert(publisher != nullptr);
  m_publisher = publisher;
}

Ipv6TunnelTermTableEntry* TunnelDecapGroupManager::getIpv6TunnelTermEntry(
    const std::string& ipv6_tunnel_term_key) {
  SWSS_LOG_ENTER();

  auto it = m_ipv6TunnelTermTable.find(ipv6_tunnel_term_key);
  if (it == m_ipv6TunnelTermTable.end()) {
    return nullptr;
  } else {
    return &it->second;
  }
};

ReturnCode TunnelDecapGroupManager::validateIpv6TunnelTermAppDbEntry(
    const Ipv6TunnelTermAppDbEntry& app_db_entry) {
  SWSS_LOG_ENTER();

  if (app_db_entry.action_str != p4orch::kIpv6TunnelTermAction) {
    return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
           << "Invalid action " << QuotedVar(app_db_entry.action_str)
           << " of Ipv6 tunnel termination table entry";
  }
  if (app_db_entry.dst_ipv6_ip.isV4()) {
    return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
           << QuotedVar(prependParamField("dst_ipv6_ip"))
           << " field is not IPv6";
  }
  if (app_db_entry.dst_ipv6_mask.isV4()) {
    return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
           << QuotedVar(prependParamField("dst_ipv6_ip"))
           << " field is not IPv6";
  }
  return ReturnCode();
}

ReturnCode TunnelDecapGroupManager::validateIpv6TunnelTermAppDbEntry(
    const Ipv6TunnelTermAppDbEntry& app_db_entry,
    const std::string& operation) {
  SWSS_LOG_ENTER();

  Ipv6TunnelTermTableEntry entry =
      Ipv6TunnelTermTableEntry(app_db_entry.dst_ipv6_ip,
                               app_db_entry.dst_ipv6_mask, app_db_entry.vrf_id);

  if (operation == SET_COMMAND) {
    RETURN_IF_ERROR(validateIpv6TunnelTermAppDbEntry(app_db_entry));
    if (getIpv6TunnelTermEntry(entry.ipv6_tunnel_term_key) == nullptr) {
      if (m_p4OidMapper->existsOID(SAI_OBJECT_TYPE_TUNNEL_TERM_TABLE_ENTRY,
                                   entry.ipv6_tunnel_term_key)) {
        RETURN_INTERNAL_ERROR_AND_RAISE_CRITICAL(
            "Ipv6 tunnel termination table entry with key "
            << QuotedVar(entry.ipv6_tunnel_term_key)
            << " already exists in centralized mapper");
      }

      // Check the existence of VRF the Ipv6 tunnel termination table entry
      // depends on.
      if (entry.vrf_id != "" && !m_vrfOrch->isVRFexists(entry.vrf_id)) {
        return ReturnCode(StatusCode::SWSS_RC_NOT_FOUND)
               << "No VRF found with id " << QuotedVar(entry.vrf_id) << " for "
               << "Ipv6 tunnel termination table entry that matches on "
               << QuotedVar(entry.dst_ipv6_ip.to_string()) << "&"
               << QuotedVar(entry.dst_ipv6_mask.to_string());
      }
    }
  } else if (operation == DEL_COMMAND) {
    // Check the existence of the Ipv6 tunnel termination table entry in tunnel
    // decap group manager and centralized mapper.
    if (getIpv6TunnelTermEntry(entry.ipv6_tunnel_term_key) == nullptr) {
      LOG_ERROR_AND_RETURN(ReturnCode(StatusCode::SWSS_RC_NOT_FOUND)
                           << "Ipv6 tunnel termination table entry with key "
                           << QuotedVar(entry.ipv6_tunnel_term_key)
                           << " does not exist in tunnel decap group manager");
    }

    // Check if there is anything referring to the IPv6 tunnel termination
    // table entry before deletion.
    uint32_t ref_count;
    if (!m_p4OidMapper->getRefCount(SAI_OBJECT_TYPE_TUNNEL_TERM_TABLE_ENTRY,
                                    entry.ipv6_tunnel_term_key, &ref_count)) {
      RETURN_INTERNAL_ERROR_AND_RAISE_CRITICAL(
          "Failed to get reference count for Ipv6 tunnel termination table "
          "entry "
          << QuotedVar(entry.ipv6_tunnel_term_key));
    }
    if (ref_count > 0) {
      LOG_ERROR_AND_RETURN(ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
                           << "Ipv6 tunnel termination table entry "
                           << QuotedVar(entry.ipv6_tunnel_term_key)
                           << " referenced by other objects (ref_count = "
                           << ref_count << ")");
    }
  }

  return ReturnCode();
}

Ipv6TunnelTermTableEntry::Ipv6TunnelTermTableEntry(
    const swss::IpAddress& dst_ipv6_ip, const swss::IpAddress& dst_ipv6_mask,
    const std::string& vrf_id)
    : dst_ipv6_ip(dst_ipv6_ip), dst_ipv6_mask(dst_ipv6_mask), vrf_id(vrf_id) {
  SWSS_LOG_ENTER();
  ipv6_tunnel_term_key = KeyGenerator::generateIpv6TunnelTermKey(
      dst_ipv6_ip, dst_ipv6_mask, vrf_id);
}

ReturnCode TunnelDecapGroupManager::getSaiObject(const std::string& json_key,
                                                 sai_object_type_t& object_type,
                                                 std::string& object_key) {
  return StatusCode::SWSS_RC_UNIMPLEMENTED;
}

void TunnelDecapGroupManager::enqueue(
    const std::string& table_name, const swss::KeyOpFieldsValuesTuple& entry) {
  m_entries.push_back(entry);
}

void TunnelDecapGroupManager::drainWithNotExecuted() {
  drainMgmtWithNotExecuted(m_entries, m_publisher);
}

ReturnCode TunnelDecapGroupManager::drain() {
  SWSS_LOG_ENTER();

  std::vector<Ipv6TunnelTermAppDbEntry> entry_list;
  std::vector<swss::KeyOpFieldsValuesTuple> tuple_list;

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
    const std::string& operation = kfvOp(key_op_fvs_tuple);

    auto app_db_entry_or = deserializeIpv6TunnelTermAppDbEntry(key, attributes);
    if (!app_db_entry_or.ok()) {
      status = app_db_entry_or.status();
      SWSS_LOG_ERROR(
          "Unable to deserialize Ipv6 tunnel termination table entry with key "
          "%s: %s",
          QuotedVar(kfvKey(key_op_fvs_tuple)).c_str(),
          status.message().c_str());
      m_publisher->publish(APP_P4RT_TABLE_NAME, kfvKey(key_op_fvs_tuple),
                           kfvFieldsValues(key_op_fvs_tuple), status,
                           /*replace=*/true);
      break;
    }
    auto& app_db_entry = *app_db_entry_or;

    const std::string ipv6_tunnel_term_entry_key =
        KeyGenerator::generateIpv6TunnelTermKey(app_db_entry.dst_ipv6_ip,
                                                app_db_entry.dst_ipv6_mask,
                                                app_db_entry.vrf_id);

    bool update =
        (getIpv6TunnelTermEntry(ipv6_tunnel_term_entry_key) != nullptr);

    status = validateIpv6TunnelTermAppDbEntry(app_db_entry, operation);
    if (!status.ok()) {
      SWSS_LOG_ERROR(
          "Validation failed for Ipv6 tunnel termination table entry with key "
          "%s: %s",
          QuotedVar(kfvKey(key_op_fvs_tuple)).c_str(),
          status.message().c_str());
      m_publisher->publish(APP_P4RT_TABLE_NAME, kfvKey(key_op_fvs_tuple),
                           kfvFieldsValues(key_op_fvs_tuple), status,
                           /*replace=*/true);
      break;
    }

    if (prev_op == "") {
      prev_op = operation;
      prev_update = update;
    }
    // Process the entries if the operation type changes.
    if (operation != prev_op || update != prev_update) {
      status = processEntries(entry_list, tuple_list, prev_op, prev_update);
      entry_list.clear();
      tuple_list.clear();
      prev_op = operation;
      prev_update = update;
    }

    if (!status.ok()) {
      m_publisher->publish(APP_P4RT_TABLE_NAME, kfvKey(key_op_fvs_tuple),
                           kfvFieldsValues(key_op_fvs_tuple),
                           ReturnCode(StatusCode::SWSS_RC_NOT_EXECUTED),
                           /*replace=*/true);

      break;
    }

    entry_list.push_back(app_db_entry);
    tuple_list.push_back(key_op_fvs_tuple);
  }

  if (!entry_list.empty()) {
    auto rc = processEntries(entry_list, tuple_list, prev_op, prev_update);
    if (!rc.ok()) {
      status = rc;
    }
  }
  drainWithNotExecuted();
  return status;
}

ReturnCodeOr<Ipv6TunnelTermAppDbEntry>
TunnelDecapGroupManager::deserializeIpv6TunnelTermAppDbEntry(
    const std::string& key,
    const std::vector<swss::FieldValueTuple>& attributes) {
  SWSS_LOG_ENTER();

  Ipv6TunnelTermAppDbEntry app_db_entry = {};

  // Default IP and mask.
  app_db_entry.dst_ipv6_ip = swss::IpAddress("0:0:0:0:0:0:0:0");
  app_db_entry.dst_ipv6_mask = swss::IpAddress("0:0:0:0:0:0:0:0");

  try {
    nlohmann::json j = nlohmann::json::parse(key);
    if (j.find(prependMatchField(p4orch::kDecapDstIpv6)) != j.end()) {
      std::string ipv6 = j[prependMatchField(p4orch::kDecapDstIpv6)];
      const auto& ip_and_mask =
          swss::tokenize(ipv6, p4orch::kDataMaskDelimiter);
      if (ip_and_mask.size() != 2) {
        return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
               << "Invalid Ipv6 tunnel termination table entry: "
               << "should be in the format of <value> & <mask>.";
      }
      app_db_entry.dst_ipv6_ip = swss::IpAddress(trim(ip_and_mask[0]));
      app_db_entry.dst_ipv6_mask = swss::IpAddress(trim(ip_and_mask[1]));
    }
  } catch (std::exception& ex) {
    return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
           << "Failed to deserialize Ipv6 tunnel termination table entry "
           << "destination IPv6";
  }

  for (const auto& it : attributes) {
    const auto& field = fvField(it);
    const auto& value = fvValue(it);
    if (field == prependParamField(p4orch::kVrfId)) {
      app_db_entry.vrf_id = value;
    } else if (field == p4orch::kAction) {
      app_db_entry.action_str = value;
    } else if (field != p4orch::kControllerMetadata) {
      return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
             << "Unexpected field " << QuotedVar(field) << " in table entry";
    }
  }

  return app_db_entry;
}

std::vector<ReturnCode> TunnelDecapGroupManager::createIpv6TunnelTermEntries(
    const std::vector<Ipv6TunnelTermAppDbEntry>& ipv6_tunnel_term_entries) {
  SWSS_LOG_ENTER();

  std::vector<Ipv6TunnelTermTableEntry> entries;
  std::vector<std::string> vrf_keys(ipv6_tunnel_term_entries.size());
  std::vector<sai_object_id_t> ipv6_tunnel_term_oids(
      ipv6_tunnel_term_entries.size());
  std::vector<std::vector<sai_attribute_t>> sai_attrs(
      ipv6_tunnel_term_entries.size());
  std::vector<sai_status_t> object_statuses(ipv6_tunnel_term_entries.size());
  std::vector<ReturnCode> statuses(ipv6_tunnel_term_entries.size());

  for (size_t i = 0; i < ipv6_tunnel_term_entries.size(); ++i) {
    statuses[i] = StatusCode::SWSS_RC_NOT_EXECUTED;
    entries.push_back(
        Ipv6TunnelTermTableEntry(ipv6_tunnel_term_entries[i].dst_ipv6_ip,
                                 ipv6_tunnel_term_entries[i].dst_ipv6_mask,
                                 ipv6_tunnel_term_entries[i].vrf_id));

    entries[i].vrf_oid =
        m_vrfOrch->getVRFid(ipv6_tunnel_term_entries[i].vrf_id);

    sai_attrs[i] = prepareSaiAttrs(entries[i]);
  }

  for (size_t i = 0; i < ipv6_tunnel_term_entries.size(); ++i) {
    object_statuses[i] = sai_tunnel_api->create_tunnel_term_table_entry(
        &(ipv6_tunnel_term_oids[i]), gSwitchId,
        static_cast<uint32_t>(sai_attrs[i].size()), sai_attrs[i].data());
    CHECK_ERROR_AND_LOG(object_statuses[i],
                        "Failed to create Ipv6 tunnel termination table entry "
                            << QuotedVar(entries[i].ipv6_tunnel_term_key));

    if (object_statuses[i] == SAI_STATUS_SUCCESS) {
      statuses[i] = StatusCode::SWSS_RC_SUCCESS;
      entries[i].ipv6_tunnel_term_oid = ipv6_tunnel_term_oids[i];

      // On successful creation, increment ref count.
      m_vrfOrch->increaseVrfRefCount(entries[i].vrf_id);

      // Add created entry to internal table.
      m_ipv6TunnelTermTable.emplace(entries[i].ipv6_tunnel_term_key,
                                    entries[i]);

      // Add the key to OID map to centralized mapper.
      m_p4OidMapper->setOID(SAI_OBJECT_TYPE_TUNNEL_TERM_TABLE_ENTRY,
                            entries[i].ipv6_tunnel_term_key,
                            entries[i].ipv6_tunnel_term_oid);
    } else {
      statuses[i] = ReturnCode(object_statuses[i])
                    << "Failed to create Ipv6 tunnel termination table entry "
                    << QuotedVar(entries[i].ipv6_tunnel_term_key);
      break;
    }
  }

  return statuses;
}

// Should always be called after validateIpv6TunnelTermAppDbEntry(), which
// checks the existence of the entries to be removed.
std::vector<ReturnCode> TunnelDecapGroupManager::removeIpv6TunnelTermEntries(
    const std::vector<Ipv6TunnelTermAppDbEntry>& ipv6_tunnel_term_entries) {
  SWSS_LOG_ENTER();

  std::vector<Ipv6TunnelTermTableEntry*> entries(
      ipv6_tunnel_term_entries.size());
  std::vector<sai_object_id_t> ipv6_tunnel_term_oids(
      ipv6_tunnel_term_entries.size());
  std::vector<sai_status_t> object_statuses(ipv6_tunnel_term_entries.size());
  std::vector<ReturnCode> statuses(ipv6_tunnel_term_entries.size());

  for (size_t i = 0; i < ipv6_tunnel_term_entries.size(); ++i) {
    statuses[i] = StatusCode::SWSS_RC_NOT_EXECUTED;

    const std::string ipv6_tunnel_term_entry_key =
        KeyGenerator::generateIpv6TunnelTermKey(
            ipv6_tunnel_term_entries[i].dst_ipv6_ip,
            ipv6_tunnel_term_entries[i].dst_ipv6_mask,
            ipv6_tunnel_term_entries[i].vrf_id);

    // getIpv6TunnelTermEntry() may return a nullptr.
    // For entry deletion operations validateIpv6TunnelTermAppDbEntry() checks
    // if the getIpv6TunnelTermEntry() function returns nullptr.
    entries[i] = getIpv6TunnelTermEntry(ipv6_tunnel_term_entry_key);
    ipv6_tunnel_term_oids[i] = entries[i]->ipv6_tunnel_term_oid;
  }

  for (size_t i = 0; i < ipv6_tunnel_term_entries.size(); ++i) {
    object_statuses[i] = sai_tunnel_api->remove_tunnel_term_table_entry(
        ipv6_tunnel_term_oids[i]);

    CHECK_ERROR_AND_LOG(object_statuses[i],
                        "Failed to remove Ipv6 tunnel termination table entry "
                            << QuotedVar(entries[i]->ipv6_tunnel_term_key));

    if (object_statuses[i] == SAI_STATUS_SUCCESS) {
      statuses[i] = StatusCode::SWSS_RC_SUCCESS;

      // On successful deletion, decrement ref count.
      m_vrfOrch->decreaseVrfRefCount(entries[i]->vrf_id);

      // Remove the key to OID map to centralized mapper.
      m_p4OidMapper->eraseOID(SAI_OBJECT_TYPE_TUNNEL_TERM_TABLE_ENTRY,
                              entries[i]->ipv6_tunnel_term_key);

      // Remove the entry from internal table.
      m_ipv6TunnelTermTable.erase(entries[i]->ipv6_tunnel_term_key);
    } else {
      statuses[i] = ReturnCode(object_statuses[i])
                    << "Failed to remove Ipv6 tunnel termination table entry "
                    << QuotedVar(entries[i]->ipv6_tunnel_term_key);
      break;
    }
  }

  return statuses;
}

ReturnCode TunnelDecapGroupManager::processEntries(
    const std::vector<Ipv6TunnelTermAppDbEntry>& entries,
    const std::vector<swss::KeyOpFieldsValuesTuple>& tuple_list,
    const std::string& op, bool update) {
  SWSS_LOG_ENTER();

  ReturnCode status;
  std::vector<ReturnCode> statuses;
  if (op == SET_COMMAND) {
    if (!update) {
      statuses = createIpv6TunnelTermEntries(entries);
    } else {
      for (size_t i = 0; i < entries.size(); ++i)
        m_publisher->publish(APP_P4RT_TABLE_NAME, kfvKey(tuple_list[i]),
                             kfvFieldsValues(tuple_list[i]),
                             ReturnCode(StatusCode::SWSS_RC_UNIMPLEMENTED),
                             /*replace=*/true);
      LOG_ERROR_AND_RETURN(
          ReturnCode(StatusCode::SWSS_RC_UNIMPLEMENTED)
          << "Currently Ipv6 tunnel termination table entry doesn't support "
          << "update by SAI.");
    }
  } else {
    statuses = removeIpv6TunnelTermEntries(entries);
  }
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

std::string TunnelDecapGroupManager::verifyState(
    const std::string& key, const std::vector<swss::FieldValueTuple>& tuple) {
  SWSS_LOG_ENTER();

  auto pos = key.find_first_of(kTableKeyDelimiter);
  if (pos == std::string::npos) {
    return std::string("Invalid key: ") + key;
  }
  std::string p4rt_table = key.substr(0, pos);
  std::string p4rt_key = key.substr(pos + 1);
  if (p4rt_table != APP_P4RT_TABLE_NAME) {
    return std::string("Invalid key: ") + key;
  }
  std::string table_name;
  std::string key_content;
  parseP4RTKey(p4rt_key, &table_name, &key_content);
  if (table_name != APP_P4RT_IPV6_TUNNEL_TERMINATION_TABLE_NAME) {
    return std::string("Invalid key: ") + key;
  }

  ReturnCode status;
  auto app_db_entry_or =
      deserializeIpv6TunnelTermAppDbEntry(key_content, tuple);
  if (!app_db_entry_or.ok()) {
    status = app_db_entry_or.status();
    std::stringstream msg;
    msg << "Unable to deserialize key " << QuotedVar(key) << ": "
        << status.message();
    return msg.str();
  }
  auto& app_db_entry = *app_db_entry_or;

  const std::string ipv6_tunnel_term_entry_key =
      KeyGenerator::generateIpv6TunnelTermKey(app_db_entry.dst_ipv6_ip,
                                              app_db_entry.dst_ipv6_mask,
                                              app_db_entry.vrf_id);
  auto* ipv6_tunnel_term_entry =
      getIpv6TunnelTermEntry(ipv6_tunnel_term_entry_key);
  if (ipv6_tunnel_term_entry == nullptr) {
    std::stringstream msg;
    msg << "No entry found with key " << QuotedVar(key);
    return msg.str();
  }

  std::string cache_result =
      verifyStateCache(app_db_entry, ipv6_tunnel_term_entry);
  std::string asic_db_result = verifyStateAsicDb(ipv6_tunnel_term_entry);
  if (cache_result.empty()) {
    return asic_db_result;
  }
  if (asic_db_result.empty()) {
    return cache_result;
  }
  return cache_result + "; " + asic_db_result;
}

// LINT.IfChange(verify_state_cache)
std::string TunnelDecapGroupManager::verifyStateCache(
    const Ipv6TunnelTermAppDbEntry& app_db_entry,
    const Ipv6TunnelTermTableEntry* ipv6_tunnel_term_entry) {
  const std::string ipv6_tunnel_term_entry_key =
      KeyGenerator::generateIpv6TunnelTermKey(app_db_entry.dst_ipv6_ip,
                                              app_db_entry.dst_ipv6_mask,
                                              app_db_entry.vrf_id);
  ReturnCode status =
      validateIpv6TunnelTermAppDbEntry(app_db_entry, SET_COMMAND);
  if (!status.ok()) {
    std::stringstream msg;
    msg << "Validation failed for Ipv6 tunnel termination table entry with key "
        << QuotedVar(ipv6_tunnel_term_entry_key) << ": " << status.message();
    return msg.str();
  }

  if (ipv6_tunnel_term_entry->ipv6_tunnel_term_key !=
      ipv6_tunnel_term_entry_key) {
    std::stringstream msg;
    msg << "Ipv6 tunnel termination table entry with key "
        << QuotedVar(ipv6_tunnel_term_entry_key)
        << " does not match internal cache "
        << QuotedVar(ipv6_tunnel_term_entry->ipv6_tunnel_term_key)
        << " in Tunnel Decap Group manager.";
    return msg.str();
  }
  if (app_db_entry.vrf_id != ipv6_tunnel_term_entry->vrf_id) {
    std::stringstream msg;
    msg << "Ipv6 tunnel termination table entry with vrf_id "
        << QuotedVar(app_db_entry.vrf_id) << " does not match internal cache "
        << QuotedVar(ipv6_tunnel_term_entry->vrf_id)
        << " in Tunnel Decap Group manager.";
    return msg.str();
  }
  if (app_db_entry.dst_ipv6_ip != ipv6_tunnel_term_entry->dst_ipv6_ip) {
    std::stringstream msg;
    msg << "Ipv6 tunnel termination table entry with dst_ipv6_ip "
        << QuotedVar(app_db_entry.dst_ipv6_ip.to_string())
        << " does not match internal cache "
        << QuotedVar(ipv6_tunnel_term_entry->dst_ipv6_ip.to_string())
        << " in Tunnel Decap Group manager.";
    return msg.str();
  }
  if (app_db_entry.dst_ipv6_mask != ipv6_tunnel_term_entry->dst_ipv6_mask) {
    std::stringstream msg;
    msg << "Ipv6 tunnel termination table entry with dst_ipv6_mask "
        << QuotedVar(app_db_entry.dst_ipv6_mask.to_string())
        << " does not match internal cache "
        << QuotedVar(ipv6_tunnel_term_entry->dst_ipv6_mask.to_string())
        << " in Tunnel Decap Group manager.";
    return msg.str();
  }

  return m_p4OidMapper->verifyOIDMapping(
      SAI_OBJECT_TYPE_TUNNEL_TERM_TABLE_ENTRY,
      ipv6_tunnel_term_entry->ipv6_tunnel_term_key,
      ipv6_tunnel_term_entry->ipv6_tunnel_term_oid);
}
// LINT.ThenChange()

std::string TunnelDecapGroupManager::verifyStateAsicDb(
    const Ipv6TunnelTermTableEntry* ipv6_tunnel_term_entry) {
  swss::DBConnector db("ASIC_DB", 0);
  swss::Table table(&db, "ASIC_STATE");

  // Verify Ipv6 tunnel termination table ASIC DB attributes
  std::vector<sai_attribute_t> attrs = prepareSaiAttrs(*ipv6_tunnel_term_entry);
  std::vector<swss::FieldValueTuple> exp =
      saimeta::SaiAttributeList::serialize_attr_list(
          SAI_OBJECT_TYPE_TUNNEL_TERM_TABLE_ENTRY, (uint32_t)attrs.size(),
          attrs.data(), /*countOnly=*/false);
  std::string key =
      sai_serialize_object_type(SAI_OBJECT_TYPE_TUNNEL_TERM_TABLE_ENTRY) + ":" +
      sai_serialize_object_id(ipv6_tunnel_term_entry->ipv6_tunnel_term_oid);
  std::vector<swss::FieldValueTuple> values;
  if (!table.get(key, values)) {
    return std::string("ASIC DB key not found ") + key;
  }

  return verifyAttrs(values, exp, std::vector<swss::FieldValueTuple>{},
                     /*allow_unknown=*/false);
}

