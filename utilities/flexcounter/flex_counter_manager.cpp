#include "flex_counter_manager.h"

#include <map>
#include "logger.h"
#include "schema.h"
#include "sai_serialize.h"
#include "rediscommand.h"

using namespace flex_counter_utilities;

// FlexCounterManager initializes a new FlexCounterManager object. It will handle
// the initial setup for the flex counter group that this object is managing.
FlexCounterManager::FlexCounterManager(const string &group_name,
                                       const FlexCounterStatsMode stats_mode,
                                       const int polling_interval) :
    group_name(group_name),
    stats_mode(stats_mode),
    polling_interval(polling_interval),
    flex_counter_db(new DBConnector(FLEX_COUNTER_DB, swss::DBConnector::DEFAULT_UNIXSOCKET, 0)),
    flex_counter_group_table(new ProducerTable(flex_counter_db.get(), FLEX_COUNTER_GROUP_TABLE)),
    flex_counter_table(new ProducerTable(flex_counter_db.get(), FLEX_COUNTER_TABLE))
{
    SWSS_LOG_ENTER();
    createFlexCounterGroup(flex_counter_group_table.get(), group_name,
            stats_mode, polling_interval);
}

// ~FlexCounterManager tears down this FlexCounterManager. It will handle the
// removal of the flex counter group this object is managing.
FlexCounterManager::~FlexCounterManager()
{
    SWSS_LOG_ENTER();
    deleteFlexCounterGroup(flex_counter_group_table.get(), group_name);
}

// addFlexCounterStat will add a new stat for the given objects to poll.
void FlexCounterManager::addFlexCounterStat(const FlexCounterType counter_type,
                                            const vector<sai_object_id_t> &object_ids, 
                                            const string &counter_stat)
{
    SWSS_LOG_ENTER();

    for (const auto &object_id: object_ids)
    {
        auto counter_stats = object_stats.find(object_id);
        if (counter_stats == object_stats.end())
        {
            unordered_set<string> new_stats = { counter_stat };
            object_stats.emplace(std::make_pair(object_id, new_stats));
            counter_stats = object_stats.find(object_id);
        }
        else
        {
            counter_stats->second.emplace(counter_stat);
        }

        string flex_counter_stats = serializeCounterStats(counter_stats->second);
        setCounterIdList(flex_counter_table.get(), group_name, counter_type, 
                object_id, flex_counter_stats);
    }
}

// removeFlexCounterStat will remove a stat from list of stats the given objects
// are polling.
void FlexCounterManager::removeFlexCounterStat(const FlexCounterType counter_type,
                                               const vector<sai_object_id_t> &object_ids,
                                               const string &counter_stat)
{
    SWSS_LOG_ENTER();

    for (const auto &object_id: object_ids)
    {
        auto counter_stats = object_stats.find(object_id);
        if (counter_stats == object_stats.end()) 
        {
            // TODO: add debug log
            continue;
        }

        counter_stats->second.erase(counter_stat);

        // If we don't have any stats left for this object, delete the flex
        // counter entirely.
        if (counter_stats->second.empty())
        {
            // TODO: add debug log
            object_stats.erase(counter_stats);
            removeFlexCounter(flex_counter_table.get(), group_name, object_id);
            continue;
        }

        string flex_counter_stats = serializeCounterStats(counter_stats->second);
        setCounterIdList(flex_counter_table.get(), group_name, counter_type, 
                object_id, flex_counter_stats);
    } 
}

// serializeCounterStats turns a list of stats into a format suitable for FLEX_COUNTER_DB.
string FlexCounterManager::serializeCounterStats(const unordered_set<string> &counter_stats)
{
    SWSS_LOG_ENTER();
    string stats_string;
    for (const auto &stat : counter_stats) 
    {
        stats_string.append(",");
        stats_string.append(stat);
    }
    return stats_string.substr(1);
}