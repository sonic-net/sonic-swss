#include <sstream>
#include <cstdio>
#include "logger.h"
#include "exec.h"
#include "shellcmd.h"
#include "kernutil.h"
#include "qosmgr.h"

using namespace std;
using namespace swss;

#define TC_CMD "/sbin/tc"

static bool execTc(const string &cmd, string &result, const char *operation)
{
    /* swss::exec captures command output, but tc writes most diagnostics to
     * stderr. Merge stderr so a failed realization is actionable in syslog. */
    int rc = swss::exec(cmd + " 2>&1", result);
    if (rc != 0)
    {
        SWSS_LOG_ERROR("tc command failed (%s), rc=%d: %s; output: %s",
                       operation, rc, cmd.c_str(), result.c_str());
        return false;
    }
    return true;
}

static void execTcQuiet(const string &cmd)
{
    /* Best-effort tc: run and discard output, ignoring failure (used for
     * delete-before-add cleanup where "nothing to delete" is expected). */
    string ignored;
    swss::exec(cmd + " 2>&1", ignored);
}

/* ------------------------------------------------------------------------ *
 * Small parse/validation helpers
 * ------------------------------------------------------------------------ */

static bool parseInt(const string &s, long long &out)
{
    if (s.empty())
        return false;
    try
    {
        out = stoll(s);
    }
    catch (...)
    {
        return false;
    }
    return true;
}

static bool intInRange(const string &s, long long lo, long long hi)
{
    long long v;
    if (!parseInt(s, v))
        return false;
    return v >= lo && v <= hi;
}

/* Validate a numeric-pair map body: every field (key) and value must be an
 * integer within [0,keyMax] and [0,valMax] respectively. */
static bool validateNumPairs(const vector<FieldValueTuple> &fvs,
                             uint32_t keyMax, uint32_t valMax, string &reason)
{
    if (fvs.empty())
    {
        reason = "empty map";
        return false;
    }
    for (const auto &fv : fvs)
    {
        if (!intInRange(fvField(fv), 0, keyMax))
        {
            reason = "map key out of range: " + fvField(fv);
            return false;
        }
        if (!intInRange(fvValue(fv), 0, valMax))
        {
            reason = "map value out of range: " + fvValue(fv);
            return false;
        }
    }
    return true;
}

/* ------------------------------------------------------------------------ *
 * QosMgr constructor
 * ------------------------------------------------------------------------ */

QosMgr::QosMgr(DBConnector *cfgDb, DBConnector *stateDb,
               const vector<string> &tableNames) :
    Orch(cfgDb, stateDb, tableNames, {}),
    m_stateDscpToTcTable(stateDb, STATE_DSCP_TO_TC_MAP_TABLE_NAME),
    m_stateDot1pToTcTable(stateDb, STATE_DOT1P_TO_TC_MAP_TABLE_NAME),
    m_stateTcToQueueTable(stateDb, STATE_TC_TO_QUEUE_MAP_TABLE_NAME),
    m_stateTcToDscpTable(stateDb, STATE_TC_TO_DSCP_MAP_TABLE_NAME),
    m_stateSchedulerTable(stateDb, STATE_SCHEDULER_TABLE_NAME),
    m_stateWredTable(stateDb, STATE_WRED_PROFILE_TABLE_NAME),
    m_stateQueueTable(stateDb, STATE_QUEUE_TABLE_NAME),
    m_statePortQosMapTable(stateDb, STATE_PORT_QOS_MAP_TABLE_NAME),
    m_cfgPortTable(cfgDb, "PORT")
{
    SWSS_LOG_ENTER();
    SWSS_LOG_NOTICE("QosMgr initialized, subscribed to %zu CONFIG_DB tables", tableNames.size());
}

/* ------------------------------------------------------------------------ *
 * doTask dispatch
 * ------------------------------------------------------------------------ */

void QosMgr::doTask(Consumer &consumer)
{
    string table = consumer.getTableName();

    if (table == CFG_DSCP_TO_TC_MAP_TABLE_NAME)
        doDscpToTcTask(consumer);
    else if (table == CFG_DOT1P_TO_TC_MAP_TABLE_NAME)
        doDot1pToTcTask(consumer);
    else if (table == CFG_TC_TO_QUEUE_MAP_TABLE_NAME)
        doTcToQueueTask(consumer);
    else if (table == CFG_TC_TO_DSCP_MAP_TABLE_NAME)
        doTcToDscpTask(consumer);
    else if (table == CFG_SCHEDULER_TABLE_NAME)
        doSchedulerTask(consumer);
    else if (table == CFG_WRED_PROFILE_TABLE_NAME)
        doWredTask(consumer);
    else if (table == CFG_PORT_QOS_MAP_TABLE_NAME)
        doPortQosMapTask(consumer);
    else if (table == CFG_QUEUE_TABLE_NAME)
        doQueueTask(consumer);
    else
    {
        SWSS_LOG_ERROR("QosMgr doTask: unknown table '%s'", table.c_str());
        throw runtime_error("QosMgr doTask failure: unknown table " + table);
    }
}

/* ------------------------------------------------------------------------ *
 * Generic map-definition writeback
 * ------------------------------------------------------------------------ */

void QosMgr::writeMapStatus(Table &stateTable, const string &name, const string &status)
{
    vector<FieldValueTuple> fvs;
    fvs.emplace_back("status", status);
    stateTable.set(name, fvs);
}

/* ------------------------------------------------------------------------ *
 * Numeric map handlers (DSCP_TO_TC, DOT1P_TO_TC, TC_TO_QUEUE, TC_TO_DSCP)
 *
 * Each is keyed by a map name; its fields are "key -> value" pairs where both
 * are small integers. On SET we validate, store in the matching in-memory map,
 * and write STATE_DB status. On DEL we erase and clear STATE_DB.
 * ------------------------------------------------------------------------ */

void QosMgr::doDscpToTcTask(Consumer &consumer)
{
    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
        auto &t = it->second;
        string name = kfvKey(t);
        string op = kfvOp(t);

        if (op == SET_COMMAND)
        {
            string reason;
            bool ok = validateNumPairs(kfvFieldsValues(t), QOS_DSCP_MAX, QOS_TC_MAX, reason);
            if (!ok)
                SWSS_LOG_WARN("DSCP_TO_TC_MAP %s rejected: %s", name.c_str(), reason.c_str());

            if (ok)
            {
                m_dscpToTcMap[name] = map<string, string>();
                for (const auto &fv : kfvFieldsValues(t))
                    m_dscpToTcMap[name][fvField(fv)] = fvValue(fv);

                reapplyMapBindings(QOS_FIELD_DSCP_TO_TC, name);
            }
            writeMapStatus(m_stateDscpToTcTable, name, ok ? "active" : "inactive");
            it = consumer.m_toSync.erase(it);
        }
        else if (op == DEL_COMMAND)
        {
            m_dscpToTcMap.erase(name);
            m_stateDscpToTcTable.del(name);
            it = consumer.m_toSync.erase(it);
        }
        else
        {
            it = consumer.m_toSync.erase(it);
        }
    }
}

void QosMgr::doDot1pToTcTask(Consumer &consumer)
{
    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
        auto &t = it->second;
        string name = kfvKey(t);
        string op = kfvOp(t);

        if (op == SET_COMMAND)
        {
            string reason;
            bool ok = validateNumPairs(kfvFieldsValues(t), QOS_PRIO_MAX, QOS_TC_MAX, reason);
            if (!ok)
                SWSS_LOG_WARN("DOT1P_TO_TC_MAP %s rejected: %s", name.c_str(), reason.c_str());

            if (ok)
            {
                m_dot1pToTcMap[name] = map<string, string>();
                for (const auto &fv : kfvFieldsValues(t))
                    m_dot1pToTcMap[name][fvField(fv)] = fvValue(fv);

                reapplyMapBindings(QOS_FIELD_DOT1P_TO_TC, name);
            }
            writeMapStatus(m_stateDot1pToTcTable, name, ok ? "active" : "inactive");
            it = consumer.m_toSync.erase(it);
        }
        else if (op == DEL_COMMAND)
        {
            m_dot1pToTcMap.erase(name);
            m_stateDot1pToTcTable.del(name);
            it = consumer.m_toSync.erase(it);
        }
        else
        {
            it = consumer.m_toSync.erase(it);
        }
    }
}

void QosMgr::doTcToQueueTask(Consumer &consumer)
{
    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
        auto &t = it->second;
        string name = kfvKey(t);
        string op = kfvOp(t);

        if (op == SET_COMMAND)
        {
            string reason;
            bool ok = validateNumPairs(kfvFieldsValues(t), QOS_TC_MAX, QOS_QUEUE_MAX, reason);
            if (!ok)
                SWSS_LOG_WARN("TC_TO_QUEUE_MAP %s rejected: %s", name.c_str(), reason.c_str());

            if (ok)
            {
                m_tcToQueueMap[name] = map<string, string>();
                for (const auto &fv : kfvFieldsValues(t))
                    m_tcToQueueMap[name][fvField(fv)] = fvValue(fv);

                reapplyMapBindings(QOS_FIELD_TC_TO_QUEUE, name);
            }
            writeMapStatus(m_stateTcToQueueTable, name, ok ? "active" : "inactive");
            it = consumer.m_toSync.erase(it);
        }
        else if (op == DEL_COMMAND)
        {
            m_tcToQueueMap.erase(name);
            m_stateTcToQueueTable.del(name);
            it = consumer.m_toSync.erase(it);
        }
        else
        {
            it = consumer.m_toSync.erase(it);
        }
    }
}

void QosMgr::doTcToDscpTask(Consumer &consumer)
{
    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
        auto &t = it->second;
        string name = kfvKey(t);
        string op = kfvOp(t);

        if (op == SET_COMMAND)
        {
            string reason;
            bool ok = validateNumPairs(kfvFieldsValues(t), QOS_TC_MAX, QOS_DSCP_MAX, reason);
            if (!ok)
                SWSS_LOG_WARN("TC_TO_DSCP_MAP %s rejected: %s", name.c_str(), reason.c_str());

            if (ok)
            {
                m_tcToDscpMap[name] = map<string, string>();
                for (const auto &fv : kfvFieldsValues(t))
                    m_tcToDscpMap[name][fvField(fv)] = fvValue(fv);

                reapplyMapBindings(QOS_FIELD_TC_TO_DSCP, name);
            }
            writeMapStatus(m_stateTcToDscpTable, name, ok ? "active" : "inactive");
            it = consumer.m_toSync.erase(it);
        }
        else if (op == DEL_COMMAND)
        {
            m_tcToDscpMap.erase(name);
            m_stateTcToDscpTable.del(name);
            it = consumer.m_toSync.erase(it);
        }
        else
        {
            it = consumer.m_toSync.erase(it);
        }
    }
}

/* ------------------------------------------------------------------------ *
 * SCHEDULER
 * ------------------------------------------------------------------------ */

void QosMgr::doSchedulerTask(Consumer &consumer)
{
    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
        auto &t = it->second;
        string name = kfvKey(t);
        string op = kfvOp(t);

        if (op == SET_COMMAND)
        {
            bool ok = true;
            string reason;
            map<string, string> cfg;

            for (const auto &fv : kfvFieldsValues(t))
            {
                string field = fvField(fv);
                string value = fvValue(fv);
                if (field == SCHED_FIELD_TYPE)
                {
                    if (value != SCHED_TYPE_DWRR && value != SCHED_TYPE_WRR &&
                        value != SCHED_TYPE_STRICT)
                    {
                        ok = false;
                        reason = "invalid scheduler type: " + value;
                    }
                }
                else if (field == SCHED_FIELD_WEIGHT || field == SCHED_FIELD_CIR ||
                         field == SCHED_FIELD_CBS || field == SCHED_FIELD_PIR ||
                         field == SCHED_FIELD_PBS)
                {
                    long long v;
                    if (!parseInt(value, v) || v < 0)
                    {
                        ok = false;
                        reason = "invalid numeric field " + field + ": " + value;
                    }
                }
                else if (field == SCHED_FIELD_METER_TYPE)
                {
                    /* accepted for compatibility; no kernel effect */
                }
                else
                {
                    ok = false;
                    reason = "unknown scheduler field: " + field;
                }
                cfg[field] = value;
            }

            if (cfg.find(SCHED_FIELD_TYPE) == cfg.end())
            {
                ok = false;
                reason = "missing mandatory type";
            }

            if (!ok)
                SWSS_LOG_WARN("SCHEDULER %s rejected: %s", name.c_str(), reason.c_str());
            else
            {
                m_schedulerMap[name] = cfg;

                /* SCHEDULER -> PORT dependency: re-apply the port-level shaper
                 * for every port whose PORT_QOS_MAP references this scheduler,
                 * so a runtime rate update is reflected without re-binding. */
                for (const auto &entry : m_portQosMap)
                {
                    const string &port = entry.first;
                    const map<string, string> &maps = entry.second;
                    if (maps.count(QOS_FIELD_SCHEDULER) &&
                        maps.at(QOS_FIELD_SCHEDULER) == name)
                    {
                        applyMapsToPort(port, maps, reason);
                    }
                }

                /* SCHEDULER -> QUEUE dependency: re-apply the queue-level
                 * scheduler for every queue referencing this scheduler. */
                for (const auto &entry : m_queueMap)
                {
                    const string &key = entry.first;
                    const map<string, string> &cfg = entry.second;
                    if (!cfg.count(QOS_FIELD_SCHEDULER) ||
                        cfg.at(QOS_FIELD_SCHEDULER) != name)
                        continue;

                    size_t sep = key.find('|');
                    if (sep == string::npos)
                        continue;
                    string port = key.substr(0, sep);
                    buildQueueTree(port, reason);
                }
            }

            writeMapStatus(m_stateSchedulerTable, name, ok ? "active" : "inactive");
            it = consumer.m_toSync.erase(it);
        }
        else if (op == DEL_COMMAND)
        {
            m_schedulerMap.erase(name);

            /* SCHEDULER -> PORT: remove the port-level shaper on referencing
             * ports so a deleted scheduler doesn't leave a stale shaper. */
            for (const auto &entry : m_portQosMap)
            {
                const string &port = entry.first;
                const map<string, string> &maps = entry.second;
                if (maps.count(QOS_FIELD_SCHEDULER) &&
                    maps.at(QOS_FIELD_SCHEDULER) == name)
                {
                    string iface = kernutil::resolveInterface(port);
                    string res;
                    string cmd = string(TC_CMD) + " qdisc del dev " + iface + " root";
                    execTc(cmd, res, "delete scheduler qdisc");
                }
            }

            m_stateSchedulerTable.del(name);
            it = consumer.m_toSync.erase(it);
        }
        else
        {
            it = consumer.m_toSync.erase(it);
        }
    }
}

/* ------------------------------------------------------------------------ *
 * WRED_PROFILE
 * ------------------------------------------------------------------------ */

void QosMgr::doWredTask(Consumer &consumer)
{
    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
        auto &t = it->second;
        string name = kfvKey(t);
        string op = kfvOp(t);

        if (op == SET_COMMAND)
        {
            bool ok = true;
            string reason;
            map<string, string> cfg;
            long long greenMin = -1, greenMax = -1, redMin = -1, redMax = -1;
            bool haveGreen = false, haveRed = false;

            for (const auto &fv : kfvFieldsValues(t))
            {
                string field = fvField(fv);
                string value = fvValue(fv);
                cfg[field] = value;

                if (field == WRED_FIELD_GREEN_MIN_THRESHOLD)
                {
                    haveGreen = true;
                    if (!parseInt(value, greenMin) || greenMin < 0)
                    { ok = false; reason = "invalid green_min_threshold"; }
                }
                else if (field == WRED_FIELD_GREEN_MAX_THRESHOLD)
                {
                    haveGreen = true;
                    if (!parseInt(value, greenMax) || greenMax < 0)
                    { ok = false; reason = "invalid green_max_threshold"; }
                }
                else if (field == WRED_FIELD_RED_MIN_THRESHOLD)
                {
                    haveRed = true;
                    if (!parseInt(value, redMin) || redMin < 0)
                    { ok = false; reason = "invalid red_min_threshold"; }
                }
                else if (field == WRED_FIELD_RED_MAX_THRESHOLD)
                {
                    haveRed = true;
                    if (!parseInt(value, redMax) || redMax < 0)
                    { ok = false; reason = "invalid red_max_threshold"; }
                }
                else if (field == WRED_FIELD_GREEN_DROP_PROBABILITY ||
                         field == WRED_FIELD_RED_DROP_PROBABILITY)
                {
                    if (!intInRange(value, 0, 100))
                    { ok = false; reason = "invalid " + field + " (0..100)"; }
                }
                else if (field == WRED_FIELD_YELLOW_MIN_THRESHOLD ||
                         field == WRED_FIELD_YELLOW_MAX_THRESHOLD ||
                         field == WRED_FIELD_YELLOW_DROP_PROBABILITY ||
                         field == WRED_FIELD_YELLOW_ENABLE)
                {
                    /* Color-aware (yellow) WRED has no Linux 'red' equivalent;
                     * accepted for compatibility but not programmed. */
                    SWSS_LOG_INFO("WRED_PROFILE %s: field %s accepted but ignored (no kernel equivalent)",
                                  name.c_str(), field.c_str());
                }
                else if (field == WRED_FIELD_GREEN_ENABLE ||
                         field == WRED_FIELD_RED_ENABLE ||
                         field == WRED_FIELD_ECN)
                {
                    /* Enable flags and ecn mode are accepted; the enabled
                     * color's thresholds drive the tc red command at apply. */
                }
                else
                {
                    ok = false;
                    reason = "unknown wred_profile field: " + field;
                }
            }

            if (!haveGreen && !haveRed)
            {
                ok = false;
                reason = "missing thresholds (green_* or red_* min/max)";
            }
            else if (ok && haveGreen && greenMin > greenMax)
            {
                ok = false;
                reason = "green_min_threshold > green_max_threshold";
            }
            else if (ok && haveRed && redMin > redMax)
            {
                ok = false;
                reason = "red_min_threshold > red_max_threshold";
            }

            if (!ok)
                SWSS_LOG_WARN("WRED_PROFILE %s rejected: %s", name.c_str(), reason.c_str());
            else
                m_wredMap[name] = cfg;

            writeMapStatus(m_stateWredTable, name, ok ? "active" : "inactive");
            it = consumer.m_toSync.erase(it);
        }
        else if (op == DEL_COMMAND)
        {
            m_wredMap.erase(name);
            m_stateWredTable.del(name);
            it = consumer.m_toSync.erase(it);
        }
        else
        {
            it = consumer.m_toSync.erase(it);
        }
    }
}

/* ------------------------------------------------------------------------ *
 * Reference resolution
 * ------------------------------------------------------------------------ */

bool QosMgr::resolvePortQosField(const string &field, const string &mapName, string &reason)
{
    if (field == QOS_FIELD_DSCP_TO_TC)
        return m_dscpToTcMap.find(mapName) != m_dscpToTcMap.end();
    if (field == QOS_FIELD_DOT1P_TO_TC)
        return m_dot1pToTcMap.find(mapName) != m_dot1pToTcMap.end();
    if (field == QOS_FIELD_TC_TO_QUEUE)
        return m_tcToQueueMap.find(mapName) != m_tcToQueueMap.end();
    if (field == QOS_FIELD_TC_TO_DSCP)
        return m_tcToDscpMap.find(mapName) != m_tcToDscpMap.end();
    if (field == QOS_FIELD_SCHEDULER)
        return m_schedulerMap.find(mapName) != m_schedulerMap.end();
    if (field == QOS_FIELD_WRED_PROFILE)
        return m_wredMap.find(mapName) != m_wredMap.end();

    reason = "unknown port_qos_map field: " + field;
    return false;
}

/* ------------------------------------------------------------------------ *
 * Kernel programming helpers
 * ------------------------------------------------------------------------ */

bool QosMgr::interfaceExists(const string &iface)
{
    string cmd = string(IP_CMD) + " link show " + iface;
    string res;
    return swss::exec(cmd, res) == 0;
}

void QosMgr::getAllPorts(vector<string> &ports)
{
    vector<string> keys;
    m_cfgPortTable.getKeys(keys);
    for (const auto &key : keys)
        ports.push_back(key);
}

void QosMgr::ensureClsact(const string &iface)
{
    /* Attach the clsact qdisc (ingress + egress hooks). Idempotent; tc returns
     * EEXIST if already present, which we ignore. */
    ostringstream cmd;
    cmd << TC_CMD << " qdisc replace dev " << iface << " clsact";
    string ignored;
    execTc(cmd.str(), ignored, "ensure clsact");
}

void QosMgr::reapplyMapBindings(const string &field, const string &name)
{
    /* Re-apply the tc realization for every port whose PORT_QOS_MAP references
     * the changed map (field -> name). Same dependency pattern as SCHEDULER ->
     * PORT / POLICER -> ACL. */
    for (const auto &entry : m_portQosMap)
    {
        const string &port = entry.first;
        const map<string, string> &maps = entry.second;
        if (maps.count(field) && maps.at(field) == name)
        {
            string reason;
            applyMapsToPort(port, maps, reason);
        }
    }
}

bool QosMgr::isKnownPortQosField(const string &field)
{
    return field == QOS_FIELD_DSCP_TO_TC ||
           field == QOS_FIELD_DOT1P_TO_TC ||
           field == QOS_FIELD_TC_TO_QUEUE ||
           field == QOS_FIELD_TC_TO_DSCP ||
           field == QOS_FIELD_SCHEDULER ||
           field == QOS_FIELD_WRED_PROFILE;
}

bool QosMgr::applyMapsToPort(const string &port,
                             const map<string, string> &maps, string &reason)
{
    string iface = kernutil::resolveInterface(port);
    string res;

    /* ---- ingress classification (clsact) ---- */

    if (maps.count(QOS_FIELD_DSCP_TO_TC))
    {
        /* DSCP_TO_TC_MAP is GLOBAL in SONiC — apply the classifier to every
         * port's ingress, not just the PORT_QOS_MAP port. A fixed prio per DSCP
         * value + `filter replace` keeps this idempotent across re-applies. */
        const auto &m = m_dscpToTcMap[maps.at(QOS_FIELD_DSCP_TO_TC)];
        vector<string> ports;
        getAllPorts(ports);
        if (ports.empty())
            ports.push_back(port);
        for (const auto &p : ports)
        {
            string piface = kernutil::resolveInterface(p);
            if (!interfaceExists(piface))
                continue;
            ensureClsact(piface);
            for (const auto &kv : m)
            {
                string tos = kernutil::dscpToTos(kv.first);
                if (tos.empty())
                    continue;
                long long dscp = 0;
                parseInt(kv.first, dscp);
                long long prio = 100 + dscp;

                /* `filter replace` without a handle fails with EEXIST when a
                 * stale filter already occupies this prio, so remove any
                 * existing filter at this prio first (best effort). */
                ostringstream del;
                del << TC_CMD << " filter del dev " << piface << " ingress prio " << prio;
                execTcQuiet(del.str());

                /* `flower ip_tos` is silently dropped on this kernel (yields a
                 * match-all filter), so match the DSCP bits with the u32
                 * classifier instead. TOS byte = dscp << 2; mask 0xfc ignores
                 * the two ECN bits. */
                ostringstream cmd;
                cmd << TC_CMD << " filter add dev " << piface << " ingress protocol ip prio "
                    << prio << " u32 match ip tos 0x" << hex << (dscp << 2)
                    << " 0xfc action skbedit priority " << kv.second;
                SWSS_LOG_NOTICE("Executing: %s", cmd.str().c_str());
                execTc(cmd.str(), res, "install DSCP classifier");
            }
        }
    }

    if (maps.count(QOS_FIELD_DOT1P_TO_TC))
    {
        const auto &m = m_dot1pToTcMap[maps.at(QOS_FIELD_DOT1P_TO_TC)];
        vector<string> ports;
        getAllPorts(ports);
        if (ports.empty())
            ports.push_back(port);
        for (const auto &p : ports)
        {
            string piface = kernutil::resolveInterface(p);
            if (!interfaceExists(piface))
                continue;
            ensureClsact(piface);
            for (const auto &kv : m)
            {
                long long pcp = 0;
                parseInt(kv.first, pcp);
                ostringstream cmd;
                cmd << TC_CMD << " filter replace dev " << piface << " ingress prio "
                    << (200 + pcp)
                    << " flower vlan_prio " << kv.first
                    << " action skbedit priority " << kv.second;
                SWSS_LOG_NOTICE("Executing: %s", cmd.str().c_str());
                execTc(cmd.str(), res, "install DOT1P classifier");
            }
        }
    }

    if (maps.count(QOS_FIELD_TC_TO_DSCP))
    {
        /* Egress DSCP rewrite via pedit, matched on the skb priority set at
         * ingress classification. */
        const auto &m = m_tcToDscpMap[maps.at(QOS_FIELD_TC_TO_DSCP)];
        ensureClsact(iface);
        for (const auto &kv : m)
        {
            long long tc = 0;
            parseInt(kv.first, tc);
            ostringstream cmd;
            cmd << TC_CMD << " filter replace dev " << iface << " egress prio "
                << (300 + tc)
                << " flower match meta priority " << kv.first
                << " " << kernutil::peditSetDscpToTc(kv.second);
            SWSS_LOG_NOTICE("Executing: %s", cmd.str().c_str());
            execTc(cmd.str(), res, "install TC-to-DSCP classifier");
        }
    }

    /* ---- egress queue/scheduler tree ---- */
    buildQueueTree(port, reason);

    reason.clear();
    return true;
}

void QosMgr::buildQueueTree(const string &port, string &reason)
{
    auto pqi = m_portQosMap.find(port);
    if (pqi == m_portQosMap.end())
        return;
    const auto &maps = pqi->second;
    if (!maps.count(QOS_FIELD_TC_TO_QUEUE))
        return;

    string iface = kernutil::resolveInterface(port);
    string res;

    const auto &tcq = m_tcToQueueMap[maps.at(QOS_FIELD_TC_TO_QUEUE)];

    /* Gather per-queue scheduling: TC_TO_QUEUE_MAP gives tc -> queue, QUEUE
     * table gives queue -> scheduler/wred. */
    struct QInfo
    {
        string type;
        long long weight;
        string wred;
    };
    map<int, QInfo> qinfo;
    for (const auto &kv : tcq)
    {
        long long tc = 0, q = 0;
        if (!parseInt(kv.first, tc) || !parseInt(kv.second, q))
            continue;
        QInfo info;
        info.weight = 1;
        string qkey = port + "|" + to_string(q);
        auto qit = m_queueMap.find(qkey);
        if (qit != m_queueMap.end())
        {
            const auto &qcfg = qit->second;
            if (qcfg.count(QOS_FIELD_SCHEDULER) &&
                m_schedulerMap.count(qcfg.at(QOS_FIELD_SCHEDULER)))
            {
                const auto &sched = m_schedulerMap.at(qcfg.at(QOS_FIELD_SCHEDULER));
                info.type = sched.count(SCHED_FIELD_TYPE) ? sched.at(SCHED_FIELD_TYPE)
                                                          : SCHED_TYPE_WRR;
                if (sched.count(SCHED_FIELD_WEIGHT))
                    parseInt(sched.at(SCHED_FIELD_WEIGHT), info.weight);
            }
            if (qcfg.count(QOS_FIELD_WRED_PROFILE))
                info.wred = qcfg.at(QOS_FIELD_WRED_PROFILE);
        }
        qinfo[(int)q] = info;
    }

    /* Port shaping rate (mbit/sec): an explicit PORT_QOS_MAP scheduler pir/cir
     * (bytes/sec) wins; otherwise derive self-contained congestion from the sum
     * of queue weights (weight x 1mbit) so weighted/strict scheduling is
     * observable on a veth that has no physical rate. */
    long long rootMbit = 0;
    if (maps.count(QOS_FIELD_SCHEDULER) &&
        m_schedulerMap.count(maps.at(QOS_FIELD_SCHEDULER)))
    {
        const auto &ps = m_schedulerMap.at(maps.at(QOS_FIELD_SCHEDULER));
        long long rateBps = 0;
        if (ps.count(SCHED_FIELD_PIR))
            parseInt(ps.at(SCHED_FIELD_PIR), rateBps);
        else if (ps.count(SCHED_FIELD_CIR))
            parseInt(ps.at(SCHED_FIELD_CIR), rateBps);
        rootMbit = rateBps * 8 / 1000000;
    }
    if (rootMbit <= 0)
    {
        for (const auto &entry : qinfo)
        {
            long long w = entry.second.weight;
            if (w < 1)
                w = 1;
            rootMbit += w;
        }
    }
    if (rootMbit <= 0)
        rootMbit = 1;

    /* `htb` qdisc itself takes no `rate` — the port cap lives on the root class
     * 1:1, and the per-queue classes hang off it as children. */
    /* `qdisc replace` on an existing htb root (with classes) fails with
     * "Change operation not supported", so delete-then-add for a clean,
     * idempotent root on every rebuild. */
    ostringstream rootdel;
    rootdel << TC_CMD << " qdisc del dev " << iface << " root";
    execTcQuiet(rootdel.str());

    ostringstream root;
    root << TC_CMD << " qdisc add dev " << iface << " root handle 1: htb default 1";
    SWSS_LOG_NOTICE("Executing: %s", root.str().c_str());
    execTc(root.str(), res, "add root qdisc");

    ostringstream rc;
    rc << TC_CMD << " class replace dev " << iface << " parent 1: classid 1:1 htb rate "
       << rootMbit << "mbit ceil " << rootMbit << "mbit";
    SWSS_LOG_NOTICE("Executing: %s", rc.str().c_str());
    execTc(rc.str(), res, "replace root class");

    /* One class per queue (classid 1:(q+1), child of root 1:1). Queue 0 maps to
     * the root class itself. STRICT uses htb prio (weight = priority, higher
     * wins -> lower prio number); DWRR/WRR use htb rate (weight = bandwidth). */
    for (const auto &entry : qinfo)
    {
        int q = entry.first;
        const QInfo &info = entry.second;
        if (q == 0)
            continue; // queue 0 == root class 1:1
        int classid = q + 1;

        ostringstream cls;
        cls << TC_CMD << " class replace dev " << iface << " parent 1:1 classid 1:"
            << classid << " htb";
        if (info.type == SCHED_TYPE_STRICT)
        {
            long long w = info.weight;
            if (w < 0)
                w = 0;
            if (w > 7)
                w = 7;
            int prio = 7 - (int)w; // lower htb prio = higher priority
            /* Strict priority: give each class only a minimal guaranteed rate
             * so htb `prio` (borrowing order) decides the split under
             * congestion. A large rate (e.g. 1gbit) leaves the class "always
             * within rate", which defeats the priority ordering. */
            cls << " prio " << prio << " rate 1mbit ceil 1gbit";
        }
        else
        {
            long long w = info.weight;
            if (w < 1)
                w = 1;
            cls << " prio 7 rate " << w << "mbit ceil 1gbit";
        }
        SWSS_LOG_NOTICE("Executing: %s", cls.str().c_str());
        execTc(cls.str(), res, "replace queue class");

        /* WRED leaf under the queue class. Prefer green (single-color)
         * thresholds; fall back to red. */
        if (!info.wred.empty() && m_wredMap.count(info.wred))
        {
            const auto &cfg = m_wredMap.at(info.wred);
            string mn, mx, prob;
            bool haveThresh = false;
            if (cfg.count(WRED_FIELD_GREEN_MIN_THRESHOLD) &&
                cfg.count(WRED_FIELD_GREEN_MAX_THRESHOLD))
            {
                mn = cfg.at(WRED_FIELD_GREEN_MIN_THRESHOLD);
                mx = cfg.at(WRED_FIELD_GREEN_MAX_THRESHOLD);
                prob = cfg.count(WRED_FIELD_GREEN_DROP_PROBABILITY)
                           ? cfg.at(WRED_FIELD_GREEN_DROP_PROBABILITY)
                           : "10";
                haveThresh = true;
            }
            else if (cfg.count(WRED_FIELD_RED_MIN_THRESHOLD) &&
                     cfg.count(WRED_FIELD_RED_MAX_THRESHOLD))
            {
                mn = cfg.at(WRED_FIELD_RED_MIN_THRESHOLD);
                mx = cfg.at(WRED_FIELD_RED_MAX_THRESHOLD);
                prob = cfg.count(WRED_FIELD_RED_DROP_PROBABILITY)
                           ? cfg.at(WRED_FIELD_RED_DROP_PROBABILITY)
                           : "10";
                haveThresh = true;
            }
            if (haveThresh)
            {
                long long maxBytes = 0, probPct = 0;
                parseInt(mx, maxBytes);
                parseInt(prob, probPct);
                long long limit = maxBytes > 0 ? maxBytes * 2 : 1000000;
                if (limit < 1000000)
                    limit = 1000000;
                string ecn = cfg.count(WRED_FIELD_ECN) ? cfg.at(WRED_FIELD_ECN)
                                                       : "ecn_none";
                double probFrac = static_cast<double>(probPct) / 100.0;
                ostringstream red;
                red << TC_CMD << " qdisc replace dev " << iface << " parent 1:"
                    << classid << " handle " << (20 + q) << ": red"
                    << " limit " << limit << " min " << mn << " max " << mx
                    << " avpkt 1000 probability " << probFrac;
                if (ecn != "ecn_none")
                    red << " ecn";
                SWSS_LOG_NOTICE("Executing: %s", red.str().c_str());
                execTc(red.str(), res, "replace WRED qdisc");
            }
        }
    }

    /* Route ingress-classified traffic (skb->priority = tc) to the correct
     * queue class. `htb` does not consult skb->priority on its own, so attach
     * one `basic meta(priority eq <tc>)` filter per tc -> queue mapping. */
    for (const auto &kv : tcq)
    {
        long long tc = 0, q = 0;
        if (!parseInt(kv.first, tc) || !parseInt(kv.second, q))
            continue;
        ostringstream filt;
        filt << TC_CMD << " filter replace dev " << iface << " parent 1: prio "
             << tc << " basic match \"meta(priority eq " << tc << ")\" classid 1:"
             << (q + 1);
        SWSS_LOG_NOTICE("Executing: %s", filt.str().c_str());
        execTc(filt.str(), res, "replace TC-to-queue filter");
    }
}

/* ------------------------------------------------------------------------ *
 * PORT_QOS_MAP — bind maps to a port
 * ------------------------------------------------------------------------ */

void QosMgr::doPortQosMapTask(Consumer &consumer)
{
    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
        auto &t = it->second;
        string key = kfvKey(t);
        string op = kfvOp(t);

        if (op == SET_COMMAND)
        {
            /* Resolve all referenced maps first; defer only if a referenced map
             * is genuinely not created yet. Unknown/unimplemented fields are
             * rejected outright (inactive) rather than deferring forever. */
            map<string, string> maps;
            bool allResolved = true;
            bool invalidField = false;
            string reason;

            for (const auto &fv : kfvFieldsValues(t))
            {
                string field = fvField(fv);
                string mapName = fvValue(fv);
                maps[field] = mapName;
                if (!isKnownPortQosField(field))
                {
                    reason = "unsupported field: " + field;
                    SWSS_LOG_WARN("PORT_QOS_MAP %s: %s", key.c_str(), reason.c_str());
                    invalidField = true;
                    break;
                }
                if (!resolvePortQosField(field, mapName, reason))
                {
                    SWSS_LOG_WARN("PORT_QOS_MAP %s: %s -> %s not resolved yet, deferring",
                                  key.c_str(), field.c_str(), mapName.c_str());
                    allResolved = false;
                    break;
                }
            }

            if (invalidField)
            {
                vector<FieldValueTuple> fvs;
                fvs.emplace_back("status", "inactive");
                fvs.emplace_back("reason", reason);
                m_statePortQosMapTable.set(key, fvs);
                it = consumer.m_toSync.erase(it);
                continue;
            }

            if (!allResolved)
            {
                /* Defer: retried on the 1s doTask() timeout once the map exists. */
                it++;
                continue;
            }

            m_portQosMap[key] = maps;

            string iface = kernutil::resolveInterface(key);
            bool ok = true;
            string status = "active";
            if (!interfaceExists(iface))
            {
                ok = false;
                status = "inactive";
                reason = "interface " + iface + " not found";
                SWSS_LOG_WARN("PORT_QOS_MAP %s: %s", key.c_str(), reason.c_str());
            }
            else if (!applyMapsToPort(key, maps, reason))
            {
                ok = false;
                status = "inactive";
            }

            vector<FieldValueTuple> fvs;
            fvs.emplace_back("status", status);
            if (!ok)
                fvs.emplace_back("reason", reason);
            m_statePortQosMapTable.set(key, fvs);

            it = consumer.m_toSync.erase(it);
        }
        else if (op == DEL_COMMAND)
        {
            /* Remove the tc realization for this port (root qdisc + ingress
             * classification filters). Without this, a removed PORT_QOS_MAP
             * leaves a stale shaper/qdisc shaping traffic. */
            string iface = kernutil::resolveInterface(key);
            string res;
            string cmd = string(TC_CMD) + " qdisc del dev " + iface + " root";
            execTc(cmd, res, "delete port root qdisc");
            cmd = string(TC_CMD) + " filter del dev " + iface + " ingress";
            execTc(cmd, res, "delete port ingress filters");

            m_portQosMap.erase(key);
            m_statePortQosMapTable.del(key);
            it = consumer.m_toSync.erase(it);
        }
        else
        {
            it = consumer.m_toSync.erase(it);
        }
    }
}

/* ------------------------------------------------------------------------ *
 * QUEUE — bind scheduler + WRED to a port's queue
 * ------------------------------------------------------------------------ */

void QosMgr::doQueueTask(Consumer &consumer)
{
    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
        auto &t = it->second;
        string key = kfvKey(t);
        string op = kfvOp(t);

        if (op == SET_COMMAND)
        {
            /* key: "<port>|<queue>" (queue may be a single index or a range;
             * for the kernel path we support a single queue index). */
            string port;
            size_t sep = key.find('|');
            if (sep != string::npos)
            {
                port = key.substr(0, sep);
            }
            else
            {
                SWSS_LOG_WARN("QUEUE: malformed key '%s'", key.c_str());
                it = consumer.m_toSync.erase(it);
                continue;
            }

            string scheduler, wred;
            bool allResolved = true;
            string reason;

            for (const auto &fv : kfvFieldsValues(t))
            {
                string field = fvField(fv);
                string value = fvValue(fv);
                if (field == QOS_FIELD_SCHEDULER)
                {
                    scheduler = value;
                    if (m_schedulerMap.find(value) == m_schedulerMap.end())
                        allResolved = false;
                }
                else if (field == QOS_FIELD_WRED_PROFILE)
                {
                    wred = value;
                    if (m_wredMap.find(value) == m_wredMap.end())
                        allResolved = false;
                }
                else
                {
                    SWSS_LOG_WARN("QUEUE %s: unknown field '%s'", key.c_str(), field.c_str());
                }
            }

            if (!allResolved)
            {
                SWSS_LOG_WARN("QUEUE %s: referenced scheduler/wred not resolved yet, deferring",
                              key.c_str());
                it++;
                continue;
            }

            map<string, string> cfg;
            if (!scheduler.empty())
                cfg[QOS_FIELD_SCHEDULER] = scheduler;
            if (!wred.empty())
                cfg[QOS_FIELD_WRED_PROFILE] = wred;
            m_queueMap[key] = cfg;

            string iface = kernutil::resolveInterface(port);
            bool ok = true;
            string status = "active";
            if (!interfaceExists(iface))
            {
                ok = false;
                status = "inactive";
                reason = "interface " + iface + " not found";
                SWSS_LOG_WARN("QUEUE %s: %s", key.c_str(), reason.c_str());
            }
            else
            {
                /* Rebuild the port's queue tree so the new scheduler/wred
                 * binding is reflected (no-op until PORT_QOS_MAP exists). */
                buildQueueTree(port, reason);
            }

            vector<FieldValueTuple> fvs;
            fvs.emplace_back("status", status);
            if (!ok)
                fvs.emplace_back("reason", reason);
            m_stateQueueTable.set(key, fvs);

            it = consumer.m_toSync.erase(it);
        }
        else if (op == DEL_COMMAND)
        {
            /* Remove the child qdisc for this queue (parent class 1:(queue+1)). */
            string port, queue;
            size_t sep = key.find('|');
            if (sep != string::npos)
            {
                port = key.substr(0, sep);
                queue = key.substr(sep + 1);

                long long qnum = 0;
                parseInt(queue, qnum);
                if (qnum < 0)
                    qnum = 0;

                string iface = kernutil::resolveInterface(port);
                string res;
                ostringstream cmd;
                cmd << TC_CMD << " qdisc del dev " << iface
                    << " parent 1:" << (qnum + 1);
                execTc(cmd.str(), res, "delete queue qdisc");
            }

            m_queueMap.erase(key);
            m_stateQueueTable.del(key);
            it = consumer.m_toSync.erase(it);
        }
        else
        {
            it = consumer.m_toSync.erase(it);
        }
    }
}
