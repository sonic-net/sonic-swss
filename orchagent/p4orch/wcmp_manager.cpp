#include "p4orch/wcmp_manager.h"

#include <nlohmann/json.hpp>
#include <sstream>
#include <string>
#include <vector>

#include "SaiAttributeList.h"
#include "crmorch.h"
#include "dbconnector.h"
#include "logger.h"
#include "p4orch/p4orch_util.h"
#include "portsorch.h"
#include "sai_serialize.h"
#include "table.h"
extern "C"
{
#include "sai.h"
}

using ::p4orch::kTableKeyDelimiter;

extern sai_object_id_t gSwitchId;
extern sai_next_hop_group_api_t *sai_next_hop_group_api;
extern CrmOrch *gCrmOrch;
extern PortsOrch* gPortsOrch;

namespace p4orch
{

namespace
{

std::vector<sai_attribute_t> prepareSaiGroupAttrs(
    P4WcmpGroupEntry& wcmp_group_entry, bool update = false) {
  std::vector<sai_attribute_t> attrs;
  sai_attribute_t attr;

  if (!update) {
    attr.id = SAI_NEXT_HOP_GROUP_ATTR_TYPE;
    attr.value.s32 = SAI_NEXT_HOP_GROUP_TYPE_ECMP_WITH_MEMBERS;
    attrs.push_back(attr);
  }

  uint32_t count = 0;
  wcmp_group_entry.nexthop_ids.clear();
  wcmp_group_entry.nexthop_weights.clear();
  for (const auto& member : wcmp_group_entry.wcmp_group_members) {
    if (!member->pruned) {
      wcmp_group_entry.nexthop_ids.push_back(member->next_hop_oid);
      wcmp_group_entry.nexthop_weights.push_back(member->weight);
      count++;
    }
  }

  sai_attribute_t nhl, nhmwl;
  nhl.id = SAI_NEXT_HOP_GROUP_ATTR_NEXT_HOP_LIST;
  nhmwl.id = SAI_NEXT_HOP_GROUP_ATTR_NEXT_HOP_MEMBER_WEIGHT_LIST;
  nhl.value.objlist.count = count;
  nhmwl.value.u32list.count = count;
  if (count) {
    nhl.value.objlist.list = wcmp_group_entry.nexthop_ids.data();
    nhmwl.value.u32list.list = wcmp_group_entry.nexthop_weights.data();
  } else {
    nhl.value.objlist.list = nullptr;
    nhmwl.value.u32list.list = nullptr;
  }
  attrs.push_back(nhl);
  attrs.push_back(nhmwl);

  return attrs;
}

ReturnCode updateGroup(P4WcmpGroupEntry& wcmp_group) {
  auto attrs = prepareSaiGroupAttrs(wcmp_group, /*update=*/true);
  std::vector<sai_object_id_t> oids(attrs.size());
  std::vector<sai_status_t> status(attrs.size());
  for (size_t i = 0; i < attrs.size(); ++i) {
    oids[i] = wcmp_group.wcmp_group_oid;
  }
  // This SAI operation is assumed to be atomic.
  CHECK_ERROR_AND_LOG_AND_RETURN(
      sai_next_hop_group_api->set_next_hop_groups_attribute(
          uint32_t(attrs.size()), oids.data(), attrs.data(),
          SAI_BULK_OP_ERROR_MODE_STOP_ON_ERROR, status.data()),
      "Failed to update next hop group  "
          << QuotedVar(wcmp_group.wcmp_group_id));
  return ReturnCode();
}

}  // namespace

ReturnCode WcmpManager::validateWcmpGroupEntry(const P4WcmpGroupEntry &app_db_entry)
{
    for (auto &wcmp_group_member : app_db_entry.wcmp_group_members)
    {
        if (wcmp_group_member->weight <= 0)
        {
            return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
                   << "Invalid WCMP group member weight " << wcmp_group_member->weight << ": should be greater than 0.";
        }
        sai_object_id_t nexthop_oid = SAI_NULL_OBJECT_ID;
        if (!m_p4OidMapper->getOID(SAI_OBJECT_TYPE_NEXT_HOP,
                                   KeyGenerator::generateNextHopKey(wcmp_group_member->next_hop_id), &nexthop_oid))
        {
            return ReturnCode(StatusCode::SWSS_RC_NOT_FOUND)
                   << "Nexthop id " << QuotedVar(wcmp_group_member->next_hop_id) << " does not exist for WCMP group "
                   << QuotedVar(app_db_entry.wcmp_group_id);
        }
        if (!wcmp_group_member->watch_port.empty())
        {
            Port port;
            if (!gPortsOrch->getPort(wcmp_group_member->watch_port, port))
            {
                return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
                       << "Invalid watch_port field " << wcmp_group_member->watch_port
                       << ": should be a valid port name.";
            }
        }
    }
    return ReturnCode();
}

ReturnCodeOr<P4WcmpGroupEntry> WcmpManager::deserializeP4WcmpGroupAppDbEntry(
    const std::string &key, const std::vector<swss::FieldValueTuple> &attributes)
{
    P4WcmpGroupEntry app_db_entry = {};
    try
    {
        nlohmann::json j = nlohmann::json::parse(key);
        if (!j.is_object())
        {
            return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM) << "Invalid WCMP group key: should be a JSON object.";
        }
        app_db_entry.wcmp_group_id = j[prependMatchField(kWcmpGroupId)];
    }
    catch (std::exception &ex)
    {
        return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM) << "Failed to deserialize WCMP group key";
    }

    for (const auto &it : attributes)
    {
        const auto &field = fvField(it);
        const auto &value = fvValue(it);
        if (field == kActions)
        {
            try
            {
                nlohmann::json j = nlohmann::json::parse(value);
                if (!j.is_array())
                {
                    return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
                           << "Invalid WCMP group actions " << QuotedVar(value) << ", expecting an array.";
                }
                for (auto &action_item : j)
                {
                    std::shared_ptr<P4WcmpGroupMemberEntry> wcmp_group_member =
                        std::make_shared<P4WcmpGroupMemberEntry>();
                    std::string action = action_item[kAction];
                    if (action != kSetNexthopId)
                    {
                        return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
                               << "Unexpected action " << QuotedVar(action) << " in WCMP group entry";
                    }
                    if (action_item[prependParamField(kNexthopId)].empty())
                    {
                        return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
                               << "Next hop id was not found in entry member for WCMP "
                                  "group "
                               << QuotedVar(app_db_entry.wcmp_group_id);
                    }
                    wcmp_group_member->next_hop_id = action_item[prependParamField(kNexthopId)];
                    if (!action_item[kWeight].empty())
                    {
                        wcmp_group_member->weight = action_item[kWeight];
                    }
                    if (!action_item[kWatchPort].empty())
                    {
                        wcmp_group_member->watch_port = action_item[kWatchPort];
                    }
                    wcmp_group_member->wcmp_group_id = app_db_entry.wcmp_group_id;
                    wcmp_group_member->pruned = false;
                    app_db_entry.wcmp_group_members.push_back(wcmp_group_member);
                }
            }
            catch (std::exception &ex)
            {
                return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
                       << "Failed to deserialize WCMP group actions fields: " << QuotedVar(value);
            }
        }
        else if (field != kControllerMetadata)
        {
            return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
                   << "Unexpected field " << QuotedVar(field) << " in table entry";
        }
    }

    return app_db_entry;
}

P4WcmpGroupEntry *WcmpManager::getWcmpGroupEntry(const std::string &wcmp_group_id)
{
    SWSS_LOG_ENTER();
    const auto &wcmp_group_it = m_wcmpGroupTable.find(wcmp_group_id);
    if (wcmp_group_it == m_wcmpGroupTable.end())
        return nullptr;
    return &wcmp_group_it->second;
}

ReturnCode WcmpManager::processAddRequest(P4WcmpGroupEntry *app_db_entry)
{
    SWSS_LOG_ENTER();
    auto status = createWcmpGroup(app_db_entry);
    if (!status.ok())
    {
        SWSS_LOG_ERROR("Failed to create WCMP group with id %s: %s", QuotedVar(app_db_entry->wcmp_group_id).c_str(),
                       status.message().c_str());
    }
    return status;
}

void WcmpManager::insertMemberInPortNameToWcmpGroupMemberMap(std::shared_ptr<P4WcmpGroupMemberEntry> member)
{
    port_name_to_wcmp_group_member_map[member->watch_port].insert(member);
}

void WcmpManager::removeMemberFromPortNameToWcmpGroupMemberMap(std::shared_ptr<P4WcmpGroupMemberEntry> member)
{
    if (port_name_to_wcmp_group_member_map.find(member->watch_port) != port_name_to_wcmp_group_member_map.end())
    {
        auto &s = port_name_to_wcmp_group_member_map[member->watch_port];
        auto it = s.find(member);
        if (it != s.end())
        {
            s.erase(it);
        }
    }
}

ReturnCode WcmpManager::fetchPortOperStatus(const std::string &port_name, sai_port_oper_status_t *oper_status)
{
    if (!getPortOperStatusFromMap(port_name, oper_status))
    {
        // Get port object for associated watch port
        Port port;
        if (!gPortsOrch->getPort(port_name, port))
        {
            SWSS_LOG_ERROR("Failed to get port object for port %s", port_name.c_str());
            return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM);
        }
        // Get the oper-status of the port from hardware. In case of warm reboot,
        // this ensures that actual state of the port oper-status is used to
        // determine whether member associated with watch_port is to be created in
        // SAI.
        if (!gPortsOrch->getPortOperStatus(port, *oper_status))
        {
            RETURN_INTERNAL_ERROR_AND_RAISE_CRITICAL("Failed to get port oper-status for port " << port.m_alias);
        }
        // Update port oper-status in local map
        updatePortOperStatusMap(port.m_alias, *oper_status);
    }
    return ReturnCode();
}

ReturnCode WcmpManager::fetchMemberInfo(P4WcmpGroupEntry* wcmp_group) {
  for (auto& member : wcmp_group->wcmp_group_members) {
    if (!member->watch_port.empty()) {
      sai_port_oper_status_t oper_status = SAI_PORT_OPER_STATUS_DOWN;
      RETURN_IF_ERROR(fetchPortOperStatus(member->watch_port, &oper_status));
      if (oper_status != SAI_PORT_OPER_STATUS_UP) {
        member->pruned = true;
      }
    }

    m_p4OidMapper->getOID(SAI_OBJECT_TYPE_NEXT_HOP,
                          KeyGenerator::generateNextHopKey(member->next_hop_id),
                          &(member->next_hop_oid));
  }
  return ReturnCode();
}

ReturnCode WcmpManager::createWcmpGroup(P4WcmpGroupEntry *wcmp_group)
{
    SWSS_LOG_ENTER();
    RETURN_IF_ERROR(fetchMemberInfo(wcmp_group));

    auto attrs = prepareSaiGroupAttrs(*wcmp_group);

    CHECK_ERROR_AND_LOG_AND_RETURN(sai_next_hop_group_api->create_next_hop_group(&wcmp_group->wcmp_group_oid, gSwitchId,
                                                                                 (uint32_t)attrs.size(), attrs.data()),
                                   "Failed to create next hop group  " << QuotedVar(wcmp_group->wcmp_group_id));

    // Update reference count
    const auto &wcmp_group_key = KeyGenerator::generateWcmpGroupKey(wcmp_group->wcmp_group_id);
    gCrmOrch->incCrmResUsedCounter(CrmResourceType::CRM_NEXTHOP_GROUP);
    m_p4OidMapper->setOID(SAI_OBJECT_TYPE_NEXT_HOP_GROUP, wcmp_group_key, wcmp_group->wcmp_group_oid);
    for (auto& member : wcmp_group->wcmp_group_members) {
      const std::string& next_hop_key =
          KeyGenerator::generateNextHopKey(member->next_hop_id);
      gCrmOrch->incCrmResUsedCounter(CrmResourceType::CRM_NEXTHOP_GROUP_MEMBER);
      m_p4OidMapper->increaseRefCount(SAI_OBJECT_TYPE_NEXT_HOP, next_hop_key);
      if (!member->watch_port.empty()) {
        insertMemberInPortNameToWcmpGroupMemberMap(member);
      }
    }
    m_wcmpGroupTable[wcmp_group->wcmp_group_id] = *wcmp_group;
    return ReturnCode();
}

ReturnCode WcmpManager::processUpdateRequest(P4WcmpGroupEntry *wcmp_group_entry)
{
    SWSS_LOG_ENTER();
    auto *old_wcmp = getWcmpGroupEntry(wcmp_group_entry->wcmp_group_id);
    wcmp_group_entry->wcmp_group_oid = old_wcmp->wcmp_group_oid;
    RETURN_IF_ERROR(fetchMemberInfo(wcmp_group_entry));
    RETURN_IF_ERROR(updateGroup(*wcmp_group_entry));

    // Update reference count
    for (auto& member : old_wcmp->wcmp_group_members) {
      const std::string& next_hop_key =
          KeyGenerator::generateNextHopKey(member->next_hop_id);
      gCrmOrch->decCrmResUsedCounter(CrmResourceType::CRM_NEXTHOP_GROUP_MEMBER);
      m_p4OidMapper->decreaseRefCount(SAI_OBJECT_TYPE_NEXT_HOP, next_hop_key);
      if (!member->watch_port.empty()) {
        removeMemberFromPortNameToWcmpGroupMemberMap(member);
      }
    }
    for (auto& member : wcmp_group_entry->wcmp_group_members) {
      const std::string& next_hop_key =
          KeyGenerator::generateNextHopKey(member->next_hop_id);
      gCrmOrch->incCrmResUsedCounter(CrmResourceType::CRM_NEXTHOP_GROUP_MEMBER);
      m_p4OidMapper->increaseRefCount(SAI_OBJECT_TYPE_NEXT_HOP, next_hop_key);
      if (!member->watch_port.empty()) {
        insertMemberInPortNameToWcmpGroupMemberMap(member);
      }
    }

    m_wcmpGroupTable[wcmp_group_entry->wcmp_group_id] = *wcmp_group_entry;
    return ReturnCode();
}

ReturnCode WcmpManager::removeWcmpGroup(const std::string &wcmp_group_id)
{
    SWSS_LOG_ENTER();
    auto *wcmp_group = getWcmpGroupEntry(wcmp_group_id);
    if (wcmp_group == nullptr)
    {
        LOG_ERROR_AND_RETURN(ReturnCode(StatusCode::SWSS_RC_NOT_FOUND)
                             << "WCMP group with id " << QuotedVar(wcmp_group_id) << " was not found.");
    }

    // Check refcount before deleting group members
    uint32_t wcmp_group_refcount = 0;
    const auto &wcmp_group_key = KeyGenerator::generateWcmpGroupKey(wcmp_group_id);
    m_p4OidMapper->getRefCount(SAI_OBJECT_TYPE_NEXT_HOP_GROUP, wcmp_group_key, &wcmp_group_refcount);
    if (wcmp_group_refcount > 0) {
      LOG_ERROR_AND_RETURN(ReturnCode(StatusCode::SWSS_RC_IN_USE)
                           << "Failed to remove WCMP group with id "
                           << QuotedVar(wcmp_group_id)
                           << ", non-zero ref count " << wcmp_group_refcount);
    }

    // Delete group
    CHECK_ERROR_AND_LOG_AND_RETURN(
        sai_next_hop_group_api->remove_next_hop_group(
            wcmp_group->wcmp_group_oid),
        "Failed to delete WCMP group with id "
            << QuotedVar(wcmp_group->wcmp_group_id));
    m_p4OidMapper->eraseOID(SAI_OBJECT_TYPE_NEXT_HOP_GROUP, wcmp_group_key);
    gCrmOrch->decCrmResUsedCounter(CrmResourceType::CRM_NEXTHOP_GROUP);

    for (auto& member : wcmp_group->wcmp_group_members) {
      const std::string& next_hop_key =
          KeyGenerator::generateNextHopKey(member->next_hop_id);
      gCrmOrch->decCrmResUsedCounter(CrmResourceType::CRM_NEXTHOP_GROUP_MEMBER);
      m_p4OidMapper->decreaseRefCount(SAI_OBJECT_TYPE_NEXT_HOP, next_hop_key);
      if (!member->watch_port.empty()) {
        removeMemberFromPortNameToWcmpGroupMemberMap(member);
      }
    }

    m_wcmpGroupTable.erase(wcmp_group->wcmp_group_id);
    return ReturnCode();
}

void WcmpManager::updateWatchPort(const std::string& port, bool prune) {
  SWSS_LOG_ENTER();

  // Get list of WCMP group members associated with the watch_port

  if (port_name_to_wcmp_group_member_map.find(port) !=
      port_name_to_wcmp_group_member_map.end()) {
    for (auto& member : port_name_to_wcmp_group_member_map[port]) {
      if (member->pruned != prune) {
        auto* wcmp_group = getWcmpGroupEntry(member->wcmp_group_id);
        if (wcmp_group == nullptr) {
          SWSS_RAISE_CRITICAL_STATE("Failed to find WCMP group " +
                                    QuotedVar(member->wcmp_group_id) +
                                    " in updateWatchPort");
        } else {
          const std::string update = prune ? "prune" : "restore";
          member->pruned = prune;
          auto status = updateGroup(*wcmp_group);
          if (!status.ok()) {
            member->pruned = !member->pruned;
            SWSS_RAISE_CRITICAL_STATE(
                "Failed to " + update + " member in group " +
                QuotedVar(member->wcmp_group_id) +
                " in updateWatchPort: " + status.message());
          } else {
            SWSS_LOG_NOTICE("%s member %s from group %s", update.c_str(),
                            member->next_hop_id.c_str(),
                            member->wcmp_group_id.c_str());
          }
        }
      }
    }
  }
}

bool WcmpManager::getPortOperStatusFromMap(const std::string &port, sai_port_oper_status_t *oper_status)
{
    if (port_oper_status_map.find(port) != port_oper_status_map.end())
    {
        *oper_status = port_oper_status_map[port];
        return true;
    }
    return false;
}

void WcmpManager::updatePortOperStatusMap(const std::string &port, const sai_port_oper_status_t &status)
{
    port_oper_status_map[port] = status;
}

ReturnCode WcmpManager::getSaiObject(const std::string &json_key, sai_object_type_t &object_type,
                                     std::string &object_key)
{
    std::string value;

    try
    {
        nlohmann::json j = nlohmann::json::parse(json_key);
        if (j.find(prependMatchField(p4orch::kWcmpGroupId)) != j.end())
        {
            value = j.at(prependMatchField(p4orch::kWcmpGroupId)).get<std::string>();
            object_key = KeyGenerator::generateWcmpGroupKey(value);
            object_type = SAI_OBJECT_TYPE_NEXT_HOP_GROUP;
            return ReturnCode();
        }
        else
        {
            SWSS_LOG_ERROR("%s match parameter absent: required for dependent object query", p4orch::kWcmpGroupId);
        }
    }
    catch (std::exception &ex)
    {
        SWSS_LOG_ERROR("json_key parse error");
    }

    return StatusCode::SWSS_RC_INVALID_PARAM;
}

void WcmpManager::enqueue(const std::string &table_name, const swss::KeyOpFieldsValuesTuple &entry)
{
    m_entries.push_back(entry);
}

void WcmpManager::drainWithNotExecuted() {
  drainMgmtWithNotExecuted(m_entries, m_publisher);
}

ReturnCode WcmpManager::drain() {
  SWSS_LOG_ENTER();

  ReturnCode status;
  while (!m_entries.empty()) {
    auto key_op_fvs_tuple = m_entries.front();
    m_entries.pop_front();
    std::string table_name;
    std::string db_key;
    parseP4RTKey(kfvKey(key_op_fvs_tuple), &table_name, &db_key);
    const std::vector<swss::FieldValueTuple>& attributes =
        kfvFieldsValues(key_op_fvs_tuple);

    auto app_db_entry_or = deserializeP4WcmpGroupAppDbEntry(db_key, attributes);
    if (!app_db_entry_or.ok()) {
      status = app_db_entry_or.status();
      SWSS_LOG_ERROR(
          "Unable to deserialize APP DB WCMP group entry with key %s: %s",
          QuotedVar(table_name + ":" + db_key).c_str(),
          status.message().c_str());
      m_publisher->publish(APP_P4RT_TABLE_NAME, kfvKey(key_op_fvs_tuple),
                           kfvFieldsValues(key_op_fvs_tuple), status,
                           /*replace=*/true);
      break;
    }
    auto& app_db_entry = *app_db_entry_or;

    const std::string& operation = kfvOp(key_op_fvs_tuple);
    if (operation == SET_COMMAND) {
      status = validateWcmpGroupEntry(app_db_entry);
      if (!status.ok()) {
        SWSS_LOG_ERROR("Invalid WCMP group with id %s: %s",
                       QuotedVar(app_db_entry.wcmp_group_id).c_str(),
                       status.message().c_str());
        m_publisher->publish(APP_P4RT_TABLE_NAME, kfvKey(key_op_fvs_tuple),
                             kfvFieldsValues(key_op_fvs_tuple), status,
                             /*replace=*/true);
        break;
      }
      auto* wcmp_group_entry = getWcmpGroupEntry(app_db_entry.wcmp_group_id);
      if (wcmp_group_entry == nullptr) {
        // Create WCMP group
        status = processAddRequest(&app_db_entry);
      } else {
        // Modify existing WCMP group
        status = processUpdateRequest(&app_db_entry);
      }
    } else if (operation == DEL_COMMAND) {
      // Delete WCMP group
      status = removeWcmpGroup(app_db_entry.wcmp_group_id);
    } else {
      status = ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
               << "Unknown operation type: " << QuotedVar(operation)
               << " for WCMP group entry with key " << QuotedVar(table_name)
               << ":" << QuotedVar(db_key)
               << "; only SET and DEL operations are allowed.";
      SWSS_LOG_ERROR("Unknown operation type %s\n",
                     QuotedVar(operation).c_str());
    }
    m_publisher->publish(APP_P4RT_TABLE_NAME, kfvKey(key_op_fvs_tuple),
                         kfvFieldsValues(key_op_fvs_tuple), status,
                         /*replace=*/true);
    if (!status.ok()) {
      break;
    }
  }
  drainWithNotExecuted();
  return status;
}

std::string WcmpManager::verifyState(const std::string &key, const std::vector<swss::FieldValueTuple> &tuple)
{
    SWSS_LOG_ENTER();

    auto pos = key.find_first_of(kTableKeyDelimiter);
    if (pos == std::string::npos)
    {
        return std::string("Invalid key: ") + key;
    }
    std::string p4rt_table = key.substr(0, pos);
    std::string p4rt_key = key.substr(pos + 1);
    if (p4rt_table != APP_P4RT_TABLE_NAME)
    {
        return std::string("Invalid key: ") + key;
    }
    std::string table_name;
    std::string key_content;
    parseP4RTKey(p4rt_key, &table_name, &key_content);
    if (table_name != APP_P4RT_WCMP_GROUP_TABLE_NAME)
    {
        return std::string("Invalid key: ") + key;
    }

    ReturnCode status;
    auto app_db_entry_or = deserializeP4WcmpGroupAppDbEntry(key_content, tuple);
    if (!app_db_entry_or.ok())
    {
        status = app_db_entry_or.status();
        std::stringstream msg;
        msg << "Unable to deserialize key " << QuotedVar(key) << ": " << status.message();
        return msg.str();
    }
    auto &app_db_entry = *app_db_entry_or;

    auto *wcmp_group_entry = getWcmpGroupEntry(app_db_entry.wcmp_group_id);
    if (wcmp_group_entry == nullptr)
    {
        std::stringstream msg;
        msg << "No entry found with key " << QuotedVar(key);
        return msg.str();
    }

    std::string cache_result = verifyStateCache(app_db_entry, wcmp_group_entry);
    std::string asic_db_result = verifyStateAsicDb(wcmp_group_entry);
    if (cache_result.empty())
    {
        return asic_db_result;
    }
    if (asic_db_result.empty())
    {
        return cache_result;
    }
    return cache_result + "; " + asic_db_result;
}

std::string WcmpManager::verifyStateCache(const P4WcmpGroupEntry &app_db_entry,
                                          const P4WcmpGroupEntry *wcmp_group_entry)
{
    const std::string &wcmp_group_key = KeyGenerator::generateWcmpGroupKey(app_db_entry.wcmp_group_id);
    ReturnCode status = validateWcmpGroupEntry(app_db_entry);
    if (!status.ok())
    {
        std::stringstream msg;
        msg << "Validation failed for WCMP group DB entry with key " << QuotedVar(wcmp_group_key) << ": "
            << status.message();
        return msg.str();
    }

    if (wcmp_group_entry->wcmp_group_id != app_db_entry.wcmp_group_id)
    {
        std::stringstream msg;
        msg << "WCMP group ID " << QuotedVar(app_db_entry.wcmp_group_id) << " does not match internal cache "
            << QuotedVar(wcmp_group_entry->wcmp_group_id) << " in wcmp manager.";
        return msg.str();
    }

    std::string err_msg = m_p4OidMapper->verifyOIDMapping(SAI_OBJECT_TYPE_NEXT_HOP_GROUP, wcmp_group_key,
                                                          wcmp_group_entry->wcmp_group_oid);
    if (!err_msg.empty())
    {
        return err_msg;
    }

    if (wcmp_group_entry->wcmp_group_members.size() != app_db_entry.wcmp_group_members.size())
    {
        std::stringstream msg;
        msg << "WCMP group with ID " << QuotedVar(app_db_entry.wcmp_group_id) << " has member size "
            << app_db_entry.wcmp_group_members.size() << " non-matching internal cache "
            << wcmp_group_entry->wcmp_group_members.size();
        return msg.str();
    }

    for (size_t i = 0; i < wcmp_group_entry->wcmp_group_members.size(); ++i)
    {
        if (wcmp_group_entry->wcmp_group_members[i]->next_hop_id != app_db_entry.wcmp_group_members[i]->next_hop_id)
        {
            std::stringstream msg;
            msg << "WCMP group member " << QuotedVar(app_db_entry.wcmp_group_members[i]->next_hop_id)
                << " does not match internal cache " << QuotedVar(wcmp_group_entry->wcmp_group_members[i]->next_hop_id)
                << " in wcmp manager.";
            return msg.str();
        }
        if (wcmp_group_entry->wcmp_group_members[i]->weight != app_db_entry.wcmp_group_members[i]->weight)
        {
            std::stringstream msg;
            msg << "WCMP group member " << QuotedVar(app_db_entry.wcmp_group_members[i]->next_hop_id) << " weight "
                << app_db_entry.wcmp_group_members[i]->weight << " does not match internal cache "
                << wcmp_group_entry->wcmp_group_members[i]->weight << " in wcmp manager.";
            return msg.str();
        }
        if (wcmp_group_entry->wcmp_group_members[i]->watch_port != app_db_entry.wcmp_group_members[i]->watch_port)
        {
            std::stringstream msg;
            msg << "WCMP group member " << QuotedVar(app_db_entry.wcmp_group_members[i]->next_hop_id) << " watch port "
                << QuotedVar(app_db_entry.wcmp_group_members[i]->watch_port) << " does not match internal cache "
                << QuotedVar(wcmp_group_entry->wcmp_group_members[i]->watch_port) << " in wcmp manager.";
            return msg.str();
        }
        if (wcmp_group_entry->wcmp_group_members[i]->wcmp_group_id != app_db_entry.wcmp_group_members[i]->wcmp_group_id)
        {
            std::stringstream msg;
            msg << "WCMP group member " << QuotedVar(app_db_entry.wcmp_group_members[i]->next_hop_id) << " group ID "
                << QuotedVar(app_db_entry.wcmp_group_members[i]->wcmp_group_id) << " does not match internal cache "
                << QuotedVar(wcmp_group_entry->wcmp_group_members[i]->wcmp_group_id) << " in wcmp manager.";
            return msg.str();
        }
        sai_object_id_t nexthop_oid = SAI_NULL_OBJECT_ID;
        m_p4OidMapper->getOID(
            SAI_OBJECT_TYPE_NEXT_HOP,
            KeyGenerator::generateNextHopKey(
                wcmp_group_entry->wcmp_group_members[i]->next_hop_id),
            &nexthop_oid);
        if (wcmp_group_entry->wcmp_group_members[i]->next_hop_oid !=
            nexthop_oid) {
          std::stringstream msg;
          msg << "WCMP group member "
              << QuotedVar(app_db_entry.wcmp_group_members[i]->next_hop_id)
              << " has unmatched nexthop OID.";
          return msg.str();
        }
    }

    return "";
}

std::string WcmpManager::verifyStateAsicDb(P4WcmpGroupEntry* wcmp_group_entry) {
  swss::DBConnector db("ASIC_DB", 0);
  swss::Table table(&db, "ASIC_STATE");

  auto group_attrs = prepareSaiGroupAttrs(*wcmp_group_entry);
  std::vector<swss::FieldValueTuple> exp =
      saimeta::SaiAttributeList::serialize_attr_list(
          SAI_OBJECT_TYPE_NEXT_HOP_GROUP, (uint32_t)group_attrs.size(),
          group_attrs.data(), /*countOnly=*/false);
  std::string key = sai_serialize_object_type(SAI_OBJECT_TYPE_NEXT_HOP_GROUP) +
                    ":" +
                    sai_serialize_object_id(wcmp_group_entry->wcmp_group_oid);
  std::vector<swss::FieldValueTuple> values;
  if (!table.get(key, values)) {
    return std::string("ASIC DB key not found ") + key;
  }
  auto group_result =
      verifyAttrs(values, exp, std::vector<swss::FieldValueTuple>{},
                  /*allow_unknown=*/false);
  if (!group_result.empty()) {
    return group_result;
  }
  return "";
}

} // namespace p4orch
