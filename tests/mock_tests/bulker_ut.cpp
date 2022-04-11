#include "ut_helper.h"
#include "bulker.h"

extern sai_route_api_t *sai_route_api;

namespace bulker_test
{
    using namespace std;

    struct BulkerTest : public ::testing::Test
    {
        BulkerTest()
        {
        }

        void SetUp() override
        {
            ASSERT_EQ(sai_route_api, nullptr);
            sai_route_api = new sai_route_api_t();
        }

        void TearDown() override
        {
            delete sai_route_api;
            sai_route_api = nullptr;
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
}
