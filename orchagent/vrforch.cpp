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

extern sai_virtual_router_api_t* sai_virtual_router_api;
extern sai_object_id_t gSwitchId;

VRFOrch::VRFOrch(DBConnector *db, const std::string& tableName) : Orch(db, tableName)
{
}

// FIXME:: better to make it part of orch.h
//         and implement only addOperation and deleteOperation
void VRFOrch::doTask(Consumer &consumer)
{
    SWSS_LOG_ENTER();

    VRFRequest request;

    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
        bool erase_from_queue = true;
        try
        {
            request.parse(it->second);

            auto op = request.getOperation();
            if (op == SET_COMMAND)
            {
                erase_from_queue = addOperation(request);
            }
            else if (op == DEL_COMMAND)
            {
                erase_from_queue = delOperation(request);
            }
            else
            {
                SWSS_LOG_ERROR("Wrong operation. Check RequestParser: %s", op.c_str());
            }
        }
        catch (const std::invalid_argument& e)
        {
            SWSS_LOG_ERROR("Parse error: %s", e.what());
        }
        catch (const std::logic_error& e)
        {
            SWSS_LOG_ERROR("Logic error: %s", e.what());
        }
        catch (const std::exception& e)
        {
            SWSS_LOG_ERROR("Exception was catched in the request parser: %s", e.what());
        }
        catch (...)
        {
            SWSS_LOG_ERROR("Unknown exception was catched in the request parser");
        }
        request.clear();

        if (erase_from_queue)
        {
            it = consumer.m_toSync.erase(it);
        }
        else
        {
            ++it;
        }
    }
}


bool VRFOrch::addOperation(const VRFRequest& request)
{
    SWSS_LOG_ENTER();

    sai_attribute_t attr;
    vector<sai_attribute_t> attrs;

    const std::string& vrf_name = request.getKeyString(0);
    if (vrf_table_.find(vrf_name) != std::end(vrf_table_))
    {
        SWSS_LOG_ERROR("VRF '%s' already exists", vrf_name.c_str());
        return true;
    }

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

    sai_object_id_t router_id;
    sai_status_t status = sai_virtual_router_api->create_virtual_router(&router_id,
                                                                        gSwitchId,
                                                                        static_cast<uint32_t>(attrs.size()),
                                                                        attrs.data());
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to create virtual router name: %s, rv:%d", vrf_name.c_str(), status);
        return false;
    }

    vrf_table_[vrf_name] = router_id;

    return true;
}

bool VRFOrch::delOperation(const VRFRequest& request)
{
    SWSS_LOG_ENTER();

    const std::string& vrf_name = request.getKeyString(0);
    if (vrf_table_.find(vrf_name) == std::end(vrf_table_))
    {
        SWSS_LOG_ERROR("VRF '%s' doesn't exist", vrf_name.c_str());
        return true;
    }

    sai_object_id_t router_id = vrf_table_[vrf_name];
    sai_status_t status = sai_virtual_router_api->remove_virtual_router(router_id);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to remove virtual router name: %s, rv:%d", vrf_name.c_str(), status);
        return false;
    }

    vrf_table_.erase(vrf_name);

    return true;
}
