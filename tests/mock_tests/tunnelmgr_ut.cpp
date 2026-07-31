#include "gtest/gtest.h"

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include "mock_table.h"
#include "schema.h"
#include "warm_restart.h"

#define private public
#include "tunnelmgr.h"
#undef private

extern int (*callback)(const std::string &cmd, std::string &stdout);
extern std::vector<std::string> mockCallArgs;

namespace tunnelmgr_ut
{

class TunnelMgrTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        testing_db::reset();
        m_configDb = std::make_shared<swss::DBConnector>("CONFIG_DB", 0);
        m_appDb = std::make_shared<swss::DBConnector>("APPL_DB", 0);

        swss::Table peerTable(m_configDb.get(), CFG_PEER_SWITCH_TABLE_NAME);
        peerTable.set("peer_switch", {{"address_ipv4", "10.1.0.33"}});

        callback = nullptr;
        mockCallArgs.clear();
        swss::WarmStart::initialize("tunnelmgrd", "swss");
        m_tunnelMgr = std::make_unique<swss::TunnelMgr>(
            m_configDb.get(),
            m_appDb.get(),
            std::vector<std::string>{CFG_TUNNEL_TABLE_NAME, CFG_LOOPBACK_INTERFACE_TABLE_NAME});
    }

    void TearDown() override
    {
        callback = nullptr;
    }

    static bool commandWasIssued(const std::string &needle)
    {
        return std::any_of(mockCallArgs.begin(), mockCallArgs.end(),
                           [&needle](const std::string &cmd) { return cmd.find(needle) != std::string::npos; });
    }

    static bool hasField(const std::vector<swss::FieldValueTuple> &values, const std::string &field)
    {
        return std::any_of(values.begin(), values.end(),
                           [&field](const swss::FieldValueTuple &fv) { return fvField(fv) == field; });
    }

    std::shared_ptr<swss::DBConnector> m_configDb;
    std::shared_ptr<swss::DBConnector> m_appDb;
    std::unique_ptr<swss::TunnelMgr> m_tunnelMgr;
};

TEST_F(TunnelMgrTest, ConstructorRemovesStaleKernelTunnel)
{
    EXPECT_TRUE(commandWasIssued("tunnel del tun0"));
}

TEST_F(TunnelMgrTest, KernelTunnelSettingIsPerEntry)
{
    const swss::KeyOpFieldsValuesTuple enabledTunnelConfig{
        "MuxTunnelWithKernelEntry",
        SET_COMMAND,
        {{"tunnel_type", "IPINIP"},
         {"dst_ip", "10.1.0.32"},
         {"src_ip", "10.1.0.33"},
         {"kernel_tunnel_enabled", "true"}}};
    const swss::KeyOpFieldsValuesTuple disabledTunnelConfig{
        "MuxTunnelWithoutKernelEntry",
        SET_COMMAND,
        {{"tunnel_type", "IPINIP"},
         {"dst_ip", "10.1.0.34"},
         {"src_ip", "10.1.0.33"},
         {"dscp_mode", "pipe"},
         {"kernel_tunnel_enabled", "false"}}};

    mockCallArgs.clear();
    EXPECT_TRUE(m_tunnelMgr->doTunnelTask(enabledTunnelConfig));
    EXPECT_TRUE(commandWasIssued("tunnel add tun0"));
    EXPECT_TRUE(commandWasIssued("link set dev tun0 up"));

    mockCallArgs.clear();
    EXPECT_TRUE(m_tunnelMgr->doTunnelTask(disabledTunnelConfig));
    EXPECT_FALSE(commandWasIssued("tunnel add tun0"));
    EXPECT_FALSE(commandWasIssued("link set dev tun0 up"));

    std::vector<swss::FieldValueTuple> appTunnelValues;
    swss::Table appTunnelTable(m_appDb.get(), APP_TUNNEL_DECAP_TABLE_NAME);
    ASSERT_TRUE(appTunnelTable.get("MuxTunnelWithoutKernelEntry", appTunnelValues));
    EXPECT_TRUE(hasField(appTunnelValues, "tunnel_type"));
    EXPECT_FALSE(hasField(appTunnelValues, "dst_ip"));
    EXPECT_FALSE(hasField(appTunnelValues, "kernel_tunnel_enabled"));

    swss::Table appTunnelTermTable(m_appDb.get(), APP_TUNNEL_DECAP_TERM_TABLE_NAME);
    ASSERT_TRUE(appTunnelTermTable.get("MuxTunnelWithoutKernelEntry:10.1.0.34", appTunnelValues));

    mockCallArgs.clear();
    EXPECT_TRUE(m_tunnelMgr->doLpbkIntfTask(
        {"Loopback3|10.1.0.32/32", SET_COMMAND, {}}));
    EXPECT_TRUE(commandWasIssued("addr add \"10.1.0.32/32\" dev tun0"));

    mockCallArgs.clear();
    EXPECT_TRUE(m_tunnelMgr->doTunnelRouteTask(
        {"192.0.2.1/32", SET_COMMAND, {}}));
    EXPECT_TRUE(commandWasIssued("route replace \"192.0.2.1/32\" dev tun0"));

    mockCallArgs.clear();
    EXPECT_TRUE(m_tunnelMgr->doTunnelTask(
        {"MuxTunnelWithoutKernelEntry", DEL_COMMAND, {}}));
    EXPECT_TRUE(mockCallArgs.empty());
    EXPECT_EQ(m_tunnelMgr->m_tunnelCache.count("MuxTunnelWithoutKernelEntry"), 0u);
    EXPECT_FALSE(appTunnelTable.get("MuxTunnelWithoutKernelEntry", appTunnelValues));
    EXPECT_FALSE(appTunnelTermTable.get("MuxTunnelWithoutKernelEntry:10.1.0.34", appTunnelValues));
}

TEST_F(TunnelMgrTest, DisabledOnlySkipsDependentKernelOperations)
{
    mockCallArgs.clear();
    EXPECT_TRUE(m_tunnelMgr->doTunnelTask(
        {"MuxTunnelWithoutKernelEntry",
         SET_COMMAND,
         {{"tunnel_type", "IPINIP"},
          {"dst_ip", "10.1.0.34"},
          {"src_ip", "10.1.0.33"},
          {"kernel_tunnel_enabled", "false"}}}));
    EXPECT_TRUE(mockCallArgs.empty());

    EXPECT_TRUE(m_tunnelMgr->doLpbkIntfTask(
        {"Loopback3|10.1.0.32/32", SET_COMMAND, {}}));
    EXPECT_TRUE(m_tunnelMgr->doTunnelRouteTask(
        {"192.0.2.1/32", SET_COMMAND, {}}));
    EXPECT_TRUE(mockCallArgs.empty());
}

TEST_F(TunnelMgrTest, InvalidKernelTunnelSettingIsRejected)
{
    mockCallArgs.clear();
    EXPECT_FALSE(m_tunnelMgr->doTunnelTask(
        {"MuxTunnel0",
         SET_COMMAND,
         {{"tunnel_type", "IPINIP"},
          {"dst_ip", "10.1.0.32"},
          {"src_ip", "10.1.0.33"},
          {"kernel_tunnel_enabled", "False"}}}));
    EXPECT_TRUE(mockCallArgs.empty());
    EXPECT_EQ(m_tunnelMgr->m_tunnelCache.count("MuxTunnel0"), 0u);

    std::vector<swss::FieldValueTuple> appTunnelValues;
    swss::Table appTunnelTable(m_appDb.get(), APP_TUNNEL_DECAP_TABLE_NAME);
    EXPECT_FALSE(appTunnelTable.get("MuxTunnel0", appTunnelValues));
}

TEST_F(TunnelMgrTest, KernelTunnelRemainsEnabledByDefault)
{
    const swss::KeyOpFieldsValuesTuple tunnelConfig{
        "MuxTunnel0",
        SET_COMMAND,
        {{"tunnel_type", "IPINIP"}, {"dst_ip", "10.1.0.32"}, {"src_ip", "10.1.0.33"}}};

    mockCallArgs.clear();
    EXPECT_TRUE(m_tunnelMgr->doTunnelTask(tunnelConfig));
    EXPECT_TRUE(commandWasIssued("tunnel add tun0"));
    EXPECT_TRUE(commandWasIssued("link set dev tun0 up"));

    mockCallArgs.clear();
    EXPECT_TRUE(m_tunnelMgr->doTunnelRouteTask(
        {"192.0.2.1/32", SET_COMMAND, {}}));
    EXPECT_TRUE(commandWasIssued("route replace \"192.0.2.1/32\" dev tun0"));
}

} // namespace tunnelmgr_ut
