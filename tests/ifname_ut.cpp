#include "gtest/gtest.h"

#include "ifname.h"

TEST(IfnameTest, AcceptsKernelInterfaceNames)
{
    EXPECT_TRUE(swss::isValidIfname("Ethernet0"));
    EXPECT_TRUE(swss::isValidIfname("PortChannel1"));
    EXPECT_TRUE(swss::isValidIfname("Eth0.100"));
    EXPECT_TRUE(swss::isValidIfname("Ethernet-BP0"));
    EXPECT_TRUE(swss::isValidIfname("1Ethernet"));
    EXPECT_TRUE(swss::isValidIfname("_eth0"));
    EXPECT_TRUE(swss::isValidIfname(".eth0"));
}

TEST(IfnameTest, RejectsInvalidKernelInterfaceNames)
{
    EXPECT_FALSE(swss::isValidIfname(""));
    EXPECT_FALSE(swss::isValidIfname("."));
    EXPECT_FALSE(swss::isValidIfname(".."));
    EXPECT_FALSE(swss::isValidIfname("-Ethernet0"));
    EXPECT_FALSE(swss::isValidIfname("Port Channel"));
    EXPECT_FALSE(swss::isValidIfname("Ethernet0/1"));
    EXPECT_FALSE(swss::isValidIfname("Ethernet0:1"));
    EXPECT_FALSE(swss::isValidIfname("Ethernet0@1"));
    EXPECT_FALSE(swss::isValidIfname("PortChannel1.100"));
}
