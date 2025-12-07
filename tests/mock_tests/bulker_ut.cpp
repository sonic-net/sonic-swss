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
}
