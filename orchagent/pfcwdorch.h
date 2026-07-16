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

// ============================================================================
// Global macros used across base and derived classes
// ============================================================================

// State and configuration table identifiers
#define PFC_WD_FLEX_COUNTER_GROUP       "PFC_WD"
#define PFC_WD_GLOBAL                   "GLOBAL"

#define PFC_WD_ACTION                   "action"
#define PFC_WD_DETECTION_TIME           "detection_time"
#define PFC_WD_RESTORATION_TIME         "restoration_time"
#define PFC_STAT_HISTORY                "pfc_stat_history"

// Timer limits in milliseconds
#define PFC_WD_DETECTION_TIME_MAX       (5 * 1000)
#define PFC_WD_DETECTION_TIME_MIN       100
#define PFC_WD_RESTORATION_TIME_MAX     (60 * 1000)
#define PFC_WD_RESTORATION_TIME_MIN     100

enum class PfcWdAction
{
    PFC_WD_ACTION_UNKNOWN,
    PFC_WD_ACTION_FORWARD,
    PFC_WD_ACTION_DROP,
    PFC_WD_ACTION_ALERT,
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

    static PfcWdAction deserializeAction(const string& key);
    static string serializeAction(const PfcWdAction &action);

    virtual task_process_status createEntry(const string& key, const vector<FieldValueTuple>& data);
    task_process_status deleteEntry(const string& name);
    PfcWdAction getPfcDlrPacketAction() { return m_pfcDlrPacketAction; }
    void setPfcDlrPacketAction(PfcWdAction action) { m_pfcDlrPacketAction = action; }

protected:
    virtual bool startWdActionOnQueue(const string &event, sai_object_id_t queueId, const string &info="") = 0;

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

    // Helper to convert counter IDs to string set for FlexCounter
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

    string m_platform = "";
    shared_ptr<FlexCounterTaggedCachedManager<sai_object_type_t>> m_pfcwdFlexCounterManager;

private:

    shared_ptr<DBConnector> m_countersDb = nullptr;
    shared_ptr<Table> m_countersTable = nullptr;
    PfcWdAction m_pfcDlrPacketAction = PfcWdAction::PFC_WD_ACTION_UNKNOWN;
    std::set<std::string> m_pfcwd_ports;
};

#endif
