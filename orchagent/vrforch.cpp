#include <cassert>
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <exception>

#include "sai.h"
#include "macaddress.h"
#include "orch.h"
#include "request_parser.h"
#include "vrforch.h"
#include "vxlanorch.h"
#include "flowcounterrouteorch.h"
#include "directory.h"

using namespace std;
using namespace swss;

extern sai_virtual_router_api_t* sai_virtual_router_api;
extern sai_object_id_t gSwitchId;

extern Directory<Orch*>      gDirectory;
extern PortsOrch*            gPortsOrch;
extern FlowCounterRouteOrch* gFlowCounterRouteOrch;

bool VRFOrch::addOperation(const Request& request)
{
    SWSS_LOG_ENTER();
    uint32_t vni = 0;
    bool error = true;

    sai_attribute_t attr;
    vector<sai_attribute_t> attrs;

    for (const auto& name: request.getAttrFieldNames())
    {
        if (name == "v4")
        {
            attr.id = SAI_VIRTUAL_ROUTER_ATTR_ADMIN_V4_STATE;
            attr.value.booldata = request.getAttrBool("v4");
        }
        else if (name == "v6")
        {
            attr.id = SAI_VIRTUAL_ROUTER_ATTR_ADMIN_V6_STATE;
            attr.value.booldata = request.getAttrBool("v6");
        }
        else if (name == "src_mac")
        {
            const auto& mac = request.getAttrMacAddress("src_mac");
            attr.id = SAI_VIRTUAL_ROUTER_ATTR_SRC_MAC_ADDRESS;
            memcpy(attr.value.mac, mac.getMac(), sizeof(sai_mac_t));
        }
        else if (name == "ttl_action")
        {
            attr.id = SAI_VIRTUAL_ROUTER_ATTR_VIOLATION_TTL1_PACKET_ACTION;
            attr.value.s32 = request.getAttrPacketAction("ttl_action");
        }
        else if (name == "ip_opt_action")
        {
            attr.id = SAI_VIRTUAL_ROUTER_ATTR_VIOLATION_IP_OPTIONS_PACKET_ACTION;
            attr.value.s32 = request.getAttrPacketAction("ip_opt_action");
        }
        else if (name == "l3_mc_action")
        {
            attr.id = SAI_VIRTUAL_ROUTER_ATTR_UNKNOWN_L3_MULTICAST_PACKET_ACTION;
            attr.value.s32 = request.getAttrPacketAction("l3_mc_action");
        }
        else if (name == "vni")
        {
            vni = static_cast<uint32_t>(request.getAttrUint(name));
            continue;
        }
        else if ((name == "mgmtVrfEnabled") || (name == "in_band_mgmt_enabled"))
        {
            SWSS_LOG_INFO("MGMT VRF field: %s ignored", name.c_str());
            continue;
        }
        else
        {
            SWSS_LOG_ERROR("Logic error: Unknown attribute: %s", name.c_str());
            continue;
        }
        attrs.push_back(attr);
    }

    const std::string& vrf_name = request.getKeyString(0);
    auto it = vrf_table_.find(vrf_name);
    if (it == std::end(vrf_table_))
    {
        // Create a new vrf
        sai_object_id_t router_id;
        sai_status_t status = sai_virtual_router_api->create_virtual_router(&router_id,
                                                                            gSwitchId,
                                                                            static_cast<uint32_t>(attrs.size()),
                                                                            attrs.data());
        if (status != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_ERROR("Failed to create virtual router name: %s, rv: %d", vrf_name.c_str(), status);
            task_process_status handle_status = handleSaiCreateStatus(SAI_API_VIRTUAL_ROUTER, status);
            if (handle_status != task_success)
            {
                return parseHandleSaiStatusFailure(handle_status);
            }
        }

        vrf_table_[vrf_name].vrf_id = router_id;
        vrf_table_[vrf_name].ref_count = 0;
        vrf_id_table_[router_id] = vrf_name;
        gFlowCounterRouteOrch->onAddVR(router_id);
        if (vni != 0)
        {
            SWSS_LOG_INFO("VRF '%s' vni %d add", vrf_name.c_str(), vni);
            error = updateVrfVNIMap(vrf_name, vni);
            if (error == false)
            {
                return false;
            }
        }
        m_stateVrfObjectTable.hset(vrf_name, "state", "ok");
        SWSS_LOG_NOTICE("VRF '%s' was added", vrf_name.c_str());
    }
    else
    {
        // Update an existing vrf

        sai_object_id_t router_id = it->second.vrf_id;

        for (const auto& attr: attrs)
        {
            sai_status_t status = sai_virtual_router_api->set_virtual_router_attribute(router_id, &attr);
            if (status != SAI_STATUS_SUCCESS)
            {
                SWSS_LOG_ERROR("Failed to update virtual router attribute. vrf name: %s, rv: %d", vrf_name.c_str(), status);
                task_process_status handle_status = handleSaiSetStatus(SAI_API_VIRTUAL_ROUTER, status);
                if (handle_status != task_success)
                {
                    return parseHandleSaiStatusFailure(handle_status);
                }
            }
        }

        SWSS_LOG_INFO("VRF '%s' vni %d modify", vrf_name.c_str(), vni);
        error = updateVrfVNIMap(vrf_name, vni);
        if (error == false)
        {
            return false;
        }

        SWSS_LOG_NOTICE("VRF '%s' was updated", vrf_name.c_str());
    }

    return true;
}

bool VRFOrch::delOperation(const Request& request)
{
    SWSS_LOG_ENTER();
    bool error = true;

    const std::string& vrf_name = request.getKeyString(0);
    if (vrf_table_.find(vrf_name) == std::end(vrf_table_))
    {
        SWSS_LOG_ERROR("VRF '%s' doesn't exist", vrf_name.c_str());
        return true;
    }

    if (vrf_table_[vrf_name].ref_count)
        return false;

    sai_object_id_t router_id = vrf_table_[vrf_name].vrf_id;
    sai_status_t status = sai_virtual_router_api->remove_virtual_router(router_id);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to remove virtual router name: %s, rv:%d", vrf_name.c_str(), status);
        task_process_status handle_status = handleSaiRemoveStatus(SAI_API_VIRTUAL_ROUTER, status);
        if (handle_status != task_success)
        {
            return parseHandleSaiStatusFailure(handle_status);
        }
    }

    gFlowCounterRouteOrch->onRemoveVR(router_id);

    vrf_table_.erase(vrf_name);
    vrf_id_table_.erase(router_id);
    error = delVrfVNIMap(vrf_name, 0);
    if (error == false)
    {
        return false;
    }
    m_stateVrfObjectTable.del(vrf_name);

    SWSS_LOG_NOTICE("VRF '%s' was removed", vrf_name.c_str());

    return true;
}

bool VRFOrch::updateVrfVNIMap(const std::string& vrf_name, uint32_t vni)
{
    SWSS_LOG_ENTER();
    uint32_t old_vni = 0;
    uint16_t vlan_id = 0;
    EvpnNvoOrch* evpn_orch = gDirectory.get<EvpnNvoOrch*>();
    VxlanTunnelOrch* tunnel_orch = gDirectory.get<VxlanTunnelOrch*>();
    bool error = true;

    old_vni = getVRFmappedVNI(vrf_name);
    SWSS_LOG_INFO("VRF '%s' vni %d old_vni %d", vrf_name.c_str(), vni, old_vni);

    if (old_vni != vni)
    {
        if (vni == 0)
        {
            error = delVrfVNIMap(vrf_name, old_vni);
            if (error == false)
            {
                return false;
            }
        } else {
            //update l3vni table, if vlan/vni is received later will be able to update L3VniStatus.
            l3vni_table_[vni].vlan_id = 0;
            l3vni_table_[vni].l3_vni = true;
            auto evpn_vtep_ptr = evpn_orch->getEVPNVtep();
            if(!evpn_vtep_ptr)
            {
                SWSS_LOG_NOTICE("updateVrfVNIMap unable to find EVPN VTEP");
                return false;
            }

            vrf_vni_map_table_[vrf_name] = vni;
            vlan_id = tunnel_orch->getVlanMappedToVni(vni);
            l3vni_table_[vni].vlan_id = vlan_id;
            SWSS_LOG_INFO("addL3VniStatus vni %d vlan %d", vni, vlan_id);
            if (vlan_id != 0)
            {
                /*call VE UP*/
                error = gPortsOrch->updateL3VniStatus(vlan_id, true);
                SWSS_LOG_INFO("addL3VniStatus vni %d vlan %d, status %d", vni, vlan_id, error);
            }
        }
        SWSS_LOG_INFO("VRF '%s' vni %d map update", vrf_name.c_str(), vni);
    }

    return true;
}

bool VRFOrch::delVrfVNIMap(const std::string& vrf_name, uint32_t vni)
{
    SWSS_LOG_ENTER();
    bool status = true;
    uint16_t vlan_id = 0;

    SWSS_LOG_INFO("VRF '%s' VNI %d map", vrf_name.c_str(), vni);
    if (vni == 0) {
        vni = getVRFmappedVNI(vrf_name);
    }

    if (vni != 0)
    {
        vlan_id = l3vni_table_[vni].vlan_id;
        SWSS_LOG_INFO("delL3VniStatus vni %d vlan %d", vni, vlan_id);
        if (vlan_id != 0)
        {
            /*call VE Down*/
            status = gPortsOrch->updateL3VniStatus(vlan_id, false);
            SWSS_LOG_INFO("delL3VniStatus vni %d vlan %d, status %d", vni, vlan_id, status);
        }
        l3vni_table_.erase(vni);
        vrf_vni_map_table_.erase(vrf_name);
    }

    SWSS_LOG_INFO("VRF '%s' VNI %d map removed", vrf_name.c_str(), vni);
    return true;
}

int VRFOrch::updateL3VniVlan(uint32_t vni, uint16_t vlan_id)
{
    bool status = true;
    l3vni_table_[vni].vlan_id = vlan_id;

    SWSS_LOG_INFO("updateL3VniStatus vni %d vlan %d", vni, vlan_id);
    /*call VE UP*/
    status = gPortsOrch->updateL3VniStatus(vlan_id, true);
    SWSS_LOG_INFO("updateL3VniStatus vni %d vlan %d, status %d", vni, vlan_id, status);

    return 0;
}
