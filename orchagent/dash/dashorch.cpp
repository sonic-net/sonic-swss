#include <cassert>
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <exception>
#include <inttypes.h>
#include <algorithm>

#include "converter.h"
#include "dashorch.h"
#include "macaddress.h"
#include "orch.h"
#include "sai.h"
#include "saiextensions.h"
#include "swssnet.h"
#include "tokenize.h"

using namespace std;
using namespace swss;

extern sai_dash_vip_api_t* sai_dash_vip_api;
extern sai_dash_direction_lookup_api_t* sai_dash_direction_lookup_api;
extern sai_object_id_t gSwitchId;
extern size_t gMaxBulkSize;

DashOrch::DashOrch(DBConnector *db, vector<string> &tableName) : Orch(db, tableName)
{
    SWSS_LOG_ENTER();
}

bool DashOrch::addApplianceEntry(const string& appliance_id, const ApplianceEntry &entry)
{
    SWSS_LOG_ENTER();


    if (appliance_entries_.find(appliance_id) != appliance_entries_.end())
    {
        SWSS_LOG_INFO("Appliance Entry already exists for %s", appliance_id.c_str());
        return true;
    }

    sai_vip_entry_t vip_entry;
    vip_entry.switch_id = gSwitchId;
    swss::copy(vip_entry.vip, entry.sip);
    sai_attribute_t appliance_attr;
    vector<sai_attribute_t> appliance_attrs;
    sai_status_t status;
    appliance_attr.id = SAI_VIP_ENTRY_ATTR_ACTION;
    appliance_attr.value.u32 = SAI_VIP_ENTRY_ACTION_ACCEPT;
    status = sai_dash_vip_api->create_vip_entry(&vip_entry, 1, &appliance_attr);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to create vip entry for %s", appliance_id.c_str());
        task_process_status handle_status = handleSaiCreateStatus((sai_api_t) SAI_API_DASH_VIP, status);
        if (handle_status != task_success)
        {
            return parseHandleSaiStatusFailure(handle_status);
        }
    }

    sai_direction_lookup_entry_t direction_lookup_entry;
    direction_lookup_entry.switch_id = gSwitchId;
    direction_lookup_entry.vni = entry.vm_vni;
    appliance_attr.id = SAI_DIRECTION_LOOKUP_ENTRY_ATTR_ACTION;
    appliance_attr.value.u32 = SAI_DIRECTION_LOOKUP_ENTRY_ACTION_SET_OUTBOUND_DIRECTION;
    status = sai_dash_direction_lookup_api->create_direction_lookup_entry(&direction_lookup_entry, 1, &appliance_attr);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to create direction lookup entry for %s", appliance_id.c_str());
        task_process_status handle_status = handleSaiCreateStatus((sai_api_t) SAI_API_DASH_DIRECTION_LOOKUP, status);
        if (handle_status != task_success)
        {
            return parseHandleSaiStatusFailure(handle_status);
        }
    }

    appliance_entries_[appliance_id] = entry;
    SWSS_LOG_INFO("Created vip and direction lookup entries for %s", appliance_id.c_str());
    return true;
}

bool DashOrch::removeApplianceEntry(const string& appliance_id)
{
    SWSS_LOG_ENTER();

    sai_status_t status;
    ApplianceEntry entry;

    if (appliance_entries_.find(appliance_id) == appliance_entries_.end())
    {
        SWSS_LOG_INFO("Appliance id does not exist: %s", appliance_id.c_str());
        return true;
    }

    entry = appliance_entries_[appliance_id];
    sai_vip_entry_t vip_entry;
    vip_entry.switch_id = gSwitchId;
    swss::copy(vip_entry.vip, entry.sip);
    status = sai_dash_vip_api->remove_vip_entry(&vip_entry);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to remove vip entry for %s", appliance_id.c_str());
        task_process_status handle_status = handleSaiRemoveStatus((sai_api_t) SAI_API_DASH_VIP, status);
        if (handle_status != task_success)
        {
            return parseHandleSaiStatusFailure(handle_status);
        }
    }

    sai_direction_lookup_entry_t direction_lookup_entry;
    direction_lookup_entry.switch_id = gSwitchId;
    direction_lookup_entry.vni = entry.vm_vni;
    status = sai_dash_direction_lookup_api->remove_direction_lookup_entry(&direction_lookup_entry);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to remove direction lookup entry for %s", appliance_id.c_str());
        task_process_status handle_status = handleSaiRemoveStatus((sai_api_t) SAI_API_DASH_DIRECTION_LOOKUP, status);
        if (handle_status != task_success)
        {
            return parseHandleSaiStatusFailure(handle_status);
        }
    }

    appliance_entries_.erase(appliance_id);

    SWSS_LOG_INFO("Removed vip and direction lookup entries for %s", appliance_id.c_str());
    return true;
}

void DashOrch::doTaskApplianceTable(Consumer& consumer)
{
    SWSS_LOG_ENTER();

    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
        KeyOpFieldsValuesTuple t = it->second;
        string appliance_id = kfvKey(t);
        string op = kfvOp(t);

        if (op == SET_COMMAND)
        {
            ApplianceEntry entry;

            for (auto i : kfvFieldsValues(t))
            {
                if (fvField(i) == "sip")
                {
                    entry.sip = IpAddress(fvValue(i));
                }
                else if (fvField(i) == "vm_vni")
                {
                    entry.vm_vni = to_uint<uint32_t>(fvValue(i));
                }
            }
            if (addApplianceEntry(appliance_id, entry))
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
            if (removeApplianceEntry(appliance_id))
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

bool DashOrch::addRoutingTypeEntry(const string& routing_type, const RoutingTypeEntry &entry)
{
    SWSS_LOG_ENTER();

    if (routing_type_entries_.find(routing_type) != routing_type_entries_.end())
    {
        SWSS_LOG_INFO("Routing type entry already exists for %s", routing_type.c_str());
        return true;
    }

    routing_type_entries_[routing_type] = entry;

    SWSS_LOG_ERROR("Routing type entry added %s", routing_type.c_str());
    return true;
}

bool DashOrch::removeRoutingTypeEntry(const string& routing_type)
{
    SWSS_LOG_ENTER();

    if (routing_type_entries_.find(routing_type) == routing_type_entries_.end())
    {
        SWSS_LOG_INFO("Routing type entry does not exist for %s", routing_type.c_str());
        return true;
    }

    routing_type_entries_.erase(routing_type);

    SWSS_LOG_ERROR("Routing type entry removed for %s", routing_type.c_str());
    return true;
}

void DashOrch::doTaskRoutingTypeTable(Consumer& consumer)
{
    SWSS_LOG_ENTER();

    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
        KeyOpFieldsValuesTuple t = it->second;
        string routing_type = kfvKey(t);
        string op = kfvOp(t);

        if (op == SET_COMMAND)
        {
            RoutingTypeEntry entry;

            for (auto i : kfvFieldsValues(t))
            {
                if (fvField(i) == "action_name")
                {
                    entry.action_name = fvValue(i);
                }
                else if (fvField(i) == "action_type")
                {
                    entry.action_type = fvValue(i);
                }
                else if (fvField(i) == "encap_type")
                {
                    entry.encap_type = fvValue(i);
                }
                else if (fvField(i) == "vni")
                {
                    entry.vni = to_uint<uint32_t>(fvValue(i));
                }
            }
            if (addRoutingTypeEntry(routing_type, entry))
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
            if (removeRoutingTypeEntry(routing_type))
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

void DashOrch::doTask(Consumer& consumer)
{
    SWSS_LOG_ENTER();

    const auto& tn = consumer.getTableName();

    SWSS_LOG_INFO("Table name: %s", tn.c_str());

    if (tn == APP_DASH_APPLIANCE_TABLE_NAME)
    {
        doTaskApplianceTable(consumer);
    }
    else if (tn == APP_DASH_ROUTING_TYPE_TABLE_NAME)
    {
        doTaskRoutingTypeTable(consumer);
    }
    else
    {
        SWSS_LOG_ERROR("Unknown table: %s", tn.c_str());
    }
}
