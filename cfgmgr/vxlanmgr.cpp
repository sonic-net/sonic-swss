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
#define VNI "vni"
#define BRIDGE "bridge"
#define VNET "vnet"

// Commands format macro
#define CMD_CREATE_VXLAN IP_CMD " link add {{" VXLAN_TUNNEL "}} type vxlan id {{" VNI "}} local {{" SOURCE_IP "}} dstport 4789"
#define CMD_UP_VXLAN IP_CMD " link set dev {{" VXLAN_TUNNEL "}} up"
#define CMD_CREATE_BRIDGE IP_CMD " link add {{" BRIDGE "}} type bridge"
#define CMD_ADD_VXLAN_INTO_BRIDGE BRCTL_CMD " addif {{" BRIDGE "}} {{" VXLAN_TUNNEL "}}"
#define CMD_ATTACH_BRIDGE_TO_VNET IP_CMD " link set dev {{" BRIDGE "}} master {{" VNET "}}"
#define CMD_UP_BRIDGE IP_CMD " link set dev {{" BRIDGE "}} up"

#define CMD_DELETE_VXLAN IP_CMD " link del dev {{" VXLAN_TUNNEL "}}"
#define CMD_DELETE_VXLAN_FROM_BRIDGE BRCTL_CMD " delif {{" BRIDGE "}} {{" VXLAN_TUNNEL "}}"
#define CMD_DELETE_BRIDGE IP_CMD " link del {{" BRIDGE "}}"
#define CMD_DETACH_BRIDGE_FROM_VXLAN IP_CMD " link set dev {{" BRIDGE "}} nomaster"

VxlanMgr::VxlanMgr(DBConnector *cfgDb, DBConnector *appDb, DBConnector *stateDb, const vector<std::string> &tables) :
        Orch(cfgDb, tables),
        m_appVxlanTableProducer(appDb, APP_VXLAN_TUNNEL_TABLE_NAME),
        m_cfgVxlanTunnelTable(cfgDb, CFG_VXLAN_TUNNEL_TABLE_NAME),
        m_cfgVnetTable(cfgDb, CFG_VNET_TABLE_NAME),
        m_stateVrfTable(stateDb, STATE_VRF_TABLE_NAME)
{
}

void VxlanMgr::doTask(Consumer &consumer)
{
    SWSS_LOG_ENTER();

    const string & table_name = consumer.getTableName();
    if (table_name == CFG_VNET_TABLE_NAME)
    {
        doVnetTask(consumer);
    }
    else
    {
        SWSS_LOG_ERROR("Unknown config table %s ", table_name.c_str());
        throw runtime_error("VxlanMgr doTask failure.");
    }
}

void VxlanMgr::doVnetTask(Consumer &consumer)
{
    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
        auto t = it->second;
        const std::string & op = kfvOp(t);
        const std::string & vnetName = kfvKey(t);
        for (auto i : kfvFieldsValues(t))
        {
            const std::string & field = fvField(i);
            const std::string & value = fvValue(i);
            if (field == VXLAN_TUNNEL)
            {
                if (op == SET_COMMAND)
                {
                    // If the VRF(Vnet is a special VRF) has been created
                    if (! isVrfStateOk(vnetName))
                    {
                        ++it;
                        continue;
                    }
                    VxlanInfo info;
                    if (! getVxlanInfo(vnetName, info))
                    {
                        ++it;
                        continue;
                    }
                    createVxlan(info);
                }
                else if (op == DEL_COMMAND)
                {
                    VxlanInfo info;
                    info[VNET] = vnetName;
                    info[VXLAN_TUNNEL] = value;
                    info[BRIDGE] = std::string("") + VXLAN_IF_NAME_PREFIX
                                        // IFNAMSIZ include '\0', so need minus one
                                        + value.substr(0, IFNAMSIZ - strlen(VXLAN_IF_NAME_PREFIX) - 1);
                    deleteVxlan(info);
                }
                else
                {
                    SWSS_LOG_ERROR("Unknown command %s ", op.c_str());
                }
            }
            it = consumer.m_toSync.erase(it);
        }
    }
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
    info[BRIDGE] = std::string("") + VXLAN_IF_NAME_PREFIX
                     // IFNAMSIZ include '\0', so need minus one
                     + it->second.substr(0, IFNAMSIZ - strlen(VXLAN_IF_NAME_PREFIX) - 1);

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

    if (! m_cfgVxlanTunnelTable.get(info[VXLAN_TUNNEL] , temp))
    {
        SWSS_LOG_WARN("vxlan_tunnel %s is missing.", info[VXLAN_TUNNEL].c_str());
        return false;
    }
    it = std::find_if(
        temp.begin(),
        temp.end(),
        [](const FieldValueTuple &fvt) { return fvt.first == "src_ip"; });
    if (it == temp.end())
    {
        SWSS_LOG_WARN("src_ip is missing in vxlan_tunnel %s.", info[VXLAN_TUNNEL].c_str());
        return false;
    }
    info["src_ip"] = it->second;

    SWSS_LOG_DEBUG("Get VxlanInfo [vxlan_tunnel : %s, src_ip : %s, vni : %s, vnet : %s]",
        info[VXLAN_TUNNEL].c_str(), info["src_ip"].c_str(), info[VNI].c_str(), info[VNET].c_str());
    return true;
}

static bool execCommand(const std::string & format, const swss::VxlanMgr::VxlanInfo & info, std::string & res)
{
	std::string command = format;
    // Extract the {{args}} from format
	std::regex argPattern("\\{\\{([\\s\\w]+)\\}\\}");
	std::smatch result;
    // Replace the {{args}} by the values in vxlaninfo
	while (std::regex_search(command, result, argPattern))
	{
		auto it = info.find(result[1]);
		if (it == info.end())
		{
    		return false;
		}
		command = std::string(result.prefix()) + it->second + std::string(result.suffix());
	}
    return swss::exec(std::string() + BASH_CMD + " -c \"" + command + "\"", res) == 0;
}

void VxlanMgr::createVxlan(VxlanInfo & info)
{
    SWSS_LOG_ENTER();
    
    std::string res;

    // Create Vxlan
    if ( ! execCommand(
        CMD_CREATE_VXLAN,
        info,
        res))
    {
        SWSS_LOG_WARN(
            "Failed to create vxlan %s (vni: %s, source ip %s)",
            info[VXLAN_TUNNEL].c_str(),
            info[VNI].c_str(),
            info["src_ip"].c_str());
        return ;
    }

    // Up Vxlan
    if ( ! execCommand(CMD_UP_VXLAN, info, res))
    {
        execCommand(CMD_DELETE_VXLAN, info, res);
        SWSS_LOG_WARN(
            "Fail to up vxlan %s",
            info[VXLAN_TUNNEL].c_str());
        return ;
    }

    // Create bridge
    if ( ! execCommand(CMD_CREATE_BRIDGE, info, res))
    {
        execCommand(CMD_DELETE_VXLAN, info, res);
        SWSS_LOG_WARN(
            "Fail to create bridge %s",
            info[BRIDGE].c_str());
        return ;
    }

    // Add vxlan into bridge
    if ( ! execCommand(CMD_ADD_VXLAN_INTO_BRIDGE, info, res))
    {
        execCommand(CMD_DELETE_BRIDGE, info, res);
        execCommand(CMD_DELETE_VXLAN, info, res);
        SWSS_LOG_WARN(
            "Fail to add %s into %s",
            info[VXLAN_TUNNEL].c_str(),
            info[BRIDGE].c_str());
        return ;
    }

    // Attach bridge to vnet
    if ( ! execCommand(CMD_ATTACH_BRIDGE_TO_VNET, info, res))
    {
        execCommand(CMD_DELETE_VXLAN_FROM_BRIDGE, info, res);
        execCommand(CMD_DELETE_BRIDGE, info, res);
        execCommand(CMD_DELETE_VXLAN, info, res);
        SWSS_LOG_WARN(
            "Fail to set %s master %s",
            info[BRIDGE].c_str(),
            info[VNET].c_str());
        return ;
       
    }

    // Up bridge
    if ( ! execCommand(CMD_UP_BRIDGE, info, res))
    {
        execCommand(CMD_DETACH_BRIDGE_FROM_VXLAN, info, res);
        execCommand(CMD_DELETE_VXLAN_FROM_BRIDGE, info, res);
        execCommand(CMD_DELETE_BRIDGE, info, res);
        execCommand(CMD_DELETE_VXLAN, info, res);
        SWSS_LOG_WARN(
            "Fail to up bridge %s",
            info[BRIDGE].c_str());
        return;
    }

    std::vector<FieldValueTuple> fvVector;
    fvVector.emplace_back(SOURCE_IP, info[SOURCE_IP]);
    m_appVxlanTableProducer.set(info[VXLAN_TUNNEL], fvVector);

    SWSS_LOG_NOTICE("Create vxlan %s", info[VXLAN_TUNNEL].c_str());

}

void VxlanMgr::deleteVxlan(VxlanInfo & info)
{
    SWSS_LOG_ENTER();

    std::string res;

    execCommand(CMD_DETACH_BRIDGE_FROM_VXLAN, info, res);
    execCommand(CMD_DELETE_VXLAN_FROM_BRIDGE, info, res);
    execCommand(CMD_DELETE_BRIDGE, info, res);
    execCommand(CMD_DELETE_VXLAN, info, res);

    m_appVxlanTableProducer.del(info[VXLAN_TUNNEL]);

}



