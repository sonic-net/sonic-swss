// Define classes and functions to mock SAI bridge functions.
#pragma once

#include <gmock/gmock.h>

extern "C"
{
#include "sai.h"
}

// Mock class including mock functions mapping to SAI bridge functions.
class MockSaiBridge
{
  public:
    MOCK_METHOD4(create_bridge_port, sai_status_t(sai_object_id_t *bridge_port_id,
                                                  sai_object_id_t switch_id,
                                                  uint32_t attr_count,
                                                  const sai_attribute_t *attr_list));
};

// Note that before mock functions below are used, mock_sai_bridge must be
// initialized to point to an instance of MockSaiBridge.
MockSaiBridge *mock_sai_bridge;

sai_status_t mock_create_bridge_port(sai_object_id_t *bridge_port_id,
                                     sai_object_id_t switch_id,
                                     uint32_t attr_count,
                                     const sai_attribute_t *attr_list)
{
    return mock_sai_bridge->create_bridge_port(bridge_port_id, switch_id, attr_count, attr_list);
}



