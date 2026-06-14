#pragma once

#include <functional>

extern "C"
{
#include "saitypes.h"
}

/**
 * Test hooks for GNU ld --wrap of
 *   sai_query_attribute_enum_values_capability
 *   sai_metadata_get_attr_metadata
 * (orchagent/icmporch.cpp, IcmpOrch::resolve_stats_count_mode).
 * count mode selection.
 */
namespace icmporch_sai_wrap_ut
{
    void setIcmpSaiHookNone();

    void setIcmpSaiHookMetadataNull();

    void setIcmpSaiHookMetadataNotEnum();

    void setIcmpSaiHookQueryEnumFail();

    void setIcmpSaiHookQueryEnumEmptyList();

    void setIcmpSaiHookQueryEnumPacketAndByteOnly();

    struct IcmpSaiHookGuard
    {
        explicit IcmpSaiHookGuard(void (*apply)());
        ~IcmpSaiHookGuard();

        IcmpSaiHookGuard(const IcmpSaiHookGuard&) = delete;
        IcmpSaiHookGuard& operator=(const IcmpSaiHookGuard&) = delete;
    };
}

/*
 * Generic, test-settable override for the shared --wrap of
 * sai_query_attribute_enum_values_capability. Any mock test can program a
 * per-attribute enum-values result (status + value list) without disturbing the
 * icmp-specific hooks above. The override is consulted first; if unset, the wrap
 * falls back to the icmp hooks / __real_*.
 */
namespace sai_enum_cap_ut
{
    using EnumValuesCapabilityOverride = std::function<sai_status_t(
        sai_object_id_t switch_id,
        sai_object_type_t object_type,
        sai_attr_id_t attr_id,
        sai_s32_list_t *enum_values_capability)>;

    void setEnumValuesCapabilityOverride(EnumValuesCapabilityOverride fn);
    void clearEnumValuesCapabilityOverride();

    /** RAII: clears the override on scope exit. */
    struct EnumValuesCapabilityOverrideGuard
    {
        explicit EnumValuesCapabilityOverrideGuard(EnumValuesCapabilityOverride fn);
        ~EnumValuesCapabilityOverrideGuard();

        EnumValuesCapabilityOverrideGuard(const EnumValuesCapabilityOverrideGuard&) = delete;
        EnumValuesCapabilityOverrideGuard& operator=(const EnumValuesCapabilityOverrideGuard&) = delete;
    };
}
