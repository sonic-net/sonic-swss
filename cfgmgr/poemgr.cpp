#include "poemgr.h"
#include "logger.h"
#include "tokenize.h"
#include "warm_restart.h"
#include "converter.h"

using namespace swss;

PoeMgr::PoeMgr(DBConnector *appDb, DBConnector *cfgDb, const std::vector<std::string> &poeTables) :
        Orch(cfgDb, poeTables),
        m_appPoeTable(appDb, APP_POE_TABLE_NAME)
{
    SWSS_LOG_ENTER();
}

void PoeMgr::doTask(Consumer &consumer)
{
    SWSS_LOG_ENTER();
    std::string table_name = consumer.getTableName();
    if (table_name != CFG_POE_TABLE_NAME)
    {
        SWSS_LOG_ERROR("Unknown config table %s ", table_name.c_str());
        throw std::runtime_error("PoeMgr doTask failure.");
    }

    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
        KeyOpFieldsValuesTuple t = it->second;
        std::string alias = kfvKey(t);
        std::string op = kfvOp(t);

        SWSS_LOG_NOTICE("TABLE key: %s : %s", alias.c_str(), op.c_str());
        if (op == SET_COMMAND)
        {
            SWSS_LOG_NOTICE("Add PoE port: %s", alias.c_str());
            m_appPoeTable.set(alias, kfvFieldsValues(t));
        }
        else if (op == DEL_COMMAND)
        {
            SWSS_LOG_NOTICE("Removing PoE port: %s", alias.c_str());
            m_appPoeTable.del(alias);
        }
        else
        {
            SWSS_LOG_ERROR("Unknown operation type %s", op.c_str());
        }
        it = consumer.m_toSync.erase(it);
    }
}
