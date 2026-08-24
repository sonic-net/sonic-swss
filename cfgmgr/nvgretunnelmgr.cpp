#include <sstream>
#include "logger.h"
#include "exec.h"
#include "shellcmd.h"
#include "ipaddress.h"
#include "nvgretunnelmgr.h"

using namespace std;
using namespace swss;

#define NVGRE_DEFAULT_TTL   "64"
#define NVGRE_DEFAULT_BRIDGE "Bridge"

/* One gretap device per (tunnel, vsid). IFNAMSIZ is 16 (15 chars + NUL). */
static string mapDeviceName(const string &tunnel, const string &vsid)
{
    string dev = tunnel + "_" + vsid;
    if (dev.size() > 15)
        dev.resize(15);
    return dev;
}

/* Parse a VSID string to a uint64_t; reject empty / non-numeric / oversized input. */
static bool parseVsid(const string &vsid, uint64_t &out)
{
    if (vsid.empty() || vsid.size() > 10)
        return false;
    for (char c : vsid)
        if (c < '0' || c > '9')
            return false;
    try
    {
        out = stoull(vsid);
    }
    catch (...)
    {
        return false;
    }
    return true;
}

NvgreTunnelMgr::NvgreTunnelMgr(DBConnector *cfgDb, DBConnector *stateDb,
                               const vector<string> &tableNames) :
    Orch(cfgDb, stateDb, tableNames, {}),
    m_stateNvgreTunnelTable(stateDb, STATE_NVGRE_TUNNEL_TABLE_NAME),
    m_stateNvgreTunnelMapTable(stateDb, STATE_NVGRE_TUNNEL_MAP_TABLE_NAME),
    m_cfgVlanTable(cfgDb, "VLAN")
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

bool NvgreTunnelMgr::interfaceExists(const string &dev)
{
    string cmd = string(IP_CMD) + " link show " + dev;
    string res;
    return swss::exec(cmd, res) == 0;
}

bool NvgreTunnelMgr::vlanExists(const string &vlanId)
{
    vector<FieldValueTuple> fvs;
    return m_cfgVlanTable.get("Vlan" + vlanId, fvs);
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
            vector<string> unknown_fields;

            for (auto i : kfvFieldsValues(t))
            {
                string field = fvField(i);
                string value = fvValue(i);
                if (field == NVGRE_FIELD_SRC_IP)
                    src_ip = value;
                else
                    unknown_fields.push_back(field);
            }

            SWSS_LOG_NOTICE("NVGRE_TUNNEL SET: %s src_ip=%s", tunnel_name.c_str(), src_ip.c_str());

            bool ok = true;
            string reason;

            if (!unknown_fields.empty())
            {
                ok = false;
                for (auto &uf : unknown_fields)
                    SWSS_LOG_WARN("NVGRE_TUNNEL %s: unknown field '%s'", tunnel_name.c_str(), uf.c_str());
                reason = "unknown field";
            }
            else if (src_ip.empty())
            {
                ok = false;
                reason = "missing mandatory src_ip";
            }
            else
            {
                try
                {
                    IpAddress ip(src_ip);
                    (void)ip;
                }
                catch (...)
                {
                    ok = false;
                    reason = "invalid src_ip";
                }
            }

            if (!ok)
            {
                SWSS_LOG_WARN("NVGRE_TUNNEL %s rejected: %s", tunnel_name.c_str(), reason.c_str());
            }
            else
            {
                /* If src_ip changed, re-program any already-created maps so their
                 * gretap `local` tracks the new VTEP source IP. programMap() does
                 * delete-before-add, so this is idempotent. */
                bool changed = (m_tunnelSrcIp.find(tunnel_name) == m_tunnelSrcIp.end()) ||
                               (m_tunnelSrcIp[tunnel_name] != src_ip);

                m_tunnelSrcIp[tunnel_name] = src_ip;

                if (changed)
                {
                    string prefix = tunnel_name + "|";
                    for (auto &entry : m_mapDev)
                    {
                        if (entry.first.compare(0, prefix.size(), prefix) == 0)
                            programMap(entry.first, entry.second.vsid, entry.second.vlanId, src_ip);
                    }
                }
            }

            vector<FieldValueTuple> fvs;
            fvs.emplace_back("status", ok ? "active" : "inactive");
            m_stateNvgreTunnelTable.set(tunnel_name, fvs);

            it = consumer.m_toSync.erase(it);
        }
        else if (op == DEL_COMMAND)
        {
            SWSS_LOG_NOTICE("NVGRE_TUNNEL DEL: %s", tunnel_name.c_str());

            removeTunnelCascade(tunnel_name);
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
        string key = kfvKey(t);
        string op = kfvOp(t);

        /* Composite key: <tunnel>|<map> */
        string tunnel, map_name;
        size_t sep = key.find('|');
        if (sep != string::npos)
        {
            tunnel = key.substr(0, sep);
            map_name = key.substr(sep + 1);
        }
        else
        {
            SWSS_LOG_WARN("NVGRE_TUNNEL_MAP: malformed key '%s'", key.c_str());
            it = consumer.m_toSync.erase(it);
            continue;
        }

        SWSS_LOG_INFO("NVGRE_TUNNEL_MAP: tunnel=%s map=%s op=%s",
                      tunnel.c_str(), map_name.c_str(), op.c_str());

        if (op == SET_COMMAND)
        {
            string vsid, vlan_id;
            vector<string> unknown_fields;

            for (auto i : kfvFieldsValues(t))
            {
                string field = fvField(i);
                string value = fvValue(i);
                if (field == NVGRE_MAP_FIELD_VSID)
                    vsid = value;
                else if (field == NVGRE_MAP_FIELD_VLAN_ID)
                    vlan_id = value;
                else
                    unknown_fields.push_back(field);
            }

            SWSS_LOG_NOTICE("NVGRE_TUNNEL_MAP SET: %s vsid=%s vlan_id=%s",
                            key.c_str(), vsid.c_str(), vlan_id.c_str());

            bool ok = true;
            string reason;

            if (!unknown_fields.empty())
            {
                ok = false;
                for (auto &uf : unknown_fields)
                    SWSS_LOG_WARN("NVGRE_TUNNEL_MAP %s: unknown field '%s'", key.c_str(), uf.c_str());
                reason = "unknown field";
            }
            else if (vsid.empty() || vlan_id.empty())
            {
                ok = false;
                reason = "missing mandatory vsid/vlan_id";
            }
            else
            {
                uint64_t vsidVal = 0;
                if (!parseVsid(vsid, vsidVal) || vsidVal == 0 || vsidVal > NVGRE_VSID_MAX_VALUE)
                {
                    ok = false;
                    reason = "invalid vsid";
                }
                else if (!vlanExists(vlan_id))
                {
                    ok = false;
                    reason = "vlan does not exist";
                }
            }

            if (!ok)
            {
                SWSS_LOG_WARN("NVGRE_TUNNEL_MAP %s rejected: %s", key.c_str(), reason.c_str());
                vector<FieldValueTuple> fvs;
                fvs.emplace_back("status", "inactive");
                m_stateNvgreTunnelMapTable.set(key, fvs);
                it = consumer.m_toSync.erase(it);
                continue;
            }

            auto srcIt = m_tunnelSrcIp.find(tunnel);
            if (srcIt == m_tunnelSrcIp.end())
            {
                /* Parent tunnel not configured yet — defer, retried on next doTask. */
                SWSS_LOG_WARN("NVGRE_TUNNEL_MAP %s: tunnel %s not configured, deferring",
                              key.c_str(), tunnel.c_str());
                it++;
                continue;
            }

            bool programmed = programMap(key, vsid, vlan_id, srcIt->second);

            vector<FieldValueTuple> fvs;
            fvs.emplace_back("status", programmed ? "active" : "inactive");
            m_stateNvgreTunnelMapTable.set(key, fvs);

            it = consumer.m_toSync.erase(it);
        }
        else if (op == DEL_COMMAND)
        {
            SWSS_LOG_NOTICE("NVGRE_TUNNEL_MAP DEL: %s", key.c_str());
            removeMap(key);
            it = consumer.m_toSync.erase(it);
        }
        else
        {
            SWSS_LOG_ERROR("NVGRE_TUNNEL_MAP: unknown operation '%s'", op.c_str());
            it = consumer.m_toSync.erase(it);
        }
    }
}

bool NvgreTunnelMgr::programMap(const string &key, const string &vsid,
                                const string &vlanId, const string &srcIp)
{
    SWSS_LOG_ENTER();

    string tunnel = key.substr(0, key.find('|'));
    string dev = mapDeviceName(tunnel, vsid);

    /* Delete-before-add for idempotency (re-SET / src_ip change). */
    if (interfaceExists(dev))
    {
        string del_cmd = string(IP_CMD) + " link del " + dev;
        string ignored;
        swss::exec(del_cmd, ignored);
    }

    /* NVGRE decap: gretap (GRE/TEB) with the VSID as the GRE key. remote 0.0.0.0
     * = decap-any (P2MP termination, matches SAI tunnel termination). */
    ostringstream add_cmd;
    add_cmd << IP_CMD << " link add " << dev << " type gretap local " << srcIp
            << " remote 0.0.0.0 key " << vsid << " ttl " << NVGRE_DEFAULT_TTL;
    SWSS_LOG_NOTICE("Executing: %s", add_cmd.str().c_str());

    string res;
    int ret = swss::exec(add_cmd.str(), res);
    if (ret != 0)
    {
        SWSS_LOG_ERROR("ip link add gretap failed on %s (ret=%d): %s",
                       dev.c_str(), ret, res.c_str());
        return false;
    }

    ostringstream up_cmd;
    up_cmd << IP_CMD << " link set " << dev << " up";
    swss::exec(up_cmd.str(), res);

    ostringstream master_cmd;
    master_cmd << IP_CMD << " link set " << dev << " master " << NVGRE_DEFAULT_BRIDGE;
    swss::exec(master_cmd.str(), res);

    /* VSID -> VLAN: the gretap is an untagged access port on the mapped VLAN. */
    ostringstream vlan_cmd;
    vlan_cmd << BRIDGE_CMD << " vlan add dev " << dev << " vid " << vlanId
             << " pvid untagged";
    SWSS_LOG_NOTICE("Executing: %s", vlan_cmd.str().c_str());

    ret = swss::exec(vlan_cmd.str(), res);
    if (ret != 0)
    {
        SWSS_LOG_ERROR("bridge vlan add failed on %s vid %s (ret=%d): %s",
                       dev.c_str(), vlanId.c_str(), ret, res.c_str());
        string del_cmd = string(IP_CMD) + " link del " + dev;
        string ignored;
        swss::exec(del_cmd, ignored);
        return false;
    }

    m_mapDev[key] = NvgreMapState{dev, vsid, vlanId};

    return true;
}

void NvgreTunnelMgr::removeMap(const string &key)
{
    SWSS_LOG_ENTER();

    auto mit = m_mapDev.find(key);
    if (mit == m_mapDev.end())
    {
        m_stateNvgreTunnelMapTable.del(key);
        return;
    }

    string dev = mit->second.dev;

    string del_cmd = string(IP_CMD) + " link del " + dev;
    string res;
    int ret = swss::exec(del_cmd, res);
    if (ret != 0)
        SWSS_LOG_WARN("ip link del failed on %s (ret=%d): %s", dev.c_str(), ret, res.c_str());

    m_mapDev.erase(mit);
    m_stateNvgreTunnelMapTable.del(key);
}

void NvgreTunnelMgr::removeTunnelCascade(const string &tunnel)
{
    SWSS_LOG_ENTER();

    string prefix = tunnel + "|";
    vector<string> toRemove;
    for (auto &entry : m_mapDev)
        if (entry.first.compare(0, prefix.size(), prefix) == 0)
            toRemove.push_back(entry.first);

    for (auto &key : toRemove)
        removeMap(key);
}
