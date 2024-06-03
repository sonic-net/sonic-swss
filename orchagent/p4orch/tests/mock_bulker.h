#pragma once

#include <gmock/gmock.h>
#define MOCK_MAX_BULK_SIZE 1000

extern "C"
{
#include "sai.h"
}

class MockNeighborBulker
{
  public:
    MOCK_METHOD4(create_entry, sai_status_t(_Out_ sai_status_t *object_status, _In_ const sai_neighbor_entry_t *entry, _In_ uint32_t attr_count, _In_ const sai_attribute_t *attr_list));

    MOCK_METHOD2(remove_entry, sai_status_t(_Out_ sai_status_t *object_status, _In_ const sai_neighbor_entry_t *entry));

    MOCK_METHOD0(flush, void());

    MOCK_METHOD0(clear, void());
};

MockNeighborBulker *mock_neighbor_bulker;

sai_status_t mock_create_entry(
    _Out_ sai_status_t *object_status,
    _In_ const sai_neighbor_entry_t *entry,
    _In_ uint32_t attr_count,
    _In_ const sai_attribute_t *attr_list)
{
    return mock_neighbor_bulker->create_entry(object_status, entry, attr_count, attr_list);
}

sai_status_t mock_remove_entry(
    _Out_ sai_status_t *object_status,
    _In_ const sai_neighbor_entry_t *entry)
{
    return mock_neighbor_bulker->remove_entry(object_status, entry);
}

void mock_flush()
{
    return mock_neighbor_bulker->flush();
}

void mock_clear()
{
    return mock_neighbor_bulker->clear();
}