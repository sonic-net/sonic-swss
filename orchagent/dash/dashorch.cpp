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
#include "saihelper.h"
#include "directory.h"

#include "taskworker.h"
#include "pbutils.h"
#include "dashrouteorch.h"

using namespace std;
using namespace swss;

extern Directory<Orch*> gDirectory;
extern std::unordered_map<std::string, sai_object_id_t> gVnetNameToId;
extern sai_dash_appliance_api_t* sai_dash_appliance_api;
extern sai_dash_vip_api_t* sai_dash_vip_api;
extern sai_dash_direction_lookup_api_t* sai_dash_direction_lookup_api;
extern sai_dash_eni_api_t* sai_dash_eni_api;
extern sai_object_id_t gSwitchId;
extern size_t gMaxBulkSize;
extern CrmOrch *gCrmOrch;

DashOrch::DashOrch(DBConnector *db, vector<string> &tableName, ZmqServer *zmqServer) : ZmqOrch(db, tableName, zmqServer)
{
    SWSS_LOG_ENTER();
}

bool DashOrch::getRouteTypeActions(dash::route_type::RoutingType routing_type, dash::route_type::RouteType& route_type)
{
    SWSS_LOG_ENTER();

    auto it = routing_type_entries_.find(routing_type);
    if (it == routing_type_entries_.end())
    {
        SWSS_LOG_WARN("Routing type %s not found", dash::route_type::RoutingType_Name(routing_type).c_str());
        return false;
    }

    route_type = it->second;
    return true;
}

bool DashOrch::addApplianceEntry(const string& appliance_id, const dash::appliance::Appliance &entry)
{
    SWSS_LOG_ENTER();

    if (appliance_entries_.find(appliance_id) != appliance_entries_.end())
    {
        SWSS_LOG_WARN("Appliance Entry already exists for %s", appliance_id.c_str());
        return true;
    }

    uint32_t attr_count = 1;
    sai_attribute_t appliance_attr;
    sai_status_t status;

    // NOTE: DASH Appliance object should be the first object pushed to SAI
    sai_object_id_t sai_appliance_id = 0UL;
    appliance_attr.id = SAI_DASH_APPLIANCE_ATTR_LOCAL_REGION_ID;
    appliance_attr.value.u32 = entry.local_region_id();
    status = sai_dash_appliance_api->create_dash_appliance(&sai_appliance_id, gSwitchId,
                                                           attr_count, &appliance_attr);
    if (status != SAI_STATUS_SUCCESS && status != SAI_STATUS_NOT_IMPLEMENTED)
    {
        SWSS_LOG_ERROR("Failed to create dash appliance object in SAI for %s", appliance_id.c_str());
        task_process_status handle_status = handleSaiCreateStatus((sai_api_t) SAI_API_DASH_APPLIANCE, status);
        if (handle_status != task_success)
        {
            return parseHandleSaiStatusFailure(handle_status);
        }
    }

    sai_vip_entry_t vip_entry;
    vip_entry.switch_id = gSwitchId;
    if (!to_sai(entry.sip(), vip_entry.vip))
    {
        return false;
    }
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
    direction_lookup_entry.vni = entry.vm_vni();
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
    appliance_entries_[appliance_id] = ApplianceEntry { sai_appliance_id, entry };
    SWSS_LOG_NOTICE("Created appliance, vip and direction lookup entries for %s", appliance_id.c_str());

    return true;
}

bool DashOrch::removeApplianceEntry(const string& appliance_id)
{
    SWSS_LOG_ENTER();

    sai_status_t status;

    if (appliance_entries_.find(appliance_id) == appliance_entries_.end())
    {
        SWSS_LOG_WARN("Appliance id does not exist: %s", appliance_id.c_str());
        return true;
    }

    const auto& entry = appliance_entries_[appliance_id].metadata;
    sai_vip_entry_t vip_entry;
    vip_entry.switch_id = gSwitchId;
    if (!to_sai(entry.sip(), vip_entry.vip))
    {
        return false;
    }
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
    direction_lookup_entry.vni = entry.vm_vni();
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

    auto sai_appliance_id = appliance_entries_[appliance_id].appliance_id;
    if (sai_appliance_id != 0UL)
    {
        status = sai_dash_appliance_api->remove_dash_appliance(sai_appliance_id);
        if (status != SAI_STATUS_SUCCESS && status != SAI_STATUS_NOT_IMPLEMENTED)
        {
            SWSS_LOG_ERROR("Failed to remove dash appliance object in SAI for %s", appliance_id.c_str());
            task_process_status handle_status = handleSaiRemoveStatus((sai_api_t) SAI_API_DASH_APPLIANCE, status);
            if (handle_status != task_success)
            {
                return parseHandleSaiStatusFailure(handle_status);
            }
        }
    }
    appliance_entries_.erase(appliance_id);
    SWSS_LOG_NOTICE("Removed appliance, vip and direction lookup entries for %s", appliance_id.c_str());

    return true;
}

void DashOrch::doTaskApplianceTable(ConsumerBase& consumer)
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
            dash::appliance::Appliance entry;

            if (!parsePbMessage(kfvFieldsValues(t), entry))
            {
                SWSS_LOG_WARN("Requires protobuff at appliance :%s", appliance_id.c_str());
                it = consumer.m_toSync.erase(it);
                continue;
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

bool DashOrch::addRoutingTypeEntry(const dash::route_type::RoutingType& routing_type, const dash::route_type::RouteType &entry)
{
    SWSS_LOG_ENTER();

    if (routing_type_entries_.find(routing_type) != routing_type_entries_.end())
    {
        SWSS_LOG_WARN("Routing type entry already exists for %s", dash::route_type::RoutingType_Name(routing_type).c_str());
        return true;
    }

    routing_type_entries_[routing_type] = entry;
    SWSS_LOG_NOTICE("Routing type entry added %s", dash::route_type::RoutingType_Name(routing_type).c_str());

    return true;
}

bool DashOrch::removeRoutingTypeEntry(const dash::route_type::RoutingType& routing_type)
{
    SWSS_LOG_ENTER();

    if (routing_type_entries_.find(routing_type) == routing_type_entries_.end())
    {
        SWSS_LOG_WARN("Routing type entry does not exist for %s", dash::route_type::RoutingType_Name(routing_type).c_str());
        return true;
    }

    routing_type_entries_.erase(routing_type);
    SWSS_LOG_NOTICE("Routing type entry removed for %s", dash::route_type::RoutingType_Name(routing_type).c_str());

    return true;
}

void DashOrch::doTaskRoutingTypeTable(ConsumerBase& consumer)
{
    SWSS_LOG_ENTER();

    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
        KeyOpFieldsValuesTuple t = it->second;
        string routing_type_str = kfvKey(t);
        string op = kfvOp(t);
        dash::route_type::RoutingType routing_type;

        std::transform(routing_type_str.begin(), routing_type_str.end(), routing_type_str.begin(), ::toupper);
        routing_type_str = "ROUTING_TYPE_" + routing_type_str;

        if (!dash::route_type::RoutingType_Parse(routing_type_str, &routing_type))
        {
            SWSS_LOG_WARN("Invalid routing type %s", routing_type_str.c_str());
            it = consumer.m_toSync.erase(it);
            continue;
        }

        if (op == SET_COMMAND)
        {
            dash::route_type::RouteType entry;

            if (!parsePbMessage(kfvFieldsValues(t), entry))
            {
                SWSS_LOG_WARN("Requires protobuff at routing type :%s", routing_type_str.c_str());
                it = consumer.m_toSync.erase(it);
                continue;
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

bool DashOrch::setEniAdminState(const string& eni, const EniEntry& entry)
{
    SWSS_LOG_ENTER();

    bool eni_enable = entry.metadata.admin_state() == dash::eni::State::STATE_ENABLED;

    sai_attribute_t eni_attr;
    eni_attr.id = SAI_ENI_ATTR_ADMIN_STATE;
    eni_attr.value.booldata = eni_enable;

    sai_status_t status = sai_dash_eni_api->set_eni_attribute(eni_entries_[eni].eni_id,
                                &eni_attr);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to set ENI admin state for %s", eni.c_str());
        task_process_status handle_status = handleSaiSetStatus((sai_api_t) SAI_API_DASH_ENI, status);
        if (handle_status != task_success)
        {
            return parseHandleSaiStatusFailure(handle_status);
        }
    }
    eni_entries_[eni].metadata.set_admin_state(entry.metadata.admin_state());
    SWSS_LOG_NOTICE("Set ENI %s admin state to %s", eni.c_str(), eni_enable ? "UP" : "DOWN");

    return true;
}

bool DashOrch::addEniObject(const string& eni, EniEntry& entry)
{
    SWSS_LOG_ENTER();

    const string &vnet = entry.metadata.vnet();

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
    eni_attr.value.oid = gVnetNameToId[entry.metadata.vnet()];
    eni_attrs.push_back(eni_attr);

    bool has_qos = qos_entries_.find(entry.metadata.qos()) != qos_entries_.end();
    if (has_qos)
    {
        eni_attr.id = SAI_ENI_ATTR_PPS;
        eni_attr.value.u32 = qos_entries_[entry.metadata.qos()].bw();
        eni_attrs.push_back(eni_attr);

        eni_attr.id = SAI_ENI_ATTR_CPS;
        eni_attr.value.u32 = qos_entries_[entry.metadata.qos()].cps();
        eni_attrs.push_back(eni_attr);

        eni_attr.id = SAI_ENI_ATTR_FLOWS;
        eni_attr.value.u32 = qos_entries_[entry.metadata.qos()].flows();
        eni_attrs.push_back(eni_attr);
    }

    eni_attr.id = SAI_ENI_ATTR_ADMIN_STATE;
    eni_attr.value.booldata = (entry.metadata.admin_state() == dash::eni::State::STATE_ENABLED);
    eni_attrs.push_back(eni_attr);

    eni_attr.id = SAI_ENI_ATTR_VM_UNDERLAY_DIP;
    if (!to_sai(entry.metadata.underlay_ip(), eni_attr.value.ipaddr))
    {
        return false;
    }
    eni_attrs.push_back(eni_attr);

    eni_attr.id = SAI_ENI_ATTR_VM_VNI;
    auto& app_entry = appliance_entries_.begin()->second.metadata;
    eni_attr.value.u32 = app_entry.vm_vni();
    eni_attrs.push_back(eni_attr);

    if (entry.metadata.has_pl_underlay_sip())
    {
        eni_attr.id = SAI_ENI_ATTR_PL_UNDERLAY_SIP;
        to_sai(entry.metadata.pl_underlay_sip(), eni_attr.value.ipaddr);
        eni_attrs.push_back(eni_attr);
    }

    if (entry.metadata.has_pl_sip_encoding())
    {
        eni_attr.id = SAI_ENI_ATTR_PL_SIP;
        to_sai(entry.metadata.pl_sip_encoding().ip(), eni_attr.value.ipaddr);
        eni_attrs.push_back(eni_attr);

        eni_attr.id = SAI_ENI_ATTR_PL_SIP_MASK;
        to_sai(entry.metadata.pl_sip_encoding().mask(), eni_attr.value.ipaddr);
        eni_attrs.push_back(eni_attr);
    }

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
    sai_eni_ether_address_map_entry_t eni_ether_address_map_entry;
    eni_ether_address_map_entry.switch_id = gSwitchId;
    memcpy(eni_ether_address_map_entry.address, entry.metadata.mac_address().c_str(), sizeof(sai_mac_t));

    sai_attribute_t eni_ether_address_map_entry_attr;
    eni_ether_address_map_entry_attr.id = SAI_ENI_ETHER_ADDRESS_MAP_ENTRY_ATTR_ENI_ID;
    eni_ether_address_map_entry_attr.value.oid = entry.eni_id;

    sai_status_t status = sai_dash_eni_api->create_eni_ether_address_map_entry(&eni_ether_address_map_entry, attr_count,
                                                                                &eni_ether_address_map_entry_attr);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to create ENI ether address map entry for %s", MacAddress::to_string(reinterpret_cast<const uint8_t *>(entry.metadata.mac_address().c_str())).c_str());
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

    auto it = eni_entries_.find(eni);
    if (it != eni_entries_.end() && it->second.metadata.admin_state() != entry.metadata.admin_state())
    {
        return setEniAdminState(eni, entry);
    }

    else if (it != eni_entries_.end())
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
    sai_eni_ether_address_map_entry_t eni_ether_address_map_entry;
    eni_ether_address_map_entry.switch_id = gSwitchId;
    memcpy(eni_ether_address_map_entry.address, entry.metadata.mac_address().c_str(), sizeof(sai_mac_t));

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

void DashOrch::doTaskEniTable(ConsumerBase& consumer)
{
    SWSS_LOG_ENTER();

    const auto& tn = consumer.getTableName();

    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
        auto t = it->second;
        string eni = kfvKey(t);
        string op = kfvOp(t);
        if (op == SET_COMMAND)
        {
            EniEntry entry;

            if (!parsePbMessage(kfvFieldsValues(t), entry.metadata))
            {
                SWSS_LOG_WARN("Requires protobuff at ENI :%s", eni.c_str());
                it = consumer.m_toSync.erase(it);
                continue;
            }

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

bool DashOrch::addQosEntry(const string& qos_name, const dash::qos::Qos &entry)
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

void DashOrch::doTaskQosTable(ConsumerBase& consumer)
{
    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
        KeyOpFieldsValuesTuple t = it->second;
        string qos_name = kfvKey(t);
        string op = kfvOp(t);

        if (op == SET_COMMAND)
        {
            dash::qos::Qos entry;

            if (!parsePbMessage(kfvFieldsValues(t), entry))
            {
                SWSS_LOG_WARN("Requires protobuff at QOS :%s", qos_name.c_str());
                it = consumer.m_toSync.erase(it);
                continue;
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

bool DashOrch::setEniRoute(const std::string& eni, const dash::eni_route::EniRoute& entry)
{
    SWSS_LOG_ENTER();


    if (eni_entries_.find(eni) == eni_entries_.end())
    {
        SWSS_LOG_INFO("ENI %s not yet created, not programming ENI route entry", eni.c_str());
        return false;
    }

    DashRouteOrch *dash_route_orch = gDirectory.get<DashRouteOrch*>();
    sai_object_id_t route_group_oid = dash_route_orch->getRouteGroupOid(entry.group_id());
    if (route_group_oid == SAI_NULL_OBJECT_ID)
    {
        SWSS_LOG_INFO("Route group not yet created, skipping route entry for ENI %s", entry.group_id().c_str());
        return false;
    }

    std::string old_group_id;
    if (eni_route_entries_.find(eni) != eni_route_entries_.end())
    {
        if (eni_route_entries_[eni].group_id() != entry.group_id())
        {
            old_group_id = eni_route_entries_[eni].group_id();
            SWSS_LOG_INFO("Updating route entry from %s to %s for ENI %s", eni_route_entries_[eni].group_id().c_str(), entry.group_id().c_str(), eni.c_str());
        }
        else
        {
            SWSS_LOG_WARN("Duplicate ENI route entry already exists for %s", eni.c_str());
            return true;
        }
    }

    sai_attribute_t eni_attr;
    eni_attr.id = SAI_ENI_ATTR_OUTBOUND_ROUTING_GROUP_ID;
    eni_attr.value.oid = route_group_oid;

    sai_status_t status = sai_dash_eni_api->set_eni_attribute(eni_entries_[eni].eni_id,
                                &eni_attr);

    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to set ENI route group for %s", eni.c_str());
        task_process_status handle_status = handleSaiSetStatus((sai_api_t) SAI_API_DASH_ENI, status);
        if (handle_status != task_success)
        {
            return parseHandleSaiStatusFailure(handle_status);
        }
    }
    eni_route_entries_[eni] = entry;
    dash_route_orch->bindRouteGroup(entry.group_id());

    if (!old_group_id.empty())
    {
        dash_route_orch->unbindRouteGroup(old_group_id);
    }

    SWSS_LOG_NOTICE("Updated ENI route group for %s to route group %s", eni.c_str(), entry.group_id().c_str());
    return true;
}

bool DashOrch::removeEniRoute(const std::string& eni)
{
    SWSS_LOG_ENTER();

    if (eni_route_entries_.find(eni) == eni_route_entries_.end())
    {
        SWSS_LOG_WARN("ENI route entry does not exist for %s", eni.c_str());
        return true;
    }

    if (eni_entries_.find(eni) != eni_entries_.end())
    {
        sai_attribute_t eni_attr;
        eni_attr.id = SAI_ENI_ATTR_OUTBOUND_ROUTING_GROUP_ID;
        eni_attr.value.oid = SAI_NULL_OBJECT_ID;

        sai_status_t status = sai_dash_eni_api->set_eni_attribute(eni_entries_[eni].eni_id,
                                    &eni_attr);
        if (status != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_ERROR("Failed to remove ENI route for %s", eni.c_str());
            task_process_status handle_status = handleSaiSetStatus((sai_api_t) SAI_API_DASH_ENI, status);
            if (handle_status != task_success)
            {
                return parseHandleSaiStatusFailure(handle_status);
            }
        }
    }

    DashRouteOrch *dash_route_orch = gDirectory.get<DashRouteOrch*>();
    dash_route_orch->unbindRouteGroup(eni_route_entries_[eni].group_id());
    eni_route_entries_.erase(eni);

    SWSS_LOG_NOTICE("Removed ENI route entry for %s", eni.c_str());

    return true;
}

void DashOrch::doTaskEniRouteTable(ConsumerBase& consumer)
{
    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
        KeyOpFieldsValuesTuple t = it->second;
        string eni = kfvKey(t);
        string op = kfvOp(t);

        if (op == SET_COMMAND)
        {
            dash::eni_route::EniRoute entry;

            if (!parsePbMessage(kfvFieldsValues(t), entry))
            {
                SWSS_LOG_WARN("Requires protobuf at ENI route:%s", eni.c_str());
                it = consumer.m_toSync.erase(it);
                continue;
            }

            if (setEniRoute(eni, entry))
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
            if (removeEniRoute(eni))
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

void DashOrch::doTask(ConsumerBase& consumer)
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
    else if (tn == APP_DASH_ENI_ROUTE_TABLE_NAME)
    {
        doTaskEniRouteTable(consumer);
    }
    else
    {
        SWSS_LOG_ERROR("Unknown table: %s", tn.c_str());
    }
}
