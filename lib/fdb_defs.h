#pragma once

#include <cstdint>

/*
 * Shared FDB-related definitions used by both orchagent (fdborch) and
 * fdbsyncd, so both sides agree on the on-wire/in-memory representation.
 */

/* Destination kind for an FDB (MAC) entry. */
enum class FdbDest : uint8_t {
    UNKNOWN = 0,
    VTEP = 1,
    NEXTHOPGROUP = 2,
    IFNAME = 3,
};
