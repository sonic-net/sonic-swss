#include "orchagent/p4orch/tests/mock_sai_router_interface.h"

MockSaiRouterInterface *mock_sai_router_intf;

sai_status_t mock_create_router_interface(_Out_ sai_object_id_t *router_interface_id, _In_ sai_object_id_t switch_id,
                                          _In_ uint32_t attr_count, _In_ const sai_attribute_t *attr_list)
{
    return mock_sai_router_intf->create_router_interface(router_interface_id, switch_id, attr_count, attr_list);
}

sai_status_t mock_remove_router_interface(_In_ sai_object_id_t router_interface_id)
{
    return mock_sai_router_intf->remove_router_interface(router_interface_id);
}

sai_status_t mock_set_router_interface_attribute(_In_ sai_object_id_t router_interface_id,
                                                 _In_ const sai_attribute_t *attr)
{
    return mock_sai_router_intf->set_router_interface_attribute(router_interface_id, attr);
}

sai_status_t mock_get_router_interface_attribute(_In_ sai_object_id_t router_interface_id, _In_ uint32_t attr_count,
                                                 _Inout_ sai_attribute_t *attr_list)
{
    return mock_sai_router_intf->get_router_interface_attribute(router_interface_id, attr_count, attr_list);
}