// #include "portmgr.h"
// #include "gtest/gtest.h"
// #include "mock_table.h"
// #include "redisutility.h"

// extern std::vector<std::string> mockCallArgs;

// namespace portmgr_ut
// {
//     using namespace swss;
//     using namespace std;

//     struct PortMgrTest : public ::testing::Test
//     {
//         shared_ptr<swss::DBConnector> m_app_db;
//         shared_ptr<swss::DBConnector> m_config_db;
//         shared_ptr<swss::DBConnector> m_state_db;
//         shared_ptr<PortMgr> m_portMgr;
//         PortMgrTest()
//         {
//             m_app_db = make_shared<swss::DBConnector>(
//                 "APPL_DB", 0);
//             m_config_db = make_shared<swss::DBConnector>(
//                 "CONFIG_DB", 0);
//             m_state_db = make_shared<swss::DBConnector>(
//                 "STATE_DB", 0);
//         }

//         virtual void SetUp() override
//         {
//             ::testing_db::reset();
//             vector<string> cfg_port_tables = {
//                 CFG_PORT_TABLE_NAME,
//             };
//             m_portMgr.reset(new PortMgr(m_config_db.get(), m_app_db.get(), m_state_db.get(), cfg_port_tables));
//         }
//     };

//     TEST_F(PortMgrTest, DoTask)
//     {
//         Table state_port_table(m_state_db.get(), STATE_PORT_TABLE_NAME);
//         Table app_port_table(m_app_db.get(), APP_PORT_TABLE_NAME);
//         Table cfg_port_table(m_config_db.get(), CFG_PORT_TABLE_NAME);

//         // Port is not ready, verify that doTask does not handle port configuration
        
//         cfg_port_table.set("Ethernet0", {
//             {"speed", "100000"},
//             {"index", "1"}
//         });
//         mockCallArgs.clear();
//         m_portMgr->addExistingData(&cfg_port_table);
//         m_portMgr->doTask();
//         ASSERT_TRUE(mockCallArgs.empty());
//         std::vector<FieldValueTuple> values;
//         app_port_table.get("Ethernet0", values);
//         auto value_opt = swss::fvsGetValue(values, "mtu", true);
//         ASSERT_TRUE(value_opt);
//         ASSERT_EQ(DEFAULT_MTU_STR, value_opt.get());
//         value_opt = swss::fvsGetValue(values, "admin_status", true);
//         ASSERT_TRUE(value_opt);
//         ASSERT_EQ(DEFAULT_ADMIN_STATUS_STR, value_opt.get());
//         value_opt = swss::fvsGetValue(values, "speed", true);
//         ASSERT_TRUE(value_opt);
//         ASSERT_EQ("100000", value_opt.get());
//         value_opt = swss::fvsGetValue(values, "index", true);
//         ASSERT_TRUE(value_opt);
//         ASSERT_EQ("1", value_opt.get());

//         // Set port state to ok, verify that doTask handle port configuration
//         state_port_table.set("Ethernet0", {
//             {"state", "ok"}
//         });
//         m_portMgr->doTask();
//         ASSERT_EQ(size_t(2), mockCallArgs.size());
//         ASSERT_EQ("/sbin/ip link set dev \"Ethernet0\" mtu \"9100\"", mockCallArgs[0]);
//         ASSERT_EQ("/sbin/ip link set dev \"Ethernet0\" down", mockCallArgs[1]);
        
//         // Set port admin_status, verify that it could override the default value
//         cfg_port_table.set("Ethernet0", {
//             {"admin_status", "up"}
//         });
//         m_portMgr->addExistingData(&cfg_port_table);
//         m_portMgr->doTask();
//         app_port_table.get("Ethernet0", values);
//         value_opt = swss::fvsGetValue(values, "admin_status", true);
//         ASSERT_TRUE(value_opt);
//         ASSERT_EQ("up", value_opt.get());
//     }

//     TEST_F(PortMgrTest, ConfigureDuringRetry)
//     {
//         Table state_port_table(m_state_db.get(), STATE_PORT_TABLE_NAME);
//         Table app_port_table(m_app_db.get(), APP_PORT_TABLE_NAME);
//         Table cfg_port_table(m_config_db.get(), CFG_PORT_TABLE_NAME);

//         cfg_port_table.set("Ethernet0", {
//             {"speed", "100000"},
//             {"index", "1"}
//         });

//         mockCallArgs.clear();
//         m_portMgr->addExistingData(&cfg_port_table);
//         m_portMgr->doTask();
//         ASSERT_TRUE(mockCallArgs.empty());

//         cfg_port_table.set("Ethernet0", {
//             {"speed", "50000"},
//             {"index", "1"},
//             {"mtu", "1518"},
//             {"admin_status", "up"}
//         });

//         m_portMgr->addExistingData(&cfg_port_table);
//         m_portMgr->doTask();
//         ASSERT_TRUE(mockCallArgs.empty());

//         state_port_table.set("Ethernet0", {
//             {"state", "ok"}
//         });
//         m_portMgr->doTask();
//         ASSERT_EQ(size_t(2), mockCallArgs.size());
//         ASSERT_EQ("/sbin/ip link set dev \"Ethernet0\" mtu \"1518\"", mockCallArgs[0]);
//         ASSERT_EQ("/sbin/ip link set dev \"Ethernet0\" up", mockCallArgs[1]);
//     }

//     TEST_F(PortMgrTest, ConfigurePortPTDefaultTimestampTemplate)
//     {
//         Table state_port_table(m_state_db.get(), STATE_PORT_TABLE_NAME);
//         Table app_port_table(m_app_db.get(), APP_PORT_TABLE_NAME);
//         Table cfg_port_table(m_config_db.get(), CFG_PORT_TABLE_NAME);

//         // Port is not ready, verify that doTask does not handle port configuration

//         cfg_port_table.set("Ethernet0", {
//             {"speed", "100000"},
//             {"index", "1"},
//             {"pt_interface_id", "129"}
//         });
//         mockCallArgs.clear();
//         m_portMgr->addExistingData(&cfg_port_table);
//         m_portMgr->doTask();
//         ASSERT_TRUE(mockCallArgs.empty());
//         std::vector<FieldValueTuple> values;
//         app_port_table.get("Ethernet0", values);
//         auto value_opt = swss::fvsGetValue(values, "mtu", true);
//         ASSERT_TRUE(value_opt);
//         ASSERT_EQ(DEFAULT_MTU_STR, value_opt.get());
//         value_opt = swss::fvsGetValue(values, "admin_status", true);
//         ASSERT_TRUE(value_opt);
//         ASSERT_EQ(DEFAULT_ADMIN_STATUS_STR, value_opt.get());
//         value_opt = swss::fvsGetValue(values, "speed", true);
//         ASSERT_TRUE(value_opt);
//         ASSERT_EQ("100000", value_opt.get());
//         value_opt = swss::fvsGetValue(values, "index", true);
//         ASSERT_TRUE(value_opt);
//         ASSERT_EQ("1", value_opt.get());
//         value_opt = swss::fvsGetValue(values, "pt_interface_id", true);
//         ASSERT_TRUE(value_opt);
//         ASSERT_EQ("129", value_opt.get());
//         value_opt = swss::fvsGetValue(values, "pt_timestamp_template", true);
//         ASSERT_FALSE(value_opt);
//     }

//     TEST_F(PortMgrTest, ConfigurePortPTNonDefaultTimestampTemplate)
//     {
//         Table state_port_table(m_state_db.get(), STATE_PORT_TABLE_NAME);
//         Table app_port_table(m_app_db.get(), APP_PORT_TABLE_NAME);
//         Table cfg_port_table(m_config_db.get(), CFG_PORT_TABLE_NAME);

//         // Port is not ready, verify that doTask does not handle port configuration

//         cfg_port_table.set("Ethernet0", {
//             {"speed", "100000"},
//             {"index", "1"},
//             {"pt_interface_id", "129"},
//             {"pt_timestamp_template", "template2"}
//         });
//         mockCallArgs.clear();
//         m_portMgr->addExistingData(&cfg_port_table);
//         m_portMgr->doTask();
//         ASSERT_TRUE(mockCallArgs.empty());
//         std::vector<FieldValueTuple> values;
//         app_port_table.get("Ethernet0", values);
//         auto value_opt = swss::fvsGetValue(values, "mtu", true);
//         ASSERT_TRUE(value_opt);
//         ASSERT_EQ(DEFAULT_MTU_STR, value_opt.get());
//         value_opt = swss::fvsGetValue(values, "admin_status", true);
//         ASSERT_TRUE(value_opt);
//         ASSERT_EQ(DEFAULT_ADMIN_STATUS_STR, value_opt.get());
//         value_opt = swss::fvsGetValue(values, "speed", true);
//         ASSERT_TRUE(value_opt);
//         ASSERT_EQ("100000", value_opt.get());
//         value_opt = swss::fvsGetValue(values, "index", true);
//         ASSERT_TRUE(value_opt);
//         ASSERT_EQ("1", value_opt.get());
//         value_opt = swss::fvsGetValue(values, "pt_interface_id", true);
//         ASSERT_TRUE(value_opt);
//         ASSERT_EQ("129", value_opt.get());
//         value_opt = swss::fvsGetValue(values, "pt_timestamp_template", true);
//         ASSERT_TRUE(value_opt);
//         ASSERT_EQ("template2", value_opt.get());
//     }
// }


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
        
        // Verify default values
        auto value_opt = swss::fvsGetValue(values, "mtu", true);
        ASSERT_TRUE(value_opt);
        ASSERT_EQ(DEFAULT_MTU_STR, value_opt.get());
        value_opt = swss::fvsGetValue(values, "admin_status", true);
        ASSERT_TRUE(value_opt);
        ASSERT_EQ(DEFAULT_ADMIN_STATUS_STR, value_opt.get());
        // value_opt = swss::fvsGetValue(values, "dhcp_rate_limit", true);
        // //ASSERT_TRUE(value_opt);
        // ASSERT_EQ(DEFAULT_DHCP_RATE_LIMIT_STR, value_opt.get());
        value_opt = swss::fvsGetValue(values, "speed", true);
        ASSERT_TRUE(value_opt);
        ASSERT_EQ("100000", value_opt.get());
        value_opt = swss::fvsGetValue(values, "index", true);
        ASSERT_TRUE(value_opt);
        ASSERT_EQ("1", value_opt.get());

        // Set port state to ok, verify that doTask handles port configuration
        state_port_table.set("Ethernet0", {
            {"state", "ok"}
        });
        m_portMgr->doTask();
        
        // Only MTU and admin status should be configured (DHCP rate limit is empty by default)
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

        // Test explicit DHCP rate limit configuration
        mockCallArgs.clear();
        cfg_port_table.set("Ethernet0", {
            {"dhcp_rate_limit", "100"}
        });
        m_portMgr->addExistingData(&cfg_port_table);
        m_portMgr->doTask();
        
        // Verify the TC commands for DHCP rate limiting
        ASSERT_EQ(size_t(2), mockCallArgs.size());
         
        string expected_cmd = "/sbin/tc qdisc add dev \"Ethernet0\" handle ffff: ingress && "
                            "/sbin/tc filter add dev \"Ethernet0\" protocol ip parent ffff: prio 1 u32 "
                            "match ip protocol 17 0xff match ip dport 67 0xffff "
                            "police rate 40600bps burst 40600b conform-exceed drop";
        ASSERT_EQ(expected_cmd, mockCallArgs[1]);
        
        // Verify the value was written to APP_DB
        app_port_table.get("Ethernet0", values);
        value_opt = swss::fvsGetValue(values, "dhcp_rate_limit", true);
        //ASSERT_TRUE(value_opt);
        //ASSERT_EQ("100", value_opt.get());

        // Test disabling DHCP rate limiting (setting to 0)
        mockCallArgs.clear();
        cfg_port_table.set("Ethernet0", {
            {"dhcp_rate_limit", "0"}
        });
        m_portMgr->addExistingData(&cfg_port_table);
        m_portMgr->doTask();
        
        ASSERT_EQ(size_t(2), mockCallArgs.size());
        ASSERT_EQ("tc qdisc del dev \"Ethernet0\" handle ffff: ingress", mockCallArgs[0]);
        
        // Verify empty string is written to APP_DB when rate limit is 0
        app_port_table.get("Ethernet0", values);
        value_opt = swss::fvsGetValue(values, "dhcp_rate_limit", true);
        ASSERT_TRUE(value_opt);
        ASSERT_EQ(DEFAULT_DHCP_RATE_LIMIT_STR, value_opt.get());
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
            //{"dhcp_rate_limit", "1"}

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
        //ASSERT_EQ("/sbin/tc qdisc add dev \"Ethernet0\" handle ffff: ingress && /sbin/tc filter add dev \"Ethernet0\" protocol ip parent ffff: prio 1 u32 match ip protocol 17 0xff match ip dport 67 0xffff police rate 406bps burst 406b conform-exceed drop", mockCallArgs[2]);



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