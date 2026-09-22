#include <algorithm>
#include <atomic>
#include <chrono>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "gtest/gtest.h"
#include "schema.h"
#include "ut_helper.h"
#include "orch_zmq_config.h"
#include "dbconnector.h"
#include "mock_table.h"
#include "select.h"
#include "zmqclient.h"
#include "zmqproducerstatetable.h"
#include "zmqrouteserver.h"
#include "zmqrouteconsumerstatetable.h"

#define protected public
#include "orch.h"
#include "zmqrouteorch.h"
#undef protected

using namespace std;
using namespace swss;

extern size_t gMaxBulkSize;

namespace {

// Restores gMaxBulkSize on scope exit. The tests below lower it to force the
// mid-burst notify branch; without this, an early return or a throw between
// the set and the restore would leave the global clobbered for every later
// test in the process.
struct ScopedMaxBulkSize
{
    explicit ScopedMaxBulkSize(size_t v) : saved(gMaxBulkSize) { gMaxBulkSize = v; }
    ~ScopedMaxBulkSize() { gMaxBulkSize = saved; }
    size_t saved;
};

// Wait until pred() becomes true or deadlineMs elapses; returns the final value.
template <typename Pred>
bool waitFor(int deadlineMs, Pred pred)
{
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(deadlineMs);
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (pred())
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return pred();
}

// Minimal subclass of ZmqRouteOrch that records doTask invocations, so tests
// can assert that drain() forwards correctly without needing a full RouteOrch.
class RecordingZmqRouteOrch : public ZmqRouteOrch
{
public:
    RecordingZmqRouteOrch(swss::DBConnector *db,
                          const std::vector<table_name_with_pri_t> &tables,
                          ZmqRouteServer *zmqServer)
        : ZmqRouteOrch(db, tables, zmqServer)
    {
    }

    void doTask(ConsumerBase &consumer) override
    {
        ++doTaskCount;
        // Record the keys handed to doTask before clearing. m_toSync is cleared
        // below, so tests that want to verify specific entries must capture
        // them here. doTask runs only on the orch main thread, so no lock.
        for (const auto &kv : consumer.m_toSync)
        {
            seenKeys.insert(kv.first);
            lastFields[kv.first] = kfvFieldsValues(kv.second);
            ++totalEntries;
        }
        // Drain the consumer's m_toSync so subsequent drain() calls observe it
        // as empty (matches the contract a real orch would honor).
        consumer.m_toSync.clear();
    }

    std::atomic<int> doTaskCount{0};
    std::set<std::string> seenKeys;
    // Fields of the most recent delivery per key, captured before the clear
    // below. Main-thread only, like seenKeys.
    std::map<std::string, std::vector<swss::FieldValueTuple>> lastFields;
    // Total entries handed to doTask across all invocations. Compared against
    // seenKeys.size() it distinguishes "delivered once" from "delivered twice".
    std::atomic<int> totalEntries{0};
};

} // namespace

// ZmqRouteOrch with a nullptr server falls back to plain Consumer (legacy
// non-ZMQ path) for APPL_DB tables.
TEST(ZmqRouteOrchTest, NullServerFallsBackToConsumer)
{
    vector<table_name_with_pri_t> tables = {
        { "ZMQ_ROUTE_UT_T1", 1 },
        { "ZMQ_ROUTE_UT_T2", 2 },
    };
    auto app_db = make_shared<DBConnector>("APPL_DB", 0);
    auto orch = make_shared<ZmqRouteOrch>(app_db.get(), tables, nullptr);

    EXPECT_EQ(orch->getSelectables().size(), tables.size());
    // Ensure the executor is a plain Consumer (not a ZmqRouteConsumer): the
    // legacy fallback shouldn't pull in the ZmqRouteConsumer machinery.
    auto exec = orch->m_consumerMap.begin()->second.get();
    EXPECT_EQ(dynamic_cast<ZmqRouteConsumer *>(exec), nullptr);
}

// vector<string> ctor (no per-table priority) — exercises the
// default_orch_pri code path in ZmqRouteOrch::ZmqRouteOrch(vector<string>,...).
TEST(ZmqRouteOrchTest, VectorOfStringsCtor)
{
    vector<string> tables = { "ZMQ_ROUTE_UT_TS1", "ZMQ_ROUTE_UT_TS2" };
    auto app_db = make_shared<DBConnector>("APPL_DB", 0);
    auto orch = make_shared<ZmqRouteOrch>(app_db.get(), tables, nullptr);
    EXPECT_EQ(orch->getSelectables().size(), tables.size());
}

// Non-APPL_DB databases are unsupported; addConsumer should warn and create
// no executor.
TEST(ZmqRouteOrchTest, UnsupportedDbProducesNoExecutor)
{
    vector<table_name_with_pri_t> tables = { { "ZMQ_ROUTE_UT_T1", 1 } };
    auto state_db = make_shared<DBConnector>("STATE_DB", 0);
    auto orch = make_shared<ZmqRouteOrch>(state_db.get(), tables, nullptr);
    EXPECT_EQ(orch->getSelectables().size(), 0u);
}

// With a real ZmqRouteServer, ZmqRouteOrch creates a ZmqRouteConsumer (not a
// plain Consumer). The server must outlive the orch.
TEST(ZmqRouteOrchTest, RealServerCreatesZmqRouteConsumer)
{
    vector<table_name_with_pri_t> tables = { { "ZMQ_ROUTE_UT_T1", 1 } };
    auto app_db = make_shared<DBConnector>("APPL_DB", 0);
    ZmqRouteServer server("tcp://*:1260", "", /*lazyBind=*/true);

    auto orch = make_shared<ZmqRouteOrch>(app_db.get(), tables, &server);
    ASSERT_EQ(orch->getSelectables().size(), tables.size());

    auto exec = orch->m_consumerMap.begin()->second.get();
    EXPECT_NE(dynamic_cast<ZmqRouteConsumer *>(exec), nullptr);
}

// doTask(Consumer&) on the base ZmqRouteOrch is a stub that forwards to the
// virtual doTask(ConsumerBase&) — this is the only piece that ZmqRouteOrch
// itself implements (besides ctors / addConsumer). Cover it.
TEST(ZmqRouteOrchTest, DoTaskConsumerForwardsToConsumerBase)
{
    vector<table_name_with_pri_t> tables = { { "ZMQ_ROUTE_UT_T1", 1 } };
    auto app_db = make_shared<DBConnector>("APPL_DB", 0);
    auto orch = make_shared<RecordingZmqRouteOrch>(app_db.get(), tables, nullptr);

    auto *exec = orch->m_consumerMap.begin()->second.get();
    auto *consumer = dynamic_cast<Consumer *>(exec);
    ASSERT_NE(consumer, nullptr);

    // Forge a single entry into m_toSync so that the recording doTask can see
    // something and so the subsequent clear() actually does work. SyncMap is a
    // multimap, so use insert rather than operator[].
    consumer->m_toSync.insert({
        "k1",
        std::make_tuple(std::string("k1"), std::string(SET_COMMAND),
                        std::vector<FieldValueTuple>{{"f", "v"}})
    });

    // ZmqRouteOrch::doTask(Consumer&) forwards to doTask(ConsumerBase&).
    static_cast<ZmqRouteOrch *>(orch.get())->doTask(*consumer);
    EXPECT_EQ(orch->doTaskCount.load(), 1);
    EXPECT_TRUE(consumer->m_toSync.empty());
}

// Drain on a ZmqRouteConsumer with empty m_toSync must NOT call doTask.
// Drain on a non-empty m_toSync must call doTask exactly once and the lock
// must allow re-entry afterwards.
TEST(ZmqRouteConsumerTest, DrainGatedByToSyncEmptiness)
{
    vector<table_name_with_pri_t> tables = { { "ZMQ_ROUTE_UT_T1", 1 } };
    auto app_db = make_shared<DBConnector>("APPL_DB", 0);
    ZmqRouteServer server("tcp://*:1261", "", /*lazyBind=*/true);
    auto orch = make_shared<RecordingZmqRouteOrch>(app_db.get(), tables, &server);

    auto *exec = orch->m_consumerMap.begin()->second.get();
    auto *zrc = dynamic_cast<ZmqRouteConsumer *>(exec);
    ASSERT_NE(zrc, nullptr);

    // Empty m_toSync: drain is a no-op.
    zrc->drain();
    EXPECT_EQ(orch->doTaskCount.load(), 0);

    // Stage one entry via addToSync; drain forwards to doTask exactly once.
    // RecordingZmqRouteOrch::doTask clears m_toSync.
    KeyOpFieldsValuesTuple kfv("route_a", SET_COMMAND,
                               vector<FieldValueTuple>{{"f", "v"}});
    zrc->addToSync(kfv);
    EXPECT_EQ(zrc->m_toSync.size(), 1u);

    zrc->drain();
    EXPECT_EQ(orch->doTaskCount.load(), 1);
    EXPECT_TRUE(zrc->m_toSync.empty());

    // A subsequent empty drain still doesn't call doTask, and the lock
    // re-acquires cleanly.
    zrc->drain();
    EXPECT_EQ(orch->doTaskCount.load(), 1);
}

// execute() drains m_ingress into m_toSync then calls drain(); with m_ingress
// empty here it reduces to drain(). Cover that override.
TEST(ZmqRouteConsumerTest, ExecuteDelegatesToDrain)
{
    vector<table_name_with_pri_t> tables = { { "ZMQ_ROUTE_UT_T1", 1 } };
    auto app_db = make_shared<DBConnector>("APPL_DB", 0);
    ZmqRouteServer server("tcp://*:1262", "", /*lazyBind=*/true);
    auto orch = make_shared<RecordingZmqRouteOrch>(app_db.get(), tables, &server);

    auto *zrc = dynamic_cast<ZmqRouteConsumer *>(
        orch->m_consumerMap.begin()->second.get());
    ASSERT_NE(zrc, nullptr);

    KeyOpFieldsValuesTuple kfv("route_b", SET_COMMAND,
                               vector<FieldValueTuple>{{"f", "v"}});
    zrc->addToSync(kfv);

    zrc->execute();
    EXPECT_EQ(orch->doTaskCount.load(), 1);
}

// Deque-form addToSync forwards to ConsumerBase::addToSync(deque) and returns
// the count.
TEST(ZmqRouteConsumerTest, AddToSyncDequeReturnsCount)
{
    vector<table_name_with_pri_t> tables = { { "ZMQ_ROUTE_UT_T1", 1 } };
    auto app_db = make_shared<DBConnector>("APPL_DB", 0);
    ZmqRouteServer server("tcp://*:1263", "", /*lazyBind=*/true);
    auto orch = make_shared<RecordingZmqRouteOrch>(app_db.get(), tables, &server);

    auto *zrc = dynamic_cast<ZmqRouteConsumer *>(
        orch->m_consumerMap.begin()->second.get());
    ASSERT_NE(zrc, nullptr);

    std::deque<KeyOpFieldsValuesTuple> entries;
    for (int i = 0; i < 5; ++i)
    {
        entries.emplace_back("k" + std::to_string(i), SET_COMMAND,
                             vector<FieldValueTuple>{{"f", "v"}});
    }

    EXPECT_EQ(zrc->addToSync(entries), 5u);
    EXPECT_EQ(zrc->m_toSync.size(), 5u);
}

// dumpPendingTasks returns the staged entries as strings.
TEST(ZmqRouteConsumerTest, DumpPendingTasksReturnsEntries)
{
    vector<table_name_with_pri_t> tables = { { "ZMQ_ROUTE_UT_T1", 1 } };
    auto app_db = make_shared<DBConnector>("APPL_DB", 0);
    ZmqRouteServer server("tcp://*:1264", "", /*lazyBind=*/true);
    auto orch = make_shared<RecordingZmqRouteOrch>(app_db.get(), tables, &server);

    auto *zrc = dynamic_cast<ZmqRouteConsumer *>(
        orch->m_consumerMap.begin()->second.get());
    ASSERT_NE(zrc, nullptr);

    zrc->addToSync(KeyOpFieldsValuesTuple("kA", SET_COMMAND,
                                          vector<FieldValueTuple>{{"f", "v"}}));
    zrc->addToSync(KeyOpFieldsValuesTuple("kB", DEL_COMMAND,
                                          vector<FieldValueTuple>{}));

    std::vector<std::string> ts;
    zrc->dumpPendingTasks(ts);
    EXPECT_EQ(ts.size(), 2u);
}

// End-to-end: ZmqProducerStateTable → ZmqRouteServer → ZmqRouteConsumer.
// The ingress callback (on mqPollThread) stages the tuple into m_ingress;
// execute() on this thread then drains m_ingress into m_toSync and forwards it
// to doTask. Verifies the callback wiring set up by ZmqRouteConsumer's
// constructor delivers the entry all the way through to doTask.
TEST(ZmqRouteConsumerTest, IngressCallbackDeliversToDoTask)
{
    const string tableName = "ZMQ_ROUTE_UT_INGRESS";
    const string pushEndpoint = "tcp://localhost:1266";
    const string pullEndpoint = "tcp://*:1266";

    vector<table_name_with_pri_t> tables = { { tableName, 1 } };
    auto app_db = make_shared<DBConnector>("APPL_DB", 0);
    ZmqRouteServer server(pullEndpoint, "", /*lazyBind=*/true);
    auto orch = make_shared<RecordingZmqRouteOrch>(app_db.get(), tables, &server);
    auto *zrc = dynamic_cast<ZmqRouteConsumer *>(
        orch->m_consumerMap.begin()->second.get());
    ASSERT_NE(zrc, nullptr);

    server.bind();

    ZmqClient client(pushEndpoint, 0);
    ZmqProducerStateTable p(app_db.get(), tableName, client, /*dbPersistence=*/false);
    p.set("route_x", vector<FieldValueTuple>{{"nh", "1.1.1.1"}});

    // The tuple arrives asynchronously on mqPollThread and lands in m_ingress.
    // Repeatedly run execute() (drains m_ingress → m_toSync → doTask) until
    // doTask observes the entry, then confirm the specific key was delivered.
    ASSERT_TRUE(waitFor(2000, [&] {
        zrc->execute();
        return orch->doTaskCount.load() >= 1;
    }));
    EXPECT_EQ(orch->seenKeys.count("route_x"), 1u);
    // The staging map must drain fully into m_toSync; a leak here would be
    // invisible to the m_toSync-side asserts.
    EXPECT_TRUE(zrc->m_ingress.empty());
}

// When the ingress callback stages past gMaxBulkSize entries, it must fire
// notifyPending mid-burst (rather than waiting for the burst quiesce timer)
// so the orch main loop wakes up and drains immediately. We lower
// gMaxBulkSize to 1 to make this trivially observable.
TEST(ZmqRouteConsumerTest, IngressCallbackFiresNotifyAtMaxBulkSize)
{
    const string tableName = "ZMQ_ROUTE_UT_BULK";
    const string pushEndpoint = "tcp://localhost:1267";
    const string pullEndpoint = "tcp://*:1267";

    vector<table_name_with_pri_t> tables = { { tableName, 1 } };
    auto app_db = make_shared<DBConnector>("APPL_DB", 0);
    ZmqRouteServer server(pullEndpoint, "", /*lazyBind=*/true);
    auto orch = make_shared<RecordingZmqRouteOrch>(app_db.get(), tables, &server);
    auto *zrc = dynamic_cast<ZmqRouteConsumer *>(
        orch->m_consumerMap.begin()->second.get());
    ASSERT_NE(zrc, nullptr);

    server.bind();

    // Force the mid-burst notify branch to trip on the very first callback.
    ScopedMaxBulkSize maxBulkGuard(1);

    Select sel;
    sel.addSelectable(zrc);

    ZmqClient client(pushEndpoint, 0);
    ZmqProducerStateTable p(app_db.get(), tableName, client, /*dbPersistence=*/false);
    p.set("route_bulk", vector<FieldValueTuple>{{"nh", "2.2.2.2"}});

    // The ingress callback fires notifyPending the moment m_ingress reaches
    // gMaxBulkSize=1, so the Select loop wakes on the consumer's event without
    // waiting for the BURST_QUIESCE_MS post-burst notify.
    Selectable *out = nullptr;
    EXPECT_EQ(sel.select(&out, 2000), Select::OBJECT);
    EXPECT_EQ(out, zrc);
}

// The producer (and hence the mqPollThread ingress callback) keeps staging into
// m_ingress while the orch main thread is inside execute()/drain()/doTask().
// This is the concurrent pair in this design: the callback holds m_ingressMutex
// only to stage, and execute() releases it before drain(), so doTask never runs
// under the lock. Assert that the overlap loses nothing, duplicates nothing and
// does not deadlock.
TEST(ZmqRouteConsumerTest, IngressCallbackConcurrentWithDrain)
{
    const string tableName = "ZMQ_ROUTE_UT_CONCURRENT";
    const string pushEndpoint = "tcp://localhost:1268";
    const string pullEndpoint = "tcp://*:1268";
    constexpr int kRoutes = 1000;

    vector<table_name_with_pri_t> tables = { { tableName, 1 } };
    auto app_db = make_shared<DBConnector>("APPL_DB", 0);
    ZmqRouteServer server(pullEndpoint, "", /*lazyBind=*/true);
    auto orch = make_shared<RecordingZmqRouteOrch>(app_db.get(), tables, &server);
    auto *zrc = dynamic_cast<ZmqRouteConsumer *>(
        orch->m_consumerMap.begin()->second.get());
    ASSERT_NE(zrc, nullptr);

    server.bind();

    ZmqClient client(pushEndpoint, 0);
    ZmqProducerStateTable p(app_db.get(), tableName, client, /*dbPersistence=*/false);

    std::atomic<bool> producerDone{false};
    std::thread producer([&] {
        for (int i = 0; i < kRoutes; ++i)
        {
            p.set("10.0." + to_string(i / 256) + "." + to_string(i % 256) + "/32",
                  vector<FieldValueTuple>{{"nh", "3.3.3.3"}});
        }
        producerDone = true;
    });

    // Spin execute() while the producer is still pushing, so staging and
    // draining genuinely interleave rather than running back to back.
    waitFor(30000, [&] {
        zrc->execute();
        return producerDone.load() && orch->seenKeys.size() == static_cast<size_t>(kRoutes);
    });
    producer.join();
    // Final pass for anything staged after the last predicate evaluation.
    ASSERT_TRUE(waitFor(5000, [&] {
        zrc->execute();
        return orch->seenKeys.size() == static_cast<size_t>(kRoutes);
    }));

    // Every key delivered, each exactly once (keys are unique per producer
    // iteration, so a duplicate delivery would show up as totalEntries drift).
    EXPECT_EQ(orch->seenKeys.size(), static_cast<size_t>(kRoutes));
    EXPECT_EQ(orch->totalEntries.load(), kRoutes);
    EXPECT_TRUE(zrc->m_toSync.empty());
    EXPECT_TRUE(zrc->m_ingress.empty());

    // Leak trap: if execute() ever left entries behind in m_ingress, this
    // extra pass would resurface them and grow the delivery count.
    zrc->execute();
    EXPECT_EQ(orch->totalEntries.load(), kRoutes);
}

// Two updates to the SAME key inside one ZMQ message exercise the
// m_ingress[key] = *kco overwrite: staging is last-writer-wins, wholesale.
// That is deliberately different from ConsumerBase::addToSync()'s field-union
// merge -- the route producer sends full replaces, so the last update must
// win with exactly its own fields, never a union with the superseded one.
TEST(ZmqRouteConsumerTest, SameKeyBurstCoalescesInIngress)
{
    const string tableName = "ZMQ_ROUTE_UT_COALESCE";
    const string pushEndpoint = "tcp://localhost:1271";
    const string pullEndpoint = "tcp://*:1271";

    vector<table_name_with_pri_t> tables = { { tableName, 1 } };
    auto app_db = make_shared<DBConnector>("APPL_DB", 0);
    ZmqRouteServer server(pullEndpoint, "", /*lazyBind=*/true);
    auto orch = make_shared<RecordingZmqRouteOrch>(app_db.get(), tables, &server);
    auto *zrc = dynamic_cast<ZmqRouteConsumer *>(
        orch->m_consumerMap.begin()->second.get());
    ASSERT_NE(zrc, nullptr);

    server.bind();

    ZmqClient client(pushEndpoint, 0);
    ZmqProducerStateTable p(app_db.get(), tableName, client, /*dbPersistence=*/false);

    // One batched set() -> one ZMQ message -> one handleReceivedData() -> one
    // ingress-callback invocation, so both tuples deterministically meet in
    // m_ingress with no execute() possible in between.
    std::vector<KeyOpFieldsValuesTuple> burst = {
        {"route_c", SET_COMMAND, {{"nh", "1.1.1.1"}}},
        {"route_c", SET_COMMAND, {{"ifname", "Ethernet0"}}},
    };
    p.set(burst);

    ASSERT_TRUE(waitFor(2000, [&] {
        zrc->execute();
        return orch->doTaskCount.load() >= 1;
    }));

    // Exactly one delivery for the key -- the burst coalesced in staging.
    EXPECT_EQ(orch->seenKeys.count("route_c"), 1u);
    EXPECT_EQ(orch->totalEntries.load(), 1);

    // And it carries only the second update's fields: no {nh} survivor.
    const auto &fields = orch->lastFields["route_c"];
    ASSERT_EQ(fields.size(), 1u);
    EXPECT_EQ(fvField(fields[0]), "ifname");
    EXPECT_EQ(fvValue(fields[0]), "Ethernet0");

    EXPECT_TRUE(zrc->m_ingress.empty());
}

// A single staged tuple below gMaxBulkSize must still wake the Select loop:
// mqPollThread fires notifyPending() once the burst quiesces (BURST_QUIESCE_MS
// in swss-common's ZmqRouteServer). At the default bulk size this quiesce
// notify is the common wake path; the mid-burst threshold branch is covered
// by IngressCallbackFiresNotifyAtMaxBulkSize above.
TEST(ZmqRouteConsumerTest, SelectWakesAfterBurstQuiesce)
{
    const string tableName = "ZMQ_ROUTE_UT_QUIESCE";
    const string pushEndpoint = "tcp://localhost:1272";
    const string pullEndpoint = "tcp://*:1272";

    vector<table_name_with_pri_t> tables = { { tableName, 1 } };
    auto app_db = make_shared<DBConnector>("APPL_DB", 0);
    ZmqRouteServer server(pullEndpoint, "", /*lazyBind=*/true);
    auto orch = make_shared<RecordingZmqRouteOrch>(app_db.get(), tables, &server);
    auto *zrc = dynamic_cast<ZmqRouteConsumer *>(
        orch->m_consumerMap.begin()->second.get());
    ASSERT_NE(zrc, nullptr);

    server.bind();

    // Make the mid-burst threshold branch unreachable: one staged entry is
    // far below 1000, so any wake below must come from the quiesce notify.
    ScopedMaxBulkSize maxBulkGuard(1000);

    Select sel;
    sel.addSelectable(zrc);

    ZmqClient client(pushEndpoint, 0);
    ZmqProducerStateTable p(app_db.get(), tableName, client, /*dbPersistence=*/false);
    p.set("route_q", vector<FieldValueTuple>{{"nh", "3.3.3.3"}});

    Selectable *out = nullptr;
    EXPECT_EQ(sel.select(&out, 2000), Select::OBJECT);
    EXPECT_EQ(out, zrc);

    // Drain and confirm the tuple made it through end to end.
    ASSERT_TRUE(waitFor(2000, [&] {
        zrc->execute();
        return orch->seenKeys.count("route_q") == 1;
    }));
    EXPECT_TRUE(zrc->m_ingress.empty());
}

// dumpPendingTasks must also report tuples still staged in m_ingress (arrived
// via the poll thread but not yet drained by execute()), so warm-reboot
// restore validation and debug dumps see the complete pending set rather than
// just m_toSync.
TEST(ZmqRouteConsumerTest, DumpPendingTasksIncludesStagedIngress)
{
    vector<table_name_with_pri_t> tables = { { "ZMQ_ROUTE_UT_DUMPI", 1 } };
    auto app_db = make_shared<DBConnector>("APPL_DB", 0);
    ZmqRouteServer server("tcp://*:1273", "", /*lazyBind=*/true);
    auto orch = make_shared<RecordingZmqRouteOrch>(app_db.get(), tables, &server);
    auto *zrc = dynamic_cast<ZmqRouteConsumer *>(
        orch->m_consumerMap.begin()->second.get());
    ASSERT_NE(zrc, nullptr);

    // One tuple staged in m_ingress, one already merged into m_toSync.
    {
        std::lock_guard<std::mutex> lk(zrc->m_ingressMutex);
        zrc->m_ingress["staged_key"] = KeyOpFieldsValuesTuple(
            "staged_key", SET_COMMAND, vector<FieldValueTuple>{{"f", "v"}});
    }
    zrc->addToSync(KeyOpFieldsValuesTuple(
        "merged_key", SET_COMMAND, vector<FieldValueTuple>{{"f", "v"}}));

    auto contains = [](const std::vector<std::string> &ts, const string &needle) {
        return std::any_of(ts.begin(), ts.end(), [&](const string &s) {
            return s.find(needle) != string::npos;
        });
    };

    std::vector<std::string> ts;
    zrc->dumpPendingTasks(ts);
    ASSERT_EQ(ts.size(), 2u);
    EXPECT_TRUE(contains(ts, "staged_key"));
    EXPECT_TRUE(contains(ts, "merged_key"));

    // The Orch-level aggregation dispatches through ConsumerBase* -- the
    // virtual hook must reach the override and see both entries as well.
    std::vector<std::string> ots;
    orch->dumpPendingTasks(ots);
    ASSERT_EQ(ots.size(), 2u);
    EXPECT_TRUE(contains(ots, "staged_key"));
    EXPECT_TRUE(contains(ots, "merged_key"));
}

// Two-batch retry-cache handoff: a failed SET parked in the retry cache is
// evicted and merged when the *coalesced* final tuple of a later burst reaches
// addToSync(). With complete route tuples, merge(cached SET, final SET)
// reduces to exactly the final SET's contents, so coalescing away the
// intermediate SET of the burst does not change the programmed result.
TEST(ZmqRouteConsumerTest, RetryCacheTwoBatchHandoffAfterCoalescing)
{
    const string tableName = "ZMQ_ROUTE_UT_RETRY1";
    vector<table_name_with_pri_t> tables = { { tableName, 1 } };
    auto app_db = make_shared<DBConnector>("APPL_DB", 0);
    ZmqRouteServer server("tcp://*:1274", "", /*lazyBind=*/true);
    auto orch = make_shared<RecordingZmqRouteOrch>(app_db.get(), tables, &server);
    auto *zrc = dynamic_cast<ZmqRouteConsumer *>(
        orch->m_consumerMap.begin()->second.get());
    ASSERT_NE(zrc, nullptr);

    orch->createRetryCache(tableName);
    auto *cache = orch->getRetryCache(tableName);
    ASSERT_NE(cache, nullptr);

    const string key = "10.1.0.0/16";
    // Complete route tuple, mirroring the fpmsyncd ZMQ contract of every SET
    // carrying the full field set.
    auto fullSet = [&](const string &nh, const string &ifn) {
        return KeyOpFieldsValuesTuple(key, SET_COMMAND,
            vector<FieldValueTuple>{
                {"protocol", "sharp"}, {"nexthop", nh},
                {"ifname", ifn}, {"weight", "1"}});
    };

    // Batch 1: SET A failed in doTask and was parked in the retry cache.
    Constraint cst = make_constraint(RETRY_CST_PIC, "ctx1");
    cache->insert(fullSet("10.0.0.1", "Ethernet0"), cst);
    ASSERT_EQ(cache->getRetryMap().count(key), 1u);

    // Batch 2: SET B then SET C stage into m_ingress; last writer wins.
    {
        std::lock_guard<std::mutex> lk(zrc->m_ingressMutex);
        zrc->m_ingress[key] = fullSet("10.0.0.2", "Ethernet4");
        zrc->m_ingress[key] = fullSet("10.0.0.3", "Ethernet8");
    }
    ASSERT_EQ(zrc->m_ingress.size(), 1u);

    zrc->execute();

    // The coalesced SET C took the count==1 SET/SET path in
    // addToSyncInternal(): cached SET A evicted and merged, one delivery.
    EXPECT_TRUE(cache->getRetryMap().empty());
    EXPECT_EQ(orch->totalEntries.load(), 1);
    ASSERT_EQ(orch->seenKeys.count(key), 1u);

    // Nothing from SET A (or the coalesced-away SET B) survives the merge:
    // exactly SET C's four fields, with SET C's values.
    const auto &fields = orch->lastFields[key];
    ASSERT_EQ(fields.size(), 4u);
    std::map<string, string> fm;
    for (const auto &fv : fields)
        fm[fvField(fv)] = fvValue(fv);
    EXPECT_EQ(fm["protocol"], "sharp");
    EXPECT_EQ(fm["nexthop"], "10.0.0.3");
    EXPECT_EQ(fm["ifname"], "Ethernet8");
    EXPECT_EQ(fm["weight"], "1");
}

// The count==2 (cached DEL + SET) retry-cache branch stays reachable with
// ingress coalescing: coalescing is per staging window only, so a DEL and a
// SET parked across earlier batches still meet a later coalesced SET in
// addToSyncInternal().
TEST(ZmqRouteConsumerTest, RetryCacheDelPlusSetReachableAfterCoalescing)
{
    const string tableName = "ZMQ_ROUTE_UT_RETRY2";
    vector<table_name_with_pri_t> tables = { { tableName, 1 } };
    auto app_db = make_shared<DBConnector>("APPL_DB", 0);
    ZmqRouteServer server("tcp://*:1275", "", /*lazyBind=*/true);
    auto orch = make_shared<RecordingZmqRouteOrch>(app_db.get(), tables, &server);
    auto *zrc = dynamic_cast<ZmqRouteConsumer *>(
        orch->m_consumerMap.begin()->second.get());
    ASSERT_NE(zrc, nullptr);

    orch->createRetryCache(tableName);
    auto *cache = orch->getRetryCache(tableName);
    ASSERT_NE(cache, nullptr);

    const string key = "10.2.0.0/16";
    auto fullSet = [&](const string &nh, const string &ifn) {
        return KeyOpFieldsValuesTuple(key, SET_COMMAND,
            vector<FieldValueTuple>{
                {"protocol", "sharp"}, {"nexthop", nh},
                {"ifname", ifn}, {"weight", "1"}});
    };

    // Earlier batches parked a DEL and a newer SET for the key.
    Constraint cst = make_constraint(RETRY_CST_PIC, "ctx2");
    cache->insert(KeyOpFieldsValuesTuple(key, DEL_COMMAND,
                                         vector<FieldValueTuple>{}), cst);
    cache->insert(fullSet("10.0.0.1", "Ethernet0"), cst);
    ASSERT_EQ(cache->getRetryMap().count(key), 2u);

    // A later burst coalesces to one final SET and reaches addToSync().
    {
        std::lock_guard<std::mutex> lk(zrc->m_ingressMutex);
        zrc->m_ingress[key] = fullSet("10.0.0.9", "Ethernet12");
    }
    zrc->execute();

    // count==2 / SET path: the cached SET is evicted and merged with the new
    // tuple; the cached DEL stays parked awaiting its constraint.
    ASSERT_EQ(cache->getRetryMap().count(key), 1u);
    EXPECT_EQ(kfvOp(cache->getRetryMap().find(key)->second.second), DEL_COMMAND);
    EXPECT_EQ(orch->totalEntries.load(), 1);

    const auto &fields = orch->lastFields[key];
    ASSERT_EQ(fields.size(), 4u);
    std::map<string, string> fm;
    for (const auto &fv : fields)
        fm[fvField(fv)] = fvValue(fv);
    EXPECT_EQ(fm["nexthop"], "10.0.0.9");
    EXPECT_EQ(fm["ifname"], "Ethernet12");
}
