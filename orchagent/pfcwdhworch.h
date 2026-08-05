#ifndef PFC_WATCHDOG_HW_H
#define PFC_WATCHDOG_HW_H

#include "pfcwdorch.h"
#include "timer.h"
#include "dbconnector.h"
#include "flex_counter/flex_counter_manager.h"

extern "C" {
#include "sai.h"
}

// Hardware watchdog STATE_DB field names
#define PFC_WD_HW_DETECTION_TIME_MIN       "DETECTION_TIME_MIN"
#define PFC_WD_HW_DETECTION_TIME_MAX       "DETECTION_TIME_MAX"
#define PFC_WD_HW_RESTORATION_TIME_MIN     "RESTORATION_TIME_MIN"
#define PFC_WD_HW_RESTORATION_TIME_MAX     "RESTORATION_TIME_MAX"

class PfcWdHwOrch: public PfcWdBaseOrch
{
public:
    PfcWdHwOrch(DBConnector *db, vector<string> &tableNames,
                const vector<sai_port_stat_t> &portStatIds,
                const vector<sai_queue_stat_t> &queueStatIds,
                const vector<sai_queue_attr_t> &queueAttrIds);
    virtual ~PfcWdHwOrch(void);

    // Implementation-specific methods
    virtual bool startWdOnPort(const Port& port, uint32_t detectionTime,
                              uint32_t restorationTime, PfcWdAction action,
                              string pfcStatHistory) override;
    virtual bool stopWdOnPort(const Port& port) override;

    task_process_status createEntry(const string& key, const vector<FieldValueTuple>& data) override;
    task_process_status deleteEntry(const string& key) override;

    // Timer-based polling for hardware counters
    virtual void doTask(SelectableTimer &timer);

protected:
    bool startWdActionOnQueue(const string &event, sai_object_id_t queueId, const string &info="") override;

public:
    // PFC deadlock notification callback
    void onQueuePfcDeadlock(uint32_t count, sai_queue_deadlock_notification_data_t *data);

private:
    // Hardware counter snapshot read from COUNTERS_DB
    struct PfcWdHwStats
    {
        uint64_t txPkt;
        uint64_t txDropPkt;
        uint64_t rxPkt;
        uint64_t rxDropPkt;
    };

    // Queue statistics for state tracking
    struct PfcWdQueueStats
    {
        uint64_t detectCount;
        uint64_t restoreCount;
        uint64_t txPkt;
        uint64_t txDropPkt;
        uint64_t rxPkt;
        uint64_t rxDropPkt;
        uint64_t txPktLast;
        uint64_t txDropPktLast;
        uint64_t rxPktLast;
        uint64_t rxDropPktLast;
        bool     operational;
    };

    // Get current queue statistics from COUNTERS_DB
    PfcWdQueueStats getQueueStats(const string &queueIdStr);

    // Update queue statistics in COUNTERS_DB
    void updateQueueStats(const string &queueIdStr, const PfcWdQueueStats &stats);

    // Read hardware counters from COUNTERS_DB
    bool readHwCounters(sai_object_id_t queueId, uint8_t queueIndex, PfcWdHwStats& counters);

    // Initialize counters when deadlock is detected
    void initQueueCounters(const string& queueIdStr, sai_object_id_t queueId, uint8_t queueIndex);

    // Update counters periodically or on recovery
    void updateQueueCounters(const string& queueIdStr, sai_object_id_t queueId,
                            uint8_t queueIndex, bool periodic);

private:
    // Hardware-specific methods
    bool configureHwWatchdog(const Port& port, uint32_t detectionTime,
                            uint32_t restorationTime, PfcWdAction action);
    bool disableHwWatchdog(const Port& port);

    // Member variables
    const vector<sai_port_stat_t> c_portStatIds;
    const vector<sai_queue_stat_t> c_queueStatIds;
    const vector<sai_queue_attr_t> c_queueAttrIds;
    
    // Hardware timer ranges
    uint32_t m_detectionTimeMin;
    uint32_t m_detectionTimeMax;
    uint32_t m_restorationTimeMin;
    uint32_t m_restorationTimeMax;

    // STATE_DB for hardware watchdog state
    shared_ptr<DBConnector> m_stateDb;
    shared_ptr<Table> m_pfcWdHwStateTable;

    // Round up user value to nearest valid interval
    uint32_t roundUpToValidInterval(uint32_t requestedTime, const vector<uint32_t>& validIntervals);

    // Write failure status to STATE_DB
    void writeFailureStatus(const Port& port);

    // Check if port has any queue in stormed state
    bool isPortInStormedState(const Port& port);

    // Read back and verify timer value from hardware
    bool readBackTimerValue(const Port& port, sai_port_attr_t attrId,
                           const set<uint8_t>& losslessTc, uint32_t expected,
                           uint32_t& actual, const string& timerName);

    // Initialization functions
    void initializeTimerRanges();
    void registerCallbacks();
    void recoverWarmReboot(DBConnector *db);

    // Configuration functions
    bool configureSwitchAction(const Port& port, PfcWdAction action,
                               const function<bool(const string&)>& handleFailure);
    bool configureTimerIntervals(const Port& port, const set<uint8_t>& losslessTc,
                                 uint32_t detectionTime, uint32_t restorationTime,
                                 const function<bool(const string&)>& handleFailure);
    bool enableDldrOnLosslessQueues(const Port& port, const set<uint8_t>& losslessTc,
                                    uint32_t detectionTime, uint32_t restorationTime,
                                    const function<bool(const string&)>& handleFailure);
    void initializeQueueStats(const Port& port, const set<uint8_t>& losslessTc);

    // Ports where hardware watchdog is configured
    std::set<std::string> m_hwWdPorts;

    // Port and queue information
    struct PortQueueInfo
    {
        sai_object_id_t port_id;
        std::string port_alias;
        uint8_t queue_index;
    };

    // Queue ID to port/queue info mapping
    std::unordered_map<sai_object_id_t, PortQueueInfo> m_queueToPortMap;

    // Queue ID to baseline hardware counters
    std::unordered_map<sai_object_id_t, PfcWdHwStats> m_queueBaselineStats;
};

#endif