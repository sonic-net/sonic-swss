#include "p4orch/wcmp_manager.h"

#include <sstream>
#include <string>
#include <vector>

#include "SaiAttributeList.h"
#include "crmorch.h"
#include "dbconnector.h"
#include "json.hpp"
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
extern PortsOrch *gPortsOrch;
extern size_t gMaxBulkSize;

namespace p4orch
{

namespace
{

std::string getWcmpGroupMemberKey(const std::string &wcmp_group_key, const sai_object_id_t wcmp_member_oid)
{
    return wcmp_group_key + kTableKeyDelimiter + sai_serialize_object_id(wcmp_member_oid);
}

std::vector<sai_attribute_t> getSaiGroupAttrs(const P4WcmpGroupEntry &wcmp_group_entry)
{
    std::vector<sai_attribute_t> attrs;
    sai_attribute_t attr;

    // TODO: Update type to WCMP when SAI supports it.
    attr.id = SAI_NEXT_HOP_GROUP_ATTR_TYPE;
    attr.value.s32 = SAI_NEXT_HOP_GROUP_TYPE_ECMP;
    attrs.push_back(attr);

    return attrs;
}

} // namespace

WcmpManager::WcmpManager(P4OidMapper *p4oidMapper, ResponsePublisherInterface *publisher)
    : gNextHopGroupMemberBulker(sai_next_hop_group_api, gSwitchId, gMaxBulkSize)
{
    SWSS_LOG_ENTER();

    assert(p4oidMapper != nullptr);
    m_p4OidMapper = p4oidMapper;
    assert(publisher != nullptr);
    m_publisher = publisher;
}

std::vector<sai_attribute_t> WcmpManager::getSaiMemberAttrs(const P4WcmpGroupMemberEntry &wcmp_member_entry,
                                                            const sai_object_id_t group_oid)
{
    std::vector<sai_attribute_t> attrs;
    sai_attribute_t attr;
    sai_object_id_t next_hop_oid = SAI_NULL_OBJECT_ID;
    m_p4OidMapper->getOID(SAI_OBJECT_TYPE_NEXT_HOP, KeyGenerator::generateNextHopKey(wcmp_member_entry.next_hop_id),
                          &next_hop_oid);

    attr.id = SAI_NEXT_HOP_GROUP_MEMBER_ATTR_NEXT_HOP_GROUP_ID;
    attr.value.oid = group_oid;
    attrs.push_back(attr);

    attr.id = SAI_NEXT_HOP_GROUP_MEMBER_ATTR_NEXT_HOP_ID;
    attr.value.oid = next_hop_oid;
    attrs.push_back(attr);

    attr.id = SAI_NEXT_HOP_GROUP_MEMBER_ATTR_WEIGHT;
    attr.value.u32 = (uint32_t)wcmp_member_entry.weight;
    attrs.push_back(attr);

    return attrs;
}

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

ReturnCode WcmpManager::createWcmpGroupMember(std::shared_ptr<P4WcmpGroupMemberEntry> wcmp_group_member,
                                              const sai_object_id_t group_oid, const std::string &wcmp_group_key)
{
    auto attrs = getSaiMemberAttrs(*wcmp_group_member, group_oid);

    CHECK_ERROR_AND_LOG_AND_RETURN(
        sai_next_hop_group_api->create_next_hop_group_member(&wcmp_group_member->member_oid, gSwitchId,
                                                             (uint32_t)attrs.size(), attrs.data()),
        "Failed to create next hop group member " << QuotedVar(wcmp_group_member->next_hop_id));

    // Update reference count
    m_p4OidMapper->setOID(SAI_OBJECT_TYPE_NEXT_HOP_GROUP_MEMBER,
                          getWcmpGroupMemberKey(wcmp_group_key, wcmp_group_member->member_oid),
                          wcmp_group_member->member_oid);
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

ReturnCode WcmpManager::processWcmpGroupMembersAddition(
    const std::vector<std::shared_ptr<P4WcmpGroupMemberEntry>> &members, const std::string &wcmp_group_key,
    sai_object_id_t wcmp_group_oid, std::vector<std::shared_ptr<P4WcmpGroupMemberEntry>> &created_wcmp_group_members)
{
    SWSS_LOG_ENTER();
    ReturnCode status;
    vector<sai_object_id_t> nhgm_ids(members.size(), SAI_NULL_OBJECT_ID);
    for (size_t i = 0; i < members.size(); ++i)
    {
        bool insert_member = true;
        auto &member = members[i];
        if (!member->watch_port.empty())
        {
            // Create member in SAI only for operationally up ports
            sai_port_oper_status_t oper_status = SAI_PORT_OPER_STATUS_DOWN;
            status = fetchPortOperStatus(member->watch_port, &oper_status);
            if (!status.ok())
            {
                break;
            }

            if (oper_status != SAI_PORT_OPER_STATUS_UP)
            {
                insert_member = false;
                member->pruned = true;
                SWSS_LOG_NOTICE("Member %s in group %s not created in asic as the associated "
                                "watchport "
                                "(%s) is not operationally up",
                                member->next_hop_id.c_str(), member->wcmp_group_id.c_str(), member->watch_port.c_str());
            }
        }
        if (insert_member)
        {
            auto attrs = getSaiMemberAttrs(*(member.get()), wcmp_group_oid);
            gNextHopGroupMemberBulker.create_entry(&nhgm_ids[i], (uint32_t)attrs.size(), attrs.data());
        }
    }
    if (status.ok())
    {
        gNextHopGroupMemberBulker.flush();
        for (size_t i = 0; i < members.size(); ++i)
        {
            auto &member = members[i];
            if (!member->pruned)
            {
                if (nhgm_ids[i] == SAI_NULL_OBJECT_ID)
                {
                    if (status.ok())
                    {
                        status = ReturnCode(StatusCode::SWSS_RC_UNKNOWN)
                                 << "Fail to create wcmp group member: " << QuotedVar(member->next_hop_id);
                    }
                    else
                    {
                        status << "; Fail to create wcmp group member: " << QuotedVar(member->next_hop_id);
                    }
                    continue;
                }
                member->member_oid = nhgm_ids[i];
                m_p4OidMapper->setOID(SAI_OBJECT_TYPE_NEXT_HOP_GROUP_MEMBER,
                                      getWcmpGroupMemberKey(wcmp_group_key, member->member_oid), member->member_oid);
                m_p4OidMapper->increaseRefCount(SAI_OBJECT_TYPE_NEXT_HOP_GROUP, wcmp_group_key);
            }
            if (!member->watch_port.empty())
            {
                // Add member to port_name_to_wcmp_group_member_map
                insertMemberInPortNameToWcmpGroupMemberMap(member);
            }
            const std::string &next_hop_key = KeyGenerator::generateNextHopKey(member->next_hop_id);
            gCrmOrch->incCrmResUsedCounter(CrmResourceType::CRM_NEXTHOP_GROUP_MEMBER);
            m_p4OidMapper->increaseRefCount(SAI_OBJECT_TYPE_NEXT_HOP, next_hop_key);
            created_wcmp_group_members.push_back(member);
        }
    }
    return status;
}

ReturnCode WcmpManager::createWcmpGroup(P4WcmpGroupEntry *wcmp_group)
{
    SWSS_LOG_ENTER();

    auto attrs = getSaiGroupAttrs(*wcmp_group);
    CHECK_ERROR_AND_LOG_AND_RETURN(sai_next_hop_group_api->create_next_hop_group(&wcmp_group->wcmp_group_oid, gSwitchId,
                                                                                 (uint32_t)attrs.size(), attrs.data()),
                                   "Failed to create next hop group  " << QuotedVar(wcmp_group->wcmp_group_id));
    // Update reference count
    const auto &wcmp_group_key = KeyGenerator::generateWcmpGroupKey(wcmp_group->wcmp_group_id);
    gCrmOrch->incCrmResUsedCounter(CrmResourceType::CRM_NEXTHOP_GROUP);
    m_p4OidMapper->setOID(SAI_OBJECT_TYPE_NEXT_HOP_GROUP, wcmp_group_key, wcmp_group->wcmp_group_oid);

    // Create next hop group members
    std::vector<std::shared_ptr<P4WcmpGroupMemberEntry>> created_wcmp_group_members;
    ReturnCode status = processWcmpGroupMembersAddition(wcmp_group->wcmp_group_members, wcmp_group_key,
                                                        wcmp_group->wcmp_group_oid, created_wcmp_group_members);
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
    SWSS_LOG_ENTER();
    std::vector<std::shared_ptr<P4WcmpGroupMemberEntry>> members;
    ReturnCode recovery_status;
    // Clean up created group members - remove created new members
    if (created_wcmp_group_members.size() != 0)
    {
        recovery_status = processWcmpGroupMembersRemoval(created_wcmp_group_members, wcmp_group_key, members)
                              .prepend("Error during recovery: ");
    }

    // Clean up removed group members - create removed old members
    if (recovery_status.ok() && removed_wcmp_group_members.size() != 0)
    {
        recovery_status = processWcmpGroupMembersAddition(removed_wcmp_group_members, wcmp_group_key,
                                                          wcmp_group_entry->wcmp_group_oid, members)
                              .prepend("Error during recovery: ");
    }

    if (!recovery_status.ok())
    {
        SWSS_RAISE_CRITICAL_STATE(recovery_status.message());
    }
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
    auto find_smallest_index = [&](p4orch::P4WcmpGroupEntry *wcmp,
                                   std::vector<std::shared_ptr<P4WcmpGroupMemberEntry>> &other_members) -> int {
        other_members.clear();
        if (wcmp->wcmp_group_members.empty())
        {
            return -1;
        }
        int reserved_idx = 0;
        for (int i = 1; i < (int)wcmp->wcmp_group_members.size(); i++)
        {
            if (wcmp->wcmp_group_members[i]->weight < wcmp->wcmp_group_members[reserved_idx]->weight)
            {
                other_members.push_back(wcmp->wcmp_group_members[reserved_idx]);
                reserved_idx = i;
            }
            else
            {
                other_members.push_back(wcmp->wcmp_group_members[i]);
            }
        }
        return reserved_idx;
    };
    // Find the old member who has the smallest weight, -1 if the member list is
    // empty
    std::vector<std::shared_ptr<P4WcmpGroupMemberEntry>> other_old_members;
    int reserved_old_member_index = find_smallest_index(old_wcmp, other_old_members);
    // Find the new member who has the smallest weight, -1 if the member list is
    // empty
    std::vector<std::shared_ptr<P4WcmpGroupMemberEntry>> other_new_members;
    int reserved_new_member_index = find_smallest_index(wcmp_group_entry, other_new_members);

    // Remove stale group members except the member with the smallest weight
    if (other_old_members.size() != 0)
    {
        update_request_status =
            processWcmpGroupMembersRemoval(other_old_members, wcmp_group_key, removed_wcmp_group_members);
        if (!update_request_status.ok())
        {
            recoverGroupMembers(wcmp_group_entry, wcmp_group_key, created_wcmp_group_members,
                                removed_wcmp_group_members);
            return update_request_status;
        }
    }

    // Create the new member with the smallest weight if member list is nonempty
    if (reserved_new_member_index != -1)
    {
        update_request_status = processWcmpGroupMembersAddition(
            {wcmp_group_entry->wcmp_group_members[reserved_new_member_index]}, wcmp_group_key,
            wcmp_group_entry->wcmp_group_oid, created_wcmp_group_members);
        if (!update_request_status.ok())
        {
            recoverGroupMembers(wcmp_group_entry, wcmp_group_key, created_wcmp_group_members,
                                removed_wcmp_group_members);
            return update_request_status;
        }
    }

    // Remove the old member with the smallest weight if member list is nonempty
    if (reserved_old_member_index != -1)
    {
        update_request_status = processWcmpGroupMembersRemoval(
            {old_wcmp->wcmp_group_members[reserved_old_member_index]}, wcmp_group_key, removed_wcmp_group_members);
        if (!update_request_status.ok())
        {
            recoverGroupMembers(wcmp_group_entry, wcmp_group_key, created_wcmp_group_members,
                                removed_wcmp_group_members);
            return update_request_status;
        }
    }

    // Create new group members
    if (other_new_members.size() != 0)
    {
        update_request_status = processWcmpGroupMembersAddition(
            other_new_members, wcmp_group_key, wcmp_group_entry->wcmp_group_oid, created_wcmp_group_members);
        if (!update_request_status.ok())
        {
            recoverGroupMembers(wcmp_group_entry, wcmp_group_key, created_wcmp_group_members,
                                removed_wcmp_group_members);
            return update_request_status;
        }
    }

    m_wcmpGroupTable[wcmp_group_entry->wcmp_group_id] = *wcmp_group_entry;
    return update_request_status;
}

ReturnCode WcmpManager::removeWcmpGroupMember(const std::shared_ptr<P4WcmpGroupMemberEntry> wcmp_group_member,
                                              const std::string &wcmp_group_key)
{
    SWSS_LOG_ENTER();

    CHECK_ERROR_AND_LOG_AND_RETURN(sai_next_hop_group_api->remove_next_hop_group_member(wcmp_group_member->member_oid),
                                   "Failed to remove WCMP group member with nexthop id "
                                       << QuotedVar(wcmp_group_member->next_hop_id));
    m_p4OidMapper->eraseOID(SAI_OBJECT_TYPE_NEXT_HOP_GROUP_MEMBER,
                            getWcmpGroupMemberKey(wcmp_group_key, wcmp_group_member->member_oid));
    m_p4OidMapper->decreaseRefCount(SAI_OBJECT_TYPE_NEXT_HOP_GROUP, wcmp_group_key);
    return ReturnCode();
}

ReturnCode WcmpManager::processWcmpGroupMembersRemoval(
    const std::vector<std::shared_ptr<P4WcmpGroupMemberEntry>> &members, const std::string &wcmp_group_key,
    std::vector<std::shared_ptr<P4WcmpGroupMemberEntry>> &removed_wcmp_group_members)
{
    SWSS_LOG_ENTER();
    ReturnCode status;
    std::vector<sai_status_t> statuses(members.size(), SAI_STATUS_FAILURE);
    for (size_t i = 0; i < members.size(); ++i)
    {
        auto &member = members[i];
        if (!member->pruned)
        {
            gNextHopGroupMemberBulker.remove_entry(&statuses[i], member->member_oid);
        }
    }
    gNextHopGroupMemberBulker.flush();
    for (size_t i = 0; i < members.size(); ++i)
    {
        auto &member = members[i];
        if (member->pruned)
        {
            SWSS_LOG_NOTICE("Removed pruned member %s from group %s", member->next_hop_id.c_str(),
                            member->wcmp_group_id.c_str());
            member->pruned = false;
        }
        else
        {
            if (statuses[i] != SAI_STATUS_SUCCESS)
            {
                if (status.ok())
                {
                    status = ReturnCode(statuses[i])
                             << "Failed to delete WCMP group member: " << QuotedVar(member->next_hop_id);
                }
                else
                {
                    status << "; Failed to delete WCMP group member: " << QuotedVar(member->next_hop_id);
                }
                continue;
            }
            else
            {
                m_p4OidMapper->eraseOID(SAI_OBJECT_TYPE_NEXT_HOP_GROUP_MEMBER,
                                        getWcmpGroupMemberKey(wcmp_group_key, member->member_oid));
                m_p4OidMapper->decreaseRefCount(SAI_OBJECT_TYPE_NEXT_HOP_GROUP, wcmp_group_key);
            }
        }
        const std::string &next_hop_key = KeyGenerator::generateNextHopKey(member->next_hop_id);
        gCrmOrch->decCrmResUsedCounter(CrmResourceType::CRM_NEXTHOP_GROUP_MEMBER);
        m_p4OidMapper->decreaseRefCount(SAI_OBJECT_TYPE_NEXT_HOP, next_hop_key);
        removeMemberFromPortNameToWcmpGroupMemberMap(member);
        removed_wcmp_group_members.push_back(member);
    }
    return status;
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

    // Delete group members
    std::vector<std::shared_ptr<p4orch::P4WcmpGroupMemberEntry>> removed_wcmp_group_members;
    ReturnCode status =
        processWcmpGroupMembersRemoval(wcmp_group->wcmp_group_members, wcmp_group_key, removed_wcmp_group_members);

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
            // Prune a member if it is not already pruned.
            if (!member->pruned)
            {
                const auto &wcmp_group_key = KeyGenerator::generateWcmpGroupKey(member->wcmp_group_id);
                auto status = removeWcmpGroupMember(member, wcmp_group_key);
                if (!status.ok())
                {
                    std::stringstream msg;
                    msg << "Failed to prune member " << member->next_hop_id << " from group " << member->wcmp_group_id
                        << ": " << status.message();
                    SWSS_RAISE_CRITICAL_STATE(msg.str());
                    return;
                }
                member->pruned = true;
                SWSS_LOG_NOTICE("Pruned member %s from group %s", member->next_hop_id.c_str(),
                                member->wcmp_group_id.c_str());
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
            if (member->pruned)
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
                member->pruned = false;
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
            status = validateWcmpGroupEntry(app_db_entry);
            if (!status.ok())
            {
                SWSS_LOG_ERROR("Invalid WCMP group with id %s: %s", QuotedVar(app_db_entry.wcmp_group_id).c_str(),
                               status.message().c_str());
                m_publisher->publish(APP_P4RT_TABLE_NAME, kfvKey(key_op_fvs_tuple), kfvFieldsValues(key_op_fvs_tuple),
                                     status,
                                     /*replace=*/true);
                continue;
            }
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
        if (!app_db_entry.wcmp_group_members[i]->watch_port.empty() && wcmp_group_entry->wcmp_group_members[i]->pruned)
        {
            continue;
        }
        err_msg = m_p4OidMapper->verifyOIDMapping(
            SAI_OBJECT_TYPE_NEXT_HOP_GROUP_MEMBER,
            getWcmpGroupMemberKey(wcmp_group_key, wcmp_group_entry->wcmp_group_members[i]->member_oid),
            wcmp_group_entry->wcmp_group_members[i]->member_oid);
        if (!err_msg.empty())
        {
            return err_msg;
        }
    }

    return "";
}

std::string WcmpManager::verifyStateAsicDb(const P4WcmpGroupEntry *wcmp_group_entry)
{
    swss::DBConnector db("ASIC_DB", 0);
    swss::Table table(&db, "ASIC_STATE");

    auto group_attrs = getSaiGroupAttrs(*wcmp_group_entry);
    std::vector<swss::FieldValueTuple> exp = saimeta::SaiAttributeList::serialize_attr_list(
        SAI_OBJECT_TYPE_NEXT_HOP_GROUP, (uint32_t)group_attrs.size(), group_attrs.data(), /*countOnly=*/false);
    std::string key = sai_serialize_object_type(SAI_OBJECT_TYPE_NEXT_HOP_GROUP) + ":" +
                      sai_serialize_object_id(wcmp_group_entry->wcmp_group_oid);
    std::vector<swss::FieldValueTuple> values;
    if (!table.get(key, values))
    {
        return std::string("ASIC DB key not found ") + key;
    }
    auto group_result = verifyAttrs(values, exp, std::vector<swss::FieldValueTuple>{},
                                    /*allow_unknown=*/false);
    if (!group_result.empty())
    {
        return group_result;
    }

    for (const auto &member : wcmp_group_entry->wcmp_group_members)
    {
        if (!member->watch_port.empty() && member->pruned)
        {
            continue;
        }
        auto member_attrs = getSaiMemberAttrs(*member, wcmp_group_entry->wcmp_group_oid);
        std::vector<swss::FieldValueTuple> exp = saimeta::SaiAttributeList::serialize_attr_list(
            SAI_OBJECT_TYPE_NEXT_HOP_GROUP_MEMBER, (uint32_t)member_attrs.size(), member_attrs.data(),
            /*countOnly=*/false);
        std::string key = sai_serialize_object_type(SAI_OBJECT_TYPE_NEXT_HOP_GROUP_MEMBER) + ":" +
                          sai_serialize_object_id(member->member_oid);
        std::vector<swss::FieldValueTuple> values;
        if (!table.get(key, values))
        {
            return std::string("ASIC DB key not found ") + key;
        }
        auto member_result = verifyAttrs(values, exp, std::vector<swss::FieldValueTuple>{},
                                         /*allow_unknown=*/false);
        if (!member_result.empty())
        {
            return member_result;
        }
    }

    return "";
}

} // namespace p4orch
