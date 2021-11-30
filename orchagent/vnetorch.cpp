#include <cassert>
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <exception>
#include <inttypes.h>
#include <algorithm>

#include "sai.h"
#include "saiextensions.h"
#include "macaddress.h"
#include "orch.h"
#include "portsorch.h"
#include "request_parser.h"
#include "vnetorch.h"
#include "vxlanorch.h"
#include "directory.h"
#include "swssnet.h"
#include "intfsorch.h"
#include "neighorch.h"
#include "crmorch.h"
#include "routeorch.h"

extern sai_virtual_router_api_t* sai_virtual_router_api;
extern sai_route_api_t* sai_route_api;
extern sai_bridge_api_t* sai_bridge_api;
extern sai_router_interface_api_t* sai_router_intfs_api;
extern sai_fdb_api_t* sai_fdb_api;
extern sai_neighbor_api_t* sai_neighbor_api;
extern sai_next_hop_api_t* sai_next_hop_api;
extern sai_next_hop_group_api_t* sai_next_hop_group_api;
extern sai_object_id_t gSwitchId;
extern sai_object_id_t gVirtualRouterId;
extern Directory<Orch*> gDirectory;
extern PortsOrch *gPortsOrch;
extern IntfsOrch *gIntfsOrch;
extern NeighOrch *gNeighOrch;
extern CrmOrch *gCrmOrch;
extern RouteOrch *gRouteOrch;
extern MacAddress gVxlanMacAddress;
extern BfdOrch *gBfdOrch;

/*
 * VRF Modeling and VNetVrf class definitions
 */
std::vector<VR_TYPE> vr_cntxt;

VNetVrfObject::VNetVrfObject(const std::string& vnet, const VNetInfo& vnetInfo,
                             vector<sai_attribute_t>& attrs) : VNetObject(vnetInfo)
{
    vnet_name_ = vnet;
    createObj(attrs);
}

sai_object_id_t VNetVrfObject::getVRidIngress() const
{
    if (vr_ids_.find(VR_TYPE::ING_VR_VALID) != vr_ids_.end())
    {
        return vr_ids_.at(VR_TYPE::ING_VR_VALID);
    }
    return SAI_NULL_OBJECT_ID;
}

sai_object_id_t VNetVrfObject::getVRidEgress() const
{
    if (vr_ids_.find(VR_TYPE::EGR_VR_VALID) != vr_ids_.end())
    {
        return vr_ids_.at(VR_TYPE::EGR_VR_VALID);
    }
    return SAI_NULL_OBJECT_ID;
}

set<sai_object_id_t> VNetVrfObject::getVRids() const
{
    set<sai_object_id_t> ids;

    for_each (vr_ids_.begin(), vr_ids_.end(), [&](std::pair<VR_TYPE, sai_object_id_t> element)
    {
        ids.insert(element.second);
    });

    return ids;
}

bool VNetVrfObject::createObj(vector<sai_attribute_t>& attrs)
{
    auto l_fn = [&] (sai_object_id_t& router_id) {

        sai_status_t status = sai_virtual_router_api->create_virtual_router(&router_id,
                                                                            gSwitchId,
                                                                            static_cast<uint32_t>(attrs.size()),
                                                                            attrs.data());
        if (status != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_ERROR("Failed to create virtual router name: %s, rv: %d",
                           vnet_name_.c_str(), status);
            throw std::runtime_error("Failed to create VR object");
        }
        return true;
    };

    /*
     * Create ingress and egress VRF based on VR_VALID
     */

    for (auto vr_type : vr_cntxt)
    {
        sai_object_id_t router_id = gVirtualRouterId;
        if (vr_type != VR_TYPE::VR_INVALID)
        {
            if (getScope() != "default")
            {
                l_fn(router_id);
            }
            SWSS_LOG_DEBUG("VNET vr_type %d router id %" PRIx64 "  ", static_cast<int>(vr_type), router_id);
            vr_ids_.insert(std::pair<VR_TYPE, sai_object_id_t>(vr_type, router_id));
        }
    }

    SWSS_LOG_INFO("VNET '%s' router object created ", vnet_name_.c_str());
    return true;
}

bool VNetVrfObject::updateObj(vector<sai_attribute_t>& attrs)
{
    set<sai_object_id_t> vr_ent = getVRids();

    for (const auto& attr: attrs)
    {
        for (auto it : vr_ent)
        {
            sai_status_t status = sai_virtual_router_api->set_virtual_router_attribute(it, &attr);
            if (status != SAI_STATUS_SUCCESS)
            {
                SWSS_LOG_ERROR("Failed to update virtual router attribute. VNET name: %s, rv: %d",
                                vnet_name_.c_str(), status);
                return false;
            }
        }
    }

    SWSS_LOG_INFO("VNET '%s' was updated", vnet_name_.c_str());
    return true;
}

bool VNetVrfObject::hasRoute(IpPrefix& ipPrefix)
{
    if ((routes_.find(ipPrefix) != routes_.end()) || (tunnels_.find(ipPrefix) != tunnels_.end()))
    {
        return true;
    }

    return false;
}

bool VNetVrfObject::addRoute(IpPrefix& ipPrefix, NextHopGroupKey& nexthops)
{
    if (nexthops.is_overlay_nexthop())
    {
        tunnels_[ipPrefix] = nexthops;
    }
    else
    {
        SWSS_LOG_ERROR("Input %s is not overlay nexthop group", nexthops.to_string().c_str());
        return false;
    }

    return true;
}

void VNetVrfObject::increaseNextHopRefCount(const nextHop& nh)
{
    /* Return when there is no next hop (dropped) */
    if (nh.ips.getSize() == 0)
    {
        return;
    }
    else if (nh.ips.getSize() == 1)
    {
        NextHopKey nexthop(nh.ips.to_string(), nh.ifname);
        if (nexthop.ip_address.isZero())
        {
            gIntfsOrch->increaseRouterIntfsRefCount(nexthop.alias);
        }
        else
        {
            gNeighOrch->increaseNextHopRefCount(nexthop);
        }
    }
    else
    {
        /* Handle ECMP routes */
    }
}
void VNetVrfObject::decreaseNextHopRefCount(const nextHop& nh)
{
    /* Return when there is no next hop (dropped) */
    if (nh.ips.getSize() == 0)
    {
        return;
    }
    else if (nh.ips.getSize() == 1)
    {
        NextHopKey nexthop(nh.ips.to_string(), nh.ifname);
        if (nexthop.ip_address.isZero())
        {
            gIntfsOrch->decreaseRouterIntfsRefCount(nexthop.alias);
        }
        else
        {
            gNeighOrch->decreaseNextHopRefCount(nexthop);
        }
    }
    else
    {
        /* Handle ECMP routes */
    }
}

bool VNetVrfObject::addRoute(IpPrefix& ipPrefix, nextHop& nh)
{
    if (hasRoute(ipPrefix))
    {
        SWSS_LOG_INFO("VNET route '%s' exists", ipPrefix.to_string().c_str());
        return false;
    }

    increaseNextHopRefCount(nh);
    routes_[ipPrefix] = nh;
    return true;
}

bool VNetVrfObject::removeRoute(IpPrefix& ipPrefix)
{
    if (!hasRoute(ipPrefix))
    {
        SWSS_LOG_INFO("VNET route '%s' does'nt exist", ipPrefix.to_string().c_str());
        return false;
    }
    /*
     * Remove nexthop tunnel object before removing route
     */

    if (tunnels_.find(ipPrefix) != tunnels_.end())
    {
        tunnels_.erase(ipPrefix);
    }
    else
    {
        nextHop nh = routes_[ipPrefix];
        decreaseNextHopRefCount(nh);
        routes_.erase(ipPrefix);
    }
    return true;
}

size_t VNetVrfObject::getRouteCount() const
{
    return (routes_.size() + tunnels_.size());
}

bool VNetVrfObject::getRouteNextHop(IpPrefix& ipPrefix, nextHop& nh)
{
    if (!hasRoute(ipPrefix))
    {
        SWSS_LOG_INFO("VNET route '%s' does'nt exist", ipPrefix.to_string().c_str());
        return false;
    }

    nh = routes_.at(ipPrefix);
    return true;
}

sai_object_id_t VNetVrfObject::getTunnelNextHop(NextHopKey& nh)
{
    sai_object_id_t nh_id = SAI_NULL_OBJECT_ID;
    auto tun_name = getTunnelName();

    VxlanTunnelOrch* vxlan_orch = gDirectory.get<VxlanTunnelOrch*>();

    nh_id = vxlan_orch->createNextHopTunnel(tun_name, nh.ip_address, nh.mac_address, nh.vni);
    if (nh_id == SAI_NULL_OBJECT_ID)
    {
        throw std::runtime_error("NH Tunnel create failed for " + vnet_name_ + " ip " + nh.ip_address.to_string());
    }

    return nh_id;
}

bool VNetVrfObject::removeTunnelNextHop(NextHopKey& nh)
{
    auto tun_name = getTunnelName();

    VxlanTunnelOrch* vxlan_orch = gDirectory.get<VxlanTunnelOrch*>();

    if (!vxlan_orch->removeNextHopTunnel(tun_name, nh.ip_address, nh.mac_address, nh.vni))
    {
        SWSS_LOG_ERROR("VNET %s Tunnel NextHop remove failed for '%s'",
                        vnet_name_.c_str(), nh.ip_address.to_string().c_str());
        return false;
    }

    return true;
}

VNetVrfObject::~VNetVrfObject()
{
    set<sai_object_id_t> vr_ent = getVRids();
    for (auto it : vr_ent)
    {
        sai_status_t status = sai_virtual_router_api->remove_virtual_router(it);
        if (status != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_ERROR("Failed to remove virtual router name: %s, rv:%d",
                            vnet_name_.c_str(), status);
        }
    }

    SWSS_LOG_INFO("VNET '%s' deleted ", vnet_name_.c_str());
}

/*
 * VNet Orch class definitions
 */

template <class T>
std::unique_ptr<T> VNetOrch::createObject(const string& vnet_name, const VNetInfo& vnet_info,
                                          vector<sai_attribute_t>& attrs)
{
    std::unique_ptr<T> vnet_obj(new T(vnet_name, vnet_info, attrs));
    return vnet_obj;
}

VNetOrch::VNetOrch(DBConnector *db, const std::string& tableName, VNET_EXEC op)
         : Orch2(db, tableName, request_)
{
    vnet_exec_ = op;

    if (op == VNET_EXEC::VNET_EXEC_VRF)
    {
        vr_cntxt = { VR_TYPE::ING_VR_VALID };
    }
    else
    {
        // BRIDGE Handling
    }
}

bool VNetOrch::setIntf(const string& alias, const string name, const IpPrefix *prefix, const bool adminUp, const uint32_t mtu)
{
    SWSS_LOG_ENTER();

    if (!isVnetExists(name))
    {
        SWSS_LOG_WARN("VNET %s doesn't exist", name.c_str());
        return false;
    }

    if (isVnetExecVrf())
    {
        auto *vnet_obj = getTypePtr<VNetVrfObject>(name);
        sai_object_id_t vrf_id = vnet_obj->getVRidIngress();

        return gIntfsOrch->setIntf(alias, vrf_id, prefix, adminUp, mtu);
    }

    return false;
}

bool VNetOrch::delIntf(const string& alias, const string name, const IpPrefix *prefix)
{
    SWSS_LOG_ENTER();

    if (!isVnetExists(name))
    {
        SWSS_LOG_WARN("VNET %s doesn't exist", name.c_str());
        return false;
    }

    if (isVnetExecVrf())
    {
        auto *vnet_obj = getTypePtr<VNetVrfObject>(name);
        sai_object_id_t vrf_id = vnet_obj->getVRidIngress();

        return gIntfsOrch->removeIntf(alias, vrf_id, prefix);
    }

    return true;
}

bool VNetOrch::addOperation(const Request& request)
{
    SWSS_LOG_ENTER();

    sai_attribute_t attr;
    vector<sai_attribute_t> attrs;
    set<string> peer_list = {};
    bool peer = false, create = false;
    uint32_t vni=0;
    string tunnel;
    string scope;

    for (const auto& name: request.getAttrFieldNames())
    {
        if (name == "src_mac")
        {
            const auto& mac = request.getAttrMacAddress("src_mac");
            attr.id = SAI_VIRTUAL_ROUTER_ATTR_SRC_MAC_ADDRESS;
            memcpy(attr.value.mac, mac.getMac(), sizeof(sai_mac_t));
            attrs.push_back(attr);
        }
        else if (name == "peer_list")
        {
            peer_list  = request.getAttrSet("peer_list");
            peer = true;
        }
        else if (name == "vni")
        {
            vni  = static_cast<sai_uint32_t>(request.getAttrUint("vni"));
        }
        else if (name == "vxlan_tunnel")
        {
            tunnel = request.getAttrString("vxlan_tunnel");
        }
        else if (name == "scope")
        {
            scope = request.getAttrString("scope");
        }
        else
        {
            SWSS_LOG_INFO("Unknown attribute: %s", name.c_str());
            continue;
        }
    }

    const std::string& vnet_name = request.getKeyString(0);
    SWSS_LOG_INFO("VNET '%s' add request", vnet_name.c_str());

    try
    {
        VNetObject_T obj;
        auto it = vnet_table_.find(vnet_name);
        if (isVnetExecVrf())
        {
            VxlanTunnelOrch* vxlan_orch = gDirectory.get<VxlanTunnelOrch*>();

            if (!vxlan_orch->isTunnelExists(tunnel))
            {
                SWSS_LOG_WARN("Vxlan tunnel '%s' doesn't exist", tunnel.c_str());
                return false;
            }

            if (it == std::end(vnet_table_))
            {
                VNetInfo vnet_info = { tunnel, vni, peer_list, scope };
                obj = createObject<VNetVrfObject>(vnet_name, vnet_info, attrs);
                create = true;

                VNetVrfObject *vrf_obj = dynamic_cast<VNetVrfObject*>(obj.get());
                if (!vxlan_orch->createVxlanTunnelMap(tunnel, TUNNEL_MAP_T_VIRTUAL_ROUTER, vni,
                                                      vrf_obj->getEncapMapId(), vrf_obj->getDecapMapId(), VXLAN_ENCAP_TTL))
                {
                    SWSS_LOG_ERROR("VNET '%s', tunnel '%s', map create failed",
                                    vnet_name.c_str(), tunnel.c_str());
                    return false;
                }

                SWSS_LOG_NOTICE("VNET '%s' was added ", vnet_name.c_str());
            }
            else
            {
                SWSS_LOG_NOTICE("VNET '%s' already exists ", vnet_name.c_str());
            }
        }

        if (create)
        {
            vnet_table_[vnet_name] = std::move(obj);
        }
        else if (peer)
        {
            it->second->setPeerList(peer_list);
        }
        else if (!attrs.empty())
        {
            if(!it->second->updateObj(attrs))
            {
                return true;
            }
        }

    }
    catch(std::runtime_error& _)
    {
        SWSS_LOG_ERROR("VNET add operation error for %s: error %s ", vnet_name.c_str(), _.what());
        return false;
    }

    SWSS_LOG_INFO("VNET '%s' added/updated ", vnet_name.c_str());
    return true;
}

bool VNetOrch::delOperation(const Request& request)
{
    SWSS_LOG_ENTER();

    const std::string& vnet_name = request.getKeyString(0);

    if (vnet_table_.find(vnet_name) == std::end(vnet_table_))
    {
        SWSS_LOG_WARN("VNET '%s' doesn't exist", vnet_name.c_str());
        return true;
    }

    SWSS_LOG_INFO("VNET '%s' del request", vnet_name.c_str());

    try
    {
        auto it = vnet_table_.find(vnet_name);
        if (isVnetExecVrf())
        {
            VxlanTunnelOrch* vxlan_orch = gDirectory.get<VxlanTunnelOrch*>();
            VNetVrfObject *vrf_obj = dynamic_cast<VNetVrfObject*>(it->second.get());

            if (vrf_obj->getRouteCount())
            {
                SWSS_LOG_ERROR("VNET '%s': Routes are still present", vnet_name.c_str());
                return false;
            }

            if (!vxlan_orch->removeVxlanTunnelMap(vrf_obj->getTunnelName(), vrf_obj->getVni()))
            {
                SWSS_LOG_ERROR("VNET '%s' map delete failed", vnet_name.c_str());
                return false;
            }
        }
    }
    catch(std::runtime_error& _)
    {
        SWSS_LOG_ERROR("VNET del operation error for %s: error %s ", vnet_name.c_str(), _.what());
        return false;
    }

    vnet_table_.erase(vnet_name);

    return true;
}

/*
 * Vnet Route Handling
 */

static bool del_route(sai_object_id_t vr_id, sai_ip_prefix_t& ip_pfx)
{
    sai_route_entry_t route_entry;
    route_entry.vr_id = vr_id;
    route_entry.switch_id = gSwitchId;
    route_entry.destination = ip_pfx;

    sai_status_t status = sai_route_api->remove_route_entry(&route_entry);
    if (status == SAI_STATUS_ITEM_NOT_FOUND || status == SAI_STATUS_INVALID_PARAMETER)
    {
        SWSS_LOG_INFO("Unable to remove route since route is already removed");
        return true;
    }
    else if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("SAI Failed to remove route, rv: %d", status);
        return false;
    }

    if (route_entry.destination.addr_family == SAI_IP_ADDR_FAMILY_IPV4)
    {
        gCrmOrch->decCrmResUsedCounter(CrmResourceType::CRM_IPV4_ROUTE);
    }
    else
    {
        gCrmOrch->decCrmResUsedCounter(CrmResourceType::CRM_IPV6_ROUTE);
    }

    return true;
}

static bool add_route(sai_object_id_t vr_id, sai_ip_prefix_t& ip_pfx, sai_object_id_t nh_id)
{
    sai_route_entry_t route_entry;
    route_entry.vr_id = vr_id;
    route_entry.switch_id = gSwitchId;
    route_entry.destination = ip_pfx;

    sai_attribute_t route_attr;

    route_attr.id = SAI_ROUTE_ENTRY_ATTR_NEXT_HOP_ID;
    route_attr.value.oid = nh_id;

    sai_status_t status = sai_route_api->create_route_entry(&route_entry, 1, &route_attr);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("SAI failed to create route");
        return false;
    }

    if (route_entry.destination.addr_family == SAI_IP_ADDR_FAMILY_IPV4)
    {
        gCrmOrch->incCrmResUsedCounter(CrmResourceType::CRM_IPV4_ROUTE);
    }
    else
    {
        gCrmOrch->incCrmResUsedCounter(CrmResourceType::CRM_IPV6_ROUTE);
    }

    return true;
}

static bool update_route(sai_object_id_t vr_id, sai_ip_prefix_t& ip_pfx, sai_object_id_t nh_id)
{
    sai_route_entry_t route_entry;
    route_entry.vr_id = vr_id;
    route_entry.switch_id = gSwitchId;
    route_entry.destination = ip_pfx;

    sai_attribute_t route_attr;

    route_attr.id = SAI_ROUTE_ENTRY_ATTR_NEXT_HOP_ID;
    route_attr.value.oid = nh_id;

    sai_status_t status = sai_route_api->set_route_entry_attribute(&route_entry, &route_attr);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("SAI failed to update route");
        return false;
    }

    return true;
}

VNetRouteOrch::VNetRouteOrch(DBConnector *db, vector<string> &tableNames, VNetOrch *vnetOrch)
                                  : Orch2(db, tableNames, request_), vnet_orch_(vnetOrch), bfd_session_producer_(db, APP_BFD_SESSION_TABLE_NAME)
{
    SWSS_LOG_ENTER();

    handler_map_.insert(handler_pair(APP_VNET_RT_TABLE_NAME, &VNetRouteOrch::handleRoutes));
    handler_map_.insert(handler_pair(APP_VNET_RT_TUNNEL_TABLE_NAME, &VNetRouteOrch::handleTunnel));

    state_db_ = shared_ptr<DBConnector>(new DBConnector("STATE_DB", 0));
    state_vnet_rt_tunnel_table_ = unique_ptr<Table>(new Table(state_db_.get(), STATE_VNET_RT_TUNNEL_TABLE_NAME));

    gBfdOrch->attach(this);
}

bool VNetRouteOrch::hasNextHopGroup(const string& vnet, const NextHopGroupKey& nexthops)
{
    return syncd_nexthop_groups_[vnet].find(nexthops) != syncd_nexthop_groups_[vnet].end();
}

sai_object_id_t VNetRouteOrch::getNextHopGroupId(const string& vnet, const NextHopGroupKey& nexthops)
{
    assert(hasNextHopGroup(vnet, nexthops));
    return syncd_nexthop_groups_[vnet][nexthops].next_hop_group_id;
}

bool VNetRouteOrch::addNextHopGroup(const string& vnet, const NextHopGroupKey &nexthops, VNetVrfObject *vrf_obj)
{
    SWSS_LOG_ENTER();

    assert(!hasNextHopGroup(vnet, nexthops));

    if (!gRouteOrch->checkNextHopGroupCount())
    {
        SWSS_LOG_ERROR("Reached maximum number of next hop groups. Failed to create new next hop group.");
        return false;
    }

    vector<sai_object_id_t> next_hop_ids;
    set<NextHopKey> next_hop_set = nexthops.getNextHops();
    std::map<sai_object_id_t, NextHopKey> nhopgroup_members_set;

    for (auto it : next_hop_set)
    {
        if (nexthop_info_[vnet].find(it.ip_address) != nexthop_info_[vnet].end() && nexthop_info_[vnet][it.ip_address].bfd_state != SAI_BFD_SESSION_STATE_UP)
        {
            continue;
        }
        sai_object_id_t next_hop_id = vrf_obj->getTunnelNextHop(it);
        next_hop_ids.push_back(next_hop_id);
        nhopgroup_members_set[next_hop_id] = it;
    }

    sai_attribute_t nhg_attr;
    vector<sai_attribute_t> nhg_attrs;

    nhg_attr.id = SAI_NEXT_HOP_GROUP_ATTR_TYPE;
    nhg_attr.value.s32 = SAI_NEXT_HOP_GROUP_TYPE_ECMP;
    nhg_attrs.push_back(nhg_attr);

    sai_object_id_t next_hop_group_id;
    sai_status_t status = sai_next_hop_group_api->create_next_hop_group(&next_hop_group_id,
                                                                        gSwitchId,
                                                                        (uint32_t)nhg_attrs.size(),
                                                                        nhg_attrs.data());

    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to create next hop group %s, rv:%d",
                       nexthops.to_string().c_str(), status);
        return false;
    }

    gRouteOrch->increaseNextHopGroupCount();
    gCrmOrch->incCrmResUsedCounter(CrmResourceType::CRM_NEXTHOP_GROUP);
    SWSS_LOG_NOTICE("Create next hop group %s", nexthops.to_string().c_str());

    NextHopGroupInfo next_hop_group_entry;
    next_hop_group_entry.next_hop_group_id = next_hop_group_id;

    for (auto nhid: next_hop_ids)
    {
        // Create a next hop group member
        vector<sai_attribute_t> nhgm_attrs;

        sai_attribute_t nhgm_attr;
        nhgm_attr.id = SAI_NEXT_HOP_GROUP_MEMBER_ATTR_NEXT_HOP_GROUP_ID;
        nhgm_attr.value.oid = next_hop_group_id;
        nhgm_attrs.push_back(nhgm_attr);

        nhgm_attr.id = SAI_NEXT_HOP_GROUP_MEMBER_ATTR_NEXT_HOP_ID;
        nhgm_attr.value.oid = nhid;
        nhgm_attrs.push_back(nhgm_attr);

        sai_object_id_t next_hop_group_member_id;
        status = sai_next_hop_group_api->create_next_hop_group_member(&next_hop_group_member_id,
                                                                    gSwitchId,
                                                                    (uint32_t)nhgm_attrs.size(),
                                                                    nhgm_attrs.data());

        if (status != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_ERROR("Failed to create next hop group %" PRIx64 " member %" PRIx64 ": %d\n",
                           next_hop_group_id, next_hop_group_member_id, status);
            return false;
        }

        gCrmOrch->incCrmResUsedCounter(CrmResourceType::CRM_NEXTHOP_GROUP_MEMBER);

        // Save the membership into next hop structure
        next_hop_group_entry.active_members[nhopgroup_members_set.find(nhid)->second] =
                                                                next_hop_group_member_id;
    }

    /*
     * Initialize the next hop group structure with ref_count as 0. This
     * count will increase once the route is successfully syncd.
     */
    next_hop_group_entry.ref_count = 0;
    syncd_nexthop_groups_[vnet][nexthops] = next_hop_group_entry;

    return true;
}

bool VNetRouteOrch::removeNextHopGroup(const string& vnet, const NextHopGroupKey &nexthops, VNetVrfObject *vrf_obj)
{
    SWSS_LOG_ENTER();

    sai_object_id_t next_hop_group_id;
    auto next_hop_group_entry = syncd_nexthop_groups_[vnet].find(nexthops);
    sai_status_t status;

    assert(next_hop_group_entry != syncd_nexthop_groups_[vnet].end());

    if (next_hop_group_entry->second.ref_count != 0)
    {
        return true;
    }

    next_hop_group_id = next_hop_group_entry->second.next_hop_group_id;
    SWSS_LOG_NOTICE("Delete next hop group %s", nexthops.to_string().c_str());

    for (auto nhop = next_hop_group_entry->second.active_members.begin();
         nhop != next_hop_group_entry->second.active_members.end();)
    {
        NextHopKey nexthop = nhop->first;

        status = sai_next_hop_group_api->remove_next_hop_group_member(nhop->second);
        if (status != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_ERROR("Failed to remove next hop group member %" PRIx64 ", rv:%d",
                           nhop->second, status);
            return false;
        }

        vrf_obj->removeTunnelNextHop(nexthop);

        gCrmOrch->decCrmResUsedCounter(CrmResourceType::CRM_NEXTHOP_GROUP_MEMBER);
        nhop = next_hop_group_entry->second.active_members.erase(nhop);
    }

    status = sai_next_hop_group_api->remove_next_hop_group(next_hop_group_id);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to remove next hop group %" PRIx64 ", rv:%d", next_hop_group_id, status);
        return false;
    }

    gRouteOrch->decreaseNextHopGroupCount();
    gCrmOrch->decCrmResUsedCounter(CrmResourceType::CRM_NEXTHOP_GROUP);

    syncd_nexthop_groups_[vnet].erase(nexthops);

    return true;
}

template<>
bool VNetRouteOrch::doRouteTask<VNetVrfObject>(const string& vnet, IpPrefix& ipPrefix,
                                               NextHopGroupKey& nexthops, string& op, 
                                               const map<NextHopKey, IpAddress>& monitors)
{
    SWSS_LOG_ENTER();

    if (!vnet_orch_->isVnetExists(vnet))
    {
        SWSS_LOG_WARN("VNET %s doesn't exist for prefix %s, op %s",
                      vnet.c_str(), ipPrefix.to_string().c_str(), op.c_str());
        return (op == DEL_COMMAND)?true:false;
    }

    set<sai_object_id_t> vr_set;
    auto& peer_list = vnet_orch_->getPeerList(vnet);

    auto l_fn = [&] (const string& vnet) {
        auto *vnet_obj = vnet_orch_->getTypePtr<VNetVrfObject>(vnet);
        sai_object_id_t vr_id = vnet_obj->getVRidIngress();
        vr_set.insert(vr_id);
    };

    l_fn(vnet);
    for (auto peer : peer_list)
    {
        if (!vnet_orch_->isVnetExists(peer))
        {
            SWSS_LOG_INFO("Peer VNET %s not yet created", peer.c_str());
            return false;
        }
        l_fn(peer);
    }

    auto *vrf_obj = vnet_orch_->getTypePtr<VNetVrfObject>(vnet);
    sai_ip_prefix_t pfx;
    copy(pfx, ipPrefix);

    if (op == SET_COMMAND)
    {
        sai_object_id_t nh_id;
        if (!hasNextHopGroup(vnet, nexthops))
        {
            setEndpointMonitor(vnet, monitors, nexthops);
            if (nexthops.getSize() == 1)
            {
                NextHopKey nexthop(nexthops.to_string(), true);
                NextHopGroupInfo next_hop_group_entry;
                next_hop_group_entry.next_hop_group_id = vrf_obj->getTunnelNextHop(nexthop);
                next_hop_group_entry.ref_count = 0;
                next_hop_group_entry.active_members[nexthop] = SAI_NULL_OBJECT_ID;
                syncd_nexthop_groups_[vnet][nexthops] = next_hop_group_entry;
            }
            else
            {
                if (!addNextHopGroup(vnet, nexthops, vrf_obj))
                {
                    delEndpointMonitor(vnet, nexthops);
                    SWSS_LOG_ERROR("Failed to create next hop group %s", nexthops.to_string().c_str());
                    return false;
                }
            }
        }
        nh_id = syncd_nexthop_groups_[vnet][nexthops].next_hop_group_id;

        auto it_route = syncd_tunnel_routes_[vnet].find(ipPrefix);
        for (auto vr_id : vr_set)
        {
            bool route_status = true;

            // Remove route if the nexthop group has no active endpoint
            if (syncd_nexthop_groups_[vnet][nexthops].active_members.empty())
            {
                if (it_route != syncd_tunnel_routes_[vnet].end())
                {
                    NextHopGroupKey nhg = it_route->second;
                    // Remove route when updating from a nhg with active member to another nhg without
                    if (!syncd_nexthop_groups_[vnet][nhg].active_members.empty())
                    {
                        del_route(vr_id, pfx);
                    }
                }
            }
            else
            {
                if (it_route == syncd_tunnel_routes_[vnet].end())
                {
                    route_status = add_route(vr_id, pfx, nh_id);
                }
                else
                {
                    NextHopGroupKey nhg = it_route->second;
                    if (syncd_nexthop_groups_[vnet][nhg].active_members.empty())
                    {
                        route_status = add_route(vr_id, pfx, nh_id);
                    } 
                    else 
                    {
                        route_status = update_route(vr_id, pfx, nh_id);
                    }
                }
            }

            if (!route_status)
            {
                SWSS_LOG_ERROR("Route add/update failed for %s, vr_id '0x%" PRIx64, ipPrefix.to_string().c_str(), vr_id);
                /* Clean up the newly created next hop group entry */
                if (nexthops.getSize() > 1)
                {
                    removeNextHopGroup(vnet, nexthops, vrf_obj);
                }
                return false;
            }
        }

        if (it_route != syncd_tunnel_routes_[vnet].end())
        {
            // In case of updating an existing route, decrease the reference count for the previous nexthop group
            NextHopGroupKey nhg = it_route->second;
            if(--syncd_nexthop_groups_[vnet][nhg].ref_count == 0)
            {
                if (nexthops.getSize() > 1)
                {
                    removeNextHopGroup(vnet, nhg, vrf_obj);
                }
                else
                {
                    syncd_nexthop_groups_[vnet].erase(nhg);
                    NextHopKey nexthop(nhg.to_string(), true);
                    vrf_obj->removeTunnelNextHop(nexthop);
                }
                delEndpointMonitor(vnet, nhg);
            }
            else
            {
                syncd_nexthop_groups_[vnet][nhg].tunnel_routes.erase(ipPrefix);
            }
            vrf_obj->removeRoute(ipPrefix);
        }

        syncd_nexthop_groups_[vnet][nexthops].tunnel_routes.insert(ipPrefix);

        syncd_tunnel_routes_[vnet][ipPrefix] = nexthops;
        syncd_nexthop_groups_[vnet][nexthops].ref_count++;
        vrf_obj->addRoute(ipPrefix, nexthops);

        postRouteState(vnet, ipPrefix, nexthops);
    }
    else if (op == DEL_COMMAND)
    {
        auto it_route = syncd_tunnel_routes_[vnet].find(ipPrefix);
        if (it_route == syncd_tunnel_routes_[vnet].end())
        {
            SWSS_LOG_INFO("Failed to find tunnel route entry, prefix %s\n",
                ipPrefix.to_string().c_str());
            return true;
        }
        NextHopGroupKey nhg = it_route->second;

        for (auto vr_id : vr_set)
        {
            // If an nhg has no active member, the route should already be removed
            if (!syncd_nexthop_groups_[vnet][nhg].active_members.empty())
            {
                if (!del_route(vr_id, pfx))
                {
                    SWSS_LOG_ERROR("Route del failed for %s, vr_id '0x%" PRIx64, ipPrefix.to_string().c_str(), vr_id);
                    return false;
                }
            }
        }

        if(--syncd_nexthop_groups_[vnet][nhg].ref_count == 0)
        {
            if (nhg.getSize() > 1)
            {
                removeNextHopGroup(vnet, nhg, vrf_obj);
            }
            else
            {
                syncd_nexthop_groups_[vnet].erase(nhg);
                NextHopKey nexthop(nhg.to_string(), true);
                vrf_obj->removeTunnelNextHop(nexthop);
            }
            delEndpointMonitor(vnet, nhg);
        }
        else
        {
            syncd_nexthop_groups_[vnet][nhg].tunnel_routes.erase(ipPrefix);
        }

        syncd_tunnel_routes_[vnet].erase(ipPrefix);
        if (syncd_tunnel_routes_[vnet].empty())
        {
            syncd_tunnel_routes_.erase(vnet);
        }

        vrf_obj->removeRoute(ipPrefix);

        removeRouteState(vnet, ipPrefix);
    }

    return true;
}

bool VNetRouteOrch::updateTunnelRoute(const string& vnet, IpPrefix& ipPrefix,
                                NextHopGroupKey& nexthops, string& op)
{
    SWSS_LOG_ENTER();

    if (!vnet_orch_->isVnetExists(vnet))
    {
        SWSS_LOG_WARN("VNET %s doesn't exist for prefix %s, op %s",
                      vnet.c_str(), ipPrefix.to_string().c_str(), op.c_str());
        return (op == DEL_COMMAND)?true:false;
    }

    set<sai_object_id_t> vr_set;
    auto& peer_list = vnet_orch_->getPeerList(vnet);

    auto l_fn = [&] (const string& vnet) {
        auto *vnet_obj = vnet_orch_->getTypePtr<VNetVrfObject>(vnet);
        sai_object_id_t vr_id = vnet_obj->getVRidIngress();
        vr_set.insert(vr_id);
    };

    l_fn(vnet);
    for (auto peer : peer_list)
    {
        if (!vnet_orch_->isVnetExists(peer))
        {
            SWSS_LOG_INFO("Peer VNET %s not yet created", peer.c_str());
            return false;
        }
        l_fn(peer);
    }

    sai_ip_prefix_t pfx;
    copy(pfx, ipPrefix);

    if (op == SET_COMMAND)
    {
        sai_object_id_t nh_id = syncd_nexthop_groups_[vnet][nexthops].next_hop_group_id;

        for (auto vr_id : vr_set)
        {
            bool route_status = true;

            route_status = add_route(vr_id, pfx, nh_id);

            if (!route_status)
            {
                SWSS_LOG_ERROR("Route add failed for %s, vr_id '0x%" PRIx64, ipPrefix.to_string().c_str(), vr_id);
                return false;
            }
        }
    }
    else if (op == DEL_COMMAND)
    {
        auto it_route = syncd_tunnel_routes_[vnet].find(ipPrefix);
        if (it_route == syncd_tunnel_routes_[vnet].end())
        {
            SWSS_LOG_INFO("Failed to find tunnel route entry, prefix %s\n",
                ipPrefix.to_string().c_str());
            return true;
        }
        NextHopGroupKey nhg = it_route->second;

        for (auto vr_id : vr_set)
        {
            if (!del_route(vr_id, pfx))
            {
                SWSS_LOG_ERROR("Route del failed for %s, vr_id '0x%" PRIx64, ipPrefix.to_string().c_str(), vr_id);
                return false;
            }
        }
    }

    return true;
}

template<>
bool VNetRouteOrch::doRouteTask<VNetVrfObject>(const string& vnet, IpPrefix& ipPrefix,
                                               nextHop& nh, string& op)
{
    SWSS_LOG_ENTER();

    if (!vnet_orch_->isVnetExists(vnet))
    {
        SWSS_LOG_WARN("VNET %s doesn't exist for prefix %s, op %s",
                      vnet.c_str(), ipPrefix.to_string().c_str(), op.c_str());
        return (op == DEL_COMMAND)?true:false;
    }

    auto *vrf_obj = vnet_orch_->getTypePtr<VNetVrfObject>(vnet);
    if (op == DEL_COMMAND && !vrf_obj->getRouteNextHop(ipPrefix, nh))
    {
        SWSS_LOG_WARN("VNET %s, Route %s get NH failed", vnet.c_str(), ipPrefix.to_string().c_str());
        return true;
    }

    bool is_subnet = (!nh.ips.getSize() || nh.ips.contains("0.0.0.0")) ? true : false;

    Port port;
    if (is_subnet && (!gPortsOrch->getPort(nh.ifname, port) || (port.m_rif_id == SAI_NULL_OBJECT_ID)))
    {
        SWSS_LOG_WARN("Port/RIF %s doesn't exist", nh.ifname.c_str());
        return false;
    }

    set<sai_object_id_t> vr_set;
    auto& peer_list = vnet_orch_->getPeerList(vnet);
    auto vr_id = vrf_obj->getVRidIngress();

    /*
     * If RIF doesn't belong to this VRF, and if it is a replicated subnet
     * route for the peering VRF, Only install in ingress VRF.
     */

    if (!is_subnet)
    {
        vr_set = vrf_obj->getVRids();
    }
    else if (vr_id == port.m_vr_id)
    {
        vr_set = vrf_obj->getVRids();
    }
    else
    {
        vr_set.insert(vr_id);
    }

    auto l_fn = [&] (const string& vnet) {
        auto *vnet_obj = vnet_orch_->getTypePtr<VNetVrfObject>(vnet);
        sai_object_id_t vr_id = vnet_obj->getVRidIngress();
        vr_set.insert(vr_id);
    };

    for (auto peer : peer_list)
    {
        if (!vnet_orch_->isVnetExists(peer))
        {
            SWSS_LOG_INFO("Peer VNET %s not yet created", peer.c_str());
            return false;
        }
        l_fn(peer);
    }

    sai_ip_prefix_t pfx;
    copy(pfx, ipPrefix);
    sai_object_id_t nh_id=SAI_NULL_OBJECT_ID;

    if (is_subnet)
    {
        nh_id = port.m_rif_id;
    }
    else if (nh.ips.getSize() == 1)
    {
        NextHopKey nexthop(nh.ips.to_string(), nh.ifname);
        if (gNeighOrch->hasNextHop(nexthop))
        {
            nh_id = gNeighOrch->getNextHopId(nexthop);
        }
        else
        {
            SWSS_LOG_INFO("Failed to get next hop %s for %s",
                           nexthop.to_string().c_str(), ipPrefix.to_string().c_str());
            return false;
        }
    }
    else
    {
        // FIXME - Handle ECMP routes
        SWSS_LOG_WARN("VNET ECMP NHs not implemented for '%s'", ipPrefix.to_string().c_str());
        return true;
    }

    for (auto vr_id : vr_set)
    {
        if (vr_id == SAI_NULL_OBJECT_ID)
        {
            continue;
        }
        if (op == SET_COMMAND && !add_route(vr_id, pfx, nh_id))
        {
            SWSS_LOG_INFO("Route add failed for %s", ipPrefix.to_string().c_str());
            break;
        }
        else if (op == DEL_COMMAND && !del_route(vr_id, pfx))
        {
            SWSS_LOG_INFO("Route del failed for %s", ipPrefix.to_string().c_str());
            break;
        }
    }

    if (op == SET_COMMAND)
    {
        vrf_obj->addRoute(ipPrefix, nh);
    }
    else
    {
        vrf_obj->removeRoute(ipPrefix);
    }

    return true;
}

bool VNetRouteOrch::handleRoutes(const Request& request)
{
    SWSS_LOG_ENTER();

    IpAddresses ip_addresses;
    string ifname = "";

    for (const auto& name: request.getAttrFieldNames())
    {
        if (name == "ifname")
        {
            ifname = request.getAttrString(name);
        }
        else if (name == "nexthop")
        {
            auto ipstr = request.getAttrString(name);
            ip_addresses = IpAddresses(ipstr);
        }
        else
        {
            SWSS_LOG_INFO("Unknown attribute: %s", name.c_str());
            continue;
        }
    }

    const std::string& vnet_name = request.getKeyString(0);
    auto ip_pfx = request.getKeyIpPrefix(1);
    auto op = request.getOperation();
    nextHop nh = { ip_addresses, ifname };

    SWSS_LOG_INFO("VNET-RT '%s' op '%s' for ip %s", vnet_name.c_str(),
                   op.c_str(), ip_pfx.to_string().c_str());
              
    if (op == SET_COMMAND)
    {
        addRoute(vnet_name, ip_pfx, nh);
    }
    else
    {
        delRoute(ip_pfx);
    }

    if (vnet_orch_->isVnetExecVrf())
    {
        return doRouteTask<VNetVrfObject>(vnet_name, ip_pfx, nh, op);
    }

    return true;
}

void VNetRouteOrch::attach(Observer* observer, const IpAddress& dstAddr)
{
    SWSS_LOG_ENTER();

    auto insert_result = next_hop_observers_.emplace(dstAddr, VNetNextHopObserverEntry());
    auto observerEntry = insert_result.first;
    /* Create a new observer entry if no current observer is observing this
     * IP address */
    if (insert_result.second)
    {
        /* Find the prefixes that cover the destination IP */
        for (auto route : syncd_routes_)
        {
            if (route.first.isAddressInSubnet(dstAddr))
            {
                SWSS_LOG_INFO("Prefix %s covers destination address",
                    route.first.to_string().c_str());

                observerEntry->second.routeTable.emplace(
                    route.first,
                    route.second
                );
            }
        }
    }

    observerEntry->second.observers.push_back(observer);

    auto bestRoute = observerEntry->second.routeTable.rbegin();
    if (bestRoute != observerEntry->second.routeTable.rend())
    {
        SWSS_LOG_NOTICE("Attached next hop observer of route %s for destination IP %s",
                        bestRoute->first.to_string().c_str(),
                        dstAddr.to_string().c_str());
        for (auto vnetEntry : bestRoute->second)
        {
            VNetNextHopUpdate update = 
            {
                SET_COMMAND,
                vnetEntry.first, // vnet name
                dstAddr, // destination
                bestRoute->first, // prefix
                vnetEntry.second // nexthop
            };
            observer->update(SUBJECT_TYPE_NEXTHOP_CHANGE, reinterpret_cast<void*>(&update));
        }
    }
}

void VNetRouteOrch::detach(Observer* observer, const IpAddress& dstAddr)
{
    SWSS_LOG_ENTER();
    auto observerEntry = next_hop_observers_.find(dstAddr);

    if (observerEntry == next_hop_observers_.end())
    {
        SWSS_LOG_ERROR("Failed to detach observer for %s. Entry not found.", dstAddr.to_string().c_str());
        assert(false);
        return;
    }

    auto iter = std::find(
        observerEntry->second.observers.begin(),
        observerEntry->second.observers.end(),
        observer);
    if (iter == observerEntry->second.observers.end())
    {
        SWSS_LOG_ERROR("Failed to detach observer for %s. Observer not found.", dstAddr.to_string().c_str());
        assert(false);
        return;
    }

    auto bestRoute = observerEntry->second.routeTable.rbegin();
    if (bestRoute != observerEntry->second.routeTable.rend())
    {
        for (auto vnetEntry : bestRoute->second)
        {
            VNetNextHopUpdate update = 
            {
                DEL_COMMAND,
                vnetEntry.first, // vnet name
                dstAddr, // destination
                bestRoute->first, // prefix
                vnetEntry.second // nexthop
            };
            observer->update(SUBJECT_TYPE_NEXTHOP_CHANGE, reinterpret_cast<void*>(&update));
        }
    }
    next_hop_observers_.erase(observerEntry);
}

void VNetRouteOrch::addRoute(const std::string& vnet, const IpPrefix& ipPrefix, const nextHop& nh)
{
    SWSS_LOG_ENTER();
    for (auto& next_hop_observer : next_hop_observers_)
    {
        if (ipPrefix.isAddressInSubnet(next_hop_observer.first))
        {
            auto route_insert_result = next_hop_observer.second.routeTable.emplace(ipPrefix, VNetEntry());            

            auto vnet_result_result = route_insert_result.first->second.emplace(vnet, nh);
            if (!vnet_result_result.second)
            {
                if (vnet_result_result.first->second.ips == nh.ips 
                    && vnet_result_result.first->second.ifname == nh.ifname)
                {
                    continue;
                }
                vnet_result_result.first->second = nh;
            }

            // If the inserted route is the best route. (Table should not be empty. Because we inserted a new entry above)
            if (route_insert_result.first == --next_hop_observer.second.routeTable.end())
            {
                VNetNextHopUpdate update = 
                {
                    SET_COMMAND,
                    vnet, // vnet name
                    next_hop_observer.first, // destination
                    ipPrefix, // prefix
                    nh // nexthop
                };
                for (auto& observer : next_hop_observer.second.observers)
                {
                    observer->update(SUBJECT_TYPE_NEXTHOP_CHANGE, reinterpret_cast<void*>(&update));
                }
            }
        }
    }
    syncd_routes_.emplace(ipPrefix, VNetEntry()).first->second[vnet] = nh;
}

void VNetRouteOrch::delRoute(const IpPrefix& ipPrefix)
{
    SWSS_LOG_ENTER();

    auto route_itr = syncd_routes_.find(ipPrefix);
    if (route_itr == syncd_routes_.end())
    {
        SWSS_LOG_ERROR("Failed to find route %s.", ipPrefix.to_string().c_str());
        assert(false);
        return;
    }
    auto next_hop_observer = next_hop_observers_.begin();
    while(next_hop_observer != next_hop_observers_.end())
    {
        if (ipPrefix.isAddressInSubnet(next_hop_observer->first))
        {
            auto itr = next_hop_observer->second.routeTable.find(ipPrefix);
            if ( itr == next_hop_observer->second.routeTable.end())
            {
                SWSS_LOG_ERROR(
                    "Failed to find any ip(%s) belong to this route(%s).", 
                    next_hop_observer->first.to_string().c_str(),
                    ipPrefix.to_string().c_str());
                assert(false);
                continue;
            }
            if (itr->second.empty())
            {
                continue;
            }
            for (auto& observer : next_hop_observer->second.observers)
            {
                VNetNextHopUpdate update = {
                    DEL_COMMAND,
                    itr->second.rbegin()->first, // vnet name
                    next_hop_observer->first, // destination
                    itr->first, // prefix
                    itr->second.rbegin()->second // nexthop
                };
                observer->update(SUBJECT_TYPE_NEXTHOP_CHANGE, reinterpret_cast<void*>(&update));
            }
            next_hop_observer->second.routeTable.erase(itr);
            if (next_hop_observer->second.routeTable.empty())
            {
                next_hop_observer = next_hop_observers_.erase(next_hop_observer);
                continue;
            }
        }
        next_hop_observer++;
    }
    syncd_routes_.erase(route_itr);
}

void VNetRouteOrch::createBfdSession(const string& vnet, const NextHopKey& endpoint, const IpAddress& monitor_addr)
{
    SWSS_LOG_ENTER();

    IpAddress endpoint_addr = endpoint.ip_address;
    if (nexthop_info_[vnet].find(endpoint_addr) != nexthop_info_[vnet].end())
    {
        SWSS_LOG_ERROR("BFD session for endpoint %s already exist", endpoint_addr.to_string().c_str());
        return;
    }

    if (bfd_sessions_.find(monitor_addr) == bfd_sessions_.end())
    {
        vector<FieldValueTuple>    data;
        string key = "default:default:" + monitor_addr.to_string();

        auto tun_name = vnet_orch_->getTunnelName(vnet);
        VxlanTunnelOrch* vxlan_orch = gDirectory.get<VxlanTunnelOrch*>();
        auto tunnel_obj = vxlan_orch->getVxlanTunnel(tun_name);
        IpAddress src_ip = tunnel_obj->getSrcIP();

        FieldValueTuple fvTuple("local_addr", src_ip.to_string());
        data.push_back(fvTuple);

        bfd_session_producer_.set(key, data);

        bfd_sessions_[monitor_addr].bfd_state = SAI_BFD_SESSION_STATE_DOWN;
    }

    BfdSessionInfo& bfd_info = bfd_sessions_[monitor_addr];
    bfd_info.vnet = vnet;
    bfd_info.endpoint = endpoint;
    VNetNextHopInfo nexthop_info;
    nexthop_info.monitor_addr = monitor_addr;
    nexthop_info.bfd_state = bfd_info.bfd_state;
    nexthop_info.ref_count = 0;
    nexthop_info_[vnet][endpoint_addr] = nexthop_info;
}

void VNetRouteOrch::removeBfdSession(const string& vnet, const NextHopKey& endpoint, const IpAddress& monitor_addr)
{
    SWSS_LOG_ENTER();

    IpAddress endpoint_addr = endpoint.ip_address;
    if (nexthop_info_[vnet].find(endpoint_addr) == nexthop_info_[vnet].end())
    {
        SWSS_LOG_ERROR("BFD session for endpoint %s does not exist", endpoint_addr.to_string().c_str());
    }
    nexthop_info_[vnet].erase(endpoint_addr);

    string key = "default:default:" + monitor_addr.to_string();

    bfd_session_producer_.del(key);

    bfd_sessions_.erase(monitor_addr);
}

void VNetRouteOrch::setEndpointMonitor(const string& vnet, const map<NextHopKey, IpAddress>& monitors, NextHopGroupKey& nexthops)
{
    SWSS_LOG_ENTER();

    for (auto monitor : monitors)
    {
        NextHopKey nh = monitor.first;
        IpAddress monitor_ip = monitor.second;
        if (nexthop_info_[vnet].find(nh.ip_address) == nexthop_info_[vnet].end())
        {
            createBfdSession(vnet, nh, monitor_ip);
        }

        nexthop_info_[vnet][nh.ip_address].ref_count++;
    }
}

void VNetRouteOrch::delEndpointMonitor(const string& vnet, NextHopGroupKey& nexthops)
{
    SWSS_LOG_ENTER();

    std::set<NextHopKey> nhks = nexthops.getNextHops();
    for (auto nhk: nhks)
    {
        IpAddress ip = nhk.ip_address;
        if (nexthop_info_[vnet].find(ip) != nexthop_info_[vnet].end()) {
            if (--nexthop_info_[vnet][ip].ref_count == 0)
            {
                removeBfdSession(vnet, nhk, nexthop_info_[vnet][ip].monitor_addr);
            }
        }
    }
}

void VNetRouteOrch::postRouteState(const string& vnet, IpPrefix& ipPrefix, NextHopGroupKey& nexthops)
{
    const string state_db_key = vnet + state_db_key_delimiter + ipPrefix.to_string();
    vector<FieldValueTuple> fvVector;

    NextHopGroupInfo& nhg_info = syncd_nexthop_groups_[vnet][nexthops];
    string route_state = nhg_info.active_members.empty() ? "inactive" : "active";
    string ep_str = "";
    int idx_ep = 0;
    for (auto nh_pair : nhg_info.active_members)
    {
        NextHopKey nh = nh_pair.first;
        ep_str += idx_ep == 0 ? nh.ip_address.to_string() : "," + nh.ip_address.to_string();
        idx_ep++;
    }

    fvVector.emplace_back("active_endpoints", ep_str);
    fvVector.emplace_back("state", route_state);

    state_vnet_rt_tunnel_table_->set(state_db_key, fvVector);
}

void VNetRouteOrch::removeRouteState(const string& vnet, IpPrefix& ipPrefix)
{
    const string state_db_key = vnet + state_db_key_delimiter + ipPrefix.to_string();
    state_vnet_rt_tunnel_table_->del(state_db_key);
}

void VNetRouteOrch::update(SubjectType type, void *cntx)
{
    SWSS_LOG_ENTER();

    assert(cntx);

    switch(type) {
    case SUBJECT_TYPE_BFD_SESSION_STATE_CHANGE:
    {
        BfdUpdate *update = static_cast<BfdUpdate *>(cntx);
        updateVnetTunnel(*update);
        break;
    }
    default:
        // Received update in which we are not interested
        // Ignore it
        return;
    }
}

void VNetRouteOrch::updateVnetTunnel(const BfdUpdate& update)
{
    SWSS_LOG_ENTER();

    auto key = update.peer;
    sai_bfd_session_state_t state = update.state;

    size_t found_vrf = key.find(state_db_key_delimiter);
    if (found_vrf == string::npos)
    {
        SWSS_LOG_ERROR("Failed to parse key %s, no vrf is given", key.c_str());
        return;
    }

    size_t found_ifname = key.find(state_db_key_delimiter, found_vrf + 1);
    if (found_ifname == string::npos)
    {
        SWSS_LOG_ERROR("Failed to parse key %s, no ifname is given", key.c_str());
        return;
    }

    string vrf_name = key.substr(0, found_vrf);
    string alias = key.substr(found_vrf + 1, found_ifname - found_vrf - 1);
    IpAddress peer_address(key.substr(found_ifname + 1));

    if (alias != "default" || vrf_name != "default")
    {
        return;
    }

    auto it_peer = bfd_sessions_.find(peer_address);

    if (it_peer == bfd_sessions_.end()) {
        SWSS_LOG_INFO("No endpoint for BFD peer %s", peer_address.to_string().c_str());
        return;
    }

    BfdSessionInfo& bfd_info = it_peer->second;
    bfd_info.bfd_state = state;

    string vnet = bfd_info.vnet;
    NextHopKey endpoint = bfd_info.endpoint;
    auto *vrf_obj = vnet_orch_->getTypePtr<VNetVrfObject>(vnet);

    if (syncd_nexthop_groups_.find(vnet) == syncd_nexthop_groups_.end())
    {
        SWSS_LOG_ERROR("Vnet %s not found", vnet.c_str());
        return;
    }

    nexthop_info_[vnet][endpoint.ip_address].bfd_state = state;

    for (auto& nhg_info_pair : syncd_nexthop_groups_[vnet])
    {
        NextHopGroupKey nexthops = nhg_info_pair.first;
        NextHopGroupInfo& nhg_info = nhg_info_pair.second;

        if (!(nexthops.contains(endpoint)))
        {
            continue;
        }

        if (state == SAI_BFD_SESSION_STATE_UP)
        {
            sai_object_id_t next_hop_group_member_id = SAI_NULL_OBJECT_ID;
            if (nexthops.getSize() > 1)
            {
                // Create a next hop group member
                vector<sai_attribute_t> nhgm_attrs;

                sai_attribute_t nhgm_attr;
                nhgm_attr.id = SAI_NEXT_HOP_GROUP_MEMBER_ATTR_NEXT_HOP_GROUP_ID;
                nhgm_attr.value.oid = nhg_info.next_hop_group_id;
                nhgm_attrs.push_back(nhgm_attr);

                nhgm_attr.id = SAI_NEXT_HOP_GROUP_MEMBER_ATTR_NEXT_HOP_ID;
                nhgm_attr.value.oid = vrf_obj->getTunnelNextHop(endpoint);
                nhgm_attrs.push_back(nhgm_attr);

                sai_status_t status = sai_next_hop_group_api->create_next_hop_group_member(&next_hop_group_member_id,
                                                                                gSwitchId,
                                                                                (uint32_t)nhgm_attrs.size(),
                                                                                nhgm_attrs.data());

                if (status != SAI_STATUS_SUCCESS)
                {
                    SWSS_LOG_ERROR("Failed to add next hop member to group %" PRIx64 ": %d\n",
                                    nhg_info.next_hop_group_id, status);
                    task_process_status handle_status = handleSaiCreateStatus(SAI_API_NEXT_HOP_GROUP, status);
                    if (handle_status != task_success)
                    {
                        continue;
                    }
                }

                gCrmOrch->incCrmResUsedCounter(CrmResourceType::CRM_NEXTHOP_GROUP_MEMBER);
            }

            // Re-create routes when it was temporarily removed
            if (nhg_info.active_members.empty())
            {
                nhg_info.active_members[endpoint] = next_hop_group_member_id;
                if (vnet_orch_->isVnetExecVrf())
                {
                    for (auto ip_pfx : syncd_nexthop_groups_[vnet][nexthops].tunnel_routes)
                    {
                        string op = SET_COMMAND;
                        updateTunnelRoute(vnet, ip_pfx, nexthops, op);
                    }
                }
            }
            else
            {
                nhg_info.active_members[endpoint] = next_hop_group_member_id;
            }
        }
        else
        {
            if (nexthops.getSize() > 1 && nhg_info.active_members.find(endpoint) != nhg_info.active_members.end())
            {
                sai_object_id_t nexthop_id = nhg_info.active_members[endpoint];
                sai_status_t status = sai_next_hop_group_api->remove_next_hop_group_member(nexthop_id);

                if (status != SAI_STATUS_SUCCESS)
                {
                    SWSS_LOG_ERROR("Failed to remove next hop member %" PRIx64 " from group %" PRIx64 ": %d\n",
                                nexthop_id, nhg_info.next_hop_group_id, status);
                    task_process_status handle_status = handleSaiRemoveStatus(SAI_API_NEXT_HOP_GROUP, status);
                    if (handle_status != task_success)
                    {
                        continue;
                    }
                }

                vrf_obj->removeTunnelNextHop(endpoint);

                gCrmOrch->decCrmResUsedCounter(CrmResourceType::CRM_NEXTHOP_GROUP_MEMBER);
            }

            if (nhg_info.active_members.find(endpoint) != nhg_info.active_members.end())
            {
                nhg_info.active_members.erase(endpoint);

                // Remove routes when nexthop group has no active endpoint
                if (nhg_info.active_members.empty())
                {
                    if (vnet_orch_->isVnetExecVrf())
                    {
                        for (auto ip_pfx : syncd_nexthop_groups_[vnet][nexthops].tunnel_routes)
                        {
                            string op = DEL_COMMAND;
                            updateTunnelRoute(vnet, ip_pfx, nexthops, op);
                        }
                    }
                }
            }
        }

        // Post configured in State DB
        for (auto ip_pfx : syncd_nexthop_groups_[vnet][nexthops].tunnel_routes)
        {
            postRouteState(vnet, ip_pfx, nexthops);
        }
    }
}

bool VNetRouteOrch::handleTunnel(const Request& request)
{
    SWSS_LOG_ENTER();

    vector<IpAddress> ip_list;
    vector<string> mac_list;
    vector<string> vni_list;
    vector<IpAddress> monitor_list;

    for (const auto& name: request.getAttrFieldNames())
    {
        if (name == "endpoint")
        {
            ip_list = request.getAttrIPList(name);
        }
        else if (name == "vni")
        {
            string vni_str = request.getAttrString(name);
            vni_list = tokenize(vni_str, ',');
        }
        else if (name == "mac_address")
        {
            string mac_str = request.getAttrString(name);
            mac_list = tokenize(mac_str, ',');
        }
        else if (name == "endpoint_monitor")
        {
            monitor_list = request.getAttrIPList(name);
        }
        else
        {
            SWSS_LOG_INFO("Unknown attribute: %s", name.c_str());
            continue;
        }
    }

    if (vni_list.size() > 1 && vni_list.size() != ip_list.size())
    {
        SWSS_LOG_ERROR("VNI size of %zu does not match endpoint size of %zu", vni_list.size(), ip_list.size());
        return false;
    }

    if (!mac_list.empty() && mac_list.size() != ip_list.size())
    {
        SWSS_LOG_ERROR("MAC address size of %zu does not match endpoint size of %zu", mac_list.size(), ip_list.size());
        return false;
    }

    if (!monitor_list.empty() && monitor_list.size() != ip_list.size())
    {
        SWSS_LOG_ERROR("Peer monitor size of %zu does not match endpoint size of %zu", monitor_list.size(), ip_list.size());
        return false;
    }
    
    const std::string& vnet_name = request.getKeyString(0);
    auto ip_pfx = request.getKeyIpPrefix(1);
    auto op = request.getOperation();

    SWSS_LOG_INFO("VNET-RT '%s' op '%s' for pfx %s", vnet_name.c_str(),
                   op.c_str(), ip_pfx.to_string().c_str());

    NextHopGroupKey nhg("", true);
    map<NextHopKey, IpAddress> monitors;
    for (size_t idx_ip = 0; idx_ip < ip_list.size(); idx_ip++)
    {
        IpAddress ip = ip_list[idx_ip];
        MacAddress mac;
        uint32_t vni = 0;
        if (vni_list.size() == 1 && vni_list[0] != "")
        {
            vni = (uint32_t)stoul(vni_list[0]);
        }
        else if (vni_list.size() > 1 && vni_list[idx_ip] != "")
        {
            vni = (uint32_t)stoul(vni_list[idx_ip]);
        }

        if (!mac_list.empty() && mac_list[idx_ip] != "")
        {
            mac = MacAddress(mac_list[idx_ip]);
        }

        NextHopKey nh(ip, mac, vni, true);
        nhg.add(nh);
        if (!monitor_list.empty())
        {
            monitors[nh] = monitor_list[idx_ip];
        }
    }

    if (vnet_orch_->isVnetExecVrf())
    {
        return doRouteTask<VNetVrfObject>(vnet_name, ip_pfx, nhg, op, monitors);
    }

    return true;
}

bool VNetRouteOrch::addOperation(const Request& request)
{
    SWSS_LOG_ENTER();

    try
    {
        auto& tn = request.getTableName();
        if (handler_map_.find(tn) == handler_map_.end())
        {
            SWSS_LOG_ERROR(" %s handler is not initialized", tn.c_str());
            return true;
        }

        return ((this->*(handler_map_[tn]))(request));
    }
    catch(std::runtime_error& _)
    {
        SWSS_LOG_ERROR("VNET add operation error %s ", _.what());
        return true;
    }

    return true;
}

bool VNetRouteOrch::delOperation(const Request& request)
{
    SWSS_LOG_ENTER();

    try
    {
        auto& tn = request.getTableName();
        if (handler_map_.find(tn) == handler_map_.end())
        {
            SWSS_LOG_ERROR(" %s handler is not initialized", tn.c_str());
            return true;
        }

        return ((this->*(handler_map_[tn]))(request));
    }
    catch(std::runtime_error& _)
    {
        SWSS_LOG_ERROR("VNET del operation error %s ", _.what());
        return true;
    }

    return true;
}

VNetCfgRouteOrch::VNetCfgRouteOrch(DBConnector *db, DBConnector *appDb, vector<string> &tableNames)
                                  : Orch(db, tableNames),
                                  m_appVnetRouteTable(appDb, APP_VNET_RT_TABLE_NAME),
                                  m_appVnetRouteTunnelTable(appDb, APP_VNET_RT_TUNNEL_TABLE_NAME)
{
}

void VNetCfgRouteOrch::doTask(Consumer &consumer)
{
    SWSS_LOG_ENTER();

    const string & table_name = consumer.getTableName();
    auto it = consumer.m_toSync.begin();

    while (it != consumer.m_toSync.end())
    {
        bool task_result = false;
        auto t = it->second;
        const string & op = kfvOp(t);
        if (table_name == CFG_VNET_RT_TABLE_NAME)
        {
            task_result = doVnetRouteTask(t, op);
        }
        else if (table_name == CFG_VNET_RT_TUNNEL_TABLE_NAME)
        {
            task_result = doVnetTunnelRouteTask(t, op);
        }
        else
        {
            SWSS_LOG_ERROR("Unknown table : %s", table_name.c_str());
        }

        if (task_result == true)
        {
            it = consumer.m_toSync.erase(it);
        }
        else
        {
            ++it;
        }
    }
}

bool VNetCfgRouteOrch::doVnetTunnelRouteTask(const KeyOpFieldsValuesTuple & t, const string & op)
{
    SWSS_LOG_ENTER();

    string vnetRouteTunnelName = kfvKey(t);
    replace(vnetRouteTunnelName.begin(), vnetRouteTunnelName.end(), config_db_key_delimiter, delimiter);
    if (op == SET_COMMAND)
    {
        m_appVnetRouteTunnelTable.set(vnetRouteTunnelName, kfvFieldsValues(t));
        SWSS_LOG_INFO("Create vnet route tunnel %s", vnetRouteTunnelName.c_str());
    }
    else if (op == DEL_COMMAND)
    {
        m_appVnetRouteTunnelTable.del(vnetRouteTunnelName);
        SWSS_LOG_INFO("Delete vnet route tunnel %s", vnetRouteTunnelName.c_str());
    }
    else
    {
        SWSS_LOG_ERROR("Unknown command : %s", op.c_str());
        return false;
    }

    return true;
}

bool VNetCfgRouteOrch::doVnetRouteTask(const KeyOpFieldsValuesTuple & t, const string & op)
{
    SWSS_LOG_ENTER();

    string vnetRouteName = kfvKey(t);
    replace(vnetRouteName.begin(), vnetRouteName.end(), config_db_key_delimiter, delimiter);
    if (op == SET_COMMAND)
    {
        m_appVnetRouteTable.set(vnetRouteName, kfvFieldsValues(t));
        SWSS_LOG_INFO("Create vnet route %s", vnetRouteName.c_str());
    }
    else if (op == DEL_COMMAND)
    {
        m_appVnetRouteTable.del(vnetRouteName);
        SWSS_LOG_INFO("Delete vnet route %s", vnetRouteName.c_str());
    }
    else
    {
        SWSS_LOG_ERROR("Unknown command : %s", op.c_str());
        return false;
    }

    return true;
}
