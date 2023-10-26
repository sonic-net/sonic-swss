#include <swss/logger.h>
#include <swss/stringutility.h>
#include <swss/redisutility.h>
#include <swss/ipaddress.h>

#include <swssnet.h>

#include "dashaclorch.h"
#include "taskworker.h"
#include "pbutils.h"
#include "crmorch.h"
#include "dashaclgroupmgr.h"
#include "dashtagmgr.h"
#include "saihelper.h"

using namespace std;
using namespace swss;
using namespace dash::acl_in;
using namespace dash::acl_out;
using namespace dash::acl_rule;
using namespace dash::acl_group;
using namespace dash::tag;
using namespace dash::types;

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

}

DashAclOrch::DashAclOrch(DBConnector *db, const vector<string> &tables, DashOrch *dash_orch, ZmqServer *zmqServer) :
    ZmqOrch(db, tables, zmqServer),
    m_dash_orch(dash_orch),
    m_group_mgr(dash_orch, this),
    m_tag_mgr(this)

{
    SWSS_LOG_ENTER();
}

DashAclGroupMgr& DashAclOrch::getDashAclGroupMgr()
{
    SWSS_LOG_ENTER();
    return m_group_mgr;
}

DashTagMgr& DashAclOrch::getDashAclTagMgr()
{
    SWSS_LOG_ENTER();
    return m_tag_mgr;
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
        PbWorker<PrefixTag>::makeMemberTask(APP_DASH_PREFIX_TAG_TABLE_NAME, SET_COMMAND, &DashAclOrch::taskUpdateDashPrefixTag, this),
        KeyOnlyWorker::makeMemberTask(APP_DASH_PREFIX_TAG_TABLE_NAME, DEL_COMMAND, &DashAclOrch::taskRemoveDashPrefixTag, this),
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

    for (const auto& gid: { data.v4_acl_group_id(), data.v6_acl_group_id() })
    {
        if (gid.empty())
        {
            continue;
        }
        auto status = bindAclToEni(DashAclDirection::IN, key, gid);
        if (status != task_success)
        {
            return status;
        }
    }

    return task_success;
}

task_process_status DashAclOrch::taskRemoveDashAclIn(
    const string &key)
{
    SWSS_LOG_ENTER();

    return unbindAclFromEni(DashAclDirection::IN, key);
}

task_process_status DashAclOrch::taskUpdateDashAclOut(
    const string &key,
    const AclOut &data)
{
    SWSS_LOG_ENTER();

    for (const auto& gid: { data.v4_acl_group_id(), data.v6_acl_group_id() })
    {
        if (gid.empty())
        {
            continue;
        }
        auto status = bindAclToEni(DashAclDirection::OUT, key, gid);
        if (status != task_success)
        {
            return status;
        }
    }

    return task_success;
}

task_process_status DashAclOrch::taskRemoveDashAclOut(
    const string &key)
{
    SWSS_LOG_ENTER();

    return unbindAclFromEni(DashAclDirection::OUT, key);
}

task_process_status DashAclOrch::taskUpdateDashAclGroup(
    const string &key,
    const AclGroup &data)
{
    SWSS_LOG_ENTER();

    if (m_group_mgr.exists(key))
    {
        SWSS_LOG_WARN("Cannot update attributes of ACL group %s", key.c_str());
        return task_failed;
    }

    DashAclGroup group = {};
    if (!from_pb(data, group))
    {
        return task_failed;
    }

    return m_group_mgr.create(key, group);
}

task_process_status DashAclOrch::taskRemoveDashAclGroup(
    const string &key)
{
    SWSS_LOG_ENTER();

    return m_group_mgr.remove(key);
}

task_process_status DashAclOrch::taskUpdateDashAclRule(
    const string &key,
    const AclRule &data)
{
    SWSS_LOG_ENTER();

    string group_id, rule_id;
    if (!extractVariables(key, ':', group_id, rule_id))
    {
        SWSS_LOG_ERROR("Failed to parse key %s", key.c_str());
        return task_failed;
    }

    DashAclRule rule = {};

    if (!from_pb(data, rule))
    {
        return task_failed;
    }

    if (m_group_mgr.ruleExists(group_id, rule_id))
    {
        return m_group_mgr.updateRule(group_id, rule_id, rule);
    }
    else
    {
        return m_group_mgr.createRule(group_id, rule_id, rule);
    }
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

    return m_group_mgr.removeRule(group_id, rule_id);
}

task_process_status DashAclOrch::taskUpdateDashPrefixTag(
    const std::string &tag_id,
    const PrefixTag &data)
{
    SWSS_LOG_ENTER();

    DashTag tag = {};

    if (!from_pb(data, tag))
    {
        return task_failed;
    }

    if (m_tag_mgr.exists(tag_id))
    {
        return m_tag_mgr.update(tag_id, tag);
    }
    else
    {
        return m_tag_mgr.create(tag_id, tag);
    }
}

task_process_status DashAclOrch::taskRemoveDashPrefixTag(
    const std::string &key)
{
    SWSS_LOG_ENTER();

    return m_tag_mgr.remove(key);
}

task_process_status DashAclOrch::bindAclToEni(DashAclDirection direction, const std::string table_id, const std::string &acl_group_id)
{
    SWSS_LOG_ENTER();

    string eni;
    DashAclStage stage;

    if (!extractVariables(table_id, ':', eni, stage))
    {
        SWSS_LOG_ERROR("Invalid key : %s", table_id.c_str());
        return task_failed;
    }

    DashAclEntry table = { .m_acl_group_id = acl_group_id };

    auto rv = m_group_mgr.bind(table.m_acl_group_id, eni, direction, stage);
    if (rv != task_success)
    {
        return rv;
    }

    DashAclTable& tables = (direction == DashAclDirection::IN) ? m_dash_acl_in_table : m_dash_acl_out_table;
    tables[table_id] = table;

    return rv;
}

task_process_status DashAclOrch::unbindAclFromEni(DashAclDirection direction, const std::string table_id)
{
    SWSS_LOG_ENTER();

    string eni;
    DashAclStage stage;
    if (!extractVariables(table_id, ':', eni, stage))
    {
        SWSS_LOG_ERROR("Invalid key : %s", table_id.c_str());
        return task_failed;
    }

    DashAclTable& acl_table = (direction == DashAclDirection::IN) ? m_dash_acl_in_table : m_dash_acl_out_table;

    auto itr = acl_table.find(table_id);
    if (itr == acl_table.end())
    {
        SWSS_LOG_WARN("ACL %s doesn't exist", table_id.c_str());
        return task_success;
    }
    auto acl = itr->second;

    auto rv = m_group_mgr.unbind(acl.m_acl_group_id, eni, direction, stage);
    if (rv != task_success)
    {
        return rv;
    }

    acl_table.erase(itr);

    return rv;
}
