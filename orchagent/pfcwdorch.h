#ifndef PFC_WATCHDOG_H
#define PFC_WATCHDOG_H

#include "orch.h"
#include "port.h"
#include "pfcactionhandler.h"
#include "producertable.h"
#include "notificationconsumer.h"
#include "timer.h"
#include "events.h"

extern "C" {
#include "sai.h"
}

#define PFC_WD_FLEX_COUNTER_GROUP       "PFC_WD"

const string pfc_wd_flex_counter_group = PFC_WD_FLEX_COUNTER_GROUP;

enum class PfcWdAction
{
    PFC_WD_ACTION_UNKNOWN,
    PFC_WD_ACTION_FORWARD,
    PFC_WD_ACTION_DROP,
    PFC_WD_ACTION_ALERT,
};

static const map<string, sai_packet_action_t> packet_action_map = {
    {"drop", SAI_PACKET_ACTION_DROP},
    {"forward", SAI_PACKET_ACTION_FORWARD},
    {"alert", SAI_PACKET_ACTION_FORWARD}
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

    virtual task_process_status createEntry(const string& key, const vector<FieldValueTuple>& data);
    task_process_status deleteEntry(const string& name);
    PfcWdAction getPfcDlrPacketAction() { return PfcDlrPacketAction; }
    void setPfcDlrPacketAction(PfcWdAction action) { PfcDlrPacketAction = action; }

protected:
    virtual bool startWdActionOnQueue(const string &event, sai_object_id_t queueId, const string &info="") = 0;
    string m_platform = "";
private:

    shared_ptr<DBConnector> m_countersDb = nullptr;
    shared_ptr<Table> m_countersTable = nullptr;
    PfcWdAction PfcDlrPacketAction = PfcWdAction::PFC_WD_ACTION_UNKNOWN;
    std::set<std::string> m_pfcwd_ports;
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

    void doTask(Consumer& consumer) override;
    virtual bool startWdOnPort(const Port& port,
            uint32_t detectionTime, uint32_t restorationTime, PfcWdAction action);
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

    template <typename T>
    static string counterIdsToStr(const vector<T> ids, string (*convert)(T));
    bool registerInWdDb(const Port& port,
            uint32_t detectionTime, uint32_t restorationTime, PfcWdAction action);
    void unregisterFromWdDb(const Port& port);
    void doTask(swss::NotificationConsumer &wdNotification);

    string filterPfcCounters(string counters, set<uint8_t>& losslessTc);
    string getFlexCounterTableKey(string s);

    void disableBigRedSwitchMode();
    void enableBigRedSwitchMode();
    void setBigRedSwitchMode(string value);

    void report_pfc_storm(sai_object_id_t id, const PfcWdQueueEntry *, const string&);

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
