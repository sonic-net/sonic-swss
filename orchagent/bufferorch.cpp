#include "tokenize.h"
#include "bufferorch.h"
#include "directory.h"
#include "logger.h"
#include "sai_serialize.h"
#include "warm_restart.h"

#include <inttypes.h>
#include <sstream>
#include <iostream>
#include <boost/algorithm/string.hpp>

#include "buffer/bufferschema.h"

using namespace std;

extern sai_port_api_t *sai_port_api;
extern sai_queue_api_t *sai_queue_api;
extern sai_switch_api_t *sai_switch_api;
extern sai_buffer_api_t *sai_buffer_api;

extern PortsOrch *gPortsOrch;
extern Directory<Orch*> gDirectory;
extern sai_object_id_t gSwitchId;
extern string gMySwitchType;
extern string gMyHostName;
extern string gMyAsicName;

static const vector<sai_buffer_pool_stat_t> bufferPoolWatermarkStatIds =
{
    SAI_BUFFER_POOL_STAT_WATERMARK_BYTES,
    SAI_BUFFER_POOL_STAT_XOFF_ROOM_WATERMARK_BYTES
};

type_map BufferOrch::m_buffer_type_maps = {
    {APP_BUFFER_POOL_TABLE_NAME, make_shared<object_reference_map>()},
    {APP_BUFFER_PROFILE_TABLE_NAME, make_shared<object_reference_map>()},
    {APP_BUFFER_QUEUE_TABLE_NAME, make_shared<object_reference_map>()},
    {APP_BUFFER_PG_TABLE_NAME, make_shared<object_reference_map>()},
    {APP_BUFFER_PORT_INGRESS_PROFILE_LIST_NAME, make_shared<object_reference_map>()},
    {APP_BUFFER_PORT_EGRESS_PROFILE_LIST_NAME, make_shared<object_reference_map>()}
};

map<string, string> buffer_to_ref_table_map = {
    {buffer_pool_field_name, APP_BUFFER_POOL_TABLE_NAME},
    {buffer_profile_field_name, APP_BUFFER_PROFILE_TABLE_NAME},
    {buffer_profile_list_field_name, APP_BUFFER_PROFILE_TABLE_NAME}
};

std::map<string, std::map<size_t, string>> pg_port_flags;
std::map<string, std::map<size_t, string>> queue_port_flags;

BufferOrch::BufferOrch(DBConnector *applDb, DBConnector *confDb, DBConnector *stateDb, vector<string> &tableNames) :
    Orch(applDb, tableNames),
    m_counterNameMapUpdater(new CounterNameMapUpdater("COUNTERS_DB", COUNTERS_BUFFER_POOL_NAME_MAP)),
    m_countersDb(new DBConnector("COUNTERS_DB", 0)),
    m_stateBufferMaximumValueTable(stateDb, STATE_BUFFER_MAXIMUM_VALUE_TABLE)
{
    SWSS_LOG_ENTER();
    initTableHandlers();
    initBufferReadyLists(applDb, confDb);
    initFlexCounterGroupTable();

    if (gMySwitchType != "dpu")
    {
        initBufferConstants();
    }
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

    m_bufferFlushHandlerMap.insert(buffer_flush_handler_pair(APP_BUFFER_PORT_INGRESS_PROFILE_LIST_NAME, &BufferOrch::processIngressBufferProfileListBulk));
    m_bufferFlushHandlerMap.insert(buffer_flush_handler_pair(APP_BUFFER_PORT_EGRESS_PROFILE_LIST_NAME, &BufferOrch::processEgressBufferProfileListBulk));
    m_bufferFlushHandlerMap.insert(buffer_flush_handler_pair(APP_BUFFER_PG_TABLE_NAME, &BufferOrch::processPriorityGroupBulk));
    m_bufferFlushHandlerMap.insert(buffer_flush_handler_pair(APP_BUFFER_QUEUE_TABLE_NAME, &BufferOrch::processQueueBulk));
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

        if(gMySwitchType == "voq") 
        {
            Table queue_table(applDb, APP_BUFFER_QUEUE_TABLE_NAME);
            initVoqBufferReadyList(queue_table, false);
        }
        else
        {
            Table queue_table(applDb, APP_BUFFER_QUEUE_TABLE_NAME);
            initBufferReadyList(queue_table, false);
        }
    }
    else
    {
        Table pg_table(confDb, CFG_BUFFER_PG_TABLE_NAME);
        initBufferReadyList(pg_table, true);

        if(gMySwitchType == "voq") 
        {
            Table queue_table(confDb, CFG_BUFFER_QUEUE_TABLE_NAME);
            initVoqBufferReadyList(queue_table, true);
        }
        else
        {
            Table queue_table(confDb, CFG_BUFFER_QUEUE_TABLE_NAME);
            initBufferReadyList(queue_table, true);
        }
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

void BufferOrch::initVoqBufferReadyList(Table& table, bool isConfigDb)
{
    SWSS_LOG_ENTER();

    std::vector<std::string> keys;
    table.getKeys(keys);

    const char dbKeyDelimiter = (isConfigDb ? config_db_key_delimiter : delimiter);

    // populate the lists with buffer configuration information
    for (const auto& key: keys)
    {
        auto &&tokens = tokenize(key, dbKeyDelimiter);
        if (tokens.size() != 4)
        {
            SWSS_LOG_ERROR("Wrong format of a table '%s' key '%s'. Skip it", table.getTableName().c_str(), key.c_str());
            continue;
        }

        // We need transform the key from config db format to appl db format
        auto appldb_key = tokens[0] + config_db_key_delimiter + tokens[1] + config_db_key_delimiter + tokens[2] + delimiter + tokens[3];
        m_ready_list[appldb_key] = false;

        auto &&port_names = tokenize(tokens[0] + config_db_key_delimiter + tokens[1] + config_db_key_delimiter + tokens[2], list_item_delimiter);
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
    string bufferPoolWmSha;

    try
    {
        string bufferPoolLuaScript = swss::loadLuaScript(bufferPoolWmPluginName);
        bufferPoolWmSha = swss::loadRedisScript(m_countersDb.get(), bufferPoolLuaScript);
    }
    catch (const runtime_error &e)
    {
        SWSS_LOG_ERROR("Buffer pool watermark lua script and/or flex counter group not set successfully. Runtime error: %s", e.what());
    }

    setFlexCounterGroupParameter(BUFFER_POOL_WATERMARK_STAT_COUNTER_FLEX_COUNTER_GROUP,
                                 BUFFER_POOL_WATERMARK_FLEX_STAT_COUNTER_POLL_MSECS,
                                 "", // do not touch stats_mode
                                 BUFFER_POOL_PLUGIN_FIELD,
                                 bufferPoolWmSha);
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
        stopFlexCounterPolling(gSwitchId, key);
    }
}

void BufferOrch::addBufferPoolWatermarkCounterIdList(const sai_object_id_t object_id)
{
    if (m_isBufferPoolWatermarkCounterIdListGenerated)
    {
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

        string key = BUFFER_POOL_WATERMARK_STAT_COUNTER_FLEX_COUNTER_GROUP ":" + sai_serialize_object_id(object_id);
        string stats_mode = "";
        startFlexCounterPolling(gSwitchId, key, statList, BUFFER_POOL_COUNTER_ID_LIST, stats_mode);
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
        setFlexCounterGroupStatsMode(BUFFER_POOL_WATERMARK_STAT_COUNTER_FLEX_COUNTER_GROUP,
                                     STATS_MODE_READ_AND_CLEAR);
    }

    // Push buffer pool watermark COUNTER_ID_LIST to FLEX_COUNTER_TABLE on a per buffer pool basis
    string stats_mode;

    bitMask = 1;

    for (const auto &it : *(m_buffer_type_maps[APP_BUFFER_POOL_TABLE_NAME]))
    {
        string key = BUFFER_POOL_WATERMARK_STAT_COUNTER_FLEX_COUNTER_GROUP ":" + sai_serialize_object_id(it.second.m_saiObjectId);

        stats_mode = "";

        if (noWmClrCapability)
        {
            if (noWmClrCapability & bitMask)
            {
                stats_mode = STATS_MODE_READ;
            }

            bitMask <<= 1;
        }

        startFlexCounterPolling(gSwitchId, key, statList, BUFFER_POOL_COUNTER_ID_LIST, stats_mode);
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

void BufferOrch::getBufferObjectsWithNonZeroProfile(vector<string> &nonZeroQueues, const string &table)
{
    for (auto &&queueRef: (*m_buffer_type_maps[table]))
    {
        for (auto &&profileRef: queueRef.second.m_objsReferencingByMe)
        {
            if (profileRef.second.find("_zero_") == std::string::npos)
            {
                SWSS_LOG_INFO("Selected key %s with profile %s", queueRef.first.c_str(), profileRef.second.c_str());
                nonZeroQueues.push_back(queueRef.first);
            }
            else
            {
                SWSS_LOG_INFO("Skipped key %s with profile %s", queueRef.first.c_str(), profileRef.second.c_str());
            }
        }
    }
}

task_process_status BufferOrch::processBufferPool(KeyOpFieldsValuesTuple &tuple)
{
    SWSS_LOG_ENTER();
    sai_status_t sai_status;
    sai_object_id_t sai_object = SAI_NULL_OBJECT_ID;
    string map_type_name = APP_BUFFER_POOL_TABLE_NAME;
    string object_name = kfvKey(tuple);
    string op = kfvOp(tuple);
    string xoff;

    SWSS_LOG_DEBUG("KEY: %s, OP: %s", object_name.c_str(), op.c_str());

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

    if (op == SET_COMMAND)
    {
        vector<sai_attribute_t> attribs;
        for (auto i = kfvFieldsValues(tuple).begin(); i != kfvFieldsValues(tuple).end(); i++)
        {
            string field = fvField(*i);
            string value = fvValue(*i);

            SWSS_LOG_DEBUG("FIELD: %s, VALUE: %s", field.c_str(), value.c_str());

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
                xoff = value;
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
            addBufferPoolWatermarkCounterIdList(sai_object);
            // Here we take the PFC watchdog approach to update the COUNTERS_DB metadata (e.g., PFC_WD_DETECTION_TIME per queue)
            // at initialization (creation and registration phase)
            // Specifically, we push the buffer pool name to oid mapping upon the creation of the oid
            // In pg and queue case, this mapping installment is deferred to FlexCounterOrch at a reception of field
            // "FLEX_COUNTER_STATUS"
            m_counterNameMapUpdater->setCounterNameMap(object_name, sai_object);
        }

        // Only publish the result when shared headroom pool is enabled and it has been successfully applied to SAI
        if (!xoff.empty())
        {
            vector<FieldValueTuple> fvs;
            fvs.emplace_back("xoff", xoff);
            SWSS_LOG_INFO("Publishing the result after applying the shared headroom pool size %s to SAI", xoff.c_str());
            m_publisher.publish(APP_BUFFER_POOL_TABLE_NAME, object_name, fvs, ReturnCode(SAI_STATUS_SUCCESS), true);
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
        m_counterNameMapUpdater->delCounterNameMap(object_name);

        vector<FieldValueTuple> fvs;
        m_publisher.publish(APP_BUFFER_POOL_TABLE_NAME, object_name, fvs, ReturnCode(SAI_STATUS_SUCCESS), true);
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

    SWSS_LOG_DEBUG("KEY: %s, OP: %s", object_name.c_str(), op.c_str());

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

    if (op == SET_COMMAND)
    {
        BufferProfileConfig cfg;
        m_bufHlpr.getBufferConfig(cfg, object_name);
        vector<sai_attribute_t> attribs;
        for (auto i = kfvFieldsValues(tuple).begin(); i != kfvFieldsValues(tuple).end(); i++)
        {
            string field = fvField(*i);
            string value = fvValue(*i);

            cfg.fieldValueMap[field] = value;

            SWSS_LOG_DEBUG("FIELD: %s, VALUE: %s", field.c_str(), value.c_str());

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
            else if (field == BUFFER_PROFILE_PACKET_DISCARD_ACTION)
            {
                attr.id = SAI_BUFFER_PROFILE_ATTR_PACKET_ADMISSION_FAIL_ACTION;

                if (value == BUFFER_PROFILE_PACKET_DISCARD_ACTION_DROP)
                {
                    attr.value.s32 = SAI_BUFFER_PROFILE_PACKET_ADMISSION_FAIL_ACTION_DROP;
                }
                else if (value == BUFFER_PROFILE_PACKET_DISCARD_ACTION_TRIM)
                {
                    attr.value.s32 = SAI_BUFFER_PROFILE_PACKET_ADMISSION_FAIL_ACTION_DROP_AND_TRIM;
                }
                else
                {
                    SWSS_LOG_ERROR("Failed to parse buffer profile(%s) field(%s): invalid value(%s)",
                        object_name.c_str(), field.c_str(), value.c_str()
                    );
                    return task_process_status::task_failed;
                }

                attribs.push_back(attr);
            }
            else
            {
                SWSS_LOG_ERROR("Unknown buffer profile field specified:%s, ignoring", field.c_str());
                continue;
            }
        }

        m_bufHlpr.parseBufferConfig(cfg);

        if (cfg.isTrimmingEligible && cfg.isTrimmingProhibited())
        {
            SWSS_LOG_ERROR(
                "Failed to configure buffer profile(%s): trimming is prohibited by dependency constraint check",
                object_name.c_str()
            );
            return task_process_status::task_failed;
        }

        if (SAI_NULL_OBJECT_ID != sai_object)
        {
            vector<sai_attribute_t> attribs_to_retry;
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
                    SWSS_LOG_NOTICE("Unable to modify buffer profile, name:%s, sai object:%" PRIx64 ", status:%d, will retry one more time", object_name.c_str(), sai_object, sai_status);
                    attribs_to_retry.push_back(attribute);
                }
            }

            for (auto &attribute : attribs_to_retry)
            {
                sai_status = sai_buffer_api->set_buffer_profile_attribute(sai_object, &attribute);
                if (SAI_STATUS_SUCCESS != sai_status)
                {
                    // A retried attribute can not be "not implemented"
                    SWSS_LOG_ERROR("Failed to modify buffer profile, name:%s, sai object:%" PRIx64 ", status:%d, will retry once", object_name.c_str(), sai_object, sai_status);
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

        // Update config state
        m_bufHlpr.setBufferConfig(object_name, cfg);

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
        m_bufHlpr.delBufferProfileConfig(object_name);
    }
    else
    {
        SWSS_LOG_ERROR("Unknown operation type %s", op.c_str());
        return task_process_status::task_invalid_entry;
    }
    return task_process_status::task_success;
}

/*
   Input sample "BUFFER_QUEUE|Ethernet4,Ethernet45|10-15" or
                "BUFFER_QUEUE|STG01-0101-0400-01T2-LC6|ASIC0|Ethernet4|10-15"
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
    bool local_port = false;
    bool counter_was_added = false;
    bool counter_needs_to_add = false;
    string old_buffer_profile_name;
    string local_port_name;

    SWSS_LOG_DEBUG("KEY: %s, OP: %s", key.c_str(), op.c_str());

    tokens = tokenize(key, delimiter);

    vector<string> port_names;
    if (gMySwitchType == "voq") 
    {
        if (tokens.size() != 4)
        {
            SWSS_LOG_ERROR("malformed key:%s. Must contain 4 tokens", key.c_str());
            return task_process_status::task_invalid_entry;
        }

        port_names = tokenize(tokens[0] + config_db_key_delimiter + tokens[1] + config_db_key_delimiter + tokens[2], list_item_delimiter);
        if (!parseIndexRange(tokens[3], range_low, range_high))
        {
            return task_process_status::task_invalid_entry;
        }

        string tmp_token_1 = tokens[1];
        string tmp_gMyAsicName = gMyAsicName;
        boost::algorithm::to_lower(tmp_token_1);
        boost::algorithm::to_lower(tmp_gMyAsicName);
        if((tokens[0] == gMyHostName) && (tmp_token_1 == tmp_gMyAsicName))
        {
           local_port = true;
           local_port_name = tokens[2];
           SWSS_LOG_INFO("System port %s is local port %d local port name %s", port_names[0].c_str(), local_port, local_port_name.c_str());
        }
    }
    else 
    {
        if (tokens.size() != 2)
        {
            SWSS_LOG_ERROR("malformed key:%s. Must contain 2 tokens", key.c_str());
            return task_process_status::task_invalid_entry;
        }

        port_names = tokenize(tokens[0], list_item_delimiter);
        if (!parseIndexRange(tokens[1], range_low, range_high))
        {
            return task_process_status::task_invalid_entry;
        }
    }

    QueueTask task;
    task.kofvs = tuple;

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

        if (doesObjectExist(m_buffer_type_maps, APP_BUFFER_QUEUE_TABLE_NAME, key, buffer_profile_field_name, old_buffer_profile_name)
            && (old_buffer_profile_name == buffer_profile_name))
        {
            if (m_partiallyAppliedQueues.find(key) == m_partiallyAppliedQueues.end())
            {
                SWSS_LOG_INFO("Skip setting buffer queue %s to %s since it is not changed", key.c_str(), buffer_profile_name.c_str());
                return task_process_status::task_success;
            }
            else
            {
                m_partiallyAppliedQueues.erase(key);
            }
        }

        SWSS_LOG_NOTICE("Set buffer queue %s to %s", key.c_str(), buffer_profile_name.c_str());

        setObjectReference(m_buffer_type_maps, APP_BUFFER_QUEUE_TABLE_NAME, key, buffer_profile_field_name, buffer_profile_name);

        // Counter operation
        counter_needs_to_add = buffer_profile_name.find("_zero_") == std::string::npos;
        SWSS_LOG_INFO("%s to create counter for %s with new profile %s", counter_needs_to_add ? "Need" : "No need", key.c_str(), buffer_profile_name.c_str());
    }
    else if (op == DEL_COMMAND)
    {
        if (!doesObjectExist(m_buffer_type_maps, APP_BUFFER_QUEUE_TABLE_NAME, key, buffer_profile_field_name, old_buffer_profile_name))
        {
            SWSS_LOG_INFO("%s doesn't not exist, don't need to notfiy SAI", key.c_str());
            need_update_sai = false;
        }
        sai_buffer_profile = SAI_NULL_OBJECT_ID;
        SWSS_LOG_NOTICE("Remove buffer queue %s", key.c_str());
        removeObject(m_buffer_type_maps, APP_BUFFER_QUEUE_TABLE_NAME, key);
        m_partiallyAppliedQueues.erase(key);
        counter_needs_to_add = false;
    }
    else
    {
        SWSS_LOG_ERROR("Unknown operation type %s", op.c_str());
        return task_process_status::task_invalid_entry;
    }

    counter_was_added = !old_buffer_profile_name.empty() && old_buffer_profile_name.find("_zero_") == std::string::npos;
    SWSS_LOG_INFO("%s to remove counter for %s with old profile %s", counter_was_added ? "Need" : "No need", key.c_str(), old_buffer_profile_name.c_str());

    sai_attribute_t attr;
    attr.id = SAI_QUEUE_ATTR_BUFFER_PROFILE_ID;
    attr.value.oid = sai_buffer_profile;
    for (string port_name : port_names)
    {
        Port port;
        SWSS_LOG_DEBUG("processing port:%s", port_name.c_str());

        if(local_port == true)
        {
            port_name = local_port_name;
        }

        if (!gPortsOrch->getPort(port_name, port))
        {
            SWSS_LOG_ERROR("Port with alias:%s not found", port_name.c_str());
            return task_process_status::task_invalid_entry;
        }

        QueueTask::PortContext portContext;
        portContext.port_name = port_name;
        portContext.local_port = local_port;
        portContext.local_port_name = local_port_name;

        for (size_t ind = range_low; ind <= range_high; ind++)
        {
            SWSS_LOG_DEBUG("processing queue:%zd", ind);
            sai_object_id_t queue_id;

            if (gMySwitchType == "voq") 
            {
                std :: vector<sai_object_id_t> queue_ids = gPortsOrch->getPortVoQIds(port);
                if (queue_ids.size() <= ind)
                {
                    SWSS_LOG_ERROR("Invalid voq index specified:%zd", ind);
                    return task_process_status::task_invalid_entry;
                }
                queue_id = queue_ids[ind];
            } 
            else
            {
                if (port.m_queue_ids.size() <= ind)
                 {
                    SWSS_LOG_ERROR("Invalid queue index specified:%zd", ind);
                    return task_process_status::task_invalid_entry;
                }
                if (port.m_queue_lock[ind])
                {
                    SWSS_LOG_WARN("Queue %zd on port %s is locked, will retry", ind, port_name.c_str());
                    m_partiallyAppliedQueues.insert(key);
                    return task_process_status::task_need_retry;
                }
                queue_id = port.m_queue_ids[ind];
            }

            if (need_update_sai)
            {
                SWSS_LOG_DEBUG("Applying buffer profile:0x%" PRIx64 " to queue index:%zd, queue sai_id:0x%" PRIx64, sai_buffer_profile, ind, queue_id);
            }

            QueueTask::QueueContext queueContext;
            queueContext.queue_id = queue_id;
            queueContext.attr = SaiAttrWrapper(SAI_OBJECT_TYPE_QUEUE, attr);
            queueContext.counter_was_added = counter_was_added;
            queueContext.counter_needs_to_add = counter_needs_to_add;
            queueContext.index = ind;
            queueContext.update_sai = need_update_sai;

            portContext.queues.emplace_back(queueContext);
        }

        task.ports.emplace_back(portContext);
    }

    m_queueBulk[op].emplace_back(task);

    return task_process_status::task_success;
}

task_process_status BufferOrch::processQueuePost(const QueueTask& task)
{
    SWSS_LOG_ENTER();

    const auto& key = kfvKey(task.kofvs);
    const auto& op = kfvOp(task.kofvs);
    const auto tokens = tokenize(key, delimiter);
    Port port;

    for (const auto& portContext: task.ports)
    {
        const auto& port_name = portContext.port_name;
        if (!gPortsOrch->getPort(port_name, port))
        {
            SWSS_LOG_ERROR("Port with alias:%s not found", port_name.c_str());
            return task_process_status::task_invalid_entry;
        }
        for (const auto& queueContext: portContext.queues)
        {
            const auto ind = queueContext.index;

            if (queueContext.update_sai)
            {
                const auto sai_status = queueContext.status;

                if (sai_status != SAI_STATUS_SUCCESS)
                {
                    SWSS_LOG_ERROR("Failed to set queue's buffer profile attribute, status:%d", sai_status);
                    task_process_status handle_status = handleSaiSetStatus(SAI_API_QUEUE, sai_status);
                    if (handle_status != task_process_status::task_success)
                    {
                        return handle_status;
                    }
                }
                // create/remove a port queue counter for the queue buffer.
                // For VOQ chassis, flexcounterorch adds the Queue Counters for all egress and VOQ queues of all front panel and system ports
                // to  the FLEX_COUNTER_DB irrespective of BUFFER_QUEUE configuration. So Port Queue counter needs to be updated only for non VOQ switch.
                else if (gMySwitchType != "voq")
                {
                    auto flexCounterOrch = gDirectory.get<FlexCounterOrch*>();
                    if (flexCounterOrch->isCreateOnlyConfigDbBuffers())
                    {
                        auto queues = tokens[1];
                        if (!queueContext.counter_was_added && queueContext.counter_needs_to_add &&
                            (flexCounterOrch->getQueueCountersState() || flexCounterOrch->getQueueWatermarkCountersState()))
                        {
                            SWSS_LOG_INFO("Creating counters for %s %zd", port_name.c_str(), ind);
                            gPortsOrch->createPortBufferQueueCounters(port, queues);
                        }
                        else if (queueContext.counter_was_added && !queueContext.counter_needs_to_add &&
                                    (flexCounterOrch->getQueueCountersState() || flexCounterOrch->getQueueWatermarkCountersState()))
                        {
                            SWSS_LOG_INFO("Removing counters for %s %zd", port_name.c_str(), ind);
                            gPortsOrch->removePortBufferQueueCounters(port, queues);
                        }
                    }
                }
            }

            /* when we apply buffer configuration we need to increase the ref counter of this port
             * or decrease the ref counter for this port when we remove buffer cfg
             * so for each priority cfg in each port we will increase/decrease the ref counter
             * also we need to know when the set command is for creating a buffer cfg or modifying buffer cfg -
             * we need to increase ref counter only on create flow.
             * so we added a map that will help us to know what was the last command for this port and priority -
             * if the last command was set command then it is a modify command and we dont need to increase the buffer counter
             * all other cases (no last command exist or del command was the last command) it means that we need to increase the ref counter */
            if (op == SET_COMMAND)
            {
                if (queue_port_flags[port_name][ind] != SET_COMMAND)
                {
                    /* if the last operation was not "set" then it's create and not modify - need to increase ref counter */
                    gPortsOrch->increasePortRefCount(port_name);
                }
            }
            else if (op == DEL_COMMAND)
            {
                if (queue_port_flags[port_name][ind] == SET_COMMAND)
		{
                    /* we need to decrease ref counter only if the last operation was "SET_COMMAND" */
                    gPortsOrch->decreasePortRefCount(port_name);
                }
            }
            else
            {
                SWSS_LOG_ERROR("operation value is not SET or DEL (op = %s)", op.c_str());
                return task_process_status::task_invalid_entry;
            }
            /* save the last command (set or delete) */
            queue_port_flags[port_name][ind] = op;

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
        for (const auto &portContext : task.ports)
        {
            const auto& port_name = portContext.port_name;
            const auto& local_port_name = portContext.local_port_name;
            const auto local_port = portContext.local_port;

            if(local_port == true)
            {
                if (gPortsOrch->isPortAdminUp(local_port_name))
                {
                    SWSS_LOG_WARN("Queue profile '%s' applied after port %s is up", key.c_str(), port_name.c_str());
                }
            }
            else
            {
                if (gPortsOrch->isPortAdminUp(port_name))
                {
                    SWSS_LOG_WARN("Queue profile '%s' applied after port %s is up", key.c_str(), port_name.c_str());
                }
            }
        }
    }

    return task_process_status::task_success;
}

void BufferOrch::processQueueBulk(Consumer& consumer)
{
    SWSS_LOG_ENTER();

    for (const auto op: {DEL_COMMAND, SET_COMMAND})
    {
        std::vector<sai_object_id_t> oids;
        std::vector<sai_attribute_t> attrs;
        std::vector<sai_status_t> statuses;

        auto& bulk = m_queueBulk[op];
        for (const auto& task: bulk)
        {
            for (const auto& port: task.ports)
            {
                for (const auto& queue: port.queues)
                {
                    if (queue.update_sai)
                    {
                        oids.push_back(queue.queue_id);
                        attrs.push_back(queue.attr.getSaiAttr());
                        statuses.push_back(SAI_STATUS_NOT_EXECUTED);
                    }
                }
            }
        }

        const auto objectCount = static_cast<uint32_t>(oids.size());

        if (objectCount > 0)
        {
            SWSS_LOG_TIMER("Set %u queues buffer profile", objectCount);

            sai_queue_api->set_queues_attribute(objectCount, oids.data(), attrs.data(),
                SAI_BULK_OP_ERROR_MODE_IGNORE_ERROR, statuses.data());
        }

        size_t i = 0;
        for (auto& task: bulk)
        {
            for (auto& port: task.ports)
            {
                for (auto& queue: port.queues)
                {
                    if (queue.update_sai)
                    {
                        queue.status = statuses[i];
                        i++;
                    }
                }
            }
        }

        for (const auto& task: bulk)
        {
            auto task_status = processQueuePost(task);
            if (task_status == task_process_status::task_need_retry)
            {
                consumer.m_toSync.emplace(kfvKey(task.kofvs), task.kofvs);
            }
        }

        bulk.clear();
    }
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
    bool counter_was_added = false;
    bool counter_needs_to_add = false;
    string old_buffer_profile_name;

    SWSS_LOG_DEBUG("KEY: %s, OP: %s", key.c_str(), op.c_str());

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

    PriorityGroupTask task;
    task.kofvs = tuple;

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

        if (doesObjectExist(m_buffer_type_maps, APP_BUFFER_PG_TABLE_NAME, key, buffer_profile_field_name, old_buffer_profile_name)
            && (old_buffer_profile_name == buffer_profile_name))
        {
            SWSS_LOG_INFO("Skip setting buffer priority group %s to %s since it is not changed", key.c_str(), buffer_profile_name.c_str());
            return task_process_status::task_success;
        }

        BufferPriorityGroupConfig cfg;
        m_bufHlpr.getBufferConfig(cfg, key);

        for (const auto &cit : kfvFieldsValues(tuple))
        {
            auto field = fvField(cit);
            auto value = fvValue(cit);

            SWSS_LOG_DEBUG("FIELD: %s, VALUE: %s", field.c_str(), value.c_str());

            cfg.fieldValueMap[field] = value;
        }

        m_bufHlpr.parseBufferConfig(cfg);

        if (cfg.profile.is_set)
        {
            BufferProfileConfig profCfg;

            if (m_bufHlpr.getBufferConfig(profCfg, cfg.profile.value))
            {
                if (profCfg.isTrimmingEligible)
                {
                    SWSS_LOG_ERROR(
                        "Failed to configure ingress priority group(%s): buffer profile(%s) is trimming eligible",
                        key.c_str(), cfg.profile.value.c_str()
                    );
                    return task_process_status::task_failed;
                }
            }
        }

        m_bufHlpr.setBufferConfig(key, cfg);

        SWSS_LOG_NOTICE("Set buffer PG %s to %s", key.c_str(), buffer_profile_name.c_str());

        setObjectReference(m_buffer_type_maps, APP_BUFFER_PG_TABLE_NAME, key, buffer_profile_field_name, buffer_profile_name);

        // Counter operation
        counter_needs_to_add = buffer_profile_name.find("_zero_") == std::string::npos;
        SWSS_LOG_INFO("%s to create counter for priority group %s with new profile %s", counter_needs_to_add ? "Need" : "No need", key.c_str(), buffer_profile_name.c_str());
    }
    else if (op == DEL_COMMAND)
    {
        if (!doesObjectExist(m_buffer_type_maps, APP_BUFFER_PG_TABLE_NAME, key, buffer_profile_field_name, old_buffer_profile_name))
        {
            SWSS_LOG_INFO("%s doesn't not exist, don't need to notfiy SAI", key.c_str());
            need_update_sai = false;
        }
        sai_buffer_profile = SAI_NULL_OBJECT_ID;
        SWSS_LOG_NOTICE("Remove buffer PG %s", key.c_str());
        removeObject(m_buffer_type_maps, APP_BUFFER_PG_TABLE_NAME, key);
        m_bufHlpr.delBufferPriorityGroupConfig(key);
    }
    else
    {
        SWSS_LOG_ERROR("Unknown operation type %s", op.c_str());
        return task_process_status::task_invalid_entry;
    }

    counter_was_added = !old_buffer_profile_name.empty() && old_buffer_profile_name.find("_zero_") == std::string::npos;
    SWSS_LOG_INFO("%s to remove counter for priority group %s with old profile %s", counter_was_added ? "Need" : "No need", key.c_str(), old_buffer_profile_name.c_str());

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

        PriorityGroupTask::PortContext portContext;
        portContext.port_name = port_name;

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
                sai_object_id_t pg_id = port.m_priority_group_ids[ind];
                if (need_update_sai)
                {
                    SWSS_LOG_DEBUG("Applying buffer profile:0x%" PRIx64 " to port:%s pg index:%zd, pg sai_id:0x%" PRIx64, sai_buffer_profile, port_name.c_str(), ind, pg_id);
                }

                PriorityGroupTask::PgContext pgContext;
                pgContext.pg_id = pg_id;
                pgContext.attr = SaiAttrWrapper(SAI_OBJECT_TYPE_INGRESS_PRIORITY_GROUP, attr);
                pgContext.counter_was_added = counter_was_added;
                pgContext.counter_needs_to_add = counter_needs_to_add;
                pgContext.index = ind;
                pgContext.update_sai = need_update_sai;

                portContext.pgs.emplace_back(pgContext);
            }
        }

        task.ports.emplace_back(portContext);
    }

    m_priorityGroupBulk[op].emplace_back(task);

    return task_process_status::task_success;
}

task_process_status BufferOrch::processPriorityGroupPost(const PriorityGroupTask& task)
{
    SWSS_LOG_ENTER();

    const auto& key = kfvKey(task.kofvs);
    const auto& op = kfvOp(task.kofvs);
    const auto tokens = tokenize(key, delimiter);
    Port port;

    for (const auto& portContext: task.ports)
    {
        const auto& port_name = portContext.port_name;
        if (!gPortsOrch->getPort(port_name, port))
        {
            SWSS_LOG_ERROR("Port with alias:%s not found", port_name.c_str());
            return task_process_status::task_invalid_entry;
        }
        for (const auto& pg: portContext.pgs)
        {
            const auto ind = pg.index;

            if (pg.update_sai)
            {
                const auto sai_status = pg.status;

                if (sai_status != SAI_STATUS_SUCCESS)
                {
                    SWSS_LOG_ERROR("Failed to set port:%s pg:%zd buffer profile attribute, status:%d", port_name.c_str(), ind, sai_status);
                    task_process_status handle_status = handleSaiSetStatus(SAI_API_BUFFER, sai_status);
                    if (handle_status != task_process_status::task_success)
                    {
                        return handle_status;
                    }
                }
                // create or remove a port PG counter for the PG buffer
                else
                {
                    auto flexCounterOrch = gDirectory.get<FlexCounterOrch*>();
                    if (flexCounterOrch->isCreateOnlyConfigDbBuffers())
                    {
                        auto pgs = tokens[1];
                        if (!pg.counter_was_added && pg.counter_needs_to_add &&
                            (flexCounterOrch->getPgCountersState() || flexCounterOrch->getPgWatermarkCountersState()))
                        {
                            SWSS_LOG_INFO("Creating counters for priority group %s %zd", port_name.c_str(), ind);
                            gPortsOrch->createPortBufferPgCounters(port, pgs);
                        }
                        else if (pg.counter_was_added && !pg.counter_needs_to_add &&
                                    (flexCounterOrch->getPgCountersState() || flexCounterOrch->getPgWatermarkCountersState()))
                        {
                            SWSS_LOG_INFO("Removing counters for priority group %s %zd", port_name.c_str(), ind);
                            gPortsOrch->removePortBufferPgCounters(port, pgs);
                        }
                    }
                }
            }

            /* when we apply buffer configuration we need to increase the ref counter of this port
             * or decrease the ref counter for this port when we remove buffer cfg
             * so for each priority cfg in each port we will increase/decrease the ref counter
             * also we need to know when the set command is for creating a buffer cfg or modifying buffer cfg -
             * we need to increase ref counter only on create flow.
             * so we added a map that will help us to know what was the last command for this port and priority -
             * if the last command was set command then it is a modify command and we dont need to increase the buffer counter
             * all other cases (no last command exist or del command was the last command) it means that we need to increase the ref counter */
            if (op == SET_COMMAND)
            {
                if (pg_port_flags[port_name][ind] != SET_COMMAND)
                {
                    /* if the last operation was not "set" then it's create and not modify - need to increase ref counter */
                    gPortsOrch->increasePortRefCount(port_name);
                }
            }
            else if (op == DEL_COMMAND)
            {
                if (pg_port_flags[port_name][ind] == SET_COMMAND)
                {
                    /* we need to decrease ref counter only if the last operation was "SET_COMMAND" */
                    gPortsOrch->decreasePortRefCount(port_name);
                }
            }
            else
            {
                SWSS_LOG_ERROR("operation value is not SET or DEL (op = %s)", op.c_str());
                return task_process_status::task_invalid_entry;
            }
            /* save the last command (set or delete) */
            pg_port_flags[port_name][ind] = op;
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
        for (const auto &portContext : task.ports)
        {
            const auto& port_name = portContext.port_name;
            if (gPortsOrch->isPortAdminUp(port_name))
            {
                SWSS_LOG_WARN("PG profile '%s' applied after port %s is up", key.c_str(), port_name.c_str());
            }
        }
    }

    return task_process_status::task_success;
}

void BufferOrch::processPriorityGroupBulk(Consumer& consumer)
{
    SWSS_LOG_ENTER();

    for (const auto op: {DEL_COMMAND, SET_COMMAND})
    {
        std::vector<sai_object_id_t> oids;
        std::vector<sai_attribute_t> attrs;
        std::vector<sai_status_t> statuses;

        auto& bulk = m_priorityGroupBulk[op];
        for (const auto& task: bulk)
        {
            for (const auto& port: task.ports)
            {
                for (const auto& pg: port.pgs)
                {
                    if (pg.update_sai)
                    {
                        oids.push_back(pg.pg_id);
                        attrs.push_back(pg.attr.getSaiAttr());
                        statuses.push_back(SAI_STATUS_NOT_EXECUTED);
                    }
                }
            }
        }

        const auto objectCount = static_cast<uint32_t>(oids.size());

        if (objectCount > 0)
        {
            SWSS_LOG_TIMER("Set %u ingress priority groups buffer profile", objectCount);

            sai_buffer_api->set_ingress_priority_groups_attribute(objectCount, oids.data(), attrs.data(),
                SAI_BULK_OP_ERROR_MODE_IGNORE_ERROR, statuses.data());
        }

        size_t i = 0;
        for (auto& task: bulk)
        {
            for (auto& port: task.ports)
            {
                for (auto& pg: port.pgs)
                {
                    if (pg.update_sai)
                    {
                        pg.status = statuses[i];
                        i++;
                    }
                }
            }
        }

        for (const auto& task: bulk)
        {
            auto task_status = processPriorityGroupPost(task);
            if (task_status == task_process_status::task_need_retry)
            {
                consumer.m_toSync.emplace(kfvKey(task.kofvs), task.kofvs);
            }
        }

        bulk.clear();
    }
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

    SWSS_LOG_DEBUG("KEY: %s, OP: %s", key.c_str(), op.c_str());

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

        string old_profile_name_list;
        if (doesObjectExist(m_buffer_type_maps, APP_BUFFER_PORT_INGRESS_PROFILE_LIST_NAME, key, buffer_profile_list_field_name, old_profile_name_list)
            && (old_profile_name_list == profile_name_list))
        {
            SWSS_LOG_INFO("Skip setting buffer ingress profile list %s to %s since it is not changed", key.c_str(), profile_name_list.c_str());
            return task_process_status::task_success;
        }

        IngressBufferProfileListConfig cfg;
        m_bufHlpr.getBufferConfig(cfg, key);

        for (const auto &cit : kfvFieldsValues(tuple))
        {
            auto field = fvField(cit);
            auto value = fvValue(cit);

            SWSS_LOG_DEBUG("FIELD: %s, VALUE: %s", field.c_str(), value.c_str());

            cfg.fieldValueMap[field] = value;
        }

        m_bufHlpr.parseBufferConfig(cfg);

        if (cfg.profile_list.is_set)
        {
            for (const auto &cit : cfg.profile_list.value)
            {
                BufferProfileConfig profCfg;

                if (m_bufHlpr.getBufferConfig(profCfg, cit))
                {
                    if (profCfg.isTrimmingEligible)
                    {
                        SWSS_LOG_ERROR(
                            "Failed to configure ingress buffer profile list(%s): buffer profile(%s) is trimming eligible",
                            key.c_str(), cit.c_str()
                        );
                        return task_process_status::task_failed;
                    }
                }
            }
        }

        m_bufHlpr.setBufferConfig(key, cfg);

        setObjectReference(m_buffer_type_maps, APP_BUFFER_PORT_INGRESS_PROFILE_LIST_NAME, key, buffer_profile_list_field_name, profile_name_list);

        attr.value.objlist.count = (uint32_t)profile_list.size();
        attr.value.objlist.list = profile_list.data();
    }
    else if (op == DEL_COMMAND)
    {
        SWSS_LOG_NOTICE("%s has been removed from BUFFER_PORT_INGRESS_PROFILE_LIST_TABLE", key.c_str());
        removeObject(m_buffer_type_maps, APP_BUFFER_PORT_INGRESS_PROFILE_LIST_NAME, key);
        m_bufHlpr.delIngressBufferProfileListConfig(key);
        attr.value.objlist.count = 0;
        attr.value.objlist.list = profile_list.data();
    }
    else
    {
        SWSS_LOG_ERROR("Unknown command %s when handling BUFFER_PORT_INGRESS_PROFILE_LIST_TABLE key %s", op.c_str(), key.c_str());
    }

    PortBufferProfileListTask task;
    task.kofvs = tuple;

    for (string port_name : port_names)
    {
        if (!gPortsOrch->getPort(port_name, port))
        {
            SWSS_LOG_ERROR("Port with alias:%s not found", port_name.c_str());
            return task_process_status::task_invalid_entry;
        }

        task.ports.emplace_back(PortBufferProfileListTask::PortContext{port_name, port.m_port_id, SaiAttrWrapper{SAI_OBJECT_TYPE_PORT, attr}, SAI_STATUS_NOT_EXECUTED});
    }

    m_portIngressBufferProfileListBulk[op].push_back(task);

    return task_process_status::task_success;
}

task_process_status BufferOrch::processIngressBufferProfileListPost(const PortBufferProfileListTask& task)
{
    for (const auto& portContext: task.ports)
    {
        const auto& port_name = portContext.port_name;
        sai_status_t sai_status = portContext.status;
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

void BufferOrch::processIngressBufferProfileListBulk(Consumer& consumer)
{
    SWSS_LOG_ENTER();

    for (const auto op: {DEL_COMMAND, SET_COMMAND})
    {
        std::vector<sai_object_id_t> oids;
        std::vector<sai_attribute_t> attrs;
        std::vector<sai_status_t> statuses;

        auto& bulk = m_portIngressBufferProfileListBulk[op];
        for (const auto& task: bulk)
        {
            for (const auto& port: task.ports)
            {
                oids.push_back(port.port_oid);
                attrs.push_back(port.attr.getSaiAttr());
                statuses.push_back(SAI_STATUS_NOT_EXECUTED);
            }
        }

        const auto objectCount = static_cast<uint32_t>(oids.size());

        if (objectCount > 0)
        {
            SWSS_LOG_TIMER("Set %u ports ingress buffer profile list", objectCount);

            sai_port_api->set_ports_attribute(objectCount, oids.data(), attrs.data(),
                SAI_BULK_OP_ERROR_MODE_IGNORE_ERROR, statuses.data());
        }

        size_t i = 0;
        for (auto& task: bulk)
        {
            for (auto& port: task.ports)
            {
                port.status = statuses[i];
                i++;
            }
        }

        for (const auto& task: bulk)
        {
            auto task_status = processIngressBufferProfileListPost(task);
            if (task_status == task_process_status::task_need_retry)
            {
                consumer.m_toSync.emplace(kfvKey(task.kofvs), task.kofvs);
            }
        }

        bulk.clear();
    }
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

    SWSS_LOG_DEBUG("KEY: %s, OP: %s", key.c_str(), op.c_str());

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

        string old_profile_name_list;
        if (doesObjectExist(m_buffer_type_maps, APP_BUFFER_PORT_EGRESS_PROFILE_LIST_NAME, key, buffer_profile_list_field_name, old_profile_name_list)
            && (old_profile_name_list == profile_name_list))
        {
            SWSS_LOG_INFO("Skip setting buffer egress profile list %s to %s since it is not changed", key.c_str(), profile_name_list.c_str());
            return task_process_status::task_success;
        }

        EgressBufferProfileListConfig cfg;
        m_bufHlpr.getBufferConfig(cfg, key);

        for (const auto &cit : kfvFieldsValues(tuple))
        {
            auto field = fvField(cit);
            auto value = fvValue(cit);

            SWSS_LOG_DEBUG("FIELD: %s, VALUE: %s", field.c_str(), value.c_str());

            cfg.fieldValueMap[field] = value;
        }

        m_bufHlpr.parseBufferConfig(cfg);

        if (cfg.profile_list.is_set)
        {
            for (const auto &cit : cfg.profile_list.value)
            {
                BufferProfileConfig profCfg;

                if (m_bufHlpr.getBufferConfig(profCfg, cit))
                {
                    if (profCfg.isTrimmingEligible)
                    {
                        SWSS_LOG_ERROR(
                            "Failed to configure egress buffer profile list(%s): buffer profile(%s) is trimming eligible",
                            key.c_str(), cit.c_str()
                        );
                        return task_process_status::task_failed;
                    }
                }
            }
        }

        m_bufHlpr.setBufferConfig(key, cfg);

        setObjectReference(m_buffer_type_maps, APP_BUFFER_PORT_EGRESS_PROFILE_LIST_NAME, key, buffer_profile_list_field_name, profile_name_list);

        attr.value.objlist.count = (uint32_t)profile_list.size();
        attr.value.objlist.list = profile_list.data();
    }
    else if (op == DEL_COMMAND)
    {
        SWSS_LOG_NOTICE("%s has been removed from BUFFER_PORT_EGRESS_PROFILE_LIST_TABLE", key.c_str());
        removeObject(m_buffer_type_maps, APP_BUFFER_PORT_EGRESS_PROFILE_LIST_NAME, key);
        m_bufHlpr.delEgressBufferProfileListConfig(key);
        attr.value.objlist.count = 0;
        attr.value.objlist.list = profile_list.data();
    }
    else
    {
        SWSS_LOG_ERROR("Unknown command %s when handling BUFFER_PORT_EGRESS_PROFILE_LIST_TABLE key %s", op.c_str(), key.c_str());
    }

    PortBufferProfileListTask task;
    task.kofvs = tuple;

    for (string port_name : port_names)
    {
        if (!gPortsOrch->getPort(port_name, port))
        {
            SWSS_LOG_ERROR("Port with alias:%s not found", port_name.c_str());
            return task_process_status::task_invalid_entry;
        }

        task.ports.emplace_back(PortBufferProfileListTask::PortContext{port_name, port.m_port_id, SaiAttrWrapper{SAI_OBJECT_TYPE_PORT, attr}, SAI_STATUS_NOT_EXECUTED});
    }

    m_portEgressBufferProfileListBulk[op].push_back(task);

    return task_process_status::task_success;
}

task_process_status BufferOrch::processEgressBufferProfileListPost(const PortBufferProfileListTask& task)
{
    for (const auto& portContext: task.ports)
    {
        const auto& port_name = portContext.port_name;
        sai_status_t sai_status = portContext.status;
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

void BufferOrch::processEgressBufferProfileListBulk(Consumer& consumer)
{
    SWSS_LOG_ENTER();

    for (const auto op: {DEL_COMMAND, SET_COMMAND})
    {
        std::vector<sai_object_id_t> oids;
        std::vector<sai_attribute_t> attrs;
        std::vector<sai_status_t> statuses;

        auto& bulk = m_portEgressBufferProfileListBulk[op];
        for (const auto& task: bulk)
        {
            for (const auto& port: task.ports)
            {
                oids.push_back(port.port_oid);
                attrs.push_back(port.attr.getSaiAttr());
                statuses.push_back(SAI_STATUS_NOT_EXECUTED);
            }
        }

        const auto objectCount = static_cast<uint32_t>(oids.size());

        if (objectCount > 0)
        {
            SWSS_LOG_TIMER("Set %u ports egress buffer profile list", objectCount);

            sai_port_api->set_ports_attribute(objectCount, oids.data(), attrs.data(),
                SAI_BULK_OP_ERROR_MODE_IGNORE_ERROR, statuses.data());
        }

        size_t i = 0;
        for (auto& task: bulk)
        {
            for (auto& port: task.ports)
            {
                port.status = statuses[i];
                i++;
            }
        }

        for (const auto& task: bulk)
        {
            auto task_status = processEgressBufferProfileListPost(task);
            if (task_status == task_process_status::task_need_retry)
            {
                consumer.m_toSync.emplace(kfvKey(task.kofvs), task.kofvs);
            }
        }

        bulk.clear();
    }
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
    gPortsOrch->flushCounters();
}

void BufferOrch::doTask(Consumer &consumer)
{
    SWSS_LOG_ENTER();

    if (gMySwitchType == "voq")
    {
        if(!gPortsOrch->isInitDone()) 
        {
            SWSS_LOG_INFO("Buffer task for %s can't be executed ahead of port config done", consumer.getTableName().c_str());
            return;
        }
    }
    else if (!gPortsOrch->isConfigDone())
    {
        SWSS_LOG_INFO("Buffer task for %s can't be executed ahead of port config done", consumer.getTableName().c_str());
        return;
    }

    auto map_type_name = consumer.getTableName();

    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
        /* Make sure the handler is initialized for the task */
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

    if (m_bufferFlushHandlerMap.find(map_type_name) != m_bufferFlushHandlerMap.end())
    {
        (this->*(m_bufferFlushHandlerMap[map_type_name]))(consumer);
    }

    gPortsOrch->flushCounters();
}
