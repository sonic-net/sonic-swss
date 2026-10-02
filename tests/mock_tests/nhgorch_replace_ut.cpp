/* Grants access to NhgOrch/NextHopGroup/RouteOrch protected/private state
 * for direct seeding below; must precede mock_orchagent_main.h, which
 * includes these headers unguarded. */
#include "directory.h"
#define protected public
#include "orch.h"
#undef protected

#define private public
#define protected public
#include "crmorch.h"
#include "srv6orch.h"
#include "nhgbase.h"
#include "nhgorch.h"
#undef protected
#undef private

#include "mock_orch_test.h"
#include "mock_orchagent_main.h"
#include "mock_sai_api.h"
#include "ut_helper.h"

#include <gtest/gtest.h>

using namespace std;
using namespace swss;

EXTERN_MOCK_FNS

namespace nhgorch_replace_test
{

DEFINE_SAI_GENERIC_APIS_MOCK(next_hop_group, next_hop_group, next_hop_group_member)
DEFINE_SAI_API_MOCK_SPECIFY_ENTRY_WITH_SET(route, route)
DEFINE_SAI_API_MOCK_SPECIFY_ENTRY_WITH_SET(mpls, inseg)

using ::testing::_;
using ::testing::DoAll;
using ::testing::Return;
using ::testing::SetArgPointee;
using namespace mock_orch_test;

static int g_route_set_calls;
static sai_object_id_t g_route_set_oid;
static NextHopKey g_watch_nh;
static int g_watch_ref_at_set;
static sai_object_id_t g_fail_route_oid;
static int g_label_set_calls;
static sai_object_id_t g_label_set_oid;
static sai_object_id_t g_fail_label_oid;
static sai_object_id_t g_member_oid;

class NhgOrchReplaceTest : public MockOrchTest
{
protected:
    const NextHopKey nh1{"10.0.0.1@Ethernet0"};
    const NextHopKey nh2{"10.0.0.2@Ethernet4"};
    const sai_object_id_t nh1_oid = 0x4401;
    const sai_object_id_t nh2_oid = 0x4402;
    const sai_object_id_t group_oid = 0x5100;
    const IpPrefix pfx{"99.99.98.0/24"};
    const Label label = 3001;
    const string index{"test_replace_nhg"};

    void PostSetUp() override
    {
        INIT_SAI_API_MOCK(next_hop_group);
        INIT_SAI_API_MOCK(route);
        INIT_SAI_API_MOCK(mpls);
        MockSaiApis();

        g_route_set_calls = 0;
        g_route_set_oid = SAI_NULL_OBJECT_ID;
        g_watch_nh = nh1;
        g_watch_ref_at_set = -1;
        g_fail_route_oid = SAI_NULL_OBJECT_ID;
        g_label_set_calls = 0;
        g_label_set_oid = SAI_NULL_OBJECT_ID;
        g_fail_label_oid = SAI_NULL_OBJECT_ID;
        g_member_oid = 0x6100;

        ON_CALL(*mock_sai_route_api, set_route_entries_attribute(_, _, _, _, _))
            .WillByDefault([](uint32_t count, const sai_route_entry_t *, const sai_attribute_t *attrs,
                              sai_bulk_op_error_mode_t, sai_status_t *statuses) {
                for (uint32_t i = 0; i < count; i++)
                {
                    g_route_set_calls++;
                    g_route_set_oid = attrs[i].value.oid;
                    g_watch_ref_at_set = gNeighOrch->m_syncdNextHops[g_watch_nh].ref_count;
                    statuses[i] = attrs[i].value.oid == g_fail_route_oid ? SAI_STATUS_FAILURE : SAI_STATUS_SUCCESS;
                }
                return SAI_STATUS_SUCCESS;
            });
        ON_CALL(*mock_sai_mpls_api, set_inseg_entries_attribute(_, _, _, _, _))
            .WillByDefault([](uint32_t count, const sai_inseg_entry_t *, const sai_attribute_t *attrs,
                              sai_bulk_op_error_mode_t, sai_status_t *statuses) {
                for (uint32_t i = 0; i < count; i++)
                {
                    g_label_set_calls++;
                    g_label_set_oid = attrs[i].value.oid;
                    statuses[i] = attrs[i].value.oid == g_fail_label_oid ? SAI_STATUS_FAILURE : SAI_STATUS_SUCCESS;
                }
                return SAI_STATUS_SUCCESS;
            });
        ON_CALL(*mock_sai_next_hop_group_api, create_next_hop_group(_, _, _, _))
            .WillByDefault(DoAll(SetArgPointee<0>(group_oid), Return(SAI_STATUS_SUCCESS)));
        ON_CALL(*mock_sai_next_hop_group_api, remove_next_hop_group(_))
            .WillByDefault(Return(SAI_STATUS_SUCCESS));
        ON_CALL(*mock_sai_next_hop_group_api, create_next_hop_group_members(_, _, _, _, _, _, _))
            .WillByDefault([](sai_object_id_t, uint32_t count, const uint32_t *, const sai_attribute_t **,
                              sai_bulk_op_error_mode_t, sai_object_id_t *ids, sai_status_t *statuses) {
                for (uint32_t i = 0; i < count; i++)
                {
                    ids[i] = g_member_oid++;
                    statuses[i] = SAI_STATUS_SUCCESS;
                }
                return SAI_STATUS_SUCCESS;
            });
        ON_CALL(*mock_sai_next_hop_group_api, remove_next_hop_group_members(_, _, _, _))
            .WillByDefault([](uint32_t count, const sai_object_id_t *, sai_bulk_op_error_mode_t,
                              sai_status_t *statuses) {
                for (uint32_t i = 0; i < count; i++)
                {
                    statuses[i] = SAI_STATUS_SUCCESS;
                }
                return SAI_STATUS_SUCCESS;
            });
    }

    void PreTearDown() override
    {
        cleanup();
        RestoreSaiApis();
        DEINIT_SAI_API_MOCK(mpls);
        DEINIT_SAI_API_MOCK(route);
        DEINIT_SAI_API_MOCK(next_hop_group);
    }

    /* A group over nhg_str, used by one route. */
    void seed(const string &nhg_str = "10.0.0.1@Ethernet0")
    {
        gNeighOrch->m_syncdNextHops[nh1] = { nh1_oid, 0, 0 };
        gNeighOrch->m_syncdNextHops[nh2] = { nh2_oid, 0, 0 };

        auto nhg = std::make_unique<NextHopGroup>(NextHopGroupKey(nhg_str, string("")), false);
        ASSERT_TRUE(nhg->sync());
        /* One reference: the route below. */
        gNhgOrch->m_syncdNextHopGroups.emplace(index, NhgEntry<NextHopGroup>(std::move(nhg), 1));
        gRouteOrch->m_syncdRoutes[gVirtualRouterId][pfx].nhg_index = index;
    }

    void cleanup()
    {
        gRouteOrch->m_syncdRoutes[gVirtualRouterId].erase(pfx);
        gRouteOrch->m_syncdLabelRoutes[gVirtualRouterId].erase(label);
        gNhgOrch->m_syncdNextHopGroups.erase(index);
        gNeighOrch->m_syncdNextHops.erase(nh1);
        gNeighOrch->m_syncdNextHops.erase(nh2);
    }
};

/* The routes move to the new ID while the old next hop is still counted, and
 * only then is the old group released. */
TEST_F(NhgOrchReplaceTest, MovesRoutesBeforeReleasingTheOldNextHop)
{
    ASSERT_NO_FATAL_FAILURE(seed());
    ASSERT_EQ(gNhgOrch->m_syncdNextHopGroups.at(index).nhg->getId(), nh1_oid);
    NextHopGroupKey new_key("10.0.0.2@Ethernet4", string(""));
    ASSERT_TRUE(gNhgOrch->m_syncdNextHopGroups.at(index).nhg->replacesIdOnUpdate(new_key));

    EXPECT_TRUE(gNhgOrch->replaceNhg(index, new_key));

    EXPECT_EQ(g_route_set_calls, 1);
    EXPECT_EQ(g_route_set_oid, nh2_oid);
    EXPECT_EQ(g_watch_ref_at_set, 1);
    EXPECT_EQ(gNhgOrch->m_syncdNextHopGroups.at(index).nhg->getId(), nh2_oid);
    EXPECT_EQ(gNeighOrch->m_syncdNextHops[nh1].ref_count, 0);
    EXPECT_EQ(gNeighOrch->m_syncdNextHops[nh2].ref_count, 1);
}

/* The case from the field: one next hop grows into a SAI group. */
TEST_F(NhgOrchReplaceTest, GrowingToSeveralNextHopsMovesRoutesToTheNewGroup)
{
    ASSERT_NO_FATAL_FAILURE(seed());
    EXPECT_CALL(*mock_sai_next_hop_group_api, create_next_hop_group(_, _, _, _)).Times(1);

    NextHopGroupKey new_key("10.0.0.1@Ethernet0,10.0.0.2@Ethernet4", string(""));
    EXPECT_TRUE(gNhgOrch->replaceNhg(index, new_key));

    EXPECT_EQ(g_route_set_calls, 1);
    EXPECT_EQ(g_route_set_oid, group_oid);
    EXPECT_EQ(gNhgOrch->m_syncdNextHopGroups.at(index).nhg->getId(), group_oid);
    /* nh1 is now referenced by the group's member only. */
    EXPECT_EQ(gNeighOrch->m_syncdNextHops[nh1].ref_count, 1);
}

TEST_F(NhgOrchReplaceTest, KeepsTheOldGroupWhenTheRoutesCannotMove)
{
    ASSERT_NO_FATAL_FAILURE(seed());
    g_fail_route_oid = nh2_oid;

    EXPECT_FALSE(gNhgOrch->replaceNhg(index, NextHopGroupKey("10.0.0.2@Ethernet4", string(""))));

    /* The failed move, then the move back. */
    EXPECT_EQ(g_route_set_calls, 2);
    EXPECT_EQ(g_route_set_oid, nh1_oid);
    EXPECT_EQ(gNhgOrch->m_syncdNextHopGroups.at(index).nhg->getId(), nh1_oid);
    EXPECT_EQ(gNeighOrch->m_syncdNextHops[nh1].ref_count, 1);
    EXPECT_EQ(gNeighOrch->m_syncdNextHops[nh2].ref_count, 0);
}

/* An SRv6 route with a PIC context programs the group's ID as its next hop
 * too, so it moves with the plain routes. */
TEST_F(NhgOrchReplaceTest, MovesRoutesWithAContextIndex)
{
    ASSERT_NO_FATAL_FAILURE(seed());
    const IpPrefix pic_pfx{"99.99.97.0/24"};
    auto &pic_rt = gRouteOrch->m_syncdRoutes[gVirtualRouterId][pic_pfx];
    pic_rt.nhg_index = index;
    pic_rt.context_index = "test_replace_ctx";
    gNhgOrch->m_syncdNextHopGroups.at(index).ref_count = 2;

    EXPECT_TRUE(gNhgOrch->replaceNhg(index, NextHopGroupKey("10.0.0.2@Ethernet4", string(""))));

    gRouteOrch->m_syncdRoutes[gVirtualRouterId].erase(pic_pfx);
    EXPECT_EQ(g_route_set_calls, 2);
    EXPECT_EQ(g_route_set_oid, nh2_oid);
    EXPECT_EQ(gNeighOrch->m_syncdNextHops[nh1].ref_count, 0);
}

TEST_F(NhgOrchReplaceTest, OnlySingleNextHopTransitionsReplaceTheId)
{
    ASSERT_NO_FATAL_FAILURE(seed());
    const auto &nhg = gNhgOrch->m_syncdNextHopGroups.at(index).nhg;
    EXPECT_TRUE(nhg->replacesIdOnUpdate(NextHopGroupKey("10.0.0.1@Ethernet0,10.0.0.2@Ethernet4", string(""))));

    NextHopGroup multi(NextHopGroupKey("10.0.0.1@Ethernet0,10.0.0.2@Ethernet4", string("")), false);
    multi.m_id = 0x5501;
    EXPECT_FALSE(multi.replacesIdOnUpdate(NextHopGroupKey("10.0.0.1@Ethernet0,10.0.0.3@Ethernet8", string(""))));
    EXPECT_TRUE(multi.replacesIdOnUpdate(NextHopGroupKey("10.0.0.1@Ethernet0", string(""))));
    multi.m_id = SAI_NULL_OBJECT_ID;
    EXPECT_FALSE(multi.replacesIdOnUpdate(NextHopGroupKey("10.0.0.1@Ethernet0", string(""))));
}

/* Several next hops shrink to one: the routes move to the next hop first, and
 * only then is the old SAI group removed, so its removal cannot fail on them. */
TEST_F(NhgOrchReplaceTest, ShrinkingToOneNextHopMovesRoutesBeforeRemovingTheGroup)
{
    ASSERT_NO_FATAL_FAILURE(seed("10.0.0.1@Ethernet0,10.0.0.2@Ethernet4"));
    ASSERT_EQ(gNhgOrch->m_syncdNextHopGroups.at(index).nhg->getId(), group_oid);

    sai_object_id_t route_oid_at_remove = SAI_NULL_OBJECT_ID;
    EXPECT_CALL(*mock_sai_next_hop_group_api, remove_next_hop_group(group_oid))
        .WillOnce([&](sai_object_id_t) {
            route_oid_at_remove = g_route_set_oid;
            return SAI_STATUS_SUCCESS;
        });

    EXPECT_TRUE(gNhgOrch->replaceNhg(index, NextHopGroupKey("10.0.0.2@Ethernet4", string(""))));

    EXPECT_EQ(g_route_set_calls, 1);
    EXPECT_EQ(g_route_set_oid, nh2_oid);
    EXPECT_EQ(route_oid_at_remove, nh2_oid);
    EXPECT_EQ(gNhgOrch->m_syncdNextHopGroups.at(index).nhg->getId(), nh2_oid);
    EXPECT_EQ(gNeighOrch->m_syncdNextHops[nh1].ref_count, 0);
    EXPECT_EQ(gNeighOrch->m_syncdNextHops[nh2].ref_count, 1);
}

/* A new SAI group none of whose members could be created would blackhole the
 * routes, so they stay on the old next hop until a retry. */
TEST_F(NhgOrchReplaceTest, KeepsTheOldNextHopWhenNoMemberOfTheNewGroupIsCreated)
{
    ASSERT_NO_FATAL_FAILURE(seed());
    EXPECT_CALL(*mock_sai_next_hop_group_api, create_next_hop_group_members(_, _, _, _, _, _, _))
        .WillOnce([](sai_object_id_t, uint32_t count, const uint32_t *, const sai_attribute_t **,
                     sai_bulk_op_error_mode_t, sai_object_id_t *ids, sai_status_t *statuses) {
            for (uint32_t i = 0; i < count; i++)
            {
                ids[i] = SAI_NULL_OBJECT_ID;
                statuses[i] = SAI_STATUS_FAILURE;
            }
            return SAI_STATUS_FAILURE;
        });
    EXPECT_CALL(*mock_sai_next_hop_group_api, remove_next_hop_group(group_oid)).Times(1);

    EXPECT_FALSE(gNhgOrch->replaceNhg(index, NextHopGroupKey("10.0.0.1@Ethernet0,10.0.0.2@Ethernet4", string(""))));

    EXPECT_EQ(g_route_set_calls, 0);
    EXPECT_EQ(gNhgOrch->m_syncdNextHopGroups.at(index).nhg->getId(), nh1_oid);
    EXPECT_EQ(gNeighOrch->m_syncdNextHops[nh1].ref_count, 1);
    EXPECT_EQ(gNeighOrch->m_syncdNextHops[nh2].ref_count, 0);
}

/* With no member that can be added, no SAI group is created and the routes
 * stay on the old next hop until a retry. */
TEST_F(NhgOrchReplaceTest, KeepsTheOldNextHopWhenNoMemberOfTheNewGroupCanBeAdded)
{
    ASSERT_NO_FATAL_FAILURE(seed());
    gNeighOrch->m_syncdNextHops[nh2].nh_flags = NHFLAGS_IFDOWN;
    EXPECT_CALL(*mock_sai_next_hop_group_api, create_next_hop_group(_, _, _, _)).Times(0);

    /* nh2's interface is down and 10.0.0.3 is not resolved. */
    EXPECT_FALSE(gNhgOrch->replaceNhg(index, NextHopGroupKey("10.0.0.2@Ethernet4,10.0.0.3@Ethernet8", string(""))));

    EXPECT_EQ(g_route_set_calls, 0);
    EXPECT_EQ(gNhgOrch->m_syncdNextHopGroups.at(index).nhg->getId(), nh1_oid);
    EXPECT_EQ(gNeighOrch->m_syncdNextHops[nh1].ref_count, 1);
}

TEST_F(NhgOrchReplaceTest, MovesLabelRoutes)
{
    ASSERT_NO_FATAL_FAILURE(seed());
    gRouteOrch->m_syncdLabelRoutes[gVirtualRouterId][label].nhg_index = index;
    gNhgOrch->m_syncdNextHopGroups.at(index).ref_count = 2;

    EXPECT_TRUE(gNhgOrch->replaceNhg(index, NextHopGroupKey("10.0.0.2@Ethernet4", string(""))));

    EXPECT_EQ(g_route_set_calls, 1);
    EXPECT_EQ(g_route_set_oid, nh2_oid);
    EXPECT_EQ(g_label_set_calls, 1);
    EXPECT_EQ(g_label_set_oid, nh2_oid);
    EXPECT_EQ(gNeighOrch->m_syncdNextHops[nh1].ref_count, 0);
}

/* A label route that cannot move sends the routes that did move back. */
TEST_F(NhgOrchReplaceTest, KeepsTheOldGroupWhenALabelRouteCannotMove)
{
    ASSERT_NO_FATAL_FAILURE(seed());
    gRouteOrch->m_syncdLabelRoutes[gVirtualRouterId][label].nhg_index = index;
    gNhgOrch->m_syncdNextHopGroups.at(index).ref_count = 2;
    g_fail_label_oid = nh2_oid;

    EXPECT_FALSE(gNhgOrch->replaceNhg(index, NextHopGroupKey("10.0.0.2@Ethernet4", string(""))));

    /* Each moved, then moved back. */
    EXPECT_EQ(g_route_set_calls, 2);
    EXPECT_EQ(g_route_set_oid, nh1_oid);
    EXPECT_EQ(g_label_set_calls, 2);
    EXPECT_EQ(g_label_set_oid, nh1_oid);
    EXPECT_EQ(gNhgOrch->m_syncdNextHopGroups.at(index).nhg->getId(), nh1_oid);
    EXPECT_EQ(gNeighOrch->m_syncdNextHops[nh1].ref_count, 1);
    EXPECT_EQ(gNeighOrch->m_syncdNextHops[nh2].ref_count, 0);
}

} // namespace nhgorch_replace_test
