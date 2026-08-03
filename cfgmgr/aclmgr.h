#ifndef __ACLMGR__
#define __ACLMGR__

#include "dbconnector.h"
#include "producerstatetable.h"
#include "orch.h"

#include <set>
#include <map>
#include <string>
#include <vector>

/* CONFIG_DB ACL table name constants.
 * These are normally generated from YANG models during the full build,
 * but may not be available during isolated component builds.
 * Using #ifndef so the build-generated versions take precedence. */
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
#define ACL_RULE_FIELD_L4_SRC_PORT      "L4_SRC_PORT"
#define ACL_RULE_FIELD_L4_DST_PORT      "L4_DST_PORT"
#define ACL_RULE_FIELD_IP_PROTOCOL      "IP_PROTOCOL"
#define ACL_RULE_FIELD_TCP_FLAGS        "TCP_FLAGS"
#define ACL_RULE_FIELD_DSCP             "DSCP"
#define ACL_RULE_FIELD_PACKET_ACTION    "PACKET_ACTION"
#define ACL_RULE_FIELD_PRIORITY         "PRIORITY"
#define ACL_RULE_FIELD_ETHER_TYPE       "ETHER_TYPE"

/* ACL types */
#define ACL_TYPE_L3                     "L3"
#define ACL_TYPE_L3V6                   "L3V6"

namespace swss {

class AclMgr : public Orch
{
public:
    AclMgr(DBConnector *cfgDb, DBConnector *appDb, DBConnector *stateDb,
           const std::vector<std::string> &tableNames);
    using Orch::doTask;

private:
    /* CONFIG_DB tables for reading init data / checking state */
    Table m_cfgAclTable;
    Table m_cfgAclRuleTable;

    /* STATE_DB tables for writing state back */
    Table m_stateAclTable;
    Table m_stateAclRuleTable;

    /* Internal tracking */
    std::set<std::string> m_programmedTables;       // table_id of successfully applied ACL tables
    std::map<std::string, uint32_t> m_priorities;   // rule_key -> prio for deletion

    /* Handler methods */
    void doTask(Consumer &consumer);
    void doAclTableTask(Consumer &consumer);
    void doAclRuleTask(Consumer &consumer);

    /* Kernel programming helpers */
    bool addTcFlowerFilter(const std::string &iface, uint32_t prio,
                           const std::string &srcIp, const std::string &dstIp,
                           const std::string &l4SrcPort, const std::string &l4DstPort,
                           const std::string &ipProto, const std::string &tcpFlags,
                           const std::string &dscp, const std::string &action);
    bool removeTcFlowerFilter(const std::string &iface, uint32_t prio);
};

} // namespace swss

#endif /* __ACLMGR__ */
