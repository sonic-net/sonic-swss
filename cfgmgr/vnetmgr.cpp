#include <algorithm>
#include <sstream>
#include <string>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "logger.h"
#include "vnetmgr.h"
#include "exec.h"
#include "shellcmd.h"
#include "tokenize.h"
#include "ipaddress.h"
#include "ipprefix.h"
#include "macaddress.h"
#include "subscriberstatetable.h"

#define TC_CMD "/sbin/tc"
#define NFT_CMD "/usr/sbin/nft"

#define VXLAN_TUNNEL "vxlan_tunnel"
#define MAC_ADDRESS "mac_address"
#define ENDPOINT "endpoint"
#define INSTALL_ON_KERNEL "install_on_kernel"
#define VNI "vni"
#define VNET "vnet"
#define SOURCE_IP "src_ip"
#define VXLAN_NAME_PREFIX "Vxlan"
#define ACL_INNER_SRC_IP "INNER_SRC_IP"
#define ACL_INNER_SRC_MAC_REWRITE "INNER_SRC_MAC_REWRITE_ACTION"
#define ACL_TUNNEL_VNI "TUNNEL_VNI"
#define ACL_PRIORITY "PRIORITY"
#define SMAC_MAX_PRIORITY 32766
#define RET_SUCCESS 0

using namespace std;
using namespace swss;

static inline std::string getVxlanDeviceName(const std::string & vnetVni)
{
    return std::string(VXLAN_NAME_PREFIX) + vnetVni;
}

static inline bool isV6(const std::string & ip)
{
    return ip.find(':') != std::string::npos;
}

static std::string normalizeMac(const std::string & mac)
{
    uint8_t raw[ETHER_ADDR_LEN];
    if (!MacAddress::parseMacString(mac, raw))
    {
        return "";
    }
    return MacAddress(raw).to_string();
}

static std::string macToNexthop(const std::string & mac)
{
    uint8_t m[ETHER_ADDR_LEN];
    if (!MacAddress::parseMacString(mac, m))
    {
        return "";
    }
    char buf[64];
    snprintf(buf, sizeof(buf), "fe80::%x:%x:%x",
             (m[0] << 8) | m[1], (m[2] << 8) | m[3], (m[4] << 8) | m[5]);
    return IpAddress(buf).to_string();
}

static bool linkExists(const std::string & dev, std::string * details = nullptr)
{
    ostringstream cmd;
    cmd << IP_CMD << (details ? " -d" : "") << " -o link show dev " << shellquote(dev);
    std::string res;
    if (swss::exec(cmd.str(), res) != RET_SUCCESS)
    {
        return false;
    }
    if (details)
    {
        *details = res;
    }
    return true;
}

static int cmdCreateTxDevice(const std::string & vtep, unsigned port, std::string & res)
{
    ostringstream cmd;
    cmd << IP_CMD " link add " VNET_VXLAN_TX_DEV " type vxlan external"
        << " dstport " << port
        << " local " << shellquote(vtep)
        << " nolearning && "
        << IP_CMD " link set dev " VNET_VXLAN_TX_DEV " up";
    return swss::exec(cmd.str(), res);
}

static int cmdDeleteTxDevice(std::string & res)
{
    return swss::exec(IP_CMD " link del dev " VNET_VXLAN_TX_DEV, res);
}

static int cmdInstallNft(const std::string & vtep, unsigned basePort, std::string & res)
{
    const unsigned txPort = basePort + 1;
    const std::string fam = isV6(vtep) ? "ip6" : "ip";
    ostringstream rules;
    rules << "table inet " VNET_NFT_TABLE " {}; "
          << "delete table inet " VNET_NFT_TABLE "; "
          << "table inet " VNET_NFT_TABLE " { "
          << "chain out { type filter hook output priority 0; policy accept; "
          << fam << " saddr " << vtep << " udp dport " << txPort
          << " udp dport set " << basePort << "; }; "
          << "chain in { type filter hook input priority 0; policy accept; "
          << "udp dport " << txPort << " drop; }; }";
    ostringstream cmd;
    cmd << NFT_CMD " " << shellquote(rules.str()) << " 2>&1";
    return swss::exec(cmd.str(), res);
}

static int cmdDeleteNft(std::string & res)
{
    return swss::exec(NFT_CMD " delete table inet " VNET_NFT_TABLE, res);
}

static int cmdCreateKernelRoute(const swss::VNetMgr::VxlanKernelRouteInfo & info, std::string & res)
{
    ostringstream cmd;
    cmd << IP_CMD " route replace "
        << shellquote(info.m_prefix)
        << " encap " << (isV6(info.m_dstIp) ? "ip6" : "ip")
        << " id " << shellquote(info.m_vni)
        << " dst " << shellquote(info.m_dstIp)
        << " src " << shellquote(info.m_srcIp)
        << " via inet6 " << shellquote(macToNexthop(info.m_dstMac))
        << " dev " VNET_VXLAN_TX_DEV
        << " vrf " << shellquote(info.m_vnet)
        << " onlink proto redirect";
    return swss::exec(cmd.str(), res);
}

static int cmdDeleteKernelRoute(const std::string & prefix, const std::string & vnet, std::string & res)
{
    ostringstream cmd;
    cmd << IP_CMD " route del "
        << shellquote(prefix)
        << " dev " VNET_VXLAN_TX_DEV
        << " vrf " << shellquote(vnet);
    return swss::exec(cmd.str(), res);
}

static int cmdCreateNeighEntry(const std::string & mac, std::string & res)
{
    ostringstream cmd;
    cmd << IP_CMD " -6 neigh replace "
        << shellquote(macToNexthop(mac))
        << " lladdr " << shellquote(mac)
        << " dev " VNET_VXLAN_TX_DEV
        << " nud permanent";
    return swss::exec(cmd.str(), res);
}

static int cmdDeleteNeighIp(const std::string & ip, std::string & res)
{
    ostringstream cmd;
    cmd << IP_CMD " -6 neigh del " << shellquote(ip) << " dev " VNET_VXLAN_TX_DEV;
    return swss::exec(cmd.str(), res);
}

static int cmdInstallDmacBypass(const std::string & vxlanDev, const std::string & brmac, std::string & res)
{
    {
        ostringstream cmd;
        cmd << TC_CMD " qdisc replace dev " << shellquote(vxlanDev) << " clsact";
        int r = swss::exec(cmd.str(), res);
        if (r != RET_SUCCESS) return r;
    }
    {
        ostringstream cmd;
        cmd << TC_CMD " filter show dev " << shellquote(vxlanDev) << " ingress pref 1";
        int r = swss::exec(cmd.str(), res);
        if (r == RET_SUCCESS && !res.empty())
        {
            return RET_SUCCESS;
        }
    }
    ostringstream cmd;
    cmd << TC_CMD " filter add dev " << shellquote(vxlanDev)
        << " ingress pref 1 handle 1 matchall action pedit ex munge eth dst set " << shellquote(brmac);
    return swss::exec(cmd.str(), res);
}

static int cmdRemoveDmacBypass(const std::string & vxlanDev, std::string & res)
{
    ostringstream cmd;
    cmd << TC_CMD " qdisc del dev " << shellquote(vxlanDev) << " clsact";
    return swss::exec(cmd.str(), res);
}

static std::string smacFilterSpec(const swss::VNetMgr::SmacRewriteRule & rule)
{
    ostringstream spec;
    spec << " dev " VNET_VXLAN_TX_DEV " egress pref " << rule.m_pref
         << " protocol " << (isV6(rule.m_srcPrefix) ? "ipv6" : "ip")
         << " handle " << rule.m_handle << " flower";
    return spec.str();
}

static int cmdInstallSmacRewrite(const swss::VNetMgr::SmacRewriteRule & rule, std::string & res)
{
    int r = swss::exec(TC_CMD " qdisc replace dev " VNET_VXLAN_TX_DEV " clsact", res);
    if (r != RET_SUCCESS) return r;
    ostringstream cmd;
    cmd << TC_CMD " filter add" << smacFilterSpec(rule)
        << " enc_key_id " << shellquote(rule.m_vni)
        << " src_ip " << shellquote(rule.m_srcPrefix)
        << " action pedit ex munge eth src set " << shellquote(rule.m_mac);
    return swss::exec(cmd.str(), res);
}

static int cmdRemoveSmacRewrite(const swss::VNetMgr::SmacRewriteRule & rule, std::string & res)
{
    ostringstream cmd;
    cmd << TC_CMD " filter del" << smacFilterSpec(rule);
    return swss::exec(cmd.str(), res);
}

VNetMgr::VNetMgr(DBConnector *cfgDb, DBConnector *appDb, const std::vector<std::string> &tables,
                 DBConnector *stateDb) :
        Orch(cfgDb, tables),
        m_appSwitchTable(appDb, APP_SWITCH_TABLE_NAME),
        m_cfgVxlanTunnelTable(cfgDb, CFG_VXLAN_TUNNEL_TABLE_NAME),
        m_appVnetRouteTunnelTable(appDb, APP_VNET_RT_TUNNEL_TABLE_NAME),
        m_appVnetRouteTable(appDb, APP_VNET_RT_TABLE_NAME),
        m_app_db(appDb)
{
    if (stateDb)
    {
        addExecutor(new Consumer(new SubscriberStateTable(stateDb, STATE_VXLAN_TABLE_NAME,
                                                          TableConsumable::DEFAULT_POP_BATCH_SIZE, 0),
                                 this, STATE_VXLAN_TABLE_NAME));
    }
}

VNetMgr::~VNetMgr()
{
}

void VNetMgr::doTask(Consumer &consumer)
{
    SWSS_LOG_ENTER();

    const string & table_name = consumer.getTableName();
    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
        bool task_result = false;
        auto t = it->second;
        const std::string & op = kfvOp(t);

        if (table_name == STATE_VXLAN_TABLE_NAME)
        {
            task_result = doVxlanStateTask(t);
        }
        else if (table_name == CFG_ACL_RULE_TABLE_NAME)
        {
            task_result = doAclRuleTask(t);
        }
        else if (op == SET_COMMAND)
        {
            if (table_name == CFG_VNET_TABLE_NAME)
            {
                task_result = doVnetCreateTask(t);
            }
            else if (table_name == CFG_VNET_RT_TUNNEL_TABLE_NAME)
            {
                task_result = doVnetRouteTunnelCreateTask(t);
            }
            else if (table_name == CFG_VNET_RT_TABLE_NAME)
            {
                task_result = doVnetRouteTask(t, op);
            }
            else
            {
                SWSS_LOG_ERROR("Unknown table : %s", table_name.c_str());
            }
        }
        else if (op == DEL_COMMAND)
        {
            if (table_name == CFG_VNET_TABLE_NAME)
            {
                task_result = doVnetDeleteTask(t);
            }
            else if (table_name == CFG_VNET_RT_TUNNEL_TABLE_NAME)
            {
                task_result = doVnetRouteTunnelDeleteTask(t);
            }
            else if (table_name == CFG_VNET_RT_TABLE_NAME)
            {
                task_result = doVnetRouteTask(t, op);
            }
            else
            {
                SWSS_LOG_ERROR("Unknown table : %s", table_name.c_str());
            }
        }
        else
        {
            SWSS_LOG_ERROR("Unknown command : %s", op.c_str());
        }

        if (task_result == true)
        {
            it = consumer.m_toSync.erase(it);
        }
        else
        {
            ++it;
        }
    }
}

bool VNetMgr::hasPendingTasks()
{
    for (auto & kv : m_consumerMap)
    {
        auto consumer = dynamic_cast<Consumer *>(kv.second.get());
        if (consumer && !consumer->m_toSync.empty())
        {
            return true;
        }
    }
    return false;
}

bool VNetMgr::doVnetCreateTask(const KeyOpFieldsValuesTuple & t)
{
    SWSS_LOG_ENTER();

    VnetInfo info;
    const std::string & vnet = kfvKey(t);
    for (auto i : kfvFieldsValues(t))
    {
        const std::string & field = fvField(i);
        const std::string & value = fvValue(i);
        if (field == VXLAN_TUNNEL) { info.m_vxlanTunnel = value; }
        else if (field == VNI) { info.m_vni = value; }
    }

    if (info.m_vxlanTunnel.empty() || info.m_vni.empty())
    {
        SWSS_LOG_DEBUG("Vnet %s information is incomplete", vnet.c_str());
        return true;
    }
    m_vnetCache[vnet] = info;

    SWSS_LOG_INFO("Create VNET %s vni %s",
                    vnet.c_str(), info.m_vni.c_str());
    return true;
}

bool VNetMgr::doVnetDeleteTask(const KeyOpFieldsValuesTuple & t)
{
    SWSS_LOG_ENTER();

    const std::string & vnetName = kfvKey(t);
    auto it = m_vnetCache.find(vnetName);
    if (it == m_vnetCache.end())
    {
        SWSS_LOG_WARN("Vnet %s hasn't been created", vnetName.c_str());
        return true;
    }
    removeDmacBypass(it->second.m_vni);
    m_vnetCache.erase(it);
    if (m_vnetCache.empty() && m_kernelRouteTunnelCache.empty())
    {
        teardownTxDevice();
    }
    SWSS_LOG_INFO("Delete vnet %s", vnetName.c_str());
    return true;
}

bool VNetMgr::doVnetRouteTask(const KeyOpFieldsValuesTuple & t, const string & op)
{
    SWSS_LOG_ENTER();
    string vnetRouteName = kfvKey(t);
    replace(vnetRouteName.begin(), vnetRouteName.end(), config_db_key_delimiter, delimiter);
    if (op == SET_COMMAND)
    {
        m_appVnetRouteTable.set(vnetRouteName, kfvFieldsValues(t));
    }
    else if (op == DEL_COMMAND)
    {
        m_appVnetRouteTable.del(vnetRouteName);
    }
    else
    {
        SWSS_LOG_ERROR("Unknown command : %s", op.c_str());
        return false;
    }
    return true;
}

bool VNetMgr::doVnetRouteTunnelCreateTask(const KeyOpFieldsValuesTuple & t)
{
    SWSS_LOG_ENTER();

    const std::string & vnet_route_name = kfvKey(t);
    VxlanRouteTunnelInfo routeInfo{};

    size_t delimiter_pos = vnet_route_name.find_first_of(config_db_key_delimiter);
    routeInfo.m_vnet = vnet_route_name.substr(0, delimiter_pos);
    routeInfo.m_prefix = vnet_route_name.substr(delimiter_pos + 1);

    for (auto i : kfvFieldsValues(t))
    {
        const std::string & field = fvField(i);
        const std::string & value = fvValue(i);
        if (field == ENDPOINT) routeInfo.m_endpoint = value;
        else if (field == MAC_ADDRESS) routeInfo.m_macAddress = value;
        else if (field == VNI) routeInfo.m_vni = value;
        else if (field == INSTALL_ON_KERNEL) routeInfo.m_installOnKernel = (value == "true");
    }

    SWSS_LOG_NOTICE("Vxlan tunnel route vnet %s prefix %s dst %s mac %s vni %s",
                    routeInfo.m_vnet.c_str(), routeInfo.m_prefix.c_str(),
                    routeInfo.m_endpoint.c_str(), routeInfo.m_macAddress.c_str(),
                    routeInfo.m_vni.c_str());

    routeInfo.m_routeName = vnet_route_name;

    string vnetRouteTunnelName = kfvKey(t);
    replace(vnetRouteTunnelName.begin(), vnetRouteTunnelName.end(), config_db_key_delimiter, delimiter);

    std::vector<swss::FieldValueTuple> values = kfvFieldsValues(t);
    values.erase(std::remove_if(values.begin(), values.end(),
                [](const swss::FieldValueTuple & fv) { return fv.first == INSTALL_ON_KERNEL; }),
                values.end());

    auto published = m_appVnetRouteTunnelPublished.find(vnetRouteTunnelName);
    if (published == m_appVnetRouteTunnelPublished.end() || published->second != values)
    {
        m_appVnetRouteTunnelTable.set(vnetRouteTunnelName, values);
        m_appVnetRouteTunnelPublished[vnetRouteTunnelName] = values;
    }

    try
    {
        if (routeInfo.m_installOnKernel)
        {
            if (!createKernelRoute(routeInfo))
            {
                SWSS_LOG_INFO("Kernel route %s not programmed yet, will retry", vnet_route_name.c_str());
                return false;
            }
        }
        else
        {
            deleteKernelRoute(routeInfo.m_vnet, routeInfo.m_prefix);
        }
    }
    catch (const std::exception & e)
    {
        SWSS_LOG_ERROR("Kernel install/uninstall for vnet route %s failed: %s",
                       vnet_route_name.c_str(), e.what());
    }

    return true;
}

bool VNetMgr::doVnetRouteTunnelDeleteTask(const KeyOpFieldsValuesTuple & t)
{
    SWSS_LOG_ENTER();
    const std::string & vnet_route_name = kfvKey(t);
    std::string appKey = vnet_route_name;
    std::replace(appKey.begin(), appKey.end(), config_db_key_delimiter, delimiter);
    auto published = m_appVnetRouteTunnelPublished.find(appKey);
    if (published == m_appVnetRouteTunnelPublished.end())
    {
        SWSS_LOG_WARN("Vxlan route tunnel %s hasn't been created", vnet_route_name.c_str());
        return true;
    }
    size_t delimiter_pos = vnet_route_name.find_first_of(config_db_key_delimiter);
    try
    {
        deleteKernelRoute(vnet_route_name.substr(0, delimiter_pos),
                          vnet_route_name.substr(delimiter_pos + 1));
    }
    catch (const std::exception & e)
    {
        SWSS_LOG_ERROR("Kernel uninstall for vnet route %s failed: %s",
                       vnet_route_name.c_str(), e.what());
    }
    m_appVnetRouteTunnelTable.del(appKey);
    m_appVnetRouteTunnelPublished.erase(published);
    return true;
}

bool VNetMgr::readRouterMac(std::string & routerMac)
{
    routerMac.clear();
    std::string value;
    if (m_appSwitchTable.hget("switch", "vxlan_router_mac", value))
    {
        routerMac = value;
    }
    return !routerMac.empty();
}

unsigned VNetMgr::readBaseVxlanPort()
{
    std::string value;
    if (m_appSwitchTable.hget("switch", "vxlan_port", value) && !value.empty())
    {
        char *end = nullptr;
        unsigned long port = strtoul(value.c_str(), &end, 10);
        if (end && *end == '\0' && port > 0 && port < 65535)
        {
            return static_cast<unsigned>(port);
        }
        SWSS_LOG_WARN("Ignoring invalid SWITCH_TABLE vxlan_port '%s'", value.c_str());
    }
    return VNET_DEFAULT_VXLAN_PORT;
}

bool VNetMgr::readVtepSrcIp(const std::string & tunnel, std::string & srcIp)
{
    srcIp.clear();
    std::string value;
    if (m_cfgVxlanTunnelTable.hget(tunnel, SOURCE_IP, value))
    {
        srcIp = value;
    }
    return !srcIp.empty();
}

bool VNetMgr::installNftTable()
{
    std::string res;
    if (cmdInstallNft(m_txVtep, m_txBasePort, res) != RET_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to install nft table %s (tx dport %u -> %u): %s",
                       VNET_NFT_TABLE, m_txBasePort + 1, m_txBasePort, res.c_str());
        return false;
    }
    SWSS_LOG_NOTICE("Installed nft table %s: %s udp dport %u -> %u",
                    VNET_NFT_TABLE, m_txVtep.c_str(), m_txBasePort + 1, m_txBasePort);
    return true;
}

bool VNetMgr::ensureTxDevice(const std::string & vtep, unsigned basePort)
{
    if (m_txReady)
    {
        return true;
    }

    const unsigned txPort = basePort + 1;
    std::string details, res;
    bool present = linkExists(VNET_VXLAN_TX_DEV, &details);
    if (present)
    {
        const std::string padded = details + " ";
        bool matches = padded.find(" external ") != std::string::npos &&
                       padded.find(" local " + vtep + " ") != std::string::npos &&
                       padded.find(" dstport " + std::to_string(txPort) + " ") != std::string::npos;
        if (!matches)
        {
            SWSS_LOG_NOTICE("Recreating %s for vtep %s dstport %u",
                            VNET_VXLAN_TX_DEV, vtep.c_str(), txPort);
            cmdDeleteTxDevice(res);
            present = false;
        }
        else
        {
            swss::exec(TC_CMD " qdisc del dev " VNET_VXLAN_TX_DEV " clsact", res);
        }
    }
    if (!present)
    {
        if (cmdCreateTxDevice(vtep, txPort, res) != RET_SUCCESS)
        {
            SWSS_LOG_ERROR("Failed to create %s: %s", VNET_VXLAN_TX_DEV, res.c_str());
            return false;
        }
        SWSS_LOG_NOTICE("Created %s (external, local %s, dstport %u)",
                        VNET_VXLAN_TX_DEV, vtep.c_str(), txPort);
    }

    m_txVtep = vtep;
    m_txBasePort = basePort;
    if (!installNftTable())
    {
        return false;
    }
    m_txReady = true;
    return true;
}

void VNetMgr::teardownTxDevice()
{
    std::string res;
    if (m_txReady || linkExists(VNET_VXLAN_TX_DEV))
    {
        cmdDeleteTxDevice(res);
        SWSS_LOG_NOTICE("Deleted %s", VNET_VXLAN_TX_DEV);
    }
    cmdDeleteNft(res);
    m_txReady = false;
    m_txVtep.clear();
    m_txBasePort = 0;
    m_neighRefs.clear();
    for (auto & bucket : m_smacRewriteRules)
    {
        for (auto & kv : bucket.second)
        {
            kv.second.m_installed = false;
        }
    }
}

bool VNetMgr::neighRef(const std::string & mac)
{
    std::string res;
    auto it = m_neighRefs.find(mac);
    if (it != m_neighRefs.end())
    {
        it->second++;
        return true;
    }
    if (cmdCreateNeighEntry(mac, res) != RET_SUCCESS)
    {
        SWSS_LOG_ERROR("Neigh add %s for %s failed: %s",
                       macToNexthop(mac).c_str(), mac.c_str(), res.c_str());
        return false;
    }
    m_neighRefs[mac] = 1;
    return true;
}

void VNetMgr::neighUnref(const std::string & mac)
{
    auto it = m_neighRefs.find(mac);
    if (it == m_neighRefs.end())
    {
        return;
    }
    if (--(it->second) <= 0)
    {
        std::string res;
        cmdDeleteNeighIp(macToNexthop(mac), res);
        m_neighRefs.erase(it);
    }
}

bool VNetMgr::installDmacBypassIfNeeded(const std::string & vnetVni, bool force)
{
    if (!force && m_dmacBypassInstalledVnis.count(vnetVni))
    {
        return true;
    }
    const std::string vxlanDev = getVxlanDeviceName(vnetVni);
    if (!linkExists(vxlanDev))
    {
        SWSS_LOG_INFO("%s not present yet, DMAC bypass waits for vxlanmgrd", vxlanDev.c_str());
        return false;
    }
    std::string routerMac;
    if (!readRouterMac(routerMac))
    {
        SWSS_LOG_WARN("vxlan_router_mac not set in SWITCH_TABLE, skipping DMAC bypass on %s",
                      vxlanDev.c_str());
        return false;
    }
    std::string res;
    if (cmdInstallDmacBypass(vxlanDev, routerMac, res) != RET_SUCCESS)
    {
        SWSS_LOG_WARN("DMAC bypass install on %s failed: %s", vxlanDev.c_str(), res.c_str());
        return false;
    }
    m_dmacBypassInstalledVnis.insert(vnetVni);
    SWSS_LOG_NOTICE("Installed inner-DMAC bypass (tc pedit -> %s) on %s", routerMac.c_str(), vxlanDev.c_str());
    return true;
}

bool VNetMgr::doVxlanStateTask(const KeyOpFieldsValuesTuple & t)
{
    const std::string & key = kfvKey(t);
    if (key.compare(0, strlen(VXLAN_NAME_PREFIX), VXLAN_NAME_PREFIX) != 0)
    {
        return true;
    }
    const std::string vni = key.substr(strlen(VXLAN_NAME_PREFIX));
    if (kfvOp(t) == DEL_COMMAND)
    {
        m_dmacBypassInstalledVnis.erase(vni);
        return true;
    }
    if (isVniKernelRouted(vni))
    {
        installDmacBypassIfNeeded(vni, true);
    }
    return true;
}

void VNetMgr::removeDmacBypass(const std::string & vnetVni)
{
    auto it = m_dmacBypassInstalledVnis.find(vnetVni);
    if (it == m_dmacBypassInstalledVnis.end())
    {
        return;
    }
    const std::string vxlanDev = getVxlanDeviceName(vnetVni);
    std::string res;
    cmdRemoveDmacBypass(vxlanDev, res);
    m_dmacBypassInstalledVnis.erase(it);
    SWSS_LOG_NOTICE("Removed inner-DMAC bypass on %s", vxlanDev.c_str());
}

bool VNetMgr::createKernelRoute(const VxlanRouteTunnelInfo & vxlanRouteInfo)
{
    SWSS_LOG_ENTER();

    auto vnetIt = m_vnetCache.find(vxlanRouteInfo.m_vnet);
    if (vnetIt == m_vnetCache.end())
    {
        SWSS_LOG_INFO("Vnet %s hasn't been created yet, defer", vxlanRouteInfo.m_vnet.c_str());
        return false;
    }
    const VnetInfo & vnetInfo = vnetIt->second;

    std::string vtep;
    if (!readVtepSrcIp(vnetInfo.m_vxlanTunnel, vtep))
    {
        SWSS_LOG_INFO("VXLAN_TUNNEL %s src_ip not available yet, defer %s",
                      vnetInfo.m_vxlanTunnel.c_str(), vxlanRouteInfo.m_routeName.c_str());
        return false;
    }

    std::string mac = vxlanRouteInfo.m_macAddress;
    if (mac.empty() && !readRouterMac(mac))
    {
        SWSS_LOG_INFO("Route %s has no mac_address and vxlan_router_mac is unset, defer",
                      vxlanRouteInfo.m_routeName.c_str());
        return false;
    }
    mac = normalizeMac(mac);
    if (mac.empty())
    {
        SWSS_LOG_ERROR("Route %s has invalid mac_address %s, not installing in kernel",
                       vxlanRouteInfo.m_routeName.c_str(), vxlanRouteInfo.m_macAddress.c_str());
        return true;
    }

    VxlanKernelRouteInfo info;
    info.m_routeName = vxlanRouteInfo.m_routeName;
    info.m_dstMac = mac;
    info.m_dstIp = vxlanRouteInfo.m_endpoint;
    info.m_srcIp = vtep;
    info.m_vni = vxlanRouteInfo.m_vni.empty() ? vnetInfo.m_vni : vxlanRouteInfo.m_vni;
    info.m_vnetVni = vnetInfo.m_vni;
    info.m_vnet = vxlanRouteInfo.m_vnet;
    info.m_prefix = vxlanRouteInfo.m_prefix;

    if (!linkExists(getVxlanDeviceName(info.m_vnetVni)))
    {
        SWSS_LOG_INFO("%s not present yet, defer kernel route %s",
                      getVxlanDeviceName(info.m_vnetVni).c_str(), info.m_routeName.c_str());
        return false;
    }

    if (!ensureTxDevice(vtep, readBaseVxlanPort()))
    {
        return false;
    }

    installDmacBypassIfNeeded(info.m_vnetVni);

    std::string prevDstMac, prevVnetVni;
    auto kvnetIt = m_kernelRouteTunnelCache.find(info.m_vnet);
    if (kvnetIt != m_kernelRouteTunnelCache.end())
    {
        auto existing = kvnetIt->second.find(info.m_prefix);
        if (existing != kvnetIt->second.end())
        {
            const VxlanKernelRouteInfo & prev = existing->second;
            if (prev.m_vni == info.m_vni &&
                prev.m_vnetVni == info.m_vnetVni &&
                prev.m_dstIp == info.m_dstIp &&
                prev.m_dstMac == info.m_dstMac &&
                prev.m_srcIp == info.m_srcIp)
            {
                return true;
            }
            prevDstMac = prev.m_dstMac;
            prevVnetVni = prev.m_vnetVni;
        }
    }

    if (!neighRef(info.m_dstMac))
    {
        return false;
    }

    std::string res;
    if (cmdCreateKernelRoute(info, res) != RET_SUCCESS)
    {
        SWSS_LOG_ERROR("Kernel route %s add failed: %s", info.m_routeName.c_str(), res.c_str());
        neighUnref(info.m_dstMac);
        return false;
    }

    if (!prevDstMac.empty())
    {
        neighUnref(prevDstMac);
    }
    const bool vniWasRouted = isVniKernelRouted(info.m_vnetVni);
    m_kernelRouteTunnelCache[info.m_vnet][info.m_prefix] = info;
    SWSS_LOG_NOTICE("Create kernel route %s vni %s dst %s mac %s",
                    info.m_routeName.c_str(), info.m_vni.c_str(),
                    info.m_dstIp.c_str(), info.m_dstMac.c_str());
    if (!vniWasRouted)
    {
        installSmacRewrites(info.m_vnetVni);
    }
    if (!prevVnetVni.empty() && prevVnetVni != info.m_vnetVni && !isVniKernelRouted(prevVnetVni))
    {
        removeSmacRewrites(prevVnetVni);
    }
    return true;
}

bool VNetMgr::deleteKernelRoute(const std::string & vnet, const std::string & prefix)
{
    SWSS_LOG_ENTER();
    auto vnetIt = m_kernelRouteTunnelCache.find(vnet);
    if (vnetIt == m_kernelRouteTunnelCache.end())
    {
        return true;
    }
    auto it = vnetIt->second.find(prefix);
    if (it == vnetIt->second.end())
    {
        return true;
    }

    const VxlanKernelRouteInfo info = it->second;
    std::string res;
    cmdDeleteKernelRoute(info.m_prefix, info.m_vnet, res);
    vnetIt->second.erase(it);
    if (vnetIt->second.empty())
    {
        m_kernelRouteTunnelCache.erase(vnetIt);
    }
    neighUnref(info.m_dstMac);
    SWSS_LOG_NOTICE("Delete kernel route %s", info.m_routeName.c_str());
    if (!isVniKernelRouted(info.m_vnetVni))
    {
        removeSmacRewrites(info.m_vnetVni);
    }
    return true;
}

bool VNetMgr::isVniKernelRouted(const std::string & vni)
{
    // All routes of a vnet carry the vnet's vni, so checking one route per vnet is enough.
    for (const auto & kv : m_kernelRouteTunnelCache)
    {
        if (!kv.second.empty() && kv.second.begin()->second.m_vnetVni == vni)
        {
            return true;
        }
    }
    return false;
}

bool VNetMgr::installSmacRewrite(SmacRewriteRule & rule)
{
    std::string res;
    if (cmdInstallSmacRewrite(rule, res) != RET_SUCCESS)
    {
        SWSS_LOG_ERROR("Inner smac rewrite vni %s src %s -> %s failed: %s",
                       rule.m_vni.c_str(), rule.m_srcPrefix.c_str(), rule.m_mac.c_str(), res.c_str());
        return false;
    }
    rule.m_installed = true;
    SWSS_LOG_NOTICE("Installed inner smac rewrite vni %s src %s -> %s on %s",
                    rule.m_vni.c_str(), rule.m_srcPrefix.c_str(), rule.m_mac.c_str(), VNET_VXLAN_TX_DEV);
    return true;
}

void VNetMgr::removeSmacRewrite(SmacRewriteRule & rule)
{
    if (!rule.m_installed)
    {
        return;
    }
    std::string res;
    cmdRemoveSmacRewrite(rule, res);
    rule.m_installed = false;
    SWSS_LOG_NOTICE("Removed inner smac rewrite vni %s src %s",
                    rule.m_vni.c_str(), rule.m_srcPrefix.c_str());
}

void VNetMgr::installSmacRewrites(const std::string & vni)
{
    auto bucket = m_smacRewriteRules.find(vni);
    if (!m_txReady || bucket == m_smacRewriteRules.end())
    {
        return;
    }
    for (auto & kv : bucket->second)
    {
        if (!kv.second.m_installed)
        {
            installSmacRewrite(kv.second);
        }
    }
}

void VNetMgr::removeSmacRewrites(const std::string & vni)
{
    auto bucket = m_smacRewriteRules.find(vni);
    if (bucket == m_smacRewriteRules.end())
    {
        return;
    }
    for (auto & kv : bucket->second)
    {
        removeSmacRewrite(kv.second);
    }
}

bool VNetMgr::doAclRuleTask(const KeyOpFieldsValuesTuple & t)
{
    SWSS_LOG_ENTER();

    const std::string & key = kfvKey(t);

    SmacRewriteRule rule{};
    std::string mac, srcIp, vni, priority;
    if (kfvOp(t) == SET_COMMAND)
    {
        for (const auto & fv : kfvFieldsValues(t))
        {
            if (fvField(fv) == ACL_INNER_SRC_MAC_REWRITE) mac = fvValue(fv);
            else if (fvField(fv) == ACL_INNER_SRC_IP) srcIp = fvValue(fv);
            else if (fvField(fv) == ACL_TUNNEL_VNI) vni = fvValue(fv);
            else if (fvField(fv) == ACL_PRIORITY) priority = fvValue(fv);
        }
    }

    bool valid = !mac.empty();
    if (valid)
    {
        char *end = nullptr;
        unsigned long vniNum = strtoul(vni.c_str(), &end, 10);
        unsigned long prio = priority.empty() ? 0 : strtoul(priority.c_str(), nullptr, 10);
        rule.m_mac = normalizeMac(mac);
        rule.m_vni = std::to_string(vniNum);
        try
        {
            rule.m_srcPrefix = IpPrefix(srcIp).to_string();
        }
        catch (const std::exception &)
        {
        }
        if (rule.m_mac.empty() || rule.m_srcPrefix.empty() || vni.empty() || !end || *end != '\0' ||
            vniNum == 0 || vniNum > 0xFFFFFF)
        {
            SWSS_LOG_ERROR("ACL rule %s has invalid inner smac rewrite fields (ip '%s' mac '%s' vni '%s')",
                           key.c_str(), srcIp.c_str(), mac.c_str(), vni.c_str());
            valid = false;
        }
        else
        {
            prio = std::min<unsigned long>(prio, SMAC_MAX_PRIORITY);
            rule.m_pref = static_cast<unsigned>(2 * (SMAC_MAX_PRIORITY + 1 - prio)) +
                          (isV6(rule.m_srcPrefix) ? 1 : 0);
        }
    }

    auto prevVni = m_smacRuleVni.find(key);
    if (prevVni != m_smacRuleVni.end())
    {
        auto bucket = m_smacRewriteRules.find(prevVni->second);
        SmacRewriteRule & prev = bucket->second.at(key);
        if (valid && prev.m_vni == rule.m_vni && prev.m_srcPrefix == rule.m_srcPrefix &&
            prev.m_mac == rule.m_mac && prev.m_pref == rule.m_pref)
        {
            return true;
        }
        rule.m_handle = prev.m_handle;
        removeSmacRewrite(prev);
        bucket->second.erase(key);
        if (bucket->second.empty())
        {
            m_smacRewriteRules.erase(bucket);
        }
        m_smacRuleVni.erase(prevVni);
    }
    else
    {
        rule.m_handle = m_nextSmacHandle++;
    }

    if (!valid)
    {
        return true;
    }

    rule.m_installed = false;
    SmacRewriteRule & stored = m_smacRewriteRules[rule.m_vni][key];
    stored = rule;
    m_smacRuleVni[key] = rule.m_vni;
    SWSS_LOG_INFO("Inner smac rewrite rule %s: vni %s src %s mac %s pref %u",
                  key.c_str(), rule.m_vni.c_str(), rule.m_srcPrefix.c_str(), rule.m_mac.c_str(), rule.m_pref);
    if (m_txReady && isVniKernelRouted(rule.m_vni))
    {
        installSmacRewrite(stored);
    }
    return true;
}

void VNetMgr::reconcileKernelState()
{
    SWSS_LOG_ENTER();
    std::string res;
    if (!linkExists(VNET_VXLAN_TX_DEV))
    {
        return;
    }

    std::map<std::string, std::set<IpPrefix>> expected;
    for (const auto & vnetKv : m_kernelRouteTunnelCache)
    {
        for (const auto & kv : vnetKv.second)
        {
            try
            {
                expected[vnetKv.first].insert(IpPrefix(kv.first));
            }
            catch (const std::exception &)
            {
            }
        }
    }

    size_t removed = 0;
    for (const auto & vnetKv : m_vnetCache)
    {
        const std::string & vnet = vnetKv.first;
        for (const char *fam : {"-4", "-6"})
        {
            ostringstream cmd;
            cmd << IP_CMD " " << fam << " -o route show vrf " << shellquote(vnet)
                << " dev " VNET_VXLAN_TX_DEV;
            std::string out;
            if (swss::exec(cmd.str(), out) != RET_SUCCESS)
            {
                continue;
            }
            for (const auto & line : tokenize(out, '\n'))
            {
                std::string pfx = line.substr(0, line.find(' '));
                if (pfx.empty())
                {
                    continue;
                }
                if (pfx == "default")
                {
                    pfx = (std::string(fam) == "-4") ? "0.0.0.0/0" : "::/0";
                }
                bool keep = false;
                try
                {
                    keep = expected[vnet].count(IpPrefix(pfx)) > 0;
                }
                catch (const std::exception &)
                {
                }
                if (!keep)
                {
                    SWSS_LOG_NOTICE("Removing stale kernel route %s in %s", pfx.c_str(), vnet.c_str());
                    cmdDeleteKernelRoute(pfx, vnet, res);
                    removed++;
                }
            }
        }
    }

    std::set<std::string> expectedNeigh;
    for (const auto & kv : m_neighRefs)
    {
        expectedNeigh.insert(macToNexthop(kv.first));
    }
    std::string out;
    if (swss::exec(IP_CMD " -6 neigh show dev " VNET_VXLAN_TX_DEV " nud permanent", out) == RET_SUCCESS)
    {
        for (const auto & line : tokenize(out, '\n'))
        {
            std::string ip = line.substr(0, line.find(' '));
            if (ip.empty())
            {
                continue;
            }
            std::string norm = ip;
            try
            {
                norm = IpAddress(ip).to_string();
            }
            catch (const std::exception &)
            {
            }
            if (!expectedNeigh.count(norm))
            {
                SWSS_LOG_NOTICE("Removing stale neighbour %s on %s", ip.c_str(), VNET_VXLAN_TX_DEV);
                cmdDeleteNeighIp(ip, res);
                removed++;
            }
        }
    }

    if (m_kernelRouteTunnelCache.empty() && m_vnetCache.empty())
    {
        teardownTxDevice();
    }
    SWSS_LOG_NOTICE("Kernel state reconciled, removed %zu stale entries", removed);
}

