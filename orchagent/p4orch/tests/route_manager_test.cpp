#include "route_manager.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <functional>
#include <map>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#include "ipprefix.h"
#include "mock_response_publisher.h"
#include "mock_sai_ipmc.h"
#include "mock_sai_route.h"
#include "mock_sai_rpf_group.h"
#include "p4orch.h"
#include "p4orch/p4orch_util.h"
#include "return_code.h"
#include "swssnet.h"
#include "vrforch.h"

using ::p4orch::kTableKeyDelimiter;

using ::testing::_;
using ::testing::DoAll;
using ::testing::Eq;
using ::testing::Return;
using ::testing::SetArgPointee;
using ::testing::SetArrayArgument;
using ::testing::StrictMock;

extern sai_object_id_t gSwitchId;
extern sai_object_id_t gVirtualRouterId;
extern sai_object_id_t gVrfOid;
extern char *gVrfName;
extern size_t gMaxBulkSize;
extern sai_ipmc_api_t* sai_ipmc_api;
extern sai_route_api_t *sai_route_api;
extern sai_rpf_group_api_t* sai_rpf_group_api;
extern VRFOrch *gVrfOrch;

namespace
{

constexpr char *kIpv4Prefix = "10.11.12.0/24";
constexpr char *kIpv4Prefix2 = "10.12.12.0/24";
constexpr char* kIpv4Prefix4 = "20.30.40.50/32";
constexpr char *kIpv6Prefix = "2001:db8:1::/32";
constexpr char* kIpv6Prefix2 = "2001:db8:2::/32";
constexpr char* kIpv6Prefix3 = "2001:db8:3:4:5:6:7:8/128";
constexpr char *kNexthopId1 = "ju1u32m1.atl11:qe-3/7";
constexpr sai_object_id_t kNexthopOid1 = 1;
constexpr char *kNexthopId2 = "ju1u32m2.atl11:qe-3/7";
constexpr sai_object_id_t kNexthopOid2 = 2;
constexpr char *kWcmpGroup1 = "wcmp-group-1";
constexpr sai_object_id_t kWcmpGroupOid1 = 3;
constexpr char *kWcmpGroup2 = "wcmp-group-2";
constexpr sai_object_id_t kWcmpGroupOid2 = 4;
constexpr char* kMulticastGroupId1 = "0x1";
constexpr char* kMulticastGroupId2 = "0x2";
constexpr char* kMulticastGroupId3 = "0x3";
constexpr char* kMulticastGroupId4 = "0x4";
constexpr sai_object_id_t kMulticastGroupOid1 = 0x1;
constexpr sai_object_id_t kMulticastGroupOid2 = 0x2;
constexpr sai_object_id_t kMulticastGroupOid3 = 0x3;
constexpr sai_object_id_t kMulticastGroupOid4 = 0x4;
constexpr sai_object_id_t kRpfGroupOid1 = 0x77;
constexpr char *kMetadata1 = "1";
constexpr char *kMetadata2 = "2";
uint32_t kMetadataInt1 = 1;
uint32_t kMetadataInt2 = 2;

// Returns true if the two prefixes are equal. False otherwise.
// Arguments must be non-nullptr.
bool PrefixCmp(const sai_ip_prefix_t *x, const sai_ip_prefix_t *y)
{
    if (x->addr_family != y->addr_family)
    {
        return false;
    }
    if (x->addr_family == SAI_IP_ADDR_FAMILY_IPV4)
    {
        return memcmp(&x->addr.ip4, &y->addr.ip4, sizeof(sai_ip4_t)) == 0 &&
               memcmp(&x->mask.ip4, &y->mask.ip4, sizeof(sai_ip4_t)) == 0;
    }
    return memcmp(&x->addr.ip6, &y->addr.ip6, sizeof(sai_ip6_t)) == 0 &&
           memcmp(&x->mask.ip6, &y->mask.ip6, sizeof(sai_ip6_t)) == 0;
}

bool AddressCmp(const sai_ip_address_t* x, const sai_ip_address_t* y) {
  if (x->addr_family != y->addr_family) {
    return false;
  }
  if (x->addr_family == SAI_IP_ADDR_FAMILY_IPV4) {
    return memcmp(&x->addr.ip4, &y->addr.ip4, sizeof(sai_ip4_t)) == 0;
  }
  return memcmp(&x->addr.ip6, &y->addr.ip6, sizeof(sai_ip6_t)) == 0;
}

// Matches two SAI route entries.
bool MatchSaiRouteEntry(const sai_route_entry_t &route_entry, const sai_route_entry_t &exp_route_entry)
{
    if (route_entry.switch_id != exp_route_entry.switch_id)
    {
        return false;
    }
    if (route_entry.vr_id != exp_route_entry.vr_id)
    {
        return false;
    }
    if (!PrefixCmp(&route_entry.destination, &exp_route_entry.destination))
    {
        return false;
    }
    return true;
}

// Matches two SAI attributes.
bool MatchSaiAttribute(const sai_attribute_t &attr, const sai_attribute_t &exp_attr)
{
    if (exp_attr.id == SAI_ROUTE_ENTRY_ATTR_PACKET_ACTION)
    {
        if (attr.id != SAI_ROUTE_ENTRY_ATTR_PACKET_ACTION || attr.value.s32 != exp_attr.value.s32)
        {
            return false;
        }
    }
    if (exp_attr.id == SAI_ROUTE_ENTRY_ATTR_NEXT_HOP_ID)
    {
        if (attr.id != SAI_ROUTE_ENTRY_ATTR_NEXT_HOP_ID || attr.value.oid != exp_attr.value.oid)
        {
            return false;
        }
    }
    if (exp_attr.id == SAI_ROUTE_ENTRY_ATTR_META_DATA)
    {
        if (attr.id != SAI_ROUTE_ENTRY_ATTR_META_DATA || attr.value.u32 != exp_attr.value.u32)
        {
            return false;
        }
    }
    return true;
}

MATCHER_P(ArrayEq, array, "")
{
    for (size_t i = 0; i < array.size(); ++i)
    {
        if (arg[i] != array[i])
        {
            return false;
        }
    }
    return true;
}

MATCHER_P(RouteEntryArrayEq, array, "")
{
    for (size_t i = 0; i < array.size(); ++i)
    {
        if (!MatchSaiRouteEntry(arg[i], array[i]))
        {
            return false;
        }
    }
    return true;
}

MATCHER_P(AttrArrayEq, array, "")
{
    for (size_t i = 0; i < array.size(); ++i)
    {
        if (!MatchSaiAttribute(arg[i], array[i]))
        {
            return false;
        }
    }
    return true;
}

MATCHER_P(AttrArrayArrayEq, array, "")
{
    for (size_t i = 0; i < array.size(); ++i)
    {
        for (size_t j = 0; j < array[i].size(); j++)
        {
            if (!MatchSaiAttribute(arg[i][j], array[i][j]))
            {
                return false;
            }
        }
    }
    return true;
}

MATCHER_P(FieldValueTupleArrayEq, array, "")
{
    for (size_t i = 0; i < array.size(); ++i)
    {
        if (fvField(arg[i]) != fvField(array[i]))
        {
            return false;
        }
        if (fvValue(arg[i]) != fvValue(array[i]))
        {
            return false;
        }
    }
    return true;
}

} // namespace

class RouteManagerTest : public ::testing::Test
{
  protected:
   RouteManagerTest()
       : route_manager_(&p4_oid_mapper_, gVrfOrch, &publisher_) {}

   void SetUp() override {
     mock_sai_route = &mock_sai_route_;
     sai_route_api->create_route_entry = create_route_entry;
     sai_route_api->remove_route_entry = remove_route_entry;
     sai_route_api->set_route_entry_attribute = set_route_entry_attribute;
     sai_route_api->get_route_entry_attribute = get_route_entry_attribute;
     sai_route_api->create_route_entries = create_route_entries;
     sai_route_api->remove_route_entries = remove_route_entries;
     sai_route_api->set_route_entries_attribute = set_route_entries_attribute;
     sai_route_api->get_route_entries_attribute = get_route_entries_attribute;
     mock_sai_ipmc = &mock_sai_ipmc_;
     sai_ipmc_api->create_ipmc_entry = mock_create_ipmc_entry;
     sai_ipmc_api->remove_ipmc_entry = mock_remove_ipmc_entry;
     sai_ipmc_api->set_ipmc_entry_attribute = mock_set_ipmc_entry_attribute;
     sai_ipmc_api->get_ipmc_entry_attribute = mock_get_ipmc_entry_attribute;
     mock_sai_rpf_group = &mock_sai_rpf_group_;
     sai_rpf_group_api->create_rpf_group = mock_create_rpf_group;
   }

    bool MergeRouteEntry(const P4RouteEntry &dest, const P4RouteEntry &src, P4RouteEntry *ret)
    {
        return route_manager_.mergeRouteEntry(dest, src, ret);
    }

    ReturnCodeOr<P4RouteEntry> DeserializeRouteEntry(const std::string &key,
                                                     const std::vector<swss::FieldValueTuple> &attributes,
                                                     const std::string &table_name)
    {
        return route_manager_.deserializeRouteEntry(key, attributes, table_name);
    }

    P4RouteEntry *GetRouteEntry(const std::string &route_entry_key)
    {
        return route_manager_.getRouteEntry(route_entry_key);
    }

    ReturnCode ValidateRouteEntry(const P4RouteEntry &route_entry, const std::string &operation)
    {
        return route_manager_.validateRouteEntry(route_entry, operation);
    }

    std::vector<ReturnCode> CreateRouteEntries(const std::vector<P4RouteEntry> &route_entries)
    {
        return route_manager_.createRouteEntries(route_entries);
    }

    std::vector<ReturnCode> UpdateRouteEntries(const std::vector<P4RouteEntry> &route_entries)
    {
        return route_manager_.updateRouteEntries(route_entries);
    }

    std::vector<ReturnCode> DeleteRouteEntries(const std::vector<P4RouteEntry> &route_entries)
    {
        return route_manager_.deleteRouteEntries(route_entries);
    }

    std::vector<ReturnCode> CreateMulticastRouteEntries(
        const std::vector<P4RouteEntry>& route_entries) {
      return route_manager_.createMulticastRouteEntries(route_entries);
    }

    std::vector<ReturnCode> UpdateMulticastRouteEntries(
        const std::vector<P4RouteEntry>& route_entries) {
      return route_manager_.updateMulticastRouteEntries(route_entries);
    }

    std::vector<ReturnCode> DeleteMulticastRouteEntries(
        const std::vector<P4RouteEntry>& route_entries) {
      return route_manager_.deleteMulticastRouteEntries(route_entries);
    }

    void AddMulticastGroup(const std::string multicast_group_id,
                           const sai_object_id_t group_oid) {
      p4_oid_mapper_.setOID(SAI_OBJECT_TYPE_IPMC_GROUP, multicast_group_id,
                            group_oid);
    }

    void Enqueue(const std::string &table_name, const swss::KeyOpFieldsValuesTuple &entry)
    {
        route_manager_.enqueue(table_name, entry);
    }

    ReturnCode Drain(bool failure_before) {
      if (failure_before) {
        route_manager_.drainWithNotExecuted();
        return ReturnCode(StatusCode::SWSS_RC_NOT_EXECUTED);
      }
      return route_manager_.drain();
    }

    std::string VerifyState(const std::string &key, const std::vector<swss::FieldValueTuple> &tuple)
    {
        return route_manager_.verifyState(key, tuple);
    }

    // Generates a KeyOpFieldsValuesTuple.
    swss::KeyOpFieldsValuesTuple GenerateKeyOpFieldsValuesTuple(const std::string &vrf_id,
                                                                const swss::IpPrefix &route_prefix,
                                                                const std::string &command, const std::string &action,
                                                                const std::string &action_param,
                                                                const std::string &route_metadata = "")
    {
        nlohmann::json j;
        std::string key_prefix;
        j[prependMatchField(p4orch::kVrfId)] = vrf_id;
        if (route_prefix.isV4())
        {
            j[prependMatchField(p4orch::kIpv4Dst)] = route_prefix.to_string();
            key_prefix = std::string(APP_P4RT_IPV4_TABLE_NAME) + kTableKeyDelimiter;
        }
        else
        {
            j[prependMatchField(p4orch::kIpv6Dst)] = route_prefix.to_string();
            key_prefix = std::string(APP_P4RT_IPV6_TABLE_NAME) + kTableKeyDelimiter;
        }
        std::vector<swss::FieldValueTuple> attributes;
        if (command == SET_COMMAND)
        {
            attributes.push_back(swss::FieldValueTuple{p4orch::kAction, action});
            if (action == p4orch::kSetNexthopId ||
                action == p4orch::kSetNexthopIdAndMetadata) {
              attributes.push_back(swss::FieldValueTuple{
                  prependParamField(p4orch::kNexthopId), action_param});
            } else if (action == p4orch::kSetWcmpGroupId ||
                       action == p4orch::kSetWcmpGroupIdAndMetadata) {
              attributes.push_back(swss::FieldValueTuple{
                  prependParamField(p4orch::kWcmpGroupId), action_param});
            } else if (action == p4orch::kSetMulticastGroupId) {
              attributes.push_back(swss::FieldValueTuple{
                  prependParamField(p4orch::kMulticastGroupId), action_param});
            }
            if (action == p4orch::kSetNexthopIdAndMetadata ||
                action == p4orch::kSetWcmpGroupIdAndMetadata ||
                action == p4orch::kSetMetadataAndDrop ||
                action == p4orch::kSetMulticastGroupId) {
              attributes.push_back(swss::FieldValueTuple{
                  prependParamField(p4orch::kRouteMetadata), route_metadata});
            }
        }
        return swss::KeyOpFieldsValuesTuple(key_prefix + j.dump(), command, attributes);
    }

    // Generates a P4RouteEntry.
    P4RouteEntry GenerateP4RouteEntry(const std::string &vrf_id, const swss::IpPrefix &route_prefix,
                                      const std::string &action, const std::string &action_param,
                                      const std::string &route_metadata = "")
    {
        P4RouteEntry route_entry = {};
        route_entry.vrf_id = vrf_id;
        route_entry.route_prefix = route_prefix;
        route_entry.route_metadata = route_metadata;
        route_entry.action = action;
        if (action == p4orch::kSetNexthopId || action == p4orch::kSetNexthopIdAndMetadata)
        {
            route_entry.nexthop_id = action_param;
        }
        else if (action == p4orch::kSetWcmpGroupId || action == p4orch::kSetWcmpGroupIdAndMetadata)
        {
            route_entry.wcmp_group = action_param;
        } else if (action == p4orch::kSetMulticastGroupId) {
          route_entry.multicast_group_id = action_param;
        }
        route_entry.route_entry_key = KeyGenerator::generateRouteKey(route_entry.vrf_id, route_entry.route_prefix);
        return route_entry;
    }

    // Sets up a nexthop route entry for test.
    void SetupNexthopIdRouteEntry(const std::string &vrf_id, const swss::IpPrefix &route_prefix,
                                  const std::string &nexthop_id, sai_object_id_t nexthop_oid,
                                  const std::string &metadata = "")
    {
        auto route_entry = GenerateP4RouteEntry(
            vrf_id, route_prefix, (metadata.empty()) ? p4orch::kSetNexthopId : p4orch::kSetNexthopIdAndMetadata,
            nexthop_id, metadata);
        p4_oid_mapper_.setOID(SAI_OBJECT_TYPE_NEXT_HOP, KeyGenerator::generateNextHopKey(route_entry.nexthop_id),
                              nexthop_oid);

        std::vector<sai_status_t> exp_status{SAI_STATUS_SUCCESS};
        EXPECT_CALL(mock_sai_route_, create_route_entries(_, _, _, _, _, _))
            .WillOnce(DoAll(SetArrayArgument<5>(exp_status.begin(), exp_status.end()), Return(SAI_STATUS_SUCCESS)));
        EXPECT_THAT(CreateRouteEntries(std::vector<P4RouteEntry>{route_entry}),
                    ArrayEq(std::vector<StatusCode>{StatusCode::SWSS_RC_SUCCESS}));
    }

    // Sets up a wcmp route entry for test.
    void SetupWcmpGroupRouteEntry(const std::string &vrf_id, const swss::IpPrefix &route_prefix,
                                  const std::string &wcmp_group_id, sai_object_id_t wcmp_group_oid,
                                  const std::string &metadata = "")
    {
        auto route_entry = GenerateP4RouteEntry(
            vrf_id, route_prefix, (metadata.empty()) ? p4orch::kSetWcmpGroupId : p4orch::kSetWcmpGroupIdAndMetadata,
            wcmp_group_id, metadata);
        p4_oid_mapper_.setOID(SAI_OBJECT_TYPE_NEXT_HOP_GROUP,
                              KeyGenerator::generateWcmpGroupKey(route_entry.wcmp_group), wcmp_group_oid);

        std::vector<sai_status_t> exp_status{SAI_STATUS_SUCCESS};
        EXPECT_CALL(mock_sai_route_, create_route_entries(_, _, _, _, _, _))
            .WillOnce(DoAll(SetArrayArgument<5>(exp_status.begin(), exp_status.end()), Return(SAI_STATUS_SUCCESS)));
        EXPECT_THAT(CreateRouteEntries(std::vector<P4RouteEntry>{route_entry}),
                    ArrayEq(std::vector<StatusCode>{StatusCode::SWSS_RC_SUCCESS}));
    }

    // Sets up a drop route entry for test.
    void SetupDropRouteEntry(const std::string &vrf_id, const swss::IpPrefix &route_prefix)
    {
        auto route_entry = GenerateP4RouteEntry(vrf_id, route_prefix, p4orch::kDrop, "");

        std::vector<sai_status_t> exp_status{SAI_STATUS_SUCCESS};
        EXPECT_CALL(mock_sai_route_, create_route_entries(_, _, _, _, _, _))
            .WillOnce(DoAll(SetArrayArgument<5>(exp_status.begin(), exp_status.end()), Return(SAI_STATUS_SUCCESS)));
        EXPECT_THAT(CreateRouteEntries(std::vector<P4RouteEntry>{route_entry}),
                    ArrayEq(std::vector<StatusCode>{StatusCode::SWSS_RC_SUCCESS}));
    }

    // Sets up a trap route entry for test.
    void SetupTrapRouteEntry(const std::string &vrf_id, const swss::IpPrefix &route_prefix)
    {
        auto route_entry = GenerateP4RouteEntry(vrf_id, route_prefix, p4orch::kTrap, "");

        std::vector<sai_status_t> exp_status{SAI_STATUS_SUCCESS};
        EXPECT_CALL(mock_sai_route_, create_route_entries(_, _, _, _, _, _))
            .WillOnce(DoAll(SetArrayArgument<5>(exp_status.begin(), exp_status.end()), Return(SAI_STATUS_SUCCESS)));
        EXPECT_THAT(CreateRouteEntries(std::vector<P4RouteEntry>{route_entry}),
                    ArrayEq(std::vector<StatusCode>{StatusCode::SWSS_RC_SUCCESS}));
    }

    // Sets up a set_multicast_group_id route entry for test.
  P4RouteEntry SetupSetMulticastGroupIdRouteEntry(
      const std::string& vrf_id, const swss::IpPrefix& route_prefix,
      const std::string& multicast_group_id,
      const sai_object_id_t multicast_group_oid,
      const std::string& metadata = "",
      bool expect_rpf = true) {
      auto route_entry = GenerateP4RouteEntry(vrf_id, route_prefix,
                                              p4orch::kSetMulticastGroupId,
                             multicast_group_id, metadata);
    // Create artificial multicast group object.
    AddMulticastGroup(multicast_group_id, multicast_group_oid);
    if (expect_rpf) {
      EXPECT_CALL(mock_sai_rpf_group_, create_rpf_group(_, _, 0, _))
          .WillOnce(DoAll(SetArgPointee<0>(kRpfGroupOid1),
                          Return(SAI_STATUS_SUCCESS)));
    }
    EXPECT_CALL(mock_sai_ipmc_,
                create_ipmc_entry(_, _, _))
        .WillOnce(Return(SAI_STATUS_SUCCESS));
    EXPECT_THAT(
      CreateMulticastRouteEntries(
          std::vector<P4RouteEntry>{route_entry}),
          ArrayEq(std::vector<StatusCode>{StatusCode::SWSS_RC_SUCCESS}));
    return route_entry;
    }

    // Verifies the two given route entries are identical.
    void VerifyRouteEntriesEq(const P4RouteEntry& x, const P4RouteEntry& y,
                              bool is_multicast = false) {
      EXPECT_EQ(x.route_entry_key, y.route_entry_key);
      EXPECT_EQ(x.vrf_id, y.vrf_id);
      EXPECT_EQ(x.route_prefix, y.route_prefix);
      EXPECT_EQ(x.action, y.action);
      EXPECT_EQ(x.nexthop_id, y.nexthop_id);
      EXPECT_EQ(x.wcmp_group, y.wcmp_group);
      EXPECT_EQ(x.route_metadata, y.route_metadata);
      EXPECT_EQ(x.multicast_group_id, y.multicast_group_id);
      if (is_multicast) {
        EXPECT_EQ(x.sai_ipmc_entry.vr_id, y.sai_ipmc_entry.vr_id);
        EXPECT_EQ(x.sai_ipmc_entry.switch_id, y.sai_ipmc_entry.switch_id);
        EXPECT_TRUE(AddressCmp(&x.sai_ipmc_entry.destination,
                               &y.sai_ipmc_entry.destination));
      } else {
        EXPECT_EQ(x.sai_route_entry.vr_id, y.sai_route_entry.vr_id);
        EXPECT_EQ(x.sai_route_entry.switch_id, y.sai_route_entry.switch_id);
        EXPECT_TRUE(PrefixCmp(&x.sai_route_entry.destination,
                              &y.sai_route_entry.destination));
      }
    }

    // Verifies the given route entry exists and matches.
    void VerifyRouteEntry(const P4RouteEntry &route_entry, const sai_ip_prefix_t &sai_route_prefix,
                          const sai_object_id_t vrf_oid)
    {
        auto *route_entry_ptr = GetRouteEntry(route_entry.route_entry_key);
        P4RouteEntry expect_entry = route_entry;
        expect_entry.sai_route_entry.vr_id = vrf_oid;
        expect_entry.sai_route_entry.switch_id = gSwitchId;
        expect_entry.sai_route_entry.destination = sai_route_prefix;
        VerifyRouteEntriesEq(expect_entry, *route_entry_ptr);
        EXPECT_TRUE(p4_oid_mapper_.existsOID(SAI_OBJECT_TYPE_ROUTE_ENTRY, route_entry.route_entry_key));
    }

    // Verifies the given multicast route entry exists and matches.
    void VerifyMulticastRouteEntry(const P4RouteEntry& route_entry,
                                   const sai_ip_address_t& sai_ip_address,
                                   const sai_object_id_t vrf_oid) {
      auto* route_entry_ptr = GetRouteEntry(route_entry.route_entry_key);
      P4RouteEntry expect_entry = route_entry;
      expect_entry.sai_ipmc_entry.vr_id = vrf_oid;
      expect_entry.sai_ipmc_entry.switch_id = gSwitchId;
      expect_entry.sai_ipmc_entry.destination = sai_ip_address;
      VerifyRouteEntriesEq(expect_entry, *route_entry_ptr);
      EXPECT_TRUE(p4_oid_mapper_.existsOID(SAI_OBJECT_TYPE_IPMC_ENTRY,
                                           route_entry.route_entry_key));
    }

    StrictMock<MockSaiIpmc> mock_sai_ipmc_;
    StrictMock<MockSaiRoute> mock_sai_route_;
    StrictMock<MockSaiRpfGroup> mock_sai_rpf_group_;
    StrictMock<MockResponsePublisher> publisher_;
    P4OidMapper p4_oid_mapper_;
    RouteManager route_manager_;
};

TEST_F(RouteManagerTest, MergeRouteEntryWithNexthopIdActionDestTest)
{
    auto swss_ipv4_route_prefix = swss::IpPrefix(kIpv4Prefix);
    auto dest = GenerateP4RouteEntry(gVrfName, swss_ipv4_route_prefix, p4orch::kSetNexthopId, kNexthopId1);
    dest.sai_route_entry.vr_id = gVrfOid;
    dest.sai_route_entry.switch_id = gSwitchId;
    copy(dest.sai_route_entry.destination, swss_ipv4_route_prefix);

    // Source is identical to destination.
    auto src = GenerateP4RouteEntry(gVrfName, swss_ipv4_route_prefix, p4orch::kSetNexthopId, kNexthopId1);
    P4RouteEntry ret = {};
    EXPECT_FALSE(MergeRouteEntry(dest, src, &ret));
    VerifyRouteEntriesEq(dest, ret);

    // Source has different nexthop ID.
    src = GenerateP4RouteEntry(gVrfName, swss_ipv4_route_prefix, p4orch::kSetNexthopId, kNexthopId2);
    EXPECT_TRUE(MergeRouteEntry(dest, src, &ret));
    P4RouteEntry expect_entry = dest;
    expect_entry.nexthop_id = kNexthopId2;
    VerifyRouteEntriesEq(expect_entry, ret);

    // Source has set nexthop ID and metadata action and dest has set nexthop ID
    // action.
    src = GenerateP4RouteEntry(gVrfName, swss_ipv4_route_prefix, p4orch::kSetNexthopIdAndMetadata, kNexthopId1,
                               kMetadata1);
    EXPECT_TRUE(MergeRouteEntry(dest, src, &ret));
    expect_entry = dest;
    expect_entry.action = p4orch::kSetNexthopIdAndMetadata;
    expect_entry.route_metadata = kMetadata1;
    VerifyRouteEntriesEq(expect_entry, ret);

    // Source has wcmp group action and dest has nexhop ID action.
    src = GenerateP4RouteEntry(gVrfName, swss_ipv4_route_prefix, p4orch::kSetWcmpGroupId, kWcmpGroup1);
    EXPECT_TRUE(MergeRouteEntry(dest, src, &ret));
    expect_entry = dest;
    expect_entry.nexthop_id = "";
    expect_entry.action = p4orch::kSetWcmpGroupId;
    expect_entry.wcmp_group = kWcmpGroup1;
    VerifyRouteEntriesEq(expect_entry, ret);

    // Source has drop action and dest has nexhop ID action.
    src = GenerateP4RouteEntry(gVrfName, swss_ipv4_route_prefix, p4orch::kDrop, "");
    EXPECT_TRUE(MergeRouteEntry(dest, src, &ret));
    expect_entry = dest;
    expect_entry.nexthop_id = "";
    expect_entry.action = p4orch::kDrop;
    VerifyRouteEntriesEq(expect_entry, ret);
}

TEST_F(RouteManagerTest, MergeRouteEntryWithNexthopIdAndMetadataActionDestTest)
{
    auto swss_ipv4_route_prefix = swss::IpPrefix(kIpv4Prefix);
    auto dest = GenerateP4RouteEntry(gVrfName, swss_ipv4_route_prefix, p4orch::kSetNexthopIdAndMetadata, kNexthopId1,
                                     kMetadata1);
    dest.sai_route_entry.vr_id = gVrfOid;
    dest.sai_route_entry.switch_id = gSwitchId;
    copy(dest.sai_route_entry.destination, swss_ipv4_route_prefix);

    // Source is identical to destination.
    auto src = GenerateP4RouteEntry(gVrfName, swss_ipv4_route_prefix, p4orch::kSetNexthopIdAndMetadata, kNexthopId1,
                                    kMetadata1);
    P4RouteEntry ret = {};
    EXPECT_FALSE(MergeRouteEntry(dest, src, &ret));
    VerifyRouteEntriesEq(dest, ret);

    // Source has different metadata.
    src.route_metadata = kMetadata2;
    EXPECT_TRUE(MergeRouteEntry(dest, src, &ret));
    P4RouteEntry expect_entry = dest;
    expect_entry.route_metadata = kMetadata2;
    VerifyRouteEntriesEq(expect_entry, ret);

    // Source has different nexthop ID and metadata.
    src = GenerateP4RouteEntry(gVrfName, swss_ipv4_route_prefix, p4orch::kSetNexthopIdAndMetadata, kNexthopId2,
                               kMetadata2);
    EXPECT_TRUE(MergeRouteEntry(dest, src, &ret));
    expect_entry = dest;
    expect_entry.nexthop_id = kNexthopId2;
    expect_entry.route_metadata = kMetadata2;
    VerifyRouteEntriesEq(expect_entry, ret);

    // Source has wcmp group action and dest has nexhop ID and metadata action.
    src = GenerateP4RouteEntry(gVrfName, swss_ipv4_route_prefix, p4orch::kSetWcmpGroupId, kWcmpGroup1);
    EXPECT_TRUE(MergeRouteEntry(dest, src, &ret));
    expect_entry = dest;
    expect_entry.nexthop_id = "";
    expect_entry.action = p4orch::kSetWcmpGroupId;
    expect_entry.wcmp_group = kWcmpGroup1;
    expect_entry.route_metadata = "";
    VerifyRouteEntriesEq(expect_entry, ret);

    // Source has drop action and dest has nexhop ID and metadata action.
    src = GenerateP4RouteEntry(gVrfName, swss_ipv4_route_prefix, p4orch::kDrop, "");
    EXPECT_TRUE(MergeRouteEntry(dest, src, &ret));
    expect_entry = dest;
    expect_entry.nexthop_id = "";
    expect_entry.action = p4orch::kDrop;
    expect_entry.route_metadata = "";
    VerifyRouteEntriesEq(expect_entry, ret);

    // Source has wcmp group and metadata action and dest has nexhop ID and
    // metadata action.
    src = GenerateP4RouteEntry(gVrfName, swss_ipv4_route_prefix, p4orch::kSetWcmpGroupIdAndMetadata, kWcmpGroup1,
                               kMetadata2);
    EXPECT_TRUE(MergeRouteEntry(dest, src, &ret));
    expect_entry = dest;
    expect_entry.nexthop_id = "";
    expect_entry.action = p4orch::kSetWcmpGroupIdAndMetadata;
    expect_entry.wcmp_group = kWcmpGroup1;
    expect_entry.route_metadata = kMetadata2;
    VerifyRouteEntriesEq(expect_entry, ret);
}

TEST_F(RouteManagerTest, MergeRouteEntryWithWcmpGroupActionDestTest)
{
    auto swss_ipv4_route_prefix = swss::IpPrefix(kIpv4Prefix);
    auto dest = GenerateP4RouteEntry(gVrfName, swss_ipv4_route_prefix, p4orch::kSetWcmpGroupId, kWcmpGroup1);
    dest.sai_route_entry.vr_id = gVrfOid;
    dest.sai_route_entry.switch_id = gSwitchId;
    copy(dest.sai_route_entry.destination, swss_ipv4_route_prefix);

    // Source is identical to destination.
    auto src = GenerateP4RouteEntry(gVrfName, swss_ipv4_route_prefix, p4orch::kSetWcmpGroupId, kWcmpGroup1);
    P4RouteEntry ret = {};
    EXPECT_FALSE(MergeRouteEntry(dest, src, &ret));
    VerifyRouteEntriesEq(dest, ret);

    // Source has different wcmp group.
    src = GenerateP4RouteEntry(gVrfName, swss_ipv4_route_prefix, p4orch::kSetWcmpGroupId, kWcmpGroup2);
    EXPECT_TRUE(MergeRouteEntry(dest, src, &ret));
    P4RouteEntry expect_entry = dest;
    expect_entry.wcmp_group = kWcmpGroup2;
    VerifyRouteEntriesEq(expect_entry, ret);

    // Source has nexthop ID action and dest has wcmp group action.
    src = GenerateP4RouteEntry(gVrfName, swss_ipv4_route_prefix, p4orch::kSetNexthopId, kNexthopId1);
    EXPECT_TRUE(MergeRouteEntry(dest, src, &ret));
    expect_entry = dest;
    expect_entry.wcmp_group = "";
    expect_entry.action = p4orch::kSetNexthopId;
    expect_entry.nexthop_id = kNexthopId1;
    VerifyRouteEntriesEq(expect_entry, ret);

    // Source has drop action and dest has wcmp group action.
    src = GenerateP4RouteEntry(gVrfName, swss_ipv4_route_prefix, p4orch::kDrop, "");
    EXPECT_TRUE(MergeRouteEntry(dest, src, &ret));
    expect_entry = dest;
    expect_entry.wcmp_group = "";
    expect_entry.action = p4orch::kDrop;
    VerifyRouteEntriesEq(expect_entry, ret);
}

TEST_F(RouteManagerTest, MergeRouteEntryWithDropActionDestTest)
{
    auto swss_ipv4_route_prefix = swss::IpPrefix(kIpv4Prefix);
    auto dest = GenerateP4RouteEntry(gVrfName, swss_ipv4_route_prefix, p4orch::kDrop, "");
    dest.sai_route_entry.vr_id = gVrfOid;
    dest.sai_route_entry.switch_id = gSwitchId;
    copy(dest.sai_route_entry.destination, swss_ipv4_route_prefix);

    // Source is identical to destination.
    auto src = GenerateP4RouteEntry(gVrfName, swss_ipv4_route_prefix, p4orch::kDrop, "");
    P4RouteEntry ret = {};
    EXPECT_FALSE(MergeRouteEntry(dest, src, &ret));
    VerifyRouteEntriesEq(dest, ret);

    // Source has nexthop ID action and dest has drop action.
    src = GenerateP4RouteEntry(gVrfName, swss_ipv4_route_prefix, p4orch::kSetNexthopId, kNexthopId1);
    EXPECT_TRUE(MergeRouteEntry(dest, src, &ret));
    P4RouteEntry expect_entry = dest;
    expect_entry.action = p4orch::kSetNexthopId;
    expect_entry.nexthop_id = kNexthopId1;
    VerifyRouteEntriesEq(expect_entry, ret);

    // Source has wcmp group and metadata action and dest has drop action.
    src = GenerateP4RouteEntry(gVrfName, swss_ipv4_route_prefix, p4orch::kSetWcmpGroupIdAndMetadata, kWcmpGroup1,
                               kMetadata1);
    EXPECT_TRUE(MergeRouteEntry(dest, src, &ret));
    expect_entry = dest;
    expect_entry.action = p4orch::kSetWcmpGroupIdAndMetadata;
    expect_entry.nexthop_id = "";
    expect_entry.wcmp_group = kWcmpGroup1;
    expect_entry.route_metadata = kMetadata1;
    VerifyRouteEntriesEq(expect_entry, ret);
}

TEST_F(RouteManagerTest, MergeRouteEntryWithTrapActionDestTest)
{
    auto swss_ipv4_route_prefix = swss::IpPrefix(kIpv4Prefix);
    auto dest = GenerateP4RouteEntry(gVrfName, swss_ipv4_route_prefix, p4orch::kTrap, "");
    dest.sai_route_entry.vr_id = gVrfOid;
    dest.sai_route_entry.switch_id = gSwitchId;
    copy(dest.sai_route_entry.destination, swss_ipv4_route_prefix);

    // Source is identical to destination.
    auto src = GenerateP4RouteEntry(gVrfName, swss_ipv4_route_prefix, p4orch::kTrap, "");
    P4RouteEntry ret = {};
    EXPECT_FALSE(MergeRouteEntry(dest, src, &ret));
    VerifyRouteEntriesEq(dest, ret);

    // Source has nexthop ID action and dest has trap action.
    src = GenerateP4RouteEntry(gVrfName, swss_ipv4_route_prefix, p4orch::kSetNexthopId, kNexthopId1);
    EXPECT_TRUE(MergeRouteEntry(dest, src, &ret));
    P4RouteEntry expect_entry = dest;
    expect_entry.action = p4orch::kSetNexthopId;
    expect_entry.nexthop_id = kNexthopId1;
    VerifyRouteEntriesEq(expect_entry, ret);

    // Source has wcmp group action and dest has trap action.
    src = GenerateP4RouteEntry(gVrfName, swss_ipv4_route_prefix, p4orch::kSetWcmpGroupId, kWcmpGroup1);
    EXPECT_TRUE(MergeRouteEntry(dest, src, &ret));
    expect_entry = dest;
    expect_entry.action = p4orch::kSetWcmpGroupId;
    expect_entry.wcmp_group = kWcmpGroup1;
    VerifyRouteEntriesEq(expect_entry, ret);
}

TEST_F(RouteManagerTest, DeserializeRouteEntryWithNexthopIdActionTest)
{
    std::string key = R"({"match/vrf_id":"b4-traffic","match/ipv4_dst":"10.11.12.0/24"})";
    std::vector<swss::FieldValueTuple> attributes;
    attributes.push_back(swss::FieldValueTuple{p4orch::kAction, p4orch::kSetNexthopId});
    attributes.push_back(swss::FieldValueTuple{prependParamField(p4orch::kNexthopId), kNexthopId1});
    auto route_entry_or = DeserializeRouteEntry(key, attributes, APP_P4RT_IPV4_TABLE_NAME);
    EXPECT_TRUE(route_entry_or.ok());
    auto &route_entry = *route_entry_or;
    auto expect_entry =
        GenerateP4RouteEntry("b4-traffic", swss::IpPrefix("10.11.12.0/24"), p4orch::kSetNexthopId, kNexthopId1);
    VerifyRouteEntriesEq(expect_entry, route_entry);
}

TEST_F(RouteManagerTest, DeserializeRouteEntryWithWcmpGroupActionTest)
{
    std::string key = R"({"match/vrf_id":"b4-traffic","match/ipv4_dst":"10.11.12.0/24"})";
    std::vector<swss::FieldValueTuple> attributes;
    attributes.push_back(swss::FieldValueTuple{p4orch::kAction, p4orch::kSetWcmpGroupId});
    attributes.push_back(swss::FieldValueTuple{prependParamField(p4orch::kWcmpGroupId), kWcmpGroup1});
    auto route_entry_or = DeserializeRouteEntry(key, attributes, APP_P4RT_IPV4_TABLE_NAME);
    EXPECT_TRUE(route_entry_or.ok());
    auto &route_entry = *route_entry_or;
    auto expect_entry =
        GenerateP4RouteEntry("b4-traffic", swss::IpPrefix("10.11.12.0/24"), p4orch::kSetWcmpGroupId, kWcmpGroup1);
    VerifyRouteEntriesEq(expect_entry, route_entry);
}

TEST_F(RouteManagerTest, DeserializeRouteEntryWithNexthopIdAdnMetadataActionTest)
{
    std::string key = R"({"match/vrf_id":"b4-traffic","match/ipv4_dst":"10.11.12.0/24"})";
    std::vector<swss::FieldValueTuple> attributes;
    attributes.push_back(swss::FieldValueTuple{p4orch::kAction, p4orch::kSetNexthopIdAndMetadata});
    attributes.push_back(swss::FieldValueTuple{prependParamField(p4orch::kNexthopId), kNexthopId1});
    attributes.push_back(swss::FieldValueTuple{prependParamField(p4orch::kRouteMetadata), kMetadata1});
    auto route_entry_or = DeserializeRouteEntry(key, attributes, APP_P4RT_IPV4_TABLE_NAME);
    EXPECT_TRUE(route_entry_or.ok());
    auto &route_entry = *route_entry_or;
    auto expect_entry = GenerateP4RouteEntry("b4-traffic", swss::IpPrefix("10.11.12.0/24"),
                                             p4orch::kSetNexthopIdAndMetadata, kNexthopId1, kMetadata1);
    VerifyRouteEntriesEq(expect_entry, route_entry);
}

TEST_F(RouteManagerTest, DeserializeRouteEntryWithWcmpGroupAndMetadataActionTest)
{
    std::string key = R"({"match/vrf_id":"b4-traffic","match/ipv4_dst":"10.11.12.0/24"})";
    std::vector<swss::FieldValueTuple> attributes;
    attributes.push_back(swss::FieldValueTuple{p4orch::kAction, p4orch::kSetWcmpGroupIdAndMetadata});
    attributes.push_back(swss::FieldValueTuple{prependParamField(p4orch::kWcmpGroupId), kWcmpGroup1});
    attributes.push_back(swss::FieldValueTuple{prependParamField(p4orch::kRouteMetadata), kMetadata1});
    auto route_entry_or = DeserializeRouteEntry(key, attributes, APP_P4RT_IPV4_TABLE_NAME);
    EXPECT_TRUE(route_entry_or.ok());
    auto &route_entry = *route_entry_or;
    auto expect_entry = GenerateP4RouteEntry("b4-traffic", swss::IpPrefix("10.11.12.0/24"),
                                             p4orch::kSetWcmpGroupIdAndMetadata, kWcmpGroup1, kMetadata1);
    VerifyRouteEntriesEq(expect_entry, route_entry);
}

TEST_F(RouteManagerTest, DeserializeRouteEntryWithDropActionTest)
{
    std::string key = R"({"match/vrf_id":"b4-traffic","match/ipv6_dst":"2001:db8:1::/32"})";
    std::vector<swss::FieldValueTuple> attributes;
    attributes.push_back(swss::FieldValueTuple{p4orch::kAction, p4orch::kDrop});
    auto route_entry_or = DeserializeRouteEntry(key, attributes, APP_P4RT_IPV6_TABLE_NAME);
    EXPECT_TRUE(route_entry_or.ok());
    auto &route_entry = *route_entry_or;
    auto expect_entry = GenerateP4RouteEntry("b4-traffic", swss::IpPrefix("2001:db8:1::/32"), p4orch::kDrop, "");
    VerifyRouteEntriesEq(expect_entry, route_entry);
}

TEST_F(RouteManagerTest, DeserializeRouteEntryWithTrapActionTest)
{
    std::string key = R"({"match/vrf_id":"b4-traffic","match/ipv6_dst":"2001:db8:1::/32"})";
    std::vector<swss::FieldValueTuple> attributes;
    attributes.push_back(swss::FieldValueTuple{p4orch::kAction, p4orch::kTrap});
    auto route_entry_or = DeserializeRouteEntry(key, attributes, APP_P4RT_IPV6_TABLE_NAME);
    EXPECT_TRUE(route_entry_or.ok());
    auto &route_entry = *route_entry_or;
    auto expect_entry = GenerateP4RouteEntry("b4-traffic", swss::IpPrefix("2001:db8:1::/32"), p4orch::kTrap, "");
    VerifyRouteEntriesEq(expect_entry, route_entry);
}

TEST_F(RouteManagerTest,
       DeserializeRouteEntryWithSetMulticastGroupIdActionTest) {
  std::string key =
      R"({"match/vrf_id":"b4-traffic","match/ipv6_dst":"2001:db8:1::/32"})";
  std::vector<swss::FieldValueTuple> attributes;
  attributes.push_back(swss::FieldValueTuple{
      p4orch::kAction, p4orch::kSetMulticastGroupId});
  attributes.push_back(swss::FieldValueTuple{
      prependParamField(p4orch::kMulticastGroupId), kMulticastGroupId1});
  auto route_entry_or =
      DeserializeRouteEntry(key, attributes, APP_P4RT_IPV6_TABLE_NAME);
  EXPECT_TRUE(route_entry_or.ok());
  auto& route_entry = *route_entry_or;
  auto expect_entry = GenerateP4RouteEntry(
      "b4-traffic", swss::IpPrefix("2001:db8:1::/32"),
      p4orch::kSetMulticastGroupId, kMulticastGroupId1);
  VerifyRouteEntriesEq(expect_entry, route_entry);
}

TEST_F(RouteManagerTest, DeserializeRouteEntryWithInvalidKeyShouldFail)
{
    std::string key = "{{{{{{{{{{{{";
    std::vector<swss::FieldValueTuple> attributes;
    attributes.push_back(swss::FieldValueTuple{p4orch::kAction, p4orch::kDrop});
    auto route_entry_or = DeserializeRouteEntry(key, attributes, APP_P4RT_IPV6_TABLE_NAME);
    EXPECT_FALSE(route_entry_or.ok());
}

TEST_F(RouteManagerTest, DeserializeRouteEntryWithInvalidFieldShouldFail)
{
    std::string key = R"({"match/vrf_id":"b4-traffic","match/ipv6_dst":"2001:db8:1::/32"})";
    std::vector<swss::FieldValueTuple> attributes;
    attributes.push_back(swss::FieldValueTuple{"invalid", "invalid"});
    auto route_entry_or = DeserializeRouteEntry(key, attributes, APP_P4RT_IPV6_TABLE_NAME);
    EXPECT_FALSE(route_entry_or.ok());
}

TEST_F(RouteManagerTest, DeserializeRouteEntryWithInvalidRouteShouldFail)
{
    std::string key = R"({"match/vrf_id":"b4-traffic","match/ipv6_dst":"invalid"})";
    std::vector<swss::FieldValueTuple> attributes;
    attributes.push_back(swss::FieldValueTuple{p4orch::kAction, p4orch::kDrop});
    auto route_entry_or = DeserializeRouteEntry(key, attributes, APP_P4RT_IPV6_TABLE_NAME);
    EXPECT_FALSE(route_entry_or.ok());
}

TEST_F(RouteManagerTest, DeserializeRouteEntryWithoutIpv4WildcardLpmMatchShouldSucceed)
{
    std::string key = R"({"match/vrf_id":"b4-traffic"})";
    std::vector<swss::FieldValueTuple> attributes;
    attributes.push_back(swss::FieldValueTuple{p4orch::kAction, p4orch::kDrop});
    auto route_entry_or = DeserializeRouteEntry(key, attributes, APP_P4RT_IPV4_TABLE_NAME);
    EXPECT_TRUE(route_entry_or.ok());
    auto &route_entry = *route_entry_or;
    auto expect_entry = GenerateP4RouteEntry("b4-traffic", swss::IpPrefix("0.0.0.0/0"), p4orch::kDrop, "");
    VerifyRouteEntriesEq(expect_entry, route_entry);
}

TEST_F(RouteManagerTest, DeserializeRouteEntryWithoutIpv6WildcardLpmMatchShouldSucceed)
{
    std::string key = R"({"match/vrf_id":"b4-traffic"})";
    std::vector<swss::FieldValueTuple> attributes;
    attributes.push_back(swss::FieldValueTuple{p4orch::kAction, p4orch::kDrop});
    auto route_entry_or = DeserializeRouteEntry(key, attributes, APP_P4RT_IPV6_TABLE_NAME);
    EXPECT_TRUE(route_entry_or.ok());
    auto &route_entry = *route_entry_or;
    auto expect_entry = GenerateP4RouteEntry("b4-traffic", swss::IpPrefix("::/0"), p4orch::kDrop, "");
    VerifyRouteEntriesEq(expect_entry, route_entry);
}

TEST_F(RouteManagerTest, ValidateRouteEntryNexthopActionWithInvalidNexthopShouldFail)
{
    auto swss_ipv4_route_prefix = swss::IpPrefix(kIpv4Prefix);
    auto route_entry = GenerateP4RouteEntry(gVrfName, swss_ipv4_route_prefix, p4orch::kSetNexthopId, kNexthopId1);
    EXPECT_EQ(StatusCode::SWSS_RC_NOT_FOUND, ValidateRouteEntry(route_entry, SET_COMMAND));
}

TEST_F(RouteManagerTest, ValidateRouteEntryNexthopActionWithValidNexthopShouldSucceed)
{
    auto swss_ipv4_route_prefix = swss::IpPrefix(kIpv4Prefix);
    auto route_entry = GenerateP4RouteEntry(gVrfName, swss_ipv4_route_prefix, p4orch::kSetNexthopId, kNexthopId1);
    p4_oid_mapper_.setOID(SAI_OBJECT_TYPE_NEXT_HOP, KeyGenerator::generateNextHopKey(route_entry.nexthop_id),
                          kNexthopOid1);
    EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS, ValidateRouteEntry(route_entry, SET_COMMAND));
}

TEST_F(RouteManagerTest, ValidateRouteEntryWcmpGroupActionWithInvalidWcmpGroupShouldFail)
{
    auto swss_ipv4_route_prefix = swss::IpPrefix(kIpv4Prefix);
    auto route_entry = GenerateP4RouteEntry(gVrfName, swss_ipv4_route_prefix, p4orch::kSetWcmpGroupId, kWcmpGroup1);
    EXPECT_EQ(StatusCode::SWSS_RC_NOT_FOUND, ValidateRouteEntry(route_entry, SET_COMMAND));
}

TEST_F(RouteManagerTest, ValidateRouteEntryWcmpGroupActionWithValidWcmpGroupShouldSucceed)
{
    auto swss_ipv4_route_prefix = swss::IpPrefix(kIpv4Prefix);
    auto route_entry = GenerateP4RouteEntry(gVrfName, swss_ipv4_route_prefix, p4orch::kSetWcmpGroupId, kWcmpGroup1);
    p4_oid_mapper_.setOID(SAI_OBJECT_TYPE_NEXT_HOP_GROUP, KeyGenerator::generateWcmpGroupKey(route_entry.wcmp_group),
                          kWcmpGroupOid1);
    EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS, ValidateRouteEntry(route_entry, SET_COMMAND));
}

TEST_F(RouteManagerTest,
       ValidateRouteEntrySetMulticastGroupIdActionShouldSucceed) {
  auto swss_ipv4_route_prefix = swss::IpPrefix(kIpv6Prefix);
  auto route_entry =
      GenerateP4RouteEntry(gVrfName, swss_ipv4_route_prefix,
                           p4orch::kSetMulticastGroupId, kMulticastGroupId1);

  // Fake that multicast groups have been added
  AddMulticastGroup(kMulticastGroupId1, kMulticastGroupOid1);
  EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS,
            ValidateRouteEntry(route_entry, SET_COMMAND));
}

TEST_F(RouteManagerTest,
       ValidateRouteEntrySetMulticastGroupIdActionShouldFail) {
  auto swss_ipv4_route_prefix = swss::IpPrefix(kIpv6Prefix);
  auto route_entry = GenerateP4RouteEntry(gVrfName, swss_ipv4_route_prefix,
                                          p4orch::kSetMulticastGroupId, "");
  // Fake that multicast groups have been added
  AddMulticastGroup(kMulticastGroupId1, kMulticastGroupOid1);
  EXPECT_EQ(StatusCode::SWSS_RC_INVALID_PARAM,
            ValidateRouteEntry(route_entry, SET_COMMAND));
}

TEST_F(RouteManagerTest,
       ValidateRouteEntrySetMulticastGroupIdActionShouldFailNoGroup) {
  auto swss_ipv4_route_prefix = swss::IpPrefix(kIpv6Prefix);
  auto route_entry =
      GenerateP4RouteEntry(gVrfName, swss_ipv4_route_prefix,
                           p4orch::kSetMulticastGroupId, kMulticastGroupId1);

  // No multicast group will be found.
  EXPECT_EQ(StatusCode::SWSS_RC_NOT_FOUND,
            ValidateRouteEntry(route_entry, SET_COMMAND));
}

TEST_F(RouteManagerTest,
       ValidateRouteEntryWcmpGroupActionWithExtraMulticastGroupShouldFail) {
  auto swss_ipv4_route_prefix = swss::IpPrefix(kIpv4Prefix);
  auto route_entry = GenerateP4RouteEntry(gVrfName, swss_ipv4_route_prefix,
                                          p4orch::kSetWcmpGroupId, kWcmpGroup1);
  // Unexpected group.
  route_entry.multicast_group_id = kMulticastGroupId1;
  p4_oid_mapper_.setOID(
      SAI_OBJECT_TYPE_NEXT_HOP_GROUP,
      KeyGenerator::generateWcmpGroupKey(route_entry.wcmp_group),
      kWcmpGroupOid1);
  // Fake that multicast groups have been added
  AddMulticastGroup(kMulticastGroupId1, kMulticastGroupOid1);
  EXPECT_EQ(StatusCode::SWSS_RC_INVALID_PARAM,
            ValidateRouteEntry(route_entry, SET_COMMAND));
}

TEST_F(RouteManagerTest, ValidateRouteEntryWithInvalidCommandShouldFail)
{
    auto swss_ipv4_route_prefix = swss::IpPrefix(kIpv4Prefix);
    auto route_entry = GenerateP4RouteEntry(gVrfName, swss_ipv4_route_prefix, p4orch::kDrop, "");
    EXPECT_EQ(StatusCode::SWSS_RC_INVALID_PARAM, ValidateRouteEntry(route_entry, "invalid"));
}

TEST_F(RouteManagerTest, ValidateSetRouteEntryDoesNotExistInManagerShouldFail)
{
    auto swss_ipv4_route_prefix = swss::IpPrefix(kIpv4Prefix);
    auto route_entry = GenerateP4RouteEntry(gVrfName, swss_ipv4_route_prefix, p4orch::kSetNexthopId, kNexthopId1);
    route_entry.action = "";
    p4_oid_mapper_.setOID(SAI_OBJECT_TYPE_NEXT_HOP, KeyGenerator::generateNextHopKey(route_entry.nexthop_id),
                          kNexthopOid1);
    EXPECT_EQ(StatusCode::SWSS_RC_INVALID_PARAM, ValidateRouteEntry(route_entry, SET_COMMAND));
}

TEST_F(RouteManagerTest, ValidateSetRouteEntryExistsInMapperDoesNotExistInManagerShouldFail)
{
    auto swss_ipv4_route_prefix = swss::IpPrefix(kIpv4Prefix);
    auto route_entry = GenerateP4RouteEntry(gVrfName, swss_ipv4_route_prefix, p4orch::kSetNexthopId, kNexthopId1);
    p4_oid_mapper_.setOID(SAI_OBJECT_TYPE_NEXT_HOP, KeyGenerator::generateNextHopKey(route_entry.nexthop_id),
                          kNexthopOid1);
    p4_oid_mapper_.setDummyOID(SAI_OBJECT_TYPE_ROUTE_ENTRY, route_entry.route_entry_key);
    EXPECT_EQ(StatusCode::SWSS_RC_NOT_FOUND, ValidateRouteEntry(route_entry, SET_COMMAND));
}

TEST_F(RouteManagerTest, ValidateSetRouteEntryExistsInManagerDoesNotExistInMapperShouldFail)
{
    auto swss_ipv4_route_prefix = swss::IpPrefix(kIpv4Prefix);
    SetupNexthopIdRouteEntry(gVrfName, swss_ipv4_route_prefix, kNexthopId1, kNexthopOid1);
    auto route_entry = GenerateP4RouteEntry(gVrfName, swss_ipv4_route_prefix, p4orch::kSetNexthopId, kNexthopId1);
    p4_oid_mapper_.eraseOID(SAI_OBJECT_TYPE_ROUTE_ENTRY, route_entry.route_entry_key);
    EXPECT_EQ(StatusCode::SWSS_RC_NOT_FOUND, ValidateRouteEntry(route_entry, SET_COMMAND));
}

TEST_F(RouteManagerTest, ValidateSetRouteEntryNexthopIdActionWithoutNexthopIdShouldFail)
{
    auto swss_ipv4_route_prefix = swss::IpPrefix(kIpv4Prefix);
    auto route_entry = GenerateP4RouteEntry(gVrfName, swss_ipv4_route_prefix, p4orch::kSetNexthopId, "");
    EXPECT_EQ(StatusCode::SWSS_RC_INVALID_PARAM, ValidateRouteEntry(route_entry, SET_COMMAND));
}

TEST_F(RouteManagerTest, ValidateSetRouteEntryNexthopIdActionWithWcmpGroupShouldFail)
{
    auto swss_ipv4_route_prefix = swss::IpPrefix(kIpv4Prefix);
    auto route_entry = GenerateP4RouteEntry(gVrfName, swss_ipv4_route_prefix, p4orch::kSetNexthopId, kNexthopId1);
    route_entry.wcmp_group = kWcmpGroup1;
    p4_oid_mapper_.setOID(SAI_OBJECT_TYPE_NEXT_HOP, KeyGenerator::generateNextHopKey(route_entry.nexthop_id),
                          kNexthopOid1);
    p4_oid_mapper_.setOID(SAI_OBJECT_TYPE_NEXT_HOP_GROUP, KeyGenerator::generateWcmpGroupKey(route_entry.wcmp_group),
                          kWcmpGroupOid1);
    EXPECT_EQ(StatusCode::SWSS_RC_INVALID_PARAM, ValidateRouteEntry(route_entry, SET_COMMAND));
}

TEST_F(RouteManagerTest, ValidateSetRouteEntryWcmpGroupActionWithoutWcmpGroupShouldFail)
{
    auto swss_ipv4_route_prefix = swss::IpPrefix(kIpv4Prefix);
    auto route_entry = GenerateP4RouteEntry(gVrfName, swss_ipv4_route_prefix, p4orch::kSetWcmpGroupId, "");
    EXPECT_EQ(StatusCode::SWSS_RC_INVALID_PARAM, ValidateRouteEntry(route_entry, SET_COMMAND));
}

TEST_F(RouteManagerTest, ValidateSetRouteEntryWcmpGroupActionWithNexthopIdShouldFail)
{
    auto swss_ipv4_route_prefix = swss::IpPrefix(kIpv4Prefix);
    auto route_entry = GenerateP4RouteEntry(gVrfName, swss_ipv4_route_prefix, p4orch::kSetWcmpGroupId, kWcmpGroup1);
    route_entry.nexthop_id = kNexthopId1;
    p4_oid_mapper_.setOID(SAI_OBJECT_TYPE_NEXT_HOP, KeyGenerator::generateNextHopKey(route_entry.nexthop_id),
                          kNexthopOid1);
    p4_oid_mapper_.setOID(SAI_OBJECT_TYPE_NEXT_HOP_GROUP, KeyGenerator::generateWcmpGroupKey(route_entry.wcmp_group),
                          kWcmpGroupOid1);
    EXPECT_EQ(StatusCode::SWSS_RC_INVALID_PARAM, ValidateRouteEntry(route_entry, SET_COMMAND));
}

TEST_F(RouteManagerTest, ValidateSetRouteEntryDropActionWithNexthopIdShouldFail)
{
    auto swss_ipv4_route_prefix = swss::IpPrefix(kIpv4Prefix);
    auto route_entry = GenerateP4RouteEntry(gVrfName, swss_ipv4_route_prefix, p4orch::kDrop, "");
    route_entry.nexthop_id = kNexthopId1;
    p4_oid_mapper_.setOID(SAI_OBJECT_TYPE_NEXT_HOP, KeyGenerator::generateNextHopKey(route_entry.nexthop_id),
                          kNexthopOid1);
    EXPECT_EQ(StatusCode::SWSS_RC_INVALID_PARAM, ValidateRouteEntry(route_entry, SET_COMMAND));
}

TEST_F(RouteManagerTest, ValidateSetRouteEntryWcmpGroupActionWithNonemptyMetadataShouldFail)
{
    auto swss_ipv4_route_prefix = swss::IpPrefix(kIpv4Prefix);
    auto route_entry =
        GenerateP4RouteEntry(gVrfName, swss_ipv4_route_prefix, p4orch::kSetWcmpGroupId, kWcmpGroup1, kMetadata1);
    p4_oid_mapper_.setOID(SAI_OBJECT_TYPE_NEXT_HOP_GROUP, KeyGenerator::generateWcmpGroupKey(route_entry.wcmp_group),
                          kWcmpGroupOid1);
    EXPECT_EQ(StatusCode::SWSS_RC_INVALID_PARAM, ValidateRouteEntry(route_entry, SET_COMMAND));
}

TEST_F(RouteManagerTest, ValidateSetRouteEntryNexthopIdAndMetadataActionWithEmptyMetadataShouldFail)
{
    auto swss_ipv4_route_prefix = swss::IpPrefix(kIpv4Prefix);
    auto route_entry =
        GenerateP4RouteEntry(gVrfName, swss_ipv4_route_prefix, p4orch::kSetNexthopIdAndMetadata, kNexthopId1);
    p4_oid_mapper_.setOID(SAI_OBJECT_TYPE_NEXT_HOP, KeyGenerator::generateNextHopKey(route_entry.nexthop_id),
                          kNexthopOid1);
    EXPECT_EQ(StatusCode::SWSS_RC_INVALID_PARAM, ValidateRouteEntry(route_entry, SET_COMMAND));
}

TEST_F(RouteManagerTest, ValidateSetRouteEntryNexthopIdAndMetadataActionWithInvalidMetadataShouldFail)
{
    auto swss_ipv4_route_prefix = swss::IpPrefix(kIpv4Prefix);
    auto route_entry = GenerateP4RouteEntry(gVrfName, swss_ipv4_route_prefix, p4orch::kSetNexthopIdAndMetadata,
                                            kNexthopId1, "invalid_int");
    p4_oid_mapper_.setOID(SAI_OBJECT_TYPE_NEXT_HOP, KeyGenerator::generateNextHopKey(route_entry.nexthop_id),
                          kNexthopOid1);
    EXPECT_EQ(StatusCode::SWSS_RC_INVALID_PARAM, ValidateRouteEntry(route_entry, SET_COMMAND));
}

TEST_F(RouteManagerTest, ValidateSetRouteEntryDropActionWithWcmpGroupShouldFail)
{
    auto swss_ipv4_route_prefix = swss::IpPrefix(kIpv4Prefix);
    auto route_entry = GenerateP4RouteEntry(gVrfName, swss_ipv4_route_prefix, p4orch::kDrop, "");
    route_entry.wcmp_group = kWcmpGroup1;
    p4_oid_mapper_.setOID(SAI_OBJECT_TYPE_NEXT_HOP_GROUP, KeyGenerator::generateWcmpGroupKey(route_entry.wcmp_group),
                          kWcmpGroupOid1);
    EXPECT_EQ(StatusCode::SWSS_RC_INVALID_PARAM, ValidateRouteEntry(route_entry, SET_COMMAND));
}

TEST_F(RouteManagerTest, ValidateSetRouteEntryTrapActionWithNexthopIdShouldFail)
{
    auto swss_ipv4_route_prefix = swss::IpPrefix(kIpv4Prefix);
    auto route_entry = GenerateP4RouteEntry(gVrfName, swss_ipv4_route_prefix, p4orch::kTrap, "");
    route_entry.nexthop_id = kNexthopId1;
    p4_oid_mapper_.setOID(SAI_OBJECT_TYPE_NEXT_HOP, KeyGenerator::generateNextHopKey(route_entry.nexthop_id),
                          kNexthopOid1);
    EXPECT_EQ(StatusCode::SWSS_RC_INVALID_PARAM, ValidateRouteEntry(route_entry, SET_COMMAND));
}

TEST_F(RouteManagerTest, ValidateSetRouteEntryTrapActionWithWcmpGroupShouldFail)
{
    auto swss_ipv4_route_prefix = swss::IpPrefix(kIpv4Prefix);
    auto route_entry = GenerateP4RouteEntry(gVrfName, swss_ipv4_route_prefix, p4orch::kTrap, "");
    route_entry.wcmp_group = kWcmpGroup1;
    p4_oid_mapper_.setOID(SAI_OBJECT_TYPE_NEXT_HOP_GROUP, KeyGenerator::generateWcmpGroupKey(route_entry.wcmp_group),
                          kWcmpGroupOid1);
    EXPECT_EQ(StatusCode::SWSS_RC_INVALID_PARAM, ValidateRouteEntry(route_entry, SET_COMMAND));
}

TEST_F(RouteManagerTest, ValidateSetRouteEntryInvalidActionShouldFail)
{
    auto swss_ipv4_route_prefix = swss::IpPrefix(kIpv4Prefix);
    auto route_entry = GenerateP4RouteEntry(gVrfName, swss_ipv4_route_prefix, p4orch::kDrop, "");
    route_entry.action = "invalid";
    EXPECT_EQ(StatusCode::SWSS_RC_INVALID_PARAM, ValidateRouteEntry(route_entry, SET_COMMAND));
}

TEST_F(RouteManagerTest, ValidateSetRouteEntrySucceeds)
{
    auto swss_ipv4_route_prefix = swss::IpPrefix(kIpv4Prefix);
    SetupNexthopIdRouteEntry(gVrfName, swss_ipv4_route_prefix, kNexthopId1, kNexthopOid1);
    auto route_entry = GenerateP4RouteEntry(gVrfName, swss_ipv4_route_prefix, p4orch::kSetNexthopId, kNexthopId2);
    route_entry.action = "";
    p4_oid_mapper_.setOID(SAI_OBJECT_TYPE_NEXT_HOP, KeyGenerator::generateNextHopKey(route_entry.nexthop_id),
                          kNexthopOid2);
    EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS, ValidateRouteEntry(route_entry, SET_COMMAND));
}

TEST_F(RouteManagerTest, ValidateDelRouteEntryDoesNotExistInManagerShouldFail)
{
    auto swss_ipv4_route_prefix = swss::IpPrefix(kIpv4Prefix);
    auto route_entry = GenerateP4RouteEntry(gVrfName, swss_ipv4_route_prefix, "", "");
    EXPECT_EQ(StatusCode::SWSS_RC_NOT_FOUND, ValidateRouteEntry(route_entry, DEL_COMMAND));
}

TEST_F(RouteManagerTest, ValidateDelRouteEntryDoesNotExistInMapperShouldFail)
{
    auto swss_ipv4_route_prefix = swss::IpPrefix(kIpv4Prefix);
    SetupNexthopIdRouteEntry(gVrfName, swss_ipv4_route_prefix, kNexthopId1, kNexthopOid1);
    auto route_entry = GenerateP4RouteEntry(gVrfName, swss_ipv4_route_prefix, "", "");
    p4_oid_mapper_.eraseOID(SAI_OBJECT_TYPE_ROUTE_ENTRY, route_entry.route_entry_key);
    // TODO: Expect critical state.
    EXPECT_EQ(StatusCode::SWSS_RC_INTERNAL, ValidateRouteEntry(route_entry, DEL_COMMAND));
}

TEST_F(RouteManagerTest, ValidateDelRouteEntryHasActionShouldFail)
{
    auto swss_ipv4_route_prefix = swss::IpPrefix(kIpv4Prefix);
    SetupNexthopIdRouteEntry(gVrfName, swss_ipv4_route_prefix, kNexthopId1, kNexthopOid1);
    auto route_entry = GenerateP4RouteEntry(gVrfName, swss_ipv4_route_prefix, p4orch::kSetNexthopId, "");
    EXPECT_EQ(StatusCode::SWSS_RC_INVALID_PARAM, ValidateRouteEntry(route_entry, DEL_COMMAND));
}

TEST_F(RouteManagerTest, ValidateDelRouteEntryHasNexthopIdShouldFail)
{
    auto swss_ipv4_route_prefix = swss::IpPrefix(kIpv4Prefix);
    SetupNexthopIdRouteEntry(gVrfName, swss_ipv4_route_prefix, kNexthopId1, kNexthopOid1);
    auto route_entry = GenerateP4RouteEntry(gVrfName, swss_ipv4_route_prefix, p4orch::kSetNexthopId, kNexthopId1);
    route_entry.action = "";
    p4_oid_mapper_.setOID(SAI_OBJECT_TYPE_NEXT_HOP, KeyGenerator::generateNextHopKey(route_entry.nexthop_id),
                          kNexthopOid1);
    EXPECT_EQ(StatusCode::SWSS_RC_INVALID_PARAM, ValidateRouteEntry(route_entry, DEL_COMMAND));
}

TEST_F(RouteManagerTest, ValidateDelRouteEntryHasWcmpGroupShouldFail)
{
    auto swss_ipv4_route_prefix = swss::IpPrefix(kIpv4Prefix);
    SetupNexthopIdRouteEntry(gVrfName, swss_ipv4_route_prefix, kNexthopId1, kNexthopOid1);
    auto route_entry = GenerateP4RouteEntry(gVrfName, swss_ipv4_route_prefix, p4orch::kSetWcmpGroupId, kWcmpGroup1);
    route_entry.action = "";
    p4_oid_mapper_.setOID(SAI_OBJECT_TYPE_NEXT_HOP_GROUP, KeyGenerator::generateWcmpGroupKey(route_entry.wcmp_group),
                          kWcmpGroupOid1);
    EXPECT_EQ(StatusCode::SWSS_RC_INVALID_PARAM, ValidateRouteEntry(route_entry, DEL_COMMAND));
}

TEST_F(RouteManagerTest, ValidateDelRouteEntryHasMetadataShouldFail)
{
    auto swss_ipv4_route_prefix = swss::IpPrefix(kIpv4Prefix);
    SetupNexthopIdRouteEntry(gVrfName, swss_ipv4_route_prefix, kNexthopId1, kNexthopOid1);
    auto route_entry = GenerateP4RouteEntry(gVrfName, swss_ipv4_route_prefix, "", "", kMetadata1);
    EXPECT_EQ(StatusCode::SWSS_RC_INVALID_PARAM, ValidateRouteEntry(route_entry, DEL_COMMAND));
}

TEST_F(RouteManagerTest,
       ValidateDelRouteEntryMulticastNotInMapperShouldRaiseCriticalState) {
  auto swss_ipv6_route_prefix = swss::IpPrefix(kIpv6Prefix3);
  auto route_entry_ipv6 = SetupSetMulticastGroupIdRouteEntry(
      gVrfName, swss_ipv6_route_prefix, kMulticastGroupId1, kMulticastGroupOid1,
      kMetadata1);

  // First, check that extraneous multicast_group_id causes an error.
  route_entry_ipv6.action = "";
  route_entry_ipv6.route_metadata = "";
  route_entry_ipv6.multicast_group_id = kMulticastGroupId1;
  EXPECT_EQ(StatusCode::SWSS_RC_INVALID_PARAM,
            ValidateRouteEntry(route_entry_ipv6, DEL_COMMAND));

  // Now force error during validation by deleting from the map.
  p4_oid_mapper_.eraseOID(SAI_OBJECT_TYPE_IPMC_ENTRY,
                          route_entry_ipv6.route_entry_key);

  EXPECT_EQ(StatusCode::SWSS_RC_INTERNAL,
            ValidateRouteEntry(route_entry_ipv6, DEL_COMMAND));
}

TEST_F(RouteManagerTest, ValidateDelRouteEntrySucceeds)
{
    auto swss_ipv4_route_prefix = swss::IpPrefix(kIpv4Prefix);
    SetupNexthopIdRouteEntry(gVrfName, swss_ipv4_route_prefix, kNexthopId1, kNexthopOid1);
    auto route_entry = GenerateP4RouteEntry(gVrfName, swss_ipv4_route_prefix, "", "");
    EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS, ValidateRouteEntry(route_entry, DEL_COMMAND));
}

TEST_F(RouteManagerTest, CreateRouteEntryWithSaiErrorShouldFail)
{
    auto swss_ipv4_route_prefix = swss::IpPrefix(kIpv4Prefix);
    auto route_entry = GenerateP4RouteEntry(gVrfName, swss_ipv4_route_prefix, p4orch::kDrop, "");

    std::vector<sai_status_t> exp_status{SAI_STATUS_FAILURE};
    EXPECT_CALL(mock_sai_route_, create_route_entries(_, _, _, _, _, _))
        .Times(3)
        .WillRepeatedly(DoAll(SetArrayArgument<5>(exp_status.begin(), exp_status.end()), Return(SAI_STATUS_FAILURE)));
    EXPECT_THAT(CreateRouteEntries(std::vector<P4RouteEntry>{route_entry}),
                ArrayEq(std::vector<StatusCode>{StatusCode::SWSS_RC_UNKNOWN}));

    route_entry.action = p4orch::kSetNexthopId;
    route_entry.nexthop_id = kNexthopId1;
    p4_oid_mapper_.setOID(SAI_OBJECT_TYPE_NEXT_HOP, KeyGenerator::generateNextHopKey(route_entry.nexthop_id),
                          kNexthopOid1);
    EXPECT_THAT(CreateRouteEntries(std::vector<P4RouteEntry>{route_entry}),
                ArrayEq(std::vector<StatusCode>{StatusCode::SWSS_RC_UNKNOWN}));

    route_entry.action = p4orch::kSetWcmpGroupId;
    route_entry.wcmp_group = kWcmpGroup1;
    p4_oid_mapper_.setOID(SAI_OBJECT_TYPE_NEXT_HOP_GROUP, KeyGenerator::generateWcmpGroupKey(route_entry.wcmp_group),
                          kWcmpGroupOid1);
    EXPECT_THAT(CreateRouteEntries(std::vector<P4RouteEntry>{route_entry}),
                ArrayEq(std::vector<StatusCode>{StatusCode::SWSS_RC_UNKNOWN}));
}

TEST_F(RouteManagerTest, CreateNexthopIdIpv4RouteSucceeds)
{
    auto swss_ipv4_route_prefix = swss::IpPrefix(kIpv4Prefix);
    sai_ip_prefix_t sai_ipv4_route_prefix;
    copy(sai_ipv4_route_prefix, swss_ipv4_route_prefix);
    auto route_entry = GenerateP4RouteEntry(gVrfName, swss_ipv4_route_prefix, p4orch::kSetNexthopId, kNexthopId1);
    p4_oid_mapper_.setOID(SAI_OBJECT_TYPE_NEXT_HOP, KeyGenerator::generateNextHopKey(route_entry.nexthop_id),
                          kNexthopOid1);

    sai_route_entry_t exp_sai_route_entry;
    exp_sai_route_entry.switch_id = gSwitchId;
    exp_sai_route_entry.vr_id = gVrfOid;
    exp_sai_route_entry.destination = sai_ipv4_route_prefix;

    sai_attribute_t exp_sai_attr;
    exp_sai_attr.id = SAI_ROUTE_ENTRY_ATTR_NEXT_HOP_ID;
    exp_sai_attr.value.oid = kNexthopOid1;

    std::vector<sai_status_t> exp_status{SAI_STATUS_SUCCESS};
    EXPECT_CALL(mock_sai_route_,
                create_route_entries(Eq(1), RouteEntryArrayEq(std::vector<sai_route_entry_t>{exp_sai_route_entry}),
                                     ArrayEq(std::vector<uint32_t>{1}),
                                     AttrArrayArrayEq(std::vector<std::vector<sai_attribute_t>>{{exp_sai_attr}}),
                                     Eq(SAI_BULK_OP_ERROR_MODE_IGNORE_ERROR), _))
        .WillOnce(DoAll(SetArrayArgument<5>(exp_status.begin(), exp_status.end()), Return(SAI_STATUS_SUCCESS)));
    EXPECT_THAT(CreateRouteEntries(std::vector<P4RouteEntry>{route_entry}),
                ArrayEq(std::vector<StatusCode>{StatusCode::SWSS_RC_SUCCESS}));
    VerifyRouteEntry(route_entry, sai_ipv4_route_prefix, gVrfOid);
    uint32_t ref_cnt;
    EXPECT_TRUE(
        p4_oid_mapper_.getRefCount(SAI_OBJECT_TYPE_NEXT_HOP, KeyGenerator::generateNextHopKey(kNexthopId1), &ref_cnt));
    EXPECT_EQ(1, ref_cnt);
}

TEST_F(RouteManagerTest, CreateNexthopIdIpv6RouteSucceeds)
{
    auto swss_ipv6_route_prefix = swss::IpPrefix(kIpv6Prefix);
    sai_ip_prefix_t sai_ipv6_route_prefix;
    copy(sai_ipv6_route_prefix, swss_ipv6_route_prefix);
    auto route_entry = GenerateP4RouteEntry(gVrfName, swss_ipv6_route_prefix, p4orch::kSetNexthopId, kNexthopId1);
    p4_oid_mapper_.setOID(SAI_OBJECT_TYPE_NEXT_HOP, KeyGenerator::generateNextHopKey(route_entry.nexthop_id),
                          kNexthopOid1);

    sai_route_entry_t exp_sai_route_entry;
    exp_sai_route_entry.switch_id = gSwitchId;
    exp_sai_route_entry.vr_id = gVrfOid;
    exp_sai_route_entry.destination = sai_ipv6_route_prefix;

    sai_attribute_t exp_sai_attr;
    exp_sai_attr.id = SAI_ROUTE_ENTRY_ATTR_NEXT_HOP_ID;
    exp_sai_attr.value.oid = kNexthopOid1;

    std::vector<sai_status_t> exp_status{SAI_STATUS_SUCCESS};
    EXPECT_CALL(mock_sai_route_,
                create_route_entries(Eq(1), RouteEntryArrayEq(std::vector<sai_route_entry_t>{exp_sai_route_entry}),
                                     ArrayEq(std::vector<uint32_t>{1}),
                                     AttrArrayArrayEq(std::vector<std::vector<sai_attribute_t>>{{exp_sai_attr}}),
                                     Eq(SAI_BULK_OP_ERROR_MODE_IGNORE_ERROR), _))
        .WillOnce(DoAll(SetArrayArgument<5>(exp_status.begin(), exp_status.end()), Return(SAI_STATUS_SUCCESS)));
    EXPECT_THAT(CreateRouteEntries(std::vector<P4RouteEntry>{route_entry}),
                ArrayEq(std::vector<StatusCode>{StatusCode::SWSS_RC_SUCCESS}));
    VerifyRouteEntry(route_entry, sai_ipv6_route_prefix, gVrfOid);
    uint32_t ref_cnt;
    EXPECT_TRUE(
        p4_oid_mapper_.getRefCount(SAI_OBJECT_TYPE_NEXT_HOP, KeyGenerator::generateNextHopKey(kNexthopId1), &ref_cnt));
    EXPECT_EQ(1, ref_cnt);
}

TEST_F(RouteManagerTest, CreateNexthopIdWithMetadataIpv4RouteSucceeds)
{
    auto swss_ipv4_route_prefix = swss::IpPrefix(kIpv4Prefix);
    sai_ip_prefix_t sai_ipv4_route_prefix;
    copy(sai_ipv4_route_prefix, swss_ipv4_route_prefix);
    auto route_entry = GenerateP4RouteEntry(gVrfName, swss_ipv4_route_prefix, p4orch::kSetNexthopIdAndMetadata,
                                            kNexthopId1, kMetadata1);
    p4_oid_mapper_.setOID(SAI_OBJECT_TYPE_NEXT_HOP, KeyGenerator::generateNextHopKey(route_entry.nexthop_id),
                          kNexthopOid1);

    sai_route_entry_t exp_sai_route_entry;
    exp_sai_route_entry.switch_id = gSwitchId;
    exp_sai_route_entry.vr_id = gVrfOid;
    exp_sai_route_entry.destination = sai_ipv4_route_prefix;

    std::vector<sai_attribute_t> exp_sai_attrs;
    sai_attribute_t exp_sai_attr;
    exp_sai_attr.id = SAI_ROUTE_ENTRY_ATTR_NEXT_HOP_ID;
    exp_sai_attr.value.oid = kNexthopOid1;
    exp_sai_attrs.push_back(exp_sai_attr);
    exp_sai_attr.id = SAI_ROUTE_ENTRY_ATTR_META_DATA;
    exp_sai_attr.value.u32 = kMetadataInt1;
    exp_sai_attrs.push_back(exp_sai_attr);

    std::vector<sai_status_t> exp_status{SAI_STATUS_SUCCESS};
    EXPECT_CALL(mock_sai_route_,
                create_route_entries(Eq(1), RouteEntryArrayEq(std::vector<sai_route_entry_t>{exp_sai_route_entry}),
                                     ArrayEq(std::vector<uint32_t>{static_cast<uint32_t>(exp_sai_attrs.size())}),
                                     AttrArrayArrayEq(std::vector<std::vector<sai_attribute_t>>{exp_sai_attrs}),
                                     Eq(SAI_BULK_OP_ERROR_MODE_IGNORE_ERROR), _))
        .WillOnce(DoAll(SetArrayArgument<5>(exp_status.begin(), exp_status.end()), Return(SAI_STATUS_SUCCESS)));
    EXPECT_THAT(CreateRouteEntries(std::vector<P4RouteEntry>{route_entry}),
                ArrayEq(std::vector<StatusCode>{StatusCode::SWSS_RC_SUCCESS}));
    VerifyRouteEntry(route_entry, sai_ipv4_route_prefix, gVrfOid);
    uint32_t ref_cnt;
    EXPECT_TRUE(
        p4_oid_mapper_.getRefCount(SAI_OBJECT_TYPE_NEXT_HOP, KeyGenerator::generateNextHopKey(kNexthopId1), &ref_cnt));
    EXPECT_EQ(1, ref_cnt);
}

TEST_F(RouteManagerTest, CreateDropSetMetadataRouteSucceeds)
{
    auto swss_ipv4_route_prefix = swss::IpPrefix(kIpv4Prefix);
    sai_ip_prefix_t sai_ipv4_route_prefix;
    copy(sai_ipv4_route_prefix, swss_ipv4_route_prefix);
    auto route_entry =
        GenerateP4RouteEntry(gVrfName, swss_ipv4_route_prefix, p4orch::kSetMetadataAndDrop, "", kMetadata1);

    sai_route_entry_t exp_sai_route_entry;
    exp_sai_route_entry.switch_id = gSwitchId;
    exp_sai_route_entry.vr_id = gVrfOid;
    exp_sai_route_entry.destination = sai_ipv4_route_prefix;

    std::vector<sai_attribute_t> exp_sai_attrs;
    sai_attribute_t exp_sai_attr;
    exp_sai_attr.id = SAI_ROUTE_ENTRY_ATTR_PACKET_ACTION;
    exp_sai_attr.value.s32 = SAI_PACKET_ACTION_DROP;
    exp_sai_attrs.push_back(exp_sai_attr);
    exp_sai_attr.id = SAI_ROUTE_ENTRY_ATTR_META_DATA;
    exp_sai_attr.value.u32 = kMetadataInt1;
    exp_sai_attrs.push_back(exp_sai_attr);

    std::vector<sai_status_t> exp_status{SAI_STATUS_SUCCESS};
    EXPECT_CALL(mock_sai_route_,
                create_route_entries(Eq(1), RouteEntryArrayEq(std::vector<sai_route_entry_t>{exp_sai_route_entry}),
                                     ArrayEq(std::vector<uint32_t>{static_cast<uint32_t>(exp_sai_attrs.size())}),
                                     AttrArrayArrayEq(std::vector<std::vector<sai_attribute_t>>{exp_sai_attrs}),
                                     Eq(SAI_BULK_OP_ERROR_MODE_IGNORE_ERROR), _))
        .WillOnce(DoAll(SetArrayArgument<5>(exp_status.begin(), exp_status.end()), Return(SAI_STATUS_SUCCESS)));

    EXPECT_THAT(CreateRouteEntries(std::vector<P4RouteEntry>{route_entry}),
                ArrayEq(std::vector<StatusCode>{StatusCode::SWSS_RC_SUCCESS}));
    VerifyRouteEntry(route_entry, sai_ipv4_route_prefix, gVrfOid);
}

TEST_F(RouteManagerTest, CreateDropIpv4RouteSucceeds)
{
    auto swss_ipv4_route_prefix = swss::IpPrefix(kIpv4Prefix);
    sai_ip_prefix_t sai_ipv4_route_prefix;
    copy(sai_ipv4_route_prefix, swss_ipv4_route_prefix);
    auto route_entry = GenerateP4RouteEntry(gVrfName, swss_ipv4_route_prefix, p4orch::kDrop, "");

    sai_route_entry_t exp_sai_route_entry;
    exp_sai_route_entry.switch_id = gSwitchId;
    exp_sai_route_entry.vr_id = gVrfOid;
    exp_sai_route_entry.destination = sai_ipv4_route_prefix;

    sai_attribute_t exp_sai_attr;
    exp_sai_attr.id = SAI_ROUTE_ENTRY_ATTR_PACKET_ACTION;
    exp_sai_attr.value.s32 = SAI_PACKET_ACTION_DROP;

    std::vector<sai_status_t> exp_status{SAI_STATUS_SUCCESS};
    EXPECT_CALL(mock_sai_route_,
                create_route_entries(Eq(1), RouteEntryArrayEq(std::vector<sai_route_entry_t>{exp_sai_route_entry}),
                                     ArrayEq(std::vector<uint32_t>{1}),
                                     AttrArrayArrayEq(std::vector<std::vector<sai_attribute_t>>{{exp_sai_attr}}),
                                     Eq(SAI_BULK_OP_ERROR_MODE_IGNORE_ERROR), _))
        .WillOnce(DoAll(SetArrayArgument<5>(exp_status.begin(), exp_status.end()), Return(SAI_STATUS_SUCCESS)));
    EXPECT_THAT(CreateRouteEntries(std::vector<P4RouteEntry>{route_entry}),
                ArrayEq(std::vector<StatusCode>{StatusCode::SWSS_RC_SUCCESS}));
    VerifyRouteEntry(route_entry, sai_ipv4_route_prefix, gVrfOid);
}

TEST_F(RouteManagerTest, CreateDropIpv6RouteSucceeds)
{
    auto swss_ipv6_route_prefix = swss::IpPrefix(kIpv6Prefix);
    sai_ip_prefix_t sai_ipv6_route_prefix;
    copy(sai_ipv6_route_prefix, swss_ipv6_route_prefix);
    auto route_entry = GenerateP4RouteEntry(gVrfName, swss_ipv6_route_prefix, p4orch::kDrop, "");

    sai_route_entry_t exp_sai_route_entry;
    exp_sai_route_entry.switch_id = gSwitchId;
    exp_sai_route_entry.vr_id = gVrfOid;
    exp_sai_route_entry.destination = sai_ipv6_route_prefix;

    sai_attribute_t exp_sai_attr;
    exp_sai_attr.id = SAI_ROUTE_ENTRY_ATTR_PACKET_ACTION;
    exp_sai_attr.value.s32 = SAI_PACKET_ACTION_DROP;

    std::vector<sai_status_t> exp_status{SAI_STATUS_SUCCESS};
    EXPECT_CALL(mock_sai_route_,
                create_route_entries(Eq(1), RouteEntryArrayEq(std::vector<sai_route_entry_t>{exp_sai_route_entry}),
                                     ArrayEq(std::vector<uint32_t>{1}),
                                     AttrArrayArrayEq(std::vector<std::vector<sai_attribute_t>>{{exp_sai_attr}}),
                                     Eq(SAI_BULK_OP_ERROR_MODE_IGNORE_ERROR), _))
        .WillOnce(DoAll(SetArrayArgument<5>(exp_status.begin(), exp_status.end()), Return(SAI_STATUS_SUCCESS)));
    EXPECT_THAT(CreateRouteEntries(std::vector<P4RouteEntry>{route_entry}),
                ArrayEq(std::vector<StatusCode>{StatusCode::SWSS_RC_SUCCESS}));
    VerifyRouteEntry(route_entry, sai_ipv6_route_prefix, gVrfOid);
}

TEST_F(RouteManagerTest, CreateTrapIpv4RouteSucceeds)
{
    auto swss_ipv4_route_prefix = swss::IpPrefix(kIpv4Prefix);
    sai_ip_prefix_t sai_ipv4_route_prefix;
    copy(sai_ipv4_route_prefix, swss_ipv4_route_prefix);
    auto route_entry = GenerateP4RouteEntry(gVrfName, swss_ipv4_route_prefix, p4orch::kTrap, "");

    sai_route_entry_t exp_sai_route_entry;
    exp_sai_route_entry.switch_id = gSwitchId;
    exp_sai_route_entry.vr_id = gVrfOid;
    exp_sai_route_entry.destination = sai_ipv4_route_prefix;

    sai_attribute_t exp_sai_attr;
    exp_sai_attr.id = SAI_ROUTE_ENTRY_ATTR_PACKET_ACTION;
    exp_sai_attr.value.s32 = SAI_PACKET_ACTION_TRAP;

    std::vector<sai_status_t> exp_status{SAI_STATUS_SUCCESS};
    EXPECT_CALL(mock_sai_route_,
                create_route_entries(Eq(1), RouteEntryArrayEq(std::vector<sai_route_entry_t>{exp_sai_route_entry}),
                                     ArrayEq(std::vector<uint32_t>{1}),
                                     AttrArrayArrayEq(std::vector<std::vector<sai_attribute_t>>{{exp_sai_attr}}),
                                     Eq(SAI_BULK_OP_ERROR_MODE_IGNORE_ERROR), _))
        .WillOnce(DoAll(SetArrayArgument<5>(exp_status.begin(), exp_status.end()), Return(SAI_STATUS_SUCCESS)));
    EXPECT_THAT(CreateRouteEntries(std::vector<P4RouteEntry>{route_entry}),
                ArrayEq(std::vector<StatusCode>{StatusCode::SWSS_RC_SUCCESS}));
    VerifyRouteEntry(route_entry, sai_ipv4_route_prefix, gVrfOid);
}

TEST_F(RouteManagerTest, CreateTrapIpv6RouteSucceeds)
{
    auto swss_ipv6_route_prefix = swss::IpPrefix(kIpv6Prefix);
    sai_ip_prefix_t sai_ipv6_route_prefix;
    copy(sai_ipv6_route_prefix, swss_ipv6_route_prefix);
    auto route_entry = GenerateP4RouteEntry(gVrfName, swss_ipv6_route_prefix, p4orch::kTrap, "");

    sai_route_entry_t exp_sai_route_entry;
    exp_sai_route_entry.switch_id = gSwitchId;
    exp_sai_route_entry.vr_id = gVrfOid;
    exp_sai_route_entry.destination = sai_ipv6_route_prefix;

    sai_attribute_t exp_sai_attr;
    exp_sai_attr.id = SAI_ROUTE_ENTRY_ATTR_PACKET_ACTION;
    exp_sai_attr.value.s32 = SAI_PACKET_ACTION_TRAP;

    std::vector<sai_status_t> exp_status{SAI_STATUS_SUCCESS};
    EXPECT_CALL(mock_sai_route_,
                create_route_entries(Eq(1), RouteEntryArrayEq(std::vector<sai_route_entry_t>{exp_sai_route_entry}),
                                     ArrayEq(std::vector<uint32_t>{1}),
                                     AttrArrayArrayEq(std::vector<std::vector<sai_attribute_t>>{{exp_sai_attr}}),
                                     Eq(SAI_BULK_OP_ERROR_MODE_IGNORE_ERROR), _))
        .WillOnce(DoAll(SetArrayArgument<5>(exp_status.begin(), exp_status.end()), Return(SAI_STATUS_SUCCESS)));
    EXPECT_THAT(CreateRouteEntries(std::vector<P4RouteEntry>{route_entry}),
                ArrayEq(std::vector<StatusCode>{StatusCode::SWSS_RC_SUCCESS}));
    VerifyRouteEntry(route_entry, sai_ipv6_route_prefix, gVrfOid);
}

TEST_F(RouteManagerTest, CreateWcmpIpv4RouteSucceeds)
{
    auto swss_ipv4_route_prefix = swss::IpPrefix(kIpv4Prefix);
    sai_ip_prefix_t sai_ipv4_route_prefix;
    copy(sai_ipv4_route_prefix, swss_ipv4_route_prefix);
    auto route_entry = GenerateP4RouteEntry(gVrfName, swss_ipv4_route_prefix, p4orch::kSetWcmpGroupId, kWcmpGroup1);
    p4_oid_mapper_.setOID(SAI_OBJECT_TYPE_NEXT_HOP_GROUP, KeyGenerator::generateWcmpGroupKey(route_entry.wcmp_group),
                          kWcmpGroupOid1);

    sai_route_entry_t exp_sai_route_entry;
    exp_sai_route_entry.switch_id = gSwitchId;
    exp_sai_route_entry.vr_id = gVrfOid;
    exp_sai_route_entry.destination = sai_ipv4_route_prefix;

    sai_attribute_t exp_sai_attr;
    exp_sai_attr.id = SAI_ROUTE_ENTRY_ATTR_NEXT_HOP_ID;
    exp_sai_attr.value.oid = kWcmpGroupOid1;

    std::vector<sai_status_t> exp_status{SAI_STATUS_SUCCESS};
    EXPECT_CALL(mock_sai_route_,
                create_route_entries(Eq(1), RouteEntryArrayEq(std::vector<sai_route_entry_t>{exp_sai_route_entry}),
                                     ArrayEq(std::vector<uint32_t>{1}),
                                     AttrArrayArrayEq(std::vector<std::vector<sai_attribute_t>>{{exp_sai_attr}}),
                                     Eq(SAI_BULK_OP_ERROR_MODE_IGNORE_ERROR), _))
        .WillOnce(DoAll(SetArrayArgument<5>(exp_status.begin(), exp_status.end()), Return(SAI_STATUS_SUCCESS)));
    EXPECT_THAT(CreateRouteEntries(std::vector<P4RouteEntry>{route_entry}),
                ArrayEq(std::vector<StatusCode>{StatusCode::SWSS_RC_SUCCESS}));
    VerifyRouteEntry(route_entry, sai_ipv4_route_prefix, gVrfOid);
    uint32_t ref_cnt;
    EXPECT_TRUE(p4_oid_mapper_.getRefCount(SAI_OBJECT_TYPE_NEXT_HOP_GROUP,
                                           KeyGenerator::generateWcmpGroupKey(kWcmpGroup1), &ref_cnt));
    EXPECT_EQ(1, ref_cnt);
}

TEST_F(RouteManagerTest, CreateWcmpIpv6RouteSucceeds)
{
    auto swss_ipv6_route_prefix = swss::IpPrefix(kIpv6Prefix);
    sai_ip_prefix_t sai_ipv6_route_prefix;
    copy(sai_ipv6_route_prefix, swss_ipv6_route_prefix);
    auto route_entry = GenerateP4RouteEntry(gVrfName, swss_ipv6_route_prefix, p4orch::kSetWcmpGroupId, kWcmpGroup1);
    p4_oid_mapper_.setOID(SAI_OBJECT_TYPE_NEXT_HOP_GROUP, KeyGenerator::generateWcmpGroupKey(route_entry.wcmp_group),
                          kWcmpGroupOid1);

    sai_route_entry_t exp_sai_route_entry;
    exp_sai_route_entry.switch_id = gSwitchId;
    exp_sai_route_entry.vr_id = gVrfOid;
    exp_sai_route_entry.destination = sai_ipv6_route_prefix;

    sai_attribute_t exp_sai_attr;
    exp_sai_attr.id = SAI_ROUTE_ENTRY_ATTR_NEXT_HOP_ID;
    exp_sai_attr.value.oid = kWcmpGroupOid1;

    std::vector<sai_status_t> exp_status{SAI_STATUS_SUCCESS};
    EXPECT_CALL(mock_sai_route_,
                create_route_entries(Eq(1), RouteEntryArrayEq(std::vector<sai_route_entry_t>{exp_sai_route_entry}),
                                     ArrayEq(std::vector<uint32_t>{1}),
                                     AttrArrayArrayEq(std::vector<std::vector<sai_attribute_t>>{{exp_sai_attr}}),
                                     Eq(SAI_BULK_OP_ERROR_MODE_IGNORE_ERROR), _))
        .WillOnce(DoAll(SetArrayArgument<5>(exp_status.begin(), exp_status.end()), Return(SAI_STATUS_SUCCESS)));
    EXPECT_THAT(CreateRouteEntries(std::vector<P4RouteEntry>{route_entry}),
                ArrayEq(std::vector<StatusCode>{StatusCode::SWSS_RC_SUCCESS}));
    VerifyRouteEntry(route_entry, sai_ipv6_route_prefix, gVrfOid);
    uint32_t ref_cnt;
    EXPECT_TRUE(p4_oid_mapper_.getRefCount(SAI_OBJECT_TYPE_NEXT_HOP_GROUP,
                                           KeyGenerator::generateWcmpGroupKey(kWcmpGroup1), &ref_cnt));
    EXPECT_EQ(1, ref_cnt);
}

TEST_F(RouteManagerTest, CreateWcmpWithMetadataIpv6RouteSucceeds)
{
    auto swss_ipv6_route_prefix = swss::IpPrefix(kIpv6Prefix);
    sai_ip_prefix_t sai_ipv6_route_prefix;
    copy(sai_ipv6_route_prefix, swss_ipv6_route_prefix);
    auto route_entry = GenerateP4RouteEntry(gVrfName, swss_ipv6_route_prefix, p4orch::kSetWcmpGroupIdAndMetadata,
                                            kWcmpGroup1, kMetadata1);
    p4_oid_mapper_.setOID(SAI_OBJECT_TYPE_NEXT_HOP_GROUP, KeyGenerator::generateWcmpGroupKey(route_entry.wcmp_group),
                          kWcmpGroupOid1);

    sai_route_entry_t exp_sai_route_entry;
    exp_sai_route_entry.switch_id = gSwitchId;
    exp_sai_route_entry.vr_id = gVrfOid;
    exp_sai_route_entry.destination = sai_ipv6_route_prefix;

    std::vector<sai_attribute_t> exp_sai_attrs;
    sai_attribute_t exp_sai_attr;
    exp_sai_attr.id = SAI_ROUTE_ENTRY_ATTR_NEXT_HOP_ID;
    exp_sai_attr.value.oid = kWcmpGroupOid1;
    exp_sai_attrs.push_back(exp_sai_attr);
    exp_sai_attr.id = SAI_ROUTE_ENTRY_ATTR_META_DATA;
    exp_sai_attr.value.u32 = kMetadataInt1;
    exp_sai_attrs.push_back(exp_sai_attr);

    std::vector<sai_status_t> exp_status{SAI_STATUS_SUCCESS};
    EXPECT_CALL(mock_sai_route_,
                create_route_entries(Eq(1), RouteEntryArrayEq(std::vector<sai_route_entry_t>{exp_sai_route_entry}),
                                     ArrayEq(std::vector<uint32_t>{static_cast<uint32_t>(exp_sai_attrs.size())}),
                                     AttrArrayArrayEq(std::vector<std::vector<sai_attribute_t>>{exp_sai_attrs}),
                                     Eq(SAI_BULK_OP_ERROR_MODE_IGNORE_ERROR), _))
        .WillOnce(DoAll(SetArrayArgument<5>(exp_status.begin(), exp_status.end()), Return(SAI_STATUS_SUCCESS)));
    EXPECT_THAT(CreateRouteEntries(std::vector<P4RouteEntry>{route_entry}),
                ArrayEq(std::vector<StatusCode>{StatusCode::SWSS_RC_SUCCESS}));
    VerifyRouteEntry(route_entry, sai_ipv6_route_prefix, gVrfOid);
    uint32_t ref_cnt;
    EXPECT_TRUE(p4_oid_mapper_.getRefCount(SAI_OBJECT_TYPE_NEXT_HOP_GROUP,
                                           KeyGenerator::generateWcmpGroupKey(kWcmpGroup1), &ref_cnt));
    EXPECT_EQ(1, ref_cnt);
}

TEST_F(RouteManagerTest, UpdateRouteEntryWcmpWithSaiErrorShouldFail)
{
    auto swss_ipv4_route_prefix = swss::IpPrefix(kIpv4Prefix);
    sai_ip_prefix_t sai_ipv4_route_prefix;
    copy(sai_ipv4_route_prefix, swss_ipv4_route_prefix);
    SetupWcmpGroupRouteEntry(gVrfName, swss_ipv4_route_prefix, kWcmpGroup1, kWcmpGroupOid1);

    auto route_entry = GenerateP4RouteEntry(gVrfName, swss_ipv4_route_prefix, p4orch::kSetWcmpGroupId, kWcmpGroup2);
    p4_oid_mapper_.setOID(SAI_OBJECT_TYPE_NEXT_HOP_GROUP, KeyGenerator::generateWcmpGroupKey(route_entry.wcmp_group),
                          kWcmpGroupOid2);
    std::vector<sai_status_t> exp_status{SAI_STATUS_FAILURE};
    EXPECT_CALL(mock_sai_route_, set_route_entries_attribute(_, _, _, _, _))
        .WillOnce(DoAll(SetArrayArgument<4>(exp_status.begin(), exp_status.end()), Return(SAI_STATUS_FAILURE)));
    EXPECT_THAT(UpdateRouteEntries(std::vector<P4RouteEntry>{route_entry}),
                ArrayEq(std::vector<StatusCode>{StatusCode::SWSS_RC_UNKNOWN}));
}

TEST_F(RouteManagerTest, UpdateRouteEntryWcmpNotExistInMapperShouldRaiseCriticalState)
{
    auto swss_ipv4_route_prefix = swss::IpPrefix(kIpv4Prefix);
    sai_ip_prefix_t sai_ipv4_route_prefix;
    copy(sai_ipv4_route_prefix, swss_ipv4_route_prefix);
    SetupWcmpGroupRouteEntry(gVrfName, swss_ipv4_route_prefix, kWcmpGroup1, kWcmpGroupOid1);

    auto route_entry = GenerateP4RouteEntry(gVrfName, swss_ipv4_route_prefix, p4orch::kSetWcmpGroupId, kWcmpGroup2);
    std::vector<sai_status_t> exp_status{SAI_STATUS_FAILURE};
    EXPECT_CALL(mock_sai_route_, set_route_entries_attribute(_, _, _, _, _))
        .WillOnce(DoAll(SetArrayArgument<4>(exp_status.begin(), exp_status.end()), Return(SAI_STATUS_FAILURE)));
    // TODO: Expect critical state.
    EXPECT_THAT(UpdateRouteEntries(std::vector<P4RouteEntry>{route_entry}),
                ArrayEq(std::vector<StatusCode>{StatusCode::SWSS_RC_UNKNOWN}));
}

TEST_F(RouteManagerTest, UpdateRouteFromSetWcmpToSetNextHopSucceeds)
{
    auto swss_ipv4_route_prefix = swss::IpPrefix(kIpv4Prefix);
    sai_ip_prefix_t sai_ipv4_route_prefix;
    copy(sai_ipv4_route_prefix, swss_ipv4_route_prefix);
    SetupWcmpGroupRouteEntry(gVrfName, swss_ipv4_route_prefix, kWcmpGroup1, kWcmpGroupOid1);

    auto route_entry = GenerateP4RouteEntry(gVrfName, swss_ipv4_route_prefix, p4orch::kSetNexthopId, kNexthopId2);
    p4_oid_mapper_.setOID(SAI_OBJECT_TYPE_NEXT_HOP, KeyGenerator::generateNextHopKey(route_entry.nexthop_id),
                          kNexthopOid2);

    sai_route_entry_t exp_sai_route_entry;
    exp_sai_route_entry.switch_id = gSwitchId;
    exp_sai_route_entry.vr_id = gVrfOid;
    exp_sai_route_entry.destination = sai_ipv4_route_prefix;

    sai_attribute_t exp_sai_attr;
    exp_sai_attr.id = SAI_ROUTE_ENTRY_ATTR_NEXT_HOP_ID;
    exp_sai_attr.value.oid = kNexthopOid2;

    std::vector<sai_status_t> exp_status{SAI_STATUS_SUCCESS};
    EXPECT_CALL(mock_sai_route_, set_route_entries_attribute(
                                     Eq(1), RouteEntryArrayEq(std::vector<sai_route_entry_t>{exp_sai_route_entry}),
                                     AttrArrayEq(std::vector<sai_attribute_t>{exp_sai_attr}),
                                     Eq(SAI_BULK_OP_ERROR_MODE_IGNORE_ERROR), _))
        .WillOnce(DoAll(SetArrayArgument<4>(exp_status.begin(), exp_status.end()), Return(SAI_STATUS_SUCCESS)));
    EXPECT_THAT(UpdateRouteEntries(std::vector<P4RouteEntry>{route_entry}),
                ArrayEq(std::vector<StatusCode>{StatusCode::SWSS_RC_SUCCESS}));
    VerifyRouteEntry(route_entry, sai_ipv4_route_prefix, gVrfOid);
    uint32_t ref_cnt;
    EXPECT_TRUE(p4_oid_mapper_.getRefCount(SAI_OBJECT_TYPE_NEXT_HOP_GROUP,
                                           KeyGenerator::generateWcmpGroupKey(kWcmpGroup1), &ref_cnt));
    EXPECT_EQ(0, ref_cnt);
    EXPECT_TRUE(
        p4_oid_mapper_.getRefCount(SAI_OBJECT_TYPE_NEXT_HOP, KeyGenerator::generateNextHopKey(kNexthopId2), &ref_cnt));
    EXPECT_EQ(1, ref_cnt);
}

TEST_F(RouteManagerTest, UpdateRouteFromSetWcmpToSetNextHopAndMetadataSucceeds)
{
    auto swss_ipv4_route_prefix = swss::IpPrefix(kIpv4Prefix);
    sai_ip_prefix_t sai_ipv4_route_prefix;
    copy(sai_ipv4_route_prefix, swss_ipv4_route_prefix);
    SetupWcmpGroupRouteEntry(gVrfName, swss_ipv4_route_prefix, kWcmpGroup1, kWcmpGroupOid1);

    auto route_entry = GenerateP4RouteEntry(gVrfName, swss_ipv4_route_prefix, p4orch::kSetNexthopIdAndMetadata,
                                            kNexthopId2, kMetadata2);
    p4_oid_mapper_.setOID(SAI_OBJECT_TYPE_NEXT_HOP, KeyGenerator::generateNextHopKey(route_entry.nexthop_id),
                          kNexthopOid2);

    sai_route_entry_t exp_sai_route_entry;
    exp_sai_route_entry.switch_id = gSwitchId;
    exp_sai_route_entry.vr_id = gVrfOid;
    exp_sai_route_entry.destination = sai_ipv4_route_prefix;

    sai_attribute_t exp_sai_attr_1;
    exp_sai_attr_1.id = SAI_ROUTE_ENTRY_ATTR_META_DATA;
    exp_sai_attr_1.value.u32 = kMetadataInt2;
    sai_attribute_t exp_sai_attr_2;
    exp_sai_attr_2.id = SAI_ROUTE_ENTRY_ATTR_NEXT_HOP_ID;
    exp_sai_attr_2.value.oid = kNexthopOid2;

    std::vector<sai_status_t> exp_status{SAI_STATUS_SUCCESS,
                                         SAI_STATUS_SUCCESS};

    EXPECT_CALL(mock_sai_route_,
                set_route_entries_attribute(
                    Eq(2),
                    RouteEntryArrayEq(std::vector<sai_route_entry_t>{
                        exp_sai_route_entry, exp_sai_route_entry}),
                    AttrArrayEq(std::vector<sai_attribute_t>{exp_sai_attr_1,
                                                             exp_sai_attr_2}),
                    Eq(SAI_BULK_OP_ERROR_MODE_IGNORE_ERROR), _))
        .WillOnce(
            DoAll(SetArrayArgument<4>(exp_status.begin(), exp_status.end()),
                  Return(SAI_STATUS_SUCCESS)));
    EXPECT_THAT(UpdateRouteEntries(std::vector<P4RouteEntry>{route_entry}),
                ArrayEq(std::vector<StatusCode>{StatusCode::SWSS_RC_SUCCESS}));
    VerifyRouteEntry(route_entry, sai_ipv4_route_prefix, gVrfOid);
    uint32_t ref_cnt;
    EXPECT_TRUE(p4_oid_mapper_.getRefCount(SAI_OBJECT_TYPE_NEXT_HOP_GROUP,
                                           KeyGenerator::generateWcmpGroupKey(kWcmpGroup1), &ref_cnt));
    EXPECT_EQ(0, ref_cnt);
    EXPECT_TRUE(
        p4_oid_mapper_.getRefCount(SAI_OBJECT_TYPE_NEXT_HOP, KeyGenerator::generateNextHopKey(kNexthopId2), &ref_cnt));
    EXPECT_EQ(1, ref_cnt);
}

TEST_F(RouteManagerTest, UpdateRouteFromSetNexthopIdToSetWcmpSucceeds)
{
    auto swss_ipv4_route_prefix = swss::IpPrefix(kIpv4Prefix);
    sai_ip_prefix_t sai_ipv4_route_prefix;
    copy(sai_ipv4_route_prefix, swss_ipv4_route_prefix);
    SetupNexthopIdRouteEntry(gVrfName, swss_ipv4_route_prefix, kNexthopId1, kNexthopOid1);

    auto route_entry = GenerateP4RouteEntry(gVrfName, swss_ipv4_route_prefix, p4orch::kSetWcmpGroupId, kWcmpGroup2);
    p4_oid_mapper_.setOID(SAI_OBJECT_TYPE_NEXT_HOP_GROUP, KeyGenerator::generateWcmpGroupKey(route_entry.wcmp_group),
                          kWcmpGroupOid2);

    sai_route_entry_t exp_sai_route_entry;
    exp_sai_route_entry.switch_id = gSwitchId;
    exp_sai_route_entry.vr_id = gVrfOid;
    exp_sai_route_entry.destination = sai_ipv4_route_prefix;

    sai_attribute_t exp_sai_attr;
    exp_sai_attr.id = SAI_ROUTE_ENTRY_ATTR_NEXT_HOP_ID;
    exp_sai_attr.value.oid = kWcmpGroupOid2;

    std::vector<sai_status_t> exp_status{SAI_STATUS_SUCCESS};
    EXPECT_CALL(mock_sai_route_, set_route_entries_attribute(
                                     Eq(1), RouteEntryArrayEq(std::vector<sai_route_entry_t>{exp_sai_route_entry}),
                                     AttrArrayEq(std::vector<sai_attribute_t>{exp_sai_attr}),
                                     Eq(SAI_BULK_OP_ERROR_MODE_IGNORE_ERROR), _))
        .WillOnce(DoAll(SetArrayArgument<4>(exp_status.begin(), exp_status.end()), Return(SAI_STATUS_SUCCESS)));
    EXPECT_THAT(UpdateRouteEntries(std::vector<P4RouteEntry>{route_entry}),
                ArrayEq(std::vector<StatusCode>{StatusCode::SWSS_RC_SUCCESS}));
    route_entry.action = p4orch::kSetWcmpGroupId;
    VerifyRouteEntry(route_entry, sai_ipv4_route_prefix, gVrfOid);
    uint32_t ref_cnt;
    EXPECT_TRUE(
        p4_oid_mapper_.getRefCount(SAI_OBJECT_TYPE_NEXT_HOP, KeyGenerator::generateNextHopKey(kNexthopId1), &ref_cnt));
    EXPECT_EQ(0, ref_cnt);
    EXPECT_TRUE(p4_oid_mapper_.getRefCount(SAI_OBJECT_TYPE_NEXT_HOP_GROUP,
                                           KeyGenerator::generateWcmpGroupKey(kWcmpGroup2), &ref_cnt));
    EXPECT_EQ(1, ref_cnt);
}

TEST_F(RouteManagerTest, UpdateRouteFromSetNexthopIdAndMetadataToSetWcmpSucceeds)
{
    auto swss_ipv4_route_prefix = swss::IpPrefix(kIpv4Prefix);
    sai_ip_prefix_t sai_ipv4_route_prefix;
    copy(sai_ipv4_route_prefix, swss_ipv4_route_prefix);
    SetupNexthopIdRouteEntry(gVrfName, swss_ipv4_route_prefix, kNexthopId1, kNexthopOid1, kMetadata2);

    auto route_entry = GenerateP4RouteEntry(gVrfName, swss_ipv4_route_prefix, p4orch::kSetWcmpGroupId, kWcmpGroup2);
    p4_oid_mapper_.setOID(SAI_OBJECT_TYPE_NEXT_HOP_GROUP, KeyGenerator::generateWcmpGroupKey(route_entry.wcmp_group),
                          kWcmpGroupOid2);

    sai_route_entry_t exp_sai_route_entry;
    exp_sai_route_entry.switch_id = gSwitchId;
    exp_sai_route_entry.vr_id = gVrfOid;
    exp_sai_route_entry.destination = sai_ipv4_route_prefix;

    sai_attribute_t exp_sai_attr_1;
    exp_sai_attr_1.id = SAI_ROUTE_ENTRY_ATTR_META_DATA;
    exp_sai_attr_1.value.u32 = 0;
    sai_attribute_t exp_sai_attr_2;
    exp_sai_attr_2.id = SAI_ROUTE_ENTRY_ATTR_NEXT_HOP_ID;
    exp_sai_attr_2.value.oid = kWcmpGroupOid2;

    std::vector<sai_status_t> exp_status{SAI_STATUS_SUCCESS,
                                         SAI_STATUS_SUCCESS};
    EXPECT_CALL(mock_sai_route_,
                set_route_entries_attribute(
                    Eq(2),
                    RouteEntryArrayEq(std::vector<sai_route_entry_t>{
                        exp_sai_route_entry, exp_sai_route_entry}),
                    AttrArrayEq(std::vector<sai_attribute_t>{exp_sai_attr_1,
                                                             exp_sai_attr_2}),
                    Eq(SAI_BULK_OP_ERROR_MODE_IGNORE_ERROR), _))
        .WillOnce(
            DoAll(SetArrayArgument<4>(exp_status.begin(), exp_status.end()),
                  Return(SAI_STATUS_SUCCESS)));
    EXPECT_THAT(UpdateRouteEntries(std::vector<P4RouteEntry>{route_entry}),
                ArrayEq(std::vector<StatusCode>{StatusCode::SWSS_RC_SUCCESS}));
    VerifyRouteEntry(route_entry, sai_ipv4_route_prefix, gVrfOid);
    uint32_t ref_cnt;
    EXPECT_TRUE(
        p4_oid_mapper_.getRefCount(SAI_OBJECT_TYPE_NEXT_HOP, KeyGenerator::generateNextHopKey(kNexthopId1), &ref_cnt));
    EXPECT_EQ(0, ref_cnt);
    EXPECT_TRUE(p4_oid_mapper_.getRefCount(SAI_OBJECT_TYPE_NEXT_HOP_GROUP,
                                           KeyGenerator::generateWcmpGroupKey(kWcmpGroup2), &ref_cnt));
    EXPECT_EQ(1, ref_cnt);
}

TEST_F(RouteManagerTest, UpdateRouteEntryNexthopIdWithSaiErrorShouldFail)
{
    auto swss_ipv4_route_prefix = swss::IpPrefix(kIpv4Prefix);
    SetupNexthopIdRouteEntry(gVrfName, swss_ipv4_route_prefix, kNexthopId1, kNexthopOid1);

    auto route_entry = GenerateP4RouteEntry(gVrfName, swss_ipv4_route_prefix, p4orch::kSetNexthopId, kNexthopId2);
    p4_oid_mapper_.setOID(SAI_OBJECT_TYPE_NEXT_HOP, KeyGenerator::generateNextHopKey(route_entry.nexthop_id),
                          kNexthopOid2);
    std::vector<sai_status_t> exp_failure_status{SAI_STATUS_FAILURE};
    EXPECT_CALL(mock_sai_route_, set_route_entries_attribute(_, _, _, _, _))
        .WillOnce(DoAll(SetArrayArgument<4>(exp_failure_status.begin(), exp_failure_status.end()),
                        Return(SAI_STATUS_FAILURE)));
    EXPECT_THAT(UpdateRouteEntries(std::vector<P4RouteEntry>{route_entry}),
                ArrayEq(std::vector<StatusCode>{StatusCode::SWSS_RC_UNKNOWN}));
}

TEST_F(RouteManagerTest, UpdateRouteEntryNexthopIdNotExistInMapperShouldRaiseCriticalState)
{
    auto swss_ipv4_route_prefix = swss::IpPrefix(kIpv4Prefix);
    SetupNexthopIdRouteEntry(gVrfName, swss_ipv4_route_prefix, kNexthopId1, kNexthopOid1);

    auto route_entry = GenerateP4RouteEntry(gVrfName, swss_ipv4_route_prefix, p4orch::kSetNexthopId, kNexthopId2);
    std::vector<sai_status_t> exp_failure_status{SAI_STATUS_FAILURE};
    EXPECT_CALL(mock_sai_route_, set_route_entries_attribute(_, _, _, _, _))
        .WillOnce(DoAll(SetArrayArgument<4>(exp_failure_status.begin(), exp_failure_status.end()),
                        Return(SAI_STATUS_FAILURE)));
    // TODO: Expect critical state.
    EXPECT_THAT(UpdateRouteEntries(std::vector<P4RouteEntry>{route_entry}),
                ArrayEq(std::vector<StatusCode>{StatusCode::SWSS_RC_UNKNOWN}));
}

TEST_F(RouteManagerTest, UpdateRouteEntryDropWithSaiErrorShouldFail)
{
    auto swss_ipv4_route_prefix = swss::IpPrefix(kIpv4Prefix);
    SetupNexthopIdRouteEntry(gVrfName, swss_ipv4_route_prefix, kNexthopId1, kNexthopOid1);

    auto route_entry = GenerateP4RouteEntry(gVrfName, swss_ipv4_route_prefix, p4orch::kDrop, "");
    std::vector<sai_status_t> exp_status_1{SAI_STATUS_FAILURE,
                                           SAI_STATUS_NOT_EXECUTED};
    std::vector<sai_status_t> exp_status_2{SAI_STATUS_SUCCESS,
                                           SAI_STATUS_FAILURE};
    std::vector<sai_status_t> exp_status_3{SAI_STATUS_SUCCESS};
    std::vector<sai_status_t> exp_status_4{SAI_STATUS_FAILURE};
    EXPECT_CALL(mock_sai_route_, set_route_entries_attribute(Eq(2), _, _, _, _))
        .WillOnce(
            DoAll(SetArrayArgument<4>(exp_status_1.begin(), exp_status_1.end()),
                  Return(SAI_STATUS_FAILURE)));
    EXPECT_THAT(UpdateRouteEntries(std::vector<P4RouteEntry>{route_entry}),
                ArrayEq(std::vector<StatusCode>{StatusCode::SWSS_RC_UNKNOWN}));
    EXPECT_CALL(mock_sai_route_, set_route_entries_attribute(Eq(2), _, _, _, _))
        .WillOnce(
            DoAll(SetArrayArgument<4>(exp_status_2.begin(), exp_status_2.end()),
                  Return(SAI_STATUS_FAILURE)));
    EXPECT_CALL(mock_sai_route_, set_route_entries_attribute(Eq(1), _, _, _, _))
        .WillOnce(
            DoAll(SetArrayArgument<4>(exp_status_3.begin(), exp_status_3.end()),
                  Return(SAI_STATUS_SUCCESS)));

    EXPECT_THAT(UpdateRouteEntries(std::vector<P4RouteEntry>{route_entry}),
                ArrayEq(std::vector<StatusCode>{StatusCode::SWSS_RC_UNKNOWN}));
    EXPECT_CALL(mock_sai_route_, set_route_entries_attribute(Eq(2), _, _, _, _))
        .WillOnce(
            DoAll(SetArrayArgument<4>(exp_status_2.begin(), exp_status_2.end()),
                  Return(SAI_STATUS_FAILURE)));
    EXPECT_CALL(mock_sai_route_, set_route_entries_attribute(Eq(1), _, _, _, _))
        .WillOnce(
            DoAll(SetArrayArgument<4>(exp_status_4.begin(), exp_status_4.end()),
                  Return(SAI_STATUS_FAILURE)));
    // TODO: Expect critical state.
    EXPECT_THAT(UpdateRouteEntries(std::vector<P4RouteEntry>{route_entry}),
                ArrayEq(std::vector<StatusCode>{StatusCode::SWSS_RC_UNKNOWN}));
}

TEST_F(RouteManagerTest, UpdateRouteEntryTrapWithSaiErrorShouldFail)
{
    auto swss_ipv4_route_prefix = swss::IpPrefix(kIpv4Prefix);
    SetupNexthopIdRouteEntry(gVrfName, swss_ipv4_route_prefix, kNexthopId1, kNexthopOid1);

    auto route_entry = GenerateP4RouteEntry(gVrfName, swss_ipv4_route_prefix, p4orch::kTrap, "");
    std::vector<sai_status_t> exp_status_1{SAI_STATUS_FAILURE,
                                           SAI_STATUS_NOT_EXECUTED};
    std::vector<sai_status_t> exp_status_2{SAI_STATUS_SUCCESS,
                                           SAI_STATUS_FAILURE};
    std::vector<sai_status_t> exp_status_3{SAI_STATUS_SUCCESS};
    std::vector<sai_status_t> exp_status_4{SAI_STATUS_FAILURE};
    EXPECT_CALL(mock_sai_route_, set_route_entries_attribute(Eq(2), _, _, _, _))
        .WillOnce(
            DoAll(SetArrayArgument<4>(exp_status_1.begin(), exp_status_1.end()),
                  Return(SAI_STATUS_FAILURE)));
    EXPECT_THAT(UpdateRouteEntries(std::vector<P4RouteEntry>{route_entry}),
                ArrayEq(std::vector<StatusCode>{StatusCode::SWSS_RC_UNKNOWN}));
    EXPECT_CALL(mock_sai_route_, set_route_entries_attribute(Eq(2), _, _, _, _))
        .WillOnce(
            DoAll(SetArrayArgument<4>(exp_status_2.begin(), exp_status_2.end()),
                  Return(SAI_STATUS_FAILURE)));
    EXPECT_CALL(mock_sai_route_, set_route_entries_attribute(Eq(1), _, _, _, _))
        .WillOnce(
            DoAll(SetArrayArgument<4>(exp_status_3.begin(), exp_status_3.end()),
                  Return(SAI_STATUS_SUCCESS)));

    EXPECT_THAT(UpdateRouteEntries(std::vector<P4RouteEntry>{route_entry}),
                ArrayEq(std::vector<StatusCode>{StatusCode::SWSS_RC_UNKNOWN}));
    EXPECT_CALL(mock_sai_route_, set_route_entries_attribute(Eq(2), _, _, _, _))
        .WillOnce(
            DoAll(SetArrayArgument<4>(exp_status_2.begin(), exp_status_2.end()),
                  Return(SAI_STATUS_FAILURE)));
    EXPECT_CALL(mock_sai_route_, set_route_entries_attribute(Eq(1), _, _, _, _))
        .WillOnce(
            DoAll(SetArrayArgument<4>(exp_status_4.begin(), exp_status_4.end()),
                  Return(SAI_STATUS_FAILURE)));
    // TODO: Expect critical state.
    EXPECT_THAT(UpdateRouteEntries(std::vector<P4RouteEntry>{route_entry}),
                ArrayEq(std::vector<StatusCode>{StatusCode::SWSS_RC_UNKNOWN}));
}

TEST_F(RouteManagerTest, UpdateRouteWithDifferentNexthopIdsSucceeds)
{
    auto swss_ipv4_route_prefix = swss::IpPrefix(kIpv4Prefix);
    sai_ip_prefix_t sai_ipv4_route_prefix;
    copy(sai_ipv4_route_prefix, swss_ipv4_route_prefix);
    SetupNexthopIdRouteEntry(gVrfName, swss_ipv4_route_prefix, kNexthopId1, kNexthopOid1);

    auto route_entry = GenerateP4RouteEntry(gVrfName, swss_ipv4_route_prefix, p4orch::kSetNexthopId, kNexthopId2);
    p4_oid_mapper_.setOID(SAI_OBJECT_TYPE_NEXT_HOP, KeyGenerator::generateNextHopKey(route_entry.nexthop_id),
                          kNexthopOid2);

    sai_route_entry_t exp_sai_route_entry;
    exp_sai_route_entry.switch_id = gSwitchId;
    exp_sai_route_entry.vr_id = gVrfOid;
    exp_sai_route_entry.destination = sai_ipv4_route_prefix;

    sai_attribute_t exp_sai_attr;
    exp_sai_attr.id = SAI_ROUTE_ENTRY_ATTR_NEXT_HOP_ID;
    exp_sai_attr.value.oid = kNexthopOid2;

    std::vector<sai_status_t> exp_status{SAI_STATUS_SUCCESS};
    EXPECT_CALL(mock_sai_route_, set_route_entries_attribute(
                                     Eq(1), RouteEntryArrayEq(std::vector<sai_route_entry_t>{exp_sai_route_entry}),
                                     AttrArrayEq(std::vector<sai_attribute_t>{exp_sai_attr}),
                                     Eq(SAI_BULK_OP_ERROR_MODE_IGNORE_ERROR), _))
        .WillOnce(DoAll(SetArrayArgument<4>(exp_status.begin(), exp_status.end()), Return(SAI_STATUS_SUCCESS)));
    EXPECT_THAT(UpdateRouteEntries(std::vector<P4RouteEntry>{route_entry}),
                ArrayEq(std::vector<StatusCode>{StatusCode::SWSS_RC_SUCCESS}));
    route_entry.action = p4orch::kSetNexthopId;
    VerifyRouteEntry(route_entry, sai_ipv4_route_prefix, gVrfOid);
    uint32_t ref_cnt;
    EXPECT_TRUE(
        p4_oid_mapper_.getRefCount(SAI_OBJECT_TYPE_NEXT_HOP, KeyGenerator::generateNextHopKey(kNexthopId1), &ref_cnt));
    EXPECT_EQ(0, ref_cnt);
    EXPECT_TRUE(
        p4_oid_mapper_.getRefCount(SAI_OBJECT_TYPE_NEXT_HOP, KeyGenerator::generateNextHopKey(kNexthopId2), &ref_cnt));
    EXPECT_EQ(1, ref_cnt);
}

TEST_F(RouteManagerTest, UpdateRouteWithDifferentNexthopIdsAndMetadatasSucceeds)
{
    auto swss_ipv4_route_prefix = swss::IpPrefix(kIpv4Prefix);
    sai_ip_prefix_t sai_ipv4_route_prefix;
    copy(sai_ipv4_route_prefix, swss_ipv4_route_prefix);
    SetupNexthopIdRouteEntry(gVrfName, swss_ipv4_route_prefix, kNexthopId1, kNexthopOid1, kMetadata1);

    auto route_entry = GenerateP4RouteEntry(gVrfName, swss_ipv4_route_prefix, p4orch::kSetNexthopIdAndMetadata,
                                            kNexthopId2, kMetadata2);
    p4_oid_mapper_.setOID(SAI_OBJECT_TYPE_NEXT_HOP, KeyGenerator::generateNextHopKey(route_entry.nexthop_id),
                          kNexthopOid2);

    sai_route_entry_t exp_sai_route_entry;
    exp_sai_route_entry.switch_id = gSwitchId;
    exp_sai_route_entry.vr_id = gVrfOid;
    exp_sai_route_entry.destination = sai_ipv4_route_prefix;

    sai_attribute_t exp_sai_attr_1;
    exp_sai_attr_1.id = SAI_ROUTE_ENTRY_ATTR_META_DATA;
    exp_sai_attr_1.value.u32 = kMetadataInt2;
    sai_attribute_t exp_sai_attr_2;
    exp_sai_attr_2.id = SAI_ROUTE_ENTRY_ATTR_NEXT_HOP_ID;
    exp_sai_attr_2.value.oid = kNexthopOid2;

    std::vector<sai_status_t> exp_status{SAI_STATUS_SUCCESS,
                                         SAI_STATUS_SUCCESS};
    EXPECT_CALL(mock_sai_route_,
                set_route_entries_attribute(
                    Eq(2),
                    RouteEntryArrayEq(std::vector<sai_route_entry_t>{
                        exp_sai_route_entry, exp_sai_route_entry}),
                    AttrArrayEq(std::vector<sai_attribute_t>{exp_sai_attr_1,
                                                             exp_sai_attr_2}),
                    Eq(SAI_BULK_OP_ERROR_MODE_IGNORE_ERROR), _))
        .WillOnce(
            DoAll(SetArrayArgument<4>(exp_status.begin(), exp_status.end()),
                  Return(SAI_STATUS_SUCCESS)));
    EXPECT_THAT(UpdateRouteEntries(std::vector<P4RouteEntry>{route_entry}),
                ArrayEq(std::vector<StatusCode>{StatusCode::SWSS_RC_SUCCESS}));
    VerifyRouteEntry(route_entry, sai_ipv4_route_prefix, gVrfOid);
    uint32_t ref_cnt;
    EXPECT_TRUE(
        p4_oid_mapper_.getRefCount(SAI_OBJECT_TYPE_NEXT_HOP, KeyGenerator::generateNextHopKey(kNexthopId1), &ref_cnt));
    EXPECT_EQ(0, ref_cnt);
    EXPECT_TRUE(
        p4_oid_mapper_.getRefCount(SAI_OBJECT_TYPE_NEXT_HOP, KeyGenerator::generateNextHopKey(kNexthopId2), &ref_cnt));
    EXPECT_EQ(1, ref_cnt);
}

TEST_F(RouteManagerTest, UpdateRouteFromNexthopIdToDropSucceeds)
{
    auto swss_ipv4_route_prefix = swss::IpPrefix(kIpv4Prefix);
    sai_ip_prefix_t sai_ipv4_route_prefix;
    copy(sai_ipv4_route_prefix, swss_ipv4_route_prefix);
    SetupNexthopIdRouteEntry(gVrfName, swss_ipv4_route_prefix, kNexthopId1, kNexthopOid1);

    auto route_entry = GenerateP4RouteEntry(gVrfName, swss_ipv4_route_prefix, p4orch::kDrop, "");

    sai_route_entry_t exp_sai_route_entry;
    exp_sai_route_entry.switch_id = gSwitchId;
    exp_sai_route_entry.vr_id = gVrfOid;
    exp_sai_route_entry.destination = sai_ipv4_route_prefix;

    sai_attribute_t exp_sai_attr_1;
    exp_sai_attr_1.id = SAI_ROUTE_ENTRY_ATTR_PACKET_ACTION;
    exp_sai_attr_1.value.s32 = SAI_PACKET_ACTION_DROP;
    sai_attribute_t exp_sai_attr_2;
    exp_sai_attr_2.id = SAI_ROUTE_ENTRY_ATTR_NEXT_HOP_ID;
    exp_sai_attr_2.value.oid = SAI_NULL_OBJECT_ID;

    std::vector<sai_status_t> exp_status{SAI_STATUS_SUCCESS,
                                         SAI_STATUS_SUCCESS};
    EXPECT_CALL(mock_sai_route_,
                set_route_entries_attribute(
                    Eq(2),
                    RouteEntryArrayEq(std::vector<sai_route_entry_t>{
                        exp_sai_route_entry, exp_sai_route_entry}),
                    AttrArrayEq(std::vector<sai_attribute_t>{exp_sai_attr_1,
                                                             exp_sai_attr_2}),
                    Eq(SAI_BULK_OP_ERROR_MODE_IGNORE_ERROR), _))
        .WillOnce(
            DoAll(SetArrayArgument<4>(exp_status.begin(), exp_status.end()),
                  Return(SAI_STATUS_SUCCESS)));
    EXPECT_THAT(UpdateRouteEntries(std::vector<P4RouteEntry>{route_entry}),
                ArrayEq(std::vector<StatusCode>{StatusCode::SWSS_RC_SUCCESS}));
    VerifyRouteEntry(route_entry, sai_ipv4_route_prefix, gVrfOid);
    uint32_t ref_cnt;
    EXPECT_TRUE(
        p4_oid_mapper_.getRefCount(SAI_OBJECT_TYPE_NEXT_HOP, KeyGenerator::generateNextHopKey(kNexthopId1), &ref_cnt));
    EXPECT_EQ(0, ref_cnt);
}

TEST_F(RouteManagerTest, UpdateRouteFromNexthopIdToDropWithMetadataSucceeds) {
  auto swss_ipv4_route_prefix = swss::IpPrefix(kIpv4Prefix);
  sai_ip_prefix_t sai_ipv4_route_prefix;
  copy(sai_ipv4_route_prefix, swss_ipv4_route_prefix);
  SetupNexthopIdRouteEntry(gVrfName, swss_ipv4_route_prefix, kNexthopId1,
                           kNexthopOid1);

  auto route_entry =
      GenerateP4RouteEntry(gVrfName, swss_ipv4_route_prefix,
                           p4orch::kSetMetadataAndDrop, "", kMetadata1);

  sai_route_entry_t exp_sai_route_entry;
  exp_sai_route_entry.switch_id = gSwitchId;
  exp_sai_route_entry.vr_id = gVrfOid;
  exp_sai_route_entry.destination = sai_ipv4_route_prefix;

  sai_attribute_t exp_sai_attr_1;
  exp_sai_attr_1.id = SAI_ROUTE_ENTRY_ATTR_PACKET_ACTION;
  exp_sai_attr_1.value.s32 = SAI_PACKET_ACTION_DROP;
  sai_attribute_t exp_sai_attr_2;
  exp_sai_attr_2.id = SAI_ROUTE_ENTRY_ATTR_NEXT_HOP_ID;
  exp_sai_attr_2.value.oid = SAI_NULL_OBJECT_ID;
  sai_attribute_t exp_sai_attr_3;
  exp_sai_attr_3.id = SAI_ROUTE_ENTRY_ATTR_META_DATA;
  exp_sai_attr_3.value.s32 = kMetadataInt1;

  std::vector<sai_status_t> exp_status{SAI_STATUS_SUCCESS, SAI_STATUS_SUCCESS,
                                       SAI_STATUS_SUCCESS};
  EXPECT_CALL(
      mock_sai_route_,
      set_route_entries_attribute(
          Eq(3),
          RouteEntryArrayEq(std::vector<sai_route_entry_t>{
              exp_sai_route_entry, exp_sai_route_entry, exp_sai_route_entry}),
          AttrArrayEq(std::vector<sai_attribute_t>{
              exp_sai_attr_1, exp_sai_attr_2, exp_sai_attr_3}),
          Eq(SAI_BULK_OP_ERROR_MODE_IGNORE_ERROR), _))
      .WillOnce(DoAll(SetArrayArgument<4>(exp_status.begin(), exp_status.end()),
                      Return(SAI_STATUS_SUCCESS)));

  EXPECT_THAT(UpdateRouteEntries(std::vector<P4RouteEntry>{route_entry}),
              ArrayEq(std::vector<StatusCode>{StatusCode::SWSS_RC_SUCCESS}));
  VerifyRouteEntry(route_entry, sai_ipv4_route_prefix, gVrfOid);
  uint32_t ref_cnt;
  EXPECT_TRUE(p4_oid_mapper_.getRefCount(
      SAI_OBJECT_TYPE_NEXT_HOP, KeyGenerator::generateNextHopKey(kNexthopId1),
      &ref_cnt));
  EXPECT_EQ(0, ref_cnt);
}

TEST_F(RouteManagerTest, UpdateRouteFromNexthopIdAndMetadataToDropSucceeds)
{
    auto swss_ipv4_route_prefix = swss::IpPrefix(kIpv4Prefix);
    sai_ip_prefix_t sai_ipv4_route_prefix;
    copy(sai_ipv4_route_prefix, swss_ipv4_route_prefix);
    SetupNexthopIdRouteEntry(gVrfName, swss_ipv4_route_prefix, kNexthopId1, kNexthopOid1, kMetadata2);

    auto route_entry = GenerateP4RouteEntry(gVrfName, swss_ipv4_route_prefix, p4orch::kDrop, "");

    sai_route_entry_t exp_sai_route_entry;
    exp_sai_route_entry.switch_id = gSwitchId;
    exp_sai_route_entry.vr_id = gVrfOid;
    exp_sai_route_entry.destination = sai_ipv4_route_prefix;

    sai_attribute_t exp_sai_attr_1;
    exp_sai_attr_1.id = SAI_ROUTE_ENTRY_ATTR_PACKET_ACTION;
    exp_sai_attr_1.value.s32 = SAI_PACKET_ACTION_DROP;
    sai_attribute_t exp_sai_attr_2;
    exp_sai_attr_2.id = SAI_ROUTE_ENTRY_ATTR_NEXT_HOP_ID;
    exp_sai_attr_2.value.oid = SAI_NULL_OBJECT_ID;
    sai_attribute_t exp_sai_attr_3;
    exp_sai_attr_3.id = SAI_ROUTE_ENTRY_ATTR_META_DATA;
    exp_sai_attr_3.value.u32 = 0;

    std::vector<sai_status_t> exp_status{SAI_STATUS_SUCCESS, SAI_STATUS_SUCCESS,
                                         SAI_STATUS_SUCCESS};
    EXPECT_CALL(
        mock_sai_route_,
        set_route_entries_attribute(
            Eq(3),
            RouteEntryArrayEq(std::vector<sai_route_entry_t>{
                exp_sai_route_entry, exp_sai_route_entry, exp_sai_route_entry}),
            AttrArrayEq(std::vector<sai_attribute_t>{
                exp_sai_attr_1, exp_sai_attr_2, exp_sai_attr_3}),
            Eq(SAI_BULK_OP_ERROR_MODE_IGNORE_ERROR), _))
        .WillOnce(
            DoAll(SetArrayArgument<4>(exp_status.begin(), exp_status.end()),
                  Return(SAI_STATUS_SUCCESS)));
    EXPECT_THAT(UpdateRouteEntries(std::vector<P4RouteEntry>{route_entry}),
                ArrayEq(std::vector<StatusCode>{StatusCode::SWSS_RC_SUCCESS}));
    VerifyRouteEntry(route_entry, sai_ipv4_route_prefix, gVrfOid);
    uint32_t ref_cnt;
    EXPECT_TRUE(
        p4_oid_mapper_.getRefCount(SAI_OBJECT_TYPE_NEXT_HOP, KeyGenerator::generateNextHopKey(kNexthopId1), &ref_cnt));
    EXPECT_EQ(0, ref_cnt);
}

TEST_F(RouteManagerTest, UpdateRouteFromDropToNexthopIdSucceeds)
{
    auto swss_ipv4_route_prefix = swss::IpPrefix(kIpv4Prefix);
    sai_ip_prefix_t sai_ipv4_route_prefix;
    copy(sai_ipv4_route_prefix, swss_ipv4_route_prefix);
    SetupDropRouteEntry(gVrfName, swss_ipv4_route_prefix);

    auto route_entry = GenerateP4RouteEntry(gVrfName, swss_ipv4_route_prefix, p4orch::kSetNexthopId, kNexthopId2);
    p4_oid_mapper_.setOID(SAI_OBJECT_TYPE_NEXT_HOP, KeyGenerator::generateNextHopKey(route_entry.nexthop_id),
                          kNexthopOid2);

    sai_route_entry_t exp_sai_route_entry;
    exp_sai_route_entry.switch_id = gSwitchId;
    exp_sai_route_entry.vr_id = gVrfOid;
    exp_sai_route_entry.destination = sai_ipv4_route_prefix;

    sai_attribute_t exp_sai_attr_1;
    exp_sai_attr_1.id = SAI_ROUTE_ENTRY_ATTR_NEXT_HOP_ID;
    exp_sai_attr_1.value.oid = kNexthopOid2;
    sai_attribute_t exp_sai_attr_2;
    exp_sai_attr_2.id = SAI_ROUTE_ENTRY_ATTR_PACKET_ACTION;
    exp_sai_attr_2.value.s32 = SAI_PACKET_ACTION_FORWARD;

    std::vector<sai_status_t> exp_status{SAI_STATUS_SUCCESS,
                                         SAI_STATUS_SUCCESS};
    EXPECT_CALL(mock_sai_route_,
                set_route_entries_attribute(
                    Eq(2),
                    RouteEntryArrayEq(std::vector<sai_route_entry_t>{
                        exp_sai_route_entry, exp_sai_route_entry}),
                    AttrArrayEq(std::vector<sai_attribute_t>{exp_sai_attr_1,
                                                             exp_sai_attr_2}),
                    Eq(SAI_BULK_OP_ERROR_MODE_IGNORE_ERROR), _))
        .WillOnce(
            DoAll(SetArrayArgument<4>(exp_status.begin(), exp_status.end()),
                  Return(SAI_STATUS_SUCCESS)));
    EXPECT_THAT(UpdateRouteEntries(std::vector<P4RouteEntry>{route_entry}),
                ArrayEq(std::vector<StatusCode>{StatusCode::SWSS_RC_SUCCESS}));
    VerifyRouteEntry(route_entry, sai_ipv4_route_prefix, gVrfOid);
    uint32_t ref_cnt;
    EXPECT_TRUE(
        p4_oid_mapper_.getRefCount(SAI_OBJECT_TYPE_NEXT_HOP, KeyGenerator::generateNextHopKey(kNexthopId2), &ref_cnt));
    EXPECT_EQ(1, ref_cnt);
}

TEST_F(RouteManagerTest, UpdateRouteFromDropToWcmpWithMetadataSucceeds)
{
    auto swss_ipv4_route_prefix = swss::IpPrefix(kIpv4Prefix);
    sai_ip_prefix_t sai_ipv4_route_prefix;
    copy(sai_ipv4_route_prefix, swss_ipv4_route_prefix);
    SetupDropRouteEntry(gVrfName, swss_ipv4_route_prefix);

    auto route_entry = GenerateP4RouteEntry(gVrfName, swss_ipv4_route_prefix, p4orch::kSetWcmpGroupIdAndMetadata,
                                            kWcmpGroup1, kMetadata2);
    p4_oid_mapper_.setOID(SAI_OBJECT_TYPE_NEXT_HOP_GROUP, KeyGenerator::generateWcmpGroupKey(route_entry.wcmp_group),
                          kWcmpGroupOid1);

    sai_route_entry_t exp_sai_route_entry;
    exp_sai_route_entry.switch_id = gSwitchId;
    exp_sai_route_entry.vr_id = gVrfOid;
    exp_sai_route_entry.destination = sai_ipv4_route_prefix;

    sai_attribute_t exp_sai_attr_1;
    exp_sai_attr_1.id = SAI_ROUTE_ENTRY_ATTR_META_DATA;
    exp_sai_attr_1.value.u32 = kMetadataInt2;
    sai_attribute_t exp_sai_attr_2;
    exp_sai_attr_2.id = SAI_ROUTE_ENTRY_ATTR_NEXT_HOP_ID;
    exp_sai_attr_2.value.oid = kWcmpGroupOid1;
    sai_attribute_t exp_sai_attr_3;
    exp_sai_attr_3.id = SAI_ROUTE_ENTRY_ATTR_PACKET_ACTION;
    exp_sai_attr_3.value.s32 = SAI_PACKET_ACTION_FORWARD;

    std::vector<sai_status_t> exp_status{SAI_STATUS_SUCCESS, SAI_STATUS_SUCCESS,
                                         SAI_STATUS_SUCCESS};
    EXPECT_CALL(
        mock_sai_route_,
        set_route_entries_attribute(
            Eq(3),
            RouteEntryArrayEq(std::vector<sai_route_entry_t>{
                exp_sai_route_entry, exp_sai_route_entry, exp_sai_route_entry}),
            AttrArrayEq(std::vector<sai_attribute_t>{
                exp_sai_attr_1, exp_sai_attr_2, exp_sai_attr_3}),
            Eq(SAI_BULK_OP_ERROR_MODE_IGNORE_ERROR), _))
        .WillOnce(
            DoAll(SetArrayArgument<4>(exp_status.begin(), exp_status.end()),
                  Return(SAI_STATUS_SUCCESS)));
    EXPECT_THAT(UpdateRouteEntries(std::vector<P4RouteEntry>{route_entry}),
                ArrayEq(std::vector<StatusCode>{StatusCode::SWSS_RC_SUCCESS}));
    VerifyRouteEntry(route_entry, sai_ipv4_route_prefix, gVrfOid);
    uint32_t ref_cnt;
    EXPECT_TRUE(p4_oid_mapper_.getRefCount(SAI_OBJECT_TYPE_NEXT_HOP_GROUP,
                                           KeyGenerator::generateWcmpGroupKey(kWcmpGroup1), &ref_cnt));
    EXPECT_EQ(1, ref_cnt);
}

TEST_F(RouteManagerTest, UpdateRouteFromTrapToDropAndSetMetadataSucceeds)
{
    auto swss_ipv4_route_prefix = swss::IpPrefix(kIpv4Prefix);
    sai_ip_prefix_t sai_ipv4_route_prefix;
    copy(sai_ipv4_route_prefix, swss_ipv4_route_prefix);
    SetupTrapRouteEntry(gVrfName, swss_ipv4_route_prefix);

    auto route_entry =
        GenerateP4RouteEntry(gVrfName, swss_ipv4_route_prefix, p4orch::kSetMetadataAndDrop, "", kMetadata2);

    sai_route_entry_t exp_sai_route_entry;
    exp_sai_route_entry.switch_id = gSwitchId;
    exp_sai_route_entry.vr_id = gVrfOid;
    exp_sai_route_entry.destination = sai_ipv4_route_prefix;

    sai_attribute_t exp_sai_attr_1;
    exp_sai_attr_1.id = SAI_ROUTE_ENTRY_ATTR_PACKET_ACTION;
    exp_sai_attr_1.value.s32 = SAI_PACKET_ACTION_DROP;
    sai_attribute_t exp_sai_attr_2;
    exp_sai_attr_2.id = SAI_ROUTE_ENTRY_ATTR_META_DATA;
    exp_sai_attr_2.value.u32 = kMetadataInt2;

    std::vector<sai_status_t> exp_status{SAI_STATUS_SUCCESS,
                                         SAI_STATUS_SUCCESS};
    EXPECT_CALL(mock_sai_route_,
                set_route_entries_attribute(
                    Eq(2),
                    RouteEntryArrayEq(std::vector<sai_route_entry_t>{
                        exp_sai_route_entry, exp_sai_route_entry}),
                    AttrArrayEq(std::vector<sai_attribute_t>{exp_sai_attr_1,
                                                             exp_sai_attr_2}),
                    Eq(SAI_BULK_OP_ERROR_MODE_IGNORE_ERROR), _))
        .WillOnce(
            DoAll(SetArrayArgument<4>(exp_status.begin(), exp_status.end()),
                  Return(SAI_STATUS_SUCCESS)));
    EXPECT_THAT(UpdateRouteEntries(std::vector<P4RouteEntry>{route_entry}),
                ArrayEq(std::vector<StatusCode>{StatusCode::SWSS_RC_SUCCESS}));
    VerifyRouteEntry(route_entry, sai_ipv4_route_prefix, gVrfOid);
}

TEST_F(RouteManagerTest, UpdateRouteFromTrapToDropSucceeds)
{
    auto swss_ipv4_route_prefix = swss::IpPrefix(kIpv4Prefix);
    sai_ip_prefix_t sai_ipv4_route_prefix;
    copy(sai_ipv4_route_prefix, swss_ipv4_route_prefix);
    SetupTrapRouteEntry(gVrfName, swss_ipv4_route_prefix);
    auto route_entry = GenerateP4RouteEntry(gVrfName, swss_ipv4_route_prefix, p4orch::kDrop, "");

    sai_route_entry_t exp_sai_route_entry;
    exp_sai_route_entry.switch_id = gSwitchId;
    exp_sai_route_entry.vr_id = gVrfOid;
    exp_sai_route_entry.destination = sai_ipv4_route_prefix;

    sai_attribute_t exp_sai_attr;
    exp_sai_attr.id = SAI_ROUTE_ENTRY_ATTR_PACKET_ACTION;
    exp_sai_attr.value.s32 = SAI_PACKET_ACTION_DROP;

    std::vector<sai_status_t> exp_status{SAI_STATUS_SUCCESS};
    EXPECT_CALL(mock_sai_route_, set_route_entries_attribute(
                                     Eq(1), RouteEntryArrayEq(std::vector<sai_route_entry_t>{exp_sai_route_entry}),
                                     AttrArrayEq(std::vector<sai_attribute_t>{exp_sai_attr}),
                                     Eq(SAI_BULK_OP_ERROR_MODE_IGNORE_ERROR), _))
        .WillOnce(DoAll(SetArrayArgument<4>(exp_status.begin(), exp_status.end()), Return(SAI_STATUS_SUCCESS)));
    EXPECT_THAT(UpdateRouteEntries(std::vector<P4RouteEntry>{route_entry}),
                ArrayEq(std::vector<StatusCode>{StatusCode::SWSS_RC_SUCCESS}));
    VerifyRouteEntry(route_entry, sai_ipv4_route_prefix, gVrfOid);
}

TEST_F(RouteManagerTest, UpdateRouteFromDropToTrapSucceeds)
{
    auto swss_ipv4_route_prefix = swss::IpPrefix(kIpv4Prefix);
    sai_ip_prefix_t sai_ipv4_route_prefix;
    copy(sai_ipv4_route_prefix, swss_ipv4_route_prefix);
    SetupDropRouteEntry(gVrfName, swss_ipv4_route_prefix);
    auto route_entry = GenerateP4RouteEntry(gVrfName, swss_ipv4_route_prefix, p4orch::kTrap, "");

    sai_route_entry_t exp_sai_route_entry;
    exp_sai_route_entry.switch_id = gSwitchId;
    exp_sai_route_entry.vr_id = gVrfOid;
    exp_sai_route_entry.destination = sai_ipv4_route_prefix;

    sai_attribute_t exp_sai_attr;
    exp_sai_attr.id = SAI_ROUTE_ENTRY_ATTR_PACKET_ACTION;
    exp_sai_attr.value.s32 = SAI_PACKET_ACTION_TRAP;

    std::vector<sai_status_t> exp_status{SAI_STATUS_SUCCESS};
    EXPECT_CALL(mock_sai_route_, set_route_entries_attribute(
                                     Eq(1), RouteEntryArrayEq(std::vector<sai_route_entry_t>{exp_sai_route_entry}),
                                     AttrArrayEq(std::vector<sai_attribute_t>{exp_sai_attr}),
                                     Eq(SAI_BULK_OP_ERROR_MODE_IGNORE_ERROR), _))
        .WillOnce(DoAll(SetArrayArgument<4>(exp_status.begin(), exp_status.end()), Return(SAI_STATUS_SUCCESS)));
    EXPECT_THAT(UpdateRouteEntries(std::vector<P4RouteEntry>{route_entry}),
                ArrayEq(std::vector<StatusCode>{StatusCode::SWSS_RC_SUCCESS}));
    VerifyRouteEntry(route_entry, sai_ipv4_route_prefix, gVrfOid);
}

TEST_F(RouteManagerTest, UpdateRouteFromNexthopIdToTrapSucceeds)
{
    auto swss_ipv4_route_prefix = swss::IpPrefix(kIpv4Prefix);
    sai_ip_prefix_t sai_ipv4_route_prefix;
    copy(sai_ipv4_route_prefix, swss_ipv4_route_prefix);
    SetupNexthopIdRouteEntry(gVrfName, swss_ipv4_route_prefix, kNexthopId1, kNexthopOid1);
    auto route_entry = GenerateP4RouteEntry(gVrfName, swss_ipv4_route_prefix, p4orch::kTrap, "");

    sai_route_entry_t exp_sai_route_entry;
    exp_sai_route_entry.switch_id = gSwitchId;
    exp_sai_route_entry.vr_id = gVrfOid;
    exp_sai_route_entry.destination = sai_ipv4_route_prefix;

    sai_attribute_t exp_sai_attr_1;
    exp_sai_attr_1.id = SAI_ROUTE_ENTRY_ATTR_PACKET_ACTION;
    exp_sai_attr_1.value.s32 = SAI_PACKET_ACTION_TRAP;
    sai_attribute_t exp_sai_attr_2;
    exp_sai_attr_2.id = SAI_ROUTE_ENTRY_ATTR_NEXT_HOP_ID;
    exp_sai_attr_2.value.oid = SAI_NULL_OBJECT_ID;

    std::vector<sai_status_t> exp_status{SAI_STATUS_SUCCESS,
                                         SAI_STATUS_SUCCESS};

    EXPECT_CALL(mock_sai_route_,
                set_route_entries_attribute(
                    Eq(2),
                    RouteEntryArrayEq(std::vector<sai_route_entry_t>{
                        exp_sai_route_entry, exp_sai_route_entry}),
                    AttrArrayEq(std::vector<sai_attribute_t>{exp_sai_attr_1,
                                                             exp_sai_attr_2}),
                    Eq(SAI_BULK_OP_ERROR_MODE_IGNORE_ERROR), _))
        .WillOnce(
            DoAll(SetArrayArgument<4>(exp_status.begin(), exp_status.end()),
                  Return(SAI_STATUS_SUCCESS)));
    EXPECT_THAT(UpdateRouteEntries(std::vector<P4RouteEntry>{route_entry}),
                ArrayEq(std::vector<StatusCode>{StatusCode::SWSS_RC_SUCCESS}));
    VerifyRouteEntry(route_entry, sai_ipv4_route_prefix, gVrfOid);
    uint32_t ref_cnt;
    EXPECT_TRUE(
        p4_oid_mapper_.getRefCount(SAI_OBJECT_TYPE_NEXT_HOP, KeyGenerator::generateNextHopKey(kNexthopId1), &ref_cnt));
    EXPECT_EQ(0, ref_cnt);
}

TEST_F(RouteManagerTest, UpdateRouteFromTrapToNexthopIdSucceeds)
{
    auto swss_ipv4_route_prefix = swss::IpPrefix(kIpv4Prefix);
    sai_ip_prefix_t sai_ipv4_route_prefix;
    copy(sai_ipv4_route_prefix, swss_ipv4_route_prefix);
    SetupTrapRouteEntry(gVrfName, swss_ipv4_route_prefix);

    auto route_entry = GenerateP4RouteEntry(gVrfName, swss_ipv4_route_prefix, p4orch::kSetNexthopId, kNexthopId2);
    p4_oid_mapper_.setOID(SAI_OBJECT_TYPE_NEXT_HOP, KeyGenerator::generateNextHopKey(route_entry.nexthop_id),
                          kNexthopOid2);

    sai_route_entry_t exp_sai_route_entry;
    exp_sai_route_entry.switch_id = gSwitchId;
    exp_sai_route_entry.vr_id = gVrfOid;
    exp_sai_route_entry.destination = sai_ipv4_route_prefix;

    sai_attribute_t exp_sai_attr_1;
    exp_sai_attr_1.id = SAI_ROUTE_ENTRY_ATTR_NEXT_HOP_ID;
    exp_sai_attr_1.value.oid = kNexthopOid2;
    sai_attribute_t exp_sai_attr_2;
    exp_sai_attr_2.id = SAI_ROUTE_ENTRY_ATTR_PACKET_ACTION;
    exp_sai_attr_2.value.s32 = SAI_PACKET_ACTION_FORWARD;

    std::vector<sai_status_t> exp_status{SAI_STATUS_SUCCESS,
                                         SAI_STATUS_SUCCESS};

    EXPECT_CALL(mock_sai_route_,
                set_route_entries_attribute(
                    Eq(2),
                    RouteEntryArrayEq(std::vector<sai_route_entry_t>{
                        exp_sai_route_entry, exp_sai_route_entry}),
                    AttrArrayEq(std::vector<sai_attribute_t>{exp_sai_attr_1,
                                                             exp_sai_attr_2}),
                    Eq(SAI_BULK_OP_ERROR_MODE_IGNORE_ERROR), _))
        .WillOnce(
            DoAll(SetArrayArgument<4>(exp_status.begin(), exp_status.end()),
                  Return(SAI_STATUS_SUCCESS)));
    EXPECT_THAT(UpdateRouteEntries(std::vector<P4RouteEntry>{route_entry}),
                ArrayEq(std::vector<StatusCode>{StatusCode::SWSS_RC_SUCCESS}));
    VerifyRouteEntry(route_entry, sai_ipv4_route_prefix, gVrfOid);
    uint32_t ref_cnt;
    EXPECT_TRUE(
        p4_oid_mapper_.getRefCount(SAI_OBJECT_TYPE_NEXT_HOP, KeyGenerator::generateNextHopKey(kNexthopId2), &ref_cnt));
    EXPECT_EQ(1, ref_cnt);
}

TEST_F(RouteManagerTest, UpdateRouteFromTrapToNexthopIdAndMetadataRecoverFailureShouldRaiseCriticalState)
{
    auto swss_ipv4_route_prefix = swss::IpPrefix(kIpv4Prefix);
    sai_ip_prefix_t sai_ipv4_route_prefix;
    copy(sai_ipv4_route_prefix, swss_ipv4_route_prefix);
    SetupTrapRouteEntry(gVrfName, swss_ipv4_route_prefix);

    auto route_entry = GenerateP4RouteEntry(gVrfName, swss_ipv4_route_prefix, p4orch::kSetNexthopIdAndMetadata,
                                            kNexthopId2, kMetadata1);
    p4_oid_mapper_.setOID(SAI_OBJECT_TYPE_NEXT_HOP, KeyGenerator::generateNextHopKey(route_entry.nexthop_id),
                          kNexthopOid2);
    std::vector<sai_status_t> exp_status_1{
        SAI_STATUS_SUCCESS, SAI_STATUS_SUCCESS, SAI_STATUS_FAILURE};
    std::vector<sai_status_t> exp_status_2{SAI_STATUS_SUCCESS,
                                           SAI_STATUS_SUCCESS};
    std::vector<sai_status_t> exp_status_3{SAI_STATUS_SUCCESS,
                                           SAI_STATUS_FAILURE};
    EXPECT_CALL(mock_sai_route_, set_route_entries_attribute(Eq(3), _, _, _, _))
        .WillOnce(
            DoAll(SetArrayArgument<4>(exp_status_1.begin(), exp_status_1.end()),
                  Return(SAI_STATUS_FAILURE)));
    EXPECT_CALL(mock_sai_route_, set_route_entries_attribute(Eq(2), _, _, _, _))
        .WillOnce(
            DoAll(SetArrayArgument<4>(exp_status_2.begin(), exp_status_2.end()),
                  Return(SAI_STATUS_SUCCESS)));

    EXPECT_THAT(UpdateRouteEntries(std::vector<P4RouteEntry>{route_entry}),
                ArrayEq(std::vector<StatusCode>{StatusCode::SWSS_RC_UNKNOWN}));

    EXPECT_CALL(mock_sai_route_, set_route_entries_attribute(Eq(3), _, _, _, _))
        .WillOnce(
            DoAll(SetArrayArgument<4>(exp_status_1.begin(), exp_status_1.end()),
                  Return(SAI_STATUS_FAILURE)));
    EXPECT_CALL(mock_sai_route_, set_route_entries_attribute(Eq(2), _, _, _, _))
        .WillOnce(
            DoAll(SetArrayArgument<4>(exp_status_3.begin(), exp_status_3.end()),
                  Return(SAI_STATUS_FAILURE)));
    // TODO: Expect critical state.
    EXPECT_THAT(UpdateRouteEntries(std::vector<P4RouteEntry>{route_entry}),
                ArrayEq(std::vector<StatusCode>{StatusCode::SWSS_RC_UNKNOWN}));
}

TEST_F(RouteManagerTest, UpdateRouteWithDifferentWcmpGroupsSucceeds)
{
    auto swss_ipv4_route_prefix = swss::IpPrefix(kIpv4Prefix);
    sai_ip_prefix_t sai_ipv4_route_prefix;
    copy(sai_ipv4_route_prefix, swss_ipv4_route_prefix);
    SetupWcmpGroupRouteEntry(gVrfName, swss_ipv4_route_prefix, kWcmpGroup1, kWcmpGroupOid1);

    auto route_entry = GenerateP4RouteEntry(gVrfName, swss_ipv4_route_prefix, p4orch::kSetWcmpGroupId, kWcmpGroup2);
    p4_oid_mapper_.setOID(SAI_OBJECT_TYPE_NEXT_HOP_GROUP, KeyGenerator::generateWcmpGroupKey(route_entry.wcmp_group),
                          kWcmpGroupOid2);

    sai_route_entry_t exp_sai_route_entry;
    exp_sai_route_entry.switch_id = gSwitchId;
    exp_sai_route_entry.vr_id = gVrfOid;
    exp_sai_route_entry.destination = sai_ipv4_route_prefix;

    sai_attribute_t exp_sai_attr;
    exp_sai_attr.id = SAI_ROUTE_ENTRY_ATTR_NEXT_HOP_ID;
    exp_sai_attr.value.oid = kWcmpGroupOid2;

    std::vector<sai_status_t> exp_status{SAI_STATUS_SUCCESS};
    EXPECT_CALL(mock_sai_route_, set_route_entries_attribute(
                                     Eq(1), RouteEntryArrayEq(std::vector<sai_route_entry_t>{exp_sai_route_entry}),
                                     AttrArrayEq(std::vector<sai_attribute_t>{exp_sai_attr}),
                                     Eq(SAI_BULK_OP_ERROR_MODE_IGNORE_ERROR), _))
        .WillOnce(DoAll(SetArrayArgument<4>(exp_status.begin(), exp_status.end()), Return(SAI_STATUS_SUCCESS)));
    EXPECT_THAT(UpdateRouteEntries(std::vector<P4RouteEntry>{route_entry}),
                ArrayEq(std::vector<StatusCode>{StatusCode::SWSS_RC_SUCCESS}));
    route_entry.action = p4orch::kSetWcmpGroupId;
    VerifyRouteEntry(route_entry, sai_ipv4_route_prefix, gVrfOid);
    uint32_t ref_cnt;
    EXPECT_TRUE(p4_oid_mapper_.getRefCount(SAI_OBJECT_TYPE_NEXT_HOP_GROUP,
                                           KeyGenerator::generateWcmpGroupKey(kWcmpGroup1), &ref_cnt));
    EXPECT_EQ(0, ref_cnt);
    EXPECT_TRUE(p4_oid_mapper_.getRefCount(SAI_OBJECT_TYPE_NEXT_HOP_GROUP,
                                           KeyGenerator::generateWcmpGroupKey(kWcmpGroup2), &ref_cnt));
    EXPECT_EQ(1, ref_cnt);
}

TEST_F(RouteManagerTest, UpdateNexthopIdRouteWithNoChangeSucceeds)
{
    auto swss_ipv4_route_prefix = swss::IpPrefix(kIpv4Prefix);
    sai_ip_prefix_t sai_ipv4_route_prefix;
    copy(sai_ipv4_route_prefix, swss_ipv4_route_prefix);
    SetupNexthopIdRouteEntry(gVrfName, swss_ipv4_route_prefix, kNexthopId1, kNexthopOid1);

    auto route_entry = GenerateP4RouteEntry(gVrfName, swss_ipv4_route_prefix, p4orch::kSetNexthopId, kNexthopId1);
    EXPECT_THAT(UpdateRouteEntries(std::vector<P4RouteEntry>{route_entry}),
                ArrayEq(std::vector<StatusCode>{StatusCode::SWSS_RC_SUCCESS}));
    VerifyRouteEntry(route_entry, sai_ipv4_route_prefix, gVrfOid);
    uint32_t ref_cnt;
    EXPECT_TRUE(
        p4_oid_mapper_.getRefCount(SAI_OBJECT_TYPE_NEXT_HOP, KeyGenerator::generateNextHopKey(kNexthopId1), &ref_cnt));
    EXPECT_EQ(1, ref_cnt);
}

TEST_F(RouteManagerTest, UpdateRouteFromNexthopIdAndMetadataToDropRecoverFailureShouldRaiseCriticalState)
{
    auto swss_ipv4_route_prefix = swss::IpPrefix(kIpv4Prefix);
    sai_ip_prefix_t sai_ipv4_route_prefix;
    copy(sai_ipv4_route_prefix, swss_ipv4_route_prefix);
    SetupNexthopIdRouteEntry(gVrfName, swss_ipv4_route_prefix, kNexthopId1, kNexthopOid1, kMetadata2);

    auto route_entry = GenerateP4RouteEntry(gVrfName, swss_ipv4_route_prefix, p4orch::kDrop, "");

    std::vector<sai_status_t> exp_status_1{
        SAI_STATUS_FAILURE, SAI_STATUS_NOT_EXECUTED, SAI_STATUS_NOT_EXECUTED};
    std::vector<sai_status_t> exp_status_2{
        SAI_STATUS_SUCCESS, SAI_STATUS_SUCCESS, SAI_STATUS_FAILURE};
    std::vector<sai_status_t> exp_status_3{SAI_STATUS_SUCCESS,
                                           SAI_STATUS_FAILURE};
    EXPECT_CALL(mock_sai_route_, set_route_entries_attribute(Eq(3), _, _, _, _))
        .WillOnce(
            DoAll(SetArrayArgument<4>(exp_status_1.begin(), exp_status_1.end()),
                  Return(SAI_STATUS_FAILURE)));
    EXPECT_THAT(UpdateRouteEntries(std::vector<P4RouteEntry>{route_entry}),
                ArrayEq(std::vector<StatusCode>{StatusCode::SWSS_RC_UNKNOWN}));

    EXPECT_CALL(mock_sai_route_, set_route_entries_attribute(Eq(3), _, _, _, _))
        .WillOnce(
            DoAll(SetArrayArgument<4>(exp_status_2.begin(), exp_status_2.end()),
                  Return(SAI_STATUS_FAILURE)));
    EXPECT_CALL(mock_sai_route_, set_route_entries_attribute(Eq(2), _, _, _, _))
        .WillOnce(
            DoAll(SetArrayArgument<4>(exp_status_3.begin(), exp_status_3.end()),
                  Return(SAI_STATUS_FAILURE)));
    // TODO: Expect critical state.
    EXPECT_THAT(UpdateRouteEntries(std::vector<P4RouteEntry>{route_entry}),
                ArrayEq(std::vector<StatusCode>{StatusCode::SWSS_RC_UNKNOWN}));
}

TEST_F(RouteManagerTest, UpdateRouteFromDifferentNexthopIdAndMetadataRecoverFailureShouldRaiseCriticalState)
{
    auto swss_ipv4_route_prefix = swss::IpPrefix(kIpv4Prefix);
    sai_ip_prefix_t sai_ipv4_route_prefix;
    copy(sai_ipv4_route_prefix, swss_ipv4_route_prefix);
    SetupNexthopIdRouteEntry(gVrfName, swss_ipv4_route_prefix, kNexthopId1, kNexthopOid1, kMetadata1);

    auto route_entry = GenerateP4RouteEntry(gVrfName, swss_ipv4_route_prefix, p4orch::kSetNexthopIdAndMetadata,
                                            kNexthopId2, kMetadata2);
    p4_oid_mapper_.setOID(SAI_OBJECT_TYPE_NEXT_HOP, KeyGenerator::generateNextHopKey(route_entry.nexthop_id),
                          kNexthopOid2);

    std::vector<sai_status_t> exp_status_1{SAI_STATUS_FAILURE,
                                           SAI_STATUS_NOT_EXECUTED};
    std::vector<sai_status_t> exp_status_2{SAI_STATUS_SUCCESS,
                                           SAI_STATUS_FAILURE};
    std::vector<sai_status_t> exp_status_3{SAI_STATUS_FAILURE};
    EXPECT_CALL(mock_sai_route_, set_route_entries_attribute(Eq(2), _, _, _, _))
        .WillOnce(
            DoAll(SetArrayArgument<4>(exp_status_1.begin(), exp_status_1.end()),
                  Return(SAI_STATUS_FAILURE)));
    EXPECT_THAT(UpdateRouteEntries(std::vector<P4RouteEntry>{route_entry}),
                ArrayEq(std::vector<StatusCode>{StatusCode::SWSS_RC_UNKNOWN}));

    EXPECT_CALL(mock_sai_route_, set_route_entries_attribute(Eq(2), _, _, _, _))
        .WillOnce(
            DoAll(SetArrayArgument<4>(exp_status_2.begin(), exp_status_2.end()),
                  Return(SAI_STATUS_FAILURE)));
    EXPECT_CALL(mock_sai_route_, set_route_entries_attribute(Eq(1), _, _, _, _))
        .WillOnce(
            DoAll(SetArrayArgument<4>(exp_status_3.begin(), exp_status_3.end()),
                  Return(SAI_STATUS_FAILURE)));
    // TODO: Expect critical state.
    EXPECT_THAT(UpdateRouteEntries(std::vector<P4RouteEntry>{route_entry}),
                ArrayEq(std::vector<StatusCode>{StatusCode::SWSS_RC_UNKNOWN}));
}

TEST_F(RouteManagerTest, UpdateMulticastRouteEntriesSucceeds) {
  // We eventually want a prefix, not an exact match address.
  // TODO(b/281706130)
  auto swss_ipv6_route_prefix = swss::IpPrefix(kIpv6Prefix3);
  auto route_entry_ipv6 = SetupSetMulticastGroupIdRouteEntry(
      gVrfName, swss_ipv6_route_prefix, kMulticastGroupId1, kMulticastGroupOid1,
      kMetadata1);
  auto swss_ipv4_route_prefix = swss::IpPrefix(kIpv4Prefix4);
  auto route_entry_ipv4 = SetupSetMulticastGroupIdRouteEntry(
      gVrfName, swss_ipv4_route_prefix, kMulticastGroupId2, kMulticastGroupOid2,
      kMetadata1, /*expect_rpf=*/false);

  // Now update those entries to point to new multicast groups.
  auto new_route_entry_ipv6 = GenerateP4RouteEntry(
      gVrfName, swss_ipv6_route_prefix, p4orch::kSetMulticastGroupId,
      kMulticastGroupId3, kMetadata1);
  auto new_route_entry_ipv4 = GenerateP4RouteEntry(
      gVrfName, swss_ipv4_route_prefix, p4orch::kSetMulticastGroupId,
      kMulticastGroupId4, kMetadata1);
  // Create fake multicast group OIDs.
  AddMulticastGroup(kMulticastGroupId3, kMulticastGroupOid3);
  AddMulticastGroup(kMulticastGroupId4, kMulticastGroupOid4);

  EXPECT_CALL(mock_sai_ipmc_, set_ipmc_entry_attribute(_, _))
      .WillOnce(Return(SAI_STATUS_SUCCESS))
      .WillOnce(Return(SAI_STATUS_SUCCESS));
  EXPECT_THAT(UpdateMulticastRouteEntries(std::vector<P4RouteEntry>{
                  new_route_entry_ipv6, new_route_entry_ipv4}),
              ArrayEq(std::vector<StatusCode>{StatusCode::SWSS_RC_SUCCESS,
                                              StatusCode::SWSS_RC_SUCCESS}));

  // Expect entries to be associated with correct multicast group.
  auto* route_entry_ptr_v6 = GetRouteEntry(route_entry_ipv6.route_entry_key);
  auto* route_entry_ptr_v4 = GetRouteEntry(route_entry_ipv4.route_entry_key);
  EXPECT_NE(route_entry_ptr_v6, nullptr);
  EXPECT_NE(route_entry_ptr_v4, nullptr);

  uint32_t group1_ref_cnt = 777;
  uint32_t group2_ref_cnt = 777;
  uint32_t group3_ref_cnt = 777;
  uint32_t group4_ref_cnt = 777;

  EXPECT_TRUE(p4_oid_mapper_.getRefCount(SAI_OBJECT_TYPE_IPMC_GROUP,
                                         kMulticastGroupId1, &group1_ref_cnt));
  EXPECT_TRUE(p4_oid_mapper_.getRefCount(SAI_OBJECT_TYPE_IPMC_GROUP,
                                         kMulticastGroupId2, &group2_ref_cnt));
  EXPECT_TRUE(p4_oid_mapper_.getRefCount(SAI_OBJECT_TYPE_IPMC_GROUP,
                                         kMulticastGroupId3, &group3_ref_cnt));
  EXPECT_TRUE(p4_oid_mapper_.getRefCount(SAI_OBJECT_TYPE_IPMC_GROUP,
                                         kMulticastGroupId4, &group4_ref_cnt));
  EXPECT_EQ(group1_ref_cnt, 0);
  EXPECT_EQ(group2_ref_cnt, 0);
  EXPECT_EQ(group3_ref_cnt, 1);
  EXPECT_EQ(group4_ref_cnt, 1);
}

TEST_F(RouteManagerTest, UpdateMulticastRouteEntriesNoChangeSucceeds) {
  // We eventually want a prefix, not an exact match address.
  // TODO(b/281706130)
  auto swss_ipv6_route_prefix = swss::IpPrefix(kIpv6Prefix3);
  auto route_entry_ipv6 = SetupSetMulticastGroupIdRouteEntry(
      gVrfName, swss_ipv6_route_prefix, kMulticastGroupId1, kMulticastGroupOid1,
      kMetadata1);
  auto swss_ipv4_route_prefix = swss::IpPrefix(kIpv4Prefix4);
  auto route_entry_ipv4 = SetupSetMulticastGroupIdRouteEntry(
      gVrfName, swss_ipv4_route_prefix, kMulticastGroupId2, kMulticastGroupOid2,
      kMetadata1, /*expect_rpf=*/false);

  // Now update those entries, but with no changes.
  EXPECT_THAT(UpdateMulticastRouteEntries(std::vector<P4RouteEntry>{
                  route_entry_ipv6, route_entry_ipv4}),
              ArrayEq(std::vector<StatusCode>{StatusCode::SWSS_RC_SUCCESS,
                                              StatusCode::SWSS_RC_SUCCESS}));

  // Expect entries to be associated with correct multicast group.
  auto* route_entry_ptr_v6 = GetRouteEntry(route_entry_ipv6.route_entry_key);
  auto* route_entry_ptr_v4 = GetRouteEntry(route_entry_ipv4.route_entry_key);
  EXPECT_NE(route_entry_ptr_v6, nullptr);
  EXPECT_NE(route_entry_ptr_v4, nullptr);

  uint32_t group1_ref_cnt = 777;
  uint32_t group2_ref_cnt = 777;

  EXPECT_TRUE(p4_oid_mapper_.getRefCount(SAI_OBJECT_TYPE_IPMC_GROUP,
                                         kMulticastGroupId1, &group1_ref_cnt));
  EXPECT_TRUE(p4_oid_mapper_.getRefCount(SAI_OBJECT_TYPE_IPMC_GROUP,
                                         kMulticastGroupId2, &group2_ref_cnt));
  EXPECT_EQ(group1_ref_cnt, 1);
  EXPECT_EQ(group2_ref_cnt, 1);
}

TEST_F(RouteManagerTest, UpdateMulticastRouteEntriesActionChangeFails) {
  // We eventually want a prefix, not an exact match address.
  // TODO(b/281706130)
  auto swss_ipv6_route_prefix = swss::IpPrefix(kIpv6Prefix3);
  auto route_entry_ipv6 = SetupSetMulticastGroupIdRouteEntry(
      gVrfName, swss_ipv6_route_prefix, kMulticastGroupId1, kMulticastGroupOid1,
      kMetadata1);
  auto swss_ipv4_route_prefix = swss::IpPrefix(kIpv4Prefix4);
  auto route_entry_ipv4 = SetupSetMulticastGroupIdRouteEntry(
      gVrfName, swss_ipv4_route_prefix, kMulticastGroupId2, kMulticastGroupOid2,
      kMetadata1, /*expect_rpf=*/false);

  // Now update those entries to point to new multicast groups.
  auto new_route_entry_ipv6 = GenerateP4RouteEntry(
      gVrfName, swss_ipv6_route_prefix, p4orch::kDrop, "", kMetadata1);
  auto new_route_entry_ipv4 = GenerateP4RouteEntry(
      gVrfName, swss_ipv4_route_prefix, p4orch::kSetMulticastGroupId,
      kMulticastGroupId4, kMetadata1);
  // Create fake multicast group OIDs.
  AddMulticastGroup(kMulticastGroupId4, kMulticastGroupOid4);

  EXPECT_THAT(
      UpdateMulticastRouteEntries(std::vector<P4RouteEntry>{
          new_route_entry_ipv6, new_route_entry_ipv4}),
      ArrayEq(std::vector<StatusCode>{StatusCode::SWSS_RC_UNIMPLEMENTED,
                                      StatusCode::SWSS_RC_NOT_EXECUTED}));

  // Expect entries to be associated with first multicast groups.
  auto* route_entry_ptr_v6 = GetRouteEntry(route_entry_ipv6.route_entry_key);
  auto* route_entry_ptr_v4 = GetRouteEntry(route_entry_ipv4.route_entry_key);
  EXPECT_NE(route_entry_ptr_v6, nullptr);
  EXPECT_NE(route_entry_ptr_v4, nullptr);

  uint32_t group1_ref_cnt = 777;
  uint32_t group2_ref_cnt = 777;
  uint32_t group4_ref_cnt = 777;

  EXPECT_TRUE(p4_oid_mapper_.getRefCount(SAI_OBJECT_TYPE_IPMC_GROUP,
                                         kMulticastGroupId1, &group1_ref_cnt));
  EXPECT_TRUE(p4_oid_mapper_.getRefCount(SAI_OBJECT_TYPE_IPMC_GROUP,
                                         kMulticastGroupId2, &group2_ref_cnt));
  EXPECT_TRUE(p4_oid_mapper_.getRefCount(SAI_OBJECT_TYPE_IPMC_GROUP,
                                         kMulticastGroupId4, &group4_ref_cnt));
  EXPECT_EQ(group1_ref_cnt, 1);
  EXPECT_EQ(group2_ref_cnt, 1);
  EXPECT_EQ(group4_ref_cnt, 0);
}

TEST_F(RouteManagerTest, UpdateRouteEntriesActionChangeToMulticastFails) {
  auto swss_ipv4_route_prefix = swss::IpPrefix(kIpv4Prefix);
  sai_ip_prefix_t sai_ipv4_route_prefix;
  copy(sai_ipv4_route_prefix, swss_ipv4_route_prefix);
  SetupNexthopIdRouteEntry(gVrfName, swss_ipv4_route_prefix, kNexthopId1,
                           kNexthopOid1, kMetadata1);

  auto new_route_entry = GenerateP4RouteEntry(gVrfName, swss_ipv4_route_prefix,
                                              p4orch::kSetMulticastGroupId,
                                              kMulticastGroupId1, kMetadata1);

  // Create artificial multicast group object.
  AddMulticastGroup(kMulticastGroupId1, kMulticastGroupOid1);

  // Check that we fail to change action to setMulticastGroupId.
  EXPECT_THAT(
      UpdateRouteEntries(std::vector<P4RouteEntry>{new_route_entry}),
      ArrayEq(std::vector<StatusCode>{StatusCode::SWSS_RC_UNIMPLEMENTED}));
}

TEST_F(RouteManagerTest, UpdateMulticastRouteEntriesNoMulticastGroupFails) {
  // We eventually want a prefix, not an exact match address.
  // TODO(b/281706130)
  auto swss_ipv6_route_prefix = swss::IpPrefix(kIpv6Prefix3);
  auto route_entry_ipv6 = SetupSetMulticastGroupIdRouteEntry(
      gVrfName, swss_ipv6_route_prefix, kMulticastGroupId1, kMulticastGroupOid1,
      kMetadata1);
  auto swss_ipv4_route_prefix = swss::IpPrefix(kIpv4Prefix4);
  auto route_entry_ipv4 = SetupSetMulticastGroupIdRouteEntry(
      gVrfName, swss_ipv4_route_prefix, kMulticastGroupId2, kMulticastGroupOid2,
      kMetadata1, /*expect_rpf=*/false);

  // Now update those entries to point to new multicast groups.
  auto new_route_entry_ipv6 = GenerateP4RouteEntry(
      gVrfName, swss_ipv6_route_prefix, p4orch::kSetMulticastGroupId,
      kMulticastGroupId3, kMetadata1);
  auto new_route_entry_ipv4 = GenerateP4RouteEntry(
      gVrfName, swss_ipv4_route_prefix, p4orch::kSetMulticastGroupId,
      kMulticastGroupId4, kMetadata1);
  // Updated multicast group is missing.
  AddMulticastGroup(kMulticastGroupId4, kMulticastGroupOid4);

  EXPECT_THAT(
      UpdateMulticastRouteEntries(std::vector<P4RouteEntry>{
          new_route_entry_ipv6, new_route_entry_ipv4}),
      ArrayEq(std::vector<StatusCode>{StatusCode::SWSS_RC_NOT_FOUND,
                                      StatusCode::SWSS_RC_NOT_EXECUTED}));

  // Expect entries to be associated with first multicast groups.
  auto* route_entry_ptr_v6 = GetRouteEntry(route_entry_ipv6.route_entry_key);
  auto* route_entry_ptr_v4 = GetRouteEntry(route_entry_ipv4.route_entry_key);
  EXPECT_NE(route_entry_ptr_v6, nullptr);
  EXPECT_NE(route_entry_ptr_v4, nullptr);

  uint32_t group1_ref_cnt = 777;
  uint32_t group2_ref_cnt = 777;
  uint32_t group3_ref_cnt = 777;
  uint32_t group4_ref_cnt = 777;

  EXPECT_TRUE(p4_oid_mapper_.getRefCount(SAI_OBJECT_TYPE_IPMC_GROUP,
                                         kMulticastGroupId1, &group1_ref_cnt));
  EXPECT_TRUE(p4_oid_mapper_.getRefCount(SAI_OBJECT_TYPE_IPMC_GROUP,
                                         kMulticastGroupId2, &group2_ref_cnt));
  EXPECT_FALSE(p4_oid_mapper_.getRefCount(SAI_OBJECT_TYPE_IPMC_GROUP,
                                          kMulticastGroupId3, &group3_ref_cnt));
  EXPECT_TRUE(p4_oid_mapper_.getRefCount(SAI_OBJECT_TYPE_IPMC_GROUP,
                                         kMulticastGroupId4, &group4_ref_cnt));
  EXPECT_EQ(group1_ref_cnt, 1);
  EXPECT_EQ(group2_ref_cnt, 1);
  EXPECT_EQ(group4_ref_cnt, 0);
}

TEST_F(RouteManagerTest, UpdateMulticastRouteEntriesSaiFails) {
  // We eventually want a prefix, not an exact match address.
  // TODO(b/281706130)
  auto swss_ipv6_route_prefix = swss::IpPrefix(kIpv6Prefix3);
  auto route_entry_ipv6 = SetupSetMulticastGroupIdRouteEntry(
      gVrfName, swss_ipv6_route_prefix, kMulticastGroupId1, kMulticastGroupOid1,
      kMetadata1);
  auto swss_ipv4_route_prefix = swss::IpPrefix(kIpv4Prefix4);
  auto route_entry_ipv4 = SetupSetMulticastGroupIdRouteEntry(
      gVrfName, swss_ipv4_route_prefix, kMulticastGroupId2, kMulticastGroupOid2,
      kMetadata1, /*expect_rpf=*/false);

  // Now update those entries to point to new multicast groups.
  auto new_route_entry_ipv6 = GenerateP4RouteEntry(
      gVrfName, swss_ipv6_route_prefix, p4orch::kSetMulticastGroupId,
      kMulticastGroupId3, kMetadata1);
  auto new_route_entry_ipv4 = GenerateP4RouteEntry(
      gVrfName, swss_ipv4_route_prefix, p4orch::kSetMulticastGroupId,
      kMulticastGroupId4, kMetadata1);
  // Create fake multicast group OIDs.
  AddMulticastGroup(kMulticastGroupId3, kMulticastGroupOid3);
  AddMulticastGroup(kMulticastGroupId4, kMulticastGroupOid4);

  EXPECT_CALL(mock_sai_ipmc_, set_ipmc_entry_attribute(_, _))
      .WillOnce(Return(SAI_STATUS_FAILURE));
  EXPECT_THAT(
      UpdateMulticastRouteEntries(std::vector<P4RouteEntry>{
          new_route_entry_ipv6, new_route_entry_ipv4}),
      ArrayEq(std::vector<StatusCode>{StatusCode::SWSS_RC_UNKNOWN,
                                      StatusCode::SWSS_RC_NOT_EXECUTED}));

  // Expect entries to be associated with first multicast groups.
  auto* route_entry_ptr_v6 = GetRouteEntry(route_entry_ipv6.route_entry_key);
  auto* route_entry_ptr_v4 = GetRouteEntry(route_entry_ipv4.route_entry_key);
  EXPECT_NE(route_entry_ptr_v6, nullptr);
  EXPECT_NE(route_entry_ptr_v4, nullptr);

  uint32_t group1_ref_cnt = 777;
  uint32_t group2_ref_cnt = 777;
  uint32_t group3_ref_cnt = 777;
  uint32_t group4_ref_cnt = 777;

  EXPECT_TRUE(p4_oid_mapper_.getRefCount(SAI_OBJECT_TYPE_IPMC_GROUP,
                                         kMulticastGroupId1, &group1_ref_cnt));
  EXPECT_TRUE(p4_oid_mapper_.getRefCount(SAI_OBJECT_TYPE_IPMC_GROUP,
                                         kMulticastGroupId2, &group2_ref_cnt));
  EXPECT_TRUE(p4_oid_mapper_.getRefCount(SAI_OBJECT_TYPE_IPMC_GROUP,
                                         kMulticastGroupId3, &group3_ref_cnt));
  EXPECT_TRUE(p4_oid_mapper_.getRefCount(SAI_OBJECT_TYPE_IPMC_GROUP,
                                         kMulticastGroupId4, &group4_ref_cnt));
  EXPECT_EQ(group1_ref_cnt, 1);
  EXPECT_EQ(group2_ref_cnt, 1);
  EXPECT_EQ(group3_ref_cnt, 0);
  EXPECT_EQ(group4_ref_cnt, 0);
}

TEST_F(RouteManagerTest, UpdateMulticastRouteEntriesNoPreviousEntryFails) {
  // We eventually want a prefix, not an exact match address.
  // TODO(b/281706130)
  // auto swss_ipv6_route_prefix = swss::IpPrefix(kIpv6Prefix);
  // sai_ip_prefix_t sai_ipv6_route_prefix;
  // copy(sai_ipv6_route_prefix, swss_ipv6_route_prefix);
  auto swss_ipv6_route_prefix = swss::IpPrefix(kIpv6Prefix3);
  auto route_entry_ipv6 = GenerateP4RouteEntry(gVrfName, swss_ipv6_route_prefix,
                                               p4orch::kSetMulticastGroupId,
                                               kMulticastGroupId1, kMetadata1);
  auto swss_ipv4_route_prefix = swss::IpPrefix(kIpv4Prefix4);
  auto route_entry_ipv4 = GenerateP4RouteEntry(gVrfName, swss_ipv4_route_prefix,
                                               p4orch::kSetMulticastGroupId,
                                               kMulticastGroupId2, kMetadata1);

  // Do not create multicast groups or add entries.

  // Now update those entries to point to new multicast groups.
  auto new_route_entry_ipv6 = GenerateP4RouteEntry(
      gVrfName, swss_ipv6_route_prefix, p4orch::kSetMulticastGroupId,
      kMulticastGroupId3, kMetadata1);
  auto new_route_entry_ipv4 = GenerateP4RouteEntry(
      gVrfName, swss_ipv4_route_prefix, p4orch::kSetMulticastGroupId,
      kMulticastGroupId4, kMetadata1);
  // Create fake multicast group OIDs.
  AddMulticastGroup(kMulticastGroupId3, kMulticastGroupOid3);
  AddMulticastGroup(kMulticastGroupId4, kMulticastGroupOid4);

  EXPECT_THAT(
      UpdateMulticastRouteEntries(std::vector<P4RouteEntry>{
          new_route_entry_ipv6, new_route_entry_ipv4}),
      ArrayEq(std::vector<StatusCode>{StatusCode::SWSS_RC_INTERNAL,
                                      StatusCode::SWSS_RC_NOT_EXECUTED}));

  // Expect entries to still not exist.
  auto* route_entry_ptr_v6 = GetRouteEntry(route_entry_ipv6.route_entry_key);
  auto* route_entry_ptr_v4 = GetRouteEntry(route_entry_ipv4.route_entry_key);
  EXPECT_EQ(route_entry_ptr_v6, nullptr);
  EXPECT_EQ(route_entry_ptr_v4, nullptr);
}

TEST_F(RouteManagerTest, DeleteRouteEntryWithSaiErrorShouldFail)
{
    auto swss_ipv4_route_prefix = swss::IpPrefix(kIpv4Prefix);
    SetupNexthopIdRouteEntry(gVrfName, swss_ipv4_route_prefix, kNexthopId1, kNexthopOid1);

    std::vector<sai_status_t> exp_status{SAI_STATUS_FAILURE};
    EXPECT_CALL(mock_sai_route_, remove_route_entries(_, _, _, _))
        .WillOnce(DoAll(SetArrayArgument<3>(exp_status.begin(), exp_status.end()), Return(SAI_STATUS_FAILURE)));
    auto route_entry = GenerateP4RouteEntry(gVrfName, swss_ipv4_route_prefix, "", "");
    EXPECT_THAT(DeleteRouteEntries(std::vector<P4RouteEntry>{route_entry}),
                ArrayEq(std::vector<StatusCode>{StatusCode::SWSS_RC_UNKNOWN}));
}

TEST_F(RouteManagerTest, DeleteIpv4RouteSucceeds)
{
    auto swss_ipv4_route_prefix = swss::IpPrefix(kIpv4Prefix);
    sai_ip_prefix_t sai_ipv4_route_prefix;
    copy(sai_ipv4_route_prefix, swss_ipv4_route_prefix);
    SetupNexthopIdRouteEntry(gVrfName, swss_ipv4_route_prefix, kNexthopId1, kNexthopOid1);

    sai_route_entry_t exp_sai_route_entry;
    exp_sai_route_entry.switch_id = gSwitchId;
    exp_sai_route_entry.vr_id = gVrfOid;
    exp_sai_route_entry.destination = sai_ipv4_route_prefix;

    std::vector<sai_status_t> exp_status{SAI_STATUS_SUCCESS};
    EXPECT_CALL(mock_sai_route_,
                remove_route_entries(Eq(1), RouteEntryArrayEq(std::vector<sai_route_entry_t>{exp_sai_route_entry}),
                                     Eq(SAI_BULK_OP_ERROR_MODE_IGNORE_ERROR), _))
        .WillOnce(DoAll(SetArrayArgument<3>(exp_status.begin(), exp_status.end()), Return(SAI_STATUS_SUCCESS)));
    auto route_entry = GenerateP4RouteEntry(gVrfName, swss_ipv4_route_prefix, "", "");
    EXPECT_THAT(DeleteRouteEntries(std::vector<P4RouteEntry>{route_entry}),
                ArrayEq(std::vector<StatusCode>{StatusCode::SWSS_RC_SUCCESS}));
    auto *route_entry_ptr = GetRouteEntry(route_entry.route_entry_key);
    EXPECT_EQ(nullptr, route_entry_ptr);
    EXPECT_FALSE(p4_oid_mapper_.existsOID(SAI_OBJECT_TYPE_ROUTE_ENTRY, route_entry.route_entry_key));
    uint32_t ref_cnt;
    EXPECT_TRUE(
        p4_oid_mapper_.getRefCount(SAI_OBJECT_TYPE_NEXT_HOP, KeyGenerator::generateNextHopKey(kNexthopId1), &ref_cnt));
    EXPECT_EQ(0, ref_cnt);
}

TEST_F(RouteManagerTest, DeleteIpv6RouteSucceeds)
{
    auto swss_ipv6_route_prefix = swss::IpPrefix(kIpv6Prefix);
    sai_ip_prefix_t sai_ipv6_route_prefix;
    copy(sai_ipv6_route_prefix, swss_ipv6_route_prefix);
    SetupWcmpGroupRouteEntry(gVrfName, swss_ipv6_route_prefix, kWcmpGroup1, kWcmpGroupOid1);

    sai_route_entry_t exp_sai_route_entry;
    exp_sai_route_entry.switch_id = gSwitchId;
    exp_sai_route_entry.vr_id = gVrfOid;
    exp_sai_route_entry.destination = sai_ipv6_route_prefix;

    std::vector<sai_status_t> exp_status{SAI_STATUS_SUCCESS};
    EXPECT_CALL(mock_sai_route_,
                remove_route_entries(Eq(1), RouteEntryArrayEq(std::vector<sai_route_entry_t>{exp_sai_route_entry}),
                                     Eq(SAI_BULK_OP_ERROR_MODE_IGNORE_ERROR), _))
        .WillOnce(DoAll(SetArrayArgument<3>(exp_status.begin(), exp_status.end()), Return(SAI_STATUS_SUCCESS)));
    auto route_entry = GenerateP4RouteEntry(gVrfName, swss_ipv6_route_prefix, "", "");
    EXPECT_THAT(DeleteRouteEntries(std::vector<P4RouteEntry>{route_entry}),
                ArrayEq(std::vector<StatusCode>{StatusCode::SWSS_RC_SUCCESS}));
    auto *route_entry_ptr = GetRouteEntry(route_entry.route_entry_key);
    EXPECT_EQ(nullptr, route_entry_ptr);
    EXPECT_FALSE(p4_oid_mapper_.existsOID(SAI_OBJECT_TYPE_ROUTE_ENTRY, route_entry.route_entry_key));
    uint32_t ref_cnt;
    EXPECT_TRUE(p4_oid_mapper_.getRefCount(SAI_OBJECT_TYPE_NEXT_HOP_GROUP,
                                           KeyGenerator::generateWcmpGroupKey(kWcmpGroup1), &ref_cnt));
    EXPECT_EQ(0, ref_cnt);
}

TEST_F(RouteManagerTest, DeleteMulticastRouteEntriesSucceeds) {
  // We eventually want a prefix, not an exact match address.
  // TODO(b/281706130)
  // Setup two entries to delete.
  auto swss_ipv6_route_prefix = swss::IpPrefix(kIpv6Prefix3);
  auto route_entry_ipv6 = SetupSetMulticastGroupIdRouteEntry(
      gVrfName, swss_ipv6_route_prefix, kMulticastGroupId1, kMulticastGroupOid1,
      kMetadata1);
  auto swss_ipv4_route_prefix = swss::IpPrefix(kIpv4Prefix4);
  auto route_entry_ipv4 = SetupSetMulticastGroupIdRouteEntry(
      gVrfName, swss_ipv4_route_prefix, kMulticastGroupId2, kMulticastGroupOid2,
      kMetadata1, /*expect_rpf=*/false);

  // Now delete those entries.
  EXPECT_CALL(mock_sai_ipmc_,
              remove_ipmc_entry(_))
      .WillOnce(Return(SAI_STATUS_SUCCESS))
      .WillOnce(Return(SAI_STATUS_SUCCESS));
  EXPECT_THAT(
      DeleteMulticastRouteEntries(
          std::vector<P4RouteEntry>{route_entry_ipv6, route_entry_ipv4}),
          ArrayEq(std::vector<StatusCode>{StatusCode::SWSS_RC_SUCCESS,
                                          StatusCode::SWSS_RC_SUCCESS}));

  // Expect entries to not be seen anymore.
  auto* route_entry_ptr_v6 = GetRouteEntry(route_entry_ipv6.route_entry_key);
  auto* route_entry_ptr_v4 = GetRouteEntry(route_entry_ipv4.route_entry_key);
  EXPECT_EQ(route_entry_ptr_v6, nullptr);
  EXPECT_EQ(route_entry_ptr_v4, nullptr);

  EXPECT_FALSE(p4_oid_mapper_.existsOID(SAI_OBJECT_TYPE_IPMC_ENTRY,
                                        route_entry_ipv6.route_entry_key));
  EXPECT_FALSE(p4_oid_mapper_.existsOID(SAI_OBJECT_TYPE_IPMC_ENTRY,
                                        route_entry_ipv4.route_entry_key));
}

TEST_F(RouteManagerTest, DeleteMulticastRouteEntriesSaiFailure) {
  // We eventually want a prefix, not an exact match address.
  // TODO(b/281706130)
  // Setup two entries to delete.
  auto swss_ipv6_route_prefix = swss::IpPrefix(kIpv6Prefix3);
  auto route_entry_ipv6 = SetupSetMulticastGroupIdRouteEntry(
      gVrfName, swss_ipv6_route_prefix, kMulticastGroupId1, kMulticastGroupOid1,
      kMetadata1);
  auto swss_ipv4_route_prefix = swss::IpPrefix(kIpv4Prefix4);
  auto route_entry_ipv4 = SetupSetMulticastGroupIdRouteEntry(
      gVrfName, swss_ipv4_route_prefix, kMulticastGroupId2, kMulticastGroupOid2,
      kMetadata1, /*expect_rpf=*/false);

  // Now delete those entries, force a failure.
  EXPECT_CALL(mock_sai_ipmc_,
              remove_ipmc_entry(_))
      .WillOnce(Return(SAI_STATUS_FAILURE));
  EXPECT_THAT(
      DeleteMulticastRouteEntries(
          std::vector<P4RouteEntry>{route_entry_ipv6, route_entry_ipv4}),
          ArrayEq(std::vector<StatusCode>{StatusCode::SWSS_RC_UNKNOWN,
                                          StatusCode::SWSS_RC_NOT_EXECUTED}));

  // Since operation failed, expect entries to still be there.
  auto* route_entry_ptr_v6 = GetRouteEntry(route_entry_ipv6.route_entry_key);
  auto* route_entry_ptr_v4 = GetRouteEntry(route_entry_ipv4.route_entry_key);
  EXPECT_NE(route_entry_ptr_v6, nullptr);
  EXPECT_NE(route_entry_ptr_v4, nullptr);

  EXPECT_TRUE(p4_oid_mapper_.existsOID(SAI_OBJECT_TYPE_IPMC_ENTRY,
                                       route_entry_ipv6.route_entry_key));
  EXPECT_TRUE(p4_oid_mapper_.existsOID(SAI_OBJECT_TYPE_IPMC_ENTRY,
                                       route_entry_ipv4.route_entry_key));
}

TEST_F(RouteManagerTest, RouteCreateAndUpdateInDrainSucceeds)
{
    auto swss_ipv4_route_prefix = swss::IpPrefix(kIpv4Prefix);
    p4_oid_mapper_.setOID(SAI_OBJECT_TYPE_NEXT_HOP, KeyGenerator::generateNextHopKey(kNexthopId1), kNexthopOid1);
    auto key_op_fvs_1 = GenerateKeyOpFieldsValuesTuple(gVrfName, swss_ipv4_route_prefix, SET_COMMAND,
                                                       p4orch::kSetNexthopId, kNexthopId1);
    Enqueue(APP_P4RT_IPV4_TABLE_NAME, key_op_fvs_1);
    std::vector<sai_status_t> exp_status{SAI_STATUS_SUCCESS};
    EXPECT_CALL(mock_sai_route_, create_route_entries(_, _, _, _, _, _))
        .WillOnce(DoAll(SetArrayArgument<5>(exp_status.begin(), exp_status.end()), Return(SAI_STATUS_SUCCESS)));
    EXPECT_CALL(publisher_, publish(Eq(APP_P4RT_TABLE_NAME), Eq(kfvKey(key_op_fvs_1)),
                                    FieldValueTupleArrayEq(kfvFieldsValues(key_op_fvs_1)),
                                    Eq(StatusCode::SWSS_RC_SUCCESS), Eq(true)))
        .Times(1);
    EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS, Drain(/*failure_before=*/false));

    p4_oid_mapper_.setOID(SAI_OBJECT_TYPE_NEXT_HOP, KeyGenerator::generateNextHopKey(kNexthopId2), kNexthopOid2);
    auto key_op_fvs_2 = GenerateKeyOpFieldsValuesTuple(gVrfName, swss_ipv4_route_prefix, SET_COMMAND,
                                                       p4orch::kSetNexthopId, kNexthopId2);
    Enqueue(APP_P4RT_IPV4_TABLE_NAME, key_op_fvs_2);
    EXPECT_CALL(mock_sai_route_, set_route_entries_attribute(_, _, _, _, _))
        .WillOnce(DoAll(SetArrayArgument<4>(exp_status.begin(), exp_status.end()), Return(SAI_STATUS_SUCCESS)));
    EXPECT_CALL(publisher_, publish(Eq(APP_P4RT_TABLE_NAME), Eq(kfvKey(key_op_fvs_2)),
                                    FieldValueTupleArrayEq(kfvFieldsValues(key_op_fvs_2)),
                                    Eq(StatusCode::SWSS_RC_SUCCESS), Eq(true)))
        .Times(1);
    EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS, Drain(/*failure_before=*/false));

    auto route_entry = GenerateP4RouteEntry(gVrfName, swss_ipv4_route_prefix, p4orch::kSetNexthopId, kNexthopId2);
    sai_ip_prefix_t sai_ipv4_route_prefix;
    copy(sai_ipv4_route_prefix, swss_ipv4_route_prefix);
    VerifyRouteEntry(route_entry, sai_ipv4_route_prefix, gVrfOid);
    uint32_t ref_cnt;
    EXPECT_TRUE(
        p4_oid_mapper_.getRefCount(SAI_OBJECT_TYPE_NEXT_HOP, KeyGenerator::generateNextHopKey(kNexthopId1), &ref_cnt));
    EXPECT_EQ(0, ref_cnt);
    EXPECT_TRUE(
        p4_oid_mapper_.getRefCount(SAI_OBJECT_TYPE_NEXT_HOP, KeyGenerator::generateNextHopKey(kNexthopId2), &ref_cnt));
    EXPECT_EQ(1, ref_cnt);

    auto key_op_fvs_3 = GenerateKeyOpFieldsValuesTuple(gVrfName, swss_ipv4_route_prefix, SET_COMMAND,
                                                       p4orch::kSetMetadataAndDrop, "", kMetadata1);
    Enqueue(APP_P4RT_IPV4_TABLE_NAME, key_op_fvs_3);
    EXPECT_CALL(mock_sai_route_, set_route_entries_attribute(_, _, _, _, _))
        .WillRepeatedly(DoAll(SetArrayArgument<4>(exp_status.begin(), exp_status.end()), Return(SAI_STATUS_SUCCESS)));
    EXPECT_CALL(publisher_, publish(Eq(APP_P4RT_TABLE_NAME), Eq(kfvKey(key_op_fvs_3)),
                                    FieldValueTupleArrayEq(kfvFieldsValues(key_op_fvs_3)),
                                    Eq(StatusCode::SWSS_RC_SUCCESS), Eq(true)))
        .Times(1);
    EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS, Drain(/*failure_before=*/false));

    route_entry = GenerateP4RouteEntry(gVrfName, swss_ipv4_route_prefix, p4orch::kSetMetadataAndDrop, "", kMetadata1);
    VerifyRouteEntry(route_entry, sai_ipv4_route_prefix, gVrfOid);
    EXPECT_TRUE(
        p4_oid_mapper_.getRefCount(SAI_OBJECT_TYPE_NEXT_HOP, KeyGenerator::generateNextHopKey(kNexthopId1), &ref_cnt));
    EXPECT_EQ(0, ref_cnt);
    EXPECT_TRUE(
        p4_oid_mapper_.getRefCount(SAI_OBJECT_TYPE_NEXT_HOP, KeyGenerator::generateNextHopKey(kNexthopId2), &ref_cnt));
    EXPECT_EQ(0, ref_cnt);
}

TEST_F(RouteManagerTest, RouteCreateAndDeleteInDrainSucceeds)
{
    auto swss_ipv4_route_prefix = swss::IpPrefix(kIpv4Prefix);
    p4_oid_mapper_.setOID(SAI_OBJECT_TYPE_NEXT_HOP, KeyGenerator::generateNextHopKey(kNexthopId1), kNexthopOid1);
    auto key_op_fvs_1 = GenerateKeyOpFieldsValuesTuple(gVrfName, swss_ipv4_route_prefix, SET_COMMAND,
                                                       p4orch::kSetNexthopId, kNexthopId1);
    Enqueue(APP_P4RT_IPV4_TABLE_NAME, key_op_fvs_1);
    std::vector<sai_status_t> exp_status{SAI_STATUS_SUCCESS};
    EXPECT_CALL(mock_sai_route_, create_route_entries(_, _, _, _, _, _))
        .WillOnce(DoAll(SetArrayArgument<5>(exp_status.begin(), exp_status.end()), Return(SAI_STATUS_SUCCESS)));
    EXPECT_CALL(publisher_, publish(Eq(APP_P4RT_TABLE_NAME), Eq(kfvKey(key_op_fvs_1)),
                                    FieldValueTupleArrayEq(kfvFieldsValues(key_op_fvs_1)),
                                    Eq(StatusCode::SWSS_RC_SUCCESS), Eq(true)))
        .Times(1);
    EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS, Drain(/*failure_before=*/false));

    auto key_op_fvs_2 = GenerateKeyOpFieldsValuesTuple(gVrfName, swss_ipv4_route_prefix, DEL_COMMAND, "", "");
    Enqueue(APP_P4RT_IPV4_TABLE_NAME, key_op_fvs_2);
    EXPECT_CALL(mock_sai_route_, remove_route_entries(_, _, _, _))
        .WillOnce(DoAll(SetArrayArgument<3>(exp_status.begin(), exp_status.end()), Return(SAI_STATUS_SUCCESS)));
    EXPECT_CALL(publisher_, publish(Eq(APP_P4RT_TABLE_NAME), Eq(kfvKey(key_op_fvs_2)),
                                    FieldValueTupleArrayEq(kfvFieldsValues(key_op_fvs_2)),
                                    Eq(StatusCode::SWSS_RC_SUCCESS), Eq(true)))
        .Times(1);
    EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS, Drain(/*failure_before=*/false));

    std::string key = KeyGenerator::generateRouteKey(gVrfName, swss_ipv4_route_prefix);
    auto *route_entry_ptr = GetRouteEntry(key);
    EXPECT_EQ(nullptr, route_entry_ptr);
    EXPECT_FALSE(p4_oid_mapper_.existsOID(SAI_OBJECT_TYPE_ROUTE_ENTRY, key));
    uint32_t ref_cnt;
    EXPECT_TRUE(
        p4_oid_mapper_.getRefCount(SAI_OBJECT_TYPE_NEXT_HOP, KeyGenerator::generateNextHopKey(kNexthopId1), &ref_cnt));
    EXPECT_EQ(0, ref_cnt);
}

TEST_F(RouteManagerTest, UpdateFailsWhenCreateAndUpdateTheSameRouteInDrain)
{
    p4_oid_mapper_.setOID(SAI_OBJECT_TYPE_NEXT_HOP, KeyGenerator::generateNextHopKey(kNexthopId1), kNexthopOid1);
    p4_oid_mapper_.setOID(SAI_OBJECT_TYPE_NEXT_HOP, KeyGenerator::generateNextHopKey(kNexthopId2), kNexthopOid2);
    auto key_op_fvs_1 = GenerateKeyOpFieldsValuesTuple(gVrfName, swss::IpPrefix(kIpv4Prefix), SET_COMMAND,
                                                       p4orch::kSetNexthopId, kNexthopId1);
    Enqueue(APP_P4RT_IPV4_TABLE_NAME, key_op_fvs_1);
    auto key_op_fvs_2 = GenerateKeyOpFieldsValuesTuple(gVrfName, swss::IpPrefix(kIpv4Prefix), SET_COMMAND,
                                                       p4orch::kSetNexthopId, kNexthopId2);
    Enqueue(APP_P4RT_IPV4_TABLE_NAME, key_op_fvs_2);

    std::vector<sai_status_t> exp_status{SAI_STATUS_SUCCESS};
    EXPECT_CALL(mock_sai_route_, create_route_entries(_, _, _, _, _, _))
        .WillOnce(DoAll(SetArrayArgument<5>(exp_status.begin(), exp_status.end()), Return(SAI_STATUS_SUCCESS)));
    EXPECT_CALL(publisher_, publish(Eq(APP_P4RT_TABLE_NAME), Eq(kfvKey(key_op_fvs_1)),
                                    FieldValueTupleArrayEq(kfvFieldsValues(key_op_fvs_1)),
                                    Eq(StatusCode::SWSS_RC_SUCCESS), Eq(true)))
        .Times(1);
    EXPECT_CALL(publisher_, publish(Eq(APP_P4RT_TABLE_NAME), Eq(kfvKey(key_op_fvs_2)),
                                    FieldValueTupleArrayEq(kfvFieldsValues(key_op_fvs_2)),
                                    Eq(StatusCode::SWSS_RC_INVALID_PARAM), Eq(true)))
        .Times(1);

    EXPECT_EQ(StatusCode::SWSS_RC_INVALID_PARAM,
              Drain(/*failure_before=*/false));
    auto swss_ipv4_route_prefix = swss::IpPrefix(kIpv4Prefix);
    sai_ip_prefix_t sai_ipv4_route_prefix;
    copy(sai_ipv4_route_prefix, swss_ipv4_route_prefix);
    auto route_entry = GenerateP4RouteEntry(gVrfName, swss_ipv4_route_prefix, p4orch::kSetNexthopId, kNexthopId1);
    VerifyRouteEntry(route_entry, sai_ipv4_route_prefix, gVrfOid);
    uint32_t ref_cnt;
    EXPECT_TRUE(
        p4_oid_mapper_.getRefCount(SAI_OBJECT_TYPE_NEXT_HOP, KeyGenerator::generateNextHopKey(kNexthopId1), &ref_cnt));
    EXPECT_EQ(1, ref_cnt);
    EXPECT_TRUE(
        p4_oid_mapper_.getRefCount(SAI_OBJECT_TYPE_NEXT_HOP, KeyGenerator::generateNextHopKey(kNexthopId2), &ref_cnt));
    EXPECT_EQ(0, ref_cnt);
}

TEST_F(RouteManagerTest, DeleteFailsWhenCreateAndDeleteTheSameRouteInDrain)
{
    p4_oid_mapper_.setOID(SAI_OBJECT_TYPE_NEXT_HOP, KeyGenerator::generateNextHopKey(kNexthopId1), kNexthopOid1);
    auto key_op_fvs_1 = GenerateKeyOpFieldsValuesTuple(gVrfName, swss::IpPrefix(kIpv4Prefix), SET_COMMAND,
                                                       p4orch::kSetNexthopId, kNexthopId1);
    Enqueue(APP_P4RT_IPV4_TABLE_NAME, key_op_fvs_1);
    auto key_op_fvs_2 = GenerateKeyOpFieldsValuesTuple(gVrfName, swss::IpPrefix(kIpv4Prefix), DEL_COMMAND, "", "");
    Enqueue(APP_P4RT_IPV4_TABLE_NAME, key_op_fvs_2);

    std::vector<sai_status_t> exp_status{SAI_STATUS_SUCCESS};
    EXPECT_CALL(mock_sai_route_, create_route_entries(_, _, _, _, _, _))
        .WillOnce(DoAll(SetArrayArgument<5>(exp_status.begin(), exp_status.end()), Return(SAI_STATUS_SUCCESS)));
    EXPECT_CALL(publisher_, publish(Eq(APP_P4RT_TABLE_NAME), Eq(kfvKey(key_op_fvs_1)),
                                    FieldValueTupleArrayEq(kfvFieldsValues(key_op_fvs_1)),
                                    Eq(StatusCode::SWSS_RC_SUCCESS), Eq(true)))
        .Times(1);
    EXPECT_CALL(publisher_, publish(Eq(APP_P4RT_TABLE_NAME), Eq(kfvKey(key_op_fvs_2)),
                                    FieldValueTupleArrayEq(kfvFieldsValues(key_op_fvs_2)),
                                    Eq(StatusCode::SWSS_RC_INVALID_PARAM), Eq(true)))
        .Times(1);
    EXPECT_EQ(StatusCode::SWSS_RC_INVALID_PARAM,
              Drain(/*failure_before=*/false));
    auto swss_ipv4_route_prefix = swss::IpPrefix(kIpv4Prefix);
    sai_ip_prefix_t sai_ipv4_route_prefix;
    copy(sai_ipv4_route_prefix, swss_ipv4_route_prefix);
    auto route_entry = GenerateP4RouteEntry(gVrfName, swss_ipv4_route_prefix, p4orch::kSetNexthopId, kNexthopId1);
    VerifyRouteEntry(route_entry, sai_ipv4_route_prefix, gVrfOid);
    uint32_t ref_cnt;
    EXPECT_TRUE(
        p4_oid_mapper_.getRefCount(SAI_OBJECT_TYPE_NEXT_HOP, KeyGenerator::generateNextHopKey(kNexthopId1), &ref_cnt));
    EXPECT_EQ(1, ref_cnt);
}

TEST_F(RouteManagerTest, RouteCreateInDrainSucceedsWhenVrfIsEmpty)
{
    const std::string kDefaultVrfName = ""; // Default Vrf
    auto swss_ipv4_route_prefix = swss::IpPrefix(kIpv4Prefix);
    sai_ip_prefix_t sai_ipv4_route_prefix;
    copy(sai_ipv4_route_prefix, swss_ipv4_route_prefix);
    p4_oid_mapper_.setOID(SAI_OBJECT_TYPE_NEXT_HOP, KeyGenerator::generateNextHopKey(kNexthopId1), kNexthopOid1);
    auto key_op_fvs = GenerateKeyOpFieldsValuesTuple(kDefaultVrfName, swss::IpPrefix(kIpv4Prefix), SET_COMMAND,
                                                     p4orch::kSetNexthopId, kNexthopId1);
    Enqueue(APP_P4RT_IPV4_TABLE_NAME, key_op_fvs);

    sai_route_entry_t exp_sai_route_entry;
    exp_sai_route_entry.switch_id = gSwitchId;
    exp_sai_route_entry.vr_id = gVirtualRouterId;
    exp_sai_route_entry.destination = sai_ipv4_route_prefix;

    sai_attribute_t exp_sai_attr;
    exp_sai_attr.id = SAI_ROUTE_ENTRY_ATTR_NEXT_HOP_ID;
    exp_sai_attr.value.oid = kNexthopOid1;

    std::vector<sai_status_t> exp_status{SAI_STATUS_SUCCESS};
    EXPECT_CALL(mock_sai_route_,
                create_route_entries(Eq(1), RouteEntryArrayEq(std::vector<sai_route_entry_t>{exp_sai_route_entry}),
                                     ArrayEq(std::vector<uint32_t>{1}),
                                     AttrArrayArrayEq(std::vector<std::vector<sai_attribute_t>>{{exp_sai_attr}}),
                                     Eq(SAI_BULK_OP_ERROR_MODE_IGNORE_ERROR), _))
        .WillOnce(DoAll(SetArrayArgument<5>(exp_status.begin(), exp_status.end()), Return(SAI_STATUS_SUCCESS)));
    EXPECT_CALL(publisher_,
                publish(Eq(APP_P4RT_TABLE_NAME), Eq(kfvKey(key_op_fvs)),
                        FieldValueTupleArrayEq(kfvFieldsValues(key_op_fvs)), Eq(StatusCode::SWSS_RC_SUCCESS), Eq(true)))
        .Times(1);

    EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS, Drain(/*failure_before=*/false));
    std::string key = KeyGenerator::generateRouteKey(kDefaultVrfName, swss::IpPrefix(kIpv4Prefix));
    auto *route_entry_ptr = GetRouteEntry(key);
    EXPECT_NE(nullptr, route_entry_ptr);
    EXPECT_TRUE(p4_oid_mapper_.existsOID(SAI_OBJECT_TYPE_ROUTE_ENTRY, key));
    uint32_t ref_cnt;
    EXPECT_TRUE(
        p4_oid_mapper_.getRefCount(SAI_OBJECT_TYPE_NEXT_HOP, KeyGenerator::generateNextHopKey(kNexthopId1), &ref_cnt));
    EXPECT_EQ(1, ref_cnt);
}

TEST_F(RouteManagerTest, DeserializeRouteEntryInDrainFails)
{
    const std::string kKeyPrefix = std::string(APP_P4RT_IPV4_TABLE_NAME) + kTableKeyDelimiter;
    auto key_op_fvs =
        swss::KeyOpFieldsValuesTuple(kKeyPrefix + "{{{{{{{{{{{{", SET_COMMAND, std::vector<swss::FieldValueTuple>{});
    Enqueue(APP_P4RT_IPV4_TABLE_NAME, key_op_fvs);
    EXPECT_CALL(publisher_, publish(Eq(APP_P4RT_TABLE_NAME), Eq(kfvKey(key_op_fvs)),
                                    FieldValueTupleArrayEq(kfvFieldsValues(key_op_fvs)),
                                    Eq(StatusCode::SWSS_RC_INVALID_PARAM), Eq(true)))
        .Times(1);
    EXPECT_EQ(StatusCode::SWSS_RC_INVALID_PARAM,
              Drain(/*failure_before=*/false));
}

TEST_F(RouteManagerTest, ValidateRouteEntryInDrainFailsWhenVrfDoesNotExist)
{
    p4_oid_mapper_.setOID(SAI_OBJECT_TYPE_NEXT_HOP, KeyGenerator::generateNextHopKey(kNexthopId1), kNexthopOid1);
    auto key_op_fvs = GenerateKeyOpFieldsValuesTuple("Invalid-Vrf", swss::IpPrefix(kIpv4Prefix), SET_COMMAND,
                                                     p4orch::kSetNexthopId, kNexthopId1);
    Enqueue(APP_P4RT_IPV4_TABLE_NAME, key_op_fvs);
    EXPECT_CALL(publisher_, publish(Eq(APP_P4RT_TABLE_NAME), Eq(kfvKey(key_op_fvs)),
                                    FieldValueTupleArrayEq(kfvFieldsValues(key_op_fvs)),
                                    Eq(StatusCode::SWSS_RC_NOT_FOUND), Eq(true)))
        .Times(1);
    EXPECT_EQ(StatusCode::SWSS_RC_NOT_FOUND, Drain(/*failure_before=*/false));
}

TEST_F(RouteManagerTest, ValidateRouteEntryInDrainFailsWhenNexthopDoesNotExist)
{
    auto key_op_fvs = GenerateKeyOpFieldsValuesTuple(gVrfName, swss::IpPrefix(kIpv4Prefix), SET_COMMAND,
                                                     p4orch::kSetNexthopId, kNexthopId1);
    Enqueue(APP_P4RT_IPV4_TABLE_NAME, key_op_fvs);
    EXPECT_CALL(publisher_, publish(Eq(APP_P4RT_TABLE_NAME), Eq(kfvKey(key_op_fvs)),
                                    FieldValueTupleArrayEq(kfvFieldsValues(key_op_fvs)),
                                    Eq(StatusCode::SWSS_RC_NOT_FOUND), Eq(true)))
        .Times(1);
    EXPECT_EQ(StatusCode::SWSS_RC_NOT_FOUND, Drain(/*failure_before=*/false));
}

TEST_F(RouteManagerTest, InvalidateSetRouteEntryInDrainFails)
{
    p4_oid_mapper_.setOID(SAI_OBJECT_TYPE_NEXT_HOP, KeyGenerator::generateNextHopKey(kNexthopId1), kNexthopOid1);
    // No nexthop ID with kSetNexthopId action.
    auto key_op_fvs =
        GenerateKeyOpFieldsValuesTuple(gVrfName, swss::IpPrefix(kIpv4Prefix), SET_COMMAND, p4orch::kSetNexthopId, "");
    Enqueue(APP_P4RT_IPV4_TABLE_NAME, key_op_fvs);
    EXPECT_CALL(publisher_, publish(Eq(APP_P4RT_TABLE_NAME), Eq(kfvKey(key_op_fvs)),
                                    FieldValueTupleArrayEq(kfvFieldsValues(key_op_fvs)),
                                    Eq(StatusCode::SWSS_RC_INVALID_PARAM), Eq(true)))
        .Times(1);
    EXPECT_EQ(StatusCode::SWSS_RC_INVALID_PARAM,
              Drain(/*failure_before=*/false));
}

TEST_F(RouteManagerTest, InvalidateDelRouteEntryInDrainFails)
{
    // Route does not exist.
    auto key_op_fvs = GenerateKeyOpFieldsValuesTuple(gVrfName, swss::IpPrefix(kIpv4Prefix), DEL_COMMAND,
                                                     p4orch::kSetNexthopId, kNexthopId1);
    Enqueue(APP_P4RT_IPV4_TABLE_NAME, key_op_fvs);
    EXPECT_CALL(publisher_, publish(Eq(APP_P4RT_TABLE_NAME), Eq(kfvKey(key_op_fvs)),
                                    FieldValueTupleArrayEq(kfvFieldsValues(key_op_fvs)),
                                    Eq(StatusCode::SWSS_RC_NOT_FOUND), Eq(true)))
        .Times(1);
    EXPECT_EQ(StatusCode::SWSS_RC_NOT_FOUND, Drain(/*failure_before=*/false));
}

TEST_F(RouteManagerTest, InvalidCommandInDrainFails)
{
    const std::string kKeyPrefix = std::string(APP_P4RT_IPV4_TABLE_NAME) + kTableKeyDelimiter;
    p4_oid_mapper_.setOID(SAI_OBJECT_TYPE_NEXT_HOP, KeyGenerator::generateNextHopKey(kNexthopId1), kNexthopOid1);
    nlohmann::json j;
    j[prependMatchField(p4orch::kVrfId)] = gVrfName;
    j[prependMatchField(p4orch::kIpv4Dst)] = kIpv4Prefix;
    std::vector<swss::FieldValueTuple> attributes;
    attributes.push_back(swss::FieldValueTuple{p4orch::kAction, p4orch::kSetNexthopId});
    attributes.push_back(swss::FieldValueTuple{prependParamField(p4orch::kNexthopId), kNexthopId1});
    auto key_op_fvs = swss::KeyOpFieldsValuesTuple(kKeyPrefix + j.dump(), "INVALID_COMMAND", attributes);
    Enqueue(APP_P4RT_IPV4_TABLE_NAME, key_op_fvs);
    EXPECT_CALL(publisher_, publish(Eq(APP_P4RT_TABLE_NAME), Eq(kfvKey(key_op_fvs)),
                                    FieldValueTupleArrayEq(kfvFieldsValues(key_op_fvs)),
                                    Eq(StatusCode::SWSS_RC_INVALID_PARAM), Eq(true)))
        .Times(1);
    EXPECT_EQ(StatusCode::SWSS_RC_INVALID_PARAM,
              Drain(/*failure_before=*/false));
}

TEST_F(RouteManagerTest, BatchedCreateSucceeds)
{
    auto swss_ipv4_route_prefix = swss::IpPrefix(kIpv4Prefix);
    sai_ip_prefix_t sai_ipv4_route_prefix;
    copy(sai_ipv4_route_prefix, swss_ipv4_route_prefix);
    auto route_entry_ipv4 = GenerateP4RouteEntry(gVrfName, swss_ipv4_route_prefix, p4orch::kSetNexthopId, kNexthopId1);
    p4_oid_mapper_.setOID(SAI_OBJECT_TYPE_NEXT_HOP, KeyGenerator::generateNextHopKey(route_entry_ipv4.nexthop_id),
                          kNexthopOid1);

    auto swss_ipv6_route_prefix = swss::IpPrefix(kIpv6Prefix);
    sai_ip_prefix_t sai_ipv6_route_prefix;
    copy(sai_ipv6_route_prefix, swss_ipv6_route_prefix);
    auto route_entry_ipv6 =
        GenerateP4RouteEntry(gVrfName, swss_ipv6_route_prefix, p4orch::kSetWcmpGroupId, kWcmpGroup1);
    p4_oid_mapper_.setOID(SAI_OBJECT_TYPE_NEXT_HOP_GROUP,
                          KeyGenerator::generateWcmpGroupKey(route_entry_ipv6.wcmp_group), kWcmpGroupOid1);

    sai_route_entry_t exp_sai_route_entry_ipv4;
    exp_sai_route_entry_ipv4.switch_id = gSwitchId;
    exp_sai_route_entry_ipv4.vr_id = gVrfOid;
    exp_sai_route_entry_ipv4.destination = sai_ipv4_route_prefix;

    sai_attribute_t exp_sai_attr_ipv4;
    exp_sai_attr_ipv4.id = SAI_ROUTE_ENTRY_ATTR_NEXT_HOP_ID;
    exp_sai_attr_ipv4.value.oid = kNexthopOid1;

    sai_route_entry_t exp_sai_route_entry_ipv6;
    exp_sai_route_entry_ipv6.switch_id = gSwitchId;
    exp_sai_route_entry_ipv6.vr_id = gVrfOid;
    exp_sai_route_entry_ipv6.destination = sai_ipv6_route_prefix;

    sai_attribute_t exp_sai_attr_ipv6;
    exp_sai_attr_ipv6.id = SAI_ROUTE_ENTRY_ATTR_NEXT_HOP_ID;
    exp_sai_attr_ipv6.value.oid = kWcmpGroupOid1;

    std::vector<sai_status_t> exp_status{SAI_STATUS_SUCCESS, SAI_STATUS_SUCCESS};
    EXPECT_CALL(
        mock_sai_route_,
        create_route_entries(
            Eq(2),
            RouteEntryArrayEq(std::vector<sai_route_entry_t>{exp_sai_route_entry_ipv4, exp_sai_route_entry_ipv6}),
            ArrayEq(std::vector<uint32_t>{1, 1}),
            AttrArrayArrayEq(std::vector<std::vector<sai_attribute_t>>{{exp_sai_attr_ipv4}, {exp_sai_attr_ipv6}}),
            Eq(SAI_BULK_OP_ERROR_MODE_IGNORE_ERROR), _))
        .WillOnce(DoAll(SetArrayArgument<5>(exp_status.begin(), exp_status.end()), Return(SAI_STATUS_SUCCESS)));
    EXPECT_THAT(CreateRouteEntries(std::vector<P4RouteEntry>{route_entry_ipv4, route_entry_ipv6}),
                ArrayEq(std::vector<StatusCode>{StatusCode::SWSS_RC_SUCCESS, StatusCode::SWSS_RC_SUCCESS}));
    VerifyRouteEntry(route_entry_ipv4, sai_ipv4_route_prefix, gVrfOid);
    VerifyRouteEntry(route_entry_ipv6, sai_ipv6_route_prefix, gVrfOid);
    uint32_t ref_cnt;
    EXPECT_TRUE(
        p4_oid_mapper_.getRefCount(SAI_OBJECT_TYPE_NEXT_HOP, KeyGenerator::generateNextHopKey(kNexthopId1), &ref_cnt));
    EXPECT_EQ(1, ref_cnt);
    EXPECT_TRUE(p4_oid_mapper_.getRefCount(SAI_OBJECT_TYPE_NEXT_HOP_GROUP,
                                           KeyGenerator::generateWcmpGroupKey(kWcmpGroup1), &ref_cnt));
    EXPECT_EQ(1, ref_cnt);
}

TEST_F(RouteManagerTest, BatchedCreatePartiallySucceeds)
{
    auto swss_ipv4_route_prefix = swss::IpPrefix(kIpv4Prefix);
    sai_ip_prefix_t sai_ipv4_route_prefix;
    copy(sai_ipv4_route_prefix, swss_ipv4_route_prefix);
    auto route_entry_ipv4 = GenerateP4RouteEntry(gVrfName, swss_ipv4_route_prefix, p4orch::kSetNexthopId, kNexthopId1);
    p4_oid_mapper_.setOID(SAI_OBJECT_TYPE_NEXT_HOP, KeyGenerator::generateNextHopKey(route_entry_ipv4.nexthop_id),
                          kNexthopOid1);

    auto swss_ipv6_route_prefix = swss::IpPrefix(kIpv6Prefix);
    sai_ip_prefix_t sai_ipv6_route_prefix;
    copy(sai_ipv6_route_prefix, swss_ipv6_route_prefix);
    auto route_entry_ipv6 =
        GenerateP4RouteEntry(gVrfName, swss_ipv6_route_prefix, p4orch::kSetWcmpGroupId, kWcmpGroup1);
    p4_oid_mapper_.setOID(SAI_OBJECT_TYPE_NEXT_HOP_GROUP,
                          KeyGenerator::generateWcmpGroupKey(route_entry_ipv6.wcmp_group), kWcmpGroupOid1);

    sai_route_entry_t exp_sai_route_entry_ipv4;
    exp_sai_route_entry_ipv4.switch_id = gSwitchId;
    exp_sai_route_entry_ipv4.vr_id = gVrfOid;
    exp_sai_route_entry_ipv4.destination = sai_ipv4_route_prefix;

    sai_attribute_t exp_sai_attr_ipv4;
    exp_sai_attr_ipv4.id = SAI_ROUTE_ENTRY_ATTR_NEXT_HOP_ID;
    exp_sai_attr_ipv4.value.oid = kNexthopOid1;

    sai_route_entry_t exp_sai_route_entry_ipv6;
    exp_sai_route_entry_ipv6.switch_id = gSwitchId;
    exp_sai_route_entry_ipv6.vr_id = gVrfOid;
    exp_sai_route_entry_ipv6.destination = sai_ipv6_route_prefix;

    sai_attribute_t exp_sai_attr_ipv6;
    exp_sai_attr_ipv6.id = SAI_ROUTE_ENTRY_ATTR_NEXT_HOP_ID;
    exp_sai_attr_ipv6.value.oid = kWcmpGroupOid1;

    std::vector<sai_status_t> exp_status{SAI_STATUS_SUCCESS, SAI_STATUS_FAILURE};
    EXPECT_CALL(
        mock_sai_route_,
        create_route_entries(
            Eq(2),
            RouteEntryArrayEq(std::vector<sai_route_entry_t>{exp_sai_route_entry_ipv4, exp_sai_route_entry_ipv6}),
            ArrayEq(std::vector<uint32_t>{1, 1}),
            AttrArrayArrayEq(std::vector<std::vector<sai_attribute_t>>{{exp_sai_attr_ipv4}, {exp_sai_attr_ipv6}}),
            Eq(SAI_BULK_OP_ERROR_MODE_IGNORE_ERROR), _))
        .WillOnce(DoAll(SetArrayArgument<5>(exp_status.begin(), exp_status.end()), Return(SAI_STATUS_FAILURE)));
    EXPECT_THAT(CreateRouteEntries(std::vector<P4RouteEntry>{route_entry_ipv4, route_entry_ipv6}),
                ArrayEq(std::vector<StatusCode>{StatusCode::SWSS_RC_SUCCESS, StatusCode::SWSS_RC_UNKNOWN}));
    VerifyRouteEntry(route_entry_ipv4, sai_ipv4_route_prefix, gVrfOid);
    auto *route_entry_ptr_ipv6 = GetRouteEntry(route_entry_ipv6.route_entry_key);
    EXPECT_EQ(nullptr, route_entry_ptr_ipv6);
    EXPECT_FALSE(p4_oid_mapper_.existsOID(SAI_OBJECT_TYPE_ROUTE_ENTRY, route_entry_ipv6.route_entry_key));
    uint32_t ref_cnt;
    EXPECT_TRUE(
        p4_oid_mapper_.getRefCount(SAI_OBJECT_TYPE_NEXT_HOP, KeyGenerator::generateNextHopKey(kNexthopId1), &ref_cnt));
    EXPECT_EQ(1, ref_cnt);
    EXPECT_TRUE(p4_oid_mapper_.getRefCount(SAI_OBJECT_TYPE_NEXT_HOP_GROUP,
                                           KeyGenerator::generateWcmpGroupKey(kWcmpGroup1), &ref_cnt));
    EXPECT_EQ(0, ref_cnt);
}

TEST_F(RouteManagerTest, BatchedUpdateSucceeds)
{
    auto swss_ipv4_route_prefix = swss::IpPrefix(kIpv4Prefix);
    sai_ip_prefix_t sai_ipv4_route_prefix;
    copy(sai_ipv4_route_prefix, swss_ipv4_route_prefix);
    auto route_entry_ipv4 =
        GenerateP4RouteEntry(gVrfName, swss_ipv4_route_prefix, p4orch::kSetWcmpGroupId, kWcmpGroup1);
    SetupNexthopIdRouteEntry(gVrfName, swss_ipv4_route_prefix, kNexthopId1, kNexthopOid1);
    p4_oid_mapper_.setOID(SAI_OBJECT_TYPE_NEXT_HOP_GROUP, KeyGenerator::generateWcmpGroupKey(kWcmpGroup1),
                          kWcmpGroupOid1);

    auto swss_ipv6_route_prefix = swss::IpPrefix(kIpv6Prefix);
    sai_ip_prefix_t sai_ipv6_route_prefix;
    copy(sai_ipv6_route_prefix, swss_ipv6_route_prefix);
    auto route_entry_ipv6 =
        GenerateP4RouteEntry(gVrfName, swss_ipv6_route_prefix, p4orch::kSetWcmpGroupId, kWcmpGroup2);
    SetupDropRouteEntry(gVrfName, swss_ipv6_route_prefix);
    p4_oid_mapper_.setOID(SAI_OBJECT_TYPE_NEXT_HOP_GROUP, KeyGenerator::generateWcmpGroupKey(kWcmpGroup2),
                          kWcmpGroupOid2);

    sai_route_entry_t exp_sai_route_entry_ipv4;
    exp_sai_route_entry_ipv4.switch_id = gSwitchId;
    exp_sai_route_entry_ipv4.vr_id = gVrfOid;
    exp_sai_route_entry_ipv4.destination = sai_ipv4_route_prefix;

    sai_attribute_t exp_sai_attr_ipv4;
    exp_sai_attr_ipv4.id = SAI_ROUTE_ENTRY_ATTR_NEXT_HOP_ID;
    exp_sai_attr_ipv4.value.oid = kWcmpGroupOid1;

    sai_route_entry_t exp_sai_route_entry_ipv6;
    exp_sai_route_entry_ipv6.switch_id = gSwitchId;
    exp_sai_route_entry_ipv6.vr_id = gVrfOid;
    exp_sai_route_entry_ipv6.destination = sai_ipv6_route_prefix;

    sai_attribute_t exp_sai_attr_ipv6;
    exp_sai_attr_ipv6.id = SAI_ROUTE_ENTRY_ATTR_NEXT_HOP_ID;
    exp_sai_attr_ipv6.value.oid = kWcmpGroupOid2;

    sai_attribute_t exp_sai_attr_ipv6_2;
    exp_sai_attr_ipv6_2.id = SAI_ROUTE_ENTRY_ATTR_PACKET_ACTION;
    exp_sai_attr_ipv6_2.value.s32 = SAI_PACKET_ACTION_FORWARD;

    std::vector<sai_status_t> exp_status{SAI_STATUS_SUCCESS, SAI_STATUS_SUCCESS,
                                         SAI_STATUS_SUCCESS};
    EXPECT_CALL(
        mock_sai_route_,
        set_route_entries_attribute(
            Eq(3),
            RouteEntryArrayEq(std::vector<sai_route_entry_t>{
                exp_sai_route_entry_ipv4, exp_sai_route_entry_ipv6,
                exp_sai_route_entry_ipv6}),
            AttrArrayEq(std::vector<sai_attribute_t>{
                exp_sai_attr_ipv4, exp_sai_attr_ipv6, exp_sai_attr_ipv6_2}),
            Eq(SAI_BULK_OP_ERROR_MODE_IGNORE_ERROR), _))
        .WillOnce(
            DoAll(SetArrayArgument<4>(exp_status.begin(), exp_status.end()),
                  Return(SAI_STATUS_SUCCESS)));

    EXPECT_THAT(UpdateRouteEntries(std::vector<P4RouteEntry>{route_entry_ipv4, route_entry_ipv6}),
                ArrayEq(std::vector<StatusCode>{StatusCode::SWSS_RC_SUCCESS, StatusCode::SWSS_RC_SUCCESS}));
    VerifyRouteEntry(route_entry_ipv4, sai_ipv4_route_prefix, gVrfOid);
    VerifyRouteEntry(route_entry_ipv6, sai_ipv6_route_prefix, gVrfOid);
    uint32_t ref_cnt;
    EXPECT_TRUE(
        p4_oid_mapper_.getRefCount(SAI_OBJECT_TYPE_NEXT_HOP, KeyGenerator::generateNextHopKey(kNexthopId1), &ref_cnt));
    EXPECT_EQ(0, ref_cnt);
    EXPECT_TRUE(p4_oid_mapper_.getRefCount(SAI_OBJECT_TYPE_NEXT_HOP_GROUP,
                                           KeyGenerator::generateWcmpGroupKey(kWcmpGroup1), &ref_cnt));
    EXPECT_EQ(1, ref_cnt);
    EXPECT_TRUE(p4_oid_mapper_.getRefCount(SAI_OBJECT_TYPE_NEXT_HOP_GROUP,
                                           KeyGenerator::generateWcmpGroupKey(kWcmpGroup2), &ref_cnt));
    EXPECT_EQ(1, ref_cnt);
}

TEST_F(RouteManagerTest, BatchedUpdatePartiallySucceeds)
{
    auto swss_ipv4_route_prefix = swss::IpPrefix(kIpv4Prefix);
    sai_ip_prefix_t sai_ipv4_route_prefix;
    copy(sai_ipv4_route_prefix, swss_ipv4_route_prefix);
    auto route_entry_ipv4 =
        GenerateP4RouteEntry(gVrfName, swss_ipv4_route_prefix, p4orch::kSetWcmpGroupId, kWcmpGroup1);
    SetupNexthopIdRouteEntry(gVrfName, swss_ipv4_route_prefix, kNexthopId1, kNexthopOid1);
    p4_oid_mapper_.setOID(SAI_OBJECT_TYPE_NEXT_HOP_GROUP, KeyGenerator::generateWcmpGroupKey(kWcmpGroup1),
                          kWcmpGroupOid1);

    auto swss_ipv6_route_prefix = swss::IpPrefix(kIpv6Prefix);
    sai_ip_prefix_t sai_ipv6_route_prefix;
    copy(sai_ipv6_route_prefix, swss_ipv6_route_prefix);
    auto route_entry_ipv6 =
        GenerateP4RouteEntry(gVrfName, swss_ipv6_route_prefix, p4orch::kSetWcmpGroupId, kWcmpGroup2);
    SetupDropRouteEntry(gVrfName, swss_ipv6_route_prefix);
    p4_oid_mapper_.setOID(SAI_OBJECT_TYPE_NEXT_HOP_GROUP, KeyGenerator::generateWcmpGroupKey(kWcmpGroup2),
                          kWcmpGroupOid2);

    sai_route_entry_t exp_sai_route_entry_ipv4;
    exp_sai_route_entry_ipv4.switch_id = gSwitchId;
    exp_sai_route_entry_ipv4.vr_id = gVrfOid;
    exp_sai_route_entry_ipv4.destination = sai_ipv4_route_prefix;

    sai_attribute_t exp_sai_attr_ipv4;
    exp_sai_attr_ipv4.id = SAI_ROUTE_ENTRY_ATTR_NEXT_HOP_ID;
    exp_sai_attr_ipv4.value.oid = kWcmpGroupOid1;

    sai_route_entry_t exp_sai_route_entry_ipv6;
    exp_sai_route_entry_ipv6.switch_id = gSwitchId;
    exp_sai_route_entry_ipv6.vr_id = gVrfOid;
    exp_sai_route_entry_ipv6.destination = sai_ipv6_route_prefix;

    sai_attribute_t exp_sai_attr_ipv6;
    exp_sai_attr_ipv6.id = SAI_ROUTE_ENTRY_ATTR_NEXT_HOP_ID;
    exp_sai_attr_ipv6.value.oid = kWcmpGroupOid2;

    sai_attribute_t exp_sai_attr_ipv6_2;
    exp_sai_attr_ipv6_2.id = SAI_ROUTE_ENTRY_ATTR_PACKET_ACTION;
    exp_sai_attr_ipv6_2.value.s32 = SAI_PACKET_ACTION_FORWARD;

    std::vector<sai_status_t> exp_status{SAI_STATUS_SUCCESS, SAI_STATUS_FAILURE,
                                         SAI_STATUS_NOT_EXECUTED};
    EXPECT_CALL(
        mock_sai_route_,
        set_route_entries_attribute(
            Eq(3),
            RouteEntryArrayEq(std::vector<sai_route_entry_t>{
                exp_sai_route_entry_ipv4, exp_sai_route_entry_ipv6,
                exp_sai_route_entry_ipv6}),
            AttrArrayEq(std::vector<sai_attribute_t>{
                exp_sai_attr_ipv4, exp_sai_attr_ipv6, exp_sai_attr_ipv6_2}),
            Eq(SAI_BULK_OP_ERROR_MODE_IGNORE_ERROR), _))
        .WillOnce(
            DoAll(SetArrayArgument<4>(exp_status.begin(), exp_status.end()),
                  Return(SAI_STATUS_FAILURE)));
    EXPECT_THAT(UpdateRouteEntries(std::vector<P4RouteEntry>{route_entry_ipv4,
                                                             route_entry_ipv6}),
                ArrayEq(std::vector<StatusCode>{StatusCode::SWSS_RC_SUCCESS,
                                                StatusCode::SWSS_RC_UNKNOWN}));
    VerifyRouteEntry(route_entry_ipv4, sai_ipv4_route_prefix, gVrfOid);
    route_entry_ipv6 = GenerateP4RouteEntry(gVrfName, swss_ipv6_route_prefix,
                                            p4orch::kDrop, "");
    VerifyRouteEntry(route_entry_ipv6, sai_ipv6_route_prefix, gVrfOid);
    uint32_t ref_cnt;
    EXPECT_TRUE(p4_oid_mapper_.getRefCount(
        SAI_OBJECT_TYPE_NEXT_HOP, KeyGenerator::generateNextHopKey(kNexthopId1),
        &ref_cnt));
    EXPECT_EQ(0, ref_cnt);
    EXPECT_TRUE(p4_oid_mapper_.getRefCount(
        SAI_OBJECT_TYPE_NEXT_HOP_GROUP,
        KeyGenerator::generateWcmpGroupKey(kWcmpGroup1), &ref_cnt));
    EXPECT_EQ(1, ref_cnt);
    EXPECT_TRUE(p4_oid_mapper_.getRefCount(
        SAI_OBJECT_TYPE_NEXT_HOP_GROUP,
        KeyGenerator::generateWcmpGroupKey(kWcmpGroup2), &ref_cnt));
    EXPECT_EQ(0, ref_cnt);
}

TEST_F(RouteManagerTest, BatchedUpdatePartiallySucceedsRecover) {
  auto swss_ipv4_route_prefix = swss::IpPrefix(kIpv4Prefix);
  sai_ip_prefix_t sai_ipv4_route_prefix;
  copy(sai_ipv4_route_prefix, swss_ipv4_route_prefix);
  auto route_entry_ipv4 = GenerateP4RouteEntry(
      gVrfName, swss_ipv4_route_prefix, p4orch::kSetWcmpGroupId, kWcmpGroup1);
  SetupNexthopIdRouteEntry(gVrfName, swss_ipv4_route_prefix, kNexthopId1,
                           kNexthopOid1);
  p4_oid_mapper_.setOID(SAI_OBJECT_TYPE_NEXT_HOP_GROUP,
                        KeyGenerator::generateWcmpGroupKey(kWcmpGroup1),
                        kWcmpGroupOid1);

  auto swss_ipv6_route_prefix = swss::IpPrefix(kIpv6Prefix);
  sai_ip_prefix_t sai_ipv6_route_prefix;
  copy(sai_ipv6_route_prefix, swss_ipv6_route_prefix);
  auto route_entry_ipv6 = GenerateP4RouteEntry(
      gVrfName, swss_ipv6_route_prefix, p4orch::kSetWcmpGroupId, kWcmpGroup2);
  SetupDropRouteEntry(gVrfName, swss_ipv6_route_prefix);
  p4_oid_mapper_.setOID(SAI_OBJECT_TYPE_NEXT_HOP_GROUP,
                        KeyGenerator::generateWcmpGroupKey(kWcmpGroup2),
                        kWcmpGroupOid2);

  sai_route_entry_t exp_sai_route_entry_ipv4;
  exp_sai_route_entry_ipv4.switch_id = gSwitchId;
  exp_sai_route_entry_ipv4.vr_id = gVrfOid;
  exp_sai_route_entry_ipv4.destination = sai_ipv4_route_prefix;

  sai_attribute_t exp_sai_attr_ipv4;
  exp_sai_attr_ipv4.id = SAI_ROUTE_ENTRY_ATTR_NEXT_HOP_ID;
  exp_sai_attr_ipv4.value.oid = kWcmpGroupOid1;

  sai_route_entry_t exp_sai_route_entry_ipv6;
  exp_sai_route_entry_ipv6.switch_id = gSwitchId;
  exp_sai_route_entry_ipv6.vr_id = gVrfOid;
  exp_sai_route_entry_ipv6.destination = sai_ipv6_route_prefix;

  sai_attribute_t exp_sai_attr_ipv6;
  exp_sai_attr_ipv6.id = SAI_ROUTE_ENTRY_ATTR_NEXT_HOP_ID;
  exp_sai_attr_ipv6.value.oid = kWcmpGroupOid2;
  sai_attribute_t exp_sai_attr_ipv6_2;
  exp_sai_attr_ipv6_2.id = SAI_ROUTE_ENTRY_ATTR_PACKET_ACTION;
  exp_sai_attr_ipv6_2.value.s32 = SAI_PACKET_ACTION_FORWARD;
  sai_attribute_t exp_sai_attr_ipv6_3;
  exp_sai_attr_ipv6_3.id = SAI_ROUTE_ENTRY_ATTR_NEXT_HOP_ID;
  exp_sai_attr_ipv6_3.value.oid = SAI_NULL_OBJECT_ID;

  std::vector<sai_status_t> exp_status_1{SAI_STATUS_SUCCESS, SAI_STATUS_SUCCESS,
                                         SAI_STATUS_FAILURE};
  std::vector<sai_status_t> exp_status_2{SAI_STATUS_SUCCESS};
  EXPECT_CALL(
      mock_sai_route_,
      set_route_entries_attribute(
          Eq(3),
          RouteEntryArrayEq(std::vector<sai_route_entry_t>{
              exp_sai_route_entry_ipv4, exp_sai_route_entry_ipv6,
              exp_sai_route_entry_ipv6}),
          AttrArrayEq(std::vector<sai_attribute_t>{
              exp_sai_attr_ipv4, exp_sai_attr_ipv6, exp_sai_attr_ipv6_2}),
          Eq(SAI_BULK_OP_ERROR_MODE_IGNORE_ERROR), _))
      .WillOnce(
          DoAll(SetArrayArgument<4>(exp_status_1.begin(), exp_status_1.end()),
                Return(SAI_STATUS_FAILURE)));
  EXPECT_CALL(
      mock_sai_route_,
      set_route_entries_attribute(
          Eq(1),
          RouteEntryArrayEq(
              std::vector<sai_route_entry_t>{exp_sai_route_entry_ipv6}),
          AttrArrayEq(std::vector<sai_attribute_t>{exp_sai_attr_ipv6_3}),
          Eq(SAI_BULK_OP_ERROR_MODE_IGNORE_ERROR), _))
      .WillOnce(
          DoAll(SetArrayArgument<4>(exp_status_2.begin(), exp_status_2.end()),
                Return(SAI_STATUS_FAILURE)));
  EXPECT_THAT(UpdateRouteEntries(std::vector<P4RouteEntry>{route_entry_ipv4,
                                                           route_entry_ipv6}),
              ArrayEq(std::vector<StatusCode>{StatusCode::SWSS_RC_SUCCESS,
                                              StatusCode::SWSS_RC_UNKNOWN}));
  VerifyRouteEntry(route_entry_ipv4, sai_ipv4_route_prefix, gVrfOid);
  route_entry_ipv6 =
      GenerateP4RouteEntry(gVrfName, swss_ipv6_route_prefix, p4orch::kDrop, "");
  VerifyRouteEntry(route_entry_ipv6, sai_ipv6_route_prefix, gVrfOid);
  uint32_t ref_cnt;
  EXPECT_TRUE(p4_oid_mapper_.getRefCount(
      SAI_OBJECT_TYPE_NEXT_HOP, KeyGenerator::generateNextHopKey(kNexthopId1),
      &ref_cnt));
  EXPECT_EQ(0, ref_cnt);
  EXPECT_TRUE(p4_oid_mapper_.getRefCount(
      SAI_OBJECT_TYPE_NEXT_HOP_GROUP,
      KeyGenerator::generateWcmpGroupKey(kWcmpGroup1), &ref_cnt));
  EXPECT_EQ(1, ref_cnt);
  EXPECT_TRUE(p4_oid_mapper_.getRefCount(
      SAI_OBJECT_TYPE_NEXT_HOP_GROUP,
      KeyGenerator::generateWcmpGroupKey(kWcmpGroup2), &ref_cnt));
  EXPECT_EQ(0, ref_cnt);
}

TEST_F(RouteManagerTest, BatchedDeleteSucceeds)
{
    auto swss_ipv4_route_prefix = swss::IpPrefix(kIpv4Prefix);
    sai_ip_prefix_t sai_ipv4_route_prefix;
    copy(sai_ipv4_route_prefix, swss_ipv4_route_prefix);
    auto route_entry_ipv4 = GenerateP4RouteEntry(gVrfName, swss_ipv4_route_prefix, p4orch::kSetNexthopId, kNexthopId1);
    SetupNexthopIdRouteEntry(gVrfName, swss_ipv4_route_prefix, kNexthopId1, kNexthopOid1);

    auto swss_ipv6_route_prefix = swss::IpPrefix(kIpv6Prefix);
    sai_ip_prefix_t sai_ipv6_route_prefix;
    copy(sai_ipv6_route_prefix, swss_ipv6_route_prefix);
    auto route_entry_ipv6 = GenerateP4RouteEntry(gVrfName, swss_ipv6_route_prefix, p4orch::kDrop, "");
    SetupDropRouteEntry(gVrfName, swss_ipv6_route_prefix);

    sai_route_entry_t exp_sai_route_entry_ipv4;
    exp_sai_route_entry_ipv4.switch_id = gSwitchId;
    exp_sai_route_entry_ipv4.vr_id = gVrfOid;
    exp_sai_route_entry_ipv4.destination = sai_ipv4_route_prefix;

    sai_route_entry_t exp_sai_route_entry_ipv6;
    exp_sai_route_entry_ipv6.switch_id = gSwitchId;
    exp_sai_route_entry_ipv6.vr_id = gVrfOid;
    exp_sai_route_entry_ipv6.destination = sai_ipv6_route_prefix;

    std::vector<sai_status_t> exp_status{SAI_STATUS_SUCCESS, SAI_STATUS_SUCCESS};
    EXPECT_CALL(mock_sai_route_, remove_route_entries(Eq(2),
                                                      RouteEntryArrayEq(std::vector<sai_route_entry_t>{
                                                          exp_sai_route_entry_ipv4, exp_sai_route_entry_ipv6}),
                                                      Eq(SAI_BULK_OP_ERROR_MODE_IGNORE_ERROR), _))
        .WillOnce(DoAll(SetArrayArgument<3>(exp_status.begin(), exp_status.end()), Return(SAI_STATUS_SUCCESS)));
    EXPECT_THAT(DeleteRouteEntries(std::vector<P4RouteEntry>{route_entry_ipv4, route_entry_ipv6}),
                ArrayEq(std::vector<StatusCode>{StatusCode::SWSS_RC_SUCCESS, StatusCode::SWSS_RC_SUCCESS}));
    auto *route_entry_ptr_ipv4 = GetRouteEntry(route_entry_ipv4.route_entry_key);
    EXPECT_EQ(nullptr, route_entry_ptr_ipv4);
    EXPECT_FALSE(p4_oid_mapper_.existsOID(SAI_OBJECT_TYPE_ROUTE_ENTRY, route_entry_ipv4.route_entry_key));
    auto *route_entry_ptr_ipv6 = GetRouteEntry(route_entry_ipv6.route_entry_key);
    EXPECT_EQ(nullptr, route_entry_ptr_ipv6);
    EXPECT_FALSE(p4_oid_mapper_.existsOID(SAI_OBJECT_TYPE_ROUTE_ENTRY, route_entry_ipv6.route_entry_key));
    uint32_t ref_cnt;
    EXPECT_TRUE(
        p4_oid_mapper_.getRefCount(SAI_OBJECT_TYPE_NEXT_HOP, KeyGenerator::generateNextHopKey(kNexthopId1), &ref_cnt));
    EXPECT_EQ(0, ref_cnt);
}

TEST_F(RouteManagerTest, BatchedDeletePartiallySucceeds)
{
    auto swss_ipv4_route_prefix = swss::IpPrefix(kIpv4Prefix);
    sai_ip_prefix_t sai_ipv4_route_prefix;
    copy(sai_ipv4_route_prefix, swss_ipv4_route_prefix);
    auto route_entry_ipv4 = GenerateP4RouteEntry(gVrfName, swss_ipv4_route_prefix, p4orch::kSetNexthopId, kNexthopId1);
    SetupNexthopIdRouteEntry(gVrfName, swss_ipv4_route_prefix, kNexthopId1, kNexthopOid1);

    auto swss_ipv6_route_prefix = swss::IpPrefix(kIpv6Prefix);
    sai_ip_prefix_t sai_ipv6_route_prefix;
    copy(sai_ipv6_route_prefix, swss_ipv6_route_prefix);
    auto route_entry_ipv6 = GenerateP4RouteEntry(gVrfName, swss_ipv6_route_prefix, p4orch::kDrop, "");
    SetupDropRouteEntry(gVrfName, swss_ipv6_route_prefix);

    sai_route_entry_t exp_sai_route_entry_ipv4;
    exp_sai_route_entry_ipv4.switch_id = gSwitchId;
    exp_sai_route_entry_ipv4.vr_id = gVrfOid;
    exp_sai_route_entry_ipv4.destination = sai_ipv4_route_prefix;

    sai_route_entry_t exp_sai_route_entry_ipv6;
    exp_sai_route_entry_ipv6.switch_id = gSwitchId;
    exp_sai_route_entry_ipv6.vr_id = gVrfOid;
    exp_sai_route_entry_ipv6.destination = sai_ipv6_route_prefix;

    std::vector<sai_status_t> exp_status{SAI_STATUS_SUCCESS, SAI_STATUS_FAILURE};
    EXPECT_CALL(mock_sai_route_, remove_route_entries(Eq(2),
                                                      RouteEntryArrayEq(std::vector<sai_route_entry_t>{
                                                          exp_sai_route_entry_ipv4, exp_sai_route_entry_ipv6}),
                                                      Eq(SAI_BULK_OP_ERROR_MODE_IGNORE_ERROR), _))
        .WillOnce(DoAll(SetArrayArgument<3>(exp_status.begin(), exp_status.end()), Return(SAI_STATUS_FAILURE)));
    EXPECT_THAT(DeleteRouteEntries(std::vector<P4RouteEntry>{route_entry_ipv4, route_entry_ipv6}),
                ArrayEq(std::vector<StatusCode>{StatusCode::SWSS_RC_SUCCESS, StatusCode::SWSS_RC_UNKNOWN}));
    auto *route_entry_ptr_ipv4 = GetRouteEntry(route_entry_ipv4.route_entry_key);
    EXPECT_EQ(nullptr, route_entry_ptr_ipv4);
    EXPECT_FALSE(p4_oid_mapper_.existsOID(SAI_OBJECT_TYPE_ROUTE_ENTRY, route_entry_ipv4.route_entry_key));
    VerifyRouteEntry(route_entry_ipv6, sai_ipv6_route_prefix, gVrfOid);
    uint32_t ref_cnt;
    EXPECT_TRUE(
        p4_oid_mapper_.getRefCount(SAI_OBJECT_TYPE_NEXT_HOP, KeyGenerator::generateNextHopKey(kNexthopId1), &ref_cnt));
    EXPECT_EQ(0, ref_cnt);
}

TEST_F(RouteManagerTest, DrainNotExecuted) {
  auto prefix_1 = swss::IpPrefix(kIpv4Prefix);
  auto prefix_2 = swss::IpPrefix(kIpv4Prefix2);
  auto prefix_3 = swss::IpPrefix(kIpv6Prefix);
  p4_oid_mapper_.setOID(SAI_OBJECT_TYPE_NEXT_HOP,
                        KeyGenerator::generateNextHopKey(kNexthopId1),
                        kNexthopOid1);
  auto key_op_fvs_1 = GenerateKeyOpFieldsValuesTuple(
      gVrfName, prefix_1, SET_COMMAND, p4orch::kSetNexthopId, kNexthopId1);
  auto key_op_fvs_2 = GenerateKeyOpFieldsValuesTuple(
      gVrfName, prefix_2, SET_COMMAND, p4orch::kSetNexthopId, kNexthopId1);
  auto key_op_fvs_3 = GenerateKeyOpFieldsValuesTuple(
      gVrfName, prefix_3, SET_COMMAND, p4orch::kSetNexthopId, kNexthopId1);
  Enqueue(APP_P4RT_IPV4_TABLE_NAME, key_op_fvs_1);
  Enqueue(APP_P4RT_IPV4_TABLE_NAME, key_op_fvs_2);
  Enqueue(APP_P4RT_IPV6_TABLE_NAME, key_op_fvs_3);

  EXPECT_CALL(publisher_,
              publish(Eq(APP_P4RT_TABLE_NAME), Eq(kfvKey(key_op_fvs_1)),
                      FieldValueTupleArrayEq(kfvFieldsValues(key_op_fvs_1)),
                      Eq(StatusCode::SWSS_RC_NOT_EXECUTED), Eq(true)));
  EXPECT_CALL(publisher_,
              publish(Eq(APP_P4RT_TABLE_NAME), Eq(kfvKey(key_op_fvs_2)),
                      FieldValueTupleArrayEq(kfvFieldsValues(key_op_fvs_2)),
                      Eq(StatusCode::SWSS_RC_NOT_EXECUTED), Eq(true)));
  EXPECT_CALL(publisher_,
              publish(Eq(APP_P4RT_TABLE_NAME), Eq(kfvKey(key_op_fvs_3)),
                      FieldValueTupleArrayEq(kfvFieldsValues(key_op_fvs_3)),
                      Eq(StatusCode::SWSS_RC_NOT_EXECUTED), Eq(true)));
  EXPECT_EQ(StatusCode::SWSS_RC_NOT_EXECUTED, Drain(/*failure_before=*/true));
  EXPECT_EQ(nullptr,
            GetRouteEntry(KeyGenerator::generateRouteKey(gVrfName, prefix_1)));
  EXPECT_EQ(nullptr,
            GetRouteEntry(KeyGenerator::generateRouteKey(gVrfName, prefix_2)));
  EXPECT_EQ(nullptr,
            GetRouteEntry(KeyGenerator::generateRouteKey(gVrfName, prefix_3)));
}

TEST_F(RouteManagerTest, DrainStopOnFirstFailureCreate) {
  auto prefix_1 = swss::IpPrefix(kIpv4Prefix);
  auto prefix_2 = swss::IpPrefix(kIpv4Prefix2);
  auto prefix_3 = swss::IpPrefix(kIpv6Prefix);
  p4_oid_mapper_.setOID(SAI_OBJECT_TYPE_NEXT_HOP,
                        KeyGenerator::generateNextHopKey(kNexthopId1),
                        kNexthopOid1);
  auto key_op_fvs_1 = GenerateKeyOpFieldsValuesTuple(
      gVrfName, prefix_1, SET_COMMAND, p4orch::kSetNexthopId, kNexthopId1);
  auto key_op_fvs_2 = GenerateKeyOpFieldsValuesTuple(
      gVrfName, prefix_2, SET_COMMAND, p4orch::kSetNexthopId, kNexthopId1);
  auto key_op_fvs_3 = GenerateKeyOpFieldsValuesTuple(
      gVrfName, prefix_3, SET_COMMAND, p4orch::kSetNexthopId, kNexthopId1);
  Enqueue(APP_P4RT_IPV4_TABLE_NAME, key_op_fvs_1);
  Enqueue(APP_P4RT_IPV4_TABLE_NAME, key_op_fvs_2);
  Enqueue(APP_P4RT_IPV6_TABLE_NAME, key_op_fvs_3);

  std::vector<sai_status_t> exp_status{SAI_STATUS_SUCCESS, SAI_STATUS_FAILURE,
                                       SAI_STATUS_NOT_EXECUTED};
  EXPECT_CALL(mock_sai_route_, create_route_entries(Eq(3), _, _, _, _, _))
      .WillOnce(DoAll(SetArrayArgument<5>(exp_status.begin(), exp_status.end()),
                      Return(SAI_STATUS_FAILURE)));
  EXPECT_CALL(publisher_,
              publish(Eq(APP_P4RT_TABLE_NAME), Eq(kfvKey(key_op_fvs_1)),
                      FieldValueTupleArrayEq(kfvFieldsValues(key_op_fvs_1)),
                      Eq(StatusCode::SWSS_RC_SUCCESS), Eq(true)));
  EXPECT_CALL(publisher_,
              publish(Eq(APP_P4RT_TABLE_NAME), Eq(kfvKey(key_op_fvs_2)),
                      FieldValueTupleArrayEq(kfvFieldsValues(key_op_fvs_2)),
                      Eq(StatusCode::SWSS_RC_UNKNOWN), Eq(true)));
  EXPECT_CALL(publisher_,
              publish(Eq(APP_P4RT_TABLE_NAME), Eq(kfvKey(key_op_fvs_3)),
                      FieldValueTupleArrayEq(kfvFieldsValues(key_op_fvs_3)),
                      Eq(StatusCode::SWSS_RC_NOT_EXECUTED), Eq(true)));
  EXPECT_EQ(StatusCode::SWSS_RC_UNKNOWN, Drain(/*failure_before=*/false));
  EXPECT_NE(nullptr,
            GetRouteEntry(KeyGenerator::generateRouteKey(gVrfName, prefix_1)));
  EXPECT_EQ(nullptr,
            GetRouteEntry(KeyGenerator::generateRouteKey(gVrfName, prefix_2)));
  EXPECT_EQ(nullptr,
            GetRouteEntry(KeyGenerator::generateRouteKey(gVrfName, prefix_3)));
}

TEST_F(RouteManagerTest, DrainStopOnFirstFailureCreateAndUpdate) {
  auto prefix_1 = swss::IpPrefix(kIpv4Prefix);
  auto prefix_2 = swss::IpPrefix(kIpv4Prefix2);
  auto prefix_3 = swss::IpPrefix(kIpv6Prefix);
  SetupDropRouteEntry(gVrfName, prefix_3);
  p4_oid_mapper_.setOID(SAI_OBJECT_TYPE_NEXT_HOP,
                        KeyGenerator::generateNextHopKey(kNexthopId1),
                        kNexthopOid1);
  auto key_op_fvs_1 = GenerateKeyOpFieldsValuesTuple(
      gVrfName, prefix_1, SET_COMMAND, p4orch::kSetNexthopId, kNexthopId1);
  auto key_op_fvs_2 = GenerateKeyOpFieldsValuesTuple(
      gVrfName, prefix_2, SET_COMMAND, p4orch::kSetNexthopId, kNexthopId1);
  auto key_op_fvs_3 = GenerateKeyOpFieldsValuesTuple(
      gVrfName, prefix_3, SET_COMMAND, p4orch::kSetNexthopId, kNexthopId1);
  Enqueue(APP_P4RT_IPV4_TABLE_NAME, key_op_fvs_1);
  Enqueue(APP_P4RT_IPV4_TABLE_NAME, key_op_fvs_2);
  Enqueue(APP_P4RT_IPV6_TABLE_NAME, key_op_fvs_3);

  std::vector<sai_status_t> exp_status{SAI_STATUS_SUCCESS, SAI_STATUS_FAILURE};
  EXPECT_CALL(mock_sai_route_, create_route_entries(Eq(2), _, _, _, _, _))
      .WillOnce(DoAll(SetArrayArgument<5>(exp_status.begin(), exp_status.end()),
                      Return(SAI_STATUS_FAILURE)));
  EXPECT_CALL(publisher_,
              publish(Eq(APP_P4RT_TABLE_NAME), Eq(kfvKey(key_op_fvs_1)),
                      FieldValueTupleArrayEq(kfvFieldsValues(key_op_fvs_1)),
                      Eq(StatusCode::SWSS_RC_SUCCESS), Eq(true)));
  EXPECT_CALL(publisher_,
              publish(Eq(APP_P4RT_TABLE_NAME), Eq(kfvKey(key_op_fvs_2)),
                      FieldValueTupleArrayEq(kfvFieldsValues(key_op_fvs_2)),
                      Eq(StatusCode::SWSS_RC_UNKNOWN), Eq(true)));
  EXPECT_CALL(publisher_,
              publish(Eq(APP_P4RT_TABLE_NAME), Eq(kfvKey(key_op_fvs_3)),
                      FieldValueTupleArrayEq(kfvFieldsValues(key_op_fvs_3)),
                      Eq(StatusCode::SWSS_RC_NOT_EXECUTED), Eq(true)));
  EXPECT_EQ(StatusCode::SWSS_RC_UNKNOWN, Drain(/*failure_before=*/false));
  EXPECT_NE(nullptr,
            GetRouteEntry(KeyGenerator::generateRouteKey(gVrfName, prefix_1)));
  EXPECT_EQ(nullptr,
            GetRouteEntry(KeyGenerator::generateRouteKey(gVrfName, prefix_2)));
  EXPECT_NE(nullptr,
            GetRouteEntry(KeyGenerator::generateRouteKey(gVrfName, prefix_3)));
}

TEST_F(RouteManagerTest, DrainStopOnFirstFailureMultipleCreateAndUpdate) {
  auto prefix_1 = swss::IpPrefix(kIpv4Prefix);
  auto prefix_2 = swss::IpPrefix(kIpv4Prefix2);
  auto prefix_3 = swss::IpPrefix(kIpv6Prefix);
  auto prefix_4 = swss::IpPrefix(kIpv6Prefix2);
  SetupDropRouteEntry(gVrfName, prefix_3);
  SetupDropRouteEntry(gVrfName, prefix_4);
  p4_oid_mapper_.setOID(SAI_OBJECT_TYPE_NEXT_HOP,
                        KeyGenerator::generateNextHopKey(kNexthopId1),
                        kNexthopOid1);
  auto key_op_fvs_1 = GenerateKeyOpFieldsValuesTuple(
      gVrfName, prefix_1, SET_COMMAND, p4orch::kSetNexthopId, kNexthopId1);
  auto key_op_fvs_2 = GenerateKeyOpFieldsValuesTuple(
      gVrfName, prefix_2, SET_COMMAND, p4orch::kSetNexthopId, kNexthopId1);
  auto key_op_fvs_3 = GenerateKeyOpFieldsValuesTuple(
      gVrfName, prefix_3, SET_COMMAND, p4orch::kSetNexthopId, kNexthopId1);
  auto key_op_fvs_4 = GenerateKeyOpFieldsValuesTuple(
      gVrfName, prefix_4, SET_COMMAND, p4orch::kSetNexthopId, kNexthopId1);
  Enqueue(APP_P4RT_IPV4_TABLE_NAME, key_op_fvs_1);
  Enqueue(APP_P4RT_IPV4_TABLE_NAME, key_op_fvs_2);
  Enqueue(APP_P4RT_IPV6_TABLE_NAME, key_op_fvs_3);
  Enqueue(APP_P4RT_IPV6_TABLE_NAME, key_op_fvs_4);

  std::vector<sai_status_t> exp_status{SAI_STATUS_SUCCESS, SAI_STATUS_FAILURE};
  EXPECT_CALL(mock_sai_route_, create_route_entries(Eq(2), _, _, _, _, _))
      .WillOnce(DoAll(SetArrayArgument<5>(exp_status.begin(), exp_status.end()),
                      Return(SAI_STATUS_FAILURE)));
  EXPECT_CALL(publisher_,
              publish(Eq(APP_P4RT_TABLE_NAME), Eq(kfvKey(key_op_fvs_1)),
                      FieldValueTupleArrayEq(kfvFieldsValues(key_op_fvs_1)),
                      Eq(StatusCode::SWSS_RC_SUCCESS), Eq(true)));
  EXPECT_CALL(publisher_,
              publish(Eq(APP_P4RT_TABLE_NAME), Eq(kfvKey(key_op_fvs_2)),
                      FieldValueTupleArrayEq(kfvFieldsValues(key_op_fvs_2)),
                      Eq(StatusCode::SWSS_RC_UNKNOWN), Eq(true)));
  EXPECT_CALL(publisher_,
              publish(Eq(APP_P4RT_TABLE_NAME), Eq(kfvKey(key_op_fvs_3)),
                      FieldValueTupleArrayEq(kfvFieldsValues(key_op_fvs_3)),
                      Eq(StatusCode::SWSS_RC_NOT_EXECUTED), Eq(true)));
  EXPECT_CALL(publisher_,
              publish(Eq(APP_P4RT_TABLE_NAME), Eq(kfvKey(key_op_fvs_4)),
                      FieldValueTupleArrayEq(kfvFieldsValues(key_op_fvs_4)),
                      Eq(StatusCode::SWSS_RC_NOT_EXECUTED), Eq(true)));
  EXPECT_EQ(StatusCode::SWSS_RC_UNKNOWN, Drain(/*failure_before=*/false));
  EXPECT_NE(nullptr,
            GetRouteEntry(KeyGenerator::generateRouteKey(gVrfName, prefix_1)));
  EXPECT_EQ(nullptr,
            GetRouteEntry(KeyGenerator::generateRouteKey(gVrfName, prefix_2)));
  EXPECT_NE(nullptr,
            GetRouteEntry(KeyGenerator::generateRouteKey(gVrfName, prefix_3)));
  EXPECT_NE(nullptr,
            GetRouteEntry(KeyGenerator::generateRouteKey(gVrfName, prefix_4)));
}

TEST_F(RouteManagerTest, VerifyStateTest)
{
    auto swss_ipv4_route_prefix = swss::IpPrefix(kIpv4Prefix);
    SetupNexthopIdRouteEntry(gVrfName, swss_ipv4_route_prefix, kNexthopId1, kNexthopOid1);
    auto route_entry = GenerateP4RouteEntry(gVrfName, swss_ipv4_route_prefix, p4orch::kSetNexthopId, kNexthopId1);

    // Setup ASIC DB.
    swss::Table table(nullptr, "ASIC_STATE");
    table.set("SAI_OBJECT_TYPE_ROUTE_ENTRY:{\"dest\":\"10.11.12.0/"
              "24\",\"switch_id\":\"oid:0x0\",\"vr\":\"oid:0x6f\"}",
              std::vector<swss::FieldValueTuple>{swss::FieldValueTuple{"SAI_ROUTE_ENTRY_ATTR_NEXT_HOP_ID", "oid:0x1"}});

    nlohmann::json j;
    j[prependMatchField(p4orch::kVrfId)] = gVrfName;
    j[prependMatchField(p4orch::kIpv4Dst)] = kIpv4Prefix;
    const std::string db_key = std::string(APP_P4RT_TABLE_NAME) + kTableKeyDelimiter + APP_P4RT_IPV4_TABLE_NAME +
                               kTableKeyDelimiter + j.dump();
    std::vector<swss::FieldValueTuple> attributes;

    // Verification should succeed with vaild key and value.
    attributes.push_back(swss::FieldValueTuple{p4orch::kAction, p4orch::kSetNexthopId});
    attributes.push_back(swss::FieldValueTuple{prependParamField(p4orch::kNexthopId), kNexthopId1});
    EXPECT_EQ(VerifyState(db_key, attributes), "");

    // TODO: Expect critical state.

    // Invalid key should fail verification.
    EXPECT_FALSE(VerifyState("invalid", attributes).empty());
    EXPECT_FALSE(VerifyState("invalid:invalid", attributes).empty());
    EXPECT_FALSE(VerifyState(std::string(APP_P4RT_TABLE_NAME) + ":invalid", attributes).empty());
    EXPECT_FALSE(VerifyState(std::string(APP_P4RT_TABLE_NAME) + ":invalid:invalid", attributes).empty());
    EXPECT_FALSE(VerifyState(std::string(APP_P4RT_TABLE_NAME) + ":FIXED_IPV4_TABLE:invalid", attributes).empty());

    // Verification should fail if nexthop ID does not exist.
    attributes.clear();
    attributes.push_back(swss::FieldValueTuple{p4orch::kAction, p4orch::kSetNexthopId});
    attributes.push_back(swss::FieldValueTuple{prependParamField(p4orch::kNexthopId), kNexthopId2});
    EXPECT_FALSE(VerifyState(db_key, attributes).empty());
    attributes.clear();
    attributes.push_back(swss::FieldValueTuple{p4orch::kAction, p4orch::kSetNexthopId});
    attributes.push_back(swss::FieldValueTuple{prependParamField(p4orch::kNexthopId), kNexthopId1});

    // Verification should fail if entry does not exist.
    j[prependMatchField(p4orch::kIpv4Dst)] = "1.1.1.0/24";
    EXPECT_FALSE(VerifyState(std::string(APP_P4RT_TABLE_NAME) + kTableKeyDelimiter + APP_P4RT_IPV4_TABLE_NAME +
                                 kTableKeyDelimiter + j.dump(),
                             attributes)
                     .empty());

    auto *route_entry_ptr = GetRouteEntry(KeyGenerator::generateRouteKey(gVrfName, swss_ipv4_route_prefix));
    EXPECT_NE(route_entry_ptr, nullptr);

    // Verification should fail if route entry key mismatches.
    auto saved_route_entry_key = route_entry_ptr->route_entry_key;
    route_entry_ptr->route_entry_key = "invalid";
    EXPECT_FALSE(VerifyState(db_key, attributes).empty());
    route_entry_ptr->route_entry_key = saved_route_entry_key;

    // Verification should fail if VRF ID mismatches.
    auto saved_vrf_id = route_entry_ptr->vrf_id;
    route_entry_ptr->vrf_id = "invalid";
    EXPECT_FALSE(VerifyState(db_key, attributes).empty());
    route_entry_ptr->vrf_id = saved_vrf_id;

    // Verification should fail if route prefix mismatches.
    auto saved_route_prefix = route_entry_ptr->route_prefix;
    route_entry_ptr->route_prefix = swss::IpPrefix(kIpv6Prefix);
    EXPECT_FALSE(VerifyState(db_key, attributes).empty());
    route_entry_ptr->route_prefix = saved_route_prefix;

    // Verification should fail if action mismatches.
    auto saved_action = route_entry_ptr->action;
    route_entry_ptr->action = p4orch::kSetWcmpGroupId;
    EXPECT_FALSE(VerifyState(db_key, attributes).empty());
    route_entry_ptr->action = saved_action;

    // Verification should fail if nexthop ID mismatches.
    auto saved_nexthop_id = route_entry_ptr->nexthop_id;
    route_entry_ptr->nexthop_id = kNexthopId2;
    EXPECT_FALSE(VerifyState(db_key, attributes).empty());
    route_entry_ptr->nexthop_id = saved_nexthop_id;

    // Verification should fail if WCMP group mismatches.
    auto saved_wcmp_group = route_entry_ptr->wcmp_group;
    route_entry_ptr->wcmp_group = kWcmpGroup1;
    EXPECT_FALSE(VerifyState(db_key, attributes).empty());
    route_entry_ptr->wcmp_group = saved_wcmp_group;

    // Verification should fail if WCMP group mismatches.
    auto saved_route_metadata = route_entry_ptr->route_metadata;
    route_entry_ptr->route_metadata = kMetadata1;
    EXPECT_FALSE(VerifyState(db_key, attributes).empty());
    route_entry_ptr->route_metadata = saved_route_metadata;
}

TEST_F(RouteManagerTest, VerifyStateAsicDbTest)
{
    auto swss_ipv4_route_prefix = swss::IpPrefix(kIpv4Prefix);
    SetupDropRouteEntry(gVrfName, swss_ipv4_route_prefix);
    auto swss_ipv6_route_prefix = swss::IpPrefix(kIpv6Prefix);
    SetupNexthopIdRouteEntry(gVrfName, swss_ipv6_route_prefix, kNexthopId1, kNexthopOid1, kMetadata1);

    auto swss_ipv4_route_prefix2 = swss::IpPrefix(kIpv4Prefix2);
    auto route_entry =
        GenerateP4RouteEntry(gVrfName, swss_ipv4_route_prefix2, p4orch::kSetMetadataAndDrop, "", kMetadata2);

    std::vector<sai_status_t> exp_status{SAI_STATUS_SUCCESS};
    EXPECT_CALL(mock_sai_route_, create_route_entries(_, _, _, _, _, _))
        .WillOnce(DoAll(SetArrayArgument<5>(exp_status.begin(), exp_status.end()), Return(SAI_STATUS_SUCCESS)));
    EXPECT_THAT(CreateRouteEntries(std::vector<P4RouteEntry>{route_entry}),
                ArrayEq(std::vector<StatusCode>{StatusCode::SWSS_RC_SUCCESS}));

    // Setup ASIC DB.
    swss::Table table(nullptr, "ASIC_STATE");
    table.set("SAI_OBJECT_TYPE_ROUTE_ENTRY:{\"dest\":\"10.11.12.0/"
              "24\",\"switch_id\":\"oid:0x0\",\"vr\":\"oid:0x6f\"}",
              std::vector<swss::FieldValueTuple>{
                  swss::FieldValueTuple{"SAI_ROUTE_ENTRY_ATTR_PACKET_ACTION", "SAI_PACKET_ACTION_DROP"},
                  swss::FieldValueTuple{"SAI_ROUTE_ENTRY_ATTR_NEXT_HOP_ID", "oid:0x0"}});
    table.set("SAI_OBJECT_TYPE_ROUTE_ENTRY:{\"dest\":\"2001:db8:1::/"
              "32\",\"switch_id\":\"oid:0x0\",\"vr\":\"oid:0x6f\"}",
              std::vector<swss::FieldValueTuple>{swss::FieldValueTuple{"SAI_ROUTE_ENTRY_ATTR_NEXT_HOP_ID", "oid:0x1"},
                                                 swss::FieldValueTuple{"SAI_ROUTE_ENTRY_ATTR_META_DATA", "1"}});

    table.set("SAI_OBJECT_TYPE_ROUTE_ENTRY:{\"dest\":\"10.12.12.0/"
              "24\",\"switch_id\":\"oid:0x0\",\"vr\":\"oid:0x6f\"}",
              std::vector<swss::FieldValueTuple>{
                  swss::FieldValueTuple{"SAI_ROUTE_ENTRY_ATTR_PACKET_ACTION", "SAI_PACKET_ACTION_DROP"},
                  swss::FieldValueTuple{"SAI_ROUTE_ENTRY_ATTR_META_DATA", "2"}});

    nlohmann::json j_1;
    j_1[prependMatchField(p4orch::kVrfId)] = gVrfName;
    j_1[prependMatchField(p4orch::kIpv4Dst)] = kIpv4Prefix;
    const std::string db_key_1 = std::string(APP_P4RT_TABLE_NAME) + kTableKeyDelimiter + APP_P4RT_IPV4_TABLE_NAME +
                                 kTableKeyDelimiter + j_1.dump();
    std::vector<swss::FieldValueTuple> attributes_1;
    attributes_1.push_back(swss::FieldValueTuple{p4orch::kAction, p4orch::kDrop});
    nlohmann::json j_2;
    j_2[prependMatchField(p4orch::kVrfId)] = gVrfName;
    j_2[prependMatchField(p4orch::kIpv6Dst)] = kIpv6Prefix;
    const std::string db_key_2 = std::string(APP_P4RT_TABLE_NAME) + kTableKeyDelimiter + APP_P4RT_IPV6_TABLE_NAME +
                                 kTableKeyDelimiter + j_2.dump();
    std::vector<swss::FieldValueTuple> attributes_2;
    attributes_2.push_back(swss::FieldValueTuple{p4orch::kAction, p4orch::kSetNexthopIdAndMetadata});
    attributes_2.push_back(swss::FieldValueTuple{prependParamField(p4orch::kNexthopId), kNexthopId1});
    attributes_2.push_back(swss::FieldValueTuple{prependParamField(p4orch::kRouteMetadata), kMetadata1});

    nlohmann::json j_3;
    j_3[prependMatchField(p4orch::kVrfId)] = gVrfName;
    j_3[prependMatchField(p4orch::kIpv6Dst)] = kIpv4Prefix2;
    const std::string db_key_3 = std::string(APP_P4RT_TABLE_NAME) + kTableKeyDelimiter + APP_P4RT_IPV6_TABLE_NAME +
                                 kTableKeyDelimiter + j_3.dump();
    std::vector<swss::FieldValueTuple> attributes_3;
    attributes_3.push_back(swss::FieldValueTuple{p4orch::kAction, p4orch::kSetMetadataAndDrop});
    attributes_3.push_back(swss::FieldValueTuple{prependParamField(p4orch::kRouteMetadata), kMetadata2});

    // Verification should succeed with correct ASIC DB values.
    EXPECT_EQ(VerifyState(db_key_1, attributes_1), "");
    EXPECT_EQ(VerifyState(db_key_2, attributes_2), "");
    EXPECT_EQ(VerifyState(db_key_3, attributes_3), "");

    // Verification should fail if ASIC DB values mismatch.
    table.set("SAI_OBJECT_TYPE_ROUTE_ENTRY:{\"dest\":\"10.11.12.0/"
              "24\",\"switch_id\":\"oid:0x0\",\"vr\":\"oid:0x6f\"}",
              std::vector<swss::FieldValueTuple>{
                  swss::FieldValueTuple{"SAI_ROUTE_ENTRY_ATTR_PACKET_ACTION", "SAI_PACKET_ACTION_FORWARD"}});
    table.set("SAI_OBJECT_TYPE_ROUTE_ENTRY:{\"dest\":\"2001:db8:1::/"
              "32\",\"switch_id\":\"oid:0x0\",\"vr\":\"oid:0x6f\"}",
              std::vector<swss::FieldValueTuple>{swss::FieldValueTuple{"SAI_ROUTE_ENTRY_ATTR_META_DATA", "2"}});
    EXPECT_FALSE(VerifyState(db_key_1, attributes_1).empty());
    EXPECT_FALSE(VerifyState(db_key_2, attributes_2).empty());

    // Verification should fail if ASIC DB table is missing.
    table.del("SAI_OBJECT_TYPE_ROUTE_ENTRY:{\"dest\":\"10.11.12.0/"
              "24\",\"switch_id\":\"oid:0x0\",\"vr\":\"oid:0x6f\"}");
    table.del("SAI_OBJECT_TYPE_ROUTE_ENTRY:{\"dest\":\"2001:db8:1::/"
              "32\",\"switch_id\":\"oid:0x0\",\"vr\":\"oid:0x6f\"}");
    EXPECT_FALSE(VerifyState(db_key_1, attributes_1).empty());
    EXPECT_FALSE(VerifyState(db_key_2, attributes_2).empty());
    table.set("SAI_OBJECT_TYPE_ROUTE_ENTRY:{\"dest\":\"10.11.12.0/"
              "24\",\"switch_id\":\"oid:0x0\",\"vr\":\"oid:0x6f\"}",
              std::vector<swss::FieldValueTuple>{
                  swss::FieldValueTuple{"SAI_ROUTE_ENTRY_ATTR_PACKET_ACTION", "SAI_PACKET_ACTION_DROP"},
                  swss::FieldValueTuple{"SAI_ROUTE_ENTRY_ATTR_NEXT_HOP_ID", "oid:0x0"}});
    table.set("SAI_OBJECT_TYPE_ROUTE_ENTRY:{\"dest\":\"2001:db8:1::/"
              "32\",\"switch_id\":\"oid:0x0\",\"vr\":\"oid:0x6f\"}",
              std::vector<swss::FieldValueTuple>{swss::FieldValueTuple{"SAI_ROUTE_ENTRY_ATTR_NEXT_HOP_ID", "oid:0x1"},
                                                 swss::FieldValueTuple{"SAI_ROUTE_ENTRY_ATTR_META_DATA", "1"}});
}

TEST_F(RouteManagerTest, CreateSetMulticastGroupIdRouteSucceeds) {
  // We eventually want a prefix, not an exact match address.
  // auto swss_ipv6_route_prefix = swss::IpPrefix(kIpv6Prefix);
  // sai_ip_prefix_t sai_ipv6_route_prefix;
  // copy(sai_ipv6_route_prefix, swss_ipv6_route_prefix);
  auto swss_ipv6_route_prefix = swss::IpPrefix(kIpv6Prefix3);
  sai_ip_address_t sai_ipv6_address;
  copy(sai_ipv6_address, swss_ipv6_route_prefix.getIp());
  auto route_entry_ipv6 = GenerateP4RouteEntry(gVrfName, swss_ipv6_route_prefix,
                                               p4orch::kSetMulticastGroupId,
                                               kMulticastGroupId1, kMetadata1);
  auto swss_ipv4_route_prefix = swss::IpPrefix(kIpv4Prefix4);
  sai_ip_address_t sai_ipv4_address;
  copy(sai_ipv4_address, swss_ipv4_route_prefix.getIp());
  auto route_entry_ipv4 = GenerateP4RouteEntry(gVrfName, swss_ipv4_route_prefix,
                                               p4orch::kSetMulticastGroupId,
                                               kMulticastGroupId2, kMetadata1);
  sai_ipmc_entry_t exp_sai_ipmc_entry_v6;
  exp_sai_ipmc_entry_v6.switch_id = gSwitchId;
  exp_sai_ipmc_entry_v6.vr_id = gVrfOid;
  exp_sai_ipmc_entry_v6.type = SAI_IPMC_ENTRY_TYPE_XG;
  exp_sai_ipmc_entry_v6.destination = sai_ipv6_address;
  exp_sai_ipmc_entry_v6.source = sai_ipv6_address;

  sai_ipmc_entry_t exp_sai_ipmc_entry_v4;
  exp_sai_ipmc_entry_v4.switch_id = gSwitchId;
  exp_sai_ipmc_entry_v4.vr_id = gVrfOid;
  exp_sai_ipmc_entry_v4.type = SAI_IPMC_ENTRY_TYPE_XG;
  exp_sai_ipmc_entry_v4.destination = sai_ipv4_address;
  exp_sai_ipmc_entry_v4.source = sai_ipv4_address;

  sai_attribute_t exp_sai_attr;

  std::vector<sai_attribute_t> exp_sai_attrs_v6;
  exp_sai_attr.id = SAI_IPMC_ENTRY_ATTR_PACKET_ACTION;
  exp_sai_attr.value.s32 = SAI_PACKET_ACTION_FORWARD;
  exp_sai_attrs_v6.push_back(exp_sai_attr);
  exp_sai_attr.id = SAI_IPMC_ENTRY_ATTR_OUTPUT_GROUP_ID;
  exp_sai_attr.value.oid = kMulticastGroupOid1;
  exp_sai_attrs_v6.push_back(exp_sai_attr);
  // exp_sai_attr.id = SAI_IPMC_ENTRY_ATTR_COUNTER_ID;
  // exp_sai_attr.value.oid = kMulticastCounterOid1;
  // exp_sai_attrs_v6.push_back(exp_sai_attr);

  std::vector<sai_attribute_t> exp_sai_attrs_v4;
  exp_sai_attr.id = SAI_IPMC_ENTRY_ATTR_PACKET_ACTION;
  exp_sai_attr.value.s32 = SAI_PACKET_ACTION_FORWARD;
  exp_sai_attrs_v4.push_back(exp_sai_attr);
  exp_sai_attr.id = SAI_IPMC_ENTRY_ATTR_OUTPUT_GROUP_ID;
  exp_sai_attr.value.oid = kMulticastGroupOid2;
  exp_sai_attrs_v4.push_back(exp_sai_attr);
  // exp_sai_attr.id = SAI_IPMC_ENTRY_ATTR_COUNTER_ID;
  // exp_sai_attr.value.oid = kMulticastCounterOid2;
  // exp_sai_attrs_v4.push_back(exp_sai_attr);

  EXPECT_CALL(mock_sai_rpf_group_, create_rpf_group(_, _, 0, _))
      .WillOnce(
          DoAll(SetArgPointee<0>(kRpfGroupOid1), Return(SAI_STATUS_SUCCESS)));
  EXPECT_CALL(mock_sai_ipmc_, create_ipmc_entry(_, _, _))
      .WillOnce(Return(SAI_STATUS_SUCCESS))
      .WillOnce(Return(SAI_STATUS_SUCCESS));

  // Create artificial multicast group object.
  AddMulticastGroup(kMulticastGroupId1, kMulticastGroupOid1);
  AddMulticastGroup(kMulticastGroupId2, kMulticastGroupOid2);

  EXPECT_THAT(CreateMulticastRouteEntries(std::vector<P4RouteEntry>{
                  route_entry_ipv6, route_entry_ipv4}),
              ArrayEq(std::vector<StatusCode>{StatusCode::SWSS_RC_SUCCESS,
                                              StatusCode::SWSS_RC_SUCCESS}));
  VerifyMulticastRouteEntry(route_entry_ipv6, sai_ipv6_address, gVrfOid);
  VerifyMulticastRouteEntry(route_entry_ipv4, sai_ipv4_address, gVrfOid);
}

TEST_F(RouteManagerTest, CreateSetMulticastGroupIdRouteMissingGroupOidFail) {
  auto swss_ipv6_route_prefix = swss::IpPrefix(kIpv6Prefix3);
  sai_ip_address_t sai_ipv6_address;
  copy(sai_ipv6_address, swss_ipv6_route_prefix.getIp());
  auto route_entry_ipv6 = GenerateP4RouteEntry(gVrfName, swss_ipv6_route_prefix,
                                               p4orch::kSetMulticastGroupId,
                                               kMulticastGroupId1, kMetadata1);
  auto swss_ipv4_route_prefix = swss::IpPrefix(kIpv4Prefix4);
  sai_ip_address_t sai_ipv4_address;
  copy(sai_ipv4_address, swss_ipv4_route_prefix.getIp());
  auto route_entry_ipv4 = GenerateP4RouteEntry(gVrfName, swss_ipv4_route_prefix,
                                               p4orch::kSetMulticastGroupId,
                                               kMulticastGroupId2, kMetadata1);

  // No Multicast group objects exist.
  EXPECT_CALL(mock_sai_rpf_group_, create_rpf_group(_, _, 0, _))
      .WillOnce(
          DoAll(SetArgPointee<0>(kRpfGroupOid1), Return(SAI_STATUS_SUCCESS)));
  EXPECT_THAT(
      CreateMulticastRouteEntries(
          std::vector<P4RouteEntry>{route_entry_ipv6, route_entry_ipv4}),
      ArrayEq(std::vector<StatusCode>{StatusCode::SWSS_RC_NOT_FOUND,
                                      StatusCode::SWSS_RC_NOT_EXECUTED}));

  auto* route_entry_ptr_v6 = GetRouteEntry(route_entry_ipv6.route_entry_key);
  auto* route_entry_ptr_v4 = GetRouteEntry(route_entry_ipv4.route_entry_key);
  EXPECT_EQ(route_entry_ptr_v6, nullptr);
  EXPECT_EQ(route_entry_ptr_v4, nullptr);
}

TEST_F(RouteManagerTest, CreateSetMulticastGroupIdSaiCreateFail) {
  auto swss_ipv6_route_prefix = swss::IpPrefix(kIpv6Prefix3);
  sai_ip_address_t sai_ipv6_address;
  copy(sai_ipv6_address, swss_ipv6_route_prefix.getIp());
  auto route_entry_ipv6 = GenerateP4RouteEntry(gVrfName, swss_ipv6_route_prefix,
                                               p4orch::kSetMulticastGroupId,
                                               kMulticastGroupId1, kMetadata1);
  auto swss_ipv4_route_prefix = swss::IpPrefix(kIpv4Prefix4);
  sai_ip_address_t sai_ipv4_address;
  copy(sai_ipv4_address, swss_ipv4_route_prefix.getIp());
  auto route_entry_ipv4 = GenerateP4RouteEntry(gVrfName, swss_ipv4_route_prefix,
                                               p4orch::kSetMulticastGroupId,
                                               kMulticastGroupId2, kMetadata1);

  // Create artificial multicast group object.
  AddMulticastGroup(kMulticastGroupId1, kMulticastGroupOid1);
  AddMulticastGroup(kMulticastGroupId2, kMulticastGroupOid2);

  EXPECT_CALL(mock_sai_rpf_group_, create_rpf_group(_, _, 0, _))
      .WillOnce(
          DoAll(SetArgPointee<0>(kRpfGroupOid1), Return(SAI_STATUS_SUCCESS)));
  EXPECT_CALL(mock_sai_ipmc_, create_ipmc_entry(_, _, _))
      .WillOnce(Return(SAI_STATUS_FAILURE));

  EXPECT_THAT(
      CreateMulticastRouteEntries(
          std::vector<P4RouteEntry>{route_entry_ipv6, route_entry_ipv4}),
      ArrayEq(std::vector<StatusCode>{StatusCode::SWSS_RC_UNKNOWN,
                                      StatusCode::SWSS_RC_NOT_EXECUTED}));

  auto* route_entry_ptr_v6 = GetRouteEntry(route_entry_ipv6.route_entry_key);
  auto* route_entry_ptr_v4 = GetRouteEntry(route_entry_ipv4.route_entry_key);
  EXPECT_EQ(route_entry_ptr_v6, nullptr);
  EXPECT_EQ(route_entry_ptr_v4, nullptr);
}

TEST_F(RouteManagerTest, CreateSetMulticastGroupIdCheckRpfGroupId) {
  auto swss_ipv6_route_prefix = swss::IpPrefix(kIpv6Prefix3);
  sai_ip_address_t sai_ipv6_address;
  copy(sai_ipv6_address, swss_ipv6_route_prefix.getIp());
  auto route_entry_ipv6 = GenerateP4RouteEntry(gVrfName, swss_ipv6_route_prefix,
                                               p4orch::kSetMulticastGroupId,
                                               kMulticastGroupId1, kMetadata1);

  // Create artificial multicast group object.
  AddMulticastGroup(kMulticastGroupId1, kMulticastGroupOid1);

  EXPECT_CALL(mock_sai_rpf_group_, create_rpf_group(_, _, 0, _))
      .WillOnce(
          DoAll(SetArgPointee<0>(kRpfGroupOid1), Return(SAI_STATUS_SUCCESS)));

  std::vector<sai_attribute_t> exp_ipmc_attrs;
  sai_attribute_t attr;
  attr.id = SAI_IPMC_ENTRY_ATTR_PACKET_ACTION;
  attr.value.s32 = SAI_PACKET_ACTION_FORWARD;
  exp_ipmc_attrs.push_back(attr);
  attr.id = SAI_IPMC_ENTRY_ATTR_OUTPUT_GROUP_ID;
  attr.value.oid = kMulticastGroupOid1;
  exp_ipmc_attrs.push_back(attr);
  attr.id = SAI_IPMC_ENTRY_ATTR_RPF_GROUP_ID;
  attr.value.oid = kRpfGroupOid1;
  exp_ipmc_attrs.push_back(attr);

  EXPECT_CALL(mock_sai_ipmc_,
              create_ipmc_entry(_, 3, AttrArrayEq(exp_ipmc_attrs)))
      .WillOnce(Return(SAI_STATUS_SUCCESS));
  EXPECT_THAT(
      CreateMulticastRouteEntries(std::vector<P4RouteEntry>{route_entry_ipv6}),
      ArrayEq(std::vector<StatusCode>{StatusCode::SWSS_RC_SUCCESS}));

  auto* route_entry_ptr_v6 = GetRouteEntry(route_entry_ipv6.route_entry_key);
  EXPECT_NE(route_entry_ptr_v6, nullptr);
}

TEST_F(RouteManagerTest, CreateSetMulticastGroupIdFailToCreateRpfGroup) {
  auto swss_ipv6_route_prefix = swss::IpPrefix(kIpv6Prefix3);
  sai_ip_address_t sai_ipv6_address;
  copy(sai_ipv6_address, swss_ipv6_route_prefix.getIp());
  auto route_entry_ipv6 = GenerateP4RouteEntry(gVrfName, swss_ipv6_route_prefix,
                                               p4orch::kSetMulticastGroupId,
                                               kMulticastGroupId1, kMetadata1);

  // Create artificial multicast group object.
  AddMulticastGroup(kMulticastGroupId1, kMulticastGroupOid1);

  EXPECT_CALL(mock_sai_rpf_group_, create_rpf_group(_, _, 0, _))
      .WillOnce(Return(SAI_STATUS_FAILURE));

  EXPECT_THAT(
      CreateMulticastRouteEntries(std::vector<P4RouteEntry>{route_entry_ipv6}),
      ArrayEq(std::vector<StatusCode>{StatusCode::SWSS_RC_UNKNOWN}));

  auto* route_entry_ptr_v6 = GetRouteEntry(route_entry_ipv6.route_entry_key);
  EXPECT_EQ(route_entry_ptr_v6, nullptr);
}
