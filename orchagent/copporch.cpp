#include "sai.h"
#include "tokenize.h"
#include "copporch.h"
#include "logger.h"

#include <sstream>
#include <iostream>

using namespace swss;

extern sai_hostif_api_t*    sai_hostif_api;
extern sai_policer_api_t*   sai_policer_api;

std::map<string, sai_meter_type_t> policer_meter_map = {
    {"packets", SAI_METER_TYPE_PACKETS},
    {"bytes", SAI_METER_TYPE_BYTES}
};

std::map<string, sai_policer_mode_t> policer_mode_map = {
    {"sr_tcm", SAI_POLICER_MODE_Sr_TCM},
    {"tr_tcm", SAI_POLICER_MODE_Tr_TCM},
    {"storm",  SAI_POLICER_MODE_STORM_CONTROL}
};

std::map<string, sai_policer_color_source_t> policer_color_aware_map = {
    {"aware", SAI_POLICER_COLOR_SOURCE_AWARE},
    {"blind", SAI_POLICER_COLOR_SOURCE_BLIND}
};

std::map<string, sai_hostif_trap_id_t> trap_id_map = {
    {"stp", SAI_HOSTIF_TRAP_ID_STP},
    {"lacp", SAI_HOSTIF_TRAP_ID_LACP},
    {"eapol", SAI_HOSTIF_TRAP_ID_EAPOL},
    {"lldp", SAI_HOSTIF_TRAP_ID_LLDP},
    {"pvrst", SAI_HOSTIF_TRAP_ID_PVRST},
    {"igmp_query", SAI_HOSTIF_TRAP_ID_IGMP_TYPE_QUERY},
    {"igmp_leave", SAI_HOSTIF_TRAP_ID_IGMP_TYPE_LEAVE},
    {"igmp_v1_report", SAI_HOSTIF_TRAP_ID_IGMP_TYPE_V1_REPORT},
    {"igmp_v2_report", SAI_HOSTIF_TRAP_ID_IGMP_TYPE_V2_REPORT},
    {"igmp_v3_report", SAI_HOSTIF_TRAP_ID_IGMP_TYPE_V3_REPORT},
    {"sample_packet", SAI_HOSTIF_TRAP_ID_SAMPLEPACKET},
    {"switch_cust_range", SAI_HOSTIF_TRAP_ID_SWITCH_CUSTOM_RANGE_BASE},
    {"arp_req", SAI_HOSTIF_TRAP_ID_ARP_REQUEST},
    {"arp_resp", SAI_HOSTIF_TRAP_ID_ARP_RESPONSE},
    {"dhcp", SAI_HOSTIF_TRAP_ID_DHCP},
    {"ospf", SAI_HOSTIF_TRAP_ID_OSPF},
    {"pim", SAI_HOSTIF_TRAP_ID_PIM},
    {"vrrp", SAI_HOSTIF_TRAP_ID_VRRP},
    {"bgp", SAI_HOSTIF_TRAP_ID_BGP},
    {"dhcpv6", SAI_HOSTIF_TRAP_ID_DHCPV6},
    {"ospvfv6", SAI_HOSTIF_TRAP_ID_OSPFV6},
    {"vrrpv6", SAI_HOSTIF_TRAP_ID_VRRPV6},
    {"bgpv6", SAI_HOSTIF_TRAP_ID_BGPV6},
    {"neigh_discovery", SAI_HOSTIF_TRAP_ID_IPV6_NEIGHBOR_DISCOVERY},
    {"mld_v1_v2", SAI_HOSTIF_TRAP_ID_IPV6_MLD_V1_V2},
    {"mld_v1_report", SAI_HOSTIF_TRAP_ID_IPV6_MLD_V1_REPORT},
    {"mld_v2_done", SAI_HOSTIF_TRAP_ID_IPV6_MLD_V1_DONE},
    {"mld_v2_report", SAI_HOSTIF_TRAP_ID_MLD_V2_REPORT},
    {"ip2me", SAI_HOSTIF_TRAP_ID_IP2ME},
    {"ssh", SAI_HOSTIF_TRAP_ID_SSH},
    {"snmp", SAI_HOSTIF_TRAP_ID_SNMP},
    {"router_custom_range", SAI_HOSTIF_TRAP_ID_ROUTER_CUSTOM_RANGE_BASE},
    {"l3_mtu_error", SAI_HOSTIF_TRAP_ID_L3_MTU_ERROR},
    {"ttl", SAI_HOSTIF_TRAP_ID_TTL_ERROR}
};

std::map<string, sai_packet_action_t> packet_action_map = {
    {"drop", SAI_PACKET_ACTION_DROP},
    {"forward", SAI_PACKET_ACTION_FORWARD},
    {"copy", SAI_PACKET_ACTION_COPY},
    {"copy_cancel", SAI_PACKET_ACTION_COPY_CANCEL},
    {"trap", SAI_PACKET_ACTION_TRAP},
    {"log", SAI_PACKET_ACTION_LOG},
    {"deny", SAI_PACKET_ACTION_DENY},
    {"transit", SAI_PACKET_ACTION_TRANSIT}
};

CoppOrch::CoppOrch(DBConnector *db, string tableName) :
    Orch(db, tableName)
{
    SWSS_LOG_ENTER();
};

bool CoppOrch::getTrapID(string &trap_id_str, sai_hostif_trap_id_t &trap_id) const
{
    SWSS_LOG_ENTER();
    auto it = trap_id_map.find(trap_id_str);
    if(it == trap_id_map.end())
    {
        SWSS_LOG_ERROR("Invalid trap_id specified %s\n", trap_id_str.c_str());
        return false;
    }
    trap_id = trap_id_map[trap_id_str];
    SWSS_LOG_DEBUG("Output value:%d", trap_id);
    return true;
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

bool CoppOrch::applyTrapIds(sai_object_id_t trap_group, vector<string> &trap_id_name_list, std::vector<sai_attribute_t> &trap_id_attribs)
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
        SWSS_LOG_DEBUG("set trap attr.1:%d", attr.id);
        if (SAI_STATUS_SUCCESS != (sai_status = sai_hostif_api->set_trap_attribute(trap_id, &attr)))
        {
            SWSS_LOG_ERROR("Failed to apply trap_id:%d to trap group:%llx, error:%d\n", trap_id, trap_group, sai_status);
            return false;
        }
        for(auto trap_id_attr : trap_id_attribs)
        {
            SWSS_LOG_DEBUG("set trap attr.2:%d", trap_id_attr.id);
            if (SAI_STATUS_SUCCESS != (sai_status = sai_hostif_api->set_trap_attribute(trap_id, &trap_id_attr)))
            {
                SWSS_LOG_ERROR("Failed to apply trap_id attribute:%d to trap_id:%d, error:%d\n", trap_id_attr.id, trap_id, sai_status);
                return false;
            }
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
    if(SAI_NULL_OBJECT_ID == policer_id)
    {
        SWSS_LOG_DEBUG("No policer is attached to trap gorup:%llx\n", trap_group);
        return true;
    }

    attr.value.oid = SAI_NULL_OBJECT_ID;
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
    std::vector<sai_attribute_t> trap_gr_attribs;
    std::vector<sai_attribute_t> trap_id_attribs;
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
                trap_id_list = tokenize(fvValue(*i), list_item_delimiter);
            }
            else if (fvField(*i) == copp_queue_field)
            {
                queue_ind = fvValue(*i);
                SWSS_LOG_DEBUG("queue data:%s", queue_ind.c_str());
                attr.id = SAI_HOSTIF_TRAP_GROUP_ATTR_QUEUE;
                attr.value.u32 = std::stoul(queue_ind);                
                trap_gr_attribs.push_back(attr);
            }
            //
            // Trap id related attributes
            //
            else if (fvField(*i) == copp_trap_action_field)
            {
                SWSS_LOG_DEBUG("trap action:%s", fvValue(*i).c_str());
                sai_packet_action_t trap_action = packet_action_map.at(fvValue(*i));
                attr.id = SAI_HOSTIF_TRAP_ATTR_PACKET_ACTION;
                attr.value.s32 = trap_action;
                trap_id_attribs.push_back(attr);
            }
            //
            // process policer attributes
            //
            else if (fvField(*i) == copp_policer_meter_type_field)
            {
                SWSS_LOG_DEBUG("policer meter:%s", fvValue(*i).c_str());
                sai_meter_type_t meter_value = policer_meter_map.at(fvValue(*i));                
                attr.id = SAI_POLICER_ATTR_METER_TYPE;
                attr.value.s32 = meter_value;
                policer_attribs.push_back(attr);
            }
            else if (fvField(*i) == copp_policer_mode_field)
            {
                SWSS_LOG_DEBUG("policer mode:%s", fvValue(*i).c_str());
                sai_policer_mode_t mode = policer_mode_map.at(fvValue(*i));
                attr.id = SAI_POLICER_ATTR_MODE;
                attr.value.s32 = mode;
                policer_attribs.push_back(attr);
            }        
            else if (fvField(*i) == copp_policer_color_field)
            {
                SWSS_LOG_DEBUG("policer color mode:%s", fvValue(*i).c_str());
                sai_policer_color_source_t color = policer_color_aware_map.at(fvValue(*i));
                attr.id = SAI_POLICER_ATTR_COLOR_SOURCE;
                attr.value.s32 = color;
                policer_attribs.push_back(attr);
            }        
            else if (fvField(*i) == copp_policer_cbs_field)  
            {
                attr.id = SAI_POLICER_ATTR_CBS;
                attr.value.u64 = std::stoul(fvValue(*i));
                policer_attribs.push_back(attr);
                SWSS_LOG_DEBUG("obtained cbs:%d", attr.value.u64);
            }
            else if (fvField(*i) == copp_policer_cir_field)          
            {
                attr.id = SAI_POLICER_ATTR_CIR;
                attr.value.u64 = std::stoul(fvValue(*i));
                policer_attribs.push_back(attr);
                SWSS_LOG_DEBUG("obtained cir:%d", attr.value.u64);
            }
            else if (fvField(*i) == copp_policer_pbs_field)          
            {
                attr.id = SAI_POLICER_ATTR_PBS;
                attr.value.u64 = std::stoul(fvValue(*i));
                policer_attribs.push_back(attr);
                SWSS_LOG_DEBUG("obtained pbs:%d", attr.value.u64);
            }
            else if (fvField(*i) == copp_policer_pir_field)          
            {
                attr.id = SAI_POLICER_ATTR_PIR;
                attr.value.u64 = std::stoul(fvValue(*i));
                policer_attribs.push_back(attr);
                SWSS_LOG_DEBUG("obtained pir:%d", attr.value.u64);
            }
            else if (fvField(*i) == copp_policer_action_green_field)
            {
                SWSS_LOG_DEBUG("green action:%s", queue_ind.c_str());
                sai_packet_action_t policer_action = packet_action_map.at(fvValue(*i));
                attr.id = SAI_POLICER_ATTR_GREEN_PACKET_ACTION;
                attr.value.s32 = policer_action;
                policer_attribs.push_back(attr);
            }
            else if (fvField(*i) == copp_policer_action_red_field) 
            {
                SWSS_LOG_DEBUG("red action:%s", queue_ind.c_str());
                sai_packet_action_t policer_action = packet_action_map.at(fvValue(*i));
                attr.id = SAI_POLICER_ATTR_RED_PACKET_ACTION;
                attr.value.s32 = policer_action;
                policer_attribs.push_back(attr);
            }
            else if (fvField(*i) == copp_policer_action_yellow_field)
            {
                SWSS_LOG_DEBUG("yellowaction:%s", queue_ind.c_str());
                sai_packet_action_t policer_action = packet_action_map.at(fvValue(*i));
                attr.id = SAI_POLICER_ATTR_YELLOW_PACKET_ACTION;
                attr.value.s32 = policer_action;
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
                if (SAI_STATUS_SUCCESS != (sai_status = sai_hostif_api->get_trap_group_attribute(m_trap_map[key], 1, &attr)))
                {
                    SWSS_LOG_ERROR("Failed to get policer for trap gorup:%s, error:%d\n", key.c_str(), sai_status);
                    return task_process_status::task_failed;
                }
                SWSS_LOG_DEBUG("Obtained policer object:%llx for trap_group:%s", attr.value.oid, key.c_str());
                policer_id = attr.value.oid;                
                SWSS_LOG_DEBUG("Applying settings to existing policer:%llx for trap_group:%llx, name:%s", policer_id, m_trap_map[key], key.c_str());
                if(SAI_NULL_OBJECT_ID == policer_id)
                {
                    SWSS_LOG_WARN("Creating policer for existing Trap group:%llx (name:%s).", m_trap_map[key], key.c_str());
                    if (SAI_STATUS_SUCCESS != (sai_status = sai_policer_api->create_policer(&policer_id, policer_attribs.size(), policer_attribs.data())))
                    {
                        SWSS_LOG_ERROR("Failed to create policer for existing trap_group:%llx, name:%s, error:%d", m_trap_map[key], key.c_str(), sai_status);
                        return task_process_status::task_failed;
                    }
                    SWSS_LOG_DEBUG("Created policer:%llx for existing trap group", policer_id);
                }
                else 
                {
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
            SWSS_LOG_DEBUG("Applying trap group attributes");
            for(sai_uint32_t ind = 0; ind < trap_gr_attribs.size(); ind++)
            {
                auto trap_gr_attr = trap_gr_attribs[ind];
                SWSS_LOG_DEBUG("Applying trap group attribute:%d", trap_gr_attr.id);
                if (SAI_STATUS_SUCCESS != (sai_status = sai_hostif_api->set_trap_group_attribute(m_trap_map[key], &trap_gr_attr)))
                {
                    SWSS_LOG_ERROR("Failed to apply attribute:%d to trap group:%llx, name:%s, error:%d\n", trap_gr_attr.id, m_trap_map[key], key.c_str(), sai_status);
                    return task_process_status::task_failed;
                }
            }                
        }
        else
        {
            SWSS_LOG_DEBUG("Creating new trap_group object:%s", key.c_str());
            sai_object_id_t new_trap;
            if (SAI_STATUS_SUCCESS != (sai_status = sai_hostif_api->create_hostif_trap_group(&new_trap, trap_gr_attribs.size(), trap_gr_attribs.data())))
            {
                SWSS_LOG_ERROR("Failed to create new trap_group with name:%s", key.c_str());
                return task_process_status::task_failed;
            }
            SWSS_LOG_DEBUG("Created new trap_group:%llx with name:%s", new_trap, key.c_str());
            m_trap_map[key] = new_trap;

            if(!policer_attribs.empty())
            {
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
        }

        //
        // The channel is always the NETDEV device.
        //
        attr.id = SAI_HOSTIF_TRAP_ATTR_TRAP_CHANNEL;
        attr.value.s32 = SAI_HOSTIF_TRAP_CHANNEL_NETDEV;
        trap_id_attribs.push_back(attr);

        if (!applyTrapIds(m_trap_map[key], trap_id_list, trap_id_attribs))
        {
            return task_process_status::task_failed;
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
        task_process_status task_status;
        try
        {
            task_status = processCoppRule(consumer);
        }
        catch(const std::out_of_range e)
        {
            SWSS_LOG_ERROR("processing copp rule threw exception:%s", e.what());
            task_status = task_process_status::task_invalid_entry;            
        }
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

