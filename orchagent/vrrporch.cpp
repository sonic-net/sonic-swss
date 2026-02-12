#include "sai.h"
#include "macaddress.h"
#include "orch.h"
#include "request_parser.h"
#include "portsorch.h"
#include "port.h"
#include "swssnet.h"
#include "fdborch.h"
#include "intfsorch.h"
#include "vrrporch.h"

extern sai_router_interface_api_t*  sai_router_intfs_api;
extern PortsOrch *gPortsOrch;
extern sai_object_id_t gSwitchId;
extern sai_route_api_t* sai_route_api;
extern FdbOrch *gFdbOrch;
extern IntfsOrch *gIntfsOrch;
bool VrrpOrch::hasSameIpAddr(const string &alias,const IpPrefix &vip_prefix)
{
    IntfsTable m_syncdIntfses = gIntfsOrch->getSyncdIntfses();
    for (const auto &intfPrefix: m_syncdIntfses[alias].ip_addresses)
    {
        if (intfPrefix.getIp() == vip_prefix.getIp())
        {
                SWSS_LOG_NOTICE("vrrp hasSameIpAddr configured %s vip %s",alias.c_str(),vip_prefix.to_string().c_str());
                return true;
        }
    }
    return false;
}
bool VrrpOrch::addOperation(const Request& request)
{
    SWSS_LOG_ENTER();
    sai_attribute_t attr;
    vector<sai_attribute_t> vmac_attrs;
    vector<sai_attribute_t> vip_attrs;
    Port port;
    MacAddress mac;
    sai_object_id_t vrrp_rif_id;
    sai_object_id_t port_oid;
    for (const auto& name: request.getAttrFieldNames())
    {
        if (name == "vmac")
        {
            mac = request.getAttrMacAddress("vmac");
            attr.id = SAI_ROUTER_INTERFACE_ATTR_SRC_MAC_ADDRESS;
            memcpy(attr.value.mac, mac.getMac(), sizeof(sai_mac_t));
            SWSS_LOG_INFO("vrrp orch add : vmac %s ",mac.to_string().c_str());
            vmac_attrs.push_back(attr);
        }
    }
    auto ip_pfx = request.getKeyIpPrefix(1);
    /* Check if the vmac,vip combination is already programmed on the port.If yes, skip & return*/
    auto key = vrrp_key_t(request.getKeyString(0),request.getKeyIpPrefix(1));
    auto it = vrrp_table_.find(key);
    if (it != vrrp_table_.end())
    {
       if (vrrp_table_[key].vmac == request.getAttrMacAddress("vmac"))
        {
            SWSS_LOG_ERROR("_vrrp_table entry already exists, with vmac %s, vip %s for port %s",mac.to_string().c_str(),
                ip_pfx.to_string().c_str(),request.getKeyString(0).c_str());
            return true;
        }
    }
    gPortsOrch->getPort(request.getKeyString(0), port);
    attr.id = SAI_ROUTER_INTERFACE_ATTR_TYPE;
    switch(port.m_type)
    {
        case Port::PHY:
        case Port::LAG:
            attr.value.s32 = SAI_ROUTER_INTERFACE_TYPE_PORT;
            break;
        case Port::VLAN:
            attr.value.s32 = SAI_ROUTER_INTERFACE_TYPE_VLAN;
            break;
        case Port::SUBPORT:
            attr.value.s32 = SAI_ROUTER_INTERFACE_TYPE_SUB_PORT;
            break;			
        default:
            SWSS_LOG_ERROR("Unsupported port type: %d", port.m_type);
            break;
    }
    vmac_attrs.push_back(attr);
    switch(port.m_type)
    {
        case Port::PHY:
            attr.id = SAI_ROUTER_INTERFACE_ATTR_PORT_ID;
            attr.value.oid = port.m_port_id;
            break;
        case Port::LAG:
            attr.id = SAI_ROUTER_INTERFACE_ATTR_PORT_ID;
            attr.value.oid = port.m_lag_id;
            break;
        case Port::VLAN:
            attr.id = SAI_ROUTER_INTERFACE_ATTR_VLAN_ID;
            attr.value.oid = port.m_vlan_info.vlan_oid;
            break;
        case Port::SUBPORT:
            attr.id = SAI_ROUTER_INTERFACE_ATTR_OUTER_VLAN_ID;
            attr.value.u16 = port.m_vlan_info.vlan_id;
            vmac_attrs.push_back(attr);
            attr.id = SAI_ROUTER_INTERFACE_ATTR_PORT_ID;
            attr.value.oid = port.m_parent_port_id;
            break;			
        default:
            SWSS_LOG_ERROR("Unsupported port type: %d", port.m_type);
            break;
    }
    port_oid = attr.value.oid;
    vmac_attrs.push_back(attr);
    SWSS_LOG_NOTICE("vrrp orch add : port %s, ip %s, vmac %s",request.getKeyString(0).c_str(),ip_pfx.to_string().c_str(),mac.to_string().c_str());
    sai_route_entry_t unicast_route_entry;
    unicast_route_entry.switch_id = gSwitchId;
    unicast_route_entry.vr_id = port.m_vr_id;
    copy(unicast_route_entry.destination, ip_pfx.getIp());
    subnet(unicast_route_entry.destination, unicast_route_entry.destination);
    attr.id = SAI_ROUTER_INTERFACE_ATTR_VIRTUAL_ROUTER_ID;
    attr.value.oid = gVirtualRouterId;
    vmac_attrs.push_back(attr);
    attr.id = SAI_ROUTER_INTERFACE_ATTR_IS_VIRTUAL;
    attr.value.booldata =  true;
    vmac_attrs.push_back(attr);
    //program VRRPMAC.
    SWSS_LOG_INFO("vrrp orch add, before create_router_interface : port name %s, 0x%" PRIx64 " &rif_id %p",request.getKeyString(0).c_str(),vrrp_rif_id,&vrrp_rif_id);
    sai_status_t vmac_status = sai_router_intfs_api->create_router_interface(&vrrp_rif_id, gSwitchId, (uint32_t)vmac_attrs.size(), vmac_attrs.data());
    SWSS_LOG_NOTICE("vrrp orch add, after create_router_interface : port name %s, rif_id 0x%" PRIx64, request.getKeyString(0).c_str(),vrrp_rif_id);
    if (vmac_status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to program Vrrp Mac on interface %s, rv:%d",
                 port.m_alias.c_str(), vmac_status);
        throw runtime_error("Failed to program Vrrp Mac on interface.");
    }
    //program VIP
    if (!hasSameIpAddr (request.getKeyString(0),request.getKeyIpPrefix(1)))
    {
        attr.id = SAI_ROUTE_ENTRY_ATTR_PACKET_ACTION;
        attr.value.s32 = SAI_PACKET_ACTION_FORWARD;
        vip_attrs.push_back(attr);
        attr.id = SAI_ROUTE_ENTRY_ATTR_NEXT_HOP_ID;
        attr.value.oid = vrrp_rif_id;
        vip_attrs.push_back(attr);
        //API to program VRRP ipaddress
        sai_status_t vip_status = sai_route_api->create_route_entry(&unicast_route_entry, (uint32_t)vip_attrs.size(), vip_attrs.data());
        if (vip_status != SAI_STATUS_SUCCESS)
        {
            sai_status_t rif_status = sai_router_intfs_api->remove_router_interface (vrrp_rif_id);
            if (rif_status != SAI_STATUS_SUCCESS)
            {
                SWSS_LOG_ERROR("Failed to delete rif 0x%" PRIx64 " added for Vrrp Mac on interface %s, rv:%d",
                        vrrp_rif_id, port.m_alias.c_str(), rif_status);
            }
            SWSS_LOG_ERROR("Failed to program Vrrp IP on interface %s, rv:%d",
                      port.m_alias.c_str(), vip_status);
            throw runtime_error("Failed to program Vrrp Ip on interface.");
        }
    }
    key = vrrp_key_t(request.getKeyString(0),request.getKeyIpPrefix(1));
    it = vrrp_table_.find(key);
    if (it == vrrp_table_.end())
    {
        vrrp_table_[key] = {request.getAttrMacAddress("vmac"),vrrp_rif_id};
        SWSS_LOG_NOTICE("_vrrp_table add port %s, ip %s, vmac %s, rif_id 0x%" PRIx64, request.getKeyString(0).c_str(),
                                ip_pfx.to_string().c_str(),mac.to_string().c_str(),vrrp_rif_id);
    }
    else
    {
        SWSS_LOG_NOTICE("_vrrp_table entry already exists with vmac %s, rif_id 0x%" PRIx64 " update vmac to %s, rifid to 0x%" PRIx64,
                vrrp_table_[key].vmac.to_string().c_str(),vrrp_table_[key].rifid,
                request.getAttrMacAddress("vmac").to_string().c_str(),vrrp_rif_id);
        vrrp_table_.erase(key);
        vrrp_table_[key] = {request.getAttrMacAddress("vmac"),vrrp_rif_id};
    }
    //Flush the FDB entry for vmac on vrrp master
    FdbEntry entry;
    entry.mac = request.getAttrMacAddress("vmac");
    entry.bv_id = port_oid;
    gFdbOrch->removeFdbEntry(entry, FDB_ORIGIN_LEARN);
    return true;
}
bool VrrpOrch::delOperation(const Request& request)
{
    SWSS_LOG_ENTER();
    Port port;
    sai_object_id_t port_oid = 0;
    auto key = vrrp_key_t(request.getKeyString(0),request.getKeyIpPrefix(1));
    auto it = vrrp_table_.find(key);
    if (it == vrrp_table_.end())
    {
        SWSS_LOG_ERROR("VRRP entry for port %s, vip %s doesn't exist", request.getKeyString(0).c_str(),request.getKeyIpPrefix(1).to_string().c_str());
        return true;
    }
    gPortsOrch->getPort(request.getKeyString(0), port);
    auto ip_pfx = request.getKeyIpPrefix(1);
    sai_route_entry_t unicast_route_entry;
    unicast_route_entry.switch_id = gSwitchId;
    unicast_route_entry.vr_id = port.m_vr_id;
    copy(unicast_route_entry.destination, ip_pfx.getIp());
    switch(port.m_type)
    {
        case Port::PHY:
            port_oid = port.m_port_id;
            break;
        case Port::LAG:
            port_oid = port.m_lag_id;
            break;
        case Port::VLAN:
            port_oid = port.m_vlan_info.vlan_oid;
            break;
        case Port::SUBPORT:
            port_oid = port.m_parent_port_id;
            break;			
        default:
            SWSS_LOG_ERROR("Unsupported port type: %d", port.m_type);
            break;
    }
    //Flush the FDB entry for vmac
    FdbEntry entry;
    entry.mac = vrrp_table_[key].vmac;
    entry.bv_id = port_oid;
    gFdbOrch->removeFdbEntry(entry, FDB_ORIGIN_LEARN);
    //If same vip is configured as DIP on the port, then skip delete vip as it will delete the DIP on the port.
    if (!hasSameIpAddr (request.getKeyString(0), request.getKeyIpPrefix(1)))
    {
        sai_status_t vip_status = sai_route_api->remove_route_entry(&unicast_route_entry);
        if (vip_status != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_ERROR("Failed to delete Vrrp Vip on interface %s, rv:%d",
                port.m_alias.c_str(), vip_status);
            throw runtime_error("Failed to remove Vrrp Vip on interface");
        }
    }
  
    sai_status_t vmac_status = sai_router_intfs_api->remove_router_interface (vrrp_table_[key].rifid);
    if (vmac_status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to delete Vrrp Mac on interface %s, rv:%d",
                 port.m_alias.c_str(), vmac_status);
        throw runtime_error("Failed to remove Vrrp Mac on interface.");
    }
    SWSS_LOG_NOTICE("vrrp orch del success,port %s vip %s vmac %s rifid 0x%" PRIx64, request.getKeyString(0).c_str(),
        ip_pfx.to_string().c_str(),vrrp_table_[key].vmac.to_string().c_str(),vrrp_table_[key].rifid);
    vrrp_table_.erase(key);
    return true;
}
