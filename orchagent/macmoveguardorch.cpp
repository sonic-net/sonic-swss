#include "macmoveguardorch.h"
#include "fdborch.h"
#include "logger.h"
#include "notifier.h"

#include <chrono>
#include <inttypes.h>

extern sai_fdb_api_t *sai_fdb_api;
extern sai_object_id_t gSwitchId;

using namespace std;
using namespace std::chrono;

/* Out-of-class definitions for the in-class-initialized static const members.
   Required when these constants are ODR-used (e.g. passed to functions that
   take their value by const reference, like the chrono duration constructors). */
const int MacMoveGuardOrch::RECOVERY_CHECK_INTERVAL_SECS;

MacMoveGuardOrch::MacMoveGuardOrch(DBConnector *configDb, const string &tableName,
                                   PortsOrch *portsOrch, FdbOrch *fdbOrch) :
    Orch(configDb, tableName),
    m_portsOrch(portsOrch),
    m_fdbOrch(fdbOrch)
{
    SWSS_LOG_ENTER();

    /* Subscribe to FDB changes from FdbOrch to receive MAC move notifications */
    if (m_fdbOrch)
    {
        m_fdbOrch->attach(this);
    }

    /* Periodic recovery check timer */
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
    /* Re-enable any ports we have administratively disabled (DISABLE_PORT action) */
    for (auto &kv : m_disabledPorts)
    {
        if (m_portsOrch)
        {
            m_portsOrch->setPortAdminStatusByAlias(kv.first, true);
            SWSS_LOG_NOTICE("MAC_MOVE_GUARD: re-enabling port %s (feature disabled)", kv.first.c_str());
        }
    }

    /* Remove any static FDB entries we programmed (DISABLE_MAC_MOVE action) */
    for (auto &kv : m_macTrackingState)
    {
        const MacKey &key = kv.first;
        MacMoveTrackingState &state = kv.second;

        if (state.static_fdb_bridge_port_id != SAI_NULL_OBJECT_ID)
        {
            sai_fdb_entry_t fdb_entry;
            fdb_entry.switch_id = gSwitchId;
            memcpy(fdb_entry.mac_address, key.mac.getMac(), sizeof(sai_mac_t));
            fdb_entry.bv_id = key.bv_id;

            sai_status_t status = sai_fdb_api->remove_fdb_entry(&fdb_entry);
            if (status != SAI_STATUS_SUCCESS)
            {
                SWSS_LOG_ERROR("MAC_MOVE_GUARD: Failed to remove static FDB entry for MAC %s (feature disabled), rv:%d",
                              key.mac.to_string().c_str(), status);
            }
            else
            {
                SWSS_LOG_NOTICE("MAC_MOVE_GUARD: Removed static FDB entry for MAC %s (feature disabled)",
                               key.mac.to_string().c_str());
            }
        }
    }

    /* Clear all tracking state when feature is disabled */
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
                        else if (value == "DISABLE_MAC_MOVE")
                        {
                            m_action = MacMoveGuardAction::DISABLE_MAC_MOVE;
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
            else if (m_action == MacMoveGuardAction::DISABLE_MAC_MOVE)
                action_str = "DISABLE_MAC_MOVE";

            SWSS_LOG_NOTICE("MAC_MOVE_GUARD config: enabled=%s threshold=%u detect_interval=%us action_interval=%us action=%s",
                            m_enabled ? "true" : "false",
                            m_threshold, m_durationSeconds, m_recoverySeconds,
                            action_str);

            if (!m_enabled)
            {
                clearAllState();
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

    /* Remove old move timestamps outside the detection window */
    while (!state.move_timestamps.empty() && state.move_timestamps.front() < cutoff)
    {
        state.move_timestamps.pop_front();
    }

    /* Remove ports that haven't been seen within the detection window */
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

    /* Look up the previously-known port for this (mac, bv_id), then update
       the cache to the newly-learned port. We never erase on AGED, so any
       previously-learned port stays cached until the next LEARN overwrites
       it — which is exactly the signal we use to detect a move. */
    auto it = m_learntMac.find(key);
    std::string prev_port;
    if (it != m_learntMac.end())
    {
        prev_port = it->second;
    }
    m_learntMac[key] = notif.port.m_alias;

    if (prev_port.empty() || prev_port == notif.port.m_alias)
    {
        /* First time we see this MAC, or same port — not a move. */
        return;
    }

    /* It's a move: synthesize a MOVE notification and run it through the
       same path used for native SAI_FDB_EVENT_MOVE. */
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

    /* Keep the learnt-MAC cache in sync for native MOVE events. (For moves
       synthesized from LEARN, handleMacLearn has already updated this.) */
    m_learntMac[key] = new_alias;

    MacMoveTrackingState &state = m_macTrackingState[key];

    auto now = steady_clock::now();

    /* Track this move timestamp in the sliding window */
    state.move_timestamps.push_back(now);

    /* Track this port and when MAC was last seen on it */
    state.ports_seen[new_alias] = now;
    state.last_port = new_alias;

    /* Prune old entries outside the detection window */
    pruneWindow(state);

    /* Update move count based on entries still in the window */
    state.move_count = state.move_timestamps.size();

    SWSS_LOG_INFO("MAC_MOVE_GUARD: MAC %s on vlan_oid=0x%" PRIx64 " move count: %zu, seen on %zu ports (threshold %u)",
                  notif.mac.to_string().c_str(), notif.bv_id,
                  state.move_count, state.ports_seen.size(), m_threshold);

    if (state.move_count >= m_threshold)
    {
        /* Threshold exceeded: mark as bad MAC and pin to one port */
        if (state.is_bad_mac)
        {
            /* Already a bad MAC - extend the action interval */
            SWSS_LOG_NOTICE("MAC_MOVE_GUARD: BAD MAC %s on vlan_oid=0x%" PRIx64
                           " continues to move (port %s), extending action interval by %us",
                           notif.mac.to_string().c_str(), notif.bv_id, new_alias.c_str(),
                           m_recoverySeconds);
        }
        else
        {
            /* First time exceeding threshold - mark as bad MAC */
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

    /* Mark as bad MAC and set/extend action expiry time */
    state.is_bad_mac = true;
    state.action_expiry_time = steady_clock::now() + seconds(m_recoverySeconds);

    /* Execute configured action */
    switch (m_action)
    {
        case MacMoveGuardAction::DISABLE_PORT:
        {
            if (!m_portsOrch)
            {
                SWSS_LOG_ERROR("MAC_MOVE_GUARD: PortsOrch not available");
                return;
            }

            /* Pin MAC to ONE port (keep it active), disable ALL other ports it appeared on
               Strategy: Minimize total disabled ports across all bad MACs by reusing already-disabled ports */

            /* If not already pinned, select the pinned port intelligently */
            if (state.pinned_port.empty())
            {
                /* Goal: Minimize total disabled ports
                   Strategy:
                   1. If there's a port already disabled by another bad MAC, keep it disabled (don't pin to it)
                   2. Pin to a port that is NOT yet disabled (keeps more ports UP overall)
                   3. If all ports already disabled, pick any (rare edge case)
                */

                string pinned_candidate;
                string already_disabled_candidate;

                for (const auto &port_entry : state.ports_seen)
                {
                    const string &port = port_entry.first;

                    /* Check if this port is already disabled by another bad MAC */
                    if (m_disabledPorts.find(port) != m_disabledPorts.end())
                    {
                        /* This port is already disabled - save as candidate but don't prefer it for pinning */
                        already_disabled_candidate = port;
                    }
                    else
                    {
                        /* This port is NOT disabled - prefer it for pinning! */
                        pinned_candidate = port;
                        break;  /* Found a good candidate, use it */
                    }
                }

                /* Select pinned port based on what we found */
                if (!pinned_candidate.empty())
                {
                    /* Best case: Found a port that's not yet disabled - pin to it */
                    state.pinned_port = pinned_candidate;
                }
                else if (!already_disabled_candidate.empty())
                {
                    /* All ports are already disabled - pin to one of them */
                    state.pinned_port = already_disabled_candidate;
                    SWSS_LOG_WARN("MAC_MOVE_GUARD: All ports for bad MAC %s are already disabled by other bad MACs, "
                                 "pinning to %s",
                                 key.mac.to_string().c_str(), state.pinned_port.c_str());
                }
                else
                {
                    /* Fallback: Use current port (should not normally happen) */
                    state.pinned_port = portName;
                }

                SWSS_LOG_NOTICE("MAC_MOVE_GUARD: Pinning bad MAC %s to port %s (keeping this port UP)",
                               key.mac.to_string().c_str(), state.pinned_port.c_str());
            }

            /* Disable all ports EXCEPT the pinned port */
            for (const auto &port_entry : state.ports_seen)
            {
                const string &port = port_entry.first;

                if (port == state.pinned_port)
                {
                    /* Skip the pinned port - keep it UP */
                    continue;
                }

                /* Check if this port is already disabled */
                if (state.disabled_ports.count(port) > 0)
                {
                    /* Already disabled by this bad MAC */
                    continue;
                }

                /* Disable this port */
                auto &bad_macs_on_port = m_disabledPorts[port];
                bool first_bad_mac = bad_macs_on_port.empty();

                if (first_bad_mac)
                {
                    /* First bad MAC on this port - actually disable it */
                    if (!m_portsOrch->setPortAdminStatusByAlias(port, false))
                    {
                        SWSS_LOG_ERROR("MAC_MOVE_GUARD: failed to disable port %s", port.c_str());
                        m_disabledPorts.erase(port);
                        continue;
                    }
                    SWSS_LOG_NOTICE("MAC_MOVE_GUARD: port %s administratively disabled (bad MAC %s pinned to %s)",
                                   port.c_str(), key.mac.to_string().c_str(), state.pinned_port.c_str());
                }

                /* Track this port as disabled by this bad MAC */
                bad_macs_on_port.insert(key);
                state.disabled_ports.insert(port);
            }
            break;
        }

        case MacMoveGuardAction::DISABLE_MAC_MOVE:
        {
            if (!m_portsOrch || !m_fdbOrch)
            {
                SWSS_LOG_ERROR("MAC_MOVE_GUARD: PortsOrch or FdbOrch not available");
                return;
            }

            /* Program the MAC as a static FDB entry with ALLOW_MAC_MOVE=false
               This prevents the MAC from moving to other ports */

            /* Get VLAN port object */
            Port vlan;
            if (!m_portsOrch->getPort(key.bv_id, vlan))
            {
                SWSS_LOG_ERROR("MAC_MOVE_GUARD: Failed to get VLAN port for bv_id 0x%" PRIx64, key.bv_id);
                return;
            }

            /* Select a port to program the static entry on
               Use the most recently seen port (state.last_port) */
            Port port;
            if (!m_portsOrch->getPort(state.last_port, port))
            {
                SWSS_LOG_ERROR("MAC_MOVE_GUARD: Failed to get port %s", state.last_port.c_str());
                return;
            }

            /* Check if bridge port ID is available */
            if (port.m_bridge_port_id == SAI_NULL_OBJECT_ID)
            {
                SWSS_LOG_ERROR("MAC_MOVE_GUARD: Bridge port ID not available for port %s", port.m_alias.c_str());
                return;
            }

            /* Update the existing FDB entry to static with ALLOW_MAC_MOVE=false
               The MAC has already been learned, so the FDB entry always exists */
            sai_status_t status;
            sai_fdb_entry_t fdb_entry;
            fdb_entry.switch_id = gSwitchId;
            memcpy(fdb_entry.mac_address, key.mac.getMac(), sizeof(sai_mac_t));
            fdb_entry.bv_id = key.bv_id;

            sai_attribute_t attr;

            /* First, update the bridge port ID to the current port */
            attr.id = SAI_FDB_ENTRY_ATTR_BRIDGE_PORT_ID;
            attr.value.oid = port.m_bridge_port_id;
            status = sai_fdb_api->set_fdb_entry_attribute(&fdb_entry, &attr);
            if (status != SAI_STATUS_SUCCESS)
            {
                SWSS_LOG_ERROR("MAC_MOVE_GUARD: Failed to update bridge port for MAC %s to port %s, rv:%d",
                              key.mac.to_string().c_str(), port.m_alias.c_str(), status);
                return;
            }

            /* Then, change type to static */
            attr.id = SAI_FDB_ENTRY_ATTR_TYPE;
            attr.value.s32 = SAI_FDB_ENTRY_TYPE_STATIC;
            status = sai_fdb_api->set_fdb_entry_attribute(&fdb_entry, &attr);
            if (status != SAI_STATUS_SUCCESS)
            {
                SWSS_LOG_ERROR("MAC_MOVE_GUARD: Failed to set FDB entry type to static for MAC %s, rv:%d",
                              key.mac.to_string().c_str(), status);
                return;
            }

            /* Finally, disable MAC move */
            attr.id = SAI_FDB_ENTRY_ATTR_ALLOW_MAC_MOVE;
            attr.value.booldata = false;
            status = sai_fdb_api->set_fdb_entry_attribute(&fdb_entry, &attr);
            if (status != SAI_STATUS_SUCCESS)
            {
                SWSS_LOG_ERROR("MAC_MOVE_GUARD: Failed to set ALLOW_MAC_MOVE to false for MAC %s, rv:%d",
                              key.mac.to_string().c_str(), status);
                return;
            }

            SWSS_LOG_NOTICE("MAC_MOVE_GUARD: Updated FDB entry to static (ALLOW_MAC_MOVE=false) for bad MAC %s on port %s",
                           key.mac.to_string().c_str(), port.m_alias.c_str());

            /* Store the bridge_port_id for cleanup later */
            state.static_fdb_bridge_port_id = port.m_bridge_port_id;
            break;
        }

        /* Future actions can be added here as new case statements */
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

    /* Release MAC from bad MAC tracking */
    state.is_bad_mac = false;

    /* Perform action-specific cleanup */
    switch (m_action)
    {
        case MacMoveGuardAction::DISABLE_PORT:
        {
            /* Re-enable all ports that were disabled for this bad MAC */
            for (const string &port : state.disabled_ports)
            {
                auto port_it = m_disabledPorts.find(port);
                if (port_it != m_disabledPorts.end())
                {
                    /* Remove this bad MAC from the port's tracking */
                    port_it->second.erase(key);

                    /* If no other bad MACs require this port to be disabled, re-enable it */
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

            /* Clear the disabled ports list and pinned port */
            state.disabled_ports.clear();
            state.pinned_port.clear();
            break;
        }

        case MacMoveGuardAction::DISABLE_MAC_MOVE:
        {
            /* Convert the static FDB entry back to dynamic to allow the MAC to move again */
            if (state.static_fdb_bridge_port_id == SAI_NULL_OBJECT_ID)
            {
                /* No static FDB entry was programmed */
                break;
            }

            /* Create the FDB entry structure */
            sai_status_t status;
            sai_fdb_entry_t fdb_entry;
            fdb_entry.switch_id = gSwitchId;
            memcpy(fdb_entry.mac_address, key.mac.getMac(), sizeof(sai_mac_t));
            fdb_entry.bv_id = key.bv_id;

            /* Convert back to dynamic FDB entry with ALLOW_MAC_MOVE=true */
            sai_attribute_t attr;

            /* First, set type back to dynamic */
            attr.id = SAI_FDB_ENTRY_ATTR_TYPE;
            attr.value.s32 = SAI_FDB_ENTRY_TYPE_DYNAMIC;
            status = sai_fdb_api->set_fdb_entry_attribute(&fdb_entry, &attr);
            if (status != SAI_STATUS_SUCCESS)
            {
                SWSS_LOG_ERROR("MAC_MOVE_GUARD: Failed to set FDB entry type to dynamic for MAC %s, rv:%d",
                              key.mac.to_string().c_str(), status);
            }

            /* Then, re-enable MAC move */
            attr.id = SAI_FDB_ENTRY_ATTR_ALLOW_MAC_MOVE;
            attr.value.booldata = true;
            status = sai_fdb_api->set_fdb_entry_attribute(&fdb_entry, &attr);
            if (status != SAI_STATUS_SUCCESS)
            {
                SWSS_LOG_ERROR("MAC_MOVE_GUARD: Failed to set ALLOW_MAC_MOVE to true for MAC %s, rv:%d",
                              key.mac.to_string().c_str(), status);
            }
            else
            {
                SWSS_LOG_NOTICE("MAC_MOVE_GUARD: Converted static FDB entry back to dynamic for MAC %s (action_interval expired)",
                               key.mac.to_string().c_str());
            }

            /* Clear the stored bridge_port_id */
            state.static_fdb_bridge_port_id = SAI_NULL_OBJECT_ID;
            break;
        }

        /* Future actions can be added here as new case statements */
        default:
            break;
    }
}

void MacMoveGuardOrch::checkRecovery()
{
    SWSS_LOG_ENTER();

    auto now = steady_clock::now();

    for (auto it = m_macTrackingState.begin(); it != m_macTrackingState.end(); )
    {
        MacMoveTrackingState &state = it->second;
        const MacKey &key = it->first;

        /* Always prune detection window so it stays correct */
        pruneWindow(state);

        if (!state.is_bad_mac)
        {
            /* If the MAC has gone quiet (no ports in detection window) and not a bad MAC,
               drop the entry to avoid unbounded memory growth */
            if (state.ports_seen.empty())
            {
                it = m_macTrackingState.erase(it);
                continue;
            }
            ++it;
            continue;
        }

        /* Check if action_interval has expired for this bad MAC */
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
