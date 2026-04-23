/*
 * SAI FDB is created as its own mock in order to add functionality for
 * non-common actions like flush_fdb_entries.
 */

#pragma once

#include <gmock/gmock.h>

extern "C"
{
#include "sai.h"
}

// Mock Class mapping methods to FDB SAI APIs.
class mock_sai_fdb_api_t {
    public:
        MOCK_METHOD3(create_fdb_entry, sai_status_t(CREATE_PARAMS(fdb)));
        MOCK_METHOD1(remove_fdb_entry, sai_status_t(REMOVE_PARAMS(fdb)));
        MOCK_METHOD3(flush_fdb_entries, sai_status_t(_In_ sai_object_id_t switch_id,
                         _In_ uint32_t attr_count,
                         _In_ const sai_attribute_t *attr_list));
};

extern sai_fdb_api_t *old_sai_fdb_api;
extern sai_fdb_api_t ut_sai_fdb_api;
extern mock_sai_fdb_api_t *mock_sai_fdb_api;

sai_status_t mock_create_fdb_entry(CREATE_PARAMS(fdb));

sai_status_t mock_remove_fdb_entry(REMOVE_PARAMS(fdb));

sai_status_t mock_flush_fdb_entries(
    _In_ sai_object_id_t switch_id,
    _In_ uint32_t attr_count,
    _In_ const sai_attribute_t *attr_list);

void apply_sai_fdb_api_mock();

void remove_sai_fdb_api_mock();
