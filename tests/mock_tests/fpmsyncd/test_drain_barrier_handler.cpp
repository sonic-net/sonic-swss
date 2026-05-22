/*
 * Unit tests: fpmsyncd_warmreboot::parseRestartCheckValues,
 *             fpmsyncd_warmreboot::handleRestartCheck,
 *             fpmsyncd_warmreboot::handleDrainAutoResumeTimerFire
 *
 * These are pure decision functions (no DB, no Selectable, no FpmLink) so
 * the tests directly construct inputs and assert on the returned outcome.
 *
 * Branch coverage we aim for:
 *
 *   parseRestartCheckValues
 *     - empty values
 *     - autoResumeTimeoutSec positive  → req.autoResumeSec set
 *     - autoResumeTimeoutSec zero      → req.autoResumeSec stays 0
 *     - autoResumeTimeoutSec negative  → req.autoResumeSec stays 0
 *     - autoResumeTimeoutSec garbage   → req.autoResumeSec stays 0
 *     - resume == "true"               → req.resumeRequested = true
 *     - resume == "false"              → req.resumeRequested = false
 *     - resume == "garbage"            → req.resumeRequested = false
 *     - unknown fields are ignored (forward compatibility)
 *     - both fields set together
 *
 *   handleRestartCheck — normal restart-check path
 *     - !hasZmq, qsize==0           → READY, drainState=ready, no flag/timer
 *     - hasZmq, !draining, qsize==0 → READY, Set + ArmFirst
 *     - hasZmq, !draining, qsize>0  → NOT_READY, Set + ArmFirst, drainState=draining
 *     - hasZmq, draining, qsize==0  → READY, ReArm (no Set)
 *     - hasZmq, draining, qsize>0   → NOT_READY, ReArm, drainState=draining
 *
 *   handleRestartCheck — autoResumeSec override
 *     - req.autoResumeSec > 0          → outcome.autoResumeSec == req value
 *     - req.autoResumeSec == 0         → outcome.autoResumeSec == default
 *
 *   handleRestartCheck — resume path
 *     - resumeRequested + !draining    → RESUMED, no flag, no timer, no STATE_DB,
 *                                        was_draining=false, resumed=false,
 *                                        forceReconnect=false
 *     - resumeRequested + draining     → RESUMED, Clear + Disarm, drainState=resumed,
 *                                        was_draining=true, resumed=true,
 *                                        forceReconnect=true
 *     - resumeRequested overrides hasZmq/qsize semantics
 *
 *   handleDrainAutoResumeTimerFire
 *     - returns fixed outcome with given queueSize
 */

#include "fpmsyncd/warmreboot_handler.h"

#include "table.h"  // for swss::FieldValueTuple / fvField / fvValue

#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

using namespace fpmsyncd_warmreboot;
using swss::FieldValueTuple;

namespace {

// Helper: find a FieldValueTuple by name; returns "" if absent.
std::string findValue(const std::vector<FieldValueTuple>& fvs,
                      const std::string& field)
{
    auto it = std::find_if(fvs.begin(), fvs.end(), [&](const FieldValueTuple& fv) {
        return fvField(fv) == field;
    });
    return it == fvs.end() ? std::string{} : fvValue(*it);
}

constexpr int kDefaultAutoResumeSec = 30;

}  // namespace

// ============================================================
// parseRestartCheckValues
// ============================================================

TEST(ParseRestartCheckValues, EmptyValues)
{
    RestartCheckRequest req = parseRestartCheckValues({});
    EXPECT_EQ(0, req.autoResumeSec);
    EXPECT_FALSE(req.resumeRequested);
}

TEST(ParseRestartCheckValues, AutoResumeTimeoutSec_Positive_Honored)
{
    RestartCheckRequest req = parseRestartCheckValues({
        {"autoResumeTimeoutSec", "45"},
    });
    EXPECT_EQ(45, req.autoResumeSec);
    EXPECT_FALSE(req.resumeRequested);
}

TEST(ParseRestartCheckValues, AutoResumeTimeoutSec_Zero_Ignored)
{
    RestartCheckRequest req = parseRestartCheckValues({
        {"autoResumeTimeoutSec", "0"},
    });
    EXPECT_EQ(0, req.autoResumeSec)
        << "0 means 'no override' — caller should fall back to default";
}

TEST(ParseRestartCheckValues, AutoResumeTimeoutSec_Negative_Ignored)
{
    RestartCheckRequest req = parseRestartCheckValues({
        {"autoResumeTimeoutSec", "-5"},
    });
    EXPECT_EQ(0, req.autoResumeSec);
}

TEST(ParseRestartCheckValues, AutoResumeTimeoutSec_Garbage_Ignored)
{
    RestartCheckRequest req = parseRestartCheckValues({
        {"autoResumeTimeoutSec", "not_a_number"},
    });
    EXPECT_EQ(0, req.autoResumeSec);
}

TEST(ParseRestartCheckValues, Resume_True_Honored)
{
    RestartCheckRequest req = parseRestartCheckValues({
        {"resume", "true"},
    });
    EXPECT_TRUE(req.resumeRequested);
}

TEST(ParseRestartCheckValues, Resume_False_NotHonored)
{
    RestartCheckRequest req = parseRestartCheckValues({
        {"resume", "false"},
    });
    EXPECT_FALSE(req.resumeRequested);
}

TEST(ParseRestartCheckValues, Resume_Garbage_NotHonored)
{
    RestartCheckRequest req = parseRestartCheckValues({
        {"resume", "yes"},
    });
    EXPECT_FALSE(req.resumeRequested)
        << "only the exact literal 'true' triggers resume — anything else is a no";
}

TEST(ParseRestartCheckValues, UnknownFields_Ignored)
{
    RestartCheckRequest req = parseRestartCheckValues({
        {"some_future_field", "42"},
        {"another_extension",  "yes"},
    });
    EXPECT_EQ(0, req.autoResumeSec);
    EXPECT_FALSE(req.resumeRequested);
}

TEST(ParseRestartCheckValues, BothFields_BothHonored)
{
    RestartCheckRequest req = parseRestartCheckValues({
        {"autoResumeTimeoutSec", "60"},
        {"resume",               "true"},
    });
    EXPECT_EQ(60, req.autoResumeSec);
    EXPECT_TRUE(req.resumeRequested);
}

// ============================================================
// handleRestartCheck — normal (non-resume) path
// ============================================================

TEST(HandleRestartCheck, NoZmq_ReplyReady_NoFlag_NoTimer)
{
    // ZMQ off — qsize is always 0 in production (totalDbUpdaterQueueSize
    // returns 0 when no ZmqProducerStateTable is wired). Handler should
    // reply READY and write drain_state=ready, but NOT arm a timer or
    // flip the drain flag (there's nothing to drain).
    RestartCheckRequest req;
    RestartCheckOutcome out = handleRestartCheck(
        req, /*hasZmq=*/false, /*qsize=*/0,
        /*currentlyDraining=*/false, kDefaultAutoResumeSec);

    EXPECT_EQ(RestartCheckOutcome::DrainFlag::NoChange, out.drainFlag);
    EXPECT_EQ(RestartCheckOutcome::TimerAction::None,   out.timerAction);
    EXPECT_EQ("ready",                                  out.drainState);
    EXPECT_EQ(0u,                                       out.drainQueueSize);
    EXPECT_EQ("READY",                                  out.replyOp);
    EXPECT_FALSE(out.forceReconnect);
    EXPECT_EQ("0", findValue(out.replyValues, "queueSize"));
}

TEST(HandleRestartCheck, Zmq_NotDraining_QueueEmpty_ReadyAndArmFirst)
{
    // First restart-check while ZMQ is on and nothing's pending. Handler
    // still enters drain mode + arms the auto-resume timer (the warm-
    // reboot script might restart fpmsyncd within the next few seconds),
    // and replies READY.
    RestartCheckRequest req;
    RestartCheckOutcome out = handleRestartCheck(
        req, /*hasZmq=*/true, /*qsize=*/0,
        /*currentlyDraining=*/false, kDefaultAutoResumeSec);

    EXPECT_EQ(RestartCheckOutcome::DrainFlag::Set,      out.drainFlag);
    EXPECT_EQ(RestartCheckOutcome::TimerAction::ArmFirst, out.timerAction);
    EXPECT_EQ(kDefaultAutoResumeSec,                    out.autoResumeSec);
    EXPECT_EQ("ready",                                  out.drainState);
    EXPECT_EQ("READY",                                  out.replyOp);
    EXPECT_FALSE(out.forceReconnect);
}

TEST(HandleRestartCheck, Zmq_NotDraining_QueueNonZero_NotReadyAndArmFirst)
{
    RestartCheckRequest req;
    RestartCheckOutcome out = handleRestartCheck(
        req, /*hasZmq=*/true, /*qsize=*/17,
        /*currentlyDraining=*/false, kDefaultAutoResumeSec);

    EXPECT_EQ(RestartCheckOutcome::DrainFlag::Set,      out.drainFlag);
    EXPECT_EQ(RestartCheckOutcome::TimerAction::ArmFirst, out.timerAction);
    EXPECT_EQ("draining",                               out.drainState);
    EXPECT_EQ(17u,                                      out.drainQueueSize);
    EXPECT_EQ("NOT_READY",                              out.replyOp);
    EXPECT_EQ("17", findValue(out.replyValues, "queueSize"));
}

TEST(HandleRestartCheck, Zmq_Draining_QueueEmpty_ReadyAndReArm)
{
    // Re-notification during an active drain window with queue now drained
    // to zero. Reply READY (the tool's retry loop will see this and exit
    // success); also re-arm the timer so a fresh auto-resume window starts
    // from this point. (Don't Set the flag — we're already draining.)
    RestartCheckRequest req;
    RestartCheckOutcome out = handleRestartCheck(
        req, /*hasZmq=*/true, /*qsize=*/0,
        /*currentlyDraining=*/true, kDefaultAutoResumeSec);

    EXPECT_EQ(RestartCheckOutcome::DrainFlag::NoChange, out.drainFlag);
    EXPECT_EQ(RestartCheckOutcome::TimerAction::ReArm,  out.timerAction);
    EXPECT_EQ(kDefaultAutoResumeSec,                    out.autoResumeSec);
    EXPECT_EQ("ready",                                  out.drainState);
    EXPECT_EQ("READY",                                  out.replyOp);
}

TEST(HandleRestartCheck, Zmq_Draining_QueueNonZero_NotReadyAndReArm)
{
    RestartCheckRequest req;
    RestartCheckOutcome out = handleRestartCheck(
        req, /*hasZmq=*/true, /*qsize=*/5,
        /*currentlyDraining=*/true, kDefaultAutoResumeSec);

    EXPECT_EQ(RestartCheckOutcome::DrainFlag::NoChange, out.drainFlag);
    EXPECT_EQ(RestartCheckOutcome::TimerAction::ReArm,  out.timerAction);
    EXPECT_EQ("draining",                               out.drainState);
    EXPECT_EQ(5u,                                       out.drainQueueSize);
    EXPECT_EQ("NOT_READY",                              out.replyOp);
}

// ---------- autoResumeSec override ----------

TEST(HandleRestartCheck, AutoResumeSecOverride_PositiveValueWins)
{
    RestartCheckRequest req;
    req.autoResumeSec = 90;
    RestartCheckOutcome out = handleRestartCheck(
        req, /*hasZmq=*/true, /*qsize=*/3,
        /*currentlyDraining=*/false, kDefaultAutoResumeSec);

    EXPECT_EQ(90, out.autoResumeSec)
        << "caller's per-call override should beat the default";
}

TEST(HandleRestartCheck, AutoResumeSecOverride_ZeroFallsBackToDefault)
{
    RestartCheckRequest req;
    req.autoResumeSec = 0;
    RestartCheckOutcome out = handleRestartCheck(
        req, /*hasZmq=*/true, /*qsize=*/3,
        /*currentlyDraining=*/false, kDefaultAutoResumeSec);

    EXPECT_EQ(kDefaultAutoResumeSec, out.autoResumeSec);
}

// ============================================================
// handleRestartCheck — resume path
// ============================================================

TEST(HandleRestartCheck_Resume, NotDraining_FullNoOpExceptReply)
{
    // Resume requested but not currently draining — the handler must
    // still reply RESUMED so the caller's exit-0 check works, but it
    // must NOT clear a non-existent drain flag, touch STATE_DB, or
    // force-disconnect a healthy FPM session.
    RestartCheckRequest req;
    req.resumeRequested = true;
    RestartCheckOutcome out = handleRestartCheck(
        req, /*hasZmq=*/true, /*qsize=*/0,
        /*currentlyDraining=*/false, kDefaultAutoResumeSec);

    EXPECT_EQ(RestartCheckOutcome::DrainFlag::NoChange, out.drainFlag);
    EXPECT_EQ(RestartCheckOutcome::TimerAction::None,   out.timerAction);
    EXPECT_TRUE(out.drainState.empty())
        << "no-op resume must NOT overwrite the existing STATE_DB row";
    EXPECT_FALSE(out.forceReconnect)
        << "no-op resume must NOT force-disconnect a healthy FPM connection";
    EXPECT_EQ("RESUMED", out.replyOp);
    EXPECT_EQ("false",   findValue(out.replyValues, "was_draining"));
    EXPECT_EQ("false",   findValue(out.replyValues, "resumed"));
    EXPECT_EQ("0",       findValue(out.replyValues, "queueSize"));
}

TEST(HandleRestartCheck_Resume, Draining_FullResumeSequence)
{
    RestartCheckRequest req;
    req.resumeRequested = true;
    RestartCheckOutcome out = handleRestartCheck(
        req, /*hasZmq=*/true, /*qsize=*/7,
        /*currentlyDraining=*/true, kDefaultAutoResumeSec);

    EXPECT_EQ(RestartCheckOutcome::DrainFlag::Clear,    out.drainFlag);
    EXPECT_EQ(RestartCheckOutcome::TimerAction::Disarm, out.timerAction);
    EXPECT_EQ("resumed",                                out.drainState);
    EXPECT_EQ(7u,                                       out.drainQueueSize);
    EXPECT_TRUE(out.forceReconnect);
    EXPECT_EQ("RESUMED", out.replyOp);
    EXPECT_EQ("true",    findValue(out.replyValues, "was_draining"));
    EXPECT_EQ("true",    findValue(out.replyValues, "resumed"));
    EXPECT_EQ("7",       findValue(out.replyValues, "queueSize"));
}

TEST(HandleRestartCheck_Resume, NotDraining_HasZmqIrrelevant)
{
    // The "not in drain mode" no-op must hold regardless of hasZmq —
    // it's a property of the drain flag, not of the ZMQ wiring.
    RestartCheckRequest req;
    req.resumeRequested = true;

    RestartCheckOutcome outZmqOn = handleRestartCheck(
        req, /*hasZmq=*/true, /*qsize=*/0, /*draining=*/false, kDefaultAutoResumeSec);
    RestartCheckOutcome outZmqOff = handleRestartCheck(
        req, /*hasZmq=*/false, /*qsize=*/0, /*draining=*/false, kDefaultAutoResumeSec);

    EXPECT_EQ("RESUMED", outZmqOn.replyOp);
    EXPECT_EQ("RESUMED", outZmqOff.replyOp);
    EXPECT_FALSE(outZmqOn.forceReconnect);
    EXPECT_FALSE(outZmqOff.forceReconnect);
}

// ============================================================
// handleDrainAutoResumeTimerFire
// ============================================================

TEST(HandleDrainAutoResumeTimerFire, FixedShape_QueueSizeForwarded)
{
    TimerFireOutcome out = handleDrainAutoResumeTimerFire(42);

    EXPECT_TRUE(out.clearDrainFlag);
    EXPECT_TRUE(out.disarmTimer);
    EXPECT_EQ("auto_resumed", out.drainState);
    EXPECT_EQ(42u,            out.drainQueueSize);
    EXPECT_TRUE(out.forceReconnect);
}

TEST(HandleDrainAutoResumeTimerFire, QueueSizeZero)
{
    // The timer-fire path doesn't care whether the queue is empty —
    // semantics are "we waited long enough, force reconnect now".
    TimerFireOutcome out = handleDrainAutoResumeTimerFire(0);

    EXPECT_TRUE(out.clearDrainFlag);
    EXPECT_TRUE(out.forceReconnect);
    EXPECT_EQ(0u, out.drainQueueSize);
}
