#pragma once

#include <gmock/gmock.h>

extern "C" {
#include "sai.h"
}

// Mock Class mapping methods to bridge SAI APIs.
class MockSaiBridge {
 public:
  MOCK_METHOD4(create_bridge,
               sai_status_t(_Out_ sai_object_id_t* bridge_id,
                            _In_ sai_object_id_t switch_id,
                            _In_ uint32_t attr_count,
                            _In_ const sai_attribute_t* attr_list));

  MOCK_METHOD1(remove_bridge,
               sai_status_t(_In_ sai_object_id_t bridge_id));

  MOCK_METHOD2(set_bridge_attribute,
               sai_status_t(_In_ sai_object_id_t bridge_id,
                            _In_ const sai_attribute_t* attr));

  MOCK_METHOD3(get_bridge_attribute,
               sai_status_t(_In_ sai_object_id_t bridge_id,
                            _In_ uint32_t attr_count,
                            _Inout_ sai_attribute_t* attr_list));

  MOCK_METHOD4(get_bridge_stats,
               sai_status_t(_In_ sai_object_id_t bridge_id,
                            _In_ uint32_t number_of_counters,
                            _In_ const sai_stat_id_t* counter_ids,
                            _Out_ uint64_t *counters));

  MOCK_METHOD5(get_bridge_stats_ext,
               sai_status_t(_In_ sai_object_id_t bridge_id,
                            _In_ uint32_t number_of_counters,
                            _In_ const sai_stat_id_t* counter_ids,
                            _In_ sai_stats_mode_t mode,
                            _Out_ uint64_t *counters));

  MOCK_METHOD3(clear_bridge_stats,
               sai_status_t(_In_ sai_object_id_t bridge_id,
                            _In_ uint32_t number_of_counters,
                            _In_ const sai_stat_id_t *counter_ids));

  MOCK_METHOD4(create_bridge_port,
               sai_status_t(_Out_ sai_object_id_t* bridge_port_id,
                            _In_ sai_object_id_t switch_id,
                            _In_ uint32_t attr_count,
                            _In_ const sai_attribute_t* attr_list));

  MOCK_METHOD1(remove_bridge_port,
               sai_status_t(_In_ sai_object_id_t bridge_port_id));

  MOCK_METHOD2(set_bridge_port_attribute,
               sai_status_t(_In_ sai_object_id_t bridge_port_id,
                            _In_ const sai_attribute_t* attr));

  MOCK_METHOD3(get_bridge_port_attribute,
               sai_status_t(_In_ sai_object_id_t bridge_port_id,
                            _In_ uint32_t attr_count,
                            _Inout_ sai_attribute_t* attr_list));

  MOCK_METHOD4(get_bridge_port_stats,
               sai_status_t(_In_ sai_object_id_t bridge_port_id,
                            _In_ uint32_t number_of_counters,
                            _In_ const sai_stat_id_t* counter_ids,
                            _Out_ uint64_t *counters));

  MOCK_METHOD5(get_bridge_port_stats_ext,
               sai_status_t(_In_ sai_object_id_t bridge_port_id,
                            _In_ uint32_t number_of_counters,
                            _In_ const sai_stat_id_t* counter_ids,
                            _In_ sai_stats_mode_t mode,
                            _Out_ uint64_t *counters));

  MOCK_METHOD3(clear_bridge_port_stats,
               sai_status_t(_In_ sai_object_id_t bridge_port_id,
                            _In_ uint32_t number_of_counters,
                            _In_ const sai_stat_id_t *counter_ids));
};

extern MockSaiBridge* mock_sai_bridge;

sai_status_t mock_create_bridge(
    _Out_ sai_object_id_t* bridge_id,
    _In_ sai_object_id_t switch_id,
    _In_ uint32_t attr_count,
    _In_ const sai_attribute_t* attr_list);

sai_status_t mock_remove_bridge(
    _In_ sai_object_id_t bridge_id);

sai_status_t mock_set_bridge_attribute(
    _In_ sai_object_id_t bridge_id,
    _In_ const sai_attribute_t* attr);

sai_status_t mock_get_bridge_attribute(
    _In_ sai_object_id_t bridge_id,
    _In_ uint32_t attr_count,
    _Inout_ sai_attribute_t* attr_list);

sai_status_t mock_get_bridge_stats(
    _In_ sai_object_id_t bridge_id,
    _In_ uint32_t number_of_counters,
    _In_ const sai_stat_id_t* counter_ids,
    _Out_ uint64_t *counters);

sai_status_t mock_get_bridge_stats_ext(
    _In_ sai_object_id_t bridge_id,
    _In_ uint32_t number_of_counters,
    _In_ const sai_stat_id_t* counter_ids,
    _In_ sai_stats_mode_t mode,
    _Out_ uint64_t *counters);

sai_status_t mock_clear_bridge_stats(
    _In_ sai_object_id_t bridge_id,
    _In_ uint32_t number_of_counters,
    _In_ const sai_stat_id_t* counter_ids);

sai_status_t mock_create_bridge_port(
    _Out_ sai_object_id_t* bridge_port_id,
    _In_ sai_object_id_t switch_id,
    _In_ uint32_t attr_count,
    _In_ const sai_attribute_t* attr_list);

sai_status_t mock_remove_bridge_port(
    _In_ sai_object_id_t bridge_port_id);

sai_status_t mock_set_bridge_port_attribute(
    _In_ sai_object_id_t bridge_port_id,
    _In_ const sai_attribute_t* attr);

sai_status_t mock_get_bridge_port_attribute(
    _In_ sai_object_id_t bridge_port_id,
    _In_ uint32_t attr_count,
    _Inout_ sai_attribute_t* attr_list);

sai_status_t mock_get_bridge_port_stats(
    _In_ sai_object_id_t bridge_port_id,
    _In_ uint32_t number_of_counters,
    _In_ const sai_stat_id_t* counter_ids,
    _Out_ uint64_t *counters);

sai_status_t mock_get_bridge_port_stats_ext(
    _In_ sai_object_id_t bridge_port_id,
    _In_ uint32_t number_of_counters,
    _In_ const sai_stat_id_t* counter_ids,
    _In_ sai_stats_mode_t mode,
    _Out_ uint64_t *counters);

sai_status_t mock_clear_bridge_port_stats(
    _In_ sai_object_id_t bridge_port_id,
    _In_ uint32_t number_of_counters,
    _In_ const sai_stat_id_t* counter_ids);
