#include <string.h>
#include <sstream>
#include "logger.h"
#include "exec.h"
#include "shellcmd.h"
#include "schema.h"
#include "nvgretunnelmgr.h"

using namespace std;
using namespace swss;

#define IP_CMD "/sbin/ip"

NvgreTunnelMgr::NvgreTunnelMgr(DBConnector *cfgDb, DBConnector *stateDb,
                               const vector<string> &tableNames) :
    Orch(cfgDb, stateDb, tableNames, {}),
    m_stateNvgreTunnelTable(stateDb, STATE_NVGRE_TUNNEL_TABLE_NAME)
{
    SWSS_LOG_ENTER();
    SWSS_LOG_NOTICE("NvgreTunnelMgr initialized, subscribed to %zu CONFIG_DB tables", tableNames.size());
}

void NvgreTunnelMgr::doTask(Consumer &consumer)
{
    string table_name = consumer.getTableName();

    SWSS_LOG_DEBUG("doTask: table=%s", table_name.c_str());

    if (table_name == CFG_NVGRE_TUNNEL_TABLE_NAME)
        doNvgreTunnelTask(consumer);
    else if (table_name == CFG_NVGRE_TUNNEL_MAP_TABLE_NAME)
        doNvgreTunnelMapTask(consumer);
    else
    {
        SWSS_LOG_ERROR("NvgreTunnelMgr doTask: unknown table '%s'", table_name.c_str());
        throw runtime_error("NvgreTunnelMgr doTask failure: unknown table " + table_name);
    }
}

void NvgreTunnelMgr::doNvgreTunnelTask(Consumer &consumer)
{
    SWSS_LOG_ENTER();

    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
        auto &t = it->second;
        string tunnel_name = kfvKey(t);
        string op = kfvOp(t);

        SWSS_LOG_INFO("NVGRE_TUNNEL: key=%s op=%s", tunnel_name.c_str(), op.c_str());

        if (op == SET_COMMAND)
        {
            string src_ip;
            for (auto i : kfvFieldsValues(t))
            {
                if (fvField(i) == NVGRE_FIELD_SRC_IP)
                    src_ip = fvValue(i);
            }

            SWSS_LOG_NOTICE("NVGRE_TUNNEL SET: %s src_ip=%s", tunnel_name.c_str(), src_ip.c_str());

            if (src_ip.empty())
            {
                SWSS_LOG_WARN("NVGRE_TUNNEL %s: no src_ip, marking inactive", tunnel_name.c_str());
                vector<FieldValueTuple> fvs;
                fvs.emplace_back("status", "inactive");
                m_stateNvgreTunnelTable.set(tunnel_name, fvs);
                it = consumer.m_toSync.erase(it);
                continue;
            }

            m_tunnelSrcIp[tunnel_name] = src_ip;
            vector<FieldValueTuple> fvs;
            fvs.emplace_back("status", "active");
            m_stateNvgreTunnelTable.set(tunnel_name, fvs);

            it = consumer.m_toSync.erase(it);
        }
        else if (op == DEL_COMMAND)
        {
            SWSS_LOG_NOTICE("NVGRE_TUNNEL DEL: %s", tunnel_name.c_str());

            if (m_programmedTunnels.count(tunnel_name))
                removeGreTunnel(tunnel_name);

            m_tunnelSrcIp.erase(tunnel_name);
            m_stateNvgreTunnelTable.del(tunnel_name);

            it = consumer.m_toSync.erase(it);
        }
        else
        {
            SWSS_LOG_ERROR("NVGRE_TUNNEL: unknown operation '%s'", op.c_str());
            it = consumer.m_toSync.erase(it);
        }
    }
}

void NvgreTunnelMgr::doNvgreTunnelMapTask(Consumer &consumer)
{
    SWSS_LOG_ENTER();

    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
        auto &t = it->second;
        string map_name = kfvKey(t);
        string op = kfvOp(t);

        SWSS_LOG_INFO("NVGRE_TUNNEL_MAP: key=%s op=%s", map_name.c_str(), op.c_str());

        if (op == SET_COMMAND)
        {
            string tunnel_name, vsid, vni;
            for (auto i : kfvFieldsValues(t))
            {
                string f = fvField(i);
                string v = fvValue(i);
                if (f == NVGRE_MAP_FIELD_TUNNEL_NAME) tunnel_name = v;
                else if (f == NVGRE_MAP_FIELD_VSID)   vsid = v;
                else if (f == NVGRE_MAP_FIELD_VNI)    vni = v;
            }

            SWSS_LOG_NOTICE("NVGRE_TUNNEL_MAP SET: %s tunnel=%s vsid=%s vni=%s",
                            map_name.c_str(), tunnel_name.c_str(), vsid.c_str(), vni.c_str());

            if (tunnel_name.empty() || vsid.empty())
            {
                SWSS_LOG_WARN("NVGRE_TUNNEL_MAP %s: missing tunnel_name/vsid, skipping", map_name.c_str());
                it = consumer.m_toSync.erase(it);
                continue;
            }

            auto srcIt = m_tunnelSrcIp.find(tunnel_name);
            if (srcIt == m_tunnelSrcIp.end())
            {
                SWSS_LOG_WARN("NVGRE_TUNNEL_MAP %s: tunnel %s not configured, deferring",
                              map_name.c_str(), tunnel_name.c_str());
                it++;   /* defer: retry on next doTask */
                continue;
            }

            if (addGreTunnel(tunnel_name, srcIt->second, vsid))
                m_programmedTunnels.insert(tunnel_name);

            it = consumer.m_toSync.erase(it);
        }
        else if (op == DEL_COMMAND)
        {
            SWSS_LOG_NOTICE("NVGRE_TUNNEL_MAP DEL: %s", map_name.c_str());
            it = consumer.m_toSync.erase(it);
        }
        else
        {
            SWSS_LOG_ERROR("NVGRE_TUNNEL_MAP: unknown operation '%s'", op.c_str());
            it = consumer.m_toSync.erase(it);
        }
    }
}

/*
 * addGreTunnel — program an NVGRE tunnel as a kernel GRE tunnel carrying the
 * VSID in the GRE key.
 *   ip link add <name> type gre local <src_ip> remote 0.0.0.0 key <vsid> ttl 64
 *   ip link set <name> up
 */
bool NvgreTunnelMgr::addGreTunnel(const string &name, const string &srcIp,
                                  const string &vsid)
{
    SWSS_LOG_ENTER();

    ostringstream cmd;
    cmd << IP_CMD << " link add " << name
        << " type gre local " << srcIp
        << " remote 0.0.0.0 key " << vsid
        << " ttl 64";

    SWSS_LOG_NOTICE("Executing: %s", cmd.str().c_str());

    string res;
    int ret = swss::exec(cmd.str(), res);
    if (ret != 0)
    {
        SWSS_LOG_WARN("ip link add gre failed on %s (ret=%d): %s", name.c_str(), ret, res.c_str());
        return false;
    }

    ostringstream up;
    up << IP_CMD << " link set " << name << " up";
    string ignored;
    swss::exec(up.str(), ignored);

    return true;
}

bool NvgreTunnelMgr::removeGreTunnel(const string &name)
{
    SWSS_LOG_ENTER();

    ostringstream cmd;
    cmd << IP_CMD << " link del " << name;

    SWSS_LOG_NOTICE("Executing: %s", cmd.str().c_str());

    string res;
    int ret = swss::exec(cmd.str(), res);
    if (ret != 0)
        SWSS_LOG_WARN("ip link del failed on %s (ret=%d): %s", name.c_str(), ret, res.c_str());

    return true;
}
