#include <string.h>
#include "logger.h"
#include "schema.h"
#include "policermgr.h"

using namespace std;
using namespace swss;

PolicerMgr::PolicerMgr(DBConnector *cfgDb, DBConnector *stateDb,
                       const vector<string> &tableNames) :
    Orch(cfgDb, stateDb, tableNames, {}),
    m_statePolicerTable(stateDb, STATE_POLICER_TABLE_NAME)
{
    SWSS_LOG_ENTER();
    SWSS_LOG_NOTICE("PolicerMgr initialized, subscribed to %zu CONFIG_DB tables", tableNames.size());
}

void PolicerMgr::doTask(Consumer &consumer)
{
    string table_name = consumer.getTableName();

    SWSS_LOG_DEBUG("doTask: table=%s", table_name.c_str());

    if (table_name == CFG_POLICER_TABLE_NAME)
        doPolicerTask(consumer);
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
            string meter_type, mode, cir, cbs, pir, pbs;
            string green_action, red_action, yellow_action;

            for (auto i : kfvFieldsValues(t))
            {
                string field = fvField(i);
                string value = fvValue(i);

                if (field == POLICER_FIELD_METER_TYPE)       meter_type = value;
                else if (field == POLICER_FIELD_MODE)        mode = value;
                else if (field == POLICER_FIELD_CIR)         cir = value;
                else if (field == POLICER_FIELD_CBS)         cbs = value;
                else if (field == POLICER_FIELD_PIR)         pir = value;
                else if (field == POLICER_FIELD_PBS)         pbs = value;
                else if (field == POLICER_FIELD_GREEN_ACTION) green_action = value;
                else if (field == POLICER_FIELD_RED_ACTION)   red_action = value;
                else if (field == POLICER_FIELD_YELLOW_ACTION) yellow_action = value;
            }

            SWSS_LOG_NOTICE("POLICER SET: %s meter_type=%s mode=%s cir=%s cbs=%s pir=%s pbs=%s green=%s red=%s",
                            policer_name.c_str(), meter_type.c_str(), mode.c_str(),
                            cir.c_str(), cbs.c_str(), pir.c_str(), pbs.c_str(),
                            green_action.c_str(), red_action.c_str());

            /* Validate: a single-rate (sr_tcm) policer needs CIR+CBS; a
             * two-rate (tr_tcm) also needs PIR+PBS. */
            bool ok = false;
            if (cir.empty() || cbs.empty())
            {
                SWSS_LOG_WARN("POLICER %s: missing mandatory CIR/CBS, marking inactive", policer_name.c_str());
            }
            else if (mode == "tr_tcm" && (pir.empty() || pbs.empty()))
            {
                SWSS_LOG_WARN("POLICER %s: tr_tcm requires PIR/PBS, marking inactive", policer_name.c_str());
            }
            else
            {
                ok = true;
                m_registeredPolicers.insert(policer_name);
            }

            vector<FieldValueTuple> fvs;
            fvs.emplace_back("status", ok ? "active" : "inactive");
            m_statePolicerTable.set(policer_name, fvs);

            it = consumer.m_toSync.erase(it);
        }
        else if (op == DEL_COMMAND)
        {
            SWSS_LOG_NOTICE("POLICER DEL: %s", policer_name.c_str());

            m_registeredPolicers.erase(policer_name);
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
