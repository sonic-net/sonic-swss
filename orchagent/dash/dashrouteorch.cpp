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
#include "dashtunnelorch.h"

#include "taskworker.h"
#include "pbutils.h"
#include "dash_api/route_type.pb.h"
#include "directory.h"

using namespace std;
using namespace swss;

extern std::unordered_map<std::string, sai_object_id_t> gVnetNameToId;
extern sai_dash_outbound_routing_api_t* sai_dash_outbound_routing_api;
extern sai_dash_inbound_routing_api_t* sai_dash_inbound_routing_api;
extern sai_object_id_t gSwitchId;
extern size_t gMaxBulkSize;
extern CrmOrch *gCrmOrch;
extern Directory<Orch*> gDirectory;

static std::unordered_map<dash::route_type::RoutingType, sai_outbound_routing_entry_action_t> sOutboundAction =
{
    { dash::route_type::RoutingType::ROUTING_TYPE_VNET, SAI_OUTBOUND_ROUTING_ENTRY_ACTION_ROUTE_VNET },
    { dash::route_type::RoutingType::ROUTING_TYPE_VNET_DIRECT, SAI_OUTBOUND_ROUTING_ENTRY_ACTION_ROUTE_VNET_DIRECT },
    { dash::route_type::RoutingType::ROUTING_TYPE_DIRECT, SAI_OUTBOUND_ROUTING_ENTRY_ACTION_ROUTE_DIRECT },
    { dash::route_type::RoutingType::ROUTING_TYPE_DROP, SAI_OUTBOUND_ROUTING_ENTRY_ACTION_DROP }
};

DashRouteOrch::DashRouteOrch(DBConnector *db, vector<string> &tableName, DashOrch *dash_orch, DBConnector *app_state_db, ZmqServer *zmqServer) :
    outbound_routing_bulker_(sai_dash_outbound_routing_api, gMaxBulkSize),
    inbound_routing_bulker_(sai_dash_inbound_routing_api, gMaxBulkSize),
    ZmqOrch(db, tableName, zmqServer),
    dash_orch_(dash_orch)
{
    SWSS_LOG_ENTER();
    dash_route_result_table_ = make_unique<Table>(app_state_db, APP_DASH_ROUTE_TABLE_NAME);
    dash_route_rule_result_table_ = make_unique<Table>(app_state_db, APP_DASH_ROUTE_RULE_TABLE_NAME);
    dash_route_group_result_table_ = make_unique<Table>(app_state_db, APP_DASH_ROUTE_GROUP_TABLE_NAME);
}

bool DashRouteOrch::addOutboundRouting(const string& key, OutboundRoutingBulkContext& ctxt)
{
    SWSS_LOG_ENTER();

    if (isRouteGroupBound(ctxt.route_group))
    {
        SWSS_LOG_WARN("Cannot add new route to route group %s as it is already bound", ctxt.route_group.c_str());
        return true;
    }
    sai_object_id_t route_group_oid = this->getRouteGroupOid(ctxt.route_group);
    if (route_group_oid == SAI_NULL_OBJECT_ID)
    {
        SWSS_LOG_INFO("Retry as route group %s not found", ctxt.route_group.c_str());
        return false;
    }

    std::string routing_type_str = dash::route_type::RoutingType_Name(ctxt.metadata.routing_type());
    if (ctxt.metadata.routing_type() == dash::route_type::RoutingType::ROUTING_TYPE_VNET &&
        ctxt.metadata.has_vnet() && gVnetNameToId.find(ctxt.metadata.vnet()) == gVnetNameToId.end())
    {
        SWSS_LOG_INFO("Retry as vnet %s not found for routing type %s",
                      ctxt.metadata.vnet().c_str(),
                      routing_type_str.c_str());
        return false;
    }
    if (ctxt.metadata.routing_type() == dash::route_type::RoutingType::ROUTING_TYPE_VNET_DIRECT &&
        ctxt.metadata.has_vnet_direct() && gVnetNameToId.find(ctxt.metadata.vnet_direct().vnet()) == gVnetNameToId.end())
    {
        SWSS_LOG_INFO("Retry as vnet %s not found for routing type %s",
                      ctxt.metadata.vnet_direct().vnet().c_str(),
                      routing_type_str.c_str());
        return false;
    }

    sai_outbound_routing_entry_t outbound_routing_entry;
    outbound_routing_entry.switch_id = gSwitchId;
    outbound_routing_entry.outbound_routing_group_id = route_group_oid;
    swss::copy(outbound_routing_entry.destination, ctxt.destination);
    sai_attribute_t outbound_routing_attr;
    vector<sai_attribute_t> outbound_routing_attrs;
    auto& object_statuses = ctxt.object_statuses;

    auto it = sOutboundAction.find(ctxt.metadata.routing_type());
    if (it == sOutboundAction.end())
    {
        SWSS_LOG_WARN("Routing type %s for outbound routing entry %s not allowed", routing_type_str.c_str(), key.c_str());
        return false;
    }

    outbound_routing_attr.id = SAI_OUTBOUND_ROUTING_ENTRY_ATTR_ACTION;
    outbound_routing_attr.value.u32 = it->second;
    outbound_routing_attrs.push_back(outbound_routing_attr);

    if (ctxt.metadata.routing_type() == dash::route_type::RoutingType::ROUTING_TYPE_DIRECT)
    {
        // Intentional empty line, for direct routing, don't need set extra attributes
    }
    else if (ctxt.metadata.routing_type() == dash::route_type::RoutingType::ROUTING_TYPE_VNET
        && ctxt.metadata.has_vnet()
        && !ctxt.metadata.vnet().empty())
    {   
        outbound_routing_attr.id = SAI_OUTBOUND_ROUTING_ENTRY_ATTR_DST_VNET_ID;
        outbound_routing_attr.value.oid = gVnetNameToId[ctxt.metadata.vnet()];
        outbound_routing_attrs.push_back(outbound_routing_attr);
    }
    else if (ctxt.metadata.routing_type() == dash::route_type::RoutingType::ROUTING_TYPE_VNET_DIRECT
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
        SWSS_LOG_WARN("Routing type %s for outbound routing entry %s either invalid or missing required attributes",
                        dash::route_type::RoutingType_Name(ctxt.metadata.routing_type()).c_str(), key.c_str());
        return false;
    }

    if (ctxt.metadata.has_underlay_sip() && ctxt.metadata.underlay_sip().has_ipv4())
    {
        outbound_routing_attr.id = SAI_OUTBOUND_ROUTING_ENTRY_ATTR_UNDERLAY_SIP;
        if (!to_sai(ctxt.metadata.underlay_sip(), outbound_routing_attr.value.ipaddr))
        {
            return false;
        }
        outbound_routing_attrs.push_back(outbound_routing_attr);
    }

    if (ctxt.metadata.has_metering_class_or()) {
        outbound_routing_attr.id = SAI_OUTBOUND_ROUTING_ENTRY_ATTR_METER_CLASS_OR;
        outbound_routing_attr.value.u32 = ctxt.metadata.metering_class_or();
        outbound_routing_attrs.push_back(outbound_routing_attr);
    }

    if (ctxt.metadata.has_metering_class_and()) {
        outbound_routing_attr.id = SAI_OUTBOUND_ROUTING_ENTRY_ATTR_METER_CLASS_AND;
        outbound_routing_attr.value.u32 = ctxt.metadata.metering_class_and();
        outbound_routing_attrs.push_back(outbound_routing_attr);
    }

    if (ctxt.metadata.has_tunnel())
    {
        auto dash_tunnel_orch = gDirectory.get<DashTunnelOrch*>();
        sai_object_id_t tunnel_oid = dash_tunnel_orch->getTunnelOid(ctxt.metadata.tunnel());
        if (tunnel_oid == SAI_NULL_OBJECT_ID)
        {
            SWSS_LOG_INFO("Retry as tunnel %s not found", ctxt.metadata.tunnel().c_str());
            return false;
        }
        outbound_routing_attr.id = SAI_OUTBOUND_ROUTING_ENTRY_ATTR_DASH_TUNNEL_ID;
        outbound_routing_attr.value.oid = tunnel_oid;
        outbound_routing_attrs.push_back(outbound_routing_attr);
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

    gCrmOrch->incCrmResUsedCounter(ctxt.destination.isV4() ? CrmResourceType::CRM_DASH_IPV4_OUTBOUND_ROUTING : CrmResourceType::CRM_DASH_IPV6_OUTBOUND_ROUTING);

    SWSS_LOG_INFO("Outbound routing entry for %s added", key.c_str());

    return true;
}

bool DashRouteOrch::removeOutboundRouting(const string& route_group, const IpPrefix& destination, OutboundRoutingBulkContext& ctxt)
{
    SWSS_LOG_ENTER();

    if (isRouteGroupBound(ctxt.route_group))
    {
        SWSS_LOG_WARN("Cannot remove route from route group %s as it is already bound", ctxt.route_group.c_str());
        return false;
    }

    auto& object_statuses = ctxt.object_statuses;
    sai_outbound_routing_entry_t outbound_routing_entry;
    outbound_routing_entry.switch_id = gSwitchId;
    outbound_routing_entry.outbound_routing_group_id = route_group_oid_map_[route_group];
    swss::copy(outbound_routing_entry.destination, destination);
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

    SWSS_LOG_INFO("Outbound routing entry for %s removed", key.c_str());

    return true;
}

void DashRouteOrch::doTaskRouteTable(ConsumerBase& consumer)
{
    SWSS_LOG_ENTER();

    auto it = consumer.m_toSync.begin();
    uint32_t result;
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
            result = DASH_RESULT_SUCCESS;

            if (!inserted)
            {
                ctxt.clear();
            }

            string& route_group = ctxt.route_group;
            IpPrefix& destination = ctxt.destination;

            vector<string> keys = tokenize(key, ':');
            route_group = keys[0];
            string ip_str;
            size_t pos = key.find(":", route_group.length());
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
                if (ctxt.metadata.routing_type() == dash::route_type::RoutingType::ROUTING_TYPE_UNSPECIFIED)
                {
                    // Route::action_type is deprecated in favor of Route::routing_type. For messages still using the old action_type field,
                    // copy it to the new routing_type field. All subsequent operations will use the new field.
                    #pragma GCC diagnostic push
                    #pragma GCC diagnostic ignored "-Wdeprecated-declarations"
                    ctxt.metadata.set_routing_type(ctxt.metadata.action_type());
                    #pragma GCC diagnostic pop
                }
                if (addOutboundRouting(key, ctxt))
                {
                    it = consumer.m_toSync.erase(it);
                    /*
                     * Write result only when removing from consumer in pre-op
                     * For other cases, this will be handled in post-op
                     */
                    writeResultToDB(dash_route_result_table_, key, result);
                }
                else
                {
                    it++;
                }
            }
            else if (op == DEL_COMMAND)
            {
                if (removeOutboundRouting(route_group, destination, ctxt))
                {
                    it = consumer.m_toSync.erase(it);
                    removeResultFromDB(dash_route_result_table_, key);
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
            result = DASH_RESULT_SUCCESS;
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
                    result = DASH_RESULT_FAILURE;
                }
                writeResultToDB(dash_route_result_table_, key, result);
            }
            else if (op == DEL_COMMAND)
            {
                if (removeOutboundRoutingPost(key, ctxt))
                {
                    it_prev = consumer.m_toSync.erase(it_prev);
                    removeResultFromDB(dash_route_result_table_, key);
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

    if (ctxt.metadata.has_metering_class_or()) {
        inbound_routing_attr.id = SAI_INBOUND_ROUTING_ENTRY_ATTR_METER_CLASS_OR;
        inbound_routing_attr.value.u32 = ctxt.metadata.metering_class_or();
        inbound_routing_attrs.push_back(inbound_routing_attr);
    }

    if (ctxt.metadata.has_metering_class_and()) {
        inbound_routing_attr.id = SAI_INBOUND_ROUTING_ENTRY_ATTR_METER_CLASS_AND;
        inbound_routing_attr.value.u32 = ctxt.metadata.metering_class_and();
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

    gCrmOrch->incCrmResUsedCounter(ctxt.sip.isV4() ? CrmResourceType::CRM_DASH_IPV4_INBOUND_ROUTING : CrmResourceType::CRM_DASH_IPV6_INBOUND_ROUTING);

    SWSS_LOG_INFO("Inbound routing entry for %s added", key.c_str());

    return true;
}

bool DashRouteOrch::removeInboundRouting(const string& key, InboundRoutingBulkContext& ctxt)
{
    SWSS_LOG_ENTER();

    auto& object_statuses = ctxt.object_statuses;
    sai_inbound_routing_entry_t inbound_routing_entry;
    inbound_routing_entry.switch_id = gSwitchId;
    inbound_routing_entry.eni_id = dash_orch_->getEni(ctxt.eni)->eni_id;
    inbound_routing_entry.vni = ctxt.vni;
    swss::copy(inbound_routing_entry.sip, ctxt.sip);
    swss::copy(inbound_routing_entry.sip_mask, ctxt.sip_mask);
    inbound_routing_entry.priority = ctxt.metadata.priority();
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

    SWSS_LOG_INFO("Inbound routing entry for %s removed", key.c_str());

    return true;
}

void DashRouteOrch::doTaskRouteRuleTable(ConsumerBase& consumer)
{
    SWSS_LOG_ENTER();

    auto it = consumer.m_toSync.begin();
    uint32_t result;
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
            result = DASH_RESULT_SUCCESS;

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
                    /*
                     * Write result only when removing from consumer in pre-op
                     * For other cases, this will be handled in post-op
                     */
                    writeResultToDB(dash_route_rule_result_table_, key, result);
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
                    removeResultFromDB(dash_route_rule_result_table_, key);
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
            result = DASH_RESULT_SUCCESS;
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
                    result = DASH_RESULT_FAILURE;
                    it_prev++;
                }
                writeResultToDB(dash_route_rule_result_table_, key, result);
            }
            else if (op == DEL_COMMAND)
            {
                if (removeInboundRoutingPost(key, ctxt))
                {
                    it_prev = consumer.m_toSync.erase(it_prev);
                    removeResultFromDB(dash_route_rule_result_table_, key);
                }
                else
                {
                    it_prev++;
                }
            }
        }
    }
}

bool DashRouteOrch::addRouteGroup(const string& route_group, const dash::route_group::RouteGroup& entry)
{
    SWSS_LOG_ENTER();

    sai_object_id_t route_group_oid = this->getRouteGroupOid(route_group);
    if (route_group_oid != SAI_NULL_OBJECT_ID)
    {
        SWSS_LOG_WARN("Route group %s already exists", route_group.c_str());
        return true;
    }

    sai_status_t status = sai_dash_outbound_routing_api->create_outbound_routing_group(&route_group_oid, gSwitchId, 0, NULL);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to create route group %s", route_group.c_str());
        task_process_status handle_status = handleSaiCreateStatus((sai_api_t) SAI_API_DASH_OUTBOUND_ROUTING, status);
        if (handle_status != task_success)
        {
            return parseHandleSaiStatusFailure(handle_status);
        }
    }

    route_group_oid_map_[route_group] = route_group_oid;
    SWSS_LOG_INFO("Route group %s added", route_group.c_str());

    return true;
}

bool DashRouteOrch::removeRouteGroup(const string& route_group)
{
    SWSS_LOG_ENTER();

    if (isRouteGroupBound(route_group))
    {
        SWSS_LOG_WARN("Cannot remove bound route group %s", route_group.c_str());
        return false;
    }

    sai_object_id_t route_group_oid = this->getRouteGroupOid(route_group);
    if (route_group_oid == SAI_NULL_OBJECT_ID)
    {
        SWSS_LOG_INFO("Failed to find route group %s to remove", route_group.c_str());
        return true;
    }

    sai_status_t status = sai_dash_outbound_routing_api->remove_outbound_routing_group(route_group_oid);
    if (status != SAI_STATUS_SUCCESS)
    {
        //Retry later if object is in use
        if (status == SAI_STATUS_OBJECT_IN_USE)
        {
            return false;
        }
        SWSS_LOG_ERROR("Failed to remove route group %s", route_group.c_str());
        task_process_status handle_status = handleSaiRemoveStatus((sai_api_t) SAI_API_DASH_OUTBOUND_ROUTING, status);
        if (handle_status != task_success)
        {
            return parseHandleSaiStatusFailure(handle_status);
        }
    }

    route_group_oid_map_.erase(route_group);
    SWSS_LOG_INFO("Route group %s removed", route_group.c_str());

    return true;
}

sai_object_id_t DashRouteOrch::getRouteGroupOid(const string& route_group) const
{
    SWSS_LOG_ENTER();

    auto it = route_group_oid_map_.find(route_group);
    if (it == route_group_oid_map_.end())
    {
        return SAI_NULL_OBJECT_ID;
    }

    return it->second;
}

void DashRouteOrch::bindRouteGroup(const std::string& route_group)
{
    auto it = route_group_bind_count_.find(route_group);

    if (it == route_group_bind_count_.end())
    {
        route_group_bind_count_[route_group] = 1;
        return;
    }
    it->second++;
}

void DashRouteOrch::unbindRouteGroup(const std::string& route_group)
{
    auto it = route_group_bind_count_.find(route_group);

    if (it == route_group_bind_count_.end())
    {
        SWSS_LOG_WARN("Cannot unbind route group %s since it is not bound to any ENIs", route_group.c_str());
        return;
    }
    it->second--;

    if (it->second == 0)
    {
        SWSS_LOG_INFO("Route group %s completely unbound", route_group.c_str());
        route_group_bind_count_.erase(it);
    }
}

bool DashRouteOrch::isRouteGroupBound(const std::string& route_group) const
{
    auto it = route_group_bind_count_.find(route_group);
    if (it == route_group_bind_count_.end())
    {
        return false;
    }
    return it->second > 0;
}

void DashRouteOrch::doTaskRouteGroupTable(ConsumerBase& consumer)
{
    SWSS_LOG_ENTER();

    auto it = consumer.m_toSync.begin();
    uint32_t result;
    while (it != consumer.m_toSync.end())
    {
        auto t = it->second;
        string route_group = kfvKey(t);
        string op = kfvOp(t);
        result = DASH_RESULT_SUCCESS;
        if (op == SET_COMMAND)
        {
            dash::route_group::RouteGroup entry;
            if (!parsePbMessage(kfvFieldsValues(t), entry))
            {
                SWSS_LOG_WARN("Requires protobuf at RouteGroup :%s", route_group.c_str());
                it = consumer.m_toSync.erase(it);
                continue;
            }

            if (addRouteGroup(route_group, entry))
            {
                it = consumer.m_toSync.erase(it);
            }
            else
            {
                result = DASH_RESULT_FAILURE;
                it++;
            }
            writeResultToDB(dash_route_group_result_table_, route_group, result, entry.version());
        }
        else if (op == DEL_COMMAND)
        {
            if (removeRouteGroup(route_group))
            {
                it = consumer.m_toSync.erase(it);
                removeResultFromDB(dash_route_group_result_table_, route_group);
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
    else if (tn == APP_DASH_ROUTE_GROUP_TABLE_NAME)
    {
        doTaskRouteGroupTable(consumer);
    }
    else
    {
        SWSS_LOG_ERROR("Unknown table: %s", tn.c_str());
    }
}
