#include "flex_counter_stat_manager.h"

#include "schema.h"
#include "rediscommand.h"
#include "logger.h"
#include "sai_serialize.h"

using std::string;
using std::unordered_map;
using std::unordered_set;
using swss::FieldValueTuple;

FlexCounterStatManager::FlexCounterStatManager(
        const string &group_name,
        const StatsMode stats_mode,
        const int polling_interval) :
    FlexCounterManager(group_name, stats_mode, polling_interval)
{
    SWSS_LOG_ENTER();
}

FlexCounterStatManager::~FlexCounterStatManager()
{
    SWSS_LOG_ENTER();
}

// addFlexCounterStat will add a new stat for the given object to poll.
void FlexCounterStatManager::addFlexCounterStat(
        const sai_object_id_t object_id,
        const CounterType counter_type,
        const string &counter_stat)
{
    SWSS_LOG_ENTER();

    auto counter_stats = object_stats.find(object_id);
    if (counter_stats == object_stats.end())
    {
        unordered_set<string> new_stats = { counter_stat };
        counter_stats = object_stats.emplace(object_id, new_stats)->first;
    }
    else
    {
        counter_stats->second.emplace(counter_stat);
    }

    FlexCounterManager::setCounterIdList(object_id, counter_type, counter_stats->second);

    SWSS_LOG_DEBUG("Added flex stat '%s' to object '%s'", counter_stat.c_str(), sai_serialize_object_id(object_id).c_str());
}

// removeFlexCounterStat will remove a stat from the set of stats the given
// object are polling.
void FlexCounterStatManager::removeFlexCounterStat(
        const sai_object_id_t object_id,
        const CounterType counter_type,
        const string &counter_stat)
{
    SWSS_LOG_ENTER();

    auto counter_stats = object_stats.find(object_id);
    if (counter_stats == object_stats.end())
    {
        SWSS_LOG_WARN("Could not find flex stat '%s' on object '%s'",
                counter_stat.c_str(), sai_serialize_object_id(object_id).c_str());
        return;
    }

    counter_stats->second.erase(counter_stat);

    // If we don't have any stats left for this object, delete the flex
    // counter entirely.
    if (counter_stats->second.empty())
    {
        object_stats.erase(counter_stats);
        FlexCounterManager::clearCounterIdList(object_id);

        SWSS_LOG_DEBUG("Flex stat is empty, removing flex counter from object '%s'",
                sai_serialize_object_id(object_id).c_str());
        return;
    }

    FlexCounterManager::setCounterIdList(object_id, counter_type, counter_stats->second);

    SWSS_LOG_DEBUG("Removing flex stat '%s' from object '%s'",
            counter_stat.c_str(),
            sai_serialize_object_id(object_id).c_str());
}
