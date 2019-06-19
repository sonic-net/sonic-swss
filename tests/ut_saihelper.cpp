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
    }
}
