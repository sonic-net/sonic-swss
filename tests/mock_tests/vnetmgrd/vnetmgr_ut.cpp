#include "gtest/gtest.h"
#include <string>
#include <vector>
#include <map>
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

static std::map<std::string, int> g_links;
static std::string g_txDetails;
static int g_nextIfindex = 100;
static bool g_fail_route_add = false;
static bool g_fail_neigh_add = false;
static bool g_fail_nft = false;
static std::string g_routeShowV4;
static std::string g_neighShow;
static std::string g_tcFilterShow;

static std::string unquote(const std::string &s)
{
    std::string r;
    for (char c : s) if (c != '"' && c != '\'') r += c;
    return r;
}

static std::string devFromShow(const std::string &cmd)
{
    auto pos = cmd.find("link show dev ");
    std::string dev = cmd.substr(pos + strlen("link show dev "));
    return dev.substr(0, dev.find(' '));
}

static int vnet_cb(const std::string &rawCmd, std::string &stdout)
{
    mockCallArgs.push_back(rawCmd);
    const std::string cmd = unquote(rawCmd);
    stdout.clear();
    if (cmd.find("link show dev ") != std::string::npos)
    {
        std::string dev = devFromShow(cmd);
        auto it = g_links.find(dev);
        if (it == g_links.end()) return 1;
        stdout = std::to_string(it->second) + ": " + dev + ": <UP> mtu 1450";
        if (dev == VNET_VXLAN_TX_DEV) stdout += " " + g_txDetails;
        stdout += "\n";
        return 0;
    }
    if (cmd.find("link add " VNET_VXLAN_TX_DEV " ") != std::string::npos)
    {
        g_links[VNET_VXLAN_TX_DEV] = g_nextIfindex++;
        std::string params = cmd.substr(cmd.find("type vxlan"));
        g_txDetails = params.substr(0, params.find(" &&")) + " ";
        if (g_txDetails.find(" srcport ") == std::string::npos) g_txDetails += "srcport 0 0 ";
        return 0;
    }
    if (cmd.find("link del dev " VNET_VXLAN_TX_DEV) != std::string::npos)
    {
        g_links.erase(VNET_VXLAN_TX_DEV);
        return 0;
    }
    if (g_fail_nft && cmd.find("nft ") != std::string::npos && cmd.find("chain out") != std::string::npos)
    {
        return 1;
    }
    if (g_fail_route_add && cmd.find(" route replace ") != std::string::npos) return 1;
    if (g_fail_neigh_add && cmd.find(" neigh replace ") != std::string::npos) return 1;
    if (cmd.find("-o route show vrf") != std::string::npos)
    {
        if (cmd.find(" -4 ") != std::string::npos) stdout = g_routeShowV4;
        return 0;
    }
    if (cmd.find("neigh show dev") != std::string::npos)
    {
        stdout = g_neighShow;
        return 0;
    }
    if (cmd.find("filter show dev") != std::string::npos)
    {
        stdout = g_tcFilterShow;
        return 0;
    }
    return 0;
}

static bool issued(const std::string &needle)
{
    for (const auto &c : mockCallArgs)
    {
        if (unquote(c).find(needle) != std::string::npos) return true;
    }
    return false;
}

static int issuedCount(const std::string &needle)
{
    int n = 0;
    for (const auto &c : mockCallArgs)
    {
        if (unquote(c).find(needle) != std::string::npos) n++;
    }
    return n;
}

static KeyOpFieldsValuesTuple makeTuple(const std::string &key,
                                        const std::string &op,
                                        const std::vector<FieldValueTuple> &fvs)
{
    return KeyOpFieldsValuesTuple{key, op, fvs};
}

static KeyOpFieldsValuesTuple route(const std::string &key, const std::string &ep,
                                    const std::string &mac, const std::string &vni = "",
                                    const std::string &kernel = "true")
{
    std::vector<FieldValueTuple> fvs = {{"endpoint", ep}};
    if (!mac.empty()) fvs.push_back({"mac_address", mac});
    if (!vni.empty()) fvs.push_back({"vni", vni});
    fvs.push_back({"install_on_kernel", kernel});
    return makeTuple(key, SET_COMMAND, fvs);
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
        g_links = {{"Vxlan1000", 10}, {"Brvxlan1000", 11}, {"Vxlan2000", 12}, {"Brvxlan2000", 13}};
        g_txDetails.clear();
        g_nextIfindex = 100;
        g_fail_route_add = false;
        g_fail_neigh_add = false;
        g_fail_nft = false;
        g_routeShowV4.clear();
        g_neighShow.clear();
        g_tcFilterShow.clear();
        swss::Table(m_app_db.get(), APP_SWITCH_TABLE_NAME).set(
            "switch", {{"vxlan_router_mac", "00:00:5e:00:53:01"}});
        swss::Table(m_cfg_db.get(), CFG_VXLAN_TUNNEL_TABLE_NAME).set(
            "tunnel0", {{"src_ip", "10.1.0.1"}});
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

static const char *R1 = "route replace 192.168.1.1/32 encap ip id 1000 dst 10.0.0.2 src 10.1.0.1 "
                        "via inet6 fe80::200:0:1 dev VxlanOutbSvd vrf Vnet1 onlink proto redirect";
static const char *N1 = "-6 neigh replace fe80::200:0:1 lladdr 02:00:00:00:00:01 dev VxlanOutbSvd nud permanent";

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

TEST_F(VNetMgrTest, RouteTunnelBeforeVnetReturnsFalse)
{
    VNetMgr mgr(m_cfg_db.get(), m_app_db.get(), m_tables);
    ASSERT_FALSE(mgr.doVnetRouteTunnelCreateTask(route("Vnet1|192.168.1.1/32", "10.0.0.2", "02:00:00:00:00:01")));
}

TEST_F(VNetMgrTest, RouteDeferredUntilVtepSrcIpKnown)
{
    swss::Table(m_cfg_db.get(), CFG_VXLAN_TUNNEL_TABLE_NAME).del("tunnel0");
    VNetMgr mgr(m_cfg_db.get(), m_app_db.get(), m_tables);
    createVnet(mgr, "Vnet1", "1000");
    auto r = route("Vnet1|192.168.1.1/32", "10.0.0.2", "02:00:00:00:00:01", "1000");
    ASSERT_FALSE(mgr.doVnetRouteTunnelCreateTask(r));
    ASSERT_FALSE(issued(" route replace "));
    ASSERT_FALSE(issued("link add"));

    swss::Table(m_cfg_db.get(), CFG_VXLAN_TUNNEL_TABLE_NAME).set("tunnel0", {{"src_ip", "10.1.0.1"}});
    ASSERT_TRUE(mgr.doVnetRouteTunnelCreateTask(r));
    ASSERT_TRUE(issued(R1));
}

TEST_F(VNetMgrTest, FirstRouteCreatesTxDeviceNftNeighAndRoute)
{
    VNetMgr mgr(m_cfg_db.get(), m_app_db.get(), m_tables);
    createVnet(mgr, "Vnet1", "1000");
    ASSERT_TRUE(mgr.doVnetRouteTunnelCreateTask(route("Vnet1|192.168.1.1/32", "10.0.0.2", "02:00:00:00:00:01", "1000")));

    ASSERT_TRUE(issued("link add VxlanOutbSvd type vxlan external dstport 4790 local 10.1.0.1 nolearning"));
    ASSERT_TRUE(issued("link set dev VxlanOutbSvd up"));
    ASSERT_TRUE(issued("table inet sonic_vnet_vxtx {}; delete table inet sonic_vnet_vxtx; table inet sonic_vnet_vxtx {"));
    ASSERT_TRUE(issued("type filter hook output priority 0; policy accept; ip saddr 10.1.0.1 udp dport 4790 udp dport set 4789;"));
    ASSERT_TRUE(issued("type filter hook input priority 0; policy accept; udp dport 4790 drop;"));
    ASSERT_TRUE(issued(N1));
    ASSERT_TRUE(issued(R1));
    ASSERT_FALSE(issued(" fdb "));
    ASSERT_FALSE(issued("Brvxlan"));
    ASSERT_TRUE(mgr.m_txReady);
    ASSERT_TRUE(mgr.m_txReady);
    ASSERT_EQ(mgr.m_neighRefs["02:00:00:00:00:01"], 1);
}

TEST_F(VNetMgrTest, TxDeviceCreatedOnlyOnce)
{
    VNetMgr mgr(m_cfg_db.get(), m_app_db.get(), m_tables);
    createVnet(mgr, "Vnet1", "1000");
    ASSERT_TRUE(mgr.doVnetRouteTunnelCreateTask(route("Vnet1|192.168.1.1/32", "10.0.0.2", "02:00:00:00:00:01")));
    mockCallArgs.clear();
    ASSERT_TRUE(mgr.doVnetRouteTunnelCreateTask(route("Vnet1|192.168.2.0/24", "10.0.0.3", "02:00:00:00:00:02")));
    ASSERT_FALSE(issued("link add"));
    ASSERT_FALSE(issued("nft "));
}

TEST_F(VNetMgrTest, TxDevicePortFollowsSwitchVxlanPort)
{
    swss::Table(m_app_db.get(), APP_SWITCH_TABLE_NAME).set("switch", {{"vxlan_port", "13330"}});
    VNetMgr mgr(m_cfg_db.get(), m_app_db.get(), m_tables);
    createVnet(mgr, "Vnet1", "1000");
    ASSERT_TRUE(mgr.doVnetRouteTunnelCreateTask(route("Vnet1|192.168.1.1/32", "10.0.0.2", "02:00:00:00:00:01")));
    ASSERT_TRUE(issued("type vxlan external dstport 13331 local 10.1.0.1"));
    ASSERT_TRUE(issued("udp dport 13331 udp dport set 13330;"));
}

TEST_F(VNetMgrTest, TxDeviceSrcPortRangeFollowsSwitchSportMask)
{
    swss::Table(m_app_db.get(), APP_SWITCH_TABLE_NAME).set("switch", {{"vxlan_sport", "32775"}, {"vxlan_mask", "4"}});
    VNetMgr mgr(m_cfg_db.get(), m_app_db.get(), m_tables);
    createVnet(mgr, "Vnet1", "1000");
    ASSERT_TRUE(mgr.doVnetRouteTunnelCreateTask(route("Vnet1|192.168.1.1/32", "10.0.0.2", "02:00:00:00:00:01")));
    ASSERT_TRUE(issued("type vxlan external dstport 4790 srcport 32768 32784 local 10.1.0.1"));
}

TEST_F(VNetMgrTest, TxDeviceSrcPortMaskZeroPinsSinglePort)
{
    swss::Table(m_app_db.get(), APP_SWITCH_TABLE_NAME).set("switch", {{"vxlan_sport", "50000"}, {"vxlan_mask", "0"}});
    VNetMgr mgr(m_cfg_db.get(), m_app_db.get(), m_tables);
    createVnet(mgr, "Vnet1", "1000");
    ASSERT_TRUE(mgr.doVnetRouteTunnelCreateTask(route("Vnet1|192.168.1.1/32", "10.0.0.2", "02:00:00:00:00:01")));
    ASSERT_TRUE(issued("dstport 4790 srcport 50000 50001 local"));
}

TEST_F(VNetMgrTest, TxDeviceInvalidSrcPortConfigUsesKernelDefault)
{
    swss::Table(m_app_db.get(), APP_SWITCH_TABLE_NAME).set("switch", {{"vxlan_sport", "32768"}, {"vxlan_mask", "17"}});
    VNetMgr mgr(m_cfg_db.get(), m_app_db.get(), m_tables);
    createVnet(mgr, "Vnet1", "1000");
    ASSERT_TRUE(mgr.doVnetRouteTunnelCreateTask(route("Vnet1|192.168.1.1/32", "10.0.0.2", "02:00:00:00:00:01")));
    ASSERT_TRUE(issued("type vxlan external dstport 4790 local 10.1.0.1"));
    ASSERT_FALSE(issued("srcport"));
}

TEST_F(VNetMgrTest, ExistingTxDeviceWithStaleSrcPortRangeIsRecreated)
{
    g_links[VNET_VXLAN_TX_DEV] = 55;
    g_txDetails = "vxlan external id 0 local 10.1.0.1 srcport 0 0 dstport 4790 nolearning ";
    swss::Table(m_app_db.get(), APP_SWITCH_TABLE_NAME).set("switch", {{"vxlan_sport", "32768"}, {"vxlan_mask", "4"}});
    VNetMgr mgr(m_cfg_db.get(), m_app_db.get(), m_tables);
    createVnet(mgr, "Vnet1", "1000");
    ASSERT_TRUE(mgr.doVnetRouteTunnelCreateTask(route("Vnet1|192.168.1.1/32", "10.0.0.2", "02:00:00:00:00:01")));
    ASSERT_TRUE(issued("link del dev VxlanOutbSvd"));
    ASSERT_TRUE(issued("dstport 4790 srcport 32768 32784 local 10.1.0.1"));
}

TEST_F(VNetMgrTest, RouteVniDefaultsToVnetVni)
{
    VNetMgr mgr(m_cfg_db.get(), m_app_db.get(), m_tables);
    createVnet(mgr, "Vnet1", "1000");
    ASSERT_TRUE(mgr.doVnetRouteTunnelCreateTask(route("Vnet1|192.168.1.1/32", "10.0.0.2", "02:00:00:00:00:01")));
    ASSERT_TRUE(issued(R1));
}

TEST_F(VNetMgrTest, RouteCrossVniCarriedInEncap)
{
    VNetMgr mgr(m_cfg_db.get(), m_app_db.get(), m_tables);
    createVnet(mgr, "Vnet1", "1000");
    ASSERT_TRUE(mgr.doVnetRouteTunnelCreateTask(route("Vnet1|192.168.2.1/32", "10.0.0.3", "02:00:00:00:00:02", "2000")));
    ASSERT_TRUE(issued("route replace 192.168.2.1/32 encap ip id 2000 dst 10.0.0.3 src 10.1.0.1 via inet6 fe80::200:0:2 dev VxlanOutbSvd vrf Vnet1 onlink"));
    ASSERT_FALSE(issued("Vxlan2000"));
}

TEST_F(VNetMgrTest, SameEndpointDifferentMacsGetOwnNexthops)
{
    VNetMgr mgr(m_cfg_db.get(), m_app_db.get(), m_tables);
    createVnet(mgr, "Vnet1", "1000");
    ASSERT_TRUE(mgr.doVnetRouteTunnelCreateTask(route("Vnet1|192.168.5.0/24", "10.0.0.2", "aa:00:00:00:00:01")));
    ASSERT_TRUE(mgr.doVnetRouteTunnelCreateTask(route("Vnet1|192.168.6.0/24", "10.0.0.2", "BB:00:00:00:00:02")));
    ASSERT_TRUE(issued("-6 neigh replace fe80::aa00:0:1 lladdr aa:00:00:00:00:01 dev VxlanOutbSvd"));
    ASSERT_TRUE(issued("-6 neigh replace fe80::bb00:0:2 lladdr bb:00:00:00:00:02 dev VxlanOutbSvd"));
    ASSERT_TRUE(issued("route replace 192.168.5.0/24 encap ip id 1000 dst 10.0.0.2 src 10.1.0.1 via inet6 fe80::aa00:0:1"));
    ASSERT_TRUE(issued("route replace 192.168.6.0/24 encap ip id 1000 dst 10.0.0.2 src 10.1.0.1 via inet6 fe80::bb00:0:2"));
}

TEST_F(VNetMgrTest, SameMacDifferentEndpointAndVniShareOneNeigh)
{
    VNetMgr mgr(m_cfg_db.get(), m_app_db.get(), m_tables);
    createVnet(mgr, "Vnet1", "1000");
    createVnet(mgr, "Vnet2", "2000");
    ASSERT_TRUE(mgr.doVnetRouteTunnelCreateTask(route("Vnet1|192.168.5.0/24", "10.0.0.2", "02:00:00:00:00:01")));
    ASSERT_TRUE(mgr.doVnetRouteTunnelCreateTask(route("Vnet1|192.168.6.0/24", "10.0.0.3", "02:00:00:00:00:01", "3000")));
    ASSERT_TRUE(mgr.doVnetRouteTunnelCreateTask(route("Vnet2|192.168.5.0/24", "10.0.0.4", "02:00:00:00:00:01")));
    ASSERT_EQ(issuedCount(" neigh replace "), 1);
    ASSERT_EQ(mgr.m_neighRefs["02:00:00:00:00:01"], 3);
    ASSERT_TRUE(issued("route replace 192.168.5.0/24 encap ip id 1000 dst 10.0.0.2 src 10.1.0.1 via inet6 fe80::200:0:1 dev VxlanOutbSvd vrf Vnet1"));
    ASSERT_TRUE(issued("route replace 192.168.6.0/24 encap ip id 3000 dst 10.0.0.3 src 10.1.0.1 via inet6 fe80::200:0:1 dev VxlanOutbSvd vrf Vnet1"));
    ASSERT_TRUE(issued("route replace 192.168.5.0/24 encap ip id 2000 dst 10.0.0.4 src 10.1.0.1 via inet6 fe80::200:0:1 dev VxlanOutbSvd vrf Vnet2"));
}

TEST_F(VNetMgrTest, Ipv6PrefixAndIpv6Endpoint)
{
    VNetMgr mgr(m_cfg_db.get(), m_app_db.get(), m_tables);
    createVnet(mgr, "Vnet1", "1000");
    ASSERT_TRUE(mgr.doVnetRouteTunnelCreateTask(route("Vnet1|2001:db8::1/128", "10.0.0.2", "02:00:00:00:00:01")));
    ASSERT_TRUE(issued("route replace 2001:db8::1/128 encap ip id 1000 dst 10.0.0.2 src 10.1.0.1 via inet6 fe80::200:0:1 dev VxlanOutbSvd vrf Vnet1 onlink"));
    ASSERT_TRUE(mgr.doVnetRouteTunnelCreateTask(route("Vnet1|2001:db8::2/128", "fc00::2", "02:00:00:00:00:01")));
    ASSERT_TRUE(issued("route replace 2001:db8::2/128 encap ip6 id 1000 dst fc00::2"));
}

TEST_F(VNetMgrTest, InstallOnKernelFalseSkipsKernel)
{
    VNetMgr mgr(m_cfg_db.get(), m_app_db.get(), m_tables);
    createVnet(mgr, "Vnet1", "1000");
    ASSERT_TRUE(mgr.doVnetRouteTunnelCreateTask(route("Vnet1|192.168.1.1/32", "10.0.0.2", "02:00:00:00:00:01", "1000", "false")));
    ASSERT_FALSE(issued(" route replace "));
    ASSERT_FALSE(issued(" neigh replace "));
    ASSERT_FALSE(issued("link add"));
}

TEST_F(VNetMgrTest, MissingMacFallsBackToRouterMac)
{
    VNetMgr mgr(m_cfg_db.get(), m_app_db.get(), m_tables);
    createVnet(mgr, "Vnet1", "1000");
    ASSERT_TRUE(mgr.doVnetRouteTunnelCreateTask(route("Vnet1|192.168.1.1/32", "10.0.0.2", "")));
    ASSERT_TRUE(issued("-6 neigh replace fe80::5e00:5301 lladdr 00:00:5e:00:53:01"));
    ASSERT_TRUE(issued("via inet6 fe80::5e00:5301 dev VxlanOutbSvd"));
}

TEST_F(VNetMgrTest, InvalidMacIsNotInstalled)
{
    VNetMgr mgr(m_cfg_db.get(), m_app_db.get(), m_tables);
    createVnet(mgr, "Vnet1", "1000");
    ASSERT_TRUE(mgr.doVnetRouteTunnelCreateTask(route("Vnet1|192.168.1.1/32", "10.0.0.2", "zz:00")));
    ASSERT_FALSE(issued(" route replace "));
    ASSERT_TRUE(mgr.m_kernelRouteTunnelCache.empty());
}

TEST_F(VNetMgrTest, SharedMacNeighRefcounted)
{
    VNetMgr mgr(m_cfg_db.get(), m_app_db.get(), m_tables);
    createVnet(mgr, "Vnet1", "1000");
    ASSERT_TRUE(mgr.doVnetRouteTunnelCreateTask(route("Vnet1|192.168.5.0/24", "10.0.0.2", "02:00:00:00:00:01")));
    ASSERT_TRUE(mgr.doVnetRouteTunnelCreateTask(route("Vnet1|192.168.6.0/24", "10.0.0.2", "02:00:00:00:00:01")));

    mockCallArgs.clear();
    ASSERT_TRUE(mgr.doVnetRouteTunnelDeleteTask(makeTuple("Vnet1|192.168.5.0/24", DEL_COMMAND, {})));
    ASSERT_TRUE(issued("route del 192.168.5.0/24 dev VxlanOutbSvd vrf Vnet1"));
    ASSERT_FALSE(issued(" neigh del "));
    ASSERT_EQ(mgr.m_neighRefs["02:00:00:00:00:01"], 1);

    mockCallArgs.clear();
    ASSERT_TRUE(mgr.doVnetRouteTunnelDeleteTask(makeTuple("Vnet1|192.168.6.0/24", DEL_COMMAND, {})));
    ASSERT_TRUE(issued("route del 192.168.6.0/24 dev VxlanOutbSvd vrf Vnet1"));
    ASSERT_TRUE(issued("-6 neigh del fe80::200:0:1 dev VxlanOutbSvd"));
    ASSERT_EQ(mgr.m_neighRefs.count("02:00:00:00:00:01"), 0u);
}

TEST_F(VNetMgrTest, EndpointChangeIsHitlessReplace)
{
    VNetMgr mgr(m_cfg_db.get(), m_app_db.get(), m_tables);
    createVnet(mgr, "Vnet1", "1000");
    ASSERT_TRUE(mgr.doVnetRouteTunnelCreateTask(route("Vnet1|192.168.5.0/24", "10.0.0.2", "02:00:00:00:00:01")));
    mockCallArgs.clear();
    ASSERT_TRUE(mgr.doVnetRouteTunnelCreateTask(route("Vnet1|192.168.5.0/24", "10.0.0.3", "02:00:00:00:00:01", "2000")));
    ASSERT_TRUE(issued("route replace 192.168.5.0/24 encap ip id 2000 dst 10.0.0.3"));
    ASSERT_FALSE(issued(" route del "));
    ASSERT_FALSE(issued(" neigh "));
    ASSERT_EQ(mgr.m_neighRefs["02:00:00:00:00:01"], 1);
    ASSERT_EQ(mgr.m_kernelRouteTunnelCache["Vnet1"]["192.168.5.0/24"].m_dstIp, "10.0.0.3");
}

TEST_F(VNetMgrTest, MacChangeSwapsNeigh)
{
    VNetMgr mgr(m_cfg_db.get(), m_app_db.get(), m_tables);
    createVnet(mgr, "Vnet1", "1000");
    ASSERT_TRUE(mgr.doVnetRouteTunnelCreateTask(route("Vnet1|192.168.5.0/24", "10.0.0.2", "02:00:00:00:00:01")));
    mockCallArgs.clear();
    ASSERT_TRUE(mgr.doVnetRouteTunnelCreateTask(route("Vnet1|192.168.5.0/24", "10.0.0.2", "02:00:00:00:00:aa")));
    ASSERT_TRUE(issued("-6 neigh replace fe80::200:0:aa lladdr 02:00:00:00:00:aa"));
    ASSERT_TRUE(issued("via inet6 fe80::200:0:aa dev VxlanOutbSvd"));
    ASSERT_TRUE(issued("-6 neigh del fe80::200:0:1 dev VxlanOutbSvd"));
    ASSERT_EQ(mgr.m_neighRefs.count("02:00:00:00:00:01"), 0u);
    ASSERT_EQ(mgr.m_neighRefs["02:00:00:00:00:aa"], 1);
}

TEST_F(VNetMgrTest, RouteAddFailureRollsBackNewNeigh)
{
    VNetMgr mgr(m_cfg_db.get(), m_app_db.get(), m_tables);
    createVnet(mgr, "Vnet1", "1000");
    g_fail_route_add = true;
    ASSERT_FALSE(mgr.doVnetRouteTunnelCreateTask(route("Vnet1|192.168.1.1/32", "10.0.0.2", "02:00:00:00:00:01")));
    ASSERT_TRUE(issued("-6 neigh del fe80::200:0:1 dev VxlanOutbSvd"));
    ASSERT_TRUE(mgr.m_neighRefs.empty());
    ASSERT_TRUE(mgr.m_kernelRouteTunnelCache.empty());
}

TEST_F(VNetMgrTest, NeighFailureSkipsRoute)
{
    VNetMgr mgr(m_cfg_db.get(), m_app_db.get(), m_tables);
    createVnet(mgr, "Vnet1", "1000");
    g_fail_neigh_add = true;
    ASSERT_FALSE(mgr.doVnetRouteTunnelCreateTask(route("Vnet1|192.168.1.1/32", "10.0.0.2", "02:00:00:00:00:01")));
    ASSERT_FALSE(issued(" route replace "));
}

TEST_F(VNetMgrTest, NftFailureFailsRouteTaskForRetry)
{
    VNetMgr mgr(m_cfg_db.get(), m_app_db.get(), m_tables);
    createVnet(mgr, "Vnet1", "1000");
    g_fail_nft = true;
    ASSERT_FALSE(mgr.doVnetRouteTunnelCreateTask(route("Vnet1|192.168.1.1/32", "10.0.0.2", "02:00:00:00:00:01")));
    ASSERT_FALSE(issued(R1));
    ASSERT_FALSE(mgr.m_txReady);

    g_fail_nft = false;
    mockCallArgs.clear();
    ASSERT_TRUE(mgr.doVnetRouteTunnelCreateTask(route("Vnet1|192.168.1.1/32", "10.0.0.2", "02:00:00:00:00:01")));
    ASSERT_FALSE(issued("link add"));
    ASSERT_TRUE(issued("udp dport 4790 udp dport set 4789"));
    ASSERT_TRUE(issued(R1));
    ASSERT_TRUE(mgr.m_txReady);
}

TEST_F(VNetMgrTest, ExistingMatchingTxDeviceIsAdopted)
{
    g_links[VNET_VXLAN_TX_DEV] = 55;
    g_txDetails = "vxlan external id 0 local 10.1.0.1 srcport 0 0 dstport 4790 nolearning ";
    VNetMgr mgr(m_cfg_db.get(), m_app_db.get(), m_tables);
    createVnet(mgr, "Vnet1", "1000");
    ASSERT_TRUE(mgr.doVnetRouteTunnelCreateTask(route("Vnet1|192.168.1.1/32", "10.0.0.2", "02:00:00:00:00:01")));
    ASSERT_FALSE(issued("link add"));
    ASSERT_FALSE(issued("link del"));
    ASSERT_TRUE(mgr.m_txReady);
}

TEST_F(VNetMgrTest, ExistingMismatchedTxDeviceIsRecreated)
{
    g_links[VNET_VXLAN_TX_DEV] = 55;
    g_txDetails = "vxlan external id 0 local 10.9.9.9 srcport 0 0 dstport 4790 nolearning ";
    VNetMgr mgr(m_cfg_db.get(), m_app_db.get(), m_tables);
    createVnet(mgr, "Vnet1", "1000");
    ASSERT_TRUE(mgr.doVnetRouteTunnelCreateTask(route("Vnet1|192.168.1.1/32", "10.0.0.2", "02:00:00:00:00:01")));
    ASSERT_TRUE(issued("link del dev VxlanOutbSvd"));
    ASSERT_TRUE(issued("link add VxlanOutbSvd type vxlan external dstport 4790 local 10.1.0.1"));
}

TEST_F(VNetMgrTest, KernelRouteWaitsForRouterMacThenSwitchConfigIsCached)
{
    swss::Table switchTable(m_app_db.get(), APP_SWITCH_TABLE_NAME);
    switchTable.hdel("switch", "vxlan_router_mac");
    switchTable.set("switch", {{"vxlan_port", "13330"}});
    VNetMgr mgr(m_cfg_db.get(), m_app_db.get(), m_tables);
    createVnet(mgr, "Vnet1", "1000");
    ASSERT_FALSE(mgr.doVnetRouteTunnelCreateTask(route("Vnet1|192.168.1.1/32", "10.0.0.2", "02:00:00:00:00:01")));
    ASSERT_FALSE(issued("link add"));
    ASSERT_FALSE(issued(R1));

    switchTable.set("switch", {{"vxlan_router_mac", "aa:bb:cc:dd:ee:ff"}});
    ASSERT_TRUE(mgr.doVnetRouteTunnelCreateTask(route("Vnet1|192.168.1.1/32", "10.0.0.2", "02:00:00:00:00:01")));
    ASSERT_TRUE(issued("type vxlan external dstport 13331 local 10.1.0.1"));
    ASSERT_TRUE(issued(R1));
    ASSERT_TRUE(issued("tc qdisc replace dev Vxlan1000 clsact"));
    ASSERT_TRUE(issued("tc filter add dev Vxlan1000 ingress pref 1 handle 1 matchall action pedit ex munge eth dst set aa:bb:cc:dd:ee:ff"));

    switchTable.set("switch", {{"vxlan_router_mac", "11:22:33:44:55:66"}, {"vxlan_port", "5000"}});
    mockCallArgs.clear();
    ASSERT_TRUE(mgr.doVnetRouteTunnelCreateTask(route("Vnet1|192.168.3.3/32", "10.0.0.4", "")));
    ASSERT_FALSE(issued("tc qdisc replace"));
    ASSERT_TRUE(issued("lladdr aa:bb:cc:dd:ee:ff"));
    ASSERT_EQ(mgr.m_switchVxlanCfg.m_routerMac, "aa:bb:cc:dd:ee:ff");
    ASSERT_EQ(mgr.m_switchVxlanCfg.m_basePort, 13330u);
}

TEST_F(VNetMgrTest, KernelRouteDeferredUntilVxlanNetdevButAppDbPublishedOnce)
{
    g_links.erase("Vxlan1000");
    VNetMgr mgr(m_cfg_db.get(), m_app_db.get(), m_tables);
    createVnet(mgr, "Vnet1", "1000");
    swss::ConsumerStateTable consumer(m_app_db.get(), APP_VNET_RT_TUNNEL_TABLE_NAME);
    auto r = route("Vnet1|192.168.1.1/32", "10.0.0.2", "02:00:00:00:00:01");

    ASSERT_FALSE(mgr.doVnetRouteTunnelCreateTask(r));
    ASSERT_FALSE(mgr.doVnetRouteTunnelCreateTask(r));
    ASSERT_FALSE(issued(" route replace "));
    ASSERT_FALSE(issued("tc "));
    std::deque<swss::KeyOpFieldsValuesTuple> entries;
    consumer.pops(entries);
    ASSERT_EQ(entries.size(), 1u);
    ASSERT_EQ(kfvOp(entries.front()), SET_COMMAND);

    g_links["Vxlan1000"] = 10;
    ASSERT_TRUE(mgr.doVnetRouteTunnelCreateTask(r));
    ASSERT_TRUE(issued("tc qdisc replace dev Vxlan1000 clsact"));
    ASSERT_TRUE(issued(R1));
    entries.clear();
    consumer.pops(entries);
    ASSERT_TRUE(entries.empty());
}

TEST_F(VNetMgrTest, DmacBypassReappliedOnVxlanStateEvent)
{
    VNetMgr mgr(m_cfg_db.get(), m_app_db.get(), m_tables);
    createVnet(mgr, "Vnet1", "1000");
    ASSERT_TRUE(mgr.doVxlanStateTask(makeTuple("Vxlan1000", SET_COMMAND, {{"state", "ok"}})));
    ASSERT_FALSE(issued("tc qdisc replace"));

    ASSERT_TRUE(mgr.doVnetRouteTunnelCreateTask(route("Vnet1|192.168.1.1/32", "10.0.0.2", "02:00:00:00:00:01")));
    ASSERT_TRUE(issued("tc qdisc replace dev Vxlan1000 clsact"));

    ASSERT_TRUE(mgr.doVxlanStateTask(makeTuple("Vxlan1000", DEL_COMMAND, {})));
    ASSERT_EQ(mgr.m_dmacBypassInstalledVnis.count("1000"), 0u);
    mockCallArgs.clear();
    ASSERT_TRUE(mgr.doVxlanStateTask(makeTuple("Vxlan1000", SET_COMMAND, {{"state", "ok"}})));
    ASSERT_TRUE(issued("tc filter add dev Vxlan1000 ingress pref 1 handle 1 matchall"));
    ASSERT_EQ(mgr.m_dmacBypassInstalledVnis.count("1000"), 1u);

    mockCallArgs.clear();
    ASSERT_TRUE(mgr.doVxlanStateTask(makeTuple("Vxlan1000", SET_COMMAND, {{"state", "ok"}})));
    ASSERT_TRUE(issued("tc filter add dev Vxlan1000"));

    g_tcFilterShow = "filter protocol all pref 1 matchall chain 0 handle 0x1\n";
    mockCallArgs.clear();
    ASSERT_TRUE(mgr.doVxlanStateTask(makeTuple("Vxlan1000", SET_COMMAND, {{"state", "ok"}})));
    ASSERT_TRUE(issued("tc filter show dev Vxlan1000 ingress pref 1"));
    ASSERT_FALSE(issued("tc filter add"));
    ASSERT_EQ(mgr.m_dmacBypassInstalledVnis.count("1000"), 1u);

    mockCallArgs.clear();
    ASSERT_TRUE(mgr.doVxlanStateTask(makeTuple("Vxlan2000", SET_COMMAND, {{"state", "ok"}})));
    ASSERT_FALSE(issued("tc "));
}

TEST_F(VNetMgrTest, VnetDeleteRemovesBypassAndLastVnetTearsDownTx)
{
    VNetMgr mgr(m_cfg_db.get(), m_app_db.get(), m_tables);
    createVnet(mgr, "Vnet1", "1000");
    auto r = route("Vnet1|192.168.1.1/32", "10.0.0.2", "02:00:00:00:00:01");
    ASSERT_TRUE(mgr.doVnetRouteTunnelCreateTask(r));
    ASSERT_TRUE(mgr.doVnetRouteTunnelDeleteTask(makeTuple("Vnet1|192.168.1.1/32", DEL_COMMAND, {})));

    mockCallArgs.clear();
    ASSERT_TRUE(mgr.doVnetDeleteTask(makeTuple("Vnet1", DEL_COMMAND, {})));
    ASSERT_TRUE(issued("tc qdisc del dev Vxlan1000 clsact"));
    ASSERT_TRUE(issued("link del dev VxlanOutbSvd"));
    ASSERT_TRUE(issued("nft delete table inet sonic_vnet_vxtx"));
    ASSERT_FALSE(mgr.m_txReady);
    ASSERT_EQ(mgr.m_vnetCache.find("Vnet1"), mgr.m_vnetCache.end());
}

TEST_F(VNetMgrTest, DmacBypassDeleteSkippedWhenNeverInstalled)
{
    VNetMgr mgr(m_cfg_db.get(), m_app_db.get(), m_tables);
    createVnet(mgr, "Vnet1", "1000");
    mockCallArgs.clear();
    ASSERT_TRUE(mgr.doVnetDeleteTask(makeTuple("Vnet1", DEL_COMMAND, {})));
    ASSERT_FALSE(issued("tc qdisc del"));
}

TEST_F(VNetMgrTest, ReconcileRemovesStaleRoutesAndNeighbours)
{
    VNetMgr mgr(m_cfg_db.get(), m_app_db.get(), m_tables);
    createVnet(mgr, "Vnet1", "1000");
    ASSERT_TRUE(mgr.doVnetRouteTunnelCreateTask(route("Vnet1|192.168.1.1/32", "10.0.0.2", "02:00:00:00:00:01")));

    g_routeShowV4 =
        "192.168.1.1 encap ip id 1000 src 10.1.0.1 dst 10.0.0.2 ttl 0 tos 0 via inet6 fe80::200:0:1 dev VxlanOutbSvd onlink \n"
        "192.168.9.0/24 encap ip id 1000 src 10.1.0.1 dst 10.0.0.9 ttl 0 tos 0 via inet6 fe80::200:0:9 dev VxlanOutbSvd onlink \n"
        "default encap ip id 1000 src 10.1.0.1 dst 10.0.0.9 ttl 0 tos 0 via inet6 fe80::200:0:9 dev VxlanOutbSvd onlink \n";
    g_neighShow =
        "fe80::200:0:1 lladdr 02:00:00:00:00:01 PERMANENT\n"
        "fe80::200:0:9 lladdr 02:00:00:00:00:09 PERMANENT\n";
    mockCallArgs.clear();
    mgr.reconcileKernelState();
    ASSERT_FALSE(issued("route del 192.168.1.1 "));
    ASSERT_TRUE(issued("route del 192.168.9.0/24 dev VxlanOutbSvd vrf Vnet1"));
    ASSERT_TRUE(issued("route del 0.0.0.0/0 dev VxlanOutbSvd vrf Vnet1"));
    ASSERT_FALSE(issued("neigh del fe80::200:0:1 "));
    ASSERT_TRUE(issued("-6 neigh del fe80::200:0:9 dev VxlanOutbSvd"));
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
    ASSERT_FALSE(issued(" route replace "));
    ASSERT_FALSE(issued(" neigh "));
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
    ASSERT_TRUE(mgr.doVnetRouteTunnelCreateTask(route("Vnet1|192.168.1.1/32", "10.0.0.2", "02:00:00:00:00:01", "1000")));

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

    ASSERT_TRUE(mgr.doVnetRouteTunnelCreateTask(route("Vnet1|192.168.1.1/32", "10.0.0.2", "02:00:00:00:00:01", "1000")));
    ASSERT_TRUE(mgr.doVnetRouteTunnelDeleteTask(makeTuple("Vnet1|192.168.1.1/32", DEL_COMMAND, {})));

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

TEST_F(VNetMgrTest, RouteTunnelMalformedPrefixStillPublished)
{
    VNetMgr mgr(m_cfg_db.get(), m_app_db.get(), m_tables);
    createVnet(mgr, "Vnet1", "1000");
    ASSERT_TRUE(mgr.doVnetRouteTunnelCreateTask(route("Vnet1|192.168.1.1/abc", "10.0.0.2", "02:00:00:00:00:01", "1000")));

    std::string op;
    std::vector<swss::FieldValueTuple> fvs;
    ASSERT_TRUE(popAppDbEntry(m_app_db.get(), APP_VNET_RT_TUNNEL_TABLE_NAME,
                              "Vnet1:192.168.1.1/abc", op, fvs));
    ASSERT_EQ(op, SET_COMMAND);

    ASSERT_TRUE(mgr.doVnetRouteTunnelDeleteTask(makeTuple("Vnet1|192.168.1.1/abc", DEL_COMMAND, {})));
    ASSERT_TRUE(popAppDbEntry(m_app_db.get(), APP_VNET_RT_TUNNEL_TABLE_NAME,
                              "Vnet1:192.168.1.1/abc", op, fvs));
    ASSERT_EQ(op, DEL_COMMAND);
}

static KeyOpFieldsValuesTuple smacRule(const std::string &key, const std::string &ip,
                                       const std::string &mac, const std::string &vni,
                                       const std::string &prio = "1000")
{
    return makeTuple(key, SET_COMMAND,
        {{"INNER_SRC_IP", ip}, {"INNER_SRC_MAC_REWRITE_ACTION", mac},
         {"PRIORITY", prio}, {"TUNNEL_VNI", vni}});
}

static const char *SMAC_V4 = "tc filter add dev VxlanOutbSvd egress pref 63534 protocol ip handle 1 flower "
                             "enc_key_id 1000 src_ip 10.6.0.4/32 action pedit ex munge eth src set 00:22:48:03:45:8b";

TEST_F(VNetMgrTest, SmacRewriteWaitsForKernelRouteInVnet)
{
    VNetMgr mgr(m_cfg_db.get(), m_app_db.get(), m_tables);
    createVnet(mgr, "Vnet1", "1000");
    ASSERT_TRUE(mgr.doAclRuleTask(smacRule("T|r1", "10.6.0.4/32", "00:22:48:03:45:8B", "1000")));
    ASSERT_FALSE(issued("filter add dev VxlanOutbSvd"));

    ASSERT_TRUE(mgr.doVnetRouteTunnelCreateTask(route("Vnet1|192.168.1.1/32", "10.0.0.2", "02:00:00:00:00:01")));
    ASSERT_TRUE(issued("tc qdisc replace dev VxlanOutbSvd clsact"));
    ASSERT_TRUE(issued(SMAC_V4));

    mockCallArgs.clear();
    ASSERT_TRUE(mgr.doVnetRouteTunnelCreateTask(route("Vnet1|192.168.2.1/32", "10.0.0.2", "02:00:00:00:00:01")));
    ASSERT_FALSE(issued("filter add dev VxlanOutbSvd"));
}

TEST_F(VNetMgrTest, SmacRewriteIgnoresOtherVniAndNonRewriteRules)
{
    VNetMgr mgr(m_cfg_db.get(), m_app_db.get(), m_tables);
    createVnet(mgr, "Vnet1", "1000");
    ASSERT_TRUE(mgr.doVnetRouteTunnelCreateTask(route("Vnet1|192.168.1.1/32", "10.0.0.2", "02:00:00:00:00:01")));
    mockCallArgs.clear();
    ASSERT_TRUE(mgr.doAclRuleTask(smacRule("T|r1", "10.6.0.4/32", "00:22:48:03:45:8b", "2000")));
    ASSERT_TRUE(mgr.doAclRuleTask(makeTuple("DATAACL|r2", SET_COMMAND,
        {{"SRC_IP", "10.0.0.0/8"}, {"PACKET_ACTION", "DROP"}})));
    ASSERT_TRUE(mgr.doAclRuleTask(smacRule("T|r3", "10.6.0.4/32", "zz:22", "1000")));
    ASSERT_FALSE(issued("filter add dev VxlanOutbSvd"));
    ASSERT_EQ(mgr.m_smacRuleVni.size(), 1u);
}

TEST_F(VNetMgrTest, SmacRewriteV6UsesOddPrefAndIpv6Protocol)
{
    VNetMgr mgr(m_cfg_db.get(), m_app_db.get(), m_tables);
    createVnet(mgr, "Vnet1", "1000");
    ASSERT_TRUE(mgr.doVnetRouteTunnelCreateTask(route("Vnet1|192.168.1.1/32", "10.0.0.2", "02:00:00:00:00:01")));
    ASSERT_TRUE(mgr.doAclRuleTask(smacRule("T|r6", "fc00:1::100/128", "00:22:48:03:45:8c", "1000")));
    ASSERT_TRUE(issued("filter add dev VxlanOutbSvd egress pref 63535 protocol ipv6 handle 1 flower "
                       "enc_key_id 1000 src_ip fc00:1::100/128 action pedit ex munge eth src set 00:22:48:03:45:8c"));
}

TEST_F(VNetMgrTest, SmacRewriteRemovedOnRuleDelUpdateAndLastRouteDelete)
{
    VNetMgr mgr(m_cfg_db.get(), m_app_db.get(), m_tables);
    createVnet(mgr, "Vnet1", "1000");
    ASSERT_TRUE(mgr.doVnetRouteTunnelCreateTask(route("Vnet1|192.168.1.1/32", "10.0.0.2", "02:00:00:00:00:01")));
    ASSERT_TRUE(mgr.doAclRuleTask(smacRule("T|r1", "10.6.0.4/32", "00:22:48:03:45:8b", "1000")));
    ASSERT_TRUE(issued(SMAC_V4));

    mockCallArgs.clear();
    ASSERT_TRUE(mgr.doAclRuleTask(smacRule("T|r1", "10.6.0.4/32", "00:22:48:03:45:8b", "1000")));
    ASSERT_TRUE(mockCallArgs.empty());

    ASSERT_TRUE(mgr.doAclRuleTask(smacRule("T|r1", "10.6.0.4/32", "00:22:48:03:45:99", "1000")));
    ASSERT_TRUE(issued("tc filter del dev VxlanOutbSvd egress pref 63534 protocol ip handle 1 flower"));
    ASSERT_TRUE(issued("handle 1 flower enc_key_id 1000 src_ip 10.6.0.4/32 action pedit ex munge eth src set 00:22:48:03:45:99"));

    mockCallArgs.clear();
    ASSERT_TRUE(mgr.doVnetRouteTunnelDeleteTask(makeTuple("Vnet1|192.168.1.1/32", DEL_COMMAND, {})));
    ASSERT_TRUE(issued("tc filter del dev VxlanOutbSvd egress pref 63534 protocol ip handle 1 flower"));
    ASSERT_FALSE(mgr.m_smacRewriteRules["1000"]["T|r1"].m_installed);

    ASSERT_TRUE(mgr.doVnetRouteTunnelCreateTask(route("Vnet1|192.168.1.1/32", "10.0.0.2", "02:00:00:00:00:01")));
    ASSERT_TRUE(mgr.m_smacRewriteRules["1000"]["T|r1"].m_installed);
    mockCallArgs.clear();
    ASSERT_TRUE(mgr.doAclRuleTask(makeTuple("T|r1", DEL_COMMAND, {})));
    ASSERT_TRUE(issued("tc filter del dev VxlanOutbSvd egress pref 63534 protocol ip handle 1 flower"));
    ASSERT_TRUE(mgr.m_smacRewriteRules.empty());
    ASSERT_TRUE(mgr.m_smacRuleVni.empty());
}

TEST_F(VNetMgrTest, SmacRewriteRemovedOnlyWhenLastRouteOfVniGoes)
{
    VNetMgr mgr(m_cfg_db.get(), m_app_db.get(), m_tables);
    createVnet(mgr, "Vnet1", "1000");
    ASSERT_TRUE(mgr.doAclRuleTask(smacRule("T|r1", "10.6.0.4/32", "00:22:48:03:45:8b", "1000")));
    ASSERT_TRUE(mgr.doVnetRouteTunnelCreateTask(route("Vnet1|192.168.1.1/32", "10.0.0.2", "02:00:00:00:00:01")));
    ASSERT_TRUE(mgr.doVnetRouteTunnelCreateTask(route("Vnet1|192.168.2.1/32", "10.0.0.2", "02:00:00:00:00:01")));
    ASSERT_EQ(mgr.m_kernelRouteTunnelCache["Vnet1"].size(), 2u);

    mockCallArgs.clear();
    ASSERT_TRUE(mgr.doVnetRouteTunnelDeleteTask(makeTuple("Vnet1|192.168.1.1/32", DEL_COMMAND, {})));
    ASSERT_FALSE(issued("tc filter del dev VxlanOutbSvd"));
    ASSERT_TRUE(mgr.m_smacRewriteRules["1000"]["T|r1"].m_installed);

    ASSERT_TRUE(mgr.doVnetRouteTunnelDeleteTask(makeTuple("Vnet1|192.168.2.1/32", DEL_COMMAND, {})));
    ASSERT_TRUE(issued("tc filter del dev VxlanOutbSvd egress pref 63534 protocol ip handle 1 flower"));
    ASSERT_TRUE(mgr.m_kernelRouteTunnelCache.empty());
}

TEST_F(VNetMgrTest, SmacRewriteRuleVniChangeMovesBucket)
{
    VNetMgr mgr(m_cfg_db.get(), m_app_db.get(), m_tables);
    createVnet(mgr, "Vnet1", "1000");
    ASSERT_TRUE(mgr.doVnetRouteTunnelCreateTask(route("Vnet1|192.168.1.1/32", "10.0.0.2", "02:00:00:00:00:01")));
    ASSERT_TRUE(mgr.doAclRuleTask(smacRule("T|r1", "10.6.0.4/32", "00:22:48:03:45:8b", "1000")));
    ASSERT_TRUE(issued(SMAC_V4));

    mockCallArgs.clear();
    ASSERT_TRUE(mgr.doAclRuleTask(smacRule("T|r1", "10.6.0.4/32", "00:22:48:03:45:8b", "2000")));
    ASSERT_TRUE(issued("tc filter del dev VxlanOutbSvd egress pref 63534 protocol ip handle 1 flower"));
    ASSERT_FALSE(issued("filter add dev VxlanOutbSvd"));
    ASSERT_EQ(mgr.m_smacRewriteRules.count("1000"), 0u);
    ASSERT_EQ(mgr.m_smacRuleVni["T|r1"], "2000");
    ASSERT_FALSE(mgr.m_smacRewriteRules["2000"]["T|r1"].m_installed);
}

TEST_F(VNetMgrTest, RouteDeleteAfterVnetDeleteStillRemovesKernelRoute)
{
    VNetMgr mgr(m_cfg_db.get(), m_app_db.get(), m_tables);
    createVnet(mgr, "Vnet1", "1000");
    ASSERT_TRUE(mgr.doVnetRouteTunnelCreateTask(route("Vnet1|192.168.1.1/32", "10.0.0.2", "02:00:00:00:00:01")));
    ASSERT_TRUE(mgr.doVnetDeleteTask(makeTuple("Vnet1", DEL_COMMAND, {})));
    mockCallArgs.clear();
    ASSERT_TRUE(mgr.doVnetRouteTunnelDeleteTask(makeTuple("Vnet1|192.168.1.1/32", DEL_COMMAND, {})));
    ASSERT_TRUE(issued("route del 192.168.1.1/32 dev VxlanOutbSvd vrf Vnet1"));
    ASSERT_TRUE(mgr.m_kernelRouteTunnelCache.empty());
}

TEST_F(VNetMgrTest, AdoptedTxDeviceFlushesStaleSmacFilters)
{
    g_links[VNET_VXLAN_TX_DEV] = 55;
    g_txDetails = "vxlan external id 0 local 10.1.0.1 srcport 0 0 dstport 4790 nolearning ";
    VNetMgr mgr(m_cfg_db.get(), m_app_db.get(), m_tables);
    createVnet(mgr, "Vnet1", "1000");
    ASSERT_TRUE(mgr.doVnetRouteTunnelCreateTask(route("Vnet1|192.168.1.1/32", "10.0.0.2", "02:00:00:00:00:01")));
    ASSERT_TRUE(issued("tc qdisc del dev VxlanOutbSvd clsact"));
}

} // namespace vnetmgr_ut

