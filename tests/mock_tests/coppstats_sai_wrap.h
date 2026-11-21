#pragma once

namespace copp_stats_ut
{
    void setSaiHookNone();
    void setSaiHookPolicerStatsAll();
    void setSaiHookPolicerStatsPartial();   // only PACKETS + BYTES
    void setSaiHookPolicerStatsNotSupported();

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
