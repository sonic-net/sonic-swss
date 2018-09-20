#include <cassert>
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <stdexcept>

#include "sai.h"
#include "macaddress.h"
#include "ipaddress.h"
#include "orch.h"
#include "request_parser.h"
#include "vxlanorch.h"
#include "directory.h"
#include "swssnet.h"

/* Global variables */
extern sai_object_id_t gSwitchId;
extern sai_object_id_t gVirtualRouterId;
extern sai_tunnel_api_t *sai_tunnel_api;
extern sai_next_hop_api_t *sai_next_hop_api;
extern Directory<Orch*> gDirectory;
extern PortsOrch*       gPortsOrch;
extern sai_object_id_t  gUnderlayIfId;

static sai_object_id_t
create_tunnel_map(map_type maptype)
{
    sai_attribute_t attr;
    std::vector<sai_attribute_t> tunnel_map_attrs;

    attr.id = SAI_TUNNEL_MAP_ATTR_TYPE;

    switch (maptype)
    {
        case map_type::VNI_TO_VLAN_ID:
            attr.value.s32 = SAI_TUNNEL_MAP_TYPE_VNI_TO_VLAN_ID;
            break;
        case map_type::VLAN_ID_TO_VNI:
            attr.value.s32 = SAI_TUNNEL_MAP_TYPE_VLAN_ID_TO_VNI;
            break;
        case map_type::VRID_TO_VNI:
            attr.value.s32 = SAI_TUNNEL_MAP_TYPE_VIRTUAL_ROUTER_ID_TO_VNI;
            break;
        case map_type::VNI_TO_VRID:
            attr.value.s32 = SAI_TUNNEL_MAP_TYPE_VNI_TO_VIRTUAL_ROUTER_ID;
            break;
        default:
            SWSS_LOG_NOTICE("Invalid map type %d", maptype);
            return 0;
    }
    tunnel_map_attrs.push_back(attr);

    sai_object_id_t tunnel_map_id;
    sai_status_t status = sai_tunnel_api->create_tunnel_map(
                                &tunnel_map_id,
                                gSwitchId,
                                static_cast<uint32_t>(tunnel_map_attrs.size()),
                                tunnel_map_attrs.data()
                          );
    if (status != SAI_STATUS_SUCCESS)
    {
        throw std::runtime_error("Can't create tunnel map object");
    }

    return tunnel_map_id;
}

sai_object_id_t create_encap_tunnel_map_entry(
    sai_object_id_t tunnel_map_id,
    sai_object_id_t router_id,
    sai_uint32_t vni)
{
    sai_attribute_t attr;
    sai_object_id_t tunnel_map_entry_id;
    std::vector<sai_attribute_t> tunnel_map_entry_attrs;

    attr.id = SAI_TUNNEL_MAP_ENTRY_ATTR_TUNNEL_MAP_TYPE;
    attr.value.s32 = SAI_TUNNEL_MAP_TYPE_VIRTUAL_ROUTER_ID_TO_VNI;
    tunnel_map_entry_attrs.push_back(attr);

    attr.id = SAI_TUNNEL_MAP_ENTRY_ATTR_TUNNEL_MAP;
    attr.value.oid = tunnel_map_id;
    tunnel_map_entry_attrs.push_back(attr);

    attr.id = SAI_TUNNEL_MAP_ENTRY_ATTR_VIRTUAL_ROUTER_ID_KEY;
    attr.value.oid = router_id;
    tunnel_map_entry_attrs.push_back(attr);

    attr.id = SAI_TUNNEL_MAP_ENTRY_ATTR_VNI_ID_VALUE;
    attr.value.u32 = vni;
    tunnel_map_entry_attrs.push_back(attr);

    sai_tunnel_api->create_tunnel_map_entry(&tunnel_map_entry_id, gSwitchId,
                                            static_cast<uint32_t> (tunnel_map_entry_attrs.size()),
                                            tunnel_map_entry_attrs.data());

    return tunnel_map_entry_id;
}

sai_object_id_t create_decap_tunnel_map_entry(
    sai_object_id_t tunnel_map_id,
    sai_object_id_t router_id,
    sai_uint32_t vni)
{
    sai_attribute_t attr;
    sai_object_id_t tunnel_map_entry_id;
    std::vector<sai_attribute_t> tunnel_map_entry_attrs;

    attr.id = SAI_TUNNEL_MAP_ENTRY_ATTR_TUNNEL_MAP_TYPE;
    attr.value.s32 = SAI_TUNNEL_MAP_TYPE_VNI_TO_VIRTUAL_ROUTER_ID;
    tunnel_map_entry_attrs.push_back(attr);

    attr.id = SAI_TUNNEL_MAP_ENTRY_ATTR_TUNNEL_MAP;
    attr.value.oid = tunnel_map_id;
    tunnel_map_entry_attrs.push_back(attr);

    attr.id = SAI_TUNNEL_MAP_ENTRY_ATTR_VNI_ID_KEY;
    attr.value.u32 = vni;
    tunnel_map_entry_attrs.push_back(attr);

    attr.id = SAI_TUNNEL_MAP_ENTRY_ATTR_VIRTUAL_ROUTER_ID_VALUE;
    attr.value.oid = router_id;
    tunnel_map_entry_attrs.push_back(attr);

    sai_tunnel_api->create_tunnel_map_entry(&tunnel_map_entry_id, gSwitchId,
                                            static_cast<uint32_t> (tunnel_map_entry_attrs.size()),
                                            tunnel_map_entry_attrs.data());

    return tunnel_map_entry_id;
}

sai_status_t create_nexthop_tunnel(
    sai_ip_address_t host_ip,
    sai_uint32_t vni, // optional vni
    sai_mac_t mac, // inner destination mac
    sai_object_id_t tunnel_id,
    sai_object_id_t *next_hop_id)
{
    std::vector<sai_attribute_t> next_hop_attrs;
    sai_attribute_t next_hop_attr;

    next_hop_attr.id = SAI_NEXT_HOP_ATTR_TYPE;
    next_hop_attr.value.s32 = SAI_NEXT_HOP_TYPE_TUNNEL_ENCAP;
    next_hop_attrs.push_back(next_hop_attr);

    next_hop_attr.id = SAI_NEXT_HOP_ATTR_IP;
    next_hop_attr.value.ipaddr = host_ip;
    next_hop_attrs.push_back(next_hop_attr);

    next_hop_attr.id = SAI_NEXT_HOP_ATTR_TUNNEL_ID;
    next_hop_attr.value.oid = tunnel_id;
    next_hop_attrs.push_back(next_hop_attr);

    if (vni != 0)
    {
        next_hop_attr.id = SAI_NEXT_HOP_ATTR_TUNNEL_VNI;
        next_hop_attr.value.u32 = vni;
        next_hop_attrs.push_back(next_hop_attr);
    }

    if (mac != NULL)
    {
        next_hop_attr.id = SAI_NEXT_HOP_ATTR_TUNNEL_MAC;
        memcpy(next_hop_attr.value.mac, mac, sizeof(sai_mac_t));
        next_hop_attrs.push_back(next_hop_attr);
    }

    sai_status_t status = sai_next_hop_api->create_next_hop(next_hop_id, gSwitchId,
                                            static_cast<uint32_t>(next_hop_attrs.size()),
                                            next_hop_attrs.data());
    return status;
}

static sai_object_id_t
create_tunnel_map_entry(
    sai_object_id_t tunnel_map_id,
    sai_uint32_t vni,
    sai_uint16_t vlan_id)
{
    sai_attribute_t attr;
    std::vector<sai_attribute_t> tunnel_map_entry_attrs;

    attr.id = SAI_TUNNEL_MAP_ENTRY_ATTR_TUNNEL_MAP_TYPE;
    attr.value.s32 = SAI_TUNNEL_MAP_TYPE_VNI_TO_VLAN_ID;
    tunnel_map_entry_attrs.push_back(attr);

    attr.id = SAI_TUNNEL_MAP_ENTRY_ATTR_TUNNEL_MAP;
    attr.value.oid = tunnel_map_id;
    tunnel_map_entry_attrs.push_back(attr);

    attr.id = SAI_TUNNEL_MAP_ENTRY_ATTR_VNI_ID_KEY;
    attr.value.u32 = vni;
    tunnel_map_entry_attrs.push_back(attr);

    attr.id = SAI_TUNNEL_MAP_ENTRY_ATTR_VLAN_ID_VALUE;
    attr.value.u16 = vlan_id;
    tunnel_map_entry_attrs.push_back(attr);

    sai_object_id_t tunnel_map_entry_id;
    sai_status_t status = sai_tunnel_api->create_tunnel_map_entry(
                                &tunnel_map_entry_id,
                                gSwitchId,
                                static_cast<uint32_t>(tunnel_map_entry_attrs.size()),
                                tunnel_map_entry_attrs.data()
                          );
    if (status != SAI_STATUS_SUCCESS)
    {
        throw std::runtime_error("Can't create a tunnel map entry object");
    }

    return tunnel_map_entry_id;
}

// Create Tunnel
static sai_object_id_t
create_tunnel(
        sai_object_id_t tunnel_encap_id,
        sai_object_id_t tunnel_decap_id,
        sai_ip_address_t *src_ip,
        sai_object_id_t underlay_rif)
{
    sai_attribute_t attr;
    std::vector<sai_attribute_t> tunnel_attrs;

    attr.id = SAI_TUNNEL_ATTR_TYPE;
    attr.value.s32 = SAI_TUNNEL_TYPE_VXLAN;
    tunnel_attrs.push_back(attr);

    sai_object_id_t decap_list[] = { tunnel_decap_id };
    attr.id = SAI_TUNNEL_ATTR_DECAP_MAPPERS;
    attr.value.objlist.count = 1;
    attr.value.objlist.list = decap_list;
    tunnel_attrs.push_back(attr);

    if (tunnel_encap_id != 0x0)
    {
        sai_object_id_t encap_list[] = { tunnel_encap_id };
        attr.id = SAI_TUNNEL_ATTR_ENCAP_MAPPERS;
        attr.value.objlist.count = 1;
        attr.value.objlist.list = encap_list;
        tunnel_attrs.push_back(attr);
    }

    attr.id = SAI_TUNNEL_ATTR_UNDERLAY_INTERFACE;
    attr.value.oid = underlay_rif;
    tunnel_attrs.push_back(attr);

    // source ip
    if (src_ip != nullptr)
    {
        attr.id = SAI_TUNNEL_ATTR_ENCAP_SRC_IP;
        attr.value.ipaddr = *src_ip;
        tunnel_attrs.push_back(attr);
    }

    sai_object_id_t tunnel_id;
    sai_status_t status = sai_tunnel_api->create_tunnel(
                                &tunnel_id,
                                gSwitchId,
                                static_cast<uint32_t>(tunnel_attrs.size()),
                                tunnel_attrs.data()
                          );
    if (status != SAI_STATUS_SUCCESS)
    {
        throw std::runtime_error("Can't create a tunnel object");
    }

    return tunnel_id;
}

// Create tunnel termination

static sai_object_id_t
create_tunnel_termination(
    sai_object_id_t   tunnel_oid,
    sai_ip_address_t *srcip,
    sai_ip_address_t *dstip,
    sai_object_id_t   default_vrid)
{
    sai_attribute_t attr;
    std::vector<sai_attribute_t> tunnel_attrs;

    if(dstip == nullptr) // It's P2MP tunnel
    {
        attr.id = SAI_TUNNEL_TERM_TABLE_ENTRY_ATTR_TYPE;
        attr.value.s32 = SAI_TUNNEL_TERM_TABLE_ENTRY_TYPE_P2MP;
        tunnel_attrs.push_back(attr);
    }
    else
    {
        attr.id = SAI_TUNNEL_TERM_TABLE_ENTRY_ATTR_TYPE;
        attr.value.s32 = SAI_TUNNEL_TERM_TABLE_ENTRY_TYPE_P2P;
        tunnel_attrs.push_back(attr);

        attr.id = SAI_TUNNEL_TERM_TABLE_ENTRY_ATTR_SRC_IP;
        attr.value.ipaddr = *dstip;
        tunnel_attrs.push_back(attr);
    }

    attr.id = SAI_TUNNEL_TERM_TABLE_ENTRY_ATTR_VR_ID;
    attr.value.oid = default_vrid;
    tunnel_attrs.push_back(attr);

    attr.id = SAI_TUNNEL_TERM_TABLE_ENTRY_ATTR_DST_IP;
    attr.value.ipaddr = *srcip;
    tunnel_attrs.push_back(attr);

    attr.id = SAI_TUNNEL_TERM_TABLE_ENTRY_ATTR_TUNNEL_TYPE;
    attr.value.s32 = SAI_TUNNEL_TYPE_VXLAN;
    tunnel_attrs.push_back(attr);

    attr.id = SAI_TUNNEL_TERM_TABLE_ENTRY_ATTR_ACTION_TUNNEL_ID;
    attr.value.oid = tunnel_oid;
    tunnel_attrs.push_back(attr);

    sai_object_id_t term_table_id;
    sai_status_t status = sai_tunnel_api->create_tunnel_term_table_entry(
                                &term_table_id,
                                gSwitchId,
                                static_cast<uint32_t>(tunnel_attrs.size()),
                                tunnel_attrs.data()
                          );
    if (status != SAI_STATUS_SUCCESS)
    {
        throw std::runtime_error("Can't create a tunnel term table object");
    }

    return term_table_id;
}

bool VxlanTunnel::createTunnel(map_type encap, map_type decap)
{
    try
    {
        sai_ip_address_t ips, ipd, *ip=nullptr;
        swss::copy(ips, src_ip);

        ids.tunnel_decap_id = create_tunnel_map(decap);

        if (encap != map_type::MAP_TYPE_INVALID )
        {
            ids.tunnel_encap_id = create_tunnel_map(encap);
            ip = &ips;
        }

        ids.tunnel_id = create_tunnel(ids.tunnel_encap_id, ids.tunnel_decap_id, ip, gUnderlayIfId);

        ip = nullptr;
        if (!dst_ip.isZero())
        {
            swss::copy(ipd, dst_ip);
            ip = &ipd;
        }

        ids.tunnel_term_id = create_tunnel_termination(ids.tunnel_id, &ips, ip, gVirtualRouterId);
        active = true;
    }
    catch (const std::runtime_error& error)
    {
        SWSS_LOG_ERROR("Error creating tunnel %s: %s", tunnel_name.c_str(), error.what());
        // FIXME: add code to remove already created objects
        return false;
    }

    SWSS_LOG_NOTICE("Vxlan tunnel '%s' was created", tunnel_name.c_str());

    return true;
}

sai_object_id_t VxlanTunnel::addEncapMapperEntry(sai_object_id_t vrf, uint32_t vni)
{
    const auto encap_id = getEncapMapId(tunnel_name);
    return create_encap_tunnel_map_entry(encap_id, vrf, vni);
}

sai_object_id_t VxlanTunnel::addDecapMapperEntry(sai_object_id_t vrf, uint32_t vni)
{
    const auto decap_id = getDecapMapId(tunnel_name);
    return create_decap_tunnel_map_entry(decap_id, vrf, vni);
}

bool VxlanTunnelOrch::addOperation(const Request& request)
{
    SWSS_LOG_ENTER();

    auto src_ip = request.getAttrIP("src_ip");
    if (!src_ip.isV4())
    {
        SWSS_LOG_ERROR("Wrong format of the attribute: 'src_ip'. Currently only IPv4 address is supported");
        return true;
    }

    IpAddress dst_ip;
    auto attr_names = request.getAttrFieldNames();
    if (attr_names.count("dst_ip") == 0)
    {
        dst_ip = IpAddress("0.0.0.0");
    }
    else
    {
        dst_ip = request.getAttrIP("dst_ip");
        if (!dst_ip.isV4())
        {
            SWSS_LOG_ERROR("Wrong format of the attribute: 'dst_ip'. Currently only IPv4 address is supported");
            return true;
        }
    }

    const auto& tunnel_name = request.getKeyString(0);

    if(isTunnelExists(tunnel_name))
    {
        SWSS_LOG_ERROR("Vxlan tunnel '%s' is already exists", tunnel_name.c_str());
        return true;
    }

    std::unique_ptr<VxlanTunnel> tunnel_obj (new VxlanTunnel(tunnel_name, src_ip, dst_ip));
    vxlan_tunnel_table_[tunnel_name] = std::move(tunnel_obj);

    SWSS_LOG_NOTICE("Vxlan tunnel '%s' saved", tunnel_name.c_str());

    return true;
}

bool VxlanTunnelOrch::delOperation(const Request& request)
{
    SWSS_LOG_ENTER();

    SWSS_LOG_ERROR("DEL operation is not implemented");

    return true;
}

bool VxlanTunnelMapOrch::addOperation(const Request& request)
{
    SWSS_LOG_ENTER();

    auto vlan_id = request.getAttrVlan("vlan");
    Port tempPort;
    if(!gPortsOrch->getVlanByVlanId(vlan_id, tempPort))
    {
        SWSS_LOG_ERROR("Vxlan tunnel map vlan id doesn't exist: %d", vlan_id);
        return false;
    }

    auto vni_id  = static_cast<sai_uint32_t>(request.getAttrUint("vni"));
    if (vni_id >= 1<<24)
    {
        SWSS_LOG_ERROR("Vxlan tunnel map vni id is too big: %d", vni_id);
        return true;
    }

    auto tunnel_name = request.getKeyString(0);
    VxlanTunnelOrch* tunnel_orch = gDirectory.get<VxlanTunnelOrch*>();
    if (!tunnel_orch->isTunnelExists(tunnel_name))
    {
        SWSS_LOG_ERROR("Vxlan tunnel '%s' doesn't exist", tunnel_name.c_str());
        return false;
    }

    auto& tunnel_obj = tunnel_orch->getVxlanTunnel(tunnel_name);
    if (!tunnel_obj->isActive())
    {
        //@Todo, currently only decap mapper is allowed
        tunnel_obj->createTunnel(map_type::MAP_TYPE_INVALID, map_type::VNI_TO_VLAN_ID);
    }

    const auto full_tunnel_map_entry_name = request.getFullKey();
    if (isTunnelMapExists(full_tunnel_map_entry_name))
    {
        SWSS_LOG_ERROR("Vxlan tunnel map '%s' is already exist", full_tunnel_map_entry_name.c_str());
        return true;
    }

    const auto tunnel_map_id = tunnel_obj->getDecapMapId(tunnel_name);
    const auto tunnel_map_entry_name = request.getKeyString(1);

    try
    {
        auto tunnel_map_entry_id = create_tunnel_map_entry(tunnel_map_id, vni_id, vlan_id);
        vxlan_tunnel_map_table_[full_tunnel_map_entry_name] = tunnel_map_entry_id;
    }
    catch(const std::runtime_error& error)
    {
        SWSS_LOG_ERROR("Error adding tunnel map entry. Tunnel: %s. Entry: %s. Error: %s",
            tunnel_name.c_str(), tunnel_map_entry_name.c_str(), error.what());
        return false;
    }

    SWSS_LOG_NOTICE("Vxlan tunnel map entry '%s' for tunnel '%s' was created", tunnel_map_entry_name.c_str(), tunnel_name.c_str());

    return true;
}

bool VxlanTunnelMapOrch::delOperation(const Request& request)
{
    SWSS_LOG_ENTER();

    SWSS_LOG_ERROR("DEL operation is not implemented");

    return true;
}

bool VxlanVrfMapOrch::addOperation(const Request& request)
{
    SWSS_LOG_ENTER();

    auto tunnel_name = request.getKeyString(0);
    VxlanTunnelOrch* tunnel_orch = gDirectory.get<VxlanTunnelOrch*>();
    if (!tunnel_orch->isTunnelExists(tunnel_name))
    {
        SWSS_LOG_ERROR("Vxlan tunnel '%s' doesn't exist", tunnel_name.c_str());
        return false;
    }

    auto vni_id  = static_cast<sai_uint32_t>(request.getAttrUint("vni"));
    if (vni_id >= 1<<24)
    {
        SWSS_LOG_ERROR("Vxlan vni id is too big: %d", vni_id);
        return true;
    }

    auto& tunnel_obj = tunnel_orch->getVxlanTunnel(tunnel_name);
    if (!tunnel_obj->isActive())
    {
        tunnel_obj->createTunnel(map_type::VRID_TO_VNI, map_type::VNI_TO_VRID);
    }

    auto vrf_name = request.getAttrString("vrf");
    VRFOrch* vrf_orch = gDirectory.get<VRFOrch*>();
    if (!vrf_orch->isVRFexists(vrf_name))
    {
        SWSS_LOG_INFO("VRF not created for VNET %s", vrf_name.c_str());
        return false;
    }

    const auto full_map_entry_name = request.getFullKey();
    if (isVrfMapExists(full_map_entry_name))
    {
        SWSS_LOG_ERROR("Vxlan VRF map '%s' is already exist", full_map_entry_name.c_str());
        return true;
    }

    const auto tunnel_map_entry_name = request.getKeyString(1);
    sai_object_id_t ivrf, evrf;
    if (!vrf_orch->isMapexists(vrf_name))
    {
        ivrf = evrf = vrf_orch->getVRFid(vrf_name);
    }
    else
    {
        auto vnet_name = vrf_orch->getVnetName(vrf_name);
        ivrf = vrf_orch->getVRFidEgress(vnet_name);
        evrf = vrf_orch->getVRFidIngress(vnet_name);
    }

    vrf_map_entry_t entry;
    try
    {
        /*
         * Create encap and decap mapper for each VRF per VNET entry
         */
        entry.encap_id = tunnel_obj->addEncapMapperEntry(ivrf, vni_id);
        entry.decap_id = tunnel_obj->addDecapMapperEntry(evrf, vni_id);

        SWSS_LOG_INFO("Vxlan tunnel encap entry '%lx' decap entry '0x%lx'",
                entry.encap_id, entry.decap_id);

        vxlan_vrf_table_[full_map_entry_name] = entry;
    }
    catch(const std::runtime_error& error)
    {
        SWSS_LOG_ERROR("Error adding tunnel map entry. Tunnel: %s. Entry: %s. Error: %s",
            tunnel_name.c_str(), tunnel_map_entry_name.c_str(), error.what());
        return false;
    }

    SWSS_LOG_NOTICE("Vxlan vrf map entry '%s' for tunnel '%s' was created",
                    tunnel_map_entry_name.c_str(), tunnel_name.c_str());

    return true;
}

bool VxlanVrfMapOrch::delOperation(const Request& request)
{
    SWSS_LOG_ENTER();

    SWSS_LOG_ERROR("DEL operation is not implemented");

    return true;
}
