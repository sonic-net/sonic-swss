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

    struct FpmSyncdSRv6RoutesTestZmqNb : public FpmSyncdSRv6RoutesTest {
        void SetUp() override
        {
            FpmSyncdSRv6RoutesTest::SetUp();
            // Simulate ZMQ being enabled by setting m_zmqClient to a non-null value
            // We use a dummy shared_ptr (pointing to address 1) since we won't actually use it
            // This makes isNbZmqEnabled() return true
            m_routeSync->m_zmqClient = shared_ptr<swss::ZmqClient>(reinterpret_cast<swss::ZmqClient*>(1), [](swss::ZmqClient*){});
        }
        void TearDown() override
        {
            // Reset m_zmqClient to nullptr
            m_routeSync->m_zmqClient = nullptr;
            FpmSyncdSRv6RoutesTest::TearDown();
        }
    };
}

namespace ut_fpmsyncd
{
    TEST_F(FpmSyncdSRv6RoutesTest, ReceivingSrv6BgpRouteWithIpv6Prefix)
    {
        IpAddress vpn_sid = IpAddress("fcbb:bbbb:2:e000::");
        IpAddress src_addr = IpAddress("fcbb:bbbb:1::1");
        IpAddress gateway = IpAddress("2001:db8:301::1");
        IpPrefix dst = IpPrefix("2001:db8:301::/64");
        std::string routeKey = "Vrf10:2001:db8:301::/64";
        std::string fieldValue;

        struct nlmsg *nl_obj = create_srv6_vpn_route_nlmsg(RTM_NEWROUTE, &dst, &src_addr, &vpn_sid,
                                                           &gateway);
        ASSERT_NE(nl_obj, nullptr);
        nl_obj->r.rtm_protocol = RTPROT_BGP;

        ASSERT_EQ(m_fpmLink->isRawProcessing(&nl_obj->n), true);
        m_fpmLink->processRawMsg(&nl_obj->n);

        ASSERT_EQ(m_routeTable->hget(routeKey, "protocol", fieldValue), true);
        ASSERT_EQ(fieldValue, "bgp");

        ASSERT_EQ(m_routeTable->hget(routeKey, "vpn_sid", fieldValue), true);
        ASSERT_EQ(fieldValue, "fcbb:bbbb:2:e000::");

        ASSERT_EQ(m_routeTable->hget(routeKey, "seg_src", fieldValue), true);
        ASSERT_EQ(fieldValue, "fcbb:bbbb:1::1");

        ASSERT_EQ(m_routeTable->hget(routeKey, "nexthop", fieldValue), true);
        ASSERT_EQ(fieldValue, "2001:db8:301::1");

        ASSERT_EQ(m_routeTable->hget(routeKey, "ifname", fieldValue), false);

        ASSERT_EQ(m_routeTable->hget(routeKey, "segment", fieldValue), false);
        ASSERT_EQ(m_srv6SidListTable->hget("fcbb:bbbb:2:e000::", "path", fieldValue), false);

        free_nlobj(nl_obj);
    }

    TEST_F(FpmSyncdSRv6RoutesTest, ReceivingSrv6BgpRouteWithIpv4Prefix)
    {
        IpAddress vpn_sid = IpAddress("fcbb:bbbb:2:e000::");
        IpAddress src_addr = IpAddress("fcbb:bbbb:1::1");
        IpAddress gateway = IpAddress("2001:db8:300::1");
        IpPrefix dst = IpPrefix("192.0.2.0/24");
        std::string routeKey = "Vrf10:192.0.2.0/24";
        std::string fieldValue;

        struct nlmsg *nl_obj = create_srv6_vpn_route_nlmsg(RTM_NEWROUTE, &dst, &src_addr, &vpn_sid,
                                                           &gateway);
        ASSERT_NE(nl_obj, nullptr);
        nl_obj->r.rtm_protocol = RTPROT_BGP;

        ASSERT_EQ(m_fpmLink->isRawProcessing(&nl_obj->n), true);
        m_fpmLink->processRawMsg(&nl_obj->n);

        ASSERT_EQ(m_routeTable->hget(routeKey, "protocol", fieldValue), true);
        ASSERT_EQ(fieldValue, "bgp");

        ASSERT_EQ(m_routeTable->hget(routeKey, "vpn_sid", fieldValue), true);
        ASSERT_EQ(fieldValue, "fcbb:bbbb:2:e000::");

        ASSERT_EQ(m_routeTable->hget(routeKey, "seg_src", fieldValue), true);
        ASSERT_EQ(fieldValue, "fcbb:bbbb:1::1");

        ASSERT_EQ(m_routeTable->hget(routeKey, "nexthop", fieldValue), true);
        ASSERT_EQ(fieldValue, "2001:db8:300::1");

        ASSERT_EQ(m_routeTable->hget(routeKey, "ifname", fieldValue), false);
        ASSERT_EQ(m_routeTable->hget(routeKey, "segment", fieldValue), false);
        ASSERT_EQ(m_srv6SidListTable->hget("fcbb:bbbb:2:e000::", "path", fieldValue), false);

        free_nlobj(nl_obj);
    }

    TEST_F(FpmSyncdSRv6RoutesTest, ReceivingSrv6BgpRouteIgnoresInvalidNexthopAttrs)
    {
        IpAddress vpn_sid = IpAddress("fcbb:bbbb:2:e000::");
        IpAddress src_addr = IpAddress("fcbb:bbbb:1::1");
        std::string fieldValue;

        {
            IpPrefix dst = IpPrefix("2001:db8:302::/64");
            std::string routeKey = "Vrf10:2001:db8:302::/64";
            struct nlmsg *nl_obj = create_srv6_vpn_route_nlmsg(RTM_NEWROUTE, &dst, &src_addr, &vpn_sid);
            ASSERT_NE(nl_obj, nullptr);
            nl_obj->r.rtm_protocol = RTPROT_BGP;

            uint8_t unsupportedGateway[5] = {};
            ASSERT_TRUE(nl_attr_put(&nl_obj->n, sizeof(*nl_obj), RTA_GATEWAY, unsupportedGateway,
                                    static_cast<unsigned int>(sizeof(unsupportedGateway))));

            ASSERT_EQ(m_fpmLink->isRawProcessing(&nl_obj->n), true);
            m_fpmLink->processRawMsg(&nl_obj->n);

            ASSERT_EQ(m_routeTable->hget(routeKey, "vpn_sid", fieldValue), true);
            ASSERT_EQ(fieldValue, "fcbb:bbbb:2:e000::");

            ASSERT_EQ(m_routeTable->hget(routeKey, "seg_src", fieldValue), true);
            ASSERT_EQ(fieldValue, "fcbb:bbbb:1::1");

            ASSERT_EQ(m_routeTable->hget(routeKey, "nexthop", fieldValue), false);
            ASSERT_EQ(m_routeTable->hget(routeKey, "ifname", fieldValue), false);

            free_nlobj(nl_obj);
        }

        {
            IpPrefix dst = IpPrefix("2001:db8:303::/64");
            std::string routeKey = "Vrf10:2001:db8:303::/64";
            struct nlmsg *nl_obj = create_srv6_vpn_route_nlmsg(RTM_NEWROUTE, &dst, &src_addr, &vpn_sid);
            ASSERT_NE(nl_obj, nullptr);
            nl_obj->r.rtm_protocol = RTPROT_BGP;

            uint16_t shortVia = AF_INET6;
            ASSERT_TRUE(nl_attr_put(&nl_obj->n, sizeof(*nl_obj), RTA_VIA, &shortVia,
                                    static_cast<unsigned int>(sizeof(shortVia))));

            ASSERT_EQ(m_fpmLink->isRawProcessing(&nl_obj->n), true);
            m_fpmLink->processRawMsg(&nl_obj->n);

            ASSERT_EQ(m_routeTable->hget(routeKey, "vpn_sid", fieldValue), true);
            ASSERT_EQ(fieldValue, "fcbb:bbbb:2:e000::");

            ASSERT_EQ(m_routeTable->hget(routeKey, "seg_src", fieldValue), true);
            ASSERT_EQ(fieldValue, "fcbb:bbbb:1::1");

            ASSERT_EQ(m_routeTable->hget(routeKey, "nexthop", fieldValue), false);
            ASSERT_EQ(m_routeTable->hget(routeKey, "ifname", fieldValue), false);

            free_nlobj(nl_obj);
        }

        {
            IpPrefix dst = IpPrefix("2001:db8:304::/64");
            std::string routeKey = "Vrf10:2001:db8:304::/64";
            struct nlmsg *nl_obj = create_srv6_vpn_route_nlmsg(RTM_NEWROUTE, &dst, &src_addr, &vpn_sid);
            ASSERT_NE(nl_obj, nullptr);
            nl_obj->r.rtm_protocol = RTPROT_BGP;

            struct
            {
                uint16_t family;
                uint8_t addr[16];
            } via = {};
            via.family = AF_UNSPEC;
            ASSERT_TRUE(nl_attr_put(&nl_obj->n, sizeof(*nl_obj), RTA_VIA, &via,
                                    static_cast<unsigned int>(sizeof(via))));

            ASSERT_EQ(m_fpmLink->isRawProcessing(&nl_obj->n), true);
            m_fpmLink->processRawMsg(&nl_obj->n);

            ASSERT_EQ(m_routeTable->hget(routeKey, "vpn_sid", fieldValue), true);
            ASSERT_EQ(fieldValue, "fcbb:bbbb:2:e000::");

            ASSERT_EQ(m_routeTable->hget(routeKey, "seg_src", fieldValue), true);
            ASSERT_EQ(fieldValue, "fcbb:bbbb:1::1");

            ASSERT_EQ(m_routeTable->hget(routeKey, "nexthop", fieldValue), false);
            ASSERT_EQ(m_routeTable->hget(routeKey, "ifname", fieldValue), false);

            free_nlobj(nl_obj);
        }
    }

    TEST_F(FpmSyncdSRv6RoutesTest, ReceivingSrv6BgpRouteMultipathWithIpv4Prefix)
    {
        IpAddress sid1 = IpAddress("fcbb:bbbb:2:e000::");
        IpAddress sid2 = IpAddress("fcbb:bbbb:3:e000::");
        IpAddress src1 = IpAddress("fcbb:bbbb:1::1");
        IpAddress src2 = IpAddress("fcbb:bbbb:1::1");
        IpAddress via_gateway = IpAddress("2001:db8:401::1");
        IpAddress gateway = IpAddress("2001:db8:400::1");
        IpPrefix dst = IpPrefix("192.0.3.0/24");
        std::string routeKey = "Vrf10:192.0.3.0/24";
        std::string fieldValue;

        std::vector<Srv6RouteNextHop> srv6Nexthops = {
            {&via_gateway, &src1, &sid1, 0},
            {&gateway, &src2, &sid2, 0},
        };

        struct nlmsg *nl_obj = create_srv6_vpn_route_nlmsg(RTM_NEWROUTE, &dst, srv6Nexthops);
        ASSERT_NE(nl_obj, nullptr);
        nl_obj->r.rtm_protocol = RTPROT_BGP;

        ASSERT_EQ(m_fpmLink->isRawProcessing(&nl_obj->n), true);
        m_fpmLink->processRawMsg(&nl_obj->n);

        ASSERT_EQ(m_routeTable->hget(routeKey, "protocol", fieldValue), true);
        ASSERT_EQ(fieldValue, "bgp");

        ASSERT_EQ(m_routeTable->hget(routeKey, "vpn_sid", fieldValue), true);
        ASSERT_EQ(fieldValue, "fcbb:bbbb:2:e000::,fcbb:bbbb:3:e000::");

        ASSERT_EQ(m_routeTable->hget(routeKey, "seg_src", fieldValue), true);
        ASSERT_EQ(fieldValue, "fcbb:bbbb:1::1,fcbb:bbbb:1::1");

        ASSERT_EQ(m_routeTable->hget(routeKey, "nexthop", fieldValue), true);
        ASSERT_EQ(fieldValue, "2001:db8:401::1,2001:db8:400::1");

        ASSERT_EQ(m_routeTable->hget(routeKey, "ifname", fieldValue), false);

        ASSERT_EQ(m_routeTable->hget(routeKey, "segment", fieldValue), false);
        ASSERT_EQ(m_srv6SidListTable->hget("fcbb:bbbb:2:e000::", "path", fieldValue), false);
        ASSERT_EQ(m_srv6SidListTable->hget("fcbb:bbbb:3:e000::", "path", fieldValue), false);

        free_nlobj(nl_obj);
    }

    /* Test Receiving an SRv6 VPN Route (with an IPv4 prefix) */
    TEST_F(FpmSyncdSRv6RoutesTest, RecevingSRv6VpnRoutesWithIPv4Prefix)
    {
        ASSERT_NE(m_routeSync, nullptr);

        struct nlmsg *nl_obj;
        std::string path;
        std::string segment;
        std::string seg_src;

        /* Create a Netlink object to install the SRv6 VPN Route */
        IpPrefix _dst = IpPrefix("192.168.6.0/24");
        IpAddress _vpn_sid = IpAddress("fc00:0:2:1::");
        IpAddress _encap_src_addr = IpAddress("fc00:0:1:1::1");

        nl_obj = create_srv6_vpn_route_nlmsg(RTM_NEWROUTE, &_dst, &_encap_src_addr, &_vpn_sid);
        if (!nl_obj)
            throw std::runtime_error("SRv6 VPN Route creation failed");

        /* Send the Netlink object to the FpmLink */
        ASSERT_EQ(m_fpmLink->isRawProcessing(&nl_obj->n), true);
        m_fpmLink->processRawMsg(&nl_obj->n);

        /* Check that fpmsyncd created the correct entries in APP_DB */
        ASSERT_EQ(m_srv6SidListTable->hget("fc00:0:2:1::", "path", path), true);
        ASSERT_EQ(path, _vpn_sid.to_string());

        ASSERT_EQ(m_routeTable->hget("Vrf10:192.168.6.0/24", "segment", segment), true);
        ASSERT_EQ(segment, "fc00:0:2:1::");

        ASSERT_EQ(m_routeTable->hget("Vrf10:192.168.6.0/24", "seg_src", seg_src), true);
        ASSERT_EQ(seg_src, _encap_src_addr.to_string());

        /* Destroy the Netlink object and free the memory */
        free_nlobj(nl_obj);


        /* Create a Netlink object to uninstall the SRv6 VPN Route */
        nl_obj = create_srv6_vpn_route_nlmsg(RTM_DELROUTE, &_dst, &_encap_src_addr, &_vpn_sid);
        if (!nl_obj)
            throw std::runtime_error("SRv6 VPN Route creation failed");

        /* Send the Netlink object to the FpmLink */
        ASSERT_EQ(m_fpmLink->isRawProcessing(&nl_obj->n), true);
        m_fpmLink->processRawMsg(&nl_obj->n);

        /* Check that fpmsyncd removed the entry from APP_DB */
        ASSERT_EQ(m_srv6SidListTable->hget("fc00:0:2:1::", "path", path), false);
        ASSERT_EQ(m_routeTable->hget("Vrf10:192.168.6.0/24", "segment", segment), false);
        ASSERT_EQ(m_routeTable->hget("Vrf10:192.168.6.0/24", "seg_src", seg_src), false);

        /* Destroy the Netlink object and free the memory */
        free_nlobj(nl_obj);
    }

    TEST_F(FpmSyncdSRv6RoutesTestZmqNb, RecevingSRv6VpnRoutesWithIPv4PrefixZmq)
    {
        ASSERT_NE(m_routeSync, nullptr);

        struct nlmsg *nl_obj;
        std::string path;
        std::string segment;
        std::string seg_src;

        /* Create a Netlink object to install the SRv6 VPN Route */
        IpPrefix _dst = IpPrefix("192.168.6.0/24");
        IpAddress _vpn_sid = IpAddress("fc00:0:2:1::");
        IpAddress _encap_src_addr = IpAddress("fc00:0:1:1::1");

        nl_obj = create_srv6_vpn_route_nlmsg(RTM_NEWROUTE, &_dst, &_encap_src_addr, &_vpn_sid);
        if (!nl_obj)
            throw std::runtime_error("SRv6 VPN Route creation failed");

        /* Send the Netlink object to the FpmLink */
        ASSERT_EQ(m_fpmLink->isRawProcessing(&nl_obj->n), true);
        m_fpmLink->processRawMsg(&nl_obj->n);

        /* Check that fpmsyncd created the correct entries in APP_DB */
        ASSERT_EQ(m_srv6SidListTable->hget("fc00:0:2:1::", "path", path), true);
        ASSERT_EQ(path, _vpn_sid.to_string());

        ASSERT_EQ(m_routeTable->hget("Vrf10:192.168.6.0/24", "segment", segment), true);
        ASSERT_EQ(segment, "fc00:0:2:1::");

        ASSERT_EQ(m_routeTable->hget("Vrf10:192.168.6.0/24", "seg_src", seg_src), true);
        ASSERT_EQ(seg_src, _encap_src_addr.to_string());

        /* Destroy the Netlink object and free the memory */
        free_nlobj(nl_obj);


        /* Create a Netlink object to uninstall the SRv6 VPN Route */
        nl_obj = create_srv6_vpn_route_nlmsg(RTM_DELROUTE, &_dst, &_encap_src_addr, &_vpn_sid);
        if (!nl_obj)
            throw std::runtime_error("SRv6 VPN Route creation failed");

        /* Send the Netlink object to the FpmLink */
        ASSERT_EQ(m_fpmLink->isRawProcessing(&nl_obj->n), true);
        m_fpmLink->processRawMsg(&nl_obj->n);

        /* Check that fpmsyncd removed the entry from APP_DB */
        ASSERT_EQ(m_srv6SidListTable->hget("fc00:0:2:1::", "path", path), false);
        ASSERT_EQ(m_routeTable->hget("Vrf10:192.168.6.0/24", "segment", segment), false);
        ASSERT_EQ(m_routeTable->hget("Vrf10:192.168.6.0/24", "seg_src", seg_src), false);

        /* Destroy the Netlink object and free the memory */
        free_nlobj(nl_obj);
    }

    /* Test Receiving an SRv6 VPN Route (with an IPv4 prefix with /32 prefix length) */
    TEST_F(FpmSyncdSRv6RoutesTest, RecevingSRv6VpnRoutesWithIPv4PrefixMaxPrefixLength)
    {
        ASSERT_NE(m_routeSync, nullptr);

        struct nlmsg *nl_obj;
        std::string path;
        std::string segment;
        std::string seg_src;


        /* Create a Netlink object containing an SRv6 VPN Route */
        IpPrefix _dst = IpPrefix("192.168.6.1/32");
        IpAddress _vpn_sid = IpAddress("fc00:0:2:1::");
        IpAddress _encap_src_addr = IpAddress("fc00:0:1:1::1");

        nl_obj = create_srv6_vpn_route_nlmsg(RTM_NEWROUTE, &_dst, &_encap_src_addr, &_vpn_sid);
        if (!nl_obj)
            throw std::runtime_error("SRv6 VPN Route creation failed");

        /* Send the Netlink object to the FpmLink */
        m_fpmLink->processRawMsg(&nl_obj->n);

        /* Check that fpmsyncd created the correct entries in APP_DB */
        ASSERT_EQ(m_srv6SidListTable->hget("fc00:0:2:1::", "path", path), true);
        ASSERT_EQ(path, _vpn_sid.to_string());

        ASSERT_EQ(m_routeTable->hget("Vrf10:192.168.6.1", "segment", segment), true);
        ASSERT_EQ(segment, "fc00:0:2:1::");

        ASSERT_EQ(m_routeTable->hget("Vrf10:192.168.6.1", "seg_src", seg_src), true);
        ASSERT_EQ(seg_src, _encap_src_addr.to_string());

        /* Destroy the Netlink object and free the memory */
        free_nlobj(nl_obj);


        /* Create a Netlink object to uninstall the SRv6 VPN Route */
        nl_obj = create_srv6_vpn_route_nlmsg(RTM_DELROUTE, &_dst, &_encap_src_addr, &_vpn_sid);
        if (!nl_obj)
            throw std::runtime_error("SRv6 VPN Route creation failed");

        /* Send the Netlink object to the FpmLink */
        ASSERT_EQ(m_fpmLink->isRawProcessing(&nl_obj->n), true);
        m_fpmLink->processRawMsg(&nl_obj->n);

        /* Check that fpmsyncd removed the entry from APP_DB */
        ASSERT_EQ(m_srv6SidListTable->hget("fc00:0:2:1::", "path", path), false);
        ASSERT_EQ(m_routeTable->hget("Vrf10:192.168.6.1/32", "segment", segment), false);
        ASSERT_EQ(m_routeTable->hget("Vrf10:192.168.6.1/32", "seg_src", seg_src), false);

        /* Destroy the Netlink object and free the memory */
        free_nlobj(nl_obj);
    }

    /* Test Receiving an SRv6 VPN Route (with an IPv6 prefix) */
    TEST_F(FpmSyncdSRv6RoutesTest, RecevingSRv6VpnRoutesWithIPv6Prefix)
    {
        ASSERT_NE(m_routeSync, nullptr);

        struct nlmsg *nl_obj;
        std::string path;
        std::string segment;
        std::string seg_src;

        /* Create a Netlink object containing an SRv6 VPN Route */
        IpPrefix _dst = IpPrefix("fd00:0:21::/64");
        IpAddress _vpn_sid = IpAddress("fc00:0:2:1::");
        IpAddress _encap_src_addr = IpAddress("fc00:0:1:1::1");

        nl_obj = create_srv6_vpn_route_nlmsg(RTM_NEWROUTE, &_dst, &_encap_src_addr, &_vpn_sid);
        if (!nl_obj)
            throw std::runtime_error("SRv6 VPN Route creation failed");

        /* Send the Netlink object to the FpmLink */
        m_fpmLink->processRawMsg(&nl_obj->n);

        /* Check that fpmsyncd created the correct entries in APP_DB */
        ASSERT_EQ(m_srv6SidListTable->hget("fc00:0:2:1::", "path", path), true);
        ASSERT_EQ(path, _vpn_sid.to_string());

        ASSERT_EQ(m_routeTable->hget("Vrf10:fd00:0:21::/64", "segment", segment), true);
        ASSERT_EQ(segment, "fc00:0:2:1::");

        ASSERT_EQ(m_routeTable->hget("Vrf10:fd00:0:21::/64", "seg_src", seg_src), true);
        ASSERT_EQ(seg_src, _encap_src_addr.to_string());

        /* Destroy the Netlink object and free the memory */
        free_nlobj(nl_obj);


        /* Create a Netlink object to uninstall the SRv6 VPN Route */
        nl_obj = create_srv6_vpn_route_nlmsg(RTM_DELROUTE, &_dst, &_encap_src_addr, &_vpn_sid);
        if (!nl_obj)
            throw std::runtime_error("SRv6 VPN Route creation failed");

        /* Send the Netlink object to the FpmLink */
        ASSERT_EQ(m_fpmLink->isRawProcessing(&nl_obj->n), true);
        m_fpmLink->processRawMsg(&nl_obj->n);

        /* Check that fpmsyncd removed the entry from APP_DB */
        ASSERT_EQ(m_srv6SidListTable->hget("fc00:0:2:1::", "path", path), false);
        ASSERT_EQ(m_routeTable->hget("Vrf10:fd00:0:21::/64", "segment", segment), false);
        ASSERT_EQ(m_routeTable->hget("Vrf10:fd00:0:21::/64", "seg_src", seg_src), false);

        /* Destroy the Netlink object and free the memory */
        free_nlobj(nl_obj);
    }

    /* Test Receiving an SRv6 VPN Route with missing destination prefix */
    TEST_F(FpmSyncdSRv6RoutesTest, SRv6VpnRoutesInvalidMissingDst)
    {
        ASSERT_NE(m_routeSync, nullptr);

        struct nlmsg *nl_obj;
        std::string path;
        std::string segment;
        std::string seg_src;

        /*
        * Scenario 1: SRv6 VPN Route with missing destination
        */

        /* Create a Netlink object containing an SRv6 VPN Route with missing destination */
        IpAddress _vpn_sid = IpAddress("fc00:0:2:1::");
        IpAddress _encap_src_addr = IpAddress("fc00:0:1:1::1");

        nl_obj = create_srv6_vpn_route_nlmsg(RTM_NEWROUTE, NULL, &_encap_src_addr, &_vpn_sid);
        if (!nl_obj)
            throw std::runtime_error("SRv6 VPN Route creation failed");

        /* Send the Netlink object to the FpmLink */
        ASSERT_EQ(m_fpmLink->isRawProcessing(&nl_obj->n), true);
        m_fpmLink->processRawMsg(&nl_obj->n);

        /* Check that fpmsyncd created the correct entries in APP_DB */
        ASSERT_EQ(m_srv6SidListTable->hget("fc00:0:2:1::", "path", path), false);

        ASSERT_EQ(m_routeTable->hget("Vrf10:192.168.6.0/24", "segment", segment), false);

        ASSERT_EQ(m_routeTable->hget("Vrf10:192.168.6.0/24", "seg_src", seg_src), false);

        /* Destroy the Netlink object and free the memory */
        free_nlobj(nl_obj);
    }


    /* Test Receiving an invalid SRv6 VPN Route IPv4 with invalid prefix length */
    TEST_F(FpmSyncdSRv6RoutesTest, SRv6VpnRoutesInvalidPrefixlenIpv4)
    {
        ASSERT_NE(m_routeSync, nullptr);

        struct nlmsg *nl_obj;
        IpPrefix _dst;
        IpAddress _vpn_sid;
        IpAddress _encap_src_addr;
        std::string path;
        std::string segment;
        std::string seg_src;

        /* Create a Netlink object containing an SRv6 VPN Route IPv4 with invalid prefix length */
        _dst = IpPrefix("192.168.6.0");
        _vpn_sid = IpAddress("fc00:0:2:1::");
        _encap_src_addr = IpAddress("fc00:0:1:1::1");

        nl_obj = create_srv6_vpn_route_nlmsg(RTM_NEWROUTE, &_dst, &_encap_src_addr, &_vpn_sid, 10, 100);
        if (!nl_obj)
            throw std::runtime_error("SRv6 VPN Route creation failed");

        /* Send the Netlink object to the FpmLink */
        ASSERT_EQ(m_fpmLink->isRawProcessing(&nl_obj->n), true);
        m_fpmLink->processRawMsg(&nl_obj->n);

        /* Check that fpmsyncd created the correct entries in APP_DB */
        ASSERT_EQ(m_srv6SidListTable->hget("fc00:0:2:1::", "path", path), false);

        ASSERT_EQ(m_routeTable->hget("Vrf10:192.168.6.0/100", "segment", segment), false);

        ASSERT_EQ(m_routeTable->hget("Vrf10:192.168.6.0/100", "seg_src", seg_src), false);

        /* Destroy the Netlink object and free the memory */
        free_nlobj(nl_obj);
    }

    /* Test Receiving an invalid SRv6 VPN Route IPv6 with invalid prefix length */
    TEST_F(FpmSyncdSRv6RoutesTest, SRv6VpnRoutesInvalidPrefixlenIpv6)
    {
        ASSERT_NE(m_routeSync, nullptr);

        struct nlmsg *nl_obj;
        IpPrefix _dst;
        IpAddress _vpn_sid;
        IpAddress _encap_src_addr;
        std::string path;
        std::string segment;
        std::string seg_src;

        /* Create a Netlink object containing an SRv6 VPN Route IPv6 with invalid prefix length */
        _dst = IpPrefix("fd00:0:21::");
        _vpn_sid = IpAddress("fc00:0:2:1::");
        _encap_src_addr = IpAddress("fc00:0:1:1::1");

        nl_obj = create_srv6_vpn_route_nlmsg(RTM_NEWROUTE, &_dst, &_encap_src_addr, &_vpn_sid, 10, 200);
        if (!nl_obj)
            throw std::runtime_error("SRv6 VPN Route creation failed");

        /* Send the Netlink object to the FpmLink */
        ASSERT_EQ(m_fpmLink->isRawProcessing(&nl_obj->n), true);
        m_fpmLink->processRawMsg(&nl_obj->n);

        /* Check that fpmsyncd created the correct entries in APP_DB */
        ASSERT_EQ(m_srv6SidListTable->hget("fc00:0:2:1::", "path", path), false);

        ASSERT_EQ(m_routeTable->hget("Vrf10:fd00:0:21::/200", "segment", segment), false);

        ASSERT_EQ(m_routeTable->hget("Vrf10:fd00:0:21::/200", "seg_src", seg_src), false);

        /* Destroy the Netlink object and free the memory */
        free_nlobj(nl_obj);
    }

    /* Test Receiving an invalid SRv6 VPN Route with invalid address family */
    TEST_F(FpmSyncdSRv6RoutesTest, SRv6VpnRoutesInvalidAddressFamily)
    {
        ASSERT_NE(m_routeSync, nullptr);

        struct nlmsg *nl_obj;
        IpPrefix _dst;
        IpAddress _vpn_sid;
        IpAddress _encap_src_addr;
        std::string path;
        std::string segment;
        std::string seg_src;


        /*
        * Scenario 4: SRv6 VPN Route with invalid address family
        */

        /* Create a Netlink object containing an SRv6 VPN Route IPv6 with invalid address family */
        _dst = IpPrefix("fd00:0:21::/64");
        _vpn_sid = IpAddress("fc00:0:2:1::");
        _encap_src_addr = IpAddress("fc00:0:1:1::1");

        nl_obj = create_srv6_vpn_route_nlmsg(RTM_NEWROUTE, &_dst, &_encap_src_addr, &_vpn_sid, 10, 64, 100);
        if (!nl_obj)
            throw std::runtime_error("SRv6 VPN Route creation failed");

        /* Send the Netlink object to the FpmLink */
        ASSERT_EQ(m_fpmLink->isRawProcessing(&nl_obj->n), true);
        m_fpmLink->processRawMsg(&nl_obj->n);

        /* Check that fpmsyncd created the correct entries in APP_DB */
        ASSERT_EQ(m_srv6SidListTable->hget("fc00:0:2:1::", "path", path), false);

        ASSERT_EQ(m_routeTable->hget("Vrf10:fd00:0:21::/64", "segment", segment), false);

        ASSERT_EQ(m_routeTable->hget("Vrf10:fd00:0:21::/64", "seg_src", seg_src), false);

        /* Destroy the Netlink object and free the memory */
        free_nlobj(nl_obj);
    }

    /* Test Receiving an invalid SRv6 VPN Route with invalid Vrf */
    TEST_F(FpmSyncdSRv6RoutesTest, SRv6VpnRoutesInvalidVrf)
    {
        ASSERT_NE(m_routeSync, nullptr);

        struct nlmsg *nl_obj;
        IpPrefix _dst;
        IpAddress _vpn_sid;
        IpAddress _encap_src_addr;
        std::string path;
        std::string segment;
        std::string seg_src;

        /* Create a Netlink object containing an SRv6 VPN Route IPv6 with invalid Vrf */
        _dst = IpPrefix("fd00:0:21::/64");
        _vpn_sid = IpAddress("fc00:0:2:1::");
        _encap_src_addr = IpAddress("fc00:0:1:1::1");

        nl_obj = create_srv6_vpn_route_nlmsg(RTM_NEWROUTE, &_dst, &_encap_src_addr, &_vpn_sid, 20);
        if (!nl_obj)
            throw std::runtime_error("SRv6 VPN Route creation failed");

        /* Send the Netlink object to the FpmLink */
        ASSERT_EQ(m_fpmLink->isRawProcessing(&nl_obj->n), true);
        m_fpmLink->processRawMsg(&nl_obj->n);

        /* Check that fpmsyncd created the correct entries in APP_DB */
        ASSERT_EQ(m_srv6SidListTable->hget("fc00:0:2:1::", "path", path), false);

        ASSERT_EQ(m_routeTable->hget("Vrf10:fd00:0:21::/64", "segment", segment), false);

        ASSERT_EQ(m_routeTable->hget("Vrf10:fd00:0:21::/64", "seg_src", seg_src), false);

        /* Destroy the Netlink object and free the memory */
        free_nlobj(nl_obj);
    }

    /* Test Receiving an invalid SRv6 VPN Route with invalid Vrf name (not starting with "Vrf" prefix) */
    TEST_F(FpmSyncdSRv6RoutesTest, SRv6VpnRoutesInvalidVrfName)
    {
        ASSERT_NE(m_routeSync, nullptr);

        struct nlmsg *nl_obj;
        IpPrefix _dst;
        IpAddress _vpn_sid;
        IpAddress _encap_src_addr;
        std::string path;
        std::string segment;
        std::string seg_src;

        /* Create a Netlink object containing an SRv6 VPN Route IPv6 with invalid Vrf name (not starting with "Vrf" prefix) */
        _dst = IpPrefix("fd00:0:21::/64");
        _vpn_sid = IpAddress("fc00:0:2:1::");
        _encap_src_addr = IpAddress("fc00:0:1:1::1");

        nl_obj = create_srv6_vpn_route_nlmsg(RTM_NEWROUTE, &_dst, &_encap_src_addr, &_vpn_sid, 30);
        if (!nl_obj)
            throw std::runtime_error("SRv6 VPN Route creation failed");

        /* Send the Netlink object to the FpmLink */
        ASSERT_EQ(m_fpmLink->isRawProcessing(&nl_obj->n), true);
        m_fpmLink->processRawMsg(&nl_obj->n);

        /* Check that fpmsyncd created the correct entries in APP_DB */
        ASSERT_EQ(m_srv6SidListTable->hget("fc00:0:2:1::", "path", path), false);

        ASSERT_EQ(m_routeTable->hget("invalidVrf:fd00:0:21::/64", "segment", segment), false);

        ASSERT_EQ(m_routeTable->hget("invalidVrf:fd00:0:21::/64", "seg_src", seg_src), false);

        /* Destroy the Netlink object and free the memory */
        free_nlobj(nl_obj);
    }

    /* Test Receiving an invalid SRv6 VPN Route with invalid route type (blackhole) */
    TEST_F(FpmSyncdSRv6RoutesTest, SRv6VpnRoutesInvalidRouteTypeBlackhole)
    {
        ASSERT_NE(m_routeSync, nullptr);

        struct nlmsg *nl_obj;
        IpPrefix _dst;
        IpAddress _vpn_sid;
        IpAddress _encap_src_addr;
        std::string path;
        std::string segment;
        std::string seg_src;

        /* Create a Netlink object containing an SRv6 VPN Route IPv6 with invalid route type (blackhole) */
        _dst = IpPrefix("fd00:0:21::/64");
        _vpn_sid = IpAddress("fc00:0:2:1::");
        _encap_src_addr = IpAddress("fc00:0:1:1::1");

        nl_obj = create_srv6_vpn_route_nlmsg(RTM_NEWROUTE, &_dst, &_encap_src_addr, &_vpn_sid, 30, 64, AF_INET6, RTN_BLACKHOLE);
        if (!nl_obj)
            throw std::runtime_error("SRv6 VPN Route creation failed");

        /* Send the Netlink object to the FpmLink */
        ASSERT_EQ(m_fpmLink->isRawProcessing(&nl_obj->n), true);
        m_fpmLink->processRawMsg(&nl_obj->n);

        /* Check that fpmsyncd created the correct entries in APP_DB */
        ASSERT_EQ(m_srv6SidListTable->hget("fc00:0:2:1::", "path", path), false);

        ASSERT_EQ(m_routeTable->hget("Vrf10:fd00:0:21::/64", "segment", segment), false);

        ASSERT_EQ(m_routeTable->hget("Vrf10:fd00:0:21::/64", "seg_src", seg_src), false);

        /* Destroy the Netlink object and free the memory */
        free_nlobj(nl_obj);
    }

    /* Test Receiving an invalid SRv6 VPN Route with invalid route type (multicast) */
    TEST_F(FpmSyncdSRv6RoutesTest, SRv6VpnRoutesInvalidRouteTypeMulticast)
    {
        ASSERT_NE(m_routeSync, nullptr);

        struct nlmsg *nl_obj;
        IpPrefix _dst;
        IpAddress _vpn_sid;
        IpAddress _encap_src_addr;
        std::string path;
        std::string segment;
        std::string seg_src;

        /* Create a Netlink object containing an SRv6 VPN Route IPv6 with invalid route type (multicast) */
        _dst = IpPrefix("fd00:0:21::/64");
        _vpn_sid = IpAddress("fc00:0:2:1::");
        _encap_src_addr = IpAddress("fc00:0:1:1::1");

        nl_obj = create_srv6_vpn_route_nlmsg(RTM_NEWROUTE, &_dst, &_encap_src_addr, &_vpn_sid, 30, 64, AF_INET6, RTN_MULTICAST);
        if (!nl_obj)
            throw std::runtime_error("SRv6 VPN Route creation failed");

        /* Send the Netlink object to the FpmLink */
        ASSERT_EQ(m_fpmLink->isRawProcessing(&nl_obj->n), true);
        m_fpmLink->processRawMsg(&nl_obj->n);

        /* Check that fpmsyncd created the correct entries in APP_DB */
        ASSERT_EQ(m_srv6SidListTable->hget("fc00:0:2:1::", "path", path), false);

        ASSERT_EQ(m_routeTable->hget("Vrf10:fd00:0:21::/64", "segment", segment), false);

        ASSERT_EQ(m_routeTable->hget("Vrf10:fd00:0:21::/64", "seg_src", seg_src), false);

        /* Destroy the Netlink object and free the memory */
        free_nlobj(nl_obj);
    }
}
