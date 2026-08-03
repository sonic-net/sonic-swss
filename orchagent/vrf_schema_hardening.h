#ifndef __VRF_SCHEMA_HARDENING_H__
#define __VRF_SCHEMA_HARDENING_H__

#include "request_parser.h"

/*
 * Local VRF schema hardening (UPSW-6663).
 *
 * Kept in its own header so the only downmerge-sensitive change in
 * orchagent/vrforch.h is swapping VRFRequest for VRFRequestRelaxed.
 *
 * Parse in relaxed mode: an unrecognized field is skipped instead of
 * aborting the request. Strict parsing throws before addOperation() runs,
 * which discards the whole row - so one stray field left the VRF with no
 * virtual router and no L3 VNI binding at all.
 *
 * The attribute schema itself remains owned by vrforch.h
 * (request_description); this class only changes the parse policy.
 */
class VRFRequestRelaxed : public Request
{
public:
    explicit VRFRequestRelaxed(const request_description_t& desc)
        : Request(desc, ':', true) { }
};

#endif
