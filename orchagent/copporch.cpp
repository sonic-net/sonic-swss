#include "sai.h"

#include "copporch.h"
#include "logger.h"

#include <sstream>
#include <iostream>

extern sai_hostif_api_t*    sai_hostif_api;
extern sai_policer_api_t*   sai_policer_api;

CoppOrch::CoppOrch(DBConnector *db, string tableName) :
    Orch(db, tableName)
{
    SWSS_LOG_ENTER();
};

bool CoppOrch::getPolicerMeter(string input, sai_meter_type_t& meter_value) const
{
    SWSS_LOG_ENTER();
    SWSS_LOG_DEBUG("copp policer meter type:%s\n", input.c_str());
    if (input == copp_policer_meter_packets)
    {
        meter_value = SAI_METER_TYPE_PACKETS;
    }
    else if (input == copp_policer_meter_bytes)
    {
        meter_value = SAI_METER_TYPE_BYTES;        
    }
    else
    {
        SWSS_LOG_DEBUG("invalid meter value :%s\n", input.c_str());
        return false;
    }
    return true;
}

bool CoppOrch::getPolicerMode(string input, sai_policer_mode_t& mode) const
{
    SWSS_LOG_ENTER();
    if (input == copp_policer_mode_sr_tcm)
    {
        mode = SAI_POLICER_MODE_Sr_TCM;
    }
    else if (input == copp_policer_mode_tr_tcm)
    {
        mode = SAI_POLICER_MODE_Tr_TCM;
    }
    else if (input == copp_policer_mode_storm)
    {
        mode = SAI_POLICER_MODE_STORM_CONTROL;
    }
    else
    {
        SWSS_LOG_ERROR("Invalid mode specified %s\n", input.c_str());
        return false;
    }        
    return true;
}

bool CoppOrch::getPlicerColor(string input, sai_policer_color_source_t& color) const
{
    SWSS_LOG_ENTER();
    if (input == copp_policer_color_aware)
    {
        color = SAI_POLICER_COLOR_SOURCE_AWARE;
    }
    else if (input == copp_policer_mode_tr_tcm)
    {
        color = SAI_POLICER_COLOR_SOURCE_BLIND;
    }
    else
    {
        SWSS_LOG_ERROR("Invalid color specified %s\n", input.c_str());
        return false;
    }        
    return true;
}

bool getPolicerAction(string input, sai_packet_action_t &policer_action)
{
    SWSS_LOG_ENTER();
    if (input == copp_policer_action_value_drop)
    {
        policer_action = SAI_PACKET_ACTION_DROP;
    }
    else if (input == copp_policer_action_value_forward)
    {
        policer_action = SAI_PACKET_ACTION_FORWARD;
    }
    else if (input == copp_policer_action_value_copy)
    {
        policer_action = SAI_PACKET_ACTION_COPY;
    }
    else if (input == copp_policer_action_value_copy_cancel)
    {
        policer_action = SAI_PACKET_ACTION_COPY_CANCEL;
    }
    else if (input == copp_policer_action_value_trap)
    {
        policer_action = SAI_PACKET_ACTION_TRAP;
    }
    else if (input == copp_policer_action_value_log)
    {
        policer_action = SAI_PACKET_ACTION_LOG;
    }
    else if (input == copp_policer_action_value_deny)
    {
        policer_action = SAI_PACKET_ACTION_DENY;
    }
    else if (input == copp_policer_action_value_transit)
    {
        policer_action = SAI_PACKET_ACTION_TRANSIT;
    }
    else
    {
        SWSS_LOG_ERROR("Invalid action specified %s\n", input.c_str());
        return false;
    }
    return true;
}

bool CoppOrch::getTrapID(string &trap_id_str, sai_hostif_trap_id_t &trap_id) const
{
    SWSS_LOG_ENTER();
    for(auto trap_id_entry : valid_trap_id_list)
    {
        if (trap_id_str == trap_id_entry.name)
        {
            trap_id = trap_id_entry.trap_id;
            return true;
        }
    }
    return false;
}

bool CoppOrch::getTrapIdList(vector<string> &trap_id_name_list, vector<sai_hostif_trap_id_t> &trap_id_list) const
{
    SWSS_LOG_ENTER();
    for(auto trap_id_str : trap_id_name_list)
    {
        sai_hostif_trap_id_t trap_id;
        if (!getTrapID(trap_id_str, trap_id))
        {
            SWSS_LOG_ERROR("Invalid trap_id value specified %s\n", trap_id_str.c_str());
            return false;
        }
        SWSS_LOG_DEBUG("Input trap_id:%s, pushing trap_id:%d", trap_id_str.c_str(), trap_id);
        trap_id_list.push_back(trap_id);
    }
    return true;
}

bool CoppOrch::applyTrapIds(sai_object_id_t trap_group, vector<string> &trap_id_name_list)
{
    SWSS_LOG_ENTER();
    sai_status_t sai_status;
    vector<sai_hostif_trap_id_t> trap_id_list;
    if (!getTrapIdList(trap_id_name_list, trap_id_list))
    {
        return false;
    }
    sai_attribute_t attr;
    attr.id = SAI_HOSTIF_TRAP_ATTR_TRAP_GROUP;
    attr.value.oid = trap_group;
    for (auto trap_id : trap_id_list)
    {
        if (SAI_STATUS_SUCCESS != (sai_status = sai_hostif_api->set_trap_attribute(trap_id, &attr)))
        {
            SWSS_LOG_ERROR("Failed to apply trap_id:%d to trap group:%llx, error:%d\n", trap_id, trap_group, sai_status);
            return false;
        }
        
    }
    return true;    
}

bool CoppOrch::removePolicerFromTrapGroup(sai_object_id_t trap_group)
{
    sai_attribute_t attr;
    sai_status_t sai_status;
    sai_object_id_t policer_id;    
    attr.id = SAI_HOSTIF_TRAP_GROUP_ATTR_POLICER;
    attr.value.oid = SAI_NULL_OBJECT_ID;
    if (SAI_STATUS_SUCCESS != (sai_status = sai_hostif_api->get_trap_group_attribute(trap_group, 1, &attr))) {
        SWSS_LOG_ERROR("Failed to get policer for trap gorup:%llx, error:%d\n", trap_group, sai_status);
        return false;
    }
    policer_id = attr.value.oid;
    if (SAI_STATUS_SUCCESS != (sai_status = sai_hostif_api->set_trap_group_attribute(trap_group, &attr))) {
        SWSS_LOG_ERROR("Failed to reset policer for trap gorup:%llx, error:%d\n", trap_group, sai_status);
        return false;
    }
    if (SAI_STATUS_SUCCESS != (sai_status = sai_policer_api->remove_policer(policer_id)))
    {
        SWSS_LOG_ERROR("Failed to remove policer:%llx, error:%d\n", policer_id, sai_status);
        return false;
    }
    return true;
}
task_process_status CoppOrch::processCoppRule(Consumer& consumer)
{
    SWSS_LOG_ENTER();
    sai_status_t sai_status;
    sai_object_id_t policer_id;
    vector<string> trap_id_list;
    string queue_ind;
    auto it = consumer.m_toSync.begin();
    KeyOpFieldsValuesTuple tuple = it->second;    
    string key = kfvKey(tuple);
    string op = kfvOp(tuple);
    

    SWSS_LOG_DEBUG("copp:processing:%s", key.c_str());
    if (consumer.m_consumer->getTableName() != APP_COPP_TABLE_NAME)
    {
        SWSS_LOG_ERROR("Key with invalid table type passed in %s, expected:%s\n", key.c_str(), APP_COPP_TABLE_NAME);
        return task_process_status::task_invalid_entry;
    }
    std::vector<sai_attribute_t> trap_attribs;
    std::vector<sai_attribute_t> policer_attribs;
    sai_attribute_t attr;
    
    if (op == SET_COMMAND)
    {
    
        for (auto i = kfvFieldsValues(tuple).begin(); i != kfvFieldsValues(tuple).end(); i++)
        {
            SWSS_LOG_DEBUG("field:%s, value:%s", fvField(*i).c_str(), fvValue(*i).c_str());
            sai_attribute_t attr;
            if (fvField(*i) == copp_trap_id_list)
            {
                if (!tokenizeString(fvValue(*i), list_item_delimiter, trap_id_list))
                {
                    SWSS_LOG_ERROR("Failed to obtain trap_id list:%s\n", fvValue(*i).c_str());
                    return task_process_status::task_invalid_entry;
                }
            }
            else if (fvField(*i) == copp_queue)
            {
                queue_ind = fvValue(*i);
                SWSS_LOG_DEBUG("queue data:%s", queue_ind.c_str());
            }
            //
            // process policer attributes
            //
            else if (fvField(*i) == copp_policer_meter_type)
            {
                sai_meter_type_t meter_value;
                if (!getPolicerMeter(fvValue(*i), meter_value))
                {
                    SWSS_LOG_ERROR("Failed to translate meter type:%s\n", fvValue(*i).c_str());
                    return task_process_status::task_invalid_entry;
                }
                attr.id = SAI_POLICER_ATTR_METER_TYPE;
                attr.value.u32 = meter_value;
                policer_attribs.push_back(attr);                
            }
            else if (fvField(*i) == copp_policer_mode)  
            {
                sai_policer_mode_t mode;
                if (!getPolicerMode(fvValue(*i), mode))
                {
                    SWSS_LOG_ERROR("Failed to translate mode:%s\n", fvValue(*i).c_str());
                    return task_process_status::task_invalid_entry;
                }
                attr.id = SAI_POLICER_ATTR_MODE;
                attr.value.u32 = mode;
                policer_attribs.push_back(attr);                
            }        
            else if (fvField(*i) == copp_policer_color)  
            {
                sai_policer_color_source_t color;
                if (!getPlicerColor(fvValue(*i), color))
                {
                    SWSS_LOG_ERROR("Failed to translate color:%s\n", fvValue(*i).c_str());
                    return task_process_status::task_invalid_entry;
                }
                attr.id = SAI_POLICER_ATTR_COLOR_SOURCE;
                attr.value.u32 = color;
                policer_attribs.push_back(attr);                
            }        
            else if (fvField(*i) == copp_policer_cbs)  
            {
                attr.id = SAI_POLICER_ATTR_CBS;
                attr.value.u64 = std::stoul(fvValue(*i));
                policer_attribs.push_back(attr);
                SWSS_LOG_DEBUG("obtained cbs:%d", attr.value.u64);
            }
            else if (fvField(*i) == copp_policer_cir)          
            {
                attr.id = SAI_POLICER_ATTR_CIR;
                attr.value.u64 = std::stoul(fvValue(*i));
                policer_attribs.push_back(attr);
                SWSS_LOG_DEBUG("obtained cir:%d", attr.value.u64);
            }
            else if (fvField(*i) == copp_policer_pbs)          
            {
                attr.id = SAI_POLICER_ATTR_PBS;
                attr.value.u64 = std::stoul(fvValue(*i));
                policer_attribs.push_back(attr);
                SWSS_LOG_DEBUG("obtained pbs:%d", attr.value.u64);
            }
            else if (fvField(*i) == copp_policer_pir)          
            {
                attr.id = SAI_POLICER_ATTR_PIR;
                attr.value.u64 = std::stoul(fvValue(*i));
                policer_attribs.push_back(attr);
                SWSS_LOG_DEBUG("obtained pir:%d", attr.value.u64);
            }
            else if (fvField(*i) == copp_policer_action_green)
            {
                sai_packet_action_t policer_action;
                if (!getPolicerAction(fvValue(*i), policer_action))
                {
                    SWSS_LOG_ERROR("Invalid green action specified %s\n", fvValue(*i).c_str());
                    return task_process_status::task_invalid_entry;
                }
                attr.id = SAI_POLICER_ATTR_GREEN_PACKET_ACTION;
                attr.value.u32 = policer_action;
                policer_attribs.push_back(attr);                
            }
            else if (fvField(*i) == copp_policer_action_red) 
            {
                sai_packet_action_t policer_action;
                if (!getPolicerAction(fvValue(*i), policer_action))
                {
                    SWSS_LOG_ERROR("Invalid red action specified %s\n", fvValue(*i).c_str());
                    return task_process_status::task_invalid_entry;
                }
                attr.id = SAI_POLICER_ATTR_RED_PACKET_ACTION;
                attr.value.u32 = policer_action;
                policer_attribs.push_back(attr);                
            }
            else if (fvField(*i) == copp_policer_action_yellow)
            {
                sai_packet_action_t policer_action;
                if (!getPolicerAction(fvValue(*i), policer_action))
                {
                    SWSS_LOG_ERROR("Invalid yellow action specified %s\n", fvValue(*i).c_str());
                    return task_process_status::task_invalid_entry;
                }
                attr.id = SAI_POLICER_ATTR_YELLOW_PACKET_ACTION;
                attr.value.u32 = policer_action;
                policer_attribs.push_back(attr);                
            }
            else
            {
                SWSS_LOG_ERROR("Unknown copp field specified:%s\n", fvField(*i).c_str());
                return task_process_status::task_invalid_entry;
            }
        }

        if (m_trap_map.find(key) != m_trap_map.end())
        {
            SWSS_LOG_DEBUG("found existing trap_group object:%s", key.c_str());
            if(!policer_attribs.empty())
            {
                attr.id = SAI_HOSTIF_TRAP_GROUP_ATTR_POLICER;
                attr.value.oid = SAI_NULL_OBJECT_ID;
                if (SAI_STATUS_SUCCESS != (sai_status = sai_hostif_api->get_trap_group_attribute(m_trap_map[key], 1, &attr))) {
                    SWSS_LOG_ERROR("Failed to get policer for trap gorup:%s, error:%d\n", key.c_str(), sai_status);
                    return task_process_status::task_failed;
                }
                SWSS_LOG_DEBUG("Obtained policer object:%llx for trap_group:%s", attr.value.oid, key.c_str());
                policer_id = attr.value.oid;                
                SWSS_LOG_DEBUG("Applying settings to existing policer for trap_group:%llx, name:%s", m_trap_map[key], key.c_str());
                for(sai_uint32_t ind = 0; ind < policer_attribs.size(); ind++)
                {
                    auto policer_attr = policer_attribs[ind];
                    if (SAI_STATUS_SUCCESS != (sai_status = sai_policer_api->set_policer_attribute(policer_id, &policer_attr)))
                    {
                        SWSS_LOG_ERROR("Failed to apply attribute[%d].id=%d to policer for trap group:%s, error:%d\n", ind, policer_attr.id, key.c_str(), sai_status);
                        return task_process_status::task_failed;
                    }                    
                }
            }
        }
        else
        {
            SWSS_LOG_DEBUG("Creating new trap_group object:%s", key.c_str());
            // create new policer and trap group and assign trap IDs
            sai_object_id_t new_trap = SAI_NULL_OBJECT_ID;
            attr.id = SAI_HOSTIF_TRAP_GROUP_ATTR_PRIO;// this is for old SAI implementation, will remove once new release is available.
            attr.value.u32 = 2;
            if (SAI_STATUS_SUCCESS != (sai_status = sai_hostif_api->create_hostif_trap_group(&new_trap, 1, &attr)))
            {
                SWSS_LOG_ERROR("Failed to create new trap_group with name:%s", key.c_str());
                return task_process_status::task_failed;
            }
            SWSS_LOG_DEBUG("Created new trap_group:%llx with name:%s", new_trap, key.c_str());
            m_trap_map[key] = new_trap;

            if (SAI_STATUS_SUCCESS != (sai_status = sai_policer_api->create_policer(&policer_id, policer_attribs.size(), policer_attribs.data())))
            {
                SWSS_LOG_ERROR("Failed to create policer for new trap_group:%llx, name:%s, error:%d", new_trap, key.c_str(), sai_status);
                return task_process_status::task_failed;
            }
            SWSS_LOG_DEBUG("Created new policer:%llx", policer_id);
            attr.id = SAI_HOSTIF_TRAP_GROUP_ATTR_POLICER;
            attr.value.oid = policer_id;
            if (SAI_STATUS_SUCCESS != (sai_status = sai_hostif_api->set_trap_group_attribute(new_trap, &attr))) {
                SWSS_LOG_ERROR("Failed to bind policer:%llx to trap gorup:%llx, name:%s, error:%d\n", policer_id, new_trap, key.c_str(), sai_status);
                return task_process_status::task_failed;
            }
            SWSS_LOG_DEBUG("Bound policer to the trap group");
        }

        if (!applyTrapIds(m_trap_map[key], trap_id_list))
        {
            return task_process_status::task_failed;
        }
        if (!queue_ind.empty())
        {
            SWSS_LOG_DEBUG("Applying queue information:%s to existing trap:%llx, name:%s", queue_ind.c_str(), m_trap_map[key], key.c_str());
            attr.id = SAI_HOSTIF_TRAP_GROUP_ATTR_QUEUE;
            attr.value.u32 = std::stoul(queue_ind);
            if (SAI_STATUS_SUCCESS != (sai_status = sai_hostif_api->set_trap_group_attribute(m_trap_map[key], &attr)))
            {
                SWSS_LOG_ERROR("Failed to apply queue attribute:%s to trap group:%llx, name:%s, error:%d\n", queue_ind.c_str(), m_trap_map[key], key.c_str(), sai_status);
                return task_process_status::task_failed;
            }                    
        }
    }
    else if (op == DEL_COMMAND)
    {
        // delete trap group and its policer.
        if (!removePolicerFromTrapGroup(m_trap_map[key]))
        {
            SWSS_LOG_ERROR("Failed to remove policer from trap gorup:%s\n", key.c_str());
            return task_process_status::task_failed;
        }
        if (SAI_STATUS_SUCCESS != (sai_status = sai_hostif_api->remove_hostif_trap_group(m_trap_map[key])))
        {
            SWSS_LOG_ERROR("Failed to remove trap group:%llx, name:%s\n", m_trap_map[key], key.c_str());
            return task_process_status::task_failed;
        }
        auto it_del = m_trap_map.find(key);
        m_trap_map.erase(it_del);
    }
    else
    {
        SWSS_LOG_ERROR("Unknown copp operation type %s\n", op.c_str());
        return task_process_status::task_invalid_entry;
    }    
    return task_process_status::task_success;
}

void CoppOrch::doTask(Consumer &consumer)
{
    SWSS_LOG_ENTER();
    if (consumer.m_toSync.empty()) 
    {
        return;
    }
    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end()) 
    {
        KeyOpFieldsValuesTuple tuple = it->second;
        string table_name = consumer.m_consumer->getTableName();
        if (table_name != APP_COPP_TABLE_NAME) 
        {
            SWSS_LOG_ERROR("Unrecognised copp table encountered:%s\n", table_name.c_str());
            it = consumer.m_toSync.erase(it);
            continue;
        }
        task_process_status task_status = processCoppRule(consumer);
        switch(task_status)
        {
        case task_process_status::task_success :
            it = consumer.m_toSync.erase(it);
            break;
        case task_process_status::task_invalid_entry:
            SWSS_LOG_ERROR("Invalid copp task item was encountered, removing from queue.");
            dumpTuple(consumer, tuple);
            it = consumer.m_toSync.erase(it);
            break;
        case task_process_status::task_failed:
            SWSS_LOG_ERROR("Processing copp task item failed, exiting. ");
            dumpTuple(consumer, tuple);
            return;
        case task_process_status::task_need_retry:
            SWSS_LOG_ERROR("Processing copp task item failed, will retry.");
            dumpTuple(consumer, tuple);
            it++;
        }
    }
}

