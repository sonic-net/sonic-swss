#include "gtest/gtest.h"
#include "tests/mock_tests/mock_table.h"
#include "redisutility.h"
#include "cfgmgr/sflowmgr.h"

namespace sflowmgr_ut
{
    using namespace swss;
    using namespace std;

    struct SflowMgrTest : public ::testing::Test
    {
        shared_ptr<swss::DBConnector> m_app_db;
        shared_ptr<swss::DBConnector> m_config_db;
        shared_ptr<swss::DBConnector> m_state_db;
        shared_ptr<SflowMgr> m_sflowMgr;
        SflowMgrTest()
        {
            m_app_db = make_shared<swss::DBConnector>(
                "APPL_DB", 0);
            m_config_db = make_shared<swss::DBConnector>(
                "CONFIG_DB", 0);
            m_state_db = make_shared<swss::DBConnector>(
                "STATE_DB", 0);
        }

        virtual void SetUp() override
        {
            ::testing_db::reset();
            TableConnector conf_port_table(m_config_db.get(), CFG_PORT_TABLE_NAME);
            TableConnector state_port_table(m_state_db.get(), STATE_PORT_TABLE_NAME);
            TableConnector conf_sflow_table(m_config_db.get(), CFG_SFLOW_TABLE_NAME);
            TableConnector conf_sflow_session_table(m_config_db.get(), CFG_SFLOW_SESSION_TABLE_NAME);

            vector<TableConnector> sflow_tables = {
                conf_port_table,
                state_port_table,
                conf_sflow_table,
                conf_sflow_session_table
            };
            m_sflowMgr.reset(new SflowMgr(m_app_db.get(), sflow_tables));
        }

        void enableSflow()
        {
            Table cfg_sflow(m_config_db.get(), CFG_SFLOW_TABLE_NAME);
            cfg_sflow.set("global", {
                {"admin_state", "up"}
            });
            m_sflowMgr->addExistingData(&cfg_sflow);
            m_sflowMgr->doTask();
        }

        void cfgSflowSession(string alias, bool status, string sample_rate, string direction = "")
        {
            Table cfg_sflow_table(m_config_db.get(), CFG_SFLOW_SESSION_TABLE_NAME);
            vector<FieldValueTuple> values;
            values.emplace_back("admin_state", status ? "up" : "down");
            if (!sample_rate.empty())
            {
                values.emplace_back("sample_rate", sample_rate);
            }
            if (!direction.empty())
            {
                values.emplace_back("sample_direction", direction);
            }
            cfg_sflow_table.set(alias, values);
            m_sflowMgr->addExistingData(&cfg_sflow_table);
            m_sflowMgr->doTask();
        }

        void cfgSflowSessionAll(bool status)
        {
            Table cfg_sflow_table(m_config_db.get(), CFG_SFLOW_SESSION_TABLE_NAME);
            cfg_sflow_table.set("all", {
                {"admin_state", status ? "up" : "down"},
            });
            m_sflowMgr->addExistingData(&cfg_sflow_table);
            m_sflowMgr->doTask();
        }

        void cfgPortSpeed(string alias, string speed)
        {
            Table cfg_port_table(m_config_db.get(), CFG_PORT_TABLE_NAME);
            cfg_port_table.set(alias, {
                {"speed", speed}
            });
            m_sflowMgr->addExistingData(&cfg_port_table);
            m_sflowMgr->doTask();
        }

        void statePortSpeed(string alias, string speed)
        {
            Table state_port_table(m_config_db.get(), STATE_PORT_TABLE_NAME);
            state_port_table.set(alias, {
                {"speed", speed}
            });
            m_sflowMgr->addExistingData(&state_port_table);
            m_sflowMgr->doTask();
        }

        string getSflowSampleRate(string alias)
        {
            Table appl_sflow_table(m_app_db.get(), APP_SFLOW_SESSION_TABLE_NAME);
            std::vector<FieldValueTuple> values;
            appl_sflow_table.get(alias, values);
            auto value_rate = swss::fvsGetValue(values, "sample_rate", true);
            if (value_rate)
            {
                string ret = value_rate.get();
                return ret;
            }
            return "";
        }

        string getSflowSampleDir(string alias)
        {
            Table appl_sflow_table(m_app_db.get(), APP_SFLOW_SESSION_TABLE_NAME);
            std::vector<FieldValueTuple> values;
            appl_sflow_table.get(alias, values);
            auto value_rate = swss::fvsGetValue(values, "sample_direction", true);
            if (value_rate)
            {
                string ret = value_rate.get();
                return ret;
            }
            return "";
        }

        string getSflowAdminStatus(string alias)
        {
            Table appl_sflow_table(m_app_db.get(), APP_SFLOW_SESSION_TABLE_NAME);
            std::vector<FieldValueTuple> values;
            appl_sflow_table.get(alias, values);
            auto value_rate = swss::fvsGetValue(values, "admin_state", true);
            if (value_rate)
            {
                string ret = value_rate.get();
                return ret;
            }
            return "down";
        }
    };

    TEST_F(SflowMgrTest, test_RateConfiguration)
    {
        enableSflow();
        cfgPortSpeed("Ethernet0", "100000");
        ASSERT_TRUE(getSflowSampleRate("Ethernet0") == "100000");

        /* Scenario: Operational Speed Changes to 25000 */
        statePortSpeed("Ethernet0", "25000");
        ASSERT_TRUE(getSflowSampleRate("Ethernet0") == "25000");
        ASSERT_TRUE(getSflowSampleDir("Ethernet0") == "rx");
    }

    TEST_F(SflowMgrTest, test_RateConfigurationCfgSpeed)
    {
        enableSflow();
        /* Configure the Speed to 100G */
        cfgPortSpeed("Ethernet0", "100000");

        /* Scenario: Operational Speed Changes to 100G with autoneg */
        statePortSpeed("Ethernet0", "100000");

        /* User changes the config speed to 10G */
        cfgPortSpeed("Ethernet0", "10000");

        ASSERT_TRUE(getSflowSampleRate("Ethernet0") == "100000");

        /* Scenario: Operational Speed Changes to 10G, with autoneg */
        statePortSpeed("Ethernet0", "10000");
        ASSERT_TRUE(getSflowSampleRate("Ethernet0") == "10000");

        /* Configured speed is updated by user */
        cfgPortSpeed("Ethernet0", "200000");

        /* Sampling Rate will not be updated */
        ASSERT_TRUE(getSflowSampleRate("Ethernet0") == "10000");
    }

    TEST_F(SflowMgrTest, test_OnlyStateDbNotif)
    {
        enableSflow();
        statePortSpeed("Ethernet0", "100000");
        ASSERT_TRUE(getSflowSampleRate("Ethernet0") == "");
    }

    TEST_F(SflowMgrTest, test_LocalRateConfiguration)
    {
        enableSflow();
        cfgPortSpeed("Ethernet0", "100000");
        cfgSflowSession("Ethernet0", true, "12345");
        ASSERT_TRUE(getSflowSampleRate("Ethernet0") == "12345");
    }

    TEST_F(SflowMgrTest, test_LocalRateConfWithOperSpeed)
    {
        enableSflow();
        cfgPortSpeed("Ethernet0", "100000");

        /* Scenario: Operational Speed Changes to 25000 */
        statePortSpeed("Ethernet0", "25000");

        /* Set per interface sampling rate*/
        cfgSflowSession("Ethernet0", true, "12345");
        ASSERT_TRUE(getSflowSampleRate("Ethernet0") == "12345");

        /* Operational Speed Changes again to 50000 */
        statePortSpeed("Ethernet0", "50000");
        ASSERT_TRUE(getSflowSampleRate("Ethernet0") == "12345");
    }

    TEST_F(SflowMgrTest, test_newSpeed)
    {
        enableSflow();
        cfgPortSpeed("Ethernet0", "800000");
        ASSERT_TRUE(getSflowSampleRate("Ethernet0") == "800000");
    }

    TEST_F(SflowMgrTest, test_CfgSpeedAdminCfg)
    {
        enableSflow();
        cfgPortSpeed("Ethernet0", "100000");
        cfgSflowSessionAll(false); /* Disable sflow on all interfaces*/
        ASSERT_TRUE(getSflowAdminStatus("Ethernet0") == "down");
        cfgSflowSession("Ethernet0", true, ""); /* Set local admin up with no rate */
        ASSERT_TRUE(getSflowAdminStatus("Ethernet0") == "up");

        /* Sampling rate should adhere to config speed*/
        ASSERT_TRUE(getSflowSampleRate("Ethernet0") == "100000");

        cfgPortSpeed("Ethernet0", "25000"); /* Change cfg speed */
        ASSERT_TRUE(getSflowSampleRate("Ethernet0") == "25000");
    }

    TEST_F(SflowMgrTest, test_OperSpeedAdminCfg)
    {
        enableSflow();
        cfgPortSpeed("Ethernet0", "100000");
        cfgSflowSessionAll(false); /* Disable sflow on all interfaces*/
        cfgSflowSession("Ethernet0", true, ""); /* Set local admin up with no rate */
        ASSERT_TRUE(getSflowSampleRate("Ethernet0") == "100000");
        ASSERT_TRUE(getSflowAdminStatus("Ethernet0") == "up");

        statePortSpeed("Ethernet0", "50000");
        /* Sampling rate should adhere to oper speed*/
        ASSERT_TRUE(getSflowSampleRate("Ethernet0") == "50000");
        ASSERT_TRUE(getSflowAdminStatus("Ethernet0") == "up");

        /* Change cfg speed */
        cfgPortSpeed("Ethernet0", "25000");
        ASSERT_TRUE(getSflowSampleRate("Ethernet0") == "50000");

        statePortSpeed("Ethernet0", "1000");
        ASSERT_TRUE(getSflowSampleRate("Ethernet0") == "1000");

        cfgSflowSession("Ethernet0", true, "12345"); /* Set local sampling rate */
        ASSERT_TRUE(getSflowSampleRate("Ethernet0") == "12345");
        ASSERT_TRUE(getSflowAdminStatus("Ethernet0") == "up");

        /* Change oper speed now */
        statePortSpeed("Ethernet0", "12345");
        ASSERT_TRUE(getSflowSampleRate("Ethernet0") == "12345");
    }

    TEST_F(SflowMgrTest, test_SflowCfgAfterPortCfg)
    {
        cfgPortSpeed("Ethernet0", "100000");
        /* Nothing is written yet since cfg is not enabled */
        ASSERT_TRUE(getSflowSampleRate("Ethernet0") == "");
        ASSERT_TRUE(getSflowAdminStatus("Ethernet0") == "down");

        /* State DB is updated with oper speed */
        statePortSpeed("Ethernet0", "100000");
        ASSERT_TRUE(getSflowSampleRate("Ethernet0") == "");
        ASSERT_TRUE(getSflowAdminStatus("Ethernet0") == "down");

        /* enable sflow */
        enableSflow();
        cfgSflowSessionAll(true);
        ASSERT_TRUE(getSflowSampleRate("Ethernet0") == "100000");
        ASSERT_TRUE(getSflowAdminStatus("Ethernet0") == "up");
        ASSERT_TRUE(getSflowSampleDir("Ethernet0") == "rx");
    }

    TEST_F(SflowMgrTest, test_SflowCfgAfterOperSpeed)
    {
        cfgPortSpeed("Ethernet0", "100000");
        /* Nothing is written yet since cfg is not enabled */
        ASSERT_TRUE(getSflowSampleRate("Ethernet0") == "");
        ASSERT_TRUE(getSflowAdminStatus("Ethernet0") == "down");

        /* State DB is updated with oper speed */
        statePortSpeed("Ethernet0", "50000");
        ASSERT_TRUE(getSflowSampleRate("Ethernet0") == "");
        ASSERT_TRUE(getSflowAdminStatus("Ethernet0") == "down");

        /* enable sflow */
        cfgSflowSessionAll(true);
        enableSflow();
        ASSERT_TRUE(getSflowSampleRate("Ethernet0") == "50000");
        ASSERT_TRUE(getSflowAdminStatus("Ethernet0") == "up");
        ASSERT_TRUE(getSflowSampleDir("Ethernet0") == "rx");
    }

    TEST_F(SflowMgrTest, test_RateConfigEgressDir)
    {
        enableSflow();
        cfgPortSpeed("Ethernet0", "100000");
        /* Set local admin up with no rate and no egress direction */
        cfgSflowSession("Ethernet0", true, "", "tx");
        ASSERT_TRUE(getSflowSampleRate("Ethernet0") == "100000");

        /* Scenario: Operational Speed Changes to 25000 */
        statePortSpeed("Ethernet0", "25000");
        ASSERT_TRUE(getSflowSampleRate("Ethernet0") == "25000");
        ASSERT_TRUE(getSflowSampleDir("Ethernet0") == "tx");
    }
}
