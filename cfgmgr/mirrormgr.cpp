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
 * resolveInterface — return the kernel interface name for a SONiC port name.
 * In switchdev mode the docker-sonic-vs container renames the front-panel veths
 * (eth1 -> Ethernet0, eth2 -> Ethernet4, ...) at startup, so the kernel
 * interface names already match the SONiC names used in CONFIG_DB. No
 * translation is needed.
 */
static string resolveInterface(const string &sonicName)
{
    return sonicName;
}

/*
 * normalizeDirection — map the MIRROR_SESSION direction field to the canonical
 * RX/TX/BOTH values. Accepts upstream RX/TX/BOTH and the legacy ingress/egress/
 * both spellings, defaulting to RX (ingress) when absent or unknown.
 */
static string normalizeDirection(const string &dir)
{
    if (dir == "RX" || dir == "rx" || dir == "INGRESS" || dir == "ingress")
        return MIRROR_RX_DIRECTION;
    if (dir == "TX" || dir == "tx" || dir == "EGRESS" || dir == "egress")
        return MIRROR_TX_DIRECTION;
    if (dir == "BOTH" || dir == "both")
        return MIRROR_BOTH_DIRECTION;
    if (dir.empty())
        return MIRROR_RX_DIRECTION;

    SWSS_LOG_WARN("Unknown mirror direction '%s', defaulting to RX", dir.c_str());
    return MIRROR_RX_DIRECTION;
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

            direction = normalizeDirection(direction);

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

            /* Assign a stable tc filter priority for this session so its
             * filters can be idempotently re-applied and cleanly torn down. */
            auto prioIt = m_sessionPrio.find(session_name);
            uint32_t prio;
            if (prioIt != m_sessionPrio.end())
            {
                prio = prioIt->second;
            }
            else
            {
                prio = m_nextPrio++;
                m_sessionPrio[session_name] = prio;
            }

            bool ok = true;
            for (auto &sp : tokenize(src_port, ','))
            {
                string iface = resolveInterface(sp);
                bool ret;
                if (type == MIRROR_SESSION_TYPE_ERSPAN)
                    ret = addErspanSession(iface, src_ip, dst_ip, dst_port, direction, prio);
                else
                    ret = addSpanSession(iface, dst_port, direction, prio);

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
                uint32_t prio = 0;
                auto prioIt = m_sessionPrio.find(session_name);
                if (prioIt != m_sessionPrio.end())
                    prio = prioIt->second;

                for (auto &sp : tokenize(portIt->second, ','))
                    removeMirrorSession(resolveInterface(sp), prio);
                m_sessionSrcPort.erase(portIt);
            }

            m_sessionPrio.erase(session_name);
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
bool MirrorMgr::addSpanSession(const string &srcPort, const string &dstPort,
                               const string &direction, uint32_t prio)
{
    SWSS_LOG_ENTER();

    if (dstPort.empty())
    {
        SWSS_LOG_WARN("SPAN session: no dst_port, skipping");
        return false;
    }

    string dstIface = resolveInterface(dstPort);

    ensureClsact(srcPort);

    ostringstream actionTail;
    actionTail << "action mirred egress mirror dev " << dstIface;

    bool ok = true;
    if (direction == MIRROR_RX_DIRECTION || direction == MIRROR_BOTH_DIRECTION)
        ok &= addTcMirrorFilter(srcPort, "ingress", prio, actionTail.str());
    if (direction == MIRROR_TX_DIRECTION || direction == MIRROR_BOTH_DIRECTION)
        ok &= addTcMirrorFilter(srcPort, "egress", prio, actionTail.str());

    return ok;
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
                                 const string &dstIp, const string &dstPort,
                                 const string &direction, uint32_t prio)
{
    SWSS_LOG_ENTER();

    if (srcIp.empty() || dstIp.empty())
    {
        SWSS_LOG_WARN("ERSPAN session: src_ip/dst_ip required, skipping");
        return false;
    }

    string dstIface = dstPort.empty() ? srcPort : resolveInterface(dstPort);

    ensureClsact(srcPort);

    ostringstream actionTail;
    actionTail << "action tunnel_key set src_ip " << srcIp
               << " dst_ip " << dstIp
               << " id 100"
               << " action mirred egress mirror tunnel_key dev " << dstIface;

    bool ok = true;
    if (direction == MIRROR_RX_DIRECTION || direction == MIRROR_BOTH_DIRECTION)
        ok &= addTcMirrorFilter(srcPort, "ingress", prio, actionTail.str());
    if (direction == MIRROR_TX_DIRECTION || direction == MIRROR_BOTH_DIRECTION)
        ok &= addTcMirrorFilter(srcPort, "egress", prio, actionTail.str());

    return ok;
}

/*
 * ensureClsact — attach the clsact qdisc (ingress + egress hooks). Idempotent;
 * tc returns EEXIST if already present, which we ignore.
 */
void MirrorMgr::ensureClsact(const string &srcPort)
{
    ostringstream qdisc_cmd;
    qdisc_cmd << TC_CMD << " qdisc add dev " << srcPort << " clsact";
    string ignored;
    swss::exec(qdisc_cmd.str(), ignored);
}

/*
 * addTcMirrorFilter — (re)program one matchall mirror filter on a given hook.
 * Deletes any existing filter at the same prio first, so re-applying a session
 * is idempotent.
 */
bool MirrorMgr::addTcMirrorFilter(const string &srcPort, const string &hook,
                                  uint32_t prio, const string &actionTail)
{
    ostringstream del_cmd;
    del_cmd << TC_CMD << " filter del dev " << srcPort << " " << hook << " prio " << prio;
    string ignored;
    swss::exec(del_cmd.str(), ignored);

    ostringstream cmd;
    cmd << TC_CMD << " filter add dev " << srcPort << " " << hook << " prio " << prio
        << " matchall " << actionTail;

    SWSS_LOG_NOTICE("Executing: %s", cmd.str().c_str());

    string res;
    int ret = swss::exec(cmd.str(), res);
    if (ret != 0)
    {
        SWSS_LOG_ERROR("tc mirror add failed on %s (%s) (ret=%d): %s",
                       srcPort.c_str(), hook.c_str(), ret, res.c_str());
        return false;
    }

    return true;
}

/*
 * removeTcMirrorFilter — remove a matchall mirror filter from a given hook.
 */
bool MirrorMgr::removeTcMirrorFilter(const string &srcPort, const string &hook,
                                     uint32_t prio)
{
    ostringstream cmd;
    cmd << TC_CMD << " filter del dev " << srcPort << " " << hook << " prio " << prio;

    SWSS_LOG_NOTICE("Executing: %s", cmd.str().c_str());

    string res;
    int ret = swss::exec(cmd.str(), res);

    /* Not found is fine — the filter may already be gone. */
    if (ret != 0)
        SWSS_LOG_WARN("tc filter del on %s (%s) (ret=%d): %s",
                      srcPort.c_str(), hook.c_str(), ret, res.c_str());

    return true;
}

bool MirrorMgr::removeMirrorSession(const string &srcPort, uint32_t prio)
{
    SWSS_LOG_ENTER();

    /* Delete from both hooks regardless of direction; removing a filter that
     * was never programmed is harmless. */
    bool ok = true;
    ok &= removeTcMirrorFilter(srcPort, "ingress", prio);
    ok &= removeTcMirrorFilter(srcPort, "egress", prio);

    return ok;
}
