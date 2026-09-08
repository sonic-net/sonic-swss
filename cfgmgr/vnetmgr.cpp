#include <algorithm>
#include <sstream>
#include <string>
#include "logger.h"
#include "vnetmgr.h"
#include "exec.h"
#include "shellcmd.h"

#define TC_CMD "/sbin/tc"

#define VXLAN_TUNNEL "vxlan_tunnel"
#define MAC_ADDRESS "mac_address"
#define ENDPOINT "endpoint"
#define INSTALL_ON_KERNEL "install_on_kernel"
#define VNI "vni"
#define VNET "vnet"
#define VXLAN_NAME_PREFIX "Vxlan"
#define VXLAN_IF_NAME_PREFIX "Brvxlan"
#define VXLAN_ACCEPT_ALL_INNER_DMACS "vxlan_accept_all_inner_dmacs"
#define RET_SUCCESS 0

using namespace std;
using namespace swss;

static inline std::string getVxlanDeviceName(const std::string & vnetVni)
{
    return std::string(VXLAN_NAME_PREFIX) + vnetVni;
}

static inline std::string bridgeDevNameFor(const std::string & vnetVni)
{
    return std::string(VXLAN_IF_NAME_PREFIX) + vnetVni;
}

static int cmdShowLink(const std::string & dev, std::string & res)
{
    ostringstream cmd;
    cmd << IP_CMD " -o link show dev " << shellquote(dev);
    return swss::exec(cmd.str(), res);
}

static int cmdCreateKernelRoute(const swss::VNetMgr::VxlanKernelRouteInfo & info, std::string & res)
{
    ostringstream cmd;
    cmd << IP_CMD " route replace "
        << shellquote(info.m_prefix)
        << " via " << shellquote(info.m_dstIp)
        << " dev " << shellquote(bridgeDevNameFor(info.m_vnetVni))
        << " vrf " << shellquote(info.m_vnet)
        << " onlink";
    return swss::exec(cmd.str(), res);
}

static int cmdDeleteKernelRoute(const swss::VNetMgr::VxlanKernelRouteInfo & info, std::string & res)
{
    ostringstream cmd;
    cmd << IP_CMD " route del "
        << shellquote(info.m_prefix)
        << " dev " << shellquote(bridgeDevNameFor(info.m_vnetVni))
        << " vrf " << shellquote(info.m_vnet);
    return swss::exec(cmd.str(), res);
}

static int cmdCreateNeighEntry(const swss::VNetMgr::VxlanKernelRouteInfo & info, std::string & res)
{
    ostringstream cmd;
    cmd << IP_CMD " neigh replace "
        << shellquote(info.m_dstIp)
        << " lladdr " << shellquote(info.m_dstMac)
        << " dev " << shellquote(bridgeDevNameFor(info.m_vnetVni))
        << " nud permanent";
    return swss::exec(cmd.str(), res);
}

static int cmdDeleteNeighEntry(const swss::VNetMgr::VxlanKernelRouteInfo & info, std::string & res)
{
    ostringstream cmd;
    cmd << IP_CMD " neigh del "
        << shellquote(info.m_dstIp)
        << " dev " << shellquote(bridgeDevNameFor(info.m_vnetVni));
    return swss::exec(cmd.str(), res);
}

static int cmdCreateFdbEntry(const swss::VNetMgr::VxlanKernelRouteInfo & info, std::string & res)
{
    // Use `bridge fdb replace` (not `append`) so repeated installs do not create dup entries
    ostringstream self;
    self << BRIDGE_CMD " fdb replace " << shellquote(info.m_dstMac)
         << " dev " << shellquote(getVxlanDeviceName(info.m_vnetVni))
         << " dst " << shellquote(info.m_dstIp);
    if (info.m_vni != info.m_vnetVni)
    {
        self << " vni " << shellquote(info.m_vni);
    }
    int r = swss::exec(self.str(), res);
    if (r != RET_SUCCESS) return r;

    ostringstream bridgeEntry;
    bridgeEntry << BRIDGE_CMD " fdb replace " << shellquote(info.m_dstMac)
                << " dev " << shellquote(getVxlanDeviceName(info.m_vnetVni))
                << " master static";
    r = swss::exec(bridgeEntry.str(), res);
    if (r != RET_SUCCESS)
    {
        ostringstream rollback;
        rollback << BRIDGE_CMD " fdb del " << shellquote(info.m_dstMac)
                 << " dev " << shellquote(getVxlanDeviceName(info.m_vnetVni))
                 << " dst " << shellquote(info.m_dstIp);
        std::string rbRes;
        swss::exec(rollback.str(), rbRes);
    }
    return r;
}

static int cmdDeleteFdbEntry(const swss::VNetMgr::VxlanKernelRouteInfo & info, std::string & res)
{
    ostringstream self;
    self << BRIDGE_CMD " fdb del " << shellquote(info.m_dstMac)
         << " dev " << shellquote(getVxlanDeviceName(info.m_vnetVni))
         << " dst " << shellquote(info.m_dstIp);
    int r = swss::exec(self.str(), res);

    ostringstream bridgeEntry;
    bridgeEntry << BRIDGE_CMD " fdb del " << shellquote(info.m_dstMac)
                << " dev " << shellquote(getVxlanDeviceName(info.m_vnetVni))
                << " master";
    swss::exec(bridgeEntry.str(), res);
    return r;
}

static int cmdInstallDmacBypass(const std::string & vxlanDev, const std::string & brmac, std::string & res)
{
    {
        ostringstream cmd;
        cmd << TC_CMD " qdisc replace dev " << shellquote(vxlanDev) << " clsact";
        int r = swss::exec(cmd.str(), res);
        if (r != RET_SUCCESS) return r;
    }
    ostringstream cmd;
    cmd << TC_CMD " filter replace dev " << shellquote(vxlanDev)
        << " ingress matchall action pedit ex munge eth dst set " << shellquote(brmac);
    return swss::exec(cmd.str(), res);
}

static int cmdRemoveDmacBypass(const std::string & vxlanDev, std::string & res)
{
    ostringstream cmd;
    cmd << TC_CMD " qdisc del dev " << shellquote(vxlanDev) << " clsact";
    return swss::exec(cmd.str(), res);
}

VNetMgr::VNetMgr(DBConnector *cfgDb, DBConnector *appDb, const std::vector<std::string> &tables) :
        Orch(cfgDb, tables),
        m_app_db(appDb),
        m_appVnetRouteTable(appDb, APP_VNET_RT_TABLE_NAME),
        m_appVnetRouteTunnelTable(appDb, APP_VNET_RT_TUNNEL_TABLE_NAME),
        m_appSwitchTable(appDb, APP_SWITCH_TABLE_NAME)
{
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

        if (op == SET_COMMAND)
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
        if (op == DEL_COMMAND)
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
    m_vnetRouteTunnelCache[vnet_route_name] = routeInfo;

    try
    {
        if (routeInfo.m_installOnKernel)
        {
            if (!createKernelRoute(routeInfo))
            {
                SWSS_LOG_ERROR("Failed to create kernel route %s", vnet_route_name.c_str());
                return false;
            }
        }
        else
        {
            deleteKernelRoute(routeInfo);
        }
    }
    catch (const std::exception & e)
    {
        SWSS_LOG_ERROR("Kernel install/uninstall for vnet route %s failed: %s",
                       vnet_route_name.c_str(), e.what());
    }

    string vnetRouteTunnelName = kfvKey(t);
    replace(vnetRouteTunnelName.begin(), vnetRouteTunnelName.end(), config_db_key_delimiter, delimiter);

    std::vector<swss::FieldValueTuple> values = kfvFieldsValues(t);
    values.erase(std::remove_if(values.begin(), values.end(),
                [](const swss::FieldValueTuple & fv) { return fv.first == INSTALL_ON_KERNEL; }),
                values.end());

    m_appVnetRouteTunnelTable.set(vnetRouteTunnelName, values);
    return true;
}

bool VNetMgr::doVnetRouteTunnelDeleteTask(const KeyOpFieldsValuesTuple & t)
{
    SWSS_LOG_ENTER();
    const std::string & vnet_route_name = kfvKey(t);
    auto it = m_vnetRouteTunnelCache.find(vnet_route_name);
    if (it == m_vnetRouteTunnelCache.end())
    {
        SWSS_LOG_WARN("Vxlan route tunnel %s hasn't been created", vnet_route_name.c_str());
        return true;
    }
    try
    {
        deleteKernelRoute(it->second);
    }
    catch (const std::exception & e)
    {
        SWSS_LOG_ERROR("Kernel uninstall for vnet route %s failed: %s",
                       vnet_route_name.c_str(), e.what());
    }
    m_vnetRouteTunnelCache.erase(it);
    std::string appKey = vnet_route_name;
    std::replace(appKey.begin(), appKey.end(), config_db_key_delimiter, delimiter);
    m_appVnetRouteTunnelTable.del(appKey);
    return true;
}

bool VNetMgr::probeVxlanBridgePair(const VxlanKernelRouteInfo & info)
{
    std::string res;
    if (cmdShowLink(getVxlanDeviceName(info.m_vnetVni), res) != RET_SUCCESS)
    {
        SWSS_LOG_NOTICE("Netdev %s not yet present; waiting for vxlanmgrd", getVxlanDeviceName(info.m_vnetVni).c_str());
        return false;
    }
    if (cmdShowLink(bridgeDevNameFor(info.m_vnetVni), res) != RET_SUCCESS)
    {
        SWSS_LOG_NOTICE("Bridge %s not yet present; waiting for vxlanmgrd", bridgeDevNameFor(info.m_vnetVni).c_str());
        return false;
    }
    return true;
}

bool VNetMgr::readSwitchState(bool & enabled, std::string & routerMac)
{
    std::vector<swss::FieldValueTuple> values;
    if (!m_appSwitchTable.get("switch", values))
    {
        return false;
    }
    enabled = false;
    routerMac.clear();
    for (const auto & kv : values)
    {
        if (fvField(kv) == VXLAN_ACCEPT_ALL_INNER_DMACS)
        {
            const std::string & v = fvValue(kv);
            enabled = (v == "true" || v == "True" || v == "TRUE" || v == "1");
        }
        else if (fvField(kv) == "vxlan_router_mac")
        {
            routerMac = fvValue(kv);
        }
    }
    return true;
}

bool VNetMgr::installDmacBypassIfNeeded(const std::string & vnetVni)
{
    if (m_dmacBypassInstalledVnis.count(vnetVni))
    {
        return true;
    }
    bool enabled = false;
    std::string routerMac;
    if (!readSwitchState(enabled, routerMac))
    {
        SWSS_LOG_INFO("SWITCH_TABLE:switch not yet populated, deferring kernel route programming");
        return false;
    }
    if (!enabled)
    {
        return true;
    }
    if (routerMac.empty())
    {
        SWSS_LOG_INFO("DMAC bypass enabled but vxlan_router_mac not yet in SWITCH_TABLE, deferring");
        return false;
    }
    const std::string vxlanDev = getVxlanDeviceName(vnetVni);
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

// Shared per-(vnet, MAC) refcount key.
static inline std::string macRefKey(const std::string & vnet, const std::string & mac)
{
    return vnet + "|" + mac;
}

bool VNetMgr::createKernelRoute(const VxlanRouteTunnelInfo & vxlanRouteInfo)
{
    SWSS_LOG_ENTER();

    if (m_vnetCache.find(vxlanRouteInfo.m_vnet) == m_vnetCache.end())
    {
        SWSS_LOG_INFO("Vnet %s hasn't been created yet, defer", vxlanRouteInfo.m_vnet.c_str());
        return false;
    }
    const VnetInfo & vnetInfo = m_vnetCache[vxlanRouteInfo.m_vnet];

    // Fall back to vnet's own VNI when unset.
    std::string effectiveVni = vxlanRouteInfo.m_vni;
    if (effectiveVni.empty())
    {
        effectiveVni = vnetInfo.m_vni;
    }

    VxlanKernelRouteInfo info;
    info.m_routeName = vxlanRouteInfo.m_routeName;
    info.m_dstMac = vxlanRouteInfo.m_macAddress;
    info.m_dstIp = vxlanRouteInfo.m_endpoint;
    info.m_vni = effectiveVni;
    info.m_vnetVni = vnetInfo.m_vni;
    info.m_vnet = vxlanRouteInfo.m_vnet;
    info.m_prefix = vxlanRouteInfo.m_prefix;

    auto existing = m_kernelRouteTunnelCache.find(info.m_routeName);
    bool isUpdate = (existing != m_kernelRouteTunnelCache.end());
    if (isUpdate)
    {
        const VxlanKernelRouteInfo & prev = existing->second;
        if (prev.m_vnetVni == info.m_vnetVni &&
            prev.m_vni == info.m_vni &&
            prev.m_dstIp == info.m_dstIp &&
            prev.m_dstMac == info.m_dstMac &&
            prev.m_prefix == info.m_prefix)
        {
            return true;
        }
    }

    if (!probeVxlanBridgePair(info))
    {
        SWSS_LOG_INFO("Kernel route %s does not have parent vxlan and bridge ready, deferring",
                      info.m_routeName.c_str());
        return false;
    }

    if (!installDmacBypassIfNeeded(info.m_vnetVni))
    {
        SWSS_LOG_INFO("Kernel route %s deferred: vxlan_router_mac not yet available", info.m_routeName.c_str());
        return false;
    }

    const std::string newKey = macRefKey(info.m_vnet, info.m_dstMac);
    auto newRef = m_macRefs.find(newKey);

    std::string res;

    if (isUpdate)
    {
        const VxlanKernelRouteInfo & prev = existing->second;
        SWSS_LOG_NOTICE("Kernel route %s changed, delete+recreate", info.m_routeName.c_str());
        cmdDeleteKernelRoute(prev, res);
        cmdDeleteNeighEntry(prev, res);
        cmdDeleteFdbEntry(prev, res);

        const std::string oldKey = macRefKey(prev.m_vnet, prev.m_dstMac);
        auto oldRef = m_macRefs.find(oldKey);
        if (oldRef != m_macRefs.end() && --(oldRef->second.count) <= 0)
        {
            m_macRefs.erase(oldRef);
        }

        newRef = m_macRefs.find(newKey);
    }

    if (cmdCreateNeighEntry(info, res) != RET_SUCCESS)
    {
        SWSS_LOG_ERROR("Neigh entry add for endpoint %s (%s) failed: %s",
                       info.m_dstIp.c_str(), info.m_routeName.c_str(), res.c_str());
        return false;
    }
    if (cmdCreateFdbEntry(info, res) != RET_SUCCESS)
    {
        SWSS_LOG_ERROR("Fdb add for endpoint %s (%s) failed: %s",
                       info.m_dstIp.c_str(), info.m_routeName.c_str(), res.c_str());
        cmdDeleteNeighEntry(info, res);
        return false;
    }
    if (cmdCreateKernelRoute(info, res) != RET_SUCCESS)
    {
        SWSS_LOG_ERROR("Kernel route %s add failed: %s", info.m_routeName.c_str(), res.c_str());
        cmdDeleteFdbEntry(info, res);
        cmdDeleteNeighEntry(info, res);
        return false;
    }

    if (newRef == m_macRefs.end())
    {
        m_macRefs[newKey] = {1, info.m_dstIp};
    }
    else
    {
        newRef->second.count += 1;
        newRef->second.endpoint = info.m_dstIp; // Latest-update-wins.
    }
    m_kernelRouteTunnelCache[info.m_routeName] = info;
    SWSS_LOG_NOTICE("Create kernel route %s", info.m_routeName.c_str());
    return true;
}

bool VNetMgr::deleteKernelRoute(const VxlanRouteTunnelInfo & vxlanRouteInfo)
{
    SWSS_LOG_ENTER();
    auto it = m_kernelRouteTunnelCache.find(vxlanRouteInfo.m_routeName);
    if (it == m_kernelRouteTunnelCache.end())
    {
        return true;
    }

    const VxlanKernelRouteInfo info = it->second;
    std::string res;

    cmdDeleteKernelRoute(info, res);
    m_kernelRouteTunnelCache.erase(it);

    const std::string refKey = macRefKey(info.m_vnet, info.m_dstMac);
    auto refIt = m_macRefs.find(refKey);
    if (refIt != m_macRefs.end() && --(refIt->second.count) <= 0)
    {
        m_macRefs.erase(refIt);
        cmdDeleteNeighEntry(info, res);
        cmdDeleteFdbEntry(info, res);
    }
    return true;
}
