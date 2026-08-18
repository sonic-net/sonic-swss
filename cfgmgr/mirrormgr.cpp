#include <string.h>
#include <sstream>
#include "logger.h"
#include "tokenize.h"
#include "exec.h"
#include "shellcmd.h"
#include "schema.h"
#include "mirrormgr.h"

using namespace std;
using namespace swss;

/* TC command path */
#define TC_CMD "/sbin/tc"

/*
 * resolveInterface — translate SONiC port name to kernel interface name.
 * In sonic-vs the kernel veth names (eth1, eth2) differ from SONiC front-panel
 * names (Ethernet0, Ethernet4). In production switchdev they are the same.
 */
static string resolveInterface(const string &sonicName)
{
    static const map<string, string> mapping = {
        {"Ethernet0", "eth1"},
        {"Ethernet4", "eth2"},
    };

    auto it = mapping.find(sonicName);
    if (it != mapping.end())
        return it->second;

    return sonicName;
}

MirrorMgr::MirrorMgr(DBConnector *cfgDb, DBConnector *stateDb,
                     const vector<string> &tableNames) :
    Orch(cfgDb, stateDb, tableNames, {}),
    m_stateMirrorSessionTable(stateDb, STATE_MIRROR_SESSION_TABLE_NAME)
{
    SWSS_LOG_ENTER();
    SWSS_LOG_NOTICE("MirrorMgr initialized, subscribed to %zu CONFIG_DB tables", tableNames.size());
}

void MirrorMgr::doTask(Consumer &consumer)
{
    string table_name = consumer.getTableName();

    SWSS_LOG_DEBUG("doTask: table=%s", table_name.c_str());

    if (table_name == CFG_MIRROR_SESSION_TABLE_NAME)
        doMirrorSessionTask(consumer);
    else
    {
        SWSS_LOG_ERROR("MirrorMgr doTask: unknown table '%s'", table_name.c_str());
        throw runtime_error("MirrorMgr doTask failure: unknown table " + table_name);
    }
}

void MirrorMgr::doMirrorSessionTask(Consumer &consumer)
{
    SWSS_LOG_ENTER();

    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
        auto &t = it->second;
        string session_name = kfvKey(t);
        string op = kfvOp(t);

        SWSS_LOG_INFO("MIRROR_SESSION: key=%s op=%s", session_name.c_str(), op.c_str());

        if (op == SET_COMMAND)
        {
            string type, src_ip, dst_ip, src_port, dst_port, direction;

            for (auto i : kfvFieldsValues(t))
            {
                string field = fvField(i);
                string value = fvValue(i);

                if (field == MIRROR_SESSION_FIELD_TYPE)          type = value;
                else if (field == MIRROR_SESSION_FIELD_SRC_IP)   src_ip = value;
                else if (field == MIRROR_SESSION_FIELD_DST_IP)   dst_ip = value;
                else if (field == MIRROR_SESSION_FIELD_SRC_PORT) src_port = value;
                else if (field == MIRROR_SESSION_FIELD_DST_PORT) dst_port = value;
                else if (field == MIRROR_SESSION_FIELD_DIRECTION) direction = value;
            }

            SWSS_LOG_NOTICE("MIRROR_SESSION SET: %s type=%s src=%s dst=%s src_ip=%s dst_ip=%s dir=%s",
                            session_name.c_str(), type.c_str(), src_port.c_str(),
                            dst_port.c_str(), src_ip.c_str(), dst_ip.c_str(), direction.c_str());

            if (src_port.empty())
            {
                SWSS_LOG_WARN("MIRROR_SESSION %s: no src_port specified, skipping", session_name.c_str());
                vector<FieldValueTuple> fvs;
                fvs.emplace_back("status", "inactive");
                m_stateMirrorSessionTable.set(session_name, fvs);
                it = consumer.m_toSync.erase(it);
                continue;
            }

            bool ok = true;
            for (auto &sp : tokenize(src_port, ','))
            {
                string iface = resolveInterface(sp);
                bool ret;
                if (type == MIRROR_SESSION_TYPE_ERSPAN)
                    ret = addErspanSession(iface, src_ip, dst_ip, dst_port);
                else
                    ret = addSpanSession(iface, dst_port);

                if (!ret)
                {
                    SWSS_LOG_ERROR("Failed to program mirror on %s for session %s",
                                   sp.c_str(), session_name.c_str());
                    ok = false;
                }
            }

            if (ok)
                m_programmedSessions.insert(session_name);
            m_sessionSrcPort[session_name] = src_port;

            vector<FieldValueTuple> fvs;
            fvs.emplace_back("status", ok ? "active" : "inactive");
            m_stateMirrorSessionTable.set(session_name, fvs);

            it = consumer.m_toSync.erase(it);
        }
        else if (op == DEL_COMMAND)
        {
            SWSS_LOG_NOTICE("MIRROR_SESSION DEL: %s", session_name.c_str());

            auto portIt = m_sessionSrcPort.find(session_name);
            if (portIt != m_sessionSrcPort.end())
            {
                for (auto &sp : tokenize(portIt->second, ','))
                    removeMirrorSession(resolveInterface(sp));
                m_sessionSrcPort.erase(portIt);
            }

            m_programmedSessions.erase(session_name);
            m_stateMirrorSessionTable.del(session_name);

            it = consumer.m_toSync.erase(it);
        }
        else
        {
            SWSS_LOG_ERROR("MIRROR_SESSION: unknown operation '%s'", op.c_str());
            it = consumer.m_toSync.erase(it);
        }
    }
}

/*
 * addSpanSession — local (SPAN) port mirroring via tc mirred.
 *   tc filter add dev <src> ingress matchall action mirred egress mirror dev <dst>
 */
bool MirrorMgr::addSpanSession(const string &srcPort, const string &dstPort)
{
    SWSS_LOG_ENTER();

    if (dstPort.empty())
    {
        SWSS_LOG_WARN("SPAN session: no dst_port, skipping");
        return false;
    }

    string dstIface = resolveInterface(dstPort);

    /* Ensure ingress qdisc exists (idempotent). */
    ostringstream qdisc_cmd;
    qdisc_cmd << TC_CMD << " qdisc add dev " << srcPort << " ingress";
    string ignored;
    swss::exec(qdisc_cmd.str(), ignored);

    ostringstream cmd;
    cmd << TC_CMD << " filter add dev " << srcPort << " ingress matchall"
        << " action mirred egress mirror dev " << dstIface;

    SWSS_LOG_NOTICE("Executing: %s", cmd.str().c_str());

    string res;
    int ret = swss::exec(cmd.str(), res);
    if (ret != 0)
    {
        SWSS_LOG_ERROR("tc mirror add failed on %s (ret=%d): %s", srcPort.c_str(), ret, res.c_str());
        return false;
    }

    return true;
}

/*
 * addErspanSession — remote (ERSPAN) mirroring via tc tunnel_key + mirred.
 *   tc filter add dev <src> ingress matchall
 *       action tunnel_key set src_ip <src_ip> dst_ip <dst_ip> id <gre_key>
 *       action mirred egress mirror tunnel_key dev <monitor>
 *
 * Phase 1: the GRE key defaults to 100; dst_port (if set) is used as the
 * monitor/egress port toward the ERSPAN destination.
 */
bool MirrorMgr::addErspanSession(const string &srcPort, const string &srcIp,
                                 const string &dstIp, const string &dstPort)
{
    SWSS_LOG_ENTER();

    if (srcIp.empty() || dstIp.empty())
    {
        SWSS_LOG_WARN("ERSPAN session: src_ip/dst_ip required, skipping");
        return false;
    }

    string dstIface = dstPort.empty() ? srcPort : resolveInterface(dstPort);

    ostringstream qdisc_cmd;
    qdisc_cmd << TC_CMD << " qdisc add dev " << srcPort << " ingress";
    string ignored;
    swss::exec(qdisc_cmd.str(), ignored);

    ostringstream cmd;
    cmd << TC_CMD << " filter add dev " << srcPort << " ingress matchall"
        << " action tunnel_key set src_ip " << srcIp
        << " dst_ip " << dstIp
        << " id 100"
        << " action mirred egress mirror tunnel_key dev " << dstIface;

    SWSS_LOG_NOTICE("Executing: %s", cmd.str().c_str());

    string res;
    int ret = swss::exec(cmd.str(), res);
    if (ret != 0)
    {
        SWSS_LOG_ERROR("tc erspan mirror add failed on %s (ret=%d): %s",
                       srcPort.c_str(), ret, res.c_str());
        return false;
    }

    return true;
}

/*
 * removeMirrorSession — remove the matchall mirror filter from a source port.
 */
bool MirrorMgr::removeMirrorSession(const string &srcPort)
{
    SWSS_LOG_ENTER();

    ostringstream cmd;
    /* matchall filters get the default pref 49152; delete by that pref
     * (tc rejects `del ... matchall` as a malformed bulk-flush). */
    cmd << TC_CMD << " filter del dev " << srcPort << " ingress pref 49152";

    SWSS_LOG_NOTICE("Executing: %s", cmd.str().c_str());

    string res;
    int ret = swss::exec(cmd.str(), res);

    /* Not found is fine — the filter may already be gone. */
    if (ret != 0)
        SWSS_LOG_WARN("tc filter del on %s (ret=%d): %s", srcPort.c_str(), ret, res.c_str());

    return true;
}
