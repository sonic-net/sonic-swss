#ifndef __VRF_APPL_FIELDS_H__
#define __VRF_APPL_FIELDS_H__

#include <set>
#include <string>

namespace swss {

/*
 * Fields that vrfmgrd forwards from CONFIG_DB VRF|<name> to APPL_DB
 * VRF_TABLE:<name>.
 *
 * APPL_DB VRF_TABLE is orchagent's interface, so it must carry only fields
 * VRFOrch can act on. Control-plane metadata written onto the same CONFIG_DB
 * row (BGP route-distinguisher, route-targets, redistribution) belongs to
 * frrcfgd and must not reach orchagent.
 *
 * This set must match request_description in orchagent/vrforch.h exactly;
 * VRFApplSchema.VrfApplForwardFieldsMatchOrchSchema enforces that.
 */
inline const std::set<std::string>& vrfApplForwardFields()
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

}

#endif
