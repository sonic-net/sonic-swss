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
#include "vxlan_tunnel_decap_orch.h"

/* Global variables */
extern sai_object_id_t gSwitchId;
extern sai_object_id_t gVirtualRouterId;
extern sai_tunnel_api_t *sai_tunnel_api;

static sai_object_id_t
create_decap_tunnel_map()
{
    sai_attribute_t attr;
    std::vector<sai_attribute_t> tunnel_map_attrs;

    attr.id = SAI_TUNNEL_MAP_ATTR_TYPE;
    attr.value.s32 = SAI_TUNNEL_MAP_TYPE_VNI_TO_VLAN_ID;
    tunnel_map_attrs.push_back(attr);

    sai_object_id_t tunnel_map_id;
    sai_status_t status = sai_tunnel_api->create_tunnel_map(
                                &tunnel_map_id,
                                gSwitchId,
                                static_cast<uint32_t>(tunnel_map_attrs.size()),
                                tunnel_map_attrs.data()
                          );
    if (status != SAI_STATUS_SUCCESS) throw std::runtime_error("Can't create tunnel map object");

    return tunnel_map_id;
}

static sai_object_id_t
create_decap_tunnel_map_entry(
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
    if (status != SAI_STATUS_SUCCESS) throw std::runtime_error("Can't create a tunnel map entry object");

    return tunnel_map_entry_id;
}

// Create Tunnel
static sai_object_id_t
create_tunnel(sai_object_id_t tunnel_decap_map_id)
{
    sai_attribute_t attr;
    std::vector<sai_attribute_t> tunnel_attrs;

    attr.id = SAI_TUNNEL_ATTR_TYPE;
    attr.value.s32 = SAI_TUNNEL_TYPE_VXLAN;
    tunnel_attrs.push_back(attr);

    sai_object_id_t decap_list[] = { tunnel_decap_map_id };
    attr.id = SAI_TUNNEL_ATTR_DECAP_MAPPERS;
    attr.value.objlist.count = 1;
    attr.value.objlist.list = decap_list;
    tunnel_attrs.push_back(attr);

    sai_object_id_t tunnel_id;
    sai_status_t status = sai_tunnel_api->create_tunnel(
                                &tunnel_id,
                                gSwitchId,
                                static_cast<uint32_t>(tunnel_attrs.size()),
                                tunnel_attrs.data()
                          );
    if (status != SAI_STATUS_SUCCESS) throw std::runtime_error("Can't create a tunnel object");

    return tunnel_id;
}

// Create tunnel termination

static sai_object_id_t
create_tunnel_termination(
    sai_object_id_t tunnel_oid,
    sai_ip4_t dstip,
    sai_object_id_t default_vrid)
{
    sai_attribute_t attr;
    std::vector<sai_attribute_t> tunnel_attrs;

    attr.id = SAI_TUNNEL_TERM_TABLE_ENTRY_ATTR_TYPE;
    attr.value.oid = SAI_TUNNEL_TERM_TABLE_ENTRY_TYPE_P2MP;
    tunnel_attrs.push_back(attr);

    attr.id = SAI_TUNNEL_TERM_TABLE_ENTRY_ATTR_VR_ID;
    attr.value.oid = default_vrid;
    tunnel_attrs.push_back(attr);

    attr.id = SAI_TUNNEL_TERM_TABLE_ENTRY_ATTR_DST_IP;
    attr.value.ipaddr.addr_family = SAI_IP_ADDR_FAMILY_IPV4;
    attr.value.ipaddr.addr.ip4 = dstip;
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
    if (status != SAI_STATUS_SUCCESS) throw std::runtime_error("Can't create a tunnel term table object");

    return term_table_id;
}

bool VxlanTunnelDecapOrch::addOperation(const Request& request)
{
    SWSS_LOG_ENTER();

    uint16_t vlan_id = 0; // initialize for a compiler

    for (const auto& name: request.getAttrFieldNames())
    {
        if (name == "vlan")
        {
            vlan_id = request.getAttrVlan("vlan");
        }
        else
        {
            SWSS_LOG_ERROR("Logic error: Unknown attribute: %s", name.c_str());
            continue;
        }
    }

    const auto& termination_ipv4 = request.getKeyIpAddress(0);
    if (!termination_ipv4.isV4())
    {
        SWSS_LOG_ERROR("Wrong key: %s. It should contain IPv4 address", request.getFullKey().c_str());
        return false;
    }

    const auto& termination_vni  = static_cast<uint32_t>(request.getKeyUINT(1));

    const auto vxlan_tunnel_tuple = std::make_tuple(termination_ipv4, termination_vni);

    if(!isTunnelExists(vxlan_tunnel_tuple))
    {
        try
        {
            sai_object_id_t tunnel_decap_map_id = create_decap_tunnel_map();
            (void)create_decap_tunnel_map_entry(tunnel_decap_map_id, termination_vni, vlan_id);
            sai_object_id_t tunnel_id = create_tunnel(tunnel_decap_map_id);
            sai_object_id_t term_table_id = create_tunnel_termination(tunnel_id, termination_ipv4.getV4Addr(), gVirtualRouterId);
            vxlan_tunnel_decap_table_[vxlan_tunnel_tuple] = term_table_id;
            SWSS_LOG_NOTICE("vxlan Tunnel '%s:%u' was added", termination_ipv4.to_string().c_str(), termination_vni);
        }
        catch(const std::runtime_error& error)
        {
            SWSS_LOG_ERROR("Failed to create a vxlan tunnel '%s:%u'. Error: %s",
                termination_ipv4.to_string().c_str(), termination_vni, error.what());
        }
    }
    else
    {
        SWSS_LOG_ERROR("vxlan tunnel '%s:%u' already exists",
            termination_ipv4.to_string().c_str(), termination_vni);
    }

    return true;
}

bool VxlanTunnelDecapOrch::delOperation(const Request& request)
{
    SWSS_LOG_ENTER();

    SWSS_LOG_ERROR("DEL operation is not implemented");

    return true;
}
