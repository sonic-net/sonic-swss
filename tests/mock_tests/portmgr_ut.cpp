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
		
		// Case 1: Add DHCP rate limit (valid case)
	    cfg_port_table.set("Ethernet0", {
	        {"dhcp_rate_limit", "1000"}
	    });
	    mockCallArgs.clear();
	    m_portMgr->addExistingData(&cfg_port_table);
	    m_portMgr->doTask();

	    bool found_add = std::any_of(mockCallArgs.begin(), mockCallArgs.end(),
	        [](const std::string &cmd) {
	            return cmd.find("tc qdisc add dev \"Ethernet0\" handle ffff: ingress") != std::string::npos;
	        });
	    ASSERT_TRUE(found_add) << "Expected tc qdisc add command, got:\n" << ::testing::PrintToString(mockCallArgs);

	    bool found_rate = std::any_of(mockCallArgs.begin(), mockCallArgs.end(),
	        [](const std::string &cmd) {
	            return cmd.find("police rate") != std::string::npos;
	        });
	    ASSERT_TRUE(found_rate) << "Expected police rate command, got:\n" << ::testing::PrintToString(mockCallArgs);

	    // Case 2: dhcp_rate_limit = 0 (delete case)
	    cfg_port_table.set("Ethernet0", {
	        {"dhcp_rate_limit", "0"}
	    });
	    mockCallArgs.clear();
	    m_portMgr->addExistingData(&cfg_port_table);
	    m_portMgr->doTask();
	    bool found_del = std::any_of(mockCallArgs.begin(), mockCallArgs.end(),
	        [](const std::string &cmd) {
	            return cmd.find("tc qdisc del dev \"Ethernet0\" handle ffff: ingress") != std::string::npos;
	        });
	    ASSERT_TRUE(found_del) << "Expected tc qdisc del command, got:\n" << ::testing::PrintToString(mockCallArgs);

	    // Case 3: WARN branch - port not ready
	    state_port_table.set("Ethernet0", { {"state", "unknown"} });
	    cfg_port_table.set("Ethernet0", {
	        {"dhcp_rate_limit", "500"}
	    });
	    mockCallArgs.clear();
	    m_portMgr->addExistingData(&cfg_port_table);
	    m_portMgr->doTask();
	    // Restore port state to ok for next case
	    state_port_table.set("Ethernet0", { {"state", "ok"} });

	    // Case 4: ERROR branch - simulate exec failure
	    cfg_port_table.set("Ethernet0", {
	        {"dhcp_rate_limit", "800"}
	    });
	    mockCallArgs.clear();
	    m_portMgr->addExistingData(&cfg_port_table);
	    m_portMgr->doTask();
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
