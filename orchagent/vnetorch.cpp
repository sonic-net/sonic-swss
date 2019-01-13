#include <cassert>
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <exception>

#include "sai.h"
#include "macaddress.h"
#include "orch.h"
#include "portsorch.h"
#include "request_parser.h"
#include "vnetorch.h"
#include "vxlanorch.h"
#include "intfsorch.h"
#include "directory.h"
#include "swssnet.h"

extern sai_virtual_router_api_t* sai_virtual_router_api;
extern sai_route_api_t* sai_route_api;
extern sai_neighbor_api_t* sai_neighbor_api;
extern sai_next_hop_api_t* sai_next_hop_api;
extern sai_object_id_t gSwitchId;
extern Directory<Orch*> gDirectory;
extern PortsOrch *gPortsOrch;
extern sai_object_id_t gVirtualRouterId;
extern IntfsOrch *gIntfsOrch;

/*
 * VRF Modeling and VNetVrf class definitions
 */
std::vector<VR_TYPE> vr_cntxt;

VNetVrfObject::VNetVrfObject(const string& vnetName, VNetOrch *vnetOrch, const VNetInfo& vnetInfo) :
    VNetObject(vnetName, vnetOrch, vnetInfo)
{
    SWSS_LOG_ENTER();

    createObj();
}

bool VNetVrfObject::addRoute(IpPrefix& ipPrefix, string& ifname)
{
    SWSS_LOG_ENTER();

    Port p;
    if (!gPortsOrch->getPort(ifname, p) || (p.m_rif_id == SAI_NULL_OBJECT_ID))
    {
        SWSS_LOG_WARN("Port/RIF %s doesn't exist", ifname.c_str());
        return false;
    }

    set<sai_object_id_t> vr_set;
    auto& peer_list = getPeerList();
    vr_set.insert(getVRidEgress());

    auto l_fn = [&] (const string& vnet) {
        auto *vnet_obj = dynamic_cast<VNetVrfObject*>(getVnetOrch()->getVnetPtr(vnet));
        sai_object_id_t vr_id = vnet_obj->getVRidIngress();
        vr_set.insert(vr_id);
    };

    for (auto peer : peer_list)
    {
        if (!getVnetOrch()->isVnetExists(peer))
        {
            SWSS_LOG_INFO("Peer VNET %s not yet created", peer.c_str());
            return false;
        }
        l_fn(peer);
    }

    sai_ip_prefix_t pfx;
    copy(pfx, ipPrefix);

    for (auto vr_id : vr_set)
    {
        if(!add_route(vr_id, pfx, p.m_rif_id))
        {
            SWSS_LOG_ERROR("Route add failed for %s", ipPrefix.to_string().c_str());
            break;
        }
    }

    return true;
}

sai_object_id_t VNetVrfObject::getNextHop(tunnelEndpoint& endp)
{
    SWSS_LOG_ENTER();

    if (nh_map_.find(endp.ip) != nh_map_.end())
    {
        return nh_map_.at(endp.ip);
    }

    sai_object_id_t nh_id = SAI_NULL_OBJECT_ID;
    auto tun_name = getTunnelName();

    VxlanTunnelOrch* vxlan_orch = gDirectory.get<VxlanTunnelOrch*>();

    nh_id = vxlan_orch->createNextHopTunnel(tun_name, endp.ip, endp.mac, endp.vni);
    if (nh_id == SAI_NULL_OBJECT_ID)
    {
        throw std::runtime_error("NH Tunnel create failed for " + getName() + " ip " + endp.ip.to_string());
    }

    nh_map_.insert({endp.ip, nh_id});
    return nh_id;
}

bool VNetVrfObject::addTunnelRoute(IpPrefix& ipPrefix, tunnelEndpoint& endp)
{
    SWSS_LOG_ENTER();

    set<sai_object_id_t> vr_set;
    vr_set.insert(getVRidIngress());
    auto& peer_list = getPeerList();

    auto l_fn = [&] (const string& vnet) {
        auto *vnet_obj = dynamic_cast<VNetVrfObject*>(getVnetOrch()->getVnetPtr(vnet));
        sai_object_id_t vr_id = vnet_obj->getVRidIngress();
        vr_set.insert(vr_id);
    };

    for (auto peer : peer_list)
    {
        if (!getVnetOrch()->isVnetExists(peer))
        {
            SWSS_LOG_INFO("Peer VNET %s not yet created", peer.c_str());
            return false;
        }
        l_fn(peer);
    }

    sai_ip_prefix_t pfx;
    copy(pfx, ipPrefix);
    sai_object_id_t nh_id = getNextHop(endp);

    for (auto vr_id : vr_set)
    {
        if(!add_route(vr_id, pfx, nh_id))
        {
            SWSS_LOG_ERROR("Route add failed for %s", ipPrefix.to_string().c_str());
            break;
        }
    }
    return true;
}

bool VNetVrfObject::addIntf(Port& port, IpPrefix *prefix)
{
    if (!gIntfsOrch->setIntf(port, getVRidIngress(), nullptr))
    {
        return false;
    }

    gIntfsOrch->addIp2MeRoute(gVirtualRouterId, *prefix);
    gIntfsOrch->addSubnetRoute(port, *prefix);

    return true;
}

sai_object_id_t VNetVrfObject::getVRidIngress() const
{
    SWSS_LOG_ENTER();

    if (vr_ids_.find(VR_TYPE::ING_VR_VALID) != vr_ids_.end())
    {
        return vr_ids_.at(VR_TYPE::ING_VR_VALID);
    }
    return SAI_NULL_OBJECT_ID;
}

sai_object_id_t VNetVrfObject::getVRidEgress() const
{
    SWSS_LOG_ENTER();

    if (vr_ids_.find(VR_TYPE::EGR_VR_VALID) != vr_ids_.end())
    {
        return vr_ids_.at(VR_TYPE::EGR_VR_VALID);
    }
    return SAI_NULL_OBJECT_ID;
}

set<sai_object_id_t> VNetVrfObject::getVRids() const
{
    SWSS_LOG_ENTER();

    set<sai_object_id_t> ids;

    for_each (vr_ids_.begin(), vr_ids_.end(), [&](std::pair<VR_TYPE, sai_object_id_t> element)
    {
        ids.insert(element.second);
    });

    return ids;
}

bool VNetVrfObject::createObj()
{
    SWSS_LOG_ENTER();

    sai_attribute_t attr;
    vector<sai_attribute_t> attrs;
    static const MacAddress emptyMac;

    if (getSrcMac() != emptyMac)
    {
        attr.id = SAI_VIRTUAL_ROUTER_ATTR_SRC_MAC_ADDRESS;
        getSrcMac().getMac(attr.value.mac);
        attrs.push_back(attr);
    }

    auto l_fn = [&] (sai_object_id_t& router_id) {

        sai_status_t status = sai_virtual_router_api->create_virtual_router(&router_id,
                                                                            gSwitchId,
                                                                            static_cast<uint32_t>(attrs.size()),
                                                                            attrs.data());
        if (status != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_ERROR("Failed to create virtual router name: %s, rv: %d",
                           getName().c_str(), status);
            throw std::runtime_error("Failed to create VR object");
        }
        return true;
    };

    /*
     * Create ingress and egress VRF based on VR_VALID
     */

    for (auto vr_type : vr_cntxt)
    {
        sai_object_id_t router_id;
        if (vr_type != VR_TYPE::VR_INVALID && l_fn(router_id))
        {
            SWSS_LOG_DEBUG("VNET vr_type %d router id %lx  ", vr_type, router_id);
            vr_ids_.insert(std::pair<VR_TYPE, sai_object_id_t>(vr_type, router_id));
        }
    }

    VxlanTunnelOrch* vxlan_orch = gDirectory.get<VxlanTunnelOrch*>();
    if (!vxlan_orch->createVxlanTunnelMap(
                getTunnelName(),
                TUNNEL_MAP_T_VIRTUAL_ROUTER,
                getVni(),
                getVRidIngress(),
                getVRidEgress()))
    {
        SWSS_LOG_ERROR("VNET '%s', tunnel '%s', map create failed",
                        getName().c_str(), getTunnelName().c_str());
    }

    SWSS_LOG_INFO("VNET '%s' router object created ", getName().c_str());
    return true;
}

bool VNetVrfObject::updateObj(const VNetInfo& vnetInfo)
{
    SWSS_LOG_ENTER();

    sai_attribute_t attr;
    vector<sai_attribute_t> attrs;
    static const MacAddress emptyMac;

    if (getSrcMac() != emptyMac)
    {
        attr.id = SAI_VIRTUAL_ROUTER_ATTR_SRC_MAC_ADDRESS;
        getSrcMac().getMac(attr.value.mac);
        attrs.push_back(attr);
    }

    set<sai_object_id_t> vr_ent = getVRids();

    for (const auto& a: attrs)
    {
        for (auto it : vr_ent)
        {
            sai_status_t status = sai_virtual_router_api->set_virtual_router_attribute(it, &a);
            if (status != SAI_STATUS_SUCCESS)
            {
                SWSS_LOG_ERROR("Failed to update virtual router attribute. VNET name: %s, rv: %d",
                                getName().c_str(), status);
                return false;
            }
        }
    }

    SWSS_LOG_INFO("VNET '%s' was updated", getName().c_str());
    return true;
}

bool VNetVrfObject::add_route(sai_object_id_t vr_id, sai_ip_prefix_t& ip_pfx, sai_object_id_t nh_id)
{
    SWSS_LOG_ENTER();

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

    return true;
}

VNetVrfObject::~VNetVrfObject()
{
    SWSS_LOG_ENTER();

    set<sai_object_id_t> vr_ent = getVRids();
    for (auto it : vr_ent)
    {
        sai_status_t status = sai_virtual_router_api->remove_virtual_router(it);
        if (status != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_ERROR("Failed to remove virtual router name: %s, rv:%d",
                            getName().c_str(), status);
        }
    }

    SWSS_LOG_INFO("VNET '%s' deleted ", getName().c_str());
}

/*
 * VNet Orch class definitions
 */

VNetOrch::VNetOrch(DBConnector *db, const std::string& tableName)
         : Orch2(db, tableName, request_)
{
    SWSS_LOG_ENTER();
}

bool VNetOrch::addOperation(const Request& request)
{
    SWSS_LOG_ENTER();

    VNetInfo vnetInfo;
    bool peer = false, create = false, isMac = false;

    for (const auto& name: request.getAttrFieldNames())
    {
        if (name == "src_mac")
        {
            vnetInfo.mac = request.getAttrMacAddress("src_mac");
            isMac = true;
        }
        else if (name == "peer_list")
        {
            vnetInfo.peers  = request.getAttrSet("peer_list");
            peer = true;
        }
        else if (name == "vni")
        {
            vnetInfo.vni  = static_cast<sai_uint32_t>(request.getAttrUint("vni"));
        }
        else if (name == "vxlan_tunnel")
        {
            vnetInfo.tunnel = request.getAttrString("vxlan_tunnel");
        }
        else
        {
            SWSS_LOG_WARN("Logic error: Unknown attribute: %s", name.c_str());
            continue;
        }
    }

    const std::string& vnet_name = request.getKeyString(0);
    SWSS_LOG_INFO("VNET '%s' add request", vnet_name.c_str());

    try
    {
        VNetObject_T obj;
        auto it = vnet_table_.find(vnet_name);
        VxlanTunnelOrch* vxlan_orch = gDirectory.get<VxlanTunnelOrch*>();

        if (!vxlan_orch->isTunnelExists(vnetInfo.tunnel))
        {
            SWSS_LOG_WARN("Vxlan tunnel '%s' doesn't exist", vnetInfo.tunnel.c_str());
            return false;
        }

        if (it == std::end(vnet_table_))
        {
            obj = createObject(vnet_name, vnetInfo);
            create = true;
            SWSS_LOG_INFO("VNET '%s' was added ", vnet_name.c_str());
        }

        if (create)
        {
            vnet_table_[vnet_name] = std::move(obj);
        }
        else if (peer)
        {
            it->second->setPeerList(vnetInfo.peers);
        }
        else if (isMac)
        {
            if(!it->second->updateObj(vnetInfo))
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

    vnet_table_.erase(vnet_name);

    SWSS_LOG_INFO("VNET '%s' del request", vnet_name.c_str());
    return true;
}

VNetVrfOrch::VNetVrfOrch(DBConnector *db, const std::string& tableName)
         : VNetOrch(db, tableName)
{
    SWSS_LOG_ENTER();

    vr_cntxt = { VR_TYPE::ING_VR_VALID, VR_TYPE::EGR_VR_VALID };
}

std::unique_ptr<VNetObject> VNetVrfOrch::createObject(const string& vnet_name, const VNetInfo& vnetInfo)
{
    SWSS_LOG_ENTER();

    std::unique_ptr<VNetObject> vnet_obj(new VNetVrfObject(vnet_name, this, vnetInfo));
    return vnet_obj;
}

/*
 * Vnet Route Handling
 */

VNetRouteOrch::VNetRouteOrch(DBConnector *db, vector<string> &tableNames, VNetOrch *vnetOrch)
                                  : Orch2(db, tableNames, request_), vnet_orch_(vnetOrch)
{
    SWSS_LOG_ENTER();

    handler_map_.insert(handler_pair(APP_VNET_RT_TABLE_NAME, &VNetRouteOrch::handleRoutes));
    handler_map_.insert(handler_pair(APP_VNET_RT_TUNNEL_TABLE_NAME, &VNetRouteOrch::handleTunnel));
}

bool VNetRouteOrch::doRouteTask(const string& vnet, IpPrefix& ipPrefix, tunnelEndpoint& endp)
{
    SWSS_LOG_ENTER();

    if (!vnet_orch_->isVnetExists(vnet))
    {
        SWSS_LOG_WARN("VNET %s doesn't exist", vnet.c_str());
        return false;
    }

    VNetObject *vnet_obj = vnet_orch_->getVnetPtr(vnet);

    return vnet_obj->addTunnelRoute(ipPrefix, endp);
}

bool VNetRouteOrch::doRouteTask(const string& vnet, IpPrefix& ipPrefix, string& ifname)
{
    SWSS_LOG_ENTER();

    if (!vnet_orch_->isVnetExists(vnet))
    {
        SWSS_LOG_WARN("VNET %s doesn't exist", vnet.c_str());
        return false;
    }

    VNetObject *vnet_obj = vnet_orch_->getVnetPtr(vnet);

    return vnet_obj->addRoute(ipPrefix, ifname);
}

void VNetRouteOrch::handleRoutes(const Request& request)
{
    SWSS_LOG_ENTER();

    string ifname = "";

    for (const auto& name: request.getAttrFieldNames())
    {
        if (name == "ifname")
        {
            ifname = request.getAttrString(name);
        }
        else
        {
            SWSS_LOG_WARN("Logic error: Unknown attribute: %s", name.c_str());
            return;
        }
    }

    const std::string& vnet_name = request.getKeyString(0);
    auto ip_pfx = request.getKeyIpPrefix(1);

    SWSS_LOG_INFO("VNET-RT '%s' add for ip %s", vnet_name.c_str(), ip_pfx.to_string().c_str());

    if (!doRouteTask(vnet_name, ip_pfx, ifname))
    {
        throw std::runtime_error("Route add failed");
    }
}

void VNetRouteOrch::handleTunnel(const Request& request)
{
    SWSS_LOG_ENTER();

    IpAddress ip;
    MacAddress mac;
    uint32_t vni = 0;

    for (const auto& name: request.getAttrFieldNames())
    {
        if (name == "endpoint")
        {
            ip = request.getAttrIP(name);
        }
        else if (name == "vni")
        {
            vni = static_cast<uint32_t>(request.getAttrUint(name));
        }
        else if (name == "mac_address")
        {
            mac = request.getAttrMacAddress(name);
        }
        else
        {
            SWSS_LOG_WARN("Logic error: Unknown attribute: %s", name.c_str());
            return;
        }
    }

    const std::string& vnet_name = request.getKeyString(0);
    auto ip_pfx = request.getKeyIpPrefix(1);

    SWSS_LOG_INFO("VNET-RT '%s' add for endpoint %s", vnet_name.c_str(), ip_pfx.to_string().c_str());

    tunnelEndpoint endp = { ip, mac, vni };
    if (!doRouteTask(vnet_name, ip_pfx, endp))
    {
        throw std::runtime_error("Route add failed");
    }
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

        (this->*(handler_map_[tn]))(request);
    }
    catch(std::runtime_error& _)
    {
        SWSS_LOG_ERROR("VNET add operation error %s ", _.what());
        return false;
    }

    return true;
}

bool VNetRouteOrch::delOperation(const Request& request)
{
    SWSS_LOG_ENTER();

    SWSS_LOG_ERROR("DEL operation is not implemented");

    return true;
}
