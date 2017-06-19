#include <fstream>
#include <iostream>
#include <mutex>
#include <sys/time.h>

#include "orch.h"
#include "portsorch.h"
#include "tokenize.h"
#include "logger.h"

using namespace swss;

extern int gBatchSize;

extern mutex gDbMutex;
extern PortsOrch *gPortsOrch;

extern bool gSwssRecord;
extern ofstream gRecordOfs;
extern bool gLogRotate;
extern string gRecordFile;
extern string getTimestamp();

Orch::Orch(DBConnector *db, string tableName) :
    m_db(db)
{
    Consumer consumer(new ConsumerStateTable(m_db, tableName, gBatchSize));
    m_consumerMap.insert(ConsumerMapPair(tableName, consumer));
}

Orch::Orch(DBConnector *db, vector<string> &tableNames) :
    m_db(db)
{
    for(auto it : tableNames)
    {
        Consumer consumer(new ConsumerStateTable(m_db, it, gBatchSize));
        m_consumerMap.insert(ConsumerMapPair(it, consumer));
    }
}

Orch::~Orch()
{
    delete(m_db);
    for(auto &it : m_consumerMap)
        delete it.second.m_consumer;

    if (gRecordOfs.is_open())
    {
        gRecordOfs.close();
    }
}

vector<Selectable *> Orch::getSelectables()
{
    vector<Selectable *> selectables;
    for(auto it : m_consumerMap) {
        selectables.push_back(it.second.m_consumer);
    }
    return selectables;
}

bool Orch::hasSelectable(ConsumerStateTable *selectable) const
{
    for(auto it : m_consumerMap) {
        if (it.second.m_consumer == selectable) {
            return true;
        }
    }
    return false;
}

bool Orch::execute(string tableName)
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

    KeyOpFieldsValuesTuple new_data;
    consumer.m_consumer->pop(new_data);

    string key = kfvKey(new_data);
    string op  = kfvOp(new_data);
    /* Possible nothing popped, ie. the oparation is already merged with other operations */
    if (op.empty())
    {
        return true;
    }

    /* Record incoming tasks */
    if (gSwssRecord)
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

    if (!consumer.m_toSync.empty())
        doTask(consumer);

    return true;
}

/*
- Validates reference has proper format which is [table_name:object_name]
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
    tokens = tokenize(ref_content, delimiter);
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
        SWSS_LOG_INFO("map:%s does not contain object with name:%s\n", tokens[0].c_str(), tokens[1].c_str());
        return false;
    }
    type_name = tokens[0];
    object_name = tokens[1];
    SWSS_LOG_DEBUG("parsed: type_name:%s, object_name:%s", type_name.c_str(), object_name.c_str());
    return true;
}

ref_resolve_status Orch::resolveFieldRefValue(
    type_map &type_maps,
    const string &field_name,
    KeyOpFieldsValuesTuple &tuple,
    sai_object_id_t &sai_object)
{
    SWSS_LOG_ENTER();

    bool hit = false;
    for (auto i = kfvFieldsValues(tuple).begin(); i != kfvFieldsValues(tuple).end(); i++)
    {
        SWSS_LOG_DEBUG("field:%s, value:%s", fvField(*i).c_str(), fvValue(*i).c_str());
        if (fvField(*i) == field_name)
        {
            if (hit)
            {
                SWSS_LOG_ERROR("Multiple same fields %s", field_name.c_str());
                return ref_resolve_status::multiple_instances;
            }
            string ref_type_name, object_name;
            if (!parseReference(type_maps, fvValue(*i), ref_type_name, object_name))
            {
                return ref_resolve_status::not_resolved;
            }
            sai_object = (*(type_maps[ref_type_name]))[object_name];
            hit = true;
        }
    }
    if (!hit)
    {
        return ref_resolve_status::field_not_found;
    }
    return ref_resolve_status::success;
}

void Orch::doTask()
{
    if (!gPortsOrch->isInitDone())
        return;

    for(auto &it : m_consumerMap)
    {
        if (!it.second.m_toSync.empty())
            doTask(it.second);
    }
}

void Orch::logfileReopen()
{
    gRecordOfs.close();

    /*
     * On log rotate we will use the same file name, we are assuming that
     * logrotate deamon move filename to filename.1 and we will create new
     * empty file here.
     */

    gRecordOfs.open(gRecordFile);

    if (!gRecordOfs.is_open())
    {
        SWSS_LOG_ERROR("failed to open gRecordOfs file %s: %s", gRecordFile.c_str(), strerror(errno));
        return;
    }
}

void Orch::recordTuple(Consumer &consumer, KeyOpFieldsValuesTuple &tuple)
{
    string s = consumer.m_consumer->getTableName() + ":" + kfvKey(tuple)
               + "|" + kfvOp(tuple);
    for (auto i = kfvFieldsValues(tuple).begin(); i != kfvFieldsValues(tuple).end(); i++)
    {
        s += "|" + fvField(*i) + ":" + fvValue(*i);
    }

    gRecordOfs << getTimestamp() << "|" << s << endl;

    if (gLogRotate)
    {
        gLogRotate = false;

        logfileReopen();

        /* double check since reopen could fail */

        if (gRecordOfs.is_open())
        {
            gRecordOfs << getTimestamp() << "|" << "#|logrotate on: " << gRecordFile << std::endl;
        }
    }
}

ref_resolve_status Orch::resolveFieldRefArray(
    type_map &type_maps,
    const string &field_name,
    KeyOpFieldsValuesTuple &tuple,
    vector<sai_object_id_t> &sai_object_arr)
{
    // example: [BUFFER_PROFILE_TABLE:e_port.profile0],[BUFFER_PROFILE_TABLE:e_port.profile1]
    SWSS_LOG_ENTER();
    size_t count = 0;
    sai_object_arr.clear();
    for (auto i = kfvFieldsValues(tuple).begin(); i != kfvFieldsValues(tuple).end(); i++)
    {
        if (fvField(*i) == field_name)
        {
            if (count > 1)
            {
                SWSS_LOG_ERROR("Singleton field with name:%s must have only 1 instance, actual count:%zd\n", field_name.c_str(), count);
                return ref_resolve_status::multiple_instances;
            }
            string ref_type_name, object_name;
            string list = fvValue(*i);
            vector<string> list_items;
            if (list.find(list_item_delimiter) != string::npos)
            {
                list_items = tokenize(list, list_item_delimiter);
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
                    return ref_resolve_status::not_resolved;
                }
                sai_object_id_t sai_obj = (*(type_maps[ref_type_name]))[object_name];
                SWSS_LOG_DEBUG("Resolved to sai_object:0x%lx, type:%s, name:%s", sai_obj, ref_type_name.c_str(), object_name.c_str());
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

bool Orch::parseIndexRange(const string &input, sai_uint32_t &range_low, sai_uint32_t &range_high)
{
    SWSS_LOG_ENTER();
    SWSS_LOG_DEBUG("input:%s", input.c_str());
    if (input.find(range_specifier) != string::npos)
    {
        vector<string> range_values;
        range_values = tokenize(input, range_specifier);
        if (range_values.size() != 2)
        {
            SWSS_LOG_ERROR("malformed index range in:%s. Must contain 2 tokens\n", input.c_str());
            return false;
        }
        range_low = stoul(range_values[0]);
        range_high = stoul(range_values[1]);
        if (range_low >= range_high)
        {
            SWSS_LOG_ERROR("malformed index range in:%s. left value must be less than righ value.\n", input.c_str());
            return false;
        }
    }
    else
    {
        range_low = range_high = stoul(input);
    }
    SWSS_LOG_DEBUG("resulting range:%d-%d", range_low, range_high);
    return true;
}
