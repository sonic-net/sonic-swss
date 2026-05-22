#include "warmreboot_handler.h"

#include <cstdlib>  // atoi

namespace fpmsyncd_warmreboot {

RestartCheckRequest parseRestartCheckValues(
    const std::vector<swss::FieldValueTuple>& values)
{
    RestartCheckRequest req;

    for (const auto& fv : values)
    {
        const std::string& field = fvField(fv);
        const std::string& value = fvValue(fv);

        if (field == "autoResumeTimeoutSec")
        {
            // Per-call override. Only honor positive values; anything else
            // (0, negative, garbage) leaves req.autoResumeSec at 0 and the
            // caller falls back to defaultAutoResumeSec.
            int parsed = std::atoi(value.c_str());
            if (parsed > 0)
            {
                req.autoResumeSec = parsed;
            }
        }
        else if (field == "resume" && value == "true")
        {
            req.resumeRequested = true;
        }
        // Unknown fields ignored — forward-compatibility with future
        // tool versions that add new optional fields.
    }

    return req;
}

RestartCheckOutcome handleRestartCheck(
    const RestartCheckRequest& req,
    bool                       hasZmq,
    size_t                     queueSize,
    bool                       currentlyDraining,
    int                        defaultAutoResumeSec)
{
    // Effective auto-resume seconds: per-call override > default.
    const int effectiveAutoResumeSec =
        (req.autoResumeSec > 0) ? req.autoResumeSec : defaultAutoResumeSec;

    RestartCheckOutcome out;
    out.drainQueueSize = queueSize;

    // -------------------- Explicit resume path --------------------
    //
    // The caller (warm-reboot abort cleanup, or operator with -R/--resume)
    // wants us to exit drain mode NOW and force a zebra re-dump, instead
    // of waiting for the auto-resume timer.
    //
    // Two sub-cases:
    //   (a) currentlyDraining == false → no-op. We still reply RESUMED
    //       (so the tool's exit code is 0) but do NOT clear a non-existent
    //       drain flag, do NOT touch STATE_DB (so we don't overwrite the
    //       "ready" / "auto_resumed" trail with bogus "resumed" state),
    //       and do NOT force-disconnect a healthy FPM connection.
    //   (b) currentlyDraining == true  → full resume sequence: clear drain
    //       flag, disarm timer, write STATE_DB drain_state=resumed, send
    //       RESUMED, force reconnect.
    if (req.resumeRequested)
    {
        const bool wasDraining = currentlyDraining;

        out.replyOp = "RESUMED";
        out.replyValues.emplace_back("queueSize",    std::to_string(queueSize));
        out.replyValues.emplace_back("was_draining", wasDraining ? "true" : "false");
        out.replyValues.emplace_back("resumed",      wasDraining ? "true" : "false");

        if (!wasDraining)
        {
            // Sub-case (a): no-op. Leave outcome at NoChange / None / "".
            return out;
        }

        // Sub-case (b): full resume.
        out.drainFlag      = RestartCheckOutcome::DrainFlag::Clear;
        out.timerAction    = RestartCheckOutcome::TimerAction::Disarm;
        out.drainState     = "resumed";
        out.forceReconnect = true;
        return out;
    }

    // -------------------- Normal restart-check path --------------------
    //
    // Decide drain flag and timer:
    //   hasZmq && !currentlyDraining → enter drain mode; ArmFirst.
    //   hasZmq &&  currentlyDraining → re-notification during drain; ReArm.
    //   !hasZmq                      → nothing to drain; no flag, no timer.
    if (hasZmq)
    {
        if (!currentlyDraining)
        {
            out.drainFlag      = RestartCheckOutcome::DrainFlag::Set;
            out.timerAction    = RestartCheckOutcome::TimerAction::ArmFirst;
            out.autoResumeSec  = effectiveAutoResumeSec;
        }
        else
        {
            out.drainFlag      = RestartCheckOutcome::DrainFlag::NoChange;
            out.timerAction    = RestartCheckOutcome::TimerAction::ReArm;
            out.autoResumeSec  = effectiveAutoResumeSec;
        }
    }

    // Reply: READY when queue is empty (qsize == 0, always true when ZMQ
    // is off because totalDbUpdaterQueueSize returns 0 in that case), else
    // NOT_READY with queueSize for the tool's retry-loop logging.
    const bool ready  = (queueSize == 0);
    out.drainState    = ready ? "ready" : "draining";
    out.replyOp       = ready ? "READY" : "NOT_READY";
    out.replyValues.emplace_back("queueSize", std::to_string(queueSize));

    return out;
}

TimerFireOutcome handleDrainAutoResumeTimerFire(size_t queueSize)
{
    TimerFireOutcome out;
    out.drainQueueSize = queueSize;
    // Other fields fixed by struct defaults: clearDrainFlag=true,
    // disarmTimer=true, drainState="auto_resumed", forceReconnect=true.
    return out;
}

// --------------------------------------------------------------
// Startup helpers
// --------------------------------------------------------------

DrainAutoResumeSecParse parseDrainAutoResumeSec(
    const std::string& raw,
    int                defaultSec,
    int                minSec,
    int                maxSec)
{
    DrainAutoResumeSecParse out;
    out.value = defaultSec;

    // Empty or "None" — caller didn't set the field. (Python None gets
    // serialized as the literal string "None" by sonic-cfggen renderers.)
    if (raw.empty() || raw == "None")
    {
        out.source = DrainAutoResumeSecParse::Source::Default;
        return out;
    }

    int parsed = 0;
    try
    {
        // std::stoi parses leading digits and ignores trailing garbage
        // ("5abc" → 5). We want strict — reject anything that isn't
        // purely an integer (optionally with leading sign / whitespace).
        std::size_t consumed = 0;
        parsed = std::stoi(raw, &consumed);
        if (consumed != raw.size())
        {
            // trailing non-digit chars: reject as invalid
            out.source = DrainAutoResumeSecParse::Source::Invalid;
            return out;
        }
    }
    catch (const std::exception&)
    {
        out.source = DrainAutoResumeSecParse::Source::Invalid;
        return out;
    }

    out.rawParsed = parsed;

    if (parsed < minSec || parsed > maxSec)
    {
        // valid int, but outside the allowed range
        out.source = DrainAutoResumeSecParse::Source::OutOfRange;
        return out;
    }

    out.value  = parsed;
    out.source = DrainAutoResumeSecParse::Source::ConfigDb;
    return out;
}

int loadDrainAutoResumeSecFromConfigDb(
    swss::Table& deviceMetadataTable,
    int          defaultSec,
    int          minSec,
    int          maxSec)
{
    std::string raw;
    deviceMetadataTable.hget("localhost", "fpmsyncd_drain_auto_resume_sec", raw);
    DrainAutoResumeSecParse p = parseDrainAutoResumeSec(raw, defaultSec, minSec, maxSec);
    return p.value;
}

void clearStaleDrainStateFields(swss::Table& warmRestartStateTable)
{
    // Idempotent: hdel on an absent field is a no-op.
    warmRestartStateTable.hdel("fpmsyncd", "drain_state");
    warmRestartStateTable.hdel("fpmsyncd", "drain_queue_size");
    // Legacy field name from an earlier iteration where we wrote a plain
    // "queueSize" before introducing the drain_* namespace. Cleaned
    // defensively in case the prior fpmsyncd was at that older version.
    warmRestartStateTable.hdel("fpmsyncd", "queueSize");
}

}  // namespace fpmsyncd_warmreboot
