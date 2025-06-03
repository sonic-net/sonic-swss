#include "ut_helper.h"
#include "bulker.h"
#include "mock_sai_api.h"

extern sai_route_api_t *sai_route_api;
extern sai_neighbor_api_t *sai_neighbor_api;

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
        }

        void PostSetUp()
        {
            INIT_SAI_API_MOCK(next_hop);
            MockSaiApis();
            old_object_create = sai_next_hop_api->create_next_hops;
            old_object_remove = sai_next_hop_api->remove_next_hops;
            old_object_set_attribute = sai_next_hop_api->set_next_hops_attribute;
            sai_next_hop_api->create_next_hops = mock_create_next_hops;
            sai_next_hop_api->remove_next_hops = mock_remove_next_hops;
            sai_next_hop_api->set_next_hops_attribute = mock_set_next_hops_attribute;
        }

        void PreTearDown()
        {
            RestoreSaiApis();
            DEINIT_SAI_API_MOCK(next_hop);
            sai_next_hop_api->create_next_hops = old_object_create;
            sai_next_hop_api->remove_next_hops = old_object_remove;
            sai_next_hop_api->set_next_hops_attribute = old_object_set_attribute;
        }

        void TearDown() override
        {
            delete sai_route_api;
            sai_route_api = nullptr;

            delete sai_neighbor_api;
            sai_neighbor_api = nullptr;
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
        sai_attribute_t next_hop_attr;
        sai_object_id_t next_hop_id_0 = 0x0;
        sai_object_id_t next_hop_id_1 = 0x1;
        std::vector<sai_status_t> exp_status{SAI_STATUS_SUCCESS};

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

        gNextHopBulker.create_entry(&next_hop_id_0 , (uint32_t)next_hop_attrs.size(), next_hop_attrs.data());
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

        gNextHopBulker.create_entry(&next_hop_id_1 , (uint32_t)next_hop_attrs.size(), next_hop_attrs.data());
        next_hop_attrs.clear();

        EXPECT_CALL(*mock_sai_next_hop_api, create_next_hops)
            .WillOnce(DoAll(SetArrayArgument<6>(exp_status.begin(), exp_status.end()), Return(SAI_STATUS_SUCCESS)));
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
        vector<sai_status_t> statuses;
        statuses.emplace_back();
        gNextHopBulker.remove_entry(&statuses.back(), next_hop_id_0);
        statuses.emplace_back();
        gNextHopBulker.remove_entry(&statuses.back(), next_hop_id_1);

        EXPECT_CALL(*mock_sai_next_hop_api, remove_next_hops)
            .WillOnce(DoAll(SetArrayArgument<3>(exp_status.begin(), exp_status.end()), Return(SAI_STATUS_SUCCESS)));
        gNextHopBulker.flush();
    }
}
