#include "mock_sai_ipmc_group.h"

MockSaiIpmcGroup* mock_sai_ipmc_group;

sai_status_t mock_create_ipmc_group(_Out_ sai_object_id_t* ipmc_group_id,
                                    _In_ sai_object_id_t switch_id,
                                    _In_ uint32_t attr_count,
                                    _In_ const sai_attribute_t* attr_list) {
  return mock_sai_ipmc_group->create_ipmc_group(ipmc_group_id, switch_id,
                                                attr_count, attr_list);
}

sai_status_t mock_remove_ipmc_group(_In_ sai_object_id_t ipmc_group_id) {
  return mock_sai_ipmc_group->remove_ipmc_group(ipmc_group_id);
}

sai_status_t mock_create_ipmc_group_member(
    _Out_ sai_object_id_t* ipmc_group_member_id, _In_ sai_object_id_t switch_id,
    _In_ uint32_t attr_count, _In_ const sai_attribute_t* attr_list) {
  return mock_sai_ipmc_group->create_ipmc_group_member(
      ipmc_group_member_id, switch_id, attr_count, attr_list);
}

sai_status_t mock_remove_ipmc_group_member(
    _In_ sai_object_id_t ipmc_group_member_id) {
  return mock_sai_ipmc_group->remove_ipmc_group_member(ipmc_group_member_id);
}

sai_status_t mock_set_ipmc_group_member_attribute(
    _In_ sai_object_id_t ipmc_group_member_id,
    _In_ const sai_attribute_t* attr) {
  return mock_sai_ipmc_group->set_ipmc_group_member_attribute(
      ipmc_group_member_id, attr);
}

sai_status_t mock_get_ipmc_group_member_attribute(
    _In_ sai_object_id_t ipmc_group_member_id, _In_ uint32_t attr_count,
    _Inout_ sai_attribute_t* attr_list) {
  return mock_sai_ipmc_group->get_ipmc_group_member_attribute(
      ipmc_group_member_id, attr_count, attr_list);
}
