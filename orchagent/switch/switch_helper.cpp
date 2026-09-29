// includes -----------------------------------------------------------------------------------------------------------

extern "C" {
#include <saihash.h>
#include <saiswitch.h>
}

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
    { SWITCH_HASH_FIELD_INNER_L4_SRC_PORT, SAI_NATIVE_HASH_FIELD_INNER_L4_SRC_PORT },
    { SWITCH_HASH_FIELD_IPV6_FLOW_LABEL,   SAI_NATIVE_HASH_FIELD_IPV6_FLOW_LABEL   },
    { SWITCH_HASH_FIELD_RDMA_BTH_OPCODE,   SAI_NATIVE_HASH_FIELD_RDMA_BTH_OPCODE   },
    { SWITCH_HASH_FIELD_RDMA_BTH_DEST_QP,  SAI_NATIVE_HASH_FIELD_RDMA_BTH_DEST_QP  }
};

static const std::unordered_map<std::string, sai_hash_algorithm_t> swHashAlgorithmMap =
{
    { SWITCH_HASH_ALGORITHM_CRC,       SAI_HASH_ALGORITHM_CRC       },
    { SWITCH_HASH_ALGORITHM_XOR,       SAI_HASH_ALGORITHM_XOR       },
    { SWITCH_HASH_ALGORITHM_RANDOM,    SAI_HASH_ALGORITHM_RANDOM    },
    { SWITCH_HASH_ALGORITHM_CRC_32LO,  SAI_HASH_ALGORITHM_CRC_32LO  },
    { SWITCH_HASH_ALGORITHM_CRC_32HI,  SAI_HASH_ALGORITHM_CRC_32HI  },
    { SWITCH_HASH_ALGORITHM_CRC_CCITT, SAI_HASH_ALGORITHM_CRC_CCITT },
    { SWITCH_HASH_ALGORITHM_CRC_XOR,   SAI_HASH_ALGORITHM_CRC_XOR   }
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

void SwitchHelper::setSwHashPacketType(const SwitchHash &hash)
{
    SWSS_LOG_ENTER();

    // Update only packet-type fields, preserve global fields
    swHash.ecmp_hash_pkt_type = hash.ecmp_hash_pkt_type;
    swHash.lag_hash_pkt_type = hash.lag_hash_pkt_type;

    SWSS_LOG_INFO("Updated packet-type hash cache: ECMP types=%zu, LAG types=%zu",
                 swHash.ecmp_hash_pkt_type.size(),
                 swHash.lag_hash_pkt_type.size());
}
template<typename T>
bool SwitchHelper::parseSwHashFieldList(T &obj, const std::string &field, const std::string &value) const
{
    SWSS_LOG_ENTER();

    // Handle empty value for delete operations
    if (value.empty())
    {
        SWSS_LOG_INFO("Empty value for field(%s): marking for deletion", field.c_str());
        obj.value.clear();
        obj.is_set = false;  // Mark as not set (to be deleted)
        return true;
    }
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

template<typename T> // SwitchHash::HashField
bool SwitchHelper::parseSwHashPacketTypeFieldList(T &obj, const std::string &field, const std::string &value) const
{
    SWSS_LOG_ENTER();

    // Empty value means: delete entire packet-type attribute and remove SAI OID
    if (value.empty())
    {
        SWSS_LOG_NOTICE("Empty value for field(%s): marking for complete deletion (will remove SAI OID)", field.c_str());
        obj.markForDeletion();
        return true;
    }

    const auto &hfList = tokenize(value, ',');

    // Empty list after tokenization also means deletion
    if (hfList.empty())
    {
        SWSS_LOG_NOTICE("Empty field list for field(%s): marking for complete deletion (will remove SAI OID)", field.c_str());
        obj.markForDeletion();
        return true;
    }

    const auto &hfSet = std::unordered_set<std::string>(hfList.cbegin(), hfList.cend());

    if (hfSet.size() != hfList.size())
    {
        SWSS_LOG_ERROR("Duplicate hash fields in field(%s): unexpected value(%s)", field.c_str(), value.c_str());
        return false;
    }

    std::set<sai_native_hash_field_t> newValue;

    for (const auto &cit1 : hfSet)
    {
        const auto &cit2 = swHashHashFieldMap.find(cit1);
        if (cit2 == swHashHashFieldMap.cend())
        {
            SWSS_LOG_ERROR("Failed to parse field(%s): invalid hash field(%s)", field.c_str(), cit1.c_str());
            return false;
        }

        newValue.insert(cit2->second);
    }

    obj.setValue(newValue);

    SWSS_LOG_INFO("Parsed field(%s): state=%s, fields=%zu",
                  field.c_str(),
                  obj.isSet() ? "SET" : (obj.isDeleteAll() ? "DELETE_ALL" : "NOT_SET"),
                  obj.value.size());

    return true;
}

template<typename T>
bool SwitchHelper::parseSwHashAlgorithm(T &obj, const std::string &field, const std::string &value) const
{
    SWSS_LOG_ENTER();

    if (value.empty())
    {
        SWSS_LOG_ERROR("Failed to parse field(%s): empty value is prohibited", field.c_str());
        return false;
    }

    const auto &cit = swHashAlgorithmMap.find(value);
    if (cit == swHashAlgorithmMap.cend())
    {
        SWSS_LOG_ERROR("Failed to parse field(%s): invalid value(%s)", field.c_str(), value.c_str());
        return false;
    }

    obj.value = cit->second;
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

bool SwitchHelper::parseSwHashEcmpHashAlgorithm(SwitchHash &hash, const std::string &field, const std::string &value) const
{
    return parseSwHashAlgorithm(hash.ecmp_hash_algorithm, field, value);
}

bool SwitchHelper::parseSwHashLagHashAlgorithm(SwitchHash &hash, const std::string &field, const std::string &value) const
{
    return parseSwHashAlgorithm(hash.lag_hash_algorithm, field, value);
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
        else if (field == SWITCH_HASH_ECMP_HASH_ALGORITHM)
        {
            if (!parseSwHashEcmpHashAlgorithm(hash, field, value))
            {
                return false;
            }
        }
        else if (field == SWITCH_HASH_LAG_HASH_ALGORITHM)
        {
            if (!parseSwHashLagHashAlgorithm(hash, field, value))
            {
                return false;
            }
        }
        else
        {
            // Try to parse as packet-type field
            HashPktType pktType;
            bool isEcmp;

            if (extractHashPktType(field, pktType, isEcmp))
            {
                auto &targetMap = isEcmp ? hash.ecmp_hash_pkt_type : hash.lag_hash_pkt_type;

                if (!parseSwHashPacketTypeFieldList(targetMap[pktType], field, value))
                {
                    SWSS_LOG_ERROR("Failed to parse packet-type field(%s): value(%s)",
                                  field.c_str(), value.c_str());
                    return false;
                }
            }
            else
            {
                // Special handling for "NULL" sentinel field used in delete-only updates
                if (field == "NULL")
                {
                    SWSS_LOG_INFO("Skipping synthetic NULL field in packet-type hash delete operation");
                    continue;
                }
                SWSS_LOG_WARN("Unknown field(%s): skipping ...", field.c_str());
            }
        }
    }

    if (!validateSwHash(hash))
    {
        SWSS_LOG_ERROR("Validation failed after parsing switch hash");
        return false;
    }

    return true;
}

bool SwitchHelper::validateSwHash(SwitchHash &hash) const
{
    SWSS_LOG_ENTER();

    // Lambda to validate consistency between state and value
    auto isConsistent = [](const SwitchHash::HashField& field, const std::string& name) -> bool {
        if (field.isDeleteAll() && !field.value.empty()) {
            SWSS_LOG_ERROR("Validation error: %s marked for deletion but has non-empty value",
                          name.c_str());
            return false;
        }
        if (field.isSet() && field.value.empty()) {
            SWSS_LOG_ERROR("Validation error: %s marked as SET but has empty value",
                          name.c_str());
            return false;
        }
        return true;
    };

    // Check ECMP packet-type fields
    for (const auto &hashEntry : hash.ecmp_hash_pkt_type)
    {
        const auto &pktType = hashEntry.first;
        const auto &hashField = hashEntry.second;

        std::string fieldName = "ecmp_hash_" + hashPktTypeToString(pktType);
        if (!isConsistent(hashField, fieldName))
        {
            return false;
        }
    }

    // Check LAG packet-type fields
    for (const auto &hashEntry : hash.lag_hash_pkt_type)
    {
        const auto &pktType = hashEntry.first;
        const auto &hashField = hashEntry.second;

        std::string fieldName = "lag_hash_" + hashPktTypeToString(pktType);
        if (!isConsistent(hashField, fieldName))
        {
            return false;
        }
    }
    SWSS_LOG_INFO("Switch hash validation successful");
    return true;
}
