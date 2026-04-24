/*
 * GNU ld --wrap for SAI calls on the ICMP echo session stats count mode path
 * (orchagent/icmporch.cpp, IcmpSaiSessionHandler::do_create).
 */

#include "icmporch_sai_wrap.h"

#include "saiobject.h"
#include "saiicmpecho.h"
#include "saimetadatautils.h"
#include "saistatus.h"
#include "saitypes.h"

#include <cstddef>

namespace
{
    enum class Hook : int
    {
        None = 0,
        MetadataNull,
        MetadataNotEnum,
        QueryEnumFail,
        QueryEnumByteOnly,
        QueryEnumPacketOnly
    };

    static thread_local Hook g_hook = Hook::None;
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

    const sai_attr_metadata_t* __wrap_sai_metadata_get_attr_metadata(
            _In_ sai_object_type_t object_type,
            _In_ sai_attr_id_t attr_id)
    {
        const bool is_icmp_stats_mode = (object_type == SAI_OBJECT_TYPE_ICMP_ECHO_SESSION)
                && (attr_id == SAI_ICMP_ECHO_SESSION_ATTR_STATS_COUNT_MODE);

        if (g_hook == Hook::MetadataNull && is_icmp_stats_mode)
        {
            return nullptr;
        }
        if (g_hook == Hook::MetadataNotEnum && is_icmp_stats_mode)
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

        if (g_hook == Hook::QueryEnumFail && is_icmp_stats_mode)
        {
            return SAI_STATUS_NOT_SUPPORTED;
        }

        if (is_icmp_stats_mode && (g_hook == Hook::QueryEnumByteOnly))
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
            enum_values_capability->list[0] = SAI_STATS_COUNT_MODE_BYTE;
            return SAI_STATUS_SUCCESS;
        }

        if (is_icmp_stats_mode && (g_hook == Hook::QueryEnumPacketOnly))
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
            enum_values_capability->list[0] = SAI_STATS_COUNT_MODE_PACKET;
            return SAI_STATUS_SUCCESS;
        }

        return __real_sai_query_attribute_enum_values_capability(
                switch_id, object_type, attr_id, enum_values_capability);
    }
}

namespace icmporch_sai_wrap_ut
{
    void setIcmpSaiHookNone()
    {
        g_hook = Hook::None;
    }

    void setIcmpSaiHookMetadataNull()
    {
        g_hook = Hook::MetadataNull;
    }

    void setIcmpSaiHookMetadataNotEnum()
    {
        g_hook = Hook::MetadataNotEnum;
    }

    void setIcmpSaiHookQueryEnumFail()
    {
        g_hook = Hook::QueryEnumFail;
    }

    void setIcmpSaiHookQueryEnumByteOnlyNoPacketModes()
    {
        g_hook = Hook::QueryEnumByteOnly;
    }

    void setIcmpSaiHookQueryEnumPacketOnly()
    {
        g_hook = Hook::QueryEnumPacketOnly;
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
