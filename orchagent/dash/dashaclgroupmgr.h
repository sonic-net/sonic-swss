#pragma once

#include <unordered_map>
#include <memory>

#include <saitypes.h>
#include <sai.h>
#include <logger.h>

#include "dashorch.h"
#include "dashtagmgr.h"
#include "table.h"

#include "dash_api/acl_group.pb.h"
#include "dash_api/acl_rule.pb.h"
#include "dash_api/acl_in.pb.h"
#include "dash_api/acl_out.pb.h"

enum class DashAclStage
{
    STAGE1,
    STAGE2,
    STAGE3,
    STAGE4,
    STAGE5,
};

enum class DashAclDirection
{
    IN,
    OUT,
};

struct DashAclRule
{
    enum class Action
    {
        ALLOW,
        DENY,
    };

    sai_uint32_t m_priority;    
    Action m_action;
    bool m_terminating;
    std::vector<std::uint8_t> m_protocols;
    std::vector<sai_ip_prefix_t> m_src_prefixes;
    std::vector<sai_ip_prefix_t> m_dst_prefixes;
    std::unordered_set<std::string> m_src_tags;
    std::unordered_set<std::string> m_dst_tags;
    std::vector<sai_u16_range_t> m_src_ports;
    std::vector<sai_u16_range_t> m_dst_ports;
};

struct DashAclRuleInfo
{
    sai_object_id_t m_dash_acl_rule_id = SAI_NULL_OBJECT_ID;

    std::unordered_set<std::string> m_src_tags;
    std::unordered_set<std::string> m_dst_tags;

    DashAclRuleInfo() = default;
    DashAclRuleInfo(const DashAclRule &rule);

    bool isTagUsed(const std::string &tag_id) const;
};

struct DashAclGroup
{
    using EniTable = std::unordered_map<std::string, std::unordered_set<DashAclStage>>;
    using RuleTable = std::unordered_map<std::string, DashAclRuleInfo>;
    using RuleKeys = std::unordered_set<std::string>;
    sai_object_id_t m_dash_acl_group_id = SAI_NULL_OBJECT_ID;

    std::string m_guid;
    sai_ip_addr_family_t m_ip_version;
    RuleTable m_dash_acl_rule_table;
    
    EniTable m_in_tables;
    EniTable m_out_tables;

    bool isIpV4() const
    {
        return m_ip_version == SAI_IP_ADDR_FAMILY_IPV4;
    }

    bool isIpV6() const
    {
        return m_ip_version == SAI_IP_ADDR_FAMILY_IPV6;
    }
};

bool from_pb(const dash::acl_rule::AclRule& data, DashAclRule& rule);
bool from_pb(const dash::acl_group::AclGroup &data, DashAclGroup& group);

class DashAclOrch;

class DashAclGroupMgr
{
    DashOrch *m_dash_orch;
    DashAclOrch *m_dash_acl_orch;
    std::unordered_map<std::string, DashAclGroup> m_groups_table;
    std::unique_ptr<swss::Table> m_dash_acl_rules_table;

public:
    DashAclGroupMgr(swss::DBConnector *db, DashOrch *dashorch, DashAclOrch *aclorch);

    task_process_status create(const std::string& group_id, DashAclGroup& group);
    task_process_status remove(const std::string& group_id);
    bool exists(const std::string& group_id) const;
    bool isBound(const std::string& group_id);

    task_process_status onUpdate(const std::string& group_id, const std::string& tag_id,const DashTag& tag);

    task_process_status createRule(const std::string& group_id, const std::string& rule_id, DashAclRule& rule);
    task_process_status updateRule(const std::string& group_id, const std::string& rule_id, DashAclRule& rule);
    task_process_status removeRule(const std::string& group_id, const std::string& rule_id);
    bool ruleExists(const std::string& group_id, const std::string& rule_id) const;

    task_process_status bind(const std::string& group_id, const std::string& eni_id, DashAclDirection direction, DashAclStage stage);
    task_process_status unbind(const std::string& group_id, const std::string& eni_id, DashAclDirection direction, DashAclStage stage);

private:
    void init(DashAclGroup& group);
    void create(DashAclGroup& group);
    void remove(DashAclGroup& group);

    DashAclRuleInfo createRule(DashAclGroup& group, DashAclRule& rule);
    void removeRule(DashAclGroup& group, DashAclRuleInfo& rule);
    bool fetchRule(const std::string &group_id, const std::string &rule_id, DashAclRule &rule);

    void bind(const DashAclGroup& group, const EniEntry& eni, DashAclDirection direction, DashAclStage stage);
    void unbind(const DashAclGroup& group, const EniEntry& eni, DashAclDirection direction, DashAclStage stage);
    bool isBound(const DashAclGroup& group);
    void attachTags(const std::string &group_id, const std::unordered_set<std::string>& tags);
    void detachTags(const std::string &group_id, const std::unordered_set<std::string>& tags);

    task_process_status refreshAclGroupFull(const std::string &group_id);
    void removeAclGroupFull(DashAclGroup& group);
};
