#include "sai.h"

#include "bufferorch.h"
#include "logger.h"

#include <sstream>
#include <iostream>

extern sai_port_api_t   *sai_port_api;
extern sai_queue_api_t  *sai_queue_api;
extern sai_switch_api_t *sai_switch_api;
extern sai_buffer_api_t *sai_buffer_api;

type_map BufferOrch::m_buffer_type_maps = {
    {APP_BUFFER_POOL_TABLE_NAME,    new object_map()},
    {APP_BUFFER_PROFILE_TABLE_NAME, new object_map()},
    {APP_BUFFER_QUEUE_TABLE_NAME,   new object_map()},
    {APP_BUFFER_PG_TABLE_NAME,     new object_map()},
    {APP_BUFFER_PORT_INGRESS_PROFILE_LIST_NAME,     new object_map()},
    {APP_BUFFER_PORT_EGRESS_PROFILE_LIST_NAME,     new object_map()}    
};

BufferOrch::BufferOrch(DBConnector *db, vector<string> &tableNames, PortsOrch *portsOrch) :
    Orch(db, tableNames), m_portsOrch(portsOrch) 
{
    SWSS_LOG_ENTER();
    initTableHandlers();
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

task_process_status BufferOrch::processBufferPool(Consumer &consumer)
{
    SWSS_LOG_ENTER();
    sai_status_t            sai_status;
    sai_object_id_t         sai_object = SAI_NULL_OBJECT_ID;
    auto                    it = consumer.m_toSync.begin();
    KeyOpFieldsValuesTuple  tuple = it->second;
    string                  map_type_name = consumer.m_consumer->getTableName();
    string                  object_name = kfvKey(tuple);
    string                  op = kfvOp(tuple);

    SWSS_LOG_DEBUG("object name:%s", object_name.c_str());
    if (map_type_name != APP_BUFFER_POOL_TABLE_NAME)
    {
        SWSS_LOG_ERROR("Key with invalid table type passed in %s, expected:%s\n", map_type_name.c_str(), APP_BUFFER_POOL_TABLE_NAME);
        return task_process_status::task_invalid_entry;
    }
    if (m_buffer_type_maps[map_type_name]->find(object_name) != m_buffer_type_maps[map_type_name]->end())
    {
        sai_object = (*(m_buffer_type_maps[map_type_name]))[object_name];
        SWSS_LOG_DEBUG("found existing object:%s of type:%s", object_name.c_str(), map_type_name.c_str());
    }
    SWSS_LOG_DEBUG("processing command:%s", op.c_str());
    if (op == SET_COMMAND)
    {
        std::vector<sai_attribute_t> attribs;
        for (auto i = kfvFieldsValues(tuple).begin(); i != kfvFieldsValues(tuple).end(); i++)
        {
            SWSS_LOG_DEBUG("field:%s, value:%s", fvField(*i).c_str(), fvValue(*i).c_str());
            sai_attribute_t attr;
            if (fvField(*i) == buffer_size_field_name)
            {
                attr.id = SAI_BUFFER_POOL_ATTR_SIZE;
                attr.value.u32 = std::stoul(fvValue(*i));                
                attribs.push_back(attr);
            }
            else if (fvField(*i) == buffer_pool_type_field_name)
            {
                string type = fvValue(*i);
                if (type == buffer_value_ingress)
                {
                    attr.value.u32 = SAI_BUFFER_POOL_INGRESS;
                }
                else if (type == buffer_value_egress)
                {
                    attr.value.u32 = SAI_BUFFER_POOL_EGRESS;
                }
                else
                {
                    SWSS_LOG_ERROR("Unknown pool type specified:%s\n", type.c_str());
                    return task_process_status::task_invalid_entry;
                }
                attr.id = SAI_BUFFER_POOL_ATTR_TYPE;
                attribs.push_back(attr);
            }
            else if (fvField(*i) == buffer_pool_mode_field_name)
            {
                string mode = fvValue(*i);
                if (mode == buffer_pool_mode_dynamic_value)
                {
                    attr.value.u32 = SAI_BUFFER_THRESHOLD_MODE_DYNAMIC;
                }
                else if (mode == buffer_pool_mode_static_value)
                {
                    attr.value.u32 = SAI_BUFFER_THRESHOLD_MODE_STATIC;
                }
                else
                {
                    SWSS_LOG_ERROR("Unknown pool mode specified:%s\n", mode.c_str());
                    return task_process_status::task_invalid_entry;
                }
                attr.id = SAI_BUFFER_POOL_ATTR_TH_MODE;
                attribs.push_back(attr);
            }
            else 
            {
                SWSS_LOG_ERROR("Unknown pool field specified:%s, ignoring\n", fvField(*i).c_str());
                continue;
            }
        }
        if (SAI_NULL_OBJECT_ID != sai_object)
        {
            SWSS_LOG_DEBUG("Modifying existing sai object:%llx ", sai_object);
            sai_status = sai_buffer_api->set_buffer_pool_attr(sai_object, &attribs[0]);
            if (SAI_STATUS_SUCCESS != sai_status)
            {
                SWSS_LOG_ERROR("Failed to modify buffer pool, name:%s, sai object:%llx, status:%d", object_name.c_str(), sai_object, sai_status);
                return task_process_status::task_failed;
            }
            SWSS_LOG_DEBUG("Modified existing pool:%llx, type:%s name:%s ", sai_object, map_type_name.c_str(), object_name.c_str());
        }
        else
        {
            SWSS_LOG_DEBUG("Creating new sai object");
            sai_status = sai_buffer_api->create_buffer_pool(&sai_object, attribs.size(), attribs.data());
            if (SAI_STATUS_SUCCESS != sai_status)
            {
                SWSS_LOG_ERROR("Failed to create buffer pool, name:%s, status:%d", object_name.c_str(), sai_status);            
                return task_process_status::task_failed;
            }
            (*(m_buffer_type_maps[map_type_name]))[object_name] = sai_object;
            SWSS_LOG_DEBUG("Created new pool:%llx, type:%s name:%s ", sai_object, map_type_name.c_str(), object_name.c_str());
        }
    }
    else if (op == DEL_COMMAND)
    {
        sai_status = sai_buffer_api->remove_buffer_pool(sai_object);
        if (SAI_STATUS_SUCCESS != sai_status)
        {
            SWSS_LOG_ERROR("Failed to remove buffer pool, name:%s, sai object:%llx, status:%d", object_name.c_str(), sai_object, sai_status);
            return task_process_status::task_failed;
        }
        auto it_to_delete = (m_buffer_type_maps[map_type_name])->find(object_name);
        (m_buffer_type_maps[map_type_name])->erase(it_to_delete);
    }
    else
    {
        SWSS_LOG_ERROR("Unknown operation type %s\n", op.c_str());
        return task_process_status::task_invalid_entry;
    }
    return task_process_status::task_success;
}

task_process_status BufferOrch::processBufferProfile(Consumer &consumer)
{
    SWSS_LOG_ENTER();
    sai_status_t            sai_status;
    sai_object_id_t         sai_object = SAI_NULL_OBJECT_ID;
    auto                    it = consumer.m_toSync.begin();
    KeyOpFieldsValuesTuple  tuple = it->second;
    string                  map_type_name = consumer.m_consumer->getTableName();
    string                  object_name = kfvKey(tuple);
    string                  op = kfvOp(tuple);

    SWSS_LOG_DEBUG("object name:%s", object_name.c_str());
    if (map_type_name != APP_BUFFER_PROFILE_TABLE_NAME)
    {
        SWSS_LOG_ERROR("Key with invalid table type passed in %s, expected:%s\n", map_type_name.c_str(), APP_BUFFER_PROFILE_TABLE_NAME);
        return task_process_status::task_invalid_entry;
    }
    if (m_buffer_type_maps[map_type_name]->find(object_name) != m_buffer_type_maps[map_type_name]->end())
    {
        sai_object = (*(m_buffer_type_maps[map_type_name]))[object_name];
        SWSS_LOG_DEBUG("found existing object:%s of type:%s", object_name.c_str(), map_type_name.c_str());
    }    
    SWSS_LOG_DEBUG("processing command:%s", op.c_str());
    if (op == SET_COMMAND)
    {
        std::vector<sai_attribute_t> attribs;
        for (auto i = kfvFieldsValues(tuple).begin(); i != kfvFieldsValues(tuple).end(); i++)
        {
            SWSS_LOG_DEBUG("field:%s, value:%s", fvField(*i).c_str(), fvValue(*i).c_str());
            sai_attribute_t attr;
            if (fvField(*i) == buffer_pool_field_name)
            {
                sai_object_id_t sai_pool;
                ref_resolve_status resolve_result = resolveFieldRefValue(m_buffer_type_maps, buffer_pool_field_name, tuple, sai_pool);
                if (ref_resolve_status::success != resolve_result)
                {
                    if(ref_resolve_status::not_resolved == resolve_result)
                    {
                        SWSS_LOG_ERROR("Missing or invalid pool reference specified\n");
                        return task_process_status::task_need_retry;
                    }
                    SWSS_LOG_ERROR("Resolving pool reference failed\n");
                    return task_process_status::task_failed;
                }
                attr.id = SAI_BUFFER_PROFILE_ATTR_POOL_ID;
                attr.value.oid = sai_pool;
                attribs.push_back(attr);
            }
            else if (fvField(*i) == buffer_xon_field_name)
            {
                attr.value.u32 = std::stoul(fvValue(*i));
                attr.id = SAI_BUFFER_PROFILE_ATTR_XON_TH;
                attribs.push_back(attr);
            }
            else if (fvField(*i) == buffer_xoff_field_name)
            {
                attr.value.u32 = std::stoul(fvValue(*i));
                attr.id = SAI_BUFFER_PROFILE_ATTR_XOFF_TH;
                attribs.push_back(attr);
            }
            else if (fvField(*i) == buffer_size_field_name)
            {
                attr.id = SAI_BUFFER_PROFILE_ATTR_BUFFER_SIZE;
                attr.value.u32 = std::stoul(fvValue(*i));
                attribs.push_back(attr);
            }
            else if (fvField(*i) == buffer_dynamic_th_field_name)
            {
                attr.id = SAI_BUFFER_PROFILE_ATTR_SHARED_DYNAMIC_TH;
                attr.value.u32 = std::stoul(fvValue(*i));
                attribs.push_back(attr);
            }
            else if (fvField(*i) == buffer_static_th_field_name)
            {
                attr.id = SAI_BUFFER_PROFILE_ATTR_SHARED_STATIC_TH;
                attr.value.u32 = std::stoul(fvValue(*i));
                attribs.push_back(attr);
            }
            else
            {
                SWSS_LOG_ERROR("Unknown buffer profile field specified:%s, ignoring\n", fvField(*i).c_str());
                continue;
            }
        }
        if (SAI_NULL_OBJECT_ID != sai_object)
        {
            SWSS_LOG_DEBUG("Modifying existing sai object:%llx ", sai_object);
            sai_status = sai_buffer_api->set_buffer_profile_attr(sai_object, &attribs[0]);
            if (SAI_STATUS_SUCCESS != sai_status)
            {
                SWSS_LOG_ERROR("Failed to modify buffer profile, name:%s, sai object:%llx, status:%d", object_name.c_str(), sai_object, sai_status);
                return task_process_status::task_failed;
            }
        }
        else
        {
            SWSS_LOG_DEBUG("Creating new sai object");
            sai_status = sai_buffer_api->create_buffer_profile(&sai_object, attribs.size(), attribs.data());
            if (SAI_STATUS_SUCCESS != sai_status)
            {
                SWSS_LOG_ERROR("Failed to create buffer profile, name:%s, status:%d", object_name.c_str(), sai_status);
                return task_process_status::task_failed;
            }
            (*(m_buffer_type_maps[map_type_name]))[object_name] = sai_object;
            SWSS_LOG_DEBUG("Created new sai object:%llx, type:%s name:%s ", sai_object, map_type_name.c_str(), object_name.c_str());
        }
    }
    else if (op == DEL_COMMAND)
    {
        sai_status = sai_buffer_api->remove_buffer_profile(sai_object);
        if (SAI_STATUS_SUCCESS != sai_status)
        {
            SWSS_LOG_ERROR("Failed to remove buffer profile, name:%s, sai object:%llx, status:%d", object_name.c_str(), sai_object, sai_status);
            return task_process_status::task_failed;
        }
        auto it_to_delete = (m_buffer_type_maps[map_type_name])->find(object_name);
        (m_buffer_type_maps[map_type_name])->erase(it_to_delete);
    }
    else
    {
        SWSS_LOG_ERROR("Unknown operation type %s\n", op.c_str());
        return task_process_status::task_invalid_entry;
    }
    return task_process_status::task_success;
}

bool BufferOrch::parseIndexRange(const string &input, sai_uint32_t &range_low, sai_uint32_t &range_high)
{
    SWSS_LOG_ENTER();
    SWSS_LOG_DEBUG("input:%s", input.c_str());
    if (input.find(range_specifier) != string::npos)
    {
        vector<string> range_values;
        if (!tokenizeString(input, range_specifier, range_values))
        {
            SWSS_LOG_ERROR("Failed to parse index in:%s\n", input.c_str());
            return false;
        }
        if (range_values.size() != 2)
        {
            SWSS_LOG_ERROR("malformed index range in:%s. Must contain 2 tokens\n", input.c_str());
            return false;
        }
        range_low = std::stoul(range_values[0]);
        range_high = std::stoul(range_values[1]);
        if (range_low >= range_high)
        {
            SWSS_LOG_ERROR("malformed index range in:%s. left value must be less than righ value.\n", input.c_str());
            return false;
        }
    }
    else
    {
        range_low = range_high = std::stoul(input);
    }
    SWSS_LOG_DEBUG("resulting range:%d-%d", range_low, range_high);
    return true;
}

bool BufferOrch::parseNameArray(const string &input, vector<string> &port_names)
{
    if (input.find(comma) == string::npos)
    {
        port_names.push_back(input);
        return true;
    }
    return tokenizeString(input, list_item_delimiter, port_names);
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
    string key = kfvKey(tuple);
    string op = kfvOp(tuple);
    vector<string> tokens;
    sai_uint32_t range_low, range_high;

    SWSS_LOG_DEBUG("Processing:%s", key.c_str());
    if (!tokenizeString(key, delimiter, tokens))
    {
        return task_process_status::task_invalid_entry;
    }
    if (tokens.size() != 2)
    {
        SWSS_LOG_ERROR("malformed key:%s. Must contain 2 tokens\n", key.c_str());
        return task_process_status::task_invalid_entry;
    }
    if (consumer.m_consumer->getTableName() != APP_BUFFER_QUEUE_TABLE_NAME)
    {
        SWSS_LOG_ERROR("Key with invalid table type passed in %s, expected:%s\n", key.c_str(), APP_BUFFER_QUEUE_TABLE_NAME);
        return task_process_status::task_invalid_entry;
    }
    vector<string> port_names;
    if (!parseNameArray(tokens[0], port_names))
    {
        SWSS_LOG_ERROR("Failed to obtain port names");
        return task_process_status::task_invalid_entry;
    }
    if (!parseIndexRange(tokens[1], range_low, range_high))
    {
        return task_process_status::task_invalid_entry;
    }
    ref_resolve_status  resolve_result = resolveFieldRefValue(m_buffer_type_maps, buffer_profile_field_name, tuple, sai_buffer_profile);
    if (ref_resolve_status::success != resolve_result)
    {
        if(ref_resolve_status::not_resolved == resolve_result)
        {
            SWSS_LOG_ERROR("Missing or invalid queue buffer profile reference specified\n");
            return task_process_status::task_need_retry;
        }
        SWSS_LOG_ERROR("Resolving queue profile reference failed\n");
        return task_process_status::task_failed;
    }
    sai_attribute_t attr;
    attr.id = SAI_QUEUE_ATTR_BUFFER_PROFILE_ID;
    attr.value.oid = sai_buffer_profile;
    for (string port_name : port_names)
    {
        Port port;
        SWSS_LOG_DEBUG("processing port:%s", port_name.c_str());
        if (!m_portsOrch->getPort(port_name, port))
        {
            SWSS_LOG_ERROR("Port with alias:%s not found\n", port_name.c_str());
            return task_process_status::task_invalid_entry;
        }
        for (size_t ind = range_low; ind <= range_high; ind++)
        {
            sai_object_id_t queue_id;
            SWSS_LOG_DEBUG("processing queue:%d", ind);
            if (!port.getQueue(ind, queue_id))
            {
                SWSS_LOG_ERROR("Invalid queue index specified:%d", ind);
                return task_process_status::task_invalid_entry;
            }
            SWSS_LOG_DEBUG("Applying buffer profile:0x%llx to queue index:%d, queue sai_id:0x%llx", sai_buffer_profile, ind, queue_id);
            sai_status_t sai_status = sai_queue_api->set_queue_attribute(queue_id, &attr);
            if (sai_status != SAI_STATUS_SUCCESS)
            {
                SWSS_LOG_ERROR("Failed to set queue's buffer profile attribute, status:%d", sai_status);
                return task_process_status::task_failed;
            }
        }
    }
    return task_process_status::task_success;
}

/*
Input sample "BUFFER_PG_TABLE:Ethernet4,Ethernet45:10-15"
*/
task_process_status BufferOrch::processPriorityGroup(Consumer &consumer)
{
    SWSS_LOG_ENTER();
    auto it = consumer.m_toSync.begin();
    KeyOpFieldsValuesTuple tuple = it->second;    
    sai_object_id_t sai_buffer_profile;
    string key = kfvKey(tuple);
    string op = kfvOp(tuple);
    vector<string> tokens;
    sai_uint32_t range_low, range_high;

    SWSS_LOG_DEBUG("processing:%s", key.c_str());
    if (!tokenizeString(key, delimiter, tokens))
    {
        return task_process_status::task_invalid_entry;
    }
    if (tokens.size() != 2)
    {
        SWSS_LOG_ERROR("malformed key:%s. Must contain 2 tokens\n", key.c_str());
        return task_process_status::task_invalid_entry;
    }
    if (consumer.m_consumer->getTableName() != APP_BUFFER_PG_TABLE_NAME)
    {
        SWSS_LOG_ERROR("Key with invalid table type passed in %s, expected:%s\n", key.c_str(), APP_BUFFER_PG_TABLE_NAME);
        return task_process_status::task_invalid_entry;
    }
    vector<string> port_names;
    if (!parseNameArray(tokens[0], port_names))
    {
        SWSS_LOG_ERROR("Failed to obtain port names");
        return task_process_status::task_invalid_entry;
    }
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
            SWSS_LOG_ERROR("Missing or invalid pg profile reference specified\n");
            return task_process_status::task_need_retry;
        }
        SWSS_LOG_ERROR("Resolving pg profile reference failed\n");
        return task_process_status::task_failed;
    }
    sai_attribute_t attr;
    attr.id = SAI_INGRESS_PRIORITY_GROUP_ATTR_BUFFER_PROFILE;
    attr.value.oid = sai_buffer_profile;
    for (string port_name : port_names)
    {
        Port port;
        SWSS_LOG_DEBUG("processing port:%s", port_name.c_str());
        if (!m_portsOrch->getPort(port_name, port))
        {
            SWSS_LOG_ERROR("Port with alias:%s not found\n", port_name.c_str());
            return task_process_status::task_invalid_entry;
        }
        for (size_t ind = range_low; ind <= range_high; ind++)
        {
            sai_object_id_t pg_id;
            SWSS_LOG_DEBUG("processing pg:%d", ind);
            if (!port.getPG(ind, pg_id))
            {
                SWSS_LOG_ERROR("Invalid pg index specified:%d", ind);
                return task_process_status::task_invalid_entry;
            }
            SWSS_LOG_DEBUG("Applying buffer profile:0x%llx to port:%s pg index:%d, pg sai_id:0x%llx", sai_buffer_profile, port_name.c_str(), ind, pg_id);
            sai_status_t sai_status = sai_buffer_api->set_ingress_priority_group_attr(pg_id, &attr);
            if (sai_status != SAI_STATUS_SUCCESS)
            {
                SWSS_LOG_ERROR("Failed to set port:%s pg:%d buffer profile attribute, status:%d", port_name.c_str(), ind, sai_status);
                return task_process_status::task_failed;
            }
        }
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
    if (consumer.m_consumer->getTableName() != APP_BUFFER_PORT_INGRESS_PROFILE_LIST_NAME)
    {
        SWSS_LOG_ERROR("Key with invalid table type passed in %s, expected:%s\n", key.c_str(), APP_BUFFER_PORT_INGRESS_PROFILE_LIST_NAME);
        return task_process_status::task_invalid_entry;
    }
    vector<string> port_names;
    if (!parseNameArray(key, port_names))
    {
        SWSS_LOG_ERROR("Failed to obtain port names");
        return task_process_status::task_invalid_entry;
    }
    vector<sai_object_id_t> profile_list;
    ref_resolve_status resolve_status = resolveFieldRefArray(m_buffer_type_maps, buffer_profile_list_field_name, tuple, profile_list);
    if (ref_resolve_status::success != resolve_status)
    {
        if(ref_resolve_status::not_resolved == resolve_status)
        {
            SWSS_LOG_ERROR("Missing or invalid ingress buffer profile reference specified for:%s\n", key.c_str());
            return task_process_status::task_need_retry;
        }
        SWSS_LOG_ERROR("Failed resolving ingress buffer profile reference specified for:%s\n", key.c_str());
        return task_process_status::task_failed;
    }
    sai_attribute_t attr;
    attr.id = SAI_PORT_ATTR_QOS_INGRESS_BUFFER_PROFILE_LIST;
    attr.value.objlist.count = profile_list.size();
    attr.value.objlist.list = profile_list.data();
    for (string port_name : port_names)
    {        
        if (!m_portsOrch->getPort(port_name, port))
        {
            SWSS_LOG_ERROR("Port with alias:%s not found\n", port_name.c_str());
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
    if (consumer.m_consumer->getTableName() != APP_BUFFER_PORT_EGRESS_PROFILE_LIST_NAME)
    {
        SWSS_LOG_ERROR("Key with invalid table type passed in %s, expected:%s\n", key.c_str(), APP_BUFFER_PORT_EGRESS_PROFILE_LIST_NAME);
        return task_process_status::task_invalid_entry;
    }
    vector<string> port_names;
    if (!parseNameArray(key, port_names))
    {
        SWSS_LOG_ERROR("Failed to obtain port names");
        return task_process_status::task_invalid_entry;
    }
    vector<sai_object_id_t> profile_list;
    ref_resolve_status resolve_status = resolveFieldRefArray(m_buffer_type_maps, buffer_profile_list_field_name, tuple, profile_list);
    if (ref_resolve_status::success != resolve_status)
    {
        if(ref_resolve_status::not_resolved == resolve_status)
        {
            SWSS_LOG_ERROR("Missing or invalid egress buffer profile reference specified for:%s\n", key.c_str());
            return task_process_status::task_need_retry;
        }
        SWSS_LOG_ERROR("Failed resolving egress buffer profile reference specified for:%s\n", key.c_str());
        return task_process_status::task_failed;
    }
    sai_attribute_t attr;
    attr.id = SAI_PORT_ATTR_QOS_EGRESS_BUFFER_PROFILE_LIST;
    attr.value.objlist.count = profile_list.size();
    attr.value.objlist.list = profile_list.data();
    for (string port_name : port_names)
    {        
        if (!m_portsOrch->getPort(port_name, port))
        {
            SWSS_LOG_ERROR("Port with alias:%s not found\n", port_name.c_str());
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
    if (consumer.m_toSync.empty()) 
    {
        return;
    }
    if (!m_portsOrch->isInitDone())
    {
        return;
    }
    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end()) 
    {
        KeyOpFieldsValuesTuple tuple = it->second;
        string map_type_name = consumer.m_consumer->getTableName();
        if (m_buffer_type_maps.find(map_type_name) == m_buffer_type_maps.end()) 
        {
            SWSS_LOG_ERROR("Unrecognised qos table encountered:%s\n", map_type_name.c_str());
            it = consumer.m_toSync.erase(it);
            continue;
        }
        if (m_bufferHandlerMap.find(map_type_name) == m_bufferHandlerMap.end())
        {
            SWSS_LOG_ERROR("No handler for key:%s found.\n", map_type_name.c_str());
            it = consumer.m_toSync.erase(it);
            continue;
        }
        task_process_status task_status = (this->*(m_bufferHandlerMap[map_type_name]))(consumer);
        switch(task_status)
        {
        case task_process_status::task_success :
            it = consumer.m_toSync.erase(it);
            break;
        case task_process_status::task_invalid_entry:
            SWSS_LOG_ERROR("Invalid buffer task item was encountered, removing from queue.");
            dumpTuple(consumer, tuple);
            it = consumer.m_toSync.erase(it);
            break;
        case task_process_status::task_failed:
            SWSS_LOG_ERROR("Processing buffer task item failed, exiting. ");
            dumpTuple(consumer, tuple);
            return;
        case task_process_status::task_need_retry:
            SWSS_LOG_ERROR("Processing buffer task item failed, will retry.");
            dumpTuple(consumer, tuple);
            it++;
        }
    }
}

