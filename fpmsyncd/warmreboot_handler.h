/*
 * Pure decision logic for the FPMSYNCD_RESTARTCHECK handler and the drain
 * auto-resume timer. Inputs only; outputs describe side effects (timer,
 * drain flag, STATE_DB, reply, forceReconnect). fpmsyncd.cpp applies them.
 * Split this way so the handler is unit-testable without Selectable /
 * FpmLink / live Redis.
 */

#pragma once

#include "table.h"  // for swss::FieldValueTuple

#include <cstddef>
#include <string>
#include <vector>

namespace fpmsyncd_warmreboot {

/*
 * Parsed notification payload from fpmsyncd_restart_check.
 *   autoResumeSec   — per-call override of the auto-resume timer. 0 means
 *                     "use the server-side default".
 *   resumeRequested — "resume"=="true" in payload: exit drain now.
 */
struct RestartCheckRequest
{
    int  autoResumeSec    = 0;
    bool resumeRequested  = false;
};

RestartCheckRequest parseRestartCheckValues(
    const std::vector<swss::FieldValueTuple>& values);

/*
 * Side effects fpmsyncd.cpp applies after the handler returns.
 *   drainFlag      — set / clear / no change (RouteSync::setDrainingForWarmRestart)
 *   timerAction    — arm-first / re-arm / disarm / none (drainAutoResumeTimer)
 *   drainState     — "draining"/"ready"/"resumed"/"auto_resumed"; empty = no write
 *   replyOp        — "READY" / "NOT_READY" / "RESUMED"
 *   forceReconnect — call forceDisconnect() + throw FpmConnectionClosedException
 */
struct RestartCheckOutcome
{
    enum class DrainFlag   { NoChange, Set, Clear };
    enum class TimerAction { None, ArmFirst, ReArm, Disarm };

    DrainFlag   drainFlag     = DrainFlag::NoChange;
    TimerAction timerAction   = TimerAction::None;
    int         autoResumeSec = 0;        // used iff ArmFirst / ReArm

    std::string drainState;
    size_t      drainQueueSize = 0;

    std::string                        replyOp;
    std::vector<swss::FieldValueTuple> replyValues;

    bool forceReconnect = false;
};

RestartCheckOutcome handleRestartCheck(
    const RestartCheckRequest& req,
    bool                       hasZmq,
    size_t                     queueSize,
    bool                       currentlyDraining,
    int                        defaultAutoResumeSec);

/* Auto-resume timer-fire outcome (fixed shape). */
struct TimerFireOutcome
{
    bool        clearDrainFlag = true;
    bool        disarmTimer    = true;
    std::string drainState     = "auto_resumed";
    size_t      drainQueueSize = 0;
    bool        forceReconnect = true;
};

TimerFireOutcome handleDrainAutoResumeTimerFire(size_t queueSize);

// ---------- Startup helpers ----------

/*
 * Result of parsing CONFIG_DB fpmsyncd_drain_auto_resume_sec. `Source`
 * lets the caller emit the matching syslog line per branch.
 */
struct DrainAutoResumeSecParse
{
    enum class Source
    {
        Default,      // empty / "None"
        ConfigDb,     // valid in-range int
        OutOfRange,   // valid int outside [minSec, maxSec]
        Invalid       // not a clean integer
    };

    int    value      = 0;
    int    rawParsed  = 0;
    Source source     = Source::Default;
};

/* Strict parser: rejects trailing junk; catches std::stoi exceptions. */
DrainAutoResumeSecParse parseDrainAutoResumeSec(
    const std::string& raw,
    int                defaultSec,
    int                minSec = 1,
    int                maxSec = 600);

/* hget + parseDrainAutoResumeSec. Returns effective seconds. */
int loadDrainAutoResumeSecFromConfigDb(
    swss::Table& deviceMetadataTable,
    int          defaultSec,
    int          minSec = 1,
    int          maxSec = 600);

/* HDEL drain_state / drain_queue_size / legacy queueSize from
 * WARM_RESTART_TABLE|fpmsyncd. Idempotent. */
void clearStaleDrainStateFields(swss::Table& warmRestartStateTable);

}  // namespace fpmsyncd_warmreboot
