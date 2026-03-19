#ifndef SWSS_TAMORCH_H
#define SWSS_TAMORCH_H

#include "orch.h"
#include "observer.h"
#include "producerstatetable.h"
#include "aclorch.h"
#include "table.h"

#include <map>
#include <inttypes.h>

#define TAM_IFA_L3_PROTOCOL        253
#define ACTION_INT_INSERT          "INT_INSERT_ACTION"
#define ACTION_INT_DELETE          "INT_DELETE_ACTION"

#define TAM_IFA_ACL_TABLE          "TAM_IFA"
#define TAM_IFA_ACL_RULE           "TRANSIT"

#define MATCH_TAM_INT_TYPE         "TAM_INT_TYPE"
#define MATCH_TAM_INT_TYPE_IFA2    "TAM_INT_TYPE_IFA2"

#define ACTION_TAM_INT_OBJECT      "TAM_INT_OBJECT"
#define ACTION_TAM_OBJECT          "TAM_OBJECT"

#define COLLECTOR_VRF              "vrf"
#define COLLECTOR_SRC_IP           "src_ip"
#define COLLECTOR_DST_IP           "dst_ip"
#define COLLECTOR_L4_DST_PORT      "dst_port"
#define COLLECTOR_DSCP_VALUE       "dscp_value"

#define SESSION_TYPE_DROP_MONITOR  "drop-monitor"
#define SESSION_REPORT_TYPE_IPFIX  "ipfix"
#define SESSION_TYPE               "type"
#define SESSION_REPORT_TYPE        "report_type"
#define SESSION_FLOW_GROUP         "flow_group"
#define SESSION_COLLECTOR          "collector"

#define TAM_DEFAULT_FLOW_GROUP_NAME "default-tam-flow-group"
#define TAM_DEFAULT_FLOW_RULE_NAME  "default-tam-flow-rule"
#define DROP_MONITOR_STAGE_INGRESS 0x01
#define DROP_MONITOR_STAGE_EGRESS  0x02
#define DROP_MONITOR_STAGE_TM      0x04
#define DROP_MONITOR_STAGE_ALL (DROP_MONITOR_STAGE_INGRESS|DROP_MONITOR_STAGE_EGRESS|DROP_MONITOR_STAGE_TM)
#define TAM_DROP_EVENT_ID_BASE     10

struct TamTransportEntry
{
    uint32_t transportType;
    uint16_t l4SrcPort;
    uint16_t l4DstPort;
    MacAddress srcMac;
    MacAddress dstMac;
    sai_object_id_t transportObjId;
    uint32_t refCount;

    TamTransportEntry( uint32_t transportType,
                       uint16_t l4SrcPort,
                       uint16_t l4DstPort,
                       const MacAddress& srcMac,
                       const MacAddress& dstMac,
                       sai_object_id_t transportObjId );
};

struct TamCollectorEntry {
    string collectorName;
    sai_object_id_t collectorObjId;

    string vrfName;
    IpAddress srcIp;
    IpAddress dstIp;
    uint8_t dscp;
    uint16_t l4DstPort;

    sai_object_id_t transportObjId;
    uint32_t refCount;
    bool resolved;

    struct NextHopInfo
    {
        IpPrefix prefix;
        NextHopKey nexthop;
    } nexthopInfo;

    struct NeighborInfo
    {
        NeighborEntry neighbor;
        MacAddress mac;
        Port port;
        sai_object_id_t portId;
    } neighborInfo;

    bool validConfig();
    TamCollectorEntry( const string& collectorName );
};

struct DropMonitorSession {
    // Report type to be generated
    string reportType;

    // Stateful or stateless
    sai_tam_event_type_t eventType;

    // Bitmap to indicate the kind of drops we are interested in
    uint8_t dropStage;

    // Flow group for which we want drops reported
    string flowGroupName;

    // Collectors for reporting these drop events
    std::vector<std::string> collectorNames;

    // Object Ids
    sai_object_id_t tamReportObjId;
    sai_object_id_t tamEventActionObjId;
    sai_object_id_t ingressEventObjId, egressEventObjId, tmEventObjId;
    sai_object_id_t tamObjId;

    DropMonitorSession();
};

enum TamSessionType {
    tamSessionTypeDropMonitor = 1
};

struct TamSessionEntry {
    string sessionName;
    TamSessionType sessionType;
    bool active;

    DropMonitorSession dropSession;

    TamSessionEntry( const string& sessionName, const TamSessionType sessionType );
};

typedef map<string, std::shared_ptr<TamCollectorEntry>> CollectorEntryByName;
typedef map<sai_object_id_t, std::shared_ptr<TamTransportEntry>> TransportEntryById;
typedef map<string, std::shared_ptr<TamSessionEntry>> SessionEntryByName;
typedef map<string, sai_tam_int_type_t> acl_tam_int_type_lookup_t;

class AclRuleTam: public AclRule
{
public:
    AclRuleTam(AclOrch *m_pAclOrch, string rule, string table, bool createCounter = true);
    bool validateAddPriority(string attrName, string attrValue) override;
    bool validateAddMatch(string attrName, string attrValue) override;
    bool validateAddAction(string attrName, string attrValue) override;
    bool validateAddAction(string attrName);
    bool validateAddAction(string attrName, sai_object_id_t attrValue);

    bool validate() override;
    void onUpdate(SubjectType, void *) override;

protected:
    bool setMatch(sai_acl_entry_attr_t matchId, sai_acl_field_data_t matchData) override;
    bool setAction(sai_acl_entry_attr_t actionId, sai_acl_action_data_t actionData) override;
};

// TAM Flow Group Rule structure
struct TamFlowGroupRule {
    string srcIp;
    string dstIp;
    string ipProtocol;
    string srcL4Port;
    string dstL4Port;
    sai_object_id_t aclRuleOid;
    shared_ptr<AclRuleTam> aclRule;
    bool active;

    TamFlowGroupRule() : aclRuleOid(SAI_NULL_OBJECT_ID), active(false) {}
};

// TAM Flow Group Table structure
struct TamFlowGroupTable {
    string agingInterval;
    vector<string> ports;
    string aclTableId;
    map<string, TamFlowGroupRule> rules;
    bool active;

    TamFlowGroupTable() : active(false) {}
};

class TamOrch : public Orch, public Observer, public Subject
{
public:
    TamOrch(DBConnector *db, vector<string> tableNames, TableConnector stateDbConnector,
            AclOrch *aclOrch, PortsOrch *portOrch, VRFOrch *vrfOrch, RouteOrch *routeOrch,
            NeighOrch *neighOrch, FdbOrch *fdbOrch);
    ~TamOrch();

    void update(SubjectType, void *);

private:
    void doTask(Consumer &consumer);
    void doTamDeviceTableTask(Consumer &consumer);
    void doTamCollectorTableTask(Consumer &consumer);
    void doTamFlowGroupTableTask(Consumer &consumer);
    void doTamSessionTableTask(Consumer &consumer);

    bool doIfaTamReportCreate();
    bool doIfaTamReportDelete();
    bool doIfaTamIntCreate();
    bool doIfaTamIntDelete();
    bool doIfaAclRuleCreate();
    bool doIfaAclRuleDelete();
    bool doIfaAclTableCreate();
    bool doIfaAclTableDelete();

    bool doIfaTransitCreate();
    bool doIfaTransitDelete();
    bool doIfaTransitUpdate();

    // TAM Transport
    void tamTransportIncRefCountById( sai_object_id_t transportObjId );
    void tamTransportDecRefCountById( sai_object_id_t transportObjId );
    bool tamTransportCreate(uint16_t l4SrcPort,
                            uint16_t l4DstPort,
                            const MacAddress& srcMac,
                            const MacAddress& dstMac,
                            sai_object_id_t& transportObjId );
    bool tamTransportDelete( sai_object_id_t transportObjId );

    // TAM
    bool unbindPortFromTam( sai_object_id_t& portId, sai_object_id_t& tamObjId );
    bool bindPortToTam( sai_object_id_t& portId, sai_object_id_t& tamObjId );
    void handleTamCreate( std::shared_ptr<TamSessionEntry> sessionEntry );
    bool tamDelete( sai_object_id_t &tamObjId );
    bool tamCreate( std::shared_ptr<TamSessionEntry> sessionEntry,
                    sai_object_id_t &tamObjId );

    // TAM Session
    bool validateTamSession( std::shared_ptr<TamSessionEntry> sessionEntry );
    string getTamSessionType( const vector<FieldValueTuple>& );

    // Drop Monitor session
    bool validateDropMonitorSession( const DropMonitorSession& dropSession );
    bool tamDropEventDelete( std::shared_ptr<TamSessionEntry> sessionEntry,
                             sai_object_id_t &eventObjId );
    bool tamDropEventCreate( std::shared_ptr<TamSessionEntry> sessionEntry,
                             uint8_t dropStage, sai_object_id_t &eventObjId );
    bool tamEventActionDelete( sai_object_id_t& eventActionObJId );
    bool tamEventActionCreate( sai_object_id_t reportObjId,
                          sai_object_id_t &eventActionObjId );
    bool tamReportCreate( sai_tam_report_type_t reportType,
                          sai_object_id_t &reportObjId );
    bool tamReportDelete( sai_object_id_t &reportObjId );
    void tamDropMonitorIncRefCountForCollector( std::shared_ptr<TamSessionEntry> sessionEntry );
    void tamDropMonitorDecRefCountForCollector( std::shared_ptr<TamSessionEntry> sessionEntry );
    bool tamDropMonitorSessionDelete( std::shared_ptr<TamSessionEntry> sessionEntry );
    bool tamDropMonitorSessionCreate( std::shared_ptr<TamSessionEntry> sessionEntry );
    bool createDropMonitorSession( const string&, const vector<FieldValueTuple>& );
    bool deleteDropMonitorSession( std::shared_ptr<TamSessionEntry> sessionEntry );

    // TAM Collector
    void handleCollectorChange( const string& collectorName );
    bool createTamCollectorEntry( const string&, const vector<FieldValueTuple>& );
    bool deleteTamCollectorEntry( const string& );
    void tamCollectorIncRefCountByName( const string &collectorName );
    void tamCollectorDecRefCountByName( const string &collectorName );
    bool tamCollectorCreate( std::shared_ptr<TamCollectorEntry> collectorEntry,
                             sai_object_id_t transportObj );
    bool tamCollectorDelete( std::shared_ptr<TamCollectorEntry> collectorEntry );
    bool getNeighborInfo( std::shared_ptr<TamCollectorEntry> collectorEntry );
    void handleNextHopUpdate(const NextHopUpdate&);
    void handleNeighborUpdate(const NeighborUpdate&);
    void updateCollector(std::shared_ptr<TamCollectorEntry> collectorEntry);

    // TAM Flow Group methods
    bool doTamFlowGroupTableCreate(const string &key, const map<string, string> &config);
    bool doTamFlowGroupTableDelete(const string &key);
    bool doTamFlowGroupRuleCreate(const string &groupKey, const string &ruleKey,
                                  const map<string, string> &config);
    bool doTamFlowGroupRuleDelete(const string &groupKey, const string &ruleKey);

    bool validateFlowGroupTableConfig(const map<string, string> &config);
    bool validateFlowGroupRuleConfig(const map<string, string> &config);
    bool hasDropMonitorSessionForFlowGroup(const string &flowGroupName);
    sai_object_id_t getTamObjectIdForFlowGroup(const string &flowGroupName);
    void activateFlowGroupRulesForDropMonitor(const string &flowGroupName);
    void deactivateFlowGroupRulesForDropMonitor(const string &flowGroupName);
    bool bindTamToSwitch(sai_object_id_t tamObjId);
    bool unbindTamFromSwitch();

    uint32_t getSystemUniqueId();
    uint32_t getDeviceId();
    uint32_t getEnterpriseId();
    bool getIfaEnabled();

    // Device table update helpers
    bool recreateDropMonitorSessions();

    // STATE_DB session status methods
    void setDropMonitorSessionState(std::shared_ptr<TamSessionEntry> sessionEntry);
    void removeDropMonitorSessionState(const string& sessionName);

    /* globalSettings contains settings from the TAM->device table.
     * - device-id
     * - enterprise-id
     * - ifa
     */
    map<string, string> globalSettings;

    bool ifaAclTableCreated            = false;
    bool ifaAclRuleCreated             = false;
    sai_object_id_t ifaTamReportObj = 0;
    sai_object_id_t ifaTamIntObj    = 0;

    // MOD
    CollectorEntryByName m_collectorTables;
    TransportEntryById m_transportTables;
    SessionEntryByName m_sessionTables;

    // TAM Flow Group storage
    map<string, TamFlowGroupTable> tamFlowGroupTables;

    AclOrch *m_aclOrch;
    PortsOrch *m_portsOrch;
    VRFOrch *m_vrfOrch;
    RouteOrch *m_routeOrch;
    NeighOrch *m_neighOrch;
    FdbOrch *m_fdbOrch;

    // STATE_DB table for drop monitor session status
    Table m_stateDbDropMonitorSessionTable;
};

#endif /* SWSS_TAMORCH_H */
