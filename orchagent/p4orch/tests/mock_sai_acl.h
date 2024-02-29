#include <gmock/gmock.h>

extern "C"
{
#include "sai.h"
#include "saiacl.h"
}

class SaiAclInterface
{
  public:
    virtual sai_status_t create_acl_table(sai_object_id_t *acl_table_id, sai_object_id_t switch_id, uint32_t attr_count,
                                          const sai_attribute_t *attr_list) = 0;
    virtual sai_status_t remove_acl_table(sai_object_id_t acl_table_id) = 0;
    virtual sai_status_t create_acl_table_group(sai_object_id_t *acl_table_group_id, sai_object_id_t switch_id,
                                                uint32_t attr_count, const sai_attribute_t *attr_list) = 0;
    virtual sai_status_t remove_acl_table_group(sai_object_id_t acl_table_group_id) = 0;
    virtual sai_status_t create_acl_table_group_member(sai_object_id_t *acl_table_group_member_id,
                                                       sai_object_id_t switch_id, uint32_t attr_count,
                                                       const sai_attribute_t *attr_list) = 0;
    virtual sai_status_t remove_acl_table_group_member(sai_object_id_t acl_table_group_member_id) = 0;

    virtual sai_status_t create_acl_entry(sai_object_id_t *acl_entry_id, sai_object_id_t switch_id, uint32_t attr_count,
                                          const sai_attribute_t *attr_list) = 0;
    virtual sai_status_t remove_acl_entry(sai_object_id_t acl_entry_id) = 0;
    virtual sai_status_t create_acl_counter(sai_object_id_t *acl_counter_id, sai_object_id_t switch_id,
                                            uint32_t attr_count, const sai_attribute_t *attr_list) = 0;
    virtual sai_status_t remove_acl_counter(sai_object_id_t acl_counter_id) = 0;
    virtual sai_status_t get_acl_counter_attribute(sai_object_id_t acl_counter_id, uint32_t attr_count,
                                                   sai_attribute_t *attr_list) = 0;
    virtual sai_status_t set_acl_entry_attribute(sai_object_id_t acl_entry_id, const sai_attribute_t *attr) = 0;
};

class MockSaiAcl : public SaiAclInterface
{
  public:
    MOCK_METHOD4(create_acl_table, sai_status_t(sai_object_id_t *acl_table_id, sai_object_id_t switch_id,
                                                uint32_t attr_count, const sai_attribute_t *attr_list));
    MOCK_METHOD1(remove_acl_table, sai_status_t(sai_object_id_t acl_table_id));
    MOCK_METHOD4(create_acl_table_group, sai_status_t(sai_object_id_t *acl_table_group_id, sai_object_id_t switch_id,
                                                      uint32_t attr_count, const sai_attribute_t *attr_list));
    MOCK_METHOD1(remove_acl_table_group, sai_status_t(sai_object_id_t acl_table_group_id));
    MOCK_METHOD4(create_acl_table_group_member,
                 sai_status_t(sai_object_id_t *acl_table_group_member_id, sai_object_id_t switch_id,
                              uint32_t attr_count, const sai_attribute_t *attr_list));
    MOCK_METHOD1(remove_acl_table_group_member, sai_status_t(sai_object_id_t acl_table_group_member_id));
    MOCK_METHOD4(create_acl_entry, sai_status_t(sai_object_id_t *acl_entry_id, sai_object_id_t switch_id,
                                                uint32_t attr_count, const sai_attribute_t *attr_list));
    MOCK_METHOD1(remove_acl_entry, sai_status_t(sai_object_id_t acl_entry_id));
    MOCK_METHOD4(create_acl_counter, sai_status_t(sai_object_id_t *acl_counter_id, sai_object_id_t switch_id,
                                                  uint32_t attr_count, const sai_attribute_t *attr_list));
    MOCK_METHOD1(remove_acl_counter, sai_status_t(sai_object_id_t acl_counter_id));
    MOCK_METHOD3(get_acl_counter_attribute,
                 sai_status_t(sai_object_id_t acl_counter_id, uint32_t attr_count, sai_attribute_t *attr_list));
    MOCK_METHOD2(set_acl_entry_attribute, sai_status_t(sai_object_id_t acl_entry_id, const sai_attribute_t *attr));
};

extern MockSaiAcl *mock_sai_acl;

sai_status_t create_acl_table(_Out_ sai_object_id_t *acl_table_id, _In_ sai_object_id_t switch_id,
                              _In_ uint32_t attr_count, _In_ const sai_attribute_t *attr_list);

sai_status_t remove_acl_table(sai_object_id_t acl_table_id);

sai_status_t create_acl_table_group(sai_object_id_t *acl_table_group_id, sai_object_id_t switch_id, uint32_t attr_count,
                                    const sai_attribute_t *attr_list);

sai_status_t remove_acl_table_group(sai_object_id_t acl_table_group_id);

sai_status_t create_acl_table_group_member(sai_object_id_t *acl_table_group_member_id, sai_object_id_t switch_id,
                                           uint32_t attr_count, const sai_attribute_t *attr_list);

sai_status_t remove_acl_table_group_member(sai_object_id_t acl_table_group_member_id);

sai_status_t create_acl_entry(sai_object_id_t *acl_entry_id, sai_object_id_t switch_id, uint32_t attr_count,
                              const sai_attribute_t *attr_list);

sai_status_t remove_acl_entry(sai_object_id_t acl_entry_id);

sai_status_t create_acl_counter(sai_object_id_t *acl_counter_id, sai_object_id_t switch_id, uint32_t attr_count,
                                const sai_attribute_t *attr_list);

sai_status_t remove_acl_counter(sai_object_id_t acl_counter_id);

sai_status_t get_acl_counter_attribute(sai_object_id_t acl_counter_id, uint32_t attr_count, sai_attribute_t *attr_list);

sai_status_t set_acl_entry_attribute(sai_object_id_t acl_entry_id, const sai_attribute_t *attr);
