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

    MOCK_METHOD7(create_tunnels,
                 sai_status_t(_In_ sai_object_id_t switch_id,
                              _In_ uint32_t object_count,
                              _In_ const uint32_t *attr_count,
                              _In_ const sai_attribute_t **attr_list,
                              _In_ sai_bulk_op_error_mode_t mode,
                              _Out_ sai_object_id_t *object_id,
                              _Out_ sai_status_t *object_statuses));

    MOCK_METHOD4(remove_tunnels,
                 sai_status_t(_In_ uint32_t object_count,
                              _In_ const sai_object_id_t *object_id,
                              _In_ sai_bulk_op_error_mode_t mode,
                              _Out_ sai_status_t *object_statuses));
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

sai_status_t mock_create_tunnels(_In_ sai_object_id_t switch_id,
                                 _In_ uint32_t object_count,
                                 _In_ const uint32_t *attr_count,
                                 _In_ const sai_attribute_t **attr_list,
                                 _In_ sai_bulk_op_error_mode_t mode,
                                 _Out_ sai_object_id_t *object_id,
                                 _Out_ sai_status_t *object_statuses) {
    return mock_sai_tunnel->create_tunnels(switch_id, object_count, attr_count,
                                           attr_list, mode, object_id,
                                           object_statuses);
}

sai_status_t mock_remove_tunnels(_In_ uint32_t object_count,
                                 _In_ const sai_object_id_t *object_id,
                                 _In_ sai_bulk_op_error_mode_t mode,
                                 _Out_ sai_status_t *object_statuses) {
    return mock_sai_tunnel->remove_tunnels(object_count, object_id, mode,
                                           object_statuses);
}
