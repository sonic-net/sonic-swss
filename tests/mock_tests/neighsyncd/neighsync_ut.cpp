#include "gtest/gtest.h"

#include "../mock_table.h"
#include "schema.h"
#include "warm_restart.h"

#define private public
#include "neighsync.h"
#undef private

namespace neighsync_ut
{
    using namespace swss;

    struct NeighSyncTest : public ::testing::Test
    {
        std::shared_ptr<DBConnector> m_app_db;
        std::shared_ptr<DBConnector> m_state_db;
        std::shared_ptr<DBConnector> m_config_db;
        std::shared_ptr<RedisPipeline> m_app_db_pipeline;
        std::shared_ptr<NeighSync> m_neighSync;

        void SetUp() override
        {
            testing_db::reset();
            WarmStart::initialize("neighsyncd", "swss");

            m_app_db = std::make_shared<DBConnector>("APPL_DB", 0);
            m_state_db = std::make_shared<DBConnector>("STATE_DB", 0);
            m_config_db = std::make_shared<DBConnector>("CONFIG_DB", 0);
            m_app_db_pipeline = std::make_shared<RedisPipeline>(m_app_db.get());

            m_neighSync = std::make_shared<NeighSync>(
                m_app_db_pipeline.get(), m_state_db.get(),
                m_config_db.get(), m_app_db.get());
        }

        void TearDown() override
        {
            m_neighSync.reset();
            testing_db::reset();
        }

        /*
         * Helper: write {ipv6_use_link_local_only: enable} to the requested
         * CONFIG_DB table under the given key.
         */
        void enableLinkLocal(const std::string &table, const std::string &key)
        {
            Table t(m_config_db.get(), table);
            std::vector<FieldValueTuple> fv;
            fv.emplace_back("ipv6_use_link_local_only", "enable");
            t.set(key, fv);
        }
    };

    /*
     * Bare interface coverage: Ethernet, PortChannel, Vlan all continue to
     * resolve to their respective CONFIG_DB tables.
     */
    TEST_F(NeighSyncTest, BareEthernetEnabled)
    {
        enableLinkLocal(CFG_INTF_TABLE_NAME, "Ethernet0");
        EXPECT_TRUE(m_neighSync->isLinkLocalEnabled("Ethernet0"));
    }

    TEST_F(NeighSyncTest, BarePortChannelEnabled)
    {
        enableLinkLocal(CFG_LAG_INTF_TABLE_NAME, "PortChannel10");
        EXPECT_TRUE(m_neighSync->isLinkLocalEnabled("PortChannel10"));
    }

    TEST_F(NeighSyncTest, BareVlanEnabled)
    {
        enableLinkLocal(CFG_VLAN_INTF_TABLE_NAME, "Vlan100");
        EXPECT_TRUE(m_neighSync->isLinkLocalEnabled("Vlan100"));
    }

    /*
     * Long-name VLAN sub-interfaces (Ethernet0.10, PortChannel1.20) must look
     * up VLAN_SUB_INTERFACE and not the bare-interface tables. This is the
     * long-name VLAN sub-interface case.
     */
    TEST_F(NeighSyncTest, LongNameEthernetSubIntfEnabled)
    {
        enableLinkLocal(CFG_VLAN_SUB_INTF_TABLE_NAME, "Ethernet0.10");
        EXPECT_TRUE(m_neighSync->isLinkLocalEnabled("Ethernet0.10"));
    }

    TEST_F(NeighSyncTest, LongNamePortChannelSubIntfEnabled)
    {
        enableLinkLocal(CFG_VLAN_SUB_INTF_TABLE_NAME, "PortChannel1.20");
        EXPECT_TRUE(m_neighSync->isLinkLocalEnabled("PortChannel1.20"));
    }

    /*
     * Short-name VLAN sub-interfaces (Eth0.10, Po1.20) must also resolve to
     * VLAN_SUB_INTERFACE. Without this, neighsyncd silently drops IPv6
     * link-local neighbors learnt from BGP unnumbered on short-name
     * sub-interfaces.
     */
    TEST_F(NeighSyncTest, ShortNameEthernetSubIntfEnabled)
    {
        enableLinkLocal(CFG_VLAN_SUB_INTF_TABLE_NAME, "Eth0.10");
        EXPECT_TRUE(m_neighSync->isLinkLocalEnabled("Eth0.10"));
    }

    TEST_F(NeighSyncTest, ShortNamePortChannelSubIntfEnabled)
    {
        enableLinkLocal(CFG_VLAN_SUB_INTF_TABLE_NAME, "Po1.20");
        EXPECT_TRUE(m_neighSync->isLinkLocalEnabled("Po1.20"));
    }

    /*
     * A sub-interface present in CONFIG_DB but without ipv6_use_link_local_only
     * set to "enable" must return false. This guards against regressions where
     * the mere presence of a VLAN_SUB_INTERFACE entry might unconditionally
     * enable link-local processing.
     */
    TEST_F(NeighSyncTest, SubIntfWithoutLinkLocalDisabled)
    {
        Table t(m_config_db.get(), CFG_VLAN_SUB_INTF_TABLE_NAME);
        std::vector<FieldValueTuple> fv;
        fv.emplace_back("admin_status", "up");
        fv.emplace_back("vlan", "10");
        t.set("Eth0.10", fv);

        EXPECT_FALSE(m_neighSync->isLinkLocalEnabled("Eth0.10"));
    }

    TEST_F(NeighSyncTest, SubIntfWithDisableValueDisabled)
    {
        Table t(m_config_db.get(), CFG_VLAN_SUB_INTF_TABLE_NAME);
        std::vector<FieldValueTuple> fv;
        fv.emplace_back("ipv6_use_link_local_only", "disable");
        t.set("Eth0.10", fv);

        EXPECT_FALSE(m_neighSync->isLinkLocalEnabled("Eth0.10"));
    }

    /*
     * A sub-interface not present in CONFIG_DB at all must return false (the
     * lookup misses and we drop the link-local neighbor).
     */
    TEST_F(NeighSyncTest, SubIntfNotConfiguredDisabled)
    {
        EXPECT_FALSE(m_neighSync->isLinkLocalEnabled("Eth0.10"));
        EXPECT_FALSE(m_neighSync->isLinkLocalEnabled("Ethernet0.10"));
        EXPECT_FALSE(m_neighSync->isLinkLocalEnabled("Po1.20"));
        EXPECT_FALSE(m_neighSync->isLinkLocalEnabled("PortChannel1.20"));
    }

    /*
     * Make sure a sub-interface lookup does NOT accidentally fall through to
     * the bare-interface tables. If we configure only the bare-port entry
     * (e.g. INTERFACE|Ethernet0), the sub-interface must still report
     * link-local as disabled.
     */
    TEST_F(NeighSyncTest, SubIntfDoesNotFallThroughToBareInterface)
    {
        enableLinkLocal(CFG_INTF_TABLE_NAME, "Ethernet0");
        EXPECT_FALSE(m_neighSync->isLinkLocalEnabled("Ethernet0.10"));
        EXPECT_FALSE(m_neighSync->isLinkLocalEnabled("Eth0.10"));

        enableLinkLocal(CFG_LAG_INTF_TABLE_NAME, "PortChannel1");
        EXPECT_FALSE(m_neighSync->isLinkLocalEnabled("PortChannel1.20"));
        EXPECT_FALSE(m_neighSync->isLinkLocalEnabled("Po1.20"));
    }

    /*
     * Unsupported interface names must continue to return false.
     */
    TEST_F(NeighSyncTest, UnsupportedInterfaceDisabled)
    {
        EXPECT_FALSE(m_neighSync->isLinkLocalEnabled("lo"));
        EXPECT_FALSE(m_neighSync->isLinkLocalEnabled("eth0"));
        EXPECT_FALSE(m_neighSync->isLinkLocalEnabled("Bridge"));
        EXPECT_FALSE(m_neighSync->isLinkLocalEnabled("Loopback0"));
    }

    /*
     * Dotted names that look like sub-interfaces but are NOT SONiC sub-intfs
     * (lowercase eth, loopback, Vlan, etc.) must not be treated as
     * sub-interfaces. Using the swss::subIntf parser (the same one
     * intfmgr / portsorch use) makes the accepted set explicit and
     * keeps it in sync with the rest of swss.
     *
     * Pre-populate VLAN_SUB_INTERFACE with these keys to make sure that
     * even if such a key were configured, the parser still rejects the
     * NAME (so we never even look it up), and that even when we DO fall
     * through to the bare-port tables, the lookup misses.
     */
    TEST_F(NeighSyncTest, NonSonicDottedNamesRejected)
    {
        enableLinkLocal(CFG_VLAN_SUB_INTF_TABLE_NAME, "eth0.10");      // lowercase
        enableLinkLocal(CFG_VLAN_SUB_INTF_TABLE_NAME, "lo.10");        // loopback
        enableLinkLocal(CFG_VLAN_SUB_INTF_TABLE_NAME, "Vlan100.10");   // Vlan sub-intf (unsupported)

        EXPECT_FALSE(m_neighSync->isLinkLocalEnabled("eth0.10"));
        EXPECT_FALSE(m_neighSync->isLinkLocalEnabled("lo.10"));
        EXPECT_FALSE(m_neighSync->isLinkLocalEnabled("Vlan100.10"));
    }
}
