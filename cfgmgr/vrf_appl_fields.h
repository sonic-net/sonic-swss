#ifndef __VRF_APPL_FIELDS_H__
#define __VRF_APPL_FIELDS_H__

#include <set>
#include <string>
#include <vector>

#include "table.h"

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
 *
 * Kept in its own translation unit so upstream downmerges of vrfmgr.cpp /
 * vrforch.h stay free of this local hardening.
 */
const std::set<std::string>& vrfApplForwardFields();

/*
 * Filter CONFIG_DB VRF fields down to the orchagent allowlist.
 *
 * Returns true when the caller should publish to APPL_DB VRF_TABLE.
 * Returns false when the SET carried only non-orchagent metadata: publishing
 * that as an empty/NULL row would make VRFOrch treat missing `vni` as 0 and
 * tear down an existing L3 VNI map on the update path.
 *
 * On first create (publish_placeholder_if_empty=true) an empty orchagent field
 * set still publishes a NULL/NULL placeholder so the VRF row materializes.
 */
bool filterVrfApplFields(const std::vector<FieldValueTuple>& values,
                         std::vector<FieldValueTuple>& filtered,
                         bool publish_placeholder_if_empty);

}

#endif
