#pragma once

extern "C" {
#include <saitypes.h>
#include <saiobject.h>
#include <saiswitch.h>
}

#include <vector>
#include <set>

class SwitchTrimmingCapabilities final
{
public:
    SwitchTrimmingCapabilities();
    ~SwitchTrimmingCapabilities() = default;

    bool isSwitchTrimmingSupported() const;

    bool validateTrimDscpModeCap(sai_packet_trim_dscp_resolution_mode_t value) const;
    bool validateTrimTcCap(sai_uint8_t value) const;
    bool validateTrimQueueModeCap(sai_packet_trim_queue_resolution_mode_t value) const;
    bool validateQueueIndexCap(sai_uint32_t value) const;

private:
    swss::FieldValueTuple makeSwitchTrimmingCapDbEntry() const;
    swss::FieldValueTuple makeDscpModeCapDbEntry() const;
    swss::FieldValueTuple makeQueueModeCapDbEntry() const;
    swss::FieldValueTuple makeTrafficClassNumberCapDbEntry() const;
    swss::FieldValueTuple makeUnicastQueueNumberCapDbEntry() const;

    sai_status_t queryEnumCapabilitiesSai(std::vector<sai_int32_t> &capList, sai_object_type_t objType, sai_attr_id_t attrId) const;
    sai_status_t queryAttrCapabilitiesSai(sai_attr_capability_t &attrCap, sai_object_type_t objType, sai_attr_id_t attrId) const;

    void queryTrimSizeAttrCapabilities();
    void queryTrimDscpModeEnumCapabilities();
    void queryTrimDscpModeAttrCapabilities();
    void queryTrimDscpAttrCapabilities();
    void queryTrimTcAttrCapabilities();
    void queryTrimQueueModeEnumCapabilities();
    void queryTrimQueueModeAttrCapabilities();
    void queryTrimQueueIndexAttrCapabilities();
    void queryTrimTrafficClassNumberAttrCapabilities();
    void queryTrimUnicastQueueNumberAttrCapabilities();

    void queryCapabilities();
    void writeCapabilitiesToDb();

    struct {
        struct {
            bool isAttrSupported = false;
        } size; // SAI_SWITCH_ATTR_PACKET_TRIM_SIZE

        struct {
            struct {
                std::set<sai_packet_trim_dscp_resolution_mode_t> mSet;
                bool isDscpValueModeSupported = true;
                bool isFromTcModeSupported = true;
                bool isEnumSupported = false;
                bool isAttrSupported = false;
            } mode; // SAI_SWITCH_ATTR_PACKET_TRIM_DSCP_RESOLUTION_MODE

            bool isAttrSupported = false;
        } dscp; // SAI_SWITCH_ATTR_PACKET_TRIM_DSCP_VALUE

        struct {
            bool isAttrSupported = false;
        } tc; // SAI_SWITCH_ATTR_PACKET_TRIM_TC_VALUE

        struct {
            struct {
                std::set<sai_packet_trim_queue_resolution_mode_t> mSet;
                bool isStaticModeSupported = true;
                bool isEnumSupported = false;
                bool isAttrSupported = false;
            } mode; // SAI_SWITCH_ATTR_PACKET_TRIM_QUEUE_RESOLUTION_MODE

            struct {
                bool isAttrSupported = false;
            } index; // SAI_SWITCH_ATTR_PACKET_TRIM_QUEUE_INDEX
        } queue;
    } trimCap;

    struct
    {
        struct {
            sai_uint8_t value;
            bool is_set = false;
            bool isAttrSupported = false;
        } tcNum; // SAI_SWITCH_ATTR_QOS_MAX_NUMBER_OF_TRAFFIC_CLASSES

        struct {
            sai_uint32_t value;
            bool is_set = false;
            bool isAttrSupported = false;
        } uqNum; // SAI_SWITCH_ATTR_NUMBER_OF_UNICAST_QUEUES
    } genCap;
};
