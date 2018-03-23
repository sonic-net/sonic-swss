#ifndef PFC_WATCHDOG_H
#define PFC_WATCHDOG_H

#include "orch.h"
#include "port.h"
#include "pfcactionhandler.h"
#include "producertable.h"
#include "notificationconsumer.h"
#include "timer.h"
#include <array>

extern "C" {
#include "sai.h"
}

enum class PfcWdAction
{
    PFC_WD_ACTION_UNKNOWN,
    PFC_WD_ACTION_FORWARD,
    PFC_WD_ACTION_DROP,
    PFC_WD_ACTION_ALERT,
};

template <typename DropHandler, typename ForwardHandler>
class PfcWdOrch: public Orch
{
public:
    PfcWdOrch(DBConnector *db, vector<string> &tableNames);
    virtual ~PfcWdOrch(void);

    virtual void doTask(Consumer& consumer);
    virtual bool startWdOnPort(const Port& port,
            uint32_t detectionTime, uint32_t restorationTime, PfcWdAction action) = 0;
    virtual bool stopWdOnPort(const Port& port) = 0;

    shared_ptr<Table> getCountersTable(void)
    {
        return m_countersTable;
    }

    shared_ptr<DBConnector> getCountersDb(void)
    {
        return m_countersDb;
    }

    static PfcWdAction deserializeAction(const string& key);
    static string serializeAction(const PfcWdAction &action); 

    virtual void createEntry(const string& key, const vector<FieldValueTuple>& data);
    void deleteEntry(const string& name);
private:

    shared_ptr<DBConnector> m_countersDb = nullptr;
    shared_ptr<Table> m_countersTable = nullptr;
};

template <typename DropHandler, typename ForwardHandler>
class PfcWdSwOrch: public PfcWdOrch<DropHandler, ForwardHandler>
{
public:
    PfcWdSwOrch(
            DBConnector *db,
            vector<string> &tableNames,
            const vector<sai_port_stat_t> &portStatIds,
            const vector<sai_queue_stat_t> &queueStatIds,
            const vector<sai_queue_attr_t> &queueAttrIds,
            int pollInterval);
    virtual ~PfcWdSwOrch(void);

    virtual bool startWdOnPort(const Port& port,
            uint32_t detectionTime, uint32_t restorationTime, PfcWdAction action);
    virtual bool stopWdOnPort(const Port& port);

    void createEntry(const string& key, const vector<FieldValueTuple>& data);
    //XXX Add port/queue state change event handlers
private:
    struct PfcWdQueueEntry
    {
        PfcWdQueueEntry(
                PfcWdAction action,
                sai_object_id_t port,
                uint8_t idx);

        PfcWdAction action = PfcWdAction::PFC_WD_ACTION_UNKNOWN;
        sai_object_id_t portId = SAI_NULL_OBJECT_ID;
        uint8_t index = 0;
        shared_ptr<PfcWdActionHandler> handler = { nullptr };
    };

    template <typename T>
    static string counterIdsToStr(const vector<T> ids, string (*convert)(T));
    void registerInWdDb(const Port& port,
            uint32_t detectionTime, uint32_t restorationTime, PfcWdAction action);
    void unregisterFromWdDb(const Port& port);
    void doTask(swss::NotificationConsumer &wdNotification);

    string filterPfcCounters(string counters, set<uint8_t>& losslessTc);
    string getFlexCounterTableKey(string s);
    map<sai_object_id_t, PfcWdQueueEntry> m_entryMap;

    const vector<sai_port_stat_t> c_portStatIds;
    const vector<sai_queue_stat_t> c_queueStatIds;
    const vector<sai_queue_attr_t> c_queueAttrIds;

    shared_ptr<DBConnector> m_flexCounterDb = nullptr;
    shared_ptr<ProducerTable> m_flexCounterTable = nullptr;
    shared_ptr<ProducerTable> m_flexCounterGroupTable = nullptr;

    atomic_bool m_runPfcWdSwOrchThread = { false };
    shared_ptr<thread> m_pfcWatchdogThread = nullptr;

    int m_pollInterval;
};

#endif
