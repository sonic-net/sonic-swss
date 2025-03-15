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
    sai_attribute_t ha_set_attr_list[attr_count]={};
    sai_status_t status;
    sai_object_id_t sai_ha_set_oid = 0UL;

    ha_set_attr_list[0].id = SAI_HA_SET_ATTR_LOCAL_IP;
    ha_set_attr_list[0].value.ipaddr = entry.local_ip();

    ha_set_attr_list[1].id = SAI_HA_SET_ATTR_PEER_IP;
    ha_set_attr_list[1].value.ipaddr = entry.peer_ip();

    ha_set_attr_list[2].id = SAI_HA_SET_ATTR_CP_DATA_CHANNEL_PORT;
    ha_set_attr_list[2].value.u16 = entry.cp_data_channel_port();
    
    ha_set_attr_list[3].id = SAI_HA_SET_ATTR_DP_CHANNEL_DST_PORT;
    ha_set_attr_list[3].value.u16 = entry.dp_channel_dst_port();

    ha_set_attr_list[4].id = SAI_HA_SET_ATTR_DP_CHANNEL_SRC_PORT_MIN;
    ha_set_attr_list[4].value.u16 = entry.dp_channel_src_port_min();

    ha_set_attr_list[5].id = SAI_HA_SET_ATTR_DP_CHANNEL_SRC_PORT_MAX;
    ha_set_attr_list[5].value.u16 = entry.dp_channel_src_port_max();

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

    if (m_ha_scope_entries.find(key) != m_ha_scope_entries.end())
    {
        SWSS_LOG_WARN("HA Scope entry already exists for %s", key.c_str());
        return true;
    }

    auto ha_set_it = m_ha_set_entries.find(key);
    if (ha_set_it == m_ha_set_entries.end())
    {
        SWSS_LOG_ERROR("HA Set entry does not exist for %s", entry.ha_set_id().c_str());
        return false;
    }
    sai_object_id_t ha_set_oid = ha_set_it->second.ha_set_id;

    uint32_t attr_count = 2;
    sai_attribute_t ha_scope_attrs[attr_count]={};
    sai_status_t status;
    sai_object_id_t sai_ha_scope_oid = 0UL;

    ha_scope_attrs[0].id = SAI_HA_SCOPE_ATTR_HA_SET_ID;
    ha_scope_attrs[0].value.oid = ha_set_oid

    // TODO: add ha_role to attribute value enum
    ha_scope_attrs[1].id = SAI_HA_SCOPE_ATTR_HA_ROLE;
    ha_scope_attrs[1].value.ha_role = entry.direction();

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

    // TODO: check HLD to confirm if need to set scope_id for ENI when scope == SCOPE_DPU. If yes, how?
    if (m_ha_set_entries[key].second.scope == dash::ha_set::Scope::SCOPE_ENI)
    {
        sai_attribute_t eni_attr;
        eni_attr.id = SAI_ENI_ATTR_HA_SCOPE_ID;
        eni_attr.value.oid = sai_ha_scope_oid;

        status = sai_dash_eni_api->set_eni_attribute(ha_set_oid, &eni_attr);

        if (status != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_ERROR("Failed to set HA Scope ID for ENI %s", key.c_str());
            task_process_status handle_status = handleSaiSetStatus((sai_api_t) SAI_API_DASH_ENI, status);
            if (handle_status != task_success)
            {
                return parseHandleSaiStatusFailure(handle_status);
            }
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