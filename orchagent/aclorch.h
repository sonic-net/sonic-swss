#ifndef SWSS_ACLORCH_H
#define SWSS_ACLORCH_H

#include <iostream>
#include <sstream>
#include <thread>
#include <mutex>
#include <condition_variable>
#include "orch.h"
#include "portsorch.h"

// Table attributes
#define DEFAULT_TABLE_PRIORITY 10
#define COUNTERS_READ_INTERVAL 10

#define TABLE_DESCRIPTION "POLICY_DESC"
#define TABLE_TYPE        "TYPE"
#define TABLE_PORTS       "PORTS"

#define TABLE_TYPE_L3     "L3"
#define TABLE_TYPE_MIRROR "MIRROR"

#define MAX_RULE_ATTRIBUTES 20  // table_oid + priority + enable + matches + actions
#define RULE_PRIORITY           "PRIORITY"
#define MATCH_SRC_IP            "SRC_IP"
#define MATCH_DST_IP            "DST_IP"
#define MATCH_L4_SRC_PORT       "L4_SRC_PORT"
#define MATCH_L4_DST_PORT       "L4_DST_PORT"
#define MATCH_ETHER_TYPE        "ETHER_TYPE"
#define MATCH_IP_PROTOCOL       "IP_PROTOCOL"
#define MATCH_TCP_FLAGS         "TCP_FLAGS"
#define MATCH_IP_TYPE           "IP_TYPE"
#define MATCH_DSCP              "DSCP"
#define MATCH_L4_SRC_PORT_RANGE "L4_SRC_PORT_RANGE"
#define MATCH_L4_DST_PORT_RANGE "L4_DST_PORT_RANGE"

#define ACTION_PACKET_ACTION    "PACKET_ACTION"
#define ACTION_MIRROR           "MIRROR"

#define PACKET_ACTION_FORWARD   "FORWARD"
#define PACKET_ACTION_DROP      "DROP"

#define IP_TYPE_ANY             "ANY"
#define IP_TYPE_IP              "IP"
#define IP_TYPE_NON_IP          "NON_IP"
#define IP_TYPE_IPv4ANY         "IPV4ANY"
#define IP_TYPE_NON_IPv4        "NON_IPv4"
#define IP_TYPE_IPv6ANY         "IPV6ANY"
#define IP_TYPE_NON_IPv6        "NON_IPv6"
#define IP_TYPE_ARP             "ARP"
#define IP_TYPE_ARP_REQUEST     "ARP_REQUEST"
#define IP_TYPE_ARP_REPLY       "ARP_REPLY"

typedef enum
{
    ACL_TABLE_UNKNOWN,
    ACL_TABLE_L3,
    ACL_TABLE_MIRROR
} acl_table_type_t;

typedef map<string, acl_table_type_t> acl_table_type_lookup_t;
typedef map<string, sai_acl_entry_attr_t> acl_rule_attr_lookup_t;
typedef map<string, sai_acl_ip_type_t> acl_ip_type_lookup_t;
typedef vector<sai_object_id_t> ports_list_t;

struct AclRule {
    string id;
    string table_id;
    sai_object_id_t counter_oid;
    uint32_t priority;
    map <sai_acl_entry_attr_t, sai_attribute_value_t> matches;
    map <sai_acl_entry_attr_t, sai_attribute_value_t> actions;
};

struct AclTable {
    string id;
    string description;
    acl_table_type_t type;
    ports_list_t ports;
    map <sai_object_id_t, AclRule> rules;
    AclTable(): type(ACL_TABLE_UNKNOWN) {}
};

template <class Iterable>
inline void split(string str, Iterable& out, char delim = ' ')
{
    string val;

    istringstream input(str);

    while (getline(input, val, delim))
    {
        out.push_back(val);
    }
}

class AclOrch : public Orch, public std::thread
{
public:
    AclOrch(DBConnector *db, vector<string> tableNames, PortsOrch *portOrch);
    ~AclOrch();

private:
    void doTask(Consumer &consumer);
    void doAclTableTask(Consumer &consumer);
    void doAclRuleTask(Consumer &consumer);

    static void collectCountersThread(AclOrch *pAclOrch);

    sai_status_t createBindAclTable(AclTable &aclTable, sai_object_id_t &table_oid);
    sai_status_t createAclRule(AclRule &aclRule, sai_object_id_t &rule_oid);
    sai_status_t createAclCounter(sai_object_id_t &counter_oid, sai_object_id_t table_oid);
    sai_status_t bindAclTable(sai_object_id_t table_oid, AclTable &aclTable, bool bind = true);
    sai_status_t deleteUnbindAclTable(sai_object_id_t table_oid);
    sai_status_t deleteAclRule(AclRule &aclRule);
    sai_status_t deleteAclCounter(sai_object_id_t counter_oid);
    sai_status_t deleteAllAclObjects();

    bool processAclTableType(string type, acl_table_type_t &table_type);
    bool processPorts(string portsList, ports_list_t& out);
    bool processIpType(string type, sai_uint32_t &ip_type);
    bool validateAclTable(AclTable &aclTable);
    bool validateAclRule(AclRule &aclRule);
    bool validateAddPriority(AclRule &aclRule, string attr_name, string attr_value);
    bool validateAddMatch(AclRule &aclRule, string attr_name, string attr_value);
    bool validateAddAction(AclRule &aclRule, string attr_name, string attr_value);

    sai_object_id_t getTableById(string table_id);
    sai_object_id_t getRuleById(string table_id, string rule_id);
    string toUpper(string str);

    //vector <AclTable> m_AclTables;
    map <sai_object_id_t, AclTable> m_AclTables;
    // Port OID to vector of ACL OIDs
    map <sai_object_id_t, vector<sai_object_id_t>> m_portBind;

    static std::mutex m_countersMutex;
    static std::condition_variable m_sleepGuard;
    static bool m_bCollectCounters;
    PortsOrch *m_portOrch;

};

#endif /* SWSS_ACLORCH_H */
