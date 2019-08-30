#include "flex_counter_utilities.h"

#include <unordered_map>
#include <string>
#include <vector>
#include "schema.h"
#include "rediscommand.h"
#include "logger.h"
#include "sai_serialize.h"

using namespace flex_counter_utilities;

using std::unordered_map;
using std::string;
using std::vector;
using swss::ProducerTable;
using swss::FieldValueTuple;

static unordered_map<FlexCounterStatsMode, std::string> stats_mode_lookup = 
{
    { FlexCounterStatsMode::READ, STATS_MODE_READ },
};

static unordered_map<FlexCounterType, std::string> counter_id_field_lookup =
{
    { FlexCounterType::PORT_DEBUG,   PORT_DEBUG_COUNTER_ID_LIST },
    { FlexCounterType::SWITCH_DEBUG, SWITCH_DEBUG_COUNTER_ID_LIST },
};

static string getFlexCounterTableKey(const string &group_name, 
                                     const sai_object_id_t object_id);

void flex_counter_utilities::createFlexCounterGroup(ProducerTable *flex_counter_group_table,
                                                    const string &group_name, 
                                                    const FlexCounterStatsMode stats_mode,
                                                    const int polling_interval)
{
    SWSS_LOG_ENTER();
    vector<FieldValueTuple> field_values;
    field_values.emplace_back(STATS_MODE_FIELD, stats_mode_lookup[stats_mode]);
    field_values.emplace_back(POLL_INTERVAL_FIELD, std::to_string(polling_interval));
    flex_counter_group_table->set(group_name, field_values);
}

void flex_counter_utilities::deleteFlexCounterGroup(ProducerTable *flex_counter_group_table,
                                                    const string &group_name)
{
    SWSS_LOG_ENTER();
    flex_counter_group_table->del(group_name);
}

void flex_counter_utilities::updatePollingInterval(ProducerTable *flex_counter_group_table,
                                                   const string &group_name,
                                                   const int polling_interval) 
{
    SWSS_LOG_ENTER();
    vector<FieldValueTuple> field_values;
    field_values.emplace_back(POLL_INTERVAL_FIELD, std::to_string(polling_interval));
    flex_counter_group_table->set(group_name, field_values);
}

void flex_counter_utilities::enableFlexCounters(ProducerTable *flex_counter_group_table, 
                                                const string &group_name)
{
    SWSS_LOG_ENTER();
    vector<FieldValueTuple> field_values;
    field_values.emplace_back(FLEX_COUNTER_STATUS_FIELD, "enable");
    flex_counter_group_table->set(group_name, field_values);
}

void flex_counter_utilities::disableFlexCounters(ProducerTable *flex_counter_group_table, 
                                                 const string &group_name)
{
    SWSS_LOG_ENTER();
    vector<FieldValueTuple> field_values;
    field_values.emplace_back(FLEX_COUNTER_STATUS_FIELD, "disable");
    flex_counter_group_table->set(group_name, field_values);
}

void flex_counter_utilities::setCounterIdList(ProducerTable *flex_counter_table,
                                              const string &group_name,
                                              const FlexCounterType counter_type,
                                              const sai_object_id_t object_id,
                                              const string &counter_id_list)
{
    SWSS_LOG_ENTER();
    std::vector<swss::FieldValueTuple> field_values;

    auto counter_id_field = counter_id_field_lookup.find(counter_type);
    if (counter_id_field == counter_id_field_lookup.end())
    {
        SWSS_LOG_ERROR("Could not update flex counter id list: specified counter type does not support list of counter ids");
        return;
    }

    field_values.emplace_back(counter_id_field->second, counter_id_list);
    flex_counter_table->set(getFlexCounterTableKey(group_name, object_id), field_values);
}

void flex_counter_utilities::removeFlexCounter(ProducerTable *flex_counter_table,
                                               const string &group_name,
                                               const sai_object_id_t object_id)
{
    SWSS_LOG_ENTER();
    flex_counter_table->del(getFlexCounterTableKey(group_name, object_id));
}

string getFlexCounterTableKey(const string &group_name, const sai_object_id_t object_id)
{
    return group_name + ':' + sai_serialize_object_id(object_id);
}