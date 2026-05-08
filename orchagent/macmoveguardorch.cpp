#include "macmoveguardorch.h"
#include "fdborch.h"
#include "logger.h"
#include "tokenize.h"
#include "notifier.h"

#include <chrono>
#include <inttypes.h>

extern FdbOrch *gFdbOrch;

using namespace std;
using namespace std::chrono;

/* Out-of-class definitions for the in-class-initialized static const members.
   Required when these constants are ODR-used (e.g. passed to functions that
   take their value by const reference, like the chrono duration constructors). */
const int MacMoveGuardOrch::RECOVERY_CHECK_INTERVAL_SECS;

MacMoveGuardOrch::MacMoveGuardOrch(DBConnector *configDb, const string &tableName,
                                   PortsOrch *portsOrch) :
    Orch(configDb, tableName),
    m_portsOrch(portsOrch)
{
    SWSS_LOG_ENTER();

    /* Subscribe to FDB changes from FdbOrch to receive MAC move notifications */
    if (gFdbOrch)
    {
        gFdbOrch->attach(this);
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
    if (gFdbOrch)
    {
        gFdbOrch->detach(this);
    }
}

void MacMoveGuardOrch::clearAllState()
{
    /* Re-enable any ports we have administratively disabled */
    for (auto &kv : m_disabledPortFlows)
    {
        if (m_portsOrch)
        {
            m_portsOrch->setPortAdminStatusByAlias(kv.first, true);
        }
    }
    m_disabledPortFlows.clear();
    m_flowState.clear();
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
                    else if (field == "duration")
                    {
                        m_durationSeconds = static_cast<uint32_t>(stoul(value));
                    }
                    else if (field == "recovery_time")
                    {
                        m_recoverySeconds = static_cast<uint32_t>(stoul(value));
                    }
                    else if (field == "action")
                    {
                        if (value != "disable_port")
                        {
                            SWSS_LOG_WARN("MAC_MOVE_GUARD: only 'disable_port' action is "
                                          "supported (got %s)", value.c_str());
                        }
                    }
                }
                catch (const exception &e)
                {
                    SWSS_LOG_ERROR("MAC_MOVE_GUARD: invalid value '%s' for field '%s': %s",
                                   value.c_str(), field.c_str(), e.what());
                }
            }

            SWSS_LOG_NOTICE("MAC_MOVE_GUARD config: enabled=%s threshold=%u duration=%us recovery_time=%us",
                            m_enabled ? "true" : "false",
                            m_threshold, m_durationSeconds, m_recoverySeconds);

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

    /* TEMP DIAG: confirm the recovery timer is firing. If you don't see this
       every RECOVERY_CHECK_INTERVAL_SECS, the SelectableTimer's executor is
       not being driven by the main loop. */
    SWSS_LOG_NOTICE("MAC_MOVE_GUARD: timer tick, enabled=%s flows=%zu disabled_ports=%zu learnt=%zu",
                    m_enabled ? "true" : "false",
                    m_flowState.size(),
                    m_disabledPortFlows.size(),
                    m_learntMac.size());

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

void MacMoveGuardOrch::pruneWindow(MacFlowState &state)
{
    auto cutoff = steady_clock::now() - seconds(m_durationSeconds);
    while (!state.move_timestamps.empty() && state.move_timestamps.front() < cutoff)
    {
        state.move_timestamps.pop_front();
    }
}

void MacMoveGuardOrch::handleMacLearn(const MacLearnNotification &notif)
{
    SWSS_LOG_ENTER();

    if (notif.port.m_alias.empty())
    {
        return;
    }

    MacFlowKey key{ notif.mac, notif.bv_id };

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

    MacFlowKey key{ notif.mac, notif.bv_id };

    /* Keep the learnt-MAC cache in sync for native MOVE events. (For moves
       synthesized from LEARN, handleMacLearn has already updated this.) */
    m_learntMac[key] = new_alias;

    MacFlowState &state = m_flowState[key];

    auto now = steady_clock::now();
    state.move_timestamps.push_back(now);
    pruneWindow(state);

    SWSS_LOG_INFO("MAC_MOVE_GUARD: flow (mac=%s bv_id=0x%" PRIx64 ") move count: %zu (threshold %u)",
                  notif.mac.to_string().c_str(), notif.bv_id,
                  state.move_timestamps.size(), m_threshold);

    if (state.move_timestamps.size() >= m_threshold && !state.port_disabled)
    {
        /* Action: disable the port on which the latest notification was received
           — i.e. the destination port of this MAC move. */
        SWSS_LOG_WARN("MAC_MOVE_GUARD: threshold exceeded for mac=%s on vlan_oid=0x%" PRIx64
                      "; disabling port %s",
                      notif.mac.to_string().c_str(), notif.bv_id, new_alias.c_str());

        disablePort(new_alias, key, state);
    }
}

void MacMoveGuardOrch::disablePort(const string &portName, const MacFlowKey &key, MacFlowState &state)
{
    SWSS_LOG_ENTER();

    if (!m_portsOrch)
    {
        SWSS_LOG_ERROR("MAC_MOVE_GUARD: PortsOrch not available, cannot disable %s",
                       portName.c_str());
        return;
    }

    auto &flows_on_port = m_disabledPortFlows[portName];
    bool first_flow = flows_on_port.empty();

    if (first_flow)
    {
        if (!m_portsOrch->setPortAdminStatusByAlias(portName, false))
        {
            SWSS_LOG_ERROR("MAC_MOVE_GUARD: failed to disable port %s", portName.c_str());
            /* Roll back the empty entry we just inserted into m_disabledPortFlows */
            m_disabledPortFlows.erase(portName);
            return;
        }
        SWSS_LOG_NOTICE("MAC_MOVE_GUARD: port %s administratively disabled", portName.c_str());
    }

    flows_on_port.insert(key);
    state.port_disabled = true;
    state.disabled_port_name = portName;
    state.disable_time = steady_clock::now();
}

void MacMoveGuardOrch::releaseFlowFromPort(const MacFlowKey &key, MacFlowState &state)
{
    SWSS_LOG_ENTER();

    if (!state.port_disabled)
    {
        return;
    }

    const string portName = state.disabled_port_name;

    auto port_it = m_disabledPortFlows.find(portName);
    if (port_it != m_disabledPortFlows.end())
    {
        port_it->second.erase(key);
        if (port_it->second.empty())
        {
            /* No flow is keeping this port down anymore — re-enable it. */
            if (m_portsOrch && !m_portsOrch->setPortAdminStatusByAlias(portName, true))
            {
                SWSS_LOG_ERROR("MAC_MOVE_GUARD: failed to re-enable port %s", portName.c_str());
            }
            else
            {
                SWSS_LOG_NOTICE("MAC_MOVE_GUARD: port %s administratively re-enabled",
                                portName.c_str());
            }
            m_disabledPortFlows.erase(port_it);
        }
    }

    state.port_disabled = false;
    state.disabled_port_name.clear();
}

void MacMoveGuardOrch::checkRecovery()
{
    SWSS_LOG_ENTER();

    auto now = steady_clock::now();
    auto recovery_dur = seconds(m_recoverySeconds);

    for (auto it = m_flowState.begin(); it != m_flowState.end(); )
    {
        MacFlowState &state = it->second;
        const MacFlowKey &key = it->first;

        /* Always prune so the window stays correct. */
        pruneWindow(state);

        if (!state.port_disabled)
        {
            /* If the flow has gone quiet for a while, drop the entry to avoid unbounded growth. */
            if (state.move_timestamps.empty())
            {
                it = m_flowState.erase(it);
                continue;
            }
            ++it;
            continue;
        }

        /* Flow currently holds a port disabled. Both conditions must hold to recover:
           1) recovery period has elapsed since disable time
           2) move count in the current window is below threshold */
        if (now - state.disable_time < recovery_dur)
        {
            ++it;
            continue;
        }

        if (state.move_timestamps.size() >= m_threshold)
        {
            ++it;
            continue;
        }

        releaseFlowFromPort(key, state);
        ++it;
    }
}
