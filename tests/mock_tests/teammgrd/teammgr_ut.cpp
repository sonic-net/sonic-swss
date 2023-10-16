#include "gtest/gtest.h"
#include "../mock_table.h"
#include "teammgr.h"

extern int (*callback)(const std::string &cmd, std::string &stdout);
extern std::vector<std::string> mockCallArgs;

int cb(const std::string &cmd, std::string &stdout)
{
    mockCallArgs.push_back(cmd);
    if (cmd.find("/usr/bin/teamd -r -t PortChannel1") != std::string::npos)
    {
        return 1;
    }
    else if (cmd.find("cat \"/var/run/teamd/PortChannel1.pid\"") != std::string::npos)
    {
        stdout = "1234";
        return 0;
    }
    return 0;
}

namespace teammgr_ut
{
    struct TeamMgrTest : public ::testing::Test
    {
        std::shared_ptr<swss::DBConnector> m_config_db;
        std::shared_ptr<swss::DBConnector> m_app_db;
        std::shared_ptr<swss::DBConnector> m_state_db;
        std::vector<TableConnector> cfg_lag_tables;

        virtual void SetUp() override
        {
            testing_db::reset();
            m_config_db = std::make_shared<swss::DBConnector>("CONFIG_DB", 0);
            m_app_db = std::make_shared<swss::DBConnector>("APPL_DB", 0);
            m_state_db = std::make_shared<swss::DBConnector>("STATE_DB", 0);

            swss::Table metadata_table = swss::Table(m_config_db.get(), CFG_DEVICE_METADATA_TABLE_NAME);
            std::vector<swss::FieldValueTuple> vec;
            vec.emplace_back("mac", "01:23:45:67:89:ab");
            metadata_table.set("localhost", vec);

            TableConnector conf_lag_table(m_config_db.get(), CFG_LAG_TABLE_NAME);
            TableConnector conf_lag_member_table(m_config_db.get(), CFG_LAG_MEMBER_TABLE_NAME);
            TableConnector state_port_table(m_state_db.get(), STATE_PORT_TABLE_NAME);

            std::vector<TableConnector> tables = {
                conf_lag_table,
                conf_lag_member_table,
                state_port_table
            };

            cfg_lag_tables = tables;
            mockCallArgs.clear();
            callback = cb;
        }
    };

    TEST_F(TeamMgrTest, testProcessKilledAfterAddLagFailure)
    {
        swss::TeamMgr teammgr(m_config_db.get(), m_app_db.get(), m_state_db.get(), cfg_lag_tables);
        swss::Table cfg_lag_table = swss::Table(m_config_db.get(), CFG_LAG_TABLE_NAME);
        cfg_lag_table.set("PortChannel1", { { "admin_status", "up" },
                                            { "mtu", "9100" },
                                            { "lacp_key", "auto" },
                                            { "min_links", "2" } });
        teammgr.addExistingData(&cfg_lag_table);
        teammgr.doTask();
        int kill_cmd_called = 0;
        for (auto cmd : mockCallArgs){
            if (cmd.find("kill -TERM 1234") != std::string::npos){
                kill_cmd_called++;
            }
        }
        ASSERT_EQ(kill_cmd_called, 1);
    }
}