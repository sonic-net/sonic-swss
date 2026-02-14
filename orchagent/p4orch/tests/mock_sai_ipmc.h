#pragma once

#include <gmock/gmock.h>

extern "C" {
#include "sai.h"
}

// Mock Class mapping methods to IPMC (multicast entry) SAI APIs.
class MockSaiIpmc {
 public:
  MOCK_METHOD3(create_ipmc_entry,
               sai_status_t(_In_ const sai_ipmc_entry_t* ipmc_entry,
                            _In_ uint32_t attr_count,
                            _In_ const sai_attribute_t* attr_list));

  MOCK_METHOD1(remove_ipmc_entry,
               sai_status_t(_In_ const sai_ipmc_entry_t* ipmc_entry));

  MOCK_METHOD2(set_ipmc_entry_attribute,
               sai_status_t(_In_ const sai_ipmc_entry_t* ipmc_entry,
                            _In_ const sai_attribute_t* attr));

  MOCK_METHOD3(get_ipmc_entry_attribute,
               sai_status_t(_In_ const sai_ipmc_entry_t* ipmc_entry,
                            _In_ uint32_t attr_count,
                            _Inout_ sai_attribute_t* attr_list));
};

extern MockSaiIpmc* mock_sai_ipmc;

sai_status_t mock_create_ipmc_entry(_In_ const sai_ipmc_entry_t* ipmc_entry,
                                    _In_ uint32_t attr_count,
                                    _In_ const sai_attribute_t* attr_list);

sai_status_t mock_remove_ipmc_entry(_In_ const sai_ipmc_entry_t* ipmc_entry);

sai_status_t mock_set_ipmc_entry_attribute(
    _In_ const sai_ipmc_entry_t* ipmc_entry, _In_ const sai_attribute_t* attr);

sai_status_t mock_get_ipmc_entry_attribute(
    _In_ const sai_ipmc_entry_t* ipmc_entry, _In_ uint32_t attr_count,
    _Inout_ sai_attribute_t* attr_list);
