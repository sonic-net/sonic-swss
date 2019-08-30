#ifndef SWSS_UTIL_FLEX_COUNTER_UTILITIES_H
#define SWSS_UTIL_FLEX_COUNTER_UTILITIES_H

#include <string>
#include "producertable.h"

extern "C" {
#include "sai.h"
}

using std::string;
using swss::ProducerTable;

// flex_counter_utilities is a set of functions for interacting with syncd
// flex counters through FLEX_COUNTER_DB and CONFIG_DB. It allows you to
// perform basic flex counter management operations, such as:
//  - Setting up a new flex counter group
//  - Attaching counters to SAI objects
//  - Updating counter parameters, such as the polling interval
//
// These functions are stateless by design in order to be optimized for cases
// where the counter parameters are static (e.g. portstat counters). If you
// need to maintain counter state in SWSS, consider using a FlexCounterManager.
//
// TODO: There are several flex counter features missing from this library.
//  - Support for all types of counters
//  - Supporting the READ_AND_CLEAR stats mode
//  - Managing flex counter plugins
namespace flex_counter_utilities 
{
    enum class FlexCounterStatsMode 
    {
        READ
    };

    enum class FlexCounterType 
    {
        PORT_DEBUG,
        SWITCH_DEBUG
    };

    // Flex Counter Group Management Functions
    void createFlexCounterGroup(ProducerTable *flex_counter_group_table,
                                const string &group_name, 
                                const FlexCounterStatsMode stats_mode,
                                const int polling_interval);
    void deleteFlexCounterGroup(ProducerTable *flex_counter_group_table,
                                const string &group_name);

    void updatePollingInterval(ProducerTable *flex_counter_group_table,
                               const string &group_name, 
                               const int polling_interval);

    void enableFlexCounters(ProducerTable *flex_counter_group_table, 
                            const string &group_name);
    void disableFlexCounters(ProducerTable *flex_counter_group_table,
                             const string &group_name);

    // Flex Counter Management Functions
    void setCounterIdList(ProducerTable *flex_counter_table,
                          const string &group_name,
                          const FlexCounterType counter_type,
                          const sai_object_id_t object_id,
                          const string &counter_id_list);

    void removeFlexCounter(ProducerTable *flex_counter_table,
                           const string &group_name,
                           const sai_object_id_t object_id);
}

#endif // SWSS_UTIL_FLEX_COUNTER_UTILITIES_H