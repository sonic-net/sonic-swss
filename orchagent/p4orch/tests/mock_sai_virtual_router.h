#pragma once

#include <gmock/gmock.h>

extern "C"
{
#include "sai.h"
}

// Mock Class mapping methods to router interface SAI APIs.
class MockSaiVirtualRouter
{
  public:
    MOCK_METHOD4(create_virtual_router,
                 sai_status_t(_Out_ sai_object_id_t *virtual_router_id, _In_ sai_object_id_t switch_id,
                              _In_ uint32_t attr_count, _In_ const sai_attribute_t *attr_list));

    MOCK_METHOD1(remove_virtual_router, sai_status_t(_In_ sai_object_id_t virtual_router_id));

    MOCK_METHOD2(set_virtual_router_attribute,
                 sai_status_t(_In_ sai_object_id_t virtual_router_id, _In_ const sai_attribute_t *attr));

    MOCK_METHOD3(get_virtual_router_attribute,
                 sai_status_t(_In_ sai_object_id_t virtual_router_id, _In_ uint32_t attr_count,
                              _Inout_ sai_attribute_t *attr_list));
};

MockSaiVirtualRouter *mock_sai_virtual_router;

sai_status_t create_virtual_router(_Out_ sai_object_id_t *virtual_router_id, _In_ sai_object_id_t switch_id,
                                   _In_ uint32_t attr_count, _In_ const sai_attribute_t *attr_list)
{
    return mock_sai_virtual_router->create_virtual_router(virtual_router_id, switch_id, attr_count, attr_list);
}

sai_status_t remove_virtual_router(_In_ sai_object_id_t virtual_router_id)
{
    return mock_sai_virtual_router->remove_virtual_router(virtual_router_id);
}

sai_status_t set_virtual_router_attribute(_In_ sai_object_id_t virtual_router_id, _In_ const sai_attribute_t *attr)
{
    return mock_sai_virtual_router->set_virtual_router_attribute(virtual_router_id, attr);
}

sai_status_t get_virtual_router_attribute(_In_ sai_object_id_t virtual_router_id, _In_ uint32_t attr_count,
                                          _Inout_ sai_attribute_t *attr_list)
{
    return mock_sai_virtual_router->get_virtual_router_attribute(virtual_router_id, attr_count, attr_list);
}
