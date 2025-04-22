#include "mock_sai_next_hop.h"
 
 MockSaiNextHop* mock_sai_next_hop;
 
 sai_status_t mock_create_next_hop(_Out_ sai_object_id_t* next_hop_id,
                                   _In_ sai_object_id_t switch_id,
                                   _In_ uint32_t attr_count,
                                   _In_ const sai_attribute_t* attr_list) {
   return mock_sai_next_hop->create_next_hop(next_hop_id, switch_id, attr_count,
                                             attr_list);
 }
 
 sai_status_t mock_remove_next_hop(_In_ sai_object_id_t next_hop_id) {
   return mock_sai_next_hop->remove_next_hop(next_hop_id);
 }
 
 sai_status_t mock_set_next_hop_attribute(_In_ sai_object_id_t next_hop_id,
                                          _In_ const sai_attribute_t* attr) {
   return mock_sai_next_hop->set_next_hop_attribute(next_hop_id, attr);
 }
 
 sai_status_t mock_get_next_hop_attribute(_In_ sai_object_id_t next_hop_id,
                                          _In_ uint32_t attr_count,
                                          _Inout_ sai_attribute_t* attr_list) {
   return mock_sai_next_hop->get_next_hop_attribute(next_hop_id, attr_count,
                                                    attr_list);
 }

sai_status_t mock_create_next_hops(_In_ sai_object_id_t switch_id,
                                   _In_ uint32_t object_count,
                                   _In_ const uint32_t *attr_count,
                                   _In_ const sai_attribute_t **attr_list,
                                   _In_ sai_bulk_op_error_mode_t mode,
                                   _Out_ sai_object_id_t *object_id,
                                   _Out_ sai_status_t *object_statuses) {
  return mock_sai_next_hop->create_next_hops(switch_id, object_count, attr_count,
                                         attr_list, mode, object_id,
                                         object_statuses);
}

sai_status_t mock_remove_next_hops(_In_ uint32_t object_count,
                                   _In_ const sai_object_id_t *object_id,
                                   _In_ sai_bulk_op_error_mode_t mode,
                                   _Out_ sai_status_t *object_statuses) {
  return mock_sai_next_hop->remove_next_hops(object_count, object_id, mode,
                                         object_statuses);
}
