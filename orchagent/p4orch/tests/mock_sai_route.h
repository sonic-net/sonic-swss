#pragma once

#include <gmock/gmock.h>

extern "C"
{
#include "sai.h"
#include "sairoute.h"
}

class MockSaiRoute {
  public:
    MOCK_METHOD3(create_route_entry, sai_status_t(const sai_route_entry_t *route_entry, uint32_t attr_count,
                                                  const sai_attribute_t *attr_list));
    MOCK_METHOD1(remove_route_entry, sai_status_t(const sai_route_entry_t *route_entry));
    MOCK_METHOD2(set_route_entry_attribute,
                 sai_status_t(const sai_route_entry_t *route_entry, const sai_attribute_t *attr));
    MOCK_METHOD3(get_route_entry_attribute,
                 sai_status_t(const sai_route_entry_t *route_entry, uint32_t attr_count, sai_attribute_t *attr_list));
    MOCK_METHOD6(create_route_entries, sai_status_t(uint32_t object_count, const sai_route_entry_t *route_entry,
                                                    const uint32_t *attr_count, const sai_attribute_t **attr_list,
                                                    sai_bulk_op_error_mode_t mode, sai_status_t *object_statuses));
    MOCK_METHOD4(remove_route_entries, sai_status_t(uint32_t object_count, const sai_route_entry_t *route_entry,
                                                    sai_bulk_op_error_mode_t mode, sai_status_t *object_statuses));
    MOCK_METHOD5(set_route_entries_attribute,
                 sai_status_t(uint32_t object_count, const sai_route_entry_t *route_entry,
                              const sai_attribute_t *attr_list, sai_bulk_op_error_mode_t mode,
                              sai_status_t *object_statuses));
    MOCK_METHOD6(get_route_entries_attribute,
                 sai_status_t(uint32_t object_count, const sai_route_entry_t *route_entry, const uint32_t *attr_count,
                              sai_attribute_t **attr_list, sai_bulk_op_error_mode_t mode,
                              sai_status_t *object_statuses));
};

extern MockSaiRoute* mock_sai_route;

sai_status_t create_route_entry(const sai_route_entry_t *route_entry, uint32_t attr_count,
                                const sai_attribute_t *attr_list);

sai_status_t remove_route_entry(const sai_route_entry_t *route_entry);

sai_status_t set_route_entry_attribute(const sai_route_entry_t *route_entry, const sai_attribute_t *attr);

sai_status_t get_route_entry_attribute(const sai_route_entry_t *route_entry, uint32_t attr_count,
                                       sai_attribute_t *attr_list);

sai_status_t create_route_entries(uint32_t object_count, const sai_route_entry_t *route_entry,
                                  const uint32_t *attr_count, const sai_attribute_t **attr_list,
                                  sai_bulk_op_error_mode_t mode, sai_status_t *object_statuses);

sai_status_t remove_route_entries(uint32_t object_count, const sai_route_entry_t *route_entry,
                                  sai_bulk_op_error_mode_t mode, sai_status_t *object_statuses);

sai_status_t set_route_entries_attribute(uint32_t object_count, const sai_route_entry_t *route_entry,
                                         const sai_attribute_t *attr_list, sai_bulk_op_error_mode_t mode,
                                         sai_status_t *object_statuses);

sai_status_t get_route_entries_attribute(uint32_t object_count, const sai_route_entry_t *route_entry,
                                         const uint32_t *attr_count, sai_attribute_t **attr_list,
                                         sai_bulk_op_error_mode_t mode, sai_status_t *object_statuses);
