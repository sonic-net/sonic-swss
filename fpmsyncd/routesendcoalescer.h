#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "dbconnector.h"
#include "producerstatetable.h"
#include "table.h"
#include "zmqclient.h"

namespace swss {

/*
 * RouteSendCoalescer decouples fpmsyncd's FRR/FPM ingest from the ZMQ send to
 * orchagent (issue sonic-buildimage #28369). Ingest upserts fully-formed KCOs
 * into a persistent, last-writer-wins coalescing map and returns immediately; a
 * single dedicated send thread drains the map in batches over ZMQ.
 *
 *  - Transient back-pressure (ZmqClient throws io_error after its inner blip
 *    absorber): the failed chunk stays in the map and coalesces with newer ingest,
 *    so the per-key backlog shrinks and the next drain succeeds -> no route drop.
 *  - Prolonged failure: bounded by two independent triggers (time since last
 *    successful flush > tFailMs, OR map depth > mMax). Either fires -> the last
 *    assert record is written to STATE_DB and fpmsyncd exits deliberately;
 *    fpmsyncd is critical + autorestart=false, so the bgp container bounces and
 *    warm-restart RIB replay repopulates the table.
 *
 * The send thread is the SOLE writer of the two ZMQ tables it owns. That is the
 * thread-safety contract: ZmqProducerStateTable::set(vector<KCO>) only touches
 * the ZmqClient socket (its own mutex) and AsyncDBUpdater's queue (its own
 * mutex), never the shared RedisPipeline the main thread uses for other tables.
 * Warm-restart reconciliation writes the same tables directly on the main
 * thread, so it parks the send thread first (see pause()) to preserve that
 * single-writer property.
 */
class RouteSendCoalescer
{
public:
    enum class TableId { Route, LabelRoute };

    struct Config
    {
        uint32_t idleTickMs;             // condvar timed-wait period (telemetry tick)
        size_t   maxBatchEntries;        // max KCOs per wire chunk
        size_t   maxBatchBytes;          // byte cap per chunk, under maxWireBytes
        size_t   maxWireBytes;           // hard ZMQ message ceiling (MQ_RESPONSE_MAX_COUNT);
                                         // a single entry above this can never be sent
        uint32_t outerBackoffMs;         // wait after a failed flush before re-draining
        uint32_t tFailMs;                // assert if now - lastSuccess exceeds this
        size_t   mMax;                   // assert if total map depth exceeds this
        uint32_t telemetryMinIntervalMs; // throttle STATE_DB publish (>= 1/interval)
        double   warnFraction;           // STALLED when age > warnFraction * tFailMs
        int      sendInnerMaxRetries;    // ZmqClient inner blip-absorber attempt cap
        int      sendInnerMaxBackoffMs;  // ZmqClient inner per-attempt backoff cap (ms)
    };

    static Config defaultConfig();

    // routeTable / labelRouteTable are the ZMQ-backed ProducerStateTables owned
    // by RouteSync. zmqClient is the shared client whose send-path counters are
    // folded into telemetry. stateDb is used only for the STATE_DB stat table.
    RouteSendCoalescer(ProducerStateTable *routeTable,
                       ProducerStateTable *labelRouteTable,
                       ZmqClient *zmqClient,
                       DBConnector *stateDb,
                       const Config &cfg = defaultConfig());
    ~RouteSendCoalescer();

    RouteSendCoalescer(const RouteSendCoalescer &) = delete;
    RouteSendCoalescer &operator=(const RouteSendCoalescer &) = delete;

    // Ingest side (main thread). Upsert a fully-formed KCO for the given table
    // (last-writer-wins) and wake the send thread. Never blocks on ZMQ.
    void upsertKco(TableId tbl, const KeyOpFieldsValuesTuple &kco);
    // Convenience for a SET with field-values, and for a DEL.
    void upsertSet(TableId tbl, const std::string &key,
                   const std::vector<FieldValueTuple> &values);
    void upsertDel(TableId tbl, const std::string &key);

    void start();   // launch the send thread (idempotent)
    void stop();     // signal + join the send thread (idempotent; drains best-effort)

    // Park the send thread between drain cycles so the caller owns the route
    // tables exclusively (warm-restart reconcile writes them directly). Unlike
    // stop() this retains the map and does not account undelivered entries as
    // lost. Blocks until the thread is parked; idempotent.
    void pause();
    void resume();

    // Drive exactly one fair drain cycle synchronously (no thread). Returns true
    // if it attempted a flush (map was non-empty). Exposed for deterministic tests.
    bool drainOnce();

    // ---- telemetry / introspection (lock-free reads) ----
    uint64_t mapDepth() const;
    uint64_t mapDepthHwm() const { return m_mapDepthHwm.load(std::memory_order_relaxed); }
    uint64_t routesSentTotal() const { return m_routesSentTotal.load(std::memory_order_relaxed); }
    uint64_t routesCoalescedTotal() const { return m_routesCoalescedTotal.load(std::memory_order_relaxed); }
    uint64_t chunksSentTotal() const { return m_chunksSentTotal.load(std::memory_order_relaxed); }
    uint64_t retryFromMapTotal() const { return m_retryFromMapTotal.load(std::memory_order_relaxed); }
    uint64_t congestionEpisodesTotal() const { return m_congestionEpisodesTotal.load(std::memory_order_relaxed); }
    uint64_t assertTotal() const { return m_assertTotal.load(std::memory_order_relaxed); }
    uint64_t routesLostTotal() const { return m_routesLostTotal.load(std::memory_order_relaxed); }

private:
    using CoalesceMap = std::map<std::string, KeyOpFieldsValuesTuple>;
    using SteadyClock = std::chrono::steady_clock;

    void sendLoop();
    // Drain one table's share of a cycle. Repeatedly pulls a chunk (bounded by
    // maxBatchEntries, maxBatchBytes and the remaining budget) from the live map
    // under the lock and set()s it with the lock released. A failed chunk is
    // re-merged last-writer-wins, so newer ingest is never clobbered, and false
    // is returned; chunks already sent in this call stay delivered. `budget` is
    // the table's depth at cycle start, which bounds the pass and keeps the two
    // tables fair. `sent` reports entries delivered, so the caller can detect a
    // cycle that made no progress. The pass resumes from the last key visited
    // rather than restarting at begin(), covering each key present at entry once.
    bool drainTable(TableId tbl, size_t budget, size_t &sent);
    // Worst stuck age across tables that still owe work, in ms; 0 when both maps
    // are empty. Tracked per table so a table whose sends fail forever is caught
    // while the other keeps flowing. Caller must hold m_mutex.
    uint64_t stuckMsLocked() const;
    static size_t tableIndex(TableId tbl) { return (tbl == TableId::Route) ? 0 : 1; }
    // Conservative estimate of a KCO's serialized wire size, used only to cap a
    // chunk's bytes below the ZMQ message ceiling.
    static size_t approxKcoSerializedBytes(const KeyOpFieldsValuesTuple &kco);
    void evaluateAssertThresholds();
    void writeAssertRecordAndExit(const char *reason, size_t depth, uint64_t stuckMs);
    void publishTelemetry(bool force);
    void onEpisodeStart();
    void onEpisodeRecovered();

    CoalesceMap &mapForLocked(TableId tbl);
    ProducerStateTable *tableFor(TableId tbl) const;
    size_t totalDepthLocked() const;

    ProducerStateTable *m_routeTable;
    ProducerStateTable *m_labelRouteTable;
    ZmqClient          *m_zmqClient;
    std::unique_ptr<Table> m_statTable;   // STATE_DB FPMSYNCD_ROUTE_STAT_TABLE|global
    Config              m_cfg;

    mutable std::mutex      m_mutex;      // guards both maps + episode/liveness state
    std::condition_variable m_cv;
    CoalesceMap             m_routeMap;
    CoalesceMap             m_labelMap;

    std::thread m_thread;
    bool        m_running{false};
    bool        m_stop{false};
    bool        m_paused{false};   // caller wants the send thread parked
    bool        m_parked{false};   // send thread is parked and not touching the maps
    std::condition_variable m_parkedCv;   // send thread -> pause() waiter

    // liveness / episode / telemetry timing (guarded by m_mutex unless atomic)
    // Per table, indexed by tableIndex(): a global timestamp would be refreshed
    // by whichever table is healthy and hide the other one being stuck forever.
    SteadyClock::time_point m_lastSuccess[2]{SteadyClock::now(), SteadyClock::now()};
    // When a table went from empty to owing work. A table idle since startup has
    // an arbitrarily old m_lastSuccess; ageing its first pending entry from that
    // would spend the whole stuck budget before a single retry.
    SteadyClock::time_point m_pendingSince[2]{SteadyClock::now(), SteadyClock::now()};
    SteadyClock::time_point m_lastTelemetry{};
    bool                    m_wasUnhealthy{false};  // drives the RECOVERED health edge

    // congestion-episode accumulators (current open episode)
    bool                    m_inEpisode{false};
    // Episode hysteresis: an episode opens only on the SECOND consecutive outer
    // failure, since a single stranded flush is a blip rather than congestion.
    // Touched only on the send thread / drainOnce; reset when a drain empties.
    int                     m_consecutiveOuterFailures{0};
    // Outcome and delivered-entry count of the most recent drain cycle; together
    // they drive the outer backoff. Atomic because drainOnce() is public and may
    // be driven from a test thread.
    std::atomic<bool>   m_lastCycleOk{true};
    std::atomic<size_t> m_lastCycleSent{0};
    SteadyClock::time_point m_epStart{};
    size_t                  m_epPeakDepth{0};

    // cumulative counters (atomic so getters/telemetry are lock-free)
    std::atomic<uint64_t> m_mapDepthHwm{0};
    std::atomic<uint64_t> m_routesSentTotal{0};
    std::atomic<uint64_t> m_routesCoalescedTotal{0};   // LWW overwrites of a still-pending key
    std::atomic<uint64_t> m_chunksSentTotal{0};         // successful wire chunks
    std::atomic<uint64_t> m_retryFromMapTotal{0};
    std::atomic<uint64_t> m_congestionEpisodesTotal{0};
    std::atomic<uint64_t> m_assertTotal{0};
    std::atomic<uint64_t> m_routesLostTotal{0};

    // last-episode record (published when an episode closes)
    std::atomic<uint64_t> m_epLastDurationMs{0};
    std::atomic<uint64_t> m_epLastPeakDepth{0};
    std::atomic<uint64_t> m_epLastCoalescedIn{0};
    std::atomic<uint64_t> m_epLastCoalescedOut{0};

    // in-flight episode coalesce accounting
    uint64_t m_epCoalescedIn{0};
    uint64_t m_epCoalescedOut{0};
};

}
