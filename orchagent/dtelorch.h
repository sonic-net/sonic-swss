#ifndef SWSS_DTELORCH_H
#define SWSS_DTELORCH_H

#include "orch.h"
#include "producerstatetable.h"
#include "portsorch.h"

#include <map>
#include <inttypes.h>

#define INT_ENDPOINT                   "INT_ENDPOINT"
#define INT_TRANSIT                    "INT_TRANSIT"
#define POSTCARD                       "POSTCARD"
#define DROP_REPORT                    "DROP_REPORT"
#define QUEUE_REPORT                   "QUEUE_REPORT"
#define SWITCH_ID                      "SWITCH_ID"
#define FLOW_STATE_CLEAR_CYCLE         "FLOW_STATE_CLEAR_CYCLE"
#define LATENCY_SENSITIVITY            "LATENCY_SENSITIVITY"
#define SINK_PORT_LIST                 "SINK_PORT_LIST"
#define INT_L4_DSCP                    "INT_L4_DSCP"
#define INT_L4_DSCP_VALUE              "INT_L4_DSCP_VALUE"
#define INT_L4_DSCP_MASK               "INT_L4_DSCP_MASK"
#define SRC_IP                         "SRC_IP"
#define DST_IP_LIST                    "DST_IP_LIST"
#define VRF                            "VRF"
#define TRUNCATE_SIZE                  "TRUNCATE_SIZE"
#define UDP_DEST_PORT                  "UDP_DEST_PORT"
#define MAX_HOP_COUNT                  "MAX_HOP_COUNT"
#define COLLECT_SWITCH_ID              "COLLECT_SWITCH_ID"
#define COLLECT_INGRESS_TIMESTAMP      "COLLECT_INGRESS_TIMESTAMP"
#define COLLECT_EGRESS_TIMESTAMP       "COLLECT_EGRESS_TIMESTAMP"
#define COLLECT_SWITCH_PORTS           "COLLECT_SWITCH_PORTS"
#define COLLECT_QUEUE_INFO             "COLLECT_QUEUE_INFO"
#define QUEUE_DEPTH_THRESHOLD          "QUEUE_DEPTH_THRESHOLD"
#define QUEUE_LATENCY_THRESHOLD        "QUEUE_LATENCY_THRESHOLD"
#define THRESHOLD_BREACH_QUOTA         "THRESHOLD_BREACH_QUOTA"
#define REPORT_TAIL_DROP               "REPORT_TAIL_DROP"
#define EVENT_REPORT_SESSION           "EVENT_REPORT_SESSION"
#define EVENT_DSCP_VALUE               "EVENT_DSCP_VALUE"
#define EVENT_TYPE_FLOW_STATE                       "EVENT_TYPE_FLOW_STATE"
#define EVENT_TYPE_FLOW_REPORT_ALL_PACKETS          "EVENT_TYPE_FLOW_REPORT_ALL_PACKETS"
#define EVENT_TYPE_FLOW_TCPFLAG                     "EVENT_TYPE_FLOW_TCPFLAG"
#define EVENT_TYPE_QUEUE_REPORT_THRESHOLD_BREACH    "EVENT_TYPE_QUEUE_REPORT_THRESHOLD_BREACH"
#define EVENT_TYPE_QUEUE_REPORT_TAIL_DROP           "EVENT_TYPE_QUEUE_REPORT_TAIL_DROP"
#define EVENT_TYPE_DROP_REPORT                      "EVENT_TYPE_DROP_REPORT"

#define ENABLED             "TRUE"
#define DISABLED            "FALSE"

struct DTelINTSessionEntry
{
    sai_object_id_t intSessionOid;
    int64_t refCount;

    DTelINTSessionEntry() :
        intSessionOid(0),
        refCount(1)
    {
    }
};

struct DTelReportSessionEntry
{
	sai_object_id_t reportSessionOid;
    int64_t refCount;

    DTelReportSessionEntry() :
        reportSessionOid(0),
        refCount(1)
    {
    }
};

struct DTelQueueReportEntry
{
    sai_object_id_t queueReportOid;
    vector<sai_attribute_t> queue_report_attr;
    uint32_t q_ind;
    sai_object_id_t queueOid;

    DTelQueueReportEntry() :
        queueReportOid(0),
        q_ind(0)
    {
    }
};

typedef map<string, DTelQueueReportEntry> dTelPortQueueTable_t;

struct DTelPortEntry
{
    dTelPortQueueTable_t queueTable;
};

struct DTelEventEntry
{
    sai_object_id_t eventOid;
    string reportSessionId;

    DTelEventEntry() :
        eventOid(0),
        reportSessionId("")
    {
    }
};


typedef map<string, DTelINTSessionEntry> dTelINTSessionTable_t;
typedef map<string, DTelReportSessionEntry> dTelReportSessionTable_t;
typedef map<string, DTelPortEntry> dTelPortTable_t;
typedef map<string, DTelEventEntry> dtelEventTable_t;
typedef map<string, sai_dtel_event_type_t> dtelEventLookup_t;
typedef map<string, sai_object_id_t> dtelSinkPortList_t;

struct DTelINTSessionUpdate
{
    string session_id;
    bool active;
};

class DTelOrch : public Orch, public Subject
{
public:
	DTelOrch(DBConnector *db, vector<string> tableNames, PortsOrch *portOrch);
    ~DTelOrch();

    bool increaseINTSessionRefCount(const string&);
    bool decreaseINTSessionRefCount(const string&);
    bool getINTSessionOid(const string& name, sai_object_id_t& oid);
    void update(SubjectType, void *);

private:

	bool intSessionExists(const string& name);
	void doTask(Consumer &consumer);
    void doDtelTableTask(Consumer &consumer);
    void doDtelReportSessionTableTask(Consumer &consumer);
    void doDtelINTSessionTableTask(Consumer &consumer);
    void doDtelQueueReportTableTask(Consumer &consumer);
    void doDtelEventTableTask(Consumer &consumer);

    int64_t getINTSessionRefCount(const string& name);
    bool reportSessionExists(const string& name);
    bool getReportSessionOid(const string& name, sai_object_id_t& oid);
    bool increaseReportSessionRefCount(const string& name);
    bool decreaseReportSessionRefCount(const string& name);
    int64_t getReportSessionRefCount(const string& name);
    bool isQueueReportEnabled(const string& port, const string& queue);
    bool getQueueReportOid(const string& port, const string& queue, sai_object_id_t& oid);
    bool addPortQueue(const string& port, const string& queue, DTelQueueReportEntry **qreport);
    bool removePortQueue(const string& port, const string& queue);
    bool isEventConfigured(const string& event);
    bool getEventOid(const string& event, sai_object_id_t& oid);
    void addEvent(const string& event, const sai_object_id_t& event_oid, const string& report_session_id);
    void removeEvent(const string& event);
    bool deleteReportSession(const string& report_session_id);
    bool deleteINTSession(const string& int_session_id);
    bool disableQueueReport(const string& port, const string& queue);
    bool unConfigureEvent(string& event);
    sai_status_t updateSinkPortList();
    bool addSinkPortToCache(const Port& port);
    bool removeSinkPortFromCache(const string& port_alias);
    sai_status_t enableQueueReport(const string& port, DTelQueueReportEntry& qreport);

	PortsOrch *m_portOrch;
	dTelINTSessionTable_t m_dTelINTSessionTable;
    dTelReportSessionTable_t m_dTelReportSessionTable;
    dTelPortTable_t m_dTelPortTable;
    dtelEventTable_t m_dtelEventTable;
    sai_object_id_t dtelId;
    dtelSinkPortList_t sinkPortList;
};

#endif /* SWSS_DTELORCH_H */
