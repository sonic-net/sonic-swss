#pragma once

namespace hftel_is_supported_ut
{
    void setSaiHookNone();
    void setSaiHookStatsStFail();
    void setSaiHookAttributeCapabilityQueryFail();
    // Fails only the ICMP echo session capability query (forces native
    // FlexCounter fallback) while leaving switch/other capability queries intact
    // so unrelated paths (e.g. state-change notification registration) succeed.
    void setSaiHookIcmpSessionCapabilityQueryFail();
    void setSaiHookCollectorCreateNotImplemented();
    void setSaiHookSwitchNotifySetNotImplemented();

    /** RAII: restores hook to None on scope exit. */
    struct SaiHookGuard
    {
        explicit SaiHookGuard(void (*apply)())
        {
            apply();
        }
        ~SaiHookGuard()
        {
            setSaiHookNone();
        }

        SaiHookGuard(const SaiHookGuard &) = delete;
        SaiHookGuard &operator=(const SaiHookGuard &) = delete;
    };
}
