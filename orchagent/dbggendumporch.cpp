#include "dbggendumporch.h"
#include "schema.h"

using namespace std;
using namespace swss;

DbgGenDumpOrch::DbgGenDumpOrch(TableConnector dbConnector, const std::string statusTableName):
        Orch(dbConnector.first, dbConnector.second),
        m_dbDumpTable(dbConnector.first, statusTableName)
{
    SWSS_LOG_ENTER();
}

DbgGenDumpOrch::~DbgGenDumpOrch()
{
    SWSS_LOG_ENTER();
}

void DbgGenDumpOrch::doTask(Consumer &consumer)
{
    SWSS_LOG_ENTER();
    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
        if (consumer.getTableName() == APP_DBG_GEN_DUMP_TABLE_NAME)
        {
            KeyOpFieldsValuesTuple t = it->second;
            string op = kfvOp(t);
            if (op == SET_COMMAND)
            {
                auto& fieldValues = kfvFieldsValues(t);
                auto value = fvValue(fieldValues[0]);
                const char* value_cstr = value.c_str();
                SWSS_LOG_INFO("call sai_dbg_generate_dump with file %s\n", value_cstr);
                sai_status_t ret = sai_dbg_generate_dump(value_cstr);
                if (ret != SAI_STATUS_SUCCESS)
                {
                    SWSS_LOG_ERROR("Failed to generate dbg dump file %s\n", value_cstr);
                }
                else
                {
                    SWSS_LOG_DEBUG("generate dbg dump file %s successfully\n", value_cstr);
                }

                //write to state DB the return status to inform the caller
                string ret_str = std::to_string(ret);
                string key = kfvKey(t);
                std::vector<swss::FieldValueTuple> fvTuples;
                fvTuples.push_back(swss::FieldValueTuple("status", ret_str));
                m_dbDumpTable.set(key, fvTuples);
            }
        }
        consumer.m_toSync.erase(it++);
    }
}
