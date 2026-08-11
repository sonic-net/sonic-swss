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

    /* Writes where fdborch would, so the replay path is exercised as it runs in
     * fpmsyncd rather than by priming the in-memory map. */
    void seedStateFdb(const std::string& key, const std::string& port, bool isStatic)
    {
        Table table(m_stateDb.get(), STATE_FDB_TABLE_NAME);
        std::vector<FieldValueTuple> values = {
            {"port", port},
            {"type", isStatic ? "static" : "dynamic"},
        };
        table.set(key, values);
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

/* Captures what MacSync sends so the replay sequence can be inspected. */
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
 * The replay must come from STATE_DB, not from whatever the subscriber happens
 * to have delivered: zebra can connect before the select loop has run, and a
 * replay of nothing tells zebra to withdraw every local MAC.
 */
TEST_F(MacSyncTest, ReplayLoadsLocalMacsFromStateDb)
{
    /* Deliberately not touching m_localMacs - only STATE_DB. "lo" is used
     * because sendLocalMac resolves the port via if_nametoindex. */
    seedStateFdb("Vlan100:00:11:22:33:44:55", "lo", false);

    RecordingFpm fpm;
    m_macSync->onFpmConnected(fpm);

    ASSERT_EQ(fpm.count(), 2u);
    EXPECT_EQ(fpm.at(0)->nlmsg_type, RTM_NEWNEIGH);
    EXPECT_EQ(fpm.at(1)->nlmsg_type, RTM_FPM_MAC_REPLAY_END);

    /* Both must carry the same generation or the marker sweeps what it replayed. */
    EXPECT_EQ(fpm.at(0)->nlmsg_seq, fpm.at(1)->nlmsg_seq);
    EXPECT_NE(fpm.at(0)->nlmsg_seq, 0u);
}

/*
 * A generation that repeats across an fpmsyncd restart leaves previously
 * stamped MACs looking current, so the sweep never removes anything.
 */
TEST_F(MacSyncTest, GenerationAdvancesOnEveryConnect)
{
    seedStateFdb("Vlan100:00:11:22:33:44:55", "lo", false);

    RecordingFpm first;
    m_macSync->onFpmConnected(first);
    ASSERT_EQ(first.count(), 2u);
    uint32_t g1 = first.at(1)->nlmsg_seq;

    m_macSync->onFpmDisconnected();

    RecordingFpm second;
    m_macSync->onFpmConnected(second);
    ASSERT_EQ(second.count(), 2u);
    uint32_t g2 = second.at(1)->nlmsg_seq;

    EXPECT_GT(g2, g1);
    EXPECT_EQ(second.at(0)->nlmsg_seq, g2);
}

/*
 * A restarted fpmsyncd must not reissue a generation zebra has already seen,
 * or the MACs stamped by the previous process look current and are never swept.
 * A second instance stands in for the restarted process: its members start from
 * their initializers, so only what reached STATE_DB carries over.
 */
TEST_F(MacSyncTest, GenerationDoesNotRepeatAcrossRestart)
{
    seedStateFdb("Vlan100:00:11:22:33:44:55", "lo", false);

    RecordingFpm first;
    m_macSync->onFpmConnected(first);
    ASSERT_EQ(first.count(), 2u);
    uint32_t before = first.at(1)->nlmsg_seq;

    MacSync restarted(m_pipeline.get(), m_stateDb.get(), m_cfgDb.get());
    restarted.m_fpmMode = true;

    RecordingFpm second;
    restarted.onFpmConnected(second);
    ASSERT_EQ(second.count(), 2u);

    EXPECT_NE(second.at(1)->nlmsg_seq, before);
    EXPECT_EQ(second.at(0)->nlmsg_seq, second.at(1)->nlmsg_seq);
}

/* The marker must still close a replay that carried no MACs. */
TEST_F(MacSyncTest, ReplayWithNoLocalMacsStillSendsMarker)
{
    RecordingFpm fpm;
    m_macSync->onFpmConnected(fpm);

    ASSERT_EQ(fpm.count(), 1u);
    EXPECT_EQ(fpm.at(0)->nlmsg_type, RTM_FPM_MAC_REPLAY_END);
    EXPECT_NE(fpm.at(0)->nlmsg_seq, 0u);
}
