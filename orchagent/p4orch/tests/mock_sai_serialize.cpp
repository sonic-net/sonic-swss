#include "mock_sai_serialize.h"

MockSaiSerialize *mock_sai_serialize;

inline std::string sai_serialize_object_id(sai_object_id_t oid)
{
    return mock_sai_serialize->sai_serialize_object_id(oid);
}

inline std::string sai_serialize_object_type(sai_object_type_t object_type)
{
    return mock_sai_serialize->sai_serialize_object_type(object_type);
}
