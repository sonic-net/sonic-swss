#include "redisutility.h"
#include "ut_helpers_fpmsyncd.h"
#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include "mock_table.h"
#define private public
#include "fpmsyncd/routesync.h"
#undef private

#include <arpa/inet.h>
#include <linux/rtnetlink.h>
#include <netlink/route/link.h>
#include <netlink/route/nexthop.h>
#include <linux/nexthop.h>

#include <sstream>

using namespace swss;
using namespace testing;

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
    MOCK_METHOD(bool, getIfName, (int, char *, size_t), (override));
    MOCK_METHOD(void, onNextHopMsg, (struct nlmsghdr *, int), (override));
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

struct nlmsghdr* createNewNextHopMsgHdr(int32_t ifindex, const char* gateway, uint32_t id) {

    struct nlmsghdr *nlh = (struct nlmsghdr *)malloc(NLMSG_SPACE(MAX_PAYLOAD));
    memset(nlh, 0, NLMSG_SPACE(MAX_PAYLOAD));

    nlh->nlmsg_type = RTM_NEWNEXTHOP;
    nlh->nlmsg_flags = NLM_F_REQUEST | NLM_F_CREATE | NLM_F_REPLACE;

    // calculate the initial length and align the nlmsg
    nlh->nlmsg_len = NLMSG_LENGTH(sizeof(struct nhmsg));

    // set nhmsg
    struct nhmsg *nhm = (struct nhmsg *)NLMSG_DATA(nlh);
    nhm->nh_family = AF_INET;

    // set NHA_ID
    ut_fpmsyncd::nl_attr_put32(nlh, NLMSG_SPACE(MAX_PAYLOAD), NHA_ID, id);

    // set NHA_GATEWAY
    struct rtattr *rta = reinterpret_cast<struct rtattr *>(static_cast<void *>(((char *)nlh) + NLMSG_ALIGN(nlh->nlmsg_len)));
    struct in_addr gw_addr;
    inet_pton(AF_INET, gateway, &gw_addr);
    rta->rta_type = NHA_GATEWAY;
    rta->rta_len = RTA_LENGTH(sizeof(struct in_addr));
    memcpy(RTA_DATA(rta), &gw_addr, sizeof(struct in_addr));
    nlh->nlmsg_len = NLMSG_ALIGN(nlh->nlmsg_len) + RTA_ALIGN(rta->rta_len);

    // set NHA_OIF attribute
    ut_fpmsyncd::nl_attr_put32(nlh, NLMSG_SPACE(MAX_PAYLOAD), NHA_OIF, ifindex);
    return nlh;
}

TEST_F(FpmSyncdResponseTest, TestNoNHAId)
{
    struct nlmsghdr *nlh = (struct nlmsghdr *)malloc(NLMSG_SPACE(MAX_PAYLOAD));
    memset(nlh, 0, NLMSG_SPACE(MAX_PAYLOAD));

    nlh->nlmsg_type = RTM_NEWNEXTHOP;
    nlh->nlmsg_flags = NLM_F_REQUEST | NLM_F_CREATE | NLM_F_REPLACE;
    nlh->nlmsg_len = NLMSG_LENGTH(sizeof(struct nhmsg));
    struct nhmsg *nhm = (struct nhmsg *)NLMSG_DATA(nlh);
    nhm->nh_family = AF_INET;

    EXPECT_CALL(m_mockRouteSync, getIfName(_, _, _))
    .Times(0);

    m_mockRouteSync.RouteSync::onNextHopMsg(nlh, 0);

    free(nlh);
}

// TEST_F(FpmSyncdResponseTest, TestSingleNextHopAdd)
// {
//     uint32_t test_id = 10;
//     const char* test_gateway = "192.168.1.1";
//     int32_t test_ifindex = 5;

//     struct nlmsghdr* nlh = createNewNextHopMsgHdr(test_ifindex, test_gateway, test_id);
//     int expected_length = (int)(nlh->nlmsg_len - NLMSG_LENGTH(sizeof(struct nhmsg)));

//     EXPECT_CALL(m_mockRouteSync, getIfName(test_ifindex, _, _))
//     .WillOnce(DoAll(
//         [](int32_t, char* ifname, size_t size) {
//             strncpy(ifname, "Ethernet1", size);
//             ifname[size-1] = '\0';
//         },
//         Return(true)
//     ));

//     m_mockRouteSync.RouteSync::onNextHopMsg(nlh, expected_length);

//     auto it = m_mockRouteSync.m_nh_groups.find(test_id);
//     ASSERT_NE(it, m_mockRouteSync.m_nh_groups.end()) << "Failed to add new nexthop";

//     free(nlh);
// }

TEST_F(FpmSyncdResponseTest, TestSkipSpecialInterfaces)
{
    uint32_t test_id = 11;
    const char* test_gateway = "192.168.1.1";
    int32_t test_ifindex = 6;

    EXPECT_CALL(m_mockRouteSync, getIfName(test_ifindex, _, _))
    .WillOnce(DoAll(
        [](int32_t ifidx, char* ifname, size_t size) {
            std::cout << "getIfName called with ifindex: " << ifidx << ", size: " << size << std::endl;
            strncpy(ifname, "eth0", size);
            std::cout << "ifname set to: " << ifname << std::endl;
        },
        Return(true)
    ));

    std::cout << "create nexthopmsg" << std::endl;
    struct nlmsghdr* nlh = createNewNextHopMsgHdr(test_ifindex, test_gateway, test_id);
    int expected_length = (int)(nlh->nlmsg_len - NLMSG_LENGTH(sizeof(struct nhmsg)));

    std::cout << "nlh pointer: " << nlh << std::endl;
    std::cout << "nlmsg_len: " << nlh->nlmsg_len << std::endl;
    std::cout << "nlmsg_type: " << nlh->nlmsg_type << std::endl;
    std::cout << "expected_length: " << expected_length << std::endl;

    m_mockRouteSync.RouteSync::onNextHopMsg(nlh, expected_length);

    auto it = m_mockRouteSync.m_nh_groups.find(test_id);
    EXPECT_EQ(it, m_mockRouteSync.m_nh_groups.end()) << "Should skip eth0 interface";
    
    free(nlh);
}

TEST_F(FpmSyncdResponseTest, testNewNextHopMsg)
{
    uint32_t test_id = 10;
    const char* test_gateway = "192.168.1.1";
    int32_t test_ifindex = 5;

    struct nlmsghdr* nlh = createNewNextHopMsgHdr(test_ifindex, test_gateway, test_id);

    int expected_length = (int)(nlh->nlmsg_len - NLMSG_LENGTH(sizeof(struct nhmsg)));

    EXPECT_CALL(m_mockRouteSync, onNextHopMsg(nlh, expected_length))
        .Times(1)
        .WillOnce([&](struct nlmsghdr *h, int actuallen)-> void {
            std::cout << "onNextHopMsg is invoked!" << std::endl;
            EXPECT_EQ(actuallen, expected_length);
            return;
        });

    m_mockRouteSync.onMsgRaw(nlh);
    free(nlh);
}