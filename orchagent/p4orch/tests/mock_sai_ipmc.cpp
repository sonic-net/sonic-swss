#include "mock_sai_ipmc.h"

MockSaiIpmc* mock_sai_ipmc;

sai_status_t mock_create_ipmc_entry(_In_ const sai_ipmc_entry_t* ipmc_entry,
                                    _In_ uint32_t attr_count,
                                    _In_ const sai_attribute_t* attr_list) {
  return mock_sai_ipmc->create_ipmc_entry(ipmc_entry, attr_count, attr_list);
}

sai_status_t mock_remove_ipmc_entry(_In_ const sai_ipmc_entry_t* ipmc_entry) {
  return mock_sai_ipmc->remove_ipmc_entry(ipmc_entry);
}

sai_status_t mock_set_ipmc_entry_attribute(
    _In_ const sai_ipmc_entry_t* ipmc_entry, _In_ const sai_attribute_t* attr) {
  return mock_sai_ipmc->set_ipmc_entry_attribute(ipmc_entry, attr);
}

sai_status_t mock_get_ipmc_entry_attribute(
    _In_ const sai_ipmc_entry_t* ipmc_entry, _In_ uint32_t attr_count,
    _Inout_ sai_attribute_t* attr_list) {
  return mock_sai_ipmc->get_ipmc_entry_attribute(ipmc_entry, attr_count,
                                                 attr_list);
}