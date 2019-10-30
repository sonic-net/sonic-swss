#ifndef SWSS_UTIL_DYNAMIC_FLEX_COUNTER_MANAGER_H
#define SWSS_UTIL_DYNAMIC_FLEX_COUNTER_MANAGER_H

#include "flex_counter_manager.h"

// DynamicFlexCounterManager allows users to manage a group of flex counters
// where the objects have highly variable sets of stats to track.
class DynamicFlexCounterManager : public FlexCounterManager
{
    public:
        DynamicFlexCounterManager(
                const std::string &group_name,
                const StatsMode stats_mode,
                const int polling_interval);

        DynamicFlexCounterManager(const DynamicFlexCounterManager&) = delete;
        DynamicFlexCounterManager& operator=(const DynamicFlexCounterManager&) = delete;
        ~DynamicFlexCounterManager();

        void addFlexCounterStat(
                const sai_object_id_t object_id,
                const CounterType counter_type,
                const std::string &counter_stat);      
        void removeFlexCounterStat(
                const sai_object_id_t object_id,
                const CounterType counter_type,
                const std::string &counter_stat);

    private:
        std::unordered_map<sai_object_id_t, std::unordered_set<std::string>> object_stats;
};

#endif // SWSS_UTIL_DYNAMIC_FLEX_COUNTER_MANAGER_H
