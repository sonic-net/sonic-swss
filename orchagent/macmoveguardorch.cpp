#include "macmoveguardorch.h"
#include "fdborch.h"
#include "logger.h"
#include "notifier.h"
#include "switchorch.h"
#include "crmorch.h"

#include <chrono>
#include <inttypes.h>
#include <sstream>
#include <iomanip>

extern sai_fdb_api_t    *sai_fdb_api;
extern sai_acl_api_t    *sai_acl_api;
extern sai_switch_api_t *sai_switch_api;
extern sai_object_id_t gSwitchId;
extern SwitchOrch    *gSwitchOrch;
extern CrmOrch       *gCrmOrch;

using namespace std;
using namespace std::chrono;

const int MacMoveGuardOrch::RECOVERY_CHECK_INTERVAL_SECS;

MacMoveGuardOrch::MacMoveGuardOrch(DBConnector *configDb, DBConnector *stateDb,
                                   const string &tableName,
                                   PortsOrch *portsOrch, FdbOrch *fdbOrch) :
    Orch(configDb, tableName),
    m_portsOrch(portsOrch),
    m_fdbOrch(fdbOrch)
{
    SWSS_LOG_ENTER();

    // Subscribe to FDB changes from FdbOrch to receive MAC move notifications
    if (m_fdbOrch)
    {
        m_fdbOrch->attach(this);
    }

    // STATE_DB table for persisting bad-MAC entries across orchagent restarts
    if (stateDb)
    {
        m_stateBadMacTable.reset(new swss::Table(stateDb, STATE_MAC_MOVE_GUARD_TABLE_NAME));
        restoreBadMacState();
    }

    // Periodic recovery check timer
    m_recoveryTimer = new swss::SelectableTimer(
        timespec{ .tv_sec = RECOVERY_CHECK_INTERVAL_SECS, .tv_nsec = 0 });
    auto recoveryExecutor = new ExecutableTimer(m_recoveryTimer, this, "MAC_MOVE_GUARD_RECOVERY");
    Orch::addExecutor(recoveryExecutor);
    m_recoveryTimer->start();

    SWSS_LOG_NOTICE("MacMoveGuardOrch initialized");
}

MacMoveGuardOrch::~MacMoveGuardOrch()
{
    if (m_fdbOrch)
    {
        m_fdbOrch->detach(this);
    }
}

void MacMoveGuardOrch::clearAllState()
{
    // Re-enable any ports we have administratively disabled (DISABLE_PORT action)
    for (auto &kv : m_disabledPorts)
    {
        if (m_portsOrch)
        {
            m_portsOrch->setPortAdminStatusByAlias(kv.first, true);
            SWSS_LOG_NOTICE("MAC_MOVE_GUARD: re-enabling port %s (feature disabled)", kv.first.c_str());
        }
    }

    // Remove any ACL entries we programmed (DISABLE_LEARN_ON_MAC_WITH_ACL action) and
    // drop every persisted bad-MAC row regardless of action.
    for (auto &kv : m_macTrackingState)
    {
        const MacKey &key = kv.first;
        MacMoveTrackingState &state = kv.second;

        if (state.learn_disable_acl_entry_id != SAI_NULL_OBJECT_ID)
        {
            removeLearnDisableAclEntry(state);
        }

        removeBadMacFromState(key);
    }

    // Tear down the shared learn-disable ACL table; all entries have been removed above.
    destroyLearnDisableAclTable();

    // Clear all tracking state when feature is disabled
    m_disabledPorts.clear();
    m_macTrackingState.clear();
    m_learntMac.clear();
}

void MacMoveGuardOrch::doTask(Consumer &consumer)
{
    SWSS_LOG_ENTER();

    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
        KeyOpFieldsValuesTuple t = it->second;
        string key = kfvKey(t);
        string op = kfvOp(t);

        if (key != "GLOBAL")
        {
            SWSS_LOG_WARN("MAC_MOVE_GUARD: unsupported key %s, only GLOBAL is supported",
                          key.c_str());
            it = consumer.m_toSync.erase(it);
            continue;
        }

        if (op == SET_COMMAND)
        {
            // Snapshot before parsing so we can detect transitions in
            // (m_enabled, m_action) and reconcile the ACL table lifecycle.
            bool prev_enabled              = m_enabled;
            MacMoveGuardAction prev_action = m_action;
            uint32_t prev_recovery_seconds = m_recoverySeconds;

            for (auto i : kfvFieldsValues(t))
            {
                const string &field = fvField(i);
                const string &value = fvValue(i);

                try
                {
                    if (field == "enabled")
                    {
                        m_enabled = (value == "true");
                    }
                    else if (field == "threshold")
                    {
                        m_threshold = static_cast<uint32_t>(stoul(value));
                    }
                    else if (field == "detect_interval")
                    {
                        m_durationSeconds = static_cast<uint32_t>(stoul(value));
                    }
                    else if (field == "action_interval")
                    {
                        m_recoverySeconds = static_cast<uint32_t>(stoul(value));
                    }
                    else if (field == "action")
                    {
                        if (value == "DISABLE_PORT")
                        {
                            m_action = MacMoveGuardAction::DISABLE_PORT;
                        }
                        else if (value == "DISABLE_LEARN_ON_MAC_WITH_ACL")
                        {
                            m_action = MacMoveGuardAction::DISABLE_LEARN_ON_MAC_WITH_ACL;
                        }
                        else
                        {
                            SWSS_LOG_WARN("MAC_MOVE_GUARD: unsupported action '%s', using DISABLE_PORT",
                                         value.c_str());
                            m_action = MacMoveGuardAction::DISABLE_PORT;
                        }
                    }
                }
                catch (const exception &e)
                {
                    SWSS_LOG_ERROR("MAC_MOVE_GUARD: invalid value '%s' for field '%s': %s",
                                   value.c_str(), field.c_str(), e.what());
                }
            }

            const char* action_str = "UNKNOWN";
            if (m_action == MacMoveGuardAction::DISABLE_PORT)
                action_str = "DISABLE_PORT";
            else if (m_action == MacMoveGuardAction::DISABLE_LEARN_ON_MAC_WITH_ACL)
                action_str = "DISABLE_LEARN_ON_MAC_WITH_ACL";

            SWSS_LOG_NOTICE("MAC_MOVE_GUARD config: enabled=%s threshold=%u detect_interval=%us action_interval=%us action=%s",
                            m_enabled ? "true" : "false",
                            m_threshold, m_durationSeconds, m_recoverySeconds,
                            action_str);

            if (!m_enabled)
            {
                clearAllState();
            }
            else
            {
                // Reconcile the ACL table lifecycle to match the new config.
                reconcileLearnDisableAclTable(prev_enabled, prev_action);

                // If action_interval changed, re-evaluate expiry of
                // already-tracked bad MACs so a shorter interval takes
                // effect promptly instead of waiting for the original
                // expiry computed at markBadMac time.
                if (m_recoverySeconds != prev_recovery_seconds)
                {
                    reapplyActionIntervalToBadMacs(prev_recovery_seconds);
                }
            }
        }
        else if (op == DEL_COMMAND)
        {
            m_enabled = false;
            clearAllState();
            SWSS_LOG_NOTICE("MAC_MOVE_GUARD config deleted; feature disabled");
        }

        it = consumer.m_toSync.erase(it);
    }
}

void MacMoveGuardOrch::doTask(swss::SelectableTimer &timer)
{
    SWSS_LOG_ENTER();

    if (!m_enabled)
    {
        return;
    }

    checkRecovery();
}

void MacMoveGuardOrch::update(SubjectType type, void *cntx)
{
    SWSS_LOG_ENTER();

    if (!m_enabled || !cntx)
    {
        return;
    }

    switch (type)
    {
    case SUBJECT_TYPE_MAC_MOVE:
        handleMacMove(*static_cast<MacMoveNotification *>(cntx));
        break;
    case SUBJECT_TYPE_MAC_LEARN:
        handleMacLearn(*static_cast<MacLearnNotification *>(cntx));
        break;
    default:
        break;
    }
}

void MacMoveGuardOrch::pruneWindow(MacMoveTrackingState &state)
{
    auto cutoff = steady_clock::now() - seconds(m_durationSeconds);

    // Remove old move timestamps outside the detection window
    while (!state.move_timestamps.empty() && state.move_timestamps.front() < cutoff)
    {
        state.move_timestamps.pop_front();
    }

    // Remove ports that haven't been seen within the detection window
    for (auto it = state.ports_seen.begin(); it != state.ports_seen.end(); )
    {
        if (it->second < cutoff)
        {
            it = state.ports_seen.erase(it);
        }
        else
        {
            ++it;
        }
    }
}

void MacMoveGuardOrch::handleMacLearn(const MacLearnNotification &notif)
{
    SWSS_LOG_ENTER();

    if (notif.port.m_alias.empty())
    {
        return;
    }

    MacKey key{ notif.mac, notif.bv_id };

    // Look up the previously-known port for this (mac, bv_id), then update
    // the cache to the newly-learned port and refresh its timestamp. We never
    // erase on AGED — a previously-learned port stays cached until the next
    // LEARN overwrites it (the signal we use to detect a move) or until
    // checkRecovery() prunes it for being older than detect_interval.
    auto it = m_learntMac.find(key);
    std::string prev_port;
    if (it != m_learntMac.end())
    {
        prev_port = it->second.port;
    }
    m_learntMac[key] = LearntMacEntry{ notif.port.m_alias, steady_clock::now() };

    if (prev_port.empty() || prev_port == notif.port.m_alias)
    {
        // First time we see this MAC, or same port — not a move.
        return;
    }

    // It's a move: synthesize a MOVE notification and run it through the
    // same path used for native SAI_FDB_EVENT_MOVE.
    MacMoveNotification synth;
    synth.port_old.m_alias = prev_port;
    synth.port_new = notif.port;
    synth.mac = notif.mac;
    synth.bv_id = notif.bv_id;
    handleMacMove(synth);
}

void MacMoveGuardOrch::handleMacMove(const MacMoveNotification &notif)
{
    SWSS_LOG_ENTER();

    const string &new_alias = notif.port_new.m_alias;
    if (new_alias.empty())
    {
        return;
    }

    MacKey key{ notif.mac, notif.bv_id };

    auto now = steady_clock::now();

    // Keep the learnt-MAC cache in sync for native MOVE events. (For moves
    // synthesized from LEARN, handleMacLearn has already updated this.)
    m_learntMac[key] = LearntMacEntry{ new_alias, now };

    MacMoveTrackingState &state = m_macTrackingState[key];

    // Track this move timestamp in the sliding window
    state.move_timestamps.push_back(now);

    // Track this port and when MAC was last seen on it
    state.ports_seen[new_alias] = now;
    state.last_port = new_alias;

    // Prune old entries outside the detection window
    pruneWindow(state);

    // Update move count based on entries still in the window
    state.move_count = state.move_timestamps.size();

    SWSS_LOG_DEBUG("MAC_MOVE_GUARD: MAC %s on vlan_oid=0x%" PRIx64 " move count: %zu, seen on %zu ports (threshold %u)",
                   notif.mac.to_string().c_str(), notif.bv_id,
                   state.move_count, state.ports_seen.size(), m_threshold);

    if (state.move_count >= m_threshold)
    {
        // Threshold exceeded: mark as bad MAC
        if (state.is_bad_mac)
        {
            // Already a bad MAC - extend the action interval
            SWSS_LOG_NOTICE("MAC_MOVE_GUARD: BAD MAC %s on vlan_oid=0x%" PRIx64
                           " continues to move (port %s), extending action interval by %us",
                           notif.mac.to_string().c_str(), notif.bv_id, new_alias.c_str(),
                           m_recoverySeconds);
        }
        else
        {
            // First time exceeding threshold - mark as bad MAC
            SWSS_LOG_NOTICE("MAC_MOVE_GUARD: BAD MAC detected: %s on vlan_oid=0x%" PRIx64
                           ", threshold %u exceeded with %zu moves seen on %zu ports in %us",
                           notif.mac.to_string().c_str(), notif.bv_id,
                           m_threshold, state.move_count, state.ports_seen.size(), m_durationSeconds);
        }

        markBadMac(key, state, new_alias);
    }
}

void MacMoveGuardOrch::markBadMac(const MacKey &key, MacMoveTrackingState &state, const string &portName)
{
    SWSS_LOG_ENTER();

    // Mark as bad MAC and set/extend action expiry time. The action is
    // captured per-MAC so that subsequent cleanup uses the action that was
    // in effect at marking time, even if config changes later.
    state.is_bad_mac = true;
    state.action = m_action;
    state.action_expiry_time = steady_clock::now() + seconds(m_recoverySeconds);

    // Execute configured action
    switch (m_action)
    {
        case MacMoveGuardAction::DISABLE_PORT:
        {
            if (!m_portsOrch)
            {
                SWSS_LOG_ERROR("MAC_MOVE_GUARD: PortsOrch not available");
                return;
            }

            // Pin MAC to ONE port (keep it active), disable ALL other ports it appeared on
            // Strategy: Minimize total disabled ports across all bad MACs by reusing already-disabled ports

            // If not already pinned, select the pinned port intelligently
            if (state.pinned_port.empty())
            {
                // Goal: Minimize total disabled ports
                // Strategy:
                // 1. If there's a port already disabled by another bad MAC, keep it disabled (don't pin to it)
                // 2. Pin to a port that is NOT yet disabled (keeps more ports UP overall)
                // 3. If all ports already disabled, pick any (rare edge case)

                string pinned_candidate;
                string already_disabled_candidate;

                for (const auto &port_entry : state.ports_seen)
                {
                    const string &port = port_entry.first;

                    // Check if this port is already disabled by another bad MAC
                    if (m_disabledPorts.find(port) != m_disabledPorts.end())
                    {
                        // This port is already disabled - save as candidate but don't prefer it for pinning
                        already_disabled_candidate = port;
                    }
                    else
                    {
                        // This port is NOT disabled - prefer it for pinning!
                        pinned_candidate = port;
                        break;  // Found a good candidate, use it
                    }
                }

                // Select pinned port based on what we found
                if (!pinned_candidate.empty())
                {
                    // Best case: Found a port that's not yet disabled - pin to it
                    state.pinned_port = pinned_candidate;
                }
                else if (!already_disabled_candidate.empty())
                {
                    // All ports are already disabled - pin to one of them
                    state.pinned_port = already_disabled_candidate;
                    SWSS_LOG_WARN("MAC_MOVE_GUARD: All ports for bad MAC %s are already disabled by other bad MACs, "
                                 "pinning to %s",
                                 key.mac.to_string().c_str(), state.pinned_port.c_str());
                }
                else
                {
                    // Fallback: Use current port (should not normally happen)
                    state.pinned_port = portName;
                }

                SWSS_LOG_NOTICE("MAC_MOVE_GUARD: Pinning bad MAC %s to port %s (keeping this port UP)",
                               key.mac.to_string().c_str(), state.pinned_port.c_str());
            }

            // Disable all ports EXCEPT the pinned port
            for (const auto &port_entry : state.ports_seen)
            {
                const string &port = port_entry.first;

                if (port == state.pinned_port)
                {
                    // Skip the pinned port - keep it UP
                    continue;
                }

                // Check if this port is already disabled
                if (state.disabled_ports.count(port) > 0)
                {
                    // Already disabled by this bad MAC
                    continue;
                }

                // Disable this port
                auto &bad_macs_on_port = m_disabledPorts[port];
                bool first_bad_mac = bad_macs_on_port.empty();

                if (first_bad_mac)
                {
                    // First bad MAC on this port - actually disable it
                    if (!m_portsOrch->setPortAdminStatusByAlias(port, false))
                    {
                        SWSS_LOG_ERROR("MAC_MOVE_GUARD: failed to disable port %s", port.c_str());
                        m_disabledPorts.erase(port);
                        continue;
                    }
                    SWSS_LOG_NOTICE("MAC_MOVE_GUARD: port %s administratively disabled (bad MAC %s pinned to %s)",
                                   port.c_str(), key.mac.to_string().c_str(), state.pinned_port.c_str());
                }

                // Track this port as disabled by this bad MAC
                bad_macs_on_port.insert(key);
                state.disabled_ports.insert(port);
            }

            // Persist so disabled ports can be re-enabled after an orchagent restart
            persistBadMac(key, state);
            break;
        }

        case MacMoveGuardAction::DISABLE_LEARN_ON_MAC_WITH_ACL:
        {
            // Install a pre-ingress ACL entry (vlan, smac) -> SET_DO_NOT_LEARN
            // so the source MAC is not re-learned while forwarding continues
            // via the normal FDB lookup.
            if (state.learn_disable_acl_entry_id != SAI_NULL_OBJECT_ID)
            {
                // Entry already installed for this bad MAC
                break;
            }

            // Soft-disable path: action is configured but unsupported on this
            // platform, or the table was not created (capability check failed
            // at config time). Mark the MAC as bad for tracking but skip ASIC
            // programming.
            if (m_learnDisableAclTable == SAI_NULL_OBJECT_ID)
            {
                persistBadMac(key, state);
                break;
            }

            if (!installLearnDisableAclEntry(key, state))
            {
                SWSS_LOG_ERROR("MAC_MOVE_GUARD: Failed to install learn-disable ACL entry for MAC %s",
                              key.mac.to_string().c_str());
                return;
            }

            // Persist so the ACL entry can be cleaned up after an orchagent restart
            persistBadMac(key, state);
            break;
        }

        // Future actions can be added here as new case statements
        default:
            SWSS_LOG_WARN("MAC_MOVE_GUARD: unknown action type, no action taken");
            break;
    }
}

void MacMoveGuardOrch::releaseBadMac(const MacKey &key, MacMoveTrackingState &state)
{
    SWSS_LOG_ENTER();

    if (!state.is_bad_mac)
    {
        return;
    }

    // Release MAC from bad MAC tracking
    state.is_bad_mac = false;

    // Use the action that was in effect when this MAC was marked bad so
    // cleanup matches what was actually programmed, regardless of any
    // later config change to m_action.
    switch (state.action)
    {
        case MacMoveGuardAction::DISABLE_PORT:
        {
            // Re-enable all ports that were disabled for this bad MAC
            for (const string &port : state.disabled_ports)
            {
                auto port_it = m_disabledPorts.find(port);
                if (port_it != m_disabledPorts.end())
                {
                    // Remove this bad MAC from the port's tracking
                    port_it->second.erase(key);

                    // If no other bad MACs require this port to be disabled, re-enable it
                    if (port_it->second.empty())
                    {
                        if (m_portsOrch && !m_portsOrch->setPortAdminStatusByAlias(port, true))
                        {
                            SWSS_LOG_ERROR("MAC_MOVE_GUARD: failed to re-enable port %s", port.c_str());
                        }
                        else
                        {
                            SWSS_LOG_NOTICE("MAC_MOVE_GUARD: port %s administratively re-enabled "
                                           "(bad MAC %s released, no other bad MACs require it disabled)",
                                           port.c_str(), key.mac.to_string().c_str());
                        }
                        m_disabledPorts.erase(port_it);
                    }
                    else
                    {
                        SWSS_LOG_INFO("MAC_MOVE_GUARD: port %s remains disabled (%zu other bad MACs still require it)",
                                     port.c_str(), port_it->second.size());
                    }
                }
            }

            // Clear the disabled ports list and pinned port
            state.disabled_ports.clear();
            state.pinned_port.clear();

            // Drop the persisted entry; it is no longer needed for restart recovery
            removeBadMacFromState(key);
            break;
        }

        case MacMoveGuardAction::DISABLE_LEARN_ON_MAC_WITH_ACL:
        {
            // Entry id may be NULL if we were on the soft-disable path (no ACL
            // table); removeLearnDisableAclEntry() is a no-op in that case.
            if (state.learn_disable_acl_entry_id != SAI_NULL_OBJECT_ID)
            {
                removeLearnDisableAclEntry(state);
            }
            removeBadMacFromState(key);
            break;
        }

        // Future actions can be added here as new case statements
        default:
            break;
    }
}

void MacMoveGuardOrch::pruneLearntMacCache()
{
    // Drop cached learnt-MAC entries that have not been seen within
    // detect_interval. This bounds memory use for MACs that have gone quiet
    // without losing any information that the sliding-window detector would
    // have acted on (anything older than detect_interval is outside the
    // window anyway).
    auto cutoff = steady_clock::now() - seconds(m_durationSeconds);

    for (auto it = m_learntMac.begin(); it != m_learntMac.end(); )
    {
        if (it->second.last_seen < cutoff)
        {
            it = m_learntMac.erase(it);
        }
        else
        {
            ++it;
        }
    }
}

void MacMoveGuardOrch::checkRecovery()
{
    SWSS_LOG_ENTER();

    auto now = steady_clock::now();

    // Bound memory growth of the learnt-MAC cache
    pruneLearntMacCache();

    for (auto it = m_macTrackingState.begin(); it != m_macTrackingState.end(); )
    {
        MacMoveTrackingState &state = it->second;
        const MacKey &key = it->first;

        // Always prune detection window so it stays correct
        pruneWindow(state);

        if (!state.is_bad_mac)
        {
            // If the MAC has gone quiet (no ports in detection window) and not a bad MAC,
            // drop the entry to avoid unbounded memory growth
            if (state.ports_seen.empty())
            {
                it = m_macTrackingState.erase(it);
                continue;
            }
            ++it;
            continue;
        }

        // Check if action_interval has expired for this bad MAC
        if (now >= state.action_expiry_time)
        {
            SWSS_LOG_NOTICE("MAC_MOVE_GUARD: MAC %s on vlan_oid=0x%" PRIx64
                           " (pinned to %s) exiting bad MAC phase after action interval",
                           key.mac.to_string().c_str(), key.bv_id, state.pinned_port.c_str());

            releaseBadMac(key, state);
        }

        ++it;
    }
}

void MacMoveGuardOrch::reapplyActionIntervalToBadMacs(uint32_t prev_recovery_seconds)
{
    SWSS_LOG_ENTER();

    auto now = steady_clock::now();
    auto new_expiry = now + seconds(m_recoverySeconds);
    size_t shortened = 0;
    size_t extended  = 0;

    for (auto &kv : m_macTrackingState)
    {
        MacMoveTrackingState &state = kv.second;
        if (!state.is_bad_mac)
        {
            continue;
        }

        // Shortening the interval: cap the existing expiry so the new
        // (smaller) action_interval takes effect on entries that were
        // marked under the previous, longer interval.
        if (new_expiry < state.action_expiry_time)
        {
            state.action_expiry_time = new_expiry;
            persistBadMac(kv.first, state);
            ++shortened;
        }
        // Lengthening the interval: extend entries whose current
        // remaining time is shorter than the new interval, so a longer
        // action_interval also takes effect immediately.
        else if (new_expiry > state.action_expiry_time)
        {
            state.action_expiry_time = new_expiry;
            persistBadMac(kv.first, state);
            ++extended;
        }
    }

    if (shortened || extended)
    {
        SWSS_LOG_NOTICE("MAC_MOVE_GUARD: action_interval changed %us -> %us; "
                        "re-evaluated bad MAC expiry (shortened=%zu extended=%zu)",
                        prev_recovery_seconds, m_recoverySeconds,
                        shortened, extended);
    }
}


// STATE_DB key format: "<bv_id_hex>:<mac>" — bv_id is the SAI bridge VLAN OID.
// Using bv_id (instead of vlan-id) keeps restoration independent of PortsOrch
// having finished initialization.
std::string MacMoveGuardOrch::makeBadMacStateKey(const MacKey &key) const
{
    std::ostringstream oss;
    oss << "0x" << std::hex << key.bv_id << ":" << key.mac.to_string();
    return oss.str();
}

// String <-> MacMoveGuardAction helpers used for STATE_DB persistence.
static const char *actionToString(MacMoveGuardAction a)
{
    switch (a)
    {
        case MacMoveGuardAction::DISABLE_PORT:                  return "DISABLE_PORT";
        case MacMoveGuardAction::DISABLE_LEARN_ON_MAC_WITH_ACL: return "DISABLE_LEARN_ON_MAC_WITH_ACL";
    }
    return "DISABLE_PORT";
}

static bool stringToAction(const std::string &s, MacMoveGuardAction &out)
{
    if      (s == "DISABLE_PORT")                  { out = MacMoveGuardAction::DISABLE_PORT;                  return true; }
    else if (s == "DISABLE_LEARN_ON_MAC_WITH_ACL") { out = MacMoveGuardAction::DISABLE_LEARN_ON_MAC_WITH_ACL; return true; }
    return false;
}

void MacMoveGuardOrch::persistBadMac(const MacKey &key, const MacMoveTrackingState &state)
{
    if (!m_stateBadMacTable)
    {
        return;
    }

    // Persist the absolute wall-clock expiry so the remaining action interval
    // can be reconstructed after an orchagent restart.
    auto wall_now    = system_clock::now();
    auto steady_now  = steady_clock::now();
    auto remaining   = duration_cast<seconds>(state.action_expiry_time - steady_now).count();
    if (remaining < 0) remaining = 0;
    auto expiry_epoch = duration_cast<seconds>(wall_now.time_since_epoch()).count() + remaining;

    std::vector<FieldValueTuple> fvs = {
        FieldValueTuple("action",              actionToString(state.action)),
        FieldValueTuple("action_expiry_epoch", std::to_string(expiry_epoch)),
    };

    switch (state.action)
    {
        case MacMoveGuardAction::DISABLE_PORT:
        {
            fvs.emplace_back("pinned_port", state.pinned_port);
            // Persist disabled ports as a comma-separated list
            std::string joined;
            for (const auto &p : state.disabled_ports)
            {
                if (!joined.empty()) joined += ",";
                joined += p;
            }
            fvs.emplace_back("disabled_ports", joined);
            break;
        }
        case MacMoveGuardAction::DISABLE_LEARN_ON_MAC_WITH_ACL:
        {
            std::ostringstream e_hex, t_hex;
            e_hex << "0x" << std::hex << state.learn_disable_acl_entry_id;
            t_hex << "0x" << std::hex << m_learnDisableAclTable;
            // The shared table OID is stored redundantly on every entry so
            // the table can be reattached on restart without an extra DB row.
            fvs.emplace_back("acl_entry_id", e_hex.str());
            fvs.emplace_back("acl_table_id", t_hex.str());
            break;
        }
    }

    m_stateBadMacTable->set(makeBadMacStateKey(key), fvs);
}

void MacMoveGuardOrch::removeBadMacFromState(const MacKey &key)
{
    if (!m_stateBadMacTable)
    {
        return;
    }
    m_stateBadMacTable->del(makeBadMacStateKey(key));
}

void MacMoveGuardOrch::restoreBadMacState()
{
    SWSS_LOG_ENTER();

    if (!m_stateBadMacTable)
    {
        return;
    }

    std::vector<std::string> keys;
    m_stateBadMacTable->getKeys(keys);

    if (keys.empty())
    {
        return;
    }

    auto wall_now_epoch = duration_cast<seconds>(system_clock::now().time_since_epoch()).count();

    for (const auto &state_key : keys)
    {
        // Parse "<bv_id_hex>:<mac>"
        auto colon = state_key.find(':');
        if (colon == std::string::npos)
        {
            SWSS_LOG_WARN("MAC_MOVE_GUARD: malformed bad-MAC state key '%s', skipping", state_key.c_str());
            m_stateBadMacTable->del(state_key);
            continue;
        }

        sai_object_id_t bv_id = SAI_NULL_OBJECT_ID;
        try
        {
            bv_id = static_cast<sai_object_id_t>(std::stoull(state_key.substr(0, colon), nullptr, 16));
        }
        catch (const std::exception &e)
        {
            SWSS_LOG_WARN("MAC_MOVE_GUARD: invalid bv_id in state key '%s': %s", state_key.c_str(), e.what());
            m_stateBadMacTable->del(state_key);
            continue;
        }

        MacAddress mac;
        try
        {
            mac = MacAddress(state_key.substr(colon + 1));
        }
        catch (const std::exception &e)
        {
            SWSS_LOG_WARN("MAC_MOVE_GUARD: invalid MAC in state key '%s': %s", state_key.c_str(), e.what());
            m_stateBadMacTable->del(state_key);
            continue;
        }

        MacKey key{ mac, bv_id };

        std::vector<FieldValueTuple> fvs;
        m_stateBadMacTable->get(state_key, fvs);

        // Common + action-specific persisted fields
        MacMoveGuardAction action  = MacMoveGuardAction::DISABLE_PORT;
        bool   action_seen         = false;
        long   expiry_epoch        = wall_now_epoch;
        sai_object_id_t acl_entry_id        = SAI_NULL_OBJECT_ID;
        sai_object_id_t acl_table_id        = SAI_NULL_OBJECT_ID;
        std::string pinned_port;
        std::string disabled_ports_csv;

        auto parseOid = [](const std::string &s) -> sai_object_id_t {
            try { return static_cast<sai_object_id_t>(std::stoull(s, nullptr, 16)); }
            catch (const std::exception &) { return SAI_NULL_OBJECT_ID; }
        };

        for (const auto &fv : fvs)
        {
            const auto &f = fvField(fv);
            const auto &v = fvValue(fv);
            if      (f == "action")              { action_seen = stringToAction(v, action); }
            else if (f == "action_expiry_epoch") { try { expiry_epoch = std::stol(v); } catch (const std::exception &) {} }
            else if (f == "pinned_port")         { pinned_port      = v; }
            else if (f == "disabled_ports")      { disabled_ports_csv = v; }
            else if (f == "acl_entry_id")        { acl_entry_id     = parseOid(v); }
            else if (f == "acl_table_id")        { acl_table_id     = parseOid(v); }
        }

        if (!action_seen)
        {
            SWSS_LOG_WARN("MAC_MOVE_GUARD: missing/invalid action in state key '%s', dropping",
                          state_key.c_str());
            m_stateBadMacTable->del(state_key);
            continue;
        }

        // Remaining action interval; clamp to 0 so the next checkRecovery()
        // tears down the programmed hardware state immediately.
        long remaining = expiry_epoch - wall_now_epoch;
        if (remaining < 0) remaining = 0;

        MacMoveTrackingState &state = m_macTrackingState[key];
        state.is_bad_mac          = true;
        state.action              = action;
        state.action_expiry_time  = steady_clock::now() + seconds(remaining);

        switch (action)
        {
            case MacMoveGuardAction::DISABLE_PORT:
            {
                state.pinned_port = pinned_port;
                // Split comma-separated disabled-ports list and rebuild
                // m_disabledPorts so per-port reference counts are preserved
                // across the restart. We do NOT re-issue setPortAdminStatus
                // here because the SAI admin state survives the restart.
                size_t start = 0;
                while (start < disabled_ports_csv.size())
                {
                    size_t comma = disabled_ports_csv.find(',', start);
                    std::string p = disabled_ports_csv.substr(
                        start, comma == std::string::npos ? std::string::npos : comma - start);
                    if (!p.empty())
                    {
                        state.disabled_ports.insert(p);
                        m_disabledPorts[p].insert(key);
                    }
                    if (comma == std::string::npos) break;
                    start = comma + 1;
                }
                break;
            }
            case MacMoveGuardAction::DISABLE_LEARN_ON_MAC_WITH_ACL:
            {
                state.learn_disable_acl_entry_id = acl_entry_id;
                // Adopt the previously created shared ACL table. All entries
                // should reference the same table OID; warn if not. The
                // switch-binding survives the restart on the SAI side, so we
                // do not re-issue the SAI_SWITCH_ATTR_PRE_INGRESS_ACL bind here.
                if (m_learnDisableAclTable == SAI_NULL_OBJECT_ID)
                {
                    m_learnDisableAclTable = acl_table_id;
                }
                else if (m_learnDisableAclTable != acl_table_id)
                {
                    SWSS_LOG_WARN("MAC_MOVE_GUARD: inconsistent acl_table_id in restored state for %s "
                                  "(0x%" PRIx64 " vs 0x%" PRIx64 ")",
                                  mac.to_string().c_str(), m_learnDisableAclTable, acl_table_id);
                }
                if (acl_entry_id != SAI_NULL_OBJECT_ID)
                {
                    m_learnDisableAclEntryCount++;
                }
                break;
            }
        }

        SWSS_LOG_NOTICE("MAC_MOVE_GUARD: restored bad-MAC state for %s on bv_id=0x%" PRIx64
                       " action=%s (action_interval remaining: %lds)",
                       mac.to_string().c_str(), bv_id, actionToString(action), remaining);
    }
}

// Query the platform once for SAI_ACL_ACTION_TYPE_SET_DO_NOT_LEARN support at
// the PRE_INGRESS stage and cache the result. Returns true if the action is
// in the platform's pre-ingress capability list. 
bool MacMoveGuardOrch::isAclSetDoNotLearnSupported()
{
    SWSS_LOG_ENTER();

    if (m_aclSetDoNotLearnSupported != -1)
    {
        return m_aclSetDoNotLearnSupported == 1;
    }

    sai_attribute_t max_attr;
    max_attr.id = SAI_SWITCH_ATTR_MAX_ACL_ACTION_COUNT;
    sai_status_t status = sai_switch_api->get_switch_attribute(gSwitchId, 1, &max_attr);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_WARN("MAC_MOVE_GUARD: cannot query SAI_SWITCH_ATTR_MAX_ACL_ACTION_COUNT "
                      "(rv:%d); assuming SAI_ACL_ACTION_TYPE_SET_DO_NOT_LEARN is unsupported "
                      "on this platform and soft-disabling DISABLE_LEARN_ON_MAC_WITH_ACL", status);
        m_aclSetDoNotLearnSupported = 0;
        return false;
    }

    std::vector<int32_t> action_list(max_attr.value.u32, 0);
    sai_attribute_t cap_attr;
    cap_attr.id = SAI_SWITCH_ATTR_ACL_STAGE_PRE_INGRESS;
    cap_attr.value.aclcapability.action_list.list = action_list.data();
    cap_attr.value.aclcapability.action_list.count = max_attr.value.u32;

    status = sai_switch_api->get_switch_attribute(gSwitchId, 1, &cap_attr);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_WARN("MAC_MOVE_GUARD: cannot query SAI_SWITCH_ATTR_ACL_STAGE_PRE_INGRESS "
                      "capabilities (rv:%d); assuming SAI_ACL_ACTION_TYPE_SET_DO_NOT_LEARN "
                      "is unsupported and soft-disabling DISABLE_LEARN_ON_MAC_WITH_ACL", status);
        m_aclSetDoNotLearnSupported = 0;
        return false;
    }

    const auto &list = cap_attr.value.aclcapability.action_list;
    for (uint32_t i = 0; i < list.count; ++i)
    {
        if (list.list[i] == SAI_ACL_ACTION_TYPE_SET_DO_NOT_LEARN)
        {
            SWSS_LOG_NOTICE("MAC_MOVE_GUARD: SAI_ACL_ACTION_TYPE_SET_DO_NOT_LEARN is "
                            "supported at the PRE_INGRESS stage; DISABLE_LEARN_ON_MAC_WITH_ACL is active");
            m_aclSetDoNotLearnSupported = 1;
            return true;
        }
    }

    SWSS_LOG_NOTICE("MAC_MOVE_GUARD: SAI_ACL_ACTION_TYPE_SET_DO_NOT_LEARN not in the "
                    "PRE_INGRESS action capability list (count=%u); soft-disabling "
                    "DISABLE_LEARN_ON_MAC_WITH_ACL. Config is accepted but no ACL table will be "
                    "created and no per-MAC entries will be installed", list.count);
    m_aclSetDoNotLearnSupported = 0;
    return false;
}

// Create the shared pre-ingress ACL table used by DISABLE_LEARN_ON_MAC_WITH_ACL and
// bind it directly to the switch pre-ingress slot (SAI_SWITCH_ATTR_PRE_INGRESS_ACL).
// Idempotent.
bool MacMoveGuardOrch::ensureLearnDisableAclTable()
{
    SWSS_LOG_ENTER();

    if (m_learnDisableAclTable != SAI_NULL_OBJECT_ID)
    {
        return true;
    }

    if (!isAclSetDoNotLearnSupported())
    {
        return false;
    }

    // Refuse to displace another feature that has already written into
    // SAI_SWITCH_ATTR_PRE_INGRESS_ACL. The attribute holds a single OID,
    // so silently overwriting it would break the other feature.
    sai_attribute_t pre_ingress_acl_attr;
    pre_ingress_acl_attr.id = SAI_SWITCH_ATTR_PRE_INGRESS_ACL;
    pre_ingress_acl_attr.value.oid = SAI_NULL_OBJECT_ID;
    sai_status_t status = sai_switch_api->get_switch_attribute(gSwitchId, 1, &pre_ingress_acl_attr);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("MAC_MOVE_GUARD: Failed to read SAI_SWITCH_ATTR_PRE_INGRESS_ACL, rv:%d", status);
        return false;
    }
    if (pre_ingress_acl_attr.value.oid != SAI_NULL_OBJECT_ID)
    {
        SWSS_LOG_ERROR("MAC_MOVE_GUARD: SAI_SWITCH_ATTR_PRE_INGRESS_ACL already bound to 0x%" PRIx64
                      " by another feature; DISABLE_LEARN_ON_MAC_WITH_ACL cannot coexist",
                      pre_ingress_acl_attr.value.oid);
        return false;
    }

    std::vector<sai_attribute_t> attrs;
    sai_attribute_t a;

    a.id = SAI_ACL_TABLE_ATTR_ACL_STAGE;
    a.value.s32 = SAI_ACL_STAGE_PRE_INGRESS;
    attrs.push_back(a);

    std::vector<int32_t> bpoint_list = { SAI_ACL_BIND_POINT_TYPE_SWITCH };
    a.id = SAI_ACL_TABLE_ATTR_ACL_BIND_POINT_TYPE_LIST;
    a.value.s32list.count = (uint32_t)bpoint_list.size();
    a.value.s32list.list  = bpoint_list.data();
    attrs.push_back(a);

    a.id = SAI_ACL_TABLE_ATTR_FIELD_SRC_MAC;
    a.value.booldata = true;
    attrs.push_back(a);

    a.id = SAI_ACL_TABLE_ATTR_FIELD_OUTER_VLAN_ID;
    a.value.booldata = true;
    attrs.push_back(a);

    std::vector<int32_t> action_list = { SAI_ACL_ACTION_TYPE_SET_DO_NOT_LEARN };
    a.id = SAI_ACL_TABLE_ATTR_ACL_ACTION_TYPE_LIST;
    a.value.s32list.count = (uint32_t)action_list.size();
    a.value.s32list.list  = action_list.data();
    attrs.push_back(a);

    status = sai_acl_api->create_acl_table(&m_learnDisableAclTable, gSwitchId,
                                           (uint32_t)attrs.size(), attrs.data());
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("MAC_MOVE_GUARD: Failed to create learn-disable ACL table, rv:%d", status);
        m_learnDisableAclTable = SAI_NULL_OBJECT_ID;
        return false;
    }

    if (gCrmOrch)
    {
        gCrmOrch->incCrmAclUsedCounter(CrmResourceType::CRM_ACL_TABLE,
                                       SAI_ACL_STAGE_PRE_INGRESS, SAI_ACL_BIND_POINT_TYPE_SWITCH);
    }

    // Bind table directly to SAI_SWITCH_ATTR_PRE_INGRESS_ACL.
    // SwitchOrch::bindAclTableToSwitch() does not support the PRE_INGRESS stage.
    sai_attribute_t bind_attr;
    bind_attr.id = SAI_SWITCH_ATTR_PRE_INGRESS_ACL;
    bind_attr.value.oid = m_learnDisableAclTable;
    sai_status_t bind_status = sai_switch_api->set_switch_attribute(gSwitchId, &bind_attr);
    if (bind_status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("MAC_MOVE_GUARD: Failed to bind learn-disable ACL table 0x%" PRIx64
                      " to SAI_SWITCH_ATTR_PRE_INGRESS_ACL, rv:%d",
                      m_learnDisableAclTable, bind_status);
        sai_status_t rm_status = sai_acl_api->remove_acl_table(m_learnDisableAclTable);
        if (rm_status != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_ERROR("MAC_MOVE_GUARD: Failed to roll back ACL table 0x%" PRIx64 ", rv:%d",
                          m_learnDisableAclTable, rm_status);
        }
        else if (gCrmOrch)
        {
            gCrmOrch->decCrmAclUsedCounter(CrmResourceType::CRM_ACL_TABLE,
                                           SAI_ACL_STAGE_PRE_INGRESS, SAI_ACL_BIND_POINT_TYPE_SWITCH,
                                           m_learnDisableAclTable);
        }
        m_learnDisableAclTable = SAI_NULL_OBJECT_ID;
        return false;
    }

    SWSS_LOG_NOTICE("MAC_MOVE_GUARD: created and pre-ingress switch-bound learn-disable ACL table 0x%" PRIx64,
                   m_learnDisableAclTable);
    return true;
}

// Tear down the shared learn-disable ACL table. Caller is responsible for
// removing any entries that reference the table before invoking this.
// Unbind from the switch pre-ingress slot first, then delete the table.
void MacMoveGuardOrch::destroyLearnDisableAclTable()
{
    SWSS_LOG_ENTER();

    if (m_learnDisableAclTable == SAI_NULL_OBJECT_ID)
    {
        return;
    }

    if (m_learnDisableAclEntryCount > 0)
    {
        SWSS_LOG_ERROR("MAC_MOVE_GUARD: refusing to destroy learn-disable ACL table 0x%" PRIx64
                      "; %zu entries still reference it",
                      m_learnDisableAclTable, m_learnDisableAclEntryCount);
        return;
    }

    // Unbind from SAI_SWITCH_ATTR_PRE_INGRESS_ACL inline; SwitchOrch's helper
    // does not support the PRE_INGRESS stage.
    sai_attribute_t unbind_attr;
    unbind_attr.id = SAI_SWITCH_ATTR_PRE_INGRESS_ACL;
    unbind_attr.value.oid = SAI_NULL_OBJECT_ID;
    sai_status_t unbind_status = sai_switch_api->set_switch_attribute(gSwitchId, &unbind_attr);
    if (unbind_status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("MAC_MOVE_GUARD: Failed to unbind learn-disable ACL table 0x%" PRIx64
                      " from SAI_SWITCH_ATTR_PRE_INGRESS_ACL, rv:%d;"
                      " aborting teardown to avoid leaking a bound table",
                      m_learnDisableAclTable, unbind_status);
        return;
    }

    sai_status_t status = sai_acl_api->remove_acl_table(m_learnDisableAclTable);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("MAC_MOVE_GUARD: Failed to remove learn-disable ACL table 0x%" PRIx64 ", rv:%d",
                      m_learnDisableAclTable, status);
        return;
    }

    if (gCrmOrch)
    {
        gCrmOrch->decCrmAclUsedCounter(CrmResourceType::CRM_ACL_TABLE,
                                       SAI_ACL_STAGE_PRE_INGRESS, SAI_ACL_BIND_POINT_TYPE_SWITCH,
                                       m_learnDisableAclTable);
    }

    SWSS_LOG_NOTICE("MAC_MOVE_GUARD: removed learn-disable ACL table 0x%" PRIx64, m_learnDisableAclTable);
    m_learnDisableAclTable = SAI_NULL_OBJECT_ID;
}

// Reconcile the learn-disable ACL table lifecycle with the new CONFIG_DB state.
// The table is created when the feature is enabled with DISABLE_LEARN_ON_MAC_WITH_ACL
// and destroyed when the feature leaves that state. Existing per-MAC entries are
// torn down before the table is destroyed.
void MacMoveGuardOrch::reconcileLearnDisableAclTable(bool prev_enabled, MacMoveGuardAction prev_action)
{
    SWSS_LOG_ENTER();

    bool prev_needs_table = prev_enabled && prev_action == MacMoveGuardAction::DISABLE_LEARN_ON_MAC_WITH_ACL;
    bool now_needs_table  = m_enabled    && m_action    == MacMoveGuardAction::DISABLE_LEARN_ON_MAC_WITH_ACL;

    if (prev_needs_table && !now_needs_table)
    {
        // Action changed away from DISABLE_LEARN_ON_MAC_WITH_ACL while the feature
        // remains enabled. Remove any installed entries and the bad-MAC tracking
        // that was specific to this action, then destroy the table.
        for (auto it = m_macTrackingState.begin(); it != m_macTrackingState.end(); )
        {
            MacMoveTrackingState &state = it->second;
            if (state.action == MacMoveGuardAction::DISABLE_LEARN_ON_MAC_WITH_ACL && state.is_bad_mac)
            {
                if (state.learn_disable_acl_entry_id != SAI_NULL_OBJECT_ID)
                {
                    removeLearnDisableAclEntry(state);
                }
                removeBadMacFromState(it->first);
                it = m_macTrackingState.erase(it);
            }
            else
            {
                ++it;
            }
        }
        destroyLearnDisableAclTable();
    }
    else if (now_needs_table && m_learnDisableAclTable == SAI_NULL_OBJECT_ID)
    {
        // Check platform capability; if SET_DO_NOT_LEARN is not supported at
        // PRE_INGRESS, soft-disable the action (config remains in CONFIG_DB
        // and will activate automatically on a platform that supports it).
        if (!isAclSetDoNotLearnSupported())
        {
            SWSS_LOG_NOTICE("MAC_MOVE_GUARD: config requests DISABLE_LEARN_ON_MAC_WITH_ACL "
                           "but SAI_ACL_ACTION_TYPE_SET_DO_NOT_LEARN is not "
                           "supported on this platform; action is soft-disabled");
        }
        else if (!ensureLearnDisableAclTable())
        {
            SWSS_LOG_ERROR("MAC_MOVE_GUARD: failed to create learn-disable ACL "
                          "table from config; DISABLE_LEARN_ON_MAC_WITH_ACL will not be effective");
        }
    }
}

// Install one per-MAC ACL entry: match (vlan, smac) and apply SET_DO_NOT_LEARN
// so the source MAC is not learned/relearned while forwarding continues via
// the normal FDB lookup. The action parameter is not used; aclaction.enable
// controls whether the action fires.
bool MacMoveGuardOrch::installLearnDisableAclEntry(const MacKey &key, MacMoveTrackingState &state)
{
    SWSS_LOG_ENTER();

    if (!m_portsOrch)
    {
        SWSS_LOG_ERROR("MAC_MOVE_GUARD: PortsOrch not available for ACL entry creation");
        return false;
    }

    Port vlan;
    if (!m_portsOrch->getPort(key.bv_id, vlan))
    {
        SWSS_LOG_ERROR("MAC_MOVE_GUARD: Failed to resolve VLAN for bv_id=0x%" PRIx64, key.bv_id);
        return false;
    }

    std::vector<sai_attribute_t> attrs;
    sai_attribute_t a;

    a.id = SAI_ACL_ENTRY_ATTR_TABLE_ID;
    a.value.oid = m_learnDisableAclTable;
    attrs.push_back(a);

    a.id = SAI_ACL_ENTRY_ATTR_PRIORITY;
    a.value.u32 = 100;
    attrs.push_back(a);

    a.id = SAI_ACL_ENTRY_ATTR_ADMIN_STATE;
    a.value.booldata = true;
    attrs.push_back(a);

    a.id = SAI_ACL_ENTRY_ATTR_FIELD_SRC_MAC;
    a.value.aclfield.enable = true;
    memcpy(a.value.aclfield.data.mac, key.mac.getMac(), sizeof(sai_mac_t));
    memset(a.value.aclfield.mask.mac, 0xff, sizeof(sai_mac_t));
    attrs.push_back(a);

    a.id = SAI_ACL_ENTRY_ATTR_FIELD_OUTER_VLAN_ID;
    a.value.aclfield.enable = true;
    a.value.aclfield.data.u16 = vlan.m_vlan_info.vlan_id;
    a.value.aclfield.mask.u16 = 0x0fff;
    attrs.push_back(a);

    a.id = SAI_ACL_ENTRY_ATTR_ACTION_SET_DO_NOT_LEARN;
    a.value.aclaction.enable = true;
    // parameter is not needed for SET_DO_NOT_LEARN; aclaction.enable suffices
    attrs.push_back(a);

    sai_object_id_t entry_oid = SAI_NULL_OBJECT_ID;
    sai_status_t status = sai_acl_api->create_acl_entry(&entry_oid, gSwitchId,
                                                       (uint32_t)attrs.size(), attrs.data());
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("MAC_MOVE_GUARD: Failed to create learn-disable ACL entry for MAC %s vlan %u, rv:%d",
                      key.mac.to_string().c_str(), vlan.m_vlan_info.vlan_id, status);
        return false;
    }

    state.learn_disable_acl_entry_id = entry_oid;
    m_learnDisableAclEntryCount++;

    if (gCrmOrch)
    {
        gCrmOrch->incCrmAclTableUsedCounter(CrmResourceType::CRM_ACL_ENTRY, m_learnDisableAclTable);
    }

    SWSS_LOG_NOTICE("MAC_MOVE_GUARD: installed learn-disable ACL entry 0x%" PRIx64
                   " for MAC %s vlan %u (ref count: %zu)",
                   entry_oid, key.mac.to_string().c_str(), vlan.m_vlan_info.vlan_id,
                   m_learnDisableAclEntryCount);
    return true;
}

// Remove the per-MAC ACL entry previously installed by installLearnDisableAclEntry.
void MacMoveGuardOrch::removeLearnDisableAclEntry(MacMoveTrackingState &state)
{
    SWSS_LOG_ENTER();

    if (state.learn_disable_acl_entry_id == SAI_NULL_OBJECT_ID)
    {
        return;
    }

    sai_object_id_t entry_oid = state.learn_disable_acl_entry_id;
    sai_status_t status = sai_acl_api->remove_acl_entry(entry_oid);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("MAC_MOVE_GUARD: Failed to remove learn-disable ACL entry 0x%" PRIx64 ", rv:%d",
                      entry_oid, status);
        // Still drop the reference so we don't leak our bookkeeping
    }
    else if (gCrmOrch && m_learnDisableAclTable != SAI_NULL_OBJECT_ID)
    {
        gCrmOrch->decCrmAclTableUsedCounter(CrmResourceType::CRM_ACL_ENTRY, m_learnDisableAclTable);
    }

    state.learn_disable_acl_entry_id = SAI_NULL_OBJECT_ID;
    if (m_learnDisableAclEntryCount > 0)
    {
        m_learnDisableAclEntryCount--;
    }

    SWSS_LOG_NOTICE("MAC_MOVE_GUARD: removed learn-disable ACL entry 0x%" PRIx64 " (ref count: %zu)",
                   entry_oid, m_learnDisableAclEntryCount);
}
