#ifndef PFC_WATCHDOG_H
#define PFC_WATCHDOG_H

#include "orch.h"
#include "port.h"
#include "pfcactionhandler.h"
#include "producertable.h"
#include "notificationconsumer.h"
#include "timer.h"
#include "events.h"
#include "table.h"

extern "C" {
#include "sai.h"
}

// ============================================================================
// Global macros used across base and derived classes (PfcWdBaseOrch,
// PfcWdSwOrch, PfcWdHwOrch)
// ============================================================================

// State and configuration table identifiers
#define PFC_WD_FLEX_COUNTER_GROUP       "PFC_WD"
#define PFC_WD_GLOBAL                   "GLOBAL"
#define PFC_WD_RECOVERY_MECHANISM       "RECOVERY_MECHANISM"
#define PFC_WD_RECOVERY_SOFTWARE        "SOFTWARE"
#define PFC_WD_RECOVERY_HARDWARE        "HARDWARE"
#define PFC_WD_DLR_PACKET_ACTION        "DLR_PACKET_ACTION"
#define PFC_WD_TC_MAX                   8

#define PFC_WD_ACTION                   "action"
#define PFC_WD_DETECTION_TIME           "detection_time"
#define PFC_WD_RESTORATION_TIME         "restoration_time"
#define PFC_STAT_HISTORY                "pfc_stat_history"

// Timer limits in milliseconds
#define PFC_WD_DETECTION_TIME_MAX       (5 * 1000)
#define PFC_WD_DETECTION_TIME_MIN       100
#define PFC_WD_RESTORATION_TIME_MAX     (60 * 1000)
#define PFC_WD_RESTORATION_TIME_MIN     100

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

class PfcWdBaseOrch: public Orch
{
public:
    PfcWdBaseOrch(DBConnector *db, vector<string> &tableNames);
    virtual ~PfcWdBaseOrch(void);

    virtual void doTask(Consumer& consumer);
    virtual bool startWdOnPort(const Port& port,
            uint32_t detectionTime, uint32_t restorationTime, PfcWdAction action, string pfcStatHistory) = 0;
    virtual bool stopWdOnPort(const Port& port) = 0;

    shared_ptr<Table> getCountersTable(void)
    {
        return m_countersTable;
    }

    shared_ptr<DBConnector> getCountersDb(void)
    {
        return m_countersDb;
    }

    shared_ptr<Table> getStateTable(void)
    {
        return m_stateTable;
    }

    shared_ptr<DBConnector> getStateDb(void)
    {
        return m_stateDb;
    }

    static PfcWdAction deserializeAction(const string& key);
    static string serializeAction(const PfcWdAction &action);

    virtual task_process_status createEntry(const string& key, const vector<FieldValueTuple>& data);
    virtual task_process_status deleteEntry(const string& name);
    PfcWdAction getPfcDlrPacketAction() { return m_pfcDlrPacketAction; }
    void setPfcDlrPacketAction(PfcWdAction action) { m_pfcDlrPacketAction = action; }

protected:
    virtual bool startWdActionOnQueue(const string &event, sai_object_id_t queueId, const string &info="") = 0;

    void updateStateTable(const string &field, const string &value)
    {
        string key = m_stateTable->getTableName() + m_stateTable->getTableNameSeparator() + "PFC_WD";
        m_stateDb->hset(key, field, value);
    }

    void updateDlrPacketActionInStateTable()
    {
        string dlrAction = PfcWdBaseOrch::serializeAction(this->getPfcDlrPacketAction());
        this->updateStateTable(PFC_WD_DLR_PACKET_ACTION, dlrAction);
    }

    // ========================================================================
    // Helper functions used in both SW and HW watchdog implementations
    // ========================================================================

    // Get lossless TCs for a port based on PFC mask
    bool getLosslessTcsForPort(const Port& port, set<uint8_t>& losslessTc);

    // Report PFC storm event
    void report_pfc_storm(sai_object_id_t queueId, sai_object_id_t portId,
                         uint8_t queueIndex, const string& portAlias,
                         const string& info = "");

    // Report PFC storm restored event
    void report_pfc_restored(sai_object_id_t queueId, sai_object_id_t portId,
                            uint8_t queueIndex, const string& portAlias);

    string m_platform = "";
    shared_ptr<FlexCounterTaggedCachedManager<sai_object_type_t>> m_pfcwdFlexCounterManager;

    // Convert counter IDs to string set for FlexCounter
    template <typename T>
    static unordered_set<string> counterIdsToStr(const vector<T> ids, string (*convert)(T))
    {
        unordered_set<string> counterIdSet;
        for (const auto& i: ids)
        {
            counterIdSet.emplace(convert(i));
        }
        return counterIdSet;
    }

private:

    shared_ptr<DBConnector> m_countersDb = nullptr;
    shared_ptr<Table> m_countersTable = nullptr;
    shared_ptr<DBConnector> m_stateDb = nullptr;
    shared_ptr<Table> m_stateTable = nullptr;
    PfcWdAction m_pfcDlrPacketAction = PfcWdAction::PFC_WD_ACTION_UNKNOWN;
    std::set<std::string> m_pfcwd_ports;
};

#endif
