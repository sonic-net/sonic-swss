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
#include "fdbsync.h"
#include "warm_restart.h"
#include "errno.h"

using namespace std;
using namespace swss;

#define VXLAN_BR_IF_NAME_PREFIX    "Brvxlan"

FdbSync::FdbSync(RedisPipeline *pipelineAppDB, DBConnector *stateDb, DBConnector *config_db) :
    m_fdbTable(pipelineAppDB, APP_VXLAN_FDB_TABLE_NAME),
    m_imetTable(pipelineAppDB, APP_VXLAN_REMOTE_VNI_TABLE_NAME),
    m_fdbStateTable(stateDb, STATE_FDB_TABLE_NAME),
    m_mclagRemoteFdbStateTable(stateDb, STATE_MCLAG_REMOTE_FDB_TABLE_NAME),
    m_l2NhgTable(pipelineAppDB, APP_L2_NEXTHOP_GROUP_TABLE_NAME),
    m_cfgEvpnNvoTable(config_db, CFG_VXLAN_EVPN_NVO_TABLE_NAME)
{
    m_AppRestartAssist = new AppRestartAssist(pipelineAppDB, "fdbsyncd", "swss", DEFAULT_FDBSYNC_WARMSTART_TIMER);
    if (m_AppRestartAssist)
    {
        m_AppRestartAssist->registerAppTable(APP_VXLAN_FDB_TABLE_NAME, &m_fdbTable);
        m_AppRestartAssist->registerAppTable(APP_VXLAN_REMOTE_VNI_TABLE_NAME, &m_imetTable);
        m_AppRestartAssist->registerAppTable(APP_L2_NEXTHOP_GROUP_TABLE_NAME, &m_l2NhgTable);
    }
}

FdbSync::~FdbSync()
{
    if (m_AppRestartAssist)
    {
        delete m_AppRestartAssist;
    }
}

// Check if interface entries are restored in kernel
bool FdbSync::isIntfRestoreDone()
{
    vector<string> required_modules = {
            "vxlanmgrd",
            "intfmgrd",
            "vlanmgrd",
            "vrfmgrd"
        };

    for (string& module : required_modules)
    {
        WarmStart::WarmStartState state;
        
        WarmStart::getWarmStartState(module, state);
        if (state == WarmStart::REPLAYED || state == WarmStart::RECONCILED)
        {
            SWSS_LOG_INFO("Module %s Replayed or Reconciled %d",module.c_str(), (int) state);            
        }
        else
        {
            SWSS_LOG_INFO("Module %s NOT Replayed or Reconciled %d",module.c_str(), (int) state);            
            return false;
        }
    }
    
    return true;
}

void FdbSync::processCfgEvpnNvo()
{
    std::deque<KeyOpFieldsValuesTuple> entries;
    m_cfgEvpnNvoTable.pops(entries);
    bool lastNvoState = m_isEvpnNvoExist;

    for (auto entry: entries)
    {
        std::string op = kfvOp(entry);

        if (op == SET_COMMAND)
        {
            m_isEvpnNvoExist = true;
        }
        else if (op == DEL_COMMAND)
        {
            m_isEvpnNvoExist = false;
        }

        if (lastNvoState != m_isEvpnNvoExist)
        {
            updateAllLocalMac();
        }
    }
    return;
}

void FdbSync::updateAllLocalMac()
{
    for ( auto it = m_fdb_mac.begin(); it != m_fdb_mac.end(); ++it )
    {
        if (m_isEvpnNvoExist)
        {
            /* Add the Local FDB entry into Kernel */
            addLocalMac(it->first, "replace");
        }
        else
        {
            /* Delete the Local FDB entry from Kernel */
            addLocalMac(it->first, "del");
        }
    }
}

void FdbSync::processStateFdb()
{
    struct m_fdb_info info;
    std::deque<KeyOpFieldsValuesTuple> entries;

    m_fdbStateTable.pops(entries);

    int count =0 ;
    for (auto entry: entries)
    {
        count++;
        std::string key = kfvKey(entry);
        std::string op = kfvOp(entry);

        std::size_t delimiter = key.find_first_of(":");
        auto vlan_name = key.substr(0, delimiter);
        auto mac_address = key.substr(delimiter+1);

        info.vid = vlan_name;
        info.mac = mac_address;

        if(op == "SET")
        {
            info.op_type = FDB_OPER_ADD ;
        }
        else
        {
            info.op_type = FDB_OPER_DEL ;
        }

        SWSS_LOG_INFO("FDBSYNCD STATE FDB updates key=%s, operation=%s\n", key.c_str(), op.c_str());

        for (auto i : kfvFieldsValues(entry))
        {
            SWSS_LOG_INFO(" FDBSYNCD STATE FDB updates : "
            "FvFiels %s, FvValues: %s \n", fvField(i).c_str(), fvValue(i).c_str());

            if(fvField(i) == "port")
            {
                info.port_name = fvValue(i);
            }

            if(fvField(i) == "type")
            {
                if(fvValue(i) == "dynamic")
                {
                    info.type = FDB_TYPE_DYNAMIC;
                }
                else if (fvValue(i) == "static")
                {
                    info.type = FDB_TYPE_STATIC;
                }
            }
        }

        if (op != "SET" && macCheckSrcDB(&info) == false)
        {
            continue;
        }
        updateLocalMac(&info);
    }
}

void FdbSync::processStateMclagRemoteFdb()
{
    struct m_fdb_info info;
    std::deque<KeyOpFieldsValuesTuple> entries;

    m_mclagRemoteFdbStateTable.pops(entries);

    int count =0 ;
    for (auto entry: entries)
    {
        count++;
        std::string key = kfvKey(entry);
        std::string op = kfvOp(entry);

        std::size_t delimiter = key.find_first_of(":");
        auto vlan_name = key.substr(0, delimiter);
        auto mac_address = key.substr(delimiter+1);

        info.vid = vlan_name;
        info.mac = mac_address;

        if(op == "SET")
        {
            info.op_type = FDB_OPER_ADD ;
        }
        else
        {
            info.op_type = FDB_OPER_DEL ;
        }

        SWSS_LOG_INFO("FDBSYNCD STATE FDB updates key=%s, operation=%s\n", key.c_str(), op.c_str());

        for (auto i : kfvFieldsValues(entry))
        {
            SWSS_LOG_INFO(" FDBSYNCD STATE FDB updates : "
            "FvFiels %s, FvValues: %s \n", fvField(i).c_str(), fvValue(i).c_str());

            if(fvField(i) == "port")
            {
                info.port_name = fvValue(i);
            }

            if(fvField(i) == "type")
            {
                if(fvValue(i) == "dynamic")
                {
                    info.type = FDB_TYPE_DYNAMIC;
                }
                else if (fvValue(i) == "static")
                {
                    info.type = FDB_TYPE_STATIC;
                }
            }
        }

        if (op != "SET" && macCheckSrcDB(&info) == false)
        {
            continue;
        }
        updateMclagRemoteMac(&info);
    }
}

void FdbSync::macUpdateCache(struct m_fdb_info *info)
{
    string key = info->vid + ":" + info->mac;
    m_fdb_mac[key].port_name = info->port_name;
    m_fdb_mac[key].type      = info->type;

    return;
}

void FdbSync::macUpdateMclagRemoteCache(struct m_fdb_info *info)
{
    string key = info->vid + ":" + info->mac;
    m_mclag_remote_fdb_mac[key].port_name = info->port_name;
    m_mclag_remote_fdb_mac[key].type      = info->type;

    return;
}

bool FdbSync::macCheckSrcDB(struct m_fdb_info *info)
{
    string key = info->vid + ":" + info->mac;
    if (m_fdb_mac.find(key) != m_fdb_mac.end())
    {
        SWSS_LOG_INFO("DEL_KEY %s ", key.c_str());
        return true;
    }

    return false;
}

void FdbSync::macDelVxlanEntry(string auxkey, struct m_fdb_info *info)
{
    std::string vtep = m_mac[auxkey].vtep;

    const std::string cmds = std::string("")
        + " bridge fdb del " + info->mac + " dev " 
        + m_mac[auxkey].ifname + " dst " + vtep + " vlan " + info->vid.substr(4);

    std::string res;
    int ret = swss::exec(cmds, res);
    if (ret != 0)
    {
        SWSS_LOG_INFO("Failed cmd:%s, res=%s, ret=%d", cmds.c_str(), res.c_str(), ret);
    }

    SWSS_LOG_INFO("Success cmd:%s, res=%s, ret=%d", cmds.c_str(), res.c_str(), ret);

    return;
}

void FdbSync::updateLocalMac (struct m_fdb_info *info)
{
    char *op;
    char *type;
    string port_name = "";
    string key = info->vid + ":" + info->mac;
    short fdb_type;    /*dynamic or static*/

    if (info->op_type == FDB_OPER_ADD)
    {
        macUpdateCache(info);
        op = "replace";
        port_name = info->port_name;
        fdb_type = info->type;
    }
    else
    {
        op = "del";
        port_name = m_fdb_mac[key].port_name;
        fdb_type = m_fdb_mac[key].type;
        m_fdb_mac.erase(key);
    }

    if (!m_isEvpnNvoExist)
    {
        SWSS_LOG_INFO("Ignore kernel update EVPN NVO is not configured MAC %s", key.c_str());
        return;
    }

    if (fdb_type == FDB_TYPE_DYNAMIC)
    {
        type = "dynamic extern_learn";
    }
    else
    {
        type = "sticky static";
    }

    const std::string cmds = std::string("")
        + " bridge fdb " + op + " " + info->mac + " dev " 
        + port_name + " master " + type + " vlan " + info->vid.substr(4);

    std::string res;
    int ret = swss::exec(cmds, res);

    SWSS_LOG_INFO("cmd:%s, res=%s, ret=%d", cmds.c_str(), res.c_str(), ret);

    if (info->op_type == FDB_OPER_ADD)
    {
        /* Check if this vlan+key is also learned by vxlan neighbor then delete the dest entry */
        if (m_mac.find(key) != m_mac.end())
        {
            macDelVxlanEntry(key, info);
            SWSS_LOG_INFO("Local learn event deleting from VXLAN table DEL_KEY %s", key.c_str());
            macDelVxlan(key);
        }
    }

    return;
}

void FdbSync::addLocalMac(string key, string op)
{
    char *type;
    string port_name = "";
    string mac = "";
    string vlan = "";
    size_t str_loc = string::npos;

    str_loc = key.find(":");
    if (str_loc == string::npos)
    {
        SWSS_LOG_ERROR("Local MAC issue with Key:%s", key.c_str());
        return;
    }
    vlan = key.substr(4,  str_loc-4);
    mac = key.substr(str_loc+1,  std::string::npos);

    SWSS_LOG_INFO("Local route Vlan:%s MAC:%s Key:%s Op:%s", vlan.c_str(), mac.c_str(), key.c_str(), op.c_str());

    if (m_fdb_mac.find(key)!=m_fdb_mac.end())
    {
        port_name = m_fdb_mac[key].port_name;
        if (port_name.empty())
        {
            SWSS_LOG_INFO("Port name not present MAC route Key:%s", key.c_str());
            return;
        }

        if (m_fdb_mac[key].type == FDB_TYPE_DYNAMIC)
        {
            type = "dynamic extern_learn";
        }
        else
        {
            type = "static";
        }

        const std::string cmds = std::string("")
                + " bridge fdb " + op + " " + mac + " dev "
                + port_name + " master " + type  + " vlan " + vlan;

        std::string res;
        int ret = swss::exec(cmds, res);
        if (ret != 0)
        {
            SWSS_LOG_INFO("Failed cmd:%s, res=%s, ret=%d", cmds.c_str(), res.c_str(), ret);
        }

        SWSS_LOG_INFO("Config triggered cmd:%s, res=%s, ret=%d", cmds.c_str(), res.c_str(), ret);
    }
    return;
}

void FdbSync::updateMclagRemoteMac (struct m_fdb_info *info)
{
    char *op;
    char *type;
    string port_name = "";
    string key = info->vid + ":" + info->mac;
    short fdb_type;    /*dynamic or static*/

    if (info->op_type == FDB_OPER_ADD)
    {
        macUpdateMclagRemoteCache(info);
        op = "replace";
        port_name = info->port_name;
        fdb_type = info->type;
    }
    else
    {
        op = "del";
        port_name = m_mclag_remote_fdb_mac[key].port_name;
        fdb_type = m_mclag_remote_fdb_mac[key].type;
        m_mclag_remote_fdb_mac.erase(key);
    }

    if (fdb_type == FDB_TYPE_DYNAMIC)
    {
        type = "dynamic extern_learn";
    }
    else
    {
        type = "static";
    }

    const std::string cmds = std::string("")
        + " bridge fdb " + op + " " + info->mac + " dev "
        + port_name + " master " + type + " vlan " + info->vid.substr(4);

    std::string res;
    int ret = swss::exec(cmds, res);

    SWSS_LOG_INFO("cmd:%s, res=%s, ret=%d", cmds.c_str(), res.c_str(), ret);

    return;
}

void FdbSync::updateMclagRemoteMacPort(int ifindex, int vlan, std::string mac)
{
    string key = "Vlan" + to_string(vlan) + ":" + mac;
    int type = 0;
    string port_name = "";

    SWSS_LOG_INFO("Updating Intf %d, Vlan:%d MAC:%s Key %s", ifindex, vlan, mac.c_str(), key.c_str());

    if (m_mclag_remote_fdb_mac.find(key) != m_mclag_remote_fdb_mac.end())
    {
        type = m_mclag_remote_fdb_mac[key].type;
        port_name = m_mclag_remote_fdb_mac[key].port_name;
        SWSS_LOG_INFO(" port %s, type %d\n", port_name.c_str(), type);

        if (type == FDB_TYPE_STATIC)
        {
            const std::string cmds = std::string("")
                + " bridge fdb replace" + " " + mac + " dev "
                + port_name + " master static vlan " + to_string(vlan);

            std::string res;
            int ret = swss::exec(cmds, res);
            if (ret != 0)
            {
                SWSS_LOG_NOTICE("Failed cmd:%s, res=%s, ret=%d", cmds.c_str(), res.c_str(), ret);
                return;
            }

            SWSS_LOG_NOTICE("Update cmd:%s, res=%s, ret=%d", cmds.c_str(), res.c_str(), ret);
        }
    }
    return;
}

/*
 * This is a special case handling where mac is learned in the ASIC.
 * Then MAC is learned in the Kernel, Since this mac is learned in the Kernel
 * This MAC will age out, when MAC delete is received from the Kernel.
 * If MAC is still present in the state DB cache then fdbsyncd will be 
 * re-programmed with MAC in the Kernel
 */
void FdbSync::macRefreshStateDB(int vlan, string kmac)
{
    string key = "Vlan" + to_string(vlan) + ":" + kmac;
    char *type;
    string port_name = "";

    SWSS_LOG_INFO("Refreshing Vlan:%d MAC route MAC:%s Key %s", vlan, kmac.c_str(), key.c_str());

    if (m_fdb_mac.find(key)!=m_fdb_mac.end())
    {
        port_name = m_fdb_mac[key].port_name;
        if (port_name.empty())
        {
            SWSS_LOG_INFO("Port name not present MAC route Key:%s", key.c_str());
            return;
        }

        if (m_fdb_mac[key].type == FDB_TYPE_DYNAMIC)
        {
            type = "dynamic extern_learn";
        }
        else
        {
            type = "static";
        }

        const std::string cmds = std::string("")
            + " bridge fdb " + "replace" + " " + kmac + " dev "
            + port_name + " master " + type  + " vlan " + to_string(vlan);

        std::string res;
        int ret = swss::exec(cmds, res);
        if (ret != 0)
        {
            SWSS_LOG_INFO("Failed cmd:%s, res=%s, ret=%d", cmds.c_str(), res.c_str(), ret);
        }

        SWSS_LOG_INFO("Refreshing cmd:%s, res=%s, ret=%d", cmds.c_str(), res.c_str(), ret);
    }
    return;
}

bool FdbSync::checkImetExist(string key, uint32_t vni)
{
    if (m_imet_route.find(key) != m_imet_route.end())
    {
        SWSS_LOG_INFO("IMET exist key:%s Vni:%d", key.c_str(), vni);
        return false;
    }
    m_imet_route[key].vni =  vni;
    return true;
}

bool FdbSync::checkDelImet(string key, uint32_t vni)
{
    int ret = false;

    SWSS_LOG_INFO("Del IMET key:%s Vni:%d", key.c_str(), vni);
    if (m_imet_route.find(key) != m_imet_route.end())
    {
        ret = true;
        m_imet_route.erase(key);
    }
    return ret;
}

void FdbSync::imetAddRoute(struct in_addr vtep, string vlan_str, uint32_t vni)
{
    string vlan_id = "Vlan" + vlan_str;
    string key = vlan_id + ":" + inet_ntoa(vtep);

    if (!checkImetExist(key, vni))
    {
        return;
    }

    SWSS_LOG_INFO("%sIMET Add route key:%s vtep:%s %s", 
            m_AppRestartAssist->isWarmStartInProgress() ? "WARM-RESTART:" : "",
            key.c_str(), inet_ntoa(vtep), vlan_id.c_str());

    std::vector<FieldValueTuple> fvVector;
    FieldValueTuple f("vni", to_string(vni));
    fvVector.push_back(f);

    // If warmstart is in progress, we take all netlink changes into the cache map
    if (m_AppRestartAssist->isWarmStartInProgress())
    {
        m_AppRestartAssist->insertToMap(APP_VXLAN_REMOTE_VNI_TABLE_NAME, key, fvVector, false);
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
    {
        return;
    }

    SWSS_LOG_INFO("%sIMET Del route key:%s vtep:%s %s", 
            m_AppRestartAssist->isWarmStartInProgress() ? "WARM-RESTART:" : "", 
            key.c_str(), inet_ntoa(vtep), vlan_id.c_str());

    std::vector<FieldValueTuple> fvVector;
    FieldValueTuple f("vni", to_string(vni));
    fvVector.push_back(f);

    // If warmstart is in progress, we take all netlink changes into the cache map
    if (m_AppRestartAssist->isWarmStartInProgress())
    {
        m_AppRestartAssist->insertToMap(APP_VXLAN_REMOTE_VNI_TABLE_NAME, key, fvVector, true);
        return;
    }
    
    m_imetTable.del(key);
    return;
}

void FdbSync::macDelVxlanDB(string key)
{
    string vtep = m_mac[key].vtep;
    string type;
    string vni = to_string(m_mac[key].vni);
    type = m_mac[key].type;

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
    
    m_fdbTable.del(key);
    return;

}

void FdbSync::macAddVxlan(string key, struct in_addr vtep, string type, uint32_t vni, string intf_name, int nhid)
{
    string svtep = inet_ntoa(vtep);
    string svni = to_string(vni);

    /* Update the DB with Vxlan MAC */
    m_mac[key] = {svtep, type, vni, intf_name};

    std::vector<FieldValueTuple> fvVector;
    if (nhid > 0)
    {
        string nhid_str = to_string(nhid);
        FieldValueTuple rv("nexthop_group", nhid_str);
        fvVector.push_back(rv);
    }
    else
    {
        FieldValueTuple rv("remote_vtep", svtep);
        fvVector.push_back(rv);
    }

    FieldValueTuple rv("remote_vtep", svtep);
    FieldValueTuple t("type", type);
    FieldValueTuple v("vni", svni);
    fvVector.push_back(t);
    fvVector.push_back(v);

    SWSS_LOG_INFO("%sVXLAN_FDB_TABLE: ADD_KEY %s vtep:%s NHID:%d type:%s",
            m_AppRestartAssist->isWarmStartInProgress() ? "WARM-RESTART:" : "" ,
            key.c_str(), svtep.c_str(), nhid, type.c_str());
    // If warmstart is in progress, we take all netlink changes into the cache map
    if (m_AppRestartAssist->isWarmStartInProgress())
    {
        m_AppRestartAssist->insertToMap(APP_VXLAN_FDB_TABLE_NAME, key, fvVector, false);
        return;
    }
    
    m_fdbTable.set(key, fvVector);

    return;
}

void FdbSync::macDelVxlan(string key)
{
    if (m_mac.find(key) != m_mac.end())
    {
        SWSS_LOG_INFO("DEL_KEY %s vtep:%s type:%s", key.c_str(), m_mac[key].vtep.c_str(), m_mac[key].type.c_str());
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
    string type = "";
    string vlan_id = "";
    bool isVxlanIntf = false;
    int nhid = -1;

    if ((nlmsg_type != RTM_NEWNEIGH) && (nlmsg_type != RTM_GETNEIGH) &&
        (nlmsg_type != RTM_DELNEIGH))
    {
        return;
    }

    /* Only MAC route is to be supported */
    if (rtnl_neigh_get_family(neigh) != AF_BRIDGE)
    {
        return;
    }
    ifindex = rtnl_neigh_get_ifindex(neigh);
    if (m_intf_info.find(ifindex) != m_intf_info.end())
    {
        isVxlanIntf = true;
        ifname = m_intf_info[ifindex].ifname;
    }

    nl_addr2str(rtnl_neigh_get_lladdr(neigh), macStr, MAX_ADDR_SIZE);

    if (isVxlanIntf == false)
    {
        if (nlmsg_type == RTM_NEWNEIGH)
        {
            int vid = rtnl_neigh_get_vlan(neigh);
            int state = rtnl_neigh_get_state(neigh);
            if (state & NUD_PERMANENT)
            {
                updateMclagRemoteMacPort(ifindex, vid, macStr);
            }
        }

        if (nlmsg_type != RTM_DELNEIGH)
        {
            return;
        }
    }
    else
    {
        /* If this is for vnet bridge vxlan interface, then return */
        if (ifname.find(VXLAN_BR_IF_NAME_PREFIX) != string::npos)
        {
            return;
        }

        /* VxLan netdevice should be in <name>-<vlan-id> format */
        str_loc = ifname.rfind("-");
        if (str_loc == string::npos)
        {
            return;
        }

        vlan_id = "Vlan" + ifname.substr(str_loc+1,  std::string::npos);
        vni = m_intf_info[ifindex].vni;
    }


    if (isVxlanIntf == false)
    {
        vlan = rtnl_neigh_get_vlan(neigh);
        if (m_isEvpnNvoExist)
        {
            macRefreshStateDB(vlan, macStr);
        }
        return;
    }

    nhid = rtnl_neigh_get_nh_id(neigh);
    if (nhid < 0)
    {
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
    }
    else
    {
        SWSS_LOG_INFO("L2 NHID %d", nhid);
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
        type = "static";
    }
    else
    {
        type = "dynamic";
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
        macAddVxlan(key, vtep, type, vni, ifname, nhid);
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
        SWSS_LOG_INFO("Op:%d VxLAN dev:%s index:%d vni:%d. Not found", nlmsg_type, ifname? ifname: nil, ifindex, vni);
        return;
    }
    SWSS_LOG_INFO("Op:%d VxLAN dev %s index:%d vni:%d", nlmsg_type, ifname? ifname: nil, ifindex, vni);
    if (nlmsg_type == RTM_NEWLINK)
    {
        m_intf_info[ifindex].vni    =  vni;
        m_intf_info[ifindex].ifname =  ifname;
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

void FdbSync::nhgDelDB(struct m_nhg_info *nhg_info)
{
    string key = to_string(nhg_info->nhid);

    std::vector<FieldValueTuple> fvVector;
    FieldValueTuple grp("nexthop_group", nhg_info->group_str);
    FieldValueTuple vtep("remote_vtep", nhg_info->vtep_str);

    SWSS_LOG_NOTICE("%sL2_NHG_TABLE: DEL_KEY %s vtep:%s grp:%s",
                    m_AppRestartAssist->isWarmStartInProgress() ? "WARM-RESTART:" : "" ,
                    key.c_str(), nhg_info->vtep_str.c_str(), nhg_info->group_str.c_str());
    if (m_AppRestartAssist->isWarmStartInProgress())
    {
        m_AppRestartAssist->insertToMap(APP_L2_NEXTHOP_GROUP_TABLE_NAME, key, fvVector, true);
        return;
    }

    m_l2NhgTable.del(key);
    return;
}


void FdbSync::nhgUpdateDB(struct m_nhg_info *nhg_info)
{
    int nhid = nhg_info->nhid;
    std::vector<FieldValueTuple> fvVector;
    string key = to_string(nhid);

    FieldValueTuple vtep("remote_vtep",    nhg_info->vtep_str);
    FieldValueTuple group("nexthop_group", nhg_info->group_str);

    fvVector.push_back(vtep);
    fvVector.push_back(group);

    SWSS_LOG_NOTICE("%sL2_NEXTHOP_GROUP_TABLE: ADD_KEY %s vtep:%s group: %s",
            m_AppRestartAssist->isWarmStartInProgress() ? "WARM-RESTART:" : "" ,
            key.c_str(), nhg_info->vtep_str.c_str(), nhg_info->group_str.c_str());

    // If warmstart is in progress, we take all netlink changes into the cache map
    if (m_AppRestartAssist->isWarmStartInProgress())
    {
        m_AppRestartAssist->insertToMap(APP_L2_NEXTHOP_GROUP_TABLE_NAME, key, fvVector, false);
        return;
    }

    m_l2NhgTable.set(key, fvVector);
    return;
}

void FdbSync::processNhgNetlinkData(struct m_nhg_info *entry)
{
    if (entry->op == NH_ADD)
    {
        nhgUpdateDB(entry);
    }
    else
    {
        nhgDelDB(entry);
    }
    return;
}

void FdbSync::netlink_parse_rtattr(struct rtattr **tb, int max, struct rtattr *rta,
                                            int len)
{
    while (RTA_OK(rta, len)) {
        if (rta->rta_type <= max)
        {
            tb[rta->rta_type] = rta;
        }
        else
        {
            if (rta->rta_type & NLA_F_NESTED)
            {
                int rta_type = rta->rta_type & ~NLA_F_NESTED;
                if (rta_type <= max)
                {
                   tb[rta_type] = rta;
                }
            }
        }
        rta = RTA_NEXT(rta, len);
    }
}

void FdbSync::nhgAddGroup(int nhid, struct nexthop_grp *nhid_grp, long unsigned int count,
                                 string &nh_id_grp_str)
{
   char nh_id_grp[IFNAMSIZ + 1] = {0};

    for (long unsigned int i = 0; i < count; i++)
    {
        memset(nh_id_grp,0, IFNAMSIZ);
        snprintf(nh_id_grp, IFNAMSIZ,"%d", nhid_grp[i].id);
        if (i > 0)
        {
            nh_id_grp_str += ",";
        }
        nh_id_grp_str += nh_id_grp;
    }
    return;
}

void FdbSync::onMsgNhg(struct nlmsghdr *h, int len)
{
    struct nhmsg *nhm;
    struct rtattr *tb[NHA_MAX + 1];
    void *gw = NULL;
    char anyaddr[MAX_ADDR_SIZE] = {0};
    char gwaddr[MAX_ADDR_SIZE] = {0};
    char nexthopaddr[MAX_ADDR_SIZE] = {0};
    int gw_af;
    int nlmsg_type = h->nlmsg_type;
    unsigned int id = 0;
    struct m_nhg_info nhg_entry;
    nhm = (struct nhmsg *)NLMSG_DATA(h);
    char op = NH_ADD;

    /* Parse attributes and extract fields of interest. */
    memset(tb, 0, sizeof(tb));
    netlink_parse_rtattr(tb, NHA_MAX, NHA_RTA(nhm), len);

    if (!tb[NHA_FDB])
        return;

    if (tb[NHA_ID])
    {
        id = *(uint32_t *) RTA_DATA(tb[NHA_ID]);
    }

    if( RTM_DELNEXTHOP == nlmsg_type)
    {
        SWSS_LOG_NOTICE("NHG DELETE MSG type %d nhid %d",
                nlmsg_type, id);
        op = NH_DEL;
    }

    if (tb[NHA_GROUP])
    {
        //Recursive/ECMP NHG
        long unsigned int count = 0;
        struct nexthop_grp *grp;
        string nh_id_grp_str = "";

        count =   RTA_PAYLOAD(tb[NHA_GROUP])/(sizeof (struct nexthop_grp));
        grp = (struct nexthop_grp *) RTA_DATA(tb[NHA_GROUP]);
        SWSS_LOG_NOTICE("NHA_GROUP MSG type %d zebra_nhid %d count %ld", nlmsg_type, id, count);

        nhgAddGroup(id, grp, count, nh_id_grp_str);

        nhg_entry.nhid      = id;
        nhg_entry.vtep_str  = string("");
        nhg_entry.group_str = nh_id_grp_str;

        SWSS_LOG_NOTICE("Set Grp nhid %d nhg:%s op:%d", id, nh_id_grp_str.c_str(), op);
    }
    else
    {
        if(tb[NHA_GATEWAY])
        {
            gw = RTA_DATA(tb[NHA_GATEWAY]);
        }
        else
        {
            gw = anyaddr;
        }

        gw_af = nhm->nh_family;
        if (gw_af == AF_INET)
        {
            memcpy(gwaddr, gw, IPV4_MAX_BYTE);
        }
        else if (gw_af == AF_INET6)
        {
            memcpy(gwaddr, gw, IPV6_MAX_BYTE);
        }
        inet_ntop(gw_af, gwaddr, nexthopaddr, MAX_ADDR_SIZE);

        nhg_entry.nhid      = id;
        nhg_entry.vtep_str  = string(nexthopaddr);
        nhg_entry.group_str = string("");

        SWSS_LOG_NOTICE("set nhid %d gw:%s op:%d",
                id, nexthopaddr, op);
    }

    nhg_entry.op = op;

    processNhgNetlinkData(&nhg_entry);

    return;
}

void FdbSync::onMsgRaw(struct nlmsghdr *h)
{
    int len;
    int nlmsg_type = h->nlmsg_type;

    if ((h->nlmsg_type != RTM_NEWNEXTHOP)
        && (h->nlmsg_type != RTM_DELNEXTHOP))
        return;

    if( nlmsg_type == RTM_NEWNEXTHOP
            || nlmsg_type == RTM_DELNEXTHOP)
    {
        /* Length validity. */
        len = (int)(h->nlmsg_len - NLMSG_LENGTH(sizeof(struct nhmsg)));
        if (len < 0)
        {
            SWSS_LOG_ERROR("%s: Message received from netlink is of a broken size %d %zu",
                    __PRETTY_FUNCTION__, h->nlmsg_len,
                    (size_t)NLMSG_LENGTH(sizeof(struct nhmsg)));
            return;
        }
        onMsgNhg(h, len);
    }
    return;
}
