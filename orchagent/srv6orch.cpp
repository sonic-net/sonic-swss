#include <iostream>
#include <sstream>

#include "routeorch.h"
#include "logger.h"
#include "srv6orch.h"
#include "sai_serialize.h"

using namespace std;
using namespace swss;

extern sai_object_id_t gSwitchId;
extern sai_object_id_t  gVirtualRouterId;
extern sai_object_id_t  gUnderlayIfId;
extern sai_srv6_api_t* sai_srv6_api;
extern sai_tunnel_api_t* sai_tunnel_api;
extern sai_next_hop_api_t* sai_next_hop_api;

extern RouteOrch *gRouteOrch;

void Srv6Orch::srv6TunnelUpdateNexthops(const string srv6_source, const NextHopKey nhkey, bool insert)
{
    if(insert)
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

    if(srv6_tunnel_table_.find(srv6_source) != srv6_tunnel_table_.end())
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
    if(status != SAI_STATUS_SUCCESS)
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
    if(srv6_nexthop_table_.find(nhKey) != srv6_nexthop_table_.end())
    {
        return true;
    }
    else
    {
        return false;
    }
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
        if(m_neighOrch->getNextHopRefCount(sr_nh) == 0)
        {
            status = sai_next_hop_api->remove_next_hop(srv6_nexthop_table_[sr_nh]);
            if(status != SAI_STATUS_SUCCESS)
            {
                SWSS_LOG_ERROR("Failed to remove SRV6 nexthop %s", sr_nh.to_string(false,true).c_str());
                return false;
            }
            // Update NH ref_count of SID list
            SWSS_LOG_NOTICE("Seg %s nexthop refcount %ld",
                      segname.c_str(),
                      sid_table_[segname].nexthops.size());
            if (sid_table_[segname].nexthops.find(sr_nh) != sid_table_[segname].nexthops.end())
            {
                sid_table_[segname].nexthops.erase(sr_nh);
            }
            m_neighOrch->updateSrv6Nexthop(sr_nh, 0);
            srv6_nexthop_table_.erase(sr_nh);
            // Delete NH from the tunnel map.
            SWSS_LOG_INFO("Delete NH %s from tunnel map",
                sr_nh.to_string(false, true).c_str());
            srv6TunnelUpdateNexthops(srv6_source, sr_nh, false);
        }

        size_t tunnel_nhs = srv6TunnelNexthopSize(srv6_source);
        if(tunnel_nhs == 0)
        {
            status = sai_tunnel_api->remove_tunnel(srv6_tunnel_table_[srv6_source].tunnel_object_id);
            if(status != SAI_STATUS_SUCCESS)
            {
                SWSS_LOG_ERROR("Failed to remove SRV6 tunnel object for source %s", srv6_source.c_str());
                return false;
            }
            srv6_tunnel_table_.erase(srv6_source);
        }
        else
        {
            SWSS_LOG_INFO("Nexthops referencing this tunnel object %s: %ld", srv6_source.c_str(),tunnel_nhs);
        }
    }
    return true;
}

bool Srv6Orch::createSrv6Nexthop(const NextHopKey &nh)
{
    SWSS_LOG_ENTER();
    string srv6_segment = nh.srv6_segment;
    string srv6_source = nh.srv6_source;

    if(srv6NexthopExists(nh))
    {
        SWSS_LOG_INFO("SRV6 nexthop already created for %s", nh.to_string(false,true).c_str());
        return true;
    }
    sai_object_id_t srv6_object_id = sid_table_[srv6_segment].sid_object_id;
    sai_object_id_t srv6_tunnel_id = srv6_tunnel_table_[srv6_source].tunnel_object_id;

    if(srv6_object_id == SAI_NULL_OBJECT_ID)
    {
        SWSS_LOG_ERROR("segment object doesn't exist for segment %s", srv6_segment.c_str());
        return false;
    }

    if(srv6_tunnel_id == SAI_NULL_OBJECT_ID)
    {
        SWSS_LOG_ERROR("tunnel object doesn't exist for source %s", srv6_source.c_str());
        return false;
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
    if(status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("FAiled to create srv6 nexthop for %s", nh.to_string(false,true).c_str());
        return false;
    }
    m_neighOrch->updateSrv6Nexthop(nh, nexthop_id);
    srv6_nexthop_table_[nh] = nexthop_id;
    sid_table_[srv6_segment].nexthops.insert(nh);
    srv6TunnelUpdateNexthops(srv6_source, nh, true);
    return true;
}

bool Srv6Orch::srv6Nexthops(const NextHopGroupKey &nhgKey, sai_object_id_t &nexthop_id)
{
    SWSS_LOG_ENTER();
    set<NextHopKey> nexthops = nhgKey.getNextHops();
    string srv6_source;
    string srv6_segment;

    for(auto nh : nexthops)
    {
        srv6_source = nh.srv6_source;
        if(!createSrv6Tunnel(srv6_source))
        {
            SWSS_LOG_ERROR("Failed to create tunnel for source %s", srv6_source.c_str());
        }
        if(!createSrv6Nexthop(nh))
        {
            SWSS_LOG_ERROR("Failed to create SRV6 nexthop %s", nh.to_string(false,true).c_str());
            return false;
        }
    }

    if(nhgKey.getSize() == 1)
    {
        NextHopKey nhkey(nhgKey.to_string(), false, true);
        nexthop_id = srv6_nexthop_table_[nhkey];
    }
    return true;
}

bool Srv6Orch::createUpdateSidList(const string sid_name, const string sid_list)
{
    SWSS_LOG_ENTER();
    bool exists = (sid_table_.find(sid_name) != sid_table_.end());
    sai_segment_list_t segment_list;
    vector<string>sid_ips = tokenize(sid_list, SID_LIST_DELIMITER);
    sai_object_id_t segment_oid;
    segment_list.count = (uint32_t)sid_ips.size();
    if(segment_list.count == 0)
    {
        SWSS_LOG_ERROR("segment list count is zero, skip");
        return true;
    }
    SWSS_LOG_INFO("Segment count %d", segment_list.count);
    segment_list.list = new sai_ip6_t[segment_list.count];
    uint32_t index = 0;
    for(string ip_str : sid_ips)
    {
        IpPrefix ip(ip_str);
        SWSS_LOG_INFO("Segment %s, count %d", ip.to_string().c_str(), segment_list.count);
        memcpy(segment_list.list[index++], ip.getIp().getV6Addr(), 16);
    }
    sai_attribute_t attr;
    sai_status_t status;
    if(!exists)
    {
        // create sidlist object
        SWSS_LOG_INFO("Create SID list");
        vector<sai_attribute_t> attributes;
        attr.id = SAI_SRV6_SIDLIST_ATTR_SEGMENT_LIST;
        attr.value.segmentlist.list = segment_list.list;
        attr.value.segmentlist.count = segment_list.count;
        attributes.push_back(attr);

        attr.id = SAI_SRV6_SIDLIST_ATTR_TYPE;
        attr.value.s32 = SAI_SRV6_SIDLIST_TYPE_ENCAPS_RED;
        attributes.push_back(attr);
        status = sai_srv6_api->create_srv6_sidlist(&segment_oid, gSwitchId, (uint32_t) attributes.size(), attributes.data());
        if(status != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_ERROR("Failed to create srv6 sidlist object, rv %d", status);
            return false;
        }
        sid_table_[sid_name].sid_object_id = segment_oid;
    }
    else
    {
        SWSS_LOG_INFO("Set SID list");
        // Update sidlist object with new set of ipv6 addresses
        attr.id = SAI_SRV6_SIDLIST_ATTR_SEGMENT_LIST;
        attr.value.segmentlist.list = segment_list.list;
        attr.value.segmentlist.count = segment_list.count;
        segment_oid = (sid_table_.find(sid_name)->second).sid_object_id;
        status = sai_srv6_api->set_srv6_sidlist_attribute(segment_oid, &attr);
        if(status != SAI_STATUS_SUCCESS)
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
        SWSS_LOG_NOTICE("segment object %s referenced by other nexthops: count %ld, not deleting",
                      sid_name.c_str(), sid_table_[sid_name].nexthops.size());
        return false;
    }
    SWSS_LOG_INFO("Remove sid list, segname %s", sid_name.c_str());
    status = sai_srv6_api->remove_srv6_sidlist(sid_table_[sid_name].sid_object_id);
    if(status != SAI_STATUS_SUCCESS)
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
    string sid_list;

    for(auto i : kfvFieldsValues(tuple))
    {
        if(fvField(i) == "path")
        {
          sid_list = fvValue(i);
        }
    }
    if(op == SET_COMMAND)
    {
        if(!createUpdateSidList(sid_name, sid_list))
        {
          SWSS_LOG_ERROR("Failed to process sid %s", sid_name.c_str());
        }
    }
    else if(op == DEL_COMMAND)
    {
        if(!deleteSidList(sid_name))
        {
            SWSS_LOG_ERROR("Failed to delete sid %s", sid_name.c_str());
        }
    } else {
        SWSS_LOG_ERROR("Invalid command");
    }
}

bool Srv6Orch::mySidExists(string my_sid_string)
{
    if(srv6_my_sid_table_.find(my_sid_string) != srv6_my_sid_table_.end())
    {
        return true;
    }
    return false;
}

bool Srv6Orch::sidEntryEndpointBehavior(string action, sai_my_sid_entry_endpoint_behavior_t &end_behavior,
                                        sai_my_sid_entry_endpoint_behavior_flavor_t &end_flavor)
{
    if(action == "end")
    {
        end_behavior = SAI_MY_SID_ENTRY_ENDPOINT_BEHAVIOR_E;
        end_flavor = SAI_MY_SID_ENTRY_ENDPOINT_BEHAVIOR_FLAVOR_PSP_AND_USD;
    }
    else if(action == "end.x")
    {
        end_behavior = SAI_MY_SID_ENTRY_ENDPOINT_BEHAVIOR_X;
        end_flavor = SAI_MY_SID_ENTRY_ENDPOINT_BEHAVIOR_FLAVOR_PSP_AND_USD;
    }
    else if(action == "end.t")
    {
        end_behavior = SAI_MY_SID_ENTRY_ENDPOINT_BEHAVIOR_T;
        end_flavor = SAI_MY_SID_ENTRY_ENDPOINT_BEHAVIOR_FLAVOR_PSP_AND_USD;
    }
    else if(action == "end.dx6")
    {
        end_behavior = SAI_MY_SID_ENTRY_ENDPOINT_BEHAVIOR_DX6;
    }
    else if(action == "end.dx4")
    {
        end_behavior = SAI_MY_SID_ENTRY_ENDPOINT_BEHAVIOR_DX4;
    }
    else if(action == "end.dt4")
    {
        end_behavior = SAI_MY_SID_ENTRY_ENDPOINT_BEHAVIOR_DT4;
    }
    else if(action == "end.dt6")
    {
        end_behavior = SAI_MY_SID_ENTRY_ENDPOINT_BEHAVIOR_DT6;
    }
    else if(action == "end.dt46")
    {
        end_behavior = SAI_MY_SID_ENTRY_ENDPOINT_BEHAVIOR_DT46;
    }
    else if(action == "end.b6.encaps")
    {
        end_behavior = SAI_MY_SID_ENTRY_ENDPOINT_BEHAVIOR_B6_ENCAPS;
    }
    else if(action == "end.b6.encaps.red")
    {
        end_behavior = SAI_MY_SID_ENTRY_ENDPOINT_BEHAVIOR_B6_ENCAPS_RED;
    }
    else if(action == "end.b6.insert")
    {
        end_behavior = SAI_MY_SID_ENTRY_ENDPOINT_BEHAVIOR_B6_INSERT;
    }
    else if(action == "end.b6.insert.red")
    {
        end_behavior = SAI_MY_SID_ENTRY_ENDPOINT_BEHAVIOR_B6_INSERT_RED;
    }
    else
    {
        SWSS_LOG_ERROR("Invalid endpoing behavior function");
        return false;
    }
    return true;
}

bool Srv6Orch::mySidVrfRequired(const sai_my_sid_entry_endpoint_behavior_t end_behavior)
{
    if(end_behavior == SAI_MY_SID_ENTRY_ENDPOINT_BEHAVIOR_T ||
       end_behavior == SAI_MY_SID_ENTRY_ENDPOINT_BEHAVIOR_DT4 ||
       end_behavior == SAI_MY_SID_ENTRY_ENDPOINT_BEHAVIOR_DT6 ||
       end_behavior == SAI_MY_SID_ENTRY_ENDPOINT_BEHAVIOR_DT46)
    {
      return true;
    }
    return false;
}

bool Srv6Orch::mySidXConnectNexthopRequired(const sai_my_sid_entry_endpoint_behavior_t end_behavior)
{
    if(end_behavior == SAI_MY_SID_ENTRY_ENDPOINT_BEHAVIOR_X ||
       end_behavior == SAI_MY_SID_ENTRY_ENDPOINT_BEHAVIOR_DX4 ||
       end_behavior == SAI_MY_SID_ENTRY_ENDPOINT_BEHAVIOR_DX6)
    {
      return true;
    }
    return false;
}

bool Srv6Orch::mySidUpdateNexthop(string my_sid_string, bool ecmp, NextHopKey nhkey, NextHopGroupKey nhgkey)
{
    sai_my_sid_entry_t my_sid_entry;
    sai_attribute_t attr;
    sai_object_id_t nh;
    nh = ecmp ? gRouteOrch->getNextHopGroupId(nhgkey) : m_neighOrch->getNextHopId(nhkey);
    attr.id = SAI_MY_SID_ENTRY_ATTR_NEXT_HOP_ID;
    attr.value.oid = nh;

    my_sid_entry = srv6_my_sid_table_[my_sid_string].entry;

    SWSS_LOG_NOTICE("Update mysid %s SAI object with forward and NH 0x%lx", my_sid_string.c_str(), nh);
    if(sai_srv6_api->set_my_sid_entry_attribute(&my_sid_entry, &attr) == SAI_STATUS_SUCCESS)
    {
        attr.id = SAI_MY_SID_ENTRY_ATTR_PACKET_ACTION;
        attr.value.s32 = SAI_PACKET_ACTION_FORWARD;
        if(sai_srv6_api->set_my_sid_entry_attribute(&my_sid_entry, &attr) == SAI_STATUS_SUCCESS)
        {
            if(ecmp)
            {
              gRouteOrch->increaseNextHopRefCount(nhgkey);
            }
            else
            {
              m_neighOrch->increaseNextHopRefCount(nhkey, 1);
            }
            SWSS_LOG_INFO("SRV6 Mysid update:Nexthop refcount %d", m_neighOrch->getNextHopRefCount(nhkey));
            srv6_my_sid_table_[my_sid_string].ecmp = ecmp;
            srv6_my_sid_table_[my_sid_string].nhkey = nhkey;
            srv6_my_sid_table_[my_sid_string].nhgkey = nhgkey;
            return true;
        }
        else
        {
            return false;
        }
    }

    return false;
}

void Srv6Orch::updateMySidEntries(const NeighborUpdate update)
{
    NextHopKey nhkey = update.entry;
    NextHopGroupKey nhgkey;
    SWSS_LOG_INFO("Received nhop update notification for NH %s", nhkey.to_string().c_str());
    SWSS_LOG_INFO("Total mysid nh map is %ld", srv6_my_sid_nexthop_table_.size());
    if(srv6_my_sid_nexthop_table_.find(nhkey) != srv6_my_sid_nexthop_table_.end())
    {
        auto sid_iter = srv6_my_sid_nexthop_table_[nhkey];
        for(auto mysid = sid_iter.begin(); mysid != sid_iter.end(); mysid++)
        {
            SWSS_LOG_NOTICE("Update mysid %s with nhkey %s",(*mysid).c_str(), nhkey.to_string().c_str());
            if(!mySidExists(*mysid))
            {
                srv6_my_sid_nexthop_table_[nhkey].erase(*mysid);
                continue;
            }
            if(!mySidUpdateNexthop(*mysid, false, nhkey, nhgkey))
            {
                SWSS_LOG_ERROR("Failed to update mysid %s with nhkey %s",(*mysid).c_str(), nhkey.to_string().c_str());
            }
        }
        srv6_my_sid_nexthop_table_.erase(nhkey);
    }

    // iterate over ECMP map and update Nexthops
    SWSS_LOG_INFO("Total mysid nhg map is %ld", srv6_my_sid_nexthop_group_table_.size());
    for(auto iter = srv6_my_sid_nexthop_group_table_.begin(); iter != srv6_my_sid_nexthop_group_table_.end();)
    {
        nhgkey = iter->first;
        SWSS_LOG_INFO("Check Nhgkey %s for NH key %s", nhgkey.to_string().c_str(), nhkey.to_string().c_str());
        // If NextHopGroup contains the nexthop, update all MY_SID entries with ECMP group.
        if(!nhgkey.contains(nhkey))
        {
            iter++;
            continue;
        }
        SWSS_LOG_INFO("NHG key %s contains NH key %s", nhgkey.to_string().c_str(), nhkey.to_string().c_str());
        if(!gRouteOrch->hasNextHopGroup(nhgkey))
        {
            SWSS_LOG_INFO("NHG doesn't exist, create NHG %s", nhgkey.to_string().c_str());
            if(!gRouteOrch->addNextHopGroup(nhgkey))
            {
                iter++;
                continue;
            }
        }
        else
        {
            SWSS_LOG_INFO("NHG %s exists in map", nhgkey.to_string().c_str());
            sai_object_id_t nhg_id = gRouteOrch->getNextHopGroupId(nhgkey);
            if(nhg_id == SAI_NULL_OBJECT_ID)
            {
                SWSS_LOG_INFO("NHG %s handle null, return", nhgkey.to_string().c_str());
                iter++;
                continue;
            }
        }
        SWSS_LOG_INFO("NHG exists for %s, 0x%lx", nhgkey.to_string().c_str(), gRouteOrch->getNextHopGroupId(nhgkey));
        auto sid_iter = iter->second;
        for(auto mysid = sid_iter.begin(); mysid != sid_iter.end(); mysid++)
        {
            SWSS_LOG_NOTICE("Update mysid %s with nhgkey %s",(*mysid).c_str(), nhgkey.to_string().c_str());
            if(!mySidExists(*mysid))
            {
                srv6_my_sid_nexthop_group_table_[nhgkey].erase(mysid);
                continue;
            }
            if(!mySidUpdateNexthop(*mysid, true, nhkey, nhgkey))
            {
                SWSS_LOG_ERROR("Failed to update mysid %s with nhkey %s",(*mysid).c_str(), nhgkey.to_string().c_str());
            }
        }
        iter = srv6_my_sid_nexthop_group_table_.erase(iter);
    }
}

void Srv6Orch::update(SubjectType type, void *ctx)
{
    SWSS_LOG_ENTER();
    assert(ctx);

    if(type == SUBJECT_TYPE_NEIGH_CHANGE)
    {
        NeighborUpdate *update = static_cast<NeighborUpdate *>(ctx);
        SWSS_LOG_NOTICE("Neighbor change notification");
        updateMySidEntries(*update);
    }
}

void Srv6Orch::mySidNexthopMapUpdate(string my_sid_string, NextHopKey nhkey)
{
    // cache my_sid entries for the future nexthop update.
    srv6_my_sid_nexthop_table_[nhkey].insert(my_sid_string);
}

void Srv6Orch::mySidNexthopGroupMapUpdate(string my_sid_string, NextHopGroupKey nhgkey)
{
    // cache my_sid entries for the future nexthop update.
    srv6_my_sid_nexthop_group_table_[nhgkey].insert(my_sid_string);
}

bool Srv6Orch::mySidXConnectNexthop(string my_sid_string, string nexthop_string, sai_object_id_t &nh_oid)
{
    // Nexthop string: <ipaddress>@<interface>,<ipaddress>@<interface>....
    vector<string> nhg_vector = tokenize(nexthop_string, NHG_DELIMITER);
    sai_object_id_t nexthop_id = SAI_NULL_OBJECT_ID;
    if(nhg_vector.size() == 1)
    {
        SWSS_LOG_NOTICE("Single nexthop %s in my_sid", nhg_vector[0].c_str());
        auto keys = tokenize(nhg_vector[0], NH_DELIMITER);
        assert(keys.size() == 2);
        NextHopKey nhkey = NextHopKey(nhg_vector[0]);
        SWSS_LOG_NOTICE("NH IP %s, interface %s", nhkey.ip_address.to_string().c_str(), nhkey.alias.c_str());
        nexthop_id = m_neighOrch->getNextHopId(nhkey);
        if(nexthop_id == SAI_NULL_OBJECT_ID)
        {
            SWSS_LOG_NOTICE("Nexthop doesn't exist for nh %s", nhkey.to_string().c_str());
            mySidNexthopMapUpdate(my_sid_string, nhkey);
            //Resolve the nexthop if it doesn't exist.
            m_neighOrch->resolveNeighbor(nhkey);
            nh_oid = SAI_NULL_OBJECT_ID;
            return false;
        }
        else
        {
            nh_oid = nexthop_id;
            srv6_my_sid_table_[my_sid_string].nhkey = nhkey;
            srv6_my_sid_table_[my_sid_string].ecmp = false;
            m_neighOrch->increaseNextHopRefCount(nhkey, 1);
            SWSS_LOG_INFO("SRV6 Mysid create: Nexthop refcount %d", m_neighOrch->getNextHopRefCount(nhkey));
        }
    }
    else
    {
        SWSS_LOG_NOTICE("Create nexthopgroup");

        NextHopGroupKey nhgkey(nexthop_string);
        if(!gRouteOrch->hasNextHopGroup(nhgkey))
        {
            if(!gRouteOrch->addNextHopGroup(nhgkey))
            {
                nexthop_id = SAI_NULL_OBJECT_ID;
                SWSS_LOG_INFO("Failed to create nexthop group %s. NHG exists %s", nhgkey.to_string().c_str(),
                    gRouteOrch->hasNextHopGroup(nhgkey) ? "Yes" : "No");

                // Resolve the nexthops in ECMP group if it doesn't exist
                for(auto it : nhgkey.getNextHops())
                {
                    if(!m_neighOrch->hasNextHop(it))
                    {
                        m_neighOrch->resolveNeighbor(it);
                    }
                }
                SWSS_LOG_NOTICE("Nexthop group doesn't exist for nhg %s", nhgkey.to_string().c_str());
                mySidNexthopGroupMapUpdate(my_sid_string, nhgkey);
                return false;
            }
        }
        nexthop_id = gRouteOrch->getNextHopGroupId(nhgkey);
        if(nexthop_id == SAI_NULL_OBJECT_ID)
        {
            SWSS_LOG_ERROR("Nexthop group object for key %s is invalid", nhgkey.to_string().c_str());
            return false;
        }
        gRouteOrch->increaseNextHopRefCount(nhgkey);
        nexthop_id = gRouteOrch->getNextHopGroupId(nhgkey);
        nh_oid = nexthop_id;
        srv6_my_sid_table_[my_sid_string].nhgkey = nhgkey;
        srv6_my_sid_table_[my_sid_string].ecmp = true;
    }
    return true;
}

bool Srv6Orch::createUpdateMysidEntry(string my_sid_string, const string dt_vrf, const string end_action, const string nexthop)
{
    SWSS_LOG_ENTER();
    vector<sai_attribute_t> attributes;
    sai_attribute_t attr;
    string key_string = my_sid_string;
    sai_my_sid_entry_endpoint_behavior_t end_behavior;
    sai_my_sid_entry_endpoint_behavior_flavor_t end_flavor = SAI_MY_SID_ENTRY_ENDPOINT_BEHAVIOR_FLAVOR_PSP_AND_USD;

    bool entry_exists = false;
    if(mySidExists(key_string))
    {
        entry_exists = true;
    }

    sai_my_sid_entry_t my_sid_entry;
    if(!entry_exists)
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

    SWSS_LOG_NOTICE("MySid: sid %s, action %s, vrf %s, block %d, node %d, func %d, arg %d dt_vrf %s",
      my_sid_string.c_str(), end_action.c_str(), dt_vrf.c_str(),my_sid_entry.locator_block_len, my_sid_entry.locator_node_len,
      my_sid_entry.function_len, my_sid_entry.args_len, dt_vrf.c_str());

    if(sidEntryEndpointBehavior(end_action, end_behavior, end_flavor) != true)
    {
        SWSS_LOG_ERROR("Invalid my_sid action %s", end_action.c_str());
        return false;
    }
    sai_attribute_t vrf_attr;
    bool vrf_update = false;
    bool nh_update = false;
    if(mySidVrfRequired(end_behavior))
    {
        sai_object_id_t dt_vrf_id;
        SWSS_LOG_NOTICE("DT VRF name %s", dt_vrf.c_str());
        if(m_vrfOrch->isVRFexists(dt_vrf))
        {
            SWSS_LOG_NOTICE("VRF %s exists in DB", dt_vrf.c_str());
            dt_vrf_id = m_vrfOrch->getVRFid(dt_vrf);
            if(dt_vrf_id == SAI_NULL_OBJECT_ID)
            {
              SWSS_LOG_ERROR("VRF object not created for DT VRF %s", dt_vrf.c_str());
              return false;
            }
            SWSS_LOG_NOTICE("DT VRF Object 0x%lx", dt_vrf_id);
        }
        else
        {
            SWSS_LOG_NOTICE("VRF %s doesn't exist in DB", dt_vrf.c_str());
            dt_vrf_id = gVirtualRouterId;
        }
        vrf_attr.id = SAI_MY_SID_ENTRY_ATTR_VRF;
        vrf_attr.value.oid = dt_vrf_id;
        attributes.push_back(vrf_attr);
        vrf_update = true;
    }
    sai_attribute_t nh_attr;
    if(mySidXConnectNexthopRequired(end_behavior))
    {
        sai_object_id_t nh_id = SAI_NULL_OBJECT_ID;
        if(mySidXConnectNexthop(key_string, nexthop, nh_id))
        {
            nh_attr.id = SAI_MY_SID_ENTRY_ATTR_NEXT_HOP_ID;
            nh_attr.value.oid = nh_id;
            attributes.push_back(nh_attr);
        }
        else
        {
            SWSS_LOG_NOTICE("Nexthop %s doesn't exist, set drop action", nexthop.c_str());
            nh_attr.id = SAI_MY_SID_ENTRY_ATTR_PACKET_ACTION;
            nh_attr.value.s32 = SAI_PACKET_ACTION_DROP;
            attributes.push_back(nh_attr);
        }
        nh_update = true;
    }
    attr.id = SAI_MY_SID_ENTRY_ATTR_ENDPOINT_BEHAVIOR;
    attr.value.s32 = end_behavior;
    attributes.push_back(attr);

    attr.id = SAI_MY_SID_ENTRY_ATTR_ENDPOINT_BEHAVIOR_FLAVOR;
    attr.value.s32 = end_flavor;
    attributes.push_back(attr);

    sai_status_t status = SAI_STATUS_SUCCESS;
    if(!entry_exists)
    {
        status = sai_srv6_api->create_my_sid_entry(&my_sid_entry, (uint32_t) attributes.size(), attributes.data());
        if(status != SAI_STATUS_SUCCESS)
        {
          SWSS_LOG_ERROR("Failed to create my_sid entry %s, rv %d", key_string.c_str(), status);
          return false;
        }
    }
    else
    {
        if(vrf_update)
        {
            status = sai_srv6_api->set_my_sid_entry_attribute(&my_sid_entry, &vrf_attr);
            if(status != SAI_STATUS_SUCCESS)
            {
                SWSS_LOG_ERROR("Failed to update VRF to my_sid_entry %s, rv %d", key_string.c_str(), status);
                return false;
            }
        }

        if(nh_update)
        {
            status = sai_srv6_api->set_my_sid_entry_attribute(&my_sid_entry, &nh_attr);
            if(status != SAI_STATUS_SUCCESS)
            {
                SWSS_LOG_ERROR("Failed to update Nexthop to my_sid_entry %s, rv %d", key_string.c_str(), status);
                return false;
            }
        }
    }
    SWSS_LOG_NOTICE("Store keystring %s in cache", key_string.c_str());
    srv6_my_sid_table_[key_string].entry = my_sid_entry;
    return true;
}

bool Srv6Orch::deleteMysidEntry(const string my_sid_string)
{
    sai_status_t status = SAI_STATUS_SUCCESS;
    if(!mySidExists(my_sid_string))
    {
        SWSS_LOG_ERROR("My_sid_entry doesn't exist for %s", my_sid_string.c_str());
        return false;
    }
    sai_my_sid_entry_t my_sid_entry = srv6_my_sid_table_[my_sid_string].entry;

    NextHopKey nhkey;
    NextHopGroupKey nhgkey;

    if(srv6_my_sid_table_[my_sid_string].ecmp)
    {
        nhgkey = srv6_my_sid_table_[my_sid_string].nhgkey;
    }
    else
    {
        nhkey = srv6_my_sid_table_[my_sid_string].nhkey;
    }

    SWSS_LOG_NOTICE("MySid Delete: sid %s", my_sid_string.c_str());
    status = sai_srv6_api->remove_my_sid_entry(&my_sid_entry);
    if(status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to delete my_sid entry rv %d", status);
        return false;
    }
    if(srv6_my_sid_table_[my_sid_string].ecmp)
    {
        if(gRouteOrch->hasNextHopGroup(nhgkey))
        {
            gRouteOrch->decreaseNextHopRefCount(nhgkey);
        }
        SWSS_LOG_INFO("Mysid %s uses ecmp, remove NHG: refcount zero %s", my_sid_string.c_str(),
          gRouteOrch->isRefCounterZero(nhgkey) ? "Yes" : "No");
        if(gRouteOrch->isRefCounterZero(nhgkey))
        {
            if(gRouteOrch->removeNextHopGroup(nhgkey))
            {
                SWSS_LOG_INFO("Removed nexthop group %s", nhgkey.to_string().c_str());
            }
            else
            {
                SWSS_LOG_ERROR("Failed to remove nexthop group %s", nhgkey.to_string().c_str());
            }
        }
    }
    else
    {
        if(m_neighOrch->hasNextHop(nhkey))
        {
            SWSS_LOG_INFO("SRV6 Mysid Nexthop refcount %d", m_neighOrch->getNextHopRefCount(nhkey));
            m_neighOrch->decreaseNextHopRefCount(nhkey, 1);
        }
    }
    srv6_my_sid_table_.erase(my_sid_string);
    return true;
}

void Srv6Orch::doTaskMySidTable(const KeyOpFieldsValuesTuple & tuple)
{
    SWSS_LOG_ENTER();
    string op = kfvOp(tuple);
    string end_action, dt_vrf, nexthop;

    //Key for mySid : block_len:node_len:function_len:args_len:sid-ip
    string keyString = kfvKey(tuple);
    for(auto i : kfvFieldsValues(tuple))
    {
        if (fvField(i) == "action")
        {
          end_action = fvValue(i);
        }
        if(fvField(i) == "vrf")
        {
          dt_vrf = fvValue(i);
        }
        if(fvField(i) == "adj")
        {
          nexthop = fvValue(i);
        }
    }
    if(op == SET_COMMAND)
    {
        if(!createUpdateMysidEntry(keyString, dt_vrf, end_action, nexthop))
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
        if(table_name == APP_SRV6_SID_LIST_TABLE_NAME)
        {
            doTaskSidTable(t);
        }
        else if(table_name == APP_SRV6_MY_SID_TABLE_NAME)
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
