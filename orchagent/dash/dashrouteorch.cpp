#include <cassert>
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <exception>
#include <inttypes.h>
#include <algorithm>

#include "converter.h"
#include "dashrouteorch.h"
#include "macaddress.h"
#include "orch.h"
#include "sai.h"
#include "saiextensions.h"
#include "swssnet.h"
#include "tokenize.h"

using namespace std;
using namespace swss;

extern std::unordered_map<std::string, sai_object_id_t> gVnetNameToId;
extern sai_dash_eni_api_t* sai_dash_eni_api;
extern sai_dash_outbound_routing_api_t* sai_dash_outbound_routing_api;
extern sai_dash_inbound_routing_api_t* sai_dash_inbound_routing_api;
extern sai_object_id_t gSwitchId;
extern size_t gMaxBulkSize;

static std::unordered_map<std::string, sai_outbound_routing_entry_action_t> sOutboundAction =
{
    { "vnet", SAI_OUTBOUND_ROUTING_ENTRY_ACTION_ROUTE_VNET },
    { "vnet_direct", SAI_OUTBOUND_ROUTING_ENTRY_ACTION_ROUTE_VNET_DIRECT },
    { "route_direct", SAI_OUTBOUND_ROUTING_ENTRY_ACTION_ROUTE_DIRECT },
    { "drop", SAI_OUTBOUND_ROUTING_ENTRY_ACTION_DROP }
};

DashRouteOrch::DashRouteOrch(DBConnector *db, vector<string> &tableName) :
    outbound_routing_bulker_(sai_dash_outbound_routing_api, gMaxBulkSize),
    inbound_routing_bulker_(sai_dash_inbound_routing_api, gMaxBulkSize),
    Orch(db, tableName)
{
    SWSS_LOG_ENTER();
}

bool DashRouteOrch::addEniEntry(const string& eni)
{
    SWSS_LOG_ENTER();

    if (eni_entries_[eni].eniEntryAdded)
    {
        SWSS_LOG_ERROR("ENI entry already exists for %s", eni.c_str());
        return true;
    }

    EniEntry &entry = eni_entries_[eni].eni_entry;
    const string &vnet = entry.vnet;

    if (!vnet.empty() && gVnetNameToId.find(vnet) == gVnetNameToId.end())
    {
        SWSS_LOG_ERROR("Retry as vnet %s not found", vnet.c_str());
        return false;
    }

    sai_object_id_t &eni_id = entry.eni_id;
    sai_attribute_t eni_attr;
    vector<sai_attribute_t> eni_attrs;

    eni_attr.id = SAI_ENI_ATTR_VNET_ID;
    eni_attr.value.oid = gVnetNameToId[entry.vnet];
    eni_attrs.push_back(eni_attr);

    bool has_qos = qos_entries_.find(entry.qos_name) != qos_entries_.end();

    eni_attr.id = SAI_ENI_ATTR_PPS;
    eni_attr.value.u32 = has_qos ? qos_entries_[entry.qos_name].bw : 0;
    eni_attrs.push_back(eni_attr);

    eni_attr.id = SAI_ENI_ATTR_CPS;
    eni_attr.value.u32 = has_qos ? qos_entries_[entry.qos_name].cps : 0;
    eni_attrs.push_back(eni_attr);

    eni_attr.id = SAI_ENI_ATTR_FLOWS;
    eni_attr.value.u32 = has_qos ? qos_entries_[entry.qos_name].flows : 0;
    eni_attrs.push_back(eni_attr);

    eni_attr.id = SAI_ENI_ATTR_ADMIN_STATE;
    eni_attr.value.booldata = entry.admin_state;
    eni_attrs.push_back(eni_attr);

    sai_status_t status = sai_dash_eni_api->create_eni(&eni_id, gSwitchId,
                                (uint32_t)eni_attrs.size(), eni_attrs.data());
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to create ENI entry for %s", eni.c_str());
        task_process_status handle_status = handleSaiCreateStatus((sai_api_t) SAI_API_DASH_ENI, status);
        if (handle_status != task_success)
        {
            return parseHandleSaiStatusFailure(handle_status);
        }
    }

    eni_entries_[eni].eniEntryAdded = true;
    entry.eni_id = eni_id;
    SWSS_LOG_INFO("Created ENI entry for %s", eni.c_str());

    return true;
}

bool DashRouteOrch::addEniAddrMapEntry(const string& eni)
{
    SWSS_LOG_ENTER();

    if (eni_entries_[eni].eniAddrMapEntryAdded)
    {
        SWSS_LOG_ERROR("ENI ether address map entry already exists for %s", eni.c_str());
        return true;
    }
    if (!eni_entries_[eni].eniEntryAdded)
    {
        // return false to retry
        return false;
    }

    EniEntry entry = eni_entries_[eni].eni_entry;
    MacAddress mac_addr = MacAddress(entry.mac_address);
    sai_eni_ether_address_map_entry_t eni_ether_address_map_entry;
    eni_ether_address_map_entry.switch_id = gSwitchId;
    memcpy(eni_ether_address_map_entry.address, mac_addr.getMac(), sizeof(sai_mac_t));

    sai_attribute_t eni_ether_address_map_entry_attr;
    eni_ether_address_map_entry_attr.id = SAI_ENI_ETHER_ADDRESS_MAP_ENTRY_ATTR_ENI_ID;
    eni_ether_address_map_entry_attr.value.oid = entry.eni_id;

    sai_status_t status = sai_dash_eni_api->create_eni_ether_address_map_entry(&eni_ether_address_map_entry, 1, &eni_ether_address_map_entry_attr);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to create ENI ether address map entry for %s", entry.mac_address.c_str());
        task_process_status handle_status = handleSaiCreateStatus((sai_api_t) SAI_API_DASH_ENI, status);
        if (handle_status != task_success)
        {
            return parseHandleSaiStatusFailure(handle_status);
        }
    }

    eni_entries_[eni].eniAddrMapEntryAdded = true;
    SWSS_LOG_INFO("Created ENI ether address map entry for %s", eni.c_str());

    return true;
}

bool DashRouteOrch::addEni(const string& eni, const EniEntry &entry)
{
    SWSS_LOG_ENTER();

    Eni obj;

    if (eni_entries_.find(eni) == eni_entries_.end())
    {
        obj.eni_entry = entry;
        obj.eniEntryAdded = false;
        obj.eniAddrMapEntryAdded = false;
        eni_entries_[eni] = obj;
    }

    return addEniEntry(eni) && addEniAddrMapEntry(eni);
}

bool DashRouteOrch::removeEniEntry(const string& eni)
{
    SWSS_LOG_ENTER();

    if (!eni_entries_[eni].eniEntryAdded)
    {
        SWSS_LOG_ERROR("ENI entry does not exist for %s", eni.c_str());
        return true;
    }

    EniEntry entry = eni_entries_[eni].eni_entry;
    sai_status_t status = sai_dash_eni_api->remove_eni(entry.eni_id);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to remove ENI entry for %s", eni.c_str());

        //Retry later if object is in use
        if (status == SAI_STATUS_OBJECT_IN_USE)
        {
            return false;
        }
        task_process_status handle_status = handleSaiRemoveStatus((sai_api_t) SAI_API_DASH_ENI, status);
        if (handle_status != task_success)
        {
            return parseHandleSaiStatusFailure(handle_status);
        }
    }

    eni_entries_[eni].eniEntryAdded = false;
    SWSS_LOG_INFO("Removed ENI entry for %s", eni.c_str());

    return true;
}

bool DashRouteOrch::removeEniAddrMapEntry(const string& eni)
{
    SWSS_LOG_ENTER();

    if (!eni_entries_[eni].eniAddrMapEntryAdded)
    {
        SWSS_LOG_ERROR("ENI ether address map entry does not exist for %s", eni.c_str());
        return true;
    }

    EniEntry entry = eni_entries_[eni].eni_entry;
    MacAddress mac_addr = MacAddress(entry.mac_address);
    sai_eni_ether_address_map_entry_t eni_ether_address_map_entry;
    eni_ether_address_map_entry.switch_id = gSwitchId;
    memcpy(eni_ether_address_map_entry.address, mac_addr.getMac(), sizeof(sai_mac_t));

    sai_status_t status = sai_dash_eni_api->remove_eni_ether_address_map_entry(&eni_ether_address_map_entry);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to remove ENI ether address map entry for %s", eni.c_str());
        task_process_status handle_status = handleSaiRemoveStatus((sai_api_t) SAI_API_DASH_ENI, status);
        if (handle_status != task_success)
        {
            return parseHandleSaiStatusFailure(handle_status);
        }
    }

    eni_entries_[eni].eniAddrMapEntryAdded = false;
    SWSS_LOG_INFO("Removed ENI ether address map entry for %s", eni.c_str());

    return true;
}

bool DashRouteOrch::removeEni(const string& eni)
{
    SWSS_LOG_ENTER();

    if (eni_entries_.find(eni) == eni_entries_.end())
    {
        SWSS_LOG_ERROR("ENI entry does not exist for %s", eni.c_str());
        return true;
    }

    if (!removeEniAddrMapEntry(eni) || !removeEniEntry(eni))
    {
        return false;
    }

    eni_entries_.erase(eni);

    return true;
}

void DashRouteOrch::doTaskEniTable(Consumer& consumer)
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

bool DashRouteOrch::addQosEntry(const string& qos_name, const QosEntry &entry)
{
    SWSS_LOG_ENTER();

    if (qos_entries_.find(qos_name) != qos_entries_.end())
    {
        return true;
    }

    qos_entries_[qos_name] = entry;
    SWSS_LOG_INFO("Added QOS entries for %s", qos_name.c_str());

    return true;
}

bool DashRouteOrch::removeQosEntry(const string& qos_name)
{
    SWSS_LOG_ENTER();

    if (qos_entries_.find(qos_name) == qos_entries_.end())
    {
        return true;
    }

    qos_entries_.erase(qos_name);
    SWSS_LOG_INFO("Removed QOS entries for %s", qos_name.c_str());

    return true;
}

void DashRouteOrch::doTaskQosTable(Consumer& consumer)
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
                    entry.qos_id = fvValue(i);
                else if (fvField(i) == "bw")
                    entry.bw = to_uint<uint32_t>(fvValue(i));
                else if (fvField(i) == "cps")
                    entry.cps = to_uint<uint32_t>(fvValue(i));
                else if (fvField(i) == "flows")
                    entry.flows = to_uint<uint32_t>(fvValue(i));
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

bool DashRouteOrch::addOutboundRouting(const string& key, OutboundRoutingBulkContext& ctxt)
{
    SWSS_LOG_ENTER();

    bool exists = (routing_entries_.find(key) != routing_entries_.end());
    if (exists)
    {
        SWSS_LOG_ERROR("Outbound routing entry already exists for %s", key.c_str());
        return true;
    }
    if (eni_entries_.find(ctxt.eni) == eni_entries_.end())
    {
        SWSS_LOG_ERROR("Retry as ENI entry %s not found", ctxt.eni.c_str());
        return false;
    }
    if (!ctxt.vnet.empty() && gVnetNameToId.find(ctxt.vnet) == gVnetNameToId.end())
    {
        SWSS_LOG_ERROR("Retry as vnet %s not found", ctxt.vnet.c_str());
        return false;
    }

    sai_outbound_routing_entry_t outbound_routing_entry;
    outbound_routing_entry.switch_id = gSwitchId;
    outbound_routing_entry.eni_id = eni_entries_[ctxt.eni].eni_entry.eni_id;
    swss::copy(outbound_routing_entry.destination, ctxt.destination);
    sai_attribute_t outbound_routing_attr;
    vector<sai_attribute_t> outbound_routing_attrs;
    auto& object_statuses = ctxt.object_statuses;

    outbound_routing_attr.id = SAI_OUTBOUND_ROUTING_ENTRY_ATTR_ACTION;
    outbound_routing_attr.value.u32 = sOutboundAction[ctxt.action_type];
    outbound_routing_attrs.push_back(outbound_routing_attr);

    if (!ctxt.vnet.empty())
    {
        outbound_routing_attr.id = SAI_OUTBOUND_ROUTING_ENTRY_ATTR_DST_VNET_ID;
        outbound_routing_attr.value.oid = gVnetNameToId[ctxt.vnet];
        outbound_routing_attrs.push_back(outbound_routing_attr);
    }

    if (!ctxt.action_type.compare("vnet_direct"))
    {
        outbound_routing_attr.id = SAI_OUTBOUND_ROUTING_ENTRY_ATTR_OVERLAY_IP;
        copy(outbound_routing_attr.value.ipaddr, ctxt.overlay_ip);
        outbound_routing_attrs.push_back(outbound_routing_attr);
    }

    object_statuses.emplace_back();
    outbound_routing_bulker_.create_entry(&object_statuses.back(), &outbound_routing_entry, (uint32_t)outbound_routing_attrs.size(), outbound_routing_attrs.data());

    return false;
}

bool DashRouteOrch::addOutboundRoutingPost(const string& key, const OutboundRoutingBulkContext& ctxt)
{
    SWSS_LOG_ENTER();

    const auto& object_statuses = ctxt.object_statuses;
    if (object_statuses.empty())
    {
        return false;
    }

    auto it_status = object_statuses.begin();
    sai_status_t status = *it_status++;
    if (status != SAI_STATUS_SUCCESS)
    {
        if (status == SAI_STATUS_ITEM_ALREADY_EXISTS)
        {
            // Retry if item exists in the bulker
            return false;
        }

        SWSS_LOG_ERROR("Failed to create outbound routing entry for %s", key.c_str());
        task_process_status handle_status = handleSaiCreateStatus((sai_api_t) SAI_API_DASH_OUTBOUND_ROUTING, status);
        if (handle_status != task_success)
        {
            return parseHandleSaiStatusFailure(handle_status);
        }
    }

    OutboundRoutingEntry entry = { eni_entries_[ctxt.eni].eni_entry.eni_id, ctxt.destination, ctxt.action_type, ctxt.vnet, ctxt.overlay_ip };
    routing_entries_[key] = entry;
    SWSS_LOG_INFO("Outbound routing entry for %s added", key.c_str());

    return true;
}

bool DashRouteOrch::removeOutboundRouting(const string& key, OutboundRoutingBulkContext& ctxt)
{
    SWSS_LOG_ENTER();

    bool exists = (routing_entries_.find(key) != routing_entries_.end());
    if (!exists)
    {
        SWSS_LOG_ERROR("Failed to find outbound routing entry %s to remove", key.c_str());
        return true;
    }

    auto& object_statuses = ctxt.object_statuses;
    OutboundRoutingEntry entry = routing_entries_[key];
    sai_outbound_routing_entry_t outbound_routing_entry;
    outbound_routing_entry.switch_id = gSwitchId;
    outbound_routing_entry.eni_id = entry.eni;
    swss::copy(outbound_routing_entry.destination, entry.destination);
    object_statuses.emplace_back();
    outbound_routing_bulker_.remove_entry(&object_statuses.back(), &outbound_routing_entry);

    return false;
}

bool DashRouteOrch::removeOutboundRoutingPost(const string& key, const OutboundRoutingBulkContext& ctxt)
{
    SWSS_LOG_ENTER();

    const auto& object_statuses = ctxt.object_statuses;
    if (object_statuses.empty())
    {
        return false;
    }

    auto it_status = object_statuses.begin();
    sai_status_t status = *it_status++;
    if (status != SAI_STATUS_SUCCESS)
    {
        if (status == SAI_STATUS_NOT_EXECUTED)
        {
            // Retry if bulk operation did not execute
            return false;
        }
        SWSS_LOG_ERROR("Failed to remove outbound routing entry for %s", key.c_str());
        task_process_status handle_status = handleSaiRemoveStatus((sai_api_t) SAI_API_DASH_OUTBOUND_ROUTING, status);
        if (handle_status != task_success)
        {
            return parseHandleSaiStatusFailure(handle_status);
        }
    }

    routing_entries_.erase(key);
    SWSS_LOG_INFO("Outbound routing entry for %s removed", key.c_str());

    return true;
}

void DashRouteOrch::doTaskRouteTable(Consumer& consumer)
{
    SWSS_LOG_ENTER();

    auto it = consumer.m_toSync.begin();

    while (it != consumer.m_toSync.end())
    {
        std::map<std::pair<std::string, std::string>,
            OutboundRoutingBulkContext> toBulk;

        while (it != consumer.m_toSync.end())
        {
            KeyOpFieldsValuesTuple tuple = it->second;
            const string& key = kfvKey(tuple);
            auto op = kfvOp(tuple);
            auto rc = toBulk.emplace(std::piecewise_construct,
                    std::forward_as_tuple(key, op),
                    std::forward_as_tuple());
            bool inserted = rc.second;
            auto &ctxt = rc.first->second;

            if (!inserted)
            {
                ctxt.clear();
            }

            string& action_type = ctxt.action_type;
            string& vnet = ctxt.vnet;
            IpAddress& overlay_ip = ctxt.overlay_ip;
            string& eni = ctxt.eni;
            IpPrefix& destination = ctxt.destination;

            vector<string> keys = tokenize(key, ':');
            eni = keys[0];
            destination = IpPrefix(keys[1]);

            if (op == SET_COMMAND)

            {
                for (auto i : kfvFieldsValues(tuple))
                {
                    if (fvField(i) == "action_type")
                    {
                        action_type = fvValue(i);
                    }
                    else if (fvField(i) == "vnet")
                    {
                        vnet = fvValue(i);
                    }
                    else if (fvField(i) == "overlay_ip")
                    {
                        overlay_ip = IpAddress(fvValue(i));
                    }
                }
                if (addOutboundRouting(key, ctxt))
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
                if (removeOutboundRouting(key, ctxt))
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

        outbound_routing_bulker_.flush();

        auto it_prev = consumer.m_toSync.begin();
        while (it_prev != it)
        {
            KeyOpFieldsValuesTuple t = it_prev->second;
            string key = kfvKey(t);
            string op = kfvOp(t);
            auto found = toBulk.find(make_pair(key, op));
            if (found == toBulk.end())
            {
                it_prev++;
                continue;
            }

            const auto& ctxt = found->second;
            const auto& object_statuses = ctxt.object_statuses;
            if (object_statuses.empty())
            {
                it_prev++;
                continue;
            }

            if (op == SET_COMMAND)
            {
                if (addOutboundRoutingPost(key, ctxt))
                {
                    it_prev = consumer.m_toSync.erase(it_prev);
                }
                else
                {
                    it_prev++;
                }
            }
            else if (op == DEL_COMMAND)
            {
                if (removeOutboundRoutingPost(key, ctxt))
                {
                    it_prev = consumer.m_toSync.erase(it_prev);
                }
                else
                {
                    it_prev++;
                }
            }
        }
    }
}

bool DashRouteOrch::addInboundRouting(const string& key, InboundRoutingBulkContext& ctxt)
{
    SWSS_LOG_ENTER();

    bool exists = (routing_rule_entries_.find(key) != routing_rule_entries_.end());
    if (exists)
    {
        SWSS_LOG_ERROR("Inbound routing entry already exists for %s", key.c_str());
        return true;
    }
    if (eni_entries_.find(ctxt.eni) == eni_entries_.end())
    {
        SWSS_LOG_ERROR("Retry as ENI entry %s not found", ctxt.eni.c_str());
        return false;
    }
    if (!ctxt.vnet.empty() && gVnetNameToId.find(ctxt.vnet) == gVnetNameToId.end())
    {
        SWSS_LOG_ERROR("Retry as vnet %s not found", ctxt.vnet.c_str());
        return false;
    }

    sai_inbound_routing_entry_t inbound_routing_entry;
    IpAddress& sip_mask = ctxt.sip_mask;
    sip_mask = IpAddress("255.255.255.0");
    bool deny = !ctxt.action_type.compare("drop");

    inbound_routing_entry.switch_id = gSwitchId;
    inbound_routing_entry.eni_id = eni_entries_[ctxt.eni].eni_entry.eni_id;
    inbound_routing_entry.vni = ctxt.vni;
    swss::copy(inbound_routing_entry.sip, ctxt.sip);
    swss::copy(inbound_routing_entry.sip_mask, sip_mask);
    inbound_routing_entry.priority = ctxt.priority;
    auto& object_statuses = ctxt.object_statuses;

    sai_attribute_t inbound_routing_attr;
    vector<sai_attribute_t> inbound_routing_attrs;

    inbound_routing_attr.id = SAI_INBOUND_ROUTING_ENTRY_ATTR_ACTION;
    inbound_routing_attr.value.u32 = deny ? SAI_INBOUND_ROUTING_ENTRY_ACTION_DENY : (ctxt.pa_validation ?
                                   SAI_INBOUND_ROUTING_ENTRY_ACTION_VXLAN_DECAP_PA_VALIDATE : SAI_INBOUND_ROUTING_ENTRY_ACTION_VXLAN_DECAP);
    inbound_routing_attrs.push_back(inbound_routing_attr);

    if (!ctxt.vnet.empty())
    {
        inbound_routing_attr.id = SAI_INBOUND_ROUTING_ENTRY_ATTR_SRC_VNET_ID;
        inbound_routing_attr.value.oid = gVnetNameToId[ctxt.vnet];
        inbound_routing_attrs.push_back(inbound_routing_attr);
    }

    object_statuses.emplace_back();
    inbound_routing_bulker_.create_entry(&object_statuses.back(), &inbound_routing_entry,
                                        (uint32_t)inbound_routing_attrs.size(), inbound_routing_attrs.data());

    return false;
}

bool DashRouteOrch::addInboundRoutingPost(const string& key, const InboundRoutingBulkContext& ctxt)
{
    SWSS_LOG_ENTER();

    const auto& object_statuses = ctxt.object_statuses;
    if (object_statuses.empty())
    {
        return false;
    }

    auto it_status = object_statuses.begin();
    sai_status_t status = *it_status++;
    if (status != SAI_STATUS_SUCCESS)
    {
        if (status == SAI_STATUS_ITEM_ALREADY_EXISTS)
        {
            // Retry if item exists in the bulker
            return false;
        }

        SWSS_LOG_ERROR("Failed to create inbound routing entry");
        task_process_status handle_status = handleSaiCreateStatus((sai_api_t) SAI_API_DASH_INBOUND_ROUTING, status);
        if (handle_status != task_success)
        {
            return parseHandleSaiStatusFailure(handle_status);
        }
    }

    InboundRoutingEntry entry = { eni_entries_[ctxt.eni].eni_entry.eni_id, ctxt.vni, ctxt.sip, ctxt.sip_mask, ctxt.action_type, ctxt.vnet, ctxt.pa_validation, ctxt.priority };
    routing_rule_entries_[key] = entry;
    SWSS_LOG_INFO("Inbound routing entry for %s added", key.c_str());

    return true;
}

bool DashRouteOrch::removeInboundRouting(const string& key, InboundRoutingBulkContext& ctxt)
{
    SWSS_LOG_ENTER();

    bool exists = (routing_rule_entries_.find(key) != routing_rule_entries_.end());
    if (!exists)
    {
        SWSS_LOG_ERROR("Failed to find inbound routing entry %s to remove", key.c_str());
        return true;
    }

    auto& object_statuses = ctxt.object_statuses;
    InboundRoutingEntry entry = routing_rule_entries_[key];
    sai_inbound_routing_entry_t inbound_routing_entry;
    inbound_routing_entry.switch_id = gSwitchId;
    inbound_routing_entry.eni_id = entry.eni;
    inbound_routing_entry.vni = entry.vni;
    swss::copy(inbound_routing_entry.sip, entry.sip);
    swss::copy(inbound_routing_entry.sip_mask, entry.sip_mask);
    inbound_routing_entry.priority = entry.priority;
    object_statuses.emplace_back();
    inbound_routing_bulker_.remove_entry(&object_statuses.back(), &inbound_routing_entry);

    return false;
}

bool DashRouteOrch::removeInboundRoutingPost(const string& key, const InboundRoutingBulkContext& ctxt)
{
    SWSS_LOG_ENTER();

    const auto& object_statuses = ctxt.object_statuses;
    if (object_statuses.empty())
    {
        return false;
    }

    auto it_status = object_statuses.begin();
    sai_status_t status = *it_status++;
    if (status != SAI_STATUS_SUCCESS)
    {
        if (status == SAI_STATUS_NOT_EXECUTED)
        {
            // Retry if bulk operation did not execute
            return false;
        }
        SWSS_LOG_ERROR("Failed to remove inbound routing entry for %s", key.c_str());
        task_process_status handle_status = handleSaiRemoveStatus((sai_api_t) SAI_API_DASH_INBOUND_ROUTING, status);
        if (handle_status != task_success)
        {
            return parseHandleSaiStatusFailure(handle_status);
        }
    }


    routing_rule_entries_.erase(key);
    SWSS_LOG_INFO("Inbound routing entry for %s removed", key.c_str());

    return true;
}

void DashRouteOrch::doTaskRouteRuleTable(Consumer& consumer)
{
    SWSS_LOG_ENTER();

    auto it = consumer.m_toSync.begin();

    while (it != consumer.m_toSync.end())
    {
        std::map<std::pair<std::string, std::string>,
            InboundRoutingBulkContext> toBulk;

        while (it != consumer.m_toSync.end())
        {
            KeyOpFieldsValuesTuple tuple = it->second;
            const string& key = kfvKey(tuple);
            auto op = kfvOp(tuple);
            auto rc = toBulk.emplace(std::piecewise_construct,
                    std::forward_as_tuple(key, op),
                    std::forward_as_tuple());
            bool inserted = rc.second;
            auto &ctxt = rc.first->second;

            if (!inserted)
            {
                ctxt.clear();
            }

            string& eni = ctxt.eni;
            uint32_t& vni = ctxt.vni;
            string& action_type = ctxt.action_type;
            string& vnet = ctxt.vnet;
            IpAddress& sip = ctxt.sip;
            uint32_t& priority = ctxt.priority;
            bool& pa_validation = ctxt.pa_validation;

            vector<string> keys = tokenize(key, ':');
            eni = keys[0];
            vni = to_uint<uint32_t>(keys[1]);
            sip = IpAddress(keys[2]);

            if (op == SET_COMMAND)
            {
                for (auto i : kfvFieldsValues(tuple))
                {
                    if (fvField(i) == "action_type")
                    {
                        action_type = fvValue(i);
                    }
                    else if (fvField(i) == "vnet")
                    {
                        vnet = fvValue(i);
                    }
                    else if (fvField(i) == "priority")
                    {
                        priority = to_uint<uint32_t>(fvValue(i));
                    }
                    else if (fvField(i) == "pa_validation")
                    {
                        pa_validation = fvValue(i) == "true";
                    }
                }
                if (addInboundRouting(key, ctxt))
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
                if (removeInboundRouting(key, ctxt))
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

        inbound_routing_bulker_.flush();

        auto it_prev = consumer.m_toSync.begin();
        while (it_prev != it)
        {
            KeyOpFieldsValuesTuple t = it_prev->second;
            string key = kfvKey(t);
            string op = kfvOp(t);
            auto found = toBulk.find(make_pair(key, op));
            if (found == toBulk.end())
            {
                it_prev++;
                continue;
            }

            const auto& ctxt = found->second;
            const auto& object_statuses = ctxt.object_statuses;
            if (object_statuses.empty())
            {
                it_prev++;
                continue;
            }

            if (op == SET_COMMAND)
            {
                if (addInboundRoutingPost(key, ctxt))
                {
                    it_prev = consumer.m_toSync.erase(it_prev);
                }
                else
                {
                    it_prev++;
                }
            }
            else if (op == DEL_COMMAND)
            {
                if (removeInboundRoutingPost(key, ctxt))
                {
                    it_prev = consumer.m_toSync.erase(it_prev);
                }
                else
                {
                    it_prev++;
                }
            }
        }
    }
}

void DashRouteOrch::doTask(Consumer& consumer)
{
    SWSS_LOG_ENTER();

    const auto& tn = consumer.getTableName();

    SWSS_LOG_ERROR("Table name: %s", tn.c_str());

    if (tn == APP_DASH_ENI_TABLE_NAME)
    {
        doTaskEniTable(consumer);
    }
    else if (tn == APP_DASH_QOS_TABLE_NAME)
    {
        doTaskQosTable(consumer);
    }
    else if (tn == APP_DASH_ROUTE_TABLE_NAME)
    {
        doTaskRouteTable(consumer);
    }
    else if (tn == APP_DASH_ROUTE_RULE_TABLE_NAME)
    {
        doTaskRouteRuleTable(consumer);
    }
    else
    {
        SWSS_LOG_ERROR("Unknown table: %s", tn.c_str());
    }
}
