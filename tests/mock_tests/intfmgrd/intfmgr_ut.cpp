#include "gtest/gtest.h"
#include <iostream>
#include <fstream>
#include <unistd.h>
#include <sys/stat.h>
#include "../mock_table.h"
#include "warm_restart.h"
#define private public
#include "intfmgr.h"
#undef private

extern int (*callback)(const std::string &cmd, std::string &stdout);
extern std::vector<std::string> mockCallArgs;

bool Ethernet0IPv6Set = false;

int cb(const std::string &cmd, std::string &stdout){
    mockCallArgs.push_back(cmd);
    if (cmd == "sysctl -w net.ipv6.conf.\"Ethernet0\".disable_ipv6=0") Ethernet0IPv6Set = true;
    else if (cmd.find("/sbin/ip -6 address \"add\"") == 0) {
        return Ethernet0IPv6Set ? 0 : 2;
    }
    else if (cmd == "/sbin/ip link set \"Ethernet64.10\" \"up\""){
        return 1;
    }
    else {
        return 0;
    }
    return 0;
}

// Test Fixture
namespace intfmgr_ut
{
    struct IntfMgrTest : public ::testing::Test
    {
        std::shared_ptr<swss::DBConnector> m_config_db;
        std::shared_ptr<swss::DBConnector> m_app_db;
        std::shared_ptr<swss::DBConnector> m_state_db;
        std::vector<std::string> cfg_intf_tables;

        virtual void SetUp() override
        {
            testing_db::reset();
            m_config_db = std::make_shared<swss::DBConnector>("CONFIG_DB", 0);
            m_app_db = std::make_shared<swss::DBConnector>("APPL_DB", 0);
            m_state_db = std::make_shared<swss::DBConnector>("STATE_DB", 0);

            swss::WarmStart::initialize("intfmgrd", "swss");

            std::vector<std::string> tables = {
                CFG_INTF_TABLE_NAME,
                CFG_LAG_INTF_TABLE_NAME,
                CFG_VLAN_INTF_TABLE_NAME,
                CFG_LOOPBACK_INTERFACE_TABLE_NAME,
                CFG_VLAN_SUB_INTF_TABLE_NAME,
                CFG_VOQ_INBAND_INTERFACE_TABLE_NAME,
            };
            cfg_intf_tables = tables;
            mockCallArgs.clear();
            callback = cb;
        }
    };

    TEST_F(IntfMgrTest, testSettingIpv6Flag){
        Ethernet0IPv6Set = false;
        swss::IntfMgr intfmgr(m_config_db.get(), m_app_db.get(), m_state_db.get(), cfg_intf_tables);
        /* Set portStateTable */
        std::vector<swss::FieldValueTuple> values;
        values.emplace_back("state", "ok");
        intfmgr.m_statePortTable.set("Ethernet0", values, "SET", "");
        /* Set m_stateIntfTable */
        values.clear();
        values.emplace_back("vrf", "");
        intfmgr.m_stateIntfTable.set("Ethernet0", values, "SET", "");
        /* Set Ipv6 prefix */
        const std::vector<std::string>& keys = {"Ethernet0", "2001::8/64"};
        const std::vector<swss::FieldValueTuple> data;
        intfmgr.doIntfAddrTask(keys, data, "SET");
        int ip_cmd_called = 0;
        for (auto cmd : mockCallArgs){
            if (cmd.find("/sbin/ip -6 address \"add\"") == 0){
                ip_cmd_called++;
            }
        }
        ASSERT_EQ(ip_cmd_called, 2);
    }

    TEST_F(IntfMgrTest, testNoSettingIpv6Flag){
        Ethernet0IPv6Set = true; // Assuming it is already set by SDK
        swss::IntfMgr intfmgr(m_config_db.get(), m_app_db.get(), m_state_db.get(), cfg_intf_tables);
        /* Set portStateTable */
        std::vector<swss::FieldValueTuple> values;
        values.emplace_back("state", "ok");
        intfmgr.m_statePortTable.set("Ethernet0", values, "SET", "");
        /* Set m_stateIntfTable */
        values.clear();
        values.emplace_back("vrf", "");
        intfmgr.m_stateIntfTable.set("Ethernet0", values, "SET", "");
        /* Set Ipv6 prefix */
        const std::vector<std::string>& keys = {"Ethernet0", "2001::8/64"};
        const std::vector<swss::FieldValueTuple> data;
        intfmgr.doIntfAddrTask(keys, data, "SET");
        int ip_cmd_called = 0;
        for (auto cmd : mockCallArgs){
            if (cmd.find("/sbin/ip -6 address \"add\"") == 0){
                ip_cmd_called++;
            }
        }
        ASSERT_EQ(ip_cmd_called, 1);
    }

    //This test except no runtime error when the set admin status command failed
    //and the subinterface has not ok status (for example not existing subinterface)
    TEST_F(IntfMgrTest, testSetAdminStatusFailToNotOkSubInt){
        swss::IntfMgr intfmgr(m_config_db.get(), m_app_db.get(), m_state_db.get(), cfg_intf_tables);
        intfmgr.setHostSubIntfAdminStatus("Ethernet64.10", "up", "up");
    }

    //This test except runtime error when the set admin status command failed
    //and the subinterface has ok status
    TEST_F(IntfMgrTest, testSetAdminStatusFailToOkSubInt){
        swss::IntfMgr intfmgr(m_config_db.get(), m_app_db.get(), m_state_db.get(), cfg_intf_tables);
        /* Set portStateTable */
        std::vector<swss::FieldValueTuple> values;
        values.emplace_back("state", "ok");
        intfmgr.m_statePortTable.set("Ethernet64.10", values, "SET", "");
        EXPECT_THROW(intfmgr.setHostSubIntfAdminStatus("Ethernet64.10", "up", "up"), std::runtime_error);
    }

    TEST_F(IntfMgrTest, testReplayLLIpv6AddressOnAdminUp){
        Ethernet0IPv6Set = true;
        swss::IntfMgr intfmgr(m_config_db.get(), m_app_db.get(), m_state_db.get(), cfg_intf_tables);

        /* Set portStateTable and stateIntfTable so doIntfAddrTask proceeds */
        std::vector<swss::FieldValueTuple> values;
        values.emplace_back("state", "ok");
        intfmgr.m_statePortTable.set("Ethernet0", values, "SET", "");
        values.clear();
        values.emplace_back("vrf", "");
        intfmgr.m_stateIntfTable.set("Ethernet0", values, "SET", "");

        /* Add an IPv6 link-local address via doIntfAddrTask to populate the cache */
        const std::vector<std::string> llKeys = {"Ethernet0", "fe80::1/64"};
        const std::vector<swss::FieldValueTuple> emptyData;
        intfmgr.doIntfAddrTask(llKeys, emptyData, "SET");

        /* Also add a global IPv6 address — this should NOT be replayed */
        const std::vector<std::string> globalKeys = {"Ethernet0", "2001::8/64"};
        intfmgr.doIntfAddrTask(globalKeys, emptyData, "SET");

        /* Also add an IPv4 address — this should NOT be replayed */
        const std::vector<std::string> ipv4Keys = {"Ethernet0", "10.0.0.1/31"};
        intfmgr.doIntfAddrTask(ipv4Keys, emptyData, "SET");

        mockCallArgs.clear();

        /* Simulate admin up by calling doPortTableTask */
        std::vector<swss::FieldValueTuple> portData;
        portData.emplace_back("admin_status", "up");
        intfmgr.doPortTableTask("Ethernet0", portData, "SET");

        /* Verify that only IPv6 link-local address add was called */
        int ipv6_ll_add_called = 0;
        int ipv6_global_add_called = 0;
        int ipv4_add_called = 0;
        for (const auto &cmd : mockCallArgs)
        {
            if (cmd.find("/sbin/ip -6 address \"add\"") != std::string::npos &&
                cmd.find("fe80::1/64") != std::string::npos)
            {
                ipv6_ll_add_called++;
            }
            if (cmd.find("/sbin/ip -6 address \"add\"") != std::string::npos &&
                cmd.find("2001::8/64") != std::string::npos)
            {
                ipv6_global_add_called++;
            }
            if (cmd.find("/sbin/ip address \"add\"") != std::string::npos &&
                cmd.find("10.0.0.1/31") != std::string::npos)
            {
                ipv4_add_called++;
            }
        }
        ASSERT_EQ(ipv6_ll_add_called, 1);
        ASSERT_EQ(ipv6_global_add_called, 0);
        ASSERT_EQ(ipv4_add_called, 0);
    }

    TEST_F(IntfMgrTest, testNoReplayLLOnAdminDown){
        Ethernet0IPv6Set = true;
        swss::IntfMgr intfmgr(m_config_db.get(), m_app_db.get(), m_state_db.get(), cfg_intf_tables);

        /* Set portStateTable and stateIntfTable so doIntfAddrTask proceeds */
        std::vector<swss::FieldValueTuple> values;
        values.emplace_back("state", "ok");
        intfmgr.m_statePortTable.set("Ethernet0", values, "SET", "");
        values.clear();
        values.emplace_back("vrf", "");
        intfmgr.m_stateIntfTable.set("Ethernet0", values, "SET", "");

        /* Add an IPv6 link-local address via doIntfAddrTask to populate the cache */
        const std::vector<std::string> llKeys = {"Ethernet0", "fe80::1/64"};
        const std::vector<swss::FieldValueTuple> emptyData;
        intfmgr.doIntfAddrTask(llKeys, emptyData, "SET");

        mockCallArgs.clear();

        /* Simulate admin down — should NOT trigger replay */
        std::vector<swss::FieldValueTuple> portData;
        portData.emplace_back("admin_status", "down");
        intfmgr.doPortTableTask("Ethernet0", portData, "SET");

        int ipv6_add_called = 0;
        for (const auto &cmd : mockCallArgs)
        {
            if (cmd.find("/sbin/ip -6 address \"add\"") != std::string::npos)
            {
                ipv6_add_called++;
            }
        }
        ASSERT_EQ(ipv6_add_called, 0);
    }
}
