#include <string>
#include <netinet/in.h>
#include <netlink/route/link.h>
#include <netlink/route/neighbour.h>
#include <netlink/route/link/vxlan.h>
#include <arpa/inet.h>

#include "logger.h"
#include "dbconnector.h"
#include "producerstatetable.h"
#include "ipaddress.h"
#include "netmsg.h"
#include "macaddress.h"
#include "exec.h"
#include "shellcmd.h"
#include "fdbsync.h"
#include "warm_restart.h"
#include "errno.h"

using namespace std;
using namespace swss;

#define VXLAN_BR_IF_NAME_PREFIX    "Brvxlan"

FdbSync::FdbSync(RedisPipeline *pipelineAppDB, DBConnector *stateDb) :
    m_fdbTable(pipelineAppDB, APP_VXLAN_FDB_TABLE_NAME),
    m_imetTable(pipelineAppDB, APP_EVPN_REMOTE_VNI_TABLE_NAME),
    m_stateFdbRestoreTable(stateDb, STATE_EVPN_FDB_RESTORE_TABLE_NAME),
    m_fdbStateTable(stateDb, STATE_FDB_TABLE_NAME)
{
    m_AppRestartAssist = new AppRestartAssist(pipelineAppDB, "fdbsyncd", "swss", DEFAULT_FDBSYNC_WARMSTART_TIMER);
    if (m_AppRestartAssist)
    {
        m_AppRestartAssist->registerAppTable(APP_VXLAN_FDB_TABLE_NAME, &m_fdbTable);
        m_AppRestartAssist->registerAppTable(APP_EVPN_REMOTE_VNI_TABLE_NAME, &m_imetTable);
    }
}

FdbSync::~FdbSync()
{
    if (m_AppRestartAssist)
    {
        delete m_AppRestartAssist;
    }
}

// Check if neighbor table is restored in kernel
bool FdbSync::isFdbRestoreDone()
{
    string value;

    m_stateFdbRestoreTable.hget("Flags", "restored", value);
    if (value == "true")
    {
        SWSS_LOG_NOTICE("neighbor table restore to kernel is done");
        return true;
    }
    return true;
}

void FdbSync::processStateFdb()
{
    struct fdb_info info;
    std::deque<KeyOpFieldsValuesTuple> entries;
    
    m_fdbStateTable.pops(entries);

    int count =0 ;
    for (auto entry: entries)
    {
        memset(&info, 0, sizeof(struct fdb_info));
        count++;
        std::string key = kfvKey(entry);
        std::string op = kfvOp(entry);

        std::size_t delimiter = key.find_first_of(":");
        auto vlan_name = key.substr(0, delimiter);
        auto mac_address = key.substr(delimiter+1);

        info.vid = (unsigned int) stoi(vlan_name.substr(4));
        memcpy(info.mac, mac_address.c_str(),mac_address.length());

        if(op == "SET")
            info.op_type = FDB_OPER_ADD ;
        else
            info.op_type = FDB_OPER_DEL ;

        SWSS_LOG_NOTICE("FDBSYNCD STATE FDB updates key=%s, operation=%s\n", key.c_str(), op.c_str());
        
        for (auto i : kfvFieldsValues(entry))
        {
            SWSS_LOG_INFO(" FDBSYNCD STATE FDB updates : "
            "FvFiels %s, FvValues: %s \n", fvField(i).c_str(), fvValue(i).c_str());

            if(fvField(i) == "port")
            {
                memcpy(info.port_name, fvValue(i).c_str(), fvValue(i).length());
            }

            if(fvField(i) == "type")
            {
                if(fvValue(i) == "dynamic")
                    info.type = FDB_TYPE_DYNAMIC;
                else if (fvValue(i) == "static")
                    info.type = FDB_TYPE_STATIC;
            }
        }
        macUpdateStateDB(&info);
    }
}

void FdbSync::macAddSrcDB(struct fdb_info *info)
{
    string key = "Vlan" + to_string(info->vid) + ":" + info->mac;
    fdbmac[key].port_name = info->port_name;
    fdbmac[key].type      = info->type;

    return;
}

bool FdbSync::macCheckSrcDB(struct fdb_info *info)
{
    string key = "Vlan" + to_string(info->vid) + ":" + info->mac;
    if (fdbmac.find(key) != fdbmac.end())
    {
        SWSS_LOG_NOTICE("DEL_KEY %s ", key.c_str());
        return true;
    }

    return false;
}

void FdbSync::macDelVxlanEntry(string auxkey, struct fdb_info *info)
{
    std::string vtep = m_mac[auxkey].vtep;

    const std::string cmds = std::string("")
        + " bridge fdb del " + info->mac + " dev " 
        + m_mac[auxkey].ifname + " dst " + vtep + " vlan " + to_string(info->vid);

    std::string res;
    int ret = swss::exec(cmds, res);
    if (ret != 0)
    {
        SWSS_LOG_INFO("Failed cmd:%s, res=%s, ret=%d", cmds.c_str(), res.c_str(), ret);
    }

    SWSS_LOG_NOTICE("Success cmd:%s, res=%s, ret=%d", cmds.c_str(), res.c_str(), ret);

    return;
}

void FdbSync::macUpdateStateDB(struct fdb_info *info)
{
    char *op;
    char *type;
    string port_name = "";
    string key = "Vlan" + to_string(info->vid) + ":" + info->mac;
	short fdb_type;    /*dynamic or static*/

    if (info->op_type == FDB_OPER_ADD)
    {
        macAddSrcDB(info);
        op = "add";
        port_name+= info->port_name;
        fdb_type = info->type;
        /* Check if this vlan+key is also learned by vxlan neighbor then delete learned on */
        if (m_mac.find(key) != m_mac.end())
        {
            op = "replace";
            macDelVxlanEntry(key, info);
            SWSS_LOG_INFO("Local learn event deleting from VXLAN table DEL_KEY %s", key.c_str());
            macDelVxlan(key);
        }
    }
    else
    {
        op = "del";
        if (macCheckSrcDB(info) == false)
        {
            return;
        }
		port_name+= fdbmac[key].port_name;
		fdb_type = fdbmac[key].type;
		fdbmac.erase(key);
    }

    if (fdb_type == FDB_TYPE_DYNAMIC)
    {
        type = "dynamic";
    }
    else
    {
        type = "static";
    }

    const std::string cmds = std::string("")
        + " bridge fdb " + op + " " + info->mac + " dev " 
        + port_name + " master " + type + " vlan " + to_string(info->vid);

    std::string res;
    int ret = swss::exec(cmds, res);

    SWSS_LOG_NOTICE("cmd:%s, res=%s, ret=%d", cmds.c_str(), res.c_str(), ret);

    return;
}

void FdbSync::macRefreshStateDB(int vlan, string kmac)
{
    string key = "Vlan" + to_string(vlan) + ":" + kmac;
    char *type;
    string port_name = "";

	SWSS_LOG_NOTICE("Refreshing Vlan:%d MAC route MAC:%s Key %s", vlan, kmac.c_str(), key.c_str());

    if (fdbmac.find(key)!=fdbmac.end())
    {
		port_name+= fdbmac[key].port_name;
        if (port_name.empty())
        {
            SWSS_LOG_INFO("Port name not present MAC route Key:%s", key.c_str());
            return;
        }

		SWSS_LOG_INFO("Refreshing MAC route MAC:%s Key %s Port %s", kmac.c_str(), key.c_str(), port_name.c_str());

		if (fdbmac[key].type == FDB_TYPE_DYNAMIC)
		{
			type = "dynamic";
		}
		else
		{
			type = "static";
		}

		const std::string cmds = std::string("")
			+ " bridge fdb " + "add" + " " + kmac + " dev "
			+ port_name + " master " + type  + " vlan " + to_string(vlan);

		std::string res;
		int ret = swss::exec(cmds, res);
		if (ret != 0)
		{
			SWSS_LOG_INFO("Failed cmd:%s, res=%s, ret=%d", cmds.c_str(), res.c_str(), ret);
		}

		SWSS_LOG_NOTICE("Refreshing cmd:%s, res=%s, ret=%d", cmds.c_str(), res.c_str(), ret);
    }
    return;
}

bool FdbSync::checkImetExit(string key, uint32_t vni)
{
    if (imet_route[key].redist_add_status == true)
    {
        SWSS_LOG_DEBUG("IMET exist key:%s Vni:%d", key.c_str(), vni);
        return false;
    }
    imet_route[key].vni =  vni;
    return true;
}

bool FdbSync::checkDelImet(string key, uint32_t vni)
{
    int ret = true;

    SWSS_LOG_DEBUG("Del IMET key:%s Vni:%d", key.c_str(), vni);
    if (imet_route.find(key) != imet_route.end())
    {
        SWSS_LOG_DEBUG("IMET found key:%s Vni:%d", key.c_str(), vni);
        if (imet_route[key].redist_add_status == false)
        {
            SWSS_LOG_DEBUG("IMET found key:%s Vni:%d, it should have not happened", key.c_str(), vni);
            ret = false;
        }
        imet_route.erase(key);
    }
    return ret;
}

void FdbSync::imetAddRoute(struct in_addr vtep, string vlan_str, uint32_t vni)
{
    string vlan_id = "Vlan" + vlan_str;
    string key = vlan_id + ":" + inet_ntoa(vtep);

    if (!checkImetExit(key, vni))
        return;

	SWSS_LOG_NOTICE("%sIMET Add route key:%s vtep:%s %s", 
			m_AppRestartAssist->isWarmStartInProgress() ? "WARM-RESTART:" : "",
			key.c_str(), inet_ntoa(vtep), vlan_id.c_str());

    std::vector<FieldValueTuple> fvVector;
    FieldValueTuple f("vni", to_string(vni));
    fvVector.push_back(f);

    imet_route[key].redist_add_status = true;
    
	// If warmstart is in progress, we take all netlink changes into the cache map
    if (m_AppRestartAssist->isWarmStartInProgress())
    {
        m_AppRestartAssist->insertToMap(APP_EVPN_REMOTE_VNI_TABLE_NAME, key, fvVector, false);
        return;
    }
    
	m_imetTable.set(key, fvVector);
    return;
}

void FdbSync::imetDelRoute(struct in_addr vtep, string vlan_str, uint32_t vni)
{
    string vlan_id = "Vlan" + vlan_str;
    string key = vlan_id + ":" + inet_ntoa(vtep);

    if (!checkDelImet(key, vni))
        return;

	SWSS_LOG_NOTICE("%sIMET Del route key:%s vtep:%s %s", 
			m_AppRestartAssist->isWarmStartInProgress() ? "WARM-RESTART:" : "", 
			key.c_str(), inet_ntoa(vtep), vlan_id.c_str());

    std::vector<FieldValueTuple> fvVector;
    FieldValueTuple f("vni", to_string(vni));
    fvVector.push_back(f);

    // If warmstart is in progress, we take all netlink changes into the cache map
    if (m_AppRestartAssist->isWarmStartInProgress())
    {
        m_AppRestartAssist->insertToMap(APP_EVPN_REMOTE_VNI_TABLE_NAME, key, fvVector, true);
        return;
    }
    
	m_imetTable.del(key);
    return;
}

void FdbSync::macUpdateVxlanDB(string key)
{
    string vtep = m_mac[key].vtep;
    string type;
    string vni = to_string(m_mac[key].vni);

    if (m_mac[key].type)
        type = "static";
    else
        type = "dynamic";

    std::vector<FieldValueTuple> fvVector;
    FieldValueTuple rv("remote_vtep", vtep);
    FieldValueTuple t("type", type);
    FieldValueTuple v("vni", vni);
    fvVector.push_back(rv);
    fvVector.push_back(t);
    fvVector.push_back(v);

	SWSS_LOG_NOTICE("%sVXLAN_FDB_TABLE: ADD_KEY %s vtep:%s type:%s", 
			m_AppRestartAssist->isWarmStartInProgress() ? "WARM-RESTART:" : "" ,
			key.c_str(), vtep.c_str(), type.c_str());

    // If warmstart is in progress, we take all netlink changes into the cache map
    if (m_AppRestartAssist->isWarmStartInProgress())
    {
        m_AppRestartAssist->insertToMap(APP_VXLAN_FDB_TABLE_NAME, key, fvVector, false);
        return;
    }
    
	SWSS_LOG_NOTICE("VXLAN_FDB_TABLE: ADD_KEY %s vtep:%s type:%s", key.c_str(), vtep.c_str(), type.c_str());
    m_fdbTable.set(key, fvVector);
}

void FdbSync::macDelVxlanDB(string key)
{
    string vtep = m_mac[key].vtep;
    string type;
    string vni = to_string(m_mac[key].vni);

    if (m_mac[key].type)
        type = "static";
    else
        type = "dynamic";

    std::vector<FieldValueTuple> fvVector;
    FieldValueTuple rv("remote_vtep", vtep);
    FieldValueTuple t("type", type);
    FieldValueTuple v("vni", vni);
    fvVector.push_back(rv);
    fvVector.push_back(t);
    fvVector.push_back(v);

	SWSS_LOG_NOTICE("%sVXLAN_FDB_TABLE: DEL_KEY %s vtep:%s type:%s", 
			m_AppRestartAssist->isWarmStartInProgress() ? "WARM-RESTART:" : "" ,            
			key.c_str(), vtep.c_str(), type.c_str());

    // If warmstart is in progress, we take all netlink changes into the cache map
    if (m_AppRestartAssist->isWarmStartInProgress())
    {
        m_AppRestartAssist->insertToMap(APP_VXLAN_FDB_TABLE_NAME, key, fvVector, true);
        return;
    }
    
	SWSS_LOG_NOTICE("VXLAN_FDB_TABLE: DEL_KEY %s vtep:%s type:%s", key.c_str(), vtep.c_str(), type.c_str());
    m_fdbTable.del(key);
    return;

}

void FdbSync::macAddVxlan(string key, struct in_addr vtep, uint32_t type, uint32_t vni, string intf_name)
{
    m_mac[key].vtep   =  inet_ntoa(vtep);
    m_mac[key].type   =  type;
    m_mac[key].vni    =  vni;
    m_mac[key].ifname = intf_name;

    macUpdateVxlanDB(key);
    return;
}

void FdbSync::macDelVxlan(string key)
{
    if (m_mac.find(key) != m_mac.end())
    {
        SWSS_LOG_INFO("DEL_KEY %s vtep:%s type:%d", key.c_str(), m_mac[key].vtep.c_str(), m_mac[key].type);
        macDelVxlanDB(key);
        m_mac.erase(key);
    }
    return;
}

void FdbSync::onMsgNbr(int nlmsg_type, struct nl_object *obj)
{
    char macStr[MAX_ADDR_SIZE + 1] = {0};
    struct rtnl_neigh *neigh = (struct rtnl_neigh *)obj;
    struct in_addr vtep = {0};
    int vlan = 0, ifindex = 0;
    uint32_t vni = 0;
    nl_addr *vtep_addr;
    string ifname;
    string key;
    bool delete_key = false;
    size_t str_loc = string::npos;
    uint32_t type = 0;
    bool isNotVxlanLink = false;
    string vlan_id = "";
    bool isIntfExist = false;

    if ((nlmsg_type != RTM_NEWNEIGH) && (nlmsg_type != RTM_GETNEIGH) &&
        (nlmsg_type != RTM_DELNEIGH))
        return;

    /* Only MAC route is to be supported */
    if (rtnl_neigh_get_family(neigh) != AF_BRIDGE)
    {
        return;
    }
    ifindex = rtnl_neigh_get_ifindex(neigh);
    if (intf_info.find(ifindex) != intf_info.end())
    {
        isIntfExist = true;
        ifname = intf_info[ifindex].ifname;
    }

    nl_addr2str(rtnl_neigh_get_lladdr(neigh), macStr, MAX_ADDR_SIZE);

    if (isIntfExist == false)
    {
        /* In Normal run kernel should not ageout this mac */
        isNotVxlanLink = true;

        /* When the mac is not learned on the vxlan interface then check if we need to refresh the route */
        if (nlmsg_type != RTM_DELNEIGH)
            return;
    }
    else
    {
        /* If this is for vnet bridge vxlan interface, then return */
        if (ifname.find(VXLAN_BR_IF_NAME_PREFIX) != string::npos)
            return;

        /* VxLan netdevice should be in <name>-<vlan-id> format */
        str_loc = ifname.find("-");
        if (str_loc == string::npos)
            return;

        vlan_id = "Vlan" + ifname.substr(str_loc+1,  std::string::npos);
        vni = intf_info[ifindex].vni;
    }


    if (isNotVxlanLink)
    {
        vlan = rtnl_neigh_get_vlan(neigh);
        macRefreshStateDB(vlan, macStr);
        return;
    }

    vtep_addr = rtnl_neigh_get_dst(neigh);
    if (vtep_addr == NULL)
    {
        return;
    }
    else
    {
        /* Currently we only support ipv4 tunnel endpoints */
        vtep.s_addr = *(uint32_t *)nl_addr_get_binary_addr(vtep_addr);
        SWSS_LOG_INFO("Tunnel IP %s Int%d", inet_ntoa(vtep), *(uint32_t *)nl_addr_get_binary_addr(vtep_addr));
    }

    int state = rtnl_neigh_get_state(neigh);
    if ((nlmsg_type == RTM_DELNEIGH) || (state == NUD_INCOMPLETE) ||
        (state == NUD_FAILED))
    {
        delete_key = true;
    }

    if (state & NUD_NOARP)
    {
        /* This is a static route */
        type = true;
    }

    /* Handling IMET routes */
    if (MacAddress(macStr) == MacAddress("00:00:00:00:00:00"))
    {
        if (vtep.s_addr)
        {
            string vlan_str = ifname.substr(str_loc+1, string::npos);

            if (!delete_key)
            {
                imetAddRoute(vtep, vlan_str, vni);
            }
            else
            {
                imetDelRoute(vtep, vlan_str, vni);
            }
        }
        return;
    }

    key+= vlan_id;
    key+= ":";
    key+= macStr;

    if (!delete_key)
    {
        macAddVxlan(key, vtep, type, vni, ifname);
    }
    else
    {
        macDelVxlan(key);
    }
    return;
}

void FdbSync::onMsgLink(int nlmsg_type, struct nl_object *obj)
{
    struct rtnl_link *link;
    char *ifname = NULL;
    char *nil = "NULL";
    int ifindex;
    unsigned int vni;

    link = (struct rtnl_link *)obj;
    ifname = rtnl_link_get_name(link);
    ifindex = rtnl_link_get_ifindex(link);
    if (rtnl_link_is_vxlan(link) == 0)
    {
        return;
    }

    if (rtnl_link_vxlan_get_id(link, &vni) != 0)
    {
        SWSS_LOG_NOTICE("Op:%d VxLAN dev:%s index:%d vni:%d. Not found", nlmsg_type, ifname? ifname: nil, ifindex, vni);
        return;
    }
    SWSS_LOG_INFO("Op:%d VxLAN dev %s index:%d vni:%d", nlmsg_type, ifname? ifname: nil, ifindex, vni);
    if (nlmsg_type == RTM_NEWLINK)
    {
        intf_info[ifindex].vni    =  vni;
        intf_info[ifindex].ifname =  ifname;
    }
    return;
}

void FdbSync::onMsg(int nlmsg_type, struct nl_object *obj)
{
    if ((nlmsg_type != RTM_NEWLINK) &&
        (nlmsg_type != RTM_NEWNEIGH) && (nlmsg_type != RTM_DELNEIGH))
    {
        SWSS_LOG_DEBUG("netlink: unhandled event: %d", nlmsg_type);
        return;
    }
    if (nlmsg_type == RTM_NEWLINK)
    {
        onMsgLink(nlmsg_type, obj);
    }
    else
    {
        onMsgNbr(nlmsg_type, obj);
    }
}

