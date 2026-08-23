#include <string.h>
#include <sstream>
#include "logger.h"
#include "tokenize.h"
#include "exec.h"
#include "shellcmd.h"
#include "schema.h"
#include "mirrormgr.h"
#include "kernutil.h"

using namespace std;
using namespace swss;

/* TC command path */
#define TC_CMD "/sbin/tc"

MirrorMgr::MirrorMgr(DBConnector *cfgDb, DBConnector *stateDb,
                     const vector<string> &tableNames) :
    Orch(cfgDb, stateDb, tableNames, {}),
    m_stateMirrorSessionTable(stateDb, STATE_MIRROR_SESSION_TABLE_NAME),
    m_cfgPolicerTable(cfgDb, CFG_POLICER_TABLE_NAME),
    m_statePolicerTable(stateDb, STATE_POLICER_TABLE_NAME)
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

/*
 * getPolicerPoliceAction — resolve a referenced policer into a tc
 * "action police ..." string. The policer must be registered active by
 * policermgrd (STATE_DB POLICER_TABLE) and have a valid CONFIG_DB POLICER
 * entry. Returns false otherwise.
 */
bool MirrorMgr::getPolicerPoliceAction(const string &policerName, string &policeAction)
{
    vector<FieldValueTuple> stateFvs;
    string status;
    if (m_statePolicerTable.get(policerName, stateFvs))
    {
        for (auto &fv : stateFvs)
        {
            if (fvField(fv) == "status")
                status = fvValue(fv);
        }
    }

    if (status != "active")
    {
        SWSS_LOG_WARN("POLICER %s not active in STATE_DB (status='%s')",
                      policerName.c_str(), status.c_str());
        return false;
    }

    vector<FieldValueTuple> cfgFvs;
    if (!m_cfgPolicerTable.get(policerName, cfgFvs))
    {
        SWSS_LOG_WARN("POLICER %s not found in CONFIG_DB", policerName.c_str());
        return false;
    }

    map<string, string> fields;
    for (auto &fv : cfgFvs)
        fields[fvField(fv)] = fvValue(fv);

    policeAction = kernutil::policerToTcPolice(fields);
    return !policeAction.empty();
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
            string gre_type, dscp, ttl, policer, sample_rate;

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
                else if (field == MIRROR_SESSION_FIELD_GRE_TYPE) gre_type = value;
                else if (field == MIRROR_SESSION_FIELD_DSCP)     dscp = value;
                else if (field == MIRROR_SESSION_FIELD_TTL)      ttl = value;
                else if (field == MIRROR_SESSION_FIELD_POLICER)  policer = value;
                else if (field == MIRROR_SESSION_FIELD_SAMPLE_RATE) sample_rate = value;
            }

            direction = kernutil::normalizeDirection(direction);

            SWSS_LOG_NOTICE("MIRROR_SESSION SET: %s type=%s src=%s dst=%s src_ip=%s dst_ip=%s gre=%s dscp=%s ttl=%s policer=%s sample_rate=%s dir=%s",
                            session_name.c_str(), type.c_str(), src_port.c_str(),
                            dst_port.c_str(), src_ip.c_str(), dst_ip.c_str(),
                            gre_type.c_str(), dscp.c_str(), ttl.c_str(),
                            policer.c_str(), sample_rate.c_str(), direction.c_str());

            /* Sampled mirroring (sample_rate) is not supported in tc/switchdev. */
            if (!sample_rate.empty() && sample_rate != "0")
            {
                SWSS_LOG_ERROR("MIRROR_SESSION %s: sampled mirroring (sample_rate=%s) not supported, marking inactive",
                               session_name.c_str(), sample_rate.c_str());
                vector<FieldValueTuple> fvs;
                fvs.emplace_back("status", "inactive");
                m_stateMirrorSessionTable.set(session_name, fvs);
                it = consumer.m_toSync.erase(it);
                continue;
            }

            if (src_port.empty())
            {
                SWSS_LOG_WARN("MIRROR_SESSION %s: no src_port specified, skipping", session_name.c_str());
                vector<FieldValueTuple> fvs;
                fvs.emplace_back("status", "inactive");
                m_stateMirrorSessionTable.set(session_name, fvs);
                it = consumer.m_toSync.erase(it);
                continue;
            }

            /* Resolve the referenced policer (if any) into a tc police action. */
            string police_action;
            if (!policer.empty())
            {
                if (!getPolicerPoliceAction(policer, police_action))
                {
                    SWSS_LOG_ERROR("MIRROR_SESSION %s: policer %s missing/inactive, marking inactive",
                                   session_name.c_str(), policer.c_str());
                    vector<FieldValueTuple> fvs;
                    fvs.emplace_back("status", "inactive");
                    m_stateMirrorSessionTable.set(session_name, fvs);
                    it = consumer.m_toSync.erase(it);
                    continue;
                }
            }

            /* Expand LAG source ports to member interfaces. */
            vector<string> ifaces;
            if (!kernutil::resolveSrcPorts(src_port, ifaces))
            {
                SWSS_LOG_ERROR("MIRROR_SESSION %s: invalid/empty LAG in src_port '%s', marking inactive",
                               session_name.c_str(), src_port.c_str());
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
            for (auto &iface : ifaces)
            {
                bool ret;
                if (type == MIRROR_SESSION_TYPE_ERSPAN)
                    ret = addErspanSession(iface, src_ip, dst_ip, dst_port,
                                           gre_type, dscp, ttl, direction,
                                           police_action, prio);
                else
                    ret = addSpanSession(iface, dst_port, direction, police_action, prio);

                if (!ret)
                {
                    SWSS_LOG_ERROR("Failed to program mirror on %s for session %s",
                                   iface.c_str(), session_name.c_str());
                    ok = false;
                }
            }

            if (ok)
                m_programmedSessions.insert(session_name);
            m_sessionSrcPort[session_name] = src_port;
            m_sessionIfaces[session_name] = ifaces;

            vector<FieldValueTuple> fvs;
            fvs.emplace_back("status", ok ? "active" : "inactive");
            m_stateMirrorSessionTable.set(session_name, fvs);

            it = consumer.m_toSync.erase(it);
        }
        else if (op == DEL_COMMAND)
        {
            SWSS_LOG_NOTICE("MIRROR_SESSION DEL: %s", session_name.c_str());

            auto ifacesIt = m_sessionIfaces.find(session_name);
            if (ifacesIt != m_sessionIfaces.end())
            {
                uint32_t prio = 0;
                auto prioIt = m_sessionPrio.find(session_name);
                if (prioIt != m_sessionPrio.end())
                    prio = prioIt->second;

                for (auto &iface : ifacesIt->second)
                    removeMirrorSession(iface, prio);
                m_sessionIfaces.erase(ifacesIt);
            }

            m_sessionSrcPort.erase(session_name);
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
                               const string &direction, const string &policeAction,
                               uint32_t prio)
{
    SWSS_LOG_ENTER();

    if (dstPort.empty())
    {
        SWSS_LOG_WARN("SPAN session: no dst_port, skipping");
        return false;
    }

    string dstIface = kernutil::resolveInterface(dstPort);

    ensureClsact(srcPort);

    ostringstream actionTail;
    if (!policeAction.empty())
        actionTail << policeAction << " ";
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
 * gre_type is used as the GRE key (tunnel_key "id"); dscp/ttl cannot be
 * expressed by the tc tunnel_key action and are logged-ignored. The monitor
 * interface is the explicit dst_port when set, otherwise the egress interface
 * toward dst_ip resolved via "ip route get".
 */
bool MirrorMgr::addErspanSession(const string &srcPort, const string &srcIp,
                                 const string &dstIp, const string &dstPort,
                                 const string &greType, const string &dscp,
                                 const string &ttl, const string &direction,
                                 const string &policeAction, uint32_t prio)
{
    SWSS_LOG_ENTER();

    if (srcIp.empty() || dstIp.empty())
    {
        SWSS_LOG_WARN("ERSPAN session: src_ip/dst_ip required, skipping");
        return false;
    }

    if (!dscp.empty())
        SWSS_LOG_WARN("ERSPAN session: dscp=%s not expressible via tc tunnel_key, ignoring", dscp.c_str());
    if (!ttl.empty())
        SWSS_LOG_WARN("ERSPAN session: ttl=%s not expressible via tc tunnel_key, ignoring", ttl.c_str());

    string greKey = greType.empty() ? MIRROR_SESSION_DEFAULT_GRE_TYPE : greType;

    string dstIface;
    if (!dstPort.empty())
    {
        dstIface = kernutil::resolveInterface(dstPort);
    }
    else
    {
        /* Resolve the egress interface toward the ERSPAN destination IP. */
        dstIface = kernutil::resolveNextHopInterface(dstIp);
        if (dstIface.empty())
        {
            SWSS_LOG_ERROR("ERSPAN session: failed to resolve egress interface for dst_ip %s", dstIp.c_str());
            return false;
        }
    }

    ensureClsact(srcPort);

    ostringstream actionTail;
    if (!policeAction.empty())
        actionTail << policeAction << " ";
    actionTail << "action tunnel_key set src_ip " << srcIp
               << " dst_ip " << dstIp
               << " id " << greKey
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
