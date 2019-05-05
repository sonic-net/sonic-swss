#ifndef SWSS_ACLORCH_H
#define SWSS_ACLORCH_H

#include <iostream>
#include <sstream>
#include <thread>
#include <mutex>
#include <tuple>
#include <map>
#include <condition_variable>
#include "orch.h"
#include "portsorch.h"
#include "mirrororch.h"
#include "dtelorch.h"
#include "observer.h"

// ACL counters update interval in the DB
// Value is in seconds. Should not be less than 5 seconds
// (in worst case update of 1265 counters takes almost 5 sec)
#define COUNTERS_READ_INTERVAL 10

#define TABLE_DESCRIPTION "POLICY_DESC"
#define TABLE_TYPE        "TYPE"
#define TABLE_PORTS       "PORTS"
#define TABLE_SERVICES    "SERVICES"

#define TABLE_TYPE_L3                   "L3"
#define TABLE_TYPE_L3V6                 "L3V6"
#define TABLE_TYPE_MIRROR               "MIRROR"
#define TABLE_TYPE_MIRRORV6             "MIRRORV6"
#define TABLE_TYPE_PFCWD                "PFCWD"
#define TABLE_TYPE_CTRLPLANE            "CTRLPLANE"
#define TABLE_TYPE_DTEL_FLOW_WATCHLIST  "DTEL_FLOW_WATCHLIST"
#define TABLE_TYPE_DTEL_DROP_WATCHLIST  "DTEL_DROP_WATCHLIST"

#define RULE_PRIORITY           "PRIORITY"
#define MATCH_IN_PORTS          "IN_PORTS"
#define MATCH_OUT_PORTS         "OUT_PORTS"
#define MATCH_SRC_IP            "SRC_IP"
#define MATCH_DST_IP            "DST_IP"
#define MATCH_SRC_IPV6          "SRC_IPV6"
#define MATCH_DST_IPV6          "DST_IPV6"
#define MATCH_L4_SRC_PORT       "L4_SRC_PORT"
#define MATCH_L4_DST_PORT       "L4_DST_PORT"
#define MATCH_ETHER_TYPE        "ETHER_TYPE"
#define MATCH_IP_PROTOCOL       "IP_PROTOCOL"
#define MATCH_TCP_FLAGS         "TCP_FLAGS"
#define MATCH_IP_TYPE           "IP_TYPE"
#define MATCH_DSCP              "DSCP"
#define MATCH_L4_SRC_PORT_RANGE "L4_SRC_PORT_RANGE"
#define MATCH_L4_DST_PORT_RANGE "L4_DST_PORT_RANGE"
#define MATCH_TC                "TC"
#define MATCH_TUNNEL_VNI        "TUNNEL_VNI"
#define MATCH_INNER_ETHER_TYPE  "INNER_ETHER_TYPE"
#define MATCH_INNER_IP_PROTOCOL "INNER_IP_PROTOCOL"
#define MATCH_INNER_L4_SRC_PORT "INNER_L4_SRC_PORT"
#define MATCH_INNER_L4_DST_PORT "INNER_L4_DST_PORT"

#define ACTION_PACKET_ACTION                "PACKET_ACTION"
#define ACTION_MIRROR_ACTION                "MIRROR_ACTION"
#define ACTION_DTEL_FLOW_OP                 "FLOW_OP"
#define ACTION_DTEL_INT_SESSION             "INT_SESSION"
#define ACTION_DTEL_DROP_REPORT_ENABLE      "DROP_REPORT_ENABLE"
#define ACTION_DTEL_TAIL_DROP_REPORT_ENABLE "TAIL_DROP_REPORT_ENABLE"
#define ACTION_DTEL_FLOW_SAMPLE_PERCENT     "FLOW_SAMPLE_PERCENT"
#define ACTION_DTEL_REPORT_ALL_PACKETS      "REPORT_ALL_PACKETS"

#define PACKET_ACTION_FORWARD   "FORWARD"
#define PACKET_ACTION_DROP      "DROP"
#define PACKET_ACTION_REDIRECT  "REDIRECT"

#define DTEL_FLOW_OP_NOP        "NOP"
#define DTEL_FLOW_OP_POSTCARD   "POSTCARD"
#define DTEL_FLOW_OP_INT        "INT"
#define DTEL_FLOW_OP_IOAM       "IOAM"

#define DTEL_ENABLED             "TRUE"
#define DTEL_DISABLED            "FALSE"

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

#define MLNX_MAX_RANGES_COUNT   16

typedef enum
{
    ACL_TABLE_UNKNOWN,
    ACL_TABLE_L3,
    ACL_TABLE_L3V6,
    ACL_TABLE_MIRROR,
    ACL_TABLE_MIRRORV6,
    ACL_TABLE_PFCWD,
    ACL_TABLE_CTRLPLANE,
    ACL_TABLE_DTEL_FLOW_WATCHLIST,
    ACL_TABLE_DTEL_DROP_WATCHLIST
} acl_table_type_t;

typedef map<string, acl_table_type_t> acl_table_type_lookup_t;
typedef map<string, sai_acl_entry_attr_t> acl_rule_attr_lookup_t;
typedef map<string, sai_acl_ip_type_t> acl_ip_type_lookup_t;
typedef map<string, sai_acl_dtel_flow_op_t> acl_dtel_flow_op_type_lookup_t;
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

struct AclRuleCounters
{
    uint64_t packets;
    uint64_t bytes;

    AclRuleCounters(uint64_t p = 0, uint64_t b = 0) :
        packets(p),
        bytes(b)
    {
    }

    AclRuleCounters(const AclRuleCounters& rhs) :
        packets(rhs.packets),
        bytes(rhs.bytes)
    {
    }

    AclRuleCounters& operator +=(const AclRuleCounters& rhs)
    {
        packets += rhs.packets;
        bytes += rhs.bytes;
        return *this;
    }
};

class AclRule
{
public:
    AclRule(AclOrch *m_pAclOrch, string rule, string table, acl_table_type_t type, bool createCounter = true);
    virtual bool validateAddPriority(string attr_name, string attr_value);
    virtual bool validateAddMatch(string attr_name, string attr_value);
    virtual bool validateAddAction(string attr_name, string attr_value) = 0;
    virtual bool validate() = 0;
    bool processIpType(string type, sai_uint32_t &ip_type);
    inline static void setRulePriorities(sai_uint32_t min, sai_uint32_t max)
    {
        m_minPriority = min;
        m_maxPriority = max;
    }

    virtual bool create();
    virtual bool remove();
    virtual void update(SubjectType, void *) = 0;
    virtual AclRuleCounters getCounters();

    string getId()
    {
        return m_id;
    }

    string getTableId()
    {
        return m_tableId;
    }

    sai_object_id_t getCounterOid()
    {
        return m_counterOid;
    }

    static shared_ptr<AclRule> makeShared(acl_table_type_t type, AclOrch *acl, MirrorOrch *mirror, DTelOrch *dtel, const string& rule, const string& table, const KeyOpFieldsValuesTuple&);
    virtual ~AclRule() {}

protected:
    virtual bool createCounter();
    virtual bool removeCounter();
    virtual bool removeRanges();

    void decreaseNextHopRefCount();

    static sai_uint32_t m_minPriority;
    static sai_uint32_t m_maxPriority;
    AclOrch *m_pAclOrch;
    string m_id;
    string m_tableId;
    acl_table_type_t m_tableType;
    sai_object_id_t m_tableOid;
    sai_object_id_t m_ruleOid;
    sai_object_id_t m_counterOid;
    uint32_t m_priority;
    map <sai_acl_entry_attr_t, sai_attribute_value_t> m_matches;
    map <sai_acl_entry_attr_t, sai_attribute_value_t> m_actions;
    string m_redirect_target_next_hop;
    string m_redirect_target_next_hop_group;

    vector<sai_object_id_t> m_inPorts;
    vector<sai_object_id_t> m_outPorts;

private:
    bool m_createCounter;
};

class AclRuleL3: public AclRule
{
public:
    AclRuleL3(AclOrch *m_pAclOrch, string rule, string table, acl_table_type_t type, bool createCounter = true);

    bool validateAddAction(string attr_name, string attr_value);
    bool validateAddMatch(string attr_name, string attr_value);
    bool validate();
    void update(SubjectType, void *);
protected:
    sai_object_id_t getRedirectObjectId(const string& redirect_param);
};

class AclRuleL3V6: public AclRuleL3
{
public:
    AclRuleL3V6(AclOrch *m_pAclOrch, string rule, string table, acl_table_type_t type);
    bool validateAddMatch(string attr_name, string attr_value);
};

class AclRulePfcwd: public AclRuleL3
{
public:
    AclRulePfcwd(AclOrch *m_pAclOrch, string rule, string table, acl_table_type_t type, bool createCounter = false);
    bool validateAddMatch(string attr_name, string attr_value);
};


class AclRuleMirror: public AclRule
{
public:
    AclRuleMirror(AclOrch *m_pAclOrch, MirrorOrch *m_pMirrorOrch, string rule, string table, acl_table_type_t type);
    bool validateAddAction(string attr_name, string attr_value);
    bool validateAddMatch(string attr_name, string attr_value);
    bool validate();
    bool create();
    bool remove();
    void update(SubjectType, void *);
    AclRuleCounters getCounters();

protected:
    bool m_state;
    string m_sessionName;
    AclRuleCounters counters;
    MirrorOrch *m_pMirrorOrch;
};

class AclRuleDTelFlowWatchListEntry: public AclRule
{
public:
    AclRuleDTelFlowWatchListEntry(AclOrch *m_pAclOrch, DTelOrch *m_pDTelOrch, string rule, string table, acl_table_type_t type);
    bool validateAddAction(string attr_name, string attr_value);
    bool validate();
    bool create();
    bool remove();
    void update(SubjectType, void *);

protected:
    DTelOrch *m_pDTelOrch;
    string m_intSessionId;
    bool INT_enabled;
    bool INT_session_valid;
};

class AclRuleDTelDropWatchListEntry: public AclRule
{
public:
    AclRuleDTelDropWatchListEntry(AclOrch *m_pAclOrch, DTelOrch *m_pDTelOrch, string rule, string table, acl_table_type_t type);
    bool validateAddAction(string attr_name, string attr_value);
    bool validate();
    void update(SubjectType, void *);

protected:
    DTelOrch *m_pDTelOrch;
};

class AclTable {
    sai_object_id_t m_oid;
    AclOrch *m_pAclOrch;
public:
    string id;
    string description;
    acl_table_type_t type;
    acl_stage_type_t stage;

    // Map port oid to group member oid
    std::map<sai_object_id_t, sai_object_id_t> ports;
    // Map rule name to rule data
    map<string, shared_ptr<AclRule>> rules;
    // Set to store the ACL table port alias
    set<string> portSet;
    // Set to store the not cofigured ACL table port alias
    set<string> pendingPortSet;

    AclTable()
        : m_pAclOrch(NULL)
        , type(ACL_TABLE_UNKNOWN)
        , m_oid(SAI_NULL_OBJECT_ID)
        , stage(ACL_STAGE_INGRESS)
    {}

    AclTable(AclOrch *aclOrch)
        : m_pAclOrch(aclOrch)
        , type(ACL_TABLE_UNKNOWN)
        , m_oid(SAI_NULL_OBJECT_ID)
        , stage(ACL_STAGE_INGRESS)
    {}

    sai_object_id_t getOid() { return m_oid; }
    string getId() { return id; }
    bool validate();
    bool create();

    // Bind the ACL table to a port which is alread linked
    bool bind(sai_object_id_t portOid);
    // Unbind the ACL table to a port which is alread linked
    bool unbind(sai_object_id_t portOid);
    // Bind the ACL table to all ports linked
    bool bind();
    // Unbind the ACL table to all ports linked
    bool unbind();
    // Link the ACL table with a port, for future bind or unbind
    void link(sai_object_id_t portOid);
    // Add or overwrite a rule into the ACL table
    bool add(shared_ptr<AclRule> newRule);
    // Remove a rule from the ACL table
    bool remove(string rule_id);
    // Remove all rules from the ACL table
    bool clear();
    // Update table subject to changes
    void update(SubjectType, void *);
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

class AclOrch : public Orch, public Observer
{
public:
    AclOrch(vector<TableConnector>& connectors, TableConnector switchTable,
            PortsOrch *portOrch, MirrorOrch *mirrorOrch, NeighOrch *neighOrch, RouteOrch *routeOrch, DTelOrch *m_dTelOrch = NULL);
    ~AclOrch();
    void update(SubjectType, void *);

    sai_object_id_t getTableById(string table_id);

    static swss::Table& getCountersTable()
    {
        return m_countersTable;
    }

    Table m_switchTable;

    // FIXME: Add getters for them? I'd better to add a common directory of orch objects and use it everywhere
    MirrorOrch *m_mirrorOrch;
    NeighOrch *m_neighOrch;
    RouteOrch *m_routeOrch;
    DTelOrch *m_dTelOrch;

    bool addAclTable(AclTable &aclTable, string table_id);
    bool removeAclTable(string table_id);
    bool addAclRule(shared_ptr<AclRule> aclRule, string table_id);
    bool removeAclRule(string table_id, string rule_id);

    bool isCombinedMirrorV6Table();

    bool m_isCombinedMirrorV6Table = true;
    map<acl_table_type_t, bool> m_mirrorTableCapabilities;

private:
    void doTask(Consumer &consumer);
    void doAclTableTask(Consumer &consumer);
    void doAclRuleTask(Consumer &consumer);
    void doTask(SelectableTimer &timer);
    void init(vector<TableConnector>& connectors, PortsOrch *portOrch, MirrorOrch *mirrorOrch, NeighOrch *neighOrch, RouteOrch *routeOrch);

    void queryMirrorTableCapability();

    static void collectCountersThread(AclOrch *pAclOrch);

    bool createBindAclTable(AclTable &aclTable, sai_object_id_t &table_oid);
    sai_status_t bindAclTable(sai_object_id_t table_oid, AclTable &aclTable, bool bind = true);
    sai_status_t deleteUnbindAclTable(sai_object_id_t table_oid);

    bool processAclTableType(string type, acl_table_type_t &table_type);
    bool processAclTableStage(string stage, acl_stage_type_t &acl_stage);
    bool processAclTablePorts(string portList, AclTable &aclTable);
    bool validateAclTable(AclTable &aclTable);
    sai_status_t createDTelWatchListTables();
    sai_status_t deleteDTelWatchListTables();

    map<sai_object_id_t, AclTable> m_AclTables;
    // TODO: Move all ACL tables into one map: name -> instance
    map<string, AclTable> m_ctrlAclTables;

    static mutex m_countersMutex;
    static condition_variable m_sleepGuard;
    static bool m_bCollectCounters;
    static DBConnector m_db;
    static Table m_countersTable;

    string m_mirrorTableId;
    string m_mirrorV6TableId;
};

#endif /* SWSS_ACLORCH_H */
