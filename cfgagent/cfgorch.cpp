#include <fstream>
#include <iostream>
#include <mutex>
#include <sys/time.h>

#include "cfgorch.h"
#include "tokenize.h"
#include "logger.h"

using namespace swss;

extern int gBatchSize;

extern mutex gDbMutex;

extern bool gSwssCfgRecord;
extern ofstream gCfgRecordOfs;
extern string gCfgRecordFile;
extern bool gInitDone;

CfgOrch::CfgOrch(DBConnector *db, string tableName) :
    m_db(db)
{
    Consumer consumer(new Subscriber(m_db, tableName));
    m_consumerMap.insert(ConsumerMapPair(tableName, consumer));
}

CfgOrch::CfgOrch(DBConnector *db, vector<string> &tableNames) :
    m_db(db)
{
    for(auto it : tableNames)
    {
        Consumer consumer(new Subscriber(m_db, it));
        m_consumerMap.insert(ConsumerMapPair(it, consumer));
    }
}

CfgOrch::~CfgOrch()
{
    delete(m_db);
    for(auto &it : m_consumerMap)
        delete it.second.m_consumer;

    if (gCfgRecordOfs.is_open())
    {
        gCfgRecordOfs.close();
    }
}

vector<Selectable *> CfgOrch::getSelectables()
{
    vector<Selectable *> selectables;
    for(auto it : m_consumerMap) {
        selectables.push_back(it.second.m_consumer);
    }
    return selectables;
}

bool CfgOrch::hasSelectable(Subscriber *selectable) const
{
    for(auto it : m_consumerMap) {
        if (it.second.m_consumer == selectable) {
            return true;
        }
    }
    return false;
}

bool CfgOrch::syncCfgDB(string tableName, Table &tableConsumer)
{
    SWSS_LOG_ENTER();

    lock_guard<mutex> lock(gDbMutex);

    auto consumer_it = m_consumerMap.find(tableName);
    if (consumer_it == m_consumerMap.end())
    {
        SWSS_LOG_ERROR("Unrecognized tableName:%s\n", tableName.c_str());
        return false;
    }
    Consumer& consumer = consumer_it->second;

    KeyOpFieldsValuesTuple tuple;
    vector<KeyOpFieldsValuesTuple> tuples;

    tableConsumer.getTableContent(tuples);
    for (auto tuple : tuples)
    {
        string key = kfvKey(tuple);

        /* Record incoming tasks, for init op is empty in the record */
        if (gSwssCfgRecord)
        {
            recordTuple(consumer, tuple);
        }

        /* Directly put it into consumer.m_toSync map */
        if (consumer.m_toSync.find(key) == consumer.m_toSync.end())
        {
           consumer.m_toSync[key] = make_tuple(key, SET_COMMAND, kfvFieldsValues(tuple));
        }
        /*
         * Syncing from DB directly, don't expect duplicate keys.
         * Or there is pending task from consumber state pipe, in this case just skip it.
         */
        else
        {
            SWSS_LOG_WARN("Duplicate key %s found in tableName:%s\n", key.c_str(), tableName.c_str());
            continue;
        }
        doTask(consumer);
    }
    return true;
}

bool CfgOrch::execute(string tableName)
{
    SWSS_LOG_ENTER();

    lock_guard<mutex> lock(gDbMutex);

    auto consumer_it = m_consumerMap.find(tableName);
    if (consumer_it == m_consumerMap.end())
    {
        SWSS_LOG_ERROR("Unrecognized tableName:%s\n", tableName.c_str());
        return false;
    }
    Consumer& consumer = consumer_it->second;

    int data_popped = 0;
    while (1)
    {
        KeyOpFieldsValuesTuple new_data;
        consumer.m_consumer->pop(new_data);

        string key = kfvKey(new_data);
        string op  = kfvOp(new_data);
        /*
         * Done with all new data. Or
         * possible nothing popped, ie. the oparation is already merged with other operations
         */
        if (op.empty())
        {
            SWSS_LOG_DEBUG("Number of kfv data popped: %d\n", data_popped);
            break;
        }
        data_popped++;

        /* Record incoming tasks */
        if (gSwssCfgRecord)
        {
            recordTuple(consumer, new_data);
        }

        /* If a new task comes or if a DEL task comes, we directly put it into consumer.m_toSync map */
        if (consumer.m_toSync.find(key) == consumer.m_toSync.end() || op == DEL_COMMAND)
        {
           consumer.m_toSync[key] = new_data;
        }
        /* If an old task is still there, we combine the old task with new task */
        else
        {
            KeyOpFieldsValuesTuple existing_data = consumer.m_toSync[key];

            auto new_values = kfvFieldsValues(new_data);
            auto existing_values = kfvFieldsValues(existing_data);


            for (auto it : new_values)
            {
                string field = fvField(it);
                string value = fvValue(it);

                auto iu = existing_values.begin();
                while (iu != existing_values.end())
                {
                    string ofield = fvField(*iu);
                    if (field == ofield)
                        iu = existing_values.erase(iu);
                    else
                        iu++;
                }
                existing_values.push_back(FieldValueTuple(field, value));
            }
            consumer.m_toSync[key] = KeyOpFieldsValuesTuple(key, op, existing_values);
        }
    }
    if (!consumer.m_toSync.empty())
        doTask(consumer);

    return true;
}

void CfgOrch::doTask()
{
    if (!gInitDone)
        return;

    for(auto &it : m_consumerMap)
    {
        if (!it.second.m_toSync.empty())
            doTask(it.second);
    }
}

void CfgOrch::recordTuple(Consumer &consumer, KeyOpFieldsValuesTuple &tuple)
{
    string s = consumer.m_consumer->getTableName() + ":" + kfvKey(tuple)
               + "|" + kfvOp(tuple);
    for (auto i = kfvFieldsValues(tuple).begin(); i != kfvFieldsValues(tuple).end(); i++)
    {
        s += "|" + fvField(*i) + ":" + fvValue(*i);
    }

    gCfgRecordOfs << getTimestamp() << "|" << s << endl;
}

string CfgOrch::dumpTuple(Consumer &consumer, KeyOpFieldsValuesTuple &tuple)
{
    string s = consumer.m_consumer->getTableName() + ":" + kfvKey(tuple)
               + "|" + kfvOp(tuple);
    for (auto i = kfvFieldsValues(tuple).begin(); i != kfvFieldsValues(tuple).end(); i++)
    {
        s += "|" + fvField(*i) + ":" + fvValue(*i);
    }

    return s;
}
