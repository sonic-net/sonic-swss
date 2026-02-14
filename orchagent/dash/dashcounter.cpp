#include "dashcounter.h"

#include "sai.h"
#include "saiextensions.h"
#include "saihelper.h"

template<>
void DashCounter<CounterType::ENI>::fetchStats()
{
    counter_stats.clear();
    auto stat_enum_list = queryAvailableCounterStats((sai_object_type_t)SAI_OBJECT_TYPE_ENI);
    for (auto &stat_enum: stat_enum_list)
    {
        auto counter_id = static_cast<sai_eni_stat_t>(stat_enum);
        counter_stats.insert(sai_serialize_eni_stat(counter_id));
    }
}

template<>
void DashCounter<CounterType::DASH_METER>::fetchStats()
{
    counter_stats.clear();
    auto stat_enum_list = queryAvailableCounterStats((sai_object_type_t)SAI_OBJECT_TYPE_METER_BUCKET_ENTRY);
    for (auto &stat_enum: stat_enum_list)
    {
        auto counter_id = static_cast<sai_meter_bucket_entry_stat_t>(stat_enum);
        counter_stats.insert(sai_serialize_meter_bucket_entry_stat(counter_id));
    }
}

template<>
void DashCounter<CounterType::HA_SET>::fetchStats()
{
    counter_stats.clear();
    auto stat_enum_list = queryAvailableCounterStats((sai_object_type_t)SAI_OBJECT_TYPE_HA_SET);
    for (auto &stat_enum: stat_enum_list)
    {
        auto counter_id = static_cast<sai_ha_set_stat_t>(stat_enum);
        counter_stats.insert(sai_serialize_ha_set_stat(counter_id));
    }
}