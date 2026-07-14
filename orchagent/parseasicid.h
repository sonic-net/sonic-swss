#pragma once

#include <string>
#include <cstring>

/*
 * Parse and validate an ASIC instance ID string.
 * Truncates to max_len if the input exceeds it, and sets truncated = true.
 */
inline std::string parseAsicInstanceId(const char *input, size_t max_len, bool &truncated)
{
    size_t len = strnlen(input, max_len);
    truncated = (len == max_len && input[len] != '\0');
    return std::string(input, len);
}
