#include "dashhaorch.h"

#include "orch.h"
#include "sai.h"
#include "saiextensions.h"
#include "dashorch.h"
#include "crmorch.h"
#include "saihelper.h"

#include "taskworker.h" 
#include "pbutils.h"

using namespace std;
using namespace swss;

extern sai_dash_ha_api_t* sai_dash_ha_api;
extern sai_dash_eni_api_t* sai_dash_eni_api;
extern sai_object_id_t gSwitchId;

static std::map<dash::ha_scope::Role, std::string> ha_role_to_str_map = {
    {dash::ha_scope::HA_SCOPE_ROLE_DEAD, "HA_SCOPE_ROLE_DEAD"},
    {dash::ha_scope::HA_SCOPE_ROLE_ACTIVE, "HA_SCOPE_ROLE_ACTIVE"},
    {dash::ha_scope::HA_SCOPE_ROLE_STANDBY, "HA_SCOPE_ROLE_STANDBY"},
    {dash::ha_scope::HA_SCOPE_ROLE_STANDALONE, "HA_SCOPE_ROLE_STANDALONE"},
    {dash::ha_scope::HA_SCOPE_ROLE_SWITCHING_TO_ACTIVE, "HA_SCOPE_ROLE_SWITCHING_TO_ACTIVE"}
};

static std::map<dash::ha_set::Scope, std::string> ha_set_scope_to_str_map = {
    {dash::ha_set::SCOPE_DPU, "SCOPE_DPU"},
    {dash::ha_set::SCOPE_ENI, "SCOPE_ENI"},
};

DashHaOrch::DashHaOrch(DBConnector *dpu_appl_db, DBConnector *dpu_state_db, vector<string> &tables, DashOrch *dash_orch, ZmqServer *zmqServer, ZmqClient *zmqClient) :
    ZmqOrch(dpu_appl_db, tables, zmqServer),
    m_dpu_state_db(dpu_state_db),
    m_dash_orch(dash_orch),
    m_zmqClient(zmqClient),
    dash_ha_set_state_table(dpu_state_db, STATE_DASH_HA_SET_STATE_TABLE_NAME, *zmqClient, true),
    dash_ha_scope_state_table(dpu_state_db, STATE_DASH_HA_SCOPE_STATE_TABLE_NAME, *zmqClient, true)
{
    SWSS_LOG_ENTER();
}

bool DashHaOrch::addHaSetEntry(const std::string &key, const dash::ha_set::HaSet &entry)
{
    SWSS_LOG_ENTER();

    auto it = m_ha_set_entries.find(key);

    if (it != m_ha_set_entries.end())
    {
        SWSS_LOG_WARN("HA Set entry already exists for %s", key.c_str());
        return true;
    }

    uint32_t attr_count = 8;
    sai_attribute_t ha_set_attr_list[8]={};
    sai_status_t status;
    sai_object_id_t sai_ha_set_oid = 0UL;

    ha_set_attr_list[0].id = SAI_HA_SET_ATTR_LOCAL_IP;
    ha_set_attr_list[0].value.ipaddr = covertPbIpaddrToSaiIpaddr(entry.local_ip());

    ha_set_attr_list[1].id = SAI_HA_SET_ATTR_PEER_IP;
    ha_set_attr_list[1].value.ipaddr = covertPbIpaddrToSaiIpaddr(entry.peer_ip());

    ha_set_attr_list[2].id = SAI_HA_SET_ATTR_CP_DATA_CHANNEL_PORT;
    ha_set_attr_list[2].value.u16 = static_cast<sai_uint16_t>(entry.cp_data_channel_port());
    
    ha_set_attr_list[3].id = SAI_HA_SET_ATTR_DP_CHANNEL_DST_PORT;
    ha_set_attr_list[3].value.u16 = static_cast<sai_uint16_t>(entry.dp_channel_dst_port());

    ha_set_attr_list[4].id = SAI_HA_SET_ATTR_DP_CHANNEL_MIN_SRC_PORT;
    ha_set_attr_list[4].value.u16 = static_cast<sai_uint16_t>(entry.dp_channel_src_port_min());

    ha_set_attr_list[5].id = SAI_HA_SET_ATTR_DP_CHANNEL_MAX_SRC_PORT;
    ha_set_attr_list[5].value.u16 = static_cast<sai_uint16_t>(entry.dp_channel_src_port_max());

    ha_set_attr_list[6].id = SAI_HA_SET_ATTR_DP_CHANNEL_PROBE_INTERVAL_MS;
    ha_set_attr_list[6].value.u32 = entry.dp_channel_probe_interval_ms();

    ha_set_attr_list[7].id = SAI_HA_SET_ATTR_DP_CHANNEL_PROBE_FAIL_THRESHOLD;
    ha_set_attr_list[7].value.u32 = entry.dp_channel_probe_fail_threshold();

    status = sai_dash_ha_api->create_ha_set(&sai_ha_set_oid,
                                                gSwitchId,
                                                attr_count,
                                                ha_set_attr_list);

    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to create HA Set object in SAI for %s", key.c_str());
        task_process_status handle_status = handleSaiCreateStatus((sai_api_t) SAI_API_DASH_HA, status);
        if (handle_status != task_success)
        {
            return parseHandleSaiStatusFailure(handle_status);
        }
    }
    m_ha_set_entries[key] = HaSetEntry {sai_ha_set_oid, entry};
    SWSS_LOG_NOTICE("Created HA Set object for %s", key.c_str());

    return true;
}

bool DashHaOrch::removeHaSetEntry(const std::string &key)
{
    SWSS_LOG_ENTER();

    auto it = m_ha_set_entries.find(key);

    if (it == m_ha_set_entries.end())
    {
        SWSS_LOG_WARN("HA Set entry does not exist for %s", key.c_str());
        return true;
    }

    sai_status_t status = sai_dash_ha_api->remove_ha_set(it->second.ha_set_id);

    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to remove HA Set object in SAI for %s", key.c_str());
        task_process_status handle_status = handleSaiRemoveStatus((sai_api_t) SAI_API_DASH_HA, status);
        if (handle_status != task_success)
        {
            return parseHandleSaiStatusFailure(handle_status);
        }
    }
    m_ha_set_entries.erase(it);
    SWSS_LOG_NOTICE("Removed HA Set object for %s", key.c_str());

    return true;
}

void DashHaOrch::doTaskHaSetTable(ConsumerBase &consumer)
{
    SWSS_LOG_ENTER();

    auto it = consumer.m_toSync.begin();

    while (it != consumer.m_toSync.end())
    {
        KeyOpFieldsValuesTuple tuple = it->second;
        const auto& key = kfvKey(tuple);
        const auto& op = kfvOp(tuple);

        if (op == SET_COMMAND)
        {
            dash::ha_set::HaSet entry;

            if (!parsePbMessage(kfvFieldsValues(tuple), entry))
            {
                SWSS_LOG_WARN("Requires protobuf at HaSet :%s", key.c_str());
                it = consumer.m_toSync.erase(it);
                continue;
            }

            if (addHaSetEntry(key, entry))
            {
                it = consumer.m_toSync.erase(it);
            }
            else
            {
                it++;
            }

        }
        else if (op == DEL_COMMAND)
        {
            if(removeHaSetEntry(key))
            {
                it = consumer.m_toSync.erase(it);
            }
            else
            {
                it++;
            }
        }
        else
        {
            SWSS_LOG_ERROR("Unknown operation %s", op.c_str());
            it = consumer.m_toSync.erase(it);
        }
    }
}

bool DashHaOrch::addHaScopeEntry(const std::string &key, const dash::ha_scope::HaScope &entry)
{
    SWSS_LOG_ENTER();

    auto ha_scope_it = m_ha_scope_entries.find(key);
    if (ha_scope_it != m_ha_scope_entries.end())
    {
        if (ha_scope_it->second.metadata.ha_role() != entry.ha_role())
        {
            return setHaScopeHaRole(key, entry);
        }

        if (entry.flow_reconcile_requested() == true)
        {
            return setHaScopeFlowReconcileRequest(key);
        }

        if (entry.activate_role_requested() == true)
        {
            return setHaScopeActivateRoleRequest(key);
        }

    }

    if (ha_scope_it != m_ha_scope_entries.end())
    {
        SWSS_LOG_WARN("HA Scope entry already exists for %s", key.c_str());
        return true;
    }

    auto ha_set_it = m_ha_set_entries.find(key);
    if (ha_set_it == m_ha_set_entries.end())
    {
        SWSS_LOG_ERROR("HA Set entry does not exist for %s", key.c_str());
        return false;
    }
    sai_object_id_t ha_set_oid = ha_set_it->second.ha_set_id;

    uint32_t attr_count = 2;
    sai_attribute_t ha_scope_attrs[attr_count]={};
    sai_status_t status;
    sai_object_id_t sai_ha_scope_oid = 0UL;

    ha_scope_attrs[0].id = SAI_HA_SCOPE_ATTR_HA_SET_ID;
    ha_scope_attrs[0].value.oid = ha_set_oid;

    // TODO: add ha_role to attribute value enum
    ha_scope_attrs[1].id = SAI_HA_SCOPE_ATTR_DASH_HA_ROLE;
    ha_scope_attrs[1].value.u16 = static_cast<sai_uint16_t>(entry.ha_role());

    status = sai_dash_ha_api->create_ha_scope(&sai_ha_scope_oid,
                                         gSwitchId,
                                         attr_count,
                                         ha_scope_attrs);

    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to create HA Scope object in SAI for %s", key.c_str());
        task_process_status handle_status = handleSaiCreateStatus((sai_api_t) SAI_API_DASH_HA, status);
        if (handle_status != task_success)
        {
            return parseHandleSaiStatusFailure(handle_status);
        }
    }
    m_ha_scope_entries[key] = HaScopeEntry {sai_ha_scope_oid, entry};
    SWSS_LOG_NOTICE("Created HA Scope object for %s", key.c_str());

    // set HA Scope ID to ENI
    if (ha_set_it->second.metadata.scope() == dash::ha_set::Scope::SCOPE_ENI)
    {
        auto eni_entry = m_dash_orch->getEni(key);
        if (eni_entry == nullptr)
        {
            SWSS_LOG_ERROR("ENI entry does not exist for %s", key.c_str());
            return false;
        }

        return setEniHaScopeId(eni_entry->eni_id, sai_ha_scope_oid);

    } else if (ha_set_it->second.metadata.scope() == dash::ha_set::Scope::SCOPE_DPU)
    {
        auto eni_table = m_dash_orch->getEniTable();
        auto it = eni_table->begin();
        bool success = true;
        while (it != eni_table->end())
        {
            if (!setEniHaScopeId(it->second.eni_id, sai_ha_scope_oid))
            {
                SWSS_LOG_ERROR("Failed to set HA Scope ID for ENI %s", it->first.c_str());
                success = false;
            }
            it++;
        }

        if (!success)
        {
            return false;
        }
    }
    else
    {
        SWSS_LOG_ERROR("Invalid HA Scope type %s: %s", ha_set_it->first.c_str(), ha_set_scope_to_str_map[ha_set_it->second.metadata.scope()].c_str());
        return false;
    }

    return true;
}

bool DashHaOrch::setHaScopeHaRole(const std::string &key, const dash::ha_scope::HaScope &entry)
{
    SWSS_LOG_ENTER();

    sai_object_id_t ha_scope_id = m_ha_scope_entries[key].ha_scope_id;

    sai_attribute_t ha_scope_attr;
    ha_scope_attr.id = SAI_HA_SCOPE_ATTR_DASH_HA_ROLE;
    ha_scope_attr.value.u32 = entry.ha_role();

    sai_status_t status = sai_dash_ha_api->set_ha_scope_attribute(ha_scope_id,
                                                                &ha_scope_attr);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to set HA Scope role in SAI for %s", key.c_str());
        task_process_status handle_status = handleSaiSetStatus((sai_api_t) SAI_API_DASH_HA, status);
        if (handle_status != task_success)
        {
            return parseHandleSaiStatusFailure(handle_status);
        }
    }

    m_ha_scope_entries[key].metadata.set_ha_role(entry.ha_role());
    SWSS_LOG_NOTICE("Set HA Scope role for %s to %s", key.c_str(), (ha_role_to_str_map[entry.ha_role()]).c_str());

    return true;
}

bool DashHaOrch::setHaScopeFlowReconcileRequest(const std::string &key)
{
    SWSS_LOG_ENTER();

    sai_object_id_t ha_scope_id = m_ha_scope_entries[key].ha_scope_id;

    sai_attribute_t ha_scope_attr;
    ha_scope_attr.id = SAI_HA_SCOPE_ATTR_FLOW_RECONCILE_REQUESTED;
    ha_scope_attr.value.booldata = true;

    sai_status_t status = sai_dash_ha_api->set_ha_scope_attribute(ha_scope_id,
                                                                &ha_scope_attr);

    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to set HA Scope flow reconcile request in SAI for %s", key.c_str());
        task_process_status handle_status = handleSaiSetStatus((sai_api_t) SAI_API_DASH_HA, status);
        if (handle_status != task_success)
        {
            return parseHandleSaiStatusFailure(handle_status);
        }
    }
    SWSS_LOG_NOTICE("Set HA Scope flow reconcile request for %s", key.c_str());

    return true;
}

bool DashHaOrch::setHaScopeActivateRoleRequest(const std::string &key)
{
    SWSS_LOG_ENTER();

    sai_object_id_t ha_scope_id = m_ha_scope_entries[key].ha_scope_id;

    sai_attribute_t ha_scope_attr;
    ha_scope_attr.id = SAI_HA_SCOPE_ATTR_ACTIVATE_ROLE;
    ha_scope_attr.value.booldata = true;

    sai_status_t status = sai_dash_ha_api->set_ha_scope_attribute(ha_scope_id,
                                                                &ha_scope_attr);

    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to set HA Scope activate role request in SAI for %s", key.c_str());
        task_process_status handle_status = handleSaiSetStatus((sai_api_t) SAI_API_DASH_HA, status);
        if (handle_status != task_success)
        {
            return parseHandleSaiStatusFailure(handle_status);
        }
    }
    SWSS_LOG_NOTICE("Set HA Scope activate role request for %s", key.c_str());

    return true;
}

bool DashHaOrch::setEniHaScopeId(const sai_object_id_t eni_id, const sai_object_id_t ha_scope_id)
{
    SWSS_LOG_ENTER();

    sai_attribute_t eni_attr;
    eni_attr.id = SAI_ENI_ATTR_HA_SCOPE_ID;
    eni_attr.value.oid = ha_scope_id;
    sai_status_t status = sai_dash_eni_api->set_eni_attribute(eni_id, &eni_attr);

    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to set HA Scope ID for ENI %s", std::to_string(eni_id).c_str());
        task_process_status handle_status = handleSaiSetStatus((sai_api_t) SAI_API_DASH_ENI, status);
        if (handle_status != task_success)
        {
            return parseHandleSaiStatusFailure(handle_status);
        }
    }

    return true;
}

bool DashHaOrch::removeHaScopeEntry(const std::string &key)
{
    SWSS_LOG_ENTER();

    auto it = m_ha_scope_entries.find(key);

    if (it == m_ha_scope_entries.end())
    {
        SWSS_LOG_WARN("HA Scope entry does not exist for %s", key.c_str());
        return true;
    }

    sai_status_t status = sai_dash_ha_api->remove_ha_scope(it->second.ha_scope_id);

    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to remove HA Scope object in SAI for %s", key.c_str());
        task_process_status handle_status = handleSaiRemoveStatus((sai_api_t) SAI_API_DASH_HA, status);
        if (handle_status != task_success)
        {
            return parseHandleSaiStatusFailure(handle_status);
        }
    }
    m_ha_scope_entries.erase(it);
    SWSS_LOG_NOTICE("Removed HA Scope object for %s", key.c_str());

    return true;
}

void DashHaOrch::doTaskHaScopeTable(ConsumerBase &consumer)
{
    SWSS_LOG_ENTER();

    auto it = consumer.m_toSync.begin();

    while (it != consumer.m_toSync.end())
    {
        KeyOpFieldsValuesTuple tuple = it->second;
        const auto& key = kfvKey(tuple);
        const auto& op = kfvOp(tuple);

        if (op == SET_COMMAND)
        {
            dash::ha_scope::HaScope entry;

            if (!parsePbMessage(kfvFieldsValues(tuple), entry))
            {
                SWSS_LOG_WARN("Requires protobuf at HaScope :%s", key.c_str());
                it = consumer.m_toSync.erase(it);
                continue;
            }

            if (addHaScopeEntry(key, entry))
            {
                it = consumer.m_toSync.erase(it);
            }
            else
            {
                it++;
            }
        }
        else if (op == DEL_COMMAND)
        {
            if(removeHaScopeEntry(key))
            {
                it = consumer.m_toSync.erase(it);
            }
            else
            {
                it++;
            }
        }
        else
        {
            SWSS_LOG_ERROR("Unknown operation %s", op.c_str());
            it = consumer.m_toSync.erase(it);
        }
    }
}

void DashHaOrch::doTask(ConsumerBase &consumer)
{
    SWSS_LOG_ENTER();

    if (consumer.getTableName() == APP_DASH_HA_SET_TABLE_NAME)
    {
        doTaskHaSetTable(consumer);
    }
    else if (consumer.getTableName() == APP_DASH_HA_SCOPE_TABLE_NAME)
    {
        doTaskHaScopeTable(consumer);
    } else
    {
        SWSS_LOG_ERROR("Unknown table: %s", consumer.getTableName().c_str());
    }
}

sai_ip_address_t DashHaOrch::covertPbIpaddrToSaiIpaddr(const dash::types::IpAddress &ipaddr)
{
    SWSS_LOG_ENTER();

    sai_ip_address_t sai_ipaddr;
    if (ipaddr.has_ipv4())
    {
        sai_ipaddr.addr_family = SAI_IP_ADDR_FAMILY_IPV4;
        sai_ipaddr.addr.ip4 = ipaddr.ipv4();
    }
    else
    {
        sai_ipaddr.addr_family = SAI_IP_ADDR_FAMILY_IPV6;
        memcpy(sai_ipaddr.addr.ip6, ipaddr.ipv6().data(), sizeof(sai_ipaddr.addr.ip6));
    }

    return sai_ipaddr;
}