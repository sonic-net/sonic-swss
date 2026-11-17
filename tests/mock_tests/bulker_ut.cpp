#include "ut_helper.h"
#include "bulker.h"
#include "mock_sai_api.h"

extern sai_route_api_t *sai_route_api;
extern sai_neighbor_api_t *sai_neighbor_api;
extern sai_next_hop_api_t *sai_next_hop_api;

EXTERN_MOCK_FNS

namespace bulker_test
{
    using namespace std;
    using ::testing::SetArrayArgument;
    using ::testing::Return;
    using ::testing::DoAll;

    DEFINE_SAI_GENERIC_API_OBJECT_BULK_MOCK_WITH_SET(next_hop, next_hop);

    sai_bulk_object_create_fn old_object_create;
    sai_bulk_object_remove_fn old_object_remove;
    sai_bulk_object_set_attribute_fn old_object_set_attribute;

    struct BulkerTest : public ::testing::Test
    {
        BulkerTest()
        {
        }

        void SetUp() override
        {
            ASSERT_EQ(sai_route_api, nullptr);
            sai_route_api = new sai_route_api_t();

            ASSERT_EQ(sai_neighbor_api, nullptr);
            sai_neighbor_api = new sai_neighbor_api_t();

            ASSERT_EQ(sai_next_hop_api, nullptr);
            sai_next_hop_api = new sai_next_hop_api_t();

            INIT_SAI_API_MOCK(next_hop);
            MockSaiApis();
            old_object_create = sai_next_hop_api->create_next_hops;
            old_object_remove = sai_next_hop_api->remove_next_hops;
            old_object_set_attribute = sai_next_hop_api->set_next_hops_attribute;
            sai_next_hop_api->create_next_hops = mock_create_next_hops;
            sai_next_hop_api->remove_next_hops = mock_remove_next_hops;
            sai_next_hop_api->set_next_hops_attribute = mock_set_next_hops_attribute;
        }

        void TearDown() override
        {
            RestoreSaiApis();
            DEINIT_SAI_API_MOCK(next_hop);
            sai_next_hop_api->create_next_hops = old_object_create;
            sai_next_hop_api->remove_next_hops = old_object_remove;
            sai_next_hop_api->set_next_hops_attribute = old_object_set_attribute;

            delete sai_route_api;
            sai_route_api = nullptr;

            delete sai_neighbor_api;
            sai_neighbor_api = nullptr;

            delete sai_next_hop_api;
            sai_next_hop_api = nullptr;
        }
    };

    TEST_F(BulkerTest, BulkerAttrOrder)
    {
        // Create bulker
        EntityBulker<sai_route_api_t> gRouteBulker(sai_route_api, 1000);
        deque<sai_status_t> object_statuses;

        // Check max bulk size
        ASSERT_EQ(gRouteBulker.max_bulk_size, 1000);

        // Create a dummy route entry
        sai_route_entry_t route_entry;
        route_entry.destination.addr_family = SAI_IP_ADDR_FAMILY_IPV4;
        route_entry.destination.addr.ip4 = htonl(0x0a00000f);
        route_entry.destination.mask.ip4 = htonl(0xffffff00);
        route_entry.vr_id = 0x0;
        route_entry.switch_id = 0x0;

        // Set packet action for route first
        sai_attribute_t route_attr;
        route_attr.id = SAI_ROUTE_ENTRY_ATTR_PACKET_ACTION;
        route_attr.value.s32 = SAI_PACKET_ACTION_FORWARD;

        object_statuses.emplace_back();
        gRouteBulker.set_entry_attribute(&object_statuses.back(), &route_entry, &route_attr);

        // Set next hop for route
        route_attr.id = SAI_ROUTE_ENTRY_ATTR_NEXT_HOP_ID;
        route_attr.value.oid = SAI_NULL_OBJECT_ID;

        object_statuses.emplace_back();
        gRouteBulker.set_entry_attribute(&object_statuses.back(), &route_entry, &route_attr);

        // Check number of routes in bulk
        ASSERT_EQ(gRouteBulker.setting_entries_count(), 1);

        // Confirm the order of attributes in bulk is the same as being set
        auto const& attrs = gRouteBulker.setting_entries[route_entry];
        ASSERT_EQ(attrs.size(), 2);
        auto ia = attrs.begin();
        ASSERT_EQ(ia->first.id, SAI_ROUTE_ENTRY_ATTR_PACKET_ACTION);
        ASSERT_EQ(ia->first.value.s32, SAI_PACKET_ACTION_FORWARD);
        ia++;
        ASSERT_EQ(ia->first.id, SAI_ROUTE_ENTRY_ATTR_NEXT_HOP_ID);
        ASSERT_EQ(ia->first.value.oid, SAI_NULL_OBJECT_ID);

        // Clear the bulk
        gRouteBulker.clear();
        object_statuses.clear();

        // Check the bulker has been cleared
        ASSERT_EQ(gRouteBulker.setting_entries_count(), 0);

        // Test the inverse order
        // Set next hop for route first
        route_attr.id = SAI_ROUTE_ENTRY_ATTR_NEXT_HOP_ID;
        route_attr.value.oid = SAI_NULL_OBJECT_ID;

        object_statuses.emplace_back();
        gRouteBulker.set_entry_attribute(&object_statuses.back(), &route_entry, &route_attr);

        // Set packet action for route
        route_attr.id = SAI_ROUTE_ENTRY_ATTR_PACKET_ACTION;
        route_attr.value.s32 = SAI_PACKET_ACTION_FORWARD;

        object_statuses.emplace_back();
        gRouteBulker.set_entry_attribute(&object_statuses.back(), &route_entry, &route_attr);

        // Check number of routes in bulk
        ASSERT_EQ(gRouteBulker.setting_entries_count(), 1);

        // Confirm the order of attributes in bulk is the same as being set
        auto const& attrs_reverse = gRouteBulker.setting_entries[route_entry];
        ASSERT_EQ(attrs_reverse.size(), 2);
        ia = attrs_reverse.begin();
        ASSERT_EQ(ia->first.id, SAI_ROUTE_ENTRY_ATTR_NEXT_HOP_ID);
        ASSERT_EQ(ia->first.value.oid, SAI_NULL_OBJECT_ID);
        ia++;
        ASSERT_EQ(ia->first.id, SAI_ROUTE_ENTRY_ATTR_PACKET_ACTION);
        ASSERT_EQ(ia->first.value.s32, SAI_PACKET_ACTION_FORWARD);
    }

    static uint32_t stub_bulk_create_calls;
    static uint32_t stub_bulk_remove_calls;

    static sai_status_t stub_bulk_create_route_entry(
        uint32_t object_count, const sai_route_entry_t * /*route_entry*/,
        const uint32_t * /*attr_count*/, const sai_attribute_t ** /*attr_list*/,
        sai_bulk_op_error_mode_t /*mode*/, sai_status_t *object_statuses)
    {
        stub_bulk_create_calls++;
        for (uint32_t i = 0; i < object_count; i++)
        {
            object_statuses[i] = SAI_STATUS_SUCCESS;
        }
        return SAI_STATUS_SUCCESS;
    }

    static sai_status_t stub_bulk_remove_route_entry(
        uint32_t object_count, const sai_route_entry_t * /*route_entry*/,
        sai_bulk_op_error_mode_t /*mode*/, sai_status_t *object_statuses)
    {
        stub_bulk_remove_calls++;
        for (uint32_t i = 0; i < object_count; i++)
        {
            object_statuses[i] = SAI_STATUS_SUCCESS;
        }
        return SAI_STATUS_SUCCESS;
    }

    static uint32_t stub_bulk_set_calls;

    static sai_status_t stub_bulk_set_route_entry(
        uint32_t object_count, const sai_route_entry_t * /*route_entry*/,
        const sai_attribute_t * /*attr_list*/,
        sai_bulk_op_error_mode_t /*mode*/, sai_status_t *object_statuses)
    {
        stub_bulk_set_calls++;
        for (uint32_t i = 0; i < object_count; i++)
        {
            object_statuses[i] = SAI_STATUS_SUCCESS;
        }
        return SAI_STATUS_SUCCESS;
    }

    /* flush_until ships pending work in max_bulk_size chunks, stops at the
     * first chunk boundary where expired() is true -- always making at least
     * one chunk of progress -- resumes from where it stopped, and on
     * completion leaves the bulker cleared exactly like flush(). */
    TEST_F(BulkerTest, BulkerFlushUntilYieldsAndResumes)
    {
        sai_route_api->create_route_entries = stub_bulk_create_route_entry;
        sai_route_api->remove_route_entries = stub_bulk_remove_route_entry;

        EntityBulker<sai_route_api_t> bulker(sai_route_api, 2);

        deque<sai_status_t> object_statuses;
        sai_attribute_t route_attr;
        route_attr.id = SAI_ROUTE_ENTRY_ATTR_PACKET_ACTION;
        route_attr.value.s32 = SAI_PACKET_ACTION_FORWARD;

        auto make_entry = [](uint32_t host) {
            sai_route_entry_t e;
            e.destination.addr_family = SAI_IP_ADDR_FAMILY_IPV4;
            e.destination.addr.ip4 = htonl(0x0a000000 + host);
            e.destination.mask.ip4 = htonl(0xffffffff);
            e.vr_id = 0x0;
            e.switch_id = 0x0;
            return e;
        };

        /* 3 removes + 5 creates with chunk size 2: remove chunks {2,1},
         * create chunks {2,2,1}. */
        for (uint32_t i = 0; i < 3; i++)
        {
            auto e = make_entry(100 + i);
            object_statuses.emplace_back();
            bulker.remove_entry(&object_statuses.back(), &e);
        }
        for (uint32_t i = 0; i < 5; i++)
        {
            auto e = make_entry(200 + i);
            object_statuses.emplace_back();
            bulker.create_entry(&object_statuses.back(), &e, 1, &route_attr);
        }

        stub_bulk_create_calls = 0;
        stub_bulk_remove_calls = 0;
        auto always_expired = []() { return true; };

        /* Call 1: one remove chunk {2}, then yield with work remaining. */
        ASSERT_FALSE(bulker.flush_until(always_expired));
        ASSERT_EQ(stub_bulk_remove_calls, 1u);
        ASSERT_EQ(stub_bulk_create_calls, 0u);
        ASSERT_EQ(object_statuses[0], SAI_STATUS_SUCCESS);
        ASSERT_EQ(object_statuses[1], SAI_STATUS_SUCCESS);
        ASSERT_EQ(object_statuses[2], SAI_STATUS_NOT_EXECUTED);
        ASSERT_EQ(bulker.removing_entries_count(), 3);

        /* Call 2: final remove chunk {1}; removing section completes and
         * clears, creates still pending. */
        ASSERT_FALSE(bulker.flush_until(always_expired));
        ASSERT_EQ(stub_bulk_remove_calls, 2u);
        ASSERT_EQ(stub_bulk_create_calls, 0u);
        ASSERT_EQ(object_statuses[2], SAI_STATUS_SUCCESS);
        ASSERT_EQ(bulker.removing_entries_count(), 0);
        ASSERT_EQ(bulker.creating_entries_count(), 5);

        /* Calls 3-4: one create chunk {2} each. */
        ASSERT_FALSE(bulker.flush_until(always_expired));
        ASSERT_EQ(stub_bulk_create_calls, 1u);
        ASSERT_EQ(object_statuses[4], SAI_STATUS_SUCCESS);
        ASSERT_EQ(object_statuses[5], SAI_STATUS_NOT_EXECUTED);
        ASSERT_FALSE(bulker.flush_until(always_expired));
        ASSERT_EQ(stub_bulk_create_calls, 2u);

        /* Call 5: final create chunk {1}; everything flushed and cleared. */
        ASSERT_TRUE(bulker.flush_until(always_expired));
        ASSERT_EQ(stub_bulk_create_calls, 3u);
        ASSERT_EQ(bulker.creating_entries_count(), 0);
        for (const auto& status : object_statuses)
        {
            ASSERT_EQ(status, SAI_STATUS_SUCCESS);
        }

        /* A non-expiring run ships everything in one call. */
        object_statuses.clear();
        for (uint32_t i = 0; i < 5; i++)
        {
            auto e = make_entry(300 + i);
            object_statuses.emplace_back();
            bulker.create_entry(&object_statuses.back(), &e, 1, &route_attr);
        }
        stub_bulk_create_calls = 0;
        ASSERT_TRUE(bulker.flush_until([]() { return false; }));
        ASSERT_EQ(stub_bulk_create_calls, 3u);  /* chunks {2,2,1} */
        ASSERT_EQ(bulker.creating_entries_count(), 0);
    }

    TEST_F(BulkerTest, BulkerPendindRemoval)
    {
        // Create bulker
        EntityBulker<sai_route_api_t> gRouteBulker(sai_route_api, 1000);
        deque<sai_status_t> object_statuses;

        // Check max bulk size
        ASSERT_EQ(gRouteBulker.max_bulk_size, 1000);

        // Create a dummy route entry
        sai_route_entry_t route_entry_remove;
        route_entry_remove.destination.addr_family = SAI_IP_ADDR_FAMILY_IPV4;
        route_entry_remove.destination.addr.ip4 = htonl(0x0a00000f);
        route_entry_remove.destination.mask.ip4 = htonl(0xffffff00);
        route_entry_remove.vr_id = 0x0;
        route_entry_remove.switch_id = 0x0;

        // Put route entry into remove
        object_statuses.emplace_back();
        gRouteBulker.remove_entry(&object_statuses.back(), &route_entry_remove);

        // Confirm route entry is pending removal
        ASSERT_TRUE(gRouteBulker.bulk_entry_pending_removal(route_entry_remove));

        // Create another dummy route entry that will not be removed
        sai_route_entry_t route_entry_non_remove;
        route_entry_non_remove.destination.addr_family = SAI_IP_ADDR_FAMILY_IPV4;
        route_entry_non_remove.destination.addr.ip4 = htonl(0x0a00010f);
        route_entry_non_remove.destination.mask.ip4 = htonl(0xffffff00);
        route_entry_non_remove.vr_id = 0x0;
        route_entry_non_remove.switch_id = 0x0;

        // Confirm route entry is not pending removal
        ASSERT_FALSE(gRouteBulker.bulk_entry_pending_removal(route_entry_non_remove));
    }

    TEST_F(BulkerTest, NeighborBulker)
    {
        // Create bulker
        EntityBulker<sai_neighbor_api_t> gNeighBulker(sai_neighbor_api, 1000);
        deque<sai_status_t> object_statuses;

        // Check max bulk size
        ASSERT_EQ(gNeighBulker.max_bulk_size, 1000);

        // Create a dummy neighbor entry
        sai_neighbor_entry_t neighbor_entry_remove;
        neighbor_entry_remove.ip_address.addr_family = SAI_IP_ADDR_FAMILY_IPV4;
        neighbor_entry_remove.ip_address.addr.ip4 = 0x10000001;
        neighbor_entry_remove.rif_id = 0x0;
        neighbor_entry_remove.switch_id = 0x0;

        // Put neighbor entry into remove
        object_statuses.emplace_back();
        gNeighBulker.remove_entry(&object_statuses.back(), &neighbor_entry_remove);

        // Confirm neighbor entry is pending removal
        ASSERT_TRUE(gNeighBulker.bulk_entry_pending_removal(neighbor_entry_remove));
    }

    TEST_F(BulkerTest, ObjectBulkSet)
    {
        // Create bulker
        ObjectBulker<sai_next_hop_api_t> gNextHopBulker(sai_next_hop_api, 0x0, 1000);
        vector<sai_attribute_t> next_hop_attrs;
        vector<sai_object_id_t> next_hop_ids = {0x101, 0x102};
        vector<sai_status_t> statuses;
        sai_attribute_t next_hop_attr;
        sai_object_id_t next_hop_id_0;
        sai_object_id_t next_hop_id_1;
        std::vector<sai_status_t> exp_status{SAI_STATUS_SUCCESS, SAI_STATUS_SUCCESS};

        // Create 2 next hops
        next_hop_attr.id = SAI_NEXT_HOP_ATTR_TYPE;
        next_hop_attr.value.s32 = SAI_NEXT_HOP_TYPE_IP;
        next_hop_attrs.push_back(next_hop_attr);

        next_hop_attr.id = SAI_NEXT_HOP_ATTR_IP;
        next_hop_attr.value.ipaddr.addr_family = SAI_IP_ADDR_FAMILY_IPV4;
        next_hop_attr.value.ipaddr.addr.ip4 = 0x10000001;
        next_hop_attrs.push_back(next_hop_attr);

        next_hop_attr.id = SAI_NEXT_HOP_ATTR_ROUTER_INTERFACE_ID;
        next_hop_attr.value.oid = 0x0;
        next_hop_attrs.push_back(next_hop_attr);

        gNextHopBulker.create_entry(&next_hop_id_0, (uint32_t)next_hop_attrs.size(), next_hop_attrs.data());
        next_hop_attrs.clear();

        next_hop_attr.id = SAI_NEXT_HOP_ATTR_TYPE;
        next_hop_attr.value.s32 = SAI_NEXT_HOP_TYPE_IP;
        next_hop_attrs.push_back(next_hop_attr);

        next_hop_attr.id = SAI_NEXT_HOP_ATTR_IP;
        next_hop_attr.value.ipaddr.addr_family = SAI_IP_ADDR_FAMILY_IPV4;
        next_hop_attr.value.ipaddr.addr.ip4 = 0x10000002;
        next_hop_attrs.push_back(next_hop_attr);

        next_hop_attr.id = SAI_NEXT_HOP_ATTR_ROUTER_INTERFACE_ID;
        next_hop_attr.value.oid = 0x0;
        next_hop_attrs.push_back(next_hop_attr);

        gNextHopBulker.create_entry(&next_hop_id_1, (uint32_t)next_hop_attrs.size(), next_hop_attrs.data());
        next_hop_attrs.clear();

        EXPECT_CALL(*mock_sai_next_hop_api, create_next_hops)
            .WillOnce(DoAll(
                SetArrayArgument<5>(next_hop_ids.begin(), next_hop_ids.end()),
                SetArrayArgument<6>(exp_status.begin(), exp_status.end()),
                Return(SAI_STATUS_SUCCESS)));
        gNextHopBulker.flush();

        // Update the nexthops
        next_hop_attr.id = SAI_NEXT_HOP_ATTR_IP;
        next_hop_attr.value.ipaddr.addr_family = SAI_IP_ADDR_FAMILY_IPV4;
        next_hop_attr.value.ipaddr.addr.ip4 = 0x10000003;

        gNextHopBulker.set_entry_attribute(next_hop_id_0, &next_hop_attr);

        next_hop_attr.id = SAI_NEXT_HOP_ATTR_IP;
        next_hop_attr.value.ipaddr.addr_family = SAI_IP_ADDR_FAMILY_IPV4;
        next_hop_attr.value.ipaddr.addr.ip4 = 0x10000004;
        next_hop_attrs.push_back(next_hop_attr);

        gNextHopBulker.set_entry_attribute(next_hop_id_1, &next_hop_attr);

        EXPECT_CALL(*mock_sai_next_hop_api, set_next_hops_attribute)
            .WillOnce(DoAll(SetArrayArgument<4>(exp_status.begin(), exp_status.end()), Return(SAI_STATUS_SUCCESS)));
        gNextHopBulker.flush();

        // Delete the nexthops
        statuses.emplace_back();
        gNextHopBulker.remove_entry(&statuses.back(), next_hop_id_0);
        statuses.emplace_back();
        gNextHopBulker.remove_entry(&statuses.back(), next_hop_id_1);

        EXPECT_CALL(*mock_sai_next_hop_api, remove_next_hops)
            .WillOnce(DoAll(SetArrayArgument<3>(exp_status.begin(), exp_status.end()), Return(SAI_STATUS_SUCCESS)));
        gNextHopBulker.flush();
    }

    TEST_F(BulkerTest, BulkerPendingRemovalOrSet_OnlyRemoval)
    {
        // Create bulker
        EntityBulker<sai_route_api_t> gRouteBulker(sai_route_api, 1000);
        deque<sai_status_t> object_statuses;

        // Create a dummy route entry for removal
        sai_route_entry_t route_entry_remove;
        route_entry_remove.destination.addr_family = SAI_IP_ADDR_FAMILY_IPV4;
        route_entry_remove.destination.addr.ip4 = htonl(0x0a00000f);
        route_entry_remove.destination.mask.ip4 = htonl(0xffffff00);
        route_entry_remove.vr_id = 0x0;
        route_entry_remove.switch_id = 0x0;

        // Put route entry into remove
        object_statuses.emplace_back();
        gRouteBulker.remove_entry(&object_statuses.back(), &route_entry_remove);

        // Confirm route entry is pending removal
        ASSERT_TRUE(gRouteBulker.bulk_entry_pending_removal(route_entry_remove));

        // Confirm route entry is detected by bulk_entry_pending_removal_or_set
        ASSERT_TRUE(gRouteBulker.bulk_entry_pending_removal_or_set(route_entry_remove));
    }

    TEST_F(BulkerTest, BulkerPendingRemovalOrSet_OnlySet)
    {
        // Create bulker
        EntityBulker<sai_route_api_t> gRouteBulker(sai_route_api, 1000);
        deque<sai_status_t> object_statuses;

        // Create a dummy route entry for setting
        sai_route_entry_t route_entry_set;
        route_entry_set.destination.addr_family = SAI_IP_ADDR_FAMILY_IPV4;
        route_entry_set.destination.addr.ip4 = htonl(0x0a00000f);
        route_entry_set.destination.mask.ip4 = htonl(0xffffff00);
        route_entry_set.vr_id = 0x0;
        route_entry_set.switch_id = 0x0;

        // Set packet action for route (this adds to setting_entries)
        sai_attribute_t route_attr;
        route_attr.id = SAI_ROUTE_ENTRY_ATTR_PACKET_ACTION;
        route_attr.value.s32 = SAI_PACKET_ACTION_DROP;

        object_statuses.emplace_back();
        gRouteBulker.set_entry_attribute(&object_statuses.back(), &route_entry_set, &route_attr);

        // Confirm route entry is NOT pending removal
        ASSERT_FALSE(gRouteBulker.bulk_entry_pending_removal(route_entry_set));

        // Confirm route entry IS detected by bulk_entry_pending_removal_or_set (because it's in setting_entries)
        ASSERT_TRUE(gRouteBulker.bulk_entry_pending_removal_or_set(route_entry_set));
    }

    TEST_F(BulkerTest, BulkerPendingRemovalOrSet_BothRemovalAndSet)
    {
        // Create bulker
        EntityBulker<sai_route_api_t> gRouteBulker(sai_route_api, 1000);
        deque<sai_status_t> object_statuses;

        // Create two different route entries
        sai_route_entry_t route_entry_remove;
        route_entry_remove.destination.addr_family = SAI_IP_ADDR_FAMILY_IPV4;
        route_entry_remove.destination.addr.ip4 = htonl(0x0a00000f);
        route_entry_remove.destination.mask.ip4 = htonl(0xffffff00);
        route_entry_remove.vr_id = 0x0;
        route_entry_remove.switch_id = 0x0;

        sai_route_entry_t route_entry_set;
        route_entry_set.destination.addr_family = SAI_IP_ADDR_FAMILY_IPV4;
        route_entry_set.destination.addr.ip4 = htonl(0x0a00010f);
        route_entry_set.destination.mask.ip4 = htonl(0xffffff00);
        route_entry_set.vr_id = 0x0;
        route_entry_set.switch_id = 0x0;

        // Put first route entry into remove
        object_statuses.emplace_back();
        gRouteBulker.remove_entry(&object_statuses.back(), &route_entry_remove);

        // Set attribute for second route entry
        sai_attribute_t route_attr;
        route_attr.id = SAI_ROUTE_ENTRY_ATTR_PACKET_ACTION;
        route_attr.value.s32 = SAI_PACKET_ACTION_DROP;

        object_statuses.emplace_back();
        gRouteBulker.set_entry_attribute(&object_statuses.back(), &route_entry_set, &route_attr);

        // Confirm both entries are detected by bulk_entry_pending_removal_or_set
        ASSERT_TRUE(gRouteBulker.bulk_entry_pending_removal_or_set(route_entry_remove));
        ASSERT_TRUE(gRouteBulker.bulk_entry_pending_removal_or_set(route_entry_set));

        // Confirm only the removal entry is detected by bulk_entry_pending_removal
        ASSERT_TRUE(gRouteBulker.bulk_entry_pending_removal(route_entry_remove));
        ASSERT_FALSE(gRouteBulker.bulk_entry_pending_removal(route_entry_set));
    }

    TEST_F(BulkerTest, BulkerPendingRemovalOrSet_NeitherRemovalNorSet)
    {
        // Create bulker
        EntityBulker<sai_route_api_t> gRouteBulker(sai_route_api, 1000);
        deque<sai_status_t> object_statuses;

        // Create a dummy route entry that is not added to bulker
        sai_route_entry_t route_entry_none;
        route_entry_none.destination.addr_family = SAI_IP_ADDR_FAMILY_IPV4;
        route_entry_none.destination.addr.ip4 = htonl(0x0a00000f);
        route_entry_none.destination.mask.ip4 = htonl(0xffffff00);
        route_entry_none.vr_id = 0x0;
        route_entry_none.switch_id = 0x0;

        // Confirm route entry is NOT pending removal
        ASSERT_FALSE(gRouteBulker.bulk_entry_pending_removal(route_entry_none));

        // Confirm route entry is NOT detected by bulk_entry_pending_removal_or_set
        ASSERT_FALSE(gRouteBulker.bulk_entry_pending_removal_or_set(route_entry_none));
    }

    TEST_F(BulkerTest, BulkerPendingRemovalOrSet_DefaultRouteScenario)
    {
        // This test simulates the default route scenario described in the code comments:
        // A DEL event occurs and automatically adds a DROP action (creating a setting_entry),
        // then a subsequent SET operation needs to check for both pending removals AND pending sets.

        EntityBulker<sai_route_api_t> gRouteBulker(sai_route_api, 1000);
        deque<sai_status_t> object_statuses;

        // Create a default route entry (0.0.0.0/0)
        sai_route_entry_t default_route;
        default_route.destination.addr_family = SAI_IP_ADDR_FAMILY_IPV4;
        default_route.destination.addr.ip4 = 0;  // 0.0.0.0
        default_route.destination.mask.ip4 = 0;  // /0
        default_route.vr_id = 0x0;
        default_route.switch_id = 0x0;

        // Simulate DEL event: Set DROP action for default route
        sai_attribute_t route_attr;
        route_attr.id = SAI_ROUTE_ENTRY_ATTR_PACKET_ACTION;
        route_attr.value.s32 = SAI_PACKET_ACTION_DROP;

        object_statuses.emplace_back();
        gRouteBulker.set_entry_attribute(&object_statuses.back(), &default_route, &route_attr);

        // Verify the route is in setting_entries
        ASSERT_EQ(gRouteBulker.setting_entries_count(), 1);

        // Verify bulk_entry_pending_removal returns false (not in removing_entries)
        ASSERT_FALSE(gRouteBulker.bulk_entry_pending_removal(default_route));

        // Verify bulk_entry_pending_removal_or_set returns true (in setting_entries)
        ASSERT_TRUE(gRouteBulker.bulk_entry_pending_removal_or_set(default_route));

        // This ensures that when a subsequent SET operation checks if the route needs to be updated,
        // it will correctly detect that there's a pending operation (the DROP action)
    }

    TEST_F(BulkerTest, BulkerPendingRemovalOrSet_IPv6Route)
    {
        // Test with IPv6 route to ensure the function works with different address families
        EntityBulker<sai_route_api_t> gRouteBulker(sai_route_api, 1000);
        deque<sai_status_t> object_statuses;

        // Create an IPv6 route entry
        sai_route_entry_t ipv6_route;
        ipv6_route.destination.addr_family = SAI_IP_ADDR_FAMILY_IPV6;
        // Set IPv6 address 2001:db8::1/64
        uint8_t ipv6_addr[16] = {0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
        uint8_t ipv6_mask[16] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0, 0, 0, 0, 0, 0, 0, 0};
        memcpy(ipv6_route.destination.addr.ip6, ipv6_addr, 16);
        memcpy(ipv6_route.destination.mask.ip6, ipv6_mask, 16);
        ipv6_route.vr_id = 0x0;
        ipv6_route.switch_id = 0x0;

        // Set packet action for IPv6 route
        sai_attribute_t route_attr;
        route_attr.id = SAI_ROUTE_ENTRY_ATTR_PACKET_ACTION;
        route_attr.value.s32 = SAI_PACKET_ACTION_FORWARD;

        object_statuses.emplace_back();
        gRouteBulker.set_entry_attribute(&object_statuses.back(), &ipv6_route, &route_attr);

        // Verify the IPv6 route is detected by bulk_entry_pending_removal_or_set
        ASSERT_TRUE(gRouteBulker.bulk_entry_pending_removal_or_set(ipv6_route));
        ASSERT_FALSE(gRouteBulker.bulk_entry_pending_removal(ipv6_route));
    }

    TEST_F(BulkerTest, BulkerPendingRemovalOrSet_AfterClear)
    {
        // Test that bulk_entry_pending_removal_or_set returns false after clearing the bulker
        EntityBulker<sai_route_api_t> gRouteBulker(sai_route_api, 1000);
        deque<sai_status_t> object_statuses;

        // Create a route entry
        sai_route_entry_t route_entry;
        route_entry.destination.addr_family = SAI_IP_ADDR_FAMILY_IPV4;
        route_entry.destination.addr.ip4 = htonl(0x0a00000f);
        route_entry.destination.mask.ip4 = htonl(0xffffff00);
        route_entry.vr_id = 0x0;
        route_entry.switch_id = 0x0;

        // Add to setting_entries
        sai_attribute_t route_attr;
        route_attr.id = SAI_ROUTE_ENTRY_ATTR_PACKET_ACTION;
        route_attr.value.s32 = SAI_PACKET_ACTION_DROP;

        object_statuses.emplace_back();
        gRouteBulker.set_entry_attribute(&object_statuses.back(), &route_entry, &route_attr);

        // Verify it's detected before clear
        ASSERT_TRUE(gRouteBulker.bulk_entry_pending_removal_or_set(route_entry));

        // Clear the bulker
        gRouteBulker.clear();

        // Verify it's NOT detected after clear
        ASSERT_FALSE(gRouteBulker.bulk_entry_pending_removal_or_set(route_entry));
        ASSERT_FALSE(gRouteBulker.bulk_entry_pending_removal(route_entry));
    }

    /* A chunk that exactly finishes the backlog must complete flush_until in
     * the same call: false means "a concrete unexecuted unit remains", never
     * "the deadline happened to expire at the final chunk boundary". */
    TEST_F(BulkerTest, BulkerFlushUntilExactChunkCompletes)
    {
        sai_route_api->create_route_entries = stub_bulk_create_route_entry;
        sai_route_api->remove_route_entries = stub_bulk_remove_route_entry;
        sai_route_api->set_route_entries_attribute = stub_bulk_set_route_entry;

        EntityBulker<sai_route_api_t> bulker(sai_route_api, 2);

        deque<sai_status_t> object_statuses;
        sai_attribute_t route_attr;
        route_attr.id = SAI_ROUTE_ENTRY_ATTR_PACKET_ACTION;
        route_attr.value.s32 = SAI_PACKET_ACTION_FORWARD;

        auto make_entry = [](uint32_t host) {
            sai_route_entry_t e;
            e.destination.addr_family = SAI_IP_ADDR_FAMILY_IPV4;
            e.destination.addr.ip4 = htonl(0x0b000000 + host);
            e.destination.mask.ip4 = htonl(0xffffffff);
            e.vr_id = 0x0;
            e.switch_id = 0x0;
            return e;
        };
        auto always_expired = []() { return true; };

        /* Exactly one full chunk of SETs. */
        for (uint32_t i = 0; i < 2; i++)
        {
            auto e = make_entry(100 + i);
            object_statuses.emplace_back();
            bulker.set_entry_attribute(&object_statuses.back(), &e, &route_attr);
        }
        stub_bulk_set_calls = 0;
        ASSERT_TRUE(bulker.flush_until(always_expired));
        ASSERT_EQ(stub_bulk_set_calls, 1u);
        for (const auto& status : object_statuses)
        {
            ASSERT_EQ(status, SAI_STATUS_SUCCESS);
        }

        /* Exactly one full chunk in EVERY section: two yields (one before
         * each later section's first unit), then completion -- three calls
         * total, one bulk call per section, never a fourth no-op call. */
        object_statuses.clear();
        for (uint32_t i = 0; i < 2; i++)
        {
            auto e = make_entry(200 + i);
            object_statuses.emplace_back();
            bulker.remove_entry(&object_statuses.back(), &e);
        }
        for (uint32_t i = 0; i < 2; i++)
        {
            auto e = make_entry(300 + i);
            object_statuses.emplace_back();
            bulker.create_entry(&object_statuses.back(), &e, 1, &route_attr);
        }
        for (uint32_t i = 0; i < 2; i++)
        {
            auto e = make_entry(400 + i);
            object_statuses.emplace_back();
            bulker.set_entry_attribute(&object_statuses.back(), &e, &route_attr);
        }
        stub_bulk_remove_calls = 0;
        stub_bulk_create_calls = 0;
        stub_bulk_set_calls = 0;

        ASSERT_FALSE(bulker.flush_until(always_expired));   /* removes shipped */
        ASSERT_EQ(stub_bulk_remove_calls, 1u);
        ASSERT_EQ(stub_bulk_create_calls, 0u);
        ASSERT_FALSE(bulker.flush_until(always_expired));   /* creates shipped */
        ASSERT_EQ(stub_bulk_create_calls, 1u);
        ASSERT_EQ(stub_bulk_set_calls, 0u);
        ASSERT_TRUE(bulker.flush_until(always_expired));    /* sets shipped, done */
        ASSERT_EQ(stub_bulk_set_calls, 1u);
        for (const auto& status : object_statuses)
        {
            ASSERT_EQ(status, SAI_STATUS_SUCCESS);
        }
    }

    /* Setting-phase yield/resume through flushing_set_pos: a yielded
     * update-heavy batch resumes from the cursor instead of rescanning
     * set_order from the start (set_order holds one slot per attribute
     * call). Also pins the duplicate-occurrence handling: an entry with two
     * attributes occupies two set_order slots, its attributes are all
     * staged at the first occurrence, and the duplicate neither
     * double-stages nor costs an extra invocation. */
    TEST_F(BulkerTest, BulkerFlushUntilSettingResumesFromCursor)
    {
        sai_route_api->set_route_entries_attribute = stub_bulk_set_route_entry;

        EntityBulker<sai_route_api_t> bulker(sai_route_api, 2);

        deque<sai_status_t> object_statuses;
        sai_attribute_t route_attr;
        route_attr.id = SAI_ROUTE_ENTRY_ATTR_PACKET_ACTION;
        route_attr.value.s32 = SAI_PACKET_ACTION_FORWARD;

        auto make_entry = [](uint32_t host) {
            sai_route_entry_t e;
            e.destination.addr_family = SAI_IP_ADDR_FAMILY_IPV4;
            e.destination.addr.ip4 = htonl(0x0c000000 + host);
            e.destination.mask.ip4 = htonl(0xffffffff);
            e.vr_id = 0x0;
            e.switch_id = 0x0;
            return e;
        };
        auto always_expired = []() { return true; };

        /* Five entries, one attribute each: chunks {2,2,1}. */
        for (uint32_t i = 0; i < 5; i++)
        {
            auto e = make_entry(100 + i);
            object_statuses.emplace_back();
            bulker.set_entry_attribute(&object_statuses.back(), &e, &route_attr);
        }
        stub_bulk_set_calls = 0;

        ASSERT_FALSE(bulker.flush_until(always_expired));
        ASSERT_EQ(stub_bulk_set_calls, 1u);
        ASSERT_EQ(bulker.flushing_set_pos, 2u);   /* resume at the 3rd slot */
        ASSERT_EQ(object_statuses[1], SAI_STATUS_SUCCESS);
        ASSERT_EQ(object_statuses[2], SAI_STATUS_NOT_EXECUTED);

        ASSERT_FALSE(bulker.flush_until(always_expired));
        ASSERT_EQ(stub_bulk_set_calls, 2u);
        ASSERT_EQ(bulker.flushing_set_pos, 4u);

        ASSERT_TRUE(bulker.flush_until(always_expired));
        ASSERT_EQ(stub_bulk_set_calls, 3u);
        ASSERT_EQ(bulker.flushing_set_pos, 0u);   /* section completed, reset */
        for (const auto& status : object_statuses)
        {
            ASSERT_EQ(status, SAI_STATUS_SUCCESS);
        }

        /* One entry, two attributes = two set_order slots, exactly one
         * chunk: the duplicate occurrence must not double-stage, and the
         * whole thing completes in a single call. */
        object_statuses.clear();
        auto dup = make_entry(200);
        sai_attribute_t second_attr;
        second_attr.id = SAI_ROUTE_ENTRY_ATTR_NEXT_HOP_ID;
        second_attr.value.oid = SAI_NULL_OBJECT_ID;
        object_statuses.emplace_back();
        bulker.set_entry_attribute(&object_statuses.back(), &dup, &route_attr);
        object_statuses.emplace_back();
        bulker.set_entry_attribute(&object_statuses.back(), &dup, &second_attr);
        stub_bulk_set_calls = 0;

        ASSERT_TRUE(bulker.flush_until(always_expired));
        ASSERT_EQ(stub_bulk_set_calls, 1u);
        for (const auto& status : object_statuses)
        {
            ASSERT_EQ(status, SAI_STATUS_SUCCESS);
        }
    }

    /* SAI-like route table for the duplicate test: every entry a bulk create
     * names is counted, and an entry named twice in one call fails the second
     * time with ITEM_ALREADY_EXISTS. */
    static std::vector<uint32_t> dup_stub_create_counts;

    static uint32_t dup_stub_host(const sai_route_entry_t &e)
    {
        return ntohl(e.destination.addr.ip4) & 0xff;
    }

    static sai_status_t dup_stub_bulk_create_route_entry(
        uint32_t object_count, const sai_route_entry_t *route_entry,
        const uint32_t * /*attr_count*/, const sai_attribute_t ** /*attr_list*/,
        sai_bulk_op_error_mode_t /*mode*/, sai_status_t *object_statuses)
    {
        std::unordered_set<uint32_t> seen;
        for (uint32_t i = 0; i < object_count; i++)
        {
            dup_stub_create_counts.push_back(dup_stub_host(route_entry[i]));
            object_statuses[i] = seen.insert(dup_stub_host(route_entry[i])).second ?
                                 SAI_STATUS_SUCCESS : SAI_STATUS_ITEM_ALREADY_EXISTS;
        }
        return SAI_STATUS_SUCCESS;
    }

    static sai_route_entry_t dup_host_entry(uint32_t host)
    {
        sai_route_entry_t e;
        e.destination.addr_family = SAI_IP_ADDR_FAMILY_IPV4;
        e.destination.addr.ip4 = htonl(0x64321900 + host);
        e.destination.mask.ip4 = htonl(0xffffffff);
        e.vr_id = 0x0;
        e.switch_id = 0x0;
        return e;
    }

    /* create X, remove X (claimed done at once), create X again: one SAI create */
    static void dup_stage_create_remove_create(EntityBulker<sai_route_api_t> &bulker, deque<sai_status_t> &statuses,
                                               const sai_attribute_t *attr)
    {
        auto e = dup_host_entry(12);
        auto other = dup_host_entry(13);
        statuses.emplace_back();
        bulker.create_entry(&statuses.back(), &e, 1, attr);
        statuses.emplace_back();
        bulker.create_entry(&statuses.back(), &other, 1, attr);
        statuses.emplace_back();
        bulker.remove_entry(&statuses.back(), &e);
        statuses.emplace_back();
        bulker.create_entry(&statuses.back(), &e, 1, attr);
    }

    TEST_F(BulkerTest, BulkerFlushUntilCreateRemoveCreateCreatesOnce)
    {
        sai_route_api->create_route_entries = dup_stub_bulk_create_route_entry;
        sai_attribute_t attr;
        attr.id = SAI_ROUTE_ENTRY_ATTR_PACKET_ACTION;
        attr.value.s32 = SAI_PACKET_ACTION_FORWARD;

        for (size_t chunk : {1, 1000})
        {
            EntityBulker<sai_route_api_t> b(sai_route_api, chunk);
            deque<sai_status_t> st;
            dup_stage_create_remove_create(b, st, &attr);
            dup_stub_create_counts.clear();
            int calls = 0;
            while (!b.flush_until([]() { return true; }))
            {
                ASSERT_LT(++calls, 10);
            }
            ASSERT_EQ(dup_stub_create_counts, (std::vector<uint32_t>{12, 13})) << "chunk " << chunk;
            EXPECT_EQ(st[1], SAI_STATUS_SUCCESS) << "chunk " << chunk;
            EXPECT_EQ(st[3], SAI_STATUS_SUCCESS) << "chunk " << chunk;
        }

        /* Both create_order slots inside one chunk of a yielding flush */
        EntityBulker<sai_route_api_t> two(sai_route_api, 2);
        deque<sai_status_t> st;
        auto e = dup_host_entry(12);
        auto other = dup_host_entry(13);
        st.emplace_back();
        two.create_entry(&st.back(), &e, 1, &attr);
        st.emplace_back();
        two.remove_entry(&st.back(), &e);
        st.emplace_back();
        two.create_entry(&st.back(), &e, 1, &attr);
        st.emplace_back();
        two.create_entry(&st.back(), &other, 1, &attr);
        dup_stub_create_counts.clear();
        int calls = 0;
        while (!two.flush_until([]() { return true; }))
        {
            ASSERT_LT(++calls, 10);
        }
        ASSERT_EQ(dup_stub_create_counts, (std::vector<uint32_t>{12, 13}));
        EXPECT_EQ(st[2], SAI_STATUS_SUCCESS);
        EXPECT_EQ(st[3], SAI_STATUS_SUCCESS);
    }
}
