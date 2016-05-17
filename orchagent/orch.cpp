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
    for ( auto it = tableNames.begin(); it != tableNames.end(); it++)
    {
        Consumer consumer(new ConsumerTable(m_db, *it));
        m_consumerMap.insert(ConsumerMapPair(*it, consumer));
    }
}

Orch::~Orch()
{
    delete(m_db);
    for (auto it : m_consumerMap)
    {
        delete it.second.m_consumer;
    }
}

std::vector<Selectable*> Orch::getConsumers()
{
    SWSS_LOG_ENTER();

    std::vector<Selectable*> consumers;
    for (auto it : m_consumerMap)
    {
        consumers.push_back(it.second.m_consumer);
    }
    return consumers;
}

bool Orch::hasConsumer(ConsumerTable *consumer) const
{
    for (auto it : m_consumerMap)
    {
        if (it.second.m_consumer == consumer)
        {
            return true;
        }
    }
    return false;
}

bool Orch::execute(string tableName)
{
    auto consumer_it = m_consumerMap.find(tableName);
    if (consumer_it == m_consumerMap.end())
    {
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

    doTask(consumer);
    return true;
}
bool Orch::tokenizeString(string str, const string &separator, vector<string> &tokens)
{
    SWSS_LOG_ENTER();
    if (0 == separator.size())
    {
        SWSS_LOG_ERROR("Invalid separator passed in:%s\n", separator.c_str());
        return false;
    }
    if (string::npos == str.find(separator))
    {
        SWSS_LOG_ERROR("Specified separator:%s not found in input:%s\n", separator.c_str(), str.c_str());
        return false;
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

/*
- Validates reference is has proper format which is [table_name:object_name]
- validates table_name exists
- validates object with object_name exists
*/
bool Orch::parseReference(type_map &type_maps, string &ref_in, string &type_name, string &object_name)
{
    SWSS_LOG_ENTER();
    SWSS_LOG_DEBUG("input:%s", ref_in.c_str());
    if (ref_in.size() < 3)
    {
        SWSS_LOG_ERROR("invalid reference received:%s\n", ref_in.c_str());
        return false;
    }
    if ((ref_in[0] != ref_start) && (ref_in[ref_in.size()-1] != ref_end))
    {
        SWSS_LOG_ERROR("malformed reference:%s. Must be surrounded by [ ]\n", ref_in.c_str());
        return false;
    }
    string ref_content = ref_in.substr(1, ref_in.size() - 2);
    vector<string> tokens;
    if (!tokenizeString(ref_content, delimiter, tokens))
    {
        return false;
    }
    if (tokens.size() != 2)
    {
        SWSS_LOG_ERROR("malformed reference:%s. Must contain 2 tokens\n", ref_content.c_str());
        return false;
    }
    auto type_it = type_maps.find(tokens[0]);
    if (type_it == type_maps.end())
    {
        SWSS_LOG_ERROR("not recognized type:%s\n", tokens[0].c_str());
        return false;
    }
    auto obj_map = type_maps[tokens[0]];
    auto obj_it = obj_map->find(tokens[1]);
    if (obj_it == obj_map->end())
    {
        SWSS_LOG_ERROR("map:%s does not contain object with name:%s\n", tokens[0].c_str(), tokens[1].c_str());
        return false;
    }
    type_name   = tokens[0];
    object_name = tokens[1];
    SWSS_LOG_DEBUG("parsed: type_name:%s, object_name:%s", type_name.c_str(), object_name.c_str());
    return true;
}

ref_resolve_status Orch::resolveFieldRefValue(
    type_map                &type_maps,
    const string            &field_name,
    KeyOpFieldsValuesTuple  &tuple, 
    sai_object_id_t         &sai_object)
{
    SWSS_LOG_ENTER();
    size_t count = 0;
    for (auto i = kfvFieldsValues(tuple).begin(); i != kfvFieldsValues(tuple).end(); i++)
    {
        if (fvField(*i) == field_name)
        {
            SWSS_LOG_DEBUG("field:%s, value:%s", fvField(*i).c_str(), fvValue(*i).c_str());
            if (count > 1)
            {
                SWSS_LOG_ERROR("Singleton field with name:%s must have only 1 instance, actual count:%d\n", field_name.c_str(), count);
                return ref_resolve_status::multiple_instances;
            }
            string ref_type_name, object_name;
            if (!parseReference(type_maps, fvValue(*i), ref_type_name, object_name))
            {
                return ref_resolve_status::failure;
            }
            sai_object = (*(type_maps[ref_type_name]))[object_name];
            count++;
        }
    }
    if (0 == count)
    {
        SWSS_LOG_NOTICE("field with name:%s not found\n", field_name.c_str());
        return ref_resolve_status::field_not_found;
    }
    return ref_resolve_status::success;
}

// example: [BUFFER_PROFILE_TABLE:e_port.profile0],[BUFFER_PROFILE_TABLE:e_port.profile1]
bool Orch::resolveFieldRefArray(
    type_map                    &type_maps,
    const string                &field_name,
    KeyOpFieldsValuesTuple      &tuple, 
    vector<sai_object_id_t>     &sai_object_arr)
{
    SWSS_LOG_ENTER();
    size_t count = 0;
    sai_object_arr.clear();
    for (auto i = kfvFieldsValues(tuple).begin(); i != kfvFieldsValues(tuple).end(); i++)
    {
        if (fvField(*i) == field_name)
        {
            if (count > 1)
            {
                SWSS_LOG_ERROR("Singleton field with name:%s must have only 1 instance, actual count:%d\n", field_name.c_str(), count);
                return false;
            }
            string ref_type_name, object_name;
            string list = fvValue(*i);            
            vector<string> list_items;
            if (list.find(list_item_delimiter) != string::npos)
            {
                if (!tokenizeString(list, list_item_delimiter, list_items))
                {
                    SWSS_LOG_ERROR("Failed to tokenize buffer profile list:%s\n", list.c_str());
                    return false;
                }
            }
            else
            {
                list_items.push_back(list);
            }
            for (size_t ind = 0; ind < list_items.size(); ind++)
            {
                if (!parseReference(type_maps, list_items[ind], ref_type_name, object_name))
                {
                    SWSS_LOG_ERROR("Failed to parse profile reference:%s\n", list_items[ind].c_str());
                    return false;
                }
                sai_object_id_t sai_obj = (*(type_maps[ref_type_name]))[object_name];
                SWSS_LOG_DEBUG("Resolved to sai_object:0x%llx, type:%s, name:%s", sai_obj, ref_type_name.c_str(), object_name.c_str());
                sai_object_arr.push_back(sai_obj);
            }
            count++;
        }
    }
    if (0 == count)
    {
        SWSS_LOG_NOTICE("field with name:%s not found\n", field_name.c_str());
        return ref_resolve_status::field_not_found;
    }
    return ref_resolve_status::success;
}

