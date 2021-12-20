#include "next_hop_manager.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <functional>
#include <string>
#include <unordered_map>

#include "ipaddress.h"
#include "json.hpp"
#include "mock_response_publisher.h"
#include "mock_sai_next_hop.h"
#include "p4oidmapper.h"
#include "p4orch/p4orch_util.h"
#include "p4orch_util.h"
#include "return_code.h"
#include "swssnet.h"
extern "C"
{
#include "sai.h"
}

using ::p4orch::kTableKeyDelimiter;

using ::testing::_;
using ::testing::DoAll;
using ::testing::Eq;
using ::testing::Return;
using ::testing::SetArgPointee;
using ::testing::StrictMock;
using ::testing::Truly;

extern sai_object_id_t gSwitchId;
extern sai_next_hop_api_t *sai_next_hop_api;
extern MockSaiNextHop *mock_sai_next_hop;

namespace
{

constexpr char *kNextHopId = "8";
constexpr char *kNextHopP4AppDbKey = R"({"match/nexthop_id":"8"})";
constexpr sai_object_id_t kNextHopOid = 1;
constexpr char *kRouterInterfaceId1 = "16";
constexpr char *kRouterInterfaceId2 = "17";
constexpr sai_object_id_t kRouterInterfaceOid1 = 1;
constexpr sai_object_id_t kRouterInterfaceOid2 = 2;
constexpr char *kNeighborId1 = "10.0.0.1";
constexpr char *kNeighborId2 = "fe80::21a:11ff:fe17:5f80";

// APP DB entries for Add and Update request.
const P4NextHopAppDbEntry kP4NextHopAppDbEntry1{/*next_hop_id=*/kNextHopId, /*router_interface_id=*/kRouterInterfaceId1,
                                                /*neighbor_id=*/swss::IpAddress(kNeighborId1),
                                                /*is_set_router_interface_id=*/true, /*is_set_neighbor_id=*/true};

const P4NextHopAppDbEntry kP4NextHopAppDbEntry2{/*next_hop_id=*/kNextHopId, /*router_interface_id=*/kRouterInterfaceId2,
                                                /*neighbor_id=*/swss::IpAddress(kNeighborId2),
                                                /*is_set_router_interface_id=*/true, /*is_set_neighbor_id=*/true};

// APP DB entries for Delete request.
const P4NextHopAppDbEntry kP4NextHopAppDbEntry3{/*next_hop_id=*/kNextHopId, /*router_interface_id=*/"",
                                                /*neighbor_id=*/swss::IpAddress(),
                                                /*is_set_router_interface_id=*/false, /*is_set_neighbor_id=*/false};

std::unordered_map<sai_attr_id_t, sai_attribute_value_t> CreateAttributeListForNextHopObject(
    const P4NextHopAppDbEntry &app_entry, const sai_object_id_t &rif_oid)
{
    std::unordered_map<sai_attr_id_t, sai_attribute_value_t> next_hop_attrs;
    sai_attribute_t next_hop_attr;

    next_hop_attr.id = SAI_NEXT_HOP_ATTR_TYPE;
    next_hop_attr.value.s32 = SAI_NEXT_HOP_TYPE_IP;
    next_hop_attrs.insert({next_hop_attr.id, next_hop_attr.value});

    next_hop_attr.id = SAI_NEXT_HOP_ATTR_IP;
    swss::copy(next_hop_attr.value.ipaddr, app_entry.neighbor_id);
    next_hop_attrs.insert({next_hop_attr.id, next_hop_attr.value});

    next_hop_attr.id = SAI_NEXT_HOP_ATTR_ROUTER_INTERFACE_ID;
    next_hop_attr.value.oid = rif_oid;
    next_hop_attrs.insert({next_hop_attr.id, next_hop_attr.value});

    return next_hop_attrs;
}

// Verifies whether the attribute list is the same as expected for SAI next
// hop's create_next_hop().
// Returns true if they match; otherwise, false.
bool MatchCreateNextHopArgAttrList(const sai_attribute_t *attr_list,
                                   const std::unordered_map<sai_attr_id_t, sai_attribute_value_t> &expected_attr_list)
{
    if (attr_list == nullptr)
    {
        return false;
    }

    // Sanity check for expected_attr_list.
    const auto end = expected_attr_list.end();
    if (expected_attr_list.size() != 3 || expected_attr_list.find(SAI_NEXT_HOP_ATTR_TYPE) == end ||
        expected_attr_list.find(SAI_NEXT_HOP_ATTR_IP) == end ||
        expected_attr_list.find(SAI_NEXT_HOP_ATTR_ROUTER_INTERFACE_ID) == end)
    {
        return false;
    }

    for (int i = 0; i < 3; ++i)
    {
        switch (attr_list[i].id)
        {
        case SAI_NEXT_HOP_ATTR_TYPE:
            if (attr_list[i].value.s32 != expected_attr_list.at(SAI_NEXT_HOP_ATTR_TYPE).s32)
                return false;
            break;
        case SAI_NEXT_HOP_ATTR_IP: {
            auto construct_ip_addr = [](const sai_ip_address_t &sai_ip_address) -> swss::IpAddress {
                swss::ip_addr_t ipaddr;
                if (sai_ip_address.addr_family == SAI_IP_ADDR_FAMILY_IPV4)
                {
                    ipaddr.family = AF_INET;
                    ipaddr.ip_addr.ipv4_addr = sai_ip_address.addr.ip4;
                }
                else
                {
                    ipaddr.family = AF_INET6;
                    memcpy(&ipaddr.ip_addr.ipv6_addr, &sai_ip_address.addr.ip6, sizeof(ipaddr.ip_addr.ipv6_addr));
                }

                return swss::IpAddress(ipaddr);
            };

            auto ipaddr = construct_ip_addr(attr_list[i].value.ipaddr);
            auto expected_ipaddr = construct_ip_addr(expected_attr_list.at(SAI_NEXT_HOP_ATTR_IP).ipaddr);
            if (ipaddr != expected_ipaddr)
            {
                return false;
            }
            break;
        }
        case SAI_NEXT_HOP_ATTR_ROUTER_INTERFACE_ID:
            if (attr_list[i].value.oid != expected_attr_list.at(SAI_NEXT_HOP_ATTR_ROUTER_INTERFACE_ID).oid)
            {
                return false;
            }
            break;
        default:
            // Invalid attribute ID in next hop's attribute list.
            return false;
        }
    }

    return true;
}

} // namespace

class NextHopManagerTest : public ::testing::Test
{
  protected:
    NextHopManagerTest() : next_hop_manager_(&p4_oid_mapper_, &publisher_)
    {
    }

    void SetUp() override
    {
        // Set up mock stuff for SAI next hop API structure.
        mock_sai_next_hop = &mock_sai_next_hop_;
        sai_next_hop_api->create_next_hop = mock_create_next_hop;
        sai_next_hop_api->remove_next_hop = mock_remove_next_hop;
        sai_next_hop_api->set_next_hop_attribute = mock_set_next_hop_attribute;
        sai_next_hop_api->get_next_hop_attribute = mock_get_next_hop_attribute;
    }

    void Enqueue(const swss::KeyOpFieldsValuesTuple &entry)
    {
        next_hop_manager_.enqueue(entry);
    }

    void Drain()
    {
        next_hop_manager_.drain();
    }

    ReturnCode ProcessAddRequest(const P4NextHopAppDbEntry &app_db_entry)
    {
        return next_hop_manager_.processAddRequest(app_db_entry);
    }

    ReturnCode ProcessUpdateRequest(const P4NextHopAppDbEntry &app_db_entry, P4NextHopEntry *next_hop_entry)
    {
        return next_hop_manager_.processUpdateRequest(app_db_entry, next_hop_entry);
    }

    ReturnCode ProcessDeleteRequest(const std::string &next_hop_key)
    {
        return next_hop_manager_.processDeleteRequest(next_hop_key);
    }

    P4NextHopEntry *GetNextHopEntry(const std::string &next_hop_key)
    {
        return next_hop_manager_.getNextHopEntry(next_hop_key);
    }

    ReturnCodeOr<P4NextHopAppDbEntry> DeserializeP4NextHopAppDbEntry(
        const std::string &key, const std::vector<swss::FieldValueTuple> &attributes)
    {
        return next_hop_manager_.deserializeP4NextHopAppDbEntry(key, attributes);
    }

    // Resolves the dependency of a next hop entry by adding depended router
    // interface and neighbor into centralized mapper.
    // Returns true on succuess.
    bool ResolveNextHopEntryDependency(const P4NextHopAppDbEntry &app_db_entry, const sai_object_id_t &rif_oid);

    // Adds the next hop entry -- kP4NextHopAppDbEntry1, via next hop manager's
    // ProcessAddRequest (). This function also takes care of all the dependencies
    // of the next hop entry.
    // Returns a valid pointer to next hop entry on success.
    P4NextHopEntry *AddNextHopEntry1();

    // Validates that a P4 App next hop entry is correctly added in next hop
    // manager and centralized mapper. Returns true on success.
    bool ValidateNextHopEntryAdd(const P4NextHopAppDbEntry &app_db_entry, const sai_object_id_t &expected_next_hop_oid);

    // Return true if the specified the object has the expected number of
    // reference.
    bool ValidateRefCnt(sai_object_type_t object_type, const std::string &key, uint32_t expected_ref_count)
    {
        uint32_t ref_count;
        if (!p4_oid_mapper_.getRefCount(object_type, key, &ref_count))
            return false;
        return ref_count == expected_ref_count;
    }

    StrictMock<MockSaiNextHop> mock_sai_next_hop_;
    MockResponsePublisher publisher_;
    P4OidMapper p4_oid_mapper_;
    NextHopManager next_hop_manager_;
};

bool NextHopManagerTest::ResolveNextHopEntryDependency(const P4NextHopAppDbEntry &app_db_entry,
                                                       const sai_object_id_t &rif_oid)
{
    const std::string rif_key = KeyGenerator::generateRouterInterfaceKey(app_db_entry.router_interface_id);
    if (!p4_oid_mapper_.setOID(SAI_OBJECT_TYPE_ROUTER_INTERFACE, rif_key, rif_oid))
    {
        return false;
    }
    const std::string neighbor_key =
        KeyGenerator::generateNeighborKey(app_db_entry.router_interface_id, app_db_entry.neighbor_id);
    if (!p4_oid_mapper_.setDummyOID(SAI_OBJECT_TYPE_NEIGHBOR_ENTRY, neighbor_key))
    {
        return false;
    }
    return true;
}

P4NextHopEntry *NextHopManagerTest::AddNextHopEntry1()
{
    if (!ResolveNextHopEntryDependency(kP4NextHopAppDbEntry1, kRouterInterfaceOid1))
    {
        return nullptr;
    }

    // Set up mock call.
    EXPECT_CALL(mock_sai_next_hop_,
                create_next_hop(
                    ::testing::NotNull(), Eq(gSwitchId), Eq(3),
                    Truly(std::bind(MatchCreateNextHopArgAttrList, std::placeholders::_1,
                                    CreateAttributeListForNextHopObject(kP4NextHopAppDbEntry1, kRouterInterfaceOid1)))))
        .WillOnce(DoAll(SetArgPointee<0>(kNextHopOid), Return(SAI_STATUS_SUCCESS)));

    EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS, ProcessAddRequest(kP4NextHopAppDbEntry1));

    return GetNextHopEntry(KeyGenerator::generateNextHopKey(kP4NextHopAppDbEntry1.next_hop_id));
}

bool NextHopManagerTest::ValidateNextHopEntryAdd(const P4NextHopAppDbEntry &app_db_entry,
                                                 const sai_object_id_t &expected_next_hop_oid)
{
    const auto *p4_next_hop_entry = GetNextHopEntry(KeyGenerator::generateNextHopKey(app_db_entry.next_hop_id));
    if (p4_next_hop_entry == nullptr || p4_next_hop_entry->next_hop_id != app_db_entry.next_hop_id ||
        p4_next_hop_entry->router_interface_id != app_db_entry.router_interface_id ||
        p4_next_hop_entry->neighbor_id != app_db_entry.neighbor_id ||
        p4_next_hop_entry->next_hop_oid != expected_next_hop_oid)
        return false;

    sai_object_id_t next_hop_oid;
    if (!p4_oid_mapper_.getOID(SAI_OBJECT_TYPE_NEXT_HOP, p4_next_hop_entry->next_hop_key, &next_hop_oid) ||
        next_hop_oid != expected_next_hop_oid)
    {
        return false;
    }

    return true;
}

TEST_F(NextHopManagerTest, ProcessAddRequestShouldSucceedAddingNewNextHop)
{
    ASSERT_TRUE(ResolveNextHopEntryDependency(kP4NextHopAppDbEntry1, kRouterInterfaceOid1));

    const std::string rif_key = KeyGenerator::generateRouterInterfaceKey(kP4NextHopAppDbEntry1.router_interface_id);
    const std::string neighbor_key =
        KeyGenerator::generateNeighborKey(kP4NextHopAppDbEntry1.router_interface_id, kP4NextHopAppDbEntry1.neighbor_id);
    uint32_t original_rif_ref_count;
    ASSERT_TRUE(p4_oid_mapper_.getRefCount(SAI_OBJECT_TYPE_ROUTER_INTERFACE, rif_key, &original_rif_ref_count));
    uint32_t original_neighbor_ref_count;
    ASSERT_TRUE(p4_oid_mapper_.getRefCount(SAI_OBJECT_TYPE_NEIGHBOR_ENTRY, neighbor_key, &original_neighbor_ref_count));

    // Set up mock call.
    EXPECT_CALL(mock_sai_next_hop_,
                create_next_hop(
                    ::testing::NotNull(), Eq(gSwitchId), Eq(3),
                    Truly(std::bind(MatchCreateNextHopArgAttrList, std::placeholders::_1,
                                    CreateAttributeListForNextHopObject(kP4NextHopAppDbEntry1, kRouterInterfaceOid1)))))
        .WillOnce(DoAll(SetArgPointee<0>(kNextHopOid), Return(SAI_STATUS_SUCCESS)));

    EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS, ProcessAddRequest(kP4NextHopAppDbEntry1));

    EXPECT_TRUE(ValidateNextHopEntryAdd(kP4NextHopAppDbEntry1, kNextHopOid));
    EXPECT_TRUE(ValidateRefCnt(SAI_OBJECT_TYPE_ROUTER_INTERFACE, rif_key, original_rif_ref_count + 1));
    EXPECT_TRUE(ValidateRefCnt(SAI_OBJECT_TYPE_NEIGHBOR_ENTRY, neighbor_key, original_neighbor_ref_count + 1));
}

TEST_F(NextHopManagerTest, ProcessAddRequestShouldFailWhenNextHopExistInCentralMapper)
{
    ASSERT_TRUE(ResolveNextHopEntryDependency(kP4NextHopAppDbEntry1, kRouterInterfaceOid1));
    ASSERT_TRUE(p4_oid_mapper_.setOID(
        SAI_OBJECT_TYPE_NEXT_HOP, KeyGenerator::generateNextHopKey(kP4NextHopAppDbEntry1.next_hop_id), kNextHopOid));
    // (TODO): Expect critical state.
    EXPECT_EQ(StatusCode::SWSS_RC_INTERNAL, ProcessAddRequest(kP4NextHopAppDbEntry1));
}

TEST_F(NextHopManagerTest, ProcessAddRequestShouldFailWhenDependingRifIsAbsentInCentralMapper)
{
    const std::string neighbor_key =
        KeyGenerator::generateNeighborKey(kP4NextHopAppDbEntry1.router_interface_id, kP4NextHopAppDbEntry1.neighbor_id);
    ASSERT_TRUE(p4_oid_mapper_.setDummyOID(SAI_OBJECT_TYPE_NEIGHBOR_ENTRY, neighbor_key));

    EXPECT_EQ(StatusCode::SWSS_RC_NOT_FOUND, ProcessAddRequest(kP4NextHopAppDbEntry1));

    EXPECT_EQ(GetNextHopEntry(KeyGenerator::generateNextHopKey(kP4NextHopAppDbEntry1.next_hop_id)), nullptr);
}

TEST_F(NextHopManagerTest, ProcessAddRequestShouldFailWhenDependingNeigherIsAbsentInCentralMapper)
{
    const std::string rif_key = KeyGenerator::generateRouterInterfaceKey(kP4NextHopAppDbEntry1.router_interface_id);
    ASSERT_TRUE(p4_oid_mapper_.setOID(SAI_OBJECT_TYPE_ROUTER_INTERFACE, rif_key, kRouterInterfaceOid1));

    EXPECT_EQ(StatusCode::SWSS_RC_NOT_FOUND, ProcessAddRequest(kP4NextHopAppDbEntry1));

    EXPECT_EQ(GetNextHopEntry(KeyGenerator::generateNextHopKey(kP4NextHopAppDbEntry1.next_hop_id)), nullptr);
}

TEST_F(NextHopManagerTest, ProcessAddRequestShouldFailWhenSaiCallFails)
{
    ASSERT_TRUE(ResolveNextHopEntryDependency(kP4NextHopAppDbEntry1, kRouterInterfaceOid1));

    // Set up mock call.
    EXPECT_CALL(mock_sai_next_hop_,
                create_next_hop(
                    ::testing::NotNull(), Eq(gSwitchId), Eq(3),
                    Truly(std::bind(MatchCreateNextHopArgAttrList, std::placeholders::_1,
                                    CreateAttributeListForNextHopObject(kP4NextHopAppDbEntry1, kRouterInterfaceOid1)))))
        .WillOnce(Return(SAI_STATUS_FAILURE));

    EXPECT_EQ(StatusCode::SWSS_RC_UNKNOWN, ProcessAddRequest(kP4NextHopAppDbEntry1));

    // The add request failed for the next hop entry.
    EXPECT_EQ(GetNextHopEntry(KeyGenerator::generateNextHopKey(kP4NextHopAppDbEntry1.next_hop_id)), nullptr);
}

TEST_F(NextHopManagerTest, ProcessAddRequestShouldDoNoOpForDuplicateAddRequest)
{
    ASSERT_NE(AddNextHopEntry1(), nullptr);

    // Add the same next hop entry again.
    EXPECT_EQ(StatusCode::SWSS_RC_EXISTS, ProcessAddRequest(kP4NextHopAppDbEntry1));

    // Adding the same next hop entry multiple times should have the same outcome
    // as adding it once.
    EXPECT_TRUE(ValidateNextHopEntryAdd(kP4NextHopAppDbEntry1, kNextHopOid));
    const std::string rif_key = KeyGenerator::generateRouterInterfaceKey(kP4NextHopAppDbEntry1.router_interface_id);
    const std::string neighbor_key =
        KeyGenerator::generateNeighborKey(kP4NextHopAppDbEntry1.router_interface_id, kP4NextHopAppDbEntry1.neighbor_id);
    EXPECT_TRUE(ValidateRefCnt(SAI_OBJECT_TYPE_ROUTER_INTERFACE, rif_key, 1));
    EXPECT_TRUE(ValidateRefCnt(SAI_OBJECT_TYPE_NEIGHBOR_ENTRY, neighbor_key, 1));
}

TEST_F(NextHopManagerTest, ProcessUpdateRequestShouldFailAsItIsUnsupported)
{
    auto *p4_next_hop_entry = AddNextHopEntry1();
    ASSERT_NE(p4_next_hop_entry, nullptr);

    EXPECT_EQ(StatusCode::SWSS_RC_UNIMPLEMENTED, ProcessUpdateRequest(kP4NextHopAppDbEntry2, p4_next_hop_entry));

    // Expect that the update call will fail, so next hop entry's fields stay the
    // same.
    EXPECT_TRUE(ValidateNextHopEntryAdd(kP4NextHopAppDbEntry1, kNextHopOid));

    // Validate ref count stay the same.
    const std::string rif_key = KeyGenerator::generateRouterInterfaceKey(kP4NextHopAppDbEntry1.router_interface_id);
    const std::string neighbor_key =
        KeyGenerator::generateNeighborKey(kP4NextHopAppDbEntry1.router_interface_id, kP4NextHopAppDbEntry1.neighbor_id);
    EXPECT_TRUE(ValidateRefCnt(SAI_OBJECT_TYPE_ROUTER_INTERFACE, rif_key, 1));
    EXPECT_TRUE(ValidateRefCnt(SAI_OBJECT_TYPE_NEIGHBOR_ENTRY, neighbor_key, 1));
}

TEST_F(NextHopManagerTest, ProcessDeleteRequestShouldSucceedForExistingNextHop)
{
    auto *p4_next_hop_entry = AddNextHopEntry1();
    ASSERT_NE(p4_next_hop_entry, nullptr);

    // Set up mock call.
    EXPECT_CALL(mock_sai_next_hop_, remove_next_hop(Eq(p4_next_hop_entry->next_hop_oid)))
        .WillOnce(Return(SAI_STATUS_SUCCESS));

    EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS, ProcessDeleteRequest(p4_next_hop_entry->next_hop_key));

    // Validate the next hop entry has been deleted in both P4 next hop manager
    // and centralized mapper.
    p4_next_hop_entry = GetNextHopEntry(KeyGenerator::generateNextHopKey(kP4NextHopAppDbEntry1.next_hop_id));
    EXPECT_EQ(p4_next_hop_entry, nullptr);
    EXPECT_FALSE(p4_oid_mapper_.existsOID(SAI_OBJECT_TYPE_NEXT_HOP,
                                          KeyGenerator::generateNextHopKey(kP4NextHopAppDbEntry1.next_hop_id)));

    // Validate ref count decrement.
    const std::string rif_key = KeyGenerator::generateRouterInterfaceKey(kP4NextHopAppDbEntry1.router_interface_id);
    const std::string neighbor_key =
        KeyGenerator::generateNeighborKey(kP4NextHopAppDbEntry1.router_interface_id, kP4NextHopAppDbEntry1.neighbor_id);
    EXPECT_TRUE(ValidateRefCnt(SAI_OBJECT_TYPE_ROUTER_INTERFACE, rif_key, 0));
    EXPECT_TRUE(ValidateRefCnt(SAI_OBJECT_TYPE_NEIGHBOR_ENTRY, neighbor_key, 0));
}

TEST_F(NextHopManagerTest, ProcessDeleteRequestShouldFailForNonExistingNextHop)
{
    EXPECT_EQ(StatusCode::SWSS_RC_NOT_FOUND,
              ProcessDeleteRequest(KeyGenerator::generateNextHopKey(kP4NextHopAppDbEntry1.next_hop_id)));
}

TEST_F(NextHopManagerTest, ProcessDeleteRequestShouldFailIfNextHopEntryIsAbsentInCentralMapper)
{
    auto *p4_next_hop_entry = AddNextHopEntry1();
    ASSERT_NE(p4_next_hop_entry, nullptr);

    ASSERT_TRUE(p4_oid_mapper_.eraseOID(SAI_OBJECT_TYPE_NEXT_HOP, p4_next_hop_entry->next_hop_key));

    // (TODO): Expect critical state.
    EXPECT_EQ(StatusCode::SWSS_RC_INTERNAL, ProcessDeleteRequest(p4_next_hop_entry->next_hop_key));

    // Validate the next hop entry is not deleted in P4 next hop manager.
    p4_next_hop_entry = GetNextHopEntry(KeyGenerator::generateNextHopKey(kP4NextHopAppDbEntry1.next_hop_id));
    ASSERT_NE(p4_next_hop_entry, nullptr);

    // Validate ref count remains the same.
    const std::string rif_key = KeyGenerator::generateRouterInterfaceKey(kP4NextHopAppDbEntry1.router_interface_id);
    const std::string neighbor_key =
        KeyGenerator::generateNeighborKey(kP4NextHopAppDbEntry1.router_interface_id, kP4NextHopAppDbEntry1.neighbor_id);
    EXPECT_TRUE(ValidateRefCnt(SAI_OBJECT_TYPE_ROUTER_INTERFACE, rif_key, 1));
    EXPECT_TRUE(ValidateRefCnt(SAI_OBJECT_TYPE_NEIGHBOR_ENTRY, neighbor_key, 1));
}

TEST_F(NextHopManagerTest, ProcessDeleteRequestShouldFailIfNextHopEntryIsStillReferenced)
{
    auto *p4_next_hop_entry = AddNextHopEntry1();
    ASSERT_NE(p4_next_hop_entry, nullptr);

    ASSERT_TRUE(p4_oid_mapper_.increaseRefCount(SAI_OBJECT_TYPE_NEXT_HOP, p4_next_hop_entry->next_hop_key));

    EXPECT_EQ(StatusCode::SWSS_RC_INVALID_PARAM, ProcessDeleteRequest(p4_next_hop_entry->next_hop_key));

    // Validate the next hop entry is not deleted in either P4 next hop manager or
    // central mapper.
    p4_next_hop_entry = GetNextHopEntry(KeyGenerator::generateNextHopKey(kP4NextHopAppDbEntry1.next_hop_id));
    ASSERT_NE(p4_next_hop_entry, nullptr);
    EXPECT_TRUE(ValidateRefCnt(SAI_OBJECT_TYPE_NEXT_HOP, p4_next_hop_entry->next_hop_key, 1));

    // Validate ref count remains the same.
    const std::string rif_key = KeyGenerator::generateRouterInterfaceKey(kP4NextHopAppDbEntry1.router_interface_id);
    const std::string neighbor_key =
        KeyGenerator::generateNeighborKey(kP4NextHopAppDbEntry1.router_interface_id, kP4NextHopAppDbEntry1.neighbor_id);
    EXPECT_TRUE(ValidateRefCnt(SAI_OBJECT_TYPE_ROUTER_INTERFACE, rif_key, 1));
    EXPECT_TRUE(ValidateRefCnt(SAI_OBJECT_TYPE_NEIGHBOR_ENTRY, neighbor_key, 1));
}

TEST_F(NextHopManagerTest, ProcessDeleteRequestShouldFailIfSaiCallFails)
{
    auto *p4_next_hop_entry = AddNextHopEntry1();
    ASSERT_NE(p4_next_hop_entry, nullptr);

    // Set up mock call.
    EXPECT_CALL(mock_sai_next_hop_, remove_next_hop(Eq(p4_next_hop_entry->next_hop_oid)))
        .WillOnce(Return(SAI_STATUS_FAILURE));

    EXPECT_EQ(StatusCode::SWSS_RC_UNKNOWN, ProcessDeleteRequest(p4_next_hop_entry->next_hop_key));

    // Validate the next hop entry is not deleted in either P4 next hop manager or
    // central mapper.
    p4_next_hop_entry = GetNextHopEntry(KeyGenerator::generateNextHopKey(kP4NextHopAppDbEntry1.next_hop_id));
    ASSERT_NE(p4_next_hop_entry, nullptr);
    EXPECT_TRUE(p4_oid_mapper_.existsOID(SAI_OBJECT_TYPE_NEXT_HOP,
                                         KeyGenerator::generateNextHopKey(kP4NextHopAppDbEntry1.next_hop_id)));

    // Validate ref count remains the same.
    const std::string rif_key = KeyGenerator::generateRouterInterfaceKey(kP4NextHopAppDbEntry1.router_interface_id);
    const std::string neighbor_key =
        KeyGenerator::generateNeighborKey(kP4NextHopAppDbEntry1.router_interface_id, kP4NextHopAppDbEntry1.neighbor_id);
    EXPECT_TRUE(ValidateRefCnt(SAI_OBJECT_TYPE_ROUTER_INTERFACE, rif_key, 1));
    EXPECT_TRUE(ValidateRefCnt(SAI_OBJECT_TYPE_NEIGHBOR_ENTRY, neighbor_key, 1));
}

TEST_F(NextHopManagerTest, GetNextHopEntryShouldReturnValidPointerForAddedNextHop)
{
    ASSERT_TRUE(ResolveNextHopEntryDependency(kP4NextHopAppDbEntry1, kRouterInterfaceOid1));

    // Set up mock call.
    EXPECT_CALL(mock_sai_next_hop_,
                create_next_hop(
                    ::testing::NotNull(), Eq(gSwitchId), Eq(3),
                    Truly(std::bind(MatchCreateNextHopArgAttrList, std::placeholders::_1,
                                    CreateAttributeListForNextHopObject(kP4NextHopAppDbEntry1, kRouterInterfaceOid1)))))
        .WillOnce(DoAll(SetArgPointee<0>(kNextHopOid), Return(SAI_STATUS_SUCCESS)));

    EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS, ProcessAddRequest(kP4NextHopAppDbEntry1));

    EXPECT_NE(GetNextHopEntry(KeyGenerator::generateNextHopKey(kP4NextHopAppDbEntry1.next_hop_id)), nullptr);
}

TEST_F(NextHopManagerTest, GetNextHopEntryShouldReturnNullPointerForNonexistingNextHop)
{
    EXPECT_EQ(GetNextHopEntry(KeyGenerator::generateNextHopKey(kP4NextHopAppDbEntry1.next_hop_id)), nullptr);
}

TEST_F(NextHopManagerTest, DeserializeP4NextHopAppDbEntryShouldSucceedForValidNextHopSetEntry)
{
    std::vector<swss::FieldValueTuple> attributes = {
        swss::FieldValueTuple(p4orch::kAction, "set_nexthop"),
        swss::FieldValueTuple(prependParamField(p4orch::kRouterInterfaceId), kRouterInterfaceId1),
        swss::FieldValueTuple(prependParamField(p4orch::kNeighborId), kNeighborId1)};

    auto app_db_entry_or = DeserializeP4NextHopAppDbEntry(kNextHopP4AppDbKey, attributes);
    EXPECT_TRUE(app_db_entry_or.ok());
    auto &app_db_entry = *app_db_entry_or;
    EXPECT_EQ(app_db_entry.next_hop_id, kNextHopId);
    EXPECT_TRUE(app_db_entry.is_set_router_interface_id);
    EXPECT_EQ(app_db_entry.router_interface_id, kRouterInterfaceId1);
    EXPECT_TRUE(app_db_entry.is_set_neighbor_id);
    EXPECT_EQ(app_db_entry.neighbor_id, swss::IpAddress(kNeighborId1));
}

TEST_F(NextHopManagerTest, DeserializeP4NextHopAppDbEntryShouldSucceedForValidNextHopDeleteEntry)
{
    auto app_db_entry_or = DeserializeP4NextHopAppDbEntry(kNextHopP4AppDbKey, std::vector<swss::FieldValueTuple>());
    EXPECT_TRUE(app_db_entry_or.ok());
    auto &app_db_entry = *app_db_entry_or;
    EXPECT_EQ(app_db_entry.next_hop_id, kNextHopId);
    EXPECT_FALSE(app_db_entry.is_set_router_interface_id);
    EXPECT_FALSE(app_db_entry.is_set_neighbor_id);
}

TEST_F(NextHopManagerTest, DeserializeP4NextHopAppDbEntryShouldReturnNullPointerWhenFailToDeserializeNextHopId)
{
    // Incorrect format of P4 App next hop entry key
    std::string key = R"({"nexthop":"8"})";
    std::vector<swss::FieldValueTuple> attributes;

    EXPECT_FALSE(DeserializeP4NextHopAppDbEntry(key, attributes).ok());
}

TEST_F(NextHopManagerTest, DeserializeP4NextHopAppDbEntryShouldReturnNullPointerForInvalidIpAddr)
{
    std::vector<swss::FieldValueTuple> attributes = {
        swss::FieldValueTuple(p4orch::kAction, "set_nexthop"),
        swss::FieldValueTuple(prependParamField(p4orch::kRouterInterfaceId), kRouterInterfaceId1),
        swss::FieldValueTuple(prependParamField(p4orch::kNeighborId), "0.0.0.0.0.0")}; // Invalid IP address.

    EXPECT_FALSE(DeserializeP4NextHopAppDbEntry(kNextHopP4AppDbKey, attributes).ok());
}

TEST_F(NextHopManagerTest, DeserializeP4NextHopAppDbEntryShouldReturnNullPointerDueToUnexpectedField)
{
    std::vector<swss::FieldValueTuple> attributes = {
        swss::FieldValueTuple(p4orch::kAction, "set_nexthop"),
        swss::FieldValueTuple(p4orch::kRouterInterfaceId, kRouterInterfaceId1),
        swss::FieldValueTuple("unexpected_field", "unexpected_value")};

    EXPECT_FALSE(DeserializeP4NextHopAppDbEntry(kNextHopP4AppDbKey, attributes).ok());
}

TEST_F(NextHopManagerTest, DrainValidAppEntryShouldSucceed)
{
    nlohmann::json j;
    j[prependMatchField(p4orch::kNexthopId)] = kNextHopId;

    std::vector<swss::FieldValueTuple> fvs{{prependParamField(p4orch::kNeighborId), kNeighborId2},
                                           {prependParamField(p4orch::kRouterInterfaceId), kRouterInterfaceId2}};

    swss::KeyOpFieldsValuesTuple app_db_entry(std::string(APP_P4RT_NEXTHOP_TABLE_NAME) + kTableKeyDelimiter + j.dump(),
                                              SET_COMMAND, fvs);

    Enqueue(app_db_entry);

    EXPECT_TRUE(ResolveNextHopEntryDependency(kP4NextHopAppDbEntry2, kRouterInterfaceOid2));
    EXPECT_CALL(mock_sai_next_hop_, create_next_hop(_, _, _, _))
        .WillOnce(DoAll(SetArgPointee<0>(kNextHopOid), Return(SAI_STATUS_SUCCESS)));

    Drain();

    EXPECT_TRUE(ValidateNextHopEntryAdd(kP4NextHopAppDbEntry2, kNextHopOid));
}

TEST_F(NextHopManagerTest, DrainAppEntryWithInvalidOpShouldBeNoOp)
{
    nlohmann::json j;
    j[prependMatchField(p4orch::kNexthopId)] = kNextHopId;

    std::vector<swss::FieldValueTuple> fvs{{prependParamField(p4orch::kNeighborId), kNeighborId2},
                                           {prependParamField(p4orch::kRouterInterfaceId), kRouterInterfaceId2}};

    swss::KeyOpFieldsValuesTuple app_db_entry(std::string(APP_P4RT_NEXTHOP_TABLE_NAME) + kTableKeyDelimiter + j.dump(),
                                              "INVALID_OP", fvs);

    Enqueue(app_db_entry);

    EXPECT_TRUE(ResolveNextHopEntryDependency(kP4NextHopAppDbEntry2, kRouterInterfaceOid2));

    Drain();

    EXPECT_FALSE(ValidateNextHopEntryAdd(kP4NextHopAppDbEntry2, kNextHopOid));
}

TEST_F(NextHopManagerTest, DrainAppEntryWithInvalidFieldShouldBeNoOp)
{
    nlohmann::json j;
    j[prependMatchField(p4orch::kNexthopId)] = kNextHopId;

    std::vector<swss::FieldValueTuple> fvs{{prependParamField(p4orch::kNeighborId), kNeighborId2},
                                           {prependParamField(p4orch::kRouterInterfaceId), kRouterInterfaceId2},
                                           {"unexpected_field", "unexpected_value"}};

    swss::KeyOpFieldsValuesTuple app_db_entry(std::string(APP_P4RT_NEXTHOP_TABLE_NAME) + kTableKeyDelimiter + j.dump(),
                                              "INVALID_OP", fvs);

    Enqueue(app_db_entry);

    EXPECT_TRUE(ResolveNextHopEntryDependency(kP4NextHopAppDbEntry2, kRouterInterfaceOid2));

    Drain();

    EXPECT_FALSE(ValidateNextHopEntryAdd(kP4NextHopAppDbEntry2, kNextHopOid));
}

TEST_F(NextHopManagerTest, DrainUpdateRequestShouldBeUnsupported)
{
    auto *p4_next_hop_entry = AddNextHopEntry1();
    ASSERT_NE(p4_next_hop_entry, nullptr);

    nlohmann::json j;
    j[prependMatchField(p4orch::kNexthopId)] = kNextHopId;
    std::vector<swss::FieldValueTuple> fvs{{prependParamField(p4orch::kNeighborId), kNeighborId2},
                                           {prependParamField(p4orch::kRouterInterfaceId), kRouterInterfaceId2}};
    swss::KeyOpFieldsValuesTuple app_db_entry(std::string(APP_P4RT_NEXTHOP_TABLE_NAME) + kTableKeyDelimiter + j.dump(),
                                              SET_COMMAND, fvs);

    Enqueue(app_db_entry);
    EXPECT_TRUE(ResolveNextHopEntryDependency(kP4NextHopAppDbEntry2, kRouterInterfaceOid2));
    Drain();

    // Expect that the update call will fail, so next hop entry's fields stay the
    // same.
    EXPECT_TRUE(ValidateNextHopEntryAdd(kP4NextHopAppDbEntry1, kNextHopOid));

    // Validate ref count stay the same.
    const std::string rif_key = KeyGenerator::generateRouterInterfaceKey(kP4NextHopAppDbEntry1.router_interface_id);
    const std::string neighbor_key =
        KeyGenerator::generateNeighborKey(kP4NextHopAppDbEntry1.router_interface_id, kP4NextHopAppDbEntry1.neighbor_id);
    EXPECT_TRUE(ValidateRefCnt(SAI_OBJECT_TYPE_ROUTER_INTERFACE, rif_key, 1));
    EXPECT_TRUE(ValidateRefCnt(SAI_OBJECT_TYPE_NEIGHBOR_ENTRY, neighbor_key, 1));
}

TEST_F(NextHopManagerTest, DrainDeleteRequestShouldSucceedForExistingNextHop)
{
    auto *p4_next_hop_entry = AddNextHopEntry1();
    ASSERT_NE(p4_next_hop_entry, nullptr);

    nlohmann::json j;
    j[prependMatchField(p4orch::kNexthopId)] = kNextHopId;
    std::vector<swss::FieldValueTuple> fvs;
    swss::KeyOpFieldsValuesTuple app_db_entry(std::string(APP_P4RT_NEXTHOP_TABLE_NAME) + kTableKeyDelimiter + j.dump(),
                                              DEL_COMMAND, fvs);
    EXPECT_CALL(mock_sai_next_hop_, remove_next_hop(Eq(p4_next_hop_entry->next_hop_oid)))
        .WillOnce(Return(SAI_STATUS_SUCCESS));

    Enqueue(app_db_entry);
    Drain();

    // Validate the next hop entry has been deleted in both P4 next hop manager
    // and centralized mapper.
    p4_next_hop_entry = GetNextHopEntry(KeyGenerator::generateNextHopKey(kP4NextHopAppDbEntry1.next_hop_id));
    EXPECT_EQ(p4_next_hop_entry, nullptr);
    EXPECT_FALSE(p4_oid_mapper_.existsOID(SAI_OBJECT_TYPE_NEXT_HOP,
                                          KeyGenerator::generateNextHopKey(kP4NextHopAppDbEntry1.next_hop_id)));

    // Validate ref count decrement.
    const std::string rif_key = KeyGenerator::generateRouterInterfaceKey(kP4NextHopAppDbEntry1.router_interface_id);
    const std::string neighbor_key =
        KeyGenerator::generateNeighborKey(kP4NextHopAppDbEntry1.router_interface_id, kP4NextHopAppDbEntry1.neighbor_id);
    EXPECT_TRUE(ValidateRefCnt(SAI_OBJECT_TYPE_ROUTER_INTERFACE, rif_key, 0));
    EXPECT_TRUE(ValidateRefCnt(SAI_OBJECT_TYPE_NEIGHBOR_ENTRY, neighbor_key, 0));
}
