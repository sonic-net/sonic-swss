#include "portmgr.h"
#include "gtest/gtest.h"
#include "mock_table.h"
#include "redisutility.h"

extern std::vector<std::string> mockCallArgs;

namespace portmgr_ut
{
    using namespace swss;
    using namespace std;

    struct PortMgrTest : public ::testing::Test
    {
        shared_ptr<swss::DBConnector> m_app_db;
        shared_ptr<swss::DBConnector> m_config_db;
        shared_ptr<swss::DBConnector> m_state_db;
        shared_ptr<PortMgr> m_portMgr;
        PortMgrTest()
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
            vector<string> cfg_port_tables = {
                CFG_PORT_TABLE_NAME,
            };
            m_portMgr.reset(new PortMgr(m_config_db.get(), m_app_db.get(), m_state_db.get(), cfg_port_tables));
        }
    };

    TEST_F(PortMgrTest, DoTask)
    {
        Table state_port_table(m_state_db.get(), STATE_PORT_TABLE_NAME);
        Table app_port_table(m_app_db.get(), APP_PORT_TABLE_NAME);
        Table cfg_port_table(m_config_db.get(), CFG_PORT_TABLE_NAME);

        // Port is not ready, verify that doTask does not handle port configuration
        
        cfg_port_table.set("Ethernet0", {
            {"speed", "100000"},
            {"index", "1"}
        });
        mockCallArgs.clear();
        m_portMgr->addExistingData(&cfg_port_table);
        m_portMgr->doTask();
        ASSERT_TRUE(mockCallArgs.empty());
        std::vector<FieldValueTuple> values;
        app_port_table.get("Ethernet0", values);
        auto value_opt = swss::fvsGetValue(values, "mtu", true);
        ASSERT_TRUE(value_opt);
        ASSERT_EQ(DEFAULT_MTU_STR, value_opt.get());
        value_opt = swss::fvsGetValue(values, "admin_status", true);
        ASSERT_TRUE(value_opt);
        ASSERT_EQ(DEFAULT_ADMIN_STATUS_STR, value_opt.get());
        value_opt = swss::fvsGetValue(values, "speed", true);
        ASSERT_TRUE(value_opt);
        ASSERT_EQ("100000", value_opt.get());
        value_opt = swss::fvsGetValue(values, "index", true);
        ASSERT_TRUE(value_opt);
        ASSERT_EQ("1", value_opt.get());

        // Set port state to ok, verify that doTask handle port configuration
        state_port_table.set("Ethernet0", {
            {"state", "ok"}
        });
        m_portMgr->doTask();
        ASSERT_EQ(size_t(2), mockCallArgs.size());
        ASSERT_EQ("/sbin/ip link set dev \"Ethernet0\" mtu \"9100\"", mockCallArgs[0]);
        ASSERT_EQ("/sbin/ip link set dev \"Ethernet0\" down", mockCallArgs[1]);
        
        // Set port admin_status, verify that it could override the default value
        cfg_port_table.set("Ethernet0", {
            {"admin_status", "up"}
        });
        m_portMgr->addExistingData(&cfg_port_table);
        m_portMgr->doTask();
        app_port_table.get("Ethernet0", values);
        value_opt = swss::fvsGetValue(values, "admin_status", true);
        ASSERT_TRUE(value_opt);
        ASSERT_EQ("up", value_opt.get());
    }

    TEST_F(PortMgrTest, ConfigureDuringRetry)
    {
        Table state_port_table(m_state_db.get(), STATE_PORT_TABLE_NAME);
        Table app_port_table(m_app_db.get(), APP_PORT_TABLE_NAME);
        Table cfg_port_table(m_config_db.get(), CFG_PORT_TABLE_NAME);

        cfg_port_table.set("Ethernet0", {
            {"speed", "100000"},
            {"index", "1"}
        });

        mockCallArgs.clear();
        m_portMgr->addExistingData(&cfg_port_table);
        m_portMgr->doTask();
        ASSERT_TRUE(mockCallArgs.empty());

        cfg_port_table.set("Ethernet0", {
            {"speed", "50000"},
            {"index", "1"},
            {"mtu", "1518"},
            {"admin_status", "up"}
        });

        m_portMgr->addExistingData(&cfg_port_table);
        m_portMgr->doTask();
        ASSERT_TRUE(mockCallArgs.empty());

        state_port_table.set("Ethernet0", {
            {"state", "ok"}
        });
        m_portMgr->doTask();
        ASSERT_EQ(size_t(2), mockCallArgs.size());
        ASSERT_EQ("/sbin/ip link set dev \"Ethernet0\" mtu \"1518\"", mockCallArgs[0]);
        ASSERT_EQ("/sbin/ip link set dev \"Ethernet0\" up", mockCallArgs[1]);
    }

    TEST_F(PortMgrTest, ConfigurePortPTDefaultTimestampTemplate)
    {
        Table state_port_table(m_state_db.get(), STATE_PORT_TABLE_NAME);
        Table app_port_table(m_app_db.get(), APP_PORT_TABLE_NAME);
        Table cfg_port_table(m_config_db.get(), CFG_PORT_TABLE_NAME);

        // Port is not ready, verify that doTask does not handle port configuration

        cfg_port_table.set("Ethernet0", {
            {"speed", "100000"},
            {"index", "1"},
            {"pt_interface_id", "129"}
        });
        mockCallArgs.clear();
        m_portMgr->addExistingData(&cfg_port_table);
        m_portMgr->doTask();
        ASSERT_TRUE(mockCallArgs.empty());
        std::vector<FieldValueTuple> values;
        app_port_table.get("Ethernet0", values);
        auto value_opt = swss::fvsGetValue(values, "mtu", true);
        ASSERT_TRUE(value_opt);
        ASSERT_EQ(DEFAULT_MTU_STR, value_opt.get());
        value_opt = swss::fvsGetValue(values, "admin_status", true);
        ASSERT_TRUE(value_opt);
        ASSERT_EQ(DEFAULT_ADMIN_STATUS_STR, value_opt.get());
        value_opt = swss::fvsGetValue(values, "speed", true);
        ASSERT_TRUE(value_opt);
        ASSERT_EQ("100000", value_opt.get());
        value_opt = swss::fvsGetValue(values, "index", true);
        ASSERT_TRUE(value_opt);
        ASSERT_EQ("1", value_opt.get());
        value_opt = swss::fvsGetValue(values, "pt_interface_id", true);
        ASSERT_TRUE(value_opt);
        ASSERT_EQ("129", value_opt.get());
        value_opt = swss::fvsGetValue(values, "pt_timestamp_template", true);
        ASSERT_FALSE(value_opt);
    }

    TEST_F(PortMgrTest, SetDHCPMitigationRateSuccess)
    {
        Table state_port_table(m_state_db.get(), STATE_PORT_TABLE_NAME);
        state_port_table.set("Ethernet0", {{"state", "ok"}});

        mockCallArgs.clear();
        ASSERT_TRUE(m_portMgr->setPortDHCPMitigationRate("Ethernet0", "100"));
        ASSERT_EQ(1, mockCallArgs.size());
        // Verify the TC command was constructed correctly
        ASSERT_NE(string::npos, mockCallArgs[0].find("tc qdisc add dev \"Ethernet0\" handle ffff: ingress"));
        ASSERT_NE(string::npos, mockCallArgs[0].find("match ip dport 67 0xffff police rate 72000bps burst 72000b"));
    }

    TEST_F(PortMgrTest, SetDHCPMitigationRateZero)
    {
        Table state_port_table(m_state_db.get(), STATE_PORT_TABLE_NAME);
        state_port_table.set("Ethernet0", {{"state", "ok"}});

        mockCallArgs.clear();
        ASSERT_TRUE(m_portMgr->setPortDHCPMitigationRate("Ethernet0", "0"));
        ASSERT_EQ(1, mockCallArgs.size());
        ASSERT_NE(string::npos, mockCallArgs[0].find("tc qdisc del dev \"Ethernet0\" handle ffff: ingress"));
    }

    TEST_F(PortMgrTest, SetDHCPMitigationRateEmpty)
    {
        Table state_port_table(m_state_db.get(), STATE_PORT_TABLE_NAME);
        state_port_table.set("Ethernet0", {{"state", "ok"}});

        mockCallArgs.clear();
        ASSERT_TRUE(m_portMgr->setPortDHCPMitigationRate("Ethernet0", ""));
        ASSERT_TRUE(mockCallArgs.empty()); // Should do nothing for empty string
    }

    TEST_F(PortMgrTest, SetDHCPMitigationRatePortNotReady)
    {
        mockCallArgs.clear();
        ASSERT_FALSE(m_portMgr->setPortDHCPMitigationRate("Ethernet0", "100"));
        // Should log warning but not execute command
        ASSERT_TRUE(mockCallArgs.empty());
    }

    TEST_F(PortMgrTest, SetDHCPMitigationRateInvalidNumber)
    {
        Table state_port_table(m_state_db.get(), STATE_PORT_TABLE_NAME);
        state_port_table.set("Ethernet0", {{"state", "ok"}});

        mockCallArgs.clear();
        ASSERT_FALSE(m_portMgr->setPortDHCPMitigationRate("Ethernet0", "invalid"));
        // Should log error about invalid number
        ASSERT_TRUE(mockCallArgs.empty());
    }

    TEST_F(PortMgrTest, DoTaskWithDHCPRateLimit)
    {
        Table state_port_table(m_state_db.get(), STATE_PORT_TABLE_NAME);
        Table app_port_table(m_app_db.get(), APP_PORT_TABLE_NAME);
        Table cfg_port_table(m_config_db.get(), CFG_PORT_TABLE_NAME);

        // First make port ready
        state_port_table.set("Ethernet0", {{"state", "ok"}});

        // Configure port with DHCP rate limit
        cfg_port_table.set("Ethernet0", {
            {"dhcp_rate_limit", "200"},
            {"admin_status", "up"}
        });

        mockCallArgs.clear();
        m_portMgr->addExistingData(&cfg_port_table);
        m_portMgr->doTask();

        // Verify all expected commands were executed
        ASSERT_EQ(3, mockCallArgs.size());
        ASSERT_NE(string::npos, mockCallArgs[0].find("mtu \"9100\"")); // Default MTU
        ASSERT_NE(string::npos, mockCallArgs[1].find("link set dev \"Ethernet0\" up"));
        ASSERT_NE(string::npos, mockCallArgs[2].find("tc qdisc add dev \"Ethernet0\""));
    }

    TEST_F(PortMgrTest, DoTaskWithDHCPRateLimitPortNotReady)
    {
        Table app_port_table(m_app_db.get(), APP_PORT_TABLE_NAME);
        Table cfg_port_table(m_config_db.get(), CFG_PORT_TABLE_NAME);

        // Port not ready yet
        cfg_port_table.set("Ethernet0", {
            {"dhcp_rate_limit", "200"},
            {"admin_status", "up"}
        });

        mockCallArgs.clear();
        m_portMgr->addExistingData(&cfg_port_table);
        m_portMgr->doTask();

        // Should write to APP DB but not execute commands
        ASSERT_TRUE(mockCallArgs.empty());
        
        // Verify config was written to APP DB
        std::vector<FieldValueTuple> values;
        app_port_table.get("Ethernet0", values);
        auto value_opt = swss::fvsGetValue(values, "dhcp_rate_limit", true);
        ASSERT_TRUE(value_opt);
        ASSERT_EQ("200", value_opt.get());
    }

    TEST_F(PortMgrTest, SetPortMtuFailurePortNotReady)
    {
        mockCallArgs.clear();
        ASSERT_FALSE(m_portMgr->setPortMtu("Ethernet0", "9100"));
        // Should log warning but not throw
        ASSERT_TRUE(mockCallArgs.empty());
    }

    TEST_F(PortMgrTest, SetPortAdminStatusFailurePortNotReady)
    {
        mockCallArgs.clear();
        ASSERT_FALSE(m_portMgr->setPortAdminStatus("Ethernet0", true));
        // Should log warning but not throw
        ASSERT_TRUE(mockCallArgs.empty());
    }

    TEST_F(PortMgrTest, SetPortAdminStatusFailure)
    {
        Table state_port_table(m_state_db.get(), STATE_PORT_TABLE_NAME);
        state_port_table.set("Ethernet0", {{"state", "ok"}});

        // Simulate command failure by making mock return error
        mockCallArgs.clear();
        ::testing_db::setExecResult(1, "Simulated failure");
        
        EXPECT_THROW(m_portMgr->setPortAdminStatus("Ethernet0", true), runtime_error);
        ASSERT_EQ(1, mockCallArgs.size());
    }

    TEST_F(PortMgrTest, ConfigurePortPTNonDefaultTimestampTemplate)
    {
        Table state_port_table(m_state_db.get(), STATE_PORT_TABLE_NAME);
        Table app_port_table(m_app_db.get(), APP_PORT_TABLE_NAME);
        Table cfg_port_table(m_config_db.get(), CFG_PORT_TABLE_NAME);

        // Port is not ready, verify that doTask does not handle port configuration

        cfg_port_table.set("Ethernet0", {
            {"speed", "100000"},
            {"index", "1"},
            {"pt_interface_id", "129"},
            {"pt_timestamp_template", "template2"}
        });
        mockCallArgs.clear();
        m_portMgr->addExistingData(&cfg_port_table);
        m_portMgr->doTask();
        ASSERT_TRUE(mockCallArgs.empty());
        std::vector<FieldValueTuple> values;
        app_port_table.get("Ethernet0", values);
        auto value_opt = swss::fvsGetValue(values, "mtu", true);
        ASSERT_TRUE(value_opt);
        ASSERT_EQ(DEFAULT_MTU_STR, value_opt.get());
        value_opt = swss::fvsGetValue(values, "admin_status", true);
        ASSERT_TRUE(value_opt);
        ASSERT_EQ(DEFAULT_ADMIN_STATUS_STR, value_opt.get());
        value_opt = swss::fvsGetValue(values, "speed", true);
        ASSERT_TRUE(value_opt);
        ASSERT_EQ("100000", value_opt.get());
        value_opt = swss::fvsGetValue(values, "index", true);
        ASSERT_TRUE(value_opt);
        ASSERT_EQ("1", value_opt.get());
        value_opt = swss::fvsGetValue(values, "pt_interface_id", true);
        ASSERT_TRUE(value_opt);
        ASSERT_EQ("129", value_opt.get());
        value_opt = swss::fvsGetValue(values, "pt_timestamp_template", true);
        ASSERT_TRUE(value_opt);
        ASSERT_EQ("template2", value_opt.get());
    }
}
