#include "mock_sai_route.h"

MockSaiRoute* mock_sai_route;

sai_status_t create_route_entry(const sai_route_entry_t* route_entry,
                                uint32_t attr_count,
                                const sai_attribute_t* attr_list) {
  return mock_sai_route->create_route_entry(route_entry, attr_count, attr_list);
}

sai_status_t remove_route_entry(const sai_route_entry_t* route_entry) {
  return mock_sai_route->remove_route_entry(route_entry);
}

sai_status_t set_route_entry_attribute(const sai_route_entry_t* route_entry,
                                       const sai_attribute_t* attr) {
  return mock_sai_route->set_route_entry_attribute(route_entry, attr);
}

sai_status_t get_route_entry_attribute(const sai_route_entry_t* route_entry,
                                       uint32_t attr_count,
                                       sai_attribute_t* attr_list) {
  return mock_sai_route->get_route_entry_attribute(route_entry, attr_count,
                                                   attr_list);
}

sai_status_t create_route_entries(uint32_t object_count,
                                  const sai_route_entry_t* route_entry,
                                  const uint32_t* attr_count,
                                  const sai_attribute_t** attr_list,
                                  sai_bulk_op_error_mode_t mode,
                                  sai_status_t* object_statuses) {
  return mock_sai_route->create_route_entries(
      object_count, route_entry, attr_count, attr_list, mode, object_statuses);
}

sai_status_t remove_route_entries(uint32_t object_count,
                                  const sai_route_entry_t* route_entry,
                                  sai_bulk_op_error_mode_t mode,
                                  sai_status_t* object_statuses) {
  return mock_sai_route->remove_route_entries(object_count, route_entry, mode,
                                              object_statuses);
}

sai_status_t set_route_entries_attribute(uint32_t object_count,
                                         const sai_route_entry_t* route_entry,
                                         const sai_attribute_t* attr_list,
                                         sai_bulk_op_error_mode_t mode,
                                         sai_status_t* object_statuses) {
  return mock_sai_route->set_route_entries_attribute(
      object_count, route_entry, attr_list, mode, object_statuses);
}

sai_status_t get_route_entries_attribute(uint32_t object_count,
                                         const sai_route_entry_t* route_entry,
                                         const uint32_t* attr_count,
                                         sai_attribute_t** attr_list,
                                         sai_bulk_op_error_mode_t mode,
                                         sai_status_t* object_statuses) {
  return mock_sai_route->get_route_entries_attribute(
      object_count, route_entry, attr_count, attr_list, mode, object_statuses);
}