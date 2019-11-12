#ifndef ORCHAGENT_FLEX_COUNTER_MANAGER_H
#define ORCHAGENT_FLEX_COUNTER_MANAGER_H

#include <string>
#include <unordered_set>
#include <unordered_map>
#include "dbconnector.h"
#include "producertable.h"

extern "C" {
#include "sai.h"
}

enum class StatsMode
{
    READ
};

enum class CounterType
{
    PORT_DEBUG,
    SWITCH_DEBUG
};

// FlexCounterManager allows users to manage a group of flex counters.
//
// TODO: FlexCounterManager doesn't currently support the full range of
// flex counter features. In particular, support for standard (i.e. non-debug)
// counters and support for plugins needs to be added.
class FlexCounterManager
{
    public:
        FlexCounterManager(
                const std::string& group_name,
                const StatsMode stats_mode,
                const uint polling_interval);

        FlexCounterManager(const FlexCounterManager&) = delete;
        FlexCounterManager& operator=(const FlexCounterManager&) = delete;
        virtual ~FlexCounterManager();

        void updateGroupPollingInterval(const uint polling_interval);
        void enableFlexCounterGroup();
        void disableFlexCounterGroup();

        void setCounterIdList(
                const sai_object_id_t object_id,
                const CounterType counter_type,
                const std::unordered_set<std::string>& counter_stats);
        void clearCounterIdList(const sai_object_id_t object_id);

    private:
        std::string getFlexCounterTableKey(
                const std::string& group_name,
                const sai_object_id_t object_id) const;
        std::string serializeCounterStats(
                const std::unordered_set<std::string>& counter_stats) const;

        std::string group_name;
        bool enabled;
        std::unordered_set<sai_object_id_t> installed_counters;

        std::shared_ptr<swss::DBConnector> flex_counter_db = nullptr;
        std::shared_ptr<swss::ProducerTable> flex_counter_group_table = nullptr;
        std::shared_ptr<swss::ProducerTable> flex_counter_table = nullptr;

        static const std::unordered_map<StatsMode, std::string> stats_mode_lookup;
        static const std::unordered_map<CounterType, std::string> counter_id_field_lookup;
};

#endif // ORCHAGENT_FLEX_COUNTER_MANAGER_H
