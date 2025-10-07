#include "mock_sai_bridge.h"

MockSaiBridge* mock_sai_bridge;

sai_status_t mock_create_bridge(
    _Out_ sai_object_id_t* bridge_id,
    _In_ sai_object_id_t switch_id,
    _In_ uint32_t attr_count,
    _In_ const sai_attribute_t* attr_list) {
  return mock_sai_bridge->create_bridge(
      bridge_id, switch_id, attr_count, attr_list);
}

sai_status_t mock_remove_bridge(
    _In_ sai_object_id_t bridge_id) {
  return mock_sai_bridge->remove_bridge(bridge_id);
}

sai_status_t mock_set_bridge_attribute(
    _In_ sai_object_id_t bridge_id,
    _In_ const sai_attribute_t* attr) {
  return mock_sai_bridge->set_bridge_attribute(bridge_id, attr);
}

sai_status_t mock_get_bridge_attribute(
    _In_ sai_object_id_t bridge_id,
    _In_ uint32_t attr_count,
    _Inout_ sai_attribute_t* attr_list) {
  return mock_sai_bridge->get_bridge_attribute(
      bridge_id, attr_count, attr_list);
}

sai_status_t mock_get_bridge_stats(
    _In_ sai_object_id_t bridge_id,
    _In_ uint32_t number_of_counters,
    _In_ const sai_stat_id_t* counter_ids,
    _Out_ uint64_t *counters) {
  return mock_sai_bridge->get_bridge_stats(
      bridge_id, number_of_counters, counter_ids, counters);
}

sai_status_t mock_get_bridge_stats_ext(
    _In_ sai_object_id_t bridge_id,
    _In_ uint32_t number_of_counters,
    _In_ const sai_stat_id_t* counter_ids,
    _In_ sai_stats_mode_t mode,
    _Out_ uint64_t *counters) {
  return mock_sai_bridge->get_bridge_stats_ext(
      bridge_id, number_of_counters, counter_ids, mode, counters);
}

sai_status_t mock_clear_bridge_stats(
    _In_ sai_object_id_t bridge_id,
    _In_ uint32_t number_of_counters,
    _In_ const sai_stat_id_t* counter_ids) {
  return mock_sai_bridge->clear_bridge_stats(
      bridge_id, number_of_counters, counter_ids);
}

sai_status_t mock_create_bridge_port(
    _Out_ sai_object_id_t* bridge_port_id,
    _In_ sai_object_id_t switch_id,
    _In_ uint32_t attr_count,
    _In_ const sai_attribute_t* attr_list) {
  return mock_sai_bridge->create_bridge_port(
      bridge_port_id, switch_id, attr_count, attr_list);
}

sai_status_t mock_remove_bridge_port(
    _In_ sai_object_id_t bridge_port_id) {
  return mock_sai_bridge->remove_bridge_port(bridge_port_id);
}

sai_status_t mock_set_bridge_port_attribute(
    _In_ sai_object_id_t bridge_port_id,
    _In_ const sai_attribute_t* attr) {
  return mock_sai_bridge->set_bridge_port_attribute(bridge_port_id, attr);
}

sai_status_t mock_get_bridge_port_attribute(
    _In_ sai_object_id_t bridge_port_id,
    _In_ uint32_t attr_count,
    _Inout_ sai_attribute_t* attr_list) {
  return mock_sai_bridge->get_bridge_port_attribute(
      bridge_port_id, attr_count, attr_list);
}

sai_status_t mock_get_bridge_port_stats(
    _In_ sai_object_id_t bridge_port_id,
    _In_ uint32_t number_of_counters,
    _In_ const sai_stat_id_t* counter_ids,
    _Out_ uint64_t *counters) {
  return mock_sai_bridge->get_bridge_port_stats(
      bridge_port_id, number_of_counters, counter_ids, counters);
}

sai_status_t mock_get_bridge_port_stats_ext(
    _In_ sai_object_id_t bridge_port_id,
    _In_ uint32_t number_of_counters,
    _In_ const sai_stat_id_t* counter_ids,
    _In_ sai_stats_mode_t mode,
    _Out_ uint64_t *counters) {
  return mock_sai_bridge->get_bridge_port_stats_ext(
      bridge_port_id, number_of_counters, counter_ids, mode, counters);
}

sai_status_t mock_clear_bridge_port_stats(
    _In_ sai_object_id_t bridge_port_id,
    _In_ uint32_t number_of_counters,
    _In_ const sai_stat_id_t* counter_ids) {
  return mock_sai_bridge->clear_bridge_port_stats(
      bridge_port_id, number_of_counters, counter_ids);
}
