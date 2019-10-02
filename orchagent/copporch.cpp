#include "sai.h"
#include "copporch.h"
#include "portsorch.h"
#include "tokenize.h"
#include "logger.h"

#include <inttypes.h>
#include <sstream>
#include <iostream>

using namespace swss;
using namespace std;

extern sai_hostif_api_t*    sai_hostif_api;
extern sai_policer_api_t*   sai_policer_api;
extern sai_switch_api_t*    sai_switch_api;

extern sai_object_id_t      gSwitchId;
extern PortsOrch*           gPortsOrch;

static map<string, sai_meter_type_t> policer_meter_map = {
    {"packets", SAI_METER_TYPE_PACKETS},
    {"bytes", SAI_METER_TYPE_BYTES}
};

static map<string, sai_policer_mode_t> policer_mode_map = {
    {"sr_tcm", SAI_POLICER_MODE_SR_TCM},
    {"tr_tcm", SAI_POLICER_MODE_TR_TCM},
    {"storm",  SAI_POLICER_MODE_STORM_CONTROL}
};

static map<string, sai_policer_color_source_t> policer_color_aware_map = {
    {"aware", SAI_POLICER_COLOR_SOURCE_AWARE},
    {"blind", SAI_POLICER_COLOR_SOURCE_BLIND}
};

static map<string, sai_hostif_trap_type_t> trap_id_map = {
    {"stp", SAI_HOSTIF_TRAP_TYPE_STP},
    {"lacp", SAI_HOSTIF_TRAP_TYPE_LACP},
    {"eapol", SAI_HOSTIF_TRAP_TYPE_EAPOL},
    {"lldp", SAI_HOSTIF_TRAP_TYPE_LLDP},
    {"pvrst", SAI_HOSTIF_TRAP_TYPE_PVRST},
    {"igmp_query", SAI_HOSTIF_TRAP_TYPE_IGMP_TYPE_QUERY},
    {"igmp_leave", SAI_HOSTIF_TRAP_TYPE_IGMP_TYPE_LEAVE},
    {"igmp_v1_report", SAI_HOSTIF_TRAP_TYPE_IGMP_TYPE_V1_REPORT},
    {"igmp_v2_report", SAI_HOSTIF_TRAP_TYPE_IGMP_TYPE_V2_REPORT},
    {"igmp_v3_report", SAI_HOSTIF_TRAP_TYPE_IGMP_TYPE_V3_REPORT},
    {"sample_packet", SAI_HOSTIF_TRAP_TYPE_SAMPLEPACKET},
    {"switch_cust_range", SAI_HOSTIF_TRAP_TYPE_SWITCH_CUSTOM_RANGE_BASE},
    {"arp_req", SAI_HOSTIF_TRAP_TYPE_ARP_REQUEST},
    {"arp_resp", SAI_HOSTIF_TRAP_TYPE_ARP_RESPONSE},
    {"dhcp", SAI_HOSTIF_TRAP_TYPE_DHCP},
    {"ospf", SAI_HOSTIF_TRAP_TYPE_OSPF},
    {"pim", SAI_HOSTIF_TRAP_TYPE_PIM},
    {"vrrp", SAI_HOSTIF_TRAP_TYPE_VRRP},
    {"bgp", SAI_HOSTIF_TRAP_TYPE_BGP},
    {"dhcpv6", SAI_HOSTIF_TRAP_TYPE_DHCPV6},
    {"ospfv6", SAI_HOSTIF_TRAP_TYPE_OSPFV6},
    {"vrrpv6", SAI_HOSTIF_TRAP_TYPE_VRRPV6},
    {"bgpv6", SAI_HOSTIF_TRAP_TYPE_BGPV6},
    {"neigh_discovery", SAI_HOSTIF_TRAP_TYPE_IPV6_NEIGHBOR_DISCOVERY},
    {"mld_v1_v2", SAI_HOSTIF_TRAP_TYPE_IPV6_MLD_V1_V2},
    {"mld_v1_report", SAI_HOSTIF_TRAP_TYPE_IPV6_MLD_V1_REPORT},
    {"mld_v1_done", SAI_HOSTIF_TRAP_TYPE_IPV6_MLD_V1_DONE},
    {"mld_v2_report", SAI_HOSTIF_TRAP_TYPE_MLD_V2_REPORT},
    {"ip2me", SAI_HOSTIF_TRAP_TYPE_IP2ME},
    {"ssh", SAI_HOSTIF_TRAP_TYPE_SSH},
    {"snmp", SAI_HOSTIF_TRAP_TYPE_SNMP},
    {"router_custom_range", SAI_HOSTIF_TRAP_TYPE_ROUTER_CUSTOM_RANGE_BASE},
    {"l3_mtu_error", SAI_HOSTIF_TRAP_TYPE_L3_MTU_ERROR},
    {"ttl_error", SAI_HOSTIF_TRAP_TYPE_TTL_ERROR},
    {"udld", SAI_HOSTIF_TRAP_TYPE_UDLD}
};

static map<string, sai_packet_action_t> packet_action_map = {
    {"drop", SAI_PACKET_ACTION_DROP},
    {"forward", SAI_PACKET_ACTION_FORWARD},
    {"copy", SAI_PACKET_ACTION_COPY},
    {"copy_cancel", SAI_PACKET_ACTION_COPY_CANCEL},
    {"trap", SAI_PACKET_ACTION_TRAP},
    {"log", SAI_PACKET_ACTION_LOG},
    {"deny", SAI_PACKET_ACTION_DENY},
    {"transit", SAI_PACKET_ACTION_TRANSIT}
};

const string default_trap_group = "default";
const vector<sai_hostif_trap_type_t> default_trap_ids = {
    SAI_HOSTIF_TRAP_TYPE_TTL_ERROR
};

CoppOrch::CoppOrch(DBConnector *db, string tableName) :
    Orch(db, tableName)
{
    SWSS_LOG_ENTER();

    initDefaultHostIntfTable();
    initDefaultTrapGroup();
    initDefaultTrapIds();
};

void CoppOrch::initDefaultHostIntfTable()
{
    SWSS_LOG_ENTER();

    sai_object_id_t default_hostif_table_id;
    vector<sai_attribute_t> attrs;

    sai_attribute_t attr;
    attr.id = SAI_HOSTIF_TABLE_ENTRY_ATTR_TYPE;
    attr.value.s32 = SAI_HOSTIF_TABLE_ENTRY_TYPE_WILDCARD;
    attrs.push_back(attr);

    attr.id = SAI_HOSTIF_TABLE_ENTRY_ATTR_CHANNEL_TYPE;
    attr.value.s32 = SAI_HOSTIF_TABLE_ENTRY_CHANNEL_TYPE_NETDEV_PHYSICAL_PORT;
    attrs.push_back(attr);

    sai_status_t status = sai_hostif_api->create_hostif_table_entry(
        &default_hostif_table_id, gSwitchId, (uint32_t)attrs.size(), attrs.data());
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to create default host interface table, rv:%d", status);
        throw "CoppOrch initialization failure";
    }

    SWSS_LOG_NOTICE("Create default host interface table");
}

void CoppOrch::initDefaultTrapIds()
{
    SWSS_LOG_ENTER();

    sai_attribute_t attr;
    vector<sai_attribute_t> trap_id_attrs;

    attr.id = SAI_HOSTIF_TRAP_ATTR_PACKET_ACTION;
    attr.value.s32 = SAI_PACKET_ACTION_TRAP;
    trap_id_attrs.push_back(attr);

    attr.id = SAI_HOSTIF_TRAP_ATTR_TRAP_GROUP;
    attr.value.oid = m_trap_group_map[default_trap_group];
    trap_id_attrs.push_back(attr);

    /* Mellanox platform doesn't support trap priority setting */
    char *platform = getenv("platform");
    if (!platform || !strstr(platform, MLNX_PLATFORM_SUBSTRING))
    {
        attr.id = SAI_HOSTIF_TRAP_ATTR_TRAP_PRIORITY;
        attr.value.u32 = 0;
        trap_id_attrs.push_back(attr);
    }

    if (!applyAttributesToTrapIds(m_trap_group_map[default_trap_group], default_trap_ids, trap_id_attrs))
    {
        SWSS_LOG_ERROR("Failed to set attributes to default trap IDs");
        throw "CoppOrch initialization failure";
    }

    SWSS_LOG_INFO("Set attributes to default trap IDs");
}

void CoppOrch::initDefaultTrapGroup()
{
    SWSS_LOG_ENTER();

    sai_attribute_t attr;
    attr.id = SAI_SWITCH_ATTR_DEFAULT_TRAP_GROUP;

    sai_status_t status = sai_switch_api->get_switch_attribute(gSwitchId, 1, &attr);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to get default trap group, rv:%d", status);
        throw "CoppOrch initialization failure";
    }

    SWSS_LOG_INFO("Get default trap group");
    m_trap_group_map[default_trap_group] = attr.value.oid;
}

void CoppOrch::getTrapIdList(vector<string> &trap_id_name_list, vector<sai_hostif_trap_type_t> &trap_id_list) const
{
    SWSS_LOG_ENTER();
    for (auto trap_id_str : trap_id_name_list)
    {
        sai_hostif_trap_type_t trap_id;
        SWSS_LOG_DEBUG("processing trap_id:%s", trap_id_str.c_str());
        trap_id = trap_id_map.at(trap_id_str);
        SWSS_LOG_DEBUG("Pushing trap_id:%d", trap_id);
        trap_id_list.push_back(trap_id);
    }
}

bool CoppOrch::applyAttributesToTrapIds(sai_object_id_t trap_group_id,
                                        const vector<sai_hostif_trap_type_t> &trap_id_list,
                                        vector<sai_attribute_t> &trap_id_attribs)
{
    for (auto trap_id : trap_id_list)
    {
        sai_attribute_t attr;
        vector<sai_attribute_t> attrs;

        attr.id = SAI_HOSTIF_TRAP_ATTR_TRAP_TYPE;
        attr.value.s32 = trap_id;
        attrs.push_back(attr);

        attrs.insert(attrs.end(), trap_id_attribs.begin(), trap_id_attribs.end());

        sai_object_id_t hostif_trap_id;
        sai_status_t status = sai_hostif_api->create_hostif_trap(&hostif_trap_id, gSwitchId, (uint32_t)attrs.size(), attrs.data());
        if (status != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_ERROR("Failed to create trap %d, rv:%d", trap_id, status);
            return false;
        }
        m_syncdTrapIds[trap_id] = trap_group_id;
    }

    return true;
}

bool CoppOrch::applyTrapIds(sai_object_id_t trap_group, vector<string> &trap_id_name_list, vector<sai_attribute_t> &trap_id_attribs)
{
    SWSS_LOG_ENTER();

    vector<sai_hostif_trap_type_t> trap_id_list;

    getTrapIdList(trap_id_name_list, trap_id_list);

    sai_attribute_t attr;
    attr.id = SAI_HOSTIF_TRAP_ATTR_TRAP_GROUP;
    attr.value.oid = trap_group;
    trap_id_attribs.push_back(attr);

    return applyAttributesToTrapIds(trap_group, trap_id_list, trap_id_attribs);
}

bool CoppOrch::removePolicer(string trap_group_name)
{
    SWSS_LOG_ENTER();

    sai_attribute_t attr;
    sai_status_t sai_status;
    sai_object_id_t policer_id = getPolicer(trap_group_name);

    if (SAI_NULL_OBJECT_ID == policer_id)
    {
        SWSS_LOG_INFO("No policer is attached to trap group %s", trap_group_name.c_str());
        return true;
    }

    attr.id = SAI_HOSTIF_TRAP_GROUP_ATTR_POLICER;
    attr.value.oid = SAI_NULL_OBJECT_ID;

    sai_status = sai_hostif_api->set_hostif_trap_group_attribute(m_trap_group_map[trap_group_name], &attr);
    if (sai_status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to set policer to NULL for trap group %s, rc=%d", trap_group_name.c_str(), sai_status);
        return false;
    }

    sai_status = sai_policer_api->remove_policer(policer_id);
    if (sai_status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to remove policer for trap group %s, rc=%d", trap_group_name.c_str(), sai_status);
        return false;
    }

    SWSS_LOG_NOTICE("Remove policer for trap group %s", trap_group_name.c_str());
    m_trap_group_policer_map.erase(m_trap_group_map[trap_group_name]);
    return true;
}

sai_object_id_t CoppOrch::getPolicer(string trap_group_name)
{
    SWSS_LOG_ENTER();

    SWSS_LOG_DEBUG("trap group name:%s:", trap_group_name.c_str());
    if (m_trap_group_map.find(trap_group_name) == m_trap_group_map.end())
    {
        return SAI_NULL_OBJECT_ID;
    }
    SWSS_LOG_DEBUG("trap group id:%" PRIx64, m_trap_group_map[trap_group_name]);
    if (m_trap_group_policer_map.find(m_trap_group_map[trap_group_name]) == m_trap_group_policer_map.end())
    {
        return SAI_NULL_OBJECT_ID;
    }
    SWSS_LOG_DEBUG("trap group policer id:%" PRIx64, m_trap_group_policer_map[m_trap_group_map[trap_group_name]]);
    return m_trap_group_policer_map[m_trap_group_map[trap_group_name]];
}

bool CoppOrch::createPolicer(string trap_group_name, vector<sai_attribute_t> &policer_attribs)
{
    SWSS_LOG_ENTER();

    sai_object_id_t policer_id;
    sai_status_t sai_status;

    sai_status = sai_policer_api->create_policer(&policer_id, gSwitchId, (uint32_t)policer_attribs.size(), policer_attribs.data());
    if (sai_status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to create policer trap group %s, rc=%d", trap_group_name.c_str(), sai_status);
        return false;
    }

    SWSS_LOG_NOTICE("Create policer for trap group %s", trap_group_name.c_str());

    sai_attribute_t attr;
    attr.id = SAI_HOSTIF_TRAP_GROUP_ATTR_POLICER;
    attr.value.oid = policer_id;

    sai_status = sai_hostif_api->set_hostif_trap_group_attribute(m_trap_group_map[trap_group_name], &attr);
    if (sai_status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to bind policer to trap group %s, rc=%d", trap_group_name.c_str(), sai_status);
        return false;
    }

    SWSS_LOG_NOTICE("Bind policer to trap group %s:", trap_group_name.c_str());
    m_trap_group_policer_map[m_trap_group_map[trap_group_name]] = policer_id;
    return true;
}

task_process_status CoppOrch::processCoppRule(Consumer& consumer)
{
    SWSS_LOG_ENTER();

    sai_status_t sai_status;
    vector<string> trap_id_list;
    string queue_ind;
    auto it = consumer.m_toSync.begin();
    KeyOpFieldsValuesTuple tuple = it->second;
    string trap_group_name = kfvKey(tuple);
    string op = kfvOp(tuple);

    vector<sai_attribute_t> trap_gr_attribs;
    vector<sai_attribute_t> trap_id_attribs;
    vector<sai_attribute_t> policer_attribs;

    if (op == SET_COMMAND)
    {
        for (auto i = kfvFieldsValues(tuple).begin(); i != kfvFieldsValues(tuple).end(); i++)
        {
            sai_attribute_t attr;

            if (fvField(*i) == copp_trap_id_list)
            {
                trap_id_list = tokenize(fvValue(*i), list_item_delimiter);
            }
            else if (fvField(*i) == copp_queue_field)
            {
                attr.id = SAI_HOSTIF_TRAP_GROUP_ATTR_QUEUE;
                attr.value.u32 = (uint32_t)stoul(fvValue(*i));
                trap_gr_attribs.push_back(attr);
            }
            //
            // Trap related attributes
            //
            else if (fvField(*i) == copp_trap_action_field)
            {
                sai_packet_action_t trap_action = packet_action_map.at(fvValue(*i));
                attr.id = SAI_HOSTIF_TRAP_ATTR_PACKET_ACTION;
                attr.value.s32 = trap_action;
                trap_id_attribs.push_back(attr);
            }
            else if (fvField(*i) == copp_trap_priority_field)
            {
                /* Mellanox platform doesn't support trap priority setting */
                char *platform = getenv("platform");
                if (!platform || !strstr(platform, MLNX_PLATFORM_SUBSTRING))
                {
                    attr.id = SAI_HOSTIF_TRAP_ATTR_TRAP_PRIORITY,
                    attr.value.u32 = (uint32_t)stoul(fvValue(*i));
                    trap_id_attribs.push_back(attr);
                }
            }
            //
            // process policer attributes
            //
            else if (fvField(*i) == copp_policer_meter_type_field)
            {
                sai_meter_type_t meter_value = policer_meter_map.at(fvValue(*i));
                attr.id = SAI_POLICER_ATTR_METER_TYPE;
                attr.value.s32 = meter_value;
                policer_attribs.push_back(attr);
            }
            else if (fvField(*i) == copp_policer_mode_field)
            {
                sai_policer_mode_t mode = policer_mode_map.at(fvValue(*i));
                attr.id = SAI_POLICER_ATTR_MODE;
                attr.value.s32 = mode;
                policer_attribs.push_back(attr);
            }
            else if (fvField(*i) == copp_policer_color_field)
            {
                sai_policer_color_source_t color = policer_color_aware_map.at(fvValue(*i));
                attr.id = SAI_POLICER_ATTR_COLOR_SOURCE;
                attr.value.s32 = color;
                policer_attribs.push_back(attr);
            }
            else if (fvField(*i) == copp_policer_cbs_field)
            {
                attr.id = SAI_POLICER_ATTR_CBS;
                attr.value.u64 = stoul(fvValue(*i));
                policer_attribs.push_back(attr);
            }
            else if (fvField(*i) == copp_policer_cir_field)
            {
                attr.id = SAI_POLICER_ATTR_CIR;
                attr.value.u64 = stoul(fvValue(*i));
                policer_attribs.push_back(attr);
            }
            else if (fvField(*i) == copp_policer_pbs_field)
            {
                attr.id = SAI_POLICER_ATTR_PBS;
                attr.value.u64 = stoul(fvValue(*i));
                policer_attribs.push_back(attr);
            }
            else if (fvField(*i) == copp_policer_pir_field)
            {
                attr.id = SAI_POLICER_ATTR_PIR;
                attr.value.u64 = stoul(fvValue(*i));
                policer_attribs.push_back(attr);
            }
            else if (fvField(*i) == copp_policer_action_green_field)
            {
                sai_packet_action_t policer_action = packet_action_map.at(fvValue(*i));
                attr.id = SAI_POLICER_ATTR_GREEN_PACKET_ACTION;
                attr.value.s32 = policer_action;
                policer_attribs.push_back(attr);
            }
            else if (fvField(*i) == copp_policer_action_red_field)
            {
                sai_packet_action_t policer_action = packet_action_map.at(fvValue(*i));
                attr.id = SAI_POLICER_ATTR_RED_PACKET_ACTION;
                attr.value.s32 = policer_action;
                policer_attribs.push_back(attr);
            }
            else if (fvField(*i) == copp_policer_action_yellow_field)
            {
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

        /* Set host interface trap group */
        if (m_trap_group_map.find(trap_group_name) != m_trap_group_map.end())
        {
            /* Create or set policer */
            if (!policer_attribs.empty())
            {
                sai_object_id_t policer_id = getPolicer(trap_group_name);
                if (SAI_NULL_OBJECT_ID == policer_id)
                {
                    SWSS_LOG_WARN("Creating policer for existing Trap group:%" PRIx64 " (name:%s).", m_trap_group_map[trap_group_name], trap_group_name.c_str());
                    if (!createPolicer(trap_group_name, policer_attribs))
                    {
                        return task_process_status::task_failed;
                    }
                    SWSS_LOG_DEBUG("Created policer:%" PRIx64 " for existing trap group", policer_id);
                }
                else
                {
                /* TODO: We should really only set changed attributes.
                The changes need to detected either by orch agent submodule or by the orch agent framework. */
                    for (sai_uint32_t ind = 0; ind < policer_attribs.size(); ind++)
                    {
                        auto policer_attr = policer_attribs[ind];
                        sai_status = sai_policer_api->set_policer_attribute(policer_id, &policer_attr);
                        if (sai_status != SAI_STATUS_SUCCESS)
                        {
                            SWSS_LOG_ERROR("Failed to apply attribute[%d].id=%d to policer for trap group:%s, error:%d\n", ind, policer_attr.id, trap_group_name.c_str(), sai_status);
                            return task_process_status::task_failed;
                        }
                    }
                }
            }

            for (sai_uint32_t ind = 0; ind < trap_gr_attribs.size(); ind++)
            {
                auto trap_gr_attr = trap_gr_attribs[ind];

                sai_status = sai_hostif_api->set_hostif_trap_group_attribute(m_trap_group_map[trap_group_name], &trap_gr_attr);
                if (sai_status != SAI_STATUS_SUCCESS)
                {
                    SWSS_LOG_ERROR("Failed to apply attribute:%d to trap group:%" PRIx64 ", name:%s, error:%d\n", trap_gr_attr.id, m_trap_group_map[trap_group_name], trap_group_name.c_str(), sai_status);
                    return task_process_status::task_failed;
                }
                SWSS_LOG_NOTICE("Set trap group %s to host interface", trap_group_name.c_str());
            }
        }
        /* Create host interface trap group */
        else
        {
            sai_object_id_t new_trap;

            sai_status = sai_hostif_api->create_hostif_trap_group(&new_trap, gSwitchId, (uint32_t)trap_gr_attribs.size(), trap_gr_attribs.data());
            if (sai_status != SAI_STATUS_SUCCESS)
            {
                SWSS_LOG_ERROR("Failed to create host interface trap group %s, rc=%d", trap_group_name.c_str(), sai_status);
                return task_process_status::task_failed;
            }

            SWSS_LOG_NOTICE("Create host interface trap group %s", trap_group_name.c_str());
            m_trap_group_map[trap_group_name] = new_trap;

            /* Create policer */
            if (!policer_attribs.empty())
            {
                if (!createPolicer(trap_group_name, policer_attribs))
                {
                    return task_process_status::task_failed;
                }
            }
        }

        /* Apply traps to trap group */
        if (!applyTrapIds(m_trap_group_map[trap_group_name], trap_id_list, trap_id_attribs))
        {
            return task_process_status::task_failed;
        }
    }
    else if (op == DEL_COMMAND)
    {
        /* Remove policer if any */
        if (!removePolicer(trap_group_name))
        {
            SWSS_LOG_ERROR("Failed to remove policer from trap group %s", trap_group_name.c_str());
            return task_process_status::task_failed;
        }

        /* Do not remove default trap group */
        if (trap_group_name == default_trap_group)
        {
            SWSS_LOG_WARN("Cannot remove default trap group");
            return task_process_status::task_ignore;
        }

        /* Reset the trap IDs to default trap group with default attributes */
        vector<sai_hostif_trap_type_t> trap_ids_to_reset;
        for (auto it : m_syncdTrapIds)
        {
            if (it.second == m_trap_group_map[trap_group_name])
            {
                trap_ids_to_reset.push_back(it.first);
            }
        }

        sai_attribute_t attr;
        vector<sai_attribute_t> default_trap_attrs;

        attr.id = SAI_HOSTIF_TRAP_ATTR_PACKET_ACTION;
        attr.value.s32 = SAI_PACKET_ACTION_FORWARD;
        default_trap_attrs.push_back(attr);

        attr.id = SAI_HOSTIF_TRAP_ATTR_TRAP_GROUP;
        attr.value.oid = m_trap_group_map[default_trap_group];
        default_trap_attrs.push_back(attr);

        if (!applyAttributesToTrapIds(m_trap_group_map[default_trap_group], trap_ids_to_reset, default_trap_attrs))
        {
            SWSS_LOG_ERROR("Failed to reset traps to default trap group with default attributes");
            return task_process_status::task_failed;
        }

        sai_status = sai_hostif_api->remove_hostif_trap_group(m_trap_group_map[trap_group_name]);
        if (sai_status != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_ERROR("Failed to remove trap group %s", trap_group_name.c_str());
            return task_process_status::task_failed;
        }

        auto it_del = m_trap_group_map.find(trap_group_name);
        m_trap_group_map.erase(it_del);
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

    if (!gPortsOrch->allPortsReady())
    {
        return;
    }

    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
        KeyOpFieldsValuesTuple tuple = it->second;
        task_process_status task_status;

        try
        {
            task_status = processCoppRule(consumer);
        }
        catch(const out_of_range& e)
        {
            SWSS_LOG_ERROR("processing copp rule threw out_of_range exception:%s", e.what());
            task_status = task_process_status::task_invalid_entry;
        }
        catch(exception& e)
        {
            SWSS_LOG_ERROR("processing copp rule threw exception:%s", e.what());
            task_status = task_process_status::task_invalid_entry;
        }
        switch(task_status)
        {
            case task_process_status::task_success :
            case task_process_status::task_ignore  :
                it = consumer.m_toSync.erase(it);
                break;
            case task_process_status::task_invalid_entry:
                SWSS_LOG_ERROR("Invalid copp task item was encountered, removing from queue.");
                it = consumer.m_toSync.erase(it);
                break;
            case task_process_status::task_failed:
                SWSS_LOG_ERROR("Processing copp task item failed, exiting. ");
                return;
            case task_process_status::task_need_retry:
                SWSS_LOG_ERROR("Processing copp task item failed, will retry.");
                it++;
                break;
            default:
                SWSS_LOG_ERROR("Invalid task status:%d", task_status);
                return;
        }
    }
}
