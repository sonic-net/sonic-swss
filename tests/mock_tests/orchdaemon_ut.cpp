#define protected public
#include "orch.h"
#include "orchdaemon.h"
#undef protected
#include "dbconnector.h"
#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include "mock_sai_switch.h"

extern sai_switch_api_t* sai_switch_api;
sai_switch_api_t test_sai_switch;

namespace orchdaemon_test
{

    using ::testing::_;
    using ::testing::Return;
    using ::testing::StrictMock;

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

}
