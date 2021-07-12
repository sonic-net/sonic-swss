#ifndef ORCHAGENT_FLEX_COUNTER_MANAGER_H
#define ORCHAGENT_FLEX_COUNTER_MANAGER_H

#include <string>
#include <unordered_set>
#include <unordered_map>
#include "dbconnector.h"
#include "producertable.h"
#include <inttypes.h>

extern "C" {
#include "sai.h"
}

enum class StatsMode
{
    READ
};

enum class CounterType
{
    PORT,
    QUEUE,
    PORT_DEBUG,
    SWITCH_DEBUG,
    MACSEC_SA_ATTR,
    TUNNEL,
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
                const uint polling_interval,
                const bool enabled);

        FlexCounterManager() 
        {}

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

        std::string getGroupName()
        {
            return group_name;
        }

        StatsMode getStatsMode()
        {
            return stats_mode;
        }

        uint getPollingInterval()
        {
            return polling_interval;
        }

        bool getEnabled()
        {
            return enabled;
        }

    protected:
        void applyGroupConfiguration();

    private:
        std::string getFlexCounterTableKey(
                const std::string& group_name,
                const sai_object_id_t object_id) const;
        std::string serializeCounterStats(
                const std::unordered_set<std::string>& counter_stats) const;

        std::string group_name;
        StatsMode stats_mode;
        uint polling_interval;
        bool enabled;
        std::unordered_set<sai_object_id_t> installed_counters;

        std::shared_ptr<swss::DBConnector> flex_counter_db = nullptr;
        std::shared_ptr<swss::ProducerTable> flex_counter_group_table = nullptr;
        std::shared_ptr<swss::ProducerTable> flex_counter_table = nullptr;

        static const std::unordered_map<StatsMode, std::string> stats_mode_lookup;
        static const std::unordered_map<bool, std::string> status_lookup;
        static const std::unordered_map<CounterType, std::string> counter_id_field_lookup;
};

class FlexManagerDirectory
{
    public:
        FlexCounterManager* createFlexCounterManager(const std::string& group_name, const StatsMode stats_mode,
                                                     const uint polling_interval, const bool enabled);
    private:
        std::unordered_map<std::string, FlexCounterManager*>  m_managers;
};

#endif // ORCHAGENT_FLEX_COUNTER_MANAGER_H
