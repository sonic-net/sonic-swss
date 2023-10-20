#pragma once

#include <gmock/gmock.h>

extern "C" {
#include "sai.h"
}

// Mock Class mapping methods to L2 multicast SAI APIs.
class MockSaiL2mc {
 public:
  MOCK_METHOD3(create_l2mc_entry,
               sai_status_t(_In_ const sai_l2mc_entry_t* l2mc_entry,
                            _In_ uint32_t attr_count,
                            _In_ const sai_attribute_t* attr_list));

  MOCK_METHOD1(remove_l2mc_entry,
               sai_status_t(_In_ const sai_l2mc_entry_t* l2mc_entry));

  MOCK_METHOD2(set_l2mc_entry_attribute,
               sai_status_t(_In_ const sai_l2mc_entry_t* l2mc_entry,
                            _In_ const sai_attribute_t* attr));

  MOCK_METHOD3(get_l2mc_entry_attribute,
               sai_status_t(_In_ const sai_l2mc_entry_t* l2mc_entry,
                            _In_ uint32_t attr_count,
                            _Inout_ sai_attribute_t* attr_list));
};

extern MockSaiL2mc* mock_sai_l2mc;

sai_status_t mock_create_l2mc_entry(_In_ const sai_l2mc_entry_t* l2mc_entry,
                                    _In_ uint32_t attr_count,
                                    _In_ const sai_attribute_t* attr_list);

sai_status_t mock_remove_l2mc_entry(_In_ const sai_l2mc_entry_t* l2mc_entry);

sai_status_t mock_set_l2mc_entry_attribute(
    _In_ const sai_l2mc_entry_t* l2mc_entry, _In_ const sai_attribute_t* attr);

sai_status_t mock_get_l2mc_entry_attribute(
    _In_ const sai_l2mc_entry_t* l2mc_entr, _In_ uint32_t attr_count,
    _Inout_ sai_attribute_t* attr_list);