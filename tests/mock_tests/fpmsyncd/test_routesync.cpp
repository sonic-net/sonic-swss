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

        EXPECT_EQ(fieldValues.size(), 3);
        EXPECT_EQ(fvField(fieldValues[0]), "nexthop");
        EXPECT_EQ(fvValue(fieldValues[0]), test_gateway);
        EXPECT_EQ(fvField(fieldValues[1]), "ifname");
        EXPECT_EQ(fvValue(fieldValues[1]), "Ethernet0");
        EXPECT_EQ(fvField(fieldValues[2]), "weight");
        EXPECT_EQ(fvValue(fieldValues[2]), "");
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

        EXPECT_EQ(fieldValues.size(), 3);
        EXPECT_EQ(fvField(fieldValues[0]), "nexthop");
        EXPECT_EQ(fvValue(fieldValues[0]), "0.0.0.0");
        EXPECT_EQ(fvField(fieldValues[1]), "ifname");
        EXPECT_EQ(fvValue(fieldValues[1]), "Ethernet0");
        EXPECT_EQ(fvField(fieldValues[2]), "weight");
        EXPECT_EQ(fvValue(fieldValues[2]), "");
    }

    // Empty interface name
    {
        NextHopGroup nhg(5, test_gateway, "");
        m_mockRouteSync.updateNextHopGroupDb(nhg);

        vector<FieldValueTuple> fieldValues;
        nexthop_group_table.get("5", fieldValues);

        EXPECT_EQ(fieldValues.size(), 3);
        EXPECT_EQ(fvField(fieldValues[0]), "nexthop");
        EXPECT_EQ(fvValue(fieldValues[0]), test_gateway);
        EXPECT_EQ(fvField(fieldValues[1]), "ifname");
        EXPECT_EQ(fvValue(fieldValues[1]), "");
        EXPECT_EQ(fvField(fieldValues[2]), "weight");
        EXPECT_EQ(fvValue(fieldValues[2]), "");
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

struct nlmsghdr* createNewNextHopMsgHdr(const vector<pair<uint32_t, uint8_t>>& group_members, uint32_t id) {
    struct nlmsghdr *nlh = (struct nlmsghdr *)malloc(NLMSG_SPACE(MAX_PAYLOAD));
    memset(nlh, 0, NLMSG_SPACE(MAX_PAYLOAD));

    // Set header
    nlh->nlmsg_type = RTM_NEWNEXTHOP;
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
        EXPECT_EQ(fvs.size(), 11);
        for (const auto& fv : fvs) {
            if (fvField(fv) == "nexthop") {
                EXPECT_EQ(fvValue(fv), test_gateway);
            } else if (fvField(fv) == "ifname") {
                EXPECT_EQ(fvValue(fv), "Ethernet1");
            } else if (fvField(fv) == "protocol") {
                EXPECT_EQ(fvValue(fv), "static");
            } else if (fvField(fv) == "blackhole") {
                EXPECT_EQ(fvValue(fv), "false");
            } else if (fvField(fv) == "nexthop_group") {
                EXPECT_EQ(fvValue(fv), "");
            } else if (fvField(fv) == "mpls_nh") {
                EXPECT_EQ(fvValue(fv), "");
            } else if (fvField(fv) == "weight") {
                EXPECT_EQ(fvValue(fv), "");
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
