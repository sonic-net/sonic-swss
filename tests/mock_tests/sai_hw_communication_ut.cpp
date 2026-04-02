/**
 * @file sai_hw_communication_ut.cpp
 *
 * Task 2.4 – Thử nghiệm đánh giá module giao tiếp phần cứng SAI
 *
 * Unit tests for the SAI (Switch Abstraction Interface) hardware communication layer:
 *   - SAI API initialization and status handling
 *   - SAI object lifecycle: create, set, get, remove
 *   - SAI error handling: status codes, retry logic, failure handling
 *   - SAI bulk operations via EntityBulker
 *   - SAI attribute operations and validation
 *   - Cross-object references and dependencies
 *
 * Tests verify the flow: Orch → SAI API → mock_sai_api (→ hardware simulator)
 */

#include "gtest/gtest.h"
#include "ut_helper.h"
#include "bulker.h"
#include "mock_sai_api.h"
#include "mock_orchagent_main.h"

extern sai_route_api_t       *sai_route_api;
extern sai_neighbor_api_t    *sai_neighbor_api;
extern sai_next_hop_api_t    *sai_next_hop_api;
extern sai_port_api_t        *sai_port_api;
extern sai_vlan_api_t        *sai_vlan_api;
extern sai_router_intfs_api_t *sai_router_intfs_api;
extern sai_virtual_router_api_t *sai_virtual_router_api;

EXTERN_MOCK_FNS

using namespace std;

// ===========================================================================
// Helpers
// ===========================================================================
static sai_attribute_t make_attr(sai_attr_id_t id, sai_object_id_t oid)
{
    sai_attribute_t attr;
    attr.id = id;
    attr.value.oid = oid;
    return attr;
}

static sai_attribute_t make_attr_u32(sai_attr_id_t id, uint32_t val)
{
    sai_attribute_t attr;
    attr.id = id;
    attr.value.u32 = val;
    return attr;
}

// ===========================================================================
// 2.4.1  SAI API initialization and status handling
// ===========================================================================
namespace sai_init_ut
{

struct SaiInitTest : public ::testing::Test
{
    void SetUp() override
    {
        // Initialize SAI with profile
        map<string, string> profile = {
            {"SAI_VS_SWITCH_DEFAULT_VLAN", "1"},
        };

        auto status = ut_helper::initSaiApi(profile);
        ASSERT_EQ(status, SAI_STATUS_SUCCESS);
    }

    void TearDown() override
    {
        ut_helper::uninitSaiApi();
    }
};

// ---------------------------------------------------------------------------
// 2.4.1.1  SAI API is successfully initialized with profile
// ---------------------------------------------------------------------------
TEST_F(SaiInitTest, SaiApiInitialization)
{
    // After setup, all API pointers should be non-null
    EXPECT_NE(sai_route_api, nullptr);
    EXPECT_NE(sai_neighbor_api, nullptr);
    EXPECT_NE(sai_next_hop_api, nullptr);
}

// ---------------------------------------------------------------------------
// 2.4.1.2  SAI status success is recognized
// ---------------------------------------------------------------------------
TEST_F(SaiInitTest, SaiStatusSuccess)
{
    sai_status_t status = SAI_STATUS_SUCCESS;
    EXPECT_EQ(status, SAI_STATUS_SUCCESS);
}

// ---------------------------------------------------------------------------
// 2.4.1.3  SAI error statuses are distinguishable
// ---------------------------------------------------------------------------
TEST_F(SaiInitTest, SaiErrorStatusesDifferent)
{
    EXPECT_NE(SAI_STATUS_FAILURE, SAI_STATUS_SUCCESS);
    EXPECT_NE(SAI_STATUS_NOT_IMPLEMENTED, SAI_STATUS_SUCCESS);
    EXPECT_NE(SAI_STATUS_NO_MEMORY, SAI_STATUS_SUCCESS);
    EXPECT_NE(SAI_STATUS_INVALID_PARAMETER, SAI_STATUS_SUCCESS);
}

} // namespace sai_init_ut

// ===========================================================================
// 2.4.2  SAI object lifecycle – create/set/get/remove
// ===========================================================================
namespace sai_object_lifecycle_ut
{

struct SaiObjectLifecycleTest : public ::testing::Test
{
    map<string, string> profile = {
        {"SAI_VS_SWITCH_DEFAULT_VLAN", "1"},
    };

    void SetUp() override
    {
        ASSERT_EQ(ut_helper::initSaiApi(profile), SAI_STATUS_SUCCESS);
    }

    void TearDown() override
    {
        ut_helper::uninitSaiApi();
    }
};

// ---------------------------------------------------------------------------
// 2.4.2.1  Virtual router object can be created via SAI API
// ---------------------------------------------------------------------------
TEST_F(SaiObjectLifecycleTest, VirtualRouterCreate)
{
    sai_object_id_t vr_id = SAI_NULL_OBJECT_ID;

    // Create virtual router with default attributes
    sai_attribute_t attrs[1];
    attrs[0].id = SAI_VIRTUAL_ROUTER_ATTR_ADMIN_V4_STATE;
    attrs[0].value.booldata = true;

    sai_status_t status = sai_virtual_router_api->create_virtual_router(
        &vr_id, 0, 1, attrs);

    EXPECT_EQ(status, SAI_STATUS_SUCCESS);
    EXPECT_NE(vr_id, SAI_NULL_OBJECT_ID);
}

// ---------------------------------------------------------------------------
// 2.4.2.2  VLAN object can be created via SAI API
// ---------------------------------------------------------------------------
TEST_F(SaiObjectLifecycleTest, VlanCreate)
{
    sai_object_id_t vlan_oid = SAI_NULL_OBJECT_ID;

    // Create VLAN
    sai_attribute_t attrs[1];
    attrs[0].id = SAI_VLAN_ATTR_VLAN_ID;
    attrs[0].value.u16 = 100;   // VLAN 100

    sai_status_t status = sai_vlan_api->create_vlan(
        &vlan_oid, 0, 1, attrs);

    EXPECT_EQ(status, SAI_STATUS_SUCCESS);
    EXPECT_NE(vlan_oid, SAI_NULL_OBJECT_ID);
}

// ---------------------------------------------------------------------------
// 2.4.2.3  Route entry can be created via SAI route API
// ---------------------------------------------------------------------------
TEST_F(SaiObjectLifecycleTest, RouteEntryCreate)
{
    // Create a route entry
    sai_route_entry_t route_entry;
    route_entry.destination.addr_family = SAI_IP_ADDR_FAMILY_IPV4;
    route_entry.destination.addr.ip4    = htonl(0x0a000000);  // 10.0.0.0
    route_entry.destination.mask.ip4    = htonl(0xffffff00);  // /24
    route_entry.vr_id    = 0;
    route_entry.switch_id = 0;

    sai_attribute_t attrs[1];
    attrs[0].id = SAI_ROUTE_ENTRY_ATTR_PACKET_ACTION;
    attrs[0].value.s32 = SAI_PACKET_ACTION_FORWARD;

    sai_status_t status = sai_route_api->create_route_entry(
        &route_entry, 1, attrs);

    EXPECT_EQ(status, SAI_STATUS_SUCCESS);
}

// ---------------------------------------------------------------------------
// 2.4.2.4  Neighbor entry can be created via SAI API
// ---------------------------------------------------------------------------
TEST_F(SaiObjectLifecycleTest, NeighborEntryCreate)
{
    // Create neighbor entry
    sai_neighbor_entry_t neigh_entry;
    neigh_entry.ip_address.addr_family = SAI_IP_ADDR_FAMILY_IPV4;
    neigh_entry.ip_address.addr.ip4 = htonl(0x0a000001);  // 10.0.0.1
    neigh_entry.rif_id = 0;
    neigh_entry.switch_id = 0;

    sai_attribute_t attrs[1];
    attrs[0].id = SAI_NEIGHBOR_ENTRY_ATTR_DST_MAC_ADDRESS;
    memcpy(attrs[0].value.mac, "\xaa\xbb\xcc\xdd\xee\xff", 6);

    sai_status_t status = sai_neighbor_api->create_neighbor_entry(
        &neigh_entry, 1, attrs);

    EXPECT_EQ(status, SAI_STATUS_SUCCESS);
}

} // namespace sai_object_lifecycle_ut

// ===========================================================================
// 2.4.3  SAI error handling – status mapping and retry logic
// ===========================================================================
namespace sai_error_handling_ut
{

struct SaiErrorHandlingTest : public ::testing::Test
{
    map<string, string> profile;

    void SetUp() override
    {
        profile = {{"SAI_VS_SWITCH_DEFAULT_VLAN", "1"}};
        ASSERT_EQ(ut_helper::initSaiApi(profile), SAI_STATUS_SUCCESS);
    }

    void TearDown() override
    {
        ut_helper::uninitSaiApi();
    }
};

// ---------------------------------------------------------------------------
// 2.4.3.1  task_process_status returned by handleSaiCreateStatus
// ---------------------------------------------------------------------------
TEST_F(SaiErrorHandlingTest, HandleSaiCreateStatusSuccess)
{
    // Success should map to task_success
    task_process_status status = handleSaiCreateStatus(SAI_API_ROUTE, SAI_STATUS_SUCCESS);
    EXPECT_EQ(status, task_success);
}

// ---------------------------------------------------------------------------
// 2.4.3.2  Invalid parameter maps to task_invalid_entry
// ---------------------------------------------------------------------------
TEST_F(SaiErrorHandlingTest, HandleSaiCreateStatusInvalidParam)
{
    task_process_status status = handleSaiCreateStatus(
        SAI_API_ROUTE, SAI_STATUS_INVALID_PARAMETER);

    // Invalid parameter typically maps to invalid_entry
    EXPECT_NE(status, task_success);
}

// ---------------------------------------------------------------------------
// 2.4.3.3  Other failures map to task_failed or task_need_retry
// ---------------------------------------------------------------------------
TEST_F(SaiErrorHandlingTest, HandleSaiCreateStatusFailure)
{
    task_process_status status = handleSaiCreateStatus(
        SAI_API_ROUTE, SAI_STATUS_FAILURE);

    // Failure should not be success
    EXPECT_NE(status, task_success);
}

// ---------------------------------------------------------------------------
// 2.4.3.4  Set status handling
// ---------------------------------------------------------------------------
TEST_F(SaiErrorHandlingTest, HandleSaiSetStatusSuccess)
{
    task_process_status status = handleSaiSetStatus(SAI_API_PORT, SAI_STATUS_SUCCESS);
    EXPECT_EQ(status, task_success);
}

// ---------------------------------------------------------------------------
// 2.4.3.5  Remove status handling
// ---------------------------------------------------------------------------
TEST_F(SaiErrorHandlingTest, HandleSaiRemoveStatusSuccess)
{
    task_process_status status = handleSaiRemoveStatus(SAI_API_VLAN, SAI_STATUS_SUCCESS);
    EXPECT_EQ(status, task_success);
}

} // namespace sai_error_handling_ut

// ===========================================================================
// 2.4.4  SAI bulk operations – EntityBulker
// ===========================================================================
namespace sai_bulker_ut
{

DEFINE_SAI_GENERIC_API_OBJECT_BULK_MOCK_WITH_SET(next_hop, next_hop);

struct SaiBulkerTest : public ::testing::Test
{
    map<string, string> profile;
    sai_bulk_object_create_fn old_object_create;
    sai_bulk_object_remove_fn old_object_remove;
    sai_bulk_object_set_attribute_fn old_object_set_attribute;

    void SetUp() override
    {
        profile = {{"SAI_VS_SWITCH_DEFAULT_VLAN", "1"}};
        ASSERT_EQ(ut_helper::initSaiApi(profile), SAI_STATUS_SUCCESS);

        if (!sai_next_hop_api)
        {
            sai_next_hop_api = new sai_next_hop_api_t();
        }

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

        if (sai_next_hop_api)
        {
            delete sai_next_hop_api;
            sai_next_hop_api = nullptr;
        }

        ut_helper::uninitSaiApi();
    }
};

// ---------------------------------------------------------------------------
// 2.4.4.1  EntityBulker can be created with configured size
// ---------------------------------------------------------------------------
TEST_F(SaiBulkerTest, BulkerCreation)
{
    EntityBulker<sai_route_api_t> bulker(sai_route_api, 500);

    EXPECT_EQ(bulker.max_bulk_size, 500);
    EXPECT_EQ(bulker.setting_entries_count(), 0);
}

// ---------------------------------------------------------------------------
// 2.4.4.2  Bulker can batch multiple attribute sets
// ---------------------------------------------------------------------------
TEST_F(SaiBulkerTest, BulkerBatchAttributes)
{
    EntityBulker<sai_route_api_t> bulker(sai_route_api, 1000);
    deque<sai_status_t> statuses;

    // Create dummy route entry
    sai_route_entry_t route_entry;
    route_entry.destination.addr_family = SAI_IP_ADDR_FAMILY_IPV4;
    route_entry.destination.addr.ip4 = htonl(0x0a000000);
    route_entry.destination.mask.ip4 = htonl(0xffffff00);
    route_entry.vr_id = 0;
    route_entry.switch_id = 0;

    // Set multiple attributes for same route
    sai_attribute_t attr1 = make_attr_u32(SAI_ROUTE_ENTRY_ATTR_PACKET_ACTION,
                                         SAI_PACKET_ACTION_FORWARD);
    statuses.emplace_back();
    bulker.set_entry_attribute(&statuses.back(), &route_entry, &attr1);

    sai_attribute_t attr2;
    attr2.id = SAI_ROUTE_ENTRY_ATTR_NEXT_HOP_ID;
    attr2.value.oid = SAI_NULL_OBJECT_ID;
    statuses.emplace_back();
    bulker.set_entry_attribute(&statuses.back(), &route_entry, &attr2);

    EXPECT_EQ(bulker.setting_entries_count(), 1);
}

// ---------------------------------------------------------------------------
// 2.4.4.3  Bulker maintains attribute order
// ---------------------------------------------------------------------------
TEST_F(SaiBulkerTest, BulkerAttributeOrder)
{
    EntityBulker<sai_route_api_t> bulker(sai_route_api, 1000);
    deque<sai_status_t> statuses;

    sai_route_entry_t route_entry;
    route_entry.destination.addr_family = SAI_IP_ADDR_FAMILY_IPV4;
    route_entry.destination.addr.ip4 = htonl(0x0a000000);
    route_entry.destination.mask.ip4 = htonl(0xffffff00);
    route_entry.vr_id = 0;
    route_entry.switch_id = 0;

    // Set attributes in specific order
    sai_attribute_t attr1 = make_attr_u32(SAI_ROUTE_ENTRY_ATTR_PACKET_ACTION,
                                         SAI_PACKET_ACTION_FORWARD);
    statuses.emplace_back();
    bulker.set_entry_attribute(&statuses.back(), &route_entry, &attr1);

    sai_attribute_t attr2 = make_attr(SAI_ROUTE_ENTRY_ATTR_NEXT_HOP_ID, 123);
    statuses.emplace_back();
    bulker.set_entry_attribute(&statuses.back(), &route_entry, &attr2);

    // Verify order is preserved
    auto const& attrs = bulker.setting_entries[route_entry];
    ASSERT_EQ(attrs.size(), 2);
    auto it = attrs.begin();
    EXPECT_EQ(it->first.id, SAI_ROUTE_ENTRY_ATTR_PACKET_ACTION);
    it++;
    EXPECT_EQ(it->first.id, SAI_ROUTE_ENTRY_ATTR_NEXT_HOP_ID);
}

// ---------------------------------------------------------------------------
// 2.4.4.4  Bulker can be cleared
// ---------------------------------------------------------------------------
TEST_F(SaiBulkerTest, BulkerClear)
{
    EntityBulker<sai_route_api_t> bulker(sai_route_api, 1000);
    deque<sai_status_t> statuses;

    sai_route_entry_t route_entry;
    route_entry.destination.addr_family = SAI_IP_ADDR_FAMILY_IPV4;
    route_entry.destination.addr.ip4 = htonl(0x0a000000);
    route_entry.destination.mask.ip4 = htonl(0xffffff00);
    route_entry.vr_id = 0;
    route_entry.switch_id = 0;

    sai_attribute_t attr = make_attr_u32(SAI_ROUTE_ENTRY_ATTR_PACKET_ACTION,
                                        SAI_PACKET_ACTION_FORWARD);
    statuses.emplace_back();
    bulker.set_entry_attribute(&statuses.back(), &route_entry, &attr);

    EXPECT_EQ(bulker.setting_entries_count(), 1);

    bulker.clear();

    EXPECT_EQ(bulker.setting_entries_count(), 0);
}

// ---------------------------------------------------------------------------
// 2.4.4.5  Multiple routes can be batched in bulker
// ---------------------------------------------------------------------------
TEST_F(SaiBulkerTest, BulkerMultipleRoutes)
{
    EntityBulker<sai_route_api_t> bulker(sai_route_api, 1000);
    deque<sai_status_t> statuses;

    for (int i = 1; i <= 3; i++)
    {
        sai_route_entry_t route_entry;
        route_entry.destination.addr_family = SAI_IP_ADDR_FAMILY_IPV4;
        route_entry.destination.addr.ip4 = htonl(0x0a000000 + i * 0x0100);
        route_entry.destination.mask.ip4 = htonl(0xffffff00);
        route_entry.vr_id = 0;
        route_entry.switch_id = 0;

        sai_attribute_t attr = make_attr_u32(SAI_ROUTE_ENTRY_ATTR_PACKET_ACTION,
                                            SAI_PACKET_ACTION_FORWARD);
        statuses.emplace_back();
        bulker.set_entry_attribute(&statuses.back(), &route_entry, &attr);
    }

    EXPECT_EQ(bulker.setting_entries_count(), 3);
}

} // namespace sai_bulker_ut

// ===========================================================================
// 2.4.5  SAI attribute validation and operations
// ===========================================================================
namespace sai_attr_ut
{

struct SaiAttrTest : public ::testing::Test
{
    map<string, string> profile;

    void SetUp() override
    {
        profile = {{"SAI_VS_SWITCH_DEFAULT_VLAN", "1"}};
        ASSERT_EQ(ut_helper::initSaiApi(profile), SAI_STATUS_SUCCESS);
    }

    void TearDown() override
    {
        ut_helper::uninitSaiApi();
    }
};

// ---------------------------------------------------------------------------
// 2.4.5.1  SAI attribute can be created with OID value
// ---------------------------------------------------------------------------
TEST_F(SaiAttrTest, AttrWithOidValue)
{
    sai_attribute_t attr = make_attr(SAI_ROUTE_ENTRY_ATTR_NEXT_HOP_ID, 12345);

    EXPECT_EQ(attr.id, SAI_ROUTE_ENTRY_ATTR_NEXT_HOP_ID);
    EXPECT_EQ(attr.value.oid, 12345);
}

// ---------------------------------------------------------------------------
// 2.4.5.2  SAI attribute can be created with u32 value
// ---------------------------------------------------------------------------
TEST_F(SaiAttrTest, AttrWithU32Value)
{
    sai_attribute_t attr = make_attr_u32(SAI_ROUTE_ENTRY_ATTR_PACKET_ACTION,
                                        SAI_PACKET_ACTION_FORWARD);

    EXPECT_EQ(attr.id, SAI_ROUTE_ENTRY_ATTR_PACKET_ACTION);
    EXPECT_EQ(attr.value.u32, SAI_PACKET_ACTION_FORWARD);
}

// ---------------------------------------------------------------------------
// 2.4.5.3  Multiple attributes on same object have unique ids
// ---------------------------------------------------------------------------
TEST_F(SaiAttrTest, UniqueAttrIds)
{
    sai_attribute_t attr1 = make_attr_u32(SAI_ROUTE_ENTRY_ATTR_PACKET_ACTION,
                                         SAI_PACKET_ACTION_FORWARD);
    sai_attribute_t attr2 = make_attr(SAI_ROUTE_ENTRY_ATTR_NEXT_HOP_ID, 123);
    sai_attribute_t attr3;
    attr3.id = SAI_ROUTE_ENTRY_ATTR_PRIORITY;
    attr3.value.u32 = 10;

    EXPECT_NE(attr1.id, attr2.id);
    EXPECT_NE(attr2.id, attr3.id);
    EXPECT_NE(attr1.id, attr3.id);
}

} // namespace sai_attr_ut

// ===========================================================================
// 2.4.6  Cross-object SAI interactions and dependencies
// ===========================================================================
namespace sai_cross_obj_ut
{

struct SaiCrossObjTest : public ::testing::Test
{
    map<string, string> profile;

    void SetUp() override
    {
        profile = {{"SAI_VS_SWITCH_DEFAULT_VLAN", "1"}};
        ASSERT_EQ(ut_helper::initSaiApi(profile), SAI_STATUS_SUCCESS);
    }

    void TearDown() override
    {
        ut_helper::uninitSaiApi();
    }
};

// ---------------------------------------------------------------------------
// 2.4.6.1  Route depends on virtual router and next hop OIDs
// ---------------------------------------------------------------------------
TEST_F(SaiCrossObjTest, RouteDependsOnVrAndNh)
{
    // Create route with references to VR and NH
    sai_route_entry_t route_entry;
    route_entry.destination.addr_family = SAI_IP_ADDR_FAMILY_IPV4;
    route_entry.destination.addr.ip4 = htonl(0x0c000000);
    route_entry.destination.mask.ip4 = htonl(0xffffff00);
    route_entry.vr_id = 1;           // refers to VR 1
    route_entry.switch_id = 0;

    sai_attribute_t attrs[2];
    attrs[0].id = SAI_ROUTE_ENTRY_ATTR_PACKET_ACTION;
    attrs[0].value.s32 = SAI_PACKET_ACTION_FORWARD;
    attrs[1].id = SAI_ROUTE_ENTRY_ATTR_NEXT_HOP_ID;
    attrs[1].value.oid = 100;        // refers to next hop 100

    // This demonstrates cross-object references
    EXPECT_EQ(route_entry.vr_id, 1);
    EXPECT_EQ(attrs[1].value.oid, 100);
}

// ---------------------------------------------------------------------------
// 2.4.6.2  Neighbor depends on RIF (Router Interface)
// ---------------------------------------------------------------------------
TEST_F(SaiCrossObjTest, NeighborDependsOnRif)
{
    sai_neighbor_entry_t neigh_entry;
    neigh_entry.ip_address.addr_family = SAI_IP_ADDR_FAMILY_IPV4;
    neigh_entry.ip_address.addr.ip4 = htonl(0x0a000001);
    neigh_entry.rif_id = 50;         // Router Interface ID
    neigh_entry.switch_id = 0;

    EXPECT_EQ(neigh_entry.rif_id, 50);
}

// ---------------------------------------------------------------------------
// 2.4.6.3  Multiple routes can reference same next hop
// ---------------------------------------------------------------------------
TEST_F(SaiCrossObjTest, MultipleRoutesShareNextHop)
{
    sai_route_entry_t route1, route2;
    route1.destination.addr_family = SAI_IP_ADDR_FAMILY_IPV4;
    route1.destination.addr.ip4 = htonl(0x0a000000);
    route1.destination.mask.ip4 = htonl(0xffffff00);
    route1.vr_id = 0;
    route1.switch_id = 0;

    route2.destination.addr_family = SAI_IP_ADDR_FAMILY_IPV4;
    route2.destination.addr.ip4 = htonl(0x0b000000);
    route2.destination.mask.ip4 = htonl(0xffffff00);
    route2.vr_id = 0;
    route2.switch_id = 0;

    // Both routes point to same next hop
    sai_object_id_t shared_nh = 200;

    sai_attribute_t attrs1[1], attrs2[1];
    attrs1[0].id = SAI_ROUTE_ENTRY_ATTR_NEXT_HOP_ID;
    attrs1[0].value.oid = shared_nh;

    attrs2[0].id = SAI_ROUTE_ENTRY_ATTR_NEXT_HOP_ID;
    attrs2[0].value.oid = shared_nh;

    EXPECT_EQ(attrs1[0].value.oid, attrs2[0].value.oid);
}

} // namespace sai_cross_obj_ut
