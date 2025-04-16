#include "orchagent/dash/dashtunnelorch.h"
#include "orchagent/dash/dashorch.h"
#include "orchagent/orch.h"
#include "sai.h"
#include "orchagent/dash/taskworker.h"
#include "orchagent/dash/pbutils.h"
#include "orchagent/directory.h"
#include "orchagent/saihelper.h"

extern size_t gMaxBulkSize;
extern sai_dash_tunnel_api_t* sai_dash_tunnel_api;
extern sai_object_id_t gSwitchId;
extern Directory<Orch*> gDirectory;

bool ipAddrLt(const dash::types::IpAddress& lhs, const dash::types::IpAddress& rhs)
{
    if (lhs.has_ipv4() && rhs.has_ipv4())
    {
        return lhs.ipv4() < rhs.ipv4();
    }
    else if (lhs.has_ipv6() && rhs.has_ipv6())
    {
        return lhs.ipv6() < rhs.ipv6();
    }
    else if (lhs.has_ipv4() && rhs.has_ipv6())
    {
        return true;
    }
    else if (lhs.has_ipv6() && rhs.has_ipv4())
    {
        return false;
    }
    SWSS_LOG_ERROR("One or more IP addresses not set");
    return false;
}

bool ipAddrEq(const dash::types::IpAddress& lhs, const dash::types::IpAddress& rhs)
{
    if (lhs.has_ipv4() && rhs.has_ipv4())
    {
        return lhs.ipv4() == rhs.ipv4();
    }
    else if (lhs.has_ipv6() && rhs.has_ipv6())
    {
        return lhs.ipv6() == rhs.ipv6();
    }
    return false;
};


DashTunnelOrch::DashTunnelOrch(
    swss::DBConnector *db,
    std::vector<std::string> &tables,
    swss::ZmqServer *zmqServer) :
    tunnel_bulker_(sai_dash_tunnel_api, gSwitchId, gMaxBulkSize, SAI_OBJECT_TYPE_DASH_TUNNEL),
    tunnel_member_bulker_(sai_dash_tunnel_api, gSwitchId, gMaxBulkSize, SAI_OBJECT_TYPE_DASH_TUNNEL_MEMBER),
    tunnel_nhop_bulker_(sai_dash_tunnel_api, gSwitchId, gMaxBulkSize, SAI_OBJECT_TYPE_DASH_TUNNEL_NEXT_HOP),
    ZmqOrch(db, tables, zmqServer)
{
    SWSS_LOG_ENTER();
}

sai_object_id_t DashTunnelOrch::getTunnelOid(const std::string& tunnel_name)
{
    SWSS_LOG_ENTER();
    auto it = tunnel_table_.find(tunnel_name);
    if (it == tunnel_table_.end())
    {
        return SAI_NULL_OBJECT_ID;
    }
    return it->second.tunnel_oid;
}

void DashTunnelOrch::doTask(ConsumerBase &consumer)
{
    /* bulk ops here need to happen in multiple steps because DASH_TUNNEL_MEMBERS depend on DASH_TUNNEL and DASH_TUNNEL_NEXT_HOPS already existing
        1. Pre-bulk 1:
            - For SET, add DASH_TUNNEL and DASH_TUNNEL_NEXT_HOP objects to bulker
            - For DEL, add DASH_TUNNEL, DASH_TUNNEL_NEXT_HOP, and DASH_TUNNEL_MEMBER objects to bulker
        2. Flush tunnel_member bulker first, then tunnel and tunnel_nhop bulkers to SAI
            - There shouldn't be any SET ops in the tunnel_member bulker yet, only DEL. We need to flush it first,
              otherwise SAI cannot delete the referenced tunnel and tunnel_nhop
        3. Post-bulk 1/pre-bulk 2:
            - For SET, add DASH_TUNNEL_MEMBER objects to bulker
            - For DEL, we are done
        4. Flush tunnel_member bulker to SAI
        5. Post-bulk 2:
            - For SET, we are done
    */
    SWSS_LOG_ENTER();

    const auto& tn = consumer.getTableName();
    SWSS_LOG_INFO("doTask: %s", tn.c_str());
    if (tn != APP_DASH_TUNNEL_TABLE_NAME)
    {
        SWSS_LOG_ERROR("DashTunnelOrch does not support table %s", tn.c_str());
        return;
    }

    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
        std::map<std::pair<std::string, std::string>,
            DashTunnelBulkContext> toBulk;
        while (it != consumer.m_toSync.end())
        {
            swss::KeyOpFieldsValuesTuple t = it->second;
            std::string tunnel_name = kfvKey(t);
            std::string op = kfvOp(t);
            auto rc = toBulk.emplace(std::piecewise_construct,
                    std::forward_as_tuple(tunnel_name, op),
                    std::forward_as_tuple());
            bool inserted = rc.second;
            auto& ctxt = rc.first->second;
            if (!inserted)
            {
                ctxt.clear();
            }
            if (op == SET_COMMAND)
            {
                if (!parsePbMessage(kfvFieldsValues(t), ctxt.metadata))
                {
                    SWSS_LOG_WARN("Requires protobuf at Tunnel :%s", tunnel_name.c_str());
                    it = consumer.m_toSync.erase(it);
                    continue;
                }
                if (addTunnel(tunnel_name, ctxt))
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
                if (removeTunnel(tunnel_name, ctxt))
                {
                    it = consumer.m_toSync.erase(it);
                }
                else
                {
                    it++;
                }
            }
        }
        
        tunnel_member_bulker_.flush();
        tunnel_bulker_.flush();
        tunnel_nhop_bulker_.flush();

        auto it_prev = consumer.m_toSync.begin();
        while (it_prev != it)
        {
            swss::KeyOpFieldsValuesTuple t = it_prev->second;
            std::string tunnel_name = kfvKey(t);
            std::string op = kfvOp(t);
            auto found = toBulk.find(std::make_pair(tunnel_name, op));
            if (found == toBulk.end())
            {
                it_prev++;
                continue;
            }
            auto& ctxt = found->second;

            if (op == SET_COMMAND)
            {
                if (addTunnelPost(tunnel_name, ctxt))
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
                if (removeTunnelPost(tunnel_name, ctxt))
                {
                    it_prev = consumer.m_toSync.erase(it_prev);
                }
                else
                {
                    it_prev++;
                }
            }
        }

        tunnel_member_bulker_.flush();

        it_prev = consumer.m_toSync.begin();
        while (it_prev != it)
        {
            swss::KeyOpFieldsValuesTuple t = it_prev->second;
            std::string tunnel_name = kfvKey(t);
            std::string op = kfvOp(t);
            auto found = toBulk.find(std::make_pair(tunnel_name, op));
            if (found == toBulk.end())
            {
                it_prev++;
                continue;
            }
            auto& ctxt = found->second;

            if (op == SET_COMMAND)
            {
                if (addTunnelMemberPost(tunnel_name, ctxt))
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
                // We should never get here
                it_prev = consumer.m_toSync.erase(it_prev);
            }
        }
    }
}

bool DashTunnelOrch::addTunnel(const std::string& tunnel_name, DashTunnelBulkContext& ctxt)
{
    SWSS_LOG_ENTER();
    auto dash_orch = gDirectory.get<DashOrch*>();
    if (!dash_orch->hasApplianceEntry())
    {
        SWSS_LOG_WARN("DASH appliance entry not found, skipping DASH tunnel %s creation", tunnel_name.c_str());
        return false;
    }
    std::vector<sai_attribute_t> tunnel_attrs;
    sai_attribute_t tunnel_attr;
    bool remove_from_consumer = true;

    bool exists = (tunnel_table_.find(tunnel_name) != tunnel_table_.end());
    if (exists)
    {
        SWSS_LOG_WARN("DASH tunnel %s already exists", tunnel_name.c_str());
        return remove_from_consumer;
    }

    tunnel_attr.id = SAI_DASH_TUNNEL_ATTR_MAX_MEMBER_SIZE;
    tunnel_attr.value.u32 = ctxt.metadata.endpoints_size();
    tunnel_attrs.push_back(tunnel_attr);

    tunnel_attr.id = SAI_DASH_TUNNEL_ATTR_DASH_ENCAPSULATION;
    switch (ctxt.metadata.encap_type())
    {
        case dash::route_type::ENCAP_TYPE_VXLAN:
            tunnel_attr.value.u32 = SAI_DASH_ENCAPSULATION_VXLAN;
            break;
        case dash::route_type::ENCAP_TYPE_NVGRE:
            tunnel_attr.value.u32 = SAI_DASH_ENCAPSULATION_NVGRE;
            break;
        default:
            SWSS_LOG_ERROR("Unsupported encap type %d", ctxt.metadata.encap_type());
            return remove_from_consumer;
    }
    tunnel_attrs.push_back(tunnel_attr);

    tunnel_attr.id = SAI_DASH_TUNNEL_ATTR_TUNNEL_KEY;
    tunnel_attr.value.u32 = ctxt.metadata.vni();
    tunnel_attrs.push_back(tunnel_attr);

    tunnel_attr.id = SAI_DASH_TUNNEL_ATTR_SIP;
    auto tunnel_sip = dash_orch->getApplianceVip();
    to_sai(tunnel_sip, tunnel_attr.value.ipaddr);
    tunnel_attrs.push_back(tunnel_attr);

    // deduplicate endpoint IPs
    std::sort(ctxt.metadata.mutable_endpoints()->begin(), ctxt.metadata.mutable_endpoints()->end(), ipAddrLt);
    auto last = std::unique(ctxt.metadata.mutable_endpoints()->begin(), ctxt.metadata.mutable_endpoints()->end(), ipAddrEq);
    ctxt.metadata.mutable_endpoints()->erase(last, ctxt.metadata.mutable_endpoints()->end());

    if (ctxt.metadata.endpoints_size() == 1)
    {
        tunnel_attr.id = SAI_DASH_TUNNEL_ATTR_DIP;
        to_sai(ctxt.metadata.endpoints(0), tunnel_attr.value.ipaddr);
        tunnel_attrs.push_back(tunnel_attr);
    }
    else
    {
        addTunnelNextHops(tunnel_name, ctxt);
    }

    auto& object_ids = ctxt.tunnel_object_ids;
    object_ids.emplace_back();
    tunnel_bulker_.create_entry(&object_ids.back(), (uint32_t) tunnel_attrs.size(), tunnel_attrs.data());

    remove_from_consumer = false;
    return remove_from_consumer;
}

void DashTunnelOrch::addTunnelNextHops(const std::string& tunnel_name, DashTunnelBulkContext& ctxt)
{
    SWSS_LOG_ENTER();
    sai_attribute_t tunnel_nhop_attr;
    auto& nhop_object_ids = ctxt.tunnel_nhop_object_ids;
    for (auto ip : ctxt.metadata.endpoints())
    {
        tunnel_nhop_attr.id = SAI_DASH_TUNNEL_NEXT_HOP_ATTR_DIP;
        to_sai(ip, tunnel_nhop_attr.value.ipaddr);
        nhop_object_ids.emplace_back();
        tunnel_nhop_bulker_.create_entry(&nhop_object_ids.back(), 1, &tunnel_nhop_attr);
    }
}

bool DashTunnelOrch::addTunnelPost(const std::string& tunnel_name, DashTunnelBulkContext& ctxt)
{
    SWSS_LOG_ENTER();

    bool remove_from_consumer = true;
    const auto& object_ids = ctxt.tunnel_object_ids;
    if (object_ids.empty())
    {
        return remove_from_consumer;
    }

    auto it_id = object_ids.begin();
    sai_object_id_t tunnel_oid = *it_id++;
    if (tunnel_oid == SAI_NULL_OBJECT_ID)
    {
        SWSS_LOG_ERROR("Failed to create DASH tunnel entry for %s", tunnel_name.c_str());
        // even if tunnel creation fails, we need to continue checking nexthop creations
        // to remove nexthops created for this tunnel
    }
    else
    {
        DashTunnelEntry entry = { tunnel_oid, std::map<std::string, DashTunnelEndpointEntry>(), std::string() };
        tunnel_table_[tunnel_name] = entry;
        remove_from_consumer = false;
        SWSS_LOG_INFO("Tunnel entry added for %s", tunnel_name.c_str());
    }

    return addTunnelNextHopsPost(tunnel_name, ctxt, remove_from_consumer);
}

bool DashTunnelOrch::addTunnelNextHopsPost(const std::string& tunnel_name, DashTunnelBulkContext& ctxt, const bool parent_tunnel_removed)
{
    SWSS_LOG_ENTER();

    if (ctxt.metadata.endpoints_size() <= 1)
    {
        return parent_tunnel_removed;
    }

    bool remove_from_consumer = true;
    const auto& nhop_oids = ctxt.tunnel_nhop_object_ids;
    auto it_nhop = nhop_oids.begin();
    for (auto ip : ctxt.metadata.endpoints())
    {
        sai_object_id_t nhop_oid = *it_nhop++;
        if (nhop_oid == SAI_NULL_OBJECT_ID)
        {
            SWSS_LOG_ERROR("Failed to create DASH tunnel next hop entry for tunnel %s, endpoint %s", tunnel_name.c_str(), to_string(ip).c_str());
            continue;
        }

        if (parent_tunnel_removed)
        {
            SWSS_LOG_INFO("Removing tunnel next hop OID %" PRIx64" for failed DASH tunnel %s endpoint %s", nhop_oid, tunnel_name.c_str(), to_string(ip).c_str());
            sai_status_t status = sai_dash_tunnel_api->remove_dash_tunnel_next_hop(nhop_oid);
            if (status != SAI_STATUS_SUCCESS)
            {
                SWSS_LOG_ERROR("Failed to remove DASH tunnel next hop OID %" PRIx64" for failed DASH tunnel %s endpoint %s", nhop_oid, tunnel_name.c_str(), to_string(ip).c_str());
            }
            continue;
        }

        DashTunnelEndpointEntry endpoint = { nhop_oid, SAI_NULL_OBJECT_ID };
        tunnel_table_[tunnel_name].endpoints[to_string(ip)] = endpoint;
        SWSS_LOG_INFO("Tunnel next hop entry added for tunnel %s, endpoint %s", tunnel_name.c_str(), to_string(ip).c_str());
        addTunnelMember(tunnel_table_[tunnel_name].tunnel_oid, nhop_oid, ctxt);
        remove_from_consumer = false; // if we add at least one tunnel member, tunnel needs to stay in consumer for tunnel member post-bulk ops
    }
    return remove_from_consumer;
}

void DashTunnelOrch::addTunnelMember(const sai_object_id_t tunnel_oid, const sai_object_id_t nhop_oid, DashTunnelBulkContext& ctxt)
{
    SWSS_LOG_ENTER();
    std::vector<sai_attribute_t> tunnel_member_attrs;
    sai_attribute_t tunnel_member_attr;

    tunnel_member_attr.id = SAI_DASH_TUNNEL_MEMBER_ATTR_DASH_TUNNEL_ID;
    tunnel_member_attr.value.oid = tunnel_oid;
    tunnel_member_attrs.push_back(tunnel_member_attr);

    tunnel_member_attr.id = SAI_DASH_TUNNEL_MEMBER_ATTR_DASH_TUNNEL_NEXT_HOP_ID;
    tunnel_member_attr.value.oid = nhop_oid;
    tunnel_member_attrs.push_back(tunnel_member_attr);

    auto& member_object_ids = ctxt.tunnel_member_object_ids;
    member_object_ids.emplace_back();
    tunnel_member_bulker_.create_entry(&member_object_ids.back(), (uint32_t) tunnel_member_attrs.size(), tunnel_member_attrs.data());
}

bool DashTunnelOrch::addTunnelMemberPost(const std::string& tunnel_name, const DashTunnelBulkContext& ctxt)
{
    SWSS_LOG_ENTER();
    bool remove_from_consumer = true;
    const auto& member_oids = ctxt.tunnel_member_object_ids;
    auto it_member = member_oids.begin();
    for (auto ip : ctxt.metadata.endpoints())
    {
        sai_object_id_t member_oid = *it_member++;
        if (member_oid == SAI_NULL_OBJECT_ID)
        {
            SWSS_LOG_WARN("Failed to create DASH tunnel member entry for tunnel %s, endpoint %s, continuing", tunnel_name.c_str(), to_string(ip).c_str());
            continue;
        }
        tunnel_table_[tunnel_name].endpoints[to_string(ip)].tunnel_member_oid = member_oid;
        SWSS_LOG_INFO("Tunnel member entry added for tunnel %s, endpoint %s", tunnel_name.c_str(), to_string(ip).c_str());
    }
    return remove_from_consumer;
}

bool DashTunnelOrch::removeTunnel(const std::string& tunnel_name, DashTunnelBulkContext& ctxt)
{
    SWSS_LOG_ENTER();
    bool remove_from_consumer = true;

    auto it = tunnel_table_.find(tunnel_name);
    if (it == tunnel_table_.end())
    {
        SWSS_LOG_WARN("Failed to find DASH tunnel %s to remove", tunnel_name.c_str());
        return remove_from_consumer;
    }

    auto& endpoints = it->second.endpoints;
    if (endpoints.size() > 1)
    {
        removeTunnelEndpoints(tunnel_name, ctxt);
    }
    

    auto& object_statuses = ctxt.tunnel_object_statuses;
    object_statuses.emplace_back();
    tunnel_bulker_.remove_entry(&object_statuses.back(), it->second.tunnel_oid);

    remove_from_consumer = false;
    return remove_from_consumer;
}

bool DashTunnelOrch::removeTunnelPost(const std::string& tunnel_name, const DashTunnelBulkContext& ctxt)
{
    SWSS_LOG_ENTER();
    bool remove_from_consumer = removeTunnelEndpointsPost(tunnel_name, ctxt);
    if (!remove_from_consumer)
    {
        // If endpoint removal requires a retry, exit immediately since the tunnel can't be deleted if endpoints still exist
        return remove_from_consumer;
    }

    const auto& object_statuses = ctxt.tunnel_object_statuses;
    if (object_statuses.empty())
    {
        return remove_from_consumer;
    }

    auto it_status = object_statuses.begin();
    sai_status_t status = *it_status++;
    if (status != SAI_STATUS_SUCCESS)
    {
        if (status == SAI_STATUS_OBJECT_IN_USE)
        {
            // Retry later if object has non-zero reference to it
            SWSS_LOG_WARN("DASH tunnel %s is in use, cannot remove", tunnel_name.c_str());
            remove_from_consumer = false;
            return remove_from_consumer;
        }

        task_process_status handle_status = handleSaiRemoveStatus((sai_api_t) SAI_API_DASH_TUNNEL, status);
        if (handle_status != task_success)
        {
            remove_from_consumer = parseHandleSaiStatusFailure(handle_status);
            return remove_from_consumer;
        }
    }

    tunnel_table_.erase(tunnel_name);
    SWSS_LOG_NOTICE("DASH tunnel entry removed for %s", tunnel_name.c_str());

    remove_from_consumer = true;
    return remove_from_consumer;
}

void DashTunnelOrch::removeTunnelEndpoints(const std::string& tunnel_name, DashTunnelBulkContext& ctxt)
{
    SWSS_LOG_ENTER();

    auto it = tunnel_table_.find(tunnel_name);
    if (it == tunnel_table_.end())
    {
        SWSS_LOG_WARN("Failed to find DASH tunnel %s to remove endpoints from", tunnel_name.c_str());
        return;
    }

    auto& endpoints = it->second.endpoints;
    for (auto& endpoint : endpoints)
    {
        auto& tunnel_member_statuses = ctxt.tunnel_member_object_statuses;
        if (endpoint.second.tunnel_member_oid == SAI_NULL_OBJECT_ID)
        {
            tunnel_member_statuses.emplace_back(SAI_STATUS_SUCCESS);
        } 
        else
        {
            tunnel_member_statuses.emplace_back();
            tunnel_member_bulker_.remove_entry(&tunnel_member_statuses.back(), endpoint.second.tunnel_member_oid);
        }

        // No null OID check needed since we cannot delete a tunnel nhop without first deleting the associated tunnel member
        // If the endpoint entry exists, safe to assume that at least the tunnel nhop still exists
        auto& tunnel_nhop_statuses = ctxt.tunnel_nhop_object_statuses;
        tunnel_nhop_statuses.emplace_back();
        tunnel_nhop_bulker_.remove_entry(&tunnel_nhop_statuses.back(), endpoint.second.tunnel_nhop_oid);
    }
}

bool DashTunnelOrch::removeTunnelEndpointsPost(const std::string& tunnel_name, const DashTunnelBulkContext& ctxt)
{
    SWSS_LOG_ENTER();
    bool remove_from_consumer = true;

    const auto& tunnel_member_statuses = ctxt.tunnel_member_object_statuses;
    const auto& tunnel_nhop_statuses = ctxt.tunnel_nhop_object_statuses;
    if (tunnel_member_statuses.empty() && tunnel_nhop_statuses.empty())
    {
        return remove_from_consumer;
    }

    auto tm_it_status = tunnel_member_statuses.begin();
    auto nh_it_status = tunnel_nhop_statuses.begin();
    auto endpoint_it = tunnel_table_[tunnel_name].endpoints.begin();
    while (endpoint_it != tunnel_table_[tunnel_name].endpoints.end())
    {
        sai_status_t tm_status = *tm_it_status++;
        sai_status_t nh_status = *nh_it_status++;
        if (tm_status != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_WARN("DASH tunnel member removal for tunnel %s endpoint %s failed with %s", tunnel_name.c_str(), endpoint_it->first.c_str(), sai_serialize_status(tm_status).c_str());
            task_process_status handle_status = handleSaiRemoveStatus((sai_api_t) SAI_API_DASH_TUNNEL, tm_status);
            if (handle_status == task_need_retry)
            {
                remove_from_consumer = false;
            }
        }
        else
        {
            if (endpoint_it->second.tunnel_member_oid != SAI_NULL_OBJECT_ID)
            {
                SWSS_LOG_INFO("DASH tunnel member removed for tunnel %s ip %s", tunnel_name.c_str(), endpoint_it->first.c_str());
                endpoint_it->second.tunnel_member_oid = SAI_NULL_OBJECT_ID;
            }
        }

        if (nh_status != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_WARN("DASH tunnel next hop removal for tunnel %s endpoint %s failed with %s", tunnel_name.c_str(), endpoint_it->first.c_str(), sai_serialize_status(tm_status).c_str());
            task_process_status handle_status = handleSaiRemoveStatus((sai_api_t) SAI_API_DASH_TUNNEL, nh_status);
            if (handle_status == task_need_retry)
            {
                remove_from_consumer = false;
            }
        }
        else
        {
            SWSS_LOG_INFO("DASH tunnel next hop removed for tunnel %s ip %s", tunnel_name.c_str(), endpoint_it->first.c_str());
            endpoint_it = tunnel_table_[tunnel_name].endpoints.erase(endpoint_it);
            continue;
        }
        endpoint_it++;
    }

    return remove_from_consumer;
}
