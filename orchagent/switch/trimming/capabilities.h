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

    bool validateQueueModeCap(sai_packet_trim_queue_resolution_mode_t value) const;

private:
    swss::FieldValueTuple makeSwitchTrimmingCapDbEntry() const;
    swss::FieldValueTuple makeQueueModeCapDbEntry() const;

    sai_status_t queryEnumCapabilitiesSai(std::vector<sai_int32_t> &capList, sai_object_type_t objType, sai_attr_id_t attrId) const;
    sai_status_t queryAttrCapabilitiesSai(sai_attr_capability_t &attrCap, sai_object_type_t objType, sai_attr_id_t attrId) const;

    void queryTrimSizeAttrCapabilities();
    void queryTrimDscpAttrCapabilities();
    void queryTrimModeEnumCapabilities();
    void queryTrimModeAttrCapabilities();
    void queryTrimQueueAttrCapabilities();

    void queryCapabilities();
    void writeCapabilitiesToDb();

    struct {
        struct {
            bool isAttrSupported = false;
        } size; // SAI_SWITCH_ATTR_PACKET_TRIM_SIZE

        struct {
            bool isAttrSupported = false;
        } dscp; // SAI_SWITCH_ATTR_PACKET_TRIM_DSCP_VALUE

        struct {
            std::set<sai_packet_trim_queue_resolution_mode_t> mSet;
            bool isStaticModeSupported = true;
            bool isEnumSupported = false;
            bool isAttrSupported = false;
        } mode; // SAI_SWITCH_ATTR_PACKET_TRIM_QUEUE_RESOLUTION_MODE

        struct {
            bool isAttrSupported = false;
        } queue; // SAI_SWITCH_ATTR_PACKET_TRIM_QUEUE_INDEX
    } capabilities;
};
