#include <swss/logger.h>
#include <swss/stringutility.h>
#include <swss/redisutility.h>
#include <swss/ipaddress.h>

#include <swssnet.h>

#include <functional>
#include <limits>

#include "dashaclorch.h"
#include "taskworker.h"
#include "pbutils.h"
#include "crmorch.h"
#include "dashaclorch.h"
#include "saihelper.h"

using namespace std;
using namespace swss;
using namespace dash::acl_in;
using namespace dash::acl_out;
using namespace dash::acl_rule;
using namespace dash::acl_group;
using namespace dash::types;

extern sai_dash_acl_api_t* sai_dash_acl_api;
extern sai_dash_eni_api_t* sai_dash_eni_api;
extern sai_object_id_t gSwitchId;
extern CrmOrch *gCrmOrch;

template <typename T, typename... Args>
static bool extractVariables(const string &input, char delimiter, T &output, Args &... args)
{
    SWSS_LOG_ENTER();

    const auto tokens = swss::tokenize(input, delimiter);
    try
    {
        swss::lexical_convert(tokens, output, args...);
        return true;
    }
    catch(const exception& e)
    {
        return false;
    }
}

sai_attr_id_t getSaiStage(DashAclDirection d, sai_ip_addr_family_t f, uint32_t s)
{
    const static map<tuple<DashAclDirection, sai_ip_addr_family_t, uint32_t>, sai_attr_id_t> StageMaps =
        {
            {{IN, SAI_IP_ADDR_FAMILY_IPV4, 1}, SAI_ENI_ATTR_INBOUND_V4_STAGE1_DASH_ACL_GROUP_ID},
            {{IN, SAI_IP_ADDR_FAMILY_IPV4, 2}, SAI_ENI_ATTR_INBOUND_V4_STAGE2_DASH_ACL_GROUP_ID},
            {{IN, SAI_IP_ADDR_FAMILY_IPV4, 3}, SAI_ENI_ATTR_INBOUND_V4_STAGE3_DASH_ACL_GROUP_ID},
            {{IN, SAI_IP_ADDR_FAMILY_IPV4, 4}, SAI_ENI_ATTR_INBOUND_V4_STAGE4_DASH_ACL_GROUP_ID},
            {{IN, SAI_IP_ADDR_FAMILY_IPV4, 5}, SAI_ENI_ATTR_INBOUND_V4_STAGE5_DASH_ACL_GROUP_ID},
            {{IN, SAI_IP_ADDR_FAMILY_IPV6, 1}, SAI_ENI_ATTR_INBOUND_V6_STAGE1_DASH_ACL_GROUP_ID},
            {{IN, SAI_IP_ADDR_FAMILY_IPV6, 2}, SAI_ENI_ATTR_INBOUND_V6_STAGE2_DASH_ACL_GROUP_ID},
            {{IN, SAI_IP_ADDR_FAMILY_IPV6, 3}, SAI_ENI_ATTR_INBOUND_V6_STAGE3_DASH_ACL_GROUP_ID},
            {{IN, SAI_IP_ADDR_FAMILY_IPV6, 4}, SAI_ENI_ATTR_INBOUND_V6_STAGE4_DASH_ACL_GROUP_ID},
            {{IN, SAI_IP_ADDR_FAMILY_IPV6, 5}, SAI_ENI_ATTR_INBOUND_V6_STAGE5_DASH_ACL_GROUP_ID},
            {{OUT, SAI_IP_ADDR_FAMILY_IPV4, 1}, SAI_ENI_ATTR_OUTBOUND_V4_STAGE1_DASH_ACL_GROUP_ID},
            {{OUT, SAI_IP_ADDR_FAMILY_IPV4, 2}, SAI_ENI_ATTR_OUTBOUND_V4_STAGE2_DASH_ACL_GROUP_ID},
            {{OUT, SAI_IP_ADDR_FAMILY_IPV4, 3}, SAI_ENI_ATTR_OUTBOUND_V4_STAGE3_DASH_ACL_GROUP_ID},
            {{OUT, SAI_IP_ADDR_FAMILY_IPV4, 4}, SAI_ENI_ATTR_OUTBOUND_V4_STAGE4_DASH_ACL_GROUP_ID},
            {{OUT, SAI_IP_ADDR_FAMILY_IPV4, 5}, SAI_ENI_ATTR_OUTBOUND_V4_STAGE5_DASH_ACL_GROUP_ID},
            {{OUT, SAI_IP_ADDR_FAMILY_IPV6, 1}, SAI_ENI_ATTR_OUTBOUND_V6_STAGE1_DASH_ACL_GROUP_ID},
            {{OUT, SAI_IP_ADDR_FAMILY_IPV6, 2}, SAI_ENI_ATTR_OUTBOUND_V6_STAGE2_DASH_ACL_GROUP_ID},
            {{OUT, SAI_IP_ADDR_FAMILY_IPV6, 3}, SAI_ENI_ATTR_OUTBOUND_V6_STAGE3_DASH_ACL_GROUP_ID},
            {{OUT, SAI_IP_ADDR_FAMILY_IPV6, 4}, SAI_ENI_ATTR_OUTBOUND_V6_STAGE4_DASH_ACL_GROUP_ID},
            {{OUT, SAI_IP_ADDR_FAMILY_IPV6, 5}, SAI_ENI_ATTR_OUTBOUND_V6_STAGE5_DASH_ACL_GROUP_ID},
        };

    auto stage = StageMaps.find({d, f, s});
    if (stage == StageMaps.end())
    {
        SWSS_LOG_WARN("Invalid stage %d %d %d", d, f, s);
        throw runtime_error("Invalid stage");
    }

    return stage->second;
}

DashAclOrch::DashAclOrch(DBConnector *db, const vector<string> &tables, DashOrch *dash_orch, ZmqServer *zmqServer) :
    ZmqOrch(db, tables, zmqServer),
    m_dash_orch(dash_orch)
{
    SWSS_LOG_ENTER();

    assert(m_dash_orch);
}

void DashAclOrch::doTask(ConsumerBase &consumer)
{
    SWSS_LOG_ENTER();

    const static TaskMap TaskMap = {
        PbWorker<AclIn>::makeMemberTask(APP_DASH_ACL_IN_TABLE_NAME, SET_COMMAND, &DashAclOrch::taskUpdateDashAclIn, this),
        KeyOnlyWorker::makeMemberTask(APP_DASH_ACL_IN_TABLE_NAME, DEL_COMMAND, &DashAclOrch::taskRemoveDashAclIn, this),
        PbWorker<AclOut>::makeMemberTask(APP_DASH_ACL_OUT_TABLE_NAME, SET_COMMAND, &DashAclOrch::taskUpdateDashAclOut, this),
        KeyOnlyWorker::makeMemberTask(APP_DASH_ACL_OUT_TABLE_NAME, DEL_COMMAND, &DashAclOrch::taskRemoveDashAclOut, this),
        PbWorker<AclGroup>::makeMemberTask(APP_DASH_ACL_GROUP_TABLE_NAME, SET_COMMAND, &DashAclOrch::taskUpdateDashAclGroup, this),
        KeyOnlyWorker::makeMemberTask(APP_DASH_ACL_GROUP_TABLE_NAME, DEL_COMMAND, &DashAclOrch::taskRemoveDashAclGroup, this),
        PbWorker<AclRule>::makeMemberTask(APP_DASH_ACL_RULE_TABLE_NAME, SET_COMMAND, &DashAclOrch::taskUpdateDashAclRule, this),
        KeyOnlyWorker::makeMemberTask(APP_DASH_ACL_RULE_TABLE_NAME, DEL_COMMAND, &DashAclOrch::taskRemoveDashAclRule, this),
    };

    const string &table_name = consumer.getTableName();
    auto itr = consumer.m_toSync.begin();
    while (itr != consumer.m_toSync.end())
    {
        task_process_status task_status = task_failed;
        auto &message = itr->second;
        const string &op = kfvOp(message);

        auto task = TaskMap.find(make_tuple(table_name, op));
        if (task != TaskMap.end())
        {
            task_status = task->second->process(kfvKey(message), kfvFieldsValues(message));
        }
        else
        {
            SWSS_LOG_ERROR(
                "Unknown task : %s - %s",
                table_name.c_str(),
                op.c_str());
        }

        if (task_status == task_need_retry)
        {
            SWSS_LOG_DEBUG(
                "Task %s - %s need retry",
                table_name.c_str(),
                op.c_str());
            ++itr;
        }
        else
        {
            if (task_status != task_success)
            {
                SWSS_LOG_WARN("Task %s - %s fail",
                              table_name.c_str(),
                              op.c_str());
            }
            else
            {
                SWSS_LOG_DEBUG(
                    "Task %s - %s success",
                    table_name.c_str(),
                    op.c_str());
            }

            itr = consumer.m_toSync.erase(itr);
        }
    }
}

task_process_status DashAclOrch::taskUpdateDashAclIn(
    const string &key,
    const AclIn &data)
{
    SWSS_LOG_ENTER();

    task_process_status v4_status = task_success, v6_status = task_success;
    if (!data.v4_acl_group_id().empty())
    {
        v4_status = bindAclToEni(m_dash_acl_in_table, key, data.v4_acl_group_id());
    }
    if (v4_status != task_success)
    {
        return v4_status;
    }
    if (!data.v6_acl_group_id().empty())
    {
        v6_status = bindAclToEni(m_dash_acl_in_table, key, data.v6_acl_group_id());
    }
    if (v6_status != task_success)
    {
        return v6_status;
    }
    return task_success;
}

task_process_status DashAclOrch::taskRemoveDashAclIn(
    const string &key)
{
    SWSS_LOG_ENTER();

    return unbindAclFromEni(m_dash_acl_in_table, key);
}

task_process_status DashAclOrch::taskUpdateDashAclOut(
    const string &key,
    const AclOut &data)
{
    SWSS_LOG_ENTER();

    task_process_status v4_status = task_success, v6_status = task_success;
    if (!data.v4_acl_group_id().empty())
    {
        v4_status = bindAclToEni(m_dash_acl_out_table, key, data.v4_acl_group_id());
    }
    if (v4_status != task_success)
    {
        return v4_status;
    }
    if (!data.v6_acl_group_id().empty())
    {
        v6_status = bindAclToEni(m_dash_acl_out_table, key, data.v6_acl_group_id());
    }
    if (v6_status != task_success)
    {
        return v6_status;
    }
    return task_success;
}

task_process_status DashAclOrch::taskRemoveDashAclOut(
    const string &key)
{
    SWSS_LOG_ENTER();

    return unbindAclFromEni(m_dash_acl_out_table, key);
}

task_process_status DashAclOrch::taskUpdateDashAclGroup(
    const string &key,
    const AclGroup &data)
{
    SWSS_LOG_ENTER();

    if (m_dash_acl_group_table.find(key) != m_dash_acl_group_table.end())
    {
        // Update the ACL group's attributes
        SWSS_LOG_WARN("Cannot update attributes of ACL group %s", key.c_str());
        return task_failed;
    }

    sai_ip_addr_family_t ip_version = data.ip_version() == IpVersion::IP_VERSION_IPV4 ? SAI_IP_ADDR_FAMILY_IPV4 : SAI_IP_ADDR_FAMILY_IPV6;
    vector<sai_attribute_t> attrs;
    attrs.emplace_back();
    attrs.back().id = SAI_DASH_ACL_GROUP_ATTR_IP_ADDR_FAMILY;
    attrs.back().value.s32 = ip_version;

    // Guid wasn't mapping to any SAI attributes

    // Create a new ACL group
    DashAclGroupEntry acl_group;
    sai_status_t status = sai_dash_acl_api->create_dash_acl_group(&acl_group.m_dash_acl_group_id, gSwitchId, static_cast<uint32_t>(attrs.size()), attrs.data());
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_WARN("Failed to create ACL group %s, rv: %s", key.c_str(), sai_serialize_status(status).c_str());
        return task_failed;
    }
    acl_group.m_rule_count = 0;
    acl_group.m_ref_count = 0;
    acl_group.m_ip_version = ip_version;
    m_dash_acl_group_table.emplace(key, acl_group);
    SWSS_LOG_NOTICE("Created ACL group %s", key.c_str());

    CrmResourceType crm_rtype = (acl_group.m_ip_version == SAI_IP_ADDR_FAMILY_IPV4) ?
        CrmResourceType::CRM_DASH_IPV4_ACL_GROUP : CrmResourceType::CRM_DASH_IPV6_ACL_GROUP;
    gCrmOrch->incCrmDashAclUsedCounter(crm_rtype, acl_group.m_dash_acl_group_id);

    return task_success;
}

task_process_status DashAclOrch::taskRemoveDashAclGroup(
    const string &key)
{
    SWSS_LOG_ENTER();

    auto acl_group = getAclGroup(key);

    if (acl_group == nullptr || acl_group->m_dash_acl_group_id == SAI_NULL_OBJECT_ID)
    {
        SWSS_LOG_WARN("ACL group %s doesn't exist", key.c_str());
        return task_success;
    }

    // The member rules of group should be removed first
    if (acl_group->m_rule_count != 0)
    {
        SWSS_LOG_INFO("ACL group %s still has %d rules", key.c_str(), acl_group->m_rule_count);
        return task_need_retry;
    }

    // The refer count of group should be cleaned first
    if (acl_group->m_ref_count != 0)
    {
        SWSS_LOG_INFO("ACL group %s still has %d references", key.c_str(), acl_group->m_ref_count);
        return task_need_retry;
    }

    // Remove the ACL group
    sai_status_t status = sai_dash_acl_api->remove_dash_acl_group(acl_group->m_dash_acl_group_id);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_WARN("Failed to remove ACL group %s, rv: %s", key.c_str(), sai_serialize_status(status).c_str());
        return task_failed;
    }
    CrmResourceType crm_rtype = (acl_group->m_ip_version == SAI_IP_ADDR_FAMILY_IPV4) ?
        CrmResourceType::CRM_DASH_IPV4_ACL_GROUP : CrmResourceType::CRM_DASH_IPV6_ACL_GROUP;
    gCrmOrch->decCrmDashAclUsedCounter(crm_rtype, acl_group->m_dash_acl_group_id);

    m_dash_acl_group_table.erase(key);
    SWSS_LOG_NOTICE("Removed ACL group %s", key.c_str());

    return task_success;
}

task_process_status DashAclOrch::taskUpdateDashAclRule(
    const string &key,
    const AclRule &data)
{
    SWSS_LOG_ENTER();

    string group_id, rule_id;
    if (!extractVariables(key, ':', group_id, rule_id))
    {
        SWSS_LOG_WARN("Failed to parse key %s", key.c_str());
        return task_failed;
    }

    auto acl_group = getAclGroup(group_id);
    if (acl_group == nullptr)
    {
        SWSS_LOG_INFO("ACL group %s doesn't exist, waiting for group creating before creating rule %s", group_id.c_str(), rule_id.c_str());
        return task_need_retry;
    }

    if (m_dash_acl_rule_table.find(key) != m_dash_acl_rule_table.end())
    {
        // Remove the old ACL rule
        auto status = taskRemoveDashAclRule(key);
        if (status != task_success)
        {
            return status;
        }
    }

    vector<sai_attribute_t> attrs;

    attrs.emplace_back();
    attrs.back().id = SAI_DASH_ACL_RULE_ATTR_PRIORITY;
    attrs.back().value.u32 = data.priority();

    attrs.emplace_back();
    attrs.back().id = SAI_DASH_ACL_RULE_ATTR_ACTION;
    if (data.action() == Action::ACTION_PERMIT)
    {
        if (data.terminating())
        {
            attrs.back().value.s32 = SAI_DASH_ACL_RULE_ACTION_PERMIT;
        }
        else
        {
            attrs.back().value.s32 = SAI_DASH_ACL_RULE_ACTION_PERMIT_AND_CONTINUE;
        }
    }
    else
    {
        if (data.terminating())
        {
            attrs.back().value.s32 = SAI_DASH_ACL_RULE_ACTION_DENY;
        }
        else
        {
            attrs.back().value.s32 = SAI_DASH_ACL_RULE_ACTION_DENY_AND_CONTINUE;
        }
    }

    attrs.emplace_back();
    attrs.back().id = SAI_DASH_ACL_RULE_ATTR_PROTOCOL;
    vector<uint8_t> protocols;
    if (data.protocol_size() == 0)
    {
        const static vector<uint8_t> ALL_PROTOCOLS = [](){
            vector<uint8_t> protocols;
            for (uint16_t i = 0; i <= 255; i++)
            {
                protocols.push_back(static_cast<uint8_t>(i));
            }
            return protocols;
        }();
        attrs.back().value.u8list.count = static_cast<uint32_t>(ALL_PROTOCOLS.size());
        attrs.back().value.u8list.list = const_cast<uint8_t *>(ALL_PROTOCOLS.data());
    }
    else
    {
        protocols.reserve(data.protocol_size());
        protocols.assign(data.protocol().begin(), data.protocol().end());
        attrs.back().value.u8list.count = static_cast<uint32_t>(protocols.size());
        attrs.back().value.u8list.list = protocols.data();
    }


    const static sai_ip_prefix_t SAI_ALL_IPV4_PREFIX = [](){
        sai_ip_prefix_t ip_prefix;
        ip_prefix.addr_family = SAI_IP_ADDR_FAMILY_IPV4;
        ip_prefix.addr.ip4 = 0;
        ip_prefix.mask.ip4 = 0;
        return ip_prefix;
    }();

    const static sai_ip_prefix_t SAI_ALL_IPV6_PREFIX = [](){
        sai_ip_prefix_t ip_prefix;
        ip_prefix.addr_family = SAI_IP_ADDR_FAMILY_IPV6;
        memset(ip_prefix.addr.ip6, 0, sizeof(ip_prefix.addr.ip6));
        memset(ip_prefix.mask.ip6, 0, sizeof(ip_prefix.mask.ip6));
        return ip_prefix;
    }();

    attrs.emplace_back();
    attrs.back().id = SAI_DASH_ACL_RULE_ATTR_SIP;
    vector<sai_ip_prefix_t> src_prefixes;
    if (data.src_addr_size() == 0)
    {
        src_prefixes.push_back(
            acl_group->m_ip_version == SAI_IP_ADDR_FAMILY_IPV4 ?
            SAI_ALL_IPV4_PREFIX :
            SAI_ALL_IPV6_PREFIX);
    }
    else if (!to_sai(data.src_addr(), src_prefixes))
    {
        return task_invalid_entry;
    }
    attrs.back().value.ipprefixlist.count = static_cast<uint32_t>(src_prefixes.size());
    attrs.back().value.ipprefixlist.list = src_prefixes.data();

    attrs.emplace_back();
    attrs.back().id = SAI_DASH_ACL_RULE_ATTR_DIP;
    vector<sai_ip_prefix_t> dst_prefixes;
    if (data.src_addr_size() == 0)
    {
        dst_prefixes.push_back(
            acl_group->m_ip_version == SAI_IP_ADDR_FAMILY_IPV4 ?
            SAI_ALL_IPV4_PREFIX :
            SAI_ALL_IPV6_PREFIX);
    }
    else if (!to_sai(data.src_addr(), dst_prefixes))
    {
        return task_invalid_entry;
    }
    attrs.back().value.ipprefixlist.count = static_cast<uint32_t>(dst_prefixes.size());
    attrs.back().value.ipprefixlist.list = dst_prefixes.data();

    const static sai_u16_range_t SAI_ALL_PORTS{numeric_limits<uint16_t>::min(), numeric_limits<uint16_t>::max()};

    attrs.emplace_back();
    attrs.back().id = SAI_DASH_ACL_RULE_ATTR_SRC_PORT;
    vector<sai_u16_range_t> src_ports;
    if (data.src_port_size() == 0)
    {
        src_ports.push_back(SAI_ALL_PORTS);
    }
    else if (!to_sai(data.src_port(), src_ports))
    {
        return task_invalid_entry;
    }
    attrs.back().value.u16rangelist.count = static_cast<uint32_t>(src_ports.size());
    attrs.back().value.u16rangelist.list = src_ports.data();

    attrs.emplace_back();
    attrs.back().id = SAI_DASH_ACL_RULE_ATTR_DST_PORT;
    vector<sai_u16_range_t> dst_ports;
    if (data.dst_port_size() == 0)
    {
        dst_ports.push_back(SAI_ALL_PORTS);
    }
    else if (!to_sai(data.dst_port(), dst_ports))
    {
        return task_invalid_entry;
    }
    attrs.back().value.u16rangelist.count = static_cast<uint32_t>(dst_ports.size());
    attrs.back().value.u16rangelist.list = dst_ports.data();

    attrs.emplace_back();
    attrs.back().id = SAI_DASH_ACL_RULE_ATTR_DASH_ACL_GROUP_ID;
    attrs.back().value.oid = acl_group->m_dash_acl_group_id;

    DashAclRuleEntry acl_rule;
    sai_status_t status = sai_dash_acl_api->create_dash_acl_rule(&acl_rule.m_dash_acl_rule_id, gSwitchId, static_cast<uint32_t>(attrs.size()), attrs.data());
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_WARN("Failed to create dash ACL rule %s, rv: %s", key.c_str(), sai_serialize_status(status).c_str());
        return task_failed;
    }
    m_dash_acl_rule_table.emplace(key, acl_rule);
    acl_group->m_rule_count++;
    SWSS_LOG_NOTICE("Created ACL rule %s", key.c_str());

    CrmResourceType crm_rtype = (acl_group->m_ip_version == SAI_IP_ADDR_FAMILY_IPV4) ?
        CrmResourceType::CRM_DASH_IPV4_ACL_RULE : CrmResourceType::CRM_DASH_IPV6_ACL_RULE;
    gCrmOrch->incCrmDashAclUsedCounter(crm_rtype, acl_group->m_dash_acl_group_id);

    return task_success;
}

task_process_status DashAclOrch::taskRemoveDashAclRule(
    const string &key)
{
    SWSS_LOG_ENTER();

    string group_id, rule_id;
    if (!extractVariables(key, ':', group_id, rule_id))
    {
        SWSS_LOG_WARN("Failed to parse key %s", key.c_str());
        return task_failed;
    }

    auto &acl_group = m_dash_acl_group_table[group_id];

    auto itr = m_dash_acl_rule_table.find(key);

    if (itr == m_dash_acl_rule_table.end())
    {
        SWSS_LOG_WARN("ACL rule %s doesn't exist", key.c_str());
        return task_success;
    }

    auto &acl_rule = itr->second;

    bool is_existing = acl_rule.m_dash_acl_rule_id != SAI_NULL_OBJECT_ID;

    if (!is_existing)
    {
        SWSS_LOG_WARN("ACL rule %s doesn't exist", key.c_str());
        return task_success;
    }

    // Remove the ACL group
    sai_status_t status = sai_dash_acl_api->remove_dash_acl_rule(acl_rule.m_dash_acl_rule_id);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_WARN("Failed to remove dash ACL rule %s, rv: %s", key.c_str(), sai_serialize_status(status).c_str());
        return task_failed;
    }
    m_dash_acl_rule_table.erase(itr);
    --acl_group.m_rule_count;

    CrmResourceType crm_resource = (acl_group.m_ip_version == SAI_IP_ADDR_FAMILY_IPV4) ?
        CrmResourceType::CRM_DASH_IPV4_ACL_RULE : CrmResourceType::CRM_DASH_IPV6_ACL_RULE;
    gCrmOrch->decCrmDashAclUsedCounter(crm_resource, acl_group.m_dash_acl_group_id);

    SWSS_LOG_NOTICE("Removed ACL rule %s", key.c_str());

    return task_success;
}

DashAclGroupEntry* DashAclOrch::getAclGroup(const string &group_id)
{
    SWSS_LOG_ENTER();

    auto itr = m_dash_acl_group_table.find(group_id);

    if (itr != m_dash_acl_group_table.end() && itr->second.m_dash_acl_group_id != SAI_NULL_OBJECT_ID)
    {
        return &itr->second;
    }
    else
    {
        return nullptr;
    }
}

task_process_status DashAclOrch::bindAclToEni(DashAclBindTable &acl_bind_table, const string &key, const string &acl_group_id)
{
    SWSS_LOG_ENTER();

    assert(&acl_bind_table == &m_dash_acl_in_table || &acl_bind_table == &m_dash_acl_out_table);
    DashAclDirection direction = ((&acl_bind_table == &m_dash_acl_in_table) ? DashAclDirection::IN : DashAclDirection::OUT);

    string eni;
    uint32_t stage;
    if (!extractVariables(key, ':', eni, stage))
    {
        SWSS_LOG_WARN("Invalid key : %s", key.c_str());
        return task_failed;
    }

    if (acl_group_id.empty())
    {
        SWSS_LOG_WARN("Empty group id in the key : %s", key.c_str());
        return task_failed;
    }

    auto eni_entry = m_dash_orch->getEni(eni);
    if (eni_entry == nullptr)
    {
        SWSS_LOG_INFO("eni %s cannot be found", eni.c_str());
        // The ENI may not be created yet, so we will wait for the ENI to be created
        return task_need_retry;
    }

    auto &acl_bind = acl_bind_table[key];

    auto acl_group = getAclGroup(acl_group_id);
    if (acl_group == nullptr)
    {
        SWSS_LOG_INFO("acl group %s cannot be found, wait for create", acl_group_id.c_str());
        return task_need_retry;
    }

    if (acl_group->m_rule_count <= 0)
    {
        SWSS_LOG_INFO("acl group %s contains 0 rules, waiting for rule creation", acl_group_id.c_str());
        return task_need_retry;
    }

    if (acl_bind.m_acl_group_id == acl_group_id)
    {
        SWSS_LOG_INFO("acl group %s is already bound to %s", acl_group_id.c_str(), key.c_str());
        return task_success;
    }
    else if (!acl_bind.m_acl_group_id.empty())
    {
        auto old_acl_group = getAclGroup(acl_bind.m_acl_group_id);
        if (old_acl_group != nullptr)
        {
            old_acl_group->m_ref_count--;
        }
        else
        {
            SWSS_LOG_WARN("Failed to find old acl group %s", acl_bind.m_acl_group_id.c_str());
        }
    }
    acl_bind.m_acl_group_id = acl_group_id;

    sai_attribute_t attr;
    attr.id = getSaiStage(direction, acl_group->m_ip_version, stage);
    attr.value.oid = acl_group->m_dash_acl_group_id;

    sai_status_t status = sai_dash_eni_api->set_eni_attribute(eni_entry->eni_id, &attr);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_WARN("Failed to bind ACL %s to eni %s attribute, status : %s", key.c_str(), acl_group_id.c_str(), sai_serialize_status(status).c_str());
        return task_failed;
    }
    acl_group->m_ref_count++;

    SWSS_LOG_NOTICE("Bind ACL group %s to %s", acl_group_id.c_str(), key.c_str());

    return task_success;
}

task_process_status DashAclOrch::unbindAclFromEni(DashAclBindTable &acl_bind_table, const string &key)
{
    SWSS_LOG_ENTER();

    assert(&acl_bind_table == &m_dash_acl_in_table || &acl_bind_table == &m_dash_acl_out_table);
    DashAclDirection direction = ((&acl_bind_table == &m_dash_acl_in_table) ? DashAclDirection::IN : DashAclDirection::OUT);

    string eni;
    uint32_t stage;
    if (!extractVariables(key, ':', eni, stage))
    {
        SWSS_LOG_WARN("Invalid key : %s", key.c_str());
        return task_failed;
    }

    auto eni_entry = m_dash_orch->getEni(eni);
    if (eni_entry == nullptr)
    {
        SWSS_LOG_WARN("eni %s cannot be found", eni.c_str());
        return task_failed;
    }

    auto itr = acl_bind_table.find(key);
    if (itr == acl_bind_table.end() || itr->second.m_acl_group_id.empty())
    {
        SWSS_LOG_WARN("ACL %s doesn't exist", key.c_str());
        return task_success;
    }
    auto acl_bind = itr->second;
    acl_bind_table.erase(itr);

    auto acl_group = getAclGroup(acl_bind.m_acl_group_id);
    if (acl_group == nullptr)
    {
        SWSS_LOG_WARN("Invalid acl group id : %s", acl_bind.m_acl_group_id.c_str());
        return task_failed;
    }

    sai_attribute_t attr;
    attr.id = getSaiStage(direction, acl_group->m_ip_version, stage);
    attr.value.oid = SAI_NULL_OBJECT_ID;

    sai_status_t status = sai_dash_eni_api->set_eni_attribute(eni_entry->eni_id, &attr);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_WARN("Failed to unbind ACL %s to eni %s attribute, status : %s", key.c_str(), acl_bind.m_acl_group_id.c_str(), sai_serialize_status(status).c_str());
        return task_failed;
    }

    acl_group->m_ref_count--;

    SWSS_LOG_NOTICE("Unbind ACL group %s from %s", acl_bind.m_acl_group_id.c_str(), key.c_str());

    return task_success;
}
