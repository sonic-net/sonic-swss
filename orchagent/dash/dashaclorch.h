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
#include "proto/acl.pb.h"

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

struct DashAclBindEntry {
    std::string m_acl_group_id;
};

struct DashAclGroupEntry {
    sai_object_id_t m_dash_acl_group_id;
    size_t m_ref_count;
    size_t m_rule_count;
    sai_ip_addr_family_t m_ip_version;
};

struct DashAclRuleEntry {
    sai_object_id_t m_dash_acl_rule_id;
};

using DashAclBindTable = std::unordered_map<std::string, DashAclBindEntry>;
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
        const dash::acl::AclIn &data);
    task_process_status taskRemoveDashAclIn(
        const std::string &key);

    task_process_status taskUpdateDashAclOut(
        const std::string &key,
        const dash::acl::AclOut &data);
    task_process_status taskRemoveDashAclOut(
        const std::string &key);

    task_process_status taskUpdateDashAclGroup(
        const std::string &key,
        const dash::acl::AclGroup &data);
    task_process_status taskRemoveDashAclGroup(
        const std::string &key);

    task_process_status taskUpdateDashAclRule(
        const std::string &key,
        const dash::acl::AclRule &data);
    task_process_status taskRemoveDashAclRule(
        const std::string &key);

    DashAclGroupEntry* getAclGroup(const std::string &group_id);

    task_process_status bindAclToEni(
        DashAclBindTable &acl_table,
        const std::string &key,
        const std::string &acl_group_id);
    task_process_status unbindAclFromEni(
        DashAclBindTable &acl_table,
        const std::string &key);

    DashAclBindTable m_dash_acl_in_table;
    DashAclBindTable m_dash_acl_out_table;
    DashAclGroupTable m_dash_acl_group_table;
    DashAclRuleTable m_dash_acl_rule_table;

    DashOrch *m_dash_orch;
};
