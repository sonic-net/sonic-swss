#pragma once

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
