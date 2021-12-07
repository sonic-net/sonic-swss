#include "neighbor_manager.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <string>
#include <unordered_set>

#include "json.hpp"
#include "mock_response_publisher.h"
#include "mock_sai_neighbor.h"
#include "p4orch.h"
#include "p4orch/p4orch_util.h"
#include "return_code.h"
#include "swssnet.h"

using ::p4orch::kTableKeyDelimiter;

using ::testing::_;
using ::testing::Eq;
using ::testing::Return;
using ::testing::StrictMock;
using ::testing::Truly;

extern sai_object_id_t gSwitchId;
extern sai_neighbor_api_t *sai_neighbor_api;

namespace
{

constexpr char *kRouterInterfaceId1 = "intf-3/4";
constexpr sai_object_id_t kRouterInterfaceOid1 = 0x295100;

constexpr char *kRouterInterfaceId2 = "Ethernet20";
constexpr sai_object_id_t kRouterInterfaceOid2 = 0x51411;

const swss::IpAddress kNeighborId1("10.0.0.22");
const swss::MacAddress kMacAddress1("00:01:02:03:04:05");

const swss::IpAddress kNeighborId2("fe80::21a:11ff:fe17:5f80");
const swss::MacAddress kMacAddress2("00:ff:ee:dd:cc:bb");

bool MatchNeighborEntry(const sai_neighbor_entry_t *neigh_entry, const sai_neighbor_entry_t &expected_neigh_entry)
{
    if (neigh_entry == nullptr)
        return false;

    if ((neigh_entry->switch_id != expected_neigh_entry.switch_id) ||
        (neigh_entry->rif_id != expected_neigh_entry.rif_id) ||
        (neigh_entry->ip_address.addr_family != expected_neigh_entry.ip_address.addr_family))
        return false;

    if ((neigh_entry->ip_address.addr_family == SAI_IP_ADDR_FAMILY_IPV4) &&
        (neigh_entry->ip_address.addr.ip4 != expected_neigh_entry.ip_address.addr.ip4))
    {
        return false;
    }
    else if ((neigh_entry->ip_address.addr_family == SAI_IP_ADDR_FAMILY_IPV6) &&
             (memcmp(neigh_entry->ip_address.addr.ip6, expected_neigh_entry.ip_address.addr.ip6, 16)))
    {
        return false;
    }

    return true;
}

bool MatchNeighborCreateAttributeList(const sai_attribute_t *attr_list, const swss::MacAddress &dst_mac_address)
{
    if (attr_list == nullptr)
        return false;

    std::unordered_set<sai_attr_id_t> attrs;

    for (int i = 0; i < 2; ++i)
    {
        if (attrs.count(attr_list[i].id) != 0)
        {
            // Repeated attribute.
            return false;
        }
        switch (attr_list[i].id)
        {
        case SAI_NEIGHBOR_ENTRY_ATTR_DST_MAC_ADDRESS:
            if (memcmp(attr_list[i].value.mac, dst_mac_address.getMac(), sizeof(sai_mac_t)) != 0)
            {
                return false;
            }
            break;
        case SAI_NEIGHBOR_ENTRY_ATTR_NO_HOST_ROUTE:
            if (!attr_list[i].value.booldata)
            {
                return false;
            }
            break;
        default:
            return false;
        }
        attrs.insert(attr_list[i].id);
    }

    return true;
}

bool MatchNeighborSetAttributeList(const sai_attribute_t *attr_list, const swss::MacAddress &dst_mac_address)
{
    if (attr_list == nullptr)
        return false;

    return (attr_list[0].id == SAI_NEIGHBOR_ENTRY_ATTR_DST_MAC_ADDRESS &&
            memcmp(attr_list[0].value.mac, dst_mac_address.getMac(), sizeof(sai_mac_t)) == 0);
}

} // namespace

class NeighborManagerTest : public ::testing::Test
{
  protected:
    NeighborManagerTest() : neighbor_manager_(&p4_oid_mapper_, &publisher_)
    {
    }

    void SetUp() override
    {
        mock_sai_neighbor = &mock_sai_neighbor_;
        sai_neighbor_api->create_neighbor_entry = mock_create_neighbor_entry;
        sai_neighbor_api->remove_neighbor_entry = mock_remove_neighbor_entry;
        sai_neighbor_api->set_neighbor_entry_attribute = mock_set_neighbor_entry_attribute;
        sai_neighbor_api->get_neighbor_entry_attribute = mock_get_neighbor_entry_attribute;
    }

    void Enqueue(const swss::KeyOpFieldsValuesTuple &entry)
    {
        neighbor_manager_.enqueue(entry);
    }

    void Drain()
    {
        neighbor_manager_.drain();
    }

    ReturnCodeOr<P4NeighborAppDbEntry> DeserializeNeighborEntry(const std::string &key,
                                                                const std::vector<swss::FieldValueTuple> &attributes)
    {
        return neighbor_manager_.deserializeNeighborEntry(key, attributes);
    }

    ReturnCode ValidateNeighborAppDbEntry(const P4NeighborAppDbEntry &app_db_entry)
    {
        return neighbor_manager_.validateNeighborAppDbEntry(app_db_entry);
    }

    ReturnCode CreateNeighbor(P4NeighborEntry &neighbor_entry)
    {
        return neighbor_manager_.createNeighbor(neighbor_entry);
    }

    ReturnCode RemoveNeighbor(const std::string &neighbor_key)
    {
        return neighbor_manager_.removeNeighbor(neighbor_key);
    }

    ReturnCode SetDstMacAddress(P4NeighborEntry *neighbor_entry, const swss::MacAddress &mac_address)
    {
        return neighbor_manager_.setDstMacAddress(neighbor_entry, mac_address);
    }

    ReturnCode ProcessAddRequest(const P4NeighborAppDbEntry &app_db_entry, const std::string &neighbor_key)
    {
        return neighbor_manager_.processAddRequest(app_db_entry, neighbor_key);
    }

    ReturnCode ProcessUpdateRequest(const P4NeighborAppDbEntry &app_db_entry, P4NeighborEntry *neighbor_entry)
    {
        return neighbor_manager_.processUpdateRequest(app_db_entry, neighbor_entry);
    }

    ReturnCode ProcessDeleteRequest(const std::string &neighbor_key)
    {
        return neighbor_manager_.processDeleteRequest(neighbor_key);
    }

    P4NeighborEntry *GetNeighborEntry(const std::string &neighbor_key)
    {
        return neighbor_manager_.getNeighborEntry(neighbor_key);
    }

    void ValidateNeighborEntry(const P4NeighborEntry &expected_entry, const uint32_t router_intf_ref_count)
    {
        auto neighbor_entry = GetNeighborEntry(expected_entry.neighbor_key);

        EXPECT_NE(nullptr, neighbor_entry);
        EXPECT_EQ(expected_entry.router_intf_id, neighbor_entry->router_intf_id);
        EXPECT_EQ(expected_entry.neighbor_id, neighbor_entry->neighbor_id);
        EXPECT_EQ(expected_entry.dst_mac_address, neighbor_entry->dst_mac_address);
        EXPECT_EQ(expected_entry.router_intf_key, neighbor_entry->router_intf_key);
        EXPECT_EQ(expected_entry.neighbor_key, neighbor_entry->neighbor_key);

        EXPECT_TRUE(MatchNeighborEntry(&neighbor_entry->neigh_entry, expected_entry.neigh_entry));

        EXPECT_TRUE(p4_oid_mapper_.existsOID(SAI_OBJECT_TYPE_NEIGHBOR_ENTRY, expected_entry.neighbor_key));

        uint32_t ref_count;
        ASSERT_TRUE(
            p4_oid_mapper_.getRefCount(SAI_OBJECT_TYPE_ROUTER_INTERFACE, expected_entry.router_intf_key, &ref_count));
        EXPECT_EQ(router_intf_ref_count, ref_count);
    }

    void ValidateNeighborEntryNotPresent(const P4NeighborEntry &neighbor_entry, bool check_ref_count,
                                         const uint32_t router_intf_ref_count = 0)
    {
        auto current_entry = GetNeighborEntry(neighbor_entry.neighbor_key);
        EXPECT_EQ(current_entry, nullptr);
        EXPECT_FALSE(p4_oid_mapper_.existsOID(SAI_OBJECT_TYPE_NEIGHBOR_ENTRY, neighbor_entry.neighbor_key));

        if (check_ref_count)
        {
            uint32_t ref_count;
            ASSERT_TRUE(p4_oid_mapper_.getRefCount(SAI_OBJECT_TYPE_ROUTER_INTERFACE, neighbor_entry.router_intf_key,
                                                   &ref_count));
            EXPECT_EQ(ref_count, router_intf_ref_count);
        }
    }

    void AddNeighborEntry(P4NeighborEntry &neighbor_entry, const sai_object_id_t router_intf_oid)
    {
        sai_neighbor_entry_t neigh_entry;
        neigh_entry.switch_id = gSwitchId;
        copy(neigh_entry.ip_address, neighbor_entry.neighbor_id);
        neigh_entry.rif_id = router_intf_oid;

        EXPECT_CALL(mock_sai_neighbor_,
                    create_neighbor_entry(Truly(std::bind(MatchNeighborEntry, std::placeholders::_1, neigh_entry)),
                                          Eq(2),
                                          Truly(std::bind(MatchNeighborCreateAttributeList, std::placeholders::_1,
                                                          neighbor_entry.dst_mac_address))))
            .WillOnce(Return(SAI_STATUS_SUCCESS));

        ASSERT_TRUE(
            p4_oid_mapper_.setOID(SAI_OBJECT_TYPE_ROUTER_INTERFACE, neighbor_entry.router_intf_key, router_intf_oid));
        EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS, CreateNeighbor(neighbor_entry));
    }

    std::string CreateNeighborAppDbKey(const std::string router_interface_id, const swss::IpAddress neighbor_id)
    {
        nlohmann::json j;
        j[prependMatchField(p4orch::kRouterInterfaceId)] = router_interface_id;
        j[prependMatchField(p4orch::kNeighborId)] = neighbor_id.to_string();
        return j.dump();
    }

    StrictMock<MockSaiNeighbor> mock_sai_neighbor_;
    MockResponsePublisher publisher_;
    P4OidMapper p4_oid_mapper_;
    NeighborManager neighbor_manager_;
};

TEST_F(NeighborManagerTest, CreateNeighborValidAttributes)
{
    P4NeighborEntry neighbor_entry(kRouterInterfaceId1, kNeighborId1, kMacAddress1);
    AddNeighborEntry(neighbor_entry, kRouterInterfaceOid1);

    ValidateNeighborEntry(neighbor_entry, /*router_intf_ref_count=*/1);
}

TEST_F(NeighborManagerTest, CreateNeighborEntryExistsInManager)
{
    P4NeighborEntry neighbor_entry(kRouterInterfaceId1, kNeighborId1, kMacAddress1);
    AddNeighborEntry(neighbor_entry, kRouterInterfaceOid1);

    // Same neighbor key with different destination mac address.
    P4NeighborEntry new_entry(kRouterInterfaceId1, kNeighborId1, kMacAddress2);
    EXPECT_EQ(StatusCode::SWSS_RC_EXISTS, CreateNeighbor(new_entry));

    // Validate that entry in Manager has not changed.
    ValidateNeighborEntry(neighbor_entry, /*router_intf_ref_count=*/1);
}

TEST_F(NeighborManagerTest, CreateNeighborEntryExistsInP4OidMapper)
{
    P4NeighborEntry neighbor_entry(kRouterInterfaceId2, kNeighborId2, kMacAddress2);
    p4_oid_mapper_.setDummyOID(SAI_OBJECT_TYPE_NEIGHBOR_ENTRY, neighbor_entry.neighbor_key);

    // (TODO): Expect critical state.
    EXPECT_EQ(StatusCode::SWSS_RC_INTERNAL, CreateNeighbor(neighbor_entry));

    auto current_entry = GetNeighborEntry(neighbor_entry.neighbor_key);
    EXPECT_EQ(current_entry, nullptr);

    // Validate that dummyOID still exists in Centralized Mapper.
    EXPECT_TRUE(p4_oid_mapper_.existsOID(SAI_OBJECT_TYPE_NEIGHBOR_ENTRY, neighbor_entry.neighbor_key));
}

TEST_F(NeighborManagerTest, CreateNeighborNonExistentRouterIntf)
{
    P4NeighborEntry neighbor_entry(kRouterInterfaceId2, kNeighborId2, kMacAddress2);
    EXPECT_EQ(StatusCode::SWSS_RC_NOT_FOUND, CreateNeighbor(neighbor_entry));

    ValidateNeighborEntryNotPresent(neighbor_entry, /*check_ref_count=*/false);
}

TEST_F(NeighborManagerTest, CreateNeighborSaiApiFails)
{
    P4NeighborEntry neighbor_entry(kRouterInterfaceId1, kNeighborId1, kMacAddress1);

    ASSERT_TRUE(
        p4_oid_mapper_.setOID(SAI_OBJECT_TYPE_ROUTER_INTERFACE, neighbor_entry.router_intf_key, kRouterInterfaceOid1));
    EXPECT_CALL(mock_sai_neighbor_, create_neighbor_entry(_, _, _)).WillOnce(Return(SAI_STATUS_FAILURE));

    EXPECT_EQ(StatusCode::SWSS_RC_UNKNOWN, CreateNeighbor(neighbor_entry));

    ValidateNeighborEntryNotPresent(neighbor_entry, /*check_ref_count=*/true);
}

TEST_F(NeighborManagerTest, RemoveNeighborExistingNeighborEntry)
{
    P4NeighborEntry neighbor_entry(kRouterInterfaceId2, kNeighborId2, kMacAddress2);
    AddNeighborEntry(neighbor_entry, kRouterInterfaceOid2);

    sai_neighbor_entry_t neigh_entry;
    neigh_entry.switch_id = gSwitchId;
    copy(neigh_entry.ip_address, neighbor_entry.neighbor_id);
    neigh_entry.rif_id = kRouterInterfaceOid2;

    EXPECT_CALL(mock_sai_neighbor_,
                remove_neighbor_entry(Truly(std::bind(MatchNeighborEntry, std::placeholders::_1, neigh_entry))))
        .WillOnce(Return(SAI_STATUS_SUCCESS));

    EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS, RemoveNeighbor(neighbor_entry.neighbor_key));

    ValidateNeighborEntryNotPresent(neighbor_entry, /*check_ref_count=*/true);
}

TEST_F(NeighborManagerTest, RemoveNeighborNonExistingNeighborEntry)
{
    P4NeighborEntry neighbor_entry(kRouterInterfaceId2, kNeighborId2, kMacAddress2);
    EXPECT_EQ(StatusCode::SWSS_RC_NOT_FOUND, RemoveNeighbor(neighbor_entry.neighbor_key));
}

TEST_F(NeighborManagerTest, RemoveNeighborNotExistInMapper)
{
    P4NeighborEntry neighbor_entry(kRouterInterfaceId2, kNeighborId2, kMacAddress2);
    AddNeighborEntry(neighbor_entry, kRouterInterfaceOid2);

    ASSERT_TRUE(p4_oid_mapper_.eraseOID(SAI_OBJECT_TYPE_NEIGHBOR_ENTRY, neighbor_entry.neighbor_key));
    // (TODO): Expect critical state.
    EXPECT_EQ(StatusCode::SWSS_RC_INTERNAL, RemoveNeighbor(neighbor_entry.neighbor_key));
}

TEST_F(NeighborManagerTest, RemoveNeighborNonZeroRefCount)
{
    P4NeighborEntry neighbor_entry(kRouterInterfaceId2, kNeighborId2, kMacAddress2);
    AddNeighborEntry(neighbor_entry, kRouterInterfaceOid2);

    ASSERT_TRUE(p4_oid_mapper_.increaseRefCount(SAI_OBJECT_TYPE_NEIGHBOR_ENTRY, neighbor_entry.neighbor_key));
    EXPECT_EQ(StatusCode::SWSS_RC_INVALID_PARAM, RemoveNeighbor(neighbor_entry.neighbor_key));

    ValidateNeighborEntry(neighbor_entry, /*router_intf_ref_count=*/1);
}

TEST_F(NeighborManagerTest, RemoveNeighborSaiApiFails)
{
    P4NeighborEntry neighbor_entry(kRouterInterfaceId2, kNeighborId2, kMacAddress2);
    AddNeighborEntry(neighbor_entry, kRouterInterfaceOid2);

    EXPECT_CALL(mock_sai_neighbor_, remove_neighbor_entry(_)).WillOnce(Return(SAI_STATUS_FAILURE));

    EXPECT_EQ(StatusCode::SWSS_RC_UNKNOWN, RemoveNeighbor(neighbor_entry.neighbor_key));

    ValidateNeighborEntry(neighbor_entry, /*router_intf_ref_count=*/1);
}

TEST_F(NeighborManagerTest, SetDstMacAddressModifyMacAddress)
{
    P4NeighborEntry neighbor_entry(kRouterInterfaceId2, kNeighborId2, kMacAddress2);
    AddNeighborEntry(neighbor_entry, kRouterInterfaceOid2);

    sai_neighbor_entry_t neigh_entry;
    neigh_entry.switch_id = gSwitchId;
    copy(neigh_entry.ip_address, neighbor_entry.neighbor_id);
    neigh_entry.rif_id = kRouterInterfaceOid2;

    EXPECT_CALL(mock_sai_neighbor_,
                set_neighbor_entry_attribute(
                    Truly(std::bind(MatchNeighborEntry, std::placeholders::_1, neigh_entry)),
                    Truly(std::bind(MatchNeighborSetAttributeList, std::placeholders::_1, kMacAddress1))))
        .WillOnce(Return(SAI_STATUS_SUCCESS));

    EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS, SetDstMacAddress(&neighbor_entry, kMacAddress1));
    EXPECT_EQ(neighbor_entry.dst_mac_address, kMacAddress1);
}

TEST_F(NeighborManagerTest, SetDstMacAddressIdempotent)
{
    P4NeighborEntry neighbor_entry(kRouterInterfaceId2, kNeighborId2, kMacAddress2);

    // SAI API not being called makes the operation idempotent.
    EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS, SetDstMacAddress(&neighbor_entry, kMacAddress2));
    EXPECT_EQ(neighbor_entry.dst_mac_address, kMacAddress2);
}

TEST_F(NeighborManagerTest, SetDstMacAddressSaiApiFails)
{
    P4NeighborEntry neighbor_entry(kRouterInterfaceId2, kNeighborId2, kMacAddress2);

    EXPECT_CALL(mock_sai_neighbor_, set_neighbor_entry_attribute(_, _)).WillOnce(Return(SAI_STATUS_FAILURE));

    EXPECT_EQ(StatusCode::SWSS_RC_UNKNOWN, SetDstMacAddress(&neighbor_entry, kMacAddress1));
    EXPECT_EQ(neighbor_entry.dst_mac_address, kMacAddress2);
}

TEST_F(NeighborManagerTest, ProcessAddRequestValidAppDbParams)
{
    const P4NeighborAppDbEntry app_db_entry = {.router_intf_id = kRouterInterfaceId1,
                                               .neighbor_id = kNeighborId1,
                                               .dst_mac_address = kMacAddress1,
                                               .is_set_dst_mac = true};

    P4NeighborEntry neighbor_entry(app_db_entry.router_intf_id, app_db_entry.neighbor_id, app_db_entry.dst_mac_address);
    neighbor_entry.neigh_entry.switch_id = gSwitchId;
    copy(neighbor_entry.neigh_entry.ip_address, app_db_entry.neighbor_id);
    neighbor_entry.neigh_entry.rif_id = kRouterInterfaceOid1;

    EXPECT_CALL(
        mock_sai_neighbor_,
        create_neighbor_entry(
            Truly(std::bind(MatchNeighborEntry, std::placeholders::_1, neighbor_entry.neigh_entry)), Eq(2),
            Truly(std::bind(MatchNeighborCreateAttributeList, std::placeholders::_1, app_db_entry.dst_mac_address))))
        .WillOnce(Return(SAI_STATUS_SUCCESS));

    ASSERT_TRUE(p4_oid_mapper_.setOID(SAI_OBJECT_TYPE_ROUTER_INTERFACE,
                                      KeyGenerator::generateRouterInterfaceKey(app_db_entry.router_intf_id),
                                      neighbor_entry.neigh_entry.rif_id));

    const std::string neighbor_key =
        KeyGenerator::generateNeighborKey(app_db_entry.router_intf_id, app_db_entry.neighbor_id);
    EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS, ProcessAddRequest(app_db_entry, neighbor_key));

    ValidateNeighborEntry(neighbor_entry, /*router_intf_ref_count=*/1);
}

TEST_F(NeighborManagerTest, ProcessAddRequesDstMacAddressNotSet)
{
    const P4NeighborAppDbEntry app_db_entry = {.router_intf_id = kRouterInterfaceId1,
                                               .neighbor_id = kNeighborId1,
                                               .dst_mac_address = swss::MacAddress(),
                                               .is_set_dst_mac = false};

    const std::string neighbor_key =
        KeyGenerator::generateNeighborKey(app_db_entry.router_intf_id, app_db_entry.neighbor_id);
    EXPECT_EQ(StatusCode::SWSS_RC_INVALID_PARAM, ProcessAddRequest(app_db_entry, neighbor_key));

    P4NeighborEntry neighbor_entry(app_db_entry.router_intf_id, app_db_entry.neighbor_id, app_db_entry.dst_mac_address);
    ValidateNeighborEntryNotPresent(neighbor_entry, /*check_ref_count=*/false);
}

TEST_F(NeighborManagerTest, ProcessAddRequestInvalidRouterInterface)
{
    const P4NeighborAppDbEntry app_db_entry = {.router_intf_id = kRouterInterfaceId1,
                                               .neighbor_id = kNeighborId1,
                                               .dst_mac_address = kMacAddress1,
                                               .is_set_dst_mac = true};

    const std::string neighbor_key =
        KeyGenerator::generateNeighborKey(app_db_entry.router_intf_id, app_db_entry.neighbor_id);
    EXPECT_EQ(StatusCode::SWSS_RC_NOT_FOUND, ProcessAddRequest(app_db_entry, neighbor_key));

    P4NeighborEntry neighbor_entry(app_db_entry.router_intf_id, app_db_entry.neighbor_id, app_db_entry.dst_mac_address);
    ValidateNeighborEntryNotPresent(neighbor_entry, /*check_ref_count=*/false);
}

TEST_F(NeighborManagerTest, ProcessUpdateRequestSetDstMacAddress)
{
    P4NeighborEntry neighbor_entry(kRouterInterfaceId1, kNeighborId1, kMacAddress1);
    AddNeighborEntry(neighbor_entry, kRouterInterfaceOid1);

    neighbor_entry.neigh_entry.switch_id = gSwitchId;
    copy(neighbor_entry.neigh_entry.ip_address, neighbor_entry.neighbor_id);
    neighbor_entry.neigh_entry.rif_id = kRouterInterfaceOid1;

    EXPECT_CALL(mock_sai_neighbor_,
                set_neighbor_entry_attribute(
                    Truly(std::bind(MatchNeighborEntry, std::placeholders::_1, neighbor_entry.neigh_entry)),
                    Truly(std::bind(MatchNeighborSetAttributeList, std::placeholders::_1, kMacAddress2))))
        .WillOnce(Return(SAI_STATUS_SUCCESS));

    const P4NeighborAppDbEntry app_db_entry = {.router_intf_id = kRouterInterfaceId1,
                                               .neighbor_id = kNeighborId1,
                                               .dst_mac_address = kMacAddress2,
                                               .is_set_dst_mac = true};

    // Update neighbor entry present in the Manager.
    auto current_entry = GetNeighborEntry(neighbor_entry.neighbor_key);
    ASSERT_NE(current_entry, nullptr);
    EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS, ProcessUpdateRequest(app_db_entry, current_entry));

    // Validate that neighbor entry present in the Manager has the updated
    // MacAddress.
    neighbor_entry.dst_mac_address = kMacAddress2;
    ValidateNeighborEntry(neighbor_entry, /*router_intf_ref_count=*/1);
}

TEST_F(NeighborManagerTest, ProcessUpdateRequestSetDstMacAddressFails)
{
    P4NeighborEntry neighbor_entry(kRouterInterfaceId1, kNeighborId1, kMacAddress1);
    AddNeighborEntry(neighbor_entry, kRouterInterfaceOid1);

    EXPECT_CALL(mock_sai_neighbor_, set_neighbor_entry_attribute(_, _)).WillOnce(Return(SAI_STATUS_FAILURE));

    const P4NeighborAppDbEntry app_db_entry = {.router_intf_id = kRouterInterfaceId1,
                                               .neighbor_id = kNeighborId1,
                                               .dst_mac_address = kMacAddress2,
                                               .is_set_dst_mac = true};

    // Update neighbor entry present in the Manager.
    auto current_entry = GetNeighborEntry(neighbor_entry.neighbor_key);
    ASSERT_NE(current_entry, nullptr);
    EXPECT_EQ(StatusCode::SWSS_RC_UNKNOWN, ProcessUpdateRequest(app_db_entry, current_entry));

    // Validate that neighbor entry present in the Manager has not changed.
    ValidateNeighborEntry(neighbor_entry, /*router_intf_ref_count=*/1);
}

TEST_F(NeighborManagerTest, ProcessDeleteRequestExistingNeighborEntry)
{
    P4NeighborEntry neighbor_entry(kRouterInterfaceId1, kNeighborId1, kMacAddress1);
    AddNeighborEntry(neighbor_entry, kRouterInterfaceOid1);

    neighbor_entry.neigh_entry.switch_id = gSwitchId;
    copy(neighbor_entry.neigh_entry.ip_address, neighbor_entry.neighbor_id);
    neighbor_entry.neigh_entry.rif_id = kRouterInterfaceOid1;

    EXPECT_CALL(mock_sai_neighbor_, remove_neighbor_entry(Truly(std::bind(MatchNeighborEntry, std::placeholders::_1,
                                                                          neighbor_entry.neigh_entry))))
        .WillOnce(Return(SAI_STATUS_SUCCESS));

    EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS, ProcessDeleteRequest(neighbor_entry.neighbor_key));

    ValidateNeighborEntryNotPresent(neighbor_entry, /*check_ref_count=*/true);
}

TEST_F(NeighborManagerTest, ProcessDeleteRequestNonExistingNeighborEntry)
{
    EXPECT_EQ(StatusCode::SWSS_RC_NOT_FOUND,
              ProcessDeleteRequest(KeyGenerator::generateNeighborKey(kRouterInterfaceId1, kNeighborId1)));
}

TEST_F(NeighborManagerTest, DeserializeNeighborEntryValidAttributes)
{
    const std::vector<swss::FieldValueTuple> attributes = {
        swss::FieldValueTuple(p4orch::kAction, "set_dst_mac"),
        swss::FieldValueTuple(prependParamField(p4orch::kDstMac), kMacAddress2.to_string()),
    };

    auto app_db_entry_or =
        DeserializeNeighborEntry(CreateNeighborAppDbKey(kRouterInterfaceId2, kNeighborId2), attributes);
    EXPECT_TRUE(app_db_entry_or.ok());
    auto &app_db_entry = *app_db_entry_or;
    EXPECT_EQ(app_db_entry.router_intf_id, kRouterInterfaceId2);
    EXPECT_EQ(app_db_entry.neighbor_id, kNeighborId2);
    EXPECT_EQ(app_db_entry.dst_mac_address, kMacAddress2);
    EXPECT_TRUE(app_db_entry.is_set_dst_mac);
}

TEST_F(NeighborManagerTest, DeserializeNeighborEntryInvalidKeyFormat)
{
    const std::vector<swss::FieldValueTuple> attributes = {
        swss::FieldValueTuple(p4orch::kAction, "set_dst_mac"),
        swss::FieldValueTuple(prependParamField(p4orch::kDstMac), kMacAddress2.to_string()),
    };

    // Ensure that the following key is valid. It shall be modified to construct
    // invalid key in rest of the test case.
    std::string valid_key = R"({"match/router_interface_id":"intf-3/4","match/neighbor_id":"10.0.0.1"})";
    EXPECT_TRUE(DeserializeNeighborEntry(valid_key, attributes).ok());

    // Invalid json format.
    std::string invalid_key = R"({"match/router_interface_id:intf-3/4,match/neighbor_id:10.0.0.1"})";
    EXPECT_FALSE(DeserializeNeighborEntry(invalid_key, attributes).ok());

    // Invalid json format.
    invalid_key = R"([{"match/router_interface_id":"intf-3/4","match/neighbor_id":"10.0.0.1"}])";
    EXPECT_FALSE(DeserializeNeighborEntry(invalid_key, attributes).ok());

    // Invalid json format.
    invalid_key = R"(["match/router_interface_id","intf-3/4","match/neighbor_id","10.0.0.1"])";
    EXPECT_FALSE(DeserializeNeighborEntry(invalid_key, attributes).ok());

    // Invalid json format.
    invalid_key = R"({"match/router_interface_id":"intf-3/4","match/neighbor_id:10.0.0.1"})";
    EXPECT_FALSE(DeserializeNeighborEntry(invalid_key, attributes).ok());

    // Invalid router interface id field name.
    invalid_key = R"({"match/router_interface":"intf-3/4","match/neighbor_id":"10.0.0.1"})";
    EXPECT_FALSE(DeserializeNeighborEntry(invalid_key, attributes).ok());

    // Invalid neighbor id field name.
    invalid_key = R"({"match/router_interface_id":"intf-3/4","match/neighbor":"10.0.0.1"})";
    EXPECT_FALSE(DeserializeNeighborEntry(invalid_key, attributes).ok());
}

TEST_F(NeighborManagerTest, DeserializeNeighborEntryMissingAction)
{
    const std::vector<swss::FieldValueTuple> attributes = {
        swss::FieldValueTuple(prependParamField(p4orch::kDstMac), kMacAddress2.to_string()),
    };

    auto app_db_entry_or =
        DeserializeNeighborEntry(CreateNeighborAppDbKey(kRouterInterfaceId2, kNeighborId2), attributes);
    EXPECT_TRUE(app_db_entry_or.ok());
    auto &app_db_entry = *app_db_entry_or;
    EXPECT_EQ(app_db_entry.router_intf_id, kRouterInterfaceId2);
    EXPECT_EQ(app_db_entry.neighbor_id, kNeighborId2);
    EXPECT_EQ(app_db_entry.dst_mac_address, kMacAddress2);
    EXPECT_TRUE(app_db_entry.is_set_dst_mac);
}

TEST_F(NeighborManagerTest, DeserializeNeighborEntryNoAttributes)
{
    const std::vector<swss::FieldValueTuple> attributes;

    auto app_db_entry_or =
        DeserializeNeighborEntry(CreateNeighborAppDbKey(kRouterInterfaceId2, kNeighborId2), attributes);
    EXPECT_TRUE(app_db_entry_or.ok());
    auto &app_db_entry = *app_db_entry_or;
    EXPECT_EQ(app_db_entry.router_intf_id, kRouterInterfaceId2);
    EXPECT_EQ(app_db_entry.neighbor_id, kNeighborId2);
    EXPECT_EQ(app_db_entry.dst_mac_address, swss::MacAddress());
    EXPECT_FALSE(app_db_entry.is_set_dst_mac);
}

TEST_F(NeighborManagerTest, DeserializeNeighborEntryInvalidField)
{
    const std::vector<swss::FieldValueTuple> attributes = {swss::FieldValueTuple("invalid_field", "invalid_value")};

    EXPECT_FALSE(DeserializeNeighborEntry(CreateNeighborAppDbKey(kRouterInterfaceId2, kNeighborId2), attributes).ok());
}

TEST_F(NeighborManagerTest, DeserializeNeighborEntryInvalidIpAddrValue)
{
    const std::vector<swss::FieldValueTuple> attributes;

    // Invalid IPv4 address.
    std::string invalid_key = R"({"match/router_interface_id":"intf-3/4","match/neighbor_id":"10.0.0.x"})";
    EXPECT_FALSE(DeserializeNeighborEntry(invalid_key, attributes).ok());

    // Invalid IPv6 address.
    invalid_key = R"({"match/router_interface_id":"intf-3/4","match/neighbor_id":"fe80::fe17:5f8g"})";
    EXPECT_FALSE(DeserializeNeighborEntry(invalid_key, attributes).ok());
}

TEST_F(NeighborManagerTest, DeserializeNeighborEntryInvalidMacAddrValue)
{
    const std::vector<swss::FieldValueTuple> attributes = {
        swss::FieldValueTuple(prependParamField(p4orch::kDstMac), "11:22:33:44:55")};

    EXPECT_FALSE(DeserializeNeighborEntry(CreateNeighborAppDbKey(kRouterInterfaceId2, kNeighborId2), attributes).ok());
}

TEST_F(NeighborManagerTest, ValidateNeighborAppDbEntryValidEntry)
{
    const P4NeighborAppDbEntry app_db_entry = {.router_intf_id = kRouterInterfaceId1,
                                               .neighbor_id = kNeighborId1,
                                               .dst_mac_address = kMacAddress1,
                                               .is_set_dst_mac = true};

    ASSERT_TRUE(p4_oid_mapper_.setOID(SAI_OBJECT_TYPE_ROUTER_INTERFACE,
                                      KeyGenerator::generateRouterInterfaceKey(app_db_entry.router_intf_id),
                                      kRouterInterfaceOid1));

    EXPECT_TRUE(ValidateNeighborAppDbEntry(app_db_entry).ok());
}

TEST_F(NeighborManagerTest, ValidateNeighborAppDbEntryNonExistentRouterInterface)
{
    const P4NeighborAppDbEntry app_db_entry = {.router_intf_id = kRouterInterfaceId1,
                                               .neighbor_id = kNeighborId1,
                                               .dst_mac_address = kMacAddress1,
                                               .is_set_dst_mac = true};

    EXPECT_FALSE(ValidateNeighborAppDbEntry(app_db_entry).ok());
}

TEST_F(NeighborManagerTest, ValidateNeighborAppDbEntryZeroMacAddress)
{
    const P4NeighborAppDbEntry app_db_entry = {.router_intf_id = kRouterInterfaceId1,
                                               .neighbor_id = kNeighborId1,
                                               .dst_mac_address = swss::MacAddress(),
                                               .is_set_dst_mac = true};

    ASSERT_TRUE(p4_oid_mapper_.setOID(SAI_OBJECT_TYPE_ROUTER_INTERFACE,
                                      KeyGenerator::generateRouterInterfaceKey(app_db_entry.router_intf_id),
                                      kRouterInterfaceOid1));

    EXPECT_FALSE(ValidateNeighborAppDbEntry(app_db_entry).ok());
}

TEST_F(NeighborManagerTest, ValidateNeighborAppDbEntryMacAddressNotPresent)
{
    const P4NeighborAppDbEntry app_db_entry = {.router_intf_id = kRouterInterfaceId1,
                                               .neighbor_id = kNeighborId1,
                                               .dst_mac_address = swss::MacAddress(),
                                               .is_set_dst_mac = false};

    ASSERT_TRUE(p4_oid_mapper_.setOID(SAI_OBJECT_TYPE_ROUTER_INTERFACE,
                                      KeyGenerator::generateRouterInterfaceKey(app_db_entry.router_intf_id),
                                      kRouterInterfaceOid1));

    EXPECT_TRUE(ValidateNeighborAppDbEntry(app_db_entry).ok());
}

TEST_F(NeighborManagerTest, DrainValidAttributes)
{
    ASSERT_TRUE(p4_oid_mapper_.setOID(SAI_OBJECT_TYPE_ROUTER_INTERFACE,
                                      KeyGenerator::generateRouterInterfaceKey(kRouterInterfaceId1),
                                      kRouterInterfaceOid1));

    const std::string appl_db_key = std::string(APP_P4RT_NEIGHBOR_TABLE_NAME) + kTableKeyDelimiter +
                                    CreateNeighborAppDbKey(kRouterInterfaceId1, kNeighborId1);

    // Enqueue entry for create operation.
    std::vector<swss::FieldValueTuple> attributes;
    attributes.push_back(swss::FieldValueTuple{prependParamField(p4orch::kDstMac), kMacAddress1.to_string()});
    Enqueue(swss::KeyOpFieldsValuesTuple(appl_db_key, SET_COMMAND, attributes));

    EXPECT_CALL(mock_sai_neighbor_, create_neighbor_entry(_, _, _)).WillOnce(Return(SAI_STATUS_SUCCESS));
    Drain();

    P4NeighborEntry neighbor_entry(kRouterInterfaceId1, kNeighborId1, kMacAddress1);
    neighbor_entry.neigh_entry.switch_id = gSwitchId;
    copy(neighbor_entry.neigh_entry.ip_address, neighbor_entry.neighbor_id);
    neighbor_entry.neigh_entry.rif_id = kRouterInterfaceOid1;
    ValidateNeighborEntry(neighbor_entry, /*router_intf_ref_count=*/1);

    // Enqueue entry for update operation.
    attributes.clear();
    attributes.push_back(swss::FieldValueTuple{prependParamField(p4orch::kDstMac), kMacAddress2.to_string()});
    Enqueue(swss::KeyOpFieldsValuesTuple(appl_db_key, SET_COMMAND, attributes));

    EXPECT_CALL(mock_sai_neighbor_, set_neighbor_entry_attribute(_, _)).WillOnce(Return(SAI_STATUS_SUCCESS));
    Drain();

    neighbor_entry.dst_mac_address = kMacAddress2;
    ValidateNeighborEntry(neighbor_entry, /*router_intf_ref_count=*/1);

    // Enqueue entry for delete operation.
    attributes.clear();
    Enqueue(swss::KeyOpFieldsValuesTuple(appl_db_key, DEL_COMMAND, attributes));

    EXPECT_CALL(mock_sai_neighbor_, remove_neighbor_entry(_)).WillOnce(Return(SAI_STATUS_SUCCESS));
    Drain();

    ValidateNeighborEntryNotPresent(neighbor_entry, /*check_ref_count=*/true);
}

TEST_F(NeighborManagerTest, DrainInvalidAppDbEntryKey)
{
    ASSERT_TRUE(p4_oid_mapper_.setOID(SAI_OBJECT_TYPE_ROUTER_INTERFACE,
                                      KeyGenerator::generateRouterInterfaceKey(kRouterInterfaceId1),
                                      kRouterInterfaceOid1));

    // create invalid neighbor key with router interface id as kRouterInterfaceId1
    // and neighbor id as kNeighborId1
    const std::string invalid_neighbor_key = R"({"match/router_interface_id:intf-3/4,match/neighbor_id:10.0.0.22"})";
    const std::string appl_db_key =
        std::string(APP_P4RT_NEIGHBOR_TABLE_NAME) + kTableKeyDelimiter + invalid_neighbor_key;

    // Enqueue entry for create operation.
    std::vector<swss::FieldValueTuple> attributes;
    Enqueue(swss::KeyOpFieldsValuesTuple(appl_db_key, SET_COMMAND, attributes));
    Drain();

    P4NeighborEntry neighbor_entry(kRouterInterfaceId1, kNeighborId1, kMacAddress1);
    ValidateNeighborEntryNotPresent(neighbor_entry, /*check_ref_count=*/true);
}

TEST_F(NeighborManagerTest, DrainInvalidAppDbEntryAttributes)
{
    ASSERT_TRUE(p4_oid_mapper_.setOID(SAI_OBJECT_TYPE_ROUTER_INTERFACE,
                                      KeyGenerator::generateRouterInterfaceKey(kRouterInterfaceId1),
                                      kRouterInterfaceOid1));

    // Non-existent router interface id in neighbor key.
    std::string appl_db_key = std::string(APP_P4RT_NEIGHBOR_TABLE_NAME) + kTableKeyDelimiter +
                              CreateNeighborAppDbKey(kRouterInterfaceId2, kNeighborId1);

    std::vector<swss::FieldValueTuple> attributes;
    Enqueue(swss::KeyOpFieldsValuesTuple(appl_db_key, SET_COMMAND, attributes));

    appl_db_key = std::string(APP_P4RT_NEIGHBOR_TABLE_NAME) + kTableKeyDelimiter +
                  CreateNeighborAppDbKey(kRouterInterfaceId1, kNeighborId1);
    // Invalid destination mac address attribute.
    attributes.clear();
    attributes.push_back(swss::FieldValueTuple{p4orch::kDstMac, swss::MacAddress().to_string()});
    Enqueue(swss::KeyOpFieldsValuesTuple(appl_db_key, SET_COMMAND, attributes));

    Drain();

    // Validate that first create operation did not create a neighbor entry.
    P4NeighborEntry neighbor_entry1(kRouterInterfaceId2, kNeighborId1, kMacAddress1);
    ValidateNeighborEntryNotPresent(neighbor_entry1, /*check_ref_count=*/false);

    // Validate that second create operation did not create a neighbor entry.
    P4NeighborEntry neighbor_entry2(kRouterInterfaceId1, kNeighborId1, kMacAddress1);
    ValidateNeighborEntryNotPresent(neighbor_entry2, /*check_ref_count=*/true);
}

TEST_F(NeighborManagerTest, DrainInvalidOperation)
{
    ASSERT_TRUE(p4_oid_mapper_.setOID(SAI_OBJECT_TYPE_ROUTER_INTERFACE,
                                      KeyGenerator::generateRouterInterfaceKey(kRouterInterfaceId1),
                                      kRouterInterfaceOid1));

    const std::string appl_db_key = std::string(APP_P4RT_NEIGHBOR_TABLE_NAME) + kTableKeyDelimiter +
                                    CreateNeighborAppDbKey(kRouterInterfaceId1, kNeighborId1);

    std::vector<swss::FieldValueTuple> attributes;
    Enqueue(swss::KeyOpFieldsValuesTuple(appl_db_key, "INVALID", attributes));
    Drain();

    P4NeighborEntry neighbor_entry(kRouterInterfaceId1, kNeighborId1, kMacAddress1);
    ValidateNeighborEntryNotPresent(neighbor_entry, /*check_ref_count=*/true);
}
