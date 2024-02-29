#include "mock_sai_acl.h"

MockSaiAcl *mock_sai_acl;

sai_status_t create_acl_table(sai_object_id_t *acl_table_id, sai_object_id_t switch_id, uint32_t attr_count,
                              const sai_attribute_t *attr_list)
{
    return mock_sai_acl->create_acl_table(acl_table_id, switch_id, attr_count, attr_list);
}

sai_status_t remove_acl_table(sai_object_id_t acl_table_id)
{
    return mock_sai_acl->remove_acl_table(acl_table_id);
}

sai_status_t create_acl_table_group(sai_object_id_t *acl_table_group_id, sai_object_id_t switch_id, uint32_t attr_count,
                                    const sai_attribute_t *attr_list)
{
    return mock_sai_acl->create_acl_table_group(acl_table_group_id, switch_id, attr_count, attr_list);
}

sai_status_t remove_acl_table_group(sai_object_id_t acl_table_group_id)
{
    return mock_sai_acl->remove_acl_table_group(acl_table_group_id);
}

sai_status_t create_acl_table_group_member(sai_object_id_t *acl_table_group_member_id, sai_object_id_t switch_id,
                                           uint32_t attr_count, const sai_attribute_t *attr_list)
{
    return mock_sai_acl->create_acl_table_group_member(acl_table_group_member_id, switch_id, attr_count, attr_list);
}

sai_status_t remove_acl_table_group_member(sai_object_id_t acl_table_group_member_id)
{
    return mock_sai_acl->remove_acl_table_group_member(acl_table_group_member_id);
}

sai_status_t create_acl_entry(sai_object_id_t *acl_entry_id, sai_object_id_t switch_id, uint32_t attr_count,
                              const sai_attribute_t *attr_list)
{
    return mock_sai_acl->create_acl_entry(acl_entry_id, switch_id, attr_count, attr_list);
}

sai_status_t remove_acl_entry(sai_object_id_t acl_entry_id)
{
    return mock_sai_acl->remove_acl_entry(acl_entry_id);
}

sai_status_t create_acl_counter(sai_object_id_t *acl_counter_id, sai_object_id_t switch_id, uint32_t attr_count,
                                const sai_attribute_t *attr_list)
{
    return mock_sai_acl->create_acl_counter(acl_counter_id, switch_id, attr_count, attr_list);
}

sai_status_t remove_acl_counter(sai_object_id_t acl_counter_id)
{
    return mock_sai_acl->remove_acl_counter(acl_counter_id);
}

sai_status_t get_acl_counter_attribute(sai_object_id_t acl_counter_id, uint32_t attr_count, sai_attribute_t *attr_list)
{
    return mock_sai_acl->get_acl_counter_attribute(acl_counter_id, attr_count, attr_list);
}

sai_status_t set_acl_entry_attribute(sai_object_id_t acl_entry_id, const sai_attribute_t *attr)
{
    return mock_sai_acl->set_acl_entry_attribute(acl_entry_id, attr);
}
