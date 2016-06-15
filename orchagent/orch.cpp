#include "orch.h"
#include "logger.h"
#include <sstream>
#include <iostream>
using namespace swss;

Orch::Orch(DBConnector *db, string tableName) :
    m_db(db)
{
    Consumer consumer(new ConsumerTable(m_db, tableName));
    m_consumerMap.insert(ConsumerMapPair(tableName, consumer));
}

Orch::Orch(DBConnector *db, vector<string> &tableNames) :
    m_db(db)
{
    for( auto it = tableNames.begin(); it != tableNames.end(); it++) {
        Consumer consumer(new ConsumerTable(m_db, *it));
        m_consumerMap.insert(ConsumerMapPair(*it, consumer));
    }
}

Orch::~Orch()
{
    delete(m_db);
    for(auto it : m_consumerMap) {
        delete it.second.m_consumer;
    }
}

std::vector<Selectable *> Orch::getSelectables()
{
    std::vector<Selectable *> selectables;
    for(auto it : m_consumerMap) {
        selectables.push_back(it.second.m_consumer);
    }
    return selectables;
}

bool Orch::hasSelectable(ConsumerTable *selectable) const
{
    for(auto it : m_consumerMap) {
        if(it.second.m_consumer == selectable) {
            return true;
        }
    }
    return false;
}

bool Orch::execute(string tableName)
{
    SWSS_LOG_ENTER();

    auto consumer_it = m_consumerMap.find(tableName);
    if(consumer_it == m_consumerMap.end()) {
        SWSS_LOG_ERROR("Unrecognized tableName:%s\n", tableName.c_str());
        return false;
    }
    Consumer& consumer = consumer_it->second;

    KeyOpFieldsValuesTuple new_data;
    consumer.m_consumer->pop(new_data);

    string key = kfvKey(new_data);
    string op  = kfvOp(new_data);

#ifdef DEBUG
    string debug = "Table : " + consumer.m_consumer.getTableName() + " key : " + kfvKey(new_data) + " op : "  + kfvOp(new_data);
    for (auto i = kfvFieldsValues(new_data).begin(); i != kfvFieldsValues(new_data).end(); i++)
        debug += " " + fvField(*i) + " : " + fvValue(*i);
    SWSS_LOG_DEBUG("%s\n", debug.c_str());
#endif

    /* If a new task comes or if a DEL task comes, we directly put it into consumer.m_toSync map */
    if ( consumer.m_toSync.find(key) == consumer.m_toSync.end() || op == DEL_COMMAND)
    {
       consumer.m_toSync[key] = new_data;
    }
    /* If an old task is still there, we combine the old task with new task */
    else
    {
        KeyOpFieldsValuesTuple existing_data = consumer.m_toSync[key];

        auto new_values = kfvFieldsValues(new_data);
        auto existing_values = kfvFieldsValues(existing_data);


        for (auto it = new_values.begin(); it != new_values.end(); it++)
        {
            string field = fvField(*it);
            string value = fvValue(*it);

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

    if (!consumer.m_toSync.empty())
        doTask(consumer);

    return true;
}

void Orch::doTask()
{
    for(auto it : m_consumerMap)
    {
        if (!it.second.m_toSync.empty())
            doTask(it.second);
    }
}

bool Orch::tokenizeString(string str, const string &separator, vector<string> &tokens)
{
    SWSS_LOG_ENTER();
    if(0 == separator.size())
    {
        SWSS_LOG_ERROR("Invalid separator passed in:%s\n", separator.c_str());
        return false;
    }
    if(string::npos == str.find(separator))
    {
        SWSS_LOG_DEBUG("Single token:'%s'", str.c_str());
        tokens.push_back(str);
        return true;
    }
    istringstream ss(str);
    string tmp;
    while (getline(ss, tmp, separator[0]))
    {
        SWSS_LOG_DEBUG("extracted token:%s", tmp.c_str());
        tokens.push_back(tmp);
    }
    return true;
}
void Orch::dumpTuple(Consumer &consumer, KeyOpFieldsValuesTuple &tuple)
{
    string debug_msg = "Full table content: " + consumer.m_consumer->getTableName() + " key : " + kfvKey(tuple) + " op : "  + kfvOp(tuple);
    for (auto i = kfvFieldsValues(tuple).begin(); i != kfvFieldsValues(tuple).end(); i++)
    {
        debug_msg += " " + fvField(*i) + " : " + fvValue(*i);
    }
    SWSS_LOG_DEBUG("%s\n", debug_msg.c_str());
}