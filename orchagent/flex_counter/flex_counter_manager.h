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
#include <boost/functional/hash.hpp>

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
    DASH_METER,
    SRV6,
    SWITCH,
    CP_DATA_CHANNEL,
    BULK_SYNC,
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
    struct PendingMapKey
    {
        std::unordered_set<std::string> counter_stats;
        CounterType counter_type;
        sai_object_id_t switch_id;

        bool operator==(const PendingMapKey& other) const {
            return counter_stats == other.counter_stats &&
                   counter_type == other.counter_type &&
                   switch_id == other.switch_id;
        }
    };

    struct PendingMapHash {
        size_t operator()(const PendingMapKey& key) const {
            size_t seed = 0;
            std::vector<std::string> sorted_stats(key.counter_stats.begin(), key.counter_stats.end());
            std::sort(sorted_stats.begin(), sorted_stats.end());
            boost::hash_combine(seed, boost::hash_range(sorted_stats.begin(), sorted_stats.end()));
            boost::hash_combine(seed, key.counter_type);
            boost::hash_combine(seed, key.switch_id);
            return seed;
        }
    };

    std::unordered_map<PendingMapKey, std::unordered_set<sai_object_id_t>, PendingMapHash> pending_objects_map;

    void cache(const sai_object_id_t object_id,
                   const CounterType counter_type,
                   const std::unordered_set<std::string>& counter_stats,
                   sai_object_id_t switch_id)
    {
        PendingMapKey key{counter_stats, counter_type, switch_id};
        pending_objects_map[key].emplace(object_id);
    }

    void flush(const std::string &group_name)
    {
        if (pending_objects_map.empty())
        {
            return;
        }

        for (const auto& entry : pending_objects_map)
        {
            const auto& counter_stats = entry.first.counter_stats;
            const auto& counter_type = entry.first.counter_type;
            const auto& switch_id = entry.first.switch_id;
            const auto& pending_sai_objects = entry.second;

            if (pending_sai_objects.empty())
            {
                continue;
            }

            auto counter_ids = FlexCounterManager::serializeCounterStats(counter_stats);
            auto counter_type_it = FlexCounterManager::counter_id_field_lookup.find(counter_type);

            auto counter_keys = group_name + ":";
            for (const auto& oid: pending_sai_objects)
            {
                counter_keys += sai_serialize_object_id(oid) + ",";
            }
            counter_keys.pop_back();

            startFlexCounterPolling(switch_id, counter_keys, counter_ids, counter_type_it->second);
        }

        /* Clear all cached entries after flush */
        pending_objects_map.clear();
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
            cached_objects.cache(object_id, counter_type, counter_stats, effective_switch_id);
        }

        void clearCounterIdList(
            struct CachedObjects &cached_objects,
            const sai_object_id_t object_id)
        {
            bool found = false;
            for (auto entry = cached_objects.pending_objects_map.begin(); entry != cached_objects.pending_objects_map.end(); )
            {
                if (entry->second.find(object_id) != entry->second.end())
                {
                    found = true;
                    installed_counters.erase(object_id);
                    entry->second.erase(object_id);
                }

                if (found && entry->second.empty())
                {
                    entry = cached_objects.pending_objects_map.erase(entry);
                    break;
                }
                else
                {
                    ++entry;
                }
            }

            if (!found)
            {
                /* If the object is not found in the cached objects, clear the counter id list assuming it is already installed */
                FlexCounterManager::clearCounterIdList(object_id);
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
