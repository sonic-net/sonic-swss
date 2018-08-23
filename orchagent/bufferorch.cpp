#include "tokenize.h"

#include "bufferorch.h"
#include "logger.h"

#include <sstream>
#include <iostream>

extern sai_port_api_t *sai_port_api;
extern sai_queue_api_t *sai_queue_api;
extern sai_switch_api_t *sai_switch_api;
extern sai_buffer_api_t *sai_buffer_api;

extern PortsOrch *gPortsOrch;
extern sai_object_id_t gSwitchId;

using namespace std;

type_map BufferOrch::m_buffer_type_maps = {
    {CFG_BUFFER_POOL_TABLE_NAME, new object_map()},
    {CFG_BUFFER_PROFILE_TABLE_NAME, new object_map()},
    {CFG_BUFFER_QUEUE_TABLE_NAME, new object_map()},
    {CFG_BUFFER_PG_TABLE_NAME, new object_map()},
    {CFG_BUFFER_PORT_INGRESS_PROFILE_LIST_NAME, new object_map()},
    {CFG_BUFFER_PORT_EGRESS_PROFILE_LIST_NAME, new object_map()}
};

BufferOrch::BufferOrch(DBConnector *db, vector<string> &tableNames) : Orch(db, tableNames)
{
    SWSS_LOG_ENTER();
    initTableHandlers();
    initBufferReadyLists(db);
};

void BufferOrch::initTableHandlers()
{
    SWSS_LOG_ENTER();
    m_bufferHandlerMap.insert(buffer_handler_pair(CFG_BUFFER_POOL_TABLE_NAME, &BufferOrch::processBufferPool));
    m_bufferHandlerMap.insert(buffer_handler_pair(CFG_BUFFER_PROFILE_TABLE_NAME, &BufferOrch::processBufferProfile));
    m_bufferHandlerMap.insert(buffer_handler_pair(CFG_BUFFER_QUEUE_TABLE_NAME, &BufferOrch::processQueue));
    m_bufferHandlerMap.insert(buffer_handler_pair(CFG_BUFFER_PG_TABLE_NAME, &BufferOrch::processPriorityGroup));
    m_bufferHandlerMap.insert(buffer_handler_pair(CFG_BUFFER_PORT_INGRESS_PROFILE_LIST_NAME, &BufferOrch::processIngressBufferProfileList));
    m_bufferHandlerMap.insert(buffer_handler_pair(CFG_BUFFER_PORT_EGRESS_PROFILE_LIST_NAME, &BufferOrch::processEgressBufferProfileList));
}

void BufferOrch::initBufferReadyLists(DBConnector *db)
{
    SWSS_LOG_ENTER();

    Table pg_table(db, CFG_BUFFER_PG_TABLE_NAME);
    initBufferReadyList(pg_table);

    Table queue_table(db, CFG_BUFFER_QUEUE_TABLE_NAME);
    initBufferReadyList(queue_table);
}

void BufferOrch::initBufferReadyList(Table& table)
{
    SWSS_LOG_ENTER();

    std::vector<std::string> keys;
    table.getKeys(keys);

    // populate the lists with buffer configuration information
    for (const auto& key: keys)
    {
        m_ready_list[key] = false;

        auto tokens = tokenize(key, config_db_key_delimiter);
        if (tokens.size() != 2)
        {
            SWSS_LOG_ERROR("Wrong format of a table '%s' key '%s'. Skip it", table.getTableName().c_str(), key.c_str());
            continue;
        }

        auto port_names = tokenize(tokens[0], list_item_delimiter);

        for(const auto& port_name: port_names)
        {
            m_port_ready_list_ref[port_name].push_back(key);
        }
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

task_process_status BufferOrch::processBufferPool(Consumer &consumer)
{
    SWSS_LOG_ENTER();
    sai_status_t sai_status;
    sai_object_id_t sai_object = SAI_NULL_OBJECT_ID;
    auto it = consumer.m_toSync.begin();
    KeyOpFieldsValuesTuple tuple = it->second;
    string map_type_name = consumer.getTableName();
    string object_name = kfvKey(tuple);
    string op = kfvOp(tuple);

    SWSS_LOG_DEBUG("object name:%s", object_name.c_str());
    if (m_buffer_type_maps[map_type_name]->find(object_name) != m_buffer_type_maps[map_type_name]->end())
    {
        sai_object = (*(m_buffer_type_maps[map_type_name]))[object_name];
        SWSS_LOG_DEBUG("found existing object:%s of type:%s", object_name.c_str(), map_type_name.c_str());
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
                attr.value.u32 = (uint32_t)stoul(value);
                attribs.push_back(attr);
            }
            else if (field == buffer_pool_type_field_name)
            {
                string type = value;
                if (type == buffer_value_ingress)
                {
                    attr.value.u32 = SAI_BUFFER_POOL_TYPE_INGRESS;
                }
                else if (type == buffer_value_egress)
                {
                    attr.value.u32 = SAI_BUFFER_POOL_TYPE_EGRESS;
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
                attr.value.u32 = (uint32_t)stoul(value);
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
            sai_status = sai_buffer_api->set_buffer_pool_attribute(sai_object, &attribs[0]);
            if (SAI_STATUS_SUCCESS != sai_status)
            {
                SWSS_LOG_ERROR("Failed to modify buffer pool, name:%s, sai object:%lx, status:%d", object_name.c_str(), sai_object, sai_status);
                return task_process_status::task_failed;
            }
            SWSS_LOG_DEBUG("Modified existing pool:%lx, type:%s name:%s ", sai_object, map_type_name.c_str(), object_name.c_str());
        }
        else
        {
            sai_status = sai_buffer_api->create_buffer_pool(&sai_object, gSwitchId, (uint32_t)attribs.size(), attribs.data());
            if (SAI_STATUS_SUCCESS != sai_status)
            {
                SWSS_LOG_ERROR("Failed to create buffer pool %s with type %s, rv:%d", object_name.c_str(), map_type_name.c_str(), sai_status);
                return task_process_status::task_failed;
            }
            (*(m_buffer_type_maps[map_type_name]))[object_name] = sai_object;
            SWSS_LOG_NOTICE("Created buffer pool %s with type %s", object_name.c_str(), map_type_name.c_str());
        }
    }
    else if (op == DEL_COMMAND)
    {
        sai_status = sai_buffer_api->remove_buffer_pool(sai_object);
        if (SAI_STATUS_SUCCESS != sai_status)
        {
            SWSS_LOG_ERROR("Failed to remove buffer pool %s with type %s, rv:%d", object_name.c_str(), map_type_name.c_str(), sai_status);
            return task_process_status::task_failed;
        }
        SWSS_LOG_NOTICE("Removed buffer pool %s with type %s", object_name.c_str(), map_type_name.c_str());
        auto it_to_delete = (m_buffer_type_maps[map_type_name])->find(object_name);
        (m_buffer_type_maps[map_type_name])->erase(it_to_delete);
    }
    else
    {
        SWSS_LOG_ERROR("Unknown operation type %s", op.c_str());
        return task_process_status::task_invalid_entry;
    }
    return task_process_status::task_success;
}

task_process_status BufferOrch::processBufferProfile(Consumer &consumer)
{
    SWSS_LOG_ENTER();
    sai_status_t sai_status;
    sai_object_id_t sai_object = SAI_NULL_OBJECT_ID;
    auto it = consumer.m_toSync.begin();
    KeyOpFieldsValuesTuple tuple = it->second;
    string map_type_name = consumer.getTableName();
    string object_name = kfvKey(tuple);
    string op = kfvOp(tuple);

    SWSS_LOG_DEBUG("object name:%s", object_name.c_str());
    if (m_buffer_type_maps[map_type_name]->find(object_name) != m_buffer_type_maps[map_type_name]->end())
    {
        sai_object = (*(m_buffer_type_maps[map_type_name]))[object_name];
        SWSS_LOG_DEBUG("found existing object:%s of type:%s", object_name.c_str(), map_type_name.c_str());
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
                ref_resolve_status resolve_result = resolveFieldRefValue(m_buffer_type_maps, buffer_pool_field_name, tuple, sai_pool);
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
                attr.id = SAI_BUFFER_PROFILE_ATTR_POOL_ID;
                attr.value.oid = sai_pool;
                attribs.push_back(attr);
            }
            else if (field == buffer_xon_field_name)
            {
                attr.value.u32 = (uint32_t)stoul(value);
                attr.id = SAI_BUFFER_PROFILE_ATTR_XON_TH;
                attribs.push_back(attr);
            }
            else if (field == buffer_xon_offset_field_name)
            {
                attr.value.u32 = (uint32_t)stoul(value);
                attr.id = SAI_BUFFER_PROFILE_ATTR_XON_OFFSET_TH;
                attribs.push_back(attr);
            }
            else if (field == buffer_xoff_field_name)
            {
                attr.value.u32 = (uint32_t)stoul(value);
                attr.id = SAI_BUFFER_PROFILE_ATTR_XOFF_TH;
                attribs.push_back(attr);
            }
            else if (field == buffer_size_field_name)
            {
                attr.id = SAI_BUFFER_PROFILE_ATTR_BUFFER_SIZE;
                attr.value.u32 = (uint32_t)stoul(value);
                attribs.push_back(attr);
            }
            else if (field == buffer_dynamic_th_field_name)
            {
                attr.id = SAI_BUFFER_PROFILE_ATTR_THRESHOLD_MODE;
                attr.value.s32 = SAI_BUFFER_PROFILE_THRESHOLD_MODE_DYNAMIC;
                attribs.push_back(attr);

                attr.id = SAI_BUFFER_PROFILE_ATTR_SHARED_DYNAMIC_TH;
                attr.value.s8 = (sai_int8_t)stol(value);
                attribs.push_back(attr);
            }
            else if (field == buffer_static_th_field_name)
            {
                attr.id = SAI_BUFFER_PROFILE_ATTR_THRESHOLD_MODE;
                attr.value.s32 = SAI_BUFFER_PROFILE_THRESHOLD_MODE_STATIC;
                attribs.push_back(attr);

                attr.id = SAI_BUFFER_PROFILE_ATTR_SHARED_STATIC_TH;
                attr.value.u32 = (uint32_t)stoul(value);
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
            SWSS_LOG_DEBUG("Modifying existing sai object:%lx ", sai_object);
            sai_status = sai_buffer_api->set_buffer_profile_attribute(sai_object, &attribs[0]);
            if (SAI_STATUS_SUCCESS != sai_status)
            {
                SWSS_LOG_ERROR("Failed to modify buffer profile, name:%s, sai object:%lx, status:%d", object_name.c_str(), sai_object, sai_status);
                return task_process_status::task_failed;
            }
        }
        else
        {
            sai_status = sai_buffer_api->create_buffer_profile(&sai_object, gSwitchId, (uint32_t)attribs.size(), attribs.data());
            if (SAI_STATUS_SUCCESS != sai_status)
            {
                SWSS_LOG_ERROR("Failed to create buffer profile %s with type %s, rv:%d", object_name.c_str(), map_type_name.c_str(), sai_status);
                return task_process_status::task_failed;
            }
            (*(m_buffer_type_maps[map_type_name]))[object_name] = sai_object;
            SWSS_LOG_NOTICE("Created buffer profile %s with type %s", object_name.c_str(), map_type_name.c_str());
        }
    }
    else if (op == DEL_COMMAND)
    {
        sai_status = sai_buffer_api->remove_buffer_profile(sai_object);
        if (SAI_STATUS_SUCCESS != sai_status)
        {
            SWSS_LOG_ERROR("Failed to remove buffer profile %s with type %s, rv:%d", object_name.c_str(), map_type_name.c_str(), sai_status);
            return task_process_status::task_failed;
        }
        SWSS_LOG_NOTICE("Remove buffer profile %s with type %s", object_name.c_str(), map_type_name.c_str());
        auto it_to_delete = (m_buffer_type_maps[map_type_name])->find(object_name);
        (m_buffer_type_maps[map_type_name])->erase(it_to_delete);
    }
    else
    {
        SWSS_LOG_ERROR("Unknown operation type %s", op.c_str());
        return task_process_status::task_invalid_entry;
    }
    return task_process_status::task_success;
}

/*
Input sample "BUFFER_QUEUE_TABLE:Ethernet4,Ethernet45:10-15"
*/
task_process_status BufferOrch::processQueue(Consumer &consumer)
{
    SWSS_LOG_ENTER();
    auto it = consumer.m_toSync.begin();
    KeyOpFieldsValuesTuple tuple = it->second;
    sai_object_id_t sai_buffer_profile;
    const string key = kfvKey(tuple);
    string op = kfvOp(tuple);
    vector<string> tokens;
    sai_uint32_t range_low, range_high;

    SWSS_LOG_DEBUG("Processing:%s", key.c_str());
    tokens = tokenize(key, config_db_key_delimiter);
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
    ref_resolve_status  resolve_result = resolveFieldRefValue(m_buffer_type_maps, buffer_profile_field_name, tuple, sai_buffer_profile);
    if (ref_resolve_status::success != resolve_result)
    {
        if(ref_resolve_status::not_resolved == resolve_result)
        {
            SWSS_LOG_INFO("Missing or invalid queue buffer profile reference specified");
            return task_process_status::task_need_retry;
        }
        SWSS_LOG_ERROR("Resolving queue profile reference failed");
        return task_process_status::task_failed;
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
            sai_object_id_t queue_id;
            SWSS_LOG_DEBUG("processing queue:%zd", ind);
            if (port.m_queue_ids.size() <= ind)
            {
                SWSS_LOG_ERROR("Invalid queue index specified:%zd", ind);
                return task_process_status::task_invalid_entry;
            }
            queue_id = port.m_queue_ids[ind];
            SWSS_LOG_DEBUG("Applying buffer profile:0x%lx to queue index:%zd, queue sai_id:0x%lx", sai_buffer_profile, ind, queue_id);
            sai_status_t sai_status = sai_queue_api->set_queue_attribute(queue_id, &attr);
            if (sai_status != SAI_STATUS_SUCCESS)
            {
                SWSS_LOG_ERROR("Failed to set queue's buffer profile attribute, status:%d", sai_status);
                return task_process_status::task_failed;
            }
        }
    }

    if (m_ready_list.find(key) != m_ready_list.end())
    {
        m_ready_list[key] = true;
    }
    else
    {
        SWSS_LOG_ERROR("Queue profile '%s' was inserted after BufferOrch init", key.c_str());
    }

    return task_process_status::task_success;
}

/*
Input sample "BUFFER_PG_TABLE|Ethernet4,Ethernet45|10-15"
*/
task_process_status BufferOrch::processPriorityGroup(Consumer &consumer)
{
    SWSS_LOG_ENTER();
    auto it = consumer.m_toSync.begin();
    KeyOpFieldsValuesTuple tuple = it->second;
    sai_object_id_t sai_buffer_profile;
    const string key = kfvKey(tuple);
    string op = kfvOp(tuple);
    vector<string> tokens;
    sai_uint32_t range_low, range_high;

    if (op != SET_COMMAND)
    {
        return task_process_status::task_success;
    }

    SWSS_LOG_DEBUG("processing:%s", key.c_str());
    tokens = tokenize(key, config_db_key_delimiter);
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
    ref_resolve_status  resolve_result = resolveFieldRefValue(m_buffer_type_maps, buffer_profile_field_name, tuple, sai_buffer_profile);
    if (ref_resolve_status::success != resolve_result)
    {
        if(ref_resolve_status::not_resolved == resolve_result)
        {
            SWSS_LOG_INFO("Missing or invalid pg profile reference specified");
            return task_process_status::task_need_retry;
        }
        SWSS_LOG_ERROR("Resolving pg profile reference failed");
        return task_process_status::task_failed;
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
            sai_object_id_t pg_id;
            SWSS_LOG_DEBUG("processing pg:%zd", ind);
            if (port.m_priority_group_ids.size() <= ind)
            {
                SWSS_LOG_ERROR("Invalid pg index specified:%zd", ind);
                return task_process_status::task_invalid_entry;
            }
            pg_id = port.m_priority_group_ids[ind];
            SWSS_LOG_DEBUG("Applying buffer profile:0x%lx to port:%s pg index:%zd, pg sai_id:0x%lx", sai_buffer_profile, port_name.c_str(), ind, pg_id);
            sai_status_t sai_status = sai_buffer_api->set_ingress_priority_group_attribute(pg_id, &attr);
            if (sai_status != SAI_STATUS_SUCCESS)
            {
                SWSS_LOG_ERROR("Failed to set port:%s pg:%zd buffer profile attribute, status:%d", port_name.c_str(), ind, sai_status);
                return task_process_status::task_failed;
            }
        }
    }

    if (m_ready_list.find(key) != m_ready_list.end())
    {
        m_ready_list[key] = true;
    }
    else
    {
        SWSS_LOG_ERROR("PG profile '%s' was inserted after BufferOrch init", key.c_str());
    }

    return task_process_status::task_success;
}

/*
Input sample:"[BUFFER_PROFILE_TABLE:i_port.profile0],[BUFFER_PROFILE_TABLE:i_port.profile1]"
*/
task_process_status BufferOrch::processIngressBufferProfileList(Consumer &consumer)
{
    SWSS_LOG_ENTER();
    auto it = consumer.m_toSync.begin();
    KeyOpFieldsValuesTuple tuple = it->second;
    Port port;
    string key = kfvKey(tuple);
    string op = kfvOp(tuple);

    SWSS_LOG_DEBUG("processing:%s", key.c_str());
    if (consumer.getTableName() != CFG_BUFFER_PORT_INGRESS_PROFILE_LIST_NAME)
    {
        SWSS_LOG_ERROR("Key with invalid table type passed in %s, expected:%s", key.c_str(), CFG_BUFFER_PORT_INGRESS_PROFILE_LIST_NAME);
        return task_process_status::task_invalid_entry;
    }
    vector<string> port_names = tokenize(key, list_item_delimiter);
    vector<sai_object_id_t> profile_list;
    ref_resolve_status resolve_status = resolveFieldRefArray(m_buffer_type_maps, buffer_profile_list_field_name, tuple, profile_list);
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
    sai_attribute_t attr;
    attr.id = SAI_PORT_ATTR_QOS_INGRESS_BUFFER_PROFILE_LIST;
    attr.value.objlist.count = (uint32_t)profile_list.size();
    attr.value.objlist.list = profile_list.data();
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
            return task_process_status::task_failed;
        }
    }
    return task_process_status::task_success;
}

/*
Input sample:"[BUFFER_PROFILE_TABLE:e_port.profile0],[BUFFER_PROFILE_TABLE:e_port.profile1]"
*/
task_process_status BufferOrch::processEgressBufferProfileList(Consumer &consumer)
{
    SWSS_LOG_ENTER();
    auto it = consumer.m_toSync.begin();
    KeyOpFieldsValuesTuple tuple = it->second;
    Port port;
    string key = kfvKey(tuple);
    string op = kfvOp(tuple);
    SWSS_LOG_DEBUG("processing:%s", key.c_str());
    vector<string> port_names = tokenize(key, list_item_delimiter);
    vector<sai_object_id_t> profile_list;
    ref_resolve_status resolve_status = resolveFieldRefArray(m_buffer_type_maps, buffer_profile_list_field_name, tuple, profile_list);
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
    sai_attribute_t attr;
    attr.id = SAI_PORT_ATTR_QOS_EGRESS_BUFFER_PROFILE_LIST;
    attr.value.objlist.count = (uint32_t)profile_list.size();
    attr.value.objlist.list = profile_list.data();
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
            return task_process_status::task_failed;
        }
    }
    return task_process_status::task_success;
}

void BufferOrch::doTask(Consumer &consumer)
{
    SWSS_LOG_ENTER();

    if (!gPortsOrch->isInitDone())
    {
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

        auto task_status = (this->*(m_bufferHandlerMap[map_type_name]))(consumer);
        switch(task_status)
        {
            case task_process_status::task_success :
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
