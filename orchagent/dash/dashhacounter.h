#pragma once

#include <map>
#include <string>

#include <saitypes.h>

#include "dashcounter.h"

#include "dash_api/ha_set.pb.h"

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
