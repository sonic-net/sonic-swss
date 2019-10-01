#ifndef SWSS_UTIL_I_FLEX_COUNTER_MANAGER_H
#define SWSS_UTIL_I_FLEX_COUNTER_MANAGER_H

#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include "flex_counter_utilities.h"
#include "dbconnector.h"
#include "producertable.h"

extern "C" {
#include "sai.h"
}

using std::string;
using std::vector;
using std::unordered_map;
using std::unordered_set;
using std::shared_ptr;

using swss::DBConnector;
using swss::ProducerTable;

using flex_counter_utilities::FlexCounterStatsMode;
using flex_counter_utilities::FlexCounterType;

// FlexCounterManager manages the configuration of a group of flex counters.
// It maintains the state of the associated flex counter group as well as
// the addition and removal of stats from configured flex counters.
class FlexCounterManager
{
    public:
        FlexCounterManager(const string &group_name,
                           const FlexCounterStatsMode stats_mode,
                           const int polling_interval);
        FlexCounterManager(const FlexCounterManager&) = delete;
        FlexCounterManager& operator=(const FlexCounterManager&) = delete;
        ~FlexCounterManager();

        void addFlexCounterStat(const FlexCounterType counter_type,
                                const vector<sai_object_id_t> &object_ids, 
                                const string &counter_stat);      
        void removeFlexCounterStat(const FlexCounterType counter_type,
                                   const vector<sai_object_id_t> &object_ids,
                                   const string &counter_stat);

    private:
        string serializeCounterStats(const unordered_set<string> &counter_stats);

        string group_name;
        FlexCounterStatsMode stats_mode;
        int polling_interval;
        bool status;

        unordered_map<sai_object_id_t, unordered_set<string>> object_stats;

        shared_ptr<DBConnector> flex_counter_db = nullptr;
        shared_ptr<ProducerTable> flex_counter_group_table = nullptr;
        shared_ptr<ProducerTable> flex_counter_table = nullptr;
};

#endif
