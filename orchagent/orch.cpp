#include "orch.h"

#include "logger.h"

Orch::Orch(DBConnector *db, string tableName) :
    m_db(db), m_name(tableName)
{
    m_consumer = new ConsumerTable(m_db, tableName);
}

Orch::~Orch()
{
    delete(m_db);
    delete(m_consumer);
}

void Orch::execute()
{
    SWSS_LOG_ENTER();

    KeyOpFieldsValuesTuple t;
    m_consumer->pop(t);

    string key = kfvKey(t);
    string op  = kfvOp(t);

#ifdef DEBUG
    string debug = "Orch : " + m_name + " key : " + kfvKey(t) + " op : "  + kfvOp(t);
    for (auto i = kfvFieldsValues(t).begin(); i != kfvFieldsValues(t).end(); i++)
        debug += " " + fvField(*i) + " : " + fvValue(*i);
    SWSS_LOG_DEBUG("%s\n", debug.c_str());
#endif

    /* If a new task comes or if a DEL task comes, we directly put it into m_toSync map */
    if ( m_toSync.find(key) == m_toSync.end() || op == DEL_COMMAND)
    {
       m_toSync[key] = t;
    }
    /* If an old task is still there, we combine the old task with new task */
    else
    {
        KeyOpFieldsValuesTuple u = m_toSync[key];

        auto tt = kfvFieldsValues(t);
	auto uu = kfvFieldsValues(u);


        for (auto it = tt.begin(); it != tt.end(); it++)
        {
            string field = fvField(*it);
            string value = fvValue(*it);

            auto iu = uu.begin();
            while (iu != uu.end())
            {
                string ofield = fvField(*iu);
                if (field == ofield)
                    iu = uu.erase(iu);
                else
                    iu++;
            }
            uu.push_back(FieldValueTuple(field, value));
        }
        m_toSync[key] = KeyOpFieldsValuesTuple(key, op, uu);
    }

    doTask();
}
