#pragma once

#include <gmock/gmock.h>

#include "sai_serialize.h"

class SaiSerializeInterface
{
  public:
    virtual std::string sai_serialize_object_id(sai_object_id_t oid) = 0;
    virtual std::string sai_serialize_object_type(sai_object_type_t object_type) = 0;
};

class MockSaiSerialize : public SaiSerializeInterface
{
  public:
    MOCK_METHOD1(sai_serialize_object_id, std::string(sai_object_id_t oid));
    MOCK_METHOD1(sai_serialize_object_type, std::string(sai_object_type_t object_type));
};

extern MockSaiSerialize *mock_sai_serialize;

std::string sai_serialize_object_id(sai_object_id_t oid);

std::string sai_serialize_object_type(sai_object_type_t object_type);
