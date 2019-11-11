#include <algorithm>
#include <regex>
#include <sstream>
#include <string>
#include <net/if.h>

#include "logger.h"
#include "producerstatetable.h"
#include "macaddress.h"
#include "vxlanmgr.h"
#include "exec.h"
#include "tokenize.h"
#include "shellcmd.h"
#include "warm_restart.h"

using namespace std;
using namespace swss;

// Fields name
#define VXLAN_TUNNEL "vxlan_tunnel"
#define SOURCE_IP "src_ip"
#define VNI "vni"
#define VNET "vnet"
#define VXLAN "vxlan"
#define VXLAN_IF "vxlan_if"

#define VXLAN_NAME_PREFIX "Vxlan"
#define VXLAN_IF_NAME_PREFIX "Brvxlan"

static std::string getVxlanName(const swss::VxlanMgr::VxlanInfo & info)
{
    return std::string("") + VXLAN_NAME_PREFIX + info.m_vni;
}

static std::string getVxlanIfName(const swss::VxlanMgr::VxlanInfo & info)
{
    return std::string("") + VXLAN_IF_NAME_PREFIX + info.m_vni;
}

// Commands

#define RET_SUCCESS 0

static int cmdCreateVxlan(const swss::VxlanMgr::VxlanInfo & info, std::string & res)
{
    // ip link add {{VXLAN}} type vxlan id {{VNI}} [local {{SOURCE IP}}] dstport 4789
    ostringstream cmd;
    cmd << IP_CMD " link add "
        << shellquote(info.m_vxlan)
        << " type vxlan id "
        << shellquote(info.m_vni)
        << " ";
    if (!info.m_sourceIp.empty())
    {
        cmd << " local " << shellquote(info.m_sourceIp);
    }
    cmd << " dstport 4789";
    return swss::exec(cmd.str(), res);
}

static int cmdUpVxlan(const swss::VxlanMgr::VxlanInfo & info, std::string & res)
{
    // ip link set dev {{VXLAN}} up
    ostringstream cmd;
    cmd << IP_CMD " link set dev "
        << shellquote(info.m_vxlan)
        << " up";
    return swss::exec(cmd.str(), res);
}

static int cmdCreateVxlanIf(const swss::VxlanMgr::VxlanInfo & info, std::string & res)
{
    // ip link add {{VXLAN_IF}} type bridge
    ostringstream cmd;
    cmd << IP_CMD " link add "
        << shellquote(info.m_vxlanIf)
        << " type bridge";
    return swss::exec(cmd.str(), res);
}

static int cmdAddVxlanIntoVxlanIf(const swss::VxlanMgr::VxlanInfo & info, std::string & res)
{
    // brctl addif {{VXLAN_IF}} {{VXLAN}}
    ostringstream cmd;
    cmd << BRCTL_CMD " addif "
        << shellquote(info.m_vxlanIf)
        << " "
        << shellquote(info.m_vxlan);
    return swss::exec(cmd.str(), res);
}

static int cmdAttachVxlanIfToVnet(const swss::VxlanMgr::VxlanInfo & info, std::string & res)
{
    // ip link set dev {{VXLAN_IF}} master {{VNET}}
    ostringstream cmd;
    cmd << IP_CMD " link set dev "
        << shellquote(info.m_vxlanIf)
        << " master "
        << shellquote(info.m_vnet);
    return swss::exec(cmd.str(), res);
}

static int cmdUpVxlanIf(const swss::VxlanMgr::VxlanInfo & info, std::string & res)
{
    // ip link set dev {{VXLAN_IF}} up
    ostringstream cmd;
    cmd << IP_CMD " link set dev "
        << shellquote(info.m_vxlanIf)
        << " up";
    return swss::exec(cmd.str(), res);
}

static int cmdDeleteVxlan(const swss::VxlanMgr::VxlanInfo & info, std::string & res)
{
    // ip link del dev {{VXLAN}}
    ostringstream cmd;
    cmd << IP_CMD " link del dev "
        << shellquote(info.m_vxlan);
    return swss::exec(cmd.str(), res);
}

static int cmdDeleteVxlanFromVxlanIf(const swss::VxlanMgr::VxlanInfo & info, std::string & res)
{
    // brctl delif {{VXLAN_IF}} {{VXLAN}}
    ostringstream cmd;
    cmd << BRCTL_CMD " delif "
        << shellquote(info.m_vxlanIf)
        << " " 
        << shellquote(info.m_vxlan);
    return swss::exec(cmd.str(), res);
}

static int cmdDeleteVxlanIf(const swss::VxlanMgr::VxlanInfo & info, std::string & res)
{
    // ip link del {{VXLAN_IF}}
    ostringstream cmd;
    cmd << IP_CMD " link del "
        << shellquote(info.m_vxlanIf);
    return swss::exec(cmd.str(), res);
}

static int cmdDetachVxlanIfFromVnet(const swss::VxlanMgr::VxlanInfo & info, std::string & res)
{
    // ip link set dev {{VXLAN_IF}} nomaster
    ostringstream cmd;
    cmd << IP_CMD " link set dev "
        << shellquote(info.m_vxlanIf)
        << " nomaster";
    return swss::exec(cmd.str(), res);
}

// Vxlanmgr

VxlanMgr::VxlanMgr(DBConnector *cfgDb, DBConnector *appDb, DBConnector *stateDb, const vector<std::string> &tables) :
        Orch(cfgDb, tables),
        m_appVxlanTunnelTable(appDb, APP_VXLAN_TUNNEL_TABLE_NAME),
        m_appVxlanTunnelMapTable(appDb, APP_VXLAN_TUNNEL_MAP_TABLE_NAME),
        m_cfgVxlanTunnelTable(cfgDb, CFG_VXLAN_TUNNEL_TABLE_NAME),
        m_cfgVnetTable(cfgDb, CFG_VNET_TABLE_NAME),
        m_stateVrfTable(stateDb, STATE_VRF_TABLE_NAME),
        m_stateVxlanTable(stateDb, STATE_VXLAN_TABLE_NAME)
{
    // Clear old vxlan devices that were created at last time.
    clearAllVxlanDevices();
}

VxlanMgr::~VxlanMgr()
{
    clearAllVxlanDevices();
}

void VxlanMgr::doTask(Consumer &consumer)
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
                task_result = doVxlanCreateTask(t);
            }
            else if (table_name == CFG_VXLAN_TUNNEL_TABLE_NAME)
            {
                task_result = doVxlanTunnelCreateTask(t);
            }
            else if (table_name == CFG_VXLAN_TUNNEL_MAP_TABLE_NAME)
            {
                task_result = doVxlanTunnelMapCreateTask(t);
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
                task_result = doVxlanDeleteTask(t);
            }
            else if (table_name == CFG_VXLAN_TUNNEL_TABLE_NAME)
            {
                task_result = doVxlanTunnelDeleteTask(t);
            }
            else if (table_name == CFG_VXLAN_TUNNEL_MAP_TABLE_NAME)
            {
                task_result = doVxlanTunnelMapDeleteTask(t);
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

bool VxlanMgr::doVxlanCreateTask(const KeyOpFieldsValuesTuple & t)
{
    SWSS_LOG_ENTER();

    VxlanInfo info;
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
        // Suspend this message util the vxlan tunnel is created
        return false;
    }

    // If the VRF(Vnet is a special VRF) has been created
    if (!isVrfStateOk(info.m_vnet))
    {
        SWSS_LOG_DEBUG("Vrf %s has not been created", info.m_vnet.c_str());
        // Suspend this message util the vrf is created
        return false;
    }

    auto sourceIp = std::find_if(
        it->second.begin(),
        it->second.end(),
        [](const FieldValueTuple & fvt){ return fvt.first == SOURCE_IP; });
    if (sourceIp  != it->second.end())
    {
        info.m_sourceIp = sourceIp->second;
    }
    info.m_vxlan = getVxlanName(info);
    info.m_vxlanIf = getVxlanIfName(info);

    // If this vxlan has been created
    if (isVxlanStateOk(info.m_vxlan))
    {
        // Because the vxlan has been create, so this message is to update 
        // the information of vxlan. 
        // This program just delete the old vxlan and create a new one
        // according to this message.
        doVxlanDeleteTask(t);
    }

    if (!createVxlan(info))
    {
        SWSS_LOG_ERROR("Cannot create vxlan %s", info.m_vxlan.c_str());
        return true;
    }

    m_vnetCache[info.m_vnet] = info;
    SWSS_LOG_INFO("Create vxlan %s", info.m_vxlan.c_str());

    return true;
}

bool VxlanMgr::doVxlanDeleteTask(const KeyOpFieldsValuesTuple & t)
{
    SWSS_LOG_ENTER();

    const std::string & vnetName = kfvKey(t);
    auto it = m_vnetCache.find(vnetName);
    if (it == m_vnetCache.end())
    {
        SWSS_LOG_WARN("Vxlan(Vnet %s) hasn't been created ", vnetName.c_str());
        return true;
    }

    const VxlanInfo & info = it->second;
    if (isVxlanStateOk(info.m_vxlan))
    {
        if ( ! deleteVxlan(info))
        {
            SWSS_LOG_ERROR("Cannot delete vxlan %s", info.m_vxlan.c_str());
            return false;
        }
    }
    else
    {
        SWSS_LOG_WARN("Vxlan %s hasn't been created ", info.m_vxlan.c_str());
    }

    m_vnetCache.erase(it);
    SWSS_LOG_INFO("Delete vxlan %s", info.m_vxlan.c_str());
    return true;

}

bool VxlanMgr::doVxlanTunnelCreateTask(const KeyOpFieldsValuesTuple & t)
{
    SWSS_LOG_ENTER();

    const std::string & vxlanTunnelName = kfvKey(t);
    m_appVxlanTunnelTable.set(vxlanTunnelName, kfvFieldsValues(t));
    
    // Update vxlan tunnel cache
    m_vxlanTunnelCache[vxlanTunnelName] = kfvFieldsValues(t);

    SWSS_LOG_INFO("Create vxlan tunnel %s", vxlanTunnelName.c_str());
    return true;
}

bool VxlanMgr::doVxlanTunnelDeleteTask(const KeyOpFieldsValuesTuple & t)
{
    SWSS_LOG_ENTER();

    const std::string & vxlanTunnelName = kfvKey(t);
    m_appVxlanTunnelTable.del(vxlanTunnelName);

    auto it = m_vxlanTunnelCache.find(vxlanTunnelName);
    if (it != m_vxlanTunnelCache.end())
    {
        m_vxlanTunnelCache.erase(it);
    }

    SWSS_LOG_INFO("Delete vxlan tunnel %s", vxlanTunnelName.c_str());
    return true;
}

bool VxlanMgr::doVxlanTunnelMapCreateTask(const KeyOpFieldsValuesTuple & t)
{
    SWSS_LOG_ENTER();

    std::string vxlanTunnelMapName = kfvKey(t);
    std::replace(vxlanTunnelMapName.begin(), vxlanTunnelMapName.end(), config_db_key_delimiter, delimiter);
    m_appVxlanTunnelMapTable.set(vxlanTunnelMapName, kfvFieldsValues(t));

    SWSS_LOG_NOTICE("Create vxlan tunnel map %s", vxlanTunnelMapName.c_str());
    return true;
}

bool VxlanMgr::doVxlanTunnelMapDeleteTask(const KeyOpFieldsValuesTuple & t)
{
    SWSS_LOG_ENTER();

    std::string vxlanTunnelMapName = kfvKey(t);
    std::replace(vxlanTunnelMapName.begin(), vxlanTunnelMapName.end(), config_db_key_delimiter, delimiter);
    m_appVxlanTunnelMapTable.del(vxlanTunnelMapName);

    SWSS_LOG_NOTICE("Delete vxlan tunnel map %s", vxlanTunnelMapName.c_str());
    return true;
}

bool VxlanMgr::isVrfStateOk(const std::string & vrfName)
{
    SWSS_LOG_ENTER();

    std::vector<FieldValueTuple> temp;

    if (m_stateVrfTable.get(vrfName, temp))
    {
        SWSS_LOG_DEBUG("Vrf %s is ready", vrfName.c_str());
        return true;
    }
    SWSS_LOG_DEBUG("Vrf %s is not ready", vrfName.c_str());
    return false;
}

bool VxlanMgr::isVxlanStateOk(const std::string & vxlanName)
{
    SWSS_LOG_ENTER();
    std::vector<FieldValueTuple> temp;

    if (m_stateVxlanTable.get(vxlanName, temp))
    {
        SWSS_LOG_DEBUG("Vxlan %s is ready", vxlanName.c_str());
        return true;
    }
    SWSS_LOG_DEBUG("Vxlan %s is not ready", vxlanName.c_str());
    return false;
}

bool VxlanMgr::createVxlan(const VxlanInfo & info)
{
    SWSS_LOG_ENTER();
    
    std::string res;
    int ret = 0;

    // Create Vxlan
    ret = cmdCreateVxlan(info, res);
    if (ret != RET_SUCCESS)
    {
        SWSS_LOG_WARN(
            "Failed to create vxlan %s (vni: %s, source ip %s)",
            info.m_vxlan.c_str(),
            info.m_vni.c_str(),
            info.m_sourceIp.c_str());
        return false;
    }

    // Up Vxlan
    ret = cmdUpVxlan(info, res);
    if (ret != RET_SUCCESS)
    {
        cmdDeleteVxlan(info, res);
        SWSS_LOG_WARN(
            "Fail to up vxlan %s",
            info.m_vxlan.c_str());
        return false;
    }

    // Create Vxlan Interface
    ret = cmdCreateVxlanIf(info, res);
    if (ret != RET_SUCCESS)
    {
        cmdDeleteVxlan(info, res);
        SWSS_LOG_WARN(
            "Fail to create vxlan interface %s",
            info.m_vxlanIf.c_str());
        return false;
    }

    // Add vxlan into vxlan interface
    ret = cmdAddVxlanIntoVxlanIf(info, res);
    if ( ret != RET_SUCCESS )
    {
        cmdDeleteVxlanIf(info, res);
        cmdDeleteVxlan(info, res);
        SWSS_LOG_WARN(
            "Fail to add %s into %s",
            info.m_vxlan.c_str(),
            info.m_vxlanIf.c_str());
        return false;
    }

    // Attach vxlan interface to vnet
    ret = cmdAttachVxlanIfToVnet(info, res);
    if ( ret != RET_SUCCESS )
    {
        cmdDeleteVxlanFromVxlanIf(info, res);
        cmdDeleteVxlanIf(info, res);
        cmdDeleteVxlan(info, res);
        SWSS_LOG_WARN(
            "Fail to set %s master %s",
            info.m_vxlanIf.c_str(),
            info.m_vnet.c_str());
        return false;
       
    }

    // Up Vxlan Interface
    ret = cmdUpVxlanIf(info, res);
    if ( ret != RET_SUCCESS )
    {
        cmdDetachVxlanIfFromVnet(info, res);
        cmdDeleteVxlanFromVxlanIf(info, res);
        cmdDeleteVxlanIf(info, res);
        cmdDeleteVxlan(info, res);
        SWSS_LOG_WARN(
            "Fail to up bridge %s",
            info.m_vxlanIf.c_str());
        return false;
    }

    std::vector<FieldValueTuple> fvVector;
    fvVector.emplace_back("state", "ok");
    m_stateVxlanTable.set(info.m_vxlan, fvVector);

    return true;
}

bool VxlanMgr::deleteVxlan(const VxlanInfo & info)
{
    SWSS_LOG_ENTER();

    std::string res;

    cmdDetachVxlanIfFromVnet(info, res);
    cmdDeleteVxlanFromVxlanIf(info, res);
    cmdDeleteVxlanIf(info, res);
    cmdDeleteVxlan(info, res);

    m_stateVxlanTable.del(info.m_vxlan);

    return true;
}

void VxlanMgr::clearAllVxlanDevices()
{
    std::string stdout;
    const std::string cmd = std::string("") + IP_CMD + " link";
    int ret = swss::exec(cmd, stdout);
    if (ret != 0)
    {
        SWSS_LOG_ERROR("Cannot get devices by command : %s", cmd.c_str());
        return;
    }
    std::regex device_name_pattern("^\\d+:\\s+([^:]+)");
    std::smatch match_result;
    auto lines = tokenize(stdout, '\n');
    for (const std::string & line : lines)
    {
        if (!std::regex_search(line, match_result, device_name_pattern))
        {
            continue;
        }
        std::string res;
        std::string device_name = match_result[1];
        VxlanInfo info;
        if (device_name.find(VXLAN_NAME_PREFIX) == 0)
        {
            info.m_vxlan = device_name;
            cmdDeleteVxlan(info, res);
        }
        else if (device_name.find(VXLAN_IF_NAME_PREFIX) == 0)
        {
            info.m_vxlanIf = device_name;
            cmdDeleteVxlanIf(info, res);
        }
    }
}
