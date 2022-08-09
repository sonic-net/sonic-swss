#include "tokenize.h"
#include "bufferorch.h"
#include "logger.h"
#include "sai_serialize.h"
#include "warm_restart.h"

#include <inttypes.h>
#include <sstream>
#include <iostream>

using namespace std;

extern sai_port_api_t *sai_port_api;
extern sai_queue_api_t *sai_queue_api;
extern sai_switch_api_t *sai_switch_api;
extern sai_buffer_api_t *sai_buffer_api;

extern PortsOrch *gPortsOrch;
extern sai_object_id_t gSwitchId;

#define BUFFER_POOL_WATERMARK_FLEX_STAT_COUNTER_POLL_MSECS  "60000"


static const vector<sai_buffer_pool_stat_t> bufferPoolWatermarkStatIds =
{
    SAI_BUFFER_POOL_STAT_WATERMARK_BYTES,
    SAI_BUFFER_POOL_STAT_XOFF_ROOM_WATERMARK_BYTES
};

type_map BufferOrch::m_buffer_type_maps = {
    {APP_BUFFER_POOL_TABLE_NAME, new object_reference_map()},
    {APP_BUFFER_PROFILE_TABLE_NAME, new object_reference_map()},
    {APP_BUFFER_QUEUE_TABLE_NAME, new object_reference_map()},
    {APP_BUFFER_PG_TABLE_NAME, new object_reference_map()},
    {APP_BUFFER_PORT_INGRESS_PROFILE_LIST_NAME, new object_reference_map()},
    {APP_BUFFER_PORT_EGRESS_PROFILE_LIST_NAME, new object_reference_map()}
};

map<string, string> buffer_to_ref_table_map = {
    {buffer_pool_field_name, APP_BUFFER_POOL_TABLE_NAME},
    {buffer_profile_field_name, APP_BUFFER_PROFILE_TABLE_NAME},
    {buffer_profile_list_field_name, APP_BUFFER_PROFILE_TABLE_NAME}
};

BufferOrch::BufferOrch(DBConnector *applDb, DBConnector *confDb, DBConnector *stateDb, vector<string> &tableNames) :
    Orch(applDb, tableNames),
    m_flexCounterDb(new DBConnector("FLEX_COUNTER_DB", 0)),
    m_flexCounterTable(new ProducerTable(m_flexCounterDb.get(), FLEX_COUNTER_TABLE)),
    m_flexCounterGroupTable(new ProducerTable(m_flexCounterDb.get(), FLEX_COUNTER_GROUP_TABLE)),
    m_countersDb(new DBConnector("COUNTERS_DB", 0)),
    m_stateBufferMaximumValueTable(stateDb, STATE_BUFFER_MAXIMUM_VALUE_TABLE)
{
    SWSS_LOG_ENTER();
    initTableHandlers();
    initBufferReadyLists(applDb, confDb);
    initFlexCounterGroupTable();
    initBufferConstants();
};

void BufferOrch::initTableHandlers()
{
    SWSS_LOG_ENTER();
    m_bufferHandlerMap.insert(buffer_handler_pair(APP_BUFFER_POOL_TABLE_NAME, &BufferOrch::processBufferPool));
    m_bufferHandlerMap.insert(buffer_handler_pair(APP_BUFFER_PROFILE_TABLE_NAME, &BufferOrch::processBufferProfile));
    m_bufferHandlerMap.insert(buffer_handler_pair(APP_BUFFER_QUEUE_TABLE_NAME, &BufferOrch::processQueue));
    m_bufferHandlerMap.insert(buffer_handler_pair(APP_BUFFER_PG_TABLE_NAME, &BufferOrch::processPriorityGroup));
    m_bufferHandlerMap.insert(buffer_handler_pair(APP_BUFFER_PORT_INGRESS_PROFILE_LIST_NAME, &BufferOrch::processIngressBufferProfileList));
    m_bufferHandlerMap.insert(buffer_handler_pair(APP_BUFFER_PORT_EGRESS_PROFILE_LIST_NAME, &BufferOrch::processEgressBufferProfileList));
}

void BufferOrch::initBufferReadyLists(DBConnector *applDb, DBConnector *confDb)
{
    /*
       Map m_ready_list and m_port_ready_list_ref are designed to track whether the ports are "ready" from buffer's POV
       by testing whether all configured buffer PGs and queues have been applied to SAI. The idea is:
       - bufferorch read initial configuration and put them into m_port_ready_list_ref.
       - A buffer pg or queue item will be put into m_ready_list once it is applied to SAI.
       The rest of port initialization won't be started before the port being ready.

       However, the items won't be applied to admin down ports in dynamic buffer model, which means the admin down ports won't be "ready"
       The solution is:
       - buffermgr to notify bufferorch explicitly to remove the PG and queue items configured on admin down ports
       - bufferorch to add the items to m_ready_list on receiving notifications, which is an existing logic

       Theoretically, the initial configuration should come from CONFIG_DB but APPL_DB is used for warm reboot, because:
       - For cold/fast start, buffermgr is responsible for injecting items to APPL_DB
         There is no guarantee that items in APPL_DB are ready when orchagent starts
       - For warm reboot, APPL_DB is restored from the previous boot, which means they are ready when orchagent starts
         In addition, bufferorch won't be notified removal of items on admin down ports during warm reboot
         because buffermgrd hasn't been started yet.
         Using APPL_DB means items of admin down ports won't be inserted into m_port_ready_list_ref
         and guarantees the admin down ports always be ready in dynamic buffer model
    */
    SWSS_LOG_ENTER();

    if (WarmStart::isWarmStart())
    {
        Table pg_table(applDb, APP_BUFFER_PG_TABLE_NAME);
        initBufferReadyList(pg_table, false);

        Table queue_table(applDb, APP_BUFFER_QUEUE_TABLE_NAME);
        initBufferReadyList(queue_table, false);
    }
    else
    {
        Table pg_table(confDb, CFG_BUFFER_PG_TABLE_NAME);
        initBufferReadyList(pg_table, true);

        Table queue_table(confDb, CFG_BUFFER_QUEUE_TABLE_NAME);
        initBufferReadyList(queue_table, true);
    }
}

void BufferOrch::initBufferReadyList(Table& table, bool isConfigDb)
{
    SWSS_LOG_ENTER();

    std::vector<std::string> keys;
    table.getKeys(keys);

    const char dbKeyDelimiter = (isConfigDb ? config_db_key_delimiter : delimiter);

    // populate the lists with buffer configuration information
    for (const auto& key: keys)
    {
        auto &&tokens = tokenize(key, dbKeyDelimiter);
        if (tokens.size() != 2)
        {
            SWSS_LOG_ERROR("Wrong format of a table '%s' key '%s'. Skip it", table.getTableName().c_str(), key.c_str());
            continue;
        }

        // We need transform the key from config db format to appl db format
        auto appldb_key = tokens[0] + delimiter + tokens[1];
        m_ready_list[appldb_key] = false;

        auto &&port_names = tokenize(tokens[0], list_item_delimiter);

        for(const auto& port_name: port_names)
        {
            SWSS_LOG_INFO("Item %s has been inserted into ready list", appldb_key.c_str());
            m_port_ready_list_ref[port_name].push_back(appldb_key);
        }
    }
}

void BufferOrch::initBufferConstants()
{
    sai_status_t status;
    sai_attribute_t attr;

    attr.id = SAI_SWITCH_ATTR_TOTAL_BUFFER_SIZE;

    status = sai_switch_api->get_switch_attribute(gSwitchId, 1, &attr);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to get Maximum memory size, rv:%d", status);
        // This is not a mandatory attribute so in case of failure we just return
        return;
    }

    vector<FieldValueTuple> fvVector;
    fvVector.emplace_back(make_pair("mmu_size", to_string(attr.value.u64 * 1024)));
    m_stateBufferMaximumValueTable.set("global", fvVector);
    SWSS_LOG_NOTICE("Got maximum memory size %" PRIx64 ", exposing to %s|global",
                    attr.value.u64, STATE_BUFFER_MAXIMUM_VALUE_TABLE);
}

void BufferOrch::initFlexCounterGroupTable(void)
{
    string bufferPoolWmPluginName = "watermark_bufferpool.lua";

    try
    {
        string bufferPoolLuaScript = swss::loadLuaScript(bufferPoolWmPluginName);
        string bufferPoolWmSha = swss::loadRedisScript(m_countersDb.get(), bufferPoolLuaScript);

        vector<FieldValueTuple> fvTuples;
        fvTuples.emplace_back(BUFFER_POOL_PLUGIN_FIELD, bufferPoolWmSha);
        fvTuples.emplace_back(POLL_INTERVAL_FIELD, BUFFER_POOL_WATERMARK_FLEX_STAT_COUNTER_POLL_MSECS);

        m_flexCounterGroupTable->set(BUFFER_POOL_WATERMARK_STAT_COUNTER_FLEX_COUNTER_GROUP, fvTuples);
    }
    catch (const runtime_error &e)
    {
        SWSS_LOG_ERROR("Buffer pool watermark lua script and/or flex counter group not set successfully. Runtime error: %s", e.what());
    }
}

bool BufferOrch::isPortReady(const std::string& port_name) const
{
    SWSS_LOG_ENTER();

    const auto it = m_port_ready_list_ref.find(port_name);
    if (it == m_port_ready_list_ref.cend())
    {
        // we got a port name which wasn't in our gPortsOrch->getAllPorts() list
        // so make the port ready, because we don't have any buffer configuration for it
        return true;
    }

    const auto& list_of_keys = it->second;

    bool result = true;
    for (const auto& key: list_of_keys)
    {
        result = result && m_ready_list.at(key);
    }

    return result;
}

void BufferOrch::clearBufferPoolWatermarkCounterIdList(const sai_object_id_t object_id)
{
    if (m_isBufferPoolWatermarkCounterIdListGenerated)
    {
        string key = BUFFER_POOL_WATERMARK_STAT_COUNTER_FLEX_COUNTER_GROUP ":" + sai_serialize_object_id(object_id);
        m_flexCounterTable->del(key);
    }
}

void BufferOrch::generateBufferPoolWatermarkCounterIdList(void)
{
    // This function will be called in FlexCounterOrch when field:value tuple "FLEX_COUNTER_STATUS":"enable"
    // is received on buffer pool watermark key under table "FLEX_COUNTER_GROUP_TABLE"
    // Because the SubscriberStateTable listens to the entire keyspace of "BUFFER_POOL_WATERMARK", any update
    // to field value tuples under key "BUFFER_POOL_WATERMARK" will cause this tuple to be heard again
    // To avoid resync the counter ID list a second time, we introduce a data member variable to mark whether
    // this operation has already been done or not yet
    if (m_isBufferPoolWatermarkCounterIdListGenerated)
    {
        return;
    }

    // Detokenize the SAI watermark stats to a string, separated by comma
    string statList;
    for (const auto &it : bufferPoolWatermarkStatIds)
    {
        statList += (sai_serialize_buffer_pool_stat(it) + list_item_delimiter);
    }
    if (!statList.empty())
    {
        statList.pop_back();
    }

    // Some platforms do not support buffer pool watermark clear operation on a particular pool
    // Invoke the SAI clear_stats API per pool to query the capability from the API call return status
    // We use bit mask to mark the clear watermark capability of each buffer pool. We use an unsigned int to place hold
    // these bits. This assumes the total number of buffer pools to be no greater than 32, which should satisfy all use cases.
    unsigned int noWmClrCapability = 0;
    unsigned int bitMask = 1;
    for (const auto &it : *(m_buffer_type_maps[APP_BUFFER_POOL_TABLE_NAME]))
    {
        sai_status_t status = sai_buffer_api->clear_buffer_pool_stats(
                it.second.m_saiObjectId,
                static_cast<uint32_t>(bufferPoolWatermarkStatIds.size()),
                reinterpret_cast<const sai_stat_id_t *>(bufferPoolWatermarkStatIds.data()));
        if (status ==  SAI_STATUS_NOT_SUPPORTED || status == SAI_STATUS_NOT_IMPLEMENTED)
        {
            SWSS_LOG_NOTICE("Clear watermark failed on %s, rv: %s", it.first.c_str(), sai_serialize_status(status).c_str());
            noWmClrCapability |= bitMask;
        }

        bitMask <<= 1;
    }

    if (!noWmClrCapability)
    {
        vector<FieldValueTuple> fvs;

        fvs.emplace_back(STATS_MODE_FIELD, STATS_MODE_READ_AND_CLEAR);
        m_flexCounterGroupTable->set(BUFFER_POOL_WATERMARK_STAT_COUNTER_FLEX_COUNTER_GROUP, fvs);
    }

    // Push buffer pool watermark COUNTER_ID_LIST to FLEX_COUNTER_TABLE on a per buffer pool basis
    vector<FieldValueTuple> fvTuples;
    fvTuples.emplace_back(BUFFER_POOL_COUNTER_ID_LIST, statList);
    bitMask = 1;
    for (const auto &it : *(m_buffer_type_maps[APP_BUFFER_POOL_TABLE_NAME]))
    {
        string key = BUFFER_POOL_WATERMARK_STAT_COUNTER_FLEX_COUNTER_GROUP ":" + sai_serialize_object_id(it.second.m_saiObjectId);

        if (noWmClrCapability)
        {
            string stats_mode = STATS_MODE_READ_AND_CLEAR;
            if (noWmClrCapability & bitMask)
            {
                stats_mode = STATS_MODE_READ;
            }
            fvTuples.emplace_back(STATS_MODE_FIELD, stats_mode);

            m_flexCounterTable->set(key, fvTuples);
            fvTuples.pop_back();
            bitMask <<= 1;
        }
        else
        {
            m_flexCounterTable->set(key, fvTuples);
        }
    }

    m_isBufferPoolWatermarkCounterIdListGenerated = true;
}

const object_reference_map &BufferOrch::getBufferPoolNameOidMap(void)
{
    // In the case different Orches are running in
    // different threads, caller may need to grab a read lock
    // before calling this function
    return *m_buffer_type_maps[APP_BUFFER_POOL_TABLE_NAME];
}

task_process_status BufferOrch::processBufferPool(KeyOpFieldsValuesTuple &tuple)
{
    SWSS_LOG_ENTER();
    sai_status_t sai_status;
    sai_object_id_t sai_object = SAI_NULL_OBJECT_ID;
    string map_type_name = APP_BUFFER_POOL_TABLE_NAME;
    string object_name = kfvKey(tuple);
    string op = kfvOp(tuple);

    SWSS_LOG_DEBUG("object name:%s", object_name.c_str());
    if (m_buffer_type_maps[map_type_name]->find(object_name) != m_buffer_type_maps[map_type_name]->end())
    {
        sai_object = (*(m_buffer_type_maps[map_type_name]))[object_name].m_saiObjectId;
        SWSS_LOG_DEBUG("found existing object:%s of type:%s", object_name.c_str(), map_type_name.c_str());
        if ((*(m_buffer_type_maps[map_type_name]))[object_name].m_pendingRemove && op == SET_COMMAND)
        {
            SWSS_LOG_NOTICE("Entry %s %s is pending remove, need retry", map_type_name.c_str(), object_name.c_str());
            return task_process_status::task_need_retry;
        }
    }
    SWSS_LOG_DEBUG("processing command:%s", op.c_str());

    if (op == SET_COMMAND)
    {
        vector<sai_attribute_t> attribs;
        for (auto i = kfvFieldsValues(tuple).begin(); i != kfvFieldsValues(tuple).end(); i++)
        {
            string field = fvField(*i);
            string value = fvValue(*i);

            SWSS_LOG_DEBUG("field:%s, value:%s", field.c_str(), value.c_str());
            sai_attribute_t attr;
            if (field == buffer_size_field_name)
            {
                attr.id = SAI_BUFFER_POOL_ATTR_SIZE;
                attr.value.u64 = (uint64_t)stoul(value);
                attribs.push_back(attr);
            }
            else if (field == buffer_pool_type_field_name)
            {
                string type = value;

                if (SAI_NULL_OBJECT_ID != sai_object)
                {
                    // We should skip the pool type when setting a pool's attribute because it's create only
                    // when setting a pool's attribute.
                    SWSS_LOG_INFO("Skip setting buffer pool type %s for pool %s", type.c_str(), object_name.c_str());
                    continue;
                }

                if (type == buffer_value_ingress)
                {
                    attr.value.u32 = SAI_BUFFER_POOL_TYPE_INGRESS;
                }
                else if (type == buffer_value_egress)
                {
                    attr.value.u32 = SAI_BUFFER_POOL_TYPE_EGRESS;
                }
                else if (type == buffer_value_both)
                {
                    attr.value.u32 = SAI_BUFFER_POOL_TYPE_BOTH;
                }
                else
                {
                    SWSS_LOG_ERROR("Unknown pool type specified:%s", type.c_str());
                    return task_process_status::task_invalid_entry;
                }
                attr.id = SAI_BUFFER_POOL_ATTR_TYPE;
                attribs.push_back(attr);
            }
            else if (field == buffer_pool_mode_field_name)
            {
                string mode = value;

                if (SAI_NULL_OBJECT_ID != sai_object)
                {
                    // We should skip the pool mode when setting a pool's attribute because it's create only.
                    SWSS_LOG_INFO("Skip setting buffer pool mode %s for pool %s", mode.c_str(), object_name.c_str());
                    continue;
                }

                if (mode == buffer_pool_mode_dynamic_value)
                {
                    attr.value.u32 = SAI_BUFFER_POOL_THRESHOLD_MODE_DYNAMIC;
                }
                else if (mode == buffer_pool_mode_static_value)
                {
                    attr.value.u32 = SAI_BUFFER_POOL_THRESHOLD_MODE_STATIC;
                }
                else
                {
                    SWSS_LOG_ERROR("Unknown pool mode specified:%s", mode.c_str());
                    return task_process_status::task_invalid_entry;
                }
                attr.id = SAI_BUFFER_POOL_ATTR_THRESHOLD_MODE;
                attribs.push_back(attr);
            }
            else if (field == buffer_pool_xoff_field_name)
            {
                attr.value.u64 = (uint64_t)stoul(value);
                attr.id = SAI_BUFFER_POOL_ATTR_XOFF_SIZE;
                attribs.push_back(attr);
            }
            else
            {
                SWSS_LOG_ERROR("Unknown pool field specified:%s, ignoring", field.c_str());
                continue;
            }
        }
        if (SAI_NULL_OBJECT_ID != sai_object)
        {
            for (auto &attribute : attribs)
            {
                sai_status = sai_buffer_api->set_buffer_pool_attribute(sai_object, &attribute);
                if (SAI_STATUS_ATTR_NOT_IMPLEMENTED_0 == sai_status)
                {
                    SWSS_LOG_NOTICE("Buffer pool SET for name:%s, sai object:%" PRIx64 ", not implemented. status:%d. Ignoring it", object_name.c_str(), sai_object, sai_status);
                    return task_process_status::task_ignore;
                }
                else if (SAI_STATUS_SUCCESS != sai_status)
                {
                    SWSS_LOG_ERROR("Failed to modify buffer pool, name:%s, sai object:%" PRIx64 ", status:%d", object_name.c_str(), sai_object, sai_status);
                    task_process_status handle_status = handleSaiSetStatus(SAI_API_BUFFER, sai_status);
                    if (handle_status != task_process_status::task_success)
                    {
                        return handle_status;
                    }
                }
            }
            SWSS_LOG_DEBUG("Modified existing pool:%" PRIx64 ", type:%s name:%s ", sai_object, map_type_name.c_str(), object_name.c_str());
        }
        else
        {
            sai_status = sai_buffer_api->create_buffer_pool(&sai_object, gSwitchId, (uint32_t)attribs.size(), attribs.data());
            if (SAI_STATUS_SUCCESS != sai_status)
            {
                SWSS_LOG_ERROR("Failed to create buffer pool %s with type %s, rv:%d", object_name.c_str(), map_type_name.c_str(), sai_status);
                task_process_status handle_status = handleSaiCreateStatus(SAI_API_BUFFER, sai_status);
                if (handle_status != task_process_status::task_success)
                {
                    return handle_status;
                }
            }

            (*(m_buffer_type_maps[map_type_name]))[object_name].m_saiObjectId = sai_object;
            (*(m_buffer_type_maps[map_type_name]))[object_name].m_pendingRemove = false;
            SWSS_LOG_NOTICE("Created buffer pool %s with type %s", object_name.c_str(), map_type_name.c_str());
            // Here we take the PFC watchdog approach to update the COUNTERS_DB metadata (e.g., PFC_WD_DETECTION_TIME per queue)
            // at initialization (creation and registration phase)
            // Specifically, we push the buffer pool name to oid mapping upon the creation of the oid
            // In pg and queue case, this mapping installment is deferred to FlexCounterOrch at a reception of field
            // "FLEX_COUNTER_STATUS"
            m_countersDb->hset(COUNTERS_BUFFER_POOL_NAME_MAP, object_name, sai_serialize_object_id(sai_object));
        }
    }
    else if (op == DEL_COMMAND)
    {
        if (isObjectBeingReferenced(m_buffer_type_maps, map_type_name, object_name))
        {
            auto hint = objectReferenceInfo(m_buffer_type_maps, map_type_name, object_name);
            SWSS_LOG_NOTICE("Can't remove object %s due to being referenced (%s)", object_name.c_str(), hint.c_str());
            (*(m_buffer_type_maps[map_type_name]))[object_name].m_pendingRemove = true;

            return task_process_status::task_need_retry;
        }

        if (SAI_NULL_OBJECT_ID != sai_object)
        {
            clearBufferPoolWatermarkCounterIdList(sai_object);
            sai_status = sai_buffer_api->remove_buffer_pool(sai_object);
            if (SAI_STATUS_SUCCESS != sai_status)
            {
                SWSS_LOG_ERROR("Failed to remove buffer pool %s with type %s, rv:%d", object_name.c_str(), map_type_name.c_str(), sai_status);
                task_process_status handle_status = handleSaiRemoveStatus(SAI_API_BUFFER, sai_status);
                if (handle_status != task_process_status::task_success)
                {
                    return handle_status;
                }
            }
            SWSS_LOG_NOTICE("Removed buffer pool %s with type %s", object_name.c_str(), map_type_name.c_str());
        }
        auto it_to_delete = (m_buffer_type_maps[map_type_name])->find(object_name);
        (m_buffer_type_maps[map_type_name])->erase(it_to_delete);
        m_countersDb->hdel(COUNTERS_BUFFER_POOL_NAME_MAP, object_name);
    }
    else
    {
        SWSS_LOG_ERROR("Unknown operation type %s", op.c_str());
        return task_process_status::task_invalid_entry;
    }
    return task_process_status::task_success;
}

task_process_status BufferOrch::processBufferProfile(KeyOpFieldsValuesTuple &tuple)
{
    SWSS_LOG_ENTER();
    sai_status_t sai_status;
    sai_object_id_t sai_object = SAI_NULL_OBJECT_ID;
    string map_type_name = APP_BUFFER_PROFILE_TABLE_NAME;
    string object_name = kfvKey(tuple);
    string op = kfvOp(tuple);
    string pool_name;

    SWSS_LOG_DEBUG("object name:%s", object_name.c_str());
    if (m_buffer_type_maps[map_type_name]->find(object_name) != m_buffer_type_maps[map_type_name]->end())
    {
        sai_object = (*(m_buffer_type_maps[map_type_name]))[object_name].m_saiObjectId;
        SWSS_LOG_DEBUG("found existing object:%s of type:%s", object_name.c_str(), map_type_name.c_str());
        if ((*(m_buffer_type_maps[map_type_name]))[object_name].m_pendingRemove && op == SET_COMMAND)
        {
            SWSS_LOG_NOTICE("Entry %s %s is pending remove, need retry", map_type_name.c_str(), object_name.c_str());
            return task_process_status::task_need_retry;
        }
    }
    SWSS_LOG_DEBUG("processing command:%s", op.c_str());
    if (op == SET_COMMAND)
    {
        vector<sai_attribute_t> attribs;
        for (auto i = kfvFieldsValues(tuple).begin(); i != kfvFieldsValues(tuple).end(); i++)
        {
            string field = fvField(*i);
            string value = fvValue(*i);

            SWSS_LOG_DEBUG("field:%s, value:%s", field.c_str(), value.c_str());
            sai_attribute_t attr;
            if (field == buffer_pool_field_name)
            {
                sai_object_id_t sai_pool;
                ref_resolve_status resolve_result = resolveFieldRefValue(m_buffer_type_maps, buffer_pool_field_name,
                                                    buffer_to_ref_table_map.at(buffer_pool_field_name),
                                                    tuple, sai_pool, pool_name);
                if (ref_resolve_status::success != resolve_result)
                {
                    if(ref_resolve_status::not_resolved == resolve_result)
                    {
                        SWSS_LOG_INFO("Missing or invalid pool reference specified");
                        return task_process_status::task_need_retry;
                    }
                    SWSS_LOG_ERROR("Resolving pool reference failed");
                    return task_process_status::task_failed;
                }
                if (SAI_NULL_OBJECT_ID != sai_object)
                {
                    // We should skip the profile's pool name because it's create only
                    // when setting a profile's attribute.
                    SWSS_LOG_INFO("Skip setting buffer profile's pool %s for profile %s", value.c_str(), object_name.c_str());
                    continue;
                }
                attr.id = SAI_BUFFER_PROFILE_ATTR_POOL_ID;
                attr.value.oid = sai_pool;
                attribs.push_back(attr);
            }
            else if (field == buffer_xon_field_name)
            {
                attr.value.u64 = (uint64_t)stoul(value);
                attr.id = SAI_BUFFER_PROFILE_ATTR_XON_TH;
                attribs.push_back(attr);
            }
            else if (field == buffer_xon_offset_field_name)
            {
                attr.value.u64 = (uint64_t)stoul(value);
                attr.id = SAI_BUFFER_PROFILE_ATTR_XON_OFFSET_TH;
                attribs.push_back(attr);
            }
            else if (field == buffer_xoff_field_name)
            {
                attr.value.u64 = (uint64_t)stoul(value);
                attr.id = SAI_BUFFER_PROFILE_ATTR_XOFF_TH;
                attribs.push_back(attr);
            }
            else if (field == buffer_size_field_name)
            {
                attr.id = SAI_BUFFER_PROFILE_ATTR_BUFFER_SIZE;
                attr.value.u64 = (uint64_t)stoul(value);
                attribs.push_back(attr);
            }
            else if (field == buffer_dynamic_th_field_name)
            {
                if (SAI_NULL_OBJECT_ID != sai_object)
                {
                    // We should skip the profile's threshold type when setting a profile's attribute because it's create only.
                    SWSS_LOG_INFO("Skip setting buffer profile's threshold type for profile %s", object_name.c_str());
                }
                else
                {
                    attr.id = SAI_BUFFER_PROFILE_ATTR_THRESHOLD_MODE;
                    attr.value.s32 = SAI_BUFFER_PROFILE_THRESHOLD_MODE_DYNAMIC;
                    attribs.push_back(attr);
                }

                attr.id = SAI_BUFFER_PROFILE_ATTR_SHARED_DYNAMIC_TH;
                attr.value.s8 = (sai_int8_t)stol(value);
                attribs.push_back(attr);
            }
            else if (field == buffer_static_th_field_name)
            {
                if (SAI_NULL_OBJECT_ID != sai_object)
                {
                    // We should skip the profile's threshold type when setting a profile's attribute because it's create only.
                    SWSS_LOG_INFO("Skip setting buffer profile's threshold type for profile %s", object_name.c_str());
                }
                else
                {
                    attr.id = SAI_BUFFER_PROFILE_ATTR_THRESHOLD_MODE;
                    attr.value.s32 = SAI_BUFFER_PROFILE_THRESHOLD_MODE_STATIC;
                    attribs.push_back(attr);
                }

                attr.id = SAI_BUFFER_PROFILE_ATTR_SHARED_STATIC_TH;
                attr.value.u64 = (uint64_t)stoul(value);
                attribs.push_back(attr);
            }
            else
            {
                SWSS_LOG_ERROR("Unknown buffer profile field specified:%s, ignoring", field.c_str());
                continue;
            }
        }
        if (SAI_NULL_OBJECT_ID != sai_object)
        {
            SWSS_LOG_DEBUG("Modifying existing sai object:%" PRIx64, sai_object);
            for (auto &attribute : attribs)
            {
                sai_status = sai_buffer_api->set_buffer_profile_attribute(sai_object, &attribute);
                if (SAI_STATUS_ATTR_NOT_IMPLEMENTED_0 == sai_status)
                {
                    SWSS_LOG_NOTICE("Buffer profile SET for name:%s, sai object:%" PRIx64 ", not implemented. status:%d. Ignoring it", object_name.c_str(), sai_object, sai_status);
                    return task_process_status::task_ignore;
                }
                else if (SAI_STATUS_SUCCESS != sai_status)
                {
                    SWSS_LOG_ERROR("Failed to modify buffer profile, name:%s, sai object:%" PRIx64 ", status:%d", object_name.c_str(), sai_object, sai_status);
                    task_process_status handle_status = handleSaiSetStatus(SAI_API_BUFFER, sai_status);
                    if (handle_status != task_process_status::task_success)
                    {
                        return handle_status;
                    }
                }
            }
        }
        else
        {
            sai_status = sai_buffer_api->create_buffer_profile(&sai_object, gSwitchId, (uint32_t)attribs.size(), attribs.data());
            if (SAI_STATUS_SUCCESS != sai_status)
            {
                SWSS_LOG_ERROR("Failed to create buffer profile %s with type %s, rv:%d", object_name.c_str(), map_type_name.c_str(), sai_status);
                task_process_status handle_status = handleSaiCreateStatus(SAI_API_BUFFER, sai_status);
                if (handle_status != task_process_status::task_success)
                {
                    return handle_status;
                }
            }
            (*(m_buffer_type_maps[map_type_name]))[object_name].m_saiObjectId = sai_object;
            (*(m_buffer_type_maps[map_type_name]))[object_name].m_pendingRemove = false;
            SWSS_LOG_NOTICE("Created buffer profile %s with type %s", object_name.c_str(), map_type_name.c_str());
        }

        // Add reference to the buffer pool object
        setObjectReference(m_buffer_type_maps, map_type_name, object_name, buffer_pool_field_name, pool_name);
    }
    else if (op == DEL_COMMAND)
    {
        if (isObjectBeingReferenced(m_buffer_type_maps, map_type_name, object_name))
        {
            auto hint = objectReferenceInfo(m_buffer_type_maps, map_type_name, object_name);
            SWSS_LOG_NOTICE("Can't remove object %s due to being referenced (%s)", object_name.c_str(), hint.c_str());
            (*(m_buffer_type_maps[map_type_name]))[object_name].m_pendingRemove = true;

            return task_process_status::task_need_retry;
        }

        if (SAI_NULL_OBJECT_ID != sai_object)
        {
            sai_status = sai_buffer_api->remove_buffer_profile(sai_object);
            if (SAI_STATUS_SUCCESS != sai_status)
            {
                SWSS_LOG_ERROR("Failed to remove buffer profile %s with type %s, rv:%d", object_name.c_str(), map_type_name.c_str(), sai_status);
                task_process_status handle_status = handleSaiRemoveStatus(SAI_API_BUFFER, sai_status);
                if (handle_status != task_process_status::task_success)
                {
                    return handle_status;
                }
            }
        }

        SWSS_LOG_NOTICE("Remove buffer profile %s with type %s", object_name.c_str(), map_type_name.c_str());
        removeObject(m_buffer_type_maps, map_type_name, object_name);
    }
    else
    {
        SWSS_LOG_ERROR("Unknown operation type %s", op.c_str());
        return task_process_status::task_invalid_entry;
    }
    return task_process_status::task_success;
}

/*
Input sample "BUFFER_QUEUE|Ethernet4,Ethernet45|10-15"
*/
task_process_status BufferOrch::processQueue(KeyOpFieldsValuesTuple &tuple)
{
    SWSS_LOG_ENTER();
    sai_object_id_t sai_buffer_profile;
    string buffer_profile_name;
    const string key = kfvKey(tuple);
    string op = kfvOp(tuple);
    vector<string> tokens;
    sai_uint32_t range_low, range_high;
    bool need_update_sai = true;

    SWSS_LOG_DEBUG("Processing:%s", key.c_str());
    tokens = tokenize(key, delimiter);
    if (tokens.size() != 2)
    {
        SWSS_LOG_ERROR("malformed key:%s. Must contain 2 tokens", key.c_str());
        return task_process_status::task_invalid_entry;
    }
    vector<string> port_names = tokenize(tokens[0], list_item_delimiter);
    if (!parseIndexRange(tokens[1], range_low, range_high))
    {
        return task_process_status::task_invalid_entry;
    }

    if (op == SET_COMMAND)
    {
        ref_resolve_status resolve_result = resolveFieldRefValue(m_buffer_type_maps, buffer_profile_field_name,
                                            buffer_to_ref_table_map.at(buffer_profile_field_name), tuple,
                                            sai_buffer_profile, buffer_profile_name);
        if (ref_resolve_status::success != resolve_result)
        {
            if (ref_resolve_status::not_resolved == resolve_result)
            {
                SWSS_LOG_INFO("Missing or invalid queue buffer profile reference specified");
                return task_process_status::task_need_retry;
            }

            SWSS_LOG_ERROR("Resolving queue profile reference failed");
            return task_process_status::task_failed;
        }

        SWSS_LOG_NOTICE("Set buffer queue %s to %s", key.c_str(), buffer_profile_name.c_str());

        setObjectReference(m_buffer_type_maps, APP_BUFFER_QUEUE_TABLE_NAME, key, buffer_profile_field_name, buffer_profile_name);
    }
    else if (op == DEL_COMMAND)
    {
        auto &typemap = (*m_buffer_type_maps[APP_BUFFER_QUEUE_TABLE_NAME]);
        if (typemap.find(key) == typemap.end())
        {
            SWSS_LOG_INFO("%s doesn't not exist, don't need to notfiy SAI", key.c_str());
            need_update_sai = false;
        }
        sai_buffer_profile = SAI_NULL_OBJECT_ID;
        SWSS_LOG_NOTICE("Remove buffer queue %s", key.c_str());
        removeObject(m_buffer_type_maps, APP_BUFFER_QUEUE_TABLE_NAME, key);
    }
    else
    {
        SWSS_LOG_ERROR("Unknown operation type %s", op.c_str());
        return task_process_status::task_invalid_entry;
    }

    sai_attribute_t attr;
    attr.id = SAI_QUEUE_ATTR_BUFFER_PROFILE_ID;
    attr.value.oid = sai_buffer_profile;
    for (string port_name : port_names)
    {
        Port port;
        SWSS_LOG_DEBUG("processing port:%s", port_name.c_str());
        if (!gPortsOrch->getPort(port_name, port))
        {
            SWSS_LOG_ERROR("Port with alias:%s not found", port_name.c_str());
            return task_process_status::task_invalid_entry;
        }
        for (size_t ind = range_low; ind <= range_high; ind++)
        {
            SWSS_LOG_DEBUG("processing queue:%zd", ind);
            if (port.m_queue_ids.size() <= ind)
            {
                SWSS_LOG_ERROR("Invalid queue index specified:%zd", ind);
                return task_process_status::task_invalid_entry;
            }
            if (port.m_queue_lock[ind])
            {
                SWSS_LOG_WARN("Queue %zd on port %s is locked, will retry", ind, port_name.c_str());
                return task_process_status::task_need_retry;
            }
            if (need_update_sai)
            {
                sai_object_id_t queue_id;
                queue_id = port.m_queue_ids[ind];
                SWSS_LOG_DEBUG("Applying buffer profile:0x%" PRIx64 " to queue index:%zd, queue sai_id:0x%" PRIx64, sai_buffer_profile, ind, queue_id);
                sai_status_t sai_status = sai_queue_api->set_queue_attribute(queue_id, &attr);
                if (sai_status != SAI_STATUS_SUCCESS)
                {
                    SWSS_LOG_ERROR("Failed to set queue's buffer profile attribute, status:%d", sai_status);
                    task_process_status handle_status = handleSaiSetStatus(SAI_API_QUEUE, sai_status);
                    if (handle_status != task_process_status::task_success)
                    {
                        return handle_status;
                    }
                }
            }
        }
    }

    if (m_ready_list.find(key) != m_ready_list.end())
    {
        m_ready_list[key] = true;
    }
    else
    {
        // If a buffer queue profile is not in the initial CONFIG_DB BUFFER_QUEUE table
        // at BufferOrch object instantiation, it is considered being applied
        // at run time, and, in this case, is not tracked in the m_ready_list. It is up to
        // the application to guarantee the set order that the buffer queue profile
        // should be applied to a physical port before the physical port is brought up to
        // carry traffic. Here, we alert to application through syslog when such a wrong
        // set order is detected.
        for (const auto &port_name : port_names)
        {
            if (gPortsOrch->isPortAdminUp(port_name)) {
                SWSS_LOG_WARN("Queue profile '%s' applied after port %s is up", key.c_str(), port_name.c_str());
            }
        }
    }

    return task_process_status::task_success;
}

/*
Input sample "BUFFER_PG|Ethernet4,Ethernet45|10-15"
*/
task_process_status BufferOrch::processPriorityGroup(KeyOpFieldsValuesTuple &tuple)
{
    SWSS_LOG_ENTER();
    sai_object_id_t sai_buffer_profile;
    string buffer_profile_name;
    const string key = kfvKey(tuple);
    string op = kfvOp(tuple);
    vector<string> tokens;
    sai_uint32_t range_low, range_high;
    bool need_update_sai = true;

    SWSS_LOG_DEBUG("processing:%s", key.c_str());
    tokens = tokenize(key, delimiter);
    if (tokens.size() != 2)
    {
        SWSS_LOG_ERROR("malformed key:%s. Must contain 2 tokens", key.c_str());
        return task_process_status::task_invalid_entry;
    }
    vector<string> port_names = tokenize(tokens[0], list_item_delimiter);
    if (!parseIndexRange(tokens[1], range_low, range_high))
    {
        SWSS_LOG_ERROR("Failed to obtain pg range values");
        return task_process_status::task_invalid_entry;
    }

    if (op == SET_COMMAND)
    {
        ref_resolve_status  resolve_result = resolveFieldRefValue(m_buffer_type_maps, buffer_profile_field_name,
                                             buffer_to_ref_table_map.at(buffer_profile_field_name), tuple, 
                                             sai_buffer_profile, buffer_profile_name);
        if (ref_resolve_status::success != resolve_result)
        {
            if (ref_resolve_status::not_resolved == resolve_result)
            {
                SWSS_LOG_INFO("Missing or invalid pg profile reference specified");
                return task_process_status::task_need_retry;
            }

            SWSS_LOG_ERROR("Resolving pg profile reference failed");
            return task_process_status::task_failed;
        }

        SWSS_LOG_NOTICE("Set buffer PG %s to %s", key.c_str(), buffer_profile_name.c_str());

        setObjectReference(m_buffer_type_maps, APP_BUFFER_PG_TABLE_NAME, key, buffer_profile_field_name, buffer_profile_name);
    }
    else if (op == DEL_COMMAND)
    {
        auto &typemap = (*m_buffer_type_maps[APP_BUFFER_PG_TABLE_NAME]);
        if (typemap.find(key) == typemap.end())
        {
            SWSS_LOG_INFO("%s doesn't not exist, don't need to notfiy SAI", key.c_str());
            need_update_sai = false;
        }
        sai_buffer_profile = SAI_NULL_OBJECT_ID;
        SWSS_LOG_NOTICE("Remove buffer PG %s", key.c_str());
        removeObject(m_buffer_type_maps, APP_BUFFER_PG_TABLE_NAME, key);
    }
    else
    {
        SWSS_LOG_ERROR("Unknown operation type %s", op.c_str());
        return task_process_status::task_invalid_entry;
    }

    sai_attribute_t attr;
    attr.id = SAI_INGRESS_PRIORITY_GROUP_ATTR_BUFFER_PROFILE;
    attr.value.oid = sai_buffer_profile;
    for (string port_name : port_names)
    {
        Port port;
        SWSS_LOG_DEBUG("processing port:%s", port_name.c_str());
        if (!gPortsOrch->getPort(port_name, port))
        {
            SWSS_LOG_ERROR("Port with alias:%s not found", port_name.c_str());
            return task_process_status::task_invalid_entry;
        }
        for (size_t ind = range_low; ind <= range_high; ind++)
        {
            SWSS_LOG_DEBUG("processing pg:%zd", ind);
            if (port.m_priority_group_ids.size() <= ind)
            {
                SWSS_LOG_ERROR("Invalid pg index specified:%zd", ind);
                return task_process_status::task_invalid_entry;
            }
            else
            {
                if (need_update_sai)
                {
                    sai_object_id_t pg_id;
                    pg_id = port.m_priority_group_ids[ind];
                    SWSS_LOG_DEBUG("Applying buffer profile:0x%" PRIx64 " to port:%s pg index:%zd, pg sai_id:0x%" PRIx64, sai_buffer_profile, port_name.c_str(), ind, pg_id);
                    sai_status_t sai_status = sai_buffer_api->set_ingress_priority_group_attribute(pg_id, &attr);
                    if (sai_status != SAI_STATUS_SUCCESS)
                    {
                        SWSS_LOG_ERROR("Failed to set port:%s pg:%zd buffer profile attribute, status:%d", port_name.c_str(), ind, sai_status);
                        task_process_status handle_status = handleSaiSetStatus(SAI_API_BUFFER, sai_status);
                        if (handle_status != task_process_status::task_success)
                        {
                            return handle_status;
                        }
                    }
                }
            }
        }
    }

    if (m_ready_list.find(key) != m_ready_list.end())
    {
        m_ready_list[key] = true;
    }
    else
    {
        // If a buffer pg profile is not in the initial CONFIG_DB BUFFER_PG table
        // at BufferOrch object instantiation, it is considered being applied
        // at run time, and, in this case, is not tracked in the m_ready_list. It is up to
        // the application to guarantee the set order that the buffer pg profile
        // should be applied to a physical port before the physical port is brought up to
        // carry traffic. Here, we alert to application through syslog when such a wrong
        // set order is detected.
        for (const auto &port_name : port_names)
        {
            if (gPortsOrch->isPortAdminUp(port_name)) {
                SWSS_LOG_WARN("PG profile '%s' applied after port %s is up", key.c_str(), port_name.c_str());
            }
        }
    }

    return task_process_status::task_success;
}

/*
Input sample:"i_port.profile0,i_port.profile1"
*/
task_process_status BufferOrch::processIngressBufferProfileList(KeyOpFieldsValuesTuple &tuple)
{
    SWSS_LOG_ENTER();
    Port port;
    string key = kfvKey(tuple);
    string op = kfvOp(tuple);

    SWSS_LOG_DEBUG("processing:%s", key.c_str());

    vector<string> port_names = tokenize(key, list_item_delimiter);
    vector<sai_object_id_t> profile_list;
    sai_attribute_t attr;
    attr.id = SAI_PORT_ATTR_QOS_INGRESS_BUFFER_PROFILE_LIST;

    if (op == SET_COMMAND)
    {
        string profile_name_list;
        ref_resolve_status resolve_status = resolveFieldRefArray(m_buffer_type_maps, buffer_profile_list_field_name,
                                                                 buffer_to_ref_table_map.at(buffer_profile_list_field_name), tuple,
                                                                 profile_list, profile_name_list);
        if (ref_resolve_status::success != resolve_status)
        {
            if(ref_resolve_status::not_resolved == resolve_status)
            {
                SWSS_LOG_INFO("Missing or invalid ingress buffer profile reference specified for:%s", key.c_str());
                return task_process_status::task_need_retry;
            }
            SWSS_LOG_ERROR("Failed resolving ingress buffer profile reference specified for:%s", key.c_str());
            return task_process_status::task_failed;
        }

        setObjectReference(m_buffer_type_maps, APP_BUFFER_PORT_INGRESS_PROFILE_LIST_NAME, key, buffer_profile_list_field_name, profile_name_list);

        attr.value.objlist.count = (uint32_t)profile_list.size();
        attr.value.objlist.list = profile_list.data();
    }
    else if (op == DEL_COMMAND)
    {
        SWSS_LOG_NOTICE("%s has been removed from BUFFER_PORT_INGRESS_PROFILE_LIST_TABLE", key.c_str());
        removeObject(m_buffer_type_maps, APP_BUFFER_PORT_INGRESS_PROFILE_LIST_NAME, key);
        attr.value.objlist.count = 0;
        attr.value.objlist.list = profile_list.data();
    }
    else
    {
        SWSS_LOG_ERROR("Unknown command %s when handling BUFFER_PORT_INGRESS_PROFILE_LIST_TABLE key %s", op.c_str(), key.c_str());
    }

    for (string port_name : port_names)
    {
        if (!gPortsOrch->getPort(port_name, port))
        {
            SWSS_LOG_ERROR("Port with alias:%s not found", port_name.c_str());
            return task_process_status::task_invalid_entry;
        }
        sai_status_t sai_status = sai_port_api->set_port_attribute(port.m_port_id, &attr);
        if (sai_status != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_ERROR("Failed to set ingress buffer profile list on port, status:%d, key:%s", sai_status, port_name.c_str());
            task_process_status handle_status = handleSaiSetStatus(SAI_API_PORT, sai_status);
            if (handle_status != task_process_status::task_success)
            {
                return handle_status;
            }
        }
    }

    return task_process_status::task_success;
}

/*
Input sample:"e_port.profile0,e_port.profile1"
*/
task_process_status BufferOrch::processEgressBufferProfileList(KeyOpFieldsValuesTuple &tuple)
{
    SWSS_LOG_ENTER();
    Port port;
    string key = kfvKey(tuple);
    string op = kfvOp(tuple);
    SWSS_LOG_DEBUG("processing:%s", key.c_str());
    vector<string> port_names = tokenize(key, list_item_delimiter);
    vector<sai_object_id_t> profile_list;
    sai_attribute_t attr;
    attr.id = SAI_PORT_ATTR_QOS_EGRESS_BUFFER_PROFILE_LIST;

    if (op == SET_COMMAND)
    {
        string profile_name_list;
        ref_resolve_status resolve_status = resolveFieldRefArray(m_buffer_type_maps, buffer_profile_list_field_name,
                                                                 buffer_to_ref_table_map.at(buffer_profile_list_field_name), tuple,
                                                                 profile_list, profile_name_list);
        if (ref_resolve_status::success != resolve_status)
        {
            if(ref_resolve_status::not_resolved == resolve_status)
            {
                SWSS_LOG_INFO("Missing or invalid egress buffer profile reference specified for:%s", key.c_str());
                return task_process_status::task_need_retry;
            }
            SWSS_LOG_ERROR("Failed resolving egress buffer profile reference specified for:%s", key.c_str());
            return task_process_status::task_failed;
        }

        setObjectReference(m_buffer_type_maps, APP_BUFFER_PORT_EGRESS_PROFILE_LIST_NAME, key, buffer_profile_list_field_name, profile_name_list);

        attr.value.objlist.count = (uint32_t)profile_list.size();
        attr.value.objlist.list = profile_list.data();
    }
    else if (op == DEL_COMMAND)
    {
        SWSS_LOG_NOTICE("%s has been removed from BUFFER_PORT_EGRESS_PROFILE_LIST_TABLE", key.c_str());
        removeObject(m_buffer_type_maps, APP_BUFFER_PORT_EGRESS_PROFILE_LIST_NAME, key);
        attr.value.objlist.count = 0;
        attr.value.objlist.list = profile_list.data();
    }
    else
    {
        SWSS_LOG_ERROR("Unknown command %s when handling BUFFER_PORT_EGRESS_PROFILE_LIST_TABLE key %s", op.c_str(), key.c_str());
    }

    for (string port_name : port_names)
    {
        if (!gPortsOrch->getPort(port_name, port))
        {
            SWSS_LOG_ERROR("Port with alias:%s not found", port_name.c_str());
            return task_process_status::task_invalid_entry;
        }
        sai_status_t sai_status = sai_port_api->set_port_attribute(port.m_port_id, &attr);
        if (sai_status != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_ERROR("Failed to set egress buffer profile list on port, status:%d, key:%s", sai_status, port_name.c_str());
            task_process_status handle_status = handleSaiSetStatus(SAI_API_PORT, sai_status);
            if (handle_status != task_process_status::task_success)
            {
                return handle_status;
            }
        }
    }

    return task_process_status::task_success;
}

void BufferOrch::doTask()
{
    // The hidden dependency tree:
    // ref: https://github.com/opencomputeproject/SAI/blob/master/doc/QOS/SAI-Proposal-buffers-Ver4.docx
    //      2	    SAI model
    //      3.1	    Ingress priority group (PG) configuration
    //      3.2.1	Buffer profile configuration
    //
    // buffer pool
    // └── buffer profile
    //     ├── buffer port ingress profile list
    //     ├── buffer port egress profile list
    //     ├── buffer queue
    //     └── buffer pq table

    SWSS_LOG_DEBUG("Handling buffer task");

    auto pool_consumer = getExecutor((APP_BUFFER_POOL_TABLE_NAME));
    pool_consumer->drain();

    auto profile_consumer = getExecutor(APP_BUFFER_PROFILE_TABLE_NAME);
    profile_consumer->drain();

    for(auto &it : m_consumerMap)
    {
        auto consumer = it.second.get();
        if (consumer == profile_consumer)
            continue;
        if (consumer == pool_consumer)
            continue;
        consumer->drain();
    }
}

void BufferOrch::doTask(Consumer &consumer)
{
    SWSS_LOG_ENTER();

    if (!gPortsOrch->isConfigDone())
    {
        SWSS_LOG_INFO("Buffer task for %s can't be executed ahead of port config done", consumer.getTableName().c_str());
        return;
    }

    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
        /* Make sure the handler is initialized for the task */
        auto map_type_name = consumer.getTableName();
        if (m_bufferHandlerMap.find(map_type_name) == m_bufferHandlerMap.end())
        {
            SWSS_LOG_ERROR("No handler for key:%s found.", map_type_name.c_str());
            it = consumer.m_toSync.erase(it);
            continue;
        }

        auto task_status = (this->*(m_bufferHandlerMap[map_type_name]))(it->second);
        switch(task_status)
        {
            case task_process_status::task_success :
            case task_process_status::task_ignore :
                it = consumer.m_toSync.erase(it);
                break;
            case task_process_status::task_invalid_entry:
                SWSS_LOG_ERROR("Failed to process invalid buffer task");
                it = consumer.m_toSync.erase(it);
                break;
            case task_process_status::task_failed:
                SWSS_LOG_ERROR("Failed to process buffer task, drop it");
                it = consumer.m_toSync.erase(it);
                return;
            case task_process_status::task_need_retry:
                SWSS_LOG_INFO("Failed to process buffer task, retry it");
                it++;
                break;
            default:
                SWSS_LOG_ERROR("Invalid task status %d", task_status);
                it = consumer.m_toSync.erase(it);
                break;
        }
    }
}
