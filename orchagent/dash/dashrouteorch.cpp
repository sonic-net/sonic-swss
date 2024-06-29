#include <cassert>
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <exception>
#include <inttypes.h>
#include <algorithm>
#include <numeric>

#include "converter.h"
#include "dashrouteorch.h"
#include "macaddress.h"
#include "orch.h"
#include "sai.h"
#include "saiextensions.h"
#include "swssnet.h"
#include "tokenize.h"
#include "dashorch.h"
#include "crmorch.h"
#include "saihelper.h"

#include "taskworker.h"
#include "pbutils.h"
#include "dash_api/route_type.pb.h"

using namespace std;
using namespace swss;

extern std::unordered_map<std::string, sai_object_id_t> gVnetNameToId;
extern sai_dash_outbound_routing_api_t* sai_dash_outbound_routing_api;
extern sai_dash_inbound_routing_api_t* sai_dash_inbound_routing_api;
extern sai_object_id_t gSwitchId;
extern size_t gMaxBulkSize;
extern CrmOrch *gCrmOrch;

static std::unordered_map<dash::route_type::RoutingType, sai_outbound_routing_entry_action_t> sOutboundAction =
{
    { dash::route_type::RoutingType::ROUTING_TYPE_VNET, SAI_OUTBOUND_ROUTING_ENTRY_ACTION_ROUTE_VNET },
    { dash::route_type::RoutingType::ROUTING_TYPE_VNET_DIRECT, SAI_OUTBOUND_ROUTING_ENTRY_ACTION_ROUTE_VNET_DIRECT },
    { dash::route_type::RoutingType::ROUTING_TYPE_DIRECT, SAI_OUTBOUND_ROUTING_ENTRY_ACTION_ROUTE_DIRECT },
    { dash::route_type::RoutingType::ROUTING_TYPE_DROP, SAI_OUTBOUND_ROUTING_ENTRY_ACTION_DROP }
};

DashRouteOrch::DashRouteOrch(DBConnector *db, vector<string> &tableName, DashOrch *dash_orch, ZmqServer *zmqServer) :
    outbound_routing_bulker_(sai_dash_outbound_routing_api, gMaxBulkSize),
    inbound_routing_bulker_(sai_dash_inbound_routing_api, gMaxBulkSize),
    ZmqOrch(db, tableName, zmqServer),
    dash_orch_(dash_orch)
{
    SWSS_LOG_ENTER();
}

bool DashRouteOrch::addOutboundRouting(const string& key, OutboundRoutingBulkContext& ctxt)
{
    SWSS_LOG_ENTER();

    bool exists = (routing_entries_.find(key) != routing_entries_.end());
    if (exists)
    {
        SWSS_LOG_WARN("Outbound routing entry already exists for %s", key.c_str());
        return true;
    }
    if (!dash_orch_->getEni(ctxt.eni))
    {
        SWSS_LOG_INFO("Retry as ENI entry %s not found", ctxt.eni.c_str());
        return false;
    }
    if (ctxt.metadata.has_vnet() && gVnetNameToId.find(ctxt.metadata.vnet()) == gVnetNameToId.end())
    {
        SWSS_LOG_INFO("Retry as vnet %s not found", ctxt.metadata.vnet().c_str());
        return false;
    }

    sai_outbound_routing_entry_t outbound_routing_entry;
    outbound_routing_entry.switch_id = gSwitchId;
    outbound_routing_entry.eni_id = dash_orch_->getEni(ctxt.eni)->eni_id;
    swss::copy(outbound_routing_entry.destination, ctxt.destination);
    sai_attribute_t outbound_routing_attr;
    vector<sai_attribute_t> outbound_routing_attrs;
    auto& object_statuses = ctxt.object_statuses;

    outbound_routing_attr.id = SAI_OUTBOUND_ROUTING_ENTRY_ATTR_ACTION;
    outbound_routing_attr.value.u32 = sOutboundAction[ctxt.metadata.action_type()];
    outbound_routing_attrs.push_back(outbound_routing_attr);

    if (ctxt.metadata.action_type() == dash::route_type::RoutingType::ROUTING_TYPE_DIRECT)
    {
        // Intentional empty line, To direct action type, don't need set extra attributes
    }
    else if (ctxt.metadata.action_type() == dash::route_type::RoutingType::ROUTING_TYPE_VNET
        && ctxt.metadata.has_vnet()
        && !ctxt.metadata.vnet().empty())
    {   
        outbound_routing_attr.id = SAI_OUTBOUND_ROUTING_ENTRY_ATTR_DST_VNET_ID;
        outbound_routing_attr.value.oid = gVnetNameToId[ctxt.metadata.vnet()];
        outbound_routing_attrs.push_back(outbound_routing_attr);
    }
    else if (ctxt.metadata.action_type() == dash::route_type::RoutingType::ROUTING_TYPE_VNET_DIRECT
        && ctxt.metadata.has_vnet_direct()
        && !ctxt.metadata.vnet_direct().vnet().empty()
        && (ctxt.metadata.vnet_direct().overlay_ip().has_ipv4() || ctxt.metadata.vnet_direct().overlay_ip().has_ipv6()))
    {
        outbound_routing_attr.id = SAI_OUTBOUND_ROUTING_ENTRY_ATTR_DST_VNET_ID;
        outbound_routing_attr.value.oid = gVnetNameToId[ctxt.metadata.vnet_direct().vnet()];
        outbound_routing_attrs.push_back(outbound_routing_attr);

        outbound_routing_attr.id = SAI_OUTBOUND_ROUTING_ENTRY_ATTR_OVERLAY_IP;
        if (!to_sai(ctxt.metadata.vnet_direct().overlay_ip(), outbound_routing_attr.value.ipaddr))
        {
            return false;
        }
        outbound_routing_attrs.push_back(outbound_routing_attr);
    }
    else
    {
        SWSS_LOG_WARN("Attribute action for outbound routing entry %s", key.c_str());
        return false;
    }

    object_statuses.emplace_back();
    outbound_routing_bulker_.create_entry(&object_statuses.back(), &outbound_routing_entry,
                                            (uint32_t)outbound_routing_attrs.size(), outbound_routing_attrs.data());

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

    OutboundRoutingEntry entry = { dash_orch_->getEni(ctxt.eni)->eni_id, ctxt.destination, ctxt.metadata };
    routing_entries_[key] = entry;

    gCrmOrch->incCrmResUsedCounter(ctxt.destination.isV4() ? CrmResourceType::CRM_DASH_IPV4_OUTBOUND_ROUTING : CrmResourceType::CRM_DASH_IPV6_OUTBOUND_ROUTING);

    SWSS_LOG_INFO("Outbound routing entry for %s added", key.c_str());

    return true;
}

bool DashRouteOrch::removeOutboundRouting(const string& key, OutboundRoutingBulkContext& ctxt)
{
    SWSS_LOG_ENTER();

    bool exists = (routing_entries_.find(key) != routing_entries_.end());
    if (!exists)
    {
        SWSS_LOG_INFO("Failed to find outbound routing entry %s to remove", key.c_str());
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

    gCrmOrch->decCrmResUsedCounter(ctxt.destination.isV4() ? CrmResourceType::CRM_DASH_IPV4_OUTBOUND_ROUTING : CrmResourceType::CRM_DASH_IPV6_OUTBOUND_ROUTING);

    routing_entries_.erase(key);
    SWSS_LOG_INFO("Outbound routing entry for %s removed", key.c_str());

    return true;
}

void DashRouteOrch::doTaskRouteTable(ConsumerBase& consumer)
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

            string& eni = ctxt.eni;
            IpPrefix& destination = ctxt.destination;

            vector<string> keys = tokenize(key, ':');
            eni = keys[0];
            string ip_str;
            size_t pos = key.find(":", eni.length());
            ip_str = key.substr(pos + 1);
            destination = IpPrefix(ip_str);

            if (op == SET_COMMAND)
            {
                if (!parsePbMessage(kfvFieldsValues(tuple), ctxt.metadata))
                {
                    SWSS_LOG_WARN("Requires protobuff at OutboundRouting :%s", key.c_str());
                    it = consumer.m_toSync.erase(it);
                    continue;
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
        SWSS_LOG_WARN("Inbound routing entry already exists for %s", key.c_str());
        return true;
    }
    if (!dash_orch_->getEni(ctxt.eni))
    {
        SWSS_LOG_INFO("Retry as ENI entry %s not found", ctxt.eni.c_str());
        return false;
    }
    if (ctxt.metadata.has_vnet() && gVnetNameToId.find(ctxt.metadata.vnet()) == gVnetNameToId.end())
    {
        SWSS_LOG_INFO("Retry as vnet %s not found", ctxt.metadata.vnet().c_str());
        return false;
    }

    sai_inbound_routing_entry_t inbound_routing_entry;

    inbound_routing_entry.switch_id = gSwitchId;
    inbound_routing_entry.eni_id = dash_orch_->getEni(ctxt.eni)->eni_id;
    inbound_routing_entry.vni = ctxt.vni;
    swss::copy(inbound_routing_entry.sip, ctxt.sip);
    swss::copy(inbound_routing_entry.sip_mask, ctxt.sip_mask);
    inbound_routing_entry.priority = ctxt.metadata.priority();
    auto& object_statuses = ctxt.object_statuses;

    sai_attribute_t inbound_routing_attr;
    vector<sai_attribute_t> inbound_routing_attrs;

    inbound_routing_attr.id = SAI_INBOUND_ROUTING_ENTRY_ATTR_ACTION;
    inbound_routing_attr.value.u32 = ctxt.metadata.pa_validation() ? SAI_INBOUND_ROUTING_ENTRY_ACTION_TUNNEL_DECAP_PA_VALIDATE : SAI_INBOUND_ROUTING_ENTRY_ACTION_TUNNEL_DECAP;
    inbound_routing_attrs.push_back(inbound_routing_attr);

    if (ctxt.metadata.has_vnet())
    {
        inbound_routing_attr.id = SAI_INBOUND_ROUTING_ENTRY_ATTR_SRC_VNET_ID;
        inbound_routing_attr.value.oid = gVnetNameToId[ctxt.metadata.vnet()];
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

    InboundRoutingEntry entry = { dash_orch_->getEni(ctxt.eni)->eni_id, ctxt.vni, ctxt.sip, ctxt.sip_mask, ctxt.metadata };
    routing_rule_entries_[key] = entry;

    gCrmOrch->incCrmResUsedCounter(ctxt.sip.isV4() ? CrmResourceType::CRM_DASH_IPV4_INBOUND_ROUTING : CrmResourceType::CRM_DASH_IPV6_INBOUND_ROUTING);

    SWSS_LOG_INFO("Inbound routing entry for %s added", key.c_str());

    return true;
}

bool DashRouteOrch::removeInboundRouting(const string& key, InboundRoutingBulkContext& ctxt)
{
    SWSS_LOG_ENTER();

    bool exists = (routing_rule_entries_.find(key) != routing_rule_entries_.end());
    if (!exists)
    {
        SWSS_LOG_INFO("Failed to find inbound routing entry %s to remove", key.c_str());
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
    inbound_routing_entry.priority = entry.metadata.priority();
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

    gCrmOrch->decCrmResUsedCounter(ctxt.sip.isV4() ? CrmResourceType::CRM_DASH_IPV4_INBOUND_ROUTING : CrmResourceType::CRM_DASH_IPV6_INBOUND_ROUTING);

    routing_rule_entries_.erase(key);
    SWSS_LOG_INFO("Inbound routing entry for %s removed", key.c_str());

    return true;
}

void DashRouteOrch::doTaskRouteRuleTable(ConsumerBase& consumer)
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
            IpAddress& sip = ctxt.sip;
            IpAddress& sip_mask = ctxt.sip_mask;
            IpPrefix prefix;

            vector<string> keys = tokenize(key, ':');
            eni = keys[0];
            vni = to_uint<uint32_t>(keys[1]);
            string ip_str;
            size_t pos = key.find(":", keys[0].length() + keys[1].length() + 1);
            ip_str = key.substr(pos + 1);
            prefix = IpPrefix(ip_str);

            sip = prefix.getIp();
            sip_mask = prefix.getMask();

            if (op == SET_COMMAND)
            {
                if (!parsePbMessage(kfvFieldsValues(tuple), ctxt.metadata))
                {
                    SWSS_LOG_WARN("Requires protobuff at InboundRouting :%s", key.c_str());
                    it = consumer.m_toSync.erase(it);
                    continue;
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

void DashRouteOrch::doTask(ConsumerBase& consumer)
{
    SWSS_LOG_ENTER();

    const auto& tn = consumer.getTableName();

    SWSS_LOG_INFO("Table name: %s", tn.c_str());

    if (tn == APP_DASH_ROUTE_TABLE_NAME)
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
