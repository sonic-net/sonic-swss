#define private public
#include "directory.h"
#undef private
#define protected public
#include "orch.h"
#undef protected

#include "ut_helper.h"
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
        sai_object_id_t standby_nh_id = 0x1234;

        vector<NextHopKey> primaries = {primary_nh};

        ASSERT_TRUE(gNhgOrch->createProtNhg(key, primaries, standby_nh, standby_nh_id));
        EXPECT_TRUE(gNhgOrch->hasProtNhg(key));
        EXPECT_NE(gNhgOrch->getProtNhgId(key), SAI_NULL_OBJECT_ID);

        ASSERT_TRUE(gNhgOrch->removeProtNhg(key));
        EXPECT_FALSE(gNhgOrch->hasProtNhg(key));
        EXPECT_EQ(gNhgOrch->getProtNhgId(key), SAI_NULL_OBJECT_ID);
    }

    TEST_F(ProtNhgTest, CreateDuplicateProtNhgFails)
    {
        string key = "prot_nhg_dup";
        NextHopKey primary_nh(IpAddress("10.0.0.1"), string("Ethernet0"));
        NextHopKey standby_nh(IpAddress("10.0.0.100"), string("Ethernet4"));
        sai_object_id_t standby_nh_id = 0x1234;
        vector<NextHopKey> primaries = {primary_nh};

        ASSERT_TRUE(gNhgOrch->createProtNhg(key, primaries, standby_nh, standby_nh_id));
        EXPECT_FALSE(gNhgOrch->createProtNhg(key, primaries, standby_nh, standby_nh_id));

        ASSERT_TRUE(gNhgOrch->removeProtNhg(key));
    }

    TEST_F(ProtNhgTest, CreateProtNhgEmptyPrimariesFails)
    {
        NextHopKey standby_nh(IpAddress("10.0.0.100"), string("Ethernet4"));
        vector<NextHopKey> primaries;

        EXPECT_FALSE(gNhgOrch->createProtNhg("empty", primaries, standby_nh, 0x1234));
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

        ASSERT_TRUE(gNhgOrch->createProtNhg(key, primaries, standby_nh, 0x1234));

        gNhgOrch->incProtNhgRefCount(key);
        EXPECT_FALSE(gNhgOrch->removeProtNhg(key));
        EXPECT_TRUE(gNhgOrch->hasProtNhg(key));

        gNhgOrch->decProtNhgRefCount(key);
        EXPECT_TRUE(gNhgOrch->removeProtNhg(key));
        EXPECT_FALSE(gNhgOrch->hasProtNhg(key));
    }

    TEST_F(ProtNhgTest, GetProtNhgMembers)
    {
        string key = "prot_nhg_members";
        NextHopKey primary_nh(IpAddress("10.0.0.1"), string("Ethernet0"));
        NextHopKey standby_nh(IpAddress("10.0.0.100"), string("Ethernet4"));
        sai_object_id_t standby_nh_id = 0x1234;
        vector<NextHopKey> primaries = {primary_nh};

        ASSERT_TRUE(gNhgOrch->createProtNhg(key, primaries, standby_nh, standby_nh_id));

        const ProtNhg &nhg = gNhgOrch->getProtNhg(key);
        EXPECT_NE(nhg.getId(), SAI_NULL_OBJECT_ID);

        const ProtNhgMember *standby = nhg.getStandbyMember();
        ASSERT_NE(standby, nullptr);
        EXPECT_EQ(standby->getRole(), ProtNhgRole::STANDBY);

        auto primary_out = nhg.getPrimaryMembers();
        ASSERT_EQ(primary_out.size(), 1u);
        EXPECT_EQ(primary_out[0]->getRole(), ProtNhgRole::PRIMARY);

        ASSERT_TRUE(gNhgOrch->removeProtNhg(key));
    }

    TEST_F(ProtNhgTest, MultiplePrimaryMembers)
    {
        string key = "prot_nhg_multi";
        NextHopKey primary1(IpAddress("10.0.0.1"), string("Ethernet0"));
        NextHopKey primary2(IpAddress("10.0.0.2"), string("Ethernet0"));
        NextHopKey standby_nh(IpAddress("10.0.0.100"), string("Ethernet4"));
        vector<NextHopKey> primaries = {primary1, primary2};

        ASSERT_TRUE(gNhgOrch->createProtNhg(key, primaries, standby_nh, 0x1234));

        const ProtNhg &nhg = gNhgOrch->getProtNhg(key);
        auto primary_out = nhg.getPrimaryMembers();
        EXPECT_EQ(primary_out.size(), 2u);

        ASSERT_TRUE(gNhgOrch->removeProtNhg(key));
    }

    TEST_F(ProtNhgTest, SetAdminRole)
    {
        string key = "prot_nhg_admin";
        NextHopKey primary_nh(IpAddress("10.0.0.1"), string("Ethernet0"));
        NextHopKey standby_nh(IpAddress("10.0.0.100"), string("Ethernet4"));
        vector<NextHopKey> primaries = {primary_nh};

        ASSERT_TRUE(gNhgOrch->createProtNhg(key, primaries, standby_nh, 0x1234));

        EXPECT_CALL(*mock_sai_next_hop_group_api,
                    set_next_hop_group_attribute(_, _))
            .Times(1)
            .WillOnce(Return(SAI_STATUS_SUCCESS));

        EXPECT_TRUE(gNhgOrch->setProtNhgAdminRole(
            key, SAI_NEXT_HOP_GROUP_MEMBER_CONFIGURED_ROLE_PRIMARY));

        ASSERT_TRUE(gNhgOrch->removeProtNhg(key));
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

        ASSERT_TRUE(gNhgOrch->createProtNhg(key, primaries, standby_nh, 0x1234));

        EXPECT_CALL(*mock_sai_next_hop_group_api,
                    set_next_hop_group_attribute(_, _))
            .Times(1)
            .WillOnce(Return(SAI_STATUS_FAILURE));

        EXPECT_FALSE(gNhgOrch->setProtNhgAdminRole(
            key, SAI_NEXT_HOP_GROUP_MEMBER_CONFIGURED_ROLE_PRIMARY));

        ASSERT_TRUE(gNhgOrch->removeProtNhg(key));
    }

    TEST_F(ProtNhgTest, SetMonitoredObjectOnStandbyMember)
    {
        string key = "prot_nhg_monitor";
        NextHopKey primary_nh(IpAddress("10.0.0.1"), string("Ethernet0"));
        NextHopKey standby_nh(IpAddress("10.0.0.100"), string("Ethernet4"));
        sai_object_id_t standby_nh_id = 0x1234;
        sai_object_id_t session_oid = 0xABCD;
        vector<NextHopKey> primaries = {primary_nh};

        ASSERT_TRUE(gNhgOrch->createProtNhg(key, primaries, standby_nh, standby_nh_id));

        EXPECT_TRUE(gNhgOrch->setProtNhgMonitoredObject(key, standby_nh, session_oid));

        ASSERT_TRUE(gNhgOrch->removeProtNhg(key));
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

        ASSERT_TRUE(gNhgOrch->createProtNhg(key, primaries, standby_nh, 0x1234));
        EXPECT_FALSE(gNhgOrch->setProtNhgMonitoredObject(key, unknown_nh, 0xABCD));

        ASSERT_TRUE(gNhgOrch->removeProtNhg(key));
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

        EXPECT_FALSE(gNhgOrch->createProtNhg(key, primaries, standby_nh, 0x1234));
        EXPECT_FALSE(gNhgOrch->hasProtNhg(key));
    }

    TEST_F(ProtNhgTest, HasAndGetIdForNonExistentKey)
    {
        EXPECT_FALSE(gNhgOrch->hasProtNhg("ghost"));
        EXPECT_EQ(gNhgOrch->getProtNhgId("ghost"), SAI_NULL_OBJECT_ID);
    }
}
