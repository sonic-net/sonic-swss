#include "tokenize.h"

#include "bufferorch.h"
#include "logger.h"

#include <sstream>
#include <iostream>

extern sai_port_api_t *sai_port_api;
extern sai_queue_api_t *sai_queue_api;
extern sai_switch_api_t *sai_switch_api;
extern sai_buffer_api_t *sai_buffer_api;

using namespace std;

type_map BufferOrch::m_buffer_type_maps = {
    {APP_BUFFER_POOL_TABLE_NAME, new object_map()},
    {APP_BUFFER_PROFILE_TABLE_NAME, new object_map()},
    {APP_BUFFER_QUEUE_TABLE_NAME, new object_map()},
    {APP_BUFFER_PG_TABLE_NAME, new object_map()},
    {APP_BUFFER_PORT_INGRESS_PROFILE_LIST_NAME, new object_map()},
    {APP_BUFFER_PORT_EGRESS_PROFILE_LIST_NAME, new object_map()},
    {APP_BUFFER_PORT_CONFIG_TO_PG_PROFILE_TABLE, new object_map()},
    {APP_BUFFER_PORT_CABLE_LENGTH_TABLE, new object_map()},
};

BufferOrch::BufferOrch(DBConnector *db, vector<string> &tableNames, PortsOrch *portsOrch) :
    Orch(db, tableNames),
    m_portsOrch(portsOrch),
    m_db(APPL_DB, DBConnector::DEFAULT_UNIXSOCKET, 0)
{
    SWSS_LOG_ENTER();
    initTableHandlers();

    m_portsOrch->attach(this);
}

void BufferOrch::update(SubjectType type, void *cntx)
{
    SWSS_LOG_ENTER();

    switch(type) {
    case SUBJECT_TYPE_PORT_SPEED_CHANGE:
    {
        PortSpeedUpdate *update = static_cast<PortSpeedUpdate *>(cntx);
        if (update)
        {
            updatePortProfile(*update);
        }
        else
        {
            SWSS_LOG_ERROR("Invalid context for SUBJECT_TYPE_PORT_SPEED_CHANGE event");
            throw runtime_error("Invalid context for SUBJECT_TYPE_PORT_SPEED_CHANGE event");
        }
        break;
    }
    default:
        break;
    }
}

void BufferOrch::initTableHandlers()
{
    SWSS_LOG_ENTER();
    m_bufferHandlerMap.insert(buffer_handler_pair(APP_BUFFER_POOL_TABLE_NAME, &BufferOrch::processBufferPool));
    m_bufferHandlerMap.insert(buffer_handler_pair(APP_BUFFER_PROFILE_TABLE_NAME, &BufferOrch::processBufferProfile));
    m_bufferHandlerMap.insert(buffer_handler_pair(APP_BUFFER_QUEUE_TABLE_NAME, &BufferOrch::processQueue));
    m_bufferHandlerMap.insert(buffer_handler_pair(APP_BUFFER_PG_TABLE_NAME, &BufferOrch::processPriorityGroup));
    m_bufferHandlerMap.insert(buffer_handler_pair(APP_BUFFER_PORT_INGRESS_PROFILE_LIST_NAME, &BufferOrch::processIngressBufferProfileList));
    m_bufferHandlerMap.insert(buffer_handler_pair(APP_BUFFER_PORT_EGRESS_PROFILE_LIST_NAME, &BufferOrch::processEgressBufferProfileList));
    m_bufferHandlerMap.insert(buffer_handler_pair(APP_BUFFER_PORT_CONFIG_TO_PG_PROFILE_TABLE, &BufferOrch::processPortConfigToPgProfile));
    m_bufferHandlerMap.insert(buffer_handler_pair(APP_BUFFER_PORT_CABLE_LENGTH_TABLE, &BufferOrch::processPortCableLenth));
}

task_process_status BufferOrch::processBufferPool(Consumer &consumer)
{
    SWSS_LOG_ENTER();
    sai_status_t sai_status;
    sai_object_id_t sai_object = SAI_NULL_OBJECT_ID;
    auto it = consumer.m_toSync.begin();
    KeyOpFieldsValuesTuple tuple = it->second;
    string map_type_name = consumer.m_consumer->getTableName();
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
            SWSS_LOG_DEBUG("field:%s, value:%s", fvField(*i).c_str(), fvValue(*i).c_str());
            sai_attribute_t attr;
            if (fvField(*i) == buffer_size_field_name)
            {
                attr.id = SAI_BUFFER_POOL_ATTR_SIZE;
                attr.value.u32 = stoul(fvValue(*i));
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
                    SWSS_LOG_ERROR("Unknown pool type specified:%s", type.c_str());
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
                    SWSS_LOG_ERROR("Unknown pool mode specified:%s", mode.c_str());
                    return task_process_status::task_invalid_entry;
                }
                attr.id = SAI_BUFFER_POOL_ATTR_TH_MODE;
                attribs.push_back(attr);
            }
            else
            {
                SWSS_LOG_ERROR("Unknown pool field specified:%s, ignoring", fvField(*i).c_str());
                continue;
            }
        }
        if (SAI_NULL_OBJECT_ID != sai_object)
        {
            sai_status = sai_buffer_api->set_buffer_pool_attr(sai_object, &attribs[0]);
            if (SAI_STATUS_SUCCESS != sai_status)
            {
                SWSS_LOG_ERROR("Failed to modify buffer pool, name:%s, sai object:%lx, status:%d", object_name.c_str(), sai_object, sai_status);
                return task_process_status::task_failed;
            }
            SWSS_LOG_DEBUG("Modified existing pool:%lx, type:%s name:%s ", sai_object, map_type_name.c_str(), object_name.c_str());
        }
        else
        {
            sai_status = sai_buffer_api->create_buffer_pool(&sai_object, attribs.size(), attribs.data());
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
    string map_type_name = consumer.m_consumer->getTableName();
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
            else if (fvField(*i) == buffer_xon_field_name)
            {
                attr.value.u32 = stoul(fvValue(*i));
                attr.id = SAI_BUFFER_PROFILE_ATTR_XON_TH;
                attribs.push_back(attr);
            }
            else if (fvField(*i) == buffer_xoff_field_name)
            {
                attr.value.u32 = stoul(fvValue(*i));
                attr.id = SAI_BUFFER_PROFILE_ATTR_XOFF_TH;
                attribs.push_back(attr);
            }
            else if (fvField(*i) == buffer_size_field_name)
            {
                attr.id = SAI_BUFFER_PROFILE_ATTR_BUFFER_SIZE;
                attr.value.u32 = stoul(fvValue(*i));
                attribs.push_back(attr);
            }
            else if (fvField(*i) == buffer_dynamic_th_field_name)
            {
                attr.id = SAI_BUFFER_PROFILE_ATTR_SHARED_DYNAMIC_TH;
                attr.value.u32 = stoul(fvValue(*i));
                attribs.push_back(attr);
            }
            else if (fvField(*i) == buffer_static_th_field_name)
            {
                attr.id = SAI_BUFFER_PROFILE_ATTR_SHARED_STATIC_TH;
                attr.value.u32 = stoul(fvValue(*i));
                attribs.push_back(attr);
            }
            else
            {
                SWSS_LOG_ERROR("Unknown buffer profile field specified:%s, ignoring", fvField(*i).c_str());
                continue;
            }
        }
        if (SAI_NULL_OBJECT_ID != sai_object)
        {
            SWSS_LOG_DEBUG("Modifying existing sai object:%lx ", sai_object);
            sai_status = sai_buffer_api->set_buffer_profile_attr(sai_object, &attribs[0]);
            if (SAI_STATUS_SUCCESS != sai_status)
            {
                SWSS_LOG_ERROR("Failed to modify buffer profile, name:%s, sai object:%lx, status:%d", object_name.c_str(), sai_object, sai_status);
                return task_process_status::task_failed;
            }
        }
        else
        {
            sai_status = sai_buffer_api->create_buffer_profile(&sai_object, attribs.size(), attribs.data());
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
    string key = kfvKey(tuple);
    string op = kfvOp(tuple);
    vector<string> tokens;
    sai_uint32_t range_low, range_high;

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
        if (!m_portsOrch->getPort(port_name, port))
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
        if (!m_portsOrch->getPort(port_name, port))
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
            sai_status_t sai_status = sai_buffer_api->set_ingress_priority_group_attr(pg_id, &attr);
            if (sai_status != SAI_STATUS_SUCCESS)
            {
                SWSS_LOG_ERROR("Failed to set port:%s pg:%zd buffer profile attribute, status:%d", port_name.c_str(), ind, sai_status);
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
        SWSS_LOG_ERROR("Key with invalid table type passed in %s, expected:%s", key.c_str(), APP_BUFFER_PORT_INGRESS_PROFILE_LIST_NAME);
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
    attr.value.objlist.count = profile_list.size();
    attr.value.objlist.list = profile_list.data();
    for (string port_name : port_names)
    {
        if (!m_portsOrch->getPort(port_name, port))
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
    attr.value.objlist.count = profile_list.size();
    attr.value.objlist.list = profile_list.data();
    for (string port_name : port_names)
    {
        if (!m_portsOrch->getPort(port_name, port))
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

/*
Input sample "BUFFER_PORT_CABLE_LENGTH_TABLE:"
             "port_alias" : "cable_length"
*/
task_process_status BufferOrch::processPortCableLenth(Consumer &consumer)
{
    SWSS_LOG_ENTER();

    KeyOpFieldsValuesTuple tuple = consumer.m_toSync.begin()->second;
    string key = kfvKey(tuple);
    string op = kfvOp(tuple);

    if (op != SET_COMMAND)
    {
        return task_process_status::task_invalid_operation;
    }

    for (auto itp : kfvFieldsValues(tuple))
    {
        Port port;
        string port_name = fvField(itp);
        string cable_length = fvValue(itp);

        if (!m_portsOrch->getPort(port_name, port))
        {
            SWSS_LOG_ERROR("Port with alias:%s not found", port_name.c_str());
            return task_process_status::task_invalid_entry;
        }

        m_cableLengthMap[port_name] = cable_length;
    }

    return task_process_status::task_success;
}

/*
Input sample "PORT_CONFIG_TO_PG_PROFILE_TABLE:"
             "40000_5"          : "[BUFFER_PROFILE_TABLE:pg_lossless_40G_5m_profile]",
             "<speed>_<length>" : "[profile_name]",
             ...
*/
task_process_status BufferOrch::processPortConfigToPgProfile(Consumer &consumer)
{
    SWSS_LOG_ENTER();

    KeyOpFieldsValuesTuple tuple = consumer.m_toSync.begin()->second;
    string table_name = consumer.m_consumer->getTableName();
    //sai_object_id_t buffer_profile_id;
    string op = kfvOp(tuple);

    SWSS_LOG_DEBUG("Processing:%s", table_name.c_str());

    if (op != SET_COMMAND)
    {
        return task_process_status::task_invalid_operation;
    }

    for (auto it : kfvFieldsValues(tuple))
    {
        string port_config = fvField(it);
        string buffer_profile_name = fvValue(it);

        m_portConfigProfileMap[port_config] = buffer_profile_name;
    }

    return task_process_status::task_success;
}

/*
Read string value fron the DB for given table/key
*/
bool BufferOrch::getTableValue(string table_name, string table_key, string item_key, string &item_value)
{
    SWSS_LOG_ENTER();

    vector<FieldValueTuple> values;
    swss::Table table(&m_db, table_name);

    table.get(table_key, values);

    for(auto it : values)
    {
        if (fvField(it) == item_key)
        {
            item_value = fvValue(it);
            return true;
        }
    }

    return false;
}

/*
Read integer value fron the DB for given table/key
*/
bool BufferOrch::getTableValue(string table_name, string table_key, string item_key, uint32_t &item_value)
{
    SWSS_LOG_ENTER();

    string str_item_value;

    bool status = getTableValue(table_name, table_key, item_key, str_item_value);
    if (status)
    {
        item_value = stoul(str_item_value);
    }

    return status;
}

/*
Write string value fron the DB for given table/key
*/
bool BufferOrch::setTableValue(string table_name, string table_key, string item_key, string &item_value)
{
    SWSS_LOG_ENTER();

    vector<FieldValueTuple> values;
    swss::Table table(&m_db, table_name);

    FieldValueTuple fv(item_key, item_value);
    values.push_back(fv);

    table.set(table_key, values);

    // since table.set returns void - check result by reading the value back
    string value;
    bool read_status = getTableValue(table_name, table_key, item_key, value);

    return read_status && value == item_value;
}

/*
Write integer value fron the DB for given table/key
*/
bool BufferOrch::setTableValue(string table_name, string table_key, string item_key, uint32_t &item_value)
{
    SWSS_LOG_ENTER();

    string str_value = to_string(item_value);

    return setTableValue(table_name, table_key, item_key, str_value);
}

/*
Return "key" part from the full table name used in the buffers configuration syntax
Input: [BUFFER_POOL_TABLE:ingress_lossless_pg_pool]
Return: ingress_lossless_pg_pool
*/
string BufferOrch::cutTableKey(string fullTableName)
{
    SWSS_LOG_ENTER();

    size_t first = fullTableName.find(":");
    size_t last = fullTableName.find("]");

    if (first == string::npos || last == string::npos)
    {
        SWSS_LOG_DEBUG("Expected delimiters not found");
        return fullTableName;
    }

    return fullTableName.substr(first + 1, last - first - 1);
}

/*
Round up input value to the closest equal or greater value from the list
Input: 56, [10, 25, 40, 50, 100]
Return: 100
*/
uint32_t BufferOrch::roundUp(uint32_t value, vector<uint32_t> round_values)
{
    SWSS_LOG_ENTER();

    for(auto round_val : round_values)
    {
        if (round_val >= value)
            return round_val;
    }

    return value;
}

void BufferOrch::updatePortProfile(const PortSpeedUpdate& update)
{
    SWSS_LOG_ENTER();

    string port_name = update.port.m_alias;

    if (m_cableLengthMap.count(port_name) == 0)
    {
        SWSS_LOG_ERROR("Cable length is not configured for port %s. Unable to update buffer profile", port_name.c_str());
        return;
    }

    uint32_t port_speed = roundUp(update.speed, supported_speed);
    string cable_length = m_cableLengthMap[port_name];
    string port_config = to_string(port_speed) + "_" + cable_length;

    if (m_portConfigProfileMap.count(port_config) == 0)
    {
        SWSS_LOG_ERROR("Buffer profile is not configured for port %s, speed %d, cable length %s. "
                       "Unable to update buffer profile", port_name.c_str(), port_speed, cable_length.c_str());
        return;
    }

    // get old profile
    string old_profile_name;
    if (!getTableValue(APP_BUFFER_PG_TABLE_NAME, port_name + ":" + pgs, buffer_profile_field_name, old_profile_name))
    {
        ;
        SWSS_LOG_ERROR("Failed to read %s from table %s", buffer_profile_field_name.c_str(),
                       (string(APP_BUFFER_PG_TABLE_NAME) + port_name + ":" + pgs).c_str());
        return;
    }
    old_profile_name = cutTableKey(old_profile_name);

    // get old profile size
    uint32_t old_profile_size;
    if (!getTableValue(APP_BUFFER_PROFILE_TABLE_NAME, old_profile_name, buffer_size_field_name, old_profile_size))
    {
        SWSS_LOG_ERROR("Failed to read %s from table %s", buffer_size_field_name.c_str(),
                       (string(APP_BUFFER_PROFILE_TABLE_NAME) + old_profile_name).c_str());
        return;
    }

    //get new profile
    string new_profile_name = cutTableKey(m_portConfigProfileMap[port_config]);

    // get new profile size
    uint32_t new_profile_size;
    if (!getTableValue(APP_BUFFER_PROFILE_TABLE_NAME, new_profile_name, buffer_size_field_name, new_profile_size))
    {
        SWSS_LOG_ERROR("Failed to read %s from table %s", buffer_size_field_name.c_str(),
                       (string(APP_BUFFER_PROFILE_TABLE_NAME) + new_profile_name).c_str());
        return;
    }

    // get profile pool name
    string pool_name;
    if (!getTableValue(APP_BUFFER_PROFILE_TABLE_NAME, old_profile_name, buffer_pool_field_name, pool_name))
    {
        SWSS_LOG_ERROR("Failed to read %s from table %s", buffer_pool_field_name.c_str(),
                       (string(APP_BUFFER_PROFILE_TABLE_NAME) + old_profile_name).c_str());
        return;
    }
    pool_name = cutTableKey(pool_name);

    // get old pool size
    uint32_t pool_size;
    if (!getTableValue(APP_BUFFER_POOL_TABLE_NAME, pool_name, buffer_size_field_name, pool_size))
    {
        SWSS_LOG_ERROR("Failed to read %s from table %s", buffer_size_field_name.c_str(),
                       (string(APP_BUFFER_POOL_TABLE_NAME) + pool_name).c_str());
        return;
    }

    // write new pool size
    pool_size = pool_size - old_profile_size + new_profile_size;
    if (!setTableValue(APP_BUFFER_POOL_TABLE_NAME, pool_name, buffer_size_field_name, pool_size))
    {
        SWSS_LOG_ERROR("Failed to write %s to table %s", buffer_size_field_name.c_str(),
                       (string(APP_BUFFER_POOL_TABLE_NAME) + pool_name).c_str());
        return;
    }

    // update port profile
    if (!setTableValue(APP_BUFFER_PG_TABLE_NAME, port_name + ":" + pgs, buffer_profile_field_name, m_portConfigProfileMap[port_config]))
    {
        SWSS_LOG_ERROR("Failed to write %s to table %s", buffer_profile_field_name.c_str(),
                       (string(APP_BUFFER_PG_TABLE_NAME) + port_name + ":" + pgs).c_str());
        return;
    }
    SWSS_LOG_NOTICE("Port %s buffer PG profile for pfc enabled queues changed to %s", port_name.c_str(), new_profile_name.c_str());
}

void BufferOrch::doTask(Consumer &consumer)
{
    SWSS_LOG_ENTER();
    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
        string map_type_name = consumer.m_consumer->getTableName();
        if (m_buffer_type_maps.find(map_type_name) == m_buffer_type_maps.end())
        {
            SWSS_LOG_ERROR("Unrecognised buffers table encountered:%s", map_type_name.c_str());
            it = consumer.m_toSync.erase(it);
            continue;
        }
        if (m_bufferHandlerMap.find(map_type_name) == m_bufferHandlerMap.end())
        {
            SWSS_LOG_ERROR("No handler for key:%s found.", map_type_name.c_str());
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
            it = consumer.m_toSync.erase(it);
            break;
        case task_process_status::task_invalid_operation:
            SWSS_LOG_ERROR("Invalid operation for the table, removing from queue (table %s)", map_type_name.c_str());
            it = consumer.m_toSync.erase(it);
            break;
        case task_process_status::task_failed:
            SWSS_LOG_ERROR("Processing buffer task item failed, exiting.");
            return;
        case task_process_status::task_need_retry:
            SWSS_LOG_INFO("Processing buffer task item failed, will retry.");
            it++;
            break;
        default:
            SWSS_LOG_ERROR("Unknown task status: %d", task_status);
            it = consumer.m_toSync.erase(it);
            break;
        }
    }
}
