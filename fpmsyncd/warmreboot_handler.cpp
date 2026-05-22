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
            // Positive override only; anything else falls back to default.
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
        // Unknown fields ignored (forward compat).
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
    const int effectiveAutoResumeSec =
        (req.autoResumeSec > 0) ? req.autoResumeSec : defaultAutoResumeSec;

    RestartCheckOutcome out;
    out.drainQueueSize = queueSize;

    // Resume path: not-draining → reply-only no-op (preserve FPM, leave
    // STATE_DB trail); draining → clear flag, disarm, force reconnect.
    if (req.resumeRequested)
    {
        const bool wasDraining = currentlyDraining;

        out.replyOp = "RESUMED";
        out.replyValues.emplace_back("queueSize",    std::to_string(queueSize));
        out.replyValues.emplace_back("was_draining", wasDraining ? "true" : "false");
        out.replyValues.emplace_back("resumed",      wasDraining ? "true" : "false");

        if (!wasDraining)
        {
            return out;
        }

        out.drainFlag      = RestartCheckOutcome::DrainFlag::Clear;
        out.timerAction    = RestartCheckOutcome::TimerAction::Disarm;
        out.drainState     = "resumed";
        out.forceReconnect = true;
        return out;
    }

    // Normal path: ZMQ off → no drain/timer; ZMQ on → ArmFirst (or ReArm
    // if already draining).
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
    return out;
}

// ---------- Startup helpers ----------

DrainAutoResumeSecParse parseDrainAutoResumeSec(
    const std::string& raw,
    int                defaultSec,
    int                minSec,
    int                maxSec)
{
    DrainAutoResumeSecParse out;
    out.value = defaultSec;

    // sonic-cfggen renders absent fields as literal "None".
    if (raw.empty() || raw == "None")
    {
        out.source = DrainAutoResumeSecParse::Source::Default;
        return out;
    }

    int parsed = 0;
    try
    {
        // Strict: std::stoi tolerates trailing junk ("5abc" → 5); we don't.
        std::size_t consumed = 0;
        parsed = std::stoi(raw, &consumed);
        if (consumed != raw.size())
        {
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
    return parseDrainAutoResumeSec(raw, defaultSec, minSec, maxSec).value;
}

void clearStaleDrainStateFields(swss::Table& warmRestartStateTable)
{
    warmRestartStateTable.hdel("fpmsyncd", "drain_state");
    warmRestartStateTable.hdel("fpmsyncd", "drain_queue_size");
    // Legacy field from before drain_* namespacing; cleaned defensively.
    warmRestartStateTable.hdel("fpmsyncd", "queueSize");
}

}  // namespace fpmsyncd_warmreboot
