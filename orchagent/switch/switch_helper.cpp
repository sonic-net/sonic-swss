// includes -----------------------------------------------------------------------------------------------------------

#include <unordered_map>
#include <unordered_set>
#include <string>

#include <tokenize.h>
#include <logger.h>

#include "switch_schema.h"
#include "switch_helper.h"

using namespace swss;

// constants ----------------------------------------------------------------------------------------------------------

static const std::unordered_map<std::string, sai_native_hash_field_t> swHashHashFieldMap =
{
    { SWITCH_HASH_FIELD_IN_PORT,           SAI_NATIVE_HASH_FIELD_IN_PORT           },
    { SWITCH_HASH_FIELD_DST_MAC,           SAI_NATIVE_HASH_FIELD_DST_MAC           },
    { SWITCH_HASH_FIELD_SRC_MAC,           SAI_NATIVE_HASH_FIELD_SRC_MAC           },
    { SWITCH_HASH_FIELD_ETHERTYPE,         SAI_NATIVE_HASH_FIELD_ETHERTYPE         },
    { SWITCH_HASH_FIELD_VLAN_ID,           SAI_NATIVE_HASH_FIELD_VLAN_ID           },
    { SWITCH_HASH_FIELD_IP_PROTOCOL,       SAI_NATIVE_HASH_FIELD_IP_PROTOCOL       },
    { SWITCH_HASH_FIELD_DST_IP,            SAI_NATIVE_HASH_FIELD_DST_IP            },
    { SWITCH_HASH_FIELD_SRC_IP,            SAI_NATIVE_HASH_FIELD_SRC_IP            },
    { SWITCH_HASH_FIELD_L4_DST_PORT,       SAI_NATIVE_HASH_FIELD_L4_DST_PORT       },
    { SWITCH_HASH_FIELD_L4_SRC_PORT,       SAI_NATIVE_HASH_FIELD_L4_SRC_PORT       },
    { SWITCH_HASH_FIELD_INNER_DST_MAC,     SAI_NATIVE_HASH_FIELD_INNER_DST_MAC     },
    { SWITCH_HASH_FIELD_INNER_SRC_MAC,     SAI_NATIVE_HASH_FIELD_INNER_SRC_MAC     },
    { SWITCH_HASH_FIELD_INNER_ETHERTYPE,   SAI_NATIVE_HASH_FIELD_INNER_ETHERTYPE   },
    { SWITCH_HASH_FIELD_INNER_IP_PROTOCOL, SAI_NATIVE_HASH_FIELD_INNER_IP_PROTOCOL },
    { SWITCH_HASH_FIELD_INNER_DST_IP,      SAI_NATIVE_HASH_FIELD_INNER_DST_IP      },
    { SWITCH_HASH_FIELD_INNER_SRC_IP,      SAI_NATIVE_HASH_FIELD_INNER_SRC_IP      },
    { SWITCH_HASH_FIELD_INNER_L4_DST_PORT, SAI_NATIVE_HASH_FIELD_INNER_L4_DST_PORT },
    { SWITCH_HASH_FIELD_INNER_L4_SRC_PORT, SAI_NATIVE_HASH_FIELD_INNER_L4_SRC_PORT }
};

// switch helper ------------------------------------------------------------------------------------------------------

const SwitchHash& SwitchHelper::getSwHash() const
{
    return swHash;
}

void SwitchHelper::setSwHash(const SwitchHash &hash)
{
    swHash = hash;
}

template<typename T>
bool SwitchHelper::parseSwHashFieldList(T &obj, const std::string &field, const std::string &value) const
{
    SWSS_LOG_ENTER();

    const auto &hfList = tokenize(value, ',');

    if (hfList.empty())
    {
        SWSS_LOG_ERROR("Failed to parse field(%s): empty list is prohibited", field.c_str());
        return false;
    }

    const auto &hfSet = std::unordered_set<std::string>(hfList.cbegin(), hfList.cend());

    if (hfSet.size() != hfList.size())
    {
        SWSS_LOG_ERROR("Duplicate hash fields in field(%s): unexpected value(%s)", field.c_str(), value.c_str());
        return false;
    }

    for (const auto &cit1 : hfSet)
    {
        const auto &cit2 = swHashHashFieldMap.find(cit1);
        if (cit2 == swHashHashFieldMap.cend())
        {
            SWSS_LOG_ERROR("Failed to parse field(%s): invalid value(%s)", field.c_str(), value.c_str());
            return false;
        }

        obj.value.insert(cit2->second);
    }

    obj.is_set = true;

    return true;
}

bool SwitchHelper::parseSwHashEcmpHash(SwitchHash &hash, const std::string &field, const std::string &value) const
{
    return parseSwHashFieldList(hash.ecmp_hash, field, value);
}

bool SwitchHelper::parseSwHashLagHash(SwitchHash &hash, const std::string &field, const std::string &value) const
{
    return parseSwHashFieldList(hash.lag_hash, field, value);
}

bool SwitchHelper::parseSwHash(SwitchHash &hash) const
{
    SWSS_LOG_ENTER();

    for (const auto &cit : hash.fieldValueMap)
    {
        const auto &field = cit.first;
        const auto &value = cit.second;

        if (field == SWITCH_HASH_ECMP_HASH)
        {
            if (!parseSwHashEcmpHash(hash, field, value))
            {
                return false;
            }
        }
        else if (field == SWITCH_HASH_LAG_HASH)
        {
            if (!parseSwHashLagHash(hash, field, value))
            {
                return false;
            }
        }
        else
        {
            SWSS_LOG_WARN("Unknown field(%s): skipping ...", field.c_str());
        }
    }

    return validateSwHash(hash);
}

bool SwitchHelper::validateSwHash(SwitchHash &hash) const
{
    SWSS_LOG_ENTER();

    if (!hash.ecmp_hash.is_set && !hash.lag_hash.is_set)
    {
        SWSS_LOG_ERROR("Validation error: missing valid fields");
        return false;
    }

    return true;
}
