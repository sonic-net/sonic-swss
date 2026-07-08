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

    // Helper: push one SET tuple into the consumer's m_toSync so
    // ConsumerBase::hasPendingTasks() returns true. Mirrors what the real
    // ProducerStateTable hop does when an upstream daemon writes APPL_DB.
    static void hasPendingSeedTask(Orch *o, const std::string &table, const std::string &key)
    {
        auto *c = dynamic_cast<ConsumerBase *>(o->getExecutor(table));
        ASSERT_NE(c, nullptr);
        swss::KeyOpFieldsValuesTuple kfvt{key, SET_COMMAND, {{"f", "v"}}};
        c->addToSync(kfvt);
    }

    TEST_F(OrchDaemonTest, ConsumerBaseHasPendingTasks)
    {
        // ConsumerBase::hasPendingTasks is an inline !m_toSync.empty() check.
        // Verify both the empty and non-empty states.
        std::vector<std::string> tables{"HPT_TABLE_A"};
        auto orch = std::make_shared<Orch>(&appl_db, tables);

        auto *c = dynamic_cast<ConsumerBase *>(orch->getExecutor("HPT_TABLE_A"));
        ASSERT_NE(c, nullptr);

        // Fresh consumer: m_toSync is empty -> hasPendingTasks() == false.
        EXPECT_FALSE(c->hasPendingTasks());

        // After addToSync, m_toSync grows -> hasPendingTasks() == true.
        swss::KeyOpFieldsValuesTuple kfvt{"key1", SET_COMMAND, {{"f", "v"}}};
        c->addToSync(kfvt);
        EXPECT_TRUE(c->hasPendingTasks());

        // Drain m_toSync; the consumer is empty again.
        c->m_toSync.clear();
        EXPECT_FALSE(c->hasPendingTasks());
    }

    TEST_F(OrchDaemonTest, OrchHasPendingTasks)
    {
        // Orch::hasPendingTasks() walks m_consumerMap and returns true iff
        // any ConsumerBase in the Orch has a non-empty m_toSync.
        std::vector<std::string> tables{"HPT_TABLE_A", "HPT_TABLE_B"};
        auto orch = std::make_shared<Orch>(&appl_db, tables);

        // All consumers fresh -> no pending tasks.
        EXPECT_FALSE(orch->hasPendingTasks());

        // Seed one consumer; the Orch now reports pending.
        hasPendingSeedTask(orch.get(), "HPT_TABLE_A", "k1");
        EXPECT_TRUE(orch->hasPendingTasks());

        // Seed the other consumer; still true.
        hasPendingSeedTask(orch.get(), "HPT_TABLE_B", "k1");
        EXPECT_TRUE(orch->hasPendingTasks());

        // Drain only TABLE_A; TABLE_B still has work -> still true.
        auto *cA = dynamic_cast<ConsumerBase *>(orch->getExecutor("HPT_TABLE_A"));
        ASSERT_NE(cA, nullptr);
        cA->m_toSync.clear();
        EXPECT_TRUE(orch->hasPendingTasks());

        // Drain TABLE_B too -> false.
        auto *cB = dynamic_cast<ConsumerBase *>(orch->getExecutor("HPT_TABLE_B"));
        ASSERT_NE(cB, nullptr);
        cB->m_toSync.clear();
        EXPECT_FALSE(orch->hasPendingTasks());
    }

    TEST_F(OrchDaemonTest, OrchDaemonHasPendingTasks)
    {
        // OrchDaemon::hasPendingTasks() iterates m_orchList. It gates the new
        // SELECT_TIMEOUT retry sweep added in this PR: when there is no ring
        // buffer and at least one orch has pending tasks, doTask() runs on
        // every orch in m_orchList. Idle systems with empty queues stay cheap.

        // Empty m_orchList -> no pending tasks.
        ASSERT_TRUE(orchd->m_orchList.empty());
        EXPECT_FALSE(orchd->hasPendingTasks());

        std::vector<std::string> tablesA{"HPT_DAEMON_A"};
        std::vector<std::string> tablesB{"HPT_DAEMON_B"};
        auto orchA = std::make_shared<Orch>(&appl_db, tablesA);
        auto orchB = std::make_shared<Orch>(&appl_db, tablesB);

        orchd->m_orchList.push_back(orchA.get());
        orchd->m_orchList.push_back(orchB.get());

        // Both Orchs fresh, no pending tasks -> daemon reports false.
        EXPECT_FALSE(orchd->hasPendingTasks());

        // Pending in the second Orch only — daemon must still report true,
        // i.e. the iteration must not short-circuit at the first empty Orch.
        hasPendingSeedTask(orchB.get(), "HPT_DAEMON_B", "k1");
        EXPECT_TRUE(orchd->hasPendingTasks());

        // Pending in the first Orch as well -> still true.
        hasPendingSeedTask(orchA.get(), "HPT_DAEMON_A", "k1");
        EXPECT_TRUE(orchd->hasPendingTasks());

        // Drain orchB only -> orchA still has work -> still true.
        auto *cB = dynamic_cast<ConsumerBase *>(orchB->getExecutor("HPT_DAEMON_B"));
        ASSERT_NE(cB, nullptr);
        cB->m_toSync.clear();
        EXPECT_TRUE(orchd->hasPendingTasks());

        // Drain orchA -> all queues empty -> false again.
        auto *cA = dynamic_cast<ConsumerBase *>(orchA->getExecutor("HPT_DAEMON_A"));
        ASSERT_NE(cA, nullptr);
        cA->m_toSync.clear();
        EXPECT_FALSE(orchd->hasPendingTasks());

        // Cleanup so subsequent fixture-shared tests start from a clean list.
        orchd->m_orchList.clear();
    }

}
