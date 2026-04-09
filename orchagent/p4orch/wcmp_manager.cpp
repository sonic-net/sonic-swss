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

}  // namespace

ReturnCode WcmpManager::validateWcmpGroupEntry(
    const P4WcmpGroupEntry& app_db_entry, const std::string& operation) {
  if (operation == SET_COMMAND) {
    for (auto& wcmp_group_member : app_db_entry.wcmp_group_members) {
      if (wcmp_group_member->weight <= 0) {
        return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
               << "Invalid WCMP group member weight "
               << wcmp_group_member->weight << ": should be greater than 0.";
      }
      sai_object_id_t nexthop_oid = SAI_NULL_OBJECT_ID;
      if (!m_p4OidMapper->getOID(
              SAI_OBJECT_TYPE_NEXT_HOP,
              KeyGenerator::generateNextHopKey(wcmp_group_member->next_hop_id),
              &nexthop_oid)) {
        return ReturnCode(StatusCode::SWSS_RC_NOT_FOUND)
               << "Nexthop id " << QuotedVar(wcmp_group_member->next_hop_id)
               << " does not exist for WCMP group "
               << QuotedVar(app_db_entry.wcmp_group_id);
      }
      if (!wcmp_group_member->watch_port.empty()) {
        Port port;
        if (!gPortsOrch->getPort(wcmp_group_member->watch_port, port) ||
            !gPortsOrch->isFrontPanelPort(port)) {
          return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
                 << "Invalid watch_port field " << wcmp_group_member->watch_port
                 << ": should be a valid front panel port name.";
        }
      }
      }
      } else if (operation == DEL_COMMAND) {
        bool exist = (getWcmpGroupEntry(app_db_entry.wcmp_group_id) != nullptr);
       if (!exist) {
            return ReturnCode(StatusCode::SWSS_RC_NOT_FOUND)
                   << "WCMP group with id " << QuotedVar(app_db_entry.wcmp_group_id)
                   << " was not found.";
        }
        uint32_t wcmp_group_refcount = 0;
    const auto& wcmp_group_key =
        KeyGenerator::generateWcmpGroupKey(app_db_entry.wcmp_group_id);
    if (!m_p4OidMapper->getRefCount(SAI_OBJECT_TYPE_NEXT_HOP_GROUP,
                                    wcmp_group_key, &wcmp_group_refcount)) {
      RETURN_INTERNAL_ERROR_AND_RAISE_CRITICAL(
          "Failed to get reference count of WCMP group with id "
          << QuotedVar(app_db_entry.wcmp_group_id));
    }
    if (wcmp_group_refcount > 0) {
      return ReturnCode(StatusCode::SWSS_RC_IN_USE)
             << "Failed to remove WCMP group with id "
             << QuotedVar(app_db_entry.wcmp_group_id) << ", non-zero ref count "
             << wcmp_group_refcount;
        }
  } else {
    return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
           << "Unknown operation type " << QuotedVar(operation);
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

void WcmpManager::insertMemberInPortNameToWcmpGroupMemberMap(std::shared_ptr<P4WcmpGroupMemberEntry> member)
{
    m_port_name_to_wcmp_group_member_map[member->watch_port].insert(member);
}

void WcmpManager::removeMemberFromPortNameToWcmpGroupMemberMap(std::shared_ptr<P4WcmpGroupMemberEntry> member)
{
    m_port_name_to_wcmp_group_member_map[member->watch_port].erase(member);
}

void WcmpManager::fetchPortOperStatus(const P4WcmpGroupEntry& wcmp_group) {
  SWSS_LOG_ENTER();

    for (auto& member : wcmp_group.wcmp_group_members) {
      if (!member->watch_port.empty()) {
        sai_port_oper_status_t oper_status = SAI_PORT_OPER_STATUS_DOWN;
        Port port;
        if (gPortsOrch->getPort(member->watch_port, port)) {
          oper_status = port.m_oper_status;
        }
        m_port_oper_status_map[member->watch_port] = oper_status;
      }
    }
    return;
}

void WcmpManager::populateMemberInfo(P4WcmpGroupEntry& wcmp_group) {
  SWSS_LOG_ENTER();

  for (auto& member : wcmp_group.wcmp_group_members) {
    if (!member->watch_port.empty()) {
      if (m_port_oper_status_map[member->watch_port] !=
          SAI_PORT_OPER_STATUS_UP) {
        member->pruned = true;
      } else {
        member->pruned = false;
      }
    }

    if (member->next_hop_oid == SAI_NULL_OBJECT_ID) {
      m_p4OidMapper->getOID(
          SAI_OBJECT_TYPE_NEXT_HOP,
          KeyGenerator::generateNextHopKey(member->next_hop_id),
          &(member->next_hop_oid));
    }
  }
  return;
}

std::vector<ReturnCode> WcmpManager::createWcmpGroups(
    std::vector<P4WcmpGroupEntry>& entries)
{
    SWSS_LOG_ENTER();
    std::vector<std::vector<sai_attribute_t>> sai_attrs(entries.size());
  std::vector<uint32_t> attrs_cnt(entries.size());
  std::vector<const sai_attribute_t*> attrs_ptr(entries.size());
  std::vector<sai_object_id_t> oids(entries.size());
  std::vector<sai_status_t> object_statuses(entries.size());
  std::vector<ReturnCode> statuses(entries.size());

  for (size_t i = 0; i < entries.size(); ++i) {
    sai_attrs[i] = prepareSaiGroupAttrs(entries[i]);
    attrs_cnt[i] = static_cast<uint32_t>(sai_attrs[i].size());
    attrs_ptr[i] = sai_attrs[i].data();
  }
  sai_next_hop_group_api->create_next_hop_groups(
      gSwitchId, static_cast<uint32_t>(entries.size()), attrs_cnt.data(),
      attrs_ptr.data(), SAI_BULK_OP_ERROR_MODE_STOP_ON_ERROR, oids.data(),
      object_statuses.data());

  for (size_t i = 0; i < entries.size(); ++i) {
    CHECK_ERROR_AND_LOG(object_statuses[i],
                        "Failed to create next hop group  "
                            << QuotedVar(entries[i].wcmp_group_id));

    if (object_statuses[i] == SAI_STATUS_SUCCESS) {
      entries[i].wcmp_group_oid = oids[i];
      const auto& wcmp_group_key =
          KeyGenerator::generateWcmpGroupKey(entries[i].wcmp_group_id);
      gCrmOrch->incCrmResUsedCounter(CrmResourceType::CRM_NEXTHOP_GROUP);
      m_p4OidMapper->setOID(SAI_OBJECT_TYPE_NEXT_HOP_GROUP, wcmp_group_key,
                            entries[i].wcmp_group_oid);
      for (auto& member : entries[i].wcmp_group_members) {
        const std::string& next_hop_key =
            KeyGenerator::generateNextHopKey(member->next_hop_id);
        gCrmOrch->incCrmResUsedCounter(
            CrmResourceType::CRM_NEXTHOP_GROUP_MEMBER);
        m_p4OidMapper->increaseRefCount(SAI_OBJECT_TYPE_NEXT_HOP, next_hop_key);
        if (!member->watch_port.empty()) {
          insertMemberInPortNameToWcmpGroupMemberMap(member);
        }
      }
      m_wcmpGroupTable[entries[i].wcmp_group_id] = entries[i];
      statuses[i] = ReturnCode();
    } else {
      statuses[i] = ReturnCode(object_statuses[i])
                    << "Failed to create next hop group  "
                    << QuotedVar(entries[i].wcmp_group_id);
      }
    }
    return statuses;
}

std::vector<ReturnCode> WcmpManager::removeWcmpGroups(
    const std::vector<P4WcmpGroupEntry>& entries)
{
    SWSS_LOG_ENTER();

    std::vector<P4WcmpGroupEntry*> entry_ptrs(entries.size());
  std::vector<sai_object_id_t> oids(entries.size());
  std::vector<sai_status_t> object_statuses(entries.size());
  std::vector<ReturnCode> statuses(entries.size());

  for (size_t i = 0; i < entries.size(); ++i) {
    entry_ptrs[i] = getWcmpGroupEntry(entries[i].wcmp_group_id);
    oids[i] = entry_ptrs[i]->wcmp_group_oid;
  }
  sai_next_hop_group_api->remove_next_hop_groups(
      static_cast<uint32_t>(entries.size()), oids.data(),
      SAI_BULK_OP_ERROR_MODE_STOP_ON_ERROR, object_statuses.data());

  for (size_t i = 0; i < entries.size(); ++i) {
    CHECK_ERROR_AND_LOG(object_statuses[i],
                        "Failed to delete WCMP group with id "
                            << QuotedVar(entries[i].wcmp_group_id));

    if (object_statuses[i] == SAI_STATUS_SUCCESS) {
      const auto& wcmp_group_key =
          KeyGenerator::generateWcmpGroupKey(entry_ptrs[i]->wcmp_group_id);
      m_p4OidMapper->eraseOID(SAI_OBJECT_TYPE_NEXT_HOP_GROUP, wcmp_group_key);
      gCrmOrch->decCrmResUsedCounter(CrmResourceType::CRM_NEXTHOP_GROUP);
      for (auto& member : entry_ptrs[i]->wcmp_group_members) {
        const std::string& next_hop_key =
            KeyGenerator::generateNextHopKey(member->next_hop_id);
        gCrmOrch->decCrmResUsedCounter(
            CrmResourceType::CRM_NEXTHOP_GROUP_MEMBER);
        m_p4OidMapper->decreaseRefCount(SAI_OBJECT_TYPE_NEXT_HOP, next_hop_key);
        if (!member->watch_port.empty()) {
          removeMemberFromPortNameToWcmpGroupMemberMap(member);
        }
      }
      m_wcmpGroupTable.erase(entry_ptrs[i]->wcmp_group_id);
      statuses[i] = ReturnCode();
    } else {
      statuses[i] = ReturnCode(object_statuses[i])
                    << "Failed to delete WCMP group with id "
                    << QuotedVar(entries[i].wcmp_group_id);
      }
    }

    return statuses;
}

std::vector<ReturnCode> WcmpManager::updateWcmpGroups(
    std::vector<P4WcmpGroupEntry>& entries, bool watchport) {
  SWSS_LOG_ENTER();

  // Each group update will have exactly two SAI attrs:
  // SAI_NEXT_HOP_GROUP_ATTR_NEXT_HOP_LIST and
  // SAI_NEXT_HOP_GROUP_ATTR_NEXT_HOP_MEMBER_WEIGHT_LIST.
  std::vector<P4WcmpGroupEntry*> old_entries(entries.size());
  std::vector<sai_attribute_t> sai_attr(2 * entries.size());
  std::vector<sai_object_id_t> oids(2 * entries.size());
  std::vector<sai_status_t> object_statuses(2 * entries.size());
  std::vector<ReturnCode> statuses(entries.size());
  // Index that points to the entry that needs to make SAI calls.
  // -1 indicates that the entry does not make any SAI call due to no change in
  // any SAI attr.
  std::vector<int> indice(entries.size());
  int index = 0;

  for (size_t i = 0; i < entries.size(); ++i) {
    old_entries[i] = getWcmpGroupEntry(entries[i].wcmp_group_id);
    entries[i].wcmp_group_oid = old_entries[i]->wcmp_group_oid;
    auto attrs = prepareSaiGroupAttrs(entries[i], /*update=*/true);
    // Only make the SAI call if SAI attributes change.
    if (entries[i].nexthop_ids != old_entries[i]->nexthop_ids ||
        entries[i].nexthop_weights != old_entries[i]->nexthop_weights) {
      // Since each group update has exactly two SAI attrs, the SAI attr indice
      // will be 2 * index and 2 * index + 1 from the entry index.
      int sai_attr_index = 2 * index;
      oids[sai_attr_index] = old_entries[i]->wcmp_group_oid;
      oids[sai_attr_index + 1] = old_entries[i]->wcmp_group_oid;
      sai_attr[sai_attr_index] = attrs[0];
      sai_attr[sai_attr_index + 1] = attrs[1];
      indice[i] = index++;
    } else {
      indice[i] = -1;
    }
  }

  if (index > 0) {
    // This SAI operation is assumed to be atomic.
    sai_next_hop_group_api->set_next_hop_groups_attribute(
        static_cast<uint32_t>(2 * index), oids.data(), sai_attr.data(),
        SAI_BULK_OP_ERROR_MODE_STOP_ON_ERROR, object_statuses.data());
  }

    for (size_t i = 0; i < entries.size(); ++i) {
    int sai_entry_index = indice[i];
    int sai_attr_index = 2 * sai_entry_index + 1;
    if (sai_entry_index == -1 ||
        object_statuses[sai_attr_index] == SAI_STATUS_SUCCESS) {
      // Do not update reference for watchport update.
      if (!watchport) {
        for (auto& member : old_entries[i]->wcmp_group_members) {
          const std::string& next_hop_key =
              KeyGenerator::generateNextHopKey(member->next_hop_id);
          gCrmOrch->decCrmResUsedCounter(
              CrmResourceType::CRM_NEXTHOP_GROUP_MEMBER);
          m_p4OidMapper->decreaseRefCount(SAI_OBJECT_TYPE_NEXT_HOP,
                                          next_hop_key);
          if (!member->watch_port.empty()) {
            removeMemberFromPortNameToWcmpGroupMemberMap(member);
          }
        }
      for (auto& member : entries[i].wcmp_group_members) {
          const std::string& next_hop_key =
              KeyGenerator::generateNextHopKey(member->next_hop_id);
          gCrmOrch->incCrmResUsedCounter(
              CrmResourceType::CRM_NEXTHOP_GROUP_MEMBER);
          m_p4OidMapper->increaseRefCount(SAI_OBJECT_TYPE_NEXT_HOP,
                                          next_hop_key);
          if (!member->watch_port.empty()) {
            insertMemberInPortNameToWcmpGroupMemberMap(member);
          }
        }
      }
      m_wcmpGroupTable[entries[i].wcmp_group_id] = entries[i];
      statuses[i] = ReturnCode();
    } else {
      CHECK_ERROR_AND_LOG(object_statuses[sai_attr_index],
                          "Failed to update WCMP group with id "
                              << QuotedVar(entries[i].wcmp_group_id));
      statuses[i] = ReturnCode(object_statuses[sai_attr_index])
                    << "Failed to update WCMP group with id "
                    << QuotedVar(entries[i].wcmp_group_id);
      }
    }

    return statuses;
}

ReturnCode WcmpManager::processEntries(
    std::vector<P4WcmpGroupEntry>& entries,
    const std::vector<swss::KeyOpFieldsValuesTuple>& tuple_list,
    const std::string& op, bool update)
{
    SWSS_LOG_ENTER();

    ReturnCode status;
  std::vector<ReturnCode> statuses;
  // In syncd, bulk SAI calls use mode SAI_BULK_OP_ERROR_MODE_STOP_ON_ERROR.
  if (op == SET_COMMAND) {
    if (!update) {
      statuses = createWcmpGroups(entries);
    } else {
      statuses = updateWcmpGroups(entries);
    }
  } else {
    statuses = removeWcmpGroups(entries);
  }
  for (size_t i = 0; i < entries.size(); ++i) {
    m_publisher->publish(APP_P4RT_TABLE_NAME, kfvKey(tuple_list[i]),
                         kfvFieldsValues(tuple_list[i]), statuses[i],
                         /*replace=*/true);
    if (status.ok()) {
      // Remove the queued watchport event if the group has been updated.
      m_watchport_groups.erase(entries[i].wcmp_group_id);
      if (!statuses[i].ok()) {
        status = statuses[i];
      }
      }
    }

    return status;
}

void WcmpManager::updateWatchPort(const std::string& port,
                                  sai_port_oper_status_t status) {
  SWSS_LOG_ENTER();

  m_port_oper_status_map[port] = status;
  if (m_port_name_to_wcmp_group_member_map.find(port) ==
      m_port_name_to_wcmp_group_member_map.end()) {
    return;
  }
  bool prune = status != SAI_PORT_OPER_STATUS_UP;
  for (auto& member : m_port_name_to_wcmp_group_member_map[port]) {
    if (member->pruned != prune) {
      auto* wcmp_group = getWcmpGroupEntry(member->wcmp_group_id);
      if (wcmp_group == nullptr) {
        SWSS_RAISE_CRITICAL_STATE("Failed to find WCMP group " +
                                  QuotedVar(member->wcmp_group_id) +
                                  " in updateWatchPort");
      } else {
        const std::string update = prune ? "prune" : "restore";
        m_watchport_groups.insert(member->wcmp_group_id);
        SWSS_LOG_NOTICE("Queued watchport event: %s member %s from group %s",
                        update.c_str(), member->next_hop_id.c_str(),
                        member->wcmp_group_id.c_str());
      }
    }
  }
  if (!m_watchport_groups.empty()) {
    m_watchport_event->notify();
  }
}

void WcmpManager::processWatchPortEvent() {
  SWSS_LOG_ENTER();

  uint count = 0;
  P4WcmpGroupEntry* entry;
  std::vector<P4WcmpGroupEntry> entries;
  auto it = m_watchport_groups.begin();
  while (it != m_watchport_groups.end()) {
    entry = getWcmpGroupEntry(*it);
    populateMemberInfo(*entry);
    entries.push_back(*entry);
    it = m_watchport_groups.erase(it);
    if (++count >= kMaxWatchportGroupBulkSize) {
      break;
    }
  }
  if (!entries.empty()) {
    auto status = updateWcmpGroups(entries, /*watchport=*/true);
    for (size_t i = 0; i < entries.size(); ++i) {
      if (!status[i].ok()) {
        SWSS_RAISE_CRITICAL_STATE(
            "Failed to process watchport event for group " +
            QuotedVar(entries[i].wcmp_group_id));
      } else {
        SWSS_LOG_NOTICE("Watchport event processed for group %s",
                        entries[i].wcmp_group_id.c_str());
      }
    }
  }

  if (!m_watchport_groups.empty()) {
    m_watchport_event->notify();
  }
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

  std::vector<P4WcmpGroupEntry> entry_list;
  std::vector<swss::KeyOpFieldsValuesTuple> tuple_list;

  ReturnCode status;
  std::string prev_op;
  bool prev_update = false;
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
    bool update = (getWcmpGroupEntry(app_db_entry.wcmp_group_id) != nullptr);

    status = validateWcmpGroupEntry(app_db_entry, operation);
    if (!status.ok()) {
      SWSS_LOG_ERROR("Validation failed for WCMP group with id %s: %s",
                     QuotedVar(app_db_entry.wcmp_group_id).c_str(),
                     status.message().c_str());
      m_publisher->publish(APP_P4RT_TABLE_NAME, kfvKey(key_op_fvs_tuple),
                           kfvFieldsValues(key_op_fvs_tuple), status,
                           /*replace=*/true);
      break;
    }

    if (operation == SET_COMMAND) {
      fetchPortOperStatus(app_db_entry);
      populateMemberInfo(app_db_entry);
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
      // Return SWSS_RC_NOT_EXECUTED if failure has occured.
      m_publisher->publish(APP_P4RT_TABLE_NAME, kfvKey(key_op_fvs_tuple),
                           kfvFieldsValues(key_op_fvs_tuple),
                           ReturnCode(StatusCode::SWSS_RC_NOT_EXECUTED),
                           /*replace=*/true);
      break;
    } else {
      entry_list.push_back(app_db_entry);
      tuple_list.push_back(key_op_fvs_tuple);
    }
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
    ReturnCode status = validateWcmpGroupEntry(app_db_entry, SET_COMMAND);
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

void WcmpManager::refreshPortOperStatus() {
  for (const auto& it : m_port_oper_status_map) {
    Port port;
    if (!gPortsOrch->getPort(it.first, port)) {
      SWSS_LOG_ERROR("Failed to get port object for port %s", it.first.c_str());
      continue;
    }
    updateWatchPort(it.first, port.m_oper_status);
  }
}

} // namespace p4orch
