#include <fstream>
#include <iostream>
#include <mutex>
#include <sys/time.h>
#include "timestamp.h"
#include "orch.h"

#include "subscriberstatetable.h"
#include "portsorch.h"
#include "tokenize.h"
#include "logger.h"
#include "consumerstatetable.h"

using namespace swss;

extern int gBatchSize;

extern mutex gDbMutex;

extern bool gSwssRecord;
extern ofstream gRecordOfs;
extern bool gLogRotate;
extern string gRecordFile;

Orch::Orch(DBConnector *db, const string tableName, int pri)
{
    addConsumer(db, tableName, pri);
}

Orch::Orch(DBConnector *db, const vector<string> &tableNames)
{
    for(auto it : tableNames)
    {
        addConsumer(db, it, default_orch_pri);
    }
}

Orch::Orch(DBConnector *db, const vector<table_name_with_pri_t> &tableNames_with_pri)
{
    for(const auto& it : tableNames_with_pri)
    {
        addConsumer(db, it.first, it.second);
    }
}

Orch::Orch(const vector<TableConnector>& tables)
{
    for (auto it : tables)
    {
        addConsumer(it.first, it.second);
    }
}

Orch::~Orch()
{
    if (gRecordOfs.is_open())
    {
        gRecordOfs.close();
    }
}

vector<Selectable *> Orch::getSelectables()
{
    vector<Selectable *> selectables;
    for(auto& it : m_consumerMap)
    {
        selectables.push_back(it.second.get());
    }
    return selectables;
}

void Consumer::execute()
{
    SWSS_LOG_ENTER();

    // TODO: remove DbMutex when there is only single thread
    lock_guard<mutex> lock(gDbMutex);

    std::deque<KeyOpFieldsValuesTuple> entries;
    getConsumerTable()->pops(entries);

    /* Nothing popped */
    if (entries.empty())
    {
        return;
    }

    for (auto& entry: entries)
    {
        string key = kfvKey(entry);
        string op  = kfvOp(entry);

        /* Record incoming tasks */
        if (gSwssRecord)
        {
            Orch::recordTuple(*this, entry);
        }

        /* If a new task comes or if a DEL task comes, we directly put it into getConsumerTable().m_toSync map */
        if (m_toSync.find(key) == m_toSync.end() || op == DEL_COMMAND)
        {
           m_toSync[key] = entry;
        }
        /* If an old task is still there, we combine the old task with new task */
        else
        {
            KeyOpFieldsValuesTuple existing_data = m_toSync[key];

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
            m_toSync[key] = KeyOpFieldsValuesTuple(key, op, existing_values);
        }
    }

    drain();
}

void Consumer::drain()
{
    if (!m_toSync.empty())
        m_orch->doTask(*this);
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
        tokens = tokenize(ref_content, config_db_key_delimiter);
        if (tokens.size() != 2)
        {
            SWSS_LOG_ERROR("malformed reference:%s. Must contain 2 tokens\n", ref_content.c_str());
            return false;
        }
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
    for(auto &it : m_consumerMap)
    {
        it.second->drain();
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
    string s = consumer.getTableName() + ":" + kfvKey(tuple)
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
    }
}

string Orch::dumpTuple(Consumer &consumer, KeyOpFieldsValuesTuple &tuple)
{
    string s = consumer.getTableName() + ":" + kfvKey(tuple)
               + "|" + kfvOp(tuple);
    for (auto i = kfvFieldsValues(tuple).begin(); i != kfvFieldsValues(tuple).end(); i++)
    {
        s += "|" + fvField(*i) + ":" + fvValue(*i);
    }

    return s;
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
        range_low = (uint32_t)stoul(range_values[0]);
        range_high = (uint32_t)stoul(range_values[1]);
        if (range_low >= range_high)
        {
            SWSS_LOG_ERROR("malformed index range in:%s. left value must be less than righ value.\n", input.c_str());
            return false;
        }
    }
    else
    {
        range_low = range_high = (uint32_t)stoul(input);
    }
    SWSS_LOG_DEBUG("resulting range:%d-%d", range_low, range_high);
    return true;
}

void Orch::addConsumer(DBConnector *db, string tableName, int pri)
{
    if (db->getDbId() == CONFIG_DB)
    {
        addExecutor(tableName, new Consumer(new SubscriberStateTable(db, tableName, TableConsumable::DEFAULT_POP_BATCH_SIZE, pri), this));
    }
    else
    {
        addExecutor(tableName, new Consumer(new ConsumerStateTable(db, tableName, gBatchSize, pri), this));
    }
}

void Orch::addExecutor(string executorName, Executor* executor)
{
    m_consumerMap.emplace(std::piecewise_construct,
            std::forward_as_tuple(executorName),
            std::forward_as_tuple(executor));
}

Executor *Orch::getExecutor(string executorName)
{
    auto it = m_consumerMap.find(executorName);
    if (it != m_consumerMap.end())
    {
        return it->second.get();
    }

    return NULL;
}

void Orch2::doTask(Consumer &consumer)
{
    SWSS_LOG_ENTER();

    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
        bool erase_from_queue = true;
        try
        {
            request_.parse(it->second);

            auto op = request_.getOperation();
            if (op == SET_COMMAND)
            {
                erase_from_queue = addOperation(request_);
            }
            else if (op == DEL_COMMAND)
            {
                erase_from_queue = delOperation(request_);
            }
            else
            {
                SWSS_LOG_ERROR("Wrong operation. Check RequestParser: %s", op.c_str());
            }
        }
        catch (const std::invalid_argument& e)
        {
            SWSS_LOG_ERROR("Parse error: %s", e.what());
        }
        catch (const std::logic_error& e)
        {
            SWSS_LOG_ERROR("Logic error: %s", e.what());
        }
        catch (const std::exception& e)
        {
            SWSS_LOG_ERROR("Exception was catched in the request parser: %s", e.what());
        }
        catch (...)
        {
            SWSS_LOG_ERROR("Unknown exception was catched in the request parser");
        }
        request_.clear();

        if (erase_from_queue)
        {
            it = consumer.m_toSync.erase(it);
        }
        else
        {
            ++it;
        }
    }
}

