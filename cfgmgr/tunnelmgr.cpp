#include <algorithm>
#include <regex>
#include <sstream>
#include <string>
#include <net/if.h>

#include "logger.h"
#include "tunnelmgr.h"
#include "tokenize.h"
#include "shellcmd.h"
#include "exec.h"
#include "warm_restart.h"

using namespace std;
using namespace swss;

#define IPINIP "IPINIP"
#define TUNIF "tun0"
#define LOOPBACK_SRC "Loopback3"

static int cmdIpTunnelIfCreate(const swss::TunnelInfo & info, std::string & res)
{
    // ip tunnel add {{tunnel intf}} mode ipip local {{dst ip}} remote {{remote ip}}
    ostringstream cmd;
    cmd << IP_CMD " tunnel add "
        << TUNIF << " mode ipip local "
        << shellquote(info.dst_ip)
        << " remote "
        << shellquote(info.remote_ip);
    return swss::exec(cmd.str(), res);
}

static int cmdIpTunnelIfRemove(std::string & res)
{
    // ip tunnel del {{tunnel intf}}
    ostringstream cmd;
    cmd << IP_CMD " tunnel del "
        << TUNIF;
    return swss::exec(cmd.str(), res);
}

static int cmdIpTunnelIfUp(std::string & res)
{
    // ip link set dev {{tunnel intf}} up
    ostringstream cmd;
    cmd << IP_CMD " link set dev "
        << TUNIF
        << " up";
    return swss::exec(cmd.str(), res);
}

static int cmdIpTunnelIfAddress(const std::string& ip, std::string & res)
{
    // ip addr add {{loopback3 ip}} dev {{tunnel intf}}
    ostringstream cmd;
    cmd << IP_CMD " addr add "
        << shellquote(ip)
        << " dev "
        << TUNIF;
    return swss::exec(cmd.str(), res);
}

static int cmdIpTunnelRouteAdd(const std::string& pfx, std::string & res)
{
    // ip route add/replace {{ip prefix}} dev {{tunnel intf}}
    // Replace route if route already exists
    ostringstream cmd;
    if (IpPrefix(pfx).isV4())
    {
        cmd << IP_CMD " route replace "
            << shellquote(pfx)
            << " dev "
            << TUNIF;
    }
    else
    {
        cmd << IP_CMD " -6 route replace "
            << shellquote(pfx)
            << " dev "
            << TUNIF;
    }

    return swss::exec(cmd.str(), res);
}

static int cmdIpTunnelRouteDel(const std::string& pfx, std::string & res)
{
    // ip route del {{ip prefix}} dev {{tunnel intf}}
    ostringstream cmd;
    if (IpPrefix(pfx).isV4())
    {
        cmd << IP_CMD " route del "
            << shellquote(pfx)
            << " dev "
            << TUNIF;
    }
    else
    {
        cmd << IP_CMD " -6 route del "
            << shellquote(pfx)
            << " dev "
            << TUNIF;
    }

    return swss::exec(cmd.str(), res);
}

TunnelMgr::TunnelMgr(DBConnector *cfgDb, DBConnector *appDb, const std::vector<std::string> &tableNames) :
        Orch(cfgDb, tableNames),
        m_appIpInIpTunnelTable(appDb, APP_TUNNEL_DECAP_TABLE_NAME),
        m_cfgPeerTable(cfgDb, CFG_PEER_SWITCH_TABLE_NAME),
        m_cfgTunnelTable(cfgDb, CFG_TUNNEL_TABLE_NAME)
{
    std::vector<string> peer_keys;
    m_cfgPeerTable.getKeys(peer_keys);

    for (auto i: peer_keys)
    {
        std::vector<FieldValueTuple> fvs;
        m_cfgPeerTable.get(i, fvs);

        for (auto j: fvs)
        {
            if (fvField(j) == "address_ipv4")
            {
                m_peerIp = fvValue(j);
                break;
            }
        }
    }
    
    if (WarmStart::isWarmStart())
    {
        std::vector<string> tunnel_keys;
        m_cfgTunnelTable.getKeys(tunnel_keys);

        for (auto tunnel: tunnel_keys)
        {
            m_tunnelReplay.insert(tunnel);
        }
        if (m_tunnelReplay.empty())
        {
            finalizeWarmReboot();
        }

    }


    auto consumerStateTable = new swss::ConsumerStateTable(appDb, APP_TUNNEL_ROUTE_TABLE_NAME,
                              TableConsumable::DEFAULT_POP_BATCH_SIZE, default_orch_pri);
    auto consumer = new Consumer(consumerStateTable, this, APP_TUNNEL_ROUTE_TABLE_NAME);
    Orch::addExecutor(consumer);

    // Cleanup any existing tunnel intf
    std::string res;
    cmdIpTunnelIfRemove(res);
}

void TunnelMgr::doTask(Consumer &consumer)
{
    SWSS_LOG_ENTER();

    string table_name = consumer.getTableName();

    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
        bool task_result = true;

        KeyOpFieldsValuesTuple t = it->second;
        const std::string & op = kfvOp(t);

        if (op == SET_COMMAND)
        {
            if (table_name == CFG_LOOPBACK_INTERFACE_TABLE_NAME)
            {
                task_result = doLpbkIntfTask(t);
            }
            else if (table_name == CFG_TUNNEL_TABLE_NAME)
            {
                task_result = doTunnelTask(t);
            }
            else if (table_name == APP_TUNNEL_ROUTE_TABLE_NAME)
            {
                task_result = doTunnelRouteTask(t);
            }
        }
        else if (op == DEL_COMMAND)
        {
            if (table_name == CFG_TUNNEL_TABLE_NAME)
            {
                task_result = doTunnelTask(t);
            }
            else if (table_name == APP_TUNNEL_ROUTE_TABLE_NAME)
            {
                task_result = doTunnelRouteTask(t);
            }
        }
        else
        {
            SWSS_LOG_ERROR("Unknown operation: '%s'", op.c_str());
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

    if (!replayDone && m_tunnelReplay.empty() && WarmStart::isWarmStart())
    {
        finalizeWarmReboot();
    }
}

bool TunnelMgr::doTunnelTask(const KeyOpFieldsValuesTuple & t)
{
    SWSS_LOG_ENTER();

    const std::string & tunnelName = kfvKey(t);
    const std::string & op = kfvOp(t);
    TunnelInfo tunInfo;

    for (auto fieldValue : kfvFieldsValues(t))
    {
        const std::string & field = fvField(fieldValue);
        const std::string & value = fvValue(fieldValue);
        if (field == "dst_ip")
        {
            tunInfo.dst_ip = value;
        }
        else if (field == "tunnel_type")
        {
            tunInfo.type = value;
        }
    }

    if (op == SET_COMMAND)
    {
        if (tunInfo.type == IPINIP)
        {
            tunInfo.remote_ip = m_peerIp;

            if (!m_peerIp.empty() && !configIpTunnel(tunInfo))
            {
                return false;
            }
            else if (m_peerIp.empty())
            {
                SWSS_LOG_NOTICE("Peer/Remote IP not configured");
            }

            /* If the tunnel is already in hardware (i.e. present in the replay),
             * don't try to create it again since it will cause an OA crash
             * (warmboot case)
             */
            if (m_tunnelReplay.find(tunnelName) == m_tunnelReplay.end())
            {
                m_appIpInIpTunnelTable.set(tunnelName, kfvFieldsValues(t));
            }
        }
        m_tunnelReplay.erase(tunnelName);
        m_tunnelCache[tunnelName] = tunInfo;
    }
    else
    {
        auto it = m_tunnelCache.find(tunnelName);

        if (it == m_tunnelCache.end())
        {
            SWSS_LOG_ERROR("Tunnel %s not found", tunnelName.c_str());
            return true;
        }

        tunInfo = it->second;
        if (tunInfo.type == IPINIP)
        {
            m_appIpInIpTunnelTable.del(tunnelName);
        }
        else
        {
            SWSS_LOG_WARN("Tunnel %s type %s is not handled", tunnelName.c_str(), tunInfo.type.c_str());
        }
        m_tunnelCache.erase(tunnelName);
    }
    
    SWSS_LOG_NOTICE("Tunnel %s task, op %s", tunnelName.c_str(), op.c_str());
    return true;
}

bool TunnelMgr::doLpbkIntfTask(const KeyOpFieldsValuesTuple & t)
{
    SWSS_LOG_ENTER();

    vector<string> keys = tokenize(kfvKey(t), config_db_key_delimiter);

    /* Skip entry with just interface name. Need to handle only IP prefix*/
    if (keys.size() == 1)
    {
        return true;
    }

    string alias(keys[0]);
    IpPrefix ipPrefix(keys[1]);

    m_intfCache[alias] = ipPrefix;

    if (alias == LOOPBACK_SRC && !m_tunnelCache.empty())
    {
        int ret = 0;
        std::string res;
        ret = cmdIpTunnelIfAddress(ipPrefix.to_string(), res);
        if (ret != 0)
        {
            SWSS_LOG_WARN("Failed to assign IP addr for tun if %s, res %s",
                           ipPrefix.to_string().c_str(), res.c_str());
        }
    }

    SWSS_LOG_NOTICE("Loopback intf %s saved %s", alias.c_str(), ipPrefix.to_string().c_str());
    return true;
}

bool TunnelMgr::doTunnelRouteTask(const KeyOpFieldsValuesTuple & t)
{
    SWSS_LOG_ENTER();

    const std::string & prefix = kfvKey(t);;
    const std::string & op = kfvOp(t);

    int ret = 0;
    std::string res;
    if (op == SET_COMMAND)
    {
        ret = cmdIpTunnelRouteAdd(prefix, res);
        if (ret != 0)
        {
            SWSS_LOG_WARN("Failed to add route %s, res %s", prefix.c_str(), res.c_str());
        }
    }
    else
    {
        ret = cmdIpTunnelRouteDel(prefix, res);
        if (ret != 0)
        {
            SWSS_LOG_WARN("Failed to del route %s, res %s", prefix.c_str(), res.c_str());
        }
    }

    SWSS_LOG_INFO("Route updated to kernel %s, op %s", prefix.c_str(), op.c_str());
    return true;
}


bool TunnelMgr::configIpTunnel(const TunnelInfo& tunInfo)
{
    int ret = 0;
    std::string res;

    ret = cmdIpTunnelIfCreate(tunInfo, res);
    if (ret != 0)
    {
        SWSS_LOG_WARN("Failed to create IP tunnel if (dst ip: %s, peer ip %s), res %s",
                       tunInfo.dst_ip.c_str(),tunInfo.remote_ip.c_str(), res.c_str());
    }

    ret = cmdIpTunnelIfUp(res);
    if (ret != 0)
    {
        SWSS_LOG_WARN("Failed to enable IP tunnel intf (dst ip: %s, peer ip %s), res %s",
                       tunInfo.dst_ip.c_str(),tunInfo.remote_ip.c_str(), res.c_str());
    }

    auto it = m_intfCache.find(LOOPBACK_SRC);
    if (it != m_intfCache.end())
    {
        ret = cmdIpTunnelIfAddress(it->second.to_string(), res);
        if (ret != 0)
        {
            SWSS_LOG_WARN("Failed to assign IP addr for tun if %s, res %s",
                           it->second.to_string().c_str(), res.c_str());
        }
    }

    return true;
}


void TunnelMgr::finalizeWarmReboot()
{
    replayDone = true;
    WarmStart::setWarmStartState("tunnelmgrd", WarmStart::REPLAYED);
    SWSS_LOG_NOTICE("tunnelmgrd warmstart state set to REPLAYED");
    WarmStart::setWarmStartState("tunnelmgrd", WarmStart::RECONCILED);
    SWSS_LOG_NOTICE("tunnelmgrd warmstart state set to RECONCILED");
}
