// includes -----------------------------------------------------------------------------------------------------------

extern "C" {
#include <saitypes.h>
#include <saiobject.h>
#include <saistatus.h>
#include <saiswitch.h>
}

#include <cstdint>
#include <cstdbool>
#include <string>
#include <vector>
#include <set>
#include <unordered_map>
#include <algorithm>

#include <sai_serialize.h>
#include <stringutility.h>
#include <dbconnector.h>
#include <table.h>
#include <schema.h>
#include <logger.h>

#include "schema.h"
#include "capabilities.h"

using namespace swss;

// defines ------------------------------------------------------------------------------------------------------------

#define CAPABILITY_SWITCH_QUEUE_RESOLUTION_MODE_FIELD "SWITCH|PACKET_TRIMMING_QUEUE_RESOLUTION_MODE"

#define CAPABILITY_SWITCH_TRIMMING_CAPABLE_FIELD "SWITCH_TRIMMING_CAPABLE"

#define CAPABILITY_KEY "switch"

#define SWITCH_STATE_DB_NAME    "STATE_DB"
#define SWITCH_STATE_DB_TIMEOUT 0

// constants ----------------------------------------------------------------------------------------------------------

static const std::unordered_map<sai_packet_trim_queue_resolution_mode_t, std::string> modeMap =
{
    { SAI_PACKET_TRIM_QUEUE_RESOLUTION_MODE_STATIC,  SWITCH_TRIMMING_QUEUE_MODE_STATIC  },
    { SAI_PACKET_TRIM_QUEUE_RESOLUTION_MODE_DYNAMIC, SWITCH_TRIMMING_QUEUE_MODE_DYNAMIC }
};

// variables ----------------------------------------------------------------------------------------------------------

extern sai_object_id_t gSwitchId;

// functions ----------------------------------------------------------------------------------------------------------

static std::string toStr(sai_object_type_t objType, sai_attr_id_t attrId)
{
    const auto *meta = sai_metadata_get_attr_metadata(objType, attrId);

    return meta != nullptr ? meta->attridname : "UNKNOWN";
}

static std::string toStr(sai_packet_trim_queue_resolution_mode_t value)
{
    const auto *name = sai_metadata_get_packet_trim_queue_resolution_mode_name(value);

    return name != nullptr ? name : "UNKNOWN";
}

static std::string toStr(const std::set<sai_packet_trim_queue_resolution_mode_t> &value)
{
    std::vector<std::string> strList;

    for (const auto &cit1 : value)
    {
        const auto &cit2 = modeMap.find(cit1);
        if (cit2 != modeMap.cend())
        {
            strList.push_back(cit2->second);
        }
    }

    return join(",", strList.cbegin(), strList.cend());
}

static std::string toStr(bool value)
{
    return value ? "true" : "false";
}

// capabilities -------------------------------------------------------------------------------------------------------

SwitchTrimmingCapabilities::SwitchTrimmingCapabilities()
{
    queryCapabilities();
    writeCapabilitiesToDb();
}

bool SwitchTrimmingCapabilities::isSwitchTrimmingSupported() const
{
    auto size = capabilities.size.isAttrSupported;
    auto dscp = capabilities.dscp.isAttrSupported;
    auto mode = capabilities.mode.isAttrSupported;
    auto queue = true;

    // Do not care of queue index configuration capabilities,
    // if static queue resolution mode is not supported
    if (capabilities.mode.isStaticModeSupported)
    {
        queue = capabilities.queue.isAttrSupported;
    }

    return size || dscp || mode || queue;
}

bool SwitchTrimmingCapabilities::isSwitchTrimmingSizeSetSupported() const
{
    return capabilities.size.isAttrSupported;
}

bool SwitchTrimmingCapabilities::isSwitchTrimmingDscpSetSupported() const
{
    return capabilities.dscp.isAttrSupported;
}

bool SwitchTrimmingCapabilities::isSwitchTrimmingQueueSetSupported() const
{
    return capabilities.mode.isStaticModeSupported && capabilities.queue.isAttrSupported;
}

bool SwitchTrimmingCapabilities::isSwitchTrimmingModeSetSupported() const
{
    return capabilities.mode.isAttrSupported;
}

bool SwitchTrimmingCapabilities::validateQueueModeCap(sai_packet_trim_queue_resolution_mode_t value) const
{
    SWSS_LOG_ENTER();

    if (!capabilities.mode.isEnumSupported)
    {
        return true;
    }

    if (capabilities.mode.mSet.empty())
    {
        SWSS_LOG_ERROR("Failed to validate queue resolution mode: no capabilities");
        return false;
    }

    if (capabilities.mode.mSet.count(value) == 0)
    {
        SWSS_LOG_ERROR("Failed to validate queue resolution mode: value(%s) is not supported", toStr(value).c_str());
        return false;
    }

    return true;
}

sai_status_t SwitchTrimmingCapabilities::queryEnumCapabilitiesSai(std::vector<sai_int32_t> &capList, sai_object_type_t objType, sai_attr_id_t attrId) const
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

sai_status_t SwitchTrimmingCapabilities::queryAttrCapabilitiesSai(sai_attr_capability_t &attrCap, sai_object_type_t objType, sai_attr_id_t attrId) const
{
    return sai_query_attribute_capability(gSwitchId, objType, attrId, &attrCap);
}

void SwitchTrimmingCapabilities::queryTrimSizeAttrCapabilities()
{
    SWSS_LOG_ENTER();

    sai_attr_capability_t attrCap;

    auto status = queryAttrCapabilitiesSai(
        attrCap, SAI_OBJECT_TYPE_SWITCH, SAI_SWITCH_ATTR_PACKET_TRIM_SIZE
    );
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR(
            "Failed to get attribute(%s) capabilities",
            toStr(SAI_OBJECT_TYPE_SWITCH, SAI_SWITCH_ATTR_PACKET_TRIM_SIZE).c_str()
        );
        return;
    }

    if (!attrCap.set_implemented)
    {
        SWSS_LOG_WARN(
            "Attribute(%s) SET is not implemented in SAI",
            toStr(SAI_OBJECT_TYPE_SWITCH, SAI_SWITCH_ATTR_PACKET_TRIM_SIZE).c_str()
        );
        return;
    }

    capabilities.size.isAttrSupported = true;
}

void SwitchTrimmingCapabilities::queryTrimDscpAttrCapabilities()
{
    SWSS_LOG_ENTER();

    sai_attr_capability_t attrCap;

    auto status = queryAttrCapabilitiesSai(
        attrCap, SAI_OBJECT_TYPE_SWITCH, SAI_SWITCH_ATTR_PACKET_TRIM_DSCP_VALUE
    );
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR(
            "Failed to get attribute(%s) capabilities",
            toStr(SAI_OBJECT_TYPE_SWITCH, SAI_SWITCH_ATTR_PACKET_TRIM_DSCP_VALUE).c_str()
        );
        return;
    }

    if (!attrCap.set_implemented)
    {
        SWSS_LOG_WARN(
            "Attribute(%s) SET is not implemented in SAI",
            toStr(SAI_OBJECT_TYPE_SWITCH, SAI_SWITCH_ATTR_PACKET_TRIM_DSCP_VALUE).c_str()
        );
        return;
    }

    capabilities.dscp.isAttrSupported = true;
}

void SwitchTrimmingCapabilities::queryTrimModeEnumCapabilities()
{
    SWSS_LOG_ENTER();

    std::vector<sai_int32_t> mList;
    auto status = queryEnumCapabilitiesSai(
        mList, SAI_OBJECT_TYPE_SWITCH, SAI_SWITCH_ATTR_PACKET_TRIM_QUEUE_RESOLUTION_MODE
    );
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR(
            "Failed to get attribute(%s) enum value capabilities",
            toStr(SAI_OBJECT_TYPE_SWITCH, SAI_SWITCH_ATTR_PACKET_TRIM_QUEUE_RESOLUTION_MODE).c_str()
        );
        return;
    }

    auto &mSet = capabilities.mode.mSet;
    std::transform(
        mList.cbegin(), mList.cend(), std::inserter(mSet, mSet.begin()),
        [](sai_int32_t value) { return static_cast<sai_packet_trim_queue_resolution_mode_t>(value); }
    );

    if (mSet.empty() || (mSet.count(SAI_PACKET_TRIM_QUEUE_RESOLUTION_MODE_STATIC) == 0))
    {
        capabilities.mode.isStaticModeSupported = false;
    }

    capabilities.mode.isEnumSupported = true;
}

void SwitchTrimmingCapabilities::queryTrimModeAttrCapabilities()
{
    SWSS_LOG_ENTER();

    sai_attr_capability_t attrCap;

    auto status = queryAttrCapabilitiesSai(
        attrCap, SAI_OBJECT_TYPE_SWITCH, SAI_SWITCH_ATTR_PACKET_TRIM_QUEUE_RESOLUTION_MODE
    );
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR(
            "Failed to get attribute(%s) capabilities",
            toStr(SAI_OBJECT_TYPE_SWITCH, SAI_SWITCH_ATTR_PACKET_TRIM_QUEUE_RESOLUTION_MODE).c_str()
        );
        return;
    }

    if (!attrCap.set_implemented)
    {
        SWSS_LOG_WARN(
            "Attribute(%s) SET is not implemented in SAI",
            toStr(SAI_OBJECT_TYPE_SWITCH, SAI_SWITCH_ATTR_PACKET_TRIM_QUEUE_RESOLUTION_MODE).c_str()
        );
        return;
    }

    capabilities.mode.isAttrSupported = true;
}

void SwitchTrimmingCapabilities::queryTrimQueueAttrCapabilities()
{
    SWSS_LOG_ENTER();

    sai_attr_capability_t attrCap;

    auto status = queryAttrCapabilitiesSai(
        attrCap, SAI_OBJECT_TYPE_SWITCH, SAI_SWITCH_ATTR_PACKET_TRIM_QUEUE_INDEX
    );
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR(
            "Failed to get attribute(%s) capabilities",
            toStr(SAI_OBJECT_TYPE_SWITCH, SAI_SWITCH_ATTR_PACKET_TRIM_QUEUE_INDEX).c_str()
        );
        return;
    }

    if (!attrCap.set_implemented)
    {
        SWSS_LOG_WARN(
            "Attribute(%s) SET is not implemented in SAI",
            toStr(SAI_OBJECT_TYPE_SWITCH, SAI_SWITCH_ATTR_PACKET_TRIM_QUEUE_INDEX).c_str()
        );
        return;
    }

    capabilities.queue.isAttrSupported = true;
}

void SwitchTrimmingCapabilities::queryCapabilities()
{
    queryTrimSizeAttrCapabilities();
    queryTrimDscpAttrCapabilities();

    queryTrimModeEnumCapabilities();
    queryTrimModeAttrCapabilities();

    queryTrimQueueAttrCapabilities();
}

FieldValueTuple SwitchTrimmingCapabilities::makeSwitchTrimmingCapDbEntry() const
{
    auto field = CAPABILITY_SWITCH_TRIMMING_CAPABLE_FIELD;
    auto value = toStr(isSwitchTrimmingSupported());

    return FieldValueTuple(field, value);
}

FieldValueTuple SwitchTrimmingCapabilities::makeQueueModeCapDbEntry() const
{
    auto field = CAPABILITY_SWITCH_QUEUE_RESOLUTION_MODE_FIELD;
    auto value = capabilities.mode.isEnumSupported ? toStr(capabilities.mode.mSet) : "N/A";

    return FieldValueTuple(field, value);
}

void SwitchTrimmingCapabilities::writeCapabilitiesToDb()
{
    SWSS_LOG_ENTER();

    DBConnector stateDb(SWITCH_STATE_DB_NAME, SWITCH_STATE_DB_TIMEOUT);
    Table capTable(&stateDb, STATE_SWITCH_CAPABILITY_TABLE_NAME);

    std::vector<FieldValueTuple> fvList = {
        makeSwitchTrimmingCapDbEntry(),
        makeQueueModeCapDbEntry()
    };

    capTable.set(CAPABILITY_KEY, fvList);

    SWSS_LOG_NOTICE(
        "Wrote switch trimming capabilities to State DB: %s key",
        capTable.getKeyName(CAPABILITY_KEY).c_str()
    );
}
