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
#include "dashvnetorch.h"
#include "ipaddress.h"
#include "macaddress.h"
#include "orch.h"
#include "sai.h"
#include "saiextensions.h"
#include "swssnet.h"
#include "tokenize.h"
#include "dashorch.h"
#include "crmorch.h"
#include "saihelper.h"
#include "directory.h"
#include "dashtunnelorch.h"

#include "taskworker.h"
#include "pbutils.h"

using namespace std;
using namespace swss;

std::unordered_map<std::string, sai_object_id_t> gVnetNameToId;
extern sai_dash_vnet_api_t* sai_dash_vnet_api;
extern sai_dash_outbound_ca_to_pa_api_t* sai_dash_outbound_ca_to_pa_api;
extern sai_dash_pa_validation_api_t* sai_dash_pa_validation_api;
extern sai_object_id_t gSwitchId;
extern size_t gMaxBulkSize;
extern CrmOrch *gCrmOrch;
extern Directory<Orch*> gDirectory;

DashVnetOrch::DashVnetOrch(DBConnector *db, vector<string> &tables, DBConnector *app_state_db, ZmqServer *zmqServer) :
    vnet_bulker_(sai_dash_vnet_api, gSwitchId, gMaxBulkSize),
    outbound_ca_to_pa_bulker_(sai_dash_outbound_ca_to_pa_api, gMaxBulkSize),
    pa_validation_bulker_(sai_dash_pa_validation_api, gMaxBulkSize),
    ZmqOrch(db, tables, zmqServer)
{
    SWSS_LOG_ENTER();
    dash_vnet_result_table_ = make_unique<Table>(app_state_db, APP_DASH_VNET_TABLE_NAME);
    dash_vnet_map_result_table_ = make_unique<Table>(app_state_db, APP_DASH_VNET_MAPPING_TABLE_NAME);
}

bool DashVnetOrch::addVnet(const string& vnet_name, DashVnetBulkContext& ctxt)
{
    SWSS_LOG_ENTER();

    bool exists = (vnet_table_.find(vnet_name) != vnet_table_.end());
    if (exists)
    {
        SWSS_LOG_WARN("Vnet already exists for %s", vnet_name.c_str());
        return true;
    }
    DashOrch* dash_orch = gDirectory.get<DashOrch*>();
    if (!dash_orch->hasApplianceEntry())
    {
        SWSS_LOG_INFO("Retry as no appliance table entry found");
        return false;
    }

    uint32_t attr_count = 1;
    auto& object_ids = ctxt.object_ids;
    sai_attribute_t dash_vnet_attr;
    dash_vnet_attr.id = SAI_VNET_ATTR_VNI;
    dash_vnet_attr.value.u32 = ctxt.metadata.vni();
    object_ids.emplace_back();
    vnet_bulker_.create_entry(&object_ids.back(), attr_count, &dash_vnet_attr);

    return false;
}

bool DashVnetOrch::addVnetPost(const string& vnet_name, const DashVnetBulkContext& ctxt)
{
    SWSS_LOG_ENTER();

    const auto& object_ids = ctxt.object_ids;
    if (object_ids.empty())
    {
        return false;
    }

    auto it_id = object_ids.begin();
    sai_object_id_t id = *it_id++;
    if (id == SAI_NULL_OBJECT_ID)
    {
        SWSS_LOG_ERROR("Failed to create vnet entry for %s", vnet_name.c_str());
        return false;
    }

    VnetEntry entry = { id, ctxt.metadata, std::set<std::string>() };
    vnet_table_[vnet_name] = entry;
    gVnetNameToId[vnet_name] = id;

    gCrmOrch->incCrmResUsedCounter(CrmResourceType::CRM_DASH_VNET);

    SWSS_LOG_INFO("Vnet entry added for %s", vnet_name.c_str());

    return true;
}

bool DashVnetOrch::removeVnet(const string& vnet_name, DashVnetBulkContext& ctxt)
{
    SWSS_LOG_ENTER();

    bool exists = (vnet_table_.find(vnet_name) != vnet_table_.end());
    if (!exists)
    {
        SWSS_LOG_WARN("Failed to find vnet entry %s to remove", vnet_name.c_str());
        return true;
    }

    auto& object_statuses = ctxt.vnet_statuses;
    sai_object_id_t vni;
    VnetEntry entry = vnet_table_[vnet_name];
    vni = entry.vni;
    object_statuses.emplace_back();
    vnet_bulker_.remove_entry(&object_statuses.back(), vni);

    removePaValidation(vnet_name, ctxt);
    return false;
}

bool DashVnetOrch::removeVnetPost(const string& vnet_name, const DashVnetBulkContext& ctxt)
{
    SWSS_LOG_ENTER();
    if (!removePaValidationPost(vnet_name, ctxt))
    {
        return false;
    }

    const auto& object_statuses = ctxt.vnet_statuses;

    if (object_statuses.empty())
    {
        return false;
    }

    auto it_status = object_statuses.begin();
    sai_status_t status = *it_status++;
    if (status != SAI_STATUS_SUCCESS)
    {
        // Retry later if object has non-zero reference to it
        if (status == SAI_STATUS_NOT_EXECUTED)
        {
            return false;
        }
        SWSS_LOG_ERROR("Failed to remove vnet entry for %s", vnet_name.c_str());
        task_process_status handle_status = handleSaiRemoveStatus((sai_api_t) SAI_API_DASH_VNET, status);
        if (handle_status != task_success)
        {
            return parseHandleSaiStatusFailure(handle_status);
        }
    }

    gCrmOrch->decCrmResUsedCounter(CrmResourceType::CRM_DASH_VNET);

    vnet_table_.erase(vnet_name);
    gVnetNameToId.erase(vnet_name);
    SWSS_LOG_INFO("Vnet entry removed for %s", vnet_name.c_str());

    return true;
}

void DashVnetOrch::doTaskVnetTable(ConsumerBase& consumer)
{
    SWSS_LOG_ENTER();

    auto it = consumer.m_toSync.begin();
    uint32_t result;
    while (it != consumer.m_toSync.end())
    {
        // Map to store vnet bulk op results
        std::map<std::pair<std::string, std::string>,
            DashVnetBulkContext> toBulk;

        while (it != consumer.m_toSync.end())
        {
            KeyOpFieldsValuesTuple tuple = it->second;
            const string& key = kfvKey(tuple);
            auto op = kfvOp(tuple);
            auto rc = toBulk.emplace(std::piecewise_construct,
                    std::forward_as_tuple(key, op),
                    std::forward_as_tuple());
            bool inserted = rc.second;
            auto& vnet_ctxt = rc.first->second;
            result = DASH_RESULT_SUCCESS;

            if (!inserted)
            {
                vnet_ctxt.clear();
            }

            if (op == SET_COMMAND)
            {
                if (!parsePbMessage(kfvFieldsValues(tuple), vnet_ctxt.metadata))
                {
                    SWSS_LOG_WARN("Requires protobuff at Vnet :%s", key.c_str());
                    it = consumer.m_toSync.erase(it);
                    continue;
                }
                if (addVnet(key, vnet_ctxt))
                {
                    it = consumer.m_toSync.erase(it);
                    /*
                     * Write result only when removing from consumer in pre-op
                     * For other cases, this will be handled in post-op
                     */
                    writeResultToDB(dash_vnet_result_table_, key, result);
                }
                else
                {
                    it++;
                }
            }
            else if (op == DEL_COMMAND)
            {
                if (removeVnet(key, vnet_ctxt))
                {
                    it = consumer.m_toSync.erase(it);
                    removeResultFromDB(dash_vnet_result_table_, key);
                }
                else
                {
                    it++;
                }
            }
            else
            {
                SWSS_LOG_ERROR("Invalid command %s", op.c_str());
                it = consumer.m_toSync.erase(it);
            }
        }

        vnet_bulker_.flush();
        pa_validation_bulker_.flush();

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

            const auto& vnet_ctxt = found->second;
            const auto& vnet_statuses = vnet_ctxt.vnet_statuses;
            const auto& pa_validation_statuses = vnet_ctxt.pa_validation_statuses;

            if (vnet_statuses.empty() && pa_validation_statuses.empty())
            {
                it_prev++;
                continue;
            }

            if (op == SET_COMMAND)
            {
               if (addVnetPost(key, vnet_ctxt))
                {
                    it_prev = consumer.m_toSync.erase(it_prev);
                }
                else
                {
                    result = DASH_RESULT_FAILURE;
                    it_prev++;
                }
                writeResultToDB(dash_vnet_result_table_, key, result);
            }
            else if (op == DEL_COMMAND)
            {
               if (removeVnetPost(key, vnet_ctxt))
                {
                    it_prev = consumer.m_toSync.erase(it_prev);
                    removeResultFromDB(dash_vnet_result_table_, key);
                }
                else
                {
                    it_prev++;
                }
            }
        }
    }
}

bool DashVnetOrch::addOutboundCaToPa(const string& key, VnetMapBulkContext& ctxt)
{
    SWSS_LOG_ENTER();

    sai_outbound_ca_to_pa_entry_t outbound_ca_to_pa_entry;
    outbound_ca_to_pa_entry.dst_vnet_id = gVnetNameToId[ctxt.vnet_name];
    outbound_ca_to_pa_entry.switch_id = gSwitchId;
    swss::copy(outbound_ca_to_pa_entry.dip, ctxt.dip);
    auto& object_statuses = ctxt.outbound_ca_to_pa_object_statuses;
    sai_attribute_t outbound_ca_to_pa_attr;
    vector<sai_attribute_t> outbound_ca_to_pa_attrs;

    DashOrch* dash_orch = gDirectory.get<DashOrch*>();
    dash::route_type::RouteType route_type_actions;
    if (!dash_orch->getRouteTypeActions(ctxt.metadata.routing_type(), route_type_actions))
    {
        SWSS_LOG_INFO("Failed to get route type actions for %s", key.c_str());
        return false;
    }

    for (auto action: route_type_actions.items())
    {
        if (action.action_type() == dash::route_type::ACTION_TYPE_STATICENCAP)
        {
            outbound_ca_to_pa_attr.id = SAI_OUTBOUND_CA_TO_PA_ENTRY_ATTR_DASH_ENCAPSULATION;
            if (action.encap_type() == dash::route_type::ENCAP_TYPE_VXLAN)
            {
                outbound_ca_to_pa_attr.value.u32 = SAI_DASH_ENCAPSULATION_VXLAN;
            }
            else if (action.encap_type() == dash::route_type::ENCAP_TYPE_NVGRE)
            {
                outbound_ca_to_pa_attr.value.u32 = SAI_DASH_ENCAPSULATION_NVGRE;
            }
            else
            {
                SWSS_LOG_ERROR("Invalid encap type %d for %s", action.encap_type(), key.c_str());
                return true;
            }
            outbound_ca_to_pa_attrs.push_back(outbound_ca_to_pa_attr);

            outbound_ca_to_pa_attr.id = SAI_OUTBOUND_CA_TO_PA_ENTRY_ATTR_TUNNEL_KEY;
            outbound_ca_to_pa_attr.value.u32 = action.vni();
            outbound_ca_to_pa_attrs.push_back(outbound_ca_to_pa_attr);

            outbound_ca_to_pa_attr.id = SAI_OUTBOUND_CA_TO_PA_ENTRY_ATTR_UNDERLAY_DIP;
            to_sai(ctxt.metadata.underlay_ip(), outbound_ca_to_pa_attr.value.ipaddr);
            outbound_ca_to_pa_attrs.push_back(outbound_ca_to_pa_attr); 

        }
    }

    if (ctxt.metadata.has_tunnel())
    {
        auto tunnel_oid = gDirectory.get<DashTunnelOrch*>()->getTunnelOid(ctxt.metadata.tunnel());
        if (tunnel_oid == SAI_NULL_OBJECT_ID)
        {
            SWSS_LOG_INFO("Tunnel %s for VnetMap %s does not exist yet", ctxt.metadata.tunnel().c_str(), key.c_str());
            return false;
        }
        outbound_ca_to_pa_attr.id = SAI_OUTBOUND_CA_TO_PA_ENTRY_ATTR_DASH_TUNNEL_ID;
        outbound_ca_to_pa_attr.value.oid = tunnel_oid;
        outbound_ca_to_pa_attrs.push_back(outbound_ca_to_pa_attr);
    }

    if (ctxt.metadata.routing_type() == dash::route_type::ROUTING_TYPE_PRIVATELINK)
    {
        outbound_ca_to_pa_attr.id = SAI_OUTBOUND_CA_TO_PA_ENTRY_ATTR_ACTION;
        outbound_ca_to_pa_attr.value.u32 = SAI_OUTBOUND_CA_TO_PA_ENTRY_ACTION_SET_PRIVATE_LINK_MAPPING;
        outbound_ca_to_pa_attrs.push_back(outbound_ca_to_pa_attr);

        outbound_ca_to_pa_attr.id = SAI_OUTBOUND_CA_TO_PA_ENTRY_ATTR_OVERLAY_DIP;
        to_sai(ctxt.metadata.overlay_dip_prefix().ip(), outbound_ca_to_pa_attr.value.ipaddr);
        outbound_ca_to_pa_attrs.push_back(outbound_ca_to_pa_attr);

        outbound_ca_to_pa_attr.id = SAI_OUTBOUND_CA_TO_PA_ENTRY_ATTR_OVERLAY_DIP_MASK;
        to_sai(ctxt.metadata.overlay_dip_prefix().mask(), outbound_ca_to_pa_attr.value.ipaddr);
        outbound_ca_to_pa_attrs.push_back(outbound_ca_to_pa_attr);


        outbound_ca_to_pa_attr.id = SAI_OUTBOUND_CA_TO_PA_ENTRY_ATTR_OVERLAY_SIP;
        to_sai(ctxt.metadata.overlay_sip_prefix().ip(), outbound_ca_to_pa_attr.value.ipaddr);
        outbound_ca_to_pa_attrs.push_back(outbound_ca_to_pa_attr);

        outbound_ca_to_pa_attr.id = SAI_OUTBOUND_CA_TO_PA_ENTRY_ATTR_OVERLAY_SIP_MASK;
        to_sai(ctxt.metadata.overlay_sip_prefix().mask(), outbound_ca_to_pa_attr.value.ipaddr);
        outbound_ca_to_pa_attrs.push_back(outbound_ca_to_pa_attr);
    }

    if (ctxt.metadata.has_mac_address())
    {
        outbound_ca_to_pa_attr.id = SAI_OUTBOUND_CA_TO_PA_ENTRY_ATTR_OVERLAY_DMAC;
        memcpy(outbound_ca_to_pa_attr.value.mac, ctxt.metadata.mac_address().c_str(), sizeof(sai_mac_t));
        outbound_ca_to_pa_attrs.push_back(outbound_ca_to_pa_attr);
    }

    if (ctxt.metadata.has_use_dst_vni())
    {
        outbound_ca_to_pa_attr.id = SAI_OUTBOUND_CA_TO_PA_ENTRY_ATTR_USE_DST_VNET_VNI;
        outbound_ca_to_pa_attr.value.booldata = ctxt.metadata.use_dst_vni();
        outbound_ca_to_pa_attrs.push_back(outbound_ca_to_pa_attr);
    }

    object_statuses.emplace_back();
    outbound_ca_to_pa_bulker_.create_entry(&object_statuses.back(), &outbound_ca_to_pa_entry,
            (uint32_t)outbound_ca_to_pa_attrs.size(), outbound_ca_to_pa_attrs.data());

    addPaValidation(key, ctxt);
    return false;
}

void DashVnetOrch::addPaValidation(const string& key, VnetMapBulkContext& ctxt)
{
    SWSS_LOG_ENTER();

    auto& object_statuses = ctxt.pa_validation_object_statuses;
    string underlay_ip_str = to_string(ctxt.metadata.underlay_ip());
    string pa_ref_key = ctxt.vnet_name + ":" + underlay_ip_str;

    auto vnet_underlay_ips = vnet_table_[ctxt.vnet_name].underlay_ips;
    std::string underlay_sip_str = to_string(ctxt.metadata.underlay_ip());
    if (vnet_underlay_ips.find(underlay_sip_str) != vnet_underlay_ips.end())
    {
        SWSS_LOG_INFO("Vnet %s already has PA validation entry for IP %s", ctxt.vnet_name.c_str(), to_string(ctxt.metadata.underlay_ip()).c_str());
        object_statuses.emplace_back(SAI_STATUS_ITEM_ALREADY_EXISTS);
        return;
    }

    uint32_t attr_count = 1;
    sai_pa_validation_entry_t pa_validation_entry;
    pa_validation_entry.vnet_id = gVnetNameToId[ctxt.vnet_name];
    pa_validation_entry.switch_id = gSwitchId;
    to_sai(ctxt.metadata.underlay_ip(), pa_validation_entry.sip);
    sai_attribute_t pa_validation_attr;

    pa_validation_attr.id = SAI_PA_VALIDATION_ENTRY_ATTR_ACTION;
    pa_validation_attr.value.u32 = SAI_PA_VALIDATION_ENTRY_ACTION_PERMIT;

    object_statuses.emplace_back();
    pa_validation_bulker_.create_entry(&object_statuses.back(), &pa_validation_entry,
            attr_count, &pa_validation_attr);
    vnet_table_[ctxt.vnet_name].underlay_ips.insert(underlay_sip_str);
    SWSS_LOG_INFO("Bulk create PA validation entry for Vnet %s underlay IP %s",
                    ctxt.vnet_name.c_str(), to_string(ctxt.metadata.underlay_ip()).c_str());
}

bool DashVnetOrch::addVnetMap(const string& key, VnetMapBulkContext& ctxt)
{
    SWSS_LOG_ENTER();

    bool vnet_exists = (gVnetNameToId.find(ctxt.vnet_name) != gVnetNameToId.end());
    if (!vnet_exists)
    {
        SWSS_LOG_INFO("Not creating VNET map for %s since VNET %s doesn't exist", key.c_str(), ctxt.vnet_name.c_str());
        return false;
    }
    return addOutboundCaToPa(key, ctxt);
}

bool DashVnetOrch::addOutboundCaToPaPost(const string& key, const VnetMapBulkContext& ctxt)
{
    SWSS_LOG_ENTER();

    const auto& object_statuses = ctxt.outbound_ca_to_pa_object_statuses;
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
            return true;
        }

        SWSS_LOG_ERROR("Failed to create CA to PA entry for %s", key.c_str());
        task_process_status handle_status = handleSaiCreateStatus((sai_api_t) SAI_API_DASH_OUTBOUND_CA_TO_PA, status);
        if (handle_status != task_success)
        {
            return parseHandleSaiStatusFailure(handle_status);
        }
    }

    gCrmOrch->incCrmResUsedCounter(ctxt.dip.isV4() ? CrmResourceType::CRM_DASH_IPV4_OUTBOUND_CA_TO_PA : CrmResourceType::CRM_DASH_IPV6_OUTBOUND_CA_TO_PA);

    SWSS_LOG_INFO("Outbound CA to PA  map entry for %s added", key.c_str());

    return true;
}

bool DashVnetOrch::addPaValidationPost(const string& key, const VnetMapBulkContext& ctxt)
{
    SWSS_LOG_ENTER();

    const auto& object_statuses = ctxt.pa_validation_object_statuses;
    if (object_statuses.empty())
    {
        return false;
    }

    auto it_status = object_statuses.begin();
    string underlay_ip_str = to_string(ctxt.metadata.underlay_ip());
    string pa_ref_key = ctxt.vnet_name + ":" + underlay_ip_str;
    sai_status_t status = *it_status++;
    if (status != SAI_STATUS_SUCCESS)
    {
        if (status == SAI_STATUS_ITEM_ALREADY_EXISTS)
        {
            return true;
        }

        SWSS_LOG_ERROR("Failed to create PA validation entry for %s", key.c_str());
        task_process_status handle_status = handleSaiCreateStatus((sai_api_t) SAI_API_DASH_PA_VALIDATION, status);
        if (handle_status != task_success)
        {
            return parseHandleSaiStatusFailure(handle_status);
        }
    }

    gCrmOrch->incCrmResUsedCounter(ctxt.metadata.underlay_ip().has_ipv4() ? CrmResourceType::CRM_DASH_IPV4_PA_VALIDATION : CrmResourceType::CRM_DASH_IPV6_PA_VALIDATION);

    SWSS_LOG_INFO("PA validation entry for %s added", key.c_str());

    return true;
}

bool DashVnetOrch::addVnetMapPost(const string& key, const VnetMapBulkContext& ctxt)
{
    SWSS_LOG_ENTER();

    bool remove_from_consumer = addOutboundCaToPaPost(key, ctxt) && addPaValidationPost(key, ctxt);
    if (!remove_from_consumer)
    {
        SWSS_LOG_ERROR("addVnetMapPost failed for %s ", key.c_str());
        return remove_from_consumer;
    }

    SWSS_LOG_INFO("Vnet map added for %s", key.c_str());

    return remove_from_consumer;
}

void DashVnetOrch::removeOutboundCaToPa(const string& key, VnetMapBulkContext& ctxt)
{
    SWSS_LOG_ENTER();

    auto& object_statuses = ctxt.outbound_ca_to_pa_object_statuses;
    sai_outbound_ca_to_pa_entry_t outbound_ca_to_pa_entry;
    outbound_ca_to_pa_entry.dst_vnet_id = gVnetNameToId[ctxt.vnet_name];
    outbound_ca_to_pa_entry.switch_id = gSwitchId;
    swss::copy(outbound_ca_to_pa_entry.dip, ctxt.dip);

    object_statuses.emplace_back();
    outbound_ca_to_pa_bulker_.remove_entry(&object_statuses.back(), &outbound_ca_to_pa_entry);
}

void DashVnetOrch::removePaValidation(const string& key, DashVnetBulkContext& ctxt)
{
    SWSS_LOG_ENTER();

    auto& object_statuses = ctxt.pa_validation_statuses;
    for (auto ip_str : vnet_table_[ctxt.vnet_name].underlay_ips)
    {
        swss::IpAddress underlay_ip(ip_str);
        sai_pa_validation_entry_t pa_validation_entry;
        pa_validation_entry.vnet_id = gVnetNameToId[ctxt.vnet_name];
        pa_validation_entry.switch_id = gSwitchId;
        swss::copy(pa_validation_entry.sip, underlay_ip);

        object_statuses.emplace_back();
        pa_validation_bulker_.remove_entry(&object_statuses.back(), &pa_validation_entry);
        SWSS_LOG_INFO("Bulk remove PA validation entry for Vnet %s IP %s, removing refcount table entry",
                        ctxt.vnet_name.c_str(), underlay_ip.to_string().c_str());

    }
}

bool DashVnetOrch::removeVnetMap(const string& key, VnetMapBulkContext& ctxt)
{
    SWSS_LOG_ENTER();

    removeOutboundCaToPa(key, ctxt);

    return false;
}

bool DashVnetOrch::removeOutboundCaToPaPost(const string& key, const VnetMapBulkContext& ctxt)
{
    SWSS_LOG_ENTER();

    const auto& object_statuses = ctxt.outbound_ca_to_pa_object_statuses;
    if (object_statuses.empty())
    {
        return false;
    }

    auto it_status = object_statuses.begin();
    sai_status_t status = *it_status++;
    if (status != SAI_STATUS_SUCCESS)
    {
        // Retry later if object has non-zero reference to it
        if (status == SAI_STATUS_NOT_EXECUTED)
        {
            return false;
        }

        if (status == SAI_STATUS_ITEM_NOT_FOUND)
        {
            SWSS_LOG_WARN("Outbound CA to PA entry for %s already removed", key.c_str());
            return true;
        }

        SWSS_LOG_ERROR("Failed to remove outbound CA to PA entry for %s", key.c_str());
        task_process_status handle_status = handleSaiRemoveStatus((sai_api_t) SAI_API_DASH_OUTBOUND_CA_TO_PA, status);
        if (handle_status != task_success)
        {
            return parseHandleSaiStatusFailure(handle_status);
        }
    }

    gCrmOrch->decCrmResUsedCounter(ctxt.dip.isV4() ? CrmResourceType::CRM_DASH_IPV4_OUTBOUND_CA_TO_PA : CrmResourceType::CRM_DASH_IPV6_OUTBOUND_CA_TO_PA);

    SWSS_LOG_INFO("Outbound CA to PA map entry for %s removed", key.c_str());

    return true;
}

bool DashVnetOrch::removePaValidationPost(const string& key, const DashVnetBulkContext& ctxt)
{
    SWSS_LOG_ENTER();
    bool remove_from_consumer = true;

    const auto& object_statuses = ctxt.pa_validation_statuses;
    if (object_statuses.empty())
    {
        return false;
    }

    auto it_status = object_statuses.begin();
    auto it_ip = vnet_table_[ctxt.vnet_name].underlay_ips.begin();
    while (it_ip != vnet_table_[ctxt.vnet_name].underlay_ips.end())
    {
        sai_status_t status = *it_status++;
        swss::IpAddress underlay_ip(*it_ip);
        if (status != SAI_STATUS_SUCCESS)
        {
            // Retry later if object has non-zero reference to it
            if (status == SAI_STATUS_NOT_EXECUTED)
            {
                SWSS_LOG_INFO("PA validation entry for Vnet %s IP %s still in use",
                                ctxt.vnet_name.c_str(), it_ip->c_str());
                remove_from_consumer = false;
                it_ip++;
                continue;
            }

            SWSS_LOG_ERROR("Failed to remove PA validation entry for %s", key.c_str());
            
        }
        it_ip = vnet_table_[ctxt.vnet_name].underlay_ips.erase(it_ip);
        if (*it_status == SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_INFO("PA validation entry for %s removed", key.c_str());
        }
        gCrmOrch->decCrmResUsedCounter(underlay_ip.isV4() ? CrmResourceType::CRM_DASH_IPV4_PA_VALIDATION : CrmResourceType::CRM_DASH_IPV6_PA_VALIDATION);
    }
    return remove_from_consumer;
}

bool DashVnetOrch::removeVnetMapPost(const string& key, const VnetMapBulkContext& ctxt)
{
    SWSS_LOG_ENTER();

    bool remove_from_consumer = removeOutboundCaToPaPost(key, ctxt);
    if (!remove_from_consumer)
    {
        SWSS_LOG_ERROR("removeVnetMapPost failed for %s ", key.c_str());
        return remove_from_consumer;
    }
    SWSS_LOG_INFO("Vnet map removed for %s", key.c_str());

    return remove_from_consumer;
}

void DashVnetOrch::doTaskVnetMapTable(ConsumerBase& consumer)
{
    SWSS_LOG_ENTER();

    auto it = consumer.m_toSync.begin();
    uint32_t result;
    while (it != consumer.m_toSync.end())
    {
        std::map<std::pair<std::string, std::string>,
            VnetMapBulkContext> toBulk;

        while (it != consumer.m_toSync.end())
        {
            KeyOpFieldsValuesTuple tuple = it->second;
            const string& key = kfvKey(tuple);
            auto op = kfvOp(tuple);
            auto rc = toBulk.emplace(std::piecewise_construct,
                    std::forward_as_tuple(key, op),
                    std::forward_as_tuple());
            bool inserted = rc.second;
            auto& ctxt = rc.first->second;
            result = DASH_RESULT_SUCCESS;

            if (!inserted)
            {
                ctxt.clear();
            }

            string& vnet_name = ctxt.vnet_name;
            IpAddress& dip = ctxt.dip;

            vector<string> keys = tokenize(key, ':');
            vnet_name = keys[0];
            size_t pos = key.find(":", vnet_name.length());
            string ip_str = key.substr(pos + 1);
            dip = IpAddress(ip_str);

            if (op == SET_COMMAND)
            {
                if (!parsePbMessage(kfvFieldsValues(tuple), ctxt.metadata))
                {
                    SWSS_LOG_WARN("Requires protobuff at VnetMap :%s", key.c_str());
                    it = consumer.m_toSync.erase(it);
                    continue;
                }
                if (ctxt.metadata.routing_type() == dash::route_type::RoutingType::ROUTING_TYPE_UNSPECIFIED)
                {
                    // VnetMapping::action_type is deprecated in favor of VnetMapping::routing_type. For messages still using the old action_type field,
                    // copy it to the new routing_type field. All subsequent operations will use the new field.
                    #pragma GCC diagnostic push
                    #pragma GCC diagnostic ignored "-Wdeprecated-declarations"
                    SWSS_LOG_WARN("VnetMapping::action_type is deprecated. Use VnetMapping::routing_type instead");
                    ctxt.metadata.set_routing_type(ctxt.metadata.action_type());
                    #pragma GCC diagnostic pop
                }
                if (addVnetMap(key, ctxt))
                {
                    it = consumer.m_toSync.erase(it);
                    /*
                     * Write result only when removing from consumer in pre-op
                     * For other cases, this will be handled in post-op
                     */
                    writeResultToDB(dash_vnet_map_result_table_, key, result);
                }
                else
                {
                    it++;
                }
            }
            else if (op == DEL_COMMAND)
            {
                if (removeVnetMap(key, ctxt))
                {
                    it = consumer.m_toSync.erase(it);
                    removeResultFromDB(dash_vnet_map_result_table_, key);
                }
                else
                {
                    it++;
                }
            }
            else
            {
                SWSS_LOG_ERROR("Invalid command %s", op.c_str());
                it = consumer.m_toSync.erase(it);
            }
        }

        outbound_ca_to_pa_bulker_.flush();
        pa_validation_bulker_.flush();

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
            const auto& outbound_ca_to_pa_object_statuses = ctxt.outbound_ca_to_pa_object_statuses;
            const auto& pa_validation_object_statuses = ctxt.pa_validation_object_statuses;
            if (outbound_ca_to_pa_object_statuses.empty() && pa_validation_object_statuses.empty())
            {
                it_prev++;
                continue;
            }

            if (op == SET_COMMAND)
            {
                if (addVnetMapPost(key, ctxt))
                {
                    it_prev = consumer.m_toSync.erase(it_prev);
                }
                else
                {
                    result = DASH_RESULT_FAILURE;
                    it_prev++;
                }
                writeResultToDB(dash_vnet_map_result_table_, key, result);
            }
            else if (op == DEL_COMMAND)
            {
                if (removeVnetMapPost(key, ctxt))
                {
                    it_prev = consumer.m_toSync.erase(it_prev);
                    removeResultFromDB(dash_vnet_map_result_table_, key);
                }
                else
                {
                    it_prev++;
                }
            }
        }
    }
}

void DashVnetOrch::doTask(ConsumerBase &consumer)
{
    SWSS_LOG_ENTER();

    const auto& tn = consumer.getTableName();

    SWSS_LOG_INFO("Table name: %s", tn.c_str());

    if (tn == APP_DASH_VNET_TABLE_NAME)
    {
        doTaskVnetTable(consumer);
    }
    else if (tn == APP_DASH_VNET_MAPPING_TABLE_NAME)
    {
        doTaskVnetMapTable(consumer);
    }
    else
    {
        SWSS_LOG_ERROR("Unknown table: %s", tn.c_str());
    }
}
