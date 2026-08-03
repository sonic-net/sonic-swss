#ifndef __CONFIG_REQUEST_RELAXED_H__
#define __CONFIG_REQUEST_RELAXED_H__

#include "request_parser.h"

/*
 * Local hardening (UPSW-6663 follow-up).
 *
 * Rows that originate in CONFIG_DB — either read from it directly or copied
 * verbatim into APPL_DB by a *mgrd — routinely carry fields orchagent has no
 * schema for. Request::parse() throws on those fields and Orch2::doTask()
 * erases the entry, discarding the whole row rather than the offending field.
 *
 * Kept in its own header so the upstream Request subclasses
 * (VNetRequest, VxlanTunnelRequest, ...) stay byte-identical. Orch headers
 * only swap their request_ member to this wrapper, which keeps downmerges of
 * those files to a one-line type change.
 */
class ConfigFacingRequestRelaxed : public Request
{
public:
    ConfigFacingRequestRelaxed(const request_description_t& desc, char key_separator)
        : Request(desc, key_separator, true) { }
};

#endif
