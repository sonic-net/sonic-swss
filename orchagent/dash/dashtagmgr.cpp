#include "dashtagmgr.h"

#include "dashaclorch.h"
#include "saihelper.h"

using namespace std;
using namespace swss;

bool from_pb(const dash::tag::PrefixTag& data, DashTag& tag)
{
    if (!to_sai(data.ip_version(), tag.m_ip_version))
    {
        return false;
    }

    if(!to_sai(data.prefix_list(), tag.m_prefixes))
    {
        return false;
    }

    return true;
}

DashTagMgr::DashTagMgr(DashAclOrch *aclorch) :
    m_dash_acl_orch(aclorch)
{
    SWSS_LOG_ENTER();
}

task_process_status DashTagMgr::create(const string& tag_id, const DashTag& tag)
{
    SWSS_LOG_ENTER();

    if (exists(tag_id))
    {
        return task_failed;
    }

    m_tag_table.emplace(tag_id, tag);

    SWSS_LOG_INFO("Created prefix tag %s", tag_id.c_str());
    
    return task_success;
}

task_process_status DashTagMgr::update(const string& tag_id, const DashTag& new_tag)
{
    SWSS_LOG_ENTER();

    SWSS_LOG_INFO("Updating existing prefix tag %s", tag_id.c_str());

    auto tag_it = m_tag_table.find(tag_id);
    if (tag_it == m_tag_table.end())
    {
        SWSS_LOG_ERROR("Prefix tag %s does not exist ", tag_id.c_str());
        return task_failed;
    }

    auto& tag = tag_it->second;

    if (tag.m_ip_version != new_tag.m_ip_version)
    {
        SWSS_LOG_WARN("'ip_version' changing is not supported for tag %s", tag_id.c_str());
        return task_failed;
    }

    // Update tag prefixes
    tag.m_prefixes = new_tag.m_prefixes;

    for (auto& group_it: tag.m_group_refcnt)
    {
        const auto& group_id = group_it.first;
        m_dash_acl_orch->getDashAclGroupMgr().onUpdate(group_id, tag_id, tag);
    }

    return task_success;
}

task_process_status DashTagMgr::remove(const string& tag_id)
{
    SWSS_LOG_ENTER();

    auto tag_it = m_tag_table.find(tag_id);
    if (tag_it == m_tag_table.end())
    {
        SWSS_LOG_WARN("Prefix tag %s does not exist ", tag_id.c_str());
        return task_success;
    }

    if (!tag_it->second.m_group_refcnt.empty())
    {
        SWSS_LOG_WARN("Prefix tag %s is still in use by ACL rule(s)", tag_id.c_str());
        return task_need_retry;
    }

    m_tag_table.erase(tag_it);

    return task_success;
}

bool DashTagMgr::exists(const string& tag_id) const
{
    SWSS_LOG_ENTER();

    return m_tag_table.find(tag_id) != m_tag_table.end();
}

const vector<sai_ip_prefix_t>& DashTagMgr::getPrefixes(const string& tag_id) const
{
    SWSS_LOG_ENTER();

    auto tag_it = m_tag_table.find(tag_id);
    ABORT_IF_NOT(tag_it != m_tag_table.end(), "Tag %s does not exist", tag_id.c_str());

    return tag_it->second.m_prefixes;
}

task_process_status DashTagMgr::attach(const string& tag_id, const string& group_id)
{
    SWSS_LOG_ENTER();

    auto tag_it = m_tag_table.find(tag_id);
    ABORT_IF_NOT(tag_it != m_tag_table.end(), "Tag %s does not exist", tag_id.c_str());
    auto& tag = tag_it->second;

    ++tag.m_group_refcnt[group_id];

    SWSS_LOG_NOTICE("Tag %s is used by ACL group %s refcnt: %u", tag_id.c_str(), group_id.c_str(), tag.m_group_refcnt[group_id]);
    return task_success;
}

task_process_status DashTagMgr::detach(const string& tag_id, const string& group_id)
{
    SWSS_LOG_ENTER();

    auto tag_it = m_tag_table.find(tag_id);
    ABORT_IF_NOT(tag_it != m_tag_table.end(), "Tag %s does not exist", tag_id.c_str());
    auto& tag = tag_it->second;
    auto group_it = tag.m_group_refcnt.find(group_id);
    ABORT_IF_NOT(group_it != tag.m_group_refcnt.end(), "Group %s is not attached to the tag %s", group_id.c_str(), tag_id.c_str());

    --group_it->second;
    if (!group_it->second)
    {
        tag.m_group_refcnt.erase(group_it);
        SWSS_LOG_NOTICE("Tag %s is no longer used by ACL group %s", tag_id.c_str(), group_id.c_str());
    }
    
    return task_success;
}
