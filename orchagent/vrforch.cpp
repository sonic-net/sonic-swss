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
#include "routeorch.h"

using namespace std;
using namespace swss;

extern sai_virtual_router_api_t* sai_virtual_router_api;
extern sai_object_id_t gSwitchId;
extern RouteOrch *gRouteOrch;

bool VRFOrch::addOperation(const Request& request)
{
    SWSS_LOG_ENTER();

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
            return false;
        }

        vrf_table_[vrf_name].vrf_id = router_id;
        vrf_table_[vrf_name].ref_count = 0;
        vrf_id_table_[router_id] = vrf_name;
        m_stateVrfObjectTable.hset(vrf_name, "state", "ok");
        SWSS_LOG_NOTICE("VRF '%s' was added", vrf_name.c_str());
        gRouteOrch->vrfDefaultRoute(router_id, SET_COMMAND);
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
                return false;
            }
        }

        SWSS_LOG_NOTICE("VRF '%s' was updated", vrf_name.c_str());
    }

    return true;
}

bool VRFOrch::delOperation(const Request& request)
{
    SWSS_LOG_ENTER();

    const std::string& vrf_name = request.getKeyString(0);
    if (vrf_table_.find(vrf_name) == std::end(vrf_table_))
    {
        SWSS_LOG_ERROR("VRF '%s' doesn't exist", vrf_name.c_str());
        return true;
    }

    if (vrf_table_[vrf_name].ref_count)
        return false;

    sai_object_id_t router_id = vrf_table_[vrf_name].vrf_id;
    gRouteOrch->vrfDefaultRoute(router_id, DEL_COMMAND);
    sai_status_t status = sai_virtual_router_api->remove_virtual_router(router_id);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to remove virtual router name: %s, rv:%d", vrf_name.c_str(), status);
        return false;
    }

    vrf_table_.erase(vrf_name);
    vrf_id_table_.erase(router_id);
    m_stateVrfObjectTable.del(vrf_name);

    SWSS_LOG_NOTICE("VRF '%s' was removed", vrf_name.c_str());

    return true;
}
