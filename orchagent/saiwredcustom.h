/**
 * @file saiwredcustom.h
 * @brief Custom SAI WRED attributes for relative (percentage-based) thresholds.
 *
 * These vendor-extension attributes allow configuring WRED/ECN thresholds as
 * percentages of the allocated buffer, instead of absolute byte values.
 * The SAI/SDK translates percentages to hardware thresholds based on actual
 * port/TC buffer allocation.
 *
 * Attribute IDs are in the vendor-extension range (0x10000000+).
 * This header is a local shim until the definitions are upstreamed to SAI.
 */
#ifndef SAI_WRED_CUSTOM_H
#define SAI_WRED_CUSTOM_H

#include <sai.h>

typedef enum _sai_wred_threshold_mode_t {
    SAI_WRED_THRESHOLD_MODE_ABSOLUTE = 0,
    SAI_WRED_THRESHOLD_MODE_RELATIVE = 1,
} sai_wred_threshold_mode_t;

typedef enum _sai_wred_attr_custom_t {
    SAI_WRED_ATTR_THRESHOLD_MODE = 0x10000000,

    SAI_WRED_ATTR_GREEN_MIN_THRESHOLD_RELATIVE,
    SAI_WRED_ATTR_GREEN_MAX_THRESHOLD_RELATIVE,
    SAI_WRED_ATTR_YELLOW_MIN_THRESHOLD_RELATIVE,
    SAI_WRED_ATTR_YELLOW_MAX_THRESHOLD_RELATIVE,
    SAI_WRED_ATTR_RED_MIN_THRESHOLD_RELATIVE,
    SAI_WRED_ATTR_RED_MAX_THRESHOLD_RELATIVE,

    SAI_WRED_ATTR_ECN_GREEN_MIN_THRESHOLD_RELATIVE,
    SAI_WRED_ATTR_ECN_GREEN_MAX_THRESHOLD_RELATIVE,
    SAI_WRED_ATTR_ECN_YELLOW_MIN_THRESHOLD_RELATIVE,
    SAI_WRED_ATTR_ECN_YELLOW_MAX_THRESHOLD_RELATIVE,
    SAI_WRED_ATTR_ECN_RED_MIN_THRESHOLD_RELATIVE,
    SAI_WRED_ATTR_ECN_RED_MAX_THRESHOLD_RELATIVE,
} sai_wred_attr_custom_t;

#endif /* SAI_WRED_CUSTOM_H */
