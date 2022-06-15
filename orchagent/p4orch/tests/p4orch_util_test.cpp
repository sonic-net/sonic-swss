#include "p4orch_util.h"

#include <gtest/gtest.h>

#include <string>

#include "ipprefix.h"
#include "swssnet.h"

namespace
{

TEST(P4OrchUtilTest, KeyGeneratorTest)
{
    std::string intf_key = KeyGenerator::generateRouterInterfaceKey("intf-qe-3/7");
    EXPECT_EQ("router_interface_id=intf-qe-3/7", intf_key);
    std::string neighbor_key = KeyGenerator::generateNeighborKey("intf-qe-3/7", swss::IpAddress("10.0.0.22"));
    EXPECT_EQ("neighbor_id=10.0.0.22:router_interface_id=intf-qe-3/7", neighbor_key);
    std::string nexthop_key = KeyGenerator::generateNextHopKey("ju1u32m1.atl11:qe-3/7");
    EXPECT_EQ("nexthop_id=ju1u32m1.atl11:qe-3/7", nexthop_key);
    std::string wcmp_group_key = KeyGenerator::generateWcmpGroupKey("group-1");
    EXPECT_EQ("wcmp_group_id=group-1", wcmp_group_key);
    std::string ipv4_route_key = KeyGenerator::generateRouteKey("b4-traffic", swss::IpPrefix("10.11.12.0/24"));
    EXPECT_EQ("ipv4_dst=10.11.12.0/24:vrf_id=b4-traffic", ipv4_route_key);
    ipv4_route_key = KeyGenerator::generateRouteKey("b4-traffic", swss::IpPrefix("0.0.0.0/0"));
    EXPECT_EQ("ipv4_dst=0.0.0.0/0:vrf_id=b4-traffic", ipv4_route_key);
    std::string ipv6_route_key = KeyGenerator::generateRouteKey("b4-traffic", swss::IpPrefix("2001:db8:1::/32"));
    EXPECT_EQ("ipv6_dst=2001:db8:1::/32:vrf_id=b4-traffic", ipv6_route_key);
    ipv6_route_key = KeyGenerator::generateRouteKey("b4-traffic", swss::IpPrefix("::/0"));
    EXPECT_EQ("ipv6_dst=::/0:vrf_id=b4-traffic", ipv6_route_key);

    // Test with special characters.
    neighbor_key = KeyGenerator::generateNeighborKey("::===::", swss::IpAddress("::1"));
    EXPECT_EQ("neighbor_id=::1:router_interface_id=::===::", neighbor_key);

    std::map<std::string, std::string> match_fvs;
    match_fvs["ether_type"] = "0x0800";
    match_fvs["ipv6_dst"] = "fdf8:f53b:82e4::53 & fdf8:f53b:82e4::53";
    auto acl_rule_key = KeyGenerator::generateAclRuleKey(match_fvs, "15");
    EXPECT_EQ("match/ether_type=0x0800:match/"
              "ipv6_dst=fdf8:f53b:82e4::53 & fdf8:f53b:82e4::53:priority=15",
              acl_rule_key);
}

TEST(P4OrchUtilTest, ParseP4RTKeyTest)
{
    std::string table;
    std::string key;
    parseP4RTKey("table:key", &table, &key);
    EXPECT_EQ("table", table);
    EXPECT_EQ("key", key);
    parseP4RTKey("|||::::", &table, &key);
    EXPECT_EQ("|||", table);
    EXPECT_EQ(":::", key);
    parseP4RTKey("invalid", &table, &key);
    EXPECT_TRUE(table.empty());
    EXPECT_TRUE(key.empty());
}

TEST(P4OrchUtilTest, PrependMatchFieldShouldSucceed)
{
    EXPECT_EQ(prependMatchField("str"), "match/str");
}

TEST(P4OrchUtilTest, PrependParamFieldShouldSucceed)
{
    EXPECT_EQ(prependParamField("str"), "param/str");
}

TEST(P4OrchUtilTest, QuotedVarTest)
{
    std::string foo("Hello World");
    std::string bar("a string has 'quote'");
    EXPECT_EQ(QuotedVar(foo), "'Hello World'");
    EXPECT_EQ(QuotedVar(foo.c_str()), "'Hello World'");
    EXPECT_EQ(QuotedVar(bar), "'a string has \\\'quote\\\''");
    EXPECT_EQ(QuotedVar(bar.c_str()), "'a string has \\\'quote\\\''");
}

} // namespace
