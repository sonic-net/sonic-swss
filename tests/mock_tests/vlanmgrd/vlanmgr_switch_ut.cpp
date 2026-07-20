#define protected public
#include "vlanmgr.h"
#undef protected
#include "gtest/gtest.h"
#include "mock_table.h"
#include <gmock/gmock.h>
#include <swss/redisutility.h>

extern int mockCmdReturn;
extern std::string mockCmdStdcout;
extern std::vector<std::string> mockCallArgs;

// VlanMgr references this extern global
swss::MacAddress gMacAddress("00:11:22:33:44:55");

namespace vlanmgr_switch_ut
{
    using namespace swss;
    using namespace std;

    struct VlanMgrSwitchTest : public ::testing::Test
    {
        shared_ptr<swss::DBConnector> m_app_db;
        shared_ptr<swss::DBConnector> m_config_db;
        shared_ptr<swss::DBConnector> m_state_db;
        shared_ptr<VlanMgr> m_vlanMgr;

        VlanMgrSwitchTest()
        {
            m_app_db = make_shared<swss::DBConnector>("APPL_DB", 0);
            m_config_db = make_shared<swss::DBConnector>("CONFIG_DB", 0);
            m_state_db = make_shared<swss::DBConnector>("STATE_DB", 0);
        }

        virtual void SetUp() override
        {
            ::testing_db::reset();
            mockCmdReturn = 0;
            mockCmdStdcout = "";
            mockCallArgs.clear();
            vector<string> cfg_vlan_tables = {
                CFG_VLAN_TABLE_NAME,
                CFG_VLAN_MEMBER_TABLE_NAME,
                CFG_SWITCH_TABLE_NAME,
            };
            vector<string> state_vlan_tables = {
                STATE_PORT_TABLE_NAME,
                STATE_LAG_TABLE_NAME,
            };
            m_vlanMgr.reset(new VlanMgr(m_config_db.get(), m_app_db.get(), m_state_db.get(),
                                        cfg_vlan_tables, state_vlan_tables));
        }

        virtual void TearDown() override
        {
            ::testing_db::reset();
        }

        // Simulates a vlanmgrd process restart
        void restartVlanMgr()
        {
            vector<string> cfg_vlan_tables = {
                CFG_VLAN_TABLE_NAME,
                CFG_VLAN_MEMBER_TABLE_NAME,
                CFG_SWITCH_TABLE_NAME,
            };
            vector<string> state_vlan_tables = {
                STATE_PORT_TABLE_NAME,
                STATE_LAG_TABLE_NAME,
            };
            m_vlanMgr.reset(new VlanMgr(m_config_db.get(), m_app_db.get(), m_state_db.get(),
                                        cfg_vlan_tables, state_vlan_tables));
        }
    };

    // After a vlanmgrd restart, an unchanged replayed VLAN_MEMBER CONFIG_DB entry must be skipped as a duplicate because the check is now backed by STATE_DB instead of the cleared in-memory m_PortVlanMember cache.
    TEST_F(VlanMgrSwitchTest, VlanMemberReplayIsSkippedAfterRestart)
    {
        Table cfg_vlan_member_table(m_config_db.get(), CFG_VLAN_MEMBER_TABLE_NAME);
        Table state_port_table(m_state_db.get(), STATE_PORT_TABLE_NAME);
        Table state_vlan_table(m_state_db.get(), STATE_VLAN_TABLE_NAME);
        Table state_vlan_member_table(m_state_db.get(), STATE_VLAN_MEMBER_TABLE_NAME);

        // Port and VLAN are both up/ready.
        state_port_table.set("Ethernet0", { {"state", "ok"} });
        state_vlan_table.set("Vlan2", { {"state", "ok"} });

        // Initial apply: untagged member, port/lag/vlan ready, shell commands succeed.
        cfg_vlan_member_table.set("Vlan2|Ethernet0", { {"tagging_mode", "untagged"} });
        m_vlanMgr->addExistingData(&cfg_vlan_member_table);
        m_vlanMgr->doTask();

        size_t callsAfterInitialApply = mockCallArgs.size();
        ASSERT_GT(callsAfterInitialApply, 0u);

        vector<FieldValueTuple> values;
        ASSERT_TRUE(state_vlan_member_table.get("Vlan2|Ethernet0", values));
        auto tagging_mode_opt = swss::fvsGetValue(values, "tagging_mode", true);
        ASSERT_TRUE(tagging_mode_opt);
        ASSERT_EQ("untagged", tagging_mode_opt.get());

        // Simulate a vlanmgrd restart
        restartVlanMgr();
        size_t callsAfterRestart = mockCallArgs.size();

        mockCmdReturn = 1;

        // CONFIG_DB replay after restart: same key, same (unchanged) tagging_mode.
        cfg_vlan_member_table.set("Vlan2|Ethernet0", { {"tagging_mode", "untagged"} });
        m_vlanMgr->addExistingData(&cfg_vlan_member_table);
        m_vlanMgr->doTask();

        // No new shell command should have been attempted for the unchanged replay.
        ASSERT_EQ(callsAfterRestart, mockCallArgs.size());
    }

    // Companion to the test above: after a restart, a genuine tagging_mode change
    // (not just a replay of the unchanged value) must still be detected and applied.
    TEST_F(VlanMgrSwitchTest, VlanMemberModeChangeAppliesAfterRestart)
    {
        Table cfg_vlan_member_table(m_config_db.get(), CFG_VLAN_MEMBER_TABLE_NAME);
        Table state_port_table(m_state_db.get(), STATE_PORT_TABLE_NAME);
        Table state_vlan_table(m_state_db.get(), STATE_VLAN_TABLE_NAME);
        Table state_vlan_member_table(m_state_db.get(), STATE_VLAN_MEMBER_TABLE_NAME);

        state_port_table.set("Ethernet0", { {"state", "ok"} });
        state_vlan_table.set("Vlan2", { {"state", "ok"} });

        cfg_vlan_member_table.set("Vlan2|Ethernet0", { {"tagging_mode", "untagged"} });
        m_vlanMgr->addExistingData(&cfg_vlan_member_table);
        m_vlanMgr->doTask();
        ASSERT_GT(mockCallArgs.size(), 0u);

        // Simulate a vlanmgrd restart.
        restartVlanMgr();
        size_t callsAfterRestart = mockCallArgs.size();

        // Shell commands succeed (this is a legitimate, healthy mode change).
        mockCmdReturn = 0;

        // CONFIG_DB replay after restart, but this time with a real mode change.
        cfg_vlan_member_table.set("Vlan2|Ethernet0", { {"tagging_mode", "tagged"} });
        m_vlanMgr->addExistingData(&cfg_vlan_member_table);
        m_vlanMgr->doTask();

        // The change should have been applied via new shell command
        ASSERT_GT(mockCallArgs.size(), callsAfterRestart);

        // STATE_DB should reflect the new tagging_mode.
        vector<FieldValueTuple> values;
        ASSERT_TRUE(state_vlan_member_table.get("Vlan2|Ethernet0", values));
        auto tagging_mode_opt = swss::fvsGetValue(values, "tagging_mode", true);
        ASSERT_TRUE(tagging_mode_opt);
        ASSERT_EQ("tagged", tagging_mode_opt.get());
    }
}

