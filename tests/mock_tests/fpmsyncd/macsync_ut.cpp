/**
 * @file macsync_ut.cpp
 * @brief Unit tests for MAC synchronization over the FPM channel.
 *
 * The cases here are deliberately shaped around defects that a build cannot
 * catch: every one of them produces a plausible-looking but wrong
 * APP_VXLAN_FDB_TABLE entry rather than a crash or a link error.
 */

#include "table.h"
#include "schema.h"
#include "redisutility.h"
#include "mock_table.h"

#include <gtest/gtest.h>

#include <net/if.h>

#include <cstring>
#include <vector>

#include <linux/if_ether.h>
#include <linux/neighbour.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define private public
#include "fpmsyncd/macsync.h"
#undef private

#include "fpmsyncd/routesync.h"

using namespace swss;

#ifndef NDA_SRC_VNI
#define NDA_SRC_VNI 11
#endif

#ifndef NDA_PROTOCOL
#define NDA_PROTOCOL 12
#endif

/* Present only on newer kernel headers. */
#ifndef NTF_STICKY
#define NTF_STICKY 0x40
#endif

namespace {

constexpr uint16_t TEST_VLAN = 100;
constexpr uint32_t TEST_VNI = 20100;
const uint8_t TEST_MAC[ETH_ALEN] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55};
const char *TEST_KEY = "Vlan100:00:11:22:33:44:55";

}

class MacSyncTest : public ::testing::Test
{
public:
    void SetUp() override
    {
        testing_db::reset();
        m_appDb = std::make_shared<DBConnector>("APPL_DB", 0);
        m_stateDb = std::make_shared<DBConnector>("STATE_DB", 0);
        m_cfgDb = std::make_shared<DBConnector>("CONFIG_DB", 0);
        m_pipeline = std::make_shared<RedisPipeline>(m_appDb.get());
        m_macSync = std::make_unique<MacSync>(m_pipeline.get(), m_stateDb.get(),
                                              m_cfgDb.get());
        /* CONFIG_DB is not populated under the mock, so drive the mode directly. */
        m_macSync->m_fpmMode = true;

        /* Needed to exercise the real dispatch path rather than calling the
         * handler directly; onMsgRaw filters message types before MacSync. */
        m_routeSync = std::make_unique<RouteSync>(m_pipeline.get());
        m_routeSync->setMacSync(m_macSync.get());
    }

    void TearDown() override
    {
        m_routeSync.reset();
        m_macSync.reset();
        m_pipeline.reset();
    }

    /* Builds an AF_BRIDGE neighbour message of the shape zebra emits. */
    struct MacMsg
    {
        struct nlmsghdr n;
        struct ndmsg ndm;
        char buf[256];
    };

    void buildMacMsg(MacMsg& req, uint16_t nlmsgType, uint16_t state)
    {
        memset(&req, 0, sizeof(req));
        req.n.nlmsg_len = NLMSG_LENGTH(sizeof(struct ndmsg));
        req.n.nlmsg_type = nlmsgType;
        req.ndm.ndm_family = AF_BRIDGE;
        req.ndm.ndm_state = state;
        req.ndm.ndm_flags = NTF_MASTER | NTF_EXT_LEARNED;
        addAttr(req, NDA_LLADDR, TEST_MAC, ETH_ALEN);
    }

    void addAttr(MacMsg& req, int type, const void *data, size_t alen)
    {
        size_t len = RTA_LENGTH(alen);
        struct rtattr *rta =
            (struct rtattr *)(((char *)&req.n) + NLMSG_ALIGN(req.n.nlmsg_len));
        rta->rta_type = (unsigned short)type;
        rta->rta_len = (unsigned short)len;
        memcpy(RTA_DATA(rta), data, alen);
        req.n.nlmsg_len = (uint32_t)(NLMSG_ALIGN(req.n.nlmsg_len) + RTA_ALIGN(len));
    }

    void addVlan(MacMsg& req, uint16_t vlan) { addAttr(req, NDA_VLAN, &vlan, sizeof(vlan)); }
    void addSrcVni(MacMsg& req, uint32_t vni) { addAttr(req, NDA_SRC_VNI, &vni, sizeof(vni)); }
    void addNhgId(MacMsg& req, uint32_t nhid) { addAttr(req, NDA_NH_ID, &nhid, sizeof(nhid)); }

    void addVtep(MacMsg& req, const char *ip)
    {
        struct in_addr addr;
        inet_pton(AF_INET, ip, &addr);
        addAttr(req, NDA_DST, &addr, sizeof(addr));
    }

    void feed(MacMsg& req)
    {
        int len = (int)(req.n.nlmsg_len - NLMSG_LENGTH(sizeof(struct ndmsg)));
        m_macSync->onMacMsg(&req.n, len);
    }

    /* Delivers through RouteSync so the message-type filter is exercised. */
    void feedRaw(MacMsg& req)
    {
        m_routeSync->onMsgRaw(&req.n);
    }

    bool getEntry(const std::string& key, std::vector<FieldValueTuple>& values)
    {
        Table table(m_appDb.get(), APP_VXLAN_FDB_TABLE_NAME);
        return table.get(key, values);
    }

    std::string fieldValue(const std::vector<FieldValueTuple>& values,
                           const std::string& field)
    {
        auto v = swss::fvsGetValue(values, field);
        return v.has_value() ? *v : std::string();
    }

protected:
    std::shared_ptr<DBConnector> m_appDb;
    std::shared_ptr<DBConnector> m_stateDb;
    std::shared_ptr<DBConnector> m_cfgDb;
    std::shared_ptr<RedisPipeline> m_pipeline;
    std::unique_ptr<MacSync> m_macSync;
    std::unique_ptr<RouteSync> m_routeSync;
};

/*
 * zebra encodes the VNI as NDA_SRC_VNI. Reading NDA_VNI instead yields a
 * well-formed entry carrying vni 0.
 */
TEST_F(MacSyncTest, RemoteMacTakesVniFromSrcVni)
{
    MacMsg req;
    buildMacMsg(req, RTM_NEWNEIGH, NUD_REACHABLE);
    addVlan(req, TEST_VLAN);
    addVtep(req, "10.1.1.1");
    addSrcVni(req, TEST_VNI);
    feed(req);

    std::vector<FieldValueTuple> values;
    ASSERT_TRUE(getEntry(TEST_KEY, values));
    EXPECT_EQ(fieldValue(values, "vni"), std::to_string(TEST_VNI));
    EXPECT_EQ(fieldValue(values, "remote_vtep"), "10.1.1.1");
}

/*
 * A withdrawal can arrive as an add carrying NUD_INCOMPLETE or NUD_FAILED,
 * which must remove the entry rather than install it.
 */
TEST_F(MacSyncTest, RemoteMacWithdrawnOnIncompleteState)
{
    MacMsg add;
    buildMacMsg(add, RTM_NEWNEIGH, NUD_REACHABLE);
    addVlan(add, TEST_VLAN);
    addVtep(add, "10.1.1.1");
    addSrcVni(add, TEST_VNI);
    feed(add);

    std::vector<FieldValueTuple> values;
    ASSERT_TRUE(getEntry(TEST_KEY, values));

    MacMsg withdraw;
    buildMacMsg(withdraw, RTM_NEWNEIGH, NUD_INCOMPLETE);
    addVlan(withdraw, TEST_VLAN);
    addVtep(withdraw, "10.1.1.1");
    feed(withdraw);

    values.clear();
    EXPECT_FALSE(getEntry(TEST_KEY, values));
}

TEST_F(MacSyncTest, RemoteMacWithdrawnOnFailedState)
{
    MacMsg add;
    buildMacMsg(add, RTM_NEWNEIGH, NUD_REACHABLE);
    addVlan(add, TEST_VLAN);
    addVtep(add, "10.1.1.1");
    addSrcVni(add, TEST_VNI);
    feed(add);

    std::vector<FieldValueTuple> values;
    ASSERT_TRUE(getEntry(TEST_KEY, values));

    MacMsg withdraw;
    buildMacMsg(withdraw, RTM_NEWNEIGH, NUD_FAILED);
    addVlan(withdraw, TEST_VLAN);
    addVtep(withdraw, "10.1.1.1");
    feed(withdraw);

    values.clear();
    EXPECT_FALSE(getEntry(TEST_KEY, values));
}

TEST_F(MacSyncTest, RemoteMacDeletedOnDelNeigh)
{
    MacMsg add;
    buildMacMsg(add, RTM_NEWNEIGH, NUD_REACHABLE);
    addVlan(add, TEST_VLAN);
    addVtep(add, "10.1.1.1");
    addSrcVni(add, TEST_VNI);
    feed(add);

    MacMsg del;
    buildMacMsg(del, RTM_DELNEIGH, NUD_REACHABLE);
    addVlan(del, TEST_VLAN);
    addVtep(del, "10.1.1.1");
    feed(del);

    std::vector<FieldValueTuple> values;
    EXPECT_FALSE(getEntry(TEST_KEY, values));
}

/* NUD_NOARP marks a sticky MAC, which fdbOrch consumes as a static entry. */
TEST_F(MacSyncTest, StickyRemoteMacIsStatic)
{
    MacMsg req;
    buildMacMsg(req, RTM_NEWNEIGH, NUD_REACHABLE | NUD_NOARP);
    addVlan(req, TEST_VLAN);
    addVtep(req, "10.1.1.1");
    addSrcVni(req, TEST_VNI);
    feed(req);

    std::vector<FieldValueTuple> values;
    ASSERT_TRUE(getEntry(TEST_KEY, values));
    EXPECT_EQ(fieldValue(values, "type"), "static");
}

TEST_F(MacSyncTest, NonStickyRemoteMacIsDynamic)
{
    MacMsg req;
    buildMacMsg(req, RTM_NEWNEIGH, NUD_REACHABLE);
    addVlan(req, TEST_VLAN);
    addVtep(req, "10.1.1.1");
    addSrcVni(req, TEST_VNI);
    feed(req);

    std::vector<FieldValueTuple> values;
    ASSERT_TRUE(getEntry(TEST_KEY, values));
    EXPECT_EQ(fieldValue(values, "type"), "dynamic");
}

/* Without NDA_VLAN the table key cannot be built, so nothing may be written. */
TEST_F(MacSyncTest, RemoteMacWithoutVlanIsRejected)
{
    MacMsg req;
    buildMacMsg(req, RTM_NEWNEIGH, NUD_REACHABLE);
    addVtep(req, "10.1.1.1");
    addSrcVni(req, TEST_VNI);
    feed(req);

    std::vector<FieldValueTuple> values;
    EXPECT_FALSE(getEntry(TEST_KEY, values));
}

TEST_F(MacSyncTest, RemoteMacWithoutVtepIsRejected)
{
    MacMsg req;
    buildMacMsg(req, RTM_NEWNEIGH, NUD_REACHABLE);
    addVlan(req, TEST_VLAN);
    addSrcVni(req, TEST_VNI);
    feed(req);

    std::vector<FieldValueTuple> values;
    EXPECT_FALSE(getEntry(TEST_KEY, values));
}

/*
 * A truncated attribute drops the whole message: reading a four byte VNI out of
 * a one byte payload reads past the attribute and yields a plausible entry
 * under the wrong key or VNI.
 */
TEST_F(MacSyncTest, RemoteMacWithTruncatedAttributeIsDropped)
{
    const uint8_t oneByte = 1;

    const int truncatedAttrs[] = {NDA_VLAN, NDA_NH_ID, NDA_SRC_VNI, NDA_VNI};

    for (int truncated : truncatedAttrs)
    {
        MacMsg req;
        buildMacMsg(req, RTM_NEWNEIGH, NUD_REACHABLE);
        if (truncated != NDA_VLAN)
        {
            addVlan(req, TEST_VLAN);
        }
        addVtep(req, "10.1.1.1");
        if (truncated != NDA_SRC_VNI)
        {
            addSrcVni(req, TEST_VNI);
        }
        addAttr(req, truncated, &oneByte, sizeof(oneByte));
        feed(req);

        std::vector<std::string> keys;
        Table(m_appDb.get(), APP_VXLAN_FDB_TABLE_NAME).getKeys(keys);
        EXPECT_TRUE(keys.empty()) << "truncated attribute " << truncated;
    }

    MacMsg intact;
    buildMacMsg(intact, RTM_NEWNEIGH, NUD_REACHABLE);
    addVlan(intact, TEST_VLAN);
    addVtep(intact, "10.1.1.1");
    addSrcVni(intact, TEST_VNI);
    feed(intact);

    std::vector<FieldValueTuple> values;
    EXPECT_TRUE(getEntry(TEST_KEY, values));
}

TEST_F(MacSyncTest, RemoteMacWithShortLladdrIsDropped)
{
    MacMsg req;
    memset(&req, 0, sizeof(req));
    req.n.nlmsg_len = NLMSG_LENGTH(sizeof(struct ndmsg));
    req.n.nlmsg_type = RTM_NEWNEIGH;
    req.ndm.ndm_family = AF_BRIDGE;
    req.ndm.ndm_state = NUD_REACHABLE;
    addAttr(req, NDA_LLADDR, TEST_MAC, 4);
    addVlan(req, TEST_VLAN);
    addVtep(req, "10.1.1.1");
    addSrcVni(req, TEST_VNI);
    feed(req);

    std::vector<std::string> keys;
    Table(m_appDb.get(), APP_VXLAN_FDB_TABLE_NAME).getKeys(keys);
    EXPECT_TRUE(keys.empty());
}

/* In kernel mode fdbsyncd owns the table and MacSync must not touch it. */
TEST_F(MacSyncTest, KernelModeIgnoresRemoteMac)
{
    m_macSync->m_fpmMode = false;

    MacMsg req;
    buildMacMsg(req, RTM_NEWNEIGH, NUD_REACHABLE);
    addVlan(req, TEST_VLAN);
    addVtep(req, "10.1.1.1");
    addSrcVni(req, TEST_VNI);
    feed(req);

    std::vector<FieldValueTuple> values;
    EXPECT_FALSE(getEntry(TEST_KEY, values));
}

/* IP neighbours share the message type and must not reach the MAC path. */
TEST_F(MacSyncTest, NonBridgeFamilyIsIgnored)
{
    MacMsg req;
    buildMacMsg(req, RTM_NEWNEIGH, NUD_REACHABLE);
    req.ndm.ndm_family = AF_INET;
    addVlan(req, TEST_VLAN);
    addVtep(req, "10.1.1.1");
    feed(req);

    std::vector<FieldValueTuple> values;
    EXPECT_FALSE(getEntry(TEST_KEY, values));
}

/*
 * onMsgRaw filters on an allow-list of message types before dispatching. A
 * handler that works when called directly is still dead if its message type
 * is missing from that list, so these two drive the real path end to end.
 */
TEST_F(MacSyncTest, RemoteMacReachesMacSyncThroughOnMsgRaw)
{
    MacMsg req;
    buildMacMsg(req, RTM_NEWNEIGH, NUD_REACHABLE);
    addVlan(req, TEST_VLAN);
    addVtep(req, "10.1.1.1");
    addSrcVni(req, TEST_VNI);
    feedRaw(req);

    std::vector<FieldValueTuple> values;
    ASSERT_TRUE(getEntry(TEST_KEY, values));
    EXPECT_EQ(fieldValue(values, "remote_vtep"), "10.1.1.1");
    EXPECT_EQ(fieldValue(values, "vni"), std::to_string(TEST_VNI));
}

TEST_F(MacSyncTest, RemoteMacWithdrawalReachesMacSyncThroughOnMsgRaw)
{
    MacMsg add;
    buildMacMsg(add, RTM_NEWNEIGH, NUD_REACHABLE);
    addVlan(add, TEST_VLAN);
    addVtep(add, "10.1.1.1");
    addSrcVni(add, TEST_VNI);
    feedRaw(add);

    std::vector<FieldValueTuple> values;
    ASSERT_TRUE(getEntry(TEST_KEY, values));

    MacMsg del;
    buildMacMsg(del, RTM_DELNEIGH, NUD_REACHABLE);
    addVlan(del, TEST_VLAN);
    feedRaw(del);

    EXPECT_FALSE(getEntry(TEST_KEY, values));
}

/* Captures what MacSync sends to zebra. */
class RecordingFpm : public FpmInterface
{
public:
    bool send(nlmsghdr *hdr) override
    {
        size_t len = hdr->nlmsg_len ? hdr->nlmsg_len : NLMSG_LENGTH(0);
        std::vector<uint8_t> copy(len);
        memcpy(copy.data(), hdr, len);
        m_sent.push_back(std::move(copy));
        return true;
    }

    int getFd() override { return -1; }
    uint64_t readData() override { return 0; }

    size_t count() const { return m_sent.size(); }

    const nlmsghdr *at(size_t i) const
    {
        return reinterpret_cast<const nlmsghdr *>(m_sent.at(i).data());
    }

private:
    std::vector<std::vector<uint8_t>> m_sent;
};

/*
 * The outbound encoding zebra actually parses. Stickiness rides in ndm_flags as
 * NTF_STICKY, and zebra sets NUD_NOARP only alongside it, so a hardware-learnt
 * MAC must carry neither: were it sent sticky, BGP would advertise it as an
 * EVPN static MAC and the address could never move. No NDA_PROTOCOL is sent:
 * zebra reads the protocol only to drop echoes of its own entries.
 */
static const struct rtattr *findAttr(const nlmsghdr *hdr, int type)
{
    const struct ndmsg *ndm = (const struct ndmsg *)NLMSG_DATA(hdr);
    int len = (int)(hdr->nlmsg_len - NLMSG_LENGTH(sizeof(struct ndmsg)));
    const struct rtattr *rta =
        (const struct rtattr *)((const char *)ndm + NLMSG_ALIGN(sizeof(struct ndmsg)));

    for (; RTA_OK(rta, len); rta = RTA_NEXT(rta, len))
    {
        if (rta->rta_type == type)
            return rta;
    }
    return nullptr;
}

TEST_F(MacSyncTest, LocalDynamicMacIsNotSticky)
{
    RecordingFpm fpm;
    m_macSync->onFpmConnected(fpm);

    /* "lo" because sendLocalMac() resolves the port with if_nametoindex(). */
    KeyOpFieldsValuesTuple add{"Vlan100:00:11:22:33:44:55", SET_COMMAND,
                               {{"port", "lo"}, {"type", "dynamic"}}};
    m_macSync->processStateFdbEntry(add);
    ASSERT_EQ(fpm.count(), 1u);

    const nlmsghdr *hdr = fpm.at(0);
    const struct ndmsg *ndm = (const struct ndmsg *)NLMSG_DATA(hdr);

    EXPECT_EQ(hdr->nlmsg_type, RTM_NEWNEIGH);
    EXPECT_TRUE(ndm->ndm_flags & NTF_MASTER);
    EXPECT_TRUE(ndm->ndm_flags & NTF_EXT_LEARNED);
    EXPECT_FALSE(ndm->ndm_flags & NTF_STICKY);
    EXPECT_FALSE(ndm->ndm_state & NUD_NOARP);

    EXPECT_EQ(findAttr(hdr, NDA_PROTOCOL), nullptr);
}

/*
 * A provisioned MAC is pinned to a port by configuration, so it is advertised
 * sticky and remote PEs reject a move for it (RFC 7432 section 7.8). Only local
 * entries reach STATE_DB, so this can never catch an EVPN-learnt address.
 */
TEST_F(MacSyncTest, LocalStaticMacIsSticky)
{
    RecordingFpm fpm;
    m_macSync->onFpmConnected(fpm);

    KeyOpFieldsValuesTuple add{"Vlan100:00:11:22:33:44:66", SET_COMMAND,
                               {{"port", "lo"}, {"type", "static"}}};
    m_macSync->processStateFdbEntry(add);
    ASSERT_EQ(fpm.count(), 1u);

    const struct ndmsg *ndm = (const struct ndmsg *)NLMSG_DATA(fpm.at(0));

    EXPECT_TRUE(ndm->ndm_flags & NTF_STICKY);
    EXPECT_TRUE(ndm->ndm_state & NUD_NOARP);
}

/*
 * A port can be torn down before orchagent removes its FDB entries from
 * STATE_DB, so the port name no longer resolves when the delete arrives. The
 * delete must still reach zebra, against the ifindex the MAC was added on.
 */
TEST_F(MacSyncTest, LocalMacDeleteReachesZebraAfterThePortIsGone)
{
    RecordingFpm fpm;
    m_macSync->onFpmConnected(fpm);
    ASSERT_EQ(fpm.count(), 0u);

    KeyOpFieldsValuesTuple add{TEST_KEY, SET_COMMAND, {{"port", "Ethernet0"}, {"type", "dynamic"}}};
    m_macSync->m_ifNameToIndex = [](const std::string&) { return 77u; };
    m_macSync->processStateFdbEntry(add);
    ASSERT_EQ(fpm.count(), 1u);
    EXPECT_EQ(fpm.at(0)->nlmsg_type, RTM_NEWNEIGH);

    m_macSync->m_ifNameToIndex = [](const std::string&) { return 0u; };
    KeyOpFieldsValuesTuple del{TEST_KEY, DEL_COMMAND, {}};
    m_macSync->processStateFdbEntry(del);

    ASSERT_EQ(fpm.count(), 2u);
    EXPECT_EQ(fpm.at(1)->nlmsg_type, RTM_DELNEIGH);
    const struct ndmsg *ndm = (const struct ndmsg *)NLMSG_DATA(fpm.at(1));
    EXPECT_EQ(ndm->ndm_ifindex, 77);
    EXPECT_EQ(m_macSync->m_localMacs.count(TEST_KEY), 0u);
}

/* A MAC whose port never resolved was never sent, so there is nothing to withdraw. */
TEST_F(MacSyncTest, LocalMacOnAnUnresolvedPortSendsNothing)
{
    RecordingFpm fpm;
    m_macSync->onFpmConnected(fpm);

    m_macSync->m_ifNameToIndex = [](const std::string&) { return 0u; };
    KeyOpFieldsValuesTuple add{TEST_KEY, SET_COMMAND, {{"port", "Ethernet0"}, {"type", "dynamic"}}};
    m_macSync->processStateFdbEntry(add);
    KeyOpFieldsValuesTuple del{TEST_KEY, DEL_COMMAND, {}};
    m_macSync->processStateFdbEntry(del);

    EXPECT_EQ(fpm.count(), 0u);
}

/*
 * A MAC on an Ethernet Segment we also hold locally is reachable through our own
 * access port, so zebra sends it against that port with neither a VTEP nor a
 * nexthop group. It must be programmed against the port, not discarded as the
 * bridge-side duplicate, and FdbOrch disables ageing on it.
 */
TEST_F(MacSyncTest, MacOnLocalEthernetSegmentProgrammedAgainstItsPort)
{
    Table cfgEs(m_cfgDb.get(), "EVPN_ETHERNET_SEGMENT");
    cfgEs.set("lo", std::vector<FieldValueTuple>{{"esi", "AUTO"}});

    MacMsg req;
    buildMacMsg(req, RTM_NEWNEIGH, NUD_REACHABLE | NUD_NOARP);
    req.ndm.ndm_ifindex = (int)if_nametoindex("lo");
    addVlan(req, TEST_VLAN);
    addSrcVni(req, TEST_VNI);
    feed(req);

    std::vector<FieldValueTuple> values;
    ASSERT_TRUE(getEntry(TEST_KEY, values));
    EXPECT_EQ(fieldValue(values, "ifname"), "lo");
    EXPECT_EQ(fieldValue(values, "remote_vtep"), "");
    EXPECT_EQ(fieldValue(values, "nexthop_group"), "");
    EXPECT_EQ(fieldValue(values, "type"), "static");

    cfgEs.del("lo");
}

/* The same message on a port with no Ethernet Segment is the bridge-side copy of
 * a remote MAC, and is still discarded. */
TEST_F(MacSyncTest, MacOnNonEsPortWithoutVtepIsSkipped)
{
    MacMsg req;
    buildMacMsg(req, RTM_NEWNEIGH, NUD_REACHABLE);
    req.ndm.ndm_ifindex = (int)if_nametoindex("lo");
    addVlan(req, TEST_VLAN);
    addSrcVni(req, TEST_VNI);
    feed(req);

    std::vector<FieldValueTuple> values;
    EXPECT_FALSE(getEntry(TEST_KEY, values));
}

/*
 * zebra sizes NDA_DST from the address family it guessed rather than from what
 * it actually holds, so a MAC with no VTEP still arrives carrying a 16 byte
 * all-zero destination. Treating that as a real address published `::` as the
 * remote VTEP and hid the Ethernet Segment the MAC really sits on.
 */
TEST_F(MacSyncTest, UnspecifiedVtepIsNotAnAddress)
{
    Table cfgEs(m_cfgDb.get(), "EVPN_ETHERNET_SEGMENT");
    cfgEs.set("lo", std::vector<FieldValueTuple>{{"esi", "AUTO"}});

    struct in6_addr unspecified;
    memset(&unspecified, 0, sizeof(unspecified));

    MacMsg req;
    buildMacMsg(req, RTM_NEWNEIGH, NUD_REACHABLE | NUD_NOARP);
    req.ndm.ndm_ifindex = (int)if_nametoindex("lo");
    addVlan(req, TEST_VLAN);
    addSrcVni(req, TEST_VNI);
    addAttr(req, NDA_DST, &unspecified, sizeof(unspecified));
    feed(req);

    std::vector<FieldValueTuple> values;
    ASSERT_TRUE(getEntry(TEST_KEY, values));
    EXPECT_EQ(fieldValue(values, "ifname"), "lo");
    EXPECT_EQ(fieldValue(values, "remote_vtep"), "");

    cfgEs.del("lo");
}

/* Same encoding on a port that is not an Ethernet Segment: there is nothing to
 * program the MAC against, so it must be dropped rather than published. */
TEST_F(MacSyncTest, UnspecifiedVtepOnNonEsPortIsSkipped)
{
    struct in6_addr unspecified;
    memset(&unspecified, 0, sizeof(unspecified));

    MacMsg req;
    buildMacMsg(req, RTM_NEWNEIGH, NUD_REACHABLE);
    req.ndm.ndm_ifindex = (int)if_nametoindex("lo");
    addVlan(req, TEST_VLAN);
    addSrcVni(req, TEST_VNI);
    addAttr(req, NDA_DST, &unspecified, sizeof(unspecified));
    feed(req);

    std::vector<FieldValueTuple> values;
    EXPECT_FALSE(getEntry(TEST_KEY, values));
}

/*
 * A MAC behind an Ethernet Segment reaches several VTEPs at once, so zebra
 * resolves it to an L2 nexthop group and sends NDA_NH_ID with no NDA_DST. It
 * has to be programmed against that group rather than discarded as though it
 * were the harmless bridge-side duplicate. fdbsyncd publishes the group itself
 * into L2_NEXTHOP_GROUP_TABLE from the kernel in either mac_sync_mode.
 */
TEST_F(MacSyncTest, EsBackedRemoteMacProgrammedAgainstNexthopGroup)
{
    MacMsg req;
    buildMacMsg(req, RTM_NEWNEIGH, NUD_REACHABLE);
    addVlan(req, TEST_VLAN);
    addSrcVni(req, TEST_VNI);
    addNhgId(req, 4242);
    feed(req);

    std::vector<FieldValueTuple> values;
    ASSERT_TRUE(getEntry(TEST_KEY, values));
    EXPECT_EQ(fieldValue(values, "nexthop_group"), "4242");
    EXPECT_EQ(fieldValue(values, "remote_vtep"), "");
    EXPECT_EQ(fieldValue(values, "vni"), std::to_string(TEST_VNI));
}

/* Zero is the "no group" sentinel, so such a MAC still needs a VTEP and is the
 * bridge-side copy when it has none. */
TEST_F(MacSyncTest, RemoteMacWithZeroNexthopGroupIsNotProgrammed)
{
    MacMsg req;
    buildMacMsg(req, RTM_NEWNEIGH, NUD_REACHABLE);
    addVlan(req, TEST_VLAN);
    addSrcVni(req, TEST_VNI);
    addNhgId(req, 0);
    feed(req);

    std::vector<FieldValueTuple> values;
    EXPECT_FALSE(getEntry(TEST_KEY, values));
}

/* An ES-backed MAC is withdrawn the same way as a VTEP-backed one. */
TEST_F(MacSyncTest, EsBackedRemoteMacWithdrawn)
{
    MacMsg add;
    buildMacMsg(add, RTM_NEWNEIGH, NUD_REACHABLE);
    addVlan(add, TEST_VLAN);
    addSrcVni(add, TEST_VNI);
    addNhgId(add, 4242);
    feed(add);

    std::vector<FieldValueTuple> values;
    ASSERT_TRUE(getEntry(TEST_KEY, values));

    MacMsg del;
    buildMacMsg(del, RTM_DELNEIGH, NUD_REACHABLE);
    addVlan(del, TEST_VLAN);
    addNhgId(del, 4242);
    feed(del);

    EXPECT_FALSE(getEntry(TEST_KEY, values));
}

/*
 * fpm mode used to be refused outright while Ethernet Segments existed, because
 * an ES-backed MAC was dropped for want of a VTEP. Now that such a MAC is
 * carried as a nexthop group the two features coexist, so the mode must be
 * accepted and the MAC still programmed.
 */
TEST_F(MacSyncTest, FpmModeCoexistsWithEvpnMultihoming)
{
    Table cfgEs(m_cfgDb.get(), "EVPN_ETHERNET_SEGMENT");
    cfgEs.set("PortChannel121", std::vector<FieldValueTuple>{{"esi", "AUTO"}});

    m_macSync->setMacSyncMode("kernel");
    m_macSync->setMacSyncMode("fpm");

    EXPECT_TRUE(m_macSync->isFpmMode());

    MacMsg req;
    buildMacMsg(req, RTM_NEWNEIGH, NUD_REACHABLE);
    addVlan(req, TEST_VLAN);
    addSrcVni(req, TEST_VNI);
    addNhgId(req, 4242);
    feed(req);

    std::vector<FieldValueTuple> values;
    ASSERT_TRUE(getEntry(TEST_KEY, values));
    EXPECT_EQ(fieldValue(values, "nexthop_group"), "4242");

    cfgEs.del("PortChannel121");
}

TEST_F(MacSyncTest, L3EvpnMhRunsFpmModeWhateverFdbSyncSays)
{
    Table fdbSync(m_cfgDb.get(), CFG_FDB_SYNC_TABLE_NAME);
    fdbSync.set("global", std::vector<FieldValueTuple>{{"mac_sync_mode", "kernel"}});

    MacSync plain(m_pipeline.get(), m_stateDb.get(), m_cfgDb.get());
    EXPECT_FALSE(plain.isFpmMode());

    Table deviceMetadata(m_cfgDb.get(), CFG_DEVICE_METADATA_TABLE_NAME);
    deviceMetadata.set("localhost", std::vector<FieldValueTuple>{{"subtype", "L3EvpnMH"}});

    MacSync l3EvpnMh(m_pipeline.get(), m_stateDb.get(), m_cfgDb.get());
    EXPECT_TRUE(l3EvpnMh.isFpmMode());

    l3EvpnMh.setMacSyncMode("kernel");
    EXPECT_TRUE(l3EvpnMh.isFpmMode());
    l3EvpnMh.setMacSyncMode("");
    EXPECT_TRUE(l3EvpnMh.isFpmMode());
}
