#ifndef SWSS_TWAMPORCH_H
#define SWSS_TWAMPORCH_H

#include "orchagent/orch.h"
#include "orchagent/observer.h"
#include "orchagent/switchorch.h"
#include "orchagent/portsorch.h"
#include "orchagent/vrforch.h"
#include "ipaddress.h"
#include "table.h"
#include <map>

struct TwampStats
{
    uint64_t rx_packets;
    uint64_t rx_bytes;
    uint64_t tx_packets;
    uint64_t tx_bytes;
    uint64_t drop_packets;
    uint64_t max_latency;
    uint64_t min_latency;
    uint64_t avg_latency;
    uint64_t max_jitter;
    uint64_t min_jitter;
    uint64_t avg_jitter;
    uint64_t avg_latency_total;
    uint64_t avg_jitter_total;
};

struct TwampEntry
{
    uint8_t  mode;         /* twamp mode: full, light */
    uint8_t  role;         /* sender, reflector */
    bool     admin_state;  /* test packet state. enabled, disabled */
    bool     hw_lookup;

    sai_object_id_t  vrf_id;
    IpAddress src_ip;
    IpAddress dst_ip;
    uint16_t  src_udp_port;
    uint16_t  dst_udp_port;
    uint16_t  padding_size;
    uint8_t   dscp;
    uint8_t   ttl;
    uint8_t   timestamp_format;

    /* sender attr */
    string    tx_mode;
    uint32_t  packet_count;
    uint32_t  monitor_time;         /* second */
    uint32_t  tx_interval;          /* millisecond */
    uint32_t  statistics_interval;  /* millisecond */
    uint8_t   timeout;              /* second */

    sai_object_id_t session_id;

    TwampEntry()
    {
        session_id = 0;
        admin_state = false;
        hw_lookup = true;
        vrf_id = 0;
        packet_count = 0;
        monitor_time = 0;
        tx_interval = 0;
        statistics_interval = 0;
        timeout = 0;
    };
};

typedef map<string, TwampEntry> TwampEntryTable;
typedef map<string, TwampStats> TwampStatsTable;

class TwampOrch : public Orch
{
public:
    TwampOrch(TableConnector confDbConnector, TableConnector stateDbConnector,
              SwitchOrch *switchOrch, PortsOrch *portOrch, VRFOrch *vrfOrch);

    ~TwampOrch()
    {
        // do nothing
    }

    bool isSessionExists(const string&);
    bool getSessionName(const sai_object_id_t oid, string& name);

private:
    SwitchOrch *m_switchOrch;
    PortsOrch  *m_portsOrch;
    VRFOrch    *m_vrfOrch;
    NotificationConsumer* m_twampNotificationConsumer;
    bool register_event_notif;

    unsigned int m_twampSessionCount;
    unsigned int m_maxTwampSessionCount;

    TwampEntryTable m_twampEntries;
    TwampStatsTable m_twampStatistics;

    shared_ptr<DBConnector> m_asicDb;
    shared_ptr<DBConnector> m_countersDb;
    unique_ptr<Table> m_counterTwampSessionNameMapTable;
    unique_ptr<Table> m_countersTable;
    unique_ptr<Table> m_vidToRidTable;
    Table m_stateDbTwampTable;

    bool validateUdpPort(uint16_t udp_port);
    void increaseTwampSessionCount(void);
    void decreaseTwampSessionCount(void);
    bool checkTwampSessionCount(void);

    void setSessionStatus(const string&, const string&);
    bool getSessionStatus(const string&, string&);
    void removeSessionStatus(const string&);
    void removeSessionCounter(const sai_object_id_t);
    void initSessionStats(const string&);

    bool registerTwampEventNotification(void);
    bool activateSession(const string&, TwampEntry&);
    bool deactivateSession(const string&, TwampEntry&);
    bool setSessionTransmitEn(TwampEntry&, string test_start);

    task_process_status createEntry(const string&, const vector<FieldValueTuple>&);
    task_process_status updateEntry(const string&, const vector<FieldValueTuple>&);
    task_process_status deleteEntry(const string&);
    void doTask(Consumer& consumer);

    bool addCounterNameMap(const string&, const sai_object_id_t session_id);
    void saveSessionStatsLatest(const sai_object_id_t session_id, const uint32_t index, const vector<uint64_t>& stats);
    void calculateCounters(const string&, const uint32_t index, const vector<uint64_t>& stats);
    void saveCountersTotal(const string&, const sai_object_id_t session_id);
    void doTask(NotificationConsumer& consumer);
};

#endif /* SWSS_TWAMPORCH_H */
