#include <cassert>
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <exception>

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

extern sai_virtual_router_api_t* sai_virtual_router_api;
extern sai_route_api_t* sai_route_api;
extern sai_bridge_api_t* sai_bridge_api;
extern sai_router_interface_api_t* sai_router_intfs_api;
extern sai_fdb_api_t* sai_fdb_api;
extern sai_neighbor_api_t* sai_neighbor_api;
extern sai_next_hop_api_t* sai_next_hop_api;
extern sai_bmtor_api_t* sai_bmtor_api;
extern sai_object_id_t gSwitchId;
extern Directory<Orch*> gDirectory;
extern PortsOrch *gPortsOrch;
extern IntfsOrch *gIntfsOrch;
extern NeighOrch *gNeighOrch;
extern CrmOrch *gCrmOrch;
extern MacAddress gVxlanMacAddress;

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
        sai_object_id_t router_id;
        if (vr_type != VR_TYPE::VR_INVALID && l_fn(router_id))
        {
            SWSS_LOG_DEBUG("VNET vr_type %d router id %lx  ", static_cast<int>(vr_type), router_id);
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

bool VNetVrfObject::addRoute(IpPrefix& ipPrefix, tunnelEndpoint& endp)
{
    if (hasRoute(ipPrefix))
    {
        SWSS_LOG_INFO("VNET route '%s' exists", ipPrefix.to_string().c_str());
        return false;
    }

    tunnels_[ipPrefix] = endp;
    return true;
}

bool VNetVrfObject::addRoute(IpPrefix& ipPrefix, nextHop& nh)
{
    if (hasRoute(ipPrefix))
    {
        SWSS_LOG_INFO("VNET route '%s' exists", ipPrefix.to_string().c_str());
        return false;
    }

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
        auto endp = tunnels_.at(ipPrefix);
        removeTunnelNextHop(endp);
        tunnels_.erase(ipPrefix);
    }
    else
    {
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

sai_object_id_t VNetVrfObject::getTunnelNextHop(tunnelEndpoint& endp)
{
    sai_object_id_t nh_id = SAI_NULL_OBJECT_ID;
    auto tun_name = getTunnelName();

    VxlanTunnelOrch* vxlan_orch = gDirectory.get<VxlanTunnelOrch*>();

    nh_id = vxlan_orch->createNextHopTunnel(tun_name, endp.ip, endp.mac, endp.vni);
    if (nh_id == SAI_NULL_OBJECT_ID)
    {
        throw std::runtime_error("NH Tunnel create failed for " + vnet_name_ + " ip " + endp.ip.to_string());
    }

    return nh_id;
}

bool VNetVrfObject::removeTunnelNextHop(tunnelEndpoint& endp)
{
    auto tun_name = getTunnelName();

    VxlanTunnelOrch* vxlan_orch = gDirectory.get<VxlanTunnelOrch*>();

    if (!vxlan_orch->removeNextHopTunnel(tun_name, endp.ip, endp.mac, endp.vni))
    {
        SWSS_LOG_ERROR("VNET %s Tunnel NextHop remove failed for '%s'",
                        vnet_name_.c_str(), endp.ip.to_string().c_str());
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
 * Bitmap based VNET class definition
 */
std::bitset<VNET_BITMAP_SIZE> VNetBitmapObject::vnetBitmap_;
std::bitset<VNET_TUNNEL_SIZE> VNetBitmapObject::tunnelOffsets_;
std::bitset<VNET_TUNNEL_SIZE> VNetBitmapObject::tunnelIdOffsets_;
map<string, uint32_t> VNetBitmapObject::vnetIds_;
map<uint32_t, VnetBridgeInfo> VNetBitmapObject::bridgeInfoMap_;
map<tuple<MacAddress, sai_object_id_t>, VnetNeighInfo> VNetBitmapObject::neighInfoMap_;
map<tuple<IpAddress, sai_object_id_t>, TunnelEndpointInfo> VNetBitmapObject::endpointMap_;

VNetBitmapObject::VNetBitmapObject(const std::string& vnet, const VNetInfo& vnetInfo,
                             vector<sai_attribute_t>& attrs) : VNetObject(vnetInfo)
{
    SWSS_LOG_ENTER();

    vnet_id_ = getFreeBitmapId(vnet);
    vnet_name_ = vnet;

    setVniInfo(vnetInfo.vni);
}

bool VNetBitmapObject::updateObj(vector<sai_attribute_t>&)
{
    SWSS_LOG_ENTER();

    return false;
}

uint32_t VNetBitmapObject::getFreeBitmapId(const string& vnet)
{
    SWSS_LOG_ENTER();

    for (uint32_t i = 0; i < vnetBitmap_.size(); i++)
    {
        uint32_t id = 1 << i;
        if (vnetBitmap_[i] == false)
        {
            vnetBitmap_[i] = true;
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

void VNetBitmapObject::recycleBitmapId(const string& vnet)
{
    SWSS_LOG_ENTER();

    uint32_t id = getBitmapId(vnet);
    if (id)
    {
        vnetBitmap_ &= ~id;
        vnetIds_.erase(vnet);
    }
}

uint32_t VNetBitmapObject::getFreeTunnelRouteTableOffset()
{
    SWSS_LOG_ENTER();

    for (uint32_t i = 0; i < tunnelOffsets_.size(); i++)
    {
        if (tunnelOffsets_[i] == false)
        {
            tunnelOffsets_[i] = true;
            return i;
        }
    }

    return -1;
}

void VNetBitmapObject::recycleTunnelRouteTableOffset(uint32_t offset)
{
    SWSS_LOG_ENTER();

    tunnelOffsets_[offset] = false;
}

uint16_t VNetBitmapObject::getFreeTunnelId()
{
    SWSS_LOG_ENTER();

    for (uint16_t i = 1; i < tunnelIdOffsets_.size(); i++)
    {
        if (tunnelIdOffsets_[i] == false)
        {
            tunnelIdOffsets_[i] = true;
            return i;
        }
    }

    return 0;
}

void VNetBitmapObject::recycleTunnelId(uint16_t offset)
{
    SWSS_LOG_ENTER();

    tunnelIdOffsets_[offset] = false;
}

VnetBridgeInfo VNetBitmapObject::getBridgeInfoByVni(uint32_t vni, string tunnelName)
{
    SWSS_LOG_ENTER();

    if (bridgeInfoMap_.find(vni) != bridgeInfoMap_.end())
    {
        bridgeInfoMap_.at(vni).use_count++;

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

    attr.id = SAI_ROUTER_INTERFACE_ATTR_BRIDGE_ID;
    attr.value.oid = info.bridge_id;
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

    // FIXME: Use "createVxlanTunnelMap()" for tunnel mapper creation
    auto tunnelEncapMapperEntry = tunnel->addEncapMapperEntry(info.bridge_id, vni);
    tunnel->insertMapperEntry(tunnelEncapMapperEntry, SAI_NULL_OBJECT_ID, vni);

    info.use_count = 1;
    bridgeInfoMap_.emplace(vni, info);

    return std::move(info);
}

bool VNetBitmapObject::clearBridgeInfoByVni(uint32_t vni, string tunnelName)
{
    if (bridgeInfoMap_.find(vni) == bridgeInfoMap_.end())
    {
        SWSS_LOG_ERROR("Bridge info doesn't exist for VNI %u", vni);
        return false;
    }

    auto& bridgeInfo = bridgeInfoMap_.at(vni);
    if (bridgeInfo.use_count > 1)
    {
        bridgeInfo.use_count--;
    }
    else
    {
        auto vxlan_orch = gDirectory.get<VxlanTunnelOrch*>();
        if (!vxlan_orch->removeVxlanTunnelMap(tunnelName, vni))
        {
            return false;
        }

        sai_status_t status;

        status = sai_bridge_api->remove_bridge_port(bridgeInfo.bridge_port_tunnel_id);
        if (status != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_ERROR("Failed to remove tunnel bridge port for VNI %u, SAI rc: %d", vni, status);
            return false;
        }

        status = sai_bridge_api->remove_bridge_port(bridgeInfo.bridge_port_rif_id);
        if (status != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_ERROR("Failed to remove RIF bridge port for VNI %u, SAI rc: %d", vni, status);
            return false;
        }

        status = sai_router_intfs_api->remove_router_interface(bridgeInfo.rif_id);
        if (status != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_ERROR("Failed to remove RIF for VNI %u, SAI rc: %d",vni, status);
            return false;
        }

        status = sai_bridge_api->remove_bridge(bridgeInfo.bridge_id);
        if (status != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_ERROR("Failed to remove bridge for VNI %u, SAI rc: %d", vni, status);
            return false;
        }

        bridgeInfoMap_.erase(vni);
    }

    return true;
}

bool VNetBitmapObject::clearNeighInfo(MacAddress mac, sai_object_id_t bridge)
{
    auto macBridge = make_tuple(mac, bridge);

    if (neighInfoMap_.find(macBridge) == neighInfoMap_.end())
    {
        SWSS_LOG_ERROR("VNET neighbor doesn't exist");
        return false;
    }

    if (neighInfoMap_.at(macBridge).use_count > 1)
    {
        neighInfoMap_.at(macBridge).use_count--;
    }
    else
    {
        sai_status_t status;

        status = sai_neighbor_api->remove_neighbor_entry(&neighInfoMap_.at(macBridge).neigh_entry);
        if (status != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_ERROR("Failed to remove neighbor entry, SAI rc: %d", status);
            return false;
        }

        status = sai_fdb_api->remove_fdb_entry(&neighInfoMap_.at(macBridge).fdb_entry);
        if (status != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_ERROR("Failed to remove FDB entry, SAI rc: %d", status);
            return false;
        }

        neighInfoMap_.erase(macBridge);
    }

    return true;
}

void VNetBitmapObject::setVniInfo(uint32_t vni)
{
    sai_attribute_t attr;
    vector<sai_attribute_t> vnet_attrs;
    auto info = getBridgeInfoByVni(getVni(), getTunnelName());

    attr.id = SAI_TABLE_BITMAP_CLASSIFICATION_ENTRY_ATTR_ACTION;
    attr.value.s32 = SAI_TABLE_BITMAP_CLASSIFICATION_ENTRY_ACTION_SET_METADATA;
    vnet_attrs.push_back(attr);

    attr.id = SAI_TABLE_BITMAP_CLASSIFICATION_ENTRY_ATTR_ROUTER_INTERFACE_KEY;
    attr.value.oid = info.rif_id;
    vnet_attrs.push_back(attr);

    attr.id = SAI_TABLE_BITMAP_CLASSIFICATION_ENTRY_ATTR_IN_RIF_METADATA;
    attr.value.u32 = vnet_id_;
    vnet_attrs.push_back(attr);

    sai_status_t status = sai_bmtor_api->create_table_bitmap_classification_entry(
            &vnetTableEntryId_,
            gSwitchId,
            (uint32_t)vnet_attrs.size(),
            vnet_attrs.data());

    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to create VNET table entry, SAI rc: %d", status);
        throw std::runtime_error("VNet interface creation failed");
    }
}

bool VNetBitmapObject::addIntf(const string& alias, const IpPrefix *prefix)
{
    SWSS_LOG_ENTER();

    sai_attribute_t attr;
    vector<sai_attribute_t> vnet_attrs;
    vector<sai_attribute_t> route_attrs;
    sai_status_t status;
    uint32_t peerBitmap = vnet_id_;

    if (prefix && !prefix->isV4())
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

    if (gIntfsOrch->getSyncdIntfses().find(alias) == gIntfsOrch->getSyncdIntfses().end())
    {
        if (intfMap_.find(alias) != intfMap_.end())
        {
            SWSS_LOG_ERROR("VNET '%s' interface '%s' already exists", getVnetName().c_str(), alias.c_str());
            return false;
        }

        if (!gIntfsOrch->setIntf(alias, gVirtualRouterId, nullptr))
        {
            return false;
        }

        VnetIntfInfo intfInfo;

        attr.id = SAI_TABLE_BITMAP_CLASSIFICATION_ENTRY_ATTR_ACTION;
        attr.value.s32 = SAI_TABLE_BITMAP_CLASSIFICATION_ENTRY_ACTION_SET_METADATA;
        vnet_attrs.push_back(attr);

        attr.id = SAI_TABLE_BITMAP_CLASSIFICATION_ENTRY_ATTR_ROUTER_INTERFACE_KEY;
        attr.value.oid = gIntfsOrch->getRouterIntfsId(alias);
        vnet_attrs.push_back(attr);

        attr.id = SAI_TABLE_BITMAP_CLASSIFICATION_ENTRY_ATTR_IN_RIF_METADATA;
        attr.value.u32 = vnet_id_;
        vnet_attrs.push_back(attr);

        status = sai_bmtor_api->create_table_bitmap_classification_entry(
                &intfInfo.vnetTableEntryId,
                gSwitchId,
                (uint32_t)vnet_attrs.size(),
                vnet_attrs.data());

        if (status != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_ERROR("Failed to create VNET table entry, SAI rc: %d", status);
            throw std::runtime_error("VNet interface creation failed");
        }

        intfMap_.emplace(alias, intfInfo);
    }

    if (prefix)
    {
        auto& intf = intfMap_.at(alias);

        if (intf.pfxMap.find(*prefix) != intf.pfxMap.end())
        {
            SWSS_LOG_WARN("VNET '%s' interface '%s' prefix '%s' already exists",
                    getVnetName().c_str(), alias.c_str(), prefix->getIp().to_string().c_str());
            return true;
        }

        RouteInfo intfPfxInfo;

        sai_ip_prefix_t saiPrefix;
        copy(saiPrefix, *prefix);

        gIntfsOrch->addIp2MeRoute(gVirtualRouterId, *prefix);

        attr.id = SAI_TABLE_BITMAP_ROUTER_ENTRY_ATTR_ACTION;
        attr.value.s32 = SAI_TABLE_BITMAP_ROUTER_ENTRY_ACTION_TO_LOCAL;
        route_attrs.push_back(attr);

        intfPfxInfo.offset = getFreeTunnelRouteTableOffset();
        attr.id = SAI_TABLE_BITMAP_ROUTER_ENTRY_ATTR_PRIORITY;
        attr.value.u32 = intfPfxInfo.offset;
        route_attrs.push_back(attr);

        attr.id = SAI_TABLE_BITMAP_ROUTER_ENTRY_ATTR_IN_RIF_METADATA_KEY;
        attr.value.u64 = 0;
        route_attrs.push_back(attr);

        attr.id = SAI_TABLE_BITMAP_ROUTER_ENTRY_ATTR_IN_RIF_METADATA_MASK;
        attr.value.u64 = ~peerBitmap;
        route_attrs.push_back(attr);

        attr.id = SAI_TABLE_BITMAP_ROUTER_ENTRY_ATTR_DST_IP_KEY;
        attr.value.ipprefix = saiPrefix;
        route_attrs.push_back(attr);

        attr.id = SAI_TABLE_BITMAP_ROUTER_ENTRY_ATTR_ROUTER_INTERFACE;
        attr.value.oid = gIntfsOrch->getRouterIntfsId(alias);
        route_attrs.push_back(attr);

        status = sai_bmtor_api->create_table_bitmap_router_entry(
                &intfPfxInfo.routeTableEntryId,
                gSwitchId,
                (uint32_t)route_attrs.size(),
                route_attrs.data());

        if (status != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_ERROR("Failed to create local VNET route entry, SAI rc: %d", status);
            throw std::runtime_error("VNet interface creation failed");
        }

        intf.pfxMap.emplace(*prefix, intfPfxInfo);
    }

    return true;
}

bool VNetBitmapObject::removeIntf(const string& alias, const IpPrefix *prefix)
{
    SWSS_LOG_ENTER();

    sai_status_t status;

    if (intfMap_.find(alias) == intfMap_.end())
    {
        SWSS_LOG_ERROR("VNET '%s' interface '%s' doesn't exist", getVnetName().c_str(), alias.c_str());
        return false;
    }

    auto& intf = intfMap_.at(alias);

    if (prefix)
    {
        if (intf.pfxMap.find(*prefix) == intf.pfxMap.end())
        {
            SWSS_LOG_ERROR("VNET '%s' interface '%s' prefix '%s' doesn't exist",
                    getVnetName().c_str(), alias.c_str(), prefix->getIp().to_string().c_str());
            return true;
        }

        auto& pfx = intf.pfxMap.at(*prefix);

        status = sai_bmtor_api->remove_table_bitmap_router_entry(pfx.routeTableEntryId);
        if (status != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_ERROR("Failed to remove VNET local route entry, SAI rc: %d", status);
            throw std::runtime_error("VNET interface removal failed");
        }

        gIntfsOrch->removeIp2MeRoute(gVirtualRouterId, *prefix);

        recycleTunnelRouteTableOffset(pfx.offset);

        intf.pfxMap.erase(*prefix);
    }

    if (intf.pfxMap.size() == 0)
    {
        status = sai_bmtor_api->remove_table_bitmap_classification_entry(intf.vnetTableEntryId);
        if (status != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_ERROR("Failed to remove VNET table entry, SAI rc: %d", status);
            throw std::runtime_error("VNET interface removal failed");
        }

        intfMap_.erase(alias);

        if (!gIntfsOrch->removeIntf(alias, gVirtualRouterId, nullptr))
        {
            return false;
        }
    }

    return true;
}

uint32_t VNetBitmapObject::getFreeNeighbor(void)
{
    static set<uint32_t> neighbors;

    for (uint32_t i = 0; i < VNET_NEIGHBOR_MAX; i++)
    {
        if (neighbors.count(i) == 0)
        {
            neighbors.insert(i);
            return i;
        }
    }

    SWSS_LOG_ERROR("No neighbors left");
    throw std::runtime_error("VNet route creation failed");
}

bool VNetBitmapObject::addTunnelRoute(IpPrefix& ipPrefix, tunnelEndpoint& endp)
{
    SWSS_LOG_ENTER();

    sai_status_t status;
    sai_attribute_t attr;
    auto& peer_list = getPeerList();
    auto bInfo = getBridgeInfoByVni(endp.vni == 0 ? getVni() : endp.vni, getTunnelName());
    uint32_t peerBitmap = vnet_id_;
    MacAddress mac = endp.mac ? endp.mac : gVxlanMacAddress;
    TunnelRouteInfo tunnelRouteInfo;
    sai_ip_address_t underlayAddr;
    copy(underlayAddr, endp.ip);

    VNetOrch* vnet_orch = gDirectory.get<VNetOrch*>();
    for (auto peer : peer_list)
    {
        if (!vnet_orch->isVnetExists(peer))
        {
            SWSS_LOG_INFO("Peer VNET %s not yet created", peer.c_str());
            return false;
        }
        peerBitmap |= getBitmapId(peer);
    }

    auto macBridge = make_tuple(mac, bInfo.bridge_id);
    if (neighInfoMap_.find(macBridge) == neighInfoMap_.end())
    {
        VnetNeighInfo neighInfo;

        /* FDB entry to the tunnel */
        vector<sai_attribute_t> fdb_attrs;
        neighInfo.fdb_entry.switch_id = gSwitchId;
        mac.getMac(neighInfo.fdb_entry.mac_address);
        neighInfo.fdb_entry.bv_id = bInfo.bridge_id;

        attr.id = SAI_FDB_ENTRY_ATTR_TYPE;
        attr.value.s32 = SAI_FDB_ENTRY_TYPE_STATIC;
        fdb_attrs.push_back(attr);

        attr.id = SAI_FDB_ENTRY_ATTR_BRIDGE_PORT_ID;
        attr.value.oid = bInfo.bridge_port_rif_id;
        fdb_attrs.push_back(attr);

        attr.id = SAI_FDB_ENTRY_ATTR_PACKET_ACTION;
        attr.value.s32 = SAI_PACKET_ACTION_FORWARD;
        fdb_attrs.push_back(attr);

        status = sai_fdb_api->create_fdb_entry(
                &neighInfo.fdb_entry,
                (uint32_t)fdb_attrs.size(),
                fdb_attrs.data());
        if (status != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_ERROR("Failed to create fdb entry for tunnel, SAI rc: %d", status);
            throw std::runtime_error("VNet route creation failed");
        }

        /* Fake neighbor */
        neighInfo.neigh_entry.switch_id = gSwitchId;
        neighInfo.neigh_entry.rif_id = bInfo.rif_id;
        neighInfo.neigh_entry.ip_address.addr_family = SAI_IP_ADDR_FAMILY_IPV4;
        neighInfo.neigh_entry.ip_address.addr.ip4 = htonl(getFreeNeighbor());
        
        vector<sai_attribute_t> n_attrs;
        attr.id = SAI_NEIGHBOR_ENTRY_ATTR_DST_MAC_ADDRESS;
        mac.getMac(attr.value.mac);
        n_attrs.push_back(attr);

        status = sai_neighbor_api->create_neighbor_entry(
                &neighInfo.neigh_entry,
                (uint32_t)n_attrs.size(),
                n_attrs.data());
        if (status != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_ERROR("Failed to create neighbor entry for tunnel, SAI rc: %d", status);
            throw std::runtime_error("VNet route creation failed");
        }
        
        neighInfo.use_count = 1;
        neighInfoMap_.emplace(macBridge, neighInfo);
    }
    else
    {
        neighInfoMap_.at(macBridge).use_count++;
    }

    /* Nexthop */
    vector<sai_attribute_t> nh_attrs;

    attr.id = SAI_NEXT_HOP_ATTR_TYPE;
    attr.value.s32 = SAI_NEXT_HOP_TYPE_IP;
    nh_attrs.push_back(attr);

    attr.id = SAI_NEXT_HOP_ATTR_IP;
    attr.value.ipaddr = neighInfoMap_.at(macBridge).neigh_entry.ip_address;
    nh_attrs.push_back(attr);

    attr.id = SAI_NEXT_HOP_ATTR_ROUTER_INTERFACE_ID;
    attr.value.oid = bInfo.rif_id;
    nh_attrs.push_back(attr);

    status = sai_next_hop_api->create_next_hop(
            &tunnelRouteInfo.nexthopId,
            gSwitchId,
            (uint32_t)nh_attrs.size(),
            nh_attrs.data());
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to create nexthop for tunnel, SAI rc: %d", status);
        throw std::runtime_error("VNet route creation failed");
    }

    /* Tunnel endpoint */
    VxlanTunnelOrch* vxlan_orch = gDirectory.get<VxlanTunnelOrch*>();
    auto *tunnel = vxlan_orch->getVxlanTunnel(getTunnelName());
    auto endpoint = make_tuple(endp.ip, tunnel->getTunnelId());
    uint16_t tunnelIndex = 0;
    TunnelEndpointInfo endpointInfo;
    if (endpointMap_.find(endpoint) == endpointMap_.end())
    {
        tunnelIndex = getFreeTunnelId();
        vector<sai_attribute_t> vxlan_attrs;

        attr.id = SAI_TABLE_META_TUNNEL_ENTRY_ATTR_ACTION;
        attr.value.s32 = SAI_TABLE_META_TUNNEL_ENTRY_ACTION_TUNNEL_ENCAP;
        vxlan_attrs.push_back(attr);

        attr.id = SAI_TABLE_META_TUNNEL_ENTRY_ATTR_METADATA_KEY;
        attr.value.u16 = tunnelIndex;
        vxlan_attrs.push_back(attr);

        attr.id = SAI_TABLE_META_TUNNEL_ENTRY_ATTR_UNDERLAY_DIP;
        attr.value.ipaddr = underlayAddr;
        vxlan_attrs.push_back(attr);

        attr.id = SAI_TABLE_META_TUNNEL_ENTRY_ATTR_TUNNEL_ID;
        attr.value.oid = tunnel->getTunnelId();
        vxlan_attrs.push_back(attr);

        status = sai_bmtor_api->create_table_meta_tunnel_entry(
                &endpointInfo.metaTunnelEntryId,
                gSwitchId,
                (uint32_t)vxlan_attrs.size(),
                vxlan_attrs.data());

        if (status != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_ERROR("Failed to create L3 VXLAN entry, SAI rc: %d", status);
            throw std::runtime_error("VNet route creation failed");
        }

        endpointInfo.tunnelIndex = tunnelIndex;
        endpointInfo.use_count = 1;
        endpointMap_.emplace(endpoint, endpointInfo);
    }
    else
    {
        tunnelIndex = endpointMap_.at(endpoint).tunnelIndex;
        endpointMap_.at(endpoint).use_count++;
    }

    /* Tunnel route */
    vector<sai_attribute_t> tr_attrs;
    sai_ip_prefix_t pfx;
    copy(pfx, ipPrefix);

    attr.id = SAI_TABLE_BITMAP_ROUTER_ENTRY_ATTR_ACTION;
    attr.value.s32 = SAI_TABLE_BITMAP_ROUTER_ENTRY_ACTION_TO_NEXTHOP;
    tr_attrs.push_back(attr);

    tunnelRouteInfo.offset = getFreeTunnelRouteTableOffset();
    attr.id = SAI_TABLE_BITMAP_ROUTER_ENTRY_ATTR_PRIORITY;
    attr.value.u32 = tunnelRouteInfo.offset;
    tr_attrs.push_back(attr);

    attr.id = SAI_TABLE_BITMAP_ROUTER_ENTRY_ATTR_IN_RIF_METADATA_KEY;
    attr.value.u64 = 0;
    tr_attrs.push_back(attr);

    attr.id = SAI_TABLE_BITMAP_ROUTER_ENTRY_ATTR_IN_RIF_METADATA_MASK;
    attr.value.u64 = ~peerBitmap;
    tr_attrs.push_back(attr);

    attr.id = SAI_TABLE_BITMAP_ROUTER_ENTRY_ATTR_DST_IP_KEY;
    attr.value.ipprefix = pfx;
    tr_attrs.push_back(attr);

    attr.id = SAI_TABLE_BITMAP_ROUTER_ENTRY_ATTR_NEXT_HOP;
    attr.value.oid = tunnelRouteInfo.nexthopId;
    tr_attrs.push_back(attr);

    attr.id = SAI_TABLE_BITMAP_ROUTER_ENTRY_ATTR_TUNNEL_INDEX;
    attr.value.u16 = tunnelIndex;
    tr_attrs.push_back(attr);

    status = sai_bmtor_api->create_table_bitmap_router_entry(
            &tunnelRouteInfo.tunnelRouteTableEntryId,
            gSwitchId,
            (uint32_t)tr_attrs.size(),
            tr_attrs.data());

    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to create local VNET route entry, SAI rc: %d", status);
        throw std::runtime_error("VNet route creation failed");
    }

    tunnelRouteInfo.vni = endp.vni == 0 ? getVni() : endp.vni;
    tunnelRouteInfo.mac = mac;
    tunnelRouteInfo.ip = endp.ip;
    tunnelRouteInfo.tunnelId = tunnel->getTunnelId();
    tunnelRouteMap_.emplace(ipPrefix, tunnelRouteInfo);

    return true;
}

bool VNetBitmapObject::removeTunnelRoute(IpPrefix& ipPrefix)
{
    SWSS_LOG_ENTER();

    if (tunnelRouteMap_.find(ipPrefix) == tunnelRouteMap_.end())
    {
        SWSS_LOG_WARN("VNET tunnel route %s doesn't exist", ipPrefix.to_string().c_str());
        return false;
    }

    auto tunnelRouteInfo = tunnelRouteMap_.at(ipPrefix);

    if (bridgeInfoMap_.find(tunnelRouteInfo.vni) == bridgeInfoMap_.end())
    {
        SWSS_LOG_ERROR("VNET bridge doesn't exist for tunnel route %s", ipPrefix.to_string().c_str());
        throw std::runtime_error("VNET tunnel route removal failed");
    }

    auto bridgeInfo = bridgeInfoMap_.at(tunnelRouteInfo.vni);
    auto macBridge = make_tuple(tunnelRouteInfo.mac, bridgeInfo.bridge_id);

    if (neighInfoMap_.find(macBridge) == neighInfoMap_.end())
    {
        SWSS_LOG_ERROR("VNET neighbor doesn't exist for tunnel route %s", ipPrefix.to_string().c_str());
        throw std::runtime_error("VNET tunnel route removal failed");
    }

    auto endpoint = make_tuple(tunnelRouteInfo.ip, tunnelRouteInfo.tunnelId);

    if (endpointMap_.find(endpoint) == endpointMap_.end())
    {
        SWSS_LOG_ERROR("Tunnel endpoint doesn't exist for tunnel route %s", ipPrefix.to_string().c_str());
        throw std::runtime_error("VNET tunnel route removal failed");
    }

    auto endpointInfo = endpointMap_.at(endpoint);

    sai_status_t status;

    status = sai_bmtor_api->remove_table_bitmap_router_entry(tunnelRouteInfo.tunnelRouteTableEntryId);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to remove VNET tunnel route entry, SAI rc: %d", status);
        throw std::runtime_error("VNET tunnel route removal failed");
    }

    if (endpointInfo.use_count > 1)
    {
        endpointInfo.use_count--;
    }
    else
    {
        status = sai_bmtor_api->remove_table_meta_tunnel_entry(endpointInfo.metaTunnelEntryId);
        if (status != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_ERROR("Failed to remove meta tunnel entry for VNET tunnel route, SAI rc: %d", status);
            throw std::runtime_error("VNET tunnel route removal failed");
        }

        recycleTunnelId(endpointInfo.tunnelIndex);
        endpointMap_.erase(endpoint);
    }

    status = sai_next_hop_api->remove_next_hop(tunnelRouteInfo.nexthopId);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to remove nexthop entry for VNET tunnel route, SAI rc: %d", status);
        throw std::runtime_error("VNET tunnel route removal failed");
    }

    if (!clearNeighInfo(tunnelRouteInfo.mac, bridgeInfo.bridge_id))
    {
        throw std::runtime_error("VNET tunnel route removal failed");
    }

    if (!clearBridgeInfoByVni(tunnelRouteInfo.vni, getTunnelName()))
    {
        throw std::runtime_error("VNET tunnel route removal failed");
    }

    recycleTunnelRouteTableOffset(tunnelRouteInfo.offset);
    tunnelRouteMap_.erase(ipPrefix);

    return true;
}

bool VNetBitmapObject::addRoute(IpPrefix& ipPrefix, nextHop& nh)
{
    SWSS_LOG_ENTER();

    sai_status_t status;
    sai_attribute_t attr;
    vector<sai_attribute_t> attrs;
    sai_ip_prefix_t pfx;
    sai_object_id_t nh_id = SAI_NULL_OBJECT_ID;
    uint32_t peerBitmap = vnet_id_;
    Port port;
    RouteInfo routeInfo;

    bool is_subnet = (!nh.ips.getSize()) ? true : false;

    if (is_subnet && (!gPortsOrch->getPort(nh.ifname, port) || (port.m_rif_id == SAI_NULL_OBJECT_ID)))
    {
        SWSS_LOG_WARN("Port/RIF %s doesn't exist", nh.ifname.c_str());
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

    /* Local route */
    copy(pfx, ipPrefix);

    if (is_subnet)
    {
        attr.id = SAI_TABLE_BITMAP_ROUTER_ENTRY_ATTR_ACTION;
        attr.value.s32 = SAI_TABLE_BITMAP_ROUTER_ENTRY_ACTION_TO_LOCAL;
        attrs.push_back(attr);

        attr.id = SAI_TABLE_BITMAP_ROUTER_ENTRY_ATTR_ROUTER_INTERFACE;
        attr.value.oid = port.m_rif_id;
        attrs.push_back(attr);
    }
    else if (nh.ips.getSize() == 1)
    {
        IpAddress ip_address(nh.ips.to_string());
        if (gNeighOrch->hasNextHop(ip_address))
        {
            nh_id = gNeighOrch->getNextHopId(ip_address);
        }
        else
        {
            SWSS_LOG_INFO("Failed to get next hop %s for %s",
                          ip_address.to_string().c_str(), ipPrefix.to_string().c_str());
            return false;
        }

        attr.id = SAI_TABLE_BITMAP_ROUTER_ENTRY_ATTR_ACTION;
        attr.value.s32 = SAI_TABLE_BITMAP_ROUTER_ENTRY_ACTION_TO_NEXTHOP;
        attrs.push_back(attr);

        attr.id = SAI_TABLE_BITMAP_ROUTER_ENTRY_ATTR_NEXT_HOP;
        attr.value.oid = nh_id;
        attrs.push_back(attr);

        attr.id = SAI_TABLE_BITMAP_ROUTER_ENTRY_ATTR_TUNNEL_INDEX;
        attr.value.u16 = 0;
        attrs.push_back(attr);

    }
    else
    {
        /* FIXME - Handle ECMP routes */
        SWSS_LOG_WARN("VNET ECMP NHs not implemented for '%s'", ipPrefix.to_string().c_str());
        return true;
    }

    routeInfo.offset = getFreeTunnelRouteTableOffset();
    attr.id = SAI_TABLE_BITMAP_ROUTER_ENTRY_ATTR_PRIORITY;
    attr.value.u32 = routeInfo.offset;
    attrs.push_back(attr);

    attr.id = SAI_TABLE_BITMAP_ROUTER_ENTRY_ATTR_IN_RIF_METADATA_KEY;
    attr.value.u64 = 0;
    attrs.push_back(attr);

    attr.id = SAI_TABLE_BITMAP_ROUTER_ENTRY_ATTR_IN_RIF_METADATA_MASK;
    attr.value.u64 = ~peerBitmap;
    attrs.push_back(attr);

    attr.id = SAI_TABLE_BITMAP_ROUTER_ENTRY_ATTR_DST_IP_KEY;
    attr.value.ipprefix = pfx;
    attrs.push_back(attr);

    status = sai_bmtor_api->create_table_bitmap_router_entry(
            &routeInfo.routeTableEntryId,
            gSwitchId,
            (uint32_t)attrs.size(),
            attrs.data());

    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to create local VNET route entry, SAI rc: %d", status);
        throw std::runtime_error("VNet route creation failed");
    }

    routeMap_.emplace(ipPrefix, routeInfo);

    return true;
}

bool VNetBitmapObject::removeRoute(IpPrefix& ipPrefix)
{
    SWSS_LOG_ENTER();

    if (routeMap_.find(ipPrefix) == routeMap_.end())
    {
        SWSS_LOG_WARN("VNET route %s doesn't exist", ipPrefix.to_string().c_str());
        return false;
    }

    sai_status_t status = sai_bmtor_api->remove_table_bitmap_router_entry(routeMap_.at(ipPrefix).routeTableEntryId);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to remove VNET route entry, SAI rc: %d", status);
        throw std::runtime_error("VNET route removal failed");
    }

    recycleTunnelRouteTableOffset(routeMap_.at(ipPrefix).offset);

    routeMap_.erase(ipPrefix);

    return true;
}

VNetBitmapObject::~VNetBitmapObject()
{
    sai_status_t status;
    
    status = sai_bmtor_api->remove_table_bitmap_classification_entry(vnetTableEntryId_);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to remove VNET '%s' table entry, SAI rc: %d", getVnetName().c_str(), status);
        throw std::runtime_error("VNET removal failed");
    }
    
    if (!clearBridgeInfoByVni(getVni(), getTunnelName()))
    {
        throw std::runtime_error("VNET removal failed");
    }

    recycleBitmapId(getVnetName());

    SWSS_LOG_INFO("VNET '%s' deleted ", getVnetName().c_str());
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
        vr_cntxt = { VR_TYPE::ING_VR_VALID, VR_TYPE::EGR_VR_VALID };
    }
    else
    {
        // BRIDGE Handling
    }
}

bool VNetOrch::setIntf(const string& alias, const string name, const IpPrefix *prefix)
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

        return gIntfsOrch->setIntf(alias, vrf_id, prefix);
    }
    else
    {
        auto *vnet_obj = getTypePtr<VNetBitmapObject>(name);
        return vnet_obj->addIntf(alias, prefix);
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
    else
    {
        auto *vnet_obj = getTypePtr<VNetBitmapObject>(name);
        return vnet_obj->removeIntf(alias, prefix);
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
                VNetInfo vnet_info = { tunnel, vni, peer_list };
                obj = createObject<VNetVrfObject>(vnet_name, vnet_info, attrs);
                create = true;
            }

            VNetVrfObject *vrf_obj = dynamic_cast<VNetVrfObject*>(obj.get());
            if (!vxlan_orch->createVxlanTunnelMap(tunnel, TUNNEL_MAP_T_VIRTUAL_ROUTER, vni,
                                                  vrf_obj->getEncapMapId(), vrf_obj->getDecapMapId()))
            {
                SWSS_LOG_ERROR("VNET '%s', tunnel '%s', map create failed",
                                vnet_name.c_str(), tunnel.c_str());
            }

            SWSS_LOG_INFO("VNET '%s' was added ", vnet_name.c_str());
        }
        else
        {
            VxlanTunnelOrch* vxlan_orch = gDirectory.get<VxlanTunnelOrch*>();

            if (!vxlan_orch->isTunnelExists(tunnel))
            {
                SWSS_LOG_WARN("Vxlan tunnel '%s' doesn't exist", tunnel.c_str());
                return false;
            }

            if (it == std::end(vnet_table_))
            {
                VNetInfo vnet_info = { tunnel, vni, peer_list };
                obj = createObject<VNetBitmapObject>(vnet_name, vnet_info, attrs);
                create = true;
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
        else
        {
            auto vnet_obj = dynamic_cast<VNetBitmapObject*>(it->second.get());

            if (vnet_obj->getRouteCount())
            {
                SWSS_LOG_ERROR("VNET '%s': Routes are still present", vnet_name.c_str());
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
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("SAI Failed to remove route");
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

VNetRouteOrch::VNetRouteOrch(DBConnector *db, vector<string> &tableNames, VNetOrch *vnetOrch, ChassisFrontendOrch * chassisFrontendOrch)
                                  : Orch2(db, tableNames, request_), vnet_orch_(vnetOrch), chassis_frontend_orch_(chassisFrontendOrch)
{
    SWSS_LOG_ENTER();

    handler_map_.insert(handler_pair(APP_VNET_RT_TABLE_NAME, &VNetRouteOrch::handleRoutes));
    handler_map_.insert(handler_pair(APP_VNET_RT_TUNNEL_TABLE_NAME, &VNetRouteOrch::handleTunnel));
}

template<>
bool VNetRouteOrch::doRouteTask<VNetVrfObject>(const string& vnet, IpPrefix& ipPrefix,
                                               tunnelEndpoint& endp, string& op)
{
    SWSS_LOG_ENTER();

    if (!vnet_orch_->isVnetExists(vnet))
    {
        SWSS_LOG_WARN("VNET %s doesn't exist", vnet.c_str());
        return false;
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
    sai_object_id_t nh_id = (op == SET_COMMAND)?vrf_obj->getTunnelNextHop(endp):SAI_NULL_OBJECT_ID;

    for (auto vr_id : vr_set)
    {
        if (op == SET_COMMAND && !add_route(vr_id, pfx, nh_id))
        {
            SWSS_LOG_ERROR("Route add failed for %s, vr_id '0x%lx", ipPrefix.to_string().c_str(), vr_id);
            return false;
        }
        else if (op == DEL_COMMAND && !del_route(vr_id, pfx))
        {
            SWSS_LOG_ERROR("Route del failed for %s, vr_id '0x%lx", ipPrefix.to_string().c_str(), vr_id);
            return false;
        }
    }

    if (op == SET_COMMAND)
    {
        vrf_obj->addRoute(ipPrefix, endp);
    }
    else
    {
        vrf_obj->removeRoute(ipPrefix);
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
        SWSS_LOG_WARN("VNET %s doesn't exist", vnet.c_str());
        return false;
    }

    auto *vrf_obj = vnet_orch_->getTypePtr<VNetVrfObject>(vnet);
    if (op == DEL_COMMAND && !vrf_obj->getRouteNextHop(ipPrefix, nh))
    {
        SWSS_LOG_WARN("VNET %s, Route %s get NH failed", vnet.c_str(), ipPrefix.to_string().c_str());
        return true;
    }

    bool is_subnet = (!nh.ips.getSize())?true:false;

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
        vr_set.insert(vrf_obj->getVRidEgress());
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
        IpAddress ip_address(nh.ips.to_string());
        if (gNeighOrch->hasNextHop(ip_address))
        {
            nh_id = gNeighOrch->getNextHopId(ip_address);
        }
        else
        {
            SWSS_LOG_INFO("Failed to get next hop %s for %s",
                           ip_address.to_string().c_str(), ipPrefix.to_string().c_str());
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
        if (op == SET_COMMAND && !add_route(vr_id, pfx, nh_id))
        {
            SWSS_LOG_ERROR("Route add failed for %s", ipPrefix.to_string().c_str());
            break;
        }
        else if (op == DEL_COMMAND && !del_route(vr_id, pfx))
        {
            SWSS_LOG_ERROR("Route del failed for %s", ipPrefix.to_string().c_str());
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

template<>
bool VNetRouteOrch::doRouteTask<VNetBitmapObject>(const string& vnet, IpPrefix& ipPrefix, tunnelEndpoint& endp, string& op)
{
    SWSS_LOG_ENTER();

    if (!vnet_orch_->isVnetExists(vnet))
    {
        SWSS_LOG_WARN("VNET %s doesn't exist", vnet.c_str());
        return false;
    }

    auto *vnet_obj = vnet_orch_->getTypePtr<VNetBitmapObject>(vnet);

    if (op == SET_COMMAND)
    {
        return vnet_obj->addTunnelRoute(ipPrefix, endp);
    }
    else
    {
        return vnet_obj->removeTunnelRoute(ipPrefix);
    }

    return true;
}

template<>
bool VNetRouteOrch::doRouteTask<VNetBitmapObject>(const string& vnet, IpPrefix& ipPrefix, nextHop& nh, string& op)
{
    SWSS_LOG_ENTER();

    if (!vnet_orch_->isVnetExists(vnet))
    {
        SWSS_LOG_WARN("VNET %s doesn't exist", vnet.c_str());
        return false;
    }

    auto *vnet_obj = vnet_orch_->getTypePtr<VNetBitmapObject>(vnet);

    if (op == SET_COMMAND)
    {
        return vnet_obj->addRoute(ipPrefix, nh);
    }
    else
    {
        return vnet_obj->removeRoute(ipPrefix);
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
    
    if (chassis_frontend_orch_)
    {
        chassis_frontend_orch_->handleVNetRoutesMessage(op, ip_pfx);
    }

    if (vnet_orch_->isVnetExecVrf())
    {
        return doRouteTask<VNetVrfObject>(vnet_name, ip_pfx, nh, op);
    }
    else
    {
        return doRouteTask<VNetBitmapObject>(vnet_name, ip_pfx, nh, op);
    }

    return true;
}

bool VNetRouteOrch::handleTunnel(const Request& request)
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
            SWSS_LOG_INFO("Unknown attribute: %s", name.c_str());
            continue;
        }
    }

    const std::string& vnet_name = request.getKeyString(0);
    auto ip_pfx = request.getKeyIpPrefix(1);
    auto op = request.getOperation();

    SWSS_LOG_INFO("VNET-RT '%s' op '%s' for pfx %s", vnet_name.c_str(),
                   op.c_str(), ip_pfx.to_string().c_str());

    tunnelEndpoint endp = { ip, mac, vni };

    if (vnet_orch_->isVnetExecVrf())
    {
        return doRouteTask<VNetVrfObject>(vnet_name, ip_pfx, endp, op);
    }
    else
    {
        return doRouteTask<VNetBitmapObject>(vnet_name, ip_pfx, endp, op);
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
