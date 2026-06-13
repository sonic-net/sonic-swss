#pragma once

#include <functional>
#include <utility>

#include <saitypes.h>
#include <saiobject.h>

namespace sai_cap_ut
{
    /*
     * Generic per-attribute capability override for the single shared
     * sai_query_attribute_capability() --wrap. Return true to handle the query
     * (filling *cap and *status); return false to fall through to default
     * behavior. Usable by any mock test.
     */
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
