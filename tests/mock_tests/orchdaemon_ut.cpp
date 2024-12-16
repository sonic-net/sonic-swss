#include "orchdaemon.h"
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

            std::shared_ptr<RingBuffer> gRingBuffer;

            std::shared_ptr<Consumer> consumer;

            std::shared_ptr<Orch> orch;
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

            void SetUp() override {
                gRingBuffer = std::make_shared<RingBuffer>();
            }

            void TearDown() override
            {
                gRingBuffer = nullptr;
            }
    };

    TEST_F(OrchDaemonTest, logRotate)
    {
        EXPECT_CALL(mock_sai_switch_, set_switch_attribute( _, _)).WillOnce(Return(SAI_STATUS_SUCCESS));

        orchd->logRotate();
    }

    TEST_F(OrchDaemonTest, RingThread)
    {
        orchd->enableRingBuffer();

        // verify ring buffer is created  
        EXPECT_TRUE(Executor::gRingBuffer != nullptr);
        EXPECT_TRUE(Executor::gRingBuffer == Orch::gRingBuffer);

        orchd->ring_thread = std::thread(&OrchDaemon::popRingBuffer, orchd);
        gRingBuffer = orchd->gRingBuffer;

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

        gRingBuffer = orchd->gRingBuffer;

        orch = make_shared<Orch>(&appl_db, "ROUTE_TABLE", 0);
        consumer = make_shared<Consumer>(new swss::ConsumerStateTable(&appl_db, "ROUTE_TABLE", 128, 1), orch.get(), "ROUTE_TABLE");

        EXPECT_TRUE(gRingBuffer->serves("ROUTE_TABLE"));
        EXPECT_FALSE(gRingBuffer->serves("OTHER_TABLE"));

        int x;
        consumer->pushRingBuffer([&](){x=3;});
        // verify `pushRingBuffer` is equivalent to executing the task immediately
        EXPECT_TRUE(x==3);

        gRingBuffer->thread_created = true;
        consumer->pushRingBuffer([&](){x=4;});
        // verify `pushRingBuffer` would not execute the task if thread_created is true
        // it only pushes the task to the ring buffer
        EXPECT_TRUE(x==3);
        AnyTask task;
        gRingBuffer->pop(task);
        task();
        // hence the task needs to be popped and explicitly executed
        EXPECT_TRUE(x==4);

        orchd->disableRingBuffer();
    }

}
