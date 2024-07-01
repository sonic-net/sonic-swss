#include "ut_helpers_fpmsyncd.h"
#include "gtest/gtest.h"
#include <gmock/gmock.h>
#include "mock_table.h"
#include <net/if.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include "ipaddress.h"
#include "ipprefix.h"

#define private public // Need to modify internal cache
#include "fpmlink.h"
#include "routesync.h"
#undef private

using namespace swss;
using namespace testing;

/*
Test Fixture
*/
namespace ut_fpmsyncd
{
    struct FpmSyncdSRv6RoutesTest : public ::testing::Test
    {
        std::shared_ptr<swss::DBConnector> m_app_db;
        std::shared_ptr<swss::RedisPipeline> pipeline;
        std::shared_ptr<RouteSync> m_routeSync;
        std::shared_ptr<FpmLink> m_fpmLink;
        std::shared_ptr<swss::Table> m_routeTable;
        std::shared_ptr<swss::Table> m_srv6SidListTable;

        virtual void SetUp() override
        {
            testing_db::reset();

            m_app_db = std::make_shared<swss::DBConnector>("APPL_DB", 0);

            /* Construct dependencies */

            /* 1) RouteSync */
            pipeline = std::make_shared<swss::RedisPipeline>(m_app_db.get());
            m_routeSync = std::make_shared<RouteSync>(pipeline.get());

            /* 2) FpmLink */
            m_fpmLink = std::make_shared<FpmLink>(m_routeSync.get());

            /* 3) ROUTE_TABLE in APP_DB */
            m_routeTable = std::make_shared<swss::Table>(m_app_db.get(), APP_ROUTE_TABLE_NAME);

            /* 4) SRV6_SID_LIST_TABLE in APP_DB */
            m_srv6SidListTable = std::make_shared<swss::Table>(m_app_db.get(), APP_SRV6_SID_LIST_TABLE_NAME);
        }

        virtual void TearDown() override
        {
        }
    };
}

namespace ut_fpmsyncd
{
    /* Test Receiving an SRv6 VPN Route (with an IPv4 prefix) */
    TEST_F(FpmSyncdSRv6RoutesTest, RecevingSRv6VpnRoutesWithIPv4Prefix)
    {
        ASSERT_NE(m_routeSync, nullptr);

        /* Create a Netlink object containing an SRv6 VPN Route */
        IpPrefix _dst = IpPrefix("192.168.6.0/24");
        IpAddress _vpn_sid = IpAddress("fc00:0:2:1::");
        IpAddress _encap_src_addr = IpAddress("fc00:0:1:1::1");

        struct nlmsg *nl_obj = create_srv6_vpn_route_nlmsg(RTM_NEWROUTE, &_dst, &_encap_src_addr, &_vpn_sid);
        if (!nl_obj)
            throw std::runtime_error("SRv6 VPN Route creation failed");

        /* Send the Netlink object to the FpmLink */
        m_fpmLink->processRawMsg(&nl_obj->n);

        /* Check that fpmsyncd created the correct entries in APP_DB */
        std::string path;
        ASSERT_EQ(m_srv6SidListTable->hget("Vrf10:192.168.6.0/24", "path", path), true);
        ASSERT_EQ(path, _vpn_sid.to_string());

        std::string segment;
        ASSERT_EQ(m_routeTable->hget("Vrf10:192.168.6.0/24", "segment", segment), true);
        ASSERT_EQ(segment, "Vrf10:192.168.6.0/24");

        std::string seg_src;
        ASSERT_EQ(m_routeTable->hget("Vrf10:192.168.6.0/24", "seg_src", seg_src), true);
        ASSERT_EQ(seg_src, _encap_src_addr.to_string());

        /* Destroy the Netlink object and free the memory */
        free_nlobj(nl_obj);
    }

    /* Test Receiving an SRv6 VPN Route (with an IPv6 prefix) */
    TEST_F(FpmSyncdSRv6RoutesTest, RecevingSRv6VpnRoutesWithIPv6Prefix)
    {
        ASSERT_NE(m_routeSync, nullptr);

        /* Create a Netlink object containing an SRv6 VPN Route */
        IpPrefix _dst = IpPrefix("fd00:0:21::/64");
        IpAddress _vpn_sid = IpAddress("fc00:0:2:1::");
        IpAddress _encap_src_addr = IpAddress("fc00:0:1:1::1");

        struct nlmsg *nl_obj = create_srv6_vpn_route_nlmsg(RTM_NEWROUTE, &_dst, &_encap_src_addr, &_vpn_sid);
        if (!nl_obj)
            throw std::runtime_error("SRv6 VPN Route creation failed");

        /* Send the Netlink object to the FpmLink */
        m_fpmLink->processRawMsg(&nl_obj->n);

        /* Check that fpmsyncd created the correct entries in APP_DB */
        std::string path;
        ASSERT_EQ(m_srv6SidListTable->hget("Vrf10:fd00:0:21::/64", "path", path), true);
        ASSERT_EQ(path, _vpn_sid.to_string());

        std::string segment;
        ASSERT_EQ(m_routeTable->hget("Vrf10:fd00:0:21::/64", "segment", segment), true);
        ASSERT_EQ(segment, "Vrf10:fd00:0:21::/64");

        std::string seg_src;
        ASSERT_EQ(m_routeTable->hget("Vrf10:fd00:0:21::/64", "seg_src", seg_src), true);
        ASSERT_EQ(seg_src, _encap_src_addr.to_string());

        /* Destroy the Netlink object and free the memory */
        free_nlobj(nl_obj);
    }
}