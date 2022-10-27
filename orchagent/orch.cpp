#include <fstream>
#include <iostream>
#include <inttypes.h>
#include <sstream>
#include <stdexcept>
#include <sys/time.h>
#include "timestamp.h"
#include "orch.h"

#include "subscriberstatetable.h"
#include "decoratorsubscriberstatetable.h"
#include "portsorch.h"
#include "tokenize.h"
#include "logger.h"
#include "consumerstatetable.h"
#include "sai_serialize.h"
#include "schema.h"

using namespace swss;

extern int gBatchSize;

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
    for (auto it : tableNames)
    {
        addConsumer(db, it, default_orch_pri);
    }
}

Orch::Orch(DBConnector *db, const vector<table_name_with_pri_t> &tableNames_with_pri)
{
    for (const auto& it : tableNames_with_pri)
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
    for (auto& it : m_consumerMap)
    {
        selectables.push_back(it.second.get());
    }
    return selectables;
}

void Consumer::addToSync(const KeyOpFieldsValuesTuple &entry)
{
    SWSS_LOG_ENTER();


    string key = kfvKey(entry);
    string op  = kfvOp(entry);

    /* Record incoming tasks */
    if (gSwssRecord)
    {
        Orch::recordTuple(*this, entry);
    }

    /*
    * m_toSync is a multimap which will allow one key with multiple values,
    * Also, the order of the key-value pairs whose keys compare equivalent
    * is the order of insertion and does not change. (since C++11)
    */

    /* If a new task comes we directly put it into getConsumerTable().m_toSync map */
    if (m_toSync.find(key) == m_toSync.end())
    {
        m_toSync.emplace(key, entry);
    }

    /* if a DEL task comes, we overwrite the old key */
    else if (op == DEL_COMMAND)
    {
        m_toSync.erase(key);
        m_toSync.emplace(key, entry);
    }
    else
    {
        /*
        * Now we are trying to add the key-value with SET.
        * We maintain maximum two values per key.
        * In case there is one key-value, it should be DEL or SET
        * In case there are two key-value pairs, it should be DEL then SET
        * The code logic is following:
        * We iterate the values with the key, we skip the value with DEL and then
        * check if that was the only one (I,E, the iter pointer now points to end or next key),
        * in such case, we insert the key-value with SET.
        * If there was a SET already (I,E, the pointer still points to the same key), we combine the kfv.
        */
        auto ret = m_toSync.equal_range(key);
        auto iter = ret.first;
        for (; iter != ret.second; ++iter)
        {
            auto old_op = kfvOp(iter->second);
            if (old_op == SET_COMMAND)
                break;
        }
        if (iter == ret.second)
        {
            m_toSync.emplace(key, entry);
        }
        else
        {
            KeyOpFieldsValuesTuple existing_data = iter->second;

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
            iter->second = KeyOpFieldsValuesTuple(key, op, existing_values);
        }
    }

}

size_t Consumer::addToSync(const std::deque<KeyOpFieldsValuesTuple> &entries)
{
    SWSS_LOG_ENTER();

    for (auto& entry: entries)
    {
        addToSync(entry);
    }

    return entries.size();
}

// TODO: Table should be const
size_t Consumer::refillToSync(Table* table)
{
    std::deque<KeyOpFieldsValuesTuple> entries;
    vector<string> keys;
    table->getKeys(keys);
    for (const auto &key: keys)
    {
        KeyOpFieldsValuesTuple kco;

        kfvKey(kco) = key;
        kfvOp(kco) = SET_COMMAND;

        if (!table->get(key, kfvFieldsValues(kco)))
        {
            continue;
        }
        entries.push_back(kco);
    }

    return addToSync(entries);
}

size_t Consumer::refillToSync()
{
    ConsumerTableBase *consumerTable = getConsumerTable();

    auto subTable = dynamic_cast<SubscriberStateTable *>(consumerTable);
    if (subTable != NULL)
    {
        size_t update_size = 0;
        size_t total_size = 0;
        do
        {
            std::deque<KeyOpFieldsValuesTuple> entries;
            subTable->pops(entries);
            update_size = addToSync(entries);
            total_size += update_size;
        } while (update_size != 0);
        return total_size;
    }
    else
    {
        // consumerTable is either ConsumerStateTable or ConsumerTable
        auto db = consumerTable->getDbConnector();
        string tableName = consumerTable->getTableName();
        auto table = Table(db, tableName);
        return refillToSync(&table);
    }
}

void Consumer::execute()
{
    SWSS_LOG_ENTER();

    size_t update_size = 0;
    do
    {
        std::deque<KeyOpFieldsValuesTuple> entries;
        getConsumerTable()->pops(entries);
        update_size = addToSync(entries);
    } while (update_size != 0);

    drain();
}

void Consumer::drain()
{
    if (!m_toSync.empty())
        m_orch->doTask(*this);
}

string Consumer::dumpTuple(const KeyOpFieldsValuesTuple &tuple)
{
    string s = getTableName() + getConsumerTable()->getTableNameSeparator() + kfvKey(tuple)
               + "|" + kfvOp(tuple);
    for (auto i = kfvFieldsValues(tuple).begin(); i != kfvFieldsValues(tuple).end(); i++)
    {
        s += "|" + fvField(*i) + ":" + fvValue(*i);
    }

    return s;
}

void Consumer::dumpPendingTasks(vector<string> &ts)
{
    for (auto &tm : m_toSync)
    {
        KeyOpFieldsValuesTuple& tuple = tm.second;

        string s = dumpTuple(tuple);

        ts.push_back(s);
    }
}

size_t Orch::addExistingData(const string& tableName)
{
    auto consumer = dynamic_cast<Consumer *>(getExecutor(tableName));
    if (consumer == NULL)
    {
        SWSS_LOG_ERROR("No consumer %s in Orch", tableName.c_str());
        return 0;
    }

    return consumer->refillToSync();
}

// TODO: Table should be const
size_t Orch::addExistingData(Table *table)
{
    string tableName = table->getTableName();
    Consumer* consumer = dynamic_cast<Consumer *>(getExecutor(tableName));
    if (consumer == NULL)
    {
        SWSS_LOG_ERROR("No consumer %s in Orch", tableName.c_str());
        return 0;
    }

    return consumer->refillToSync(table);
}

bool Orch::bake()
{
    SWSS_LOG_ENTER();

    for (auto &it : m_consumerMap)
    {
        string executorName = it.first;
        auto executor = it.second;
        auto consumer = dynamic_cast<Consumer *>(executor.get());
        if (consumer == NULL)
        {
            continue;
        }

        size_t refilled = consumer->refillToSync();
        SWSS_LOG_NOTICE("Add warm input: %s, %zd", executorName.c_str(), refilled);
    }

    return true;
}

/*
- Validates reference has proper format which is object_name
- validates table_name exists
- validates object with object_name exists

- Special case:
- Deem reference format [] as valid, and return true. But in such a case,
- both type_name and object_name are cleared to empty strings as an
- indication to the caller of the special case
*/
bool Orch::parseReference(type_map &type_maps, string &ref_in, const string &type_name, string &object_name)
{
    SWSS_LOG_ENTER();

    SWSS_LOG_DEBUG("input:%s", ref_in.c_str());

    if (ref_in.size() == 0)
    {
        // value set by user is ""
        // Deem it as a valid format
        // clear both type_name and object_name
        // as an indication to the caller that
        // such a case has been encountered
        object_name.clear();
        return true;
    }

    if ((ref_in[0] == ref_start) || (ref_in[ref_in.size()-1] == ref_end))
    {
        SWSS_LOG_ERROR("malformed reference:%s. Must not be surrounded by [ ]\n", ref_in.c_str());
        return false;
    }
    auto type_it = type_maps.find(type_name);
    if (type_it == type_maps.end())
    {
        SWSS_LOG_ERROR("not recognized type:%s\n", type_name.c_str());
        return false;
    }
    auto obj_map = type_maps[type_name];
    auto obj_it = obj_map->find(ref_in);
    if (obj_it == obj_map->end())
    {
        SWSS_LOG_INFO("map:%s does not contain object with name:%s\n", type_name.c_str(), ref_in.c_str());
        return false;
    }
    if (obj_it->second.m_pendingRemove)
    {
        SWSS_LOG_NOTICE("map:%s contains a pending removed object %s, skip\n", type_name.c_str(), ref_in.c_str());
        return false;
    }
    object_name = ref_in;
    SWSS_LOG_DEBUG("parsed: type_name:%s, object_name:%s", type_name.c_str(), object_name.c_str());
    return true;
}

ref_resolve_status Orch::resolveFieldRefValue(
    type_map &type_maps,
    const string &field_name,
    const string &ref_type_name,
    KeyOpFieldsValuesTuple &tuple,
    sai_object_id_t &sai_object,
    string &referenced_object_name)
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
            string object_name;
            if (!parseReference(type_maps, fvValue(*i), ref_type_name, object_name))
            {
                return ref_resolve_status::not_resolved;
            }
            else if (object_name.empty())
            {
                return ref_resolve_status::empty;
            }
            sai_object = (*(type_maps[ref_type_name]))[object_name].m_saiObjectId;
            referenced_object_name = ref_type_name + delimiter + object_name;
            hit = true;
        }
    }
    if (!hit)
    {
        return ref_resolve_status::field_not_found;
    }

    return ref_resolve_status::success;
}

void Orch::removeMeFromObjsReferencedByMe(
    type_map &type_maps,
    const string &table,
    const string &obj_name,
    const string &field,
    const string &old_referenced_obj_name,
    bool remove_field)
{
    vector<string> objects = tokenize(old_referenced_obj_name, list_item_delimiter);
    for (auto &obj : objects)
    {
        // obj_name references token
        auto tokens = tokenize(obj, delimiter);
        auto &referenced_table = tokens[0];
        auto &ref_obj_name = tokens[1];
        auto &old_referenced_obj = (*type_maps[referenced_table])[ref_obj_name];
        old_referenced_obj.m_objsDependingOnMe.erase(obj_name);
        SWSS_LOG_INFO("Obj %s.%s Field %s: Remove reference to %s %s (now %s)",
                      table.c_str(), obj_name.c_str(), field.c_str(),
                      referenced_table.c_str(), ref_obj_name.c_str(),
                      to_string(old_referenced_obj.m_objsDependingOnMe.size()).c_str());
    }

    if (remove_field)
    {
        auto &referencing_object = (*type_maps[table])[obj_name];
        referencing_object.m_objsReferencingByMe.erase(field);
    }
}

void Orch::setObjectReference(
    type_map &type_maps,
    const string &table,
    const string &obj_name,
    const string &field,
    const string &referenced_obj)
{
    auto &obj = (*type_maps[table])[obj_name];
    auto field_ref = obj.m_objsReferencingByMe.find(field);

    if (field_ref != obj.m_objsReferencingByMe.end())
        removeMeFromObjsReferencedByMe(type_maps, table, obj_name, field, field_ref->second, false);

    obj.m_objsReferencingByMe[field] = referenced_obj;

    // Add the reference to the new object being referenced
    vector<string> objs = tokenize(referenced_obj, list_item_delimiter);
    for (auto &obj : objs)
    {
        auto tokens = tokenize(obj, delimiter);
        auto &referenced_table = tokens[0];
        auto &referenced_obj_name = tokens[1];
        auto &new_obj_being_referenced = (*type_maps[referenced_table])[referenced_obj_name];
        new_obj_being_referenced.m_objsDependingOnMe.insert(obj_name);
        SWSS_LOG_INFO("Obj %s.%s Field %s: Add reference to %s %s (now %s)",
                      table.c_str(), obj_name.c_str(), field.c_str(),
                      referenced_table.c_str(), referenced_obj_name.c_str(),
                      to_string(new_obj_being_referenced.m_objsDependingOnMe.size()).c_str());
    }
}

bool Orch::doesObjectExist(
    type_map &type_maps,
    const string &table,
    const string &obj_name,
    const string &field,
    string &referenced_obj)
{
    auto &&searchRef = (*type_maps[table]).find(obj_name);
    if (searchRef != (*type_maps[table]).end())
    {
        auto &obj = searchRef->second;
        auto &&searchReferencingObjectRef = obj.m_objsReferencingByMe.find(field);
        if (searchReferencingObjectRef != obj.m_objsReferencingByMe.end())
        {
            referenced_obj = searchReferencingObjectRef->second;
            return true;
        }
    }

    return false;
}

void Orch::removeObject(
    type_map &type_maps,
    const string &table,
    const string &obj_name)
{
    auto &&searchRef = (*type_maps[table]).find(obj_name);
    if (searchRef == (*type_maps[table]).end())
    {
        return;
    }

    auto &obj = searchRef->second;

    for (auto field_ref : obj.m_objsReferencingByMe)
    {
        removeMeFromObjsReferencedByMe(type_maps, table, obj_name, field_ref.first, field_ref.second, false);
    }

    // Update the field store
    (*type_maps[table]).erase(obj_name);
    SWSS_LOG_INFO("Obj %s:%s is removed from store", table.c_str(), obj_name.c_str());
}

bool Orch::isObjectBeingReferenced(
    type_map &type_maps,
    const string &table,
    const string &obj_name)
{
    return !(*type_maps[table])[obj_name].m_objsDependingOnMe.empty();
}

string Orch::objectReferenceInfo(
    type_map &type_maps,
    const string &table,
    const string &obj_name)
{
    auto &objsDependingSet = (*type_maps[table])[obj_name].m_objsDependingOnMe;
    for (auto &depObjName : objsDependingSet)
    {
        string hint = table + " " + obj_name + " one object: " + depObjName;
        hint += " reference count: " + to_string(objsDependingSet.size());
        return hint;
    }
    return "reference count: 0";
}

void Orch::doTask()
{
    for (auto &it : m_consumerMap)
    {
        it.second->drain();
    }
}

void Orch::dumpPendingTasks(vector<string> &ts)
{
    for (auto &it : m_consumerMap)
    {
        Consumer* consumer = dynamic_cast<Consumer *>(it.second.get());
        if (consumer == NULL)
        {
            SWSS_LOG_DEBUG("Executor is not a Consumer");
            continue;
        }

        consumer->dumpPendingTasks(ts);
    }
}

void Orch::logfileReopen()
{
    gRecordOfs.close();

    /*
     * On log rotate we will use the same file name, we are assuming that
     * logrotate daemon move filename to filename.1 and we will create new
     * empty file here.
     */

    gRecordOfs.open(gRecordFile, std::ofstream::out | std::ofstream::app);

    if (!gRecordOfs.is_open())
    {
        SWSS_LOG_ERROR("failed to open gRecordOfs file %s: %s", gRecordFile.c_str(), strerror(errno));
        return;
    }
}

void Orch::recordTuple(Consumer &consumer, const KeyOpFieldsValuesTuple &tuple)
{
    string s = consumer.dumpTuple(tuple);

    gRecordOfs << getTimestamp() << "|" << s << endl;

    if (gLogRotate)
    {
        gLogRotate = false;

        logfileReopen();
    }
}

string Orch::dumpTuple(Consumer &consumer, const KeyOpFieldsValuesTuple &tuple)
{
    string s = consumer.dumpTuple(tuple);
    return s;
}

ref_resolve_status Orch::resolveFieldRefArray(
    type_map &type_maps,
    const string &field_name,
    const string &ref_type_name,
    KeyOpFieldsValuesTuple &tuple,
    vector<sai_object_id_t> &sai_object_arr,
    string &object_name_list)
{
    // example: e_port.profile0,e_port.profile1
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
            string object_name;
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
                    SWSS_LOG_NOTICE("Failed to parse profile reference:%s\n", list_items[ind].c_str());
                    return ref_resolve_status::not_resolved;
                }
                sai_object_id_t sai_obj = (*(type_maps[ref_type_name]))[object_name].m_saiObjectId;
                SWSS_LOG_DEBUG("Resolved to sai_object:0x%" PRIx64 ", type:%s, name:%s", sai_obj, ref_type_name.c_str(), object_name.c_str());
                sai_object_arr.push_back(sai_obj);
                if (!object_name_list.empty())
                    object_name_list += list_item_delimiter;
                object_name_list += ref_type_name + delimiter + object_name;
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
            SWSS_LOG_ERROR("malformed index range in:%s. left value must be less than right value.\n", input.c_str());
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

/*
 * generateBitMapFromIdsStr
 *
 * Generates the bit map representing the idsMap in string
 * Args:
 *      idsStr: The string representing the IDs.
 *              Typically it's part of a key of BUFFER_QUEUE or BUFFER_PG, like "3-4" from "Ethernet0|3-4"
 * Return:
 *      idsMap: The bitmap of IDs. The LSB stands for ID 0.
 *
 * Example:
 *      Input idsMap: 3-4
 *      Return: 00011000b
 */
unsigned long Orch::generateBitMapFromIdsStr(const string &idsStr)
{
    sai_uint32_t lowerBound, upperBound;
    unsigned long idsMap = 0;

    if (!parseIndexRange(idsStr, lowerBound, upperBound))
        return 0;

    for (sai_uint32_t id = lowerBound; id <= upperBound; id ++)
    {
        idsMap |= (1 << id);
    }

    return idsMap;
}

/*
 * generateIdListFromMap
 *
 * Parse the idsMap and generate a vector which contains slices representing bits in idsMap
 * Args:
 *     idsMap: The bitmap of IDs. The LSB stands for ID 0.
 *     maxId: The (exclusive) upperbound of ID range.
 *            Eg: if ID range is 0~7, maxId should be 8.
 * Return:
 *     A vector which contains slices representing bits in idsMap
 *
 * Example:
 *     Input idsMap: 00100110b, maxId: 8
 *     Return vector: ["1-2", "5"]
 */
set<string> Orch::generateIdListFromMap(unsigned long idsMap, sai_uint32_t maxId)
{
    unsigned long currentIdMask = 1;
    bool started = false, needGenerateMap = false;
    sai_uint32_t lower, upper;
    set<string> idStringList;
    for (sai_uint32_t id = 0; id <= maxId; id ++)
    {
        // currentIdMask represents the bit mask corresponding to id: (1<<id)
        if (idsMap & currentIdMask)
        {
            if (!started)
            {
                started = true;
                lower = id;
            }
        }
        else
        {
            if (started)
            {
                started = false;
                upper = id - 1;
                needGenerateMap = true;
            }
        }

        if (needGenerateMap)
        {
            if (lower != upper)
            {
                idStringList.insert(to_string(lower) + "-" + to_string(upper));
            }
            else
            {
                idStringList.insert(to_string(lower));
            }
            needGenerateMap = false;
        }

        currentIdMask <<= 1;
    }

    return idStringList;
}


/*
 * isItemIdsMapContinuous
 *
 * Check whether the input idsMap is continuous.
 * An idsMap is continuous means there is no "0"s between any two "1"s in the map.
 * Args:
 *     idsMap: The bitmap of IDs. The LSB stands for ID 0.
 *     maxId: The maximum value of ID.
 * Return:
 *     Whether the idsMap is continous
 *
 * Example:
 *     idsMaps like 00011100, 00001000, 00000000 are continuous
 *     while 00110010 is not because there are 2 "0"s in bit 2, 3 surrounded by "1"s
 */
bool Orch::isItemIdsMapContinuous(unsigned long idsMap, sai_uint32_t maxId)
{
    unsigned long currentIdMask = 1;
    bool isCurrentBitValid = false, hasValidBits = false, hasZeroAfterValidBit = false;

    for (sai_uint32_t id = 0; id < maxId; id ++)
    {
        isCurrentBitValid = ((idsMap & currentIdMask) != 0);
        if (isCurrentBitValid)
        {
            if (!hasValidBits)
                hasValidBits = true;
            if (hasZeroAfterValidBit)
                return false;
        }
        else
        {
            if (hasValidBits && !hasZeroAfterValidBit)
                hasZeroAfterValidBit = true;
        }
        currentIdMask <<= 1;
    }

    return true;
}

void Orch::addConsumer(DBConnector *db, string tableName, int pri)
{
    SWSS_LOG_INFO("addConsumer for %d table %s", db->getDbId(), tableName.c_str());
    if (db->getDbId() == CONFIG_DB || db->getDbId() == STATE_DB || db->getDbId() == CHASSIS_APP_DB)
    {
        if (db->getDbId() == CONFIG_DB && (tableName == CFG_BUFFER_POOL_TABLE_NAME || tableName == CFG_BUFFER_PROFILE_TABLE_NAME))
        {
            SWSS_LOG_INFO("Add executor with DecoratorSubscriberStateTable for CONFIG_DB table %s", tableName.c_str());
            addExecutor(new Consumer(new DecoratorSubscriberStateTable(db, tableName, TableConsumable::DEFAULT_POP_BATCH_SIZE, pri), this, tableName));
        }
        else
        {
            addExecutor(new Consumer(new SubscriberStateTable(db, tableName, TableConsumable::DEFAULT_POP_BATCH_SIZE, pri), this, tableName));
        }
    }
    else
    {
        addExecutor(new Consumer(new ConsumerStateTable(db, tableName, gBatchSize, pri), this, tableName));
    }
}

void Orch::addExecutor(Executor* executor)
{
    auto inserted = m_consumerMap.emplace(std::piecewise_construct,
            std::forward_as_tuple(executor->getName()),
            std::forward_as_tuple(executor));

    // If there is duplication of executorName in m_consumerMap, logic error
    if (!inserted.second)
    {
        SWSS_LOG_THROW("Duplicated executorName in m_consumerMap: %s", executor->getName().c_str());
    }
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

task_process_status Orch::handleSaiCreateStatus(sai_api_t api, sai_status_t status, void *context)
{
    /*
     * This function aims to provide coarse handling of failures in sairedis create
     * operation (i.e., notify users by throwing excepions when failures happen).
     * Return value: task_success - Handled the status successfully. No need to retry this SAI operation.
     *               task_need_retry - Cannot handle the status. Need to retry the SAI operation.
     *               task_failed - Failed to handle the status but another attempt is unlikely to resolve the failure.
     * TODO: 1. Add general handling logic for specific statuses (e.g., SAI_STATUS_ITEM_ALREADY_EXISTS)
     *       2. Develop fine-grain failure handling mechanisms and replace this coarse handling
     *          in each orch.
     *       3. Take the type of sai api into consideration.
     */
    switch (api)
    {
        case SAI_API_FDB:
            switch (status)
            {
                case SAI_STATUS_SUCCESS:
                    SWSS_LOG_WARN("SAI_STATUS_SUCCESS is not expected in handleSaiCreateStatus");
                    return task_success;
                case SAI_STATUS_ITEM_ALREADY_EXISTS:
                    /*
                     *  In FDB creation, there are scenarios where the hardware learns an FDB entry before orchagent.
                     *  In such cases, the FDB SAI creation would report the status of SAI_STATUS_ITEM_ALREADY_EXISTS,
                     *  and orchagent should ignore the error and treat it as entry was explicitly created.
                     */
                    return task_success;
                default:
                    SWSS_LOG_ERROR("Encountered failure in create operation, exiting orchagent, SAI API: %s, status: %s",
                                sai_serialize_api(api).c_str(), sai_serialize_status(status).c_str());
                    abort();
            }
            break;
        case SAI_API_HOSTIF:
            switch (status)
            {
                case SAI_STATUS_SUCCESS:
                    return task_success;
                case SAI_STATUS_FAILURE:
                    /*
                     * Host interface maybe failed due to lane not available.
                     * In some scenarios, like SONiC virtual machine, the invalid lane may be not enabled by VM configuration,
                     * So just ignore the failure and report an error log.
                     */
                    return task_ignore;
                default:
                    SWSS_LOG_ERROR("Encountered failure in create operation, exiting orchagent, SAI API: %s, status: %s",
                                sai_serialize_api(api).c_str(), sai_serialize_status(status).c_str());
                    abort();
            }
        default:
            switch (status)
            {
                case SAI_STATUS_SUCCESS:
                    SWSS_LOG_WARN("SAI_STATUS_SUCCESS is not expected in handleSaiCreateStatus");
                    return task_success;
                default:
                    SWSS_LOG_ERROR("Encountered failure in create operation, exiting orchagent, SAI API: %s, status: %s",
                                sai_serialize_api(api).c_str(), sai_serialize_status(status).c_str());
                    abort();
            }
    }
    return task_need_retry;
}

task_process_status Orch::handleSaiSetStatus(sai_api_t api, sai_status_t status, void *context)
{
    /*
     * This function aims to provide coarse handling of failures in sairedis set
     * operation (i.e., notify users by throwing excepions when failures happen).
     * Return value: task_success - Handled the status successfully. No need to retry this SAI operation.
     *               task_need_retry - Cannot handle the status. Need to retry the SAI operation.
     *               task_failed - Failed to handle the status but another attempt is unlikely to resolve the failure.
     * TODO: 1. Add general handling logic for specific statuses
     *       2. Develop fine-grain failure handling mechanisms and replace this coarse handling
     *          in each orch.
     *       3. Take the type of sai api into consideration.
     */
    if (status == SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_WARN("SAI_STATUS_SUCCESS is not expected in handleSaiSetStatus");
        return task_success;
    }

    switch (api)
    {
        case SAI_API_PORT:
            switch (status)
            {
                case SAI_STATUS_INVALID_ATTR_VALUE_0:
                    /*
                     * If user gives an invalid attribute value, no need to retry or exit orchagent, just fail the current task
                     * and let user correct the configuration.
                     */
                    SWSS_LOG_ERROR("Encountered SAI_STATUS_INVALID_ATTR_VALUE_0 in set operation, task failed, SAI API: %s, status: %s",
                            sai_serialize_api(api).c_str(), sai_serialize_status(status).c_str());
                    return task_failed;
                default:
                    SWSS_LOG_ERROR("Encountered failure in set operation, exiting orchagent, SAI API: %s, status: %s",
                            sai_serialize_api(api).c_str(), sai_serialize_status(status).c_str());
                    abort();
            }
        default:
            SWSS_LOG_ERROR("Encountered failure in set operation, exiting orchagent, SAI API: %s, status: %s",
                        sai_serialize_api(api).c_str(), sai_serialize_status(status).c_str());
            abort();
    }

    return task_need_retry;
}

task_process_status Orch::handleSaiRemoveStatus(sai_api_t api, sai_status_t status, void *context)
{
    /*
     * This function aims to provide coarse handling of failures in sairedis remove
     * operation (i.e., notify users by throwing excepions when failures happen).
     * Return value: task_success - Handled the status successfully. No need to retry this SAI operation.
     *               task_need_retry - Cannot handle the status. Need to retry the SAI operation.
     *               task_failed - Failed to handle the status but another attempt is unlikely to resolve the failure.
     * TODO: 1. Add general handling logic for specific statuses (e.g., SAI_STATUS_OBJECT_IN_USE,
     *          SAI_STATUS_ITEM_NOT_FOUND)
     *       2. Develop fine-grain failure handling mechanisms and replace this coarse handling
     *          in each orch.
     *       3. Take the type of sai api into consideration.
     */
    switch (status)
    {
        case SAI_STATUS_SUCCESS:
            SWSS_LOG_WARN("SAI_STATUS_SUCCESS is not expected in handleSaiRemoveStatus");
            return task_success;
        default:
            SWSS_LOG_ERROR("Encountered failure in remove operation, exiting orchagent, SAI API: %s, status: %s",
                        sai_serialize_api(api).c_str(), sai_serialize_status(status).c_str());
            abort();
    }
    return task_need_retry;
}

task_process_status Orch::handleSaiGetStatus(sai_api_t api, sai_status_t status, void *context)
{
    /*
     * This function aims to provide coarse handling of failures in sairedis get
     * operation (i.e., notify users by throwing excepions when failures happen).
     * Return value: task_success - Handled the status successfully. No need to retry this SAI operation.
     *               task_need_retry - Cannot handle the status. Need to retry the SAI operation.
     *               task_failed - Failed to handle the status but another attempt is unlikely to resolve the failure.
     * TODO: 1. Add general handling logic for specific statuses
     *       2. Develop fine-grain failure handling mechanisms and replace this coarse handling
     *          in each orch.
     *       3. Take the type of sai api into consideration.
     */
    switch (status)
    {
        case SAI_STATUS_SUCCESS:
            SWSS_LOG_WARN("SAI_STATUS_SUCCESS is not expected in handleSaiGetStatus");
            return task_success;
        case SAI_STATUS_NOT_IMPLEMENTED:
            SWSS_LOG_ERROR("Encountered failure in get operation due to the function is not implemented, exiting orchagent, SAI API: %s",
                        sai_serialize_api(api).c_str());
            throw std::logic_error("SAI get function not implemented");
        default:
            SWSS_LOG_ERROR("Encountered failure in get operation, SAI API: %s, status: %s",
                        sai_serialize_api(api).c_str(), sai_serialize_status(status).c_str());
    }
    return task_failed;
}

bool Orch::parseHandleSaiStatusFailure(task_process_status status)
{
    /*
     * This function parses task process status from SAI failure handling function to whether a retry is needed.
     * Return value: true - no retry is needed.
     *               false - retry is needed.
     */
    switch (status)
    {
        case task_need_retry:
            return false;
        case task_failed:
            return true;
        default:
            SWSS_LOG_WARN("task_process_status %d is not expected in parseHandleSaiStatusFailure", status);
    }
    return true;
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
            auto table_name = consumer.getTableName();
            request_.setTableName(table_name);

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
