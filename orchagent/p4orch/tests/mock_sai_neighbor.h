#pragma once

#include <gmock/gmock.h>

extern "C"
{
#include "sai.h"
}

// Mock Class mapping methods to neighbor object SAI APIs.
class MockSaiNeighbor
{
  public:
    MOCK_METHOD3(create_neighbor_entry, sai_status_t(_In_ const sai_neighbor_entry_t *neighbor_entry,
                                                     _In_ uint32_t attr_count, _In_ const sai_attribute_t *attr_list));

    MOCK_METHOD1(remove_neighbor_entry, sai_status_t(_In_ const sai_neighbor_entry_t *neighbor_entry));

    MOCK_METHOD2(set_neighbor_entry_attribute,
                 sai_status_t(_In_ const sai_neighbor_entry_t *neighbor_entry, _In_ const sai_attribute_t *attr));

    MOCK_METHOD3(get_neighbor_entry_attribute,
                 sai_status_t(_In_ const sai_neighbor_entry_t *neighbor_entry, _In_ uint32_t attr_count,
                              _Inout_ sai_attribute_t *attr_list));
};

MockSaiNeighbor *mock_sai_neighbor;

sai_status_t mock_create_neighbor_entry(_In_ const sai_neighbor_entry_t *neighbor_entry, _In_ uint32_t attr_count,
                                        _In_ const sai_attribute_t *attr_list)
{
    return mock_sai_neighbor->create_neighbor_entry(neighbor_entry, attr_count, attr_list);
}

sai_status_t mock_remove_neighbor_entry(_In_ const sai_neighbor_entry_t *neighbor_entry)
{
    return mock_sai_neighbor->remove_neighbor_entry(neighbor_entry);
}

sai_status_t mock_set_neighbor_entry_attribute(_In_ const sai_neighbor_entry_t *neighbor_entry,
                                               _In_ const sai_attribute_t *attr)
{
    return mock_sai_neighbor->set_neighbor_entry_attribute(neighbor_entry, attr);
}

sai_status_t mock_get_neighbor_entry_attribute(_In_ const sai_neighbor_entry_t *neighbor_entry,
                                               _In_ uint32_t attr_count, _Inout_ sai_attribute_t *attr_list)
{
    return mock_sai_neighbor->get_neighbor_entry_attribute(neighbor_entry, attr_count, attr_list);
}
