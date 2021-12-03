// Define classes and functions to mock SAI next hop functions.
#pragma once

#include <gmock/gmock.h>

extern "C"
{
#include "sai.h"
}

// Mock class including mock functions mapping to SAI next hop's functions.
class MockSaiNextHop
{
  public:
    MOCK_METHOD4(create_next_hop, sai_status_t(_Out_ sai_object_id_t *next_hop_id, _In_ sai_object_id_t switch_id,
                                               _In_ uint32_t attr_count, _In_ const sai_attribute_t *attr_list));

    MOCK_METHOD1(remove_next_hop, sai_status_t(_In_ sai_object_id_t next_hop_id));

    MOCK_METHOD2(set_next_hop_attribute,
                 sai_status_t(_In_ sai_object_id_t next_hop_id, _In_ const sai_attribute_t *attr));

    MOCK_METHOD3(get_next_hop_attribute, sai_status_t(_In_ sai_object_id_t next_hop_id, _In_ uint32_t attr_count,
                                                      _Inout_ sai_attribute_t *attr_list));
};

// Note that before mock functions below are used, mock_sai_next_hop must be
// initialized to point to an instance of MockSaiNextHop.
MockSaiNextHop *mock_sai_next_hop;

sai_status_t mock_create_next_hop(_Out_ sai_object_id_t *next_hop_id, _In_ sai_object_id_t switch_id,
                                  _In_ uint32_t attr_count, _In_ const sai_attribute_t *attr_list)
{
    return mock_sai_next_hop->create_next_hop(next_hop_id, switch_id, attr_count, attr_list);
}

sai_status_t mock_remove_next_hop(_In_ sai_object_id_t next_hop_id)
{
    return mock_sai_next_hop->remove_next_hop(next_hop_id);
}

sai_status_t mock_set_next_hop_attribute(_In_ sai_object_id_t next_hop_id, _In_ const sai_attribute_t *attr)
{
    return mock_sai_next_hop->set_next_hop_attribute(next_hop_id, attr);
}

sai_status_t mock_get_next_hop_attribute(_In_ sai_object_id_t next_hop_id, _In_ uint32_t attr_count,
                                         _Inout_ sai_attribute_t *attr_list)
{
    return mock_sai_next_hop->get_next_hop_attribute(next_hop_id, attr_count, attr_list);
}
