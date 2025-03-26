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
#include "orchagent/bulker.h"
#include "orchagent/orch.h"
#include "orchagent/zmqorch.h"
#include "zmqserver.h"

#include "orchagent/dash/dashorch.h"
#include "orchagent/dash/dashaclgroupmgr.h"
#include "orchagent/dash/dashtagmgr.h"
#include "dash_api/acl_group.pb.h"
#include "dash_api/acl_rule.pb.h"
#include "dash_api/acl_in.pb.h"
#include "dash_api/acl_out.pb.h"

struct DashAclEntry {
    std::string m_acl_group_id;
};

using DashAclTable = std::unordered_map<std::string, DashAclEntry>;

class DashAclOrch : public ZmqOrch
{
public:
    using TaskArgs = std::vector<swss::FieldValueTuple>;

    DashAclOrch(swss::DBConnector *db, const std::vector<std::string> &tables, DashOrch *dash_orch, swss::ZmqServer *zmqServer);
    DashAclGroupMgr& getDashAclGroupMgr();
    DashTagMgr& getDashAclTagMgr();

private:
    void doTask(ConsumerBase &consumer);

    task_process_status taskUpdateDashAclIn(
        const std::string &key,
        const dash::acl_in::AclIn &data);
    task_process_status taskRemoveDashAclIn(
        const std::string &key);

    task_process_status taskUpdateDashAclOut(
        const std::string &key,
        const dash::acl_out::AclOut &data);
    task_process_status taskRemoveDashAclOut(
        const std::string &key);

    task_process_status taskUpdateDashAclGroup(
        const std::string &key,
        const dash::acl_group::AclGroup &data);
    task_process_status taskRemoveDashAclGroup(
        const std::string &key);

    task_process_status taskUpdateDashAclRule(
        const std::string &key,
        const dash::acl_rule::AclRule &data);
    task_process_status taskRemoveDashAclRule(
        const std::string &key);

    task_process_status taskUpdateDashPrefixTag(
        const std::string &key,
        const dash::tag::PrefixTag &data);

    task_process_status taskRemoveDashPrefixTag(
        const std::string &key);

    task_process_status bindAclToEni(
        DashAclDirection direction, 
        const std::string table_id, 
        const std::string &acl_group_id);
    task_process_status unbindAclFromEni(
        DashAclDirection direction, 
        const std::string table_id);

    DashAclTable m_dash_acl_in_table;
    DashAclTable m_dash_acl_out_table;

    DashAclGroupMgr m_group_mgr;
    DashTagMgr m_tag_mgr;

    DashOrch *m_dash_orch;
};
