#ifndef ORCHAGENT_FLEX_COUNTER_STAT_MANAGER_H
#define ORCHAGENT_FLEX_COUNTER_STAT_MANAGER_H

#include "orchagent/flex_counter/flex_counter_manager.h"

// FlexCounterStatManager allows users to manage a group of flex counters
// where the objects have highly variable sets of stats to track.
class FlexCounterStatManager : public FlexCounterManager
{
    public:
        FlexCounterStatManager(
                const std::string& group_name,
                const StatsMode stats_mode,
                const int polling_interval,
                const bool enabled);

        FlexCounterStatManager(const FlexCounterStatManager&) = delete;
        FlexCounterStatManager& operator=(const FlexCounterStatManager&) = delete;
        ~FlexCounterStatManager();

        void addFlexCounterStat(
                const sai_object_id_t object_id,
                const CounterType counter_type,
                const std::string& counter_stat);
        void removeFlexCounterStat(
                const sai_object_id_t object_id,
                const CounterType counter_type,
                const std::string& counter_stat);

    private:
        std::unordered_map<sai_object_id_t, std::unordered_set<std::string>> object_stats;
};

#endif // ORCHAGENT_FLEX_COUNTER_STAT_MANAGER_H
