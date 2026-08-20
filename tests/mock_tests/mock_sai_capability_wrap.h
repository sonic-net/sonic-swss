#pragma once

/*
 * GNU ld --wrap hooks for free-standing SAI capability/metadata functions
 * exercised by the orchagent unit tests.
 *
 * These functions are not reachable through the sai_*_api function-pointer
 * structs, so they cannot be mocked via the usual pointer-swap helpers in
 * mock_sai_api.h. Instead they are intercepted at link time with
 * -Wl,--wrap=<symbol>. Because each __wrap_<symbol> is a single definition for
 * the entire tests binary, all wraps live in one translation unit
 * (mock_sai_capability_wrap.cpp) rather than in per-orch files. Per-orch hook
 * state is exposed through the namespaces below.
 *
 * Wrapped symbols:
 *   sai_query_attribute_enum_values_capability  (icmporch.cpp)
 *   sai_metadata_get_attr_metadata              (icmporch.cpp)
 *   sai_query_stats_st_capability               (hftelorch.cpp)
 *   sai_query_attribute_capability              (hftelorch.cpp, and icmporch.cpp
 *                                                via the shared SaiOffloadSession
 *                                                selective-counter capability check)
 *
 * sai_query_attribute_capability is a single wrapped symbol shared by both
 * orchs, so its __wrap_ dispatches by object_type: ICMP echo session queries are
 * driven by the ICMP hook state, all other object types by the HFTel hook state.
 * Each orch therefore drives only its own namespace below.
 */

#include <functional>
#include <utility>

#include <saitypes.h>
#include <saiobject.h>

/**
 * Generic per-attribute capability override for the single shared
 * sai_query_attribute_capability() --wrap. Return true to handle the query
 * (filling *cap and *status); return false to fall through to the default
 * per-orch behaviour. Usable by any mock test (e.g. the ACL match-field gate).
 */
namespace sai_cap_ut
{
    using AttrCapabilityOverride = std::function<bool(
        sai_object_type_t object_type,
        sai_attr_id_t attr_id,
        sai_attr_capability_t *cap,
        sai_status_t *status)>;

    void setAttrCapabilityOverride(AttrCapabilityOverride fn);
    void clearAttrCapabilityOverride();

    /** RAII: clears the override on scope exit. */
    struct AttrCapabilityOverrideGuard
    {
        explicit AttrCapabilityOverrideGuard(AttrCapabilityOverride fn)
        {
            setAttrCapabilityOverride(std::move(fn));
        }
        ~AttrCapabilityOverrideGuard()
        {
            clearAttrCapabilityOverride();
        }

        AttrCapabilityOverrideGuard(const AttrCapabilityOverrideGuard &) = delete;
        AttrCapabilityOverrideGuard &operator=(const AttrCapabilityOverrideGuard &) = delete;
    };
}

/**
 * Test hooks for the SAI calls on the ICMP echo session stats count mode path
 * (orchagent/icmporch.cpp, IcmpOrch::resolve_stats_count_mode).
 */
namespace icmporch_sai_wrap_ut
{
    void setIcmpSaiHookNone();

    void setIcmpSaiHookMetadataNull();

    void setIcmpSaiHookMetadataNotEnum();

    void setIcmpSaiHookQueryEnumFail();

    void setIcmpSaiHookQueryEnumEmptyList();

    void setIcmpSaiHookQueryEnumPacketAndByteOnly();

    // Fails only the ICMP echo session selective-counter capability query
    // (sai_query_attribute_capability on SAI_OBJECT_TYPE_ICMP_ECHO_SESSION),
    // forcing IcmpOrch onto the native FlexCounter fallback. Queries on other
    // object types are forwarded to the real implementation.
    void setIcmpSaiHookSessionCapabilityQueryFail();

    /** RAII: restores all ICMP hook state to None on scope exit. */
    struct IcmpSaiHookGuard
    {
        explicit IcmpSaiHookGuard(void (*apply)());
        ~IcmpSaiHookGuard();

        IcmpSaiHookGuard(const IcmpSaiHookGuard&) = delete;
        IcmpSaiHookGuard& operator=(const IcmpSaiHookGuard&) = delete;
    };
}

/**
 * Test hooks for the SAI capability queries used by HFTelOrch::isSupportedHFTel
 * (orchagent/high_frequency_telemetry/hftelorch.cpp).
 */
namespace hftelorch_sai_wrap_ut
{
    void setSaiHookNone();
    void setSaiHookStatsStFail();
    void setSaiHookAttributeCapabilityQueryFail();
    void setSaiHookCollectorCreateNotImplemented();
    void setSaiHookSwitchNotifySetNotImplemented();
    void setSaiHookAllSupported();

    /** RAII: restores the HFTel hook to None on scope exit. */
    struct HFTelSaiHookGuard
    {
        explicit HFTelSaiHookGuard(void (*apply)());
        ~HFTelSaiHookGuard();

        HFTelSaiHookGuard(const HFTelSaiHookGuard&) = delete;
        HFTelSaiHookGuard& operator=(const HFTelSaiHookGuard&) = delete;
    };
}
