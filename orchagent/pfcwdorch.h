#ifndef PFC_WATCHDOG_H
#define PFC_WATCHDOG_H

#include <mutex>
#include <condition_variable>
#include "orch.h"
#include "port.h"
#include "pfcactionhandler.h"

extern "C" {
#include "sai.h"
}

class PfcWdOrch: public Orch
{
public:
    enum class PfcWdAction
    {
        PFC_WD_ACTION_UNKNOWN,
        PFC_WD_ACTION_FORWARD,
        PFC_WD_ACTION_DROP,
    };

    PfcWdOrch(DBConnector *db, vector<string> &tableNames);
    virtual ~PfcWdOrch(void);

    virtual void doTask(Consumer& consumer);
    virtual bool startWdOnPort(const Port& port,
            uint32_t detectionTime, uint32_t restorationTime, PfcWdAction action) = 0;
    virtual bool stopWdOnPort(const Port& port) = 0;

    inline shared_ptr<ProducerStateTable> getPfcWdTable(void)
    {
        return m_pfcWdTable;
    }

    inline shared_ptr<Table> getCountersTable(void)
    {
        return m_countersTable;;
    }

private:
    static PfcWdAction deserializeAction(const string& key);
    void createEntry(const string& key, const vector<FieldValueTuple>& data);
    void deleteEntry(const string& name);

    shared_ptr<DBConnector> m_pfcWdDb = nullptr;
    shared_ptr<DBConnector> m_countersDb = nullptr;
    shared_ptr<ProducerStateTable> m_pfcWdTable = nullptr;
    shared_ptr<Table> m_countersTable = nullptr;
};

class PfcWdSwOrch: public PfcWdOrch
{
public:
    PfcWdSwOrch(DBConnector *db, vector<string> &tableNames);
    virtual ~PfcWdSwOrch(void);

    virtual vector<sai_port_stat_t> getPortCounterIds(sai_object_id_t queueId) = 0;
    virtual vector<sai_queue_stat_t> getQueueCounterIds(sai_object_id_t queueId) = 0;
    virtual string getStormDetectionCriteria(void) = 0;

    virtual shared_ptr<PfcWdActionHandler> createForwardHandler(sai_object_id_t port,
            sai_object_id_t queue, uint32_t queueId) = 0;
    virtual shared_ptr<PfcWdActionHandler> createDropHandler(sai_object_id_t port,
            sai_object_id_t queue, uint32_t queueId) = 0;

    virtual bool startWdOnPort(const Port& port,
            uint32_t detectionTime, uint32_t restorationTime, PfcWdAction action);
    virtual bool stopWdOnPort(const Port& port);

    //XXX Add port/queue state change event handlers
private:
    struct PfcWdQueueEntry
    {
        PfcWdQueueEntry(uint32_t detectionTime, uint32_t restorationTime, PfcWdAction action, sai_object_id_t port);

        const uint32_t c_detectionTime = 0;
        const uint32_t c_restorationTime = 0;
        const PfcWdAction c_action = PfcWdAction::PFC_WD_ACTION_UNKNOWN;

        sai_object_id_t portId = SAI_NULL_OBJECT_ID;
        // Remaining time till the next poll
        uint32_t pollTimeLeft = 0;
        uint32_t index = 0;
        shared_ptr<PfcWdActionHandler> handler = { nullptr };
    };

    bool startWdOnQueue(sai_object_id_t queueId, sai_object_id_t portId,
            uint32_t detectionTime, uint32_t restorationTime, PfcWdAction action);
    bool stopWdOnQueue(sai_object_id_t queueId);
    template <typename T>
    static string counterIdsToStr(const vector<T> ids, string (*convert)(T));
    bool addToWatchdogDb(sai_object_id_t queueId, sai_object_id_t portId,
            uint32_t detectionTime, uint32_t restorationTime, PfcWdAction action);
    bool removeFromWatchdogDb(sai_object_id_t queueId);
    uint32_t getNearestPollTime(void);
    void pollQueues(uint32_t nearestTime, DBConnector& db, string detectSha, string restoreSha);
    void pfcWatchdogThread(void);
    void startWatchdogThread(void);
    void endWatchdogThread(void);

    map<sai_object_id_t, PfcWdQueueEntry> m_entryMap;
    mutex m_pfcWdMutex;

    atomic_bool m_runPfcWdSwOrchThread = { false };
    shared_ptr<thread> m_pfcWatchdogThread = nullptr;
    mutex m_mtxSleep;
    condition_variable m_cvSleep;
};

class PfcDurationWatchdog: public PfcWdSwOrch
{
public:
    PfcDurationWatchdog(DBConnector *db, vector<string> &tableNames);
    virtual ~PfcDurationWatchdog(void);

    virtual vector<sai_port_stat_t> getPortCounterIds(sai_object_id_t queueId);
    virtual vector<sai_queue_stat_t> getQueueCounterIds(sai_object_id_t queueId);
    virtual string getStormDetectionCriteria(void);

    virtual shared_ptr<PfcWdActionHandler> createForwardHandler(sai_object_id_t port,
            sai_object_id_t queue, uint32_t queueId);
    virtual shared_ptr<PfcWdActionHandler> createDropHandler(sai_object_id_t port,
            sai_object_id_t queue, uint32_t queueId);
};

#endif
