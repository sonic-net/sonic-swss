#include <fstream>
#include <iostream>
#include <mutex>
#include <sys/time.h>

#include "orchbase.h"
#include "subscriberstatetable.h"
#include "tokenize.h"
#include "logger.h"
#include "consumerstatetable.h"

using namespace swss;

OrchBase::OrchBase(DBConnector *db, string tableName)
{
    addConsumer(db, tableName);
}

OrchBase::OrchBase(DBConnector *db, const vector<string> &tableNames)
{
    for(auto it : tableNames)
    {
        addConsumer(db, it);
    }
}

OrchBase::OrchBase(const vector<TableConnector>& tables)
{
    for (auto it : tables)
    {
        addConsumer(it.first, it.second);
    }
}

OrchBase::~OrchBase()
{
    for(auto &it : m_consumerMap)
        delete it.second.m_consumer;
}

vector<Selectable *> OrchBase::getSelectables()
{
    vector<Selectable *> selectables;
    for(auto it : m_consumerMap) {
        selectables.push_back(it.second.m_consumer);
    }
    return selectables;
}

bool OrchBase::hasSelectable(TableConsumable *selectable) const
{
    for(auto it : m_consumerMap) {
        if (it.second.m_consumer == selectable) {
            return true;
        }
    }
    return false;
}

bool OrchBase::syncDB(string tableName, Table &tableConsumer)
{
    SWSS_LOG_ENTER();

    auto consumer_it = m_consumerMap.find(tableName);
    if (consumer_it == m_consumerMap.end())
    {
        SWSS_LOG_ERROR("Unrecognized tableName:%s\n", tableName.c_str());
        return false;
    }
    Consumer& consumer = consumer_it->second;

    vector<KeyOpFieldsValuesTuple> tuples;

    tableConsumer.getTableContent(tuples);
    for (auto tuple : tuples)
    {
        string key = kfvKey(tuple);
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
        SWSS_LOG_DEBUG("%s", (dumpTuple(consumer, tuple)).c_str());
        doTask(consumer);
    }
    return true;
}

bool OrchBase::execute(string tableName)
{
    SWSS_LOG_ENTER();

    auto consumer_it = m_consumerMap.find(tableName);
    if (consumer_it == m_consumerMap.end())
    {
        SWSS_LOG_ERROR("Unrecognized tableName:%s\n", tableName.c_str());
        return false;
    }
    Consumer& consumer = consumer_it->second;

    std::deque<KeyOpFieldsValuesTuple> entries;
    consumer.m_consumer->pops(entries);

    /* Nothing popped */
    if (entries.empty())
    {
        return true;
    }

    for (auto entry: entries)
    {
        string key = kfvKey(entry);
        string op  = kfvOp(entry);

        recordTuple(consumer, entry);

        /* If a new task comes or if a DEL task comes, we directly put it into consumer.m_toSync map */
        if (consumer.m_toSync.find(key) == consumer.m_toSync.end() || op == DEL_COMMAND)
        {
           consumer.m_toSync[key] = entry;
        }
        /* If an old task is still there, we combine the old task with new task */
        else
        {
            KeyOpFieldsValuesTuple existing_data = consumer.m_toSync[key];

            auto new_values = kfvFieldsValues(entry);
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

void OrchBase::recordTuple(Consumer &consumer, KeyOpFieldsValuesTuple &tuple)
{
    return;
}

void OrchBase::doTask()
{
    for(auto &it : m_consumerMap)
    {
        if (!it.second.m_toSync.empty())
            doTask(it.second);
    }
}

string OrchBase::dumpTuple(Consumer &consumer, KeyOpFieldsValuesTuple &tuple)
{
    string s = consumer.m_consumer->getTableName() + ":" + kfvKey(tuple)
               + "|" + kfvOp(tuple);
    for (auto i = kfvFieldsValues(tuple).begin(); i != kfvFieldsValues(tuple).end(); i++)
    {
        s += "|" + fvField(*i) + ":" + fvValue(*i);
    }

    return s;
}

void OrchBase::addConsumer(DBConnector *db, string tableName, int batchSize)
{
    if (db->getDB() == CONFIG_DB)
    {
        Consumer consumer(new SubscriberStateTable(db, tableName));
        m_consumerMap.insert(ConsumerMapPair(tableName, consumer));
    } else {
        Consumer consumer(new ConsumerStateTable(db, tableName, batchSize));
        m_consumerMap.insert(ConsumerMapPair(tableName, consumer));
    }
}
