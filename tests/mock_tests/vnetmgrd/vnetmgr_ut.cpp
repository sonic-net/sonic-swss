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

static bool g_fail_link_show_dev = false;
static bool g_fail_route_add    = false;
static bool g_fail_neigh_add    = false;
static bool g_fail_fdb_add      = false;

static int vnet_cb(const std::string &cmd, std::string &stdout)
{
    mockCallArgs.push_back(cmd);
    if (cmd.find("-o link show dev ") != std::string::npos)
    {
        return g_fail_link_show_dev ? 1 : 0;
    }
    if (g_fail_route_add && cmd.find(" route add ") != std::string::npos)
    {
        return 1;
    }
    if (g_fail_neigh_add && cmd.find(" neigh add ") != std::string::npos)
    {
        return 1;
    }
    if (g_fail_fdb_add && cmd.find(" fdb append ") != std::string::npos)
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

static bool cmdHasTokens(const std::string &tokens)
{
    std::istringstream iss(tokens);
    std::vector<std::string> toks;
    std::string t;
    while (iss >> t) toks.push_back(t);
    for (const auto &c : mockCallArgs)
    {
        bool ok = true;
        for (const auto &tok : toks)
        {
            if (c.find(tok) == std::string::npos) { ok = false; break; }
        }
        if (ok) return true;
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
        g_fail_link_show_dev = false;
        g_fail_route_add = false;
        g_fail_neigh_add = false;
        g_fail_fdb_add = false;
    }

    void TearDown() override
    {
        callback = nullptr;
    }
};

static void createVnet(VNetMgr &mgr,
                       const std::string &vnet,
                       const std::string &vni,
                       const std::string &tunnel = "tunnel0")
{
    auto v = makeTuple(vnet, SET_COMMAND,
        {{"vxlan_tunnel", tunnel}, {"vni", vni}});
    ASSERT_TRUE(mgr.doVnetCreateTask(v));
}

TEST_F(VNetMgrTest, VnetCreateCachesVni)
{
    VNetMgr mgr(m_cfg_db.get(), m_app_db.get(), m_tables);
    createVnet(mgr, "Vnet1", "1000");
    ASSERT_NE(mgr.m_vnetCache.find("Vnet1"), mgr.m_vnetCache.end());
    ASSERT_EQ(mgr.m_vnetCache["Vnet1"].m_vni, "1000");
    ASSERT_EQ(mgr.m_vnetCache["Vnet1"].m_vxlanTunnel, "tunnel0");
}

TEST_F(VNetMgrTest, VnetCreateIncompleteFieldsAreIgnored)
{
    VNetMgr mgr(m_cfg_db.get(), m_app_db.get(), m_tables);
    auto v = makeTuple("Vnet1", SET_COMMAND, {{"vxlan_tunnel", "tunnel0"}});
    ASSERT_TRUE(mgr.doVnetCreateTask(v));
    ASSERT_EQ(mgr.m_vnetCache.find("Vnet1"), mgr.m_vnetCache.end());
}

TEST_F(VNetMgrTest, VnetDeleteRemovesCacheEntry)
{
    VNetMgr mgr(m_cfg_db.get(), m_app_db.get(), m_tables);
    createVnet(mgr, "Vnet1", "1000");
    auto d = makeTuple("Vnet1", DEL_COMMAND, {});
    ASSERT_TRUE(mgr.doVnetDeleteTask(d));
    ASSERT_EQ(mgr.m_vnetCache.find("Vnet1"), mgr.m_vnetCache.end());
}

TEST_F(VNetMgrTest, RouteTunnelBeforeVnetReturnsFalse)
{
    VNetMgr mgr(m_cfg_db.get(), m_app_db.get(), m_tables);
    auto r = makeTuple("Vnet1|192.168.1.1/32", SET_COMMAND,
        {{"endpoint", "10.0.0.2"}, {"mac_address", "02:00:00:00:00:01"},
         {"vni", "1000"}, {"install_on_kernel", "true"}});
    ASSERT_FALSE(mgr.doVnetRouteTunnelCreateTask(r));
}

TEST_F(VNetMgrTest, RouteTunnelDeferredUntilNetdevAppears)
{
    VNetMgr mgr(m_cfg_db.get(), m_app_db.get(), m_tables);
    createVnet(mgr, "Vnet1", "1000");
    g_fail_link_show_dev = true;

    auto r = makeTuple("Vnet1|192.168.1.1/32", SET_COMMAND,
        {{"endpoint", "10.0.0.2"}, {"mac_address", "02:00:00:00:00:01"},
         {"vni", "1000"}, {"install_on_kernel", "true"}});
    ASSERT_FALSE(mgr.doVnetRouteTunnelCreateTask(r));
    ASSERT_FALSE(cmdWasIssued(" route add "));
    ASSERT_FALSE(cmdWasIssued(" neigh add "));
    ASSERT_FALSE(cmdWasIssued(" fdb append "));

    g_fail_link_show_dev = false;
    mockCallArgs.clear();

    ASSERT_TRUE(mgr.doVnetRouteTunnelCreateTask(r));
    ASSERT_TRUE(cmdHasTokens("route add 192.168.1.1/32 Brvxlan1000 Vnet1"));
    ASSERT_TRUE(cmdHasTokens("neigh add 192.168.1.1 lladdr 02:00:00:00:00:01 Brvxlan1000"));
    ASSERT_TRUE(cmdHasTokens("fdb append 02:00:00:00:00:01 Vxlan1000 10.0.0.2"));
}

TEST_F(VNetMgrTest, RouteTunnelSameVni)
{
    VNetMgr mgr(m_cfg_db.get(), m_app_db.get(), m_tables);
    createVnet(mgr, "Vnet1", "1000");
    auto r = makeTuple("Vnet1|192.168.1.1/32", SET_COMMAND,
        {{"endpoint", "10.0.0.2"}, {"mac_address", "02:00:00:00:00:01"},
         {"vni", "1000"}, {"install_on_kernel", "true"}});
    ASSERT_TRUE(mgr.doVnetRouteTunnelCreateTask(r));
    ASSERT_TRUE(cmdHasTokens("route add 192.168.1.1/32 Brvxlan1000 Vnet1"));
    ASSERT_TRUE(cmdHasTokens("neigh add 192.168.1.1 lladdr 02:00:00:00:00:01 Brvxlan1000"));
    ASSERT_TRUE(cmdHasTokens("fdb append 02:00:00:00:00:01 Vxlan1000 10.0.0.2"));
    ASSERT_FALSE(cmdWasIssued(" vni 1000"));
}

TEST_F(VNetMgrTest, RouteTunnelCrossVniUsesFdbOverride)
{
    VNetMgr mgr(m_cfg_db.get(), m_app_db.get(), m_tables);
    createVnet(mgr, "Vnet1", "1000");
    auto r = makeTuple("Vnet1|192.168.2.1/32", SET_COMMAND,
        {{"endpoint", "10.0.0.3"}, {"mac_address", "02:00:00:00:00:02"},
         {"vni", "2000"}, {"install_on_kernel", "true"}});
    ASSERT_TRUE(mgr.doVnetRouteTunnelCreateTask(r));
    ASSERT_TRUE(cmdHasTokens("route add 192.168.2.1/32 Brvxlan1000 Vnet1"));
    ASSERT_TRUE(cmdHasTokens("fdb append 02:00:00:00:00:02 Vxlan1000 10.0.0.3 vni 2000"));
    ASSERT_FALSE(cmdHasTokens("Vxlan2000"));
}

TEST_F(VNetMgrTest, RouteTunnelIpv6HostRouteAddsNeigh)
{
    VNetMgr mgr(m_cfg_db.get(), m_app_db.get(), m_tables);
    createVnet(mgr, "Vnet1", "1000");
    auto r = makeTuple("Vnet1|2001:db8::1/128", SET_COMMAND,
        {{"endpoint", "10.0.0.2"}, {"mac_address", "02:00:00:00:00:01"},
         {"vni", "1000"}, {"install_on_kernel", "true"}});
    ASSERT_TRUE(mgr.doVnetRouteTunnelCreateTask(r));
    ASSERT_TRUE(cmdHasTokens("neigh add 2001:db8::1 lladdr 02:00:00:00:00:01 Brvxlan1000"));
}

TEST_F(VNetMgrTest, RouteTunnelNonHostPrefixSkipsKernelInstall)
{
    VNetMgr mgr(m_cfg_db.get(), m_app_db.get(), m_tables);
    createVnet(mgr, "Vnet1", "1000");
    auto r = makeTuple("Vnet1|192.168.1.0/24", SET_COMMAND,
        {{"endpoint", "10.0.0.2"}, {"mac_address", "02:00:00:00:00:01"},
         {"vni", "1000"}, {"install_on_kernel", "true"}});
    ASSERT_TRUE(mgr.doVnetRouteTunnelCreateTask(r));
    ASSERT_FALSE(cmdWasIssued(" route add "));
    ASSERT_FALSE(cmdWasIssued(" neigh add "));
    ASSERT_FALSE(cmdWasIssued(" fdb append "));
}

TEST_F(VNetMgrTest, RouteTunnelInstallOnKernelFalseSkipsIpCommands)
{
    VNetMgr mgr(m_cfg_db.get(), m_app_db.get(), m_tables);
    createVnet(mgr, "Vnet1", "1000");
    auto r = makeTuple("Vnet1|192.168.1.1/32", SET_COMMAND,
        {{"endpoint", "10.0.0.2"}, {"mac_address", "02:00:00:00:00:01"},
         {"vni", "1000"}, {"install_on_kernel", "false"}});
    ASSERT_TRUE(mgr.doVnetRouteTunnelCreateTask(r));
    ASSERT_FALSE(cmdWasIssued(" route add "));
    ASSERT_FALSE(cmdWasIssued(" neigh add "));
    ASSERT_FALSE(cmdWasIssued(" fdb append "));
}

TEST_F(VNetMgrTest, RouteTunnelDeleteRemovesKernelState)
{
    VNetMgr mgr(m_cfg_db.get(), m_app_db.get(), m_tables);
    createVnet(mgr, "Vnet1", "1000");
    auto r = makeTuple("Vnet1|192.168.1.1/32", SET_COMMAND,
        {{"endpoint", "10.0.0.2"}, {"mac_address", "02:00:00:00:00:01"},
         {"vni", "1000"}, {"install_on_kernel", "true"}});
    ASSERT_TRUE(mgr.doVnetRouteTunnelCreateTask(r));
    mockCallArgs.clear();

    auto d = makeTuple("Vnet1|192.168.1.1/32", DEL_COMMAND, {});
    ASSERT_TRUE(mgr.doVnetRouteTunnelDeleteTask(d));
    ASSERT_TRUE(cmdHasTokens("route del 192.168.1.1/32 Brvxlan1000 Vnet1"));
    ASSERT_TRUE(cmdHasTokens("neigh del 192.168.1.1 Brvxlan1000"));
    ASSERT_TRUE(cmdHasTokens("fdb del 02:00:00:00:00:01 Vxlan1000 10.0.0.2"));
}

TEST_F(VNetMgrTest, RouteTunnelFdbFailureRollsBackNeighAndRoute)
{
    VNetMgr mgr(m_cfg_db.get(), m_app_db.get(), m_tables);
    createVnet(mgr, "Vnet1", "1000");
    g_fail_fdb_add = true;
    auto r = makeTuple("Vnet1|192.168.1.1/32", SET_COMMAND,
        {{"endpoint", "10.0.0.2"}, {"mac_address", "02:00:00:00:00:01"},
         {"vni", "1000"}, {"install_on_kernel", "true"}});
    ASSERT_FALSE(mgr.doVnetRouteTunnelCreateTask(r));
    ASSERT_TRUE(cmdWasIssued(" neigh del "));
    ASSERT_TRUE(cmdWasIssued(" route del "));
}

TEST_F(VNetMgrTest, RouteTunnelNeighFailureRollsBackRoute)
{
    VNetMgr mgr(m_cfg_db.get(), m_app_db.get(), m_tables);
    createVnet(mgr, "Vnet1", "1000");
    g_fail_neigh_add = true;
    auto r = makeTuple("Vnet1|192.168.1.1/32", SET_COMMAND,
        {{"endpoint", "10.0.0.2"}, {"mac_address", "02:00:00:00:00:01"},
         {"vni", "1000"}, {"install_on_kernel", "true"}});
    ASSERT_FALSE(mgr.doVnetRouteTunnelCreateTask(r));
    ASSERT_TRUE(cmdWasIssued(" route del "));
}

TEST_F(VNetMgrTest, LocalVnetRouteDoesNotHitKernel)
{
    VNetMgr mgr(m_cfg_db.get(), m_app_db.get(), m_tables);
    createVnet(mgr, "Vnet1", "1000");
    auto r = makeTuple("Vnet1|192.168.10.0/24", SET_COMMAND,
        {{"nexthop", "192.168.10.254"}});
    ASSERT_TRUE(mgr.doVnetRouteTask(r, SET_COMMAND));
    auto d = makeTuple("Vnet1|192.168.10.0/24", DEL_COMMAND, {});
    ASSERT_TRUE(mgr.doVnetRouteTask(d, DEL_COMMAND));
    ASSERT_FALSE(cmdWasIssued(" route add "));
    ASSERT_FALSE(cmdWasIssued(" neigh add "));
}

} // namespace vnetmgr_ut
