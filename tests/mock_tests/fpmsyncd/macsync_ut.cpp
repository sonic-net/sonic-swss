/**
 * @file macsync_ut.cpp
 * @brief Unit tests for MAC synchronization over the FPM channel.
 */

#include "table.h"
#include "schema.h"
#include "mock_table.h"

#include <gtest/gtest.h>

#include <cstring>
#include <vector>

#include <linux/neighbour.h>

#define private public
#include "fpmsyncd/macsync.h"
#undef private

using namespace swss;

#ifndef NDA_PROTOCOL
#define NDA_PROTOCOL 12
#endif

/* Present only on newer kernel headers. */
#ifndef NTF_STICKY
#define NTF_STICKY 0x40
#endif

namespace {

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
    }

    void TearDown() override
    {
        m_macSync.reset();
        m_pipeline.reset();
    }

protected:
    std::shared_ptr<DBConnector> m_appDb;
    std::shared_ptr<DBConnector> m_stateDb;
    std::shared_ptr<DBConnector> m_cfgDb;
    std::shared_ptr<RedisPipeline> m_pipeline;
    std::unique_ptr<MacSync> m_macSync;
};

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
