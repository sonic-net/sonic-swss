#pragma once

#include "table.h"

namespace swss
{
// Private producer bookkeeping, not fields on the HLD Member/Group rows.
// Persist accepted keys so a process restart can withdraw its own empty or
// orphan Groups without enumerating/classifying the shared object table.
constexpr char FPM_PROTECTION_ID_TABLE[] = "_FPM_PROTECTION_OBJECT_IDS";
const std::string FPM_PROTECTION_MARKER_FIELD = "NULL";
static inline bool hasFpmProtectionId(Table &ids, const std::string &key)
{
    std::string value;
    return ids.hget(key, FPM_PROTECTION_MARKER_FIELD, value) && value == "NULL";
}
}
