#include "gtest/gtest.h"
#include "../mock_table.h"

#define private public
#include "macsecmgr.h"
#undef private

#include <memory>
#include <string>
#include <vector>

namespace macsecmgr_ut
{
    using namespace swss;

    // The APPL_DB MACsec tables are owned by wpa_supplicant's macsec_sonic
    // driver. macsecmgrd only touches them to clear what a supplicant that
    // died without running its deinit left behind, so these tests seed the
    // tables the way such a supplicant leaves them and check what survives.
    class MACsecMgrTest : public ::testing::Test
    {
    protected:
        std::shared_ptr<DBConnector> m_config_db;
        std::shared_ptr<DBConnector> m_app_db;
        std::shared_ptr<DBConnector> m_state_db;
        std::unique_ptr<MACsecMgr>   m_mgr;

        void SetUp() override
        {
            testing_db::reset();
            m_config_db = std::make_shared<DBConnector>("CONFIG_DB", 0);
            m_app_db    = std::make_shared<DBConnector>("APPL_DB", 0);
            m_state_db  = std::make_shared<DBConnector>("STATE_DB", 0);
            std::vector<std::string> tables = { CFG_MACSEC_PROFILE_TABLE_NAME, CFG_PORT_TABLE_NAME };
            m_mgr.reset(new MACsecMgr(m_config_db.get(), m_app_db.get(), m_state_db.get(), tables));
            // The mock producer removes the key synchronously, so the wait
            // never has to spin; keep the bound small anyway.
            m_mgr->m_clearStaleTimeoutMs = 300;
        }

        void TearDown() override
        {
            m_mgr.reset();
            testing_db::reset();
        }

        void seed(const std::string &table, const std::string &key)
        {
            Table t(m_app_db.get(), table);
            t.set(key, { { "left_by", "dead_supplicant" } });
        }

        bool exists(const std::string &table, const std::string &key)
        {
            Table t(m_app_db.get(), table);
            std::vector<FieldValueTuple> fvs;
            return t.get(key, fvs);
        }

        // A SET the dead supplicant wrote through its ProducerStateTable that
        // orchagent has not popped: it sits in the staging hash, not the table.
        void seed_staged(const std::string &table, const std::string &key)
        {
            Table t(m_app_db.get(), "_" + table);
            t.set(key, { { "left_by", "dead_supplicant_unpopped" } });
        }

        bool staged(const std::string &table, const std::string &key)
        {
            return exists("_" + table, key);
        }
    };

    static const std::string kSci = "922bd3147c1d0001";

    TEST_F(MACsecMgrTest, clearStale_nothingToClear)
    {
        EXPECT_TRUE(m_mgr->clearStaleMACsecState("Ethernet1"));
    }

    TEST_F(MACsecMgrTest, clearStale_removesEveryEntryOfThePortAndNothingElse)
    {
        seed(APP_MACSEC_PORT_TABLE_NAME,       "Ethernet1");
        seed(APP_MACSEC_EGRESS_SC_TABLE_NAME,  "Ethernet1:" + kSci);
        seed(APP_MACSEC_INGRESS_SC_TABLE_NAME, "Ethernet1:" + kSci);
        seed(APP_MACSEC_EGRESS_SA_TABLE_NAME,  "Ethernet1:" + kSci + ":0");
        seed(APP_MACSEC_INGRESS_SA_TABLE_NAME, "Ethernet1:" + kSci + ":1");
        // Ethernet10 starts with "Ethernet1" but is a different port.
        seed(APP_MACSEC_PORT_TABLE_NAME,       "Ethernet10");
        seed(APP_MACSEC_EGRESS_SA_TABLE_NAME,  "Ethernet10:" + kSci + ":0");

        EXPECT_TRUE(m_mgr->clearStaleMACsecState("Ethernet1"));

        EXPECT_FALSE(exists(APP_MACSEC_PORT_TABLE_NAME,       "Ethernet1"));
        EXPECT_FALSE(exists(APP_MACSEC_EGRESS_SC_TABLE_NAME,  "Ethernet1:" + kSci));
        EXPECT_FALSE(exists(APP_MACSEC_INGRESS_SC_TABLE_NAME, "Ethernet1:" + kSci));
        EXPECT_FALSE(exists(APP_MACSEC_EGRESS_SA_TABLE_NAME,  "Ethernet1:" + kSci + ":0"));
        EXPECT_FALSE(exists(APP_MACSEC_INGRESS_SA_TABLE_NAME, "Ethernet1:" + kSci + ":1"));

        EXPECT_TRUE(exists(APP_MACSEC_PORT_TABLE_NAME,      "Ethernet10"));
        EXPECT_TRUE(exists(APP_MACSEC_EGRESS_SA_TABLE_NAME, "Ethernet10:" + kSci + ":0"));
    }

    TEST_F(MACsecMgrTest, clearStale_scAndSaLeftoversWithoutPortEntry)
    {
        // orchagent already dropped the port (for example after its own
        // restart) but the old session's SC and SA rows are still in APPL_DB.
        // They must go so a later orchagent restart cannot replay old keys,
        // and with no PORT entry there is nothing to wait for.
        seed(APP_MACSEC_EGRESS_SC_TABLE_NAME, "Ethernet1:" + kSci);
        seed(APP_MACSEC_EGRESS_SA_TABLE_NAME, "Ethernet1:" + kSci + ":0");

        EXPECT_TRUE(m_mgr->clearStaleMACsecState("Ethernet1"));

        EXPECT_FALSE(exists(APP_MACSEC_EGRESS_SC_TABLE_NAME, "Ethernet1:" + kSci));
        EXPECT_FALSE(exists(APP_MACSEC_EGRESS_SA_TABLE_NAME, "Ethernet1:" + kSci + ":0"));
    }

    TEST_F(MACsecMgrTest, clearStale_removesSetsOrchagentHasNotPoppedYet)
    {
        // The supplicant died between writing an SA SET and orchagent popping
        // it: nothing is visible in the SA table yet, but the staged SET would be
        // applied after the new session started if it were left alone.
        seed(APP_MACSEC_PORT_TABLE_NAME, "Ethernet1");
        seed_staged(APP_MACSEC_EGRESS_SA_TABLE_NAME, "Ethernet1:" + kSci + ":1");
        seed_staged(APP_MACSEC_EGRESS_SC_TABLE_NAME, "Ethernet1:" + kSci);
        seed_staged(APP_MACSEC_EGRESS_SA_TABLE_NAME, "Ethernet10:" + kSci + ":1");

        EXPECT_TRUE(m_mgr->clearStaleMACsecState("Ethernet1"));

        EXPECT_FALSE(staged(APP_MACSEC_EGRESS_SA_TABLE_NAME, "Ethernet1:" + kSci + ":1"));
        EXPECT_FALSE(staged(APP_MACSEC_EGRESS_SC_TABLE_NAME, "Ethernet1:" + kSci));
        EXPECT_FALSE(exists(APP_MACSEC_PORT_TABLE_NAME, "Ethernet1"));
        EXPECT_TRUE(staged(APP_MACSEC_EGRESS_SA_TABLE_NAME, "Ethernet10:" + kSci + ":1"));
    }

    TEST_F(MACsecMgrTest, clearStale_stagedPortEntryCountsAsPortEntry)
    {
        // Only a staged PORT SET exists: it must still be deleted through the
        // producer and waited for, like a visible one.
        seed_staged(APP_MACSEC_PORT_TABLE_NAME, "Ethernet1");

        EXPECT_TRUE(m_mgr->clearStaleMACsecState("Ethernet1"));

        EXPECT_FALSE(staged(APP_MACSEC_PORT_TABLE_NAME, "Ethernet1"));
        EXPECT_FALSE(exists(APP_MACSEC_PORT_TABLE_NAME, "Ethernet1"));
    }
}
