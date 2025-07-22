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

#define CAPABILITY_SWITCH_DSCP_RESOLUTION_MODE_FIELD  "SWITCH|PACKET_TRIMMING_DSCP_RESOLUTION_MODE"
#define CAPABILITY_SWITCH_QUEUE_RESOLUTION_MODE_FIELD "SWITCH|PACKET_TRIMMING_QUEUE_RESOLUTION_MODE"
#define CAPABILITY_SWITCH_NUMBER_OF_TRAFFIC_CLASSES_FIELD "SWITCH|NUMBER_OF_TRAFFIC_CLASSES"
#define CAPABILITY_SWITCH_NUMBER_OF_UNICAST_QUEUES_FIELD "SWITCH|NUMBER_OF_UNICAST_QUEUES"

#define CAPABILITY_SWITCH_TRIMMING_CAPABLE_FIELD "SWITCH_TRIMMING_CAPABLE"

#define CAPABILITY_KEY "switch"

#define SWITCH_STATE_DB_NAME    "STATE_DB"
#define SWITCH_STATE_DB_TIMEOUT 0

// constants ----------------------------------------------------------------------------------------------------------

static const std::unordered_map<sai_packet_trim_dscp_resolution_mode_t, std::string> dscpModeMap =
{
    { SAI_PACKET_TRIM_DSCP_RESOLUTION_MODE_DSCP_VALUE, SWITCH_TRIMMING_DSCP_MODE_DSCP_VALUE },
    { SAI_PACKET_TRIM_DSCP_RESOLUTION_MODE_FROM_TC,    SWITCH_TRIMMING_DSCP_MODE_FROM_TC    }
};

static const std::unordered_map<sai_packet_trim_queue_resolution_mode_t, std::string> queueModeMap =
{
    { SAI_PACKET_TRIM_QUEUE_RESOLUTION_MODE_STATIC,  SWITCH_TRIMMING_QUEUE_MODE_STATIC  },
    { SAI_PACKET_TRIM_QUEUE_RESOLUTION_MODE_DYNAMIC, SWITCH_TRIMMING_QUEUE_MODE_DYNAMIC }
};

// variables ----------------------------------------------------------------------------------------------------------

extern sai_object_id_t gSwitchId;
extern sai_switch_api_t *sai_switch_api;

// functions ----------------------------------------------------------------------------------------------------------

static std::string toStr(sai_object_type_t objType, sai_attr_id_t attrId)
{
    const auto *meta = sai_metadata_get_attr_metadata(objType, attrId);

    return meta != nullptr ? meta->attridname : "UNKNOWN";
}

static std::string toStr(sai_packet_trim_dscp_resolution_mode_t value)
{
    const auto *name = sai_metadata_get_packet_trim_dscp_resolution_mode_name(value);

    return name != nullptr ? name : "UNKNOWN";
}

static std::string toStr(const std::set<sai_packet_trim_dscp_resolution_mode_t> &value)
{
    std::vector<std::string> strList;

    for (const auto &cit1 : value)
    {
        const auto &cit2 = dscpModeMap.find(cit1);
        if (cit2 != dscpModeMap.cend())
        {
            strList.push_back(cit2->second);
        }
    }

    return join(",", strList.cbegin(), strList.cend());
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
        const auto &cit2 = queueModeMap.find(cit1);
        if (cit2 != queueModeMap.cend())
        {
            strList.push_back(cit2->second);
        }
    }

    return join(",", strList.cbegin(), strList.cend());
}

static std::string toStr(sai_uint8_t value)
{
    return std::to_string(value);
}

static std::string toStr(sai_uint32_t value)
{
    return std::to_string(value);
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
    auto size = trimCap.size.isAttrSupported;
    auto dscpMode = trimCap.dscp.mode.isAttrSupported;
    auto dscp = true;
    auto tc = true;
    auto queueMode = trimCap.queue.mode.isAttrSupported;
    auto queueIndex = true;

    // Do not care of dscp configuration capabilities,
    // if DSCP_VALUE dscp resolution mode is not supported
    if (trimCap.dscp.mode.isDscpValueModeSupported)
    {
        dscp = trimCap.dscp.isAttrSupported;
    }

    // Do not care of tc configuration capabilities,
    // if FROM_TC dscp resolution mode is not supported
    if (trimCap.dscp.mode.isFromTcModeSupported)
    {
        tc = trimCap.tc.isAttrSupported;
    }

    // Do not care of queue index configuration capabilities,
    // if STATIC queue resolution mode is not supported
    if (trimCap.queue.mode.isStaticModeSupported)
    {
        queueIndex = trimCap.queue.index.isAttrSupported;
    }

    return size && dscpMode && dscp && tc && queueMode && queueIndex;
}

bool SwitchTrimmingCapabilities::validateTrimDscpModeCap(sai_packet_trim_dscp_resolution_mode_t value) const
{
    SWSS_LOG_ENTER();

    if (!trimCap.dscp.mode.isEnumSupported)
    {
        return true;
    }

    if (trimCap.dscp.mode.mSet.empty())
    {
        SWSS_LOG_ERROR("Failed to validate dscp resolution mode: no capabilities");
        return false;
    }

    if (trimCap.dscp.mode.mSet.count(value) == 0)
    {
        SWSS_LOG_ERROR("Failed to validate dscp resolution mode: value(%s) is not supported", toStr(value).c_str());
        return false;
    }

    return true;
}

bool SwitchTrimmingCapabilities::validateTrimTcCap(sai_uint8_t value) const
{
    SWSS_LOG_ENTER();

    if (!genCap.tcNum.isAttrSupported)
    {
        return true;
    }

    auto maxTC = genCap.tcNum.value - 1;

    if (!(value <= maxTC))
    {
        SWSS_LOG_ERROR(
            "Failed to validate traffic class: value(%u) is out of range: 0 <= class <= %u",
            value, maxTC
        );
        return false;
    }

    return true;
}

bool SwitchTrimmingCapabilities::validateTrimQueueModeCap(sai_packet_trim_queue_resolution_mode_t value) const
{
    SWSS_LOG_ENTER();

    if (!trimCap.queue.mode.isEnumSupported)
    {
        return true;
    }

    if (trimCap.queue.mode.mSet.empty())
    {
        SWSS_LOG_ERROR("Failed to validate queue resolution mode: no capabilities");
        return false;
    }

    if (trimCap.queue.mode.mSet.count(value) == 0)
    {
        SWSS_LOG_ERROR("Failed to validate queue resolution mode: value(%s) is not supported", toStr(value).c_str());
        return false;
    }

    return true;
}

bool SwitchTrimmingCapabilities::validateQueueIndexCap(sai_uint32_t value) const
{
    SWSS_LOG_ENTER();

    if (!genCap.uqNum.isAttrSupported)
    {
        return true;
    }

    auto maxUQIdx = genCap.uqNum.value - 1;

    if (!(value <= maxUQIdx))
    {
        SWSS_LOG_ERROR(
            "Failed to validate queue index: value(%u) is out of range: 0 <= index <= %u",
            value, maxUQIdx
        );
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

    trimCap.size.isAttrSupported = true;
}

void SwitchTrimmingCapabilities::queryTrimDscpModeEnumCapabilities()
{
    SWSS_LOG_ENTER();

    std::vector<sai_int32_t> mList;
    auto status = queryEnumCapabilitiesSai(
        mList, SAI_OBJECT_TYPE_SWITCH, SAI_SWITCH_ATTR_PACKET_TRIM_DSCP_RESOLUTION_MODE
    );
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR(
            "Failed to get attribute(%s) enum value capabilities",
            toStr(SAI_OBJECT_TYPE_SWITCH, SAI_SWITCH_ATTR_PACKET_TRIM_DSCP_RESOLUTION_MODE).c_str()
        );
        return;
    }

    auto &mSet = trimCap.dscp.mode.mSet;
    std::transform(
        mList.cbegin(), mList.cend(), std::inserter(mSet, mSet.begin()),
        [](sai_int32_t value) { return static_cast<sai_packet_trim_dscp_resolution_mode_t>(value); }
    );

    if (!mSet.empty())
    {
        if (mSet.count(SAI_PACKET_TRIM_DSCP_RESOLUTION_MODE_DSCP_VALUE) == 0)
        {
            trimCap.dscp.mode.isDscpValueModeSupported = false;
        }

        if (mSet.count(SAI_PACKET_TRIM_DSCP_RESOLUTION_MODE_FROM_TC) == 0)
        {
            trimCap.dscp.mode.isFromTcModeSupported = false;
        }
    }
    else
    {
        trimCap.dscp.mode.isDscpValueModeSupported = false;
        trimCap.dscp.mode.isFromTcModeSupported = false;
    }

    trimCap.dscp.mode.isEnumSupported = true;
}

void SwitchTrimmingCapabilities::queryTrimDscpModeAttrCapabilities()
{
    SWSS_LOG_ENTER();

    sai_attr_capability_t attrCap;

    auto status = queryAttrCapabilitiesSai(
        attrCap, SAI_OBJECT_TYPE_SWITCH, SAI_SWITCH_ATTR_PACKET_TRIM_DSCP_RESOLUTION_MODE
    );
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR(
            "Failed to get attribute(%s) capabilities",
            toStr(SAI_OBJECT_TYPE_SWITCH, SAI_SWITCH_ATTR_PACKET_TRIM_DSCP_RESOLUTION_MODE).c_str()
        );
        return;
    }

    if (!attrCap.set_implemented)
    {
        SWSS_LOG_WARN(
            "Attribute(%s) SET is not implemented in SAI",
            toStr(SAI_OBJECT_TYPE_SWITCH, SAI_SWITCH_ATTR_PACKET_TRIM_DSCP_RESOLUTION_MODE).c_str()
        );
        return;
    }

    trimCap.dscp.mode.isAttrSupported = true;
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

    trimCap.dscp.isAttrSupported = true;
}

void SwitchTrimmingCapabilities::queryTrimTcAttrCapabilities()
{
    SWSS_LOG_ENTER();

    sai_attr_capability_t attrCap;

    auto status = queryAttrCapabilitiesSai(
        attrCap, SAI_OBJECT_TYPE_SWITCH, SAI_SWITCH_ATTR_PACKET_TRIM_TC_VALUE
    );
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR(
            "Failed to get attribute(%s) capabilities",
            toStr(SAI_OBJECT_TYPE_SWITCH, SAI_SWITCH_ATTR_PACKET_TRIM_TC_VALUE).c_str()
        );
        return;
    }

    if (!attrCap.set_implemented)
    {
        SWSS_LOG_WARN(
            "Attribute(%s) SET is not implemented in SAI",
            toStr(SAI_OBJECT_TYPE_SWITCH, SAI_SWITCH_ATTR_PACKET_TRIM_TC_VALUE).c_str()
        );
        return;
    }

    trimCap.tc.isAttrSupported = true;
}

void SwitchTrimmingCapabilities::queryTrimQueueModeEnumCapabilities()
{
    SWSS_LOG_ENTER();

    std::vector<sai_int32_t> mList;
    auto status = queryEnumCapabilitiesSai(
        mList, SAI_OBJECT_TYPE_SWITCH, SAI_SWITCH_ATTR_PACKET_TRIM_QUEUE_RESOLUTION_MODE
    );
    if (status == SAI_STATUS_NOT_SUPPORTED)
    {
        SWSS_LOG_NOTICE(
            "Attribute not supported(%s)enum value capabilities",
            toStr(SAI_OBJECT_TYPE_SWITCH, SAI_SWITCH_ATTR_PACKET_TRIM_QUEUE_RESOLUTION_MODE).c_str()
        );
        return;
    }
    else if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR(
            "Failed to get attribute(%s) enum value capabilities",
            toStr(SAI_OBJECT_TYPE_SWITCH, SAI_SWITCH_ATTR_PACKET_TRIM_QUEUE_RESOLUTION_MODE).c_str()
        );
        return;
    }

    auto &mSet = trimCap.queue.mode.mSet;
    std::transform(
        mList.cbegin(), mList.cend(), std::inserter(mSet, mSet.begin()),
        [](sai_int32_t value) { return static_cast<sai_packet_trim_queue_resolution_mode_t>(value); }
    );

    if (mSet.empty() || (mSet.count(SAI_PACKET_TRIM_QUEUE_RESOLUTION_MODE_STATIC) == 0))
    {
        trimCap.queue.mode.isStaticModeSupported = false;
    }

    trimCap.queue.mode.isEnumSupported = true;
}

void SwitchTrimmingCapabilities::queryTrimQueueModeAttrCapabilities()
{
    SWSS_LOG_ENTER();

    sai_attr_capability_t attrCap;

    auto status = queryAttrCapabilitiesSai(
        attrCap, SAI_OBJECT_TYPE_SWITCH, SAI_SWITCH_ATTR_PACKET_TRIM_QUEUE_RESOLUTION_MODE
    );
    if (status == SAI_STATUS_NOT_SUPPORTED)
    {
        SWSS_LOG_NOTICE(
            "Attribute not supported(%s)enum value capabilities",
            toStr(SAI_OBJECT_TYPE_SWITCH, SAI_SWITCH_ATTR_PACKET_TRIM_QUEUE_RESOLUTION_MODE).c_str()
        );
        return;
    }
    else if (status != SAI_STATUS_SUCCESS)
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

    trimCap.queue.mode.isAttrSupported = true;
}

void SwitchTrimmingCapabilities::queryTrimQueueIndexAttrCapabilities()
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

    trimCap.queue.index.isAttrSupported = true;
}

void SwitchTrimmingCapabilities::queryTrimTrafficClassNumberAttrCapabilities()
{
    SWSS_LOG_ENTER();

    sai_attr_capability_t attrCap;

    auto status = queryAttrCapabilitiesSai(
        attrCap, SAI_OBJECT_TYPE_SWITCH, SAI_SWITCH_ATTR_QOS_MAX_NUMBER_OF_TRAFFIC_CLASSES
    );
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR(
            "Failed to get attribute(%s) capabilities",
            toStr(SAI_OBJECT_TYPE_SWITCH, SAI_SWITCH_ATTR_QOS_MAX_NUMBER_OF_TRAFFIC_CLASSES).c_str()
        );
        return;
    }

    if (!attrCap.get_implemented)
    {
        SWSS_LOG_WARN(
            "Attribute(%s) GET is not implemented in SAI",
            toStr(SAI_OBJECT_TYPE_SWITCH, SAI_SWITCH_ATTR_QOS_MAX_NUMBER_OF_TRAFFIC_CLASSES).c_str()
        );
        return;
    }

    sai_attribute_t attr;
    attr.id = SAI_SWITCH_ATTR_QOS_MAX_NUMBER_OF_TRAFFIC_CLASSES;

    status = sai_switch_api->get_switch_attribute(gSwitchId, 1, &attr);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR(
            "Failed to get attribute(%s) value",
            toStr(SAI_OBJECT_TYPE_SWITCH, SAI_SWITCH_ATTR_QOS_MAX_NUMBER_OF_TRAFFIC_CLASSES).c_str()
        );
        return;
    }

    if (attr.value.u8 == 0)
    {
        SWSS_LOG_WARN(
            "Unexpected attribute(%s) value: traffic classes are not supported",
            toStr(SAI_OBJECT_TYPE_SWITCH, SAI_SWITCH_ATTR_QOS_MAX_NUMBER_OF_TRAFFIC_CLASSES).c_str()
        );
        return;
    }

    genCap.tcNum.isAttrSupported = true;
    genCap.tcNum.value = attr.value.u8;
}

void SwitchTrimmingCapabilities::queryTrimUnicastQueueNumberAttrCapabilities()
{
    SWSS_LOG_ENTER();

    sai_attr_capability_t attrCap;

    auto status = queryAttrCapabilitiesSai(
        attrCap, SAI_OBJECT_TYPE_SWITCH, SAI_SWITCH_ATTR_NUMBER_OF_UNICAST_QUEUES
    );
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR(
            "Failed to get attribute(%s) capabilities",
            toStr(SAI_OBJECT_TYPE_SWITCH, SAI_SWITCH_ATTR_NUMBER_OF_UNICAST_QUEUES).c_str()
        );
        return;
    }

    if (!attrCap.get_implemented)
    {
        SWSS_LOG_WARN(
            "Attribute(%s) GET is not implemented in SAI",
            toStr(SAI_OBJECT_TYPE_SWITCH, SAI_SWITCH_ATTR_NUMBER_OF_UNICAST_QUEUES).c_str()
        );
        return;
    }

    sai_attribute_t attr;
    attr.id = SAI_SWITCH_ATTR_NUMBER_OF_UNICAST_QUEUES;

    status = sai_switch_api->get_switch_attribute(gSwitchId, 1, &attr);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR(
            "Failed to get attribute(%s) value",
            toStr(SAI_OBJECT_TYPE_SWITCH, SAI_SWITCH_ATTR_NUMBER_OF_UNICAST_QUEUES).c_str()
        );
        return;
    }

    if (attr.value.u32 == 0)
    {
        SWSS_LOG_WARN(
            "Unexpected attribute(%s) value: unicast queues are not supported",
            toStr(SAI_OBJECT_TYPE_SWITCH, SAI_SWITCH_ATTR_NUMBER_OF_UNICAST_QUEUES).c_str()
        );
        return;
    }

    genCap.uqNum.isAttrSupported = true;
    genCap.uqNum.value = attr.value.u32;
}

void SwitchTrimmingCapabilities::queryCapabilities()
{
    queryTrimSizeAttrCapabilities();

    queryTrimDscpModeEnumCapabilities();
    queryTrimDscpModeAttrCapabilities();

    queryTrimDscpAttrCapabilities();
    queryTrimTcAttrCapabilities();

    queryTrimQueueModeEnumCapabilities();
    queryTrimQueueModeAttrCapabilities();

    queryTrimQueueIndexAttrCapabilities();

    queryTrimTrafficClassNumberAttrCapabilities();
    queryTrimUnicastQueueNumberAttrCapabilities();
}

FieldValueTuple SwitchTrimmingCapabilities::makeSwitchTrimmingCapDbEntry() const
{
    auto field = CAPABILITY_SWITCH_TRIMMING_CAPABLE_FIELD;
    auto value = toStr(isSwitchTrimmingSupported());

    return FieldValueTuple(field, value);
}

FieldValueTuple SwitchTrimmingCapabilities::makeDscpModeCapDbEntry() const
{
    auto field = CAPABILITY_SWITCH_DSCP_RESOLUTION_MODE_FIELD;
    auto value = trimCap.dscp.mode.isEnumSupported ? toStr(trimCap.dscp.mode.mSet) : "N/A";

    return FieldValueTuple(field, value);
}

FieldValueTuple SwitchTrimmingCapabilities::makeQueueModeCapDbEntry() const
{
    auto field = CAPABILITY_SWITCH_QUEUE_RESOLUTION_MODE_FIELD;
    auto value = trimCap.queue.mode.isEnumSupported ? toStr(trimCap.queue.mode.mSet) : "N/A";

    return FieldValueTuple(field, value);
}

FieldValueTuple SwitchTrimmingCapabilities::makeTrafficClassNumberCapDbEntry() const
{
    auto field = CAPABILITY_SWITCH_NUMBER_OF_TRAFFIC_CLASSES_FIELD;
    auto value = genCap.tcNum.isAttrSupported ? toStr(genCap.tcNum.value) : "N/A";

    return FieldValueTuple(field, value);
}

FieldValueTuple SwitchTrimmingCapabilities::makeUnicastQueueNumberCapDbEntry() const
{
    auto field = CAPABILITY_SWITCH_NUMBER_OF_UNICAST_QUEUES_FIELD;
    auto value = genCap.uqNum.isAttrSupported ? toStr(genCap.uqNum.value) : "N/A";

    return FieldValueTuple(field, value);
}

void SwitchTrimmingCapabilities::writeCapabilitiesToDb()
{
    SWSS_LOG_ENTER();

    DBConnector stateDb(SWITCH_STATE_DB_NAME, SWITCH_STATE_DB_TIMEOUT);
    Table capTable(&stateDb, STATE_SWITCH_CAPABILITY_TABLE_NAME);

    std::vector<FieldValueTuple> fvList = {
        makeSwitchTrimmingCapDbEntry(),
        makeDscpModeCapDbEntry(),
        makeQueueModeCapDbEntry(),
        makeTrafficClassNumberCapDbEntry(),
        makeUnicastQueueNumberCapDbEntry()
    };

    capTable.set(CAPABILITY_KEY, fvList);

    SWSS_LOG_NOTICE(
        "Wrote switch trimming capabilities to State DB: %s key",
        capTable.getKeyName(CAPABILITY_KEY).c_str()
    );
}
