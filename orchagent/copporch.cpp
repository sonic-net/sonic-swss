#include "sai.h"
#include "copporch.h"
#include "portsorch.h"
#include "tokenize.h"
#include "logger.h"

#include <inttypes.h>
#include <sstream>
#include <iostream>
#include <algorithm>

using namespace swss;
using namespace std;

extern sai_hostif_api_t*    sai_hostif_api;
extern sai_policer_api_t*   sai_policer_api;
extern sai_switch_api_t*    sai_switch_api;

extern sai_object_id_t      gSwitchId;
extern PortsOrch*           gPortsOrch;
extern bool                 gIsNatSupported;

static string               old_nat_group_name = "trap.group.nat";

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
    {"udld", SAI_HOSTIF_TRAP_TYPE_UDLD},
    {"bfd", SAI_HOSTIF_TRAP_TYPE_BFD},
    {"bfdv6", SAI_HOSTIF_TRAP_TYPE_BFDV6},
    {"src_nat_miss", SAI_HOSTIF_TRAP_TYPE_SNAT_MISS},
    {"dest_nat_miss", SAI_HOSTIF_TRAP_TYPE_DNAT_MISS}
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

CoppOrch::CoppOrch(DBConnector* db, string tableName) :
    Orch(db, tableName)
{
    SWSS_LOG_ENTER();

    m_coppTable = unique_ptr<Table>(new Table(db, APP_COPP_TABLE_NAME));
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
    /* Marvell platform doesn't support trap priority. */
    char *platform = getenv("platform");
    if (!platform || (!strstr(platform, MLNX_PLATFORM_SUBSTRING) && (!strstr(platform, MRVL_PLATFORM_SUBSTRING))))
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
        if (((trap_id == SAI_HOSTIF_TRAP_TYPE_SNAT_MISS) or (trap_id == SAI_HOSTIF_TRAP_TYPE_DNAT_MISS)) and
            (gIsNatSupported == false))
        {
            SWSS_LOG_NOTICE("Ignoring the trap_id: %s, as NAT is not supported", trap_id_str.c_str());
            continue;
        }
        trap_id_list.push_back(trap_id);
    }
}

bool CoppOrch::createGenetlinkHostIfTable(vector<sai_hostif_trap_type_t> &trap_id_list)
{
    SWSS_LOG_ENTER();

    for (auto trap_id : trap_id_list)
    {
        auto host_tbl_entry = m_trapid_hostif_table_map.find(trap_id);

        if (host_tbl_entry == m_trapid_hostif_table_map.end())
        {
            sai_object_id_t trap_group_id = m_syncdTrapIds[trap_id].trap_group_obj;
            auto hostif_map = m_trap_group_hostif_map.find(trap_group_id);
            if (hostif_map != m_trap_group_hostif_map.end())
            {
                sai_object_id_t hostif_table_entry = SAI_NULL_OBJECT_ID;
                sai_attribute_t attr;
                vector<sai_attribute_t> sai_host_table_attr;

                attr.id = SAI_HOSTIF_TABLE_ENTRY_ATTR_TYPE;
                attr.value.s32 = SAI_HOSTIF_TABLE_ENTRY_TYPE_TRAP_ID;
                sai_host_table_attr.push_back(attr);

                attr.id = SAI_HOSTIF_TABLE_ENTRY_ATTR_TRAP_ID;
                attr.value.oid = m_syncdTrapIds[trap_id].trap_obj;
                sai_host_table_attr.push_back(attr);

                attr.id = SAI_HOSTIF_TABLE_ENTRY_ATTR_CHANNEL_TYPE;
                attr.value.s32 =  SAI_HOSTIF_TABLE_ENTRY_CHANNEL_TYPE_GENETLINK;
                sai_host_table_attr.push_back(attr);

                attr.id = SAI_HOSTIF_TABLE_ENTRY_ATTR_HOST_IF;
                attr.value.oid = hostif_map->second;
                sai_host_table_attr.push_back(attr);

                sai_status_t status = sai_hostif_api->create_hostif_table_entry(&hostif_table_entry,
                                                                                gSwitchId,
                                                                                (uint32_t)sai_host_table_attr.size(),
                                                                                sai_host_table_attr.data());
                if (status != SAI_STATUS_SUCCESS)
                {
                    SWSS_LOG_ERROR("Failed to create hostif table entry failed, rv %d", status);
                    return false;
                }
                m_trapid_hostif_table_map[trap_id] = hostif_table_entry;
            }
        }
    }
    return true;
}

bool CoppOrch::removeGenetlinkHostIfTable(vector<sai_hostif_trap_type_t> &trap_id_list)
{
    sai_status_t sai_status;

    for (auto trap_id : trap_id_list)
    {
        if ( m_trapid_hostif_table_map.find(trap_id) != m_trapid_hostif_table_map.end())
        {
            sai_status = sai_hostif_api->remove_hostif_table_entry(
                                                           m_trapid_hostif_table_map[trap_id]);
            if(sai_status != SAI_STATUS_SUCCESS)
            {
                SWSS_LOG_ERROR("Failed to delete hostif table entry %" PRId64 " \
                               rc=%d", m_trapid_hostif_table_map[trap_id], sai_status);
                return false;
            }
            m_trapid_hostif_table_map.erase(trap_id);
        }
    }
    return true;

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
        m_syncdTrapIds[trap_id].trap_group_obj = trap_group_id;
        m_syncdTrapIds[trap_id].trap_obj = hostif_trap_id;
    }
    return true;
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

bool CoppOrch::createGenetlinkHostIf(string trap_group_name, vector<sai_attribute_t> &genetlink_attribs)
{
    SWSS_LOG_ENTER();

    sai_object_id_t hostif_id;
    sai_status_t sai_status;

    sai_status = sai_hostif_api->create_hostif(&hostif_id, gSwitchId,
                                               (uint32_t)genetlink_attribs.size(),
                                               genetlink_attribs.data());
    if (sai_status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to create genetlink hostif for trap group %s, rc=%d",
                       trap_group_name.c_str(), sai_status);
        return false;
    }

    m_trap_group_hostif_map[m_trap_group_map[trap_group_name]] = hostif_id;
    return true;
}

bool CoppOrch::removeGenetlinkHostIf(string trap_group_name)
{
    SWSS_LOG_ENTER();

    sai_status_t sai_status;
    vector<sai_hostif_trap_type_t> group_trap_ids;

    getTrapIdsFromTrapGroup (m_trap_group_map[trap_group_name], group_trap_ids);
    if (!removeGenetlinkHostIfTable(group_trap_ids))
    {
        return false;
    }

    auto hostInfo = m_trap_group_hostif_map.find(m_trap_group_map[trap_group_name]);
    if(hostInfo != m_trap_group_hostif_map.end())
    {
        sai_status = sai_hostif_api->remove_hostif(hostInfo->second);
        if(sai_status != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_ERROR("Failed to delete host info %" PRId64 " on trap group %s. rc=%d",
                           hostInfo->second, trap_group_name.c_str(), sai_status);
            return false;
        }
        m_trap_group_hostif_map.erase(m_trap_group_map[trap_group_name]);
    }

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
    vector<sai_attribute_t> genetlink_attribs;

    vector<sai_hostif_trap_type_t> trap_ids;
    vector<sai_hostif_trap_type_t> add_trap_ids;
    vector<sai_hostif_trap_type_t> rem_trap_ids;
    std::vector<FieldValueTuple> fv_tuple = kfvFieldsValues(tuple);

    if (op == SET_COMMAND)
    {
        if(checkDupCoppGrpNameAndUpdate(trap_group_name, fv_tuple))
        {
            return task_process_status::task_success;
        }
        for (auto i = fv_tuple.begin(); i != fv_tuple.end(); i++)
        {
            if (fvField(*i) == copp_trap_id_list)
            {
                trap_id_list = tokenize(fvValue(*i), list_item_delimiter);
                getTrapIdList(trap_id_list, trap_ids);
                getTrapAddandRemoveList(trap_group_name, trap_ids, add_trap_ids, rem_trap_ids);
            }
        }

        if (!getAttribsFromTrapGroup(fv_tuple, trap_gr_attribs, trap_id_attribs,
                                    policer_attribs, genetlink_attribs))
        {
            return task_process_status::task_invalid_entry;
        } 

        /* Set host interface trap group */
        if (m_trap_group_map.find(trap_group_name) != m_trap_group_map.end())
        {
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
        }

        if (!policer_attribs.empty())
        {
            if (!trapGroupUpdatePolicer(trap_group_name, policer_attribs))
            {
                return task_process_status::task_failed;
            }
        }
        if (!trap_id_attribs.empty())
        {
            vector<sai_hostif_trap_type_t> group_trap_ids;
            TrapIdAttribs trap_attr;
            getTrapIdsFromTrapGroup(m_trap_group_map[trap_group_name],
                                    group_trap_ids);
            for (auto trap_id : group_trap_ids)
            {
                for (auto i: trap_id_attribs)
                {
                    sai_status = sai_hostif_api->set_hostif_trap_attribute(
                                                   m_syncdTrapIds[trap_id].trap_obj, &i);
                    if (sai_status != SAI_STATUS_SUCCESS)
                    {
                        SWSS_LOG_ERROR("Failed to set attribute %d on trap %" PRIx64 ""
                                " on group %s", i.id, m_syncdTrapIds[trap_id].trap_obj, 
                                trap_group_name.c_str());
                        return task_process_status::task_failed;
                    }
                }
            }
            for (auto i: trap_id_attribs)
            {
                trap_attr[i.id] = i.value;
            }
            m_trap_group_trap_id_attrs[trap_group_name] = trap_attr;
        }
        if (!genetlink_attribs.empty())
        {
            if (m_trap_group_hostif_map.find(m_trap_group_map[trap_group_name]) != 
                    m_trap_group_hostif_map.end())
            {
                SWSS_LOG_ERROR("Genetlink hostif exists for the trap group %s", 
                               trap_group_name.c_str());
                return task_process_status::task_failed;
            }
            vector<sai_hostif_trap_type_t> genetlink_trap_ids;
            getTrapIdsFromTrapGroup(m_trap_group_map[trap_group_name], genetlink_trap_ids);
            if (!createGenetlinkHostIf(trap_group_name, genetlink_attribs))
            {
                return task_process_status::task_failed;
            }
            if (!createGenetlinkHostIfTable(genetlink_trap_ids))
            {
                return task_process_status::task_failed;
            }
        }
        if (!trapGroupProcessTrapIdChange(trap_group_name, add_trap_ids, rem_trap_ids))
        {
            return task_process_status::task_failed;
        }
    }
    else if (op == DEL_COMMAND)
    {
        /* Do not remove default trap group */
        if (trap_group_name == default_trap_group)
        {
            SWSS_LOG_WARN("Cannot remove default trap group");
            return task_process_status::task_ignore;
        }

        if (!processTrapGroupDel(trap_group_name))
        {
            return task_process_status::task_failed;
        }
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
    string table_name = consumer.getTableName();

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
                it = consumer.m_toSync.erase(it);
                SWSS_LOG_ERROR("Processing copp task item failed, exiting. ");
                return;
            case task_process_status::task_need_retry:
                SWSS_LOG_ERROR("Processing copp task item failed, will retry.");
                it++;
                break;
            default:
                it = consumer.m_toSync.erase(it);
                SWSS_LOG_ERROR("Invalid task status:%d", task_status);
                return;
        }
    }
}

/* Check if the Copp Group name is different while all trap ids remain the same
 * This is going to be warm boot scenario from an old image which has different naming scheme 
 */
bool CoppOrch::checkDupCoppGrpNameAndUpdate(string new_copp_grp_name,
                                            vector<FieldValueTuple> &fv_tuple)
{
    vector<string> trap_id_list;
    sai_object_id_t trap_group_obj = SAI_NULL_OBJECT_ID;
    vector<FieldValueTuple> mod_fv_tuple;
    vector<sai_hostif_trap_type_t> trap_ids;
    bool ret = false;

    if (m_trap_group_map.find(new_copp_grp_name) != m_trap_group_map.end())
    {
        return false;
    }

    for (auto i = fv_tuple.begin(); i != fv_tuple.end(); i++)
    {
        if (fvField(*i) == copp_trap_id_list)
        {
            trap_id_list = tokenize(fvValue(*i), list_item_delimiter);
            getTrapIdList(trap_id_list, trap_ids);
        }
    }

    /* In the case where NAT is not supported, there will be no trap IDs associated with
     * the nat trap group and hence group ID cannot be figured out by iterating through traps.
     * Using the hardcoded name for this specific case
     */
    if (trap_ids.size() == 0)
    {
        if (!gIsNatSupported)
        {
            auto src_nat = std::find(trap_id_list.begin(), trap_id_list.end(), "src_nat_miss");
            auto dst_nat = std::find(trap_id_list.begin(), trap_id_list.end(), "dest_nat_miss");
            if ((src_nat != trap_id_list.end()) && (dst_nat != trap_id_list.end()))
            {
                if(m_trap_group_map.find(old_nat_group_name) != m_trap_group_map.end())
                {
                    trap_group_obj = m_trap_group_map[old_nat_group_name];
                }
                else
                {
                    return false;
                }
            }
            else
            {
                return false;
            }
        }
        else
        {
            return false;
        }
    }
	else
	{
        for(auto it = trap_ids.begin(); it != trap_ids.end(); it++)
        {
            if (m_syncdTrapIds.find(*it) == m_syncdTrapIds.end())
            {
                return false;
            }
            sai_object_id_t  trap_id_group = m_syncdTrapIds[*it].trap_group_obj;
            if (trap_id_group == SAI_NULL_OBJECT_ID)
            {
                return false;
            }
            if (trap_group_obj == SAI_NULL_OBJECT_ID)
            {
                trap_group_obj = trap_id_group;
            }
            else if (trap_group_obj != trap_id_group)
            {
                return false;
            }
        }
    }
    
    for (auto it : m_trap_group_map)    
    {
        if (it.second ==  trap_group_obj)
        {
            vector<FieldValueTuple> old_tuples;
            vector<FieldValueTuple> new_tuples;
            m_coppTable->get(it.first, old_tuples);
            m_coppTable->get(new_copp_grp_name, new_tuples);
            vector<FieldValueTuple> tmp_tuples = new_tuples;
            for (auto o: old_tuples)
            {
                if (fvField(o) == copp_trap_id_list)
                {
                    continue;
                }
                bool not_found = true;
                for (auto n : new_tuples)
                {
                    if (fvField(o) == fvField(n))
                    {
                        if (fvValue(o) != fvValue(n))
                        {
                            mod_fv_tuple.push_back(n);
                        }
                        auto vec_idx = std::find(tmp_tuples.begin(), tmp_tuples.end(), n);
                        tmp_tuples.erase(vec_idx);
                        not_found = false;
                    }
                }
                /* Old table has a field which is not present in new table
                 * Delete the entry and return false so it is recreated
                 */
                if (not_found)
                {
                    processTrapGroupDel(it.first);
                    m_coppTable->del(it.first);
                    return false;
                }
            }
            if (!tmp_tuples.empty())
            {
                mod_fv_tuple.insert(mod_fv_tuple.end(), tmp_tuples.begin(), tmp_tuples.end());
            }
            if (!mod_fv_tuple.empty())
            {
                fv_tuple = mod_fv_tuple;
            }
            else
            {
                ret = true;
            }
            m_trap_group_map[new_copp_grp_name] = it.second;
            m_trap_group_map.erase(m_trap_group_map.find(it.first));
            m_coppTable->del(it.first);
            break;
        }
    }
    return ret;
}

void CoppOrch::getTrapAddandRemoveList(string trap_group_name,
                                       vector<sai_hostif_trap_type_t> &trap_ids,
                                       vector<sai_hostif_trap_type_t> &add_trap_ids,
                                       vector<sai_hostif_trap_type_t> &rem_trap_ids)
{
    
    vector<sai_hostif_trap_type_t> tmp_trap_ids = trap_ids;
    if(m_trap_group_map.find(trap_group_name) == m_trap_group_map.end())
    {
        add_trap_ids = trap_ids;
        rem_trap_ids.clear();
        return;
    }


	for (auto it : m_syncdTrapIds)
	{
		if (it.second.trap_group_obj == m_trap_group_map[trap_group_name])
		{
            /* If new trap list contains already mapped ID remove it */
            auto i = std::find(std::begin(tmp_trap_ids), std::end(tmp_trap_ids), it.first);

            if (i != std::end(tmp_trap_ids))
            {
			    tmp_trap_ids.erase(i);
            }
            /* The mapped Trap ID is not found on newly set list and to be removed*/
            else
            {
                if ((trap_group_name != default_trap_group) ||
                    ((trap_group_name == default_trap_group) &&
                    (it.first != SAI_HOSTIF_TRAP_TYPE_TTL_ERROR)))
                {
                    rem_trap_ids.push_back(it.first);
                }
            }
		}
    }

    add_trap_ids = tmp_trap_ids;
}

void CoppOrch::getTrapIdsFromTrapGroup (sai_object_id_t trap_group_obj,
                                        vector<sai_hostif_trap_type_t> &trap_ids)
{
    for(auto it: m_syncdTrapIds)
    {
        if (it.second.trap_group_obj == trap_group_obj)
        {
            trap_ids.push_back(it.first);
        }
    }
}

bool CoppOrch::trapGroupProcessTrapIdChange (string trap_group_name,
                                          vector<sai_hostif_trap_type_t> &add_trap_ids,
                                          vector<sai_hostif_trap_type_t> &rem_trap_ids)
{
    if (!add_trap_ids.empty())
    {
        sai_attribute_t attr;
        vector<sai_attribute_t> add_trap_attr;
        attr.id = SAI_HOSTIF_TRAP_ATTR_TRAP_GROUP;
        attr.value.oid = m_trap_group_map[trap_group_name];

        add_trap_attr.push_back(attr);

        for(auto i: add_trap_ids)
        {
            if (m_syncdTrapIds.find(i)!= m_syncdTrapIds.end())
            {
                sai_status_t sai_status = sai_hostif_api->remove_hostif_trap(
                                                             m_syncdTrapIds[i].trap_obj);
                if (sai_status != SAI_STATUS_SUCCESS)
                {
                    SWSS_LOG_ERROR("Failed to remove trap object %" PRId64 "",
                            m_syncdTrapIds[i].trap_obj);
                    return false;
                }
            }
        }

        for (auto it: m_trap_group_trap_id_attrs[trap_group_name])
        {
            attr.id = it.first;
            attr.value = it.second;
            add_trap_attr.push_back(attr);
        }
        if (!applyAttributesToTrapIds(m_trap_group_map[trap_group_name], add_trap_ids,
                                      add_trap_attr))
        {
            SWSS_LOG_ERROR("Failed to set traps to trap group %s", trap_group_name.c_str());
            return false;
        }
        if (m_trap_group_hostif_map.find(m_trap_group_map[trap_group_name]) != 
                                         m_trap_group_hostif_map.end())
        {
            if (!createGenetlinkHostIfTable(add_trap_ids))
            {
                return task_process_status::task_failed;
            }
        }
    }
    if (!rem_trap_ids.empty())
    {
        for (auto i: rem_trap_ids)
        {
            if (m_syncdTrapIds.find(i)!= m_syncdTrapIds.end())
            {
                /*
                 * A trap ID will be present in rem_trap_id in two scenarios
                 * 1) When trap group for a trap ID is changed
                 * 2) When trap ID is completely removed
                 * In case 1 the first call would be to add the trap ids to a different 
                 * group. This would result in changing the mapping of trap id to trap group
                 * In case 2 the mapping will remain the same. In this case the trap
                 * object needs to be deleted
                 */
                if (m_syncdTrapIds[i].trap_group_obj ==  m_trap_group_map[trap_group_name])
                {
                    sai_status_t sai_status = sai_hostif_api->remove_hostif_trap(
                                                                    m_syncdTrapIds[i].trap_obj);
                    if (sai_status != SAI_STATUS_SUCCESS)
                    {
                        SWSS_LOG_ERROR("Failed to remove trap object %" PRId64 "", 
                                       m_syncdTrapIds[i].trap_obj);
                        return false;
                    }
                    m_syncdTrapIds.erase(i);
                }
            }
        }
        if (!removeGenetlinkHostIfTable(rem_trap_ids))
        {
            return false;
        }
    }
    return true;
}

bool CoppOrch::processTrapGroupDel (string trap_group_name)
{
    auto it_del = m_trap_group_map.find(trap_group_name);

    if (it_del == m_trap_group_map.end())
    {
        return true;
    }
    /* Remove policer if any */
    if (!removePolicer(trap_group_name))
    {
        SWSS_LOG_ERROR("Failed to remove policer from trap group %s", trap_group_name.c_str());
        return false;
    }

    if (!removeGenetlinkHostIf(trap_group_name))
    {
        SWSS_LOG_ERROR("Failed to remove hostif from trap group %s", trap_group_name.c_str());
        return false;
    }

    /* Reset the trap IDs to default trap group with default attributes */
    vector<sai_hostif_trap_type_t> trap_ids_to_reset;
    for (auto it : m_syncdTrapIds)
    {
        if (it.second.trap_group_obj == m_trap_group_map[trap_group_name])
        {
            trap_ids_to_reset.push_back(it.first);
            sai_status_t sai_status = sai_hostif_api->remove_hostif_trap(it.second.trap_obj);
            if (sai_status != SAI_STATUS_SUCCESS)
            {
                SWSS_LOG_ERROR("Failed to remove trap object %" PRId64 "", it.second.trap_obj);
                return false;
            }
        }
    }

    for (auto it: trap_ids_to_reset)
    {
        m_syncdTrapIds.erase(it);
    }

    sai_status_t sai_status = sai_hostif_api->remove_hostif_trap_group(
                                                  m_trap_group_map[trap_group_name]);
    if (sai_status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to remove trap group %s", trap_group_name.c_str());
        return false;
    }

    m_trap_group_map.erase(it_del);
    return true;
}

bool CoppOrch::getAttribsFromTrapGroup (vector<FieldValueTuple> &fv_tuple,
                                        vector<sai_attribute_t> &trap_gr_attribs,
                                        vector<sai_attribute_t> &trap_id_attribs,
                                        vector<sai_attribute_t> &policer_attribs,
                                        vector<sai_attribute_t> &genetlink_attribs)
{
    sai_attribute_t attr;

    for (auto i = fv_tuple.begin(); i != fv_tuple.end(); i++)
    {
        if (fvField(*i) == copp_trap_id_list)
        {
            continue;
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
            /* Marvell platform doesn't support trap priority. */
            char *platform = getenv("platform");
            if (!platform || (!strstr(platform, MLNX_PLATFORM_SUBSTRING) && (!strstr(platform, MRVL_PLATFORM_SUBSTRING))))
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
        else if (fvField(*i) == copp_genetlink_name)
        {
            attr.id = SAI_HOSTIF_ATTR_TYPE;
            attr.value.s32 = SAI_HOSTIF_TYPE_GENETLINK;
            genetlink_attribs.push_back(attr);

            attr.id = SAI_HOSTIF_ATTR_NAME;
            strncpy(attr.value.chardata, fvValue(*i).c_str(),
                    sizeof(attr.value.chardata));
            genetlink_attribs.push_back(attr);

        }
        else if (fvField(*i) == copp_genetlink_mcgrp_name)
        {
            attr.id = SAI_HOSTIF_ATTR_GENETLINK_MCGRP_NAME;
            strncpy(attr.value.chardata, fvValue(*i).c_str(),
                    sizeof(attr.value.chardata));
            genetlink_attribs.push_back(attr);
        }
        else
        {
            SWSS_LOG_ERROR("Unknown copp field specified:%s\n", fvField(*i).c_str());
            return false;
        }
    }
    return true;
}

bool CoppOrch::trapGroupUpdatePolicer (string trap_group_name,
                                       vector<sai_attribute_t> &policer_attribs)
{
    sai_object_id_t policer_id = getPolicer(trap_group_name);

    if (m_trap_group_map.find(trap_group_name) == m_trap_group_map.end())
    {
        return false;
    }
    if (SAI_NULL_OBJECT_ID == policer_id)
    {
        SWSS_LOG_WARN("Creating policer for existing Trap group: %" PRIx64 " (name:%s).",
                      m_trap_group_map[trap_group_name], trap_group_name.c_str());
        if (!createPolicer(trap_group_name, policer_attribs))
        {
            return false;
        }
        SWSS_LOG_DEBUG("Created policer:%" PRIx64 " for existing trap group", policer_id);
    }
    else
    {
        for (sai_uint32_t ind = 0; ind < policer_attribs.size(); ind++)
        {
            auto policer_attr = policer_attribs[ind];
            sai_status_t sai_status = sai_policer_api->set_policer_attribute(policer_id,
                                                                             &policer_attr);
            if (sai_status != SAI_STATUS_SUCCESS)
            {
                SWSS_LOG_ERROR("Failed to apply attribute[%d].id=%d to policer for trap group:"
                               "%s, error:%d\n",ind, policer_attr.id, trap_group_name.c_str(),
                               sai_status);
                return false;
            }
        }
    }
    return true;
}

