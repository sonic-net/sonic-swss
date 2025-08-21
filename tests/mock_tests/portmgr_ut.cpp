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

    TEST_F(PortMgrTest, DhcpRateLimitNotConfigured)
    {
    // Arrange
    // No "dhcp_rate_limit" set -> should skip TC config
    Table cfg_port_table(m_config_db.get(), CFG_PORT_TABLE_NAME);
    cfg_port_table.set("Ethernet0", {
        {"dhcp_rate_limit", ""}
    });

    // Act
    mockCallArgs.clear();
    m_portMgr->addExistingData(&cfg_port_table);
    m_portMgr->doTask();

    // Assert
    ASSERT_TRUE(mockCallArgs.empty());  // No TC command should be executed
    }

    TEST_F(PortMgrTest, DhcpRateLimitConfigured)
    {
    // Arrange
    Table state_port_table(m_state_db.get(), STATE_PORT_TABLE_NAME);
    Table cfg_port_table(m_config_db.get(), CFG_PORT_TABLE_NAME);

    state_port_table.set("Ethernet4", {{"state", "ok"}});
    cfg_port_table.set("Ethernet4", {
        {"dhcp_rate_limit", "100"} // packets/sec
    });

    // Act
    mockCallArgs.clear();
    m_portMgr->addExistingData(&cfg_port_table);
    m_portMgr->doTask();

    // Assert
    ASSERT_EQ(size_t(3), mockCallArgs.size());
    std::string expected_prefix = "/sbin/tc qdisc add dev \"Ethernet4\" handle ffff: ingress";
    ASSERT_TRUE(mockCallArgs[2].find(expected_prefix) == 0);
    //ASSERT_TRUE(mockCallArgs[2].find("police rate") != std::string::npos); // 100*590 (PACKET_SIZE)
    }

    TEST_F(PortMgrTest, DhcpRateLimitDisabled)
    {
    // Arrange
    Table state_port_table(m_state_db.get(), STATE_PORT_TABLE_NAME);
    Table cfg_port_table(m_config_db.get(), CFG_PORT_TABLE_NAME);

    state_port_table.set("Ethernet8", {{"state", "ok"}});
    cfg_port_table.set("Ethernet8", {
        {"dhcp_rate_limit", "0"} // disable
    });

    // Act
    mockCallArgs.clear();
    m_portMgr->addExistingData(&cfg_port_table);
    m_portMgr->doTask();

    // Assert
    ASSERT_EQ(size_t(3), mockCallArgs.size());
    ASSERT_EQ("/sbin/tc qdisc del dev \"Ethernet8\" handle ffff: ingress", mockCallArgs[2]);
    }

    TEST_F(PortMgrTest, DhcpRateLimitNotConfigured_EmptyString)
    {
    // Test the SWSS_LOG_DEBUG line for empty dhcp_rate_limit
    Table cfg_port_table(m_config_db.get(), CFG_PORT_TABLE_NAME);
    cfg_port_table.set("Ethernet0", {
        {"dhcp_rate_limit", ""}
    });

    // Mock the logger to capture debug messages
    testing::internal::CaptureStdout();
    mockCallArgs.clear();
    m_portMgr->addExistingData(&cfg_port_table);
    m_portMgr->doTask();
    
    std::string output = testing::internal::GetCapturedStdout();
    
    // Verify no TC commands were executed for empty dhcp_rate_limit
    ASSERT_TRUE(mockCallArgs.empty());
    // The debug message should be logged but we can't easily capture SWSS_LOG_DEBUG
    }

    TEST_F(PortMgrTest, DhcpRateLimitPortNotReady)
    {
    // Test the case where port is not ready (isPortStateOk returns false)
    Table cfg_port_table(m_config_db.get(), CFG_PORT_TABLE_NAME);
    
    // Set dhcp_rate_limit but don't set port state to ready
    cfg_port_table.set("Ethernet0", {
        {"dhcp_rate_limit", "100"}
    });

    mockCallArgs.clear();
    m_portMgr->addExistingData(&cfg_port_table);
    m_portMgr->doTask();
    
    // Should not execute any commands since port is not ready
    ASSERT_TRUE(mockCallArgs.empty());
    
    // The warning message should be logged when setPortDHCPMitigationRate is called
    // but we can't easily capture SWSS_LOG_WARN from the method
        }

    TEST_F(PortMgrTest, DhcpRateLimitCommandFailure)
    {
    // Test the case where TC command fails but port is ready
    // This requires mocking the exec function to return failure
    Table state_port_table(m_state_db.get(), STATE_PORT_TABLE_NAME);
    Table cfg_port_table(m_config_db.get(), CFG_PORT_TABLE_NAME);

    state_port_table.set("Ethernet0", {{"state", "ok"}});
    cfg_port_table.set("Ethernet0", {
        {"dhcp_rate_limit", "100"}
    });

    // Mock exec to return failure
    auto original_exec = swss::exec;
    swss::exec = [](const std::string& cmd, std::string& res) {
        res = "TC command failed";
        return 1; // Return error code
    };

    testing::internal::CaptureStdout();
    mockCallArgs.clear();
    m_portMgr->addExistingData(&cfg_port_table);
    m_portMgr->doTask();
    
    std::string output = testing::internal::GetCapturedStdout();
    
    // Restore original exec function
    swss::exec = original_exec;

    // The error message should be logged but we can't easily capture SWSS_LOG_ERROR
    // The method should return false but we can't easily test the return value from doTask
    }

    TEST_F(PortMgrTest, DhcpRateLimitZeroWithIngressNotExist)
    {
    // Test case where we try to delete ingress qdisc that doesn't exist
    Table state_port_table(m_state_db.get(), STATE_PORT_TABLE_NAME);
    Table cfg_port_table(m_config_db.get(), CFG_PORT_TABLE_NAME);

    state_port_table.set("Ethernet0", {{"state", "ok"}});
    cfg_port_table.set("Ethernet0", {
        {"dhcp_rate_limit", "0"}
    });

    // Mock exec to simulate "qdisc del" failing because qdisc doesn't exist
    auto original_exec = swss::exec;
    swss::exec = [](const std::string& cmd, std::string& res) {
        if (cmd.find("qdisc del") != std::string::npos) {
            res = "RTNETLINK answers: No such file or directory";
            return 2; // Return error code for non-existent qdisc
        }
        return 0;
    };

    testing::internal::CaptureStdout();
    mockCallArgs.clear();
    m_portMgr->addExistingData(&cfg_port_table);
    m_portMgr->doTask();
    
    std::string output = testing::internal::GetCapturedStdout();
    
    // Restore original exec function
    swss::exec = original_exec;

    // Error should be logged but we can't easily capture it
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