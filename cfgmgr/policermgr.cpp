#include <string.h>
#include <sstream>
#include <set>
#include "logger.h"
#include "schema.h"
#include "exec.h"
#include "policermgr.h"
#include "kernutil.h"

using namespace std;
using namespace swss;

#define TC_CMD "/sbin/tc"

/* Valid policer packet actions (lowercase, per sonic-policer.yang). */
static bool validPolicerAction(const string &action)
{
    static const set<string> valid = {
        "drop", "forward", "copy", "trap", "log", "deny", "transit", "copy_cancel",
    };
    return action.empty() || valid.find(action) != valid.end();
}

PolicerMgr::PolicerMgr(DBConnector *cfgDb, DBConnector *stateDb,
                       const vector<string> &tableNames) :
    Orch(cfgDb, stateDb, tableNames, {}),
    m_statePolicerTable(stateDb, STATE_POLICER_TABLE_NAME)
{
    SWSS_LOG_ENTER();
    SWSS_LOG_NOTICE("PolicerMgr initialized, subscribed to %zu CONFIG_DB tables", tableNames.size());
}

bool PolicerMgr::policerExists(const string &name)
{
    return m_policerRefCounts.find(name) != m_policerRefCounts.end();
}

bool PolicerMgr::increaseRefCount(const string &name)
{
    if (!policerExists(name))
    {
        SWSS_LOG_WARN("Policer %s does not exist", name.c_str());
        return false;
    }
    ++m_policerRefCounts[name];
    return true;
}

bool PolicerMgr::decreaseRefCount(const string &name)
{
    if (!policerExists(name))
    {
        SWSS_LOG_WARN("Policer %s does not exist", name.c_str());
        return false;
    }
    --m_policerRefCounts[name];
    return true;
}

void PolicerMgr::doTask(Consumer &consumer)
{
    string table_name = consumer.getTableName();

    SWSS_LOG_DEBUG("doTask: table=%s", table_name.c_str());

    if (table_name == CFG_POLICER_TABLE_NAME)
        doPolicerTask(consumer);
    else if (table_name == CFG_PORT_STORM_CONTROL_TABLE_NAME)
        doPortStormControlTask(consumer);
    else
    {
        SWSS_LOG_ERROR("PolicerMgr doTask: unknown table '%s'", table_name.c_str());
        throw runtime_error("PolicerMgr doTask failure: unknown table " + table_name);
    }
}

void PolicerMgr::doPolicerTask(Consumer &consumer)
{
    SWSS_LOG_ENTER();

    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
        auto &t = it->second;
        string policer_name = kfvKey(t);
        string op = kfvOp(t);

        SWSS_LOG_INFO("POLICER: key=%s op=%s", policer_name.c_str(), op.c_str());

        if (op == SET_COMMAND)
        {
            string meter_type, mode, color, cir, cbs, pir, pbs;
            string green_action, red_action, yellow_action;
            vector<string> unknown_fields;

            for (auto i : kfvFieldsValues(t))
            {
                string field = fvField(i);
                string value = fvValue(i);

                if (field == POLICER_FIELD_METER_TYPE)       meter_type = value;
                else if (field == POLICER_FIELD_MODE)        mode = value;
                else if (field == POLICER_FIELD_COLOR)       color = value;
                else if (field == POLICER_FIELD_CIR)         cir = value;
                else if (field == POLICER_FIELD_CBS)         cbs = value;
                else if (field == POLICER_FIELD_PIR)         pir = value;
                else if (field == POLICER_FIELD_PBS)         pbs = value;
                else if (field == POLICER_FIELD_GREEN_ACTION) green_action = value;
                else if (field == POLICER_FIELD_RED_ACTION)   red_action = value;
                else if (field == POLICER_FIELD_YELLOW_ACTION) yellow_action = value;
                else
                    unknown_fields.push_back(field);
            }

            SWSS_LOG_NOTICE("POLICER SET: %s meter_type=%s mode=%s color=%s cir=%s cbs=%s pir=%s pbs=%s green=%s red=%s yellow=%s",
                            policer_name.c_str(), meter_type.c_str(), mode.c_str(),
                            color.c_str(), cir.c_str(), cbs.c_str(), pir.c_str(), pbs.c_str(),
                            green_action.c_str(), red_action.c_str(), yellow_action.c_str());

            bool ok = true;
            string reason;

            if (!unknown_fields.empty())
            {
                ok = false;
                for (auto &uf : unknown_fields)
                    SWSS_LOG_WARN("POLICER %s: unknown field '%s'", policer_name.c_str(), uf.c_str());
                reason = "unknown field";
            }
            else if (meter_type.empty() || mode.empty())
            {
                ok = false;
                reason = "missing mandatory meter_type/mode";
            }
            else if (meter_type != "packets" && meter_type != "bytes")
            {
                ok = false;
                reason = "invalid meter_type";
            }
            else if (mode != "sr_tcm" && mode != "tr_tcm" && mode != "storm_control")
            {
                ok = false;
                reason = "invalid mode";
            }
            else if (cir.empty() || cbs.empty())
            {
                ok = false;
                reason = "missing mandatory CIR/CBS";
            }
            else if (mode == "tr_tcm" && (pir.empty() || pbs.empty()))
            {
                ok = false;
                reason = "tr_tcm requires PIR/PBS";
            }
            else if (!validPolicerAction(green_action) || !validPolicerAction(red_action) ||
                     !validPolicerAction(yellow_action))
            {
                ok = false;
                reason = "invalid packet action";
            }

            if (ok && m_policerRefCounts.find(policer_name) == m_policerRefCounts.end())
                m_policerRefCounts[policer_name] = 0;

            if (!ok)
                SWSS_LOG_WARN("POLICER %s rejected: %s", policer_name.c_str(), reason.c_str());

            vector<FieldValueTuple> fvs;
            fvs.emplace_back("status", ok ? "active" : "inactive");
            if (!meter_type.empty())
                fvs.emplace_back("meter_type", meter_type);
            m_statePolicerTable.set(policer_name, fvs);

            it = consumer.m_toSync.erase(it);
        }
        else if (op == DEL_COMMAND)
        {
            SWSS_LOG_NOTICE("POLICER DEL: %s", policer_name.c_str());

            auto rit = m_policerRefCounts.find(policer_name);
            if (rit != m_policerRefCounts.end() && rit->second > 0)
            {
                SWSS_LOG_WARN("POLICER %s still referenced (refcount %d), deferring deletion",
                              policer_name.c_str(), rit->second);
                it++;
                continue;
            }

            m_policerRefCounts.erase(policer_name);
            m_statePolicerTable.del(policer_name);

            it = consumer.m_toSync.erase(it);
        }
        else
        {
            SWSS_LOG_ERROR("POLICER: unknown operation '%s'", op.c_str());
            it = consumer.m_toSync.erase(it);
        }
    }
}

void PolicerMgr::doPortStormControlTask(Consumer &consumer)
{
    SWSS_LOG_ENTER();

    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
        auto &t = it->second;
        string key = kfvKey(t);
        string op = kfvOp(t);

        /* Key: <port>|<storm_type> */
        string iface, storm_type;
        size_t sep = key.find('|');
        if (sep != string::npos)
        {
            iface = key.substr(0, sep);
            storm_type = key.substr(sep + 1);
        }
        else
        {
            SWSS_LOG_WARN("PORT_STORM_CONTROL: malformed key '%s'", key.c_str());
            it = consumer.m_toSync.erase(it);
            continue;
        }

        SWSS_LOG_INFO("PORT_STORM_CONTROL: iface=%s type=%s op=%s",
                      iface.c_str(), storm_type.c_str(), op.c_str());

        if (op == SET_COMMAND)
        {
            string kbps;
            for (auto i : kfvFieldsValues(t))
                if (fvField(i) == STORM_CONTROL_FIELD_KBPS)
                    kbps = fvValue(i);

            if (kbps.empty())
            {
                SWSS_LOG_WARN("PORT_STORM_CONTROL %s: missing kbps, skipping", key.c_str());
                it = consumer.m_toSync.erase(it);
                continue;
            }

            uint32_t prio;
            auto pit = m_stormPrio.find(key);
            if (pit != m_stormPrio.end())
                prio = pit->second;
            else
            {
                prio = m_nextStormPrio++;
                m_stormPrio[key] = prio;
            }

            addStormControlFilter(kernutil::resolveInterface(iface), storm_type, kbps, prio);

            it = consumer.m_toSync.erase(it);
        }
        else if (op == DEL_COMMAND)
        {
            auto pit = m_stormPrio.find(key);
            if (pit != m_stormPrio.end())
            {
                removeStormControlFilter(kernutil::resolveInterface(iface), pit->second);
                m_stormPrio.erase(pit);
            }
            it = consumer.m_toSync.erase(it);
        }
        else
        {
            SWSS_LOG_ERROR("PORT_STORM_CONTROL: unknown operation '%s'", op.c_str());
            it = consumer.m_toSync.erase(it);
        }
    }
}

void PolicerMgr::addStormControlFilter(const string &iface, const string &stormType,
                                       const string &kbps, uint32_t prio)
{
    SWSS_LOG_ENTER();

    string dstMac;
    if (stormType == "broadcast")
        dstMac = "ff:ff:ff:ff:ff:ff";
    else if (stormType == "unknown-multicast")
        dstMac = "01:00:00:00:00:00/01:00:00:00:00:00";
    else if (stormType == "unknown-unicast")
        dstMac = "00:00:00:00:00:00/01:00:00:00:00:00";
    else
    {
        SWSS_LOG_WARN("Unknown storm type '%s', skipping", stormType.c_str());
        return;
    }

    uint64_t bps = stoull(kbps) * 1000;      /* kbps -> bits/sec */
    uint64_t burst = bps / 8;                 /* bytes */

    ostringstream qdisc_cmd;
    qdisc_cmd << TC_CMD << " qdisc add dev " << iface << " clsact";
    string ignored;
    swss::exec(qdisc_cmd.str(), ignored);

    ostringstream del_cmd;
    del_cmd << TC_CMD << " filter del dev " << iface << " ingress prio " << prio;
    swss::exec(del_cmd.str(), ignored);

    ostringstream cmd;
    cmd << TC_CMD << " filter add dev " << iface << " ingress protocol all prio " << prio
        << " flower dst_mac " << dstMac
        << " skip_sw action police rate " << bps << " burst " << burst << " drop";

    SWSS_LOG_NOTICE("Executing: %s", cmd.str().c_str());

    string res;
    int ret = swss::exec(cmd.str(), res);
    if (ret != 0)
    {
        string cmd_sw = cmd.str();
        size_t skip = cmd_sw.find(" skip_sw");
        if (skip != string::npos)
            cmd_sw.erase(skip, 8);
        SWSS_LOG_NOTICE("Executing (fallback): %s", cmd_sw.c_str());
        ret = swss::exec(cmd_sw, res);
        if (ret != 0)
            SWSS_LOG_ERROR("tc storm-control add failed on %s (ret=%d): %s",
                           iface.c_str(), ret, res.c_str());
    }
}

void PolicerMgr::removeStormControlFilter(const string &iface, uint32_t prio)
{
    SWSS_LOG_ENTER();

    ostringstream cmd;
    cmd << TC_CMD << " filter del dev " << iface << " ingress prio " << prio;

    SWSS_LOG_NOTICE("Executing: %s", cmd.str().c_str());

    string res;
    int ret = swss::exec(cmd.str(), res);
    if (ret != 0)
        SWSS_LOG_WARN("tc storm-control del on %s prio %u (ret=%d): %s",
                      iface.c_str(), prio, ret, res.c_str());
}
