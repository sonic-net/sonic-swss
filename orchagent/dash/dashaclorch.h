#pragma once

#include <boost/optional.hpp>
#include <unordered_map>
#include <vector>
#include <string>
#include <deque>
#include <functional>

#include <saitypes.h>
#include <sai.h>
#include <logger.h>
#include <dbconnector.h>
#include <bulker.h>
#include <orch.h>

#include "dashorch.h"

typedef enum _DashAclStage
{
    STAGE1,
    STAGE2,
    STAGE3,
    STAGE4,
    STAGE5,
} DashAclStage;

typedef enum _DashAclDirection
{
    IN,
    OUT,
} DashAclDirection;

struct DashAclEntry {
    boost::optional<std::string> m_acl_group_id;
};

struct DashAclGroupEntry {
    sai_object_id_t m_dash_acl_group_id;
    size_t m_ref_count;
    size_t m_rule_count;
    boost::optional<std::string> m_guid;
    boost::optional<sai_ip_addr_family_t> m_ip_version;
};

struct DashAclRuleEntry {
    sai_object_id_t m_dash_acl_rule_id;
    boost::optional<sai_uint32_t> m_priority;
    typedef enum
    {
        ALLOW,
        DENY,
    } Action;
    boost::optional<DashAclRuleEntry::Action> m_action;
    boost::optional<bool> m_terminating;
    boost::optional<std::vector<std::uint8_t> > m_protocols;
    boost::optional<std::vector<sai_ip_prefix_t> > m_src_prefixes;
    boost::optional<std::vector<sai_ip_prefix_t> > m_dst_prefixes;
    boost::optional<std::vector<sai_u16_range_t> > m_src_ports;
    boost::optional<std::vector<sai_u16_range_t> > m_dst_ports;
};

using DashAclTable = std::unordered_map<std::string, DashAclEntry>;
using DashAclGroupTable = std::unordered_map<std::string, DashAclGroupEntry>;
using DashAclRuleTable = std::unordered_map<std::string, DashAclRuleEntry>;

class DashAclOrch : public Orch
{
public:
    using TaskArgs = std::vector<swss::FieldValueTuple>;

    DashAclOrch(swss::DBConnector *db, const std::vector<std::string> &tables, DashOrch *dash_orch);

private:
    void doTask(Consumer &consumer);

    task_process_status taskUpdateDashAclIn(
        const std::string &key,
        const TaskArgs &data);
    task_process_status taskRemoveDashAclIn(
        const std::string &key,
        const TaskArgs &data);

    task_process_status taskUpdateDashAclOut(
        const std::string &key,
        const TaskArgs &data);
    task_process_status taskRemoveDashAclOut(
        const std::string &key,
        const TaskArgs &data);

    task_process_status taskUpdateDashAclGroup(
        const std::string &key,
        const TaskArgs &data);
    task_process_status taskRemoveDashAclGroup(
        const std::string &key,
        const TaskArgs &data);

    task_process_status taskUpdateDashAclRule(
        const std::string &key,
        const TaskArgs &data);

    task_process_status taskRemoveDashAclRule(
        const std::string &key,
        const TaskArgs &data);

    DashAclGroupEntry* getAclGroup(const std::string &group_id);

    task_process_status bindAclToEni(
        DashAclTable &acl_table,
        const std::string &key,
        const TaskArgs &data);
    task_process_status unbindAclFromEni(
        DashAclTable &acl_table,
        const std::string &key,
        const TaskArgs &data);

    DashAclTable m_dash_acl_in_table;
    DashAclTable m_dash_acl_out_table;
    DashAclGroupTable m_dash_acl_group_table;
    DashAclRuleTable m_dash_acl_rule_table;

    DashOrch *m_dash_orch;
};
