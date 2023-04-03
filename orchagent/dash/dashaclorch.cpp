#include <swss/logger.h>
#include <swss/stringutility.h>
#include <swss/redisutility.h>
#include <swss/ipaddress.h>

#include <swssnet.h>

#include <functional>
#include <limits>

#include "dashaclorch.h"
#include "taskworker.h"


using namespace std;
using namespace swss;
using namespace dash::acl;
using namespace google::protobuf;


extern sai_dash_acl_api_t* sai_dash_acl_api;
extern sai_dash_eni_api_t* sai_dash_eni_api;
extern sai_object_id_t gSwitchId;

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

sai_ip_prefix_t pbPrefix2SAIPrefix(const dash::types::IpPrefix *pb_prefix, sai_ip_addr_family_t addr_family)
{
    SWSS_LOG_ENTER();

    if (pb_prefix == nullptr)
    {
        const static sai_ip_prefix_t ALL_IPV4_PREFIX = [](){
            sai_ip_prefix_t ip_prefix;
            ip_prefix.addr_family = SAI_IP_ADDR_FAMILY_IPV4;
            ip_prefix.addr.ip4 = 0;
            ip_prefix.mask.ip4 = 0;
            return ip_prefix;
        }();
        const static sai_ip_prefix_t ALL_IPV6_PREFIX = [](){
            sai_ip_prefix_t ip_prefix;
            ip_prefix.addr_family = SAI_IP_ADDR_FAMILY_IPV6;
            memset(ip_prefix.addr.ip6, 0, sizeof(ip_prefix.addr.ip6));
            memset(ip_prefix.mask.ip6, 0, sizeof(ip_prefix.mask.ip6));
            return ip_prefix;
        }();
        if (addr_family == SAI_IP_ADDR_FAMILY_IPV4)
        {
            return ALL_IPV4_PREFIX;
        }
        else
        {
            return ALL_IPV6_PREFIX;
        }
    }

    sai_ip_prefix_t sai_prefix;
    sai_prefix.addr_family = addr_family;
    if (addr_family == SAI_IP_ADDR_FAMILY_IPV4)
    {
        if (!pb_prefix->ip().has_ipv4() || !pb_prefix->mask().has_ipv4())
        {
            SWSS_LOG_WARN("The expected ip version is IPv4 at the message %s", pb_prefix->DebugString().c_str());
            throw invalid_argument("The expected ip version is IPv4");
        }
        sai_prefix.addr.ip4 = pb_prefix->ip().ipv4();
        sai_prefix.mask.ip4 = pb_prefix->mask().ipv4();
    }
    else
    {
        if (!pb_prefix->ip().has_ipv6() || !pb_prefix->mask().has_ipv6())
        {
            SWSS_LOG_WARN("The expected ip version is IPv6 at the message %s", pb_prefix->DebugString().c_str());
            throw invalid_argument("The expected ip version is IPv6");
        }
        memcpy(sai_prefix.addr.ip6, pb_prefix->ip().ipv6().c_str(), sizeof(sai_prefix.addr.ip6));
        memcpy(sai_prefix.mask.ip6, pb_prefix->mask().ipv6().c_str(), sizeof(sai_prefix.mask.ip6));
    }

    return sai_prefix;
}

vector<sai_ip_prefix_t> pbPrefixes2SAIPrefixes(
    RepeatedPtrField<dash::types::IpPrefix>::const_iterator begin,
    RepeatedPtrField<dash::types::IpPrefix>::const_iterator end,
    sai_ip_addr_family_t addr_family)
{
    SWSS_LOG_ENTER();

    vector<sai_ip_prefix_t> sai_prefixes;

    if (begin != end)
    {
        sai_prefixes.reserve(end - begin);
        try
        {
            for (auto it = begin; it != end; ++it)
            {
                sai_prefixes.push_back(pbPrefix2SAIPrefix(&*it, addr_family));
            }
        }
        catch(const std::invalid_argument&)
        {
            sai_prefixes.clear();
            return sai_prefixes;
        }
    }
    else
    {
        sai_prefixes.push_back(pbPrefix2SAIPrefix(nullptr, addr_family));
    }

    return sai_prefixes;
}

template<typename RangeType>
vector<RangeType> pbRangeOrValues2SAIRanges(
    RepeatedPtrField<dash::types::ValueOrRange>::const_iterator begin,
    RepeatedPtrField<dash::types::ValueOrRange>::const_iterator end)
{
    SWSS_LOG_ENTER();

    vector<RangeType> sai_ranges;
    using range_type = typename conditional<is_same<RangeType, sai_u32_range_t>::value, uint32_t,
                        typename conditional<is_same<RangeType, sai_s32_range_t>::value, int32_t,
                            typename conditional<is_same<RangeType, sai_u16_range_t>::value, uint16_t,
                            void>::type>::type>::type;

    if (begin != end)
    {
        sai_ranges.reserve(end - begin);
        for (auto it = begin; it != end; ++it)
        {
            if (it->has_range())
            {
                if (it->range().min() > it->range().max() || it->range().min() < numeric_limits<range_type>::min() || it->range().max() > numeric_limits<range_type>::max())
                {
                    SWSS_LOG_WARN("The range %s is invalid", it->range().DebugString().c_str());
                    sai_ranges.clear();
                    return sai_ranges;
                }
                sai_ranges.push_back({static_cast<range_type>(it->range().min()), static_cast<range_type>(it->range().max())});
            }
            else
            {
                if (it->value() < numeric_limits<range_type>::min() || it->value() > numeric_limits<range_type>::max())
                {
                    SWSS_LOG_WARN("The value %s is invalid", it->value());
                    sai_ranges.clear();
                    return sai_ranges;
                }
                sai_ranges.push_back({static_cast<range_type>(it->value()), static_cast<range_type>(it->value())});
            }
        }
    }
    else
    {
        sai_ranges.push_back({numeric_limits<range_type>::min(), numeric_limits<range_type>::max()});
    }

    return sai_ranges;
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

    const static TaskMap TaskMap = {
        PbWorker<AclIn>::makeMemberTask(APP_DASH_ACL_IN_TABLE_NAME, SET_COMMAND, &DashAclOrch::taskUpdateDashAclIn, this),
        KeyOnlyWorker::makeMemberTask(APP_DASH_ACL_IN_TABLE_NAME, DEL_COMMAND, &DashAclOrch::taskRemoveDashAclIn, this),
        PbWorker<AclOut>::makeMemberTask(APP_DASH_ACL_IN_TABLE_NAME, SET_COMMAND, &DashAclOrch::taskUpdateDashAclOut, this),
        KeyOnlyWorker::makeMemberTask(APP_DASH_ACL_IN_TABLE_NAME, DEL_COMMAND, &DashAclOrch::taskRemoveDashAclOut, this),
        PbWorker<AclGroup>::makeMemberTask(APP_DASH_ACL_GROUP_TABLE_NAME, SET_COMMAND, &DashAclOrch::taskUpdateDashAclGroup, this),
        KeyOnlyWorker::makeMemberTask(APP_DASH_ACL_GROUP_TABLE_NAME, DEL_COMMAND, &DashAclOrch::taskRemoveDashAclGroup, this),
        PbWorker<AclRule>::makeMemberTask(APP_DASH_ACL_GROUP_TABLE_NAME, SET_COMMAND, &DashAclOrch::taskUpdateDashAclRule, this),
        KeyOnlyWorker::makeMemberTask(APP_DASH_ACL_GROUP_TABLE_NAME, DEL_COMMAND, &DashAclOrch::taskRemoveDashAclRule, this),
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

    return bindAclToEni(m_dash_acl_in_table, key, data.acl_group_id());
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

    return bindAclToEni(m_dash_acl_out_table, key, data.acl_group_id());
}

task_process_status DashAclOrch::taskRemoveDashAclOut(
    const string &key)
{
    SWSS_LOG_ENTER();

    return unbindAclFromEni(m_dash_acl_out_table, key);
}

task_process_status DashAclOrch::taskUpdateDashAclGroup(
    const string &key,
    const dash::acl::AclGroup &data)
{
    SWSS_LOG_ENTER();

    if (m_dash_acl_group_table.find(key) == m_dash_acl_group_table.end())
    {
        // Update the ACL group's attributes
        SWSS_LOG_WARN("Cannot update attributes of ACL group %s", key.c_str());
        return task_failed;
    }

    sai_ip_addr_family_t ip_version = data.ip_version() == dash::types::IpVersion::IP_VERSION_IPV4 ? SAI_IP_ADDR_FAMILY_IPV4 : SAI_IP_ADDR_FAMILY_IPV6;
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
        SWSS_LOG_ERROR("Failed to create ACL group %s, rv: %s", key.c_str(), sai_serialize_status(status).c_str());
        return task_failed;
    }
    acl_group.m_rule_count = 0;
    acl_group.m_ref_count = 0;
    acl_group.m_ip_version = ip_version;
    m_dash_acl_group_table.emplace(key, acl_group);
    SWSS_LOG_NOTICE("Created ACL group %s", key.c_str());

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
        SWSS_LOG_ERROR("Failed to remove ACL group %s, rv: %s", key.c_str(), sai_serialize_status(status).c_str());
        return task_failed;
    }
    m_dash_acl_group_table.erase(key);
    SWSS_LOG_NOTICE("Removed ACL group %s", key.c_str());

    return task_success;
}

task_process_status DashAclOrch::taskUpdateDashAclRule(
    const string &key,
    const dash::acl::AclRule &data)
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

    // auto &acl_rule = m_dash_acl_rule_table[key];
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
    if (data.action() == dash::acl::Action::ACTION_PERMIT)
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

    attrs.emplace_back();
    attrs.back().id = SAI_DASH_ACL_RULE_ATTR_SIP;
    vector<sai_ip_prefix_t> src_prefixes = pbPrefixes2SAIPrefixes(data.src_addr().begin(), data.src_addr().end(), acl_group->m_ip_version);
    if (src_prefixes.empty())
    {
        return task_invalid_entry;
    }
    attrs.back().value.ipprefixlist.count = static_cast<uint32_t>(src_prefixes.size());
    attrs.back().value.ipprefixlist.list = src_prefixes.data();

    attrs.emplace_back();
    attrs.back().id = SAI_DASH_ACL_RULE_ATTR_DIP;
    vector<sai_ip_prefix_t> dst_prefixes = pbPrefixes2SAIPrefixes(data.dst_addr().begin(), data.dst_addr().end(), acl_group->m_ip_version);
    if (dst_prefixes.empty())
    {
        return task_invalid_entry;
    }
    attrs.back().value.ipprefixlist.count = static_cast<uint32_t>(dst_prefixes.size());
    attrs.back().value.ipprefixlist.list = dst_prefixes.data();

    attrs.emplace_back();
    attrs.back().id = SAI_DASH_ACL_RULE_ATTR_SRC_PORT;
    vector<sai_u16_range_t> src_ports = pbRangeOrValues2SAIRanges<sai_u16_range_t>(data.src_port().begin(), data.src_port().end());
    if (src_ports.empty())
    {
        return task_invalid_entry;
    }
    attrs.back().value.u16rangelist.count = static_cast<uint32_t>(src_ports.size());
    attrs.back().value.u16rangelist.list = src_ports.data();

    attrs.emplace_back();
    attrs.back().id = SAI_DASH_ACL_RULE_ATTR_DST_PORT;
    vector<sai_u16_range_t> dst_ports = pbRangeOrValues2SAIRanges<sai_u16_range_t>(data.dst_port().begin(), data.dst_port().end());
    if (dst_ports.empty())
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
        SWSS_LOG_ERROR("Failed to create dash ACL rule %s, rv: %s", key.c_str(), sai_serialize_status(status).c_str());
        return task_failed;
    }
    m_dash_acl_rule_table.emplace(key, acl_rule);
    acl_group->m_rule_count++;
    SWSS_LOG_NOTICE("Created ACL rule %s", key.c_str());

    return task_success;
}

task_process_status DashAclOrch::taskRemoveDashAclRule(
    const string &key)
{
    SWSS_LOG_ENTER();

    string group_id, rule_id;
    if (!extractVariables(key, ':', group_id, rule_id))
    {
        SWSS_LOG_ERROR("Failed to parse key %s", key.c_str());
        return task_failed;
    }

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
    m_dash_acl_group_table[group_id].m_rule_count--;
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
        SWSS_LOG_ERROR("Failed to bind ACL %s to eni %s attribute, status : %s", key.c_str(), acl_group_id.c_str(), sai_serialize_status(status).c_str());
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
        SWSS_LOG_ERROR("Failed to unbind ACL %s to eni %s attribute, status : %s", key.c_str(), acl_bind.m_acl_group_id.c_str(), sai_serialize_status(status).c_str());
        return task_failed;
    }

    acl_group->m_ref_count--;

    SWSS_LOG_NOTICE("Unbind ACL group %s from %s", acl_bind.m_acl_group_id.c_str(), key.c_str());

    return task_success;
}
