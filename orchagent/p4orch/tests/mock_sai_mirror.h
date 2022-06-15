// Define classes and functions to mock SAI mirror functions.
#pragma once

#include <gmock/gmock.h>

extern "C"
{
#include "sai.h"
}

// Mock class including mock functions mapping to SAI mirror's functions.
class MockSaiMirror
{
  public:
    MOCK_METHOD4(create_mirror_session,
                 sai_status_t(_Out_ sai_object_id_t *mirror_session_id, _In_ sai_object_id_t switch_id,
                              _In_ uint32_t attr_count, _In_ const sai_attribute_t *attr_list));

    MOCK_METHOD1(remove_mirror_session, sai_status_t(_In_ sai_object_id_t mirror_session_id));

    MOCK_METHOD2(set_mirror_session_attribute,
                 sai_status_t(_In_ sai_object_id_t mirror_session_id, _In_ const sai_attribute_t *attr));

    MOCK_METHOD3(get_mirror_session_attribute,
                 sai_status_t(_In_ sai_object_id_t mirror_session_id, _In_ uint32_t attr_count,
                              _Inout_ sai_attribute_t *attr_list));
};

// Note that before mock functions below are used, mock_sai_mirror must be
// initialized to point to an instance of MockSaiMirror.
MockSaiMirror *mock_sai_mirror;

sai_status_t mock_create_mirror_session(_Out_ sai_object_id_t *mirror_session_id, _In_ sai_object_id_t switch_id,
                                        _In_ uint32_t attr_count, _In_ const sai_attribute_t *attr_list)
{
    return mock_sai_mirror->create_mirror_session(mirror_session_id, switch_id, attr_count, attr_list);
}

sai_status_t mock_remove_mirror_session(_In_ sai_object_id_t mirror_session_id)
{
    return mock_sai_mirror->remove_mirror_session(mirror_session_id);
}

sai_status_t mock_set_mirror_session_attribute(_In_ sai_object_id_t mirror_session_id, _In_ const sai_attribute_t *attr)
{
    return mock_sai_mirror->set_mirror_session_attribute(mirror_session_id, attr);
}

sai_status_t mock_get_mirror_session_attribute(_In_ sai_object_id_t mirror_session_id, _In_ uint32_t attr_count,
                                               _Inout_ sai_attribute_t *attr_list)
{
    return mock_sai_mirror->get_mirror_session_attribute(mirror_session_id, attr_count, attr_list);
}
