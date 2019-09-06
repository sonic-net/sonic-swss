#include "logger.h"
#include "dbconnector.h"
#include "producerstatetable.h"
#include "tokenize.h"
#include "ipprefix.h"
#include "sflowmgr.h"
#include "exec.h"
#include "shellcmd.h"

using namespace std;
using namespace swss;

const map<string,string> sflowSpeedRateInitMap =
{
    {SFLOW_SAMPLE_RATE_KEY_400G, SFLOW_SAMPLE_RATE_VALUE_400G},
    {SFLOW_SAMPLE_RATE_KEY_100G, SFLOW_SAMPLE_RATE_VALUE_100G},
    {SFLOW_SAMPLE_RATE_KEY_50G, SFLOW_SAMPLE_RATE_VALUE_50G},
    {SFLOW_SAMPLE_RATE_KEY_40G, SFLOW_SAMPLE_RATE_VALUE_40G},
    {SFLOW_SAMPLE_RATE_KEY_25G, SFLOW_SAMPLE_RATE_VALUE_25G},
    {SFLOW_SAMPLE_RATE_KEY_10G, SFLOW_SAMPLE_RATE_VALUE_10G},
    {SFLOW_SAMPLE_RATE_KEY_1G, SFLOW_SAMPLE_RATE_VALUE_1G}
};
SflowMgr::SflowMgr(DBConnector *cfgDb, DBConnector *appDb, const vector<string> &tableNames) :
        Orch(cfgDb, tableNames),
        m_cfgSflowTable(cfgDb, CFG_SFLOW_TABLE_NAME),
        m_cfgSflowSessionTable(cfgDb, CFG_SFLOW_SESSION_TABLE_NAME),
        m_appSflowTable(appDb, APP_SFLOW_TABLE_NAME),
        m_appSflowSessionTable(appDb, APP_SFLOW_SESSION_TABLE_NAME),
        m_appSflowSpeedRateTable(appDb, APP_SFLOW_SAMPLE_RATE_TABLE_NAME)
{
    vector<FieldValueTuple> fieldValues;

    for (auto it : sflowSpeedRateInitMap)
    {
        fieldValues.emplace_back(it.first, it.second);
    }

    m_appSflowSpeedRateTable.set("global", fieldValues);
}

void SflowMgr::doTask(Consumer &consumer)
{
    stringstream cmd;
    string res;

    SWSS_LOG_ENTER();

    auto table = consumer.getTableName();

    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
        KeyOpFieldsValuesTuple t = it->second;

        string key = kfvKey(t);
        string op = kfvOp(t);
        auto values = kfvFieldsValues(t);

        if (op == SET_COMMAND)
        {
            if (table == CFG_SFLOW_TABLE_NAME)
            {
                for (auto i : values)
                {
                    if (fvField(i) == "admin_state")
                    {
                        if (fvValue(i) == "enable")
                        {
                            cmd << "service hsflowd restart";
                        }
                        else
                        {
                            cmd << "service hsflowd stop";
                        }

                        int ret = swss::exec(cmd.str(), res);
                        if (ret)
                        {
                            SWSS_LOG_ERROR("Command '%s' failed with rc %d", cmd.str().c_str(), ret);
                        }
                        else
                        {
                            SWSS_LOG_INFO("Command '%s' succeeded", cmd.str().c_str());
                        }
                    }
                }
                m_appSflowTable.set(key, values);
            }
            else
            {
                m_appSflowSessionTable.set(key, values);
            }
        }
        else if (op == DEL_COMMAND)
        {
            if (table == CFG_SFLOW_TABLE_NAME)
            {
                cmd << "service hsflowd stop";

                int ret = swss::exec(cmd.str(), res);
                if (ret)
                {
                    SWSS_LOG_ERROR("Command '%s' failed with rc %d", cmd.str().c_str(), ret);
                }
                else
                {
                    SWSS_LOG_INFO("Command '%s' succeeded", cmd.str().c_str());
                }
                m_appSflowTable.del(key);
            }
            else
            {
                m_appSflowSessionTable.del(key);
            }
        }

        it = consumer.m_toSync.erase(it);
    }
}
