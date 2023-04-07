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
#include "crmorch.h"

using namespace std;
using namespace swss;

extern std::unordered_map<std::string, sai_object_id_t> gVnetNameToId;
extern sai_dash_vip_api_t* sai_dash_vip_api;
extern sai_dash_direction_lookup_api_t* sai_dash_direction_lookup_api;
extern sai_dash_eni_api_t* sai_dash_eni_api;
extern sai_object_id_t gSwitchId;
extern size_t gMaxBulkSize;
extern CrmOrch *gCrmOrch;

DashOrch::DashOrch(DBConnector *db, vector<string> &tableName) : Orch(db, tableName)
{
    SWSS_LOG_ENTER();
}

bool DashOrch::addApplianceEntry(const string& appliance_id, const ApplianceEntry &entry)
{
    SWSS_LOG_ENTER();

    if (appliance_entries_.find(appliance_id) != appliance_entries_.end())
    {
        SWSS_LOG_WARN("Appliance Entry already exists for %s", appliance_id.c_str());
        return true;
    }

    uint32_t attr_count = 1;
    sai_vip_entry_t vip_entry;
    vip_entry.switch_id = gSwitchId;
    swss::copy(vip_entry.vip, entry.sip);
    sai_attribute_t appliance_attr;
    vector<sai_attribute_t> appliance_attrs;
    sai_status_t status;
    appliance_attr.id = SAI_VIP_ENTRY_ATTR_ACTION;
    appliance_attr.value.u32 = SAI_VIP_ENTRY_ACTION_ACCEPT;
    status = sai_dash_vip_api->create_vip_entry(&vip_entry, attr_count, &appliance_attr);
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
    status = sai_dash_direction_lookup_api->create_direction_lookup_entry(&direction_lookup_entry, attr_count, &appliance_attr);
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
    SWSS_LOG_NOTICE("Created vip and direction lookup entries for %s", appliance_id.c_str());

    return true;
}

bool DashOrch::removeApplianceEntry(const string& appliance_id)
{
    SWSS_LOG_ENTER();

    sai_status_t status;
    ApplianceEntry entry;

    if (appliance_entries_.find(appliance_id) == appliance_entries_.end())
    {
        SWSS_LOG_WARN("Appliance id does not exist: %s", appliance_id.c_str());
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
    SWSS_LOG_NOTICE("Removed vip and direction lookup entries for %s", appliance_id.c_str());

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
        SWSS_LOG_WARN("Routing type entry already exists for %s", routing_type.c_str());
        return true;
    }

    routing_type_entries_[routing_type] = entry;
    SWSS_LOG_NOTICE("Routing type entry added %s", routing_type.c_str());

    return true;
}

bool DashOrch::removeRoutingTypeEntry(const string& routing_type)
{
    SWSS_LOG_ENTER();

    if (routing_type_entries_.find(routing_type) == routing_type_entries_.end())
    {
        SWSS_LOG_WARN("Routing type entry does not exist for %s", routing_type.c_str());
        return true;
    }

    routing_type_entries_.erase(routing_type);
    SWSS_LOG_NOTICE("Routing type entry removed for %s", routing_type.c_str());

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

bool DashOrch::addEniObject(const string& eni, EniEntry& entry)
{
    SWSS_LOG_ENTER();

    const string &vnet = entry.vnet;

    if (!vnet.empty() && gVnetNameToId.find(vnet) == gVnetNameToId.end())
    {
        SWSS_LOG_INFO("Retry as vnet %s not found", vnet.c_str());
        return false;
    }

    if (appliance_entries_.empty())
    {
        SWSS_LOG_INFO("Retry as no appliance table entry found");
        return false;
    }

    sai_object_id_t &eni_id = entry.eni_id;
    sai_attribute_t eni_attr;
    vector<sai_attribute_t> eni_attrs;

    eni_attr.id = SAI_ENI_ATTR_VNET_ID;
    eni_attr.value.oid = gVnetNameToId[entry.vnet];
    eni_attrs.push_back(eni_attr);

    bool has_qos = qos_entries_.find(entry.qos_name) != qos_entries_.end();
    if (has_qos)
    {
        eni_attr.id = SAI_ENI_ATTR_PPS;
        eni_attr.value.u32 = qos_entries_[entry.qos_name].bw;
        eni_attrs.push_back(eni_attr);

        eni_attr.id = SAI_ENI_ATTR_CPS;
        eni_attr.value.u32 = qos_entries_[entry.qos_name].cps;
        eni_attrs.push_back(eni_attr);

        eni_attr.id = SAI_ENI_ATTR_FLOWS;
        eni_attr.value.u32 = qos_entries_[entry.qos_name].flows;
        eni_attrs.push_back(eni_attr);
    }

    eni_attr.id = SAI_ENI_ATTR_ADMIN_STATE;
    eni_attr.value.booldata = entry.admin_state;
    eni_attrs.push_back(eni_attr);

    eni_attr.id = SAI_ENI_ATTR_VM_UNDERLAY_DIP;
    copy(eni_attr.value.ipaddr, entry.underlay_ip);
    eni_attrs.push_back(eni_attr);

    eni_attr.id = SAI_ENI_ATTR_VM_VNI;
    auto app_entry = appliance_entries_.begin()->second;
    eni_attr.value.u32 = app_entry.vm_vni;
    eni_attrs.push_back(eni_attr);

    sai_status_t status = sai_dash_eni_api->create_eni(&eni_id, gSwitchId,
                                (uint32_t)eni_attrs.size(), eni_attrs.data());
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to create ENI object for %s", eni.c_str());
        task_process_status handle_status = handleSaiCreateStatus((sai_api_t) SAI_API_DASH_ENI, status);
        if (handle_status != task_success)
        {
            return parseHandleSaiStatusFailure(handle_status);
        }
    }

    gCrmOrch->incCrmResUsedCounter(CrmResourceType::CRM_DASH_ENI);

    SWSS_LOG_NOTICE("Created ENI object for %s", eni.c_str());

    return true;
}

bool DashOrch::addEniAddrMapEntry(const string& eni, const EniEntry& entry)
{
    SWSS_LOG_ENTER();

    uint32_t attr_count = 1;
    MacAddress mac_addr = MacAddress(entry.mac_address);
    sai_eni_ether_address_map_entry_t eni_ether_address_map_entry;
    eni_ether_address_map_entry.switch_id = gSwitchId;
    memcpy(eni_ether_address_map_entry.address, mac_addr.getMac(), sizeof(sai_mac_t));

    sai_attribute_t eni_ether_address_map_entry_attr;
    eni_ether_address_map_entry_attr.id = SAI_ENI_ETHER_ADDRESS_MAP_ENTRY_ATTR_ENI_ID;
    eni_ether_address_map_entry_attr.value.oid = entry.eni_id;

    sai_status_t status = sai_dash_eni_api->create_eni_ether_address_map_entry(&eni_ether_address_map_entry, attr_count,
                                                                                &eni_ether_address_map_entry_attr);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to create ENI ether address map entry for %s", entry.mac_address.c_str());
        task_process_status handle_status = handleSaiCreateStatus((sai_api_t) SAI_API_DASH_ENI, status);
        if (handle_status != task_success)
        {
            return parseHandleSaiStatusFailure(handle_status);
        }
    }

    gCrmOrch->incCrmResUsedCounter(CrmResourceType::CRM_DASH_ENI_ETHER_ADDRESS_MAP);

    SWSS_LOG_NOTICE("Created ENI ether address map entry for %s", eni.c_str());

    return true;
}

bool DashOrch::addEni(const string& eni, EniEntry &entry)
{
    SWSS_LOG_ENTER();

    if (eni_entries_.find(eni) != eni_entries_.end())
    {
        SWSS_LOG_WARN("ENI %s already exists", eni.c_str());
        return true;
    }

    if (!addEniObject(eni, entry) || !addEniAddrMapEntry(eni, entry))
    {
        return false;
    }
    eni_entries_[eni] = entry;

    return true;
}

const EniEntry *DashOrch::getEni(const string& eni) const
{
    SWSS_LOG_ENTER();

    auto it = eni_entries_.find(eni);
    if (it == eni_entries_.end())
    {
        return nullptr;
    }

    return &it->second;
}

bool DashOrch::removeEniObject(const string& eni)
{
    SWSS_LOG_ENTER();

    EniEntry entry = eni_entries_[eni];
    sai_status_t status = sai_dash_eni_api->remove_eni(entry.eni_id);
    if (status != SAI_STATUS_SUCCESS)
    {
        //Retry later if object is in use
        if (status == SAI_STATUS_OBJECT_IN_USE)
        {
            return false;
        }
        SWSS_LOG_ERROR("Failed to remove ENI object for %s", eni.c_str());
        task_process_status handle_status = handleSaiRemoveStatus((sai_api_t) SAI_API_DASH_ENI, status);
        if (handle_status != task_success)
        {
            return parseHandleSaiStatusFailure(handle_status);
        }
    }

    gCrmOrch->decCrmResUsedCounter(CrmResourceType::CRM_DASH_ENI);

    SWSS_LOG_NOTICE("Removed ENI object for %s", eni.c_str());

    return true;
}

bool DashOrch::removeEniAddrMapEntry(const string& eni)
{
    SWSS_LOG_ENTER();

    EniEntry entry = eni_entries_[eni];
    MacAddress mac_addr = MacAddress(entry.mac_address);
    sai_eni_ether_address_map_entry_t eni_ether_address_map_entry;
    eni_ether_address_map_entry.switch_id = gSwitchId;
    memcpy(eni_ether_address_map_entry.address, mac_addr.getMac(), sizeof(sai_mac_t));

    sai_status_t status = sai_dash_eni_api->remove_eni_ether_address_map_entry(&eni_ether_address_map_entry);
    if (status != SAI_STATUS_SUCCESS)
    {
        if (status == SAI_STATUS_ITEM_NOT_FOUND || status == SAI_STATUS_INVALID_PARAMETER)
        {
            // Entry might have already been deleted. Do not retry
            return true;
        }
        SWSS_LOG_ERROR("Failed to remove ENI ether address map entry for %s", eni.c_str());
        task_process_status handle_status = handleSaiRemoveStatus((sai_api_t) SAI_API_DASH_ENI, status);
        if (handle_status != task_success)
        {
            return parseHandleSaiStatusFailure(handle_status);
        }
    }

    gCrmOrch->decCrmResUsedCounter(CrmResourceType::CRM_DASH_ENI_ETHER_ADDRESS_MAP);

    SWSS_LOG_NOTICE("Removed ENI ether address map entry for %s", eni.c_str());

    return true;
}

bool DashOrch::removeEni(const string& eni)
{
    SWSS_LOG_ENTER();

    if (eni_entries_.find(eni) == eni_entries_.end())
    {
        SWSS_LOG_WARN("ENI %s does not exist", eni.c_str());
        return true;
    }
    if (!removeEniAddrMapEntry(eni) || !removeEniObject(eni))
    {
        return false;
    }
    eni_entries_.erase(eni);

    return true;
}

void DashOrch::doTaskEniTable(Consumer& consumer)
{
    SWSS_LOG_ENTER();

    const auto& tn = consumer.getTableName();

    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
        auto t = it->second;
        string eni = kfvKey(t);
        string op = kfvOp(t);
        EniEntry entry;
        for (auto i : kfvFieldsValues(t))
        {
            if (fvField(i) == "mac_address")
            {
                entry.mac_address = fvValue(i);
            }
            else if (fvField(i) == "underlay_ip")
            {
                entry.underlay_ip = IpAddress(fvValue(i));
            }
            else if (fvField(i) == "admin_state")
            {
                entry.admin_state = (fvValue(i) == "enabled" ? true : false);
            }
            else if (fvField(i) == "vnet")
            {
                entry.vnet = fvValue(i);
            }
            else if (fvField(i) == "qos")
            {
                entry.qos_name = fvValue(i);
            }
        }
        if (op == SET_COMMAND)
        {
            if (addEni(eni, entry))
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
            if (removeEni(eni))
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

bool DashOrch::addQosEntry(const string& qos_name, const QosEntry &entry)
{
    SWSS_LOG_ENTER();

    if (qos_entries_.find(qos_name) != qos_entries_.end())
    {
        return true;
    }

    qos_entries_[qos_name] = entry;
    SWSS_LOG_NOTICE("Added QOS entries for %s", qos_name.c_str());

    return true;
}

bool DashOrch::removeQosEntry(const string& qos_name)
{
    SWSS_LOG_ENTER();

    if (qos_entries_.find(qos_name) == qos_entries_.end())
    {
        return true;
    }
    qos_entries_.erase(qos_name);
    SWSS_LOG_NOTICE("Removed QOS entries for %s", qos_name.c_str());

    return true;
}

void DashOrch::doTaskQosTable(Consumer& consumer)
{
    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
        KeyOpFieldsValuesTuple t = it->second;
        string qos_name = kfvKey(t);
        string op = kfvOp(t);

        if (op == SET_COMMAND)
        {
            QosEntry entry;

            for (auto i : kfvFieldsValues(t))
            {
                if (fvField(i) == "qos_id")
                {
                    entry.qos_id = fvValue(i);
                }
                else if (fvField(i) == "bw")
                {
                    entry.bw = to_uint<uint32_t>(fvValue(i));
                }
                else if (fvField(i) == "cps")
                {
                    entry.cps = to_uint<uint32_t>(fvValue(i));
                }
                else if (fvField(i) == "flows")
                {
                    entry.flows = to_uint<uint32_t>(fvValue(i));
                }
            }
            if (addQosEntry(qos_name, entry))
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
            if (removeQosEntry(qos_name))
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
    else if (tn == APP_DASH_ENI_TABLE_NAME)
    {
        doTaskEniTable(consumer);
    }
    else if (tn == APP_DASH_QOS_TABLE_NAME)
    {
        doTaskQosTable(consumer);
    }
    else
    {
        SWSS_LOG_ERROR("Unknown table: %s", tn.c_str());
    }
}
