#include "gtest/gtest.h"
#include "../mock_table.h"

#define private public
#include "macsecmgr.h"
#undef private

#include <memory>
#include <string>
#include <vector>
#include <cstdlib>
#include <fstream>
#include <unistd.h>

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

    // macsecmgrd finds a leftover wpa_supplicant by its global control socket
    // and asks it to exit over that socket. Stand in for the supplicant: record
    // the request and, when "alive", unlink the socket the way a real exit does.
    class MACsecMgrWithFakeSupplicant : public MACsecMgr
    {
    public:
        using MACsecMgr::MACsecMgr;
        bool alive = false;
        std::vector<std::string> terminated;

        bool terminateWPASupplicant(const std::string &sock) const override
        {
            const_cast<MACsecMgrWithFakeSupplicant *>(this)->terminated.push_back(sock);
            if (!alive)
            {
                return false;
            }
            unlink(sock.c_str());
            return true;
        }
    };

    class MACsecMgrStaleSupplicantTest : public MACsecMgrTest
    {
    protected:
        std::unique_ptr<MACsecMgrWithFakeSupplicant> m_fake;
        std::string m_dir;

        void SetUp() override
        {
            MACsecMgrTest::SetUp();
            char tmpl[] = "/tmp/macsecmgr_ut_XXXXXX";
            m_dir = std::string(mkdtemp(tmpl)) + "/";
            std::vector<std::string> tables = { CFG_MACSEC_PROFILE_TABLE_NAME, CFG_PORT_TABLE_NAME };
            m_fake.reset(new MACsecMgrWithFakeSupplicant(m_config_db.get(), m_app_db.get(), m_state_db.get(), tables));
            m_fake->m_clearStaleTimeoutMs = 300;
            m_fake->m_stopStaleTimeoutMs = 300;
            m_fake->m_sockDir = m_dir;
        }

        void TearDown() override
        {
            m_fake.reset();
            unlink((m_dir + "Ethernet1").c_str());
            rmdir(m_dir.c_str());
            MACsecMgrTest::TearDown();
        }

        void touch_socket(const std::string &port)
        {
            std::ofstream(m_dir + port).put('\n');
        }

        bool socket_exists(const std::string &port)
        {
            return access((m_dir + port).c_str(), F_OK) == 0;
        }
    };

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

    TEST_F(MACsecMgrTest, disableUnknownPort_clearsLeftoversOfThatPortOnly)
    {
        // Config for Ethernet1 was removed while a previous macsecmgrd's
        // supplicant had its entries in APPL_DB; this macsecmgrd never started
        // a session there, so the PORT SET without a profile is all it sees.
        seed(APP_MACSEC_PORT_TABLE_NAME, "Ethernet1");
        seed(APP_MACSEC_EGRESS_SC_TABLE_NAME, "Ethernet1:" + kSci);
        seed(APP_MACSEC_EGRESS_SA_TABLE_NAME, "Ethernet1:" + kSci + ":0");
        seed_staged(APP_MACSEC_INGRESS_SA_TABLE_NAME, "Ethernet1:" + kSci + ":0");
        seed(APP_MACSEC_PORT_TABLE_NAME, "Ethernet10");
        seed(APP_MACSEC_EGRESS_SA_TABLE_NAME, "Ethernet10:" + kSci + ":0");

        EXPECT_EQ(m_mgr->disableMACsec("Ethernet1", MACsecMgr::TaskArgs()), task_success);

        EXPECT_FALSE(exists(APP_MACSEC_PORT_TABLE_NAME, "Ethernet1"));
        EXPECT_FALSE(exists(APP_MACSEC_EGRESS_SC_TABLE_NAME, "Ethernet1:" + kSci));
        EXPECT_FALSE(exists(APP_MACSEC_EGRESS_SA_TABLE_NAME, "Ethernet1:" + kSci + ":0"));
        EXPECT_FALSE(staged(APP_MACSEC_INGRESS_SA_TABLE_NAME, "Ethernet1:" + kSci + ":0"));
        EXPECT_TRUE(exists(APP_MACSEC_PORT_TABLE_NAME, "Ethernet10"));
        EXPECT_TRUE(exists(APP_MACSEC_EGRESS_SA_TABLE_NAME, "Ethernet10:" + kSci + ":0"));
    }

    TEST_F(MACsecMgrStaleSupplicantTest, stopStale_noSocketNothingAsked)
    {
        m_fake->stopStaleWPASupplicant("Ethernet1");
        EXPECT_TRUE(m_fake->terminated.empty());
    }

    TEST_F(MACsecMgrStaleSupplicantTest, stopStale_liveSupplicantIsTerminatedAndWaitedFor)
    {
        touch_socket("Ethernet1");
        m_fake->alive = true;

        m_fake->stopStaleWPASupplicant("Ethernet1");

        ASSERT_EQ(m_fake->terminated.size(), 1u);
        EXPECT_EQ(m_fake->terminated[0], m_dir + "Ethernet1");
        EXPECT_FALSE(socket_exists("Ethernet1"));
    }

    TEST_F(MACsecMgrStaleSupplicantTest, stopStale_leftoverSocketWithoutListenerIsLeftAlone)
    {
        touch_socket("Ethernet1");
        m_fake->alive = false;

        m_fake->stopStaleWPASupplicant("Ethernet1");

        ASSERT_EQ(m_fake->terminated.size(), 1u);
        EXPECT_TRUE(socket_exists("Ethernet1"));
    }

    TEST_F(MACsecMgrStaleSupplicantTest, disableUnknownPort_terminatesSupplicantBeforeClearing)
    {
        touch_socket("Ethernet1");
        m_fake->alive = true;
        seed(APP_MACSEC_PORT_TABLE_NAME, "Ethernet1");
        seed(APP_MACSEC_INGRESS_SA_TABLE_NAME, "Ethernet1:" + kSci + ":0");

        EXPECT_EQ(m_fake->disableMACsec("Ethernet1", MACsecMgr::TaskArgs()), task_success);

        ASSERT_EQ(m_fake->terminated.size(), 1u);
        EXPECT_FALSE(socket_exists("Ethernet1"));
        EXPECT_FALSE(exists(APP_MACSEC_PORT_TABLE_NAME, "Ethernet1"));
        EXPECT_FALSE(exists(APP_MACSEC_INGRESS_SA_TABLE_NAME, "Ethernet1:" + kSci + ":0"));
    }
}
