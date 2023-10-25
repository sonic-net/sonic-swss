#include <boost/iterator/counting_iterator.hpp>

#include <map>

#include "dashaclgroupmgr.h"

#include "crmorch.h"
#include "dashorch.h"
#include "dashaclorch.h"
#include "saihelper.h"
#include "pbutils.h"

extern sai_dash_acl_api_t* sai_dash_acl_api;
extern sai_dash_eni_api_t* sai_dash_eni_api;
extern sai_object_id_t gSwitchId;
extern CrmOrch *gCrmOrch;

using namespace std;
using namespace swss;
using namespace dash::acl_in;
using namespace dash::acl_out;
using namespace dash::acl_rule;
using namespace dash::acl_group;
using namespace dash::tag;
using namespace dash::types;

const static vector<uint8_t> all_protocols(boost::counting_iterator<int>(0), boost::counting_iterator<int>(UINT8_MAX + 1));
const static vector<sai_u16_range_t> all_ports = {{numeric_limits<uint16_t>::min(), numeric_limits<uint16_t>::max()}};

bool from_pb(const AclRule& data, DashAclRule& rule)
{
    rule.m_priority = data.priority();
    rule.m_action = (data.action() == ACTION_PERMIT) ? DashAclRule::Action::ALLOW : DashAclRule::Action::DENY;
    rule.m_terminating = data.terminating();

    if (data.protocol_size())
    {
        rule.m_protocols.reserve(data.protocol_size());
        rule.m_protocols.assign(data.protocol().begin(), data.protocol().end());
    }

    if (!to_sai(data.src_addr(), rule.m_src_prefixes))
    {
        return false;
    }

    if (!to_sai(data.dst_addr(), rule.m_dst_prefixes))
    {
        return false;
    }

    if (data.src_tag_size())
    {
        rule.m_src_tags.insert(data.src_tag().begin(), data.src_tag().end());
    }

    if (data.dst_tag_size())
    {
        rule.m_dst_tags.insert(data.dst_tag().begin(), data.dst_tag().end());
    }

    if (!data.src_port_size())
    {
        rule.m_src_ports = all_ports;
    }
    else if (!to_sai(data.src_port(), rule.m_src_ports))
    {
        return false;
    }

    if (!data.dst_port_size())
    {
        rule.m_dst_ports = all_ports;
    }
    else if (!to_sai(data.dst_port(), rule.m_dst_ports))
    {
        return false;
    }

    return true;
}

bool from_pb(const dash::acl_group::AclGroup &data, DashAclGroup& group)
{
    if (!to_sai(data.ip_version(), group.m_ip_version))
    {
        return false;
    }

    return true;
}

sai_attr_id_t getSaiStage(DashAclDirection d, sai_ip_addr_family_t f, DashAclStage s)
{
    const static map<tuple<DashAclDirection, sai_ip_addr_family_t, DashAclStage>, sai_attr_id_t> StageMaps =
        {
            {{DashAclDirection::IN, SAI_IP_ADDR_FAMILY_IPV4, DashAclStage::STAGE1}, SAI_ENI_ATTR_INBOUND_V4_STAGE1_DASH_ACL_GROUP_ID},
            {{DashAclDirection::IN, SAI_IP_ADDR_FAMILY_IPV4, DashAclStage::STAGE2}, SAI_ENI_ATTR_INBOUND_V4_STAGE2_DASH_ACL_GROUP_ID},
            {{DashAclDirection::IN, SAI_IP_ADDR_FAMILY_IPV4, DashAclStage::STAGE3}, SAI_ENI_ATTR_INBOUND_V4_STAGE3_DASH_ACL_GROUP_ID},
            {{DashAclDirection::IN, SAI_IP_ADDR_FAMILY_IPV4, DashAclStage::STAGE4}, SAI_ENI_ATTR_INBOUND_V4_STAGE4_DASH_ACL_GROUP_ID},
            {{DashAclDirection::IN, SAI_IP_ADDR_FAMILY_IPV4, DashAclStage::STAGE5}, SAI_ENI_ATTR_INBOUND_V4_STAGE5_DASH_ACL_GROUP_ID},
            {{DashAclDirection::IN, SAI_IP_ADDR_FAMILY_IPV6, DashAclStage::STAGE1}, SAI_ENI_ATTR_INBOUND_V6_STAGE1_DASH_ACL_GROUP_ID},
            {{DashAclDirection::IN, SAI_IP_ADDR_FAMILY_IPV6, DashAclStage::STAGE2}, SAI_ENI_ATTR_INBOUND_V6_STAGE2_DASH_ACL_GROUP_ID},
            {{DashAclDirection::IN, SAI_IP_ADDR_FAMILY_IPV6, DashAclStage::STAGE3}, SAI_ENI_ATTR_INBOUND_V6_STAGE3_DASH_ACL_GROUP_ID},
            {{DashAclDirection::IN, SAI_IP_ADDR_FAMILY_IPV6, DashAclStage::STAGE4}, SAI_ENI_ATTR_INBOUND_V6_STAGE4_DASH_ACL_GROUP_ID},
            {{DashAclDirection::IN, SAI_IP_ADDR_FAMILY_IPV6, DashAclStage::STAGE5}, SAI_ENI_ATTR_INBOUND_V6_STAGE5_DASH_ACL_GROUP_ID},
            {{DashAclDirection::OUT, SAI_IP_ADDR_FAMILY_IPV4, DashAclStage::STAGE1}, SAI_ENI_ATTR_OUTBOUND_V4_STAGE1_DASH_ACL_GROUP_ID},
            {{DashAclDirection::OUT, SAI_IP_ADDR_FAMILY_IPV4, DashAclStage::STAGE2}, SAI_ENI_ATTR_OUTBOUND_V4_STAGE2_DASH_ACL_GROUP_ID},
            {{DashAclDirection::OUT, SAI_IP_ADDR_FAMILY_IPV4, DashAclStage::STAGE3}, SAI_ENI_ATTR_OUTBOUND_V4_STAGE3_DASH_ACL_GROUP_ID},
            {{DashAclDirection::OUT, SAI_IP_ADDR_FAMILY_IPV4, DashAclStage::STAGE4}, SAI_ENI_ATTR_OUTBOUND_V4_STAGE4_DASH_ACL_GROUP_ID},
            {{DashAclDirection::OUT, SAI_IP_ADDR_FAMILY_IPV4, DashAclStage::STAGE5}, SAI_ENI_ATTR_OUTBOUND_V4_STAGE5_DASH_ACL_GROUP_ID},
            {{DashAclDirection::OUT, SAI_IP_ADDR_FAMILY_IPV6, DashAclStage::STAGE1}, SAI_ENI_ATTR_OUTBOUND_V6_STAGE1_DASH_ACL_GROUP_ID},
            {{DashAclDirection::OUT, SAI_IP_ADDR_FAMILY_IPV6, DashAclStage::STAGE2}, SAI_ENI_ATTR_OUTBOUND_V6_STAGE2_DASH_ACL_GROUP_ID},
            {{DashAclDirection::OUT, SAI_IP_ADDR_FAMILY_IPV6, DashAclStage::STAGE3}, SAI_ENI_ATTR_OUTBOUND_V6_STAGE3_DASH_ACL_GROUP_ID},
            {{DashAclDirection::OUT, SAI_IP_ADDR_FAMILY_IPV6, DashAclStage::STAGE4}, SAI_ENI_ATTR_OUTBOUND_V6_STAGE4_DASH_ACL_GROUP_ID},
            {{DashAclDirection::OUT, SAI_IP_ADDR_FAMILY_IPV6, DashAclStage::STAGE5}, SAI_ENI_ATTR_OUTBOUND_V6_STAGE5_DASH_ACL_GROUP_ID},
        };

    auto stage = StageMaps.find({d, f, s});
    if (stage == StageMaps.end())
    {
        SWSS_LOG_ERROR("Invalid stage %d %d %d", static_cast<int>(d), f, static_cast<int>(s));
        throw runtime_error("Invalid stage");
    }

    return stage->second;
}

DashAclGroupMgr::DashAclGroupMgr(DashOrch *dashorch, DashAclOrch *aclorch) :
    m_dash_orch(dashorch),
    m_dash_acl_orch(aclorch)
{
    SWSS_LOG_ENTER();
}

void DashAclGroupMgr::init(DashAclGroup& group)
{
    SWSS_LOG_ENTER();
    group.m_dash_acl_group_id = SAI_NULL_OBJECT_ID;

    for (auto& rule: group.m_dash_acl_rule_table)
    {
        rule.second.m_dash_acl_rule_id = SAI_NULL_OBJECT_ID;
    }
}

void DashAclGroupMgr::create(DashAclGroup& group)
{
    SWSS_LOG_ENTER();

    vector<sai_attribute_t> attrs;

    attrs.emplace_back();
    attrs.back().id = SAI_DASH_ACL_GROUP_ATTR_IP_ADDR_FAMILY;
    attrs.back().value.s32 = group.m_ip_version;

    auto status = sai_dash_acl_api->create_dash_acl_group(&group.m_dash_acl_group_id, gSwitchId, static_cast<uint32_t>(attrs.size()), attrs.data());
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to create ACL group: %d, %s", status, sai_serialize_status(status).c_str());
        handleSaiCreateStatus((sai_api_t)SAI_API_DASH_ACL, status);
    }

    CrmResourceType crm_rtype = (group.m_ip_version == SAI_IP_ADDR_FAMILY_IPV4) ?
        CrmResourceType::CRM_DASH_IPV4_ACL_GROUP : CrmResourceType::CRM_DASH_IPV6_ACL_GROUP;
    gCrmOrch->incCrmDashAclUsedCounter(crm_rtype, group.m_dash_acl_group_id);
}

task_process_status DashAclGroupMgr::create(const string& group_id, DashAclGroup& group)
{
    SWSS_LOG_ENTER();

    if (exists(group_id))
    {
        return task_failed;
    }

    create(group);

    m_groups_table.emplace(group_id, group);

    SWSS_LOG_INFO("Created ACL group %s", group_id.c_str());

    return task_success;
}

void DashAclGroupMgr::remove(DashAclGroup& group)
{
    SWSS_LOG_ENTER();

    if (group.m_dash_acl_group_id == SAI_NULL_OBJECT_ID)
    {
        return;
    }

    sai_status_t status = sai_dash_acl_api->remove_dash_acl_group(group.m_dash_acl_group_id);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to remove ACL group: %d, %s", status, sai_serialize_status(status).c_str());
        handleSaiRemoveStatus((sai_api_t)SAI_API_DASH_ACL, status);
    }

    CrmResourceType crm_rtype = (group.m_ip_version == SAI_IP_ADDR_FAMILY_IPV4) ?
        CrmResourceType::CRM_DASH_IPV4_ACL_GROUP : CrmResourceType::CRM_DASH_IPV6_ACL_GROUP;
    gCrmOrch->decCrmDashAclUsedCounter(crm_rtype, group.m_dash_acl_group_id);

    group.m_dash_acl_group_id = SAI_NULL_OBJECT_ID;
}

task_process_status DashAclGroupMgr::remove(const string& group_id)
{
    SWSS_LOG_ENTER();

    auto group_it = m_groups_table.find(group_id);
    if (group_it == m_groups_table.end())
    {
        SWSS_LOG_INFO("ACL group %s doesn't exist", group_id.c_str());
        return task_success;
    }

    auto& group = group_it->second;

    if (!group.m_dash_acl_rule_table.empty())
    {
        SWSS_LOG_ERROR("ACL group %s still has %zu rules", group_id.c_str(), group.m_dash_acl_rule_table.size());
        return task_need_retry;
    }

    if (isBound(group))
    {
        SWSS_LOG_ERROR("ACL group %s still has %zu references", group_id.c_str(), group.m_in_tables.size() + group.m_out_tables.size());
        return task_need_retry;
    }

    remove(group);

    m_groups_table.erase(group_id);
    SWSS_LOG_INFO("Removed ACL group %s", group_id.c_str());

    return task_success;
}

bool DashAclGroupMgr::exists(const string& group_id) const
{
    SWSS_LOG_ENTER();

    return m_groups_table.find(group_id) != m_groups_table.end();
}

void DashAclGroupMgr::onUpdate(const string& group_id, const string& tag_id, const DashTag& tag)
{
    SWSS_LOG_ENTER();

    auto group_it = m_groups_table.find(group_id);
    if (group_it == m_groups_table.end())
    {
        return;
    }

    auto& group = group_it->second;
    if (isBound(group))
    {
        // If the group is bound to at least one ENI refresh the full group to update the affected rules.
        // When the group is bound to the ENI we need to make sure that the update of the affected rules will be atomic.
        SWSS_LOG_INFO("Update full ACL group %s", group_id.c_str());

        refreshAclGroupFull(group_id);
    }
    else
    {
        // If the group is not bound to ENI update the rule immediately.
        SWSS_LOG_INFO("Update ACL group %s", group_id.c_str());
        for (auto& rule_it: group.m_dash_acl_rule_table)
        {
            auto& rule = rule_it.second;
            if (rule.m_src_tags.find(tag_id) != rule.m_src_tags.end() || rule.m_dst_tags.find(tag_id) != rule.m_dst_tags.end())
            {
                removeRule(group, rule);
                createRule(group, rule);
            }
        }
    }
}

void DashAclGroupMgr::refreshAclGroupFull(const string &group_id)
{
    SWSS_LOG_ENTER();

    auto& group = m_groups_table[group_id];

    DashAclGroup new_group = group;
    init(new_group);
    create(new_group);

    for (auto& rule: new_group.m_dash_acl_rule_table)
    {
        createRule(new_group, rule.second);
    }

    for (const auto& table: new_group.m_in_tables)
    {
        const auto& eni_id = table.first;
        const auto& stages = table.second;

        const auto eni = m_dash_orch->getEni(eni_id);
        ABORT_IF_NOT(eni != nullptr, "Failed to get ENI %s", eni_id.c_str());

        for (const auto& stage: stages)
        {
            bind(new_group, *eni, DashAclDirection::IN, stage);
        }
    }

    for (const auto& table: new_group.m_out_tables)
    {
        const auto& eni_id = table.first;
        const auto& stages = table.second;

        const auto eni = m_dash_orch->getEni(eni_id);
        ABORT_IF_NOT(eni != nullptr, "Failed to get ENI %s", eni_id.c_str());

        for (const auto& stage: stages)
        {
            bind(new_group, *eni, DashAclDirection::OUT, stage);
        }
    }

    removeAclGroupFull(group);

    group = new_group;
}

void DashAclGroupMgr::removeAclGroupFull(DashAclGroup& group)
{
    SWSS_LOG_ENTER();

    for (auto& rule: group.m_dash_acl_rule_table)
    {
        removeRule(group, rule.second);
    }

    remove(group);
}

void DashAclGroupMgr::createRule(DashAclGroup& group, DashAclRule& rule)
{
    SWSS_LOG_ENTER();

    vector<sai_attribute_t> attrs;
    vector<sai_ip_prefix_t> src_prefixes = {};
    vector<sai_ip_prefix_t> dst_prefixes = {};

    auto any_ip = [] (const auto& g)
    {
        sai_ip_prefix_t ip_prefix = {};
        ip_prefix.addr_family = g.isIpV4() ? SAI_IP_ADDR_FAMILY_IPV4 : SAI_IP_ADDR_FAMILY_IPV6;
        return ip_prefix;
    };

    attrs.emplace_back();
    attrs.back().id = SAI_DASH_ACL_RULE_ATTR_PRIORITY;
    attrs.back().value.u32 = rule.m_priority;

    attrs.emplace_back();
    attrs.back().id = SAI_DASH_ACL_RULE_ATTR_ACTION;

    if (rule.m_action == DashAclRule::Action::ALLOW)
    {
        attrs.back().value.s32 = rule.m_terminating ?
            SAI_DASH_ACL_RULE_ACTION_PERMIT : SAI_DASH_ACL_RULE_ACTION_PERMIT_AND_CONTINUE;
    }
    else
    {
        attrs.back().value.s32 = rule.m_terminating ?
            SAI_DASH_ACL_RULE_ACTION_DENY : SAI_DASH_ACL_RULE_ACTION_DENY_AND_CONTINUE;
    }

    attrs.emplace_back();
    attrs.back().id = SAI_DASH_ACL_RULE_ATTR_PROTOCOL;

    if (rule.m_protocols.size())
    {
        attrs.back().value.u8list.count = static_cast<uint32_t>(rule.m_protocols.size());
        attrs.back().value.u8list.list = rule.m_protocols.data();
    }
    else
    {
        auto protocols = all_protocols;
        attrs.back().value.u8list.count = static_cast<uint32_t>(protocols.size());
        attrs.back().value.u8list.list = protocols.data();
    }

    if (!rule.m_src_prefixes.empty())
    {
        src_prefixes.insert(src_prefixes.end(),
                 rule.m_src_prefixes.begin(),  rule.m_src_prefixes.end());
    }

    if (!rule.m_dst_prefixes.empty())
    {
        dst_prefixes.insert(dst_prefixes.end(),
                rule.m_dst_prefixes.begin(), rule.m_dst_prefixes.end());
    }

    for (const auto &tag : rule.m_src_tags)
    {
        const auto& prefixes = m_dash_acl_orch->getDashAclTagMgr().getPrefixes(tag);

        src_prefixes.insert(src_prefixes.end(),
            prefixes.begin(), prefixes.end());
    }

    for (const auto &tag : rule.m_dst_tags)
    {
        const auto& prefixes = m_dash_acl_orch->getDashAclTagMgr().getPrefixes(tag);

        dst_prefixes.insert(dst_prefixes.end(),
            prefixes.begin(), prefixes.end());
    }

    if (src_prefixes.empty())
    {
        src_prefixes.push_back(any_ip(group));
    }

    if (dst_prefixes.empty())
    {
        dst_prefixes.push_back(any_ip(group));
    }

    attrs.emplace_back();
    attrs.back().id = SAI_DASH_ACL_RULE_ATTR_SIP;
    attrs.back().value.ipprefixlist.count = static_cast<uint32_t>(src_prefixes.size());
    attrs.back().value.ipprefixlist.list = src_prefixes.data();

    attrs.emplace_back();
    attrs.back().id = SAI_DASH_ACL_RULE_ATTR_DIP;
    attrs.back().value.ipprefixlist.count = static_cast<uint32_t>(dst_prefixes.size());
    attrs.back().value.ipprefixlist.list = dst_prefixes.data();

    attrs.emplace_back();
    attrs.back().id = SAI_DASH_ACL_RULE_ATTR_SRC_PORT;
    attrs.back().value.u16rangelist.count = static_cast<uint32_t>(rule.m_src_ports.size());
    attrs.back().value.u16rangelist.list = rule.m_src_ports.data();

    attrs.emplace_back();
    attrs.back().id = SAI_DASH_ACL_RULE_ATTR_DST_PORT;
    attrs.back().value.u16rangelist.count = static_cast<uint32_t>(rule.m_dst_ports.size());
    attrs.back().value.u16rangelist.list = rule.m_dst_ports.data();

    attrs.emplace_back();
    attrs.back().id = SAI_DASH_ACL_RULE_ATTR_DASH_ACL_GROUP_ID;
    attrs.back().value.oid = group.m_dash_acl_group_id;

    auto status = sai_dash_acl_api->create_dash_acl_rule(&rule.m_dash_acl_rule_id, gSwitchId, static_cast<uint32_t>(attrs.size()), attrs.data());
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to create ACL rule: %d, %s", status, sai_serialize_status(status).c_str());
        handleSaiCreateStatus((sai_api_t)SAI_API_DASH_ACL, status);
    }

    CrmResourceType crm_rtype = (group.m_ip_version == SAI_IP_ADDR_FAMILY_IPV4) ?
            CrmResourceType::CRM_DASH_IPV4_ACL_RULE : CrmResourceType::CRM_DASH_IPV6_ACL_RULE;
    gCrmOrch->incCrmDashAclUsedCounter(crm_rtype, group.m_dash_acl_group_id);
}

task_process_status DashAclGroupMgr::createRule(const string& group_id, const string& rule_id, DashAclRule& rule)
{
    SWSS_LOG_ENTER();

    auto group_it = m_groups_table.find(group_id);
    if (group_it == m_groups_table.end())
    {
        SWSS_LOG_INFO("ACL group %s doesn't exist, waiting for group creating before creating rule %s", group_id.c_str(), rule_id.c_str());
        return task_need_retry;
    }
    auto& group = group_it->second;

    auto acl_rule_it = group.m_dash_acl_rule_table.find(rule_id);
    ABORT_IF_NOT(acl_rule_it == group.m_dash_acl_rule_table.end(), "Failed to create ACL rule %s. Rule already exist in ACL group %s", rule_id.c_str(), group_id.c_str());

    createRule(group, rule);

    group.m_dash_acl_rule_table.emplace(rule_id, rule);
    attachTags(group_id, rule.m_src_tags);
    attachTags(group_id, rule.m_dst_tags);

    SWSS_LOG_INFO("Created ACL rule %s:%s", group_id.c_str(), rule_id.c_str());

    return task_success;
}

task_process_status DashAclGroupMgr::updateRule(const string& group_id, const string& rule_id, DashAclRule& rule)
{
    SWSS_LOG_ENTER();

    if (isBound(group_id))
    {
        SWSS_LOG_INFO("Failed to update dash ACL rule %s:%s, ACL group is bound to the ENI", group_id.c_str(), rule_id.c_str());
        return task_failed;
    }

    if (ruleExists(group_id, rule_id))
    {
        removeRule(group_id, rule_id);
    }

    createRule(group_id, rule_id, rule);

    return task_success;
}

void DashAclGroupMgr::removeRule(DashAclGroup& group, DashAclRule& rule)
{
    SWSS_LOG_ENTER();

    if (rule.m_dash_acl_rule_id == SAI_NULL_OBJECT_ID)
    {
        return;
    }

    // Remove the ACL group
    auto status = sai_dash_acl_api->remove_dash_acl_rule(rule.m_dash_acl_rule_id);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to remove ACL rule: %d, %s", status, sai_serialize_status(status).c_str());
        handleSaiRemoveStatus((sai_api_t)SAI_API_DASH_ACL, status);
    }

    CrmResourceType crm_resource = (group.m_ip_version == SAI_IP_ADDR_FAMILY_IPV4) ?
        CrmResourceType::CRM_DASH_IPV4_ACL_RULE : CrmResourceType::CRM_DASH_IPV6_ACL_RULE;
    gCrmOrch->decCrmDashAclUsedCounter(crm_resource, group.m_dash_acl_group_id);

    rule.m_dash_acl_rule_id = SAI_NULL_OBJECT_ID;
}

task_process_status DashAclGroupMgr::removeRule(const string& group_id, const string& rule_id)
{
    SWSS_LOG_ENTER();

    if (!exists(group_id) || !ruleExists(group_id, rule_id))
    {
        SWSS_LOG_INFO("ACL rule %s:%s does not exists", group_id.c_str(), rule_id.c_str());
        return task_success;
    }

    auto& group = m_groups_table[group_id];
    if (isBound(group))
    {
        SWSS_LOG_INFO("Failed to remove dash ACL rule %s:%s, ACL group is bound to the ENI", group_id.c_str(), rule_id.c_str());
        return task_need_retry;
    }

    auto& rule = group.m_dash_acl_rule_table[rule_id];

    removeRule(group, rule);

    detachTags(group_id, rule.m_src_tags);
    detachTags(group_id, rule.m_dst_tags);

    group.m_dash_acl_rule_table.erase(rule_id);

    SWSS_LOG_INFO("Removed ACL rule %s:%s", group_id.c_str(), rule_id.c_str());

    return task_success;
}

void DashAclGroupMgr::bind(const DashAclGroup& group, const EniEntry& eni, DashAclDirection direction, DashAclStage stage)
{
    SWSS_LOG_ENTER();

    sai_attribute_t attr;

    attr.id = getSaiStage(direction, group.m_ip_version, stage);
    attr.value.oid = group.m_dash_acl_group_id;

    auto status = sai_dash_eni_api->set_eni_attribute(eni.eni_id, &attr);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to bind ACL group to ENI: %d", status);
        handleSaiSetStatus((sai_api_t)SAI_API_DASH_ENI, status);
    }
}

bool DashAclGroupMgr::ruleExists(const string& group_id, const string& rule_id) const
{
    SWSS_LOG_ENTER();

    auto group_it = m_groups_table.find(group_id);
    if (group_it == m_groups_table.end())
    {
        return false;
    }

    return group_it->second.m_dash_acl_rule_table.find(rule_id) != group_it->second.m_dash_acl_rule_table.end();
}

task_process_status DashAclGroupMgr::bind(const string& group_id, const string& eni_id, DashAclDirection direction, DashAclStage stage)
{
    SWSS_LOG_ENTER();

    auto group_it = m_groups_table.find(group_id);
    if (group_it == m_groups_table.end())
    {
        SWSS_LOG_INFO("Failed to bind ACL group %s to ENI %s. ACL group does not exist", group_id.c_str(), eni_id.c_str());
        return task_need_retry;
    }

    auto& group = group_it->second;

    if (group.m_dash_acl_rule_table.empty())
    {
        SWSS_LOG_INFO("ACL group %s has no rules attached. Waiting for ACL rules creation", group_id.c_str());
        return task_need_retry;
    }

    auto eni = m_dash_orch->getEni(eni_id);
    if (!eni)
    {
        SWSS_LOG_INFO("eni %s cannot be found", eni_id.c_str());
        return task_need_retry;
    }

    bind(group, *eni, direction, stage);

    auto& table = (direction == DashAclDirection::IN) ? group.m_in_tables : group.m_out_tables;
    auto& eni_stages = table[eni_id];

    eni_stages.insert(stage);

    SWSS_LOG_INFO("Bound ACL group %s to ENI %s", group_id.c_str(), eni_id.c_str());

    return task_success;
}

void DashAclGroupMgr::unbind(const DashAclGroup& group, const EniEntry& eni, DashAclDirection direction, DashAclStage stage)
{
    SWSS_LOG_ENTER();

    sai_attribute_t attr;

    attr.id = getSaiStage(direction, group.m_ip_version, stage);
    attr.value.oid = SAI_NULL_OBJECT_ID;

    auto status = sai_dash_eni_api->set_eni_attribute(eni.eni_id, &attr);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to unbind ACL group from ENI: %d", status);
        handleSaiSetStatus((sai_api_t)SAI_API_DASH_ENI, status);
    }
}

task_process_status DashAclGroupMgr::unbind(const string& group_id, const string& eni_id, DashAclDirection direction, DashAclStage stage)
{
    SWSS_LOG_ENTER();

    auto group_it = m_groups_table.find(group_id);
    if (group_it == m_groups_table.end())
    {
        SWSS_LOG_INFO("ACL group %s does not exist", group_id.c_str());
        return task_success;
    }

    auto& group = group_it->second;

    auto eni_entry = m_dash_orch->getEni(eni_id);
    if (!eni_entry)
    {
        SWSS_LOG_INFO("eni %s cannot be found", eni_id.c_str());
        return task_success;
    }

    auto& table = (direction == DashAclDirection::IN) ? group.m_in_tables : group.m_out_tables;
    auto eni_it = table.find(eni_id);
    if (eni_it == table.end())
    {
        SWSS_LOG_INFO("ACL group %s is not bound to ENI %s", group_id.c_str(), eni_id.c_str());
        return task_success;
    }

    auto& eni_stages = eni_it->second;
    if (eni_stages.find(stage) == eni_stages.end())
    {
        SWSS_LOG_INFO("ACL group %s is not bound to ENI %s stage %d", group_id.c_str(), eni_id.c_str(), static_cast<int>(stage));
        return task_success;
    }

    unbind(group, *eni_entry, direction, stage);

    eni_stages.erase(stage);
    if (eni_stages.empty())
    {
        table.erase(eni_it);
    }

    return task_success;
}

bool DashAclGroupMgr::isBound(const string &group_id)
{
    SWSS_LOG_ENTER();

    if (!exists(group_id))
    {
        return false;
    }

    return isBound(m_groups_table[group_id]);
}

bool DashAclGroupMgr::isBound(const DashAclGroup& group)
{
    SWSS_LOG_ENTER();

    return !group.m_in_tables.empty() || !group.m_out_tables.empty();
}

void DashAclGroupMgr::attachTags(const string &group_id, const unordered_set<string>& tags)
{
    SWSS_LOG_ENTER();

    for (const auto& tag_id : tags)
    {
        m_dash_acl_orch->getDashAclTagMgr().attach(tag_id, group_id);
    }
}

void DashAclGroupMgr::detachTags(const string &group_id, const unordered_set<string>& tags)
{
    SWSS_LOG_ENTER();

    for (const auto& tag_id : tags)
    {
        m_dash_acl_orch->getDashAclTagMgr().detach(tag_id, group_id);
    }
}
