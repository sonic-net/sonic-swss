#ifndef DEBUG_COUNTER_ORCH_H
#define DEBUG_COUNTER_ORCH_H

#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>

#include "orch.h"
#include "flex_counter_stat_manager.h"
#include "debug_counter.h"
#include "drop_counter.h"
#include "observer.h"

extern "C" {
#include "sai.h"
}

#define DEBUG_COUNTER_FLEX_COUNTER_GROUP "DEBUG_COUNTER"

using DebugCounterMap = std::unordered_map<std::string, std::unique_ptr<DebugCounter>>;

// DebugCounterOrch is an orchestrator for managing debug counters. It handles
// the creation, deletion, and modification of debug counters.
class DebugCounterOrch: public Orch, public Observer
{
public:
    DebugCounterOrch(swss::DBConnector *db, const std::vector<std::string>& table_names, int poll_interval);
    virtual ~DebugCounterOrch(void);

    void doTask(Consumer& consumer);

    void update(SubjectType, void *cntx);
private:
    // Debug Capability Reporting Functions
    void publishDropCounterCapabilities();

    // doTask Handler Functions
    task_process_status installDebugCounter(const std::string& counter_name, const std::vector<swss::FieldValueTuple>& attributes);
    task_process_status uninstallDebugCounter(const std::string& counter_name);
    task_process_status addDropReason(const std::string& counter_name, const std::string& drop_reason);
    task_process_status removeDropReason(const std::string& counter_name, const std::string& drop_reason);

    // Free Table Management Functions
    void addFreeCounter(const std::string& counter_name, const std::string& counter_type);
    void deleteFreeCounter(const std::string& counter_name);
    void addFreeDropReason(const std::string& counter_name, const std::string& drop_reason);
    void deleteFreeDropReason(const std::string& counter_name, const std::string& drop_reason);
    void reconcileFreeDropCounters(const std::string& counter_name);

    // Flex Counter Management Functions
    CounterType getFlexCounterType(const std::string& counter_type) noexcept(false);
    void installDebugFlexCounters(
            const std::string& counter_type,
            const std::string& counter_stat,
            sai_object_id_t port_id = SAI_NULL_OBJECT_ID);
    void uninstallDebugFlexCounters(
            const std::string& counter_type,
            const std::string& counter_stat,
            sai_object_id_t port_id = SAI_NULL_OBJECT_ID);

    // Debug Counter Initialization Helper Functions
    std::string getDebugCounterType(
            const std::vector<swss::FieldValueTuple>& values) const noexcept(false);
    void createDropCounter(
            const std::string& counter_name,
            const std::string& counter_type,
            const std::unordered_set<std::string>& drop_reasons) noexcept(false);

    // Debug Counter Configuration Helper Functions
    void parseDropReasonUpdate(
            const std::string& key,
            const char delimeter,
            std::string *counter_name,
            std::string *drop_reason) const;
    bool isDropReasonValid(const std::string& drop_reason) const;

    // Data Members
    std::shared_ptr<swss::DBConnector> m_stateDb = nullptr;
    std::shared_ptr<swss::Table> m_debugCapabilitiesTable = nullptr;

    std::shared_ptr<swss::DBConnector> m_countersDb = nullptr;
    std::shared_ptr<swss::Table> m_counterNameToPortStatMap = nullptr;
    std::shared_ptr<swss::Table> m_counterNameToSwitchStatMap = nullptr;

    std::unordered_set<std::string> supported_counter_types;
    std::unordered_set<std::string> supported_ingress_drop_reasons;
    std::unordered_set<std::string> supported_egress_drop_reasons;

    FlexCounterStatManager flex_counter_manager;

    DebugCounterMap debug_counters;

    // free_drop_counters are drop counters that have been created by a user
    // that do not have any drop reasons associated with them yet. Because
    // we cannot create a drop counter without any drop reasons, we keep track
    // of these counters in this table.
    std::unordered_map<std::string, std::string> free_drop_counters;

    // free_drop_reasons are drop reasons that have been added by a user
    // that do not have a counter associated with them yet. Because we
    // cannot add drop reasons to a counter that doesn't exist yet,
    // we keep track of the reasons in this table.
    std::unordered_map<std::string, std::unordered_set<std::string>> free_drop_reasons;
};

#endif
