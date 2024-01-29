#include "logger.h"
#include "dbconnector.h"
#include "producerstatetable.h"
#include "tokenize.h"
#include "ipprefix.h"
#include "fabricmgr.h"
#include "exec.h"
#include "shellcmd.h"
#include <swss/redisutility.h>

using namespace std;
using namespace swss;

FabricMgr::FabricMgr(DBConnector *cfgDb, DBConnector *appDb, const vector<string> &tableNames) :
        Orch(cfgDb, tableNames),
        m_cfgFabricMonitorTable(cfgDb, CFG_FABRIC_MONITOR_DATA_TABLE_NAME),
        m_cfgFabricPortTable(cfgDb, CFG_FABRIC_MONITOR_PORT_TABLE_NAME),
        m_appFabricMonitorTable(appDb, APP_FABRIC_MONITOR_DATA_TABLE_NAME),
        m_appFabricPortTable(appDb, APP_FABRIC_MONITOR_PORT_TABLE_NAME)
{
}

void FabricMgr::doTask(Consumer &consumer)
{
    SWSS_LOG_ENTER();

    auto table = consumer.getTableName();

    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
        KeyOpFieldsValuesTuple t = it->second;

        string key = kfvKey(t);
        string op = kfvOp(t);

        if (op == SET_COMMAND)
        {

            string monErrThreshCrcCells, monErrThreshRxCells;
            string monPollThreshRecovery, monPollThreshIsolation;
            string isolateStatus;
            string alias, lanes;
            std::vector<FieldValueTuple> field_values;
            string value;

            for (auto i : kfvFieldsValues(t))
            {
                if (fvField(i) == "monErrThreshCrcCells")
                {
                    monErrThreshCrcCells = fvValue(i);
                    writeConfigToAppDb(key, "monErrThreshCrcCells", monErrThreshCrcCells);
                }
                else if (fvField(i) == "monErrThreshRxCells")
                {
                    monErrThreshRxCells = fvValue(i);
                    writeConfigToAppDb(key, "monErrThreshRxCells", monErrThreshRxCells);
                }
                else if (fvField(i) == "monPollThreshRecovery")
                {
                    monPollThreshRecovery = fvValue(i);
                    writeConfigToAppDb(key, "monPollThreshRecovery", monPollThreshRecovery);
                }
                else if (fvField(i) == "monPollThreshIsolation")
                {
                    monPollThreshIsolation = fvValue(i);
                    writeConfigToAppDb(key, "monPollThreshIsolation", monPollThreshIsolation);
                }
                else if (fvField(i) == "alias")
                {
                    alias = fvValue(i);
                    writeConfigToAppDb(key, "alias", alias);
                }
                else if (fvField(i) == "lanes")
                {
                    lanes = fvValue(i);
                    writeConfigToAppDb(key, "lanes", lanes);
                }
                else if (fvField(i) == "isolateStatus")
                {
                    isolateStatus = fvValue(i);
                    writeConfigToAppDb(key, "isolateStatus", isolateStatus);
                }
                else
                {
                    field_values.emplace_back(i);
                }
            }

            for (auto &entry : field_values)
            {
                writeConfigToAppDb(key, fvField(entry), fvValue(entry));
            }

        }
        it = consumer.m_toSync.erase(it);
    }
}

bool FabricMgr::writeConfigToAppDb(const std::string &key, const std::string &field, const std::string &value)
{
    vector<FieldValueTuple> fvs;
    FieldValueTuple fv(field, value);
    fvs.push_back(fv);
    if (key == "FABRIC_MONITOR_DATA")
    {
        m_appFabricMonitorTable.set(key, fvs);
        SWSS_LOG_NOTICE("Write FABRIC_MONITOR:%s %s to %s", key.c_str(), field.c_str(), value.c_str());
    }
    else
    {
        m_appFabricPortTable.set(key, fvs);
        SWSS_LOG_NOTICE("Write FABRIC_PORT:%s %s to %s", key.c_str(), field.c_str(), value.c_str());
    }

    return true;
}


