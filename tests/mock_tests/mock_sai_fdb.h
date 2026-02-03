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

sai_fdb_api_t *old_sai_fdb_api;
sai_fdb_api_t ut_sai_fdb_api;

class mock_sai_fdb_api_t {
    public:
        MOCK_METHOD3(create_fdb_entry, sai_status_t(CREATE_PARAMS(fdb)));
        MOCK_METHOD1(remove_fdb_entry, sai_status_t(REMOVE_PARAMS(fdb)));
        MOCK_METHOD3(flush_fdb_entries, sai_status_t(_In_ sai_object_id_t switch_id,
                         _In_ uint32_t attr_count,
                         _In_ const sai_attribute_t *attr_list));
};

    mock_sai_fdb_api_t *mock_sai_fdb_api;
    sai_status_t mock_create_fdb_entry(CREATE_PARAMS(fdb))
    {
        return mock_sai_fdb_api->create_fdb_entry(CREATE_ARGS(fdb));
    }

    sai_status_t mock_remove_fdb_entry(REMOVE_PARAMS(fdb))
    {
        return mock_sai_fdb_api->remove_fdb_entry(REMOVE_ARGS(fdb));
    }

    sai_status_t mock_flush_fdb_entries(
        _In_ sai_object_id_t switch_id,
        _In_ uint32_t attr_count,
        _In_ const sai_attribute_t *attr_list)
    {
        return mock_sai_fdb_api->flush_fdb_entries(switch_id, attr_count, attr_list);
    }

    void apply_sai_fdb_api_mock()
    {
        mock_sai_fdb_api = new NiceMock<mock_sai_fdb_api_t>();

        old_sai_fdb_api = sai_fdb_api;
        ut_sai_fdb_api = *sai_fdb_api;
        sai_fdb_api = &ut_sai_fdb_api;

        sai_fdb_api->create_fdb_entry = mock_create_fdb_entry;
        sai_fdb_api->remove_fdb_entry = mock_remove_fdb_entry;
        sai_fdb_api->flush_fdb_entries = mock_flush_fdb_entries;
    }

    void remove_sai_fdb_api_mock()
    {
        sai_fdb_api = old_sai_fdb_api;
        delete mock_sai_fdb_api;
    }
