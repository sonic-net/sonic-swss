#include "fpmsyncd/routesendcoalescer.h"

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <string>
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
    // A full chunk is ~7.5 MiB worst case, well inside swss-common's 16 MiB
    // MQ_RESPONSE_MAX_COUNT.
    c.maxBatchEntries = 256;          // KCOs per wire chunk
    c.outerBackoffMs = 50;            // brief pause after a transient send failure
    c.tFailMs = 60000;                // 60s stuck -> assert (transient vs crash-loop)
    c.mMax = 1000000;                 // hard cap on backlog to bound memory
    c.telemetryMinIntervalMs = 10000; // <= 1 STATE_DB publish / 10s
    c.warnFraction = 0.5;             // STALLED once stuck age passes half of tFailMs
    // Bound the ZmqClient inner retry so a batched set() returns to the outer
    // loop in ~10ms rather than running its ~41s default ladder: long enough to
    // absorb a sub-tick blip inline, short enough to keep the tFailMs budget
    // meaningful.
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
        try
        {
            m_statTable = std::make_unique<Table>(stateDb, FPMSYNCD_ROUTE_STAT_TABLE);
            // assert_total is a lifetime counter across restarts, so seed it from
            // any sticky record a prior process left behind.
            std::string persisted;
            if (m_statTable->hget(FPMSYNCD_ROUTE_STAT_KEY, "assert_total", persisted))
            {
                try
                {
                    m_assertTotal.store(std::stoull(persisted), std::memory_order_relaxed);
                }
                catch (const std::exception &)
                {
                    // Malformed value -> start from zero.
                }
            }
        }
        catch (const std::exception &e)
        {
            // Telemetry is best-effort: route delivery must start even when
            // STATE_DB is unavailable. Every use of m_statTable is null-guarded.
            m_statTable.reset();
            SWSS_LOG_WARN("route stat telemetry disabled, STATE_DB unavailable: %s",
                          e.what());
        }
    }
    // Retry beyond a sub-tick blip belongs to the outer loop (re-merge plus
    // outerBackoffMs), so cap the inner one here.
    if (m_zmqClient != nullptr)
    {
        m_zmqClient->setSendRetryConfig(m_cfg.sendInnerMaxRetries,
                                        m_cfg.sendInnerMaxBackoffMs);
    }
    // A zero entry cap would produce empty chunks and never retire budget.
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

bool RouteSendCoalescer::stop()
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_running)
        {
            return false;
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
    return true;
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
    // so queueing it would silently delete the route. Drop it rather than post a
    // destructive tuple.
    if (kfvOp(kco) == SET_COMMAND && kfvFieldsValues(kco).empty())
    {
        SWSS_LOG_ERROR("Dropping malformed SET for %s: empty field set "
                       "(an empty SET is a DEL on the wire)", kfvKey(kco).c_str());
        return;
    }
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        CoalesceMap &m = mapForLocked(tbl);
        if (m.empty())
        {
            m_pendingSince[tableIndex(tbl)] = SteadyClock::now();
        }
        // Last-writer-wins: a newer op supersedes the pending one for the same
        // key (SET over DEL, DEL over SET, or a refreshed SET).
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
        // Ops received during an episode are the "in" side of the episode
        // coalesce ratio, which the consumer derives.
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
    // upsertKco drops this too; checked here so the log names the direct caller.
    if (values.empty())
    {
        SWSS_LOG_ERROR("Dropping malformed SET for %s: empty field set "
                       "(an empty SET is a DEL on the wire)", key.c_str());
        return;
    }
    upsertKco(tbl, KeyOpFieldsValuesTuple{key, SET_COMMAND, values});
}

void RouteSendCoalescer::upsertDel(TableId tbl, const std::string &key)
{
    upsertKco(tbl, KeyOpFieldsValuesTuple{key, DEL_COMMAND, std::vector<FieldValueTuple>{}});
}

bool RouteSendCoalescer::drainTable(TableId tbl, size_t budget, size_t &sent)
{
    sent = 0;
    ProducerStateTable *table = tableFor(tbl);
    if (table == nullptr)
    {
        return true;
    }

    // Chunked drain of the live map: each iteration pulls a chunk under the
    // lock and sends it lock-free, so ingest keeps coalescing between chunks and
    // a transient failure strands only the chunk in flight.
    //
    // One ordered sweep per cycle. Each chunk resumes at the last key visited
    // rather than at begin(), so keys arriving mid-drain below the cursor do not
    // consume the remaining budget; keys arriving above it are swept with it and
    // do. The budget, taken from the table's depth at cycle entry, is what makes
    // the sweep terminate. Whatever the cycle does not reach is served by the
    // next one, which restarts at the lowest key still present.
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
            auto it = resume ? m.lower_bound(cursor) : m.begin();
            while (it != m.end() && chunk.size() < chunkCap)
            {
                chunk.push_back(it->second);
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
            // Transient send failure. Re-merge the chunk into the live map and
            // retry on the next drain; ingest that arrived during the send is
            // newer, so it wins.
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
    // Sample both depths up front: the pair fixes this cycle's fair share.
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
    // not hold up label-route delivery.
    bool ok = true;
    size_t routeSent = 0;
    size_t labelSent = 0;
    ok = drainTable(TableId::Route, routeBudget, routeSent) && ok;
    ok = drainTable(TableId::LabelRoute, labelBudget, labelSent) && ok;

    m_lastCycleOk = ok;
    m_lastCycleSent = routeSent + labelSent;

    if (!ok)
    {
        // One stranded chunk is a blip, so an episode opens only on a second
        // consecutive failure, once the stall has outlived one backoff.
        m_retryFromMapTotal.fetch_add(1, std::memory_order_relaxed);
        ++m_consecutiveOuterFailures;
        if (m_consecutiveOuterFailures >= 2)
        {
            onEpisodeStart();
        }
    }
    else
    {
        // Any success breaks the streak, whether or not the map fully emptied.
        m_consecutiveOuterFailures = 0;
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
    // Arm the RECOVERED edge here: an episode that opens and closes between two
    // throttled publishes would otherwise never set it.
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
    // Force a snapshot at episode close: this is the post-incident artifact.
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
    // Entries stranded in the map are lost across the deliberate exit. Record
    // the count before it goes.
    m_routesLostTotal.fetch_add(depth, std::memory_order_relaxed);

    SWSS_LOG_ERROR("fpmsyncd route send stalled (reason=%s, map_depth=%zu, stuck_ms=%" PRIu64
                   "); writing assert record and exiting",
                   reason, depth, stuckMs);

    // Written immediately before exit so crash-loop RCA survives the restart.
    if (m_statTable != nullptr)
    {
        // Human-readable UTC rather than a raw epoch.
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

    // Deliberate termination: fpmsyncd is a critical process, so the bgp
    // container bounces and the table is rebuilt by BGP re-convergence.
    // _Exit(), not exit(): this runs on the send thread while the main thread is
    // live, and exit() would run atexit handlers and static destructors across
    // both. The STATE_DB record above is already durable.
    std::_Exit(EXIT_FAILURE);
}

void RouteSendCoalescer::publishTelemetry(bool force)
{
    if (m_statTable == nullptr)
    {
        return;
    }

    // Snapshot the guarded liveness/episode state and derive health under a
    // single lock, which doubles as the throttle gate.
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

        // Precedence: STALLED (backlog past warnFraction of the assert budget)
        // > CONGESTED (episode open) > RECOVERED (one-shot edge) > OK.
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

    // Ordered so a raw HGETALL reads top-down: health -> pending state ->
    // lifetime -> last episode -> send-path back-pressure. Raw counters only;
    // ratios are left to the consumer.
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
        // --- last congestion episode ---
        {"ep_duration_ms", std::to_string(m_epLastDurationMs.load(std::memory_order_relaxed))},
        {"ep_peak_depth", std::to_string(m_epLastPeakDepth.load(std::memory_order_relaxed))},
        {"ep_coalesced_in", std::to_string(epIn)},
        {"ep_coalesced_out", std::to_string(epOut)},
    };

    // Send-path back-pressure counters: leading indicators of congestion.
    if (m_zmqClient != nullptr)
    {
        fvs.emplace_back("zmq_eagain_total", std::to_string(m_zmqClient->getSendEagainTotal()));
        fvs.emplace_back("zmq_blip_absorbed_total",
                         std::to_string(m_zmqClient->getSendBlipAbsorbedTotal()));
        fvs.emplace_back("zmq_backoff_max_ms", std::to_string(m_zmqClient->getSendBackoffMaxMs()));
    }
    // Outer re-merges: one per stranded chunk. Published next to the ZMQ
    // counters so the two can be compared in one record.
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

void RouteSendCoalescer::sendLoop()
{
    SWSS_LOG_NOTICE("route send thread started");
    while (true)
    {
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            // Timed, not signal-only: guarantees a tick without ingest so
            // telemetry still publishes on an idle box.
            m_cv.wait_for(lock, std::chrono::milliseconds(m_cfg.idleTickMs), [this] {
                return m_stop || totalDepthLocked() != 0;
            });
            if (m_stop && totalDepthLocked() == 0)
            {
                break;
            }
        }

        bool progressed = drainOnce();
        if (progressed)
        {
            // Back off on a failed cycle, and on one that delivered nothing
            // while the map still owes work: an undrainable table would otherwise
            // spin the thread and never reach the stop predicate. A non-empty map
            // after a productive cycle is the normal bounded-pass steady state.
            bool noProgress = false;
            if (m_lastCycleSent == 0)
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                noProgress = (totalDepthLocked() != 0);
            }
            if (!m_lastCycleOk || noProgress)
            {
                // Pause before re-draining so newer ingest coalesces onto the
                // pending keys.
                std::unique_lock<std::mutex> lock(m_mutex);
                m_cv.wait_for(lock, std::chrono::milliseconds(m_cfg.outerBackoffMs),
                              [this] { return m_stop; });
                if (m_stop)
                {
                    break;
                }
            }
        }
    }

    // Best-effort final drain. Ingest has stopped, so one budgeted sweep per
    // table covers the map.
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

    // The map is in-memory only, so anything still held at exit is lost.
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
