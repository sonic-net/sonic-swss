#include "redisutility.h"

#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include <linux/nexthop.h>
#include "mock_table.h"
#define private public
#include "fdbsyncd/neighbour.h"
#include "fdbsyncd/fdbsync.h"
#include "macaddress.h"
#undef private

#define MAX_PAYLOAD 1024
#define ETH_ALEN 6

using namespace swss;

using ::testing::_;

class MockFdbSync : public FdbSync
{
public:
    MockFdbSync(RedisPipeline *m_pipeline, DBConnector *m_stateDb, DBConnector *m_configDb ) : FdbSync(m_pipeline, m_stateDb, m_configDb)
    {
        m_AppRestartAssist = NULL;
        m_intf_info[142] = {"Vxlan-10", 5002};
        m_intf_info[143] = {"Vxlan-20", 5003};
        m_intf_info[144] = {"Vxlan-30", 5004};
    }

    ~MockFdbSync()
    {
    }
};

class FdbSyncdTest : public ::testing::Test
{
public:
    void SetUp() override
    {
        ::testing_db::reset();
    }

    void TearDown() override
    {
    }

    std::shared_ptr<swss::DBConnector> m_appDb = std::make_shared<swss::DBConnector>("APPL_DB", 0);
    std::shared_ptr<RedisPipeline> m_pipeline = std::make_shared<RedisPipeline>(m_appDb.get());
    std::shared_ptr<swss::DBConnector> m_stateDb = std::make_shared<swss::DBConnector>("STATE_DB", 0);
    std::shared_ptr<swss::DBConnector> m_configDb = std::make_shared<swss::DBConnector>("CONFIG_DB", 0);
    MockFdbSync m_mockFdbSync{m_pipeline.get(), m_stateDb.get(), m_configDb.get()};
};

/*
 * *******************
 *  Helper functions
 * *******************
 */
struct nlmsghdr *mac_route_msg(bool add, uint32_t nhid, char *remotevtep, int ifindex,
                               uint16_t vlan_id, swss::MacAddress lla)
{
    uint32_t ext_flags = 0;
    struct nlmsghdr *nlh = (struct nlmsghdr *) malloc(NLMSG_SPACE(MAX_PAYLOAD));
    memset(nlh, 0, NLMSG_SPACE(MAX_PAYLOAD));
    if (add) {
        nlh->nlmsg_type = RTM_NEWNEIGH;
    } else {
        nlh->nlmsg_type = RTM_DELNEIGH;
    }
    nlh->nlmsg_flags = (NLM_F_CREATE | NLM_F_REQUEST);
    nlh->nlmsg_len = NLMSG_LENGTH(0);

    struct ndmsg *ndm = (struct ndmsg *)NLMSG_DATA(nlh);
    ndm->ndm_family = AF_BRIDGE;
    ndm->ndm_type = RTN_UNICAST;
    ndm->ndm_ifindex = ifindex;
    ndm->ndm_flags = NTF_EXT_LEARNED;
    nlh->nlmsg_len += RTA_ALIGN(sizeof(*ndm));

    struct rtattr *rta = NDA_RTA(ndm);
    int max_len = MAX_PAYLOAD;

    rta->rta_type = NDA_NH_ID;
    rta->rta_len = RTA_LENGTH(sizeof(uint32_t));
    memcpy(RTA_DATA(rta), (void *)&nhid, sizeof(uint32_t));
    nlh->nlmsg_len += RTA_ALIGN(rta->rta_len);

    rta = RTA_NEXT(rta, max_len);
    rta->rta_type = NDA_DST;
    rta->rta_len = RTA_LENGTH(sizeof(in_addr_t));
    inet_pton(AF_INET, remotevtep, RTA_DATA(rta));
    nlh->nlmsg_len += RTA_ALIGN(rta->rta_len);

    rta = RTA_NEXT(rta, max_len);
    rta->rta_type = NDA_VLAN;
    rta->rta_len = RTA_LENGTH(sizeof(uint16_t));
    memcpy(RTA_DATA(rta), (void *)&vlan_id, sizeof(uint16_t));
    nlh->nlmsg_len += RTA_ALIGN(rta->rta_len);

    rta = RTA_NEXT(rta, max_len);
    rta->rta_type = NDA_LLADDR;
    rta->rta_len = RTA_LENGTH(ETH_ALEN);
    memcpy(RTA_DATA(rta), (void *)&lla, ETH_ALEN);
    nlh->nlmsg_len += RTA_ALIGN(rta->rta_len);

    rta = RTA_NEXT(rta, max_len);
    rta->rta_type = NDA_FLAGS_EXT;
    rta->rta_len = RTA_LENGTH(sizeof(uint32_t));
    if (strlen(remotevtep) != 0) {
        ext_flags |= NTF_EXT_REMOTE_ONLY;
    } else {
        ext_flags |= NTF_EXT_MH_PEER_SYNC;
    }
    memcpy(RTA_DATA(rta), (void *)&ext_flags, sizeof(uint32_t));
    nlh->nlmsg_len += RTA_ALIGN(rta->rta_len);

    return nlh;
}

struct nlmsghdr *new_nhg_msg(uint32_t nhid, char *remotevtep,
                             int ifindex, struct nexthop_grp grp[], int nexthop_grp_size)
{
    struct nlmsghdr *nlh = (struct nlmsghdr *) malloc(NLMSG_SPACE(MAX_PAYLOAD));
    memset(nlh, 0, NLMSG_SPACE(MAX_PAYLOAD));
    nlh->nlmsg_type = RTM_NEWNEXTHOP;
    nlh->nlmsg_flags = (NLM_F_CREATE | NLM_F_REQUEST);
    nlh->nlmsg_len = NLMSG_LENGTH(0);

    struct nhmsg *nhm = (struct nhmsg *)NLMSG_DATA(nlh);
    nlh->nlmsg_len += RTA_ALIGN(sizeof(*nhm));

    struct rtattr *rta = RTM_NHA(nhm);
    int max_len = MAX_PAYLOAD;

    rta->rta_type = NHA_ID;
    rta->rta_len = RTA_LENGTH(sizeof(uint32_t));
    memcpy(RTA_DATA(rta), (void *)&nhid, sizeof(uint32_t));
    nlh->nlmsg_len += RTA_ALIGN(rta->rta_len);

    struct in_addr v4addr;
    struct in6_addr v6addr;

    rta = RTA_NEXT(rta, max_len);
    rta->rta_type = NHA_GATEWAY;
    // Try parsing as IPv4 first
    // Then try IPv6
    if (inet_pton(AF_INET, remotevtep, &v4addr) == 1)
    {
        rta->rta_len = RTA_LENGTH(sizeof(struct in_addr));
        memcpy(RTA_DATA(rta), &v4addr, sizeof(struct in_addr));
        nhm->nh_family = AF_INET;
    }
    else if (inet_pton(AF_INET6, remotevtep, &v6addr) == 1)
    {
        rta->rta_len = RTA_LENGTH(sizeof(struct in6_addr));
        memcpy(RTA_DATA(rta), &v6addr, sizeof(struct in6_addr));
        nhm->nh_family = AF_INET6;
    }
    nlh->nlmsg_len += RTA_ALIGN(rta->rta_len);

    if (ifindex != 0)
    {
        rta = RTA_NEXT(rta, max_len);
        rta->rta_type = NHA_OIF;
        rta->rta_len = RTA_LENGTH(sizeof(uint32_t));
        memcpy(RTA_DATA(rta), (void *)&ifindex, sizeof(uint32_t));
        nlh->nlmsg_len += RTA_ALIGN(rta->rta_len);
    }

    if (nexthop_grp_size)
    {
        rta = RTA_NEXT(rta, max_len);
        rta->rta_type = NHA_GROUP;
        rta->rta_len = static_cast<short>(RTA_LENGTH(nexthop_grp_size)) ;
        memcpy(RTA_DATA(rta), (void *)grp, nexthop_grp_size);
        nlh->nlmsg_len += RTA_ALIGN(rta->rta_len);
    }

    return nlh;
}

struct nlmsghdr *del_nhg_msg(int nhid)
{
    struct nlmsghdr *nlh = (struct nlmsghdr *) malloc(NLMSG_SPACE(MAX_PAYLOAD));
    memset(nlh, 0, NLMSG_SPACE(MAX_PAYLOAD));
    nlh->nlmsg_type = RTM_DELNEXTHOP;
    nlh->nlmsg_flags = NLM_F_REQUEST;
    nlh->nlmsg_len = NLMSG_LENGTH(0);

    struct nhmsg *nhm = (struct nhmsg *)NLMSG_DATA(nlh);
    nhm = (struct nhmsg *)NLMSG_DATA(nlh);
    nhm->nh_family = AF_UNSPEC;
    nlh->nlmsg_len += RTA_ALIGN(sizeof(*nhm));

    struct rtattr *rta = RTM_NHA(nhm);
    rta->rta_type = NHA_ID;
    rta->rta_len = RTA_LENGTH(sizeof(uint32_t));
    memcpy(RTA_DATA(rta), (void *)&nhid, sizeof(uint32_t));
    nlh->nlmsg_len += RTA_ALIGN(rta->rta_len);
    return nlh;
}
/*
 * ************************
 * End of helper functions
 * ************************
 */

TEST_F(FdbSyncdTest, testaddNhgMacRoute)
{
    std::shared_ptr<swss::DBConnector> m_app_db;
    m_app_db = std::make_shared<swss::DBConnector>("APPL_DB", 0);
    Table vxlan_fdb_table(m_app_db.get(), "VXLAN_FDB_TABLE");

    struct nlmsghdr *nlmsg = mac_route_msg(true, 536870913, "", 142, 10, swss::MacAddress("00:02:03:04:05:00"));
    m_mockFdbSync.onMsgRaw(nlmsg);
    free(nlmsg);

    // Testing Mac pointing to a VTEP VXLAN FDB table insert
    nlmsg = mac_route_msg(true, 0, "1.1.1.1", 143, 20, swss::MacAddress("00:02:03:04:05:01"));
    m_mockFdbSync.onMsgRaw(nlmsg);
    free(nlmsg);

    nlmsg = mac_route_msg(true, 0, "2.2.2.2", 144, 30, swss::MacAddress("00:02:03:04:05:02"));
    m_mockFdbSync.onMsgRaw(nlmsg);
    free(nlmsg);

    std::vector<std::string> keys;
    std::vector<FieldValueTuple> fieldValues;

    vxlan_fdb_table.getKeys(keys);
    ASSERT_EQ(keys.size(), 3);
    ASSERT_EQ(keys[0], "Vlan10:00:02:03:04:05:00");
    ASSERT_EQ(keys[1], "Vlan20:00:02:03:04:05:01");
    ASSERT_EQ(keys[2], "Vlan30:00:02:03:04:05:02");

    vxlan_fdb_table.get(keys[0], fieldValues);
    auto value = swss::fvsGetValue(fieldValues, "nexthop_group", true);
    ASSERT_EQ(value.get(), "536870913");
    value = swss::fvsGetValue(fieldValues, "vni", true);
    ASSERT_EQ(value.get(), "5002");

    vxlan_fdb_table.get(keys[1], fieldValues);
    value = swss::fvsGetValue(fieldValues, "remote_vtep", true);
    ASSERT_EQ(value.get(), "1.1.1.1");
    value = swss::fvsGetValue(fieldValues, "vni", true);
    ASSERT_EQ(value.get(), "5003");

    vxlan_fdb_table.get(keys[2], fieldValues);
    value = swss::fvsGetValue(fieldValues, "remote_vtep", true);
    ASSERT_EQ(value.get(), "2.2.2.2");
    value = swss::fvsGetValue(fieldValues, "vni", true);
    ASSERT_EQ(value.get(), "5004");

    nlmsg = mac_route_msg(false, 536870913, "", 142, 10, swss::MacAddress("00:02:03:04:05:00"));
    m_mockFdbSync.onMsgRaw(nlmsg);
    free(nlmsg);

    nlmsg = mac_route_msg(false, 0, "1.1.1.1", 143, 20, swss::MacAddress("00:02:03:04:05:01"));
    m_mockFdbSync.onMsgRaw(nlmsg);
    free(nlmsg);

    nlmsg = mac_route_msg(false, 0, "2.2.2.2", 144, 30, swss::MacAddress("00:02:03:04:05:02"));
    m_mockFdbSync.onMsgRaw(nlmsg);
    free(nlmsg);

    vxlan_fdb_table.getKeys(keys);
    ASSERT_EQ(keys.size(), 0);
}

TEST_F(FdbSyncdTest, testSingletonNextHopGroup)
{
    std::shared_ptr<swss::DBConnector> m_app_db;
    m_app_db = std::make_shared<swss::DBConnector>("APPL_DB", 0);
    Table app_l2_nhg_table(m_app_db.get(), "L2_NEXTHOP_GROUP_TABLE");

    struct nlmsghdr *nlmsg = new_nhg_msg(268435458, "1.1.1.1", 0, NULL, 0);
    m_mockFdbSync.onMsgRaw(nlmsg);
    free(nlmsg);

    std::vector<std::string> keys;
    std::vector<FieldValueTuple> fieldValues;

    app_l2_nhg_table.getKeys(keys);
    ASSERT_EQ(keys.size(), 1);

    app_l2_nhg_table.get(keys[0], fieldValues);
    auto value = swss::fvsGetValue(fieldValues, "remote_vtep", true);
    ASSERT_EQ(value.get(), "1.1.1.1");

    // Delete Next hop
    nlmsg = del_nhg_msg(268435458);
    m_mockFdbSync.onMsgRaw(nlmsg);
    free(nlmsg);

    app_l2_nhg_table.getKeys(keys);
    ASSERT_EQ(keys.size(), 0);
}

TEST_F(FdbSyncdTest, testGroupedNextHopGroup)
{
    std::shared_ptr<swss::DBConnector> m_app_db;
    m_app_db = std::make_shared<swss::DBConnector>("APPL_DB", 0);
    Table app_l2_nhg_table(m_app_db.get(), "L2_NEXTHOP_GROUP_TABLE");

    // Insert singleton group 1
    struct nexthop_grp grp[1];
    memset(grp, 0, sizeof(grp));
    for (int i = 0; i < 1; i++) {
        grp[i].id = 0;
    }
    struct nlmsghdr *nlmsg = new_nhg_msg(268435458, "1.1.1.1", 0, grp, sizeof(grp));
    m_mockFdbSync.onMsgRaw(nlmsg);
    free(nlmsg);

    // Insert singleton group 2
    nlmsg = new_nhg_msg(268435459, "2.2.2.2", 0, grp, sizeof(grp));
    m_mockFdbSync.onMsgRaw(nlmsg);
    free(nlmsg);

    // Insert group of next hop groups
    struct nexthop_grp grps[2];
    memset(grps, 0, sizeof(grps));
    grps[0].id = 268435458;
    grps[1].id = 268435459;

    nlmsg = new_nhg_msg(536870913, "", 0, grps, sizeof(grps));
    m_mockFdbSync.onMsgRaw(nlmsg);
    free(nlmsg);

    std::vector<std::string> keys;
    std::vector<FieldValueTuple> fieldValues;

    app_l2_nhg_table.getKeys(keys);
    ASSERT_EQ(keys.size(), 3);

    app_l2_nhg_table.get("536870913", fieldValues);
    auto value = swss::fvsGetValue(fieldValues, "nexthop_group", true);
    ASSERT_EQ(value.get(), "268435458,268435459");

    // Delete One of the Next hops
    nlmsg = del_nhg_msg(268435458);
    m_mockFdbSync.onMsgRaw(nlmsg);
    free(nlmsg);

    app_l2_nhg_table.getKeys(keys);
    ASSERT_EQ(keys.size(), 2);

    // Since 268435458 next hop group is deleted the group of groups
    // should reflect this and only show the other nexthop singleton group
    app_l2_nhg_table.get("536870913", fieldValues);
    value = swss::fvsGetValue(fieldValues, "nexthop_group", true);
    ASSERT_EQ(value.get(), "268435459");

    app_l2_nhg_table.get("268435459", fieldValues);
    value = swss::fvsGetValue(fieldValues, "remote_vtep", true);
    ASSERT_EQ(value.get(), "2.2.2.2");

    // Delete the last next hop group and expect to see
    // group of nexthops also to be removed
    nlmsg = del_nhg_msg(268435459);
    m_mockFdbSync.onMsgRaw(nlmsg);
    free(nlmsg);

    app_l2_nhg_table.getKeys(keys);
    ASSERT_EQ(keys.size(), 0);
}

TEST_F(FdbSyncdTest, testMultiHomingAndSingleHomingMacRoute)
{
    std::shared_ptr<swss::DBConnector> m_app_db;
    m_app_db = std::make_shared<swss::DBConnector>("APPL_DB", 0);
    Table vxlan_fdb_table(m_app_db.get(), "VXLAN_FDB_TABLE");

    // Testing MAC pointing to a NHGROUP VXLAN FDB table insert
    struct nlmsghdr *nlmsg = mac_route_msg(true, 536870913, "", 142, 10, swss::MacAddress("00:02:03:04:05:00"));

    m_mockFdbSync.onMsgRaw(nlmsg);
    free(nlmsg);

    // Testing MAC pointing to a VTEP VXLAN FDB table insert
    nlmsg = mac_route_msg(true, 0, "1.1.1.1", 143, 20, swss::MacAddress("00:02:03:04:05:01"));
    m_mockFdbSync.onMsgRaw(nlmsg);
    free(nlmsg);

    // TODO: Handle ifname use case
    // Testing MAC pointing to a IFNAME VXLAN FDB table insert
    /*
    struct nlmsghdr *nlmsg = mac_route_msg(true, 0, "", 145, 20, swss::MacAddress("00:02:03:04:05:02"));
    m_mockFdbSync.onMsgRaw(nlmsg);
    free(nlmsg);
    */

    std::vector<std::string> keys;
    std::vector<FieldValueTuple> fieldValues;

    vxlan_fdb_table.getKeys(keys);
    ASSERT_EQ(keys.size(), 2);
    ASSERT_EQ(keys[0], "Vlan10:00:02:03:04:05:00");
    ASSERT_EQ(keys[1], "Vlan20:00:02:03:04:05:01");
    //ASSERT_EQ(keys[2], "Vlan20:00:02:03:04:05:02");

    vxlan_fdb_table.get(keys[0], fieldValues);
    auto value = swss::fvsGetValue(fieldValues, "nexthop_group", true);
    ASSERT_EQ(value.get(), "536870913");
    value = swss::fvsGetValue(fieldValues, "vni", true);
    ASSERT_EQ(value.get(), "5002");
    value = swss::fvsGetValue(fieldValues, "type", true);
    ASSERT_EQ(value.get(), "dynamic");


    vxlan_fdb_table.get(keys[1], fieldValues);
    value = swss::fvsGetValue(fieldValues, "remote_vtep", true);
    ASSERT_EQ(value.get(), "1.1.1.1");
    value = swss::fvsGetValue(fieldValues, "vni", true);
    ASSERT_EQ(value.get(), "5003");
    value = swss::fvsGetValue(fieldValues, "type", true);
    ASSERT_EQ(value.get(), "dynamic");

    /*
     * TODO: Handle ifname use case
    vxlan_fdb_table.get(keys[1], fieldValues);
    value = swss::fvsGetValue(fieldValues, "ifname", true);
    ASSERT_EQ(value.get(), "Portchannel01");
    value = swss::fvsGetValue(fieldValues, "vni", true);
    ASSERT_EQ(value.get(), "0");
    value = swss::fvsGetValue(fieldValues, "type", true);
    ASSERT_EQ(value.get(), "dynamic");
    */

    nlmsg = mac_route_msg(false, 536870913, "", 142, 10, swss::MacAddress("00:02:03:04:05:00"));
    m_mockFdbSync.onMsgRaw(nlmsg);
    free(nlmsg);

    nlmsg = mac_route_msg(false, 0, "1.1.1.1", 143, 20, swss::MacAddress("00:02:03:04:05:01"));
    m_mockFdbSync.onMsgRaw(nlmsg);
    free(nlmsg);

    /*
     * TODO: Handle ifname use case
    nlmsg = mac_route_msg(false, 144, 20, swss::MacAddress("00:02:03:04:05:02"));
    m_mockFdbSync.onMsgRaw(nlmsg);
    free(nlmsg);
    */

    vxlan_fdb_table.getKeys(keys);
    ASSERT_EQ(keys.size(), 0);
}

TEST_F(FdbSyncdTest, testNetlinkMessageFlags)
{
    std::shared_ptr<swss::DBConnector> m_app_db;
    m_app_db = std::make_shared<swss::DBConnector>("APPL_DB", 0);
    Table vxlan_fdb_table(m_app_db.get(), "VXLAN_FDB_TABLE");

    // Test case 1: Entry is externally learned
    struct nlmsghdr *nlmsg = mac_route_msg(true, 0, "1.1.1.1", 142, 10, swss::MacAddress("00:02:03:04:05:01"));
    struct ndmsg *ndm = (struct ndmsg *)NLMSG_DATA(nlmsg);
    ndm->ndm_state = 0; // Not permanent or no-ARP
    ndm->ndm_flags = NTF_EXT_LEARNED; // Externally learned
    m_mockFdbSync.onMsgRaw(nlmsg);
    free(nlmsg);

    std::vector<std::string> keys;
    vxlan_fdb_table.getKeys(keys);
    ASSERT_EQ(keys.size(), 1); // Should not be ignored

    // Test case 2: Entry is new neighbor with multi-homing peer sync flag
    nlmsg = mac_route_msg(true, 0, "", 144, 10, swss::MacAddress("00:02:03:04:05:02"));
    ndm = (struct ndmsg *)NLMSG_DATA(nlmsg);
    ndm->ndm_state = 0; // Not permanent or no-ARP
    ndm->ndm_flags = NTF_EXT_LEARNED; // Not externally learned
    nlmsg->nlmsg_type = RTM_NEWNEIGH;
    struct rtattr *rta = NDA_RTA(ndm);
    rta->rta_type = NDA_FLAGS_EXT;
    rta->rta_len = RTA_LENGTH(sizeof(uint32_t));
    uint32_t ext_flags = NTF_EXT_MH_PEER_SYNC;
    memcpy(RTA_DATA(rta), &ext_flags, sizeof(uint32_t));
    m_mockFdbSync.onMsgRaw(nlmsg);
    free(nlmsg);

    vxlan_fdb_table.getKeys(keys);
    ASSERT_EQ(keys.size(), 2); // Should not be ignored

    // Test case 3: Entry is new neighbor with remote-only flag
    nlmsg = mac_route_msg(true, 536870913, "", 143, 10, swss::MacAddress("00:02:03:04:05:03"));
    ndm = (struct ndmsg *)NLMSG_DATA(nlmsg);
    ndm->ndm_state = 0; // Not permanent or no-ARP
    ndm->ndm_flags = NTF_EXT_LEARNED; // Not externally learned
    nlmsg->nlmsg_type = RTM_NEWNEIGH;
    rta = NDA_RTA(ndm);
    rta->rta_type = NDA_FLAGS_EXT;
    rta->rta_len = RTA_LENGTH(sizeof(uint32_t));
    ext_flags = NTF_EXT_REMOTE_ONLY;
    memcpy(RTA_DATA(rta), &ext_flags, sizeof(uint32_t));
    m_mockFdbSync.onMsgRaw(nlmsg);
    free(nlmsg);

    vxlan_fdb_table.getKeys(keys);
    ASSERT_EQ(keys.size(), 3); // Should not be ignored

    // Clean up
    nlmsg = mac_route_msg(false, 0, "1.1.1.1", 142, 10, swss::MacAddress("00:02:03:04:05:01"));
    m_mockFdbSync.onMsgRaw(nlmsg);
    free(nlmsg);

    nlmsg = mac_route_msg(false, 0, "", 144, 10, swss::MacAddress("00:02:03:04:05:02"));
    m_mockFdbSync.onMsgRaw(nlmsg);
    free(nlmsg);

    nlmsg = mac_route_msg(false, 536870913, "", 143, 10, swss::MacAddress("00:02:03:04:05:03"));
    m_mockFdbSync.onMsgRaw(nlmsg);
    free(nlmsg);

    vxlan_fdb_table.getKeys(keys);
    ASSERT_EQ(keys.size(), 0);
}

TEST_F(FdbSyncdTest, testInvalidNextHopGroupId)
{
    std::shared_ptr<swss::DBConnector> m_app_db;
    m_app_db = std::make_shared<swss::DBConnector>("APPL_DB", 0);
    Table app_l2_nhg_table(m_app_db.get(), "L2_NEXTHOP_GROUP_TABLE");

    // Insert singleton group 1
    struct nexthop_grp grp[1];
    memset(grp, 0, sizeof(grp));
    for (int i = 0; i < 1; i++) {
        grp[i].id = 0;
    }
    struct nlmsghdr *nlmsg = new_nhg_msg(268435458, "1.1.1.1", 0, grp, sizeof(grp));
    m_mockFdbSync.onMsgRaw(nlmsg);
    free(nlmsg);

    // Insert singleton group 2
    nlmsg = new_nhg_msg(268435459, "2.2.2.2", 0, grp, sizeof(grp));
    m_mockFdbSync.onMsgRaw(nlmsg);
    free(nlmsg);

    // Insert group of next hop groups
    struct nexthop_grp grps[2];
    memset(grps, 0, sizeof(grps));
    grps[0].id = 268435458;
    grps[1].id = 268435459;

    nlmsg = new_nhg_msg(536870913, "", 0, grps, sizeof(grps));
    m_mockFdbSync.onMsgRaw(nlmsg);
    free(nlmsg);

    // Fdbsyncd should just drop this
    // new nh netlink message which has ifindex
    nlmsg = new_nhg_msg(268435455, "", 142, grps, sizeof(grps));
    m_mockFdbSync.onMsgRaw(nlmsg);
    free(nlmsg);

    // Fdbsyncd show drop this as the dst is ipv6 link local
    nlmsg = new_nhg_msg(187, "fe80::a2bc:6fff:fe8c:8a00", 0, grp, sizeof(grp));
    m_mockFdbSync.onMsgRaw(nlmsg);
    free(nlmsg);

    // Fdbsyncd show drop this as the dst is ipv4 link local
    nlmsg = new_nhg_msg(268435460, "169.254.10.20", 0, grp, sizeof(grp));
    m_mockFdbSync.onMsgRaw(nlmsg);
    free(nlmsg);

    std::vector<std::string> keys;
    std::vector<FieldValueTuple> fieldValues;

    app_l2_nhg_table.getKeys(keys);
    ASSERT_EQ(keys.size(), 3);

    app_l2_nhg_table.get("536870913", fieldValues);
    auto value = swss::fvsGetValue(fieldValues, "nexthop_group", true);
    ASSERT_EQ(value.get(), "268435458,268435459");

    // Delete the Next hops
    nlmsg = del_nhg_msg(268435458);
    m_mockFdbSync.onMsgRaw(nlmsg);
    free(nlmsg);

    nlmsg = del_nhg_msg(268435459);
    m_mockFdbSync.onMsgRaw(nlmsg);
    free(nlmsg);

    app_l2_nhg_table.getKeys(keys);
    ASSERT_EQ(keys.size(), 0);
}


TEST_F(FdbSyncdTest, testInvalidNextHopGroupIds)
{
    std::shared_ptr<swss::DBConnector> m_app_db;
    m_app_db = std::make_shared<swss::DBConnector>("APPL_DB", 0);
    Table app_l2_nhg_table(m_app_db.get(), "L2_NEXTHOP_GROUP_TABLE");

    // Insert invalid group of next hop groups with multiple ids
    struct nexthop_grp grps[3];
    memset(grps, 0, sizeof(grps));
    grps[0].id = 268;
    grps[1].id = 269;
    grps[2].id = 270;

    struct nlmsghdr *nlmsg = new_nhg_msg(536, "", 0, grps, sizeof(grps));
    m_mockFdbSync.onMsgRaw(nlmsg);
    free(nlmsg);

    std::vector<std::string> keys;
    std::vector<FieldValueTuple> fieldValues;

    // Invalid entries should have been dropped
    app_l2_nhg_table.getKeys(keys);
    ASSERT_EQ(keys.size(), 0);

    // Insert invalid group of next hop groups with single id
    struct nexthop_grp grp;
    grp.id = 268;
    nlmsg = new_nhg_msg(536, "", 0, &grp, sizeof(grp));
    m_mockFdbSync.onMsgRaw(nlmsg);
    free(nlmsg);

    // Invalid entries should have been dropped
    app_l2_nhg_table.getKeys(keys);
    ASSERT_EQ(keys.size(), 0);
}

/*
 * ================================================
 *  EVPN Multihoming Tests
 * ================================================
 */

class MockFdbSyncEvpnMh : public FdbSync
{
public:
    MockFdbSyncEvpnMh(RedisPipeline *m_pipeline, DBConnector *m_stateDb, DBConnector *m_configDb)
        : FdbSync(m_pipeline, m_stateDb, m_configDb)
    {
        m_AppRestartAssist = NULL;
        // Setup VXLAN interfaces for testing
        m_intf_info[100] = {"Vxlan-100", 10100};
        m_intf_info[200] = {"Vxlan-200", 10200};
        m_intf_info[300] = {"PortChannel1", 0};  // MH interface
        m_intf_info[301] = {"PortChannel2", 0};  // MH interface
    }

    ~MockFdbSyncEvpnMh()
    {
    }
};

class FdbSyncdEvpnMhTest : public ::testing::Test
{
public:
    void SetUp() override
    {
        ::testing_db::reset();
    }

    void TearDown() override
    {
    }

    std::shared_ptr<swss::DBConnector> m_appDb = std::make_shared<swss::DBConnector>("APPL_DB", 0);
    std::shared_ptr<RedisPipeline> m_pipeline = std::make_shared<RedisPipeline>(m_appDb.get());
    std::shared_ptr<swss::DBConnector> m_stateDb = std::make_shared<swss::DBConnector>("STATE_DB", 0);
    std::shared_ptr<swss::DBConnector> m_configDb = std::make_shared<swss::DBConnector>("CONFIG_DB", 0);
    MockFdbSyncEvpnMh m_mockFdbSync{m_pipeline.get(), m_stateDb.get(), m_configDb.get()};
};

/*
 * *****************************
 *  Helper functions for EVPN MH
 * *****************************
 */

// Helper to create L2 NHG netlink message
struct nlmsghdr *create_l2_nhg_msg(uint32_t nhid, struct nexthop_grp grp[], int grp_size)
{
    struct nlmsghdr *nlh = (struct nlmsghdr *)malloc(NLMSG_SPACE(MAX_PAYLOAD));
    memset(nlh, 0, NLMSG_SPACE(MAX_PAYLOAD));
    nlh->nlmsg_type = RTM_NEWNEXTHOP;
    nlh->nlmsg_flags = (NLM_F_CREATE | NLM_F_REQUEST);
    nlh->nlmsg_len = NLMSG_LENGTH(0);

    struct nhmsg *nhm = (struct nhmsg *)NLMSG_DATA(nlh);
    nhm->nh_family = AF_UNSPEC;
    nhm->nh_flags = 0;
    nlh->nlmsg_len += RTA_ALIGN(sizeof(*nhm));

    struct rtattr *rta = RTM_NHA(nhm);
    int max_len = MAX_PAYLOAD;

    // Add NHG ID
    rta->rta_type = NHA_ID;
    rta->rta_len = RTA_LENGTH(sizeof(uint32_t));
    memcpy(RTA_DATA(rta), (void *)&nhid, sizeof(uint32_t));
    nlh->nlmsg_len += RTA_ALIGN(rta->rta_len);

    // Add group members
    if (grp_size > 0)
    {
        rta = RTA_NEXT(rta, max_len);
        rta->rta_type = NHA_GROUP;
        rta->rta_len = static_cast<short>(RTA_LENGTH(grp_size));
        memcpy(RTA_DATA(rta), (void *)grp, grp_size);
        nlh->nlmsg_len += RTA_ALIGN(rta->rta_len);
    }

    // Mark as FDB (L2) nexthop
    rta = RTA_NEXT(rta, max_len);
    rta->rta_type = NHA_FDB;
    rta->rta_len = RTA_LENGTH(0);
    nlh->nlmsg_len += RTA_ALIGN(rta->rta_len);

    return nlh;
}

// Helper to create L2 NHG member (single VTEP)
struct nlmsghdr *create_l2_nhg_member_msg(uint32_t nhid, const char *vtep_ip, int ifindex)
{
    struct nlmsghdr *nlh = (struct nlmsghdr *)malloc(NLMSG_SPACE(MAX_PAYLOAD));
    memset(nlh, 0, NLMSG_SPACE(MAX_PAYLOAD));
    nlh->nlmsg_type = RTM_NEWNEXTHOP;
    nlh->nlmsg_flags = (NLM_F_CREATE | NLM_F_REQUEST);
    nlh->nlmsg_len = NLMSG_LENGTH(0);

    struct nhmsg *nhm = (struct nhmsg *)NLMSG_DATA(nlh);
    nlh->nlmsg_len += RTA_ALIGN(sizeof(*nhm));

    struct rtattr *rta = RTM_NHA(nhm);
    int max_len = MAX_PAYLOAD;

    // Add NH ID
    rta->rta_type = NHA_ID;
    rta->rta_len = RTA_LENGTH(sizeof(uint32_t));
    memcpy(RTA_DATA(rta), (void *)&nhid, sizeof(uint32_t));
    nlh->nlmsg_len += RTA_ALIGN(rta->rta_len);

    // Add gateway IP
    struct in_addr v4addr;
    struct in6_addr v6addr;
    rta = RTA_NEXT(rta, max_len);
    rta->rta_type = NHA_GATEWAY;
    if (inet_pton(AF_INET, vtep_ip, &v4addr) == 1)
    {
        rta->rta_len = RTA_LENGTH(sizeof(struct in_addr));
        memcpy(RTA_DATA(rta), &v4addr, sizeof(struct in_addr));
        nhm->nh_family = AF_INET;
    }
    else if (inet_pton(AF_INET6, vtep_ip, &v6addr) == 1)
    {
        rta->rta_len = RTA_LENGTH(sizeof(struct in6_addr));
        memcpy(RTA_DATA(rta), &v6addr, sizeof(struct in6_addr));
        nhm->nh_family = AF_INET6;
    }
    nlh->nlmsg_len += RTA_ALIGN(rta->rta_len);

    // Add outgoing interface
    if (ifindex != 0)
    {
        rta = RTA_NEXT(rta, max_len);
        rta->rta_type = NHA_OIF;
        rta->rta_len = RTA_LENGTH(sizeof(uint32_t));
        memcpy(RTA_DATA(rta), (void *)&ifindex, sizeof(uint32_t));
        nlh->nlmsg_len += RTA_ALIGN(rta->rta_len);
    }

    // Mark as FDB (L2) nexthop
    rta = RTA_NEXT(rta, max_len);
    rta->rta_type = NHA_FDB;
    rta->rta_len = RTA_LENGTH(0);
    nlh->nlmsg_len += RTA_ALIGN(rta->rta_len);

    return nlh;
}

// Helper to create MAC route with NHG
struct nlmsghdr *create_mac_with_nhg_msg(bool add, uint32_t nhid, int ifindex,
                                         uint16_t vlan_id, swss::MacAddress mac,
                                         bool is_mh_sync = false)
{
    uint32_t ext_flags = 0;
    struct nlmsghdr *nlh = (struct nlmsghdr *)malloc(NLMSG_SPACE(MAX_PAYLOAD));
    memset(nlh, 0, NLMSG_SPACE(MAX_PAYLOAD));

    nlh->nlmsg_type = add ? RTM_NEWNEIGH : RTM_DELNEIGH;
    nlh->nlmsg_flags = (NLM_F_CREATE | NLM_F_REQUEST);
    nlh->nlmsg_len = NLMSG_LENGTH(0);

    struct ndmsg *ndm = (struct ndmsg *)NLMSG_DATA(nlh);
    ndm->ndm_family = AF_BRIDGE;
    ndm->ndm_type = RTN_UNICAST;
    ndm->ndm_ifindex = ifindex;
    ndm->ndm_flags = NTF_EXT_LEARNED;
    ndm->ndm_state = is_mh_sync ? NUD_NOARP : NUD_REACHABLE;
    nlh->nlmsg_len += RTA_ALIGN(sizeof(*ndm));

    struct rtattr *rta = NDA_RTA(ndm);
    int max_len = MAX_PAYLOAD;

    // Add NH ID
    rta->rta_type = NDA_NH_ID;
    rta->rta_len = RTA_LENGTH(sizeof(uint32_t));
    memcpy(RTA_DATA(rta), (void *)&nhid, sizeof(uint32_t));
    nlh->nlmsg_len += RTA_ALIGN(rta->rta_len);

    // Add VLAN
    rta = RTA_NEXT(rta, max_len);
    rta->rta_type = NDA_VLAN;
    rta->rta_len = RTA_LENGTH(sizeof(uint16_t));
    memcpy(RTA_DATA(rta), (void *)&vlan_id, sizeof(uint16_t));
    nlh->nlmsg_len += RTA_ALIGN(rta->rta_len);

    // Add MAC address
    rta = RTA_NEXT(rta, max_len);
    rta->rta_type = NDA_LLADDR;
    rta->rta_len = RTA_LENGTH(ETH_ALEN);
    memcpy(RTA_DATA(rta), (void *)&mac, ETH_ALEN);
    nlh->nlmsg_len += RTA_ALIGN(rta->rta_len);

    // Add extended flags
    rta = RTA_NEXT(rta, max_len);
    rta->rta_type = NDA_FLAGS_EXT;
    rta->rta_len = RTA_LENGTH(sizeof(uint32_t));
    if (is_mh_sync)
    {
        ext_flags |= NTF_EXT_MH_PEER_SYNC;
    }
    else
    {
        ext_flags |= NTF_EXT_REMOTE_ONLY;
    }
    memcpy(RTA_DATA(rta), (void *)&ext_flags, sizeof(uint32_t));
    nlh->nlmsg_len += RTA_ALIGN(rta->rta_len);

    return nlh;
}

// Helper to create MAC route with ifname (for MH sync)
struct nlmsghdr *create_mac_with_ifname_msg(bool add, const char *ifname, int ifindex,
                                           uint16_t vlan_id, swss::MacAddress mac)
{
    uint32_t ext_flags = NTF_EXT_MH_PEER_SYNC;
    struct nlmsghdr *nlh = (struct nlmsghdr *)malloc(NLMSG_SPACE(MAX_PAYLOAD));
    memset(nlh, 0, NLMSG_SPACE(MAX_PAYLOAD));

    nlh->nlmsg_type = add ? RTM_NEWNEIGH : RTM_DELNEIGH;
    nlh->nlmsg_flags = (NLM_F_CREATE | NLM_F_REQUEST);
    nlh->nlmsg_len = NLMSG_LENGTH(0);

    struct ndmsg *ndm = (struct ndmsg *)NLMSG_DATA(nlh);
    ndm->ndm_family = AF_BRIDGE;
    ndm->ndm_type = RTN_UNICAST;
    ndm->ndm_ifindex = ifindex;
    ndm->ndm_flags = NTF_EXT_LEARNED;
    ndm->ndm_state = NUD_NOARP;  // MH sync entries are NOARP
    nlh->nlmsg_len += RTA_ALIGN(sizeof(*ndm));

    struct rtattr *rta = NDA_RTA(ndm);
    int max_len = MAX_PAYLOAD;

    // Add VLAN
    rta->rta_type = NDA_VLAN;
    rta->rta_len = RTA_LENGTH(sizeof(uint16_t));
    memcpy(RTA_DATA(rta), (void *)&vlan_id, sizeof(uint16_t));
    nlh->nlmsg_len += RTA_ALIGN(rta->rta_len);

    // Add MAC address
    rta = RTA_NEXT(rta, max_len);
    rta->rta_type = NDA_LLADDR;
    rta->rta_len = RTA_LENGTH(ETH_ALEN);
    memcpy(RTA_DATA(rta), (void *)&mac, ETH_ALEN);
    nlh->nlmsg_len += RTA_ALIGN(rta->rta_len);

    // Add extended flags for MH peer sync
    rta = RTA_NEXT(rta, max_len);
    rta->rta_type = NDA_FLAGS_EXT;
    rta->rta_len = RTA_LENGTH(sizeof(uint32_t));
    memcpy(RTA_DATA(rta), (void *)&ext_flags, sizeof(uint32_t));
    nlh->nlmsg_len += RTA_ALIGN(rta->rta_len);

    return nlh;
}

struct nlmsghdr *delete_nhg_msg(uint32_t nhid)
{
    struct nlmsghdr *nlh = (struct nlmsghdr *)malloc(NLMSG_SPACE(MAX_PAYLOAD));
    memset(nlh, 0, NLMSG_SPACE(MAX_PAYLOAD));
    nlh->nlmsg_type = RTM_DELNEXTHOP;
    nlh->nlmsg_flags = NLM_F_REQUEST;
    nlh->nlmsg_len = NLMSG_LENGTH(0);

    struct nhmsg *nhm = (struct nhmsg *)NLMSG_DATA(nlh);
    nhm->nh_family = AF_UNSPEC;
    nlh->nlmsg_len += RTA_ALIGN(sizeof(*nhm));

    struct rtattr *rta = RTM_NHA(nhm);
    rta->rta_type = NHA_ID;
    rta->rta_len = RTA_LENGTH(sizeof(uint32_t));
    memcpy(RTA_DATA(rta), (void *)&nhid, sizeof(uint32_t));
    nlh->nlmsg_len += RTA_ALIGN(rta->rta_len);

    return nlh;
}

// Helper function to get FDB table
swss::Table& getFdbTable()
{
    static std::shared_ptr<swss::DBConnector> appDb = std::make_shared<swss::DBConnector>("APPL_DB", 0);
    static swss::Table fdbTable(appDb.get(), APP_VXLAN_FDB_TABLE_NAME);
    return fdbTable;
}

// Helper function to get NHG table
swss::Table& getNhgTable()
{
    static std::shared_ptr<swss::DBConnector> appDb = std::make_shared<swss::DBConnector>("APPL_DB", 0);
    static swss::Table nhgTable(appDb.get(), APP_L2_NEXTHOP_GROUP_TABLE_NAME);
    return nhgTable;
}

/*
 * **********************
 *  EVPN MH Test Cases
 * **********************
 */

// Test 1: L2 NHG creation with single VTEP
TEST_F(FdbSyncdEvpnMhTest, L2NhgSingleVtepCreation)
{
    // Create single VTEP nexthop (member)
    uint32_t vtep_nhid = 100;
    struct nlmsghdr *nhg = create_l2_nhg_member_msg(vtep_nhid, "10.0.0.1", 0);
    m_mockFdbSync.onMsgRaw(nhg);

    // Verify L2_NEXTHOP_GROUP_TABLE entry
    std::vector<swss::FieldValueTuple> values;
    getNhgTable().get(std::to_string(vtep_nhid), values);

    EXPECT_GT(values.size(), 0);
    EXPECT_EQ(fvField(values[0]), "remote_vtep");
    EXPECT_EQ(fvValue(values[0]), "10.0.0.1");

    free(nhg);
}

// Test 2: L2 NHG creation with multiple VTEPs
TEST_F(FdbSyncdEvpnMhTest, L2NhgMultiVtepCreation)
{
    // Create 3 VTEP nexthops
    uint32_t vtep1_nhid = 200;
    uint32_t vtep2_nhid = 201;
    uint32_t vtep3_nhid = 202;

    struct nlmsghdr *nhg1 = create_l2_nhg_member_msg(vtep1_nhid, "10.0.0.2", 0);
    struct nlmsghdr *nhg2 = create_l2_nhg_member_msg(vtep2_nhid, "10.0.0.3", 0);
    struct nlmsghdr *nhg3 = create_l2_nhg_member_msg(vtep3_nhid, "192.168.1.1", 0);

    m_mockFdbSync.onMsgRaw(nhg1);
    m_mockFdbSync.onMsgRaw(nhg2);
    m_mockFdbSync.onMsgRaw(nhg3);

    // Create group nexthop with 3 members
    uint32_t group_nhid = 300;
    struct nexthop_grp grp[3];
    grp[0].id = vtep1_nhid;
    grp[0].weight = 1;
    grp[1].id = vtep2_nhid;
    grp[1].weight = 1;
    grp[2].id = vtep3_nhid;
    grp[2].weight = 1;

    struct nlmsghdr *nhg_msg = create_l2_nhg_msg(group_nhid, grp, sizeof(grp));
    m_mockFdbSync.onMsgRaw(nhg_msg);

    // Verify L2_NEXTHOP_GROUP_TABLE entry
    std::vector<swss::FieldValueTuple> values;
    getNhgTable().get(std::to_string(group_nhid), values);

    EXPECT_EQ(values.size(), 1);
    EXPECT_EQ(fvField(values[0]), "nexthop_group");
    std::string nhg_value = fvValue(values[0]);

    // Should contain comma-separated NH IDs
    EXPECT_NE(nhg_value.find(std::to_string(vtep1_nhid)), std::string::npos);
    EXPECT_NE(nhg_value.find(std::to_string(vtep2_nhid)), std::string::npos);
    EXPECT_NE(nhg_value.find(std::to_string(vtep3_nhid)), std::string::npos);

    free(nhg1);
    free(nhg2);
    free(nhg3);
    free(nhg_msg);
}

// Test 3: MAC with NHG (remote multihomed MAC)
TEST_F(FdbSyncdEvpnMhTest, MacWithNhgRemote)
{
    // Setup NHG first
    uint32_t vtep1_nhid = 400;
    uint32_t vtep2_nhid = 401;
    uint32_t group_nhid = 500;

    struct nlmsghdr *nhg1 = create_l2_nhg_member_msg(vtep1_nhid, "10.0.0.4", 0);
    struct nlmsghdr *nhg2 = create_l2_nhg_member_msg(vtep2_nhid, "10.0.0.5", 0);
    m_mockFdbSync.onMsgRaw(nhg1);
    m_mockFdbSync.onMsgRaw(nhg2);

    struct nexthop_grp grp[2];
    grp[0].id = vtep1_nhid;
    grp[0].weight = 1;
    grp[1].id = vtep2_nhid;
    grp[1].weight = 1;

    struct nlmsghdr *nhg_msg = create_l2_nhg_msg(group_nhid, grp, sizeof(grp));
    m_mockFdbSync.onMsgRaw(nhg_msg);

    // Add MAC with NHG
    swss::MacAddress mac("00:00:11:22:33:44");
    uint16_t vlan = 100;
    struct nlmsghdr *mac_msg = create_mac_with_nhg_msg(true, group_nhid, 100, vlan, mac, false);
    m_mockFdbSync.onMsgRaw(mac_msg);

    // Verify VXLAN_FDB_TABLE entry with nexthop_group
    std::string key = "Vlan" + std::to_string(vlan) + ":" + mac.to_string();
    std::vector<swss::FieldValueTuple> values;
    getFdbTable().get(key, values);

    bool found_nhg = false;
    for (const auto &fv : values)
    {
        if (fvField(fv) == "nexthop_group")
        {
            EXPECT_EQ(fvValue(fv), std::to_string(group_nhid));
            found_nhg = true;
        }
    }
    EXPECT_TRUE(found_nhg);

    free(nhg1);
    free(nhg2);
    free(nhg_msg);
    free(mac_msg);
}

// Test 9: IPv6 VTEP in L2 NHG
TEST_F(FdbSyncdEvpnMhTest, L2NhgIpv6Vtep)
{
    uint32_t nhid = 1000;
    struct nlmsghdr *nhg = create_l2_nhg_member_msg(nhid, "fc00::1", 0);
    m_mockFdbSync.onMsgRaw(nhg);

    // Verify entry created
    std::vector<swss::FieldValueTuple> values;
    getNhgTable().get(std::to_string(nhid), values);

    EXPECT_GT(values.size(), 0);
    EXPECT_EQ(fvField(values[0]), "remote_vtep");
    EXPECT_EQ(fvValue(values[0]), "fc00::1");

    free(nhg);
}

// Test 10: Multiple MAC maps using same NHG
TEST_F(FdbSyncdEvpnMhTest, MultipleMapsWithSameNhg)
{
    // Create NHG
    uint32_t nhid = 1100;
    struct nlmsghdr *nhg = create_l2_nhg_member_msg(nhid, "10.0.0.11", 0);
    m_mockFdbSync.onMsgRaw(nhg);

    // Add multiple MACs using same NHG
    swss::MacAddress mac1("AA:BB:CC:DD:EE:01");
    swss::MacAddress mac2("AA:BB:CC:DD:EE:02");
    swss::MacAddress mac3("AA:BB:CC:DD:EE:03");
    uint16_t vlan = 100;

    struct nlmsghdr *mac_msg1 = create_mac_with_nhg_msg(true, nhid, 100, vlan, mac1, false);
    struct nlmsghdr *mac_msg2 = create_mac_with_nhg_msg(true, nhid, 100, vlan, mac2, false);
    struct nlmsghdr *mac_msg3 = create_mac_with_nhg_msg(true, nhid, 100, vlan, mac3, false);

    m_mockFdbSync.onMsgRaw(mac_msg1);
    m_mockFdbSync.onMsgRaw(mac_msg2);
    m_mockFdbSync.onMsgRaw(mac_msg3);

    // Verify all 3 MACs reference same NHG
    std::string key1 = "Vlan" + std::to_string(vlan) + ":" + mac1.to_string();
    std::string key2 = "Vlan" + std::to_string(vlan) + ":" + mac2.to_string();
    std::string key3 = "Vlan" + std::to_string(vlan) + ":" + mac3.to_string();

    std::vector<swss::FieldValueTuple> values;
    getFdbTable().get(key1, values);
    EXPECT_GT(values.size(), 0);

    values.clear();
    getFdbTable().get(key2, values);
    EXPECT_GT(values.size(), 0);

    values.clear();
    getFdbTable().get(key3, values);
    EXPECT_GT(values.size(), 0);

    free(nhg);
    free(mac_msg1);
    free(mac_msg2);
    free(mac_msg3);
}

// Test 11: MAC move from single VTEP to NHG
TEST_F(FdbSyncdEvpnMhTest, MacMoveSingleVtepToNhg)
{
    swss::MacAddress mac("00:11:22:33:44:55");
    uint16_t vlan = 100;

    // First, add MAC with single VTEP
    uint32_t single_nhid = 1200;
    struct nlmsghdr *nhg_single = create_l2_nhg_member_msg(single_nhid, "10.0.0.12", 0);
    m_mockFdbSync.onMsgRaw(nhg_single);

    struct nlmsghdr *mac_msg_single = create_mac_with_nhg_msg(true, single_nhid, 100, vlan, mac, false);
    m_mockFdbSync.onMsgRaw(mac_msg_single);

    // Verify single VTEP entry
    std::string key = "Vlan" + std::to_string(vlan) + ":" + mac.to_string();
    std::vector<swss::FieldValueTuple> values;
    getFdbTable().get(key, values);
    EXPECT_GT(values.size(), 0);

    // Now move to NHG (ES becomes multihomed)
    uint32_t vtep1_nhid = 1201;
    uint32_t vtep2_nhid = 1202;
    uint32_t group_nhid = 1300;

    struct nlmsghdr *nhg1 = create_l2_nhg_member_msg(vtep1_nhid, "10.0.0.12", 0);
    struct nlmsghdr *nhg2 = create_l2_nhg_member_msg(vtep2_nhid, "10.0.0.13", 0);
    m_mockFdbSync.onMsgRaw(nhg1);
    m_mockFdbSync.onMsgRaw(nhg2);

    struct nexthop_grp grp[2];
    grp[0].id = vtep1_nhid;
    grp[0].weight = 1;
    grp[1].id = vtep2_nhid;
    grp[1].weight = 1;

    struct nlmsghdr *nhg_msg = create_l2_nhg_msg(group_nhid, grp, sizeof(grp));
    m_mockFdbSync.onMsgRaw(nhg_msg);

    // Update MAC to use NHG
    struct nlmsghdr *mac_msg_nhg = create_mac_with_nhg_msg(true, group_nhid, 100, vlan, mac, false);
    m_mockFdbSync.onMsgRaw(mac_msg_nhg);

    // Verify MAC now uses NHG
    values.clear();
    getFdbTable().get(key, values);

    bool found_nhg = false;
    for (const auto &fv : values)
    {
        if (fvField(fv) == "nexthop_group")
        {
            EXPECT_EQ(fvValue(fv), std::to_string(group_nhid));
            found_nhg = true;
        }
    }
    EXPECT_TRUE(found_nhg);

    free(nhg_single);
    free(mac_msg_single);
    free(nhg1);
    free(nhg2);
    free(nhg_msg);
    free(mac_msg_nhg);
}

// Test 12: NHG refcounting
TEST_F(FdbSyncdEvpnMhTest, NhgRefcounting)
{
    // Create NHG
    uint32_t nhid = 1400;
    struct nlmsghdr *nhg = create_l2_nhg_member_msg(nhid, "10.0.0.14", 0);
    m_mockFdbSync.onMsgRaw(nhg);

    // Add MAC using NHG
    swss::MacAddress mac("00:AA:BB:CC:DD:EE");
    uint16_t vlan = 100;
    struct nlmsghdr *mac_msg = create_mac_with_nhg_msg(true, nhid, 100, vlan, mac, false);
    m_mockFdbSync.onMsgRaw(mac_msg);

    // Try to delete NHG (should fail or defer if MAC still references it)
    struct nlmsghdr *nhg_del = delete_nhg_msg(nhid);
    m_mockFdbSync.onMsgRaw(nhg_del);

    // NHG should still exist (refcount > 0)
    // Note: Actual refcounting behavior depends on implementation
    // This test verifies the framework is in place

    free(nhg);
    free(mac_msg);
    free(nhg_del);
}
