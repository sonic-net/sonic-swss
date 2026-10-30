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
#include <map>
#include <vector>

#include <linux/if_ether.h>
#include <linux/neighbour.h>
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
 * the kernel keeps them on the port) and other VLANs' rows stay. */
TEST_F(MacSyncLinkDelTest, RowsOfTheVlanGoWhoeverWroteThem)
{
    putRow(key(100, 0x01), {{"remote_vtep", "10.1.1.1"}, {"type", "dynamic"}, {"vni", "20100"}});
    putRow(key(100, 0x02), {{"nexthop_group", "7"}, {"type", "dynamic"}, {"vni", "20100"}});
    putRow(key(100, 0x03), {{"ifname", "PortChannel1"}, {"type", "dynamic"}, {"vni", "0"}});
    putRow(key(1000, 0x01), {{"remote_vtep", "10.1.1.1"}, {"type", "dynamic"}, {"vni", "21000"}});

    delLink(VTEP_IFINDEX, "vtep1-100", "vxlan");

    EXPECT_FALSE(hasEntry(key(100, 0x01)));
    EXPECT_FALSE(hasEntry(key(100, 0x02)));
    EXPECT_TRUE(hasEntry(key(100, 0x03)));
    EXPECT_TRUE(hasEntry(key(1000, 0x01)));
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
