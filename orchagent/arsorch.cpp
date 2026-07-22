#include "arsorch.h"
#include "routeorch.h"
#include "neighorch.h"
#include "intfsorch.h"
#include "crmorch.h"
#include "swssnet.h"
#include "switchorch.h"
#include "portsorch.h"
#include "logger.h"
#include "schema.h"
#include "tokenize.h"
#include "sai_serialize.h"
#include "converter.h"
#include "directory.h"
#include "swssnet.h"
#include "warm_restart.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <exception>
#include <unordered_set>
#include <unistd.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <arpa/inet.h>

using namespace std;
using namespace swss;

extern sai_ars_api_t*            sai_ars_api;
extern sai_ars_profile_api_t*    sai_ars_profile_api;
extern sai_switch_api_t*         sai_switch_api;
extern sai_next_hop_group_api_t* sai_next_hop_group_api;
extern sai_lag_api_t*            sai_lag_api;
extern sai_port_api_t*                  sai_port_api;
extern sai_router_interface_api_t*      sai_router_intfs_api;
extern sai_neighbor_api_t*              sai_neighbor_api;
extern sai_next_hop_api_t*              sai_next_hop_api;
extern sai_route_api_t*                 sai_route_api;
extern sai_object_id_t                  gSwitchId;
extern sai_object_id_t                  gVirtualRouterId;
extern RouteOrch                       *gRouteOrch;
extern NeighOrch                       *gNeighOrch;
extern IntfsOrch                       *gIntfsOrch;
extern CrmOrch                         *gCrmOrch;

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

    // Retry a previously-failed setPortArsEnable. doArsInterfaceTask
    // queues a port here when its CONFIG_DB ARS_INTERFACES row requested
    // admin_state=up but the SAI enable was rejected (port OID not yet
    // published by PortsOrch at cold boot / config-reload time, or the
    // port briefly carried a RIF). The port-up notification is a good
    // retry moment: by the time the oper state reaches UP, PortsOrch
    // has certainly created the SAI port object and IntfsOrch has
    // either not yet created the RIF (setup path — egress ports that
    // came up without IPs configured, as in setup_lab.py's ARS path)
    // or has already finished creating it (in which case we're out of
    // luck and the retry will fail again, but failing at port-up and
    // leaving the port queued is still better than never retrying at
    // all on platforms where the transient clears by itself).
    if (m_arsInterfacesPendingEnable.count(portName))
    {
        SWSS_LOG_NOTICE("ARS: port %s came up — retrying previously-failed "
                        "setPortArsEnable", portName.c_str());
        if (setPortArsEnable(portName, true))
        {
            m_arsInterfacesPendingEnable.erase(portName);
            m_arsEnabledPorts.insert(portName);
            auto it = m_arsInterfaces.find(portName);
            if (it != m_arsInterfaces.end())
            {
                it->second.enabled = true;
                SWSS_LOG_NOTICE("ARS: port %s ARS now enabled on retry",
                                portName.c_str());
                // Ports becoming ARS-enabled makes previously-degraded
                // NHGs eligible for binding. RouteOrch::bindArsToExistingNhgs
                // calls resolveArsForNhg which will now find ars_object for
                // this port, and (on platforms where SAI_NEXT_HOP_GROUP_ATTR
                // _ARS_OBJECT_ID is settable on an existing NHG) attach the
                // ARS object. On Mellanox the set is rejected on populated
                // NHGs (write-once-at-create), so the retry merely restores
                // correct m_arsInterfaces state — the operator still needs
                // a route flap to materialize the binding.
                if (gRouteOrch && m_arsEnabled)
                    gRouteOrch->bindArsToExistingNhgs();
            }
        }
        else
        {
            SWSS_LOG_WARN("ARS: retry of setPortArsEnable on %s still failed "
                          "— leaving queued", portName.c_str());
        }
    }

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

/*
 * Override Orch::doTask() to enforce ARS_PROFILE-before-ARS_INTERFACES
 * ordering during warm restart replay.
 *
 * The base Orch::doTask() iterates m_consumerMap alphabetically, which
 * processes ARS_INTERFACES before ARS_PROFILE. During normal operation
 * the retry queue (m_arsInterfacesPendingEnable) handles this, but
 * during warm restart the TEMP view recorded in syncd must have the
 * profile created before port ARS enables so that the APPLY_VIEW
 * comparison generates operations in a safe order.
 *
 * Outside of warm restart, fall through to the default alphabetical
 * iteration.
 */
void ArsOrch::doTask()
{
    if (!WarmStart::isWarmStart())
    {
        Orch::doTask();
        return;
    }

    static const vector<string> priorityTables = {
        CFG_ARS_TABLE_NAME,
        CFG_ARS_PROFILE_TABLE_NAME,
    };

    for (const auto &name : priorityTables)
    {
        auto it = m_consumerMap.find(name);
        if (it != m_consumerMap.end())
            it->second->drain();
    }

    for (auto &it : m_consumerMap)
    {
        const auto &name = it.first;
        bool already = false;
        for (const auto &p : priorityTables)
        {
            if (name == p) { already = true; break; }
        }
        if (!already)
            it.second->drain();
    }
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
            bool hasProfileField = false;
            for (auto &fv : kfvFieldsValues(kfv))
            {
                if (fvField(fv) == "admin_state")
                    wantEnable = (toLower(fvValue(fv)) == "up");
                else if (fvField(fv) == "profile")
                {
                    profileName = fvValue(fv);
                    hasProfileField = true;
                }
            }

            if (wantEnable && !m_arsEnabled)
            {
                SWSS_LOG_NOTICE("ARS: Adaptive Routing globally enabled");
                m_arsEnabled = true;

                // ARS requires DYNAMIC_ORDERED_ECMP NHGs.  The uCLI
                // auto-sets ecmp_type=ordered in CONFIG_DB when the user
                // runs `load-balance adaptive enable`.  If orchagent sees
                // static ECMP at this point, CONFIG_DB was written directly
                // (bypassing uCLI) or the ecmp_type update hasn't been
                // processed by SwitchOrch yet.  Log a warning; existing
                // NHGs may need a route flap or config reload to pick up
                // the ordered type.
                if (m_switchOrch && !m_switchOrch->checkOrderedEcmpEnable())
                {
                    SWSS_LOG_WARN("ARS: Adaptive Routing enabled while ECMP "
                                  "type is still 'static'. The uCLI should "
                                  "have auto-set ecmp_type=ordered; if "
                                  "CONFIG_DB was written directly, run "
                                  "'ecmp type ordered' first. Existing NHGs "
                                  "may need to be re-created.");
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

            if (hasProfileField && !profileName.empty() && profileName != m_globalProfileName)
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
            else if (hasProfileField && profileName.empty() && !m_globalProfileName.empty())
            {
                if (!m_arsEnabledPorts.empty())
                {
                    SWSS_LOG_NOTICE("ARS: deferring profile unbind — %zu port(s) "
                                    "still ARS-enabled in ASIC. Will reconcile "
                                    "on config reload.", m_arsEnabledPorts.size());
                    m_activeSwitchProfileOid = SAI_NULL_OBJECT_ID;
                    m_globalProfileName.clear();
                }
                else if (bindArsProfileToSwitch(SAI_NULL_OBJECT_ID))
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
                if (m_arsEnabledPorts.empty())
                {
                    bindArsProfileToSwitch(SAI_NULL_OBJECT_ID);
                }
                else
                {
                    SWSS_LOG_NOTICE("ARS: deferring profile unbind from switch "
                                    "— %zu port(s) still ARS-enabled in ASIC. "
                                    "Will reconcile on config reload.",
                                    m_arsEnabledPorts.size());
                    m_activeSwitchProfileOid = SAI_NULL_OBJECT_ID;
                }
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
            //
            // Only validate when ALL three bands have been set to non-zero
            // values. uCLI writes thresholds incrementally (one field per
            // CONFIG_DB update), so intermediate states with only band0 set
            // are expected and should not be rejected.
            bool allQuantBandsSet = (entry.quantBand0MinThreshold != 0 &&
                                     entry.quantBand1MinThreshold != 0 &&
                                     entry.quantBand2MinThreshold != 0);
            if (allQuantBandsSet)
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
                    if (!bindArsProfileToSwitch(entry.profileOid))
                    {
                        SWSS_LOG_ERROR("ARS: profile '%s' created but switch "
                                       "bind failed — ARS inactive until "
                                       "profile is re-bound", name.c_str());
                    }
                }
            }
            else
            {
                // Mellanox SAI only implements 3 of the ~20 ARS_PROFILE
                // attributes for SET: the quant-band thresholds. All others
                // (PORT_LOAD_*, LOAD_EXPONENT, MAX_FLOWS, SAMPLING_INTERVAL,
                // ENABLE_IPV4/6, etc.) are absent from the vendor attrib
                // table (ars_profile_vendor_attribs[] in mlnx_sai_ars.c).
                // syncd rejects any set_ars_profile_attribute for an
                // unregistered attr with "vendor data not found" and sends
                // switch_shutdown_request, killing orchagent instantly.
                //
                // Only push quant-band thresholds (the sole SET-capable
                // attrs on Mellanox). EWMA tuning is handled by the SDK /
                // ars-classifier-daemon, not by SAI profile attrs.
                bool anyFailed = false;
                // Push quant-band thresholds only when the full triple is
                // valid: either all three are non-zero (complete config), or
                // a previously-valid triple is being cleared to all-zero.
                // Intermediate incremental states (e.g. band0=10, band1=0,
                // band2=0) are cached locally but NOT pushed to SAI —
                // Mellanox SAI rejects partial sets with INVALID_PARAMETER
                // and enters shutdown-wait, killing syncd.
                bool allQuantBandsZero = (entry.quantBand0MinThreshold == 0 &&
                                          entry.quantBand1MinThreshold == 0 &&
                                          entry.quantBand2MinThreshold == 0);
                bool pushQuantBands = allQuantBandsSet ||
                                     (hadQuantBandConfig && allQuantBandsZero);
                if (pushQuantBands)
                {
                    sai_object_id_t oid = entry.profileOid;
                    anyFailed |= !updateArsProfileAttr(oid, SAI_ARS_PROFILE_ATTR_QUANT_BAND_0_MIN_THRESHOLD,
                                                       entry.quantBand0MinThreshold);
                    anyFailed |= !updateArsProfileAttr(oid, SAI_ARS_PROFILE_ATTR_QUANT_BAND_1_MIN_THRESHOLD,
                                                       entry.quantBand1MinThreshold);
                    anyFailed |= !updateArsProfileAttr(oid, SAI_ARS_PROFILE_ATTR_QUANT_BAND_2_MIN_THRESHOLD,
                                                       entry.quantBand2MinThreshold);
                }
                if (anyFailed)
                {
                    SWSS_LOG_WARN("ARS: one or more quant-band threshold SETs "
                                  "failed for profile '%s'", name.c_str());
                }
                m_arsProfiles[name] = entry;
                publishArsProfileState(name, entry);
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

    // Mellanox SAI requires an ARS profile to exist before
    // SAI_PORT_ATTR_ARS_ENABLE can succeed.  Because m_consumerMap is
    // sorted alphabetically, ARS_INTERFACES is processed before
    // ARS_PROFILE at boot — so doArsInterfaceTask's setPortArsEnable
    // calls fail with SAI_STATUS_NOT_SUPPORTED ("ARS profile is not
    // created") and the ports are queued in m_arsInterfacesPendingEnable.
    // Now that a profile exists, retry those deferred enables while the
    // ports are still bare (no RIF yet — IntfsOrch hasn't run).
    //
    // Only retry if at least one profile was successfully created in
    // this batch — if all profile creates failed (e.g.
    // SAI_STATUS_ATTR_NOT_IMPLEMENTED), there's no profile in SAI and
    // the port enables would fail again for the same reason.
    bool anyProfileExists = false;
    for (const auto &p : m_arsProfiles)
    {
        if (p.second.profileOid != SAI_NULL_OBJECT_ID)
        {
            anyProfileExists = true;
            break;
        }
    }

    if (anyProfileExists && !m_arsInterfacesPendingEnable.empty())
    {
        SWSS_LOG_NOTICE("ARS: profile created — retrying %zu pending "
                        "port ARS enables",
                        m_arsInterfacesPendingEnable.size());

        auto pending = m_arsInterfacesPendingEnable;
        for (const auto &portName : pending)
        {
            if (setPortArsEnable(portName, true))
            {
                m_arsInterfacesPendingEnable.erase(portName);
                m_arsEnabledPorts.insert(portName);
                auto ifIt = m_arsInterfaces.find(portName);
                if (ifIt != m_arsInterfaces.end())
                    ifIt->second.enabled = true;
                SWSS_LOG_NOTICE("ARS: deferred enable on %s succeeded "
                                "after profile creation",
                                portName.c_str());
            }
            else
            {
                SWSS_LOG_WARN("ARS: deferred enable on %s still failed "
                              "after profile creation — leaving queued",
                              portName.c_str());
            }
        }
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
                // Mellanox SAI rejects set_ars_attribute with
                // OBJECT_IN_USE once any NHG references the object, and
                // syncd treats the error as fatal.  Since NHGs bind
                // almost immediately after creation, we block all SETs
                // and cache values locally.  To apply new values the
                // operator must delete + re-create the ARS object.
                if (prev.saiSetBlocked)
                {
                    if (prev.idleTime != entry.idleTime ||
                        prev.maxFlows != entry.maxFlows)
                    {
                        SWSS_LOG_NOTICE("ARS: object '%s' attribute update "
                                        "cached locally (idle %u→%u, flows "
                                        "%u→%u) — SAI SETs skipped because "
                                        "NHGs may reference the object "
                                        "(Mellanox OBJECT_IN_USE limitation)",
                                        name.c_str(), prev.idleTime,
                                        entry.idleTime, prev.maxFlows,
                                        entry.maxFlows);
                    }
                    entry.saiSetBlocked = true;
                }
                else
                {
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

    // Enable batch migration so migratePortToArs defers NHG member
    // re-addition (Phase 10) and NHG recreation (Phase 11). This prevents
    // the transient mixed AR / non-AR member state that crashes the
    // Mellanox SDK.
    m_batchMigrationMode = true;

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
                {
                    m_arsEnabledPorts.insert(portName);
                    m_arsInterfacesPendingEnable.erase(portName);
                }
                else
                {
                    // SAI rejected the enable. Most common causes: PortsOrch
                    // hasn't published the port's OID yet (cold boot /
                    // config-reload race), or the port is carrying a RIF
                    // that must be removed first. Queue for retry on the
                    // next PORT_OPER_STATE_CHANGE=UP so we don't leave the
                    // port permanently misbound when the transient clears.
                    SWSS_LOG_ERROR("ARS: failed to enable ARS on port %s — "
                                   "keeping entry.enabled=false so NHG resolver "
                                   "does not bind ARS on this port; queued "
                                   "for retry on next port-up event",
                                   portName.c_str());
                    entry.enabled = false;
                    m_arsInterfacesPendingEnable.insert(portName);
                }
            }
            else if (!entry.enabled && prevEnabled)
            {
                if (setPortArsEnable(portName, false))
                    m_arsEnabledPorts.erase(portName);
                else
                    SWSS_LOG_ERROR("ARS: failed to disable ARS on port %s", portName.c_str());
                // Explicit admin_state=down cancels any pending retry.
                m_arsInterfacesPendingEnable.erase(portName);
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
            // Interface entry is gone — no enable intent remains to retry.
            m_arsInterfacesPendingEnable.erase(portName);
        }

        it = consumer.m_toSync.erase(it);
    }

    m_batchMigrationMode = false;

    // Process deferred NHG members accumulated during batch migrations.
    // This binds ARS to now-empty NHGs first, then adds all members — all
    // using AR RIFs — avoiding the mixed AR/non-AR state that crashes the
    // Mellanox SDK.
    if (!m_deferredNhgMembers.empty())
    {
        processDeferredNhgMembers();
    }

    // Only re-evaluate NHG bindings if something resolver-relevant changed
    // this batch. Skipping no-op re-binds keeps sairedis.rec clean and, on
    // Mellanox, avoids a flurry of INVALID_PARAMETER failures from the
    // set-on-populated-NHG restriction (see commit notes on ARS NHG
    // binding immutability).
    if (resolverInputsChanged && gRouteOrch && m_arsEnabled)
        gRouteOrch->bindArsToExistingNhgs();

    // Retry deferred members that couldn't be processed earlier (e.g.
    // because admin_state arrived before ars_object in a prior batch).
    // Now that arsObject fields may have been updated in this batch,
    // processDeferredNhgMembers may succeed where it previously failed.
    if (!m_deferredNhgMembers.empty())
    {
        SWSS_LOG_NOTICE("ARS-BATCH: retrying %zu deferred NHG member(s) after "
                        "resolver inputs changed",
                        m_deferredNhgMembers.size());
        processDeferredNhgMembers();
    }
}

/* ── Deferred NHG member processing (batch migration) ───────────────── */

void ArsOrch::processDeferredNhgMembers()
{
    SWSS_LOG_ENTER();

    SWSS_LOG_NOTICE("ARS-BATCH: processing %zu deferred NHG member(s)",
                    m_deferredNhgMembers.size());

    map<NextHopGroupKey, vector<DeferredNhgMember>> byNhg;
    for (auto &dm : m_deferredNhgMembers)
        byNhg[dm.nhgKey].push_back(dm);
    m_deferredNhgMembers.clear();

    for (auto &nhgEntry : byNhg)
    {
        const NextHopGroupKey &nhgKey = nhgEntry.first;
        const vector<DeferredNhgMember> &members = nhgEntry.second;

        auto &syncdNhgs = gRouteOrch->getSyncdNextHopGroups();
        auto nhgIt = syncdNhgs.find(nhgKey);
        if (nhgIt == syncdNhgs.end() ||
            nhgIt->second.next_hop_group_id == SAI_NULL_OBJECT_ID)
        {
            SWSS_LOG_WARN("ARS-BATCH: NHG %s not found — skipping %zu members",
                          nhgKey.to_string().c_str(), members.size());
            continue;
        }

        sai_object_id_t nhgOid = nhgIt->second.next_hop_group_id;

        // Check whether ALL remaining (non-deferred) NHG members' ports are
        // already ARS-enabled.  If any existing member's port hasn't migrated
        // yet, adding our AR-enabled deferred members would create a mixed
        // AR / non-AR state and crash the Mellanox SDK.  In that case, put
        // the deferred members back and wait for the next port migration.
        bool allExistingMembersAr = true;
        for (auto &existingMember : nhgIt->second.nhopgroup_members)
        {
            const string &portName = existingMember.first.alias;
            if (m_arsEnabledPorts.find(portName) == m_arsEnabledPorts.end())
            {
                allExistingMembersAr = false;
                SWSS_LOG_NOTICE("ARS-BATCH: NHG %s member port %s not yet "
                                "ARS-enabled — deferring %zu members until "
                                "all ports migrate",
                                nhgKey.to_string().c_str(), portName.c_str(),
                                members.size());
                break;
            }
        }

        if (!allExistingMembersAr)
        {
            for (auto &dm : members)
                m_deferredNhgMembers.push_back(dm);
            continue;
        }

        SWSS_LOG_NOTICE("ARS-BATCH: NHG %s (oid=0x%" PRIx64 ") has %zu existing "
                        "members (all AR), %zu deferred members to add",
                        nhgKey.to_string().c_str(), nhgOid,
                        nhgIt->second.nhopgroup_members.size(), members.size());

        // All existing members are AR-capable (or NHG is empty). Safe to
        // proceed with ARS NHG setup and deferred member addition.
        //
        // CRITICAL: On Mellanox, binding ARS to an EXISTING NHG via
        // set_attribute does NOT properly convert the hardware ECMP to AR
        // mode — the SAI call succeeds but the SDK crashes ~1s later when
        // AR members are added. We MUST use make-before-break: create a
        // NEW NHG, bind ARS at creation, add members, repoint routes,
        // delete old NHG.
        if (nhgIt->second.nhopgroup_members.empty())
        {
            auto arsOid = resolveArsForNhg(nhgOid, nhgKey);

            if (arsOid == SAI_NULL_OBJECT_ID)
            {
                SWSS_LOG_NOTICE("ARS-BATCH: resolveArsForNhg returned NULL for "
                                "NHG %s — trying direct lookup from deferred "
                                "members' port config and NHG key ports",
                                nhgKey.to_string().c_str());

                // Strategy 1: check deferred members' port config
                for (auto &dm : members)
                {
                    auto ifIt = m_arsInterfaces.find(dm.nhKey.alias);
                    if (ifIt != m_arsInterfaces.end() &&
                        !ifIt->second.arsObject.empty())
                    {
                        auto objIt = m_arsObjects.find(ifIt->second.arsObject);
                        if (objIt != m_arsObjects.end() &&
                            objIt->second.arsOid != SAI_NULL_OBJECT_ID)
                        {
                            arsOid = objIt->second.arsOid;
                            SWSS_LOG_NOTICE("ARS-BATCH: resolved ARS oid=0x%" PRIx64
                                            " from deferred member port %s -> object '%s'",
                                            arsOid, dm.nhKey.alias.c_str(),
                                            ifIt->second.arsObject.c_str());
                            break;
                        }
                    }
                }

                // Strategy 2: check ALL ports in the NHG key (covers case
                // where admin_state and ars_object arrive in separate batches
                // and deferred members' ports haven't had arsObject set yet)
                if (arsOid == SAI_NULL_OBJECT_ID)
                {
                    for (const auto &nh : nhgKey.getNextHops())
                    {
                        auto ifIt = m_arsInterfaces.find(nh.alias);
                        if (ifIt != m_arsInterfaces.end() &&
                            !ifIt->second.arsObject.empty())
                        {
                            auto objIt = m_arsObjects.find(ifIt->second.arsObject);
                            if (objIt != m_arsObjects.end() &&
                                objIt->second.arsOid != SAI_NULL_OBJECT_ID)
                            {
                                arsOid = objIt->second.arsOid;
                                SWSS_LOG_NOTICE("ARS-BATCH: resolved ARS oid=0x%" PRIx64
                                                " from NHG key port %s -> object '%s'",
                                                arsOid, nh.alias.c_str(),
                                                ifIt->second.arsObject.c_str());
                                break;
                            }
                        }
                    }
                }

                // Strategy 3: if only one ARS object exists (common on
                // Mellanox which supports a single ARS object), use it
                // directly. This handles the case where no port in the
                // NHG has its arsObject field set yet but the ARS object
                // was already created.
                if (arsOid == SAI_NULL_OBJECT_ID && m_arsObjects.size() == 1)
                {
                    auto &soleObj = m_arsObjects.begin()->second;
                    if (soleObj.arsOid != SAI_NULL_OBJECT_ID)
                    {
                        arsOid = soleObj.arsOid;
                        SWSS_LOG_NOTICE("ARS-BATCH: resolved ARS oid=0x%" PRIx64
                                        " from sole ARS object '%s' (no port had "
                                        "arsObject set yet — admin_state/ars_object "
                                        "split-notification race)",
                                        arsOid,
                                        m_arsObjects.begin()->first.c_str());
                    }
                }
            }

            if (arsOid == SAI_NULL_OBJECT_ID)
            {
                SWSS_LOG_ERROR("ARS-BATCH: cannot resolve any ARS object for "
                               "NHG %s — keeping %zu members deferred",
                               nhgKey.to_string().c_str(), members.size());
                for (auto &dm : members)
                    m_deferredNhgMembers.push_back(dm);
                continue;
            }

            // Make-before-break: create new NHG with ARS, add members,
            // repoint routes, delete old NHG.
            SWSS_LOG_NOTICE("ARS-BATCH: make-before-break for NHG %s "
                            "(old_oid=0x%" PRIx64 " ars=0x%" PRIx64 ")",
                            nhgKey.to_string().c_str(), nhgOid, arsOid);

            // Step 1+2: Create new NHG with ARS bound at creation time.
            // On Mellanox the NHG-ARS binding is write-once and MUST be set
            // in the create_next_hop_group attribute list — a post-creation
            // set_next_hop_group_attribute(ARS) is rejected/unreliable (and
            // bindArsToNhg() short-circuits for an OID not yet tracked in
            // m_syncdNextHopGroups). Include SAI_NEXT_HOP_GROUP_ATTR_ARS_OBJECT_ID
            // directly so the SDK programs the ECMP as AR-capable from birth.
            sai_object_id_t newNhgOid;
            {
                vector<sai_attribute_t> nhg_attrs;
                sai_attribute_t attr;
                attr.id = SAI_NEXT_HOP_GROUP_ATTR_TYPE;
                attr.value.s32 = m_switchOrch->getEcmpNhgType();
                nhg_attrs.push_back(attr);

                attr.id = SAI_NEXT_HOP_GROUP_ATTR_ARS_OBJECT_ID;
                attr.value.oid = arsOid;
                nhg_attrs.push_back(attr);

                sai_status_t st = sai_next_hop_group_api->create_next_hop_group(
                    &newNhgOid, gSwitchId,
                    (uint32_t)nhg_attrs.size(), nhg_attrs.data());
                if (st != SAI_STATUS_SUCCESS)
                {
                    SWSS_LOG_ERROR("ARS-BATCH: failed to create new NHG with ARS "
                                   "0x%" PRIx64 " rc=%d — keeping members deferred",
                                   arsOid, st);
                    for (auto &dm : members)
                        m_deferredNhgMembers.push_back(dm);
                    continue;
                }
                gCrmOrch->incCrmResUsedCounter(CrmResourceType::CRM_NEXTHOP_GROUP);
            }

            SWSS_LOG_NOTICE("ARS-BATCH: created new NHG 0x%" PRIx64 " with ARS "
                            "0x%" PRIx64 " bound at creation", newNhgOid, arsOid);

            // Step 3: Add all deferred members to new NHG
            NextHopGroupMembers newMembers;
            bool memberFail = false;
            for (auto &dm : members)
            {
                auto &syncdNhs = gNeighOrch->getSyncdNextHops();
                auto nhIt = syncdNhs.find(dm.nhKey);
                if (nhIt == syncdNhs.end() ||
                    nhIt->second.next_hop_id == SAI_NULL_OBJECT_ID)
                {
                    SWSS_LOG_WARN("ARS-BATCH: NH %s not found — skipping",
                                  dm.nhKey.to_string().c_str());
                    continue;
                }

                vector<sai_attribute_t> member_attrs;
                sai_attribute_t m_attr;

                m_attr.id = SAI_NEXT_HOP_GROUP_MEMBER_ATTR_NEXT_HOP_GROUP_ID;
                m_attr.value.oid = newNhgOid;
                member_attrs.push_back(m_attr);

                m_attr.id = SAI_NEXT_HOP_GROUP_MEMBER_ATTR_NEXT_HOP_ID;
                m_attr.value.oid = nhIt->second.next_hop_id;
                member_attrs.push_back(m_attr);

                if (dm.seqId > 0)
                {
                    m_attr.id = SAI_NEXT_HOP_GROUP_MEMBER_ATTR_SEQUENCE_ID;
                    m_attr.value.u32 = dm.seqId;
                    member_attrs.push_back(m_attr);
                }

                sai_object_id_t newMemberOid;
                sai_status_t st = sai_next_hop_group_api->create_next_hop_group_member(
                    &newMemberOid, gSwitchId,
                    (uint32_t)member_attrs.size(), member_attrs.data());
                if (st != SAI_STATUS_SUCCESS)
                {
                    SWSS_LOG_ERROR("ARS-BATCH: failed to add member %s to new "
                                   "NHG 0x%" PRIx64 " rc=%d",
                                   dm.nhKey.to_string().c_str(), newNhgOid, st);
                    memberFail = true;
                    break;
                }
                gCrmOrch->incCrmResUsedCounter(CrmResourceType::CRM_NEXTHOP_GROUP_MEMBER);

                NextHopGroupMemberEntry entry;
                entry.next_hop_id = newMemberOid;
                entry.seq_id = dm.seqId;
                newMembers[dm.nhKey] = entry;

                SWSS_LOG_NOTICE("ARS-BATCH: added member to new NHG: nh=%s "
                                "oid=0x%" PRIx64 " seq=%u",
                                dm.nhKey.to_string().c_str(), newMemberOid,
                                dm.seqId);
            }

            if (memberFail)
            {
                SWSS_LOG_ERROR("ARS-BATCH: member creation failed — rolling "
                               "back new NHG 0x%" PRIx64, newNhgOid);
                for (auto &mem : newMembers)
                {
                    sai_next_hop_group_api->remove_next_hop_group_member(
                        mem.second.next_hop_id);
                    gCrmOrch->decCrmResUsedCounter(
                        CrmResourceType::CRM_NEXTHOP_GROUP_MEMBER);
                }
                forgetNhg(newNhgOid);
                sai_next_hop_group_api->remove_next_hop_group(newNhgOid);
                gCrmOrch->decCrmResUsedCounter(CrmResourceType::CRM_NEXTHOP_GROUP);
                for (auto &dm : members)
                    m_deferredNhgMembers.push_back(dm);
                continue;
            }

            // Step 4: Repoint routes from old NHG to new NHG
            sai_attribute_t route_attr;
            route_attr.id = SAI_ROUTE_ENTRY_ATTR_NEXT_HOP_ID;
            route_attr.value.oid = newNhgOid;

            size_t routesRepointed = 0;
            const auto &syncdRoutes = gRouteOrch->getSyncdRoutes();
            for (auto &vrfRoutes : syncdRoutes)
            {
                for (auto &routeEntry : vrfRoutes.second)
                {
                    if (routeEntry.second.nhg_key != nhgKey)
                        continue;

                    sai_route_entry_t sai_route;
                    sai_route.switch_id = gSwitchId;
                    sai_route.vr_id = vrfRoutes.first;
                    copy(sai_route.destination, routeEntry.first);

                    sai_status_t st = sai_route_api->set_route_entry_attribute(
                        &sai_route, &route_attr);
                    if (st != SAI_STATUS_SUCCESS)
                    {
                        SWSS_LOG_ERROR("ARS-BATCH: failed to repoint route "
                                       "%s to new NHG rc=%d",
                                       routeEntry.first.to_string().c_str(), st);
                    }
                    else
                    {
                        routesRepointed++;
                    }
                }
            }

            SWSS_LOG_NOTICE("ARS-BATCH: repointed %zu routes to new NHG "
                            "0x%" PRIx64, routesRepointed, newNhgOid);

            // Step 5: Delete old (empty) NHG
            forgetNhg(nhgOid);
            sai_status_t rmSt = sai_next_hop_group_api->remove_next_hop_group(nhgOid);
            if (rmSt != SAI_STATUS_SUCCESS)
            {
                SWSS_LOG_WARN("ARS-BATCH: failed to remove old NHG 0x%" PRIx64
                              " rc=%d (non-fatal, it's empty)", nhgOid, rmSt);
            }
            else
            {
                gCrmOrch->decCrmResUsedCounter(CrmResourceType::CRM_NEXTHOP_GROUP);
            }

            // Step 6: Update syncdNhgs map — replace old entry with new
            auto &syncdNhgs2 = gRouteOrch->getSyncdNextHopGroups();
            NextHopGroupEntry newEntry;
            newEntry.next_hop_group_id = newNhgOid;
            newEntry.ref_count = nhgIt->second.ref_count;
            newEntry.nhopgroup_members = newMembers;
            newEntry.nh_member_install_count = (uint32_t)newMembers.size();
            syncdNhgs2[nhgKey] = newEntry;

            SWSS_LOG_NOTICE("ARS-BATCH: make-before-break COMPLETE for NHG %s: "
                            "old=0x%" PRIx64 " new=0x%" PRIx64 " members=%zu "
                            "routes=%zu",
                            nhgKey.to_string().c_str(), nhgOid, newNhgOid,
                            newMembers.size(), routesRepointed);
        }
        else
        {
            // NHG still has existing members but they're all AR — we can
            // try to bind ARS now (if the NHG doesn't already have it).
            auto arsOid = resolveArsForNhg(nhgOid, nhgKey);
            if (arsOid == SAI_NULL_OBJECT_ID)
            {
                for (auto &dm : members)
                {
                    auto ifIt = m_arsInterfaces.find(dm.nhKey.alias);
                    if (ifIt != m_arsInterfaces.end() &&
                        !ifIt->second.arsObject.empty())
                    {
                        auto objIt = m_arsObjects.find(ifIt->second.arsObject);
                        if (objIt != m_arsObjects.end() &&
                            objIt->second.arsOid != SAI_NULL_OBJECT_ID)
                        {
                            arsOid = objIt->second.arsOid;
                            break;
                        }
                    }
                }
            }
            if (arsOid != SAI_NULL_OBJECT_ID)
            {
                // Remove all existing members, bind ARS, re-add them + deferred
                SWSS_LOG_NOTICE("ARS-BATCH: NHG 0x%" PRIx64 " has %zu AR members "
                                "— removing them, binding ARS, then re-adding all",
                                nhgOid, nhgIt->second.nhopgroup_members.size());

                vector<DeferredNhgMember> existingAsDeferred;
                vector<NextHopKey> toRemove;
                for (auto &em : nhgIt->second.nhopgroup_members)
                    toRemove.push_back(em.first);

                for (auto &rmKey : toRemove)
                {
                    auto memberIt = nhgIt->second.nhopgroup_members.find(rmKey);
                    if (memberIt == nhgIt->second.nhopgroup_members.end())
                        continue;
                    sai_status_t st = sai_next_hop_group_api->remove_next_hop_group_member(
                        memberIt->second.next_hop_id);
                    if (st != SAI_STATUS_SUCCESS)
                    {
                        SWSS_LOG_ERROR("ARS-BATCH: failed to remove existing member "
                                       "%s from NHG 0x%" PRIx64 " rc=%d",
                                       rmKey.to_string().c_str(), nhgOid, st);
                        continue;
                    }
                    gCrmOrch->decCrmResUsedCounter(CrmResourceType::CRM_NEXTHOP_GROUP_MEMBER);
                    DeferredNhgMember edm;
                    edm.nhgKey = nhgKey;
                    edm.nhKey = rmKey;
                    edm.seqId = memberIt->second.seq_id;
                    existingAsDeferred.push_back(edm);
                    nhgIt->second.nhopgroup_members.erase(memberIt);
                    nhgIt->second.nh_member_install_count--;
                }

                if (bindArsToNhg(nhgOid, arsOid))
                {
                    SWSS_LOG_NOTICE("ARS-BATCH: bound ARS 0x%" PRIx64 " to NHG "
                                    "0x%" PRIx64 " after clearing members",
                                    arsOid, nhgOid);
                }

                // Merge existing (now removed) + originally deferred for re-add
                vector<DeferredNhgMember> allMembers;
                allMembers.insert(allMembers.end(), existingAsDeferred.begin(),
                                  existingAsDeferred.end());
                allMembers.insert(allMembers.end(), members.begin(), members.end());

                size_t added = 0, failed = 0;
                for (auto &dm : allMembers)
                {
                    auto &syncdNhs = gNeighOrch->getSyncdNextHops();
                    auto nhIt = syncdNhs.find(dm.nhKey);
                    if (nhIt == syncdNhs.end() ||
                        nhIt->second.next_hop_id == SAI_NULL_OBJECT_ID)
                    {
                        SWSS_LOG_WARN("ARS-BATCH: NH %s not found — skipping",
                                      dm.nhKey.to_string().c_str());
                        failed++;
                        continue;
                    }

                    vector<sai_attribute_t> member_attrs;
                    sai_attribute_t m_attr;

                    m_attr.id = SAI_NEXT_HOP_GROUP_MEMBER_ATTR_NEXT_HOP_GROUP_ID;
                    m_attr.value.oid = nhgOid;
                    member_attrs.push_back(m_attr);

                    m_attr.id = SAI_NEXT_HOP_GROUP_MEMBER_ATTR_NEXT_HOP_ID;
                    m_attr.value.oid = nhIt->second.next_hop_id;
                    member_attrs.push_back(m_attr);

                    if (dm.seqId > 0)
                    {
                        m_attr.id = SAI_NEXT_HOP_GROUP_MEMBER_ATTR_SEQUENCE_ID;
                        m_attr.value.u32 = dm.seqId;
                        member_attrs.push_back(m_attr);
                    }

                    sai_object_id_t newMemberOid;
                    sai_status_t st = sai_next_hop_group_api->create_next_hop_group_member(
                        &newMemberOid, gSwitchId,
                        (uint32_t)member_attrs.size(), member_attrs.data());
                    if (st != SAI_STATUS_SUCCESS)
                    {
                        SWSS_LOG_ERROR("ARS-BATCH: FAILED create NHG member: "
                                       "nhg_oid=0x%" PRIx64 " nh=%s seq=%u rc=%d",
                                       nhgOid, dm.nhKey.to_string().c_str(),
                                       dm.seqId, st);
                        failed++;
                        continue;
                    }

                    gCrmOrch->incCrmResUsedCounter(CrmResourceType::CRM_NEXTHOP_GROUP_MEMBER);

                    NextHopGroupMemberEntry memberEntry;
                    memberEntry.next_hop_id = newMemberOid;
                    memberEntry.seq_id = dm.seqId;
                    nhgIt->second.nhopgroup_members[dm.nhKey] = memberEntry;
                    nhgIt->second.nh_member_install_count++;
                    added++;

                    SWSS_LOG_NOTICE("ARS-BATCH: created NHG member: nhg_oid=0x%" PRIx64
                                    " member_oid=0x%" PRIx64 " nh=%s seq=%u",
                                    nhgOid, newMemberOid,
                                    dm.nhKey.to_string().c_str(), dm.seqId);
                }

                SWSS_LOG_NOTICE("ARS-BATCH: NHG %s — added %zu/%zu members (%zu failed)",
                                nhgKey.to_string().c_str(), added, allMembers.size(), failed);
                continue;
            }
            else
            {
                SWSS_LOG_ERROR("ARS-BATCH: cannot resolve ARS for non-empty NHG %s "
                               "— keeping %zu members deferred",
                               nhgKey.to_string().c_str(), members.size());
                for (auto &dm : members)
                    m_deferredNhgMembers.push_back(dm);
                continue;
            }
        }
    }

    SWSS_LOG_NOTICE("ARS-BATCH: deferred NHG member processing COMPLETE "
                    "(%zu still deferred)", m_deferredNhgMembers.size());
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

    // Mellanox SAI only implements quant-band threshold attributes on
    // ARS_PROFILE. Attributes like ALGO, PORT_LOAD_*, ENABLE_IPV4/6,
    // SAMPLING_INTERVAL are handled internally by the SDK and the
    // ars-classifier-daemon (via sx_api_ar_default_classification_set).
    //
    // Sending unsupported attrs works at runtime (SAI ignores them) but
    // FAILS during config-reload APPLY_VIEW: syncd validates each attribute
    // against capability metadata (CREATE_IMP=false) and rejects the entire
    // CREATE with SAI_STATUS_ATTR_NOT_IMPLEMENTED_0. Sending only quant-band
    // thresholds avoids this and works in both runtime and apply-view paths.
    //
    // When all three bands are zero, auto-fill with conservative defaults
    // so flowlet works out of the box. Non-zero quant-band values at CREATE
    // tell Mellanox SAI to call sx_api_ar_congestion_threshold_set (non-hardened
    // mode). Unit is bytes since SAI v2511.36.0.0 (was cells previously).
    uint32_t qb0 = entry.quantBand0MinThreshold;
    uint32_t qb1 = entry.quantBand1MinThreshold;
    uint32_t qb2 = entry.quantBand2MinThreshold;
    if (!(qb0 != 0 && qb1 != 0 && qb2 != 0 && qb0 < qb1 && qb1 < qb2))
    {
        // Mellanox SAI accepts large quant-band values at CREATE time but
        // rejects them during SET with SAI_STATUS_INVALID_PARAMETER. The
        // deferred-OID reuse path calls updateArsProfileAttr (SET), so
        // defaults must be within the SET-safe range (matching ucli defaults).
        // On Spectrum-4 (192-byte cells): 2560→14, 5120→27, 12800→67 cells.
        qb0 = 2560;
        qb1 = 5120;
        qb2 = 12800;
        SWSS_LOG_NOTICE("ARS: profile '%s' quant-band thresholds incomplete or "
                        "non-monotonic (%u/%u/%u) — using SET-safe defaults "
                        "(%u/%u/%u); final values will be applied via SET "
                        "once all three bands arrive from CONFIG_DB",
                        name.c_str(),
                        entry.quantBand0MinThreshold,
                        entry.quantBand1MinThreshold,
                        entry.quantBand2MinThreshold,
                        qb0, qb1, qb2);
    }

    vector<sai_attribute_t> attrs;
    sai_attribute_t attr;

    attr.id = SAI_ARS_PROFILE_ATTR_QUANT_BAND_0_MIN_THRESHOLD;
    attr.value.u32 = qb0;
    attrs.push_back(attr);

    attr.id = SAI_ARS_PROFILE_ATTR_QUANT_BAND_1_MIN_THRESHOLD;
    attr.value.u32 = qb1;
    attrs.push_back(attr);

    attr.id = SAI_ARS_PROFILE_ATTR_QUANT_BAND_2_MIN_THRESHOLD;
    attr.value.u32 = qb2;
    attrs.push_back(attr);

    sai_object_id_t profileOid;

    // A previous removeArsProfile may have deferred the SAI removal because
    // ports still had ARS enabled (RIF guard). The old profile OID is still
    // live in the ASIC — calling create_ars_profile would return
    // SAI_STATUS_ITEM_ALREADY_EXISTS and crash syncd. Reuse the leaked OID
    // and update its quant-band thresholds via SET instead.
    if (m_deferredProfileOid != SAI_NULL_OBJECT_ID)
    {
        profileOid = m_deferredProfileOid;
        m_deferredProfileOid = SAI_NULL_OBJECT_ID;

        SWSS_LOG_NOTICE("ARS: reusing deferred profile OID 0x%" PRIx64
                        " for '%s' (previous removal was skipped because "
                        "ports still had ARS enabled in ASIC)",
                        profileOid, name.c_str());

        updateArsProfileAttr(profileOid,
                             SAI_ARS_PROFILE_ATTR_QUANT_BAND_0_MIN_THRESHOLD, qb0);
        updateArsProfileAttr(profileOid,
                             SAI_ARS_PROFILE_ATTR_QUANT_BAND_1_MIN_THRESHOLD, qb1);
        updateArsProfileAttr(profileOid,
                             SAI_ARS_PROFILE_ATTR_QUANT_BAND_2_MIN_THRESHOLD, qb2);
    }
    else
    {
        sai_status_t status = sai_ars_profile_api->create_ars_profile(
            &profileOid, gSwitchId, (uint32_t)attrs.size(), attrs.data());

        if (status != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_ERROR("ARS: create_ars_profile failed for %s: %s",
                           name.c_str(), sai_serialize_status(status).c_str());
            return false;
        }

        SWSS_LOG_NOTICE("ARS: created profile %s OID 0x%" PRIx64,
                        name.c_str(), profileOid);
    }

    ArsProfileEntry stored = entry;
    stored.profileOid = profileOid;
    stored.quantBand0MinThreshold = qb0;
    stored.quantBand1MinThreshold = qb1;
    stored.quantBand2MinThreshold = qb2;
    m_arsProfiles[name] = stored;

    if (m_activeSwitchProfileOid == SAI_NULL_OBJECT_ID)
    {
        if (!bindArsProfileToSwitch(profileOid))
        {
            SWSS_LOG_ERROR("ARS: profile %s created (OID 0x%" PRIx64
                           ") but failed to bind to switch — data-plane "
                           "ARS will not be active until the profile is "
                           "re-bound (e.g. via `load-balance adaptive "
                           "bind-profile`)",
                           name.c_str(), profileOid);
        }
    }

    return true;
}

bool ArsOrch::removeArsProfile(const string &name)
{
    SWSS_LOG_ENTER();

    auto it = m_arsProfiles.find(name);
    if (it == m_arsProfiles.end())
        return true;

    // Mellanox SAI rejects profile unbind/removal when ports still have
    // SAI_PORT_ATTR_ARS_ENABLE=true ("ARS ports exist - remove N ports
    // before unbinding"). syncd treats any SAI failure as fatal in async
    // mode, crashing the switch. If ports are still ARS-enabled in ASIC
    // (because setPortArsEnable was blocked by the RIF guard), defer the
    // SAI removal and let config reload reconcile the ASIC state.
    if (!m_arsEnabledPorts.empty())
    {
        string portList;
        for (const auto &p : m_arsEnabledPorts)
        {
            if (!portList.empty()) portList += ", ";
            portList += p;
        }
        SWSS_LOG_NOTICE("ARS: deferring SAI removal of profile '%s' (OID "
                        "0x%" PRIx64 ") — %zu port(s) still have ARS enabled "
                        "in ASIC (%s). Removing from orchagent cache only; "
                        "the ASIC state will be reconciled on the next config "
                        "reload. The leaked OID will be reused if a new "
                        "profile is created before reload.",
                        name.c_str(), it->second.profileOid,
                        m_arsEnabledPorts.size(), portList.c_str());
        m_deferredProfileOid = it->second.profileOid;
        m_arsProfiles.erase(it);
        m_activeSwitchProfileOid = SAI_NULL_OBJECT_ID;
        return true;
    }

    sai_object_id_t oid = it->second.profileOid;

    if (oid == m_activeSwitchProfileOid)
    {
        // Before unbinding the profile, ensure all ARS objects are fully
        // removed. The forceUnbindArsFromNhg path now DELETES NHGs rather
        // than trying set_attribute(ARS=NULL), so ref_count drops to 0 and
        // remove_ars should succeed.
        SWSS_LOG_NOTICE("ARS: removeArsProfile(%s): unbinding ARS from NHGs "
                        "and removing ARS objects before profile unbind",
                        name.c_str());

        if (gRouteOrch)
            gRouteOrch->unbindArsFromAllNhgs();

        for (auto &objEntry : m_arsObjects)
        {
            if (objEntry.second.arsOid == SAI_NULL_OBJECT_ID)
                continue;
            if (objEntry.first == name)
                continue;

            SWSS_LOG_NOTICE("ARS: removeArsProfile(%s): force-removing ARS "
                            "object '%s' (oid=0x%" PRIx64 ")",
                            name.c_str(), objEntry.first.c_str(),
                            objEntry.second.arsOid);
            sai_status_t rmSt = sai_ars_api->remove_ars(objEntry.second.arsOid);
            if (rmSt == SAI_STATUS_SUCCESS)
            {
                objEntry.second.arsOid = SAI_NULL_OBJECT_ID;
            }
            else
            {
                SWSS_LOG_ERROR("ARS: removeArsProfile: failed to remove object "
                               "'%s' (rc=%d)", objEntry.first.c_str(), rmSt);
            }
        }

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

    // A previous removeArsObject may have deferred the SAI removal because
    // ports still had ARS enabled (RIF guard). The old ARS OID is still live
    // in the ASIC — calling create_ars would return ITEM_ALREADY_EXISTS and
    // crash syncd. Reuse the leaked OID and attempt to SET the requested
    // attributes. If SAI rejects the SET (OBJECT_IN_USE because leaked NHGs
    // still reference it), log a warning; the orchagent cache records the new
    // values and a config reload will reconcile the ASIC.
    if (m_deferredArsOid != SAI_NULL_OBJECT_ID)
    {
        arsOid = m_deferredArsOid;
        m_deferredArsOid = SAI_NULL_OBJECT_ID;

        SWSS_LOG_NOTICE("ARS: reusing deferred ARS OID 0x%" PRIx64
                        " for '%s' (previous removal was skipped because "
                        "ports still had ARS enabled in ASIC).",
                        arsOid, name.c_str());

        // Attempt to SET the requested attributes on the reused OID.
        // Mellanox SAI fatally rejects set_ars_attribute when NHGs still
        // reference the object (OBJECT_IN_USE). Only attempt if no
        // ARS-bound NHGs are tracked — this covers the common case where
        // topology cleanup already deleted all NHGs via forceUnbindArsFromNhg.
        if (m_nhgStateKeys.empty())
        {
            for (const auto &a : attrs)
            {
                sai_status_t setRc = sai_ars_api->set_ars_attribute(arsOid, &a);
                if (setRc != SAI_STATUS_SUCCESS)
                {
                    const char *attrName =
                        (a.id == SAI_ARS_ATTR_MODE) ? "MODE" :
                        (a.id == SAI_ARS_ATTR_IDLE_TIME) ? "IDLE_TIME" :
                        (a.id == SAI_ARS_ATTR_MAX_FLOWS) ? "MAX_FLOWS" : "UNKNOWN";
                    SWSS_LOG_WARN("ARS: deferred-OID reuse — SET %s on 0x%" PRIx64
                                  " failed (%s). ASIC retains previous value; "
                                  "config reload will reconcile.",
                                  attrName, arsOid,
                                  sai_serialize_status(setRc).c_str());
                }
            }
        }
        else
        {
            SWSS_LOG_WARN("ARS: deferred-OID reuse for '%s' — %zu ARS NHG "
                          "binding(s) still tracked, skipping attribute SET to "
                          "avoid fatal SAI error. ASIC retains previous "
                          "attributes; config reload will reconcile.",
                          name.c_str(), m_nhgStateKeys.size());
        }
    }
    else
    {
        sai_status_t status = sai_ars_api->create_ars(
            &arsOid, gSwitchId, (uint32_t)attrs.size(), attrs.data());

        if (status != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_ERROR("ARS: create_ars failed for %s: %s",
                           name.c_str(), sai_serialize_status(status).c_str());
            return false;
        }
    }

    SWSS_LOG_NOTICE("ARS: created object %s OID 0x%" PRIx64 " mode %d",
                    name.c_str(), arsOid, (int)entry.mode);

    ArsObjectEntry stored = entry;
    stored.arsOid = arsOid;
    stored.saiSetBlocked = true;
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

    // Mellanox SAI crashes (SDK health-check FATAL) if we remove an ARS
    // object while ports still have SAI_PORT_ATTR_ARS_ENABLE=true in the
    // ASIC — the hardware ends up with dangling ARS references. Check
    // whether any ports associated with this object are still ARS-enabled
    // in hardware (i.e. still in m_arsEnabledPorts because setPortArsEnable
    // was blocked by the RIF guard). If so, skip the SAI removal and let
    // config reload reconcile the ASIC state from a clean CONFIG_DB.
    vector<string> stuckPorts;
    for (const auto &kv : m_arsInterfaces)
    {
        if (kv.second.arsObject == name &&
            m_arsEnabledPorts.count(kv.first))
        {
            stuckPorts.push_back(kv.first);
        }
    }
    if (!stuckPorts.empty())
    {
        string portList;
        for (const auto &p : stuckPorts)
        {
            if (!portList.empty()) portList += ", ";
            portList += p;
        }
        SWSS_LOG_NOTICE("ARS: deferring SAI removal of object '%s' (OID "
                        "0x%" PRIx64 ") — %zu port(s) still have ARS enabled "
                        "in ASIC (%s). Removing from orchagent cache only; "
                        "the ASIC state will be reconciled on the next config "
                        "reload. The leaked OID will be reused if a new "
                        "object is created before reload.",
                        name.c_str(), oid, stuckPorts.size(),
                        portList.c_str());
        m_deferredArsOid = oid;
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
    // rebind. On Mellanox, forceUnbindArsFromNhg now DELETES ARS-bound NHGs
    // entirely (since set_attribute(ARS=NULL) is unsupported), which releases
    // the ARS object ref_count.
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
        if (status == SAI_STATUS_OBJECT_IN_USE)
        {
            // A stale NHG (e.g. from ordered-ECMP reuse) still references
            // this ARS object and could not be cleaned up. Do NOT attempt
            // to restore NHG/LAG bindings — that triggers more SAI errors
            // on the same stuck NHG, flooding syncd with failures that
            // ultimately cause switch_shutdown_request.
            //
            // Keep the real arsOid so downstream code (bindArsProfileToSwitch,
            // removeArsProfile, doArsGlobalTask) can detect the leaked object
            // and refuse to unbind the profile — unbinding while a leaked ARS
            // object exists triggers an SDK shutdown. On the next ARS
            // re-enable cycle, enableArsDataPlane() will re-enable this entry
            // and RIF migration will recreate NHGs from scratch, eventually
            // freeing the stale reference.
            SWSS_LOG_ERROR("ARS: remove_ars failed for %s: %s — "
                           "stale SAI reference prevents removal. "
                           "Leaving object disabled (will be cleaned "
                           "up on next ARS re-enable cycle).",
                           name.c_str(), sai_serialize_status(status).c_str());
            it->second.enabled = false;
            return false;
        }

        SWSS_LOG_ERROR("ARS: remove_ars failed for %s: %s — restoring "
                       "NHG and LAG bindings so the failure is side-effect free",
                       name.c_str(), sai_serialize_status(status).c_str());

        it->second.enabled = true;

        if (gRouteOrch)
            gRouteOrch->rebindArsForAllNhgs();

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

    // Refuse to unbind the profile while leaked ARS objects exist in SAI.
    // Unbinding triggers the Mellanox SDK to clean up internal ARS state;
    // if a leaked NHG still holds a ref_count on an ARS object, the SDK
    // detects the dangling reference and sends switch_shutdown_request,
    // killing orchagent. Keeping the profile bound is harmless — it just
    // configures EWMA parameters. The leaked objects will be cleaned up
    // on the next ARS re-enable cycle.
    if (profileOid == SAI_NULL_OBJECT_ID)
    {
        for (const auto &kv : m_arsObjects)
        {
            if (kv.second.arsOid != SAI_NULL_OBJECT_ID)
            {
                SWSS_LOG_WARN("ARS: skipping profile unbind — ARS object "
                              "'%s' (OID 0x%" PRIx64 ") still exists in SAI. "
                              "Unbinding would trigger SDK shutdown. Profile "
                              "will remain bound until next re-enable cycle.",
                              kv.first.c_str(), kv.second.arsOid);
                return false;
            }
        }
    }

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

    // Pre-check: Mellanox SAI rejects SAI_NEXT_HOP_GROUP_ATTR_ARS_OBJECT_ID
    // when the NHG already has members (mlnx_sai_nexthopgroup.c: "Cannot
    // modify SAI_NEXT_HOP_GROUP_ATTR_ARS_OBJECT_ID when NHG is not empty").
    // syncd treats the resulting SAI_STATUS_INVALID_PARAMETER as a fatal
    // runtime error and sends switch_shutdown_request, killing orchagent
    // before the return value can be handled. Guard here so the SAI call
    // never reaches syncd in the known-failure case.
    if (gRouteOrch)
    {
        auto &nhgTable = gRouteOrch->getSyncdNextHopGroups();
        bool foundInMap = false;
        for (auto &entry : nhgTable)
        {
            if (entry.second.next_hop_group_id == nhgOid)
            {
                foundInMap = true;

                // For unbind (arsOid == NULL): ALWAYS delete the NHG.
                // On Mellanox, set_next_hop_group_attribute(ARS=NULL) crashes
                // regardless of whether the NHG is empty or has members.
                // The ARS binding is truly write-once.
                if (arsOid == SAI_NULL_OBJECT_ID)
                {
                    return gRouteOrch->forceUnbindArsFromNhg(nhgOid);
                }

                // For bind (arsOid != NULL) with non-empty NHG: cannot
                // proceed — caller must use make-before-break NHG recreation.
                if (!entry.second.nhopgroup_members.empty())
                {
                    const string key = sai_serialize_object_id(nhgOid);
                    string reason = "NHG has " +
                        to_string(entry.second.nhopgroup_members.size()) +
                        " members (Mellanox write-once restriction). "
                        "Caller should use make-before-break NHG recreation.";
                    SWSS_LOG_NOTICE("ARS: bind ARS to NHG 0x%" PRIx64
                                   " (target ARS 0x%" PRIx64 ") skipped — %s",
                                   nhgOid, arsOid, reason.c_str());
                    m_nhgStateKeys[nhgOid] = key;
                    writeArsNhgState(key, true, reason);
                    return false;
                }
                break;
            }
        }

        // NHG not found in orchagent map — it was already deleted
        // (e.g. by forceUnbindArsFromNhg). Nothing to do.
        if (!foundInMap)
        {
            SWSS_LOG_NOTICE("ARS: bindArsToNhg(0x%" PRIx64 ") — NHG not found "
                            "in orchagent map (already deleted). Skipping.",
                            nhgOid);
            return true;
        }
    }

    sai_attribute_t attr;
    attr.id = SAI_NEXT_HOP_GROUP_ATTR_ARS_OBJECT_ID;
    attr.value.oid = arsOid;

    sai_status_t status = sai_next_hop_group_api->set_next_hop_group_attribute(nhgOid, &attr);
    if (status != SAI_STATUS_SUCCESS)
    {
        const string key = sai_serialize_object_id(nhgOid);
        const char *direction = (arsOid == SAI_NULL_OBJECT_ID) ? "unbind ARS from"
                                                               : "bind ARS to";
        string reason;
        if (status == SAI_STATUS_INVALID_PARAMETER)
        {
            reason = "SAI set-attribute rejected (INVALID_PARAMETER)";
            SWSS_LOG_ERROR("ARS: %s NHG 0x%" PRIx64 " (target ARS 0x%" PRIx64
                           ") failed (INVALID_PARAMETER)",
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
        m_nhgStateKeys[nhgOid] = key;
        if (arsOid == SAI_NULL_OBJECT_ID)
        {
            writeArsNhgState(key, true,
                             string("unbind rejected by SAI — ASIC retains previous "
                                    "ARS binding. ") + reason);
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

    // Pre-check: Mellanox SAI rejects SAI_PORT_ATTR_ARS_ENABLE on ports with
    // RIFs (mlnx_sai_port.c "Can't modify SAI_PORT_ATTR_ARS_ENABLE on port
    // with created RIFs"). Critically, syncd treats this SAI_STATUS_INVALID_-
    // PARAMETER as a fatal runtime error and sends switch_shutdown_request,
    // killing orchagent before we can handle the return value. Guard here so
    // the SAI call never reaches syncd in the known-failure case.
    if (port.m_rif_id != 0)
    {
        if (enable)
        {
            SWSS_LOG_NOTICE("ARS: setPortArsEnable(%s, true) — port has "
                            "RIF 0x%" PRIx64 " vrf=0x%" PRIx64
                            " port_oid=0x%" PRIx64 ". Initiating orchestrated "
                            "RIF migration to enable ARS dynamically.",
                            portName.c_str(), port.m_rif_id,
                            port.m_vr_id, port.m_port_id);
            return migratePortToArs(portName);
        }
        else
        {
            SWSS_LOG_ERROR("ARS: setPortArsEnable(%s, false) — port has "
                           "RIF 0x%" PRIx64 ". Cannot disable ARS while "
                           "RIF exists. Remove IPs first, then disable ARS.",
                           portName.c_str(), port.m_rif_id);
            return false;
        }
    }

    sai_attribute_t attr;
    attr.id = SAI_PORT_ATTR_ARS_ENABLE;
    attr.value.booldata = enable;

    sai_status_t status = sai_port_api->set_port_attribute(port.m_port_id, &attr);

    if (status != SAI_STATUS_SUCCESS)
    {
        if (status == SAI_STATUS_NOT_SUPPORTED)
        {
            SWSS_LOG_ERROR("ARS: setPortArsEnable(%s, %s) returned NOT_SUPPORTED — "
                           "check that ARS profile is created and SAI ARS is initialized. "
                           "port_oid=0x%" PRIx64 " rif=0x%" PRIx64,
                           portName.c_str(), enable ? "true" : "false",
                           port.m_port_id, port.m_rif_id);
        }
        else
        {
            SWSS_LOG_ERROR("ARS: setPortArsEnable(%s, %s) failed: SAI rc=%s "
                           "port_oid=0x%" PRIx64 " rif=0x%" PRIx64,
                           portName.c_str(), enable ? "true" : "false",
                           sai_serialize_status(status).c_str(),
                           port.m_port_id, port.m_rif_id);
        }
        return false;
    }

    SWSS_LOG_NOTICE("ARS: port %s ARS %s", portName.c_str(), enable ? "enabled" : "disabled");
    return true;
}

/* ── Orchestrated RIF migration for dynamic ARS enablement ──────────────
 *
 * Mellanox SAI rejects SAI_PORT_ATTR_ARS_ENABLE when the port has RIFs because
 * the SDK RIF type (SX_L2_INTERFACE_TYPE_ADAPTIVE_ROUTING vs PORT_VLAN) is
 * immutable after creation. This method performs a coordinated teardown and
 * rebuild of all objects that depend on the RIF, enabling ARS on the bare port
 * between removal and recreation.
 *
 * Sequence:
 *   1. Collect all neighbors/next-hops on this port
 *   2. For each NH, record which NHGs reference it (and their routes)
 *   3. Remove NHG members that reference NHs on this port
 *   4. Remove next-hop SAI objects (decrements RIF ref_count)
 *   5. Remove neighbor SAI entries (decrements RIF ref_count)
 *   6. Remove the RIF (ref_count now 0)
 *   7. Enable ARS on the bare port
 *   8. Recreate the RIF (SAI picks AR type since port->ars_enable is true)
 *   9. Recreate neighbor entries with new RIF
 *  10. Recreate next-hop objects with new RIF OID
 *  11. Recreate NHG members (reusing same NHG OIDs where possible)
 *  12. Trigger make-before-break NHG recreation for ARS binding
 *
 * Traffic impact: brief per-prefix blackhole during NHG member drain/refill
 * (sub-second with bulk ops). Significantly less disruptive than config reload.
 */
bool ArsOrch::migratePortToArs(const string &portName)
{
    SWSS_LOG_ENTER();
    SWSS_LOG_NOTICE("ARS-MIGRATE[%s]: ===== BEGIN orchestrated RIF migration =====",
                    portName.c_str());

    Port port;
    if (!m_portsOrch->getPort(portName, port))
    {
        SWSS_LOG_ERROR("ARS-MIGRATE[%s]: ABORT - port not found in PortsOrch", portName.c_str());
        return false;
    }

    SWSS_LOG_NOTICE("ARS-MIGRATE[%s]: port_oid=0x%" PRIx64 " rif_oid=0x%" PRIx64
                    " vrf_oid=0x%" PRIx64 " mtu=%u oper_status=%d",
                    portName.c_str(), port.m_port_id, port.m_rif_id,
                    port.m_vr_id, port.m_mtu, port.m_oper_status);

    if (port.m_rif_id == 0)
    {
        SWSS_LOG_NOTICE("ARS-MIGRATE[%s]: no RIF on port, falling through to direct enable",
                        portName.c_str());
        return setPortArsEnable(portName, true);
    }

    sai_object_id_t oldRifId = port.m_rif_id;
    sai_object_id_t vrfId = port.m_vr_id;

    // ─── Phase 1: Collect dependent objects ───────────────────────────────

    struct SavedNeighbor {
        NeighborEntry entry;
        MacAddress mac;
    };
    struct SavedNextHop {
        NextHopKey key;
        sai_object_id_t oldNhId;
    };

    vector<SavedNeighbor> savedNeighbors;
    vector<SavedNextHop> savedNextHops;

    if (gNeighOrch)
    {
        for (auto &nhEntry : gNeighOrch->getSyncdNextHops())
        {
            if (nhEntry.first.alias == portName)
            {
                SavedNextHop snh;
                snh.key = nhEntry.first;
                snh.oldNhId = nhEntry.second.next_hop_id;
                savedNextHops.push_back(snh);
                SWSS_LOG_INFO("ARS-MIGRATE[%s]: Phase1 collected NH: %s "
                              "oid=0x%" PRIx64 " ref_count=%d",
                              portName.c_str(),
                              nhEntry.first.to_string().c_str(),
                              nhEntry.second.next_hop_id,
                              nhEntry.second.ref_count);
            }
        }
        for (auto &neighEntry : gNeighOrch->getSyncdNeighbors())
        {
            if (neighEntry.first.alias == portName && neighEntry.second.hw_configured)
            {
                SavedNeighbor sn;
                sn.entry = neighEntry.first;
                sn.mac = neighEntry.second.mac;
                savedNeighbors.push_back(sn);
                SWSS_LOG_INFO("ARS-MIGRATE[%s]: Phase1 collected neighbor: %s mac=%s",
                              portName.c_str(),
                              neighEntry.first.ip_address.to_string().c_str(),
                              neighEntry.second.mac.to_string().c_str());
            }
        }
    }

    // Also collect IP prefixes configured on this interface (for IP-to-me routes)
    vector<IpPrefix> savedIpPrefixes;
    if (gIntfsOrch)
    {
        auto &syncdIntfses = gIntfsOrch->getSyncdIntfses();
        auto intfIt = syncdIntfses.find(portName);
        if (intfIt != syncdIntfses.end())
        {
            for (auto &prefix : intfIt->second.ip_addresses)
            {
                savedIpPrefixes.push_back(prefix);
                SWSS_LOG_INFO("ARS-MIGRATE[%s]: Phase1 collected IP prefix: %s",
                              portName.c_str(), prefix.to_string().c_str());
            }
        }
    }

    SWSS_LOG_NOTICE("ARS-MIGRATE[%s]: Phase1 COMPLETE - collected %zu neighbors, "
                    "%zu next-hops, %zu IP prefixes",
                    portName.c_str(), savedNeighbors.size(), savedNextHops.size(),
                    savedIpPrefixes.size());

    // Forward-declare variables used across Phases 2-5 so that goto
    // to restore_rif_without_ars does not cross their initialization.
    size_t nhRemoveSuccess = 0, nhRemoveFail = 0;
    size_t neighRemoveSuccess = 0;
    size_t ip2meRemoved = 0;
    size_t routeDrainFail = 0;
    size_t routesScanned = 0;
    sai_status_t st;

    struct SavedSingleNhRoute {
        sai_object_id_t vrfId;
        IpPrefix prefix;
        NextHopKey nhKey;
    };
    vector<SavedSingleNhRoute> savedSingleNhRoutes;

    // ─── Phase 2: Tear down NHG members referencing this port's NHs ──────

    struct NhgMemberInfo {
        NextHopGroupKey nhgKey;
        NextHopKey nhKey;
        sai_object_id_t memberOid;
        uint32_t seqId;
    };
    vector<NhgMemberInfo> removedMembers;

    // Track default-route-swap members removed in Phase 2 so rollback can
    // restore them (CodeRabbit: these were previously lost on failure).
    struct DfltSwapMemberInfo {
        NextHopGroupKey nhgKey;
        NextHopKey nhKey;
    };
    vector<DfltSwapMemberInfo> removedDfltSwapMembers;

    if (gRouteOrch)
    {
        for (auto &nhgEntry : gRouteOrch->getSyncdNextHopGroups())
        {
            auto &nhgKey = nhgEntry.first;
            auto &nhgData = nhgEntry.second;
            if (nhgData.next_hop_group_id == SAI_NULL_OBJECT_ID)
                continue;

            for (auto &savedNh : savedNextHops)
            {
                auto memberIt = nhgData.nhopgroup_members.find(savedNh.key);
                if (memberIt != nhgData.nhopgroup_members.end())
                {
                    NhgMemberInfo info;
                    info.nhgKey = nhgKey;
                    info.nhKey = savedNh.key;
                    info.memberOid = memberIt->second.next_hop_id;
                    info.seqId = memberIt->second.seq_id;
                    removedMembers.push_back(info);

                    SWSS_LOG_NOTICE("ARS-MIGRATE[%s]: Phase2 removing NHG member: "
                                   "nhg_oid=0x%" PRIx64 " member_oid=0x%" PRIx64
                                   " nh=%s seq=%u nhg_key=%s",
                                  portName.c_str(),
                                  nhgData.next_hop_group_id,
                                  memberIt->second.next_hop_id,
                                  savedNh.key.to_string().c_str(),
                                  memberIt->second.seq_id,
                                  nhgKey.to_string().c_str());

                    st = sai_next_hop_group_api->remove_next_hop_group_member(
                        memberIt->second.next_hop_id);
                    if (st != SAI_STATUS_SUCCESS)
                    {
                        SWSS_LOG_ERROR("ARS-MIGRATE[%s]: Phase2 FAILED - "
                                       "remove_next_hop_group_member(0x%" PRIx64 ") "
                                       "from nhg_oid=0x%" PRIx64 " returned SAI rc=%d. "
                                       "NHG key=%s, NH=%s. Aborting migration — "
                                       "jumping to emergency rollback.",
                                       portName.c_str(),
                                       memberIt->second.next_hop_id,
                                       nhgData.next_hop_group_id, st,
                                       nhgKey.to_string().c_str(),
                                       savedNh.key.to_string().c_str());
                        goto restore_rif_without_ars;
                    }
                    gCrmOrch->decCrmResUsedCounter(CrmResourceType::CRM_NEXTHOP_GROUP_MEMBER);
                    nhgData.nhopgroup_members.erase(memberIt);
                    if (nhgData.nh_member_install_count > 0)
                        nhgData.nh_member_install_count--;
                }

                // Also remove from default_route_nhopgroup_members — the
                // "Default Route NH Swap" mechanism can place our NHs into
                // other NHGs (those with all-down members), creating extra SAI
                // references that block Phase 3's remove_next_hop.
                auto dfltIt = nhgData.default_route_nhopgroup_members.find(savedNh.key);
                if (dfltIt != nhgData.default_route_nhopgroup_members.end())
                {
                    SWSS_LOG_NOTICE("ARS-MIGRATE[%s]: Phase2 removing default-route-swap "
                                   "NHG member: nhg_oid=0x%" PRIx64 " member_oid=0x%" PRIx64
                                   " nh=%s nhg_key=%s",
                                  portName.c_str(),
                                  nhgData.next_hop_group_id,
                                  dfltIt->second.next_hop_id,
                                  savedNh.key.to_string().c_str(),
                                  nhgKey.to_string().c_str());

                    st = sai_next_hop_group_api->remove_next_hop_group_member(
                        dfltIt->second.next_hop_id);
                    if (st != SAI_STATUS_SUCCESS)
                    {
                        SWSS_LOG_WARN("ARS-MIGRATE[%s]: Phase2 failed to remove "
                                      "default-route-swap member 0x%" PRIx64
                                      " from nhg 0x%" PRIx64 " rc=%d (non-fatal, "
                                      "Phase3 may still fail)",
                                      portName.c_str(), dfltIt->second.next_hop_id,
                                      nhgData.next_hop_group_id, st);
                    }
                    else
                    {
                        gCrmOrch->decCrmResUsedCounter(CrmResourceType::CRM_NEXTHOP_GROUP_MEMBER);
                        gNeighOrch->decreaseNextHopRefCount(savedNh.key);
                        nhgData.default_route_nhopgroup_members.erase(dfltIt);
                        removedDfltSwapMembers.push_back({nhgKey, savedNh.key});
                    }
                }
            }
        }
    }

    {
        set<string> uniqueNhgs;
        for (auto &m : removedMembers) uniqueNhgs.insert(m.nhgKey.to_string());
        SWSS_LOG_NOTICE("ARS-MIGRATE[%s]: Phase2 COMPLETE - removed %zu NHG members "
                        "across %zu unique NHGs",
                        portName.c_str(), removedMembers.size(), uniqueNhgs.size());
    }

    // ─── Phase 2.5: Drain single-NH routes referencing this port's NHs ──
    //
    // Routes with a single next-hop are programmed in SAI with
    // NEXT_HOP_ID = nh_oid (no NHG involved).  Phase 2 only drains NHG
    // members, so these direct route→NH references survive and cause
    // Phase 3's remove_next_hop() to fail with SAI_STATUS_OBJECT_IN_USE.
    // Re-point them to DROP now; Phase 9.5 restores them.

    // Block scope for savedNhKeySet so that earlier gotos to
    // restore_rif_without_ars do not cross its non-trivial initialization.
    {
        // Build a set for O(1) NH key lookups instead of O(K) inner loop.
        // Note: the outer loops iterate getSyncdRoutes() by reference.
        // decreaseNextHopRefCount() below is a simple counter decrement that
        // does not mutate the route map, so the iterators remain valid.
        std::unordered_set<NextHopKey, boost::hash<NextHopKey>> savedNhKeySet;
        for (auto &snh : savedNextHops)
            savedNhKeySet.insert(snh.key);

        if (gRouteOrch)
        {
            for (auto &vrfRoutes : gRouteOrch->getSyncdRoutes())
            {
                for (auto &routeEntry : vrfRoutes.second)
                {
                    const auto &nhgKey = routeEntry.second.nhg_key;
                    if (nhgKey.getSize() != 1)
                        continue;

                    const NextHopKey &routeNh = *nhgKey.getNextHops().begin();
                    if (routeNh.isIntfNextHop())
                        continue;

                    if (savedNhKeySet.find(routeNh) == savedNhKeySet.end())
                        continue;

                    routesScanned++;

                    sai_route_entry_t route_entry;
                    route_entry.switch_id = gSwitchId;
                    route_entry.vr_id = vrfRoutes.first;
                    copy(route_entry.destination, routeEntry.first);

                    sai_attribute_t drop_attr;
                    drop_attr.id = SAI_ROUTE_ENTRY_ATTR_PACKET_ACTION;
                    drop_attr.value.s32 = SAI_PACKET_ACTION_DROP;

                    sai_status_t rs = sai_route_api->set_route_entry_attribute(
                        &route_entry, &drop_attr);
                    if (rs != SAI_STATUS_SUCCESS)
                    {
                        SWSS_LOG_ERROR("ARS-MIGRATE[%s]: Phase2.5 FAILED to set "
                                       "DROP on route %s vrf=0x%" PRIx64 " rc=%d",
                                       portName.c_str(),
                                       routeEntry.first.to_string().c_str(),
                                       vrfRoutes.first, rs);
                        routeDrainFail++;
                        continue;
                    }

                    sai_attribute_t nh_attr;
                    nh_attr.id = SAI_ROUTE_ENTRY_ATTR_NEXT_HOP_ID;
                    nh_attr.value.oid = SAI_NULL_OBJECT_ID;
                    rs = sai_route_api->set_route_entry_attribute(
                        &route_entry, &nh_attr);
                    if (rs != SAI_STATUS_SUCCESS)
                    {
                        SWSS_LOG_ERROR("ARS-MIGRATE[%s]: Phase2.5 FAILED to "
                                       "null NEXT_HOP_ID on route %s rc=%d "
                                       "— restoring FORWARD",
                                       portName.c_str(),
                                       routeEntry.first.to_string().c_str(),
                                       rs);
                        sai_attribute_t fwd_attr;
                        fwd_attr.id = SAI_ROUTE_ENTRY_ATTR_PACKET_ACTION;
                        fwd_attr.value.s32 = SAI_PACKET_ACTION_FORWARD;
                        sai_route_api->set_route_entry_attribute(
                            &route_entry, &fwd_attr);
                        routeDrainFail++;
                        continue;
                    }

                    savedSingleNhRoutes.push_back(
                        {vrfRoutes.first, routeEntry.first, routeNh});
                    gNeighOrch->decreaseNextHopRefCount(routeNh);

                    SWSS_LOG_NOTICE("ARS-MIGRATE[%s]: Phase2.5 drained single-NH "
                                   "route: %s nh=%s vrf=0x%" PRIx64,
                                   portName.c_str(),
                                   routeEntry.first.to_string().c_str(),
                                   routeNh.to_string().c_str(),
                                   vrfRoutes.first);
                }
            }
        }
    } // savedNhKeySet destroyed here

    SWSS_LOG_NOTICE("ARS-MIGRATE[%s]: Phase2.5 COMPLETE - drained %zu single-NH "
                    "routes, %zu failed, %zu matched (scanned all VRFs)",
                    portName.c_str(), savedSingleNhRoutes.size(),
                    routeDrainFail, routesScanned);

    if (routeDrainFail > 0)
    {
        SWSS_LOG_ERROR("ARS-MIGRATE[%s]: Phase2.5 had %zu drain failures — "
                       "aborting before Phase 3 (NH removal would fail with "
                       "OBJECT_IN_USE for undrained routes)",
                       portName.c_str(), routeDrainFail);
        goto restore_rif_without_ars;
    }

    // ─── Phase 3: Remove next-hop SAI objects ─────────────────────────────

    nhRemoveSuccess = 0;
    nhRemoveFail = 0;
    for (auto &snh : savedNextHops)
    {
        auto &syncdNhs = gNeighOrch->getSyncdNextHops();
        auto nhIt = syncdNhs.find(snh.key);
        if (nhIt == syncdNhs.end())
        {
            SWSS_LOG_WARN("ARS-MIGRATE[%s]: Phase3 NH %s not found in NeighOrch "
                          "syncdNextHops — skipping (stale entry?)",
                          portName.c_str(), snh.key.to_string().c_str());
            continue;
        }

        int savedRefCount = nhIt->second.ref_count;
        SWSS_LOG_NOTICE("ARS-MIGRATE[%s]: Phase3 removing NH: %s oid=0x%" PRIx64
                       " ref_count=%d (preserving ref_count across migration)",
                      portName.c_str(), snh.key.to_string().c_str(),
                      snh.oldNhId, savedRefCount);

        st = sai_next_hop_api->remove_next_hop(snh.oldNhId);
        if (st == SAI_STATUS_OBJECT_IN_USE)
        {
            // SAI may need a brief moment to process the NHG member removals
            // from Phase 2 (especially on Mellanox where SDK operations can be
            // asynchronous internally). Retry once after a short delay.
            SWSS_LOG_WARN("ARS-MIGRATE[%s]: Phase3 remove_next_hop(0x%" PRIx64
                          ") returned OBJECT_IN_USE — retrying once after 50ms",
                          portName.c_str(), snh.oldNhId);
            usleep(50000);
            st = sai_next_hop_api->remove_next_hop(snh.oldNhId);
        }
        if (st != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_ERROR("ARS-MIGRATE[%s]: Phase3 FAILED - remove_next_hop"
                           "(0x%" PRIx64 ") for NH %s returned SAI rc=%d. "
                           "ref_count was %d. Jumping to emergency rollback.",
                           portName.c_str(), snh.oldNhId,
                           snh.key.to_string().c_str(), st, savedRefCount);
            nhRemoveFail++;
            goto restore_rif_without_ars;
        }

        if (snh.key.ip_address.isV4())
            gCrmOrch->decCrmResUsedCounter(CrmResourceType::CRM_IPV4_NEXTHOP);
        else
            gCrmOrch->decCrmResUsedCounter(CrmResourceType::CRM_IPV6_NEXTHOP);

        gIntfsOrch->decreaseRouterIntfsRefCount(portName);
        nhIt->second.next_hop_id = SAI_NULL_OBJECT_ID;
        nhIt->second.ref_count = savedRefCount;
        nhRemoveSuccess++;
    }

    SWSS_LOG_NOTICE("ARS-MIGRATE[%s]: Phase3 COMPLETE - removed %zu/%zu next-hops",
                    portName.c_str(), nhRemoveSuccess, savedNextHops.size());

    // ─── Phase 4: Remove neighbor SAI entries ─────────────────────────────

    neighRemoveSuccess = 0;
    for (auto &sn : savedNeighbors)
    {
        sai_neighbor_entry_t neighbor_entry;
        neighbor_entry.switch_id = gSwitchId;
        neighbor_entry.rif_id = oldRifId;
        copy(neighbor_entry.ip_address, sn.entry.ip_address);

        SWSS_LOG_NOTICE("ARS-MIGRATE[%s]: Phase4 removing neighbor: ip=%s mac=%s "
                       "rif=0x%" PRIx64,
                      portName.c_str(),
                      sn.entry.ip_address.to_string().c_str(),
                      sn.mac.to_string().c_str(), oldRifId);

        st = sai_neighbor_api->remove_neighbor_entry(&neighbor_entry);
        if (st != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_ERROR("ARS-MIGRATE[%s]: Phase4 FAILED - remove_neighbor_entry"
                           "(ip=%s, rif=0x%" PRIx64 ") returned SAI rc=%d. "
                           "Jumping to emergency rollback.",
                           portName.c_str(),
                           sn.entry.ip_address.to_string().c_str(),
                           oldRifId, st);
            goto restore_rif_without_ars;
        }

        if (sn.entry.ip_address.isV4())
            gCrmOrch->decCrmResUsedCounter(CrmResourceType::CRM_IPV4_NEIGHBOR);
        else
            gCrmOrch->decCrmResUsedCounter(CrmResourceType::CRM_IPV6_NEIGHBOR);

        gIntfsOrch->decreaseRouterIntfsRefCount(portName);
        neighRemoveSuccess++;
    }

    SWSS_LOG_NOTICE("ARS-MIGRATE[%s]: Phase4 COMPLETE - removed %zu/%zu neighbors",
                    portName.c_str(), neighRemoveSuccess, savedNeighbors.size());

    // ─── Phase 4.5: Remove routes that hold RIF references ──────────────
    //
    // Mellanox SDK internally links routes to the RIF whose subnet
    // contains the route's destination.  remove_router_interface will
    // return SAI_STATUS_OBJECT_IN_USE (-17) if these routes still exist.
    // We must remove BOTH:
    //   - IP-to-me host routes  (e.g. 10.0.1.1/32) created by IntfOrch
    //   - Connected subnet routes (e.g. 10.0.1.0/31) created by RouteOrch

    ip2meRemoved = 0;
    for (auto &prefix : savedIpPrefixes)
    {
        // Remove IP-to-me host route (/32)
        SWSS_LOG_NOTICE("ARS-MIGRATE[%s]: Phase4.5 removing IP-to-me route: "
                        "ip=%s vrf=0x%" PRIx64,
                        portName.c_str(), prefix.getIp().to_string().c_str(),
                        vrfId);
        try
        {
            gIntfsOrch->removeIp2MeRoute(vrfId, prefix);
            ip2meRemoved++;
            SWSS_LOG_NOTICE("ARS-MIGRATE[%s]: Phase4.5 removed IP-to-me route: "
                            "%s/32 rc=OK",
                            portName.c_str(),
                            prefix.getIp().to_string().c_str());
        }
        catch (const exception &e)
        {
            SWSS_LOG_ERROR("ARS-MIGRATE[%s]: Phase4.5 FAILED to remove IP-to-me "
                           "route %s: %s. Jumping to emergency rollback.",
                           portName.c_str(), prefix.to_string().c_str(), e.what());
            goto restore_rif_without_ars;
        }

        // Remove connected subnet route (next_hop_id points to the RIF)
        IpPrefix subnet = prefix.getSubnet();
        sai_route_entry_t subnet_route;
        subnet_route.switch_id = gSwitchId;
        subnet_route.vr_id = vrfId;
        copy(subnet_route.destination, subnet);

        SWSS_LOG_NOTICE("ARS-MIGRATE[%s]: Phase4.5 removing subnet route: "
                        "%s vrf=0x%" PRIx64 " (next_hop was RIF 0x%" PRIx64 ")",
                        portName.c_str(), subnet.to_string().c_str(),
                        vrfId, oldRifId);

        sai_status_t rs = sai_route_api->remove_route_entry(&subnet_route);
        if (rs == SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_NOTICE("ARS-MIGRATE[%s]: Phase4.5 removed subnet route: "
                            "%s rc=OK",
                            portName.c_str(), subnet.to_string().c_str());
        }
        else if (rs == SAI_STATUS_ITEM_NOT_FOUND)
        {
            SWSS_LOG_WARN("ARS-MIGRATE[%s]: Phase4.5 subnet route %s not found "
                          "rc=%d (not programmed or already removed)",
                          portName.c_str(), subnet.to_string().c_str(), rs);
        }
        else
        {
            SWSS_LOG_ERROR("ARS-MIGRATE[%s]: Phase4.5 FAILED to remove subnet "
                           "route %s: SAI rc=%d — RIF may still be in use",
                           portName.c_str(), subnet.to_string().c_str(), rs);
        }
    }

    SWSS_LOG_NOTICE("ARS-MIGRATE[%s]: Phase4.5 COMPLETE - removed %zu IP-to-me "
                    "routes + subnet routes for %zu prefixes",
                    portName.c_str(), ip2meRemoved, savedIpPrefixes.size());

    // ─── Phase 5: Remove the RIF ─────────────────────────────────────────

    SWSS_LOG_NOTICE("ARS-MIGRATE[%s]: Phase5 removing RIF: oid=0x%" PRIx64
                    " vrf=0x%" PRIx64 " port_oid=0x%" PRIx64,
                    portName.c_str(), oldRifId, vrfId, port.m_port_id);

    {
        int intfRefCount = 0;
        if (gIntfsOrch)
        {
            auto &syncdIntfses = gIntfsOrch->getSyncdIntfses();
            auto intfIt = syncdIntfses.find(portName);
            if (intfIt != syncdIntfses.end())
                intfRefCount = intfIt->second.ref_count;
        }
        SWSS_LOG_NOTICE("ARS-MIGRATE[%s]: Phase5 pre-check: IntfsOrch ref_count=%d "
                        "ip_addrs_remaining=%zu",
                        portName.c_str(), intfRefCount,
                        gIntfsOrch ? gIntfsOrch->getSyncdIntfses().count(portName)
                            ? gIntfsOrch->getSyncdIntfses().at(portName).ip_addresses.size()
                            : 0 : 0);
    }

    st = sai_router_intfs_api->remove_router_interface(oldRifId);
    if (st != SAI_STATUS_SUCCESS)
    {
        // Phase 5 failed.  If savedIpPrefixes was empty, it's likely
        // that SAI routes (IP-to-me /32, connected subnet) still reference
        // this RIF but weren't collected in Phase 1 (IntfsOrch state gap).
        // Try to discover IPs from the kernel and remove those routes.
        if (st == (sai_status_t)(-17) && savedIpPrefixes.empty())
        {
            SWSS_LOG_WARN("ARS-MIGRATE[%s]: Phase5 rc=-17 with 0 prefixes "
                          "collected — attempting IP route discovery from kernel",
                          portName.c_str());

            vector<IpPrefix> discoveredPrefixes;
            struct ifaddrs *ifAddrList = nullptr;
            if (getifaddrs(&ifAddrList) == 0)
            {
                for (struct ifaddrs *ifa = ifAddrList; ifa; ifa = ifa->ifa_next)
                {
                    if (!ifa->ifa_addr || !ifa->ifa_netmask)
                        continue;
                    if (string(ifa->ifa_name) != portName)
                        continue;
                    if (ifa->ifa_addr->sa_family == AF_INET)
                    {
                        char ipBuf[INET_ADDRSTRLEN];
                        char maskBuf[INET_ADDRSTRLEN];
                        inet_ntop(AF_INET,
                                  &((struct sockaddr_in *)ifa->ifa_addr)->sin_addr,
                                  ipBuf, sizeof(ipBuf));
                        inet_ntop(AF_INET,
                                  &((struct sockaddr_in *)ifa->ifa_netmask)->sin_addr,
                                  maskBuf, sizeof(maskBuf));
                        uint32_t mask = ntohl(
                            ((struct sockaddr_in *)ifa->ifa_netmask)->sin_addr.s_addr);
                        int prefixLen = __builtin_popcount(mask);
                        string prefixStr = string(ipBuf) + "/" +
                                           to_string(prefixLen);
                        try
                        {
                            IpPrefix pfx(prefixStr);
                            discoveredPrefixes.push_back(pfx);
                            SWSS_LOG_NOTICE("ARS-MIGRATE[%s]: Phase5 discovered "
                                            "kernel IP: %s",
                                            portName.c_str(), prefixStr.c_str());
                        }
                        catch (...) {}
                    }
                }
                freeifaddrs(ifAddrList);
            }

            if (!discoveredPrefixes.empty())
            {
                for (auto &prefix : discoveredPrefixes)
                {
                    SWSS_LOG_NOTICE("ARS-MIGRATE[%s]: Phase5 removing "
                                    "discovered IP-to-me route: %s/32",
                                    portName.c_str(),
                                    prefix.getIp().to_string().c_str());
                    try { gIntfsOrch->removeIp2MeRoute(vrfId, prefix); }
                    catch (...) {}

                    IpPrefix subnet = prefix.getSubnet();
                    sai_route_entry_t subnet_route;
                    subnet_route.switch_id = gSwitchId;
                    subnet_route.vr_id = vrfId;
                    copy(subnet_route.destination, subnet);

                    SWSS_LOG_NOTICE("ARS-MIGRATE[%s]: Phase5 removing "
                                    "discovered subnet route: %s",
                                    portName.c_str(),
                                    subnet.to_string().c_str());
                    sai_route_api->remove_route_entry(&subnet_route);
                }

                // Retry RIF removal
                st = sai_router_intfs_api->remove_router_interface(oldRifId);
                if (st == SAI_STATUS_SUCCESS)
                {
                    SWSS_LOG_NOTICE("ARS-MIGRATE[%s]: Phase5 RETRY SUCCEEDED "
                                    "after removing discovered routes",
                                    portName.c_str());
                    // Update savedIpPrefixes for Phase 7.5 restoration
                    savedIpPrefixes = discoveredPrefixes;
                    goto phase5_success;
                }
                SWSS_LOG_ERROR("ARS-MIGRATE[%s]: Phase5 RETRY FAILED rc=%d "
                               "even after removing discovered routes",
                               portName.c_str(), st);
            }
        }

        SWSS_LOG_ERROR("ARS-MIGRATE[%s]: Phase5 FAILED - "
                       "remove_router_interface(0x%" PRIx64 ") returned SAI rc=%d. "
                       "Possible remaining references: connected routes, FDB, ACL, "
                       "or other SAI objects bound to this RIF. "
                       "Jumping to emergency rollback.",
                       portName.c_str(), oldRifId, st);
        goto restore_rif_without_ars;
    }
phase5_success:
    SWSS_LOG_NOTICE("ARS-MIGRATE[%s]: Phase5 COMPLETE - RIF 0x%" PRIx64
                    " removed successfully (SAI rc=0)",
                    portName.c_str(), oldRifId);

    port.m_rif_id = 0;
    m_portsOrch->setPort(portName, port);

    // ─── Phase 6: Enable ARS on the bare port ────────────────────────────

    SWSS_LOG_NOTICE("ARS-MIGRATE[%s]: Phase6 setting SAI_PORT_ATTR_ARS_ENABLE=true "
                    "on port_oid=0x%" PRIx64 " (port now has rifs=0)",
                    portName.c_str(), port.m_port_id);

    sai_attribute_t attr;
    attr.id = SAI_PORT_ATTR_ARS_ENABLE;
    attr.value.booldata = true;

    st = sai_port_api->set_port_attribute(port.m_port_id, &attr);
    if (st != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("ARS-MIGRATE[%s]: Phase6 CRITICAL FAILURE - "
                       "set_port_attribute(ARS_ENABLE) on port_oid=0x%" PRIx64
                       " returned SAI rc=%d even though RIF was removed. "
                       "Possible causes: (1) another RIF exists on this port/LAG, "
                       "(2) ARS profile not created, (3) SAI internal error. "
                       "Attempting emergency RIF restoration.",
                       portName.c_str(), port.m_port_id, st);
        goto restore_rif_without_ars;
    }
    SWSS_LOG_NOTICE("ARS-MIGRATE[%s]: Phase6 COMPLETE - ARS enabled on bare port",
                    portName.c_str());

    // ─── Phase 7: Recreate the RIF (SAI now picks AR type) ───────────────
    {
        vector<sai_attribute_t> rif_attrs;
        sai_attribute_t rif_attr;

        rif_attr.id = SAI_ROUTER_INTERFACE_ATTR_VIRTUAL_ROUTER_ID;
        rif_attr.value.oid = vrfId;
        rif_attrs.push_back(rif_attr);

        rif_attr.id = SAI_ROUTER_INTERFACE_ATTR_TYPE;
        rif_attr.value.s32 = SAI_ROUTER_INTERFACE_TYPE_PORT;
        rif_attrs.push_back(rif_attr);

        rif_attr.id = SAI_ROUTER_INTERFACE_ATTR_PORT_ID;
        rif_attr.value.oid = port.m_port_id;
        rif_attrs.push_back(rif_attr);

        rif_attr.id = SAI_ROUTER_INTERFACE_ATTR_SRC_MAC_ADDRESS;
        memcpy(rif_attr.value.mac, gMacAddress.getMac(), sizeof(sai_mac_t));
        rif_attrs.push_back(rif_attr);

        if (port.m_mtu != 0)
        {
            rif_attr.id = SAI_ROUTER_INTERFACE_ATTR_MTU;
            rif_attr.value.u32 = port.m_mtu;
            rif_attrs.push_back(rif_attr);
        }

        SWSS_LOG_NOTICE("ARS-MIGRATE[%s]: Phase7 creating RIF with ARS: "
                        "vrf=0x%" PRIx64 " port_oid=0x%" PRIx64 " mtu=%u "
                        "(SAI should pick SX_L2_INTERFACE_TYPE_ADAPTIVE_ROUTING)",
                        portName.c_str(), vrfId, port.m_port_id, port.m_mtu);

        sai_object_id_t newRifId;
        st = sai_router_intfs_api->create_router_interface(
            &newRifId, gSwitchId,
            (uint32_t)rif_attrs.size(), rif_attrs.data());
        if (st != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_ERROR("ARS-MIGRATE[%s]: Phase7 CRITICAL FAILURE - "
                           "create_router_interface returned SAI rc=%d. "
                           "ARS is enabled on port but RIF creation failed. "
                           "Disabling ARS and attempting non-AR RIF restore.",
                           portName.c_str(), st);
            attr.value.booldata = false;
            sai_port_api->set_port_attribute(port.m_port_id, &attr);
            goto restore_rif_without_ars;
        }

        SWSS_LOG_NOTICE("ARS-MIGRATE[%s]: Phase7 COMPLETE - AR RIF created: "
                        "old_rif=0x%" PRIx64 " new_rif=0x%" PRIx64,
                        portName.c_str(), oldRifId, newRifId);

        port.m_rif_id = newRifId;
        port.m_vr_id = vrfId;
        m_portsOrch->setPort(portName, port);

        // ─── Phase 7.5: Recreate routes removed in Phase 4.5 ────────────

        for (auto &prefix : savedIpPrefixes)
        {
            // Restore IP-to-me host route
            SWSS_LOG_NOTICE("ARS-MIGRATE[%s]: Phase7.5 restoring IP-to-me route: "
                            "%s/32 vrf=0x%" PRIx64,
                            portName.c_str(),
                            prefix.getIp().to_string().c_str(), vrfId);
            try
            {
                gIntfsOrch->addIp2MeRoute(vrfId, prefix);
                SWSS_LOG_NOTICE("ARS-MIGRATE[%s]: Phase7.5 restored IP-to-me "
                                "route: %s/32 rc=OK",
                                portName.c_str(),
                                prefix.getIp().to_string().c_str());
            }
            catch (const exception &e)
            {
                SWSS_LOG_ERROR("ARS-MIGRATE[%s]: Phase7.5 FAILED to restore "
                               "IP-to-me route %s: %s",
                               portName.c_str(), prefix.to_string().c_str(),
                               e.what());
            }

            // Restore connected subnet route pointing to the new AR RIF
            IpPrefix subnet = prefix.getSubnet();
            sai_route_entry_t subnet_route;
            subnet_route.switch_id = gSwitchId;
            subnet_route.vr_id = vrfId;
            copy(subnet_route.destination, subnet);

            sai_attribute_t rt_attr;
            rt_attr.id = SAI_ROUTE_ENTRY_ATTR_NEXT_HOP_ID;
            rt_attr.value.oid = newRifId;

            SWSS_LOG_NOTICE("ARS-MIGRATE[%s]: Phase7.5 restoring subnet route: "
                            "%s next_hop=new_rif=0x%" PRIx64,
                            portName.c_str(), subnet.to_string().c_str(),
                            newRifId);

            sai_status_t rs = sai_route_api->create_route_entry(
                &subnet_route, 1, &rt_attr);
            if (rs == SAI_STATUS_SUCCESS)
            {
                SWSS_LOG_NOTICE("ARS-MIGRATE[%s]: Phase7.5 restored subnet "
                                "route: %s rc=OK",
                                portName.c_str(), subnet.to_string().c_str());
            }
            else if (rs == SAI_STATUS_ITEM_ALREADY_EXISTS)
            {
                SWSS_LOG_WARN("ARS-MIGRATE[%s]: Phase7.5 subnet route %s "
                              "already exists rc=%d (re-created by RouteOrch?)",
                              portName.c_str(), subnet.to_string().c_str(), rs);
            }
            else
            {
                SWSS_LOG_ERROR("ARS-MIGRATE[%s]: Phase7.5 FAILED to restore "
                               "subnet route %s: SAI rc=%d",
                               portName.c_str(), subnet.to_string().c_str(), rs);
            }
        }

        SWSS_LOG_NOTICE("ARS-MIGRATE[%s]: Phase7.5 COMPLETE - restored routes "
                        "for %zu prefixes",
                        portName.c_str(), savedIpPrefixes.size());

        // ─── Phase 8: Recreate neighbor entries ──────────────────────────

        size_t neighCreateSuccess = 0, neighCreateFail = 0;
        for (auto &sn : savedNeighbors)
        {
            sai_neighbor_entry_t neighbor_entry;
            neighbor_entry.switch_id = gSwitchId;
            neighbor_entry.rif_id = newRifId;
            copy(neighbor_entry.ip_address, sn.entry.ip_address);

            sai_attribute_t neigh_attr;
            neigh_attr.id = SAI_NEIGHBOR_ENTRY_ATTR_DST_MAC_ADDRESS;
            memcpy(neigh_attr.value.mac, sn.mac.getMac(), sizeof(sai_mac_t));

            st = sai_neighbor_api->create_neighbor_entry(
                &neighbor_entry, 1, &neigh_attr);
            if (st != SAI_STATUS_SUCCESS)
            {
                SWSS_LOG_ERROR("ARS-MIGRATE[%s]: Phase8 FAILED to recreate "
                               "neighbor ip=%s mac=%s on new_rif=0x%" PRIx64
                               " SAI rc=%d",
                               portName.c_str(),
                               sn.entry.ip_address.to_string().c_str(),
                               sn.mac.to_string().c_str(), newRifId, st);
                neighCreateFail++;
                continue;
            }

            if (sn.entry.ip_address.isV4())
                gCrmOrch->incCrmResUsedCounter(CrmResourceType::CRM_IPV4_NEIGHBOR);
            else
                gCrmOrch->incCrmResUsedCounter(CrmResourceType::CRM_IPV6_NEIGHBOR);

            gIntfsOrch->increaseRouterIntfsRefCount(portName);
            neighCreateSuccess++;
        }

        SWSS_LOG_NOTICE("ARS-MIGRATE[%s]: Phase8 COMPLETE - recreated %zu/%zu "
                        "neighbors (%zu failed)",
                        portName.c_str(), neighCreateSuccess,
                        savedNeighbors.size(), neighCreateFail);

        // ─── Phase 9: Recreate next-hop objects with new RIF OID ─────────

        size_t nhCreateSuccess = 0, nhCreateFail = 0;
        for (auto &snh : savedNextHops)
        {
            vector<sai_attribute_t> nh_attrs;
            sai_attribute_t nh_attr;

            nh_attr.id = SAI_NEXT_HOP_ATTR_TYPE;
            nh_attr.value.s32 = SAI_NEXT_HOP_TYPE_IP;
            nh_attrs.push_back(nh_attr);

            nh_attr.id = SAI_NEXT_HOP_ATTR_IP;
            copy(nh_attr.value.ipaddr, snh.key.ip_address);
            nh_attrs.push_back(nh_attr);

            nh_attr.id = SAI_NEXT_HOP_ATTR_ROUTER_INTERFACE_ID;
            nh_attr.value.oid = newRifId;
            nh_attrs.push_back(nh_attr);

            sai_object_id_t newNhId;
            st = sai_next_hop_api->create_next_hop(
                &newNhId, gSwitchId,
                (uint32_t)nh_attrs.size(), nh_attrs.data());
            if (st != SAI_STATUS_SUCCESS)
            {
                SWSS_LOG_ERROR("ARS-MIGRATE[%s]: Phase9 FAILED to recreate NH %s "
                               "with new_rif=0x%" PRIx64 " SAI rc=%d "
                               "(old_nh_oid was 0x%" PRIx64 ")",
                               portName.c_str(), snh.key.to_string().c_str(),
                               newRifId, st, snh.oldNhId);
                nhCreateFail++;
                continue;
            }

            if (snh.key.ip_address.isV4())
                gCrmOrch->incCrmResUsedCounter(CrmResourceType::CRM_IPV4_NEXTHOP);
            else
                gCrmOrch->incCrmResUsedCounter(CrmResourceType::CRM_IPV6_NEXTHOP);

            gIntfsOrch->increaseRouterIntfsRefCount(portName);

            auto &syncdNhs = gNeighOrch->getSyncdNextHops();
            auto nhIt = syncdNhs.find(snh.key);
            if (nhIt != syncdNhs.end())
            {
                nhIt->second.next_hop_id = newNhId;
            }

            SWSS_LOG_NOTICE("ARS-MIGRATE[%s]: Phase9 recreated NH %s: "
                           "old_oid=0x%" PRIx64 " new_oid=0x%" PRIx64
                           " rif=0x%" PRIx64,
                           portName.c_str(), snh.key.to_string().c_str(),
                           snh.oldNhId, newNhId, newRifId);
            nhCreateSuccess++;
        }

        SWSS_LOG_NOTICE("ARS-MIGRATE[%s]: Phase9 COMPLETE - recreated %zu/%zu "
                        "next-hops (%zu failed)",
                        portName.c_str(), nhCreateSuccess,
                        savedNextHops.size(), nhCreateFail);

        // ─── Phase 9.5: Restore single-NH routes drained in Phase 2.5 ───

        size_t routeRestoreSuccess = 0, routeRestoreFail = 0;
        for (auto &savedRoute : savedSingleNhRoutes)
        {
            auto &syncdNhs = gNeighOrch->getSyncdNextHops();
            auto nhIt = syncdNhs.find(savedRoute.nhKey);
            if (nhIt == syncdNhs.end() ||
                nhIt->second.next_hop_id == SAI_NULL_OBJECT_ID)
            {
                SWSS_LOG_WARN("ARS-MIGRATE[%s]: Phase9.5 SKIP route %s - "
                              "NH %s not recreated (NULL OID)",
                              portName.c_str(),
                              savedRoute.prefix.to_string().c_str(),
                              savedRoute.nhKey.to_string().c_str());
                routeRestoreFail++;
                continue;
            }

            sai_route_entry_t route_entry;
            route_entry.switch_id = gSwitchId;
            route_entry.vr_id = savedRoute.vrfId;
            copy(route_entry.destination, savedRoute.prefix);

            sai_attribute_t nh_attr;
            nh_attr.id = SAI_ROUTE_ENTRY_ATTR_NEXT_HOP_ID;
            nh_attr.value.oid = nhIt->second.next_hop_id;

            sai_status_t rs = sai_route_api->set_route_entry_attribute(
                &route_entry, &nh_attr);
            if (rs != SAI_STATUS_SUCCESS)
            {
                SWSS_LOG_ERROR("ARS-MIGRATE[%s]: Phase9.5 FAILED to restore "
                               "route %s nh=%s new_nh_oid=0x%" PRIx64
                               " rc=%d",
                               portName.c_str(),
                               savedRoute.prefix.to_string().c_str(),
                               savedRoute.nhKey.to_string().c_str(),
                               nhIt->second.next_hop_id, rs);
                routeRestoreFail++;
                continue;
            }

            sai_attribute_t fwd_attr;
            fwd_attr.id = SAI_ROUTE_ENTRY_ATTR_PACKET_ACTION;
            fwd_attr.value.s32 = SAI_PACKET_ACTION_FORWARD;
            sai_status_t fwd_rs = sai_route_api->set_route_entry_attribute(
                &route_entry, &fwd_attr);

            // Increment refcount unconditionally: the NH SAI binding above
            // already succeeded, so the hardware holds a reference from this
            // route to the NH object.  The refcount must reflect reality even
            // if the FORWARD action set fails below (the route would remain
            // DROP with a valid NH binding — an operator-visible error, but
            // rolling back the NH binding here would add complexity to an
            // already multi-phase error path with marginal benefit).
            gNeighOrch->increaseNextHopRefCount(savedRoute.nhKey);

            if (fwd_rs != SAI_STATUS_SUCCESS)
            {
                SWSS_LOG_ERROR("ARS-MIGRATE[%s]: Phase9.5 route %s NH rebound "
                               "but FORWARD failed rc=%d — route stays DROP "
                               "with elevated NH refcount (accepted trade-off)",
                               portName.c_str(),
                               savedRoute.prefix.to_string().c_str(), fwd_rs);
                routeRestoreFail++;
                continue;
            }

            routeRestoreSuccess++;

            SWSS_LOG_NOTICE("ARS-MIGRATE[%s]: Phase9.5 restored route: %s "
                            "nh=%s new_nh_oid=0x%" PRIx64,
                            portName.c_str(),
                            savedRoute.prefix.to_string().c_str(),
                            savedRoute.nhKey.to_string().c_str(),
                            nhIt->second.next_hop_id);
        }

        SWSS_LOG_NOTICE("ARS-MIGRATE[%s]: Phase9.5 COMPLETE - restored %zu/%zu "
                        "single-NH routes (%zu failed)",
                        portName.c_str(), routeRestoreSuccess,
                        savedSingleNhRoutes.size(), routeRestoreFail);

        // ─── Phase 10: Recreate NHG members with new NH OIDs ─────────────
        //
        // BATCH MODE: when m_batchMigrationMode is set (multiple ports being
        // migrated in a single doArsInterfaceTask round), we DEFER member
        // re-addition.  Re-adding an AR next-hop member into a non-AR NHG
        // creates a mixed AR/non-AR member state that causes the Mellanox SDK
        // to send an asynchronous switch_shutdown_request ~15 ms later.
        //
        // By deferring, the NHG shrinks to 0 members across all port
        // migrations.  processDeferredNhgMembers() then binds ARS to the
        // (empty) NHG and adds all members — all using AR RIFs.

        if (m_batchMigrationMode)
        {
            size_t deferred = 0;
            for (auto &rmInfo : removedMembers)
            {
                m_deferredNhgMembers.push_back({rmInfo.nhgKey, rmInfo.nhKey, rmInfo.seqId});
                deferred++;
            }

            // Discovery: if the NHG was emptied externally (e.g. by
            // forceUnbindArsFromNhg during cleanup), Phase 2 finds no members
            // to remove. Scan all NHGs to find ones whose KEY contains this
            // port's NH and ensure it gets deferred for re-addition.
            if (gRouteOrch)
            {
                for (auto &nhgEntry : gRouteOrch->getSyncdNextHopGroups())
                {
                    auto &nhgKey = nhgEntry.first;
                    if (nhgEntry.second.next_hop_group_id == SAI_NULL_OBJECT_ID)
                        continue;

                    for (auto &savedNh : savedNextHops)
                    {
                        if (!nhgKey.contains(savedNh.key))
                            continue;

                        // Check if already deferred (from Phase 2 removal)
                        bool alreadyDeferred = false;
                        for (auto &dm : m_deferredNhgMembers)
                        {
                            if (dm.nhgKey == nhgKey && dm.nhKey == savedNh.key)
                            {
                                alreadyDeferred = true;
                                break;
                            }
                        }
                        if (alreadyDeferred)
                            continue;

                        // Determine sequence ID from position in NHG key
                        uint32_t seqId = 1;
                        for (auto &nh : nhgKey.getNextHops())
                        {
                            if (nh == savedNh.key)
                                break;
                            seqId++;
                        }

                        m_deferredNhgMembers.push_back({nhgKey, savedNh.key, seqId});
                        deferred++;
                        SWSS_LOG_NOTICE("ARS-MIGRATE[%s]: Phase10 discovered missing "
                                        "NHG membership: nh=%s nhg=%s seq=%u",
                                        portName.c_str(),
                                        savedNh.key.to_string().c_str(),
                                        nhgKey.to_string().c_str(), seqId);
                    }
                }
            }

            SWSS_LOG_NOTICE("ARS-MIGRATE[%s]: Phase10 DEFERRED - %zu NHG members "
                            "queued for batch re-addition (avoiding mixed AR/non-AR state)",
                            portName.c_str(), deferred);
            SWSS_LOG_NOTICE("ARS-MIGRATE[%s]: Phase11 DEFERRED - NHG ARS binding "
                            "will happen after all ports are migrated",
                            portName.c_str());

            bool partialFailure = (neighCreateFail > 0 || nhCreateFail > 0 ||
                                   routeRestoreFail > 0);
            if (partialFailure)
            {
                SWSS_LOG_ERROR("ARS-MIGRATE[%s]: ===== MIGRATION PARTIAL (batch) ===== "
                               "old_rif=0x%" PRIx64 " new_rif=0x%" PRIx64
                               " neighbors=%zu/%zu NHs=%zu/%zu routes=%zu/%zu "
                               "NHG_members=DEFERRED(%zu)",
                               portName.c_str(), oldRifId, newRifId,
                               neighCreateSuccess, savedNeighbors.size(),
                               nhCreateSuccess, savedNextHops.size(),
                               routeRestoreSuccess, savedSingleNhRoutes.size(),
                               deferred);
                return false;
            }

            SWSS_LOG_NOTICE("ARS-MIGRATE[%s]: ===== MIGRATION COMPLETE (batch) ===== "
                            "Summary: old_rif=0x%" PRIx64 " new_rif=0x%" PRIx64
                            " neighbors=%zu/%zu NHs=%zu/%zu routes=%zu/%zu "
                            "NHG_members=DEFERRED(%zu)",
                            portName.c_str(), oldRifId, newRifId,
                            neighCreateSuccess, savedNeighbors.size(),
                            nhCreateSuccess, savedNextHops.size(),
                            routeRestoreSuccess, savedSingleNhRoutes.size(),
                            deferred);
            return true;
        }

        size_t memberCreateSuccess = 0, memberCreateFail = 0;
        for (auto &rmInfo : removedMembers)
        {
            auto &syncdNhs = gNeighOrch->getSyncdNextHops();
            auto nhIt = syncdNhs.find(rmInfo.nhKey);
            if (nhIt == syncdNhs.end() ||
                nhIt->second.next_hop_id == SAI_NULL_OBJECT_ID)
            {
                SWSS_LOG_WARN("ARS-MIGRATE[%s]: Phase10 SKIP - cannot restore "
                              "NHG member for NH %s (NH OID is NULL — "
                              "Phase9 create likely failed for this NH)",
                              portName.c_str(), rmInfo.nhKey.to_string().c_str());
                memberCreateFail++;
                continue;
            }

            auto &syncdNhgs = gRouteOrch->getSyncdNextHopGroups();
            auto nhgIt = syncdNhgs.find(rmInfo.nhgKey);
            if (nhgIt == syncdNhgs.end() ||
                nhgIt->second.next_hop_group_id == SAI_NULL_OBJECT_ID)
            {
                SWSS_LOG_WARN("ARS-MIGRATE[%s]: Phase10 SKIP - NHG %s not found "
                              "or has NULL OID",
                              portName.c_str(), rmInfo.nhgKey.to_string().c_str());
                memberCreateFail++;
                continue;
            }

            sai_object_id_t nhgOid = nhgIt->second.next_hop_group_id;

            vector<sai_attribute_t> member_attrs;
            sai_attribute_t m_attr;

            m_attr.id = SAI_NEXT_HOP_GROUP_MEMBER_ATTR_NEXT_HOP_GROUP_ID;
            m_attr.value.oid = nhgOid;
            member_attrs.push_back(m_attr);

            m_attr.id = SAI_NEXT_HOP_GROUP_MEMBER_ATTR_NEXT_HOP_ID;
            m_attr.value.oid = nhIt->second.next_hop_id;
            member_attrs.push_back(m_attr);

            // Do NOT add SEQUENCE_ID here. The target NHG may have been
            // created as non-ordered ECMP (e.g., during ARS disable when
            // switch hash was static). Adding SEQUENCE_ID to a non-ordered
            // NHG causes the Memory SDK to reject the call, crashing syncd.
            // Phase11 (recreateNhgsWithArs) will recreate this NHG as
            // properly ordered ECMP with correct SEQUENCE_ID on all members.

            sai_object_id_t newMemberOid;
            st = sai_next_hop_group_api->create_next_hop_group_member(
                &newMemberOid, gSwitchId,
                (uint32_t)member_attrs.size(), member_attrs.data());
            if (st != SAI_STATUS_SUCCESS)
            {
                SWSS_LOG_ERROR("ARS-MIGRATE[%s]: Phase10 FAILED to create NHG member: "
                               "nhg_oid=0x%" PRIx64 " nh_oid=0x%" PRIx64
                               " nh=%s seq=%u SAI rc=%d",
                               portName.c_str(), nhgOid, nhIt->second.next_hop_id,
                               rmInfo.nhKey.to_string().c_str(), rmInfo.seqId, st);
                memberCreateFail++;
                continue;
            }

            gCrmOrch->incCrmResUsedCounter(CrmResourceType::CRM_NEXTHOP_GROUP_MEMBER);

            NextHopGroupMemberEntry memberEntry;
            memberEntry.next_hop_id = newMemberOid;
            memberEntry.seq_id = 0;
            nhgIt->second.nhopgroup_members[rmInfo.nhKey] = memberEntry;
            nhgIt->second.nh_member_install_count++;
            memberCreateSuccess++;

            SWSS_LOG_INFO("ARS-MIGRATE[%s]: Phase10 created NHG member (no seq): "
                          "nhg_oid=0x%" PRIx64 " member_oid=0x%" PRIx64
                          " nh=%s (saved_seq=%u will be set by Phase11)",
                          portName.c_str(), nhgOid, newMemberOid,
                          rmInfo.nhKey.to_string().c_str(), rmInfo.seqId);
        }

        SWSS_LOG_NOTICE("ARS-MIGRATE[%s]: Phase10 COMPLETE - recreated %zu/%zu "
                        "NHG members (%zu failed)",
                        portName.c_str(), memberCreateSuccess,
                        removedMembers.size(), memberCreateFail);

        // ─── Phase 11: Recreate NHGs with ARS (make-before-break) ────────
        SWSS_LOG_NOTICE("ARS-MIGRATE[%s]: Phase11 triggering make-before-break "
                        "NHG recreation for ARS binding",
                        portName.c_str());

        if (gRouteOrch)
        {
            gRouteOrch->recreateNhgsWithArs(portName);
        }

        bool partialFailure = (neighCreateFail > 0 || nhCreateFail > 0 ||
                               memberCreateFail > 0 || routeRestoreFail > 0);

        if (partialFailure)
        {
            SWSS_LOG_ERROR("ARS-MIGRATE[%s]: ===== MIGRATION PARTIAL ===== "
                           "ARS enabled and RIF recreated, but some dependent "
                           "objects failed to restore. "
                           "old_rif=0x%" PRIx64 " new_rif=0x%" PRIx64
                           " neighbors=%zu/%zu NHs=%zu/%zu routes=%zu/%zu "
                           "NHG_members=%zu/%zu. "
                           "Forwarding may be degraded on this port until "
                           "missing objects are resolved (e.g., neighbor re-learn).",
                           portName.c_str(), oldRifId, newRifId,
                           neighCreateSuccess, savedNeighbors.size(),
                           nhCreateSuccess, savedNextHops.size(),
                           routeRestoreSuccess, savedSingleNhRoutes.size(),
                           memberCreateSuccess, removedMembers.size());
            return false;
        }

        SWSS_LOG_NOTICE("ARS-MIGRATE[%s]: ===== MIGRATION COMPLETE ===== "
                        "Summary: old_rif=0x%" PRIx64 " new_rif=0x%" PRIx64
                        " neighbors=%zu/%zu NHs=%zu/%zu routes=%zu/%zu "
                        "NHG_members=%zu/%zu",
                        portName.c_str(), oldRifId, newRifId,
                        neighCreateSuccess, savedNeighbors.size(),
                        nhCreateSuccess, savedNextHops.size(),
                        routeRestoreSuccess, savedSingleNhRoutes.size(),
                        memberCreateSuccess, removedMembers.size());
        return true;
    }

restore_rif_without_ars:
    SWSS_LOG_ERROR("ARS-MIGRATE[%s]: ===== EMERGENCY ROLLBACK ===== "
                   "Restoring non-AR RIF and dependent objects. "
                   "Port state: port_oid=0x%" PRIx64
                   " rif_id=0x%" PRIx64 " vrf=0x%" PRIx64
                   " Objects to restore: %zu neighbors, %zu NHs, "
                   "%zu IP-to-me routes, %zu single-NH routes",
                   portName.c_str(), port.m_port_id, port.m_rif_id,
                   vrfId, savedNeighbors.size(), savedNextHops.size(),
                   ip2meRemoved, savedSingleNhRoutes.size());
    {
        sai_object_id_t restoredRifId;

        if (port.m_rif_id != 0)
        {
            // Old RIF was never removed (failed before Phase 5)
            restoredRifId = port.m_rif_id;
            SWSS_LOG_NOTICE("ARS-MIGRATE[%s]: ROLLBACK reusing existing RIF "
                            "0x%" PRIx64, portName.c_str(), restoredRifId);
        }
        else
        {
            vector<sai_attribute_t> rif_attrs;
            sai_attribute_t rif_attr;

            rif_attr.id = SAI_ROUTER_INTERFACE_ATTR_VIRTUAL_ROUTER_ID;
            rif_attr.value.oid = vrfId;
            rif_attrs.push_back(rif_attr);

            rif_attr.id = SAI_ROUTER_INTERFACE_ATTR_TYPE;
            rif_attr.value.s32 = SAI_ROUTER_INTERFACE_TYPE_PORT;
            rif_attrs.push_back(rif_attr);

            rif_attr.id = SAI_ROUTER_INTERFACE_ATTR_PORT_ID;
            rif_attr.value.oid = port.m_port_id;
            rif_attrs.push_back(rif_attr);

            rif_attr.id = SAI_ROUTER_INTERFACE_ATTR_SRC_MAC_ADDRESS;
            memcpy(rif_attr.value.mac, gMacAddress.getMac(), sizeof(sai_mac_t));
            rif_attrs.push_back(rif_attr);

            st = sai_router_intfs_api->create_router_interface(
                &restoredRifId, gSwitchId,
                (uint32_t)rif_attrs.size(), rif_attrs.data());
            if (st != SAI_STATUS_SUCCESS)
            {
                SWSS_LOG_ERROR("ARS-MIGRATE[%s]: ROLLBACK CRITICAL FAILURE - "
                               "create_router_interface returned SAI rc=%d. "
                               "Port has NO RIF — all traffic on this port "
                               "will be dropped. Immediate config reload "
                               "required. port_oid=0x%" PRIx64 " vrf=0x%" PRIx64,
                               portName.c_str(), st, port.m_port_id, vrfId);
                return false;
            }

            port.m_rif_id = restoredRifId;
            port.m_vr_id = vrfId;
            m_portsOrch->setPort(portName, port);
        }

        {
            SWSS_LOG_ERROR("ARS-MIGRATE[%s]: ROLLBACK restored non-AR RIF "
                           "0x%" PRIx64 ". Now restoring %zu neighbors and "
                           "%zu next-hops. Port will function without ARS.",
                           portName.c_str(), restoredRifId,
                           savedNeighbors.size(), savedNextHops.size());

            // Attempt to restore neighbors, NHs, and NHG members
            for (auto &sn : savedNeighbors)
            {
                sai_neighbor_entry_t ne;
                ne.switch_id = gSwitchId;
                ne.rif_id = restoredRifId;
                copy(ne.ip_address, sn.entry.ip_address);
                sai_attribute_t na;
                na.id = SAI_NEIGHBOR_ENTRY_ATTR_DST_MAC_ADDRESS;
                memcpy(na.value.mac, sn.mac.getMac(), sizeof(sai_mac_t));
                if (sai_neighbor_api->create_neighbor_entry(&ne, 1, &na) == SAI_STATUS_SUCCESS)
                {
                    if (sn.entry.ip_address.isV4())
                        gCrmOrch->incCrmResUsedCounter(CrmResourceType::CRM_IPV4_NEIGHBOR);
                    else
                        gCrmOrch->incCrmResUsedCounter(CrmResourceType::CRM_IPV6_NEIGHBOR);
                    gIntfsOrch->increaseRouterIntfsRefCount(portName);
                }
            }
            for (auto &snh : savedNextHops)
            {
                auto &syncdNhs = gNeighOrch->getSyncdNextHops();
                auto nhIt = syncdNhs.find(snh.key);

                // If Phase 3 failed to remove the NH, it still exists in SAI
                // with the original OID. Do NOT create a duplicate — just
                // verify the tracking is consistent and skip recreation.
                if (nhIt != syncdNhs.end() &&
                    nhIt->second.next_hop_id != SAI_NULL_OBJECT_ID)
                {
                    SWSS_LOG_NOTICE("ARS-MIGRATE[%s]: ROLLBACK NH %s still exists "
                                    "(oid=0x%" PRIx64 ") — reusing (Phase3 did not "
                                    "remove it)",
                                    portName.c_str(), snh.key.to_string().c_str(),
                                    nhIt->second.next_hop_id);
                    continue;
                }

                vector<sai_attribute_t> nh_attrs;
                sai_attribute_t na;
                na.id = SAI_NEXT_HOP_ATTR_TYPE;
                na.value.s32 = SAI_NEXT_HOP_TYPE_IP;
                nh_attrs.push_back(na);
                na.id = SAI_NEXT_HOP_ATTR_IP;
                copy(na.value.ipaddr, snh.key.ip_address);
                nh_attrs.push_back(na);
                na.id = SAI_NEXT_HOP_ATTR_ROUTER_INTERFACE_ID;
                na.value.oid = restoredRifId;
                nh_attrs.push_back(na);
                sai_object_id_t nhId;
                if (sai_next_hop_api->create_next_hop(&nhId, gSwitchId,
                    (uint32_t)nh_attrs.size(), nh_attrs.data()) == SAI_STATUS_SUCCESS)
                {
                    if (snh.key.ip_address.isV4())
                        gCrmOrch->incCrmResUsedCounter(CrmResourceType::CRM_IPV4_NEXTHOP);
                    else
                        gCrmOrch->incCrmResUsedCounter(CrmResourceType::CRM_IPV6_NEXTHOP);
                    if (nhIt != syncdNhs.end())
                        nhIt->second.next_hop_id = nhId;
                    gIntfsOrch->increaseRouterIntfsRefCount(portName);
                }
            }
            // Restore NHG members that were drained in Phase 2
            for (auto &rmInfo : removedMembers)
            {
                auto &syncdNhs = gNeighOrch->getSyncdNextHops();
                auto nhIt = syncdNhs.find(rmInfo.nhKey);
                if (nhIt == syncdNhs.end() ||
                    nhIt->second.next_hop_id == SAI_NULL_OBJECT_ID)
                    continue;

                auto &syncdNhgs = gRouteOrch->getSyncdNextHopGroups();
                auto nhgIt = syncdNhgs.find(rmInfo.nhgKey);
                if (nhgIt == syncdNhgs.end() ||
                    nhgIt->second.next_hop_group_id == SAI_NULL_OBJECT_ID)
                    continue;

                vector<sai_attribute_t> member_attrs;
                sai_attribute_t m_attr;
                m_attr.id = SAI_NEXT_HOP_GROUP_MEMBER_ATTR_NEXT_HOP_GROUP_ID;
                m_attr.value.oid = nhgIt->second.next_hop_group_id;
                member_attrs.push_back(m_attr);
                m_attr.id = SAI_NEXT_HOP_GROUP_MEMBER_ATTR_NEXT_HOP_ID;
                m_attr.value.oid = nhIt->second.next_hop_id;
                member_attrs.push_back(m_attr);
                // Do NOT add SEQUENCE_ID during rollback — the target NHG
                // may have been created as non-ordered ECMP (e.g., recreated
                // by forceUnbindArsFromNhg during ARS disable). Adding
                // SEQUENCE_ID to a non-ordered NHG crashes the Mellanox SDK.

                sai_object_id_t newMemberOid;
                if (sai_next_hop_group_api->create_next_hop_group_member(
                    &newMemberOid, gSwitchId,
                    (uint32_t)member_attrs.size(), member_attrs.data()) == SAI_STATUS_SUCCESS)
                {
                    gCrmOrch->incCrmResUsedCounter(CrmResourceType::CRM_NEXTHOP_GROUP_MEMBER);
                    NextHopGroupMemberEntry memberEntry;
                    memberEntry.next_hop_id = newMemberOid;
                    memberEntry.seq_id = 0;
                    nhgIt->second.nhopgroup_members[rmInfo.nhKey] = memberEntry;
                    nhgIt->second.nh_member_install_count++;
                }
            }
            // Restore default-route-swap NHG members removed in Phase 2
            for (auto &dsm : removedDfltSwapMembers)
            {
                auto &syncdNhs = gNeighOrch->getSyncdNextHops();
                auto nhIt = syncdNhs.find(dsm.nhKey);
                if (nhIt == syncdNhs.end() ||
                    nhIt->second.next_hop_id == SAI_NULL_OBJECT_ID)
                    continue;

                auto &syncdNhgs = gRouteOrch->getSyncdNextHopGroups();
                auto nhgIt = syncdNhgs.find(dsm.nhgKey);
                if (nhgIt == syncdNhgs.end() ||
                    nhgIt->second.next_hop_group_id == SAI_NULL_OBJECT_ID)
                    continue;

                vector<sai_attribute_t> member_attrs;
                sai_attribute_t m_attr;
                m_attr.id = SAI_NEXT_HOP_GROUP_MEMBER_ATTR_NEXT_HOP_GROUP_ID;
                m_attr.value.oid = nhgIt->second.next_hop_group_id;
                member_attrs.push_back(m_attr);
                m_attr.id = SAI_NEXT_HOP_GROUP_MEMBER_ATTR_NEXT_HOP_ID;
                m_attr.value.oid = nhIt->second.next_hop_id;
                member_attrs.push_back(m_attr);

                sai_object_id_t newMemberOid;
                if (sai_next_hop_group_api->create_next_hop_group_member(
                    &newMemberOid, gSwitchId,
                    (uint32_t)member_attrs.size(), member_attrs.data()) == SAI_STATUS_SUCCESS)
                {
                    gCrmOrch->incCrmResUsedCounter(CrmResourceType::CRM_NEXTHOP_GROUP_MEMBER);
                    gNeighOrch->increaseNextHopRefCount(dsm.nhKey);
                    NextHopGroupMemberEntry memberEntry;
                    memberEntry.next_hop_id = newMemberOid;
                    memberEntry.seq_id = 0;
                    nhgIt->second.default_route_nhopgroup_members[dsm.nhKey] = memberEntry;
                }
            }
            // Restore routes removed in Phase 4.5
            for (auto &prefix : savedIpPrefixes)
            {
                try
                {
                    gIntfsOrch->addIp2MeRoute(vrfId, prefix);
                }
                catch (const exception &e)
                {
                    SWSS_LOG_ERROR("ARS-MIGRATE[%s]: ROLLBACK failed to restore "
                                   "IP-to-me route %s: %s",
                                   portName.c_str(), prefix.to_string().c_str(),
                                   e.what());
                }

                IpPrefix subnet = prefix.getSubnet();
                sai_route_entry_t subnet_route;
                subnet_route.switch_id = gSwitchId;
                subnet_route.vr_id = vrfId;
                copy(subnet_route.destination, subnet);

                sai_attribute_t rt_attr;
                rt_attr.id = SAI_ROUTE_ENTRY_ATTR_NEXT_HOP_ID;
                rt_attr.value.oid = restoredRifId;
                sai_route_api->create_route_entry(
                    &subnet_route, 1, &rt_attr);
            }
            // Restore single-NH routes drained in Phase 2.5
            size_t rollbackRouteSuccess = 0, rollbackRouteFail = 0;
            for (auto &savedRoute : savedSingleNhRoutes)
            {
                auto &syncdNhs = gNeighOrch->getSyncdNextHops();
                auto nhIt = syncdNhs.find(savedRoute.nhKey);
                if (nhIt == syncdNhs.end() ||
                    nhIt->second.next_hop_id == SAI_NULL_OBJECT_ID)
                {
                    SWSS_LOG_WARN("ARS-MIGRATE[%s]: ROLLBACK SKIP route %s - "
                                  "NH %s not available (NULL OID) — route "
                                  "stays DROP",
                                  portName.c_str(),
                                  savedRoute.prefix.to_string().c_str(),
                                  savedRoute.nhKey.to_string().c_str());
                    rollbackRouteFail++;
                    continue;
                }

                sai_route_entry_t re;
                re.switch_id = gSwitchId;
                re.vr_id = savedRoute.vrfId;
                copy(re.destination, savedRoute.prefix);

                sai_attribute_t nh_attr;
                nh_attr.id = SAI_ROUTE_ENTRY_ATTR_NEXT_HOP_ID;
                nh_attr.value.oid = nhIt->second.next_hop_id;
                sai_status_t nh_rs = sai_route_api->set_route_entry_attribute(
                    &re, &nh_attr);
                if (nh_rs != SAI_STATUS_SUCCESS)
                {
                    SWSS_LOG_ERROR("ARS-MIGRATE[%s]: ROLLBACK failed to rebind "
                                   "NH on route %s rc=%d",
                                   portName.c_str(),
                                   savedRoute.prefix.to_string().c_str(),
                                   nh_rs);
                    rollbackRouteFail++;
                    continue;
                }

                gNeighOrch->increaseNextHopRefCount(savedRoute.nhKey);

                sai_attribute_t fwd_attr;
                fwd_attr.id = SAI_ROUTE_ENTRY_ATTR_PACKET_ACTION;
                fwd_attr.value.s32 = SAI_PACKET_ACTION_FORWARD;
                sai_status_t fwd_rs = sai_route_api->set_route_entry_attribute(
                    &re, &fwd_attr);
                if (fwd_rs != SAI_STATUS_SUCCESS)
                {
                    SWSS_LOG_ERROR("ARS-MIGRATE[%s]: ROLLBACK NH rebound but "
                                   "FORWARD failed on route %s rc=%d",
                                   portName.c_str(),
                                   savedRoute.prefix.to_string().c_str(),
                                   fwd_rs);
                    rollbackRouteFail++;
                }
                else
                {
                    rollbackRouteSuccess++;
                }

                SWSS_LOG_NOTICE("ARS-MIGRATE[%s]: ROLLBACK restored single-NH "
                                "route: %s nh=%s",
                                portName.c_str(),
                                savedRoute.prefix.to_string().c_str(),
                                savedRoute.nhKey.to_string().c_str());
            }
            if (!savedSingleNhRoutes.empty())
            {
                SWSS_LOG_NOTICE("ARS-MIGRATE[%s]: ROLLBACK route restore: "
                                "%zu/%zu succeeded, %zu failed",
                                portName.c_str(), rollbackRouteSuccess,
                                savedSingleNhRoutes.size(), rollbackRouteFail);
            }
        }
        return false;
    }
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

    // Re-enable any ARS objects left over from a previous cleanup failure
    // (e.g. remove_ars returned OBJECT_IN_USE due to a leaked NHG from
    // ordered-ECMP reuse). The SAI object still exists and can be reused;
    // RIF migration will create fresh NHGs that bind to it, eventually
    // orphaning the leaked NHG.
    for (auto &kv : m_arsObjects)
    {
        if (!kv.second.enabled && kv.second.arsOid != SAI_NULL_OBJECT_ID)
        {
            SWSS_LOG_NOTICE("ARS: re-enabling leaked object '%s' "
                            "(OID 0x%" PRIx64 ") from previous cycle",
                            kv.first.c_str(), kv.second.arsOid);
            kv.second.enabled = true;
        }
    }

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

    if (!m_portScalingFactorSupported)
    {
        SWSS_LOG_NOTICE("ARS: SAI_PORT_ATTR_ARS_PORT_LOAD_SCALING_FACTOR not "
                        "implemented on this platform — skipping SET on %s "
                        "(factor=%u). The SDK may derive scaling internally.",
                        portName.c_str(), factor);
        return true;
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
        if (!m_portPastWeightSupported)
        {
            SWSS_LOG_NOTICE("ARS: SAI_PORT_ATTR_ARS_PORT_LOAD_PAST_WEIGHT not "
                            "implemented — skipping SET on %s (weight=%u)",
                            portName.c_str(), pastWeight);
        }
        else
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
    }

    if (futureWeight > 0)
    {
        if (!m_portFutureWeightSupported)
        {
            SWSS_LOG_NOTICE("ARS: SAI_PORT_ATTR_ARS_PORT_LOAD_FUTURE_WEIGHT not "
                            "implemented — skipping SET on %s (weight=%u)",
                            portName.c_str(), futureWeight);
        }
        else
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

    /* Probe ARS_PROFILE attributes so the ars-classifier-daemon knows
       whether SAI handles ipv4/ipv6 enable natively or whether the daemon
       must make direct SDK calls. */
    static const vector<pair<string, sai_ars_profile_attr_t>> profileAttrProbes = {
        {"SAI_ARS_PROFILE_ATTR_ENABLE_IPV4", SAI_ARS_PROFILE_ATTR_ENABLE_IPV4},
        {"SAI_ARS_PROFILE_ATTR_ENABLE_IPV6", SAI_ARS_PROFILE_ATTR_ENABLE_IPV6},
    };
    for (const auto &attrPair : profileAttrProbes)
    {
        const auto &attrName = attrPair.first;
        const auto &attrId = attrPair.second;
        sai_attr_capability_t ac = {};
        sai_status_t qs = sai_query_attribute_capability(
            gSwitchId, SAI_OBJECT_TYPE_ARS_PROFILE, attrId, &ac);
        string capStr = "unknown";
        if (qs == SAI_STATUS_SUCCESS)
        {
            capStr = string("create=") + (ac.create_implemented ? "true" : "false") +
                     ",set=" + (ac.set_implemented ? "true" : "false") +
                     ",get=" + (ac.get_implemented ? "true" : "false");
            if (attrId == SAI_ARS_PROFILE_ATTR_ENABLE_IPV4)
                m_profileIpv4Supported = ac.create_implemented;
            else if (attrId == SAI_ARS_PROFILE_ATTR_ENABLE_IPV6)
                m_profileIpv6Supported = ac.create_implemented;
        }
        SWSS_LOG_NOTICE("ARS profile capability %s: %s", attrName.c_str(), capStr.c_str());
        m_stateArsCapTable.set(attrName, {{attrName, capStr}});
    }

    // Probe per-port ARS attributes so we never send unsupported SETs to
    // syncd (which treats any SET failure as fatal -> shutdown).
    {
        sai_attr_capability_t ac = {};
        sai_status_t qs = sai_query_attribute_capability(
            gSwitchId, SAI_OBJECT_TYPE_PORT,
            (sai_attr_id_t)SAI_PORT_ATTR_ARS_PORT_LOAD_SCALING_FACTOR, &ac);
        m_portScalingFactorSupported = (qs == SAI_STATUS_SUCCESS && ac.set_implemented);
        SWSS_LOG_NOTICE("ARS: SAI_PORT_ATTR_ARS_PORT_LOAD_SCALING_FACTOR set_supported=%s",
                        m_portScalingFactorSupported ? "true" : "false");
    }
    {
        sai_attr_capability_t ac = {};
        sai_status_t qs = sai_query_attribute_capability(
            gSwitchId, SAI_OBJECT_TYPE_PORT,
            (sai_attr_id_t)SAI_PORT_ATTR_ARS_PORT_LOAD_PAST_WEIGHT, &ac);
        m_portPastWeightSupported = (qs == SAI_STATUS_SUCCESS && ac.set_implemented);
        SWSS_LOG_NOTICE("ARS: SAI_PORT_ATTR_ARS_PORT_LOAD_PAST_WEIGHT set_supported=%s",
                        m_portPastWeightSupported ? "true" : "false");
    }
    {
        sai_attr_capability_t ac = {};
        sai_status_t qs = sai_query_attribute_capability(
            gSwitchId, SAI_OBJECT_TYPE_PORT,
            (sai_attr_id_t)SAI_PORT_ATTR_ARS_PORT_LOAD_FUTURE_WEIGHT, &ac);
        m_portFutureWeightSupported = (qs == SAI_STATUS_SUCCESS && ac.set_implemented);
        SWSS_LOG_NOTICE("ARS: SAI_PORT_ATTR_ARS_PORT_LOAD_FUTURE_WEIGHT set_supported=%s",
                        m_portFutureWeightSupported ? "true" : "false");
    }
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
    // Mellanox SAI accepts large quant-band values during create_ars_profile
    // but rejects them during set_ars_profile_attribute with
    // SAI_STATUS_INVALID_PARAMETER. The deferred-OID reuse path uses SET,
    // so defaults must be within the SET-safe range. These values match
    // the ucli defaults (lb_adaptive_profile.py).
    // Unit is bytes since SAI v2511.36.0.0 (was cells previously).
    // On Spectrum-4 (SN5610), cell size = 192 bytes. These map to
    // ceil(2560/192)=14, ceil(5120/192)=27, ceil(12800/192)=67 cells.
    entry.quantBand0MinThreshold = 2560;
    entry.quantBand1MinThreshold = 5120;
    entry.quantBand2MinThreshold = 12800;
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
