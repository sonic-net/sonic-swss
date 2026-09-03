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
                        string iface = kernutil::resolveInterface(port);
                        applyMapsToPort(iface, maps, reason);
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
                    string queue = key.substr(sep + 1);
                    string wred = cfg.count(QOS_FIELD_WRED_PROFILE) ?
                                  cfg.at(QOS_FIELD_WRED_PROFILE) : "";
                    string iface = kernutil::resolveInterface(port);
                    applyQueueToPort(iface, queue, name, wred, reason);
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
                    swss::exec(cmd, res);
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
            long long redMin = -1, redMax = -1;

            for (const auto &fv : kfvFieldsValues(t))
            {
                string field = fvField(fv);
                string value = fvValue(fv);
                cfg[field] = value;

                if (field == WRED_FIELD_RED_MIN_THRESHOLD)
                {
                    if (!parseInt(value, redMin) || redMin < 0)
                    {
                        ok = false;
                        reason = "invalid red_min_threshold";
                    }
                }
                else if (field == WRED_FIELD_RED_MAX_THRESHOLD)
                {
                    if (!parseInt(value, redMax) || redMax < 0)
                    {
                        ok = false;
                        reason = "invalid red_max_threshold";
                    }
                }
                else if (field == WRED_FIELD_RED_DROP_PROBABILITY)
                {
                    if (!intInRange(value, 0, 100))
                    {
                        ok = false;
                        reason = "invalid red_drop_probability (0..100)";
                    }
                }
                else if (field == WRED_FIELD_GREEN_MIN_THRESHOLD ||
                         field == WRED_FIELD_GREEN_MAX_THRESHOLD ||
                         field == WRED_FIELD_YELLOW_MIN_THRESHOLD ||
                         field == WRED_FIELD_YELLOW_MAX_THRESHOLD ||
                         field == WRED_FIELD_GREEN_DROP_PROBABILITY ||
                         field == WRED_FIELD_YELLOW_DROP_PROBABILITY)
                {
                    /* Color-aware WRED has no Linux 'red' equivalent; accepted
                     * for compatibility but not programmed (logged-ignored). */
                    SWSS_LOG_INFO("WRED_PROFILE %s: field %s accepted but ignored (no kernel equivalent)",
                                  name.c_str(), field.c_str());
                }
                else if (field == WRED_FIELD_ECN)
                {
                    /* ecn_none / ecn_red / ... all accepted; only ecn != none
                     * changes the tc red command at apply time. */
                }
                else
                {
                    ok = false;
                    reason = "unknown wred_profile field: " + field;
                }
            }

            if (cfg.find(WRED_FIELD_RED_MIN_THRESHOLD) == cfg.end() ||
                cfg.find(WRED_FIELD_RED_MAX_THRESHOLD) == cfg.end())
            {
                ok = false;
                reason = "missing mandatory red_min/max_threshold";
            }
            else if (ok && redMin > redMax)
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
    cmd << TC_CMD << " qdisc add dev " << iface << " clsact";
    string ignored;
    swss::exec(cmd.str(), ignored);
}

bool QosMgr::applyMapsToPort(const string &iface,
                             const map<string, string> &maps, string &reason)
{
    /* Best-effort tc programming. Each map type is applied to the interface;
     * failures are logged but do not abort the other maps. */
    string res;

    if (maps.count(QOS_FIELD_DSCP_TO_TC))
    {
        const auto &m = m_dscpToTcMap[maps.at(QOS_FIELD_DSCP_TO_TC)];
        /* DSCP_TO_TC_MAP is GLOBAL in SONiC — apply the classifier to every
         * port's ingress, not just the PORT_QOS_MAP port. */
        vector<string> ports;
        getAllPorts(ports);
        if (ports.empty())
            ports.push_back(iface);
        for (const auto &port : ports)
        {
            string piface = kernutil::resolveInterface(port);
            if (!interfaceExists(piface))
                continue;
            ensureClsact(piface);
            uint32_t prio = 100;
            for (const auto &kv : m)
            {
                string tos = kernutil::dscpToTos(kv.first);
                if (tos.empty())
                    continue;
                ostringstream cmd;
                cmd << TC_CMD << " filter add dev " << piface << " ingress prio " << prio++
                    << " flower ip_tos " << tos
                    << " action skbedit priority " << kv.second;
                SWSS_LOG_NOTICE("Executing: %s", cmd.str().c_str());
                swss::exec(cmd.str(), res);
            }
        }
    }

    if (maps.count(QOS_FIELD_DOT1P_TO_TC))
    {
        const auto &m = m_dot1pToTcMap[maps.at(QOS_FIELD_DOT1P_TO_TC)];
        vector<string> ports;
        getAllPorts(ports);
        if (ports.empty())
            ports.push_back(iface);
        for (const auto &port : ports)
        {
            string piface = kernutil::resolveInterface(port);
            if (!interfaceExists(piface))
                continue;
            ensureClsact(piface);
            uint32_t prio = 200;
            for (const auto &kv : m)
            {
                ostringstream cmd;
                cmd << TC_CMD << " filter add dev " << piface << " ingress prio " << prio++
                    << " flower vlan_prio " << kv.first
                    << " action skbedit priority " << kv.second;
                SWSS_LOG_NOTICE("Executing: %s", cmd.str().c_str());
                swss::exec(cmd.str(), res);
            }
        }
    }

    if (maps.count(QOS_FIELD_TC_TO_QUEUE))
    {
        const auto &m = m_tcToQueueMap[maps.at(QOS_FIELD_TC_TO_QUEUE)];
        /* Build the 16-entry skb->priority -> band map from TC_TO_QUEUE_MAP.
         * Use the software `prio` qdisc instead of `mqprio`: veth interfaces
         * have a single tx queue, so mqprio (which needs real hardware queues)
         * fails with "Device does not support hardware offload". */
        int priomap[16] = {0};
        for (const auto &kv : m)
        {
            long long tc = 0, queue = 0;
            if (parseInt(kv.first, tc) && parseInt(kv.second, queue) &&
                tc >= 0 && tc < 16 && queue >= 0 && queue < 16)
                priomap[tc] = (int)queue;
        }
        ostringstream cmd;
        cmd << TC_CMD << " qdisc replace dev " << iface << " root handle 1: prio bands 8 priomap";
        for (int i = 0; i < 16; i++)
            cmd << " " << priomap[i];
        SWSS_LOG_NOTICE("Executing: %s", cmd.str().c_str());
        swss::exec(cmd.str(), res);
    }

    if (maps.count(QOS_FIELD_TC_TO_DSCP))
    {
        const auto &m = m_tcToDscpMap[maps.at(QOS_FIELD_TC_TO_DSCP)];
        uint32_t prio = 300;
        for (const auto &kv : m)
        {
            ostringstream cmd;
            cmd << TC_CMD << " filter add dev " << iface << " egress prio " << prio++
                << " flower match meta priority " << kv.first
                << " " << kernutil::peditSetDscpToTc(kv.second);
            SWSS_LOG_NOTICE("Executing: %s", cmd.str().c_str());
            swss::exec(cmd.str(), res);
        }
    }

    if (maps.count(QOS_FIELD_SCHEDULER))
    {
        const auto &cfg = m_schedulerMap[maps.at(QOS_FIELD_SCHEDULER)];
        string rate = cfg.count(SCHED_FIELD_PIR) ? cfg.at(SCHED_FIELD_PIR)
                    : (cfg.count(SCHED_FIELD_CIR) ? cfg.at(SCHED_FIELD_CIR) : "");
        string burst = cfg.count(SCHED_FIELD_PBS) ? cfg.at(SCHED_FIELD_PBS)
                     : (cfg.count(SCHED_FIELD_CBS) ? cfg.at(SCHED_FIELD_CBS) : "");
        if (!rate.empty() && !burst.empty())
        {
            /* Port-level scheduler = port shaping. A single tbf shaper caps the
             * whole port to the scheduler's peak (max-bandwidth) rate/burst. */
            ostringstream cmd;
            cmd << TC_CMD << " qdisc replace dev " << iface
                << " root handle 1: tbf rate " << rate << "bps burst " << burst
                << " latency 50ms";
            SWSS_LOG_NOTICE("Executing: %s", cmd.str().c_str());
            swss::exec(cmd.str(), res);
        }
    }

    reason.clear();
    return true;
}

bool QosMgr::applyQueueToPort(const string &iface, const string &queue,
                              const string &scheduler, const string &wred, string &reason)
{
    string res;

    /* The queue token may be a single index or a range ("0-1"); parse a safe
     * numeric prefix for the tc handle, defaulting to 0. */
    long long qnum = 0;
    parseInt(queue, qnum);
    if (qnum < 0)
        qnum = 0;

    if (!scheduler.empty())
    {
        const auto &cfg = m_schedulerMap[scheduler];
        string type = cfg.count(SCHED_FIELD_TYPE) ? cfg.at(SCHED_FIELD_TYPE) : SCHED_TYPE_WRR;
        ostringstream cmd;
        if (type == SCHED_TYPE_STRICT)
        {
            cmd << TC_CMD << " qdisc replace dev " << iface << " parent 1:" << (qnum + 1)
                << " handle " << (10 + qnum) << ": prio";
        }
        else
        {
            string quantum = cfg.count(SCHED_FIELD_WEIGHT) ? cfg.at(SCHED_FIELD_WEIGHT) : "1";
            cmd << TC_CMD << " qdisc replace dev " << iface << " parent 1:" << (qnum + 1)
                << " handle " << (10 + qnum) << ": drr quantum " << quantum;
        }
        SWSS_LOG_NOTICE("Executing: %s", cmd.str().c_str());
        swss::exec(cmd.str(), res);
    }

    if (!wred.empty())
    {
        const auto &cfg = m_wredMap[wred];
        string mn = cfg.count(WRED_FIELD_RED_MIN_THRESHOLD) ? cfg.at(WRED_FIELD_RED_MIN_THRESHOLD) : "0";
        string mx = cfg.count(WRED_FIELD_RED_MAX_THRESHOLD) ? cfg.at(WRED_FIELD_RED_MAX_THRESHOLD) : "100";
        string prob = cfg.count(WRED_FIELD_RED_DROP_PROBABILITY) ? cfg.at(WRED_FIELD_RED_DROP_PROBABILITY) : "10";
        string ecn = cfg.count(WRED_FIELD_ECN) ? cfg.at(WRED_FIELD_ECN) : "ecn_none";

        ostringstream cmd;
        cmd << TC_CMD << " qdisc replace dev " << iface << " parent 1:" << (qnum + 1)
            << " handle " << (20 + qnum) << ": red min " << mn << " max " << mx
            << " probability " << prob << "%";
        if (ecn != "ecn_none")
            cmd << " ecn";
        SWSS_LOG_NOTICE("Executing: %s", cmd.str().c_str());
        swss::exec(cmd.str(), res);
    }

    reason.clear();
    return true;
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
            /* Resolve all referenced maps first; defer if any is missing. */
            map<string, string> maps;
            bool allResolved = true;
            string reason;

            for (const auto &fv : kfvFieldsValues(t))
            {
                string field = fvField(fv);
                string mapName = fvValue(fv);
                maps[field] = mapName;
                if (!resolvePortQosField(field, mapName, reason))
                {
                    SWSS_LOG_WARN("PORT_QOS_MAP %s: %s -> %s not resolved yet, deferring",
                                  key.c_str(), field.c_str(), mapName.c_str());
                    allResolved = false;
                    break;
                }
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
            else if (!applyMapsToPort(iface, maps, reason))
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
            swss::exec(cmd, res);
            cmd = string(TC_CMD) + " filter del dev " + iface + " ingress";
            swss::exec(cmd, res);

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
            string port, queue;
            size_t sep = key.find('|');
            if (sep != string::npos)
            {
                port = key.substr(0, sep);
                queue = key.substr(sep + 1);
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
            else if (!applyQueueToPort(iface, queue, scheduler, wred, reason))
            {
                ok = false;
                status = "inactive";
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
                swss::exec(cmd.str(), res);
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
