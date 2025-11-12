#pragma once

#include <gmock/gmock.h>

extern "C" {
#include "sai.h"
}

// Mock Class mapping methods to RPF group SAI APIs.
class MockSaiRpfGroup {
 public:
  MOCK_METHOD4(create_rpf_group,
               sai_status_t(_Out_ sai_object_id_t* rpf_group_id,
                            _In_ sai_object_id_t switch_id,
                            _In_ uint32_t attr_count,
                            _In_ const sai_attribute_t* attr_list));

  MOCK_METHOD1(remove_rpf_group,
               sai_status_t(_In_ sai_object_id_t rpf_group_id));

  MOCK_METHOD2(set_rpf_group_attribute,
               sai_status_t(_In_ sai_object_id_t rpf_group_id,
                            _In_ const sai_attribute_t* attr));

  MOCK_METHOD3(get_rpf_group_attribute,
               sai_status_t(_In_ sai_object_id_t rpf_group_id,
                            _In_ uint32_t attr_count,
                            _Inout_ sai_attribute_t* attr_list));

  MOCK_METHOD4(create_rpf_group_member,
               sai_status_t(_Out_ sai_object_id_t* rpf_group_member_id,
                            _In_ sai_object_id_t switch_id,
                            _In_ uint32_t attr_count,
                            _In_ const sai_attribute_t* attr_list));

  MOCK_METHOD1(remove_rpf_group_member,
               sai_status_t(_In_ sai_object_id_t rpf_group_member_id));

  MOCK_METHOD2(set_rpf_group_member_attribute,
               sai_status_t(_In_ sai_object_id_t rpf_group_member_id,
                            _In_ const sai_attribute_t* attr));

  MOCK_METHOD3(get_rpf_group_member_attribute,
               sai_status_t(_In_ sai_object_id_t rpf_group_member_id,
                            _In_ uint32_t attr_count,
                            _Inout_ sai_attribute_t* attr_list));
};

extern MockSaiRpfGroup* mock_sai_rpf_group;

sai_status_t mock_create_rpf_group(_Out_ sai_object_id_t* rpf_group_id,
                                   _In_ sai_object_id_t switch_id,
                                   _In_ uint32_t attr_count,
                                   _In_ const sai_attribute_t* attr_list);

sai_status_t mock_remove_rpf_group(_In_ sai_object_id_t rpf_group_id);

sai_status_t mock_set_rpf_group_attribute(_In_ sai_object_id_t rpf_group_id,
                                          _In_ const sai_attribute_t* attr);

sai_status_t mock_get_rpf_group_attribute(_In_ sai_object_id_t rpf_group_id,
                                          _In_ uint32_t attr_count,
                                          _Inout_ sai_attribute_t* attr_list);

sai_status_t mock_create_rpf_group_member(
    _Out_ sai_object_id_t* rpf_group_member_id, _In_ sai_object_id_t switch_id,
    _In_ uint32_t attr_count, _In_ const sai_attribute_t* attr_list);

sai_status_t mock_remove_rpf_group_member(
    _In_ sai_object_id_t rpf_group_member_id);

sai_status_t mock_set_rpf_group_member_attribute(
    _In_ sai_object_id_t rpf_group_member_id, _In_ const sai_attribute_t* attr);

sai_status_t mock_get_rpf_group_member_attribute(
    _In_ sai_object_id_t rpf_group_member_id, _In_ uint32_t attr_count,
    _Inout_ sai_attribute_t* attr_list);
