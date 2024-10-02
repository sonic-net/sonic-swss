#pragma once

#include <gmock/gmock.h>

extern "C"
{
#include "sai.h"
}

// Mock Class mapping methods to tunnel object SAI APIs.
class MockSaiTunnel
{
  public:
    MOCK_METHOD4(create_tunnel, sai_status_t(_Out_ sai_object_id_t *tunnel_id, _In_ sai_object_id_t switch_id,
                                             _In_ uint32_t attr_count, _In_ const sai_attribute_t *attr_list));

    MOCK_METHOD1(remove_tunnel, sai_status_t(_In_ sai_object_id_t tunnel_id));
};

MockSaiTunnel *mock_sai_tunnel;

sai_status_t mock_create_tunnel(_Out_ sai_object_id_t *tunnel_id, _In_ sai_object_id_t switch_id,
                                _In_ uint32_t attr_count, _In_ const sai_attribute_t *attr_list)
{
    return mock_sai_tunnel->create_tunnel(tunnel_id, switch_id, attr_count, attr_list);
}

sai_status_t mock_remove_tunnel(_In_ sai_object_id_t tunnel_id)
{
    return mock_sai_tunnel->remove_tunnel(tunnel_id);
}
