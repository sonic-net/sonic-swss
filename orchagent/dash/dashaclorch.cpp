#include <swss/logger.h>
#include <swss/stringutility.h>
#include <swss/redisutility.h>
#include <swss/ipaddress.h>

#include <swssnet.h>

#include "crmorch.h"
#include "dashaclorch.h"

using namespace std;
using namespace swss;


extern sai_dash_acl_api_t* sai_dash_acl_api;
extern sai_dash_eni_api_t* sai_dash_eni_api;
extern sai_object_id_t gSwitchId;
extern CrmOrch *gCrmOrch;

template <typename T, typename... Args>
static bool extractVariables(const string &input, char delimiter, T &output, Args &... args)
{
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

template<class T>
static bool getValue(
    const DashAclOrch::TaskArgs &ta,
    const string &field,
    T &value)
{
    SWSS_LOG_ENTER();

    auto value_opt = swss::fvsGetValue(ta, field, true);
    if (!value_opt)
    {
        SWSS_LOG_DEBUG("Cannot find field : %s", field.c_str());
        return false;
    }

    try
    {
        lexical_convert(*value_opt, value);
        return true;
    }
    catch(const exception &err)
    {
        SWSS_LOG_WARN("Cannot convert field %s to type %s (%s)", field.c_str(), typeid(T).name(), err.what());
        return false;
    }
}

namespace swss {

template<>
inline void lexical_convert(const string &buffer, DashAclStage &stage)
{
    SWSS_LOG_ENTER();

    if (buffer == "1")
    {
        stage = DashAclStage::STAGE1;
    }
    else if (buffer == "2")
    {
        stage = DashAclStage::STAGE2;
    }
    else if (buffer == "3")
    {
        stage = DashAclStage::STAGE3;
    }
    else if (buffer == "4")
    {
        stage = DashAclStage::STAGE4;
    }
    else if (buffer == "5")
    {
        stage = DashAclStage::STAGE5;
    }
    else
    {
        SWSS_LOG_ERROR("Invalid stage : %s", buffer.c_str());
        throw invalid_argument("Invalid stage");
    }

}

template<>
inline void lexical_convert(const string &buffer, sai_ip_addr_family_t &ip_version)
{
    SWSS_LOG_ENTER();

    if (buffer == "ipv4")
    {
        ip_version = SAI_IP_ADDR_FAMILY_IPV4;
    }
    else if (buffer == "ipv6")
    {
        ip_version = SAI_IP_ADDR_FAMILY_IPV6;
    }
    else
    {
        SWSS_LOG_ERROR("Invalid ip version : %s", buffer.c_str());
        throw invalid_argument("Invalid ip version");
    }
}

template<>
inline void lexical_convert(const string &buffer, DashAclRuleEntry::Action &action)
{
    SWSS_LOG_ENTER();

    if (buffer == "allow")
    {
        action = DashAclRuleEntry::Action::ALLOW;
    }
    else if (buffer == "deny")
    {
        action = DashAclRuleEntry::Action::DENY;
    }
    else
    {
        SWSS_LOG_ERROR("Invalid action : %s", buffer.c_str());
        throw invalid_argument("Invalid action");
    }
}

template<>
inline void lexical_convert(const string &buffer, bool &terminating)
{
    SWSS_LOG_ENTER();

    if (buffer == "true" || buffer == "True" || buffer == "1")
    {
        terminating = true;
    }
    else if (buffer == "false" || buffer == "False" || buffer == "0")
    {
        terminating = false;
    }
    else
    {
        SWSS_LOG_ERROR("Invalid terminating : %s", buffer.c_str());
        throw invalid_argument("Invalid terminating");
    }
}

template<>
inline void lexical_convert(const string &buffer, vector<uint8_t> &protocols)
{
    SWSS_LOG_ENTER();

    auto tokens = tokenize(buffer, ',');
    protocols.clear();
    protocols.reserve(tokens.size());
    for (auto &token : tokens)
    {
        uint32_t protocol;
        lexical_convert(token, protocol);
        protocols.push_back(static_cast<uint8_t>(protocol));
    }
}

template<>
inline void lexical_convert(const string &buffer, vector<sai_ip_prefix_t> &prefixes)
{
    SWSS_LOG_ENTER();

    auto tokens = tokenize(buffer, ',');
    prefixes.clear();
    prefixes.reserve(tokens.size());
    for (auto &token : tokens)
    {
        sai_ip_prefix_t prefix;
        IpPrefix ipPrefix(token);
        swss::copy(prefix, ipPrefix);
        prefixes.push_back(prefix);
    }
}

template<>
inline void lexical_convert(const string &buffer, vector<sai_u16_range_t> &ports)
{
    SWSS_LOG_ENTER();

    auto tokens = tokenize(buffer, ',');
    ports.clear();
    ports.reserve(tokens.size());
    for (auto &token : tokens)
    {
        sai_u16_range_t port;
        if (token.find('-') == string::npos)
        {
            // Only one port
            lexical_convert(token, port.min);
            port.max = port.min;
        }
        else if (!extractVariables(token, '-', port.min, port.max))
        {
            // Range
            SWSS_LOG_ERROR("Invalid port range : %s", token.c_str());
            throw invalid_argument("Invalid port range");
        }
        ports.push_back(port);
    }
}

}

static bool operator==(const sai_u16_range_t& a, const sai_u16_range_t& b)
{
    SWSS_LOG_ENTER();

    return a.min == b.min && a.max == b.max;
}

template<class T>
static bool updateValue(
    const DashAclOrch::TaskArgs &ta,
    const string &field,
    boost::optional<T> &opt)
{
    SWSS_LOG_ENTER();

    T value;

    if (!getValue(ta, field, value))
    {
        return false;
    }

    if (opt && opt.value() == value)
    {
        return false;
    }

    opt = value;

    return true;
}

sai_attr_id_t getSaiStage(DashAclDirection d, sai_ip_addr_family_t f, DashAclStage s)
{
    const static map<tuple<DashAclDirection, sai_ip_addr_family_t, DashAclStage>, sai_attr_id_t> StageMaps =
        {
            {{IN, SAI_IP_ADDR_FAMILY_IPV4, DashAclStage::STAGE1}, SAI_ENI_ATTR_INBOUND_V4_STAGE1_DASH_ACL_GROUP_ID},
            {{IN, SAI_IP_ADDR_FAMILY_IPV4, DashAclStage::STAGE2}, SAI_ENI_ATTR_INBOUND_V4_STAGE2_DASH_ACL_GROUP_ID},
            {{IN, SAI_IP_ADDR_FAMILY_IPV4, DashAclStage::STAGE3}, SAI_ENI_ATTR_INBOUND_V4_STAGE3_DASH_ACL_GROUP_ID},
            {{IN, SAI_IP_ADDR_FAMILY_IPV4, DashAclStage::STAGE4}, SAI_ENI_ATTR_INBOUND_V4_STAGE4_DASH_ACL_GROUP_ID},
            {{IN, SAI_IP_ADDR_FAMILY_IPV4, DashAclStage::STAGE5}, SAI_ENI_ATTR_INBOUND_V4_STAGE5_DASH_ACL_GROUP_ID},
            {{IN, SAI_IP_ADDR_FAMILY_IPV6, DashAclStage::STAGE1}, SAI_ENI_ATTR_INBOUND_V6_STAGE1_DASH_ACL_GROUP_ID},
            {{IN, SAI_IP_ADDR_FAMILY_IPV6, DashAclStage::STAGE2}, SAI_ENI_ATTR_INBOUND_V6_STAGE2_DASH_ACL_GROUP_ID},
            {{IN, SAI_IP_ADDR_FAMILY_IPV6, DashAclStage::STAGE3}, SAI_ENI_ATTR_INBOUND_V6_STAGE3_DASH_ACL_GROUP_ID},
            {{IN, SAI_IP_ADDR_FAMILY_IPV6, DashAclStage::STAGE4}, SAI_ENI_ATTR_INBOUND_V6_STAGE4_DASH_ACL_GROUP_ID},
            {{IN, SAI_IP_ADDR_FAMILY_IPV6, DashAclStage::STAGE5}, SAI_ENI_ATTR_INBOUND_V6_STAGE5_DASH_ACL_GROUP_ID},
            {{OUT, SAI_IP_ADDR_FAMILY_IPV4, DashAclStage::STAGE1}, SAI_ENI_ATTR_OUTBOUND_V4_STAGE1_DASH_ACL_GROUP_ID},
            {{OUT, SAI_IP_ADDR_FAMILY_IPV4, DashAclStage::STAGE2}, SAI_ENI_ATTR_OUTBOUND_V4_STAGE2_DASH_ACL_GROUP_ID},
            {{OUT, SAI_IP_ADDR_FAMILY_IPV4, DashAclStage::STAGE3}, SAI_ENI_ATTR_OUTBOUND_V4_STAGE3_DASH_ACL_GROUP_ID},
            {{OUT, SAI_IP_ADDR_FAMILY_IPV4, DashAclStage::STAGE4}, SAI_ENI_ATTR_OUTBOUND_V4_STAGE4_DASH_ACL_GROUP_ID},
            {{OUT, SAI_IP_ADDR_FAMILY_IPV4, DashAclStage::STAGE5}, SAI_ENI_ATTR_OUTBOUND_V4_STAGE5_DASH_ACL_GROUP_ID},
            {{OUT, SAI_IP_ADDR_FAMILY_IPV6, DashAclStage::STAGE1}, SAI_ENI_ATTR_OUTBOUND_V6_STAGE1_DASH_ACL_GROUP_ID},
            {{OUT, SAI_IP_ADDR_FAMILY_IPV6, DashAclStage::STAGE2}, SAI_ENI_ATTR_OUTBOUND_V6_STAGE2_DASH_ACL_GROUP_ID},
            {{OUT, SAI_IP_ADDR_FAMILY_IPV6, DashAclStage::STAGE3}, SAI_ENI_ATTR_OUTBOUND_V6_STAGE3_DASH_ACL_GROUP_ID},
            {{OUT, SAI_IP_ADDR_FAMILY_IPV6, DashAclStage::STAGE4}, SAI_ENI_ATTR_OUTBOUND_V6_STAGE4_DASH_ACL_GROUP_ID},
            {{OUT, SAI_IP_ADDR_FAMILY_IPV6, DashAclStage::STAGE5}, SAI_ENI_ATTR_OUTBOUND_V6_STAGE5_DASH_ACL_GROUP_ID},
        };

    auto stage = StageMaps.find({d, f, s});
    if (stage == StageMaps.end())
    {
        SWSS_LOG_ERROR("Invalid stage %d %d %d", d, f, s);
        throw runtime_error("Invalid stage");
    }

    return stage->second;
}

DashAclOrch::DashAclOrch(DBConnector *db, const vector<string> &tables, DashOrch *dash_orch) :
    Orch(db, tables),
    m_dash_orch(dash_orch)
{
    SWSS_LOG_ENTER();

    assert(m_dash_orch);
}

void DashAclOrch::doTask(Consumer &consumer)
{
    SWSS_LOG_ENTER();

    using TaskType = tuple<const string, const string>;
    using TaskFunc = task_process_status (DashAclOrch::*)(
        const string &,
        const TaskArgs &);
    const static map<TaskType, TaskFunc> TaskMap = {
        {{APP_DASH_ACL_IN_TABLE_NAME, SET_COMMAND}, &DashAclOrch::taskUpdateDashAclIn},
        {{APP_DASH_ACL_IN_TABLE_NAME, DEL_COMMAND}, &DashAclOrch::taskRemoveDashAclIn},
        {{APP_DASH_ACL_OUT_TABLE_NAME, SET_COMMAND}, &DashAclOrch::taskUpdateDashAclOut},
        {{APP_DASH_ACL_OUT_TABLE_NAME, DEL_COMMAND}, &DashAclOrch::taskRemoveDashAclOut},
        {{APP_DASH_ACL_GROUP_TABLE_NAME, SET_COMMAND}, &DashAclOrch::taskUpdateDashAclGroup},
        {{APP_DASH_ACL_GROUP_TABLE_NAME, DEL_COMMAND}, &DashAclOrch::taskRemoveDashAclGroup},
        {{APP_DASH_ACL_RULE_TABLE_NAME, SET_COMMAND}, &DashAclOrch::taskUpdateDashAclRule},
        {{APP_DASH_ACL_RULE_TABLE_NAME, DEL_COMMAND}, &DashAclOrch::taskRemoveDashAclRule},
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
            task_status = (this->*task->second)(
                kfvKey(message),
                kfvFieldsValues(message));
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
    const TaskArgs &data)
{
    SWSS_LOG_ENTER();

    return bindAclToEni(m_dash_acl_in_table, key, data);
}

task_process_status DashAclOrch::taskRemoveDashAclIn(
    const string &key,
    const TaskArgs &data)
{
    SWSS_LOG_ENTER();

    return unbindAclFromEni(m_dash_acl_in_table, key, data);
}

task_process_status DashAclOrch::taskUpdateDashAclOut(
    const string &key,
    const TaskArgs &data)
{
    SWSS_LOG_ENTER();

    return bindAclToEni(m_dash_acl_out_table, key, data);
}

task_process_status DashAclOrch::taskRemoveDashAclOut(
    const string &key,
    const TaskArgs &data)
{
    SWSS_LOG_ENTER();

    return unbindAclFromEni(m_dash_acl_out_table, key, data);
}

task_process_status DashAclOrch::taskUpdateDashAclGroup(
    const string &key,
    const TaskArgs &data)
{
    SWSS_LOG_ENTER();

    auto &acl_group = m_dash_acl_group_table[key];
    bool is_existing = acl_group.m_dash_acl_group_id != SAI_NULL_OBJECT_ID;

    vector<sai_attribute_t> attrs;

    if (updateValue(data, "ip_version", acl_group.m_ip_version) || (acl_group.m_ip_version && !is_existing))
    {
        attrs.emplace_back();
        attrs.back().id = SAI_DASH_ACL_GROUP_ATTR_IP_ADDR_FAMILY;
        attrs.back().value.s32 = *(acl_group.m_ip_version);
    }

    // Guid wasn't mapping to any SAI attributes
    updateValue(data, "guid", acl_group.m_guid);

    if (!is_existing && attrs.empty())
    {
        // Assign default values
        if (!acl_group.m_ip_version)
        {
            acl_group.m_ip_version = SAI_IP_ADDR_FAMILY_IPV4;
            attrs.emplace_back();
            attrs.back().id = SAI_DASH_ACL_GROUP_ATTR_IP_ADDR_FAMILY;
            attrs.back().value.s32 = *(acl_group.m_ip_version);
        }
    }

    if (!attrs.empty())
    {
        if (!is_existing)
        {
            // Create a new ACL group
            sai_status_t status = sai_dash_acl_api->create_dash_acl_group(&acl_group.m_dash_acl_group_id, gSwitchId, static_cast<uint32_t>(attrs.size()), attrs.data());
            if (status != SAI_STATUS_SUCCESS)
            {
                SWSS_LOG_ERROR("Failed to create ACL group %s, rv: %s", key.c_str(), sai_serialize_status(status).c_str());
                return task_failed;
            }
            acl_group.m_rule_count = 0;

            CrmResourceType crm_rtype = acl_group.m_ip_version == SAI_IP_ADDR_FAMILY_IPV4 ? 
                CrmResourceType::CRM_DASH_IPV4_ACL_GROUP : CrmResourceType::CRM_DASH_IPV6_ACL_GROUP;
            gCrmOrch->incCrmDashAclUsedCounter(crm_rtype, acl_group.m_dash_acl_group_id);

            SWSS_LOG_NOTICE("Created ACL group %s", key.c_str());
        }
        else
        {
            // Update the ACL group's attributes
            SWSS_LOG_WARN("Cannot update attributes of ACL group %s", key.c_str());
            return task_failed;
        }
    }

    return task_success;
}

task_process_status DashAclOrch::taskRemoveDashAclGroup(
    const string &key,
    const TaskArgs &data)
{
    SWSS_LOG_ENTER();

    auto acl_group = getAclGroup(key);

    if (acl_group == nullptr)
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
        SWSS_LOG_ERROR("Failed to remove ACL group %s, rv: %s", key.c_str(), sai_serialize_status(status).c_str());
        return task_failed;
    }
    CrmResourceType crm_rtype = acl_group->m_ip_version == SAI_IP_ADDR_FAMILY_IPV4 ? 
        CrmResourceType::CRM_DASH_IPV4_ACL_GROUP : CrmResourceType::CRM_DASH_IPV6_ACL_GROUP;
    gCrmOrch->decCrmDashAclGroupUsedCounter(crm_rtype, acl_group->m_dash_acl_group_id);

    m_dash_acl_group_table.erase(key);
    SWSS_LOG_NOTICE("Removed ACL group %s", key.c_str());

    return task_success;
}

task_process_status DashAclOrch::taskUpdateDashAclRule(
    const string &key,
    const TaskArgs &data)
{
    SWSS_LOG_ENTER();

    string group_id, rule_id;
    if (!extractVariables(key, ':', group_id, rule_id))
    {
        SWSS_LOG_ERROR("Failed to parse key %s", key.c_str());
        return task_failed;
    }

    auto acl_group = getAclGroup(group_id);
    if (acl_group == nullptr)
    {
        SWSS_LOG_INFO("ACL group %s doesn't exist, waiting for group creating before creating rule %s", group_id.c_str(), rule_id.c_str());
        return task_need_retry;
    }

    auto &acl_rule = m_dash_acl_rule_table[key];
    bool is_existing = acl_rule.m_dash_acl_rule_id != SAI_NULL_OBJECT_ID;
    vector<sai_attribute_t> attrs;

    if (updateValue(data, "priority", acl_rule.m_priority) || (acl_rule.m_priority && !is_existing))
    {
        attrs.emplace_back();
        attrs.back().id = SAI_DASH_ACL_RULE_ATTR_PRIORITY;
        attrs.back().value.u32 = *(acl_rule.m_priority);
    }

    bool update_action = false;
    update_action |= updateValue(data, "action", acl_rule.m_action);
    update_action |= updateValue(data, "terminating", acl_rule.m_terminating);
    if (update_action || (acl_rule.m_action && acl_rule.m_terminating && !is_existing))
    {
        attrs.emplace_back();
        attrs.back().id = SAI_DASH_ACL_RULE_ATTR_ACTION;
        if (*(acl_rule.m_action) == DashAclRuleEntry::Action::ALLOW)
        {
            if (*(acl_rule.m_terminating))
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
            if (*(acl_rule.m_terminating))
            {
                attrs.back().value.s32 = SAI_DASH_ACL_RULE_ACTION_DENY;
            }
            else
            {
                attrs.back().value.s32 = SAI_DASH_ACL_RULE_ACTION_DENY_AND_CONTINUE;
            }
        }
    }

    if (updateValue(data, "protocol", acl_rule.m_protocols) || (acl_rule.m_protocols && !is_existing))
    {
        attrs.emplace_back();
        attrs.back().id = SAI_DASH_ACL_RULE_ATTR_PROTOCOL;
        attrs.back().value.u8list.count = static_cast<uint32_t>(acl_rule.m_protocols.value().size());
        attrs.back().value.u8list.list = acl_rule.m_protocols.value().data();
    }

    if (updateValue(data, "src_addr", acl_rule.m_src_prefixes) || (acl_rule.m_src_prefixes && !is_existing))
    {
        attrs.emplace_back();
        attrs.back().id = SAI_DASH_ACL_RULE_ATTR_SIP;
        attrs.back().value.ipprefixlist.count = static_cast<uint32_t>(acl_rule.m_src_prefixes.value().size());
        attrs.back().value.ipprefixlist.list = acl_rule.m_src_prefixes.value().data();
    }

    if (updateValue(data, "dst_addr", acl_rule.m_dst_prefixes) || (acl_rule.m_dst_prefixes && !is_existing))
    {
        attrs.emplace_back();
        attrs.back().id = SAI_DASH_ACL_RULE_ATTR_DIP;
        attrs.back().value.ipprefixlist.count = static_cast<uint32_t>(acl_rule.m_dst_prefixes.value().size());
        attrs.back().value.ipprefixlist.list = acl_rule.m_dst_prefixes.value().data();
    }

    if (updateValue(data, "src_port", acl_rule.m_src_ports) || (acl_rule.m_src_ports && !is_existing))
    {
        attrs.emplace_back();
        attrs.back().id = SAI_DASH_ACL_RULE_ATTR_SRC_PORT;
        attrs.back().value.u16rangelist.count = static_cast<uint32_t>(acl_rule.m_src_ports.value().size());
        attrs.back().value.u16rangelist.list = acl_rule.m_src_ports.value().data();
    }

    if (updateValue(data, "dst_port", acl_rule.m_dst_ports) || (acl_rule.m_dst_ports && !is_existing))
    {
        attrs.emplace_back();
        attrs.back().id = SAI_DASH_ACL_RULE_ATTR_DST_PORT;
        attrs.back().value.u16rangelist.count = static_cast<uint32_t>(acl_rule.m_dst_ports.value().size());
        attrs.back().value.u16rangelist.list = acl_rule.m_dst_ports.value().data();
    }

    if (!is_existing)
    {
        // Mandatory on create attributes should be setted, otherwise assign the default value.
        // If the attributes don't have default value, just skip and wait for the user to set the value at the next message
        if (!acl_rule.m_protocols)
        {
            const static vector<uint8_t> all_protocols = [](){
                vector<uint8_t> protocols;
                for (uint16_t i = 0; i <= 255; i++)
                {
                    protocols.push_back(static_cast<uint8_t>(i));
                }
                return protocols;
            }();
            acl_rule.m_protocols = all_protocols;
            attrs.emplace_back();
            attrs.back().id = SAI_DASH_ACL_RULE_ATTR_PROTOCOL;
            attrs.back().value.u8list.count = static_cast<uint32_t>(acl_rule.m_protocols.value().size());
            attrs.back().value.u8list.list = acl_rule.m_protocols.value().data();
        }

        if (!acl_rule.m_priority || !acl_rule.m_dst_prefixes || !acl_rule.m_src_prefixes || !acl_rule.m_dst_ports || !acl_rule.m_src_ports)
        {
            SWSS_LOG_WARN("ACL rule %s doesn't have all mandatory attributes, waiting for user to set the value", key.c_str());
            return task_success;
        }
    }

    if (!attrs.empty())
    {
        if (!is_existing)
        {
            // Create a new ACL rule

            attrs.emplace_back();
            attrs.back().id = SAI_DASH_ACL_RULE_ATTR_DASH_ACL_GROUP_ID;
            attrs.back().value.oid = acl_group->m_dash_acl_group_id;

            sai_status_t status = sai_dash_acl_api->create_dash_acl_rule(&acl_rule.m_dash_acl_rule_id, gSwitchId, static_cast<uint32_t>(attrs.size()), attrs.data());
            if (status != SAI_STATUS_SUCCESS)
            {
                SWSS_LOG_ERROR("Failed to create dash ACL rule %s, rv: %s", key.c_str(), sai_serialize_status(status).c_str());
                return task_failed;
            }
            acl_group->m_rule_count++;

            CrmResourceType crm_rtype = acl_group->m_ip_version == SAI_IP_ADDR_FAMILY_IPV4 ? 
                CrmResourceType::CRM_DASH_IPV4_ACL_RULE : CrmResourceType::CRM_DASH_IPV6_ACL_RULE;
            gCrmOrch->incCrmDashAclUsedCounter(crm_rtype, acl_group->m_dash_acl_group_id);

            SWSS_LOG_NOTICE("Created ACL rule %s", key.c_str());
        }
        else
        {
            // Update the ACL rule's attributes
            for (const auto &attr : attrs)
            {
                sai_status_t status = sai_dash_acl_api->set_dash_acl_rule_attribute(acl_rule.m_dash_acl_rule_id, &attr);
                if (status != SAI_STATUS_SUCCESS)
                {
                    SWSS_LOG_ERROR("Failed to update attribute %d to dash ACL rule %s, rv:%s", attr.id, key.c_str(), sai_serialize_status(status).c_str());
                    return task_failed;
                }
            }
        }
    }

    return task_success;
}

task_process_status DashAclOrch::taskRemoveDashAclRule(
    const string &key,
    const TaskArgs &data)
{
    SWSS_LOG_ENTER();

    string group_id, rule_id;
    if (!extractVariables(key, ':', group_id, rule_id))
    {
        SWSS_LOG_ERROR("Failed to parse key %s", key.c_str());
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
        SWSS_LOG_ERROR("Failed to remove dash ACL rule %s, rv: %s", key.c_str(), sai_serialize_status(status).c_str());
        return task_failed;
    }
    m_dash_acl_rule_table.erase(itr);
    --acl_group.m_rule_count;

    CrmResourceType crm_resource = acl_group.m_ip_version == SAI_IP_ADDR_FAMILY_IPV4 ? 
        CrmResourceType::CRM_DASH_IPV4_ACL_RULE : CrmResourceType::CRM_DASH_IPV6_ACL_RULE;
    gCrmOrch->decCrmDashAclGroupUsedCounter(crm_resource, acl_group.m_dash_acl_group_id);

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

task_process_status DashAclOrch::bindAclToEni(DashAclTable &acl_table, const string &key, const TaskArgs &data)
{
    SWSS_LOG_ENTER();

    assert(&acl_table == &m_dash_acl_in_table || &acl_table == &m_dash_acl_out_table);
    DashAclDirection direction = ((&acl_table == &m_dash_acl_in_table) ? DashAclDirection::IN : DashAclDirection::OUT);

    string eni;
    DashAclStage stage;
    if (!extractVariables(key, ':', eni, stage))
    {
        SWSS_LOG_ERROR("Invalid key : %s", key.c_str());
        return task_failed;
    }

    auto eni_entry = m_dash_orch->getEni(eni);
    if (eni_entry == nullptr)
    {
        SWSS_LOG_INFO("eni %s cannot be found", eni.c_str());
        // The ENI may not be created yet, so we will wait for the ENI to be created
        return task_need_retry;
    }

    auto &acl = acl_table[key];
    sai_attribute_t attr;

    if (updateValue(data, "acl_group_id", acl.m_acl_group_id))
    {
        auto acl_group = getAclGroup(*(acl.m_acl_group_id));
        if (acl_group == nullptr)
        {
            SWSS_LOG_INFO("acl group %s cannot be found, wait for create", acl.m_acl_group_id->c_str());
            acl.m_acl_group_id.reset();
            return task_need_retry;
        }

        if (acl_group->m_rule_count <= 0)
        {
            SWSS_LOG_INFO("acl group %s contains 0 rules, waiting for rule creation", acl.m_acl_group_id->c_str());
            acl.m_acl_group_id.reset();
            return task_need_retry;
        }

        attr.id = getSaiStage(direction, *(acl_group->m_ip_version), stage);
        attr.value.oid = acl_group->m_dash_acl_group_id;
    }
    else
    {
        if (!acl.m_acl_group_id)
        {
            SWSS_LOG_WARN("acl_group_id is not specified in %s", key.c_str());
            return task_failed;
        }
        return task_success;
    }

    sai_status_t status = sai_dash_eni_api->set_eni_attribute(eni_entry->eni_id, &attr);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to bind ACL %s to eni %s attribute, status : %s", key.c_str(), acl.m_acl_group_id->c_str(), sai_serialize_status(status).c_str());
        return task_failed;
    }
    getAclGroup(*(acl.m_acl_group_id))->m_ref_count++;

    SWSS_LOG_NOTICE("Bind ACL group %s to %s", acl.m_acl_group_id->c_str(), key.c_str());

    return task_success;
}

task_process_status DashAclOrch::unbindAclFromEni(DashAclTable &acl_table, const string &key, const TaskArgs &data)
{
    SWSS_LOG_ENTER();

    assert(&acl_table == &m_dash_acl_in_table || &acl_table == &m_dash_acl_out_table);
    DashAclDirection direction = ((&acl_table == &m_dash_acl_in_table) ? DashAclDirection::IN : DashAclDirection::OUT);

    string eni;
    DashAclStage stage;
    if (!extractVariables(key, ':', eni, stage))
    {
        SWSS_LOG_ERROR("Invalid key : %s", key.c_str());
        return task_failed;
    }

    auto eni_entry = m_dash_orch->getEni(eni);
    if (eni_entry == nullptr)
    {
        SWSS_LOG_WARN("eni %s cannot be found", eni.c_str());
        return task_failed;
    }

    auto itr = acl_table.find(key);
    if (itr == acl_table.end())
    {
        SWSS_LOG_WARN("ACL %s doesn't exist", key.c_str());
        return task_success;
    }
    auto acl = itr->second;
    acl_table.erase(itr);

    auto acl_group = getAclGroup(*(acl.m_acl_group_id));
    if (acl_group == nullptr)
    {
        SWSS_LOG_WARN("Invalid acl group id : %s", acl.m_acl_group_id->c_str());
        return task_failed;
    }

    sai_attribute_t attr;
    attr.id = getSaiStage(direction, *(acl_group->m_ip_version), stage);
    attr.value.oid = SAI_NULL_OBJECT_ID;

    sai_status_t status = sai_dash_eni_api->set_eni_attribute(eni_entry->eni_id, &attr);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to unbind ACL %s to eni %s attribute, status : %s", key.c_str(), acl.m_acl_group_id->c_str(), sai_serialize_status(status).c_str());
        return task_failed;
    }

    acl_group->m_ref_count--;

    SWSS_LOG_NOTICE("Unbind ACL group %s from %s", acl.m_acl_group_id->c_str(), key.c_str());

    return task_success;
}
