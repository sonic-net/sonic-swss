#include "redisutility.h"
#include "ut_helpers_fpmsyncd.h"
#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include "mock_table.h"
#define private public
#include "fpmsyncd/routesync.h"
#include "fpmsyncd/fpmlink.h"
#undef private
#include "orch_zmq_config.h"

#include <arpa/inet.h>
#include <linux/rtnetlink.h>
#include <netlink/route/link.h>
#include <netlink/route/nexthop.h>
#include <linux/nexthop.h>
#include <linux/lwtunnel.h>
#include <linux/seg6_iptunnel.h>

#include <sstream>

using namespace swss;
using namespace testing;
using namespace ut_fpmsyncd;

#define MAX_PAYLOAD 1024

using ::testing::_;

extern void resetMockWarmStartHelper();

int rt_build_ret = 0;
bool nlmsg_alloc_ret = true;
#pragma GCC diagnostic ignored "-Wcast-align"

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
        testing_db::reset();
        EXPECT_EQ(rtnl_route_read_protocol_names(DefaultRtProtoPath), 0);
        m_routeSync.setSuppressionEnabled(true);
    }

    void TearDown() override
    {
        testing_db::reset();
    }

    shared_ptr<swss::DBConnector> m_db = make_shared<swss::DBConnector>("APPL_DB", 0);
    shared_ptr<RedisPipeline> m_pipeline = make_shared<RedisPipeline>(m_db.get());
    RouteSync m_routeSync{m_pipeline.get()};
    MockFpm m_mockFpm{&m_routeSync};
    MockRouteSync m_mockRouteSync{m_pipeline.get()};

    const char* test_gateway = "192.168.1.1";
    const char* test_gateway_ = "192.168.1.2";
    const char* test_gateway__ = "192.168.1.3";
};

class FpmSyncdResponseTestWithZmqNb : public FpmSyncdResponseTest {
    void SetUp() override
    {
        FpmSyncdResponseTest::SetUp();
        // Simulate ZMQ being enabled by setting m_zmqClient to a non-null value
        // We use a dummy shared_ptr (pointing to address 1) since we won't actually use it
        // This makes isNbZmqEnabled() return true
        m_mockRouteSync.m_zmqClient = shared_ptr<swss::ZmqClient>(reinterpret_cast<swss::ZmqClient*>(1), [](swss::ZmqClient*){});
    }

    void TearDown() override
    {
        // Reset m_zmqClient to nullptr
        m_mockRouteSync.m_zmqClient = nullptr;
        FpmSyncdResponseTest::TearDown();
    }
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

TEST_F(FpmSyncdResponseTest, testSendOffloadReply)
{
    rt_build_ret = 1;
    rtnl_route* routeObject{};

    ASSERT_EQ(m_routeSync.sendOffloadReply(routeObject), false);
    rt_build_ret = 0;
    nlmsg_alloc_ret = false;
    ASSERT_EQ(m_routeSync.sendOffloadReply(routeObject), false);
    nlmsg_alloc_ret = true;
}

struct nlmsghdr* createNewNextHopMsgHdr(int32_t ifindex, const char* gateway, uint32_t id, unsigned char nh_family=AF_INET) {
    struct nlmsghdr *nlh = (struct nlmsghdr *)malloc(NLMSG_SPACE(MAX_PAYLOAD));
    memset(nlh, 0, NLMSG_SPACE(MAX_PAYLOAD));

    // Set header
    nlh->nlmsg_type = RTM_NEWNEXTHOP;
    nlh->nlmsg_flags = NLM_F_REQUEST | NLM_F_CREATE | NLM_F_REPLACE;
    nlh->nlmsg_len = NLMSG_LENGTH(sizeof(struct nhmsg));

    // Set nhmsg
    struct nhmsg *nhm = (struct nhmsg *)NLMSG_DATA(nlh);
    nhm->nh_family = nh_family;

    // Add NHA_ID
    struct rtattr *rta = (struct rtattr *)((char *)nlh + NLMSG_ALIGN(nlh->nlmsg_len));
    rta->rta_type = NHA_ID;
    rta->rta_len = RTA_LENGTH(sizeof(uint32_t));
    *(uint32_t *)RTA_DATA(rta) = id;
    nlh->nlmsg_len = NLMSG_ALIGN(nlh->nlmsg_len) + RTA_ALIGN(rta->rta_len);

    // Add NHA_OIF
    rta = (struct rtattr *)((char *)nlh + NLMSG_ALIGN(nlh->nlmsg_len));
    rta->rta_type = NHA_OIF;
    rta->rta_len = RTA_LENGTH(sizeof(int32_t));
    *(int32_t *)RTA_DATA(rta) = ifindex;
    nlh->nlmsg_len = NLMSG_ALIGN(nlh->nlmsg_len) + RTA_ALIGN(rta->rta_len);

    // Add NHA_GATEWAY
    rta = (struct rtattr *)((char *)nlh + NLMSG_ALIGN(nlh->nlmsg_len));
    rta->rta_type = NHA_GATEWAY;
    if (nh_family == AF_INET6)
    {
        struct in6_addr gw_addr6;
        inet_pton(AF_INET6, gateway, &gw_addr6);
        rta->rta_len = RTA_LENGTH(sizeof(struct in6_addr));
        memcpy(RTA_DATA(rta), &gw_addr6, sizeof(struct in6_addr));
    }
    else
    {
        struct in_addr gw_addr;
        inet_pton(AF_INET, gateway, &gw_addr);
        rta->rta_len = RTA_LENGTH(sizeof(struct in_addr));
        memcpy(RTA_DATA(rta), &gw_addr, sizeof(struct in_addr));
    }
    nlh->nlmsg_len = NLMSG_ALIGN(nlh->nlmsg_len) + RTA_ALIGN(rta->rta_len);

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

    m_mockRouteSync.onNextHopMsg(nlh, 0);

    free(nlh);
}

TEST_F(FpmSyncdResponseTest, TestNextHopAdd)
{
    uint32_t test_id = 10;
    int32_t test_ifindex = 5;

    struct nlmsghdr* nlh = createNewNextHopMsgHdr(test_ifindex, test_gateway, test_id);
    int expected_length = (int)(nlh->nlmsg_len - NLMSG_LENGTH(sizeof(struct nhmsg)));

    EXPECT_CALL(m_mockRouteSync, getIfName(test_ifindex, _, _))
    .WillOnce(DoAll(
        [](int32_t, char* ifname, size_t size) {
            strncpy(ifname, "Ethernet1", size);
            ifname[size-1] = '\0';
        },
        Return(true)
    ));

    m_mockRouteSync.onNextHopMsg(nlh, expected_length);

    auto it = m_mockRouteSync.m_nh_groups.find(test_id);
    ASSERT_NE(it, m_mockRouteSync.m_nh_groups.end()) << "Failed to add new nexthop";

    free(nlh);
}

TEST_F(FpmSyncdResponseTest, TestIPv6NextHopAdd)
{
    uint32_t test_id = 20;
    const char* test_gateway = "2001:db8::1";
    int32_t test_ifindex = 7;

    struct nlmsghdr* nlh = createNewNextHopMsgHdr(test_ifindex, test_gateway, test_id, AF_INET6);
    int expected_length = (int)(nlh->nlmsg_len - NLMSG_LENGTH(sizeof(struct nhmsg)));

    EXPECT_CALL(m_mockRouteSync, getIfName(test_ifindex, _, _))
        .WillOnce(DoAll(
            [](int32_t, char* ifname, size_t size) {
                strncpy(ifname, "Ethernet2", size);
                ifname[size-1] = '\0';
            },
            Return(true)
        ));

    m_mockRouteSync.onNextHopMsg(nlh, expected_length);

    Table nexthop_group_table(m_db.get(), APP_NEXTHOP_GROUP_TABLE_NAME);

    vector<FieldValueTuple> fieldValues;
    string key = to_string(test_id);
    nexthop_group_table.get(key, fieldValues);

    // onNextHopMsg only updates m_nh_groups unless the nhg is marked as installed
    ASSERT_TRUE(fieldValues.empty());

    // Update the nexthop group to mark it as installed and write to DB
    m_mockRouteSync.installNextHopGroup(test_id);
    nexthop_group_table.get(key, fieldValues);

    string nexthop, ifname;
    for (const auto& fv : fieldValues) {
        if (fvField(fv) == "nexthop") {
            nexthop = fvValue(fv);
        } else if (fvField(fv) == "ifname") {
            ifname = fvValue(fv);
        }
    }

    EXPECT_EQ(nexthop, test_gateway);
    EXPECT_EQ(ifname, "Ethernet2");

    free(nlh);
}

TEST_F(FpmSyncdResponseTestWithZmqNb, TestIPv6NextHopAddZmqNb)
{
    uint32_t test_id = 20;
    const char* test_gateway = "2001:db8::1";
    int32_t test_ifindex = 7;

    struct nlmsghdr* nlh = createNewNextHopMsgHdr(test_ifindex, test_gateway, test_id, AF_INET6);
    int expected_length = (int)(nlh->nlmsg_len - NLMSG_LENGTH(sizeof(struct nhmsg)));

    EXPECT_CALL(m_mockRouteSync, getIfName(test_ifindex, _, _))
        .WillOnce(DoAll(
            [](int32_t, char* ifname, size_t size) {
                strncpy(ifname, "Ethernet2", size);
                ifname[size-1] = '\0';
            },
            Return(true)
        ));

    m_mockRouteSync.onNextHopMsg(nlh, expected_length);

    Table nexthop_group_table(m_db.get(), APP_NEXTHOP_GROUP_TABLE_NAME);

    vector<FieldValueTuple> fieldValues;
    string key = to_string(test_id);
    nexthop_group_table.get(key, fieldValues);

    // onNextHopMsg only updates m_nh_groups unless the nhg is marked as installed
    ASSERT_TRUE(fieldValues.empty());

    // Update the nexthop group to mark it as installed and write to DB
    m_mockRouteSync.installNextHopGroup(test_id);
    nexthop_group_table.get(key, fieldValues);

    string nexthop, ifname;
    for (const auto& fv : fieldValues) {
        if (fvField(fv) == "nexthop") {
            nexthop = fvValue(fv);
        } else if (fvField(fv) == "ifname") {
            ifname = fvValue(fv);
        }
    }

    EXPECT_EQ(nexthop, test_gateway);
    EXPECT_EQ(ifname, "Ethernet2");

    free(nlh);
}


TEST_F(FpmSyncdResponseTest, TestGetIfNameFailure)
{
    uint32_t test_id = 22;
    int32_t test_ifindex = 9;

    struct nlmsghdr* nlh = createNewNextHopMsgHdr(test_ifindex, test_gateway, test_id);
    int expected_length = (int)(nlh->nlmsg_len - NLMSG_LENGTH(sizeof(struct nhmsg)));

    EXPECT_CALL(m_mockRouteSync, getIfName(test_ifindex, _, _))
        .WillOnce(Return(false));

    m_mockRouteSync.onNextHopMsg(nlh, expected_length);

    auto it = m_mockRouteSync.m_nh_groups.find(test_id);
    ASSERT_NE(it, m_mockRouteSync.m_nh_groups.end());
    EXPECT_EQ(it->second.intf, "unknown");

    free(nlh);
}
TEST_F(FpmSyncdResponseTest, TestSkipSpecialInterfaces)
{
    uint32_t test_id = 11;
    int32_t test_ifindex = 6;

    EXPECT_CALL(m_mockRouteSync, getIfName(test_ifindex, _, _))
    .WillOnce(DoAll(
        [](int32_t ifidx, char* ifname, size_t size) {
            strncpy(ifname, "eth0", size);
        },
        Return(true)
    ));

    struct nlmsghdr* nlh = createNewNextHopMsgHdr(test_ifindex, test_gateway, test_id);
    int expected_length = (int)(nlh->nlmsg_len - NLMSG_LENGTH(sizeof(struct nhmsg)));

    m_mockRouteSync.onNextHopMsg(nlh, expected_length);

    auto it = m_mockRouteSync.m_nh_groups.find(test_id);
    EXPECT_EQ(it, m_mockRouteSync.m_nh_groups.end()) << "Should skip eth0 interface";

    free(nlh);
}

TEST_F(FpmSyncdResponseTest, TestNextHopGroupKeyString)
{
    EXPECT_EQ(m_mockRouteSync.getNextHopGroupKeyAsString(1), "1");
    EXPECT_EQ(m_mockRouteSync.getNextHopGroupKeyAsString(1234), "1234");
}

TEST_F(FpmSyncdResponseTest, TestGetNextHopGroupFields)
{
    // Test single next hop case
    {
        NextHopGroup nhg(1, test_gateway, "Ethernet0");
        m_mockRouteSync.m_nh_groups.insert({1, nhg});

        string nexthops, ifnames, weights;
        m_mockRouteSync.getNextHopGroupFields(nhg, nexthops, ifnames, weights);

        EXPECT_EQ(nexthops, test_gateway);
        EXPECT_EQ(ifnames, "Ethernet0");
        EXPECT_TRUE(weights.empty());
    }

    // Test multiple next hops with weights
    {
        // Create the component next hops first
        NextHopGroup nhg1(1, test_gateway, "Ethernet0");
        NextHopGroup nhg2(2, test_gateway_, "Ethernet1");
        m_mockRouteSync.m_nh_groups.insert({1, nhg1});
        m_mockRouteSync.m_nh_groups.insert({2, nhg2});

        // Create the group with multiple next hops
        vector<pair<uint32_t,uint8_t>> group_members;
        group_members.push_back(make_pair(1, 1));  // id=1, weight=1
        group_members.push_back(make_pair(2, 2));  // id=2, weight=2

        NextHopGroup nhg(3, group_members);
        m_mockRouteSync.m_nh_groups.insert({3, nhg});

        string nexthops, ifnames, weights;
        m_mockRouteSync.getNextHopGroupFields(nhg, nexthops, ifnames, weights);

        EXPECT_EQ(nexthops, "192.168.1.1,192.168.1.2");
        EXPECT_EQ(ifnames, "Ethernet0,Ethernet1");
        EXPECT_EQ(weights, "1,2");
    }

    // Test IPv6 default case
    {
        NextHopGroup nhg(4, "", "Ethernet0");
        m_mockRouteSync.m_nh_groups.insert({4, nhg});

        string nexthops, ifnames, weights;
        m_mockRouteSync.getNextHopGroupFields(nhg, nexthops, ifnames, weights, AF_INET6);

        EXPECT_EQ(nexthops, "::");
        EXPECT_EQ(ifnames, "Ethernet0");
        EXPECT_TRUE(weights.empty());
    }

     // Both empty
    {
        NextHopGroup nhg(5, "", "");
        string nexthops, ifnames, weights;
        m_mockRouteSync.getNextHopGroupFields(nhg, nexthops, ifnames, weights, AF_INET);

        EXPECT_EQ(nexthops, "0.0.0.0");
        EXPECT_TRUE(ifnames.empty());
        EXPECT_TRUE(weights.empty());
    }
}

TEST_F(FpmSyncdResponseTest, TestUpdateNextHopGroupDb)
{
    Table nexthop_group_table(m_db.get(), APP_NEXTHOP_GROUP_TABLE_NAME);

    // Test single next hop group
    {
        NextHopGroup nhg(1, test_gateway, "Ethernet0");
        m_mockRouteSync.updateNextHopGroupDb(nhg);

        vector<FieldValueTuple> fieldValues;
        nexthop_group_table.get("1", fieldValues);

        EXPECT_EQ(fieldValues.size(), 2);
        EXPECT_EQ(fvField(fieldValues[0]), "nexthop");
        EXPECT_EQ(fvValue(fieldValues[0]), test_gateway);
        EXPECT_EQ(fvField(fieldValues[1]), "ifname");
        EXPECT_EQ(fvValue(fieldValues[1]), "Ethernet0");
    }

    // Test group with multiple next hops
    {
        vector<pair<uint32_t,uint8_t>> group_members;
        group_members.push_back(make_pair(1, 1));
        group_members.push_back(make_pair(2, 2));

        NextHopGroup nhg1(1, test_gateway, "Ethernet0");
        NextHopGroup nhg2(2, test_gateway_, "Ethernet1");
        NextHopGroup group(3, group_members);

        m_mockRouteSync.m_nh_groups.insert({1, nhg1});
        m_mockRouteSync.m_nh_groups.insert({2, nhg2});
        m_mockRouteSync.m_nh_groups.insert({3, group});

        m_mockRouteSync.installNextHopGroup(3);

        auto it = m_mockRouteSync.m_nh_groups.find(3);
        ASSERT_NE(it, m_mockRouteSync.m_nh_groups.end());
        EXPECT_TRUE(it->second.installed);
        vector<FieldValueTuple> fieldValues;
        nexthop_group_table.get("3", fieldValues);
        EXPECT_EQ(fieldValues.size(), 3);
        EXPECT_EQ(fvField(fieldValues[0]), "nexthop");
        EXPECT_EQ(fvValue(fieldValues[0]), "192.168.1.1,192.168.1.2");
        EXPECT_EQ(fvField(fieldValues[1]), "ifname");
        EXPECT_EQ(fvValue(fieldValues[1]), "Ethernet0,Ethernet1");
        EXPECT_EQ(fvField(fieldValues[2]), "weight");
        EXPECT_EQ(fvValue(fieldValues[2]), "1,2");
    }

    // Empty nexthop (default route case)
    {
        NextHopGroup nhg(4, "", "Ethernet0");
        m_mockRouteSync.updateNextHopGroupDb(nhg);

        vector<FieldValueTuple> fieldValues;
        nexthop_group_table.get("4", fieldValues);

        EXPECT_EQ(fieldValues.size(), 2);
        EXPECT_EQ(fvField(fieldValues[0]), "nexthop");
        EXPECT_EQ(fvValue(fieldValues[0]), "0.0.0.0");
        EXPECT_EQ(fvField(fieldValues[1]), "ifname");
        EXPECT_EQ(fvValue(fieldValues[1]), "Ethernet0");
    }

    // Empty interface name
    {
        NextHopGroup nhg(5, test_gateway, "");
        m_mockRouteSync.updateNextHopGroupDb(nhg);

        vector<FieldValueTuple> fieldValues;
        nexthop_group_table.get("5", fieldValues);

        EXPECT_EQ(fieldValues.size(), 1);
        EXPECT_EQ(fvField(fieldValues[0]), "nexthop");
        EXPECT_EQ(fvValue(fieldValues[0]), test_gateway);
    }
}

TEST_F(FpmSyncdResponseTest, TestDeleteNextHopGroup)
{
    // Setup test groups
    NextHopGroup nhg1(1, test_gateway, "Ethernet0");
    NextHopGroup nhg2(2, test_gateway_, "Ethernet1");
    nhg1.installed = true;
    nhg2.installed = true;

    m_mockRouteSync.m_nh_groups.insert({1, nhg1});
    m_mockRouteSync.m_nh_groups.insert({2, nhg2});

    // Test deletion
    m_mockRouteSync.deleteNextHopGroup(1);
    EXPECT_EQ(m_mockRouteSync.m_nh_groups.find(1), m_mockRouteSync.m_nh_groups.end());
    EXPECT_NE(m_mockRouteSync.m_nh_groups.find(2), m_mockRouteSync.m_nh_groups.end());

    // Test deleting non-existent group
    m_mockRouteSync.deleteNextHopGroup(999);
    EXPECT_EQ(m_mockRouteSync.m_nh_groups.find(999), m_mockRouteSync.m_nh_groups.end());
}

struct nlmsghdr* createNewNextHopMsgHdr(const vector<pair<uint32_t, uint8_t>>& group_members, uint32_t id,
                                        uint32_t nlmsg_type = RTM_NEWNEXTHOP) {
    struct nlmsghdr *nlh = (struct nlmsghdr *)malloc(NLMSG_SPACE(MAX_PAYLOAD));
    memset(nlh, 0, NLMSG_SPACE(MAX_PAYLOAD));

    // Set header
    nlh->nlmsg_type = static_cast<unsigned short>(nlmsg_type);
    nlh->nlmsg_flags = NLM_F_REQUEST | NLM_F_CREATE | NLM_F_REPLACE;
    nlh->nlmsg_len = NLMSG_LENGTH(sizeof(struct nhmsg));

    // Set nhmsg
    struct nhmsg *nhm = (struct nhmsg *)NLMSG_DATA(nlh);
    nhm->nh_family = AF_INET;

    // Add NHA_ID
    struct rtattr *rta = (struct rtattr *)((char *)nlh + NLMSG_ALIGN(nlh->nlmsg_len));
    rta->rta_type = NHA_ID;
    rta->rta_len = RTA_LENGTH(sizeof(uint32_t));
    *(uint32_t *)RTA_DATA(rta) = id;
    nlh->nlmsg_len = NLMSG_ALIGN(nlh->nlmsg_len) + RTA_ALIGN(rta->rta_len);

    // Add NHA_GROUP
    rta = (struct rtattr *)((char *)nlh + NLMSG_ALIGN(nlh->nlmsg_len));
    rta->rta_type = NHA_GROUP;
    struct nexthop_grp* grp = (struct nexthop_grp*)malloc(group_members.size() * sizeof(struct nexthop_grp));

    for (size_t i = 0; i < group_members.size(); i++) {
        grp[i].id = group_members[i].first;
        grp[i].weight = group_members[i].second - 1; // kernel stores weight-1
    }

    size_t payload_size = group_members.size() * sizeof(struct nexthop_grp);
    if (payload_size > USHRT_MAX - RTA_LENGTH(0)) {
        free(nlh);
        return nullptr;
    }

    rta->rta_len = static_cast<unsigned short>(RTA_LENGTH(group_members.size() * sizeof(struct nexthop_grp)));
    memcpy(RTA_DATA(rta), grp, group_members.size() * sizeof(struct nexthop_grp));
    nlh->nlmsg_len = NLMSG_ALIGN(nlh->nlmsg_len) + RTA_ALIGN(rta->rta_len);

    free(grp);
    return nlh;
}

TEST_F(FpmSyncdResponseTest, TestNextHopGroupAdd)
{
    // 1. create nexthops
    uint32_t nh1_id = 1;
    uint32_t nh2_id = 2;
    uint32_t nh3_id = 3;

    struct nlmsghdr* nlh1 = createNewNextHopMsgHdr(1, test_gateway, nh1_id);
    struct nlmsghdr* nlh2 = createNewNextHopMsgHdr(2, test_gateway_, nh2_id);
    struct nlmsghdr* nlh3 = createNewNextHopMsgHdr(3, test_gateway__, nh3_id);

    EXPECT_CALL(m_mockRouteSync, getIfName(1, _, _))
        .WillOnce(DoAll(
            [](int32_t, char* ifname, size_t size) {
                strncpy(ifname, "Ethernet1", size);
                ifname[size-1] = '\0';
            },
            Return(true)
        ));

    EXPECT_CALL(m_mockRouteSync, getIfName(2, _, _))
        .WillOnce(DoAll(
            [](int32_t, char* ifname, size_t size) {
                strncpy(ifname, "Ethernet2", size);
                ifname[size-1] = '\0';
            },
            Return(true)
        ));

    EXPECT_CALL(m_mockRouteSync, getIfName(3, _, _))
        .WillOnce(DoAll(
            [](int32_t, char* ifname, size_t size) {
                strncpy(ifname, "Ethernet3", size);
                ifname[size-1] = '\0';
            },
            Return(true)
        ));

    m_mockRouteSync.onNextHopMsg(nlh1, (int)(nlh1->nlmsg_len - NLMSG_LENGTH(sizeof(struct nhmsg))));
    m_mockRouteSync.onNextHopMsg(nlh2, (int)(nlh2->nlmsg_len - NLMSG_LENGTH(sizeof(struct nhmsg))));
    m_mockRouteSync.onNextHopMsg(nlh3, (int)(nlh3->nlmsg_len - NLMSG_LENGTH(sizeof(struct nhmsg))));

    // 2. create a nexthop group with these nexthops
    uint32_t group_id = 10;
    vector<pair<uint32_t, uint8_t>> group_members = {
        {nh1_id, 1},  // id=1, weight=1
        {nh2_id, 2},  // id=2, weight=2
        {nh3_id, 3}   // id=3, weight=3
    };

    struct nlmsghdr* group_nlh = createNewNextHopMsgHdr(group_members, group_id);
    ASSERT_NE(group_nlh, nullptr) << "Failed to create group nexthop message";
    m_mockRouteSync.onNextHopMsg(group_nlh, (int)(group_nlh->nlmsg_len - NLMSG_LENGTH(sizeof(struct nhmsg))));

    // Verify the group was added correctly
    auto it = m_mockRouteSync.m_nh_groups.find(group_id);
    ASSERT_NE(it, m_mockRouteSync.m_nh_groups.end()) << "Failed to add nexthop group";

    // Verify group members
    const auto& group = it->second.group;
    ASSERT_EQ(group.size(), 3) << "Wrong number of group members";

    // Check each member's ID and weight
    EXPECT_EQ(group[0].first, nh1_id);
    EXPECT_EQ(group[0].second, 1);
    EXPECT_EQ(group[1].first, nh2_id);
    EXPECT_EQ(group[1].second, 2);
    EXPECT_EQ(group[2].first, nh3_id);
    EXPECT_EQ(group[2].second, 3);

    // Mark the group as installed and verify DB update
    m_mockRouteSync.installNextHopGroup(group_id);

    Table nexthop_group_table(m_db.get(), APP_NEXTHOP_GROUP_TABLE_NAME);
    vector<FieldValueTuple> fieldValues;
    string key = to_string(group_id);
    nexthop_group_table.get(key, fieldValues);

    ASSERT_EQ(fieldValues.size(), 3) << "Wrong number of fields in DB";

    // Verify the DB fields
    string nexthops, ifnames, weights;
    for (const auto& fv : fieldValues) {
        if (fvField(fv) == "nexthop") {
            nexthops = fvValue(fv);
        } else if (fvField(fv) == "ifname") {
            ifnames = fvValue(fv);
        } else if (fvField(fv) == "weight") {
            weights = fvValue(fv);
        }
    }

    EXPECT_EQ(nexthops, "192.168.1.1,192.168.1.2,192.168.1.3");
    EXPECT_EQ(ifnames, "Ethernet1,Ethernet2,Ethernet3");
    EXPECT_EQ(weights, "1,2,3");

    // Cleanup
    free(nlh1);
    free(nlh2);
    free(nlh3);
    free(group_nlh);
}

TEST_F(FpmSyncdResponseTest, TestRouteMsgWithNHG)
{
    Table route_table(m_db.get(), APP_ROUTE_TABLE_NAME);
    auto createRoute = [](const char* prefix, uint8_t prefixlen) -> rtnl_route* {
        rtnl_route* route = rtnl_route_alloc();
        nl_addr* dst_addr;
        nl_addr_parse(prefix, AF_INET, &dst_addr);
        rtnl_route_set_dst(route, dst_addr);
        rtnl_route_set_type(route, RTN_UNICAST);
        rtnl_route_set_protocol(route, RTPROT_STATIC);
        rtnl_route_set_family(route, AF_INET);
        rtnl_route_set_scope(route, RT_SCOPE_UNIVERSE);
        rtnl_route_set_table(route, RT_TABLE_MAIN);
        nl_addr_put(dst_addr);
        return route;
    };

    uint32_t test_nh_id = 1;
    uint32_t test_nhg_id = 2;
    uint32_t test_nh_id_ = 3;
    uint32_t test_nh_id__ = 4;

    // create a route
    const char* test_destipprefix = "10.1.1.0";
    rtnl_route* test_route = createRoute(test_destipprefix, 24);

    // Test 1: use a non-existent nh_id
    {
        rtnl_route_set_nh_id(test_route, test_nh_id);

        m_mockRouteSync.onRouteMsg(RTM_NEWROUTE, (nl_object*)test_route, nullptr);

        vector<string> keys;
        route_table.getKeys(keys);

        // verify the route is discarded
        EXPECT_TRUE(std::find(keys.begin(), keys.end(), test_destipprefix) == keys.end());
    }

    // Test 2: using a nexthop
    {
        // create the nexthop
        struct nlmsghdr* nlh = createNewNextHopMsgHdr(1, test_gateway, test_nh_id);

        EXPECT_CALL(m_mockRouteSync, getIfName(1, _, _))
            .WillOnce(DoAll(
                [](int32_t, char* ifname, size_t size) {
                    strncpy(ifname, "Ethernet1", size);
                    ifname[size-1] = '\0';
                },
                Return(true)
            ));

        m_mockRouteSync.onNextHopMsg(nlh, (int)(nlh->nlmsg_len - NLMSG_LENGTH(sizeof(struct nhmsg))));

        free(nlh);

        rtnl_route_set_nh_id(test_route, test_nh_id);

        m_mockRouteSync.onRouteMsg(RTM_NEWROUTE, (nl_object*)test_route, nullptr);

        vector<FieldValueTuple> fvs;
        EXPECT_TRUE(route_table.get(test_destipprefix, fvs));
        EXPECT_EQ(fvs.size(), 3);
        for (const auto& fv : fvs) {
            if (fvField(fv) == "nexthop") {
                EXPECT_EQ(fvValue(fv), test_gateway);
            } else if (fvField(fv) == "ifname") {
                EXPECT_EQ(fvValue(fv), "Ethernet1");
            } else if (fvField(fv) == "protocol") {
                EXPECT_EQ(fvValue(fv), "static");
            }
        }
    }

    // Test 3: using an nhg
    {
        struct nlmsghdr* nlh1 = createNewNextHopMsgHdr(2, test_gateway_, test_nh_id_);
        struct nlmsghdr* nlh2 = createNewNextHopMsgHdr(3, test_gateway__, test_nh_id__);

        EXPECT_CALL(m_mockRouteSync, getIfName(2, _, _))
            .WillOnce(DoAll(
                [](int32_t, char* ifname, size_t size) {
                    strncpy(ifname, "Ethernet2", size);
                    ifname[size-1] = '\0';
                },
                Return(true)
            ));

        EXPECT_CALL(m_mockRouteSync, getIfName(3, _, _))
            .WillOnce(DoAll(
                [](int32_t, char* ifname, size_t size) {
                    strncpy(ifname, "Ethernet3", size);
                    ifname[size-1] = '\0';
                },
                Return(true)
            ));

        m_mockRouteSync.onNextHopMsg(nlh1, (int)(nlh1->nlmsg_len - NLMSG_LENGTH(sizeof(struct nhmsg))));
        m_mockRouteSync.onNextHopMsg(nlh2, (int)(nlh2->nlmsg_len - NLMSG_LENGTH(sizeof(struct nhmsg))));

        vector<pair<uint32_t, uint8_t>> group_members = {
            {test_nh_id_, 1},
            {test_nh_id__, 2}
        };

        struct nlmsghdr* group_nlh = createNewNextHopMsgHdr(group_members, test_nhg_id);
        m_mockRouteSync.onNextHopMsg(group_nlh, (int)(group_nlh->nlmsg_len - NLMSG_LENGTH(sizeof(struct nhmsg))));

        // create the route object referring to this next hop group
        rtnl_route_set_nh_id(test_route, test_nhg_id);
        m_mockRouteSync.onRouteMsg(RTM_NEWROUTE, (nl_object*)test_route, nullptr);

        vector<FieldValueTuple> fvs;
        EXPECT_TRUE(route_table.get(test_destipprefix, fvs));

        for (const auto& fv : fvs) {
            if (fvField(fv) == "nexthop_group") {
                EXPECT_EQ(fvValue(fv), "2");
            } else if (fvField(fv) == "protocol") {
                EXPECT_EQ(fvValue(fv), "static");
            }
        }

        vector<FieldValueTuple> group_fvs;
        Table nexthop_group_table(m_db.get(), APP_NEXTHOP_GROUP_TABLE_NAME);
        EXPECT_TRUE(nexthop_group_table.get("2", group_fvs));

        // clean up
        free(nlh1);
        free(nlh2);
        free(group_nlh);
    }

    rtnl_route_put(test_route);
}

// Test for VnetTunnelTableFieldValueTupleWrapper with ZMQ enabled (line 1127)
TEST_F(FpmSyncdResponseTest, TestVxlanTunnelRouteMsgWithZmqEnabled)
{
    // Simulate ZMQ being enabled by setting m_zmqClient to a non-null value
    m_mockRouteSync.m_zmqClient = shared_ptr<swss::ZmqClient>(reinterpret_cast<swss::ZmqClient*>(1), [](swss::ZmqClient*){});

    Table vnet_tunnel_table(m_db.get(), APP_VNET_RT_TUNNEL_TABLE_NAME);

    // Create a VNET route with VXLAN interface
    auto createVxlanTunnelRoute = [](const char* prefix, uint8_t prefixlen, uint32_t vnet_table_id) -> rtnl_route* {
        rtnl_route* route = rtnl_route_alloc();
        nl_addr* dst_addr;
        nl_addr_parse(prefix, AF_INET, &dst_addr);
        rtnl_route_set_dst(route, dst_addr);
        rtnl_route_set_type(route, RTN_UNICAST);
        rtnl_route_set_protocol(route, RTPROT_STATIC);
        rtnl_route_set_family(route, AF_INET);
        rtnl_route_set_scope(route, RT_SCOPE_UNIVERSE);
        rtnl_route_set_table(route, vnet_table_id);
        nl_addr_put(dst_addr);
        return route;
    };

    const char* test_destipprefix = "192.168.10.0/24";
    uint32_t vnet_table_id = 3000;
    rtnl_route* test_route = createVxlanTunnelRoute(test_destipprefix, 24, vnet_table_id);

    // Create a nexthop with VXLAN interface (starts with "vtep")
    rtnl_nexthop* nh = rtnl_route_nh_alloc();
    rtnl_route_nh_set_ifindex(nh, 30);
    nl_addr* gateway_addr;
    nl_addr_parse("10.0.0.1", AF_INET, &gateway_addr);
    rtnl_route_nh_set_gateway(nh, gateway_addr);
    nl_addr_put(gateway_addr);
    rtnl_route_add_nexthop(test_route, nh);

    // Mock getIfName to return VNET interface name
    EXPECT_CALL(m_mockRouteSync, getIfName(vnet_table_id, _, _))
        .WillOnce(DoAll(
            [](int32_t, char* ifname, size_t size) {
                strncpy(ifname, "Vnet300", size);
                ifname[size-1] = '\0';
            },
            Return(true)
        ));

    // Mock getIfName for the VXLAN interface (starts with "Brvxlan")
    EXPECT_CALL(m_mockRouteSync, getIfName(30, _, _))
        .WillRepeatedly(DoAll(
            [](int32_t, char* ifname, size_t size) {
                strncpy(ifname, "Brvxlan100", size);  // VXLAN_IF_NAME_PREFIX is "Brvxlan"
                ifname[size-1] = '\0';
            },
            Return(true)
        ));

    m_mockRouteSync.onMsg(RTM_NEWROUTE, (nl_object*)test_route);

    // Verify the VXLAN tunnel route was written to the database
    vector<FieldValueTuple> fvs;
    std::string key = "Vnet300:192.168.10.0/24";
    EXPECT_TRUE(vnet_tunnel_table.get(key, fvs));

    // With ZMQ enabled, the endpoint field should be present
    EXPECT_EQ(fvs.size(), 1);

    // Build a map for easier verification
    std::map<std::string, std::string> fieldMap;
    for (const auto& fv : fvs) {
        fieldMap[fvField(fv)] = fvValue(fv);
    }

    // Verify endpoint field is present
    EXPECT_TRUE(fieldMap.find("endpoint") != fieldMap.end());
    EXPECT_EQ(fieldMap["endpoint"], "10.0.0.1");

    // Reset m_zmqClient to nullptr
    m_mockRouteSync.m_zmqClient = nullptr;

    rtnl_route_put(test_route);
}
TEST_F(FpmSyncdResponseTest, TestSrv6MySidMsgWithZmqEnabled)
{
    // Simulate ZMQ being enabled by setting m_zmqClient to a non-null value
    m_mockRouteSync.m_zmqClient = shared_ptr<swss::ZmqClient>(reinterpret_cast<swss::ZmqClient*>(1), [](swss::ZmqClient*){});

    Table srv6_mysid_table(m_db.get(), APP_SRV6_MY_SID_TABLE_NAME);

    // Create an SRv6 My SID with End.DT4 action
    IpAddress mysid = IpAddress("fc00:0:1:1::");
    int8_t block_len = 32;
    int8_t node_len = 16;
    int8_t func_len = 16;
    int8_t arg_len = 0;
    uint32_t action = SRV6_LOCALSID_ACTION_END_DT4;
    char vrf_name[] = "Vrf100";

    struct nlmsg *nl_obj = create_srv6_mysid_nlmsg(
        RTM_NEWSRV6LOCALSID,
        &mysid,
        block_len,
        node_len,
        func_len,
        arg_len,
        action,
        vrf_name
    );
    if (!nl_obj) {
        ADD_FAILURE() << "Failed to create SRv6 My SID message";
        return;
    }

    // Call the target function
    m_mockRouteSync.onSrv6MySidMsg(&nl_obj->n, nl_obj->n.nlmsg_len);

    // Verify the SRv6 My SID was written to the database
    vector<FieldValueTuple> fvs;
    std::string key = "32:16:16:0:fc00:0:1:1::";
    EXPECT_TRUE(srv6_mysid_table.get(key, fvs));

    // With ZMQ enabled, all 3 fields should be present (action, vrf, adj)
    EXPECT_EQ(fvs.size(), 3);

    // Build a map for easier verification
    std::map<std::string, std::string> fieldMap;
    for (const auto& fv : fvs) {
        fieldMap[fvField(fv)] = fvValue(fv);
    }

    // Verify all fields are present
    EXPECT_TRUE(fieldMap.find("action") != fieldMap.end());
    EXPECT_EQ(fieldMap["action"], "end.dt4");

    EXPECT_TRUE(fieldMap.find("vrf") != fieldMap.end());
    EXPECT_EQ(fieldMap["vrf"], "Vrf100");

    // adj field should be present but empty
    EXPECT_TRUE(fieldMap.find("adj") != fieldMap.end());
    EXPECT_EQ(fieldMap["adj"], "");

    // Reset m_zmqClient to nullptr
    m_mockRouteSync.m_zmqClient = nullptr;

    free(nl_obj);
}

TEST_F(FpmSyncdResponseTest, RouteResponseOnNoProto)
{
    // Expect the message to zebra is sent
    EXPECT_CALL(m_mockFpm, send(_)).Times(0);

    m_routeSync.onRouteResponse("1.0.0.0/24", {
        {"err_str", "SWSS_RC_SUCCESS"},
    });
}

TEST_F(FpmSyncdResponseTest, TestBlackholeRoute)
{
    Table route_table(m_db.get(), APP_ROUTE_TABLE_NAME);
    Table label_route_table(m_db.get(), APP_LABEL_ROUTE_TABLE_NAME);
    auto createRoute = [](const char* prefix, uint8_t prefixlen) -> rtnl_route* {
        rtnl_route* route = rtnl_route_alloc();
        nl_addr* dst_addr;
        nl_addr_parse(prefix, AF_INET, &dst_addr);
        rtnl_route_set_dst(route, dst_addr);
        rtnl_route_set_type(route, RTN_BLACKHOLE);
        rtnl_route_set_protocol(route, RTPROT_STATIC);
        rtnl_route_set_family(route, AF_INET);
        rtnl_route_set_scope(route, RT_SCOPE_UNIVERSE);
        rtnl_route_set_table(route, RT_TABLE_UNSPEC);
        nl_addr_put(dst_addr);
        return route;
    };

    // create a route
    const char* test_destipprefix = "10.1.1.0";
    rtnl_route* test_route = createRoute(test_destipprefix, 24);

    const char* test_destipprefix2 = "20.1.1.0";
    rtnl_route* test_route2 = createRoute(test_destipprefix2, 24);
    {

        m_mockRouteSync.onRouteMsg(RTM_NEWROUTE, (nl_object*)test_route, nullptr);

        // verify the blackhole route has protocol programmed
        vector<FieldValueTuple> fvs;
        EXPECT_TRUE(route_table.get(test_destipprefix, fvs));

        bool proto_found = false;
        for (const auto& fv : fvs) {
            if (fvField(fv) == "protocol") {
                proto_found = true;
                EXPECT_EQ(fvValue(fv), "static");
            }
        }
        EXPECT_TRUE(proto_found);

        m_mockRouteSync.onLabelRouteMsg(RTM_NEWROUTE, (nl_object*)test_route2);

        // verify the blackhole route has protocol programmed
        EXPECT_TRUE(label_route_table.get(test_destipprefix2, fvs));

        proto_found = false;
        for (const auto& fv : fvs) {
            if (fvField(fv) == "protocol") {
                proto_found = true;
                EXPECT_EQ(fvValue(fv), "static");
            }
        }
        EXPECT_TRUE(proto_found);
    }
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
    rtnl_nexthop* nh1 = create_nexthop(test_gateway);
    rtnl_nexthop* nh2 = create_nexthop(test_gateway_);

    // Add new nexthops to the route
    rtnl_route_add_nexthop(test_route.get(), nh1);
    rtnl_route_add_nexthop(test_route.get(), nh2);

    EXPECT_EQ(m_mockRouteSync.getNextHopWt(test_route.get()), "1,1");
}

class WarmRestartRouteSyncTest : public ::testing::Test
{
public:
    void SetUp() override
    {
        resetMockWarmStartHelper();  // Reset warm restart state before each test
        testing_db::reset();
        EXPECT_EQ(rtnl_route_read_protocol_names(DefaultRtProtoPath), 0);
    }

    void TearDown() override
    {
        resetMockWarmStartHelper();  // Reset warm restart state after each test
        testing_db::reset();
    }

    shared_ptr<swss::DBConnector> m_db = make_shared<swss::DBConnector>("APPL_DB", 0);
    shared_ptr<RedisPipeline> m_pipeline = make_shared<RedisPipeline>(m_db.get());
    RouteSync m_testRouteSync{m_pipeline.get()};
};

TEST_F(WarmRestartRouteSyncTest, TestRouteMessageHandlingWarmRestartNotInProgress)
{
    EXPECT_FALSE(m_testRouteSync.getWarmStartHelper().inProgress());

    auto route = create_route("192.168.1.0/24");

    rtnl_route_set_type(route.get(), RTN_BLACKHOLE);
    rtnl_route_set_protocol(route.get(), RTPROT_BGP);

    m_testRouteSync.onRouteMsg(RTM_NEWROUTE, (struct nl_object*)route.get(), nullptr);

    // Verify: Route was set directly in the table
    Table routeTable(m_db.get(), APP_ROUTE_TABLE_NAME);
    vector<FieldValueTuple> result;
    EXPECT_TRUE(routeTable.get("192.168.1.0/24", result));

    // Should have protocol and blackhole fields
    bool foundProtocol = false, foundBlackhole = false;
    for (const auto& fv : result) {
        if (fvField(fv) == "protocol" && fvValue(fv) == "bgp") {
            foundProtocol = true;
        } else if (fvField(fv) == "blackhole" && fvValue(fv) == "true") {
            foundBlackhole = true;
        }
    }
    EXPECT_TRUE(foundProtocol);
    EXPECT_TRUE(foundBlackhole);
}

TEST_F(WarmRestartRouteSyncTest, TestRouteDeleteHandlingWarmRestartNotInProgress)
{
    auto route = create_route("192.168.2.0/24");
    rtnl_route_set_type(route.get(), RTN_BLACKHOLE);
    rtnl_route_set_protocol(route.get(), RTPROT_STATIC);

    m_testRouteSync.onRouteMsg(RTM_NEWROUTE, (struct nl_object*)route.get(), nullptr);

    Table routeTable(m_db.get(), APP_ROUTE_TABLE_NAME);
    vector<FieldValueTuple> result;
    EXPECT_TRUE(routeTable.get("192.168.2.0/24", result));

    EXPECT_FALSE(m_testRouteSync.getWarmStartHelper().inProgress());

    m_testRouteSync.onRouteMsg(RTM_DELROUTE, (struct nl_object*)route.get(), nullptr);

    // Verify: Route was deleted from the table
    EXPECT_FALSE(routeTable.get("192.168.2.0/24", result));
}

TEST_F(WarmRestartRouteSyncTest, TestBlackholeRouteHandlingWarmRestartNotInProgress)
{
    EXPECT_FALSE(m_testRouteSync.getWarmStartHelper().inProgress());

    auto route = create_route("192.168.6.0/24");
    rtnl_route_set_type(route.get(), RTN_BLACKHOLE);
    rtnl_route_set_protocol(route.get(), RTPROT_STATIC);

    m_testRouteSync.onRouteMsg(RTM_NEWROUTE, (struct nl_object*)route.get(), nullptr);

    Table routeTable(m_db.get(), APP_ROUTE_TABLE_NAME);
    vector<FieldValueTuple> result;
    EXPECT_TRUE(routeTable.get("192.168.6.0/24", result));

    bool foundBlackhole = false, foundProtocol = false;
    for (const auto& fv : result) {
        if (fvField(fv) == "blackhole" && fvValue(fv) == "true") {
            foundBlackhole = true;
        } else if (fvField(fv) == "protocol" && fvValue(fv) == "static") {
            foundProtocol = true;
        }
    }
    EXPECT_TRUE(foundBlackhole);
    EXPECT_TRUE(foundProtocol);
}

TEST_F(WarmRestartRouteSyncTest, TestVrfRouteHandlingWarmRestartNotInProgress)
{
    // Test VRF route handling with warm restart integration

    EXPECT_FALSE(m_testRouteSync.getWarmStartHelper().inProgress());

    auto route = create_route("192.168.8.0/24");
    rtnl_route_set_type(route.get(), RTN_BLACKHOLE);
    rtnl_route_set_protocol(route.get(), RTPROT_BGP);
    rtnl_route_set_table(route.get(), 100); // VRF table ID

    m_testRouteSync.onRouteMsg(RTM_NEWROUTE, (struct nl_object*)route.get(), "Vrf100");

    Table routeTable(m_db.get(), APP_ROUTE_TABLE_NAME);
    vector<FieldValueTuple> result;
    EXPECT_TRUE(routeTable.get("Vrf100:192.168.8.0/24", result));

    bool foundProtocol = false, foundBlackhole = false;
    for (const auto& fv : result) {
        if (fvField(fv) == "protocol" && fvValue(fv) == "bgp") {
            foundProtocol = true;
        } else if (fvField(fv) == "blackhole" && fvValue(fv) == "true") {
            foundBlackhole = true;
        }
    }
    EXPECT_TRUE(foundProtocol);
    EXPECT_TRUE(foundBlackhole);
}

TEST_F(WarmRestartRouteSyncTest, TestStaticRouteHandlingWarmRestartNotInProgress)
{
    // Test static route handling with warm restart integration
    EXPECT_FALSE(m_testRouteSync.getWarmStartHelper().inProgress());

    auto route = create_route("192.168.3.0/24");
    rtnl_route_set_type(route.get(), RTN_BLACKHOLE);
    rtnl_route_set_protocol(route.get(), RTPROT_STATIC);

    m_testRouteSync.onRouteMsg(RTM_NEWROUTE, (struct nl_object*)route.get(), nullptr);

    Table routeTable(m_db.get(), APP_ROUTE_TABLE_NAME);
    vector<FieldValueTuple> result;
    EXPECT_TRUE(routeTable.get("192.168.3.0/24", result));

    bool foundProtocol = false;
    for (const auto& fv : result) {
        if (fvField(fv) == "protocol" && fvValue(fv) == "static") {
            foundProtocol = true;
        }
    }
    EXPECT_TRUE(foundProtocol);
}

// Tests for when warm restart IS in progress
TEST_F(WarmRestartRouteSyncTest, TestRouteHandlingWarmRestartInProgress)
{
    // Simulate warm restart in progress by setting state to INITIALIZED (not RECONCILED)
    m_testRouteSync.getWarmStartHelper().setState(WarmStart::INITIALIZED);

    EXPECT_TRUE(m_testRouteSync.getWarmStartHelper().inProgress());
    EXPECT_FALSE(m_testRouteSync.getWarmStartHelper().isReconciled());

    auto route = create_route("192.168.10.0/24");
    rtnl_route_set_type(route.get(), RTN_BLACKHOLE);
    rtnl_route_set_protocol(route.get(), RTPROT_BGP);

    m_testRouteSync.onRouteMsg(RTM_NEWROUTE, (struct nl_object*)route.get(), nullptr);

    Table routeTable(m_db.get(), APP_ROUTE_TABLE_NAME);
    vector<FieldValueTuple> result;
    EXPECT_FALSE(routeTable.get("192.168.10.0/24", result));
}

TEST_F(WarmRestartRouteSyncTest, TestVrfRouteHandlingWarmRestartInProgress)
{
    // Simulate warm restart in progress by setting state to RESTORED
    m_testRouteSync.getWarmStartHelper().setState(WarmStart::RESTORED);

    EXPECT_TRUE(m_testRouteSync.getWarmStartHelper().inProgress());

    auto route = create_route("192.168.11.0/24");
    rtnl_route_set_type(route.get(), RTN_BLACKHOLE);
    rtnl_route_set_protocol(route.get(), RTPROT_STATIC);
    rtnl_route_set_table(route.get(), 200); // Different VRF table

    m_testRouteSync.onRouteMsg(RTM_NEWROUTE, (struct nl_object*)route.get(), "Vrf200");

    // Verify: Route should NOT be in the regular table yet (handled by warm restart helper)
    Table routeTable(m_db.get(), APP_ROUTE_TABLE_NAME);
    vector<FieldValueTuple> result;
    EXPECT_FALSE(routeTable.get("Vrf200:192.168.11.0/24", result));
}

TEST_F(WarmRestartRouteSyncTest, TestRouteDeleteHandlingWarmRestartInProgress)
{
    auto route = create_route("192.168.12.0/24");
    rtnl_route_set_type(route.get(), RTN_BLACKHOLE);
    rtnl_route_set_protocol(route.get(), RTPROT_STATIC);

    m_testRouteSync.onRouteMsg(RTM_NEWROUTE, (struct nl_object*)route.get(), nullptr);

    Table routeTable(m_db.get(), APP_ROUTE_TABLE_NAME);
    vector<FieldValueTuple> result;
    EXPECT_TRUE(routeTable.get("192.168.12.0/24", result));

    // Now simulate warm restart in progress
    m_testRouteSync.getWarmStartHelper().setState(WarmStart::INITIALIZED);
    EXPECT_TRUE(m_testRouteSync.getWarmStartHelper().inProgress());

    m_testRouteSync.onRouteMsg(RTM_DELROUTE, (struct nl_object*)route.get(), nullptr);

    // Verify: Route should still be in table (deletion handled by warm restart helper)
    EXPECT_TRUE(routeTable.get("192.168.12.0/24", result));


}


TEST_F(FpmSyncdResponseTest, TestSrv6VpnRoute_Add_NHG)
{
    std::string dst_prefix = "2001:db8::/64";
    std::string encap_src = "2001:db8::1";
    std::string vpn_sid = "2001:db8::2";
    uint16_t vrf_table_id = 100;
    uint32_t pic_id = 67;
    uint32_t nhg_id = 12;

    /* Create IpAddress and IpPrefix Object */
    IpAddress _encap_src_obj = IpAddress(encap_src);
    IpAddress _vpn_sid_obj = IpAddress(vpn_sid);
    IpPrefix _dst_obj = IpPrefix(dst_prefix);

    /* Create Srv6 Vpn route netlink msg */
    struct nlmsg *nl_obj = create_srv6_vpn_route_nlmsg(
        RTM_NEWSRV6VPNROUTE,
        &_dst_obj,
        &_encap_src_obj,
        &_vpn_sid_obj,
        vrf_table_id,
        64,
        AF_INET6,
        RTN_UNICAST,
        nhg_id,
        pic_id);
    if (!nl_obj) {
        ADD_FAILURE() << "Failed to create SRv6 VPN Route message";
        return;
    }

    /* Mock using getIfName to return vrfname */
    EXPECT_CALL(m_mockRouteSync, getIfName(vrf_table_id, _, _))
        .Times(2)
        .WillRepeatedly(DoAll(
            [](int32_t, char* ifname, size_t size) {
                strncpy(ifname, "Vrf100", size);
                ifname[size-1] = '\0';
            },
            Return(true)
        ));

    /* for case: not found pic_it or nhg_it
     * nothing to check and would return.
     */
    m_mockRouteSync.onSrv6VpnRouteMsg(&nl_obj->n, nl_obj->n.nlmsg_len);

    /* Construct PIC Group */
    NextHopGroup pic_group(pic_id, encap_src, "sr0");
    pic_group.vpn_sid = vpn_sid;
    pic_group.seg_src = encap_src;
    m_mockRouteSync.m_nh_groups.insert({pic_id, pic_group});

    /* Construct NHG */
    vector<pair<uint32_t, uint8_t>> nhg_data;
    nhg_data.push_back(make_pair(1, 1));
    NextHopGroup nh_group(nhg_id, nhg_data);
    nh_group.nexthop = "fe80::1";
    nh_group.intf = "eth0";
    m_mockRouteSync.m_nh_groups.insert({nhg_id, nh_group});

    /* Call the target function */
    m_mockRouteSync.onSrv6VpnRouteMsg(&nl_obj->n, nl_obj->n.nlmsg_len);

    // Check whether use the m_routeTable.set
    Table route_table(m_db.get(), APP_ROUTE_TABLE_NAME);
    std::vector<FieldValueTuple> fvs;
    std::string key = "Vrf100:" + dst_prefix;

    /* Check the results */
    bool found = route_table.get(key, fvs);
    EXPECT_TRUE(found);
    // Check each attr value
    for (const auto& fv : fvs) {
        if (fvField(fv) == "pic_context_id") {
            EXPECT_EQ(fvValue(fv), "67");
        } else if (fvField(fv) == "nexthop_group") {
            EXPECT_EQ(fvValue(fv), "12");
        }
    }

    /* Check whether use the m_nexthop_groupTable.set */
    Table nhg_table(m_db.get(), APP_NEXTHOP_GROUP_TABLE_NAME);
    std::vector<FieldValueTuple> fvs_nhg;
    std::string key_nhg = m_mockRouteSync.getNextHopGroupKeyAsString(nhg_id);

    /* Check the result */
    bool found_nhg = nhg_table.get(key_nhg.c_str(), fvs_nhg);
    EXPECT_TRUE(found_nhg);
    // Check each attr value
    for (const auto& fv_nhg : fvs_nhg) {
        if (fvField(fv_nhg) == "seg_src") {
            EXPECT_EQ(fvValue(fv_nhg), "2001:db8::1");
        }
    }

    free(nl_obj);
}

TEST_F(FpmSyncdResponseTest, TestSrv6VpnRoute_NH)
{
    std::string dst_prefix = "2001:db8:1::/64";
    std::string encap_src = "2001:db8:1::1";
    std::string vpn_sid = "2001:db8:1::2";
    uint16_t vrf_table_id = 101;
    uint32_t pic_id = 89;
    uint32_t nhg_id = 34;

    /* Create IpAddress and IpPrefix Object */
    IpAddress _encap_src_obj = IpAddress(encap_src);
    IpAddress _vpn_sid_obj = IpAddress(vpn_sid);
    IpPrefix _dst_obj = IpPrefix(dst_prefix);

    /* Mock using getIfName to return vrfname */
    EXPECT_CALL(m_mockRouteSync, getIfName(vrf_table_id, _, _))
        .Times(11)
        .WillRepeatedly(DoAll(
            [](int32_t, char* ifname, size_t size) {
                strncpy(ifname, "Vrf101", size);
                ifname[size-1] = '\0';
            },
            Return(true)
        ));

    /*-----------------------------------------------*/
    /* Test 1: Create and process ADD message for NH */
    /*-----------------------------------------------*/
    {
        /* Create Srv6 Vpn route netlink msg with ADD cmd */
        struct nlmsg *nl_obj = create_srv6_vpn_route_nlmsg(
            RTM_NEWSRV6VPNROUTE,
            &_dst_obj,
            &_encap_src_obj,
            &_vpn_sid_obj,
            vrf_table_id,
            64,
            AF_INET6,
            RTN_UNICAST,
            nhg_id,
            pic_id);
        if (!nl_obj) {
            ADD_FAILURE() << "Failed to create SRv6 VPN Route message";
            return;
        }

        /* Construct PIC Group */
        NextHopGroup pic_group(pic_id, encap_src, "sr0");
        pic_group.vpn_sid = vpn_sid;
        pic_group.seg_src = encap_src;
        m_mockRouteSync.m_nh_groups.insert({pic_id, pic_group});

        /* Construct NHG with no group */
        NextHopGroup nh_group(nhg_id, "fe80::2", "eth1");
        nh_group.nexthop = "fe80::2";
        nh_group.intf = "eth1";
        m_mockRouteSync.m_nh_groups.insert({nhg_id, nh_group});

        /* Call the target function */
        m_mockRouteSync.onSrv6VpnRouteMsg(&nl_obj->n, nl_obj->n.nlmsg_len);

        /* Check whether use the m_routeTable.set */
        Table route_table(m_db.get(), APP_ROUTE_TABLE_NAME);
        std::vector<FieldValueTuple> fvs;
        std::string key = "Vrf101:" + dst_prefix;

        /* Check the result */
        bool found = route_table.get(key, fvs);
        EXPECT_TRUE(found);
        // Check each attr value
        for (const auto& fv : fvs) {
            if (fvField(fv) == "nexthop") {
                EXPECT_EQ(fvValue(fv), "2001:db8:1::1");
            } else if (fvField(fv) == "vpn_sid") {
                EXPECT_EQ(fvValue(fv), "2001:db8:1::2");
            } else if (fvField(fv) == "seg_src") {
                EXPECT_EQ(fvValue(fv), "2001:db8:1::1");
            } else if (fvField(fv) == "ifname") {
                EXPECT_EQ(fvValue(fv), "eth1");
            }
        }

        /* Free the memory */
        free(nl_obj);
    }

    /*-----------------------------------------------*/
    /* Test 2: Create and process DEL message for NH */
    /*-----------------------------------------------*/
    {
        /* Create Srv6 Vpn route netlink msg with DEL cmd */
        struct nlmsg *del_nl_obj = create_srv6_vpn_route_nlmsg(
            RTM_DELSRV6VPNROUTE,
            &_dst_obj,
            &_encap_src_obj,
            &_vpn_sid_obj,
            vrf_table_id,
            64,
            AF_INET6,
            RTN_UNICAST,
            nhg_id,
            pic_id);
        if (!del_nl_obj) {
            ADD_FAILURE() << "Failed to create SRv6 DEL Route message";
            return;
        }

        /* Call the target function for DEL */
        m_mockRouteSync.onSrv6VpnRouteMsg(&del_nl_obj->n, del_nl_obj->n.nlmsg_len);

        /* Check whether use the m_routeTable.set */
        Table route_table(m_db.get(), APP_ROUTE_TABLE_NAME);
        std::vector<FieldValueTuple> fvs;
        std::string key = "Vrf101:" + dst_prefix;

        /* Check whether the route was deleted */
        bool found = route_table.get(key, fvs);
        EXPECT_FALSE(found);

        free(del_nl_obj);
    }

    /*-----------------------------*/
    /* Test 3: Test other branches */
    /*-----------------------------*/
    // Case 1: no DST
    {
        /* Create a route message without RTA_DST */
        struct nlmsg *nl_obj_no_dst = create_srv6_vpn_route_nlmsg(
            RTM_NEWSRV6VPNROUTE,
            nullptr,
            &_encap_src_obj,
            &_vpn_sid_obj,
            vrf_table_id,
            64,
            AF_INET6,
            RTN_UNICAST,
            nhg_id,
            pic_id);
        if (!nl_obj_no_dst) {
            ADD_FAILURE() << "Failed to create SRv6 VPN Route message";
            return;
        }

        /* Call the target function */
        m_mockRouteSync.onSrv6VpnRouteMsg(&nl_obj_no_dst->n, nl_obj_no_dst->n.nlmsg_len);

        free(nl_obj_no_dst);
    }

    // Case 2: AF_INET6 with too large dst bitlen
    {
        /* Create a route message with too large dst bitlen */
        struct nlmsg *nl_obj_large_bitlen = create_srv6_vpn_route_nlmsg(
            RTM_NEWSRV6VPNROUTE,
            &_dst_obj,
            &_encap_src_obj,
            &_vpn_sid_obj,
            vrf_table_id,
            130,
            AF_INET6,
            RTN_UNICAST,
            nhg_id,
            pic_id);
        if (!nl_obj_large_bitlen) {
            ADD_FAILURE() << "Failed to create SRv6 VPN Route message";
            return;
        }

        /* Call the target function */
        m_mockRouteSync.onSrv6VpnRouteMsg(&nl_obj_large_bitlen->n, nl_obj_large_bitlen->n.nlmsg_len);

        free(nl_obj_large_bitlen);
    }

    // Case 3: AF_INET6 with max dst bitlen
    {
        /* Create a route message with max dst bitlen */
        struct nlmsg *nl_obj_max_bitlen = create_srv6_vpn_route_nlmsg(
            RTM_NEWSRV6VPNROUTE,
            &_dst_obj,
            &_encap_src_obj,
            &_vpn_sid_obj,
            vrf_table_id,
            128,
            AF_INET6,
            RTN_UNICAST,
            nhg_id,
            pic_id);
        if (!nl_obj_max_bitlen) {
            ADD_FAILURE() << "Failed to create SRv6 VPN Route message";
            return;
        }

        /* Call the target function */
        m_mockRouteSync.onSrv6VpnRouteMsg(&nl_obj_max_bitlen->n, nl_obj_max_bitlen->n.nlmsg_len);

        free(nl_obj_max_bitlen);
    }

    // Case 4: wrong nlmsg_type, neither RTM_NEWSRV6VPNROUTE nor RTM_DELSRV6VPNROUTE
    {
        /* Create a route message with wrong nlmsg_type */
        struct nlmsg *nl_obj_wrong_nlmsg_type = create_srv6_vpn_route_nlmsg(
            RTM_NEWSRV6LOCALSID,
            &_dst_obj,
            &_encap_src_obj,
            &_vpn_sid_obj,
            vrf_table_id,
            64,
            AF_INET6,
            RTN_UNICAST,
            nhg_id,
            pic_id);
        if (!nl_obj_wrong_nlmsg_type) {
            ADD_FAILURE() << "Failed to create SRv6 VPN Route message";
            return;
        }

        /* Call the target function */
        m_mockRouteSync.onSrv6VpnRouteMsg(&nl_obj_wrong_nlmsg_type->n, nl_obj_wrong_nlmsg_type->n.nlmsg_len);

        free(nl_obj_wrong_nlmsg_type);
    }

    // Case 5: wrong rtm_type
    {
        /* List of rtm_types to test */
        int types_to_test[] = {
            RTN_BLACKHOLE,
            RTN_UNREACHABLE,
            RTN_PROHIBIT,
            RTN_MULTICAST,
            RTN_BROADCAST,
            RTN_LOCAL,
            __RTN_MAX       // default case
        };

        for (int rtm_type : types_to_test) {
            struct nlmsg *nl_obj_wrong_rtm_type = create_srv6_vpn_route_nlmsg(
                RTM_NEWSRV6VPNROUTE,
                &_dst_obj,
                &_encap_src_obj,
                &_vpn_sid_obj,
                vrf_table_id,
                64,
                AF_INET6,
                static_cast<uint8_t>(rtm_type),
                nhg_id,
                pic_id);
            if (!nl_obj_wrong_rtm_type) {
                ADD_FAILURE() << "Failed to create SRv6 VPN Route message with type " << rtm_type;
                continue;
            }

            /* Call the target function */
            m_mockRouteSync.onSrv6VpnRouteMsg(&nl_obj_wrong_rtm_type->n, nl_obj_wrong_rtm_type->n.nlmsg_len);

            free(nl_obj_wrong_rtm_type);
        }
    }

    // Case 6: invalid rtm_family
    {
        /* Create a route message with invalid rtm_family */
        struct nlmsg *nl_obj_invalid_rtm_family = create_srv6_vpn_route_nlmsg(
            RTM_NEWSRV6VPNROUTE,
            &_dst_obj,
            &_encap_src_obj,
            &_vpn_sid_obj,
            vrf_table_id,
            64,
            AF_LOCAL,
            RTN_UNICAST,
            nhg_id,
            pic_id);
        if (!nl_obj_invalid_rtm_family) {
            ADD_FAILURE() << "Failed to create SRv6 VPN Route message";
            return;
        }

        /* Call the target function */
        m_mockRouteSync.onSrv6VpnRouteMsg(&nl_obj_invalid_rtm_family->n, nl_obj_invalid_rtm_family->n.nlmsg_len);

        free(nl_obj_invalid_rtm_family);
    }

    // Case 7: create RTA_TABLE
    {
        /* Create a route message with RTA_TABLE */
        struct nlmsg *nl_obj_RTA_TABLE = create_srv6_vpn_route_nlmsg(
            RTM_NEWSRV6VPNROUTE,
            &_dst_obj,
            &_encap_src_obj,
            &_vpn_sid_obj,
            257,    // set vrf_table_id > 256
            64,
            AF_INET6,
            RTN_UNICAST,
            nhg_id,
            pic_id);
        if (!nl_obj_RTA_TABLE) {
            ADD_FAILURE() << "Failed to create SRv6 VPN Route message";
            return;
        }

        /* Mock using getIfName to return vrfname, vrf_table_id == 257 */
        EXPECT_CALL(m_mockRouteSync, getIfName(257, _, _))
            .WillOnce(DoAll(
                [](int32_t, char* ifname, size_t size) {
                    strncpy(ifname, "Vrf257", size);
                    ifname[size-1] = '\0';
                },
                Return(true)
            ));

        /* Call the target function */
        m_mockRouteSync.onSrv6VpnRouteMsg(&nl_obj_RTA_TABLE->n, nl_obj_RTA_TABLE->n.nlmsg_len);

        free(nl_obj_RTA_TABLE);
    }
}


/* Add UT for onPicContextMsg */
struct nlmsghdr* createPicContextMsgHdr(uint16_t msg_type, uint32_t id = 0, const char *gateway = nullptr,
                                        int32_t ifindex = 0, unsigned char nh_family = AF_INET,
                                        const char *seg6_sid = nullptr,
                                        const char *seg6_src = nullptr,
                                        uint32_t encap_type = LWTUNNEL_ENCAP_SEG6)
{
    struct nlmsghdr *nlh = (struct nlmsghdr *)malloc(NLMSG_SPACE(MAX_PAYLOAD));
    memset(nlh, 0, NLMSG_SPACE(MAX_PAYLOAD));

    // Set header
    nlh->nlmsg_type = msg_type;
    nlh->nlmsg_flags = NLM_F_REQUEST | NLM_F_CREATE | NLM_F_REPLACE;
    nlh->nlmsg_len = NLMSG_LENGTH(sizeof(struct nhmsg));

    // Set nhmsg
    struct nhmsg *nhm = (struct nhmsg *)NLMSG_DATA(nlh);
    nhm->nh_family = nh_family;

    // Prepare the rta
    struct rtattr *rta;
    // Add NHA_ID
    if (id) {
        rta = (struct rtattr *)((char *)nlh + NLMSG_ALIGN(nlh->nlmsg_len));
        rta->rta_type = NHA_ID;
        rta->rta_len = RTA_LENGTH(sizeof(uint32_t));
        *(uint32_t *)RTA_DATA(rta) = id;
        nlh->nlmsg_len = NLMSG_ALIGN(nlh->nlmsg_len) + RTA_ALIGN(rta->rta_len);
    }

    // Add NHA_OIF
    if (ifindex) {
        rta = (struct rtattr *)((char *)nlh + NLMSG_ALIGN(nlh->nlmsg_len));
        rta->rta_type = NHA_OIF;
        rta->rta_len = RTA_LENGTH(sizeof(int32_t));
        *(int32_t *)RTA_DATA(rta) = ifindex;
        nlh->nlmsg_len = NLMSG_ALIGN(nlh->nlmsg_len) + RTA_ALIGN(rta->rta_len);
    }

    // Add NHA_GATEWAY
    if (gateway) {
        rta = (struct rtattr *)((char *)nlh + NLMSG_ALIGN(nlh->nlmsg_len));
        rta->rta_type = NHA_GATEWAY;
        if (nh_family == AF_INET6) {
            struct in6_addr gw_addr6;
            inet_pton(AF_INET6, gateway, &gw_addr6);
            rta->rta_len = RTA_LENGTH(sizeof(struct in6_addr));
            memcpy(RTA_DATA(rta), &gw_addr6, sizeof(struct in6_addr));
        }
        else {
            struct in_addr gw_addr;
            inet_pton(AF_INET, gateway, &gw_addr);
            rta->rta_len = RTA_LENGTH(sizeof(struct in_addr));
            memcpy(RTA_DATA(rta), &gw_addr, sizeof(struct in_addr));
        }
        nlh->nlmsg_len = NLMSG_ALIGN(nlh->nlmsg_len) + RTA_ALIGN(rta->rta_len);
    }

    // Add Srv6 Encap info if provided
    if (seg6_sid && seg6_src) {
        // Add NHA_ENCAP_TYPE tlv, type of value is int. Similar with rta_type, change it to uint16_t
        rta = (struct rtattr *)((char *)nlh + NLMSG_ALIGN(nlh->nlmsg_len));
        rta->rta_type = NHA_ENCAP_TYPE;
        rta->rta_len = RTA_LENGTH(sizeof(uint16_t));
        *(uint16_t *)RTA_DATA(rta) = static_cast<unsigned short>(encap_type);
        nlh->nlmsg_len = NLMSG_ALIGN(nlh->nlmsg_len) + RTA_ALIGN(rta->rta_len);

        /*
         * Prepare work: Calculate the nested tlv length,
         * we need it to fill the value of outside len.
         */
        size_t num_segments = 1;
        size_t value_size = sizeof(struct seg6_iptunnel_encap_pri)
                            + num_segments * sizeof(struct ipv6_sr_hdr)
                            + num_segments * sizeof(struct in6_addr);
        size_t nested_size = sizeof(struct rtattr) + value_size;

        // Add type and len of NHA_ENCAP
        rta = (struct rtattr *)((char *)nlh + NLMSG_ALIGN(nlh->nlmsg_len));
        rta->rta_type = NHA_ENCAP;
        rta->rta_len = static_cast<unsigned short>(RTA_LENGTH(nested_size));

        // Add nested SEG6_IPTUNNEL_SRH as ENCAP's payload
        struct rtattr *sub_rta = (struct rtattr *)(RTA_DATA(rta));
        // Add type and len of SEG6_IPTUNNEL_SRH
        sub_rta->rta_type = SEG6_IPTUNNEL_SRH;
        sub_rta->rta_len = static_cast<unsigned short>(RTA_LENGTH(value_size));

        // Prepare the value we truly need
        struct seg6_iptunnel_encap_pri *encap_data = (struct seg6_iptunnel_encap_pri *)malloc(value_size);
        if (!encap_data) {
            free(nlh);
            return NULL;
        }
        memset(encap_data, 0, value_size);

        // Set the src
        struct in6_addr src;
        inet_pton(AF_INET6, seg6_src, &src);
        encap_data->src = src;

        // Aquire srh pointer
        struct ipv6_sr_hdr *srh = encap_data->srh;
        // Set segments Address
        struct in6_addr sid;
        inet_pton(AF_INET6, seg6_sid, &sid);
        memcpy(srh->segments, &sid, sizeof(sid));

        // Copy the entire data into the Netlink message
        memcpy(RTA_DATA(sub_rta), encap_data, value_size);
        nlh->nlmsg_len = NLMSG_ALIGN(nlh->nlmsg_len) + RTA_ALIGN(rta->rta_len);

        free(encap_data);
    }

    return nlh;
}

TEST_F(FpmSyncdResponseTest, TestPicContext_NH)
{
    uint16_t msg_type = RTM_NEWPICCONTEXT;
    uint32_t id = 100;
    const char *gateway = "2001:db8::1";
    int32_t ifindex = 101;
    unsigned char nh_family = AF_INET6;
    const char *seg6_sid = "2001:db8::2";
    const char *seg6_src = "2001:db8::3";


    /*-------------------------------------------*/
    /* Test 1: Create and process ADD msg for NH */
    /*-------------------------------------------*/
    {
        /* Create netlink msg header with ADD cmd */
        struct nlmsghdr *nlh = createPicContextMsgHdr(
            msg_type,
            id,
            gateway,
            ifindex,
            nh_family,
            seg6_sid,
            seg6_src
        );
        if (!nlh) {
            ADD_FAILURE() << "Failed to create Pic Context nlmsghdr";
            return;
        }
        int expected_length = (int)(nlh->nlmsg_len - NLMSG_LENGTH(sizeof(struct nhmsg)));

        /* Construct the IfName */
        EXPECT_CALL(m_mockRouteSync, getIfName(ifindex, _, _))
            .WillOnce(DoAll(
                [](int32_t, char *ifname, size_t size) {
                    strncpy(ifname, "Ethernet1", size);
                    ifname[size-1] = '\0';
                },
                Return(true)
            ));

        /* Call the target function */
        m_mockRouteSync.onPicContextMsg(nlh, expected_length);

        /* Check the results */
        auto it = m_mockRouteSync.m_nh_groups.find(id);
        ASSERT_NE(it, m_mockRouteSync.m_nh_groups.end()) << "Failed to add new Pic Context";

        /* Check other attrs */
        const NextHopGroup &nhg = it->second;
        // Check each value of attr
        EXPECT_EQ(nhg.nexthop, gateway);    // Check gateway
        EXPECT_EQ(nhg.intf, "Ethernet1");       // Check interface name
        EXPECT_EQ(nhg.vpn_sid, seg6_sid);       // Check sid
        EXPECT_EQ(nhg.seg_src, seg6_src);       // Check seg_src

        free(nlh);
    }

    /*-------------------------------------------*/
    /* Test 2: Create and process DEL msg for NH */
    /*-------------------------------------------*/
    {
        /* Create netlink msg header with DEL cmd */
        struct nlmsghdr *nlh_del = createPicContextMsgHdr(RTM_DELPICCONTEXT, id);
        if (!nlh_del) {
            ADD_FAILURE() << "Failed to create Pic Context nlmsghdr";
            return;
        }
        int expected_length = (int)(nlh_del->nlmsg_len - NLMSG_LENGTH(sizeof(struct nhmsg)));

        /* Call the target function */
        m_mockRouteSync.onPicContextMsg(nlh_del, expected_length);

        /* Check the result */
        auto it = m_mockRouteSync.m_nh_groups.find(id);
        ASSERT_EQ(it, m_mockRouteSync.m_nh_groups.end()) << "Failed to remove Pic Context";

        free(nlh_del);
    }

    /*-------------------------------*/
    /* Test 3: Other branches for NH */
    /*-------------------------------*/
    // Case 1: nlmsg_type is nothing to do with PIC
    {
        /* Create netlink msg header with wrong nlmsg_type */
        struct nlmsghdr *nlh_no_pic = createPicContextMsgHdr(RTM_NEWSRV6VPNROUTE);
        if (!nlh_no_pic) {
            ADD_FAILURE() << "Failed to create Pic Context nlmsghdr";
            return;
        }
        int expected_length = (int)(nlh_no_pic->nlmsg_len - NLMSG_LENGTH(sizeof(struct nhmsg)));

        /* Call the target function */
        m_mockRouteSync.onPicContextMsg(nlh_no_pic, expected_length);

        /* Check the result */
        auto it = m_mockRouteSync.m_nh_groups.find(id);
        ASSERT_EQ(it, m_mockRouteSync.m_nh_groups.end()) << "We should've find nothing for no pic case";

        free(nlh_no_pic);
    }

    // Case 2: missing NHA_ID
    {
        /* Create netlink msg header without NHA_ID */
        struct nlmsghdr *nlh_no_id = createPicContextMsgHdr(msg_type, 0);
        if (!nlh_no_id) {
            ADD_FAILURE() << "Failed to create Pic Context nlmsghdr";
            return;
        }
        int expected_length = (int)(nlh_no_id->nlmsg_len - NLMSG_LENGTH(sizeof(struct nhmsg)));

        /* Call the target function */
        m_mockRouteSync.onPicContextMsg(nlh_no_id, expected_length);

        /* Check the result */
        auto it = m_mockRouteSync.m_nh_groups.find(id);
        ASSERT_EQ(it, m_mockRouteSync.m_nh_groups.end()) << "We should've find nothing for no id case";

        free(nlh_no_id);
    }

    // Case 3: has NHA_ENCAP & NHA_ENCAP_TYPE but not LWTUNNEL_ENCAP_SEG6
    {
        /* Create netlink msg header with other encap_type */
        struct nlmsghdr *nlh_wrong_encap = createPicContextMsgHdr(
            msg_type,
            200,
            gateway,
            ifindex,
            nh_family,
            seg6_sid,
            seg6_src,
            __LWTUNNEL_ENCAP_MAX
        );
        if (!nlh_wrong_encap) {
            ADD_FAILURE() << "Failed to create Pic Context nlmsghdr";
            return;
        }
        int expected_length = (int)(nlh_wrong_encap->nlmsg_len - NLMSG_ALIGN(sizeof(struct nhmsg)));

        /* Construct the IfName */
        EXPECT_CALL(m_mockRouteSync, getIfName(ifindex, _, _))
            .WillOnce(DoAll(
                [](int32_t, char *ifname, size_t size) {
                    strncpy(ifname, "Ethernet1", size);
                    ifname[size-1] = '\0';
                },
                Return(true)
            ));

        /* Call the target fuction */
        m_mockRouteSync.onPicContextMsg(nlh_wrong_encap, expected_length);

        /* Check the results */
        auto it = m_mockRouteSync.m_nh_groups.find(200);
        ASSERT_NE(it, m_mockRouteSync.m_nh_groups.end()) << "Failed to add new Pic Context";

        /* Check other attrs */
        const NextHopGroup &nhg = it->second;
        // Check the values
        EXPECT_EQ(nhg.vpn_sid, "");
        EXPECT_EQ(nhg.seg_src, "");

        free(nlh_wrong_encap);
    }

    // Case 4: addr_family == AF_INET
    {
        /* Create netlink msg header for AF_INET case */
        struct nlmsghdr *nlh_ipv4 = createPicContextMsgHdr(
            msg_type,
            300,
            "192.168.0.1",
            ifindex
        );
        if (!nlh_ipv4) {
            ADD_FAILURE() << "Failed to create Pic Context nlmsghdr";
            return;
        }
        int expected_length = (int)(nlh_ipv4->nlmsg_len - NLMSG_ALIGN(sizeof(struct nhmsg)));

        /* Construct the IfName */
        EXPECT_CALL(m_mockRouteSync, getIfName(ifindex, _, _))
            .WillOnce(DoAll(
                [](int32_t, char *ifname, size_t size) {
                    strncpy(ifname, "Ethernet1", size);
                    ifname[size-1] = '\0';
                },
                Return(true)
            ));

        /* Call the target function */
        m_mockRouteSync.onPicContextMsg(nlh_ipv4, expected_length);

        /* Check the results */
        auto it = m_mockRouteSync.m_nh_groups.find(300);
        ASSERT_NE(it, m_mockRouteSync.m_nh_groups.end()) << "Failed to add new Pic Context for ipv4";

        /* Check other attrs */
        const NextHopGroup &nhg = it->second;
        // Check each value of attr
        EXPECT_EQ(nhg.nexthop, "192.168.0.1");
        EXPECT_EQ(nhg.intf, "Ethernet1");

        free(nlh_ipv4);
    }

    // case 5: unknown addr_family type
    {
        /* Create netlink msg header with unknown addr_family type */
        struct nlmsghdr *nlh_unknown_af = createPicContextMsgHdr(
            msg_type,
            id,
            gateway,
            ifindex,
            AF_UNSPEC
        );
        if (!nlh_unknown_af) {
            ADD_FAILURE() << "Failed to create Pic Context nlmsghdr";
            return;
        }
        int expected_length = (int)(nlh_unknown_af->nlmsg_len - NLMSG_LENGTH(sizeof(struct nhmsg)));

        /* Call the target function */
        m_mockRouteSync.onPicContextMsg(nlh_unknown_af, expected_length);

        /* Check the result */
        auto it = m_mockRouteSync.m_nh_groups.find(id);
        ASSERT_EQ(it, m_mockRouteSync.m_nh_groups.end()) << "We should not process the unknown af";

        free(nlh_unknown_af);
    }

    // case 6: ifName does not exist
    {
        /* Create netlink msg header with ADD cmd */
        struct nlmsghdr *nlh_unknown_intf = createPicContextMsgHdr(
            msg_type,
            400,
            gateway,
            ifindex,
            nh_family
        );
        if (!nlh_unknown_intf) {
            ADD_FAILURE() << "Failed to create Pic Context nlmsghdr";
            return;
        }
        int expected_length = (int)(nlh_unknown_intf->nlmsg_len - NLMSG_LENGTH(sizeof(struct nhmsg)));

        /* Construct the IfName, mock unknown case */
        EXPECT_CALL(m_mockRouteSync, getIfName(ifindex, _, _))
            .WillOnce(Return(false));

        /* Call the target function */
        m_mockRouteSync.onPicContextMsg(nlh_unknown_intf, expected_length);

        /* Check the results */
        auto it = m_mockRouteSync.m_nh_groups.find(400);
        ASSERT_NE(it, m_mockRouteSync.m_nh_groups.end()) << "Failed to add new Pic Context";

        /* Check other attrs */
        const NextHopGroup &nhg = it->second;
        // Check each value of attr
        EXPECT_EQ(nhg.nexthop, gateway);
        EXPECT_EQ(nhg.intf, "unknown");

        free(nlh_unknown_intf);
    }

    // case 7: ifName is docker0
    {
        /* Create netlink msg header with ifName "docker0" */
        struct nlmsghdr *nlh_docker0 = createPicContextMsgHdr(
            msg_type,
            id,
            gateway,
            ifindex,
            nh_family
        );
        if (!nlh_docker0) {
            ADD_FAILURE() << "Failed to create Pic Context nlmsghdr";
            return;
        }
        int expected_length = (int)(nlh_docker0->nlmsg_len - NLMSG_LENGTH(sizeof(struct nhmsg)));

        /* Construct the IfName, docker0 */
        EXPECT_CALL(m_mockRouteSync, getIfName(ifindex, _, _))
            .WillOnce(DoAll(
                [](int32_t, char *ifname, size_t size) {
                    strncpy(ifname, "docker0", size);
                    ifname[size-1] = '\0';
                },
                Return(true)
            ));

        /* Call the target function */
        m_mockRouteSync.onPicContextMsg(nlh_docker0, expected_length);

        /* Check the results */
        auto it = m_mockRouteSync.m_nh_groups.find(id);
        ASSERT_EQ(it, m_mockRouteSync.m_nh_groups.end()) << "Failed to skip docker0 case";

        free(nlh_docker0);
    }
}

TEST_F(FpmSyncdResponseTest, TestPicContext_NHG)
{
    // Prepare the information of nexthops
    uint16_t msg_type = RTM_NEWPICCONTEXT;
    uint32_t nh1_id = 1;
    uint32_t nh2_id = 2;
    uint32_t nh3_id = 3;
    const char *gateway_1 = "2001:db8::1";
    const char *gateway_2 = "2002:db8::1";
    const char *gateway_3 = "2003:db8::1";
    uint32_t ifindex_1 = 1;
    uint32_t ifindex_2 = 2;
    uint32_t ifindex_3 = 3;
    unsigned char nh_family = AF_INET6;
    const char *seg6_sid_1 = "2001:db8::2";
    const char *seg6_sid_2 = "2002:db8::2";
    const char *seg6_sid_3 = "2003:db8::2";
    const char *seg6_src_1 = "2001:db8::3";
    const char *seg6_src_2 = "2002:db8::3";
    const char *seg6_src_3 = "2003:db8::3";

    /* First, we need to add the nexthops to m_nh_groups */
    struct nlmsghdr *nlh_1 = createPicContextMsgHdr(msg_type, nh1_id, gateway_1, ifindex_1,
                                                    nh_family, seg6_sid_1, seg6_src_1);
    struct nlmsghdr *nlh_2 = createPicContextMsgHdr(msg_type, nh2_id, gateway_2, ifindex_2,
                                                    nh_family, seg6_sid_2, seg6_src_2);
    struct nlmsghdr *nlh_3 = createPicContextMsgHdr(msg_type, nh3_id, gateway_3, ifindex_3,
                                                    nh_family, seg6_sid_3, seg6_src_3);
    if (!nlh_1 || !nlh_2 || !nlh_3) {
        ADD_FAILURE() << "Failed to create Pic Context nlmsghdr";
        return;
    }

    // Construct the IfName
    EXPECT_CALL(m_mockRouteSync, getIfName(ifindex_1, _, _))
            .WillOnce(DoAll(
                [](int32_t, char *ifname, size_t size) {
                    strncpy(ifname, "Ethernet1", size);
                    ifname[size-1] = '\0';
                },
                Return(true)
            ));
    EXPECT_CALL(m_mockRouteSync, getIfName(ifindex_2, _, _))
            .WillOnce(DoAll(
                [](int32_t, char *ifname, size_t size) {
                    strncpy(ifname, "Ethernet2", size);
                    ifname[size-1] = '\0';
                },
                Return(true)
            ));
    EXPECT_CALL(m_mockRouteSync, getIfName(ifindex_3, _, _))
            .WillOnce(DoAll(
                [](int32_t, char *ifname, size_t size) {
                    strncpy(ifname, "Ethernet3", size);
                    ifname[size-1] = '\0';
                },
                Return(true)
            ));
    // Call onPicContextMsg to insert these nexthops
    m_mockRouteSync.onPicContextMsg(nlh_1, (int)(nlh_1->nlmsg_len - NLMSG_LENGTH(sizeof(struct nhmsg))));
    m_mockRouteSync.onPicContextMsg(nlh_2, (int)(nlh_2->nlmsg_len - NLMSG_LENGTH(sizeof(struct nhmsg))));
    m_mockRouteSync.onPicContextMsg(nlh_3, (int)(nlh_3->nlmsg_len - NLMSG_LENGTH(sizeof(struct nhmsg))));

    /* Create a nexthop group with these nexthops */
    uint32_t group_id = 100;
    vector<pair<uint32_t, uint8_t>> group_members = {
        {nh1_id, 1},  // id=1, weight=1
        {nh2_id, 2},  // id=2, weight=2
        {nh3_id, 3},  // id=3, weight=3
    };

    // Create group_nlh
    struct nlmsghdr* group_nlh = createNewNextHopMsgHdr(group_members, group_id, RTM_NEWPICCONTEXT);
    ASSERT_NE(group_nlh, nullptr) << "Failed to create group nexthop message";

    // Call the target function
    m_mockRouteSync.onPicContextMsg(group_nlh, (int)(group_nlh->nlmsg_len - NLMSG_LENGTH(sizeof(struct nhmsg))));

    // Verify the group was added correctly
    auto it = m_mockRouteSync.m_nh_groups.find(group_id);
    ASSERT_NE(it, m_mockRouteSync.m_nh_groups.end()) << "Failed to add nexthop group";

    // Verify group members
    const auto& group = it->second.group;
    ASSERT_EQ(group.size(), 3) << "Wrong number of group members";

    // Check each member's ID and weight
    EXPECT_EQ(group[0].first, nh1_id);
    EXPECT_EQ(group[0].second, 1);
    EXPECT_EQ(group[1].first, nh2_id);
    EXPECT_EQ(group[1].second, 2);
    EXPECT_EQ(group[2].first, nh3_id);
    EXPECT_EQ(group[2].second, 3);

    // Check values in PIC table
    Table pic_context_group_table(m_db.get(), APP_PIC_CONTEXT_TABLE_NAME);
    vector<FieldValueTuple> fieldValues;
    string key = to_string(group_id);
    pic_context_group_table.get(key, fieldValues);

    ASSERT_EQ(fieldValues.size(), 5) << "Wrong number of fields in DB";

    // Verify the DB fields
    string nexthops, ifnames, sids, srcs, weights;
    for (const auto& fv : fieldValues) {
        if (fvField(fv) == "nexthop") {
            nexthops = fvValue(fv);
        } else if (fvField(fv) == "ifname") {
            ifnames = fvValue(fv);
        } else if (fvField(fv) == "vpn_sid") {
            sids = fvValue(fv);
        } else if (fvField(fv) == "seg_src") {
            srcs = fvValue(fv);
        } else if (fvField(fv) == "weight") {
            weights = fvValue(fv);
        }
    }
    EXPECT_EQ(nexthops, "2001:db8::1,2002:db8::1,2003:db8::1");
    EXPECT_EQ(ifnames, "Ethernet1,Ethernet2,Ethernet3");
    EXPECT_EQ(sids, "2001:db8::2,2002:db8::2,2003:db8::2");
    EXPECT_EQ(srcs, "2001:db8::3,2002:db8::3,2003:db8::3");
    EXPECT_EQ(weights, "1,2,3");

    free(nlh_1);
    free(nlh_2);
    free(nlh_3);
    free(group_nlh);
}

// ============================================================================
// ZMQ-Enabled Integration Test Cases
// ============================================================================
// These tests verify that when ZMQ is enabled, RouteSync writes all fields
// (including empty ones) to the database tables.
// ============================================================================

TEST_F(FpmSyncdResponseTest, TestRouteMsgWithZmqEnabled_AllFieldsIncluded)
{
    // Simulate ZMQ being enabled by setting m_zmqClient to a non-null value
    // We use a dummy shared_ptr (pointing to address 1) since we won't actually use it
    // This makes isNbZmqEnabled() return true
    m_mockRouteSync.m_zmqClient = shared_ptr<swss::ZmqClient>(reinterpret_cast<swss::ZmqClient*>(1), [](swss::ZmqClient*){});

    Table route_table(m_db.get(), APP_ROUTE_TABLE_NAME);

    // Create a simple route
    auto createRoute = [](const char* prefix, uint8_t prefixlen) -> rtnl_route* {
        rtnl_route* route = rtnl_route_alloc();
        nl_addr* dst_addr;
        nl_addr_parse(prefix, AF_INET, &dst_addr);
        rtnl_route_set_dst(route, dst_addr);
        rtnl_route_set_type(route, RTN_UNICAST);
        rtnl_route_set_protocol(route, RTPROT_BGP);
        rtnl_route_set_family(route, AF_INET);
        rtnl_route_set_scope(route, RT_SCOPE_UNIVERSE);
        rtnl_route_set_table(route, RT_TABLE_MAIN);
        nl_addr_put(dst_addr);
        return route;
    };

    const char* test_destipprefix = "10.1.1.0";
    rtnl_route* test_route = createRoute(test_destipprefix, 24);

    // Create a nexthop
    uint32_t test_nh_id = 1;
    struct nlmsghdr* nlh = createNewNextHopMsgHdr(1, test_gateway, test_nh_id);

    EXPECT_CALL(m_mockRouteSync, getIfName(1, _, _))
        .WillOnce(DoAll(
            [](int32_t, char* ifname, size_t size) {
                strncpy(ifname, "Ethernet1", size);
                ifname[size-1] = '\0';
            },
            Return(true)
        ));

    m_mockRouteSync.onNextHopMsg(nlh, (int)(nlh->nlmsg_len - NLMSG_LENGTH(sizeof(struct nhmsg))));
    free(nlh);

    rtnl_route_set_nh_id(test_route, test_nh_id);
    m_mockRouteSync.onRouteMsg(RTM_NEWROUTE, (nl_object*)test_route, nullptr);

    // Verify the route was written to the database
    vector<FieldValueTuple> fvs;
    EXPECT_TRUE(route_table.get(test_destipprefix, fvs));

    // With ZMQ enabled, all 11 fields should be present (including empty ones)
    EXPECT_EQ(fvs.size(), 11);

    // Build a map for easier verification
    std::map<std::string, std::string> fieldMap;
    for (const auto& fv : fvs) {
        fieldMap[fvField(fv)] = fvValue(fv);
    }

    // Verify non-empty fields
    EXPECT_EQ(fieldMap["protocol"], "bgp");
    EXPECT_EQ(fieldMap["nexthop"], test_gateway);
    EXPECT_EQ(fieldMap["ifname"], "Ethernet1");
    EXPECT_EQ(fieldMap["blackhole"], "false");  // Default value

    // Verify empty fields are present
    EXPECT_TRUE(fieldMap.count("nexthop_group") > 0);
    EXPECT_EQ(fieldMap["nexthop_group"], "");
    EXPECT_TRUE(fieldMap.count("mpls_nh") > 0);
    EXPECT_EQ(fieldMap["mpls_nh"], "");
    EXPECT_TRUE(fieldMap.count("weight") > 0);
    EXPECT_EQ(fieldMap["weight"], "");
    EXPECT_TRUE(fieldMap.count("vni_label") > 0);
    EXPECT_EQ(fieldMap["vni_label"], "");
    EXPECT_TRUE(fieldMap.count("router_mac") > 0);
    EXPECT_EQ(fieldMap["router_mac"], "");
    EXPECT_TRUE(fieldMap.count("segment") > 0);
    EXPECT_EQ(fieldMap["segment"], "");
    EXPECT_TRUE(fieldMap.count("seg_src") > 0);
    EXPECT_EQ(fieldMap["seg_src"], "");

    rtnl_route_put(test_route);

    // Reset m_zmqClient to nullptr
    m_mockRouteSync.m_zmqClient = nullptr;
}

TEST_F(FpmSyncdResponseTest, TestRouteMsgWithZmqDisabled_OnlyNonEmptyFields)
{
    // ZMQ is disabled by default in the fixture
    Table route_table(m_db.get(), APP_ROUTE_TABLE_NAME);

    // Create a simple route
    auto createRoute = [](const char* prefix, uint8_t prefixlen) -> rtnl_route* {
        rtnl_route* route = rtnl_route_alloc();
        nl_addr* dst_addr;
        nl_addr_parse(prefix, AF_INET, &dst_addr);
        rtnl_route_set_dst(route, dst_addr);
        rtnl_route_set_type(route, RTN_UNICAST);
        rtnl_route_set_protocol(route, RTPROT_BGP);
        rtnl_route_set_family(route, AF_INET);
        rtnl_route_set_scope(route, RT_SCOPE_UNIVERSE);
        rtnl_route_set_table(route, RT_TABLE_MAIN);
        nl_addr_put(dst_addr);
        return route;
    };

    const char* test_destipprefix = "10.2.2.0";
    rtnl_route* test_route = createRoute(test_destipprefix, 24);

    // Create a nexthop
    uint32_t test_nh_id = 2;
    struct nlmsghdr* nlh = createNewNextHopMsgHdr(1, test_gateway, test_nh_id);

    EXPECT_CALL(m_mockRouteSync, getIfName(1, _, _))
        .WillOnce(DoAll(
            [](int32_t, char* ifname, size_t size) {
                strncpy(ifname, "Ethernet2", size);
                ifname[size-1] = '\0';
            },
            Return(true)
        ));

    m_mockRouteSync.onNextHopMsg(nlh, (int)(nlh->nlmsg_len - NLMSG_LENGTH(sizeof(struct nhmsg))));
    free(nlh);

    rtnl_route_set_nh_id(test_route, test_nh_id);
    m_mockRouteSync.onRouteMsg(RTM_NEWROUTE, (nl_object*)test_route, nullptr);

    // Verify the route was written to the database
    vector<FieldValueTuple> fvs;
    EXPECT_TRUE(route_table.get(test_destipprefix, fvs));

    // With ZMQ disabled, only non-empty fields should be present
    EXPECT_EQ(fvs.size(), 3);  // protocol, nexthop, ifname

    // Build a map for easier verification
    std::map<std::string, std::string> fieldMap;
    for (const auto& fv : fvs) {
        fieldMap[fvField(fv)] = fvValue(fv);
    }

    // Verify the non-empty fields
    EXPECT_EQ(fieldMap["protocol"], "bgp");
    EXPECT_EQ(fieldMap["nexthop"], test_gateway);
    EXPECT_EQ(fieldMap["ifname"], "Ethernet2");

    // Verify empty fields are NOT present
    EXPECT_EQ(fieldMap.count("nexthop_group"), 0);
    EXPECT_EQ(fieldMap.count("mpls_nh"), 0);
    EXPECT_EQ(fieldMap.count("weight"), 0);
    EXPECT_EQ(fieldMap.count("vni_label"), 0);
    EXPECT_EQ(fieldMap.count("router_mac"), 0);
    EXPECT_EQ(fieldMap.count("segment"), 0);
    EXPECT_EQ(fieldMap.count("seg_src"), 0);

    rtnl_route_put(test_route);
}

TEST_F(FpmSyncdResponseTest, TestLabelRouteMsgWithZmqEnabled_AllFieldsIncluded)
{
    // Simulate ZMQ being enabled by setting m_zmqClient to a non-null value
    m_mockRouteSync.m_zmqClient = shared_ptr<swss::ZmqClient>(reinterpret_cast<swss::ZmqClient*>(1), [](swss::ZmqClient*){});

    Table label_route_table(m_db.get(), APP_LABEL_ROUTE_TABLE_NAME);

    // Create a label route (AF_MPLS)
    auto createLabelRoute = [](const char* prefix, uint8_t prefixlen) -> rtnl_route* {
        rtnl_route* route = rtnl_route_alloc();
        nl_addr* dst_addr;
        nl_addr_parse(prefix, AF_MPLS, &dst_addr);
        rtnl_route_set_dst(route, dst_addr);
        rtnl_route_set_type(route, RTN_UNICAST);
        rtnl_route_set_protocol(route, RTPROT_STATIC);
        rtnl_route_set_family(route, AF_MPLS);
        rtnl_route_set_scope(route, RT_SCOPE_UNIVERSE);
        rtnl_route_set_table(route, RT_TABLE_UNSPEC);  // Label routes must use RT_TABLE_UNSPEC
        nl_addr_put(dst_addr);
        return route;
    };

    const char* test_label = "100";
    rtnl_route* test_route = createLabelRoute(test_label, 20);

    // Create a nexthop for the label route
    rtnl_nexthop* nh = rtnl_route_nh_alloc();
    rtnl_route_nh_set_ifindex(nh, 1);
    nl_addr* gateway_addr;
    nl_addr_parse("10.0.0.1", AF_INET, &gateway_addr);
    rtnl_route_nh_set_gateway(nh, gateway_addr);
    nl_addr_put(gateway_addr);
    rtnl_route_add_nexthop(test_route, nh);

    // Mock getIfName to return interface name
    EXPECT_CALL(m_mockRouteSync, getIfName(1, _, _))
        .WillOnce(DoAll(
            [](int32_t, char* ifname, size_t size) {
                strncpy(ifname, "Ethernet0", size);
                ifname[size-1] = '\0';
            },
            Return(true)
        ));

    m_mockRouteSync.onLabelRouteMsg(RTM_NEWROUTE, (nl_object*)test_route);

    // Verify the label route was written to the database
    vector<FieldValueTuple> fvs;
    EXPECT_TRUE(label_route_table.get(test_label, fvs));

    // With ZMQ enabled, all 6 fields should be present (including empty ones)
    EXPECT_EQ(fvs.size(), 6);

    // Build a map for easier verification
    std::map<std::string, std::string> fieldMap;
    for (const auto& fv : fvs) {
        fieldMap[fvField(fv)] = fvValue(fv);
    }

    // Verify all fields are present
    EXPECT_TRUE(fieldMap.find("protocol") != fieldMap.end());
    EXPECT_EQ(fieldMap["protocol"], "static");

    EXPECT_TRUE(fieldMap.find("blackhole") != fieldMap.end());
    EXPECT_EQ(fieldMap["blackhole"], "false");

    EXPECT_TRUE(fieldMap.find("nexthop") != fieldMap.end());
    EXPECT_EQ(fieldMap["nexthop"], "10.0.0.1");

    EXPECT_TRUE(fieldMap.find("ifname") != fieldMap.end());
    EXPECT_EQ(fieldMap["ifname"], "Ethernet0");

    // mpls_pop is always set to "1" for label routes
    EXPECT_TRUE(fieldMap.find("mpls_pop") != fieldMap.end());
    EXPECT_EQ(fieldMap["mpls_pop"], "1");

    // mpls_nh should be present but empty
    EXPECT_TRUE(fieldMap.find("mpls_nh") != fieldMap.end());
    EXPECT_EQ(fieldMap["mpls_nh"], "");

    // Reset m_zmqClient to nullptr
    m_mockRouteSync.m_zmqClient = nullptr;

    rtnl_route_put(test_route);
}

TEST_F(FpmSyncdResponseTest, TestLabelRouteMsgWithZmqDisabled_OnlyNonEmptyFields)
{
    // ZMQ is disabled by default in the fixture
    Table label_route_table(m_db.get(), APP_LABEL_ROUTE_TABLE_NAME);

    // Create a label route (AF_MPLS)
    auto createLabelRoute = [](const char* prefix, uint8_t prefixlen) -> rtnl_route* {
        rtnl_route* route = rtnl_route_alloc();
        nl_addr* dst_addr;
        nl_addr_parse(prefix, AF_MPLS, &dst_addr);
        rtnl_route_set_dst(route, dst_addr);
        rtnl_route_set_type(route, RTN_UNICAST);
        rtnl_route_set_protocol(route, RTPROT_STATIC);
        rtnl_route_set_family(route, AF_MPLS);
        rtnl_route_set_scope(route, RT_SCOPE_UNIVERSE);
        rtnl_route_set_table(route, RT_TABLE_UNSPEC);  // Label routes must use RT_TABLE_UNSPEC
        nl_addr_put(dst_addr);
        return route;
    };

    const char* test_label = "200";
    rtnl_route* test_route = createLabelRoute(test_label, 20);

    // Create a nexthop for the label route
    rtnl_nexthop* nh = rtnl_route_nh_alloc();
    rtnl_route_nh_set_ifindex(nh, 2);
    nl_addr* gateway_addr;
    nl_addr_parse("10.0.0.2", AF_INET, &gateway_addr);
    rtnl_route_nh_set_gateway(nh, gateway_addr);
    nl_addr_put(gateway_addr);
    rtnl_route_add_nexthop(test_route, nh);

    // Mock getIfName to return interface name
    EXPECT_CALL(m_mockRouteSync, getIfName(2, _, _))
        .WillOnce(DoAll(
            [](int32_t, char* ifname, size_t size) {
                strncpy(ifname, "Ethernet1", size);
                ifname[size-1] = '\0';
            },
            Return(true)
        ));

    m_mockRouteSync.onLabelRouteMsg(RTM_NEWROUTE, (nl_object*)test_route);

    // Verify the label route was written to the database
    vector<FieldValueTuple> fvs;
    EXPECT_TRUE(label_route_table.get(test_label, fvs));

    // With ZMQ disabled, only non-empty fields should be present
    // protocol, nexthop, ifname, mpls_pop (always set to "1")
    // Note: blackhole is "false" by default and is excluded when ZMQ is disabled
    EXPECT_EQ(fvs.size(), 4);

    // Build a map for easier verification
    std::map<std::string, std::string> fieldMap;
    for (const auto& fv : fvs) {
        fieldMap[fvField(fv)] = fvValue(fv);
    }

    // Verify only non-empty fields are present
    EXPECT_TRUE(fieldMap.find("protocol") != fieldMap.end());
    EXPECT_EQ(fieldMap["protocol"], "static");

    EXPECT_TRUE(fieldMap.find("nexthop") != fieldMap.end());
    EXPECT_EQ(fieldMap["nexthop"], "10.0.0.2");

    EXPECT_TRUE(fieldMap.find("ifname") != fieldMap.end());
    EXPECT_EQ(fieldMap["ifname"], "Ethernet1");

    // mpls_pop is always set to "1" for label routes
    EXPECT_TRUE(fieldMap.find("mpls_pop") != fieldMap.end());
    EXPECT_EQ(fieldMap["mpls_pop"], "1");

    // blackhole is "false" by default and is excluded when ZMQ is disabled
    EXPECT_TRUE(fieldMap.find("blackhole") == fieldMap.end());

    // mpls_nh should NOT be present when ZMQ is disabled and it's empty
    EXPECT_TRUE(fieldMap.find("mpls_nh") == fieldMap.end());

    rtnl_route_put(test_route);
}

// Note: SRv6 VPN routes (onSrv6VpnRouteMsg) do not use RouteTableFieldValueTupleWrapper
// They directly create field-value tuples, so ZMQ flag doesn't affect them the same way.
// Therefore, we don't add ZMQ tests for SRv6 VPN routes here.

TEST_F(FpmSyncdResponseTest, TestVnetRouteMsgWithZmqEnabled_AllFieldsIncluded)
{
    // Simulate ZMQ being enabled by setting m_zmqClient to a non-null value
    m_mockRouteSync.m_zmqClient = shared_ptr<swss::ZmqClient>(reinterpret_cast<swss::ZmqClient*>(1), [](swss::ZmqClient*){});

    Table vnet_route_table(m_db.get(), APP_VNET_RT_TABLE_NAME);

    // Create a VNET route
    auto createVnetRoute = [](const char* prefix, uint8_t prefixlen, uint32_t vnet_table_id) -> rtnl_route* {
        rtnl_route* route = rtnl_route_alloc();
        nl_addr* dst_addr;
        nl_addr_parse(prefix, AF_INET, &dst_addr);
        rtnl_route_set_dst(route, dst_addr);
        rtnl_route_set_type(route, RTN_UNICAST);
        rtnl_route_set_protocol(route, RTPROT_STATIC);
        rtnl_route_set_family(route, AF_INET);
        rtnl_route_set_scope(route, RT_SCOPE_UNIVERSE);
        rtnl_route_set_table(route, vnet_table_id);  // Set VNET table ID
        nl_addr_put(dst_addr);
        return route;
    };

    const char* test_destipprefix = "192.168.1.0/24";
    uint32_t vnet_table_id = 1000;
    rtnl_route* test_route = createVnetRoute(test_destipprefix, 24, vnet_table_id);

    // Create a nexthop for the VNET route
    rtnl_nexthop* nh = rtnl_route_nh_alloc();
    rtnl_route_nh_set_ifindex(nh, 10);
    nl_addr* gateway_addr;
    nl_addr_parse("192.168.1.1", AF_INET, &gateway_addr);
    rtnl_route_nh_set_gateway(nh, gateway_addr);
    nl_addr_put(gateway_addr);
    rtnl_route_add_nexthop(test_route, nh);

    // Mock getIfName to return VNET interface name (starts with "Vnet")
    EXPECT_CALL(m_mockRouteSync, getIfName(vnet_table_id, _, _))
        .WillOnce(DoAll(
            [](int32_t, char* ifname, size_t size) {
                strncpy(ifname, "Vnet100", size);  // VNET_PREFIX is "Vnet"
                ifname[size-1] = '\0';
            },
            Return(true)
        ));

    // Mock getIfName for the nexthop interface
    EXPECT_CALL(m_mockRouteSync, getIfName(10, _, _))
        .WillOnce(DoAll(
            [](int32_t, char* ifname, size_t size) {
                strncpy(ifname, "Ethernet100", size);
                ifname[size-1] = '\0';
            },
            Return(true)
        ));

    m_mockRouteSync.onMsg(RTM_NEWROUTE, (nl_object*)test_route);

    // Verify the VNET route was written to the database
    vector<FieldValueTuple> fvs;
    std::string key = "Vnet100:192.168.1.0/24";
    EXPECT_TRUE(vnet_route_table.get(key, fvs));

    // With ZMQ enabled, all 2 fields should be present (including empty ones if any)
    EXPECT_EQ(fvs.size(), 2);

    // Build a map for easier verification
    std::map<std::string, std::string> fieldMap;
    for (const auto& fv : fvs) {
        fieldMap[fvField(fv)] = fvValue(fv);
    }

    // Verify all fields are present
    EXPECT_TRUE(fieldMap.find("nexthop") != fieldMap.end());
    EXPECT_EQ(fieldMap["nexthop"], "192.168.1.1");

    EXPECT_TRUE(fieldMap.find("ifname") != fieldMap.end());
    EXPECT_EQ(fieldMap["ifname"], "Ethernet100");

    // Reset m_zmqClient to nullptr
    m_mockRouteSync.m_zmqClient = nullptr;

    rtnl_route_put(test_route);
}

TEST_F(FpmSyncdResponseTest, TestVnetRouteMsgWithZmqDisabled_OnlyNonEmptyFields)
{
    // ZMQ is disabled by default in the fixture
    Table vnet_route_table(m_db.get(), APP_VNET_RT_TABLE_NAME);

    // Create a VNET route with only ifname (no nexthop gateway)
    auto createVnetRoute = [](const char* prefix, uint8_t prefixlen, uint32_t vnet_table_id) -> rtnl_route* {
        rtnl_route* route = rtnl_route_alloc();
        nl_addr* dst_addr;
        nl_addr_parse(prefix, AF_INET, &dst_addr);
        rtnl_route_set_dst(route, dst_addr);
        rtnl_route_set_type(route, RTN_UNICAST);
        rtnl_route_set_protocol(route, RTPROT_STATIC);
        rtnl_route_set_family(route, AF_INET);
        rtnl_route_set_scope(route, RT_SCOPE_LINK);  // Link scope for direct routes
        rtnl_route_set_table(route, vnet_table_id);  // Set VNET table ID
        nl_addr_put(dst_addr);
        return route;
    };

    const char* test_destipprefix = "192.168.2.0/24";
    uint32_t vnet_table_id = 2000;
    rtnl_route* test_route = createVnetRoute(test_destipprefix, 24, vnet_table_id);

    // Create a nexthop without gateway (direct route)
    rtnl_nexthop* nh = rtnl_route_nh_alloc();
    rtnl_route_nh_set_ifindex(nh, 20);
    rtnl_route_add_nexthop(test_route, nh);

    // Mock getIfName to return VNET interface name (starts with "Vnet")
    EXPECT_CALL(m_mockRouteSync, getIfName(vnet_table_id, _, _))
        .WillOnce(DoAll(
            [](int32_t, char* ifname, size_t size) {
                strncpy(ifname, "Vnet200", size);  // VNET_PREFIX is "Vnet"
                ifname[size-1] = '\0';
            },
            Return(true)
        ));

    // Mock getIfName for the nexthop interface
    EXPECT_CALL(m_mockRouteSync, getIfName(20, _, _))
        .WillOnce(DoAll(
            [](int32_t, char* ifname, size_t size) {
                strncpy(ifname, "Ethernet200", size);
                ifname[size-1] = '\0';
            },
            Return(true)
        ));

    m_mockRouteSync.onMsg(RTM_NEWROUTE, (nl_object*)test_route);

    // Verify the VNET route was written to the database
    vector<FieldValueTuple> fvs;
    std::string key = "Vnet200:192.168.2.0/24";
    EXPECT_TRUE(vnet_route_table.get(key, fvs));

    // With ZMQ disabled, only non-empty fields should be present
    // Note: Even for direct routes, getNextHopGw returns "0.0.0.0" for IPv4,
    // so nexthop field will be present with that value
    EXPECT_EQ(fvs.size(), 2);

    // Build a map for easier verification
    std::map<std::string, std::string> fieldMap;
    for (const auto& fv : fvs) {
        fieldMap[fvField(fv)] = fvValue(fv);
    }

    // Verify both fields are present
    EXPECT_TRUE(fieldMap.find("ifname") != fieldMap.end());
    EXPECT_EQ(fieldMap["ifname"], "Ethernet200");

    EXPECT_TRUE(fieldMap.find("nexthop") != fieldMap.end());
    EXPECT_EQ(fieldMap["nexthop"], "0.0.0.0");

    rtnl_route_put(test_route);
}
