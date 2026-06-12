#include "gtest/gtest.h"
#include <iostream>
#include <fstream>
#include <unistd.h>
#include <sys/stat.h>
#include "../mock_table.h"
#include "warm_restart.h"
#include "macaddress.h"
#define private public
#include "intfmgr.h"
#undef private

extern int (*callback)(const std::string &cmd, std::string &stdout);
extern std::vector<std::string> mockCallArgs;
extern swss::MacAddress gMacAddress;
extern swss::MacAddress gSagMacAddress;

bool Ethernet0IPv6Set = false;
bool FailBridgeFdbCommand = false;

int cb(const std::string &cmd, std::string &stdout){
    mockCallArgs.push_back(cmd);
    if (cmd == "sysctl -w net.ipv6.conf.\"Ethernet0\".disable_ipv6=0") Ethernet0IPv6Set = true;
    else if (cmd.find("/sbin/ip -6 address \"add\"") == 0) {
        return Ethernet0IPv6Set ? 0 : 2;
    }
    else if (cmd == "/sbin/ip link set \"Ethernet64.10\" \"up\""){
        return 1;
    }
    else if (cmd.find("/sbin/ip address show ") == 0) {
        stdout = "0\n";
        return 0;
    }
    else if (cmd.find("bridge fdb") == 0) {
        return FailBridgeFdbCommand ? 1 : 0;
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
            FailBridgeFdbCommand = false;
        }
    };

    static bool commandWasIssued(const std::string &needle)
    {
        for (const auto &cmd : mockCallArgs)
        {
            if (cmd.find(needle) != std::string::npos)
            {
                return true;
            }
        }
        return false;
    }

    static bool getFieldValue(const std::vector<swss::FieldValueTuple> &values,
                              const std::string &field,
                              std::string &value)
    {
        for (const auto &fv : values)
        {
            if (fvField(fv) == field)
            {
                value = fvValue(fv);
                return true;
            }
        }
        return false;
    }

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

        /* Now delete the link-local address and verify it is no longer replayed */
        intfmgr.doIntfAddrTask(llKeys, emptyData, "DEL");
        ASSERT_EQ(intfmgr.m_intfLLAddresses.count("Ethernet0"), 0u);

        mockCallArgs.clear();
        intfmgr.doPortTableTask("Ethernet0", portData, "SET");

        ipv6_ll_add_called = 0;
        for (const auto &cmd : mockCallArgs)
        {
            if (cmd.find("/sbin/ip -6 address \"add\"") != std::string::npos &&
                cmd.find("fe80::1/64") != std::string::npos)
            {
                ipv6_ll_add_called++;
            }
        }
        ASSERT_EQ(ipv6_ll_add_called, 0);
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

    TEST_F(IntfMgrTest, testSetSagFdbEntryValidationAndBridgeCommand){
        gMacAddress = swss::MacAddress("00:11:22:33:44:55");
        swss::IntfMgr intfmgr(m_config_db.get(), m_app_db.get(), m_state_db.get(), cfg_intf_tables);

        mockCallArgs.clear();
        intfmgr.setSagFdbEntry("update", "Vlan100", "02:03:04:05:06:07");
        intfmgr.setSagFdbEntry("replace", "Ethernet0", "02:03:04:05:06:07");
        intfmgr.setSagFdbEntry("replace", "VlanABC", "02:03:04:05:06:07");
        intfmgr.setSagFdbEntry("replace", "Vlan100", gMacAddress.to_string());
        EXPECT_TRUE(mockCallArgs.empty());

        FailBridgeFdbCommand = true;
        intfmgr.setSagFdbEntry("replace", "Vlan100", "02:03:04:05:06:07");
        ASSERT_EQ(mockCallArgs.size(), 1u);
        EXPECT_EQ(mockCallArgs[0], "bridge fdb replace 02:03:04:05:06:07 dev Bridge vlan 100 permanent");
    }

    TEST_F(IntfMgrTest, testUpdateSagMacProgramsSagVlans){
        gMacAddress = swss::MacAddress("00:11:22:33:44:55");
        gSagMacAddress = swss::MacAddress("00:aa:bb:cc:dd:ee");
        swss::IntfMgr intfmgr(m_config_db.get(), m_app_db.get(), m_state_db.get(), cfg_intf_tables);

        intfmgr.m_cfgVlanIntfTable.set("Vlan100", {
            {"static_anycast_gateway", "true"},
            {"proxy_arp", "enabled"}
        });
        intfmgr.m_cfgVlanIntfTable.set("Vlan200", {
            {"static_anycast_gateway", "false"}
        });
        intfmgr.m_cfgVlanIntfTable.set("Vlan300|10.0.0.1/24", {
            {"static_anycast_gateway", "true"}
        });

        mockCallArgs.clear();
        intfmgr.updateSagMac("02:03:04:05:06:07");

        EXPECT_TRUE(commandWasIssued("/sbin/ip link set \"Vlan100\" down"));
        EXPECT_TRUE(commandWasIssued("/sbin/ip link set Vlan100 address 02:03:04:05:06:07"));
        EXPECT_TRUE(commandWasIssued("/sbin/ip link set \"Vlan100\" up"));
        EXPECT_TRUE(commandWasIssued("bridge fdb del 00:aa:bb:cc:dd:ee dev Bridge vlan 100 permanent"));
        EXPECT_TRUE(commandWasIssued("bridge fdb replace 02:03:04:05:06:07 dev Bridge vlan 100 permanent"));
        EXPECT_FALSE(commandWasIssued("Vlan200 address"));
        EXPECT_FALSE(commandWasIssued("Vlan300"));

        swss::Table appIntfTable(m_app_db.get(), APP_INTF_TABLE_NAME);
        std::vector<swss::FieldValueTuple> values;
        ASSERT_TRUE(appIntfTable.get("Vlan100", values));
        std::string mac;
        ASSERT_TRUE(getFieldValue(values, "mac_addr", mac));
        EXPECT_EQ(mac, "02:03:04:05:06:07");
    }

    TEST_F(IntfMgrTest, testDoSagTaskSetAndDelete){
        gMacAddress = swss::MacAddress("00:11:22:33:44:55");
        gSagMacAddress = swss::MacAddress("00:00:00:00:00:00");
        swss::IntfMgr intfmgr(m_config_db.get(), m_app_db.get(), m_state_db.get(), cfg_intf_tables);

        intfmgr.m_cfgVlanIntfTable.set("Vlan100", {
            {"static_anycast_gateway", "true"}
        });

        const std::vector<std::string> keys = {"GLOBAL"};
        mockCallArgs.clear();
        intfmgr.doSagTask(keys, {}, SET_COMMAND);
        EXPECT_TRUE(mockCallArgs.empty());

        intfmgr.doSagTask(keys, {{"gateway_mac", "02:03:04:05:06:07"}}, SET_COMMAND);
        EXPECT_TRUE(commandWasIssued("bridge fdb replace 02:03:04:05:06:07 dev Bridge vlan 100 permanent"));

        swss::Table appSagTable(m_app_db.get(), APP_SAG_TABLE_NAME);
        std::vector<swss::FieldValueTuple> values;
        ASSERT_TRUE(appSagTable.get("GLOBAL", values));
        std::string mac;
        ASSERT_TRUE(getFieldValue(values, "gateway_mac", mac));
        EXPECT_EQ(mac, "02:03:04:05:06:07");

        mockCallArgs.clear();
        intfmgr.doSagTask(keys, {}, DEL_COMMAND);
        EXPECT_TRUE(commandWasIssued("/sbin/ip link set Vlan100 address 00:11:22:33:44:55"));
        EXPECT_TRUE(commandWasIssued("bridge fdb del 02:03:04:05:06:07 dev Bridge vlan 100 permanent"));
        EXPECT_FALSE(commandWasIssued("bridge fdb replace 00:11:22:33:44:55"));
        EXPECT_FALSE(appSagTable.get("GLOBAL", values));

        intfmgr.doSagTask(keys, {}, "UNKNOWN");
    }

    TEST_F(IntfMgrTest, testDoIntfGeneralTaskStaticAnycastGateway)
    {
        gMacAddress = swss::MacAddress("00:11:22:33:44:55");
        gSagMacAddress = swss::MacAddress("00:aa:bb:cc:dd:ee");
        swss::IntfMgr intfmgr(m_config_db.get(), m_app_db.get(), m_state_db.get(), cfg_intf_tables);

        intfmgr.m_stateVlanTable.set("Vlan100", {{"state", "ok"}}, "SET", "");
        intfmgr.m_stateVlanTable.set("Vlan200", {{"state", "ok"}}, "SET", "");
        intfmgr.m_cfgSagTable.set("GLOBAL", {{"gateway_mac", "02:03:04:05:06:07"}});

        mockCallArgs.clear();
        EXPECT_TRUE(intfmgr.doIntfGeneralTask({"Vlan100"}, {{"static_anycast_gateway", "true"}}, SET_COMMAND));
        EXPECT_TRUE(commandWasIssued("/sbin/ip link set \"Vlan100\" down"));
        EXPECT_TRUE(commandWasIssued("/sbin/ip link set Vlan100 address 02:03:04:05:06:07"));
        EXPECT_TRUE(commandWasIssued("/sbin/ip link set \"Vlan100\" up"));
        EXPECT_TRUE(commandWasIssued("bridge fdb replace 02:03:04:05:06:07 dev Bridge vlan 100 permanent"));
        EXPECT_TRUE(intfmgr.m_sagIntfList.at("Vlan100"));

        swss::Table appIntfTable(m_app_db.get(), APP_INTF_TABLE_NAME);
        std::vector<swss::FieldValueTuple> values;
        ASSERT_TRUE(appIntfTable.get("Vlan100", values));
        std::string mac;
        ASSERT_TRUE(getFieldValue(values, "mac_addr", mac));
        EXPECT_EQ(mac, "02:03:04:05:06:07");

        mockCallArgs.clear();
        EXPECT_TRUE(intfmgr.doIntfGeneralTask({"Vlan100"}, {}, DEL_COMMAND));
        EXPECT_TRUE(commandWasIssued("bridge fdb del 00:aa:bb:cc:dd:ee dev Bridge vlan 100 permanent"));
        EXPECT_TRUE(commandWasIssued("/sbin/ip link set Vlan100 address 00:11:22:33:44:55"));
        EXPECT_EQ(intfmgr.m_sagIntfList.count("Vlan100"), 0u);

        mockCallArgs.clear();
        EXPECT_TRUE(intfmgr.doIntfGeneralTask({"Vlan200"}, {{"static_anycast_gateway", "false"}}, SET_COMMAND));
        EXPECT_TRUE(commandWasIssued("bridge fdb del 00:aa:bb:cc:dd:ee dev Bridge vlan 200 permanent"));
        EXPECT_TRUE(commandWasIssued("/sbin/ip link set Vlan200 address 00:11:22:33:44:55"));
        ASSERT_TRUE(appIntfTable.get("Vlan200", values));
        ASSERT_TRUE(getFieldValue(values, "mac_addr", mac));
        EXPECT_EQ(mac, swss::MacAddress().to_string());

        mockCallArgs.clear();
        EXPECT_TRUE(intfmgr.doIntfGeneralTask({"Vlan200"}, {{"static_anycast_gateway", "invalid"}}, SET_COMMAND));
        EXPECT_FALSE(commandWasIssued("bridge fdb"));
    }

}
