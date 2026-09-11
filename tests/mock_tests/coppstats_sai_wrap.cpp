/*
 * GNU ld --wrap shim for sai_query_stats_capability used by CoppOrch's
 * policer-stats probe. When no hook is active, forwards to __real_* so the
 * rest of mock_tests is unchanged.
 *
 * vslib's queryStatsCapability returns SAI_STATUS_NOT_SUPPORTED for
 * SAI_OBJECT_TYPE_POLICER, so the "unsupported" case is the default
 * (no hook needed). The other hooks inject synthetic capability lists so
 * we can exercise the supported / partial paths in CoppOrch.
 */

#include "coppstats_sai_wrap.h"

#include <cstring>
#include <sai.h>

namespace
{
    enum class Hook
    {
        None = 0,
        PolicerStatsAll,
        PolicerStatsPartial,        // PACKETS + BYTES only, no color counters
        PolicerStatsNotSupported,
    };

    static thread_local Hook g_hook = Hook::None;

    // Full SAI policer-stats set as advertised by a Broadcom XGS-class vendor.
    static const sai_policer_stat_t kAllPolicerStats[] = {
        SAI_POLICER_STAT_PACKETS,
        SAI_POLICER_STAT_ATTR_BYTES,
        SAI_POLICER_STAT_GREEN_PACKETS,
        SAI_POLICER_STAT_GREEN_BYTES,
        SAI_POLICER_STAT_YELLOW_PACKETS,
        SAI_POLICER_STAT_YELLOW_BYTES,
        SAI_POLICER_STAT_RED_PACKETS,
        SAI_POLICER_STAT_RED_BYTES,
    };

    // Partial set: vendor that supports the aggregate but not per-color.
    static const sai_policer_stat_t kPartialPolicerStats[] = {
        SAI_POLICER_STAT_PACKETS,
        SAI_POLICER_STAT_ATTR_BYTES,
    };

    template <size_t N>
    sai_status_t fillCapList(sai_stat_capability_list_t *cap_list,
                             const sai_policer_stat_t (&stats)[N])
    {
        // Two-call protocol: if the caller passed a list with insufficient
        // room (or list==nullptr), return BUFFER_OVERFLOW with the required
        // count. Otherwise fill the list and return SUCCESS.
        if (cap_list->list == nullptr || cap_list->count < N)
        {
            cap_list->count = static_cast<uint32_t>(N);
            return SAI_STATUS_BUFFER_OVERFLOW;
        }
        cap_list->count = static_cast<uint32_t>(N);
        for (size_t i = 0; i < N; i++)
        {
            cap_list->list[i].stat_enum  = static_cast<sai_stat_id_t>(stats[i]);
            cap_list->list[i].stat_modes = SAI_STATS_MODE_READ;
        }
        return SAI_STATUS_SUCCESS;
    }
}

extern "C"
{

    sai_status_t __real_sai_query_stats_capability(
        _In_ sai_object_id_t switch_id,
        _In_ sai_object_type_t object_type,
        _Inout_ sai_stat_capability_list_t *stats_capability);

    sai_status_t __wrap_sai_query_stats_capability(
        _In_ sai_object_id_t switch_id,
        _In_ sai_object_type_t object_type,
        _Inout_ sai_stat_capability_list_t *stats_capability)
    {
        if (g_hook == Hook::None || object_type != SAI_OBJECT_TYPE_POLICER)
        {
            return __real_sai_query_stats_capability(switch_id, object_type, stats_capability);
        }

        if (g_hook == Hook::PolicerStatsNotSupported)
        {
            return SAI_STATUS_NOT_SUPPORTED;
        }

        if (g_hook == Hook::PolicerStatsAll)
        {
            return fillCapList(stats_capability, kAllPolicerStats);
        }

        if (g_hook == Hook::PolicerStatsPartial)
        {
            return fillCapList(stats_capability, kPartialPolicerStats);
        }

        return __real_sai_query_stats_capability(switch_id, object_type, stats_capability);
    }
}

namespace copp_stats_ut
{
    void setSaiHookNone()                    { g_hook = Hook::None; }
    void setSaiHookPolicerStatsAll()         { g_hook = Hook::PolicerStatsAll; }
    void setSaiHookPolicerStatsPartial()     { g_hook = Hook::PolicerStatsPartial; }
    void setSaiHookPolicerStatsNotSupported(){ g_hook = Hook::PolicerStatsNotSupported; }
}
