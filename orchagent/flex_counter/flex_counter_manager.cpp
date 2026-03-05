#include "flex_counter_manager.h"

#include <vector>

#include "schema.h"
#include "rediscommand.h"
#include "logger.h"

#include <macsecorch.h>

using std::shared_ptr;
using std::string;
using std::unordered_map;
using std::unordered_set;
using std::vector;
using swss::DBConnector;
using swss::FieldValueTuple;
using swss::ProducerTable;

extern sai_switch_api_t *sai_switch_api;

const string FLEX_COUNTER_ENABLE("enable");
const string FLEX_COUNTER_DISABLE("disable");

const unordered_map<StatsMode, string> FlexCounterManager::stats_mode_lookup =
{
    { StatsMode::READ, STATS_MODE_READ },
    { StatsMode::READ_AND_CLEAR, STATS_MODE_READ_AND_CLEAR },
};

const unordered_map<bool, string> FlexCounterManager::status_lookup =
{
    { false, FLEX_COUNTER_DISABLE },
    { true,  FLEX_COUNTER_ENABLE }
};

const unordered_map<CounterType, string> FlexCounterManager::counter_id_field_lookup =
{
    { CounterType::PORT_DEBUG,          PORT_DEBUG_COUNTER_ID_LIST },
    { CounterType::PORT_PHY_ATTR,       PORT_PHY_ATTR_ID_LIST },
    { CounterType::SWITCH_DEBUG,        SWITCH_DEBUG_COUNTER_ID_LIST },
    { CounterType::PORT,                PORT_COUNTER_ID_LIST },
    { CounterType::QUEUE,               QUEUE_COUNTER_ID_LIST },
    { CounterType::QUEUE_ATTR,          QUEUE_ATTR_ID_LIST },
    { CounterType::PRIORITY_GROUP,      PG_COUNTER_ID_LIST },
    { CounterType::MACSEC_SA_ATTR,      MACSEC_SA_ATTR_ID_LIST },
    { CounterType::MACSEC_SA,           MACSEC_SA_COUNTER_ID_LIST },
    { CounterType::MACSEC_FLOW,         MACSEC_FLOW_COUNTER_ID_LIST },
    { CounterType::ACL_COUNTER,         ACL_COUNTER_ATTR_ID_LIST },
    { CounterType::TUNNEL,              TUNNEL_COUNTER_ID_LIST },
    { CounterType::HOSTIF_TRAP,         FLOW_COUNTER_ID_LIST },
    { CounterType::ROUTE,               FLOW_COUNTER_ID_LIST },
    { CounterType::ENI,                 ENI_COUNTER_ID_LIST },
    { CounterType::DASH_METER,          DASH_METER_COUNTER_ID_LIST },
    { CounterType::SRV6,                SRV6_COUNTER_ID_LIST },
    { CounterType::SWITCH,              SWITCH_COUNTER_ID_LIST },
};

FlexManagerDirectory g_FlexManagerDirectory;

FlexCounterManager *FlexManagerDirectory::createFlexCounterManager(const string& group_name,
                                                                   const StatsMode stats_mode,
                                                                   const uint polling_interval,
                                                                   const bool enabled,
                                                                   FieldValueTuple fv_plugin)
{
    if (m_managers.find(group_name) != m_managers.end())
    {
        if (stats_mode != m_managers[group_name]->getStatsMode())
        {
            SWSS_LOG_ERROR("Stats mode mismatch with already created flex counter manager %s",
                          group_name.c_str());
            return NULL;
        }
        if (polling_interval != m_managers[group_name]->getPollingInterval())
        {
            SWSS_LOG_ERROR("Polling interval mismatch with already created flex counter manager %s",
                          group_name.c_str());
            return NULL;
        }
        if (enabled != m_managers[group_name]->getEnabled())
        {
            SWSS_LOG_ERROR("Enabled field mismatch with already created flex counter manager %s",
                          group_name.c_str());
            return NULL;
        }
        return m_managers[group_name];
    }
    FlexCounterManager *fc_manager = new FlexCounterManager(group_name, stats_mode, polling_interval,
                                                            enabled, fv_plugin);
    m_managers[group_name] = fc_manager;
    return fc_manager;
}

FlexCounterManager::FlexCounterManager(
        const string& group_name,
        const StatsMode stats_mode,
        const uint polling_interval,
        const bool enabled,
        FieldValueTuple fv_plugin) :
    FlexCounterManager(false, group_name, stats_mode,
            polling_interval, enabled, fv_plugin)
{
}

FlexCounterManager::FlexCounterManager(
        const bool is_gearbox,
        const string& group_name,
        const StatsMode stats_mode,
        const uint polling_interval,
        const bool enabled,
        FieldValueTuple fv_plugin) :
    group_name(group_name),
    stats_mode(stats_mode),
    polling_interval(polling_interval),
    enabled(enabled),
    fv_plugin(fv_plugin),
    is_gearbox(is_gearbox)
{
    SWSS_LOG_ENTER();

    applyGroupConfiguration();

    SWSS_LOG_DEBUG("Initialized flex counter group '%s'.", group_name.c_str());
}

FlexCounterManager::~FlexCounterManager()
{
    SWSS_LOG_ENTER();

    for (const auto& counter: installed_counters)
    {
        stopFlexCounterPolling(counter.second, getFlexCounterTableKey(group_name, counter.first));
    }

    delFlexCounterGroup(group_name, is_gearbox);

    SWSS_LOG_DEBUG("Deleted flex counter group '%s'.", group_name.c_str());
}

void FlexCounterManager::applyGroupConfiguration()
{
    SWSS_LOG_ENTER();

    setFlexCounterGroupParameter(group_name,
                                 std::to_string(polling_interval),
                                 stats_mode_lookup.at(stats_mode),
                                 fvField(fv_plugin),
                                 fvValue(fv_plugin),
                                 status_lookup.at(enabled),
                                 is_gearbox);
}

void FlexCounterManager::updateGroupPollingInterval(
        const uint polling_interval)
{
    SWSS_LOG_ENTER();

    setFlexCounterGroupPollInterval(group_name, std::to_string(polling_interval), is_gearbox);

    SWSS_LOG_DEBUG("Set polling interval for flex counter group '%s' to %d ms.",
            group_name.c_str(), polling_interval);
}

// enableFlexCounterGroup will do nothing if the flex counter group is already
// enabled.
void FlexCounterManager::enableFlexCounterGroup()
{
    SWSS_LOG_ENTER();

    if (enabled)
    {
        return;
    }

    setFlexCounterGroupOperation(group_name, FLEX_COUNTER_ENABLE, is_gearbox);
    enabled = true;

    SWSS_LOG_DEBUG("Enabling flex counters for group '%s'.",
            group_name.c_str());
}

// disableFlexCounterGroup will do nothing if the flex counter group has been
// disabled.
void FlexCounterManager::disableFlexCounterGroup()
{
    SWSS_LOG_ENTER();

    if (!enabled)
    {
        return;
    }

    setFlexCounterGroupOperation(group_name, FLEX_COUNTER_DISABLE, is_gearbox);
    enabled = false;

    SWSS_LOG_DEBUG("Disabling flex counters for group '%s'.",
            group_name.c_str());
}

// setCounterIdList configures a flex counter to poll the set of provided stats
// that are associated with the given object.
void FlexCounterManager::setCounterIdList(
        const sai_object_id_t object_id,
        const CounterType counter_type,
        const unordered_set<string>& counter_stats,
        const sai_object_id_t switch_id)
{
    SWSS_LOG_ENTER();

    auto counter_type_it = counter_id_field_lookup.find(counter_type);
    if (counter_type_it == counter_id_field_lookup.end())
    {
        SWSS_LOG_ERROR("Could not update flex counter id list for group '%s': counter type not found.",
                group_name.c_str());
        return;
    }

    auto key = getFlexCounterTableKey(group_name, object_id);
    auto counter_ids = serializeCounterStats(counter_stats);
    auto effective_switch_id = switch_id == SAI_NULL_OBJECT_ID ? gSwitchId : switch_id;

    startFlexCounterPolling(effective_switch_id, key, counter_ids, counter_type_it->second);
    installed_counters[object_id] = effective_switch_id;

    SWSS_LOG_DEBUG("Updated flex counter id list for object '%" PRIu64 "' in group '%s'.",
            object_id,
            group_name.c_str());
}

// clearCounterIdList clears all stats that are currently being polled from
// the given object.
void FlexCounterManager::clearCounterIdList(const sai_object_id_t object_id)
{
    SWSS_LOG_ENTER();

    auto counter_it = installed_counters.find(object_id);
    if (counter_it == installed_counters.end())
    {
        SWSS_LOG_INFO("No counters found on object '%" PRIu64 "' in group '%s'.",
                object_id,
                group_name.c_str());
        return;
    }

    auto key = getFlexCounterTableKey(group_name, object_id);
    stopFlexCounterPolling(installed_counters[object_id], key);
    installed_counters.erase(counter_it);

    SWSS_LOG_DEBUG("Cleared flex counter id list for object '%" PRIu64 "' in group '%s'.",
            object_id,
            group_name.c_str());
}

string FlexCounterManager::getFlexCounterTableKey(
        const string& group_name,
        const sai_object_id_t object_id) const
{
    SWSS_LOG_ENTER();

    return group_name + ":" + sai_serialize_object_id(object_id);
}

// serializeCounterStats turns a set of stats into a format suitable for FLEX_COUNTER_DB.
string FlexCounterManager::serializeCounterStats(
        const unordered_set<string>& counter_stats)
{
    SWSS_LOG_ENTER();

    string stats_string;
    for (const auto& stat : counter_stats)
    {
        stats_string.append(stat);
        stats_string.append(",");
    }

    if (!stats_string.empty())
    {
        // Fence post: remove the trailing comma
        stats_string.pop_back();
    }

    return stats_string;
}
