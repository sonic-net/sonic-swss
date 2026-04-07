#pragma once

#include <string>
#include <unordered_set>

#include <saitypes.h>

#include "logger.h"
#include "flex_counter_manager.h"

template<CounterType CT, typename TableT>
struct DashCounter
{
    FlexCounterManager stat_manager;
    bool fc_status = false;
    std::unordered_set<std::string> counter_stats;

    DashCounter() {}
    DashCounter(const std::string& group_name, StatsMode stats_mode, uint32_t polling_interval, bool enabled)
        : stat_manager(group_name, stats_mode, polling_interval, enabled), fc_status(enabled) {fetchStats();}
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

    void refreshStats(bool install, const TableT& entries)
    {
        for (auto it = entries.begin(); it != entries.end(); it++)
        {
            if (install)
            {
                addToFC(it->second.getOid(), it->first);
            }
            else
            {
                removeFromFC(it->second.getOid(), it->first);
            }
        }
    }

    void handleStatusUpdate(bool enabled, const TableT& entries)
    {
        bool prev_enabled = fc_status;
        fc_status = enabled;
        if (fc_status != prev_enabled)
        {
            refreshStats(fc_status, entries);
        }
    }
};
