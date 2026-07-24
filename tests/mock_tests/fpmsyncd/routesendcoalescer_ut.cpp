/*
 * Unit tests for RouteSendCoalescer (sonic-buildimage #28369).
 *
 * The tests drive the coalescer deterministically via drainOnce() (no send
 * thread) and inject transient or permanent send failures through a
 * controllable ProducerStateTable subclass. Invariants covered:
 *   1. last-writer-wins coalescing collapses same-key churn,
 *   2. a transient chunked-send failure keeps the chunk in the map,
 *   3. the converged batch equals a full replay of per-key final state,
 *   4. the chunked drain splits a large backlog across bounded set() calls,
 *   5. episode hysteresis: one stranded chunk is a blip, an episode opens only
 *      on a second consecutive outer failure,
 *   6. the two independent assert triggers (STUCK_TIMEOUT, DEPTH_OVERFLOW) fire,
 *   7. a drain cycle is bounded by the depth sampled at entry, keeping the two
 *      tables fair under sustained churn on one, and the sweep covers the whole
 *      cycle-entry backlog even when arrivals sort below it,
 *   8. a table whose sends fail forever trips the liveness assert even while the
 *      other table keeps flowing.
 */
#include <atomic>
#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include "gtest/gtest.h"

#include "dbconnector.h"
#include "producerstatetable.h"
#include "redispipeline.h"
#include "table.h"
#include "fpmsyncd/routesendcoalescer.h"

using namespace swss;

namespace {

// A ProducerStateTable whose batched set() is fully controllable: it records
// every chunk it "sends", and can be told to throw (simulating ZmqClient
// exhausting its inner blip-absorber) for the next N calls.
//
// Built on the RedisPipeline ctor, the one fake_producerstatetable.cpp stubs for
// this binary. The DBConnector ctor is not stubbed: it builds a real pipeline and
// SCRIPT LOADs the apply-view script through mocked hiredis, which throws.
class FakeSendTable : public ProducerStateTable
{
public:
    FakeSendTable(RedisPipeline *pipeline, const std::string &tableName) :
        ProducerStateTable(pipeline, tableName, /*buffered=*/false)
    {
    }

    void set(const std::vector<KeyOpFieldsValuesTuple> &values) override
    {
        // Cleared on every exit path, including the injected throw.
        struct InSendGuard
        {
            std::atomic<bool> &flag;
            ~InSendGuard() { flag.store(false, std::memory_order_relaxed); }
        } guard{m_inSend};
        m_inSend.store(true, std::memory_order_relaxed);
        if (m_sendDelayMs > 0)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(m_sendDelayMs));
        }
        m_setCalls.fetch_add(1, std::memory_order_relaxed);
        // Fires while the coalescer holds no lock, so a hook may upsert into the
        // live map -- the way real ingest lands during a send.
        if (m_onSet)
        {
            m_onSet();
        }
        if (m_throwRemaining > 0)
        {
            --m_throwRemaining;
            throw std::system_error(std::make_error_code(std::errc::io_error),
                                    "injected transient send failure");
        }
        // Record the delivered batch as the new converged wire state.
        for (const auto &kco : values)
        {
            if (kfvOp(kco) == DEL_COMMAND)
            {
                m_delivered.erase(kfvKey(kco));
            }
            else
            {
                m_delivered[kfvKey(kco)] = kco;
            }
        }
        m_deliveredRows.fetch_add(values.size(), std::memory_order_relaxed);
    }

    void failNext(int n) { m_throwRemaining = n; }
    void failForever() { m_throwRemaining = 1 << 30; }
    // Hold each send open for ms, so a caller can observe a send in flight.
    void sendDelay(int ms) { m_sendDelayMs = ms; }
    bool inSend() const { return m_inSend.load(std::memory_order_relaxed); }
    // Run fn at the start of every set(), to simulate ingest arriving mid-drain.
    void onSet(std::function<void()> fn) { m_onSet = std::move(fn); }

    int throwRemaining() const { return m_throwRemaining; }
    uint64_t setCalls() const { return m_setCalls.load(); }
    uint64_t deliveredRows() const { return m_deliveredRows.load(); }
    const std::map<std::string, KeyOpFieldsValuesTuple> &delivered() const { return m_delivered; }

private:
    int m_throwRemaining{0};
    int m_sendDelayMs{0};
    std::atomic<bool> m_inSend{false};
    std::function<void()> m_onSet;
    std::atomic<uint64_t> m_setCalls{0};
    std::atomic<uint64_t> m_deliveredRows{0};
    std::map<std::string, KeyOpFieldsValuesTuple> m_delivered;
};

// Fixture: mocked DBConnector + a RedisPipeline over it (the sonic-swss
// mock_tests convention, see fpmsyncd/test_routesync.cpp), controllable route
// tables, no send thread.
class RouteSendCoalescerTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        m_db = std::make_unique<DBConnector>("STATE_DB", 0, true);
        m_pipeline = std::make_unique<RedisPipeline>(m_db.get());
        // The assert-`_Exit` death tests leave a sticky FPMSYNCD_ROUTE_STAT_TABLE|global
        // record in the shared STATE_DB, which the ctor seeds assert_total from
        // lifetime counter). Clear it so each test starts from an empty stat record,
        // independent of test-execution order.
        Table(m_db.get(), "FPMSYNCD_ROUTE_STAT_TABLE").del("global");
        m_route = std::make_unique<FakeSendTable>(m_pipeline.get(), "ROUTE_TABLE");
        m_label = std::make_unique<FakeSendTable>(m_pipeline.get(), "LABEL_ROUTE_TABLE");
    }

    // Build a coalescer with a tight config so assert triggers are testable.
    std::unique_ptr<RouteSendCoalescer> makeCoalescer(RouteSendCoalescer::Config cfg)
    {
        return std::make_unique<RouteSendCoalescer>(
            m_route.get(), m_label.get(), /*zmqClient=*/nullptr,
            /*stateDb=*/m_db.get(), cfg);
    }

    static RouteSendCoalescer::Config baseConfig()
    {
        auto c = RouteSendCoalescer::defaultConfig();
        c.telemetryMinIntervalMs = 0; // don't let throttling hide telemetry in tests
        return c;
    }

    static std::vector<FieldValueTuple> nh(const std::string &v)
    {
        return {{"nexthop", v}};
    }

    std::unique_ptr<DBConnector> m_db;
    std::unique_ptr<RedisPipeline> m_pipeline;
    std::unique_ptr<FakeSendTable> m_route;
    std::unique_ptr<FakeSendTable> m_label;
};

// 1. Last-writer-wins: N upserts of the same key collapse to ONE delivered row
//    carrying the final value.
TEST_F(RouteSendCoalescerTest, CoalescesSameKeyLastWriterWins)
{
    auto co = makeCoalescer(baseConfig());

    co->upsertSet(RouteSendCoalescer::TableId::Route, "1.1.1.0/24", nh("10.0.0.1"));
    co->upsertSet(RouteSendCoalescer::TableId::Route, "1.1.1.0/24", nh("10.0.0.2"));
    co->upsertSet(RouteSendCoalescer::TableId::Route, "1.1.1.0/24", nh("10.0.0.3"));

    EXPECT_EQ(co->mapDepth(), 1u); // three churns, one pending key

    ASSERT_TRUE(co->drainOnce());
    EXPECT_EQ(co->mapDepth(), 0u);
    EXPECT_EQ(m_route->setCalls(), 1u);       // single batched send
    EXPECT_EQ(m_route->deliveredRows(), 1u);  // one coalesced row
    ASSERT_EQ(m_route->delivered().count("1.1.1.0/24"), 1u);
    // Final value wins.
    const auto &kco = m_route->delivered().at("1.1.1.0/24");
    ASSERT_EQ(fvField(kfvFieldsValues(kco)[0]), "nexthop");
    EXPECT_EQ(fvValue(kfvFieldsValues(kco)[0]), "10.0.0.3");
    EXPECT_EQ(co->routesSentTotal(), 1u);
    EXPECT_EQ(co->routesCoalescedTotal(), 2u);  // 3 upserts -> 2 collapsed onto pending
    EXPECT_EQ(co->chunksSentTotal(), 1u);        // one wire chunk
}

// 2. Transient failure keeps the batch (lossless): a one-shot send failure must
//    leave every key pending; the next drain delivers them all. No route drop.
//    A single stranded chunk is a blip -> no episode opens.
TEST_F(RouteSendCoalescerTest, TransientFailureKeepsBatchNoLoss)
{
    auto co = makeCoalescer(baseConfig());

    co->upsertSet(RouteSendCoalescer::TableId::Route, "2.2.2.0/24", nh("10.0.0.1"));
    co->upsertSet(RouteSendCoalescer::TableId::Route, "3.3.3.0/24", nh("10.0.0.2"));
    ASSERT_EQ(co->mapDepth(), 2u);

    m_route->failNext(1); // first batched send throws
    ASSERT_TRUE(co->drainOnce());

    // Batch retained: nothing delivered, both keys still pending, retry counted,
    // but a single stranded chunk is a blip -> NO episode yet (hysteresis).
    EXPECT_EQ(m_route->deliveredRows(), 0u);
    EXPECT_EQ(co->mapDepth(), 2u);
    EXPECT_EQ(co->retryFromMapTotal(), 1u);
    EXPECT_EQ(co->congestionEpisodesTotal(), 0u);

    // Next drain succeeds -> both routes delivered.
    ASSERT_TRUE(co->drainOnce());
    EXPECT_EQ(co->mapDepth(), 0u);
    EXPECT_EQ(m_route->deliveredRows(), 2u);
    EXPECT_EQ(m_route->delivered().count("2.2.2.0/24"), 1u);
    EXPECT_EQ(m_route->delivered().count("3.3.3.0/24"), 1u);
    EXPECT_EQ(co->routesLostTotal(), 0u);
    EXPECT_EQ(co->congestionEpisodesTotal(), 0u); // one blip never became an episode
}

// 2c. Episode hysteresis: an episode opens only on the SECOND consecutive
//     outer failure; every failure still bumps retry_from_map_total.
TEST_F(RouteSendCoalescerTest, EpisodeOpensOnSecondConsecutiveFailure)
{
    auto co = makeCoalescer(baseConfig());
    co->upsertSet(RouteSendCoalescer::TableId::Route, "2.2.2.0/24", nh("10.0.0.1"));

    m_route->failNext(2); // fail the next two sends

    ASSERT_TRUE(co->drainOnce());                 // failure #1 -> blip, no episode
    EXPECT_EQ(co->retryFromMapTotal(), 1u);
    EXPECT_EQ(co->congestionEpisodesTotal(), 0u);

    ASSERT_TRUE(co->drainOnce());                 // failure #2 -> episode opens
    EXPECT_EQ(co->retryFromMapTotal(), 2u);
    EXPECT_EQ(co->congestionEpisodesTotal(), 1u);

    ASSERT_TRUE(co->drainOnce());                 // succeeds -> drains, episode closes
    EXPECT_EQ(co->mapDepth(), 0u);
    EXPECT_EQ(m_route->deliveredRows(), 1u);
    EXPECT_EQ(co->routesLostTotal(), 0u);
}

// 2b. Newer ingest during a failed send wins over the stranded batch
//     (re-merge must not clobber a fresher op for the same key).
TEST_F(RouteSendCoalescerTest, NewerIngestWinsOverStrandedBatch)
{
    auto co = makeCoalescer(baseConfig());

    co->upsertSet(RouteSendCoalescer::TableId::Route, "4.4.4.0/24", nh("old"));
    m_route->failNext(1);
    ASSERT_TRUE(co->drainOnce());        // batch [old] stranded back into the map
    EXPECT_EQ(co->mapDepth(), 1u);

    // Newer op for the same key arrives while stranded.
    co->upsertSet(RouteSendCoalescer::TableId::Route, "4.4.4.0/24", nh("new"));
    EXPECT_EQ(co->mapDepth(), 1u);       // still one key (coalesced)

    ASSERT_TRUE(co->drainOnce());        // succeeds now
    ASSERT_EQ(m_route->delivered().count("4.4.4.0/24"), 1u);
    const auto &kco = m_route->delivered().at("4.4.4.0/24");
    EXPECT_EQ(fvValue(kfvFieldsValues(kco)[0]), "new"); // fresher op won
}

// 3. Converged == replay: an arbitrary SET/DEL interleaving must converge to the
//    per-key final state (a DEL'd key absent, a re-SET key present with latest).
TEST_F(RouteSendCoalescerTest, ConvergedEqualsReplay)
{
    auto co = makeCoalescer(baseConfig());
    using T = RouteSendCoalescer::TableId;

    co->upsertSet(T::Route, "a", nh("1"));
    co->upsertSet(T::Route, "b", nh("1"));
    co->upsertDel(T::Route, "a");          // a: set then del -> absent
    co->upsertSet(T::Route, "c", nh("1"));
    co->upsertSet(T::Route, "b", nh("2"));  // b: latest value 2
    co->upsertDel(T::Route, "c");          // c: set then del -> absent
    co->upsertSet(T::Route, "c", nh("3"));  // c: re-set -> present value 3

    ASSERT_TRUE(co->drainOnce());
    EXPECT_EQ(co->mapDepth(), 0u);

    const auto &d = m_route->delivered();
    EXPECT_EQ(d.count("a"), 0u);                                   // deleted
    ASSERT_EQ(d.count("b"), 1u);
    EXPECT_EQ(fvValue(kfvFieldsValues(d.at("b"))[0]), "2");        // latest
    ASSERT_EQ(d.count("c"), 1u);
    EXPECT_EQ(fvValue(kfvFieldsValues(d.at("c"))[0]), "3");        // re-set wins
}

// 3b. Chunked drain: a backlog larger than maxBatchEntries is delivered
//     across multiple bounded set() calls, losslessly and in full.
TEST_F(RouteSendCoalescerTest, ChunkedDrainSplitsLargeBacklog)
{
    auto cfg = baseConfig();
    cfg.maxBatchEntries = 3;   // force multiple chunks
    auto co = makeCoalescer(cfg);

    for (int i = 0; i < 7; ++i)
    {
        co->upsertSet(RouteSendCoalescer::TableId::Route,
                      "10.0." + std::to_string(i) + ".0/24", nh("nh"));
    }
    ASSERT_EQ(co->mapDepth(), 7u);

    ASSERT_TRUE(co->drainOnce());
    EXPECT_EQ(co->mapDepth(), 0u);
    EXPECT_EQ(m_route->setCalls(), 3u);        // ceil(7/3) chunks
    EXPECT_EQ(co->chunksSentTotal(), 3u);
    EXPECT_EQ(m_route->deliveredRows(), 7u);   // every route delivered
    EXPECT_EQ(m_route->delivered().size(), 7u);
    EXPECT_EQ(co->routesSentTotal(), 7u);
}

// 3c. Byte-capped chunk: a chunk is also split when it would exceed
//     maxBatchBytes, but a single oversized KCO still makes progress alone.
TEST_F(RouteSendCoalescerTest, ByteCapSplitsChunk)
{
    auto cfg = baseConfig();
    cfg.maxBatchEntries = 1000;   // entry cap won't bind
    cfg.maxBatchBytes = 200;      // small byte cap forces per-entry chunks
    auto co = makeCoalescer(cfg);

    // Each KCO is ~ key + nexthop(~40B) + overhead, comfortably > 100B, so two
    // entries exceed the 200B cap and must land in separate chunks.
    co->upsertSet(RouteSendCoalescer::TableId::Route, "10.0.0.0/24",
                  nh("2001:db8:aaaa:bbbb:cccc:dddd:eeee:ffff"));
    co->upsertSet(RouteSendCoalescer::TableId::Route, "10.0.1.0/24",
                  nh("2001:db8:aaaa:bbbb:cccc:dddd:eeee:0001"));
    ASSERT_EQ(co->mapDepth(), 2u);

    ASSERT_TRUE(co->drainOnce());
    EXPECT_EQ(co->mapDepth(), 0u);
    EXPECT_EQ(m_route->setCalls(), 2u);        // one entry per chunk (byte cap)
    EXPECT_EQ(m_route->deliveredRows(), 2u);
}

// 4a. Assert on time trigger (STUCK_TIMEOUT): a permanently-failing send with a
//     non-empty map and tFailMs=0 must deliberately exit(EXIT_FAILURE).
TEST_F(RouteSendCoalescerTest, AssertOnTimeTrigger)
{
    auto cfg = baseConfig();
    cfg.tFailMs = 0;   // any elapsed time since last success trips the time guard
    cfg.mMax = 1000000;

    EXPECT_EXIT(
        {
            auto co = makeCoalescer(cfg);
            co->upsertSet(RouteSendCoalescer::TableId::Route, "5.5.5.0/24", nh("x"));
            m_route->failForever();
            // Let a few ms elapse since construction so the "stuck since last
            // success" measurement is non-zero and clears tFailMs=0.
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            co->drainOnce(); // flush fails -> map non-empty -> STUCK_TIMEOUT trips -> exit
        },
        ::testing::ExitedWithCode(EXIT_FAILURE), "");
}

// 4b. Assert on depth trigger (DEPTH_OVERFLOW): backlog beyond the cap must exit, even
//     without any send failure (pure depth guard).
TEST_F(RouteSendCoalescerTest, AssertOnDepthTrigger)
{
    auto cfg = baseConfig();
    cfg.tFailMs = 3600000; // don't let the time guard fire
    cfg.mMax = 2;          // trip once depth exceeds 2

    EXPECT_EXIT(
        {
            auto co = makeCoalescer(cfg);
            // Fail the send so the batch stays in the map and depth stays > mMax
            // when evaluateAssertThresholds() runs.
            m_route->failForever();
            co->upsertSet(RouteSendCoalescer::TableId::Route, "a", nh("1"));
            co->upsertSet(RouteSendCoalescer::TableId::Route, "b", nh("1"));
            co->upsertSet(RouteSendCoalescer::TableId::Route, "c", nh("1"));
            co->drainOnce(); // depth 3 > mMax 2 -> DEPTH_OVERFLOW trips -> exit
        },
        ::testing::ExitedWithCode(EXIT_FAILURE), "");
}

// 5. Route + label tables are drained independently in one cycle.
TEST_F(RouteSendCoalescerTest, DrainsBothTables)
{
    auto co = makeCoalescer(baseConfig());
    co->upsertSet(RouteSendCoalescer::TableId::Route, "6.6.6.0/24", nh("1"));
    co->upsertSet(RouteSendCoalescer::TableId::LabelRoute, "100", nh("2"));
    ASSERT_EQ(co->mapDepth(), 2u);

    ASSERT_TRUE(co->drainOnce());
    EXPECT_EQ(co->mapDepth(), 0u);
    EXPECT_EQ(m_route->deliveredRows(), 1u);
    EXPECT_EQ(m_label->deliveredRows(), 1u);
}

// 5b. Cross-table fairness: a drain cycle covers both tables and is bounded
//     by the depth each had at cycle entry, so route churn arriving mid-drain is
//     deferred to the next cycle and the label-route table is served in the same
//     one. The liveness assert cannot catch this starvation, because the
//     churning table keeps refreshing m_lastSuccess.
TEST_F(RouteSendCoalescerTest, DrainCycleIsBoundedAndFairUnderChurn)
{
    auto cfg = baseConfig();
    cfg.maxBatchEntries = 4; // force several chunks per cycle
    auto co = makeCoalescer(cfg);
    using T = RouteSendCoalescer::TableId;

    for (int i = 0; i < 12; i++)
    {
        co->upsertSet(T::Route, "10.0." + std::to_string(i) + ".0/24", nh("10.0.0.1"));
    }
    co->upsertSet(T::LabelRoute, "100", nh("2"));
    ASSERT_EQ(co->mapDepth(), 13u);

    // Every route chunk that goes out injects a brand-new route key, as FRR would
    // during a sustained burst. Capped so an unbounded drain fails the assertions
    // below rather than looping forever.
    int injected = 0;
    const int kMaxInjections = 6;
    m_route->onSet([&]() {
        if (injected >= kMaxInjections)
        {
            return;
        }
        co->upsertSet(T::Route, "172.16." + std::to_string(injected++) + ".0/24", nh("10.0.0.9"));
    });

    ASSERT_TRUE(co->drainOnce());

    // The cycle delivered exactly the route backlog sampled at entry (12 in
    // 3 chunks of 4) -- not the keys injected during it.
    EXPECT_EQ(m_route->deliveredRows(), 12u);
    EXPECT_EQ(m_route->setCalls(), 3u);
    EXPECT_EQ(injected, 3);

    // The label table was served in the same cycle despite the route churn.
    EXPECT_EQ(m_label->deliveredRows(), 1u);
    EXPECT_EQ(m_label->delivered().count("100"), 1u);

    // The mid-cycle arrivals are still pending, and a following cycle drains them.
    EXPECT_EQ(co->mapDepth(), 3u);
    m_route->onSet(nullptr);
    ASSERT_TRUE(co->drainOnce());
    EXPECT_EQ(co->mapDepth(), 0u);
    EXPECT_EQ(co->routesLostTotal(), 0u);
}

// 5c. The drain sweep resumes where it left off, so keys arriving mid-drain
//     that sort BELOW the walk position do not consume the cycle's budget and
//     starve the high end of the table. Every key queued at cycle entry is
//     delivered in that cycle.
TEST_F(RouteSendCoalescerTest, DrainSweepCoversBacklogUnderLowSortingChurn)
{
    auto cfg = baseConfig();
    cfg.maxBatchEntries = 4;
    auto co = makeCoalescer(cfg);
    using T = RouteSendCoalescer::TableId;

    std::vector<std::string> backlog;
    for (int i = 0; i < 12; i++)
    {
        // "10.0.*" sorts above the "0.0.*" keys injected below.
        auto key = "10.0." + std::to_string(i) + ".0/24";
        backlog.push_back(key);
        co->upsertSet(T::Route, key, nh("10.0.0.1"));
    }

    int injected = 0;
    const int kMaxInjections = 6;
    m_route->onSet([&]() {
        if (injected >= kMaxInjections)
        {
            return;
        }
        co->upsertSet(T::Route, "0.0." + std::to_string(injected++) + ".0/24", nh("10.0.0.9"));
    });

    ASSERT_TRUE(co->drainOnce());
    m_route->onSet(nullptr);

    // The whole entry backlog went out this cycle; the low-sorting arrivals did
    // not displace any of it.
    EXPECT_EQ(m_route->deliveredRows(), 12u);
    for (const auto &key : backlog)
    {
        EXPECT_EQ(m_route->delivered().count(key), 1u) << "starved key: " << key;
    }

    // The arrivals are still queued and the next cycle clears them.
    EXPECT_EQ(co->mapDepth(), static_cast<size_t>(injected));
    ASSERT_TRUE(co->drainOnce());
    EXPECT_EQ(co->mapDepth(), 0u);
    EXPECT_EQ(co->routesLostTotal(), 0u);
}

// 8. Liveness is per table: a table whose sends fail forever trips the
//    assert even while the other table keeps draining successfully. A single
//    global "last success" would be refreshed by the healthy table and hide it.
TEST_F(RouteSendCoalescerTest, StuckTableTripsAssertWhileOtherTableFlows)
{
    auto cfg = baseConfig();
    cfg.tFailMs = 0;   // any measurable stuck age trips the time guard
    cfg.mMax = 1000000;

    EXPECT_EXIT(
        {
            auto co = makeCoalescer(cfg);
            m_label->failForever();
            co->upsertSet(RouteSendCoalescer::TableId::LabelRoute, "100", nh("2"));
            co->upsertSet(RouteSendCoalescer::TableId::Route, "5.5.5.0/24", nh("x"));
            // Let the label table accrue a non-zero stuck age; the route table
            // drains in this same cycle and refreshes only its own timestamp.
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            co->drainOnce();
        },
        ::testing::ExitedWithCode(EXIT_FAILURE), "");
}

// A table idle since startup must still get its full stuck budget when its first
// entry fails to send. Ageing from the last successful send would spend the whole
// budget on time the table owed nothing, turning one transient failure into an exit.
TEST_F(RouteSendCoalescerTest, IdleTableGetsFullStuckBudgetOnFirstFailure)
{
    auto cfg = baseConfig();
    cfg.tFailMs = 50;
    cfg.mMax = 1000000;   // isolate: only the time guard can trip here

    auto co = makeCoalescer(cfg);
    // Both tables idle: no work is owed, so no guard may fire while this elapses.
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // First label route ever, and its send fails. Age must be measured from now,
    // not from the construction-time success stamp 200 ms ago.
    m_label->failForever();
    co->upsertSet(RouteSendCoalescer::TableId::LabelRoute, "100", nh("2"));
    co->drainOnce();

    EXPECT_EQ(co->mapDepth(), 1u);        // retained for retry, not dropped
    EXPECT_EQ(co->assertTotal(), 0u);     // and emphatically not an exit
}

// 6. drainOnce() on an empty map does no work and reports so.
TEST_F(RouteSendCoalescerTest, EmptyDrainIsNoop)
{
    auto co = makeCoalescer(baseConfig());
    EXPECT_FALSE(co->drainOnce());
    EXPECT_EQ(m_route->setCalls(), 0u);
    EXPECT_EQ(m_label->setCalls(), 0u);
}

// 7. start()/stop() are idempotent and the thread joins cleanly; a pre-loaded
//    map is drained by the send thread before stop returns.
TEST_F(RouteSendCoalescerTest, ThreadStartStopDrains)
{
    auto co = makeCoalescer(baseConfig());
    co->upsertSet(RouteSendCoalescer::TableId::Route, "7.7.7.0/24", nh("1"));
    co->start();
    co->start(); // idempotent
    // Give the send thread a moment to drain.
    for (int i = 0; i < 50 && co->mapDepth() != 0; ++i)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    EXPECT_EQ(co->mapDepth(), 0u);
    co->stop();
    co->stop(); // idempotent
    EXPECT_GE(m_route->deliveredRows(), 1u);
}

// 8. The STATE_DB telemetry record carries the operator-readable schema --
//    the derived `health`, `last_success_age_sec`, and the raw coalesce counters
//    the consumer derives ratios from. Only RAW counters go on the wire; no
//    producer-side ratio field. Drives a persistent failure + recovery so an
//    episode opens and closes, then reads the published record back from STATE_DB.
TEST_F(RouteSendCoalescerTest, TelemetryPublishesDerivedFields)
{
    auto co = makeCoalescer(baseConfig());

    // Coalesce a key, then strand it for two consecutive drains so an episode
    // opens (hysteresis), then recover so ep_coalesced_* are set.
    co->upsertSet(RouteSendCoalescer::TableId::Route, "8.8.8.0/24", nh("10.0.0.1"));
    co->upsertSet(RouteSendCoalescer::TableId::Route, "8.8.8.0/24", nh("10.0.0.2"));
    m_route->failNext(2);
    ASSERT_TRUE(co->drainOnce());   // failure #1 -> blip
    ASSERT_TRUE(co->drainOnce());   // failure #2 -> opens episode
    ASSERT_TRUE(co->drainOnce());   // delivers, closes episode -> publishTelemetry(true)
    co->drainOnce();                // empty drain -> publishTelemetry(false), throttle=0

    Table statTable(m_db.get(), "FPMSYNCD_ROUTE_STAT_TABLE");
    std::vector<FieldValueTuple> fvs;
    ASSERT_TRUE(statTable.get("global", fvs));

    std::map<std::string, std::string> rec;
    for (const auto &fv : fvs)
    {
        rec[fvField(fv)] = fvValue(fv);
    }

    // Derived + raw fields the consumer needs.
    ASSERT_EQ(rec.count("health"), 1u);
    ASSERT_EQ(rec.count("last_success_age_sec"), 1u);
    EXPECT_EQ(rec["chunks_sent_total"], "1");   // two failures, then one delivered chunk
    EXPECT_EQ(rec.count("zmq_eagain_total"), 0u); // no ZmqClient wired in this fixture
    // Raw episode in/out present; the producer does NOT emit a pre-derived ratio.
    EXPECT_EQ(rec["ep_coalesced_out"], "1");       // the one chunk that closed the episode
    EXPECT_EQ(rec.count("ep_coalesced_in"), 1u);
    EXPECT_EQ(rec.count("ep_coalesce_ratio"), 0u); // consumer derives it
    // Reorganized/renamed fields; legacy names gone.
    EXPECT_EQ(rec.count("last_success_ts"), 0u);   // replaced by last_success_age_sec
    EXPECT_EQ(rec.count("ep_last_duration_ms"), 0u); // renamed to ep_duration_ms
    EXPECT_EQ(rec.count("zmq_send_retry_total"), 0u);     // renamed to zmq_blip_absorbed_total
    EXPECT_EQ(rec.count("zmq_ladder_exhausted_total"), 0u); // dropped

    // Recovered/idle after a closed episode -> not stuck.
    EXPECT_EQ(rec["map_depth"], "0");
    EXPECT_TRUE(rec["health"] == "OK" || rec["health"] == "RECOVERED");
    EXPECT_EQ(rec["routes_coalesced_total"], "1"); // one op collapsed onto pending
}

// 9. The default config carries the inner blip-absorber caps the ctor wires
//    into ZmqClient::setSendRetryConfig (bounded ~10ms, not the ~41s default).
//    Guards against a silent regression of the wired values.
TEST_F(RouteSendCoalescerTest, DefaultConfigCarriesInnerRetryCaps)
{
    auto c = RouteSendCoalescer::defaultConfig();
    EXPECT_EQ(c.sendInnerMaxRetries, 2);
    EXPECT_EQ(c.sendInnerMaxBackoffMs, 5);
}

// 10. An empty-fields SET serializes identically to a DEL on the ZMQ
//     wire, so it must fail loudly rather than silently delete a route. Debug-only
//     assert (compiled out under NDEBUG). Both ingest entry points are guarded:
//     upsertSet (convenience) and upsertKco (the hot path routesync feeds).
#ifndef NDEBUG
TEST_F(RouteSendCoalescerTest, EmptyFieldSetAssertsInDebug)
{
    auto co = makeCoalescer(baseConfig());
    ASSERT_DEATH(
        {
            co->upsertSet(RouteSendCoalescer::TableId::Route, "9.9.9.0/24",
                          std::vector<FieldValueTuple>{});
        },
        "non-empty fields");
}

// 10b. Same invariant on the hot path: routesync feeds pre-formed KCOs via upsertKco,
//      so the empty-SET guard must fire there too.
TEST_F(RouteSendCoalescerTest, EmptyFieldSetOnKcoAssertsInDebug)
{
    auto co = makeCoalescer(baseConfig());
    KeyOpFieldsValuesTuple kco(
        "9.9.9.1/32", SET_COMMAND, std::vector<FieldValueTuple>{});
    ASSERT_DEATH(
        { co->upsertKco(RouteSendCoalescer::TableId::Route, kco); },
        "non-empty fields");
}
#endif

// 11. assert_total is a lifetime counter across restarts. A fresh
//     coalescer must seed it from the sticky STATE_DB record left by a prior
//     process rather than publishing 0 and clobbering the persisted value.
TEST_F(RouteSendCoalescerTest, SeedsAssertTotalFromStateDb)
{
    // Simulate a prior process having recorded 7 lifetime asserts.
    Table stat(m_db.get(), "FPMSYNCD_ROUTE_STAT_TABLE");
    stat.hset("global", "assert_total", "7");

    auto co = makeCoalescer(baseConfig());
    EXPECT_EQ(co->assertTotal(), 7u);

    // Clean up so the shared STATE_DB record doesn't leak into other tests.
    stat.hdel("global", "assert_total");
}

// 12. Warm-restart reconcile writes the route tables directly, so pause() must
//     park the send thread before returning and retain the pending map. stop()
//     is not usable here: it accounts undelivered entries as lost.
TEST_F(RouteSendCoalescerTest, PauseParksSendThreadAndRetainsMap)
{
    auto co = makeCoalescer(baseConfig());
    co->start();

    m_route->failForever();   // keep work pending so the thread stays busy
    co->upsertSet(RouteSendCoalescer::TableId::Route, "10.0.0.0/24", nh("1"));

    co->pause();   // blocks until parked

    // Parked: the entry is still owed, and nothing was counted as lost.
    EXPECT_EQ(co->mapDepth(), 1u);
    EXPECT_EQ(co->routesLostTotal(), 0u);

    // No sends occur while parked, even as ingest continues.
    const uint64_t sendsAtPause = m_route->setCalls();
    co->upsertSet(RouteSendCoalescer::TableId::Route, "10.0.1.0/24", nh("2"));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_EQ(m_route->setCalls(), sendsAtPause);
    EXPECT_EQ(co->mapDepth(), 2u);

    // Resuming lets the thread drain the retained work.
    m_route->failNext(0);   // stop failing
    co->resume();
    for (int i = 0; i < 100 && co->mapDepth() != 0; ++i)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    EXPECT_EQ(co->mapDepth(), 0u);
    EXPECT_EQ(co->routesLostTotal(), 0u);

    co->stop();
}

// 13. pause()/resume() are idempotent, and pause() on a coalescer whose thread
//     was never started must not block waiting for a park that cannot happen.
TEST_F(RouteSendCoalescerTest, PauseIsIdempotentAndSafeWhenNotRunning)
{
    auto co = makeCoalescer(baseConfig());

    co->pause();    // never started: returns immediately
    co->pause();    // idempotent
    co->resume();
    co->resume();   // idempotent

    // Still fully functional afterwards.
    co->upsertSet(RouteSendCoalescer::TableId::Route, "10.0.2.0/24", nh("3"));
    EXPECT_TRUE(co->drainOnce());
    EXPECT_EQ(co->mapDepth(), 0u);
}

// 14. The property warm-restart reconcile depends on: when pause() returns, the
//     send thread is not inside a send. Returning early would leave reconcile
//     writing the route tables concurrently with an in-flight producer send.
TEST_F(RouteSendCoalescerTest, PauseWaitsForAnInFlightSendToComplete)
{
    auto co = makeCoalescer(baseConfig());
    m_route->sendDelay(100);   // hold the send open long enough to catch it
    co->start();

    co->upsertSet(RouteSendCoalescer::TableId::Route, "10.0.0.0/24", nh("1"));

    // Wait until the thread is genuinely inside set(); otherwise the assertion
    // below would hold trivially and prove nothing.
    bool caught = false;
    for (int i = 0; i < 200 && !caught; ++i)
    {
        caught = m_route->inSend();
        if (!caught)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    ASSERT_TRUE(caught) << "precondition: never observed a send in flight";

    co->pause();
    EXPECT_FALSE(m_route->inSend());

    co->stop();
}

// 15. An entry too large for a ZMQ message can never be delivered. It must be
//     dropped and accounted rather than retried until the liveness guard fires,
//     and it must not block the deliverable entries behind it.
TEST_F(RouteSendCoalescerTest, UndeliverableOversizeEntryIsDroppedNotRetriedForever)
{
    auto cfg = baseConfig();
    cfg.maxWireBytes = 512;   // scale the ceiling down instead of building 16 MiB

    auto co = makeCoalescer(cfg);
    co->upsertSet(RouteSendCoalescer::TableId::Route, "10.0.0.0/24",
                  nh(std::string(4096, 'x')));          // over the ceiling
    co->upsertSet(RouteSendCoalescer::TableId::Route, "10.0.1.0/24", nh("1"));

    EXPECT_TRUE(co->drainOnce());

    EXPECT_EQ(co->mapDepth(), 0u);              // nothing left stuck
    EXPECT_EQ(co->routesLostTotal(), 1u);       // the oversize one, accounted
    EXPECT_EQ(m_route->deliveredRows(), 1u);    // the deliverable one still went
    EXPECT_EQ(m_route->delivered().count("10.0.1.0/24"), 1u);
    EXPECT_EQ(m_route->delivered().count("10.0.0.0/24"), 0u);
}

// 16. Entries still held when the send thread stops are gone for good: the map is
//     in-memory only. They must be accounted rather than silently discarded.
TEST_F(RouteSendCoalescerTest, StopWithUndrainableMapAccountsRoutesLost)
{
    auto co = makeCoalescer(baseConfig());
    m_route->failForever();   // the final best-effort drain cannot succeed either

    co->upsertSet(RouteSendCoalescer::TableId::Route, "10.0.0.0/24", nh("1"));
    co->upsertSet(RouteSendCoalescer::TableId::Route, "10.0.1.0/24", nh("2"));

    co->start();
    co->stop();

    EXPECT_EQ(co->routesLostTotal(), 2u);
    EXPECT_EQ(m_route->deliveredRows(), 0u);
}

// 17. A zero entry cap would build empty chunks, retire no budget and spin the
//     drain loop forever, so the ctor holds the floor at one entry per chunk.
TEST_F(RouteSendCoalescerTest, ZeroMaxBatchEntriesIsClampedToOne)
{
    auto cfg = baseConfig();
    cfg.maxBatchEntries = 0;

    auto co = makeCoalescer(cfg);
    co->upsertSet(RouteSendCoalescer::TableId::Route, "10.0.0.0/24", nh("1"));
    co->upsertSet(RouteSendCoalescer::TableId::Route, "10.0.1.0/24", nh("2"));

    EXPECT_TRUE(co->drainOnce());
    EXPECT_EQ(co->mapDepth(), 0u);
    EXPECT_EQ(m_route->deliveredRows(), 2u);
    EXPECT_EQ(m_route->setCalls(), 2u);   // clamped to one entry per chunk
}

} // namespace
