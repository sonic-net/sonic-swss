#include <cassert>
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <exception>
#include <inttypes.h>
#include <algorithm>
#include <numeric>

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
#include "tunneldecaporch.h"
#include "flowcounterrouteorch.h"

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
extern FlowCounterRouteOrch *gFlowCounterRouteOrch;
extern RouteOrch *gRouteOrch;
extern MacAddress gVxlanMacAddress;
extern BfdOrch *gBfdOrch;
extern SwitchOrch *gSwitchOrch;
extern TunnelDecapOrch *gTunneldecapOrch;
extern FgNhgOrch *gFgNhgOrch;
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
        gFlowCounterRouteOrch->onAddVR(router_id);
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

    tunnels_[ipPrefix] = nexthops;

    if (!nexthops.is_overlay_nexthop())
    {
        SWSS_LOG_NOTICE("Input %s is not overlay nexthop group", nexthops.to_string().c_str());
        return false;
    }

    return true;
}

void VNetVrfObject::addProfile(IpPrefix& ipPrefix, string& profile)
{
    profile_[ipPrefix] = profile;
}

void VNetVrfObject::removeProfile(IpPrefix& ipPrefix)
{
    if (profile_.find(ipPrefix) != profile_.end())
    {
        profile_.erase(ipPrefix);
    }
}

string VNetVrfObject::getProfile(IpPrefix& ipPrefix)
{
    if (profile_.find(ipPrefix) != profile_.end())
    {
        return profile_[ipPrefix];
    }
    return string();
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

bool VNetVrfObject::addRoute(IpPrefix& ipPrefix, nextHop& nh, bool increaseRefCount)
{
    if (hasRoute(ipPrefix))
    {
        SWSS_LOG_INFO("VNET route '%s' exists", ipPrefix.to_string().c_str());
        return false;
    }

    if (increaseRefCount)
    {
        increaseNextHopRefCount(nh);
    }
    routes_[ipPrefix] = nh;
    return true;
}

bool VNetVrfObject::removeRoute(IpPrefix& ipPrefix, bool decreaseRefCount)
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
        if (decreaseRefCount)
        {
            decreaseNextHopRefCount(nh);
        }
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
        if (it != gVirtualRouterId)
        {
            sai_status_t status = sai_virtual_router_api->remove_virtual_router(it);
            if (status != SAI_STATUS_SUCCESS)
            {
                SWSS_LOG_ERROR("Failed to remove virtual router name: %s, rv:%d",
                                vnet_name_.c_str(), status);
            }
        }
        gFlowCounterRouteOrch->onRemoveVR(it);
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
    bool peer = false, create = false, advertise_prefix = false;
    uint32_t vni=0;
    string tunnel;
    string scope;
    swss::MacAddress overlay_dmac;

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
        else if (name == "advertise_prefix")
        {
            advertise_prefix = request.getAttrBool("advertise_prefix");
        }
        else if (name == "overlay_dmac")
        {
            overlay_dmac = request.getAttrMacAddress("overlay_dmac");
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
                VNetInfo vnet_info = { tunnel, vni, peer_list, scope, advertise_prefix, overlay_dmac };
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
                if (!!overlay_dmac && overlay_dmac != it->second->getOverlayDMac())
                {
                    it->second->setOverlayDMac(overlay_dmac);
                    VNetRouteOrch* vnet_route_orch = gDirectory.get<VNetRouteOrch*>();
                    vnet_route_orch->updateAllMonitoringSession(vnet_name);
                }
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

bool VNetOrch::getVrfIdByVnetName(const std::string& vnet_name, sai_object_id_t &vrf_id)
{
    if (!isVnetExists(vnet_name))
    {
        return false;
    }

    auto *vrf_obj = getTypePtr<VNetVrfObject>(vnet_name);
    // Now we only support ingress VR for VNET, so just get ingress VR ID
    // Once we support egress VR, need revisit here.
    vrf_id = vrf_obj->getVRidIngress();
    return vrf_id != SAI_NULL_OBJECT_ID;
}

bool VNetOrch::getVnetNameByVrfId(sai_object_id_t vrf_id, std::string& vnet_name)
{
    for (auto &entry : vnet_table_)
    {
        auto *vrf_obj = dynamic_cast<VNetVrfObject *>(entry.second.get());
        if (!vrf_obj)
        {
            continue;
        }

        if (vrf_obj->getVRidIngress() == vrf_id)
        {
            vnet_name = entry.first;
            return true;
        }
    }

    return false;
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

    gFlowCounterRouteOrch->onRemoveMiscRouteEntry(vr_id, ip_pfx, false);

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

    gFlowCounterRouteOrch->onAddMiscRouteEntry(vr_id, ip_pfx, false);

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
                                  : Orch2(db, tableNames, request_), vnet_orch_(vnetOrch), bfd_session_producer_(db, APP_BFD_SESSION_TABLE_NAME),
                                    app_tunnel_decap_term_producer_(db, APP_TUNNEL_DECAP_TERM_TABLE_NAME)
{
    SWSS_LOG_ENTER();

    handler_map_.insert(handler_pair(APP_VNET_RT_TABLE_NAME, &VNetRouteOrch::handleRoutes));
    handler_map_.insert(handler_pair(APP_VNET_RT_TUNNEL_TABLE_NAME, &VNetRouteOrch::handleTunnel));

    config_db_ = shared_ptr<DBConnector>(new DBConnector("CONFIG_DB", 0));
    state_db_ = shared_ptr<DBConnector>(new DBConnector("STATE_DB", 0));
    app_db_ = shared_ptr<DBConnector>(new DBConnector("APPL_DB", 0));

    state_vnet_rt_tunnel_table_ = unique_ptr<Table>(new Table(state_db_.get(), STATE_VNET_RT_TUNNEL_TABLE_NAME));
    state_vnet_rt_adv_table_ = unique_ptr<Table>(new Table(state_db_.get(), STATE_ADVERTISE_NETWORK_TABLE_NAME));
    monitor_session_producer_ = unique_ptr<Table>(new Table(app_db_.get(), APP_VNET_MONITOR_TABLE_NAME));

    vnet_tunnel_term_acl_ = make_shared<VNetTunnelTermAcl>(config_db_.get(), app_db_.get());

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

bool VNetRouteOrch::addNextHopGroup(const string& vnet, const NextHopGroupKey &nexthops, VNetVrfObject *vrf_obj, const string& monitoring, const bool isLocalEp, const uint16 _t consistent_hashing_buckets, IpPrefix &ipPrefix, bool *isNextHopChanged)
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
    std::map<NextHopKey, uint32_t> nh_seq_id_in_nhgrp;
    uint32_t seq_id = 0;

    for (auto it : next_hop_set)
    {
        nh_seq_id_in_nhgrp[it] = ++seq_id;
        if (monitoring != VNET_MONITORING_TYPE_CUSTOM && monitoring != VNET_MONITORING_TYPE_CUSTOM_BFD && nexthop_info_[vnet].find(it.ip_address) != nexthop_info_[vnet].end() && nexthop_info_[vnet][it.ip_address].bfd_state != SAI_BFD_SESSION_STATE_UP)
        {
            continue;
        }
        if (isLocalEp && !gNeighOrch->hasNextHop(it))
        {
            SWSS_LOG_NOTICE("Next hop %s not found in neighorch, skipping.", it.to_string().c_str());
            continue;
        }
        sai_object_id_t next_hop_id = isLocalEp? gNeighOrch->getNextHopId(it):vrf_obj->getTunnelNextHop(it);
        next_hop_ids.push_back(next_hop_id);
        nhopgroup_members_set[next_hop_id] = it;
    }

    bool isFineGrainedNextHopIdChanged = false;

    if (consistent_hashing_buckets > 0 && ipPrefix != nullptr)
    {
        sai_object_id_t vrf_id;
        vnet_orch_->getVrfIdByVnetName(vnet, vrf_id);
        return FgNhgOrch->setFgNhg(vrf_id, *ipPrefix, nhopgroup_members_set, consistent_hashing_buckets, *isNextHopChanged);
    }

    sai_attribute_t nhg_attr;
    vector<sai_attribute_t> nhg_attrs;

    nhg_attr.id = SAI_NEXT_HOP_GROUP_ATTR_TYPE;
    nhg_attr.value.s32 = gSwitchOrch->checkOrderedEcmpEnable() ? SAI_NEXT_HOP_GROUP_TYPE_DYNAMIC_ORDERED_ECMP : SAI_NEXT_HOP_GROUP_TYPE_ECMP;
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

        if (gSwitchOrch->checkOrderedEcmpEnable())
        {
            nhgm_attr.id = SAI_NEXT_HOP_GROUP_MEMBER_ATTR_SEQUENCE_ID;
            nhgm_attr.value.u32 = nh_seq_id_in_nhgrp[nhopgroup_members_set.find(nhid)->second];
            nhgm_attrs.push_back(nhgm_attr);
        }

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

        /* For local endpoint, we don't remove the next hop from NeighOrch,
         * as it is not created by VNetRouteOrch.
        */
        if (!isLocalEndpoint(vnet, nexthop.ip_address))
        {
            vrf_obj->removeTunnelNextHop(nexthop);
        }

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

bool VNetRouteOrch::createNextHopGroup(const string& vnet,
                                       NextHopGroupKey& nexthops,
                                       VNetVrfObject *vrf_obj,
                                       const string& monitoring,
                                       const uint16_t consistent_hashing_buckets,
                                       IpPrefix *ipPrefix)
{
    SWSS_LOG_INFO("Creating nexthop group from nexthops(%s)\n", nexthops.to_string().c_str());
    if (nexthops.getSize() == 0)
    {
        return true;
    }
    else if (nexthops.getSize() == 1)
    {
        NextHopKey nexthop = *nexthops.getNextHops().begin();
        bool isLocalEp = isLocalEndpoint(vnet, nexthop.ip_address);
        if (isLocalEp && !gNeighOrch->hasNextHop(nexthop))
        {
            SWSS_LOG_NOTICE("Next hop %s not found in neighorch, skipping.", nexthop.to_string().c_str());
            return false;
        }
        NextHopGroupInfo next_hop_group_entry;
        if (isLocalEp)
        {
            gNeighOrch->increaseNextHopRefCount(nexthop);
            next_hop_group_entry.next_hop_group_id = gNeighOrch->getNextHopId(nexthop);
            next_hop_group_entry.ref_count = gNeighOrch->getNextHopRefCount(nexthop);
        }
        else
        {
            next_hop_group_entry.next_hop_group_id = vrf_obj->getTunnelNextHop(nexthop);
            next_hop_group_entry.ref_count = 0;
        }

        if (monitoring == VNET_MONITORING_TYPE_CUSTOM || monitoring == VNET_MONITORING_TYPE_CUSTOM_BFD || nexthop_info_[vnet].find(nexthop.ip_address) == nexthop_info_[vnet].end() || nexthop_info_[vnet][nexthop.ip_address].bfd_state == SAI_BFD_SESSION_STATE_UP)
        {
            SWSS_LOG_INFO("Adding nexthop: %s to the active group", nexthop.ip_address.to_string().c_str());
            next_hop_group_entry.active_members[nexthop] = SAI_NULL_OBJECT_ID;
        }
        syncd_nexthop_groups_[vnet][nexthops] = next_hop_group_entry;
    }
    else
    {
        const bool isLocalEp = isLocalEndpoint(vnet, nexthops.getNextHops().begin()->ip_address);
        if (!addNextHopGroup(vnet, nexthops, vrf_obj, monitoring, isLocalEp, consistent_hashing_buckets, ipPrefix))
        {
            SWSS_LOG_ERROR("Failed to create next hop group %s", nexthops.to_string().c_str());
            return false;
        }
    }
    return true;
}

NextHopGroupKey VNetRouteOrch::getActiveNHSet(const string& vnet,
                                       NextHopGroupKey& nexthops,
                                       const IpPrefix& ipPrefix)
{
    // This  function takes  a nexthop group key and iterates over the nexthops in that group
    // to identify the ones which are active based on their monitor session state.
    // These next hops are collected into another next hop group key called nhg_custom and returned.
    NextHopGroupKey nhg_custom("", true);
    set<NextHopKey> next_hop_set = nexthops.getNextHops();
    for (auto it : next_hop_set)
    {
        if(monitor_info_.find(vnet) != monitor_info_.end() &&
            monitor_info_[vnet].find(ipPrefix) != monitor_info_[vnet].end())
        {
            for (auto monitor : monitor_info_[vnet][ipPrefix])
            {
                if (monitor.second.endpoint == it)
                {
                    if (monitor.second.monitoring_type == VNET_MONITORING_TYPE_CUSTOM && monitor.second.state == MONITOR_SESSION_STATE_UP)
                    {
                        // monitor session exists and is up
                        nhg_custom.add(it);

                    }

                    if (monitor.second.monitoring_type == VNET_MONITORING_TYPE_CUSTOM_BFD && monitor.second.custom_bfd_state == SAI_BFD_SESSION_STATE_UP)
                    {
                        // BFD session exists and is up
                        nhg_custom.add(it);
                    }
                    continue;
                }
            }
        }
    }
    return nhg_custom;
}

bool VNetRouteOrch::selectNextHopGroup(const string& vnet,
                                       NextHopGroupKey& nexthops_primary,
                                       NextHopGroupKey& nexthops_secondary,
                                       const string& monitoring,
                                       const int32_t rx_monitor_timer,
                                       const int32_t tx_monitor_timer,
                                       IpPrefix& ipPrefix,
                                       VNetVrfObject *vrf_obj,
                                       NextHopGroupKey& nexthops_selected,
                                       const uint16_t consistent_hashing_buckets,
                                       const map<NextHopKey, IpAddress>& monitors)
{
    // This function returns the next hop group which is to be used to in the hardware.
    // for non priority tunnel routes, this would return nexthops_primary or its subset if
    // BFD sessions for the endpoits in the NHG are up.
    // For priority tunnel scenario, it sets up endpoint monitors for both primary and secondary.
    // This is followed by an attempt to create a NHG which can be subset of nexthops_primary
    // depending on the endpoint monitor state. If no NHG from primary is created, we attempt
    // the same for secondary.

    if(nexthops_secondary.getSize() != 0 &&
        (monitoring == VNET_MONITORING_TYPE_CUSTOM || monitoring == VNET_MONITORING_TYPE_CUSTOM_BFD))
    {
        auto it_route =  syncd_tunnel_routes_[vnet].find(ipPrefix);
        if (it_route == syncd_tunnel_routes_[vnet].end())
        {
            setEndpointMonitor(vnet, monitors, nexthops_primary, monitoring, rx_monitor_timer, tx_monitor_timer, ipPrefix);
            setEndpointMonitor(vnet, monitors, nexthops_secondary, monitoring, rx_monitor_timer, tx_monitor_timer, ipPrefix);
        }
        else
        {
            if (it_route->second.primary != nexthops_primary)
            {
                setEndpointMonitor(vnet, monitors, nexthops_primary, monitoring, rx_monitor_timer, tx_monitor_timer, ipPrefix);
            }
            if (it_route->second.secondary != nexthops_secondary)
            {
                setEndpointMonitor(vnet, monitors, nexthops_secondary, monitoring, rx_monitor_timer, tx_monitor_timer, ipPrefix);
            }
            nexthops_selected = it_route->second.nhg_key;
            return true;
        }

        NextHopGroupKey nhg_custom = getActiveNHSet( vnet, nexthops_primary, ipPrefix);
        if (!hasNextHopGroup(vnet, nhg_custom))
        {
            if (!createNextHopGroup(vnet, nhg_custom, vrf_obj, monitoring, consistent_hashing_buckets, &ipPrefix))
            {
                SWSS_LOG_WARN("Failed to create Primary based custom next hop group. Cannot proceed.");
                delEndpointMonitor(vnet, nexthops_primary, ipPrefix);
                delEndpointMonitor(vnet, nexthops_secondary, ipPrefix);
                monitor_info_[vnet].erase(ipPrefix);

                return false;
            }
        }
        if (nhg_custom.getSize() > 0 )
        {
            SWSS_LOG_INFO(" Created Primary based custom next hop group.%s", nhg_custom.to_string().c_str() );
            nexthops_selected = nhg_custom;
            return true;
        }
        NextHopGroupKey nhg_custom_sec = getActiveNHSet( vnet, nexthops_secondary, ipPrefix);

        if (!hasNextHopGroup(vnet, nhg_custom_sec))
        {
            if (!createNextHopGroup(vnet, nhg_custom_sec, vrf_obj, monitoring, consistent_hashing_buckets, &ipPrefix))
            {
                SWSS_LOG_WARN("Failed to create secondary based custom next hop group. Cannot proceed.");
                delEndpointMonitor(vnet, nexthops_primary, ipPrefix);
                delEndpointMonitor(vnet, nexthops_secondary, ipPrefix);
                monitor_info_[vnet].erase(ipPrefix);

                return false;
            }
        }
        if (nhg_custom_sec.getSize() > 0 )
        {
            SWSS_LOG_INFO(" Created Secondary based custom next hop group.(%s).", nhg_custom_sec.to_string().c_str() );
            nexthops_selected = nhg_custom_sec;
            return true;
        }
        // nhg_custom is empty. we shall create a dummy enpty NHG for book keeping.
        if (!hasNextHopGroup(vnet, nhg_custom) && !hasNextHopGroup(vnet, nhg_custom_sec) )
        {
            NextHopGroupInfo next_hop_group_entry;
            next_hop_group_entry.next_hop_group_id = SAI_NULL_OBJECT_ID;
            next_hop_group_entry.ref_count = 0;
            syncd_nexthop_groups_[vnet][nhg_custom] = next_hop_group_entry;
        }
        nexthops_selected = nhg_custom;
        return true;
    }
    else if (!hasNextHopGroup(vnet, nexthops_primary))
    {
        SWSS_LOG_INFO("Creating next hop group  %s", nexthops_primary.to_string().c_str());
        setEndpointMonitor(vnet, monitors, nexthops_primary, monitoring, rx_monitor_timer, tx_monitor_timer, ipPrefix);
        if (!createNextHopGroup(vnet, nexthops_primary, vrf_obj, monitoring, consistent_hashing_buckets, &ipPrefix))
        {
            delEndpointMonitor(vnet, nexthops_primary, ipPrefix);
            return false;
        }
    }
    nexthops_selected = nexthops_primary;
    return true;
}

template<>
bool VNetRouteOrch::doRouteTask<VNetVrfObject>(const string& vnet, IpPrefix& ipPrefix,
                                               NextHopGroupKey& nexthops, string& op, string& profile,
                                               const string& monitoring,
                                               const int32_t rx_monitor_timer,
                                               const int32_t tx_monitor_timer,
                                               NextHopGroupKey& nexthops_secondary,
                                               const IpPrefix& adv_prefix,
                                               const uint16_t consistent_hashing_buckets,
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
        sai_object_id_t nh_id = SAI_NULL_OBJECT_ID;
        NextHopGroupKey active_nhg("", true);
        if (!selectNextHopGroup(vnet, nexthops, nexthops_secondary, monitoring, rx_monitor_timer, tx_monitor_timer, ipPrefix, vrf_obj, active_nhg, consistent_hashing_buckets, monitors))
        {
            return true;
        }

        // note: nh_id can be SAI_NULL_OBJECT_ID when active_nhg is empty.
        nh_id = syncd_nexthop_groups_[vnet][active_nhg].next_hop_group_id;

        auto it_route = syncd_tunnel_routes_[vnet].find(ipPrefix);
        for (auto vr_id : vr_set)
        {
            bool route_status = true;

            // Remove route if the nexthop group has no active endpoint
            if (syncd_nexthop_groups_[vnet][active_nhg].active_members.empty())
            {
                if (it_route != syncd_tunnel_routes_[vnet].end())
                {
                    NextHopGroupKey nhg = it_route->second.nhg_key;
                    // Remove route when updating from a nhg with active member to another nhg without
                    if (!syncd_nexthop_groups_[vnet][nhg].active_members.empty())
                    {
                        del_route(vr_id, pfx);
                    }
                }
            }
            else
            {
                auto prefixToRemove = ipPrefix;
                if (adv_prefix.to_string() != ipPrefix.to_string())
                {
                    prefixToRemove = adv_prefix;
                }
                auto prefixSubnet = prefixToRemove.getSubnet();
                if(gRouteOrch && gRouteOrch->isRouteExists(vr_id, prefixSubnet))
                {
                    if (!gRouteOrch->removeRoutePrefix(prefixSubnet))
                    {
                        SWSS_LOG_ERROR("Could not remove existing bgp route for prefix: %s\n", prefixSubnet.to_string().c_str());
                        return false;
                    }
                    SWSS_LOG_INFO("Successfully removed existing bgp route for prefix: %s\n",
                                  prefixSubnet.to_string().c_str());
                }
                if (it_route == syncd_tunnel_routes_[vnet].end())
                {
                    route_status = add_route(vr_id, pfx, nh_id);
                }
                else
                {
                    NextHopGroupKey nhg = it_route->second.nhg_key;
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
                if (active_nhg.getSize() > 1)
                {
                    removeNextHopGroup(vnet, active_nhg, vrf_obj);
                }
                return false;
            }
        }
        bool route_updated = false;
        bool priority_route_updated = false;
        if (it_route != syncd_tunnel_routes_[vnet].end() &&
            ((monitoring == "" && it_route->second.nhg_key != nexthops) ||
            ((monitoring == VNET_MONITORING_TYPE_CUSTOM || monitoring == VNET_MONITORING_TYPE_CUSTOM_BFD) && (it_route->second.primary != nexthops || it_route->second.secondary != nexthops_secondary))))
        {
            route_updated = true;
            NextHopGroupKey nhg = it_route->second.nhg_key;
            if (monitoring == VNET_MONITORING_TYPE_CUSTOM || monitoring == VNET_MONITORING_TYPE_CUSTOM_BFD)
            {
                // if the previously active NHG is same as the newly created active NHG.case of primary secondary swap or
                //when primary is active and secondary is changed or vice versa. In these cases we dont remove the NHG
                // but only remove the monitors for the set which has changed.
                if (it_route->second.primary != nexthops)
                {
                    delEndpointMonitor(vnet, it_route->second.primary, ipPrefix);
                }
                if (it_route->second.secondary != nexthops_secondary)
                {
                    delEndpointMonitor(vnet, it_route->second.secondary, ipPrefix);
                }
                if (monitor_info_[vnet][ipPrefix].empty())
                {
                    monitor_info_[vnet].erase(ipPrefix);
                }
                priority_route_updated = true;
            }
            else
            {
                // In case of updating an existing route, decrease the reference count for the previous nexthop group
                if (--syncd_nexthop_groups_[vnet][nhg].ref_count == 0)
                {
                    if (nhg.getSize() > 1)
                    {
                        removeNextHopGroup(vnet, nhg, vrf_obj);
                    }
                    else
                    {
                        syncd_nexthop_groups_[vnet].erase(nhg);
                        if(nhg.getSize() == 1)
                        {
                            NextHopKey nexthop = *nhg.getNextHops().begin();
                            if (!isLocalEndpoint(vnet, nexthop.ip_address))
                            {
                                vrf_obj->removeTunnelNextHop(nexthop);
                            }
                        }
                    }
                    if (monitoring != VNET_MONITORING_TYPE_CUSTOM && monitoring != VNET_MONITORING_TYPE_CUSTOM_BFD)
                    {
                        delEndpointMonitor(vnet, nhg, ipPrefix);
                    }
                }
                else
                {
                    syncd_nexthop_groups_[vnet][nhg].tunnel_routes.erase(ipPrefix);
                }
                vrf_obj->removeRoute(ipPrefix);
                vrf_obj->removeProfile(ipPrefix);
            }
        }
        if (!profile.empty())
        {
            vrf_obj->addProfile(ipPrefix, profile);
        }
        if (it_route == syncd_tunnel_routes_[vnet].end() || route_updated)
        {
            syncd_nexthop_groups_[vnet][active_nhg].tunnel_routes.insert(ipPrefix);
            VNetTunnelRouteEntry tunnel_route_entry;
            tunnel_route_entry.nhg_key = active_nhg;
            tunnel_route_entry.primary = nexthops;
            tunnel_route_entry.secondary = nexthops_secondary;
            syncd_tunnel_routes_[vnet][ipPrefix] = tunnel_route_entry;
            syncd_nexthop_groups_[vnet][active_nhg].ref_count++;

            if (priority_route_updated)
            {
                MonitorUpdate update;
                update.prefix = ipPrefix;
                update.state = MONITOR_SESSION_STATE_UNKNOWN;
                update.vnet = vnet;
                updateVnetTunnelCustomMonitor(update);
                return true;
            }

            if (adv_prefix.to_string() != ipPrefix.to_string() && prefix_to_adv_prefix_.find(ipPrefix) == prefix_to_adv_prefix_.end())
            {
                prefix_to_adv_prefix_[ipPrefix] = adv_prefix;
                if (adv_prefix_refcount_.find(adv_prefix) == adv_prefix_refcount_.end())
                {
                    adv_prefix_refcount_[adv_prefix] = 0;
                }
                if(active_nhg.getSize() > 0)
                {
                    adv_prefix_refcount_[adv_prefix] += 1;
                }
            }
            vrf_obj->addRoute(ipPrefix, active_nhg);
        }
        postRouteState(vnet, ipPrefix, active_nhg, profile);
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
        NextHopGroupKey nhg = it_route->second.nhg_key;
        auto last_nhg_size = nhg.getSize();
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
                SWSS_LOG_INFO("Successfully deleted the route for prefix: %s", ipPrefix.to_string().c_str());

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
                // We need to check specifically if there is only one next hop active.
                // In case of Priority routes we can end up in a situation where the active NHG has 0 nexthops.
                if(nhg.getSize() == 1)
                {
                    NextHopKey nexthop = *nhg.getNextHops().begin();
                    if (!isLocalEndpoint(vnet, nexthop.ip_address))
                    {
                        vrf_obj->removeTunnelNextHop(nexthop);
                    }
                }
            }
            if (monitor_info_[vnet].find(ipPrefix) == monitor_info_[vnet].end())
            {
                delEndpointMonitor(vnet, nhg, ipPrefix);
            }
        }
        else
        {
            syncd_nexthop_groups_[vnet][nhg].tunnel_routes.erase(ipPrefix);
        }
        if (monitor_info_[vnet].find(ipPrefix) != monitor_info_[vnet].end())
        {
            delEndpointMonitor(vnet, it_route->second.primary, ipPrefix);
            delEndpointMonitor(vnet, it_route->second.secondary, ipPrefix);
            monitor_info_[vnet].erase(ipPrefix);
        }

        syncd_tunnel_routes_[vnet].erase(ipPrefix);
        if (syncd_tunnel_routes_[vnet].empty())
        {
            syncd_tunnel_routes_.erase(vnet);
        }

        vrf_obj->removeRoute(ipPrefix);
        vrf_obj->removeProfile(ipPrefix);

        removeRouteState(vnet, ipPrefix);
        if (prefix_to_adv_prefix_.find(ipPrefix) != prefix_to_adv_prefix_.end())
        {
            auto adv_pfx = prefix_to_adv_prefix_[ipPrefix];
            prefix_to_adv_prefix_.erase(ipPrefix);

            if (last_nhg_size > 0)
            {
                adv_prefix_refcount_[adv_pfx] -= 1;
                if (adv_prefix_refcount_[adv_pfx] == 0)
                {
                    adv_prefix_refcount_.erase(adv_pfx);
                }
            }
        }
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
        NextHopGroupKey nhg = it_route->second.nhg_key;

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

inline void VNetRouteOrch::createSubnetDecapTerm(const IpPrefix &ipPrefix)
{
    const SubnetDecapConfig &config = gTunneldecapOrch->getSubnetDecapConfig();
    if (!config.enable || subnet_decap_terms_created_.find(ipPrefix) != subnet_decap_terms_created_.end())
    {
        return;
    }
    SWSS_LOG_NOTICE("Add subnet decap term for %s", ipPrefix.to_string().c_str());
    static const vector<FieldValueTuple> data = {
        {"term_type", "MP2MP"},
        {"subnet_type", "vip"}
    };
    string tunnel_name = ipPrefix.isV4() ? config.tunnel : config.tunnel_v6;
    string key = tunnel_name + ":" + ipPrefix.to_string();
    app_tunnel_decap_term_producer_.set(key, data);
    subnet_decap_terms_created_.insert(ipPrefix);
}

inline void VNetRouteOrch::removeSubnetDecapTerm(const IpPrefix &ipPrefix)
{
    const SubnetDecapConfig &config = gTunneldecapOrch->getSubnetDecapConfig();
    auto it = subnet_decap_terms_created_.find(ipPrefix);
    if (it == subnet_decap_terms_created_.end())
    {
        return;
    }
    SWSS_LOG_NOTICE("Remove subnet decap term for %s", ipPrefix.to_string().c_str());
    string tunnel_name = ipPrefix.isV4() ? config.tunnel : config.tunnel_v6;
    string key = tunnel_name + ":" + ipPrefix.to_string();
    app_tunnel_decap_term_producer_.del(key);
    subnet_decap_terms_created_.erase(it);
}

bool VNetRouteOrch::setAndDeleteRoutesWithRouteOrch(const sai_object_id_t vr_id, const IpPrefix& ipPrefix,
                                                    const NextHopGroupKey& nhg, const string& op)
{
    auto& bulkNhgReducedRefCnt = gRouteOrch->getBulkNhgReducedRefCnt();

    // Get vnet name from vrf id
    std::string vnet_name;
    if (!vnet_orch_->getVnetNameByVrfId(vr_id, vnet_name))
    {
        SWSS_LOG_INFO("Failed to get VNET name for vrf id '0x%" PRIx64, vr_id);
        return false;
    }

    // Set up route bulk context
    string key = vnet_name + ":" + ipPrefix.to_string();
    RouteBulkContext ctx(key, (op == SET_COMMAND));
    ctx.vrf_id = vr_id;
    ctx.ip_prefix = ipPrefix;
    ctx.nhg = nhg;

    if (op == SET_COMMAND)
    {
        // Add route via route orch
        if (gRouteOrch->addRoute(ctx, nhg))
        {
            return true;
        }
        
        // Flush the route bulker, so routes will be written to syncd and ASIC
        gRouteOrch->flushRouteBulker();
        bulkNhgReducedRefCnt.clear();

        // Post add route via route orch
        if (gRouteOrch->addRoutePost(ctx, nhg))
        {
            SWSS_LOG_NOTICE("Route %s added via routeorch for vnet %s", ipPrefix.to_string().c_str(), vnet_name.c_str());
        }
        else
        {
            SWSS_LOG_ERROR("Route %s add failed in routeorch for vnet %s", ipPrefix.to_string().c_str(), vnet_name.c_str());
            return false;
        }
    }
    else if (op == DEL_COMMAND)
    {
        // Remove route via route orch
        if (gRouteOrch->removeRoute(ctx))
        {
            return true;
        }

        // Flush the route bulker, so routes will be written to syncd and ASIC
        gRouteOrch->flushRouteBulker();
        bulkNhgReducedRefCnt.clear();

        // Post remove route via route orch
        if (gRouteOrch->removeRoutePost(ctx))
        {
            SWSS_LOG_NOTICE("Route %s removed via routeorch for vnet %s", ipPrefix.to_string().c_str(), vnet_name.c_str());
        }
        else
        {
            SWSS_LOG_ERROR("Route %s remove failed in routeorch for vnet %s", ipPrefix.to_string().c_str(), vnet_name.c_str());
            return false;
        }
    }

    // Remove next hop groups with 0 ref count
    for (auto& it : bulkNhgReducedRefCnt)
    {
        if (gRouteOrch->getNextHopGroupRefCount(it.first) == 0)
        {
            gRouteOrch->removeNextHopGroup(it.first);
            SWSS_LOG_INFO("Next hop group %s has 0 references, removed via routeorch", it.first.to_string().c_str());
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
    string nhg_str;

    if (is_subnet)
    {
        nh_id = port.m_rif_id;
    }
    else
    {
        // Populate next hop group string
        auto ifnames = tokenize(nh.ifname, ',');
        int idx = 0;
        for (auto it : nh.ips.getIpAddresses())
        {
            if (!nhg_str.empty())
            {
                nhg_str += ",";
            }

            nhg_str += it.to_string() + "@" + ifnames[idx];
            idx++;
        }
    }

    for (auto vr_id : vr_set)
    {
        if (vr_id == SAI_NULL_OBJECT_ID)
        {
            continue;
        }

        if (is_subnet)
        {
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
        else
        {
            NextHopGroupKey nhg(nhg_str);
            if (!setAndDeleteRoutesWithRouteOrch(vr_id, ipPrefix, nhg, op))
            {
                return false;
            }
        }
    }

    if (op == SET_COMMAND)
    {
        vrf_obj->addRoute(ipPrefix, nh, is_subnet);
    }
    else
    {
        vrf_obj->removeRoute(ipPrefix, is_subnet);
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

void VNetRouteOrch::createBfdSession(const string& vnet, const NextHopKey& endpoint, const IpAddress& monitor_addr, const int32_t rx_monitor_timer, const int32_t tx_monitor_timer)
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
        /*
            Even for local endpoints, we will use tunnel source IP as local_addr of BFD session.
        */
        IpAddress src_ip = tunnel_obj->getSrcIP();

        FieldValueTuple fvTuple("local_addr", src_ip.to_string());
        data.push_back(fvTuple);
        data.emplace_back("multihop", "true");
        // The BFD sessions established by the Vnet routes with monitoring need to be brought down
        // when the device goes into TSA.  The following parameter ensures that these session are
        // brought down while transitioning to TSA and brought back up when transitioning to TSB.
        data.emplace_back("shutdown_bfd_during_tsa", "true");

        if (rx_monitor_timer >= 0)
        {
            FieldValueTuple fv_rx("rx_interval", to_string(rx_monitor_timer));
            data.push_back(fv_rx);
        }

        if (tx_monitor_timer >= 0)
        {
            FieldValueTuple fv_tx("tx_interval", to_string(tx_monitor_timer));
            data.push_back(fv_tx);
        }

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
    SWSS_LOG_INFO("Removing nexthop info for endpoint: %s\n", endpoint_addr.to_string().c_str());
    nexthop_info_[vnet].erase(endpoint_addr);

    string key = "default:default:" + monitor_addr.to_string();
    bfd_session_producer_.del(key);
    bfd_sessions_.erase(monitor_addr);
}

void VNetRouteOrch::updateAllMonitoringSession(const string& vnet)
{
    SWSS_LOG_ENTER();
    vector<FieldValueTuple>  data;
    auto *vnet_obj = vnet_orch_->getTypePtr<VNetVrfObject>(vnet);
    auto overlay_dmac = vnet_obj->getOverlayDMac();
    SWSS_LOG_INFO ("updating overlay dmac value to %s", overlay_dmac.to_string().c_str());

    if (monitor_info_.find(vnet) != monitor_info_.end())
    {
        for (auto prefix : monitor_info_[vnet])
        {
            for (auto monitor_addr : monitor_info_[vnet][prefix.first])
            {

                string key = monitor_addr.first.to_string() + ":" + prefix.first.to_string();
                SWSS_LOG_INFO ("updating the overlay dmac of %s", key.c_str());

                FieldValueTuple fvTuple1("packet_type", "vxlan");
                data.push_back(fvTuple1);

                FieldValueTuple fvTuple3("overlay_dmac", overlay_dmac.to_string());
                data.push_back(fvTuple3);

                monitor_session_producer_->set(key, data);
            }
        }
    }
}

void VNetRouteOrch::createMonitoringSession(const string& vnet, const NextHopKey& endpoint, const IpAddress& monitor_addr, IpPrefix& ipPrefix)
{
    SWSS_LOG_ENTER();

    vector<FieldValueTuple>  data;
    auto *vnet_obj = vnet_orch_->getTypePtr<VNetVrfObject>(vnet);

    auto overlay_dmac = vnet_obj->getOverlayDMac();
    string key = monitor_addr.to_string() + ":" + ipPrefix.to_string();
    FieldValueTuple fvTuple1("packet_type", "vxlan");
    data.push_back(fvTuple1);

    FieldValueTuple fvTuple3("overlay_dmac", overlay_dmac.to_string());
    data.push_back(fvTuple3);

    monitor_session_producer_->set(key, data);

    MonitorSessionInfo info = monitor_info_[vnet][ipPrefix][monitor_addr];
    info.endpoint = endpoint;
    info.ref_count = 1;
    info.state = MONITOR_SESSION_STATE_DOWN;
    monitor_info_[vnet][ipPrefix][monitor_addr] = info;

}

void VNetRouteOrch::createCustomBFDMonitoringSession(const string& vnet, const NextHopKey& endpoint, const IpAddress& monitor_addr, IpPrefix& ipPrefix,  const int32_t rx_monitor_timer, const int32_t tx_monitor_timer)
{
    SWSS_LOG_ENTER();

    if (bfd_sessions_.find(monitor_addr) == bfd_sessions_.end())
    {
        vector<FieldValueTuple>    data;
        string key = "default:default:" + monitor_addr.to_string();

        auto tun_name = vnet_orch_->getTunnelName(vnet);
        VxlanTunnelOrch* vxlan_orch = gDirectory.get<VxlanTunnelOrch*>();
        auto tunnel_obj = vxlan_orch->getVxlanTunnel(tun_name);
        /*
            Even for local endpoints, we will use tunnel source IP as local_addr of BFD session.
        */
        IpAddress src_ip = tunnel_obj->getSrcIP();

        FieldValueTuple fvTuple("local_addr", src_ip.to_string());
        data.push_back(fvTuple);
        data.emplace_back("multihop", "true");
        // The BFD sessions established by the Vnet routes with monitoring need to be brought down
        // when the device goes into TSA.  The following parameter ensures that these session are
        // brought down while transitioning to TSA and brought back up when transitioning to TSB.
        data.emplace_back("shutdown_bfd_during_tsa", "true");

        if (rx_monitor_timer >= 0)
        {
            FieldValueTuple fv_rx("rx_interval", to_string(rx_monitor_timer));
            data.push_back(fv_rx);
        }

        if (tx_monitor_timer >= 0)
        {
            FieldValueTuple fv_tx("tx_interval", to_string(tx_monitor_timer));
            data.push_back(fv_tx);
        }

        bfd_session_producer_.set(key, data);
        bfd_sessions_[monitor_addr].bfd_state = SAI_BFD_SESSION_STATE_DOWN;
        bfd_sessions_[monitor_addr].vnet = vnet;
        bfd_sessions_[monitor_addr].endpoint = endpoint;
        bfd_sessions_[monitor_addr].custom_bfd = true;
    }

    MonitorSessionInfo info = monitor_info_[vnet][ipPrefix][monitor_addr];
    info.endpoint = endpoint;
    info.ref_count = 1;
    info.monitoring_type = VNET_MONITORING_TYPE_CUSTOM_BFD;
    info.custom_bfd_state = SAI_BFD_SESSION_STATE_DOWN;
    monitor_info_[vnet][ipPrefix][monitor_addr] = info;
}

void VNetRouteOrch::removeMonitoringSession(const string& vnet, const NextHopKey& endpoint, const IpAddress& monitor_addr, IpPrefix& ipPrefix)
{
    SWSS_LOG_ENTER();

    if (monitor_info_[vnet].find(ipPrefix) == monitor_info_[vnet].end() ||
        monitor_info_[vnet][ipPrefix].find(monitor_addr) == monitor_info_[vnet][ipPrefix].end())
    {
        SWSS_LOG_NOTICE("Monitor session for prefix %s endpoint %s does not exist", ipPrefix.to_string().c_str(), endpoint.to_string().c_str());
    }

    if (monitor_info_[vnet][ipPrefix][monitor_addr].monitoring_type == VNET_MONITORING_TYPE_CUSTOM_BFD)
    {
        string key = "default:default:" + monitor_addr.to_string();
        bfd_session_producer_.del(key);
        bfd_sessions_.erase(monitor_addr);
    }
    else
    {
        string key = monitor_addr.to_string() + ":" + ipPrefix.to_string();
        monitor_session_producer_->del(key);
    }

    monitor_info_[vnet][ipPrefix].erase(monitor_addr);
}

void VNetRouteOrch::setEndpointMonitor(const string& vnet, const map<NextHopKey, IpAddress>& monitors, NextHopGroupKey& nexthops, const string& monitoring, const int32_t rx_monitor_timer, const int32_t tx_monitor_timer, IpPrefix& ipPrefix)
{
    SWSS_LOG_ENTER();

    for (auto monitor : monitors)
    {
        NextHopKey nh = monitor.first;
        IpAddress monitor_ip = monitor.second;
        set<NextHopKey> next_hop_set = nexthops.getNextHops();
        if (next_hop_set.find(nh) != next_hop_set.end())
        {
            if (!monitoring.empty())
            {
                if (monitor_info_[vnet].find(ipPrefix) == monitor_info_[vnet].end() ||
                    monitor_info_[vnet][ipPrefix].find(monitor_ip) == monitor_info_[vnet][ipPrefix].end())
                {
                    if (monitoring == VNET_MONITORING_TYPE_CUSTOM)
                    {
                        createMonitoringSession(vnet, nh, monitor_ip, ipPrefix);
                    }
                    else if (monitoring == VNET_MONITORING_TYPE_CUSTOM_BFD)
                    {
                        /*
                        * Current BFD monitoring doesn't support the failover between primary and secondary NHG.
                        * To avoid the complexity/regression, we temporarily introduce custom_bfd monitoring type.
                        * It will be same behavior as custom monitoring type, except that it will create BFD session.
                        */
                        createCustomBFDMonitoringSession(vnet, nh, monitor_ip, ipPrefix, rx_monitor_timer, tx_monitor_timer);
                    }
                }
                else
                {
                    SWSS_LOG_INFO("Monitoring session for prefix %s endpoint %s, monitor %s already exists", ipPrefix.to_string().c_str(),
                        nh.to_string().c_str(), monitor_ip.to_string().c_str());
                    monitor_info_[vnet][ipPrefix][monitor_ip].ref_count += 1;
                }
            }
            else
            {
                if (nexthop_info_[vnet].find(nh.ip_address) == nexthop_info_[vnet].end())
                {
                    createBfdSession(vnet, nh, monitor_ip, rx_monitor_timer, tx_monitor_timer);
                }
                nexthop_info_[vnet][nh.ip_address].ref_count++;
            }
        }
        else
        {
            SWSS_LOG_NOTICE("Next hop %s not in nexthop group for prefix %s, skipping monitoring",
                nh.to_string().c_str(), ipPrefix.to_string().c_str());
        }
    }
}

void VNetRouteOrch::delEndpointMonitor(const string& vnet, NextHopGroupKey& nexthops, IpPrefix& ipPrefix)
{
    SWSS_LOG_ENTER();

    std::set<NextHopKey> nhks = nexthops.getNextHops();
    bool is_custom_monitoring = false;
    if (monitor_info_[vnet].find(ipPrefix) != monitor_info_[vnet].end())
    {
        is_custom_monitoring = true;
    }
    for (auto nhk: nhks)
    {
        IpAddress ip = nhk.ip_address;
        if (is_custom_monitoring)
        {
            for ( auto monitor : monitor_info_[vnet][ipPrefix])
            {
                if (monitor.second.endpoint == nhk)
                {
                    if (--monitor_info_[vnet][ipPrefix][monitor.first].ref_count == 0)
                    {
                        removeMonitoringSession(vnet, nhk, monitor.first, ipPrefix);
                        break;
                    }
                }
            }
        }
        else
        {
            if (nexthop_info_[vnet].find(ip) != nexthop_info_[vnet].end()) {
                if (--nexthop_info_[vnet][ip].ref_count == 0)
                {
                    IpAddress monitor_addr = nexthop_info_[vnet][ip].monitor_addr;
                    removeBfdSession(vnet, nhk, monitor_addr);
                }
            }
        }
    }
}

void VNetRouteOrch::updateCustomBfdState(const IpAddress& monitoring_ip, const string& state)
{
    SWSS_LOG_ENTER();

    auto it_peer = bfd_sessions_.find(monitoring_ip);
    if (it_peer == bfd_sessions_.end())
    {
        SWSS_LOG_WARN("BFD session for %s not found", monitoring_ip.to_string().c_str());
        return;
    }

    sai_bfd_session_state_t sai_state;
    if (state == "Up")
    {
        sai_state = SAI_BFD_SESSION_STATE_UP;
    }
    else if (state == "Down")
    {
        sai_state = SAI_BFD_SESSION_STATE_DOWN;
    }
    else
    {
        SWSS_LOG_WARN("Unknown BFD state: %s", state.c_str());
        return;
    }

    BfdSessionInfo& bfd_info = it_peer->second;
    bfd_info.bfd_state = sai_state;
    string vnet = bfd_info.vnet;
    NextHopKey endpoint = bfd_info.endpoint;

    if (monitor_info_.find(vnet) == monitor_info_.end())
    {
        SWSS_LOG_WARN("No custom monitoring session info for vnet %s", vnet.c_str());
        return;
    }

    for (auto iter : monitor_info_[vnet])
    {
        auto prefix = iter.first;
        if (monitor_info_[vnet][prefix].find(monitoring_ip) != monitor_info_[vnet][prefix].end() &&
            monitor_info_[vnet][prefix][monitoring_ip].endpoint == endpoint)
        {
            if ((sai_state == SAI_BFD_SESSION_STATE_UP || sai_state == SAI_BFD_SESSION_STATE_DOWN) &&
                monitor_info_[vnet][prefix][monitoring_ip].custom_bfd_state != sai_state)
            {
                SWSS_LOG_NOTICE("Custom BFD Monitor session state for %s:%s, endpoint:%s, monitoring_ip:%s changed to %s",
                    vnet.c_str(),
                    prefix.to_string().c_str(),
                    endpoint.ip_address.to_string().c_str(),
                    monitoring_ip.to_string().c_str(),
                    state.c_str());

                struct MonitorUpdate status_update;
                status_update.monitoring_type = VNET_MONITORING_TYPE_CUSTOM_BFD;
                status_update.custom_bfd_state = sai_state;
                status_update.prefix = prefix;
                status_update.monitor = monitoring_ip;
                status_update.vnet = vnet;
                updateVnetTunnelCustomMonitor(status_update);
            }
        }
    }
}

void VNetRouteOrch::updateMonitorState(string& op, const IpPrefix& prefix, const IpAddress& monitor, string state)
{
    SWSS_LOG_ENTER();
    if( op == SET_COMMAND)
    {
        for (auto iter :  monitor_info_)
        {
            std::string vnet = iter.first;
            if (monitor_info_[vnet].find(prefix) != monitor_info_[vnet].end() &&
                monitor_info_[vnet][prefix].find(monitor) != monitor_info_[vnet][prefix].end())
            {
                if (state =="up")
                {
                    if (monitor_info_[vnet][prefix][monitor].state != MONITOR_SESSION_STATE_UP)
                    {
                        SWSS_LOG_NOTICE("Monitor session state for %s|%s (%s) changed from down to up", prefix.to_string().c_str(),
                            monitor.to_string().c_str(), monitor_info_[vnet][prefix][monitor].endpoint.ip_address.to_string().c_str());
                        struct MonitorUpdate status_update;
                        status_update.state = MONITOR_SESSION_STATE_UP;
                        status_update.prefix = prefix;
                        status_update.monitor = monitor;
                        status_update.vnet = vnet;
                        updateVnetTunnelCustomMonitor(status_update);
                    }
                }
                else if (state =="down")
                {
                    if (monitor_info_[vnet][prefix][monitor].state != MONITOR_SESSION_STATE_DOWN)
                    {
                        SWSS_LOG_NOTICE("Monitor session state for %s|%s (%s) changed from up to down", prefix.to_string().c_str(),
                            monitor.to_string().c_str(), monitor_info_[vnet][prefix][monitor].endpoint.ip_address.to_string().c_str());
                        struct MonitorUpdate status_update;
                        status_update.state = MONITOR_SESSION_STATE_DOWN;
                        status_update.prefix = prefix;
                        status_update.monitor = monitor;
                        status_update.vnet = vnet;
                        updateVnetTunnelCustomMonitor(status_update);
                    }
                }
            }
        }
    }
}

void VNetRouteOrch::postRouteState(const string& vnet, IpPrefix& ipPrefix, NextHopGroupKey& nexthops, string& profile)
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

    auto prefix_to_use = ipPrefix;
    if (prefix_to_adv_prefix_.find(ipPrefix) != prefix_to_adv_prefix_.end())
    {
        auto adv_pfx = prefix_to_adv_prefix_[ipPrefix];
        if (route_state == "active" and adv_prefix_refcount_[adv_pfx] == 1)
        {
            prefix_to_use = adv_pfx;
        }
        else
        {
            route_state = "";
        }
    }
    SWSS_LOG_NOTICE("Advertisement of prefix: %s with profile: %s, status: %s via prefix: %s\n",
                                ipPrefix.to_string().c_str(), profile.c_str(),
                                route_state.c_str(), prefix_to_use.to_string().c_str());
    if (vnet_orch_->getAdvertisePrefix(vnet))
    {
        if (route_state == "active")
        {
            addRouteAdvertisement(prefix_to_use, profile);
        }
        else if (route_state == "inactive")
        {
            removeRouteAdvertisement(prefix_to_use);
        }
    }
    if (route_state == "active")
    {
        createSubnetDecapTerm(prefix_to_use);
    }
    else if (route_state == "inactive")
    {
        removeSubnetDecapTerm(prefix_to_use);
    }
}

void VNetRouteOrch::removeRouteState(const string& vnet, IpPrefix& ipPrefix)
{
    const string state_db_key = vnet + state_db_key_delimiter + ipPrefix.to_string();
    state_vnet_rt_tunnel_table_->del(state_db_key);
    SWSS_LOG_NOTICE("Advertisement of prefix: %s stopped.\n", ipPrefix.to_string().c_str());

    if(prefix_to_adv_prefix_.find(ipPrefix) !=prefix_to_adv_prefix_.end())
    {
        auto adv_pfx = prefix_to_adv_prefix_[ipPrefix];
        if(adv_prefix_refcount_[adv_pfx] == 1)
        {
            removeRouteAdvertisement(adv_pfx);
            removeSubnetDecapTerm(adv_pfx);
        }
    }
    else
    {
        removeRouteAdvertisement(ipPrefix);
        removeSubnetDecapTerm(ipPrefix);
    }
}

void VNetRouteOrch::addRouteAdvertisement(IpPrefix& ipPrefix, string& profile)
{
    const string key = ipPrefix.to_string();
    vector<FieldValueTuple> fvs;
    if (profile.empty())
    {
        fvs.push_back(FieldValueTuple("", ""));
    }
    else
    {
        fvs.push_back(FieldValueTuple("profile", profile));
    }
    state_vnet_rt_adv_table_->set(key, fvs);
}

void VNetRouteOrch::removeRouteAdvertisement(IpPrefix& ipPrefix)
{
    const string key = ipPrefix.to_string();
    state_vnet_rt_adv_table_->del(key);
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
        SWSS_LOG_WARN("Failed to parse key %s, no vrf is given", key.c_str());
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

    if (bfd_info.custom_bfd)
    {
        SWSS_LOG_DEBUG("Skip single NHG BFD state update for custom BFD session %s", peer_address.to_string().c_str());
        return;
    }

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

        std::set<NextHopKey> next_hop_set = nexthops.getNextHops();
        uint32_t seq_id = 0;
        uint32_t nh_seq_id = 0;
        for (auto nh: next_hop_set)
        {
            seq_id++;
            if (nh == endpoint)
            {
                nh_seq_id = seq_id;
                break;
            }
        }

        if (!nh_seq_id)
        {
            continue;
        }
        // when we add the first nexthop to the route, we dont create a nexthop group, we call the updateTunnelRoute with NHG with one member.
        // when adding the 2nd, 3rd ... members we create each NH using this create_next_hop_group_member call but give it the reference of next_hop_group_id.
        // this way we dont have to update the route, the syncd does it by itself. we only call the updateTunnelRoute to add/remove when adding or removing the
        // route fully.

        bool failed = false;
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

                if (gSwitchOrch->checkOrderedEcmpEnable())
                {
                    nhgm_attr.id = SAI_NEXT_HOP_GROUP_MEMBER_ATTR_SEQUENCE_ID;
                    nhgm_attr.value.u32 = nh_seq_id;
                    nhgm_attrs.push_back(nhgm_attr);
                }

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
                        // remove the bgp learnt route first if any exists and then add the tunnel route.
                        auto ipPrefixsubnet = ip_pfx.getSubnet();
                        auto prefixStr = ip_pfx.to_string();
                        auto nhStr = nexthops.to_string();
                        if (prefix_to_adv_prefix_.find(ip_pfx) != prefix_to_adv_prefix_.end())
                        {
                            auto adv_prefix = prefix_to_adv_prefix_[ip_pfx];
                            if(adv_prefix.to_string() != prefixStr)
                            {
                                ipPrefixsubnet = adv_prefix.getSubnet();
                            }
                        }

                        sai_object_id_t vr_id = vrf_obj->getVRidIngress();
                        if(gRouteOrch && gRouteOrch->isRouteExists(vr_id, ipPrefixsubnet))
                        {
                            if (!gRouteOrch->removeRoutePrefix(ipPrefixsubnet))
                            {
                                SWSS_LOG_ERROR("Could not remove existing bgp route for prefix: %s\n", prefixStr.c_str());
                                return;
                            }
                            SWSS_LOG_INFO("Successfully removed existing bgp route for prefix: %s\n", prefixStr.c_str());
                        }
                        string op = SET_COMMAND;
                        SWSS_LOG_INFO("Adding Vnet route for prefix:%s with nexthop group: %s\n", prefixStr.c_str(), nhStr.c_str());

                        if (!updateTunnelRoute(vnet, ip_pfx, nexthops, op))
                        {
                            SWSS_LOG_NOTICE("Failed to create tunnel route in hardware for prefix: %s\n", prefixStr.c_str());
                            failed = true;
                        }
                        else
                        {
                            SWSS_LOG_INFO("Successfully created tunnel route in hardware for prefix: %s\n", prefixStr.c_str());
                        }
                    }
                }
                if (failed)
                {
                    // This is an unrecoverable error, Throw a LOG_ERROR and return
                    SWSS_LOG_ERROR("Inconsistent hardware State. Failed to create tunnel routes.\n");
                    return;
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

                if (!isLocalEndpoint(vnet, endpoint.ip_address))
                {
                    vrf_obj->removeTunnelNextHop(endpoint);
                    SWSS_LOG_INFO("Successfully removed nexthop: %s\n",endpoint.to_string().c_str() );
                }

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
                            SWSS_LOG_NOTICE("Removing Vnet route for prefix : %s due to no active nexthops.\n",ip_pfx.to_string().c_str());
                            string op = DEL_COMMAND;
                            updateTunnelRoute(vnet, ip_pfx, nexthops, op);
                        }
                    }
                }
            }
        }
        if (!failed)
        {
            // Post configured in State DB
            for (auto ip_pfx : syncd_nexthop_groups_[vnet][nexthops].tunnel_routes)
            {
                string profile = vrf_obj->getProfile(ip_pfx);
                postRouteState(vnet, ip_pfx, nexthops, profile);
            }
        }
    }
}

void VNetRouteOrch::updateVnetTunnelCustomMonitor(const MonitorUpdate& update)
{
    SWSS_LOG_ENTER();
// This function recieves updates from the MonitorOrch for the endpoints state.
// Based on the state of the endpoints for a particular route, this function attempts
// to construct the primary next hop group. if it fails to do so,it attempts to create
// the secondary next hop group. After that it applies the next hop group and deletes
// the old next hop group.
// This function is also called in the case when the route configuration is updated to
// apply the new next hop group. In this case, the caller sets the state to
// MONITOR_SESSION_STATE_UNKNOWN and config_update and updateRoute are set to true.
// This function should never recieve MONITOR_SESSION_STATE_UNKNOWN from MonitorOrch.

    auto monitoring_type = update.monitoring_type;
    auto custom_bfd_state = update.custom_bfd_state;
    auto prefix = update.prefix;
    auto state = update.state;
    auto monitor = update.monitor;
    auto vnet = update.vnet;
    bool updateRoute = false;
    bool config_update = false;

    if (monitoring_type == VNET_MONITORING_TYPE_CUSTOM)
    {
        if (state != MONITOR_SESSION_STATE_UNKNOWN)
        {
            monitor_info_[vnet][prefix][monitor].state = state;
        }
        else
        {
            // we are coming here as a result of route config update. We need to repost the route if applicable.
            updateRoute = true;
            config_update = true;
        }
    }

    if(monitoring_type == VNET_MONITORING_TYPE_CUSTOM_BFD)
    {
        monitor_info_[vnet][prefix][monitor].custom_bfd_state = custom_bfd_state;
    }

    auto route = syncd_tunnel_routes_[vnet].find(prefix);
    if (route == syncd_tunnel_routes_[vnet].end())
    {
        SWSS_LOG_ERROR("Unexpected! Monitor Update for absent route.");
        return;

    }
    auto *vrf_obj = vnet_orch_->getTypePtr<VNetVrfObject>(vnet);
    set<sai_object_id_t> vr_set;

    auto l_fn = [&] (const string& vnet) {
        auto *vnet_obj = vnet_orch_->getTypePtr<VNetVrfObject>(vnet);
        sai_object_id_t vr_id = vnet_obj->getVRidIngress();
        vr_set.insert(vr_id);
    };

    l_fn(vnet);

    auto primary = route->second.primary;
    auto secondary = route->second.secondary;
    auto active_nhg = route->second.nhg_key;
    NextHopGroupKey nhg_custom("", true);
    sai_ip_prefix_t pfx;
    copy(pfx, prefix);
    NextHopGroupKey nhg_custom_primary = getActiveNHSet( vnet, primary, prefix);
    NextHopGroupKey nhg_custom_secondary = getActiveNHSet( vnet, secondary, prefix);
    SWSS_LOG_INFO("Primary active(%s), Secondary active (%s), Current active(%s)\n", nhg_custom_primary.to_string().c_str(),
                  nhg_custom_secondary.to_string().c_str(), active_nhg.to_string().c_str());
    if (nhg_custom_primary.getSize() > 0)
    {
        if (nhg_custom_primary != active_nhg )
        {
            if (!hasNextHopGroup(vnet, nhg_custom_primary))
            {
                if (!createNextHopGroup(vnet, nhg_custom_primary, vrf_obj, monitoring_type))
                {
                    SWSS_LOG_WARN("Failed to create primary based custom next hop group. Cannot proceed.");
                    return;
                }
            }
            updateRoute = true;
        }
        if (updateRoute)
        {
            nhg_custom = nhg_custom_primary;
        }
    }
    else if (nhg_custom_secondary.getSize() > 0)
    {
        if (nhg_custom_secondary != active_nhg )
        {
            if (!hasNextHopGroup(vnet, nhg_custom_secondary))
            {
                if (!createNextHopGroup(vnet, nhg_custom_secondary, vrf_obj, monitoring_type))
                {
                    SWSS_LOG_WARN("Failed to create secondary based custom next hop group. Cannot proceed.");
                    return;
                }
            }
            updateRoute = true;
        }
        if (updateRoute)
        {
            nhg_custom = nhg_custom_secondary;
        }
    }
    else
    {
        //both HHG's are inactive, need to remove the route.
        updateRoute = true;
    }

    if (nhg_custom.getSize() == 0)
    {
        // nhg_custom is empty. we shall create a dummy empty NHG for book keeping.
        SWSS_LOG_INFO(" Neither Primary or Secondary endpoints are up.\n");
        if (!hasNextHopGroup(vnet, nhg_custom))
        {
            NextHopGroupInfo next_hop_group_entry;
            next_hop_group_entry.next_hop_group_id = SAI_NULL_OBJECT_ID;
            next_hop_group_entry.ref_count = 0;
            syncd_nexthop_groups_[vnet][nhg_custom] = next_hop_group_entry;
        }
    }
    auto active_nhg_size = active_nhg.getSize();
    if (updateRoute)
    {
        for (auto vr_id : vr_set)
        {
            if (nhg_custom.getSize() == 0)
            {
                if (active_nhg_size > 0)
                {
                    SWSS_LOG_INFO(" Removing the route for prefix: %s.",prefix.to_string().c_str());
                    // we need to remove the route
                    del_route(vr_id, pfx);
                }
            }
            else
            {
                bool route_status = true;
                // note: nh_id can be SAI_NULL_OBJECT_ID when active_nhg is empty.
                auto nh_id = syncd_nexthop_groups_[vnet][nhg_custom].next_hop_group_id;
                if (active_nhg_size > 0)
                {
                    // we need to replace the nhg in the route
                    SWSS_LOG_INFO("Replacing nexthop group for prefix: %s, nexthop group: %s\n",
                                    prefix.to_string().c_str(), nhg_custom.to_string().c_str());
                    route_status = update_route(vr_id, pfx, nh_id);
                }
                else
                {
                    // we need to readd the route.
                    SWSS_LOG_NOTICE("Adding Custom monitored Route with prefix: %s and nexthop group: %s\n",
                                    prefix.to_string().c_str(), nhg_custom.to_string().c_str());
                    auto prefixToUse = prefix;
                    if (prefix_to_adv_prefix_.find(prefix) != prefix_to_adv_prefix_.end())
                    {
                        auto adv_prefix = prefix_to_adv_prefix_[prefix];
                        if(adv_prefix.to_string() != prefix.to_string())
                        {
                            prefixToUse = adv_prefix;
                        }
                    }
                    auto prefixsubnet = prefixToUse.getSubnet();
                    if (gRouteOrch && gRouteOrch->isRouteExists(vr_id, prefixsubnet))
                    {
                        if (!gRouteOrch->removeRoutePrefix(prefixsubnet))
                        {
                            SWSS_LOG_ERROR("Could not remove existing bgp route for prefix: %s\n", prefix.to_string().c_str());
                        }
                        SWSS_LOG_INFO("Successfully removed existing bgp route for prefix: %s\n", prefix.to_string().c_str());
                    }
                    route_status = add_route(vr_id, pfx, nh_id);
                }
                if (!route_status)
                {
                    SWSS_LOG_ERROR("Route add/update failed for %s, vr_id '0x%" PRIx64, prefix.to_string().c_str(), vr_id);
                    /* Clean up the newly created next hop group entry */
                    if (nhg_custom.getSize() > 1)
                    {
                        removeNextHopGroup(vnet, nhg_custom, vrf_obj);
                    }
                    return;
                }
                vrf_obj->addRoute(prefix, nhg_custom);
            }
        }
        if (config_update && nhg_custom != active_nhg)
        {
            // This convoluted logic has very good reason behind it.
            // when a route configuration gets updated, if the new endpoints are same but primaries
            // are changed, we must increase the ref count of active group to save it from premature
            // deletion at this place. So, we increment the refcount of existing active_nhg in doRotueTask right
            // before we call this function. Once here we need to undo this increment of refCount for the active_nhg
            // which is no longer relevant.
            syncd_nexthop_groups_[vnet][active_nhg].ref_count--;
        }

        if(--syncd_nexthop_groups_[vnet][active_nhg].ref_count == 0)
        {
            if (active_nhg_size > 1)
            {
                removeNextHopGroup(vnet, active_nhg, vrf_obj);
            }
            else
            {
                syncd_nexthop_groups_[vnet].erase(active_nhg);
                if(active_nhg_size == 1)
                {
                    NextHopKey nexthop(active_nhg.to_string(), true);
                    if (!isLocalEndpoint(vnet, nexthop.ip_address))
                    {
                        vrf_obj->removeTunnelNextHop(nexthop);
                    }
                }
            }
        }
        else
        {
            SWSS_LOG_INFO("Prefix %s no longer references nexthop group: %s\n",prefix.to_string().c_str(), active_nhg.to_string().c_str());
            syncd_nexthop_groups_[vnet][active_nhg].tunnel_routes.erase(prefix);
        }
        SWSS_LOG_INFO("Prefix %s now references nexthop group: %s\n",prefix.to_string().c_str(), nhg_custom.to_string().c_str());
        syncd_nexthop_groups_[vnet][nhg_custom].tunnel_routes.insert(prefix);
        syncd_tunnel_routes_[vnet][prefix].nhg_key = nhg_custom;
        if (nhg_custom != active_nhg)
        {
            syncd_nexthop_groups_[vnet][nhg_custom].ref_count++;
        }
        if (nhg_custom.getSize() == 0 && active_nhg_size > 0)
        {
            vrf_obj->removeRoute(prefix);
            SWSS_LOG_NOTICE("Route prefix is no longer active: %s\n", prefix.to_string().c_str());
            removeRouteState(vnet, prefix);
            if (prefix_to_adv_prefix_.find(prefix) != prefix_to_adv_prefix_.end())
            {
                auto adv_pfx = prefix_to_adv_prefix_[prefix];
                adv_prefix_refcount_[adv_pfx] -=1;
                if (adv_prefix_refcount_[adv_pfx] == 0)
                {
                    adv_prefix_refcount_.erase(adv_pfx);
                }
            }
        }
        else if (nhg_custom.getSize() > 0 && active_nhg_size == 0)
        {
            if (prefix_to_adv_prefix_.find(prefix) != prefix_to_adv_prefix_.end())
            {
                auto adv_prefix = prefix_to_adv_prefix_[prefix];
                if (adv_prefix_refcount_.find(adv_prefix) == adv_prefix_refcount_.end())
                {
                    adv_prefix_refcount_[adv_prefix] = 0;
                }
                adv_prefix_refcount_[adv_prefix] += 1;
            }
            string profile = vrf_obj->getProfile(prefix);
            postRouteState(vnet, prefix, nhg_custom, profile);
        }
        else
        {
            string profile = vrf_obj->getProfile(prefix);
            postRouteState(vnet, prefix, nhg_custom, profile);
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
    string profile = "";
    vector<IpAddress> primary_list;
    vector<IpAddress> secondary_list;
    string monitoring;
    int32_t rx_monitor_timer = -1;
    int32_t tx_monitor_timer = -1;
    int16_t consistent_hashing_buckets = -1;
    swss::IpPrefix adv_prefix;
    bool has_priority_ep = false;
    bool has_adv_pfx = false;
    bool check_directly_connected = false;
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
        else if (name == "profile")
        {
            profile = request.getAttrString(name);
        }
        else if (name == "primary")
        {
            primary_list = request.getAttrIPList(name);
        }
        else if (name == "monitoring")
        {
            monitoring = request.getAttrString(name);
        }
        else if (name == "adv_prefix")
        {
            adv_prefix = request.getAttrIpPrefix(name);
            has_adv_pfx = true;
        }
        else if (name == "check_directly_connected")
        {
            check_directly_connected = request.getAttrBool(name);
        }
        else if (name == "rx_monitor_timer")
        {
            rx_monitor_timer = static_cast<int32_t>(request.getAttrUint(name));
        }
        else if (name == "tx_monitor_timer")
        {
            tx_monitor_timer = static_cast<int32_t>(request.getAttrUint(name));
        }
        else if (name == "consistent_hashing_buckets")
        {
            consistent_hashing_buckets = static_cast<int16_t>(request.getAttrInt(name));
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
    if (!primary_list.empty() && monitor_list.empty())
    {
        SWSS_LOG_ERROR("Primary/backup behaviour cannot function without endpoint monitoring.");
        return true;
    }

    if (consistent_hashing_buckets != -1 && consistent_hashing_buckets < 1)
    {
        SWSS_LOG_ERROR("consistent_hashing_buckets must be greater than 0");
        return false;
    }

    const std::string& vnet_name = request.getKeyString(0);
    auto ip_pfx = request.getKeyIpPrefix(1);
    auto op = request.getOperation();

    vnet_tunnel_route_check_directly_connected[vnet_name] = check_directly_connected;

    SWSS_LOG_INFO("VNET-RT '%s' op '%s' for pfx %s", vnet_name.c_str(),
                   op.c_str(), ip_pfx.to_string().c_str());

    if (!primary_list.empty())
    {
        has_priority_ep = true;
        SWSS_LOG_INFO("Handling Priority Tunnel with prefix %s", ip_pfx.to_string().c_str());
    }

    if (primary_list.size() != ip_list.size())
    {
        for (auto& ip : primary_list)
        {
            if (std::find(ip_list.begin(), ip_list.end(), ip) == ip_list.end())
            {
                secondary_list.push_back(ip);
            }
        }
    }

    /*
    * A local endpoint is an endpoint that is directly connected, i.e., present in the neighbor table.
    * This check ensures that for primary/backup endpoint groups, all endpoints must be either local or all remote.
    * Partially local means that some endpoints are local and some are not. Mixing local and remote endpoints in a
    * single group is not supported.
    */
    if (check_directly_connected &&  (isPartiallyLocal(primary_list) || isPartiallyLocal(secondary_list) ))
    {
        SWSS_LOG_ERROR("Endpoints in Primary/backup should either all be local endpoints or no local endpoint at all.");
        return false;
    }

    NextHopGroupKey nhg_primary;
    NextHopGroupKey nhg_secondary;
    if (check_directly_connected)
    {
        nhg_primary = NextHopGroupKey("", primary_list.empty() || !isLocalEndpoint(vnet_name, primary_list[0]));
        nhg_secondary = NextHopGroupKey("", secondary_list.empty() || !isLocalEndpoint(vnet_name, secondary_list[0]));
    }
    else
    {
        nhg_primary = NextHopGroupKey("", true);
        nhg_secondary = NextHopGroupKey("", true);
    }
    NextHopGroupKey nhg("", true);
    map<NextHopKey, IpAddress> monitors;
    for (size_t idx_ip = 0; idx_ip < ip_list.size(); idx_ip++)
    {
        IpAddress ip = ip_list[idx_ip];
        bool is_local = isLocalEndpoint(vnet_name, ip);
        bool is_overlay = !is_local;
        string alias = is_local ? gIntfsOrch->getRouterIntfsAlias(ip) : "";
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

        if (is_local)
        {
            SWSS_LOG_INFO("Attempting to add TUNNEL TERM ACL for local endpoint %s", ip.to_string().c_str());
            vnet_tunnel_term_acl_->createAclRule(vnet_name, ip_pfx, ip);
        }

        NextHopKey nh(ip, alias, mac, vni, is_overlay);
        if (!monitor_list.empty())
        {
            monitors[nh] = monitor_list[idx_ip];
        }
        if (has_priority_ep)
        {
            if (std::find(primary_list.begin(), primary_list.end(), ip) != primary_list.end())
            {
                // only add the primary endpoint ips.
                nhg_primary.add(nh);
            }
            else
            {
                nhg_secondary.add(nh);
            }
        }
        nhg.add(nh);
    }
    if (!has_adv_pfx)
    {
        adv_prefix = ip_pfx;
    }
    if (vnet_orch_->isVnetExecVrf())
    {
        return doRouteTask<VNetVrfObject>(vnet_name, ip_pfx, (has_priority_ep == true) ? nhg_primary : nhg, op, profile, monitoring, rx_monitor_timer, tx_monitor_timer, nhg_secondary, adv_prefix, consistent_hashing_buckets, monitors);
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

bool VNetRouteOrch::isLocalEndpoint(const string&vnet, const IpAddress &ipAddr)
{
    auto it = vnet_tunnel_route_check_directly_connected.find(vnet);
    if (it == vnet_tunnel_route_check_directly_connected.end() || !it->second)
    {
        return false;
    }

    NeighborEntry n;
    MacAddress m;
    return gNeighOrch->getNeighborEntry(ipAddr, n, m);
}

bool VNetRouteOrch::isPartiallyLocal(const std::vector<swss::IpAddress>& ip_list)
{
    bool all_true = std::all_of(ip_list.begin(), ip_list.end(),
        [this](const swss::IpAddress& ip) {
            NeighborEntry n;
            MacAddress m;
            return gNeighOrch->getNeighborEntry(ip, n, m);
        });
    bool all_false = std::all_of(ip_list.begin(), ip_list.end(),
        [this](const swss::IpAddress& ip) {
            NeighborEntry n;
            MacAddress m;
            return !gNeighOrch->getNeighborEntry(ip, n, m);
        });

    return !(all_true || all_false);
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

MonitorOrch::MonitorOrch(DBConnector *db, string tableName):
    Orch2(db, tableName, request_)
{
    SWSS_LOG_ENTER();
}

MonitorOrch::~MonitorOrch(void)
{
    SWSS_LOG_ENTER();
}

bool MonitorOrch::addOperation(const Request& request)
{
    SWSS_LOG_ENTER();
    auto monitor = request.getKeyIpAddress(0);
    auto ip_Prefix = request.getKeyIpPrefix(1);

    auto session_state = request.getAttrString("state");
    SWSS_LOG_INFO("Added state table entry for monitor %s|%s", ip_Prefix.to_string().c_str(),monitor.to_string().c_str());

    string op = SET_COMMAND;
    VNetRouteOrch* vnet_route_orch = gDirectory.get<VNetRouteOrch*>();
    vnet_route_orch->updateMonitorState(op, ip_Prefix, monitor, session_state);

    return true;
}

bool MonitorOrch::delOperation(const Request& request)
{
    SWSS_LOG_ENTER();
    auto monitor = request.getKeyIpAddress(0);
    auto ip_Prefix = request.getKeyIpPrefix(1);

    SWSS_LOG_INFO("Deleting state table entry for monitor %s|%s", ip_Prefix.to_string().c_str(),monitor.to_string().c_str());
    VNetRouteOrch* vnet_route_orch = gDirectory.get<VNetRouteOrch*>();
    string op = DEL_COMMAND;
    vnet_route_orch->updateMonitorState(op, ip_Prefix, monitor, "" );

    return true;
}

BfdMonitorOrch::BfdMonitorOrch(DBConnector *db, std::string tableName):
    Orch2(db, tableName, request_)
{
    SWSS_LOG_ENTER();
}

BfdMonitorOrch::~BfdMonitorOrch(void)
{
    SWSS_LOG_ENTER();
}

bool BfdMonitorOrch::addOperation(const Request& request)
{
    SWSS_LOG_ENTER();

    auto monitor = request.getKeyIpAddress(2);
    auto session_state = request.getAttrString("state");

    VNetRouteOrch* vnet_route_orch = gDirectory.get<VNetRouteOrch*>();
    vnet_route_orch->updateCustomBfdState(monitor, session_state);

    return true;
}

bool BfdMonitorOrch::delOperation(const Request& request)
{
    return true;
}

VNetTunnelTermAcl::VNetTunnelTermAcl(DBConnector *cfgDb, DBConnector *appDb)
{
    SWSS_LOG_ENTER();

    acl_table_ = make_unique<ProducerStateTable>(appDb, APP_ACL_TABLE_TABLE_NAME);
    acl_table_type_ = make_unique<ProducerStateTable>(appDb, APP_ACL_TABLE_TYPE_TABLE_NAME);
    acl_rule_table_ = make_unique<ProducerStateTable>(appDb, APP_ACL_RULE_TABLE_NAME);

    ctx_ = make_shared<TunnelTermHelper>(cfgDb);
}

void VNetTunnelTermAcl::lazyInit()
{

    SWSS_LOG_ENTER();

    if (acl_table_initialized_)
    {
        return;
    }

    ctx_->initialize();

    vector<string> match_list = {
        MATCH_DST_IP,
        MATCH_DST_IPV6,
        MATCH_TUNNEL_TERM
    };
    string matches = std::accumulate(std::next(match_list.begin()), match_list.end(), match_list[0], concat);

    vector<string> action_list = {
        ACTION_REDIRECT_ACTION,
        ACTION_COUNTER
    };
    string actions = std::accumulate(std::next(action_list.begin()), action_list.end(), action_list[0], concat);

    vector<string> bpoint_list = {
        BIND_POINT_TYPE_PORT,
        BIND_POINT_TYPE_PORTCHANNEL
    };
    string bpoints = std::accumulate(std::next(bpoint_list.begin()), bpoint_list.end(), bpoint_list[0], concat);

    vector<FieldValueTuple> fvs = {
        {ACL_TABLE_TYPE_MATCHES, matches},
        {ACL_TABLE_TYPE_ACTIONS, actions},
        {ACL_TABLE_TYPE_BPOINT_TYPES, bpoints}
    };

    acl_table_type_->set(VNET_TUNNEL_TERM_ACL_TABLE_TYPE, fvs);

    std::string ports_str = "";
    auto ports = ctx_->getBindPoints();
    if (!ports.empty())
    {
        ports_str = std::accumulate(std::next(ports.begin()), ports.end(), ports[0], concat);
    }

    vector<FieldValueTuple> fvs2 = {
        {ACL_TABLE_DESCRIPTION, "Vnet Tunnel Termination ACL"},
        {ACL_TABLE_TYPE, VNET_TUNNEL_TERM_ACL_TABLE_TYPE},
        {ACL_TABLE_STAGE, STAGE_INGRESS},
        {ACL_TABLE_PORTS, ports_str}
    };

    acl_table_->set(VNET_TUNNEL_TERM_ACL_TABLE, fvs2);

    acl_table_initialized_ = true;
}

bool VNetTunnelTermAcl::createAclRule(const string vnet_name, swss::IpPrefix& vip, swss::IpAddress nh_ip)
{
    SWSS_LOG_ENTER();

    lazyInit();

    VNetLocEpAclRule rule;

    if (getAclRule(vnet_name, vip, rule))
    {
        /* If there are more than one local points for the same VIP, we will not create a new rule. */
        SWSS_LOG_NOTICE("ACL rule already exists for VNet %s with VIP %s", vnet_name.c_str(), vip.to_string().c_str());
        return true;
    }

    std::string rule_name = ctx_->getRuleName(vnet_name, vip);
    std::string alias = ctx_->getNbrAlias(nh_ip);

    if (alias.empty())
    {
        SWSS_LOG_ERROR("Failed to get interface alias for IP %s", nh_ip.to_string().c_str());
        return false;
    }

    vector<FieldValueTuple> fvs = {
        {RULE_PRIORITY, to_string(VNET_TUNNEL_TERM_ACL_BASE_PRIORITY)},
        {MATCH_DST_IP, vip.to_string()},
        {MATCH_TUNNEL_TERM, "true"},
        /* This tunnel term acl is to handle a transient state in DPU failover, so the redirect can't point to a VIP.*/
        {ACTION_REDIRECT_ACTION, nh_ip.to_string()}
    };

    SWSS_LOG_NOTICE("Creating ACL rule %s for VNet %s with VIP %s to redirect to %s",
                   rule_name.c_str(), vnet_name.c_str(), vip.to_string().c_str(), nh_ip.to_string().c_str());

    acl_rule_table_->set(rule_name, fvs);

    rule.rule_name = rule_name;
    rule.vip = vip;
    rule.nh_ip = nh_ip;

    vnet_loc_ep_acl_rule_map_[vnet_name].push_back(rule);

    return true;
}

bool VNetTunnelTermAcl::removeAclRule(const string vnet_name, swss::IpPrefix& vip)
{
    SWSS_LOG_ENTER();

    VNetLocEpAclRule rule;

    if (!getAclRule(vnet_name, vip, rule))
    {
        SWSS_LOG_ERROR("No ACL rule found for VNet %s with VIP %s", vnet_name.c_str(), vip.to_string().c_str());
        return false;
    }

    acl_rule_table_->del(rule.rule_name);

    vnet_loc_ep_acl_rule_map_[vnet_name].erase(
        std::remove_if(vnet_loc_ep_acl_rule_map_[vnet_name].begin(),
                       vnet_loc_ep_acl_rule_map_[vnet_name].end(),
                       [&vip](const VNetLocEpAclRule& rule) { return rule.vip == vip; }),
        vnet_loc_ep_acl_rule_map_[vnet_name].end());

    return true;
}

bool VNetTunnelTermAcl::getAclRule(const string vnet_name, const swss::IpPrefix& vip, VNetLocEpAclRule& rule_found)
{
    auto it = vnet_loc_ep_acl_rule_map_.find(vnet_name);
    if (it != vnet_loc_ep_acl_rule_map_.end())
    {
        for (const auto& rule : it->second)
        {
            if (rule.vip == vip)
            {
                rule_found = rule;
                return true;
            }
        }
    }

    return false;
}

