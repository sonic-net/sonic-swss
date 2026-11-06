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
#include "selectabletimer.h"

#include <gtest/gtest.h>

#include <net/if.h>

#include <cerrno>
#include <cstring>
#include <map>
#include <set>
#include <vector>

#include <linux/if_ether.h>
#include <linux/neighbour.h>
#include <linux/nexthop.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netlink/route/link.h>

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
            (struct rtattr *)(void *)(((char *)&req.n) + NLMSG_ALIGN(req.n.nlmsg_len));
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

    /* Where the next attribute goes, so a test can corrupt it once added. */
    struct rtattr *nextAttr(MacMsg& req)
    {
        return (struct rtattr *)(void *)(((char *)&req.n) + NLMSG_ALIGN(req.n.nlmsg_len));
    }

    void feed(MacMsg& req, MacSync *macSync = nullptr)
    {
        int len = (int)(req.n.nlmsg_len - NLMSG_LENGTH(sizeof(struct ndmsg)));
        (macSync ? macSync : m_macSync.get())->onMacMsg(&req.n, len);
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

    /* orchagent has not read MacSync's last write to the row yet. */
    void unread(const std::string& k)
    {
        Table appFdb(m_appDb.get(), APP_VXLAN_FDB_TABLE_NAME);
        appFdb.del(k);
    }

    std::size_t rowDels() { return testing_db::getProducerDelCount(APP_VXLAN_FDB_TABLE_NAME); }

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
        return static_cast<const nlmsghdr *>(static_cast<const void *>(m_sent.at(i).data()));
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
        (const struct rtattr *)(const void *)((const char *)ndm + NLMSG_ALIGN(sizeof(struct ndmsg)));

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
 * sticky and remote PEs reject a move for it (RFC 7432 section 15.2). Only local
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

/*
 * orchagent can also refresh the entry once the port is gone. The refresh cannot be
 * sent, so zebra still holds the MAC on the ifindex it last heard, whichever port
 * the refresh names, and the delete must go there, even once that port resolves.
 */
TEST_F(MacSyncTest, LocalMacRefreshAfterThePortIsGoneKeepsItsIfindex)
{
    RecordingFpm fpm;
    m_macSync->onFpmConnected(fpm);

    KeyOpFieldsValuesTuple add{TEST_KEY, SET_COMMAND, {{"port", "Ethernet0"}, {"type", "dynamic"}}};
    m_macSync->m_ifNameToIndex = [](const std::string&) { return 77u; };
    m_macSync->processStateFdbEntry(add);
    ASSERT_EQ(fpm.count(), 1u);

    m_macSync->m_ifNameToIndex = [](const std::string&) { return 0u; };
    m_macSync->processStateFdbEntry(add);
    KeyOpFieldsValuesTuple moved{TEST_KEY, SET_COMMAND, {{"port", "Ethernet4"}, {"type", "dynamic"}}};
    m_macSync->processStateFdbEntry(moved);
    EXPECT_EQ(fpm.count(), 1u);

    m_macSync->m_ifNameToIndex = [](const std::string& port) { return port == "Ethernet4" ? 88u : 0u; };
    KeyOpFieldsValuesTuple del{TEST_KEY, DEL_COMMAND, {}};
    m_macSync->processStateFdbEntry(del);

    ASSERT_EQ(fpm.count(), 2u);
    EXPECT_EQ(fpm.at(1)->nlmsg_type, RTM_DELNEIGH);
    const struct ndmsg *ndm = (const struct ndmsg *)NLMSG_DATA(fpm.at(1));
    EXPECT_EQ(ndm->ndm_ifindex, 77);
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
 * bridge-side duplicate. zebra's NUD_NOARP only keeps it from ageing: the row is
 * dynamic, so FdbOrch installs a static entry that a local learn may move.
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
    EXPECT_EQ(fieldValue(values, "type"), "dynamic");

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
 * A VTEP is 4 or 16 bytes. A destination of any other size is malformed, not "no
 * VTEP": on an Ethernet Segment port it would replace the MAC's row with that port,
 * so the message is dropped and the row left as it was.
 */
TEST_F(MacSyncTest, RemoteMacWithMalformedVtepIsDropped)
{
    Table cfgEs(m_cfgDb.get(), "EVPN_ETHERNET_SEGMENT");
    cfgEs.set("lo", std::vector<FieldValueTuple>{{"esi", "AUTO"}});

    MacMsg add;
    buildMacMsg(add, RTM_NEWNEIGH, NUD_REACHABLE);
    add.ndm.ndm_ifindex = (int)if_nametoindex("lo");
    addVlan(add, TEST_VLAN);
    addSrcVni(add, TEST_VNI);
    addVtep(add, "10.1.1.1");
    feed(add);

    const uint8_t partial[5] = {10, 2, 2, 2, 2};
    const size_t lengths[] = {2, 5};
    for (size_t len : lengths)
    {
        MacMsg req;
        buildMacMsg(req, RTM_NEWNEIGH, NUD_REACHABLE | NUD_NOARP);
        req.ndm.ndm_ifindex = (int)if_nametoindex("lo");
        addVlan(req, TEST_VLAN);
        addSrcVni(req, TEST_VNI);
        addAttr(req, NDA_DST, partial, len);
        feed(req);
    }

    std::vector<FieldValueTuple> values;
    ASSERT_TRUE(getEntry(TEST_KEY, values));
    EXPECT_EQ(fieldValue(values, "remote_vtep"), "10.1.1.1");
    EXPECT_EQ(fieldValue(values, "ifname"), "");
    EXPECT_EQ(fieldValue(values, "type"), "dynamic");

    cfgEs.del("lo");
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
    Table fdbSync(m_cfgDb.get(), CFG_FDB_SYNC_TABLE_NAME);
    fdbSync.set("global", std::vector<FieldValueTuple>{{"mac_sync_mode", "fpm"}});

    MacSync macSync(m_pipeline.get(), m_stateDb.get(), m_cfgDb.get());
    EXPECT_TRUE(macSync.isFpmMode());

    MacMsg req;
    buildMacMsg(req, RTM_NEWNEIGH, NUD_REACHABLE);
    addVlan(req, TEST_VLAN);
    addSrcVni(req, TEST_VNI);
    addNhgId(req, 4242);
    feed(req, &macSync);

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

    /* fdbsyncd follows the mode fpmsyncd runs, not the one configured. */
    std::string published;
    ASSERT_TRUE(Table(m_stateDb.get(), STATE_FDB_SYNC_TABLE_NAME).hget("global", "mac_sync_mode", published));
    EXPECT_EQ(published, "fpm");
}

/*
 * The mode is read once, at startup, as zebra reads --kernel-mac-ext-learn, so a
 * change to FDB_SYNC applies on the next bgp restart and never splits the two ends
 * of the FPM channel. It is published for fdbsyncd, which runs in the swss container
 * and so is not restarted with bgp.
 */
TEST_F(MacSyncTest, MacSyncModeIsReadAtStartupAndPublished)
{
    Table stateFdbSync(m_stateDb.get(), STATE_FDB_SYNC_TABLE_NAME);
    std::string published;

    MacSync unset(m_pipeline.get(), m_stateDb.get(), m_cfgDb.get());
    EXPECT_FALSE(unset.isFpmMode());
    ASSERT_TRUE(stateFdbSync.hget("global", "mac_sync_mode", published));
    EXPECT_EQ(published, "kernel");

    Table fdbSync(m_cfgDb.get(), CFG_FDB_SYNC_TABLE_NAME);
    fdbSync.set("global", std::vector<FieldValueTuple>{{"mac_sync_mode", "fpm"}});

    MacSync fpm(m_pipeline.get(), m_stateDb.get(), m_cfgDb.get());
    EXPECT_TRUE(fpm.isFpmMode());
    EXPECT_FALSE(unset.isFpmMode());
    ASSERT_TRUE(stateFdbSync.hget("global", "mac_sync_mode", published));
    EXPECT_EQ(published, "fpm");
}

/*
 * set() merges fields, so a MAC that moves from a VTEP to a nexthop group must
 * not keep its remote_vtep: fdbOrch would pick one of the two destinations.
 */
TEST_F(MacSyncTest, RemoteMacDestinationChangeReplacesTheRow)
{
    MacMsg vtep;
    buildMacMsg(vtep, RTM_NEWNEIGH, NUD_REACHABLE);
    addVlan(vtep, TEST_VLAN);
    addVtep(vtep, "10.1.1.1");
    addSrcVni(vtep, TEST_VNI);
    feed(vtep);

    MacMsg nhg;
    buildMacMsg(nhg, RTM_NEWNEIGH, NUD_REACHABLE);
    addVlan(nhg, TEST_VLAN);
    addSrcVni(nhg, TEST_VNI);
    addNhgId(nhg, 4242);
    feed(nhg);

    std::vector<FieldValueTuple> values;
    ASSERT_TRUE(getEntry(TEST_KEY, values));
    EXPECT_EQ(fieldValue(values, "nexthop_group"), "4242");
    EXPECT_EQ(fieldValue(values, "remote_vtep"), "");
}

/*
 * A stream that ends in an incomplete attribute stops the parse there. The
 * attributes before it are well formed, so the message must be dropped rather
 * than programmed without the VNI that follows.
 */
TEST_F(MacSyncTest, RemoteMacWithMalformedTailIsDropped)
{
    MacMsg overrun;
    buildMacMsg(overrun, RTM_NEWNEIGH, NUD_REACHABLE);
    addVlan(overrun, TEST_VLAN);
    addVtep(overrun, "10.1.1.1");
    struct rtattr *vni = nextAttr(overrun);
    addSrcVni(overrun, TEST_VNI);
    vni->rta_len = (unsigned short)RTA_LENGTH(2 * sizeof(uint32_t));
    feed(overrun);

    MacMsg partialHeader;
    buildMacMsg(partialHeader, RTM_NEWNEIGH, NUD_REACHABLE);
    addVlan(partialHeader, TEST_VLAN);
    addVtep(partialHeader, "10.1.1.1");
    addSrcVni(partialHeader, TEST_VNI);
    partialHeader.n.nlmsg_len += 2;
    feed(partialHeader);

    std::vector<FieldValueTuple> values;
    EXPECT_FALSE(getEntry(TEST_KEY, values));
}

/* The last attribute may come without its alignment padding, as the kernel allows. */
TEST_F(MacSyncTest, RemoteMacWithUnpaddedLastAttributeIsAccepted)
{
    MacMsg req;
    buildMacMsg(req, RTM_NEWNEIGH, NUD_REACHABLE);
    addVtep(req, "10.1.1.1");
    addSrcVni(req, TEST_VNI);
    addVlan(req, TEST_VLAN);
    req.n.nlmsg_len -= (uint32_t)(RTA_ALIGN(RTA_LENGTH(sizeof(uint16_t))) - RTA_LENGTH(sizeof(uint16_t)));
    feed(req);

    std::vector<FieldValueTuple> values;
    EXPECT_TRUE(getEntry(TEST_KEY, values));
}

/*
 * zebra withdraws and moves a MAC by key, whoever wrote its row: an earlier
 * fpmsyncd, or fdbsyncd in kernel mode, can leave one this instance never wrote.
 */
TEST_F(MacSyncTest, RemoteMacWithdrawalRemovesARowItDidNotWrite)
{
    Table appFdb(m_appDb.get(), APP_VXLAN_FDB_TABLE_NAME);
    appFdb.set(TEST_KEY, std::vector<FieldValueTuple>{{"remote_vtep", "10.1.1.1"},
                                                      {"type", "dynamic"},
                                                      {"vni", std::to_string(TEST_VNI)}});

    MacMsg del;
    buildMacMsg(del, RTM_DELNEIGH, NUD_REACHABLE);
    addVlan(del, TEST_VLAN);
    addVtep(del, "10.1.1.1");
    feed(del);

    std::vector<FieldValueTuple> values;
    EXPECT_FALSE(getEntry(TEST_KEY, values));
}

TEST_F(MacSyncTest, RemoteMacDestinationChangeReplacesARowItDidNotWrite)
{
    Table appFdb(m_appDb.get(), APP_VXLAN_FDB_TABLE_NAME);
    appFdb.set(TEST_KEY, std::vector<FieldValueTuple>{{"remote_vtep", "10.1.1.1"},
                                                      {"type", "dynamic"},
                                                      {"vni", std::to_string(TEST_VNI)}});

    MacMsg nhg;
    buildMacMsg(nhg, RTM_NEWNEIGH, NUD_REACHABLE);
    addVlan(nhg, TEST_VLAN);
    addSrcVni(nhg, TEST_VNI);
    addNhgId(nhg, 4242);
    feed(nhg);

    std::vector<FieldValueTuple> values;
    ASSERT_TRUE(getEntry(TEST_KEY, values));
    EXPECT_EQ(fieldValue(values, "nexthop_group"), "4242");
    EXPECT_EQ(fieldValue(values, "remote_vtep"), "");
}

/*
 * Deleting a VxLAN netdev flushes its FDB in the kernel, and zebra sends no
 * per-MAC delete for it. The rows of its VLAN must go with it.
 */
namespace {

const unsigned int VTEP_IFINDEX = 1000;
const unsigned int OTHER_VTEP_IFINDEX = 1001;

}

class MacSyncLinkDelTest : public MacSyncTest
{
public:
    void SetUp() override
    {
        MacSyncTest::SetUp();
        m_macSync->m_ifIndexToName = [this](unsigned int ifindex) {
            auto it = m_links.find(ifindex);
            return it == m_links.end() ? std::string() : it->second;
        };
        m_links[VTEP_IFINDEX] = "vtep1-100";
        m_links[OTHER_VTEP_IFINDEX] = "vtep1-200";
    }

    void addRemoteMac(unsigned int ifindex, uint16_t vlan, uint8_t lastOctet,
                      uint16_t type = RTM_NEWNEIGH)
    {
        MacMsg req;
        buildMacMsg(req, type, NUD_REACHABLE);
        setLastOctet(req, lastOctet);
        req.ndm.ndm_ifindex = (int)ifindex;
        addVlan(req, vlan);
        addVtep(req, "10.1.1.1");
        addSrcVni(req, TEST_VNI);
        feed(req);
    }

    /* NDA_LLADDR is the first attribute buildMacMsg() adds. */
    void setLastOctet(MacMsg& req, uint8_t octet)
    {
        auto *rta = (struct rtattr *)(void *)((char *)&req.n + NLMSG_LENGTH(sizeof(struct ndmsg)));
        ((uint8_t *)RTA_DATA(rta))[ETH_ALEN - 1] = octet;
    }

    std::string key(uint16_t vlan, uint8_t lastOctet)
    {
        char mac[18];
        snprintf(mac, sizeof(mac), "00:11:22:33:44:%02x", lastOctet);
        return "Vlan" + std::to_string(vlan) + ":" + mac;
    }

    bool hasEntry(const std::string& k)
    {
        std::vector<FieldValueTuple> values;
        return getEntry(k, values);
    }

    /* A row some other writer left in APPL_DB. */
    void putRow(const std::string& k, const std::vector<FieldValueTuple>& row)
    {
        Table appFdb(m_appDb.get(), APP_VXLAN_FDB_TABLE_NAME);
        appFdb.set(k, row);
    }

    /* Delivers a link deletion the way fpmsyncd's netlink socket does. */
    void delLink(unsigned int ifindex, const char *name, const char *type, int family = AF_UNSPEC)
    {
        struct rtnl_link *link = rtnl_link_alloc();
        rtnl_link_set_ifindex(link, (int)ifindex);
        rtnl_link_set_name(link, name);
        if (type)
        {
            rtnl_link_set_type(link, type);
        }
        rtnl_link_set_family(link, family);
        m_links.erase(ifindex);
        m_routeSync->onMsg(RTM_DELLINK, (struct nl_object *)link);
        rtnl_link_put(link);
    }

    std::map<unsigned int, std::string> m_links;
};

TEST_F(MacSyncLinkDelTest, RemoteMacsWithdrawnWithTheirVxlanLink)
{
    addRemoteMac(VTEP_IFINDEX, 100, 0x01);
    addRemoteMac(VTEP_IFINDEX, 100, 0x02);
    addRemoteMac(OTHER_VTEP_IFINDEX, 200, 0x01);
    ASSERT_TRUE(hasEntry(key(100, 0x01)));
    ASSERT_TRUE(hasEntry(key(100, 0x02)));

    delLink(VTEP_IFINDEX, "vtep1-100", "vxlan");

    EXPECT_FALSE(hasEntry(key(100, 0x01)));
    EXPECT_FALSE(hasEntry(key(100, 0x02)));
    EXPECT_TRUE(hasEntry(key(200, 0x01)));
}

/* An add zebra queued before the netdev went away must not bring a row back. */
TEST_F(MacSyncLinkDelTest, AddOnADeletedLinkIsSkipped)
{
    delLink(VTEP_IFINDEX, "vtep1-100", "vxlan");
    addRemoteMac(VTEP_IFINDEX, 100, 0x01);

    EXPECT_FALSE(hasEntry(key(100, 0x01)));
}

/* A late delete still resolves its row through NDA_VLAN. */
TEST_F(MacSyncLinkDelTest, DeleteOnADeletedLinkStillWithdraws)
{
    addRemoteMac(VTEP_IFINDEX, 100, 0x01);
    m_links.erase(VTEP_IFINDEX);
    addRemoteMac(VTEP_IFINDEX, 100, 0x01, RTM_DELNEIGH);

    EXPECT_FALSE(hasEntry(key(100, 0x01)));
}

/* A VLAN remapped to a new VNI gets a new netdev under the same name. The old
 * one's delete, still queued when zebra sends a MAC on the new one, is read
 * first, so it takes only the old rows. */
TEST_F(MacSyncLinkDelTest, QueuedLinkDeleteIsReadBeforeAMac)
{
    const unsigned int NEW_IFINDEX = 1002;
    bool queued = false;
    m_macSync->setLinkReader([this, &queued]() {
        if (queued)
        {
            queued = false;
            delLink(VTEP_IFINDEX, "vtep1-100", "vxlan");
        }
    });

    addRemoteMac(VTEP_IFINDEX, 100, 0x01);
    m_links[NEW_IFINDEX] = "vtep1-100";
    queued = true;
    addRemoteMac(NEW_IFINDEX, 100, 0x02);

    EXPECT_FALSE(queued);
    EXPECT_FALSE(hasEntry(key(100, 0x01)));
    EXPECT_TRUE(hasEntry(key(100, 0x02)));
    m_macSync->setLinkReader(nullptr);
}

/* A row orchagent has not read yet is not in APPL_DB, and goes all the same. */
TEST_F(MacSyncLinkDelTest, RowOrchagentHasNotReadGoesWithTheLink)
{
    addRemoteMac(VTEP_IFINDEX, 100, 0x01);
    unread(key(100, 0x01));
    auto dels = rowDels();

    delLink(VTEP_IFINDEX, "vtep1-100", "vxlan");

    EXPECT_EQ(rowDels(), dels + 1);
}

/* The VLAN's rows go whoever wrote them; Ethernet Segment port rows (no VTEP,
 * the kernel keeps them on the port) and other VLANs' rows stay. Rows another
 * writer left are the ones the connect marked stale. */
TEST_F(MacSyncLinkDelTest, RowsOfTheVlanGoWhoeverWroteThem)
{
    putRow(key(100, 0x01), {{"remote_vtep", "10.1.1.1"}, {"type", "dynamic"}, {"vni", "20100"}});
    putRow(key(100, 0x02), {{"nexthop_group", "7"}, {"type", "dynamic"}, {"vni", "20100"}});
    putRow(key(100, 0x03), {{"ifname", "PortChannel1"}, {"type", "dynamic"}, {"vni", "0"}});
    putRow(key(1000, 0x01), {{"remote_vtep", "10.1.1.1"}, {"type", "dynamic"}, {"vni", "21000"}});
    ASSERT_TRUE(m_macSync->markRemoteMacsStale(0));

    delLink(VTEP_IFINDEX, "vtep1-100", "vxlan");

    EXPECT_FALSE(hasEntry(key(100, 0x01)));
    EXPECT_FALSE(hasEntry(key(100, 0x02)));
    EXPECT_TRUE(hasEntry(key(100, 0x03)));
    EXPECT_TRUE(hasEntry(key(1000, 0x01)));
    EXPECT_EQ(m_macSync->m_staleRemoteMacs.count(key(100, 0x01)), 0u);
    EXPECT_EQ(m_macSync->m_staleRemoteMacs.count(key(100, 0x03)), 1u);
}

/* The rows of a deleted netdev are found without searching APPL_DB, which
 * orchagent shares, once per netdev. */
TEST_F(MacSyncLinkDelTest, LinkDeleteDoesNotSearchAppDb)
{
    putRow(key(100, 0x01), {{"remote_vtep", "10.1.1.1"}, {"type", "dynamic"}, {"vni", "20100"}});
    ASSERT_TRUE(m_macSync->markRemoteMacsStale(0));
    addRemoteMac(VTEP_IFINDEX, 100, 0x02);
    testing_db::resetOperationCounters();

    delLink(VTEP_IFINDEX, "vtep1-100", "vxlan");
    delLink(OTHER_VTEP_IFINDEX, "vtep1-200", "vxlan");

    EXPECT_EQ(testing_db::getGetKeysCount(APP_VXLAN_FDB_TABLE_NAME), 0u);
    EXPECT_EQ(testing_db::getGetCount(APP_VXLAN_FDB_TABLE_NAME), 0u);
    EXPECT_FALSE(hasEntry(key(100, 0x01)));
    EXPECT_FALSE(hasEntry(key(100, 0x02)));
}

/* A row seen at an earlier connect and kept without its fields is read back
 * to tell an Ethernet Segment port row from a remote one. */
TEST_F(MacSyncLinkDelTest, StaleRowKeptWithoutItsFieldsIsReadBack)
{
    ASSERT_FALSE(m_macSync->markRemoteMacsStale(0));
    putRow(key(100, 0x01), {{"remote_vtep", "10.1.1.1"}, {"type", "dynamic"}, {"vni", "20100"}});
    putRow(key(100, 0x03), {{"ifname", "PortChannel1"}, {"type", "dynamic"}, {"vni", "0"}});
    ASSERT_TRUE(m_macSync->markRemoteMacsStale(0));
    ASSERT_TRUE(m_macSync->m_staleRemoteMacs[key(100, 0x01)].empty());
    ASSERT_TRUE(m_macSync->m_staleRemoteMacs[key(100, 0x03)].empty());

    delLink(VTEP_IFINDEX, "vtep1-100", "vxlan");

    EXPECT_FALSE(hasEntry(key(100, 0x01)));
    EXPECT_TRUE(hasEntry(key(100, 0x03)));
}

/* A queued row is marked by its key alone, and zebra's unchanged replay of it
 * is still found unchanged. */
TEST_F(MacSyncLinkDelTest, QueuedRowIsMarkedByKeyAlone)
{
    addRemoteMac(VTEP_IFINDEX, 100, 0x01);
    ASSERT_TRUE(m_macSync->markRemoteMacsStale(0));
    ASSERT_EQ(m_macSync->m_staleRemoteMacs.count(key(100, 0x01)), 1u);
    EXPECT_TRUE(m_macSync->m_staleRemoteMacs[key(100, 0x01)].empty());

    testing_db::resetOperationCounters();
    addRemoteMac(VTEP_IFINDEX, 100, 0x01);
    EXPECT_EQ(testing_db::getProducerSetCount(APP_VXLAN_FDB_TABLE_NAME), 0u);
    EXPECT_TRUE(m_macSync->m_staleRemoteMacs.empty());

    delLink(VTEP_IFINDEX, "vtep1-100", "vxlan");
    EXPECT_FALSE(hasEntry(key(100, 0x01)));
}

/* A port leaving the bridge (AF_BRIDGE) and a non-VxLAN netdev flush nothing:
 * the kernel keeps a VxLAN netdev's own FDB in the first case, and has none of
 * these rows in the second. */
TEST_F(MacSyncLinkDelTest, OnlyAVxlanNetdevDeletionFlushes)
{
    addRemoteMac(VTEP_IFINDEX, 100, 0x01);

    delLink(VTEP_IFINDEX, "vtep1-100", "vxlan", AF_BRIDGE);
    m_links[VTEP_IFINDEX] = "vtep1-100";
    EXPECT_TRUE(hasEntry(key(100, 0x01)));

    delLink(VTEP_IFINDEX, "vtep1-100", nullptr);
    m_links[VTEP_IFINDEX] = "vtep1-100";
    EXPECT_TRUE(hasEntry(key(100, 0x01)));

    delLink(VTEP_IFINDEX, "vtep1-100", "bond");
    EXPECT_TRUE(hasEntry(key(100, 0x01)));
}

/* fdbsyncd owns APPL_DB in kernel mode. */
TEST_F(MacSyncLinkDelTest, KernelModeLeavesRowsToFdbsyncd)
{
    putRow(key(100, 0x01), {{"remote_vtep", "10.1.1.1"}, {"type", "dynamic"}, {"vni", "20100"}});
    m_macSync->m_fpmMode = false;

    delLink(VTEP_IFINDEX, "vtep1-100", "vxlan");

    EXPECT_TRUE(hasEntry(key(100, 0x01)));
}

/* A netdev not named <tunnel>-<vid> names no VLAN, so no row is taken. */
TEST_F(MacSyncLinkDelTest, UnconventionalNameWithdrawsNothing)
{
    m_links[VTEP_IFINDEX] = "vxlanif";
    MacMsg req;
    buildMacMsg(req, RTM_NEWNEIGH, NUD_REACHABLE);
    req.ndm.ndm_ifindex = (int)VTEP_IFINDEX;
    addVlan(req, TEST_VLAN);
    addVtep(req, "10.1.1.1");
    feed(req);
    ASSERT_TRUE(hasEntry(TEST_KEY));

    delLink(VTEP_IFINDEX, "vxlanif", "vxlan");

    EXPECT_TRUE(hasEntry(TEST_KEY));
}

/*
 * zebra replays the remote MACs it holds on every FPM connect, and after a
 * restart of either side or a dropped connection that is the only way a MAC
 * it withdrew meanwhile can be found out. Rows it does not send again go,
 * whoever wrote them.
 */
class MacSyncStaleTest : public MacSyncLinkDelTest
{
public:
    /* Rows an earlier fpmsyncd, or fdbsyncd in kernel mode, left in APPL_DB. */
    void putRows(std::initializer_list<std::string> keys)
    {
        for (const auto& k : keys)
        {
            putRow(k, {{"remote_vtep", "10.1.1.1"}, {"type", "dynamic"}, {"vni", std::to_string(TEST_VNI)}});
        }
    }

    /* A bgp warm restart timer unlike any settle rule interval. */
    static constexpr time_t WARM_TIMER = 300;

    std::chrono::steady_clock::time_point m_t0 = std::chrono::steady_clock::now();
};

constexpr time_t MacSyncStaleTest::WARM_TIMER;

TEST_F(MacSyncStaleTest, ReconnectWithdrawsRowsZebraDoesNotSendAgain)
{
    addRemoteMac(VTEP_IFINDEX, 100, 0x01);
    addRemoteMac(VTEP_IFINDEX, 100, 0x02);

    ASSERT_TRUE(m_macSync->markRemoteMacsStale(0, m_t0));
    addRemoteMac(VTEP_IFINDEX, 100, 0x01);
    addRemoteMac(VTEP_IFINDEX, 100, 0x03);

    EXPECT_EQ(m_macSync->onStaleSweepTimer(m_t0 + std::chrono::seconds(MacSync::STALE_SWEEP_HOLD)), 0);
    EXPECT_TRUE(hasEntry(key(100, 0x01)));
    EXPECT_FALSE(hasEntry(key(100, 0x02)));
    EXPECT_TRUE(hasEntry(key(100, 0x03)));
    EXPECT_TRUE(m_macSync->m_staleRemoteMacs.empty());
}

/* A row zebra withdraws during the replay is gone either way. */
TEST_F(MacSyncStaleTest, DeleteDuringReplayStillWithdraws)
{
    addRemoteMac(VTEP_IFINDEX, 100, 0x01);
    ASSERT_TRUE(m_macSync->markRemoteMacsStale(0, m_t0));

    addRemoteMac(VTEP_IFINDEX, 100, 0x01, RTM_DELNEIGH);
    EXPECT_FALSE(hasEntry(key(100, 0x01)));
    EXPECT_TRUE(m_macSync->m_staleRemoteMacs.empty());

    testing_db::resetOperationCounters();
    EXPECT_EQ(m_macSync->onStaleSweepTimer(m_t0 + std::chrono::seconds(MacSync::STALE_SWEEP_HOLD)), 0);
    EXPECT_EQ(testing_db::getProducerDelCount(APP_VXLAN_FDB_TABLE_NAME), 0u);
}

/* The replay sends every MAC again; an unchanged row is not rewritten, so the
 * ASIC entry is not removed and added back. */
TEST_F(MacSyncStaleTest, UnchangedRowIsNotRewritten)
{
    putRows({key(100, 0x01)});
    addRemoteMac(VTEP_IFINDEX, 100, 0x02);
    ASSERT_TRUE(m_macSync->markRemoteMacsStale(0, m_t0));

    testing_db::resetOperationCounters();
    addRemoteMac(VTEP_IFINDEX, 100, 0x01);
    addRemoteMac(VTEP_IFINDEX, 100, 0x02);

    EXPECT_EQ(testing_db::getProducerDelCount(APP_VXLAN_FDB_TABLE_NAME), 0u);
    EXPECT_EQ(testing_db::getProducerSetCount(APP_VXLAN_FDB_TABLE_NAME), 0u);

    m_macSync->withdrawStaleRemoteMacs();
    EXPECT_TRUE(hasEntry(key(100, 0x01)));
    EXPECT_TRUE(hasEntry(key(100, 0x02)));
}

/* Outside a replay an add is written again even when unchanged. */
TEST_F(MacSyncStaleTest, LiveAddRewritesTheRow)
{
    addRemoteMac(VTEP_IFINDEX, 100, 0x01);

    testing_db::resetOperationCounters();
    addRemoteMac(VTEP_IFINDEX, 100, 0x01);

    EXPECT_EQ(testing_db::getProducerDelCount(APP_VXLAN_FDB_TABLE_NAME), 1u);
    EXPECT_EQ(testing_db::getProducerSetCount(APP_VXLAN_FDB_TABLE_NAME), 1u);
    EXPECT_TRUE(hasEntry(key(100, 0x01)));
}

/* A MAC that moved to another VTEP while the connection was down is replaced. */
TEST_F(MacSyncStaleTest, ChangedRowIsReplaced)
{
    addRemoteMac(VTEP_IFINDEX, 100, 0x01);
    ASSERT_TRUE(m_macSync->markRemoteMacsStale(0, m_t0));

    MacMsg req;
    buildMacMsg(req, RTM_NEWNEIGH, NUD_REACHABLE);
    setLastOctet(req, 0x01);
    req.ndm.ndm_ifindex = (int)VTEP_IFINDEX;
    addVlan(req, 100);
    addVtep(req, "10.2.2.2");
    addSrcVni(req, TEST_VNI);
    feed(req);

    m_macSync->withdrawStaleRemoteMacs();
    std::vector<FieldValueTuple> values;
    ASSERT_TRUE(getEntry(key(100, 0x01), values));
    EXPECT_EQ(fieldValue(values, "remote_vtep"), "10.2.2.2");
}

/* After a bgp restart the rows the old processes left in APPL_DB are
 * reconciled against what the new zebra sends. */
TEST_F(MacSyncStaleTest, RestartWithdrawsRowsTheNewZebraDoesNotSend)
{
    putRows({key(100, 0x01), key(100, 0x02)});
    ASSERT_TRUE(m_macSync->markRemoteMacsStale(0, m_t0));

    addRemoteMac(VTEP_IFINDEX, 100, 0x01);
    addRemoteMac(VTEP_IFINDEX, 100, 0x03);
    EXPECT_EQ(m_macSync->onStaleSweepTimer(m_t0 + std::chrono::seconds(MacSync::STALE_SWEEP_HOLD)), 0);

    EXPECT_TRUE(hasEntry(key(100, 0x01)));
    EXPECT_FALSE(hasEntry(key(100, 0x02)));
    EXPECT_TRUE(hasEntry(key(100, 0x03)));
}

/* A row orchagent has not read yet is not in APPL_DB, and is stale all the
 * same: zebra may have withdrawn the MAC while the connection was down. */
TEST_F(MacSyncStaleTest, RowOrchagentHasNotReadIsMarked)
{
    ASSERT_FALSE(m_macSync->markRemoteMacsStale(0, m_t0));
    addRemoteMac(VTEP_IFINDEX, 100, 0x01);
    unread(key(100, 0x01));

    ASSERT_TRUE(m_macSync->markRemoteMacsStale(0, m_t0));
    auto dels = rowDels();
    m_macSync->withdrawStaleRemoteMacs();
    EXPECT_EQ(rowDels(), dels + 1);
}

/* A row MacSync withdrew is still in APPL_DB until orchagent reads the delete,
 * so zebra's replay of the MAC writes it again instead of keeping it. */
TEST_F(MacSyncStaleTest, WithdrawnRowOrchagentHasNotReadIsWrittenAgain)
{
    ASSERT_FALSE(m_macSync->markRemoteMacsStale(0, m_t0));
    addRemoteMac(VTEP_IFINDEX, 100, 0x01);
    addRemoteMac(VTEP_IFINDEX, 100, 0x01, RTM_DELNEIGH);
    putRows({key(100, 0x01)});
    ASSERT_TRUE(m_macSync->markRemoteMacsStale(0, m_t0));

    testing_db::resetOperationCounters();
    addRemoteMac(VTEP_IFINDEX, 100, 0x01);
    EXPECT_EQ(testing_db::getProducerSetCount(APP_VXLAN_FDB_TABLE_NAME), 1u);
}

/* Each connect starts again from what APPL_DB holds. */
TEST_F(MacSyncStaleTest, SecondConnectMarksTheRowsAgain)
{
    putRows({key(100, 0x01), key(100, 0x02)});
    ASSERT_TRUE(m_macSync->markRemoteMacsStale(0, m_t0));
    addRemoteMac(VTEP_IFINDEX, 100, 0x01);

    ASSERT_TRUE(m_macSync->markRemoteMacsStale(0, m_t0));
    EXPECT_EQ(m_macSync->m_staleRemoteMacs.size(), 2u);
    addRemoteMac(VTEP_IFINDEX, 100, 0x02);
    m_macSync->withdrawStaleRemoteMacs();

    EXPECT_FALSE(hasEntry(key(100, 0x01)));
    EXPECT_TRUE(hasEntry(key(100, 0x02)));
}

/* The sweep waits for zebra's replay, which runs behind its routes, to end. */
TEST_F(MacSyncStaleTest, SweepWaitsForTheChannelToGoQuiet)
{
    using std::chrono::seconds;

    addRemoteMac(VTEP_IFINDEX, 100, 0x01);
    addRemoteMac(VTEP_IFINDEX, 100, 0x02);
    ASSERT_TRUE(m_macSync->markRemoteMacsStale(0, m_t0));
    addRemoteMac(VTEP_IFINDEX, 100, 0x02);

    auto hold = m_t0 + seconds(MacSync::STALE_SWEEP_HOLD);
    m_macSync->noteFpmMessage(hold - seconds(1));
    EXPECT_EQ(m_macSync->onStaleSweepTimer(hold), MacSync::STALE_SWEEP_QUIET - 1);
    EXPECT_TRUE(hasEntry(key(100, 0x01)));

    EXPECT_EQ(m_macSync->onStaleSweepTimer(hold + seconds(MacSync::STALE_SWEEP_QUIET - 1)), 0);
    EXPECT_FALSE(hasEntry(key(100, 0x01)));
    EXPECT_TRUE(hasEntry(key(100, 0x02)));
}

/*
 * The first remote MAC of a burst can be read in the same poll that returns
 * the stale timer, before the main loop notes the FPM message: the channel is
 * not quiet then.
 */
TEST_F(MacSyncStaleTest, FirstRemoteMacOfABurstIsNotQuiet)
{
    using std::chrono::seconds;

    putRows({key(100, 0x01)});
    auto connect = std::chrono::steady_clock::now() - seconds(MacSync::STALE_SWEEP_HOLD + 5);
    ASSERT_TRUE(m_macSync->markRemoteMacsStale(0, connect));
    m_macSync->noteFpmMessage(connect);

    addRemoteMac(VTEP_IFINDEX, 100, 0x02);
    EXPECT_GT(m_macSync->onStaleSweepTimer(std::chrono::steady_clock::now()), 0);
    EXPECT_TRUE(hasEntry(key(100, 0x01)));
}

/* No remote MAC from zebra yet means the EVPN sessions are not up, however
 * quiet the channel: the rows stay until the bound. */
TEST_F(MacSyncStaleTest, SweepWaitsForARemoteMacFromZebra)
{
    using std::chrono::seconds;

    putRows({key(100, 0x01)});
    ASSERT_TRUE(m_macSync->markRemoteMacsStale(0, m_t0));

    EXPECT_EQ(m_macSync->onStaleSweepTimer(m_t0 + seconds(MacSync::STALE_SWEEP_HOLD)),
              MacSync::STALE_SWEEP_QUIET);
    EXPECT_TRUE(hasEntry(key(100, 0x01)));

    EXPECT_EQ(m_macSync->onStaleSweepTimer(m_t0 + seconds(MacSync::STALE_SWEEP_MAX)), 0);
    EXPECT_FALSE(hasEntry(key(100, 0x01)));
}

/* A channel that never goes quiet still gets its sweep. */
TEST_F(MacSyncStaleTest, SweepRunsByTheBoundOnABusyChannel)
{
    using std::chrono::seconds;

    addRemoteMac(VTEP_IFINDEX, 100, 0x01);
    ASSERT_TRUE(m_macSync->markRemoteMacsStale(0, m_t0));

    auto bound = m_t0 + seconds(MacSync::STALE_SWEEP_MAX);
    m_macSync->noteFpmMessage(bound - seconds(1));
    EXPECT_GT(m_macSync->onStaleSweepTimer(bound - seconds(1)), 0);
    EXPECT_EQ(m_macSync->onStaleSweepTimer(bound), 0);
    EXPECT_FALSE(hasEntry(key(100, 0x01)));
}

/* fpmsyncd re-arms macStaleTimer from its own expiry, while the periodic timer
 * still runs; a start() without the stop() keeps the first interval. */
TEST(MacStaleTimerTest, ReArmWhileRunningTakesTheNewInterval)
{
    SelectableTimer timer(timespec{0, 0});
    timer.setInterval(timespec{1, 0});
    timer.start();

    timer.stop();
    timer.setInterval(timespec{3, 0});
    timer.start();

    itimerspec armed;
    ASSERT_EQ(timerfd_gettime(timer.getFd(), &armed), 0);
    EXPECT_EQ(armed.it_interval.tv_sec, 3);
    EXPECT_GE(armed.it_value.tv_sec, 2);
    timer.stop();
}

/* Nothing in APPL_DB starts no sweep. */
TEST_F(MacSyncStaleTest, EmptyTableMarksNothing)
{
    EXPECT_FALSE(m_macSync->markRemoteMacsStale(0, m_t0));
}

/* fdbsyncd owns APPL_DB in kernel mode, and nothing there is swept. */
TEST_F(MacSyncStaleTest, KernelModeMarksNothing)
{
    putRows({key(100, 0x01)});
    m_macSync->m_fpmMode = false;

    EXPECT_FALSE(m_macSync->markRemoteMacsStale(0, m_t0));
    m_macSync->withdrawStaleRemoteMacs();
    EXPECT_TRUE(hasEntry(key(100, 0x01)));
}

/* With bgp warm restart off, a connect takes the settle rule. */
TEST_F(MacSyncStaleTest, ColdStartTakesTheSettleRule)
{
    using std::chrono::seconds;

    putRows({key(100, 0x01)});
    EXPECT_EQ(m_macSync->markRemoteMacsStale(0, m_t0), MacSync::STALE_SWEEP_HOLD);
    EXPECT_EQ(m_macSync->onStaleSweepTimer(m_t0 + seconds(MacSync::STALE_SWEEP_HOLD)),
              MacSync::STALE_SWEEP_QUIET);
    EXPECT_TRUE(hasEntry(key(100, 0x01)));
}

/*
 * In a warm start the rows zebra has not sent again go at the warm restart
 * timer, however long BGP takes and whatever the channel does before then.
 */
TEST_F(MacSyncStaleTest, WarmStartWithdrawsAtTheWarmRestartTimer)
{
    using std::chrono::seconds;

    putRows({key(100, 0x01), key(100, 0x02)});
    EXPECT_EQ(m_macSync->markRemoteMacsStale(WARM_TIMER, m_t0), WARM_TIMER);
    addRemoteMac(VTEP_IFINDEX, 100, 0x02);

    /* zebra has sent a MAC and the channel is quiet: the settle rule would sweep. */
    auto settled = m_t0 + seconds(MacSync::STALE_SWEEP_HOLD + MacSync::STALE_SWEEP_QUIET);
    m_macSync->noteFpmMessage(m_t0);
    EXPECT_EQ(m_macSync->onStaleSweepTimer(settled), WARM_TIMER - MacSync::STALE_SWEEP_HOLD - MacSync::STALE_SWEEP_QUIET);
    EXPECT_TRUE(hasEntry(key(100, 0x01)));

    EXPECT_EQ(m_macSync->onStaleSweepTimer(m_t0 + seconds(WARM_TIMER)), 0);
    EXPECT_FALSE(hasEntry(key(100, 0x01)));
    EXPECT_TRUE(hasEntry(key(100, 0x02)));
}

/* No remote MAC from zebra by the warm restart timer: the rows go anyway. */
TEST_F(MacSyncStaleTest, WarmStartNeedsNoRemoteMacFromZebra)
{
    putRows({key(100, 0x01)});
    ASSERT_TRUE(m_macSync->markRemoteMacsStale(WARM_TIMER, m_t0));

    m_macSync->noteFpmMessage(m_t0 + std::chrono::seconds(WARM_TIMER));
    EXPECT_EQ(m_macSync->onStaleSweepTimer(m_t0 + std::chrono::seconds(WARM_TIMER)), 0);
    EXPECT_FALSE(hasEntry(key(100, 0x01)));
}

/* An FPM drop after the warm start's sweep has zebra still up: settle rule. */
TEST_F(MacSyncStaleTest, ReconnectAfterTheWarmStartTakesTheSettleRule)
{
    using std::chrono::seconds;

    putRows({key(100, 0x01)});
    ASSERT_TRUE(m_macSync->markRemoteMacsStale(WARM_TIMER, m_t0));
    addRemoteMac(VTEP_IFINDEX, 100, 0x01);
    ASSERT_EQ(m_macSync->onStaleSweepTimer(m_t0 + seconds(WARM_TIMER)), 0);

    auto t1 = m_t0 + seconds(1000);
    EXPECT_EQ(m_macSync->markRemoteMacsStale(WARM_TIMER, t1), MacSync::STALE_SWEEP_HOLD);
    EXPECT_EQ(m_macSync->onStaleSweepTimer(t1 + seconds(MacSync::STALE_SWEEP_HOLD)), MacSync::STALE_SWEEP_QUIET);
    EXPECT_TRUE(hasEntry(key(100, 0x01)));

    EXPECT_EQ(m_macSync->onStaleSweepTimer(t1 + seconds(MacSync::STALE_SWEEP_MAX)), 0);
    EXPECT_FALSE(hasEntry(key(100, 0x01)));
}

/* A warm start with nothing stale is over at once; a later connect settles. */
TEST_F(MacSyncStaleTest, ReconnectAfterAnEmptyWarmStartTakesTheSettleRule)
{
    EXPECT_FALSE(m_macSync->markRemoteMacsStale(WARM_TIMER, m_t0));
    addRemoteMac(VTEP_IFINDEX, 100, 0x01);

    EXPECT_EQ(m_macSync->markRemoteMacsStale(WARM_TIMER, m_t0), MacSync::STALE_SWEEP_HOLD);
}

/* An FPM drop before the warm start's sweep keeps the warm restart timer,
 * counted again from the reconnect as the route reconcile is. */
TEST_F(MacSyncStaleTest, ReconnectDuringTheWarmStartKeepsTheWarmRestartTimer)
{
    using std::chrono::seconds;

    putRows({key(100, 0x01), key(100, 0x02)});
    ASSERT_TRUE(m_macSync->markRemoteMacsStale(WARM_TIMER, m_t0));
    addRemoteMac(VTEP_IFINDEX, 100, 0x02);
    ASSERT_GT(m_macSync->onStaleSweepTimer(m_t0 + seconds(60)), 0);

    auto t1 = m_t0 + seconds(60);
    EXPECT_EQ(m_macSync->markRemoteMacsStale(WARM_TIMER, t1), WARM_TIMER);
    EXPECT_EQ(m_macSync->m_staleRemoteMacs.size(), 2u);
    addRemoteMac(VTEP_IFINDEX, 100, 0x02);

    EXPECT_EQ(m_macSync->onStaleSweepTimer(m_t0 + seconds(WARM_TIMER)), 60);
    EXPECT_TRUE(hasEntry(key(100, 0x01)));
    EXPECT_EQ(m_macSync->onStaleSweepTimer(t1 + seconds(WARM_TIMER)), 0);
    EXPECT_FALSE(hasEntry(key(100, 0x01)));
    EXPECT_TRUE(hasEntry(key(100, 0x02)));

    EXPECT_EQ(m_macSync->markRemoteMacsStale(WARM_TIMER, t1 + seconds(1000)), MacSync::STALE_SWEEP_HOLD);
}

/* A row still stale from the last connect, which zebra then replays unchanged,
 * is not rewritten either. */
TEST_F(MacSyncStaleTest, ReconnectKeepsARowStillStaleFromTheLastConnect)
{
    putRows({key(100, 0x01)});
    ASSERT_TRUE(m_macSync->markRemoteMacsStale(WARM_TIMER, m_t0));
    ASSERT_TRUE(m_macSync->markRemoteMacsStale(WARM_TIMER, m_t0));

    testing_db::resetOperationCounters();
    addRemoteMac(VTEP_IFINDEX, 100, 0x01);
    EXPECT_EQ(testing_db::getProducerDelCount(APP_VXLAN_FDB_TABLE_NAME), 0u);
    EXPECT_EQ(testing_db::getProducerSetCount(APP_VXLAN_FDB_TABLE_NAME), 0u);
    EXPECT_TRUE(m_macSync->m_staleRemoteMacs.empty());
}

/* Guards the logic only: within a process checkAndStart() stays true once it
 * returned true, unless restore_count goes missing from STATE_DB. */
TEST_F(MacSyncStaleTest, ReconnectDuringTheWarmStartWithWarmRestartOffSettles)
{
    putRows({key(100, 0x01)});
    ASSERT_TRUE(m_macSync->markRemoteMacsStale(WARM_TIMER, m_t0));

    EXPECT_EQ(m_macSync->markRemoteMacsStale(0, m_t0), MacSync::STALE_SWEEP_HOLD);
    EXPECT_EQ(m_macSync->markRemoteMacsStale(WARM_TIMER, m_t0), MacSync::STALE_SWEEP_HOLD);
}

/* bgp warm restart turned on after a cold start: an FPM drop still settles. */
TEST_F(MacSyncStaleTest, ReconnectAfterAColdStartSettles)
{
    putRows({key(100, 0x01)});
    ASSERT_EQ(m_macSync->markRemoteMacsStale(0, m_t0), MacSync::STALE_SWEEP_HOLD);

    EXPECT_EQ(m_macSync->markRemoteMacsStale(WARM_TIMER, m_t0), MacSync::STALE_SWEEP_HOLD);
}

/*
 * With bgp_eoiu set the routes reconcile at the EOIU hold, which waits only
 * for the unicast EORs; the remote MAC rows wait for the warm restart timer.
 */
TEST_F(MacSyncStaleTest, EoiuReconcileLeavesTheStaleRows)
{
    putRows({key(100, 0x01)});
    ASSERT_TRUE(m_macSync->markRemoteMacsStale(WARM_TIMER, m_t0));

    auto dels = rowDels();
    m_macSync->onWarmStartEnd();
    EXPECT_EQ(rowDels(), dels);
    EXPECT_TRUE(hasEntry(key(100, 0x01)));

    EXPECT_EQ(m_macSync->onStaleSweepTimer(m_t0 + std::chrono::seconds(WARM_TIMER)), 0);
    EXPECT_FALSE(hasEntry(key(100, 0x01)));
}

/*
 * fpmsyncd holds the pipeline during a warm restart, which an FPM reconnect
 * also starts; MacSync's writes tell it to flush them anyway.
 */
TEST_F(MacSyncStaleTest, RemoteMacWritesAskForAFlush)
{
    EXPECT_FALSE(m_macSync->hasPendingWrites());

    addRemoteMac(VTEP_IFINDEX, 100, 0x01);
    EXPECT_TRUE(m_macSync->hasPendingWrites());
    /* Held until the caller has emptied the pipeline. */
    EXPECT_TRUE(m_macSync->hasPendingWrites());
    m_macSync->clearPendingWrites();
    EXPECT_FALSE(m_macSync->hasPendingWrites());

    addRemoteMac(VTEP_IFINDEX, 100, 0x01, RTM_DELNEIGH);
    EXPECT_TRUE(m_macSync->hasPendingWrites());

    addRemoteMac(VTEP_IFINDEX, 100, 0x02);
    m_macSync->clearPendingWrites();
    delLink(VTEP_IFINDEX, "vtep1-100", "vxlan");
    EXPECT_TRUE(m_macSync->hasPendingWrites());

    addRemoteMac(OTHER_VTEP_IFINDEX, 200, 0x03);
    ASSERT_TRUE(m_macSync->markRemoteMacsStale(0, m_t0));
    m_macSync->clearPendingWrites();
    EXPECT_EQ(m_macSync->onStaleSweepTimer(m_t0 + std::chrono::seconds(MacSync::STALE_SWEEP_MAX)), 0);
    EXPECT_TRUE(m_macSync->hasPendingWrites());
}

/* A replayed row that is not rewritten leaves nothing to flush. */
TEST_F(MacSyncStaleTest, UnchangedReplayRowAsksForNoFlush)
{
    putRows({key(100, 0x01)});
    ASSERT_TRUE(m_macSync->markRemoteMacsStale(0, m_t0));
    m_macSync->clearPendingWrites();

    addRemoteMac(VTEP_IFINDEX, 100, 0x01);
    EXPECT_FALSE(m_macSync->hasPendingWrites());

    addRemoteMac(VTEP_IFINDEX, 100, 0x02);
    EXPECT_TRUE(m_macSync->hasPendingWrites());
}

/*
 * bgpd does not send a route again when only its ESI changes, so a port's
 * local MACs are reported to zebra again (delete, then add back) when the port
 * joins or leaves an Ethernet Segment in a VLAN.
 */
class MacSyncEsReReportTest : public MacSyncTest
{
public:
    void SetUp() override
    {
        MacSyncTest::SetUp();
        m_macSync->m_ifNameToIndex = [](const std::string& name) -> unsigned int {
            if (name == "PortChannel1")
                return 101;
            if (name == "Ethernet4")
                return 102;
            if (name == "Ethernet0")
                return 21;
            return 0;
        };
        m_macSync->onFpmConnected(m_fpm);
        learn("Vlan100:00:11:22:33:44:01", "PortChannel1");
        learn("Vlan100:00:11:22:33:44:02", "PortChannel1");
        learn("Vlan100:00:11:22:33:44:03", "Ethernet4");
        learn("Vlan200:00:11:22:33:44:04", "PortChannel1");
        m_base = m_fpm.count();
    }

    void learn(const std::string& key, const std::string& port)
    {
        m_macSync->processStateFdbEntry(KeyOpFieldsValuesTuple{key, SET_COMMAND,
                                                               {{"port", port}, {"type", "dynamic"}}});
    }

    /* "add 01", "del 02", ... for what was sent since SetUp(). */
    std::vector<std::string> sent()
    {
        std::vector<std::string> out;
        for (size_t i = m_base; i < m_fpm.count(); i++)
        {
            const nlmsghdr *hdr = m_fpm.at(i);
            const struct rtattr *lladdr = findAttr(hdr, NDA_LLADDR);
            char octet[3];
            snprintf(octet, sizeof(octet), "%02x", ((const uint8_t *)RTA_DATA(lladdr))[ETH_ALEN - 1]);
            out.push_back(std::string(hdr->nlmsg_type == RTM_NEWNEIGH ? "add " : "del ") + octet);
        }
        return out;
    }

    std::chrono::steady_clock::time_point due()
    {
        return std::chrono::steady_clock::now() + std::chrono::milliseconds(2 * MacSync::ES_READD_DELAY_MS);
    }

    RecordingFpm m_fpm;
    size_t m_base = 0;
};

TEST_F(MacSyncEsReReportTest, EsAttachReportsThePortsMacsAgain)
{
    m_macSync->onEsPortVlan("Vlan100:PortChannel1", true);
    EXPECT_EQ(sent(), (std::vector<std::string>{"del 01", "del 02"}));

    m_macSync->processEsReAdd(std::chrono::steady_clock::now());
    EXPECT_EQ(sent().size(), 2u);

    m_macSync->processEsReAdd(due());
    EXPECT_EQ(sent(), (std::vector<std::string>{"del 01", "del 02", "add 01", "add 02"}));
    EXPECT_TRUE(m_macSync->m_esReAddPending.empty());
}

TEST_F(MacSyncEsReReportTest, EsRemovalReportsThePortsMacsAgain)
{
    m_macSync->onEsPortVlan("Vlan100:PortChannel1", true);
    m_macSync->processEsReAdd(due());
    m_base = m_fpm.count();

    m_macSync->onEsPortVlan("Vlan100:PortChannel1", false);
    m_macSync->processEsReAdd(due());
    EXPECT_EQ(sent(), (std::vector<std::string>{"del 01", "del 02", "add 01", "add 02"}));
}

/* A DF election or ES peer change rewrites a row that is already there. */
TEST_F(MacSyncEsReReportTest, RowRewriteReportsNothing)
{
    m_macSync->onEsPortVlan("Vlan100:PortChannel1", true);
    m_base = m_fpm.count();

    m_macSync->onEsPortVlan("Vlan100:PortChannel1", true);
    m_macSync->onEsPortVlan("Vlan300:PortChannel1", false);
    EXPECT_TRUE(sent().empty());
    m_macSync->processEsReAdd(due());
    EXPECT_EQ(sent(), (std::vector<std::string>{"add 01", "add 02"}));
}

/* The rows zebra wrote before this daemon started are already known. */
TEST_F(MacSyncEsReReportTest, RowsInAppDbAtStartAreKnown)
{
    Table df(m_appDb.get(), "EVPN_DF_TABLE");
    df.set("Vlan100:PortChannel1", std::vector<FieldValueTuple>{{"df", "true"}});
    MacSync restarted(m_pipeline.get(), m_stateDb.get(), m_cfgDb.get());
    restarted.m_fpmMode = true;
    RecordingFpm fpm;
    restarted.onFpmConnected(fpm);
    restarted.m_ifNameToIndex = m_macSync->m_ifNameToIndex;
    restarted.processStateFdbEntry(KeyOpFieldsValuesTuple{"Vlan100:00:11:22:33:44:01", SET_COMMAND,
                                                          {{"port", "PortChannel1"}, {"type", "dynamic"}}});

    restarted.onEsPortVlan("Vlan100:PortChannel1", true);
    EXPECT_EQ(fpm.count(), 1u);
}

/* At the end of a warm restart the keys the new zebra has not sent again are
 * taken as gone: the segment coming back later is a change. */
TEST_F(MacSyncEsReReportTest, WarmRestartForgetsRowsZebraDidNotSendAgain)
{
    Table df(m_appDb.get(), "EVPN_DF_TABLE");
    df.set("Vlan100:PortChannel1", std::vector<FieldValueTuple>{{"df", "true"}});
    df.set("Vlan200:PortChannel1", std::vector<FieldValueTuple>{{"df", "true"}});
    MacSync restarted(m_pipeline.get(), m_stateDb.get(), m_cfgDb.get());
    restarted.m_fpmMode = true;
    RecordingFpm fpm;
    restarted.onFpmConnected(fpm);
    restarted.m_ifNameToIndex = m_macSync->m_ifNameToIndex;
    restarted.processStateFdbEntry(KeyOpFieldsValuesTuple{"Vlan100:00:11:22:33:44:01", SET_COMMAND,
                                                          {{"port", "PortChannel1"}, {"type", "dynamic"}}});
    restarted.processStateFdbEntry(KeyOpFieldsValuesTuple{"Vlan200:00:11:22:33:44:04", SET_COMMAND,
                                                          {{"port", "PortChannel1"}, {"type", "dynamic"}}});
    ASSERT_EQ(fpm.count(), 2u);

    restarted.onEsPortVlan("Vlan200:PortChannel1", true);
    restarted.onWarmStartEnd();
    EXPECT_EQ(fpm.count(), 2u);

    restarted.onEsPortVlan("Vlan100:PortChannel1", true);
    EXPECT_EQ(fpm.count(), 3u);
    restarted.onEsPortVlan("Vlan200:PortChannel1", true);
    EXPECT_EQ(fpm.count(), 3u);
}

/* An update of the MAC in the window reaches zebra itself; no re-add follows. */
TEST_F(MacSyncEsReReportTest, UpdateInTheWindowCancelsTheReAdd)
{
    m_macSync->onEsPortVlan("Vlan100:PortChannel1", true);
    m_macSync->processStateFdbEntry(KeyOpFieldsValuesTuple{"Vlan100:00:11:22:33:44:01", DEL_COMMAND, {}});
    m_macSync->processEsReAdd(due());

    EXPECT_EQ(sent(), (std::vector<std::string>{"del 01", "del 02", "del 01", "add 02"}));
}

/* zebra stays up across a dropped FPM connection, so the re-add waits for the next one. */
TEST_F(MacSyncEsReReportTest, ReAddWaitsForTheConnection)
{
    m_macSync->onEsPortVlan("Vlan100:PortChannel1", true);
    m_macSync->onFpmDisconnected();
    m_macSync->processEsReAdd(due());
    EXPECT_EQ(sent().size(), 2u);
    EXPECT_EQ(m_macSync->m_esReAddPending.size(), 2u);

    m_macSync->onFpmConnected(m_fpm);
    m_macSync->processEsReAdd(due() + std::chrono::seconds(1));
    EXPECT_EQ(sent(), (std::vector<std::string>{"del 01", "del 02", "add 01", "add 02"}));
}

/* fdbsyncd reports the local MACs in kernel mode. */
TEST_F(MacSyncEsReReportTest, KernelModeReportsNothing)
{
    m_macSync->m_fpmMode = false;
    m_macSync->onEsPortVlan("Vlan100:PortChannel1", true);
    m_macSync->processEsReAdd(due());

    EXPECT_TRUE(sent().empty());
}

/* The EVPN_DF_TABLE row zebra sends for the port reaches MacSync through RouteSync. */
TEST_F(MacSyncEsReReportTest, DfMessageReachesMacSync)
{
    learn("Vlan10:00:11:22:33:44:05", "Ethernet0");
    m_base = m_fpm.count();

    struct
    {
        struct nlmsghdr n;
        struct evpn_df_msg df;
    } msg{};
    msg.n.nlmsg_len = NLMSG_LENGTH(sizeof(struct evpn_df_msg));
    msg.n.nlmsg_type = RTM_FPM_ADD_EVPN_DF;
    msg.df.edm_ifindex = 21;
    msg.df.edm_vid = 10;
    m_routeSync->onMsgRaw(&msg.n);

    EXPECT_EQ(sent(), (std::vector<std::string>{"del 05"}));
}

/*
 * zebra answers the re-report's delete of a MAC an ES peer also advertises by
 * sending the MAC as remote (on the segment's next hop group), and keeps it
 * after the sessions close. Written, the row replaces the hardware's local
 * entry and its STATE_DB delete cancels the re-add, so the MAC stays remote.
 */
class MacSyncEsHeldRowTest : public MacSyncEsReReportTest
{
public:
    static constexpr int VXLAN_IFINDEX = 50;

    void SetUp() override
    {
        MacSyncEsReReportTest::SetUp();
        m_macSync->m_ifIndexToName = [](unsigned int ifindex) {
            return ifindex == VXLAN_IFINDEX ? std::string("vtep1-100") : std::string();
        };
        learn(TEST_KEY, "PortChannel1");
        m_macSync->onEsPortVlan("Vlan100:PortChannel1", true);
        m_macSync->processEsReAdd(due());
        m_macSync->m_esReAddGrace.clear();
        m_base = m_fpm.count();
    }

    /* zebra's remote MAC on VXLAN_IFINDEX: on the ES group, or with vtep on a VTEP. */
    void zebraSendsRemote(uint16_t type = RTM_NEWNEIGH, const char *vtep = nullptr,
                          uint16_t state = NUD_REACHABLE)
    {
        MacMsg req;
        buildMacMsg(req, type, state);
        req.ndm.ndm_ifindex = VXLAN_IFINDEX;
        addSrcVni(req, TEST_VNI);
        if (vtep)
        {
            addVtep(req, vtep);
        }
        else
        {
            addNhgId(req, 536870916);
        }
        feed(req);
    }

    void esChange()
    {
        m_macSync->onEsPortVlan("Vlan100:PortChannel1", false);
        m_base = m_fpm.count();
    }

    bool hasRow()
    {
        std::vector<FieldValueTuple> values;
        return getEntry(TEST_KEY, values);
    }

    std::string rowField(const std::string& field)
    {
        std::vector<FieldValueTuple> values;
        getEntry(TEST_KEY, values);
        return fieldValue(values, field);
    }

    std::vector<std::string> sentFor55()
    {
        std::vector<std::string> out;
        for (const auto& s : sent())
        {
            if (s.compare(s.size() - 2, 2, "55") == 0)
            {
                out.push_back(s);
            }
        }
        return out;
    }
};

constexpr int MacSyncEsHeldRowTest::VXLAN_IFINDEX;

TEST_F(MacSyncEsHeldRowTest, ReAddFiresAndZebrasHeldRowIsNeverWritten)
{
    m_macSync->onEsPortVlan("Vlan100:PortChannel1", false);
    EXPECT_EQ(sentFor55(), (std::vector<std::string>{"del 55"}));

    zebraSendsRemote();
    EXPECT_FALSE(hasRow());

    m_macSync->processEsReAdd(due());
    EXPECT_EQ(sentFor55(), (std::vector<std::string>{"del 55", "add 55"}));
    EXPECT_FALSE(hasRow());
    EXPECT_TRUE(m_macSync->m_esReAddDeferred.empty());
    EXPECT_EQ(m_macSync->m_localMacs.count(TEST_KEY), 1u);
}

/* zebra's answer can be read after the re-add went out. */
TEST_F(MacSyncEsHeldRowTest, AnswerReadAfterTheReAddIsDropped)
{
    esChange();
    m_macSync->processEsReAdd(due());
    zebraSendsRemote();
    EXPECT_FALSE(hasRow());

    m_macSync->m_esReAddGrace[TEST_KEY] = std::chrono::steady_clock::now() - std::chrono::seconds(1);
    zebraSendsRemote();
    EXPECT_EQ(rowField("nexthop_group"), "536870916");
}

TEST_F(MacSyncEsHeldRowTest, StateDbDeleteInTheWindowWritesTheHeldRow)
{
    esChange();
    zebraSendsRemote();
    m_macSync->processStateFdbEntry(KeyOpFieldsValuesTuple{TEST_KEY, DEL_COMMAND, {}});
    EXPECT_EQ(rowField("nexthop_group"), "536870916");

    m_macSync->processEsReAdd(due());
    EXPECT_EQ(sentFor55(), (std::vector<std::string>{"del 55"}));
    EXPECT_TRUE(hasRow());

    zebraSendsRemote(RTM_DELNEIGH);
    EXPECT_FALSE(hasRow());
}

TEST_F(MacSyncEsHeldRowTest, StateDbAddInTheWindowDropsTheHeldRow)
{
    esChange();
    zebraSendsRemote();
    learn(TEST_KEY, "PortChannel1");
    m_macSync->processEsReAdd(due());

    EXPECT_EQ(sentFor55(), (std::vector<std::string>{"add 55"}));
    EXPECT_FALSE(hasRow());
    EXPECT_TRUE(m_macSync->m_esReAddDeferred.empty());
}

/* An update the daemon cannot use leaves the re-add and the held row as they are. */
TEST_F(MacSyncEsHeldRowTest, MalformedStateDbAddKeepsTheReAdd)
{
    esChange();
    zebraSendsRemote();
    m_macSync->processStateFdbEntry(KeyOpFieldsValuesTuple{TEST_KEY, SET_COMMAND, {{"type", "dynamic"}}});

    EXPECT_EQ(m_macSync->m_esReAddPending.count(TEST_KEY), 1u);
    EXPECT_EQ(m_macSync->m_esReAddDeferred.count(TEST_KEY), 1u);
}

TEST_F(MacSyncEsHeldRowTest, ZebraDeleteInTheWindowDropsTheHeldRow)
{
    esChange();
    zebraSendsRemote();
    zebraSendsRemote(RTM_DELNEIGH);
    m_macSync->processStateFdbEntry(KeyOpFieldsValuesTuple{TEST_KEY, DEL_COMMAND, {}});

    EXPECT_FALSE(hasRow());
    EXPECT_TRUE(m_macSync->m_esReAddDeferred.empty());
}

/* A move to a single-homed VTEP is no answer to the re-report. */
TEST_F(MacSyncEsHeldRowTest, MoveToAVtepInTheWindowIsWritten)
{
    esChange();
    zebraSendsRemote(RTM_NEWNEIGH, "10.0.0.9");
    EXPECT_EQ(rowField("remote_vtep"), "10.0.0.9");
    EXPECT_TRUE(m_macSync->m_esReAddDeferred.empty());
}

/* FdbOrch keeps a provisioned local MAC over a sticky remote one either way. */
TEST_F(MacSyncEsHeldRowTest, StickyAnswerIsWritten)
{
    esChange();
    zebraSendsRemote(RTM_NEWNEIGH, nullptr, NUD_REACHABLE | NUD_NOARP);
    EXPECT_EQ(rowField("type"), "static");
}

TEST_F(MacSyncEsHeldRowTest, RowAlreadyThereGoesWhenTheAnswerIsHeld)
{
    zebraSendsRemote(RTM_NEWNEIGH, "10.0.0.9");
    ASSERT_TRUE(hasRow());

    esChange();
    zebraSendsRemote();
    EXPECT_FALSE(hasRow());
    EXPECT_EQ(m_macSync->m_esReAddDeferred.count(TEST_KEY), 1u);
}

TEST_F(MacSyncEsHeldRowTest, EsPortRowInTheWindowReplacesTheHeldRow)
{
    Table cfgEs(m_cfgDb.get(), "EVPN_ETHERNET_SEGMENT");
    cfgEs.set("lo", std::vector<FieldValueTuple>{{"esi", "AUTO"}});
    unsigned int lo = if_nametoindex("lo");
    m_macSync->m_ifIndexToName = [lo](unsigned int ifindex) {
        return ifindex == lo ? std::string("lo") : std::string();
    };

    esChange();
    zebraSendsRemote();
    MacMsg req;
    buildMacMsg(req, RTM_NEWNEIGH, NUD_REACHABLE | NUD_NOARP);
    req.ndm.ndm_ifindex = (int)lo;
    addVlan(req, TEST_VLAN);
    addSrcVni(req, TEST_VNI);
    feed(req);

    EXPECT_EQ(rowField("ifname"), "lo");
    EXPECT_TRUE(m_macSync->m_esReAddDeferred.empty());
    cfgEs.del("lo");
}

/* zebra sends its remote MACs again on the next connect. */
TEST_F(MacSyncEsHeldRowTest, DisconnectDropsTheHeldRow)
{
    esChange();
    zebraSendsRemote();
    m_macSync->onFpmDisconnected();
    m_macSync->processStateFdbEntry(KeyOpFieldsValuesTuple{TEST_KEY, DEL_COMMAND, {}});

    EXPECT_FALSE(hasRow());
    EXPECT_TRUE(m_macSync->m_esReAddDeferred.empty());
}

TEST_F(MacSyncEsHeldRowTest, LinkDeleteDropsTheHeldRow)
{
    esChange();
    zebraSendsRemote();
    m_macSync->onVxlanLinkDel("vtep1-100");
    m_macSync->m_ifIndexToName = [](unsigned int) { return std::string(); };
    m_macSync->processStateFdbEntry(KeyOpFieldsValuesTuple{TEST_KEY, DEL_COMMAND, {}});

    EXPECT_FALSE(hasRow());
    EXPECT_TRUE(m_macSync->m_esReAddDeferred.empty());
}

/* With no re-add sent, zebra keeps the MAC it answered with. */
TEST_F(MacSyncEsHeldRowTest, ReAddNotSentWritesTheHeldRow)
{
    esChange();
    zebraSendsRemote();
    m_macSync->m_ifNameToIndex = [](const std::string&) -> unsigned int { return 0; };
    m_macSync->processEsReAdd(due());

    EXPECT_TRUE(sentFor55().empty());
    EXPECT_EQ(rowField("nexthop_group"), "536870916");
}

/* With no re-add pending, zebra's answer is written at once. */
TEST_F(MacSyncEsHeldRowTest, RemoteMacOutsideTheWindowIsWritten)
{
    zebraSendsRemote();
    EXPECT_EQ(rowField("nexthop_group"), "536870916");
}

/*
 * A local learn replaces a remote MAC in the kernel bridge, so zebra sends no
 * delete for the remote one: the row goes when the MAC is learnt locally.
 */
TEST_F(MacSyncEsHeldRowTest, LocalLearnRemovesTheRemoteRow)
{
    m_macSync->processStateFdbEntry(KeyOpFieldsValuesTuple{TEST_KEY, DEL_COMMAND, {}});
    zebraSendsRemote();
    ASSERT_TRUE(hasRow());

    m_macSync->clearPendingWrites();
    learn(TEST_KEY, "PortChannel1");
    EXPECT_FALSE(hasRow());
    EXPECT_TRUE(m_macSync->hasPendingWrites());
}

/* The row goes whoever wrote it. */
TEST_F(MacSyncEsHeldRowTest, LocalLearnRemovesARowItDidNotWrite)
{
    m_macSync->processStateFdbEntry(KeyOpFieldsValuesTuple{TEST_KEY, DEL_COMMAND, {}});
    Table appFdb(m_appDb.get(), APP_VXLAN_FDB_TABLE_NAME);
    appFdb.set(TEST_KEY, std::vector<FieldValueTuple>{{"remote_vtep", "10.1.1.1"},
                                                      {"type", "dynamic"},
                                                      {"vni", std::to_string(TEST_VNI)}});
    /* As the connect finds it. */
    ASSERT_TRUE(m_macSync->markRemoteMacsStale(0));

    learn(TEST_KEY, "PortChannel1");
    EXPECT_FALSE(hasRow());
    EXPECT_TRUE(m_macSync->m_staleRemoteMacs.empty());
}

/* A row orchagent has not read yet is not in APPL_DB, and goes all the same. */
TEST_F(MacSyncEsHeldRowTest, LocalLearnRemovesARowOrchagentHasNotRead)
{
    m_macSync->processStateFdbEntry(KeyOpFieldsValuesTuple{TEST_KEY, DEL_COMMAND, {}});
    zebraSendsRemote();
    unread(TEST_KEY);
    auto dels = rowDels();

    learn(TEST_KEY, "PortChannel1");
    EXPECT_EQ(rowDels(), dels + 1);
}

/* A local learn with no remote row for the MAC reads nothing from APPL_DB. */
TEST_F(MacSyncEsHeldRowTest, LocalLearnDoesNotReadAppDb)
{
    testing_db::resetOperationCounters();
    learn("Vlan100:00:11:22:33:44:66", "PortChannel1");
    learn("Vlan100:00:11:22:33:44:67", "PortChannel1");

    EXPECT_EQ(testing_db::getGetCount(APP_VXLAN_FDB_TABLE_NAME), 0u);
    EXPECT_EQ(testing_db::getGetKeysCount(APP_VXLAN_FDB_TABLE_NAME), 0u);
}

/* With no remote row, a local learn writes nothing to APPL_DB. */
TEST_F(MacSyncEsHeldRowTest, LocalLearnWithoutARemoteRowWritesNothing)
{
    testing_db::resetOperationCounters();
    m_macSync->clearPendingWrites();
    learn("Vlan100:00:11:22:33:44:66", "PortChannel1");

    EXPECT_EQ(testing_db::getProducerDelCount(APP_VXLAN_FDB_TABLE_NAME), 0u);
    EXPECT_FALSE(m_macSync->hasPendingWrites());
}

/* A row on an Ethernet Segment port is the module's to withdraw. */
TEST_F(MacSyncTest, LocalLearnKeepsAnEthernetSegmentPortRow)
{
    Table cfgEs(m_cfgDb.get(), "EVPN_ETHERNET_SEGMENT");
    cfgEs.set("lo", std::vector<FieldValueTuple>{{"esi", "AUTO"}});
    RecordingFpm fpm;
    m_macSync->onFpmConnected(fpm);

    MacMsg req;
    buildMacMsg(req, RTM_NEWNEIGH, NUD_REACHABLE | NUD_NOARP);
    req.ndm.ndm_ifindex = (int)if_nametoindex("lo");
    addVlan(req, TEST_VLAN);
    addSrcVni(req, TEST_VNI);
    feed(req);
    m_macSync->processStateFdbEntry(KeyOpFieldsValuesTuple{TEST_KEY, SET_COMMAND,
                                                           {{"port", "lo"}, {"type", "dynamic"}}});

    std::vector<FieldValueTuple> values;
    EXPECT_TRUE(getEntry(TEST_KEY, values));
    EXPECT_EQ(fieldValue(values, "ifname"), "lo");

    cfgEs.del("lo");
}

/*
 * zebra programs an Ethernet Segment's L2 nexthop group straight into the
 * kernel, and deleting it there flushes the kernel FDB entries on it. A MAC
 * zebra frees on the group without a delete of its own must lose its row the
 * same way, or the row and the group stay in the ASIC.
 */
class MacSyncNhgFlushTest : public MacSyncTest
{
public:
    static constexpr int VXLAN_IFINDEX = 50;
    static constexpr uint32_t ES_NHG = 536870916;
    static constexpr uint32_t OTHER_NHG = 536870917;
    static constexpr uint32_t VTEP_NH = 11;

    void SetUp() override
    {
        MacSyncTest::SetUp();
        m_macSync->m_ifIndexToName = [](unsigned int ifindex) {
            return ifindex == VXLAN_IFINDEX ? std::string("vtep1-100") : std::string();
        };
        m_realNexthopState = m_macSync->m_kernelNexthopState;
        m_macSync->m_kernelNexthopState = [this](uint32_t id) {
            auto it = m_kernelIds.find(id);
            if (it == m_kernelIds.end())
            {
                return MacSync::NexthopState::Absent;
            }
            return it->second ? MacSync::NexthopState::Fdb : MacSync::NexthopState::Absent;
        };
    }

    /* The ids the kernel holds, and whether each is an FDB nexthop, as
     * RTM_GETNEXTHOP would answer. */
    std::map<uint32_t, bool> m_kernelIds;
    std::function<MacSync::NexthopState(uint32_t)> m_realNexthopState;

    struct NhMsg
    {
        struct nlmsghdr n;
        struct nhmsg nhm;
        char buf[256];
    };

    void addNhAttr(NhMsg& req, int type, const void *data, size_t alen)
    {
        size_t len = RTA_LENGTH(alen);
        struct rtattr *rta =
            (struct rtattr *)(void *)(((char *)&req.n) + NLMSG_ALIGN(req.n.nlmsg_len));
        rta->rta_type = (unsigned short)type;
        rta->rta_len = (unsigned short)len;
        if (alen)
        {
            memcpy(RTA_DATA(rta), data, alen);
        }
        req.n.nlmsg_len = (uint32_t)(NLMSG_ALIGN(req.n.nlmsg_len) + RTA_ALIGN(len));
    }

    /* A kernel nexthop change and its notification; members make it a group. */
    void kernelNh(uint16_t type, uint32_t id, bool fdb = true, std::vector<uint32_t> members = {VTEP_NH})
    {
        if (type == RTM_NEWNEXTHOP)
        {
            m_kernelIds[id] = fdb;
        }
        else
        {
            m_kernelIds.erase(id);
        }

        NhMsg req;
        memset(&req, 0, sizeof(req));
        req.n.nlmsg_len = NLMSG_LENGTH(sizeof(struct nhmsg));
        req.n.nlmsg_type = type;
        req.nhm.nh_family = AF_UNSPEC;
        addNhAttr(req, NHA_ID, &id, sizeof(id));
        if (!members.empty())
        {
            std::vector<struct nexthop_grp> grp(members.size());
            for (size_t i = 0; i < members.size(); i++)
            {
                memset(&grp[i], 0, sizeof(grp[i]));
                grp[i].id = members[i];
            }
            addNhAttr(req, NHA_GROUP, grp.data(), grp.size() * sizeof(struct nexthop_grp));
        }
        if (fdb)
        {
            addNhAttr(req, NHA_FDB, nullptr, 0);
        }
        m_macSync->onKernelNexthop(&req.n);
    }

    /* zebra's remote MAC for the MAC whose last byte is last, on a group or a VTEP. */
    void remote(uint8_t last, uint32_t nhg, const char *vtep = nullptr)
    {
        MacMsg req;
        buildMacMsg(req, RTM_NEWNEIGH, NUD_REACHABLE);
        struct rtattr *lladdr =
            (struct rtattr *)(void *)(((char *)&req.n) + NLMSG_ALIGN(NLMSG_LENGTH(sizeof(struct ndmsg))));
        ((uint8_t *)RTA_DATA(lladdr))[ETH_ALEN - 1] = last;
        req.ndm.ndm_ifindex = VXLAN_IFINDEX;
        addSrcVni(req, TEST_VNI);
        if (vtep)
        {
            addVtep(req, vtep);
        }
        else
        {
            addNhgId(req, nhg);
        }
        feed(req);
    }

    static std::string key(uint8_t last)
    {
        char buf[64];
        snprintf(buf, sizeof(buf), "Vlan100:00:11:22:33:44:%02x", last);
        return buf;
    }

    bool hasRow(uint8_t last)
    {
        std::vector<FieldValueTuple> values;
        return getEntry(key(last), values);
    }

    static std::chrono::steady_clock::time_point due()
    {
        return std::chrono::steady_clock::now() +
               std::chrono::milliseconds(MacSync::NHG_FLUSH_DELAY_MS + 1000);
    }
};

constexpr int MacSyncNhgFlushTest::VXLAN_IFINDEX;
constexpr uint32_t MacSyncNhgFlushTest::ES_NHG;
constexpr uint32_t MacSyncNhgFlushTest::OTHER_NHG;
constexpr uint32_t MacSyncNhgFlushTest::VTEP_NH;

TEST_F(MacSyncNhgFlushTest, GroupDeleteWithdrawsItsRows)
{
    kernelNh(RTM_NEWNEXTHOP, ES_NHG);
    remote(0x55, ES_NHG);
    ASSERT_TRUE(hasRow(0x55));

    kernelNh(RTM_DELNEXTHOP, ES_NHG);
    m_macSync->processNhgFlush(std::chrono::steady_clock::now());
    EXPECT_TRUE(hasRow(0x55));

    m_macSync->processNhgFlush(due());
    EXPECT_FALSE(hasRow(0x55));
    EXPECT_EQ(m_macSync->m_remoteRows.count(key(0x55)), 0u);
    EXPECT_TRUE(m_macSync->m_nhgFlushPending.empty());
}

/* A MAC zebra sent before its group's delete is read after the flush when the
 * FPM channel lags; the kernel has no such group, so it is not written. */
TEST_F(MacSyncNhgFlushTest, MacReadAfterItsGroupWentIsDropped)
{
    kernelNh(RTM_NEWNEXTHOP, ES_NHG);
    kernelNh(RTM_DELNEXTHOP, ES_NHG);
    m_macSync->processNhgFlush(due());

    remote(0x55, ES_NHG);
    EXPECT_FALSE(hasRow(0x55));
}

/* zebra's MACs on a group it deleted arrive back to back; the kernel is asked
 * once for the burst, not once per MAC. */
TEST_F(MacSyncNhgFlushTest, BurstOnAGoneGroupAsksTheKernelOnce)
{
    kernelNh(RTM_NEWNEXTHOP, ES_NHG);
    kernelNh(RTM_DELNEXTHOP, ES_NHG);
    m_macSync->processNhgFlush(due());

    int asked = 0;
    auto kernel = m_macSync->m_kernelNexthopState;
    m_macSync->m_kernelNexthopState = [&asked, kernel](uint32_t id) {
        asked++;
        return kernel(id);
    };

    for (uint8_t last = 0x50; last < 0x60; last++)
    {
        remote(last, ES_NHG);
        EXPECT_FALSE(hasRow(last));
    }
    EXPECT_EQ(asked, 1);
}

/* The answer stands only for the burst: a creation that was not read is found
 * by the next lookup. */
TEST_F(MacSyncNhgFlushTest, AnswerThatAGroupIsGoneExpires)
{
    kernelNh(RTM_NEWNEXTHOP, ES_NHG);
    kernelNh(RTM_DELNEXTHOP, ES_NHG);
    remote(0x55, ES_NHG);
    ASSERT_FALSE(hasRow(0x55));
    ASSERT_EQ(m_macSync->m_deletedNhgs.count(ES_NHG), 1u);

    m_kernelIds[ES_NHG] = true;
    m_macSync->m_deletedNhgs[ES_NHG] = std::chrono::steady_clock::now() - std::chrono::milliseconds(1);
    remote(0x56, ES_NHG);
    EXPECT_TRUE(hasRow(0x56));
    EXPECT_TRUE(m_macSync->m_deletedNhgs.empty());
}

/* A creation read from the kernel ends the answer at once. */
TEST_F(MacSyncNhgFlushTest, CreationEndsTheAnswerThatTheGroupIsGone)
{
    kernelNh(RTM_NEWNEXTHOP, ES_NHG);
    kernelNh(RTM_DELNEXTHOP, ES_NHG);
    remote(0x55, ES_NHG);
    ASSERT_FALSE(hasRow(0x55));

    kernelNh(RTM_NEWNEXTHOP, ES_NHG);
    remote(0x56, ES_NHG);
    EXPECT_TRUE(hasRow(0x56));
}

/* The rows are found by their group, which follows each rewrite and withdrawal. */
TEST_F(MacSyncNhgFlushTest, RowsAreIndexedByTheirGroup)
{
    remote(0x55, ES_NHG);
    remote(0x56, ES_NHG);
    ASSERT_EQ(m_macSync->m_rowsByGroup[ES_NHG].size(), 2u);

    remote(0x55, OTHER_NHG);
    EXPECT_EQ(m_macSync->m_rowsByGroup[ES_NHG], std::set<std::string>{key(0x56)});
    EXPECT_EQ(m_macSync->m_rowsByGroup[OTHER_NHG], std::set<std::string>{key(0x55)});

    remote(0x55, 0, "10.0.0.2");
    remote(0x56, 0, "10.0.0.2");
    EXPECT_TRUE(m_macSync->m_rowsByGroup.empty());
    EXPECT_EQ(m_macSync->m_remoteRows[key(0x55)].group, 0u);

    remote(0x57, ES_NHG);
    kernelNh(RTM_DELNEXTHOP, ES_NHG);
    m_macSync->processNhgFlush(due());
    EXPECT_FALSE(hasRow(0x57));
    EXPECT_TRUE(m_macSync->m_rowsByGroup.empty());
}

/* A creation that was not read: the kernel has the group again. */
TEST_F(MacSyncNhgFlushTest, MacOnAGroupTheKernelHasAgainIsWritten)
{
    kernelNh(RTM_NEWNEXTHOP, ES_NHG);
    kernelNh(RTM_DELNEXTHOP, ES_NHG);
    m_kernelIds[ES_NHG] = true;

    remote(0x55, ES_NHG);
    EXPECT_TRUE(hasRow(0x55));
    EXPECT_TRUE(m_macSync->m_deletedNhgs.empty());
}

/* A group the daemon never saw created still flushes on its own delete. */
TEST_F(MacSyncNhgFlushTest, UnseenGroupDeleteWithdrawsItsRows)
{
    remote(0x55, ES_NHG);
    kernelNh(RTM_DELNEXTHOP, ES_NHG);
    m_macSync->processNhgFlush(due());
    EXPECT_FALSE(hasRow(0x55));
}

/* The kernel notifies the group's own delete when its last member goes. */
TEST_F(MacSyncNhgFlushTest, MemberDeleteAloneKeepsTheRows)
{
    remote(0x55, ES_NHG);
    kernelNh(RTM_DELNEXTHOP, VTEP_NH, true, {});
    EXPECT_TRUE(m_macSync->m_nhgFlushPending.empty());
    m_macSync->processNhgFlush(due());
    EXPECT_TRUE(hasRow(0x55));
}

/* A group created again with the same id: what zebra sends for it stays, and
 * the rows it does not send again went with the deleted group. */
TEST_F(MacSyncNhgFlushTest, RecreatedGroupKeepsOnlyTheRowsSentAgain)
{
    remote(0x55, ES_NHG);
    remote(0x56, ES_NHG);
    kernelNh(RTM_DELNEXTHOP, ES_NHG);
    /* Sent before the creation: on the old group, whose kernel entry is gone. */
    remote(0x58, ES_NHG);
    kernelNh(RTM_NEWNEXTHOP, ES_NHG);
    remote(0x56, ES_NHG);
    remote(0x57, ES_NHG);
    m_macSync->processNhgFlush(due());
    EXPECT_FALSE(hasRow(0x55));
    EXPECT_FALSE(hasRow(0x58));
    EXPECT_TRUE(hasRow(0x56));
    EXPECT_TRUE(hasRow(0x57));
    EXPECT_TRUE(m_macSync->m_nhgFlushPending.empty());
}

/* A creation still queued at the flush is read first. */
TEST_F(MacSyncNhgFlushTest, FlushReadsQueuedKernelMessagesFirst)
{
    int asked = 0;
    m_macSync->m_kernelNexthopState = [&asked](uint32_t) {
        asked++;
        return MacSync::NexthopState::Fdb;
    };
    std::vector<std::pair<uint16_t, uint32_t>> queued;
    m_macSync->setKernelNexthopReader([this, &queued]() {
        auto events = std::move(queued);
        queued.clear();
        for (const auto& event : events)
        {
            kernelNh(event.first, event.second);
        }
    });

    remote(0x55, ES_NHG);
    kernelNh(RTM_DELNEXTHOP, ES_NHG);
    queued = {{RTM_NEWNEXTHOP, ES_NHG}};
    m_macSync->processNhgFlush(due());
    EXPECT_TRUE(queued.empty());
    EXPECT_EQ(asked, 0);
    EXPECT_FALSE(hasRow(0x55));
    m_macSync->setKernelNexthopReader(nullptr);
}

/* With the group gone, a row sent after the delete names nothing either. */
TEST_F(MacSyncNhgFlushTest, LateRowOnAGoneGroupIsWithdrawn)
{
    kernelNh(RTM_DELNEXTHOP, ES_NHG);
    remote(0x55, ES_NHG);
    m_macSync->processNhgFlush(due());
    EXPECT_FALSE(hasRow(0x55));
}

/* zebra sends MACs on a group only after it creates the group in the kernel,
 * so the kernel messages queued when an FPM MAC message is read come first. */
TEST_F(MacSyncNhgFlushTest, QueuedKernelMessagesAreReadBeforeAMac)
{
    std::vector<std::pair<uint16_t, uint32_t>> queued;
    m_macSync->setKernelNexthopReader([this, &queued]() {
        auto events = std::move(queued);
        queued.clear();
        for (const auto& event : events)
        {
            kernelNh(event.first, event.second);
        }
    });

    kernelNh(RTM_NEWNEXTHOP, ES_NHG);
    remote(0x55, ES_NHG);
    queued = {{RTM_DELNEXTHOP, ES_NHG}, {RTM_NEWNEXTHOP, ES_NHG}};
    remote(0x56, ES_NHG);
    EXPECT_TRUE(queued.empty());
    m_macSync->processNhgFlush(due());
    EXPECT_FALSE(hasRow(0x55));
    EXPECT_TRUE(hasRow(0x56));
    m_macSync->setKernelNexthopReader(nullptr);
}

/* A later delete of the new group takes the rows sent for it too. */
TEST_F(MacSyncNhgFlushTest, SecondDeleteWithdrawsTheNewGroupsRows)
{
    remote(0x55, ES_NHG);
    kernelNh(RTM_DELNEXTHOP, ES_NHG);
    kernelNh(RTM_NEWNEXTHOP, ES_NHG);
    remote(0x56, ES_NHG);
    kernelNh(RTM_DELNEXTHOP, ES_NHG);
    m_macSync->processNhgFlush(due());
    EXPECT_FALSE(hasRow(0x55));
    EXPECT_FALSE(hasRow(0x56));
}

/* Routing and FDB nexthops share the ids: a routing nexthop that takes the id
 * neither keeps the rows nor counts as the group created again. */
TEST_F(MacSyncNhgFlushTest, RoutingNexthopOnTheIdDoesNotKeepTheRows)
{
    remote(0x55, ES_NHG);
    kernelNh(RTM_DELNEXTHOP, ES_NHG);
    kernelNh(RTM_NEWNEXTHOP, ES_NHG, false);
    EXPECT_FALSE(m_macSync->m_nhgFlushPending.begin()->second.recreated);
    m_macSync->processNhgFlush(due());
    EXPECT_FALSE(hasRow(0x55));
}

/* The kernel has a group under the id, but its creation was never read: the
 * rows sent before the delete went with the old group, later ones stay. */
TEST_F(MacSyncNhgFlushTest, GroupPresentWithoutItsCreationKeepsTheLaterRows)
{
    remote(0x55, ES_NHG);
    kernelNh(RTM_DELNEXTHOP, ES_NHG);
    m_kernelIds[ES_NHG] = true;
    remote(0x56, ES_NHG);
    m_macSync->processNhgFlush(due());
    EXPECT_FALSE(hasRow(0x55));
    EXPECT_TRUE(hasRow(0x56));
    EXPECT_TRUE(m_macSync->m_nhgFlushPending.empty());
}

/* zebra programs the group before it re-installs the MACs over FPM, so the
 * re-install can be read before the kernel's RTM_NEWNEXTHOP, or that is lost. */
TEST_F(MacSyncNhgFlushTest, UnreadRecreationKeepsTheReinstalledRows)
{
    kernelNh(RTM_NEWNEXTHOP, ES_NHG);
    remote(0x55, ES_NHG);
    kernelNh(RTM_DELNEXTHOP, ES_NHG);
    m_kernelIds[ES_NHG] = true;
    remote(0x55, ES_NHG);
    m_macSync->processNhgFlush(due());
    EXPECT_TRUE(hasRow(0x55));
    EXPECT_TRUE(m_macSync->m_nhgFlushPending.empty());
}

/* An unclear answer is asked again, and the rows are kept once the tries run out. */
TEST_F(MacSyncNhgFlushTest, UnknownKernelAnswerKeepsTheRows)
{
    int asked = 0;
    m_macSync->m_kernelNexthopState = [&asked](uint32_t) {
        asked++;
        return MacSync::NexthopState::Unknown;
    };
    remote(0x55, ES_NHG);
    kernelNh(RTM_DELNEXTHOP, ES_NHG);
    for (int i = 0; i < MacSync::NHG_FLUSH_TRIES; i++)
    {
        ASSERT_EQ(m_macSync->m_nhgFlushPending.size(), 1u);
        m_macSync->processNhgFlush(m_macSync->m_nhgFlushPending.begin()->second.due);
        EXPECT_TRUE(hasRow(0x55));
    }
    EXPECT_EQ(asked, MacSync::NHG_FLUSH_TRIES);
    EXPECT_TRUE(m_macSync->m_nhgFlushPending.empty());
}

TEST_F(MacSyncNhgFlushTest, UnknownKernelAnswerIsAskedAgain)
{
    auto answer = MacSync::NexthopState::Unknown;
    m_macSync->m_kernelNexthopState = [&answer](uint32_t) { return answer; };
    remote(0x55, ES_NHG);
    kernelNh(RTM_DELNEXTHOP, ES_NHG);
    m_macSync->processNhgFlush(due());
    EXPECT_TRUE(hasRow(0x55));
    ASSERT_EQ(m_macSync->m_nhgFlushPending.size(), 1u);

    answer = MacSync::NexthopState::Absent;
    m_macSync->processNhgFlush(m_macSync->m_nhgFlushPending.begin()->second.due);
    EXPECT_FALSE(hasRow(0x55));
}

/* A row zebra moved off the group before its delete is not the group's. */
TEST_F(MacSyncNhgFlushTest, RowMovedOffTheGroupStays)
{
    remote(0x55, ES_NHG);
    remote(0x55, 0, "10.0.0.2");
    kernelNh(RTM_DELNEXTHOP, ES_NHG);
    m_macSync->processNhgFlush(due());

    std::vector<FieldValueTuple> values;
    ASSERT_TRUE(getEntry(key(0x55), values));
    EXPECT_EQ(fieldValue(values, "remote_vtep"), "10.0.0.2");
}

/* A row an earlier fpmsyncd wrote that zebra replays unchanged is not
 * rewritten, and is on the group as much as a new one. */
TEST_F(MacSyncNhgFlushTest, UnchangedReplayRowIsOnTheGroup)
{
    remote(0x55, ES_NHG);
    m_macSync->m_remoteRows.clear();
    m_macSync->m_rowsByGroup.clear();
    ASSERT_TRUE(m_macSync->markRemoteMacsStale(0));
    testing_db::resetOperationCounters();
    remote(0x55, ES_NHG);
    ASSERT_EQ(testing_db::getProducerSetCount(APP_VXLAN_FDB_TABLE_NAME), 0u);

    kernelNh(RTM_DELNEXTHOP, ES_NHG);
    m_macSync->processNhgFlush(due());
    EXPECT_FALSE(hasRow(0x55));
}

TEST_F(MacSyncNhgFlushTest, OnlyTheDeletedGroupsRowsGo)
{
    kernelNh(RTM_NEWNEXTHOP, OTHER_NHG);
    remote(0x55, ES_NHG);
    remote(0x56, OTHER_NHG);
    remote(0x57, 0, "10.0.0.2");
    kernelNh(RTM_DELNEXTHOP, ES_NHG);
    m_macSync->processNhgFlush(due());
    EXPECT_FALSE(hasRow(0x55));
    EXPECT_TRUE(hasRow(0x56));
    EXPECT_TRUE(hasRow(0x57));
}

/* Deletes read close together go in one pass. */
TEST_F(MacSyncNhgFlushTest, DeletesCloseTogetherFlushTogether)
{
    remote(0x55, ES_NHG);
    remote(0x56, OTHER_NHG);
    kernelNh(RTM_DELNEXTHOP, ES_NHG);
    kernelNh(RTM_DELNEXTHOP, OTHER_NHG);
    m_macSync->processNhgFlush(m_macSync->m_nhgFlushPending.begin()->second.due);
    EXPECT_FALSE(hasRow(0x55));
    EXPECT_FALSE(hasRow(0x56));
    EXPECT_TRUE(m_macSync->m_nhgFlushPending.empty());
}

TEST_F(MacSyncNhgFlushTest, RoutingNexthopDeleteIsIgnored)
{
    remote(0x55, ES_NHG);
    kernelNh(RTM_DELNEXTHOP, ES_NHG, false);
    kernelNh(RTM_DELNEXTHOP, ES_NHG, false, {});
    EXPECT_TRUE(m_macSync->m_nhgFlushPending.empty());
    m_macSync->processNhgFlush(due());
    EXPECT_TRUE(hasRow(0x55));
}

/* Rows from before a reconnect carry the old zebra's ids; the stale sweep has them. */
TEST_F(MacSyncNhgFlushTest, StaleRowIsLeftToTheSweep)
{
    remote(0x55, ES_NHG);
    ASSERT_TRUE(m_macSync->markRemoteMacsStale(0));
    kernelNh(RTM_DELNEXTHOP, ES_NHG);
    m_macSync->processNhgFlush(due());
    EXPECT_TRUE(hasRow(0x55));
}

TEST_F(MacSyncNhgFlushTest, HeldRowOnTheGroupIsDropped)
{
    MacSync::HeldRow held{VXLAN_IFINDEX, {{"nexthop_group", std::to_string(ES_NHG)}, {"type", "dynamic"}}};
    m_macSync->m_esReAddDeferred.emplace(key(0x55), held);
    MacSync::HeldRow other{VXLAN_IFINDEX, {{"nexthop_group", std::to_string(OTHER_NHG)}, {"type", "dynamic"}}};
    m_macSync->m_esReAddDeferred.emplace(key(0x56), other);

    kernelNh(RTM_DELNEXTHOP, ES_NHG);
    m_macSync->processNhgFlush(due());
    EXPECT_EQ(m_macSync->m_esReAddDeferred.count(key(0x55)), 0u);
    EXPECT_EQ(m_macSync->m_esReAddDeferred.count(key(0x56)), 1u);
}

TEST_F(MacSyncNhgFlushTest, HeldRowOnALiveGroupIsKept)
{
    kernelNh(RTM_DELNEXTHOP, ES_NHG);
    m_kernelIds[ES_NHG] = true;
    MacSync::HeldRow held{VXLAN_IFINDEX, {{"nexthop_group", std::to_string(ES_NHG)}, {"type", "dynamic"}},
                          ++m_macSync->m_remoteMacSeq};
    m_macSync->m_esReAddDeferred.emplace(key(0x55), held);
    m_macSync->processNhgFlush(due());
    EXPECT_EQ(m_macSync->m_esReAddDeferred.count(key(0x55)), 1u);
}

TEST_F(MacSyncNhgFlushTest, DisconnectCancelsTheFlush)
{
    remote(0x55, ES_NHG);
    kernelNh(RTM_DELNEXTHOP, ES_NHG);
    m_macSync->onFpmDisconnected();
    m_macSync->processNhgFlush(due());
    EXPECT_TRUE(hasRow(0x55));
}

TEST_F(MacSyncNhgFlushTest, KernelModeIgnoresNexthops)
{
    remote(0x55, ES_NHG);
    m_macSync->m_fpmMode = false;
    kernelNh(RTM_DELNEXTHOP, ES_NHG);
    EXPECT_TRUE(m_macSync->m_nhgFlushPending.empty());
}

TEST_F(MacSyncNhgFlushTest, TruncatedMessageIsIgnored)
{
    remote(0x55, ES_NHG);
    NhMsg req;
    memset(&req, 0, sizeof(req));
    req.n.nlmsg_len = NLMSG_LENGTH(sizeof(struct nhmsg)) - 1;
    req.n.nlmsg_type = RTM_DELNEXTHOP;
    m_macSync->onKernelNexthop(&req.n);

    uint32_t id = ES_NHG;
    struct nexthop_grp grp;
    memset(&grp, 0, sizeof(grp));
    grp.id = VTEP_NH;
    req.n.nlmsg_len = NLMSG_LENGTH(sizeof(struct nhmsg));
    addNhAttr(req, NHA_ID, &id, 2);
    addNhAttr(req, NHA_GROUP, &grp, sizeof(grp));
    addNhAttr(req, NHA_FDB, nullptr, 0);
    m_macSync->onKernelNexthop(&req.n);
    EXPECT_TRUE(m_macSync->m_nhgFlushPending.empty());
}

/* The kernel's answers to RTM_GETNEXTHOP. */
TEST_F(MacSyncNhgFlushTest, NexthopReplyIsParsed)
{
    struct
    {
        struct nlmsghdr n;
        struct nlmsgerr err;
    } nack;
    auto error = [&nack](int code, uint32_t seq) {
        memset(&nack, 0, sizeof(nack));
        nack.n.nlmsg_len = NLMSG_LENGTH(sizeof(struct nlmsgerr));
        nack.n.nlmsg_type = NLMSG_ERROR;
        nack.n.nlmsg_seq = seq;
        nack.err.error = code;
    };
    MacSync::NexthopState state = MacSync::NexthopState::Fdb;

    error(-ENOENT, 1);
    ASSERT_TRUE(MacSync::parseNexthopReply((char *)&nack, sizeof(nack), 1, state));
    EXPECT_EQ(state, MacSync::NexthopState::Absent);

    error(-EPERM, 1);
    ASSERT_TRUE(MacSync::parseNexthopReply((char *)&nack, sizeof(nack), 1, state));
    EXPECT_EQ(state, MacSync::NexthopState::Unknown);

    error(0, 1);
    ASSERT_TRUE(MacSync::parseNexthopReply((char *)&nack, sizeof(nack), 1, state));
    EXPECT_EQ(state, MacSync::NexthopState::Unknown);

    error(-ENOENT, 2);
    state = MacSync::NexthopState::Fdb;
    EXPECT_FALSE(MacSync::parseNexthopReply((char *)&nack, sizeof(nack), 1, state));
    EXPECT_EQ(state, MacSync::NexthopState::Fdb);

    /* Another request's answer, then this one's. */
    struct
    {
        decltype(nack) other;
        decltype(nack) mine;
    } both;
    error(-EPERM, 2);
    both.other = nack;
    error(-ENOENT, 1);
    both.mine = nack;
    ASSERT_TRUE(MacSync::parseNexthopReply((char *)&both, sizeof(both), 1, state));
    EXPECT_EQ(state, MacSync::NexthopState::Absent);

    auto reply = [this](bool fdb) {
        NhMsg msg;
        memset(&msg, 0, sizeof(msg));
        msg.n.nlmsg_len = NLMSG_LENGTH(sizeof(struct nhmsg));
        msg.n.nlmsg_type = RTM_NEWNEXTHOP;
        msg.n.nlmsg_seq = 1;
        uint32_t id = ES_NHG;
        addNhAttr(msg, NHA_ID, &id, sizeof(id));
        if (fdb)
        {
            addNhAttr(msg, NHA_FDB, nullptr, 0);
        }
        return msg;
    };
    NhMsg msg = reply(true);
    ASSERT_TRUE(MacSync::parseNexthopReply((char *)&msg, msg.n.nlmsg_len, 1, state));
    EXPECT_EQ(state, MacSync::NexthopState::Fdb);

    msg = reply(false);
    ASSERT_TRUE(MacSync::parseNexthopReply((char *)&msg, msg.n.nlmsg_len, 1, state));
    EXPECT_EQ(state, MacSync::NexthopState::Absent);

    /* An attribute cut short. */
    msg = reply(true);
    struct rtattr *last = (struct rtattr *)(void *)((char *)&msg.n + msg.n.nlmsg_len - RTA_LENGTH(0));
    last->rta_len = RTA_LENGTH(8);
    ASSERT_TRUE(MacSync::parseNexthopReply((char *)&msg, msg.n.nlmsg_len, 1, state));
    EXPECT_EQ(state, MacSync::NexthopState::Unknown);
}

/* A real lookup of an id nothing holds. */
TEST_F(MacSyncNhgFlushTest, KernelLookupOfAnUnusedId)
{
    auto state = m_realNexthopState(0xfffffff0u);
    if (state == MacSync::NexthopState::Unknown)
    {
        GTEST_SKIP() << "no RTM_GETNEXTHOP answer here";
    }
    EXPECT_EQ(state, MacSync::NexthopState::Absent);
}
