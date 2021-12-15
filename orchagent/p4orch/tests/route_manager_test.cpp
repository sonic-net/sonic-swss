#include "route_manager.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <functional>
#include <map>
#include <string>

#include "ipprefix.h"
#include "json.hpp"
#include "mock_response_publisher.h"
#include "mock_sai_route.h"
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
using ::testing::StrictMock;
using ::testing::Truly;

extern sai_object_id_t gSwitchId;
extern sai_object_id_t gVirtualRouterId;
extern sai_object_id_t gVrfOid;
extern char *gVrfName;
extern sai_route_api_t *sai_route_api;
extern VRFOrch *gVrfOrch;

namespace
{

constexpr char *kIpv4Prefix = "10.11.12.0/24";
constexpr char *kIpv6Prefix = "2001:db8:1::/32";
constexpr char *kNexthopId1 = "ju1u32m1.atl11:qe-3/7";
constexpr sai_object_id_t kNexthopOid1 = 1;
constexpr char *kNexthopId2 = "ju1u32m2.atl11:qe-3/7";
constexpr sai_object_id_t kNexthopOid2 = 2;
constexpr char *kWcmpGroup1 = "wcmp-group-1";
constexpr sai_object_id_t kWcmpGroupOid1 = 3;
constexpr char *kWcmpGroup2 = "wcmp-group-2";
constexpr sai_object_id_t kWcmpGroupOid2 = 4;

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

// Matches the sai_route_entry_t argument.
bool MatchSaiRouteEntry(const sai_ip_prefix_t &expected_prefix, const sai_route_entry_t *route_entry,
                        const sai_object_id_t expected_vrf_oid)
{
    if (route_entry == nullptr)
    {
        return false;
    }
    if (route_entry->vr_id != expected_vrf_oid)
    {
        return false;
    }
    if (route_entry->switch_id != gSwitchId)
    {
        return false;
    }
    if (!PrefixCmp(&route_entry->destination, &expected_prefix))
    {
        return false;
    }
    return true;
}

// Matches the action type sai_attribute_t argument.
bool MatchSaiAttributeAction(sai_packet_action_t expected_action, const sai_attribute_t *attr)
{
    if (attr == nullptr)
    {
        return false;
    }
    if (attr->id != SAI_ROUTE_ENTRY_ATTR_PACKET_ACTION)
    {
        return false;
    }
    if (attr->value.s32 != expected_action)
    {
        return false;
    }
    return true;
}

// Matches the nexthop ID type sai_attribute_t argument.
bool MatchSaiAttributeNexthopId(sai_object_id_t expected_oid, const sai_attribute_t *attr)
{
    if (attr == nullptr)
    {
        return false;
    }
    if (attr->id != SAI_ROUTE_ENTRY_ATTR_NEXT_HOP_ID)
    {
        return false;
    }
    if (attr->value.oid != expected_oid)
    {
        return false;
    }
    return true;
}

} // namespace

class RouteManagerTest : public ::testing::Test
{
  protected:
    RouteManagerTest() : route_manager_(&p4_oid_mapper_, gVrfOrch, &publisher_)
    {
    }

    void SetUp() override
    {
        mock_sai_route = &mock_sai_route_;
        sai_route_api->create_route_entry = create_route_entry;
        sai_route_api->remove_route_entry = remove_route_entry;
        sai_route_api->set_route_entry_attribute = set_route_entry_attribute;
        sai_route_api->get_route_entry_attribute = get_route_entry_attribute;
        sai_route_api->create_route_entries = create_route_entries;
        sai_route_api->remove_route_entries = remove_route_entries;
        sai_route_api->set_route_entries_attribute = set_route_entries_attribute;
        sai_route_api->get_route_entries_attribute = get_route_entries_attribute;
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

    ReturnCode ValidateRouteEntry(const P4RouteEntry &route_entry)
    {
        return route_manager_.validateRouteEntry(route_entry);
    }

    ReturnCode ValidateSetRouteEntry(const P4RouteEntry &route_entry)
    {
        return route_manager_.validateSetRouteEntry(route_entry);
    }

    ReturnCode ValidateDelRouteEntry(const P4RouteEntry &route_entry)
    {
        return route_manager_.validateDelRouteEntry(route_entry);
    }

    ReturnCode CreateRouteEntry(const P4RouteEntry &route_entry)
    {
        return route_manager_.createRouteEntry(route_entry);
    }

    ReturnCode UpdateRouteEntry(const P4RouteEntry &route_entry)
    {
        return route_manager_.updateRouteEntry(route_entry);
    }

    ReturnCode DeleteRouteEntry(const P4RouteEntry &route_entry)
    {
        return route_manager_.deleteRouteEntry(route_entry);
    }

    void Enqueue(const swss::KeyOpFieldsValuesTuple &entry)
    {
        route_manager_.enqueue(entry);
    }

    void Drain()
    {
        route_manager_.drain();
    }

    // Sets up a nexthop route entry for test.
    void SetupNexthopIdRouteEntry(const std::string &vrf_id, const swss::IpPrefix &route_prefix,
                                  const std::string &nexthop_id, sai_object_id_t nexthop_oid)
    {
        P4RouteEntry route_entry = {};
        route_entry.vrf_id = vrf_id;
        route_entry.route_prefix = route_prefix;
        route_entry.action = p4orch::kSetNexthopId;
        route_entry.nexthop_id = nexthop_id;
        route_entry.route_entry_key = KeyGenerator::generateRouteKey(route_entry.vrf_id, route_entry.route_prefix);
        p4_oid_mapper_.setOID(SAI_OBJECT_TYPE_NEXT_HOP, KeyGenerator::generateNextHopKey(route_entry.nexthop_id),
                              nexthop_oid);

        EXPECT_CALL(mock_sai_route_, create_route_entry(_, _, _)).WillOnce(Return(SAI_STATUS_SUCCESS));
        EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS, CreateRouteEntry(route_entry));
    }

    // Sets up a wcmp route entry for test.
    void SetupWcmpGroupRouteEntry(const std::string &vrf_id, const swss::IpPrefix &route_prefix,
                                  const std::string &wcmp_group_id, sai_object_id_t wcmp_group_oid)
    {
        P4RouteEntry route_entry = {};
        route_entry.vrf_id = vrf_id;
        route_entry.route_prefix = route_prefix;
        route_entry.action = p4orch::kSetWcmpGroupId;
        route_entry.wcmp_group = wcmp_group_id;
        route_entry.route_entry_key = KeyGenerator::generateRouteKey(route_entry.vrf_id, route_entry.route_prefix);
        p4_oid_mapper_.setOID(SAI_OBJECT_TYPE_NEXT_HOP_GROUP,
                              KeyGenerator::generateWcmpGroupKey(route_entry.wcmp_group), wcmp_group_oid);

        EXPECT_CALL(mock_sai_route_, create_route_entry(_, _, _)).WillOnce(Return(SAI_STATUS_SUCCESS));
        EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS, CreateRouteEntry(route_entry));
    }

    // Verifies the two given route entries are identical.
    void VerifyRouteEntriesEq(const P4RouteEntry &x, const P4RouteEntry &y)
    {
        EXPECT_EQ(x.route_entry_key, y.route_entry_key);
        EXPECT_EQ(x.vrf_id, y.vrf_id);
        EXPECT_EQ(x.route_prefix, y.route_prefix);
        EXPECT_EQ(x.action, y.action);
        EXPECT_EQ(x.nexthop_id, y.nexthop_id);
        EXPECT_EQ(x.wcmp_group, y.wcmp_group);
        EXPECT_EQ(x.sai_route_entry.vr_id, y.sai_route_entry.vr_id);
        EXPECT_EQ(x.sai_route_entry.switch_id, y.sai_route_entry.switch_id);
        EXPECT_TRUE(PrefixCmp(&x.sai_route_entry.destination, &y.sai_route_entry.destination));
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

    StrictMock<MockSaiRoute> mock_sai_route_;
    MockResponsePublisher publisher_;
    P4OidMapper p4_oid_mapper_;
    RouteManager route_manager_;
};

TEST_F(RouteManagerTest, MergeRouteEntryWithNexthopIdActionDestTest)
{
    auto swss_ipv4_route_prefix = swss::IpPrefix(kIpv4Prefix);
    P4RouteEntry dest = {};
    dest.vrf_id = gVrfName;
    dest.route_prefix = swss_ipv4_route_prefix;
    dest.action = p4orch::kSetNexthopId;
    dest.nexthop_id = kNexthopId1;
    dest.route_entry_key = KeyGenerator::generateRouteKey(dest.vrf_id, dest.route_prefix);
    dest.sai_route_entry.vr_id = gVrfOid;
    dest.sai_route_entry.switch_id = gSwitchId;
    copy(dest.sai_route_entry.destination, swss_ipv4_route_prefix);

    // Source is identical to destination.
    P4RouteEntry src = {};
    src.vrf_id = gVrfName;
    src.route_prefix = swss_ipv4_route_prefix;
    src.action = p4orch::kSetNexthopId;
    src.nexthop_id = kNexthopId1;
    src.route_entry_key = KeyGenerator::generateRouteKey(src.vrf_id, src.route_prefix);
    P4RouteEntry ret = {};
    EXPECT_FALSE(MergeRouteEntry(dest, src, &ret));
    VerifyRouteEntriesEq(dest, ret);

    // Source has different nexthop ID.
    src = {};
    src.vrf_id = gVrfName;
    src.route_prefix = swss_ipv4_route_prefix;
    src.nexthop_id = kNexthopId2;
    src.route_entry_key = KeyGenerator::generateRouteKey(src.vrf_id, src.route_prefix);
    EXPECT_TRUE(MergeRouteEntry(dest, src, &ret));
    P4RouteEntry expect_entry = dest;
    expect_entry.nexthop_id = kNexthopId2;
    VerifyRouteEntriesEq(expect_entry, ret);

    // Source has wcmp group action and dest has nexhop ID action.
    src = {};
    src.vrf_id = gVrfName;
    src.route_prefix = swss_ipv4_route_prefix;
    src.action = p4orch::kSetWcmpGroupId;
    src.wcmp_group = kWcmpGroup1;
    src.route_entry_key = KeyGenerator::generateRouteKey(src.vrf_id, src.route_prefix);
    EXPECT_TRUE(MergeRouteEntry(dest, src, &ret));
    expect_entry = dest;
    expect_entry.nexthop_id = "";
    expect_entry.action = p4orch::kSetWcmpGroupId;
    expect_entry.wcmp_group = kWcmpGroup1;
    VerifyRouteEntriesEq(expect_entry, ret);

    // Source has drop action and dest has nexhop ID action.
    src = {};
    src.vrf_id = gVrfName;
    src.route_prefix = swss_ipv4_route_prefix;
    src.action = p4orch::kDrop;
    src.route_entry_key = KeyGenerator::generateRouteKey(src.vrf_id, src.route_prefix);
    EXPECT_TRUE(MergeRouteEntry(dest, src, &ret));
    expect_entry = dest;
    expect_entry.nexthop_id = "";
    expect_entry.action = p4orch::kDrop;
    VerifyRouteEntriesEq(expect_entry, ret);
}

TEST_F(RouteManagerTest, MergeRouteEntryWithWcmpGroupActionDestTest)
{
    auto swss_ipv4_route_prefix = swss::IpPrefix(kIpv4Prefix);
    P4RouteEntry dest = {};
    dest.vrf_id = gVrfName;
    dest.route_prefix = swss_ipv4_route_prefix;
    dest.action = p4orch::kSetWcmpGroupId;
    dest.wcmp_group = kWcmpGroup1;
    dest.route_entry_key = KeyGenerator::generateRouteKey(dest.vrf_id, dest.route_prefix);
    dest.sai_route_entry.vr_id = gVrfOid;
    dest.sai_route_entry.switch_id = gSwitchId;
    copy(dest.sai_route_entry.destination, swss_ipv4_route_prefix);

    // Source is identical to destination.
    P4RouteEntry src = {};
    src.vrf_id = gVrfName;
    src.route_prefix = swss_ipv4_route_prefix;
    src.action = p4orch::kSetWcmpGroupId;
    src.wcmp_group = kWcmpGroup1;
    src.route_entry_key = KeyGenerator::generateRouteKey(src.vrf_id, src.route_prefix);
    P4RouteEntry ret = {};
    EXPECT_FALSE(MergeRouteEntry(dest, src, &ret));
    VerifyRouteEntriesEq(dest, ret);

    // Source has different wcmp group.
    src = {};
    src.vrf_id = gVrfName;
    src.route_prefix = swss_ipv4_route_prefix;
    src.wcmp_group = kWcmpGroup2;
    src.route_entry_key = KeyGenerator::generateRouteKey(dest.vrf_id, dest.route_prefix);
    EXPECT_TRUE(MergeRouteEntry(dest, src, &ret));
    P4RouteEntry expect_entry = dest;
    expect_entry.wcmp_group = kWcmpGroup2;
    VerifyRouteEntriesEq(expect_entry, ret);

    // Source has nexthop ID action and dest has wcmp group action.
    src = {};
    src.vrf_id = gVrfName;
    src.route_prefix = swss_ipv4_route_prefix;
    src.action = p4orch::kSetNexthopId;
    src.nexthop_id = kNexthopId1;
    src.route_entry_key = KeyGenerator::generateRouteKey(src.vrf_id, src.route_prefix);
    EXPECT_TRUE(MergeRouteEntry(dest, src, &ret));
    expect_entry = dest;
    expect_entry.wcmp_group = "";
    expect_entry.action = p4orch::kSetNexthopId;
    expect_entry.nexthop_id = kNexthopId1;
    VerifyRouteEntriesEq(expect_entry, ret);

    // Source has drop action and dest has wcmp group action.
    src = {};
    src.vrf_id = gVrfName;
    src.route_prefix = swss_ipv4_route_prefix;
    src.action = p4orch::kDrop;
    src.route_entry_key = KeyGenerator::generateRouteKey(src.vrf_id, src.route_prefix);
    EXPECT_TRUE(MergeRouteEntry(dest, src, &ret));
    expect_entry = dest;
    expect_entry.wcmp_group = "";
    expect_entry.action = p4orch::kDrop;
    VerifyRouteEntriesEq(expect_entry, ret);
}

TEST_F(RouteManagerTest, MergeRouteEntryWithDropActionDestTest)
{
    auto swss_ipv4_route_prefix = swss::IpPrefix(kIpv4Prefix);
    P4RouteEntry dest = {};
    dest.vrf_id = gVrfName;
    dest.route_prefix = swss_ipv4_route_prefix;
    dest.action = p4orch::kDrop;
    dest.route_entry_key = KeyGenerator::generateRouteKey(dest.vrf_id, dest.route_prefix);
    dest.sai_route_entry.vr_id = gVrfOid;
    dest.sai_route_entry.switch_id = gSwitchId;
    copy(dest.sai_route_entry.destination, swss_ipv4_route_prefix);

    // Source is identical to destination.
    P4RouteEntry src = {};
    src.vrf_id = gVrfName;
    src.route_prefix = swss_ipv4_route_prefix;
    src.action = p4orch::kDrop;
    src.route_entry_key = KeyGenerator::generateRouteKey(src.vrf_id, src.route_prefix);
    P4RouteEntry ret = {};
    EXPECT_FALSE(MergeRouteEntry(dest, src, &ret));
    VerifyRouteEntriesEq(dest, ret);

    // Source has nexthop ID action and dest has drop action.
    src = {};
    src.vrf_id = gVrfName;
    src.route_prefix = swss_ipv4_route_prefix;
    src.action = p4orch::kSetNexthopId;
    src.nexthop_id = kNexthopId1;
    src.route_entry_key = KeyGenerator::generateRouteKey(src.vrf_id, src.route_prefix);
    EXPECT_TRUE(MergeRouteEntry(dest, src, &ret));
    P4RouteEntry expect_entry = dest;
    expect_entry.action = p4orch::kSetNexthopId;
    expect_entry.nexthop_id = kNexthopId1;
    VerifyRouteEntriesEq(expect_entry, ret);

    // Source has wcmp group action and dest has drop action.
    src = {};
    src.vrf_id = gVrfName;
    src.route_prefix = swss_ipv4_route_prefix;
    src.action = p4orch::kSetWcmpGroupId;
    src.wcmp_group = kWcmpGroup1;
    src.route_entry_key = KeyGenerator::generateRouteKey(src.vrf_id, src.route_prefix);
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
    P4RouteEntry expect_entry = {};
    expect_entry.vrf_id = "b4-traffic";
    expect_entry.route_prefix = swss::IpPrefix("10.11.12.0/24");
    expect_entry.action = p4orch::kSetNexthopId;
    expect_entry.nexthop_id = kNexthopId1;
    expect_entry.route_entry_key = KeyGenerator::generateRouteKey(expect_entry.vrf_id, expect_entry.route_prefix);
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
    P4RouteEntry expect_entry = {};
    expect_entry.vrf_id = "b4-traffic";
    expect_entry.route_prefix = swss::IpPrefix("10.11.12.0/24");
    expect_entry.action = p4orch::kSetWcmpGroupId;
    expect_entry.wcmp_group = kWcmpGroup1;
    expect_entry.route_entry_key = KeyGenerator::generateRouteKey(expect_entry.vrf_id, expect_entry.route_prefix);
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
    P4RouteEntry expect_entry = {};
    expect_entry.vrf_id = "b4-traffic";
    expect_entry.route_prefix = swss::IpPrefix("2001:db8:1::/32");
    expect_entry.action = p4orch::kDrop;
    expect_entry.route_entry_key = KeyGenerator::generateRouteKey(expect_entry.vrf_id, expect_entry.route_prefix);
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
    P4RouteEntry expect_entry = {};
    expect_entry.vrf_id = "b4-traffic";
    expect_entry.route_prefix = swss::IpPrefix("0.0.0.0/0");
    expect_entry.action = p4orch::kDrop;
    expect_entry.route_entry_key = KeyGenerator::generateRouteKey(expect_entry.vrf_id, expect_entry.route_prefix);
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
    P4RouteEntry expect_entry = {};
    expect_entry.vrf_id = "b4-traffic";
    expect_entry.route_prefix = swss::IpPrefix("::/0");
    expect_entry.action = p4orch::kDrop;
    expect_entry.route_entry_key = KeyGenerator::generateRouteKey(expect_entry.vrf_id, expect_entry.route_prefix);
    VerifyRouteEntriesEq(expect_entry, route_entry);
}

TEST_F(RouteManagerTest, ValidateRouteEntryTest)
{
    auto swss_ipv4_route_prefix = swss::IpPrefix(kIpv4Prefix);

    // ValidateRouteEntry should fail when the nexthop does not exist in
    // centralized map.
    P4RouteEntry route_entry = {};
    route_entry.vrf_id = gVrfName;
    route_entry.route_prefix = swss_ipv4_route_prefix;
    route_entry.action = p4orch::kSetNexthopId;
    route_entry.nexthop_id = kNexthopId1;
    route_entry.route_entry_key = KeyGenerator::generateRouteKey(route_entry.vrf_id, route_entry.route_prefix);
    EXPECT_EQ(StatusCode::SWSS_RC_NOT_FOUND, ValidateRouteEntry(route_entry));
    p4_oid_mapper_.setOID(SAI_OBJECT_TYPE_NEXT_HOP, KeyGenerator::generateNextHopKey(route_entry.nexthop_id),
                          kNexthopOid1);
    EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS, ValidateRouteEntry(route_entry));
}

TEST_F(RouteManagerTest, ValidateRouteEntryWcmpGroupActionWithInvalidWcmpGroupShouldFail)
{
    auto swss_ipv4_route_prefix = swss::IpPrefix(kIpv4Prefix);
    P4RouteEntry route_entry = {};
    route_entry.vrf_id = gVrfName;
    route_entry.route_prefix = swss_ipv4_route_prefix;
    route_entry.action = p4orch::kSetWcmpGroupId;
    route_entry.wcmp_group = kWcmpGroup1;
    route_entry.route_entry_key = KeyGenerator::generateRouteKey(route_entry.vrf_id, route_entry.route_prefix);
    EXPECT_EQ(StatusCode::SWSS_RC_NOT_FOUND, ValidateRouteEntry(route_entry));
}

TEST_F(RouteManagerTest, ValidateRouteEntryWcmpGroupActionWithValidWcmpGroupShouldSucceed)
{
    auto swss_ipv4_route_prefix = swss::IpPrefix(kIpv4Prefix);
    P4RouteEntry route_entry = {};
    route_entry.vrf_id = gVrfName;
    route_entry.route_prefix = swss_ipv4_route_prefix;
    route_entry.action = p4orch::kSetWcmpGroupId;
    route_entry.wcmp_group = kWcmpGroup1;
    route_entry.route_entry_key = KeyGenerator::generateRouteKey(route_entry.vrf_id, route_entry.route_prefix);
    p4_oid_mapper_.setOID(SAI_OBJECT_TYPE_NEXT_HOP_GROUP, KeyGenerator::generateWcmpGroupKey(route_entry.wcmp_group),
                          kWcmpGroupOid1);
    EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS, ValidateRouteEntry(route_entry));
}

TEST_F(RouteManagerTest, ValidateSetRouteEntryDoesNotExistInManagerShouldFail)
{
    auto swss_ipv4_route_prefix = swss::IpPrefix(kIpv4Prefix);
    P4RouteEntry route_entry = {};
    route_entry.vrf_id = gVrfName;
    route_entry.route_prefix = swss_ipv4_route_prefix;
    route_entry.nexthop_id = kNexthopId1;
    route_entry.route_entry_key = KeyGenerator::generateRouteKey(route_entry.vrf_id, route_entry.route_prefix);
    EXPECT_EQ(StatusCode::SWSS_RC_INVALID_PARAM, ValidateSetRouteEntry(route_entry));
}

TEST_F(RouteManagerTest, ValidateSetRouteEntryExistsInMapperDoesNotExistInManagerShouldFail)
{
    auto swss_ipv4_route_prefix = swss::IpPrefix(kIpv4Prefix);
    P4RouteEntry route_entry = {};
    route_entry.vrf_id = gVrfName;
    route_entry.route_prefix = swss_ipv4_route_prefix;
    route_entry.action = p4orch::kSetNexthopId;
    route_entry.nexthop_id = kNexthopId1;
    route_entry.route_entry_key = KeyGenerator::generateRouteKey(route_entry.vrf_id, route_entry.route_prefix);
    p4_oid_mapper_.setDummyOID(SAI_OBJECT_TYPE_ROUTE_ENTRY, route_entry.route_entry_key);
    EXPECT_EQ(StatusCode::SWSS_RC_NOT_FOUND, ValidateSetRouteEntry(route_entry));
}

TEST_F(RouteManagerTest, ValidateSetRouteEntryExistsInManagerDoesNotExistInMapperShouldFail)
{
    auto swss_ipv4_route_prefix = swss::IpPrefix(kIpv4Prefix);
    SetupNexthopIdRouteEntry(gVrfName, swss_ipv4_route_prefix, kNexthopId1, kNexthopOid1);
    P4RouteEntry route_entry = {};
    route_entry.vrf_id = gVrfName;
    route_entry.route_prefix = swss_ipv4_route_prefix;
    route_entry.nexthop_id = kNexthopId2;
    route_entry.route_entry_key = KeyGenerator::generateRouteKey(route_entry.vrf_id, route_entry.route_prefix);
    p4_oid_mapper_.eraseOID(SAI_OBJECT_TYPE_ROUTE_ENTRY, route_entry.route_entry_key);
    EXPECT_EQ(StatusCode::SWSS_RC_NOT_FOUND, ValidateSetRouteEntry(route_entry));
}

TEST_F(RouteManagerTest, ValidateSetRouteEntryNexthopIdActionWithoutNexthopIdShouldFail)
{
    auto swss_ipv4_route_prefix = swss::IpPrefix(kIpv4Prefix);
    P4RouteEntry route_entry = {};
    route_entry.vrf_id = gVrfName;
    route_entry.route_prefix = swss_ipv4_route_prefix;
    route_entry.action = p4orch::kSetNexthopId;
    route_entry.route_entry_key = KeyGenerator::generateRouteKey(route_entry.vrf_id, route_entry.route_prefix);
    EXPECT_EQ(StatusCode::SWSS_RC_INVALID_PARAM, ValidateSetRouteEntry(route_entry));
}

TEST_F(RouteManagerTest, ValidateSetRouteEntryNexthopIdActionWithWcmpGroupShouldFail)
{
    auto swss_ipv4_route_prefix = swss::IpPrefix(kIpv4Prefix);
    P4RouteEntry route_entry = {};
    route_entry.vrf_id = gVrfName;
    route_entry.route_prefix = swss_ipv4_route_prefix;
    route_entry.action = p4orch::kSetNexthopId;
    route_entry.nexthop_id = kNexthopId1;
    route_entry.wcmp_group = kWcmpGroup1;
    route_entry.route_entry_key = KeyGenerator::generateRouteKey(route_entry.vrf_id, route_entry.route_prefix);
    EXPECT_EQ(StatusCode::SWSS_RC_INVALID_PARAM, ValidateSetRouteEntry(route_entry));
}

TEST_F(RouteManagerTest, ValidateSetRouteEntryWcmpGroupActionWithoutWcmpGroupShouldFail)
{
    auto swss_ipv4_route_prefix = swss::IpPrefix(kIpv4Prefix);
    P4RouteEntry route_entry = {};
    route_entry.vrf_id = gVrfName;
    route_entry.route_prefix = swss_ipv4_route_prefix;
    route_entry.action = p4orch::kSetWcmpGroupId;
    route_entry.route_entry_key = KeyGenerator::generateRouteKey(route_entry.vrf_id, route_entry.route_prefix);
    EXPECT_EQ(StatusCode::SWSS_RC_INVALID_PARAM, ValidateSetRouteEntry(route_entry));
}

TEST_F(RouteManagerTest, ValidateSetRouteEntryWcmpGroupActionWithNexthopIdShouldFail)
{
    auto swss_ipv4_route_prefix = swss::IpPrefix(kIpv4Prefix);
    P4RouteEntry route_entry = {};
    route_entry.vrf_id = gVrfName;
    route_entry.route_prefix = swss_ipv4_route_prefix;
    route_entry.action = p4orch::kSetWcmpGroupId;
    route_entry.nexthop_id = kNexthopId1;
    route_entry.wcmp_group = kWcmpGroup1;
    route_entry.route_entry_key = KeyGenerator::generateRouteKey(route_entry.vrf_id, route_entry.route_prefix);
    EXPECT_EQ(StatusCode::SWSS_RC_INVALID_PARAM, ValidateSetRouteEntry(route_entry));
}

TEST_F(RouteManagerTest, ValidateSetRouteEntryDropActionWithNexthopIdShouldFail)
{
    auto swss_ipv4_route_prefix = swss::IpPrefix(kIpv4Prefix);
    P4RouteEntry route_entry = {};
    route_entry.vrf_id = gVrfName;
    route_entry.route_prefix = swss_ipv4_route_prefix;
    route_entry.action = p4orch::kDrop;
    route_entry.nexthop_id = kNexthopId1;
    route_entry.route_entry_key = KeyGenerator::generateRouteKey(route_entry.vrf_id, route_entry.route_prefix);
    EXPECT_EQ(StatusCode::SWSS_RC_INVALID_PARAM, ValidateSetRouteEntry(route_entry));
}

TEST_F(RouteManagerTest, ValidateSetRouteEntryDropActionWithWcmpGroupShouldFail)
{
    auto swss_ipv4_route_prefix = swss::IpPrefix(kIpv4Prefix);
    P4RouteEntry route_entry = {};
    route_entry.vrf_id = gVrfName;
    route_entry.route_prefix = swss_ipv4_route_prefix;
    route_entry.action = p4orch::kDrop;
    route_entry.wcmp_group = kWcmpGroup1;
    route_entry.route_entry_key = KeyGenerator::generateRouteKey(route_entry.vrf_id, route_entry.route_prefix);
    EXPECT_EQ(StatusCode::SWSS_RC_INVALID_PARAM, ValidateSetRouteEntry(route_entry));
}

TEST_F(RouteManagerTest, ValidateSetRouteEntryInvalidActionShouldFail)
{
    auto swss_ipv4_route_prefix = swss::IpPrefix(kIpv4Prefix);
    P4RouteEntry route_entry = {};
    route_entry.vrf_id = gVrfName;
    route_entry.route_prefix = swss_ipv4_route_prefix;
    route_entry.action = "invalid";
    route_entry.route_entry_key = KeyGenerator::generateRouteKey(route_entry.vrf_id, route_entry.route_prefix);
    EXPECT_EQ(StatusCode::SWSS_RC_INVALID_PARAM, ValidateSetRouteEntry(route_entry));
}

TEST_F(RouteManagerTest, ValidateSetRouteEntrySucceeds)
{
    auto swss_ipv4_route_prefix = swss::IpPrefix(kIpv4Prefix);
    SetupNexthopIdRouteEntry(gVrfName, swss_ipv4_route_prefix, kNexthopId1, kNexthopOid1);
    P4RouteEntry route_entry = {};
    route_entry.vrf_id = gVrfName;
    route_entry.route_prefix = swss_ipv4_route_prefix;
    route_entry.nexthop_id = kNexthopId2;
    route_entry.route_entry_key = KeyGenerator::generateRouteKey(route_entry.vrf_id, route_entry.route_prefix);
    EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS, ValidateSetRouteEntry(route_entry));
}

TEST_F(RouteManagerTest, ValidateDelRouteEntryDoesNotExistInManagerShouldFail)
{
    auto swss_ipv4_route_prefix = swss::IpPrefix(kIpv4Prefix);
    P4RouteEntry route_entry = {};
    route_entry.vrf_id = gVrfName;
    route_entry.route_prefix = swss_ipv4_route_prefix;
    route_entry.route_entry_key = KeyGenerator::generateRouteKey(route_entry.vrf_id, route_entry.route_prefix);
    EXPECT_EQ(StatusCode::SWSS_RC_NOT_FOUND, ValidateDelRouteEntry(route_entry));
}

TEST_F(RouteManagerTest, ValidateDelRouteEntryDoesNotExistInMapperShouldFail)
{
    auto swss_ipv4_route_prefix = swss::IpPrefix(kIpv4Prefix);
    SetupNexthopIdRouteEntry(gVrfName, swss_ipv4_route_prefix, kNexthopId1, kNexthopOid1);
    P4RouteEntry route_entry = {};
    route_entry.vrf_id = gVrfName;
    route_entry.route_prefix = swss_ipv4_route_prefix;
    route_entry.route_entry_key = KeyGenerator::generateRouteKey(route_entry.vrf_id, route_entry.route_prefix);
    p4_oid_mapper_.eraseOID(SAI_OBJECT_TYPE_ROUTE_ENTRY, route_entry.route_entry_key);
    // (TODO): Expect critical state.
    EXPECT_EQ(StatusCode::SWSS_RC_INTERNAL, ValidateDelRouteEntry(route_entry));
}

TEST_F(RouteManagerTest, ValidateDelRouteEntryHasActionShouldFail)
{
    auto swss_ipv4_route_prefix = swss::IpPrefix(kIpv4Prefix);
    SetupNexthopIdRouteEntry(gVrfName, swss_ipv4_route_prefix, kNexthopId1, kNexthopOid1);
    P4RouteEntry route_entry = {};
    route_entry.vrf_id = gVrfName;
    route_entry.route_prefix = swss_ipv4_route_prefix;
    route_entry.action = p4orch::kSetNexthopId;
    route_entry.route_entry_key = KeyGenerator::generateRouteKey(route_entry.vrf_id, route_entry.route_prefix);
    EXPECT_EQ(StatusCode::SWSS_RC_INVALID_PARAM, ValidateDelRouteEntry(route_entry));
}

TEST_F(RouteManagerTest, ValidateDelRouteEntryHasNexthopIdShouldFail)
{
    auto swss_ipv4_route_prefix = swss::IpPrefix(kIpv4Prefix);
    SetupNexthopIdRouteEntry(gVrfName, swss_ipv4_route_prefix, kNexthopId1, kNexthopOid1);
    P4RouteEntry route_entry = {};
    route_entry.vrf_id = gVrfName;
    route_entry.route_prefix = swss_ipv4_route_prefix;
    route_entry.nexthop_id = kNexthopId1;
    route_entry.route_entry_key = KeyGenerator::generateRouteKey(route_entry.vrf_id, route_entry.route_prefix);
    EXPECT_EQ(StatusCode::SWSS_RC_INVALID_PARAM, ValidateDelRouteEntry(route_entry));
}

TEST_F(RouteManagerTest, ValidateDelRouteEntryHasWcmpGroupShouldFail)
{
    auto swss_ipv4_route_prefix = swss::IpPrefix(kIpv4Prefix);
    SetupNexthopIdRouteEntry(gVrfName, swss_ipv4_route_prefix, kNexthopId1, kNexthopOid1);
    P4RouteEntry route_entry = {};
    route_entry.vrf_id = gVrfName;
    route_entry.route_prefix = swss_ipv4_route_prefix;
    route_entry.wcmp_group = kWcmpGroup1;
    route_entry.route_entry_key = KeyGenerator::generateRouteKey(route_entry.vrf_id, route_entry.route_prefix);
    EXPECT_EQ(StatusCode::SWSS_RC_INVALID_PARAM, ValidateDelRouteEntry(route_entry));
}

TEST_F(RouteManagerTest, ValidateDelRouteEntrySucceeds)
{
    auto swss_ipv4_route_prefix = swss::IpPrefix(kIpv4Prefix);
    SetupNexthopIdRouteEntry(gVrfName, swss_ipv4_route_prefix, kNexthopId1, kNexthopOid1);
    P4RouteEntry route_entry = {};
    route_entry.vrf_id = gVrfName;
    route_entry.route_prefix = swss_ipv4_route_prefix;
    route_entry.route_entry_key = KeyGenerator::generateRouteKey(route_entry.vrf_id, route_entry.route_prefix);
    EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS, ValidateDelRouteEntry(route_entry));
}

TEST_F(RouteManagerTest, CreateRouteEntryWithSaiErrorShouldFail)
{
    auto swss_ipv4_route_prefix = swss::IpPrefix(kIpv4Prefix);
    P4RouteEntry route_entry;
    route_entry.vrf_id = gVrfName;
    route_entry.route_prefix = swss_ipv4_route_prefix;
    route_entry.action = p4orch::kDrop;
    route_entry.route_entry_key = KeyGenerator::generateRouteKey(route_entry.vrf_id, route_entry.route_prefix);

    EXPECT_CALL(mock_sai_route_, create_route_entry(_, _, _)).Times(3).WillRepeatedly(Return(SAI_STATUS_FAILURE));
    EXPECT_EQ(StatusCode::SWSS_RC_UNKNOWN, CreateRouteEntry(route_entry));

    route_entry.action = p4orch::kSetNexthopId;
    route_entry.nexthop_id = kNexthopId1;
    EXPECT_EQ(StatusCode::SWSS_RC_UNKNOWN, CreateRouteEntry(route_entry));

    route_entry.action = p4orch::kSetWcmpGroupId;
    route_entry.wcmp_group = kWcmpGroup1;
    EXPECT_EQ(StatusCode::SWSS_RC_UNKNOWN, CreateRouteEntry(route_entry));
}

TEST_F(RouteManagerTest, CreateNexthopIdIpv4RouteSucceeds)
{
    auto swss_ipv4_route_prefix = swss::IpPrefix(kIpv4Prefix);
    sai_ip_prefix_t sai_ipv4_route_prefix;
    copy(sai_ipv4_route_prefix, swss_ipv4_route_prefix);
    P4RouteEntry route_entry;
    route_entry.vrf_id = gVrfName;
    route_entry.route_prefix = swss_ipv4_route_prefix;
    route_entry.action = p4orch::kSetNexthopId;
    route_entry.nexthop_id = kNexthopId1;
    route_entry.route_entry_key = KeyGenerator::generateRouteKey(route_entry.vrf_id, route_entry.route_prefix);
    p4_oid_mapper_.setOID(SAI_OBJECT_TYPE_NEXT_HOP, KeyGenerator::generateNextHopKey(route_entry.nexthop_id),
                          kNexthopOid1);

    EXPECT_CALL(
        mock_sai_route_,
        create_route_entry(Truly(std::bind(MatchSaiRouteEntry, sai_ipv4_route_prefix, std::placeholders::_1, gVrfOid)),
                           Eq(1), Truly(std::bind(MatchSaiAttributeNexthopId, kNexthopOid1, std::placeholders::_1))))
        .WillOnce(Return(SAI_STATUS_SUCCESS));
    EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS, CreateRouteEntry(route_entry));
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
    P4RouteEntry route_entry;
    route_entry.vrf_id = gVrfName;
    route_entry.route_prefix = swss_ipv6_route_prefix;
    route_entry.action = p4orch::kSetNexthopId;
    route_entry.nexthop_id = kNexthopId1;
    route_entry.route_entry_key = KeyGenerator::generateRouteKey(route_entry.vrf_id, route_entry.route_prefix);
    p4_oid_mapper_.setOID(SAI_OBJECT_TYPE_NEXT_HOP, KeyGenerator::generateNextHopKey(route_entry.nexthop_id),
                          kNexthopOid1);

    EXPECT_CALL(
        mock_sai_route_,
        create_route_entry(Truly(std::bind(MatchSaiRouteEntry, sai_ipv6_route_prefix, std::placeholders::_1, gVrfOid)),
                           Eq(1), Truly(std::bind(MatchSaiAttributeNexthopId, kNexthopOid1, std::placeholders::_1))))
        .WillOnce(Return(SAI_STATUS_SUCCESS));
    EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS, CreateRouteEntry(route_entry));
    VerifyRouteEntry(route_entry, sai_ipv6_route_prefix, gVrfOid);
    uint32_t ref_cnt;
    EXPECT_TRUE(
        p4_oid_mapper_.getRefCount(SAI_OBJECT_TYPE_NEXT_HOP, KeyGenerator::generateNextHopKey(kNexthopId1), &ref_cnt));
    EXPECT_EQ(1, ref_cnt);
}

TEST_F(RouteManagerTest, CreateDropIpv4RouteSucceeds)
{
    auto swss_ipv4_route_prefix = swss::IpPrefix(kIpv4Prefix);
    sai_ip_prefix_t sai_ipv4_route_prefix;
    copy(sai_ipv4_route_prefix, swss_ipv4_route_prefix);
    P4RouteEntry route_entry;
    route_entry.vrf_id = gVrfName;
    route_entry.route_prefix = swss_ipv4_route_prefix;
    route_entry.action = p4orch::kDrop;
    route_entry.route_entry_key = KeyGenerator::generateRouteKey(route_entry.vrf_id, route_entry.route_prefix);

    EXPECT_CALL(mock_sai_route_,
                create_route_entry(
                    Truly(std::bind(MatchSaiRouteEntry, sai_ipv4_route_prefix, std::placeholders::_1, gVrfOid)), Eq(1),
                    Truly(std::bind(MatchSaiAttributeAction, SAI_PACKET_ACTION_DROP, std::placeholders::_1))))
        .WillOnce(Return(SAI_STATUS_SUCCESS));
    EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS, CreateRouteEntry(route_entry));
    VerifyRouteEntry(route_entry, sai_ipv4_route_prefix, gVrfOid);
}

TEST_F(RouteManagerTest, CreateDropIpv6RouteSucceeds)
{
    auto swss_ipv6_route_prefix = swss::IpPrefix(kIpv6Prefix);
    sai_ip_prefix_t sai_ipv6_route_prefix;
    copy(sai_ipv6_route_prefix, swss_ipv6_route_prefix);
    P4RouteEntry route_entry;
    route_entry.vrf_id = gVrfName;
    route_entry.route_prefix = swss_ipv6_route_prefix;
    route_entry.action = p4orch::kDrop;
    route_entry.route_entry_key = KeyGenerator::generateRouteKey(route_entry.vrf_id, route_entry.route_prefix);

    EXPECT_CALL(mock_sai_route_,
                create_route_entry(
                    Truly(std::bind(MatchSaiRouteEntry, sai_ipv6_route_prefix, std::placeholders::_1, gVrfOid)), Eq(1),
                    Truly(std::bind(MatchSaiAttributeAction, SAI_PACKET_ACTION_DROP, std::placeholders::_1))))
        .WillOnce(Return(SAI_STATUS_SUCCESS));
    EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS, CreateRouteEntry(route_entry));
    VerifyRouteEntry(route_entry, sai_ipv6_route_prefix, gVrfOid);
}

TEST_F(RouteManagerTest, CreateWcmpIpv4RouteSucceeds)
{
    auto swss_ipv4_route_prefix = swss::IpPrefix(kIpv4Prefix);
    sai_ip_prefix_t sai_ipv4_route_prefix;
    copy(sai_ipv4_route_prefix, swss_ipv4_route_prefix);
    P4RouteEntry route_entry;
    route_entry.vrf_id = gVrfName;
    route_entry.route_prefix = swss_ipv4_route_prefix;
    route_entry.action = p4orch::kSetWcmpGroupId;
    route_entry.wcmp_group = kWcmpGroup1;
    route_entry.route_entry_key = KeyGenerator::generateRouteKey(route_entry.vrf_id, route_entry.route_prefix);
    p4_oid_mapper_.setOID(SAI_OBJECT_TYPE_NEXT_HOP_GROUP, KeyGenerator::generateWcmpGroupKey(route_entry.wcmp_group),
                          kWcmpGroupOid1);

    EXPECT_CALL(
        mock_sai_route_,
        create_route_entry(Truly(std::bind(MatchSaiRouteEntry, sai_ipv4_route_prefix, std::placeholders::_1, gVrfOid)),
                           Eq(1), Truly(std::bind(MatchSaiAttributeNexthopId, kWcmpGroupOid1, std::placeholders::_1))))
        .WillOnce(Return(SAI_STATUS_SUCCESS));
    EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS, CreateRouteEntry(route_entry));
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
    P4RouteEntry route_entry;
    route_entry.vrf_id = gVrfName;
    route_entry.route_prefix = swss_ipv6_route_prefix;
    route_entry.action = p4orch::kSetWcmpGroupId;
    route_entry.wcmp_group = kWcmpGroup1;
    route_entry.route_entry_key = KeyGenerator::generateRouteKey(route_entry.vrf_id, route_entry.route_prefix);
    p4_oid_mapper_.setOID(SAI_OBJECT_TYPE_NEXT_HOP_GROUP, KeyGenerator::generateWcmpGroupKey(route_entry.wcmp_group),
                          kWcmpGroupOid1);

    EXPECT_CALL(
        mock_sai_route_,
        create_route_entry(Truly(std::bind(MatchSaiRouteEntry, sai_ipv6_route_prefix, std::placeholders::_1, gVrfOid)),
                           Eq(1), Truly(std::bind(MatchSaiAttributeNexthopId, kWcmpGroupOid1, std::placeholders::_1))))
        .WillOnce(Return(SAI_STATUS_SUCCESS));
    EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS, CreateRouteEntry(route_entry));
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

    P4RouteEntry route_entry = {};
    route_entry.vrf_id = gVrfName;
    route_entry.route_prefix = swss_ipv4_route_prefix;
    route_entry.action = p4orch::kSetWcmpGroupId;
    route_entry.wcmp_group = kWcmpGroup2;
    route_entry.route_entry_key = KeyGenerator::generateRouteKey(route_entry.vrf_id, route_entry.route_prefix);
    p4_oid_mapper_.setOID(SAI_OBJECT_TYPE_NEXT_HOP_GROUP, KeyGenerator::generateWcmpGroupKey(route_entry.wcmp_group),
                          kWcmpGroupOid2);
    EXPECT_CALL(mock_sai_route_, set_route_entry_attribute(_, _)).WillOnce(Return(SAI_STATUS_FAILURE));
    EXPECT_EQ(StatusCode::SWSS_RC_UNKNOWN, UpdateRouteEntry(route_entry));
    EXPECT_CALL(mock_sai_route_, set_route_entry_attribute(_, _))
        .WillOnce(Return(SAI_STATUS_SUCCESS))
        .WillOnce(Return(SAI_STATUS_FAILURE))
        .WillOnce(Return(SAI_STATUS_SUCCESS));
    EXPECT_EQ(StatusCode::SWSS_RC_UNKNOWN, UpdateRouteEntry(route_entry));
}

TEST_F(RouteManagerTest, UpdateRouteEntryWcmpNotExistInMapperShouldFail)
{
    auto swss_ipv4_route_prefix = swss::IpPrefix(kIpv4Prefix);
    sai_ip_prefix_t sai_ipv4_route_prefix;
    copy(sai_ipv4_route_prefix, swss_ipv4_route_prefix);
    SetupWcmpGroupRouteEntry(gVrfName, swss_ipv4_route_prefix, kWcmpGroup1, kWcmpGroupOid1);

    P4RouteEntry route_entry = {};
    route_entry.vrf_id = gVrfName;
    route_entry.route_prefix = swss_ipv4_route_prefix;
    route_entry.action = p4orch::kSetWcmpGroupId;
    route_entry.wcmp_group = kWcmpGroup2;
    route_entry.route_entry_key = KeyGenerator::generateRouteKey(route_entry.vrf_id, route_entry.route_prefix);

    // (TODO): Expect critical state.
    EXPECT_EQ(StatusCode::SWSS_RC_INTERNAL, UpdateRouteEntry(route_entry));
}

TEST_F(RouteManagerTest, UpdateRouteFromSetWcmpToSetNextHopSucceeds)
{
    auto swss_ipv4_route_prefix = swss::IpPrefix(kIpv4Prefix);
    sai_ip_prefix_t sai_ipv4_route_prefix;
    copy(sai_ipv4_route_prefix, swss_ipv4_route_prefix);
    SetupWcmpGroupRouteEntry(gVrfName, swss_ipv4_route_prefix, kWcmpGroup1, kWcmpGroupOid1);

    P4RouteEntry route_entry = {};
    route_entry.vrf_id = gVrfName;
    route_entry.route_prefix = swss_ipv4_route_prefix;
    route_entry.action = p4orch::kSetNexthopId;
    route_entry.nexthop_id = kNexthopId2;
    route_entry.route_entry_key = KeyGenerator::generateRouteKey(route_entry.vrf_id, route_entry.route_prefix);
    p4_oid_mapper_.setOID(SAI_OBJECT_TYPE_NEXT_HOP, KeyGenerator::generateNextHopKey(route_entry.nexthop_id),
                          kNexthopOid2);
    EXPECT_CALL(mock_sai_route_,
                set_route_entry_attribute(
                    Truly(std::bind(MatchSaiRouteEntry, sai_ipv4_route_prefix, std::placeholders::_1, gVrfOid)),
                    Truly(std::bind(MatchSaiAttributeNexthopId, kNexthopOid2, std::placeholders::_1))))
        .WillOnce(Return(SAI_STATUS_SUCCESS));
    EXPECT_CALL(mock_sai_route_,
                set_route_entry_attribute(
                    Truly(std::bind(MatchSaiRouteEntry, sai_ipv4_route_prefix, std::placeholders::_1, gVrfOid)),
                    Truly(std::bind(MatchSaiAttributeAction, SAI_PACKET_ACTION_FORWARD, std::placeholders::_1))))
        .WillOnce(Return(SAI_STATUS_SUCCESS));
    EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS, UpdateRouteEntry(route_entry));
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

    P4RouteEntry route_entry = {};
    route_entry.vrf_id = gVrfName;
    route_entry.route_prefix = swss_ipv4_route_prefix;
    route_entry.action = p4orch::kSetWcmpGroupId;
    route_entry.wcmp_group = kWcmpGroup2;
    route_entry.route_entry_key = KeyGenerator::generateRouteKey(route_entry.vrf_id, route_entry.route_prefix);
    p4_oid_mapper_.setOID(SAI_OBJECT_TYPE_NEXT_HOP_GROUP, KeyGenerator::generateWcmpGroupKey(route_entry.wcmp_group),
                          kWcmpGroupOid2);
    EXPECT_CALL(mock_sai_route_,
                set_route_entry_attribute(
                    Truly(std::bind(MatchSaiRouteEntry, sai_ipv4_route_prefix, std::placeholders::_1, gVrfOid)),
                    Truly(std::bind(MatchSaiAttributeNexthopId, kWcmpGroupOid2, std::placeholders::_1))))
        .WillOnce(Return(SAI_STATUS_SUCCESS));
    EXPECT_CALL(mock_sai_route_,
                set_route_entry_attribute(
                    Truly(std::bind(MatchSaiRouteEntry, sai_ipv4_route_prefix, std::placeholders::_1, gVrfOid)),
                    Truly(std::bind(MatchSaiAttributeAction, SAI_PACKET_ACTION_FORWARD, std::placeholders::_1))))
        .WillOnce(Return(SAI_STATUS_SUCCESS));
    EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS, UpdateRouteEntry(route_entry));
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

TEST_F(RouteManagerTest, UpdateRouteEntryNexthopIdWithSaiErrorShouldFail)
{
    auto swss_ipv4_route_prefix = swss::IpPrefix(kIpv4Prefix);
    SetupNexthopIdRouteEntry(gVrfName, swss_ipv4_route_prefix, kNexthopId1, kNexthopOid1);

    P4RouteEntry route_entry = {};
    route_entry.vrf_id = gVrfName;
    route_entry.route_prefix = swss_ipv4_route_prefix;
    route_entry.nexthop_id = kNexthopId2;
    route_entry.route_entry_key = KeyGenerator::generateRouteKey(route_entry.vrf_id, route_entry.route_prefix);
    p4_oid_mapper_.setOID(SAI_OBJECT_TYPE_NEXT_HOP, KeyGenerator::generateNextHopKey(route_entry.nexthop_id),
                          kNexthopOid2);
    EXPECT_CALL(mock_sai_route_, set_route_entry_attribute(_, _)).WillOnce(Return(SAI_STATUS_FAILURE));
    EXPECT_EQ(StatusCode::SWSS_RC_UNKNOWN, UpdateRouteEntry(route_entry));
    EXPECT_CALL(mock_sai_route_, set_route_entry_attribute(_, _))
        .WillOnce(Return(SAI_STATUS_SUCCESS))
        .WillOnce(Return(SAI_STATUS_FAILURE))
        .WillOnce(Return(SAI_STATUS_SUCCESS));
    EXPECT_EQ(StatusCode::SWSS_RC_UNKNOWN, UpdateRouteEntry(route_entry));
}

TEST_F(RouteManagerTest, UpdateRouteEntryNexthopIdNotExistInMapperShouldFail)
{
    auto swss_ipv4_route_prefix = swss::IpPrefix(kIpv4Prefix);
    SetupNexthopIdRouteEntry(gVrfName, swss_ipv4_route_prefix, kNexthopId1, kNexthopOid1);

    P4RouteEntry route_entry = {};
    route_entry.vrf_id = gVrfName;
    route_entry.route_prefix = swss_ipv4_route_prefix;
    route_entry.nexthop_id = kNexthopId2;
    route_entry.route_entry_key = KeyGenerator::generateRouteKey(route_entry.vrf_id, route_entry.route_prefix);

    // (TODO): Expect critical state.
    EXPECT_EQ(StatusCode::SWSS_RC_INTERNAL, UpdateRouteEntry(route_entry));
}

TEST_F(RouteManagerTest, UpdateRouteEntryDropWithSaiErrorShouldFail)
{
    auto swss_ipv4_route_prefix = swss::IpPrefix(kIpv4Prefix);
    SetupNexthopIdRouteEntry(gVrfName, swss_ipv4_route_prefix, kNexthopId1, kNexthopOid1);

    P4RouteEntry route_entry = {};
    route_entry.vrf_id = gVrfName;
    route_entry.route_prefix = swss_ipv4_route_prefix;
    route_entry.action = p4orch::kDrop;
    route_entry.route_entry_key = KeyGenerator::generateRouteKey(route_entry.vrf_id, route_entry.route_prefix);
    EXPECT_CALL(mock_sai_route_, set_route_entry_attribute(_, _)).WillOnce(Return(SAI_STATUS_FAILURE));
    EXPECT_EQ(StatusCode::SWSS_RC_UNKNOWN, UpdateRouteEntry(route_entry));
    EXPECT_CALL(mock_sai_route_, set_route_entry_attribute(_, _))
        .WillOnce(Return(SAI_STATUS_SUCCESS))
        .WillOnce(Return(SAI_STATUS_FAILURE))
        .WillOnce(Return(SAI_STATUS_SUCCESS));
    EXPECT_EQ(StatusCode::SWSS_RC_UNKNOWN, UpdateRouteEntry(route_entry));
}

TEST_F(RouteManagerTest, UpdateRouteWithDifferentNexthopIdsSucceeds)
{
    auto swss_ipv4_route_prefix = swss::IpPrefix(kIpv4Prefix);
    sai_ip_prefix_t sai_ipv4_route_prefix;
    copy(sai_ipv4_route_prefix, swss_ipv4_route_prefix);
    SetupNexthopIdRouteEntry(gVrfName, swss_ipv4_route_prefix, kNexthopId1, kNexthopOid1);

    P4RouteEntry route_entry = {};
    route_entry.vrf_id = gVrfName;
    route_entry.route_prefix = swss_ipv4_route_prefix;
    route_entry.nexthop_id = kNexthopId2;
    route_entry.route_entry_key = KeyGenerator::generateRouteKey(route_entry.vrf_id, route_entry.route_prefix);
    p4_oid_mapper_.setOID(SAI_OBJECT_TYPE_NEXT_HOP, KeyGenerator::generateNextHopKey(route_entry.nexthop_id),
                          kNexthopOid2);
    EXPECT_CALL(mock_sai_route_,
                set_route_entry_attribute(
                    Truly(std::bind(MatchSaiRouteEntry, sai_ipv4_route_prefix, std::placeholders::_1, gVrfOid)),
                    Truly(std::bind(MatchSaiAttributeNexthopId, kNexthopOid2, std::placeholders::_1))))
        .WillOnce(Return(SAI_STATUS_SUCCESS));
    EXPECT_CALL(mock_sai_route_,
                set_route_entry_attribute(
                    Truly(std::bind(MatchSaiRouteEntry, sai_ipv4_route_prefix, std::placeholders::_1, gVrfOid)),
                    Truly(std::bind(MatchSaiAttributeAction, SAI_PACKET_ACTION_FORWARD, std::placeholders::_1))))
        .WillOnce(Return(SAI_STATUS_SUCCESS));
    EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS, UpdateRouteEntry(route_entry));
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

TEST_F(RouteManagerTest, UpdateRouteFromNexthopIdToDropSucceeds)
{
    auto swss_ipv4_route_prefix = swss::IpPrefix(kIpv4Prefix);
    sai_ip_prefix_t sai_ipv4_route_prefix;
    copy(sai_ipv4_route_prefix, swss_ipv4_route_prefix);
    SetupNexthopIdRouteEntry(gVrfName, swss_ipv4_route_prefix, kNexthopId1, kNexthopOid1);

    P4RouteEntry route_entry = {};
    route_entry.vrf_id = gVrfName;
    route_entry.route_prefix = swss_ipv4_route_prefix;
    route_entry.action = p4orch::kDrop;
    route_entry.route_entry_key = KeyGenerator::generateRouteKey(route_entry.vrf_id, route_entry.route_prefix);
    EXPECT_CALL(mock_sai_route_,
                set_route_entry_attribute(
                    Truly(std::bind(MatchSaiRouteEntry, sai_ipv4_route_prefix, std::placeholders::_1, gVrfOid)),
                    Truly(std::bind(MatchSaiAttributeAction, SAI_PACKET_ACTION_DROP, std::placeholders::_1))))
        .WillOnce(Return(SAI_STATUS_SUCCESS));
    EXPECT_CALL(mock_sai_route_,
                set_route_entry_attribute(
                    Truly(std::bind(MatchSaiRouteEntry, sai_ipv4_route_prefix, std::placeholders::_1, gVrfOid)),
                    Truly(std::bind(MatchSaiAttributeNexthopId, SAI_NULL_OBJECT_ID, std::placeholders::_1))))
        .WillOnce(Return(SAI_STATUS_SUCCESS));
    EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS, UpdateRouteEntry(route_entry));
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
    SetupNexthopIdRouteEntry(gVrfName, swss_ipv4_route_prefix, kNexthopId1, kNexthopOid1);

    P4RouteEntry route_entry = {};
    route_entry.vrf_id = gVrfName;
    route_entry.route_prefix = swss_ipv4_route_prefix;
    route_entry.action = p4orch::kSetNexthopId;
    route_entry.nexthop_id = kNexthopId2;
    route_entry.route_entry_key = KeyGenerator::generateRouteKey(route_entry.vrf_id, route_entry.route_prefix);
    p4_oid_mapper_.setOID(SAI_OBJECT_TYPE_NEXT_HOP, KeyGenerator::generateNextHopKey(route_entry.nexthop_id),
                          kNexthopOid2);
    EXPECT_CALL(mock_sai_route_,
                set_route_entry_attribute(
                    Truly(std::bind(MatchSaiRouteEntry, sai_ipv4_route_prefix, std::placeholders::_1, gVrfOid)),
                    Truly(std::bind(MatchSaiAttributeNexthopId, kNexthopOid2, std::placeholders::_1))))
        .WillOnce(Return(SAI_STATUS_SUCCESS));
    EXPECT_CALL(mock_sai_route_,
                set_route_entry_attribute(
                    Truly(std::bind(MatchSaiRouteEntry, sai_ipv4_route_prefix, std::placeholders::_1, gVrfOid)),
                    Truly(std::bind(MatchSaiAttributeAction, SAI_PACKET_ACTION_FORWARD, std::placeholders::_1))))
        .WillOnce(Return(SAI_STATUS_SUCCESS));
    EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS, UpdateRouteEntry(route_entry));
    VerifyRouteEntry(route_entry, sai_ipv4_route_prefix, gVrfOid);
    uint32_t ref_cnt;
    EXPECT_TRUE(
        p4_oid_mapper_.getRefCount(SAI_OBJECT_TYPE_NEXT_HOP, KeyGenerator::generateNextHopKey(kNexthopId2), &ref_cnt));
    EXPECT_EQ(1, ref_cnt);
}

TEST_F(RouteManagerTest, UpdateRouteWithDifferentWcmpGroupsSucceeds)
{
    auto swss_ipv4_route_prefix = swss::IpPrefix(kIpv4Prefix);
    sai_ip_prefix_t sai_ipv4_route_prefix;
    copy(sai_ipv4_route_prefix, swss_ipv4_route_prefix);
    SetupWcmpGroupRouteEntry(gVrfName, swss_ipv4_route_prefix, kWcmpGroup1, kWcmpGroupOid1);

    P4RouteEntry route_entry = {};
    route_entry.vrf_id = gVrfName;
    route_entry.route_prefix = swss_ipv4_route_prefix;
    route_entry.action = p4orch::kSetWcmpGroupId;
    route_entry.wcmp_group = kWcmpGroup2;
    route_entry.route_entry_key = KeyGenerator::generateRouteKey(route_entry.vrf_id, route_entry.route_prefix);
    p4_oid_mapper_.setOID(SAI_OBJECT_TYPE_NEXT_HOP_GROUP, KeyGenerator::generateWcmpGroupKey(route_entry.wcmp_group),
                          kWcmpGroupOid2);
    EXPECT_CALL(mock_sai_route_,
                set_route_entry_attribute(
                    Truly(std::bind(MatchSaiRouteEntry, sai_ipv4_route_prefix, std::placeholders::_1, gVrfOid)),
                    Truly(std::bind(MatchSaiAttributeNexthopId, kWcmpGroupOid2, std::placeholders::_1))))
        .WillOnce(Return(SAI_STATUS_SUCCESS));
    EXPECT_CALL(mock_sai_route_,
                set_route_entry_attribute(
                    Truly(std::bind(MatchSaiRouteEntry, sai_ipv4_route_prefix, std::placeholders::_1, gVrfOid)),
                    Truly(std::bind(MatchSaiAttributeAction, SAI_PACKET_ACTION_FORWARD, std::placeholders::_1))))
        .WillOnce(Return(SAI_STATUS_SUCCESS));
    EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS, UpdateRouteEntry(route_entry));
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

    P4RouteEntry route_entry = {};
    route_entry.vrf_id = gVrfName;
    route_entry.route_prefix = swss_ipv4_route_prefix;
    route_entry.action = p4orch::kSetNexthopId;
    route_entry.nexthop_id = kNexthopId1;
    route_entry.route_entry_key = KeyGenerator::generateRouteKey(route_entry.vrf_id, route_entry.route_prefix);
    EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS, UpdateRouteEntry(route_entry));
    VerifyRouteEntry(route_entry, sai_ipv4_route_prefix, gVrfOid);
    uint32_t ref_cnt;
    EXPECT_TRUE(
        p4_oid_mapper_.getRefCount(SAI_OBJECT_TYPE_NEXT_HOP, KeyGenerator::generateNextHopKey(kNexthopId1), &ref_cnt));
    EXPECT_EQ(1, ref_cnt);
}

TEST_F(RouteManagerTest, UpdateRouteEntryRecoverFailureShouldRaiseCriticalState)
{
    auto swss_ipv4_route_prefix = swss::IpPrefix(kIpv4Prefix);
    SetupNexthopIdRouteEntry(gVrfName, swss_ipv4_route_prefix, kNexthopId1, kNexthopOid1);

    P4RouteEntry route_entry = {};
    route_entry.vrf_id = gVrfName;
    route_entry.route_prefix = swss_ipv4_route_prefix;
    route_entry.action = p4orch::kDrop;
    route_entry.route_entry_key = KeyGenerator::generateRouteKey(route_entry.vrf_id, route_entry.route_prefix);
    EXPECT_CALL(mock_sai_route_, set_route_entry_attribute(_, _)).WillOnce(Return(SAI_STATUS_FAILURE));
    EXPECT_EQ(StatusCode::SWSS_RC_UNKNOWN, UpdateRouteEntry(route_entry));
    EXPECT_CALL(mock_sai_route_, set_route_entry_attribute(_, _))
        .WillOnce(Return(SAI_STATUS_SUCCESS))
        .WillOnce(Return(SAI_STATUS_FAILURE))
        .WillOnce(Return(SAI_STATUS_FAILURE));
    // (TODO): Expect critical state.
    EXPECT_EQ(StatusCode::SWSS_RC_UNKNOWN, UpdateRouteEntry(route_entry));
}

TEST_F(RouteManagerTest, DeleteRouteEntryWithSaiErrorShouldFail)
{
    auto swss_ipv4_route_prefix = swss::IpPrefix(kIpv4Prefix);
    SetupNexthopIdRouteEntry(gVrfName, swss_ipv4_route_prefix, kNexthopId1, kNexthopOid1);

    EXPECT_CALL(mock_sai_route_, remove_route_entry(_)).WillOnce(Return(SAI_STATUS_FAILURE));
    P4RouteEntry route_entry = {};
    route_entry.vrf_id = gVrfName;
    route_entry.route_prefix = swss_ipv4_route_prefix;
    route_entry.route_entry_key = KeyGenerator::generateRouteKey(route_entry.vrf_id, route_entry.route_prefix);
    EXPECT_EQ(StatusCode::SWSS_RC_UNKNOWN, DeleteRouteEntry(route_entry));
}

TEST_F(RouteManagerTest, DeleteIpv4RouteSucceeds)
{
    auto swss_ipv4_route_prefix = swss::IpPrefix(kIpv4Prefix);
    sai_ip_prefix_t sai_ipv4_route_prefix;
    copy(sai_ipv4_route_prefix, swss_ipv4_route_prefix);
    SetupNexthopIdRouteEntry(gVrfName, swss_ipv4_route_prefix, kNexthopId1, kNexthopOid1);

    EXPECT_CALL(mock_sai_route_, remove_route_entry(Truly(std::bind(MatchSaiRouteEntry, sai_ipv4_route_prefix,
                                                                    std::placeholders::_1, gVrfOid))))
        .WillOnce(Return(SAI_STATUS_SUCCESS));
    P4RouteEntry route_entry = {};
    route_entry.vrf_id = gVrfName;
    route_entry.route_prefix = swss_ipv4_route_prefix;
    route_entry.route_entry_key = KeyGenerator::generateRouteKey(route_entry.vrf_id, route_entry.route_prefix);
    EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS, DeleteRouteEntry(route_entry));
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

    EXPECT_CALL(mock_sai_route_, remove_route_entry(Truly(std::bind(MatchSaiRouteEntry, sai_ipv6_route_prefix,
                                                                    std::placeholders::_1, gVrfOid))))
        .WillOnce(Return(SAI_STATUS_SUCCESS));
    P4RouteEntry route_entry = {};
    route_entry.vrf_id = gVrfName;
    route_entry.route_prefix = swss_ipv6_route_prefix;
    route_entry.route_entry_key = KeyGenerator::generateRouteKey(route_entry.vrf_id, route_entry.route_prefix);
    EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS, DeleteRouteEntry(route_entry));
    auto *route_entry_ptr = GetRouteEntry(route_entry.route_entry_key);
    EXPECT_EQ(nullptr, route_entry_ptr);
    EXPECT_FALSE(p4_oid_mapper_.existsOID(SAI_OBJECT_TYPE_ROUTE_ENTRY, route_entry.route_entry_key));
    uint32_t ref_cnt;
    EXPECT_TRUE(p4_oid_mapper_.getRefCount(SAI_OBJECT_TYPE_NEXT_HOP_GROUP,
                                           KeyGenerator::generateWcmpGroupKey(kWcmpGroup1), &ref_cnt));
    EXPECT_EQ(0, ref_cnt);
}

TEST_F(RouteManagerTest, RouteCreateAndUpdateInDrainSucceeds)
{
    const std::string kKeyPrefix = std::string(APP_P4RT_IPV4_TABLE_NAME) + kTableKeyDelimiter;
    p4_oid_mapper_.setOID(SAI_OBJECT_TYPE_NEXT_HOP, KeyGenerator::generateNextHopKey(kNexthopId1), kNexthopOid1);
    p4_oid_mapper_.setOID(SAI_OBJECT_TYPE_NEXT_HOP, KeyGenerator::generateNextHopKey(kNexthopId2), kNexthopOid2);
    nlohmann::json j;
    j[prependMatchField(p4orch::kVrfId)] = gVrfName;
    j[prependMatchField(p4orch::kIpv4Dst)] = kIpv4Prefix;
    std::vector<swss::FieldValueTuple> attributes;
    attributes.push_back(swss::FieldValueTuple{p4orch::kAction, p4orch::kSetNexthopId});
    attributes.push_back(swss::FieldValueTuple{prependParamField(p4orch::kNexthopId), kNexthopId1});
    Enqueue(swss::KeyOpFieldsValuesTuple(kKeyPrefix + j.dump(), SET_COMMAND, attributes));
    attributes.clear();
    attributes.push_back(swss::FieldValueTuple{prependParamField(p4orch::kNexthopId), kNexthopId2});
    Enqueue(swss::KeyOpFieldsValuesTuple(kKeyPrefix + j.dump(), SET_COMMAND, attributes));

    EXPECT_CALL(mock_sai_route_, create_route_entry(_, _, _)).WillOnce(Return(SAI_STATUS_SUCCESS));
    EXPECT_CALL(mock_sai_route_, set_route_entry_attribute(_, _)).Times(2).WillRepeatedly(Return(SAI_STATUS_SUCCESS));

    Drain();
    auto swss_ipv4_route_prefix = swss::IpPrefix(kIpv4Prefix);
    sai_ip_prefix_t sai_ipv4_route_prefix;
    copy(sai_ipv4_route_prefix, swss_ipv4_route_prefix);
    P4RouteEntry route_entry;
    route_entry.vrf_id = gVrfName;
    route_entry.route_prefix = swss_ipv4_route_prefix;
    route_entry.action = p4orch::kSetNexthopId;
    route_entry.nexthop_id = kNexthopId2;
    route_entry.route_entry_key = KeyGenerator::generateRouteKey(route_entry.vrf_id, route_entry.route_prefix);
    VerifyRouteEntry(route_entry, sai_ipv4_route_prefix, gVrfOid);
    uint32_t ref_cnt;
    EXPECT_TRUE(
        p4_oid_mapper_.getRefCount(SAI_OBJECT_TYPE_NEXT_HOP, KeyGenerator::generateNextHopKey(kNexthopId1), &ref_cnt));
    EXPECT_EQ(0, ref_cnt);
    EXPECT_TRUE(
        p4_oid_mapper_.getRefCount(SAI_OBJECT_TYPE_NEXT_HOP, KeyGenerator::generateNextHopKey(kNexthopId2), &ref_cnt));
    EXPECT_EQ(1, ref_cnt);
}

TEST_F(RouteManagerTest, RouteCreateAndDeleteInDrainSucceeds)
{
    const std::string kKeyPrefix = std::string(APP_P4RT_IPV4_TABLE_NAME) + kTableKeyDelimiter;
    p4_oid_mapper_.setOID(SAI_OBJECT_TYPE_NEXT_HOP, KeyGenerator::generateNextHopKey(kNexthopId1), kNexthopOid1);
    nlohmann::json j;
    j[prependMatchField(p4orch::kVrfId)] = gVrfName;
    j[prependMatchField(p4orch::kIpv4Dst)] = kIpv4Prefix;
    std::vector<swss::FieldValueTuple> attributes;
    attributes.push_back(swss::FieldValueTuple{p4orch::kAction, p4orch::kSetNexthopId});
    attributes.push_back(swss::FieldValueTuple{prependParamField(p4orch::kNexthopId), kNexthopId1});
    Enqueue(swss::KeyOpFieldsValuesTuple(kKeyPrefix + j.dump(), SET_COMMAND, attributes));
    attributes.clear();
    Enqueue(swss::KeyOpFieldsValuesTuple(kKeyPrefix + j.dump(), DEL_COMMAND, attributes));

    EXPECT_CALL(mock_sai_route_, create_route_entry(_, _, _)).WillOnce(Return(SAI_STATUS_SUCCESS));
    EXPECT_CALL(mock_sai_route_, remove_route_entry(_)).WillOnce(Return(SAI_STATUS_SUCCESS));
    Drain();
    std::string key = KeyGenerator::generateRouteKey(gVrfName, swss::IpPrefix(kIpv4Prefix));
    auto *route_entry_ptr = GetRouteEntry(key);
    EXPECT_EQ(nullptr, route_entry_ptr);
    EXPECT_FALSE(p4_oid_mapper_.existsOID(SAI_OBJECT_TYPE_ROUTE_ENTRY, key));
    uint32_t ref_cnt;
    EXPECT_TRUE(
        p4_oid_mapper_.getRefCount(SAI_OBJECT_TYPE_NEXT_HOP, KeyGenerator::generateNextHopKey(kNexthopId1), &ref_cnt));
    EXPECT_EQ(0, ref_cnt);
}

TEST_F(RouteManagerTest, RouteCreateInDrainSucceedsWhenVrfIsEmpty)
{
    const std::string kKeyPrefix = std::string(APP_P4RT_IPV4_TABLE_NAME) + kTableKeyDelimiter;
    const std::string kDefaultVrfName = ""; // Default Vrf
    auto swss_ipv4_route_prefix = swss::IpPrefix(kIpv4Prefix);
    sai_ip_prefix_t sai_ipv4_route_prefix;
    copy(sai_ipv4_route_prefix, swss_ipv4_route_prefix);
    p4_oid_mapper_.setOID(SAI_OBJECT_TYPE_NEXT_HOP, KeyGenerator::generateNextHopKey(kNexthopId1), kNexthopOid1);
    nlohmann::json j;
    j[prependMatchField(p4orch::kVrfId)] = kDefaultVrfName;
    j[prependMatchField(p4orch::kIpv4Dst)] = kIpv4Prefix;
    std::vector<swss::FieldValueTuple> attributes;
    attributes.push_back(swss::FieldValueTuple{p4orch::kAction, p4orch::kSetNexthopId});
    attributes.push_back(swss::FieldValueTuple{prependParamField(p4orch::kNexthopId), kNexthopId1});
    Enqueue(swss::KeyOpFieldsValuesTuple(kKeyPrefix + j.dump(), SET_COMMAND, attributes));

    EXPECT_CALL(
        mock_sai_route_,
        create_route_entry(
            Truly(std::bind(MatchSaiRouteEntry, sai_ipv4_route_prefix, std::placeholders::_1, gVirtualRouterId)), Eq(1),
            Truly(std::bind(MatchSaiAttributeNexthopId, kNexthopOid1, std::placeholders::_1))))
        .WillOnce(Return(SAI_STATUS_SUCCESS));

    Drain();
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
    Enqueue(
        swss::KeyOpFieldsValuesTuple(kKeyPrefix + "{{{{{{{{{{{{", SET_COMMAND, std::vector<swss::FieldValueTuple>{}));
    Drain();
}

TEST_F(RouteManagerTest, ValidateRouteEntryInDrainFailsWhenVrfDoesNotExist)
{
    const std::string kKeyPrefix = std::string(APP_P4RT_IPV4_TABLE_NAME) + kTableKeyDelimiter;
    p4_oid_mapper_.setOID(SAI_OBJECT_TYPE_NEXT_HOP, KeyGenerator::generateNextHopKey(kNexthopId1), kNexthopOid1);
    nlohmann::json j;
    j[prependMatchField(p4orch::kVrfId)] = "Invalid-Vrf";
    j[prependMatchField(p4orch::kIpv4Dst)] = kIpv4Prefix;
    std::vector<swss::FieldValueTuple> attributes;
    attributes.push_back(swss::FieldValueTuple{p4orch::kAction, p4orch::kSetNexthopId});
    // Vrf does not exist.
    attributes.push_back(swss::FieldValueTuple{prependParamField(p4orch::kNexthopId), kNexthopId1});
    Enqueue(swss::KeyOpFieldsValuesTuple(kKeyPrefix + j.dump(), SET_COMMAND, attributes));
    Drain();
}

TEST_F(RouteManagerTest, ValidateRouteEntryInDrainFailsWhenNexthopDoesNotExist)
{
    const std::string kKeyPrefix = std::string(APP_P4RT_IPV4_TABLE_NAME) + kTableKeyDelimiter;
    nlohmann::json j;
    j[prependMatchField(p4orch::kVrfId)] = gVrfName;
    j[prependMatchField(p4orch::kIpv4Dst)] = kIpv4Prefix;
    std::vector<swss::FieldValueTuple> attributes;
    attributes.push_back(swss::FieldValueTuple{p4orch::kAction, p4orch::kSetNexthopId});
    // Nexthop ID does not exist.
    attributes.push_back(swss::FieldValueTuple{prependParamField(p4orch::kNexthopId), kNexthopId1});
    Enqueue(swss::KeyOpFieldsValuesTuple(kKeyPrefix + j.dump(), SET_COMMAND, attributes));
    Drain();
}

TEST_F(RouteManagerTest, ValidateSetRouteEntryInDrainFails)
{
    const std::string kKeyPrefix = std::string(APP_P4RT_IPV4_TABLE_NAME) + kTableKeyDelimiter;
    p4_oid_mapper_.setOID(SAI_OBJECT_TYPE_NEXT_HOP, KeyGenerator::generateNextHopKey(kNexthopId1), kNexthopOid1);
    nlohmann::json j;
    j[prependMatchField(p4orch::kVrfId)] = gVrfName;
    j[prependMatchField(p4orch::kIpv4Dst)] = kIpv4Prefix;
    std::vector<swss::FieldValueTuple> attributes;
    attributes.push_back(swss::FieldValueTuple{p4orch::kAction, p4orch::kSetNexthopId});
    // No nexthop ID with kSetNexthopId action.
    Enqueue(swss::KeyOpFieldsValuesTuple(kKeyPrefix + j.dump(), SET_COMMAND, attributes));
    Drain();
}

TEST_F(RouteManagerTest, ValidateDelRouteEntryInDrainFails)
{
    const std::string kKeyPrefix = std::string(APP_P4RT_IPV4_TABLE_NAME) + kTableKeyDelimiter;
    p4_oid_mapper_.setOID(SAI_OBJECT_TYPE_NEXT_HOP, KeyGenerator::generateNextHopKey(kNexthopId1), kNexthopOid1);
    nlohmann::json j;
    j[prependMatchField(p4orch::kVrfId)] = gVrfName;
    j[prependMatchField(p4orch::kIpv4Dst)] = kIpv4Prefix;
    std::vector<swss::FieldValueTuple> attributes;
    // Fields are non-empty for DEl.
    attributes.push_back(swss::FieldValueTuple{p4orch::kAction, p4orch::kSetNexthopId});
    attributes.push_back(swss::FieldValueTuple{prependParamField(p4orch::kNexthopId), kNexthopId1});
    Enqueue(swss::KeyOpFieldsValuesTuple(kKeyPrefix + j.dump(), DEL_COMMAND, attributes));
    Drain();
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
    Enqueue(swss::KeyOpFieldsValuesTuple(kKeyPrefix + j.dump(), "INVALID_COMMAND", attributes));
    Drain();
}
