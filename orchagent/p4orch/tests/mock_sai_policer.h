#pragma once

extern "C"
{
#include "sai.h"
#include "saipolicer.h"
}

class SaiPolicerInterface
{
  public:
    virtual sai_status_t create_policer(sai_object_id_t *policer_id, sai_object_id_t switch_id, uint32_t attr_count,
                                        const sai_attribute_t *attr_list) = 0;
    virtual sai_status_t remove_policer(sai_object_id_t policer_id) = 0;
    virtual sai_status_t get_policer_stats(sai_object_id_t policer_id, uint32_t number_of_counters,
                                           const sai_stat_id_t *counter_ids, uint64_t *counters) = 0;
    virtual sai_status_t set_policer_attribute(sai_object_id_t policer_id, const sai_attribute_t *attr) = 0;
};

class MockSaiPolicer : public SaiPolicerInterface
{
  public:
    MOCK_METHOD4(create_policer, sai_status_t(sai_object_id_t *policer_id, sai_object_id_t switch_id,
                                              uint32_t attr_count, const sai_attribute_t *attr_list));
    MOCK_METHOD1(remove_policer, sai_status_t(sai_object_id_t policer_id));
    MOCK_METHOD4(get_policer_stats, sai_status_t(sai_object_id_t policer_id, uint32_t number_of_counters,
                                                 const sai_stat_id_t *counter_ids, uint64_t *counters));
    MOCK_METHOD2(set_policer_attribute, sai_status_t(sai_object_id_t policer_id, const sai_attribute_t *attr));
};

MockSaiPolicer *mock_sai_policer;

sai_status_t create_policer(sai_object_id_t *policer_id, sai_object_id_t switch_id, uint32_t attr_count,
                            const sai_attribute_t *attr_list)
{
    return mock_sai_policer->create_policer(policer_id, switch_id, attr_count, attr_list);
}

sai_status_t remove_policer(sai_object_id_t policer_id)
{
    return mock_sai_policer->remove_policer(policer_id);
}

sai_status_t get_policer_stats(sai_object_id_t policer_id, uint32_t number_of_counters,
                               const sai_stat_id_t *counter_ids, uint64_t *counters)
{
    return mock_sai_policer->get_policer_stats(policer_id, number_of_counters, counter_ids, counters);
}

sai_status_t set_policer_attribute(sai_object_id_t policer_id, const sai_attribute_t *attr)
{
    return mock_sai_policer->set_policer_attribute(policer_id, attr);
}
