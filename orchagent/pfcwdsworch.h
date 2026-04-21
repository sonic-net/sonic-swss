#ifndef PFC_WATCHDOG_SW_H
#define PFC_WATCHDOG_SW_H

#include "pfcwdorch.h"
#include "orch.h"
#include "port.h"
#include "producertable.h"
#include "notificationconsumer.h"
#include "timer.h"

extern "C" {
#include "sai.h"
}

template <typename DropHandler, typename ForwardHandler>
class PfcWdSwOrch: public PfcWdBaseOrch
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

    void doTask(Consumer& consumer) override;
    virtual bool startWdOnPort(const Port& port,
            uint32_t detectionTime, uint32_t restorationTime, PfcWdAction action, string pfcStatHistory);
    virtual bool stopWdOnPort(const Port& port);

    task_process_status createEntry(const string& key, const vector<FieldValueTuple>& data) override;
    virtual void doTask(SelectableTimer &timer);
    //XXX Add port/queue state change event handlers

    bool bake() override;
    void doTask() override;

protected:
    bool startWdActionOnQueue(const string &event, sai_object_id_t queueId, const string &info="") override;

private:
    struct PfcWdQueueEntry
    {
        PfcWdQueueEntry(
                PfcWdAction action,
                sai_object_id_t port,
                uint8_t idx,
                string alias);

        PfcWdAction action = PfcWdAction::PFC_WD_ACTION_UNKNOWN;
        sai_object_id_t portId = SAI_NULL_OBJECT_ID;
        uint8_t index = 0;
        string portAlias;
        shared_ptr<PfcWdActionHandler> handler = { nullptr };
    };

    bool registerInWdDb(const Port& port,
            uint32_t detectionTime, uint32_t restorationTime, PfcWdAction action, string pfcStatHistory);
    void unregisterFromWdDb(const Port& port);
    void doTask(swss::NotificationConsumer &wdNotification);

    unordered_set<string> filterPfcCounters(const unordered_set<string> &counters, set<uint8_t>& losslessTc);
    string getFlexCounterTableKey(string s);

    void disableBigRedSwitchMode();
    void enableBigRedSwitchMode();
    void setBigRedSwitchMode(string value);

    map<sai_object_id_t, PfcWdQueueEntry> m_entryMap;
    map<sai_object_id_t, PfcWdQueueEntry> m_brsEntryMap;

    const vector<sai_port_stat_t> c_portStatIds;
    const vector<sai_queue_stat_t> c_queueStatIds;
    const vector<sai_queue_attr_t> c_queueAttrIds;

    bool m_bigRedSwitchFlag = false;
    int m_pollInterval;

    shared_ptr<DBConnector> m_applDb = nullptr;
    // Track queues in storm
    shared_ptr<Table> m_applTable = nullptr;
};

#endif

