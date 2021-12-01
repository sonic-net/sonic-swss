#include "p4orch/wcmp_manager.h"

#include <sstream>
#include <string>
#include <vector>

#include "crmorch.h"
#include "json.hpp"
#include "logger.h"
#include "p4orch/p4orch_util.h"
#include "portsorch.h"
#include "sai_serialize.h"
extern "C"
{
#include "sai.h"
}

extern sai_object_id_t gSwitchId;
extern sai_next_hop_group_api_t *sai_next_hop_group_api;
extern CrmOrch *gCrmOrch;
extern PortsOrch *gPortsOrch;

namespace p4orch
{

namespace
{

std::string getWcmpGroupMemberKey(const std::string &wcmp_group_key, const sai_object_id_t wcmp_member_oid)
{
    return wcmp_group_key + kTableKeyDelimiter + sai_serialize_object_id(wcmp_member_oid);
}

} // namespace

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
    auto status = validateWcmpGroupEntry(*app_db_entry);
    if (!status.ok())
    {
        SWSS_LOG_ERROR("Invalid WCMP group with id %s: %s", QuotedVar(app_db_entry->wcmp_group_id).c_str(),
                       status.message().c_str());
        return status;
    }
    status = createWcmpGroup(app_db_entry);
    if (!status.ok())
    {
        SWSS_LOG_ERROR("Failed to create WCMP group with id %s: %s", QuotedVar(app_db_entry->wcmp_group_id).c_str(),
                       status.message().c_str());
    }
    return status;
}

ReturnCode WcmpManager::createWcmpGroupMember(std::shared_ptr<P4WcmpGroupMemberEntry> wcmp_group_member,
                                              const sai_object_id_t group_oid, const std::string &wcmp_group_key)
{
    std::vector<sai_attribute_t> nhgm_attrs;
    sai_attribute_t nhgm_attr;
    sai_object_id_t next_hop_oid = SAI_NULL_OBJECT_ID;
    m_p4OidMapper->getOID(SAI_OBJECT_TYPE_NEXT_HOP, KeyGenerator::generateNextHopKey(wcmp_group_member->next_hop_id),
                          &next_hop_oid);

    nhgm_attr.id = SAI_NEXT_HOP_GROUP_MEMBER_ATTR_NEXT_HOP_GROUP_ID;
    nhgm_attr.value.oid = group_oid;
    nhgm_attrs.push_back(nhgm_attr);

    nhgm_attr.id = SAI_NEXT_HOP_GROUP_MEMBER_ATTR_NEXT_HOP_ID;
    nhgm_attr.value.oid = next_hop_oid;
    nhgm_attrs.push_back(nhgm_attr);

    nhgm_attr.id = SAI_NEXT_HOP_GROUP_MEMBER_ATTR_WEIGHT;
    nhgm_attr.value.u32 = (uint32_t)wcmp_group_member->weight;
    nhgm_attrs.push_back(nhgm_attr);

    CHECK_ERROR_AND_LOG_AND_RETURN(
        sai_next_hop_group_api->create_next_hop_group_member(&wcmp_group_member->member_oid, gSwitchId,
                                                             (uint32_t)nhgm_attrs.size(), nhgm_attrs.data()),
        "Failed to create next hop group member " << QuotedVar(wcmp_group_member->next_hop_id));

    // Update reference count
    const auto &next_hop_key = KeyGenerator::generateNextHopKey(wcmp_group_member->next_hop_id);
    m_p4OidMapper->setOID(SAI_OBJECT_TYPE_NEXT_HOP_GROUP_MEMBER,
                          getWcmpGroupMemberKey(wcmp_group_key, wcmp_group_member->member_oid),
                          wcmp_group_member->member_oid);
    gCrmOrch->incCrmResUsedCounter(CrmResourceType::CRM_NEXTHOP_GROUP_MEMBER);
    m_p4OidMapper->increaseRefCount(SAI_OBJECT_TYPE_NEXT_HOP, next_hop_key);
    m_p4OidMapper->increaseRefCount(SAI_OBJECT_TYPE_NEXT_HOP_GROUP, wcmp_group_key);

    return ReturnCode();
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

ReturnCode WcmpManager::createWcmpGroupMemberWithWatchport(P4WcmpGroupEntry *wcmp_group,
                                                           std::shared_ptr<P4WcmpGroupMemberEntry> member,
                                                           const std::string &wcmp_group_key)
{
    // Create member in SAI only for operationally up ports
    sai_port_oper_status_t oper_status = SAI_PORT_OPER_STATUS_DOWN;
    auto status = fetchPortOperStatus(member->watch_port, &oper_status);
    if (!status.ok())
    {
        return status;
    }

    if (oper_status == SAI_PORT_OPER_STATUS_UP)
    {
        auto status = createWcmpGroupMember(member, wcmp_group->wcmp_group_oid, wcmp_group_key);
        if (!status.ok())
        {
            SWSS_LOG_ERROR("Failed to create next hop member %s with watch_port %s", member->next_hop_id.c_str(),
                           member->watch_port.c_str());
            return status;
        }
    }
    else
    {
        pruned_wcmp_members_set.emplace(member);
        SWSS_LOG_NOTICE("Member %s in group %s not created in asic as the associated watchport "
                        "(%s) is not operationally up",
                        member->next_hop_id.c_str(), member->wcmp_group_id.c_str(), member->watch_port.c_str());
    }
    // Add member to port_name_to_wcmp_group_member_map
    insertMemberInPortNameToWcmpGroupMemberMap(member);
    return ReturnCode();
}

ReturnCode WcmpManager::processWcmpGroupMemberAddition(std::shared_ptr<P4WcmpGroupMemberEntry> member,
                                                       P4WcmpGroupEntry *wcmp_group, const std::string &wcmp_group_key)
{
    ReturnCode status = ReturnCode();
    if (!member->watch_port.empty())
    {
        status = createWcmpGroupMemberWithWatchport(wcmp_group, member, wcmp_group_key);
        if (!status.ok())
        {
            SWSS_LOG_ERROR("Failed to create WCMP group member %s with watch_port %s", member->next_hop_id.c_str(),
                           member->watch_port.c_str());
        }
    }
    else
    {
        status = createWcmpGroupMember(member, wcmp_group->wcmp_group_oid, wcmp_group_key);
        if (!status.ok())
        {
            SWSS_LOG_ERROR("Failed to create WCMP group member %s", member->next_hop_id.c_str());
        }
    }
    return status;
}

ReturnCode WcmpManager::processWcmpGroupMemberRemoval(std::shared_ptr<P4WcmpGroupMemberEntry> member,
                                                      const std::string &wcmp_group_key)
{
    // If member exists in pruned_wcmp_members_set, remove from set. Else, remove
    // member using SAI.
    auto it = pruned_wcmp_members_set.find(member);
    if (it != pruned_wcmp_members_set.end())
    {
        pruned_wcmp_members_set.erase(it);
        SWSS_LOG_NOTICE("Removed pruned member %s from group %s", member->next_hop_id.c_str(),
                        member->wcmp_group_id.c_str());
    }
    else
    {
        auto status = removeWcmpGroupMember(member, wcmp_group_key);
        if (!status.ok())
        {
            return status;
        }
    }
    // Remove member from port_name_to_wcmp_group_member_map
    removeMemberFromPortNameToWcmpGroupMemberMap(member);
    return ReturnCode();
}

ReturnCode WcmpManager::createWcmpGroup(P4WcmpGroupEntry *wcmp_group)
{
    SWSS_LOG_ENTER();
    // Create SAI next hop group
    sai_attribute_t nhg_attr;
    std::vector<sai_attribute_t> nhg_attrs;

    // TODO: Update type to WCMP when SAI supports it.
    nhg_attr.id = SAI_NEXT_HOP_GROUP_ATTR_TYPE;
    nhg_attr.value.s32 = SAI_NEXT_HOP_GROUP_TYPE_ECMP;
    nhg_attrs.push_back(nhg_attr);

    CHECK_ERROR_AND_LOG_AND_RETURN(sai_next_hop_group_api->create_next_hop_group(&wcmp_group->wcmp_group_oid, gSwitchId,
                                                                                 (uint32_t)nhg_attrs.size(),
                                                                                 nhg_attrs.data()),
                                   "Failed to create next hop group  " << QuotedVar(wcmp_group->wcmp_group_id));
    // Update reference count
    const auto &wcmp_group_key = KeyGenerator::generateWcmpGroupKey(wcmp_group->wcmp_group_id);
    gCrmOrch->incCrmResUsedCounter(CrmResourceType::CRM_NEXTHOP_GROUP);
    m_p4OidMapper->setOID(SAI_OBJECT_TYPE_NEXT_HOP_GROUP, wcmp_group_key, wcmp_group->wcmp_group_oid);

    // Create next hop group members
    std::vector<std::shared_ptr<P4WcmpGroupMemberEntry>> created_wcmp_group_members;
    ReturnCode status;
    for (auto &wcmp_group_member : wcmp_group->wcmp_group_members)
    {
        status = processWcmpGroupMemberAddition(wcmp_group_member, wcmp_group, wcmp_group_key);
        if (!status.ok())
        {
            break;
        }
        created_wcmp_group_members.push_back(wcmp_group_member);
    }
    if (!status.ok())
    {
        // Clean up created group members and the group
        recoverGroupMembers(wcmp_group, wcmp_group_key, created_wcmp_group_members, {});
        auto sai_status = sai_next_hop_group_api->remove_next_hop_group(wcmp_group->wcmp_group_oid);
        if (sai_status != SAI_STATUS_SUCCESS)
        {
            std::stringstream ss;
            ss << "Failed to delete WCMP group with id " << QuotedVar(wcmp_group->wcmp_group_id);
            SWSS_LOG_ERROR("%s SAI_STATUS: %s", ss.str().c_str(), sai_serialize_status(sai_status).c_str());
            SWSS_RAISE_CRITICAL_STATE(ss.str());
        }
        gCrmOrch->decCrmResUsedCounter(CrmResourceType::CRM_NEXTHOP_GROUP);
        m_p4OidMapper->eraseOID(SAI_OBJECT_TYPE_NEXT_HOP_GROUP, wcmp_group_key);
        return status;
    }
    m_wcmpGroupTable[wcmp_group->wcmp_group_id] = *wcmp_group;
    return ReturnCode();
}

void WcmpManager::recoverGroupMembers(
    p4orch::P4WcmpGroupEntry *wcmp_group_entry, const std::string &wcmp_group_key,
    const std::vector<std::shared_ptr<p4orch::P4WcmpGroupMemberEntry>> &created_wcmp_group_members,
    const std::vector<std::shared_ptr<p4orch::P4WcmpGroupMemberEntry>> &removed_wcmp_group_members)
{
    // Keep track of recovery status during clean up
    ReturnCode recovery_status;
    // Clean up created group members - remove created new members
    for (const auto &new_member : created_wcmp_group_members)
    {
        auto status = processWcmpGroupMemberRemoval(new_member, wcmp_group_key);
        if (!status.ok())
        {
            SWSS_LOG_ERROR("Failed to remove created next hop group member %s in "
                           "processUpdateRequest().",
                           QuotedVar(new_member->next_hop_id).c_str());
            recovery_status.ok() ? recovery_status = status.prepend("Error during recovery: ")
                                 : recovery_status << "; Error during recovery: " << status.message();
        }
    }
    // Clean up removed group members - create removed old members
    for (auto &old_member : removed_wcmp_group_members)
    {
        auto status = processWcmpGroupMemberAddition(old_member, wcmp_group_entry, wcmp_group_key);
        if (!status.ok())
        {
            recovery_status.ok() ? recovery_status = status.prepend("Error during recovery: ")
                                 : recovery_status << "; Error during recovery: " << status.message();
        }
    }
    if (!recovery_status.ok())
        SWSS_RAISE_CRITICAL_STATE(recovery_status.message());
}

ReturnCode WcmpManager::processUpdateRequest(P4WcmpGroupEntry *wcmp_group_entry)
{
    SWSS_LOG_ENTER();
    auto *old_wcmp = getWcmpGroupEntry(wcmp_group_entry->wcmp_group_id);
    wcmp_group_entry->wcmp_group_oid = old_wcmp->wcmp_group_oid;
    const auto &wcmp_group_key = KeyGenerator::generateWcmpGroupKey(wcmp_group_entry->wcmp_group_id);
    // Keep record of created next hop group members
    std::vector<std::shared_ptr<p4orch::P4WcmpGroupMemberEntry>> created_wcmp_group_members;
    // Keep record of removed next hop group members
    std::vector<std::shared_ptr<p4orch::P4WcmpGroupMemberEntry>> removed_wcmp_group_members;

    // Update group members steps:
    // 1. Find the old member in the list with the smallest weight
    // 2. Find the new member in the list with the smallest weight
    // 3. Make SAI calls to remove old members except the reserved member with the
    // smallest weight
    // 4. Make SAI call to create the new member with the smallest weight
    // 5. Make SAI call to remove the reserved old member
    // 6. Make SAI calls to create remaining new members
    ReturnCode update_request_status;
    auto find_smallest_index = [&](p4orch::P4WcmpGroupEntry *wcmp) {
        if (wcmp->wcmp_group_members.empty())
            return -1;
        int reserved_idx = 0;
        for (int i = 1; i < (int)wcmp->wcmp_group_members.size(); i++)
        {
            if (wcmp->wcmp_group_members[i]->weight < wcmp->wcmp_group_members[reserved_idx]->weight)
            {
                reserved_idx = i;
            }
        }
        return reserved_idx;
    };
    // Find the old member who has the smallest weight, -1 if the member list is
    // empty
    int reserved_old_member_index = find_smallest_index(old_wcmp);
    // Find the new member who has the smallest weight, -1 if the member list is
    // empty
    int reserved_new_member_index = find_smallest_index(wcmp_group_entry);

    // Remove stale group members except the member with the smallest weight
    for (int i = 0; i < (int)old_wcmp->wcmp_group_members.size(); i++)
    {
        // Reserve the old member with smallest weight
        if (i == reserved_old_member_index)
            continue;
        auto &stale_member = old_wcmp->wcmp_group_members[i];
        update_request_status = processWcmpGroupMemberRemoval(stale_member, wcmp_group_key);
        if (!update_request_status.ok())
        {
            SWSS_LOG_ERROR("Failed to remove stale next hop group member %s in "
                           "processUpdateRequest().",
                           QuotedVar(sai_serialize_object_id(stale_member->member_oid)).c_str());
            recoverGroupMembers(wcmp_group_entry, wcmp_group_key, created_wcmp_group_members,
                                removed_wcmp_group_members);
            return update_request_status;
        }
        removed_wcmp_group_members.push_back(stale_member);
    }

    // Create the new member with the smallest weight if member list is nonempty
    if (!wcmp_group_entry->wcmp_group_members.empty())
    {
        auto &member = wcmp_group_entry->wcmp_group_members[reserved_new_member_index];
        update_request_status = processWcmpGroupMemberAddition(member, wcmp_group_entry, wcmp_group_key);
        if (!update_request_status.ok())
        {
            recoverGroupMembers(wcmp_group_entry, wcmp_group_key, created_wcmp_group_members,
                                removed_wcmp_group_members);
            return update_request_status;
        }
        created_wcmp_group_members.push_back(member);
    }

    // Remove the old member with the smallest weight if member list is nonempty
    if (!old_wcmp->wcmp_group_members.empty())
    {
        auto &stale_member = old_wcmp->wcmp_group_members[reserved_old_member_index];
        update_request_status = processWcmpGroupMemberRemoval(stale_member, wcmp_group_key);
        if (!update_request_status.ok())
        {
            SWSS_LOG_ERROR("Failed to remove stale next hop group member %s in "
                           "processUpdateRequest().",
                           QuotedVar(sai_serialize_object_id(stale_member->member_oid)).c_str());
            recoverGroupMembers(wcmp_group_entry, wcmp_group_key, created_wcmp_group_members,
                                removed_wcmp_group_members);
            return update_request_status;
        }
        removed_wcmp_group_members.push_back(stale_member);
    }

    // Create new group members
    for (int i = 0; i < (int)wcmp_group_entry->wcmp_group_members.size(); i++)
    {
        // Skip the new member with the lowest weight as it is already created
        if (i == reserved_new_member_index)
            continue;
        auto &member = wcmp_group_entry->wcmp_group_members[i];
        // Create new group member
        update_request_status = processWcmpGroupMemberAddition(member, wcmp_group_entry, wcmp_group_key);
        if (!update_request_status.ok())
        {
            recoverGroupMembers(wcmp_group_entry, wcmp_group_key, created_wcmp_group_members,
                                removed_wcmp_group_members);
            return update_request_status;
        }
        created_wcmp_group_members.push_back(member);
    }

    m_wcmpGroupTable[wcmp_group_entry->wcmp_group_id] = *wcmp_group_entry;
    return update_request_status;
}

ReturnCode WcmpManager::removeWcmpGroupMember(const std::shared_ptr<P4WcmpGroupMemberEntry> wcmp_group_member,
                                              const std::string &wcmp_group_key)
{
    SWSS_LOG_ENTER();
    const std::string &next_hop_key = KeyGenerator::generateNextHopKey(wcmp_group_member->next_hop_id);

    CHECK_ERROR_AND_LOG_AND_RETURN(sai_next_hop_group_api->remove_next_hop_group_member(wcmp_group_member->member_oid),
                                   "Failed to remove WCMP group member with nexthop id "
                                       << QuotedVar(wcmp_group_member->next_hop_id));
    m_p4OidMapper->eraseOID(SAI_OBJECT_TYPE_NEXT_HOP_GROUP_MEMBER,
                            getWcmpGroupMemberKey(wcmp_group_key, wcmp_group_member->member_oid));
    gCrmOrch->decCrmResUsedCounter(CrmResourceType::CRM_NEXTHOP_GROUP_MEMBER);
    m_p4OidMapper->decreaseRefCount(SAI_OBJECT_TYPE_NEXT_HOP, next_hop_key);
    m_p4OidMapper->decreaseRefCount(SAI_OBJECT_TYPE_NEXT_HOP_GROUP, wcmp_group_key);
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
    uint32_t expected_refcount = (uint32_t)wcmp_group->wcmp_group_members.size();
    uint32_t wcmp_group_refcount = 0;
    const auto &wcmp_group_key = KeyGenerator::generateWcmpGroupKey(wcmp_group_id);
    m_p4OidMapper->getRefCount(SAI_OBJECT_TYPE_NEXT_HOP_GROUP, wcmp_group_key, &wcmp_group_refcount);
    if (wcmp_group_refcount > expected_refcount)
    {
        LOG_ERROR_AND_RETURN(ReturnCode(StatusCode::SWSS_RC_IN_USE)
                             << "Failed to remove WCMP group with id " << QuotedVar(wcmp_group_id) << ", as it has "
                             << wcmp_group_refcount - expected_refcount << " more objects than its group members (size="
                             << expected_refcount << ") referencing it.");
    }
    std::vector<std::shared_ptr<p4orch::P4WcmpGroupMemberEntry>> removed_wcmp_group_members;
    ReturnCode status;
    // Delete group members
    for (const auto &member : wcmp_group->wcmp_group_members)
    {
        status = processWcmpGroupMemberRemoval(member, wcmp_group_key);
        if (!status.ok())
        {
            break;
        }
        removed_wcmp_group_members.push_back(member);
    }
    // Delete group
    if (status.ok())
    {
        auto sai_status = sai_next_hop_group_api->remove_next_hop_group(wcmp_group->wcmp_group_oid);
        if (sai_status == SAI_STATUS_SUCCESS)
        {
            m_p4OidMapper->eraseOID(SAI_OBJECT_TYPE_NEXT_HOP_GROUP, wcmp_group_key);
            gCrmOrch->decCrmResUsedCounter(CrmResourceType::CRM_NEXTHOP_GROUP);
            m_wcmpGroupTable.erase(wcmp_group->wcmp_group_id);
            return ReturnCode();
        }
        status = ReturnCode(sai_status) << "Failed to delete WCMP group with id "
                                        << QuotedVar(wcmp_group->wcmp_group_id);
        SWSS_LOG_ERROR("%s SAI_STATUS: %s", status.message().c_str(), sai_serialize_status(sai_status).c_str());
    }
    // Recover group members.
    recoverGroupMembers(wcmp_group, wcmp_group_key, {}, removed_wcmp_group_members);
    return status;
}

void WcmpManager::pruneNextHops(const std::string &port)
{
    SWSS_LOG_ENTER();

    // Get list of WCMP group members associated with the watch_port
    if (port_name_to_wcmp_group_member_map.find(port) != port_name_to_wcmp_group_member_map.end())
    {
        for (const auto &member : port_name_to_wcmp_group_member_map[port])
        {
            auto it = pruned_wcmp_members_set.find(member);
            // Prune a member if it is not already pruned.
            if (it == pruned_wcmp_members_set.end())
            {
                const auto &wcmp_group_key = KeyGenerator::generateWcmpGroupKey(member->wcmp_group_id);
                auto status = removeWcmpGroupMember(member, wcmp_group_key);
                if (!status.ok())
                {
                    SWSS_LOG_NOTICE("Failed to remove member %s from group %s, rv: %s", member->next_hop_id.c_str(),
                                    member->wcmp_group_id.c_str(), status.message().c_str());
                }
                else
                {
                    // Add pruned member to pruned set
                    pruned_wcmp_members_set.emplace(member);
                    SWSS_LOG_NOTICE("Pruned member %s from group %s", member->next_hop_id.c_str(),
                                    member->wcmp_group_id.c_str());
                }
            }
        }
    }
}

void WcmpManager::restorePrunedNextHops(const std::string &port)
{
    SWSS_LOG_ENTER();

    //  Get list of WCMP group members associated with the watch_port that were
    //  pruned
    if (port_name_to_wcmp_group_member_map.find(port) != port_name_to_wcmp_group_member_map.end())
    {
        ReturnCode status;
        for (auto member : port_name_to_wcmp_group_member_map[port])
        {
            auto it = pruned_wcmp_members_set.find(member);
            if (it != pruned_wcmp_members_set.end())
            {
                const auto &wcmp_group_key = KeyGenerator::generateWcmpGroupKey(member->wcmp_group_id);
                sai_object_id_t wcmp_group_oid = SAI_NULL_OBJECT_ID;
                if (!m_p4OidMapper->getOID(SAI_OBJECT_TYPE_NEXT_HOP_GROUP, wcmp_group_key, &wcmp_group_oid))
                {
                    status = ReturnCode(StatusCode::SWSS_RC_INTERNAL)
                             << "Error during restoring pruned next hop: Failed to get "
                                "WCMP group OID for group "
                             << member->wcmp_group_id;
                    SWSS_LOG_ERROR("%s", status.message().c_str());
                    SWSS_RAISE_CRITICAL_STATE(status.message());
                    return;
                }
                status = createWcmpGroupMember(member, wcmp_group_oid, wcmp_group_key);
                if (!status.ok())
                {
                    status.prepend("Error during restoring pruned next hop: ");
                    SWSS_LOG_ERROR("%s", status.message().c_str());
                    SWSS_RAISE_CRITICAL_STATE(status.message());
                    return;
                }
                pruned_wcmp_members_set.erase(it);
                SWSS_LOG_NOTICE("Restored pruned member %s in group %s", member->next_hop_id.c_str(),
                                member->wcmp_group_id.c_str());
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

void WcmpManager::enqueue(const swss::KeyOpFieldsValuesTuple &entry)
{
    m_entries.push_back(entry);
}

void WcmpManager::drain()
{
    SWSS_LOG_ENTER();

    for (const auto &key_op_fvs_tuple : m_entries)
    {
        std::string table_name;
        std::string db_key;
        parseP4RTKey(kfvKey(key_op_fvs_tuple), &table_name, &db_key);
        const std::vector<swss::FieldValueTuple> &attributes = kfvFieldsValues(key_op_fvs_tuple);

        ReturnCode status;
        auto app_db_entry_or = deserializeP4WcmpGroupAppDbEntry(db_key, attributes);
        if (!app_db_entry_or.ok())
        {
            status = app_db_entry_or.status();
            SWSS_LOG_ERROR("Unable to deserialize APP DB WCMP group entry with key %s: %s",
                           QuotedVar(table_name + ":" + db_key).c_str(), status.message().c_str());
            m_publisher->publish(APP_P4RT_TABLE_NAME, kfvKey(key_op_fvs_tuple), kfvFieldsValues(key_op_fvs_tuple),
                                 status,
                                 /*replace=*/true);
            continue;
        }
        auto &app_db_entry = *app_db_entry_or;

        const std::string &operation = kfvOp(key_op_fvs_tuple);
        if (operation == SET_COMMAND)
        {
            auto *wcmp_group_entry = getWcmpGroupEntry(app_db_entry.wcmp_group_id);
            if (wcmp_group_entry == nullptr)
            {
                // Create WCMP group
                status = processAddRequest(&app_db_entry);
            }
            else
            {
                // Modify existing WCMP group
                status = processUpdateRequest(&app_db_entry);
            }
        }
        else if (operation == DEL_COMMAND)
        {
            // Delete WCMP group
            status = removeWcmpGroup(app_db_entry.wcmp_group_id);
        }
        else
        {
            status = ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
                     << "Unknown operation type: " << QuotedVar(operation) << " for WCMP group entry with key "
                     << QuotedVar(table_name) << ":" << QuotedVar(db_key)
                     << "; only SET and DEL operations are allowed.";
            SWSS_LOG_ERROR("Unknown operation type %s\n", QuotedVar(operation).c_str());
        }
        m_publisher->publish(APP_P4RT_TABLE_NAME, kfvKey(key_op_fvs_tuple), kfvFieldsValues(key_op_fvs_tuple), status,
                             /*replace=*/true);
    }
    m_entries.clear();
}

} // namespace p4orch
