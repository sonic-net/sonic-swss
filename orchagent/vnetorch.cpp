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

#define VNET_BITMAP_SIZE 32

extern sai_virtual_router_api_t* sai_virtual_router_api;
extern sai_route_api_t* sai_route_api;
extern sai_neighbor_api_t* sai_neighbor_api;
extern sai_next_hop_api_t* sai_next_hop_api;
extern sai_bmtor_api_t* sai_bmtor_api;
extern sai_bridge_api_t* sai_bridge_api;
extern sai_router_interface_api_t* sai_router_intfs_api;
extern sai_fdb_api_t* sai_fdb_api;
extern sai_object_id_t gSwitchId;
extern Directory<Orch*> gDirectory;
extern PortsOrch *gPortsOrch;
extern IntfsOrch *gIntfsOrch;
extern sai_object_id_t gVirtualRouterId;

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
 * Bitmap based VNET class definition
 */
uint32_t VNetBitmapObject::vnetBitmap_ = 0;
set<uint32_t> VNetBitmapObject::vnetOffsets_;
set<uint32_t> VNetBitmapObject::tunnelOffsets_;
map<string, uint32_t> VNetBitmapObject::vnetIds_;
map<uint32_t, VnetBridgeInfo> VNetBitmapObject::bridgeInfoMap_;

VNetBitmapObject::VNetBitmapObject(const string& vnetName, VNetOrch *vnetOrch, const VNetInfo& vnetInfo) :
    VNetObject(vnetName, vnetOrch, vnetInfo)
{
    SWSS_LOG_ENTER();

    vnet_id_ = getFreeBitmapId(getName());
}

bool VNetBitmapObject::updateObj(const VNetInfo& vnetInfo)
{
    SWSS_LOG_ENTER();

    return false;
}

uint32_t VNetBitmapObject::getFreeBitmapId(const string& vnet)
{
    SWSS_LOG_ENTER();

    for (uint32_t i = 0; i < VNET_BITMAP_SIZE; i++)
    {
        uint32_t id = 1 << i;
        if ((id & vnetBitmap_) == 0)
        {
            vnetBitmap_ |= id;
            vnetIds_.emplace(vnet, id);
            return id;
        }
    }

    return 0;
}

uint32_t VNetBitmapObject::getBitmapId(const string& vnet)
{
    SWSS_LOG_ENTER();

    if (vnetIds_.find(vnet) == vnetIds_.end())
    {
        return 0;
    }

    return vnetIds_[vnet];
}

void VNetBitmapObject::recycleBitmapId(uint32_t id)
{
    SWSS_LOG_ENTER();

    vnetBitmap_ &= ~id;
}

uint32_t VNetBitmapObject::getFreeVnetTableOffset()
{
    SWSS_LOG_ENTER();

    for (uint32_t i = 0; i < 256; i++)
    {
        if (vnetOffsets_.count(i) == 0)
        {
            vnetOffsets_.insert(i);
            return i;
        }
    }

    return -1;
}

void VNetBitmapObject::recycleVnetTableOffset(uint32_t offset)
{
    SWSS_LOG_ENTER();

    vnetOffsets_.erase(offset);
}

uint32_t VNetBitmapObject::getFreeTunnelRouteTableOffset()
{
    SWSS_LOG_ENTER();

    for (uint32_t i = 0; i < 256; i++)
    {
        if (tunnelOffsets_.count(i) == 0)
        {
            tunnelOffsets_.insert(i);
            return i;
        }
    }

    return -1;
}

void VNetBitmapObject::recycleTunnelRouteTableOffset(uint32_t offset)
{
    SWSS_LOG_ENTER();

    tunnelOffsets_.erase(offset);
}

VnetBridgeInfo VNetBitmapObject::getBridgeInfoByVni(uint32_t vni, string tunnelName)
{
    SWSS_LOG_ENTER();

    if (bridgeInfoMap_.find(vni) != bridgeInfoMap_.end())
    {
        return std::move(bridgeInfoMap_.at(vni));
    }

    sai_status_t status;
    VnetBridgeInfo info;
    sai_attribute_t attr;
    vector<sai_attribute_t> bridge_attrs;
    attr.id = SAI_BRIDGE_ATTR_TYPE;
    attr.value.s32 = SAI_BRIDGE_TYPE_1D;
    bridge_attrs.push_back(attr);

    status = sai_bridge_api->create_bridge(
            &info.bridge_id,
            gSwitchId,
            (uint32_t)bridge_attrs.size(),
            bridge_attrs.data());
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to create bridge for vni %u", vni);
        throw std::runtime_error("vni creation failed");
    }

    vector<sai_attribute_t> rif_attrs;

    attr.id = SAI_ROUTER_INTERFACE_ATTR_VIRTUAL_ROUTER_ID;
    attr.value.oid = gVirtualRouterId;
    rif_attrs.push_back(attr);

    attr.id = SAI_ROUTER_INTERFACE_ATTR_SRC_MAC_ADDRESS;
    memcpy(attr.value.mac, gMacAddress.getMac(), sizeof(sai_mac_t));
    rif_attrs.push_back(attr);

    attr.id = SAI_ROUTER_INTERFACE_ATTR_TYPE;
    attr.value.s32 = SAI_ROUTER_INTERFACE_TYPE_BRIDGE;
    rif_attrs.push_back(attr);

    status = sai_router_intfs_api->create_router_interface(
            &info.rif_id,
            gSwitchId,
            (uint32_t)rif_attrs.size(),
            rif_attrs.data());
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to create rif for vni %u", vni);
        throw std::runtime_error("vni creation failed");
    }

    SWSS_LOG_NOTICE("Created RIF");

    vector<sai_attribute_t> bpr_attrs;

    attr.id = SAI_BRIDGE_PORT_ATTR_TYPE;
    attr.value.s32 = SAI_BRIDGE_PORT_TYPE_1D_ROUTER;
    bpr_attrs.push_back(attr);

    attr.id = SAI_BRIDGE_PORT_ATTR_RIF_ID;
    attr.value.oid = info.rif_id;
    bpr_attrs.push_back(attr);

    attr.id = SAI_BRIDGE_PORT_ATTR_BRIDGE_ID;
    attr.value.oid = info.bridge_id;
    bpr_attrs.push_back(attr);

    attr.id = SAI_BRIDGE_PORT_ATTR_FDB_LEARNING_MODE;
    attr.value.s32 = SAI_BRIDGE_PORT_FDB_LEARNING_MODE_DISABLE;
    bpr_attrs.push_back(attr);

    status = sai_bridge_api->create_bridge_port(
            &info.bridge_port_rif_id,
            gSwitchId,
            (uint32_t)bpr_attrs.size(),
            bpr_attrs.data());
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to create rif bridge port for vni %u", vni);
        throw std::runtime_error("vni creation failed");
    }

    vector<sai_attribute_t> bpt_attrs;
    auto* vxlan_orch = gDirectory.get<VxlanTunnelOrch*>();
    auto *tunnel = vxlan_orch->getVxlanTunnel(tunnelName);
    if (!tunnel->isActive())
    {
        tunnel->createTunnel(MAP_T::BRIDGE_TO_VNI, MAP_T::VNI_TO_BRIDGE);
    }

    attr.id = SAI_BRIDGE_PORT_ATTR_TYPE;
    attr.value.s32 = SAI_BRIDGE_PORT_TYPE_TUNNEL;
    bpt_attrs.push_back(attr);

    attr.id = SAI_BRIDGE_PORT_ATTR_BRIDGE_ID;
    attr.value.oid = info.bridge_id;
    bpt_attrs.push_back(attr);

    attr.id = SAI_BRIDGE_PORT_ATTR_ADMIN_STATE;
    attr.value.booldata = true;
    bpt_attrs.push_back(attr);

    attr.id = SAI_BRIDGE_PORT_ATTR_TUNNEL_ID;
    attr.value.oid = tunnel->getTunnelId();
    bpt_attrs.push_back(attr);

    attr.id = SAI_BRIDGE_PORT_ATTR_FDB_LEARNING_MODE;
    attr.value.s32 = SAI_BRIDGE_PORT_FDB_LEARNING_MODE_DISABLE;
    bpt_attrs.push_back(attr);

    status = sai_bridge_api->create_bridge_port(
            &info.bridge_port_tunnel_id,
            gSwitchId,
            (uint32_t)bpt_attrs.size(),
            bpt_attrs.data());
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to create tunnel bridge port for vni %u", vni);
        throw std::runtime_error("vni creation failed");
    }

    tunnel->addEncapMapperEntry(info.bridge_id, vni);

    bridgeInfoMap_.emplace(vni, info);

    return std::move(info);
}

bool VNetBitmapObject::addVlan(uint16_t vlan_id)
{
    SWSS_LOG_ENTER();

    SWSS_LOG_ERROR("marianp: %s", __PRETTY_FUNCTION__);
    Port p;
    if (!gPortsOrch->getPort("Ethernet0", p))
    {
        SWSS_LOG_ERROR("Failed to get port Ethernet0");
    }

    sai_attribute_t attr;
    vector<sai_attribute_t> attrs;
    sai_status_t status;
    sai_object_id_t vnetTableEntryId;

    attr.id = SAI_TABLE_VNET_ENTRY_ATTR_ACTION;
    attr.value.s32 = SAI_TABLE_VNET_ENTRY_ACTION_SET_VNET_BITMAP;
    attrs.push_back(attr);

    attr.id = SAI_TABLE_VNET_ENTRY_ATTR_PRIORITY;
    attr.value.u32 = getFreeVnetTableOffset();
    attrs.push_back(attr);

    attr.id = SAI_TABLE_VNET_ENTRY_ATTR_SRC_PORT_KEY;
    attr.value.oid = p.m_port_id;
    attrs.push_back(attr);

    attr.id = SAI_TABLE_VNET_ENTRY_ATTR_SRC_PORT_MASK;
    attr.value.booldata = 0;
    attrs.push_back(attr);

    attr.id = SAI_TABLE_VNET_ENTRY_ATTR_VLAN_ID_KEY;
    attr.value.u16 = vlan_id;
    attrs.push_back(attr);

    attr.id = SAI_TABLE_VNET_ENTRY_ATTR_VLAN_ID_MASK;
    attr.value.u16 = 0xfff;
    attrs.push_back(attr);

    attr.id = SAI_TABLE_VNET_ENTRY_ATTR_VNI_ID_KEY;
    attr.value.u32 = 0;
    attrs.push_back(attr);

    attr.id = SAI_TABLE_VNET_ENTRY_ATTR_VNI_ID_MASK;
    attr.value.u32 = 0;
    attrs.push_back(attr);

    attr.id = SAI_TABLE_VNET_ENTRY_ATTR_METADATA;
    attr.value.u32 = vnet_id_;
    attrs.push_back(attr);

    SWSS_LOG_ERROR("marianp: %s before %p", __PRETTY_FUNCTION__, sai_bmtor_api);
    status = sai_bmtor_api->create_table_vnet_entry(
            &vnetTableEntryId,
            gSwitchId,
            (uint32_t)attrs.size(),
            attrs.data());
    SWSS_LOG_ERROR("marianp: %s after", __PRETTY_FUNCTION__);

    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to create VNET table entry, SAI rc: %d", status);
        return false;
    }

    return true;
}

bool VNetBitmapObject::addIntf(Port& port, IpPrefix *prefix)
{
    SWSS_LOG_ENTER();

    sai_attribute_t attr;
    vector<sai_attribute_t> attrs;
    sai_status_t status;
    sai_object_id_t tunnelRouteTableEntryId;
    sai_ip_prefix_t saiPrefix;
    uint32_t peerBitmap = vnet_id_;

    if (!prefix || !prefix->isV4())
    {
        return false;
    }

    for (const auto& vnet : getPeerList())
    {
        uint32_t id = getBitmapId(vnet);
        if (id == 0)
        {
            SWSS_LOG_WARN("Peer vnet %s not ready", vnet.c_str());
            return false;
        }
        peerBitmap |= id;
    }


    saiPrefix.addr_family = SAI_IP_ADDR_FAMILY_IPV4;
    /* saiPrefix.mask.ip4 = prefix->getSubnet().getIp().getV4Addr(); */
    saiPrefix.mask.ip4 = 0xffffff;
    saiPrefix.addr.ip4 = prefix->getIp().getV4Addr();


    if (gIntfsOrch->getSyncdIntfses().find(port.m_alias) == gIntfsOrch->getSyncdIntfses().end())
    {
        if (!gIntfsOrch->setIntf(port, gVirtualRouterId, nullptr))
        {
            return false;
        }

        gIntfsOrch->addIp2MeRoute(gVirtualRouterId, *prefix);
    }

    attr.id = SAI_TABLE_TUNNEL_ROUTE_ENTRY_ATTR_ACTION;
    attr.value.s32 = SAI_TABLE_TUNNEL_ROUTE_ENTRY_ACTION_TO_LOCAL;
    attrs.push_back(attr);

    attr.id = SAI_TABLE_TUNNEL_ROUTE_ENTRY_ATTR_PRIORITY;
    attr.value.u32 = getFreeTunnelRouteTableOffset();
    attrs.push_back(attr);

    attr.id = SAI_TABLE_TUNNEL_ROUTE_ENTRY_ATTR_METADATA_KEY;
    attr.value.u32 = 0;
    attrs.push_back(attr);

    attr.id = SAI_TABLE_TUNNEL_ROUTE_ENTRY_ATTR_METADATA_MASK;
    attr.value.u32 = ~peerBitmap;
    attrs.push_back(attr);

    attr.id = SAI_TABLE_TUNNEL_ROUTE_ENTRY_ATTR_DST_IP_KEY;
    attr.value.ipprefix = saiPrefix;
    attrs.push_back(attr);

    attr.id = SAI_TABLE_TUNNEL_ROUTE_ENTRY_ATTR_ROUTER_INTERFACE;
    attr.value.oid = port.m_rif_id;
    attrs.push_back(attr);

    SWSS_LOG_ERROR("marianp: %s %p %p", __PRETTY_FUNCTION__, sai_bmtor_api, sai_bmtor_api->create_table_tunnel_route_entry);
    status = sai_bmtor_api->create_table_tunnel_route_entry(
            &tunnelRouteTableEntryId,
            gSwitchId,
            (uint32_t)attrs.size(),
            attrs.data());
    SWSS_LOG_ERROR("marianp: %s after", __PRETTY_FUNCTION__);

    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to create local VNET route entry, SAI rc: %d", status);
        return false;
    }

    if (!addVlan(port.m_vlan_info.vlan_id))
    {
        return false;
    }

    return true;
}

bool VNetBitmapObject::addTunnelRoute(IpPrefix& ipPrefix, tunnelEndpoint& endp)
{
    SWSS_LOG_ENTER();

    sai_status_t status;
    sai_attribute_t attr;
    sai_object_id_t tunnelRouteTableEntryId;
    auto& peer_list = getPeerList();
    auto bInfo = getBridgeInfoByVni(endp.vni, getTunnelName());
    uint32_t peerBitmap = vnet_id_;

    for (auto peer : peer_list)
    {
        if (!getVnetOrch()->isVnetExists(peer))
        {
            SWSS_LOG_INFO("Peer VNET %s not yet created", peer.c_str());
            return false;
        }
        peerBitmap |= getBitmapId(peer);
    }

    /* FDB entry to the tunnel */
    vector<sai_attribute_t> fdb_attrs;
    sai_ip_address_t underlayAddr;
    copy(underlayAddr, endp.ip);
    sai_fdb_entry_t fdbEntry;
    fdbEntry.switch_id = gSwitchId;
    endp.mac.getMac(fdbEntry.mac_address);
    fdbEntry.bv_id = bInfo.bridge_id;

    attr.id = SAI_FDB_ENTRY_ATTR_TYPE;
    attr.value.s32 = SAI_FDB_ENTRY_TYPE_STATIC;
    fdb_attrs.push_back(attr);

    attr.id = SAI_FDB_ENTRY_ATTR_BRIDGE_PORT_ID;
    attr.value.oid = bInfo.bridge_port_tunnel_id;
    fdb_attrs.push_back(attr);

    attr.id = SAI_FDB_ENTRY_ATTR_ENDPOINT_IP;
    attr.value.ipaddr = underlayAddr;
    fdb_attrs.push_back(attr);

    status = sai_fdb_api->create_fdb_entry(
            &fdbEntry,
            (uint32_t)fdb_attrs.size(),
            fdb_attrs.data());
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to create fdb entry for tunnel, SAI rc: %d", status);
        return false;
    }

    /* Fake neighbor */
    vector<sai_attribute_t> n_attrs;
    sai_neighbor_entry_t neigh;
    neigh.switch_id = gSwitchId;
    neigh.rif_id = bInfo.rif_id;
    neigh.ip_address.addr_family = SAI_IP_ADDR_FAMILY_IPV4;
    neigh.ip_address.addr.ip4 = 0x64646464;

    attr.id = SAI_NEIGHBOR_ENTRY_ATTR_DST_MAC_ADDRESS;
    endp.mac.getMac(attr.value.mac);
    n_attrs.push_back(attr);

    status = sai_neighbor_api->create_neighbor_entry(
            &neigh,
            (uint32_t)n_attrs.size(),
            n_attrs.data());
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to create neighbor entry for tunnel, SAI rc: %d", status);
        return false;
    }

    /* Nexthop */
    vector<sai_attribute_t> nh_attrs;
    sai_object_id_t nexthopId;

    attr.id = SAI_NEXT_HOP_ATTR_TYPE;
    attr.value.s32 = SAI_NEXT_HOP_TYPE_IP;
    nh_attrs.push_back(attr);

    attr.id = SAI_NEXT_HOP_ATTR_IP;
    attr.value.ipaddr = neigh.ip_address;
    nh_attrs.push_back(attr);

    attr.id = SAI_NEXT_HOP_ATTR_ROUTER_INTERFACE_ID;
    attr.value.oid = bInfo.rif_id;
    nh_attrs.push_back(attr);

    status = sai_next_hop_api->create_next_hop(
            &nexthopId,
            gSwitchId,
            (uint32_t)nh_attrs.size(),
            nh_attrs.data());
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to create nexthop for tunnel, SAI rc: %d", status);
        return false;
    }

    /* Tunnel route */
    vector<sai_attribute_t> tr_attrs;
    sai_ip_prefix_t pfx;
    copy(pfx, ipPrefix);

    attr.id = SAI_TABLE_TUNNEL_ROUTE_ENTRY_ATTR_ACTION;
    attr.value.s32 = SAI_TABLE_TUNNEL_ROUTE_ENTRY_ACTION_TO_TUNNEL;
    tr_attrs.push_back(attr);

    attr.id = SAI_TABLE_TUNNEL_ROUTE_ENTRY_ATTR_PRIORITY;
    attr.value.u32 = getFreeTunnelRouteTableOffset();
    tr_attrs.push_back(attr);

    attr.id = SAI_TABLE_TUNNEL_ROUTE_ENTRY_ATTR_METADATA_KEY;
    attr.value.u32 = 0;
    tr_attrs.push_back(attr);

    attr.id = SAI_TABLE_TUNNEL_ROUTE_ENTRY_ATTR_METADATA_MASK;
    attr.value.u32 = ~peerBitmap;
    tr_attrs.push_back(attr);

    attr.id = SAI_TABLE_TUNNEL_ROUTE_ENTRY_ATTR_DST_IP_KEY;
    attr.value.ipprefix = pfx;
    tr_attrs.push_back(attr);

    attr.id = SAI_TABLE_TUNNEL_ROUTE_ENTRY_ATTR_NEXT_HOP;
    attr.value.oid = nexthopId;
    tr_attrs.push_back(attr);

    SWSS_LOG_ERROR("marianp: %s %p %p", __PRETTY_FUNCTION__, sai_bmtor_api, sai_bmtor_api->create_table_tunnel_route_entry);
    status = sai_bmtor_api->create_table_tunnel_route_entry(
            &tunnelRouteTableEntryId,
            gSwitchId,
            (uint32_t)tr_attrs.size(),
            tr_attrs.data());
    SWSS_LOG_ERROR("marianp: %s after", __PRETTY_FUNCTION__);

    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to create local VNET route entry, SAI rc: %d", status);
        return false;
    }

    return true;
}
/*
 * VNet Orch class definitions
 */

VNetOrch::VNetOrch(DBConnector *db, const std::string& tableName)
         : Orch2(db, tableName, request_)
{
    SWSS_LOG_ENTER();
    SWSS_LOG_ERROR("marianp: %s", __PRETTY_FUNCTION__);
}

bool VNetOrch::addOperation(const Request& request)
{
    SWSS_LOG_ENTER();
    SWSS_LOG_ERROR("marianp: %s", __FUNCTION__);

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
    SWSS_LOG_ERROR("marianp: %s", __PRETTY_FUNCTION__);

    vr_cntxt = { VR_TYPE::ING_VR_VALID, VR_TYPE::EGR_VR_VALID };
}

std::unique_ptr<VNetObject> VNetVrfOrch::createObject(const string& vnet_name, const VNetInfo& vnetInfo)
{
    SWSS_LOG_ENTER();

    std::unique_ptr<VNetObject> vnet_obj(new VNetVrfObject(vnet_name, this, vnetInfo));
    return vnet_obj;
}

VNetBitmapOrch::VNetBitmapOrch(DBConnector *db, const std::string& tableName)
         : VNetOrch(db, tableName)
{
    SWSS_LOG_ENTER();
    SWSS_LOG_ERROR("marianp: %s", __PRETTY_FUNCTION__);
}

std::unique_ptr<VNetObject> VNetBitmapOrch::createObject(const string& vnet_name, const VNetInfo& vnetInfo)
{
    SWSS_LOG_ENTER();

    std::unique_ptr<VNetObject> vnet_obj(new VNetBitmapObject(vnet_name, this, vnetInfo));
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
