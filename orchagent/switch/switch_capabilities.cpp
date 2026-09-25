// includes -----------------------------------------------------------------------------------------------------------

extern "C" {
#include <saitypes.h>
#include <saiobject.h>
#include <saistatus.h>
#include <saihash.h>
#include <saiswitch.h>
}

#include <cstdint>
#include <string>
#include <vector>
#include <set>
#include <unordered_map>
#include <algorithm>

#include <sai_serialize.h>
#include <stringutility.h>
#include <schema.h>
#include <logger.h>

#include "switch_schema.h"
#include "switch_capabilities.h"
#include "switch_helper.h"

using namespace swss;

// defines ------------------------------------------------------------------------------------------------------------

#define SWITCH_CAPABILITY_HASH_NATIVE_HASH_FIELD_LIST_FIELD "HASH|NATIVE_HASH_FIELD_LIST"

#define SWITCH_CAPABILITY_ECMP_HASH_CAPABLE_FIELD "ECMP_HASH_CAPABLE"
#define SWITCH_CAPABILITY_LAG_HASH_CAPABLE_FIELD  "LAG_HASH_CAPABLE"

#define SWITCH_CAPABILITY_ECMP_HASH_ALGORITHM_FIELD         "ECMP_HASH_ALGORITHM"
#define SWITCH_CAPABILITY_ECMP_HASH_ALGORITHM_CAPABLE_FIELD "ECMP_HASH_ALGORITHM_CAPABLE"
#define SWITCH_CAPABILITY_LAG_HASH_ALGORITHM_FIELD          "LAG_HASH_ALGORITHM"
#define SWITCH_CAPABILITY_LAG_HASH_ALGORITHM_CAPABLE_FIELD  "LAG_HASH_ALGORITHM_CAPABLE"

#define SWITCH_CAPABILITY_ECMP_PKT_TYPE_HASH_CAPABLE_FIELD  "ECMP_PKT_TYPE_HASH_CAPABLE"
#define SWITCH_CAPABILITY_LAG_PKT_TYPE_HASH_CAPABLE_FIELD   "LAG_PKT_TYPE_HASH_CAPABLE"
#define SWITCH_CAPABILITY_HASH_ECMP_PKT_TYPE_LIST_FIELD     "HASH|ECMP_PKT_TYPE_LIST"
#define SWITCH_CAPABILITY_HASH_LAG_PKT_TYPE_LIST_FIELD      "HASH|LAG_PKT_TYPE_LIST"

#define SWITCH_CAPABILITY_KEY "switch"

#define SWITCH_STATE_DB_NAME    "STATE_DB"
#define SWITCH_STATE_DB_TIMEOUT 0

// constants ----------------------------------------------------------------------------------------------------------

static const std::unordered_map<sai_native_hash_field_t, std::string> swHashHashFieldMap =
{
    { SAI_NATIVE_HASH_FIELD_IN_PORT,           SWITCH_HASH_FIELD_IN_PORT           },
    { SAI_NATIVE_HASH_FIELD_DST_MAC,           SWITCH_HASH_FIELD_DST_MAC           },
    { SAI_NATIVE_HASH_FIELD_SRC_MAC,           SWITCH_HASH_FIELD_SRC_MAC           },
    { SAI_NATIVE_HASH_FIELD_ETHERTYPE,         SWITCH_HASH_FIELD_ETHERTYPE         },
    { SAI_NATIVE_HASH_FIELD_VLAN_ID,           SWITCH_HASH_FIELD_VLAN_ID           },
    { SAI_NATIVE_HASH_FIELD_IP_PROTOCOL,       SWITCH_HASH_FIELD_IP_PROTOCOL       },
    { SAI_NATIVE_HASH_FIELD_DST_IP,            SWITCH_HASH_FIELD_DST_IP            },
    { SAI_NATIVE_HASH_FIELD_SRC_IP,            SWITCH_HASH_FIELD_SRC_IP            },
    { SAI_NATIVE_HASH_FIELD_L4_DST_PORT,       SWITCH_HASH_FIELD_L4_DST_PORT       },
    { SAI_NATIVE_HASH_FIELD_L4_SRC_PORT,       SWITCH_HASH_FIELD_L4_SRC_PORT       },
    { SAI_NATIVE_HASH_FIELD_INNER_DST_MAC,     SWITCH_HASH_FIELD_INNER_DST_MAC     },
    { SAI_NATIVE_HASH_FIELD_INNER_SRC_MAC,     SWITCH_HASH_FIELD_INNER_SRC_MAC     },
    { SAI_NATIVE_HASH_FIELD_INNER_ETHERTYPE,   SWITCH_HASH_FIELD_INNER_ETHERTYPE   },
    { SAI_NATIVE_HASH_FIELD_INNER_IP_PROTOCOL, SWITCH_HASH_FIELD_INNER_IP_PROTOCOL },
    { SAI_NATIVE_HASH_FIELD_INNER_DST_IP,      SWITCH_HASH_FIELD_INNER_DST_IP      },
    { SAI_NATIVE_HASH_FIELD_INNER_SRC_IP,      SWITCH_HASH_FIELD_INNER_SRC_IP      },
    { SAI_NATIVE_HASH_FIELD_INNER_L4_DST_PORT, SWITCH_HASH_FIELD_INNER_L4_DST_PORT },
    { SAI_NATIVE_HASH_FIELD_INNER_L4_SRC_PORT, SWITCH_HASH_FIELD_INNER_L4_SRC_PORT },
    { SAI_NATIVE_HASH_FIELD_IPV6_FLOW_LABEL,   SWITCH_HASH_FIELD_IPV6_FLOW_LABEL   },
    { SAI_NATIVE_HASH_FIELD_RDMA_BTH_OPCODE,   SWITCH_HASH_FIELD_RDMA_BTH_OPCODE   },
    { SAI_NATIVE_HASH_FIELD_RDMA_BTH_DEST_QP,  SWITCH_HASH_FIELD_RDMA_BTH_DEST_QP  }
};

static const std::unordered_map<sai_hash_algorithm_t, std::string> swHashAlgorithmMap =
{
    { SAI_HASH_ALGORITHM_CRC,       SWITCH_HASH_ALGORITHM_CRC       },
    { SAI_HASH_ALGORITHM_XOR,       SWITCH_HASH_ALGORITHM_XOR       },
    { SAI_HASH_ALGORITHM_RANDOM,    SWITCH_HASH_ALGORITHM_RANDOM    },
    { SAI_HASH_ALGORITHM_CRC_32LO,  SWITCH_HASH_ALGORITHM_CRC_32LO  },
    { SAI_HASH_ALGORITHM_CRC_32HI,  SWITCH_HASH_ALGORITHM_CRC_32HI  },
    { SAI_HASH_ALGORITHM_CRC_CCITT, SWITCH_HASH_ALGORITHM_CRC_CCITT },
    { SAI_HASH_ALGORITHM_CRC_XOR,   SWITCH_HASH_ALGORITHM_CRC_XOR   }
};

// variables ----------------------------------------------------------------------------------------------------------

extern sai_object_id_t gSwitchId;

// functions ----------------------------------------------------------------------------------------------------------

static std::string toStr(sai_object_type_t objType, sai_attr_id_t attrId)
{
    const auto *meta = sai_metadata_get_attr_metadata(objType, attrId);

    return meta != nullptr ? meta->attridname : "UNKNOWN";
}

static std::string toStr(sai_native_hash_field_t value)
{
    const auto *name = sai_metadata_get_native_hash_field_name(value);

    return name != nullptr ? name : "UNKNOWN";
}

static std::string toStr(sai_hash_algorithm_t value)
{
    const auto *name = sai_metadata_get_hash_algorithm_name(value);

    return name != nullptr ? name : "UNKNOWN";
}

static std::string toStr(const std::set<sai_native_hash_field_t> &value)
{
    std::vector<std::string> strList;

    for (const auto &cit1 : value)
    {
        const auto &cit2 = swHashHashFieldMap.find(cit1);
        if (cit2 != swHashHashFieldMap.cend())
        {
            strList.push_back(cit2->second);
        }
    }

    return join(",", strList.cbegin(), strList.cend());
}

static std::string toStr(const std::set<sai_hash_algorithm_t> &value)
{
    std::vector<std::string> strList;

    for (const auto &cit1 : value)
    {
        const auto &cit2 = swHashAlgorithmMap.find(cit1);
        if (cit2 != swHashAlgorithmMap.cend())
        {
            strList.push_back(cit2->second);
        }
    }

    return join(",", strList.cbegin(), strList.cend());
}

static std::string toPktTypeListStr(const std::set<HashPktType>& pktTypeList)
{
    std::vector<std::string> names;

    for (auto pktType : pktTypeList)
    {
        names.push_back(hashPktTypeToString(pktType));
    }

    return names.empty() ? "N/A" : join(",", names.cbegin(), names.cend());
}

static std::string toStr(bool value)
{
    return value ? "true" : "false";
}

template <typename T1, typename T2>
static void insertBack(T1 &out, const T2 &in)
{
    out.insert(out.end(), in.cbegin(), in.cend());
}

// Switch capabilities ------------------------------------------------------------------------------------------------

DBConnector SwitchCapabilities::stateDb(SWITCH_STATE_DB_NAME, SWITCH_STATE_DB_TIMEOUT);
Table SwitchCapabilities::capTable(&stateDb, STATE_SWITCH_CAPABILITY_TABLE_NAME);

SwitchCapabilities::SwitchCapabilities()
{
    queryHashCapabilities();
    querySwitchCapabilities();

    writeHashCapabilitiesToDb();
    writeSwitchCapabilitiesToDb();
}

bool SwitchCapabilities::isSwitchEcmpHashSupported() const
{
    const auto &nativeHashFieldList = hashCapabilities.nativeHashFieldList;
    const auto &ecmpHash = switchCapabilities.ecmpHash;

    return nativeHashFieldList.isAttrSupported && ecmpHash.isAttrSupported;
}

bool SwitchCapabilities::isSwitchLagHashSupported() const
{
    const auto &nativeHashFieldList = hashCapabilities.nativeHashFieldList;
    const auto &lagHash = switchCapabilities.lagHash;

    return nativeHashFieldList.isAttrSupported && lagHash.isAttrSupported;
}

bool SwitchCapabilities::isSwitchEcmpPktTypeHashSupported() const
{
    const auto &nativeHashFieldList = hashCapabilities.nativeHashFieldList;
    const auto &ecmpPktTypeHashCapable = switchCapabilities.ecmpPktTypeHashCapable;

    return nativeHashFieldList.isAttrSupported && ecmpPktTypeHashCapable;
}

bool SwitchCapabilities::isSwitchLagPktTypeHashSupported() const
{
    const auto &nativeHashFieldList = hashCapabilities.nativeHashFieldList;
    const auto &lagPktTypeHashCapable = switchCapabilities.lagPktTypeHashCapable;

    return nativeHashFieldList.isAttrSupported && lagPktTypeHashCapable;
}

bool SwitchCapabilities::isSwitchEcmpHashAlgorithmSupported() const
{
    return switchCapabilities.ecmpHashAlgorithm.isAttrSupported;
}

bool SwitchCapabilities::isSwitchLagHashAlgorithmSupported() const
{
    return switchCapabilities.lagHashAlgorithm.isAttrSupported;
}

bool SwitchCapabilities::validateSwitchHashFieldCap(const std::set<sai_native_hash_field_t> &hfSet) const
{
    SWSS_LOG_ENTER();

    if (!hashCapabilities.nativeHashFieldList.isEnumSupported)
    {
        return true;
    }

    if (hashCapabilities.nativeHashFieldList.hfSet.empty())
    {
        SWSS_LOG_ERROR("Failed to validate hash field: no hash field capabilities");
        return false;
    }

    for (const auto &cit : hfSet)
    {
        if (hashCapabilities.nativeHashFieldList.hfSet.count(cit) == 0)
        {
            SWSS_LOG_ERROR("Failed to validate hash field: value(%s) is not supported", toStr(cit).c_str());
            return false;
        }
    }

    return true;
}

bool SwitchCapabilities::validateSwitchEcmpHashAlgorithmCap(sai_hash_algorithm_t haValue) const
{
    return validateSwitchHashAlgorithmCap(switchCapabilities.ecmpHashAlgorithm, haValue);
}

bool SwitchCapabilities::validateSwitchLagHashAlgorithmCap(sai_hash_algorithm_t haValue) const
{
    return validateSwitchHashAlgorithmCap(switchCapabilities.lagHashAlgorithm, haValue);
}

template<typename T>
bool SwitchCapabilities::validateSwitchHashAlgorithmCap(const T &obj, sai_hash_algorithm_t haValue) const
{
    SWSS_LOG_ENTER();

    if (!obj.isEnumSupported)
    {
        return true;
    }

    if (obj.haSet.empty())
    {
        SWSS_LOG_ERROR("Failed to validate hash algorithm: no hash algorithm capabilities");
        return false;
    }

    if (obj.haSet.count(haValue) == 0)
    {
        SWSS_LOG_ERROR("Failed to validate hash algorithm: value(%s) is not supported", toStr(haValue).c_str());
        return false;
    }

    return true;
}

bool SwitchCapabilities::validatePktTypeHashCap(const std::set<HashPktType> &supportedTypes, HashPktType pktType) const
{
    SWSS_LOG_ENTER();

    if (supportedTypes.count(pktType) == 0)
    {
        SWSS_LOG_ERROR("Failed to validate hash packet type: value %d (%s) is not supported.",
                       static_cast<int>(pktType),
                       hashPktTypeToString(pktType).c_str());
        return false;
    }

    return true;
}

bool SwitchCapabilities::validateSwitchEcmpPktTypeHashCap(HashPktType pktType) const
{
    return validatePktTypeHashCap(switchCapabilities.ecmpPktTypeList, pktType);
}

bool SwitchCapabilities::validateSwitchLagPktTypeHashCap(HashPktType pktType) const
{
    return validatePktTypeHashCap(switchCapabilities.lagPktTypeList, pktType);
}

FieldValueTuple SwitchCapabilities::makeHashFieldCapDbEntry() const
{
    const auto &nativeHashFieldList = hashCapabilities.nativeHashFieldList;

    auto field = SWITCH_CAPABILITY_HASH_NATIVE_HASH_FIELD_LIST_FIELD;
    auto value = nativeHashFieldList.isEnumSupported ? toStr(nativeHashFieldList.hfSet) : "N/A";

    return FieldValueTuple(field, value);
}

FieldValueTuple SwitchCapabilities::makeEcmpHashCapDbEntry() const
{
    auto field = SWITCH_CAPABILITY_ECMP_HASH_CAPABLE_FIELD;
    auto value = toStr(isSwitchEcmpHashSupported());

    return FieldValueTuple(field, value);
}

FieldValueTuple SwitchCapabilities::makeLagHashCapDbEntry() const
{
    auto field = SWITCH_CAPABILITY_LAG_HASH_CAPABLE_FIELD;
    auto value = toStr(isSwitchLagHashSupported());

    return FieldValueTuple(field, value);
}

std::vector<FieldValueTuple> SwitchCapabilities::makeEcmpHashAlgorithmCapDbEntry() const
{
    const auto &ecmpHashAlgorithm = switchCapabilities.ecmpHashAlgorithm;

    std::vector<FieldValueTuple> fvList;

    fvList.emplace_back(
        SWITCH_CAPABILITY_ECMP_HASH_ALGORITHM_FIELD,
        ecmpHashAlgorithm.isEnumSupported ? toStr(ecmpHashAlgorithm.haSet) : "N/A"
    );
    fvList.emplace_back(
        SWITCH_CAPABILITY_ECMP_HASH_ALGORITHM_CAPABLE_FIELD,
        toStr(isSwitchEcmpHashAlgorithmSupported())
    );

    return fvList;
}

std::vector<FieldValueTuple> SwitchCapabilities::makeLagHashAlgorithmCapDbEntry() const
{
    const auto &lagHashAlgorithm = switchCapabilities.lagHashAlgorithm;

    std::vector<FieldValueTuple> fvList;

    fvList.emplace_back(
        SWITCH_CAPABILITY_LAG_HASH_ALGORITHM_FIELD,
        lagHashAlgorithm.isEnumSupported ? toStr(lagHashAlgorithm.haSet) : "N/A"
    );
    fvList.emplace_back(
        SWITCH_CAPABILITY_LAG_HASH_ALGORITHM_CAPABLE_FIELD,
        toStr(isSwitchLagHashAlgorithmSupported())
    );

    return fvList;
}

FieldValueTuple SwitchCapabilities::makeEcmpPktTypeHashCapDbEntry() const
{
    auto field = SWITCH_CAPABILITY_ECMP_PKT_TYPE_HASH_CAPABLE_FIELD;
    auto value = toStr(isSwitchEcmpPktTypeHashSupported());

    return FieldValueTuple(field, value);
}

FieldValueTuple SwitchCapabilities::makeLagPktTypeHashCapDbEntry() const
{
    auto field = SWITCH_CAPABILITY_LAG_PKT_TYPE_HASH_CAPABLE_FIELD;
    auto value = toStr(isSwitchLagPktTypeHashSupported());

    return FieldValueTuple(field, value);
}

FieldValueTuple SwitchCapabilities::makeEcmpPktTypeListDbEntry() const
{
    auto field = SWITCH_CAPABILITY_HASH_ECMP_PKT_TYPE_LIST_FIELD;
    auto value = toPktTypeListStr(switchCapabilities.ecmpPktTypeList);

    return FieldValueTuple(field, value);
}

FieldValueTuple SwitchCapabilities::makeLagPktTypeListDbEntry() const
{
    auto field = SWITCH_CAPABILITY_HASH_LAG_PKT_TYPE_LIST_FIELD;
    auto value = toPktTypeListStr(switchCapabilities.lagPktTypeList);

    return FieldValueTuple(field, value);
}

sai_status_t SwitchCapabilities::queryEnumCapabilitiesSai(std::vector<sai_int32_t> &capList, sai_object_type_t objType, sai_attr_id_t attrId) const
{
    sai_s32_list_t enumList = { .count = 0, .list = nullptr };

    auto status = sai_query_attribute_enum_values_capability(gSwitchId, objType, attrId, &enumList);
    if ((status != SAI_STATUS_SUCCESS) && (status != SAI_STATUS_BUFFER_OVERFLOW))
    {
        return status;
    }

    capList.resize(enumList.count);
    enumList.list = capList.data();

    return sai_query_attribute_enum_values_capability(gSwitchId, objType, attrId, &enumList);
}

sai_status_t SwitchCapabilities::queryAttrCapabilitiesSai(sai_attr_capability_t &attrCap, sai_object_type_t objType, sai_attr_id_t attrId) const
{
    return sai_query_attribute_capability(gSwitchId, objType, attrId, &attrCap);
}

void SwitchCapabilities::queryHashNativeHashFieldListEnumCapabilities()
{
    SWSS_LOG_ENTER();

    std::vector<sai_int32_t> hfList;
    auto status = queryEnumCapabilitiesSai(
        hfList, SAI_OBJECT_TYPE_HASH, SAI_HASH_ATTR_NATIVE_HASH_FIELD_LIST
    );
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR(
            "Failed to get attribute(%s) enum value capabilities",
            toStr(SAI_OBJECT_TYPE_HASH, SAI_HASH_ATTR_NATIVE_HASH_FIELD_LIST).c_str()
        );
        return;
    }

    auto &hfSet = hashCapabilities.nativeHashFieldList.hfSet;
    std::transform(
        hfList.cbegin(), hfList.cend(), std::inserter(hfSet, hfSet.begin()),
        [](sai_int32_t value) { return static_cast<sai_native_hash_field_t>(value); }
    );

    hashCapabilities.nativeHashFieldList.isEnumSupported = true;
}

void SwitchCapabilities::queryHashNativeHashFieldListAttrCapabilities()
{
    SWSS_LOG_ENTER();

    sai_attr_capability_t attrCap;

    auto status = queryAttrCapabilitiesSai(
        attrCap, SAI_OBJECT_TYPE_HASH, SAI_HASH_ATTR_NATIVE_HASH_FIELD_LIST
    );
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR(
            "Failed to get attribute(%s) capabilities",
            toStr(SAI_OBJECT_TYPE_HASH, SAI_HASH_ATTR_NATIVE_HASH_FIELD_LIST).c_str()
        );
        return;
    }

    if (!attrCap.set_implemented)
    {
        SWSS_LOG_WARN(
            "Attribute(%s) SET is not implemented in SAI",
            toStr(SAI_OBJECT_TYPE_HASH, SAI_HASH_ATTR_NATIVE_HASH_FIELD_LIST).c_str()
        );
        return;
    }

    hashCapabilities.nativeHashFieldList.isAttrSupported = true;
}

bool SwitchCapabilities::isHashObjectPktTypeCapable() const
{
    SWSS_LOG_ENTER();

    sai_attr_capability_t attrCap{};

    auto status = queryAttrCapabilitiesSai(
        attrCap, SAI_OBJECT_TYPE_HASH, SAI_HASH_ATTR_NATIVE_HASH_FIELD_LIST
    );
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_WARN(
            "Failed to get attribute(%s) capabilities for packet-type hash",
            toStr(SAI_OBJECT_TYPE_HASH, SAI_HASH_ATTR_NATIVE_HASH_FIELD_LIST).c_str()
        );
        return false;
    }

    if (!attrCap.create_implemented || !attrCap.set_implemented)
    {
        SWSS_LOG_NOTICE(
            "Packet-type hash requires HASH attribute(%s) CREATE/SET support, but got "
            "create_implemented=%d set_implemented=%d; disabling packet-type hash",
            toStr(SAI_OBJECT_TYPE_HASH, SAI_HASH_ATTR_NATIVE_HASH_FIELD_LIST).c_str(),
            attrCap.create_implemented,
            attrCap.set_implemented
        );
        return false;
    }

    return true;
}

void SwitchCapabilities::queryHashCapabilities()
{
    queryHashNativeHashFieldListEnumCapabilities();
    queryHashNativeHashFieldListAttrCapabilities();
}

void SwitchCapabilities::querySwitchEcmpHashAttrCapabilities()
{
    SWSS_LOG_ENTER();

    sai_attr_capability_t attrCap;

    auto status = queryAttrCapabilitiesSai(
        attrCap, SAI_OBJECT_TYPE_SWITCH, SAI_SWITCH_ATTR_ECMP_HASH
    );
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR(
            "Failed to get attribute(%s) capabilities",
            toStr(SAI_OBJECT_TYPE_SWITCH, SAI_SWITCH_ATTR_ECMP_HASH).c_str()
        );
        return;
    }

    if (!attrCap.get_implemented)
    {
        SWSS_LOG_WARN(
            "Attribute(%s) GET is not implemented in SAI",
            toStr(SAI_OBJECT_TYPE_SWITCH, SAI_SWITCH_ATTR_ECMP_HASH).c_str()
        );
        return;
    }

    switchCapabilities.ecmpHash.isAttrSupported = true;
}

void SwitchCapabilities::querySwitchLagHashAttrCapabilities()
{
    SWSS_LOG_ENTER();

    sai_attr_capability_t attrCap;

    auto status = queryAttrCapabilitiesSai(
        attrCap, SAI_OBJECT_TYPE_SWITCH, SAI_SWITCH_ATTR_LAG_HASH
    );
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR(
            "Failed to get attribute(%s) capabilities",
            toStr(SAI_OBJECT_TYPE_SWITCH, SAI_SWITCH_ATTR_LAG_HASH).c_str()
        );
        return;
    }

    if (!attrCap.get_implemented)
    {
        SWSS_LOG_WARN(
            "Attribute(%s) GET is not implemented in SAI",
            toStr(SAI_OBJECT_TYPE_SWITCH, SAI_SWITCH_ATTR_LAG_HASH).c_str()
        );
        return;
    }

    switchCapabilities.lagHash.isAttrSupported = true;
}

void SwitchCapabilities::querySwitchEcmpHashAlgorithmEnumCapabilities()
{
    SWSS_LOG_ENTER();

    std::vector<sai_int32_t> haList;
    auto status = queryEnumCapabilitiesSai(
        haList, SAI_OBJECT_TYPE_SWITCH, SAI_SWITCH_ATTR_ECMP_DEFAULT_HASH_ALGORITHM
    );
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR(
            "Failed to get attribute(%s) enum value capabilities",
            toStr(SAI_OBJECT_TYPE_SWITCH, SAI_SWITCH_ATTR_ECMP_DEFAULT_HASH_ALGORITHM).c_str()
        );
        return;
    }

    auto &haSet = switchCapabilities.ecmpHashAlgorithm.haSet;
    std::transform(
        haList.cbegin(), haList.cend(), std::inserter(haSet, haSet.begin()),
        [](sai_int32_t value) { return static_cast<sai_hash_algorithm_t>(value); }
    );

    switchCapabilities.ecmpHashAlgorithm.isEnumSupported = true;
}

void SwitchCapabilities::querySwitchEcmpHashAlgorithmAttrCapabilities()
{
    SWSS_LOG_ENTER();

    sai_attr_capability_t attrCap;

    auto status = queryAttrCapabilitiesSai(
        attrCap, SAI_OBJECT_TYPE_SWITCH, SAI_SWITCH_ATTR_ECMP_DEFAULT_HASH_ALGORITHM
    );
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR(
            "Failed to get attribute(%s) capabilities",
            toStr(SAI_OBJECT_TYPE_SWITCH, SAI_SWITCH_ATTR_ECMP_DEFAULT_HASH_ALGORITHM).c_str()
        );
        return;
    }

    if (!attrCap.set_implemented)
    {
        SWSS_LOG_WARN(
            "Attribute(%s) SET is not implemented in SAI",
            toStr(SAI_OBJECT_TYPE_SWITCH, SAI_SWITCH_ATTR_ECMP_DEFAULT_HASH_ALGORITHM).c_str()
        );
        return;
    }

    switchCapabilities.ecmpHashAlgorithm.isAttrSupported = true;
}

void SwitchCapabilities::querySwitchLagHashAlgorithmEnumCapabilities()
{
    SWSS_LOG_ENTER();

    std::vector<sai_int32_t> haList;
    auto status = queryEnumCapabilitiesSai(
        haList, SAI_OBJECT_TYPE_SWITCH, SAI_SWITCH_ATTR_LAG_DEFAULT_HASH_ALGORITHM
    );
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR(
            "Failed to get attribute(%s) enum value capabilities",
            toStr(SAI_OBJECT_TYPE_SWITCH, SAI_SWITCH_ATTR_LAG_DEFAULT_HASH_ALGORITHM).c_str()
        );
        return;
    }

    auto &haSet = switchCapabilities.lagHashAlgorithm.haSet;
    std::transform(
        haList.cbegin(), haList.cend(), std::inserter(haSet, haSet.begin()),
        [](sai_int32_t value) { return static_cast<sai_hash_algorithm_t>(value); }
    );

    switchCapabilities.lagHashAlgorithm.isEnumSupported = true;
}

void SwitchCapabilities::querySwitchLagHashAlgorithmAttrCapabilities()
{
    SWSS_LOG_ENTER();

    sai_attr_capability_t attrCap;

    auto status = queryAttrCapabilitiesSai(
        attrCap, SAI_OBJECT_TYPE_SWITCH, SAI_SWITCH_ATTR_LAG_DEFAULT_HASH_ALGORITHM
    );
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR(
            "Failed to get attribute(%s) capabilities",
            toStr(SAI_OBJECT_TYPE_SWITCH, SAI_SWITCH_ATTR_LAG_DEFAULT_HASH_ALGORITHM).c_str()
        );
        return;
    }

    if (!attrCap.set_implemented)
    {
        SWSS_LOG_WARN(
            "Attribute(%s) SET is not implemented in SAI",
            toStr(SAI_OBJECT_TYPE_SWITCH, SAI_SWITCH_ATTR_LAG_DEFAULT_HASH_ALGORITHM).c_str()
        );
        return;
    }

    switchCapabilities.lagHashAlgorithm.isAttrSupported = true;
}

void SwitchCapabilities::querySwitchEcmpHashPktTypeCapabilities()
{
    SWSS_LOG_ENTER();

    // Verify that HASH object can be created and set
    if (!isHashObjectPktTypeCapable())
    {
        SWSS_LOG_NOTICE("HASH object is not packet-type capable; disabling ECMP packet-type hash");
        switchCapabilities.ecmpPktTypeHashCapable = false;
        switchCapabilities.ecmpPktTypeList.clear();
        return;
    }

    struct AttrMapEntry {
        sai_attr_id_t attrId;
        HashPktType pktType;
    };

    // List all ECMP hash packet type attributes and their corresponding enum
    const std::vector<AttrMapEntry> attrMap = {
        {SAI_SWITCH_ATTR_ECMP_HASH_IPV4, HashPktType::IPV4},
        {SAI_SWITCH_ATTR_ECMP_HASH_IPV6, HashPktType::IPV6},
        {SAI_SWITCH_ATTR_ECMP_HASH_IPV4_IN_IPV4, HashPktType::IPNIP},
        {SAI_SWITCH_ATTR_ECMP_HASH_IPV4_RDMA, HashPktType::IPV4_RDMA},
        {SAI_SWITCH_ATTR_ECMP_HASH_IPV6_RDMA, HashPktType::IPV6_RDMA}
    };

    bool foundAny = false;
    for (const auto& entry : attrMap)
    {
        sai_attr_capability_t attrCap;
        auto status = queryAttrCapabilitiesSai(attrCap, SAI_OBJECT_TYPE_SWITCH, entry.attrId);

        if (status != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_WARN(
                "Failed to get ECMP hash packet type attribute(%s) capabilities",
                toStr(SAI_OBJECT_TYPE_SWITCH, entry.attrId).c_str()
            );
            continue;
        }

        if (attrCap.set_implemented)
        {
            // Mark capability and insert supported packet type
            switchCapabilities.ecmpPktTypeList.insert(entry.pktType);
            foundAny = true;
        }
        else
        {
            SWSS_LOG_WARN(
                "Attribute(%s) SET is not implemented in SAI",
                toStr(SAI_OBJECT_TYPE_SWITCH, entry.attrId).c_str()
            );
        }
    }

    // Set flag if at least one packet type is supported
    switchCapabilities.ecmpPktTypeHashCapable = foundAny;
}

void SwitchCapabilities::querySwitchLagHashPktTypeCapabilities()
{
    SWSS_LOG_ENTER();

    // Verify that HASH object can be created and set
    if (!isHashObjectPktTypeCapable())
    {
        SWSS_LOG_NOTICE("HASH object is not packet-type capable; disabling LAG packet-type hash");
        switchCapabilities.lagPktTypeHashCapable = false;
        switchCapabilities.lagPktTypeList.clear();
        return;
    }

    struct AttrMapEntry {
        sai_attr_id_t attrId;
        HashPktType pktType;
    };

    // List all LAG hash packet type attributes and their corresponding enum
    const std::vector<AttrMapEntry> attrMap = {
        {SAI_SWITCH_ATTR_LAG_HASH_IPV4, HashPktType::IPV4},
        {SAI_SWITCH_ATTR_LAG_HASH_IPV6, HashPktType::IPV6},
        {SAI_SWITCH_ATTR_LAG_HASH_IPV4_IN_IPV4, HashPktType::IPNIP},
        {SAI_SWITCH_ATTR_LAG_HASH_IPV4_RDMA, HashPktType::IPV4_RDMA},
        {SAI_SWITCH_ATTR_LAG_HASH_IPV6_RDMA, HashPktType::IPV6_RDMA}
    };

    bool foundAny = false;
    for (const auto& entry : attrMap)
    {
        sai_attr_capability_t attrCap;
        auto status = queryAttrCapabilitiesSai(attrCap, SAI_OBJECT_TYPE_SWITCH, entry.attrId);

        if (status != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_WARN(
                "Failed to get LAG hash packet type attribute(%s) capabilities",
                toStr(SAI_OBJECT_TYPE_SWITCH, entry.attrId).c_str()
            );
            continue;
        }

        if (attrCap.set_implemented)
        {
            // Mark capability and insert supported packet type
            switchCapabilities.lagPktTypeList.insert(entry.pktType);
            foundAny = true;
        }
        else
        {
            SWSS_LOG_WARN(
                "Attribute(%s) SET is not implemented in SAI",
                toStr(SAI_OBJECT_TYPE_SWITCH, entry.attrId).c_str()
            );
        }
    }

    // Set flag if at least one packet type is supported
    switchCapabilities.lagPktTypeHashCapable = foundAny;
}

void SwitchCapabilities::querySwitchCapabilities()
{
    querySwitchEcmpHashAttrCapabilities();
    querySwitchLagHashAttrCapabilities();

    querySwitchEcmpHashAlgorithmEnumCapabilities();
    querySwitchEcmpHashAlgorithmAttrCapabilities();
    querySwitchLagHashAlgorithmEnumCapabilities();
    querySwitchLagHashAlgorithmAttrCapabilities();

    querySwitchEcmpHashPktTypeCapabilities();
    querySwitchLagHashPktTypeCapabilities();
}

void SwitchCapabilities::writeHashCapabilitiesToDb()
{
    SWSS_LOG_ENTER();

    std::vector<FieldValueTuple> fvList = {
        makeHashFieldCapDbEntry()
    };

    SwitchCapabilities::capTable.set(SWITCH_CAPABILITY_KEY, fvList);

    SWSS_LOG_NOTICE(
        "Wrote hash capabilities to State DB: %s key",
        SwitchCapabilities::capTable.getKeyName(SWITCH_CAPABILITY_KEY).c_str()
    );
}

void SwitchCapabilities::writeSwitchCapabilitiesToDb()
{
    SWSS_LOG_ENTER();

    std::vector<FieldValueTuple> fvList = {
        makeEcmpHashCapDbEntry(),
        makeLagHashCapDbEntry(),
        makeEcmpPktTypeHashCapDbEntry(),      // ECMP packet type hash support flag
        makeLagPktTypeHashCapDbEntry(),       // LAG packet type hash support flag
        makeEcmpPktTypeListDbEntry(),         // ECMP packet type list
        makeLagPktTypeListDbEntry()           // LAG packet type list
    };
    insertBack(fvList, makeEcmpHashAlgorithmCapDbEntry());
    insertBack(fvList, makeLagHashAlgorithmCapDbEntry());

    SwitchCapabilities::capTable.set(SWITCH_CAPABILITY_KEY, fvList);

    SWSS_LOG_NOTICE(
        "Wrote switch capabilities to State DB: %s key",
        SwitchCapabilities::capTable.getKeyName(SWITCH_CAPABILITY_KEY).c_str()
    );
}
