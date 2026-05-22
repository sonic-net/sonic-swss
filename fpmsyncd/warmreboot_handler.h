/*
 * Warm-reboot drain-barrier handler: pure decision logic for the
 * FPMSYNCD_RESTARTCHECK notification and the drain-auto-resume timer fire.
 *
 * Design intent
 * -------------
 * fpmsyncd's main select loop is awkward to unit-test directly — it owns
 * a real Selectable timer, a real FpmLink, a NotificationProducer, etc.,
 * and the natural test boundary (the FPMSYNCD_RESTARTCHECK case in the
 * switch-on-Selectable-fired) is wedged inside ~50 lines of imperative
 * "apply side effects" code.
 *
 * Rather than mock the timer / FpmLink / NotificationProducer / Table,
 * we split the handler in two:
 *
 *   1. Pure decision functions (this header). Inputs only — parsed
 *      request, current drain flag, queue size, default auto-resume sec.
 *      Output is a *description* of side effects: timer ArmFirst/ReArm/
 *      Disarm, STATE_DB row to write, reply payload, force-reconnect
 *      flag. No DB I/O, no timer manipulation, no throw.
 *
 *   2. fpmsyncd.cpp applies the outcome. Calls `parseRestartCheckValues`
 *      and `handleRestartCheck`, then runs the small "apply" block that
 *      arms timers, writes STATE_DB, sends the reply, and (if asked)
 *      throws FpmConnectionClosedException to drive the reconnect chain.
 *
 * The split keeps the decision logic verifiable in isolation while
 * leaving the imperative bits short enough to eyeball.
 */

#pragma once

#include "table.h"  // for swss::FieldValueTuple

#include <cstddef>
#include <string>
#include <vector>

namespace fpmsyncd_warmreboot {

/*
 * Parsed inputs from an FPMSYNCD_RESTARTCHECK notification's values vector.
 * The tool fpmsyncd_restart_check sends optional fields:
 *   autoResumeTimeoutSec — per-call override of the drain auto-resume
 *     timer. Caller-supplied positive int "wins" over the CONFIG_DB /
 *     compile-time default. <=0 or absent → use default.
 *   resume — "true" means "exit drain immediately, force FPM reconnect".
 *     Used by the warm-reboot abort cleanup path and by the tool's
 *     -R/--resume flag.
 */
struct RestartCheckRequest
{
    int  autoResumeSec    = 0;     // 0 means "no override; use default"
    bool resumeRequested  = false; // true iff "resume"=="true" in payload
};

RestartCheckRequest parseRestartCheckValues(
    const std::vector<swss::FieldValueTuple>& values);

/*
 * Description of side effects the caller must apply after the handler runs.
 *
 * Each enum'd action maps 1:1 to a small block in fpmsyncd.cpp's select
 * loop. Keeping the state-mutation and I/O out of this struct means tests
 * can simply construct inputs and assert on the returned outcome — no
 * Selectable, no FpmLink, no live Redis.
 */
struct RestartCheckOutcome
{
    // Drain flag change. Caller applies via RouteSync::setDrainingForWarmRestart.
    enum class DrainFlag
    {
        NoChange,
        Set,    // enter drain mode (new route SET/DEL get gated)
        Clear   // exit drain mode (resume / auto-resume; route flow resumes)
    };
    DrainFlag drainFlag = DrainFlag::NoChange;

    // Auto-resume timer action. Caller applies on drainAutoResumeTimer.
    //   ArmFirst — setInterval + reset + start + addSelectable(timer).
    //              Used when entering drain mode for the first time.
    //   ReArm    — setInterval + reset + start (already in select set).
    //              Used on re-notification while already draining.
    //   Disarm   — removeSelectable(timer).
    //              Used on explicit / auto resume.
    //   None     — no timer change.
    enum class TimerAction
    {
        None,
        ArmFirst,
        ReArm,
        Disarm
    };
    TimerAction timerAction   = TimerAction::None;
    int         autoResumeSec = 0;        // valid iff timerAction is ArmFirst or ReArm

    // STATE_DB|WARM_RESTART_TABLE|fpmsyncd row update. drainState empty
    // means "do not write" (the handler may choose not to disturb the
    // existing row in some no-op cases, but the standard reply paths
    // always write).
    std::string drainState;               // "draining"/"ready"/"resumed"/"auto_resumed"/""
    size_t      drainQueueSize = 0;

    // Reply to send on FPMSYNCD_RESTARTCHECKREPLY.
    //   "READY"     — qsize == 0, ok to proceed with warm reboot
    //   "NOT_READY" — qsize > 0, still draining
    //   "RESUMED"   — answer to a resume request; payload has was_draining/resumed flags
    std::string                        replyOp;
    std::vector<swss::FieldValueTuple> replyValues;

    // After replying, fpmsyncd.cpp will call fpm.forceDisconnect() and
    // throw FpmConnectionClosedException to enter the outer-catch
    // reconnect chain (zebra re-dumps its full FIB on the new connection).
    bool forceReconnect = false;
};

RestartCheckOutcome handleRestartCheck(
    const RestartCheckRequest& req,
    bool                       hasZmq,
    size_t                     queueSize,
    bool                       currentlyDraining,
    int                        defaultAutoResumeSec);

/*
 * Outcome of the drain-auto-resume timer firing. This case is fixed —
 * the only inputs are "did the timer fire" and the current queue size
 * (purely for the STATE_DB drain_queue_size column). The outcome is
 * always the same shape, so a struct keeps the shape consistent with
 * RestartCheckOutcome for callers that want to apply both via a single
 * code path.
 */
struct TimerFireOutcome
{
    bool        clearDrainFlag = true;   // always true
    bool        disarmTimer    = true;   // always true (remove from select set)
    std::string drainState     = "auto_resumed";
    size_t      drainQueueSize = 0;
    bool        forceReconnect = true;   // always true
};

TimerFireOutcome handleDrainAutoResumeTimerFire(size_t queueSize);

// ====================================================================
// Startup helpers: CONFIG_DB read + STATE_DB cleanup
// ====================================================================
//
// fpmsyncd's main() runs two warm-reboot-related setup steps before the
// select loop:
//   1. Read fpmsyncd_drain_auto_resume_sec from CONFIG_DB DEVICE_METADATA
//      to override the compile-time default.
//   2. HDEL stale drain_state / drain_queue_size / queueSize from
//      STATE_DB WARM_RESTART_TABLE|fpmsyncd. (Stale data is possible if
//      the previous fpmsyncd was killed mid-warm-reboot; the new
//      fpmsyncd is not actually in any drain phase, so the row is a lie
//      until cleaned.)
//
// Both are extracted here so they can be unit-tested without standing
// up a real fpmsyncd process.

/*
 * Outcome of parsing a CONFIG_DB string value for fpmsyncd_drain_auto_resume_sec.
 *
 * Used by parseDrainAutoResumeSec and loadDrainAutoResumeSecFromConfigDb.
 * Source describes which branch the value came from — useful for the
 * "wrote what to syslog" assertion in tests.
 */
struct DrainAutoResumeSecParse
{
    enum class Source
    {
        Default,      // raw was empty / "None" — fell back to default
        ConfigDb,     // raw was a valid in-range int — used it
        OutOfRange,   // raw was a valid int but outside [minSec, maxSec] — fell back
        Invalid       // raw was not parseable as an int — fell back
    };

    int    value      = 0;                    // effective seconds (default or parsed)
    int    rawParsed  = 0;                    // the int we actually parsed out of raw (0 if Invalid/Default)
    Source source     = Source::Default;
};

/*
 * Pure CONFIG_DB-string parser for fpmsyncd_drain_auto_resume_sec.
 *
 * Empty string or literal "None" → Default. (sonic-cfggen materializes
 * absent CONFIG_DB fields as Python None — when subsequently rendered to
 * text, that becomes the literal string "None". Treat both as "no value
 * configured".)
 *
 * Otherwise: std::stoi the string. If it throws → Invalid. If the result
 * is outside [minSec, maxSec] → OutOfRange. Else → ConfigDb.
 *
 * In all "fell back" branches, .value == defaultSec.
 *
 * minSec/maxSec are explicit (1, 600 in production) so tests can pin
 * the range edges without dragging in the rest of fpmsyncd.
 */
DrainAutoResumeSecParse parseDrainAutoResumeSec(
    const std::string& raw,
    int                defaultSec,
    int                minSec = 1,
    int                maxSec = 600);

/*
 * Thin wrapper over parseDrainAutoResumeSec that does the actual hget.
 * Returns the effective seconds (parse.value). This is the function
 * fpmsyncd.cpp main() calls.
 *
 * Logging is the caller's responsibility — we return the structured
 * parse result via parseDrainAutoResumeSec for tests, but the wrapper
 * just returns the int because the caller has different logging needs
 * per Source.
 */
int loadDrainAutoResumeSecFromConfigDb(
    swss::Table& deviceMetadataTable,
    int          defaultSec,
    int          minSec = 1,
    int          maxSec = 600);

/*
 * Clear stale drain-barrier visibility fields from
 * STATE_DB|WARM_RESTART_TABLE|fpmsyncd at fpmsyncd startup.
 *
 * Removes three fields:
 *   drain_state       (e.g. "draining" / "ready" / "auto_resumed" / "resumed")
 *   drain_queue_size  (numeric, last observed AsyncDBUpdater queue size)
 *   queueSize         (legacy field name from an earlier iteration; cleaned
 *                      defensively in case the prior fpmsyncd wrote it)
 *
 * Idempotent — hdel is a no-op for absent fields.
 */
void clearStaleDrainStateFields(swss::Table& warmRestartStateTable);

}  // namespace fpmsyncd_warmreboot
