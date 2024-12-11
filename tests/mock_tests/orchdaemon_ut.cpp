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

            RingBuffer* gRingBuffer = RingBuffer::get();

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
                gRingBuffer = RingBuffer::reset();
            }

            void TearDown() override
            {
                RingBuffer::release();
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

        EXPECT_TRUE(Executor::gRingBuffer != nullptr);
        EXPECT_TRUE(Executor::gRingBuffer == Orch::gRingBuffer);

        orchd->ring_thread = std::thread(&OrchDaemon::popRingBuffer, orchd);

        while (!RingBuffer::get()->thread_created)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        bool task_executed = false;
        AnyTask task = [&task_executed]() { task_executed = true;};
        RingBuffer::get()->push(task);

        EXPECT_TRUE(RingBuffer::get()->IsIdle());

        RingBuffer::get()->notify();

        while (!RingBuffer::get()->IsEmpty() || !RingBuffer::get()->IsIdle())
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        EXPECT_TRUE(task_executed);

        EXPECT_TRUE(orchd->ring_thread.joinable());

        delete orchd;

        EXPECT_FALSE(orchd->ring_thread.joinable());

        // reset the orchd
        orchd = new OrchDaemon(&appl_db, &config_db, &state_db, &counters_db, nullptr);
    }

    TEST_F(OrchDaemonTest, PushAnyTask)
    {
        orchd->enableRingBuffer();

        orch = make_shared<Orch>(&appl_db, "ROUTE_TABLE", 0);
        consumer = make_shared<Consumer>(new swss::ConsumerStateTable(&appl_db, "ROUTE_TABLE", 128, 1), orch.get(), "ROUTE_TABLE");

        EXPECT_TRUE(gRingBuffer->serves("ROUTE_TABLE"));
        EXPECT_FALSE(gRingBuffer->serves("OTHER_TABLE"));
        EXPECT_TRUE(gRingBuffer->IsEmpty());

        int x = 1;
        int y = 3;
        AnyTask t1 = [&](){x=2;};
        AnyTask t2 = [](){};
        AnyTask t3 = [&](){x=3;y=2;};

        gRingBuffer->push(t1);
        gRingBuffer->push(t2);
        EXPECT_FALSE(gRingBuffer->IsEmpty());

        gRingBuffer->pop(t3);
        t3();
        EXPECT_TRUE(x==2);
        EXPECT_TRUE(y==3);

        EXPECT_TRUE(gRingBuffer->pop(t3));
        EXPECT_FALSE(gRingBuffer->pop(t3));

        consumer->pushRingBuffer([&](){x=3;});
        EXPECT_TRUE(x==3);

        gRingBuffer->thread_created = true;
        consumer->pushRingBuffer([&](){x=4;});
        EXPECT_TRUE(x==3);

        gRingBuffer->pop(t3);
        t3();
        EXPECT_TRUE(x==4);

        orchd->disableRingBuffer();
    }

    TEST_F(OrchDaemonTest, ThreadPauseAndNotify) {

        bool thread_finished = false;
        std::thread t([this, &thread_finished]() {
            gRingBuffer->setIdle(true);
            gRingBuffer->pauseThread();
            thread_finished = true;
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        AnyTask task = []() { };
        EXPECT_TRUE(gRingBuffer->push(task));
        gRingBuffer->notify();

        t.join();
        EXPECT_TRUE(thread_finished);
    }

    TEST_F(OrchDaemonTest, MultiThread) {
        std::vector<std::thread> producers;
        std::vector<std::thread> consumers;

        for (int i = 0; i < 3; i++) {
            producers.emplace_back([this]() {
                AnyTask task = []() { };
                for (int j = 0; j < 10; j++) {
                    gRingBuffer->push(task);
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
            });
        }

        for (int i = 0; i < 3; i++) {
            consumers.emplace_back([this]() {
                for (int j = 0; j < 10; j++) {
                    AnyTask task;
                    while (!gRingBuffer->pop(task)) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    }
                }
            });
        }

        for (auto& t : producers) {
            t.join();
        }
        for (auto& t : consumers) {
            t.join();
        }
    }

}
