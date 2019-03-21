#include <string>
#include <functional>
#include <list>
#include <map>
#include <string.h>
#include <regex>

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

// Need be moved to swss-commom
#define VXLAN_IF_NAME_PREFIX "brvxlan"

// Fields name
#define SOURCE_IP "src_ip"
#define VXLAN_TUNNEL "vxlan_tunnel"
#define VXLAN "vxlan"
#define VNI "vni"
#define BRIDGE "bridge"
#define VNET "vnet"

// Commands format macro
#define CMD_CREATE_VXLAN IP_CMD " link add {{" VXLAN "}} type vxlan id {{" VNI "}} local {{" SOURCE_IP "}} dstport 4789"
#define CMD_UP_VXLAN IP_CMD " link set dev {{" VXLAN "}} up"
#define CMD_CREATE_BRIDGE IP_CMD " link add {{" BRIDGE "}} type bridge"
#define CMD_ADD_VXLAN_INTO_BRIDGE BRCTL_CMD " addif {{" BRIDGE "}} {{" VXLAN "}}"
#define CMD_ATTACH_BRIDGE_TO_VNET IP_CMD " link set dev {{" BRIDGE "}} master {{" VNET "}}"
#define CMD_UP_BRIDGE IP_CMD " link set dev {{" BRIDGE "}} up"

#define CMD_DELETE_VXLAN IP_CMD " link del dev {{" VXLAN "}}"
#define CMD_DELETE_VXLAN_FROM_BRIDGE BRCTL_CMD " delif {{" BRIDGE "}} {{" VXLAN "}}"
#define CMD_DELETE_BRIDGE IP_CMD " link del {{" BRIDGE "}}"
#define CMD_DETACH_BRIDGE_FROM_VXLAN IP_CMD " link set dev {{" BRIDGE "}} nomaster"

static std::string getVxlanName(const swss::VxlanMgr::VxlanInfo & info)
{
    return info.at(VXLAN_TUNNEL) + info.at(VNI);
}

static std::string getBridgeName(const swss::VxlanMgr::VxlanInfo & info)
{
    return std::string("") + VXLAN_IF_NAME_PREFIX
                   // IFNAMSIZ include '\0', so need minus one
                   + info.at(VXLAN).substr(0, IFNAMSIZ - strlen(VXLAN_IF_NAME_PREFIX) - 1);
}

VxlanMgr::VxlanMgr(DBConnector *cfgDb, DBConnector *appDb, DBConnector *stateDb, const vector<std::string> &tables) :
        Orch(cfgDb, tables),
        m_appVxlanTunnelTableProducer(appDb, APP_VXLAN_TUNNEL_TABLE_NAME),
        m_cfgVxlanTunnelTable(cfgDb, CFG_VXLAN_TUNNEL_TABLE_NAME),
        m_cfgVnetTable(cfgDb, CFG_VNET_TABLE_NAME),
        m_stateVrfTable(stateDb, STATE_VRF_TABLE_NAME),
        m_stateVxlanTable(stateDb, STATE_VXLAN_TABLE_NAME)
{
}

void VxlanMgr::doTask(Consumer &consumer)
{
    SWSS_LOG_ENTER();

    const string & table_name = consumer.getTableName();
    try
    {
        if (table_name == CFG_VNET_TABLE_NAME)
        {
            doVnetTableTask(consumer);
        }
        else
        {
            SWSS_LOG_ERROR("Unknown config table %s ", table_name.c_str());
            throw runtime_error("VxlanMgr doTask failure.");
        }
    }
    catch(std::out_of_range e)
    {
        SWSS_LOG_ERROR("Internal error : %s", e.what());
        throw e;
    }
}

void VxlanMgr::doVnetTableTask(Consumer &consumer)
{
    SWSS_LOG_ENTER();

    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
        auto t = it->second;
        const std::string & op = kfvOp(t);
        if (op == SET_COMMAND)
        {
            if ( ! doVxlanCreateTask(t))
            {
                ++it;
                continue;
            }
        }
        else if (op == DEL_COMMAND)
        {
            if (! doVxlanDeleteTask(t))
            {
                ++it;
                continue;
            }
        }
        else
        {
            SWSS_LOG_ERROR("Unknown command %s ", op.c_str());
            throw std::runtime_error("Unknown command " + op);
        }
        it = consumer.m_toSync.erase(it);
    }
}

bool VxlanMgr::doVxlanCreateTask(const KeyOpFieldsValuesTuple & t)
{
    SWSS_LOG_ENTER();

    const std::string & vnetName = kfvKey(t);
    const std::string & op = kfvOp(t);
    for (auto i : kfvFieldsValues(t))
    {
        const std::string & field = fvField(i);
        if (field == VXLAN_TUNNEL)
        {
            if (op == SET_COMMAND)
            {
                // If the VRF(Vnet is a special VRF) has been created
                if (! isVrfStateOk(vnetName))
                {
                    return false;
                }
                VxlanInfo info;
                if (! getVxlanInfo(vnetName, info))
                {
                    return false;
                }
                // If the VXLAN has been created
                if ( isVxlanStateOk(info[VXLAN_TUNNEL]))
                {
                    SWSS_LOG_WARN("Vxlan %s isn't created ", info[VXLAN_TUNNEL].c_str());
                    return true;
                }
                createVxlan(info);

                m_vnetVxlanInfoMapping[info.at(VNET)] = info;
                return true;
            }
            else
            {
                SWSS_LOG_ERROR("Unknown command %s ", op.c_str());
                throw std::runtime_error("Unknown command " + op);
            }
        }
    }
    return true;
}

bool VxlanMgr::doVxlanDeleteTask(const KeyOpFieldsValuesTuple & t)
{
    SWSS_LOG_ENTER();

    const std::string & vnetName = kfvKey(t);
    const std::string & op = kfvOp(t);

    if (op == DEL_COMMAND)
    {
        auto it = m_vnetVxlanInfoMapping.find(vnetName);
        if ( it == m_vnetVxlanInfoMapping.end() || ! isVxlanStateOk(it->second.at(VXLAN)))
        {
            SWSS_LOG_WARN("Vxlan %s isn't created ", it->second.at(VXLAN).c_str());
            return true;
        }

        deleteVxlan(it->second);

        m_vnetVxlanInfoMapping.erase(it);
    }
    else
    {
        SWSS_LOG_ERROR("Unknown command %s ", op.c_str());
        throw std::runtime_error("Unknown command " + op);
    }
    return true;
}

bool VxlanMgr::isVrfStateOk(const std::string & vrfName)
{
    SWSS_LOG_ENTER();

    std::vector<FieldValueTuple> temp;

    if (m_stateVrfTable.get(vrfName, temp))
    {
        SWSS_LOG_INFO("Vrf %s is ready", vrfName.c_str());
        return true;
    }
    return false;
}

bool VxlanMgr::isVxlanStateOk(const std::string & vxlanName)
{
    SWSS_LOG_ENTER();
    std::vector<FieldValueTuple> temp;

    if (m_stateVxlanTable.get(vxlanName, temp))
    {
        SWSS_LOG_DEBUG("%s is ready", vxlanName.c_str());
        return true;
    }

    SWSS_LOG_DEBUG("%s is not ready", vxlanName.c_str());
    return false;
}

bool VxlanMgr::getVxlanInfo(const std::string & vnetName, VxlanInfo & info)
{
    SWSS_LOG_ENTER();

    std::vector<FieldValueTuple> temp;
    if (! m_cfgVnetTable.get(vnetName, temp))
    {
        SWSS_LOG_ERROR("Vnet %s is missing.", vnetName.c_str());
        return false;
    }
    info[VNET] = vnetName;

    auto it = std::find_if(
        temp.begin(),
        temp.end(),
        [](const FieldValueTuple &fvt) { return fvt.first == VXLAN_TUNNEL; });
    if (it == temp.end())
    {
        SWSS_LOG_WARN("vxlan_tunnel is missing in vnet %s.", vnetName.c_str());
        return false;
    }
    info[VXLAN_TUNNEL] = it->second;

    it = std::find_if(
        temp.begin(),
        temp.end(),
        [](const FieldValueTuple &fvt) { return fvt.first == VNI; });
    if (it == temp.end())
    {
        SWSS_LOG_WARN("vni is missing in vnet %s.", vnetName.c_str());
        return false;
    }
    info[VNI] = it->second;
    info[VXLAN] = getVxlanName(info);
    info[BRIDGE] = getBridgeName(info);

    if (! m_cfgVxlanTunnelTable.get(info[VXLAN_TUNNEL] , temp))
    {
        SWSS_LOG_WARN("vxlan_tunnel %s is missing.", info[VXLAN_TUNNEL].c_str());
        return false;
    }
    it = std::find_if(
        temp.begin(),
        temp.end(),
        [](const FieldValueTuple &fvt) { return fvt.first == SOURCE_IP; });
    if (it == temp.end())
    {
        SWSS_LOG_WARN("src_ip is missing in vxlan_tunnel %s.", info[VXLAN_TUNNEL].c_str());
        return false;
    }
    info[SOURCE_IP] = it->second;

    SWSS_LOG_DEBUG("Get VxlanInfo [vxlan: %s, src_ip : %s, vnet : %s]",
        info[VXLAN].c_str(), info[SOURCE_IP].c_str(), info[VNET].c_str());
    return true;
}

#define RET_SUCCESS 0

static int execCommand(const std::string & format, const swss::VxlanMgr::VxlanInfo & info, std::string & res)
{
    SWSS_LOG_ENTER();

	std::string command = format;
    // Extract the {{args}} from format
	std::regex argPattern("\\{\\{([\\s\\w]+)\\}\\}");
	std::smatch result;
    // Replace the {{args}} by the values in vxlaninfo
	while (std::regex_search(command, result, argPattern))
	{
		command = std::string(result.prefix()) + info.at(result[1]) + std::string(result.suffix());
	}

    return swss::exec(std::string() + BASH_CMD + " -c \"" + command + "\"", res);
}

void VxlanMgr::createVxlan(const VxlanInfo & info)
{
    SWSS_LOG_ENTER();
    
    std::string res;
    int ret = 0;

    ret = execCommand( CMD_CREATE_VXLAN, info, res);
    // Create Vxlan
    if ( ret != RET_SUCCESS )
    {
        SWSS_LOG_WARN(
            "Failed to create vxlan %s (vni: %s, source ip %s)",
            info.at(VXLAN).c_str(),
            info.at(VNI).c_str(),
            info.at(SOURCE_IP).c_str());
        return ;
    }

    // Up Vxlan
    ret = execCommand(CMD_UP_VXLAN, info, res);
    if ( ret != RET_SUCCESS )
    {
        execCommand(CMD_DELETE_VXLAN, info, res);
        SWSS_LOG_WARN(
            "Fail to up vxlan %s",
            info.at(VXLAN).c_str());
        return ;
    }

    // Create bridge
    ret = execCommand(CMD_CREATE_BRIDGE, info, res);
    if ( ret != RET_SUCCESS )
    {
        execCommand(CMD_DELETE_VXLAN, info, res);
        SWSS_LOG_WARN(
            "Fail to create bridge %s",
            info.at(BRIDGE).c_str());
        return ;
    }

    // Add vxlan into bridge
    ret = execCommand(CMD_ADD_VXLAN_INTO_BRIDGE, info, res);
    if ( ret != RET_SUCCESS )
    {
        execCommand(CMD_DELETE_BRIDGE, info, res);
        execCommand(CMD_DELETE_VXLAN, info, res);
        SWSS_LOG_WARN(
            "Fail to add %s into %s",
            info.at(VXLAN).c_str(),
            info.at(BRIDGE).c_str());
        return ;
    }

    // Attach bridge to vnet
    ret = execCommand(CMD_ATTACH_BRIDGE_TO_VNET, info, res);
    if ( ret != RET_SUCCESS )
    {
        execCommand(CMD_DELETE_VXLAN_FROM_BRIDGE, info, res);
        execCommand(CMD_DELETE_BRIDGE, info, res);
        execCommand(CMD_DELETE_VXLAN, info, res);
        SWSS_LOG_WARN(
            "Fail to set %s master %s",
            info.at(BRIDGE).c_str(),
            info.at(VNET).c_str());
        return ;
       
    }

    // Up bridge
    ret = execCommand(CMD_UP_BRIDGE, info, res);
    if ( ret != RET_SUCCESS )
    {
        execCommand(CMD_DETACH_BRIDGE_FROM_VXLAN, info, res);
        execCommand(CMD_DELETE_VXLAN_FROM_BRIDGE, info, res);
        execCommand(CMD_DELETE_BRIDGE, info, res);
        execCommand(CMD_DELETE_VXLAN, info, res);
        SWSS_LOG_WARN(
            "Fail to up bridge %s",
            info.at(BRIDGE).c_str());
        return;
    }

    std::vector<FieldValueTuple> fvVector;
    if (! m_cfgVxlanTunnelTable.get(info.at(VXLAN_TUNNEL) , fvVector))
    {
        execCommand(CMD_DETACH_BRIDGE_FROM_VXLAN, info, res);
        execCommand(CMD_DELETE_VXLAN_FROM_BRIDGE, info, res);
        execCommand(CMD_DELETE_BRIDGE, info, res);
        execCommand(CMD_DELETE_VXLAN, info, res);
        SWSS_LOG_ERROR("Vxlan tunnel %s was deleted during creating.", info.at(VXLAN_TUNNEL).c_str());
        return ;
    }
    // If the first refers vxlan tunnel
    if (m_vxlanTunnelReferenceCount[info.at(VXLAN_TUNNEL)]++ == 0)
    {
        m_appVxlanTunnelTableProducer.set(info.at(VXLAN_TUNNEL), fvVector);
    }

    fvVector.clear();
    fvVector.emplace_back("state", "ok");
    m_stateVxlanTable.set(info.at(VXLAN), fvVector);

    SWSS_LOG_NOTICE("Create vxlan %s", info.at(VXLAN).c_str());
}
#include <iostream>
void VxlanMgr::deleteVxlan(const VxlanInfo & info)
{
    SWSS_LOG_ENTER();

    std::string res;

    execCommand(CMD_DETACH_BRIDGE_FROM_VXLAN, info, res);
    execCommand(CMD_DELETE_VXLAN_FROM_BRIDGE, info, res);
    execCommand(CMD_DELETE_BRIDGE, info, res);
    execCommand(CMD_DELETE_VXLAN, info, res);
    // If the last refers vxlan tunnel
    if (--m_vxlanTunnelReferenceCount[info.at(VXLAN_TUNNEL)] <= 0)
    {
        m_appVxlanTunnelTableProducer.del(info.at(VXLAN_TUNNEL));
        m_vxlanTunnelReferenceCount.erase(info.at(VXLAN_TUNNEL));
    }

    m_stateVxlanTable.del(info.at(VXLAN));

    SWSS_LOG_NOTICE("Delete vxlan %s", info.at(VXLAN).c_str());

}



