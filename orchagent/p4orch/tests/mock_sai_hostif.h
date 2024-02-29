#pragma once

#include <gmock/gmock.h>

extern "C"
{
#include "sai.h"
}

// Mock Class mapping methods to host interface object SAI APIs.
class MockSaiHostif
{
  public:
    MOCK_METHOD4(create_hostif_trap_group,
                 sai_status_t(_Out_ sai_object_id_t *hostif_trap_group_id, _In_ sai_object_id_t switch_id,
                              _In_ uint32_t attr_count, _In_ const sai_attribute_t *attr_list));

    MOCK_METHOD2(set_hostif_trap_group_attribute,
                 sai_status_t(_In_ sai_object_id_t hostif_trap_group_id, _In_ const sai_attribute_t *attr));

    MOCK_METHOD1(remove_hostif_trap_group, sai_status_t(_In_ sai_object_id_t hostif_trap_group_id));

    MOCK_METHOD4(create_hostif_table_entry,
                 sai_status_t(_Out_ sai_object_id_t *hostif_table_entry_id, _In_ sai_object_id_t switch_id,
                              _In_ uint32_t attr_count, _In_ const sai_attribute_t *attr_list));

    MOCK_METHOD1(remove_hostif_table_entry, sai_status_t(_In_ sai_object_id_t hostif_table_entry_id));

    MOCK_METHOD4(create_hostif_user_defined_trap,
                 sai_status_t(_Out_ sai_object_id_t *hostif_user_defined_trap_id, _In_ sai_object_id_t switch_id,
                              _In_ uint32_t attr_count, _In_ const sai_attribute_t *attr_list));

    MOCK_METHOD1(remove_hostif_user_defined_trap, sai_status_t(_In_ sai_object_id_t hostif_user_defined_trap_id));

    MOCK_METHOD4(create_hostif_trap, sai_status_t(_Out_ sai_object_id_t *hostif_trap_id, _In_ sai_object_id_t switch_id,
                                                  _In_ uint32_t attr_count, _In_ const sai_attribute_t *attr_list));

    MOCK_METHOD1(remove_hostif_trap, sai_status_t(_In_ sai_object_id_t hostif_trap_id));

    MOCK_METHOD4(create_hostif, sai_status_t(_Out_ sai_object_id_t *hostif_id, _In_ sai_object_id_t switch_id,
                                             _In_ uint32_t attr_count, _In_ const sai_attribute_t *attr_list));

    MOCK_METHOD1(remove_hostif, sai_status_t(_In_ sai_object_id_t hostif_id));
};

extern MockSaiHostif *mock_sai_hostif;

sai_status_t mock_create_hostif_trap_group(_Out_ sai_object_id_t *hostif_trap_group_id, _In_ sai_object_id_t switch_id,
                                           _In_ uint32_t attr_count, _In_ const sai_attribute_t *attr_list);

sai_status_t mock_set_hostif_trap_group_attribute(_In_ sai_object_id_t hostif_trap_group_id,
                                                  _In_ const sai_attribute_t *attr);

sai_status_t mock_remove_hostif_trap_group(_In_ const sai_object_id_t hostif_trap_group_id);

sai_status_t mock_create_hostif_table_entry(_Out_ sai_object_id_t *hostif_table_entry_id,
                                            _In_ sai_object_id_t switch_id, _In_ uint32_t attr_count,
                                            _In_ const sai_attribute_t *attr_list);

sai_status_t mock_remove_hostif_table_entry(_In_ const sai_object_id_t hostif_table_entry_id);

sai_status_t mock_create_hostif_user_defined_trap(_Out_ sai_object_id_t *hostif_user_defined_trap_id,
                                                  _In_ sai_object_id_t switch_id, _In_ uint32_t attr_count,
                                                  _In_ const sai_attribute_t *attr_list);

sai_status_t mock_remove_hostif_user_defined_trap(_In_ const sai_object_id_t hostif_user_defined_trap_id);

sai_status_t mock_create_hostif_trap(_Out_ sai_object_id_t *hostif_trap_id, _In_ sai_object_id_t switch_id,
                                     _In_ uint32_t attr_count, _In_ const sai_attribute_t *attr_list);

sai_status_t mock_remove_hostif_trap(_In_ const sai_object_id_t hostif_trap_id);

sai_status_t mock_create_hostif(_Out_ sai_object_id_t *hostif_id, _In_ sai_object_id_t switch_id,
                                _In_ uint32_t attr_count, _In_ const sai_attribute_t *attr_list);

sai_status_t mock_remove_hostif(_In_ const sai_object_id_t hostif_id);
