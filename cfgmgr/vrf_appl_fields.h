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
 * CONFIG_DB rows reach vrfmgrd through SubscriberStateTable, which re-reads the
 * whole hash on every keyspace event, so `values` is always the complete VRF
 * row and a dropped field means the operator really removed it. The caller
 * therefore publishes unconditionally, exactly as it did before this filter
 * existed; only non-orchagent fields are withheld.
 *
 * `filtered` always ends up non-empty: a row carrying nothing but metadata
 * yields a NULL/NULL placeholder so the APPL_DB row still materializes.
 */
void filterVrfApplFields(const std::vector<FieldValueTuple>& values,
                         std::vector<FieldValueTuple>& filtered);

}

#endif
