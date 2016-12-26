#ifndef SWSS_ACLORCH_H
#define SWSS_ACLORCH_H

#include <iostream>
#include <sstream>
#include <thread>
#include <mutex>
#include <tuple>
#include <condition_variable>
#include "orch.h"
#include "portsorch.h"
#include "mirrororch.h"
#include "observer.h"

#define DEFAULT_TABLE_PRIORITY 10
// ACL counters update interval in the DB
// Value is in seconds. Should not be less than 5 seconds
// (in worst case update of 1265 counters takes almost 5 sec)
#define COUNTERS_READ_INTERVAL 10

#define TABLE_DESCRIPTION "POLICY_DESC"
#define TABLE_TYPE        "TYPE"
#define TABLE_PORTS       "PORTS"

#define TABLE_TYPE_L3     "L3"
#define TABLE_TYPE_MIRROR "MIRROR"

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
#define ACTION_MIRROR_ACTION    "MIRROR_ACTION"

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
typedef tuple<sai_acl_range_type_t, int, int> acl_range_properties_t;

class AclOrch;

class AclRange
{
public:
    static AclRange *create(sai_acl_range_type_t type, int min, int max);
    static bool remove(sai_acl_range_type_t type, int min, int max);
    static bool remove(sai_object_id_t *oids, int oidsCnt);
    sai_object_id_t getOid()
    {
        return m_oid;
    }

private:
    AclRange(sai_acl_range_type_t type, sai_object_id_t oid, int min, int max);
    bool remove();
    sai_object_id_t m_oid;
    int m_refCnt;
    int m_min;
    int m_max;
    sai_acl_range_type_t m_type;
    static map<acl_range_properties_t, AclRange*> m_ranges;
};

class AclRule
{
public:
    AclRule(AclOrch *aclOrch, string rule, string table);
    virtual bool validateAddPriority(string attr_name, string attr_value);
    virtual bool validateAddMatch(string attr_name, string attr_value);
    virtual bool validateAddAction(string attr_name, string attr_value) = 0;
    virtual bool validate() = 0;
    bool processIpType(string type, sai_uint32_t &ip_type);

    virtual bool create();
    virtual bool remove();
    virtual void update(SubjectType, void *) = 0;

    string getId()
    {
        return id;
    }

    string getTableId()
    {
        return table_id;
    }

    sai_object_id_t getCounterOid()
    {
        return counter_oid;
    }

    static shared_ptr<AclRule> makeShared(acl_table_type_t type, AclOrch *acl, MirrorOrch *mirror, string rule, string table);
    virtual ~AclRule() {}

protected:
    virtual bool createCounter();
    virtual bool removeCounter();
    virtual bool removeRanges();

    AclOrch *aclOrch;
    string id;
    string table_id;
    sai_object_id_t table_oid;
    sai_object_id_t rule_oid;
    sai_object_id_t counter_oid;
    uint32_t priority;
    map <sai_acl_entry_attr_t, sai_attribute_value_t> matches;
    map <sai_acl_entry_attr_t, sai_attribute_value_t> actions;
};

class AclRuleL3: public AclRule
{
public:
    AclRuleL3(AclOrch *aclOrch, string rule, string table);

    bool validateAddAction(string attr_name, string attr_value);
    bool validate();
    void update(SubjectType, void *);
};

class AclRuleMirror: public AclRule
{
public:
    AclRuleMirror(AclOrch *aclOrch, MirrorOrch *mirrorOrch, string rule, string table);
    bool validateAddAction(string attr_name, string attr_value);
    bool validate();
    bool create();
    bool remove();
    void update(SubjectType, void *);

protected:
    bool state;
    string sessionName;
    MirrorOrch *mirrorOrch;
};

struct AclTable {
    string id;
    string description;
    acl_table_type_t type;
    ports_list_t ports;
    // Map rule name to rule data
    map<string, shared_ptr<AclRule>> rules;
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

class AclOrch : public Orch, public Observer, public std::thread
{
public:
    AclOrch(DBConnector *db, vector<string> tableNames, PortsOrch *portOrch, MirrorOrch *mirrorOrch);
    ~AclOrch();
    void update(SubjectType, void *);

    sai_object_id_t getTableById(string table_id);

    static std::mutex& getContextMutex()
    {
        return m_countersMutex;
    }

    static swss::Table& getCountersTable()
    {
        return m_countersTable;
    }

private:
    void doTask(Consumer &consumer);
    void doAclTableTask(Consumer &consumer);
    void doAclRuleTask(Consumer &consumer);

    static void collectCountersThread(AclOrch *pAclOrch);

    sai_status_t createBindAclTable(AclTable &aclTable, sai_object_id_t &table_oid);
    sai_status_t bindAclTable(sai_object_id_t table_oid, AclTable &aclTable, bool bind = true);
    sai_status_t deleteUnbindAclTable(sai_object_id_t table_oid);

    bool processAclTableType(string type, acl_table_type_t &table_type);
    bool processPorts(string portsList, ports_list_t& out);
    bool validateAclTable(AclTable &aclTable);

    //vector <AclTable> m_AclTables;
    map <sai_object_id_t, AclTable> m_AclTables;
    // Port OID to vector of ACL OIDs
    map <sai_object_id_t, vector<sai_object_id_t>> m_portBind;

    static std::mutex m_countersMutex;
    static std::condition_variable m_sleepGuard;
    static bool m_bCollectCounters;
    static swss::DBConnector m_db;
    static swss::Table m_countersTable;

    PortsOrch *m_portOrch;
    MirrorOrch *m_mirrorOrch;
};

#endif /* SWSS_ACLORCH_H */
