#include <unistd.h>
#include <algorithm>
#include <regex>
#include <sstream>
#include <string>
#include <net/if.h>

#include "logger.h"
#include "producerstatetable.h"
#include "macaddress.h"
#include "vnetmgr.h"
#include "exec.h"
#include "tokenize.h"
#include "shellcmd.h"
#include "warm_restart.h"
#include <swss/logger.h>

using namespace std;
using namespace swss;

// Fields name
#define VXLAN_TUNNEL "vxlan_tunnel"
#define SOURCE_IP "src_ip"
#define SOURCE_MAC "src_mac"
#define MAC_ADDRESS "mac_address"
#define ENDPOINT "endpoint"
#define INSTALL_ON_KERNEL "install_on_kernel"
#define VNI "vni"
#define VNET "vnet"
#define VXLAN "vxlan"
#define VXLAN_NAME_PREFIX "Vxlan"
#define VXLAN_SRC_PORT "vxlan_sport"
#define SWITCH "switch"

#define RET_SUCCESS 0

// Commands

static int cmdCreateVxlan(const swss::VNetMgr::VxlanKernelRouteInfo & info, std::string & res)
{
    // ip link add {{VXLAN}} [address {{SOURCE MAC}}] type vxlan id {{VNI}} [local {{SOURCE IP}}] [remote {{DEST IP}}] dstport 4789
    ostringstream cmd;
    cmd << IP_CMD " link add "
        << shellquote(info.m_vxlanDevName);

    if (!info.m_srcMac.empty())
    {
        cmd << " address " << shellquote(info.m_srcMac);
    }

    cmd << " type vxlan id "
        << shellquote(info.m_vni);

    if (!info.m_srcIp.empty())
    {
        cmd << " local " << shellquote(info.m_srcIp);
    }

    if (!info.m_dstIp.empty())
    {
        cmd << " remote " << shellquote(info.m_dstIp);
    }
    cmd << " dstport " << shellquote(info.m_vxlanSrcUdpPort);
    return swss::exec(cmd.str(), res);
}

static int cmdDeleteVxlan(const swss::VNetMgr::VxlanKernelRouteInfo & info, std::string & res)
{
    // ip link del {{VXLAN}}
    ostringstream cmd;
    cmd << IP_CMD " link del "
        << shellquote(info.m_vxlanDevName);
    return swss::exec(cmd.str(), res);
}

static int cmdUpVxlan(const swss::VNetMgr::VxlanKernelRouteInfo & info, std::string & res)
{
    // ip link set dev {{VXLAN}} up
    ostringstream cmd;
    cmd << IP_CMD " link set dev "
        << shellquote(info.m_vxlanDevName)
        << " up";
    return swss::exec(cmd.str(), res);
}

static int cmdAttachVxlanIfToVnet(const swss::VNetMgr::VxlanKernelRouteInfo & info, std::string & res)
{
    // ip link set dev {{VXLAN_IF}} vrf {{VNET}}
    ostringstream cmd;
    cmd << IP_CMD " link set dev "
        << shellquote(info.m_vxlanDevName)
        << " vrf "
        << shellquote(info.m_vnet);
    return swss::exec(cmd.str(), res);
}

static int cmdCreateKernelRoute(const swss::VNetMgr::VxlanKernelRouteInfo & info, std::string & res)
{
    // ip route add {{PREFIX}} dev {{VXLAN_IF}} vrf {{VNET}}
    ostringstream cmd;
    cmd << IP_CMD " route add "
        << shellquote(info.m_prefix)
        << " dev "
        << shellquote(info.m_vxlanDevName)
        << " vrf "
        << shellquote(info.m_vnet);
    return swss::exec(cmd.str(), res);
}

static bool shouldAddStaticMacEntry(const std::string prefix, std::string &address) {
    size_t slashPos = prefix.find('/');
    if (slashPos == std::string::npos) {
        return false;
    }
    
    address = prefix.substr(0, slashPos);
    int prefixLen = std::stoi(prefix.substr(slashPos + 1));

    // Add a static MAC entry only for /32 IPv4 or /128 IPv6 prefix
    if (prefix.find('.') != std::string::npos) {
        return prefixLen == 32;
    }
    else if (prefix.find(':') != std::string::npos) {
        return prefixLen == 128;
    }

    return false;
}

static int cmdCreateStaticMacEntry(const swss::VNetMgr::VxlanKernelRouteInfo & info, std::string & res)
{
    std::string address;
    if (!shouldAddStaticMacEntry(info.m_prefix, address)) {
        return RET_SUCCESS;
    }

    // ip neigh add {{PREFIX}} lladdr {{DEST MAC}} dev {{VXLAN_IF}}
    ostringstream cmd;
    cmd << IP_CMD " neigh add "
        << shellquote(address)
        << " lladdr "
        << shellquote(info.m_dstMac)
        << " dev "
        << shellquote(info.m_vxlanDevName);
    return swss::exec(cmd.str(), res);
}

VNetMgr::VNetMgr(DBConnector *cfgDb, DBConnector *appDb, const std::vector<std::string> &tables) :
        m_app_db(appDb),
        Orch(cfgDb, tables),
        m_appVnetRouteTable(appDb, APP_VNET_RT_TABLE_NAME),
        m_appVnetRouteTunnelTable(appDb, APP_VNET_RT_TUNNEL_TABLE_NAME),
        m_appSwitchTable(appDb, APP_SWITCH_TABLE_NAME)
{
    getAllVxlanNetDevices();
}

std::vector<std::string> VNetMgr::parseNetDev(const std::string& stdout){
    std::vector<std::string> netdevs;
    std::regex device_name_pattern("^\\d+:\\s+([^:]+)");
    std::smatch match_result;
    auto lines = tokenize(stdout, '\n');
    for (const std::string & line : lines)
    {
        SWSS_LOG_NOTICE("line : %s\n",line.c_str());
        if (!std::regex_search(line, match_result, device_name_pattern))
        {
            continue;
        }
        std::string dev_name = match_result[1];
        netdevs.push_back(dev_name);
    }
    return netdevs;
}

void VNetMgr::getAllVxlanNetDevices()
{
    std::string stdout;

    // Get VxLan Netdev Interfaces
    std::string cmd = std::string("") + IP_CMD + " link show type vxlan";
    int ret = swss::exec(cmd, stdout);
    if (ret != 0)
    {
        SWSS_LOG_ERROR("Cannot get vxlan devices by command : %s", cmd.c_str());
        stdout.clear();
    }
    std::vector<std::string> netdevs = parseNetDev(stdout);
    for (auto netdev : netdevs)
    {
        SWSS_LOG_NOTICE("Found vxlan device: %s", netdev.c_str());
        m_vxlanNetDevices[netdev] = VXLAN;
    }

    return;
}

std::string VNetMgr::getVxlanSourcePort()
{
    std::vector<FieldValueTuple> temp;

    if (m_appSwitchTable.get(SWITCH, temp))
    {
        auto itr = std::find_if(
            temp.begin(),
            temp.end(),
            [](const FieldValueTuple &fvt) { return fvt.first == VXLAN_SRC_PORT; });
        if (itr != temp.end() && !(itr->second.empty()))
        {
            SWSS_LOG_DEBUG("Using Vxlan source port %s", itr->second.c_str());
            return itr->second;
        }
    }
    
    SWSS_LOG_DEBUG("Using default Vxlan source port: 4789");
    return "4789"; // default port
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
            else if (table_name == CFG_VXLAN_TUNNEL_TABLE_NAME)
            {
                task_result = doVxlanTunnelCreateTask(t);
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
            else if (table_name == CFG_VXLAN_TUNNEL_TABLE_NAME)
            {
                task_result = doVxlanTunnelDeleteTask(t);
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
    info.m_vnet = kfvKey(t);
    for (auto i : kfvFieldsValues(t))
    {
        const std::string & field = fvField(i);
        const std::string & value = fvValue(i);
        if (field == VXLAN_TUNNEL)
        {
            info.m_vxlanTunnel = value;
        }
        else if (field == VNI)
        {
            info.m_vni = value;
        }
        else if (field == SOURCE_MAC)
        {
            info.m_macAddress = value;
        }
    }

    // If all information of vnet has been set
    if (info.m_vxlanTunnel.empty() 
     || info.m_vni.empty())
    {
        SWSS_LOG_DEBUG("Vnet %s information is incomplete", info.m_vnet.c_str());
        // if the information is incomplete, just ignore this message
        // because all information will be sent if the information was
        // completely set.
        return true;
    }

    // If the vxlan tunnel has been created
    auto it = m_vxlanTunnelCache.find(info.m_vxlanTunnel);
    if (it == m_vxlanTunnelCache.end())
    {
        SWSS_LOG_DEBUG("Vxlan tunnel %s has not been created", info.m_vxlanTunnel.c_str());
        // Suspend this message until the vxlan tunnel is created
        return false;
    }

    info.m_sourceIp = it->second.m_sourceIp;
    m_vnetCache[info.m_vnet] = info;

    std::string vxlan_dev_name = VXLAN_NAME_PREFIX + info.m_vni;
    m_vxlanNetDevices[vxlan_dev_name] = VXLAN;

    SWSS_LOG_NOTICE("Create VNET %s, vni: %s, src_ip: %s, src_mac: %s", info.m_vnet.c_str(), info.m_vni.c_str(), info.m_sourceIp.c_str(), info.m_macAddress.c_str());

    return true;
}

bool VNetMgr::doVnetDeleteTask(const KeyOpFieldsValuesTuple & t)
{
    SWSS_LOG_ENTER();

    const std::string & vnetName = kfvKey(t);

    auto it = m_vnetCache.find(vnetName);
    if (it == m_vnetCache.end())
    {
        SWSS_LOG_WARN("Vxlan(Vnet %s) hasn't been created ", vnetName.c_str());
        return true;
    }

    std::string vxlan_dev_name = VXLAN_NAME_PREFIX + it->second.m_vni;
    auto dev = m_vxlanNetDevices.find(vxlan_dev_name);
    if (dev != m_vxlanNetDevices.end())
    {
        m_vxlanNetDevices.erase(dev);
    }

    m_vnetCache.erase(it);

    SWSS_LOG_INFO("Delete vxlan %s", vnetName.c_str());

    return true;
}

bool VNetMgr::doVxlanTunnelCreateTask(const KeyOpFieldsValuesTuple & t)
{
    SWSS_LOG_ENTER();

    const std::string & vxlanTunnelName = kfvKey(t);
    
    // Update vxlan tunnel cache
    TunCache tuncache;

    tuncache.fvt = kfvFieldsValues(t);
    tuncache.m_sourceIp = "NULL";

    for (auto i : kfvFieldsValues(t))
    {
        const std::string & field = fvField(i);
        const std::string & value = fvValue(i);
        if (field == SOURCE_IP)
        {
            tuncache.m_sourceIp = value;
        }
    }

    m_vxlanTunnelCache[vxlanTunnelName] = tuncache;

    SWSS_LOG_NOTICE("Create vxlan tunnel %s, src_ip: %s", vxlanTunnelName.c_str(), tuncache.m_sourceIp.c_str());
    return true;
}

bool VNetMgr::doVxlanTunnelDeleteTask(const KeyOpFieldsValuesTuple & t)
{
    SWSS_LOG_ENTER();

    const std::string & vxlanTunnelName = kfvKey(t);

    auto it = m_vxlanTunnelCache.find(vxlanTunnelName);
    if (it == m_vxlanTunnelCache.end())
    {
        SWSS_LOG_WARN("Vxlan tunnel %s hasn't been created ", vxlanTunnelName.c_str());
        return true;
    }

    m_vxlanTunnelCache.erase(it);

    SWSS_LOG_INFO("Delete vxlan tunnel %s", vxlanTunnelName.c_str());

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
        SWSS_LOG_INFO("Create vnet route %s", vnetRouteName.c_str());
    }
    else if (op == DEL_COMMAND)
    {
        m_appVnetRouteTable.del(vnetRouteName);
        SWSS_LOG_INFO("Delete vnet route %s", vnetRouteName.c_str());
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

    size_t delimiter_pos;
    delimiter_pos = vnet_route_name.find_first_of(config_db_key_delimiter);
    routeInfo.m_vnet = vnet_route_name.substr(0, delimiter_pos);
    routeInfo.m_prefix = vnet_route_name.substr(delimiter_pos + 1);

    for (auto i : kfvFieldsValues(t))
    {
        const std::string & field = fvField(i);
        const std::string & value = fvValue(i);
        if (field == ENDPOINT)
        {
            routeInfo.m_endpoint = value;
        }
        else if (field == MAC_ADDRESS)
        {
            routeInfo.m_macAddress = value;
        }
        else if (field == VNI)
        {
            routeInfo.m_vni = value;
        }
        else if (field == INSTALL_ON_KERNEL)
        {
            routeInfo.m_installOnKernel = (value == "true");
        }
    }

    SWSS_LOG_NOTICE("Create vxlan tunnel route for vnet %s and prefix %s, dst_ip: %s, dst_mac: %s, vni: %s", routeInfo.m_vnet.c_str(), routeInfo.m_prefix.c_str(), routeInfo.m_endpoint.c_str(), routeInfo.m_macAddress.c_str(), routeInfo.m_vni.c_str());

    routeInfo.m_routeName = vnet_route_name;
    m_vnetRouteTunnelCache[vnet_route_name] = routeInfo;

    if (routeInfo.m_installOnKernel)
    {
        if (!createKernelRoute(routeInfo))
        {
            SWSS_LOG_ERROR("Failed to create kernel route for vxlan route %s", vnet_route_name.c_str());
            return false;
        }
    }
    else
    {
        // Remove any existing kernel route if present
        SWSS_LOG_NOTICE("Install on kernel is false. Deleting kernel interface for route %s", vnet_route_name.c_str());
        deleteKernelRoute(routeInfo);
    }

    string vnetRouteTunnelName = kfvKey(t);
    replace(vnetRouteTunnelName.begin(), vnetRouteTunnelName.end(), config_db_key_delimiter, delimiter);
    
    std::vector<swss::FieldValueTuple> values = const_cast<std::vector<swss::FieldValueTuple>&>(kfvFieldsValues(t));
    
    // if values contains INSTALL_ON_KERNEL, remove only this value
    values.erase(std::remove_if(values.begin(), values.end(),
                [](const swss::FieldValueTuple & fv) {
                    return fv.first == INSTALL_ON_KERNEL;
                }), values.end());

    m_appVnetRouteTunnelTable.set(vnetRouteTunnelName, values);

    SWSS_LOG_NOTICE("Create vxlan tunnel route %s", vnetRouteTunnelName.c_str());
    return true;
}

bool VNetMgr::doVnetRouteTunnelDeleteTask(const KeyOpFieldsValuesTuple & t)
{
    SWSS_LOG_ENTER();

    const std::string & vnet_route_name = kfvKey(t);

    auto it = m_vnetRouteTunnelCache.find(vnet_route_name);
    if (it == m_vnetRouteTunnelCache.end())
    {
        SWSS_LOG_WARN("Vxlan route tunnel %s hasn't been created ", vnet_route_name.c_str());
        return true;
    }

    deleteKernelRoute(it->second);

    m_vnetRouteTunnelCache.erase(it);
    m_appVnetRouteTunnelTable.del(vnet_route_name);

    SWSS_LOG_INFO("Delete vxlan route tunnel %s", vnet_route_name.c_str());

    return true;
}

bool VNetMgr::createKernelRoute(const VxlanRouteTunnelInfo & vxlanRouteInfo)
{
    SWSS_LOG_ENTER();

    if (m_vnetCache.find(vxlanRouteInfo.m_vnet) == m_vnetCache.end())
    {
        SWSS_LOG_WARN("Vnet %s hasn't been created ", vxlanRouteInfo.m_vnet.c_str());
        return false;
    }

    VnetInfo vnetInfo = m_vnetCache[vxlanRouteInfo.m_vnet];

    if (vnetInfo.m_vni == vxlanRouteInfo.m_vni)
    {
        SWSS_LOG_DEBUG("Skipping kernel routes since Vnet %s VNI %s match route VNI %s", 
                        vxlanRouteInfo.m_vnet.c_str(),
                        vnetInfo.m_vni.c_str(),
                        vxlanRouteInfo.m_vni.c_str());

        return false;
    }

    std::string vxlanDevName = VXLAN_NAME_PREFIX + vxlanRouteInfo.m_vni;

    auto it = m_vxlanNetDevices.find(vxlanDevName);
    if (it != m_vxlanNetDevices.end())
    {
        SWSS_LOG_INFO("Vxlan device %s already present", it->first.c_str());
        //return false;
    }

    VxlanKernelRouteInfo vxlanKernelRouteInfo;
    vxlanKernelRouteInfo.m_routeName = vxlanRouteInfo.m_routeName;
    vxlanKernelRouteInfo.m_dstMac = vxlanRouteInfo.m_macAddress;
    vxlanKernelRouteInfo.m_dstIp = vxlanRouteInfo.m_endpoint;
    vxlanKernelRouteInfo.m_vni = vxlanRouteInfo.m_vni;
    vxlanKernelRouteInfo.m_vnet = vxlanRouteInfo.m_vnet;
    vxlanKernelRouteInfo.m_prefix = vxlanRouteInfo.m_prefix;
    vxlanKernelRouteInfo.m_srcIp = vnetInfo.m_sourceIp;
    vxlanKernelRouteInfo.m_srcMac = vnetInfo.m_macAddress;
    vxlanKernelRouteInfo.m_vxlanDevName = vxlanDevName;
    vxlanKernelRouteInfo.m_vxlanSrcUdpPort = getVxlanSourcePort();

    // Create Vxlan Device
    std::string res;
    int ret = cmdCreateVxlan(vxlanKernelRouteInfo, res);
    if (ret != RET_SUCCESS)
    {
        SWSS_LOG_ERROR("Vxlan device %s creation failed: %s", 
                        vxlanDevName.c_str(), res.c_str());
        return false;
    }

    // Attach Vxlan Device to Vnet
    ret = cmdAttachVxlanIfToVnet(vxlanKernelRouteInfo, res);
    if (ret != RET_SUCCESS)
    {
        SWSS_LOG_ERROR("Vxlan device %s failed to attach to vnet: %s", 
                        vxlanDevName.c_str(), res.c_str());
        return false;
    }

    // Bring up Vxlan Device
    ret = cmdUpVxlan(vxlanKernelRouteInfo, res);
    if (ret != RET_SUCCESS)
    {
        SWSS_LOG_ERROR("Vxlan device %s up failed: %s", 
                        vxlanDevName.c_str(), res.c_str());
        return false;
    }

    // Create Kernel Route
    ret = cmdCreateKernelRoute(vxlanKernelRouteInfo, res);
    if (ret != RET_SUCCESS)
    {
        SWSS_LOG_ERROR("Kernel route %s creation failed: %s", 
                        vxlanKernelRouteInfo.m_routeName.c_str(), res.c_str());
        return false;
    }

    // Create Static MAC Entry
    ret = cmdCreateStaticMacEntry(vxlanKernelRouteInfo, res);
    if (ret != RET_SUCCESS)
    {
        SWSS_LOG_ERROR("Static MAC entry for route %s creation failed: %s", 
                        vxlanKernelRouteInfo.m_routeName.c_str(), res.c_str());
        return false;
    }

    m_kernelRouteTunnelCache[vxlanRouteInfo.m_routeName] = vxlanKernelRouteInfo;
    m_vxlanNetDevices[vxlanDevName] = VXLAN;

    SWSS_LOG_NOTICE("Create kernel route %s", vxlanRouteInfo.m_routeName.c_str());
    return true;
}

bool VNetMgr::deleteKernelRoute(const VxlanRouteTunnelInfo & vxlanRouteInfo)
{
    SWSS_LOG_ENTER();
    auto it = m_kernelRouteTunnelCache.find(vxlanRouteInfo.m_routeName);
    if (it == m_kernelRouteTunnelCache.end())
    {
        SWSS_LOG_INFO("Vxlan route %s does not exists", vxlanRouteInfo.m_routeName.c_str());
        return true;
    }
    
    std::string vxlanDevName = VXLAN_NAME_PREFIX + vxlanRouteInfo.m_vni;
    std::string res;
    int ret = cmdDeleteVxlan(it->second, res);
    if (ret != RET_SUCCESS)
    {
        SWSS_LOG_ERROR("Vxlan device %s deletion failed: %s", 
                        vxlanDevName.c_str(), res.c_str());
        return false;
    }

    m_kernelRouteTunnelCache.erase(it);

    auto dev = m_vxlanNetDevices.find(vxlanDevName);
    if (dev != m_vxlanNetDevices.end())
    {
        m_vxlanNetDevices.erase(dev);
    }

    return true;
}