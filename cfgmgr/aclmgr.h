#ifndef __ACLMGR__
#define __ACLMGR__

#include "dbconnector.h"
#include "producerstatetable.h"
#include "orch.h"

#include <set>
#include <map>
#include <string>
#include <vector>

/* CONFIG_DB ACL table name constants. */
#ifndef CFG_ACL_TABLE_TABLE_NAME
#define CFG_ACL_TABLE_TABLE_NAME        "ACL_TABLE"
#endif
#ifndef CFG_ACL_RULE_TABLE_NAME
#define CFG_ACL_RULE_TABLE_NAME         "ACL_RULE"
#endif
#ifndef CFG_ACL_TABLE_TYPE_TABLE_NAME
#define CFG_ACL_TABLE_TYPE_TABLE_NAME   "ACL_TABLE_TYPE"
#endif

/* ACL table/rule field names from CONFIG_DB */
#define ACL_TABLE_FIELD_TYPE            "type"
#define ACL_TABLE_FIELD_PORTS           "ports"
#define ACL_TABLE_FIELD_POLICY_DESC     "policy_desc"
#define ACL_TABLE_FIELD_STAGE           "stage"

#define ACL_RULE_FIELD_SRC_IP           "SRC_IP"
#define ACL_RULE_FIELD_DST_IP           "DST_IP"
#define ACL_RULE_FIELD_SRC_IP_MASK      "SRC_IP_MASK"
#define ACL_RULE_FIELD_DST_IP_MASK      "DST_IP_MASK"
#define ACL_RULE_FIELD_SRC_IPV6         "SRC_IPV6"
#define ACL_RULE_FIELD_DST_IPV6         "DST_IPV6"
#define ACL_RULE_FIELD_L4_SRC_PORT      "L4_SRC_PORT"
#define ACL_RULE_FIELD_L4_DST_PORT      "L4_DST_PORT"
#define ACL_RULE_FIELD_L4_SRC_PORT_RANGE "L4_SRC_PORT_RANGE"
#define ACL_RULE_FIELD_L4_DST_PORT_RANGE "L4_DST_PORT_RANGE"
#define ACL_RULE_FIELD_IP_PROTOCOL      "IP_PROTOCOL"
#define ACL_RULE_FIELD_NEXT_HEADER      "NEXT_HEADER"
#define ACL_RULE_FIELD_TCP_FLAGS        "TCP_FLAGS"
#define ACL_RULE_FIELD_DSCP             "DSCP"
#define ACL_RULE_FIELD_ETHER_TYPE       "ETHER_TYPE"
#define ACL_RULE_FIELD_VLAN_ID          "VLAN_ID"
#define ACL_RULE_FIELD_IP_TYPE          "IP_TYPE"
#define ACL_RULE_FIELD_ICMP_TYPE        "ICMP_TYPE"
#define ACL_RULE_FIELD_ICMP_CODE        "ICMP_CODE"
#define ACL_RULE_FIELD_ICMPV6_TYPE      "ICMPV6_TYPE"
#define ACL_RULE_FIELD_ICMPV6_CODE      "ICMPV6_CODE"
#define ACL_RULE_FIELD_PACKET_ACTION    "PACKET_ACTION"
#define ACL_RULE_FIELD_PRIORITY         "PRIORITY"

#define ACL_RULE_FIELD_REDIRECT_ACTION  "REDIRECT_ACTION"
#define ACL_RULE_FIELD_MIRROR_ACTION    "MIRROR_ACTION"
#define ACL_RULE_FIELD_MIRROR_INGRESS_ACTION "MIRROR_INGRESS_ACTION"
#define ACL_RULE_FIELD_MIRROR_EGRESS_ACTION  "MIRROR_EGRESS_ACTION"
#define ACL_RULE_FIELD_POLICER_ACTION   "POLICER_ACTION"
#define ACL_RULE_FIELD_DSCP_ACTION      "DSCP_ACTION"

/* Packet actions */
#define PACKET_ACTION_FORWARD   "FORWARD"
#define PACKET_ACTION_DROP      "DROP"
#define PACKET_ACTION_REDIRECT  "REDIRECT"

/* ACL table types */
#define ACL_TYPE_L3             "L3"
#define ACL_TYPE_L3V6           "L3V6"
#define ACL_TYPE_L3V4V6         "L3V4V6"
#define ACL_TYPE_MIRROR         "MIRROR"
#define ACL_TYPE_MIRRORV6       "MIRRORV6"
#define ACL_TYPE_MIRROR_DSCP    "MIRROR_DSCP"
#define ACL_TYPE_CTRLPLANE      "CTRLPLANE"
#define ACL_TYPE_DROP           "DROP"
#define ACL_TYPE_EGR_SET_DSCP   "EGR_SET_DSCP"

namespace swss {

/*
 * AclRuleFields — the parsed CONFIG_DB fields of a single ACL_RULE, split into
 * match fields and action fields. Passed to addTcFlowerFilter() so new fields
 * don't require adding positional arguments.
 */
struct AclRuleFields
{
    /* match fields */
    std::string src_ip, dst_ip;
    std::string src_ip_mask, dst_ip_mask;
    std::string src_ipv6, dst_ipv6;
    std::string l4_src_port, l4_dst_port;
    std::string l4_src_port_range, l4_dst_port_range;
    std::string ip_proto, next_header;
    std::string tcp_flags;
    std::string dscp;
    std::string ether_type;
    std::string vlan_id;
    std::string ip_type;
    std::string icmp_type, icmp_code;
    std::string icmpv6_type, icmpv6_code;

    /* action fields */
    std::string packet_action;
    std::string redirect_action;
    std::string mirror_action;
    std::string mirror_ingress_action;
    std::string mirror_egress_action;
    std::string policer_action;
    std::string dscp_action;

    uint32_t priority = 100;
    std::string stage = "ingress";
};

/*
 * AclRuleState — how a rule was programmed, so its tc filter can be torn down
 * exactly (correct protocol + hook + priority + resolved interfaces).
 */
struct AclRuleState
{
    uint32_t priority = 100;
    std::string protocol = "ip";
    std::string hook = "ingress";
    std::vector<std::string> interfaces;
};

class AclMgr : public Orch
{
public:
    AclMgr(DBConnector *cfgDb, DBConnector *appDb, DBConnector *stateDb,
           const std::vector<std::string> &tableNames);
    using Orch::doTask;

private:
    DBConnector *m_cfgDb;
    DBConnector *m_stateDb;

    /* CONFIG_DB tables for reading init data / checking state */
    Table m_cfgAclTable;
    Table m_cfgAclRuleTable;

    /* STATE_DB tables for writing state back */
    Table m_stateAclTable;
    Table m_stateAclRuleTable;

    /* Internal tracking */
    std::set<std::string> m_programmedTables;      // table_id of accepted ACL tables
    std::map<std::string, AclRuleState> m_ruleState; // rule_key -> programmed state

    /* Handler methods */
    void doTask(Consumer &consumer);
    void doAclTableTask(Consumer &consumer);
    void doAclRuleTask(Consumer &consumer);

    /* Resolve a referenced policer into a tc "action police ..." string. */
    bool getPolicerPoliceAction(const std::string &policerName, std::string &policeAction);

    /* Kernel programming helpers */
    std::string buildProtocol(const AclRuleFields &fields) const;
    bool addTcFlowerFilter(const std::string &iface, const AclRuleFields &fields);
    bool removeTcFlowerFilter(const std::string &iface, const AclRuleState &state);
};

} // namespace swss

#endif /* __ACLMGR__ */
