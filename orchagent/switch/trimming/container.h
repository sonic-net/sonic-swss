#pragma once

extern "C" {
#include <saiswitch.h>
}

#include <unordered_map>
#include <string>

class SwitchTrimming final
{
public:
    SwitchTrimming() = default;
    ~SwitchTrimming() = default;

    struct {
        sai_uint32_t value;
        bool is_set = false;
    } size; // Trim packets to this size to reduce bandwidth

    struct {
        sai_uint8_t value;
        bool is_set = false;
    } dscp; // New packet trimming DSCP value

    struct {
        struct {
            sai_packet_trim_queue_resolution_mode_t value;
            bool is_set = false;
        } mode;
        struct {
            sai_uint8_t value;
            bool is_set = false;
        } index;
    } queue; // New packet trimming queue index

    std::unordered_map<std::string, std::string> fieldValueMap;
};
