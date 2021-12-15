#include "router_interface_manager.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <functional>
#include <string>

#include "mock_response_publisher.h"
#include "mock_sai_router_interface.h"
#include "p4orch.h"
#include "p4orch/p4orch_util.h"
#include "portsorch.h"
#include "return_code.h"
#include "swssnet.h"

using ::p4orch::kTableKeyDelimiter;

using ::testing::_;
using ::testing::DoAll;
using ::testing::Eq;
using ::testing::Return;
using ::testing::SetArgPointee;
using ::testing::StrictMock;
using ::testing::Truly;

extern PortsOrch *gPortsOrch;

extern sai_object_id_t gSwitchId;
extern sai_object_id_t gVirtualRouterId;
extern sai_router_interface_api_t *sai_router_intfs_api;

namespace
{

constexpr char *kPortName1 = "Ethernet1";
constexpr sai_object_id_t kPortOid1 = 0x112233;
constexpr uint32_t kMtu1 = 1500;

constexpr char *kPortName2 = "Ethernet2";
constexpr sai_object_id_t kPortOid2 = 0x1fed3;
constexpr uint32_t kMtu2 = 4500;

constexpr char *kRouterInterfaceId1 = "intf-3/4";
constexpr sai_object_id_t kRouterInterfaceOid1 = 0x295100;
const swss::MacAddress kMacAddress1("00:01:02:03:04:05");

constexpr char *kRouterInterfaceId2 = "Ethernet20";
constexpr sai_object_id_t kRouterInterfaceOid2 = 0x51411;
const swss::MacAddress kMacAddress2("00:ff:ee:dd:cc:bb");

const swss::MacAddress kZeroMacAddress("00:00:00:00:00:00");

constexpr char *kRouterIntfAppDbKey = R"({"match/router_interface_id":"intf-3/4"})";

std::unordered_map<sai_attr_id_t, sai_attribute_value_t> CreateRouterInterfaceAttributeList(
    const sai_object_id_t &virtual_router_oid, const swss::MacAddress mac_address, const sai_object_id_t &port_oid,
    const uint32_t mtu)
{
    std::unordered_map<sai_attr_id_t, sai_attribute_value_t> attr_list;
    sai_attribute_value_t attr_value;

    attr_value.oid = virtual_router_oid;
    attr_list[SAI_ROUTER_INTERFACE_ATTR_VIRTUAL_ROUTER_ID] = attr_value;

    if (mac_address != kZeroMacAddress)
    {
        memcpy(attr_value.mac, mac_address.getMac(), sizeof(sai_mac_t));
        attr_list[SAI_ROUTER_INTERFACE_ATTR_SRC_MAC_ADDRESS] = attr_value;
    }

    attr_value.s32 = SAI_ROUTER_INTERFACE_TYPE_PORT;
    attr_list[SAI_ROUTER_INTERFACE_ATTR_TYPE] = attr_value;

    attr_value.oid = port_oid;
    attr_list[SAI_ROUTER_INTERFACE_ATTR_PORT_ID] = attr_value;

    attr_value.u32 = mtu;
    attr_list[SAI_ROUTER_INTERFACE_ATTR_MTU] = attr_value;

    return attr_list;
}

bool MatchCreateRouterInterfaceAttributeList(
    const sai_attribute_t *attr_list,
    const std::unordered_map<sai_attr_id_t, sai_attribute_value_t> &expected_attr_list)
{
    if (attr_list == nullptr)
        return false;

    int matched_attr_num = 0;
    const int attr_list_length = (int)expected_attr_list.size();
    for (int i = 0; i < attr_list_length; i++)
    {
        switch (attr_list[i].id)
        {
        case SAI_ROUTER_INTERFACE_ATTR_VIRTUAL_ROUTER_ID:
            if (attr_list[i].value.oid != expected_attr_list.at(SAI_ROUTER_INTERFACE_ATTR_VIRTUAL_ROUTER_ID).oid)
            {
                return false;
            }
            matched_attr_num++;
            break;

        case SAI_ROUTER_INTERFACE_ATTR_SRC_MAC_ADDRESS:
            if (memcmp(attr_list[i].value.mac, expected_attr_list.at(SAI_ROUTER_INTERFACE_ATTR_SRC_MAC_ADDRESS).mac,
                       sizeof(sai_mac_t)))
            {
                return false;
            }
            matched_attr_num++;
            break;

        case SAI_ROUTER_INTERFACE_ATTR_TYPE:
            if (attr_list[i].value.s32 != expected_attr_list.at(SAI_ROUTER_INTERFACE_ATTR_TYPE).s32)
            {
                return false;
            }
            matched_attr_num++;
            break;

        case SAI_ROUTER_INTERFACE_ATTR_PORT_ID:
            if (attr_list[i].value.oid != expected_attr_list.at(SAI_ROUTER_INTERFACE_ATTR_PORT_ID).oid)
            {
                return false;
            }
            matched_attr_num++;
            break;

        case SAI_ROUTER_INTERFACE_ATTR_MTU:
            if (attr_list[i].value.u32 != expected_attr_list.at(SAI_ROUTER_INTERFACE_ATTR_MTU).u32)
            {
                return false;
            }
            matched_attr_num++;
            break;

        default:
            // Unexpected attribute present in attribute list
            return false;
        }
    }

    return (matched_attr_num == attr_list_length);
}

} // namespace

class RouterInterfaceManagerTest : public ::testing::Test
{
  protected:
    RouterInterfaceManagerTest() : router_intf_manager_(&p4_oid_mapper_, &publisher_)
    {
    }

    void SetUp() override
    {
        mock_sai_router_intf = &mock_sai_router_intf_;
        sai_router_intfs_api->create_router_interface = mock_create_router_interface;
        sai_router_intfs_api->remove_router_interface = mock_remove_router_interface;
        sai_router_intfs_api->set_router_interface_attribute = mock_set_router_interface_attribute;
        sai_router_intfs_api->get_router_interface_attribute = mock_get_router_interface_attribute;
    }

    void Enqueue(const swss::KeyOpFieldsValuesTuple &entry)
    {
        router_intf_manager_.enqueue(entry);
    }

    void Drain()
    {
        router_intf_manager_.drain();
    }

    ReturnCodeOr<P4RouterInterfaceAppDbEntry> DeserializeRouterIntfEntry(
        const std::string &key, const std::vector<swss::FieldValueTuple> &attributes)
    {
        return router_intf_manager_.deserializeRouterIntfEntry(key, attributes);
    }

    ReturnCode CreateRouterInterface(const std::string &router_intf_key, P4RouterInterfaceEntry &router_intf_entry)
    {
        return router_intf_manager_.createRouterInterface(router_intf_key, router_intf_entry);
    }

    ReturnCode RemoveRouterInterface(const std::string &router_intf_key)
    {
        return router_intf_manager_.removeRouterInterface(router_intf_key);
    }

    ReturnCode SetSourceMacAddress(P4RouterInterfaceEntry *router_intf_entry, const swss::MacAddress &mac_address)
    {
        return router_intf_manager_.setSourceMacAddress(router_intf_entry, mac_address);
    }

    ReturnCode ProcessAddRequest(const P4RouterInterfaceAppDbEntry &app_db_entry, const std::string &router_intf_key)
    {
        return router_intf_manager_.processAddRequest(app_db_entry, router_intf_key);
    }

    ReturnCode ProcessUpdateRequest(const P4RouterInterfaceAppDbEntry &app_db_entry,
                                    P4RouterInterfaceEntry *router_intf_entry)
    {
        return router_intf_manager_.processUpdateRequest(app_db_entry, router_intf_entry);
    }

    ReturnCode ProcessDeleteRequest(const std::string &router_intf_key)
    {
        return router_intf_manager_.processDeleteRequest(router_intf_key);
    }

    P4RouterInterfaceEntry *GetRouterInterfaceEntry(const std::string &router_intf_key)
    {
        return router_intf_manager_.getRouterInterfaceEntry(router_intf_key);
    }

    void ValidateRouterInterfaceEntry(const P4RouterInterfaceEntry &expected_entry)
    {
        const std::string router_intf_key =
            KeyGenerator::generateRouterInterfaceKey(expected_entry.router_interface_id);
        auto router_intf_entry = GetRouterInterfaceEntry(router_intf_key);

        EXPECT_NE(nullptr, router_intf_entry);
        EXPECT_EQ(expected_entry.router_interface_id, router_intf_entry->router_interface_id);
        EXPECT_EQ(expected_entry.port_name, router_intf_entry->port_name);
        EXPECT_EQ(expected_entry.src_mac_address, router_intf_entry->src_mac_address);
        EXPECT_EQ(expected_entry.router_interface_oid, router_intf_entry->router_interface_oid);

        sai_object_id_t p4_mapper_oid;
        ASSERT_TRUE(p4_oid_mapper_.getOID(SAI_OBJECT_TYPE_ROUTER_INTERFACE, router_intf_key, &p4_mapper_oid));
        EXPECT_EQ(expected_entry.router_interface_oid, p4_mapper_oid);
    }

    void ValidateRouterInterfaceEntryNotPresent(const std::string router_interface_id)
    {
        const std::string router_intf_key = KeyGenerator::generateRouterInterfaceKey(router_interface_id);
        auto current_entry = GetRouterInterfaceEntry(router_intf_key);
        EXPECT_EQ(current_entry, nullptr);
        EXPECT_FALSE(p4_oid_mapper_.existsOID(SAI_OBJECT_TYPE_ROUTER_INTERFACE, router_intf_key));
    }

    void AddRouterInterfaceEntry(P4RouterInterfaceEntry &router_intf_entry, const sai_object_id_t port_oid,
                                 const uint32_t mtu)
    {
        EXPECT_CALL(mock_sai_router_intf_,
                    create_router_interface(
                        ::testing::NotNull(), Eq(gSwitchId), Eq(5),
                        Truly(std::bind(MatchCreateRouterInterfaceAttributeList, std::placeholders::_1,
                                        CreateRouterInterfaceAttributeList(
                                            gVirtualRouterId, router_intf_entry.src_mac_address, port_oid, mtu)))))
            .WillOnce(DoAll(SetArgPointee<0>(router_intf_entry.router_interface_oid), Return(SAI_STATUS_SUCCESS)));

        const std::string router_intf_key =
            KeyGenerator::generateRouterInterfaceKey(router_intf_entry.router_interface_id);
        EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS, CreateRouterInterface(router_intf_key, router_intf_entry));
    }

    StrictMock<MockSaiRouterInterface> mock_sai_router_intf_;
    MockResponsePublisher publisher_;
    P4OidMapper p4_oid_mapper_;
    RouterInterfaceManager router_intf_manager_;
};

TEST_F(RouterInterfaceManagerTest, CreateRouterInterfaceValidAttributes)
{
    P4RouterInterfaceEntry router_intf_entry(kRouterInterfaceId1, kPortName1, kMacAddress1);
    AddRouterInterfaceEntry(router_intf_entry, kPortOid1, kMtu1);

    ValidateRouterInterfaceEntry(router_intf_entry);
}

TEST_F(RouterInterfaceManagerTest, CreateRouterInterfaceEntryExistsInManager)
{
    P4RouterInterfaceEntry router_intf_entry(kRouterInterfaceId1, kPortName1, kMacAddress1);
    router_intf_entry.router_interface_oid = kRouterInterfaceOid1;
    AddRouterInterfaceEntry(router_intf_entry, kPortOid1, kMtu1);

    // Same router interface key with different attributes
    P4RouterInterfaceEntry new_entry(router_intf_entry.router_interface_id, kPortName2, kMacAddress2);
    const std::string router_intf_key = KeyGenerator::generateRouterInterfaceKey(router_intf_entry.router_interface_id);
    EXPECT_EQ(StatusCode::SWSS_RC_EXISTS, CreateRouterInterface(router_intf_key, new_entry));

    // Validate that entry in Manager and Centralized Mapper has not changed
    ValidateRouterInterfaceEntry(router_intf_entry);
}

TEST_F(RouterInterfaceManagerTest, CreateRouterInterfaceEntryExistsInP4OidMapper)
{
    const std::string router_intf_key = KeyGenerator::generateRouterInterfaceKey(kRouterInterfaceId2);
    p4_oid_mapper_.setOID(SAI_OBJECT_TYPE_ROUTER_INTERFACE, router_intf_key, kRouterInterfaceOid2);
    P4RouterInterfaceEntry router_intf_entry(kRouterInterfaceId2, kPortName2, kMacAddress2);

    // (TODO): Expect critical state.
    EXPECT_EQ(StatusCode::SWSS_RC_INTERNAL, CreateRouterInterface(router_intf_key, router_intf_entry));

    auto current_entry = GetRouterInterfaceEntry(router_intf_key);
    EXPECT_EQ(current_entry, nullptr);

    // Validate that OID doesn't change in Centralized Mapper
    sai_object_id_t mapper_oid;
    ASSERT_TRUE(p4_oid_mapper_.getOID(SAI_OBJECT_TYPE_ROUTER_INTERFACE, router_intf_key, &mapper_oid));
    EXPECT_EQ(mapper_oid, kRouterInterfaceOid2);
}

TEST_F(RouterInterfaceManagerTest, CreateRouterInterfaceInvalidPort)
{
    const std::string invalid_port_name = "xyz";
    P4RouterInterfaceEntry router_intf_entry(kRouterInterfaceId2, invalid_port_name, kMacAddress2);

    EXPECT_EQ(StatusCode::SWSS_RC_NOT_FOUND,
              CreateRouterInterface(KeyGenerator::generateRouterInterfaceKey(kRouterInterfaceId2), router_intf_entry));

    ValidateRouterInterfaceEntryNotPresent(kRouterInterfaceId2);
}

TEST_F(RouterInterfaceManagerTest, CreateRouterInterfaceNoMacAddress)
{
    P4RouterInterfaceEntry router_intf_entry;
    router_intf_entry.router_interface_id = kRouterInterfaceId1;
    router_intf_entry.port_name = kPortName1;

    EXPECT_CALL(mock_sai_router_intf_,
                create_router_interface(::testing::NotNull(), Eq(gSwitchId), Eq(4),
                                        Truly(std::bind(MatchCreateRouterInterfaceAttributeList, std::placeholders::_1,
                                                        CreateRouterInterfaceAttributeList(
                                                            gVirtualRouterId, kZeroMacAddress, kPortOid1, kMtu1)))))
        .WillOnce(DoAll(SetArgPointee<0>(kRouterInterfaceOid1), Return(SAI_STATUS_SUCCESS)));

    const std::string router_intf_key = KeyGenerator::generateRouterInterfaceKey(kRouterInterfaceId1);
    EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS, CreateRouterInterface(router_intf_key, router_intf_entry));

    ValidateRouterInterfaceEntry(router_intf_entry);
}

TEST_F(RouterInterfaceManagerTest, CreateRouterInterfaceSaiApiFails)
{
    P4RouterInterfaceEntry router_intf_entry(kRouterInterfaceId1, kPortName1, kMacAddress1);
    EXPECT_CALL(mock_sai_router_intf_, create_router_interface(_, _, _, _)).WillOnce(Return(SAI_STATUS_FAILURE));

    EXPECT_EQ(StatusCode::SWSS_RC_UNKNOWN,
              CreateRouterInterface(KeyGenerator::generateRouterInterfaceKey(router_intf_entry.router_interface_id),
                                    router_intf_entry));

    ValidateRouterInterfaceEntryNotPresent(router_intf_entry.router_interface_id);
}

TEST_F(RouterInterfaceManagerTest, RemoveRouterInterfaceExistingInterface)
{
    P4RouterInterfaceEntry router_intf_entry(kRouterInterfaceId2, kPortName2, kMacAddress2);
    router_intf_entry.router_interface_oid = kRouterInterfaceOid2;
    AddRouterInterfaceEntry(router_intf_entry, kPortOid2, kMtu2);

    EXPECT_CALL(mock_sai_router_intf_, remove_router_interface(Eq(router_intf_entry.router_interface_oid)))
        .WillOnce(Return(SAI_STATUS_SUCCESS));

    EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS,
              RemoveRouterInterface(KeyGenerator::generateRouterInterfaceKey(router_intf_entry.router_interface_id)));

    ValidateRouterInterfaceEntryNotPresent(router_intf_entry.router_interface_id);
}

TEST_F(RouterInterfaceManagerTest, RemoveRouterInterfaceNonExistingInterface)
{
    EXPECT_EQ(StatusCode::SWSS_RC_NOT_FOUND,
              RemoveRouterInterface(KeyGenerator::generateRouterInterfaceKey(kRouterInterfaceId2)));
}

TEST_F(RouterInterfaceManagerTest, RemoveRouterInterfaceNonZeroRefCount)
{
    P4RouterInterfaceEntry router_intf_entry(kRouterInterfaceId2, kPortName2, kMacAddress2);
    router_intf_entry.router_interface_oid = kRouterInterfaceOid2;
    AddRouterInterfaceEntry(router_intf_entry, kPortOid2, kMtu2);

    const std::string router_intf_key = KeyGenerator::generateRouterInterfaceKey(router_intf_entry.router_interface_id);
    ASSERT_TRUE(p4_oid_mapper_.increaseRefCount(SAI_OBJECT_TYPE_ROUTER_INTERFACE, router_intf_key));

    EXPECT_EQ(StatusCode::SWSS_RC_INVALID_PARAM, RemoveRouterInterface(router_intf_key));

    ValidateRouterInterfaceEntry(router_intf_entry);
}

TEST_F(RouterInterfaceManagerTest, RemoveRouterInterfaceSaiApiFails)
{
    P4RouterInterfaceEntry router_intf_entry(kRouterInterfaceId2, kPortName2, kMacAddress2);
    router_intf_entry.router_interface_oid = kRouterInterfaceOid2;
    AddRouterInterfaceEntry(router_intf_entry, kPortOid2, kMtu2);

    EXPECT_CALL(mock_sai_router_intf_, remove_router_interface(Eq(router_intf_entry.router_interface_oid)))
        .WillOnce(Return(SAI_STATUS_FAILURE));

    EXPECT_EQ(StatusCode::SWSS_RC_UNKNOWN,
              RemoveRouterInterface(KeyGenerator::generateRouterInterfaceKey(router_intf_entry.router_interface_id)));

    ValidateRouterInterfaceEntry(router_intf_entry);
}

TEST_F(RouterInterfaceManagerTest, SetSourceMacAddressModifyMacAddress)
{
    P4RouterInterfaceEntry router_intf_entry(kRouterInterfaceId1, kPortName1, kMacAddress1);
    router_intf_entry.router_interface_oid = kRouterInterfaceOid1;

    sai_attribute_value_t attr_value;
    memcpy(attr_value.mac, kMacAddress2.getMac(), sizeof(sai_mac_t));
    std::unordered_map<sai_attr_id_t, sai_attribute_value_t> attr_list = {
        {SAI_ROUTER_INTERFACE_ATTR_SRC_MAC_ADDRESS, attr_value}};
    EXPECT_CALL(mock_sai_router_intf_,
                set_router_interface_attribute(
                    Eq(router_intf_entry.router_interface_oid),
                    Truly(std::bind(MatchCreateRouterInterfaceAttributeList, std::placeholders::_1, attr_list))))
        .WillOnce(Return(SAI_STATUS_SUCCESS));

    EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS, SetSourceMacAddress(&router_intf_entry, kMacAddress2));
    EXPECT_EQ(router_intf_entry.src_mac_address, kMacAddress2);
}

TEST_F(RouterInterfaceManagerTest, SetSourceMacAddressIdempotent)
{
    P4RouterInterfaceEntry router_intf_entry(kRouterInterfaceId1, kPortName1, kMacAddress1);

    // SAI API not being called makes the operation idempotent.
    EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS, SetSourceMacAddress(&router_intf_entry, kMacAddress1));
    EXPECT_EQ(router_intf_entry.src_mac_address, kMacAddress1);
}

TEST_F(RouterInterfaceManagerTest, SetSourceMacAddressSaiApiFails)
{
    P4RouterInterfaceEntry router_intf_entry(kRouterInterfaceId1, kPortName1, kMacAddress1);
    router_intf_entry.router_interface_oid = kRouterInterfaceOid1;

    sai_attribute_value_t attr_value;
    memcpy(attr_value.mac, kMacAddress2.getMac(), sizeof(sai_mac_t));
    std::unordered_map<sai_attr_id_t, sai_attribute_value_t> attr_list = {
        {SAI_ROUTER_INTERFACE_ATTR_SRC_MAC_ADDRESS, attr_value}};
    EXPECT_CALL(mock_sai_router_intf_,
                set_router_interface_attribute(
                    Eq(router_intf_entry.router_interface_oid),
                    Truly(std::bind(MatchCreateRouterInterfaceAttributeList, std::placeholders::_1, attr_list))))
        .WillOnce(Return(SAI_STATUS_FAILURE));

    EXPECT_EQ(StatusCode::SWSS_RC_UNKNOWN, SetSourceMacAddress(&router_intf_entry, kMacAddress2));
    EXPECT_EQ(router_intf_entry.src_mac_address, kMacAddress1);
}

TEST_F(RouterInterfaceManagerTest, ProcessAddRequestValidAppDbParams)
{
    const P4RouterInterfaceAppDbEntry app_db_entry = {.router_interface_id = kRouterInterfaceId1,
                                                      .port_name = kPortName1,
                                                      .src_mac_address = kMacAddress1,
                                                      .is_set_port_name = true,
                                                      .is_set_src_mac = true};

    EXPECT_CALL(mock_sai_router_intf_,
                create_router_interface(::testing::NotNull(), Eq(gSwitchId), Eq(5),
                                        Truly(std::bind(MatchCreateRouterInterfaceAttributeList, std::placeholders::_1,
                                                        CreateRouterInterfaceAttributeList(
                                                            gVirtualRouterId, kMacAddress1, kPortOid1, kMtu1)))))
        .WillOnce(DoAll(SetArgPointee<0>(kRouterInterfaceOid1), Return(SAI_STATUS_SUCCESS)));

    const std::string router_intf_key = KeyGenerator::generateRouterInterfaceKey(app_db_entry.router_interface_id);
    EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS, ProcessAddRequest(app_db_entry, router_intf_key));

    P4RouterInterfaceEntry router_intf_entry(app_db_entry.router_interface_id, app_db_entry.port_name,
                                             app_db_entry.src_mac_address);
    router_intf_entry.router_interface_oid = kRouterInterfaceOid1;
    ValidateRouterInterfaceEntry(router_intf_entry);
}

TEST_F(RouterInterfaceManagerTest, ProcessAddRequestPortNameMissing)
{
    const P4RouterInterfaceAppDbEntry app_db_entry = {.router_interface_id = kRouterInterfaceId1,
                                                      .port_name = "",
                                                      .src_mac_address = kMacAddress1,
                                                      .is_set_port_name = false,
                                                      .is_set_src_mac = true};

    const std::string router_intf_key = KeyGenerator::generateRouterInterfaceKey(app_db_entry.router_interface_id);
    EXPECT_EQ(StatusCode::SWSS_RC_INVALID_PARAM, ProcessAddRequest(app_db_entry, router_intf_key));
}

TEST_F(RouterInterfaceManagerTest, ProcessAddRequestInvalidPortName)
{
    const P4RouterInterfaceAppDbEntry app_db_entry = {.router_interface_id = kRouterInterfaceId1,
                                                      .port_name = "",
                                                      .src_mac_address = kMacAddress1,
                                                      .is_set_port_name = true,
                                                      .is_set_src_mac = true};

    const std::string router_intf_key = KeyGenerator::generateRouterInterfaceKey(app_db_entry.router_interface_id);
    EXPECT_EQ(StatusCode::SWSS_RC_NOT_FOUND, ProcessAddRequest(app_db_entry, router_intf_key));
}

TEST_F(RouterInterfaceManagerTest, ProcessUpdateRequestSetSourceMacAddress)
{
    P4RouterInterfaceEntry router_intf_entry(kRouterInterfaceId1, kPortName1, kMacAddress1);
    router_intf_entry.router_interface_oid = kRouterInterfaceOid1;
    AddRouterInterfaceEntry(router_intf_entry, kPortOid1, kMtu1);

    sai_attribute_value_t attr_value;
    memcpy(attr_value.mac, kMacAddress2.getMac(), sizeof(sai_mac_t));
    std::unordered_map<sai_attr_id_t, sai_attribute_value_t> attr_list = {
        {SAI_ROUTER_INTERFACE_ATTR_SRC_MAC_ADDRESS, attr_value}};
    EXPECT_CALL(mock_sai_router_intf_,
                set_router_interface_attribute(
                    Eq(router_intf_entry.router_interface_oid),
                    Truly(std::bind(MatchCreateRouterInterfaceAttributeList, std::placeholders::_1, attr_list))))
        .WillOnce(Return(SAI_STATUS_SUCCESS));

    const P4RouterInterfaceAppDbEntry app_db_entry = {.router_interface_id = router_intf_entry.router_interface_id,
                                                      .port_name = "",
                                                      .src_mac_address = kMacAddress2,
                                                      .is_set_port_name = false,
                                                      .is_set_src_mac = true};

    // Update router interface entry present in the Manager.
    auto current_entry =
        GetRouterInterfaceEntry(KeyGenerator::generateRouterInterfaceKey(router_intf_entry.router_interface_id));
    ASSERT_NE(current_entry, nullptr);
    EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS, ProcessUpdateRequest(app_db_entry, current_entry));

    // Validate that router interface entry present in the Manager has the updated
    // MacAddress.
    router_intf_entry.src_mac_address = kMacAddress2;
    ValidateRouterInterfaceEntry(router_intf_entry);
}

TEST_F(RouterInterfaceManagerTest, ProcessUpdateRequestSetPortNameIdempotent)
{
    P4RouterInterfaceEntry router_intf_entry(kRouterInterfaceId1, kPortName1, kMacAddress1);
    router_intf_entry.router_interface_oid = kRouterInterfaceOid1;
    AddRouterInterfaceEntry(router_intf_entry, kPortOid1, kMtu1);

    const P4RouterInterfaceAppDbEntry app_db_entry = {.router_interface_id = router_intf_entry.router_interface_id,
                                                      .port_name = kPortName1,
                                                      .src_mac_address = swss::MacAddress(),
                                                      .is_set_port_name = true,
                                                      .is_set_src_mac = false};

    // Update router interface entry present in the Manager.
    auto current_entry =
        GetRouterInterfaceEntry(KeyGenerator::generateRouterInterfaceKey(router_intf_entry.router_interface_id));
    ASSERT_NE(current_entry, nullptr);
    EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS, ProcessUpdateRequest(app_db_entry, current_entry));

    // Validate that router interface entry present in the Manager has not
    // changed.
    ValidateRouterInterfaceEntry(router_intf_entry);
}

TEST_F(RouterInterfaceManagerTest, ProcessUpdateRequestSetPortName)
{
    P4RouterInterfaceEntry router_intf_entry(kRouterInterfaceId1, kPortName1, kMacAddress1);
    router_intf_entry.router_interface_oid = kRouterInterfaceOid1;
    AddRouterInterfaceEntry(router_intf_entry, kPortOid1, kMtu1);

    const P4RouterInterfaceAppDbEntry app_db_entry = {.router_interface_id = router_intf_entry.router_interface_id,
                                                      .port_name = kPortName2,
                                                      .src_mac_address = swss::MacAddress(),
                                                      .is_set_port_name = true,
                                                      .is_set_src_mac = false};

    // Update router interface entry present in the Manager.
    auto current_entry =
        GetRouterInterfaceEntry(KeyGenerator::generateRouterInterfaceKey(router_intf_entry.router_interface_id));
    ASSERT_NE(current_entry, nullptr);
    EXPECT_EQ(StatusCode::SWSS_RC_UNIMPLEMENTED, ProcessUpdateRequest(app_db_entry, current_entry));

    // Validate that router interface entry present in the Manager has not
    // changed.
    ValidateRouterInterfaceEntry(router_intf_entry);
}

TEST_F(RouterInterfaceManagerTest, ProcessUpdateRequestMacAddrAndPort)
{
    P4RouterInterfaceEntry router_intf_entry(kRouterInterfaceId1, kPortName1, kMacAddress1);
    router_intf_entry.router_interface_oid = kRouterInterfaceOid1;
    AddRouterInterfaceEntry(router_intf_entry, kPortOid1, kMtu1);

    const P4RouterInterfaceAppDbEntry app_db_entry = {.router_interface_id = router_intf_entry.router_interface_id,
                                                      .port_name = kPortName2,
                                                      .src_mac_address = kMacAddress2,
                                                      .is_set_port_name = true,
                                                      .is_set_src_mac = true};

    // Update router interface entry present in the Manager.
    auto current_entry =
        GetRouterInterfaceEntry(KeyGenerator::generateRouterInterfaceKey(router_intf_entry.router_interface_id));
    ASSERT_NE(current_entry, nullptr);
    // Update port name not supported, hence ProcessUpdateRequest should fail.
    EXPECT_EQ(StatusCode::SWSS_RC_UNIMPLEMENTED, ProcessUpdateRequest(app_db_entry, current_entry));

    // Validate that router interface entry present in the Manager does not
    // changed.
    ValidateRouterInterfaceEntry(router_intf_entry);
}

TEST_F(RouterInterfaceManagerTest, ProcessDeleteRequestExistingInterface)
{
    P4RouterInterfaceEntry router_intf_entry(kRouterInterfaceId1, kPortName1, kMacAddress1);
    router_intf_entry.router_interface_oid = kRouterInterfaceOid1;
    AddRouterInterfaceEntry(router_intf_entry, kPortOid1, kMtu1);

    EXPECT_CALL(mock_sai_router_intf_, remove_router_interface(Eq(router_intf_entry.router_interface_oid)))
        .WillOnce(Return(SAI_STATUS_SUCCESS));

    EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS,
              ProcessDeleteRequest(KeyGenerator::generateRouterInterfaceKey(router_intf_entry.router_interface_id)));

    ValidateRouterInterfaceEntryNotPresent(router_intf_entry.router_interface_id);
}

TEST_F(RouterInterfaceManagerTest, ProcessDeleteRequestNonExistingInterface)
{
    EXPECT_EQ(StatusCode::SWSS_RC_NOT_FOUND,
              ProcessDeleteRequest(KeyGenerator::generateRouterInterfaceKey(kRouterInterfaceId1)));
}

TEST_F(RouterInterfaceManagerTest, ProcessDeleteRequestInterfaceNotExistInMapper)
{
    P4RouterInterfaceEntry router_intf_entry(kRouterInterfaceId1, kPortName1, kMacAddress1);
    router_intf_entry.router_interface_oid = kRouterInterfaceOid1;
    AddRouterInterfaceEntry(router_intf_entry, kPortOid1, kMtu1);

    p4_oid_mapper_.eraseOID(SAI_OBJECT_TYPE_ROUTER_INTERFACE,
                            KeyGenerator::generateRouterInterfaceKey(kRouterInterfaceId1));
    // (TODO): Expect critical state.
    EXPECT_EQ(StatusCode::SWSS_RC_INTERNAL,
              ProcessDeleteRequest(KeyGenerator::generateRouterInterfaceKey(router_intf_entry.router_interface_id)));
}

TEST_F(RouterInterfaceManagerTest, DeserializeRouterIntfEntryValidAttributes)
{
    const std::vector<swss::FieldValueTuple> attributes = {
        swss::FieldValueTuple(p4orch::kAction, "set_port_and_src_mac"),
        swss::FieldValueTuple(prependParamField(p4orch::kPort), kPortName1),
        swss::FieldValueTuple(prependParamField(p4orch::kSrcMac), kMacAddress1.to_string()),
    };

    auto app_db_entry_or = DeserializeRouterIntfEntry(kRouterIntfAppDbKey, attributes);
    EXPECT_TRUE(app_db_entry_or.ok());
    auto &app_db_entry = *app_db_entry_or;
    EXPECT_EQ(app_db_entry.router_interface_id, kRouterInterfaceId1);
    EXPECT_EQ(app_db_entry.port_name, kPortName1);
    EXPECT_EQ(app_db_entry.src_mac_address, kMacAddress1);
    EXPECT_TRUE(app_db_entry.is_set_port_name);
    EXPECT_TRUE(app_db_entry.is_set_src_mac);
}

TEST_F(RouterInterfaceManagerTest, DeserializeRouterIntfEntryInvalidKeyFormat)
{
    const std::vector<swss::FieldValueTuple> attributes = {
        swss::FieldValueTuple(p4orch::kAction, "set_port_and_src_mac"),
        swss::FieldValueTuple(prependParamField(p4orch::kPort), kPortName1),
        swss::FieldValueTuple(prependParamField(p4orch::kSrcMac), kMacAddress1.to_string()),
    };

    // Invalid json format.
    std::string invalid_key = R"({"match/router_interface_id:intf-3/4"})";
    auto app_db_entry_or = DeserializeRouterIntfEntry(invalid_key, attributes);
    EXPECT_FALSE(app_db_entry_or.ok());

    // Invalid json format.
    invalid_key = R"([{"match/router_interface_id":"intf-3/4"}])";
    app_db_entry_or = DeserializeRouterIntfEntry(invalid_key, attributes);
    EXPECT_FALSE(app_db_entry_or.ok());

    // Invalid json format.
    invalid_key = R"(["match/router_interface_id","intf-3/4"])";
    app_db_entry_or = DeserializeRouterIntfEntry(invalid_key, attributes);
    EXPECT_FALSE(app_db_entry_or.ok());

    // Invalid field name.
    invalid_key = R"({"router_interface_id":"intf-3/4"})";
    app_db_entry_or = DeserializeRouterIntfEntry(invalid_key, attributes);
    EXPECT_FALSE(app_db_entry_or.ok());
}

TEST_F(RouterInterfaceManagerTest, DeserializeRouterIntfEntryMissingAction)
{
    const std::vector<swss::FieldValueTuple> attributes = {
        swss::FieldValueTuple(prependParamField(p4orch::kPort), kPortName1),
        swss::FieldValueTuple(prependParamField(p4orch::kSrcMac), kMacAddress1.to_string()),
    };

    auto app_db_entry_or = DeserializeRouterIntfEntry(kRouterIntfAppDbKey, attributes);
    EXPECT_TRUE(app_db_entry_or.ok());
    auto &app_db_entry = *app_db_entry_or;
    EXPECT_EQ(app_db_entry.router_interface_id, kRouterInterfaceId1);
    EXPECT_EQ(app_db_entry.port_name, kPortName1);
    EXPECT_EQ(app_db_entry.src_mac_address, kMacAddress1);
    EXPECT_TRUE(app_db_entry.is_set_port_name);
    EXPECT_TRUE(app_db_entry.is_set_src_mac);
}

TEST_F(RouterInterfaceManagerTest, DeserializeRouterIntfEntryOnlyPortNameAttribute)
{
    const std::vector<swss::FieldValueTuple> attributes = {
        swss::FieldValueTuple(prependParamField(p4orch::kPort), kPortName1)};

    auto app_db_entry_or = DeserializeRouterIntfEntry(kRouterIntfAppDbKey, attributes);
    EXPECT_TRUE(app_db_entry_or.ok());
    auto &app_db_entry = *app_db_entry_or;
    EXPECT_EQ(app_db_entry.router_interface_id, kRouterInterfaceId1);
    EXPECT_EQ(app_db_entry.port_name, kPortName1);
    EXPECT_EQ(app_db_entry.src_mac_address, kZeroMacAddress);
    EXPECT_TRUE(app_db_entry.is_set_port_name);
    EXPECT_FALSE(app_db_entry.is_set_src_mac);
}

TEST_F(RouterInterfaceManagerTest, DeserializeRouterIntfEntryOnlyMacAddrAttribute)
{
    const std::vector<swss::FieldValueTuple> attributes = {
        swss::FieldValueTuple(prependParamField(p4orch::kSrcMac), kMacAddress1.to_string())};

    auto app_db_entry_or = DeserializeRouterIntfEntry(kRouterIntfAppDbKey, attributes);
    EXPECT_TRUE(app_db_entry_or.ok());
    auto &app_db_entry = *app_db_entry_or;
    EXPECT_EQ(app_db_entry.router_interface_id, kRouterInterfaceId1);
    EXPECT_EQ(app_db_entry.port_name, "");
    EXPECT_EQ(app_db_entry.src_mac_address, kMacAddress1);
    EXPECT_FALSE(app_db_entry.is_set_port_name);
    EXPECT_TRUE(app_db_entry.is_set_src_mac);
}

TEST_F(RouterInterfaceManagerTest, DeserializeRouterIntfEntryNoAttributes)
{
    const std::vector<swss::FieldValueTuple> attributes;

    auto app_db_entry_or = DeserializeRouterIntfEntry(kRouterIntfAppDbKey, attributes);
    EXPECT_TRUE(app_db_entry_or.ok());
    auto &app_db_entry = *app_db_entry_or;
    EXPECT_EQ(app_db_entry.router_interface_id, kRouterInterfaceId1);
    EXPECT_EQ(app_db_entry.port_name, "");
    EXPECT_EQ(app_db_entry.src_mac_address, kZeroMacAddress);
    EXPECT_FALSE(app_db_entry.is_set_port_name);
    EXPECT_FALSE(app_db_entry.is_set_src_mac);
}

TEST_F(RouterInterfaceManagerTest, DeserializeRouterIntfEntryInvalidField)
{
    const std::vector<swss::FieldValueTuple> attributes = {swss::FieldValueTuple("invalid_field", "invalid_value")};

    auto app_db_entry_or = DeserializeRouterIntfEntry(kRouterIntfAppDbKey, attributes);
    EXPECT_FALSE(app_db_entry_or.ok());
}

TEST_F(RouterInterfaceManagerTest, DeserializeRouterIntfEntryInvalidMacAddrValue)
{
    const std::vector<swss::FieldValueTuple> attributes = {
        swss::FieldValueTuple(prependParamField(p4orch::kSrcMac), "00:11:22:33:44")};

    auto app_db_entry_or = DeserializeRouterIntfEntry(kRouterIntfAppDbKey, attributes);
    EXPECT_FALSE(app_db_entry_or.ok());
}

TEST_F(RouterInterfaceManagerTest, DrainValidAttributes)
{
    const std::string appl_db_key =
        std::string(APP_P4RT_ROUTER_INTERFACE_TABLE_NAME) + kTableKeyDelimiter + std::string(kRouterIntfAppDbKey);

    // Enqueue entry for create operation.
    std::vector<swss::FieldValueTuple> attributes;
    attributes.push_back(swss::FieldValueTuple{prependParamField(p4orch::kPort), kPortName1});
    attributes.push_back(swss::FieldValueTuple{prependParamField(p4orch::kSrcMac), kMacAddress1.to_string()});
    Enqueue(swss::KeyOpFieldsValuesTuple(appl_db_key, SET_COMMAND, attributes));

    EXPECT_CALL(mock_sai_router_intf_, create_router_interface(_, _, _, _))
        .WillOnce(DoAll(SetArgPointee<0>(kRouterInterfaceOid1), Return(SAI_STATUS_SUCCESS)));
    Drain();

    P4RouterInterfaceEntry router_intf_entry(kRouterInterfaceId1, kPortName1, kMacAddress1);
    router_intf_entry.router_interface_oid = kRouterInterfaceOid1;
    ValidateRouterInterfaceEntry(router_intf_entry);

    // Enqueue entry for update operation.
    attributes.clear();
    attributes.push_back(swss::FieldValueTuple{prependParamField(p4orch::kSrcMac), kMacAddress2.to_string()});
    Enqueue(swss::KeyOpFieldsValuesTuple(appl_db_key, SET_COMMAND, attributes));

    EXPECT_CALL(mock_sai_router_intf_, set_router_interface_attribute(_, _)).WillOnce(Return(SAI_STATUS_SUCCESS));
    Drain();

    router_intf_entry.src_mac_address = kMacAddress2;
    ValidateRouterInterfaceEntry(router_intf_entry);

    // Enqueue entry for delete operation.
    attributes.clear();
    Enqueue(swss::KeyOpFieldsValuesTuple(appl_db_key, DEL_COMMAND, attributes));

    EXPECT_CALL(mock_sai_router_intf_, remove_router_interface(_)).WillOnce(Return(SAI_STATUS_SUCCESS));
    Drain();

    ValidateRouterInterfaceEntryNotPresent(router_intf_entry.router_interface_id);
}

TEST_F(RouterInterfaceManagerTest, DrainInvalidAppDbEntryKey)
{
    // Create invalid json key with router interface id as kRouterInterfaceId1.
    const std::string invalid_router_intf_key = R"({"match/router_interface_id:intf-3/4"})";
    const std::string appl_db_key =
        std::string(APP_P4RT_ROUTER_INTERFACE_TABLE_NAME) + kTableKeyDelimiter + invalid_router_intf_key;

    // Enqueue entry for create operation.
    std::vector<swss::FieldValueTuple> attributes;
    Enqueue(swss::KeyOpFieldsValuesTuple(appl_db_key, SET_COMMAND, attributes));
    Drain();

    ValidateRouterInterfaceEntryNotPresent(kRouterInterfaceId1);
}

TEST_F(RouterInterfaceManagerTest, DrainInvalidAppDbEntryAttributes)
{
    const std::string appl_db_key =
        std::string(APP_P4RT_ROUTER_INTERFACE_TABLE_NAME) + kTableKeyDelimiter + std::string(kRouterIntfAppDbKey);

    // Invalid port attribute.
    std::vector<swss::FieldValueTuple> attributes;
    attributes.push_back(swss::FieldValueTuple{prependParamField(p4orch::kPort), "xyz"});
    Enqueue(swss::KeyOpFieldsValuesTuple(appl_db_key, SET_COMMAND, attributes));

    // Zero mac address attribute.
    attributes.clear();
    attributes.push_back(swss::FieldValueTuple{prependParamField(p4orch::kPort), kPortName1});
    attributes.push_back(swss::FieldValueTuple{prependParamField(p4orch::kSrcMac), kZeroMacAddress.to_string()});
    Enqueue(swss::KeyOpFieldsValuesTuple(appl_db_key, SET_COMMAND, attributes));

    Drain();
    ValidateRouterInterfaceEntryNotPresent(kRouterInterfaceId1);
}

TEST_F(RouterInterfaceManagerTest, DrainInvalidOperation)
{
    const std::string appl_db_key =
        std::string(APP_P4RT_ROUTER_INTERFACE_TABLE_NAME) + kTableKeyDelimiter + std::string(kRouterIntfAppDbKey);

    // Enqueue entry for invalid operation.
    std::vector<swss::FieldValueTuple> attributes;
    attributes.push_back(swss::FieldValueTuple{prependParamField(p4orch::kPort), kPortName1});
    attributes.push_back(swss::FieldValueTuple{prependParamField(p4orch::kSrcMac), kMacAddress1.to_string()});
    Enqueue(swss::KeyOpFieldsValuesTuple(appl_db_key, "INVALID", attributes));
    Drain();

    ValidateRouterInterfaceEntryNotPresent(kRouterInterfaceId1);
}
