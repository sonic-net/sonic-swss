#include <arpa/inet.h>
#include <gtest/gtest.h>
#include <string>
#include <iostream>
#include "swssnet.h"

using namespace std;
using namespace swss;

TEST(swssnet, copy1)
{
    IpAddress ip("2001:4898:f0:f153:357c:77b2:49c9:627c");
    sai_ip_address_t dst;
    copy(dst, ip);
    EXPECT_EQ(dst.addr_family, SAI_IP_ADDR_FAMILY_IPV6);
    
    char buf[INET6_ADDRSTRLEN];
    inet_ntop(AF_INET6, dst.addr.ip6, buf, INET6_ADDRSTRLEN);
    EXPECT_STREQ(buf, "2001:4898:f0:f153:357c:77b2:49c9:627c");
}

TEST(swssnet, copy2)
{
    IpPrefix ip("2001:4898:f0:f153:357c:77b2:49c9:627c/27");
    sai_ip_prefix_t dst;
    copy(dst, ip);
    EXPECT_EQ(dst.addr_family, SAI_IP_ADDR_FAMILY_IPV6);
    
    char buf[INET6_ADDRSTRLEN];
    inet_ntop(AF_INET6, dst.addr.ip6, buf, INET6_ADDRSTRLEN);
    EXPECT_STREQ(buf, "2001:4898:f0:f153:357c:77b2:49c9:627c");
    inet_ntop(AF_INET6, dst.mask.ip6, buf, INET6_ADDRSTRLEN);
    EXPECT_STREQ(buf, "ffff:ffe0::");
}

TEST(swssnet, copy3)
{
    IpAddress ip("2001:4898:f0:f153:357c:77b2:49c9:627c");
    sai_ip_prefix_t dst;
    copy(dst, ip);
    EXPECT_EQ(dst.addr_family, SAI_IP_ADDR_FAMILY_IPV6);
    
    char buf[INET6_ADDRSTRLEN];
    inet_ntop(AF_INET6, dst.addr.ip6, buf, INET6_ADDRSTRLEN);
    EXPECT_STREQ(buf, "2001:4898:f0:f153:357c:77b2:49c9:627c");
    inet_ntop(AF_INET6, dst.mask.ip6, buf, INET6_ADDRSTRLEN);
    EXPECT_STREQ(buf, "ffff:ffff:ffff:ffff:ffff:ffff:ffff:ffff");
}

TEST(swssnet, subnet)
{
    sai_ip_prefix_t dst, src;
    src.addr_family = SAI_IP_ADDR_FAMILY_IPV6;
    inet_pton(AF_INET6, "2001:4898:f0:f153:357c:77b2:49c9:627c", src.addr.ip6);
    inet_pton(AF_INET6, "ffff:ffe0::", src.mask.ip6);
    
    subnet(dst, src);
    char buf[INET6_ADDRSTRLEN];
    inet_ntop(AF_INET6, dst.addr.ip6, buf, INET6_ADDRSTRLEN);
    EXPECT_STREQ(buf, "2001:4880::");
    inet_ntop(AF_INET6, dst.mask.ip6, buf, INET6_ADDRSTRLEN);
    EXPECT_STREQ(buf, "ffff:ffe0::");
}