#include "gre_tunnel_manager.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <functional>
#include <nlohmann/json.hpp>
#include <string>
#include <unordered_map>

#include "ipaddress.h"
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
using ::testing::Pointee;
using ::testing::Return;
using ::testing::SetArgPointee;
using ::testing::SetArrayArgument;
using ::testing::StrictMock;
using ::testing::Truly;

using sai_attrs_array_t =
    std::vector<std::unordered_map<sai_attr_id_t, sai_attribute_value_t>>;

extern sai_object_id_t gSwitchId;
extern sai_object_id_t gUnderlayIfId;
extern sai_tunnel_api_t *sai_tunnel_api;
extern sai_router_interface_api_t *sai_router_intfs_api;
extern MockSaiTunnel *mock_sai_tunnel;

namespace
{
constexpr char *kRouterInterfaceId1 = "intf-eth-1/2/3";
constexpr char* kRouterInterfaceId2 = "intf-eth-4/5/6";
constexpr char* kRouterInterfaceId3 = "intf-eth-7/8/9";
constexpr sai_object_id_t kRouterInterfaceOid1 = 1;
constexpr sai_object_id_t kRouterInterfaceOid2 = 2;
constexpr sai_object_id_t kRouterInterfaceOid3 = 3;
constexpr char *kGreTunnelP4AppDbId1 = "tunnel-1";
constexpr char* kGreTunnelP4AppDbId2 = "tunnel-2";
constexpr char* kGreTunnelP4AppDbId3 = "tunnel-3";
constexpr char *kGreTunnelP4AppDbKey1 = R"({"match/tunnel_id":"tunnel-1"})";
constexpr char* kGreTunnelP4AppDbKey2 = R"({"match/tunnel_id":"tunnel-2"})";
constexpr char* kGreTunnelP4AppDbKey3 = R"({"match/tunnel_id":"tunnel-3"})";
constexpr sai_object_id_t kGreTunnelOid1 = 0x11;
constexpr sai_object_id_t kGreTunnelOid2 = 0x12;
constexpr sai_object_id_t kGreTunnelOid3 = 0x13;

MATCHER_P(ArrayEq, array, "") {
  for (size_t i = 0; i < array.size(); ++i) {
    if (arg[i] != array[i]) {
      return false;
    }
  }
  return true;
}

// APP DB entries for Add request.
const P4GreTunnelAppDbEntry kP4GreTunnelAppDbEntry1{/*tunnel_id=*/"tunnel-1",
                                                    /*router_interface_id=*/"intf-eth-1/2/3",
                                                    /*encap_src_ip=*/swss::IpAddress("2607:f8b0:8096:3110::1"),
                                                    /*encap_dst_ip=*/swss::IpAddress("2607:f8b0:8096:311a::2"),
                                                    /*action_str=*/"mark_for_p2p_tunnel_encap"};

const P4GreTunnelAppDbEntry kP4GreTunnelAppDbEntry2{
    /*tunnel_id=*/"tunnel-2",
    /*router_interface_id=*/"intf-eth-4/5/6",
    /*encap_src_ip=*/swss::IpAddress("2607:f8b0:8096:3110::3"),
    /*encap_dst_ip=*/swss::IpAddress("2607:f8b0:8096:311a::4"),
    /*action_str=*/"mark_for_p2p_tunnel_encap"};

const P4GreTunnelAppDbEntry kP4GreTunnelAppDbEntry3{
    /*tunnel_id=*/"tunnel-3",
    /*router_interface_id=*/"intf-eth-7/8/9",
    /*encap_src_ip=*/swss::IpAddress("2607:f8b0:8096:3110::5"),
    /*encap_dst_ip=*/swss::IpAddress("2607:f8b0:8096:311a::6"),
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
    tunnel_attr.value.oid = gUnderlayIfId;
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

MATCHER_P(AttrArrayEq, array, "") {
  for (size_t i = 0; i < array.size(); ++i) {
    if (!MatchCreateGreTunnelArgAttrList(arg[i], array[i])) {
      return false;
    }
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
        sai_tunnel_api->create_tunnels = mock_create_tunnels;
        sai_tunnel_api->remove_tunnels = mock_remove_tunnels;
        // Set up mock stuff for SAI router interface API structure.
        mock_sai_router_intf = &mock_sai_router_intf_;
        sai_router_intfs_api->create_router_interface = mock_create_router_interface;
        sai_router_intfs_api->remove_router_interface = mock_remove_router_interface;

        mock_sai_serialize = &mock_sai_serialize_;
    }

    void Enqueue(const swss::KeyOpFieldsValuesTuple &entry)
    {
        gre_tunnel_manager_.enqueue(APP_P4RT_TUNNEL_TABLE_NAME, entry);
    }

    ReturnCode Drain(bool failure_before) {
      if (failure_before) {
        gre_tunnel_manager_.drainWithNotExecuted();
        return ReturnCode(StatusCode::SWSS_RC_NOT_EXECUTED);
      }
      return gre_tunnel_manager_.drain();
    }

    std::string VerifyState(const std::string &key, const std::vector<swss::FieldValueTuple> &tuple)
    {
        return gre_tunnel_manager_.verifyState(key, tuple);
    }

    P4GreTunnelEntry* GetGreTunnelEntry(const std::string& tunnel_key)
    {
        return gre_tunnel_manager_.getGreTunnelEntry(tunnel_key);
    }

    std::vector<ReturnCode> CreateGreTunnels(
        const std::vector<P4GreTunnelAppDbEntry>& entries)
    {
        return gre_tunnel_manager_.createGreTunnels(entries);
    }

    std::vector<ReturnCode> RemoveGreTunnels(
        const std::vector<P4GreTunnelAppDbEntry>& entries)
    {
        return gre_tunnel_manager_.removeGreTunnels(entries);
    }

    ReturnCode processEntries(
        const std::vector<P4GreTunnelAppDbEntry>& entries,
        const std::vector<swss::KeyOpFieldsValuesTuple>& tuple_list,
        const std::string& op, bool update)
    {
        return gre_tunnel_manager_.processEntries(entries, tuple_list, op, update);
    }

    ReturnCodeOr<P4GreTunnelAppDbEntry> DeserializeP4GreTunnelAppDbEntry(
        const std::string &key, const std::vector<swss::FieldValueTuple> &attributes)
    {
        return gre_tunnel_manager_.deserializeP4GreTunnelAppDbEntry(key, attributes);
    }

    ReturnCode ValidateGreTunnelAppDbEntry(
        const P4GreTunnelAppDbEntry& app_db_entry, const std::string& operation)
    {
        return gre_tunnel_manager_.validateGreTunnelAppDbEntry(app_db_entry,
                                                               operation);
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
    StrictMock<MockResponsePublisher> publisher_;
    P4OidMapper p4_oid_mapper_;
    GreTunnelManager gre_tunnel_manager_;
};

P4GreTunnelEntry *GreTunnelManagerTest::AddGreTunnelEntry1()
{
    const std::string neighbor_key = KeyGenerator::generateNeighborKey(
      kP4GreTunnelAppDbEntry1.router_interface_id,
      kP4GreTunnelAppDbEntry1.encap_dst_ip);
  EXPECT_TRUE(
      p4_oid_mapper_.setDummyOID(SAI_OBJECT_TYPE_NEIGHBOR_ENTRY, neighbor_key));
  uint32_t original_neighbor_ref_count;
  EXPECT_TRUE(p4_oid_mapper_.getRefCount(SAI_OBJECT_TYPE_NEIGHBOR_ENTRY,
                                         neighbor_key,
                                         &original_neighbor_ref_count));
  const std::string rif_key = KeyGenerator::generateRouterInterfaceKey(
      kP4GreTunnelAppDbEntry1.router_interface_id);
    EXPECT_TRUE(p4_oid_mapper_.setOID(SAI_OBJECT_TYPE_ROUTER_INTERFACE,
                                      KeyGenerator::generateRouterInterfaceKey(kRouterInterfaceId1),
                                      kRouterInterfaceOid1));
    uint32_t original_rif_ref_count;
    EXPECT_TRUE(p4_oid_mapper_.getRefCount(SAI_OBJECT_TYPE_ROUTER_INTERFACE,
                                         rif_key, &original_rif_ref_count));

  const std::string gre_tunnel_key =
      KeyGenerator::generateTunnelKey(kP4GreTunnelAppDbEntry1.tunnel_id);
    std::vector<sai_status_t> exp_status{SAI_STATUS_SUCCESS};
    // Set up mock call.
    EXPECT_CALL(
        mock_sai_tunnel_,
        create_tunnels(
            Eq(gSwitchId), Eq(1), Pointee(Eq(6)),
            AttrArrayEq(sai_attrs_array_t{CreateAttributeListForGreTunnelObject(
                kP4GreTunnelAppDbEntry1, kRouterInterfaceOid1)}),
            Eq(SAI_BULK_OP_ERROR_MODE_STOP_ON_ERROR), ::testing::NotNull(),
            ::testing::NotNull()))
        .WillOnce(DoAll(SetArrayArgument<6>(exp_status.begin(), exp_status.end()),
                        SetArgPointee<5>(kGreTunnelOid1),
                        Return(SAI_STATUS_SUCCESS)));

    EXPECT_THAT(
        CreateGreTunnels(
            std::vector<P4GreTunnelAppDbEntry>{kP4GreTunnelAppDbEntry1}),
        ArrayEq(std::vector<StatusCode>{StatusCode::SWSS_RC_SUCCESS}));

    EXPECT_TRUE(ValidateRefCnt(SAI_OBJECT_TYPE_NEIGHBOR_ENTRY, neighbor_key,
                             original_neighbor_ref_count + 1));
    EXPECT_TRUE(ValidateRefCnt(SAI_OBJECT_TYPE_ROUTER_INTERFACE, rif_key,
                             original_rif_ref_count + 1));
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

TEST_F(GreTunnelManagerTest, CreateGreTunnelsShouldSucceedAddingNewGreTunnel)
{
  AddGreTunnelEntry1();
  EXPECT_TRUE(ValidateGreTunnelEntryAdd(kP4GreTunnelAppDbEntry1));

  const auto gre_tunnel_key =
      KeyGenerator::generateTunnelKey(kP4GreTunnelAppDbEntry1.tunnel_id);
  EXPECT_TRUE(p4_oid_mapper_.existsOID(SAI_OBJECT_TYPE_TUNNEL, gre_tunnel_key));

  const auto router_interface_key = KeyGenerator::generateRouterInterfaceKey(
      kP4GreTunnelAppDbEntry1.router_interface_id);
  EXPECT_TRUE(ValidateRefCnt(SAI_OBJECT_TYPE_ROUTER_INTERFACE,
                             router_interface_key, 1));
}

TEST_F(GreTunnelManagerTest, CreateGreTunnelsShouldFailWhenTunnelSaiCallFails)
{
  const auto gre_tunnel_key =
      KeyGenerator::generateTunnelKey(kP4GreTunnelAppDbEntry1.tunnel_id);
  const auto router_interface_key = KeyGenerator::generateRouterInterfaceKey(
      kP4GreTunnelAppDbEntry1.router_interface_id);

  EXPECT_TRUE(p4_oid_mapper_.setOID(SAI_OBJECT_TYPE_ROUTER_INTERFACE,
                                    router_interface_key,
                                    kRouterInterfaceOid1));
  // Set up mock call.
  std::vector<sai_status_t> exp_status{SAI_STATUS_FAILURE};
  EXPECT_CALL(
      mock_sai_tunnel_,
      create_tunnels(
          Eq(gSwitchId), Eq(1), Pointee(Eq(6)),
          AttrArrayEq(sai_attrs_array_t{CreateAttributeListForGreTunnelObject(
              kP4GreTunnelAppDbEntry1, kRouterInterfaceOid1)}),
          Eq(SAI_BULK_OP_ERROR_MODE_STOP_ON_ERROR), ::testing::NotNull(),
          ::testing::NotNull()))
      .WillOnce(DoAll(SetArrayArgument<6>(exp_status.begin(), exp_status.end()),
                      SetArgPointee<5>(kGreTunnelOid1),
                      Return(SAI_STATUS_FAILURE)));

  EXPECT_THAT(CreateGreTunnels(
                  std::vector<P4GreTunnelAppDbEntry>{kP4GreTunnelAppDbEntry1}),
              ArrayEq(std::vector<StatusCode>{StatusCode::SWSS_RC_UNKNOWN}));

  // The add request failed for the gre tunnel entry.
  EXPECT_EQ(GetGreTunnelEntry(gre_tunnel_key), nullptr);
  EXPECT_FALSE(
      p4_oid_mapper_.existsOID(SAI_OBJECT_TYPE_TUNNEL, gre_tunnel_key));

  EXPECT_TRUE(ValidateRefCnt(SAI_OBJECT_TYPE_ROUTER_INTERFACE,
                             router_interface_key, 0));
}


TEST_F(GreTunnelManagerTest, ProcessDeleteRequestShouldFailIfTunnelSaiCallFails)
{
    auto *p4_tunnel_entry = AddGreTunnelEntry1();
    ASSERT_NE(p4_tunnel_entry, nullptr);

    const auto gre_tunnel_key = KeyGenerator::generateTunnelKey(kP4GreTunnelAppDbEntry1.tunnel_id);

    std::vector<sai_status_t> exp_status{SAI_STATUS_FAILURE};
    // Set up mock call.
    EXPECT_CALL(mock_sai_tunnel_, remove_tunnels(_, _, _, _))
        .WillOnce(DoAll(SetArrayArgument<3>(exp_status.begin(), exp_status.end()),
                        Return(SAI_STATUS_FAILURE)));

    EXPECT_THAT(RemoveGreTunnels(
                    std::vector<P4GreTunnelAppDbEntry>{kP4GreTunnelAppDbEntry1}),
                ArrayEq(std::vector<StatusCode>{StatusCode::SWSS_RC_UNKNOWN}));

    // Validate the gre tunnel entry is not deleted in either P4 gre tunnel
    // manager or central mapper.
    p4_tunnel_entry = GetGreTunnelEntry(gre_tunnel_key);
    ASSERT_NE(p4_tunnel_entry, nullptr);
    EXPECT_TRUE(p4_oid_mapper_.existsOID(SAI_OBJECT_TYPE_TUNNEL, gre_tunnel_key));
    const auto router_interface_key = KeyGenerator::generateRouterInterfaceKey(
        kP4GreTunnelAppDbEntry1.router_interface_id);
    EXPECT_TRUE(ValidateRefCnt(SAI_OBJECT_TYPE_ROUTER_INTERFACE,
                               router_interface_key, 1));
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

TEST_F(GreTunnelManagerTest, ValidateGreTunnelAppDbEntryValidDelEntry) {
  auto* p4_tunnel_entry = AddGreTunnelEntry1();
  ASSERT_NE(p4_tunnel_entry, nullptr);

  EXPECT_TRUE(
      ValidateGreTunnelAppDbEntry(kP4GreTunnelAppDbEntry1, DEL_COMMAND).ok());
}

TEST_F(GreTunnelManagerTest, ValidateGreTunnelAppDbEntryEntryNotFoundInDel) {
  EXPECT_FALSE(
      ValidateGreTunnelAppDbEntry(kP4GreTunnelAppDbEntry1, DEL_COMMAND).ok());
}

TEST_F(GreTunnelManagerTest,
       ValidateGreTunnelAppDbEntryOidMapperEntryNotFoundInDel) {
  auto* p4_tunnel_entry = AddGreTunnelEntry1();
  ASSERT_NE(p4_tunnel_entry, nullptr);

  ASSERT_TRUE(p4_oid_mapper_.eraseOID(SAI_OBJECT_TYPE_TUNNEL,
                                      p4_tunnel_entry->tunnel_key));

  EXPECT_FALSE(
      ValidateGreTunnelAppDbEntry(kP4GreTunnelAppDbEntry1, DEL_COMMAND).ok());
}

TEST_F(GreTunnelManagerTest, ValidateGreTunnelAppDbEntryRefCntNotZeroInDel) {
  auto* p4_tunnel_entry = AddGreTunnelEntry1();
  ASSERT_NE(p4_tunnel_entry, nullptr);

  ASSERT_TRUE(p4_oid_mapper_.increaseRefCount(SAI_OBJECT_TYPE_TUNNEL,
                                              p4_tunnel_entry->tunnel_key));
  EXPECT_FALSE(
      ValidateGreTunnelAppDbEntry(kP4GreTunnelAppDbEntry1, DEL_COMMAND).ok());
}

TEST_F(GreTunnelManagerTest, ValidateGreTunnelAppDbEntryValidSetEntry) {
  const auto gre_tunnel_key =
      KeyGenerator::generateTunnelKey(kP4GreTunnelAppDbEntry1.tunnel_id);
  EXPECT_TRUE(p4_oid_mapper_.setOID(
      SAI_OBJECT_TYPE_ROUTER_INTERFACE,
      KeyGenerator::generateRouterInterfaceKey(kRouterInterfaceId1),
      kRouterInterfaceOid1));
  EXPECT_TRUE(p4_oid_mapper_.setDummyOID(
      SAI_OBJECT_TYPE_NEIGHBOR_ENTRY,
      KeyGenerator::generateNeighborKey(
          kP4GreTunnelAppDbEntry1.router_interface_id,
          kP4GreTunnelAppDbEntry1.encap_dst_ip)));

  EXPECT_TRUE(
      ValidateGreTunnelAppDbEntry(kP4GreTunnelAppDbEntry1, SET_COMMAND).ok());
}

TEST_F(GreTunnelManagerTest, ValidateGreTunnelAppDbEntryInvalidAction) {
  const P4GreTunnelAppDbEntry app_db_entry{
      /*tunnel_id=*/"tunnel-1",
      /*router_interface_id=*/"intf-eth-1/2/3",
      /*encap_src_ip=*/swss::IpAddress("2607:f8b0:8096:3110::1"),
      /*encap_dst_ip=*/swss::IpAddress("2607:f8b0:8096:311a::2"),
      /*action_str=*/"invalid action"};
  EXPECT_FALSE(
      ValidateGreTunnelAppDbEntry(kP4GreTunnelAppDbEntry1, SET_COMMAND).ok());
}

TEST_F(GreTunnelManagerTest,
       ValidateGreTunnelAppDbEntryMissingRouterInterfaceId) {
  const P4GreTunnelAppDbEntry app_db_entry{
      /*tunnel_id=*/"tunnel-1",
      /*router_interface_id=*/"",
      /*encap_src_ip=*/swss::IpAddress("2607:f8b0:8096:3110::1"),
      /*encap_dst_ip=*/swss::IpAddress("2607:f8b0:8096:311a::2"),
      /*action_str=*/"mark_for_p2p_tunnel_encap"};
  EXPECT_FALSE(
      ValidateGreTunnelAppDbEntry(kP4GreTunnelAppDbEntry1, SET_COMMAND).ok());
}

TEST_F(GreTunnelManagerTest, ValidateGreTunnelAppDbEntryEmptyEncapSrcIp) {
  const P4GreTunnelAppDbEntry app_db_entry{
      /*tunnel_id=*/"tunnel-1",
      /*router_interface_id=*/"intf-eth-1/2/3",
      /*encap_src_ip=*/swss::IpAddress(),
      /*encap_dst_ip=*/swss::IpAddress("2607:f8b0:8096:311a::2"),
      /*action_str=*/"mark_for_p2p_tunnel_encap"};
  EXPECT_FALSE(
      ValidateGreTunnelAppDbEntry(kP4GreTunnelAppDbEntry1, SET_COMMAND).ok());
}

TEST_F(GreTunnelManagerTest, ValidateGreTunnelAppDbEntryEmptyEncapDistIp) {
  const P4GreTunnelAppDbEntry app_db_entry{
      /*tunnel_id=*/"tunnel-1",
      /*router_interface_id=*/"intf-eth-1/2/3",
      /*encap_src_ip=*/swss::IpAddress("2607:f8b0:8096:3110::1"),
      /*encap_dst_ip=*/swss::IpAddress(),
      /*action_str=*/"mark_for_p2p_tunnel_encap"};
  EXPECT_FALSE(
      ValidateGreTunnelAppDbEntry(kP4GreTunnelAppDbEntry1, SET_COMMAND).ok());
}

TEST_F(GreTunnelManagerTest,
       ValidateGreTunnelAppDbEntryMapperOidExistsForCreate) {
  const auto gre_tunnel_key =
      KeyGenerator::generateTunnelKey(kP4GreTunnelAppDbEntry1.tunnel_id);
  ASSERT_TRUE(p4_oid_mapper_.setOID(SAI_OBJECT_TYPE_TUNNEL, gre_tunnel_key,
                                    kGreTunnelOid1));

  EXPECT_FALSE(
      ValidateGreTunnelAppDbEntry(kP4GreTunnelAppDbEntry1, SET_COMMAND).ok());
}

TEST_F(GreTunnelManagerTest,
       ValidateGreTunnelAppDbEntryNonexistRouterInterfaceForCreate) {
  EXPECT_FALSE(
      ValidateGreTunnelAppDbEntry(kP4GreTunnelAppDbEntry1, SET_COMMAND).ok());
}

TEST_F(GreTunnelManagerTest, DrainDuplicateSetRequestShouldFail) {
  auto* p4_tunnel_entry = AddGreTunnelEntry1();
  ASSERT_NE(p4_tunnel_entry, nullptr);

  nlohmann::json j;
  j[prependMatchField(p4orch::kTunnelId)] = kP4GreTunnelAppDbEntry1.tunnel_id;

  std::vector<swss::FieldValueTuple> fvs{
      {p4orch::kAction, p4orch::kTunnelAction},
      {prependParamField(p4orch::kRouterInterfaceId),
       kP4GreTunnelAppDbEntry1.router_interface_id},
      {prependParamField(p4orch::kEncapSrcIp),
       kP4GreTunnelAppDbEntry1.encap_src_ip.to_string()},
      {prependParamField(p4orch::kEncapDstIp),
       kP4GreTunnelAppDbEntry1.encap_dst_ip.to_string()}};

  swss::KeyOpFieldsValuesTuple app_db_entry(
      std::string(APP_P4RT_TUNNEL_TABLE_NAME) + kTableKeyDelimiter + j.dump(),
      SET_COMMAND, fvs);

  Enqueue(app_db_entry);
  EXPECT_CALL(publisher_,
              publish(Eq(APP_P4RT_TABLE_NAME), Eq(kfvKey(app_db_entry)),
                      Eq(kfvFieldsValues(app_db_entry)),
                      Eq(StatusCode::SWSS_RC_UNIMPLEMENTED), Eq(true)));
  EXPECT_EQ(StatusCode::SWSS_RC_UNIMPLEMENTED, Drain(/*failure_before=*/false));

  // Expect that the update call will fail, so gre tunnel entry's fields stay
  // the same.
  EXPECT_TRUE(ValidateGreTunnelEntryAdd(kP4GreTunnelAppDbEntry1));
  const auto gre_tunnel_key =
      KeyGenerator::generateTunnelKey(kP4GreTunnelAppDbEntry1.tunnel_id);
  EXPECT_TRUE(p4_oid_mapper_.existsOID(SAI_OBJECT_TYPE_TUNNEL, gre_tunnel_key));

  const auto router_interface_key = KeyGenerator::generateRouterInterfaceKey(
      kP4GreTunnelAppDbEntry1.router_interface_id);
  EXPECT_TRUE(ValidateRefCnt(SAI_OBJECT_TYPE_ROUTER_INTERFACE,
                             router_interface_key, 1));
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
    std::vector<sai_status_t> exp_status{SAI_STATUS_SUCCESS};
    EXPECT_CALL(mock_sai_tunnel_, remove_tunnels(_, _, _, _))
        .WillOnce(DoAll(SetArrayArgument<3>(exp_status.begin(), exp_status.end()),
                        Return(SAI_STATUS_SUCCESS)));

    Enqueue(app_db_entry);
    EXPECT_CALL(publisher_,
                publish(Eq(APP_P4RT_TABLE_NAME), Eq(kfvKey(app_db_entry)),
                        Eq(kfvFieldsValues(app_db_entry)),
                        Eq(StatusCode::SWSS_RC_SUCCESS), Eq(true)));
    EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS, Drain(/*failure_before=*/false));

    // Validate the gre tunnel entry has been deleted in both P4 gre tunnel
    // manager and centralized mapper.
    const auto gre_tunnel_key = KeyGenerator::generateTunnelKey(kP4GreTunnelAppDbEntry1.tunnel_id);
    p4_tunnel_entry = GetGreTunnelEntry(gre_tunnel_key);
    EXPECT_EQ(p4_tunnel_entry, nullptr);
    EXPECT_FALSE(p4_oid_mapper_.existsOID(SAI_OBJECT_TYPE_TUNNEL, gre_tunnel_key));
    const auto router_interface_key = KeyGenerator::generateRouterInterfaceKey(
        kP4GreTunnelAppDbEntry1.router_interface_id);
    EXPECT_TRUE(ValidateRefCnt(SAI_OBJECT_TYPE_ROUTER_INTERFACE,
                               router_interface_key, 0));
    const auto neighbor_key = KeyGenerator::generateNeighborKey(
      kP4GreTunnelAppDbEntry1.router_interface_id,
      kP4GreTunnelAppDbEntry1.encap_dst_ip);
    EXPECT_TRUE(ValidateRefCnt(SAI_OBJECT_TYPE_NEIGHBOR_ENTRY, neighbor_key, 0));
}

TEST_F(GreTunnelManagerTest, DrainValidAppEntryShouldSucceed)
{
    nlohmann::json j;
    j[prependMatchField(p4orch::kTunnelId)] = kGreTunnelP4AppDbId1;

    const auto router_interface_key = KeyGenerator::generateRouterInterfaceKey(
        kP4GreTunnelAppDbEntry1.router_interface_id);
    EXPECT_TRUE(p4_oid_mapper_.setOID(SAI_OBJECT_TYPE_ROUTER_INTERFACE,
                                      router_interface_key,
                                      kRouterInterfaceOid1));
    uint32_t original_rif_ref_count;
    EXPECT_TRUE(p4_oid_mapper_.getRefCount(SAI_OBJECT_TYPE_ROUTER_INTERFACE,
                                         router_interface_key,
                                         &original_rif_ref_count));
    const auto neighbor_key = KeyGenerator::generateNeighborKey(
      kP4GreTunnelAppDbEntry1.router_interface_id,
      kP4GreTunnelAppDbEntry1.encap_dst_ip);
    EXPECT_TRUE(
      p4_oid_mapper_.setDummyOID(SAI_OBJECT_TYPE_NEIGHBOR_ENTRY, neighbor_key));
    uint32_t original_neighbor_ref_count;
    EXPECT_TRUE(p4_oid_mapper_.getRefCount(SAI_OBJECT_TYPE_NEIGHBOR_ENTRY,
                                         neighbor_key,
                                         &original_neighbor_ref_count));
    std::vector<swss::FieldValueTuple> fvs{
        {p4orch::kAction, p4orch::kTunnelAction},
        {prependParamField(p4orch::kRouterInterfaceId), kP4GreTunnelAppDbEntry1.router_interface_id},
        {prependParamField(p4orch::kEncapSrcIp), kP4GreTunnelAppDbEntry1.encap_src_ip.to_string()},
        {prependParamField(p4orch::kEncapDstIp), kP4GreTunnelAppDbEntry1.encap_dst_ip.to_string()}};

    swss::KeyOpFieldsValuesTuple app_db_entry(std::string(APP_P4RT_TUNNEL_TABLE_NAME) + kTableKeyDelimiter + j.dump(),
                                              SET_COMMAND, fvs);

    Enqueue(app_db_entry);
    std::vector<sai_status_t> exp_status{SAI_STATUS_SUCCESS};
    EXPECT_CALL(mock_sai_tunnel_, create_tunnels(_, _, _, _, _, _, _))
        .WillOnce(DoAll(SetArrayArgument<6>(exp_status.begin(), exp_status.end()),
                        SetArgPointee<5>(kGreTunnelOid1),
                        Return(SAI_STATUS_SUCCESS)));
    EXPECT_CALL(publisher_,
                publish(Eq(APP_P4RT_TABLE_NAME), Eq(kfvKey(app_db_entry)),
                        Eq(kfvFieldsValues(app_db_entry)),
                        Eq(StatusCode::SWSS_RC_SUCCESS), Eq(true)));
    EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS, Drain(/*failure_before=*/false));

    EXPECT_TRUE(ValidateGreTunnelEntryAdd(kP4GreTunnelAppDbEntry1));
    const auto gre_tunnel_key =
        KeyGenerator::generateTunnelKey(kP4GreTunnelAppDbEntry1.tunnel_id);
    EXPECT_TRUE(p4_oid_mapper_.existsOID(SAI_OBJECT_TYPE_TUNNEL, gre_tunnel_key));

    EXPECT_TRUE(ValidateRefCnt(SAI_OBJECT_TYPE_ROUTER_INTERFACE,
                               router_interface_key, original_rif_ref_count + 1));
    EXPECT_TRUE(ValidateRefCnt(SAI_OBJECT_TYPE_NEIGHBOR_ENTRY, neighbor_key,
                             original_neighbor_ref_count + 1));
}

TEST_F(GreTunnelManagerTest, DrainInvalidAppEntryShouldFail)
{
    nlohmann::json j;
    j[prependMatchField(p4orch::kTunnelId)] = kGreTunnelP4AppDbId1;
    j[p4orch::kTunnelId] = 1000;

    const auto router_interface_key = KeyGenerator::generateRouterInterfaceKey(
        kP4GreTunnelAppDbEntry1.router_interface_id);
    EXPECT_TRUE(p4_oid_mapper_.setOID(SAI_OBJECT_TYPE_ROUTER_INTERFACE,
                                      router_interface_key,
                                      kRouterInterfaceOid1));

    std::vector<swss::FieldValueTuple> fvs{
        {p4orch::kAction, p4orch::kTunnelAction},
        {prependParamField(p4orch::kRouterInterfaceId), kP4GreTunnelAppDbEntry1.router_interface_id},
        {prependParamField(p4orch::kEncapSrcIp), "1"},
        {prependParamField(p4orch::kEncapDstIp), kP4GreTunnelAppDbEntry1.encap_dst_ip.to_string()}};

    swss::KeyOpFieldsValuesTuple app_db_entry(std::string(APP_P4RT_TUNNEL_TABLE_NAME) + kTableKeyDelimiter + j.dump(),
                                              SET_COMMAND, fvs);

    Enqueue(app_db_entry);

    EXPECT_CALL(publisher_,
                publish(Eq(APP_P4RT_TABLE_NAME), Eq(kfvKey(app_db_entry)),
                        Eq(kfvFieldsValues(app_db_entry)),
                        Eq(StatusCode::SWSS_RC_INVALID_PARAM), Eq(true)));
    EXPECT_EQ(StatusCode::SWSS_RC_INVALID_PARAM,
              Drain(/*failure_before=*/false));
    EXPECT_EQ(GetGreTunnelEntry(kGreTunnelP4AppDbKey1), nullptr);
    EXPECT_FALSE(p4_oid_mapper_.existsOID(SAI_OBJECT_TYPE_TUNNEL, kGreTunnelP4AppDbKey1));
    EXPECT_TRUE(ValidateRefCnt(SAI_OBJECT_TYPE_ROUTER_INTERFACE,
                               router_interface_key, 0));

    // Invalid action_str
    fvs = {{p4orch::kAction, "set_nexthop"},
           {prependParamField(p4orch::kRouterInterfaceId), kP4GreTunnelAppDbEntry1.router_interface_id},
           {prependParamField(p4orch::kEncapSrcIp), kP4GreTunnelAppDbEntry1.encap_src_ip.to_string()},
           {prependParamField(p4orch::kEncapDstIp), kP4GreTunnelAppDbEntry1.encap_dst_ip.to_string()}};

    app_db_entry = {std::string(APP_P4RT_TUNNEL_TABLE_NAME) + kTableKeyDelimiter + j.dump(), SET_COMMAND, fvs};

    Enqueue(app_db_entry);

    EXPECT_CALL(publisher_,
                publish(Eq(APP_P4RT_TABLE_NAME), Eq(kfvKey(app_db_entry)),
                        Eq(kfvFieldsValues(app_db_entry)),
                        Eq(StatusCode::SWSS_RC_INVALID_PARAM), Eq(true)));
    EXPECT_EQ(StatusCode::SWSS_RC_INVALID_PARAM,
              Drain(/*failure_before=*/false));
    EXPECT_EQ(GetGreTunnelEntry(kGreTunnelP4AppDbKey1), nullptr);
    EXPECT_FALSE(p4_oid_mapper_.existsOID(SAI_OBJECT_TYPE_TUNNEL, kGreTunnelP4AppDbKey1));
    EXPECT_TRUE(ValidateRefCnt(SAI_OBJECT_TYPE_ROUTER_INTERFACE,
                               router_interface_key, 0));

    // Miss action
    fvs = {{prependParamField(p4orch::kRouterInterfaceId), kP4GreTunnelAppDbEntry1.router_interface_id},
           {prependParamField(p4orch::kEncapSrcIp), kP4GreTunnelAppDbEntry1.encap_src_ip.to_string()},
           {prependParamField(p4orch::kEncapDstIp), kP4GreTunnelAppDbEntry1.encap_dst_ip.to_string()}};

    app_db_entry = {std::string(APP_P4RT_TUNNEL_TABLE_NAME) + kTableKeyDelimiter + j.dump(), SET_COMMAND, fvs};

    Enqueue(app_db_entry);

    EXPECT_CALL(publisher_,
                publish(Eq(APP_P4RT_TABLE_NAME), Eq(kfvKey(app_db_entry)),
                        Eq(kfvFieldsValues(app_db_entry)),
                        Eq(StatusCode::SWSS_RC_INVALID_PARAM), Eq(true)));
    EXPECT_EQ(StatusCode::SWSS_RC_INVALID_PARAM,
              Drain(/*failure_before=*/false));
    EXPECT_EQ(GetGreTunnelEntry(kGreTunnelP4AppDbKey1), nullptr);
    EXPECT_FALSE(p4_oid_mapper_.existsOID(SAI_OBJECT_TYPE_TUNNEL, kGreTunnelP4AppDbKey1));
    EXPECT_TRUE(ValidateRefCnt(SAI_OBJECT_TYPE_ROUTER_INTERFACE,
                               router_interface_key, 0));

    // Miss router_interface_id
    fvs = {{p4orch::kAction, p4orch::kTunnelAction},
           {prependParamField(p4orch::kEncapSrcIp), kP4GreTunnelAppDbEntry1.encap_src_ip.to_string()},
           {prependParamField(p4orch::kEncapDstIp), kP4GreTunnelAppDbEntry1.encap_dst_ip.to_string()}};

    app_db_entry = {std::string(APP_P4RT_TUNNEL_TABLE_NAME) + kTableKeyDelimiter + j.dump(), SET_COMMAND, fvs};

    Enqueue(app_db_entry);

    EXPECT_CALL(publisher_,
                publish(Eq(APP_P4RT_TABLE_NAME), Eq(kfvKey(app_db_entry)),
                        Eq(kfvFieldsValues(app_db_entry)),
                        Eq(StatusCode::SWSS_RC_INVALID_PARAM), Eq(true)));
    EXPECT_EQ(StatusCode::SWSS_RC_INVALID_PARAM,
              Drain(/*failure_before=*/false));
    EXPECT_EQ(GetGreTunnelEntry(kGreTunnelP4AppDbKey1), nullptr);
    EXPECT_FALSE(p4_oid_mapper_.existsOID(SAI_OBJECT_TYPE_TUNNEL, kGreTunnelP4AppDbKey1));
    EXPECT_TRUE(ValidateRefCnt(SAI_OBJECT_TYPE_ROUTER_INTERFACE,
                               router_interface_key, 0));

    // Miss encap_src_ip
    fvs = {{p4orch::kAction, p4orch::kTunnelAction},
           {prependParamField(p4orch::kRouterInterfaceId), kP4GreTunnelAppDbEntry1.router_interface_id},
           {prependParamField(p4orch::kEncapDstIp), kP4GreTunnelAppDbEntry1.encap_dst_ip.to_string()}};

    app_db_entry = {std::string(APP_P4RT_TUNNEL_TABLE_NAME) + kTableKeyDelimiter + j.dump(), SET_COMMAND, fvs};

    Enqueue(app_db_entry);

    EXPECT_CALL(publisher_,
                publish(Eq(APP_P4RT_TABLE_NAME), Eq(kfvKey(app_db_entry)),
                        Eq(kfvFieldsValues(app_db_entry)),
                        Eq(StatusCode::SWSS_RC_INVALID_PARAM), Eq(true)));
    EXPECT_EQ(StatusCode::SWSS_RC_INVALID_PARAM,
              Drain(/*failure_before=*/false));
    EXPECT_EQ(GetGreTunnelEntry(kGreTunnelP4AppDbKey1), nullptr);
    EXPECT_FALSE(p4_oid_mapper_.existsOID(SAI_OBJECT_TYPE_TUNNEL, kGreTunnelP4AppDbKey1));
    EXPECT_TRUE(ValidateRefCnt(SAI_OBJECT_TYPE_ROUTER_INTERFACE,
                               router_interface_key, 0));

    // Miss encap_dst_ip
    fvs = {{p4orch::kAction, p4orch::kTunnelAction},
           {prependParamField(p4orch::kRouterInterfaceId), kP4GreTunnelAppDbEntry1.router_interface_id},
           {prependParamField(p4orch::kEncapSrcIp), kP4GreTunnelAppDbEntry1.encap_src_ip.to_string()}};

    app_db_entry = {std::string(APP_P4RT_TUNNEL_TABLE_NAME) + kTableKeyDelimiter + j.dump(), SET_COMMAND, fvs};

    Enqueue(app_db_entry);

    EXPECT_CALL(publisher_,
                publish(Eq(APP_P4RT_TABLE_NAME), Eq(kfvKey(app_db_entry)),
                        Eq(kfvFieldsValues(app_db_entry)),
                        Eq(StatusCode::SWSS_RC_INVALID_PARAM), Eq(true)));
    EXPECT_EQ(StatusCode::SWSS_RC_INVALID_PARAM,
              Drain(/*failure_before=*/false));
    EXPECT_EQ(GetGreTunnelEntry(kGreTunnelP4AppDbKey1), nullptr);
    EXPECT_FALSE(p4_oid_mapper_.existsOID(SAI_OBJECT_TYPE_TUNNEL, kGreTunnelP4AppDbKey1));
    EXPECT_TRUE(ValidateRefCnt(SAI_OBJECT_TYPE_ROUTER_INTERFACE,
                               router_interface_key, 0));
}

TEST_F(GreTunnelManagerTest, DrainNotExecuted) {
  const auto router_interface_key = KeyGenerator::generateRouterInterfaceKey(
      kP4GreTunnelAppDbEntry1.router_interface_id);

  EXPECT_TRUE(p4_oid_mapper_.setOID(SAI_OBJECT_TYPE_ROUTER_INTERFACE,
                                    router_interface_key,
                                    kRouterInterfaceOid1));
  std::vector<swss::FieldValueTuple> fvs{
      {p4orch::kAction, p4orch::kTunnelAction},
      {prependParamField(p4orch::kRouterInterfaceId),
       kP4GreTunnelAppDbEntry1.router_interface_id},
      {prependParamField(p4orch::kEncapSrcIp),
       kP4GreTunnelAppDbEntry1.encap_src_ip.to_string()},
      {prependParamField(p4orch::kEncapDstIp),
       kP4GreTunnelAppDbEntry1.encap_dst_ip.to_string()}};

  nlohmann::json j;
  j[prependMatchField(p4orch::kTunnelId)] = "1";
  swss::KeyOpFieldsValuesTuple app_db_entry_1(
      std::string(APP_P4RT_TUNNEL_TABLE_NAME) + kTableKeyDelimiter + j.dump(),
      SET_COMMAND, fvs);
  j[prependMatchField(p4orch::kTunnelId)] = "2";
  swss::KeyOpFieldsValuesTuple app_db_entry_2(
      std::string(APP_P4RT_TUNNEL_TABLE_NAME) + kTableKeyDelimiter + j.dump(),
      SET_COMMAND, fvs);
  j[prependMatchField(p4orch::kTunnelId)] = "3";
  swss::KeyOpFieldsValuesTuple app_db_entry_3(
      std::string(APP_P4RT_TUNNEL_TABLE_NAME) + kTableKeyDelimiter + j.dump(),
      SET_COMMAND, fvs);

  Enqueue(app_db_entry_1);
  Enqueue(app_db_entry_2);
  Enqueue(app_db_entry_3);

  EXPECT_CALL(publisher_,
              publish(Eq(APP_P4RT_TABLE_NAME), Eq(kfvKey(app_db_entry_1)),
                      Eq(kfvFieldsValues(app_db_entry_1)),
                      Eq(StatusCode::SWSS_RC_NOT_EXECUTED), Eq(true)));
  EXPECT_CALL(publisher_,
              publish(Eq(APP_P4RT_TABLE_NAME), Eq(kfvKey(app_db_entry_2)),
                      Eq(kfvFieldsValues(app_db_entry_2)),
                      Eq(StatusCode::SWSS_RC_NOT_EXECUTED), Eq(true)));
  EXPECT_CALL(publisher_,
              publish(Eq(APP_P4RT_TABLE_NAME), Eq(kfvKey(app_db_entry_3)),
                      Eq(kfvFieldsValues(app_db_entry_3)),
                      Eq(StatusCode::SWSS_RC_NOT_EXECUTED), Eq(true)));
  EXPECT_EQ(StatusCode::SWSS_RC_NOT_EXECUTED, Drain(/*failure_before=*/true));
  EXPECT_EQ(nullptr, GetGreTunnelEntry(KeyGenerator::generateTunnelKey("1")));
  EXPECT_EQ(nullptr, GetGreTunnelEntry(KeyGenerator::generateTunnelKey("2")));
  EXPECT_EQ(nullptr, GetGreTunnelEntry(KeyGenerator::generateTunnelKey("3")));
  EXPECT_FALSE(p4_oid_mapper_.existsOID(SAI_OBJECT_TYPE_TUNNEL,
                                        KeyGenerator::generateTunnelKey("1")));
  EXPECT_FALSE(p4_oid_mapper_.existsOID(SAI_OBJECT_TYPE_TUNNEL,
                                        KeyGenerator::generateTunnelKey("2")));
  EXPECT_FALSE(p4_oid_mapper_.existsOID(SAI_OBJECT_TYPE_TUNNEL,
                                        KeyGenerator::generateTunnelKey("3")));

  EXPECT_TRUE(ValidateRefCnt(SAI_OBJECT_TYPE_ROUTER_INTERFACE,
                             router_interface_key, 0));
}

TEST_F(GreTunnelManagerTest, DrainStopOnFirstFailureCreate) {
  EXPECT_TRUE(p4_oid_mapper_.setDummyOID(
      SAI_OBJECT_TYPE_NEIGHBOR_ENTRY,
      KeyGenerator::generateNeighborKey(
          kP4GreTunnelAppDbEntry1.router_interface_id,
          kP4GreTunnelAppDbEntry1.encap_dst_ip)));
  EXPECT_TRUE(p4_oid_mapper_.setDummyOID(
      SAI_OBJECT_TYPE_NEIGHBOR_ENTRY,
      KeyGenerator::generateNeighborKey(
          kP4GreTunnelAppDbEntry3.router_interface_id,
          kP4GreTunnelAppDbEntry3.encap_dst_ip)));
  const std::string neighbor_key = KeyGenerator::generateNeighborKey(
      kP4GreTunnelAppDbEntry2.router_interface_id,
      kP4GreTunnelAppDbEntry2.encap_dst_ip);
  EXPECT_TRUE(
      p4_oid_mapper_.setDummyOID(SAI_OBJECT_TYPE_NEIGHBOR_ENTRY, neighbor_key));
  uint32_t original_neighbor_ref_count;
  EXPECT_TRUE(p4_oid_mapper_.getRefCount(SAI_OBJECT_TYPE_NEIGHBOR_ENTRY,
                                         neighbor_key,
                                         &original_neighbor_ref_count));
  const std::string router_interface_key_2 =
      KeyGenerator::generateRouterInterfaceKey(kRouterInterfaceId2);

  EXPECT_TRUE(p4_oid_mapper_.setOID(
      SAI_OBJECT_TYPE_ROUTER_INTERFACE,
      KeyGenerator::generateRouterInterfaceKey(kRouterInterfaceId1),
      kRouterInterfaceOid1));
  EXPECT_TRUE(p4_oid_mapper_.setOID(SAI_OBJECT_TYPE_ROUTER_INTERFACE,
                                    router_interface_key_2,
                                    kRouterInterfaceOid2));
  EXPECT_TRUE(p4_oid_mapper_.setOID(
      SAI_OBJECT_TYPE_ROUTER_INTERFACE,
      KeyGenerator::generateRouterInterfaceKey(kRouterInterfaceId3),
      kRouterInterfaceOid3));

  uint32_t original_rif_ref_count;
  EXPECT_TRUE(p4_oid_mapper_.getRefCount(SAI_OBJECT_TYPE_ROUTER_INTERFACE,
                                         router_interface_key_2,
                                         &original_rif_ref_count));

  std::vector<swss::FieldValueTuple> fvs1{
      {p4orch::kAction, p4orch::kTunnelAction},
      {prependParamField(p4orch::kRouterInterfaceId),
       kP4GreTunnelAppDbEntry1.router_interface_id},
      {prependParamField(p4orch::kEncapSrcIp),
       kP4GreTunnelAppDbEntry1.encap_src_ip.to_string()},
      {prependParamField(p4orch::kEncapDstIp),
       kP4GreTunnelAppDbEntry1.encap_dst_ip.to_string()}};
  std::vector<swss::FieldValueTuple> fvs2{
      {p4orch::kAction, p4orch::kTunnelAction},
      {prependParamField(p4orch::kRouterInterfaceId),
       kP4GreTunnelAppDbEntry2.router_interface_id},
      {prependParamField(p4orch::kEncapSrcIp),
       kP4GreTunnelAppDbEntry2.encap_src_ip.to_string()},
      {prependParamField(p4orch::kEncapDstIp),
       kP4GreTunnelAppDbEntry2.encap_dst_ip.to_string()}};
  std::vector<swss::FieldValueTuple> fvs3{
      {p4orch::kAction, p4orch::kTunnelAction},
      {prependParamField(p4orch::kRouterInterfaceId),
       kP4GreTunnelAppDbEntry3.router_interface_id},
      {prependParamField(p4orch::kEncapSrcIp),
       kP4GreTunnelAppDbEntry3.encap_src_ip.to_string()},
      {prependParamField(p4orch::kEncapDstIp),
       kP4GreTunnelAppDbEntry3.encap_dst_ip.to_string()}};

  nlohmann::json j;
  j[prependMatchField(p4orch::kTunnelId)] = "tunnel-1";
  swss::KeyOpFieldsValuesTuple app_db_entry_1(
      std::string(APP_P4RT_TUNNEL_TABLE_NAME) + kTableKeyDelimiter + j.dump(),
      SET_COMMAND, fvs1);
  j[prependMatchField(p4orch::kTunnelId)] = "tunnel-2";
  swss::KeyOpFieldsValuesTuple app_db_entry_2(
      std::string(APP_P4RT_TUNNEL_TABLE_NAME) + kTableKeyDelimiter + j.dump(),
      SET_COMMAND, fvs2);
  j[prependMatchField(p4orch::kTunnelId)] = "tunnel-3";
  swss::KeyOpFieldsValuesTuple app_db_entry_3(
      std::string(APP_P4RT_TUNNEL_TABLE_NAME) + kTableKeyDelimiter + j.dump(),
      SET_COMMAND, fvs3);

  Enqueue(app_db_entry_1);
  Enqueue(app_db_entry_2);
  Enqueue(app_db_entry_3);

  std::vector<sai_status_t> exp_status{SAI_STATUS_SUCCESS, SAI_STATUS_FAILURE,
                                       SAI_STATUS_NOT_EXECUTED};
  EXPECT_CALL(mock_sai_tunnel_, create_tunnels(_, Eq(3), _, _, _, _, _))
      .WillOnce(DoAll(SetArrayArgument<6>(exp_status.begin(), exp_status.end()),
                      SetArgPointee<5>(kGreTunnelOid1),
                      Return(SAI_STATUS_FAILURE)));

  EXPECT_CALL(publisher_,
              publish(Eq(APP_P4RT_TABLE_NAME), Eq(kfvKey(app_db_entry_1)),
                      Eq(kfvFieldsValues(app_db_entry_1)),
                      Eq(StatusCode::SWSS_RC_SUCCESS), Eq(true)));
  EXPECT_CALL(publisher_,
              publish(Eq(APP_P4RT_TABLE_NAME), Eq(kfvKey(app_db_entry_2)),
                      Eq(kfvFieldsValues(app_db_entry_2)),
                      Eq(StatusCode::SWSS_RC_UNKNOWN), Eq(true)));
  EXPECT_CALL(publisher_,
              publish(Eq(APP_P4RT_TABLE_NAME), Eq(kfvKey(app_db_entry_3)),
                      Eq(kfvFieldsValues(app_db_entry_3)),
                      Eq(StatusCode::SWSS_RC_NOT_EXECUTED), Eq(true)));
  EXPECT_EQ(StatusCode::SWSS_RC_UNKNOWN, Drain(/*failure_before=*/false));

  const auto gre_tunnel_key_1 = KeyGenerator::generateTunnelKey("tunnel-1");
  EXPECT_NE(nullptr, GetGreTunnelEntry(gre_tunnel_key_1));
  EXPECT_TRUE(
      p4_oid_mapper_.existsOID(SAI_OBJECT_TYPE_TUNNEL, gre_tunnel_key_1));

  const auto router_interface_key_1 = KeyGenerator::generateRouterInterfaceKey(
      kP4GreTunnelAppDbEntry1.router_interface_id);
  EXPECT_TRUE(ValidateRefCnt(SAI_OBJECT_TYPE_ROUTER_INTERFACE,
                             router_interface_key_1, 1));

  const auto gre_tunnel_key_2 = KeyGenerator::generateTunnelKey("tunnel-2");
  EXPECT_EQ(nullptr, GetGreTunnelEntry(gre_tunnel_key_2));
  EXPECT_FALSE(
      p4_oid_mapper_.existsOID(SAI_OBJECT_TYPE_TUNNEL, gre_tunnel_key_2));

  EXPECT_TRUE(ValidateRefCnt(SAI_OBJECT_TYPE_ROUTER_INTERFACE,
                             router_interface_key_2, original_rif_ref_count));
  EXPECT_TRUE(ValidateRefCnt(SAI_OBJECT_TYPE_NEIGHBOR_ENTRY, neighbor_key,
                             original_neighbor_ref_count));

  const auto gre_tunnel_key_3 = KeyGenerator::generateTunnelKey("tunnel-3");
  EXPECT_EQ(nullptr, GetGreTunnelEntry(gre_tunnel_key_3));
  EXPECT_FALSE(
      p4_oid_mapper_.existsOID(SAI_OBJECT_TYPE_TUNNEL, gre_tunnel_key_3));

  const auto router_interface_key_3 = KeyGenerator::generateRouterInterfaceKey(
      kP4GreTunnelAppDbEntry3.router_interface_id);
  EXPECT_TRUE(ValidateRefCnt(SAI_OBJECT_TYPE_ROUTER_INTERFACE,
                             router_interface_key_3, 0));
}

TEST_F(GreTunnelManagerTest, DrainStopOnFirstFailureDel) {
  EXPECT_TRUE(p4_oid_mapper_.setDummyOID(
      SAI_OBJECT_TYPE_NEIGHBOR_ENTRY,
      KeyGenerator::generateNeighborKey(
          kP4GreTunnelAppDbEntry1.router_interface_id,
          kP4GreTunnelAppDbEntry1.encap_dst_ip)));
  EXPECT_TRUE(p4_oid_mapper_.setDummyOID(
      SAI_OBJECT_TYPE_NEIGHBOR_ENTRY,
      KeyGenerator::generateNeighborKey(
          kP4GreTunnelAppDbEntry2.router_interface_id,
          kP4GreTunnelAppDbEntry2.encap_dst_ip)));
  EXPECT_TRUE(p4_oid_mapper_.setDummyOID(
      SAI_OBJECT_TYPE_NEIGHBOR_ENTRY,
      KeyGenerator::generateNeighborKey(
          kP4GreTunnelAppDbEntry3.router_interface_id,
          kP4GreTunnelAppDbEntry3.encap_dst_ip)));
  EXPECT_TRUE(p4_oid_mapper_.setOID(
      SAI_OBJECT_TYPE_ROUTER_INTERFACE,
      KeyGenerator::generateRouterInterfaceKey(kRouterInterfaceId1),
      kRouterInterfaceOid1));
  EXPECT_TRUE(p4_oid_mapper_.setOID(
      SAI_OBJECT_TYPE_ROUTER_INTERFACE,
      KeyGenerator::generateRouterInterfaceKey(kRouterInterfaceId2),
      kRouterInterfaceOid2));
  EXPECT_TRUE(p4_oid_mapper_.setOID(
      SAI_OBJECT_TYPE_ROUTER_INTERFACE,
      KeyGenerator::generateRouterInterfaceKey(kRouterInterfaceId3),
      kRouterInterfaceOid3));

  std::vector<sai_status_t> create_status{
      SAI_STATUS_SUCCESS, SAI_STATUS_SUCCESS, SAI_STATUS_SUCCESS};
  std::vector<sai_object_id_t> tunnel_oids{kGreTunnelOid1, kGreTunnelOid2,
                                           kGreTunnelOid3};

  EXPECT_CALL(mock_sai_tunnel_, create_tunnels(_, Eq(3), _, _, _, _, _))
      .WillOnce(
          DoAll(SetArrayArgument<5>(tunnel_oids.begin(), tunnel_oids.end()),
                SetArrayArgument<6>(create_status.begin(), create_status.end()),
                Return(SAI_STATUS_SUCCESS)));

  EXPECT_THAT(CreateGreTunnels(std::vector<P4GreTunnelAppDbEntry>{
                  kP4GreTunnelAppDbEntry1, kP4GreTunnelAppDbEntry2,
                  kP4GreTunnelAppDbEntry3}),
              ArrayEq(std::vector<StatusCode>{StatusCode::SWSS_RC_SUCCESS,
                                              StatusCode::SWSS_RC_SUCCESS,
                                              StatusCode::SWSS_RC_SUCCESS}));

  const auto gre_tunnel_key_1 = KeyGenerator::generateTunnelKey("tunnel-1");
  EXPECT_NE(nullptr, GetGreTunnelEntry(gre_tunnel_key_1));
  EXPECT_TRUE(
      p4_oid_mapper_.existsOID(SAI_OBJECT_TYPE_TUNNEL, gre_tunnel_key_1));

  const auto router_interface_key_1 = KeyGenerator::generateRouterInterfaceKey(
      kP4GreTunnelAppDbEntry1.router_interface_id);
  EXPECT_TRUE(ValidateRefCnt(SAI_OBJECT_TYPE_ROUTER_INTERFACE,
                             router_interface_key_1, 1));

  const auto gre_tunnel_key_2 = KeyGenerator::generateTunnelKey("tunnel-2");
  EXPECT_NE(nullptr, GetGreTunnelEntry(gre_tunnel_key_2));
  EXPECT_TRUE(
      p4_oid_mapper_.existsOID(SAI_OBJECT_TYPE_TUNNEL, gre_tunnel_key_2));

  const auto router_interface_key_2 = KeyGenerator::generateRouterInterfaceKey(
      kP4GreTunnelAppDbEntry2.router_interface_id);
  EXPECT_TRUE(ValidateRefCnt(SAI_OBJECT_TYPE_ROUTER_INTERFACE,
                             router_interface_key_2, 1));

  const auto gre_tunnel_key_3 = KeyGenerator::generateTunnelKey("tunnel-3");
  EXPECT_NE(nullptr, GetGreTunnelEntry(gre_tunnel_key_3));
  EXPECT_TRUE(
      p4_oid_mapper_.existsOID(SAI_OBJECT_TYPE_TUNNEL, gre_tunnel_key_3));

  const auto router_interface_key_3 = KeyGenerator::generateRouterInterfaceKey(
      kP4GreTunnelAppDbEntry3.router_interface_id);
  EXPECT_TRUE(ValidateRefCnt(SAI_OBJECT_TYPE_ROUTER_INTERFACE,
                             router_interface_key_3, 1));

  std::vector<swss::FieldValueTuple> fvs1{
      {p4orch::kAction, p4orch::kTunnelAction},
      {prependParamField(p4orch::kRouterInterfaceId),
       kP4GreTunnelAppDbEntry1.router_interface_id},
      {prependParamField(p4orch::kEncapSrcIp),
       kP4GreTunnelAppDbEntry1.encap_src_ip.to_string()},
      {prependParamField(p4orch::kEncapDstIp),
       kP4GreTunnelAppDbEntry1.encap_dst_ip.to_string()}};
  std::vector<swss::FieldValueTuple> fvs2{
      {p4orch::kAction, p4orch::kTunnelAction},
      {prependParamField(p4orch::kRouterInterfaceId),
       kP4GreTunnelAppDbEntry2.router_interface_id},
      {prependParamField(p4orch::kEncapSrcIp),
       kP4GreTunnelAppDbEntry2.encap_src_ip.to_string()},
      {prependParamField(p4orch::kEncapDstIp),
       kP4GreTunnelAppDbEntry2.encap_dst_ip.to_string()}};
  std::vector<swss::FieldValueTuple> fvs3{
      {p4orch::kAction, p4orch::kTunnelAction},
      {prependParamField(p4orch::kRouterInterfaceId),
       kP4GreTunnelAppDbEntry3.router_interface_id},
      {prependParamField(p4orch::kEncapSrcIp),
       kP4GreTunnelAppDbEntry3.encap_src_ip.to_string()},
      {prependParamField(p4orch::kEncapDstIp),
       kP4GreTunnelAppDbEntry3.encap_dst_ip.to_string()}};

  nlohmann::json j;
  j[prependMatchField(p4orch::kTunnelId)] = "tunnel-1";
  swss::KeyOpFieldsValuesTuple app_db_entry_1(
      std::string(APP_P4RT_TUNNEL_TABLE_NAME) + kTableKeyDelimiter + j.dump(),
      DEL_COMMAND, fvs1);
  j[prependMatchField(p4orch::kTunnelId)] = "tunnel-2";
  swss::KeyOpFieldsValuesTuple app_db_entry_2(
      std::string(APP_P4RT_TUNNEL_TABLE_NAME) + kTableKeyDelimiter + j.dump(),
      DEL_COMMAND, fvs2);
  j[prependMatchField(p4orch::kTunnelId)] = "tunnel-3";
  swss::KeyOpFieldsValuesTuple app_db_entry_3(
      std::string(APP_P4RT_TUNNEL_TABLE_NAME) + kTableKeyDelimiter + j.dump(),
      DEL_COMMAND, fvs3);

  Enqueue(app_db_entry_1);
  Enqueue(app_db_entry_2);
  Enqueue(app_db_entry_3);

  std::vector<sai_status_t> exp_status{SAI_STATUS_SUCCESS, SAI_STATUS_FAILURE,
                                       SAI_STATUS_NOT_EXECUTED};
  EXPECT_CALL(mock_sai_tunnel_, remove_tunnels(Eq(3), _, _, _))
      .WillOnce(DoAll(SetArrayArgument<3>(exp_status.begin(), exp_status.end()),
                      Return(SAI_STATUS_FAILURE)));

  EXPECT_CALL(publisher_,
              publish(Eq(APP_P4RT_TABLE_NAME), Eq(kfvKey(app_db_entry_1)),
                      Eq(kfvFieldsValues(app_db_entry_1)),
                      Eq(StatusCode::SWSS_RC_SUCCESS), Eq(true)));
  EXPECT_CALL(publisher_,
              publish(Eq(APP_P4RT_TABLE_NAME), Eq(kfvKey(app_db_entry_2)),
                      Eq(kfvFieldsValues(app_db_entry_2)),
                      Eq(StatusCode::SWSS_RC_UNKNOWN), Eq(true)));
  EXPECT_CALL(publisher_,
              publish(Eq(APP_P4RT_TABLE_NAME), Eq(kfvKey(app_db_entry_3)),
                      Eq(kfvFieldsValues(app_db_entry_3)),
                      Eq(StatusCode::SWSS_RC_NOT_EXECUTED), Eq(true)));
  EXPECT_EQ(StatusCode::SWSS_RC_UNKNOWN, Drain(/*failure_before=*/false));
  EXPECT_EQ(nullptr, GetGreTunnelEntry(gre_tunnel_key_1));
  EXPECT_FALSE(
      p4_oid_mapper_.existsOID(SAI_OBJECT_TYPE_TUNNEL, gre_tunnel_key_1));
  EXPECT_TRUE(ValidateRefCnt(SAI_OBJECT_TYPE_ROUTER_INTERFACE,
                             router_interface_key_1, 0));

  EXPECT_NE(nullptr, GetGreTunnelEntry(gre_tunnel_key_2));
  EXPECT_TRUE(
      p4_oid_mapper_.existsOID(SAI_OBJECT_TYPE_TUNNEL, gre_tunnel_key_2));
  EXPECT_TRUE(ValidateRefCnt(SAI_OBJECT_TYPE_ROUTER_INTERFACE,
                             router_interface_key_2, 1));

  EXPECT_NE(nullptr, GetGreTunnelEntry(gre_tunnel_key_3));
  EXPECT_TRUE(
      p4_oid_mapper_.existsOID(SAI_OBJECT_TYPE_TUNNEL, gre_tunnel_key_3));
  EXPECT_TRUE(ValidateRefCnt(SAI_OBJECT_TYPE_ROUTER_INTERFACE,
                             router_interface_key_3, 1));
}

TEST_F(GreTunnelManagerTest, DrainStopOnFirstFailureDifferentTypes) {
  AddGreTunnelEntry1();

  EXPECT_TRUE(p4_oid_mapper_.setDummyOID(
      SAI_OBJECT_TYPE_NEIGHBOR_ENTRY,
      KeyGenerator::generateNeighborKey(
          kP4GreTunnelAppDbEntry2.router_interface_id,
          kP4GreTunnelAppDbEntry2.encap_dst_ip)));
  EXPECT_TRUE(p4_oid_mapper_.setOID(
      SAI_OBJECT_TYPE_ROUTER_INTERFACE,
      KeyGenerator::generateRouterInterfaceKey(kRouterInterfaceId2),
      kRouterInterfaceOid2));

  std::vector<swss::FieldValueTuple> fvs1{
      {p4orch::kAction, p4orch::kTunnelAction},
      {prependParamField(p4orch::kRouterInterfaceId),
       kP4GreTunnelAppDbEntry1.router_interface_id},
      {prependParamField(p4orch::kEncapSrcIp),
       kP4GreTunnelAppDbEntry1.encap_src_ip.to_string()},
      {prependParamField(p4orch::kEncapDstIp),
       kP4GreTunnelAppDbEntry1.encap_dst_ip.to_string()}};
  std::vector<swss::FieldValueTuple> fvs2{
      {p4orch::kAction, p4orch::kTunnelAction},
      {prependParamField(p4orch::kRouterInterfaceId),
       kP4GreTunnelAppDbEntry2.router_interface_id},
      {prependParamField(p4orch::kEncapSrcIp),
       kP4GreTunnelAppDbEntry2.encap_src_ip.to_string()},
      {prependParamField(p4orch::kEncapDstIp),
       kP4GreTunnelAppDbEntry2.encap_dst_ip.to_string()}};

  nlohmann::json j;
  j[prependMatchField(p4orch::kTunnelId)] = "tunnel-1";
  swss::KeyOpFieldsValuesTuple app_db_entry_1(
      std::string(APP_P4RT_TUNNEL_TABLE_NAME) + kTableKeyDelimiter + j.dump(),
      SET_COMMAND, fvs1);
  j[prependMatchField(p4orch::kTunnelId)] = "tunnel-2";
  swss::KeyOpFieldsValuesTuple app_db_entry_2(
      std::string(APP_P4RT_TUNNEL_TABLE_NAME) + kTableKeyDelimiter + j.dump(),
      SET_COMMAND, fvs2);

  Enqueue(app_db_entry_2);
  Enqueue(app_db_entry_1);
  Enqueue(app_db_entry_2);

  std::vector<sai_status_t> create_status{SAI_STATUS_SUCCESS};
  std::vector<sai_object_id_t> tunnel_oids{kGreTunnelOid1};

  EXPECT_CALL(mock_sai_tunnel_, create_tunnels(_, Eq(1), _, _, _, _, _))
      .WillOnce(
          DoAll(SetArrayArgument<5>(tunnel_oids.begin(), tunnel_oids.end()),
                SetArrayArgument<6>(create_status.begin(), create_status.end()),
                Return(SAI_STATUS_SUCCESS)));

  EXPECT_CALL(publisher_,
              publish(Eq(APP_P4RT_TABLE_NAME), Eq(kfvKey(app_db_entry_2)),
                      Eq(kfvFieldsValues(app_db_entry_2)),
                      Eq(StatusCode::SWSS_RC_SUCCESS), Eq(true)));
  EXPECT_CALL(publisher_,
              publish(Eq(APP_P4RT_TABLE_NAME), Eq(kfvKey(app_db_entry_1)),
                      Eq(kfvFieldsValues(app_db_entry_1)),
                      Eq(StatusCode::SWSS_RC_UNIMPLEMENTED), Eq(true)));
  EXPECT_CALL(publisher_,
              publish(Eq(APP_P4RT_TABLE_NAME), Eq(kfvKey(app_db_entry_2)),
                      Eq(kfvFieldsValues(app_db_entry_2)),
                      Eq(StatusCode::SWSS_RC_NOT_EXECUTED), Eq(true)));
  EXPECT_EQ(StatusCode::SWSS_RC_UNIMPLEMENTED, Drain(/*failure_before=*/false));

  const auto gre_tunnel_key_1 = KeyGenerator::generateTunnelKey("tunnel-1");
  EXPECT_NE(nullptr, GetGreTunnelEntry(gre_tunnel_key_1));
  EXPECT_TRUE(
      p4_oid_mapper_.existsOID(SAI_OBJECT_TYPE_TUNNEL, gre_tunnel_key_1));

  const auto router_interface_key_1 = KeyGenerator::generateRouterInterfaceKey(
      kP4GreTunnelAppDbEntry1.router_interface_id);
  EXPECT_TRUE(ValidateRefCnt(SAI_OBJECT_TYPE_ROUTER_INTERFACE,
                             router_interface_key_1, 1));
  // Verify that the update request failed.
  EXPECT_TRUE(ValidateGreTunnelEntryAdd(kP4GreTunnelAppDbEntry1));

  const auto gre_tunnel_key_2 = KeyGenerator::generateTunnelKey("tunnel-2");
  EXPECT_NE(nullptr, GetGreTunnelEntry(gre_tunnel_key_2));
  EXPECT_TRUE(
      p4_oid_mapper_.existsOID(SAI_OBJECT_TYPE_TUNNEL, gre_tunnel_key_2));

  const auto router_interface_key_2 = KeyGenerator::generateRouterInterfaceKey(
      kP4GreTunnelAppDbEntry2.router_interface_id);
  EXPECT_TRUE(ValidateRefCnt(SAI_OBJECT_TYPE_ROUTER_INTERFACE,
                             router_interface_key_2, 1));
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
