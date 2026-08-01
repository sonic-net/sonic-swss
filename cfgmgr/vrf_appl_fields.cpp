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

void filterVrfApplFields(const std::vector<FieldValueTuple>& values,
                         std::vector<FieldValueTuple>& filtered)
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

    /* Metadata-only row (rd / rt_* / redistribute_*): keep the APPL_DB entry
     * present so VRFOrch still creates the virtual router. */
    if (filtered.empty())
    {
        filtered.emplace_back("NULL", "NULL");
    }
}

}
