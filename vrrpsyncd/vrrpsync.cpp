#include "logger.h"
#include "netmsg.h"
#include "tokenize.h"
#include "linkcache.h"
#include "macaddress.h"
#include "exec.h"
#include "shellcmd.h"
#include <linux/if.h>
#include <netlink/route/link.h>
#include "converter.h"
#include <swss/stringutility.h>

#include "vrrpsync.h"
#include "warm_restart.h"

using namespace std;
using namespace swss;

const string MGMT_PREFIX = "eth";
const string INTFS_PREFIX = "Ethernet";
const string LAG_PREFIX = "PortChannel";

const char CONFIG_DB_KEY_SEPARATOR = '|';
const char APPL_DB_KEY_SEPARATOR = ':';

const char TRACK_INTF_SEPARATOR = ',';
const char TRACK_INTF_AND_WEIGHT_SEPARATOR = '|';

VrrpSync::VrrpSync(RedisPipeline *pipelineAppDB, DBConnector *cfgDb, DBConnector *appDb, DBConnector *stateDb) : 
        m_vrrpTable(pipelineAppDB, APP_VRRP_TABLE_NAME),
        m_cfgVrrpTable(cfgDb, CFG_VRRP_TABLE_NAME),
        m_appVrrpTable(appDb, APP_VRRP_TABLE_NAME)
{
}

void VrrpSync::onMsg(int nlmsg_type, nl_object *obj)
{
    SWSS_LOG_ENTER();

    struct rtnl_link *link = (struct rtnl_link *)obj;
    string intf_name = rtnl_link_get_name(link);

    if (intf_name.substr(0, strlen(VRRP_V4_PREFIX)) != VRRP_V4_PREFIX &&
        intf_name.substr(0, strlen(VRRP_V6_PREFIX)) != VRRP_V6_PREFIX &&
        intf_name.substr(0, strlen(FRONT_PANEL_PORT_PREFIX)) != FRONT_PANEL_PORT_PREFIX &&
        intf_name.substr(0, strlen(PORTCHANNEL_PREFIX)) != PORTCHANNEL_PREFIX &&
        intf_name.substr(0, strlen(VLAN_PREFIX)) != VLAN_PREFIX)
    {
        return;
    }

    if ((nlmsg_type != RTM_NEWLINK) && (nlmsg_type != RTM_GETLINK) &&
        (nlmsg_type != RTM_DELLINK))
    {
        return;
    }

    auto flags = rtnl_link_get_flags(link);
    bool admin = flags & IFF_UP;
    bool oper = flags & IFF_RUNNING;
    bool kernel_state = admin && oper;

    unsigned int ifindex = rtnl_link_get_ifindex(link);
    int link_link = rtnl_link_get_link(link);
    char *link_type = rtnl_link_get_type(link);

    string link_name = link_link != 0 ? LinkCache::getInstance().ifindexToName(link_link) : "";

    SWSS_LOG_DEBUG("intf %s, if: %d, admin: %d, oper: %d, link: %d, link name:%s, link type: %s",
                   intf_name.c_str(), ifindex, admin, oper, link_link, link_name.c_str(), link_type ? link_type : "NONE");

    if (nlmsg_type == RTM_DELLINK)
    {
        m_linkStateList.erase(intf_name);
        SWSS_LOG_INFO("del link state: %s", intf_name.c_str());
        return;
    }
    /* save intf state */
    m_linkStateList[intf_name] = kernel_state;

    if (intf_name.substr(0, strlen(VRRP_V4_PREFIX)) == VRRP_V4_PREFIX ||
        intf_name.substr(0, strlen(VRRP_V6_PREFIX)) == VRRP_V6_PREFIX)
    {
        if (link_name.empty())
        {
            SWSS_LOG_WARN("not found vrrp link interface, ignore it, intf %s", intf_name.c_str());
            return;
        }

        VrrpIntf vrrp(link_name, intf_name);
        if (!vrrp.isValid())
        {
            SWSS_LOG_WARN("vrrp has invaild name, ignore it, intf %s", intf_name.c_str());
            return;
        }
        if (vrrp.isIpv4() && !m_vrrpInfoList[vrrp.getVrid()].netlink_vrrp_ipv4.isValid())
        {
            m_vrrpInfoList[vrrp.getVrid()].netlink_vrrp_ipv4 = vrrp;
        }
        else if (!vrrp.isIpv4() && !m_vrrpInfoList[vrrp.getVrid()].netlink_vrrp_ipv6.isValid())
        {
            m_vrrpInfoList[vrrp.getVrid()].netlink_vrrp_ipv6 = vrrp;
        }
        updateVrrpStateWithIpTpye(vrrp.getParentName(), vrrp.getVrid(), vrrp.isIpv4(), kernel_state);
    }
    else if (intf_name.substr(0, strlen(FRONT_PANEL_PORT_PREFIX)) == FRONT_PANEL_PORT_PREFIX ||
             intf_name.substr(0, strlen(PORTCHANNEL_PREFIX)) == PORTCHANNEL_PREFIX ||
             intf_name.substr(0, strlen(VLAN_PREFIX)) == VLAN_PREFIX)
    {
        for (auto &it : m_vrrpInfoList)
        {
            auto &track = it.second.conf.track;
            if (track.find(intf_name) == track.end())
            {
                continue;
            }

            string parent_name;
            int vrid = 0;
            if (it.second.netlink_vrrp_ipv4.isValid())
            {
                parent_name = it.second.netlink_vrrp_ipv4.getParentName();
                vrid = it.second.netlink_vrrp_ipv4.getVrid();
            }
            else if (it.second.netlink_vrrp_ipv6.isValid())
            {
                parent_name = it.second.netlink_vrrp_ipv6.getParentName();
                vrid = it.second.netlink_vrrp_ipv6.getVrid();
            }
            if (!parent_name.empty())
            {
                int priority = calculateCurrentPriority(it.second.conf);
                if (priority != it.second.effect_priority)
                {
                    it.second.effect_priority = priority;
                    updateVrrpEffectPriority(parent_name, vrid, priority);
                }
            }
        }
    }
}

void VrrpSync::processCfgVrrp()
{
    deque<KeyOpFieldsValuesTuple> entries;
    m_cfgVrrpTable.pops(entries);

    for (auto &entry : entries)
    {
        vector<string> keys = tokenize(kfvKey(entry), CONFIG_DB_KEY_SEPARATOR);
        string op = kfvOp(entry);
        const vector<FieldValueTuple> &data = kfvFieldsValues(entry);
        if (keys.size() != 2)
        {
            continue;
        }

        string intf_alias(keys[0]);
        int vrrp_id = 0;
        /* check vrrp id */
        try
        {
            vrrp_id = to_int<int>(keys[1]);
        }
        catch (const std::exception &e)
        {
            SWSS_LOG_ERROR("vrid: %s is invaild number on vrrp table: %s, ignore. Runtime error: %s",
                           keys[1].c_str(), kfvKey(entry).c_str(), e.what());
            continue;
        }
        if (op == SET_COMMAND)
        {
            string config_priority_str = "";
            string track_intfs = "";
            bool backup_forward = false;
            for (auto i : data)
            {
                if (fvField(i) == "priority")
                {
                    config_priority_str = fvValue(i);
                }
                else if (fvField(i) == "track_interface")
                {
                    track_intfs = fvValue(i);
                }
                else if (fvField(i) == "backup_forward")
                {
                    backup_forward = fvValue(i) == "enabled";
                }
            }

            map<string, int> track;
            int config_priority = VRRP_DEFAULT_PRIORITY;
            /* check priorty */
            if (!config_priority_str.empty())
            {
                try
                {
                    config_priority = to_int<int>(config_priority_str);
                }
                catch (const std::exception &e)
                {
                    SWSS_LOG_ERROR("priority: %s is invaild number on vrrp table: %s, ignore. Runtime error: %s",
                                   config_priority_str.c_str(), kfvKey(entry).c_str(), e.what());
                    continue;
                }
            }
            if (config_priority > 255 or config_priority < 1)
            {
                SWSS_LOG_WARN("priority range of 1-255, intf: %s, priority: %d in vrid: %d",
                              intf_alias.c_str(), config_priority, vrrp_id);
                continue;
            }
            SWSS_LOG_INFO("intf: %s, priority: %d in vrid: %d", intf_alias.c_str(), config_priority, vrrp_id);

            /* check track intf */
            vector<string> track_intf_list = tokenize(track_intfs, TRACK_INTF_SEPARATOR);
            for (string &track_intf : track_intf_list)
            {
                vector<string> intf_weight = tokenize(track_intf, TRACK_INTF_AND_WEIGHT_SEPARATOR);
                if (intf_weight.size() != 3)
                {
                    SWSS_LOG_WARN("track intf has invaild format: %s on vrrp table %s", track_intf.c_str(), kfvKey(entry).c_str());
                    continue;
                }

                string intf = intf_weight[0];
                string weight = intf_weight[2];
                try
                {
                    track[intf] = to_int<int>(weight);
                }
                catch (const std::exception &e)
                {
                    SWSS_LOG_ERROR("weight: %s is invaild number on vrrp table: %s track: %s, ignore. Runtime error: %s",
                                   weight.c_str(), kfvKey(entry).c_str(), track_intf.c_str(), e.what());
                    continue;
                }
                SWSS_LOG_INFO("track intf: %s, weight: %d in vrid: %d", intf.c_str(), track[intf], vrrp_id);
            }

            /* save vrrp config */
            bool backup_forward_change = false;
            if (m_vrrpInfoList.find(vrrp_id) != m_vrrpInfoList.end())
            {
                backup_forward_change = m_vrrpInfoList[vrrp_id].conf.backup_forward != backup_forward;

                m_vrrpInfoList[vrrp_id].conf.priority = config_priority;
                m_vrrpInfoList[vrrp_id].conf.track = track;
                m_vrrpInfoList[vrrp_id].conf.backup_forward = backup_forward;
            }
            else
            {
                backup_forward_change = true;

                VrrpInfo vrrp;
                vrrp.conf.priority = config_priority;
                vrrp.conf.track = track;
                vrrp.conf.backup_forward = backup_forward;
                m_vrrpInfoList[vrrp_id] = vrrp;
            }
            /* update priority and state */
            string parent_name;
            string vrrp_name_v4, vrrp_name_v6;
            if (m_vrrpInfoList[vrrp_id].netlink_vrrp_ipv4.isValid())
            {
                parent_name = m_vrrpInfoList[vrrp_id].netlink_vrrp_ipv4.getParentName();
                vrrp_name_v4 = m_vrrpInfoList[vrrp_id].netlink_vrrp_ipv4.getVrrpName();
            }
            else if (m_vrrpInfoList[vrrp_id].netlink_vrrp_ipv6.isValid())
            {
                parent_name = m_vrrpInfoList[vrrp_id].netlink_vrrp_ipv6.getParentName();
                vrrp_name_v6 = m_vrrpInfoList[vrrp_id].netlink_vrrp_ipv6.getVrrpName();
            }
            if (!parent_name.empty())
            {
                int priority = calculateCurrentPriority(m_vrrpInfoList[vrrp_id].conf);
                if (priority != m_vrrpInfoList[vrrp_id].effect_priority)
                {
                    m_vrrpInfoList[vrrp_id].effect_priority = priority;
                    updateVrrpEffectPriority(parent_name, vrrp_id, priority);
                }
            }

            if (backup_forward_change)
            {
                if (backup_forward)
                {
                    updateVrrpStateWithIpTpye(parent_name, vrrp_id, true, true);
                    updateVrrpStateWithIpTpye(parent_name, vrrp_id, false, true);
                }
                else
                {
                    if (!vrrp_name_v4.empty() && m_linkStateList.find(vrrp_name_v4) != m_linkStateList.end())
                    {
                        bool state = m_linkStateList[vrrp_name_v4];
                        updateVrrpStateWithIpTpye(parent_name, vrrp_id, true, state);
                    }
                    if (!vrrp_name_v6.empty() && m_linkStateList.find(vrrp_name_v6) != m_linkStateList.end())
                    {
                        bool state = m_linkStateList[vrrp_name_v6];
                        updateVrrpStateWithIpTpye(parent_name, vrrp_id, false, state);
                    }
                }
            }
        }
        else if (op == DEL_COMMAND)
        {
            if (m_vrrpInfoList.find(vrrp_id) != m_vrrpInfoList.end())
            {
                SWSS_LOG_NOTICE("del vrrp cache: %d", vrrp_id);
                m_vrrpInfoList.erase(vrrp_id);
            }
            else
            {
                SWSS_LOG_WARN("not found vrid: %d", vrrp_id);
            }
        }
    }
}

/// @brief Calculate the current priority based on the comparison of the input vrrp with the locally cached linkState
/// @return vrrp priority range of 1-255
int VrrpSync::calculateCurrentPriority(const VrrpConf &vrrp)
{
    if (vrrp.track.size() == 0 || m_linkStateList.size() == 0)
    {
        SWSS_LOG_INFO("track size: %d, state size: %d", (int)vrrp.track.size(), (int)m_linkStateList.size());
        return vrrp.priority;
    }

    int appl_priority = vrrp.priority;
    for (auto &it : vrrp.track)
    {
        if (m_linkStateList.find(it.first) != m_linkStateList.end())
        {
            appl_priority = m_linkStateList[it.first] ? appl_priority : appl_priority - it.second;
        }
        if (appl_priority <= 1)
        {
            appl_priority = 1;
            break;
        }
    }
    return appl_priority;
}

bool VrrpSync::updateVrrpEffectPriority(const std::string &parent_name, const int vrid, const int priority)
{
    bool priority_change = false;
    if (updateVrrpEffectPriorityWithIpTpye(parent_name, vrid, true, priority))
    {
        priority_change = true;
    }
    if (updateVrrpEffectPriorityWithIpTpye(parent_name, vrid, false, priority))
    {
        priority_change = true;
    }

    if (priority_change)
    {
        setFrrVrrpPriority(parent_name, vrid, priority);
    }
    else
    {
        SWSS_LOG_NOTICE("not change any vrrp priority: %d to effect, intf: %s vrid: %d",
                        priority, parent_name.c_str(), vrid);
    }

    return true;
}

bool VrrpSync::updateVrrpEffectPriorityWithIpTpye(const std::string &parent_name, const int vrid, const bool is_ipv4, const int priority)
{
    string appl_key = join(APPL_DB_KEY_SEPARATOR, parent_name, to_string(vrid), (is_ipv4 ? IPV4_NAME : IPV6_NAME));

    std::vector<FieldValueTuple> values;
    if (!m_appVrrpTable.get(appl_key, values))
    {
        SWSS_LOG_WARN("vrrp not found intf: %s vrid: %d ipv4: %d in table", parent_name.c_str(), vrid, is_ipv4);
        return false;
    }

    auto vrrp_priority = find_if(values.begin(), values.end(), [](const auto &pair){ return pair.first == "priority"; });
    if (vrrp_priority != values.end())
    {
        if (vrrp_priority->second != to_string(priority))
        {
            SWSS_LOG_INFO("same vrrp priority in effect: %d, intf: %s vrid: %d ipv4: %d",
                          priority, parent_name.c_str(), vrid, is_ipv4);
            return true;
        }
        /* del original priority in appl values */
        values.erase(vrrp_priority);
    }

    SWSS_LOG_NOTICE("vrrp priority: %d to effect, intf: %s vrid: %d ipv4: %d",
                    priority, parent_name.c_str(), vrid, is_ipv4);
    values.emplace_back(FieldValueTuple("priority", to_string(priority)));
    m_vrrpTable.set(appl_key, values);
    return true;
}

// bool VrrpSync::updateVrrpState(const std::string &parent_name, const int vrid, const bool state)
// {
//     bool state_change = false;
//     if (updateVrrpStateWithIpTpye(parent_name, vrid, true, state))
//     {
//         state_change = true;
//     }

//     if (updateVrrpStateWithIpTpye(parent_name, vrid, false, state))
//     {
//         state_change = true;
//     }

//     if (!state_change)
//     {
//         SWSS_LOG_NOTICE("not change any vrrp state: %d to effect, intf: %s vrid: %d",
//                         state, parent_name.c_str(), vrid);
//     }

//     return true;
// }

bool VrrpSync::updateVrrpStateWithIpTpye(const std::string &parent_name, const int vrid, const bool is_ipv4, const bool state)
{
    string appl_key = join(APPL_DB_KEY_SEPARATOR, parent_name, to_string(vrid), (is_ipv4 ? IPV4_NAME : IPV6_NAME));

    std::vector<FieldValueTuple> values;
    if (!m_appVrrpTable.get(appl_key, values))
    {
        SWSS_LOG_WARN("vrrp not found intf: %s vrid: %d ipv4: %d in table", parent_name.c_str(), vrid, is_ipv4);
        return false;
    }
    if (m_vrrpInfoList.find(vrid) != m_vrrpInfoList.end())
    {
        if (m_vrrpInfoList[vrid].conf.backup_forward && !state)
        {
            SWSS_LOG_NOTICE("do not update state down when backup forward enabled, intf: %s vrid: %d ipv4: %d",
                            parent_name.c_str(), vrid, is_ipv4);
            return true;
        }
    }
    auto vrrp_state = find_if(values.begin(), values.end(), [](const auto &pair){ return pair.first == "state"; });
    if (vrrp_state != values.end())
    {
        if ((vrrp_state->second == "up" && state) || (vrrp_state->second == "down" && !state))
        {
            SWSS_LOG_INFO("same vrrp state in effect: %s, intf: %s vrid: %d ipv4: %d",
                          vrrp_state->second.c_str(), parent_name.c_str(), vrid, is_ipv4);
            return true;
        }
        /* del original state in appl values */
        values.erase(vrrp_state);
    }
    SWSS_LOG_NOTICE("vrrp state: %s to effect, intf: %s vrid: %d ipv4: %d",
                    state ? "up" : "down", parent_name.c_str(), vrid, is_ipv4);
    values.emplace_back(FieldValueTuple("state", state ? "up" : "down"));
    m_vrrpTable.set(appl_key, values);
    return true;
}

bool VrrpSync::setFrrVrrpPriority(const std::string &vrrp_name, const int vrid, const int priority)
{
    stringstream cmd;
    string res;

    cmd << "/usr/bin/vtysh"
        << " -c 'configure terminal'"
        << " -c 'interface " << vrrp_name << "'"
        << " -c 'vrrp " << vrid << " priority " << priority << "'";

    SWSS_LOG_DEBUG("cmd: %s", cmd.str().c_str());
    try
    {
        EXEC_WITH_ERROR_THROW(cmd.str(), res);
    }
    catch (const std::exception &e)
    {
        SWSS_LOG_ERROR("fail to set frr vrrp: %s, priority: %d. Runtime error: %s", vrrp_name.c_str(), priority, e.what());
        return false;
    }

    SWSS_LOG_INFO("set frr vrrp: %s, priority: %d", vrrp_name.c_str(), priority);
    return true;
}
