#pragma once

#include <gmock/gmock.h>

extern "C"
{
#include "sai.h"
}

// Mock Class mapping methods to router interface SAI APIs.
class MockSaiRouterInterface
{
  public:
    MOCK_METHOD4(create_router_interface,
                 sai_status_t(_Out_ sai_object_id_t *router_interface_id, _In_ sai_object_id_t switch_id,
                              _In_ uint32_t attr_count, _In_ const sai_attribute_t *attr_list));

    MOCK_METHOD1(remove_router_interface, sai_status_t(_In_ sai_object_id_t router_interface_id));

    MOCK_METHOD2(set_router_interface_attribute,
                 sai_status_t(_In_ sai_object_id_t router_interface_id, _In_ const sai_attribute_t *attr));

    MOCK_METHOD3(get_router_interface_attribute,
                 sai_status_t(_In_ sai_object_id_t router_interface_id, _In_ uint32_t attr_count,
                              _Inout_ sai_attribute_t *attr_list));
};

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
