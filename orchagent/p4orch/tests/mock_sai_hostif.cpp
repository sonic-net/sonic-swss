#include "mock_sai_hostif.h"

MockSaiHostif *mock_sai_hostif;

sai_status_t mock_create_hostif_trap_group(_Out_ sai_object_id_t *hostif_trap_group_id, _In_ sai_object_id_t switch_id,
                                           _In_ uint32_t attr_count, _In_ const sai_attribute_t *attr_list)
{
    return mock_sai_hostif->create_hostif_trap_group(hostif_trap_group_id, switch_id, attr_count, attr_list);
}

sai_status_t mock_set_hostif_trap_group_attribute(_In_ sai_object_id_t hostif_trap_group_id,
                                                  _In_ const sai_attribute_t *attr)
{
    return mock_sai_hostif->set_hostif_trap_group_attribute(hostif_trap_group_id, attr);
}

sai_status_t mock_remove_hostif_trap_group(_In_ const sai_object_id_t hostif_trap_group_id)
{
    return mock_sai_hostif->remove_hostif_trap_group(hostif_trap_group_id);
}

sai_status_t mock_create_hostif_table_entry(_Out_ sai_object_id_t *hostif_table_entry_id,
                                            _In_ sai_object_id_t switch_id, _In_ uint32_t attr_count,
                                            _In_ const sai_attribute_t *attr_list)
{
    return mock_sai_hostif->create_hostif_table_entry(hostif_table_entry_id, switch_id, attr_count, attr_list);
}

sai_status_t mock_remove_hostif_table_entry(_In_ const sai_object_id_t hostif_table_entry_id)
{
    return mock_sai_hostif->remove_hostif_table_entry(hostif_table_entry_id);
}

sai_status_t mock_create_hostif_user_defined_trap(_Out_ sai_object_id_t *hostif_user_defined_trap_id,
                                                  _In_ sai_object_id_t switch_id, _In_ uint32_t attr_count,
                                                  _In_ const sai_attribute_t *attr_list)
{
    return mock_sai_hostif->create_hostif_user_defined_trap(hostif_user_defined_trap_id, switch_id, attr_count,
                                                            attr_list);
}

sai_status_t mock_remove_hostif_user_defined_trap(_In_ const sai_object_id_t hostif_user_defined_trap_id)
{
    return mock_sai_hostif->remove_hostif_user_defined_trap(hostif_user_defined_trap_id);
}

sai_status_t mock_create_hostif_trap(_Out_ sai_object_id_t *hostif_trap_id, _In_ sai_object_id_t switch_id,
                                     _In_ uint32_t attr_count, _In_ const sai_attribute_t *attr_list)
{
    return mock_sai_hostif->create_hostif_trap(hostif_trap_id, switch_id, attr_count, attr_list);
}

sai_status_t mock_remove_hostif_trap(_In_ const sai_object_id_t hostif_trap_id)
{
    return mock_sai_hostif->remove_hostif_trap(hostif_trap_id);
}

sai_status_t mock_create_hostif(_Out_ sai_object_id_t *hostif_id, _In_ sai_object_id_t switch_id,
                                _In_ uint32_t attr_count, _In_ const sai_attribute_t *attr_list)
{
    return mock_sai_hostif->create_hostif(hostif_id, switch_id, attr_count, attr_list);
}

sai_status_t mock_remove_hostif(_In_ const sai_object_id_t hostif_id)
{
    return mock_sai_hostif->remove_hostif(hostif_id);
}
