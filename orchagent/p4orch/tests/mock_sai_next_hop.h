// Define classes and functions to mock SAI next hop functions.
#pragma once

#include <gmock/gmock.h>

extern "C"
{
#include "sai.h"
}

// Mock class including mock functions mapping to SAI next hop's functions.
class MockSaiNextHop
{
  public:
    MOCK_METHOD4(create_next_hop, sai_status_t(_Out_ sai_object_id_t *next_hop_id, _In_ sai_object_id_t switch_id,
                                               _In_ uint32_t attr_count, _In_ const sai_attribute_t *attr_list));

    MOCK_METHOD1(remove_next_hop, sai_status_t(_In_ sai_object_id_t next_hop_id));

    MOCK_METHOD2(set_next_hop_attribute,
                 sai_status_t(_In_ sai_object_id_t next_hop_id, _In_ const sai_attribute_t *attr));

    MOCK_METHOD3(get_next_hop_attribute, sai_status_t(_In_ sai_object_id_t next_hop_id, _In_ uint32_t attr_count,
                                                      _Inout_ sai_attribute_t *attr_list));

    MOCK_METHOD7(create_next_hops, sai_status_t(_In_ sai_object_id_t switch_id, _In_ uint32_t object_count, _In_ const uint32_t *attr_count,
                                                _In_ const sai_attribute_t **attr_list, _In_ sai_bulk_op_error_mode_t mode,
                                                _Out_ sai_object_id_t *object_id, _Out_ sai_status_t *object_statuses));

    MOCK_METHOD4(remove_next_hops, sai_status_t(_In_ uint32_t object_count, _In_ const sai_object_id_t *object_id, _In_ sai_bulk_op_error_mode_t mode,
                                                _Out_ sai_status_t *object_statuses));

    MOCK_METHOD5(set_next_hops_attribute, sai_status_t(_In_ uint32_t object_count, _In_ const sai_object_id_t *object_id, _In_ const sai_attribute_t *attr_list,
                                                       _In_ sai_bulk_op_error_mode_t mode, _Out_ sai_status_t *object_statuses));
};

// Note that before mock functions below are used, mock_sai_next_hop must be
// initialized to point to an instance of MockSaiNextHop.
extern MockSaiNextHop *mock_sai_next_hop;

sai_status_t mock_create_next_hop(_Out_ sai_object_id_t *next_hop_id, _In_ sai_object_id_t switch_id,
                                  _In_ uint32_t attr_count, _In_ const sai_attribute_t *attr_list);

sai_status_t mock_remove_next_hop(_In_ sai_object_id_t next_hop_id);

sai_status_t mock_set_next_hop_attribute(_In_ sai_object_id_t next_hop_id, _In_ const sai_attribute_t *attr);

sai_status_t mock_get_next_hop_attribute(_In_ sai_object_id_t next_hop_id, _In_ uint32_t attr_count,
                                         _Inout_ sai_attribute_t *attr_list);

sai_status_t mock_create_next_hops(_In_ sai_object_id_t switch_id, _In_ uint32_t object_count, _In_ const uint32_t *attr_count,
                                   _In_ const sai_attribute_t **attr_list, _In_ sai_bulk_op_error_mode_t mode,
                                   _Out_ sai_object_id_t *object_id, _Out_ sai_status_t *object_statuses);

sai_status_t mock_remove_next_hops(_In_ uint32_t object_count, _In_ const sai_object_id_t *object_id, _In_ sai_bulk_op_error_mode_t mode,
                                   _Out_ sai_status_t *object_statuses);

sai_status_t mock_set_next_hops_attribute(_In_ uint32_t object_count, _In_ const sai_object_id_t *object_id, _In_ const sai_attribute_t *attr_list,
                                          _In_ sai_bulk_op_error_mode_t mode, _Out_ sai_status_t *object_statuses);
