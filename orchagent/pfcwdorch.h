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
    virtual bool startWd(sai_object_id_t queueId, sai_object_id_t portId,
            uint32_t detectionTime, uint32_t restorationTime, PfcWdAction action) = 0;
    virtual bool stopWd(sai_object_id_t queueId) = 0;

    void updateWdCounters(const std::string& queueIdStr, bool operational);

    inline ProducerStateTable &getPfcWdTable(void)
    {
        return m_pfcWdTable;
    }

private:
    void initWdCounters(const std::string &queueIdStr);
    void getWdCounters(const std::string& queueIdStr, uint32_t& detectCount, uint32_t& restoreCount);
    static PfcWdAction deserializeAction(const string& key);
    void createEntry(const string& key, const vector<FieldValueTuple>& data);
    void deleteEntry(const string& name);

    DBConnector m_pfcWdDb;
    DBConnector m_countersDb;
    ProducerStateTable m_pfcWdTable;
    Table m_countersTable;
};

class PfcWdSwOrch: public PfcWdOrch
{
public:
    PfcWdSwOrch(DBConnector *db, vector<string> &tableNames);
    virtual ~PfcWdSwOrch(void);

    virtual std::vector<sai_port_stat_t> getPortCounterIds(sai_object_id_t queueId) = 0;
    virtual std::vector<sai_queue_stat_t> getQueueCounterIds(sai_object_id_t queueId) = 0;
    virtual std::string getStormDetectionCriteria(void) = 0;

    virtual std::shared_ptr<PfcWdActionHandler> createForwardHandler(sai_object_id_t port,
            sai_object_id_t queue, uint32_t queueId) = 0;
    virtual std::shared_ptr<PfcWdActionHandler> createDropHandler(sai_object_id_t port,
            sai_object_id_t queue, uint32_t queueId) = 0;

    virtual bool startWd(sai_object_id_t queueId, sai_object_id_t portId,
            uint32_t detectionTime, uint32_t restorationTime, PfcWdAction action);
    virtual bool stopWd(sai_object_id_t queueId);

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
        std::shared_ptr<PfcWdActionHandler> handler = { nullptr };
    };

    template <typename T>
    static std::string counterIdsToStr(const std::vector<T> ids, std::string (*convert)(T));
    bool addToWatchdogDb(sai_object_id_t queueId, sai_object_id_t portId,
            uint32_t detectionTime, uint32_t restorationTime, PfcWdAction action);
    bool removeFromWatchdogDb(sai_object_id_t queueId);
    uint32_t getNearestPollTime(void);
    void pollQueues(uint32_t nearestTime, DBConnector& db, std::string detectSha, std::string restoreSha);
    void pfcWatchdogThread(void);
    void startWatchdogThread(void);
    void endWatchdogThread(void);

    std::map<sai_object_id_t, PfcWdQueueEntry> m_entryMap;
    std::mutex m_pfcWdMutex;

    std::atomic_bool m_runPfcWdSwOrchThread = { false };
    std::shared_ptr<std::thread> m_pfcWatchdogThread = nullptr;
    std::mutex m_mtxSleep;
    std::condition_variable m_cvSleep;
};

class PfcDurationWatchdog: public PfcWdSwOrch
{
public:
    PfcDurationWatchdog(DBConnector *db, vector<string> &tableNames);
    virtual ~PfcDurationWatchdog(void);

    virtual std::vector<sai_port_stat_t> getPortCounterIds(sai_object_id_t queueId);
    virtual std::vector<sai_queue_stat_t> getQueueCounterIds(sai_object_id_t queueId);
    virtual std::string getStormDetectionCriteria(void);

    virtual std::shared_ptr<PfcWdActionHandler> createForwardHandler(sai_object_id_t port,
            sai_object_id_t queue, uint32_t queueId);
    virtual std::shared_ptr<PfcWdActionHandler> createDropHandler(sai_object_id_t port,
            sai_object_id_t queue, uint32_t queueId);
};

#endif
