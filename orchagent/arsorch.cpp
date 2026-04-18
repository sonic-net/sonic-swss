#include "arsorch.h"
#include "routeorch.h"
#include "logger.h"
#include "schema.h"
#include "tokenize.h"
#include "sai_serialize.h"
#include "converter.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <exception>

using namespace std;
using namespace swss;

extern sai_ars_api_t*            sai_ars_api;
extern sai_ars_profile_api_t*    sai_ars_profile_api;
extern sai_switch_api_t*         sai_switch_api;
extern sai_next_hop_group_api_t* sai_next_hop_group_api;
extern sai_lag_api_t*            sai_lag_api;
extern sai_port_api_t*                  sai_port_api;
extern sai_router_interface_api_t*      sai_router_intfs_api;
extern sai_object_id_t                  gSwitchId;
extern RouteOrch                       *gRouteOrch;

static const map<string, sai_ars_mode_t> arsModeLookup = {
    {"flowlet-quality",  SAI_ARS_MODE_FLOWLET_QUALITY},
    {"packet-quality",   SAI_ARS_MODE_PER_PACKET_QUALITY},
};

// Clamp a u32 CONFIG_DB value to the u8 range expected by several
// SAI_ARS_PROFILE_ATTR_* attributes (PORT_LOAD_PAST/FUTURE_WEIGHT,
// PORT_LOAD_EXPONENT). A raw stoul() parse can easily produce values
// >255 (e.g. an operator mistyping a scaling factor as a weight); the
// SAI wrapper truncates silently, which is an easy way to set e.g.
// load_past_weight=256 and end up with 0 in the ASIC. Clamp + warn
// instead so the value the operator sees in STATE_DB matches SAI.
static uint32_t clampToU8(const string &profile, const string &field, uint32_t val)
{
    if (val > 255)
    {
        SWSS_LOG_WARN("ARS: profile '%s' field '%s' value %u exceeds u8 "
                      "range; clamping to 255 (SAI attribute is u8).",
                      profile.c_str(), field.c_str(), val);
        return 255;
    }
    return val;
}

// Lowercase a string in-place copy for case-insensitive comparisons of
// CONFIG_DB enum-ish values (e.g. algorithm "EWMA" vs "ewma", admin_state
// "up" vs "UP"). Keeps the caller logic simple.
static string toLower(string s)
{
    // Explicit narrowing cast to char — std::tolower takes/returns int, and
    // assigning an int directly to a char iterator is flagged as
    // -Wnarrowing by some toolchains. This keeps the lambda well-formed
    // under -Wall -Wextra without suppressing the diagnostic.
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return s;
}

static bool isFlowletMode(sai_ars_mode_t mode);

ArsOrch::ArsOrch(DBConnector *configDb,
                 DBConnector *stateDb,
                 const vector<string> &tableNames,
                 SwitchOrch *switchOrch,
                 PortsOrch  *portsOrch)
    : Orch(configDb, tableNames),
      m_switchOrch(switchOrch),
      m_portsOrch(portsOrch),
      m_stateArsCapTable(stateDb, STATE_ARS_CAPABILITY_TABLE_NAME),
      m_stateArsProfileTable(stateDb, STATE_ARS_PROFILE_TABLE_NAME),
      m_stateArsNhgTable(stateDb, STATE_ARS_NHG_TABLE_NAME),
      // Match declaration order in arsorch.h (m_cfgArsObjectTable before
      // m_stateArsObjectTable before m_cfgArsTable). Keeping the init list
      // in declaration order is what -Wreorder checks for, and swss builds
      // with -Wall -Wextra -Werror (see configure.ac), so any mismatch
      // here fails the entire orchagent build. Do NOT reorder without
      // also updating the header.
      m_cfgArsObjectTable(configDb, CFG_ARS_OBJECT_TABLE_NAME),
      m_stateArsObjectTable(stateDb, STATE_ARS_OBJECT_TABLE_NAME),
      m_cfgArsTable(configDb, CFG_ARS_TABLE_NAME)
{
    SWSS_LOG_ENTER();
    m_portsOrch->attach(this);
    publishArsCaps();
}

void ArsOrch::update(SubjectType type, void *cntx)
{
    if (type != SUBJECT_TYPE_PORT_OPER_STATE_CHANGE)
        return;

    auto *stateUpdate = reinterpret_cast<PortOperStateUpdate *>(cntx);
    if (stateUpdate->operStatus != SAI_PORT_OPER_STATUS_UP)
        return;

    const string &portName = stateUpdate->port.m_alias;
    auto it = m_arsInterfaces.find(portName);
    if (it == m_arsInterfaces.end())
        return;

    const auto &entry = it->second;
    if (!entry.enabled || entry.portProfile.empty())
        return;

    auto ppIt = m_arsPortProfiles.find(entry.portProfile);
    if (ppIt == m_arsPortProfiles.end())
        return;

    const auto &pp = ppIt->second;
    if (!pp.loadScalingFactorAuto)
        return;

    SWSS_LOG_NOTICE("ARS: port %s came up (speed=%u), re-applying auto scaling factor",
                    portName.c_str(), stateUpdate->port.m_speed);
    applyPortProfileToInterface(portName, entry.portProfile);
}

void ArsOrch::doTask(Consumer &consumer)
{
    SWSS_LOG_ENTER();

    const string &tableName = consumer.getTableName();

    if (tableName == CFG_ARS_TABLE_NAME)
        doArsGlobalTask(consumer);
    else if (tableName == CFG_ARS_PROFILE_TABLE_NAME)
        doArsProfileTask(consumer);
    else if (tableName == CFG_ARS_OBJECT_TABLE_NAME)
        doArsObjectTask(consumer);
    else if (tableName == CFG_ARS_INTERFACES_TABLE_NAME)
        doArsInterfaceTask(consumer);
    else if (tableName == CFG_ARS_PORT_PROFILE_TABLE_NAME)
        doArsPortProfileTask(consumer);
    else if (tableName == CFG_ARS_NEXTHOPS_TABLE_NAME)
        doArsNexthopsTask(consumer);
    else if (tableName == CFG_ARS_PORTCHANNELS_TABLE_NAME)
        doArsPortChannelTask(consumer);
    else
        SWSS_LOG_ERROR("ArsOrch: unknown table %s", tableName.c_str());
}

/* ── Global enable/disable ───────────────────────────────────────────── */

void ArsOrch::doArsGlobalTask(Consumer &consumer)
{
    SWSS_LOG_ENTER();

    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
        auto &kfv = it->second;
        string key = kfvKey(kfv);
        string op  = kfvOp(kfv);

        if (key != "GLOBAL")
        {
            it = consumer.m_toSync.erase(it);
            continue;
        }

        if (op == SET_COMMAND)
        {
            bool wantEnable = false;
            string profileName;
            for (auto &fv : kfvFieldsValues(kfv))
            {
                if (fvField(fv) == "admin_state")
                    wantEnable = (toLower(fvValue(fv)) == "up");
                else if (fvField(fv) == "profile")
                    profileName = fvValue(fv);
            }

            if (wantEnable && !m_arsEnabled)
            {
                SWSS_LOG_NOTICE("ARS: Adaptive Routing globally enabled");
                m_arsEnabled = true;

                // Per the user guide (docs/07.USER_GUIDE_ECMP.md and
                // docs/08.USER_GUIDE_FLOWLET.md §"Quick Reference: Config
                // Order Matters") adaptive routing requires
                // SAI_NEXT_HOP_GROUP_TYPE_DYNAMIC_ORDERED_ECMP — i.e. the
                // operator must have configured `ecmp type ordered` before
                // enabling ARS. If the switch is still set to static ECMP
                // we let the configuration proceed (the vendor SAI may
                // auto-switch the underlying SDK type to ADAPTIVE_E when
                // an ARS object is bound) but emit a loud warning so the
                // operator can correlate any unexpected behavior with the
                // missing prerequisite.
                if (m_switchOrch && !m_switchOrch->checkOrderedEcmpEnable())
                {
                    SWSS_LOG_WARN("ARS: Adaptive Routing enabled while ECMP "
                                  "type is 'static' — the documented "
                                  "prerequisite is `ecmp type ordered` "
                                  "(SWITCH_HASH|GLOBAL.ecmp_type=ordered). "
                                  "Existing NHGs built with static ECMP may "
                                  "need to be re-created for ARS bindings "
                                  "to take effect.");
                }

                if (profileName.empty() && m_globalProfileName.empty() &&
                    m_activeSwitchProfileOid == SAI_NULL_OBJECT_ID)
                {
                    createDefaultProfileIfNeeded();
                }

                // Re-apply ARS to the data plane: per-port enables, LAG
                // bindings, and NHG re-evaluation for any routes that were
                // installed while ARS was disabled.
                enableArsDataPlane();
            }
            else if (!wantEnable && m_arsEnabled)
            {
                SWSS_LOG_NOTICE("ARS: Adaptive Routing globally disabled");
                // Tear down the data plane *before* flipping the flag so the
                // helpers still treat ARS as "enabled" while iterating.
                disableArsDataPlane();
                m_arsEnabled = false;
            }

            if (!profileName.empty() && profileName != m_globalProfileName)
            {
                auto profIt = m_arsProfiles.find(profileName);
                if (profIt != m_arsProfiles.end() &&
                    profIt->second.profileOid != SAI_NULL_OBJECT_ID)
                {
                    if (bindArsProfileToSwitch(profIt->second.profileOid))
                    {
                        m_globalProfileName = profileName;
                        SWSS_LOG_NOTICE("ARS: bound profile '%s' to switch",
                                        profileName.c_str());
                        if (gRouteOrch && m_arsEnabled)
                            gRouteOrch->rebindArsForAllNhgs();
                    }
                    else
                    {
                        SWSS_LOG_ERROR("ARS: failed to bind profile '%s' to switch",
                                       profileName.c_str());
                    }
                }
                else
                {
                    m_globalProfileName = profileName;
                    SWSS_LOG_NOTICE("ARS: profile '%s' requested but not yet "
                                    "created — will bind when available",
                                    profileName.c_str());
                }
            }
            else if (profileName.empty() && !m_globalProfileName.empty())
            {
                if (bindArsProfileToSwitch(SAI_NULL_OBJECT_ID))
                {
                    m_globalProfileName.clear();
                    SWSS_LOG_NOTICE("ARS: unbound profile from switch");
                    if (gRouteOrch && m_arsEnabled)
                        gRouteOrch->rebindArsForAllNhgs();
                }
                else
                {
                    SWSS_LOG_ERROR("ARS: failed to unbind profile from switch");
                }
            }
        }
        else if (op == DEL_COMMAND)
        {
            if (m_arsEnabled)
            {
                SWSS_LOG_NOTICE("ARS: Adaptive Routing global entry removed — disabling");
                disableArsDataPlane();
                m_arsEnabled = false;
            }
            if (m_activeSwitchProfileOid != SAI_NULL_OBJECT_ID)
            {
                bindArsProfileToSwitch(SAI_NULL_OBJECT_ID);
                m_globalProfileName.clear();
            }
        }

        it = consumer.m_toSync.erase(it);
    }
}

/* ── ARS Profile (EWMA tuning) ──────────────────────────────────────── */

void ArsOrch::doArsProfileTask(Consumer &consumer)
{
    SWSS_LOG_ENTER();

    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
        auto &kfv = it->second;
        string name = kfvKey(kfv);
        string op   = kfvOp(kfv);

        if (op == SET_COMMAND)
        {
            // Reserve names beginning with '__ARS_' for orchagent-internal
            // use (see createDefaultProfileIfNeeded). A user-supplied profile
            // with the same name would clash with the auto-created default
            // and silently replace it in m_arsProfiles, causing hard-to-debug
            // mismatches between CONFIG_DB and SAI.
            if (name.rfind("__ARS_", 0) == 0)
            {
                SWSS_LOG_ERROR("ARS: profile name '%s' is reserved (prefix "
                               "'__ARS_' is orchagent-internal); rejecting",
                               name.c_str());
                it = consumer.m_toSync.erase(it);
                continue;
            }

            ArsProfileEntry entry;
            const bool isCreate = (m_arsProfiles.count(name) == 0);
            if (!isCreate)
                entry = m_arsProfiles[name];

            // Track which EWMA-related fields appeared in *this* update so we
            // can only derive defaults for fields the operator didn't touch.
            // Previously a single explicitLoadCurrent flag guarded a recompute
            // of loadCurrentEnable that used entry.loadCurrentWeight — which
            // was seeded from the prior cached state. That meant an unrelated
            // update (say sampling_interval) could re-derive loadCurrentEnable
            // from a stale weight and silently flip it.
            //
            // Also capture the pre-existing quant-band state before the
            // kfvFieldsValues loop potentially overwrites it. We need this so
            // a transition from a previously-configured triple (e.g. 10/20/30)
            // to all-zero actually writes the zeroes through to SAI —
            // otherwise the device retains the old thresholds while
            // CONFIG_DB / m_arsProfiles claim they are cleared.
            const bool hadQuantBandConfig = (entry.quantBand0MinThreshold |
                                             entry.quantBand1MinThreshold |
                                             entry.quantBand2MinThreshold) != 0;

            bool explicitLoadCurrent = false;
            bool explicitLoadCurrentWeight = false;
            bool rejectProfile = false;
            for (auto &fv : kfvFieldsValues(kfv))
            {
                const string &field = fvField(fv);
                const string &value = fvValue(fv);

                try
                {
                if      (field == "port_load_past_weight" || field == "load_past_weight")
                    entry.loadPastWeight   = clampToU8(name, field,
                                                       static_cast<uint32_t>(stoul(value)));
                else if (field == "port_load_future_weight" || field == "load_future_weight")
                    entry.loadFutureWeight = clampToU8(name, field,
                                                       static_cast<uint32_t>(stoul(value)));
                else if (field == "port_load_current_weight" || field == "load_current_weight")
                {
                    entry.loadCurrentWeight = clampToU8(name, field,
                                                        static_cast<uint32_t>(stoul(value)));
                    explicitLoadCurrentWeight = true;
                }
                else if (field == "load_exponent" || field == "port_load_exponent")
                    entry.loadExponent     = clampToU8(name, field,
                                                       static_cast<uint32_t>(stoul(value)));
                else if (field == "max_flows")           entry.maxFlows          = static_cast<uint32_t>(stoul(value));
                else if (field == "load_past_min_val")   entry.loadPastMinVal    = static_cast<uint32_t>(stoul(value));
                else if (field == "load_past_max_val")   entry.loadPastMaxVal    = static_cast<uint32_t>(stoul(value));
                else if (field == "load_future_min_val") entry.loadFutureMinVal  = static_cast<uint32_t>(stoul(value));
                else if (field == "load_future_max_val") entry.loadFutureMaxVal  = static_cast<uint32_t>(stoul(value));
                else if (field == "load_current_min_val") entry.loadCurrentMinVal = static_cast<uint32_t>(stoul(value));
                else if (field == "load_current_max_val") entry.loadCurrentMaxVal = static_cast<uint32_t>(stoul(value));
                else if (field == "port_load_past")      entry.loadPastEnable    = (value == "true");
                else if (field == "port_load_future")    entry.loadFutureEnable  = (value == "true");
                else if (field == "port_load_current")   { entry.loadCurrentEnable = (value == "true"); explicitLoadCurrent = true; }
                else if (field == "ipv4_enable")         entry.ipv4Enable        = (value == "true");
                else if (field == "ipv6_enable")         entry.ipv6Enable        = (value == "true");
                else if (field == "sampling_interval")   entry.samplingInterval  = static_cast<uint32_t>(stoul(value));
                else if (field == "random_seed")         entry.randomSeed        = static_cast<uint32_t>(stoul(value));
                else if (field == "algorithm")
                {
                    // EWMA is the only algorithm supported by SAI on
                    // Spectrum. Previously we warned-and-coerced: whatever
                    // the operator wrote we silently stored as EWMA and
                    // published 'algorithm=0' in STATE_DB, contradicting
                    // CONFIG_DB. Reject the row so the operator gets a
                    // clear signal that the value isn't honored.
                    if (toLower(value) != "ewma")
                    {
                        SWSS_LOG_ERROR("ARS: profile '%s' rejected — "
                                       "algorithm '%s' is not supported "
                                       "(only 'ewma'). Fix CONFIG_DB and "
                                       "retry.",
                                       name.c_str(), value.c_str());
                        rejectProfile = true;
                        break;
                    }
                }
                else if (field == "quantization_type")
                {
                    if (value == "log2")
                        entry.quantizationType = 1;
                    else
                        entry.quantizationType = 0;
                    SWSS_LOG_WARN("ARS: profile field 'quantization_type' is "
                                  "parsed but NOT applied — no corresponding "
                                  "SAI attribute exists on this platform. "
                                  "Ignoring for profile %s.", name.c_str());
                }
                else if (field == "link_utilization_threshold")
                {
                    entry.profileLinkUtilThreshold = static_cast<uint32_t>(stoul(value));
                    SWSS_LOG_WARN("ARS: profile field 'link_utilization_threshold' "
                                  "is parsed but NOT applied — no SAI profile "
                                  "attribute backs it on this platform. "
                                  "Ignoring for profile %s.", name.c_str());
                }
                else if (field == "idle_time")
                {
                    entry.profileIdleTime = static_cast<uint32_t>(stoul(value));
                    SWSS_LOG_WARN("ARS: profile field 'idle_time' is parsed "
                                  "but NOT applied at profile scope — "
                                  "idle_time is an ARS_OBJECT attribute. "
                                  "Set idle_time on ARS_OBJECT|<name> instead. "
                                  "Ignoring for profile %s.", name.c_str());
                }
                else if (field == "default_ars_object")
                {
                    entry.defaultArsObject = value;
                    // Accepted for cross-vendor schema compatibility but not
                    // wired into SAI in our design — we use explicit per-
                    // interface ars_object binding instead. See
                    // docs/08.USER_GUIDE_FLOWLET.md §"Cross-Vendor ARS
                    // Design Alignment".
                    SWSS_LOG_NOTICE("ARS: profile field 'default_ars_object=%s' "
                                    "accepted but NOT honored by the NHG "
                                    "resolver in this design (use "
                                    "ARS_INTERFACES.ars_object for per-NHG "
                                    "binding). Published to STATE_DB for "
                                    "visibility.", value.c_str());
                }
                else if (field == "quant_band_0_min_threshold")
                    entry.quantBand0MinThreshold = static_cast<uint32_t>(stoul(value));
                else if (field == "quant_band_1_min_threshold")
                    entry.quantBand1MinThreshold = static_cast<uint32_t>(stoul(value));
                else if (field == "quant_band_2_min_threshold")
                    entry.quantBand2MinThreshold = static_cast<uint32_t>(stoul(value));
                else
                    SWSS_LOG_WARN("ARS: unknown profile field '%s'", field.c_str());
                }
                catch (const std::exception &e)
                {
                    SWSS_LOG_ERROR("ARS: failed to parse profile '%s' field '%s' value '%s': %s",
                                   name.c_str(), field.c_str(), value.c_str(), e.what());
                    rejectProfile = true;
                    break;
                }
            }

            if (rejectProfile)
            {
                it = consumer.m_toSync.erase(it);
                continue;
            }

            // Only auto-derive loadCurrentEnable when:
            //   - this is an initial CREATE (no prior cached state), or
            //   - the operator explicitly wrote load_current_weight in this
            //     update (so the derivation uses a value we just parsed, not
            //     one seeded from the prior state).
            // This prevents unrelated partial updates (e.g. touching only
            // sampling_interval) from silently flipping a previously-set
            // loadCurrentEnable based on a stale loadCurrentWeight.
            if (!explicitLoadCurrent && (isCreate || explicitLoadCurrentWeight))
                entry.loadCurrentEnable = (entry.loadCurrentWeight > 0);

            // Reject non-monotonic quant-band thresholds before they reach SAI:
            // Mellanox SAI enforces band0 < band1 < band2 when any are non-zero
            // and will return SAI_STATUS_INVALID_ATTR_VALUE. Detect here so the
            // operator gets a clear log message instead of a silent SAI rejection.
            bool anyQuantBandSet = (entry.quantBand0MinThreshold |
                                    entry.quantBand1MinThreshold |
                                    entry.quantBand2MinThreshold) != 0;
            if (anyQuantBandSet)
            {
                if (!(entry.quantBand0MinThreshold < entry.quantBand1MinThreshold &&
                      entry.quantBand1MinThreshold < entry.quantBand2MinThreshold))
                {
                    SWSS_LOG_ERROR(
                        "ARS: profile '%s' quant-band thresholds must be strictly "
                        "monotonic (band0=%u < band1=%u < band2=%u); skipping update",
                        name.c_str(),
                        entry.quantBand0MinThreshold,
                        entry.quantBand1MinThreshold,
                        entry.quantBand2MinThreshold);
                    it = consumer.m_toSync.erase(it);
                    continue;
                }
            }

            if (entry.profileOid == SAI_NULL_OBJECT_ID)
            {
                if (!createArsProfile(name, entry))
                {
                    SWSS_LOG_ERROR("ARS: failed to create profile %s", name.c_str());
                    it = consumer.m_toSync.erase(it);
                    continue;
                }
                entry.profileOid = m_arsProfiles[name].profileOid;

                publishArsProfileState(name, m_arsProfiles[name]);

                if (!m_globalProfileName.empty() && m_globalProfileName == name &&
                    m_activeSwitchProfileOid == SAI_NULL_OBJECT_ID)
                {
                    bindArsProfileToSwitch(entry.profileOid);
                }
            }
            else
            {
                sai_object_id_t oid = entry.profileOid;
                bool anyFailed = false;
                anyFailed |= !updateArsProfileAttr(oid, SAI_ARS_PROFILE_ATTR_PORT_LOAD_PAST_WEIGHT,   entry.loadPastWeight);
                anyFailed |= !updateArsProfileAttr(oid, SAI_ARS_PROFILE_ATTR_PORT_LOAD_FUTURE_WEIGHT,  entry.loadFutureWeight);
                anyFailed |= !updateArsProfileAttrBool(oid, SAI_ARS_PROFILE_ATTR_PORT_LOAD_PAST,       entry.loadPastEnable);
                anyFailed |= !updateArsProfileAttrBool(oid, SAI_ARS_PROFILE_ATTR_PORT_LOAD_FUTURE,     entry.loadFutureEnable);
                anyFailed |= !updateArsProfileAttrBool(oid, SAI_ARS_PROFILE_ATTR_PORT_LOAD_CURRENT,    entry.loadCurrentEnable);
                anyFailed |= !updateArsProfileAttr(oid, SAI_ARS_PROFILE_ATTR_PORT_LOAD_EXPONENT,       entry.loadExponent);
                anyFailed |= !updateArsProfileAttr(oid, SAI_ARS_PROFILE_ATTR_MAX_FLOWS,                entry.maxFlows);
                anyFailed |= !updateArsProfileAttr(oid, SAI_ARS_PROFILE_ATTR_LOAD_PAST_MIN_VAL,        entry.loadPastMinVal);
                anyFailed |= !updateArsProfileAttr(oid, SAI_ARS_PROFILE_ATTR_LOAD_PAST_MAX_VAL,        entry.loadPastMaxVal);
                anyFailed |= !updateArsProfileAttr(oid, SAI_ARS_PROFILE_ATTR_LOAD_FUTURE_MIN_VAL,      entry.loadFutureMinVal);
                anyFailed |= !updateArsProfileAttr(oid, SAI_ARS_PROFILE_ATTR_LOAD_FUTURE_MAX_VAL,      entry.loadFutureMaxVal);
                anyFailed |= !updateArsProfileAttr(oid, SAI_ARS_PROFILE_ATTR_LOAD_CURRENT_MIN_VAL,     entry.loadCurrentMinVal);
                anyFailed |= !updateArsProfileAttr(oid, SAI_ARS_PROFILE_ATTR_LOAD_CURRENT_MAX_VAL,     entry.loadCurrentMaxVal);
                anyFailed |= !updateArsProfileAttrBool(oid, SAI_ARS_PROFILE_ATTR_ENABLE_IPV4,          entry.ipv4Enable);
                anyFailed |= !updateArsProfileAttrBool(oid, SAI_ARS_PROFILE_ATTR_ENABLE_IPV6,          entry.ipv6Enable);
                if (entry.samplingInterval > 0)
                    anyFailed |= !updateArsProfileAttr(oid, SAI_ARS_PROFILE_ATTR_SAMPLING_INTERVAL,    entry.samplingInterval);
                if (entry.randomSeed > 0)
                    anyFailed |= !updateArsProfileAttr(oid, SAI_ARS_PROFILE_ATTR_ARS_RANDOM_SEED,      entry.randomSeed);
                // Per-band quant thresholds — these gate whether the Mellanox
                // SAI backend calls sx_api_ar_congestion_threshold_set at bind
                // time. Write through on any transition that affects the three
                // thresholds: either the new values are non-zero, or the previous
                // values were non-zero (so clearing to 0/0/0 actually resets the
                // device to hardened defaults rather than leaving stale state).
                if (anyQuantBandSet || hadQuantBandConfig)
                {
                    anyFailed |= !updateArsProfileAttr(oid, SAI_ARS_PROFILE_ATTR_QUANT_BAND_0_MIN_THRESHOLD,
                                         entry.quantBand0MinThreshold);
                    anyFailed |= !updateArsProfileAttr(oid, SAI_ARS_PROFILE_ATTR_QUANT_BAND_1_MIN_THRESHOLD,
                                         entry.quantBand1MinThreshold);
                    anyFailed |= !updateArsProfileAttr(oid, SAI_ARS_PROFILE_ATTR_QUANT_BAND_2_MIN_THRESHOLD,
                                         entry.quantBand2MinThreshold);
                }
                if (anyFailed)
                    SWSS_LOG_WARN("ARS: one or more SAI attributes failed to update for profile '%s'",
                                  name.c_str());
                else
                {
                    m_arsProfiles[name] = entry;
                    publishArsProfileState(name, entry);
                }
            }
        }
        else if (op == DEL_COMMAND)
        {
            if (removeArsProfile(name))
                m_stateArsProfileTable.del(name);
            else
                SWSS_LOG_ERROR("ARS: failed to remove profile %s — "
                               "keeping STATE_DB entry to reflect SAI state",
                               name.c_str());
        }

        it = consumer.m_toSync.erase(it);
    }
}

/* ── ARS Object (per-NHG) ───────────────────────────────────────────── */

void ArsOrch::doArsObjectTask(Consumer &consumer)
{
    SWSS_LOG_ENTER();

    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
        auto &kfv = it->second;
        string name = kfvKey(kfv);
        string op   = kfvOp(kfv);

        if (op == SET_COMMAND)
        {
            ArsObjectEntry entry;
            if (m_arsObjects.count(name))
                entry = m_arsObjects[name];

            // Capture the pre-update 'enabled' state so we can detect
            // admin_state transitions and drive NHG/LAG rebinds.
            const bool wasEnabled = entry.enabled;

            bool rejectEntry = false;
            for (auto &fv : kfvFieldsValues(kfv))
            {
                const string &field = fvField(fv);
                const string &value = fvValue(fv);

                try
                {
                if (field == "assign_mode")
                {
                    sai_ars_mode_t parsed;
                    if (!parseArsMode(value, &parsed))
                    {
                        SWSS_LOG_ERROR("ARS: object %s rejected — unknown "
                                       "assign_mode '%s'", name.c_str(), value.c_str());
                        rejectEntry = true;
                        break;
                    }
                    entry.mode = parsed;
                }
                else if (field == "idle_time")   entry.idleTime = static_cast<uint32_t>(stoul(value));
                else if (field == "max_flows")   entry.maxFlows = static_cast<uint32_t>(stoul(value));
                else if (field == "admin_state") entry.enabled  = (toLower(value) == "up");
                else if (field == "profile")
                {
                    // SAI binds the ARS profile at switch scope (via
                    // SAI_SWITCH_ATTR_ARS_PROFILE) — there is no per-
                    // ARS_OBJECT profile attribute. Keeping a cached value
                    // here would advertise a behavior we cannot deliver.
                    SWSS_LOG_WARN("ARS: field 'profile=%s' on ARS_OBJECT is "
                                  "not supported (SAI binds ARS_PROFILE at "
                                  "switch scope — use ARS|GLOBAL.profile). "
                                  "Ignoring for %s.",
                                  value.c_str(), name.c_str());
                }
                else if (field == "port_profile")
                {
                    // Same reason: SAI has no per-object port-profile
                    // binding. Per-port profile assignment is done via
                    // ARS_INTERFACES.port_profile on each egress port.
                    SWSS_LOG_WARN("ARS: field 'port_profile=%s' on ARS_OBJECT "
                                  "is not supported — use "
                                  "ARS_INTERFACES.port_profile on each "
                                  "member port instead. Ignoring for %s.",
                                  value.c_str(), name.c_str());
                }
                else if (field == "ipv4_enable" || field == "ipv6_enable")
                {
                    // SAI models IPv{4,6} enable on the *profile* only
                    // (SAI_ARS_PROFILE_ATTR_ENABLE_IPV{4,6}), not on the
                    // per-object ARS. Accepting these fields at the object
                    // level silently drops them; direct the operator to the
                    // right knob instead of pretending it worked.
                    SWSS_LOG_WARN("ARS: field '%s' is not supported on ARS_OBJECT "
                                  "(set it on ARS_PROFILE instead) — ignoring for %s",
                                  field.c_str(), name.c_str());
                }
                else
                {
                    SWSS_LOG_WARN("ARS: unknown object field '%s' on %s",
                                  field.c_str(), name.c_str());
                }
                }
                catch (const std::exception &e)
                {
                    SWSS_LOG_ERROR("ARS: failed to parse object '%s' field '%s' value '%s': %s",
                                   name.c_str(), field.c_str(), value.c_str(), e.what());
                    rejectEntry = true;
                    break;
                }
            }

            if (rejectEntry)
            {
                it = consumer.m_toSync.erase(it);
                continue;
            }

            if (!m_arsEnabled)
            {
                // Defer creation by persisting the parsed entry with a null
                // SAI OID. enableArsDataPlane() walks m_arsObjects on global
                // ARS|GLOBAL admin_state=up and creates everything with
                // arsOid == SAI_NULL_OBJECT_ID. We still erase from m_toSync
                // (the config-DB view is already captured in our cache) so
                // the orchagent doesn't spin on a can-never-progress entry.
                m_arsObjects[name] = entry;
                SWSS_LOG_WARN("ARS: global ARS not enabled, cached object %s "
                              "for creation on next ARS|GLOBAL admin_state=up",
                              name.c_str());
                it = consumer.m_toSync.erase(it);
                continue;
            }

            if (entry.arsOid == SAI_NULL_OBJECT_ID)
            {
                // Re-read the full current CONFIG_DB row before creating the
                // SAI object. uCLI actioners write each field as a separate
                // HSET — the ConsumerStateTable notification we just popped
                // may only contain one of them (e.g. `assign_mode` without
                // `idle_time` or `max_flows`). Relying on ArsObjectEntry's
                // struct defaults here means the SAI create call ships
                // idleTime=256 / maxFlows=512 even when CONFIG_DB actually
                // holds 200 / 4096 from prior HSETs in the same uCLI
                // session. Worse, Mellanox SAI flips both attrs to
                // SAI_STATUS_OBJECT_IN_USE once an NHG binds to the ARS
                // object (mlnx_sai_ars.c:mlnx_ars_set_idle_time), so the
                // subsequent set_attribute we fire from the idle_time/
                // max_flows notification gets rejected and ASIC_DB keeps
                // the stale create-time default. Folding the full row in
                // here ensures the create always carries CONFIG_DB's
                // latest view.
                std::vector<swss::FieldValueTuple> cfgRow;
                if (m_cfgArsObjectTable.get(name, cfgRow))
                {
                    for (const auto &fv : cfgRow)
                    {
                        const string &cfgField = fvField(fv);
                        const string &cfgValue = fvValue(fv);
                        try
                        {
                            if (cfgField == "assign_mode")
                            {
                                sai_ars_mode_t parsed;
                                if (parseArsMode(cfgValue, &parsed))
                                    entry.mode = parsed;
                            }
                            else if (cfgField == "idle_time")
                            {
                                entry.idleTime =
                                    static_cast<uint32_t>(stoul(cfgValue));
                            }
                            else if (cfgField == "max_flows")
                            {
                                entry.maxFlows =
                                    static_cast<uint32_t>(stoul(cfgValue));
                            }
                            else if (cfgField == "admin_state")
                            {
                                entry.enabled = (toLower(cfgValue) == "up");
                            }
                        }
                        catch (const std::exception &e)
                        {
                            SWSS_LOG_WARN("ARS: failed to parse "
                                          "CONFIG_DB.%s.%s='%s' for %s: %s "
                                          "(using notification/default value)",
                                          CFG_ARS_OBJECT_TABLE_NAME,
                                          cfgField.c_str(), cfgValue.c_str(),
                                          name.c_str(), e.what());
                        }
                    }
                }

                if (!createArsObject(name, entry))
                {
                    SWSS_LOG_ERROR("ARS: failed to create object %s", name.c_str());
                    it = consumer.m_toSync.erase(it);
                    continue;
                }
                if (gRouteOrch)
                    gRouteOrch->bindArsToExistingNhgs();

                for (const auto &profKv : m_arsProfiles)
                {
                    if (profKv.second.defaultArsObject == name)
                        publishArsProfileState(profKv.first, profKv.second);
                }

                for (auto &lagKv : m_arsLags)
                {
                    if (lagKv.second.arsObject == name && lagKv.second.enabled &&
                        m_arsEnabledLags.count(lagKv.first) == 0)
                    {
                        sai_object_id_t arsOid = m_arsObjects[name].arsOid;
                        if (bindArsToLag(lagKv.first, arsOid))
                        {
                            m_arsEnabledLags.insert(lagKv.first);
                            SWSS_LOG_NOTICE("ARS: deferred LAG %s now bound to ARS object '%s'",
                                            lagKv.first.c_str(), name.c_str());
                        }
                    }
                }
            }
            else
            {
                sai_object_id_t oid = entry.arsOid;
                // Diff before set: only push SAI attrs whose value actually
                // changed in this update. Previously every ARS_OBJECT SET
                // issued three SAI calls regardless, cluttering sairedis.rec
                // and churning vendor-SAI state for no effect.
                const auto &prev = m_arsObjects[name];

                // Mellanox SAI explicitly rejects changing SAI_ARS_ATTR_MODE
                // on an existing ARS object ("Not supported to change ars
                // type on the fly"). Pushing the set would leave our cache
                // claiming the new mode while the ASIC still runs the old
                // one. Reject the transition and keep the cached mode at the
                // previous value so STATE_DB / arsOrch-internal state stays
                // truthful; operators must delete the ARS_OBJECT and
                // re-create it.
                //
                // Also surface the split to STATE_DB so a `show` command
                // (or an external monitor) can detect that CONFIG_DB
                // advertises a mode the ASIC is not using. Without this
                // marker the CLI still writes the new assign_mode into
                // CONFIG_DB (KLISH doesn't roll back the row on orchagent
                // rejection), producing a silent mismatch.
                if (prev.mode != entry.mode)
                {
                    const char *prevStr =
                        (prev.mode == SAI_ARS_MODE_FLOWLET_QUALITY)
                            ? "flowlet-quality" : "packet-quality";
                    const char *newStr =
                        (entry.mode == SAI_ARS_MODE_FLOWLET_QUALITY)
                            ? "flowlet-quality" : "packet-quality";

                    SWSS_LOG_ERROR("ARS: object '%s' assign_mode change "
                                   "(%s → %s) is not supported on live ARS "
                                   "objects by the underlying SAI. Delete "
                                   "ARS_OBJECT|%s and re-create it to change "
                                   "mode. Keeping previous mode in cache; "
                                   "CONFIG_DB will temporarily advertise the "
                                   "rejected value.",
                                   name.c_str(), prevStr, newStr, name.c_str());

                    // Republish a degraded row for this ARS object name so
                    // operators can see the split. Keyed by object name
                    // (ARS_OBJECT_TABLE) rather than NHG OID so it can be
                    // found by the `show load-balance adaptive object`
                    // backend without a reverse lookup.
                    vector<FieldValueTuple> fvs;
                    fvs.emplace_back("status", "mode_change_rejected");
                    fvs.emplace_back("current_mode", prevStr);
                    fvs.emplace_back("config_db_mode", newStr);
                    fvs.emplace_back("reason",
                        "Mellanox SAI rejects SAI_ARS_ATTR_MODE changes on "
                        "live ARS objects. Delete and re-create the object "
                        "to change mode.");
                    m_stateArsObjectTable.set(name, fvs);

                    entry.mode = prev.mode;
                }
                else
                {
                    // Clear any stale "mode_change_rejected" marker on the
                    // object name once CONFIG_DB stops advertising a
                    // different mode — either the operator reverted the
                    // row or deleted+recreated the object.
                    std::vector<FieldValueTuple> existing;
                    if (m_stateArsObjectTable.get(name, existing))
                    {
                        for (const auto &fv : existing)
                        {
                            if (fvField(fv) == "status" &&
                                fvValue(fv) == "mode_change_rejected")
                            {
                                m_stateArsObjectTable.del(name);
                                break;
                            }
                        }
                    }
                }
                if (isFlowletMode(entry.mode) && prev.idleTime != entry.idleTime)
                {
                    if (!setArsObjectAttr(oid, SAI_ARS_ATTR_IDLE_TIME, entry.idleTime))
                    {
                        SWSS_LOG_ERROR("ARS: object '%s' idle_time change "
                                       "(%u → %u) rejected by SAI — keeping "
                                       "previous value.",
                                       name.c_str(), prev.idleTime,
                                       entry.idleTime);
                        entry.idleTime = prev.idleTime;
                    }
                }
                // SAI_ARS_ATTR_MAX_FLOWS cannot be modified while any NHG
                // references the object. Try the set; if SAI rejects with
                // OBJECT_IN_USE, keep the cache at the previous value so a
                // 'show' of the object doesn't advertise a value the ASIC
                // isn't actually using.
                if (prev.maxFlows != entry.maxFlows)
                {
                    if (!setArsObjectAttr(oid, SAI_ARS_ATTR_MAX_FLOWS, entry.maxFlows))
                    {
                        SWSS_LOG_ERROR("ARS: object '%s' max_flows change "
                                       "(%u → %u) rejected by SAI — this "
                                       "attribute is immutable while NHGs "
                                       "reference the object. Unbind all "
                                       "NHGs (or delete/recreate the object) "
                                       "to apply. Keeping previous value.",
                                       name.c_str(), prev.maxFlows,
                                       entry.maxFlows);
                        entry.maxFlows = prev.maxFlows;
                    }
                }
                m_arsObjects[name] = entry;

                // If admin_state flipped, re-evaluate all NHG bindings so the
                // resolver's updated view of 'enabled' takes effect on the
                // data plane. Also sync LAG bindings that reference this
                // object name.
                if (wasEnabled != entry.enabled)
                {
                    SWSS_LOG_NOTICE("ARS: object '%s' admin_state %s → %s, "
                                    "rebinding NHGs and LAGs",
                                    name.c_str(),
                                    wasEnabled ? "up" : "down",
                                    entry.enabled ? "up" : "down");

                    if (gRouteOrch)
                        gRouteOrch->rebindArsForAllNhgs();

                    for (const auto &lagKv : m_arsLags)
                    {
                        if (lagKv.second.arsObject != name)
                            continue;
                        const auto &lagName = lagKv.first;
                        const bool isBound = m_arsEnabledLags.count(lagName) > 0;
                        if (entry.enabled && !isBound && lagKv.second.enabled)
                        {
                            if (bindArsToLag(lagName, oid))
                                m_arsEnabledLags.insert(lagName);
                        }
                        else if (!entry.enabled && isBound)
                        {
                            if (unbindArsFromLag(lagName))
                                m_arsEnabledLags.erase(lagName);
                        }
                    }
                }
            }
        }
        else if (op == DEL_COMMAND)
        {
            if (removeArsObject(name))
            {
                // Drop any STATE_DB "mode_change_rejected" marker left over
                // from a previously-rejected transition on this object name.
                // Without this a deleted+recreated-as-different-mode cycle
                // could leave the previous object's degraded row visible
                // indefinitely.
                m_stateArsObjectTable.del(name);
            }
            else
            {
                SWSS_LOG_ERROR("ARS: failed to remove object %s — "
                               "keeping STATE_DB entry to reflect SAI state",
                               name.c_str());
            }
        }

        it = consumer.m_toSync.erase(it);
    }
}

/* ── ARS Interfaces (per-port membership) ────────────────────────────── */

void ArsOrch::doArsInterfaceTask(Consumer &consumer)
{
    SWSS_LOG_ENTER();

    // Track whether any update in this batch actually touched a field that
    // could alter NHG→ARS resolution (admin_state, ars_object). If nothing
    // relevant changed, skip the (expensive + noisy on Mellanox) call to
    // bindArsToExistingNhgs() at the tail of the function.
    bool resolverInputsChanged = false;

    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
        auto &kfv = it->second;
        string portName = kfvKey(kfv);
        string op       = kfvOp(kfv);

        if (op == SET_COMMAND)
        {
            ArsInterfaceEntry entry;
            if (m_arsInterfaces.count(portName))
                entry = m_arsInterfaces[portName];

            const ArsInterfaceEntry prevEntry = entry;

            bool rejectInterface = false;
            for (auto &fv : kfvFieldsValues(kfv))
            {
                const string &field = fvField(fv);
                const string &value = fvValue(fv);

                try
                {
                if      (field == "admin_state")              entry.enabled = (toLower(value) == "up");
                else if (field == "ars_object")               entry.arsObject = value;
                else if (field == "port_profile")             entry.portProfile = value;
                else if (field == "link_utilization_threshold") entry.linkUtilThreshold = static_cast<uint32_t>(stoul(value));
                else if (field == "weight")
                    entry.weight = static_cast<uint32_t>(stoul(value));
                else
                    SWSS_LOG_WARN("ARS: unknown interface field '%s' on %s",
                                  field.c_str(), portName.c_str());
                }
                catch (const std::exception &e)
                {
                    SWSS_LOG_ERROR("ARS: failed to parse interface '%s' field '%s' value '%s': %s",
                                   portName.c_str(), field.c_str(), value.c_str(), e.what());
                    rejectInterface = true;
                    break;
                }
            }

            if (rejectInterface)
            {
                it = consumer.m_toSync.erase(it);
                continue;
            }

            if (prevEntry.enabled != entry.enabled ||
                prevEntry.arsObject != entry.arsObject)
            {
                resolverInputsChanged = true;
            }

            bool prevEnabled = (m_arsEnabledPorts.find(portName) != m_arsEnabledPorts.end());

            if (entry.enabled && !prevEnabled)
            {
                if (setPortArsEnable(portName, true))
                    m_arsEnabledPorts.insert(portName);
                else
                {
                    SWSS_LOG_ERROR("ARS: failed to enable ARS on port %s — "
                                   "keeping entry.enabled=false so NHG resolver "
                                   "does not bind ARS on this port",
                                   portName.c_str());
                    entry.enabled = false;
                }
            }
            else if (!entry.enabled && prevEnabled)
            {
                if (setPortArsEnable(portName, false))
                    m_arsEnabledPorts.erase(portName);
                else
                    SWSS_LOG_ERROR("ARS: failed to disable ARS on port %s", portName.c_str());
            }

            m_arsInterfaces[portName] = entry;

            if (!entry.arsObject.empty())
            {
                SWSS_LOG_NOTICE("ARS: interface %s associated with ARS object '%s'",
                                portName.c_str(), entry.arsObject.c_str());
            }
            if (!entry.portProfile.empty())
            {
                applyPortProfileToInterface(portName, entry.portProfile);
                SWSS_LOG_NOTICE("ARS: interface %s bound to port-profile '%s'",
                                portName.c_str(), entry.portProfile.c_str());
            }
            if (entry.linkUtilThreshold > 0)
            {
                SWSS_LOG_WARN("ARS: interface %s link_utilization_threshold=%u%% "
                              "configured, but SAI has no per-port link-util "
                              "attribute. Set link_utilization_threshold on the "
                              "ARS_PROFILE instead to affect all ports, or use "
                              "an ARS_PORT_PROFILE with load-band overrides for "
                              "per-port tuning. Ignoring the per-interface value.",
                              portName.c_str(), entry.linkUtilThreshold);
            }
        }
        else if (op == DEL_COMMAND)
        {
            if (m_arsInterfaces.count(portName))
                resolverInputsChanged = true;
            if (m_arsEnabledPorts.count(portName))
            {
                // Only drop from m_arsEnabledPorts if SAI actually accepted
                // the disable. Mellanox SAI will reject the set when the
                // port has a RIF (mlnx_sai_port.c "Can't modify
                // SAI_PORT_ATTR_ARS_ENABLE on port with created RIFs").
                // Previously the erase was unconditional, causing the
                // orchagent view to falsely claim ARS is off while the
                // ASIC still had it on.
                if (setPortArsEnable(portName, false))
                {
                    m_arsEnabledPorts.erase(portName);
                }
                else
                {
                    SWSS_LOG_ERROR("ARS: failed to disable ARS on port %s at "
                                   "DEL — keeping in m_arsEnabledPorts so the "
                                   "ASIC / orchagent view stay consistent. "
                                   "Operator may need to remove the RIF first.",
                                   portName.c_str());
                }
            }
            m_arsInterfaces.erase(portName);
        }

        it = consumer.m_toSync.erase(it);
    }

    // Only re-evaluate NHG bindings if something resolver-relevant changed
    // this batch. Skipping no-op re-binds keeps sairedis.rec clean and, on
    // Mellanox, avoids a flurry of INVALID_PARAMETER failures from the
    // set-on-populated-NHG restriction (see commit notes on ARS NHG
    // binding immutability).
    if (resolverInputsChanged && gRouteOrch && m_arsEnabled)
        gRouteOrch->bindArsToExistingNhgs();
}

/* ── ARS Port Profile ────────────────────────────────────────────────── */

void ArsOrch::doArsPortProfileTask(Consumer &consumer)
{
    SWSS_LOG_ENTER();

    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
        auto &kfv = it->second;
        string name = kfvKey(kfv);
        string op   = kfvOp(kfv);

        if (op == SET_COMMAND)
        {
            ArsPortProfileEntry entry;
            if (m_arsPortProfiles.count(name))
                entry = m_arsPortProfiles[name];

            bool rejectPortProfile = false;
            for (auto &fv : kfvFieldsValues(kfv))
            {
                const string &field = fvField(fv);
                const string &value = fvValue(fv);

                try
                {
                if      (field == "load_past_min_val")    entry.loadPastMinVal    = static_cast<uint32_t>(stoul(value));
                else if (field == "load_past_max_val")    entry.loadPastMaxVal    = static_cast<uint32_t>(stoul(value));
                else if (field == "load_future_min_val")  entry.loadFutureMinVal  = static_cast<uint32_t>(stoul(value));
                else if (field == "load_future_max_val")  entry.loadFutureMaxVal  = static_cast<uint32_t>(stoul(value));
                else if (field == "load_current_min_val") entry.loadCurrentMinVal = static_cast<uint32_t>(stoul(value));
                else if (field == "load_current_max_val") entry.loadCurrentMaxVal = static_cast<uint32_t>(stoul(value));
                else if (field == "enable")
                    entry.enabled = (value == "true");
                else if (field == "port_load_past_weight")
                    entry.portLoadPastWeight = static_cast<uint32_t>(stoul(value));
                else if (field == "port_load_future_weight")
                    entry.portLoadFutureWeight = static_cast<uint32_t>(stoul(value));
                else if (field == "load_scaling_factor")
                {
                    double fval = stod(value);
                    if (fabs(fval) < 1e-9)
                        entry.loadScalingFactorAuto = true;
                    entry.loadScalingFactor = static_cast<uint32_t>(round(fval * 10));
                    entry.loadScalingFactorRaw = fval;
                }
                }
                catch (const std::exception &e)
                {
                    SWSS_LOG_ERROR("ARS: failed to parse port-profile '%s' field '%s' value '%s': %s",
                                   name.c_str(), field.c_str(), value.c_str(), e.what());
                    rejectPortProfile = true;
                    break;
                }
            }

            if (rejectPortProfile)
            {
                it = consumer.m_toSync.erase(it);
                continue;
            }

            m_arsPortProfiles[name] = entry;
            SWSS_LOG_NOTICE("ARS: port-profile '%s' updated", name.c_str());

            // Re-apply to every port AND PortChannel that references this
            // profile (LAGs live in m_arsLags now — covering both ensures
            // an update to a shared port-profile actually reaches every
            // user).
            auto reapply = [&](const auto &map) {
                for (const auto &kv : map)
                {
                    if (kv.second.portProfile == name && kv.second.enabled)
                    {
                        SWSS_LOG_NOTICE("ARS: re-applying updated port-profile '%s' to %s",
                                        name.c_str(), kv.first.c_str());
                        applyPortProfileToInterface(kv.first, name);
                    }
                }
            };
            reapply(m_arsInterfaces);
            reapply(m_arsLags);
        }
        else if (op == DEL_COMMAND)
        {
            m_arsPortProfiles.erase(name);
            SWSS_LOG_NOTICE("ARS: port-profile '%s' removed", name.c_str());
        }

        it = consumer.m_toSync.erase(it);
    }
}

/* ── ARS Nexthops (prefix → ARS object binding for routeorch) ────────── */

void ArsOrch::doArsNexthopsTask(Consumer &consumer)
{
    SWSS_LOG_ENTER();

    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
        auto &kfv = it->second;
        string prefix = kfvKey(kfv);
        string op     = kfvOp(kfv);

        if (op == SET_COMMAND)
        {
            string arsObjName;
            for (auto &fv : kfvFieldsValues(kfv))
            {
                if (fvField(fv) == "ars_object")
                    arsObjName = fvValue(fv);
            }

            if (arsObjName.empty())
            {
                SWSS_LOG_WARN("ARS: nexthop entry for prefix %s has no ars_object",
                              prefix.c_str());
                it = consumer.m_toSync.erase(it);
                continue;
            }

            m_nexthopArsBindings[prefix] = arsObjName;
            // NHGs are shared across prefixes in SONiC; there is no reliable
            // way to look up "the prefix for this NHG" at resolve time, so a
            // per-prefix ARS binding cannot currently be enforced by the
            // resolver. The mapping is accepted and cached for future use,
            // but has no effect on NHG→ARS binding today — use
            // ARS_INTERFACES.ars_object for per-NHG control instead.
            //
            // Escalated to ERROR so the row's no-op status is visible in
            // syslog without LOG_DEBUG; operators who wrote this expecting
            // per-prefix ARS routing will otherwise see a CONFIG_DB row
            // and assume it works.
            SWSS_LOG_ERROR("ARS: ARS_NEXTHOPS|%s → ARS object '%s' is NOT "
                           "honored on this release. NHGs are shared across "
                           "prefixes, so a per-prefix override cannot be "
                           "applied; the row has been cached but has no "
                           "effect. Use ARS_INTERFACES.ars_object on the "
                           "egress ports instead. Remove the ARS_NEXTHOPS "
                           "row to silence this message.",
                           prefix.c_str(), arsObjName.c_str());
        }
        else if (op == DEL_COMMAND)
        {
            m_nexthopArsBindings.erase(prefix);
            SWSS_LOG_NOTICE("ARS: prefix %s ARS binding removed", prefix.c_str());
        }

        it = consumer.m_toSync.erase(it);
    }
}

/* ── ARS PortChannels (LAG-level ARS membership) ─────────────────────── */

void ArsOrch::doArsPortChannelTask(Consumer &consumer)
{
    SWSS_LOG_ENTER();

    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
        auto &kfv = it->second;
        string lagName = kfvKey(kfv);
        string op      = kfvOp(kfv);

        if (op == SET_COMMAND)
        {
            ArsInterfaceEntry entry;
            if (m_arsLags.count(lagName))
                entry = m_arsLags[lagName];

            bool rejectLag = false;
            for (auto &fv : kfvFieldsValues(kfv))
            {
                const string &field = fvField(fv);
                const string &value = fvValue(fv);

                try
                {
                if      (field == "admin_state") entry.enabled = (toLower(value) == "up");
                else if (field == "ars_object")  entry.arsObject = value;
                else if (field == "port_profile") entry.portProfile = value;
                else if (field == "link_utilization_threshold")
                    entry.linkUtilThreshold = static_cast<uint32_t>(stoul(value));
                }
                catch (const std::exception &e)
                {
                    SWSS_LOG_ERROR("ARS: failed to parse PortChannel '%s' field '%s' value '%s': %s",
                                   lagName.c_str(), field.c_str(), value.c_str(), e.what());
                    rejectLag = true;
                    break;
                }
            }

            if (rejectLag)
            {
                it = consumer.m_toSync.erase(it);
                continue;
            }

            m_arsLags[lagName] = entry;

            bool prevBound = (m_arsEnabledLags.count(lagName) > 0);

            if (entry.enabled && !entry.arsObject.empty() && m_arsEnabled)
            {
                sai_object_id_t arsOid = getArsObjectOid(entry.arsObject);
                if (arsOid != SAI_NULL_OBJECT_ID)
                {
                    if (bindArsToLag(lagName, arsOid))
                    {
                        m_arsEnabledLags.insert(lagName);
                        SWSS_LOG_NOTICE("ARS: PortChannel %s bound to ARS object '%s'",
                                        lagName.c_str(), entry.arsObject.c_str());
                    }
                    else
                    {
                        SWSS_LOG_ERROR("ARS: failed to bind ARS object '%s' to PortChannel %s",
                                       entry.arsObject.c_str(), lagName.c_str());
                    }
                }
                else
                {
                    SWSS_LOG_WARN("ARS: ARS object '%s' not yet created, deferring LAG %s binding",
                                  entry.arsObject.c_str(), lagName.c_str());
                }
            }
            else if ((!entry.enabled || entry.arsObject.empty()) && prevBound)
            {
                if (unbindArsFromLag(lagName))
                    m_arsEnabledLags.erase(lagName);
                else
                    SWSS_LOG_ERROR("ARS: failed to unbind ARS from PortChannel %s", lagName.c_str());
            }

            if (!entry.portProfile.empty() && entry.enabled)
            {
                applyPortProfileToInterface(lagName, entry.portProfile);
            }
        }
        else if (op == DEL_COMMAND)
        {
            if (m_arsEnabledLags.count(lagName))
            {
                unbindArsFromLag(lagName);
                m_arsEnabledLags.erase(lagName);
            }
            m_arsLags.erase(lagName);
            SWSS_LOG_NOTICE("ARS: PortChannel %s ARS config removed", lagName.c_str());
        }

        it = consumer.m_toSync.erase(it);
    }
}

/* ── SAI helpers: ARS Profile ────────────────────────────────────────── */

bool ArsOrch::createArsProfile(const string &name, const ArsProfileEntry &entry)
{
    SWSS_LOG_ENTER();

    // Mellanox SAI hard-codes ars_profile_idx = 0 in
    // mlnx_sai_create_ars_profile, so any subsequent create_ars_profile
    // returns SAI_STATUS_ITEM_ALREADY_EXISTS. Detect the single-profile
    // limit from this side so the operator gets a single clear error
    // (pointing at the pre-existing profile) rather than a generic SAI
    // failure, and so the second ARS_PROFILE row in CONFIG_DB is visibly
    // rejected rather than silently leaving its profileOid at
    // SAI_NULL_OBJECT_ID. Mirrors the check in createArsObject().
    //
    // One exception: if the only thing holding the slot is the internal
    // __ARS_DEFAULT__ profile (auto-created by createDefaultProfileIfNeeded
    // when ARS|GLOBAL came up without an explicit profile), transparently
    // evict it so the operator-supplied profile can take the slot. Without
    // this evict path, users on Mellanox could NEVER create a named
    // ARS_PROFILE after enabling ARS, because __ARS_DEFAULT__ would
    // permanently occupy the SAI slot.
    //
    // For any *other* pre-existing profile the user must remove it
    // explicitly (`no load-balance adaptive profile <name>`) first.
    // See docs/04.FLOWLET.md §"Spectrum-4 single-profile limit".
    static const string kDefaultName = "__ARS_DEFAULT__";
    string blockingProfile;
    for (const auto &kv : m_arsProfiles)
    {
        if (kv.first == name)
            continue;
        if (kv.second.profileOid == SAI_NULL_OBJECT_ID)
            continue;
        blockingProfile = kv.first;
        break;
    }
    if (!blockingProfile.empty())
    {
        if (blockingProfile == kDefaultName)
        {
            SWSS_LOG_NOTICE("ARS: evicting auto-created '%s' to make room "
                            "for operator-supplied profile '%s' (Mellanox "
                            "single-profile slot)",
                            blockingProfile.c_str(), name.c_str());
            // removeArsProfile unbinds from the switch (if bound) and
            // calls remove_ars_profile(); on success the slot frees up.
            // If the remove itself fails (e.g. because an ARS object is
            // still live — but Mellanox allows the profile OID to be
            // unbound from the switch without touching ARS objects), we
            // still fall through to fail the create with a clear error.
            if (!removeArsProfile(blockingProfile))
            {
                SWSS_LOG_ERROR("ARS: failed to evict '%s' — new profile '%s' "
                               "cannot be created while the default holds the "
                               "SAI slot. Retry after orchagent recovers.",
                               blockingProfile.c_str(), name.c_str());
                return false;
            }
            m_stateArsProfileTable.del(blockingProfile);
        }
        else
        {
            SWSS_LOG_ERROR("ARS: cannot create ARS_PROFILE '%s' — the "
                           "underlying SAI on this platform supports at most "
                           "one ARS profile per switch, and '%s' already holds "
                           "that slot. Remove '%s' from CONFIG_DB "
                           "(`ARS_PROFILE|%s`) first, or update it in-place "
                           "instead of creating a new one.",
                           name.c_str(), blockingProfile.c_str(),
                           blockingProfile.c_str(), blockingProfile.c_str());
            return false;
        }
    }

    vector<sai_attribute_t> attrs;
    sai_attribute_t attr;

    attr.id = SAI_ARS_PROFILE_ATTR_ALGO;
    attr.value.s32 = entry.algorithm;
    attrs.push_back(attr);

    attr.id = SAI_ARS_PROFILE_ATTR_PORT_LOAD_PAST;
    attr.value.booldata = entry.loadPastEnable;
    attrs.push_back(attr);

    attr.id = SAI_ARS_PROFILE_ATTR_PORT_LOAD_PAST_WEIGHT;
    attr.value.u8 = (uint8_t)entry.loadPastWeight;
    attrs.push_back(attr);

    attr.id = SAI_ARS_PROFILE_ATTR_PORT_LOAD_FUTURE;
    attr.value.booldata = entry.loadFutureEnable;
    attrs.push_back(attr);

    attr.id = SAI_ARS_PROFILE_ATTR_PORT_LOAD_FUTURE_WEIGHT;
    attr.value.u8 = (uint8_t)entry.loadFutureWeight;
    attrs.push_back(attr);

    attr.id = SAI_ARS_PROFILE_ATTR_PORT_LOAD_CURRENT;
    attr.value.booldata = entry.loadCurrentEnable;
    attrs.push_back(attr);

    attr.id = SAI_ARS_PROFILE_ATTR_PORT_LOAD_EXPONENT;
    attr.value.u8 = (uint8_t)entry.loadExponent;
    attrs.push_back(attr);

    attr.id = SAI_ARS_PROFILE_ATTR_ENABLE_IPV4;
    attr.value.booldata = entry.ipv4Enable;
    attrs.push_back(attr);

    attr.id = SAI_ARS_PROFILE_ATTR_ENABLE_IPV6;
    attr.value.booldata = entry.ipv6Enable;
    attrs.push_back(attr);

    if (entry.samplingInterval > 0)
    {
        attr.id = SAI_ARS_PROFILE_ATTR_SAMPLING_INTERVAL;
        attr.value.u32 = entry.samplingInterval;
        attrs.push_back(attr);
    }

    if (entry.randomSeed > 0)
    {
        attr.id = SAI_ARS_PROFILE_ATTR_ARS_RANDOM_SEED;
        attr.value.u32 = entry.randomSeed;
        attrs.push_back(attr);
    }

    if (entry.maxFlows > 0)
    {
        attr.id = SAI_ARS_PROFILE_ATTR_MAX_FLOWS;
        attr.value.u32 = entry.maxFlows;
        attrs.push_back(attr);
    }

    if (entry.loadPastMinVal > 0 || entry.loadPastMaxVal > 0)
    {
        attr.id = SAI_ARS_PROFILE_ATTR_LOAD_PAST_MIN_VAL;
        attr.value.u32 = entry.loadPastMinVal;
        attrs.push_back(attr);
        attr.id = SAI_ARS_PROFILE_ATTR_LOAD_PAST_MAX_VAL;
        attr.value.u32 = entry.loadPastMaxVal;
        attrs.push_back(attr);
    }

    if (entry.loadFutureMinVal > 0 || entry.loadFutureMaxVal > 0)
    {
        attr.id = SAI_ARS_PROFILE_ATTR_LOAD_FUTURE_MIN_VAL;
        attr.value.u32 = entry.loadFutureMinVal;
        attrs.push_back(attr);
        attr.id = SAI_ARS_PROFILE_ATTR_LOAD_FUTURE_MAX_VAL;
        attr.value.u32 = entry.loadFutureMaxVal;
        attrs.push_back(attr);
    }

    if (entry.loadCurrentMinVal > 0 || entry.loadCurrentMaxVal > 0)
    {
        attr.id = SAI_ARS_PROFILE_ATTR_LOAD_CURRENT_MIN_VAL;
        attr.value.u32 = entry.loadCurrentMinVal;
        attrs.push_back(attr);
        attr.id = SAI_ARS_PROFILE_ATTR_LOAD_CURRENT_MAX_VAL;
        attr.value.u32 = entry.loadCurrentMaxVal;
        attrs.push_back(attr);
    }

    // Per-band congestion thresholds (Mbps). Sending these at CREATE is what
    // allows the Mellanox SAI backend to take the non-hardened path and call
    // sx_api_ar_congestion_threshold_set on the SDK — without them the EWMA
    // quality signal cannot trigger flowlet reassignment on CPU-scale loads
    // because the SDK keeps its line-rate default thresholds.
    if (entry.quantBand0MinThreshold != 0 ||
        entry.quantBand1MinThreshold != 0 ||
        entry.quantBand2MinThreshold != 0)
    {
        attr.id = SAI_ARS_PROFILE_ATTR_QUANT_BAND_0_MIN_THRESHOLD;
        attr.value.u32 = entry.quantBand0MinThreshold;
        attrs.push_back(attr);
        attr.id = SAI_ARS_PROFILE_ATTR_QUANT_BAND_1_MIN_THRESHOLD;
        attr.value.u32 = entry.quantBand1MinThreshold;
        attrs.push_back(attr);
        attr.id = SAI_ARS_PROFILE_ATTR_QUANT_BAND_2_MIN_THRESHOLD;
        attr.value.u32 = entry.quantBand2MinThreshold;
        attrs.push_back(attr);
    }

    sai_object_id_t profileOid;
    sai_status_t status = sai_ars_profile_api->create_ars_profile(
        &profileOid, gSwitchId, (uint32_t)attrs.size(), attrs.data());

    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("ARS: create_ars_profile failed for %s: %s",
                       name.c_str(), sai_serialize_status(status).c_str());
        return false;
    }

    SWSS_LOG_NOTICE("ARS: created profile %s OID 0x%" PRIx64, name.c_str(), profileOid);

    ArsProfileEntry stored = entry;
    stored.profileOid = profileOid;
    m_arsProfiles[name] = stored;

    if (m_activeSwitchProfileOid == SAI_NULL_OBJECT_ID)
    {
        bindArsProfileToSwitch(profileOid);
    }

    return true;
}

bool ArsOrch::removeArsProfile(const string &name)
{
    SWSS_LOG_ENTER();

    auto it = m_arsProfiles.find(name);
    if (it == m_arsProfiles.end())
        return true;

    sai_object_id_t oid = it->second.profileOid;

    if (oid == m_activeSwitchProfileOid)
    {
        bindArsProfileToSwitch(SAI_NULL_OBJECT_ID);
    }

    sai_status_t status = sai_ars_profile_api->remove_ars_profile(oid);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("ARS: remove_ars_profile failed for %s: %s",
                       name.c_str(), sai_serialize_status(status).c_str());
        return false;
    }

    SWSS_LOG_NOTICE("ARS: removed profile %s", name.c_str());
    m_arsProfiles.erase(it);
    return true;
}

bool ArsOrch::updateArsProfileAttr(sai_object_id_t oid, sai_ars_profile_attr_t attrId, uint32_t val)
{
    sai_attribute_t attr;
    attr.id = attrId;

    switch (attrId)
    {
    case SAI_ARS_PROFILE_ATTR_PORT_LOAD_PAST_WEIGHT:
    case SAI_ARS_PROFILE_ATTR_PORT_LOAD_FUTURE_WEIGHT:
    case SAI_ARS_PROFILE_ATTR_PORT_LOAD_EXPONENT:
        attr.value.u8 = (uint8_t)val;
        break;
    default:
        attr.value.u32 = val;
        break;
    }

    sai_status_t status = sai_ars_profile_api->set_ars_profile_attribute(oid, &attr);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("ARS: set_ars_profile_attribute (attr %d) failed: %s",
                       attrId, sai_serialize_status(status).c_str());
        return false;
    }
    return true;
}

bool ArsOrch::updateArsProfileAttrBool(sai_object_id_t oid, sai_ars_profile_attr_t attrId, bool val)
{
    sai_attribute_t attr;
    attr.id = attrId;
    attr.value.booldata = val;

    sai_status_t status = sai_ars_profile_api->set_ars_profile_attribute(oid, &attr);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("ARS: set_ars_profile_attribute bool (attr %d) failed: %s",
                       attrId, sai_serialize_status(status).c_str());
        return false;
    }
    return true;
}

/* ── SAI helpers: ARS Object (per-NHG) ───────────────────────────────── */

static bool isFlowletMode(sai_ars_mode_t mode)
{
    return mode == SAI_ARS_MODE_FLOWLET_QUALITY;
}

bool ArsOrch::createArsObject(const string &name, const ArsObjectEntry &entry)
{
    SWSS_LOG_ENTER();

    // Mellanox SAI internally hard-codes ars_obj_idx = 0 in mlnx_sai_create_ars
    // and the subsequent mlnx_ars_find_ars_by_oid() check makes any second
    // create_ars() call return SAI_STATUS_ITEM_ALREADY_EXISTS. Detect this
    // from the orchagent side so the operator gets a single clear error
    // (pointing at the pre-existing object) instead of a generic SAI failure,
    // and so the second ARS_OBJECT row in CONFIG_DB is visibly rejected
    // rather than silently leaving its arsOid at SAI_NULL_OBJECT_ID.
    //
    // The user-guide section "Cross-Vendor ARS Design Alignment" calls out
    // per-NHG granularity as a design goal, but on this SAI it is not
    // achievable with more than one ARS object. Document the limit here.
    for (const auto &kv : m_arsObjects)
    {
        if (kv.first == name)
            continue;
        if (kv.second.arsOid == SAI_NULL_OBJECT_ID)
            continue;
        SWSS_LOG_ERROR("ARS: cannot create ARS_OBJECT '%s' — the underlying "
                       "SAI on this platform supports at most one ARS object "
                       "per switch, and '%s' already holds that slot. Delete "
                       "'%s' first, or reuse it (per-NHG ARS granularity is "
                       "not available on this SAI).",
                       name.c_str(), kv.first.c_str(), kv.first.c_str());
        return false;
    }

    vector<sai_attribute_t> attrs;
    sai_attribute_t attr;

    attr.id = SAI_ARS_ATTR_MODE;
    attr.value.s32 = entry.mode;
    attrs.push_back(attr);

    if (isFlowletMode(entry.mode))
    {
        attr.id = SAI_ARS_ATTR_IDLE_TIME;
        attr.value.u32 = entry.idleTime;
        attrs.push_back(attr);
    }

    attr.id = SAI_ARS_ATTR_MAX_FLOWS;
    attr.value.u32 = entry.maxFlows;
    attrs.push_back(attr);

    sai_object_id_t arsOid;
    sai_status_t status = sai_ars_api->create_ars(
        &arsOid, gSwitchId, (uint32_t)attrs.size(), attrs.data());

    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("ARS: create_ars failed for %s: %s",
                       name.c_str(), sai_serialize_status(status).c_str());
        return false;
    }

    SWSS_LOG_NOTICE("ARS: created object %s OID 0x%" PRIx64 " mode %d",
                    name.c_str(), arsOid, (int)entry.mode);

    ArsObjectEntry stored = entry;
    stored.arsOid = arsOid;
    m_arsObjects[name] = stored;
    return true;
}

bool ArsOrch::removeArsObject(const string &name)
{
    SWSS_LOG_ENTER();

    auto it = m_arsObjects.find(name);
    if (it == m_arsObjects.end())
        return true;

    const sai_object_id_t oid = it->second.arsOid;

    // A deferred-creation entry (arsOid still NULL) has no SAI state to tear
    // down — just drop the cache.
    if (oid == SAI_NULL_OBJECT_ID)
    {
        SWSS_LOG_NOTICE("ARS: removed deferred-creation entry for object %s",
                        name.c_str());
        m_arsObjects.erase(it);
        return true;
    }

    // Unbind any NHGs and LAGs that currently reference this object. SAI's
    // remove_ars returns SAI_STATUS_OBJECT_IN_USE while bindings exist, so
    // without this sweep removal of an ARS object in active use silently
    // fails and the orchagent-side cache drifts from config-DB.
    //
    // NHGs: flip each NHG whose resolver currently returns this arsOid to
    // SAI_NULL_OBJECT_ID by temporarily hiding the object from
    // resolveArsForNhg (setting enabled=false) and asking RouteOrch to
    // rebind.
    //
    // Safety note: if remove_ars subsequently fails we must restore both
    // the NHG bindings and the LAG bindings so the failure is side-effect
    // free. The operator can then address the underlying SAI error and
    // retry deletion without having to rebuild ARS bindings manually.
    it->second.enabled = false;
    if (gRouteOrch)
        gRouteOrch->rebindArsForAllNhgs();

    // LAGs tracked by m_arsEnabledLags that point at this object name.
    vector<string> lagsToUnbind;
    for (const auto &kv : m_arsLags)
    {
        if (kv.second.arsObject == name && m_arsEnabledLags.count(kv.first))
            lagsToUnbind.push_back(kv.first);
    }
    for (const auto &lag : lagsToUnbind)
    {
        unbindArsFromLag(lag);
        m_arsEnabledLags.erase(lag);
    }

    sai_status_t status = sai_ars_api->remove_ars(oid);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("ARS: remove_ars failed for %s: %s — restoring "
                       "NHG and LAG bindings so the failure is side-effect free",
                       name.c_str(), sai_serialize_status(status).c_str());

        // Restore in-memory view first so the resolver and LAG loop below
        // see the object as live again.
        it->second.enabled = true;

        // Rebinding NHGs: the resolver will now return oid again because
        // enabled=true, so rebindArsForAllNhgs puts each NHG back onto
        // this ARS object (or whichever the resolver dictates).
        if (gRouteOrch)
            gRouteOrch->rebindArsForAllNhgs();

        // Rebind each LAG we unbound above. Use the same oid we captured
        // up top since the entry's arsOid is unchanged.
        for (const auto &lag : lagsToUnbind)
        {
            if (bindArsToLag(lag, oid))
                m_arsEnabledLags.insert(lag);
            else
                SWSS_LOG_ERROR("ARS: failed to restore LAG %s binding for %s "
                               "after remove_ars failure", lag.c_str(), name.c_str());
        }
        return false;
    }

    SWSS_LOG_NOTICE("ARS: removed object %s", name.c_str());
    m_arsObjects.erase(it);
    return true;
}

bool ArsOrch::setArsObjectAttr(sai_object_id_t oid, sai_ars_attr_t attrId, uint32_t val)
{
    sai_attribute_t attr;
    attr.id = attrId;
    if (attrId == SAI_ARS_ATTR_MODE)
        attr.value.s32 = (int32_t)val;
    else
        attr.value.u32 = val;

    sai_status_t status = sai_ars_api->set_ars_attribute(oid, &attr);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("ARS: set_ars_attribute (attr %d) failed: %s",
                       attrId, sai_serialize_status(status).c_str());
        return false;
    }
    return true;
}

/* ── Bind profile to switch, bind ARS to NHG ─────────────────────────── */

bool ArsOrch::bindArsProfileToSwitch(sai_object_id_t profileOid)
{
    SWSS_LOG_ENTER();

    sai_attribute_t attr;
    attr.id = SAI_SWITCH_ATTR_ARS_PROFILE;
    attr.value.oid = profileOid;

    sai_status_t status = sai_switch_api->set_switch_attribute(gSwitchId, &attr);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("ARS: bind profile to switch failed: %s",
                       sai_serialize_status(status).c_str());
        return false;
    }

    m_activeSwitchProfileOid = profileOid;
    SWSS_LOG_NOTICE("ARS: bound profile OID 0x%" PRIx64 " to switch", profileOid);
    return true;
}

bool ArsOrch::bindArsToNhg(sai_object_id_t nhgOid, sai_object_id_t arsOid)
{
    SWSS_LOG_ENTER();

    sai_attribute_t attr;
    attr.id = SAI_NEXT_HOP_GROUP_ATTR_ARS_OBJECT_ID;
    attr.value.oid = arsOid;

    sai_status_t status = sai_next_hop_group_api->set_next_hop_group_attribute(nhgOid, &attr);
    if (status != SAI_STATUS_SUCCESS)
    {
        // Mellanox SAI rejects SAI_NEXT_HOP_GROUP_ATTR_ARS_OBJECT_ID with
        // SAI_STATUS_INVALID_PARAMETER when the NHG already has members.
        // This makes the binding effectively write-once at create time:
        // any later transition (enable ARS after routes exist, admin_state
        // flip on ARS_OBJECT, profile rebind, interface remap) cannot be
        // applied in-place. We can't fix the SAI limitation from here, but
        // we can make it visible to operators instead of silently failing
        // and leaving the orchagent cache out of sync with the ASIC.
        //
        // Surface the specific failure in STATE_DB so 'show load-balance
        // adaptive' and monitoring can show *which* NHGs are out of sync
        // and *why*. The workaround is to flap the affected route(s) so
        // the NHG is re-created with the new binding at create time.
        const string key = sai_serialize_object_id(nhgOid);
        const char *direction = (arsOid == SAI_NULL_OBJECT_ID) ? "unbind ARS from"
                                                               : "bind ARS to";
        string reason;
        if (status == SAI_STATUS_INVALID_PARAMETER)
        {
            reason = "SAI set-attribute rejected: NHG already has members "
                     "(Mellanox write-once restriction). Re-create the route "
                     "to apply the new ARS binding.";
            SWSS_LOG_ERROR("ARS: %s NHG 0x%" PRIx64 " (target ARS 0x%" PRIx64
                           ") failed (INVALID_PARAMETER) — NHG-ARS binding is "
                           "write-once at create time on this SAI. Flap the "
                           "affected routes to apply the change.",
                           direction, nhgOid, arsOid);
        }
        else if (status == SAI_STATUS_NOT_SUPPORTED)
        {
            reason = "SAI NHG-ARS binding not supported (SAI ARS not enabled?)";
            SWSS_LOG_ERROR("ARS: %s NHG 0x%" PRIx64 " (target ARS 0x%" PRIx64
                           ") failed: NOT_SUPPORTED — check that SAI ARS is "
                           "enabled on the switch.", direction, nhgOid, arsOid);
        }
        else
        {
            reason = string("SAI set_next_hop_group_attribute failed: ") +
                     sai_serialize_status(status);
            SWSS_LOG_ERROR("ARS: %s NHG 0x%" PRIx64 " (target ARS 0x%" PRIx64
                           ") failed: %s",
                           direction, nhgOid, arsOid,
                           sai_serialize_status(status).c_str());
        }
        // Surface the failure in STATE_DB for BOTH bind and unbind failures.
        // A failed *unbind* (arsOid == NULL) is particularly dangerous: the
        // caller's intent was to remove the binding, but the SAI rejection
        // means the ASIC still carries the old ars_object_id. Previously
        // this path did nothing (no STATE_DB row), silently dropping any
        // existing row and leaving operators with no visible signal that
        // orchagent state and ASIC state have diverged.
        //
        // Record a 'degraded' row tagged with the direction so tooling can
        // distinguish a stuck bind from a stuck unbind, and keep
        // m_nhgStateKeys populated so forgetNhg() on NHG removal still
        // cleans up.
        m_nhgStateKeys[nhgOid] = key;
        if (arsOid == SAI_NULL_OBJECT_ID)
        {
            writeArsNhgState(key, true,
                             string("unbind rejected by SAI — ASIC retains previous "
                                    "ARS binding (write-once NHG-ARS on Mellanox). "
                                    "Flap the affected routes to apply: ") + reason);
        }
        else
        {
            writeArsNhgState(key, true, reason);
        }
        return false;
    }

    SWSS_LOG_NOTICE("ARS: bound ARS OID 0x%" PRIx64 " to NHG 0x%" PRIx64, arsOid, nhgOid);

    // Mirror the outcome into STATE_DB's ARS_NHG_TABLE so an 'active' row
    // exists alongside the 'degraded' rows written by resolveArsForNhg's
    // failure paths. Without this, operators reading ARS_NHG_TABLE only
    // ever see failures.
    const string key = sai_serialize_object_id(nhgOid);
    if (arsOid == SAI_NULL_OBJECT_ID)
    {
        // Unbind: drop both the STATE_DB row and the bookkeeping entry so
        // m_nhgStateKeys doesn't accumulate stale keys over the lifetime of
        // the NHG. (Previously the map was inserted-but-never-erased on
        // every unbind, leaking an entry per unbind cycle.)
        auto it = m_nhgStateKeys.find(nhgOid);
        if (it != m_nhgStateKeys.end())
        {
            removeArsNhgState(it->second);
            m_nhgStateKeys.erase(it);
        }
        else
        {
            removeArsNhgState(key);
        }
    }
    else
    {
        m_nhgStateKeys[nhgOid] = key;
        writeArsNhgState(key, false);
    }
    return true;
}

bool ArsOrch::unbindArsFromNhg(sai_object_id_t nhgOid)
{
    return bindArsToNhg(nhgOid, SAI_NULL_OBJECT_ID);
}

void ArsOrch::forgetNhg(sai_object_id_t nhgOid)
{
    SWSS_LOG_ENTER();
    auto it = m_nhgStateKeys.find(nhgOid);
    if (it == m_nhgStateKeys.end())
        return;
    removeArsNhgState(it->second);
    m_nhgStateKeys.erase(it);
}

bool ArsOrch::bindArsToLag(const string &lagName, sai_object_id_t arsOid)
{
    SWSS_LOG_ENTER();

    Port port;
    if (!m_portsOrch->getPort(lagName, port))
    {
        SWSS_LOG_ERROR("ARS: PortChannel %s not found", lagName.c_str());
        return false;
    }

    if (port.m_lag_id == SAI_NULL_OBJECT_ID)
    {
        SWSS_LOG_ERROR("ARS: %s has no SAI LAG object", lagName.c_str());
        return false;
    }

    sai_attribute_t attr;
    attr.id = SAI_LAG_ATTR_ARS_OBJECT_ID;
    attr.value.oid = arsOid;

    sai_status_t status = sai_lag_api->set_lag_attribute(port.m_lag_id, &attr);
    if (status != SAI_STATUS_SUCCESS)
    {
        // Same RIF-present caveat as setPortArsEnable — Mellanox SAI wires
        // both SAI_PORT_ATTR_ARS_ENABLE and SAI_LAG_ATTR_ARS_OBJECT_ID
        // through mlnx_port_lag_ars_enable_set_impl, which refuses the set
        // if the LAG has any RIFs bound. Give the operator a pointer to the
        // remove-RIF / set-ARS / re-add-RIF sequence rather than a generic
        // SAI error.
        if (status == SAI_STATUS_INVALID_PARAMETER)
        {
            SWSS_LOG_ERROR("ARS: set SAI_LAG_ATTR_ARS_OBJECT_ID on %s failed "
                           "(INVALID_PARAMETER). Most likely cause on Mellanox: "
                           "the LAG has a router interface (RIF) attached; SAI "
                           "forbids toggling the ARS binding on a LAG with RIFs. "
                           "Workaround: remove the IP from the LAG, set the ARS "
                           "binding, then re-add the IP.", lagName.c_str());
        }
        else
        {
            SWSS_LOG_ERROR("ARS: set SAI_LAG_ATTR_ARS_OBJECT_ID on %s failed: %s",
                           lagName.c_str(), sai_serialize_status(status).c_str());
        }
        return false;
    }

    SWSS_LOG_NOTICE("ARS: bound ARS OID 0x%" PRIx64 " to LAG %s (0x%" PRIx64 ")",
                    arsOid, lagName.c_str(), port.m_lag_id);
    return true;
}

bool ArsOrch::unbindArsFromLag(const string &lagName)
{
    return bindArsToLag(lagName, SAI_NULL_OBJECT_ID);
}

string ArsOrch::getArsObjectForPort(const string &portName) const
{
    auto it = m_arsInterfaces.find(portName);
    if (it != m_arsInterfaces.end() && it->second.enabled)
        return it->second.arsObject;

    auto lagIt = m_arsLags.find(portName);
    if (lagIt != m_arsLags.end() && lagIt->second.enabled)
        return lagIt->second.arsObject;

    return "";
}

sai_object_id_t ArsOrch::resolveArsForNhg(sai_object_id_t nhgOid, const NextHopGroupKey &nhgKey)
{
    SWSS_LOG_ENTER();

    if (!m_arsEnabled)
        return SAI_NULL_OBJECT_ID;

    // Per design (see docs/08.USER_GUIDE_FLOWLET.md §"Per-Interface ARS
    // Object Association"): every NHG member interface must reference the
    // same ARS object. If any member is missing or disagrees, the NHG falls
    // back to standard hash-based ECMP. This is intentionally strict so
    // partial config doesn't silently run some flows adaptive and others
    // static. The previous loop was permissive (treated "no ars_object" as
    // "abstain") which contradicted the documented behavior and allowed
    // accidental adaptive binding on mixed NHGs.
    string commonArsObj;
    bool inconsistent = false;
    string inconsistencyReason;

    const auto &nextHops = nhgKey.getNextHops();
    for (const auto &nh : nextHops)
    {
        const string &portName = nh.alias;
        string arsObj = getArsObjectForPort(portName);

        if (arsObj.empty())
        {
            inconsistent = true;
            inconsistencyReason = "member " + portName +
                                  " has no ars_object association";
            break;
        }

        if (commonArsObj.empty())
        {
            commonArsObj = arsObj;
        }
        else if (commonArsObj != arsObj)
        {
            inconsistent = true;
            inconsistencyReason = "member " + portName + " binds '" + arsObj +
                                  "' but earlier members bind '" +
                                  commonArsObj + "'";
            break;
        }
    }

    if (inconsistent)
    {
        SWSS_LOG_NOTICE("ARS: NHG %s not eligible for adaptive routing: %s. "
                        "Falling back to plain ECMP.",
                        nhgKey.to_string().c_str(),
                        inconsistencyReason.c_str());
        // Surface the reason in STATE_DB so operators can see why ARS isn't
        // active on this NHG without having to grep syslog.
        const std::string key = sai_serialize_object_id(nhgOid);
        m_nhgStateKeys[nhgOid] = key;
        writeArsNhgState(key, true, inconsistencyReason);
        return SAI_NULL_OBJECT_ID;
    }

    if (commonArsObj.empty())
    {
        // No members at all (should not happen, but be defensive) — nothing
        // to bind. Note: the prefix→ARS mapping (ARS_NEXTHOPS) cannot be
        // consulted here because NHGs are shared across prefixes, so there
        // is no well-defined "prefix for this NHG". The previous fallback
        // (m_nexthopArsBindings.begin()->second) picked an arbitrary entry
        // from an unordered map, which was effectively random; it has been
        // removed. See doArsNexthopsTask for the warning logged when that
        // table is populated.
        return SAI_NULL_OBJECT_ID;
    }

    // Honor ARS_OBJECT.admin_state: if the resolved object is disabled we
    // return NULL so the NHG falls back to plain ECMP.
    auto objIt = m_arsObjects.find(commonArsObj);
    if (objIt == m_arsObjects.end() || !objIt->second.enabled)
    {
        const std::string key = sai_serialize_object_id(nhgOid);
        if (objIt != m_arsObjects.end())
        {
            SWSS_LOG_NOTICE("ARS: object '%s' admin_state=down — NHG falls back "
                            "to plain ECMP", commonArsObj.c_str());
            m_nhgStateKeys[nhgOid] = key;
            writeArsNhgState(key, true, "ars_object '" + commonArsObj +
                                         "' admin_state=down");
        }
        else
        {
            SWSS_LOG_WARN("ARS: NHG %s references unknown ARS object '%s'",
                          nhgKey.to_string().c_str(), commonArsObj.c_str());
            m_nhgStateKeys[nhgOid] = key;
            writeArsNhgState(key, true, "ars_object '" + commonArsObj +
                                         "' not defined");
        }
        return SAI_NULL_OBJECT_ID;
    }

    auto arsOid = objIt->second.arsOid;
    if (arsOid == SAI_NULL_OBJECT_ID)
    {
        SWSS_LOG_WARN("ARS: object '%s' not yet created for NHG %s",
                      commonArsObj.c_str(), nhgKey.to_string().c_str());
        // Key the ARS_NHG_TABLE by the SAI NHG OID so forgetNhg() can
        // reliably delete it on NHG removal. Storing the key also means a
        // subsequent successful bind can replace this row with an 'active'
        // entry via bindArsToNhg's positive path.
        const std::string key = sai_serialize_object_id(nhgOid);
        m_nhgStateKeys[nhgOid] = key;
        writeArsNhgState(key, true, "ARS object not created");
    }

    return arsOid;
}

/* ── Per-port ARS enable via SAI_PORT_ATTR_ARS_ENABLE ─────────────────── */

bool ArsOrch::setPortArsEnable(const string &portName, bool enable)
{
    SWSS_LOG_ENTER();

    Port port;
    if (!m_portsOrch->getPort(portName, port))
    {
        SWSS_LOG_ERROR("ARS: port %s not found", portName.c_str());
        return false;
    }

    sai_attribute_t attr;
    attr.id = SAI_PORT_ATTR_ARS_ENABLE;
    attr.value.booldata = enable;

    sai_status_t status = sai_port_api->set_port_attribute(port.m_port_id, &attr);

    // Mellanox SAI rejects SAI_PORT_ATTR_ARS_ENABLE with
    // SAI_STATUS_INVALID_PARAMETER when the port has a router interface
    // (RIF). L3 uplink ports — exactly where ARS matters — always have
    // RIFs at steady state. Handle this transparently by temporarily
    // removing the RIF, enabling ARS, then re-creating the RIF. This
    // is the same sequence setup_lab.py performed externally, but done
    // atomically inside orchagent where it doesn't race with other
    // notification processing.
    if (status == SAI_STATUS_INVALID_PARAMETER && port.m_rif_id != 0)
    {
        SWSS_LOG_NOTICE("ARS: SAI_PORT_ATTR_ARS_ENABLE=%s on %s rejected "
                        "(port has RIF oid:0x%" PRIx64 "). Attempting RIF "
                        "bounce: remove → set ARS → re-create.",
                        enable ? "true" : "false", portName.c_str(),
                        port.m_rif_id);

        // Save the RIF attributes we need for re-creation.
        sai_object_id_t saved_rif_id = port.m_rif_id;
        sai_object_id_t saved_vr_id  = port.m_vr_id;

        // Step 1: Remove the RIF.
        sai_status_t rif_status = sai_router_intfs_api->remove_router_interface(saved_rif_id);
        if (rif_status != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_ERROR("ARS: RIF bounce failed — could not remove RIF "
                           "oid:0x%" PRIx64 " on %s: %s. ARS enable aborted.",
                           saved_rif_id, portName.c_str(),
                           sai_serialize_status(rif_status).c_str());
            return false;
        }

        // Step 2: Enable ARS on the now-bare port.
        status = sai_port_api->set_port_attribute(port.m_port_id, &attr);
        if (status != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_ERROR("ARS: RIF bounce — ARS enable still failed after "
                           "RIF removal on %s: %s. Re-creating RIF.",
                           portName.c_str(),
                           sai_serialize_status(status).c_str());
        }

        // Step 3: Re-create the RIF (must succeed regardless of ARS result).
        sai_attribute_t rif_attrs[5];
        uint32_t rif_attr_count = 0;

        rif_attrs[rif_attr_count].id = SAI_ROUTER_INTERFACE_ATTR_VIRTUAL_ROUTER_ID;
        rif_attrs[rif_attr_count].value.oid = saved_vr_id;
        rif_attr_count++;

        rif_attrs[rif_attr_count].id = SAI_ROUTER_INTERFACE_ATTR_TYPE;
        rif_attrs[rif_attr_count].value.s32 = SAI_ROUTER_INTERFACE_TYPE_PORT;
        rif_attr_count++;

        rif_attrs[rif_attr_count].id = SAI_ROUTER_INTERFACE_ATTR_PORT_ID;
        rif_attrs[rif_attr_count].value.oid = port.m_port_id;
        rif_attr_count++;

        rif_attrs[rif_attr_count].id = SAI_ROUTER_INTERFACE_ATTR_MTU;
        rif_attrs[rif_attr_count].value.u32 = port.m_mtu;
        rif_attr_count++;

        sai_object_id_t new_rif_id;
        rif_status = sai_router_intfs_api->create_router_interface(
            &new_rif_id, gSwitchId, rif_attr_count, rif_attrs);

        if (rif_status != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_ERROR("ARS: CRITICAL — RIF re-creation failed on %s: %s. "
                           "Port has lost its router interface!",
                           portName.c_str(),
                           sai_serialize_status(rif_status).c_str());
            // Update port state to reflect the lost RIF.
            port.m_rif_id = 0;
            m_portsOrch->setPort(portName, port);
            return (status == SAI_STATUS_SUCCESS);
        }

        // Update port state with the new RIF OID.
        port.m_rif_id = new_rif_id;
        m_portsOrch->setPort(portName, port);

        if (status == SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_NOTICE("ARS: RIF bounce succeeded — port %s ARS %s "
                            "(new RIF oid:0x%" PRIx64 ")",
                            portName.c_str(), enable ? "enabled" : "disabled",
                            new_rif_id);
            return true;
        }
        return false;
    }

    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("ARS: set SAI_PORT_ATTR_ARS_ENABLE on %s failed: %s",
                       portName.c_str(), sai_serialize_status(status).c_str());
        return false;
    }

    SWSS_LOG_NOTICE("ARS: port %s ARS %s", portName.c_str(), enable ? "enabled" : "disabled");
    return true;
}

/* ── Wholesale data-plane enable/disable (ARS|GLOBAL admin_state) ─────── */

void ArsOrch::disableArsDataPlane()
{
    SWSS_LOG_ENTER();
    SWSS_LOG_NOTICE("ARS: tearing down data plane (unbinding NHGs/LAGs, "
                    "clearing per-port enables)");

    if (gRouteOrch)
        gRouteOrch->unbindArsFromAllNhgs();

    // Only forget a LAG/port from our bookkeeping if SAI actually accepted
    // the unbind/disable. Previously this blindly .clear()-ed both sets
    // regardless of the SAI return status, which on Mellanox can fail for
    // LAGs with active RIFs or ports whose ARS_ENABLE setting is locked
    // by other state. Keeping the entries until we know SAI agrees means
    // subsequent enableArsDataPlane() calls can retry correctly, and
    // operators see accurate state in 'show load-balance adaptive'.
    for (auto it = m_arsEnabledLags.begin(); it != m_arsEnabledLags.end(); )
    {
        if (unbindArsFromLag(*it))
        {
            it = m_arsEnabledLags.erase(it);
        }
        else
        {
            SWSS_LOG_ERROR("ARS: failed to unbind ARS from LAG %s; keeping "
                           "in m_arsEnabledLags so teardown can retry",
                           it->c_str());
            ++it;
        }
    }

    for (auto it = m_arsEnabledPorts.begin(); it != m_arsEnabledPorts.end(); )
    {
        if (setPortArsEnable(*it, false))
        {
            it = m_arsEnabledPorts.erase(it);
        }
        else
        {
            SWSS_LOG_ERROR("ARS: failed to disable ARS on port %s; keeping "
                           "in m_arsEnabledPorts so teardown can retry",
                           it->c_str());
            ++it;
        }
    }
}

void ArsOrch::enableArsDataPlane()
{
    SWSS_LOG_ENTER();
    SWSS_LOG_NOTICE("ARS: re-applying data plane from cached CONFIG_DB state");

    // Create any ARS objects that were parsed while global ARS was disabled
    // (stored with arsOid == SAI_NULL_OBJECT_ID by doArsObjectTask). Done
    // before port / LAG / NHG steps so the resolver can find them.
    for (auto &kv : m_arsObjects)
    {
        if (kv.second.arsOid != SAI_NULL_OBJECT_ID)
            continue;
        const auto &name = kv.first;
        if (createArsObject(name, kv.second))
        {
            SWSS_LOG_NOTICE("ARS: created deferred object '%s' on global enable",
                            name.c_str());
        }
        else
        {
            SWSS_LOG_ERROR("ARS: failed to create deferred object '%s'",
                           name.c_str());
        }
    }

    // Re-enable per-port ARS on every physical port that was admin_state=up.
    for (const auto &kv : m_arsInterfaces)
    {
        const auto &portName = kv.first;
        const auto &entry = kv.second;
        if (!entry.enabled)
            continue;
        if (setPortArsEnable(portName, true))
            m_arsEnabledPorts.insert(portName);
    }

    // Re-bind any LAGs whose ARS object already exists.
    for (const auto &kv : m_arsLags)
    {
        const auto &name = kv.first;
        const auto &entry = kv.second;
        if (!entry.enabled || entry.arsObject.empty())
            continue;
        sai_object_id_t arsOid = getArsObjectOid(entry.arsObject);
        if (arsOid == SAI_NULL_OBJECT_ID)
            continue;
        if (bindArsToLag(name, arsOid))
            m_arsEnabledLags.insert(name);
    }

    // Retroactively bind ARS to any routes/NHGs that were installed while ARS
    // was disabled.
    if (gRouteOrch)
        gRouteOrch->bindArsToExistingNhgs();
}

/* ── Per-port ARS profile attributes via SAI ──────────────────────────── */

bool ArsOrch::setPortArsLoadBands(const string &portName, const ArsPortProfileEntry &pp)
{
    // Previously this routine wrote the ARS_PORT_PROFILE.load_*_{min,max}_val
    // fields to the *switch-level* profile OID (m_activeSwitchProfileOid)
    // via SAI_ARS_PROFILE_ATTR_LOAD_*_{MIN,MAX}_VAL. Those SAI attributes are
    // profile-scope (there is no per-port SAI_PORT_ATTR_ARS_LOAD_*_MIN_VAL),
    // so two different ports bound to two different ARS_PORT_PROFILEs that
    // each set load bands would OVERWRITE each other on the single
    // switch-level profile — the last-applied port-profile would win
    // globally, silently reconfiguring every other port's EWMA computation.
    //
    // This is the same class of bug as setPortArsLinkUtilThreshold (also a
    // stub now). The SAI model simply does not support per-port load-band
    // customisation; we cannot deliver what the ARS_PORT_PROFILE schema
    // promises. Refuse it explicitly and point the operator at the right
    // knob (ARS_PROFILE load-band attributes for a switch-wide setting).
    if (pp.loadPastMinVal == 0 && pp.loadPastMaxVal == 0 &&
        pp.loadFutureMinVal == 0 && pp.loadFutureMaxVal == 0 &&
        pp.loadCurrentMinVal == 0 && pp.loadCurrentMaxVal == 0)
    {
        return true; // nothing configured; no-op
    }

    SWSS_LOG_WARN("ARS: ARS_PORT_PROFILE load-band values on port %s "
                  "(past[%u/%u], future[%u/%u], current[%u/%u]) are IGNORED — "
                  "SAI has no per-port load-band attributes on this platform. "
                  "Set load_*_min_val / load_*_max_val on the ARS_PROFILE "
                  "itself to apply switch-wide; the previous per-port path "
                  "silently clobbered the global profile for all ports.",
                  portName.c_str(),
                  pp.loadPastMinVal, pp.loadPastMaxVal,
                  pp.loadFutureMinVal, pp.loadFutureMaxVal,
                  pp.loadCurrentMinVal, pp.loadCurrentMaxVal);
    return false;
}

bool ArsOrch::setPortArsScalingFactor(const string &portName, const ArsPortProfileEntry &pp)
{
    Port port;
    if (!m_portsOrch->getPort(portName, port))
    {
        SWSS_LOG_ERROR("ARS: port %s not found for scaling factor", portName.c_str());
        return false;
    }

    // Callers should have already fanned LAGs out to their members via
    // applyPortProfileToInterface; belt-and-braces check here so a stray
    // direct caller doesn't pass SAI_NULL_OBJECT_ID to set_port_attribute.
    if (port.m_type == Port::LAG || port.m_port_id == SAI_NULL_OBJECT_ID)
    {
        SWSS_LOG_WARN("ARS: %s has no SAI port OID (type=%d) — cannot set "
                      "per-port scaling factor directly; caller should "
                      "iterate LAG members instead",
                      portName.c_str(), (int)port.m_type);
        return false;
    }

    // Unit convention: pp.loadScalingFactor stores the operator's multiplier
    // scaled by 10 (so 1.0 → 10, 2.5 → 25, 10.0 → 100, cf. doArsPortProfileTask
    // where round(fval * 10) is assigned). The auto path must produce the
    // same representation, not a raw "speed in 10G units" number.
    // For 100G: multiplier is 10.0 → scaled = 100.
    // For  25G: multiplier is  2.5 → scaled = 25.
    // That translates to (speedMbps / 1000), not (speedMbps / 10000).
    uint32_t factor = pp.loadScalingFactor;
    if (pp.loadScalingFactorAuto)
    {
        uint32_t speedMbps = port.m_speed;
        factor = (speedMbps > 0) ? (speedMbps / 1000) : 10; // 1.0 default
        SWSS_LOG_NOTICE("ARS: auto scaling factor for %s: speed=%u Mbps → "
                        "multiplier=%u.%u (SAI u32 = %u)",
                        portName.c_str(), speedMbps, factor / 10, factor % 10, factor);
    }

    sai_attribute_t attr;
    attr.id = SAI_PORT_ATTR_ARS_PORT_LOAD_SCALING_FACTOR;
    attr.value.u32 = factor;

    sai_status_t status = sai_port_api->set_port_attribute(port.m_port_id, &attr);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_WARN("ARS: set scaling factor on %s failed: %s (may require SDK-only path)",
                      portName.c_str(), sai_serialize_status(status).c_str());
        return false;
    }
    return true;
}

bool ArsOrch::setPortArsLinkUtilThreshold(const string &portName, uint32_t threshold)
{
    // Retained as a stub for source compatibility with older callers; the
    // per-port 'link utilization threshold' concept does not have a
    // corresponding SAI attribute. The previous implementation wrote a
    // percentage value into SAI_ARS_PROFILE_ATTR_LOAD_PAST_MAX_VAL on the
    // switch-level profile — which is both (a) a byte-rate ceiling, not a
    // percentage, and (b) global, not per-port — effectively clobbering
    // the globally-bound EWMA knob every time any port's threshold was
    // written. See commit message for the full rationale.
    //
    // If operators want per-port behavior, they should use an
    // ARS_PORT_PROFILE with the load_*_max_val bands; for a global
    // link-utilization threshold, set it on ARS_PROFILE directly.
    SWSS_LOG_WARN("ARS: setPortArsLinkUtilThreshold(%s, %u) ignored — "
                  "no SAI per-port link-util attribute exists; use "
                  "ARS_PORT_PROFILE load bands or the profile-level "
                  "link_utilization_threshold",
                  portName.c_str(), threshold);
    return false;
}

bool ArsOrch::setPortArsWeights(const string &portName, uint32_t pastWeight, uint32_t futureWeight)
{
    Port port;
    if (!m_portsOrch->getPort(portName, port))
    {
        SWSS_LOG_ERROR("ARS: port %s not found for weights", portName.c_str());
        return false;
    }

    // See setPortArsScalingFactor — SAI_PORT_ATTR_ARS_* are port-level and
    // cannot be set on a LAG's (null) port OID.
    if (port.m_type == Port::LAG || port.m_port_id == SAI_NULL_OBJECT_ID)
    {
        SWSS_LOG_WARN("ARS: %s has no SAI port OID (type=%d) — cannot set "
                      "per-port weights directly; caller should iterate "
                      "LAG members instead",
                      portName.c_str(), (int)port.m_type);
        return false;
    }

    sai_attribute_t attr;
    bool success = true;

    if (pastWeight > 0)
    {
        attr.id = SAI_PORT_ATTR_ARS_PORT_LOAD_PAST_WEIGHT;
        attr.value.u32 = pastWeight;
        sai_status_t status = sai_port_api->set_port_attribute(port.m_port_id, &attr);
        if (status != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_WARN("ARS: set past weight on %s failed: %s",
                          portName.c_str(), sai_serialize_status(status).c_str());
            success = false;
        }
    }

    if (futureWeight > 0)
    {
        attr.id = SAI_PORT_ATTR_ARS_PORT_LOAD_FUTURE_WEIGHT;
        attr.value.u32 = futureWeight;
        sai_status_t status = sai_port_api->set_port_attribute(port.m_port_id, &attr);
        if (status != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_WARN("ARS: set future weight on %s failed: %s",
                          portName.c_str(), sai_serialize_status(status).c_str());
            success = false;
        }
    }

    return success;
}

void ArsOrch::applyPortProfileToInterface(const string &portName, const string &profileName)
{
    auto it = m_arsPortProfiles.find(profileName);
    if (it == m_arsPortProfiles.end())
    {
        SWSS_LOG_WARN("ARS: port-profile '%s' not found for interface %s",
                      profileName.c_str(), portName.c_str());
        return;
    }

    const auto &pp = it->second;

    if (!pp.enabled)
    {
        SWSS_LOG_NOTICE("ARS: port-profile '%s' is disabled, skipping application to %s",
                        profileName.c_str(), portName.c_str());
        return;
    }

    // For PortChannels (LAGs) the SAI_PORT_ATTR_ARS_* attributes do not
    // apply to the LAG object itself — they're port-level. A naive
    // set_port_attribute(port.m_port_id) on a LAG would pass
    // SAI_NULL_OBJECT_ID (because Port.m_port_id for a LAG is 0) and be
    // rejected by the SAI. Fan the port-profile out across the LAG's
    // physical member ports instead so each contributes the correct
    // per-port EWMA weighting / scaling factor.
    Port p;
    if (m_portsOrch->getPort(portName, p) && p.m_type == Port::LAG)
    {
        if (p.m_members.empty())
        {
            SWSS_LOG_NOTICE("ARS: PortChannel %s has no members yet — "
                            "deferring port-profile '%s' application until "
                            "members join", portName.c_str(), profileName.c_str());
            return;
        }
        SWSS_LOG_NOTICE("ARS: fanning port-profile '%s' across %zu member(s) "
                        "of PortChannel %s", profileName.c_str(),
                        p.m_members.size(), portName.c_str());
        for (const auto &member : p.m_members)
        {
            // Recurse with the member name; each member is a physical port
            // so the second branch below applies.
            applyPortProfileToInterface(member, profileName);
        }
        return;
    }

    if (pp.loadScalingFactor > 0 || pp.loadScalingFactorAuto)
        setPortArsScalingFactor(portName, pp);

    if (pp.portLoadPastWeight > 0 || pp.portLoadFutureWeight > 0)
        setPortArsWeights(portName, pp.portLoadPastWeight, pp.portLoadFutureWeight);

    if (pp.loadPastMinVal > 0 || pp.loadPastMaxVal > 0 ||
        pp.loadFutureMinVal > 0 || pp.loadFutureMaxVal > 0 ||
        pp.loadCurrentMinVal > 0 || pp.loadCurrentMaxVal > 0)
    {
        setPortArsLoadBands(portName, pp);
    }
}

/* ── Publish ARS capabilities to STATE_DB ─────────────────────────────── */

void ArsOrch::publishArsCaps()
{
    SWSS_LOG_ENTER();

    vector<FieldValueTuple> caps;

    sai_attr_capability_t modeCap = {};
    sai_status_t status = sai_query_attribute_capability(
        gSwitchId, SAI_OBJECT_TYPE_ARS, SAI_ARS_ATTR_MODE, &modeCap);

    bool arsSupported = (status == SAI_STATUS_SUCCESS && modeCap.create_implemented);
    caps.emplace_back("ars_supported", arsSupported ? "true" : "false");

    string modesStr;
    static const vector<pair<string, sai_ars_mode_t>> modeProbes = {
        {"flowlet-quality",  SAI_ARS_MODE_FLOWLET_QUALITY},
        {"packet-quality",   SAI_ARS_MODE_PER_PACKET_QUALITY},
    };

    if (arsSupported)
    {
        // Query the enum capability once into a vector that we grow on
        // SAI_STATUS_BUFFER_OVERFLOW rather than silently truncating. Then
        // probe each mode of interest against the collected list. Doing the
        // query once (not per mode) also avoids an N× SAI round-trip.
        std::vector<int32_t> enumList(16);
        sai_s32_list_t enumCap = {};
        enumCap.count = (uint32_t)enumList.size();
        enumCap.list  = enumList.data();
        sai_status_t qs = sai_query_attribute_enum_values_capability(
            gSwitchId, SAI_OBJECT_TYPE_ARS, SAI_ARS_ATTR_MODE, &enumCap);
        if (qs == SAI_STATUS_BUFFER_OVERFLOW)
        {
            enumList.resize(enumCap.count);
            enumCap.list = enumList.data();
            qs = sai_query_attribute_enum_values_capability(
                gSwitchId, SAI_OBJECT_TYPE_ARS, SAI_ARS_ATTR_MODE, &enumCap);
        }

        for (const auto &modePair : modeProbes)
        {
            const auto &name = modePair.first;
            const auto &mode = modePair.second;

            bool found = false;
            if (qs == SAI_STATUS_SUCCESS)
            {
                for (uint32_t i = 0; i < enumCap.count; i++)
                {
                    if (enumCap.list[i] == (int32_t)mode)
                    {
                        found = true;
                        break;
                    }
                }
            }
            else
            {
                // Platforms that don't implement the enum capability query
                // advertise everything — this matches the previous behavior.
                found = true;
            }

            if (found)
            {
                if (!modesStr.empty()) modesStr += ",";
                modesStr += name;
            }
        }
    }
    else
    {
        modesStr = "flowlet-quality,packet-quality";
    }
    caps.emplace_back("modes_supported", modesStr);

    static const vector<pair<string, sai_ars_attr_t>> attrProbes = {
        {"SAI_ARS_ATTR_MODE",      SAI_ARS_ATTR_MODE},
        {"SAI_ARS_ATTR_IDLE_TIME", SAI_ARS_ATTR_IDLE_TIME},
        {"SAI_ARS_ATTR_MAX_FLOWS", SAI_ARS_ATTR_MAX_FLOWS},
    };
    for (const auto &attrPair : attrProbes)
    {
        const auto &attrName = attrPair.first;
        const auto &attrId = attrPair.second;
        sai_attr_capability_t ac = {};
        sai_status_t qs = sai_query_attribute_capability(
            gSwitchId, SAI_OBJECT_TYPE_ARS, attrId, &ac);
        string capStr = "unknown";
        if (qs == SAI_STATUS_SUCCESS)
        {
            capStr = string("create=") + (ac.create_implemented ? "true" : "false") +
                     ",set=" + (ac.set_implemented ? "true" : "false") +
                     ",get=" + (ac.get_implemented ? "true" : "false");
        }
        m_stateArsCapTable.set(attrName, {{attrName, capStr}});
    }

    m_stateArsCapTable.set("switch", caps);
}

/* ── Publish ARS Profile state to STATE_DB (incl. default_ars_object) ── */

void ArsOrch::publishArsProfileState(const string &profileName, const ArsProfileEntry &entry)
{
    vector<FieldValueTuple> fvs;

    // SAI identity first so operators can cross-reference against sairedis.
    fvs.emplace_back("profile_oid",
                     sai_serialize_object_id(entry.profileOid));
    fvs.emplace_back("is_bound",
                     (entry.profileOid != SAI_NULL_OBJECT_ID &&
                      entry.profileOid == m_activeSwitchProfileOid) ? "true" : "false");

    // Mirror the in-orchagent view of the EWMA tuning so 'show' commands and
    // external monitors have a source of truth for *applied* config (as
    // opposed to whatever CONFIG_DB currently contains — which may have
    // been rejected by a validator and left unpropagated).
    fvs.emplace_back("algorithm",                std::to_string((int)entry.algorithm));
    fvs.emplace_back("port_load_past",           entry.loadPastEnable ? "true" : "false");
    fvs.emplace_back("port_load_past_weight",    std::to_string(entry.loadPastWeight));
    fvs.emplace_back("port_load_future",         entry.loadFutureEnable ? "true" : "false");
    fvs.emplace_back("port_load_future_weight",  std::to_string(entry.loadFutureWeight));
    fvs.emplace_back("port_load_current",        entry.loadCurrentEnable ? "true" : "false");
    fvs.emplace_back("port_load_current_weight", std::to_string(entry.loadCurrentWeight));
    fvs.emplace_back("load_exponent",            std::to_string(entry.loadExponent));
    fvs.emplace_back("max_flows",                std::to_string(entry.maxFlows));
    fvs.emplace_back("load_past_min_val",        std::to_string(entry.loadPastMinVal));
    fvs.emplace_back("load_past_max_val",        std::to_string(entry.loadPastMaxVal));
    fvs.emplace_back("load_future_min_val",      std::to_string(entry.loadFutureMinVal));
    fvs.emplace_back("load_future_max_val",      std::to_string(entry.loadFutureMaxVal));
    fvs.emplace_back("load_current_min_val",     std::to_string(entry.loadCurrentMinVal));
    fvs.emplace_back("load_current_max_val",     std::to_string(entry.loadCurrentMaxVal));
    fvs.emplace_back("ipv4_enable",              entry.ipv4Enable ? "true" : "false");
    fvs.emplace_back("ipv6_enable",              entry.ipv6Enable ? "true" : "false");
    fvs.emplace_back("sampling_interval",        std::to_string(entry.samplingInterval));
    fvs.emplace_back("random_seed",              std::to_string(entry.randomSeed));
    // quant-band thresholds land here now that feat/ars-quant-band-thresholds
    // (upscale-ai-network/sonic-swss#15) has been merged into this branch
    // and ArsProfileEntry carries the fields.
    fvs.emplace_back("quant_band_0_min_threshold", std::to_string(entry.quantBand0MinThreshold));
    fvs.emplace_back("quant_band_1_min_threshold", std::to_string(entry.quantBand1MinThreshold));
    fvs.emplace_back("quant_band_2_min_threshold", std::to_string(entry.quantBand2MinThreshold));

    fvs.emplace_back("default_ars_object", entry.defaultArsObject);

    if (!entry.defaultArsObject.empty())
    {
        auto objIt = m_arsObjects.find(entry.defaultArsObject);
        if (objIt != m_arsObjects.end() && objIt->second.arsOid != SAI_NULL_OBJECT_ID)
        {
            fvs.emplace_back("default_ars_object_oid",
                             sai_serialize_object_id(objIt->second.arsOid));
        }
        else
        {
            fvs.emplace_back("default_ars_object_oid", "N/A");
        }
    }

    m_stateArsProfileTable.set(profileName, fvs);
    SWSS_LOG_NOTICE("ARS: published profile '%s' state to STATE_DB "
                    "(is_bound=%s, default_ars_object=%s)",
                    profileName.c_str(),
                    (entry.profileOid != SAI_NULL_OBJECT_ID &&
                     entry.profileOid == m_activeSwitchProfileOid) ? "true" : "false",
                    entry.defaultArsObject.c_str());
}

/* ── Helpers ──────────────────────────────────────────────────────────── */

bool ArsOrch::parseArsMode(const string &modeStr, sai_ars_mode_t *out) const
{
    string lower = toLower(modeStr);
    auto it = arsModeLookup.find(lower);
    if (it == arsModeLookup.end())
        return false;
    if (out)
        *out = it->second;
    return true;
}

sai_object_id_t ArsOrch::getArsProfileOid(const string &name) const
{
    auto it = m_arsProfiles.find(name);
    return (it != m_arsProfiles.end()) ? it->second.profileOid : SAI_NULL_OBJECT_ID;
}

sai_object_id_t ArsOrch::getArsObjectOid(const string &name) const
{
    auto it = m_arsObjects.find(name);
    return (it != m_arsObjects.end()) ? it->second.arsOid : SAI_NULL_OBJECT_ID;
}

void ArsOrch::createDefaultProfileIfNeeded()
{
    SWSS_LOG_ENTER();

    static const string kDefaultName = "__ARS_DEFAULT__";
    if (m_arsProfiles.count(kDefaultName))
        return;

    ArsProfileEntry entry;
    // Seed non-zero quant-band thresholds on the auto-created default so the
    // Mellanox SAI backend doesn't classify the profile as "hardened". A
    // hardened profile (all three band*_min_thresholds == 0) makes SAI refuse
    // to create flowlet-quality ARS objects outright — see
    // mlnx_sai_create_ars / is_hardened_profile_bound. Without this the
    // documented "bare minimum" path (ARS|GLOBAL admin_state=up → create an
    // ARS_OBJECT with assign_mode=flowlet-quality) fails at the ARS_OBJECT
    // create call with SAI_STATUS_INVALID_ATTR_VALUE and the operator is
    // left with no flowlet behavior despite a clean CONFIG_DB.
    //
    // The values here are deliberately conservative (1/2/4 Gbps in Mbps)
    // and strictly monotonic as required by SAI; operators who want tighter
    // bands for their topology should create an explicit ARS_PROFILE and
    // bind it via ARS|GLOBAL.profile.
    entry.quantBand0MinThreshold = 1000;
    entry.quantBand1MinThreshold = 2000;
    entry.quantBand2MinThreshold = 4000;
    if (!createArsProfile(kDefaultName, entry))
    {
        SWSS_LOG_WARN("ARS: failed to auto-create default profile — "
                      "SAI will use built-in defaults");
        return;
    }

    m_globalProfileName = kDefaultName;
    SWSS_LOG_NOTICE("ARS: auto-created default profile '%s' with SDK defaults",
                    kDefaultName.c_str());

    // Mirror the auto-created default into STATE_DB so 'show load-balance
    // adaptive' and other readers can see the effective profile (OID,
    // bound status, EWMA tuning) without having to infer that it exists
    // from the CONFIG_DB absence of a user-defined profile.
    auto it = m_arsProfiles.find(kDefaultName);
    if (it != m_arsProfiles.end())
        publishArsProfileState(kDefaultName, it->second);
}

string ArsOrch::getArsObjectForPrefix(const string &prefix) const
{
    auto it = m_nexthopArsBindings.find(prefix);
    return (it != m_nexthopArsBindings.end()) ? it->second : "";
}

void ArsOrch::writeArsNhgState(const string &nhgName, bool degraded, const string &reason)
{
    vector<FieldValueTuple> fvs;
    fvs.emplace_back("status", degraded ? "degraded" : "active");
    if (degraded && !reason.empty())
        fvs.emplace_back("reason", reason);
    m_stateArsNhgTable.set(nhgName, fvs);
}

void ArsOrch::removeArsNhgState(const string &nhgName)
{
    m_stateArsNhgTable.del(nhgName);
}
