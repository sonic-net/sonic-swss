#include "mock_sai_my_mac.h"

MockSaiMyMac* mock_sai_my_mac;

sai_status_t mock_create_my_mac(_Out_ sai_object_id_t* my_mac_id,
                                _In_ sai_object_id_t switch_id,
                                _In_ uint32_t attr_count,
                                _In_ const sai_attribute_t* attr_list) {
  return mock_sai_my_mac->create_my_mac(my_mac_id, switch_id, attr_count,
                                        attr_list);
}

sai_status_t mock_remove_my_mac(_In_ sai_object_id_t my_mac_id) {
  return mock_sai_my_mac->remove_my_mac(my_mac_id);
}