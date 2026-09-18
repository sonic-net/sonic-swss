#include "gtest/gtest.h"
#include <string>
#include <vector>
#include <algorithm>
#include <deque>
#include <sstream>
#include "schema.h"
#include "warm_restart.h"
#include "table.h"
#include "consumerstatetable.h"

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
    if (g_fail_route_add && cmd.find(" route replace ") != std::string::npos)
    {
        return 1;
    }
    if (g_fail_neigh_add && cmd.find(" neigh replace ") != std::string::npos)
    {
        return 1;
    }
    if (g_fail_fdb_add && cmd.find(" fdb replace ") != std::string::npos)
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
        swss::Table(m_app_db.get(), APP_SWITCH_TABLE_NAME).set(
            "switch", {{"vxlan_router_mac", "00:00:5e:00:53:01"}});
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
    ASSERT_FALSE(cmdWasIssued(" route replace "));
    ASSERT_FALSE(cmdWasIssued(" neigh replace "));
    ASSERT_FALSE(cmdWasIssued(" fdb replace "));

    g_fail_link_show_dev = false;
    mockCallArgs.clear();

    ASSERT_TRUE(mgr.doVnetRouteTunnelCreateTask(r));
    ASSERT_TRUE(cmdHasTokens("route replace 192.168.1.1/32 via 10.0.0.2 Brvxlan1000 Vnet1 onlink"));
    ASSERT_TRUE(cmdHasTokens("neigh replace 10.0.0.2 lladdr 02:00:00:00:00:01 Brvxlan1000 nud permanent"));
    ASSERT_TRUE(cmdHasTokens("fdb replace 02:00:00:00:00:01 Vxlan1000 10.0.0.2"));
}

TEST_F(VNetMgrTest, RouteTunnelSameVni)
{
    VNetMgr mgr(m_cfg_db.get(), m_app_db.get(), m_tables);
    createVnet(mgr, "Vnet1", "1000");
    auto r = makeTuple("Vnet1|192.168.1.1/32", SET_COMMAND,
        {{"endpoint", "10.0.0.2"}, {"mac_address", "02:00:00:00:00:01"},
         {"vni", "1000"}, {"install_on_kernel", "true"}});
    ASSERT_TRUE(mgr.doVnetRouteTunnelCreateTask(r));
    ASSERT_TRUE(cmdHasTokens("route replace 192.168.1.1/32 via 10.0.0.2 Brvxlan1000 Vnet1 onlink"));
    ASSERT_TRUE(cmdHasTokens("neigh replace 10.0.0.2 lladdr 02:00:00:00:00:01 Brvxlan1000"));
    ASSERT_TRUE(cmdHasTokens("fdb replace 02:00:00:00:00:01 Vxlan1000 10.0.0.2"));
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
    ASSERT_TRUE(cmdHasTokens("route replace 192.168.2.1/32 via 10.0.0.3 Brvxlan1000 Vnet1 onlink"));
    ASSERT_TRUE(cmdHasTokens("fdb replace 02:00:00:00:00:02 Vxlan1000 10.0.0.3 vni 2000"));
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
    // Neigh keyed on endpoint (v4 underlay), not the inner v6 prefix.
    ASSERT_TRUE(cmdHasTokens("neigh replace 10.0.0.2 lladdr 02:00:00:00:00:01 Brvxlan1000"));
    ASSERT_TRUE(cmdHasTokens("route replace 2001:db8::1/128 via 10.0.0.2 Brvxlan1000 Vnet1 onlink"));
}

TEST_F(VNetMgrTest, RouteTunnelPrefixShapesInstallIdentically)
{
    VNetMgr mgr(m_cfg_db.get(), m_app_db.get(), m_tables);
    createVnet(mgr, "Vnet1", "1000");

    auto subnet = makeTuple("Vnet1|192.168.5.0/24", SET_COMMAND,
        {{"endpoint", "10.0.0.2"}, {"mac_address", "02:00:00:00:00:01"},
         {"vni", "1000"}, {"install_on_kernel", "true"}});
    ASSERT_TRUE(mgr.doVnetRouteTunnelCreateTask(subnet));
    ASSERT_TRUE(cmdHasTokens("route replace 192.168.5.0/24 via 10.0.0.2 Brvxlan1000 Vnet1 onlink"));
    ASSERT_TRUE(cmdHasTokens("neigh replace 10.0.0.2 lladdr 02:00:00:00:00:01 Brvxlan1000 nud permanent"));
    ASSERT_TRUE(cmdHasTokens("fdb replace 02:00:00:00:00:01 Vxlan1000 10.0.0.2"));

    mockCallArgs.clear();
    auto def = makeTuple("Vnet1|0.0.0.0/0", SET_COMMAND,
        {{"endpoint", "10.0.0.9"}, {"mac_address", "02:00:00:00:00:aa"},
         {"vni", "1000"}, {"install_on_kernel", "true"}});
    ASSERT_TRUE(mgr.doVnetRouteTunnelCreateTask(def));
    ASSERT_TRUE(cmdHasTokens("route replace 0.0.0.0/0 via 10.0.0.9 Brvxlan1000 Vnet1 onlink"));
    ASSERT_TRUE(cmdHasTokens("neigh replace 10.0.0.9 lladdr 02:00:00:00:00:aa Brvxlan1000 nud permanent"));
    ASSERT_TRUE(cmdHasTokens("fdb replace 02:00:00:00:00:aa Vxlan1000 10.0.0.9"));
}

TEST_F(VNetMgrTest, RouteTunnelInstallOnKernelFalseSkipsIpCommands)
{
    VNetMgr mgr(m_cfg_db.get(), m_app_db.get(), m_tables);
    createVnet(mgr, "Vnet1", "1000");
    auto r = makeTuple("Vnet1|192.168.1.1/32", SET_COMMAND,
        {{"endpoint", "10.0.0.2"}, {"mac_address", "02:00:00:00:00:01"},
         {"vni", "1000"}, {"install_on_kernel", "false"}});
    ASSERT_TRUE(mgr.doVnetRouteTunnelCreateTask(r));
    ASSERT_FALSE(cmdWasIssued(" route replace "));
    ASSERT_FALSE(cmdWasIssued(" neigh replace "));
    ASSERT_FALSE(cmdWasIssued(" fdb replace "));
}

TEST_F(VNetMgrTest, RouteTunnelSharedMacLifecycle)
{
    VNetMgr mgr(m_cfg_db.get(), m_app_db.get(), m_tables);
    createVnet(mgr, "Vnet1", "1000");

    auto r1 = makeTuple("Vnet1|192.168.5.0/24", SET_COMMAND,
        {{"endpoint", "10.0.0.2"}, {"mac_address", "02:00:00:00:00:01"},
         {"vni", "1000"}, {"install_on_kernel", "true"}});
    auto r2 = makeTuple("Vnet1|192.168.6.0/24", SET_COMMAND,
        {{"endpoint", "10.0.0.2"}, {"mac_address", "02:00:00:00:00:01"},
         {"vni", "1000"}, {"install_on_kernel", "true"}});
    ASSERT_TRUE(mgr.doVnetRouteTunnelCreateTask(r1));
    mockCallArgs.clear();

    ASSERT_TRUE(mgr.doVnetRouteTunnelCreateTask(r2));
    ASSERT_TRUE(cmdHasTokens("route replace 192.168.6.0/24 via 10.0.0.2 Brvxlan1000 Vnet1 onlink"));
    ASSERT_TRUE(cmdHasTokens("neigh replace 10.0.0.2 lladdr 02:00:00:00:00:01 Brvxlan1000"));
    ASSERT_TRUE(cmdHasTokens("fdb replace 02:00:00:00:00:01 Vxlan1000 10.0.0.2"));
    ASSERT_EQ(mgr.m_macRefs["Vnet1|02:00:00:00:00:01"].count, 2);
    ASSERT_EQ(mgr.m_macRefs["Vnet1|02:00:00:00:00:01"].endpoint, "10.0.0.2");

    // Deleting the first prefix must NOT tear down neigh/FDB — r2 still uses them.
    mockCallArgs.clear();
    ASSERT_TRUE(mgr.doVnetRouteTunnelDeleteTask(makeTuple("Vnet1|192.168.5.0/24", DEL_COMMAND, {})));
    ASSERT_TRUE(cmdHasTokens("route del 192.168.5.0/24 Brvxlan1000 Vnet1"));
    ASSERT_FALSE(cmdWasIssued(" neigh del "));
    ASSERT_FALSE(cmdWasIssued(" fdb del "));
    ASSERT_EQ(mgr.m_macRefs["Vnet1|02:00:00:00:00:01"].count, 1);

    mockCallArgs.clear();
    ASSERT_TRUE(mgr.doVnetRouteTunnelDeleteTask(makeTuple("Vnet1|192.168.6.0/24", DEL_COMMAND, {})));
    ASSERT_TRUE(cmdHasTokens("route del 192.168.6.0/24 Brvxlan1000 Vnet1"));
    ASSERT_TRUE(cmdHasTokens("neigh del 10.0.0.2 Brvxlan1000"));
    ASSERT_TRUE(cmdHasTokens("fdb del 02:00:00:00:00:01 Vxlan1000 10.0.0.2"));
    ASSERT_EQ(mgr.m_macRefs.count("Vnet1|02:00:00:00:00:01"), 0u);
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
    ASSERT_TRUE(cmdHasTokens("neigh del 10.0.0.2 Brvxlan1000"));
    ASSERT_TRUE(cmdHasTokens("fdb del 02:00:00:00:00:01 Vxlan1000 10.0.0.2"));
}

TEST_F(VNetMgrTest, RouteTunnelFdbFailureRollsBackNeigh)
{
    VNetMgr mgr(m_cfg_db.get(), m_app_db.get(), m_tables);
    createVnet(mgr, "Vnet1", "1000");
    g_fail_fdb_add = true;
    auto r = makeTuple("Vnet1|192.168.1.1/32", SET_COMMAND,
        {{"endpoint", "10.0.0.2"}, {"mac_address", "02:00:00:00:00:01"},
         {"vni", "1000"}, {"install_on_kernel", "true"}});
    ASSERT_FALSE(mgr.doVnetRouteTunnelCreateTask(r));
    ASSERT_TRUE(cmdWasIssued(" neigh del "));
    // route replace must NOT have been attempted (neigh/FDB set up before route now).
    ASSERT_FALSE(cmdWasIssued(" route replace "));
}

TEST_F(VNetMgrTest, RouteTunnelRouteAddFailureRollsBackSharedState)
{
    VNetMgr mgr(m_cfg_db.get(), m_app_db.get(), m_tables);
    createVnet(mgr, "Vnet1", "1000");
    g_fail_route_add = true;
    auto r = makeTuple("Vnet1|192.168.1.1/32", SET_COMMAND,
        {{"endpoint", "10.0.0.2"}, {"mac_address", "02:00:00:00:00:01"},
         {"vni", "1000"}, {"install_on_kernel", "true"}});
    ASSERT_FALSE(mgr.doVnetRouteTunnelCreateTask(r));
    // Freshly-installed neigh+FDB must be torn down because no prefix survived.
    ASSERT_TRUE(cmdWasIssued(" fdb del "));
    ASSERT_TRUE(cmdWasIssued(" neigh del "));
}

TEST_F(VNetMgrTest, RouteTunnelNeighFailureLeavesNoState)
{
    VNetMgr mgr(m_cfg_db.get(), m_app_db.get(), m_tables);
    createVnet(mgr, "Vnet1", "1000");
    g_fail_neigh_add = true;
    auto r = makeTuple("Vnet1|192.168.1.1/32", SET_COMMAND,
        {{"endpoint", "10.0.0.2"}, {"mac_address", "02:00:00:00:00:01"},
         {"vni", "1000"}, {"install_on_kernel", "true"}});
    ASSERT_FALSE(mgr.doVnetRouteTunnelCreateTask(r));
    // Neigh failed first -> route replace must not have been attempted, no FDB either.
    ASSERT_FALSE(cmdWasIssued(" route replace "));
    ASSERT_FALSE(cmdWasIssued(" fdb replace "));
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
    ASSERT_FALSE(cmdWasIssued(" route replace "));
    ASSERT_FALSE(cmdWasIssued(" neigh add "));
}


static bool popAppDbEntry(swss::DBConnector *db, const std::string &tableName,
                          const std::string &key, std::string &opOut,
                          std::vector<swss::FieldValueTuple> &fvsOut)
{
    swss::ConsumerStateTable consumer(db, tableName);
    std::deque<swss::KeyOpFieldsValuesTuple> entries;
    consumer.pops(entries);
    for (const auto &e : entries)
    {
        if (kfvKey(e) == key)
        {
            opOut = kfvOp(e);
            fvsOut = kfvFieldsValues(e);
            return true;
        }
    }
    return false;
}

TEST_F(VNetMgrTest, RouteTunnelPublishedToAppDbNStripsInstallOnKernel)
{
    VNetMgr mgr(m_cfg_db.get(), m_app_db.get(), m_tables);
    createVnet(mgr, "Vnet1", "1000");
    auto r = makeTuple("Vnet1|192.168.1.1/32", SET_COMMAND,
        {{"endpoint", "10.0.0.2"}, {"mac_address", "02:00:00:00:00:01"},
         {"vni", "1000"}, {"install_on_kernel", "true"}});
    ASSERT_TRUE(mgr.doVnetRouteTunnelCreateTask(r));

    std::string op;
    std::vector<swss::FieldValueTuple> fvs;
    ASSERT_TRUE(popAppDbEntry(m_app_db.get(), APP_VNET_RT_TUNNEL_TABLE_NAME,
                              "Vnet1:192.168.1.1/32", op, fvs));
    ASSERT_EQ(op, SET_COMMAND);
    bool hasInstallOnKernel = false, endpointOk = false, vniOk = false, macOk = false;
    for (const auto &fv : fvs)
    {
        if (fvField(fv) == "install_on_kernel") hasInstallOnKernel = true;
        if (fvField(fv) == "endpoint" && fvValue(fv) == "10.0.0.2") endpointOk = true;
        if (fvField(fv) == "vni" && fvValue(fv) == "1000") vniOk = true;
        if (fvField(fv) == "mac_address" && fvValue(fv) == "02:00:00:00:00:01") macOk = true;
    }
    ASSERT_FALSE(hasInstallOnKernel);
    ASSERT_TRUE(endpointOk);
    ASSERT_TRUE(vniOk);
    ASSERT_TRUE(macOk);
}

TEST_F(VNetMgrTest, RouteTunnelDeletePublishesDelToAppDb)
{
    VNetMgr mgr(m_cfg_db.get(), m_app_db.get(), m_tables);
    createVnet(mgr, "Vnet1", "1000");
    swss::ConsumerStateTable consumer(m_app_db.get(), APP_VNET_RT_TUNNEL_TABLE_NAME);

    auto r = makeTuple("Vnet1|192.168.1.1/32", SET_COMMAND,
        {{"endpoint", "10.0.0.2"}, {"mac_address", "02:00:00:00:00:01"},
         {"vni", "1000"}, {"install_on_kernel", "true"}});
    ASSERT_TRUE(mgr.doVnetRouteTunnelCreateTask(r));
    auto d = makeTuple("Vnet1|192.168.1.1/32", DEL_COMMAND, {});
    ASSERT_TRUE(mgr.doVnetRouteTunnelDeleteTask(d));

    std::deque<swss::KeyOpFieldsValuesTuple> entries;
    consumer.pops(entries);
    std::string lastOp;
    for (const auto &e : entries)
    {
        if (kfvKey(e) == "Vnet1:192.168.1.1/32") lastOp = kfvOp(e);
    }
    ASSERT_EQ(lastOp, DEL_COMMAND);
}

TEST_F(VNetMgrTest, LocalVnetRoutePublishedToAppDbAndDeleted)
{
    VNetMgr mgr(m_cfg_db.get(), m_app_db.get(), m_tables);
    createVnet(mgr, "Vnet1", "1000");
    auto r = makeTuple("Vnet1|192.168.10.0/24", SET_COMMAND,
        {{"nexthop", "192.168.10.254"}});
    ASSERT_TRUE(mgr.doVnetRouteTask(r, SET_COMMAND));

    std::string op;
    std::vector<swss::FieldValueTuple> fvs;
    ASSERT_TRUE(popAppDbEntry(m_app_db.get(), APP_VNET_RT_TABLE_NAME,
                              "Vnet1:192.168.10.0/24", op, fvs));
    ASSERT_EQ(op, SET_COMMAND);
    bool nexthopOk = false;
    for (const auto &fv : fvs)
    {
        if (fvField(fv) == "nexthop" && fvValue(fv) == "192.168.10.254") nexthopOk = true;
    }
    ASSERT_TRUE(nexthopOk);

    auto d = makeTuple("Vnet1|192.168.10.0/24", DEL_COMMAND, {});
    ASSERT_TRUE(mgr.doVnetRouteTask(d, DEL_COMMAND));
    ASSERT_TRUE(popAppDbEntry(m_app_db.get(), APP_VNET_RT_TABLE_NAME,
                              "Vnet1:192.168.10.0/24", op, fvs));
    ASSERT_EQ(op, DEL_COMMAND);
}

TEST_F(VNetMgrTest, RouteTunnelMalformedPrefix)
{
    VNetMgr mgr(m_cfg_db.get(), m_app_db.get(), m_tables);
    createVnet(mgr, "Vnet1", "1000");
    auto r = makeTuple("Vnet1|192.168.1.1/abc", SET_COMMAND,
        {{"endpoint", "10.0.0.2"}, {"mac_address", "02:00:00:00:00:01"},
         {"vni", "1000"}, {"install_on_kernel", "true"}});
    ASSERT_TRUE(mgr.doVnetRouteTunnelCreateTask(r));

    std::string op;
    std::vector<swss::FieldValueTuple> fvs;
    ASSERT_TRUE(popAppDbEntry(m_app_db.get(), APP_VNET_RT_TUNNEL_TABLE_NAME,
                              "Vnet1:192.168.1.1/abc", op, fvs));
    ASSERT_EQ(op, SET_COMMAND);
    bool endpointOk = false;
    for (const auto &fv : fvs)
    {
        if (fvField(fv) == "endpoint" && fvValue(fv) == "10.0.0.2") endpointOk = true;
    }
    ASSERT_TRUE(endpointOk);

    auto d = makeTuple("Vnet1|192.168.1.1/abc", DEL_COMMAND, {});
    ASSERT_TRUE(mgr.doVnetRouteTunnelDeleteTask(d));
    ASSERT_TRUE(popAppDbEntry(m_app_db.get(), APP_VNET_RT_TUNNEL_TABLE_NAME,
                              "Vnet1:192.168.1.1/abc", op, fvs));
    ASSERT_EQ(op, DEL_COMMAND);
}


TEST_F(VNetMgrTest, RouteTunnelSinglePrefixEndpointChangeDeletesAndRecreates)
{
    VNetMgr mgr(m_cfg_db.get(), m_app_db.get(), m_tables);
    createVnet(mgr, "Vnet1", "1000");

    auto r1 = makeTuple("Vnet1|192.168.5.0/24", SET_COMMAND,
        {{"endpoint", "10.0.0.2"}, {"mac_address", "02:00:00:00:00:01"},
         {"vni", "1000"}, {"install_on_kernel", "true"}});
    ASSERT_TRUE(mgr.doVnetRouteTunnelCreateTask(r1));
    mockCallArgs.clear();

    auto r1_moved = makeTuple("Vnet1|192.168.5.0/24", SET_COMMAND,
        {{"endpoint", "10.0.0.3"}, {"mac_address", "02:00:00:00:00:01"},
         {"vni", "1000"}, {"install_on_kernel", "true"}});
    ASSERT_TRUE(mgr.doVnetRouteTunnelCreateTask(r1_moved));

    // Tear-down at OLD_EP.
    ASSERT_TRUE(cmdHasTokens("route del 192.168.5.0/24 Brvxlan1000 Vnet1"));
    ASSERT_TRUE(cmdHasTokens("neigh del 10.0.0.2 Brvxlan1000"));
    ASSERT_TRUE(cmdHasTokens("fdb del 02:00:00:00:00:01 Vxlan1000 10.0.0.2"));
    // Reinstall at NEW_EP.
    ASSERT_TRUE(cmdHasTokens("neigh replace 10.0.0.3 lladdr 02:00:00:00:00:01 Brvxlan1000"));
    ASSERT_TRUE(cmdHasTokens("fdb replace 02:00:00:00:00:01 Vxlan1000 10.0.0.3"));
    ASSERT_TRUE(cmdHasTokens("route replace 192.168.5.0/24 via 10.0.0.3 Brvxlan1000 Vnet1 onlink"));

    ASSERT_EQ(mgr.m_macRefs["Vnet1|02:00:00:00:00:01"].count, 1);
    ASSERT_EQ(mgr.m_macRefs["Vnet1|02:00:00:00:00:01"].endpoint, "10.0.0.3");
    ASSERT_EQ(mgr.m_kernelRouteTunnelCache["Vnet1|192.168.5.0/24"].m_dstIp, "10.0.0.3");
}

TEST_F(VNetMgrTest, RouteTunnelLatestWinsAcrossCollisions)
{
    VNetMgr mgr(m_cfg_db.get(), m_app_db.get(), m_tables);
    createVnet(mgr, "Vnet1", "1000");

    auto p1 = makeTuple("Vnet1|192.168.5.0/24", SET_COMMAND,
        {{"endpoint", "10.0.0.2"}, {"mac_address", "02:00:00:00:00:01"},
         {"vni", "1000"}, {"install_on_kernel", "true"}});
    ASSERT_TRUE(mgr.doVnetRouteTunnelCreateTask(p1));

    // Fresh prefix, same endpoint, different MAC: neigh (keyed by IP) is clobbered
    // to the new MAC; a fresh FDB entry is installed for the new MAC.
    mockCallArgs.clear();
    auto p2 = makeTuple("Vnet1|192.168.6.0/24", SET_COMMAND,
        {{"endpoint", "10.0.0.2"}, {"mac_address", "02:ff:ff:ff:ff:ff"},
         {"vni", "1000"}, {"install_on_kernel", "true"}});
    ASSERT_TRUE(mgr.doVnetRouteTunnelCreateTask(p2));
    ASSERT_TRUE(cmdHasTokens("neigh replace 10.0.0.2 lladdr 02:ff:ff:ff:ff:ff Brvxlan1000"));
    ASSERT_TRUE(cmdHasTokens("fdb replace 02:ff:ff:ff:ff:ff Vxlan1000 10.0.0.2"));

    // Fresh prefix, same MAC as p1, different endpoint: shared FDB moves to new EP.
    mockCallArgs.clear();
    auto p3 = makeTuple("Vnet1|192.168.7.0/24", SET_COMMAND,
        {{"endpoint", "10.0.0.9"}, {"mac_address", "02:00:00:00:00:01"},
         {"vni", "1000"}, {"install_on_kernel", "true"}});
    ASSERT_TRUE(mgr.doVnetRouteTunnelCreateTask(p3));
    ASSERT_TRUE(cmdHasTokens("neigh replace 10.0.0.9 lladdr 02:00:00:00:00:01 Brvxlan1000"));
    ASSERT_TRUE(cmdHasTokens("fdb replace 02:00:00:00:00:01 Vxlan1000 10.0.0.9"));
    ASSERT_TRUE(cmdHasTokens("route replace 192.168.7.0/24 via 10.0.0.9 Brvxlan1000 Vnet1 onlink"));
    ASSERT_EQ(mgr.m_macRefs["Vnet1|02:00:00:00:00:01"].count, 2);
    ASSERT_EQ(mgr.m_macRefs["Vnet1|02:00:00:00:00:01"].endpoint, "10.0.0.9");

    // Update p1 with a new endpoint (same MAC): old state torn down, new installed.
    mockCallArgs.clear();
    auto p1_new = makeTuple("Vnet1|192.168.5.0/24", SET_COMMAND,
        {{"endpoint", "10.0.0.5"}, {"mac_address", "02:00:00:00:00:01"},
         {"vni", "1000"}, {"install_on_kernel", "true"}});
    ASSERT_TRUE(mgr.doVnetRouteTunnelCreateTask(p1_new));
    ASSERT_TRUE(cmdHasTokens("route del 192.168.5.0/24 Brvxlan1000 Vnet1"));
    ASSERT_TRUE(cmdHasTokens("neigh del 10.0.0.2 Brvxlan1000"));
    ASSERT_TRUE(cmdHasTokens("fdb del 02:00:00:00:00:01 Vxlan1000 10.0.0.2"));
    ASSERT_TRUE(cmdHasTokens("route replace 192.168.5.0/24 via 10.0.0.5 Brvxlan1000 Vnet1 onlink"));
    ASSERT_EQ(mgr.m_macRefs["Vnet1|02:00:00:00:00:01"].count, 2);
    ASSERT_EQ(mgr.m_macRefs["Vnet1|02:00:00:00:00:01"].endpoint, "10.0.0.5");
    ASSERT_EQ(mgr.m_kernelRouteTunnelCache["Vnet1|192.168.5.0/24"].m_dstIp, "10.0.0.5");
}

TEST_F(VNetMgrTest, RouteTunnelMacChangeOnExistingPrefixTearsDownAndRecreates)
{
    VNetMgr mgr(m_cfg_db.get(), m_app_db.get(), m_tables);
    createVnet(mgr, "Vnet1", "1000");

    auto r = makeTuple("Vnet1|192.168.5.0/24", SET_COMMAND,
        {{"endpoint", "10.0.0.2"}, {"mac_address", "02:00:00:00:00:01"},
         {"vni", "1000"}, {"install_on_kernel", "true"}});
    ASSERT_TRUE(mgr.doVnetRouteTunnelCreateTask(r));
    mockCallArgs.clear();

    auto r_mac_swap = makeTuple("Vnet1|192.168.5.0/24", SET_COMMAND,
        {{"endpoint", "10.0.0.3"}, {"mac_address", "02:00:00:00:00:aa"},
         {"vni", "1000"}, {"install_on_kernel", "true"}});
    ASSERT_TRUE(mgr.doVnetRouteTunnelCreateTask(r_mac_swap));

    // Old MAC torn down.
    ASSERT_TRUE(cmdHasTokens("neigh del 10.0.0.2 Brvxlan1000"));
    ASSERT_TRUE(cmdHasTokens("fdb del 02:00:00:00:00:01 Vxlan1000 10.0.0.2"));
    // New MAC installed at new endpoint.
    ASSERT_TRUE(cmdHasTokens("neigh replace 10.0.0.3 lladdr 02:00:00:00:00:aa Brvxlan1000"));
    ASSERT_TRUE(cmdHasTokens("fdb replace 02:00:00:00:00:aa Vxlan1000 10.0.0.3"));
    ASSERT_TRUE(cmdHasTokens("route replace 192.168.5.0/24 via 10.0.0.3 Brvxlan1000 Vnet1 onlink"));

    ASSERT_EQ(mgr.m_macRefs.count("Vnet1|02:00:00:00:00:01"), 0u);
    ASSERT_EQ(mgr.m_macRefs["Vnet1|02:00:00:00:00:aa"].count, 1);
    ASSERT_EQ(mgr.m_macRefs["Vnet1|02:00:00:00:00:aa"].endpoint, "10.0.0.3");
}

TEST_F(VNetMgrTest, RouteTunnelDeleteAfterMigrationTearsDownAtCurrentEndpoint)
{
    VNetMgr mgr(m_cfg_db.get(), m_app_db.get(), m_tables);
    createVnet(mgr, "Vnet1", "1000");

    auto r = makeTuple("Vnet1|192.168.5.0/24", SET_COMMAND,
        {{"endpoint", "10.0.0.2"}, {"mac_address", "02:00:00:00:00:01"},
         {"vni", "1000"}, {"install_on_kernel", "true"}});
    ASSERT_TRUE(mgr.doVnetRouteTunnelCreateTask(r));

    auto r_moved = makeTuple("Vnet1|192.168.5.0/24", SET_COMMAND,
        {{"endpoint", "10.0.0.9"}, {"mac_address", "02:00:00:00:00:01"},
         {"vni", "1000"}, {"install_on_kernel", "true"}});
    ASSERT_TRUE(mgr.doVnetRouteTunnelCreateTask(r_moved));
    mockCallArgs.clear();

    auto d = makeTuple("Vnet1|192.168.5.0/24", DEL_COMMAND, {});
    ASSERT_TRUE(mgr.doVnetRouteTunnelDeleteTask(d));
    ASSERT_TRUE(cmdHasTokens("neigh del 10.0.0.9 Brvxlan1000"));
    ASSERT_TRUE(cmdHasTokens("fdb del 02:00:00:00:00:01 Vxlan1000 10.0.0.9"));
    ASSERT_FALSE(cmdHasTokens("fdb del 02:00:00:00:00:01 Vxlan1000 10.0.0.2"));
    ASSERT_EQ(mgr.m_macRefs.count("Vnet1|02:00:00:00:00:01"), 0u);
}

static void enableAcceptAllInnerDmacs(swss::DBConnector *appDb, bool enable, const std::string & routerMac = "aa:bb:cc:dd:ee:ff")
{
    swss::Table t(appDb, APP_SWITCH_TABLE_NAME);
    t.set("switch", {
        {"vxlan_accept_all_inner_dmacs", enable ? "true" : "false"},
        {"vxlan_router_mac", routerMac}});
}

TEST_F(VNetMgrTest, DmacBypassNotInstalledWhenSwitchAttrUnset)
{
    VNetMgr mgr(m_cfg_db.get(), m_app_db.get(), m_tables);
    createVnet(mgr, "Vnet1", "1000");
    auto r = makeTuple("Vnet1|192.168.1.1/32", SET_COMMAND,
        {{"endpoint", "10.0.0.2"}, {"mac_address", "02:00:00:00:00:01"},
         {"vni", "1000"}, {"install_on_kernel", "true"}});
    ASSERT_TRUE(mgr.doVnetRouteTunnelCreateTask(r));
    ASSERT_FALSE(cmdWasIssued("tc qdisc replace dev Vxlan1000 clsact"));
    ASSERT_FALSE(cmdWasIssued("tc filter replace dev Vxlan1000"));
    ASSERT_EQ(mgr.m_dmacBypassInstalledVnis.count("1000"), 0u);
}

TEST_F(VNetMgrTest, RouteDefersWhenSwitchTableMissing)
{
    swss::Table(m_app_db.get(), APP_SWITCH_TABLE_NAME).del("switch");
    VNetMgr mgr(m_cfg_db.get(), m_app_db.get(), m_tables);
    createVnet(mgr, "Vnet1", "1000");

    auto r = makeTuple("Vnet1|192.168.1.1/32", SET_COMMAND,
        {{"endpoint", "10.0.0.2"}, {"mac_address", "02:00:00:00:00:01"},
         {"vni", "1000"}, {"install_on_kernel", "true"}});
    ASSERT_FALSE(mgr.doVnetRouteTunnelCreateTask(r));
    ASSERT_FALSE(cmdWasIssued(" route replace "));
    ASSERT_FALSE(cmdWasIssued(" neigh replace "));
    ASSERT_FALSE(cmdWasIssued(" fdb replace "));
    ASSERT_FALSE(cmdWasIssued("tc qdisc replace"));
}

TEST_F(VNetMgrTest, DmacBypassInstalledOncePerVniWhenSwitchAttrTrue)
{
    enableAcceptAllInnerDmacs(m_app_db.get(), true);
    VNetMgr mgr(m_cfg_db.get(), m_app_db.get(), m_tables);
    createVnet(mgr, "Vnet1", "1000");

    auto r1 = makeTuple("Vnet1|192.168.1.1/32", SET_COMMAND,
        {{"endpoint", "10.0.0.2"}, {"mac_address", "02:00:00:00:00:01"},
         {"vni", "1000"}, {"install_on_kernel", "true"}});
    ASSERT_TRUE(mgr.doVnetRouteTunnelCreateTask(r1));
    ASSERT_TRUE(cmdHasTokens("tc qdisc replace dev Vxlan1000 clsact"));
    ASSERT_TRUE(cmdHasTokens("tc filter replace dev Vxlan1000 ingress matchall action pedit ex munge eth dst set aa:bb:cc:dd:ee:ff"));
    ASSERT_EQ(mgr.m_dmacBypassInstalledVnis.count("1000"), 1u);

    mockCallArgs.clear();
    auto r2 = makeTuple("Vnet1|192.168.2.2/32", SET_COMMAND,
        {{"endpoint", "10.0.0.3"}, {"mac_address", "02:00:00:00:00:02"},
         {"vni", "1000"}, {"install_on_kernel", "true"}});
    ASSERT_TRUE(mgr.doVnetRouteTunnelCreateTask(r2));
    ASSERT_FALSE(cmdWasIssued("tc qdisc replace"));
    ASSERT_FALSE(cmdWasIssued("tc filter replace"));
}

TEST_F(VNetMgrTest, DmacBypassDefersRouteWhenSwitchMacMissing)
{
    enableAcceptAllInnerDmacs(m_app_db.get(), true, "");
    VNetMgr mgr(m_cfg_db.get(), m_app_db.get(), m_tables);
    createVnet(mgr, "Vnet1", "1000");

    auto r = makeTuple("Vnet1|192.168.1.1/32", SET_COMMAND,
        {{"endpoint", "10.0.0.2"}, {"mac_address", "02:00:00:00:00:01"},
         {"vni", "1000"}, {"install_on_kernel", "true"}});
    ASSERT_FALSE(mgr.doVnetRouteTunnelCreateTask(r));
    ASSERT_FALSE(cmdWasIssued("tc qdisc replace"));
    ASSERT_FALSE(cmdWasIssued(" route replace "));
    ASSERT_FALSE(cmdWasIssued(" neigh replace "));
    ASSERT_FALSE(cmdWasIssued(" fdb replace "));
    ASSERT_EQ(mgr.m_dmacBypassInstalledVnis.count("1000"), 0u);

    enableAcceptAllInnerDmacs(m_app_db.get(), true, "aa:bb:cc:dd:ee:ff");
    mockCallArgs.clear();
    ASSERT_TRUE(mgr.doVnetRouteTunnelCreateTask(r));
    ASSERT_TRUE(cmdHasTokens("tc filter replace dev Vxlan1000 ingress matchall action pedit ex munge eth dst set aa:bb:cc:dd:ee:ff"));
    ASSERT_TRUE(cmdWasIssued(" route replace "));
    ASSERT_EQ(mgr.m_dmacBypassInstalledVnis.count("1000"), 1u);
}

TEST_F(VNetMgrTest, DmacBypassRemovedOnVnetDelete)
{
    enableAcceptAllInnerDmacs(m_app_db.get(), true);
    VNetMgr mgr(m_cfg_db.get(), m_app_db.get(), m_tables);
    createVnet(mgr, "Vnet1", "1000");
    auto r = makeTuple("Vnet1|192.168.1.1/32", SET_COMMAND,
        {{"endpoint", "10.0.0.2"}, {"mac_address", "02:00:00:00:00:01"},
         {"vni", "1000"}, {"install_on_kernel", "true"}});
    ASSERT_TRUE(mgr.doVnetRouteTunnelCreateTask(r));
    ASSERT_EQ(mgr.m_dmacBypassInstalledVnis.count("1000"), 1u);
    mockCallArgs.clear();

    auto d = makeTuple("Vnet1", DEL_COMMAND, {});
    ASSERT_TRUE(mgr.doVnetDeleteTask(d));
    ASSERT_TRUE(cmdHasTokens("tc qdisc del dev Vxlan1000 clsact"));
    ASSERT_EQ(mgr.m_dmacBypassInstalledVnis.count("1000"), 0u);
}

TEST_F(VNetMgrTest, DmacBypassDeleteSkippedWhenNeverInstalled)
{
    VNetMgr mgr(m_cfg_db.get(), m_app_db.get(), m_tables);
    createVnet(mgr, "Vnet1", "1000");
    mockCallArgs.clear();

    auto d = makeTuple("Vnet1", DEL_COMMAND, {});
    ASSERT_TRUE(mgr.doVnetDeleteTask(d));
    ASSERT_FALSE(cmdWasIssued("tc qdisc del"));
}

} // namespace vnetmgr_ut
