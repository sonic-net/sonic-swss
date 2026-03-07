#pragma once

#include <map>
#include <string>
#include <unordered_set>

#include <saitypes.h>

#include "flex_counter_manager.h"

#include "dash_api/eni.pb.h"
#include "dash_api/ha_set.pb.h"

struct EniEntry
{
    sai_object_id_t eni_id;
    dash::eni::Eni metadata;
};

typedef std::map<std::string, EniEntry> EniTable;

template<CounterType CT>
struct DashCounter
{
    FlexCounterManager stat_manager;
    bool fc_status = false;
    std::unordered_set<std::string> counter_stats;

    DashCounter() {}
    DashCounter(const std::string& group_name, StatsMode stats_mode, uint polling_interval, bool enabled) 
        : stat_manager(group_name, stats_mode, polling_interval, enabled) {fetchStats();}
    void fetchStats();
    
    void addToFC(sai_object_id_t oid, const std::string& name)
    {
        if (!fc_status)
        {
            return;
        }

        if (oid == SAI_NULL_OBJECT_ID)
        {
            SWSS_LOG_WARN("Cannot add counter on NULL OID for %s", name.c_str());
            return;
        }
        stat_manager.setCounterIdList(oid, CT, counter_stats);
    }

    void removeFromFC(sai_object_id_t oid, const std::string& name)
    {
        if (oid == SAI_NULL_OBJECT_ID)
        {
            SWSS_LOG_WARN("Cannot remove counter on NULL OID for %s", name.c_str());
            return;
        }
        stat_manager.clearCounterIdList(oid);
    }
};

struct DashEniCounter: DashCounter<CounterType::ENI>
{
    DashEniCounter(const std::string& group_name, StatsMode stats_mode, uint polling_interval, bool enabled) 
        : DashCounter<CounterType::ENI>(group_name, stats_mode, polling_interval, enabled) {}

    void refreshStats(bool install, const EniTable& eni_entries)
    {
        for (auto it = eni_entries.begin(); it != eni_entries.end(); it++)
        {
            if (install)
            {
                addToFC(it->second.eni_id, it->first);
            }
            else
            {
                removeFromFC(it->second.eni_id, it->first);
            }
        }
    }

    void handleStatusUpdate(bool enabled, const EniTable& eni_entries)
    {
        bool prev_enabled = fc_status;
        fc_status = enabled;
        if (fc_status != prev_enabled)
        {
            refreshStats(fc_status, eni_entries);
        }
    }
};

struct DashMeterCounter: DashCounter<CounterType::DASH_METER>
{
    DashMeterCounter(const std::string& group_name, StatsMode stats_mode, uint polling_interval, bool enabled) 
        : DashCounter<CounterType::DASH_METER>(group_name, stats_mode, polling_interval, enabled) {}
    
    void refreshStats(bool install, const EniTable& eni_entries)
    {
        for (auto it = eni_entries.begin(); it != eni_entries.end(); it++)
        {
            if (install)
            {
                addToFC(it->second.eni_id, it->first);
            }
            else
            {
                removeFromFC(it->second.eni_id, it->first);
            }
        }
    }

    void handleStatusUpdate(bool enabled, const EniTable& eni_entries)
    {
        bool prev_enabled = fc_status;
        fc_status = enabled;
        if (fc_status != prev_enabled)
        {
            refreshStats(fc_status, eni_entries);
        }
    }
    };

struct HaSetEntry
{
    sai_object_id_t ha_set_id;
    dash::ha_set::HaSet metadata;
};

typedef std::map<std::string, HaSetEntry> HaSetTable;

struct DashHaCounter: DashCounter<CounterType::HA_SET>
{
    DashHaCounter(const std::string& group_name, StatsMode stats_mode, uint polling_interval, bool enabled) 
        : DashCounter<CounterType::HA_SET>(group_name, stats_mode, polling_interval, enabled) {}
    
    void refreshStats(bool install, const HaSetTable& ha_set_entries)
    {
        for (auto it = ha_set_entries.begin(); it != ha_set_entries.end(); it++)
        {
            if (install)
            {
                addToFC(it->second.ha_set_id, it->first);
            }
            else
            {
                removeFromFC(it->second.ha_set_id, it->first);
            }
        }
    }

    void handleStatusUpdate(bool enabled, const HaSetTable& ha_set_entries)
    {
        bool prev_enabled = fc_status;
        fc_status = enabled;
        if (fc_status != prev_enabled)
        {
            refreshStats(fc_status, ha_set_entries);
        }
    }
};
