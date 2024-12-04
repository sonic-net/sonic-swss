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

int rt_build_ret = 0;
bool nlmsg_alloc_ret = true;
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

struct nlmsghdr* createNewNextHopMsgHdr(int32_t ifindex, const char* gateway, uint32_t id) {
    struct nlmsghdr *nlh = (struct nlmsghdr *)malloc(NLMSG_SPACE(MAX_PAYLOAD));
    memset(nlh, 0, NLMSG_SPACE(MAX_PAYLOAD));

    // 设置基本header
    nlh->nlmsg_type = RTM_NEWNEXTHOP;
    nlh->nlmsg_flags = NLM_F_REQUEST | NLM_F_CREATE | NLM_F_REPLACE;
    nlh->nlmsg_len = NLMSG_LENGTH(sizeof(struct nhmsg));
    
    printf("After header setup - nlmsg_len: %d\n", nlh->nlmsg_len);

    // 设置nhmsg
    struct nhmsg *nhm = (struct nhmsg *)NLMSG_DATA(nlh);
    nhm->nh_family = AF_INET;

    // 先添加 NHA_ID
    struct rtattr *rta = (struct rtattr *)((char *)nlh + NLMSG_ALIGN(nlh->nlmsg_len));
    rta->rta_type = NHA_ID;
    rta->rta_len = RTA_LENGTH(sizeof(uint32_t));
    *(uint32_t *)RTA_DATA(rta) = id;
    nlh->nlmsg_len = NLMSG_ALIGN(nlh->nlmsg_len) + RTA_ALIGN(rta->rta_len);
    
    printf("After NHA_ID - nlmsg_len: %d\n", nlh->nlmsg_len);

    // 添加 NHA_GATEWAY
    rta = (struct rtattr *)((char *)nlh + NLMSG_ALIGN(nlh->nlmsg_len));
    struct in_addr gw_addr;
    inet_pton(AF_INET, gateway, &gw_addr);
    rta->rta_type = NHA_GATEWAY;
    rta->rta_len = RTA_LENGTH(sizeof(struct in_addr));
    memcpy(RTA_DATA(rta), &gw_addr, sizeof(struct in_addr));
    nlh->nlmsg_len = NLMSG_ALIGN(nlh->nlmsg_len) + RTA_ALIGN(rta->rta_len);
    
    printf("After NHA_GATEWAY - nlmsg_len: %d\n", nlh->nlmsg_len);

    // 添加 NHA_OIF
    rta = (struct rtattr *)((char *)nlh + NLMSG_ALIGN(nlh->nlmsg_len));
    rta->rta_type = NHA_OIF;
    rta->rta_len = RTA_LENGTH(sizeof(int32_t));
    *(int32_t *)RTA_DATA(rta) = ifindex;
    nlh->nlmsg_len = NLMSG_ALIGN(nlh->nlmsg_len) + RTA_ALIGN(rta->rta_len);
    
    printf("After NHA_OIF - final nlmsg_len: %d\n", nlh->nlmsg_len);

    return nlh;
}

void dump_nexthop_msg(struct nlmsghdr *nlh) {
    printf("\nDumping nexthop message:\n");
    printf("nlmsg_len: %d\n", nlh->nlmsg_len);
    printf("nlmsg_type: %d\n", nlh->nlmsg_type);
    
    struct nhmsg *nhm = (struct nhmsg *)NLMSG_DATA(nlh);
    printf("nh_family: %d\n", nhm->nh_family);
    
    struct rtattr *rta = (struct rtattr *)((char *)nhm + NLMSG_ALIGN(sizeof(*nhm)));
    int len = nlh->nlmsg_len - (int)NLMSG_LENGTH(sizeof(*nhm));
    
    while (RTA_OK(rta, len)) {
        printf("Attribute type: %d, len: %d\n", rta->rta_type, rta->rta_len);
        if (rta->rta_type == NHA_OIF) {
            printf("  OIF value: %d\n", *(int32_t *)RTA_DATA(rta));
        }
        rta = RTA_NEXT(rta, len);
    }
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

TEST_F(FpmSyncdResponseTest, TestSingleNextHopAdd)
{
    uint32_t test_id = 10;
    const char* test_gateway = "192.168.1.1";
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

TEST_F(FpmSyncdResponseTest, TestSkipSpecialInterfaces)
{
    uint32_t test_id = 11;
    const char* test_gateway = "192.168.1.1";
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
    dump_nexthop_msg(nlh);
    m_mockRouteSync.onNextHopMsg(nlh, expected_length);

    auto it = m_mockRouteSync.m_nh_groups.find(test_id);
    EXPECT_EQ(it, m_mockRouteSync.m_nh_groups.end()) << "Should skip eth0 interface";

    free(nlh);
}

TEST_F(FpmSyncdResponseTest, TestNextHopGroupKeyString)
{
    EXPECT_EQ(m_mockRouteSync.getNextHopGroupKeyAsString(1), "ID1");
    EXPECT_EQ(m_mockRouteSync.getNextHopGroupKeyAsString(1234), "ID1234");
}

TEST_F(FpmSyncdResponseTest, TestGetNextHopGroupFields)
{
    // Test single next hop case
    {
        NextHopGroup nhg(1, "192.168.1.1", "Ethernet0");
        m_mockRouteSync.m_nh_groups.insert({1, nhg});
        
        string nexthops, ifnames, weights;
        m_mockRouteSync.getNextHopGroupFields(nhg, nexthops, ifnames, weights);
        
        EXPECT_EQ(nexthops, "192.168.1.1");
        EXPECT_EQ(ifnames, "Ethernet0");
        EXPECT_TRUE(weights.empty());
    }

    // Test multiple next hops with weights
    {
        // Create the component next hops first
        NextHopGroup nhg1(1, "192.168.1.1", "Ethernet0");
        NextHopGroup nhg2(2, "192.168.1.2", "Ethernet1");
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
        NextHopGroup nhg(1, "192.168.1.1", "Ethernet0");
        m_mockRouteSync.updateNextHopGroupDb(nhg);

        vector<FieldValueTuple> fieldValues;
        nexthop_group_table.get("ID1", fieldValues);
        
        EXPECT_EQ(fieldValues.size(), 2);
        EXPECT_EQ(fvField(fieldValues[0]), "nexthop");
        EXPECT_EQ(fvValue(fieldValues[0]), "192.168.1.1");
        EXPECT_EQ(fvField(fieldValues[1]), "ifname");
        EXPECT_EQ(fvValue(fieldValues[1]), "Ethernet0");
    }

    // Test group with multiple next hops
    {
        vector<pair<uint32_t,uint8_t>> group_members;
        group_members.push_back(make_pair(1, 1));
        group_members.push_back(make_pair(2, 2));
        
        NextHopGroup nhg1(1, "192.168.1.1", "Ethernet0");
        NextHopGroup nhg2(2, "192.168.1.2", "Ethernet1");
        NextHopGroup group(3, group_members);
        
        m_mockRouteSync.m_nh_groups.insert({1, nhg1});
        m_mockRouteSync.m_nh_groups.insert({2, nhg2});
        m_mockRouteSync.m_nh_groups.insert({3, group});
        
        m_mockRouteSync.updateNextHopGroup(3);
        
        auto it = m_mockRouteSync.m_nh_groups.find(3);
        ASSERT_NE(it, m_mockRouteSync.m_nh_groups.end());
        EXPECT_TRUE(it->second.installed);
        vector<FieldValueTuple> fieldValues;
        nexthop_group_table.get("ID3", fieldValues);
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
        nexthop_group_table.get("ID4", fieldValues);
        
        EXPECT_EQ(fieldValues.size(), 2);
        EXPECT_EQ(fvField(fieldValues[0]), "nexthop");
        EXPECT_EQ(fvValue(fieldValues[0]), "0.0.0.0");
        EXPECT_EQ(fvField(fieldValues[1]), "ifname");
        EXPECT_EQ(fvValue(fieldValues[1]), "Ethernet0");
    }

    // Empty interface name
    {
        NextHopGroup nhg(5, "192.168.1.1", "");
        m_mockRouteSync.updateNextHopGroupDb(nhg);
        
        vector<FieldValueTuple> fieldValues;
        nexthop_group_table.get("ID5", fieldValues);
        
        EXPECT_EQ(fieldValues.size(), 2);
        EXPECT_EQ(fvField(fieldValues[0]), "nexthop");
        EXPECT_EQ(fvValue(fieldValues[0]), "192.168.1.1");
        EXPECT_EQ(fvField(fieldValues[1]), "ifname");
        EXPECT_EQ(fvValue(fieldValues[1]), "");
    }
}

TEST_F(FpmSyncdResponseTest, TestDeleteNextHopGroup)
{
    // Setup test groups
    NextHopGroup nhg1(1, "192.168.1.1", "Ethernet0");
    NextHopGroup nhg2(2, "192.168.1.2", "Ethernet1");
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
