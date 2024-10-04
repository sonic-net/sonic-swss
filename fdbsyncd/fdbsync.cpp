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
    m_cfgEvpnNvoTable(config_db, CFG_VXLAN_EVPN_NVO_TABLE_NAME),
    m_l2nhgTable(pipelineAppDB, APP_L2_NEXTHOP_GROUP_TABLE_NAME)
{
    m_AppRestartAssist = new AppRestartAssist(pipelineAppDB, "fdbsyncd", "swss", DEFAULT_FDBSYNC_WARMSTART_TIMER);
    if (m_AppRestartAssist)
    {
        m_AppRestartAssist->registerAppTable(APP_VXLAN_FDB_TABLE_NAME, &m_fdbTable);
        m_AppRestartAssist->registerAppTable(APP_VXLAN_REMOTE_VNI_TABLE_NAME, &m_imetTable);
        m_AppRestartAssist->registerAppTable(APP_L2_NEXTHOP_GROUP_TABLE_NAME, &m_l2nhgTable);
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
    std::string nh = m_mac[auxkey].nh;

    const std::string cmds = std::string("")
        + " bridge fdb del " + info->mac + " dev " 
        + m_mac[auxkey].ifname + " dst " + nh + " vlan " + info->vid.substr(4);

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
    string nh_type = m_mac[key].nh_type;
    string snh = m_mac[key].nh;
    string type;
    string vni = to_string(m_mac[key].vni);
    type = m_mac[key].type;

    std::vector<FieldValueTuple> fvVector;
    FieldValueTuple nh(nh_type, snh);
    FieldValueTuple t("type", type);
    FieldValueTuple v("vni", vni);
    fvVector.push_back(nh);
    fvVector.push_back(t);
    fvVector.push_back(v);

    SWSS_LOG_NOTICE("%sVXLAN_FDB_TABLE: DEL_KEY %s %s:%s type:%s", 
            m_AppRestartAssist->isWarmStartInProgress() ? "WARM-RESTART:" : "" ,
            key.c_str(), nh_type.c_str(), snh.c_str(), type.c_str());

    // If warmstart is in progress, we take all netlink changes into the cache map
    if (m_AppRestartAssist->isWarmStartInProgress())
    {
        m_AppRestartAssist->insertToMap(APP_VXLAN_FDB_TABLE_NAME, key, fvVector, true);
        return;
    }
    
    m_fdbTable.del(key);
    return;

}

void FdbSync::macAddVxlan(string key, struct in_addr vtep, uint32_t nh_id, string type, uint32_t vni, string intf_name)
{
    string nh_type = (vtep.s_addr != 0) ? "remote_vtep" : "nexthop_group";
    string snh = (vtep.s_addr != 0) ? inet_ntoa(vtep) : std::to_string(nh_id);
    string svni = to_string(vni);

    /* Update the DB with Vxlan MAC */
    m_mac[key] = {snh, nh_type, type, vni, intf_name};

    std::vector<FieldValueTuple> fvVector;
    FieldValueTuple nh(nh_type, snh);
    FieldValueTuple t("type", type);
    FieldValueTuple v("vni", svni);
    fvVector.push_back(nh);
    fvVector.push_back(t);
    fvVector.push_back(v);

    SWSS_LOG_INFO("%sVXLAN_FDB_TABLE: ADD_KEY %s %s:%s type:%s", 
            m_AppRestartAssist->isWarmStartInProgress() ? "WARM-RESTART:" : "" ,
            key.c_str(), nh_type.c_str(), snh.c_str(), type.c_str());
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
        SWSS_LOG_INFO("DEL_KEY %s %s:%s type:%s", key.c_str(), m_mac[key].nh_type.c_str(), m_mac[key].nh.c_str(), m_mac[key].type.c_str());
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
    uint32_t vni = 0, nh_id = 0;
    nl_addr *vtep_addr;
    string ifname;
    string key;
    bool delete_key = false;
    size_t str_loc = string::npos;
    string type = "";
    string vlan_id = "";
    bool isVxlanIntf = false;

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

    vtep_addr = rtnl_neigh_get_dst(neigh);
    if (vtep_addr == NULL)
    {
        if (rtnl_neigh_get_nhid(neigh, &nh_id))
        {
            return;
        }
        SWSS_LOG_INFO("Tunnel NH_ID %u", nh_id);
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
        macAddVxlan(key, vtep, nh_id, type, vni, ifname);
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

void FdbSync::onMsgNexthop(int nlmsg_type, struct nl_object *obj)
{
    struct rtnl_nh *nh = (struct rtnl_nh *)obj;
    bool del_op = (nlmsg_type == RTM_DELNEXTHOP);

    if (rtnl_nh_get_id(nh) < 0)
    {
        SWSS_LOG_ERROR("Nexthop %d < 0", rtnl_nh_get_id(nh));
        return;
    }

    if (!rtnl_nh_get_fdb(nh))
    {
        SWSS_LOG_INFO("Unhandled non-fdb nexthop %d", rtnl_nh_get_id(nh));
        return;
    }

    if (rtnl_nh_get_group_size(nh) >= 0)
    {
        if (del_op) {
            delL2NexthopGroup(nh);
        }
        else
        {
            addL2NexthopGroup(nh);
        }
    }
    else
    {
        if (del_op) {
            delL2Nexthop(nh);
        }
        else
        {
            addL2Nexthop(nh);
        }
    }
}

void FdbSync::delL2NexthopGroup(struct rtnl_nh *nh)
{
    std::vector<FieldValueTuple> fvVector;
    int id = rtnl_nh_get_id(nh);
    string key = to_string(id);

    flushNhgFDB(id);
    SWSS_LOG_INFO("%sNEXTHOP_GROUP_TABLE: DEL key:%s",
            m_AppRestartAssist->isWarmStartInProgress() ? "WARM-RESTART:" : "",
            key.c_str());

    if (m_AppRestartAssist->isWarmStartInProgress())
    {
        m_AppRestartAssist->insertToMap(APP_L2_NEXTHOP_GROUP_TABLE_NAME, key, fvVector, true);
    }
    else
    {
        m_l2nhgTable.del(key);
    }
}

void FdbSync::flushNhgFDB(int id)
{
    for (auto it = m_mac.begin(); it != m_mac.end(); )
    {
        const std::string& key = it->first;
        const m_mac_info& value = it->second;

        if (value.nh_type == "nexthop_group" && value.nh == std::to_string(id))
        {
            SWSS_LOG_INFO("DEL_KEY %s %s:%s type:%s", key.c_str(), m_mac[key].nh_type.c_str(),
                          m_mac[key].nh.c_str(), m_mac[key].type.c_str());
            macDelVxlanDB(key);
            it = m_mac.erase(it);
        }
        else
        {
            ++it;
        }
    }
}

void FdbSync::addL2NexthopGroup(struct rtnl_nh *nh)
{
    string ip_str;
    string ifname_str;
    std::vector<FieldValueTuple> fvVector;
    int nhg_size = rtnl_nh_get_group_size(nh);
    int id = rtnl_nh_get_id(nh);
    string key = to_string(id);
    string nhg_str = to_string(rtnl_nh_get_group_entry(nh, 0));

    for (int i = 1; i < nhg_size; i++)
    {
        int nh_id = rtnl_nh_get_group_entry(nh, i);
        nhg_str += "," + to_string(nh_id);
    }

    FieldValueTuple fvnhg("nexthop_group", nhg_str);
    fvVector.push_back(fvnhg);

    SWSS_LOG_INFO("%sL2_NEXTHOP_GROUP_TABLE: SET key:%s nexthop_group:%s",
            m_AppRestartAssist->isWarmStartInProgress() ? "WARM-RESTART:" : "",
            key.c_str(), nhg_str.c_str());

    if (m_AppRestartAssist->isWarmStartInProgress())
    {
        m_AppRestartAssist->insertToMap(APP_L2_NEXTHOP_GROUP_TABLE_NAME, key, fvVector, false);
    }
    else
    {
        m_l2nhgTable.set(key, fvVector);
    }
}

void FdbSync::delL2Nexthop(struct rtnl_nh *nh)
{
    std::vector<FieldValueTuple> fvVector;
    int id = rtnl_nh_get_id(nh);
    string key = to_string(id);

    SWSS_LOG_INFO("%ssAPP_L2_NEXTHOP_GROUP_TABLE_NAME: DEL key:%s",
            m_AppRestartAssist->isWarmStartInProgress() ? "WARM-RESTART:" : "", key.c_str());

    if (m_AppRestartAssist->isWarmStartInProgress())
    {
        m_AppRestartAssist->insertToMap(APP_L2_NEXTHOP_GROUP_TABLE_NAME, key, fvVector, true);
    }
    else
    {
        m_l2nhgTable.del(key);
    }
}

void FdbSync::addL2Nexthop(struct rtnl_nh *nh)
{
    char gw_str[MAX_ADDR_SIZE + 1] = {0};
    std::vector<FieldValueTuple> fvVector;
    int id = rtnl_nh_get_id(nh);
    string key = to_string(id);

    nl_addr2str(rtnl_nh_get_gateway(nh), gw_str, MAX_ADDR_SIZE);
    FieldValueTuple fvremote_vtep("remote_vtep", gw_str);
    fvVector.push_back(fvremote_vtep);

    SWSS_LOG_INFO("%sAPP_L2_NEXTHOP_GROUP_TABLE_NAME: SET key:%s",
            m_AppRestartAssist->isWarmStartInProgress() ? "WARM-RESTART:" : "", key.c_str());

    if (m_AppRestartAssist->isWarmStartInProgress())
    {
        m_AppRestartAssist->insertToMap(APP_L2_NEXTHOP_GROUP_TABLE_NAME, key, fvVector, false);
    }
    else
    {
        m_l2nhgTable.set(key, fvVector);
    }
}

void FdbSync::onMsg(int nlmsg_type, struct nl_object *obj)
{
    if ((nlmsg_type != RTM_NEWLINK) &&
        (nlmsg_type != RTM_NEWNEIGH) && (nlmsg_type != RTM_DELNEIGH) &&
        (nlmsg_type != RTM_NEWNEXTHOP) && (nlmsg_type != RTM_DELNEXTHOP))
    {
        SWSS_LOG_DEBUG("netlink: unhandled event: %d", nlmsg_type);
        return;
    }
    if (nlmsg_type == RTM_NEWLINK)
    {
        onMsgLink(nlmsg_type, obj);
    }
    else if (nlmsg_type == RTM_NEWNEIGH || nlmsg_type == RTM_DELNEIGH)
    {
        onMsgNbr(nlmsg_type, obj);
    }
    else if (nlmsg_type == RTM_NEWNEXTHOP || nlmsg_type == RTM_DELNEXTHOP)
    {
        onMsgNexthop(nlmsg_type, obj);
    }
}

