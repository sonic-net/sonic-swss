#include "arsorch.h"
#include "routeorch.h"
#include "logger.h"
#include "schema.h"
#include "tokenize.h"
#include "sai_serialize.h"
#include "converter.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

using namespace std;
using namespace swss;

extern sai_ars_api_t*            sai_ars_api;
extern sai_ars_profile_api_t*    sai_ars_profile_api;
extern sai_switch_api_t*         sai_switch_api;
extern sai_next_hop_group_api_t* sai_next_hop_group_api;
extern sai_lag_api_t*            sai_lag_api;
extern sai_port_api_t*           sai_port_api;
extern sai_object_id_t           gSwitchId;
extern RouteOrch                *gRouteOrch;

static const map<string, sai_ars_mode_t> arsModeLookup = {
    {"flowlet-quality",  SAI_ARS_MODE_FLOWLET_QUALITY},
    {"packet-quality",   SAI_ARS_MODE_PER_PACKET_QUALITY},
};

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
      m_stateArsNhgTable(stateDb, "ARS_NHG_TABLE"),
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
                    wantEnable = (fvValue(fv) == "up");
                else if (fvField(fv) == "profile")
                    profileName = fvValue(fv);
            }

            if (wantEnable && !m_arsEnabled)
            {
                SWSS_LOG_NOTICE("ARS: Adaptive Routing globally enabled");
                m_arsEnabled = true;

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
                    bindArsProfileToSwitch(profIt->second.profileOid);
                    m_globalProfileName = profileName;
                    SWSS_LOG_NOTICE("ARS: bound profile '%s' to switch",
                                    profileName.c_str());
                    // The new profile may carry different EWMA thresholds
                    // and port-select behavior than the previous one. Some
                    // vendor backends snapshot profile values into ARS
                    // object state at creation time, so existing NHG
                    // bindings may not pick up the new thresholds without
                    // a rebind. Walk all NHGs so the updated profile
                    // reaches the data plane.
                    if (gRouteOrch && m_arsEnabled)
                        gRouteOrch->rebindArsForAllNhgs();
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
                bindArsProfileToSwitch(SAI_NULL_OBJECT_ID);
                m_globalProfileName.clear();
                SWSS_LOG_NOTICE("ARS: unbound profile from switch");
                // Same reasoning as above — without a bound profile the
                // ARS objects revert to whatever the vendor default is,
                // so refresh NHG bindings.
                if (gRouteOrch && m_arsEnabled)
                    gRouteOrch->rebindArsForAllNhgs();
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
            bool explicitLoadCurrent = false;
            bool explicitLoadCurrentWeight = false;
            for (auto &fv : kfvFieldsValues(kfv))
            {
                const string &field = fvField(fv);
                const string &value = fvValue(fv);

                if      (field == "port_load_past_weight" || field == "load_past_weight")
                    entry.loadPastWeight   = static_cast<uint32_t>(stoul(value));
                else if (field == "port_load_future_weight" || field == "load_future_weight")
                    entry.loadFutureWeight = static_cast<uint32_t>(stoul(value));
                else if (field == "port_load_current_weight" || field == "load_current_weight")
                {
                    entry.loadCurrentWeight = static_cast<uint32_t>(stoul(value));
                    explicitLoadCurrentWeight = true;
                }
                else if (field == "load_exponent")       entry.loadExponent      = static_cast<uint32_t>(stoul(value));
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
                    if (value != "EWMA")
                        SWSS_LOG_WARN("ARS: unsupported algorithm '%s', using EWMA", value.c_str());
                }
                else if (field == "quantization_type")
                {
                    if (value == "log2")
                        entry.quantizationType = 1;
                    else
                        entry.quantizationType = 0;
                }
                else if (field == "link_utilization_threshold")
                    entry.profileLinkUtilThreshold = static_cast<uint32_t>(stoul(value));
                else if (field == "idle_time")
                    entry.profileIdleTime = static_cast<uint32_t>(stoul(value));
                else if (field == "default_ars_object")
                    entry.defaultArsObject = value;
                else
                    SWSS_LOG_WARN("ARS: unknown profile field '%s'", field.c_str());
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
                updateArsProfileAttr(oid, SAI_ARS_PROFILE_ATTR_PORT_LOAD_PAST_WEIGHT,   entry.loadPastWeight);
                updateArsProfileAttr(oid, SAI_ARS_PROFILE_ATTR_PORT_LOAD_FUTURE_WEIGHT,  entry.loadFutureWeight);
                updateArsProfileAttrBool(oid, SAI_ARS_PROFILE_ATTR_PORT_LOAD_PAST,       entry.loadPastEnable);
                updateArsProfileAttrBool(oid, SAI_ARS_PROFILE_ATTR_PORT_LOAD_FUTURE,     entry.loadFutureEnable);
                updateArsProfileAttrBool(oid, SAI_ARS_PROFILE_ATTR_PORT_LOAD_CURRENT,    entry.loadCurrentEnable);
                updateArsProfileAttr(oid, SAI_ARS_PROFILE_ATTR_PORT_LOAD_EXPONENT,       entry.loadExponent);
                updateArsProfileAttr(oid, SAI_ARS_PROFILE_ATTR_MAX_FLOWS,                entry.maxFlows);
                updateArsProfileAttr(oid, SAI_ARS_PROFILE_ATTR_LOAD_PAST_MIN_VAL,        entry.loadPastMinVal);
                updateArsProfileAttr(oid, SAI_ARS_PROFILE_ATTR_LOAD_PAST_MAX_VAL,        entry.loadPastMaxVal);
                updateArsProfileAttr(oid, SAI_ARS_PROFILE_ATTR_LOAD_FUTURE_MIN_VAL,      entry.loadFutureMinVal);
                updateArsProfileAttr(oid, SAI_ARS_PROFILE_ATTR_LOAD_FUTURE_MAX_VAL,      entry.loadFutureMaxVal);
                updateArsProfileAttr(oid, SAI_ARS_PROFILE_ATTR_LOAD_CURRENT_MIN_VAL,     entry.loadCurrentMinVal);
                updateArsProfileAttr(oid, SAI_ARS_PROFILE_ATTR_LOAD_CURRENT_MAX_VAL,     entry.loadCurrentMaxVal);
                updateArsProfileAttrBool(oid, SAI_ARS_PROFILE_ATTR_ENABLE_IPV4,          entry.ipv4Enable);
                updateArsProfileAttrBool(oid, SAI_ARS_PROFILE_ATTR_ENABLE_IPV6,          entry.ipv6Enable);
                if (entry.samplingInterval > 0)
                    updateArsProfileAttr(oid, SAI_ARS_PROFILE_ATTR_SAMPLING_INTERVAL,    entry.samplingInterval);
                if (entry.randomSeed > 0)
                    updateArsProfileAttr(oid, SAI_ARS_PROFILE_ATTR_ARS_RANDOM_SEED,      entry.randomSeed);
                m_arsProfiles[name] = entry;
                publishArsProfileState(name, entry);
            }
        }
        else if (op == DEL_COMMAND)
        {
            if (!removeArsProfile(name))
                SWSS_LOG_ERROR("ARS: failed to remove profile %s", name.c_str());
            m_stateArsProfileTable.del(name);
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
                else if (field == "admin_state") entry.enabled  = (value == "up");
                else if (field == "profile")     entry.profileName = value;
                else if (field == "port_profile") entry.portProfileName = value;
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
                if (prev.mode != entry.mode)
                    setArsObjectAttr(oid, SAI_ARS_ATTR_MODE, (uint32_t)entry.mode);
                if (isFlowletMode(entry.mode) && prev.idleTime != entry.idleTime)
                    setArsObjectAttr(oid, SAI_ARS_ATTR_IDLE_TIME, entry.idleTime);
                if (prev.maxFlows != entry.maxFlows)
                    setArsObjectAttr(oid, SAI_ARS_ATTR_MAX_FLOWS, entry.maxFlows);
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
            if (!removeArsObject(name))
                SWSS_LOG_ERROR("ARS: failed to remove object %s", name.c_str());
        }

        it = consumer.m_toSync.erase(it);
    }
}

/* ── ARS Interfaces (per-port membership) ────────────────────────────── */

void ArsOrch::doArsInterfaceTask(Consumer &consumer)
{
    SWSS_LOG_ENTER();

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

            for (auto &fv : kfvFieldsValues(kfv))
            {
                const string &field = fvField(fv);
                const string &value = fvValue(fv);

                if      (field == "admin_state")              entry.enabled = (value == "up");
                else if (field == "ars_object")               entry.arsObject = value;
                else if (field == "port_profile")             entry.portProfile = value;
                else if (field == "link_utilization_threshold") entry.linkUtilThreshold = static_cast<uint32_t>(stoul(value));
                else if (field == "weight")
                    entry.weight = static_cast<uint32_t>(stoul(value));
                else
                    SWSS_LOG_WARN("ARS: unknown interface field '%s' on %s",
                                  field.c_str(), portName.c_str());
            }

            bool prevEnabled = (m_arsEnabledPorts.find(portName) != m_arsEnabledPorts.end());

            if (entry.enabled && !prevEnabled)
            {
                if (setPortArsEnable(portName, true))
                    m_arsEnabledPorts.insert(portName);
                else
                    SWSS_LOG_ERROR("ARS: failed to enable ARS on port %s", portName.c_str());
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
            if (m_arsEnabledPorts.count(portName))
            {
                setPortArsEnable(portName, false);
                m_arsEnabledPorts.erase(portName);
            }
            m_arsInterfaces.erase(portName);
        }

        it = consumer.m_toSync.erase(it);
    }

    if (gRouteOrch && m_arsEnabled)
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

            for (auto &fv : kfvFieldsValues(kfv))
            {
                const string &field = fvField(fv);
                const string &value = fvValue(fv);

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
            SWSS_LOG_NOTICE("ARS: prefix %s mapped to ARS object '%s'",
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

            for (auto &fv : kfvFieldsValues(kfv))
            {
                const string &field = fvField(fv);
                const string &value = fvValue(fv);

                if      (field == "admin_state") entry.enabled = (value == "up");
                else if (field == "ars_object")  entry.arsObject = value;
                else if (field == "port_profile") entry.portProfile = value;
                else if (field == "link_utilization_threshold")
                    entry.linkUtilThreshold = static_cast<uint32_t>(stoul(value));
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
        SWSS_LOG_ERROR("ARS: bind ARS OID 0x%" PRIx64 " to NHG 0x%" PRIx64 " failed: %s",
                       arsOid, nhgOid, sai_serialize_status(status).c_str());
        return false;
    }

    SWSS_LOG_NOTICE("ARS: bound ARS OID 0x%" PRIx64 " to NHG 0x%" PRIx64, arsOid, nhgOid);

    // Mirror the positive outcome into STATE_DB's ARS_NHG_TABLE so an
    // 'active' row exists alongside the 'degraded' rows written by
    // resolveArsForNhg's failure paths. Without this, operators reading
    // ARS_NHG_TABLE only ever see failures.
    m_nhgStateKeys[nhgOid] = sai_serialize_object_id(nhgOid);
    if (arsOid == SAI_NULL_OBJECT_ID)
        removeArsNhgState(m_nhgStateKeys[nhgOid]);
    else
        writeArsNhgState(m_nhgStateKeys[nhgOid], false);
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
        SWSS_LOG_ERROR("ARS: set SAI_LAG_ATTR_ARS_OBJECT_ID on %s failed: %s",
                       lagName.c_str(), sai_serialize_status(status).c_str());
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
    return "";
}

sai_object_id_t ArsOrch::resolveArsForNhg(sai_object_id_t nhgOid, const NextHopGroupKey &nhgKey)
{
    SWSS_LOG_ENTER();

    if (!m_arsEnabled)
        return SAI_NULL_OBJECT_ID;

    string commonArsObj;
    bool mismatch = false;

    for (const auto &nh : nhgKey.getNextHops())
    {
        string portName = nh.alias;
        string arsObj = getArsObjectForPort(portName);

        if (arsObj.empty())
            continue;

        if (commonArsObj.empty())
        {
            commonArsObj = arsObj;
        }
        else if (commonArsObj != arsObj)
        {
            SWSS_LOG_WARN("ARS: NHG members have different ARS objects (%s vs %s), "
                          "falling back to plain ECMP",
                          commonArsObj.c_str(), arsObj.c_str());
            mismatch = true;
            break;
        }
    }

    if (commonArsObj.empty() && !m_nexthopArsBindings.empty())
    {
        commonArsObj = m_nexthopArsBindings.begin()->second;
    }

    if (mismatch || commonArsObj.empty())
        return SAI_NULL_OBJECT_ID;

    // Honor ARS_OBJECT.admin_state: if the resolved object is disabled we
    // return NULL so the NHG falls back to plain ECMP.
    auto objIt = m_arsObjects.find(commonArsObj);
    if (objIt == m_arsObjects.end() || !objIt->second.enabled)
    {
        if (objIt != m_arsObjects.end())
        {
            SWSS_LOG_NOTICE("ARS: object '%s' admin_state=down — NHG falls back "
                            "to plain ECMP", commonArsObj.c_str());
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

    for (const auto &lagName : m_arsEnabledLags)
        unbindArsFromLag(lagName);
    m_arsEnabledLags.clear();

    for (const auto &portName : m_arsEnabledPorts)
        setPortArsEnable(portName, false);
    m_arsEnabledPorts.clear();
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
    if (m_activeSwitchProfileOid == SAI_NULL_OBJECT_ID)
    {
        SWSS_LOG_WARN("ARS: no active ARS profile to set load bands for %s", portName.c_str());
        return false;
    }

    sai_attribute_t attr;
    auto setAttr = [&](sai_ars_profile_attr_t id, uint32_t val) {
        attr.id = id;
        attr.value.u32 = val;
        sai_status_t s = sai_ars_profile_api->set_ars_profile_attribute(
            m_activeSwitchProfileOid, &attr);
        if (s != SAI_STATUS_SUCCESS)
            SWSS_LOG_WARN("ARS: set profile load band attr %d for %s failed: %s",
                          id, portName.c_str(), sai_serialize_status(s).c_str());
    };

    if (pp.loadPastMinVal > 0 || pp.loadPastMaxVal > 0)
    {
        setAttr(SAI_ARS_PROFILE_ATTR_LOAD_PAST_MIN_VAL, pp.loadPastMinVal);
        setAttr(SAI_ARS_PROFILE_ATTR_LOAD_PAST_MAX_VAL, pp.loadPastMaxVal);
    }
    if (pp.loadFutureMinVal > 0 || pp.loadFutureMaxVal > 0)
    {
        setAttr(SAI_ARS_PROFILE_ATTR_LOAD_FUTURE_MIN_VAL, pp.loadFutureMinVal);
        setAttr(SAI_ARS_PROFILE_ATTR_LOAD_FUTURE_MAX_VAL, pp.loadFutureMaxVal);
    }
    if (pp.loadCurrentMinVal > 0 || pp.loadCurrentMaxVal > 0)
    {
        setAttr(SAI_ARS_PROFILE_ATTR_LOAD_CURRENT_MIN_VAL, pp.loadCurrentMinVal);
        setAttr(SAI_ARS_PROFILE_ATTR_LOAD_CURRENT_MAX_VAL, pp.loadCurrentMaxVal);
    }
    return true;
}

bool ArsOrch::setPortArsScalingFactor(const string &portName, const ArsPortProfileEntry &pp)
{
    Port port;
    if (!m_portsOrch->getPort(portName, port))
    {
        SWSS_LOG_ERROR("ARS: port %s not found for scaling factor", portName.c_str());
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

    sai_attribute_t attr;

    if (pastWeight > 0)
    {
        attr.id = SAI_PORT_ATTR_ARS_PORT_LOAD_PAST_WEIGHT;
        attr.value.u32 = pastWeight;
        sai_status_t status = sai_port_api->set_port_attribute(port.m_port_id, &attr);
        if (status != SAI_STATUS_SUCCESS)
            SWSS_LOG_WARN("ARS: set past weight on %s failed: %s",
                          portName.c_str(), sai_serialize_status(status).c_str());
    }

    if (futureWeight > 0)
    {
        attr.id = SAI_PORT_ATTR_ARS_PORT_LOAD_FUTURE_WEIGHT;
        attr.value.u32 = futureWeight;
        sai_status_t status = sai_port_api->set_port_attribute(port.m_port_id, &attr);
        if (status != SAI_STATUS_SUCCESS)
            SWSS_LOG_WARN("ARS: set future weight on %s failed: %s",
                          portName.c_str(), sai_serialize_status(status).c_str());
    }

    return true;
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
    auto it = arsModeLookup.find(modeStr);
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
    if (!createArsProfile(kDefaultName, entry))
    {
        SWSS_LOG_WARN("ARS: failed to auto-create default profile — "
                      "SAI will use built-in defaults");
        return;
    }

    m_globalProfileName = kDefaultName;
    SWSS_LOG_NOTICE("ARS: auto-created default profile '%s' with SDK defaults",
                    kDefaultName.c_str());
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
