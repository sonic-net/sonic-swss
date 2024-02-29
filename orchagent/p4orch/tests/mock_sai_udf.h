#pragma once

#include <gmock/gmock.h>

extern "C"
{
#include "sai.h"
#include "saiudf.h"
}

class SaiUdfInterface
{
  public:
    virtual sai_status_t create_udf(sai_object_id_t *udf_id, sai_object_id_t switch_id, uint32_t attr_count,
                                    const sai_attribute_t *attr_list) = 0;
    virtual sai_status_t remove_udf(sai_object_id_t udf_id) = 0;
    virtual sai_status_t create_udf_group(sai_object_id_t *udf_group_id, sai_object_id_t switch_id, uint32_t attr_count,
                                          const sai_attribute_t *attr_list) = 0;
    virtual sai_status_t remove_udf_group(sai_object_id_t udf_group_id) = 0;
    virtual sai_status_t create_udf_match(sai_object_id_t *udf_match_id, sai_object_id_t switch_id, uint32_t attr_count,
                                          const sai_attribute_t *attr_list) = 0;
    virtual sai_status_t remove_udf_match(sai_object_id_t udf_match_id) = 0;
};

class MockSaiUdf : public SaiUdfInterface
{
  public:
    MOCK_METHOD4(create_udf, sai_status_t(sai_object_id_t *udf_id, sai_object_id_t switch_id, uint32_t attr_count,
                                          const sai_attribute_t *attr_list));
    MOCK_METHOD1(remove_udf, sai_status_t(sai_object_id_t udf_id));
    MOCK_METHOD4(create_udf_group, sai_status_t(sai_object_id_t *udf_group_id, sai_object_id_t switch_id,
                                                uint32_t attr_count, const sai_attribute_t *attr_list));
    MOCK_METHOD1(remove_udf_group, sai_status_t(sai_object_id_t udf_group_id));
    MOCK_METHOD4(create_udf_match, sai_status_t(sai_object_id_t *udf_match_id, sai_object_id_t switch_id,
                                                uint32_t attr_count, const sai_attribute_t *attr_list));
    MOCK_METHOD1(remove_udf_match, sai_status_t(sai_object_id_t udf_match_id));
};

extern MockSaiUdf *mock_sai_udf;

sai_status_t create_udf(sai_object_id_t *udf_id, sai_object_id_t switch_id, uint32_t attr_count,
                        const sai_attribute_t *attr_list);

sai_status_t remove_udf(sai_object_id_t udf_id);

sai_status_t create_udf_group(sai_object_id_t *udf_group_id, sai_object_id_t switch_id, uint32_t attr_count,
                              const sai_attribute_t *attr_list);

sai_status_t remove_udf_group(sai_object_id_t udf_group_id);

sai_status_t create_udf_match(sai_object_id_t *udf_match_id, sai_object_id_t switch_id, uint32_t attr_count,
                              const sai_attribute_t *attr_list);

sai_status_t remove_udf_match(sai_object_id_t udf_match_id);
