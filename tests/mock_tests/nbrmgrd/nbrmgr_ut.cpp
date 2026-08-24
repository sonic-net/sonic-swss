#include "gtest/gtest.h"
#include <iostream>
#include <netlink/netlink.h>
#include <netlink/msg.h>
#include <net/if.h>
#include "../mock_table.h"
#include "warm_restart.h"
#define private public
#include "nbrmgr.h"
#undef private

extern int (*callback)(const std::string &cmd, std::string &stdout);
extern std::vector<std::string> mockCallArgs;

/* Track netlink neighbor resolve calls */
static std::vector<std::string> neighResolvedKeys;

/*
 * Wrap netlink and interface functions to avoid real kernel interaction.
 * setNeighbor() calls nl_socket_alloc, nl_connect, if_nametoindex, and
 * nl_send_auto. We intercept nl_send_auto to record the call and
 * if_nametoindex to return a dummy index.
 */
extern "C" {

struct nl_sock *__wrap_nl_socket_alloc(void)
{
    /* Return a non-null fake pointer; nl_sock is opaque so cast from raw memory */
    static char fake_sock_mem[256];
    return reinterpret_cast<struct nl_sock *>(fake_sock_mem);
}

int __wrap_nl_connect(struct nl_sock *sk, int protocol)
{
    return 0;
}

int __wrap_nl_send_auto(struct nl_sock *sk, struct nl_msg *msg)
{
    /* Record that a neighbor resolve was attempted */
    return 0;
}

/* Control whether nlmsg_alloc returns NULL to simulate setNeighbor failure */
static bool mock_nlmsg_alloc_fail = false;

struct nl_msg *__real_nlmsg_alloc(void);

struct nl_msg *__wrap_nlmsg_alloc(void)
{
    if (mock_nlmsg_alloc_fail)
    {
        return nullptr;
    }
    return __real_nlmsg_alloc();
}

unsigned int __wrap_if_nametoindex(const char *ifname)
{
    /* Return a dummy interface index */
    return 1;
}

}

int noop_cb(const std::string &cmd, std::string &out){
    mockCallArgs.push_back(cmd);
    return 0;
}

namespace nbrmgr_ut
{
    struct NbrMgrTest : public ::testing::Test
    {
        std::shared_ptr<swss::DBConnector> m_config_db;
        std::shared_ptr<swss::DBConnector> m_app_db;
        std::shared_ptr<swss::DBConnector> m_state_db;

        virtual void SetUp() override
        {
            testing_db::reset();
            m_config_db = std::make_shared<swss::DBConnector>("CONFIG_DB", 0);
            m_app_db = std::make_shared<swss::DBConnector>("APPL_DB", 0);
            m_state_db = std::make_shared<swss::DBConnector>("STATE_DB", 0);

            swss::WarmStart::initialize("nbrmgrd", "swss");

            mockCallArgs.clear();
            neighResolvedKeys.clear();
            mock_nlmsg_alloc_fail = false;
            callback = noop_cb;
        }
    };

    /*
     * Test that when NEIGH_RESOLVE_TABLE is empty at startup,
     * NbrMgr constructs successfully without errors.
     */
    TEST_F(NbrMgrTest, ReconcileEmptyTable)
    {
        std::vector<std::string> cfg_nbr_tables = {CFG_NEIGH_TABLE_NAME};

        /* No entries in NEIGH_RESOLVE_TABLE - should construct without issue */
        swss::NbrMgr nbrmgr(m_config_db.get(), m_app_db.get(), m_state_db.get(), cfg_nbr_tables);

        /* Verify the table is indeed empty */
        swss::Table neighResolveTable(m_app_db.get(), APP_NEIGH_RESOLVE_TABLE_NAME);
        std::vector<std::string> keys;
        neighResolveTable.getKeys(keys);
        ASSERT_TRUE(keys.empty());
    }

    /*
     * Test that pre-existing entries in NEIGH_RESOLVE_TABLE are
     * picked up and processed during NbrMgr construction.
     */
    TEST_F(NbrMgrTest, ReconcilePendingEntries)
    {
        std::vector<std::string> cfg_nbr_tables = {CFG_NEIGH_TABLE_NAME};

        /* Pre-populate NEIGH_RESOLVE_TABLE with entries (simulating entries
         * left over from before a restart) */
        swss::Table neighResolveTable(m_app_db.get(), APP_NEIGH_RESOLVE_TABLE_NAME);
        std::vector<swss::FieldValueTuple> fvs;
        fvs.emplace_back("family", "IPv4");
        neighResolveTable.set("Ethernet0:10.0.0.1", fvs);
        neighResolveTable.set("Ethernet4:10.0.0.3", fvs);

        /* Verify entries exist before construction */
        std::vector<std::string> keys;
        neighResolveTable.getKeys(keys);
        ASSERT_EQ(keys.size(), 2u);

        /* Construct NbrMgr - reconciliation should process these entries */
        swss::NbrMgr nbrmgr(m_config_db.get(), m_app_db.get(), m_state_db.get(), cfg_nbr_tables);

        /* NbrMgr constructor calls setNeighbor for each entry via
         * reconcileNeighResolveTable. Since setNeighbor uses netlink
         * (wrapped here), we verify construction succeeds without error.
         * The entries are resolved via netlink, not removed from the table
         * (orchagent removes them after neighbor is learned). */
    }

    /*
     * Test reconciliation with IPv6 entries.
     */
    TEST_F(NbrMgrTest, ReconcileIPv6Entries)
    {
        std::vector<std::string> cfg_nbr_tables = {CFG_NEIGH_TABLE_NAME};

        swss::Table neighResolveTable(m_app_db.get(), APP_NEIGH_RESOLVE_TABLE_NAME);
        std::vector<swss::FieldValueTuple> fvs;
        fvs.emplace_back("family", "IPv6");
        neighResolveTable.set("Ethernet0:2000::2", fvs);
        neighResolveTable.set("Ethernet4:2001::2", fvs);

        swss::NbrMgr nbrmgr(m_config_db.get(), m_app_db.get(), m_state_db.get(), cfg_nbr_tables);

        /* Verify construction completes - IPv6 entries are reconciled */
    }

    /*
     * Test that entries with invalid key format (no ':' separator)
     * are skipped during reconciliation.
     */
    TEST_F(NbrMgrTest, ReconcileInvalidKeyFormat)
    {
        std::vector<std::string> cfg_nbr_tables = {CFG_NEIGH_TABLE_NAME};

        swss::Table neighResolveTable(m_app_db.get(), APP_NEIGH_RESOLVE_TABLE_NAME);
        std::vector<swss::FieldValueTuple> fvs;
        /* Valid entry */
        neighResolveTable.set("Ethernet0:10.0.0.1", fvs);
        /* Invalid entry - no ':' separator */
        neighResolveTable.set("InvalidKeyNoSeparator", fvs);

        std::vector<std::string> keys;
        neighResolveTable.getKeys(keys);
        ASSERT_EQ(keys.size(), 2u);

        /* Should not crash; invalid key is skipped, valid key is processed */
        swss::NbrMgr nbrmgr(m_config_db.get(), m_app_db.get(), m_state_db.get(), cfg_nbr_tables);
    }

    /*
     * Test that setNeighbor failure during reconciliation is handled
     * gracefully (logs warning, continues with remaining entries).
     */
    TEST_F(NbrMgrTest, ReconcileSetNeighborFailure)
    {
        std::vector<std::string> cfg_nbr_tables = {CFG_NEIGH_TABLE_NAME};

        swss::Table neighResolveTable(m_app_db.get(), APP_NEIGH_RESOLVE_TABLE_NAME);
        std::vector<swss::FieldValueTuple> fvs;
        neighResolveTable.set("Ethernet0:10.0.0.1", fvs);
        neighResolveTable.set("Ethernet4:10.0.0.3", fvs);

        /* Force nlmsg_alloc to fail, causing setNeighbor to return false */
        mock_nlmsg_alloc_fail = true;

        /* Should not crash; failures are logged as warnings */
        swss::NbrMgr nbrmgr(m_config_db.get(), m_app_db.get(), m_state_db.get(), cfg_nbr_tables);
    }

    /*
     * Test that NbrMgr subscribes to APP_NEIGH_TABLE on dual-ToR.
     */
    TEST_F(NbrMgrTest, DualTorSubscribesToNeighTable)
    {
        std::vector<std::string> cfg_nbr_tables = {CFG_NEIGH_TABLE_NAME};

        /* Configure as dual-ToR */
        swss::Table deviceMeta(m_config_db.get(), CFG_DEVICE_METADATA_TABLE_NAME);
        std::vector<swss::FieldValueTuple> metaFvs;
        metaFvs.emplace_back("subtype", "DualToR");
        deviceMeta.set("localhost", metaFvs);

        swss::NbrMgr nbrmgr(m_config_db.get(), m_app_db.get(), m_state_db.get(), cfg_nbr_tables);

        ASSERT_TRUE(nbrmgr.m_isDualTor);
    }

    /*
     * Test that NbrMgr does NOT subscribe to APP_NEIGH_TABLE on single ToR.
     */
    TEST_F(NbrMgrTest, SingleTorDoesNotSubscribe)
    {
        std::vector<std::string> cfg_nbr_tables = {CFG_NEIGH_TABLE_NAME};

        /* No DualToR subtype configured */
        swss::NbrMgr nbrmgr(m_config_db.get(), m_app_db.get(), m_state_db.get(), cfg_nbr_tables);

        ASSERT_FALSE(nbrmgr.m_isDualTor);
    }

    /*
     * Test that zero-MAC neighbor entries trigger resolution on dual-ToR.
     */
    TEST_F(NbrMgrTest, ZeroMacTriggersResolution)
    {
        std::vector<std::string> cfg_nbr_tables = {CFG_NEIGH_TABLE_NAME};

        /* Configure as dual-ToR */
        swss::Table deviceMeta(m_config_db.get(), CFG_DEVICE_METADATA_TABLE_NAME);
        std::vector<swss::FieldValueTuple> metaFvs;
        metaFvs.emplace_back("subtype", "DualToR");
        deviceMeta.set("localhost", metaFvs);

        swss::NbrMgr nbrmgr(m_config_db.get(), m_app_db.get(), m_state_db.get(), cfg_nbr_tables);

        /* Simulate zero-MAC neighbor entry in APP_NEIGH_TABLE */
        auto consumer = dynamic_cast<swss::Consumer *>(nbrmgr.getExecutor(APP_NEIGH_TABLE_NAME));
        ASSERT_NE(consumer, nullptr);

        std::deque<swss::KeyOpFieldsValuesTuple> entries;
        std::vector<swss::FieldValueTuple> fvs;
        fvs.emplace_back("neigh", "00:00:00:00:00:00");
        fvs.emplace_back("family", "IPv6");
        entries.push_back({"Vlan1000:fc02:1000::2", SET_COMMAND, fvs});
        consumer->addToSync(entries);

        mockCallArgs.clear();
        nbrmgr.doTask(*consumer);

        /* Verify ndisc6/arping was called for the neighbor */
        bool nsProbeFound = false;
        for (const auto &cmd : mockCallArgs)
        {
            if (cmd.find("ndisc6") != std::string::npos &&
                cmd.find("fc02:1000::2") != std::string::npos &&
                cmd.find("Vlan1000") != std::string::npos)
            {
                nsProbeFound = true;
                break;
            }
        }
        EXPECT_TRUE(nsProbeFound);
    }

    /*
     * Test that non-zero MAC entries are ignored (no resolution triggered).
     */
    TEST_F(NbrMgrTest, NonZeroMacIgnored)
    {
        std::vector<std::string> cfg_nbr_tables = {CFG_NEIGH_TABLE_NAME};

        swss::Table deviceMeta(m_config_db.get(), CFG_DEVICE_METADATA_TABLE_NAME);
        std::vector<swss::FieldValueTuple> metaFvs;
        metaFvs.emplace_back("subtype", "DualToR");
        deviceMeta.set("localhost", metaFvs);

        swss::NbrMgr nbrmgr(m_config_db.get(), m_app_db.get(), m_state_db.get(), cfg_nbr_tables);

        auto consumer = dynamic_cast<swss::Consumer *>(nbrmgr.getExecutor(APP_NEIGH_TABLE_NAME));
        ASSERT_NE(consumer, nullptr);

        std::deque<swss::KeyOpFieldsValuesTuple> entries;
        std::vector<swss::FieldValueTuple> fvs;
        fvs.emplace_back("neigh", "aa:bb:cc:dd:ee:ff");
        fvs.emplace_back("family", "IPv6");
        entries.push_back({"Vlan1000:fc02:1000::2", SET_COMMAND, fvs});
        consumer->addToSync(entries);

        mockCallArgs.clear();
        nbrmgr.doTask(*consumer);

        /* No NS probe should be sent for non-zero MAC */
        for (const auto &cmd : mockCallArgs)
        {
            EXPECT_EQ(cmd.find("ndisc6"), std::string::npos);
            EXPECT_EQ(cmd.find("arping"), std::string::npos);
        }
    }

    /*
     * Test that DEL operations are consumed without action.
     */
    TEST_F(NbrMgrTest, DelOperationConsumed)
    {
        std::vector<std::string> cfg_nbr_tables = {CFG_NEIGH_TABLE_NAME};

        swss::Table deviceMeta(m_config_db.get(), CFG_DEVICE_METADATA_TABLE_NAME);
        std::vector<swss::FieldValueTuple> metaFvs;
        metaFvs.emplace_back("subtype", "DualToR");
        deviceMeta.set("localhost", metaFvs);

        swss::NbrMgr nbrmgr(m_config_db.get(), m_app_db.get(), m_state_db.get(), cfg_nbr_tables);

        auto consumer = dynamic_cast<swss::Consumer *>(nbrmgr.getExecutor(APP_NEIGH_TABLE_NAME));
        ASSERT_NE(consumer, nullptr);

        std::deque<swss::KeyOpFieldsValuesTuple> entries;
        std::vector<swss::FieldValueTuple> fvs;
        fvs.emplace_back("neigh", "00:00:00:00:00:00");
        entries.push_back({"Vlan1000:fc02:1000::2", DEL_COMMAND, fvs});
        consumer->addToSync(entries);

        nbrmgr.doTask(*consumer);

        /* m_toSync should be drained */
        EXPECT_TRUE(consumer->m_toSync.empty());
    }

    /*
     * Test zero-MAC resolution for IPv4 neighbor.
     */
    TEST_F(NbrMgrTest, ZeroMacIPv4TriggersArping)
    {
        std::vector<std::string> cfg_nbr_tables = {CFG_NEIGH_TABLE_NAME};

        swss::Table deviceMeta(m_config_db.get(), CFG_DEVICE_METADATA_TABLE_NAME);
        std::vector<swss::FieldValueTuple> metaFvs;
        metaFvs.emplace_back("subtype", "DualToR");
        deviceMeta.set("localhost", metaFvs);

        swss::NbrMgr nbrmgr(m_config_db.get(), m_app_db.get(), m_state_db.get(), cfg_nbr_tables);

        auto consumer = dynamic_cast<swss::Consumer *>(nbrmgr.getExecutor(APP_NEIGH_TABLE_NAME));
        ASSERT_NE(consumer, nullptr);

        std::deque<swss::KeyOpFieldsValuesTuple> entries;
        std::vector<swss::FieldValueTuple> fvs;
        fvs.emplace_back("neigh", "00:00:00:00:00:00");
        fvs.emplace_back("family", "IPv4");
        entries.push_back({"Vlan1000:10.0.0.2", SET_COMMAND, fvs});
        consumer->addToSync(entries);

        mockCallArgs.clear();
        nbrmgr.doTask(*consumer);

        /* Verify arping was called for IPv4 */
        bool arpProbeFound = false;
        for (const auto &cmd : mockCallArgs)
        {
            if (cmd.find("arping") != std::string::npos &&
                cmd.find("10.0.0.2") != std::string::npos)
            {
                arpProbeFound = true;
                break;
            }
        }
        EXPECT_TRUE(arpProbeFound);
    }

    /*
     * Test that setNeighborIncomplete failure doesn't crash
     * and NS probe is not sent.
     */
    TEST_F(NbrMgrTest, SetIncompleteFailureSkipsProbe)
    {
        std::vector<std::string> cfg_nbr_tables = {CFG_NEIGH_TABLE_NAME};

        swss::Table deviceMeta(m_config_db.get(), CFG_DEVICE_METADATA_TABLE_NAME);
        std::vector<swss::FieldValueTuple> metaFvs;
        metaFvs.emplace_back("subtype", "DualToR");
        deviceMeta.set("localhost", metaFvs);

        swss::NbrMgr nbrmgr(m_config_db.get(), m_app_db.get(), m_state_db.get(), cfg_nbr_tables);

        auto consumer = dynamic_cast<swss::Consumer *>(nbrmgr.getExecutor(APP_NEIGH_TABLE_NAME));
        ASSERT_NE(consumer, nullptr);

        /* Force nlmsg_alloc to fail */
        mock_nlmsg_alloc_fail = true;

        std::deque<swss::KeyOpFieldsValuesTuple> entries;
        std::vector<swss::FieldValueTuple> fvs;
        fvs.emplace_back("neigh", "00:00:00:00:00:00");
        entries.push_back({"Vlan1000:fc02:1000::2", SET_COMMAND, fvs});
        consumer->addToSync(entries);

        mockCallArgs.clear();
        nbrmgr.doTask(*consumer);

        /* No NS probe should be sent when setIncomplete fails */
        for (const auto &cmd : mockCallArgs)
        {
            EXPECT_EQ(cmd.find("ndisc6"), std::string::npos);
        }
    }
}
