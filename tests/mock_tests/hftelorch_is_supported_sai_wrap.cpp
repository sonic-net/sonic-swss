/*
 * GNU ld --wrap symbols for sai_query_* used by HFTelOrch::isSupportedHFTel.
 * When no hook is active, forwards to __real_* so the rest of mock_tests is unchanged.
 */

#include "hftelorch_is_supported_sai_wrap.h"

#include <cstring>
#include <sai.h>

namespace
{
    enum class Hook
    {
        None = 0,
        StatsStFail,
        AttributeCapabilityQueryFail,
        CollectorCreateNotImplemented,
        SwitchNotifySetNotImplemented,
    };

    static thread_local Hook g_hook = Hook::None;
}

extern "C"
{

    sai_status_t __real_sai_query_stats_st_capability(
        _In_ sai_object_id_t switch_id,
        _In_ sai_object_type_t object_type,
        _Inout_ sai_stat_st_capability_list_t *stats_capability);

    sai_status_t __real_sai_query_attribute_capability(
        _In_ sai_object_id_t switch_id,
        _In_ sai_object_type_t object_type,
        _In_ sai_attr_id_t attr_id,
        _Out_ sai_attr_capability_t *attr_capability);

    sai_status_t __wrap_sai_query_stats_st_capability(
        _In_ sai_object_id_t switch_id,
        _In_ sai_object_type_t object_type,
        _Inout_ sai_stat_st_capability_list_t *stats_capability)
    {
        if (g_hook == Hook::StatsStFail)
        {
            return SAI_STATUS_NOT_SUPPORTED;
        }

        return __real_sai_query_stats_st_capability(switch_id, object_type, stats_capability);
    }

    sai_status_t __wrap_sai_query_attribute_capability(
        _In_ sai_object_id_t switch_id,
        _In_ sai_object_type_t object_type,
        _In_ sai_attr_id_t attr_id,
        _Out_ sai_attr_capability_t *attr_capability)
    {
        if (g_hook == Hook::None)
        {
            return __real_sai_query_attribute_capability(switch_id, object_type, attr_id, attr_capability);
        }

        if (g_hook == Hook::AttributeCapabilityQueryFail)
        {
            return SAI_STATUS_NOT_SUPPORTED;
        }

        if (g_hook == Hook::CollectorCreateNotImplemented)
        {
            if (!attr_capability)
            {
                return SAI_STATUS_INVALID_PARAMETER;
            }

            if (object_type == SAI_OBJECT_TYPE_TAM_COLLECTOR)
            {
                std::memset(attr_capability, 0, sizeof(*attr_capability));
                attr_capability->create_implemented = false;
                return SAI_STATUS_SUCCESS;
            }

            return __real_sai_query_attribute_capability(switch_id, object_type, attr_id, attr_capability);
        }

        if (g_hook == Hook::SwitchNotifySetNotImplemented)
        {
            if (!attr_capability)
            {
                return SAI_STATUS_INVALID_PARAMETER;
            }

            /*
             * isSupportedHFTel checks TAM_COLLECTOR attrs for create_implemented first.
             * Return synthetic success so we reach the SWITCH notify attribute.
             */
            if (object_type == SAI_OBJECT_TYPE_TAM_COLLECTOR)
            {
                std::memset(attr_capability, 0, sizeof(*attr_capability));
                attr_capability->create_implemented = true;
                return SAI_STATUS_SUCCESS;
            }

            if (object_type == SAI_OBJECT_TYPE_SWITCH &&
                attr_id == SAI_SWITCH_ATTR_TAM_TEL_TYPE_CONFIG_CHANGE_NOTIFY)
            {
                std::memset(attr_capability, 0, sizeof(*attr_capability));
                attr_capability->set_implemented = false;
                return SAI_STATUS_SUCCESS;
            }

            return __real_sai_query_attribute_capability(switch_id, object_type, attr_id, attr_capability);
        }

        return __real_sai_query_attribute_capability(switch_id, object_type, attr_id, attr_capability);
    }
}

namespace hftel_is_supported_ut
{
    void setSaiHookNone()
    {
        g_hook = Hook::None;
    }

    void setSaiHookStatsStFail()
    {
        g_hook = Hook::StatsStFail;
    }

    void setSaiHookAttributeCapabilityQueryFail()
    {
        g_hook = Hook::AttributeCapabilityQueryFail;
    }

    void setSaiHookCollectorCreateNotImplemented()
    {
        g_hook = Hook::CollectorCreateNotImplemented;
    }

    void setSaiHookSwitchNotifySetNotImplemented()
    {
        g_hook = Hook::SwitchNotifySetNotImplemented;
    }
}
