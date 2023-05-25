#include <iostream>
#include <sstream>

#include "routeorch.h"
#include "logger.h"
#include "srv6orch.h"
#include "sai_serialize.h"
#include "crmorch.h"

using namespace std;
using namespace swss;

extern sai_object_id_t gSwitchId;
extern sai_object_id_t  gVirtualRouterId;
extern sai_object_id_t  gUnderlayIfId;
extern sai_srv6_api_t* sai_srv6_api;
extern sai_tunnel_api_t* sai_tunnel_api;
extern sai_next_hop_api_t* sai_next_hop_api;

extern RouteOrch *gRouteOrch;
extern CrmOrch *gCrmOrch;

const map<string, sai_my_sid_entry_endpoint_behavior_t> end_behavior_map =
{
    {"end",                SAI_MY_SID_ENTRY_ENDPOINT_BEHAVIOR_E},
    {"end.x",              SAI_MY_SID_ENTRY_ENDPOINT_BEHAVIOR_X},
    {"end.t",              SAI_MY_SID_ENTRY_ENDPOINT_BEHAVIOR_T},
    {"end.dx6",            SAI_MY_SID_ENTRY_ENDPOINT_BEHAVIOR_DX6},
    {"end.dx4",            SAI_MY_SID_ENTRY_ENDPOINT_BEHAVIOR_DX4},
    {"end.dt4",            SAI_MY_SID_ENTRY_ENDPOINT_BEHAVIOR_DT4},
    {"end.dt6",            SAI_MY_SID_ENTRY_ENDPOINT_BEHAVIOR_DT6},
    {"end.dt46",           SAI_MY_SID_ENTRY_ENDPOINT_BEHAVIOR_DT46},
    {"end.b6.encaps",      SAI_MY_SID_ENTRY_ENDPOINT_BEHAVIOR_B6_ENCAPS},
    {"end.b6.encaps.red",  SAI_MY_SID_ENTRY_ENDPOINT_BEHAVIOR_B6_ENCAPS_RED},
    {"end.b6.insert",      SAI_MY_SID_ENTRY_ENDPOINT_BEHAVIOR_B6_INSERT},
    {"end.b6.insert.red",  SAI_MY_SID_ENTRY_ENDPOINT_BEHAVIOR_B6_INSERT_RED},
    {"udx6",               SAI_MY_SID_ENTRY_ENDPOINT_BEHAVIOR_DX6},
    {"udx4",               SAI_MY_SID_ENTRY_ENDPOINT_BEHAVIOR_DX4},
    {"udt6",               SAI_MY_SID_ENTRY_ENDPOINT_BEHAVIOR_DT6},
    {"udt4",               SAI_MY_SID_ENTRY_ENDPOINT_BEHAVIOR_DT4},
    {"udt46",              SAI_MY_SID_ENTRY_ENDPOINT_BEHAVIOR_DT46},
    {"un",                 SAI_MY_SID_ENTRY_ENDPOINT_BEHAVIOR_UN},
    {"ua",                 SAI_MY_SID_ENTRY_ENDPOINT_BEHAVIOR_UA}
};

const map<string, sai_my_sid_entry_endpoint_behavior_flavor_t> end_flavor_map =
{
    {"end",                SAI_MY_SID_ENTRY_ENDPOINT_BEHAVIOR_FLAVOR_PSP_AND_USD},
    {"end.x",              SAI_MY_SID_ENTRY_ENDPOINT_BEHAVIOR_FLAVOR_PSP_AND_USD},
    {"end.t",              SAI_MY_SID_ENTRY_ENDPOINT_BEHAVIOR_FLAVOR_PSP_AND_USD},
    {"un",                 SAI_MY_SID_ENTRY_ENDPOINT_BEHAVIOR_FLAVOR_PSP_AND_USD},
    {"ua",                 SAI_MY_SID_ENTRY_ENDPOINT_BEHAVIOR_FLAVOR_PSP_AND_USD}
};

const map<string, sai_srv6_sidlist_type_t> sidlist_type_map =
{
    {"insert",             SAI_SRV6_SIDLIST_TYPE_INSERT},
    {"insert.red",         SAI_SRV6_SIDLIST_TYPE_INSERT_RED},
    {"encaps",             SAI_SRV6_SIDLIST_TYPE_ENCAPS},
    {"encaps.red",         SAI_SRV6_SIDLIST_TYPE_ENCAPS_RED}
};

void Srv6Orch::srv6TunnelUpdateNexthops(const string srv6_source, const NextHopKey nhkey, bool insert)
{
    if (insert)
    {
        srv6_tunnel_table_[srv6_source].nexthops.insert(nhkey);
    }
    else
    {
        srv6_tunnel_table_[srv6_source].nexthops.erase(nhkey);
    }
}

size_t Srv6Orch::srv6TunnelNexthopSize(const string srv6_source)
{
    return srv6_tunnel_table_[srv6_source].nexthops.size();
}

bool Srv6Orch::createSrv6Tunnel(const string srv6_source)
{
    SWSS_LOG_ENTER();
    vector<sai_attribute_t> tunnel_attrs;
    sai_attribute_t attr;
    sai_status_t status;
    sai_object_id_t tunnel_id;

    if (srv6_tunnel_table_.find(srv6_source) != srv6_tunnel_table_.end())
    {
        SWSS_LOG_INFO("Tunnel exists for the source %s", srv6_source.c_str());
        return true;
    }

    SWSS_LOG_INFO("Create tunnel for the source %s", srv6_source.c_str());
    attr.id = SAI_TUNNEL_ATTR_TYPE;
    attr.value.s32 = SAI_TUNNEL_TYPE_SRV6;
    tunnel_attrs.push_back(attr);
    attr.id = SAI_TUNNEL_ATTR_UNDERLAY_INTERFACE;
    attr.value.oid = gUnderlayIfId;
    tunnel_attrs.push_back(attr);

    IpAddress src_ip(srv6_source);
    sai_ip_address_t ipaddr;
    ipaddr.addr_family = SAI_IP_ADDR_FAMILY_IPV6;
    memcpy(ipaddr.addr.ip6, src_ip.getV6Addr(), sizeof(ipaddr.addr.ip6));
    attr.id = SAI_TUNNEL_ATTR_ENCAP_SRC_IP;
    attr.value.ipaddr = ipaddr;
    tunnel_attrs.push_back(attr);

    status = sai_tunnel_api->create_tunnel(&tunnel_id, gSwitchId, (uint32_t)tunnel_attrs.size(), tunnel_attrs.data());
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to create tunnel for %s", srv6_source.c_str());
        return false;
    }
    srv6_tunnel_table_[srv6_source].tunnel_object_id = tunnel_id;
    return true;
}

bool Srv6Orch::srv6NexthopExists(const NextHopKey &nhKey)
{
    SWSS_LOG_ENTER();
    if (srv6_nexthop_table_.find(nhKey) != srv6_nexthop_table_.end())
    {
        return true;
    }
    else
    {
        return false;
    }
}

bool Srv6Orch::removeSrv6Nexthops(const std::vector<NextHopGroupKey> &nhgv)
{
    SWSS_LOG_ENTER();

    // 1. remove vpn_sid first
    for (auto& it_nhg : nhgv)
    {
        for (auto &sr_nh : it_nhg.getNextHops())
        {
            if (sr_nh.isSrv6Vpn())
            {
                if (!deleteSrv6Vpn(sr_nh, getAggId(it_nhg)))
                {
                    SWSS_LOG_ERROR("Failed to delete SRV6 vpn %s", sr_nh.to_string(false, true).c_str());
                    return false;
                }
            }
        }
    }

    // 2. delete nexthop & prefix agg id
    for (auto& nhg : nhgv)
    {
        for (auto &sr_nh : nhg.getNextHops())
        {
            if (!deleteSrv6Nexthop(sr_nh))
            {
                SWSS_LOG_ERROR("Failed to delete SRV6 nexthop %s", sr_nh.to_string(false,true).c_str());
                return false;
            }
        }

        if (nhg.is_srv6_vpn())
        {
            deleteAggId(nhg);
        }
    }

    return true;
}

bool Srv6Orch::removeSrv6Nexthops(const NextHopGroupKey &nhg)
{
    SWSS_LOG_ENTER();

    for (auto &sr_nh : nhg.getNextHops())
    {
        string srv6_source, segname;
        sai_status_t status = SAI_STATUS_SUCCESS;
        srv6_source = sr_nh.srv6_source;
        segname = sr_nh.srv6_segment;

        SWSS_LOG_NOTICE("SRV6 Nexthop %s refcount %d", sr_nh.to_string(false,true).c_str(), m_neighOrch->getNextHopRefCount(sr_nh));
        if (m_neighOrch->getNextHopRefCount(sr_nh) == 0)
        {
            status = sai_next_hop_api->remove_next_hop(srv6_nexthop_table_[sr_nh]);
            if (status != SAI_STATUS_SUCCESS)
            {
                SWSS_LOG_ERROR("Failed to remove SRV6 nexthop %s", sr_nh.to_string(false,true).c_str());
                return false;
            }

            /* Update nexthop in SID table after deleting the nexthop */
            SWSS_LOG_INFO("Seg %s nexthop refcount %zu",
                      segname.c_str(),
                      sid_table_[segname].nexthops.size());
            if (sid_table_[segname].nexthops.find(sr_nh) != sid_table_[segname].nexthops.end())
            {
                sid_table_[segname].nexthops.erase(sr_nh);
            }
            m_neighOrch->updateSrv6Nexthop(sr_nh, 0);
            srv6_nexthop_table_.erase(sr_nh);

            /* Delete NH from the tunnel map */
            SWSS_LOG_INFO("Delete NH %s from tunnel map",
                sr_nh.to_string(false, true).c_str());
            srv6TunnelUpdateNexthops(srv6_source, sr_nh, false);
        }

        size_t tunnel_nhs = srv6TunnelNexthopSize(srv6_source);
        if (tunnel_nhs == 0)
        {
            status = sai_tunnel_api->remove_tunnel(srv6_tunnel_table_[srv6_source].tunnel_object_id);
            if (status != SAI_STATUS_SUCCESS)
            {
                SWSS_LOG_ERROR("Failed to remove SRV6 tunnel object for source %s", srv6_source.c_str());
                return false;
            }
            srv6_tunnel_table_.erase(srv6_source);
        }
        else
        {
            SWSS_LOG_INFO("Nexthops referencing this tunnel object %s: %zu", srv6_source.c_str(),tunnel_nhs);
        }
    }
    return true;
}

bool Srv6Orch::createSrv6Nexthop(const NextHopKey &nh)
{
    SWSS_LOG_ENTER();
    string srv6_segment = nh.srv6_segment;
    string srv6_source = nh.srv6_source;

    if (srv6NexthopExists(nh))
    {
        SWSS_LOG_INFO("SRV6 nexthop already created for %s", nh.to_string(false,true).c_str());
        return true;
    }

    sai_object_id_t srv6_object_id;
    sai_object_id_t srv6_tunnel_id;

    if (srv6_segment == "")
    {
        srv6_object_id = SAI_NULL_OBJECT_ID;
    }
    else
    {
        srv6_object_id = sid_table_[srv6_segment].sid_object_id;
    }

    if (nh.ip_address.to_string() == "0.0.0.0")
    {
        srv6_tunnel_id = srv6_tunnel_table_[srv6_source].tunnel_object_id;
    }
    else
    {
        P2pTunnelKey k;
        k.src_ip = nh.srv6_source;
        k.endpoint = nh.ip_address.to_string();
        srv6_tunnel_id = srv6_p2p_tunnel_table_[k].tunnel_id;
    }

    SWSS_LOG_INFO("Create srv6 nh for tunnel src %s with seg %s", srv6_source.c_str(), srv6_segment.c_str());
    vector<sai_attribute_t> nh_attrs;
    sai_object_id_t nexthop_id;
    sai_attribute_t attr;
    sai_status_t status;

    attr.id = SAI_NEXT_HOP_ATTR_TYPE;
    attr.value.s32 = SAI_NEXT_HOP_TYPE_SRV6_SIDLIST;
    nh_attrs.push_back(attr);

    attr.id = SAI_NEXT_HOP_ATTR_SRV6_SIDLIST_ID;
    attr.value.oid = srv6_object_id;
    nh_attrs.push_back(attr);

    attr.id = SAI_NEXT_HOP_ATTR_TUNNEL_ID;
    attr.value.oid = srv6_tunnel_id;
    nh_attrs.push_back(attr);

    status = sai_next_hop_api->create_next_hop(&nexthop_id, gSwitchId,
                                                (uint32_t)nh_attrs.size(),
                                                nh_attrs.data());
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to create srv6 nexthop for %s", nh.to_string(false,true).c_str());
        return false;
    }
    m_neighOrch->updateSrv6Nexthop(nh, nexthop_id);
    srv6_nexthop_table_[nh] = nexthop_id;
    if (srv6_segment != "")
    {
        sid_table_[srv6_segment].nexthops.insert(nh);
    }

    if (nh.ip_address.to_string() == "0.0.0.0")
    {
        srv6TunnelUpdateNexthops(srv6_source, nh, true);
    }
    else
    {
        P2pTunnelKey k;
        k.src_ip = nh.srv6_source;
        k.endpoint = nh.ip_address.to_string();
        srv6P2ptunnelUpdateNexthops(k, nh, true);
    }
    return true;
}

bool Srv6Orch::deleteSrv6Nexthop(const NextHopKey &nh)
{
    SWSS_LOG_ENTER();

    sai_status_t status = SAI_STATUS_SUCCESS;

    if (!srv6NexthopExists(nh))
    {
        return true;
    }

    SWSS_LOG_DEBUG("SRV6 Nexthop %s refcount %d", nh.to_string(false,true).c_str(), m_neighOrch->getNextHopRefCount(nh));
    if (m_neighOrch->getNextHopRefCount(nh) == 0)
    {
        status = sai_next_hop_api->remove_next_hop(srv6_nexthop_table_[nh]);
        if (status != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_ERROR("Failed to remove SRV6 nexthop %s", nh.to_string(false,true).c_str());
            return false;
        }

        /* Decrease srv6 segment reference */
        if (nh.srv6_segment != "")
        {
            /* Update nexthop in SID table after deleting the nexthop */
            SWSS_LOG_INFO("Seg %s nexthop refcount %zu",
                      nh.srv6_segment.c_str(),
                      sid_table_[nh.srv6_segment].nexthops.size());
            if (sid_table_[nh.srv6_segment].nexthops.find(nh) != sid_table_[nh.srv6_segment].nexthops.end())
            {
                sid_table_[nh.srv6_segment].nexthops.erase(nh);
            }
        }
        m_neighOrch->updateSrv6Nexthop(nh, 0);
        srv6_nexthop_table_.erase(nh);

        /* Delete NH from the tunnel map */
        SWSS_LOG_INFO("Delete NH %s from tunnel map",
            nh.to_string(false, true).c_str());

        if (nh.ip_address.to_string() == "0.0.0.0")
        {
            string srv6_source = nh.srv6_source;
            srv6TunnelUpdateNexthops(srv6_source, nh, false);
            size_t tunnel_nhs = srv6TunnelNexthopSize(srv6_source);
            if (tunnel_nhs == 0)
            {
                status = sai_tunnel_api->remove_tunnel(srv6_tunnel_table_[srv6_source].tunnel_object_id);
                if (status != SAI_STATUS_SUCCESS)
                {
                    SWSS_LOG_ERROR("Failed to remove SRV6 tunnel object for source %s", srv6_source.c_str());
                    return false;
                }
                srv6_tunnel_table_.erase(srv6_source);
            }
            else
            {
                SWSS_LOG_INFO("Nexthops referencing this tunnel object %s: %zu", srv6_source.c_str(),tunnel_nhs);
            }
        }
        else
        {
            P2pTunnelKey k;
            k.src_ip = nh.srv6_source;
            k.endpoint = nh.ip_address.to_string();
            srv6P2ptunnelUpdateNexthops(k, nh, false);

            size_t tunnel_nhs = srv6P2pTunnelNexthopSize(k);
            if (tunnel_nhs == 0)
            {
                if (!deleteSrv6P2pTunnel(k))
                {
                    SWSS_LOG_ERROR("Failed to remove SRV6 p2p tunnel object for src %s, dst %s,", k.src_ip.c_str(), k.endpoint.c_str());
                    return false;
                }
                srv6_p2p_tunnel_table_.erase(k);
            }
            else
            {
                SWSS_LOG_INFO("Nexthops referencing this srv6 p2p tunnel object src %s, dst %s : %zu", k.src_ip.c_str(), k.endpoint.c_str(), tunnel_nhs);
            }
        }
    }


    return true;
}

bool Srv6Orch::srv6Nexthops(const NextHopGroupKey &nhgKey, sai_object_id_t &nexthop_id)
{
    SWSS_LOG_ENTER();
    set<NextHopKey> nexthops = nhgKey.getNextHops();
    string srv6_source;
    string srv6_segment;

    for (auto nh : nexthops)
    {
        // 1. create tunnel
        if (nh.ip_address.to_string() == "0.0.0.0")
        {
            // create srv6 tunnel
            srv6_source = nh.srv6_source;
            if (!createSrv6Tunnel(srv6_source))
            {
                SWSS_LOG_ERROR("Failed to create tunnel for source %s", srv6_source.c_str());
                return false;
            }
        }
        else
        {
            // create p2p tunnel
            P2pTunnelKey k;
            k.src_ip = nh.srv6_source;
            k.endpoint = nh.ip_address.to_string();

            if (!createSrv6P2pTunnel(k))
            {
                SWSS_LOG_ERROR("Failed to create SRV6 p2p tunnel %s", nh.to_string(false, true).c_str());
                return false;
            }
        }

        // 2. create vpn if need
        if (nh.isSrv6Vpn())
        {
            if (!createSrv6Vpn(nh, getAggId(nhgKey)))
            {
                SWSS_LOG_ERROR("Failed to create SRV6 vpn %s", nh.to_string(false, true).c_str());
                return false;
            }
        }

        // 3. create nexthop
        if (!createSrv6Nexthop(nh))
        {
            SWSS_LOG_ERROR("Failed to create SRV6 nexthop %s", nh.to_string(false,true).c_str());
            return false;
        }
    }

    if (nhgKey.getSize() == 1)
    {
        NextHopKey nhkey = *nhgKey.getNextHops().begin();
        nexthop_id = srv6_nexthop_table_[nhkey];
    }
    return true;
}

bool Srv6Orch::createUpdateSidList(const string sid_name, const string sid_list, const string sidlist_type)
{
    SWSS_LOG_ENTER();
    bool exists = (sid_table_.find(sid_name) != sid_table_.end());
    sai_segment_list_t segment_list;
    vector<string>sid_ips = tokenize(sid_list, SID_LIST_DELIMITER);
    sai_object_id_t segment_oid;
    segment_list.count = (uint32_t)sid_ips.size();
    if (segment_list.count == 0)
    {
        SWSS_LOG_ERROR("segment list count is zero, skip");
        return true;
    }
    SWSS_LOG_INFO("Segment count %d", segment_list.count);
    segment_list.list = new sai_ip6_t[segment_list.count];
    uint32_t index = 0;

    for (string ip_str : sid_ips)
    {
        IpPrefix ip(ip_str);
        SWSS_LOG_INFO("Segment %s, count %d", ip.to_string().c_str(), segment_list.count);
        memcpy(segment_list.list[index++], ip.getIp().getV6Addr(), 16);
    }
    sai_attribute_t attr;
    sai_status_t status;
    if (!exists)
    {
        /* Create sidlist object with list of ipv6 prefixes */
        SWSS_LOG_INFO("Create SID list");
        vector<sai_attribute_t> attributes;
        attr.id = SAI_SRV6_SIDLIST_ATTR_SEGMENT_LIST;
        attr.value.segmentlist.list = segment_list.list;
        attr.value.segmentlist.count = segment_list.count;
        attributes.push_back(attr);

        attr.id = SAI_SRV6_SIDLIST_ATTR_TYPE;
        if (sidlist_type_map.find(sidlist_type) == sidlist_type_map.end())
        {
            SWSS_LOG_INFO("Use default sidlist type: ENCAPS_RED");
            attr.value.s32 = SAI_SRV6_SIDLIST_TYPE_ENCAPS_RED;
        }
        else
        {
            SWSS_LOG_INFO("sidlist type: %s", sidlist_type.c_str());
            attr.value.s32 = sidlist_type_map.at(sidlist_type);
        }
        attributes.push_back(attr);
        status = sai_srv6_api->create_srv6_sidlist(&segment_oid, gSwitchId, (uint32_t) attributes.size(), attributes.data());
        if (status != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_ERROR("Failed to create srv6 sidlist object, rv %d", status);
            return false;
        }
        sid_table_[sid_name].sid_object_id = segment_oid;
    }
    else
    {
        SWSS_LOG_INFO("Set SID list");

        /* Update sidlist object with new set of ipv6 addresses */
        attr.id = SAI_SRV6_SIDLIST_ATTR_SEGMENT_LIST;
        attr.value.segmentlist.list = segment_list.list;
        attr.value.segmentlist.count = segment_list.count;
        segment_oid = (sid_table_.find(sid_name)->second).sid_object_id;
        status = sai_srv6_api->set_srv6_sidlist_attribute(segment_oid, &attr);
        if (status != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_ERROR("Failed to set srv6 sidlist object with new segments, rv %d", status);
            return false;
        }
    }
    delete segment_list.list;
    return true;
}

bool Srv6Orch::deleteSidList(const string sid_name)
{
    SWSS_LOG_ENTER();
    sai_status_t status = SAI_STATUS_SUCCESS;
    if (sid_table_.find(sid_name) == sid_table_.end())
    {
        SWSS_LOG_ERROR("segment name %s doesn't exist", sid_name.c_str());
        return false;
    }

    if (sid_table_[sid_name].nexthops.size() > 1)
    {
        SWSS_LOG_NOTICE("segment object %s referenced by other nexthops: count %zu, not deleting",
                      sid_name.c_str(), sid_table_[sid_name].nexthops.size());
        return false;
    }
    SWSS_LOG_INFO("Remove sid list, segname %s", sid_name.c_str());
    status = sai_srv6_api->remove_srv6_sidlist(sid_table_[sid_name].sid_object_id);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to delete SRV6 sidlist object for %s", sid_name.c_str());
        return false;
    }
    sid_table_.erase(sid_name);
    return true;
}

void Srv6Orch::doTaskSidTable(const KeyOpFieldsValuesTuple & tuple)
{
    SWSS_LOG_ENTER();
    string sid_name = kfvKey(tuple);
    string op = kfvOp(tuple);
    string sid_list, sidlist_type;

    for (auto i : kfvFieldsValues(tuple))
    {
        if (fvField(i) == "path")
        {
          sid_list = fvValue(i);
        }
        if (fvField(i) == "type")
        {
          sidlist_type = fvValue(i);
        }
    }
    if (op == SET_COMMAND)
    {
        if (!createUpdateSidList(sid_name, sid_list, sidlist_type))
        {
          SWSS_LOG_ERROR("Failed to process sid %s", sid_name.c_str());
        }
    }
    else if (op == DEL_COMMAND)
    {
        if (!deleteSidList(sid_name))
        {
            SWSS_LOG_ERROR("Failed to delete sid %s", sid_name.c_str());
        }
    } else {
        SWSS_LOG_ERROR("Invalid command");
    }
}

bool Srv6Orch::mySidExists(string my_sid_string)
{
    if (srv6_my_sid_table_.find(my_sid_string) != srv6_my_sid_table_.end())
    {
        return true;
    }
    return false;
}

bool Srv6Orch::sidEntryEndpointBehavior(string action, sai_my_sid_entry_endpoint_behavior_t &end_behavior,
                                        sai_my_sid_entry_endpoint_behavior_flavor_t &end_flavor)
{
    if (end_behavior_map.find(action) == end_behavior_map.end())
    {
        SWSS_LOG_ERROR("Invalid endpoint behavior function");
        return false;
    }
    end_behavior = end_behavior_map.at(action);

    if (end_flavor_map.find(action) != end_flavor_map.end())
    {
        end_flavor = end_flavor_map.at(action);
    }

    return true;
}

bool Srv6Orch::mySidVrfRequired(const sai_my_sid_entry_endpoint_behavior_t end_behavior)
{
    if (end_behavior == SAI_MY_SID_ENTRY_ENDPOINT_BEHAVIOR_T ||
        end_behavior == SAI_MY_SID_ENTRY_ENDPOINT_BEHAVIOR_DT4 ||
        end_behavior == SAI_MY_SID_ENTRY_ENDPOINT_BEHAVIOR_DT6 ||
        end_behavior == SAI_MY_SID_ENTRY_ENDPOINT_BEHAVIOR_DT46)
    {
      return true;
    }
    return false;
}

bool Srv6Orch::createUpdateMysidEntry(string my_sid_string, const string dt_vrf, const string end_action)
{
    SWSS_LOG_ENTER();
    vector<sai_attribute_t> attributes;
    sai_attribute_t attr;
    string key_string = my_sid_string;
    sai_my_sid_entry_endpoint_behavior_t end_behavior;
    sai_my_sid_entry_endpoint_behavior_flavor_t end_flavor = SAI_MY_SID_ENTRY_ENDPOINT_BEHAVIOR_FLAVOR_PSP_AND_USD;

    bool entry_exists = false;
    if (mySidExists(key_string))
    {
        entry_exists = true;
    }

    sai_my_sid_entry_t my_sid_entry;
    if (!entry_exists)
    {
        vector<string>keys = tokenize(my_sid_string, MY_SID_KEY_DELIMITER);

        my_sid_entry.vr_id = gVirtualRouterId;
        my_sid_entry.switch_id = gSwitchId;
        my_sid_entry.locator_block_len = (uint8_t)stoi(keys[0]);
        my_sid_entry.locator_node_len = (uint8_t)stoi(keys[1]);
        my_sid_entry.function_len = (uint8_t)stoi(keys[2]);
        my_sid_entry.args_len = (uint8_t)stoi(keys[3]);
        size_t keylen = keys[0].length()+keys[1].length()+keys[2].length()+keys[3].length() + 4;
        my_sid_string.erase(0, keylen);
        string my_sid = my_sid_string;
        SWSS_LOG_INFO("MY SID STRING %s", my_sid.c_str());
        IpAddress address(my_sid);
        memcpy(my_sid_entry.sid, address.getV6Addr(), sizeof(my_sid_entry.sid));
    }
    else
    {
        my_sid_entry = srv6_my_sid_table_[key_string].entry;
    }

    SWSS_LOG_INFO("MySid: sid %s, action %s, vrf %s, block %d, node %d, func %d, arg %d dt_vrf %s",
      my_sid_string.c_str(), end_action.c_str(), dt_vrf.c_str(),my_sid_entry.locator_block_len, my_sid_entry.locator_node_len,
      my_sid_entry.function_len, my_sid_entry.args_len, dt_vrf.c_str());

    if (sidEntryEndpointBehavior(end_action, end_behavior, end_flavor) != true)
    {
        SWSS_LOG_ERROR("Invalid my_sid action %s", end_action.c_str());
        return false;
    }
    sai_attribute_t vrf_attr;
    bool vrf_update = false;
    if (mySidVrfRequired(end_behavior))
    {
        sai_object_id_t dt_vrf_id;
        SWSS_LOG_INFO("DT VRF name %s", dt_vrf.c_str());
        if (dt_vrf == "default")
        {
            dt_vrf_id = gVirtualRouterId;
        }
        else if (m_vrfOrch->isVRFexists(dt_vrf))
        {
            SWSS_LOG_INFO("VRF %s exists in DB", dt_vrf.c_str());
            dt_vrf_id = m_vrfOrch->getVRFid(dt_vrf);
            if(dt_vrf_id == SAI_NULL_OBJECT_ID)
            {
              SWSS_LOG_ERROR("VRF object not created for DT VRF %s", dt_vrf.c_str());
              return false;
            }
        }
        else
        {
            SWSS_LOG_ERROR("VRF %s doesn't exist in DB", dt_vrf.c_str());
            return false;
        }
        vrf_attr.id = SAI_MY_SID_ENTRY_ATTR_VRF;
        vrf_attr.value.oid = dt_vrf_id;
        attributes.push_back(vrf_attr);
        vrf_update = true;
    }
    attr.id = SAI_MY_SID_ENTRY_ATTR_ENDPOINT_BEHAVIOR;
    attr.value.s32 = end_behavior;
    attributes.push_back(attr);

    attr.id = SAI_MY_SID_ENTRY_ATTR_ENDPOINT_BEHAVIOR_FLAVOR;
    attr.value.s32 = end_flavor;
    attributes.push_back(attr);

    sai_status_t status = SAI_STATUS_SUCCESS;
    if (!entry_exists)
    {
        status = sai_srv6_api->create_my_sid_entry(&my_sid_entry, (uint32_t) attributes.size(), attributes.data());
        if (status != SAI_STATUS_SUCCESS)
        {
          SWSS_LOG_ERROR("Failed to create my_sid entry %s, rv %d", key_string.c_str(), status);
          return false;
        }
        gCrmOrch->incCrmResUsedCounter(CrmResourceType::CRM_SRV6_MY_SID_ENTRY);
    }
    else
    {
        if (vrf_update)
        {
            status = sai_srv6_api->set_my_sid_entry_attribute(&my_sid_entry, &vrf_attr);
            if(status != SAI_STATUS_SUCCESS)
            {
                SWSS_LOG_ERROR("Failed to update VRF to my_sid_entry %s, rv %d", key_string.c_str(), status);
                return false;
            }
        }
    }
    SWSS_LOG_INFO("Store keystring %s in cache", key_string.c_str());
    if(vrf_update)
    {
        m_vrfOrch->increaseVrfRefCount(dt_vrf);
        srv6_my_sid_table_[key_string].endVrfString = dt_vrf;
    }
    srv6_my_sid_table_[key_string].endBehavior = end_behavior;
    srv6_my_sid_table_[key_string].entry = my_sid_entry;

    return true;
}

bool Srv6Orch::deleteMysidEntry(const string my_sid_string)
{
    sai_status_t status = SAI_STATUS_SUCCESS;
    if (!mySidExists(my_sid_string))
    {
        SWSS_LOG_ERROR("My_sid_entry doesn't exist for %s", my_sid_string.c_str());
        return false;
    }
    sai_my_sid_entry_t my_sid_entry = srv6_my_sid_table_[my_sid_string].entry;

    SWSS_LOG_NOTICE("MySid Delete: sid %s", my_sid_string.c_str());
    status = sai_srv6_api->remove_my_sid_entry(&my_sid_entry);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to delete my_sid entry rv %d", status);
        return false;
    }
    gCrmOrch->decCrmResUsedCounter(CrmResourceType::CRM_SRV6_MY_SID_ENTRY);

    /* Decrease VRF refcount */
    if (mySidVrfRequired(srv6_my_sid_table_[my_sid_string].endBehavior))
    {
        m_vrfOrch->decreaseVrfRefCount(srv6_my_sid_table_[my_sid_string].endVrfString);
    }
    srv6_my_sid_table_.erase(my_sid_string);
    return true;
}

uint32_t Srv6Orch::getAggId(const NextHopGroupKey &nhg)
{
    SWSS_LOG_ENTER();
    static uint32_t g_agg_id = 1;
    uint32_t agg_id;
    string agg_id_key = nhg.get_srv6_vpn_key();

    if (srv6_prefix_agg_id_table_.find(agg_id_key) != srv6_prefix_agg_id_table_.end()) {
        agg_id = srv6_prefix_agg_id_table_[agg_id_key].prefix_agg_id;
        SWSS_LOG_INFO("Agg id already exist, agg_id_key: %s, agg_id %u", agg_id_key.c_str(), agg_id);
    } else {
        while (srv6_prefix_agg_id_set_.find(g_agg_id) != srv6_prefix_agg_id_set_.end()) {
            SWSS_LOG_INFO("Agg id %d is busy, try next", g_agg_id);
            g_agg_id++;
            // restart with 1 if flip
            if (g_agg_id == 0) {
                g_agg_id = 1;
            }
        }
        agg_id = g_agg_id;
        srv6_prefix_agg_id_table_[agg_id_key].prefix_agg_id = g_agg_id;
        // initual ref_count with 0, will be added in increasePrefixAggIdRefCount() later
        srv6_prefix_agg_id_table_[agg_id_key].ref_count = 0;
        srv6_prefix_agg_id_set_.insert(g_agg_id);
        SWSS_LOG_INFO("Agg id not exist, create agg_id_key: %s, agg_id %u", agg_id_key.c_str(), agg_id);
    }

    return agg_id;
}

void Srv6Orch::deleteAggId(const NextHopGroupKey &nhg)
{
    SWSS_LOG_ENTER();

    string agg_id_key = nhg.get_srv6_vpn_key();
    uint32_t agg_id;

    if (srv6_prefix_agg_id_table_.find(agg_id_key) == srv6_prefix_agg_id_table_.end()) {
        return;
    }

    agg_id = srv6_prefix_agg_id_table_[agg_id_key].prefix_agg_id;
    if (srv6_prefix_agg_id_table_[agg_id_key].ref_count == 0) {
        srv6_prefix_agg_id_table_.erase(agg_id_key);
        srv6_prefix_agg_id_set_.erase(agg_id);
        SWSS_LOG_INFO("Delete Agg id %d, agg_id_key: %s", agg_id, agg_id_key.c_str());
    }
    else
    {
        SWSS_LOG_INFO("Referencing this prefix agg id %u : %u", agg_id, srv6_prefix_agg_id_table_[agg_id_key].ref_count);
    }
}

void Srv6Orch::increasePrefixAggIdRefCount(const NextHopGroupKey &nhg)
{
    SWSS_LOG_ENTER();
    string k = nhg.get_srv6_vpn_key();
    if (srv6_prefix_agg_id_table_.find(k) == srv6_prefix_agg_id_table_.end())
    {
        SWSS_LOG_ERROR("Unexpected prefix agg refcount increase for nexthop %s", nhg.to_string().c_str());
    }
    else
    {
        srv6_prefix_agg_id_table_[k].ref_count++;
    }
}

void Srv6Orch::decreasePrefixAggIdRefCount(const NextHopGroupKey &nhg)
{
    SWSS_LOG_ENTER();
    string k = nhg.get_srv6_vpn_key();
    if (srv6_prefix_agg_id_table_.find(k) == srv6_prefix_agg_id_table_.end())
    {
        SWSS_LOG_ERROR("Unexpected prefix agg refcount decrease for nexthop %s", nhg.to_string().c_str());
    }
    else
    {
        srv6_prefix_agg_id_table_[k].ref_count--;
    }

}

bool Srv6Orch::srv6P2pTunnelExists(const P2pTunnelKey &k)
{
    if (srv6_p2p_tunnel_table_.find(k) != srv6_p2p_tunnel_table_.end())
    {
        return true;
    }
    return false;
}

bool Srv6Orch::createSrv6P2pTunnel(const P2pTunnelKey &k)
{
    SWSS_LOG_ENTER();
    sai_status_t saistatus;
    sai_object_id_t srv6_tunnel_map_id;

    sai_attribute_t tunnel_map_attr;
    vector<sai_attribute_t> tunnel_map_attrs;

    if (srv6P2pTunnelExists(k)) {
        return true;
    }

    // 0. create tunnel map
    tunnel_map_attr.id = SAI_TUNNEL_MAP_ATTR_TYPE;
    tunnel_map_attr.value.u32 = SAI_TUNNEL_MAP_TYPE_PREFIX_AGG_ID_TO_SRV6_VPN_SID;
    tunnel_map_attrs.push_back(tunnel_map_attr);

    saistatus = sai_tunnel_api->create_tunnel_map(&srv6_tunnel_map_id, gSwitchId,
        (uint32_t)tunnel_map_attrs.size(), tunnel_map_attrs.data());
    if (saistatus != SAI_STATUS_SUCCESS) {
        SWSS_LOG_ERROR("Failed to create srv6 p2p tunnel map for src_ip: %s dst_ip: %s", k.src_ip.c_str(), k.endpoint.c_str());
        return false;
    }

    // 1. create tunnel
    sai_object_id_t tunnel_id;
    sai_attribute_t tunnel_attr;
    vector<sai_attribute_t> tunnel_attrs;
    sai_ip_address_t ipaddr;

    tunnel_attr.id = SAI_TUNNEL_ATTR_TYPE;
    tunnel_attr.value.s32 = SAI_TUNNEL_TYPE_SRV6;
    tunnel_attrs.push_back(tunnel_attr);

    IpAddress src_ip(k.src_ip);
    ipaddr.addr_family = SAI_IP_ADDR_FAMILY_IPV6;
    memcpy(ipaddr.addr.ip6, src_ip.getV6Addr(), sizeof(ipaddr.addr.ip6));
    tunnel_attr.id = SAI_TUNNEL_ATTR_ENCAP_SRC_IP;
    tunnel_attr.value.ipaddr = ipaddr;
    tunnel_attrs.push_back(tunnel_attr);

    tunnel_attr.id = SAI_TUNNEL_ATTR_UNDERLAY_INTERFACE;
    tunnel_attr.value.oid = gUnderlayIfId;
    tunnel_attrs.push_back(tunnel_attr);

    sai_object_id_t tunnel_map_list[1];
    tunnel_map_list[0] = srv6_tunnel_map_id;
    tunnel_attr.id = SAI_TUNNEL_ATTR_ENCAP_MAPPERS;
    tunnel_attr.value.objlist.count = 1;
    tunnel_attr.value.objlist.list = tunnel_map_list;
    tunnel_attrs.push_back(tunnel_attr);

    tunnel_attr.id = SAI_TUNNEL_ATTR_PEER_MODE;
    tunnel_attr.value.u32 = SAI_TUNNEL_PEER_MODE_P2P;
    tunnel_attrs.push_back(tunnel_attr);

    IpAddress dst_ip(k.endpoint);
    ipaddr.addr_family = SAI_IP_ADDR_FAMILY_IPV6;
    memcpy(ipaddr.addr.ip6, dst_ip.getV6Addr(), sizeof(ipaddr.addr.ip6));
    tunnel_attr.id = SAI_TUNNEL_ATTR_ENCAP_DST_IP;
    tunnel_attr.value.ipaddr = ipaddr;
    tunnel_attrs.push_back(tunnel_attr);

    saistatus = sai_tunnel_api->create_tunnel(
        &tunnel_id, gSwitchId, (uint32_t)tunnel_attrs.size(), tunnel_attrs.data());
    if (saistatus != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to create srv6 p2p tunnel for src ip: %s, dst ip: %s",
            k.src_ip.c_str(), k.endpoint.c_str());

        sai_tunnel_api->remove_tunnel_map(srv6_tunnel_map_id);
        return false;
    }

    srv6_p2p_tunnel_table_[k].tunnel_id = tunnel_id;
    srv6_p2p_tunnel_table_[k].tunnel_map_id = srv6_tunnel_map_id;
    return true;
}

bool Srv6Orch::deleteSrv6P2pTunnel(const P2pTunnelKey &k)
{
    if (srv6_p2p_tunnel_table_.find(k) == srv6_p2p_tunnel_table_.end())
    {
        return true;
    }

    sai_status_t status;

    // 0. remove tunnel
    status = sai_tunnel_api->remove_tunnel(srv6_p2p_tunnel_table_[k].tunnel_id);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to remove SRV6 p2p tunnel object for src_ip %s, dst_ip: %s", k.src_ip.c_str(), k.endpoint.c_str());
        return false;
    }

    // 1. remove tunnel map
    status = sai_tunnel_api->remove_tunnel_map(srv6_p2p_tunnel_table_[k].tunnel_map_id);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to remove SRV6 tunnel map object for src_ip %s, dst_ip: %s", k.src_ip.c_str(), k.endpoint.c_str());
        return false;
    }

    srv6_p2p_tunnel_table_.erase(k);
    return true;
}

void Srv6Orch::srv6P2ptunnelUpdateNexthops(const P2pTunnelKey &k, const NextHopKey &nhkey, bool insert)
{
    if (insert)
    {
        srv6_p2p_tunnel_table_[k].nexthops.insert(nhkey);
    }
    else
    {
        srv6_p2p_tunnel_table_[k].nexthops.erase(nhkey);
    }
}

size_t Srv6Orch::srv6P2pTunnelNexthopSize(const P2pTunnelKey &k)
{
    return srv6_p2p_tunnel_table_[k].nexthops.size();
}

bool Srv6Orch::createSrv6Vpn(const NextHopKey &nh, const uint32_t prefix_agg_id)
{
    SWSS_LOG_ENTER();

    sai_status_t status;

    Srv6TunnelMapEntryKey tmek;
    tmek.src_ip = nh.srv6_source;
    tmek.endpoint = nh.ip_address.to_string();
    tmek.vpn_sid = nh.srv6_vpn_sid;
    tmek.prefix_agg_id = prefix_agg_id;

    if (srv6_tunnel_map_entry_table_.find(tmek) != srv6_tunnel_map_entry_table_.end())
    {
        srv6_tunnel_map_entry_table_[tmek].ref_count++;
        return true;
    }

    P2pTunnelKey k;
    k.src_ip = nh.srv6_source;
    k.endpoint = nh.ip_address.to_string();

    // Accessibility is guaranteed by external
    sai_object_id_t tunnel_map_id = srv6_p2p_tunnel_table_[k].tunnel_map_id;

    // 1. create vpn tunnel_map entry
    sai_attribute_t tunnel_map_entry_attr;
    vector<sai_attribute_t> tunnel_map_entry_attrs;
    sai_object_id_t tunnel_entry_id;

    tunnel_map_entry_attr.id = SAI_TUNNEL_MAP_ENTRY_ATTR_TUNNEL_MAP_TYPE;
    tunnel_map_entry_attr.value.u32 = SAI_TUNNEL_MAP_TYPE_PREFIX_AGG_ID_TO_SRV6_VPN_SID;
    tunnel_map_entry_attrs.push_back(tunnel_map_entry_attr);

    tunnel_map_entry_attr.id = SAI_TUNNEL_MAP_ENTRY_ATTR_TUNNEL_MAP;
    tunnel_map_entry_attr.value.oid = tunnel_map_id;
    tunnel_map_entry_attrs.push_back(tunnel_map_entry_attr);

    tunnel_map_entry_attr.id = SAI_TUNNEL_MAP_ENTRY_ATTR_PREFIX_AGG_ID_KEY;
    tunnel_map_entry_attr.value.u32 = prefix_agg_id;
    tunnel_map_entry_attrs.push_back(tunnel_map_entry_attr);

    IpAddress vpn_sid(nh.srv6_vpn_sid);
    tunnel_map_entry_attr.id = SAI_TUNNEL_MAP_ENTRY_ATTR_SRV6_VPN_SID_VALUE;
    memcpy(tunnel_map_entry_attr.value.ip6, vpn_sid.getV6Addr(), sizeof(sai_ip6_t));
    tunnel_map_entry_attrs.push_back(tunnel_map_entry_attr);

    status = sai_tunnel_api->create_tunnel_map_entry(&tunnel_entry_id, gSwitchId,
                                                (uint32_t)tunnel_map_entry_attrs.size(),
                                                tunnel_map_entry_attrs.data());
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to create vpn tunnel_map entry for vpn_sid: %s", nh.srv6_vpn_sid.c_str());
        return false;
    }

    // add reference for tunnel map entry
    srv6_tunnel_map_entry_table_[tmek].tunnel_map_entry_id = tunnel_entry_id;
    srv6_tunnel_map_entry_table_[tmek].ref_count = 1;
    return true;
}

bool Srv6Orch::deleteSrv6Vpn(const NextHopKey &nh, const uint32_t prefix_agg_id)
{
    SWSS_LOG_ENTER();
    sai_status_t status;

    // 1. remove tunnel_map entry if need
    sai_object_id_t tunnel_entry_id;

    Srv6TunnelMapEntryKey tmek;
    tmek.src_ip = nh.srv6_source;
    tmek.endpoint = nh.ip_address.to_string();
    tmek.vpn_sid = nh.srv6_vpn_sid;
    tmek.prefix_agg_id = prefix_agg_id;

    if (srv6_tunnel_map_entry_table_.find(tmek) == srv6_tunnel_map_entry_table_.end())
    {
        return true;
    }

    srv6_tunnel_map_entry_table_[tmek].ref_count--;
    if (srv6_tunnel_map_entry_table_[tmek].ref_count == 0)
    {
        tunnel_entry_id = srv6_tunnel_map_entry_table_[tmek].tunnel_map_entry_id;
        status = sai_tunnel_api->remove_tunnel_map_entry(tunnel_entry_id);
        if (status != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_ERROR("Failed to remove nexthop tunnel map entry %s", nh.to_string(false, true).c_str());
            return false;
        }
        srv6_tunnel_map_entry_table_.erase(tmek);
    }
    else
    {
        SWSS_LOG_INFO("Nexthops referencing this tunnel map entry srv_ip %s, endpoint %s, vpn_sid %s, prefix_agg_id %u : %u",
            tmek.src_ip.c_str(),
            tmek.endpoint.c_str(),
            tmek.vpn_sid.c_str(),
            tmek.prefix_agg_id,
            srv6_tunnel_map_entry_table_[tmek].ref_count);
    }
    return true;
}

void Srv6Orch::doTaskMySidTable(const KeyOpFieldsValuesTuple & tuple)
{
    SWSS_LOG_ENTER();
    string op = kfvOp(tuple);
    string end_action, dt_vrf;

    /* Key for mySid : block_len:node_len:function_len:args_len:sid-ip */
    string keyString = kfvKey(tuple);

    for (auto i : kfvFieldsValues(tuple))
    {
        if (fvField(i) == "action")
        {
          end_action = fvValue(i);
        }
        if(fvField(i) == "vrf")
        {
          dt_vrf = fvValue(i);
        }
    }
    if (op == SET_COMMAND)
    {
        if(!createUpdateMysidEntry(keyString, dt_vrf, end_action))
        {
          SWSS_LOG_ERROR("Failed to create/update my_sid entry for sid %s", keyString.c_str());
          return;
        }
    }
    else if(op == DEL_COMMAND)
    {
        if(!deleteMysidEntry(keyString))
        {
          SWSS_LOG_ERROR("Failed to delete my_sid entry for sid %s", keyString.c_str());
          return;
        }
    }
    else
    {
        SWSS_LOG_ERROR("Invalid command");
    }
}

void Srv6Orch::doTask(Consumer &consumer)
{
    SWSS_LOG_ENTER();
    const string &table_name = consumer.getTableName();
    auto it = consumer.m_toSync.begin();
    while(it != consumer.m_toSync.end())
    {
        auto t = it->second;
        SWSS_LOG_INFO("table name : %s",table_name.c_str());
        if (table_name == APP_SRV6_SID_LIST_TABLE_NAME)
        {
            doTaskSidTable(t);
        }
        else if (table_name == APP_SRV6_MY_SID_TABLE_NAME)
        {
            doTaskMySidTable(t);
        }
        else
        {
            SWSS_LOG_ERROR("Unknown table : %s",table_name.c_str());
        }
        consumer.m_toSync.erase(it++);
    }
}
