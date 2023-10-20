#include "mock_sai_l2mc.h"

MockSaiL2mc* mock_sai_l2mc;

sai_status_t mock_create_l2mc_entry(_In_ const sai_l2mc_entry_t* l2mc_entry,
                                    _In_ uint32_t attr_count,
                                    _In_ const sai_attribute_t* attr_list) {
  return mock_sai_l2mc->create_l2mc_entry(l2mc_entry, attr_count, attr_list);
}

sai_status_t mock_remove_l2mc_entry(_In_ const sai_l2mc_entry_t* l2mc_entry) {
  return mock_sai_l2mc->remove_l2mc_entry(l2mc_entry);
}

sai_status_t mock_set_l2mc_entry_attribute(
    _In_ const sai_l2mc_entry_t* l2mc_entry, _In_ const sai_attribute_t* attr) {
  return mock_sai_l2mc->set_l2mc_entry_attribute(l2mc_entry, attr);
}

sai_status_t mock_get_l2mc_entry_attribute(
    _In_ const sai_l2mc_entry_t* l2mc_entr, _In_ uint32_t attr_count,
    _Inout_ sai_attribute_t* attr_list) {
  return mock_sai_l2mc->get_l2mc_entry_attribute(l2mc_entr, attr_count,
                                                 attr_list);
}