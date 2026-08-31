#include <algorithm>
#include <sstream>
#include <string>
#include "logger.h"
#include "vnetmgr.h"
#include "exec.h"
#include "shellcmd.h"

#define VXLAN_TUNNEL "vxlan_tunnel"
#define MAC_ADDRESS "mac_address"
#define ENDPOINT "endpoint"
#define INSTALL_ON_KERNEL "install_on_kernel"
#define VNI "vni"
#define VNET "vnet"
#define VXLAN_NAME_PREFIX "Vxlan"
#define VXLAN_IF_NAME_PREFIX "Brvxlan"
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
    cmd << IP_CMD " route add "
        << shellquote(info.m_prefix)
        << " dev " << shellquote(bridgeDevNameFor(info.m_vnetVni))
        << " vrf " << shellquote(info.m_vnet);
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

static bool shouldAddNeighEntry(const std::string & prefix, std::string & address)
{
    size_t slashPos = prefix.find('/');
    if (slashPos == std::string::npos)
    {
        return false;
    }
    address = prefix.substr(0, slashPos);
    int prefixLen = std::stoi(prefix.substr(slashPos + 1));
    if (prefix.find('.') != std::string::npos)
    {
        return prefixLen == 32;
    }
    if (prefix.find(':') != std::string::npos)
    {
        return prefixLen == 128;
    }
    return false;
}

static int cmdCreateNeighEntry(const swss::VNetMgr::VxlanKernelRouteInfo & info, std::string & res)
{
    std::string address;
    if (!shouldAddNeighEntry(info.m_prefix, address))
    {
        return RET_SUCCESS;
    }
    ostringstream cmd;
    cmd << IP_CMD " neigh add "
        << shellquote(address)
        << " lladdr " << shellquote(info.m_dstMac)
        << " dev " << shellquote(bridgeDevNameFor(info.m_vnetVni));
    return swss::exec(cmd.str(), res);
}

static int cmdDeleteNeighEntry(const swss::VNetMgr::VxlanKernelRouteInfo & info, std::string & res)
{
    std::string address;
    if (!shouldAddNeighEntry(info.m_prefix, address))
    {
        return RET_SUCCESS;
    }
    ostringstream cmd;
    cmd << IP_CMD " neigh del "
        << shellquote(address)
        << " dev " << shellquote(bridgeDevNameFor(info.m_vnetVni));
    return swss::exec(cmd.str(), res);
}

static int cmdCreateFdbEntry(const swss::VNetMgr::VxlanKernelRouteInfo & info, std::string & res)
{
    ostringstream self;
    self << BRIDGE_CMD " fdb append " << shellquote(info.m_dstMac)
         << " dev " << shellquote(getVxlanDeviceName(info.m_vnetVni))
         << " dst " << shellquote(info.m_dstIp);
    if (info.m_vni != info.m_vnetVni)
    {
        self << " vni " << shellquote(info.m_vni);
    }
    int r = swss::exec(self.str(), res);
    if (r != RET_SUCCESS) return r;

    ostringstream bridgeEntry;
    bridgeEntry << BRIDGE_CMD " fdb append " << shellquote(info.m_dstMac)
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
    VxlanRouteTunnelInfo routeInfo;
    routeInfo.m_endpoint = "NULL";
    routeInfo.m_macAddress = "NULL";
    routeInfo.m_vni = "NULL";
    routeInfo.m_installOnKernel = false;

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
            std::string _addr;
            if (!shouldAddNeighEntry(routeInfo.m_prefix, _addr))
            {
                SWSS_LOG_ERROR("Skipping kernel install for non-host tunnel route %s"
                               " in vnet %s: bridge/FDB path requires a host prefix",
                               routeInfo.m_prefix.c_str(), routeInfo.m_vnet.c_str());
            }
            else if (!createKernelRoute(routeInfo))
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

bool VNetMgr::createKernelRoute(const VxlanRouteTunnelInfo & vxlanRouteInfo)
{
    SWSS_LOG_ENTER();

    if (m_vnetCache.find(vxlanRouteInfo.m_vnet) == m_vnetCache.end())
    {
        SWSS_LOG_INFO("Vnet %s hasn't been created yet, defer", vxlanRouteInfo.m_vnet.c_str());
        return false;
    }
    const VnetInfo & vnetInfo = m_vnetCache[vxlanRouteInfo.m_vnet];

    auto existing = m_kernelRouteTunnelCache.find(vxlanRouteInfo.m_routeName);
    if (existing != m_kernelRouteTunnelCache.end())
    {
        const VxlanKernelRouteInfo & prev = existing->second;
        if (prev.m_vnetVni == vnetInfo.m_vni &&
            prev.m_vni == vxlanRouteInfo.m_vni &&
            prev.m_dstIp == vxlanRouteInfo.m_endpoint &&
            prev.m_dstMac == vxlanRouteInfo.m_macAddress &&
            prev.m_prefix == vxlanRouteInfo.m_prefix)
        {
            return true;
        }
        SWSS_LOG_NOTICE("Kernel route %s changed, delete and recreate with new info", vxlanRouteInfo.m_routeName.c_str());
        deleteKernelRoute(vxlanRouteInfo);
    }

    VxlanKernelRouteInfo info;
    info.m_routeName = vxlanRouteInfo.m_routeName;
    info.m_dstMac = vxlanRouteInfo.m_macAddress;
    info.m_dstIp = vxlanRouteInfo.m_endpoint;
    info.m_vni = vxlanRouteInfo.m_vni;        // encap VNI for FDB override
    info.m_vnetVni = vnetInfo.m_vni;          // vnet's own VNI selects Vxlan<vnetVni>
    info.m_vnet = vxlanRouteInfo.m_vnet;
    info.m_prefix = vxlanRouteInfo.m_prefix;

    if (!probeVxlanBridgePair(info))
    {
        SWSS_LOG_INFO("Kernel route %s does not have parent vxlan and bridge ready, deferring", vxlanRouteInfo.m_routeName.c_str());
        return false;
    }

    std::string res;
    if (cmdCreateKernelRoute(info, res) != RET_SUCCESS)
    {
        SWSS_LOG_ERROR("Kernel route %s add failed: %s", info.m_routeName.c_str(), res.c_str());
        return false;
    }
    if (cmdCreateNeighEntry(info, res) != RET_SUCCESS)
    {
        SWSS_LOG_ERROR("Neigh entry add for %s failed: %s", info.m_routeName.c_str(), res.c_str());
        cmdDeleteKernelRoute(info, res);
        return false;
    }
    if (cmdCreateFdbEntry(info, res) != RET_SUCCESS)
    {
        SWSS_LOG_ERROR("Fdb add for %s failed: %s", info.m_routeName.c_str(), res.c_str());
        cmdDeleteNeighEntry(info, res);
        cmdDeleteKernelRoute(info, res);
        return false;
    }

    m_kernelRouteTunnelCache[vxlanRouteInfo.m_routeName] = info;
    SWSS_LOG_NOTICE("Create kernel route %s", vxlanRouteInfo.m_routeName.c_str());
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

    cmdDeleteFdbEntry(info, res);
    cmdDeleteNeighEntry(info, res);
    cmdDeleteKernelRoute(info, res);

    m_kernelRouteTunnelCache.erase(it);
    return true;
}
