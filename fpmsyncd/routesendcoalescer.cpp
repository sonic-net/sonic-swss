#include "fpmsyncd/routesendcoalescer.h"

#include <algorithm>
#include <cassert>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <system_error>
#include <utility>

#include "logger.h"

using namespace std;

namespace swss {

#define FPMSYNCD_ROUTE_STAT_TABLE "FPMSYNCD_ROUTE_STAT_TABLE"
#define FPMSYNCD_ROUTE_STAT_KEY   "global"

RouteSendCoalescer::Config RouteSendCoalescer::defaultConfig()
{
    Config c;
    c.idleTickMs = 1000;              // wake at least once a second even with no ingest
    c.maxBatchEntries = 256;          // KCOs per wire chunk
    c.maxBatchBytes = 8u * 1024 * 1024; // soft cap per chunk, under maxWireBytes
    c.maxWireBytes = 16u * 1024 * 1024; // MQ_RESPONSE_MAX_COUNT in swss-common zmqserver.h
    c.outerBackoffMs = 50;            // brief pause after a transient send failure
    c.tFailMs = 60000;                // 60s stuck -> assert (transient vs crash-loop)
    c.mMax = 1000000;                 // hard cap on backlog to bound memory
    c.telemetryMinIntervalMs = 10000; // <= 1 STATE_DB publish / 10s
    c.warnFraction = 0.5;             // STALLED once stuck age passes half of tFailMs
    // Bound the ZmqClient inner retry so a batched set() returns to the outer
    // loop in ~10ms rather than running its ~41s default ladder. Long enough to
    // absorb a sub-tick blip inline, short enough that newer ingest can coalesce
    // onto a deferred batch and that the tFailMs budget stays meaningful.
    c.sendInnerMaxRetries = 2;
    c.sendInnerMaxBackoffMs = 5;
    return c;
}

RouteSendCoalescer::RouteSendCoalescer(ProducerStateTable *routeTable,
                                       ProducerStateTable *labelRouteTable,
                                       ZmqClient *zmqClient,
                                       DBConnector *stateDb,
                                       const Config &cfg) :
    m_routeTable(routeTable),
    m_labelRouteTable(labelRouteTable),
    m_zmqClient(zmqClient),
    m_cfg(cfg)
{
    if (stateDb != nullptr)
    {
        m_statTable = std::make_unique<Table>(stateDb, FPMSYNCD_ROUTE_STAT_TABLE);
        // assert_total is a lifetime counter across restarts, so seed it from any
        // sticky record a prior process left behind. Without this a fresh process
        // publishes 0 and clobbers the persisted value.
        std::string persisted;
        if (m_statTable->hget(FPMSYNCD_ROUTE_STAT_KEY, "assert_total", persisted))
        {
            try
            {
                m_assertTotal.store(std::stoull(persisted), std::memory_order_relaxed);
            }
            catch (const std::exception &)
            {
                // Malformed value -> start from zero (defensive; never fatal).
            }
        }
    }
    // Retry beyond a sub-tick blip belongs to the outer loop (re-merge plus
    // outerBackoffMs), so cap the inner one here.
    if (m_zmqClient != nullptr)
    {
        m_zmqClient->setSendRetryConfig(m_cfg.sendInnerMaxRetries,
                                        m_cfg.sendInnerMaxBackoffMs);
    }
    // A zero entry cap would produce empty chunks and never retire budget, so
    // hold the floor at one entry per chunk.
    m_cfg.maxBatchEntries = std::max<size_t>(1, m_cfg.maxBatchEntries);
    m_lastSuccess[tableIndex(TableId::Route)] = SteadyClock::now();
    m_lastSuccess[tableIndex(TableId::LabelRoute)] = m_lastSuccess[tableIndex(TableId::Route)];
}

RouteSendCoalescer::~RouteSendCoalescer()
{
    stop();
}

void RouteSendCoalescer::start()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_running)
    {
        return;
    }
    m_stop = false;
    m_running = true;
    m_thread = std::thread(&RouteSendCoalescer::sendLoop, this);
}

void RouteSendCoalescer::stop()
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_running)
        {
            return;
        }
        m_stop = true;
    }
    m_cv.notify_all();
    if (m_thread.joinable())
    {
        m_thread.join();
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    m_running = false;
    m_parkedCv.notify_all();   // release a pause() racing with shutdown
}

RouteSendCoalescer::CoalesceMap &RouteSendCoalescer::mapForLocked(TableId tbl)
{
    return (tbl == TableId::Route) ? m_routeMap : m_labelMap;
}

ProducerStateTable *RouteSendCoalescer::tableFor(TableId tbl) const
{
    return (tbl == TableId::Route) ? m_routeTable : m_labelRouteTable;
}

size_t RouteSendCoalescer::totalDepthLocked() const
{
    return m_routeMap.size() + m_labelMap.size();
}

uint64_t RouteSendCoalescer::mapDepth() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return static_cast<uint64_t>(totalDepthLocked());
}

void RouteSendCoalescer::upsertKco(TableId tbl, const KeyOpFieldsValuesTuple &kco)
{
    // A SET with no field-values serializes identically to a DEL on the ZMQ wire,
    // so it would silently delete the route. Guarded here as well as in upsertSet
    // because routesync feeds pre-formed KCOs straight into this entry point.
    assert(!(kfvOp(kco) == SET_COMMAND && kfvFieldsValues(kco).empty()) &&
           "SET KCO requires non-empty fields (empty == DEL on wire)");
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        CoalesceMap &m = mapForLocked(tbl);
        if (m.empty())
        {
            m_pendingSince[tableIndex(tbl)] = SteadyClock::now();
        }
        // Last-writer-wins: a newer op for the same key supersedes the pending
        // one (SET over DEL, DEL over SET, or a refreshed SET). Overwriting a
        // still-pending key is exactly the coalescing win under churn.
        auto it = m.find(kfvKey(kco));
        if (it != m.end())
        {
            it->second = kco;
            m_routesCoalescedTotal.fetch_add(1, std::memory_order_relaxed);
        }
        else
        {
            m.emplace(kfvKey(kco), kco);
        }
        // Every op received during an episode is an input folded toward the next
        // emitted chunk (feeds the consumer-derived ep_coalesce_ratio).
        if (m_inEpisode)
        {
            ++m_epCoalescedIn;
        }

        size_t depth = totalDepthLocked();
        if (depth > m_mapDepthHwm.load(std::memory_order_relaxed))
        {
            m_mapDepthHwm.store(depth, std::memory_order_relaxed);
        }
        if (m_inEpisode && depth > m_epPeakDepth)
        {
            m_epPeakDepth = depth;
        }
    }
    m_cv.notify_one();
}

void RouteSendCoalescer::upsertSet(TableId tbl, const std::string &key,
                                   const std::vector<FieldValueTuple> &values)
{
    // Route SETs always carry fields today. Assert it, because a field-less SET
    // is indistinguishable from a DEL on the ZMQ wire and would delete the route.
    assert(!values.empty() && "upsertSet requires non-empty fields (empty == DEL on wire)");
    upsertKco(tbl, KeyOpFieldsValuesTuple{key, SET_COMMAND, values});
}

void RouteSendCoalescer::upsertDel(TableId tbl, const std::string &key)
{
    upsertKco(tbl, KeyOpFieldsValuesTuple{key, DEL_COMMAND, std::vector<FieldValueTuple>{}});
}

size_t RouteSendCoalescer::approxKcoSerializedBytes(const KeyOpFieldsValuesTuple &kco)
{
    // Rough upper-ish estimate of the on-wire size: key + op + each field/value
    // plus a small constant per entry and per field for framing/length prefixes.
    // Only used to keep a chunk under the byte cap; exactness is not required.
    constexpr size_t kPerEntryOverhead = 32;
    constexpr size_t kPerFieldOverhead = 16;
    size_t bytes = kfvKey(kco).size() + kPerEntryOverhead;
    for (const auto &fv : kfvFieldsValues(kco))
    {
        bytes += fvField(fv).size() + fvValue(fv).size() + kPerFieldOverhead;
    }
    return bytes;
}

bool RouteSendCoalescer::drainTable(TableId tbl, size_t budget, size_t &sent)
{
    sent = 0;
    ProducerStateTable *table = tableFor(tbl);
    if (table == nullptr)
    {
        return true;
    }

    // Chunked drain of the live map, bounded by this table's cycle budget. Each
    // iteration pulls a chunk under the lock, sends it lock-free, then accounts
    // it. Ingest keeps coalescing onto the map between chunks, and a transient
    // failure strands only the one chunk in flight.
    //
    // The pass is one ordered sweep: each chunk resumes at the last key visited
    // instead of restarting at begin(), so every key present at cycle entry is
    // covered exactly once. Restarting at the head would let keys that arrive
    // mid-drain and sort below the cursor consume the budget, starving the high
    // end of the table. The budget bounds the sweep, since keys arriving ahead of
    // the cursor would otherwise extend it without a fixed point; those are
    // served by the next cycle.
    std::string cursor;
    bool resume = false;
    size_t remaining = budget;
    while (remaining != 0)
    {
        std::vector<KeyOpFieldsValuesTuple> chunk;
        bool sweepDone = false;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            CoalesceMap &m = mapForLocked(tbl);
            // Cap the chunk by what this table is still owed for the cycle.
            const size_t chunkCap = std::min(m_cfg.maxBatchEntries, remaining);
            size_t chunkBytes = 0;
            auto it = resume ? m.lower_bound(cursor) : m.begin();
            while (it != m.end() && chunk.size() < chunkCap)
            {
                size_t kcoBytes = approxKcoSerializedBytes(it->second);
                // An entry larger than the ZMQ message ceiling can never be
                // delivered. Retrying it would hold the table stuck until the
                // liveness guard exits the process, so drop and account it.
                if (chunk.empty() && kcoBytes >= m_cfg.maxWireBytes)
                {
                    SWSS_LOG_ERROR("dropping undeliverable route %s: %zu bytes exceeds "
                                   "the %zu byte wire limit",
                                   it->first.c_str(), kcoBytes, m_cfg.maxWireBytes);
                    m_routesLostTotal.fetch_add(1, std::memory_order_relaxed);
                    it = m.erase(it);
                    continue;
                }
                // Byte cap, but always take at least one entry so a single large
                // KCO still makes progress (bounded ultimately by the 16 MiB ZMQ
                // ceiling inside set()).
                if (!chunk.empty() && chunkBytes + kcoBytes > m_cfg.maxBatchBytes)
                {
                    break;
                }
                chunk.push_back(it->second);
                chunkBytes += kcoBytes;
                it = m.erase(it);
            }
            if (it == m.end())
            {
                sweepDone = true;
            }
            else
            {
                cursor = it->first;
                resume = true;
            }
        }

        if (chunk.empty())
        {
            return true;
        }

        try
        {
            // Batched wire path: one zmq_send for this coalesced chunk.
            table->set(chunk);
        }
        catch (const std::exception &e)
        {
            // Transient send failure (ZmqClient exhausted its inner blip-absorber,
            // or a connection error). Keep the chunk by re-merging it into the live
            // map; newer ingest that arrived during the send wins (do not clobber a
            // fresher op for the same key). The chunk is retried on the next drain.
            std::lock_guard<std::mutex> lock(m_mutex);
            CoalesceMap &m = mapForLocked(tbl);
            for (auto &kco : chunk)
            {
                m.emplace(kfvKey(kco), kco); // no-op if a newer op already landed
            }
            SWSS_LOG_WARN("route send chunk deferred (%zu entries kept in map): %s",
                          chunk.size(), e.what());
            return false;
        }

        // Success: this chunk cleared the ZMQ HWM cliff.
        m_routesSentTotal.fetch_add(chunk.size(), std::memory_order_relaxed);
        m_chunksSentTotal.fetch_add(1, std::memory_order_relaxed);
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_lastSuccess[tableIndex(tbl)] = SteadyClock::now();
            if (m_inEpisode)
            {
                m_epCoalescedOut += chunk.size();
            }
        }
        // Charge the delivered chunk against this table's cycle budget.
        sent += chunk.size();
        remaining -= chunk.size();
        if (sweepDone)
        {
            break;
        }
    }

    return true;
}

uint64_t RouteSendCoalescer::stuckMsLocked() const
{
    const auto now = SteadyClock::now();
    uint64_t worst = 0;
    auto ageOf = [&](const CoalesceMap &m, size_t idx) {
        if (m.empty())
        {
            return; // an empty table owes nothing and so cannot be stuck
        }
        const auto since = std::max(m_lastSuccess[idx], m_pendingSince[idx]);
        const auto ms = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                now - since).count());
        worst = std::max(worst, ms);
    };
    ageOf(m_routeMap, tableIndex(TableId::Route));
    ageOf(m_labelMap, tableIndex(TableId::LabelRoute));
    return worst;
}

bool RouteSendCoalescer::drainOnce()
{
    // Sample both depths up front. The pair fixes this cycle's fair share, so
    // one bounded pass covers both tables and entries arriving mid-cycle are
    // served by the next one.
    size_t routeBudget = 0;
    size_t labelBudget = 0;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        routeBudget = m_routeMap.size();
        labelBudget = m_labelMap.size();
    }

    if (routeBudget == 0 && labelBudget == 0)
    {
        publishTelemetry(false);
        return false;
    }

    // Drain both tables regardless of either outcome: a stalled route table must
    // not hold up label-route delivery. A transient failure on either -> back off
    // and retry.
    bool ok = true;
    size_t routeSent = 0;
    size_t labelSent = 0;
    ok = drainTable(TableId::Route, routeBudget, routeSent) && ok;
    ok = drainTable(TableId::LabelRoute, labelBudget, labelSent) && ok;

    m_lastCycleOk = ok;
    m_lastCycleSent = routeSent + labelSent;

    if (!ok)
    {
        // Hysteresis: one stranded chunk is a blip, so an episode opens only on a
        // second consecutive failure, once the stall has outlived one backoff.
        m_retryFromMapTotal.fetch_add(1, std::memory_order_relaxed);
        ++m_consecutiveOuterFailures;
        if (m_consecutiveOuterFailures >= 2)
        {
            onEpisodeStart();
        }
    }
    else
    {
        // The socket accepted a chunk, so the stall cleared. Any success breaks
        // the streak, whether or not the map fully emptied.
        m_consecutiveOuterFailures = 0;
        // If we were in an episode and the map is now empty, close the episode.
        bool empty = false;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            empty = (totalDepthLocked() == 0);
        }
        if (empty)
        {
            onEpisodeRecovered();
        }
    }

    evaluateAssertThresholds();
    publishTelemetry(false);
    return true;
}

void RouteSendCoalescer::onEpisodeStart()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_inEpisode)
    {
        return;
    }
    m_inEpisode = true;
    // Arm the RECOVERED edge here (not only when publishTelemetry observes
    // congestion): a short episode that opens and closes between two throttled
    // publishes would otherwise never set this and skip RECOVERED.
    m_wasUnhealthy = true;
    m_epStart = SteadyClock::now();
    m_epPeakDepth = totalDepthLocked();
    m_epCoalescedIn = 0;
    m_epCoalescedOut = 0;
    m_congestionEpisodesTotal.fetch_add(1, std::memory_order_relaxed);
}

void RouteSendCoalescer::onEpisodeRecovered()
{
    uint64_t durationMs = 0;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_inEpisode)
        {
            return;
        }
        durationMs = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                SteadyClock::now() - m_epStart).count());
        m_epLastDurationMs.store(durationMs, std::memory_order_relaxed);
        m_epLastPeakDepth.store(m_epPeakDepth, std::memory_order_relaxed);
        m_epLastCoalescedIn.store(m_epCoalescedIn, std::memory_order_relaxed);
        m_epLastCoalescedOut.store(m_epCoalescedOut, std::memory_order_relaxed);
        m_inEpisode = false;
    }
    // Force a telemetry snapshot at episode close (the post-incident artifact).
    publishTelemetry(true);
}

void RouteSendCoalescer::evaluateAssertThresholds()
{
    size_t depth = 0;
    uint64_t stuckMs = 0;
    bool tripTime = false;
    bool tripDepth = false;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        depth = totalDepthLocked();
        if (depth > 0)
        {
            stuckMs = stuckMsLocked();
            tripTime = (stuckMs > m_cfg.tFailMs);
        }
        tripDepth = (depth > m_cfg.mMax);
    }

    if (tripTime)
    {
        writeAssertRecordAndExit("STUCK_TIMEOUT", depth, stuckMs);
    }
    else if (tripDepth)
    {
        writeAssertRecordAndExit("DEPTH_OVERFLOW", depth, stuckMs);
    }
}

void RouteSendCoalescer::writeAssertRecordAndExit(const char *reason, size_t depth, uint64_t stuckMs)
{
    m_assertTotal.fetch_add(1, std::memory_order_relaxed);
    // Entries stranded in the map are lost across the deliberate exit; warm-restart
    // RIB replay repopulates them. Record the count for the loss guard.
    m_routesLostTotal.fetch_add(depth, std::memory_order_relaxed);

    SWSS_LOG_ERROR("fpmsyncd route send stalled (reason=%s, map_depth=%zu, stuck_ms=%" PRIu64
                   "); writing assert record and exiting for warm-restart recovery",
                   reason, depth, stuckMs);

    // Write the last-assert record to STATE_DB IMMEDIATELY BEFORE exit so
    // crash-loop RCA survives the container restart.
    if (m_statTable != nullptr)
    {
        // Human-readable UTC, so the record reads as a wall-clock instant rather
        // than a raw epoch.
        auto nowEpoch = std::chrono::system_clock::to_time_t(
                            std::chrono::system_clock::now());
        char tsBuf[32] = {0};
        struct tm tmUtc;
        if (gmtime_r(&nowEpoch, &tmUtc) != nullptr)
        {
            strftime(tsBuf, sizeof(tsBuf), "%Y-%m-%d %H:%M:%S", &tmUtc);
        }
        std::vector<FieldValueTuple> fvs = {
            {"assert_total", std::to_string(m_assertTotal.load(std::memory_order_relaxed))},
            {"assert_last_ts", tsBuf},
            {"assert_last_reason", reason},
            {"assert_last_depth", std::to_string(depth)},
            {"assert_last_stuck_ms", std::to_string(stuckMs)},
            {"routes_lost_total", std::to_string(m_routesLostTotal.load(std::memory_order_relaxed))},
        };
        try
        {
            m_statTable->set(FPMSYNCD_ROUTE_STAT_KEY, fvs);
        }
        catch (const std::exception &e)
        {
            SWSS_LOG_ERROR("failed to persist assert record before exit: %s", e.what());
        }
    }

    // Deliberate, logged termination (NOT an uncaught throw). fpmsyncd is critical +
    // autorestart=false -> the bgp container bounces -> warm-restart replay.
    // _Exit(), not exit(): this runs on the send thread while the main thread is
    // still live, and exit() would run atexit handlers and static destructors
    // across both, risking a deadlock or double-free that swallows the assert.
    // The STATE_DB record above is written synchronously, so it is already durable.
    std::_Exit(EXIT_FAILURE);
}

void RouteSendCoalescer::publishTelemetry(bool force)
{
    if (m_statTable == nullptr)
    {
        return;
    }

    // Snapshot the mutex-guarded liveness/episode state and derive the health word
    // under a single lock, which doubles as the throttle gate. Health is derived
    // here so the record leads with one word an alert can key on.
    uint64_t depth = 0;
    uint64_t lastSuccessAgeSec = 0;
    const char *health = "OK";
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto sinceMs = m_lastTelemetry.time_since_epoch().count() == 0
                           ? m_cfg.telemetryMinIntervalMs
                           : std::chrono::duration_cast<std::chrono::milliseconds>(
                                 SteadyClock::now() - m_lastTelemetry).count();
        if (!force && sinceMs < m_cfg.telemetryMinIntervalMs)
        {
            return;
        }
        m_lastTelemetry = SteadyClock::now();

        depth = static_cast<uint64_t>(totalDepthLocked());
        auto stuckMs = stuckMsLocked();
        lastSuccessAgeSec = static_cast<uint64_t>(stuckMs / 1000);

        // Health precedence: STALLED (backlog persisting past warnFraction of the
        // assert budget -> bounce approaching) > CONGESTED (episode open, actively
        // coalescing under pressure) > RECOVERED (one-shot edge after clearing) > OK.
        bool stalled = depth > 0 &&
                       static_cast<double>(stuckMs) >= m_cfg.warnFraction * m_cfg.tFailMs;
        bool congested = m_inEpisode;
        if (stalled)
        {
            health = "STALLED";
            m_wasUnhealthy = true;
        }
        else if (congested)
        {
            health = "CONGESTED";
            m_wasUnhealthy = true;
        }
        else if (m_wasUnhealthy)
        {
            health = "RECOVERED";
            m_wasUnhealthy = false;  // report RECOVERED once, then settle to OK
        }
    }

    auto epIn = m_epLastCoalescedIn.load(std::memory_order_relaxed);
    auto epOut = m_epLastCoalescedOut.load(std::memory_order_relaxed);

    // Ordered so a raw HGETALL reads top-down: health -> pending state -> lifetime
    // -> last episode -> send-path back-pressure. Only raw counters are published;
    // ratios are left to the consumer so no producer rounding is baked in.
    std::vector<FieldValueTuple> fvs = {
        {"health", health},                                       // read this first
        // --- pending state ---
        {"map_depth", std::to_string(depth)},
        {"map_depth_hwm", std::to_string(m_mapDepthHwm.load(std::memory_order_relaxed))},
        {"last_success_age_sec", std::to_string(lastSuccessAgeSec)},  // derived, not raw epoch
        // --- lifetime ---
        {"routes_sent_total", std::to_string(m_routesSentTotal.load(std::memory_order_relaxed))},
        {"routes_coalesced_total", std::to_string(m_routesCoalescedTotal.load(std::memory_order_relaxed))},
        {"chunks_sent_total", std::to_string(m_chunksSentTotal.load(std::memory_order_relaxed))},
        {"routes_lost_total", std::to_string(m_routesLostTotal.load(std::memory_order_relaxed))},
        {"congestion_episodes_total", std::to_string(m_congestionEpisodesTotal.load(std::memory_order_relaxed))},
        {"assert_total", std::to_string(m_assertTotal.load(std::memory_order_relaxed))},
        // --- last congestion episode (raw in/out; consumer derives ep_coalesce_ratio) ---
        {"ep_duration_ms", std::to_string(m_epLastDurationMs.load(std::memory_order_relaxed))},
        {"ep_peak_depth", std::to_string(m_epLastPeakDepth.load(std::memory_order_relaxed))},
        {"ep_coalesced_in", std::to_string(epIn)},
        {"ep_coalesced_out", std::to_string(epOut)},
    };

    // Fold in the PR-A send-path back-pressure counters (leading indicators).
    if (m_zmqClient != nullptr)
    {
        fvs.emplace_back("zmq_eagain_total", std::to_string(m_zmqClient->getSendEagainTotal()));
        fvs.emplace_back("zmq_blip_absorbed_total",
                         std::to_string(m_zmqClient->getSendBlipAbsorbedTotal()));
        fvs.emplace_back("zmq_backoff_max_ms", std::to_string(m_zmqClient->getSendBackoffMaxMs()));
    }
    // retry_from_map_total = outer re-merges (each stranded chunk = one re-drain);
    // publish it alongside the ZMQ counters so the consistency check is local.
    fvs.emplace_back("retry_from_map_total",
                     std::to_string(m_retryFromMapTotal.load(std::memory_order_relaxed)));

    try
    {
        m_statTable->set(FPMSYNCD_ROUTE_STAT_KEY, fvs);
    }
    catch (const std::exception &e)
    {
        SWSS_LOG_WARN("failed to publish route stat telemetry: %s", e.what());
    }
}

void RouteSendCoalescer::pause()
{
    std::unique_lock<std::mutex> lock(m_mutex);
    if (m_paused)
    {
        return;
    }
    m_paused = true;
    if (!m_running)
    {
        return; // no send thread, so nothing can be touching the maps
    }
    m_cv.notify_all();
    // Bounded by one drain cycle: the thread parks at the top of the next
    // iteration. Waiting is what makes exclusive ownership meaningful.
    m_parkedCv.wait(lock, [this] { return m_parked || !m_running; });
}

void RouteSendCoalescer::resume()
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_paused)
        {
            return;
        }
        m_paused = false;
    }
    m_cv.notify_all();
}

void RouteSendCoalescer::sendLoop()
{
    SWSS_LOG_NOTICE("route send thread started");
    while (true)
    {
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            if (m_paused)
            {
                // Park between cycles: the maps and the ZMQ socket are the
                // caller's until resume().
                m_parked = true;
                m_parkedCv.notify_all();
                m_cv.wait(lock, [this] { return !m_paused || m_stop; });
                m_parked = false;
            }
            // Timed wait (NOT signal-only): guarantees a tick even without
            // ingest, so telemetry still publishes on an idle box. Wake early on
            // new ingest or stop.
            m_cv.wait_for(lock, std::chrono::milliseconds(m_cfg.idleTickMs), [this] {
                return m_stop || totalDepthLocked() != 0;
            });
            if (m_stop && totalDepthLocked() == 0)
            {
                break;
            }
        }

        // Run one fair drain cycle over both tables, then re-check the queue.
        bool progressed = drainOnce();
        if (progressed)
        {
            // Back off on a failed cycle, and also on a cycle that delivered
            // nothing while the map still owes work -- otherwise a table that
            // cannot be drained at all would spin the thread and never let the
            // stop predicate below run. A non-empty map after a successful,
            // productive cycle is the normal bounded-pass steady state, and the
            // condvar predicate re-fires on it immediately.
            bool noProgress = false;
            if (m_lastCycleSent == 0)
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                noProgress = (totalDepthLocked() != 0);
            }
            if (!m_lastCycleOk || noProgress)
            {
                // Chunk still pending after a transient failure: pause before
                // re-draining so newer ingest coalesces onto the stuck keys.
                std::unique_lock<std::mutex> lock(m_mutex);
                m_cv.wait_for(lock, std::chrono::milliseconds(m_cfg.outerBackoffMs),
                              [this] { return m_stop || m_paused; });
                if (m_stop)
                {
                    break;
                }
            }
        }
    }

    // Best-effort final drain so a clean shutdown does not strand a ready batch.
    // Ingest has stopped by now, so one budgeted sweep per table covers the map.
    size_t routeLeft = 0;
    size_t labelLeft = 0;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        routeLeft = m_routeMap.size();
        labelLeft = m_labelMap.size();
    }
    size_t sent = 0;
    drainTable(TableId::Route, routeLeft, sent);
    drainTable(TableId::LabelRoute, labelLeft, sent);

    // Anything still held at exit is lost: the map is in-memory only, so account
    // it rather than let routes_lost_total under-report a failed final drain.
    size_t stranded = 0;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        stranded = totalDepthLocked();
    }
    if (stranded != 0)
    {
        m_routesLostTotal.fetch_add(stranded, std::memory_order_relaxed);
        SWSS_LOG_WARN("route send thread stopping with %zu undelivered entries", stranded);
    }
    SWSS_LOG_NOTICE("route send thread stopped");
}

}
