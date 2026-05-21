#define private public
#undef private
#include "directory.h"
#define protected public
#include "orch.h"
#undef protected

#include "ut_helper.h"

#define protected public
#include "nhgbase.h"
#undef protected

#define private public
#include "neighorch.h"
#undef private


#include "mock_orchagent_main.h"
#include "mock_sai_api.h"
#include "mock_orch_test.h"
#include "nhgorch.h"
#include "protnhg.h"

#include "gtest/gtest.h"

#include <string>
#include <vector>

using namespace mock_orch_test;
using namespace std;

using ::testing::_;
using ::testing::Return;

DEFINE_SAI_GENERIC_APIS_MOCK(next_hop_group, next_hop_group, next_hop_group_member)

EXTERN_MOCK_FNS

namespace protnhg_test
{
    static uint64_t nhg_oid_counter = 0x5000000;
    static uint64_t nhgm_oid_counter = 0x6000000;
    static uint64_t nh_oid_counter = 0x8000000;

    static void registerNextHop(const NextHopKey &nh,
                                sai_object_id_t nh_id = SAI_NULL_OBJECT_ID)
    {
        if (nh_id == SAI_NULL_OBJECT_ID)
        {
            nh_id = ++nh_oid_counter;
        }
        gNeighOrch->m_syncdNextHops[nh] = { nh_id, 0, 0 };
    }

    static void unregisterNextHop(const NextHopKey &nh)
    {
        gNeighOrch->m_syncdNextHops.erase(nh);
    }

    class ProtNhgTest : public MockOrchTest
    {
    protected:
        void PostSetUp() override
        {
            INIT_SAI_API_MOCK(next_hop_group);
            MockSaiApis();

            ON_CALL(*mock_sai_next_hop_group_api,
                    create_next_hop_group(_, _, _, _))
                .WillByDefault(
                    [](sai_object_id_t *id, sai_object_id_t,
                       uint32_t, const sai_attribute_t *) {
                        *id = ++nhg_oid_counter;
                        return SAI_STATUS_SUCCESS;
                    });

            ON_CALL(*mock_sai_next_hop_group_api,
                    remove_next_hop_group(_))
                .WillByDefault(Return(SAI_STATUS_SUCCESS));

            ON_CALL(*mock_sai_next_hop_group_api,
                    set_next_hop_group_attribute(_, _))
                .WillByDefault(Return(SAI_STATUS_SUCCESS));

            ON_CALL(*mock_sai_next_hop_group_api,
                    create_next_hop_group_member(_, _, _, _))
                .WillByDefault(
                    [](sai_object_id_t *id, sai_object_id_t,
                       uint32_t, const sai_attribute_t *) {
                        *id = ++nhgm_oid_counter;
                        return SAI_STATUS_SUCCESS;
                    });

            ON_CALL(*mock_sai_next_hop_group_api,
                    remove_next_hop_group_member(_))
                .WillByDefault(Return(SAI_STATUS_SUCCESS));

            ON_CALL(*mock_sai_next_hop_group_api,
                    set_next_hop_group_member_attribute(_, _))
                .WillByDefault(Return(SAI_STATUS_SUCCESS));

            ON_CALL(*mock_sai_next_hop_group_api,
                    create_next_hop_group_members(_, _, _, _, _, _, _))
                .WillByDefault(
                    [](sai_object_id_t, uint32_t count,
                       const uint32_t *, const sai_attribute_t **,
                       sai_bulk_op_error_mode_t,
                       sai_object_id_t *ids, sai_status_t *statuses) {
                        for (uint32_t i = 0; i < count; i++)
                        {
                            ids[i] = ++nhgm_oid_counter;
                            statuses[i] = SAI_STATUS_SUCCESS;
                        }
                        return SAI_STATUS_SUCCESS;
                    });

            ON_CALL(*mock_sai_next_hop_group_api,
                    remove_next_hop_group_members(_, _, _, _))
                .WillByDefault(
                    [](uint32_t count, const sai_object_id_t *,
                       sai_bulk_op_error_mode_t, sai_status_t *statuses) {
                        for (uint32_t i = 0; i < count; i++)
                        {
                            statuses[i] = SAI_STATUS_SUCCESS;
                        }
                        return SAI_STATUS_SUCCESS;
                    });
        }

        void PreTearDown() override
        {
            RestoreSaiApis();
            DEINIT_SAI_API_MOCK(next_hop_group);
        }
    };

    TEST_F(ProtNhgTest, CreateAndRemoveProtNhg)
    {
        string key = "prot_nhg_1";
        NextHopKey primary_nh(IpAddress("10.0.0.1"), string("Ethernet0"));
        NextHopKey standby_nh(IpAddress("10.0.0.100"), string("Ethernet4"));

        registerNextHop(primary_nh);
        registerNextHop(standby_nh);

        vector<NextHopKey> primaries = {primary_nh};

        ASSERT_TRUE(gNhgOrch->createProtNhg(key, primaries, standby_nh));
        EXPECT_TRUE(gNhgOrch->hasProtNhg(key));
        EXPECT_NE(gNhgOrch->getProtNhgId(key), SAI_NULL_OBJECT_ID);

        ASSERT_TRUE(gNhgOrch->removeProtNhg(key));
        EXPECT_FALSE(gNhgOrch->hasProtNhg(key));
        EXPECT_EQ(gNhgOrch->getProtNhgId(key), SAI_NULL_OBJECT_ID);

        unregisterNextHop(primary_nh);
        unregisterNextHop(standby_nh);
    }

    TEST_F(ProtNhgTest, CreateDuplicateProtNhgFails)
    {
        string key = "prot_nhg_dup";
        NextHopKey primary_nh(IpAddress("10.0.0.1"), string("Ethernet0"));
        NextHopKey standby_nh(IpAddress("10.0.0.100"), string("Ethernet4"));
        vector<NextHopKey> primaries = {primary_nh};

        registerNextHop(primary_nh);
        registerNextHop(standby_nh);

        ASSERT_TRUE(gNhgOrch->createProtNhg(key, primaries, standby_nh));
        EXPECT_FALSE(gNhgOrch->createProtNhg(key, primaries, standby_nh));

        ASSERT_TRUE(gNhgOrch->removeProtNhg(key));
        unregisterNextHop(primary_nh);
        unregisterNextHop(standby_nh);
    }

    TEST_F(ProtNhgTest, CreateProtNhgEmptyPrimariesFails)
    {
        NextHopKey standby_nh(IpAddress("10.0.0.100"), string("Ethernet4"));
        vector<NextHopKey> primaries;

        EXPECT_FALSE(gNhgOrch->createProtNhg("empty", primaries, standby_nh));
        EXPECT_FALSE(gNhgOrch->hasProtNhg("empty"));
    }

    TEST_F(ProtNhgTest, RemoveNonExistentProtNhgFails)
    {
        EXPECT_FALSE(gNhgOrch->removeProtNhg("does_not_exist"));
    }

    TEST_F(ProtNhgTest, RemoveReferencedProtNhgFails)
    {
        string key = "prot_nhg_ref";
        NextHopKey primary_nh(IpAddress("10.0.0.1"), string("Ethernet0"));
        NextHopKey standby_nh(IpAddress("10.0.0.100"), string("Ethernet4"));
        vector<NextHopKey> primaries = {primary_nh};

        registerNextHop(primary_nh);
        registerNextHop(standby_nh);

        ASSERT_TRUE(gNhgOrch->createProtNhg(key, primaries, standby_nh));

        gNhgOrch->incProtNhgRefCount(key);
        EXPECT_FALSE(gNhgOrch->removeProtNhg(key));
        EXPECT_TRUE(gNhgOrch->hasProtNhg(key));

        gNhgOrch->decProtNhgRefCount(key);
        EXPECT_TRUE(gNhgOrch->removeProtNhg(key));
        EXPECT_FALSE(gNhgOrch->hasProtNhg(key));

        unregisterNextHop(primary_nh);
        unregisterNextHop(standby_nh);
    }

    TEST_F(ProtNhgTest, GetProtNhgMembers)
    {
        string key = "prot_nhg_members";
        NextHopKey primary_nh(IpAddress("10.0.0.1"), string("Ethernet0"));
        NextHopKey standby_nh(IpAddress("10.0.0.100"), string("Ethernet4"));
        vector<NextHopKey> primaries = {primary_nh};

        registerNextHop(primary_nh);
        registerNextHop(standby_nh);

        ASSERT_TRUE(gNhgOrch->createProtNhg(key, primaries, standby_nh));

        const ProtNhg &nhg = gNhgOrch->getProtNhg(key);
        EXPECT_NE(nhg.getId(), SAI_NULL_OBJECT_ID);

        const ProtNhgMember *standby = nhg.getStandbyMember();
        ASSERT_NE(standby, nullptr);
        EXPECT_EQ(standby->getRole(), ProtNhgRole::STANDBY);

        auto primary_out = nhg.getPrimaryMembers();
        ASSERT_EQ(primary_out.size(), 1u);
        EXPECT_EQ(primary_out[0]->getRole(), ProtNhgRole::PRIMARY);

        ASSERT_TRUE(gNhgOrch->removeProtNhg(key));
        unregisterNextHop(primary_nh);
        unregisterNextHop(standby_nh);
    }

    TEST_F(ProtNhgTest, MultiplePrimaryMembers)
    {
        string key = "prot_nhg_multi";
        NextHopKey primary1(IpAddress("10.0.0.1"), string("Ethernet0"));
        NextHopKey primary2(IpAddress("10.0.0.2"), string("Ethernet0"));
        NextHopKey standby_nh(IpAddress("10.0.0.100"), string("Ethernet4"));
        vector<NextHopKey> primaries = {primary1, primary2};

        registerNextHop(primary1);
        registerNextHop(primary2);
        registerNextHop(standby_nh);

        ASSERT_TRUE(gNhgOrch->createProtNhg(key, primaries, standby_nh));

        const ProtNhg &nhg = gNhgOrch->getProtNhg(key);
        auto primary_out = nhg.getPrimaryMembers();
        EXPECT_EQ(primary_out.size(), 2u);

        ASSERT_TRUE(gNhgOrch->removeProtNhg(key));
        unregisterNextHop(primary1);
        unregisterNextHop(primary2);
        unregisterNextHop(standby_nh);
    }

    TEST_F(ProtNhgTest, SetAdminRole)
    {
        string key = "prot_nhg_admin";
        NextHopKey primary_nh(IpAddress("10.0.0.1"), string("Ethernet0"));
        NextHopKey standby_nh(IpAddress("10.0.0.100"), string("Ethernet4"));
        vector<NextHopKey> primaries = {primary_nh};

        registerNextHop(primary_nh);
        registerNextHop(standby_nh);

        ASSERT_TRUE(gNhgOrch->createProtNhg(key, primaries, standby_nh));

        EXPECT_CALL(*mock_sai_next_hop_group_api,
                    set_next_hop_group_attribute(_, _))
            .Times(1)
            .WillOnce(Return(SAI_STATUS_SUCCESS));

        EXPECT_TRUE(gNhgOrch->setProtNhgAdminRole(
            key, SAI_NEXT_HOP_GROUP_MEMBER_CONFIGURED_ROLE_PRIMARY));

        ASSERT_TRUE(gNhgOrch->removeProtNhg(key));
        unregisterNextHop(primary_nh);
        unregisterNextHop(standby_nh);
    }

    TEST_F(ProtNhgTest, SetAdminRoleNonExistentFails)
    {
        EXPECT_FALSE(gNhgOrch->setProtNhgAdminRole(
            "no_such_key", SAI_NEXT_HOP_GROUP_MEMBER_CONFIGURED_ROLE_PRIMARY));
    }

    TEST_F(ProtNhgTest, SetAdminRoleSaiFailure)
    {
        string key = "prot_nhg_admin_fail";
        NextHopKey primary_nh(IpAddress("10.0.0.1"), string("Ethernet0"));
        NextHopKey standby_nh(IpAddress("10.0.0.100"), string("Ethernet4"));
        vector<NextHopKey> primaries = {primary_nh};

        registerNextHop(primary_nh);
        registerNextHop(standby_nh);

        ASSERT_TRUE(gNhgOrch->createProtNhg(key, primaries, standby_nh));

        EXPECT_CALL(*mock_sai_next_hop_group_api,
                    set_next_hop_group_attribute(_, _))
            .Times(1)
            .WillOnce(Return(SAI_STATUS_FAILURE));

        EXPECT_FALSE(gNhgOrch->setProtNhgAdminRole(
            key, SAI_NEXT_HOP_GROUP_MEMBER_CONFIGURED_ROLE_PRIMARY));

        ASSERT_TRUE(gNhgOrch->removeProtNhg(key));
        unregisterNextHop(primary_nh);
        unregisterNextHop(standby_nh);
    }

    TEST_F(ProtNhgTest, SetMonitoredObjectOnStandbyMember)
    {
        string key = "prot_nhg_monitor";
        NextHopKey primary_nh(IpAddress("10.0.0.1"), string("Ethernet0"));
        NextHopKey standby_nh(IpAddress("10.0.0.100"), string("Ethernet4"));
        sai_object_id_t session_oid = 0xABCD;
        vector<NextHopKey> primaries = {primary_nh};


        registerNextHop(primary_nh);
        registerNextHop(standby_nh);

        ASSERT_TRUE(gNhgOrch->createProtNhg(key, primaries, standby_nh));
        EXPECT_TRUE(gNhgOrch->setProtNhgMonitoredObject(key, standby_nh, session_oid));

        ASSERT_TRUE(gNhgOrch->removeProtNhg(key));
        unregisterNextHop(primary_nh);
        unregisterNextHop(standby_nh);
    }

    TEST_F(ProtNhgTest, SetMonitoredObjectNonExistentNhgFails)
    {
        NextHopKey nh(IpAddress("10.0.0.1"), string("Ethernet0"));
        EXPECT_FALSE(gNhgOrch->setProtNhgMonitoredObject("no_such_key", nh, 0xABCD));
    }

    TEST_F(ProtNhgTest, SetMonitoredObjectNonExistentMemberFails)
    {
        string key = "prot_nhg_monitor_bad_mbr";
        NextHopKey primary_nh(IpAddress("10.0.0.1"), string("Ethernet0"));
        NextHopKey standby_nh(IpAddress("10.0.0.100"), string("Ethernet4"));
        NextHopKey unknown_nh(IpAddress("10.0.0.99"), string("Ethernet0"));
        vector<NextHopKey> primaries = {primary_nh};

        registerNextHop(primary_nh);
        registerNextHop(standby_nh);

        ASSERT_TRUE(gNhgOrch->createProtNhg(key, primaries, standby_nh));
        EXPECT_FALSE(gNhgOrch->setProtNhgMonitoredObject(key, unknown_nh, 0xABCD));

        ASSERT_TRUE(gNhgOrch->removeProtNhg(key));
        unregisterNextHop(primary_nh);
        unregisterNextHop(standby_nh);
    }

    TEST_F(ProtNhgTest, ObservedRoleNonExistentNhgFails)
    {
        NextHopKey nh(IpAddress("10.0.0.1"), string("Ethernet0"));
        sai_next_hop_group_member_observed_role_t role;
        EXPECT_FALSE(gNhgOrch->getProtNhgMemberObservedRole("no_such_key", nh, role));
    }

    TEST_F(ProtNhgTest, AllObservedRolesNonExistentNhgFails)
    {
        map<NextHopKey, sai_next_hop_group_member_observed_role_t> roles;
        EXPECT_FALSE(gNhgOrch->getProtNhgAllObservedRoles("no_such_key", roles));
    }

    TEST_F(ProtNhgTest, CreateSaiFailure)
    {
        EXPECT_CALL(*mock_sai_next_hop_group_api,
                    create_next_hop_group(_, _, _, _))
            .WillOnce(Return(SAI_STATUS_FAILURE));

        string key = "prot_nhg_sai_fail";
        NextHopKey primary_nh(IpAddress("10.0.0.1"), string("Ethernet0"));
        NextHopKey standby_nh(IpAddress("10.0.0.100"), string("Ethernet4"));
        vector<NextHopKey> primaries = {primary_nh};

        registerNextHop(primary_nh);
        registerNextHop(standby_nh);

        EXPECT_FALSE(gNhgOrch->createProtNhg(key, primaries, standby_nh));
        EXPECT_FALSE(gNhgOrch->hasProtNhg(key));

        unregisterNextHop(primary_nh);
        unregisterNextHop(standby_nh);
    }

    TEST_F(ProtNhgTest, HasAndGetIdForNonExistentKey)
    {
        EXPECT_FALSE(gNhgOrch->hasProtNhg("ghost"));
        EXPECT_EQ(gNhgOrch->getProtNhgId("ghost"), SAI_NULL_OBJECT_ID);
    }

    TEST_F(ProtNhgTest, SyncAlreadySynced)
    {
        string key = "prot_nhg_double_sync";
        NextHopKey primary_nh(IpAddress("10.0.0.1"), string("Ethernet0"));
        NextHopKey standby_nh(IpAddress("10.0.0.100"), string("Ethernet4"));
        vector<NextHopKey> primaries = {primary_nh};

        registerNextHop(primary_nh);
        registerNextHop(standby_nh);

        ASSERT_TRUE(gNhgOrch->createProtNhg(key, primaries, standby_nh));

        auto &nhg = const_cast<ProtNhg&>(gNhgOrch->getProtNhg(key));
        EXPECT_TRUE(nhg.sync());

        ASSERT_TRUE(gNhgOrch->removeProtNhg(key));
        unregisterNextHop(primary_nh);
        unregisterNextHop(standby_nh);
    }

    TEST_F(ProtNhgTest, SyncMembersFailure)
    {
        EXPECT_CALL(*mock_sai_next_hop_group_api,
                    create_next_hop_group_members(_, _, _, _, _, _, _))
            .WillOnce(
                [](sai_object_id_t, uint32_t count,
                   const uint32_t *, const sai_attribute_t **,
                   sai_bulk_op_error_mode_t,
                   sai_object_id_t *ids, sai_status_t *statuses) {
                    for (uint32_t i = 0; i < count; i++)
                    {
                        ids[i] = SAI_NULL_OBJECT_ID;
                        statuses[i] = SAI_STATUS_FAILURE;
                    }
                    return SAI_STATUS_FAILURE;
                });

        string key = "prot_nhg_sync_mbr_fail";
        NextHopKey primary_nh(IpAddress("10.0.0.1"), string("Ethernet0"));
        NextHopKey standby_nh(IpAddress("10.0.0.100"), string("Ethernet4"));
        vector<NextHopKey> primaries = {primary_nh};

        registerNextHop(primary_nh);
        registerNextHop(standby_nh);

        EXPECT_FALSE(gNhgOrch->createProtNhg(key, primaries, standby_nh));
        EXPECT_FALSE(gNhgOrch->hasProtNhg(key));

        unregisterNextHop(primary_nh);
        unregisterNextHop(standby_nh);
    }

    TEST_F(ProtNhgTest, SetAdminRoleUnsyncedNhg)
    {
        NextHopKey primary_nh(IpAddress("10.0.0.1"), string("Ethernet0"));
        NextHopKey standby_nh(IpAddress("10.0.0.100"), string("Ethernet4"));
        vector<NextHopKey> primaries = {primary_nh};

        ProtNhg nhg("unsynced_nhg", primaries, standby_nh);
        EXPECT_FALSE(nhg.setAdminRole(
            SAI_NEXT_HOP_GROUP_MEMBER_CONFIGURED_ROLE_PRIMARY));
    }

    TEST_F(ProtNhgTest, UpdateMonitoredObjectUnsynced)
    {
        NextHopKey primary_nh(IpAddress("10.0.0.1"), string("Ethernet0"));
        NextHopKey standby_nh(IpAddress("10.0.0.100"), string("Ethernet4"));
        vector<NextHopKey> primaries = {primary_nh};

        ProtNhg nhg("unsynced_mon", primaries, standby_nh);
        EXPECT_TRUE(nhg.updateMemberMonitoredObject(standby_nh, 0xABCD));
    }

    TEST_F(ProtNhgTest, UpdateMonitoredObjectSaiFailure)
    {
        string key = "prot_nhg_mon_sai_fail";
        NextHopKey primary_nh(IpAddress("10.0.0.1"), string("Ethernet0"));
        NextHopKey standby_nh(IpAddress("10.0.0.100"), string("Ethernet4"));
        vector<NextHopKey> primaries = {primary_nh};

        registerNextHop(primary_nh);
        registerNextHop(standby_nh);

        ASSERT_TRUE(gNhgOrch->createProtNhg(key, primaries, standby_nh));

        EXPECT_CALL(*mock_sai_next_hop_group_api,
                    set_next_hop_group_member_attribute(_, _))
            .WillOnce(Return(SAI_STATUS_FAILURE));

        EXPECT_FALSE(gNhgOrch->setProtNhgMonitoredObject(key, standby_nh, 0xABCD));

        ASSERT_TRUE(gNhgOrch->removeProtNhg(key));
        unregisterNextHop(primary_nh);
        unregisterNextHop(standby_nh);
    }

    TEST_F(ProtNhgTest, ObservedRoleUnsyncedMember)
    {
        string key = "prot_nhg_obs_unsynced";
        NextHopKey primary_nh(IpAddress("10.0.0.1"), string("Ethernet0"));
        NextHopKey standby_nh(IpAddress("10.0.0.100"), string("Ethernet4"));
        vector<NextHopKey> primaries = {primary_nh};

        registerNextHop(standby_nh);

        ASSERT_TRUE(gNhgOrch->createProtNhg(key, primaries, standby_nh));

        sai_next_hop_group_member_observed_role_t role;
        EXPECT_FALSE(gNhgOrch->getProtNhgMemberObservedRole(
            key, primary_nh, role));

        ASSERT_TRUE(gNhgOrch->removeProtNhg(key));
        unregisterNextHop(standby_nh);
    }

    TEST_F(ProtNhgTest, ObservedRoleSuccess)
    {
        string key = "prot_nhg_obs_ok";
        NextHopKey primary_nh(IpAddress("10.0.0.1"), string("Ethernet0"));
        NextHopKey standby_nh(IpAddress("10.0.0.100"), string("Ethernet4"));
        vector<NextHopKey> primaries = {primary_nh};

        registerNextHop(primary_nh);
        registerNextHop(standby_nh);

        ASSERT_TRUE(gNhgOrch->createProtNhg(key, primaries, standby_nh));

        auto old_get_fn =
            ut_sai_next_hop_group_api.get_next_hop_group_member_attribute;
        ut_sai_next_hop_group_api.get_next_hop_group_member_attribute =
            [](sai_object_id_t, uint32_t, sai_attribute_t *attr_list)
                -> sai_status_t {
                attr_list[0].value.s32 = 0;
                return SAI_STATUS_SUCCESS;
            };

        sai_next_hop_group_member_observed_role_t role;
        EXPECT_TRUE(gNhgOrch->getProtNhgMemberObservedRole(
            key, standby_nh, role));
        EXPECT_EQ(static_cast<int32_t>(role), 0);

        ut_sai_next_hop_group_api.get_next_hop_group_member_attribute =
            old_get_fn;
        ASSERT_TRUE(gNhgOrch->removeProtNhg(key));
        unregisterNextHop(primary_nh);
        unregisterNextHop(standby_nh);
    }

    TEST_F(ProtNhgTest, ObservedRoleSaiFailure)
    {
        string key = "prot_nhg_obs_fail";
        NextHopKey primary_nh(IpAddress("10.0.0.1"), string("Ethernet0"));
        NextHopKey standby_nh(IpAddress("10.0.0.100"), string("Ethernet4"));
        vector<NextHopKey> primaries = {primary_nh};

        registerNextHop(primary_nh);
        registerNextHop(standby_nh);

        ASSERT_TRUE(gNhgOrch->createProtNhg(key, primaries, standby_nh));

        auto old_get_fn =
            ut_sai_next_hop_group_api.get_next_hop_group_member_attribute;
        ut_sai_next_hop_group_api.get_next_hop_group_member_attribute =
            [](sai_object_id_t, uint32_t, sai_attribute_t *)
                -> sai_status_t {
                return SAI_STATUS_FAILURE;
            };

        sai_next_hop_group_member_observed_role_t role;
        EXPECT_FALSE(gNhgOrch->getProtNhgMemberObservedRole(
            key, standby_nh, role));

        ut_sai_next_hop_group_api.get_next_hop_group_member_attribute =
            old_get_fn;
        ASSERT_TRUE(gNhgOrch->removeProtNhg(key));
        unregisterNextHop(primary_nh);
        unregisterNextHop(standby_nh);
    }

    TEST_F(ProtNhgTest, GetMemberObservedRoleNotFound)
    {
        string key = "prot_nhg_obs_notfound";
        NextHopKey primary_nh(IpAddress("10.0.0.1"), string("Ethernet0"));
        NextHopKey standby_nh(IpAddress("10.0.0.100"), string("Ethernet4"));
        NextHopKey unknown_nh(IpAddress("10.0.0.99"), string("Ethernet0"));
        vector<NextHopKey> primaries = {primary_nh};

        registerNextHop(primary_nh);
        registerNextHop(standby_nh);

        ASSERT_TRUE(gNhgOrch->createProtNhg(key, primaries, standby_nh));

        sai_next_hop_group_member_observed_role_t role;
        EXPECT_FALSE(gNhgOrch->getProtNhgMemberObservedRole(
            key, unknown_nh, role));

        ASSERT_TRUE(gNhgOrch->removeProtNhg(key));
        unregisterNextHop(primary_nh);
        unregisterNextHop(standby_nh);
    }

    TEST_F(ProtNhgTest, GetAllMemberObservedRolesSuccess)
    {
        string key = "prot_nhg_all_obs";
        NextHopKey primary_nh(IpAddress("10.0.0.1"), string("Ethernet0"));
        NextHopKey standby_nh(IpAddress("10.0.0.100"), string("Ethernet4"));
        vector<NextHopKey> primaries = {primary_nh};

        registerNextHop(primary_nh);
        registerNextHop(standby_nh);

        ASSERT_TRUE(gNhgOrch->createProtNhg(key, primaries, standby_nh));

        auto old_get_fn =
            ut_sai_next_hop_group_api.get_next_hop_group_member_attribute;
        ut_sai_next_hop_group_api.get_next_hop_group_member_attribute =
            [](sai_object_id_t, uint32_t, sai_attribute_t *attr_list)
                -> sai_status_t {
                attr_list[0].value.s32 = 0;
                return SAI_STATUS_SUCCESS;
            };

        map<NextHopKey, sai_next_hop_group_member_observed_role_t> roles;
        EXPECT_TRUE(gNhgOrch->getProtNhgAllObservedRoles(key, roles));
        EXPECT_EQ(roles.size(), 2u);

        ut_sai_next_hop_group_api.get_next_hop_group_member_attribute =
            old_get_fn;
        ASSERT_TRUE(gNhgOrch->removeProtNhg(key));
        unregisterNextHop(primary_nh);
        unregisterNextHop(standby_nh);
    }

    TEST_F(ProtNhgTest, GetAllMemberObservedRolesSaiFailure)
    {
        string key = "prot_nhg_all_obs_fail";
        NextHopKey primary_nh(IpAddress("10.0.0.1"), string("Ethernet0"));
        NextHopKey standby_nh(IpAddress("10.0.0.100"), string("Ethernet4"));
        vector<NextHopKey> primaries = {primary_nh};

        registerNextHop(primary_nh);
        registerNextHop(standby_nh);

        ASSERT_TRUE(gNhgOrch->createProtNhg(key, primaries, standby_nh));

        auto old_get_fn =
            ut_sai_next_hop_group_api.get_next_hop_group_member_attribute;
        ut_sai_next_hop_group_api.get_next_hop_group_member_attribute =
            [](sai_object_id_t, uint32_t, sai_attribute_t *)
                -> sai_status_t {
                return SAI_STATUS_FAILURE;
            };

        map<NextHopKey, sai_next_hop_group_member_observed_role_t> roles;
        EXPECT_FALSE(gNhgOrch->getProtNhgAllObservedRoles(key, roles));
        EXPECT_TRUE(roles.empty());

        ut_sai_next_hop_group_api.get_next_hop_group_member_attribute =
            old_get_fn;
        ASSERT_TRUE(gNhgOrch->removeProtNhg(key));
        unregisterNextHop(primary_nh);
        unregisterNextHop(standby_nh);
    }

    TEST_F(ProtNhgTest, RemoveProtNhgSaiFailure)
    {
        string key = "prot_nhg_remove_fail";
        NextHopKey primary_nh(IpAddress("10.0.0.1"), string("Ethernet0"));
        NextHopKey standby_nh(IpAddress("10.0.0.100"), string("Ethernet4"));
        vector<NextHopKey> primaries = {primary_nh};

        registerNextHop(primary_nh);
        registerNextHop(standby_nh);

        ASSERT_TRUE(gNhgOrch->createProtNhg(key, primaries, standby_nh));

        EXPECT_CALL(*mock_sai_next_hop_group_api,
                    remove_next_hop_group(_))
            .WillOnce(Return(SAI_STATUS_FAILURE))
            .WillRepeatedly(Return(SAI_STATUS_SUCCESS));

        EXPECT_FALSE(gNhgOrch->removeProtNhg(key));
        EXPECT_TRUE(gNhgOrch->hasProtNhg(key));

        ASSERT_TRUE(gNhgOrch->removeProtNhg(key));
        unregisterNextHop(primary_nh);
        unregisterNextHop(standby_nh);
    }

    TEST_F(ProtNhgTest, ProtNhgInlineMethods)
    {
        string key = "prot_nhg_inline";
        NextHopKey primary_nh(IpAddress("10.0.0.1"), string("Ethernet0"));
        NextHopKey standby_nh(IpAddress("10.0.0.100"), string("Ethernet4"));
        vector<NextHopKey> primaries = {primary_nh};

        registerNextHop(primary_nh);
        registerNextHop(standby_nh);

        ASSERT_TRUE(gNhgOrch->createProtNhg(key, primaries, standby_nh));

        const ProtNhg &nhg = gNhgOrch->getProtNhg(key);
        EXPECT_FALSE(nhg.isTemp());
        EXPECT_EQ(nhg.getNhgKey(), NextHopGroupKey());
        EXPECT_EQ(nhg.to_string(), key);

        ASSERT_TRUE(gNhgOrch->removeProtNhg(key));
        unregisterNextHop(primary_nh);
        unregisterNextHop(standby_nh);
    }

    TEST_F(ProtNhgTest, MemberToString)
    {
        string key = "prot_nhg_mbr_str";
        NextHopKey primary_nh(IpAddress("10.0.0.1"), string("Ethernet0"));
        NextHopKey standby_nh(IpAddress("10.0.0.100"), string("Ethernet4"));
        vector<NextHopKey> primaries = {primary_nh};

        registerNextHop(primary_nh);
        registerNextHop(standby_nh);

        ASSERT_TRUE(gNhgOrch->createProtNhg(key, primaries, standby_nh));

        const ProtNhg &nhg = gNhgOrch->getProtNhg(key);
        const ProtNhgMember *standby = nhg.getStandbyMember();
        ASSERT_NE(standby, nullptr);
        string sstr = standby->to_string();
        EXPECT_FALSE(sstr.empty());
        EXPECT_NE(sstr.find("standby"), string::npos);

        auto primary_out = nhg.getPrimaryMembers();
        ASSERT_GE(primary_out.size(), 1u);
        string pstr = primary_out[0]->to_string();
        EXPECT_NE(pstr.find("primary"), string::npos);

        ASSERT_TRUE(gNhgOrch->removeProtNhg(key));
        unregisterNextHop(primary_nh);
        unregisterNextHop(standby_nh);
    }

    TEST_F(ProtNhgTest, MemberRemoveUnsynced)
    {
        NextHopKey nh(IpAddress("10.0.0.1"), string("Ethernet0"));
        ProtNhgMember member(nh, ProtNhgRole::PRIMARY);

        EXPECT_FALSE(member.isSynced());
        member.remove();
        EXPECT_FALSE(member.isSynced());
    }

    TEST_F(ProtNhgTest, MoveConstructor)
    {
        NextHopKey primary_nh(IpAddress("10.0.0.1"), string("Ethernet0"));
        NextHopKey standby_nh(IpAddress("10.0.0.100"), string("Ethernet4"));
        vector<NextHopKey> primaries = {primary_nh};

        ProtNhg nhg1("nhg_move", primaries, standby_nh);
        ProtNhg nhg2(std::move(nhg1));

        EXPECT_NE(nhg2.getStandbyMember(), nullptr);
        EXPECT_EQ(nhg2.to_string(), "nhg_move");
    }

    TEST_F(ProtNhgTest, SyncWithMonitoredObject)
    {
        NextHopKey primary_nh(IpAddress("10.0.0.1"), string("Ethernet0"));
        NextHopKey standby_nh(IpAddress("10.0.0.100"), string("Ethernet4"));
        sai_object_id_t session_oid = 0xABCD;
        vector<NextHopKey> primaries = {primary_nh};

        registerNextHop(primary_nh);
        registerNextHop(standby_nh);

        ProtNhg nhg("nhg_mon_sync", primaries, standby_nh);

        EXPECT_TRUE(nhg.updateMemberMonitoredObject(standby_nh, session_oid));
        EXPECT_TRUE(nhg.sync());
        EXPECT_TRUE(nhg.isSynced());

        unregisterNextHop(primary_nh);
        unregisterNextHop(standby_nh);
    }

    static uint64_t ecmp_nhg_oid_counter = 0x7000000;

    /* Helper: register a fake synced ECMP NHG in gNhgOrch.
     * Directly assigns a SAI OID instead of calling sync(), which would
     * try to resolve individual NHs through NeighOrch.
     */
    static void addEcmpNhg(const NextHopGroupKey &nhg_key)
    {
        string key_str = nhg_key.to_string();
        auto nhg = make_unique<NextHopGroup>(nhg_key, false);
        nhg->m_id = ++ecmp_nhg_oid_counter;
        gNhgOrch->m_syncdNextHopGroups.emplace(
            key_str, NhgEntry<NextHopGroup>(move(nhg)));
    }

    static void removeEcmpNhg(const string &key_str)
    {
        auto it = gNhgOrch->m_syncdNextHopGroups.find(key_str);
        if (it != gNhgOrch->m_syncdNextHopGroups.end())
        {
            it->second.nhg->m_id = SAI_NULL_OBJECT_ID;
            gNhgOrch->m_syncdNextHopGroups.erase(it);
        }
    }

    TEST_F(ProtNhgTest, CreateProtNhgWithNhgKeys)
    {
        NextHopGroupKey primary_nhg_key("10.0.0.1@Ethernet0,10.0.0.2@Ethernet0");
        NextHopGroupKey standby_nhg_key("10.0.0.100@Ethernet4");

        addEcmpNhg(primary_nhg_key);
        addEcmpNhg(standby_nhg_key);

        string key = "prot_nhg_keys";
        ASSERT_TRUE(gNhgOrch->createProtNhg(key, primary_nhg_key, standby_nhg_key));
        EXPECT_TRUE(gNhgOrch->hasProtNhg(key));
        EXPECT_NE(gNhgOrch->getProtNhgId(key), SAI_NULL_OBJECT_ID);

        ASSERT_TRUE(gNhgOrch->removeProtNhg(key));
        EXPECT_FALSE(gNhgOrch->hasProtNhg(key));

        removeEcmpNhg(primary_nhg_key.to_string());
        removeEcmpNhg(standby_nhg_key.to_string());
    }

    TEST_F(ProtNhgTest, CreateProtNhgWithNhgKeysPrimaryNotFound)
    {
        NextHopGroupKey primary_nhg_key("10.0.0.1@Ethernet0");
        NextHopGroupKey standby_nhg_key("10.0.0.100@Ethernet4");

        addEcmpNhg(standby_nhg_key);

        EXPECT_FALSE(gNhgOrch->createProtNhg("prot_no_primary",
                                              primary_nhg_key,
                                              standby_nhg_key));
        EXPECT_FALSE(gNhgOrch->hasProtNhg("prot_no_primary"));

        removeEcmpNhg(standby_nhg_key.to_string());
    }

    TEST_F(ProtNhgTest, CreateProtNhgWithNhgKeysStandbyNotFound)
    {
        NextHopGroupKey primary_nhg_key("10.0.0.1@Ethernet0");
        NextHopGroupKey standby_nhg_key("10.0.0.100@Ethernet4");

        addEcmpNhg(primary_nhg_key);

        EXPECT_FALSE(gNhgOrch->createProtNhg("prot_no_standby",
                                              primary_nhg_key,
                                              standby_nhg_key));
        EXPECT_FALSE(gNhgOrch->hasProtNhg("prot_no_standby"));

        removeEcmpNhg(primary_nhg_key.to_string());
    }

    TEST_F(ProtNhgTest, CreateProtNhgWithNhgKeysEmptyPrimary)
    {
        NextHopGroupKey empty_primary;
        NextHopGroupKey standby_nhg_key("10.0.0.100@Ethernet4");

        EXPECT_FALSE(gNhgOrch->createProtNhg("prot_empty_primary",
                                              empty_primary,
                                              standby_nhg_key));
    }

    TEST_F(ProtNhgTest, CreateProtNhgWithNhgKeysEmptyStandby)
    {
        NextHopGroupKey primary_nhg_key("10.0.0.1@Ethernet0");
        NextHopGroupKey empty_standby;

        EXPECT_FALSE(gNhgOrch->createProtNhg("prot_empty_standby",
                                              primary_nhg_key,
                                              empty_standby));
    }

    TEST_F(ProtNhgTest, CreateNonHwProtectionNhg)
    {
        string key = "prot_nhg_sw";
        NextHopKey primary_nh(IpAddress("10.0.0.1"), string("Ethernet0"));
        NextHopKey standby_nh(IpAddress("10.0.0.100"), string("Ethernet4"));
        vector<NextHopKey> primaries = {primary_nh};

        registerNextHop(primary_nh);
        registerNextHop(standby_nh);

        EXPECT_CALL(*mock_sai_next_hop_group_api,
                    create_next_hop_group(_, _, _, _))
            .WillOnce(
                [](sai_object_id_t *id, sai_object_id_t,
                   uint32_t attr_count, const sai_attribute_t *attrs) {
                    for (uint32_t i = 0; i < attr_count; i++)
                    {
                        if (attrs[i].id == SAI_NEXT_HOP_GROUP_ATTR_TYPE)
                        {
                            EXPECT_EQ(attrs[i].value.s32,
                                      SAI_NEXT_HOP_GROUP_TYPE_PROTECTION);
                        }
                    }
                    *id = 0xAA00;
                    return SAI_STATUS_SUCCESS;
                });

        ASSERT_TRUE(gNhgOrch->createProtNhg(key, primaries, standby_nh,
                                             false));
        EXPECT_TRUE(gNhgOrch->hasProtNhg(key));

        ASSERT_TRUE(gNhgOrch->removeProtNhg(key));
        unregisterNextHop(primary_nh);
        unregisterNextHop(standby_nh);
    }

    TEST_F(ProtNhgTest, SetAdminRoleOnProtectionNhgFails)
    {
        string key = "prot_nhg_admin_prot";
        NextHopKey primary_nh(IpAddress("10.0.0.1"), string("Ethernet0"));
        NextHopKey standby_nh(IpAddress("10.0.0.100"), string("Ethernet4"));
        vector<NextHopKey> primaries = {primary_nh};

        registerNextHop(primary_nh);
        registerNextHop(standby_nh);

        ASSERT_TRUE(gNhgOrch->createProtNhg(key, primaries, standby_nh,
                                             false));

        EXPECT_FALSE(gNhgOrch->setProtNhgAdminRole(
            key, SAI_NEXT_HOP_GROUP_MEMBER_CONFIGURED_ROLE_PRIMARY));

        ASSERT_TRUE(gNhgOrch->removeProtNhg(key));
        unregisterNextHop(primary_nh);
        unregisterNextHop(standby_nh);
    }

    TEST_F(ProtNhgTest, SetSwitchoverSuccess)
    {
        string key = "prot_nhg_switchover";
        NextHopKey primary_nh(IpAddress("10.0.0.1"), string("Ethernet0"));
        NextHopKey standby_nh(IpAddress("10.0.0.100"), string("Ethernet4"));
        vector<NextHopKey> primaries = {primary_nh};

        registerNextHop(primary_nh);
        registerNextHop(standby_nh);

        ASSERT_TRUE(gNhgOrch->createProtNhg(key, primaries, standby_nh,
                                             false));

        EXPECT_CALL(*mock_sai_next_hop_group_api,
                    set_next_hop_group_attribute(_, _))
            .Times(1)
            .WillOnce(Return(SAI_STATUS_SUCCESS));

        EXPECT_TRUE(gNhgOrch->setProtNhgSwitchover(key, true));

        ASSERT_TRUE(gNhgOrch->removeProtNhg(key));
        unregisterNextHop(primary_nh);
        unregisterNextHop(standby_nh);
    }

    TEST_F(ProtNhgTest, SetSwitchoverOnHwProtectionFails)
    {
        string key = "prot_nhg_switchover_hw";
        NextHopKey primary_nh(IpAddress("10.0.0.1"), string("Ethernet0"));
        NextHopKey standby_nh(IpAddress("10.0.0.100"), string("Ethernet4"));
        vector<NextHopKey> primaries = {primary_nh};

        registerNextHop(primary_nh);
        registerNextHop(standby_nh);

        ASSERT_TRUE(gNhgOrch->createProtNhg(key, primaries, standby_nh,
                                             true));

        EXPECT_FALSE(gNhgOrch->setProtNhgSwitchover(key, true));

        ASSERT_TRUE(gNhgOrch->removeProtNhg(key));
        unregisterNextHop(primary_nh);
        unregisterNextHop(standby_nh);
    }

    TEST_F(ProtNhgTest, SetSwitchoverNonExistentNhgFails)
    {
        EXPECT_FALSE(gNhgOrch->setProtNhgSwitchover("no_such_key", true));
    }

    TEST_F(ProtNhgTest, CreateProtNhgAutoKey)
    {
        NextHopKey primary_nh(IpAddress("10.0.0.1"), string("Ethernet0"));
        NextHopKey standby_nh(IpAddress("10.0.0.100"), string("Ethernet4"));
        vector<NextHopKey> primaries = {primary_nh};

        registerNextHop(primary_nh);
        registerNextHop(standby_nh);

        string expected_key = NhgOrch::buildProtNhgKey(primaries, standby_nh);
        EXPECT_FALSE(expected_key.empty());

        ASSERT_TRUE(gNhgOrch->createProtNhg(primaries, standby_nh));
        EXPECT_TRUE(gNhgOrch->hasProtNhg(expected_key));

        EXPECT_FALSE(gNhgOrch->createProtNhg(primaries, standby_nh));

        ASSERT_TRUE(gNhgOrch->removeProtNhg(expected_key));
        EXPECT_FALSE(gNhgOrch->hasProtNhg(expected_key));

        unregisterNextHop(primary_nh);
        unregisterNextHop(standby_nh);
    }

    TEST_F(ProtNhgTest, CreateProtNhgAutoKeyWithNhgKeys)
    {
        NextHopGroupKey primary_nhg_key("10.0.0.1@Ethernet0,10.0.0.2@Ethernet0");
        NextHopGroupKey standby_nhg_key("10.0.0.100@Ethernet4");

        addEcmpNhg(primary_nhg_key);
        addEcmpNhg(standby_nhg_key);

        string expected_key = NhgOrch::buildProtNhgKey(primary_nhg_key,
                                                       standby_nhg_key);
        EXPECT_FALSE(expected_key.empty());

        ASSERT_TRUE(gNhgOrch->createProtNhg(primary_nhg_key, standby_nhg_key));
        EXPECT_TRUE(gNhgOrch->hasProtNhg(expected_key));

        ASSERT_TRUE(gNhgOrch->removeProtNhg(expected_key));

        removeEcmpNhg(primary_nhg_key.to_string());
        removeEcmpNhg(standby_nhg_key.to_string());
    }

    TEST_F(ProtNhgTest, BuildProtNhgKeySortsPrimaries)
    {
        NextHopKey nh_a(IpAddress("10.0.0.1"), string("Ethernet0"));
        NextHopKey nh_b(IpAddress("10.0.0.2"), string("Ethernet0"));
        NextHopKey standby(IpAddress("10.0.0.100"), string("Ethernet4"));

        string key_ab = NhgOrch::buildProtNhgKey({nh_a, nh_b}, standby);
        string key_ba = NhgOrch::buildProtNhgKey({nh_b, nh_a}, standby);

        EXPECT_EQ(key_ab, key_ba);
    }

    /* --- IPinIP tunnel NextHopKey tests --- */

    TEST_F(ProtNhgTest, TunnelNextHopKeyConstructor)
    {
        IpAddress ip("10.1.0.32");
        NextHopKey nh(ip, string("MuxTunnel0"), true /*tunnel_nh*/, 0 /*tag*/);

        EXPECT_TRUE(nh.isTunnelNextHop());
        EXPECT_EQ(nh.ip_address, ip);
        EXPECT_EQ(nh.tunnel_name, "MuxTunnel0");
        EXPECT_EQ(nh.alias, "");
        EXPECT_EQ(nh.vni, 0u);
        EXPECT_FALSE(nh.isSrv6NextHop());
        EXPECT_FALSE(nh.isMplsNextHop());
    }

    TEST_F(ProtNhgTest, TunnelNextHopKeyToStringRoundtrip)
    {
        IpAddress ip("192.168.1.1");
        NextHopKey original(ip, string("IPINIP_TUNNEL"), true /*tunnel_nh*/, 0 /*tag*/);

        string str = original.to_string();
        EXPECT_EQ(str, "tunnel:IPINIP_TUNNEL@192.168.1.1");

        NextHopKey parsed(str);
        EXPECT_TRUE(parsed.isTunnelNextHop());
        EXPECT_EQ(parsed.tunnel_name, "IPINIP_TUNNEL");
        EXPECT_EQ(parsed.ip_address, ip);
        EXPECT_EQ(original, parsed);
    }

    TEST_F(ProtNhgTest, TunnelNextHopKeyComparison)
    {
        NextHopKey nh_a(IpAddress("10.0.0.1"), string("TunA"), true, 0);
        NextHopKey nh_b(IpAddress("10.0.0.1"), string("TunB"), true, 0);
        NextHopKey nh_same(IpAddress("10.0.0.1"), string("TunA"), true, 0);

        EXPECT_EQ(nh_a, nh_same);
        EXPECT_NE(nh_a, nh_b);

        NextHopKey regular_nh(IpAddress("10.0.0.1"), string("Ethernet0"));
        EXPECT_NE(nh_a, regular_nh);
    }

    TEST_F(ProtNhgTest, TunnelNextHopKeyInvalidParseFails)
    {
        EXPECT_THROW(NextHopKey("tunnel:@10.0.0.1@extra"), std::invalid_argument);
        EXPECT_THROW(NextHopKey("tunnel:OnlyName"), std::invalid_argument);
    }

    /* --- Protection NHG key prefix tests --- */

    TEST_F(ProtNhgTest, BuildProtNhgKeyHwPrefix)
    {
        NextHopKey primary(IpAddress("10.0.0.1"), string("Ethernet0"));
        NextHopKey standby(IpAddress("10.0.0.100"), string("Ethernet4"));

        string key = NhgOrch::buildProtNhgKey({primary}, standby, true);
        EXPECT_EQ(key.substr(0, 8), "prot:hw:");
        EXPECT_NE(key.find("10.0.0.1@Ethernet0"), string::npos);
    }

    TEST_F(ProtNhgTest, BuildProtNhgKeySwPrefix)
    {
        NextHopKey primary(IpAddress("10.0.0.1"), string("Ethernet0"));
        NextHopKey standby(IpAddress("10.0.0.100"), string("Ethernet4"));

        string key = NhgOrch::buildProtNhgKey({primary}, standby, false);
        EXPECT_EQ(key.substr(0, 8), "prot:sw:");
    }

    TEST_F(ProtNhgTest, BuildProtNhgKeyNhgHwPrefix)
    {
        NextHopGroupKey primary("10.0.0.1@Ethernet0,10.0.0.2@Ethernet0");
        NextHopGroupKey standby("10.0.0.100@Ethernet4");

        string key = NhgOrch::buildProtNhgKey(primary, standby, true);
        EXPECT_EQ(key.substr(0, 8), "prot:hw:");
    }

    TEST_F(ProtNhgTest, BuildProtNhgKeyNhgSwPrefix)
    {
        NextHopGroupKey primary("10.0.0.1@Ethernet0,10.0.0.2@Ethernet0");
        NextHopGroupKey standby("10.0.0.100@Ethernet4");

        string key = NhgOrch::buildProtNhgKey(primary, standby, false);
        EXPECT_EQ(key.substr(0, 8), "prot:sw:");
    }

    TEST_F(ProtNhgTest, HwAndSwKeysAreDifferent)
    {
        NextHopKey primary(IpAddress("10.0.0.1"), string("Ethernet0"));
        NextHopKey standby(IpAddress("10.0.0.100"), string("Ethernet4"));

        string hw_key = NhgOrch::buildProtNhgKey({primary}, standby, true);
        string sw_key = NhgOrch::buildProtNhgKey({primary}, standby, false);
        EXPECT_NE(hw_key, sw_key);
    }

    /* --- Tunnel NH in a protection NHG --- */

    TEST_F(ProtNhgTest, CreateProtNhgWithTunnelStandby)
    {
        NextHopKey primary_nh(IpAddress("10.0.0.1"), string("Ethernet0"));
        NextHopKey standby_nh(IpAddress("10.1.0.32"), string("MuxTunnel0"),
                              true /*tunnel_nh*/, 0 /*tag*/);
        vector<NextHopKey> primaries = {primary_nh};

        registerNextHop(primary_nh);
        registerNextHop(standby_nh, 0xBEEF);

        string key = "prot_tunnel_standby";
        ASSERT_TRUE(gNhgOrch->createProtNhg(key, primaries, standby_nh));
        EXPECT_TRUE(gNhgOrch->hasProtNhg(key));
        EXPECT_NE(gNhgOrch->getProtNhgId(key), SAI_NULL_OBJECT_ID);

        ASSERT_TRUE(gNhgOrch->removeProtNhg(key));
        EXPECT_FALSE(gNhgOrch->hasProtNhg(key));

        unregisterNextHop(primary_nh);
        unregisterNextHop(standby_nh);
    }

    TEST_F(ProtNhgTest, AutoKeyWithTunnelNextHop)
    {
        NextHopKey primary_nh(IpAddress("10.0.0.1"), string("Ethernet0"));
        NextHopKey standby_nh(IpAddress("10.1.0.32"), string("MuxTunnel0"),
                              true /*tunnel_nh*/, 0 /*tag*/);
        vector<NextHopKey> primaries = {primary_nh};

        registerNextHop(primary_nh);
        registerNextHop(standby_nh, 0xBEEF);

        string expected_key = NhgOrch::buildProtNhgKey(primaries, standby_nh, true);
        EXPECT_NE(expected_key.find("tunnel:MuxTunnel0"), string::npos);

        ASSERT_TRUE(gNhgOrch->createProtNhg(primaries, standby_nh));
        EXPECT_TRUE(gNhgOrch->hasProtNhg(expected_key));

        ASSERT_TRUE(gNhgOrch->removeProtNhg(expected_key));
        unregisterNextHop(primary_nh);
        unregisterNextHop(standby_nh);
    }

    TEST_F(ProtNhgTest, RecursiveMemberResolvesViaNhgOrch)
    {
        NextHopGroupKey primary_nhg_key("10.0.0.1@Ethernet0,10.0.0.2@Ethernet0");
        NextHopGroupKey standby_nhg_key("10.0.0.100@Ethernet4");

        addEcmpNhg(primary_nhg_key);
        addEcmpNhg(standby_nhg_key);

        string key = "prot_recursive_resolve";
        ASSERT_TRUE(gNhgOrch->createProtNhg(key, primary_nhg_key, standby_nhg_key));

        const ProtNhg &nhg = gNhgOrch->getProtNhg(key);
        auto primaries = nhg.getPrimaryMembers();
        ASSERT_EQ(primaries.size(), 1u);
        EXPECT_TRUE(primaries[0]->isRecursive());
        EXPECT_NE(primaries[0]->getNhId(), SAI_NULL_OBJECT_ID);

        const ProtNhgMember *standby = nhg.getStandbyMember();
        ASSERT_NE(standby, nullptr);
        EXPECT_TRUE(standby->isRecursive());
        EXPECT_NE(standby->getNhId(), SAI_NULL_OBJECT_ID);

        ASSERT_TRUE(gNhgOrch->removeProtNhg(key));
        removeEcmpNhg(primary_nhg_key.to_string());
        removeEcmpNhg(standby_nhg_key.to_string());
    }
 }
