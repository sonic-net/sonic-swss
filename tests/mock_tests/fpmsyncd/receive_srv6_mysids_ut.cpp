#include "ut_helpers_fpmsyncd.h"
#include "gtest/gtest.h"
#include <gmock/gmock.h>
#include "mock_table.h"
#include <net/if.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include "ipaddress.h"

#define private public // Need to modify internal cache
#include "fpmlink.h"
#include "routesync.h"
#undef private

using namespace swss;
using namespace testing;

#define MY_SID_KEY_DELIMITER ':'

/*
Test Fixture
*/
namespace ut_fpmsyncd
{
    struct FpmSyncdSRv6MySIDsTest : public ::testing::Test
    {
        std::shared_ptr<swss::DBConnector> m_app_db;
        std::shared_ptr<swss::RedisPipeline> pipeline;
        std::shared_ptr<RouteSync> m_routeSync;
        std::shared_ptr<FpmLink> m_fpmLink;
        std::shared_ptr<swss::Table> m_srv6MySidTable;

        virtual void SetUp() override
        {
            testing_db::reset();

            m_app_db = std::make_shared<swss::DBConnector>("APPL_DB", 0);

            /* Construct dependencies */

            /*  1) RouteSync */
            pipeline = std::make_shared<swss::RedisPipeline>(m_app_db.get());
            m_routeSync = std::make_shared<RouteSync>(pipeline.get());

            /* 2) FpmLink */
            m_fpmLink = std::make_shared<FpmLink>(m_routeSync.get());

            /* 3) SRV6_MY_SID_TABLE in APP_DB */
            m_srv6MySidTable = std::make_shared<swss::Table>(m_app_db.get(), APP_SRV6_MY_SID_TABLE_NAME);
        }

        virtual void TearDown() override
        {
        }
    };
}

string get_srv6_my_sid_table_key(IpAddress *mysid, int8_t block_len, int8_t node_len, int8_t func_len, int8_t arg_len)
{
    string my_sid_table_key;

    my_sid_table_key += to_string(block_len);
    my_sid_table_key += MY_SID_KEY_DELIMITER;
    my_sid_table_key += to_string(node_len);
    my_sid_table_key += MY_SID_KEY_DELIMITER;
    my_sid_table_key += to_string(func_len);
    my_sid_table_key += MY_SID_KEY_DELIMITER;
    my_sid_table_key += to_string(arg_len);
    my_sid_table_key += MY_SID_KEY_DELIMITER;
    my_sid_table_key += mysid->to_string();

    return my_sid_table_key;
}

namespace ut_fpmsyncd
{
    /* Test Receiving an SRv6 My SID nexthop bound to the End behavior */
    TEST_F(FpmSyncdSRv6MySIDsTest, RecevingRouteWithSRv6MySIDEnd)
    {
        ASSERT_NE(m_routeSync, nullptr);

        /* Create a Netlink object containing an SRv6 My SID */
        IpAddress _mysid = IpAddress("fc00:0:1:1::");
        int8_t _block_len = 32;
        int8_t _node_len = 16;
        int8_t _func_len = 16;
        int8_t _arg_len = 0;
        uint32_t _action = SRV6_LOCALSID_ACTION_END;
        string my_sid_table_key = get_srv6_my_sid_table_key(&_mysid, _block_len, _node_len, _func_len, _arg_len);

        struct nlmsg *nl_obj = create_srv6_mysid_nlmsg(RTM_NEWSRV6LOCALSID, &_mysid, _block_len, _node_len, _func_len, _arg_len, _action);
        if (!nl_obj)
            throw std::runtime_error("SRv6 My SID creation failed");

        /* Send the Netlink object to the FpmLink */
        m_fpmLink->processRawMsg(&nl_obj->n);

        /* Check that fpmsyncd created the correct entries in APP_DB */
        std::string action;
        ASSERT_EQ(m_srv6MySidTable->hget(my_sid_table_key, "action", action), true);
        ASSERT_EQ(action, "end");

        /* Destroy the Netlink object and free the memory */
        free_nlobj(nl_obj);

        /* Delete My SID */
        nl_obj = create_srv6_mysid_nlmsg(RTM_DELSRV6LOCALSID, &_mysid, _block_len, _node_len, _func_len, _arg_len, _action);
        if (!nl_obj)
            throw std::runtime_error("SRv6 My SID creation failed");

        /* Send the Netlink object to the FpmLink */
        m_fpmLink->processRawMsg(&nl_obj->n);

        /* Check that fpmsyncd created the correct entries in APP_DB */
        ASSERT_EQ(m_srv6MySidTable->hget(my_sid_table_key, "action", action), false);

        /* Destroy the Netlink object and free the memory */
        free_nlobj(nl_obj);
    }

    /* Test Receiving an SRv6 My SID nexthop bound to the End.X behavior */
    TEST_F(FpmSyncdSRv6MySIDsTest, RecevingRouteWithSRv6MySIDEndX)
    {
        ASSERT_NE(m_routeSync, nullptr);

        /* Create a Netlink object containing an SRv6 My SID */
        IpAddress _mysid = IpAddress("fc00:0:1:1::");
        int8_t _block_len = 32;
        int8_t _node_len = 16;
        int8_t _func_len = 16;
        int8_t _arg_len = 0;
        uint32_t _action = SRV6_LOCALSID_ACTION_END_X;
        IpAddress _adj = IpAddress("2001:db8:1::1");
        string my_sid_table_key = get_srv6_my_sid_table_key(&_mysid, _block_len, _node_len, _func_len, _arg_len);

        struct nlmsg *nl_obj = create_srv6_mysid_nlmsg(RTM_NEWSRV6LOCALSID, &_mysid, _block_len, _node_len, _func_len, _arg_len, _action, NULL, &_adj);
        if (!nl_obj)
            throw std::runtime_error("SRv6 My SID creation failed");

        /* Send the Netlink object to the FpmLink */
        m_fpmLink->processRawMsg(&nl_obj->n);

        /* Check that fpmsyncd created the correct entries in APP_DB */
        std::string action;
        ASSERT_EQ(m_srv6MySidTable->hget(my_sid_table_key, "action", action), true);
        ASSERT_EQ(action, "end.x");

        std::string adj;
        ASSERT_EQ(m_srv6MySidTable->hget(my_sid_table_key, "adj", adj), true);
        ASSERT_EQ(adj, "2001:db8:1::1");

        /* Destroy the Netlink object and free the memory */
        free_nlobj(nl_obj);

        /* Delete My SID */
        nl_obj = create_srv6_mysid_nlmsg(RTM_DELSRV6LOCALSID, &_mysid, _block_len, _node_len, _func_len, _arg_len, _action, NULL, &_adj);
        if (!nl_obj)
            throw std::runtime_error("SRv6 My SID creation failed");

        /* Send the Netlink object to the FpmLink */
        m_fpmLink->processRawMsg(&nl_obj->n);

        /* Check that fpmsyncd created the correct entries in APP_DB */
        ASSERT_EQ(m_srv6MySidTable->hget(my_sid_table_key, "action", action), false);

        ASSERT_EQ(m_srv6MySidTable->hget(my_sid_table_key, "adj", adj), false);

        /* Destroy the Netlink object and free the memory */
        free_nlobj(nl_obj);
    }

    /* Test Receiving an SRv6 My SID nexthop bound to the End.T behavior */
    TEST_F(FpmSyncdSRv6MySIDsTest, RecevingRouteWithSRv6MySIDEndT)
    {
        ASSERT_NE(m_routeSync, nullptr);

        /* Create a Netlink object containing an SRv6 My SID */
        IpAddress _mysid = IpAddress("fc00:0:1:1::");
        int8_t _block_len = 32;
        int8_t _node_len = 16;
        int8_t _func_len = 16;
        int8_t _arg_len = 0;
        uint32_t _action = SRV6_LOCALSID_ACTION_END_T;
        char *_vrf = "Vrf10";
        string my_sid_table_key = get_srv6_my_sid_table_key(&_mysid, _block_len, _node_len, _func_len, _arg_len);

        struct nlmsg *nl_obj = create_srv6_mysid_nlmsg(RTM_NEWSRV6LOCALSID, &_mysid, _block_len, _node_len, _func_len, _arg_len, _action, _vrf);
        if (!nl_obj)
            throw std::runtime_error("SRv6 My SID creation failed");

        /* Send the Netlink object to the FpmLink */
        m_fpmLink->processRawMsg(&nl_obj->n);

        /* Check that fpmsyncd created the correct entries in APP_DB */
        std::string action;
        ASSERT_EQ(m_srv6MySidTable->hget(my_sid_table_key, "action", action), true);
        ASSERT_EQ(action, "end.t");

        std::string vrf;
        ASSERT_EQ(m_srv6MySidTable->hget(my_sid_table_key, "vrf", vrf), true);
        ASSERT_EQ(vrf, "Vrf10");

        /* Destroy the Netlink object and free the memory */
        free_nlobj(nl_obj);

        /* Delete My SID */
        nl_obj = create_srv6_mysid_nlmsg(RTM_DELSRV6LOCALSID, &_mysid, _block_len, _node_len, _func_len, _arg_len, _action, _vrf);
        if (!nl_obj)
            throw std::runtime_error("SRv6 My SID creation failed");

        /* Send the Netlink object to the FpmLink */
        m_fpmLink->processRawMsg(&nl_obj->n);

        /* Check that fpmsyncd created the correct entries in APP_DB */
        ASSERT_EQ(m_srv6MySidTable->hget(my_sid_table_key, "action", action), false);

        ASSERT_EQ(m_srv6MySidTable->hget(my_sid_table_key, "vrf", vrf), false);

        /* Destroy the Netlink object and free the memory */
        free_nlobj(nl_obj);
    }

    /* Test Receiving an SRv6 My SID nexthop bound to the End.DX6 behavior */
    TEST_F(FpmSyncdSRv6MySIDsTest, RecevingRouteWithSRv6MySIDEndDX6)
    {
        ASSERT_NE(m_routeSync, nullptr);

        /* Create a Netlink object containing an SRv6 My SID */
        IpAddress _mysid = IpAddress("fc00:0:1:1::");
        int8_t _block_len = 32;
        int8_t _node_len = 16;
        int8_t _func_len = 16;
        int8_t _arg_len = 0;
        uint32_t _action = SRV6_LOCALSID_ACTION_END_DX6;
        IpAddress _adj = IpAddress("2001:db8:1::1");
        string my_sid_table_key = get_srv6_my_sid_table_key(&_mysid, _block_len, _node_len, _func_len, _arg_len);

        struct nlmsg *nl_obj = create_srv6_mysid_nlmsg(RTM_NEWSRV6LOCALSID, &_mysid, _block_len, _node_len, _func_len, _arg_len, _action, NULL, &_adj);
        if (!nl_obj)
            throw std::runtime_error("SRv6 My SID creation failed");

        /* Send the Netlink object to the FpmLink */
        m_fpmLink->processRawMsg(&nl_obj->n);

        /* Check that fpmsyncd created the correct entries in APP_DB */
        std::string action;
        ASSERT_EQ(m_srv6MySidTable->hget(my_sid_table_key, "action", action), true);
        ASSERT_EQ(action, "end.dx6");

        std::string adj;
        ASSERT_EQ(m_srv6MySidTable->hget(my_sid_table_key, "adj", adj), true);
        ASSERT_EQ(adj, "2001:db8:1::1");

        /* Destroy the Netlink object and free the memory */
        free_nlobj(nl_obj);

        /* Delete My SID */
        nl_obj = create_srv6_mysid_nlmsg(RTM_DELSRV6LOCALSID, &_mysid, _block_len, _node_len, _func_len, _arg_len, _action, NULL, &_adj);
        if (!nl_obj)
            throw std::runtime_error("SRv6 My SID creation failed");

        /* Send the Netlink object to the FpmLink */
        m_fpmLink->processRawMsg(&nl_obj->n);

        /* Check that fpmsyncd created the correct entries in APP_DB */
        ASSERT_EQ(m_srv6MySidTable->hget(my_sid_table_key, "action", action), false);

        ASSERT_EQ(m_srv6MySidTable->hget(my_sid_table_key, "adj", adj), false);
        /* Destroy the Netlink object and free the memory */
        free_nlobj(nl_obj);
    }

    /* Test Receiving an SRv6 My SID nexthop bound to the End.DX4 behavior */
    TEST_F(FpmSyncdSRv6MySIDsTest, RecevingRouteWithSRv6MySIDEndDX4)
    {
        ASSERT_NE(m_routeSync, nullptr);

        /* Create a Netlink object containing an SRv6 My SID */
        IpAddress _mysid = IpAddress("fc00:0:1:1::");
        int8_t _block_len = 32;
        int8_t _node_len = 16;
        int8_t _func_len = 16;
        int8_t _arg_len = 0;
        uint32_t _action = SRV6_LOCALSID_ACTION_END_DX4;
        IpAddress _adj = IpAddress("10.0.0.1");
        string my_sid_table_key = get_srv6_my_sid_table_key(&_mysid, _block_len, _node_len, _func_len, _arg_len);

        struct nlmsg *nl_obj = create_srv6_mysid_nlmsg(RTM_NEWSRV6LOCALSID, &_mysid, _block_len, _node_len, _func_len, _arg_len, _action, NULL, &_adj);
        if (!nl_obj)
            throw std::runtime_error("SRv6 My SID creation failed");

        /* Send the Netlink object to the FpmLink */
        m_fpmLink->processRawMsg(&nl_obj->n);

        /* Check that fpmsyncd created the correct entries in APP_DB */
        std::string action;
        ASSERT_EQ(m_srv6MySidTable->hget(my_sid_table_key, "action", action), true);
        ASSERT_EQ(action, "end.dx4");

        std::string adj;
        ASSERT_EQ(m_srv6MySidTable->hget(my_sid_table_key, "adj", adj), true);
        ASSERT_EQ(adj, "10.0.0.1");

        /* Destroy the Netlink object and free the memory */
        free_nlobj(nl_obj);

        /* Delete My SID */
        nl_obj = create_srv6_mysid_nlmsg(RTM_DELSRV6LOCALSID, &_mysid, _block_len, _node_len, _func_len, _arg_len, _action, NULL, &_adj);
        if (!nl_obj)
            throw std::runtime_error("SRv6 My SID creation failed");

        /* Send the Netlink object to the FpmLink */
        m_fpmLink->processRawMsg(&nl_obj->n);

        /* Check that fpmsyncd created the correct entries in APP_DB */
        ASSERT_EQ(m_srv6MySidTable->hget(my_sid_table_key, "action", action), false);

        ASSERT_EQ(m_srv6MySidTable->hget(my_sid_table_key, "adj", adj), false);

        /* Destroy the Netlink object and free the memory */
        free_nlobj(nl_obj);
    }

    /* Test Receiving an SRv6 My SID nexthop bound to the End.DT4 behavior */
    TEST_F(FpmSyncdSRv6MySIDsTest, RecevingRouteWithSRv6MySIDEndDT4)
    {
        ASSERT_NE(m_routeSync, nullptr);

        /* Create a Netlink object containing an SRv6 My SID */
        IpAddress _mysid = IpAddress("fc00:0:1:1::");
        int8_t _block_len = 32;
        int8_t _node_len = 16;
        int8_t _func_len = 16;
        int8_t _arg_len = 0;
        uint32_t _action = SRV6_LOCALSID_ACTION_END_DT4;
        char *_vrf = "Vrf10";
        string my_sid_table_key = get_srv6_my_sid_table_key(&_mysid, _block_len, _node_len, _func_len, _arg_len);

        struct nlmsg *nl_obj = create_srv6_mysid_nlmsg(RTM_NEWSRV6LOCALSID, &_mysid, _block_len, _node_len, _func_len, _arg_len, _action, _vrf);
        if (!nl_obj)
            throw std::runtime_error("SRv6 My SID creation failed");

        /* Send the Netlink object to the FpmLink */
        m_fpmLink->processRawMsg(&nl_obj->n);

        /* Check that fpmsyncd created the correct entries in APP_DB */
        std::string action;
        ASSERT_EQ(m_srv6MySidTable->hget(my_sid_table_key, "action", action), true);
        ASSERT_EQ(action, "end.dt4");

        std::string vrf;
        ASSERT_EQ(m_srv6MySidTable->hget(my_sid_table_key, "vrf", vrf), true);
        ASSERT_EQ(vrf, "Vrf10");

        /* Destroy the Netlink object and free the memory */
        free_nlobj(nl_obj);

        /* Delete My SID */
        nl_obj = create_srv6_mysid_nlmsg(RTM_DELSRV6LOCALSID, &_mysid, _block_len, _node_len, _func_len, _arg_len, _action, _vrf);
        if (!nl_obj)
            throw std::runtime_error("SRv6 My SID creation failed");

        /* Send the Netlink object to the FpmLink */
        m_fpmLink->processRawMsg(&nl_obj->n);

        /* Check that fpmsyncd created the correct entries in APP_DB */
        ASSERT_EQ(m_srv6MySidTable->hget(my_sid_table_key, "action", action), false);

        ASSERT_EQ(m_srv6MySidTable->hget(my_sid_table_key, "vrf", vrf), false);

        /* Destroy the Netlink object and free the memory */
        free_nlobj(nl_obj);
    }

    /* Test Receiving an SRv6 My SID nexthop bound to the End.DT6 behavior */
    TEST_F(FpmSyncdSRv6MySIDsTest, RecevingRouteWithSRv6MySIDEndDT6)
    {
        ASSERT_NE(m_routeSync, nullptr);

        /* Create a Netlink object containing an SRv6 My SID */
        IpAddress _mysid = IpAddress("fc00:0:1:1::");
        int8_t _block_len = 32;
        int8_t _node_len = 16;
        int8_t _func_len = 16;
        int8_t _arg_len = 0;
        uint32_t _action = SRV6_LOCALSID_ACTION_END_DT6;
        char *_vrf = "Vrf10";
        string my_sid_table_key = get_srv6_my_sid_table_key(&_mysid, _block_len, _node_len, _func_len, _arg_len);

        struct nlmsg *nl_obj = create_srv6_mysid_nlmsg(RTM_NEWSRV6LOCALSID, &_mysid, _block_len, _node_len, _func_len, _arg_len, _action, _vrf);
        if (!nl_obj)
            throw std::runtime_error("SRv6 My SID creation failed");

        /* Send the Netlink object to the FpmLink */
        m_fpmLink->processRawMsg(&nl_obj->n);

        /* Check that fpmsyncd created the correct entries in APP_DB */
        std::string action;
        ASSERT_EQ(m_srv6MySidTable->hget(my_sid_table_key, "action", action), true);
        ASSERT_EQ(action, "end.dt6");

        std::string vrf;
        ASSERT_EQ(m_srv6MySidTable->hget(my_sid_table_key, "vrf", vrf), true);
        ASSERT_EQ(vrf, "Vrf10");

        /* Destroy the Netlink object and free the memory */
        free_nlobj(nl_obj);

        /* Delete My SID */
        nl_obj = create_srv6_mysid_nlmsg(RTM_DELSRV6LOCALSID, &_mysid, _block_len, _node_len, _func_len, _arg_len, _action, _vrf);
        if (!nl_obj)
            throw std::runtime_error("SRv6 My SID creation failed");

        /* Send the Netlink object to the FpmLink */
        m_fpmLink->processRawMsg(&nl_obj->n);

        /* Check that fpmsyncd created the correct entries in APP_DB */
        ASSERT_EQ(m_srv6MySidTable->hget(my_sid_table_key, "action", action), false);

        ASSERT_EQ(m_srv6MySidTable->hget(my_sid_table_key, "vrf", vrf), false);

        /* Destroy the Netlink object and free the memory */
        free_nlobj(nl_obj);
    }

    /* Test Receiving an SRv6 My SID nexthop bound to the End.DT46 behavior */
    TEST_F(FpmSyncdSRv6MySIDsTest, RecevingRouteWithSRv6MySIDEndDT46)
    {
        ASSERT_NE(m_routeSync, nullptr);

        /* Create a Netlink object containing an SRv6 My SID */
        IpAddress _mysid = IpAddress("fc00:0:1:1::");
        int8_t _block_len = 32;
        int8_t _node_len = 16;
        int8_t _func_len = 16;
        int8_t _arg_len = 0;
        uint32_t _action = SRV6_LOCALSID_ACTION_END_DT46;
        char *_vrf = "Vrf10";
        string my_sid_table_key = get_srv6_my_sid_table_key(&_mysid, _block_len, _node_len, _func_len, _arg_len);

        struct nlmsg *nl_obj = create_srv6_mysid_nlmsg(RTM_NEWSRV6LOCALSID, &_mysid, _block_len, _node_len, _func_len, _arg_len, _action, _vrf);
        if (!nl_obj)
            throw std::runtime_error("SRv6 My SID creation failed");

        /* Send the Netlink object to the FpmLink */
        m_fpmLink->processRawMsg(&nl_obj->n);

        /* Check that fpmsyncd created the correct entries in APP_DB */
        std::string action;
        ASSERT_EQ(m_srv6MySidTable->hget(my_sid_table_key, "action", action), true);
        ASSERT_EQ(action, "end.dt46");

        std::string vrf;
        ASSERT_EQ(m_srv6MySidTable->hget(my_sid_table_key, "vrf", vrf), true);
        ASSERT_EQ(vrf, "Vrf10");

        /* Destroy the Netlink object and free the memory */
        free_nlobj(nl_obj);

        /* Delete My SID */
        nl_obj = create_srv6_mysid_nlmsg(RTM_DELSRV6LOCALSID, &_mysid, _block_len, _node_len, _func_len, _arg_len, _action, _vrf);
        if (!nl_obj)
            throw std::runtime_error("SRv6 My SID creation failed");

        /* Send the Netlink object to the FpmLink */
        m_fpmLink->processRawMsg(&nl_obj->n);

        /* Check that fpmsyncd created the correct entries in APP_DB */
        ASSERT_EQ(m_srv6MySidTable->hget(my_sid_table_key, "action", action), false);

        ASSERT_EQ(m_srv6MySidTable->hget(my_sid_table_key, "vrf", vrf), false);

        /* Destroy the Netlink object and free the memory */
        free_nlobj(nl_obj);
    }

    /* Test Receiving an SRv6 My SID nexthop bound to the uN behavior */
    TEST_F(FpmSyncdSRv6MySIDsTest, RecevingRouteWithSRv6MySIDUN)
    {
        ASSERT_NE(m_routeSync, nullptr);

        /* Create a Netlink object containing an SRv6 My SID */
        IpAddress _mysid = IpAddress("fc00:0:1::");
        int8_t _block_len = 32;
        int8_t _node_len = 16;
        int8_t _func_len = 16;
        int8_t _arg_len = 0;
        uint32_t _action = SRV6_LOCALSID_ACTION_UN;
        string my_sid_table_key = get_srv6_my_sid_table_key(&_mysid, _block_len, _node_len, _func_len, _arg_len);

        struct nlmsg *nl_obj = create_srv6_mysid_nlmsg(RTM_NEWSRV6LOCALSID, &_mysid, _block_len, _node_len, _func_len, _arg_len, _action);
        if (!nl_obj)
            throw std::runtime_error("SRv6 My SID creation failed");

        /* Send the Netlink object to the FpmLink */
        m_fpmLink->processRawMsg(&nl_obj->n);

        /* Check that fpmsyncd created the correct entries in APP_DB */
        std::string action;
        ASSERT_EQ(m_srv6MySidTable->hget(my_sid_table_key, "action", action), true);
        ASSERT_EQ(action, "un");

        /* Destroy the Netlink object and free the memory */
        free_nlobj(nl_obj);

        /* Delete My SID */
        nl_obj = create_srv6_mysid_nlmsg(RTM_DELSRV6LOCALSID, &_mysid, _block_len, _node_len, _func_len, _arg_len, _action);
        if (!nl_obj)
            throw std::runtime_error("SRv6 My SID creation failed");

        /* Send the Netlink object to the FpmLink */
        m_fpmLink->processRawMsg(&nl_obj->n);

        /* Check that fpmsyncd created the correct entries in APP_DB */
        ASSERT_EQ(m_srv6MySidTable->hget(my_sid_table_key, "action", action), false);

        /* Destroy the Netlink object and free the memory */
        free_nlobj(nl_obj);
    }

    /* Test Receiving an SRv6 My SID nexthop bound to the uA behavior */
    TEST_F(FpmSyncdSRv6MySIDsTest, RecevingRouteWithSRv6MySIDUA)
    {
        ASSERT_NE(m_routeSync, nullptr);

        /* Create a Netlink object containing an SRv6 My SID */
        IpAddress _mysid = IpAddress("fc00:0:1:e000::");
        int8_t _block_len = 32;
        int8_t _node_len = 16;
        int8_t _func_len = 16;
        int8_t _arg_len = 0;
        uint32_t _action = SRV6_LOCALSID_ACTION_UA;
        IpAddress _adj = IpAddress("2001:db8:1::1");
        string my_sid_table_key = get_srv6_my_sid_table_key(&_mysid, _block_len, _node_len, _func_len, _arg_len);

        struct nlmsg *nl_obj = create_srv6_mysid_nlmsg(RTM_NEWSRV6LOCALSID, &_mysid, _block_len, _node_len, _func_len, _arg_len, _action, NULL, &_adj);
        if (!nl_obj)
            throw std::runtime_error("SRv6 My SID creation failed");

        /* Send the Netlink object to the FpmLink */
        m_fpmLink->processRawMsg(&nl_obj->n);

        /* Check that fpmsyncd created the correct entries in APP_DB */
        std::string action;
        ASSERT_EQ(m_srv6MySidTable->hget(my_sid_table_key, "action", action), true);
        ASSERT_EQ(action, "ua");

        std::string adj;
        ASSERT_EQ(m_srv6MySidTable->hget(my_sid_table_key, "adj", adj), true);
        ASSERT_EQ(adj, "2001:db8:1::1");

        /* Destroy the Netlink object and free the memory */
        free_nlobj(nl_obj);

        /* Delete My SID */
        nl_obj = create_srv6_mysid_nlmsg(RTM_DELSRV6LOCALSID, &_mysid, _block_len, _node_len, _func_len, _arg_len, _action, NULL, &_adj);
        if (!nl_obj)
            throw std::runtime_error("SRv6 My SID creation failed");

        /* Send the Netlink object to the FpmLink */
        m_fpmLink->processRawMsg(&nl_obj->n);

        /* Check that fpmsyncd created the correct entries in APP_DB */
        ASSERT_EQ(m_srv6MySidTable->hget(my_sid_table_key, "action", action), false);

        ASSERT_EQ(m_srv6MySidTable->hget(my_sid_table_key, "adj", adj), false);

        /* Destroy the Netlink object and free the memory */
        free_nlobj(nl_obj);
    }

    /* Test Receiving an SRv6 My SID nexthop bound to the uA behavior with an interface */
    TEST_F(FpmSyncdSRv6MySIDsTest, RecevingRouteWithSRv6MySIDUAWithIntf)
    {
        ASSERT_NE(m_routeSync, nullptr);

        /* Create a Netlink object containing an SRv6 My SID */
        IpAddress _mysid = IpAddress("fc00:0:1:e000::");
        int8_t _block_len = 32;
        int8_t _node_len = 16;
        int8_t _func_len = 16;
        int8_t _arg_len = 0;
        uint32_t _action = SRV6_LOCALSID_ACTION_UA;
        IpAddress _adj = IpAddress("fe80::e822:daff:feab:3ee9");
        char *_intf = "Ethernet0";
        string my_sid_table_key = get_srv6_my_sid_table_key(&_mysid, _block_len, _node_len, _func_len, _arg_len);

        struct nlmsg *nl_obj = create_srv6_mysid_nlmsg(RTM_NEWSRV6LOCALSID, &_mysid, _block_len, _node_len, _func_len, _arg_len, _action, NULL, &_adj, _intf);
        if (!nl_obj)
            throw std::runtime_error("SRv6 My SID creation failed");

        /* Send the Netlink object to the FpmLink */
        m_fpmLink->processRawMsg(&nl_obj->n);

        /* Check that fpmsyncd created the correct entries in APP_DB */
        std::string action;
        ASSERT_EQ(m_srv6MySidTable->hget(my_sid_table_key, "action", action), true);
        ASSERT_EQ(action, "ua");

        std::string adj;
        ASSERT_EQ(m_srv6MySidTable->hget(my_sid_table_key, "adj", adj), true);
        ASSERT_EQ(adj, "fe80::e822:daff:feab:3ee9@Ethernet0");

        /* Destroy the Netlink object and free the memory */
        free_nlobj(nl_obj);

        /* Delete My SID */
        nl_obj = create_srv6_mysid_nlmsg(RTM_DELSRV6LOCALSID, &_mysid, _block_len, _node_len, _func_len, _arg_len, _action, NULL, &_adj, _intf);
        if (!nl_obj)
            throw std::runtime_error("SRv6 My SID creation failed");

        /* Send the Netlink object to the FpmLink */
        m_fpmLink->processRawMsg(&nl_obj->n);

        /* Check that fpmsyncd created the correct entries in APP_DB */
        ASSERT_EQ(m_srv6MySidTable->hget(my_sid_table_key, "action", action), false);

        ASSERT_EQ(m_srv6MySidTable->hget(my_sid_table_key, "adj", adj), false);

        /* Destroy the Netlink object and free the memory */
        free_nlobj(nl_obj);
    }

    /* Test Receiving an SRv6 My SID nexthop bound to the uDX6 behavior */
    TEST_F(FpmSyncdSRv6MySIDsTest, RecevingRouteWithSRv6MySIDUDX6)
    {
        ASSERT_NE(m_routeSync, nullptr);

        /* Create a Netlink object containing an SRv6 My SID */
        IpAddress _mysid = IpAddress("fc00:0:1:e000::");
        int8_t _block_len = 32;
        int8_t _node_len = 16;
        int8_t _func_len = 16;
        int8_t _arg_len = 0;
        uint32_t _action = SRV6_LOCALSID_ACTION_UDX6;
        IpAddress _adj = IpAddress("2001:db8:1::1");
        string my_sid_table_key = get_srv6_my_sid_table_key(&_mysid, _block_len, _node_len, _func_len, _arg_len);

        struct nlmsg *nl_obj = create_srv6_mysid_nlmsg(RTM_NEWSRV6LOCALSID, &_mysid, _block_len, _node_len, _func_len, _arg_len, _action, NULL, &_adj);
        if (!nl_obj)
            throw std::runtime_error("SRv6 My SID creation failed");

        /* Send the Netlink object to the FpmLink */
        m_fpmLink->processRawMsg(&nl_obj->n);

        /* Check that fpmsyncd created the correct entries in APP_DB */
        std::string action;
        ASSERT_EQ(m_srv6MySidTable->hget(my_sid_table_key, "action", action), true);
        ASSERT_EQ(action, "udx6");

        std::string adj;
        ASSERT_EQ(m_srv6MySidTable->hget(my_sid_table_key, "adj", adj), true);
        ASSERT_EQ(adj, "2001:db8:1::1");

        /* Destroy the Netlink object and free the memory */
        free_nlobj(nl_obj);

        /* Delete My SID */
        nl_obj = create_srv6_mysid_nlmsg(RTM_DELSRV6LOCALSID, &_mysid, _block_len, _node_len, _func_len, _arg_len, _action, NULL, &_adj);
        if (!nl_obj)
            throw std::runtime_error("SRv6 My SID creation failed");

        /* Send the Netlink object to the FpmLink */
        m_fpmLink->processRawMsg(&nl_obj->n);

        /* Check that fpmsyncd created the correct entries in APP_DB */
        ASSERT_EQ(m_srv6MySidTable->hget(my_sid_table_key, "action", action), false);

        ASSERT_EQ(m_srv6MySidTable->hget(my_sid_table_key, "adj", adj), false);

        /* Destroy the Netlink object and free the memory */
        free_nlobj(nl_obj);
    }

    /* Test Receiving an SRv6 My SID nexthop bound to the uDX4 behavior */
    TEST_F(FpmSyncdSRv6MySIDsTest, RecevingRouteWithSRv6MySIDUDX4)
    {
        ASSERT_NE(m_routeSync, nullptr);

        /* Create a Netlink object containing an SRv6 My SID */
        IpAddress _mysid = IpAddress("fc00:0:1:e000::");
        int8_t _block_len = 32;
        int8_t _node_len = 16;
        int8_t _func_len = 16;
        int8_t _arg_len = 0;
        uint32_t _action = SRV6_LOCALSID_ACTION_UDX4;
        IpAddress _adj = IpAddress("10.0.0.1");
        string my_sid_table_key = get_srv6_my_sid_table_key(&_mysid, _block_len, _node_len, _func_len, _arg_len);

        struct nlmsg *nl_obj = create_srv6_mysid_nlmsg(RTM_NEWSRV6LOCALSID, &_mysid, _block_len, _node_len, _func_len, _arg_len, _action, NULL, &_adj);
        if (!nl_obj)
            throw std::runtime_error("SRv6 My SID creation failed");

        /* Send the Netlink object to the FpmLink */
        m_fpmLink->processRawMsg(&nl_obj->n);

        /* Check that fpmsyncd created the correct entries in APP_DB */
        std::string action;
        ASSERT_EQ(m_srv6MySidTable->hget(my_sid_table_key, "action", action), true);
        ASSERT_EQ(action, "udx4");

        std::string adj;
        ASSERT_EQ(m_srv6MySidTable->hget(my_sid_table_key, "adj", adj), true);
        ASSERT_EQ(adj, "10.0.0.1");

        /* Destroy the Netlink object and free the memory */
        free_nlobj(nl_obj);

        /* Delete My SID */
        nl_obj = create_srv6_mysid_nlmsg(RTM_DELSRV6LOCALSID, &_mysid, _block_len, _node_len, _func_len, _arg_len, _action, NULL, &_adj);
        if (!nl_obj)
            throw std::runtime_error("SRv6 My SID creation failed");

        /* Send the Netlink object to the FpmLink */
        m_fpmLink->processRawMsg(&nl_obj->n);

        /* Check that fpmsyncd created the correct entries in APP_DB */
        ASSERT_EQ(m_srv6MySidTable->hget(my_sid_table_key, "action", action), false);

        ASSERT_EQ(m_srv6MySidTable->hget(my_sid_table_key, "adj", adj), false);

        /* Destroy the Netlink object and free the memory */
        free_nlobj(nl_obj);
    }

    /* Test Receiving an SRv6 My SID nexthop bound to the uDT4 behavior */
    TEST_F(FpmSyncdSRv6MySIDsTest, RecevingRouteWithSRv6MySIDUDT4)
    {
        ASSERT_NE(m_routeSync, nullptr);

        /* Create a Netlink object containing an SRv6 My SID */
        IpAddress _mysid = IpAddress("fc00:0:1:e000::");
        int8_t _block_len = 32;
        int8_t _node_len = 16;
        int8_t _func_len = 16;
        int8_t _arg_len = 0;
        uint32_t _action = SRV6_LOCALSID_ACTION_UDT4;
        char *_vrf = "Vrf10";
        string my_sid_table_key = get_srv6_my_sid_table_key(&_mysid, _block_len, _node_len, _func_len, _arg_len);

        struct nlmsg *nl_obj = create_srv6_mysid_nlmsg(RTM_NEWSRV6LOCALSID, &_mysid, _block_len, _node_len, _func_len, _arg_len, _action, _vrf);
        if (!nl_obj)
            throw std::runtime_error("SRv6 My SID creation failed");

        /* Send the Netlink object to the FpmLink */
        m_fpmLink->processRawMsg(&nl_obj->n);

        /* Check that fpmsyncd created the correct entries in APP_DB */
        std::string action;
        ASSERT_EQ(m_srv6MySidTable->hget(my_sid_table_key, "action", action), true);
        ASSERT_EQ(action, "udt4");

        std::string vrf;
        ASSERT_EQ(m_srv6MySidTable->hget(my_sid_table_key, "vrf", vrf), true);
        ASSERT_EQ(vrf, "Vrf10");

        /* Destroy the Netlink object and free the memory */
        free_nlobj(nl_obj);

        /* Delete My SID */
        nl_obj = create_srv6_mysid_nlmsg(RTM_DELSRV6LOCALSID, &_mysid, _block_len, _node_len, _func_len, _arg_len, _action, _vrf);
        if (!nl_obj)
            throw std::runtime_error("SRv6 My SID creation failed");

        /* Send the Netlink object to the FpmLink */
        m_fpmLink->processRawMsg(&nl_obj->n);

        /* Check that fpmsyncd created the correct entries in APP_DB */
        ASSERT_EQ(m_srv6MySidTable->hget(my_sid_table_key, "action", action), false);

        ASSERT_EQ(m_srv6MySidTable->hget(my_sid_table_key, "vrf", vrf), false);

        /* Destroy the Netlink object and free the memory */
        free_nlobj(nl_obj);
    }

    /* Test Receiving an SRv6 My SID nexthop bound to the uDT6 behavior */
    TEST_F(FpmSyncdSRv6MySIDsTest, RecevingRouteWithSRv6MySIDUDT6)
    {
        ASSERT_NE(m_routeSync, nullptr);

        /* Create a Netlink object containing an SRv6 My SID */
        IpAddress _mysid = IpAddress("fc00:0:1:e000::");
        int8_t _block_len = 32;
        int8_t _node_len = 16;
        int8_t _func_len = 16;
        int8_t _arg_len = 0;
        uint32_t _action = SRV6_LOCALSID_ACTION_UDT6;
        char *_vrf = "Vrf10";
        string my_sid_table_key = get_srv6_my_sid_table_key(&_mysid, _block_len, _node_len, _func_len, _arg_len);

        struct nlmsg *nl_obj = create_srv6_mysid_nlmsg(RTM_NEWSRV6LOCALSID, &_mysid, _block_len, _node_len, _func_len, _arg_len, _action, _vrf);
        if (!nl_obj)
            throw std::runtime_error("SRv6 My SID creation failed");

        /* Send the Netlink object to the FpmLink */
        m_fpmLink->processRawMsg(&nl_obj->n);

        /* Check that fpmsyncd created the correct entries in APP_DB */
        std::string action;
        ASSERT_EQ(m_srv6MySidTable->hget(my_sid_table_key, "action", action), true);
        ASSERT_EQ(action, "udt6");

        std::string vrf;
        ASSERT_EQ(m_srv6MySidTable->hget(my_sid_table_key, "vrf", vrf), true);
        ASSERT_EQ(vrf, "Vrf10");

        /* Destroy the Netlink object and free the memory */
        free_nlobj(nl_obj);

        /* Delete My SID */
        nl_obj = create_srv6_mysid_nlmsg(RTM_DELSRV6LOCALSID, &_mysid, _block_len, _node_len, _func_len, _arg_len, _action, _vrf);
        if (!nl_obj)
            throw std::runtime_error("SRv6 My SID creation failed");

        /* Send the Netlink object to the FpmLink */
        m_fpmLink->processRawMsg(&nl_obj->n);

        /* Check that fpmsyncd created the correct entries in APP_DB */
        ASSERT_EQ(m_srv6MySidTable->hget(my_sid_table_key, "action", action), false);

        ASSERT_EQ(m_srv6MySidTable->hget(my_sid_table_key, "vrf", vrf), false);

        /* Destroy the Netlink object and free the memory */
        free_nlobj(nl_obj);
    }

    /* Test Receiving an SRv6 My SID nexthop bound to the uDT46 behavior */
    TEST_F(FpmSyncdSRv6MySIDsTest, RecevingRouteWithSRv6MySIDUDT46)
    {
        ASSERT_NE(m_routeSync, nullptr);

        /* Create a Netlink object containing an SRv6 My SID */
        IpAddress _mysid = IpAddress("fc00:0:1:e000::");
        int8_t _block_len = 32;
        int8_t _node_len = 16;
        int8_t _func_len = 16;
        int8_t _arg_len = 0;
        uint32_t _action = SRV6_LOCALSID_ACTION_UDT46;
        char *_vrf = "Vrf10";
        string my_sid_table_key = get_srv6_my_sid_table_key(&_mysid, _block_len, _node_len, _func_len, _arg_len);

        struct nlmsg *nl_obj = create_srv6_mysid_nlmsg(RTM_NEWSRV6LOCALSID, &_mysid, _block_len, _node_len, _func_len, _arg_len, _action, _vrf);
        if (!nl_obj)
            throw std::runtime_error("SRv6 My SID creation failed");

        /* Send the Netlink object to the FpmLink */
        m_fpmLink->processRawMsg(&nl_obj->n);

        /* Check that fpmsyncd created the correct entries in APP_DB */
        std::string action;
        ASSERT_EQ(m_srv6MySidTable->hget(my_sid_table_key, "action", action), true);
        ASSERT_EQ(action, "udt46");

        std::string vrf;
        ASSERT_EQ(m_srv6MySidTable->hget(my_sid_table_key, "vrf", vrf), true);
        ASSERT_EQ(vrf, "Vrf10");

        /* Destroy the Netlink object and free the memory */
        free_nlobj(nl_obj);

        /* Delete My SID */
        nl_obj = create_srv6_mysid_nlmsg(RTM_DELSRV6LOCALSID, &_mysid, _block_len, _node_len, _func_len, _arg_len, _action, _vrf);
        if (!nl_obj)
            throw std::runtime_error("SRv6 My SID creation failed");

        /* Send the Netlink object to the FpmLink */
        m_fpmLink->processRawMsg(&nl_obj->n);

        /* Check that fpmsyncd created the correct entries in APP_DB */
        ASSERT_EQ(m_srv6MySidTable->hget(my_sid_table_key, "action", action), false);

        ASSERT_EQ(m_srv6MySidTable->hget(my_sid_table_key, "vrf", vrf), false);

        /* Destroy the Netlink object and free the memory */
        free_nlobj(nl_obj);
    }

    /* Test Receiving an SRv6 My SID with default SID structure */
    TEST_F(FpmSyncdSRv6MySIDsTest, SRv6MySidEndDefaultSidStructure)
    {
        ASSERT_NE(m_routeSync, nullptr);

        shared_ptr<swss::DBConnector> m_app_db;
        m_app_db = make_shared<swss::DBConnector>("APPL_DB", 0);
        Table srv6_my_sid_table(m_app_db.get(), APP_SRV6_MY_SID_TABLE_NAME);

        struct nlmsg *nl_obj;
        IpAddress _mysid = IpAddress("fc00:0:1:40::");
        int8_t _block_len = 32;
        int8_t _node_len = 16;
        int8_t _func_len = 16;
        int8_t _arg_len = 0;
        std::string action;
        std::string adj;
        std::string vrf;
        string my_sid_table_key = get_srv6_my_sid_table_key(&_mysid, _block_len, _node_len, _func_len, _arg_len);

        /* Create a Netlink object containing an SRv6 My SID */

        nl_obj = create_srv6_mysid_nlmsg(RTM_NEWSRV6LOCALSID, &_mysid, -1, -1, -1, -1, SRV6_LOCALSID_ACTION_END);
        if (!nl_obj)
            throw std::runtime_error("SRv6 My SID creation failed");

        /* Send the Netlink object to the FpmLink */
        ASSERT_EQ(m_fpmLink->isRawProcessing(&nl_obj->n), true);
        m_fpmLink->processRawMsg(&nl_obj->n);

        /* Check that fpmsyncd created the correct entries in APP_DB */
        ASSERT_EQ(m_srv6MySidTable->hget(my_sid_table_key, "action", action), true);
        ASSERT_EQ(action, "end");

        /* Destroy the Netlink object and free the memory */
        free_nlobj(nl_obj);

        /* Delete My SID */
        nl_obj = create_srv6_mysid_nlmsg(RTM_DELSRV6LOCALSID, &_mysid, 32, 16, 16, 0, SRV6_LOCALSID_ACTION_END);
        if (!nl_obj)
            throw std::runtime_error("SRv6 My SID creation failed");

        /* Send the Netlink object to the FpmLink */
        ASSERT_EQ(m_fpmLink->isRawProcessing(&nl_obj->n), true);
        m_fpmLink->processRawMsg(&nl_obj->n);

        /* Check that fpmsyncd removed the entry from the APP_DB */
        ASSERT_EQ(m_srv6MySidTable->hget(my_sid_table_key, "action", action), false);

        /* Destroy the Netlink object and free the memory */
        free_nlobj(nl_obj);
    }

    /* Test Receiving a route containing an invalid SRv6 My SID */
    TEST_F(FpmSyncdSRv6MySIDsTest, RecevingRouteWithSRv6MySIDInvalid)
    {
        ASSERT_NE(m_routeSync, nullptr);

        struct nlmsg *nl_obj;
        IpAddress _mysid = IpAddress("fc00:0:1:1::");
        int8_t _block_len;
        int8_t _node_len;
        int8_t _func_len;
        int8_t _arg_len;
        uint32_t _action = SRV6_LOCALSID_ACTION_UN;
        std::string action;
        string my_sid_table_key;

        /* Create a Netlink object containing an SRv6 My SID with missing block length */
        _block_len = -1;
        _node_len = 16;
        _func_len = 16;
        _arg_len = 0;
        my_sid_table_key = get_srv6_my_sid_table_key(&_mysid, _block_len, _node_len, _func_len, _arg_len);

        nl_obj = create_srv6_mysid_nlmsg(RTM_NEWSRV6LOCALSID, &_mysid, _block_len, _node_len, _func_len, _arg_len, _action);
        if (!nl_obj)
            throw std::runtime_error("SRv6 My SID creation failed");

        /* Send the Netlink object to the FpmLink */
        m_fpmLink->processRawMsg(&nl_obj->n);

        /* Ensure that fpmsyncd does not create an entry in APP_DB (because my SID is invalid)*/
        ASSERT_EQ(m_srv6MySidTable->hget(my_sid_table_key, "action", action), false);

        /* Destroy the Netlink object and free the memory */
        free_nlobj(nl_obj);


        /* Create a Netlink object containing an SRv6 My SID with missing node length */
        _block_len = 32;
        _node_len = -1;
        _func_len = 16;
        _arg_len = 0;
        my_sid_table_key = get_srv6_my_sid_table_key(&_mysid, _block_len, _node_len, _func_len, _arg_len);

        nl_obj = create_srv6_mysid_nlmsg(RTM_NEWSRV6LOCALSID, &_mysid, _block_len, _node_len, _func_len, _arg_len, _action);
        if (!nl_obj)
            throw std::runtime_error("SRv6 My SID creation failed");

        /* Send the Netlink object to the FpmLink */
        m_fpmLink->processRawMsg(&nl_obj->n);

        /* Ensure that fpmsyncd does not create an entry in APP_DB (because my SID is invalid)*/
        ASSERT_EQ(m_srv6MySidTable->hget(my_sid_table_key, "action", action), false);

        /* Destroy the Netlink object and free the memory */
        free_nlobj(nl_obj);


        /* Create a Netlink object containing an SRv6 My SID with missing function length */
        _block_len = 32;
        _node_len = 16;
        _func_len = -1;
        _arg_len = 0;
        my_sid_table_key = get_srv6_my_sid_table_key(&_mysid, _block_len, _node_len, _func_len, _arg_len);

        nl_obj = create_srv6_mysid_nlmsg(RTM_NEWSRV6LOCALSID, &_mysid, _block_len, _node_len, _func_len, _arg_len, _action);
        if (!nl_obj)
            throw std::runtime_error("SRv6 My SID creation failed");

        /* Send the Netlink object to the FpmLink */
        m_fpmLink->processRawMsg(&nl_obj->n);

        /* Ensure that fpmsyncd does not create an entry in APP_DB (because my SID is invalid)*/
        ASSERT_EQ(m_srv6MySidTable->hget(my_sid_table_key, "action", action), false);

        /* Destroy the Netlink object and free the memory */
        free_nlobj(nl_obj);


        /* Create a Netlink object containing an SRv6 My SID with missing argument length */
        _block_len = 32;
        _node_len = 16;
        _func_len = 16;
        _arg_len = -1;
        my_sid_table_key = get_srv6_my_sid_table_key(&_mysid, _block_len, _node_len, _func_len, 0);

        nl_obj = create_srv6_mysid_nlmsg(RTM_NEWSRV6LOCALSID, &_mysid, _block_len, _node_len, _func_len, _arg_len, _action);
        if (!nl_obj)
            throw std::runtime_error("SRv6 My SID creation failed");

        /* Send the Netlink object to the FpmLink */
        m_fpmLink->processRawMsg(&nl_obj->n);

        /* Check that fpmsyncd created the correct entries in APP_DB (with default argument length)*/
        ASSERT_EQ(m_srv6MySidTable->hget(my_sid_table_key, "action", action), true);

        /* Destroy the Netlink object and free the memory */
        free_nlobj(nl_obj);

    }

    /* Test Receiving a route containing an invalid SRv6 My SID with missing SID value */
    TEST_F(FpmSyncdSRv6MySIDsTest, SRv6MySidInvalidMissingSidValue)
    {
        ASSERT_NE(m_routeSync, nullptr);

        struct nlmsg *nl_obj;
        std::string action;

        /* Create a Netlink object containing an SRv6 My SID with missing SID value */
        nl_obj = create_srv6_mysid_nlmsg(RTM_NEWSRV6LOCALSID, NULL, 32, 16, 16, 0, SRV6_LOCALSID_ACTION_END);

        /* Send the Netlink object to the FpmLink */
        ASSERT_EQ(m_fpmLink->isRawProcessing(&nl_obj->n), true);
        m_fpmLink->processRawMsg(&nl_obj->n);

        /* Check that fpmsyncd did not create any entry in APP_DB */
        ASSERT_EQ(m_srv6MySidTable->hget("32:16:16:0:", "action", action), false);

        /* Destroy the Netlink object and free the memory */
        free_nlobj(nl_obj);
    }

    /* Test Receiving a route containing an invalid SRv6 My SID with IPv4 address as the SID value */
    TEST_F(FpmSyncdSRv6MySIDsTest, SRv6MySidInvalidIpv4SidValue)
    {
        ASSERT_NE(m_routeSync, nullptr);

        struct nlmsg *nl_obj;
        IpAddress _mysid = IpAddress("10.0.0.1");
        int8_t _block_len = 32;
        int8_t _node_len = 16;
        int8_t _func_len = 16;
        int8_t _arg_len = 0;
        std::string action;
        string my_sid_table_key = get_srv6_my_sid_table_key(&_mysid, _block_len, _node_len, _func_len, _arg_len);

        /* Create a Netlink object containing an SRv6 My SID with IPv4 SID value */
        nl_obj = create_srv6_mysid_nlmsg(RTM_NEWSRV6LOCALSID, &_mysid, 32, 16, 16, 0, SRV6_LOCALSID_ACTION_END, NULL, NULL, NULL, 10, 0, AF_INET);

        /* Send the Netlink object to the FpmLink */
        ASSERT_EQ(m_fpmLink->isRawProcessing(&nl_obj->n), true);
        m_fpmLink->processRawMsg(&nl_obj->n);

        /* Check that fpmsyncd did not create any entry in APP_DB */
        ASSERT_EQ(m_srv6MySidTable->hget(my_sid_table_key, "action", action), false);

        /* Destroy the Netlink object and free the memory */
        free_nlobj(nl_obj);
    }

    /* Test Receiving a route containing an invalid SRv6 My SID with invalid SID value prefix length */
    TEST_F(FpmSyncdSRv6MySIDsTest, SRv6MySidInvalidSidPrefixlen)
    {
        ASSERT_NE(m_routeSync, nullptr);

        struct nlmsg *nl_obj;
        IpAddress _mysid = IpAddress("fc00:0:1:e000::");
        int8_t _block_len = 32;
        int8_t _node_len = 16;
        int8_t _func_len = 16;
        int8_t _arg_len = 0;
        std::string action;
        string my_sid_table_key = get_srv6_my_sid_table_key(&_mysid, _block_len, _node_len, _func_len, _arg_len);

        /* Create a Netlink object containing an SRv6 My SID with invalid SID value prefix length */
        nl_obj = create_srv6_mysid_nlmsg(RTM_NEWSRV6LOCALSID, &_mysid, 32, 16, 16, 0, SRV6_LOCALSID_ACTION_END, NULL, NULL, NULL, 10, 200, AF_INET6);

        /* Send the Netlink object to the FpmLink */
        ASSERT_EQ(m_fpmLink->isRawProcessing(&nl_obj->n), true);
        m_fpmLink->processRawMsg(&nl_obj->n);

        /* Check that fpmsyncd did not create any entry in APP_DB */
        ASSERT_EQ(m_srv6MySidTable->hget(my_sid_table_key, "action", action), false);

        /* Destroy the Netlink object and free the memory */
        free_nlobj(nl_obj);
    }

    /* Test Receiving a route containing an SRv6 My SID with invalid action */
    TEST_F(FpmSyncdSRv6MySIDsTest, SRv6MySidInvalidAction)
    {
        ASSERT_NE(m_routeSync, nullptr);

        struct nlmsg *nl_obj;
        IpAddress _mysid = IpAddress("fc00:0:1:e000::");
        int8_t _block_len = 32;
        int8_t _node_len = 16;
        int8_t _func_len = 16;
        int8_t _arg_len = 0;
        std::string action;
        string my_sid_table_key = get_srv6_my_sid_table_key(&_mysid, _block_len, _node_len, _func_len, _arg_len);

        /* Create a Netlink object containing an SRv6 My SID with invalid action */
        nl_obj = create_srv6_mysid_nlmsg(RTM_NEWSRV6LOCALSID, &_mysid, 32, 16, 16, 0, 329);

        /* Send the Netlink object to the FpmLink */
        ASSERT_EQ(m_fpmLink->isRawProcessing(&nl_obj->n), true);
        m_fpmLink->processRawMsg(&nl_obj->n);

        /* Check that fpmsyncd did not create any entry in APP_DB */
        ASSERT_EQ(m_srv6MySidTable->hget(my_sid_table_key, "action", action), false);

        /* Destroy the Netlink object and free the memory */
        free_nlobj(nl_obj);
    }

    /* Test Receiving a route containing an SRv6 My SID with unspec action */
    TEST_F(FpmSyncdSRv6MySIDsTest, SRv6MySidInvalidUnspecAction)
    {
        ASSERT_NE(m_routeSync, nullptr);

        struct nlmsg *nl_obj;
        IpAddress _mysid = IpAddress("fc00:0:1:e000::");
        int8_t _block_len = 32;
        int8_t _node_len = 16;
        int8_t _func_len = 16;
        int8_t _arg_len = 0;
        std::string action;
        string my_sid_table_key = get_srv6_my_sid_table_key(&_mysid, _block_len, _node_len, _func_len, _arg_len);

        /* Create a Netlink object containing an SRv6 My SID with unspec action */
        nl_obj = create_srv6_mysid_nlmsg(RTM_NEWSRV6LOCALSID, &_mysid, 32, 16, 16, 0, SRV6_LOCALSID_ACTION_UNSPEC);

        /* Send the Netlink object to the FpmLink */
        ASSERT_EQ(m_fpmLink->isRawProcessing(&nl_obj->n), true);
        m_fpmLink->processRawMsg(&nl_obj->n);

        /* Check that fpmsyncd did not create any entry in APP_DB */
        ASSERT_EQ(m_srv6MySidTable->hget(my_sid_table_key, "action", action), false);

        /* Destroy the Netlink object and free the memory */
        free_nlobj(nl_obj);
    }

    /* Test Receiving a route containing an SRv6 My SID bound to End.DT6 behavior with empty VRF */
    TEST_F(FpmSyncdSRv6MySIDsTest, SRv6MySidInvalidVrf)
    {
        ASSERT_NE(m_routeSync, nullptr);

        struct nlmsg *nl_obj;
        IpAddress _mysid = IpAddress("fc00:0:1:e000::");
        int8_t _block_len = 32;
        int8_t _node_len = 16;
        int8_t _func_len = 16;
        int8_t _arg_len = 0;
        std::string action;
        string my_sid_table_key = get_srv6_my_sid_table_key(&_mysid, _block_len, _node_len, _func_len, _arg_len);

        /* Create a Netlink object containing an SRv6 My SID bound to End.DT6 behavior with empty VRF */
        nl_obj = create_srv6_mysid_nlmsg(RTM_NEWSRV6LOCALSID, &_mysid, 32, 16, 16, 0, SRV6_LOCALSID_ACTION_END_DT6, NULL);

        /* Send the Netlink object to the FpmLink */
        ASSERT_EQ(m_fpmLink->isRawProcessing(&nl_obj->n), true);
        m_fpmLink->processRawMsg(&nl_obj->n);

        /* Check that fpmsyncd did not create any entry in APP_DB */
        ASSERT_EQ(m_srv6MySidTable->hget(my_sid_table_key, "action", action), false);

        /* Destroy the Netlink object and free the memory */
        free_nlobj(nl_obj);
    }

    /* Test Receiving a route containing an invalid SRv6 My SID bound to End.X behavior with empty adjacency */
    TEST_F(FpmSyncdSRv6MySIDsTest, SRv6MySidInvalidAdjacency)
    {
        ASSERT_NE(m_routeSync, nullptr);

        struct nlmsg *nl_obj;
        IpAddress _mysid = IpAddress("fc00:0:1:e000::");
        int8_t _block_len = 32;
        int8_t _node_len = 16;
        int8_t _func_len = 16;
        int8_t _arg_len = 0;
        std::string action;
        string my_sid_table_key = get_srv6_my_sid_table_key(&_mysid, _block_len, _node_len, _func_len, _arg_len);

        /* Create a Netlink object containing an SRv6 My SID bound to End.X behavior with empty adjacency */
        nl_obj = create_srv6_mysid_nlmsg(RTM_NEWSRV6LOCALSID, &_mysid, 32, 16, 16, 0, SRV6_LOCALSID_ACTION_END_X, NULL, NULL);

        /* Send the Netlink object to the FpmLink */
        ASSERT_EQ(m_fpmLink->isRawProcessing(&nl_obj->n), true);
        m_fpmLink->processRawMsg(&nl_obj->n);

        /* Check that fpmsyncd did not create any entry in APP_DB */
        ASSERT_EQ(m_srv6MySidTable->hget(my_sid_table_key, "action", action), false);

        /* Destroy the Netlink object and free the memory */
        free_nlobj(nl_obj);
    }

    /* Test Receiving a route containing an SRv6 My SID with missing block length */
    TEST_F(FpmSyncdSRv6MySIDsTest, SRv6MySidInvalidMissingBlockLen)
    {
        ASSERT_NE(m_routeSync, nullptr);

        struct nlmsg *nl_obj;
        IpAddress _mysid = IpAddress("fc00:0:1:e000::");
        int8_t _block_len = 32;
        int8_t _node_len = 16;
        int8_t _func_len = 16;
        int8_t _arg_len = 0;
        std::string action;
        string my_sid_table_key = get_srv6_my_sid_table_key(&_mysid, _block_len, _node_len, _func_len, _arg_len);

        /* Create a Netlink object containing an SRv6 My SID with missing block length */
        nl_obj = create_srv6_mysid_nlmsg(RTM_NEWSRV6LOCALSID, &_mysid, -1, 16, 16, 0, SRV6_LOCALSID_ACTION_END, NULL, NULL, NULL, 10, AF_INET6, 200);

        /* Send the Netlink object to the FpmLink */
        ASSERT_EQ(m_fpmLink->isRawProcessing(&nl_obj->n), true);
        m_fpmLink->processRawMsg(&nl_obj->n);

        /* Check that fpmsyncd did not create any entry in APP_DB */
        ASSERT_EQ(m_srv6MySidTable->hget(my_sid_table_key, "action", action), false);

        /* Destroy the Netlink object and free the memory */
        free_nlobj(nl_obj);
    }

    /* Test Receiving a route containing an SRv6 My SID with missing node length  */
    TEST_F(FpmSyncdSRv6MySIDsTest, SRv6MySidInvalidMissingNodeLen)
    {
        ASSERT_NE(m_routeSync, nullptr);

        struct nlmsg *nl_obj;
        IpAddress _mysid = IpAddress("fc00:0:1:e000::");
        int8_t _block_len = 32;
        int8_t _node_len = 16;
        int8_t _func_len = 16;
        int8_t _arg_len = 0;
        std::string action;
        string my_sid_table_key = get_srv6_my_sid_table_key(&_mysid, _block_len, _node_len, _func_len, _arg_len);

        /* Create a Netlink object containing an SRv6 My SID with missing node length */
        nl_obj = create_srv6_mysid_nlmsg(RTM_NEWSRV6LOCALSID, &_mysid, 32, -1, 16, 0, SRV6_LOCALSID_ACTION_END, NULL, NULL, NULL, 10, AF_INET6, 200);

        /* Send the Netlink object to the FpmLink */
        ASSERT_EQ(m_fpmLink->isRawProcessing(&nl_obj->n), true);
        m_fpmLink->processRawMsg(&nl_obj->n);

        /* Check that fpmsyncd did not create any entry in APP_DB */
        ASSERT_EQ(m_srv6MySidTable->hget(my_sid_table_key, "action", action), false);

        /* Destroy the Netlink object and free the memory */
        free_nlobj(nl_obj);
    }

    /* Test Receiving a route containing an SRv6 My SID with missing function length */
    TEST_F(FpmSyncdSRv6MySIDsTest, SRv6MySidMissingFunctionLen)
    {
        ASSERT_NE(m_routeSync, nullptr);

        struct nlmsg *nl_obj;
        IpAddress _mysid = IpAddress("fc00:0:1:e000::");
        int8_t _block_len = 32;
        int8_t _node_len = 16;
        int8_t _func_len = 16;
        int8_t _arg_len = 0;
        std::string action;
        string my_sid_table_key = get_srv6_my_sid_table_key(&_mysid, _block_len, _node_len, _func_len, _arg_len);

        /* Create a Netlink object containing an SRv6 My SID with missing node length */
        nl_obj = create_srv6_mysid_nlmsg(RTM_NEWSRV6LOCALSID, &_mysid, 32, 16, -1, 0, SRV6_LOCALSID_ACTION_END, NULL, NULL, NULL, 10, AF_INET6, 200);

        /* Send the Netlink object to the FpmLink */
        ASSERT_EQ(m_fpmLink->isRawProcessing(&nl_obj->n), true);
        m_fpmLink->processRawMsg(&nl_obj->n);

        /* Check that fpmsyncd did not create any entry in APP_DB */
        ASSERT_EQ(m_srv6MySidTable->hget(my_sid_table_key, "action", action), false);

        /* Destroy the Netlink object and free the memory */
        free_nlobj(nl_obj);
    }

    /* Test Receiving a route containing an SRv6 My SID with missing argument length */
    TEST_F(FpmSyncdSRv6MySIDsTest, SRv6MySidMissingArgumentLen)
    {
        ASSERT_NE(m_routeSync, nullptr);

        struct nlmsg *nl_obj;
        IpAddress _mysid = IpAddress("fc00:0:1:e000::");
        int8_t _block_len = 32;
        int8_t _node_len = 16;
        int8_t _func_len = 16;
        int8_t _arg_len = 0;
        std::string action;
        string my_sid_table_key = get_srv6_my_sid_table_key(&_mysid, _block_len, _node_len, _func_len, _arg_len);

        /* Create a Netlink object containing an SRv6 My SID with missing node length */
        nl_obj = create_srv6_mysid_nlmsg(RTM_NEWSRV6LOCALSID, &_mysid, 32, 16, 16, -1, SRV6_LOCALSID_ACTION_END);

        /* Send the Netlink object to the FpmLink */
        ASSERT_EQ(m_fpmLink->isRawProcessing(&nl_obj->n), true);
        m_fpmLink->processRawMsg(&nl_obj->n);

        /* Check that fpmsyncd did not create any entry in APP_DB */
        ASSERT_EQ(m_srv6MySidTable->hget(my_sid_table_key, "action", action), true);
        ASSERT_EQ(action, "end");

        /* Destroy the Netlink object and free the memory */
        free_nlobj(nl_obj);

        /* Delete My SID */
        nl_obj = create_srv6_mysid_nlmsg(RTM_DELSRV6LOCALSID, &_mysid, 32, 16, 16, 0, SRV6_LOCALSID_ACTION_END);
        if (!nl_obj)
            throw std::runtime_error("SRv6 My SID creation failed");

        /* Send the Netlink object to the FpmLink */
        ASSERT_EQ(m_fpmLink->isRawProcessing(&nl_obj->n), true);
        m_fpmLink->processRawMsg(&nl_obj->n);

        /* Check that fpmsyncd removed the entry from the APP_DB */
        ASSERT_EQ(m_srv6MySidTable->hget(my_sid_table_key, "action", action), false);

        /* Destroy the Netlink object and free the memory */
        free_nlobj(nl_obj);
    }
}