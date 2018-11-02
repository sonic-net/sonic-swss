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

const map<MAP_T, uint32_t> vxlanTunnelMap =
{
    { MAP_T::VNI_TO_VLAN_ID, SAI_TUNNEL_MAP_TYPE_VNI_TO_VLAN_ID },
    { MAP_T::VLAN_ID_TO_VNI, SAI_TUNNEL_MAP_TYPE_VLAN_ID_TO_VNI },
    { MAP_T::VRID_TO_VNI, SAI_TUNNEL_MAP_TYPE_VIRTUAL_ROUTER_ID_TO_VNI },
    { MAP_T::VNI_TO_VRID, SAI_TUNNEL_MAP_TYPE_VNI_TO_VIRTUAL_ROUTER_ID },
    { MAP_T::BRIDGE_TO_VNI, SAI_TUNNEL_MAP_TYPE_BRIDGE_IF_TO_VNI },
    { MAP_T::VNI_TO_BRIDGE,  SAI_TUNNEL_MAP_TYPE_VNI_TO_BRIDGE_IF},
};

const map<MAP_T, std::pair<uint32_t, uint32_t>> vxlanTunnelMapKeyVal =
{
    { MAP_T::VNI_TO_VLAN_ID,
        { SAI_TUNNEL_MAP_ENTRY_ATTR_VNI_ID_KEY, SAI_TUNNEL_MAP_ENTRY_ATTR_VLAN_ID_VALUE }
    },
    { MAP_T::VLAN_ID_TO_VNI,
        { SAI_TUNNEL_MAP_ENTRY_ATTR_VLAN_ID_KEY, SAI_TUNNEL_MAP_ENTRY_ATTR_VNI_ID_VALUE }
    },
    { MAP_T::VRID_TO_VNI,
        { SAI_TUNNEL_MAP_ENTRY_ATTR_VIRTUAL_ROUTER_ID_KEY, SAI_TUNNEL_MAP_ENTRY_ATTR_VNI_ID_VALUE }
    },
    { MAP_T::VNI_TO_VRID,
        { SAI_TUNNEL_MAP_ENTRY_ATTR_VNI_ID_KEY, SAI_TUNNEL_MAP_ENTRY_ATTR_VIRTUAL_ROUTER_ID_VALUE }
    },
    { MAP_T::BRIDGE_TO_VNI,
        { SAI_TUNNEL_MAP_ENTRY_ATTR_BRIDGE_ID_KEY, SAI_TUNNEL_MAP_ENTRY_ATTR_VNI_ID_VALUE }
    },
    { MAP_T::VNI_TO_BRIDGE,
        { SAI_TUNNEL_MAP_ENTRY_ATTR_VNI_ID_KEY, SAI_TUNNEL_MAP_ENTRY_ATTR_BRIDGE_ID_VALUE }
    },
};

/*
 * Manipulators for the above Map
 */
static inline uint32_t tunnel_map_type (MAP_T map_t)
{
    return vxlanTunnelMap.at(map_t);
}

static inline uint32_t tunnel_map_key (MAP_T map_t)
{
    return vxlanTunnelMapKeyVal.at(map_t).first;
}

static inline uint32_t tunnel_map_val (MAP_T map_t)
{
    return vxlanTunnelMapKeyVal.at(map_t).second;
}

static sai_object_id_t
create_tunnel_map(MAP_T map_t)
{
    sai_attribute_t attr;
    std::vector<sai_attribute_t> tunnel_map_attrs;

    if (map_t == MAP_T::MAP_TO_INVALID)
    {
        SWSS_LOG_ERROR("Invalid map type %d", map_t);
        return SAI_NULL_OBJECT_ID;
    }

    attr.id = SAI_TUNNEL_MAP_ATTR_TYPE;
    attr.value.s32 = tunnel_map_type(map_t);

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

sai_object_id_t create_tunnel_map_entry(
    MAP_T map_t,
    sai_object_id_t tunnel_map_id,
    sai_object_id_t obj_id,
    sai_uint32_t vni,
    bool encap)
{
    sai_attribute_t attr;
    sai_object_id_t tunnel_map_entry_id;
    std::vector<sai_attribute_t> tunnel_map_entry_attrs;

    attr.id = SAI_TUNNEL_MAP_ENTRY_ATTR_TUNNEL_MAP_TYPE;
    attr.value.s32 = tunnel_map_type(map_t);
    tunnel_map_entry_attrs.push_back(attr);

    attr.id = SAI_TUNNEL_MAP_ENTRY_ATTR_TUNNEL_MAP;
    attr.value.oid = tunnel_map_id;
    tunnel_map_entry_attrs.push_back(attr);

    attr.id = (encap)? tunnel_map_key(map_t):tunnel_map_val(map_t);
    attr.value.oid = obj_id;
    tunnel_map_entry_attrs.push_back(attr);

    attr.id = (encap)? tunnel_map_val(map_t):tunnel_map_key(map_t);
    attr.value.u32 = vni;
    tunnel_map_entry_attrs.push_back(attr);

    sai_tunnel_api->create_tunnel_map_entry(&tunnel_map_entry_id, gSwitchId,
                                            static_cast<uint32_t> (tunnel_map_entry_attrs.size()),
                                            tunnel_map_entry_attrs.data());

    return tunnel_map_entry_id;
}

sai_status_t create_nexthop_tunnel(
    sai_ip_address_t host_ip,
    sai_uint32_t vni, // optional vni
    sai_mac_t *mac, // inner destination mac
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

    if (mac != nullptr)
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
    MAP_T map_t,
    sai_object_id_t tunnel_map_id,
    sai_uint32_t vni,
    sai_uint16_t vlan_id)
{
    sai_attribute_t attr;
    std::vector<sai_attribute_t> tunnel_map_entry_attrs;

    attr.id = SAI_TUNNEL_MAP_ENTRY_ATTR_TUNNEL_MAP_TYPE;
    attr.value.s32 = tunnel_map_type(map_t);
    tunnel_map_entry_attrs.push_back(attr);

    attr.id = SAI_TUNNEL_MAP_ENTRY_ATTR_TUNNEL_MAP;
    attr.value.oid = tunnel_map_id;
    tunnel_map_entry_attrs.push_back(attr);

    attr.id = tunnel_map_key(map_t);
    attr.value.u32 = vni;
    tunnel_map_entry_attrs.push_back(attr);

    attr.id = tunnel_map_val(map_t);
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

    attr.id = SAI_TUNNEL_ATTR_UNDERLAY_INTERFACE;
    attr.value.oid = underlay_rif;
    tunnel_attrs.push_back(attr);

    sai_object_id_t decap_list[] = { tunnel_decap_id };
    attr.id = SAI_TUNNEL_ATTR_DECAP_MAPPERS;
    attr.value.objlist.count = 1;
    attr.value.objlist.list = decap_list;
    tunnel_attrs.push_back(attr);

    if (tunnel_encap_id != SAI_NULL_OBJECT_ID)
    {
        sai_object_id_t encap_list[] = { tunnel_encap_id };
        attr.id = SAI_TUNNEL_ATTR_ENCAP_MAPPERS;
        attr.value.objlist.count = 1;
        attr.value.objlist.list = encap_list;
        tunnel_attrs.push_back(attr);
    }

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
    sai_object_id_t tunnel_oid,
    sai_ip_address_t srcip,
    sai_ip_address_t *dstip,
    sai_object_id_t default_vrid)
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
    attr.value.ipaddr = srcip;
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

bool VxlanTunnel::createTunnel(MAP_T encap, MAP_T decap)
{
    try
    {
        sai_ip_address_t ips, ipd, *ip=nullptr;
        swss::copy(ips, src_ip_);

        ids_.tunnel_decap_id = SAI_NULL_OBJECT_ID;
        ids_.tunnel_encap_id = SAI_NULL_OBJECT_ID;

        if (decap != MAP_T::MAP_TO_INVALID)
        {
            ids_.tunnel_decap_id = create_tunnel_map(decap);
        }
        if (encap != MAP_T::MAP_TO_INVALID)
        {
            ids_.tunnel_encap_id = create_tunnel_map(encap);
            ip = &ips;
        }

        ids_.tunnel_id = create_tunnel(ids_.tunnel_encap_id, ids_.tunnel_decap_id, ip, gUnderlayIfId);

        ip = nullptr;
        if (!dst_ip_.isZero())
        {
            swss::copy(ipd, dst_ip_);
            ip = &ipd;
        }

        ids_.tunnel_term_id = create_tunnel_termination(ids_.tunnel_id, ips, ip, gVirtualRouterId);
        active_ = true;
        tunnel_map_ = { encap, decap };
    }
    catch (const std::runtime_error& error)
    {
        SWSS_LOG_ERROR("Error creating tunnel %s: %s", tunnel_name_.c_str(), error.what());
        // FIXME: add code to remove already created objects
        return false;
    }

    SWSS_LOG_INFO("Vxlan tunnel '%s' was created", tunnel_name_.c_str());
    return true;
}

sai_object_id_t VxlanTunnel::addEncapMapperEntry(sai_object_id_t obj, uint32_t vni)
{
    const auto encap_id = getEncapMapId();
    const auto map_t = tunnel_map_.first;
    return create_tunnel_map_entry(map_t, encap_id, obj, vni, true);
}

sai_object_id_t VxlanTunnel::addDecapMapperEntry(sai_object_id_t obj, uint32_t vni)
{
    const auto decap_id = getDecapMapId();
    const auto map_t = tunnel_map_.second;
    return create_tunnel_map_entry(map_t, decap_id, obj, vni, false);
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

    vxlan_tunnel_table_[tunnel_name] = std::unique_ptr<VxlanTunnel>(new VxlanTunnel(tunnel_name, src_ip, dst_ip));

    SWSS_LOG_INFO("Vxlan tunnel '%s' was added", tunnel_name.c_str());
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
        SWSS_LOG_WARN("Vxlan tunnel map vlan id doesn't exist: %d", vlan_id);
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
        SWSS_LOG_WARN("Vxlan tunnel '%s' doesn't exist", tunnel_name.c_str());
        return false;
    }

    auto tunnel_obj = tunnel_orch->getVxlanTunnel(tunnel_name);
    if (!tunnel_obj->isActive())
    {
        //@Todo, currently only decap mapper is allowed
        tunnel_obj->createTunnel(MAP_T::MAP_TO_INVALID, MAP_T::VNI_TO_VLAN_ID);
    }

    const auto full_tunnel_map_entry_name = request.getFullKey();
    if (isTunnelMapExists(full_tunnel_map_entry_name))
    {
        SWSS_LOG_NOTICE("Vxlan tunnel map '%s' already exist", full_tunnel_map_entry_name.c_str());
        return true;
    }

    const auto tunnel_map_id = tunnel_obj->getDecapMapId();
    const auto tunnel_map_entry_name = request.getKeyString(1);

    try
    {
        auto tunnel_map_entry_id = create_tunnel_map_entry(MAP_T::VNI_TO_VLAN_ID,
                                                           tunnel_map_id, vni_id, vlan_id);
        vxlan_tunnel_map_table_[full_tunnel_map_entry_name] = tunnel_map_entry_id;
    }
    catch(const std::runtime_error& error)
    {
        SWSS_LOG_ERROR("Error adding tunnel map entry. Tunnel: %s. Entry: %s. Error: %s",
            tunnel_name.c_str(), tunnel_map_entry_name.c_str(), error.what());
        return false;
    }

    SWSS_LOG_NOTICE("Vxlan tunnel map entry '%s' for tunnel '%s' was created",
                   tunnel_map_entry_name.c_str(), tunnel_name.c_str());

    return true;
}

bool VxlanTunnelMapOrch::delOperation(const Request& request)
{
    SWSS_LOG_ENTER();

    SWSS_LOG_ERROR("DEL operation is not implemented");

    return true;
}

sai_object_id_t
VxlanVrfMapOrch::createNextHopTunnel(const std::string& vName, IpAddress& ipAddr, MacAddress macAddress, uint32_t vni)
{
    if (vxlan_vrf_tunnel_.find(vName) == vxlan_vrf_tunnel_.end())
    {
        return SAI_NULL_OBJECT_ID;
    }

    sai_ip_address_t host_ip;
    swss::copy(host_ip, ipAddr);

    sai_mac_t mac, *macptr = nullptr;
    if (macAddress)
    {
        memcpy(mac, macAddress.getMac(), ETHER_ADDR_LEN);
        macptr = &mac;
    }

    sai_object_id_t nh_id, tunnel_id = vxlan_vrf_tunnel_[vName];

    if (create_nexthop_tunnel(host_ip, vni, macptr, tunnel_id, &nh_id) != SAI_STATUS_SUCCESS)
    {
        string err_msg = "NH tunnel create failed for " + ipAddr.to_string() + " " + to_string(vni);
        throw std::runtime_error(err_msg);
    }

    SWSS_LOG_INFO("NH vxlan tunnel was created for %s, id 0x%lx", vName.c_str(), nh_id);
    return nh_id;
}

void VxlanVrfMapOrch::vnetTaskHandler(string& vrf_name, VxlanTunnel* tunnel_obj, handler_pair& cap_obj)
{
    VNetOrch* vnet_orch = gDirectory.get<VNetOrch*>();
    if (!tunnel_obj->isActive())
    {
        if (vnet_orch->isVnetExecVrf())
        {
            tunnel_obj->createTunnel(MAP_T::VRID_TO_VNI, MAP_T::VNI_TO_VRID);
        }
        else
        {
            tunnel_obj->createTunnel(MAP_T::BRIDGE_TO_VNI, MAP_T::VNI_TO_BRIDGE);
        }
    }

    cap_obj.first = vnet_orch->getEncapMapId(vrf_name);
    cap_obj.second = vnet_orch->getDecapMapId(vrf_name);
}

bool VxlanVrfMapOrch::addOperation(const Request& request)
{
    SWSS_LOG_ENTER();

    auto tunnel_name = request.getKeyString(0);
    VxlanTunnelOrch* tunnel_orch = gDirectory.get<VxlanTunnelOrch*>();
    if (!tunnel_orch->isTunnelExists(tunnel_name))
    {
        SWSS_LOG_WARN("Vxlan tunnel '%s' doesn't exist", tunnel_name.c_str());
        return false;
    }

    auto vni_id  = static_cast<sai_uint32_t>(request.getAttrUint("vni"));
    if (vni_id >= 1<<24)
    {
        SWSS_LOG_ERROR("Vxlan vni id is too big: %d", vni_id);
        return true;
    }

    const auto full_map_entry_name = request.getFullKey();
    if (isVrfMapExists(full_map_entry_name))
    {
        SWSS_LOG_ERROR("Vxlan map '%s' is already exist", full_map_entry_name.c_str());
        return true;
    }

    auto tunnel_obj = tunnel_orch->getVxlanTunnel(tunnel_name);
    std::pair<sai_object_id_t, sai_object_id_t> cap_obj;

    string vrf_name = request.getAttrString("vrf");
    VNetOrch* vnet_orch = gDirectory.get<VNetOrch*>();
    VRFOrch* vrf_orch = gDirectory.get<VRFOrch*>();

    if (vnet_orch->isVnetExists(vrf_name))
    {
        vnetTaskHandler(vrf_name, tunnel_obj, cap_obj);
    }
    else if (vrf_orch->isVRFexists(vrf_name))
    {
        //Placeholder for "Do VRF task" (for non-vnet L3 Vxlan use-cases)
    }
    else
    {
        SWSS_LOG_WARN("VNet/Vrf '%s' hasn't been created yet", vrf_name.c_str());
        return false;
    }

    const auto tunnel_map_entry_name = request.getKeyString(1);
    vrf_map_entry_t entry;
    try
    {
        /*
         * Create encap and decap mapper
         */
        entry.encap_id = tunnel_obj->addEncapMapperEntry(cap_obj.first, vni_id);
        entry.decap_id = tunnel_obj->addDecapMapperEntry(cap_obj.second, vni_id);

        SWSS_LOG_DEBUG("Vxlan tunnel encap entry '%lx' decap entry '0x%lx'",
                entry.encap_id, entry.decap_id);

        vxlan_vrf_table_[full_map_entry_name] = entry;
        vxlan_vrf_tunnel_[vrf_name] = tunnel_obj->getTunnelId();
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
