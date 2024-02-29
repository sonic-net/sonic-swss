#pragma once

#include <gmock/gmock.h>

extern "C"
{
#include "sai.h"
}

// Mock Class mapping methods to switch object SAI APIs.
class MockSaiSwitch
{
  public:
    MOCK_METHOD3(get_switch_attribute, sai_status_t(_In_ sai_object_id_t switch_id, _In_ uint32_t attr_count,
                                                    _Inout_ sai_attribute_t *attr_list));
    MOCK_METHOD2(set_switch_attribute, sai_status_t(_In_ sai_object_id_t switch_id, _In_ const sai_attribute_t *attr));
};

extern MockSaiSwitch *mock_sai_switch;

sai_status_t mock_get_switch_attribute(_In_ sai_object_id_t switch_id, _In_ uint32_t attr_count,
                                       _Inout_ sai_attribute_t *attr_list);

sai_status_t mock_set_switch_attribute(_In_ sai_object_id_t switch_id, _In_ const sai_attribute_t *attr);
