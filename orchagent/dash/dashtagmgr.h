#pragma once

#include <unordered_map>
#include <vector>

#include <saitypes.h>
#include <sai.h>
#include <logger.h>

#include "orchagent/dash/dashorch.h"
#include "orchagent/dash/pbutils.h"

#include "dash_api/prefix_tag.pb.h"

struct DashTag {
    sai_ip_addr_family_t m_ip_version;
    std::vector<sai_ip_prefix_t> m_prefixes;
    std::unordered_map<std::string, uint16_t> m_group_refcnt;
};

bool from_pb(const dash::tag::PrefixTag& data, DashTag& tag);

class DashAclOrch;

class DashTagMgr
{
public:

    DashTagMgr(DashAclOrch *aclorch);

    task_process_status create(const std::string& tag_id, const DashTag& tag);
    task_process_status update(const std::string& tag_id, const DashTag& tag);
    task_process_status remove(const std::string& tag_id);
    bool exists(const std::string& tag_id) const;

    const std::vector<sai_ip_prefix_t>& getPrefixes(const std::string& tag_id) const;

    task_process_status attach(const std::string& tag_id, const std::string& group_id);
    task_process_status detach(const std::string& tag_id, const std::string& group_id);

private:
    DashAclOrch *m_dash_acl_orch;
    std::unordered_map<std::string, DashTag> m_tag_table;
};
