#ifndef DEBUG_COUNTER_ORCH_H
#define DEBUG_COUNTER_ORCH_H

#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>

#include "orch.h"
#include "flex_counter_manager.h"
#include "debug_counter.h"
#include "drop_counter.h"

extern "C" {
#include "sai.h"
}

#define DEBUG_COUNTER_FLEX_COUNTER_GROUP "DEBUG_COUNTER"

// DebugCounterOrch is an orchestrator for managing debug counters. It handles
// the creation, deletion, and modification of debug counters.
class DebugCounterOrch: public Orch
{
public:
    DebugCounterOrch(DBConnector *db, const vector<string> &table_names, int poll_interval);
    virtual ~DebugCounterOrch(void);

    void doTask(Consumer &consumer);

private:
    // Debug Capability Reporting Functions
    void publishDropCounterCapabilities();

    // doTask Handler Functions
    task_process_status installDebugCounter(const string &counter_name, const std::vector<FieldValueTuple> &attributes);
    task_process_status uninstallDebugCounter(const string &counter_name);
    task_process_status addDropReason(const string &counter_name, const string &drop_reason);
    task_process_status removeDropReason(const string &counter_name, const string &drop_reason);

    // Free Table Management Functions
    void addFreeCounter(const string &counter_name, const string &counter_type);
    void deleteFreeCounter(const string &counter_name);
    void addFreeDropReason(const string &counter_name, const string &drop_reason);
    void deleteFreeDropReason(const string &counter_name, const string &drop_reason);
    void reconcileFreeDropCounters(const string &counter_name);

    // Flex Counter Management Functions
    FlexCounterType getFlexCounterType(const string &counter_type);
    string getFlexCounterStats(const FlexCounterType counter_type);
    void installDebugFlexCounters(const string &counter_type, const string &counter_stat);
    void uninstallDebugFlexCounters(const string &counter_type, const string &counter_stat);

    // Debug Counter Initialization Helper Functions
    string getDebugCounterType(const std::vector<FieldValueTuple> &values) const;
    void createDropCounter(const string &counter_name, const string &counter_type, const unordered_set<string> &drop_reasons);

    // Debug Counter Configuration Helper Functions
    void parseDropReasonUpdate(const string &key, const char delimeter, string *counter_name, string *drop_reason) const;
    bool isDropReasonValid(const string &drop_reason) const;

    // Data Members
    shared_ptr<DBConnector> m_stateDb = nullptr;
    shared_ptr<Table> m_debugCapabilitiesTable = nullptr;

    shared_ptr<DBConnector> m_countersDb = nullptr;
    shared_ptr<Table> m_counterNameToPortStatMap = nullptr;
    shared_ptr<Table> m_counterNameToSwitchStatMap = nullptr;

    FlexCounterManager flex_counter_manager;

    unordered_map<string, unique_ptr<DebugCounter>> debug_counters;

    // free_drop_counters are drop counters that have been created by a user
    // that do not have any drop reasons associated with them yet. Because
    // we cannot create a drop counter without any drop reasons, we keep track
    // of these counters in this table.
    unordered_map<string, string> free_drop_counters;

    // free_drop_reasons are drop reasons that have been added by a user
    // that do not have a counter associated with them yet. Because we
    // cannot add drop reasons to a counter that doesn't exist yet,
    // we keep track of the reasons in this table.
    unordered_map<string, unordered_set<string>> free_drop_reasons;
};

#endif
