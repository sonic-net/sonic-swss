#include "macmoveguard.h"
#include "fdborch.h"
#include "logger.h"
#include "notifier.h"
#include "subscriberstatetable.h"
#include "switchorch.h"
#include "crmorch.h"

#include <chrono>
#include <inttypes.h>
#include <array>
#include <cstring>
#include <vector>
#include <cstring>
extern sai_fdb_api_t    *sai_fdb_api;
extern sai_acl_api_t    *sai_acl_api;
extern sai_switch_api_t *sai_switch_api;
extern sai_object_id_t gSwitchId;
extern SwitchOrch    *gSwitchOrch;
extern CrmOrch       *gCrmOrch;

using namespace std;
using namespace std::chrono;

const int MacMoveGuard::RECOVERY_CHECK_INTERVAL_SECS;
constexpr const char *MacMoveGuard::HW_RESOURCES_KEY;
constexpr const char *MacMoveGuard::DISABLED_PORTS_FIELD;

MacMoveGuard::MacMoveGuard(DBConnector *configDb, DBConnector *stateDb,
                           const string &tableName,
                           PortsOrch *portsOrch, FdbOrch *fdbOrch) :
    m_portsOrch(portsOrch),
    m_fdbOrch(fdbOrch),
    m_tableName(tableName)
{
    SWSS_LOG_ENTER();

    // STATE_DB table for the cleanup-on-restart record. The recovery timer's
    // first eligible tick (once PortsOrch reports allPortsReady()) runs a
    // one-shot reconcile via restoreHwResources() that reverts any hardware
    // state we may have left behind in a previous orchagent run
    // (admin-disabled ports + a stale pre-ingress ACL table). Reconcile is
    // deliberately deferred out of the constructor so that PortsOrch has
    // finished populating port OIDs from APP_DB before we try to drive SAI.
    m_stateTable.reset(new swss::Table(stateDb, STATE_MAC_MOVE_GUARD_TABLE_NAME));
    m_capabilityTable.reset(new swss::Table(stateDb, STATE_MMG_CAPABILITY_TABLE_NAME));

    // Probe DLOMWA support and publish the action-capability row to STATE_DB.
    // Probing eagerly here (rather than lazily on first DLOMWA configure)
    // keeps the published row's contents stable for the lifetime of the
    // process and lets consumers learn what's available before any config is
    // applied. The result is cached in m_aclSetDoNotLearnSupported.
    (void)isAclSetDoNotLearnSupported();
    publishActionCapabilities();

    // Register a Consumer for the MAC_MOVE_GUARD CONFIG_DB table on the owning
    // FdbOrch. FdbOrch::doTask(Consumer&) dispatches back into doConfigTask().
    auto *consumerTable = new swss::SubscriberStateTable(
        configDb, tableName, swss::TableConsumable::DEFAULT_POP_BATCH_SIZE, default_orch_pri);
    auto *consumer      = new Consumer(consumerTable, m_fdbOrch, tableName);
    m_fdbOrch->addExecutor(consumer);

    // Periodic recovery check timer, also registered on the owning FdbOrch.
    m_recoveryTimer = new swss::SelectableTimer(
        timespec{ .tv_sec = RECOVERY_CHECK_INTERVAL_SECS, .tv_nsec = 0 });
    auto recoveryExecutor = new ExecutableTimer(m_recoveryTimer, m_fdbOrch,
                                                MAC_MOVE_GUARD_RECOVERY_TIMER_NAME);
    m_fdbOrch->addExecutor(recoveryExecutor);
    m_recoveryTimer->start();

    SWSS_LOG_NOTICE("MacMoveGuard initialized");
}

MacMoveGuard::~MacMoveGuard()
{
    // Executors registered on FdbOrch are owned by FdbOrch and will be
    // destroyed with it; nothing to detach here.
}

void MacMoveGuard::clearAllState()
{
    // Re-enable any ports we have administratively disabled (DISABLE_PORT
    // action). Only drop a port from m_disabledPorts if SAI actually accepted
    // the re-enable — otherwise the port stays in the in-memory set and the
    // STATE_DB row remains populated, so the next reconcile (this run or a
    // subsequent restart) can retry rather than orphaning the port.
    for (auto it = m_disabledPorts.begin(); it != m_disabledPorts.end(); )
    {
        if (m_portsOrch && m_portsOrch->setPortAdminStatusByAlias(it->first, true))
        {
            SWSS_LOG_NOTICE("MAC_MOVE_GUARD: re-enabled port %s (feature disabled)",
                            it->first.c_str());
            it = m_disabledPorts.erase(it);
        }
        else
        {
            SWSS_LOG_ERROR("MAC_MOVE_GUARD: failed to re-enable port %s on feature "
                           "disable; keeping it persisted for retry", it->first.c_str());
            // Drop the now-orphaned bad-MAC refs: m_macTrackingState is wiped
            // below, so these MacKey copies no longer correspond to anything.
            // Only the port-name key matters for the retry path (in-run via
            // a later clearAllState/markBadMac, or across restart via the
            // STATE_DB CSV that persistDisabledPorts writes).
            it->second.clear();
            ++it;
        }
    }

    // Remove any ACL entries we programmed (DISABLE_LEARN_ON_MAC_WITH_ACL action).
    for (auto &kv : m_macTrackingState)
    {
        MacMoveTrackingState &state = kv.second;
        if (state.learn_disable_acl_entry_id != SAI_NULL_OBJECT_ID)
        {
            removeLearnDisableAclEntry(state);
        }
    }

    // Tear down the shared learn-disable ACL table; all entries have been removed above.
    destroyLearnDisableAclTable();

    // Clear tracking state that is unrelated to the disabled-ports retry set.
    m_macTrackingState.clear();
    m_learntMac.clear();

    // Persist whatever ports remain (may be empty, in which case the row is
    // deleted) so a restart will reattempt the failed re-enables.
    persistDisabledPorts();
}

void MacMoveGuard::doConfigTask(Consumer &consumer)
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
                        uint32_t v = static_cast<uint32_t>(stoul(value));
                        if (v < 1)
                        {
                            SWSS_LOG_WARN("MAC_MOVE_GUARD: ignoring threshold=%u (must be >= 1); "
                                          "keeping current value %u", v, m_threshold);
                        }
                        else
                        {
                            m_threshold = v;
                        }
                    }
                    else if (field == "detect_interval")
                    {
                        uint32_t v = static_cast<uint32_t>(stoul(value));
                        if (v < 1)
                        {
                            SWSS_LOG_WARN("MAC_MOVE_GUARD: ignoring detect_interval=%u (must be >= 1s); "
                                          "keeping current value %us", v, m_durationSeconds);
                        }
                        else
                        {
                            m_durationSeconds = v;
                        }
                    }
                    else if (field == "action_interval")
                    {
                        uint32_t v = static_cast<uint32_t>(stoul(value));
                        if (v < 1)
                        {
                            SWSS_LOG_WARN("MAC_MOVE_GUARD: ignoring action_interval=%u (must be >= 1s); "
                                          "keeping current value %us", v, m_recoverySeconds);
                        }
                        else
                        {
                            m_recoverySeconds = v;
                        }
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

void MacMoveGuard::doRecoveryTimerTask()
{
    SWSS_LOG_ENTER();

    // One-shot post-ports-ready reconcile. Runs at most once per process
    // lifetime, on the first tick where PortsOrch reports allPortsReady().
    // We defer this out of the constructor because reconcile may issue SAI
    // port and ACL operations, and during FdbOrch construction PortsOrch
    // has not necessarily finished populating port OIDs from APP_DB.
    //
    // Reconcile runs regardless of m_enabled: if the feature was enabled in
    // a previous orchagent run and is now disabled, we still need to revert
    // any HW state recorded in STATE_DB rather than orphan it.
    if (!m_reconcileDone)
    {
        if (m_portsOrch && !m_portsOrch->allPortsReady())
        {
            return;
        }
        restoreHwResources();
        m_reconcileDone = true;
    }

    if (!m_enabled)
    {
        return;
    }

    checkRecovery();
}

void MacMoveGuard::onMacMove(const MacMoveNotification &notif)
{
    SWSS_LOG_ENTER();

    // Latch the native-MOVE indicator regardless of whether the feature is
    // currently enabled. The latch reflects platform behaviour, not feature
    // state — and we want it set as early as possible so that any LEARN
    // notifications interleaved with the first native MOVE after the feature
    // is enabled do not double-count.
    if (!m_nativeMovesSeen)
    {
        m_nativeMovesSeen = true;
        SWSS_LOG_NOTICE("MAC_MOVE_GUARD: observed native SAI_FDB_EVENT_MOVE; "
                        "disabling LEARN-path move synthesis for this run");
    }

    if (!m_enabled)
    {
        return;
    }
    handleMacMove(notif);
}

void MacMoveGuard::onMacLearn(const MacLearnNotification &notif)
{
    SWSS_LOG_ENTER();
    if (!m_enabled)
    {
        return;
    }
    handleMacLearn(notif);
}

void MacMoveGuard::pruneWindow(MacMoveTrackingState &state)
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

void MacMoveGuard::handleMacLearn(const MacLearnNotification &notif)
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

    // On platforms that emit a native SAI_FDB_EVENT_MOVE for MAC moves, the
    // MOVE path has already counted this event. Suppress the LEARN-synthesis
    // path so the same move is not counted twice. m_learntMac is still
    // updated above so that the cache stays current.
    if (m_nativeMovesSeen)
    {
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

void MacMoveGuard::handleMacMove(const MacMoveNotification &notif)
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
            SWSS_LOG_INFO("MAC_MOVE_GUARD: BAD MAC %s on vlan_oid=0x%" PRIx64
                          " continues to move (port %s), extending action interval by %us",
                          notif.mac.to_string().c_str(), notif.bv_id, new_alias.c_str(),
                          m_recoverySeconds);
        }
        else
        {
            // First time exceeding threshold - mark as bad MAC
            SWSS_LOG_INFO("MAC_MOVE_GUARD: BAD MAC detected: %s on vlan_oid=0x%" PRIx64
                          ", threshold %u exceeded with %zu moves seen on %zu ports in %us",
                          notif.mac.to_string().c_str(), notif.bv_id,
                          m_threshold, state.move_count, state.ports_seen.size(), m_durationSeconds);
        }

        markBadMac(key, state, new_alias);
    }
}

void MacMoveGuard::markBadMac(const MacKey &key, MacMoveTrackingState &state, const string &portName)
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

            // Update the persisted disabled-ports CSV so the cleanup sweep
            // can re-enable these ports if orchagent restarts.
            persistDisabledPorts();
            break;
        }

        case MacMoveGuardAction::DISABLE_LEARN_ON_MAC_WITH_ACL:
        {
            // Install a pre-ingress ACL entry (vlan, smac) -> SET_DO_NOT_LEARN
            // so the source MAC is not re-learned while forwarding continues
            // via the normal FDB lookup. Nothing is persisted to STATE_DB;
            // on restart the cleanup sweep rediscovers the table via the
            // SAI_SWITCH_ATTR_PRE_INGRESS_ACL binding.
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
                break;
            }

            if (!installLearnDisableAclEntry(key, state))
            {
                SWSS_LOG_ERROR("MAC_MOVE_GUARD: Failed to install learn-disable ACL entry for MAC %s",
                              key.mac.to_string().c_str());
                return;
            }
            break;
        }

        // Future actions can be added here as new case statements
        default:
            SWSS_LOG_WARN("MAC_MOVE_GUARD: unknown action type, no action taken");
            break;
    }
}

void MacMoveGuard::releaseBadMac(const MacKey &key, MacMoveTrackingState &state)
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

            // Rewrite the persisted disabled-ports CSV (may now be empty).
            persistDisabledPorts();
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
            break;
        }

        // Future actions can be added here as new case statements
        default:
            break;
    }
}

void MacMoveGuard::pruneLearntMacCache()
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

void MacMoveGuard::checkRecovery()
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

void MacMoveGuard::reapplyActionIntervalToBadMacs(uint32_t prev_recovery_seconds)
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
        // marked under the previous, longer interval. Expiry is in-memory
        // only — nothing to persist.
        if (new_expiry < state.action_expiry_time)
        {
            state.action_expiry_time = new_expiry;
            ++shortened;
        }
        // Lengthening the interval: extend entries whose current
        // remaining time is shorter than the new interval, so a longer
        // action_interval also takes effect immediately.
        else if (new_expiry > state.action_expiry_time)
        {
            state.action_expiry_time = new_expiry;
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


// Persist the current set of admin-disabled ports as a single comma-separated
// field in a single STATE_DB row. The constructor's restart sweep consumes
// this CSV to re-enable any ports we left disabled in a previous run. If the
// set is empty we delete the row outright.
void MacMoveGuard::persistDisabledPorts()
{
    if (m_disabledPorts.empty())
    {
        m_stateTable->del(HW_RESOURCES_KEY);
        return;
    }

    std::string joined;
    for (const auto &kv : m_disabledPorts)
    {
        if (!joined.empty()) joined += ",";
        joined += kv.first;
    }

    std::vector<FieldValueTuple> fvs = {
        FieldValueTuple(DISABLED_PORTS_FIELD, joined),
    };
    m_stateTable->set(HW_RESOURCES_KEY, fvs);
}

// One-shot cleanup sweep run from the constructor. Reverts any hardware state
// we may have left behind in a previous orchagent run:
//   1) Re-enables ports listed in STATE_DB's disabled_ports CSV.
//   2) If SAI_SWITCH_ATTR_PRE_INGRESS_ACL is bound and the table's signature
//      matches ours, deletes all of its entries, unbinds the switch attr, and
//      deletes the table.
// After this completes the in-memory bad-MAC tracking is empty and the
// STATE_DB row is gone. There is no replay of per-MAC state.
void MacMoveGuard::restoreHwResources()
{
    SWSS_LOG_ENTER();

    // (1) Re-enable any previously disabled ports. We only erase a port from
    // STATE_DB if SAI accepted the re-enable; ports that fail are kept in
    // m_disabledPorts (and re-persisted at the end) so a subsequent reconcile
    // or restart can retry them rather than orphaning the admin-down state.
    std::vector<FieldValueTuple> fvs;
    if (m_stateTable->get(HW_RESOURCES_KEY, fvs))
    {
        std::string csv;
        for (const auto &fv : fvs)
        {
            if (fvField(fv) == DISABLED_PORTS_FIELD)
            {
                csv = fvValue(fv);
                break;
            }
        }

        size_t start = 0;
        while (start < csv.size())
        {
            size_t comma = csv.find(',', start);
            std::string port = csv.substr(
                start, comma == std::string::npos ? std::string::npos : comma - start);
            if (!port.empty() && m_portsOrch)
            {
                if (m_portsOrch->setPortAdminStatusByAlias(port, true))
                {
                    SWSS_LOG_NOTICE("MAC_MOVE_GUARD: restart cleanup re-enabled port %s",
                                    port.c_str());
                }
                else
                {
                    SWSS_LOG_WARN("MAC_MOVE_GUARD: restart cleanup failed to re-enable port %s; "
                                  "keeping it persisted for retry", port.c_str());
                    // Track unresolved ports so the row survives the
                    // persistDisabledPorts() rewrite below. The MacKey set is
                    // left empty — there is no per-MAC tracking restored on
                    // restart by design.
                    m_disabledPorts[port];
                }
            }
            if (comma == std::string::npos) break;
            start = comma + 1;
        }

        // Rewrite the STATE_DB row from the (possibly reduced) in-memory set.
        // If everything was re-enabled, m_disabledPorts is empty and
        // persistDisabledPorts() will delete the row.
        persistDisabledPorts();
    }

    // (2) If a pre-ingress ACL table is bound and looks like ours
    // (signature match on stage / fields / action_list), tear it down.
    sai_attribute_t cur;
    cur.id = SAI_SWITCH_ATTR_PRE_INGRESS_ACL;
    cur.value.oid = SAI_NULL_OBJECT_ID;
    if (sai_switch_api->get_switch_attribute(gSwitchId, 1, &cur) != SAI_STATUS_SUCCESS)
    {
        return;
    }
    sai_object_id_t bound = cur.value.oid;
    if (bound == SAI_NULL_OBJECT_ID || !aclTableMatchesOurSignature(bound))
    {
        return;
    }

    SWSS_LOG_NOTICE("MAC_MOVE_GUARD: restart cleanup found pre-ingress ACL table 0x%" PRIx64
                    " matching our signature; tearing down", bound);

    // Enumerate and delete every entry in the table. We use a stack buffer
    // sized generously; if the table somehow has more entries than that, we
    // log and continue with a best-effort partial delete (subsequent passes
    // when entries are recreated will not be a problem since the table will
    // also be torn down below).
    constexpr uint32_t MAX_ENTRIES = 4096;
    std::vector<sai_object_id_t> entries(MAX_ENTRIES, SAI_NULL_OBJECT_ID);
    sai_attribute_t list_attr;
    list_attr.id = SAI_ACL_TABLE_ATTR_ENTRY_LIST;
    list_attr.value.objlist.count = MAX_ENTRIES;
    list_attr.value.objlist.list  = entries.data();
    if (sai_acl_api->get_acl_table_attribute(bound, 1, &list_attr) == SAI_STATUS_SUCCESS)
    {
        for (uint32_t i = 0; i < list_attr.value.objlist.count; ++i)
        {
            sai_object_id_t entry = list_attr.value.objlist.list[i];
            if (entry == SAI_NULL_OBJECT_ID) continue;
            if (sai_acl_api->remove_acl_entry(entry) == SAI_STATUS_SUCCESS && gCrmOrch)
            {
                gCrmOrch->decCrmAclTableUsedCounter(CrmResourceType::CRM_ACL_ENTRY, bound);
            }
        }
    }

    // Unbind via helper. Helper re-reads the binding and only writes when it
    // still equals `bound`; if another feature has taken the slot since our
    // initial read, the helper logs a warning and we proceed to remove the
    // table without touching the binding.
    (void)unbindPreIngressAclTable(bound);

    if (sai_acl_api->remove_acl_table(bound) == SAI_STATUS_SUCCESS)
    {
        crmPreIngressAclTableDec(bound);
    }
}

// Decide whether a pre-ingress ACL table looks like one we created. We check
// the small set of attributes that define our table's contract: stage,
// matched fields, and the action type list. Anything matching all three is
// treated as ours.
bool MacMoveGuard::aclTableMatchesOurSignature(sai_object_id_t table_oid) const
{
    std::array<sai_attribute_t, 4> attrs{};
    std::array<int32_t, 16> action_list_buf{};

    attrs[0].id = SAI_ACL_TABLE_ATTR_ACL_STAGE;
    attrs[1].id = SAI_ACL_TABLE_ATTR_FIELD_SRC_MAC;
    attrs[2].id = SAI_ACL_TABLE_ATTR_FIELD_OUTER_VLAN_ID;
    attrs[3].id = SAI_ACL_TABLE_ATTR_ACL_ACTION_TYPE_LIST;
    attrs[3].value.s32list.count = (uint32_t)action_list_buf.size();
    attrs[3].value.s32list.list  = action_list_buf.data();

    if (sai_acl_api->get_acl_table_attribute(table_oid,
                                             (uint32_t)attrs.size(),
                                             attrs.data()) != SAI_STATUS_SUCCESS)
    {
        return false;
    }

    if (attrs[0].value.s32 != SAI_ACL_STAGE_PRE_INGRESS)         return false;
    if (!attrs[1].value.booldata)                                return false;
    if (!attrs[2].value.booldata)                                return false;
    if (attrs[3].value.s32list.count != 1)                       return false;
    if (attrs[3].value.s32list.list[0] != SAI_ACL_ACTION_TYPE_SET_DO_NOT_LEARN) return false;
    return true;
}

// Query the platform once for SAI_ACL_ACTION_TYPE_SET_DO_NOT_LEARN support at
// the PRE_INGRESS stage and cache the result. Returns true if the action is
// in the platform's pre-ingress capability list.
bool MacMoveGuard::isAclSetDoNotLearnSupported()
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

// Publish the per-action capability row to STATE_DB. DISABLE_PORT is always
// "true"; DISABLE_LEARN_ON_MAC_WITH_ACL reflects the platform probe.
void MacMoveGuard::publishActionCapabilities()
{
    SWSS_LOG_ENTER();

    std::vector<FieldValueTuple> fvs = {
        FieldValueTuple(MMG_ACTION_DISABLE_PORT, "true"),
        FieldValueTuple(MMG_ACTION_DISABLE_LEARN_ON_MAC_WITH_ACL,
                        (m_aclSetDoNotLearnSupported == 1) ? "true" : "false"),
    };
    m_capabilityTable->set(MMG_CAPABILITY_ACTIONS_KEY, fvs);
}

// Create the shared pre-ingress ACL table used by DISABLE_LEARN_ON_MAC_WITH_ACL and
// bind it directly to the switch pre-ingress slot (SAI_SWITCH_ATTR_PRE_INGRESS_ACL).
// Idempotent.
bool MacMoveGuard::ensureLearnDisableAclTable()
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

    sai_status_t status = sai_acl_api->create_acl_table(&m_learnDisableAclTable, gSwitchId,
                                                        (uint32_t)attrs.size(), attrs.data());
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("MAC_MOVE_GUARD: Failed to create learn-disable ACL table, rv:%d", status);
        m_learnDisableAclTable = SAI_NULL_OBJECT_ID;
        return false;
    }

    crmPreIngressAclTableInc();

    // Bind directly to SAI_SWITCH_ATTR_PRE_INGRESS_ACL via helper. Helper
    // refuses if another feature has the slot, in which case we roll back
    // the table we just created. SwitchOrch::bindAclTableToSwitch() does
    // not support the PRE_INGRESS stage, so this is the only bind path.
    if (!bindPreIngressAclTable(m_learnDisableAclTable))
    {
        sai_status_t rm_status = sai_acl_api->remove_acl_table(m_learnDisableAclTable);
        if (rm_status != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_ERROR("MAC_MOVE_GUARD: Failed to roll back ACL table 0x%" PRIx64 ", rv:%d",
                          m_learnDisableAclTable, rm_status);
        }
        else
        {
            crmPreIngressAclTableDec(m_learnDisableAclTable);
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
void MacMoveGuard::destroyLearnDisableAclTable()
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

    // Unbind from the pre-ingress slot via helper. Helper is conservative —
    // it only writes when the current binding still equals our table, so a
    // foreign feature that replaced our binding after we created the table
    // is left untouched. We treat helper-failure (couldn't read or couldn't
    // write the set) as a teardown abort to avoid leaking a bound table.
    if (!unbindPreIngressAclTable(m_learnDisableAclTable))
    {
        SWSS_LOG_ERROR("MAC_MOVE_GUARD: unbind failed for learn-disable ACL table 0x%" PRIx64
                      "; aborting teardown",
                      m_learnDisableAclTable);
        return;
    }

    sai_status_t status = sai_acl_api->remove_acl_table(m_learnDisableAclTable);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("MAC_MOVE_GUARD: Failed to remove learn-disable ACL table 0x%" PRIx64 ", rv:%d",
                      m_learnDisableAclTable, status);
        return;
    }

    crmPreIngressAclTableDec(m_learnDisableAclTable);

    SWSS_LOG_NOTICE("MAC_MOVE_GUARD: removed learn-disable ACL table 0x%" PRIx64, m_learnDisableAclTable);
    m_learnDisableAclTable = SAI_NULL_OBJECT_ID;
}

// Reconcile the learn-disable ACL table lifecycle with the new CONFIG_DB state.
// The table is created when the feature is enabled with DISABLE_LEARN_ON_MAC_WITH_ACL
// and destroyed when the feature leaves that state. Existing per-MAC entries are
// torn down before the table is destroyed.
void MacMoveGuard::reconcileLearnDisableAclTable(bool prev_enabled, MacMoveGuardAction prev_action)
{
    SWSS_LOG_ENTER();

    bool prev_needs_table = prev_enabled && prev_action == MacMoveGuardAction::DISABLE_LEARN_ON_MAC_WITH_ACL;
    bool now_needs_table  = m_enabled    && m_action    == MacMoveGuardAction::DISABLE_LEARN_ON_MAC_WITH_ACL;

    if (prev_needs_table && !now_needs_table)
    {
        // Action changed away from DISABLE_LEARN_ON_MAC_WITH_ACL while the feature
        // remains enabled. Remove any installed entries and the bad-MAC tracking
        // that was specific to this action, then destroy the table. Nothing to
        // remove from STATE_DB — DLOMWA does not persist per-MAC state.
        for (auto it = m_macTrackingState.begin(); it != m_macTrackingState.end(); )
        {
            MacMoveTrackingState &state = it->second;
            if (state.action == MacMoveGuardAction::DISABLE_LEARN_ON_MAC_WITH_ACL && state.is_bad_mac)
            {
                if (state.learn_disable_acl_entry_id != SAI_NULL_OBJECT_ID)
                {
                    removeLearnDisableAclEntry(state);
                }
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

// Bind the given table OID into SAI_SWITCH_ATTR_PRE_INGRESS_ACL. Refuses if
// the slot is already occupied so we don't overwrite another feature's
// binding. Caller is responsible for any rollback (e.g. removing the table
// it just created) when this returns false.
bool MacMoveGuard::bindPreIngressAclTable(sai_object_id_t table_oid)
{
    SWSS_LOG_ENTER();

    sai_attribute_t cur;
    cur.id = SAI_SWITCH_ATTR_PRE_INGRESS_ACL;
    cur.value.oid = SAI_NULL_OBJECT_ID;
    sai_status_t status = sai_switch_api->get_switch_attribute(gSwitchId, 1, &cur);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("MAC_MOVE_GUARD: Failed to read SAI_SWITCH_ATTR_PRE_INGRESS_ACL "
                       "before binding 0x%" PRIx64 ", rv:%d",
                       table_oid, status);
        return false;
    }
    if (cur.value.oid != SAI_NULL_OBJECT_ID)
    {
        SWSS_LOG_ERROR("MAC_MOVE_GUARD: SAI_SWITCH_ATTR_PRE_INGRESS_ACL already bound to 0x%" PRIx64
                       " by another feature; refusing to overwrite with 0x%" PRIx64,
                       cur.value.oid, table_oid);
        return false;
    }

    sai_attribute_t bind;
    bind.id = SAI_SWITCH_ATTR_PRE_INGRESS_ACL;
    bind.value.oid = table_oid;
    status = sai_switch_api->set_switch_attribute(gSwitchId, &bind);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("MAC_MOVE_GUARD: Failed to bind ACL table 0x%" PRIx64
                       " to SAI_SWITCH_ATTR_PRE_INGRESS_ACL, rv:%d",
                       table_oid, status);
        return false;
    }
    return true;
}

// Unbind SAI_SWITCH_ATTR_PRE_INGRESS_ACL when its current value matches the
// expected OID. If the slot is bound to something else (another feature has
// replaced our binding since we last set it), we skip the write to avoid
// clobbering them and return true so the caller may still proceed to remove
// its own table. Returns false only if the SAI get itself failed or the
// matching set failed.
bool MacMoveGuard::unbindPreIngressAclTable(sai_object_id_t expected_table_oid)
{
    SWSS_LOG_ENTER();

    sai_attribute_t cur;
    cur.id = SAI_SWITCH_ATTR_PRE_INGRESS_ACL;
    cur.value.oid = SAI_NULL_OBJECT_ID;
    sai_status_t status = sai_switch_api->get_switch_attribute(gSwitchId, 1, &cur);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("MAC_MOVE_GUARD: Failed to read SAI_SWITCH_ATTR_PRE_INGRESS_ACL "
                       "before unbinding 0x%" PRIx64 ", rv:%d",
                       expected_table_oid, status);
        return false;
    }
    if (cur.value.oid != expected_table_oid)
    {
        SWSS_LOG_WARN("MAC_MOVE_GUARD: pre-ingress binding is 0x%" PRIx64 ", not our table "
                      "0x%" PRIx64 "; skipping unbind",
                      cur.value.oid, expected_table_oid);
        return true;
    }

    sai_attribute_t unbind;
    unbind.id = SAI_SWITCH_ATTR_PRE_INGRESS_ACL;
    unbind.value.oid = SAI_NULL_OBJECT_ID;
    status = sai_switch_api->set_switch_attribute(gSwitchId, &unbind);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("MAC_MOVE_GUARD: Failed to unbind ACL table 0x%" PRIx64
                       " from SAI_SWITCH_ATTR_PRE_INGRESS_ACL, rv:%d",
                       expected_table_oid, status);
        return false;
    }
    return true;
}

void MacMoveGuard::crmPreIngressAclTableInc()
{
    if (gCrmOrch)
    {
        gCrmOrch->incCrmAclUsedCounter(CrmResourceType::CRM_ACL_TABLE,
                                       SAI_ACL_STAGE_PRE_INGRESS,
                                       SAI_ACL_BIND_POINT_TYPE_SWITCH);
    }
}

void MacMoveGuard::crmPreIngressAclTableDec(sai_object_id_t table_oid)
{
    if (gCrmOrch)
    {
        gCrmOrch->decCrmAclUsedCounter(CrmResourceType::CRM_ACL_TABLE,
                                       SAI_ACL_STAGE_PRE_INGRESS,
                                       SAI_ACL_BIND_POINT_TYPE_SWITCH,
                                       table_oid);
    }
}

// Install one per-MAC ACL entry: match (vlan, smac) and apply SET_DO_NOT_LEARN
// so the source MAC is not learned/relearned while forwarding continues via
// the normal FDB lookup. The action parameter is not used; aclaction.enable
// controls whether the action fires.
bool MacMoveGuard::installLearnDisableAclEntry(const MacKey &key, MacMoveTrackingState &state)
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

    // Defensive duplicate-key check: the SAI ACL entry match key is
    // (vlan_id, smac), not (bv_id, smac). If a prior bad MAC happened to
    // resolve to the same vlan_id with the same MAC (e.g. two bridge
    // objects mapping to the same VLAN), pushing another entry would burn
    // an ACL hardware slot for an identical match. Scan our tracking state
    // and reuse the existing OID instead of issuing a duplicate create.
    for (const auto &kv : m_macTrackingState)
    {
        const MacMoveTrackingState &other = kv.second;
        if (other.learn_disable_acl_entry_id == SAI_NULL_OBJECT_ID) continue;
        if (!(kv.first.mac == key.mac)) continue;

        Port other_vlan;
        if (!m_portsOrch->getPort(kv.first.bv_id, other_vlan)) continue;
        if (other_vlan.m_vlan_info.vlan_id != vlan.m_vlan_info.vlan_id) continue;

        SWSS_LOG_WARN("MAC_MOVE_GUARD: learn-disable ACL entry 0x%" PRIx64
                      " already covers mac=%s vlan=%u (peer bv_id=0x%" PRIx64
                      "); not installing a duplicate for bv_id=0x%" PRIx64
                      ". The existing entry's SET_DO_NOT_LEARN action will apply.",
                      other.learn_disable_acl_entry_id,
                      key.mac.to_string().c_str(), vlan.m_vlan_info.vlan_id,
                      kv.first.bv_id, key.bv_id);
        // Leave state.learn_disable_acl_entry_id == SAI_NULL_OBJECT_ID so the
        // symmetric removeLearnDisableAclEntry() path will not attempt to
        // remove an OID it does not own. The peer state remains the sole
        // owner of the SAI entry and will tear it down on its own release.
        return true;
    }

    // Check CRM availability for the pre-ingress ACL entry resource before
    // issuing the SAI create. The "available" attribute is reported by SAI
    // on the table OID itself; treating 0 as a hard failure avoids a futile
    // SAI call that would also bump used counters incorrectly on failure.
    sai_attribute_t avail_attr;
    avail_attr.id = SAI_ACL_TABLE_ATTR_AVAILABLE_ACL_ENTRY;
    avail_attr.value.u32 = 0;
    if (sai_acl_api->get_acl_table_attribute(m_learnDisableAclTable, 1, &avail_attr) == SAI_STATUS_SUCCESS &&
        avail_attr.value.u32 == 0)
    {
        SWSS_LOG_ERROR("MAC_MOVE_GUARD: pre-ingress ACL entry resource exhausted "
                       "(available=0 on table 0x%" PRIx64 "); skipping create for mac=%s vlan=%u",
                       m_learnDisableAclTable, key.mac.to_string().c_str(), vlan.m_vlan_info.vlan_id);
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
void MacMoveGuard::removeLearnDisableAclEntry(MacMoveTrackingState &state)
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
