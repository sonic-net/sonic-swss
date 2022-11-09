#include "gre_tunnel_manager.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <functional>
#include <string>
#include <unordered_map>

#include "ipaddress.h"
#include "json.hpp"
#include "mock_response_publisher.h"
#include "mock_sai_router_interface.h"
#include "mock_sai_serialize.h"
#include "mock_sai_tunnel.h"
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
extern sai_tunnel_api_t *sai_tunnel_api;
extern sai_router_interface_api_t *sai_router_intfs_api;
extern MockSaiTunnel *mock_sai_tunnel;

namespace
{
constexpr char *kRouterInterfaceId1 = "intf-eth-1/2/3";
constexpr sai_object_id_t kRouterInterfaceOid1 = 1;
constexpr char *kGreTunnelP4AppDbId1 = "tunnel-1";
constexpr char *kGreTunnelP4AppDbKey1 = R"({"match/tunnel_id":"tunnel-1"})";
constexpr sai_object_id_t kGreTunnelOid1 = 0x11;
constexpr sai_object_id_t kOverlayRifOid1 = 0x101;

// APP DB entries for Add request.
const P4GreTunnelAppDbEntry kP4GreTunnelAppDbEntry1{/*tunnel_id=*/"tunnel-1",
                                                    /*router_interface_id=*/"intf-eth-1/2/3",
                                                    /*encap_src_ip=*/swss::IpAddress("2607:f8b0:8096:3110::1"),
                                                    /*encap_dst_ip=*/swss::IpAddress("2607:f8b0:8096:311a::2"),
                                                    /*action_str=*/"mark_for_p2p_tunnel_encap"};

std::unordered_map<sai_attr_id_t, sai_attribute_value_t> CreateAttributeListForGreTunnelObject(
    const P4GreTunnelAppDbEntry &app_entry, const sai_object_id_t &rif_oid)
{
    std::unordered_map<sai_attr_id_t, sai_attribute_value_t> tunnel_attrs;
    sai_attribute_t tunnel_attr;

    tunnel_attr.id = SAI_TUNNEL_ATTR_TYPE;
    tunnel_attr.value.s32 = SAI_TUNNEL_TYPE_IPINIP_GRE;
    tunnel_attrs.insert({tunnel_attr.id, tunnel_attr.value});

    tunnel_attr.id = SAI_TUNNEL_ATTR_PEER_MODE;
    tunnel_attr.value.s32 = SAI_TUNNEL_PEER_MODE_P2P;
    tunnel_attrs.insert({tunnel_attr.id, tunnel_attr.value});

    tunnel_attr.id = SAI_TUNNEL_ATTR_OVERLAY_INTERFACE;
    tunnel_attr.value.oid = kOverlayRifOid1;
    tunnel_attrs.insert({tunnel_attr.id, tunnel_attr.value});

    tunnel_attr.id = SAI_TUNNEL_ATTR_UNDERLAY_INTERFACE;
    tunnel_attr.value.oid = rif_oid;
    tunnel_attrs.insert({tunnel_attr.id, tunnel_attr.value});

    tunnel_attr.id = SAI_TUNNEL_ATTR_ENCAP_SRC_IP;
    swss::copy(tunnel_attr.value.ipaddr, app_entry.encap_src_ip);
    tunnel_attrs.insert({tunnel_attr.id, tunnel_attr.value});

    tunnel_attr.id = SAI_TUNNEL_ATTR_ENCAP_DST_IP;
    swss::copy(tunnel_attr.value.ipaddr, app_entry.encap_dst_ip);
    tunnel_attrs.insert({tunnel_attr.id, tunnel_attr.value});

    return tunnel_attrs;
}

// Verifies whether the attribute list is the same as expected.
// Returns true if they match; otherwise, false.
bool MatchCreateGreTunnelArgAttrList(const sai_attribute_t *attr_list,
                                     const std::unordered_map<sai_attr_id_t, sai_attribute_value_t> &expected_attr_list)
{
    if (attr_list == nullptr)
    {
        return false;
    }

    // Sanity check for expected_attr_list.
    const auto end = expected_attr_list.end();
    if (expected_attr_list.size() < 3 || expected_attr_list.find(SAI_TUNNEL_ATTR_TYPE) == end ||
        expected_attr_list.find(SAI_TUNNEL_ATTR_PEER_MODE) == end ||
        expected_attr_list.find(SAI_TUNNEL_ATTR_UNDERLAY_INTERFACE) == end ||
        expected_attr_list.find(SAI_TUNNEL_ATTR_OVERLAY_INTERFACE) == end ||
        expected_attr_list.find(SAI_TUNNEL_ATTR_ENCAP_SRC_IP) == end ||
        expected_attr_list.find(SAI_TUNNEL_ATTR_ENCAP_DST_IP) == end)
    {
        return false;
    }

    size_t valid_attrs_num = 0;
    for (size_t i = 0; i < expected_attr_list.size(); ++i)
    {
        switch (attr_list[i].id)
        {
        case SAI_TUNNEL_ATTR_TYPE: {
            if (attr_list[i].value.s32 != expected_attr_list.at(SAI_TUNNEL_ATTR_TYPE).s32)
            {
                return false;
            }
            valid_attrs_num++;
            break;
        }
        case SAI_TUNNEL_ATTR_PEER_MODE: {
            if (attr_list[i].value.s32 != expected_attr_list.at(SAI_TUNNEL_ATTR_PEER_MODE).s32)
            {
                return false;
            }
            valid_attrs_num++;
            break;
        }
        case SAI_TUNNEL_ATTR_ENCAP_SRC_IP: {
            if (attr_list[i].value.ipaddr.addr_family !=
                    expected_attr_list.at(SAI_TUNNEL_ATTR_ENCAP_SRC_IP).ipaddr.addr_family ||
                (attr_list[i].value.ipaddr.addr_family == SAI_IP_ADDR_FAMILY_IPV4 &&
                 attr_list[i].value.ipaddr.addr.ip4 !=
                     expected_attr_list.at(SAI_TUNNEL_ATTR_ENCAP_SRC_IP).ipaddr.addr.ip4) ||
                (attr_list[i].value.ipaddr.addr_family == SAI_IP_ADDR_FAMILY_IPV6 &&
                 memcmp(&attr_list[i].value.ipaddr.addr.ip6,
                        &expected_attr_list.at(SAI_TUNNEL_ATTR_ENCAP_SRC_IP).ipaddr.addr.ip6, sizeof(sai_ip6_t)) != 0))
            {
                return false;
            }
            valid_attrs_num++;
            break;
        }
        case SAI_TUNNEL_ATTR_ENCAP_DST_IP: {
            if (attr_list[i].value.ipaddr.addr_family !=
                    expected_attr_list.at(SAI_TUNNEL_ATTR_ENCAP_DST_IP).ipaddr.addr_family ||
                (attr_list[i].value.ipaddr.addr_family == SAI_IP_ADDR_FAMILY_IPV4 &&
                 attr_list[i].value.ipaddr.addr.ip4 !=
                     expected_attr_list.at(SAI_TUNNEL_ATTR_ENCAP_DST_IP).ipaddr.addr.ip4) ||
                (attr_list[i].value.ipaddr.addr_family == SAI_IP_ADDR_FAMILY_IPV6 &&
                 memcmp(&attr_list[i].value.ipaddr.addr.ip6,
                        &expected_attr_list.at(SAI_TUNNEL_ATTR_ENCAP_DST_IP).ipaddr.addr.ip6, sizeof(sai_ip6_t)) != 0))
            {
                return false;
            }
            valid_attrs_num++;
            break;
        }
        case SAI_TUNNEL_ATTR_UNDERLAY_INTERFACE: {
            if (expected_attr_list.find(SAI_TUNNEL_ATTR_UNDERLAY_INTERFACE) == end ||
                expected_attr_list.at(SAI_TUNNEL_ATTR_UNDERLAY_INTERFACE).oid != attr_list[i].value.oid)
            {
                return false;
            }
            valid_attrs_num++;
            break;
        }
        case SAI_TUNNEL_ATTR_OVERLAY_INTERFACE: {
            if (expected_attr_list.find(SAI_TUNNEL_ATTR_OVERLAY_INTERFACE) == end ||
                expected_attr_list.at(SAI_TUNNEL_ATTR_OVERLAY_INTERFACE).oid != attr_list[i].value.oid)
            {
                return false;
            }
            valid_attrs_num++;
            break;
        }
        default:
            return false;
        }
    }

    if (expected_attr_list.size() != valid_attrs_num)
    {
        return false;
    }

    return true;
}
} // namespace

class GreTunnelManagerTest : public ::testing::Test
{
  protected:
    GreTunnelManagerTest() : gre_tunnel_manager_(&p4_oid_mapper_, &publisher_)
    {
    }

    void SetUp() override
    {
        // Set up mock stuff for SAI tunnel API structure.
        mock_sai_tunnel = &mock_sai_tunnel_;
        sai_tunnel_api->create_tunnel = mock_create_tunnel;
        sai_tunnel_api->remove_tunnel = mock_remove_tunnel;
        // Set up mock stuff for SAI router interface API structure.
        mock_sai_router_intf = &mock_sai_router_intf_;
        sai_router_intfs_api->create_router_interface = mock_create_router_interface;
        sai_router_intfs_api->remove_router_interface = mock_remove_router_interface;

        mock_sai_serialize = &mock_sai_serialize_;
    }

    void Enqueue(const swss::KeyOpFieldsValuesTuple &entry)
    {
        gre_tunnel_manager_.enqueue(entry);
    }

    void Drain()
    {
        gre_tunnel_manager_.drain();
    }

    std::string VerifyState(const std::string &key, const std::vector<swss::FieldValueTuple> &tuple)
    {
        return gre_tunnel_manager_.verifyState(key, tuple);
    }

    ReturnCode ProcessAddRequest(const P4GreTunnelAppDbEntry &app_db_entry)
    {
        return gre_tunnel_manager_.processAddRequest(app_db_entry);
    }

    ReturnCode ProcessDeleteRequest(const std::string &tunnel_key)
    {
        return gre_tunnel_manager_.processDeleteRequest(tunnel_key);
    }

    P4GreTunnelEntry *GetGreTunnelEntry(const std::string &tunnel_key)
    {
        return gre_tunnel_manager_.getGreTunnelEntry(tunnel_key);
    }

    ReturnCodeOr<P4GreTunnelAppDbEntry> DeserializeP4GreTunnelAppDbEntry(
        const std::string &key, const std::vector<swss::FieldValueTuple> &attributes)
    {
        return gre_tunnel_manager_.deserializeP4GreTunnelAppDbEntry(key, attributes);
    }

    // Adds the gre tunnel entry -- kP4GreTunnelAppDbEntry1, via gre tunnel
    // manager's ProcessAddRequest (). This function also takes care of all the
    // dependencies of the gre tunnel entry. Returns a valid pointer to gre tunnel
    // entry on success.
    P4GreTunnelEntry *AddGreTunnelEntry1();

    // Validates that a P4 App gre tunnel entry is correctly added in gre tunnel
    // manager and centralized mapper. Returns true on success.
    bool ValidateGreTunnelEntryAdd(const P4GreTunnelAppDbEntry &app_db_entry);

    // Return true if the specified the object has the expected number of
    // reference.
    bool ValidateRefCnt(sai_object_type_t object_type, const std::string &key, uint32_t expected_ref_count)
    {
        uint32_t ref_count;
        if (!p4_oid_mapper_.getRefCount(object_type, key, &ref_count))
            return false;
        return ref_count == expected_ref_count;
    }

    StrictMock<MockSaiTunnel> mock_sai_tunnel_;
    StrictMock<MockSaiRouterInterface> mock_sai_router_intf_;
    StrictMock<MockSaiSerialize> mock_sai_serialize_;
    MockResponsePublisher publisher_;
    P4OidMapper p4_oid_mapper_;
    GreTunnelManager gre_tunnel_manager_;
};

P4GreTunnelEntry *GreTunnelManagerTest::AddGreTunnelEntry1()
{
    const auto gre_tunnel_key = KeyGenerator::generateTunnelKey(kP4GreTunnelAppDbEntry1.tunnel_id);
    EXPECT_TRUE(p4_oid_mapper_.setOID(SAI_OBJECT_TYPE_ROUTER_INTERFACE,
                                      KeyGenerator::generateRouterInterfaceKey(kRouterInterfaceId1),
                                      kRouterInterfaceOid1));

    // Set up mock call.
    EXPECT_CALL(mock_sai_router_intf_, create_router_interface(::testing::NotNull(), Eq(gSwitchId), Eq(2), _))
        .WillOnce(DoAll(SetArgPointee<0>(kOverlayRifOid1), Return(SAI_STATUS_SUCCESS)));

    EXPECT_CALL(mock_sai_tunnel_, create_tunnel(::testing::NotNull(), Eq(gSwitchId), Eq(6),
                                                Truly(std::bind(MatchCreateGreTunnelArgAttrList, std::placeholders::_1,
                                                                CreateAttributeListForGreTunnelObject(
                                                                    kP4GreTunnelAppDbEntry1, kRouterInterfaceOid1)))))
        .WillOnce(DoAll(SetArgPointee<0>(kGreTunnelOid1), Return(SAI_STATUS_SUCCESS)));

    EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS, ProcessAddRequest(kP4GreTunnelAppDbEntry1));

    return GetGreTunnelEntry(gre_tunnel_key);
}

bool GreTunnelManagerTest::ValidateGreTunnelEntryAdd(const P4GreTunnelAppDbEntry &app_db_entry)
{
    const auto *p4_gre_tunnel_entry = GetGreTunnelEntry(KeyGenerator::generateTunnelKey(app_db_entry.tunnel_id));
    if (p4_gre_tunnel_entry == nullptr || p4_gre_tunnel_entry->encap_src_ip != app_db_entry.encap_src_ip ||
        p4_gre_tunnel_entry->encap_dst_ip != app_db_entry.encap_dst_ip ||
        p4_gre_tunnel_entry->neighbor_id != app_db_entry.encap_dst_ip ||
        p4_gre_tunnel_entry->router_interface_id != app_db_entry.router_interface_id ||
        p4_gre_tunnel_entry->tunnel_id != app_db_entry.tunnel_id)
    {
        return false;
    }

    return true;
}

TEST_F(GreTunnelManagerTest, ProcessAddRequestShouldSucceedAddingNewGreTunnel)
{
    AddGreTunnelEntry1();
    EXPECT_TRUE(ValidateGreTunnelEntryAdd(kP4GreTunnelAppDbEntry1));
}

TEST_F(GreTunnelManagerTest, ProcessAddRequestShouldFailWhenGreTunnelExistInCentralMapper)
{
    const auto gre_tunnel_key = KeyGenerator::generateTunnelKey(kP4GreTunnelAppDbEntry1.tunnel_id);
    ASSERT_EQ(gre_tunnel_key, "tunnel_id=tunnel-1");
    ASSERT_TRUE(p4_oid_mapper_.setOID(SAI_OBJECT_TYPE_TUNNEL, gre_tunnel_key, kGreTunnelOid1));
    // TODO: Expect critical state.
    EXPECT_EQ(StatusCode::SWSS_RC_INTERNAL, ProcessAddRequest(kP4GreTunnelAppDbEntry1));
}

TEST_F(GreTunnelManagerTest, ProcessAddRequestShouldFailWhenDependingPortIsNotPresent)
{
    const P4GreTunnelAppDbEntry kAppDbEntry{/*tunnel_id=*/"tunnel-1",
                                            /*router_interface_id=*/"intf-eth-1/2/3",
                                            /*encap_src_ip=*/swss::IpAddress("2607:f8b0:8096:3110::1"),
                                            /*encap_dst_ip=*/swss::IpAddress("2607:f8b0:8096:311a::2"),
                                            /*action_str=*/"mark_for_p2p_tunnel_encap"};
    const auto gre_tunnel_key = KeyGenerator::generateTunnelKey(kAppDbEntry.tunnel_id);

    EXPECT_EQ(StatusCode::SWSS_RC_NOT_FOUND, ProcessAddRequest(kAppDbEntry));

    EXPECT_EQ(GetGreTunnelEntry(gre_tunnel_key), nullptr);
}

TEST_F(GreTunnelManagerTest, ProcessAddRequestShouldFailWhenRifSaiCallFails)
{
    const auto gre_tunnel_key = KeyGenerator::generateTunnelKey(kP4GreTunnelAppDbEntry1.tunnel_id);
    EXPECT_TRUE(p4_oid_mapper_.setOID(SAI_OBJECT_TYPE_ROUTER_INTERFACE,
                                      KeyGenerator::generateRouterInterfaceKey(kRouterInterfaceId1),
                                      kRouterInterfaceOid1));
    // Set up mock call.
    EXPECT_CALL(mock_sai_router_intf_, create_router_interface(::testing::NotNull(), Eq(gSwitchId), Eq(2), _))
        .WillOnce(DoAll(SetArgPointee<0>(kOverlayRifOid1), Return(SAI_STATUS_FAILURE)));

    EXPECT_EQ(StatusCode::SWSS_RC_UNKNOWN, ProcessAddRequest(kP4GreTunnelAppDbEntry1));

    // The add request failed for the gre tunnel entry.
    EXPECT_EQ(GetGreTunnelEntry(gre_tunnel_key), nullptr);
}

TEST_F(GreTunnelManagerTest, ProcessAddRequestShouldFailWhenTunnelSaiCallFails)
{
    const auto gre_tunnel_key = KeyGenerator::generateTunnelKey(kP4GreTunnelAppDbEntry1.tunnel_id);
    EXPECT_TRUE(p4_oid_mapper_.setOID(SAI_OBJECT_TYPE_ROUTER_INTERFACE,
                                      KeyGenerator::generateRouterInterfaceKey(kRouterInterfaceId1),
                                      kRouterInterfaceOid1));
    // Set up mock call.
    EXPECT_CALL(mock_sai_router_intf_, create_router_interface(::testing::NotNull(), Eq(gSwitchId), Eq(2), _))
        .WillOnce(DoAll(SetArgPointee<0>(kOverlayRifOid1), Return(SAI_STATUS_SUCCESS)));
    EXPECT_CALL(mock_sai_tunnel_, create_tunnel(::testing::NotNull(), Eq(gSwitchId), Eq(6),
                                                Truly(std::bind(MatchCreateGreTunnelArgAttrList, std::placeholders::_1,
                                                                CreateAttributeListForGreTunnelObject(
                                                                    kP4GreTunnelAppDbEntry1, kRouterInterfaceOid1)))))
        .WillOnce(Return(SAI_STATUS_FAILURE));
    EXPECT_CALL(mock_sai_router_intf_, remove_router_interface(Eq(kOverlayRifOid1)))
        .WillOnce(Return(SAI_STATUS_SUCCESS));

    EXPECT_EQ(StatusCode::SWSS_RC_UNKNOWN, ProcessAddRequest(kP4GreTunnelAppDbEntry1));

    // The add request failed for the gre tunnel entry.
    EXPECT_EQ(GetGreTunnelEntry(gre_tunnel_key), nullptr);
}

TEST_F(GreTunnelManagerTest, ProcessAddRequestShouldRaiseCriticalWhenRecoverySaiCallFails)
{
    const auto gre_tunnel_key = KeyGenerator::generateTunnelKey(kP4GreTunnelAppDbEntry1.tunnel_id);
    EXPECT_TRUE(p4_oid_mapper_.setOID(SAI_OBJECT_TYPE_ROUTER_INTERFACE,
                                      KeyGenerator::generateRouterInterfaceKey(kRouterInterfaceId1),
                                      kRouterInterfaceOid1));
    // Set up mock call.
    EXPECT_CALL(mock_sai_router_intf_, create_router_interface(::testing::NotNull(), Eq(gSwitchId), Eq(2), _))
        .WillOnce(DoAll(SetArgPointee<0>(kOverlayRifOid1), Return(SAI_STATUS_SUCCESS)));
    EXPECT_CALL(mock_sai_tunnel_, create_tunnel(::testing::NotNull(), Eq(gSwitchId), Eq(6),
                                                Truly(std::bind(MatchCreateGreTunnelArgAttrList, std::placeholders::_1,
                                                                CreateAttributeListForGreTunnelObject(
                                                                    kP4GreTunnelAppDbEntry1, kRouterInterfaceOid1)))))
        .WillOnce(Return(SAI_STATUS_FAILURE));
    EXPECT_CALL(mock_sai_router_intf_, remove_router_interface(Eq(kOverlayRifOid1)))
        .WillOnce(Return(SAI_STATUS_FAILURE));
    // TODO: Expect critical state.

    EXPECT_EQ(StatusCode::SWSS_RC_UNKNOWN, ProcessAddRequest(kP4GreTunnelAppDbEntry1));

    // The add request failed for the gre tunnel entry.
    EXPECT_EQ(GetGreTunnelEntry(gre_tunnel_key), nullptr);
}

TEST_F(GreTunnelManagerTest, ProcessDeleteRequestShouldFailForNonExistingGreTunnel)
{
    const auto gre_tunnel_key = KeyGenerator::generateTunnelKey(kP4GreTunnelAppDbEntry1.tunnel_id);
    EXPECT_EQ(StatusCode::SWSS_RC_NOT_FOUND, ProcessDeleteRequest(gre_tunnel_key));
}

TEST_F(GreTunnelManagerTest, ProcessDeleteRequestShouldFailIfGreTunnelEntryIsAbsentInCentralMapper)
{
    auto *p4_tunnel_entry = AddGreTunnelEntry1();
    ASSERT_NE(p4_tunnel_entry, nullptr);

    const auto gre_tunnel_key = KeyGenerator::generateTunnelKey(kP4GreTunnelAppDbEntry1.tunnel_id);

    ASSERT_TRUE(p4_oid_mapper_.eraseOID(SAI_OBJECT_TYPE_TUNNEL, gre_tunnel_key));

    // TODO: Expect critical state.
    EXPECT_EQ(StatusCode::SWSS_RC_INTERNAL, ProcessDeleteRequest(gre_tunnel_key));

    // Validate the gre tunnel entry is not deleted in P4 gre tunnel manager.
    p4_tunnel_entry = GetGreTunnelEntry(gre_tunnel_key);
    ASSERT_NE(p4_tunnel_entry, nullptr);
}

TEST_F(GreTunnelManagerTest, ProcessDeleteRequestShouldFailIfGreTunnelEntryIsStillReferenced)
{
    auto *p4_tunnel_entry = AddGreTunnelEntry1();
    ASSERT_NE(p4_tunnel_entry, nullptr);

    const auto gre_tunnel_key = KeyGenerator::generateTunnelKey(kP4GreTunnelAppDbEntry1.tunnel_id);
    ASSERT_TRUE(p4_oid_mapper_.increaseRefCount(SAI_OBJECT_TYPE_TUNNEL, gre_tunnel_key));

    EXPECT_EQ(StatusCode::SWSS_RC_INVALID_PARAM, ProcessDeleteRequest(gre_tunnel_key));

    // Validate the gre tunnel entry is not deleted in either P4 gre tunnel
    // manager or central mapper.
    p4_tunnel_entry = GetGreTunnelEntry(gre_tunnel_key);
    ASSERT_NE(p4_tunnel_entry, nullptr);
    EXPECT_TRUE(ValidateRefCnt(SAI_OBJECT_TYPE_TUNNEL, gre_tunnel_key, 1));
}

TEST_F(GreTunnelManagerTest, ProcessDeleteRequestShouldFailIfTunnelSaiCallFails)
{
    auto *p4_tunnel_entry = AddGreTunnelEntry1();
    ASSERT_NE(p4_tunnel_entry, nullptr);

    const auto gre_tunnel_key = KeyGenerator::generateTunnelKey(kP4GreTunnelAppDbEntry1.tunnel_id);

    // Set up mock call.
    EXPECT_CALL(mock_sai_tunnel_, remove_tunnel(Eq(p4_tunnel_entry->tunnel_oid))).WillOnce(Return(SAI_STATUS_FAILURE));

    EXPECT_EQ(StatusCode::SWSS_RC_UNKNOWN, ProcessDeleteRequest(gre_tunnel_key));

    // Validate the gre tunnel entry is not deleted in either P4 gre tunnel
    // manager or central mapper.
    p4_tunnel_entry = GetGreTunnelEntry(gre_tunnel_key);
    ASSERT_NE(p4_tunnel_entry, nullptr);
    EXPECT_TRUE(p4_oid_mapper_.existsOID(SAI_OBJECT_TYPE_TUNNEL, gre_tunnel_key));
}

TEST_F(GreTunnelManagerTest, ProcessDeleteRequestShouldFailIfRifSaiCallFails)
{
    auto *p4_tunnel_entry = AddGreTunnelEntry1();
    ASSERT_NE(p4_tunnel_entry, nullptr);

    const auto gre_tunnel_key = KeyGenerator::generateTunnelKey(kP4GreTunnelAppDbEntry1.tunnel_id);

    // Set up mock call.
    EXPECT_CALL(mock_sai_tunnel_, remove_tunnel(Eq(p4_tunnel_entry->tunnel_oid))).WillOnce(Return(SAI_STATUS_SUCCESS));
    EXPECT_CALL(mock_sai_router_intf_, remove_router_interface(Eq(kOverlayRifOid1)))
        .WillOnce(Return(SAI_STATUS_FAILURE));
    EXPECT_CALL(mock_sai_tunnel_, create_tunnel(::testing::NotNull(), Eq(gSwitchId), Eq(6),
                                                Truly(std::bind(MatchCreateGreTunnelArgAttrList, std::placeholders::_1,
                                                                CreateAttributeListForGreTunnelObject(
                                                                    kP4GreTunnelAppDbEntry1, kRouterInterfaceOid1)))))
        .WillOnce(DoAll(SetArgPointee<0>(kGreTunnelOid1), Return(SAI_STATUS_SUCCESS)));

    EXPECT_EQ(StatusCode::SWSS_RC_UNKNOWN, ProcessDeleteRequest(gre_tunnel_key));

    // Validate the gre tunnel entry is not deleted in either P4 gre tunnel
    // manager or central mapper.
    p4_tunnel_entry = GetGreTunnelEntry(gre_tunnel_key);
    ASSERT_NE(p4_tunnel_entry, nullptr);
    EXPECT_TRUE(p4_oid_mapper_.existsOID(SAI_OBJECT_TYPE_TUNNEL, gre_tunnel_key));
}

TEST_F(GreTunnelManagerTest, ProcessDeleteRequestShouldRaiseCriticalIfRecoverySaiCallFails)
{
    auto *p4_tunnel_entry = AddGreTunnelEntry1();
    ASSERT_NE(p4_tunnel_entry, nullptr);

    const auto gre_tunnel_key = KeyGenerator::generateTunnelKey(kP4GreTunnelAppDbEntry1.tunnel_id);

    // Set up mock call.
    EXPECT_CALL(mock_sai_tunnel_, remove_tunnel(Eq(p4_tunnel_entry->tunnel_oid))).WillOnce(Return(SAI_STATUS_SUCCESS));
    EXPECT_CALL(mock_sai_router_intf_, remove_router_interface(Eq(kOverlayRifOid1)))
        .WillOnce(Return(SAI_STATUS_FAILURE));
    EXPECT_CALL(mock_sai_tunnel_, create_tunnel(::testing::NotNull(), Eq(gSwitchId), Eq(6),
                                                Truly(std::bind(MatchCreateGreTunnelArgAttrList, std::placeholders::_1,
                                                                CreateAttributeListForGreTunnelObject(
                                                                    kP4GreTunnelAppDbEntry1, kRouterInterfaceOid1)))))
        .WillOnce(Return(SAI_STATUS_FAILURE));

    // TODO: Expect critical state.

    EXPECT_EQ(StatusCode::SWSS_RC_UNKNOWN, ProcessDeleteRequest(gre_tunnel_key));

    // Validate the gre tunnel entry is not deleted in either P4 gre tunnel
    // manager or central mapper.
    p4_tunnel_entry = GetGreTunnelEntry(gre_tunnel_key);
    ASSERT_NE(p4_tunnel_entry, nullptr);
    EXPECT_TRUE(p4_oid_mapper_.existsOID(SAI_OBJECT_TYPE_TUNNEL, gre_tunnel_key));
}

TEST_F(GreTunnelManagerTest, GetGreTunnelEntryShouldReturnNullPointerForNonexistingGreTunnel)
{
    const auto gre_tunnel_key = KeyGenerator::generateTunnelKey(kP4GreTunnelAppDbEntry1.tunnel_id);
    EXPECT_EQ(GetGreTunnelEntry(gre_tunnel_key), nullptr);
}

TEST_F(GreTunnelManagerTest, DeserializeP4GreTunnelAppDbEntryShouldReturnNullPointerForInvalidField)
{
    std::vector<swss::FieldValueTuple> attributes = {swss::FieldValueTuple(p4orch::kAction, p4orch::kTunnelAction),
                                                     swss::FieldValueTuple("UNKNOWN_FIELD", "UNKOWN")};

    EXPECT_FALSE(DeserializeP4GreTunnelAppDbEntry(kGreTunnelP4AppDbKey1, attributes).ok());
}

TEST_F(GreTunnelManagerTest, DeserializeP4GreTunnelAppDbEntryShouldReturnNullPointerForInvalidIP)
{
    std::vector<swss::FieldValueTuple> attributes = {
        swss::FieldValueTuple(p4orch::kAction, p4orch::kTunnelAction),
        swss::FieldValueTuple(prependParamField(p4orch::kRouterInterfaceId), kRouterInterfaceId1),
        swss::FieldValueTuple(prependParamField(p4orch::kEncapSrcIp), "1.2.3.4"),
        swss::FieldValueTuple(prependParamField(p4orch::kEncapDstIp), "2.3.4.5")};
    EXPECT_TRUE(DeserializeP4GreTunnelAppDbEntry(kGreTunnelP4AppDbKey1, attributes).ok());
    attributes = {swss::FieldValueTuple(p4orch::kAction, p4orch::kTunnelAction),
                  swss::FieldValueTuple(prependParamField(p4orch::kRouterInterfaceId), kRouterInterfaceId1),
                  swss::FieldValueTuple(prependParamField(p4orch::kEncapSrcIp), "1:2:3:4"),
                  swss::FieldValueTuple(prependParamField(p4orch::kEncapDstIp), "1.2.3.5")};
    EXPECT_FALSE(DeserializeP4GreTunnelAppDbEntry(kGreTunnelP4AppDbKey1, attributes).ok());
    attributes = {swss::FieldValueTuple(p4orch::kAction, p4orch::kTunnelAction),
                  swss::FieldValueTuple(prependParamField(p4orch::kRouterInterfaceId), kRouterInterfaceId1),
                  swss::FieldValueTuple(prependParamField(p4orch::kEncapSrcIp), "1.2.3.4"),
                  swss::FieldValueTuple(prependParamField(p4orch::kEncapDstIp), "1:2:3:5")};
    EXPECT_FALSE(DeserializeP4GreTunnelAppDbEntry(kGreTunnelP4AppDbKey1, attributes).ok());
}

TEST_F(GreTunnelManagerTest, DeserializeP4GreTunnelAppDbEntryShouldReturnNullPointerForInvalidKey)
{
    std::vector<swss::FieldValueTuple> attributes = {
        {p4orch::kAction, p4orch::kTunnelAction},
        {prependParamField(p4orch::kRouterInterfaceId), kP4GreTunnelAppDbEntry1.router_interface_id},
        {prependParamField(p4orch::kEncapSrcIp), kP4GreTunnelAppDbEntry1.encap_src_ip.to_string()},
        {prependParamField(p4orch::kEncapDstIp), kP4GreTunnelAppDbEntry1.encap_dst_ip.to_string()}};
    constexpr char *kInvalidAppDbKey = R"({"tunnel_id":1})";
    EXPECT_FALSE(DeserializeP4GreTunnelAppDbEntry(kInvalidAppDbKey, attributes).ok());
}

TEST_F(GreTunnelManagerTest, DrainDuplicateSetRequestShouldSucceed)
{
    auto *p4_tunnel_entry = AddGreTunnelEntry1();
    ASSERT_NE(p4_tunnel_entry, nullptr);

    nlohmann::json j;
    j[prependMatchField(p4orch::kTunnelId)] = kP4GreTunnelAppDbEntry1.tunnel_id;

    std::vector<swss::FieldValueTuple> fvs{
        {p4orch::kAction, p4orch::kTunnelAction},
        {prependParamField(p4orch::kRouterInterfaceId), kP4GreTunnelAppDbEntry1.router_interface_id},
        {prependParamField(p4orch::kEncapSrcIp), kP4GreTunnelAppDbEntry1.encap_src_ip.to_string()},
        {prependParamField(p4orch::kEncapDstIp), kP4GreTunnelAppDbEntry1.encap_dst_ip.to_string()}};

    swss::KeyOpFieldsValuesTuple app_db_entry(std::string(APP_P4RT_TUNNEL_TABLE_NAME) + kTableKeyDelimiter + j.dump(),
                                              SET_COMMAND, fvs);

    Enqueue(app_db_entry);
    Drain();

    // Expect that the update call will fail, so gre tunnel entry's fields stay
    // the same.
    EXPECT_TRUE(ValidateGreTunnelEntryAdd(kP4GreTunnelAppDbEntry1));
}

TEST_F(GreTunnelManagerTest, DrainDeleteRequestShouldSucceedForExistingGreTunnel)
{
    auto *p4_tunnel_entry = AddGreTunnelEntry1();
    ASSERT_NE(p4_tunnel_entry, nullptr);
    EXPECT_EQ(p4_tunnel_entry->tunnel_oid, kGreTunnelOid1);

    nlohmann::json j;
    j[prependMatchField(p4orch::kTunnelId)] = kP4GreTunnelAppDbEntry1.tunnel_id;

    std::vector<swss::FieldValueTuple> fvs;
    swss::KeyOpFieldsValuesTuple app_db_entry(std::string(APP_P4RT_TUNNEL_TABLE_NAME) + kTableKeyDelimiter + j.dump(),
                                              DEL_COMMAND, fvs);
    EXPECT_CALL(mock_sai_router_intf_, remove_router_interface(Eq(kOverlayRifOid1)))
        .WillOnce(Return(SAI_STATUS_SUCCESS));
    EXPECT_CALL(mock_sai_tunnel_, remove_tunnel(Eq(p4_tunnel_entry->tunnel_oid))).WillOnce(Return(SAI_STATUS_SUCCESS));

    Enqueue(app_db_entry);
    Drain();

    // Validate the gre tunnel entry has been deleted in both P4 gre tunnel
    // manager and centralized mapper.
    const auto gre_tunnel_key = KeyGenerator::generateTunnelKey(kP4GreTunnelAppDbEntry1.tunnel_id);
    p4_tunnel_entry = GetGreTunnelEntry(gre_tunnel_key);
    EXPECT_EQ(p4_tunnel_entry, nullptr);
    EXPECT_FALSE(p4_oid_mapper_.existsOID(SAI_OBJECT_TYPE_TUNNEL, gre_tunnel_key));
}

TEST_F(GreTunnelManagerTest, DrainValidAppEntryShouldSucceed)
{
    nlohmann::json j;
    j[prependMatchField(p4orch::kTunnelId)] = kGreTunnelP4AppDbId1;
    EXPECT_TRUE(p4_oid_mapper_.setOID(SAI_OBJECT_TYPE_ROUTER_INTERFACE,
                                      KeyGenerator::generateRouterInterfaceKey(kRouterInterfaceId1),
                                      kRouterInterfaceOid1));

    std::vector<swss::FieldValueTuple> fvs{
        {p4orch::kAction, p4orch::kTunnelAction},
        {prependParamField(p4orch::kRouterInterfaceId), kP4GreTunnelAppDbEntry1.router_interface_id},
        {prependParamField(p4orch::kEncapSrcIp), kP4GreTunnelAppDbEntry1.encap_src_ip.to_string()},
        {prependParamField(p4orch::kEncapDstIp), kP4GreTunnelAppDbEntry1.encap_dst_ip.to_string()}};

    swss::KeyOpFieldsValuesTuple app_db_entry(std::string(APP_P4RT_TUNNEL_TABLE_NAME) + kTableKeyDelimiter + j.dump(),
                                              SET_COMMAND, fvs);

    Enqueue(app_db_entry);
    EXPECT_CALL(mock_sai_router_intf_, create_router_interface(::testing::NotNull(), Eq(gSwitchId), Eq(2), _))
        .WillOnce(DoAll(SetArgPointee<0>(kOverlayRifOid1), Return(SAI_STATUS_SUCCESS)));

    EXPECT_CALL(mock_sai_tunnel_, create_tunnel(_, _, _, _))
        .WillOnce(DoAll(SetArgPointee<0>(kGreTunnelOid1), Return(SAI_STATUS_SUCCESS)));

    Drain();

    EXPECT_TRUE(ValidateGreTunnelEntryAdd(kP4GreTunnelAppDbEntry1));
}

TEST_F(GreTunnelManagerTest, DrainInvalidAppEntryShouldFail)
{
    nlohmann::json j;
    j[prependMatchField(p4orch::kTunnelId)] = kGreTunnelP4AppDbId1;
    j[p4orch::kTunnelId] = 1000;

    std::vector<swss::FieldValueTuple> fvs{
        {p4orch::kAction, p4orch::kTunnelAction},
        {prependParamField(p4orch::kRouterInterfaceId), kP4GreTunnelAppDbEntry1.router_interface_id},
        {prependParamField(p4orch::kEncapSrcIp), "1"},
        {prependParamField(p4orch::kEncapDstIp), kP4GreTunnelAppDbEntry1.encap_dst_ip.to_string()}};

    swss::KeyOpFieldsValuesTuple app_db_entry(std::string(APP_P4RT_TUNNEL_TABLE_NAME) + kTableKeyDelimiter + j.dump(),
                                              SET_COMMAND, fvs);

    Enqueue(app_db_entry);

    Drain();
    EXPECT_EQ(GetGreTunnelEntry(kGreTunnelP4AppDbKey1), nullptr);
    EXPECT_FALSE(p4_oid_mapper_.existsOID(SAI_OBJECT_TYPE_TUNNEL, kGreTunnelP4AppDbKey1));

    // Invalid action_str
    fvs = {{p4orch::kAction, "set_nexthop"},
           {prependParamField(p4orch::kRouterInterfaceId), kP4GreTunnelAppDbEntry1.router_interface_id},
           {prependParamField(p4orch::kEncapSrcIp), kP4GreTunnelAppDbEntry1.encap_src_ip.to_string()},
           {prependParamField(p4orch::kEncapDstIp), kP4GreTunnelAppDbEntry1.encap_dst_ip.to_string()}};

    app_db_entry = {std::string(APP_P4RT_TUNNEL_TABLE_NAME) + kTableKeyDelimiter + j.dump(), SET_COMMAND, fvs};

    Enqueue(app_db_entry);

    Drain();
    EXPECT_EQ(GetGreTunnelEntry(kGreTunnelP4AppDbKey1), nullptr);
    EXPECT_FALSE(p4_oid_mapper_.existsOID(SAI_OBJECT_TYPE_TUNNEL, kGreTunnelP4AppDbKey1));

    // Miss action
    fvs = {{prependParamField(p4orch::kRouterInterfaceId), kP4GreTunnelAppDbEntry1.router_interface_id},
           {prependParamField(p4orch::kEncapSrcIp), kP4GreTunnelAppDbEntry1.encap_src_ip.to_string()},
           {prependParamField(p4orch::kEncapDstIp), kP4GreTunnelAppDbEntry1.encap_dst_ip.to_string()}};

    app_db_entry = {std::string(APP_P4RT_TUNNEL_TABLE_NAME) + kTableKeyDelimiter + j.dump(), SET_COMMAND, fvs};

    Enqueue(app_db_entry);

    Drain();
    EXPECT_EQ(GetGreTunnelEntry(kGreTunnelP4AppDbKey1), nullptr);
    EXPECT_FALSE(p4_oid_mapper_.existsOID(SAI_OBJECT_TYPE_TUNNEL, kGreTunnelP4AppDbKey1));

    // Miss router_interface_id
    fvs = {{p4orch::kAction, p4orch::kTunnelAction},
           {prependParamField(p4orch::kEncapSrcIp), kP4GreTunnelAppDbEntry1.encap_src_ip.to_string()},
           {prependParamField(p4orch::kEncapDstIp), kP4GreTunnelAppDbEntry1.encap_dst_ip.to_string()}};

    app_db_entry = {std::string(APP_P4RT_TUNNEL_TABLE_NAME) + kTableKeyDelimiter + j.dump(), SET_COMMAND, fvs};

    Enqueue(app_db_entry);

    Drain();
    EXPECT_EQ(GetGreTunnelEntry(kGreTunnelP4AppDbKey1), nullptr);
    EXPECT_FALSE(p4_oid_mapper_.existsOID(SAI_OBJECT_TYPE_TUNNEL, kGreTunnelP4AppDbKey1));

    // Miss encap_src_ip
    fvs = {{p4orch::kAction, p4orch::kTunnelAction},
           {prependParamField(p4orch::kRouterInterfaceId), kP4GreTunnelAppDbEntry1.router_interface_id},
           {prependParamField(p4orch::kEncapDstIp), kP4GreTunnelAppDbEntry1.encap_dst_ip.to_string()}};

    app_db_entry = {std::string(APP_P4RT_TUNNEL_TABLE_NAME) + kTableKeyDelimiter + j.dump(), SET_COMMAND, fvs};

    Enqueue(app_db_entry);

    Drain();
    EXPECT_EQ(GetGreTunnelEntry(kGreTunnelP4AppDbKey1), nullptr);
    EXPECT_FALSE(p4_oid_mapper_.existsOID(SAI_OBJECT_TYPE_TUNNEL, kGreTunnelP4AppDbKey1));

    // Miss encap_dst_ip
    fvs = {{p4orch::kAction, p4orch::kTunnelAction},
           {prependParamField(p4orch::kRouterInterfaceId), kP4GreTunnelAppDbEntry1.router_interface_id},
           {prependParamField(p4orch::kEncapSrcIp), kP4GreTunnelAppDbEntry1.encap_src_ip.to_string()}};

    app_db_entry = {std::string(APP_P4RT_TUNNEL_TABLE_NAME) + kTableKeyDelimiter + j.dump(), SET_COMMAND, fvs};

    Enqueue(app_db_entry);

    Drain();
    EXPECT_EQ(GetGreTunnelEntry(kGreTunnelP4AppDbKey1), nullptr);
    EXPECT_FALSE(p4_oid_mapper_.existsOID(SAI_OBJECT_TYPE_TUNNEL, kGreTunnelP4AppDbKey1));
}

TEST_F(GreTunnelManagerTest, VerifyStateTest)
{
    auto *p4_tunnel_entry = AddGreTunnelEntry1();
    ASSERT_NE(p4_tunnel_entry, nullptr);

    // Setup ASIC DB.
    swss::Table table(nullptr, "ASIC_STATE");
    table.set("SAI_OBJECT_TYPE_TUNNEL:oid:0x11",
              std::vector<swss::FieldValueTuple>{
                  swss::FieldValueTuple{"SAI_TUNNEL_ATTR_TYPE", "SAI_TUNNEL_TYPE_IPINIP_GRE"},
                  swss::FieldValueTuple{"SAI_TUNNEL_ATTR_PEER_MODE", "SAI_TUNNEL_PEER_MODE_P2P"},
                  swss::FieldValueTuple{"SAI_TUNNEL_ATTR_ENCAP_SRC_IP", "2607:f8b0:8096:3110::1"},
                  swss::FieldValueTuple{"SAI_TUNNEL_ATTR_ENCAP_DST_IP", "2607:f8b0:8096:311a::2"},
                  swss::FieldValueTuple{"SAI_TUNNEL_ATTR_UNDERLAY_INTERFACE", "oid:0x1"},
                  swss::FieldValueTuple{"SAI_TUNNEL_ATTR_OVERLAY_INTERFACE", "oid:0x101"}});

    // Overlay router interface
    table.set("SAI_OBJECT_TYPE_ROUTER_INTERFACE:oid:0x101",
              std::vector<swss::FieldValueTuple>{
                  swss::FieldValueTuple{"SAI_ROUTER_INTERFACE_ATTR_VIRTUAL_ROUTER_ID", "oid:0x0"},
                  swss::FieldValueTuple{"SAI_ROUTER_INTERFACE_ATTR_TYPE", "SAI_ROUTER_INTERFACE_TYPE_LOOPBACK"}});

    // Underlay router interface
    table.set("SAI_OBJECT_TYPE_ROUTER_INTERFACE:oid:0x1",
              std::vector<swss::FieldValueTuple>{
                  swss::FieldValueTuple{"SAI_ROUTER_INTERFACE_ATTR_VIRTUAL_ROUTER_ID", "oid:0x0"},
                  swss::FieldValueTuple{"SAI_ROUTER_INTERFACE_ATTR_SRC_MAC_ADDRESS", "00:01:02:03:04:05"},
                  swss::FieldValueTuple{"SAI_ROUTER_INTERFACE_ATTR_TYPE", "SAI_ROUTER_INTERFACE_TYPE_PORT"},
                  swss::FieldValueTuple{"SAI_ROUTER_INTERFACE_ATTR_PORT_ID", "oid:0x1234"},
                  swss::FieldValueTuple{"SAI_ROUTER_INTERFACE_ATTR_MTU", "9100"}});

    nlohmann::json j;
    j[prependMatchField(p4orch::kTunnelId)] = kGreTunnelP4AppDbId1;
    const std::string db_key = std::string(APP_P4RT_TABLE_NAME) + kTableKeyDelimiter + APP_P4RT_TUNNEL_TABLE_NAME +
                               kTableKeyDelimiter + j.dump();
    std::vector<swss::FieldValueTuple> attributes;

    // Verification should succeed with vaild key and value.
    attributes.push_back(swss::FieldValueTuple{p4orch::kAction, p4orch::kTunnelAction});
    attributes.push_back(swss::FieldValueTuple{prependParamField(p4orch::kRouterInterfaceId),
                                               kP4GreTunnelAppDbEntry1.router_interface_id});
    attributes.push_back(swss::FieldValueTuple{prependParamField(p4orch::kEncapSrcIp),
                                               kP4GreTunnelAppDbEntry1.encap_src_ip.to_string()});
    attributes.push_back(swss::FieldValueTuple{prependParamField(p4orch::kEncapDstIp),
                                               kP4GreTunnelAppDbEntry1.encap_dst_ip.to_string()});
    EXPECT_EQ(VerifyState(db_key, attributes), "");

    // Invalid key should fail verification.
    EXPECT_FALSE(VerifyState("invalid", attributes).empty());
    EXPECT_FALSE(VerifyState("invalid:invalid", attributes).empty());
    EXPECT_FALSE(VerifyState(std::string(APP_P4RT_TABLE_NAME) + ":invalid", attributes).empty());
    EXPECT_FALSE(VerifyState(std::string(APP_P4RT_TABLE_NAME) + ":invalid:invalid", attributes).empty());
    EXPECT_FALSE(VerifyState(std::string(APP_P4RT_TABLE_NAME) + ":FIXED_TUNNEL_TABLE:invalid", attributes).empty());

    // Verification should fail if entry does not exist.
    j[prependMatchField(p4orch::kTunnelId)] = "invalid";
    EXPECT_FALSE(VerifyState(std::string(APP_P4RT_TABLE_NAME) + kTableKeyDelimiter + APP_P4RT_TUNNEL_TABLE_NAME +
                                 kTableKeyDelimiter + j.dump(),
                             attributes)
                     .empty());

    // Verification should fail if router interface name mismatches.
    auto saved_router_interface_id = p4_tunnel_entry->router_interface_id;
    p4_tunnel_entry->router_interface_id = "invalid";
    EXPECT_FALSE(VerifyState(db_key, attributes).empty());
    p4_tunnel_entry->router_interface_id = saved_router_interface_id;

    // Verification should fail if tunnel key mismatches.
    auto saved_tunnel_key = p4_tunnel_entry->tunnel_key;
    p4_tunnel_entry->tunnel_key = "invalid";
    EXPECT_FALSE(VerifyState(db_key, attributes).empty());
    p4_tunnel_entry->tunnel_key = saved_tunnel_key;

    // Verification should fail if IP mismatches.
    auto saved_SRC_IP = p4_tunnel_entry->encap_src_ip;
    p4_tunnel_entry->encap_src_ip = swss::IpAddress("1.1.1.1");
    EXPECT_FALSE(VerifyState(db_key, attributes).empty());
    p4_tunnel_entry->encap_src_ip = saved_SRC_IP;

    // Verification should fail if IP mask mismatches.
    auto saved_DST_IP = p4_tunnel_entry->encap_dst_ip;
    p4_tunnel_entry->encap_dst_ip = swss::IpAddress("2.2.2.2");
    EXPECT_FALSE(VerifyState(db_key, attributes).empty());
    p4_tunnel_entry->encap_dst_ip = saved_DST_IP;

    // Verification should fail if IP mask mismatches.
    auto saved_NEIGHBOR_ID = p4_tunnel_entry->neighbor_id;
    p4_tunnel_entry->neighbor_id = swss::IpAddress("2.2.2.2");
    EXPECT_FALSE(VerifyState(db_key, attributes).empty());
    p4_tunnel_entry->neighbor_id = saved_NEIGHBOR_ID;

    // Verification should fail if tunnel_id mismatches.
    auto saved_tunnel_id = p4_tunnel_entry->tunnel_id;
    p4_tunnel_entry->tunnel_id = "invalid";
    EXPECT_FALSE(VerifyState(db_key, attributes).empty());
    p4_tunnel_entry->tunnel_id = saved_tunnel_id;

    // Verification should fail if OID mapper mismatches.
    const auto gre_tunnel_key = KeyGenerator::generateTunnelKey(kP4GreTunnelAppDbEntry1.tunnel_id);
    p4_oid_mapper_.eraseOID(SAI_OBJECT_TYPE_TUNNEL, gre_tunnel_key);
    EXPECT_FALSE(VerifyState(db_key, attributes).empty());
    p4_oid_mapper_.setOID(SAI_OBJECT_TYPE_TUNNEL, gre_tunnel_key, kGreTunnelOid1);
}

TEST_F(GreTunnelManagerTest, VerifyStateAsicDbTest)
{
    auto *p4_tunnel_entry = AddGreTunnelEntry1();
    ASSERT_NE(p4_tunnel_entry, nullptr);

    // Setup ASIC DB.
    swss::Table table(nullptr, "ASIC_STATE");
    table.set("SAI_OBJECT_TYPE_TUNNEL:oid:0x11",
              std::vector<swss::FieldValueTuple>{
                  swss::FieldValueTuple{"SAI_TUNNEL_ATTR_TYPE", "SAI_TUNNEL_TYPE_IPINIP_GRE"},
                  swss::FieldValueTuple{"SAI_TUNNEL_ATTR_PEER_MODE", "SAI_TUNNEL_PEER_MODE_P2P"},
                  swss::FieldValueTuple{"SAI_TUNNEL_ATTR_ENCAP_SRC_IP", "2607:f8b0:8096:3110::1"},
                  swss::FieldValueTuple{"SAI_TUNNEL_ATTR_ENCAP_DST_IP", "2607:f8b0:8096:311a::2"},
                  swss::FieldValueTuple{"SAI_TUNNEL_ATTR_UNDERLAY_INTERFACE", "oid:0x1"},
                  swss::FieldValueTuple{"SAI_TUNNEL_ATTR_OVERLAY_INTERFACE", "oid:0x101"}});

    // Overlay router interface
    table.set("SAI_OBJECT_TYPE_ROUTER_INTERFACE:oid:0x101",
              std::vector<swss::FieldValueTuple>{
                  swss::FieldValueTuple{"SAI_ROUTER_INTERFACE_ATTR_VIRTUAL_ROUTER_ID", "oid:0x0"},
                  swss::FieldValueTuple{"SAI_ROUTER_INTERFACE_ATTR_TYPE", "SAI_ROUTER_INTERFACE_TYPE_LOOPBACK"}});

    // Underlay router interface
    table.set("SAI_OBJECT_TYPE_ROUTER_INTERFACE:oid:0x1",
              std::vector<swss::FieldValueTuple>{
                  swss::FieldValueTuple{"SAI_ROUTER_INTERFACE_ATTR_VIRTUAL_ROUTER_ID", "oid:0x0"},
                  swss::FieldValueTuple{"SAI_ROUTER_INTERFACE_ATTR_SRC_MAC_ADDRESS", "00:01:02:03:04:05"},
                  swss::FieldValueTuple{"SAI_ROUTER_INTERFACE_ATTR_TYPE", "SAI_ROUTER_INTERFACE_TYPE_PORT"},
                  swss::FieldValueTuple{"SAI_ROUTER_INTERFACE_ATTR_PORT_ID", "oid:0x1234"},
                  swss::FieldValueTuple{"SAI_ROUTER_INTERFACE_ATTR_MTU", "9100"}});

    nlohmann::json j;
    j[prependMatchField(p4orch::kTunnelId)] = kGreTunnelP4AppDbId1;
    const std::string db_key = std::string(APP_P4RT_TABLE_NAME) + kTableKeyDelimiter + APP_P4RT_TUNNEL_TABLE_NAME +
                               kTableKeyDelimiter + j.dump();
    std::vector<swss::FieldValueTuple> attributes;

    // Verification should succeed with vaild key and value.
    attributes.push_back(swss::FieldValueTuple{p4orch::kAction, p4orch::kTunnelAction});
    attributes.push_back(swss::FieldValueTuple{prependParamField(p4orch::kRouterInterfaceId),
                                               kP4GreTunnelAppDbEntry1.router_interface_id});
    attributes.push_back(swss::FieldValueTuple{prependParamField(p4orch::kEncapSrcIp),
                                               kP4GreTunnelAppDbEntry1.encap_src_ip.to_string()});
    attributes.push_back(swss::FieldValueTuple{prependParamField(p4orch::kEncapDstIp),
                                               kP4GreTunnelAppDbEntry1.encap_dst_ip.to_string()});
    EXPECT_EQ(VerifyState(db_key, attributes), "");

    // Verification should fail if ASIC DB values mismatch.
    table.set("SAI_OBJECT_TYPE_TUNNEL:oid:0x11", std::vector<swss::FieldValueTuple>{swss::FieldValueTuple{
                                                     "SAI_TUNNEL_ATTR_ENCAP_SRC_IP", "2607:f8b0:8096:3110::3"}});
    EXPECT_FALSE(VerifyState(db_key, attributes).empty());

    // Verification should fail if ASIC DB table is missing.
    table.del("SAI_OBJECT_TYPE_TUNNEL:oid:0x11");
    EXPECT_FALSE(VerifyState(db_key, attributes).empty());

    table.set("SAI_OBJECT_TYPE_TUNNEL:oid:0x11",
              std::vector<swss::FieldValueTuple>{
                  swss::FieldValueTuple{"SAI_TUNNEL_ATTR_TYPE", "SAI_TUNNEL_TYPE_IPINIP_GRE"},
                  swss::FieldValueTuple{"SAI_TUNNEL_ATTR_PEER_MODE", "SAI_TUNNEL_PEER_MODE_P2P"},
                  swss::FieldValueTuple{"SAI_TUNNEL_ATTR_ENCAP_SRC_IP", "2607:f8b0:8096:3110::1"},
                  swss::FieldValueTuple{"SAI_TUNNEL_ATTR_ENCAP_DST_IP", "2607:f8b0:8096:311a::2"},
                  swss::FieldValueTuple{"SAI_TUNNEL_ATTR_UNDERLAY_INTERFACE", "oid:0x1"},
                  swss::FieldValueTuple{"SAI_TUNNEL_ATTR_OVERLAY_INTERFACE", "oid:0x101"}});

    // Verification should fail if SAI attr cannot be constructed.
    p4_tunnel_entry->encap_src_ip = swss::IpAddress("1.2.3.4");
    EXPECT_FALSE(VerifyState(db_key, attributes).empty());
    p4_tunnel_entry->encap_src_ip = swss::IpAddress("2607:f8b0:8096:3110::1");
}