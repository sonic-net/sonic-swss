// includes -----------------------------------------------------------------------------------------------------------

extern "C" {
#include <saiobject.h>
#include <saistatus.h>
#include <saitypes.h>
#include <saiswitch.h>
}

#include <cstdint>
#include <string>
#include <vector>
#include <set>
#include <algorithm>

#include <sai_serialize.h>
#include <stringutility.h>
#include <schema.h>
#include <logger.h>

#include "switch_schema.h"
#include "switch_capabilities.h"

using namespace swss;

// defines ------------------------------------------------------------------------------------------------------------

#define SWITCH_CAPABILITY_HASH_NATIVE_HASH_FIELD_LIST_FIELD "HASH|NATIVE_HASH_FIELD_LIST"
#define SWITCH_CAPABILITY_ECMP_HASH_CAPABLE_FIELD           "ECMP_HASH_CAPABLE"
#define SWITCH_CAPABILITY_LAG_HASH_CAPABLE_FIELD            "LAG_HASH_CAPABLE"

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
    { SAI_NATIVE_HASH_FIELD_INNER_L4_SRC_PORT, SWITCH_HASH_FIELD_INNER_L4_SRC_PORT }
};

// variables ----------------------------------------------------------------------------------------------------------

extern sai_object_id_t gSwitchId;

// functions ----------------------------------------------------------------------------------------------------------

static std::string toStr(sai_object_type_t objType, sai_attr_id_t attrId) noexcept
{
    const auto *meta = sai_metadata_get_attr_metadata(objType, attrId);

    return meta != nullptr ? meta->attridname : "UNKNOWN";
}

static std::string toStr(const std::set<sai_native_hash_field_t> &value) noexcept
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

static std::string toStr(bool value) noexcept
{
    return value ? "true" : "false";
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

bool SwitchCapabilities::validateSwitchHashFieldCap(const std::set<sai_native_hash_field_t> &hfSet) const
{
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
            SWSS_LOG_ERROR("Failed to validate hash field: value(%s) is not supported");
            return false;
        }
    }

    return true;
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

void SwitchCapabilities::queryHashCapabilities()
{
    queryHashNativeHashFieldListEnumCapabilities();
    queryHashNativeHashFieldListAttrCapabilities();
}

void SwitchCapabilities::querySwitchEcmpHashCapabilities()
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

void SwitchCapabilities::querySwitchLagHashCapabilities()
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

void SwitchCapabilities::querySwitchCapabilities()
{
    querySwitchEcmpHashCapabilities();
    querySwitchLagHashCapabilities();
}

void SwitchCapabilities::writeHashCapabilitiesToDb()
{
    SWSS_LOG_ENTER();

    auto key = SwitchCapabilities::capTable.getKeyName(SWITCH_CAPABILITY_KEY);

    std::vector<FieldValueTuple> fvList = {
        makeHashFieldCapDbEntry()
    };

    SwitchCapabilities::capTable.set(SWITCH_CAPABILITY_KEY, fvList);

    SWSS_LOG_NOTICE("Wrote hash enum capabilities to State DB: %s key", key.c_str());
}

void SwitchCapabilities::writeSwitchCapabilitiesToDb()
{
    SWSS_LOG_ENTER();

    auto key = SwitchCapabilities::capTable.getKeyName(SWITCH_CAPABILITY_KEY);

    std::vector<FieldValueTuple> fvList = {
        makeEcmpHashCapDbEntry(),
        makeLagHashCapDbEntry()
    };

    SwitchCapabilities::capTable.set(SWITCH_CAPABILITY_KEY, fvList);

    SWSS_LOG_NOTICE("Wrote switch hash capabilities to State DB: %s key", key.c_str());
}
