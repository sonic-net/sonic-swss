#pragma once

namespace hftel_is_supported_ut
{
    void setSaiHookNone();
    void setSaiHookStatsStFail();
    void setSaiHookAttributeCapabilityQueryFail();
    void setSaiHookCollectorCreateNotImplemented();
    void setSaiHookSwitchNotifySetNotImplemented();
    void setSaiHookAllSupported();

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
