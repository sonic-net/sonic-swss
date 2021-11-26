#include "sai.h"
#include "copporch.h"
#include "portsorch.h"
#include "flexcounterorch.h"
#include "tokenize.h"
#include "logger.h"
#include "sai_serialize.h"
#include "schema.h"
#include "directory.h"
#include "flow_counter_handler.h"
#include "timer.h"

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
extern Directory<Orch*>     gDirectory;
extern bool                 gIsNatSupported;

#define FLEX_COUNTER_UPD_INTERVAL 1

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
    {"isis", SAI_HOSTIF_TRAP_TYPE_ISIS},
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
    {"dest_nat_miss", SAI_HOSTIF_TRAP_TYPE_DNAT_MISS},
    {"ldp", SAI_HOSTIF_TRAP_TYPE_LDP},
    {"bfd_micro", SAI_HOSTIF_TRAP_TYPE_BFD_MICRO},
    {"bfdv6_micro", SAI_HOSTIF_TRAP_TYPE_BFDV6_MICRO}
};


std::string get_trap_name_by_type(sai_hostif_trap_type_t trap_type)
{
    static map<sai_hostif_trap_type_t, string> trap_name_to_id_map;
    if (trap_name_to_id_map.empty())
    {
        for (const auto &kv : trap_id_map)
        {
            trap_name_to_id_map.emplace(kv.second, kv.first);
        }
    }

    return trap_name_to_id_map.at(trap_type);
}

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
const uint HOSTIF_TRAP_COUNTER_POLLING_INTERVAL_MS = 10000;

CoppOrch::CoppOrch(DBConnector* db, string tableName) :
    Orch(db, tableName),
    m_counter_db(std::shared_ptr<DBConnector>(new DBConnector("COUNTERS_DB", 0))),
    m_flex_db(std::shared_ptr<DBConnector>(new DBConnector("FLEX_COUNTER_DB", 0))),
    m_asic_db(std::shared_ptr<DBConnector>(new DBConnector("ASIC_DB", 0))),
    m_counter_table(std::unique_ptr<Table>(new Table(m_counter_db.get(), COUNTERS_TRAP_NAME_MAP))),
    m_vidToRidTable(std::unique_ptr<Table>(new Table(m_asic_db.get(), "VIDTORID"))),
    m_flex_counter_group_table(std::unique_ptr<ProducerTable>(new ProducerTable(m_flex_db.get(), FLEX_COUNTER_GROUP_TABLE))),
    m_trap_counter_manager(HOSTIF_TRAP_COUNTER_FLEX_COUNTER_GROUP, StatsMode::READ, HOSTIF_TRAP_COUNTER_POLLING_INTERVAL_MS, false)
{
    SWSS_LOG_ENTER();
    auto intervT = timespec { .tv_sec = FLEX_COUNTER_UPD_INTERVAL , .tv_nsec = 0 };
    m_FlexCounterUpdTimer = new SelectableTimer(intervT);
    auto executorT = new ExecutableTimer(m_FlexCounterUpdTimer, this, "FLEX_COUNTER_UPD_TIMER");
    Orch::addExecutor(executorT);

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
        if (handleSaiCreateStatus(SAI_API_HOSTIF, status) != task_success)
        {
            throw "CoppOrch initialization failure";
        }
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
        task_process_status handle_status = handleSaiGetStatus(SAI_API_SWITCH, status);
        if (handle_status != task_process_status::task_success)
        {
            throw "CoppOrch initialization failure";
        }
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
                    task_process_status handle_status = handleSaiCreateStatus(SAI_API_HOSTIF, status);
                    if (handle_status != task_success)
                    {
                        return parseHandleSaiStatusFailure(handle_status);
                    }
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
                task_process_status handle_status = handleSaiRemoveStatus(SAI_API_HOSTIF, sai_status);
                if (handle_status != task_success)
                {
                    return parseHandleSaiStatusFailure(handle_status);
                }
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
            task_process_status handle_status = handleSaiCreateStatus(SAI_API_HOSTIF, status);
            if (handle_status != task_success)
            {
                return parseHandleSaiStatusFailure(handle_status);
            }
        }
        m_syncdTrapIds[trap_id].trap_group_obj = trap_group_id;
        m_syncdTrapIds[trap_id].trap_obj = hostif_trap_id;
        m_syncdTrapIds[trap_id].trap_type = trap_id;
        bindTrapCounter(hostif_trap_id, trap_id);
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
        task_process_status handle_status = handleSaiSetStatus(SAI_API_HOSTIF, sai_status);
        if (handle_status != task_success)
        {
            return parseHandleSaiStatusFailure(handle_status);
        }
    }

    sai_status = sai_policer_api->remove_policer(policer_id);
    if (sai_status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to remove policer for trap group %s, rc=%d", trap_group_name.c_str(), sai_status);
        task_process_status handle_status = handleSaiRemoveStatus(SAI_API_POLICER, sai_status);
        if (handle_status != task_success)
        {
            return parseHandleSaiStatusFailure(handle_status);
        }
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
        task_process_status handle_status = handleSaiCreateStatus(SAI_API_POLICER, sai_status);
        if (handle_status != task_success)
        {
            return parseHandleSaiStatusFailure(handle_status);
        }
    }

    SWSS_LOG_NOTICE("Create policer for trap group %s", trap_group_name.c_str());

    sai_attribute_t attr;
    attr.id = SAI_HOSTIF_TRAP_GROUP_ATTR_POLICER;
    attr.value.oid = policer_id;

    sai_status = sai_hostif_api->set_hostif_trap_group_attribute(m_trap_group_map[trap_group_name], &attr);
    if (sai_status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to bind policer to trap group %s, rc=%d", trap_group_name.c_str(), sai_status);
        task_process_status handle_status = handleSaiSetStatus(SAI_API_HOSTIF, sai_status);
        if (handle_status != task_success)
        {
            return parseHandleSaiStatusFailure(handle_status);
        }
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
        task_process_status handle_status = handleSaiCreateStatus(SAI_API_HOSTIF, sai_status);
        if (handle_status != task_success)
        {
            return parseHandleSaiStatusFailure(handle_status);
        }
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
            task_process_status handle_status = handleSaiRemoveStatus(SAI_API_HOSTIF, sai_status);
            if (handle_status != task_success)
            {
                return parseHandleSaiStatusFailure(handle_status);
            }
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
                    task_process_status handle_status = handleSaiSetStatus(SAI_API_HOSTIF, sai_status);
                    if (handle_status != task_process_status::task_success)
                    {
                        return handle_status;
                    }
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
                task_process_status handle_status = handleSaiCreateStatus(SAI_API_HOSTIF, sai_status);
                if (handle_status != task_process_status::task_success)
                {
                    return handle_status;
                }
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
                        task_process_status handle_status = handleSaiSetStatus(SAI_API_HOSTIF, sai_status);
                        if (handle_status != task_process_status::task_success)
                        {
                            return handle_status;
                        }
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

void CoppOrch::doTask(SelectableTimer &timer)
{
    SWSS_LOG_ENTER();

    string value;
    for (auto it = m_pendingAddToFlexCntr.begin(); it != m_pendingAddToFlexCntr.end(); )
    {
        const auto id = sai_serialize_object_id(it->first);
        if (m_vidToRidTable->hget("", id, value))
        {
            SWSS_LOG_INFO("Registering %s, id %s", it->second.c_str(), id.c_str());

            std::unordered_set<std::string> counter_stats;
            FlowCounterHandler::getGenericCounterStatIdList(counter_stats);
            m_trap_counter_manager.setCounterIdList(it->first, CounterType::HOSTIF_TRAP, counter_stats);
            it = m_pendingAddToFlexCntr.erase(it);
        }
        else
        {
            ++it;
        }
    }

    if (m_pendingAddToFlexCntr.empty())
    {
        m_FlexCounterUpdTimer->stop();
    }
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
                if (!removeTrap(m_syncdTrapIds[i].trap_obj))
                {
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
                return false;
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
                    if (!removeTrap(m_syncdTrapIds[i].trap_obj))
                    {
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
            if (!removeTrap(it.second.trap_obj))
            {
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
        task_process_status handle_status = handleSaiRemoveStatus(SAI_API_HOSTIF, sai_status);
        if (handle_status != task_success)
        {
            return parseHandleSaiStatusFailure(handle_status);
        }
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
                task_process_status handle_status = handleSaiSetStatus(SAI_API_POLICER, sai_status);
                if (handle_status != task_success)
                {
                    return parseHandleSaiStatusFailure(handle_status);
                }
            }
        }
    }
    return true;
}

void CoppOrch::initTrapRatePlugin()
{
    if (m_trap_rate_plugin_loaded)
    {
        return;
    }

    std::string trapRatePluginName = "trap_rates.lua";
    try
    {
        std::string trapLuaScript = swss::loadLuaScript(trapRatePluginName);
        std::string trapSha = swss::loadRedisScript(m_counter_db.get(), trapLuaScript);

        vector<FieldValueTuple> fieldValues;
        fieldValues.emplace_back(FLOW_COUNTER_PLUGIN_FIELD, trapSha);
        fieldValues.emplace_back(STATS_MODE_FIELD, STATS_MODE_READ);
        m_flex_counter_group_table->set(HOSTIF_TRAP_COUNTER_FLEX_COUNTER_GROUP, fieldValues);
    }
    catch (const runtime_error &e)
    {
        SWSS_LOG_ERROR("Trap flex counter groups were not set successfully: %s", e.what());
    }
    m_trap_rate_plugin_loaded = true;
}

bool CoppOrch::removeTrap(sai_object_id_t hostif_trap_id)
{
    unbindTrapCounter(hostif_trap_id);

    sai_status_t sai_status = sai_hostif_api->remove_hostif_trap(hostif_trap_id);
    if (sai_status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to remove trap object %" PRId64 "",
                hostif_trap_id);
        task_process_status handle_status = handleSaiRemoveStatus(SAI_API_HOSTIF, sai_status);
        if (handle_status != task_success)
        {
            return parseHandleSaiStatusFailure(handle_status);
        }
    }

    return true;
}

bool CoppOrch::bindTrapCounter(sai_object_id_t hostif_trap_id, sai_hostif_trap_type_t trap_type)
{
    auto flex_counters_orch = gDirectory.get<FlexCounterOrch*>();

    if (!flex_counters_orch || !flex_counters_orch->getHostIfTrapCounterState())
    {
        return false;
    }

    if (m_trap_obj_name_map.count(hostif_trap_id) > 0)
    {
        return true;
    }

    initTrapRatePlugin();

    // Create generic counter
    sai_object_id_t counter_id;
    if (!FlowCounterHandler::createGenericCounter(counter_id))
    {
        return false;
    }

    // Bind generic counter to trap
    sai_attribute_t trap_attr;
    trap_attr.id = SAI_HOSTIF_TRAP_ATTR_COUNTER_ID;
    trap_attr.value.oid = counter_id;
    sai_status_t sai_status = sai_hostif_api->set_hostif_trap_attribute(hostif_trap_id, &trap_attr);
    if (sai_status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_WARN("Failed to bind trap %" PRId64 " to counter %" PRId64 "", hostif_trap_id, counter_id);
        return false;
    }

    // Update COUNTERS_TRAP_NAME_MAP
    auto trap_name = get_trap_name_by_type(trap_type);
    vector<FieldValueTuple> nameMapFvs;
    nameMapFvs.emplace_back(trap_name, sai_serialize_object_id(counter_id));
    m_counter_table->set("", nameMapFvs);

    auto was_empty = m_pendingAddToFlexCntr.empty();
    m_pendingAddToFlexCntr[counter_id] = trap_name;

    if (was_empty)
    {
        m_FlexCounterUpdTimer->start();
    }

    m_trap_obj_name_map.emplace(hostif_trap_id, trap_name);
    return true;
}

void CoppOrch::unbindTrapCounter(sai_object_id_t hostif_trap_id)
{
    auto iter = m_trap_obj_name_map.find(hostif_trap_id);
    if (iter == m_trap_obj_name_map.end())
    {
        return;
    }

    std::string counter_oid_str;
    m_counter_table->hget("", iter->second, counter_oid_str);

    // Clear FLEX_COUNTER table
    sai_object_id_t counter_id;
    sai_deserialize_object_id(counter_oid_str, counter_id);
    auto update_iter = m_pendingAddToFlexCntr.find(counter_id);
    if (update_iter == m_pendingAddToFlexCntr.end())
    {
        m_trap_counter_manager.clearCounterIdList(counter_id);
    }
    else
    {
        m_pendingAddToFlexCntr.erase(update_iter);
    }

    // Remove trap from COUNTERS_TRAP_NAME_MAP
    m_counter_table->hdel("", iter->second);

    // Unbind generic counter to trap
    sai_attribute_t trap_attr;
    trap_attr.id = SAI_HOSTIF_TRAP_ATTR_COUNTER_ID;
    trap_attr.value.oid = SAI_NULL_OBJECT_ID;
    sai_status_t sai_status = sai_hostif_api->set_hostif_trap_attribute(hostif_trap_id, &trap_attr);
    if (sai_status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to unbind trap %" PRId64 " to counter %" PRId64 "", hostif_trap_id, counter_id);
    }

    // Remove generic counter
    FlowCounterHandler::removeGenericCounter(counter_id);

    m_trap_obj_name_map.erase(iter);
}

void CoppOrch::generateHostIfTrapCounterIdList()
{
    for (const auto &kv : m_syncdTrapIds)
    {
        bindTrapCounter(kv.second.trap_obj, kv.second.trap_type);
    }
}

void CoppOrch::clearHostIfTrapCounterIdList()
{
    for (const auto &kv : m_syncdTrapIds)
    {
        unbindTrapCounter(kv.second.trap_obj);
    }
}
