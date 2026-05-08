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

sai_status_t mock_set_ipmc_group_attribute(_In_ sai_object_id_t ipmc_group_id,
                                           _In_ const sai_attribute_t* attr) {
  return mock_sai_ipmc_group->set_ipmc_group_attribute(ipmc_group_id, attr);
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

sai_status_t mock_create_ipmc_groups(_In_ sai_object_id_t switch_id,
                                     _In_ uint32_t object_count,
                                     _In_ const uint32_t* attr_count,
                                     _In_ const sai_attribute_t** attr_list,
                                     _In_ sai_bulk_op_error_mode_t mode,
                                     _Out_ sai_object_id_t* object_id,
                                     _Out_ sai_status_t* object_statuses) {
  return mock_sai_ipmc_group->create_ipmc_groups(switch_id, object_count,
                                                 attr_count, attr_list, mode,
                                                 object_id, object_statuses);
}

sai_status_t mock_remove_ipmc_groups(_In_ uint32_t object_count,
                                     _In_ const sai_object_id_t* object_id,
                                     _In_ sai_bulk_op_error_mode_t mode,
                                     _Out_ sai_status_t* object_statuses) {
  return mock_sai_ipmc_group->remove_ipmc_groups(object_count, object_id, mode,
                                                 object_statuses);
}

sai_status_t mock_set_ipmc_groups_attribute(
    _In_ uint32_t object_count, _In_ const sai_object_id_t* object_id,
    _In_ const sai_attribute_t* attr_list, _In_ sai_bulk_op_error_mode_t mode,
    _Out_ sai_status_t* object_statuses) {
  return mock_sai_ipmc_group->set_ipmc_groups_attribute(
      object_count, object_id, attr_list, mode, object_statuses);
}

sai_status_t mock_get_ipmc_groups_attribute(
    _In_ uint32_t object_count, _In_ const sai_object_id_t* object_id,
    _In_ const uint32_t* attr_count, _Inout_ sai_attribute_t** attr_list,
    _In_ sai_bulk_op_error_mode_t mode, _Out_ sai_status_t* object_statuses) {
  return mock_sai_ipmc_group->get_ipmc_groups_attribute(
      object_count, object_id, attr_count, attr_list, mode, object_statuses);
}
