#include <sstream>
#include <cstdio>
#include "logger.h"
#include "exec.h"
#include "shellcmd.h"
#include "ipaddress.h"
#include "nvgretunnelmgr.h"

using namespace std;
using namespace swss;

#define NVGRE_DEFAULT_TTL     "64"
#define NVGRE_DEFAULT_BRIDGE  "Bridge"

/* FNV-1a 32-bit hash — deterministic, collision-resistant for our scale. */
static uint32_t fnv1a(const string &s)
{
    uint32_t h = 2166136261u;
    for (char c : s)
    {
        h ^= (unsigned char)c;
        h *= 16777619u;
    }
    return h;
}

/*
 * Device name is derived from the full composite key via a hash so it is always
 * <= IFNAMSIZ (15 chars) and never silently truncates/collides (Fix 5). "ng" +
 * 8 hex chars of FNV-1a("<tunnel>|<map>") = 10 chars.
 */
static string mapDeviceName(const string &tunnel, const string &mapName)
{
    char buf[16];
    snprintf(buf, sizeof(buf), "ng%08x", static_cast<unsigned int>(fnv1a(tunnel + "|" + mapName)));
    return string(buf);
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

/*
 * Strict name allowlist (Fix 7). tunnel_name/map_name flow into shell commands and
 * (historically) device names, so restrict to [A-Za-z0-9_-] to remove the injection
 * surface. A full netlink rewrite would relax this; this is the documented stopgap.
 */
static bool validName(const string &name)
{
    if (name.empty())
        return false;
    for (char c : name)
    {
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || c == '-' || c == '_';
        if (!ok)
            return false;
    }
    return true;
}

/* Best-effort rollback of a partially-created link. */
static void rollbackDevice(const string &dev)
{
    string cmd = string(IP_CMD) + " link del " + dev;
    string ignored;
    swss::exec(cmd, ignored);
}

NvgreTunnelMgr::NvgreTunnelMgr(DBConnector *cfgDb, DBConnector *stateDb,
                               const vector<string> &tableNames) :
    Orch(cfgDb, stateDb, tableNames, {}),
    m_stateNvgreTunnelTable(stateDb, STATE_NVGRE_TUNNEL_TABLE_NAME),
    m_stateNvgreTunnelMapTable(stateDb, STATE_NVGRE_TUNNEL_MAP_TABLE_NAME),
    m_cfgVlanTable(cfgDb, "VLAN"),
    m_cfgMapTable(cfgDb, CFG_NVGRE_TUNNEL_MAP_TABLE_NAME)
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

bool NvgreTunnelMgr::bridgeExists()
{
    return interfaceExists(NVGRE_DEFAULT_BRIDGE);
}

bool NvgreTunnelMgr::tunnelHasMaps(const string &tunnel)
{
    vector<string> keys;
    m_cfgMapTable.getKeys(keys);
    string prefix = tunnel + "|";
    for (auto &k : keys)
        if (k.compare(0, prefix.size(), prefix) == 0)
            return true;
    return false;
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
            string src_ip, dst_ip;
            vector<string> unknown_fields;

            for (auto i : kfvFieldsValues(t))
            {
                string field = fvField(i);
                string value = fvValue(i);
                if (field == NVGRE_FIELD_SRC_IP)
                    src_ip = value;
                else if (field == NVGRE_FIELD_DST_IP)
                    dst_ip = value;
                else
                    unknown_fields.push_back(field);
            }

            SWSS_LOG_NOTICE("NVGRE_TUNNEL SET: %s src_ip=%s dst_ip=%s",
                            tunnel_name.c_str(), src_ip.c_str(), dst_ip.c_str());

            bool ok = true;
            string reason;

            if (!validName(tunnel_name))
            {
                ok = false;
                reason = "invalid tunnel_name (allow [A-Za-z0-9_-])";
            }
            else if (!unknown_fields.empty())
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
                    if (!dst_ip.empty())
                    {
                        IpAddress dip(dst_ip);
                        (void)dip;
                    }
                }
                catch (...)
                {
                    ok = false;
                    reason = "invalid src_ip/dst_ip";
                }
            }

            if (!ok)
            {
                SWSS_LOG_WARN("NVGRE_TUNNEL %s rejected: %s", tunnel_name.c_str(), reason.c_str());
            }
            else
            {
                /* If src_ip or dst_ip changed, re-program any already-created maps
                 * so their gretap `local`/`remote` track the new VTEP addresses.
                 * programMap() does delete-before-add, so this is idempotent. */
                bool changed = (m_tunnelSrcIp.find(tunnel_name) == m_tunnelSrcIp.end()) ||
                               (m_tunnelSrcIp[tunnel_name] != src_ip) ||
                               (m_tunnelDstIp.find(tunnel_name) == m_tunnelDstIp.end()) ||
                               (m_tunnelDstIp[tunnel_name] != dst_ip);

                m_tunnelSrcIp[tunnel_name] = src_ip;
                m_tunnelDstIp[tunnel_name] = dst_ip;

                if (changed)
                {
                    string prefix = tunnel_name + "|";
                    for (auto &entry : m_mapDev)
                    {
                        if (entry.first.compare(0, prefix.size(), prefix) == 0)
                            programMap(entry.first, entry.second.vsid, entry.second.vlanId, src_ip, dst_ip);
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

            /* HLD: deleting a tunnel does NOT remove its maps automatically — the
             * user must delete the dependent maps first. Defer until none remain. */
            if (tunnelHasMaps(tunnel_name))
            {
                SWSS_LOG_WARN("NVGRE_TUNNEL %s still has dependent maps, deferring deletion (remove maps first)",
                              tunnel_name.c_str());
                it++;
                continue;
            }

            m_tunnelSrcIp.erase(tunnel_name);
            m_tunnelDstIp.erase(tunnel_name);
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

            if (!validName(tunnel) || !validName(map_name))
            {
                ok = false;
                reason = "invalid tunnel/map name (allow [A-Za-z0-9_-])";
            }
            else if (!unknown_fields.empty())
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
                /* YANG range is 0..16777214; VSID 0 is allowed (RFC 7637 reserves
                 * 0-0xFFF but the YANG does not exclude 0, and the legacy orch
                 * accepts it too). */
                if (!parseVsid(vsid, vsidVal) || vsidVal > NVGRE_VSID_MAX_VALUE)
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

            if (!bridgeExists())
            {
                /* Bridge not up yet — defer like the tunnel-not-configured case. */
                SWSS_LOG_WARN("NVGRE_TUNNEL_MAP %s: bridge %s not up, deferring",
                              key.c_str(), NVGRE_DEFAULT_BRIDGE);
                it++;
                continue;
            }

            auto dstIt = m_tunnelDstIp.find(tunnel);
            string dstIp = (dstIt != m_tunnelDstIp.end()) ? dstIt->second : "";
            bool programmed = programMap(key, vsid, vlan_id, srcIt->second, dstIp);

            vector<FieldValueTuple> fvs;
            fvs.emplace_back("status", programmed ? "active" : "inactive");
            if (programmed)
                fvs.emplace_back(NVGRE_STATE_FIELD_DEV, m_mapDev[key].dev);
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
                                const string &vlanId, const string &srcIp,
                                const string &dstIp)
{
    SWSS_LOG_ENTER();

    string tunnel = key.substr(0, key.find('|'));
    string mapName = key.substr(key.find('|') + 1);
    string dev = mapDeviceName(tunnel, mapName);

    /* IPv4 -> gretap, IPv6 -> ip6gretap (Fix 4).
     * `remote` is the peer VTEP IP used for encap. Default to `any` (decap-only
     * P2MP termination) when dst_ip is not configured; a specific dst_ip makes
     * the return encap path work for a fixed peer. */
    string linkType, remote, ttlOpt;
    try
    {
        IpAddress ip(srcIp);
        if (ip.isV4())
        {
            linkType = "gretap";
            remote = dstIp.empty() ? "0.0.0.0" : dstIp;
            ttlOpt = "ttl";
        }
        else
        {
            linkType = "ip6gretap";
            remote = dstIp.empty() ? "::" : dstIp;
            ttlOpt = "hoplimit";
        }
    }
    catch (...)
    {
        SWSS_LOG_ERROR("NVGRE_TUNNEL_MAP %s: invalid src_ip '%s'", key.c_str(), srcIp.c_str());
        return false;
    }

    /* RFC 7637 (Fix 1): GRE Key = VSID (high 24 bits) | FlowID (low 8 bits, =0). */
    uint64_t vsidVal = stoull(vsid);
    uint32_t greKey = static_cast<uint32_t>(vsidVal << 8);

    /* Delete-before-add for idempotency (re-SET / src_ip change). */
    if (interfaceExists(dev))
        rollbackDevice(dev);

    /* NVGRE decap: gretap (GRE/TEB) with the VSID in the high 24 bits of the key.
     * remote any = decap-any (P2MP termination, matches SAI tunnel termination). */
    ostringstream add_cmd;
    add_cmd << IP_CMD << " link add " << dev << " type " << linkType
            << " local " << srcIp << " remote " << remote
            << " key " << greKey << " " << ttlOpt << " " << NVGRE_DEFAULT_TTL;
    SWSS_LOG_NOTICE("Executing: %s", add_cmd.str().c_str());

    string res;
    int ret = swss::exec(add_cmd.str(), res);
    if (ret != 0)
    {
        SWSS_LOG_ERROR("ip link add %s failed on %s (ret=%d): %s",
                       linkType.c_str(), dev.c_str(), ret, res.c_str());
        return false;
    }

    ostringstream up_cmd;
    up_cmd << IP_CMD << " link set " << dev << " up";
    SWSS_LOG_NOTICE("Executing: %s", up_cmd.str().c_str());
    ret = swss::exec(up_cmd.str(), res);
    if (ret != 0)
    {
        SWSS_LOG_ERROR("ip link set up failed on %s (ret=%d): %s", dev.c_str(), ret, res.c_str());
        rollbackDevice(dev);
        return false;
    }

    ostringstream master_cmd;
    master_cmd << IP_CMD << " link set " << dev << " master " << NVGRE_DEFAULT_BRIDGE;
    SWSS_LOG_NOTICE("Executing: %s", master_cmd.str().c_str());
    ret = swss::exec(master_cmd.str(), res);
    if (ret != 0)
    {
        SWSS_LOG_ERROR("ip link set master failed on %s (ret=%d): %s", dev.c_str(), ret, res.c_str());
        rollbackDevice(dev);
        return false;
    }

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
        rollbackDevice(dev);
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
