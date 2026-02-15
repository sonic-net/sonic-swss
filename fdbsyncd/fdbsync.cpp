/* Quick hack to bring the latest header into fdbsyncd compilation */
#include "neighbour.h"

#include <string>
#include <algorithm>
#include <netinet/in.h>
#include <linux/nexthop.h>
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
    m_l2NhgTable(pipelineAppDB, APP_L2_NEXTHOP_GROUP_TABLE_NAME),
    m_fdbStateTable(stateDb, STATE_FDB_TABLE_NAME),
    m_mclagRemoteFdbStateTable(stateDb, STATE_MCLAG_REMOTE_FDB_TABLE_NAME),
    m_cfgEvpnNvoTable(config_db, CFG_VXLAN_EVPN_NVO_TABLE_NAME)
{
    m_AppRestartAssist = new AppRestartAssist(pipelineAppDB, "fdbsyncd", "swss", DEFAULT_FDBSYNC_WARMSTART_TIMER);
    if (m_AppRestartAssist)
    {
        m_AppRestartAssist->registerAppTable(APP_VXLAN_FDB_TABLE_NAME, &m_fdbTable);
        m_AppRestartAssist->registerAppTable(APP_VXLAN_REMOTE_VNI_TABLE_NAME, &m_imetTable);
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
    std::string cmds;
    auto mac = info->mac;
    auto ifname = m_mac[auxkey].ifname;
    auto vid =  info->vid.substr(4);
    if (m_mac[auxkey].nhtype == VTEP)
    {
        std::string vtep = m_mac[auxkey].v.remote_vtep;

        // The usage of self allow the avoidance of
        // deleting both bridge and VxLAN FDB.
        cmds = std::string("")
            + " bridge fdb del " + mac + " dev "
            + ifname + " dst " + vtep + " vlan " + vid + " self";
        //bridge fdb del 00:00:00:00:66:66 dev Ethernet1_13 vlan 10 master

    }
    else if (m_mac[auxkey].nhtype == NEXTHOPGROUP)
    {
        std::string nexthop_group = m_mac[auxkey].v.nexthop_group;

        // The usage of self allow the avoidance of
        // deleting both bridge and VxLAN FDB.
        cmds = std::string("")
            + " bridge fdb del " + mac + " dev "
            + ifname + " nhid " + nexthop_group + " vlan " + vid + " self";
        //00:00:00:22:22:22 dev VXLAN-10 nhid 536870913 self static
    }
    else
    {
        SWSS_LOG_INFO("Delete of this Vxlan entry is not supported. \
                       Mac points to neither a NHG or VTEP. \
                        nhtype: %d", m_mac[auxkey].nhtype);
        return;
    }

    std::string res;
    int ret = swss::exec(cmds, res);
    if (ret != 0)
    {
        SWSS_LOG_ERROR("Failed cmd:%s, res=%s, ret=%d", cmds.c_str(), res.c_str(), ret);
    }
    else
    {
        SWSS_LOG_INFO("Success cmd:%s, res=%s, ret=%d", cmds.c_str(), res.c_str(), ret);
    }
}

void FdbSync::updateLocalMac (struct m_fdb_info *info)
{
    char *op;
    char *type;
    char *proto_string;
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
        proto_string = " proto hw";
    }
    else
    {
        type = "static";
    }

    const std::string cmds = std::string("")
        + " bridge fdb " + op + " " + info->mac + " dev "
        + port_name + " master " + type + " vlan " + info->vid.substr(4) + proto_string;

    std::string res;
    int ret = swss::exec(cmds, res);
    if (ret != 0)
    {
        SWSS_LOG_ERROR("Failed cmd:%s, res=%s, ret=%d", cmds.c_str(), res.c_str(), ret);
    }
    else
    {
        SWSS_LOG_INFO("Success cmd:%s, res=%s, ret=%d", cmds.c_str(), res.c_str(), ret);
    }

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
    std::string proto_string = "";

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
            proto_string = " proto hw";
        }
        else
        {
            type = "static";
        }

        const std::string cmds = std::string("")
                + " bridge fdb " + op + " " + mac + " dev "
                + port_name + " master " + type  + " vlan " + vlan
                + proto_string;

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
    std::string proto_string = "";

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
        proto_string = " proto hw";
    }
    else
    {
        type = "static";
    }

    const std::string cmds = std::string("")
        + " bridge fdb " + op + " " + info->mac + " dev "
        + port_name + " master " + type + " vlan " + info->vid.substr(4) + proto_string;

    std::string res;
    int ret = swss::exec(cmds, res);

    SWSS_LOG_INFO("cmd:%s, res=%s, ret=%d", cmds.c_str(), res.c_str(), ret);

    return;
}

void FdbSync::updateMclagRemoteMacPort(int ifindex, int vlan, std::string mac, uint8_t protocol)
{
    string key = "Vlan" + to_string(vlan) + ":" + mac;
    int type = 0;
    string port_name = "";
    std::string proto_string = "";

    SWSS_LOG_INFO("Updating Intf %d, Vlan:%d MAC:%s Key %s", ifindex, vlan, mac.c_str(), key.c_str());

    if (m_mclag_remote_fdb_mac.find(key) != m_mclag_remote_fdb_mac.end())
    {
        type = m_mclag_remote_fdb_mac[key].type;
        port_name = m_mclag_remote_fdb_mac[key].port_name;
        if (protocol == RTPROT_ZEBRA)
            proto_string = " proto zebra";
        else if (protocol == 193)/*RTPRO_HW might not be in swss inc rtnetlink.h*/
            /* Unlikely this can happen, but keeping just in case */
            proto_string = " proto hw";
        SWSS_LOG_INFO(" port %s, type %d %s\n", port_name.c_str(), type, proto_string.c_str());

        if (type == FDB_TYPE_STATIC)
        {
            const std::string cmds = std::string("")
                + " bridge fdb replace" + " " + mac + " dev "
                + port_name + " master static vlan " + to_string(vlan) + proto_string;

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
void FdbSync::macRefreshStateDB(int vlan, string kmac, uint8_t protocol)
{
    string key = "Vlan" + to_string(vlan) + ":" + kmac;
    char *type;
    string port_name = "";
    std::string proto_string = "";

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

        if (protocol == RTPROT_ZEBRA)
            proto_string = " proto zebra";
        else if (protocol == 193)/*RTPRO_HW might not be in swss inc rtnetlink.h*/
            proto_string = " proto hw";

        const std::string cmds = std::string("")
            + " bridge fdb " + "replace" + " " + kmac + " dev "
            + port_name + " master " + type  + " vlan " + to_string(vlan) + proto_string;

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
            (m_AppRestartAssist && m_AppRestartAssist->isWarmStartInProgress()) ? "WARM-RESTART:" : "",
            key.c_str(), inet_ntoa(vtep), vlan_id.c_str());

    std::vector<FieldValueTuple> fvVector;
    FieldValueTuple f("vni", to_string(vni));
    fvVector.push_back(f);

    // If warmstart is in progress, we take all netlink changes into the cache map
    if (m_AppRestartAssist && m_AppRestartAssist->isWarmStartInProgress())
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
            (m_AppRestartAssist && m_AppRestartAssist->isWarmStartInProgress()) ? "WARM-RESTART:" : "",
            key.c_str(), inet_ntoa(vtep), vlan_id.c_str());

    std::vector<FieldValueTuple> fvVector;
    FieldValueTuple f("vni", to_string(vni));
    fvVector.push_back(f);

    // If warmstart is in progress, we take all netlink changes into the cache map
    if (m_AppRestartAssist && m_AppRestartAssist->isWarmStartInProgress())
    {
        m_AppRestartAssist->insertToMap(APP_VXLAN_REMOTE_VNI_TABLE_NAME, key, fvVector, true);
        return;
    }
    
    m_imetTable.del(key);
    return;
}

void FdbSync::macDelVxlanDB(string key)
{
    std::vector<FieldValueTuple> fvVector;
    string type = m_mac[key].type;
    string vni = to_string(m_mac[key].vni);
    string ifname = m_mac[key].ifname;
    string protocol = to_string(m_mac[key].protocol);

    FieldValueTuple t("type", type);
    FieldValueTuple v("vni", vni);
    FieldValueTuple f("ifname", ifname);
    FieldValueTuple p("protocol", protocol);

    if (m_mac[key].nhtype == VTEP)
    {
        // VTEP destination
        string vtep = m_mac[key].v.remote_vtep;
        FieldValueTuple nh("remote_vtep", vtep);
        fvVector.push_back(nh);
        SWSS_LOG_NOTICE("VXLAN_FDB_TABLE: DEL_KEY %s vtep:%s type:%s vni:%s protocol:%u",
                        key.c_str(), vtep.c_str(), type.c_str(), vni.c_str(), m_mac[key].protocol);
    }
    else if (m_mac[key].nhtype == NEXTHOPGROUP)
    {
        // Nexthop group destination
        string nexthop_group = m_mac[key].v.nexthop_group;
        FieldValueTuple nh("nexthop_group", nexthop_group);
        fvVector.push_back(nh);
        SWSS_LOG_NOTICE("VXLAN_FDB_TABLE: DEL_KEY %s nexthop_group:%s type:%s vni:%s protocol:%u",
                        key.c_str(), nexthop_group.c_str(), type.c_str(), vni.c_str(), m_mac[key].protocol);
    }
    else if (m_mac[key].nhtype == IFNAME)
    {
        // Interface name destination
        string ifname = m_mac[key].v.ifname;
        FieldValueTuple nh("ifname", ifname);
        fvVector.push_back(nh);
        SWSS_LOG_INFO("VXLAN_FDB_TABLE: DEL_KEY %s ifname:%s type:%s vni:%s protocol:%u",
                        key.c_str(), ifname.c_str(), type.c_str(), vni.c_str(), m_mac[key].protocol);
    }

    fvVector.push_back(t);
    fvVector.push_back(v);
    fvVector.push_back(f);
    fvVector.push_back(p);

    // If warmstart is in progress, we take all netlink changes into the cache map
    if (m_AppRestartAssist && m_AppRestartAssist->isWarmStartInProgress())
    {
        m_AppRestartAssist->insertToMap(APP_VXLAN_FDB_TABLE_NAME, key, fvVector, true);
        return;
    }
    
    m_fdbTable.del(key);
    return;

}

void FdbSync::macAddVxlan(string key, struct nl_addr *vtep, string type, uint32_t vni, string intf_name,
     string nexthop_group, NEXT_HOP_VALUE_TYPE dest_type, uint8_t protocol)
{
    std::vector<FieldValueTuple> fvVector;
    string svni = to_string(vni);

    /* Update the DB with Vxlan MAC */
    m_mac[key].type = type;
    m_mac[key].vni = vni;
    m_mac[key].ifname = intf_name;
    m_mac[key].protocol = protocol;

    FieldValueTuple fv_type("type", type);
    FieldValueTuple fv_vni("vni", svni);
    FieldValueTuple fv_protocol("protocol", to_string(protocol));

    if (dest_type == NEXTHOPGROUP)
    {
        if (m_mac.find(key) != m_mac.end())
            m_fdbTable.del(key);
        m_mac[key].nhtype = NEXTHOPGROUP;
        m_mac[key].v.nexthop_group = nexthop_group;
        FieldValueTuple nh("nexthop_group", nexthop_group);
        fvVector.push_back(nh);
        SWSS_LOG_INFO("VXLAN_FDB_TABLE: ADD_KEY %s nexthop_group:%s type:%s vni:%s ifname:%s protocol:%u",
                      key.c_str(), nexthop_group.c_str(), type.c_str(), svni.c_str(), intf_name.c_str(), protocol);
    }
    else if (dest_type == VTEP)
    {
        if (m_mac.find(key) != m_mac.end())
            m_fdbTable.del(key);
        char buf[MAX_ADDR_SIZE + 1] = {0};
        m_mac[key].nhtype = VTEP;
        string svtep = nl_addr2str(vtep, buf, sizeof(buf));
        m_mac[key].v.remote_vtep = svtep;
        FieldValueTuple nh("remote_vtep", svtep);
        fvVector.push_back(nh);
        SWSS_LOG_INFO("VXLAN_FDB_TABLE: ADD_KEY %s vtep:%s type:%s vni:%s ifname:%s protocol:%u",
                      key.c_str(), svtep.c_str(), type.c_str(), svni.c_str(), intf_name.c_str(), protocol);
    }
    else if (dest_type == IFNAME)
    {
        if (m_mac.find(key) != m_mac.end())
            m_fdbTable.del(key);
        m_mac[key].nhtype = IFNAME;
        m_mac[key].v.ifname = intf_name;
        FieldValueTuple fv_ifname("ifname", intf_name);
        fvVector.push_back(fv_ifname);
        SWSS_LOG_INFO("VXLAN_FDB_TABLE: ADD_KEY %s ifname:%s type:%s vni:%s ifname:%s protocol:%u",
                      key.c_str(), intf_name.c_str(), type.c_str(), svni.c_str(), intf_name.c_str(), protocol);
    } else {
        SWSS_LOG_ERROR("VXLAN_FDB_TABLE: dest_type:%d is invalid, ADD_KEY %s type:%s vni:%s ifname:%s",
                dest_type, key.c_str(), type.c_str(), svni.c_str(), intf_name.c_str());
        return;
    }

    fvVector.push_back(fv_type);
    fvVector.push_back(fv_vni);
    fvVector.push_back(fv_protocol);

    // If warmstart is in progress, we take all netlink changes into the cache map
    if (m_AppRestartAssist && m_AppRestartAssist->isWarmStartInProgress())
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
        if (m_mac[key].nhtype == VTEP)
        {
            SWSS_LOG_INFO("DEL_KEY %s vtep:%s type:%s vni:%s ifname:%s protocol:%u", key.c_str(),
                           m_mac[key].v.remote_vtep.c_str(), m_mac[key].type.c_str(),
                           to_string(m_mac[key].vni).c_str(), m_mac[key].ifname.c_str(), m_mac[key].protocol);
        }
        else if (m_mac[key].nhtype == NEXTHOPGROUP)
        {
            SWSS_LOG_INFO("DEL_KEY %s nexthop:%s type:%s vni:%s ifname:%s protocol:%u", key.c_str(),
                           m_mac[key].v.nexthop_group.c_str(), m_mac[key].type.c_str(),
                           to_string(m_mac[key].vni).c_str(), m_mac[key].ifname.c_str(), m_mac[key].protocol);
        }
        else if (m_mac[key].nhtype == IFNAME)
        {
            SWSS_LOG_INFO("DEL_KEY %s ifname:%s type:%s vni:%s protocol:%u", key.c_str(),
                           m_mac[key].v.ifname.c_str(), m_mac[key].type.c_str(),
                           to_string(m_mac[key].vni).c_str(), m_mac[key].protocol);
        }
        else {
            SWSS_LOG_ERROR("DEL_KEY %s type: %s vni:%s ifname:%s protocol:%u, entry nhtype is invalid, nhtype: %d", key.c_str(),
                m_mac[key].type.c_str(), to_string(m_mac[key].vni).c_str(), m_mac[key].ifname.c_str(), m_mac[key].protocol, m_mac[key].nhtype);

        }
        macDelVxlanDB(key);
        m_mac.erase(key);
    } else {
        SWSS_LOG_ERROR("DEL_KEY %s entry doesn't exist", key.c_str());
    }
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
    uint8_t protocol = RTPROT_UNSPEC;

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
                updateMclagRemoteMacPort(ifindex, vid, macStr, protocol);
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
            macRefreshStateDB(vlan, macStr, protocol);
        }
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
        macAddVxlan(key, vtep_addr, type, vni, ifname, "0", VTEP, protocol);
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

void FdbSync::onMsgNhg(struct nlmsghdr *msg)
{
    struct nhmsg *nhm = (struct nhmsg *)NLMSG_DATA(msg);
    int len = (int)(msg->nlmsg_len - NLMSG_LENGTH(sizeof(*nhm)));

    uint32_t nhid = 0;
    bool has_gateway = false;
    bool has_group = false;
    bool has_oif = false;
    struct in_addr v4addr;
    struct in6_addr v6addr;
    struct nexthop_grp *grp = NULL;
    int grp_count = 0;

    memset(&v4addr, 0, sizeof(v4addr));
    memset(&v6addr, 0, sizeof(v6addr));

    struct rtattr *rta = (struct rtattr *)((char *)nhm + NLMSG_ALIGN(sizeof(*nhm)));

    for (; RTA_OK(rta, len); rta = RTA_NEXT(rta, len))
    {
        switch (rta->rta_type)
        {
        case NHA_ID:
            nhid = *(uint32_t *)RTA_DATA(rta);
            break;
        case NHA_GATEWAY:
            has_gateway = true;
            if (nhm->nh_family == AF_INET)
            {
                memcpy(&v4addr, RTA_DATA(rta), sizeof(struct in_addr));
            }
            else if (nhm->nh_family == AF_INET6)
            {
                memcpy(&v6addr, RTA_DATA(rta), sizeof(struct in6_addr));
            }
            break;
        case NHA_GROUP:
            has_group = true;
            grp = (struct nexthop_grp *)RTA_DATA(rta);
            grp_count = (int)(RTA_PAYLOAD(rta) / sizeof(struct nexthop_grp));
            break;
        case NHA_OIF:
            has_oif = true;
            break;
        case NHA_FDB:
            /* Flag attribute, no data to extract */
            break;
        default:
            break;
        }
    }

    if (msg->nlmsg_type == RTM_NEWNEXTHOP)
    {
        /* Drop messages with OIF (not an L2 NHG we care about) */
        if (has_oif)
        {
            SWSS_LOG_INFO("NHG %u has OIF, dropping", nhid);
            return;
        }

        if (has_gateway)
        {
            char ip_str[INET6_ADDRSTRLEN] = {0};

            if (nhm->nh_family == AF_INET)
            {
                inet_ntop(AF_INET, &v4addr, ip_str, sizeof(ip_str));

                /* Filter out IPv4 link-local addresses (169.254.x.x) */
                if ((ntohl(v4addr.s_addr) & 0xFFFF0000) == 0xA9FE0000)
                {
                    SWSS_LOG_INFO("NHG %u has link-local IPv4 address %s, dropping", nhid, ip_str);
                    return;
                }
            }
            else if (nhm->nh_family == AF_INET6)
            {
                inet_ntop(AF_INET6, &v6addr, ip_str, sizeof(ip_str));

                /* Filter out IPv6 link-local addresses (fe80::) */
                if (v6addr.s6_addr[0] == 0xfe && (v6addr.s6_addr[1] & 0xc0) == 0x80)
                {
                    SWSS_LOG_INFO("NHG %u has link-local IPv6 address %s, dropping", nhid, ip_str);
                    return;
                }
            }
            else
            {
                SWSS_LOG_INFO("NHG %u has unsupported address family %d, dropping", nhid, nhm->nh_family);
                return;
            }

            std::string ip_string(ip_str);

            /* Write to L2_NEXTHOP_GROUP_TABLE */
            std::vector<FieldValueTuple> fvVector;
            FieldValueTuple fv("remote_vtep", ip_string);
            fvVector.push_back(fv);
            m_l2NhgTable.set(to_string(nhid), fvVector);

            /* Store in internal map */
            l2_nhg_info info;
            info.type = L2_NHG_TYPE_VTEP;
            info.vtep_ip = ip_string;
            m_l2NhgMap[nhid] = info;

            SWSS_LOG_INFO("L2_NEXTHOP_GROUP_TABLE: ADD nhid=%u remote_vtep=%s", nhid, ip_string.c_str());
        }
        else if (has_group && grp != NULL && grp_count > 0)
        {
            /* Validate all member IDs exist in the internal map */
            std::vector<uint32_t> member_ids;
            for (int i = 0; i < grp_count; i++)
            {
                if (m_l2NhgMap.find(grp[i].id) == m_l2NhgMap.end())
                {
                    SWSS_LOG_INFO("NHG %u references unknown member %u, dropping", nhid, grp[i].id);
                    return;
                }
                member_ids.push_back(grp[i].id);
            }

            /* Build comma-separated member ID string */
            std::string nhg_str;
            for (size_t i = 0; i < member_ids.size(); i++)
            {
                if (i > 0) nhg_str += ",";
                nhg_str += to_string(member_ids[i]);
            }

            /* Write to L2_NEXTHOP_GROUP_TABLE */
            std::vector<FieldValueTuple> fvVector;
            FieldValueTuple fv("nexthop_group", nhg_str);
            fvVector.push_back(fv);
            m_l2NhgTable.set(to_string(nhid), fvVector);

            /* Store in internal map */
            l2_nhg_info info;
            info.type = L2_NHG_TYPE_GROUP;
            info.member_ids = member_ids;
            m_l2NhgMap[nhid] = info;

            SWSS_LOG_INFO("L2_NEXTHOP_GROUP_TABLE: ADD nhid=%u nexthop_group=%s", nhid, nhg_str.c_str());
        }
    }
    else if (msg->nlmsg_type == RTM_DELNEXTHOP)
    {
        /* Delete from L2_NEXTHOP_GROUP_TABLE */
        m_l2NhgTable.del(to_string(nhid));
        m_l2NhgMap.erase(nhid);

        SWSS_LOG_INFO("L2_NEXTHOP_GROUP_TABLE: DEL nhid=%u", nhid);

        /* Update any GROUP entries that reference this deleted NHG ID */
        std::vector<uint32_t> groups_to_delete;

        for (auto &entry : m_l2NhgMap)
        {
            if (entry.second.type != L2_NHG_TYPE_GROUP)
                continue;

            auto &members = entry.second.member_ids;
            auto it = std::find(members.begin(), members.end(), nhid);
            if (it == members.end())
                continue;

            /* Remove this member from the group */
            members.erase(it);

            if (members.empty())
            {
                /* Group is now empty, mark for deletion */
                groups_to_delete.push_back(entry.first);
            }
            else
            {
                /* Update the group's table entry with remaining members */
                std::string nhg_str;
                for (size_t i = 0; i < members.size(); i++)
                {
                    if (i > 0) nhg_str += ",";
                    nhg_str += to_string(members[i]);
                }

                std::vector<FieldValueTuple> fvVector;
                FieldValueTuple fv("nexthop_group", nhg_str);
                fvVector.push_back(fv);
                m_l2NhgTable.set(to_string(entry.first), fvVector);

                SWSS_LOG_INFO("L2_NEXTHOP_GROUP_TABLE: UPDATE nhid=%u nexthop_group=%s", entry.first, nhg_str.c_str());
            }
        }

        /* Delete empty groups */
        for (auto gid : groups_to_delete)
        {
            m_l2NhgTable.del(to_string(gid));
            m_l2NhgMap.erase(gid);
            SWSS_LOG_INFO("L2_NEXTHOP_GROUP_TABLE: DEL empty group nhid=%u", gid);
        }
    }
}

void FdbSync::onMsgNbrRaw(struct nlmsghdr *msg)
{
    struct ndmsg *ndm = (struct ndmsg *)NLMSG_DATA(msg);
    int len = (int)(msg->nlmsg_len - NLMSG_LENGTH(sizeof(*ndm)));

    /* Only MAC routes (AF_BRIDGE) */
    if (ndm->ndm_family != AF_BRIDGE)
    {
        return;
    }

    uint32_t nhid = 0;
    uint16_t vlan_id = 0;
    char macStr[MAX_ADDR_SIZE + 1] = {0};
    bool has_nhid = false;
    bool has_dst = false;
    struct in_addr dst_v4 = {0};
    uint32_t ext_flags __attribute__((unused)) = 0;

    struct rtattr *rta = (struct rtattr *)((char *)ndm + NLMSG_ALIGN(sizeof(*ndm)));

    for (; RTA_OK(rta, len); rta = RTA_NEXT(rta, len))
    {
        switch (rta->rta_type)
        {
        case NDA_NH_ID:
            nhid = *(uint32_t *)RTA_DATA(rta);
            has_nhid = true;
            break;
        case NDA_VLAN:
            vlan_id = *(uint16_t *)RTA_DATA(rta);
            break;
        case NDA_LLADDR:
            {
                unsigned char *addr = (unsigned char *)RTA_DATA(rta);
                snprintf(macStr, sizeof(macStr), "%02x:%02x:%02x:%02x:%02x:%02x",
                         addr[0], addr[1], addr[2], addr[3], addr[4], addr[5]);
            }
            break;
        case NDA_DST:
            has_dst = true;
            if (RTA_PAYLOAD(rta) == sizeof(struct in_addr))
            {
                memcpy(&dst_v4, RTA_DATA(rta), sizeof(struct in_addr));
            }
            break;
        case NDA_FLAGS_EXT:
            ext_flags = *(uint32_t *)RTA_DATA(rta);
            break;
        default:
            break;
        }
    }

    int ifindex = ndm->ndm_ifindex;
    bool isVxlanIntf = false;
    string ifname;
    uint32_t vni = 0;

    if (m_intf_info.find(ifindex) != m_intf_info.end())
    {
        isVxlanIntf = true;
        ifname = m_intf_info[ifindex].ifname;
        vni = m_intf_info[ifindex].vni;
    }

    if (!isVxlanIntf)
    {
        return;
    }

    /* Build the vlan_id string from interface name */
    string vlan_str;
    size_t str_loc = ifname.rfind("-");
    if (str_loc != string::npos)
    {
        vlan_str = "Vlan" + ifname.substr(str_loc + 1, string::npos);
    }
    else if (vlan_id > 0)
    {
        vlan_str = "Vlan" + to_string(vlan_id);
    }
    else
    {
        return;
    }

    string key = vlan_str + ":" + macStr;

    bool delete_key = (msg->nlmsg_type == RTM_DELNEIGH);
    int state = ndm->ndm_state;
    if ((state == NUD_INCOMPLETE) || (state == NUD_FAILED))
    {
        delete_key = true;
    }

    string type = (state & NUD_NOARP) ? "static" : "dynamic";

    if (delete_key)
    {
        macDelVxlan(key);
        return;
    }

    if (has_nhid && nhid != 0)
    {
        /* MAC points to a nexthop group */
        macAddVxlan(key, NULL, type, vni, ifname, to_string(nhid), NEXTHOPGROUP, RTPROT_UNSPEC);
    }
    else if (has_dst && dst_v4.s_addr != 0)
    {
        /* MAC points to a remote VTEP */
        struct nl_addr *vtep_addr = nl_addr_build(AF_INET, &dst_v4, sizeof(dst_v4));
        macAddVxlan(key, vtep_addr, type, vni, ifname, "0", VTEP, RTPROT_UNSPEC);
        nl_addr_put(vtep_addr);
    }
}

void FdbSync::onMsgRaw(struct nlmsghdr *msg)
{
    if (!msg)
    {
        SWSS_LOG_ERROR("Received NULL message");
        return;
    }

    if (msg->nlmsg_type == RTM_NEWLINK)
    {
        struct nl_object *obj = (struct nl_object *)NLMSG_DATA(msg);
        onMsg(msg->nlmsg_type, obj);
    }
    else if (msg->nlmsg_type == RTM_NEWNEIGH || msg->nlmsg_type == RTM_DELNEIGH)
    {
        onMsgNbrRaw(msg);
    }
    else if (msg->nlmsg_type == RTM_NEWNEXTHOP || msg->nlmsg_type == RTM_DELNEXTHOP)
    {
        onMsgNhg(msg);
    }
}
