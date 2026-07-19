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

    // Test: Set FDB aging time
    TEST_F(VlanMgrSwitchTest, SetFdbAgingTime)
    {
        Table cfg_switch_table(m_config_db.get(), CFG_SWITCH_TABLE_NAME);
        Table app_switch_table(m_app_db.get(), APP_SWITCH_TABLE_NAME);

        // Set fdb_aging_time to 300 seconds
        cfg_switch_table.set("switch", {
            {"fdb_aging_time", "300"}
        });

        m_vlanMgr->addExistingData(&cfg_switch_table);
        m_vlanMgr->doTask();

        // Verify that the value was forwarded to APPL_DB
        vector<FieldValueTuple> values;
        app_switch_table.get("switch", values);
        auto value_opt = swss::fvsGetValue(values, "fdb_aging_time", true);
        ASSERT_TRUE(value_opt);
        ASSERT_EQ("300", value_opt.get());
    }

    // Test: Update FDB aging time
    TEST_F(VlanMgrSwitchTest, UpdateFdbAgingTime)
    {
        Table cfg_switch_table(m_config_db.get(), CFG_SWITCH_TABLE_NAME);
        Table app_switch_table(m_app_db.get(), APP_SWITCH_TABLE_NAME);

        // Set initial value
        cfg_switch_table.set("switch", {
            {"fdb_aging_time", "300"}
        });
        m_vlanMgr->addExistingData(&cfg_switch_table);
        m_vlanMgr->doTask();

        // Update to new value
        cfg_switch_table.set("switch", {
            {"fdb_aging_time", DEFAULT_FDB_AGING_TIME_STR}
        });
        m_vlanMgr->addExistingData(&cfg_switch_table);
        m_vlanMgr->doTask();

        // Verify the updated value
        vector<FieldValueTuple> values;
        app_switch_table.get("switch", values);
        auto value_opt = swss::fvsGetValue(values, "fdb_aging_time", true);
        ASSERT_TRUE(value_opt);
        ASSERT_EQ(DEFAULT_FDB_AGING_TIME_STR, value_opt.get());
    }

    // Test: Delete FDB aging time (restore default)
    TEST_F(VlanMgrSwitchTest, DeleteFdbAgingTime)
    {
        Table cfg_switch_table(m_config_db.get(), CFG_SWITCH_TABLE_NAME);
        Table app_switch_table(m_app_db.get(), APP_SWITCH_TABLE_NAME);

        // Set a custom value first
        cfg_switch_table.set("switch", {
            {"fdb_aging_time", "300"}
        });
        m_vlanMgr->addExistingData(&cfg_switch_table);
        m_vlanMgr->doTask();

        // Delete directly to test VlanMgr::doSwitchTask() DEL_COMMAND; addExistingData() only replays SET_COMMANDs.

        cfg_switch_table.del("switch");
        auto consumer = dynamic_cast<Consumer *>(m_vlanMgr->getExecutor(CFG_SWITCH_TABLE_NAME));
        std::deque<KeyOpFieldsValuesTuple> entries;
        entries.push_back({"switch", DEL_COMMAND, {}});
        consumer->addToSync(entries);
        m_vlanMgr->doTask();

        // Verify that default value is set
        vector<FieldValueTuple> values;
        app_switch_table.get("switch", values);
        auto value_opt = swss::fvsGetValue(values, "fdb_aging_time", true);
        ASSERT_TRUE(value_opt);
        ASSERT_EQ(DEFAULT_FDB_AGING_TIME_STR, value_opt.get());
    }

    // Test: Set FDB aging time with minimum value
    TEST_F(VlanMgrSwitchTest, SetMinimumFdbAgingTime)
    {
        Table cfg_switch_table(m_config_db.get(), CFG_SWITCH_TABLE_NAME);
        Table app_switch_table(m_app_db.get(), APP_SWITCH_TABLE_NAME);

        // Set minimum value (0 seconds)
        cfg_switch_table.set("switch", {
            {"fdb_aging_time", "0"}
        });
        m_vlanMgr->addExistingData(&cfg_switch_table);
        m_vlanMgr->doTask();

        // Verify the value
        vector<FieldValueTuple> values;
        app_switch_table.get("switch", values);
        auto value_opt = swss::fvsGetValue(values, "fdb_aging_time", true);
        ASSERT_TRUE(value_opt);
        ASSERT_EQ("0", value_opt.get());
    }

    // Test: Set FDB aging time with maximum value
    TEST_F(VlanMgrSwitchTest, SetMaximumFdbAgingTime)
    {
        Table cfg_switch_table(m_config_db.get(), CFG_SWITCH_TABLE_NAME);
        Table app_switch_table(m_app_db.get(), APP_SWITCH_TABLE_NAME);

        // Set maximum value (1000000 seconds)
        cfg_switch_table.set("switch", {
            {"fdb_aging_time", "1000000"}
        });
        m_vlanMgr->addExistingData(&cfg_switch_table);
        m_vlanMgr->doTask();

        // Verify the value
        vector<FieldValueTuple> values;
        app_switch_table.get("switch", values);
        auto value_opt = swss::fvsGetValue(values, "fdb_aging_time", true);
        ASSERT_TRUE(value_opt);
        ASSERT_EQ("1000000", value_opt.get());
    }

    // Test: Preserve boot-time fields when setting FDB aging time
    TEST_F(VlanMgrSwitchTest, PreserveBootTimeFields)
    {
        Table cfg_switch_table(m_config_db.get(), CFG_SWITCH_TABLE_NAME);
        Table app_switch_table(m_app_db.get(), APP_SWITCH_TABLE_NAME);

        // Simulate boot-time fields in APPL_DB
        app_switch_table.set("switch", {
            {"ecmp_hash_seed", "10"},
            {"lag_hash_seed", "20"},
            {"ecmp_hash_offset", "0"},
            {"lag_hash_offset", "0"},
            {"ordered_ecmp", "false"}
        });

        // Set fdb_aging_time via CONFIG_DB
        cfg_switch_table.set("switch", {
            {"fdb_aging_time", "300"}
        });
        m_vlanMgr->addExistingData(&cfg_switch_table);
        m_vlanMgr->doTask();

        // Verify that fdb_aging_time is set and boot-time fields are preserved
        vector<FieldValueTuple> values;
        app_switch_table.get("switch", values);

        auto aging_time = swss::fvsGetValue(values, "fdb_aging_time", true);
        ASSERT_TRUE(aging_time);
        ASSERT_EQ("300", aging_time.get());

        // Boot-time fields should still exist
        auto ecmp_hash = swss::fvsGetValue(values, "ecmp_hash_seed", true);
        ASSERT_TRUE(ecmp_hash);
        ASSERT_EQ("10", ecmp_hash.get());

        auto lag_hash = swss::fvsGetValue(values, "lag_hash_seed", true);
        ASSERT_TRUE(lag_hash);
        ASSERT_EQ("20", lag_hash.get());
    }

    // Test: Multiple SET operations
    TEST_F(VlanMgrSwitchTest, MultipleSetOperations)
    {
        Table cfg_switch_table(m_config_db.get(), CFG_SWITCH_TABLE_NAME);
        Table app_switch_table(m_app_db.get(), APP_SWITCH_TABLE_NAME);

        // First SET
        cfg_switch_table.set("switch", {
            {"fdb_aging_time", "100"}
        });
        m_vlanMgr->addExistingData(&cfg_switch_table);
        m_vlanMgr->doTask();

        // Second SET
        cfg_switch_table.set("switch", {
            {"fdb_aging_time", "200"}
        });
        m_vlanMgr->addExistingData(&cfg_switch_table);
        m_vlanMgr->doTask();

        // Third SET
        cfg_switch_table.set("switch", {
            {"fdb_aging_time", "300"}
        });
        m_vlanMgr->addExistingData(&cfg_switch_table);
        m_vlanMgr->doTask();

        // Verify final value
        vector<FieldValueTuple> values;
        app_switch_table.get("switch", values);
        auto value_opt = swss::fvsGetValue(values, "fdb_aging_time", true);
        ASSERT_TRUE(value_opt);
        ASSERT_EQ("300", value_opt.get());
    }

    // Test: SET followed by DELETE
    TEST_F(VlanMgrSwitchTest, SetThenDelete)
    {
        Table cfg_switch_table(m_config_db.get(), CFG_SWITCH_TABLE_NAME);
        Table app_switch_table(m_app_db.get(), APP_SWITCH_TABLE_NAME);

        // SET
        cfg_switch_table.set("switch", {
            {"fdb_aging_time", "500"}
        });
        m_vlanMgr->addExistingData(&cfg_switch_table);
        m_vlanMgr->doTask();

        // Verify SET
        vector<FieldValueTuple> values;
        app_switch_table.get("switch", values);
        auto value_opt = swss::fvsGetValue(values, "fdb_aging_time", true);
        ASSERT_TRUE(value_opt);
        ASSERT_EQ("500", value_opt.get());

        // Delete directly to test VlanMgr::doSwitchTask() DEL_COMMAND; addExistingData() only replays SET_COMMANDs.
        cfg_switch_table.del("switch");
        auto consumer = dynamic_cast<Consumer *>(m_vlanMgr->getExecutor(CFG_SWITCH_TABLE_NAME));
        std::deque<KeyOpFieldsValuesTuple> entries;
        entries.push_back({"switch", DEL_COMMAND, {}});
        consumer->addToSync(entries);
        m_vlanMgr->doTask();

        // Verify default value restored
        values.clear();
        app_switch_table.get("switch", values);
        value_opt = swss::fvsGetValue(values, "fdb_aging_time", true);
        ASSERT_TRUE(value_opt);
        ASSERT_EQ(DEFAULT_FDB_AGING_TIME_STR, value_opt.get());
    }

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

