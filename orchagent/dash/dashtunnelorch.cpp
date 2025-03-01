#include "dashtunnelorch.h"
#include "dashorch.h"
#include "orch.h"
#include "sai.h"
#include "taskworker.h"
#include "pbutils.h"
#include "directory.h"

extern size_t gMaxBulkSize;
extern sai_dash_tunnel_api_t* sai_dash_tunnel_api;
extern sai_object_id_t gSwitchId;
extern Directory<Orch*> gDirectory;

DashTunnelOrch::DashTunnelOrch(
    swss::DBConnector *db,
    std::vector<std::string> &tables,
    swss::ZmqServer *zmqServer) :
    tunnel_bulker_(sai_dash_tunnel_api, gMaxBulkSize, SAI_OBJECT_TYPE_DASH_TUNNEL),
    tunnel_member_bulker_(sai_dash_tunnel_api, gMaxBulkSize, SAI_OBJECT_TYPE_DASH_TUNNEL_MEMBER),
    tunnel_nhop_bulker_(sai_dash_tunnel_api, gMaxBulkSize, SAI_OBJECT_TYPE_DASH_TUNNEL_NEXT_HOP),
    ZmqOrch(db, tables, zmqServer)
{
    SWSS_LOG_ENTER();
}

void DashTunnelOrch::doTask(ConsumerBase &consumer)
{
    /* bulk ops here need to happen in multiple steps because DASH_TUNNEL_MEMBERS depend on DASH_TUNNEL and DASH_TUNNEL_NEXT_HOPS already existing
        1. Pre-bulk 1:
            - For SET, add DASH_TUNNEL and DASH_TUNNEL_NEXT_HOP objects to bulker
            - For DEL, add DASH_TUNNEL, DASH_TUNNEL_NEXT_HOP, and DASH_TUNNEL_MEMBER objects to bulker
        2. Flush tunnel_member bulker first, then tunnel and tunnel_nhop bulkers to SAI
            - There shouldn't be any SET ops in the tunnel_member bulker yet, only DEL. We need to flush it first,
              otherwise SAI cannot delete the reference tunnel and tunnel_nhop
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
                    it++;
                }
                else
                {
                    SWSS_LOG_ERROR("Failed to add DASH tunnel %s", tunnel_name.c_str());
                    it = consumer.m_toSync.erase(it);
                }
            }
            else if (op == DEL_COMMAND)
            {
                if (removeTunnel(tunnel_name, ctxt))
                {
                    it++;
                }
                else
                {
                    SWSS_LOG_ERROR("Failed to remove DASH tunnel %s", tunnel_name.c_str());
                    it = consumer.m_toSync.erase(it);
                }
            }
        }
        
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
                    it_prev++;
                }
                else
                {
                    SWSS_LOG_ERROR("DASH tunnel %s bulk add failed", tunnel_name.c_str());
                    it_prev = consumer.m_toSync.erase(it_prev);
                }
            }
            else if (op == DEL_COMMAND)
            {
                if (removeTunnelPost(tunnel_name, ctxt))
                {
                    it_prev++;
                }
                else
                {
                    SWSS_LOG_ERROR("DASH tunnel %s bulk remove failed", tunnel_name.c_str());
                    it_prev = consumer.m_toSync.erase(it_prev);
                }
            }
        }

        tunnel_member_bulker_.flush();

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
                if (addTunnelMemberPost(tunnel_name, ctxt))
                {
                    it_prev++;
                }
                else
                {
                    it_prev = consumer.m_toSync.erase(it_prev);
                }
            }
            else if (op == DEL_COMMAND)
            {
                if (removeTunnelMemberPost(tunnel_name, ctxt))
                {
                    it_prev++;
                }
                else
                {
                    it_prev = consumer.m_toSync.erase(it_prev);
                }
            }
        }
    }
}

bool DashTunnelOrch::addTunnel(const std::string& tunnel_name, DashTunnelBulkContext& ctxt)
{
    SWSS_LOG_ENTER();
    std::vector<sai_attribute_t> tunnel_attrs;
    sai_attribute_t tunnel_attr;
    bool keep_in_consumer = false;

    bool exists = (tunnel_table_.find(tunnel_name) != tunnel_table_.end());
    if (exists)
    {
        SWSS_LOG_WARN("DASH tunnel %s already exists", tunnel_name.c_str());
        return keep_in_consumer;
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
            return keep_in_consumer;
    }
    tunnel_attrs.push_back(tunnel_attr);

    tunnel_attr.id = SAI_DASH_TUNNEL_ATTR_TUNNEL_KEY;
    tunnel_attr.value.u32 = ctxt.metadata.vni();
    tunnel_attrs.push_back(tunnel_attr);

    tunnel_attr.id = SAI_DASH_TUNNEL_ATTR_SIP;
    auto dash_orch = gDirectory.get<DashOrch*>();
    auto tunnel_sip = dash_orch->getApplianceVip();
    to_sai(tunnel_sip, tunnel_attr.value.ipaddr);
    tunnel_attrs.push_back(tunnel_attr);

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
    tunnel_bulker_.create_entry(&object_ids.back(), tunnel_attrs.size(), tunnel_attrs.data());

    keep_in_consumer = true;
    return keep_in_consumer;
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

    bool keep_in_consumer = false;
    const auto& object_ids = ctxt.tunnel_object_ids;
    if (object_ids.empty())
    {
        return keep_in_consumer;
    }

    auto it_id = object_ids.begin();
    sai_object_id_t tunnel_oid = *it_id++;
    if (tunnel_oid == SAI_NULL_OBJECT_ID)
    {
        SWSS_LOG_ERROR("Failed to create DASH tunnel entry for %s", tunnel_name.c_str());
        // even if tunnel creation fails, we need to continue checking nexthop creations so that
        // tunnel_object_ids and tunnel_nhop_object_ids remain aligned
    }
    else
    {
        DashTunnelEntry entry = { tunnel_oid };
        tunnel_table_[tunnel_name] = entry;
        keep_in_consumer = true;
    }

    SWSS_LOG_INFO("Tunnel entry added for %s", tunnel_name.c_str());

    if (ctxt.metadata.endpoints_size() > 1)
    {
        return keep_in_consumer && addTunnelNextHopsPost(tunnel_name, ctxt, keep_in_consumer);
    }

    return keep_in_consumer;
}

bool DashTunnelOrch::addTunnelNextHopsPost(const std::string& tunnel_name, DashTunnelBulkContext& ctxt, const bool tunnel_success)
{
    SWSS_LOG_ENTER();

    bool keep_in_consumer = false;
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

        if (!tunnel_success)
        {
            SWSS_LOG_INFO("Removing tunnel next hop OID %%" PRIx64" for failed DASH tunnel %s", nhop_oid, tunnel_name.c_str());
            sai_status_t status = sai_dash_tunnel_api->remove_dash_tunnel_next_hop(nhop_oid);
            if (status != SAI_STATUS_SUCCESS)
            {
                SWSS_LOG_ERROR("Failed to remove DASH tunnel next hop OID %%" PRIx64" for failed DASH tunnel %s", nhop_oid, tunnel_name.c_str());
            }
            continue;
        }

        DashTunnelEndpointEntry endpoint = { nhop_oid };
        tunnel_table_[tunnel_name].endpoints[to_string(ip)] = endpoint;
        addTunnelMember(tunnel_table_[tunnel_name].tunnel_oid, nhop_oid, ctxt);
        keep_in_consumer = true; // if we add at least one tunnel member, tunnel needs to stay in consumer for tunnel member post-bulk ops
    }
    return keep_in_consumer;
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
    tunnel_member_bulker_.create_entry(&member_object_ids.back(), tunnel_member_attrs.size(), tunnel_member_attrs.data());
}

bool DashTunnelOrch::addTunnelMemberPost(const std::string& tunnel_name, const DashTunnelBulkContext& ctxt)
{
    SWSS_LOG_ENTER();
    bool keep_in_consumer = false;
    const auto& member_oids = ctxt.tunnel_member_object_ids;
    auto it_member = member_oids.begin();
    for (auto ip : ctxt.metadata.endpoints())
    {
        sai_object_id_t member_oid = *it_member++;
        if (member_oid == SAI_NULL_OBJECT_ID)
        {
            SWSS_LOG_ERROR("Failed to create DASH tunnel member entry for tunnel %s, endpoint %s", tunnel_name.c_str(), to_string(ip).c_str());
            continue;
        }
        tunnel_table_[tunnel_name].endpoints[to_string(ip)].tunnel_member_oid = member_oid;
        SWSS_LOG_INFO("Tunnel member entry added for tunnel %s, endpoint %s", tunnel_name.c_str(), to_string(ip).c_str());
    }
    return keep_in_consumer;
}