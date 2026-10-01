#include "gtest/gtest.h"
#include <algorithm>
#include <string>
#include <vector>
#include "../mock_table.h"
#include "warm_restart.h"
#include "macaddress.h"
#include "vlanmgr.h"

/* From common/mock_shell_command.cpp */
extern int mockCmdReturn;
extern std::string mockCmdStdcout;
extern std::vector<std::string> mockCallArgs;
extern int (*callback)(const std::string &cmd, std::string &stdout);

extern swss::MacAddress gMacAddress;

namespace vlanmgr_ut
{
    using namespace swss;

    class TestableVlanMgr : public VlanMgr
    {
    public:
        using VlanMgr::VlanMgr;
        using Orch::getExecutor;
    };

    struct VlanMgrTest : public ::testing::Test
    {
        std::shared_ptr<DBConnector> m_config_db;
        std::shared_ptr<DBConnector> m_app_db;
        std::shared_ptr<DBConnector> m_state_db;
        std::shared_ptr<TestableVlanMgr> m_vlanMgr;

        virtual void SetUp() override
        {
            testing_db::reset();
            m_config_db = std::make_shared<DBConnector>("CONFIG_DB", 0);
            m_app_db = std::make_shared<DBConnector>("APPL_DB", 0);
            m_state_db = std::make_shared<DBConnector>("STATE_DB", 0);

            WarmStart::initialize("vlanmgrd", "swss");

            /* doVlanTask() bails out until the switch MAC is known */
            gMacAddress = MacAddress("02:01:02:03:04:05");

            callback = nullptr;
            mockCmdReturn = 0;
            mockCmdStdcout = "";
            mockCallArgs.clear();

            std::vector<std::string> cfg_vlan_tables = {
                CFG_VLAN_TABLE_NAME,
                CFG_VLAN_MEMBER_TABLE_NAME,
            };
            std::vector<std::string> state_vlan_tables = {
                STATE_OPER_PORT_TABLE_NAME,
                STATE_OPER_FDB_TABLE_NAME,
                STATE_OPER_VLAN_MEMBER_TABLE_NAME
            };
            m_vlanMgr = std::make_shared<TestableVlanMgr>(m_config_db.get(),
                                                          m_app_db.get(),
                                                          m_state_db.get(),
                                                          cfg_vlan_tables,
                                                          state_vlan_tables);
            /* Drop the bridge-creation commands from the constructor */
            mockCallArgs.clear();
        }

        void feedTask(const std::string &tableName,
                      const std::string &key,
                      const std::string &op,
                      const std::vector<FieldValueTuple> &fvs)
        {
            auto consumer = dynamic_cast<Consumer *>(m_vlanMgr->getExecutor(tableName));
            ASSERT_NE(consumer, nullptr);
            consumer->addToSync(KeyOpFieldsValuesTuple{key, op, fvs});
            static_cast<Orch *>(m_vlanMgr.get())->doTask(*consumer);
        }

        void setPacPort(const std::string &alias, const std::string &learnMode)
        {
            feedTask(STATE_OPER_PORT_TABLE_NAME, alias, SET_COMMAND,
                     {{"learn_mode", learnMode}});
        }

        void delPacPort(const std::string &alias)
        {
            feedTask(STATE_OPER_PORT_TABLE_NAME, alias, DEL_COMMAND, {});
        }

        void createVlan(const std::string &vlanKey)
        {
            feedTask(CFG_VLAN_TABLE_NAME, vlanKey, SET_COMMAND,
                     {{"admin_status", "up"}, {"mtu", "9100"}});
        }

        void markPortReady(const std::string &alias)
        {
            Table statePortTable(m_state_db.get(), STATE_PORT_TABLE_NAME);
            statePortTable.set(alias, {{"state", "ok"}});
        }

        void addVlanMember(const std::string &vlanKey, const std::string &alias)
        {
            feedTask(CFG_VLAN_MEMBER_TABLE_NAME, vlanKey + "|" + alias,
                     SET_COMMAND, {{"tagging_mode", "untagged"}});
        }

        /* True when one of the recorded swss::exec calls contains all parts */
        bool cmdIssued(const std::vector<std::string> &parts)
        {
            for (const auto &cmd : mockCallArgs)
            {
                bool all = true;
                for (const auto &part : parts)
                {
                    if (cmd.find(part) == std::string::npos)
                    {
                        all = false;
                        break;
                    }
                }
                if (all)
                {
                    return true;
                }
            }
            return false;
        }

        std::string getAppDbField(const std::string &tableName,
                                  const std::string &key,
                                  const std::string &field)
        {
            Table table(m_app_db.get(), tableName);
            std::vector<FieldValueTuple> fvs;
            if (!table.get(key, fvs))
            {
                return "";
            }
            for (const auto &fv : fvs)
            {
                if (fvField(fv) == field)
                {
                    return fvValue(fv);
                }
            }
            return "";
        }
    };

    /*
     * An unauthenticated PAC port (learn_mode cpu_trap or drop) must be
     * locked on the kernel bridge, its dynamic FDB entries flushed and
     * flooding towards it disabled, in addition to the learn_mode being
     * forwarded to APPL_DB.
     */
    TEST_F(VlanMgrTest, PacUnauthenticatedPortIsLocked)
    {
        setPacPort("Ethernet0", "cpu_trap");

        EXPECT_EQ(getAppDbField(APP_PORT_TABLE_NAME, "Ethernet0", "learn_mode"),
                  "cpu_trap");
        EXPECT_TRUE(cmdIssued({"bridge", "link set dev \"Ethernet0\" locked on"}));
        EXPECT_TRUE(cmdIssued({"bridge", "fdb flush dev Bridge brport \"Ethernet0\" dynamic"}));
        EXPECT_TRUE(cmdIssued({"link set dev \"Ethernet0\"",
                               "flood off", "mcast_flood off", "bcast_flood off"}));
    }

    /*
     * A learn mode that keeps the port forwarding (PAC authorized the
     * port itself) must not lock the bridge port.
     */
    TEST_F(VlanMgrTest, PacHardwareLearnModeIsNotLocked)
    {
        setPacPort("Ethernet0", "hardware");

        EXPECT_EQ(getAppDbField(APP_PORT_TABLE_NAME, "Ethernet0", "learn_mode"),
                  "hardware");
        EXPECT_FALSE(cmdIssued({"locked on"}));
        EXPECT_FALSE(cmdIssued({"flood off"}));
    }

    /*
     * Removing the PAC port configuration must release the guard:
     * locked off and flooding restored.
     */
    TEST_F(VlanMgrTest, PacPortDeleteUnlocks)
    {
        markPortReady("Ethernet0");
        setPacPort("Ethernet0", "cpu_trap");
        mockCallArgs.clear();

        delPacPort("Ethernet0");

        EXPECT_EQ(getAppDbField(APP_PORT_TABLE_NAME, "Ethernet0", "learn_mode"),
                  "hardware");
        EXPECT_TRUE(cmdIssued({"link set dev \"Ethernet0\" locked off"}));
        EXPECT_TRUE(cmdIssued({"link set dev \"Ethernet0\"",
                               "flood on", "mcast_flood on", "bcast_flood on"}));
    }

    /*
     * A port that joins a VLAN (and with it the bridge) after PAC left it
     * unauthenticated starts unlocked in the kernel, so the guard has to
     * be re-applied when the bridge membership is created.
     */
    TEST_F(VlanMgrTest, LockReappliedWhenPortJoinsBridge)
    {
        setPacPort("Ethernet0", "drop");
        createVlan("Vlan10");
        markPortReady("Ethernet0");
        mockCallArgs.clear();

        addVlanMember("Vlan10", "Ethernet0");

        EXPECT_TRUE(cmdIssued({"link set dev \"Ethernet0\" locked on"}));
        EXPECT_TRUE(cmdIssued({"flood off", "mcast_flood off", "bcast_flood off"}));
    }

    /*
     * An authorized client (OPER_FDB entry with discard=false) on a locked
     * port must be mirrored into the kernel FDB as a static entry, so the
     * locked bridge port admits its CPU-bound frames; removing the entry
     * removes the mirror.
     */
    TEST_F(VlanMgrTest, PacAuthorizedClientMirroredWhileLocked)
    {
        createVlan("Vlan10");
        setPacPort("Ethernet0", "cpu_trap");
        mockCallArgs.clear();

        feedTask(STATE_OPER_FDB_TABLE_NAME, "Vlan10|00:11:22:33:44:55",
                 SET_COMMAND,
                 {{"port", "Ethernet0"}, {"discard", "false"}, {"type", "static"}});

        EXPECT_TRUE(cmdIssued({"fdb replace \"00:11:22:33:44:55\" dev \"Ethernet0\" "
                               "vlan 10 master static"}));

        mockCallArgs.clear();
        feedTask(STATE_OPER_FDB_TABLE_NAME, "Vlan10|00:11:22:33:44:55",
                 DEL_COMMAND, {});

        EXPECT_TRUE(cmdIssued({"fdb del \"00:11:22:33:44:55\" dev \"Ethernet0\" "
                               "vlan 10 master static"}));
    }

    /*
     * A blocked client (discard=true) must not be mirrored into the
     * kernel FDB: the lock has to keep dropping its frames.
     */
    TEST_F(VlanMgrTest, PacBlockedClientNotMirrored)
    {
        createVlan("Vlan10");
        setPacPort("Ethernet0", "cpu_trap");
        mockCallArgs.clear();

        feedTask(STATE_OPER_FDB_TABLE_NAME, "Vlan10|00:11:22:33:44:55",
                 SET_COMMAND,
                 {{"port", "Ethernet0"}, {"discard", "true"}, {"type", "static"}});

        EXPECT_FALSE(cmdIssued({"fdb replace"}));
    }

    /*
     * A client authorized on an unlocked port must not be mirrored at
     * once, but the mirror has to appear as soon as the port is locked
     * and disappear when it is unlocked again.
     */
    TEST_F(VlanMgrTest, PacMirrorFollowsLockState)
    {
        createVlan("Vlan10");

        feedTask(STATE_OPER_FDB_TABLE_NAME, "Vlan10|00:11:22:33:44:55",
                 SET_COMMAND,
                 {{"port", "Ethernet0"}, {"discard", "false"}, {"type", "static"}});
        EXPECT_FALSE(cmdIssued({"fdb replace"}));

        mockCallArgs.clear();
        setPacPort("Ethernet0", "cpu_trap");
        EXPECT_TRUE(cmdIssued({"fdb replace \"00:11:22:33:44:55\""}));

        markPortReady("Ethernet0");
        mockCallArgs.clear();
        delPacPort("Ethernet0");
        EXPECT_TRUE(cmdIssued({"fdb del \"00:11:22:33:44:55\""}));
    }
}
