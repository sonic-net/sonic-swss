#ifndef SWSS_UDFORCH_CONSTANTS_H
#define SWSS_UDFORCH_CONSTANTS_H

#include <string>
#include "logger.h"

#define UDF_MAX_OFFSET          255
#define UDF_GROUP_MAX_LENGTH    20
#define UDF_GROUP_MIN_LENGTH    1
#define UDF_NAME_MAX_LENGTH     64

inline sai_udf_group_type_t getUdfGroupType(const std::string& type_str)
{
    if (type_str == "GENERIC") return SAI_UDF_GROUP_TYPE_GENERIC;
    if (type_str == "HASH") return SAI_UDF_GROUP_TYPE_HASH;
    SWSS_LOG_WARN("Unknown UDF group type '%s', defaulting to GENERIC", type_str.c_str());
    return SAI_UDF_GROUP_TYPE_GENERIC;
}

inline sai_udf_base_t getUdfBaseType(const std::string& base_str)
{
    if (base_str == "L2") return SAI_UDF_BASE_L2;
    if (base_str == "L3") return SAI_UDF_BASE_L3;
    if (base_str == "L4") return SAI_UDF_BASE_L4;
    SWSS_LOG_WARN("Unknown UDF base type '%s', defaulting to L2", base_str.c_str());
    return SAI_UDF_BASE_L2;
}

inline std::string getUdfGroupTypeString(sai_udf_group_type_t type)
{
    switch (type)
    {
        case SAI_UDF_GROUP_TYPE_GENERIC: return "GENERIC";
        case SAI_UDF_GROUP_TYPE_HASH: return "HASH";
        default: return "GENERIC";
    }
}

inline std::string getUdfBaseTypeString(sai_udf_base_t base)
{
    switch (base)
    {
        case SAI_UDF_BASE_L2: return "L2";
        case SAI_UDF_BASE_L3: return "L3";
        case SAI_UDF_BASE_L4: return "L4";
        default: return "L2";
    }
}

inline bool isValidUdfOffset(uint16_t offset)
{
    return (offset <= UDF_MAX_OFFSET);
}

inline bool isValidUdfBase(const std::string& base_str)
{
    return (base_str == "L2" || base_str == "L3" || base_str == "L4");
}

inline bool isValidUdfGroupLength(uint16_t length)
{
    return (length >= UDF_GROUP_MIN_LENGTH && length <= UDF_GROUP_MAX_LENGTH);
}

inline bool isValidUdfName(const std::string& name)
{
    return (!name.empty() && name.length() <= UDF_NAME_MAX_LENGTH);
}

#endif /* SWSS_UDFORCH_CONSTANTS_H */
