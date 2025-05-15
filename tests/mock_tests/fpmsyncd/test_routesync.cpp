#include "redisutility.h"

#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include "mock_table.h"
#define private public
#include "fpmsyncd/routesync.h"
#undef private

using namespace swss;
#define MAX_PAYLOAD 1024

using ::testing::_;

class MockRouteSync : public RouteSync
{
public:
    MockRouteSync(RedisPipeline *m_pipeline) : RouteSync(m_pipeline)
    {
    }

    ~MockRouteSync()
    {
    }
    MOCK_METHOD(bool, getEvpnNextHop, (nlmsghdr *, int,
                               rtattr *[], std::string&,
                               std::string& , std::string&,
                               std::string&), (override));
};
class MockFpm : public FpmInterface
{
public:
    MockFpm(RouteSync* routeSync) :
        m_routeSync(routeSync)
    {
        m_routeSync->onFpmConnected(*this);
    }

    ~MockFpm() override
    {
        m_routeSync->onFpmDisconnected();
    }

    MOCK_METHOD1(send, bool(nlmsghdr*));
    MOCK_METHOD0(getFd, int());
    MOCK_METHOD0(readData, uint64_t());

private:
    RouteSync* m_routeSync{};
};

class FpmSyncdResponseTest : public ::testing::Test
{
public:
    void SetUp() override
    {
        EXPECT_EQ(rtnl_route_read_protocol_names(DefaultRtProtoPath), 0);
        m_routeSync.setSuppressionEnabled(true);
    }

    void TearDown() override
    {
    }

    shared_ptr<swss::DBConnector> m_db = make_shared<swss::DBConnector>("APPL_DB", 0);
    shared_ptr<RedisPipeline> m_pipeline = make_shared<RedisPipeline>(m_db.get());
    RouteSync m_routeSync{m_pipeline.get()};
    MockFpm m_mockFpm{&m_routeSync};
    MockRouteSync m_mockRouteSync{m_pipeline.get()};
};

TEST_F(FpmSyncdResponseTest, RouteResponseFeedbackV4)
{
    // Expect the message to zebra is sent
    EXPECT_CALL(m_mockFpm, send(_)).WillOnce([&](nlmsghdr* hdr) -> bool {
        rtnl_route* routeObject{};

        rtnl_route_parse(hdr, &routeObject);

        // table is 0 when no in default VRF
        EXPECT_EQ(rtnl_route_get_table(routeObject), 0);
        EXPECT_EQ(rtnl_route_get_protocol(routeObject), RTPROT_KERNEL);

        // Offload flag is set
        EXPECT_EQ(rtnl_route_get_flags(routeObject) & RTM_F_OFFLOAD, RTM_F_OFFLOAD);

        return true;
    });

    m_routeSync.onRouteResponse("1.0.0.0/24", {
        {"err_str", "SWSS_RC_SUCCESS"},
        {"protocol", "kernel"},
    });
}

TEST_F(FpmSyncdResponseTest, RouteResponseFeedbackV4Vrf)
{
    // Expect the message to zebra is sent
    EXPECT_CALL(m_mockFpm, send(_)).WillOnce([&](nlmsghdr* hdr) -> bool {
        rtnl_route* routeObject{};

        rtnl_route_parse(hdr, &routeObject);

        // table is 42 (returned by fake link cache) when in non default VRF
        EXPECT_EQ(rtnl_route_get_table(routeObject), 42);
        EXPECT_EQ(rtnl_route_get_protocol(routeObject), 200);

        // Offload flag is set
        EXPECT_EQ(rtnl_route_get_flags(routeObject) & RTM_F_OFFLOAD, RTM_F_OFFLOAD);

        return true;
    });

    m_routeSync.onRouteResponse("Vrf0:1.0.0.0/24", {
        {"err_str", "SWSS_RC_SUCCESS"},
        {"protocol", "200"},
    });
}

TEST_F(FpmSyncdResponseTest, RouteResponseFeedbackV6)
{
    // Expect the message to zebra is sent
    EXPECT_CALL(m_mockFpm, send(_)).WillOnce([&](nlmsghdr* hdr) -> bool {
        rtnl_route* routeObject{};

        rtnl_route_parse(hdr, &routeObject);

        // table is 0 when no in default VRF
        EXPECT_EQ(rtnl_route_get_table(routeObject), 0);
        EXPECT_EQ(rtnl_route_get_protocol(routeObject), RTPROT_KERNEL);

        // Offload flag is set
        EXPECT_EQ(rtnl_route_get_flags(routeObject) & RTM_F_OFFLOAD, RTM_F_OFFLOAD);

        return true;
    });

    m_routeSync.onRouteResponse("1::/64", {
        {"err_str", "SWSS_RC_SUCCESS"},
        {"protocol", "kernel"},
    });
}

TEST_F(FpmSyncdResponseTest, RouteResponseFeedbackV6Vrf)
{
    // Expect the message to zebra is sent
    EXPECT_CALL(m_mockFpm, send(_)).WillOnce([&](nlmsghdr* hdr) -> bool {
        rtnl_route* routeObject{};

        rtnl_route_parse(hdr, &routeObject);

        // table is 42 (returned by fake link cache) when in non default VRF
        EXPECT_EQ(rtnl_route_get_table(routeObject), 42);
        EXPECT_EQ(rtnl_route_get_protocol(routeObject), 200);

        // Offload flag is set
        EXPECT_EQ(rtnl_route_get_flags(routeObject) & RTM_F_OFFLOAD, RTM_F_OFFLOAD);

        return true;
    });

    m_routeSync.onRouteResponse("Vrf0:1::/64", {
        {"err_str", "SWSS_RC_SUCCESS"},
        {"protocol", "200"},
    });
}

TEST_F(FpmSyncdResponseTest, WarmRestart)
{
    std::vector<FieldValueTuple> fieldValues = {
        {"protocol", "kernel"},
    };

    DBConnector applStateDb{"APPL_STATE_DB", 0};
    Table routeStateTable{&applStateDb, APP_ROUTE_TABLE_NAME};

    routeStateTable.set("1.0.0.0/24", fieldValues);
    routeStateTable.set("2.0.0.0/24", fieldValues);
    routeStateTable.set("Vrf0:3.0.0.0/24", fieldValues);

    EXPECT_CALL(m_mockFpm, send(_)).Times(3).WillRepeatedly([&](nlmsghdr* hdr) -> bool {
        rtnl_route* routeObject{};

        rtnl_route_parse(hdr, &routeObject);

        // Offload flag is set
        EXPECT_EQ(rtnl_route_get_flags(routeObject) & RTM_F_OFFLOAD, RTM_F_OFFLOAD);

        return true;
    });

    m_routeSync.onWarmStartEnd(applStateDb);
}

TEST_F(FpmSyncdResponseTest, testEvpn)
{
    struct nlmsghdr *nlh = (struct nlmsghdr *) malloc(NLMSG_SPACE(MAX_PAYLOAD));
    shared_ptr<swss::DBConnector> m_app_db;
    m_app_db = make_shared<swss::DBConnector>("APPL_DB", 0);
    Table app_route_table(m_app_db.get(), APP_ROUTE_TABLE_NAME);

    memset(nlh, 0, NLMSG_SPACE(MAX_PAYLOAD));
    nlh->nlmsg_type = RTM_NEWROUTE;
    struct rtmsg rtm;
    rtm.rtm_family = AF_INET;
    rtm.rtm_protocol = 200;
    rtm.rtm_type = RTN_UNICAST;
    rtm.rtm_table = 0;
    rtm.rtm_dst_len = 32;
    nlh->nlmsg_len = NLMSG_SPACE(MAX_PAYLOAD);
    memcpy(NLMSG_DATA(nlh), &rtm, sizeof(rtm));

    EXPECT_CALL(m_mockRouteSync, getEvpnNextHop(_, _, _, _, _, _, _)).Times(testing::AtLeast(1)).WillOnce([&](
                               struct nlmsghdr *h, int received_bytes,
                               struct rtattr *tb[], std::string& nexthops,
                               std::string& vni_list, std::string& mac_list,
                               std::string& intf_list)-> bool {
        vni_list="100";
        mac_list="aa:aa:aa:aa:aa:aa";
        intf_list="Ethernet0";
        nexthops = "1.1.1.1";
        return true;
    });
    m_mockRouteSync.onMsgRaw(nlh);
    vector<string> keys;
    vector<FieldValueTuple> fieldValues;
    app_route_table.getKeys(keys);
    ASSERT_EQ(keys.size(), 1);

    app_route_table.get(keys[0], fieldValues);
    auto value = swss::fvsGetValue(fieldValues, "protocol", true);
    ASSERT_EQ(value.get(), "0xc8");

}

auto create_nl_addr(const char* addr_str)
{
    nl_addr* addr;
    nl_addr_parse(addr_str, AF_INET, &addr);
    return unique_ptr<nl_addr, decltype(nl_addr_put)*>(addr, nl_addr_put);
}

auto create_route(const char* dst_addr_str)
{
    rtnl_route* route = rtnl_route_alloc();
    auto dst_addr = create_nl_addr(dst_addr_str);
    rtnl_route_set_dst(route, dst_addr.get());
    rtnl_route_set_type(route, RTN_UNICAST);
    rtnl_route_set_protocol(route, RTPROT_STATIC);
    rtnl_route_set_family(route, AF_INET);
    rtnl_route_set_scope(route, RT_SCOPE_UNIVERSE);
    rtnl_route_set_table(route, RT_TABLE_MAIN);
    return unique_ptr<rtnl_route, decltype(rtnl_route_put)*>(route, rtnl_route_put);
}

rtnl_nexthop* create_nexthop(const char* gateway_str)
{
    static int idx = 1; // interface index
    ++idx;
    // Create a nexthop with 0 weight
    rtnl_nexthop* nh = rtnl_route_nh_alloc();
    rtnl_route_nh_set_weight(nh, 0);
    rtnl_route_nh_set_ifindex(nh, idx);
    auto gateway_addr = create_nl_addr(gateway_str);
    rtnl_route_nh_set_gateway(nh, gateway_addr.get());
    return nh;
}

// Checks that when a nexthop is not assigned a weight, the default weight of 1 is used.
TEST_F(FpmSyncdResponseTest, TestGetNextHopWt)
{
    auto test_route = create_route("10.1.1.0");

    // Create two nexthops with 0 weight
    rtnl_nexthop* nh1 = create_nexthop("192.168.1.1");
    rtnl_nexthop* nh2 = create_nexthop("192.168.1.2");

    // Add new nexthops to the route
    rtnl_route_add_nexthop(test_route.get(), nh1);
    rtnl_route_add_nexthop(test_route.get(), nh2);

    EXPECT_EQ(m_mockRouteSync.getNextHopWt(test_route.get()), "1,1");
}
