#include "vrf_appl_fields.h"

#include "logger.h"

namespace swss {

const std::set<std::string>& vrfApplForwardFields()
{
    static const std::set<std::string> fields = {
        "v4",
        "v6",
        "src_mac",
        "ttl_action",
        "ip_opt_action",
        "l3_mc_action",
        "fallback",
        "vni",
        "mgmtVrfEnabled",
        "in_band_mgmt_enabled",
    };

    return fields;
}

bool filterVrfApplFields(const std::vector<FieldValueTuple>& values,
                         std::vector<FieldValueTuple>& filtered,
                         bool publish_placeholder_if_empty)
{
    const auto& allowed = vrfApplForwardFields();
    filtered.clear();

    for (const auto& fv : values)
    {
        if (allowed.count(fvField(fv)))
        {
            filtered.push_back(fv);
        }
        else
        {
            SWSS_LOG_INFO("Not forwarding VRF field '%s' to APPL_DB", fvField(fv).c_str());
        }
    }

    if (!filtered.empty())
    {
        return true;
    }

    /*
     * Metadata-only SET (rd / rt_* / redistribute_*). Publishing NULL:NULL on
     * an existing VRF makes VRFOrch's update path see vni=0 and delete the L3
     * VNI map. Only emit a placeholder when the VRF row must be created.
     */
    if (publish_placeholder_if_empty)
    {
        filtered.emplace_back("NULL", "NULL");
        return true;
    }

    return false;
}

}
