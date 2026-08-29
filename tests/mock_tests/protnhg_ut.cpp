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

/* Must precede mock_orchagent_main.h, which includes nhgorch.h unguarded;
 * #pragma once would otherwise discard this override. */
#define private public
#include "nhgorch.h"
#undef private

#include "mock_orchagent_main.h"
#include "mock_sai_api.h"
#include "mock_orch_test.h"
#include "protnhg.h"
#include "portal.h"

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

    /*
     * A real port OID from the mock switch. attachProtNhgMonitoredObject()
     * resolves an object's type with sai_object_type_query(), so tests must
     * use OIDs the SAI library actually issued rather than fabricated ones:
     * the type is encoded in the OID, and the encoding is library-specific.
     * SAI_OBJECT_TYPE_PORT is what PostSetUp() advertises as attachable.
     */
    static sai_object_id_t monitoredOid()
    {
        sai_attribute_t attr;
        vector<sai_object_id_t> ports(256);

        attr.id = SAI_SWITCH_ATTR_PORT_LIST;
        attr.value.objlist.count = static_cast<uint32_t>(ports.size());
        attr.value.objlist.list = ports.data();

        if (sai_switch_api->get_switch_attribute(gSwitchId, 1, &attr) !=
                SAI_STATUS_SUCCESS ||
            attr.value.objlist.count == 0)
        {
            return SAI_NULL_OBJECT_ID;
        }

        return attr.value.objlist.list[0];
    }

    /* Sum the CRM "used" counter across all keys for a resource type. */
    static uint32_t crmUsed(CrmResourceType type)
    {
        uint32_t count = 0;
        const auto &resourceMap = Portal::CrmOrchInternal::getResourceMap(gCrmOrch);
        for (const auto &kv : resourceMap.at(type).countersMap)
        {
            count += kv.second.usedCounter;
        }
        return count;
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

            /* Force protection-capability support by default so tests don't
             * depend on the real SAI capability queries, which aren't
             * meaningful against a mock switch. Tests exercising the probe
             * or an unsupported capability override this explicitly. */
            gNhgOrch->m_protCapChecked = true;
            gNhgOrch->m_protectionSupported = true;
            gNhgOrch->m_backupGroupHintSupported = true;
            gNhgOrch->m_monitoredObjectTypes = { SAI_OBJECT_TYPE_PORT };
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

        ASSERT_TRUE(gNhgOrch->createProtNhg(key, primary_nh, standby_nh));
        EXPECT_TRUE(gNhgOrch->hasProtNhg(key));
        EXPECT_NE(gNhgOrch->getProtNhgId(key), SAI_NULL_OBJECT_ID);

        ASSERT_TRUE(gNhgOrch->removeProtNhg(key));
        EXPECT_FALSE(gNhgOrch->hasProtNhg(key));
        EXPECT_EQ(gNhgOrch->getProtNhgId(key), SAI_NULL_OBJECT_ID);

        unregisterNextHop(primary_nh);
        unregisterNextHop(standby_nh);
    }

    TEST_F(ProtNhgTest, CreateDuplicateProtNhgIsIdempotent)
    {
        string key = "prot_nhg_dup";
        NextHopKey primary_nh(IpAddress("10.0.0.1"), string("Ethernet0"));
        NextHopKey standby_nh(IpAddress("10.0.0.100"), string("Ethernet4"));
        registerNextHop(primary_nh);
        registerNextHop(standby_nh);

        ASSERT_TRUE(gNhgOrch->createProtNhg(key, primary_nh, standby_nh));
        EXPECT_TRUE(gNhgOrch->createProtNhg(key, primary_nh, standby_nh));

        ASSERT_TRUE(gNhgOrch->removeProtNhg(key));
        unregisterNextHop(primary_nh);
        unregisterNextHop(standby_nh);
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
        registerNextHop(primary_nh);
        registerNextHop(standby_nh);

        ASSERT_TRUE(gNhgOrch->createProtNhg(key, primary_nh, standby_nh));

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
        registerNextHop(primary_nh);
        registerNextHop(standby_nh);

        ASSERT_TRUE(gNhgOrch->createProtNhg(key, primary_nh, standby_nh));

        const ProtNhg &nhg = gNhgOrch->getProtNhg(key);
        EXPECT_NE(nhg.getId(), SAI_NULL_OBJECT_ID);

        const ProtNhgMember *standby = nhg.getStandbyMember();
        ASSERT_NE(standby, nullptr);
        EXPECT_EQ(standby->getRole(), ProtNhgRole::STANDBY);

        const ProtNhgMember *primary = nhg.getPrimaryMember();
        ASSERT_NE(primary, nullptr);
        EXPECT_EQ(primary->getRole(), ProtNhgRole::PRIMARY);

        ASSERT_TRUE(gNhgOrch->removeProtNhg(key));
        unregisterNextHop(primary_nh);
        unregisterNextHop(standby_nh);
    }

    TEST_F(ProtNhgTest, SetAdminRole)
    {
        string key = "prot_nhg_admin";
        NextHopKey primary_nh(IpAddress("10.0.0.1"), string("Ethernet0"));
        NextHopKey standby_nh(IpAddress("10.0.0.100"), string("Ethernet4"));
        registerNextHop(primary_nh);
        registerNextHop(standby_nh);

        ASSERT_TRUE(gNhgOrch->createProtNhg(key, primary_nh, standby_nh));

        /* The admin role only exists as an override of a hardware decision,
         * so the group has to be HW-autonomous first. The attach itself
         * clears any standing SET_SWITCHOVER, which is the first group write. */
        EXPECT_CALL(*mock_sai_next_hop_group_api,
                    set_next_hop_group_attribute(_, _))
            .Times(2)
            .WillRepeatedly(Return(SAI_STATUS_SUCCESS));

        ASSERT_TRUE(gNhgOrch->attachProtNhgMonitoredObject(
            key, primary_nh, monitoredOid()));

        EXPECT_TRUE(gNhgOrch->setProtNhgAdminRole(
            key, SAI_NEXT_HOP_GROUP_ADMIN_ROLE_PRIMARY));

        ASSERT_TRUE(gNhgOrch->removeProtNhg(key));
        unregisterNextHop(primary_nh);
        unregisterNextHop(standby_nh);
    }

    TEST_F(ProtNhgTest, SetAdminRoleNonExistentFails)
    {
        EXPECT_FALSE(gNhgOrch->setProtNhgAdminRole(
            "no_such_key", SAI_NEXT_HOP_GROUP_ADMIN_ROLE_PRIMARY));
    }

    TEST_F(ProtNhgTest, SetAdminRoleSaiFailure)
    {
        string key = "prot_nhg_admin_fail";
        NextHopKey primary_nh(IpAddress("10.0.0.1"), string("Ethernet0"));
        NextHopKey standby_nh(IpAddress("10.0.0.100"), string("Ethernet4"));
        registerNextHop(primary_nh);
        registerNextHop(standby_nh);

        ASSERT_TRUE(gNhgOrch->createProtNhg(key, primary_nh, standby_nh));
        ASSERT_TRUE(gNhgOrch->attachProtNhgMonitoredObject(
            key, primary_nh, monitoredOid()));

        EXPECT_CALL(*mock_sai_next_hop_group_api,
                    set_next_hop_group_attribute(_, _))
            .Times(1)
            .WillOnce(Return(SAI_STATUS_FAILURE));

        EXPECT_FALSE(gNhgOrch->setProtNhgAdminRole(
            key, SAI_NEXT_HOP_GROUP_ADMIN_ROLE_PRIMARY));

        ASSERT_TRUE(gNhgOrch->removeProtNhg(key));
        unregisterNextHop(primary_nh);
        unregisterNextHop(standby_nh);
    }

    /* --- Monitored object attach / detach and the switchover mode --- */

    TEST_F(ProtNhgTest, AttachMonitoredObjectPromotesToHwAutonomous)
    {
        string key = "prot_nhg_monitor";
        NextHopKey primary_nh(IpAddress("10.0.0.1"), string("Ethernet0"));
        NextHopKey standby_nh(IpAddress("10.0.0.100"), string("Ethernet4"));

        registerNextHop(primary_nh);
        registerNextHop(standby_nh);

        ASSERT_TRUE(gNhgOrch->createProtNhg(key, primary_nh, standby_nh));

        /* A group starts SW-driven; the attach is what hands the switchover
         * decision to the hardware. */
        const ProtNhg &nhg = gNhgOrch->getProtNhg(key);
        EXPECT_FALSE(nhg.isHwAutonomous());

        EXPECT_TRUE(gNhgOrch->attachProtNhgMonitoredObject(
            key, primary_nh, monitoredOid()));
        EXPECT_TRUE(nhg.isHwAutonomous());

        EXPECT_TRUE(gNhgOrch->detachProtNhgMonitoredObject(key, primary_nh));
        EXPECT_FALSE(nhg.isHwAutonomous());

        ASSERT_TRUE(gNhgOrch->removeProtNhg(key));
        unregisterNextHop(primary_nh);
        unregisterNextHop(standby_nh);
    }

    TEST_F(ProtNhgTest, AttachMonitoredObjectOnStandbyMember)
    {
        string key = "prot_nhg_monitor_standby";
        NextHopKey primary_nh(IpAddress("10.0.0.1"), string("Ethernet0"));
        NextHopKey standby_nh(IpAddress("10.0.0.100"), string("Ethernet4"));

        registerNextHop(primary_nh);
        registerNextHop(standby_nh);

        ASSERT_TRUE(gNhgOrch->createProtNhg(key, primary_nh, standby_nh));
        EXPECT_TRUE(gNhgOrch->attachProtNhgMonitoredObject(
            key, standby_nh, monitoredOid()));

        /* Only the primary member's monitored object decides the mode. */
        EXPECT_FALSE(gNhgOrch->getProtNhg(key).isHwAutonomous());

        ASSERT_TRUE(gNhgOrch->removeProtNhg(key));
        unregisterNextHop(primary_nh);
        unregisterNextHop(standby_nh);
    }

    TEST_F(ProtNhgTest, AttachUnsupportedMonitoredObjectTypeFails)
    {
        string key = "prot_nhg_monitor_bad_type";
        NextHopKey primary_nh(IpAddress("10.0.0.1"), string("Ethernet0"));
        NextHopKey standby_nh(IpAddress("10.0.0.100"), string("Ethernet4"));
        registerNextHop(primary_nh);
        registerNextHop(standby_nh);

        ASSERT_TRUE(gNhgOrch->createProtNhg(key, primary_nh, standby_nh));

        /* The advertised type list is authoritative, so a type outside it --
         * here the switch object itself -- is refused before any SAI call and
         * the group stays SW-driven. */
        EXPECT_CALL(*mock_sai_next_hop_group_api,
                    set_next_hop_group_member_attribute(_, _)).Times(0);

        EXPECT_FALSE(gNhgOrch->attachProtNhgMonitoredObject(
            key, primary_nh, gSwitchId));
        EXPECT_FALSE(gNhgOrch->getProtNhg(key).isHwAutonomous());

        ASSERT_TRUE(gNhgOrch->removeProtNhg(key));
        unregisterNextHop(primary_nh);
        unregisterNextHop(standby_nh);
    }

    TEST_F(ProtNhgTest, AttachNullMonitoredObjectFails)
    {
        string key = "prot_nhg_monitor_null";
        NextHopKey primary_nh(IpAddress("10.0.0.1"), string("Ethernet0"));
        NextHopKey standby_nh(IpAddress("10.0.0.100"), string("Ethernet4"));
        registerNextHop(primary_nh);
        registerNextHop(standby_nh);

        ASSERT_TRUE(gNhgOrch->createProtNhg(key, primary_nh, standby_nh));
        EXPECT_FALSE(gNhgOrch->attachProtNhgMonitoredObject(
            key, primary_nh, SAI_NULL_OBJECT_ID));

        ASSERT_TRUE(gNhgOrch->removeProtNhg(key));
        unregisterNextHop(primary_nh);
        unregisterNextHop(standby_nh);
    }

    TEST_F(ProtNhgTest, AttachMonitoredObjectNonExistentNhgFails)
    {
        NextHopKey nh(IpAddress("10.0.0.1"), string("Ethernet0"));
        EXPECT_FALSE(gNhgOrch->attachProtNhgMonitoredObject(
            "no_such_key", nh, monitoredOid()));
        EXPECT_FALSE(gNhgOrch->detachProtNhgMonitoredObject("no_such_key", nh));
    }

    TEST_F(ProtNhgTest, AttachMonitoredObjectNonExistentMemberFails)
    {
        string key = "prot_nhg_monitor_bad_mbr";
        NextHopKey primary_nh(IpAddress("10.0.0.1"), string("Ethernet0"));
        NextHopKey standby_nh(IpAddress("10.0.0.100"), string("Ethernet4"));
        NextHopKey unknown_nh(IpAddress("10.0.0.99"), string("Ethernet0"));
        registerNextHop(primary_nh);
        registerNextHop(standby_nh);

        ASSERT_TRUE(gNhgOrch->createProtNhg(key, primary_nh, standby_nh));
        EXPECT_FALSE(gNhgOrch->attachProtNhgMonitoredObject(
            key, unknown_nh, monitoredOid()));

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
        registerNextHop(primary_nh);
        registerNextHop(standby_nh);

        EXPECT_FALSE(gNhgOrch->createProtNhg(key, primary_nh, standby_nh));
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
        registerNextHop(primary_nh);
        registerNextHop(standby_nh);

        ASSERT_TRUE(gNhgOrch->createProtNhg(key, primary_nh, standby_nh));

        auto &nhg = const_cast<ProtNhg&>(gNhgOrch->getProtNhg(key));
        EXPECT_TRUE(nhg.sync());

        ASSERT_TRUE(gNhgOrch->removeProtNhg(key));
        unregisterNextHop(primary_nh);
        unregisterNextHop(standby_nh);
    }

    TEST_F(ProtNhgTest, SyncFailsWhenPrimaryAndStandbyAreIdentical)
    {
        NextHopKey nh(IpAddress("10.0.0.1"), string("Ethernet0"));
        registerNextHop(nh);

        /*
         * Primary and standby resolve to the same NextHopKey, so the
         * constructor's second m_members.emplace() is a no-op (map keys
         * must be unique) and the group ends up with a single member.
         * sync() must reject this rather than silently creating a
         * degenerate 1-member "protection" group.
         */
        ProtNhg nhg("prot_nhg_dup_nh", nh, nh);
        EXPECT_FALSE(nhg.sync());
        EXPECT_FALSE(nhg.isSynced());

        unregisterNextHop(nh);
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
        registerNextHop(primary_nh);
        registerNextHop(standby_nh);

        /*
         * The group's SAI object was created successfully, only the members
         * failed to sync. createProtNhg() still reports success since the
         * group is registered; it isn't stuck forever, as a later
         * validateNextHop() call can complete it.
         */
        EXPECT_TRUE(gNhgOrch->createProtNhg(key, primary_nh, standby_nh));
        EXPECT_TRUE(gNhgOrch->hasProtNhg(key));
        EXPECT_NE(gNhgOrch->getProtNhgId(key), SAI_NULL_OBJECT_ID);

        ASSERT_TRUE(gNhgOrch->removeProtNhg(key));
        unregisterNextHop(primary_nh);
        unregisterNextHop(standby_nh);
    }

    TEST_F(ProtNhgTest, UnresolvedMemberSyncedLaterViaValidateNextHop)
    {
        string key = "prot_nhg_unresolved";
        NextHopKey primary_nh(IpAddress("10.0.0.1"), string("Ethernet0"));
        NextHopKey standby_nh(IpAddress("10.0.0.100"), string("Ethernet4"));

        /* Only the primary's next hop is resolved; the standby's next hop
         * hasn't been learned by NeighOrch yet (e.g. ARP still pending). */
        registerNextHop(primary_nh);

        EXPECT_TRUE(gNhgOrch->createProtNhg(key, primary_nh, standby_nh));
        ASSERT_TRUE(gNhgOrch->hasProtNhg(key));

        const ProtNhg &nhg = gNhgOrch->getProtNhg(key);
        EXPECT_NE(nhg.getId(), SAI_NULL_OBJECT_ID);

        /* Both members must still be tracked -- the unresolved standby must
         * not have been dropped from the group (Tamer's "zero members"
         * concern from the original review). */
        EXPECT_EQ(nhg.getSize(), 2u);

        const ProtNhgMember *primary = nhg.getPrimaryMember();
        ASSERT_NE(primary, nullptr);
        EXPECT_TRUE(primary->isSynced());

        const ProtNhgMember *standby = nhg.getStandbyMember();
        ASSERT_NE(standby, nullptr);
        EXPECT_FALSE(standby->isSynced());

        /* The standby's next hop resolves; NeighOrch::addNextHop() would
         * call gNhgOrch->validateNextHop() at this point. */
        registerNextHop(standby_nh);
        EXPECT_TRUE(gNhgOrch->validateNextHop(standby_nh));

        standby = nhg.getStandbyMember();
        ASSERT_NE(standby, nullptr);
        EXPECT_TRUE(standby->isSynced());

        ASSERT_TRUE(gNhgOrch->removeProtNhg(key));
        unregisterNextHop(primary_nh);
        unregisterNextHop(standby_nh);
    }

    TEST_F(ProtNhgTest, ValidateNextHopNoOpWhenMemberNotFound)
    {
        NextHopKey unrelated_nh(IpAddress("10.0.0.200"), string("Ethernet8"));

        /* No group exists yet, so this doesn't exercise ProtNhg's own guard. */
        EXPECT_TRUE(gNhgOrch->validateNextHop(unrelated_nh));
        EXPECT_TRUE(gNhgOrch->invalidateNextHop(unrelated_nh));

        /* Call directly, bypassing NhgOrch's guard, to test ProtNhg's own. */
        string key = "prot_nhg_unrelated_member";
        NextHopKey primary_nh(IpAddress("10.0.1.1"), string("Ethernet0"));
        NextHopKey standby_nh(IpAddress("10.0.1.100"), string("Ethernet4"));
        registerNextHop(primary_nh);
        registerNextHop(standby_nh);

        ASSERT_TRUE(gNhgOrch->createProtNhg(key, primary_nh, standby_nh));
        ProtNhg &nhg = *gNhgOrch->m_protNhgs.at(key).nhg;

        ASSERT_FALSE(nhg.hasMember(unrelated_nh));
        EXPECT_TRUE(nhg.validateNextHop(unrelated_nh));

        ASSERT_TRUE(gNhgOrch->removeProtNhg(key));
        unregisterNextHop(primary_nh);
        unregisterNextHop(standby_nh);
    }

    /*
     * NeighOrch calls invalidateNextHop() when an interface goes down. A
     * protection NHG keeps both legs programmed and switches over instead, so
     * the call must leave its members alone.
     */
    TEST_F(ProtNhgTest, InvalidateNextHopLeavesProtectionMembersProgrammed)
    {
        string key = "prot_nhg_invalidate";
        NextHopKey primary_nh(IpAddress("10.0.0.1"), string("Ethernet0"));
        NextHopKey standby_nh(IpAddress("10.0.0.100"), string("Ethernet4"));
        registerNextHop(primary_nh);
        registerNextHop(standby_nh);

        ASSERT_TRUE(gNhgOrch->createProtNhg(key, primary_nh, standby_nh));

        const ProtNhg &nhg = gNhgOrch->getProtNhg(key);
        ASSERT_NE(nhg.getPrimaryMember(), nullptr);
        ASSERT_NE(nhg.getStandbyMember(), nullptr);
        ASSERT_TRUE(nhg.getPrimaryMember()->isSynced());
        ASSERT_TRUE(nhg.getStandbyMember()->isSynced());

        sai_object_id_t primary_gm_id = nhg.getPrimaryMember()->getId();
        sai_object_id_t standby_gm_id = nhg.getStandbyMember()->getId();

        /* e.g. either interface goes down. */
        EXPECT_TRUE(gNhgOrch->invalidateNextHop(standby_nh));
        EXPECT_TRUE(gNhgOrch->invalidateNextHop(primary_nh));

        EXPECT_TRUE(nhg.getPrimaryMember()->isSynced());
        EXPECT_TRUE(nhg.getStandbyMember()->isSynced());

        /* The SAI members were not torn down and recreated either. */
        EXPECT_EQ(nhg.getPrimaryMember()->getId(), primary_gm_id);
        EXPECT_EQ(nhg.getStandbyMember()->getId(), standby_gm_id);

        /* Coming back up is a no-op for an already synced member. */
        EXPECT_TRUE(gNhgOrch->validateNextHop(standby_nh));
        EXPECT_TRUE(nhg.getStandbyMember()->isSynced());
        EXPECT_EQ(nhg.getStandbyMember()->getId(), standby_gm_id);

        ASSERT_TRUE(gNhgOrch->removeProtNhg(key));
        unregisterNextHop(primary_nh);
        unregisterNextHop(standby_nh);
    }

    TEST_F(ProtNhgTest, SetAdminRoleUnsyncedNhg)
    {
        NextHopKey primary_nh(IpAddress("10.0.0.1"), string("Ethernet0"));
        NextHopKey standby_nh(IpAddress("10.0.0.100"), string("Ethernet4"));
        ProtNhg nhg("unsynced_nhg", primary_nh, standby_nh);

        /* Attaching before sync only records the intent, so the group counts
         * as HW-autonomous and the write is rejected by the sync guard. */
        ASSERT_TRUE(nhg.updateMemberMonitoredObject(primary_nh, monitoredOid()));
        ASSERT_TRUE(nhg.isHwAutonomous());

        EXPECT_FALSE(nhg.setAdminRole(SAI_NEXT_HOP_GROUP_ADMIN_ROLE_PRIMARY));
    }

    TEST_F(ProtNhgTest, SetAdminRoleOnSwDrivenNhgFails)
    {
        NextHopKey primary_nh(IpAddress("10.0.0.1"), string("Ethernet0"));
        NextHopKey standby_nh(IpAddress("10.0.0.100"), string("Ethernet4"));
        ProtNhg nhg("sw_driven_nhg", primary_nh, standby_nh);

        ASSERT_FALSE(nhg.isHwAutonomous());
        EXPECT_FALSE(nhg.setAdminRole(SAI_NEXT_HOP_GROUP_ADMIN_ROLE_PRIMARY));
    }

    TEST_F(ProtNhgTest, UpdateMonitoredObjectUnsynced)
    {
        NextHopKey primary_nh(IpAddress("10.0.0.1"), string("Ethernet0"));
        NextHopKey standby_nh(IpAddress("10.0.0.100"), string("Ethernet4"));
        ProtNhg nhg("unsynced_mon", primary_nh, standby_nh);
        EXPECT_TRUE(nhg.updateMemberMonitoredObject(standby_nh, monitoredOid()));
    }

    TEST_F(ProtNhgTest, UpdateMonitoredObjectSaiFailure)
    {
        string key = "prot_nhg_mon_sai_fail";
        NextHopKey primary_nh(IpAddress("10.0.0.1"), string("Ethernet0"));
        NextHopKey standby_nh(IpAddress("10.0.0.100"), string("Ethernet4"));
        registerNextHop(primary_nh);
        registerNextHop(standby_nh);

        ASSERT_TRUE(gNhgOrch->createProtNhg(key, primary_nh, standby_nh));

        EXPECT_CALL(*mock_sai_next_hop_group_api,
                    set_next_hop_group_member_attribute(_, _))
            .WillOnce(Return(SAI_STATUS_FAILURE));

        EXPECT_FALSE(gNhgOrch->attachProtNhgMonitoredObject(
            key, standby_nh, monitoredOid()));

        ASSERT_TRUE(gNhgOrch->removeProtNhg(key));
        unregisterNextHop(primary_nh);
        unregisterNextHop(standby_nh);
    }

    TEST_F(ProtNhgTest, ObservedRoleUnsyncedMember)
    {
        string key = "prot_nhg_obs_unsynced";
        NextHopKey primary_nh(IpAddress("10.0.0.1"), string("Ethernet0"));
        NextHopKey standby_nh(IpAddress("10.0.0.100"), string("Ethernet4"));
        registerNextHop(standby_nh);

        ASSERT_TRUE(gNhgOrch->createProtNhg(key, primary_nh, standby_nh));

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
        registerNextHop(primary_nh);
        registerNextHop(standby_nh);

        ASSERT_TRUE(gNhgOrch->createProtNhg(key, primary_nh, standby_nh));

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
        registerNextHop(primary_nh);
        registerNextHop(standby_nh);

        ASSERT_TRUE(gNhgOrch->createProtNhg(key, primary_nh, standby_nh));

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
        registerNextHop(primary_nh);
        registerNextHop(standby_nh);

        ASSERT_TRUE(gNhgOrch->createProtNhg(key, primary_nh, standby_nh));

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
        registerNextHop(primary_nh);
        registerNextHop(standby_nh);

        ASSERT_TRUE(gNhgOrch->createProtNhg(key, primary_nh, standby_nh));

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
        registerNextHop(primary_nh);
        registerNextHop(standby_nh);

        ASSERT_TRUE(gNhgOrch->createProtNhg(key, primary_nh, standby_nh));

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
        registerNextHop(primary_nh);
        registerNextHop(standby_nh);

        ASSERT_TRUE(gNhgOrch->createProtNhg(key, primary_nh, standby_nh));

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
        registerNextHop(primary_nh);
        registerNextHop(standby_nh);

        ASSERT_TRUE(gNhgOrch->createProtNhg(key, primary_nh, standby_nh));

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
        registerNextHop(primary_nh);
        registerNextHop(standby_nh);

        ASSERT_TRUE(gNhgOrch->createProtNhg(key, primary_nh, standby_nh));

        const ProtNhg &nhg = gNhgOrch->getProtNhg(key);
        const ProtNhgMember *standby = nhg.getStandbyMember();
        ASSERT_NE(standby, nullptr);
        string sstr = standby->to_string();
        EXPECT_FALSE(sstr.empty());
        EXPECT_NE(sstr.find("standby"), string::npos);

        const ProtNhgMember *primary = nhg.getPrimaryMember();
        ASSERT_NE(primary, nullptr);
        string pstr = primary->to_string();
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
        ProtNhg nhg1("nhg_move", primary_nh, standby_nh);
        ProtNhg nhg2(std::move(nhg1));

        EXPECT_NE(nhg2.getStandbyMember(), nullptr);
        EXPECT_EQ(nhg2.to_string(), "nhg_move");
    }

    TEST_F(ProtNhgTest, SyncWithMonitoredObject)
    {
        NextHopKey primary_nh(IpAddress("10.0.0.1"), string("Ethernet0"));
        NextHopKey standby_nh(IpAddress("10.0.0.100"), string("Ethernet4"));
        sai_object_id_t session_oid = 0xABCD;
        registerNextHop(primary_nh);
        registerNextHop(standby_nh);

        ProtNhg nhg("nhg_mon_sync", primary_nh, standby_nh);

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

    TEST_F(ProtNhgTest, CreateProtNhgWithNhgKeysFailsWhenRepresentativesCollide)
    {
        /*
         * The ProtNhg(NextHopGroupKey, NextHopGroupKey) constructor keys each
         * member on *nhg_key.getNextHops().begin(), not the full group key.
         * Here the primary and standby groups both contain 10.0.0.1@Ethernet0,
         * which sorts first in both, so the second m_members.emplace() is a
         * no-op and the group would collapse to a single member if sync()
         * didn't reject it (see SyncFailsWhenPrimaryAndStandbyAreIdentical for
         * the equivalent direct-NextHopKey-overload case).
         */
        NextHopGroupKey primary_nhg_key("10.0.0.1@Ethernet0,10.0.0.2@Ethernet0");
        NextHopGroupKey standby_nhg_key("10.0.0.1@Ethernet0,10.0.0.100@Ethernet4");

        addEcmpNhg(primary_nhg_key);
        addEcmpNhg(standby_nhg_key);

        EXPECT_CALL(*mock_sai_next_hop_group_api, create_next_hop_group(_, _, _, _)).Times(0);

        string key = "prot_nhg_keys_rep_collision";
        EXPECT_FALSE(gNhgOrch->createProtNhg(key, primary_nhg_key, standby_nhg_key));
        EXPECT_FALSE(gNhgOrch->hasProtNhg(key));

        removeEcmpNhg(primary_nhg_key.to_string());
        removeEcmpNhg(standby_nhg_key.to_string());
    }

    /* Every protection NHG is created with the one protection type; there is
     * no separate HW-protection group type. */
    TEST_F(ProtNhgTest, CreateUsesProtectionGroupType)
    {
        string key = "prot_nhg_type";
        NextHopKey primary_nh(IpAddress("10.0.0.1"), string("Ethernet0"));
        NextHopKey standby_nh(IpAddress("10.0.0.100"), string("Ethernet4"));
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

        ASSERT_TRUE(gNhgOrch->createProtNhg(key, primary_nh, standby_nh));
        EXPECT_TRUE(gNhgOrch->hasProtNhg(key));

        ASSERT_TRUE(gNhgOrch->removeProtNhg(key));
        unregisterNextHop(primary_nh);
        unregisterNextHop(standby_nh);
    }

    TEST_F(ProtNhgTest, SetSwitchoverSuccess)
    {
        string key = "prot_nhg_switchover";
        NextHopKey primary_nh(IpAddress("10.0.0.1"), string("Ethernet0"));
        NextHopKey standby_nh(IpAddress("10.0.0.100"), string("Ethernet4"));
        registerNextHop(primary_nh);
        registerNextHop(standby_nh);

        ASSERT_TRUE(gNhgOrch->createProtNhg(key, primary_nh, standby_nh));

        EXPECT_CALL(*mock_sai_next_hop_group_api,
                    set_next_hop_group_attribute(_, _))
            .Times(1)
            .WillOnce(Return(SAI_STATUS_SUCCESS));

        EXPECT_TRUE(gNhgOrch->setProtNhgSwitchover(key, true));

        ASSERT_TRUE(gNhgOrch->removeProtNhg(key));
        unregisterNextHop(primary_nh);
        unregisterNextHop(standby_nh);
    }

    TEST_F(ProtNhgTest, SetSwitchoverOnHwAutonomousNhgFails)
    {
        string key = "prot_nhg_switchover_hw";
        NextHopKey primary_nh(IpAddress("10.0.0.1"), string("Ethernet0"));
        NextHopKey standby_nh(IpAddress("10.0.0.100"), string("Ethernet4"));
        registerNextHop(primary_nh);
        registerNextHop(standby_nh);

        ASSERT_TRUE(gNhgOrch->createProtNhg(key, primary_nh, standby_nh));
        ASSERT_TRUE(gNhgOrch->attachProtNhgMonitoredObject(
            key, primary_nh, monitoredOid()));

        /* The hardware owns the decision now, so software must not trigger
         * a switchover behind its back. */
        EXPECT_FALSE(gNhgOrch->setProtNhgSwitchover(key, true));

        /* Detaching hands the trigger back. */
        ASSERT_TRUE(gNhgOrch->detachProtNhgMonitoredObject(key, primary_nh));
        EXPECT_TRUE(gNhgOrch->setProtNhgSwitchover(key, true));

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
        registerNextHop(primary_nh);
        registerNextHop(standby_nh);

        string expected_key = NhgOrch::buildProtNhgKey(primary_nh, standby_nh);
        EXPECT_FALSE(expected_key.empty());

        ASSERT_TRUE(gNhgOrch->createProtNhg(primary_nh, standby_nh));
        EXPECT_TRUE(gNhgOrch->hasProtNhg(expected_key));

        EXPECT_TRUE(gNhgOrch->createProtNhg(primary_nh, standby_nh));

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

    TEST_F(ProtNhgTest, BuildProtNhgKeyDiffersByPrimary)
    {
        NextHopKey nh_a(IpAddress("10.0.0.1"), string("Ethernet0"));
        NextHopKey nh_b(IpAddress("10.0.0.2"), string("Ethernet0"));
        NextHopKey standby(IpAddress("10.0.0.100"), string("Ethernet4"));

        string key_ab = NhgOrch::buildProtNhgKey(nh_a, standby);
        string key_ba = NhgOrch::buildProtNhgKey(nh_b, standby);

        EXPECT_NE(key_ab, key_ba);
    }

    /* --- Protection NHG key format --- */

    TEST_F(ProtNhgTest, BuildProtNhgKeyFromNextHops)
    {
        NextHopKey primary(IpAddress("10.0.0.1"), string("Ethernet0"));
        NextHopKey standby(IpAddress("10.0.0.100"), string("Ethernet4"));

        string key = NhgOrch::buildProtNhgKey(primary, standby);
        EXPECT_EQ(key, "prot:10.0.0.1@Ethernet0|10.0.0.100@Ethernet4");
    }

    TEST_F(ProtNhgTest, BuildProtNhgKeyFromNhgKeys)
    {
        NextHopGroupKey primary("10.0.0.1@Ethernet0,10.0.0.2@Ethernet0");
        NextHopGroupKey standby("10.0.0.100@Ethernet4");

        string key = NhgOrch::buildProtNhgKey(primary, standby);
        EXPECT_EQ(key.substr(0, 5), "prot:");
        EXPECT_NE(key.find(primary.to_string()), string::npos);
        EXPECT_NE(key.find(standby.to_string()), string::npos);
    }

    /* Membership alone identifies a group, so the key must not move when the
     * switchover mode does. */
    TEST_F(ProtNhgTest, KeyIsStableAcrossModeChanges)
    {
        NextHopKey primary_nh(IpAddress("10.0.0.1"), string("Ethernet0"));
        NextHopKey standby_nh(IpAddress("10.0.0.100"), string("Ethernet4"));
        registerNextHop(primary_nh);
        registerNextHop(standby_nh);

        string key = NhgOrch::buildProtNhgKey(primary_nh, standby_nh);
        ASSERT_TRUE(gNhgOrch->createProtNhg(primary_nh, standby_nh));
        ASSERT_TRUE(gNhgOrch->hasProtNhg(key));

        sai_object_id_t nhg_id = gNhgOrch->getProtNhgId(key);

        ASSERT_TRUE(gNhgOrch->attachProtNhgMonitoredObject(
            key, primary_nh, monitoredOid()));
        EXPECT_TRUE(gNhgOrch->hasProtNhg(key));
        EXPECT_EQ(gNhgOrch->getProtNhgId(key), nhg_id);

        ASSERT_TRUE(gNhgOrch->detachProtNhgMonitoredObject(key, primary_nh));
        EXPECT_TRUE(gNhgOrch->hasProtNhg(key));
        EXPECT_EQ(gNhgOrch->getProtNhgId(key), nhg_id);

        ASSERT_TRUE(gNhgOrch->removeProtNhg(key));
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
        const ProtNhgMember *primary = nhg.getPrimaryMember();
        ASSERT_NE(primary, nullptr);
        EXPECT_TRUE(primary->isRecursive());
        EXPECT_NE(primary->getNhId(), SAI_NULL_OBJECT_ID);

        const ProtNhgMember *standby = nhg.getStandbyMember();
        ASSERT_NE(standby, nullptr);
        EXPECT_TRUE(standby->isRecursive());
        EXPECT_NE(standby->getNhId(), SAI_NULL_OBJECT_ID);

        ASSERT_TRUE(gNhgOrch->removeProtNhg(key));
        removeEcmpNhg(primary_nhg_key.to_string());
        removeEcmpNhg(standby_nhg_key.to_string());
    }

    /* --- Protection capabilities are published to STATE_DB --- */

    TEST_F(ProtNhgTest, ProtectionCapabilitiesPublishedToStateDb)
    {
        /* Undo PostSetUp()'s forced capability state so this test exercises
         * the real probe; it only cares about publish behavior, not the
         * specific values a mock switch reports. */
        gNhgOrch->m_protCapChecked = false;
        gNhgOrch->m_protectionSupported = false;
        gNhgOrch->m_backupGroupHintSupported = false;
        gNhgOrch->m_monitoredObjectTypes.clear();

        /* One probe must publish all four capability fields to the standard
         * switch capability row, whatever the platform reports. */
        bool protection = gNhgOrch->isProtectionSupported();
        bool hw_switchover = gNhgOrch->isHwSwitchoverSupported();
        bool hint = gNhgOrch->isBackupGroupHintSupported();

        /* get_switch_capability() leaves val untouched when the field is
         * absent, so start each read from a known sentinel. */
        auto readCapability = [](const string &field) {
            string val = "<unset>";
            gSwitchOrch->get_switch_capability(field, val);
            return val;
        };

        EXPECT_EQ(readCapability(SWITCH_CAPABILITY_TABLE_NHG_PROTECTION_CAPABLE),
                  protection ? "true" : "false");
        EXPECT_EQ(readCapability(SWITCH_CAPABILITY_TABLE_NHG_HW_SWITCHOVER_CAPABLE),
                  hw_switchover ? "true" : "false");
        EXPECT_EQ(readCapability(SWITCH_CAPABILITY_TABLE_NHG_BACKUP_GROUP_HINT_CAPABLE),
                  hint ? "true" : "false");

        /* Hardware switchover support is derived from the monitored object
         * type list, so the two must always agree. */
        EXPECT_EQ(readCapability(SWITCH_CAPABILITY_TABLE_NHG_MONITORED_OBJECT_TYPES).empty(),
                  !hw_switchover);

        /* The retired per-type fields must not be written. */
        EXPECT_EQ(readCapability("SW_NHG_PROTECTION_CAPABLE"), "<unset>");
        EXPECT_EQ(readCapability("HW_NHG_PROTECTION_CAPABLE"), "<unset>");
    }

    TEST_F(ProtNhgTest, CreateFailsWhenProtectionUnsupported)
    {
        /* Simulate an ASIC that doesn't support SAI_NEXT_HOP_GROUP_TYPE_PROTECTION. */
        gNhgOrch->m_protectionSupported = false;

        NextHopKey primary_nh(IpAddress("10.0.0.1"), string("Ethernet0"));
        NextHopKey standby_nh(IpAddress("10.0.0.100"), string("Ethernet4"));
        registerNextHop(primary_nh);
        registerNextHop(standby_nh);

        /* createProtNhg() must reject the request before ever touching SAI. */
        EXPECT_CALL(*mock_sai_next_hop_group_api, create_next_hop_group(_, _, _, _)).Times(0);

        string key = "prot_nhg_unsupported";
        EXPECT_FALSE(gNhgOrch->createProtNhg(key, primary_nh, standby_nh));
        EXPECT_FALSE(gNhgOrch->hasProtNhg(key));

        unregisterNextHop(primary_nh);
        unregisterNextHop(standby_nh);
    }

    /* A platform with no monitored object support still gets working
     * protection NHGs; they just stay SW-driven. */
    TEST_F(ProtNhgTest, ProtectionWorksWithoutHwSwitchoverSupport)
    {
        gNhgOrch->m_monitoredObjectTypes.clear();
        EXPECT_FALSE(gNhgOrch->isHwSwitchoverSupported());

        string key = "prot_nhg_no_hw_switchover";
        NextHopKey primary_nh(IpAddress("10.0.0.1"), string("Ethernet0"));
        NextHopKey standby_nh(IpAddress("10.0.0.100"), string("Ethernet4"));
        registerNextHop(primary_nh);
        registerNextHop(standby_nh);

        ASSERT_TRUE(gNhgOrch->createProtNhg(key, primary_nh, standby_nh));

        /* No type is attachable, so every attach is refused ... */
        EXPECT_FALSE(gNhgOrch->attachProtNhgMonitoredObject(
            key, primary_nh, monitoredOid()));

        /* ... and the group keeps failing over under software control. */
        EXPECT_FALSE(gNhgOrch->getProtNhg(key).isHwAutonomous());
        EXPECT_TRUE(gNhgOrch->setProtNhgSwitchover(key, true));

        ASSERT_TRUE(gNhgOrch->removeProtNhg(key));
        unregisterNextHop(primary_nh);
        unregisterNextHop(standby_nh);
    }

    /* --- CRM resource accounting --- */

    TEST_F(ProtNhgTest, CrmAccountingOnCreateAndRemove)
    {
        NextHopKey primary_nh(IpAddress("10.0.0.1"), string("Ethernet0"));
        NextHopKey standby_nh(IpAddress("10.0.0.100"), string("Ethernet4"));

        registerNextHop(primary_nh);
        registerNextHop(standby_nh);

        uint32_t grp_before = crmUsed(CrmResourceType::CRM_NEXTHOP_GROUP);
        uint32_t mbr_before = crmUsed(CrmResourceType::CRM_NEXTHOP_GROUP_MEMBER);

        string key = "prot_crm";
        ASSERT_TRUE(gNhgOrch->createProtNhg(key, primary_nh, standby_nh));

        /* One protection group with two synced members (1 primary + 1 standby). */
        EXPECT_EQ(crmUsed(CrmResourceType::CRM_NEXTHOP_GROUP), grp_before + 1);
        EXPECT_EQ(crmUsed(CrmResourceType::CRM_NEXTHOP_GROUP_MEMBER), mbr_before + 2);

        ASSERT_TRUE(gNhgOrch->removeProtNhg(key));

        /* Removal must release both the group and the member counters. */
        EXPECT_EQ(crmUsed(CrmResourceType::CRM_NEXTHOP_GROUP), grp_before);
        EXPECT_EQ(crmUsed(CrmResourceType::CRM_NEXTHOP_GROUP_MEMBER), mbr_before);

        unregisterNextHop(primary_nh);
        unregisterNextHop(standby_nh);
    }
 }
