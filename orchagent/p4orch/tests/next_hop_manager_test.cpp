#include "next_hop_manager.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <functional>
#include <nlohmann/json.hpp>
#include <string>
#include <unordered_map>

#include "ipaddress.h"
#include "mock_response_publisher.h"
#include "mock_sai_hostif.h"
#include "mock_sai_next_hop.h"
#include "mock_sai_serialize.h"
#include "mock_sai_switch.h"
#include "p4oidmapper.h"
#include "p4orch.h"
#include "return_code.h"
#include "swssnet.h"

#define APP_P4RT_TABLE_NAME_SEPARATOR ":"

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
extern MockSaiNextHop *mock_sai_next_hop;
extern P4Orch *gP4Orch;
extern VRFOrch *gVrfOrch;
extern swss::DBConnector *gAppDb;
extern sai_hostif_api_t *sai_hostif_api;
extern sai_switch_api_t *sai_switch_api;
extern sai_next_hop_api_t *sai_next_hop_api;

namespace
{

constexpr char *kNextHopId = "8";
constexpr char *kNextHopP4AppDbKey = R"({"match/nexthop_id":"8"})";
constexpr sai_object_id_t kNextHopOid = 101;
constexpr char *kTunnelNextHopId = "tunnel-nexthop-1";
constexpr char *kTunnelNextHopP4AppDbKey = R"({"match/nexthop_id":"tunnel-nexthop-1"})";
constexpr sai_object_id_t kTunnelNextHopOid = 102;
constexpr char *kRouterInterfaceId1 = "16";
constexpr char *kRouterInterfaceId2 = "17";
constexpr sai_object_id_t kRouterInterfaceOid1 = 1;
constexpr sai_object_id_t kRouterInterfaceOid2 = 2;
constexpr sai_object_id_t kRouterInterfaceOid3 = 3;
constexpr char *kTunnelId1 = "tunnel-1";
constexpr char *kTunnelId2 = "tunnel-2";
constexpr sai_object_id_t kTunnelOid1 = 11;
constexpr sai_object_id_t kTunnelOid2 = 12;
constexpr char *kNeighborId1 = "10.0.0.1";
constexpr char *kNeighborId2 = "fe80::21a:11ff:fe17:5f80";

MATCHER_P(ArrayEq, array, "") {
  for (size_t i = 0; i < array.size(); ++i) {
    if (arg[i] != array[i]) {
      return false;
    }
  }
  return true;
}

// APP DB entries for Add and Update request.
const P4NextHopAppDbEntry kP4NextHopAppDbEntry1{/*next_hop_id=*/kNextHopId,
                                                /*router_interface_id=*/kRouterInterfaceId1,
                                                /*gre_tunnel_id=*/"",
                                                /*neighbor_id=*/swss::IpAddress(kNeighborId1),
                                                /*action_str=*/"set_ip_nexthop"};

const P4NextHopAppDbEntry kP4NextHopAppDbEntry2{/*next_hop_id=*/kNextHopId,
                                                /*router_interface_id=*/kRouterInterfaceId2,
                                                /*gre_tunnel_id=*/"",
                                                /*neighbor_id=*/swss::IpAddress(kNeighborId2),
                                                /*action_str=*/"set_ip_nexthop"};

// APP DB entries for Delete request.
const P4NextHopAppDbEntry kP4NextHopAppDbEntry3{/*next_hop_id=*/kNextHopId,
                                                /*router_interface_id=*/"",
                                                /*gre_tunnel_id=*/"",
                                                /*neighbor_id=*/swss::IpAddress(),
                                                /*action_str=*/""};

// APP DB entry for tunnel next hop entry
const P4NextHopAppDbEntry kP4TunnelNextHopAppDbEntry1{/*next_hop_id=*/kTunnelNextHopId,
                                                      /*router_interface_id=*/"",
                                                      /*gre_tunnel_id=*/kTunnelId1,
                                                      /*neighbor_id=*/swss::IpAddress("0.0.0.0"),
                                                      /*action_str=*/"set_p2p_tunnel_encap_nexthop"};

const P4NextHopAppDbEntry kP4TunnelNextHopAppDbEntry2{/*next_hop_id=*/kTunnelNextHopId,
                                                      /*router_interface_id=*/"",
                                                      /*gre_tunnel_id=*/kTunnelId2,
                                                      /*neighbor_id=*/swss::IpAddress("0.0.0.0"),
                                                      /*action_str=*/"set_p2p_tunnel_encap_nexthop"};

const P4GreTunnelEntry kP4TunnelEntry1(
    /*tunnel_id=*/kTunnelId1,
    /*router_interface_id=*/kRouterInterfaceId1,
    /*encap_src_ip=*/swss::IpAddress("1.2.3.4"),
    /*encap_dst_ip=*/swss::IpAddress(kNeighborId1),
    /*neighbor_id=*/swss::IpAddress(kNeighborId1));

const P4GreTunnelEntry kP4TunnelEntry2(
    /*tunnel_id=*/kTunnelId2,
    /*router_interface_id=*/kRouterInterfaceId2,
    /*encap_src_ip=*/swss::IpAddress("1.2.3.4"),
    /*encap_dst_ip=*/swss::IpAddress(kNeighborId2),
    /*neighbor_id=*/swss::IpAddress(kNeighborId2));

std::unordered_map<sai_attr_id_t, sai_attribute_value_t> CreateAttributeListForNextHopObject(
    const P4NextHopAppDbEntry &app_entry, const sai_object_id_t &oid,
    const swss::IpAddress &neighbor_id = swss::IpAddress("0.0.0.0"))
{
    std::unordered_map<sai_attr_id_t, sai_attribute_value_t> next_hop_attrs;
    sai_attribute_t next_hop_attr;

    if (app_entry.action_str == p4orch::kSetTunnelNexthop)
    {
        next_hop_attr.id = SAI_NEXT_HOP_ATTR_TYPE;
        next_hop_attr.value.s32 = SAI_NEXT_HOP_TYPE_TUNNEL_ENCAP;
        next_hop_attrs.insert({next_hop_attr.id, next_hop_attr.value});
        next_hop_attr.id = SAI_NEXT_HOP_ATTR_TUNNEL_ID;
        next_hop_attr.value.oid = oid;
        next_hop_attrs.insert({next_hop_attr.id, next_hop_attr.value});
    }
    else
    {
        next_hop_attr.id = SAI_NEXT_HOP_ATTR_TYPE;
        next_hop_attr.value.s32 = SAI_NEXT_HOP_TYPE_IP;
        next_hop_attrs.insert({next_hop_attr.id, next_hop_attr.value});
        next_hop_attr.id = SAI_NEXT_HOP_ATTR_ROUTER_INTERFACE_ID;
        next_hop_attr.value.oid = oid;
        next_hop_attrs.insert({next_hop_attr.id, next_hop_attr.value});
    }

    next_hop_attr.id = SAI_NEXT_HOP_ATTR_IP;
    if (!neighbor_id.isZero())
    {
        swss::copy(next_hop_attr.value.ipaddr, neighbor_id);
    }
    else
    {
        swss::copy(next_hop_attr.value.ipaddr, app_entry.neighbor_id);
    }
    next_hop_attrs.insert({next_hop_attr.id, next_hop_attr.value});

    return next_hop_attrs;
}

// Verifies whether the attribute list is the same as expected for SAI next
// hop's create_next_hops().
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
        (expected_attr_list.find(SAI_NEXT_HOP_ATTR_ROUTER_INTERFACE_ID) == end &&
         expected_attr_list.find(SAI_NEXT_HOP_ATTR_TUNNEL_ID) == end))
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
        case SAI_NEXT_HOP_ATTR_TUNNEL_ID:
            if (attr_list[i].value.oid != expected_attr_list.at(SAI_NEXT_HOP_ATTR_TUNNEL_ID).oid)
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

MATCHER_P(AttrArrayEq, array, "") {
  for (size_t i = 0; i < array.size(); ++i) {
    if (!MatchCreateNextHopArgAttrList(arg[i], array[i])) {
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
        mock_sai_hostif = &mock_sai_hostif_;
        mock_sai_switch = &mock_sai_switch_;
        sai_switch_api->get_switch_attribute = mock_get_switch_attribute;
        sai_hostif_api->create_hostif_trap = mock_create_hostif_trap;
        sai_hostif_api->create_hostif_table_entry = mock_create_hostif_table_entry;
        EXPECT_CALL(mock_sai_hostif_, create_hostif_table_entry(_, _, _, _)).WillRepeatedly(Return(SAI_STATUS_SUCCESS));
        EXPECT_CALL(mock_sai_hostif_, create_hostif_trap(_, _, _, _)).WillOnce(Return(SAI_STATUS_SUCCESS));
        EXPECT_CALL(mock_sai_switch_, get_switch_attribute(_, _, _)).WillRepeatedly(Return(SAI_STATUS_SUCCESS));
        copp_orch_ = new CoppOrch(gAppDb, APP_COPP_TABLE_NAME);
        std::vector<std::string> p4_tables;
        gP4Orch = new P4Orch(gAppDb, p4_tables, gVrfOrch, copp_orch_);
    }

    ~NextHopManagerTest()
    {
        delete gP4Orch;
        delete copp_orch_;
    }

    void SetUp() override
    {
        // Set up mock stuff for SAI next hop API structure.
        mock_sai_next_hop = &mock_sai_next_hop_;
        sai_next_hop_api->create_next_hop = mock_create_next_hop;
        sai_next_hop_api->remove_next_hop = mock_remove_next_hop;
        sai_next_hop_api->set_next_hop_attribute = mock_set_next_hop_attribute;
        sai_next_hop_api->get_next_hop_attribute = mock_get_next_hop_attribute;
        sai_next_hop_api->create_next_hops = mock_create_next_hops;
        sai_next_hop_api->remove_next_hops = mock_remove_next_hops;
    }

    void TearDown() override
    {
        gP4Orch->getGreTunnelManager()->m_greTunnelTable.clear();
    }

    void Enqueue(const swss::KeyOpFieldsValuesTuple &entry)
    {
        next_hop_manager_.enqueue(APP_P4RT_NEXTHOP_TABLE_NAME, entry);
    }

    ReturnCode Drain(bool failure_before) {
      if (failure_before) {
        next_hop_manager_.drainWithNotExecuted();
        return ReturnCode(StatusCode::SWSS_RC_NOT_EXECUTED);
      }
      return next_hop_manager_.drain();
    }

    std::string VerifyState(const std::string &key, const std::vector<swss::FieldValueTuple> &tuple)
    {
        return next_hop_manager_.verifyState(key, tuple);
    }

    P4NextHopEntry* GetNextHopEntry(const std::string& next_hop_key) {
      return next_hop_manager_.getNextHopEntry(next_hop_key);
    }

    std::vector<ReturnCode> CreateNextHops(
        const std::vector<P4NextHopAppDbEntry>& entries) {
      return next_hop_manager_.createNextHops(entries);
    }

    std::vector<ReturnCode> RemoveNextHops(
        const std::vector<P4NextHopAppDbEntry>& entries) {
      return next_hop_manager_.removeNextHops(entries);
    }

    ReturnCode processEntries(
        const std::vector<P4NextHopAppDbEntry>& entries,
        const std::vector<swss::KeyOpFieldsValuesTuple>& tuple_list,
        const std::string& op, bool update) {
      return next_hop_manager_.processEntries(entries, tuple_list, op, update);
    }

    ReturnCode ValidateNextHopAppDbEntry(
        const P4NextHopAppDbEntry& app_db_entry, const std::string& operation) {
      return next_hop_manager_.validateAppDbEntry(app_db_entry, operation);
    }

    ReturnCodeOr<P4NextHopAppDbEntry> DeserializeP4NextHopAppDbEntry(
        const std::string &key, const std::vector<swss::FieldValueTuple> &attributes)
    {
        return next_hop_manager_.deserializeP4NextHopAppDbEntry(key, attributes);
    }

    // Resolves the dependency of a next hop entry by adding depended router
    // interface/tunnel and neighbor into centralized mapper.
    // Returns true on succuess.
    bool ResolveNextHopEntryDependency(const P4NextHopAppDbEntry &app_db_entry, const sai_object_id_t &rif_oid);

    // Adds the next hop entry -- kP4NextHopAppDbEntry1, via next hop manager's
    // ProcessAddRequest (). This function also takes care of all the dependencies
    // of the next hop entry.
    // Returns a valid pointer to next hop entry on success.
    P4NextHopEntry *AddNextHopEntry1();

    // Adds the next hop entry -- kP4TunnelNextHopAppDbEntry1, via next hop
    // manager's ProcessAddRequest (). This function also takes care of all the
    // dependencies of the next hop entry. Returns a valid pointer to next hop
    // entry on success.
    P4NextHopEntry *AddTunnelNextHopEntry1();

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
    StrictMock<MockResponsePublisher> publisher_;
    P4OidMapper p4_oid_mapper_;
    NextHopManager next_hop_manager_;
    StrictMock<MockSaiHostif> mock_sai_hostif_;
    StrictMock<MockSaiSwitch> mock_sai_switch_;
    CoppOrch *copp_orch_;
};

bool NextHopManagerTest::ResolveNextHopEntryDependency(const P4NextHopAppDbEntry &app_db_entry,
                                                       const sai_object_id_t &oid)
{
    std::string rif_id = app_db_entry.router_interface_id;
    auto neighbor_id = app_db_entry.neighbor_id;
    if (app_db_entry.action_str == p4orch::kSetTunnelNexthop)
    {
        const std::string tunnel_key = KeyGenerator::generateTunnelKey(app_db_entry.gre_tunnel_id);
        if (!p4_oid_mapper_.setOID(SAI_OBJECT_TYPE_TUNNEL, tunnel_key, oid))
        {
            return false;
        }
        gP4Orch->getGreTunnelManager()->m_greTunnelTable.emplace(
            tunnel_key, app_db_entry.gre_tunnel_id == kTunnelId1 ? kP4TunnelEntry1 : kP4TunnelEntry2);
        auto gre_tunnel_or = gP4Orch->getGreTunnelManager()->getConstGreTunnelEntry(tunnel_key);
        EXPECT_TRUE(gre_tunnel_or.ok());
        rif_id = (*gre_tunnel_or).router_interface_id;
        auto rif_oid = rif_id == kRouterInterfaceId1 ? kRouterInterfaceOid1 : kRouterInterfaceOid2;
        neighbor_id = (*gre_tunnel_or).neighbor_id;
        const std::string rif_key = KeyGenerator::generateRouterInterfaceKey(rif_id);
        if (!p4_oid_mapper_.setOID(SAI_OBJECT_TYPE_ROUTER_INTERFACE, rif_key, rif_oid))
        {
            return false;
        }
    }
    else
    {
        const std::string rif_key = KeyGenerator::generateRouterInterfaceKey(rif_id);
        if (!p4_oid_mapper_.setOID(SAI_OBJECT_TYPE_ROUTER_INTERFACE, rif_key, oid))
        {
            return false;
        }
    }

    const std::string neighbor_key = KeyGenerator::generateNeighborKey(rif_id, neighbor_id);
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

    std::vector<sai_status_t> exp_status{SAI_STATUS_SUCCESS};

    // Set up mock call.
    EXPECT_CALL(
        mock_sai_next_hop_,
        create_next_hops(
            Eq(gSwitchId), Eq(1), Pointee(Eq(3)),
            AttrArrayEq(sai_attrs_array_t{CreateAttributeListForNextHopObject(
                kP4NextHopAppDbEntry1, kRouterInterfaceOid1)}),
            Eq(SAI_BULK_OP_ERROR_MODE_STOP_ON_ERROR), ::testing::NotNull(),
            ::testing::NotNull()))
        .WillOnce(
            DoAll(SetArrayArgument<6>(exp_status.begin(), exp_status.end()),
                  SetArgPointee<5>(kNextHopOid), Return(SAI_STATUS_SUCCESS)));

    EXPECT_THAT(
        CreateNextHops(std::vector<P4NextHopAppDbEntry>{kP4NextHopAppDbEntry1}),
        ArrayEq(std::vector<StatusCode>{StatusCode::SWSS_RC_SUCCESS}));

    return GetNextHopEntry(KeyGenerator::generateNextHopKey(kP4NextHopAppDbEntry1.next_hop_id));
}

P4NextHopEntry *NextHopManagerTest::AddTunnelNextHopEntry1()
{
    if (!ResolveNextHopEntryDependency(kP4TunnelNextHopAppDbEntry1, kTunnelOid1))
    {
        return nullptr;
    }

    std::vector<sai_status_t> exp_status{SAI_STATUS_SUCCESS};

    // Set up mock call.
    EXPECT_CALL(
        mock_sai_next_hop_,
        create_next_hops(
            Eq(gSwitchId), Eq(1), Pointee(Eq(3)),
            AttrArrayEq(sai_attrs_array_t{CreateAttributeListForNextHopObject(
                kP4TunnelNextHopAppDbEntry1, kTunnelOid1,
                swss::IpAddress(kNeighborId1))}),
            Eq(SAI_BULK_OP_ERROR_MODE_STOP_ON_ERROR), ::testing::NotNull(),
            ::testing::NotNull()))
        .WillOnce(
            DoAll(SetArrayArgument<6>(exp_status.begin(), exp_status.end()),
                  SetArgPointee<5>(kNextHopOid), Return(SAI_STATUS_SUCCESS)));

    EXPECT_THAT(CreateNextHops(std::vector<P4NextHopAppDbEntry>{
                    kP4TunnelNextHopAppDbEntry1}),
                ArrayEq(std::vector<StatusCode>{StatusCode::SWSS_RC_SUCCESS}));

    return GetNextHopEntry(KeyGenerator::generateNextHopKey(kP4TunnelNextHopAppDbEntry1.next_hop_id));
}

bool NextHopManagerTest::ValidateNextHopEntryAdd(const P4NextHopAppDbEntry &app_db_entry,
                                                 const sai_object_id_t &expected_next_hop_oid)
{
    const auto *p4_next_hop_entry = GetNextHopEntry(KeyGenerator::generateNextHopKey(app_db_entry.next_hop_id));
    if (p4_next_hop_entry == nullptr || p4_next_hop_entry->next_hop_id != app_db_entry.next_hop_id ||
        p4_next_hop_entry->next_hop_oid != expected_next_hop_oid)
    {
        return false;
    }

    if (app_db_entry.action_str == p4orch::kSetTunnelNexthop &&
        p4_next_hop_entry->gre_tunnel_id != app_db_entry.gre_tunnel_id)
    {
        return false;
    }

    if (app_db_entry.action_str == p4orch::kSetIpNexthop &&
        (p4_next_hop_entry->router_interface_id != app_db_entry.router_interface_id ||
         p4_next_hop_entry->neighbor_id != app_db_entry.neighbor_id))
    {
        return false;
    }

    sai_object_id_t next_hop_oid;
    if (!p4_oid_mapper_.getOID(SAI_OBJECT_TYPE_NEXT_HOP, p4_next_hop_entry->next_hop_key, &next_hop_oid) ||
        next_hop_oid != expected_next_hop_oid)
    {
        return false;
    }

    return true;
}

TEST_F(NextHopManagerTest, CreateRequestShouldSucceedAddingNewNextHop) {
  ASSERT_TRUE(ResolveNextHopEntryDependency(kP4NextHopAppDbEntry1,
                                            kRouterInterfaceOid1));

  const std::string rif_key = KeyGenerator::generateRouterInterfaceKey(
      kP4NextHopAppDbEntry1.router_interface_id);
  const std::string neighbor_key = KeyGenerator::generateNeighborKey(
      kP4NextHopAppDbEntry1.router_interface_id,
      kP4NextHopAppDbEntry1.neighbor_id);
  uint32_t original_rif_ref_count;
  ASSERT_TRUE(p4_oid_mapper_.getRefCount(SAI_OBJECT_TYPE_ROUTER_INTERFACE,
                                         rif_key, &original_rif_ref_count));
  uint32_t original_neighbor_ref_count;
  ASSERT_TRUE(p4_oid_mapper_.getRefCount(SAI_OBJECT_TYPE_NEIGHBOR_ENTRY,
                                         neighbor_key,
                                         &original_neighbor_ref_count));

  std::vector<sai_status_t> exp_status{SAI_STATUS_SUCCESS};

  // Set up mock call.
  EXPECT_CALL(
      mock_sai_next_hop_,
      create_next_hops(
          Eq(gSwitchId), Eq(1), Pointee(Eq(3)),
          AttrArrayEq(sai_attrs_array_t{CreateAttributeListForNextHopObject(
              kP4NextHopAppDbEntry1, kRouterInterfaceOid1)}),
          Eq(SAI_BULK_OP_ERROR_MODE_STOP_ON_ERROR), ::testing::NotNull(),
          ::testing::NotNull()))
      .WillOnce(DoAll(SetArrayArgument<6>(exp_status.begin(), exp_status.end()),
                      SetArgPointee<5>(kNextHopOid),
                      Return(SAI_STATUS_SUCCESS)));

  EXPECT_THAT(
      CreateNextHops(std::vector<P4NextHopAppDbEntry>{kP4NextHopAppDbEntry1}),
      ArrayEq(std::vector<StatusCode>{StatusCode::SWSS_RC_SUCCESS}));

  EXPECT_TRUE(ValidateNextHopEntryAdd(kP4NextHopAppDbEntry1, kNextHopOid));
  EXPECT_TRUE(ValidateRefCnt(SAI_OBJECT_TYPE_ROUTER_INTERFACE, rif_key,
                             original_rif_ref_count + 1));
  EXPECT_TRUE(ValidateRefCnt(SAI_OBJECT_TYPE_NEIGHBOR_ENTRY, neighbor_key,
                             original_neighbor_ref_count + 1));
}

TEST_F(NextHopManagerTest,
       ValidateShouldFailForCreateWhenNextHopExistInCentralMapper) {
  ASSERT_TRUE(ResolveNextHopEntryDependency(kP4NextHopAppDbEntry1,
                                            kRouterInterfaceOid1));
  ASSERT_TRUE(p4_oid_mapper_.setOID(
      SAI_OBJECT_TYPE_NEXT_HOP,
      KeyGenerator::generateNextHopKey(kP4NextHopAppDbEntry1.next_hop_id),
      kNextHopOid));
  // TODO: Expect critical state.
  EXPECT_FALSE(
      ValidateNextHopAppDbEntry(kP4NextHopAppDbEntry1, SET_COMMAND).ok());
}

TEST_F(NextHopManagerTest,
       ValidateCreateShouldFailWhenDependingRifIsAbsentInCentralMapper) {
  const std::string neighbor_key = KeyGenerator::generateNeighborKey(
      kP4NextHopAppDbEntry1.router_interface_id,
      kP4NextHopAppDbEntry1.neighbor_id);
  ASSERT_TRUE(
      p4_oid_mapper_.setDummyOID(SAI_OBJECT_TYPE_NEIGHBOR_ENTRY, neighbor_key));

  EXPECT_FALSE(
      ValidateNextHopAppDbEntry(kP4NextHopAppDbEntry1, SET_COMMAND).ok());
}

TEST_F(NextHopManagerTest,
       ValidateCreateShouldFailWhenDependingTunnelIsAbsentInCentralMapper) {
  const std::string neighbor_key = KeyGenerator::generateNeighborKey(
      kP4TunnelNextHopAppDbEntry1.router_interface_id,
      kP4TunnelEntry1.neighbor_id);
  ASSERT_TRUE(
      p4_oid_mapper_.setDummyOID(SAI_OBJECT_TYPE_NEIGHBOR_ENTRY, neighbor_key));

  EXPECT_FALSE(
      ValidateNextHopAppDbEntry(kP4TunnelNextHopAppDbEntry1, SET_COMMAND).ok());
}

TEST_F(NextHopManagerTest,
       ValidateCreateShouldFailWhenDependingNeigherIsAbsentInCentralMapper) {
  const std::string rif_key = KeyGenerator::generateRouterInterfaceKey(
      kP4NextHopAppDbEntry1.router_interface_id);
  ASSERT_TRUE(p4_oid_mapper_.setOID(SAI_OBJECT_TYPE_ROUTER_INTERFACE, rif_key,
                                    kRouterInterfaceOid1));

  EXPECT_FALSE(
      ValidateNextHopAppDbEntry(kP4TunnelNextHopAppDbEntry1, SET_COMMAND).ok());
}

TEST_F(NextHopManagerTest, CreateNextHopsShouldFailWhenSaiCallFails) {
  ASSERT_TRUE(ResolveNextHopEntryDependency(kP4NextHopAppDbEntry1,
                                            kRouterInterfaceOid1));

  std::vector<sai_status_t> exp_status{SAI_STATUS_FAILURE};

  // Set up mock call.
  EXPECT_CALL(
      mock_sai_next_hop_,

      create_next_hops(
          Eq(gSwitchId), Eq(1), Pointee(Eq(3)),
          AttrArrayEq(sai_attrs_array_t{CreateAttributeListForNextHopObject(
              kP4NextHopAppDbEntry1, kRouterInterfaceOid1)}),
          Eq(SAI_BULK_OP_ERROR_MODE_STOP_ON_ERROR), ::testing::NotNull(),
          ::testing::NotNull()))
      .WillOnce(DoAll(SetArrayArgument<6>(exp_status.begin(), exp_status.end()),
                      SetArgPointee<5>(kNextHopOid),
                      Return(SAI_STATUS_FAILURE)));

  EXPECT_THAT(
      CreateNextHops(std::vector<P4NextHopAppDbEntry>{kP4NextHopAppDbEntry1}),
      ArrayEq(std::vector<StatusCode>{StatusCode::SWSS_RC_UNKNOWN}));

  // The add request failed for the next hop entry.
  EXPECT_EQ(GetNextHopEntry(KeyGenerator::generateNextHopKey(
                kP4NextHopAppDbEntry1.next_hop_id)),
            nullptr);
}

TEST_F(NextHopManagerTest, CreateNextHopsShouldSuccessForTunnelNexthop) {
  ASSERT_TRUE(
      ResolveNextHopEntryDependency(kP4TunnelNextHopAppDbEntry1, kTunnelOid1));

  std::vector<sai_status_t> exp_status{SAI_STATUS_SUCCESS};

  // Set up mock call.
  EXPECT_CALL(mock_sai_next_hop_, create_next_hops(_, _, _, _, _, _, _))
      /*      create_next_hops(
                Eq(gSwitchId), Eq(1), Pointee(Eq(3)),
                AttrArrayEq(sai_attrs_array_t{CreateAttributeListForNextHopObject(
                    kP4TunnelNextHopAppDbEntry1, kTunnelOid1,
                    swss::IpAddress(kNeighborId1))}),
                Eq(SAI_BULK_OP_ERROR_MODE_STOP_ON_ERROR),
                ::testing::NotNull(), ::testing::NotNull()))
      */
      .WillOnce(DoAll(SetArrayArgument<6>(exp_status.begin(), exp_status.end()),
                      SetArgPointee<5>(kTunnelNextHopOid),
                      Return(SAI_STATUS_SUCCESS)));

  EXPECT_THAT(CreateNextHops(std::vector<P4NextHopAppDbEntry>{
                  kP4TunnelNextHopAppDbEntry1}),
              ArrayEq(std::vector<StatusCode>{StatusCode::SWSS_RC_SUCCESS}));

  EXPECT_NE(GetNextHopEntry(KeyGenerator::generateNextHopKey(
                kP4TunnelNextHopAppDbEntry1.next_hop_id)),
            nullptr);

  EXPECT_TRUE(
      ValidateNextHopEntryAdd(kP4TunnelNextHopAppDbEntry1, kTunnelNextHopOid));
  const std::string tunnel_key = KeyGenerator::generateTunnelKey(
      kP4TunnelNextHopAppDbEntry1.gre_tunnel_id);
  const std::string neighbor_key = KeyGenerator::generateNeighborKey(
      kP4TunnelEntry1.router_interface_id, kP4TunnelEntry1.neighbor_id);
  EXPECT_TRUE(ValidateRefCnt(SAI_OBJECT_TYPE_TUNNEL, tunnel_key, 1));
  EXPECT_TRUE(ValidateRefCnt(SAI_OBJECT_TYPE_NEIGHBOR_ENTRY, neighbor_key, 1));
}

TEST_F(NextHopManagerTest, RemoveRequestShouldSucceedForExistingNextHop) {
  auto* p4_next_hop_entry = AddNextHopEntry1();
  ASSERT_NE(p4_next_hop_entry, nullptr);

  std::vector<sai_status_t> exp_status{SAI_STATUS_SUCCESS};
  EXPECT_CALL(
      mock_sai_next_hop_,
      remove_next_hops(_, Pointee(Eq(p4_next_hop_entry->next_hop_oid)), _, _))
      .WillOnce(DoAll(SetArrayArgument<3>(exp_status.begin(), exp_status.end()),
                      Return(SAI_STATUS_SUCCESS)));

  EXPECT_THAT(
      RemoveNextHops(std::vector<P4NextHopAppDbEntry>{kP4NextHopAppDbEntry1}),
      ArrayEq(std::vector<StatusCode>{StatusCode::SWSS_RC_SUCCESS}));

  // Validate the next hop entry has been deleted in both P4 next hop manager
  // and centralized mapper.
  p4_next_hop_entry = GetNextHopEntry(
      KeyGenerator::generateNextHopKey(kP4NextHopAppDbEntry1.next_hop_id));
  EXPECT_EQ(p4_next_hop_entry, nullptr);
  EXPECT_FALSE(p4_oid_mapper_.existsOID(
      SAI_OBJECT_TYPE_NEXT_HOP,
      KeyGenerator::generateNextHopKey(kP4NextHopAppDbEntry1.next_hop_id)));

  // Validate ref count decrement.
  const std::string rif_key = KeyGenerator::generateRouterInterfaceKey(
      kP4NextHopAppDbEntry1.router_interface_id);
  const std::string neighbor_key = KeyGenerator::generateNeighborKey(
      kP4NextHopAppDbEntry1.router_interface_id,
      kP4NextHopAppDbEntry1.neighbor_id);
  EXPECT_TRUE(ValidateRefCnt(SAI_OBJECT_TYPE_ROUTER_INTERFACE, rif_key, 0));
  EXPECT_TRUE(ValidateRefCnt(SAI_OBJECT_TYPE_NEIGHBOR_ENTRY, neighbor_key, 0));
}

TEST_F(NextHopManagerTest, ValidateRemoveShouldFailForNonExistingNextHop) {
  EXPECT_FALSE(
      ValidateNextHopAppDbEntry(kP4TunnelNextHopAppDbEntry1, DEL_COMMAND).ok());
}

TEST_F(NextHopManagerTest,
       ValidateRemoveRequestShouldFailIfNextHopEntryIsAbsentInCentralMapper) {
  auto* p4_next_hop_entry = AddNextHopEntry1();
  ASSERT_NE(p4_next_hop_entry, nullptr);

  ASSERT_TRUE(p4_oid_mapper_.eraseOID(SAI_OBJECT_TYPE_NEXT_HOP,
                                      p4_next_hop_entry->next_hop_key));

  EXPECT_EQ(
      StatusCode::SWSS_RC_NOT_FOUND,
      ValidateNextHopAppDbEntry(kP4TunnelNextHopAppDbEntry1, DEL_COMMAND));
}

TEST_F(NextHopManagerTest,
       ValidateRemoveRequestShouldFailIfNextHopEntryIsStillReferenced) {
  auto* p4_next_hop_entry = AddNextHopEntry1();
  ASSERT_NE(p4_next_hop_entry, nullptr);

  ASSERT_TRUE(p4_oid_mapper_.increaseRefCount(SAI_OBJECT_TYPE_NEXT_HOP,
                                              p4_next_hop_entry->next_hop_key));

  EXPECT_EQ(StatusCode::SWSS_RC_INVALID_PARAM,
            ValidateNextHopAppDbEntry(kP4NextHopAppDbEntry1, DEL_COMMAND));
}

TEST_F(NextHopManagerTest, RemoveRequestShouldFailIfSaiCallFails) {
  auto* p4_next_hop_entry = AddNextHopEntry1();
  ASSERT_NE(p4_next_hop_entry, nullptr);

  std::vector<sai_status_t> exp_status{SAI_STATUS_FAILURE};
  EXPECT_CALL(
      mock_sai_next_hop_,
      remove_next_hops(_, Pointee(Eq(p4_next_hop_entry->next_hop_oid)), _, _))
      .WillOnce(DoAll(SetArrayArgument<3>(exp_status.begin(), exp_status.end()),
                      Return(SAI_STATUS_FAILURE)));

  EXPECT_THAT(
      RemoveNextHops(std::vector<P4NextHopAppDbEntry>{kP4NextHopAppDbEntry1}),
      ArrayEq(std::vector<StatusCode>{StatusCode::SWSS_RC_UNKNOWN}));

  // Validate the next hop entry is not deleted in either P4 next hop manager or
  // central mapper.
  p4_next_hop_entry = GetNextHopEntry(
      KeyGenerator::generateNextHopKey(kP4NextHopAppDbEntry1.next_hop_id));
  ASSERT_NE(p4_next_hop_entry, nullptr);
  EXPECT_TRUE(p4_oid_mapper_.existsOID(
      SAI_OBJECT_TYPE_NEXT_HOP,
      KeyGenerator::generateNextHopKey(kP4NextHopAppDbEntry1.next_hop_id)));

  // Validate ref count remains the same.
  const std::string rif_key = KeyGenerator::generateRouterInterfaceKey(
      kP4NextHopAppDbEntry1.router_interface_id);
  const std::string neighbor_key = KeyGenerator::generateNeighborKey(
      kP4NextHopAppDbEntry1.router_interface_id,
      kP4NextHopAppDbEntry1.neighbor_id);
  EXPECT_TRUE(ValidateRefCnt(SAI_OBJECT_TYPE_ROUTER_INTERFACE, rif_key, 1));
  EXPECT_TRUE(ValidateRefCnt(SAI_OBJECT_TYPE_NEIGHBOR_ENTRY, neighbor_key, 1));
}

TEST_F(NextHopManagerTest, GetNextHopEntryShouldReturnValidPointerForAddedNextHop)
{
    ASSERT_TRUE(ResolveNextHopEntryDependency(kP4NextHopAppDbEntry1, kRouterInterfaceOid1));

    std::vector<sai_status_t> exp_status{SAI_STATUS_SUCCESS};

    // Set up mock call.
    EXPECT_CALL(
        mock_sai_next_hop_,
        create_next_hops(
            Eq(gSwitchId), Eq(1), Pointee(Eq(3)),
            AttrArrayEq(sai_attrs_array_t{CreateAttributeListForNextHopObject(
                kP4NextHopAppDbEntry1, kRouterInterfaceOid1)}),
            Eq(SAI_BULK_OP_ERROR_MODE_STOP_ON_ERROR), ::testing::NotNull(),
            ::testing::NotNull()))
        .WillOnce(
            DoAll(SetArrayArgument<6>(exp_status.begin(), exp_status.end()),
                  SetArgPointee<5>(kNextHopOid), Return(SAI_STATUS_SUCCESS)));

    EXPECT_THAT(
        CreateNextHops(std::vector<P4NextHopAppDbEntry>{kP4NextHopAppDbEntry1}),
        ArrayEq(std::vector<StatusCode>{StatusCode::SWSS_RC_SUCCESS}));

    EXPECT_NE(GetNextHopEntry(KeyGenerator::generateNextHopKey(kP4NextHopAppDbEntry1.next_hop_id)), nullptr);
}

TEST_F(NextHopManagerTest, GetNextHopEntryShouldReturnNullPointerForNonexistingNextHop)
{
    EXPECT_EQ(GetNextHopEntry(KeyGenerator::generateNextHopKey(kP4NextHopAppDbEntry1.next_hop_id)), nullptr);
}

TEST_F(NextHopManagerTest, DeserializeP4NextHopAppDbEntryShouldSucceedForValidNextHopSetEntry)
{
    std::vector<swss::FieldValueTuple> attributes = {
        swss::FieldValueTuple(p4orch::kAction, "set_ip_nexthop"),
        swss::FieldValueTuple(prependParamField(p4orch::kRouterInterfaceId), kRouterInterfaceId1),
        swss::FieldValueTuple(prependParamField(p4orch::kNeighborId), kNeighborId1)};

    auto app_db_entry_or = DeserializeP4NextHopAppDbEntry(kNextHopP4AppDbKey, attributes);
    EXPECT_TRUE(app_db_entry_or.ok());
    auto &app_db_entry = *app_db_entry_or;
    EXPECT_EQ(app_db_entry.next_hop_id, kNextHopId);
    EXPECT_FALSE(app_db_entry.router_interface_id.empty());
    EXPECT_EQ(app_db_entry.router_interface_id, kRouterInterfaceId1);
    EXPECT_FALSE(app_db_entry.neighbor_id.isZero());
    EXPECT_EQ(app_db_entry.neighbor_id, swss::IpAddress(kNeighborId1));
}

TEST_F(NextHopManagerTest, DeserializeP4NextHopAppDbEntryShouldSucceedForValidNextHopDeleteEntry)
{
    auto app_db_entry_or = DeserializeP4NextHopAppDbEntry(kNextHopP4AppDbKey, std::vector<swss::FieldValueTuple>());
    EXPECT_TRUE(app_db_entry_or.ok());
    auto &app_db_entry = *app_db_entry_or;
    EXPECT_EQ(app_db_entry.next_hop_id, kNextHopId);
    EXPECT_TRUE(app_db_entry.router_interface_id.empty());
    EXPECT_TRUE(app_db_entry.neighbor_id.isZero());
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
        swss::FieldValueTuple(p4orch::kAction, "set_ip_nexthop"),
        swss::FieldValueTuple(prependParamField(p4orch::kRouterInterfaceId), kRouterInterfaceId1),
        swss::FieldValueTuple(prependParamField(p4orch::kNeighborId), "0.0.0.0.0.0")}; // Invalid IP address.

    EXPECT_FALSE(DeserializeP4NextHopAppDbEntry(kNextHopP4AppDbKey, attributes).ok());
}

TEST_F(NextHopManagerTest, DeserializeP4NextHopAppDbEntryShouldReturnNullPointerDueToUnexpectedField)
{
    std::vector<swss::FieldValueTuple> attributes = {
        swss::FieldValueTuple(p4orch::kAction, "set_ip_nexthop"),
        swss::FieldValueTuple(p4orch::kRouterInterfaceId, kRouterInterfaceId1),
        swss::FieldValueTuple("unexpected_field", "unexpected_value")};

    EXPECT_FALSE(DeserializeP4NextHopAppDbEntry(kNextHopP4AppDbKey, attributes).ok());
}

TEST_F(NextHopManagerTest, DrainValidAppEntryShouldSucceed)
{
    nlohmann::json j;
    j[prependMatchField(p4orch::kNexthopId)] = kNextHopId;

    std::vector<swss::FieldValueTuple> fvs{{p4orch::kAction, p4orch::kSetIpNexthop},
                                           {prependParamField(p4orch::kNeighborId), kNeighborId2},
                                           {prependParamField(p4orch::kRouterInterfaceId), kRouterInterfaceId2}};

    swss::KeyOpFieldsValuesTuple app_db_entry(std::string(APP_P4RT_NEXTHOP_TABLE_NAME) + kTableKeyDelimiter + j.dump(),
                                              SET_COMMAND, fvs);

    Enqueue(app_db_entry);

    EXPECT_TRUE(ResolveNextHopEntryDependency(kP4NextHopAppDbEntry2, kRouterInterfaceOid2));

    std::vector<sai_status_t> exp_status{SAI_STATUS_SUCCESS};

    // Set up mock call.
    EXPECT_CALL(mock_sai_next_hop_, create_next_hops(_, _, _, _, _, _, _))
        .WillOnce(
            DoAll(SetArrayArgument<6>(exp_status.begin(), exp_status.end()),
                  SetArgPointee<5>(kNextHopOid), Return(SAI_STATUS_SUCCESS)));

    EXPECT_CALL(publisher_,
                publish(Eq(APP_P4RT_TABLE_NAME), Eq(kfvKey(app_db_entry)),
                        Eq(kfvFieldsValues(app_db_entry)),
                        Eq(StatusCode::SWSS_RC_SUCCESS), Eq(true)));
    EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS, Drain(/*failure_before=*/false));

    EXPECT_TRUE(ValidateNextHopEntryAdd(kP4NextHopAppDbEntry2, kNextHopOid));
}

TEST_F(NextHopManagerTest, DrainValidTunnelNexthopAppEntryShouldSucceed)
{
    nlohmann::json tunnel_j;
    tunnel_j[prependMatchField(p4orch::kNexthopId)] = kTunnelNextHopId;
    std::vector<swss::FieldValueTuple> tunnel_fvs = {{p4orch::kAction, p4orch::kSetTunnelNexthop},
                                                     {prependParamField(p4orch::kNeighborId), kNeighborId2},
                                                     {prependParamField(p4orch::kTunnelId), kTunnelId2}};

    swss::KeyOpFieldsValuesTuple tunnel_app_db_entry(
        std::string(APP_P4RT_NEXTHOP_TABLE_NAME) + kTableKeyDelimiter + tunnel_j.dump(), SET_COMMAND, tunnel_fvs);

    Enqueue(tunnel_app_db_entry);

    EXPECT_TRUE(ResolveNextHopEntryDependency(kP4TunnelNextHopAppDbEntry2, kTunnelOid2));

    std::vector<sai_status_t> exp_status{SAI_STATUS_SUCCESS};
    // Set up mock call.
    EXPECT_CALL(mock_sai_next_hop_, create_next_hops(_, _, _, _, _, _, _))
        .WillOnce(DoAll(
            SetArrayArgument<6>(exp_status.begin(), exp_status.end()),
            SetArgPointee<5>(kTunnelNextHopOid), Return(SAI_STATUS_SUCCESS)));

    EXPECT_CALL(publisher_, publish(Eq(APP_P4RT_TABLE_NAME),
                                    Eq(kfvKey(tunnel_app_db_entry)),
                                    Eq(kfvFieldsValues(tunnel_app_db_entry)),
                                    Eq(StatusCode::SWSS_RC_SUCCESS), Eq(true)));
    EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS, Drain(/*failure_before=*/false));

    EXPECT_TRUE(ValidateNextHopEntryAdd(kP4TunnelNextHopAppDbEntry2, kTunnelNextHopOid));

    nlohmann::json j;
    j[prependMatchField(p4orch::kNexthopId)] = kTunnelNextHopId;
    std::vector<swss::FieldValueTuple> fvs;
    swss::KeyOpFieldsValuesTuple app_db_entry(std::string(APP_P4RT_NEXTHOP_TABLE_NAME) + kTableKeyDelimiter + j.dump(),
                                              DEL_COMMAND, fvs);

    EXPECT_CALL(mock_sai_next_hop_,
                remove_next_hops(_, Pointee(Eq(kTunnelNextHopOid)), _, _))
        .WillOnce(
            DoAll(SetArrayArgument<3>(exp_status.begin(), exp_status.end()),
                  Return(SAI_STATUS_SUCCESS)));

    Enqueue(app_db_entry);
    EXPECT_CALL(publisher_,
                publish(Eq(APP_P4RT_TABLE_NAME), Eq(kfvKey(app_db_entry)),
                        Eq(kfvFieldsValues(app_db_entry)),
                        Eq(StatusCode::SWSS_RC_SUCCESS), Eq(true)));
    EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS, Drain(/*failure_before=*/false));

    // Validate the next hop entry has been deleted in both P4 next hop manager
    // and centralized mapper.
    auto p4_next_hop_entry = GetNextHopEntry(KeyGenerator::generateNextHopKey(kP4TunnelNextHopAppDbEntry2.next_hop_id));
    EXPECT_EQ(p4_next_hop_entry, nullptr);
    EXPECT_FALSE(p4_oid_mapper_.existsOID(SAI_OBJECT_TYPE_NEXT_HOP,
                                          KeyGenerator::generateNextHopKey(kP4TunnelNextHopAppDbEntry2.next_hop_id)));

    // Validate ref count decrement.
    const std::string tunnel_key = KeyGenerator::generateTunnelKey(kP4TunnelNextHopAppDbEntry2.gre_tunnel_id);
    const std::string neighbor_key =
        KeyGenerator::generateNeighborKey(kP4TunnelEntry2.router_interface_id, kP4TunnelEntry2.neighbor_id);
    EXPECT_TRUE(ValidateRefCnt(SAI_OBJECT_TYPE_TUNNEL, tunnel_key, 0));
    EXPECT_TRUE(ValidateRefCnt(SAI_OBJECT_TYPE_NEIGHBOR_ENTRY, neighbor_key, 0));
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
    EXPECT_CALL(publisher_,
                publish(Eq(APP_P4RT_TABLE_NAME), Eq(kfvKey(app_db_entry)),
                        Eq(kfvFieldsValues(app_db_entry)),
                        Eq(StatusCode::SWSS_RC_INVALID_PARAM), Eq(true)));
    EXPECT_EQ(StatusCode::SWSS_RC_INVALID_PARAM,
              Drain(/*failure_before=*/false));

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
                                              SET_COMMAND, fvs);

    Enqueue(app_db_entry);
    EXPECT_CALL(publisher_,
                publish(Eq(APP_P4RT_TABLE_NAME), Eq(kfvKey(app_db_entry)),
                        Eq(kfvFieldsValues(app_db_entry)),
                        Eq(StatusCode::SWSS_RC_INVALID_PARAM), Eq(true)));
    EXPECT_EQ(StatusCode::SWSS_RC_INVALID_PARAM,
              Drain(/*failure_before=*/false));

    EXPECT_FALSE(ValidateNextHopEntryAdd(kP4NextHopAppDbEntry2, kNextHopOid));

    // Missing action field
    fvs = {{prependParamField(p4orch::kNeighborId), kNeighborId2},
           {prependParamField(p4orch::kRouterInterfaceId), kRouterInterfaceId2}};
    app_db_entry = {std::string(APP_P4RT_NEXTHOP_TABLE_NAME) + kTableKeyDelimiter + j.dump(), SET_COMMAND, fvs};

    Enqueue(app_db_entry);
    EXPECT_CALL(publisher_,
                publish(Eq(APP_P4RT_TABLE_NAME), Eq(kfvKey(app_db_entry)),
                        Eq(kfvFieldsValues(app_db_entry)),
                        Eq(StatusCode::SWSS_RC_INVALID_PARAM), Eq(true)));
    EXPECT_FALSE(ValidateNextHopEntryAdd(kP4NextHopAppDbEntry2, kNextHopOid));

    EXPECT_EQ(StatusCode::SWSS_RC_INVALID_PARAM,
              Drain(/*failure_before=*/false));
    // Missing neighbor field
    fvs = {{p4orch::kAction, p4orch::kSetIpNexthop},
           {prependParamField(p4orch::kRouterInterfaceId), kRouterInterfaceId2}};
    app_db_entry = {std::string(APP_P4RT_NEXTHOP_TABLE_NAME) + kTableKeyDelimiter + j.dump(), SET_COMMAND, fvs};

    Enqueue(app_db_entry);
    EXPECT_CALL(publisher_,
                publish(Eq(APP_P4RT_TABLE_NAME), Eq(kfvKey(app_db_entry)),
                        Eq(kfvFieldsValues(app_db_entry)),
                        Eq(StatusCode::SWSS_RC_INVALID_PARAM), Eq(true)));
    EXPECT_EQ(StatusCode::SWSS_RC_INVALID_PARAM,
              Drain(/*failure_before=*/false));
    EXPECT_FALSE(ValidateNextHopEntryAdd(kP4NextHopAppDbEntry2, kNextHopOid));

    // set_ip_nexthop + missing router_interface_id
    fvs = {{p4orch::kAction, p4orch::kSetIpNexthop}, {prependParamField(p4orch::kNeighborId), kNeighborId2}};
    app_db_entry = {std::string(APP_P4RT_NEXTHOP_TABLE_NAME) + kTableKeyDelimiter + j.dump(), SET_COMMAND, fvs};

    Enqueue(app_db_entry);
    EXPECT_CALL(publisher_,
                publish(Eq(APP_P4RT_TABLE_NAME), Eq(kfvKey(app_db_entry)),
                        Eq(kfvFieldsValues(app_db_entry)),
                        Eq(StatusCode::SWSS_RC_INVALID_PARAM), Eq(true)));
    EXPECT_EQ(StatusCode::SWSS_RC_INVALID_PARAM,
              Drain(/*failure_before=*/false));

    EXPECT_FALSE(ValidateNextHopEntryAdd(kP4NextHopAppDbEntry2, kNextHopOid));

    //  set_ip_nexthop + invalid param/tunnel_id
    fvs = {{p4orch::kAction, p4orch::kSetIpNexthop},
           {prependParamField(p4orch::kNeighborId), kNeighborId2},
           {prependParamField(p4orch::kTunnelId), kTunnelId1}};
    app_db_entry = {std::string(APP_P4RT_NEXTHOP_TABLE_NAME) + kTableKeyDelimiter + j.dump(), SET_COMMAND, fvs};

    Enqueue(app_db_entry);
    EXPECT_CALL(publisher_,
                publish(Eq(APP_P4RT_TABLE_NAME), Eq(kfvKey(app_db_entry)),
                        Eq(kfvFieldsValues(app_db_entry)),
                        Eq(StatusCode::SWSS_RC_INVALID_PARAM), Eq(true)));
    EXPECT_EQ(StatusCode::SWSS_RC_INVALID_PARAM,
              Drain(/*failure_before=*/false));

    EXPECT_FALSE(ValidateNextHopEntryAdd(kP4NextHopAppDbEntry2, kNextHopOid));

    // set_p2p_tunnel_encap_nexthop + invalid router_interface_id
    fvs = {{p4orch::kAction, p4orch::kSetTunnelNexthop},
           {prependParamField(p4orch::kNeighborId), kNeighborId2},
           {prependParamField(p4orch::kRouterInterfaceId), kRouterInterfaceId1}};
    app_db_entry = {std::string(APP_P4RT_NEXTHOP_TABLE_NAME) + kTableKeyDelimiter + j.dump(), SET_COMMAND, fvs};

    Enqueue(app_db_entry);
    EXPECT_CALL(publisher_,
                publish(Eq(APP_P4RT_TABLE_NAME), Eq(kfvKey(app_db_entry)),
                        Eq(kfvFieldsValues(app_db_entry)),
                        Eq(StatusCode::SWSS_RC_INVALID_PARAM), Eq(true)));
    EXPECT_EQ(StatusCode::SWSS_RC_INVALID_PARAM,
              Drain(/*failure_before=*/false));

    EXPECT_FALSE(ValidateNextHopEntryAdd(kP4TunnelNextHopAppDbEntry2, kNextHopOid));

    // set_p2p_tunnel_encap_nexthop + missing tunnel_id
    fvs = {{p4orch::kAction, p4orch::kSetTunnelNexthop}, {prependParamField(p4orch::kNeighborId), kNeighborId2}};
    app_db_entry = {std::string(APP_P4RT_NEXTHOP_TABLE_NAME) + kTableKeyDelimiter + j.dump(), SET_COMMAND, fvs};

    Enqueue(app_db_entry);
    EXPECT_CALL(publisher_,
                publish(Eq(APP_P4RT_TABLE_NAME), Eq(kfvKey(app_db_entry)),
                        Eq(kfvFieldsValues(app_db_entry)),
                        Eq(StatusCode::SWSS_RC_INVALID_PARAM), Eq(true)));
    EXPECT_EQ(StatusCode::SWSS_RC_INVALID_PARAM,
              Drain(/*failure_before=*/false));
    EXPECT_FALSE(ValidateNextHopEntryAdd(kP4TunnelNextHopAppDbEntry2, kNextHopOid));
}

TEST_F(NextHopManagerTest, DrainUpdateRequestShouldBeUnsupported)
{
    auto *p4_next_hop_entry = AddNextHopEntry1();
    ASSERT_NE(p4_next_hop_entry, nullptr);

    nlohmann::json j;
    j[prependMatchField(p4orch::kNexthopId)] = kNextHopId;
    std::vector<swss::FieldValueTuple> fvs{
        {p4orch::kAction, p4orch::kSetIpNexthop},
        {prependParamField(p4orch::kNeighborId), kNeighborId2},
        {prependParamField(p4orch::kRouterInterfaceId), kRouterInterfaceId2}};
    swss::KeyOpFieldsValuesTuple app_db_entry(std::string(APP_P4RT_NEXTHOP_TABLE_NAME) + kTableKeyDelimiter + j.dump(),
                                              SET_COMMAND, fvs);

    Enqueue(app_db_entry);
    EXPECT_TRUE(ResolveNextHopEntryDependency(kP4NextHopAppDbEntry2, kRouterInterfaceOid2));
    EXPECT_CALL(publisher_,
                publish(Eq(APP_P4RT_TABLE_NAME), Eq(kfvKey(app_db_entry)),
                        Eq(kfvFieldsValues(app_db_entry)),
                        Eq(StatusCode::SWSS_RC_UNIMPLEMENTED), Eq(true)));
    EXPECT_EQ(StatusCode::SWSS_RC_UNIMPLEMENTED,
              Drain(/*failure_before=*/false));

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

    std::vector<sai_status_t> exp_status{SAI_STATUS_SUCCESS};
    EXPECT_CALL(
        mock_sai_next_hop_,
        remove_next_hops(_, Pointee(Eq(p4_next_hop_entry->next_hop_oid)), _, _))
        .WillOnce(
            DoAll(SetArrayArgument<3>(exp_status.begin(), exp_status.end()),
                  Return(SAI_STATUS_SUCCESS)));

    Enqueue(app_db_entry);
    EXPECT_CALL(publisher_,
                publish(Eq(APP_P4RT_TABLE_NAME), Eq(kfvKey(app_db_entry)),
                        Eq(kfvFieldsValues(app_db_entry)),
                        Eq(StatusCode::SWSS_RC_SUCCESS), Eq(true)));
    EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS, Drain(/*failure_before=*/false));

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

TEST_F(NextHopManagerTest, VerifyIpNextHopStateTest)
{
    auto *p4_next_hop_entry = AddNextHopEntry1();
    ASSERT_NE(p4_next_hop_entry, nullptr);

    // Setup ASIC DB.
    swss::Table table(nullptr, "ASIC_STATE");
    table.set(
        "SAI_OBJECT_TYPE_NEXT_HOP:oid:0x65",
        std::vector<swss::FieldValueTuple>{swss::FieldValueTuple{"SAI_NEXT_HOP_ATTR_TYPE", "SAI_NEXT_HOP_TYPE_IP"},
                                           swss::FieldValueTuple{"SAI_NEXT_HOP_ATTR_IP", "10.0.0.1"},
                                           swss::FieldValueTuple{"SAI_NEXT_HOP_ATTR_ROUTER_INTERFACE_ID", "oid:0x1"}});

    nlohmann::json j;
    j[prependMatchField(p4orch::kNexthopId)] = kNextHopId;
    const std::string db_key = std::string(APP_P4RT_TABLE_NAME) + kTableKeyDelimiter + APP_P4RT_NEXTHOP_TABLE_NAME +
                               kTableKeyDelimiter + j.dump();
    std::vector<swss::FieldValueTuple> attributes;

    // Verification should succeed with vaild key and value.
    attributes.push_back(swss::FieldValueTuple{prependParamField(p4orch::kNeighborId), kNeighborId1});
    attributes.push_back(swss::FieldValueTuple{prependParamField(p4orch::kRouterInterfaceId), kRouterInterfaceId1});
    EXPECT_EQ(VerifyState(db_key, attributes), "");

    // Invalid key should fail verification.
    EXPECT_FALSE(VerifyState("invalid", attributes).empty());
    EXPECT_FALSE(VerifyState("invalid:invalid", attributes).empty());
    EXPECT_FALSE(VerifyState(std::string(APP_P4RT_TABLE_NAME) + ":invalid", attributes).empty());
    EXPECT_FALSE(VerifyState(std::string(APP_P4RT_TABLE_NAME) + ":invalid:invalid", attributes).empty());
    EXPECT_FALSE(VerifyState(std::string(APP_P4RT_TABLE_NAME) + ":FIXED_NEXTHOP_TABLE:invalid", attributes).empty());

    // Verification should fail with non-existing nexthop.
    j[prependMatchField(p4orch::kNexthopId)] = "invalid";
    EXPECT_FALSE(VerifyState(std::string(APP_P4RT_TABLE_NAME) + kTableKeyDelimiter + APP_P4RT_NEXTHOP_TABLE_NAME +
                                 kTableKeyDelimiter + j.dump(),
                             attributes)
                     .empty());

    // Verification should fail if nexthop key mismatches.
    auto saved_next_hop_key = p4_next_hop_entry->next_hop_key;
    p4_next_hop_entry->next_hop_key = "invalid";
    EXPECT_FALSE(VerifyState(db_key, attributes).empty());
    p4_next_hop_entry->next_hop_key = saved_next_hop_key;

    // Verification should fail if nexthop ID mismatches.
    auto saved_next_hop_id = p4_next_hop_entry->next_hop_id;
    p4_next_hop_entry->next_hop_id = "invalid";
    EXPECT_FALSE(VerifyState(db_key, attributes).empty());
    p4_next_hop_entry->next_hop_id = saved_next_hop_id;

    // Verification should fail if ritf ID mismatches.
    auto saved_router_interface_id = p4_next_hop_entry->router_interface_id;
    p4_next_hop_entry->router_interface_id = kRouterInterfaceId2;
    EXPECT_FALSE(VerifyState(db_key, attributes).empty());
    p4_next_hop_entry->router_interface_id = saved_router_interface_id;

    // Verification should fail if neighbor ID mismatches.
    auto saved_neighbor_id = p4_next_hop_entry->neighbor_id;
    p4_next_hop_entry->neighbor_id = swss::IpAddress(kNeighborId2);
    EXPECT_FALSE(VerifyState(db_key, attributes).empty());
    p4_next_hop_entry->neighbor_id = saved_neighbor_id;

    // Verification should fail if tunnel ID mismatches.
    auto saved_gre_tunnel_id = p4_next_hop_entry->gre_tunnel_id;
    p4_next_hop_entry->gre_tunnel_id = "invalid";
    EXPECT_FALSE(VerifyState(db_key, attributes).empty());
    p4_next_hop_entry->gre_tunnel_id = saved_gre_tunnel_id;
}

TEST_F(NextHopManagerTest, DrainNotExecuted) {
  std::vector<swss::FieldValueTuple> fvs{
      {p4orch::kAction, p4orch::kSetIpNexthop},
      {prependParamField(p4orch::kNeighborId), kNeighborId2},
      {prependParamField(p4orch::kRouterInterfaceId), kRouterInterfaceId2}};
  EXPECT_TRUE(ResolveNextHopEntryDependency(kP4NextHopAppDbEntry2,
                                            kRouterInterfaceOid2));
  nlohmann::json j;
  j[prependMatchField(p4orch::kNexthopId)] = "1";
  swss::KeyOpFieldsValuesTuple app_db_entry_1(
      std::string(APP_P4RT_NEXTHOP_TABLE_NAME) + kTableKeyDelimiter + j.dump(),
      SET_COMMAND, fvs);
  j[prependMatchField(p4orch::kNexthopId)] = "2";
  swss::KeyOpFieldsValuesTuple app_db_entry_2(
      std::string(APP_P4RT_NEXTHOP_TABLE_NAME) + kTableKeyDelimiter + j.dump(),
      SET_COMMAND, fvs);
  j[prependMatchField(p4orch::kNexthopId)] = "3";
  swss::KeyOpFieldsValuesTuple app_db_entry_3(
      std::string(APP_P4RT_NEXTHOP_TABLE_NAME) + kTableKeyDelimiter + j.dump(),
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
  EXPECT_EQ(nullptr, GetNextHopEntry(KeyGenerator::generateNextHopKey("1")));
  EXPECT_EQ(nullptr, GetNextHopEntry(KeyGenerator::generateNextHopKey("2")));
  EXPECT_EQ(nullptr, GetNextHopEntry(KeyGenerator::generateNextHopKey("3")));
}

TEST_F(NextHopManagerTest, DrainStopOnFirstFailureCreate) {
  std::vector<swss::FieldValueTuple> fvs{
      {p4orch::kAction, p4orch::kSetIpNexthop},
      {prependParamField(p4orch::kNeighborId), kNeighborId2},
      {prependParamField(p4orch::kRouterInterfaceId), kRouterInterfaceId2}};
  EXPECT_TRUE(ResolveNextHopEntryDependency(kP4NextHopAppDbEntry2,
                                            kRouterInterfaceOid2));
  nlohmann::json j;
  j[prependMatchField(p4orch::kNexthopId)] = "1";
  swss::KeyOpFieldsValuesTuple app_db_entry_1(
      std::string(APP_P4RT_NEXTHOP_TABLE_NAME) + kTableKeyDelimiter + j.dump(),
      SET_COMMAND, fvs);
  j[prependMatchField(p4orch::kNexthopId)] = "2";
  swss::KeyOpFieldsValuesTuple app_db_entry_2(
      std::string(APP_P4RT_NEXTHOP_TABLE_NAME) + kTableKeyDelimiter + j.dump(),
      SET_COMMAND, fvs);
  j[prependMatchField(p4orch::kNexthopId)] = "3";
  swss::KeyOpFieldsValuesTuple app_db_entry_3(
      std::string(APP_P4RT_NEXTHOP_TABLE_NAME) + kTableKeyDelimiter + j.dump(),
      SET_COMMAND, fvs);

  Enqueue(app_db_entry_1);
  Enqueue(app_db_entry_2);
  Enqueue(app_db_entry_3);

  std::vector<sai_status_t> exp_status{SAI_STATUS_SUCCESS, SAI_STATUS_FAILURE,
                                       SAI_STATUS_NOT_EXECUTED};

  EXPECT_CALL(mock_sai_next_hop_, create_next_hops(_, _, _, _, _, _, _))
      .WillOnce(DoAll(SetArrayArgument<6>(exp_status.begin(), exp_status.end()),
                      SetArgPointee<5>(kNextHopOid),
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
  EXPECT_NE(nullptr, GetNextHopEntry(KeyGenerator::generateNextHopKey("1")));
  EXPECT_EQ(nullptr, GetNextHopEntry(KeyGenerator::generateNextHopKey("2")));
  EXPECT_EQ(nullptr, GetNextHopEntry(KeyGenerator::generateNextHopKey("3")));
}

TEST_F(NextHopManagerTest, DrainStopOnFirstFailureDel) {
  auto* p4_next_hop_entry = AddNextHopEntry1();
  ASSERT_NE(p4_next_hop_entry, nullptr);

  std::vector<swss::FieldValueTuple> fvs;

  nlohmann::json j;
  j[prependMatchField(p4orch::kNexthopId)] = kNextHopId;
  swss::KeyOpFieldsValuesTuple del_app_db_entry_1(
      std::string(APP_P4RT_NEXTHOP_TABLE_NAME) + APP_P4RT_TABLE_NAME_SEPARATOR +
          j.dump(),
      DEL_COMMAND, fvs);
  j[prependMatchField(p4orch::kNexthopId)] = kNextHopId;
  swss::KeyOpFieldsValuesTuple del_app_db_entry_2(
      std::string(APP_P4RT_NEXTHOP_TABLE_NAME) + APP_P4RT_TABLE_NAME_SEPARATOR +
          j.dump(),
      DEL_COMMAND, fvs);
  j[prependMatchField(p4orch::kNexthopId)] = kNextHopId;
  swss::KeyOpFieldsValuesTuple del_app_db_entry_3(
      std::string(APP_P4RT_NEXTHOP_TABLE_NAME) + APP_P4RT_TABLE_NAME_SEPARATOR +
          j.dump(),
      DEL_COMMAND, fvs);

  Enqueue(del_app_db_entry_1);
  Enqueue(del_app_db_entry_2);
  Enqueue(del_app_db_entry_3);

  std::vector<sai_status_t> exp_status{SAI_STATUS_SUCCESS, SAI_STATUS_FAILURE,
                                       SAI_STATUS_NOT_EXECUTED};
  EXPECT_CALL(mock_sai_next_hop_, remove_next_hops(_, _, _, _))
      .WillOnce(DoAll(SetArrayArgument<3>(exp_status.begin(), exp_status.end()),
                      Return(SAI_STATUS_FAILURE)));

  EXPECT_CALL(publisher_,
              publish(Eq(APP_P4RT_TABLE_NAME), Eq(kfvKey(del_app_db_entry_1)),
                      Eq(kfvFieldsValues(del_app_db_entry_1)),
                      Eq(StatusCode::SWSS_RC_SUCCESS), Eq(true)));
  EXPECT_CALL(publisher_,
              publish(Eq(APP_P4RT_TABLE_NAME), Eq(kfvKey(del_app_db_entry_2)),
                      Eq(kfvFieldsValues(del_app_db_entry_2)),
                      Eq(StatusCode::SWSS_RC_UNKNOWN), Eq(true)));
  EXPECT_CALL(publisher_,
              publish(Eq(APP_P4RT_TABLE_NAME), Eq(kfvKey(del_app_db_entry_3)),
                      Eq(kfvFieldsValues(del_app_db_entry_3)),
                      Eq(StatusCode::SWSS_RC_NOT_EXECUTED), Eq(true)));
  EXPECT_EQ(StatusCode::SWSS_RC_UNKNOWN, Drain(/*failure_before=*/false));
  EXPECT_EQ(nullptr,
            GetNextHopEntry(KeyGenerator::generateNextHopKey(kNextHopId)));
}

TEST_F(NextHopManagerTest, DrainStopOnFirstFailureDifferentTypes) {
  auto* p4_next_hop_entry = AddNextHopEntry1();
  ASSERT_NE(p4_next_hop_entry, nullptr);

  std::vector<swss::FieldValueTuple> fvs{
      {p4orch::kAction, p4orch::kSetIpNexthop},
      {prependParamField(p4orch::kNeighborId), kNeighborId2},
      {prependParamField(p4orch::kRouterInterfaceId), kRouterInterfaceId2}};
  EXPECT_TRUE(ResolveNextHopEntryDependency(kP4NextHopAppDbEntry2,
                                            kRouterInterfaceOid2));
  nlohmann::json j;
  j[prependMatchField(p4orch::kNexthopId)] = "1";
  swss::KeyOpFieldsValuesTuple app_db_entry_1(
      std::string(APP_P4RT_NEXTHOP_TABLE_NAME) + APP_P4RT_TABLE_NAME_SEPARATOR +
          j.dump(),
      SET_COMMAND, fvs);
  j[prependMatchField(p4orch::kNexthopId)] = kNextHopId;
  swss::KeyOpFieldsValuesTuple app_db_entry_2(
      std::string(APP_P4RT_NEXTHOP_TABLE_NAME) + APP_P4RT_TABLE_NAME_SEPARATOR +
          j.dump(),
      SET_COMMAND, fvs);
  j[prependMatchField(p4orch::kNexthopId)] = kNextHopId;
  swss::KeyOpFieldsValuesTuple app_db_entry_3(
      std::string(APP_P4RT_NEXTHOP_TABLE_NAME) + APP_P4RT_TABLE_NAME_SEPARATOR +
          j.dump(),
      DEL_COMMAND, fvs);

  Enqueue(app_db_entry_1);
  Enqueue(app_db_entry_2);
  Enqueue(app_db_entry_3);

  std::vector<sai_status_t> exp_status{SAI_STATUS_SUCCESS};

  EXPECT_CALL(mock_sai_next_hop_, create_next_hops(_, _, _, _, _, _, _))
      .WillOnce(DoAll(SetArrayArgument<6>(exp_status.begin(), exp_status.end()),
                      SetArgPointee<5>(kNextHopOid),
                      Return(SAI_STATUS_SUCCESS)));

  EXPECT_CALL(publisher_,
              publish(Eq(APP_P4RT_TABLE_NAME), Eq(kfvKey(app_db_entry_1)),
                      Eq(kfvFieldsValues(app_db_entry_1)),
                      Eq(StatusCode::SWSS_RC_SUCCESS), Eq(true)));
  EXPECT_CALL(publisher_,
              publish(Eq(APP_P4RT_TABLE_NAME), Eq(kfvKey(app_db_entry_2)),
                      Eq(kfvFieldsValues(app_db_entry_2)),
                      Eq(StatusCode::SWSS_RC_UNIMPLEMENTED), Eq(true)));
  EXPECT_CALL(publisher_,
              publish(Eq(APP_P4RT_TABLE_NAME), Eq(kfvKey(app_db_entry_3)),
                      Eq(kfvFieldsValues(app_db_entry_3)),
                      Eq(StatusCode::SWSS_RC_NOT_EXECUTED), Eq(true)));
  EXPECT_EQ(StatusCode::SWSS_RC_UNIMPLEMENTED, Drain(/*failure_before=*/false));
  EXPECT_NE(nullptr, GetNextHopEntry(KeyGenerator::generateNextHopKey("1")));
  EXPECT_NE(nullptr,
            GetNextHopEntry(KeyGenerator::generateNextHopKey(kNextHopId)));
}

TEST_F(NextHopManagerTest, VerifyTunnelNextHopStateTest)
{
    ASSERT_TRUE(ResolveNextHopEntryDependency(kP4TunnelNextHopAppDbEntry1, kTunnelOid1));

    std::vector<sai_status_t> exp_status{SAI_STATUS_SUCCESS};

    // Set up mock call.
    EXPECT_CALL(
        mock_sai_next_hop_,
        create_next_hops(
            Eq(gSwitchId), Eq(1), Pointee(Eq(3)),
            AttrArrayEq(sai_attrs_array_t{CreateAttributeListForNextHopObject(
                kP4TunnelNextHopAppDbEntry1, kTunnelOid1,
                swss::IpAddress(kNeighborId1))}),
            Eq(SAI_BULK_OP_ERROR_MODE_STOP_ON_ERROR), ::testing::NotNull(),
            ::testing::NotNull()))
        .WillOnce(DoAll(
            SetArrayArgument<6>(exp_status.begin(), exp_status.end()),
            SetArgPointee<5>(kTunnelNextHopOid), Return(SAI_STATUS_SUCCESS)));

    EXPECT_THAT(CreateNextHops(std::vector<P4NextHopAppDbEntry>{
                    kP4TunnelNextHopAppDbEntry1}),
                ArrayEq(std::vector<StatusCode>{StatusCode::SWSS_RC_SUCCESS}));

    auto p4_next_hop_entry = GetNextHopEntry(KeyGenerator::generateNextHopKey(kP4TunnelNextHopAppDbEntry1.next_hop_id));
    ASSERT_NE(p4_next_hop_entry, nullptr);

    // Setup ASIC DB.
    swss::Table table(nullptr, "ASIC_STATE");
    table.set("SAI_OBJECT_TYPE_NEXT_HOP:oid:0x66",
              std::vector<swss::FieldValueTuple>{
                  swss::FieldValueTuple{"SAI_NEXT_HOP_ATTR_TYPE", "SAI_NEXT_HOP_TYPE_TUNNEL_ENCAP"},
                  swss::FieldValueTuple{"SAI_NEXT_HOP_ATTR_IP", "10.0.0.1"},
                  swss::FieldValueTuple{"SAI_NEXT_HOP_ATTR_TUNNEL_ID", "oid:0xb"}});

    nlohmann::json j;
    j[prependMatchField(p4orch::kNexthopId)] = kTunnelNextHopId;
    const std::string db_key = std::string(APP_P4RT_TABLE_NAME) + kTableKeyDelimiter + APP_P4RT_NEXTHOP_TABLE_NAME +
                               kTableKeyDelimiter + j.dump();
    std::vector<swss::FieldValueTuple> attributes;

    // Verification should succeed with vaild key and value.
    attributes.push_back(swss::FieldValueTuple{prependParamField(p4orch::kNeighborId), kNeighborId1});
    attributes.push_back(swss::FieldValueTuple{prependParamField(p4orch::kRouterInterfaceId), kRouterInterfaceId1});
    EXPECT_EQ(VerifyState(db_key, attributes), "");

    // Verification should fail if nexthop key mismatches.
    auto saved_next_hop_key = p4_next_hop_entry->next_hop_key;
    p4_next_hop_entry->next_hop_key = "invalid";
    EXPECT_FALSE(VerifyState(db_key, attributes).empty());
    p4_next_hop_entry->next_hop_key = saved_next_hop_key;

    // Verification should fail if nexthop ID mismatches.
    auto saved_next_hop_id = p4_next_hop_entry->next_hop_id;
    p4_next_hop_entry->next_hop_id = "invalid";
    EXPECT_FALSE(VerifyState(db_key, attributes).empty());
    p4_next_hop_entry->next_hop_id = saved_next_hop_id;

    // Verification should fail if ritf ID mismatches.
    auto saved_router_interface_id = p4_next_hop_entry->router_interface_id;
    p4_next_hop_entry->router_interface_id = kRouterInterfaceId2;
    EXPECT_FALSE(VerifyState(db_key, attributes).empty());
    p4_next_hop_entry->router_interface_id = saved_router_interface_id;

    // Verification should fail if neighbor ID mismatches.
    auto saved_neighbor_id = p4_next_hop_entry->neighbor_id;
    p4_next_hop_entry->neighbor_id = swss::IpAddress(kNeighborId2);
    EXPECT_FALSE(VerifyState(db_key, attributes).empty());
    p4_next_hop_entry->neighbor_id = saved_neighbor_id;

    // Verification should fail if tunnel ID mismatches.
    auto saved_gre_tunnel_id = p4_next_hop_entry->gre_tunnel_id;
    p4_next_hop_entry->gre_tunnel_id = "invalid";
    EXPECT_FALSE(VerifyState(db_key, attributes).empty());
    p4_next_hop_entry->gre_tunnel_id = saved_gre_tunnel_id;
}

TEST_F(NextHopManagerTest, VerifyStateAsicDbTest)
{
    auto *p4_next_hop_entry = AddNextHopEntry1();
    ASSERT_NE(p4_next_hop_entry, nullptr);

    // Setup ASIC DB.
    swss::Table table(nullptr, "ASIC_STATE");
    table.set(
        "SAI_OBJECT_TYPE_NEXT_HOP:oid:0x65",
        std::vector<swss::FieldValueTuple>{swss::FieldValueTuple{"SAI_NEXT_HOP_ATTR_TYPE", "SAI_NEXT_HOP_TYPE_IP"},
                                           swss::FieldValueTuple{"SAI_NEXT_HOP_ATTR_IP", "10.0.0.1"},
                                           swss::FieldValueTuple{"SAI_NEXT_HOP_ATTR_ROUTER_INTERFACE_ID", "oid:0x1"}});

    nlohmann::json j;
    j[prependMatchField(p4orch::kNexthopId)] = kNextHopId;
    const std::string db_key = std::string(APP_P4RT_TABLE_NAME) + kTableKeyDelimiter + APP_P4RT_NEXTHOP_TABLE_NAME +
                               kTableKeyDelimiter + j.dump();
    std::vector<swss::FieldValueTuple> attributes;
    attributes.push_back(swss::FieldValueTuple{prependParamField(p4orch::kNeighborId), kNeighborId1});
    attributes.push_back(swss::FieldValueTuple{prependParamField(p4orch::kRouterInterfaceId), kRouterInterfaceId1});

    // Verification should succeed with correct ASIC DB values.
    EXPECT_EQ(VerifyState(db_key, attributes), "");

    // Verification should fail if ASIC DB values mismatch.
    table.set("SAI_OBJECT_TYPE_NEXT_HOP:oid:0x65", std::vector<swss::FieldValueTuple>{swss::FieldValueTuple{
                                                       "SAI_NEXT_HOP_ATTR_IP", "fe80::21a:11ff:fe17:5f80"}});
    EXPECT_FALSE(VerifyState(db_key, attributes).empty());

    // Verification should fail if ASIC DB table is missing.
    table.del("SAI_OBJECT_TYPE_NEXT_HOP:oid:0x65");
    EXPECT_FALSE(VerifyState(db_key, attributes).empty());
    table.set(
        "SAI_OBJECT_TYPE_NEXT_HOP:oid:0x65",
        std::vector<swss::FieldValueTuple>{swss::FieldValueTuple{"SAI_NEXT_HOP_ATTR_TYPE", "SAI_NEXT_HOP_TYPE_IP"},
                                           swss::FieldValueTuple{"SAI_NEXT_HOP_ATTR_IP", "10.0.0.1"},
                                           swss::FieldValueTuple{"SAI_NEXT_HOP_ATTR_ROUTER_INTERFACE_ID", "oid:0x1"}});
}
