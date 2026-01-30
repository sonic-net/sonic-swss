#include "mock_sai_l2mc_group.h"
MockSaiL2mcGroup* mock_sai_l2mc_group;
sai_status_t mock_create_l2mc_group(
    _Out_ sai_object_id_t* l2mc_group_id,
    _In_ sai_object_id_t switch_id,
    _In_ uint32_t attr_count,
    _In_ const sai_attribute_t* attr_list) {
  return mock_sai_l2mc_group->create_l2mc_group(
      l2mc_group_id, switch_id, attr_count, attr_list);
}
sai_status_t mock_remove_l2mc_group(
    _In_ sai_object_id_t l2mc_group_id) {
  return mock_sai_l2mc_group->remove_l2mc_group(l2mc_group_id);
}
sai_status_t mock_set_l2mc_group_attribute(
    _In_ sai_object_id_t l2mc_group_id,
    _In_ const sai_attribute_t* attr) {
  return mock_sai_l2mc_group->set_l2mc_group_attribute(l2mc_group_id, attr);
}
sai_status_t mock_get_l2mc_group_attribute(
    _In_ sai_object_id_t l2mc_group_id,
    _In_ uint32_t attr_count,
    _Inout_ sai_attribute_t* attr_list) {
  return mock_sai_l2mc_group->get_l2mc_group_attribute(
      l2mc_group_id, attr_count, attr_list);
}
sai_status_t mock_create_l2mc_group_member(
    _Out_ sai_object_id_t* l2mc_group_member_id,
    _In_ sai_object_id_t switch_id,
    _In_ uint32_t attr_count,
    _In_ const sai_attribute_t* attr_list) {
  return mock_sai_l2mc_group->create_l2mc_group_member(
      l2mc_group_member_id, switch_id, attr_count, attr_list);
}
sai_status_t mock_remove_l2mc_group_member(
    _In_ sai_object_id_t l2mc_group_member_id) {
  return mock_sai_l2mc_group->remove_l2mc_group_member(l2mc_group_member_id);
}
sai_status_t mock_set_l2mc_group_member_attribute(
    _In_ sai_object_id_t l2mc_group_member_id,
    _In_ const sai_attribute_t* attr) {
  return mock_sai_l2mc_group->set_l2mc_group_member_attribute(
      l2mc_group_member_id, attr);
}
sai_status_t mock_get_l2mc_group_member_attribute(
    _In_ sai_object_id_t l2mc_group_member_id,
    _In_ uint32_t attr_count,
    _Inout_ sai_attribute_t* attr_list) {
  return mock_sai_l2mc_group->get_l2mc_group_member_attribute(
      l2mc_group_member_id, attr_count, attr_list);
}
