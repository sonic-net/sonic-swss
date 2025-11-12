#pragma once

#include <gmock/gmock.h>

extern "C" {
#include "sai.h"
}

// Mock Class mapping methods to L2 multicast group SAI APIs.
class MockSaiL2mcGroup {
 public:
  MOCK_METHOD4(create_l2mc_group,
               sai_status_t(_Out_ sai_object_id_t* l2mc_group_id,
                            _In_ sai_object_id_t switch_id,
                            _In_ uint32_t attr_count,
                            _In_ const sai_attribute_t* attr_list));

  MOCK_METHOD1(remove_l2mc_group,
               sai_status_t(_In_ sai_object_id_t l2mc_group_id));

  MOCK_METHOD2(set_l2mc_group_attribute,
               sai_status_t(_In_ sai_object_id_t l2mc_group_id,
                            _In_ const sai_attribute_t* attr));

  MOCK_METHOD3(get_l2mc_group_attribute,
               sai_status_t(_In_ sai_object_id_t l2mc_group_id,
                            _In_ uint32_t attr_count,
                            _Inout_ sai_attribute_t* attr_list));

  MOCK_METHOD4(create_l2mc_group_member,
               sai_status_t(_Out_ sai_object_id_t* l2mc_group_member_id,
                            _In_ sai_object_id_t switch_id,
                            _In_ uint32_t attr_count,
                            _In_ const sai_attribute_t* attr_list));

  MOCK_METHOD1(remove_l2mc_group_member,
               sai_status_t(_In_ sai_object_id_t l2mc_group_member_id));

  MOCK_METHOD2(set_l2mc_group_member_attribute,
               sai_status_t(_In_ sai_object_id_t l2mc_group_member_id,
                            _In_ const sai_attribute_t* attr));

  MOCK_METHOD3(get_l2mc_group_member_attribute,
               sai_status_t(_In_ sai_object_id_t l2mc_group_member_id,
                            _In_ uint32_t attr_count,
                            _Inout_ sai_attribute_t* attr_list));
};

extern MockSaiL2mcGroup* mock_sai_l2mc_group;

sai_status_t mock_create_l2mc_group(_Out_ sai_object_id_t* l2mc_group_id,
                                    _In_ sai_object_id_t switch_id,
                                    _In_ uint32_t attr_count,
                                    _In_ const sai_attribute_t* attr_list);

sai_status_t mock_remove_l2mc_group(_In_ sai_object_id_t l2mc_group_id);

sai_status_t mock_set_l2mc_group_attribute(_In_ sai_object_id_t l2mc_group_id,
                                           _In_ const sai_attribute_t* attr);

sai_status_t mock_get_l2mc_group_attribute(_In_ sai_object_id_t l2mc_group_id,
                                           _In_ uint32_t attr_count,
                                           _Inout_ sai_attribute_t* attr_list);

sai_status_t mock_create_l2mc_group_member(
    _Out_ sai_object_id_t* l2mc_group_member_id, _In_ sai_object_id_t switch_id,
    _In_ uint32_t attr_count, _In_ const sai_attribute_t* attr_list);

sai_status_t mock_remove_l2mc_group_member(
    _In_ sai_object_id_t l2mc_group_member_id);

sai_status_t mock_set_l2mc_group_member_attribute(
    _In_ sai_object_id_t l2mc_group_member_id,
    _In_ const sai_attribute_t* attr);

sai_status_t mock_get_l2mc_group_member_attribute(
    _In_ sai_object_id_t l2mc_group_member_id, _In_ uint32_t attr_count,
    _Inout_ sai_attribute_t* attr_list);
