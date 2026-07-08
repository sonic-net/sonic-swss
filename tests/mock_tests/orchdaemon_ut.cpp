#define protected public
#include "orch.h"
#include "orchdaemon.h"
#undef protected
#include "dbconnector.h"
#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include "mock_sai_switch.h"
#include "saihelper.h"

extern sai_switch_api_t* sai_switch_api;
sai_switch_api_t test_sai_switch;

namespace orchdaemon_test
{

    using ::testing::_;
    using ::testing::Return;
    using ::testing::StrictMock;
    using ::testing::InSequence;

    DBConnector appl_db("APPL_DB", 0);
    DBConnector state_db("STATE_DB", 0);
    DBConnector config_db("CONFIG_DB", 0);
    DBConnector counters_db("COUNTERS_DB", 0);

    class OrchDaemonTest : public ::testing::Test
    {
        public:
            StrictMock<MockSaiSwitch> mock_sai_switch_;

            OrchDaemon* orchd;

            OrchDaemonTest()
            {
                mock_sai_switch = &mock_sai_switch_;
                sai_switch_api = &test_sai_switch;
                sai_switch_api->get_switch_attribute = &mock_get_switch_attribute;
                sai_switch_api->set_switch_attribute = &mock_set_switch_attribute;

                orchd = new OrchDaemon(&appl_db, &config_db, &state_db, &counters_db, nullptr);

            };

            ~OrchDaemonTest()
            {
                sai_switch_api = nullptr;
                delete orchd;
            };

    };

    TEST_F(OrchDaemonTest, logRotate)
    {
        EXPECT_CALL(mock_sai_switch_, set_switch_attribute( _, _)).WillOnce(Return(SAI_STATUS_SUCCESS));

        orchd->logRotate();
    }

    TEST_F(OrchDaemonTest, ringBuffer)
    {
        int test_ring_size = 2;

        auto ring = new RingBuffer(test_ring_size);

        for (int i = 0; i < test_ring_size - 1; i++)
        {
            EXPECT_TRUE(ring->push([](){}));
        }
        EXPECT_FALSE(ring->push([](){}));

        AnyTask task;
        for (int i = 0; i < test_ring_size - 1; i++)
        {
            EXPECT_TRUE(ring->pop(task));
        }

        EXPECT_FALSE(ring->pop(task));

        ring->setIdle(true);
        EXPECT_TRUE(ring->IsIdle());
        delete ring;
    }

    TEST_F(OrchDaemonTest, RingThread)
    {
        orchd->enableRingBuffer();

        // verify ring buffer is created
        EXPECT_TRUE(Executor::gRingBuffer != nullptr);
        EXPECT_TRUE(Executor::gRingBuffer == Orch::gRingBuffer);

        orchd->ring_thread = std::thread(&OrchDaemon::popRingBuffer, orchd);
        auto gRingBuffer = orchd->gRingBuffer;

        // verify ring_thread is created
        while (!gRingBuffer->thread_created)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        bool task_executed = false;
        AnyTask task = [&task_executed]() { task_executed = true;};
        gRingBuffer->push(task);

        // verify ring thread is conditional locked
        EXPECT_TRUE(gRingBuffer->IsIdle());
        EXPECT_FALSE(task_executed);

        gRingBuffer->notify();

        // verify notify() would activate the ring thread when buffer is not empty
        while (!gRingBuffer->IsEmpty() || !gRingBuffer->IsIdle())
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        EXPECT_TRUE(task_executed);

        delete orchd;

        // verify the destructor of orchdaemon will stop the ring thread
        EXPECT_FALSE(orchd->ring_thread.joinable());
        // verify the destructor of orchdaemon also resets ring buffer
        EXPECT_TRUE(Executor::gRingBuffer == nullptr);

        // reset the orchd for other testcases
        orchd = new OrchDaemon(&appl_db, &config_db, &state_db, &counters_db, nullptr);
    }

    TEST_F(OrchDaemonTest, RingThreadTeardownSafeWhenRingDisabled)
    {
        // Reproduces the scenario fixed alongside PR #4400's graceful
        // shutdown path: OrchDaemon::start() always launches ring_thread,
        // but popRingBuffer() returns immediately when gRingBuffer is null
        // (ring mode disabled). The destructor must not dereference the
        // null gRingBuffer while tearing down a joinable ring_thread.

        // Ring mode intentionally left disabled: do NOT call enableRingBuffer.
        EXPECT_EQ(orchd->gRingBuffer, nullptr);

        // Mimic OrchDaemon::start() unconditionally launching the ring thread.
        orchd->ring_thread = std::thread(&OrchDaemon::popRingBuffer, orchd);

        // popRingBuffer() returns immediately when gRingBuffer is null, but
        // ring_thread stays joinable until the destructor joins it.
        while (!orchd->ring_thread.joinable())
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        EXPECT_TRUE(orchd->ring_thread.joinable());

        // Destructor must be safe in this state (previously null-deref'd).
        delete orchd;

        // Restore fixture invariants for the remaining test cases.
        orchd = new OrchDaemon(&appl_db, &config_db, &state_db, &counters_db, nullptr);
    }

    TEST_F(OrchDaemonTest, PushRingBuffer)
    {
        orchd->enableRingBuffer();

        auto gRingBuffer = orchd->gRingBuffer;

        std::vector<std::string> tables = {"ROUTE_TABLE", "OTHER_TABLE"};
        auto orch = make_shared<Orch>(&appl_db, tables);
        auto route_consumer = dynamic_cast<Consumer *>(orch->getExecutor("ROUTE_TABLE"));
        auto other_consumer = dynamic_cast<Consumer *>(orch->getExecutor("OTHER_TABLE"));

        EXPECT_TRUE(gRingBuffer->serves("ROUTE_TABLE"));
        EXPECT_FALSE(gRingBuffer->serves("OTHER_TABLE"));

        int x = 0;
        route_consumer->processAnyTask([&](){x=3;});
        // verify `processAnyTask` is equivalent to executing the task immediately
        EXPECT_TRUE(gRingBuffer->IsEmpty() && gRingBuffer->IsIdle() && !gRingBuffer->thread_created && x==3);

        gRingBuffer->thread_created = true; // set the flag to assume the ring thread is created (actually not)

        // verify `processAnyTask` is equivalent to executing the task immediately when ring is empty and idle
        other_consumer->processAnyTask([&](){x=4;});
        EXPECT_TRUE(gRingBuffer->IsEmpty() && gRingBuffer->IsIdle() && x==4);

        route_consumer->processAnyTask([&](){x=5;});
        // verify `processAnyTask` would not execute the task if thread_created is true
        // it only pushes the task to the ring buffer, without executing it
        EXPECT_TRUE(!gRingBuffer->IsEmpty() && x==4);

        AnyTask task;
        gRingBuffer->pop(task);
        task();
        // hence the task needs to be popped and explicitly executed
        EXPECT_TRUE(gRingBuffer->IsEmpty() && x==5);

        orchd->disableRingBuffer();
    }

    TEST_F(OrchDaemonTest, TestRedisFlushFailure)
    {

        ASSERT_DEATH(
            {
                InSequence s;

                EXPECT_CALL(mock_sai_switch_, set_switch_attribute(_, _))
                .WillOnce(Return(SAI_STATUS_FAILURE));
                EXPECT_CALL(mock_sai_switch_, set_switch_attribute(_, _));

                orchd->flush();
            },
            ".*"
        );
    }

    TEST_F(OrchDaemonTest, TestFlushWithRingBufferEntry)
    {
        // Allow one or more calls to set_switch_attribute
        EXPECT_CALL(mock_sai_switch_, set_switch_attribute(testing::_, testing::_))
            .WillRepeatedly(Return(SAI_STATUS_SUCCESS));

        orchd->enableRingBuffer();

        auto gRingBuffer = orchd->gRingBuffer;

        std::vector<std::string> tables = {"ROUTE_TABLE", "OTHER_TABLE"};
        auto orch = make_shared<Orch>(&appl_db, tables);
        auto route_consumer = dynamic_cast<Consumer *>(orch->getExecutor("ROUTE_TABLE"));

        EXPECT_TRUE(gRingBuffer->serves("ROUTE_TABLE"));

        int x = 0;

        gRingBuffer->thread_created = true; // set the flag to assume the ring thread is created (actually not)
        route_consumer->processAnyTask([&](){x=5;});

       // Ring is not empty, flush would not be triggered
        orchd->flush();
        EXPECT_TRUE(!gRingBuffer->IsEmpty() && x==0);
        AnyTask task;
        gRingBuffer->pop(task);
        task();
        // hence the task needs to be popped and explicitly executed
        EXPECT_TRUE(gRingBuffer->IsEmpty() && x==5);
       // Ring is empty, flush would be triggered
        orchd->flush();

        orchd->disableRingBuffer();
    }

    // Two distinct Orch subclasses that intentionally share the same consumer
    // table name. This mirrors the real situation where CFG_FLEX_COUNTER_TABLE
    // is registered as a consumer in both WatermarkOrch and FlexCounterOrch.
    // Their demangled class names must be present in the STATE_DB key so the
    // two rows do not collide.
    class QueueDepthTestOrchA : public Orch
    {
      public:
        QueueDepthTestOrchA(DBConnector *db, const std::vector<std::string> &tables)
            : Orch(db, tables) {}
        void doTask(Consumer &) override {}
    };

    class QueueDepthTestOrchB : public Orch
    {
      public:
        QueueDepthTestOrchB(DBConnector *db, const std::vector<std::string> &tables)
            : Orch(db, tables) {}
        void doTask(Consumer &) override {}
    };

    static void seedPendingTask(Orch *o, const std::string &table, const std::string &key)
    {
        auto *c = dynamic_cast<ConsumerBase *>(o->getExecutor(table));
        ASSERT_NE(c, nullptr);
        swss::KeyOpFieldsValuesTuple kfvt{key, SET_COMMAND, {{"f", "v"}}};
        c->addToSync(kfvt);
    }

    TEST_F(OrchDaemonTest, PublishQueueDepth)
    {
        // Sanity: the publisher table was created in the daemon constructor.
        ASSERT_NE(orchd->m_queueDepthTable, nullptr);

        // Build two Orchs whose consumers intentionally share a table name.
        const std::string kSharedTable = "QUEUE_DEPTH_UT_DUP_TABLE";
        const std::string kOrchAOnly   = "QUEUE_DEPTH_UT_A_ONLY";
        const std::string kOrchBOnly   = "QUEUE_DEPTH_UT_B_ONLY";
        auto orchA = std::make_shared<QueueDepthTestOrchA>(
            &appl_db, std::vector<std::string>{kSharedTable, kOrchAOnly});
        auto orchB = std::make_shared<QueueDepthTestOrchB>(
            &appl_db, std::vector<std::string>{kSharedTable, kOrchBOnly});

        // Pre-seed a stale row to verify it gets removed once the orch is no
        // longer in m_orchList. (Simulates a row left over from a previous
        // orchagent run that registered a now-removed consumer.)
        const std::string kStaleKey = "GhostOrch|GHOST_TABLE";
        orchd->m_queueDepthTable->set(
            kStaleKey, {{"pending_count", "42"}, {"orch", "GhostOrch"}, {"consumer", "GHOST_TABLE"}});
        orchd->m_lastPublishedQueueKeys.insert(kStaleKey);

        // Seed different pending counts so we can tell the two rows apart.
        seedPendingTask(orchA.get(), kSharedTable, "k1");
        seedPendingTask(orchA.get(), kSharedTable, "k2");  // OrchA: 2 pending
        seedPendingTask(orchA.get(), kOrchAOnly,   "ka");  // OrchA-only: 1
        seedPendingTask(orchB.get(), kSharedTable, "k3");  // OrchB: 1 pending
        // OrchB's kOrchBOnly consumer stays empty; should still publish 0.

        orchd->m_orchList.push_back(orchA.get());
        orchd->m_orchList.push_back(orchB.get());

        orchd->publishQueueDepth();

        const std::string kKeyAShared =
            std::string("orchdaemon_test::QueueDepthTestOrchA|") + kSharedTable;
        const std::string kKeyBShared =
            std::string("orchdaemon_test::QueueDepthTestOrchB|") + kSharedTable;
        const std::string kKeyAOnly =
            std::string("orchdaemon_test::QueueDepthTestOrchA|") + kOrchAOnly;
        const std::string kKeyBOnly =
            std::string("orchdaemon_test::QueueDepthTestOrchB|") + kOrchBOnly;

        // Both Orchs' rows for the shared table must coexist — the per-Orch
        // class name in the key prevents the collision Copilot called out.
        std::string val;
        ASSERT_TRUE(orchd->m_queueDepthTable->hget(kKeyAShared, "pending_count", val));
        EXPECT_EQ(val, "2");
        ASSERT_TRUE(orchd->m_queueDepthTable->hget(kKeyBShared, "pending_count", val));
        EXPECT_EQ(val, "1");
        ASSERT_TRUE(orchd->m_queueDepthTable->hget(kKeyAOnly, "pending_count", val));
        EXPECT_EQ(val, "1");
        ASSERT_TRUE(orchd->m_queueDepthTable->hget(kKeyBOnly, "pending_count", val));
        EXPECT_EQ(val, "0");
        ASSERT_TRUE(orchd->m_queueDepthTable->hget(kKeyAShared, "orch", val));
        EXPECT_EQ(val, "orchdaemon_test::QueueDepthTestOrchA");
        ASSERT_TRUE(orchd->m_queueDepthTable->hget(kKeyAShared, "consumer", val));
        EXPECT_EQ(val, kSharedTable);

        // Stale key from prior cycle (not in current snapshot) must be gone.
        EXPECT_FALSE(orchd->m_queueDepthTable->hget(kStaleKey, "pending_count", val));

        // Now drop OrchB from m_orchList and re-publish. OrchB's rows should
        // be DELed; OrchA's rows should remain.
        orchd->m_orchList.pop_back();
        orchd->publishQueueDepth();

        EXPECT_TRUE(orchd->m_queueDepthTable->hget(kKeyAShared, "pending_count", val));
        EXPECT_FALSE(orchd->m_queueDepthTable->hget(kKeyBShared, "pending_count", val));
        EXPECT_FALSE(orchd->m_queueDepthTable->hget(kKeyBOnly, "pending_count", val));

        // Cleanup so subsequent tests start from an empty publisher table.
        orchd->m_orchList.clear();
        orchd->publishQueueDepth();
    }

    TEST_F(OrchDaemonTest, PublishQueueDepthEmptyOrchListIsNoOp)
    {
        ASSERT_NE(orchd->m_queueDepthTable, nullptr);
        ASSERT_TRUE(orchd->m_orchList.empty());

        // Nothing should crash and nothing should be written.
        orchd->publishQueueDepth();
        std::vector<std::string> keys;
        orchd->m_queueDepthTable->getKeys(keys);
        EXPECT_TRUE(keys.empty());
    }

}
