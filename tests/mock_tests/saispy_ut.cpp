#include "gtest/gtest.h"
#include "saispy.h"

#include "sai.h"

TEST(SaiSpy, CURD)
{
    auto acl_api = std::make_shared<sai_acl_api_t>();

    acl_api->create_acl_table = [](sai_object_id_t *oid, sai_object_id_t, uint32_t,
                                   const sai_attribute_t *) {
        *oid = 1;
        return (sai_status_t)SAI_STATUS_SUCCESS;
    };

    acl_api->remove_acl_table = [](sai_object_id_t oid) {
        return (sai_status_t)(oid == 2 ? SAI_STATUS_SUCCESS : SAI_STATUS_FAILURE);
    };

    acl_api->set_acl_table_attribute = [](sai_object_id_t oid,
                                          const sai_attribute_t *) {
        return (sai_status_t)(oid == 3 ? SAI_STATUS_SUCCESS : SAI_STATUS_FAILURE);
    };

    acl_api->get_acl_table_attribute = [](sai_object_id_t oid, uint32_t,
                                          sai_attribute_t *) {
        return (sai_status_t)(oid == 4 ? SAI_STATUS_SUCCESS : SAI_STATUS_FAILURE);
    };

    sai_object_id_t oid;

    auto status = acl_api->create_acl_table(&oid, 1, 0, nullptr);
    ASSERT_EQ(oid, 1);
    ASSERT_EQ(status, SAI_STATUS_SUCCESS);

    status = acl_api->remove_acl_table(2);
    ASSERT_EQ(status, SAI_STATUS_SUCCESS);

    status = acl_api->set_acl_table_attribute(3, nullptr);
    ASSERT_EQ(status, SAI_STATUS_SUCCESS);

    status = acl_api->get_acl_table_attribute(4, 0, nullptr);
    ASSERT_EQ(status, SAI_STATUS_SUCCESS);

    sai_object_id_t exp_oid_1 = 100;
    sai_object_id_t exp_oid_2 = 200;

    auto x = SpyOn<0, offsetof(sai_acl_api_t, create_acl_table)>(&acl_api.get()->create_acl_table);
    x->callFake([&](sai_object_id_t *oid, sai_object_id_t, uint32_t, const sai_attribute_t *) -> sai_status_t {
        *oid = exp_oid_1;
        return (sai_status_t)SAI_STATUS_SUCCESS;
    });

    auto y = SpyOn<0, offsetof(sai_acl_api_t, remove_acl_table)>(&acl_api.get()->remove_acl_table);
    y->callFake([&](sai_object_id_t oid) -> sai_status_t {
        return (sai_status_t)(oid == exp_oid_2 ? SAI_STATUS_SUCCESS : SAI_STATUS_FAILURE);
    });

    auto w = SpyOn<0, offsetof(sai_acl_api_t, set_acl_table_attribute)>(&acl_api.get()->set_acl_table_attribute);
    w->callFake([&](sai_object_id_t oid, const sai_attribute_t *) -> sai_status_t {
        return (sai_status_t)(oid == exp_oid_2 ? SAI_STATUS_SUCCESS : SAI_STATUS_FAILURE);
    });

    auto z = SpyOn<0, offsetof(sai_acl_api_t, get_acl_table_attribute)>(&acl_api.get()->get_acl_table_attribute);
    z->callFake([&](sai_object_id_t oid, uint32_t, sai_attribute_t *) -> sai_status_t {
        return (sai_status_t)(oid == exp_oid_2 ? SAI_STATUS_SUCCESS : SAI_STATUS_FAILURE);
    });

    acl_api->create_acl_table(&oid, 1, 0, nullptr);
    ASSERT_EQ(oid, exp_oid_1);

    status = acl_api->remove_acl_table(exp_oid_2);
    ASSERT_EQ(status, SAI_STATUS_SUCCESS);

    status = acl_api->set_acl_table_attribute(exp_oid_2, nullptr);
    ASSERT_EQ(status, SAI_STATUS_SUCCESS);

    status = acl_api->get_acl_table_attribute(exp_oid_2, 0, nullptr);
    ASSERT_EQ(status, SAI_STATUS_SUCCESS);
}

TEST(SaiSpy, Same_Function_Signature_In_Same_API_Table)
{
    auto acl_api_1 = std::make_shared<sai_acl_api_t>();
    acl_api_1->create_acl_table = [](sai_object_id_t *oid, sai_object_id_t, uint32_t,
                                     const sai_attribute_t *) {
        *oid = 1;
        return (sai_status_t)SAI_STATUS_SUCCESS;
    };

    acl_api_1->create_acl_entry = [](sai_object_id_t *oid, sai_object_id_t, uint32_t,
                                     const sai_attribute_t *) {
        *oid = 2;
        return (sai_status_t)SAI_STATUS_SUCCESS;
    };

    sai_object_id_t oid;

    acl_api_1->create_acl_table(&oid, 1, 0, nullptr);
    ASSERT_EQ(oid, 1);

    acl_api_1->create_acl_entry(&oid, 1, 0, nullptr);
    ASSERT_EQ(oid, 2);

    sai_object_id_t exp_oid_1 = 100;
    sai_object_id_t exp_oid_2 = 200;

    auto x = SpyOn<0, SAI_OBJECT_TYPE_ACL_TABLE>(&acl_api_1.get()->create_acl_table);
    x->callFake([&](sai_object_id_t *oid, sai_object_id_t, uint32_t, const sai_attribute_t *) -> sai_status_t {
        *oid = exp_oid_1;
        return (sai_status_t)SAI_STATUS_SUCCESS;
    });

    auto y = SpyOn<0, SAI_OBJECT_TYPE_ACL_ENTRY>(&acl_api_1.get()->create_acl_entry);
    y->callFake([&](sai_object_id_t *oid, sai_object_id_t, uint32_t, const sai_attribute_t *) -> sai_status_t {
        *oid = exp_oid_2;
        return (sai_status_t)SAI_STATUS_SUCCESS;
    });

    acl_api_1->create_acl_table(&oid, 1, 0, nullptr);
    ASSERT_EQ(oid, exp_oid_1);

    acl_api_1->create_acl_entry(&oid, 1, 0, nullptr);
    ASSERT_EQ(oid, exp_oid_2);
}

TEST(SaiSpy, Same_Function_Signature_In_Different_API_Table)
{
    auto acl_api_1 = std::make_shared<sai_acl_api_t>(); //std::shared_ptr<sai_acl_api_t>(new sai_acl_api_t());
    auto acl_api_2 = std::make_shared<sai_acl_api_t>();
    acl_api_1->create_acl_table = [](sai_object_id_t *oid, sai_object_id_t, uint32_t,
                                     const sai_attribute_t *) {
        *oid = 1;
        return (sai_status_t)SAI_STATUS_SUCCESS;
    };

    acl_api_2->create_acl_table = [](sai_object_id_t *oid, sai_object_id_t, uint32_t,
                                     const sai_attribute_t *) {
        *oid = 2;
        return (sai_status_t)SAI_STATUS_SUCCESS;
    };

    sai_object_id_t oid;

    acl_api_1->create_acl_table(&oid, 1, 0, nullptr);
    ASSERT_EQ(oid, 1);

    acl_api_2->create_acl_table(&oid, 1, 0, nullptr);
    ASSERT_EQ(oid, 2);

    sai_object_id_t exp_oid_1 = 100;
    sai_object_id_t exp_oid_2 = 200;

    auto x = SpyOn<0, SAI_OBJECT_TYPE_ACL_TABLE>(&acl_api_1.get()->create_acl_table);
    //             ^ using different number for same api table type with different instance
    x->callFake([&](sai_object_id_t *oid, sai_object_id_t, uint32_t, const sai_attribute_t *) -> sai_status_t {
        *oid = exp_oid_1;
        return (sai_status_t)SAI_STATUS_SUCCESS;
    });

    auto y = SpyOn<1, SAI_OBJECT_TYPE_ACL_TABLE>(&acl_api_2.get()->create_acl_table);
    //             ^ using different number for same api table type with different instance
    y->callFake([&](sai_object_id_t *oid, sai_object_id_t, uint32_t, const sai_attribute_t *) -> sai_status_t {
        *oid = exp_oid_2;
        return (sai_status_t)SAI_STATUS_SUCCESS;
    });

    acl_api_1->create_acl_table(&oid, 1, 0, nullptr);
    ASSERT_EQ(oid, exp_oid_1);

    acl_api_2->create_acl_table(&oid, 1, 0, nullptr);
    ASSERT_EQ(oid, exp_oid_2);
}

TEST(SaiSpy, create_switch_and_acl_table)
{
    auto acl_api = std::make_shared<sai_acl_api_t>();
    auto switch_api = std::make_shared<sai_switch_api_t>();
    acl_api->create_acl_table = [](sai_object_id_t *oid, sai_object_id_t, uint32_t,
                                   const sai_attribute_t *) {
        *oid = 1;
        return (sai_status_t)SAI_STATUS_SUCCESS;
    };

    switch_api->create_switch = [](sai_object_id_t *oid, uint32_t,
                                   const sai_attribute_t *) {
        *oid = 2;
        return (sai_status_t)SAI_STATUS_SUCCESS;
    };

    sai_object_id_t oid;

    acl_api->create_acl_table(&oid, 1, 0, nullptr);
    ASSERT_EQ(oid, 1);

    switch_api->create_switch(&oid, 0, nullptr);
    ASSERT_EQ(oid, 2);

    sai_object_id_t exp_oid_1 = 100;
    sai_object_id_t exp_oid_2 = 200;

    auto x = SpyOn<SAI_API_ACL, SAI_OBJECT_TYPE_ACL_TABLE>(&acl_api.get()->create_acl_table);
    x->callFake([&](sai_object_id_t *oid, sai_object_id_t, uint32_t, const sai_attribute_t *) -> sai_status_t {
        *oid = exp_oid_1;
        return (sai_status_t)SAI_STATUS_SUCCESS;
    });

    auto y = SpyOn<SAI_API_SWITCH, SAI_OBJECT_TYPE_SWITCH>(&switch_api.get()->create_switch);
    y->callFake([&](sai_object_id_t *oid, uint32_t, const sai_attribute_t *) -> sai_status_t {
        *oid = exp_oid_2;
        return (sai_status_t)SAI_STATUS_SUCCESS;
    });

    acl_api->create_acl_table(&oid, 1, 0, nullptr);
    ASSERT_EQ(oid, exp_oid_1);

    switch_api->create_switch(&oid, 0, nullptr);
    ASSERT_EQ(oid, exp_oid_2);
}

TEST(SaiSpy, Destructor)
{
    auto acl_api = std::make_shared<sai_acl_api_t>();

    acl_api->create_acl_table = [](sai_object_id_t *oid, sai_object_id_t, uint32_t,
                                   const sai_attribute_t *) {
        *oid = 1;
        return (sai_status_t)SAI_STATUS_SUCCESS;
    };

    sai_object_id_t oid;

    auto status = acl_api->create_acl_table(&oid, 1, 0, nullptr);
    ASSERT_EQ(status, SAI_STATUS_SUCCESS);
    ASSERT_EQ(oid, 1);

    // Setup SAI spy in a limited scope.
    {
        auto aclSpy = SpyOn<SAI_API_ACL, SAI_OBJECT_TYPE_ACL_TABLE>(&acl_api.get()->create_acl_table);
        aclSpy->callFake([&](sai_object_id_t *oid, sai_object_id_t, uint32_t, const sai_attribute_t *) -> sai_status_t {
            *oid = 2;
            return SAI_STATUS_SUCCESS;
        });

        status = acl_api->create_acl_table(&oid, 1, 0, nullptr);
        ASSERT_EQ(status, SAI_STATUS_SUCCESS);
        ASSERT_EQ(oid, 2);
    }

    // Make sure original SAI API is restored.
    status = acl_api->create_acl_table(&oid, 1, 0, nullptr);
    ASSERT_EQ(status, SAI_STATUS_SUCCESS);
    ASSERT_EQ(oid, 1);
}
