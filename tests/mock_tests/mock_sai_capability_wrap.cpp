/*
 * GNU ld --wrap symbols for free-standing SAI capability/metadata functions
 * used by the orchagent unit tests. See mock_sai_capability_wrap.h for the
 * rationale and the list of wrapped symbols.
 *
 * When no hook is active each __wrap_* forwards to __real_*, so the rest of
 * mock_tests is unchanged. Hook state is thread_local and scoped per orch.
 */

#include "mock_sai_capability_wrap.h"

#include "saiobject.h"
#include "saiicmpecho.h"
#include "saimetadatautils.h"
#include "saistatus.h"
#include "saitypes.h"
#include <sai.h>

#include <cstddef>
#include <cstring>

namespace
{
    // ICMP echo session stats-count-mode path (IcmpOrch::resolve_stats_count_mode).
    namespace icmp
    {
        enum class Hook : int
        {
            None = 0,
            MetadataNull,
            MetadataNotEnum,
            QueryEnumFail,
            QueryEnumEmptyList,
            QueryEnumPacketAndByteOnly,
        };

        thread_local Hook g_hook = Hook::None;

        // Independent of g_hook (different wrapped symbol): when set, the ICMP
        // echo session selective-counter capability query
        // (sai_query_attribute_capability) fails, forcing native FlexCounter.
        thread_local bool g_fail_session_capability = false;
    }

    // HFTel capability discovery path (HFTelOrch::isSupportedHFTel).
    namespace hftel
    {
        enum class Hook
        {
            None = 0,
            StatsStFail,
            AttributeCapabilityQueryFail,
            CollectorCreateNotImplemented,
            SwitchNotifySetNotImplemented,
        };

        thread_local Hook g_hook = Hook::None;
    }
}

static const sai_attr_metadata_t g_nonEnumMetadataTest{};

extern "C"
{

    const sai_attr_metadata_t* __real_sai_metadata_get_attr_metadata(
            _In_ sai_object_type_t object_type,
            _In_ sai_attr_id_t attr_id);

    sai_status_t __real_sai_query_attribute_enum_values_capability(
            _In_ sai_object_id_t switch_id,
            _In_ sai_object_type_t object_type,
            _In_ sai_attr_id_t attr_id,
            _Inout_ sai_s32_list_t* enum_values_capability);

    sai_status_t __real_sai_query_stats_st_capability(
            _In_ sai_object_id_t switch_id,
            _In_ sai_object_type_t object_type,
            _Inout_ sai_stat_st_capability_list_t *stats_capability);

    sai_status_t __real_sai_query_attribute_capability(
            _In_ sai_object_id_t switch_id,
            _In_ sai_object_type_t object_type,
            _In_ sai_attr_id_t attr_id,
            _Out_ sai_attr_capability_t *attr_capability);

    const sai_attr_metadata_t* __wrap_sai_metadata_get_attr_metadata(
            _In_ sai_object_type_t object_type,
            _In_ sai_attr_id_t attr_id)
    {
        const bool is_icmp_stats_mode = (object_type == SAI_OBJECT_TYPE_ICMP_ECHO_SESSION)
                && (attr_id == SAI_ICMP_ECHO_SESSION_ATTR_STATS_COUNT_MODE);

        if (icmp::g_hook == icmp::Hook::MetadataNull && is_icmp_stats_mode)
        {
            return nullptr;
        }
        if (icmp::g_hook == icmp::Hook::MetadataNotEnum && is_icmp_stats_mode)
        {
            return &g_nonEnumMetadataTest;
        }
        return __real_sai_metadata_get_attr_metadata(object_type, attr_id);
    }

    sai_status_t __wrap_sai_query_attribute_enum_values_capability(
            _In_ sai_object_id_t switch_id,
            _In_ sai_object_type_t object_type,
            _In_ sai_attr_id_t attr_id,
            _Inout_ sai_s32_list_t* enum_values_capability)
    {
        const bool is_icmp_stats_mode = (object_type == SAI_OBJECT_TYPE_ICMP_ECHO_SESSION)
                && (attr_id == SAI_ICMP_ECHO_SESSION_ATTR_STATS_COUNT_MODE);

        if (icmp::g_hook == icmp::Hook::QueryEnumFail && is_icmp_stats_mode)
        {
            return SAI_STATUS_NOT_SUPPORTED;
        }

        if (icmp::g_hook == icmp::Hook::QueryEnumEmptyList && is_icmp_stats_mode)
        {
            if (enum_values_capability && enum_values_capability->list)
            {
                enum_values_capability->count = 0;
            }
            return SAI_STATUS_SUCCESS;
        }

        if (is_icmp_stats_mode && (icmp::g_hook == icmp::Hook::QueryEnumPacketAndByteOnly))
        {
            if (!enum_values_capability || !enum_values_capability->list
                    || enum_values_capability->count < 1)
            {
                if (enum_values_capability)
                {
                    enum_values_capability->count = 0;
                }
                return SAI_STATUS_BUFFER_OVERFLOW;
            }
            enum_values_capability->count = 1;
            enum_values_capability->list[0] = SAI_STATS_COUNT_MODE_PACKET_AND_BYTE;
            return SAI_STATUS_SUCCESS;
        }

        return __real_sai_query_attribute_enum_values_capability(
                switch_id, object_type, attr_id, enum_values_capability);
    }

    sai_status_t __wrap_sai_query_stats_st_capability(
            _In_ sai_object_id_t switch_id,
            _In_ sai_object_type_t object_type,
            _Inout_ sai_stat_st_capability_list_t *stats_capability)
    {
        if (hftel::g_hook == hftel::Hook::StatsStFail)
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
        // ICMP echo session selective-counter capability (IcmpOrch via the shared
        // SaiOffloadSession base). Fail only the ICMP echo session object; any
        // other object type falls through to the HFTel handling / real impl so
        // unrelated paths (e.g. state-change notification registration) succeed.
        if (icmp::g_fail_session_capability
                && object_type == SAI_OBJECT_TYPE_ICMP_ECHO_SESSION)
        {
            return SAI_STATUS_NOT_SUPPORTED;
        }

        if (hftel::g_hook == hftel::Hook::None)
        {
            return __real_sai_query_attribute_capability(switch_id, object_type, attr_id, attr_capability);
        }

        if (hftel::g_hook == hftel::Hook::AttributeCapabilityQueryFail)
        {
            return SAI_STATUS_NOT_SUPPORTED;
        }

        if (hftel::g_hook == hftel::Hook::CollectorCreateNotImplemented)
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

        if (hftel::g_hook == hftel::Hook::SwitchNotifySetNotImplemented)
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

namespace icmporch_sai_wrap_ut
{
    void setIcmpSaiHookNone()
    {
        icmp::g_hook = icmp::Hook::None;
        icmp::g_fail_session_capability = false;
    }

    void setIcmpSaiHookMetadataNull()
    {
        icmp::g_hook = icmp::Hook::MetadataNull;
    }

    void setIcmpSaiHookMetadataNotEnum()
    {
        icmp::g_hook = icmp::Hook::MetadataNotEnum;
    }

    void setIcmpSaiHookQueryEnumFail()
    {
        icmp::g_hook = icmp::Hook::QueryEnumFail;
    }

    void setIcmpSaiHookQueryEnumEmptyList()
    {
        icmp::g_hook = icmp::Hook::QueryEnumEmptyList;
    }

    void setIcmpSaiHookQueryEnumPacketAndByteOnly()
    {
        icmp::g_hook = icmp::Hook::QueryEnumPacketAndByteOnly;
    }

    void setIcmpSaiHookSessionCapabilityQueryFail()
    {
        icmp::g_fail_session_capability = true;
    }

    IcmpSaiHookGuard::IcmpSaiHookGuard(void (*apply)())
    {
        apply();
    }

    IcmpSaiHookGuard::~IcmpSaiHookGuard()
    {
        setIcmpSaiHookNone();
    }
}

namespace hftelorch_sai_wrap_ut
{
    void setSaiHookNone()
    {
        hftel::g_hook = hftel::Hook::None;
    }

    void setSaiHookStatsStFail()
    {
        hftel::g_hook = hftel::Hook::StatsStFail;
    }

    void setSaiHookAttributeCapabilityQueryFail()
    {
        hftel::g_hook = hftel::Hook::AttributeCapabilityQueryFail;
    }

    void setSaiHookCollectorCreateNotImplemented()
    {
        hftel::g_hook = hftel::Hook::CollectorCreateNotImplemented;
    }

    void setSaiHookSwitchNotifySetNotImplemented()
    {
        hftel::g_hook = hftel::Hook::SwitchNotifySetNotImplemented;
    }

    HFTelSaiHookGuard::HFTelSaiHookGuard(void (*apply)())
    {
        apply();
    }

    HFTelSaiHookGuard::~HFTelSaiHookGuard()
    {
        setSaiHookNone();
    }
}
