#include <atomic>
#include <chrono>
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
        }
        // Drain the consumer's m_toSync so subsequent drain() calls observe it
        // as empty (matches the contract a real orch would honor).
        consumer.m_toSync.clear();
    }

    std::atomic<int> doTaskCount{0};
    std::set<std::string> seenKeys;
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
    const size_t savedMaxBulk = gMaxBulkSize;
    gMaxBulkSize = 1;

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

    gMaxBulkSize = savedMaxBulk;
}
