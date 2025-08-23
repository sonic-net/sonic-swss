#ifndef ORCHAGENT_FLEX_COUNTER_MANAGER_H
#define ORCHAGENT_FLEX_COUNTER_MANAGER_H

#include <string>
#include <unordered_set>
#include <unordered_map>
#include <utility>
#include "dbconnector.h"
#include "producertable.h"
#include "table.h"
#include <inttypes.h>
#include <type_traits>
#include "sai_serialize.h"
#include "saihelper.h"

extern "C" {
#include "sai.h"
}

enum class StatsMode
{
    READ,
    READ_AND_CLEAR
};

enum class CounterType
{
    PORT,
    QUEUE,
    QUEUE_ATTR,
    PRIORITY_GROUP,
    PORT_DEBUG,
    SWITCH_DEBUG,
    MACSEC_SA_ATTR,
    MACSEC_SA,
    MACSEC_FLOW,
    ACL_COUNTER,
    TUNNEL,
    HOSTIF_TRAP,
    ROUTE,
    ENI,
    SRV6
};

extern bool gTraditionalFlexCounter;
extern sai_object_id_t gSwitchId;

struct CachedObjects;
// FlexCounterManager allows users to manage a group of flex counters.
//
// TODO: FlexCounterManager doesn't currently support the full range of
// flex counter features. In particular, support for standard (i.e. non-debug)
// counters and support for plugins needs to be added.
class FlexCounterManager
{
    friend struct CachedObjects;
    public:
        FlexCounterManager(
                const std::string& group_name,
                const StatsMode stats_mode,
                const uint polling_interval,
                const bool enabled,
                swss::FieldValueTuple fv_plugin = std::make_pair("",""));

        FlexCounterManager()
        {}

        FlexCounterManager(
                const bool is_gearbox,
                const std::string& group_name,
                const StatsMode stats_mode,
                const uint polling_interval,
                const bool enabled,
                swss::FieldValueTuple fv_plugin = std::make_pair("",""));

        FlexCounterManager(const FlexCounterManager&) = delete;
        FlexCounterManager& operator=(const FlexCounterManager&) = delete;
        virtual ~FlexCounterManager();

        void updateGroupPollingInterval(const uint polling_interval);
        void enableFlexCounterGroup();
        void disableFlexCounterGroup();

        virtual void setCounterIdList(
                const sai_object_id_t object_id,
                const CounterType counter_type,
                const std::unordered_set<std::string>& counter_stats,
                const sai_object_id_t switch_id=SAI_NULL_OBJECT_ID);
        virtual void clearCounterIdList(const sai_object_id_t object_id);

        const std::string& getGroupName() const
        {
            return group_name;
        }

        const StatsMode& getStatsMode() const
        {
            return stats_mode;
        }

        const uint& getPollingInterval() const
        {
            return polling_interval;
        }

        const bool& getEnabled() const
        {
            return enabled;
        }

    protected:
        void applyGroupConfiguration();

        std::string getFlexCounterTableKey(
                const std::string& group_name,
                const sai_object_id_t object_id) const;

        std::string group_name;
        StatsMode stats_mode;
        uint polling_interval;
        bool enabled;
        swss::FieldValueTuple fv_plugin;
        std::unordered_map<sai_object_id_t, sai_object_id_t> installed_counters;
        bool is_gearbox;

        static std::string serializeCounterStats(
                const std::unordered_set<std::string>& counter_stats);

        static const std::unordered_map<StatsMode, std::string> stats_mode_lookup;
        static const std::unordered_map<bool, std::string> status_lookup;
        static const std::unordered_map<CounterType, std::string> counter_id_field_lookup;
};

struct CachedObjects
{
    CounterType pending_counter_type;
    sai_object_id_t pending_switch_id;
    std::unordered_set<std::string> pending_counter_stats;
    std::unordered_set<sai_object_id_t> pending_sai_objects;

    bool try_cache(const sai_object_id_t object_id,
                   const CounterType counter_type,
                   const std::unordered_set<std::string>& counter_stats,
                   sai_object_id_t switch_id)
    {
        if (pending_sai_objects.empty())
        {
            pending_counter_type = counter_type;
            pending_switch_id = switch_id;
            // Just to avoid recreating counter IDs
            if (pending_counter_stats != counter_stats)
            {
                pending_counter_stats = counter_stats;
            }
        }
        else if (counter_type != pending_counter_type ||
                 switch_id != pending_switch_id ||
                 counter_stats != pending_counter_stats)
        {
            return false;
        }

        cache(object_id);

        return true;
    }

    bool is_cached(const sai_object_id_t object_id)
    {
        return pending_sai_objects.find(object_id) != pending_sai_objects.end();
    }

    void flush(const std::string &group_name)
    {
        if (pending_sai_objects.empty())
        {
            return;
        }

        auto counter_ids = FlexCounterManager::serializeCounterStats(pending_counter_stats);
        auto counter_type_it = FlexCounterManager::counter_id_field_lookup.find(pending_counter_type);

        // Temporary fix for SNMP PFC counter issue: disable bulk init requests for PORT counters
        // This forces each port to be processed individually, avoiding capability mismatches
        // for differnt ports in bulk requests
        if (pending_counter_type == CounterType::PORT)
        {
            for (const auto& oid: pending_sai_objects)
            {
                auto counter_key = group_name + ":" +  sai_serialize_object_id(oid);
                startFlexCounterPolling(pending_switch_id, counter_key, counter_ids, counter_type_it->second);
            }
        }
        else
        {
            auto counter_keys = group_name + ":";
            for (const auto& oid: pending_sai_objects)
            {
                counter_keys += sai_serialize_object_id(oid) + ",";
            }
            counter_keys.pop_back();
    
            startFlexCounterPolling(pending_switch_id, counter_keys, counter_ids, counter_type_it->second);
        }
        
        /* Clear the cached stats and objects after flush */
        pending_sai_objects.clear();
        pending_counter_stats.clear();
        pending_switch_id = SAI_NULL_OBJECT_ID;
    }

    void cache(sai_object_id_t object_id)
    {
        pending_sai_objects.emplace(object_id);
    }
};

class FlexCounterCachedManager : public FlexCounterManager
{
    public:
        FlexCounterCachedManager(
                const std::string& group_name,
                const StatsMode stats_mode,
                const uint polling_interval,
                const bool enabled,
                swss::FieldValueTuple fv_plugin = std::make_pair("","")) :
            FlexCounterManager(group_name, stats_mode, polling_interval, enabled, fv_plugin)
        {
        }

        virtual void flush()
        {
        }

    protected:
        void flush(const std::string &group_name, struct CachedObjects &cached_objects)
        {
            cached_objects.flush(group_name);
        }

        void setCounterIdList(
            struct CachedObjects &cached_objects,
            const sai_object_id_t object_id,
            const CounterType counter_type,
            const std::unordered_set<std::string>& counter_stats,
            const sai_object_id_t switch_id=SAI_NULL_OBJECT_ID)
        {
            if (gTraditionalFlexCounter)
            {
                // Unable to cache an object and initialize in bulk in traditional flex counter mode
                FlexCounterManager::setCounterIdList(object_id, counter_type, counter_stats, switch_id);
                return;
            }

            auto effective_switch_id = switch_id == SAI_NULL_OBJECT_ID ? gSwitchId : switch_id;
            installed_counters[object_id] = effective_switch_id;
            if (cached_objects.try_cache(object_id, counter_type, counter_stats, effective_switch_id))
            {
                return;
            }
            else
            {
                // Either counter_type, counter_ids or switch_id has changed in the new entry 
                // Flush the old entries and save the new one 
                // TODO: Improve the logic to read all the entries and group them before flushing 
                //       to reduce number of sairedis calls
                flush(group_name, cached_objects);
                cached_objects.try_cache(object_id, counter_type, counter_stats, effective_switch_id);
            }
        }

        void clearCounterIdList(
            struct CachedObjects &cached_objects,
            const sai_object_id_t object_id)
        {
            auto search = cached_objects.pending_sai_objects.find(object_id);
            if (search == cached_objects.pending_sai_objects.end())
            {
                FlexCounterManager::clearCounterIdList(object_id);
            }
            else
            {
                installed_counters.erase(object_id);
                cached_objects.pending_sai_objects.erase(search);
            }
        }
};

template <typename TagType, typename Enable=void>
class FlexCounterTaggedCachedManager : public FlexCounterCachedManager
{
    public:
        FlexCounterTaggedCachedManager(
                const std::string& group_name,
                const StatsMode stats_mode,
                const uint polling_interval,
                const bool enabled,
                swss::FieldValueTuple fv_plugin = std::make_pair("","")) :
            FlexCounterCachedManager(group_name, stats_mode, polling_interval, enabled, fv_plugin)
        {
        }

        void flush()
        {
            FlexCounterCachedManager::flush(group_name, cached_objects);
        }

        virtual void setCounterIdList(
            const sai_object_id_t object_id,
            const CounterType counter_type,
            const std::unordered_set<std::string>& counter_stats,
            const sai_object_id_t switch_id=SAI_NULL_OBJECT_ID)
        {
            FlexCounterCachedManager::setCounterIdList(cached_objects,
                                                       object_id,
                                                       counter_type,
                                                       counter_stats);
        }

        virtual void clearCounterIdList(
            const sai_object_id_t object_id)
        {
            FlexCounterCachedManager::clearCounterIdList(cached_objects, object_id);
        }

    private:
        struct CachedObjects cached_objects;
};

template <typename TagType>
class FlexCounterTaggedCachedManager<TagType, typename std::enable_if_t<std::is_enum<TagType>::value>> : public FlexCounterCachedManager
{
    public:
        FlexCounterTaggedCachedManager(
                const std::string& group_name,
                const StatsMode stats_mode,
                const uint polling_interval,
                const bool enabled,
                swss::FieldValueTuple fv_plugin = std::make_pair("","")) :
            FlexCounterCachedManager(group_name, stats_mode, polling_interval, enabled, fv_plugin)
        {
        }

        void flush()
        {
            for(auto &it : cached_objects)
            {
                FlexCounterCachedManager::flush(group_name, it.second);
            }
        }

        void setCounterIdList(
            const sai_object_id_t object_id,
            const CounterType counter_type,
            const std::unordered_set<std::string>& counter_stats,
            const TagType tag,
            const sai_object_id_t switch_id=SAI_NULL_OBJECT_ID)
        {
            FlexCounterCachedManager::setCounterIdList(cached_objects[tag],
                                                       object_id,
                                                       counter_type,
                                                       counter_stats);
        }

        void clearCounterIdList(
            const sai_object_id_t object_id,
            const TagType tag)
        {
            FlexCounterCachedManager::clearCounterIdList(cached_objects[tag], object_id);
        }

    private:
        std::map<TagType, struct CachedObjects> cached_objects;
};

class FlexManagerDirectory
{
    public:
        FlexCounterManager* createFlexCounterManager(const std::string& group_name, const StatsMode stats_mode,
                                                     const uint polling_interval, const bool enabled,
                                                     swss::FieldValueTuple fv_plugin = std::make_pair("",""));
    private:
        std::unordered_map<std::string, FlexCounterManager*>  m_managers;
};

#endif // ORCHAGENT_FLEX_COUNTER_MANAGER_H
