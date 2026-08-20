#include "gtest/gtest.h"
#include <string>
#include <vector>
#include <algorithm>
#include "schema.h"
#include "warm_restart.h"
#include "table.h"

#define private public
#include "vnetmgr.h"
#undef private

extern int (*callback)(const std::string &cmd, std::string &stdout);
extern std::vector<std::string> mockCallArgs;

namespace vnetmgr_ut
{

using namespace swss;

static bool g_fail_link_add = false;
static bool g_fail_link_set_vrf = false;
static bool g_fail_link_set_up = false;
static bool g_fail_route_add = false;
static bool g_fail_neigh_add = false;
static bool g_fail_link_show_dev = false;

static int vnet_cb(const std::string &cmd, std::string &stdout)
{
    mockCallArgs.push_back(cmd);
    if (cmd.find("link show type vxlan") != std::string::npos)
    {
        stdout = "";
        return 0;
    }
    if (cmd.find("-o link show dev ") != std::string::npos)
    {
        return g_fail_link_show_dev ? 1 : 0;
    }
    if (g_fail_link_add && cmd.find("link add ") != std::string::npos)
    {
        return 1;
    }
    if (g_fail_link_set_vrf && cmd.find("link set dev ") != std::string::npos
        && cmd.find(" vrf ") != std::string::npos)
    {
        return 1;
    }
    if (g_fail_link_set_up && cmd.find("link set dev ") != std::string::npos
        && cmd.find(" up") != std::string::npos
        && cmd.find(" vrf ") == std::string::npos)
    {
        return 1;
    }
    if (g_fail_route_add && cmd.find("route add ") != std::string::npos)
    {
        return 1;
    }
    if (g_fail_neigh_add && cmd.find("neigh add ") != std::string::npos)
    {
        return 1;
    }
    return 0;
}

static bool cmdWasIssued(const std::string &needle)
{
    for (const auto &c : mockCallArgs)
    {
        if (c.find(needle) != std::string::npos) return true;
    }
    return false;
}

static KeyOpFieldsValuesTuple makeTuple(const std::string &key,
                                        const std::string &op,
                                        const std::vector<FieldValueTuple> &fvs)
{
    return KeyOpFieldsValuesTuple{key, op, fvs};
}

struct VNetMgrTest : public ::testing::Test
{
    std::shared_ptr<swss::DBConnector> m_cfg_db;
    std::shared_ptr<swss::DBConnector> m_app_db;
    std::vector<std::string> m_tables;

    void SetUp() override
    {
        m_cfg_db = std::make_shared<swss::DBConnector>("CONFIG_DB", 0);
        m_app_db = std::make_shared<swss::DBConnector>("APPL_DB", 0);
        m_cfg_db->flushdb();
        m_app_db->flushdb();
        swss::WarmStart::initialize("vnetmgrd", "swss");
        m_tables = {};
        mockCallArgs.clear();
        callback = vnet_cb;
        g_fail_link_add = false;
        g_fail_link_set_vrf = false;
        g_fail_link_set_up = false;
        g_fail_route_add = false;
        g_fail_neigh_add = false;
        g_fail_link_show_dev = false;
    }

    void TearDown() override
    {
        callback = nullptr;
    }
};

static void addBaseTunnelAndVnet(VNetMgr &mgr,
                               const std::string &tunnel = "tunnel0",
                               const std::string &tunnelSrcIp = "10.0.0.1",
                               const std::string &vnet = "Vnet1",
                               const std::string &vni = "2000",
                               const std::string &srcMac = "aa:bb:cc:dd:ee:ff",
                               swss::DBConnector *appDb = nullptr)
{
    if (appDb != nullptr)
    {
        swss::Table t(appDb, APP_SWITCH_TABLE_NAME);
        t.set("switch", std::vector<swss::FieldValueTuple>{{"vxlan_port", "4789"}});
    }
    auto tun = KeyOpFieldsValuesTuple{tunnel, SET_COMMAND,
        {{"src_ip", tunnelSrcIp}}};
    mgr.doVxlanTunnelCreateTask(tun);
    auto v = KeyOpFieldsValuesTuple{vnet, SET_COMMAND,
        {{"vxlan_tunnel", tunnel}, {"vni", vni}, {"src_mac", srcMac}}};
    mgr.doVnetCreateTask(v);
}

TEST_F(VNetMgrTest, VxlanTunnelCreateThenDelete)
{
    VNetMgr mgr(m_cfg_db.get(), m_app_db.get(), m_tables);
    auto t = makeTuple("tunnel0", SET_COMMAND, {{"src_ip", "10.0.0.1"}});
    ASSERT_TRUE(mgr.doVxlanTunnelCreateTask(t));
    ASSERT_NE(mgr.m_vxlanTunnelCache.find("tunnel0"), mgr.m_vxlanTunnelCache.end());
    ASSERT_EQ(mgr.m_vxlanTunnelCache["tunnel0"].m_sourceIp, "10.0.0.1");
    auto d = makeTuple("tunnel0", DEL_COMMAND, {});
    ASSERT_TRUE(mgr.doVxlanTunnelDeleteTask(d));
    ASSERT_EQ(mgr.m_vxlanTunnelCache.find("tunnel0"), mgr.m_vxlanTunnelCache.end());
    ASSERT_TRUE(mgr.doVxlanTunnelDeleteTask(d));
}

TEST_F(VNetMgrTest, VnetCreateWaitsForTunnel)
{
    VNetMgr mgr(m_cfg_db.get(), m_app_db.get(), m_tables);
    auto t = makeTuple("Vnet1", SET_COMMAND,
                       {{"vxlan_tunnel", "tunnel0"}, {"vni", "2000"},
                        {"src_mac", "00:11:22:33:44:55"}});
    ASSERT_FALSE(mgr.doVnetCreateTask(t));
    ASSERT_EQ(mgr.m_vnetCache.find("Vnet1"), mgr.m_vnetCache.end());
}

TEST_F(VNetMgrTest, VnetCreateIncompleteFieldsAreIgnored)
{
    VNetMgr mgr(m_cfg_db.get(), m_app_db.get(), m_tables);
    auto t = makeTuple("Vnet1", SET_COMMAND, {{"vxlan_tunnel", "tunnel0"}});
    ASSERT_TRUE(mgr.doVnetCreateTask(t));
    ASSERT_EQ(mgr.m_vnetCache.find("Vnet1"), mgr.m_vnetCache.end());
    auto t2 = makeTuple("Vnet1", SET_COMMAND, {{"vni", "2000"}});
    ASSERT_TRUE(mgr.doVnetCreateTask(t2));
    ASSERT_EQ(mgr.m_vnetCache.find("Vnet1"), mgr.m_vnetCache.end());
}

TEST_F(VNetMgrTest, VnetCreate)
{
    VNetMgr mgr(m_cfg_db.get(), m_app_db.get(), m_tables);
    auto tun = makeTuple("tunnel0", SET_COMMAND, {{"src_ip", "10.0.0.1"}});
    ASSERT_TRUE(mgr.doVxlanTunnelCreateTask(tun));
    auto vnet = makeTuple("Vnet1", SET_COMMAND,
                          {{"vxlan_tunnel", "tunnel0"}, {"vni", "2000"},
                           {"src_mac", "aa:bb:cc:dd:ee:ff"}});
    ASSERT_TRUE(mgr.doVnetCreateTask(vnet));
    ASSERT_NE(mgr.m_vnetCache.find("Vnet1"), mgr.m_vnetCache.end());
    ASSERT_EQ(mgr.m_vnetCache["Vnet1"].m_sourceIp, "10.0.0.1");
    ASSERT_EQ(mgr.m_vnetCache["Vnet1"].m_vni, "2000");
    ASSERT_NE(mgr.m_vxlanNetDevices.find("Vxlan2000"), mgr.m_vxlanNetDevices.end());
    auto d = makeTuple("Vnet1", DEL_COMMAND, {});
    ASSERT_TRUE(mgr.doVnetDeleteTask(d));
    ASSERT_EQ(mgr.m_vnetCache.find("Vnet1"), mgr.m_vnetCache.end());
    ASSERT_EQ(mgr.m_vxlanNetDevices.find("Vxlan2000"), mgr.m_vxlanNetDevices.end());
    ASSERT_TRUE(mgr.doVnetDeleteTask(d));
}

TEST_F(VNetMgrTest, RouteTunnelCreateBeforeVnetCreation)
{
    VNetMgr mgr(m_cfg_db.get(), m_app_db.get(), m_tables);
    auto rt = makeTuple("Vnet1|20.0.0.0/24", SET_COMMAND,
                        {{"endpoint", "10.1.1.1"}, {"mac_address", "22:33:44:55:66:77"},
                         {"vni", "3000"}, {"install_on_kernel", "true"}});
    ASSERT_FALSE(mgr.doVnetRouteTunnelCreateTask(rt));
    ASSERT_EQ(mgr.m_kernelRouteTunnelCache.find("Vnet1|20.0.0.0/24"),
              mgr.m_kernelRouteTunnelCache.end());
}

TEST_F(VNetMgrTest, RouteTunnelVniMatchesUsesExistingVxlanmgrdNetdev)
{
    // vxlanmgrd owns the netdev when vnis match; existing netdev -> just add route.
    VNetMgr mgr(m_cfg_db.get(), m_app_db.get(), m_tables);
    addBaseTunnelAndVnet(mgr, "tunnel0", "10.0.0.1", "Vnet1", "2000", "aa:bb:cc:dd:ee:ff", m_app_db.get());
    mockCallArgs.clear();
    auto rt = makeTuple("Vnet1|20.0.0.0/24", SET_COMMAND,
                        {{"endpoint", "10.1.1.1"}, {"mac_address", "22:33:44:55:66:77"},
                         {"vni", "2000"}, {"install_on_kernel", "true"}});
    ASSERT_TRUE(mgr.doVnetRouteTunnelCreateTask(rt));
    ASSERT_NE(mgr.m_kernelRouteTunnelCache.find("Vnet1|20.0.0.0/24"),
              mgr.m_kernelRouteTunnelCache.end());
    ASSERT_TRUE(cmdWasIssued("-o link show dev \"Vxlan2000\""));
    ASSERT_FALSE(cmdWasIssued("link add \"Vxlan2000\""));
    ASSERT_FALSE(cmdWasIssued("link set dev \"Vxlan2000\" vrf \"Vnet1\""));
    ASSERT_FALSE(cmdWasIssued("link set dev \"Vxlan2000\" up"));
    ASSERT_TRUE(cmdWasIssued("route add "));
}

TEST_F(VNetMgrTest, RouteTunnelVniMatchesReturnsFalseWhenNetdevMissing)
{
    // vxlanmgrd has not created the netdev yet -> return false to retry.
    VNetMgr mgr(m_cfg_db.get(), m_app_db.get(), m_tables);
    addBaseTunnelAndVnet(mgr, "tunnel0", "10.0.0.1", "Vnet1", "2000", "aa:bb:cc:dd:ee:ff", m_app_db.get());
    g_fail_link_show_dev = true;
    mockCallArgs.clear();
    auto rt = makeTuple("Vnet1|20.0.0.0/24", SET_COMMAND,
                        {{"endpoint", "10.1.1.1"}, {"mac_address", "22:33:44:55:66:77"},
                         {"vni", "2000"}, {"install_on_kernel", "true"}});
    ASSERT_FALSE(mgr.doVnetRouteTunnelCreateTask(rt));
    ASSERT_EQ(mgr.m_kernelRouteTunnelCache.find("Vnet1|20.0.0.0/24"),
              mgr.m_kernelRouteTunnelCache.end());
    ASSERT_FALSE(cmdWasIssued("link add \"Vxlan2000\""));
    ASSERT_FALSE(cmdWasIssued("route add "));
}

TEST_F(VNetMgrTest, RouteTunnelCreateHappyPathIssuesIpCommands)
{
    VNetMgr mgr(m_cfg_db.get(), m_app_db.get(), m_tables);
    addBaseTunnelAndVnet(mgr, "tunnel0", "10.0.0.1", "Vnet1", "2000", "aa:bb:cc:dd:ee:ff", m_app_db.get());
    mockCallArgs.clear();
    auto rt = makeTuple("Vnet1|20.0.0.0/24", SET_COMMAND,
                        {{"endpoint", "10.1.1.1"}, {"mac_address", "22:33:44:55:66:77"},
                         {"vni", "3000"}, {"install_on_kernel", "true"}});
    ASSERT_TRUE(mgr.doVnetRouteTunnelCreateTask(rt));
    ASSERT_NE(mgr.m_kernelRouteTunnelCache.find("Vnet1|20.0.0.0/24"),
              mgr.m_kernelRouteTunnelCache.end());
    ASSERT_TRUE(cmdWasIssued("link add \"Vxlan3000\""));
    ASSERT_TRUE(cmdWasIssued("link set dev \"Vxlan3000\" vrf \"Vnet1\""));
    ASSERT_TRUE(cmdWasIssued("link set dev \"Vxlan3000\" up"));
    ASSERT_TRUE(cmdWasIssued("route add \"20.0.0.0/24\" dev \"Vxlan3000\" vrf \"Vnet1\""));
    ASSERT_FALSE(cmdWasIssued("neigh add"));
}

TEST_F(VNetMgrTest, RouteTunnelHostRouteAddsStaticNeigh)
{
    VNetMgr mgr(m_cfg_db.get(), m_app_db.get(), m_tables);
    addBaseTunnelAndVnet(mgr, "tunnel0", "10.0.0.1", "Vnet1", "2000", "aa:bb:cc:dd:ee:ff", m_app_db.get());
    mockCallArgs.clear();
    auto rt = makeTuple("Vnet1|20.0.0.5/32", SET_COMMAND,
                        {{"endpoint", "10.1.1.1"}, {"mac_address", "22:33:44:55:66:77"},
                         {"vni", "3000"}, {"install_on_kernel", "true"}});
    ASSERT_TRUE(mgr.doVnetRouteTunnelCreateTask(rt));
    ASSERT_TRUE(cmdWasIssued("neigh add \"20.0.0.5\" lladdr \"22:33:44:55:66:77\" dev \"Vxlan3000\""));
}

TEST_F(VNetMgrTest, RouteTunnelIpv6HostRouteAddsStaticNeigh)
{
    VNetMgr mgr(m_cfg_db.get(), m_app_db.get(), m_tables);
    addBaseTunnelAndVnet(mgr, "tunnel0", "10.0.0.1", "Vnet1", "2000", "aa:bb:cc:dd:ee:ff", m_app_db.get());
    mockCallArgs.clear();
    auto rt = makeTuple("Vnet1|2001:db8::1/128", SET_COMMAND,
                        {{"endpoint", "2001:db8:1::1"}, {"mac_address", "22:33:44:55:66:77"},
                         {"vni", "3000"}, {"install_on_kernel", "true"}});
    ASSERT_TRUE(mgr.doVnetRouteTunnelCreateTask(rt));
    ASSERT_TRUE(cmdWasIssued("route add \"2001:db8::1/128\""));
    ASSERT_TRUE(cmdWasIssued("neigh add \"2001:db8::1\""));
}

TEST_F(VNetMgrTest, RouteTunnelIpv6PrefixNoNeigh)
{
    VNetMgr mgr(m_cfg_db.get(), m_app_db.get(), m_tables);
    addBaseTunnelAndVnet(mgr, "tunnel0", "10.0.0.1", "Vnet1", "2000", "aa:bb:cc:dd:ee:ff", m_app_db.get());
    mockCallArgs.clear();
    auto rt = makeTuple("Vnet1|2001:db8::/64", SET_COMMAND,
                        {{"endpoint", "2001:db8:1::1"}, {"mac_address", "22:33:44:55:66:77"},
                         {"vni", "3000"}, {"install_on_kernel", "true"}});
    ASSERT_TRUE(mgr.doVnetRouteTunnelCreateTask(rt));
    ASSERT_TRUE(cmdWasIssued("route add \"2001:db8::/64\""));
    ASSERT_FALSE(cmdWasIssued("neigh add"));
}

TEST_F(VNetMgrTest, RouteTunnelInstallOnKernelFalseSkipsIpCommands)
{
    VNetMgr mgr(m_cfg_db.get(), m_app_db.get(), m_tables);
    addBaseTunnelAndVnet(mgr, "tunnel0", "10.0.0.1", "Vnet1", "2000", "aa:bb:cc:dd:ee:ff", m_app_db.get());
    mockCallArgs.clear();
    auto rt = makeTuple("Vnet1|20.0.0.0/24", SET_COMMAND,
                        {{"endpoint", "10.1.1.1"}, {"mac_address", "22:33:44:55:66:77"},
                         {"vni", "3000"}, {"install_on_kernel", "false"}});
    ASSERT_TRUE(mgr.doVnetRouteTunnelCreateTask(rt));
    ASSERT_FALSE(cmdWasIssued("link add"));
    ASSERT_FALSE(cmdWasIssued("route add"));
    ASSERT_NE(mgr.m_vnetRouteTunnelCache.find("Vnet1|20.0.0.0/24"),
              mgr.m_vnetRouteTunnelCache.end());
}

TEST_F(VNetMgrTest, RouteTunnelDeleteRemovesKernelRoute)
{
    VNetMgr mgr(m_cfg_db.get(), m_app_db.get(), m_tables);
    addBaseTunnelAndVnet(mgr, "tunnel0", "10.0.0.1", "Vnet1", "2000", "aa:bb:cc:dd:ee:ff", m_app_db.get());
    auto rt = makeTuple("Vnet1|20.0.0.0/24", SET_COMMAND,
                        {{"endpoint", "10.1.1.1"}, {"mac_address", "22:33:44:55:66:77"},
                         {"vni", "3000"}, {"install_on_kernel", "true"}});
    ASSERT_TRUE(mgr.doVnetRouteTunnelCreateTask(rt));
    mockCallArgs.clear();
    auto d = makeTuple("Vnet1|20.0.0.0/24", DEL_COMMAND, {});
    ASSERT_TRUE(mgr.doVnetRouteTunnelDeleteTask(d));
    ASSERT_TRUE(cmdWasIssued("link del \"Vxlan3000\""));
    ASSERT_EQ(mgr.m_vnetRouteTunnelCache.find("Vnet1|20.0.0.0/24"),
              mgr.m_vnetRouteTunnelCache.end());
    ASSERT_TRUE(mgr.doVnetRouteTunnelDeleteTask(d));
}

TEST_F(VNetMgrTest, ExecFailurePropagatesForEachIpCommand)
{
    struct Case
    {
        const char *name;
        bool *flag;
        const char *prefix;
    };
    const std::vector<Case> cases = {
        {"link add",         &g_fail_link_add,     "Vnet1|20.0.0.0/24"},
        {"link set vrf",     &g_fail_link_set_vrf, "Vnet1|20.0.0.0/24"},
        {"link set up",      &g_fail_link_set_up,  "Vnet1|20.0.0.0/24"},
        {"route add",        &g_fail_route_add,    "Vnet1|20.0.0.0/24"},
        {"neigh add",        &g_fail_neigh_add,    "Vnet1|20.0.0.5/32"},
    };
    for (const auto &c : cases)
    {
        SCOPED_TRACE(c.name);
        VNetMgr mgr(m_cfg_db.get(), m_app_db.get(), m_tables);
        addBaseTunnelAndVnet(mgr, "tunnel0", "10.0.0.1", "Vnet1", "2000", "aa:bb:cc:dd:ee:ff", m_app_db.get());
        *c.flag = true;
        auto rt = makeTuple(c.prefix, SET_COMMAND,
                            {{"endpoint", "10.1.1.1"}, {"mac_address", "22:33:44:55:66:77"},
                             {"vni", "3000"}, {"install_on_kernel", "true"}});
        ASSERT_FALSE(mgr.doVnetRouteTunnelCreateTask(rt));
        ASSERT_EQ(mgr.m_kernelRouteTunnelCache.find(c.prefix),
                  mgr.m_kernelRouteTunnelCache.end());
        *c.flag = false;
        m_cfg_db->flushdb();
        m_app_db->flushdb();
    }
}

TEST_F(VNetMgrTest, LocalVnetRouteSetAndDelete)
{
    VNetMgr mgr(m_cfg_db.get(), m_app_db.get(), m_tables);
    auto s = makeTuple("Vnet1|30.0.0.0/24", SET_COMMAND,
                       {{"nexthop", "1.2.3.4"}, {"ifname", "Ethernet0"}});
    ASSERT_TRUE(mgr.doVnetRouteTask(s, SET_COMMAND));
    auto d = makeTuple("Vnet1|30.0.0.0/24", DEL_COMMAND, {});
    ASSERT_TRUE(mgr.doVnetRouteTask(d, DEL_COMMAND));
    ASSERT_FALSE(mgr.doVnetRouteTask(s, "UNKNOWN"));
}

static void writeSwitchTable(swss::DBConnector *appDb,
                             const std::vector<swss::FieldValueTuple> &fvs)
{
    swss::Table t(appDb, APP_SWITCH_TABLE_NAME);
    t.set("switch", fvs);
}

TEST_F(VNetMgrTest, SwitchTableConfigFailsWhenSwitchKeyAbsent)
{
    // Absent SWITCH_TABLE|switch key must fail the load.
    VNetMgr mgr(m_cfg_db.get(), m_app_db.get(), m_tables);
    ASSERT_FALSE(mgr.getSwitchTableVxlanConfig());
    ASSERT_FALSE(mgr.m_VxlanSwitchTableConfig.m_loaded);
}

TEST_F(VNetMgrTest, SwitchTableConfigRejectsInvalidMask)
{
    VNetMgr mgr(m_cfg_db.get(), m_app_db.get(), m_tables);
    writeSwitchTable(m_app_db.get(),
                     {{"vxlan_sport", "20000"},
                      {"vxlan_mask", "99"}});
    ASSERT_TRUE(mgr.getSwitchTableVxlanConfig());
    ASSERT_TRUE(mgr.m_VxlanSwitchTableConfig.m_vxlanSrcPortRangeStart.empty());
}

TEST_F(VNetMgrTest, RouteTunnelUsesVxlanPortOverrideForDstport)
{
    // SWITCH_TABLE|switch.vxlan_port=6789 overrides the default
    VNetMgr mgr(m_cfg_db.get(), m_app_db.get(), m_tables);
    addBaseTunnelAndVnet(mgr, "tunnel0", "10.0.0.1", "Vnet1", "2000", "aa:bb:cc:dd:ee:ff", m_app_db.get());
    writeSwitchTable(m_app_db.get(),
                     {{"vxlan_port", "6789"}});
    mockCallArgs.clear();
    auto rt = makeTuple("Vnet1|30.0.0.0/24", SET_COMMAND,
                        {{"endpoint", "10.1.1.1"}, {"mac_address", "22:33:44:55:66:77"},
                         {"vni", "3000"}, {"install_on_kernel", "true"}});
    ASSERT_TRUE(mgr.doVnetRouteTunnelCreateTask(rt));
    ASSERT_TRUE(mgr.m_VxlanSwitchTableConfig.m_loaded);
    ASSERT_EQ(mgr.m_VxlanSwitchTableConfig.m_vxlanUdpPort, "6789");
    ASSERT_TRUE(cmdWasIssued("dstport \"6789\""));
}

TEST_F(VNetMgrTest, RouteTunnelDefaultsDstportTo4789)
{
    // vxlan_port omitted from SWITCH_TABLE|switch: config defaults to 4789
    VNetMgr mgr(m_cfg_db.get(), m_app_db.get(), m_tables);
    addBaseTunnelAndVnet(mgr, "tunnel0", "10.0.0.1", "Vnet1", "2000", "aa:bb:cc:dd:ee:ff", m_app_db.get());
    mockCallArgs.clear();
    auto rt = makeTuple("Vnet1|31.0.0.0/24", SET_COMMAND,
                        {{"endpoint", "10.1.1.2"}, {"mac_address", "22:33:44:55:66:78"},
                         {"vni", "3001"}, {"install_on_kernel", "true"}});
    ASSERT_TRUE(mgr.doVnetRouteTunnelCreateTask(rt));
    ASSERT_TRUE(mgr.m_VxlanSwitchTableConfig.m_loaded);
    ASSERT_EQ(mgr.m_VxlanSwitchTableConfig.m_vxlanUdpPort, "4789");
    ASSERT_TRUE(cmdWasIssued("dstport \"4789\""));
}


TEST_F(VNetMgrTest, RouteTunnelPassesSrcPortRangeToCmdCreateVxlan)
{
    // vxlan_sport=20000, vxlan_mask=8: [19968, 20223].
    VNetMgr mgr(m_cfg_db.get(), m_app_db.get(), m_tables);
    writeSwitchTable(m_app_db.get(),
                     {{"vxlan_sport", "20000"},
                      {"vxlan_mask", "8"}});
    addBaseTunnelAndVnet(mgr, "tunnel0", "10.0.0.1", "Vnet1", "2000", "aa:bb:cc:dd:ee:ff", m_app_db.get());
    mockCallArgs.clear();
    auto rt = makeTuple("Vnet1|32.0.0.0/24", SET_COMMAND,
                        {{"endpoint", "10.1.1.3"}, {"mac_address", "22:33:44:55:66:79"},
                         {"vni", "3002"}, {"install_on_kernel", "true"}});
    ASSERT_TRUE(mgr.doVnetRouteTunnelCreateTask(rt));
    ASSERT_TRUE(mgr.m_VxlanSwitchTableConfig.m_loaded);
    ASSERT_EQ(mgr.m_VxlanSwitchTableConfig.m_vxlanSrcPortRangeStart, "19968");
    ASSERT_EQ(mgr.m_VxlanSwitchTableConfig.m_vxlanSrcPortRangeEnd,   "20223");
    ASSERT_TRUE(cmdWasIssued("srcport \"19968\" \"20223\""));
}

}  // namespace vnetmgr_ut
