#pragma once

#include <string>
#include <cstring>
#include "logger.h"

/*
 * Parse and validate an ASIC instance ID string.
 * Truncates to max_len if the input exceeds it, and sets truncated = true.
 */
inline std::string parseAsicInstanceId(const char *input, size_t max_len, bool &truncated)
{
    if (input == nullptr)
    {
        truncated = false;
        return std::string();
    }

    size_t len = strnlen(input, max_len);
    truncated = (len == max_len && input[len] != '\0');
    return std::string(input, len);
}

inline std::string parseAsicInstanceIdWithLimit(const char *input, size_t max_len)
{
    bool truncated = false;
    std::string instance_id = parseAsicInstanceId(input, max_len, truncated);
    if (truncated)
    {
        SWSS_LOG_WARN("ASIC instance_id length > SAI_MAX_HARDWARE_ID_LEN, LIMITING !!");
    }
    return instance_id;
}
