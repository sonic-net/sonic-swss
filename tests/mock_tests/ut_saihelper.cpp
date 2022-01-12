#include "ut_helper.h"
#include "mock_orchagent_main.h"

namespace ut_helper
{
    map<string, string> gProfileMap;
    map<string, string>::iterator gProfileIter;

    const char *profile_get_value(
        sai_switch_profile_id_t profile_id,
        const char *variable)
    {
        map<string, string>::const_iterator it = gProfileMap.find(variable);
        if (it == gProfileMap.end())
        {
            return NULL;
        }

        return it->second.c_str();
    }

    int profile_get_next_value(
        sai_switch_profile_id_t profile_id,
        const char **variable,
        const char **value)
    {
        if (value == NULL)
        {
            gProfileIter = gProfileMap.begin();
            return 0;
        }

        if (variable == NULL)
        {
            return -1;
        }

        if (gProfileIter == gProfileMap.end())
        {
            return -1;
        }

        *variable = gProfileIter->first.c_str();
        *value = gProfileIter->second.c_str();

        gProfileIter++;

        return 0;
    }

    sai_status_t initSaiApi(const std::map<std::string, std::string> &profile)
    {
        sai_service_method_table_t services = {
            profile_get_value,
            profile_get_next_value
        };

        gProfileMap = profile;

        auto status = sai_api_initialize(0, (sai_service_method_table_t *)&services);
        if (status != SAI_STATUS_SUCCESS)
        {
            return status;
        }

        sai_api_query(SAI_API_SWITCH, (void **)&sai_switch_api);
        sai_api_query(SAI_API_BRIDGE, (void **)&sai_bridge_api);
        sai_api_query(SAI_API_VIRTUAL_ROUTER, (void **)&sai_virtual_router_api);
        sai_api_query(SAI_API_PORT, (void **)&sai_port_api);
        sai_api_query(SAI_API_LAG, (void **)&sai_lag_api);
        sai_api_query(SAI_API_VLAN, (void **)&sai_vlan_api);
        sai_api_query(SAI_API_ROUTER_INTERFACE, (void **)&sai_router_intfs_api);
        sai_api_query(SAI_API_ROUTE, (void **)&sai_route_api);
        sai_api_query(SAI_API_NEIGHBOR, (void **)&sai_neighbor_api);
        sai_api_query(SAI_API_TUNNEL, (void **)&sai_tunnel_api);
        sai_api_query(SAI_API_NEXT_HOP, (void **)&sai_next_hop_api);
        sai_api_query(SAI_API_ACL, (void **)&sai_acl_api);
        sai_api_query(SAI_API_HOSTIF, (void **)&sai_hostif_api);
        sai_api_query(SAI_API_BUFFER, (void **)&sai_buffer_api);
        sai_api_query(SAI_API_QUEUE, (void **)&sai_queue_api);
        sai_api_query(SAI_API_MPLS, (void**)&sai_mpls_api);

        return SAI_STATUS_SUCCESS;
    }

    void uninitSaiApi()
    {
        sai_api_uninitialize();

        sai_switch_api = nullptr;
        sai_bridge_api = nullptr;
        sai_virtual_router_api = nullptr;
        sai_port_api = nullptr;
        sai_lag_api = nullptr;
        sai_vlan_api = nullptr;
        sai_router_intfs_api = nullptr;
        sai_route_api = nullptr;
        sai_neighbor_api = nullptr;
        sai_tunnel_api = nullptr;
        sai_next_hop_api = nullptr;
        sai_acl_api = nullptr;
        sai_hostif_api = nullptr;
        sai_buffer_api = nullptr;
        sai_queue_api = nullptr;
    }

    map<string, vector<FieldValueTuple>> getInitialSaiPorts()
    {
        vector<sai_object_id_t> port_list;
        sai_attribute_t attr;
        sai_status_t status;
        uint32_t port_count;

        attr.id = SAI_SWITCH_ATTR_PORT_NUMBER;
        status = sai_switch_api->get_switch_attribute(gSwitchId, 1, &attr);
        if (status != SAI_STATUS_SUCCESS)
        {
            throw std::runtime_error("failed to get port count");
        }
        port_count = attr.value.u32;

        port_list.resize(port_count);
        attr.id = SAI_SWITCH_ATTR_PORT_LIST;
        attr.value.objlist.count = port_count;
        attr.value.objlist.list = port_list.data();
        status = sai_switch_api->get_switch_attribute(gSwitchId, 1, &attr);
        if (status != SAI_STATUS_SUCCESS || attr.value.objlist.count != port_count)
        {
            throw std::runtime_error("failed to get port list");
        }

        std::map<std::string, vector<FieldValueTuple>> ports;
        for (uint32_t i = 0; i < port_count; ++i)
        {
            string lanes_str = "";
            sai_uint32_t lanes[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
            attr.id = SAI_PORT_ATTR_HW_LANE_LIST;
            attr.value.u32list.count = 8;
            attr.value.u32list.list = lanes;
            status = sai_port_api->get_port_attribute(port_list[i], 1, &attr);
            if (status != SAI_STATUS_SUCCESS)
            {
                throw std::runtime_error("failed to get hw lane list");
            }

            for (uint32_t j = 0; j < attr.value.u32list.count; ++j)
            {
                lanes_str += (j == 0) ? "" : ",";
                lanes_str += to_string(attr.value.u32list.list[j]);
            }

            vector<FieldValueTuple> fvs = {
                { "lanes", lanes_str },
                { "speed", "1000" },
                { "mtu", "6000" },
                { "admin_status", "up" }
            };

            auto key = FRONT_PANEL_PORT_PREFIX + to_string(i);

            ports[key] = fvs;
        }

        return ports;
    }
}
