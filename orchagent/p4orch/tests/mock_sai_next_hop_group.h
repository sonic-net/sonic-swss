// Define classes and functions to mock SAI next hop group functions.
#pragma once

#include <gmock/gmock.h>

extern "C"
{
#include "sai.h"
}

// Mock class including mock functions mapping to SAI next hop group's
// functions.
class MockSaiNextHopGroup
{
  public:
    MOCK_METHOD4(create_next_hop_group,
                 sai_status_t(_Out_ sai_object_id_t *next_hop_group_id, _In_ sai_object_id_t switch_id,
                              _In_ uint32_t attr_count, _In_ const sai_attribute_t *attr_list));

    MOCK_METHOD1(remove_next_hop_group, sai_status_t(_In_ sai_object_id_t next_hop_group_id));

    MOCK_METHOD4(create_next_hop_group_member,
                 sai_status_t(_Out_ sai_object_id_t *next_hop_group_id, _In_ sai_object_id_t switch_id,
                              _In_ uint32_t attr_count, _In_ const sai_attribute_t *attr_list));

    MOCK_METHOD1(remove_next_hop_group_member, sai_status_t(_In_ sai_object_id_t next_hop_group_member_id));

    MOCK_METHOD2(set_next_hop_group_member_attribute,
                 sai_status_t(_In_ sai_object_id_t next_hop_group_member_id, _In_ const sai_attribute_t *attr));
};

// Note that before mock functions below are used, mock_sai_next_hop_group must
// be initialized to point to an instance of MockSaiNextHopGroup.
MockSaiNextHopGroup *mock_sai_next_hop_group;

sai_status_t create_next_hop_group(_Out_ sai_object_id_t *next_hop_group_id, _In_ sai_object_id_t switch_id,
                                   _In_ uint32_t attr_count, _In_ const sai_attribute_t *attr_list)
{
    return mock_sai_next_hop_group->create_next_hop_group(next_hop_group_id, switch_id, attr_count, attr_list);
}

sai_status_t remove_next_hop_group(_In_ sai_object_id_t next_hop_group_id)
{
    return mock_sai_next_hop_group->remove_next_hop_group(next_hop_group_id);
}

sai_status_t create_next_hop_group_member(_Out_ sai_object_id_t *next_hop_group_member_id,
                                          _In_ sai_object_id_t switch_id, _In_ uint32_t attr_count,
                                          _In_ const sai_attribute_t *attr_list)
{
    return mock_sai_next_hop_group->create_next_hop_group_member(next_hop_group_member_id, switch_id, attr_count,
                                                                 attr_list);
}

sai_status_t remove_next_hop_group_member(_In_ sai_object_id_t next_hop_group_member_id)
{
    return mock_sai_next_hop_group->remove_next_hop_group_member(next_hop_group_member_id);
}

sai_status_t set_next_hop_group_member_attribute(_In_ sai_object_id_t next_hop_group_member_id,
                                                 _In_ const sai_attribute_t *attr)
{
    return mock_sai_next_hop_group->set_next_hop_group_member_attribute(next_hop_group_member_id, attr);
}
