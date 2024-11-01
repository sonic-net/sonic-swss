#include "orch.h"
#include "dbconnector.h"
#include <gtest/gtest.h>
#include <gmock/gmock.h>

// let the compiler to generate codes for the instantiated template
static constexpr int TEST_ORCH_RING_SIZE = 60;
template class RingBuffer<AnyTask, TEST_ORCH_RING_SIZE>;
using TestRing = RingBuffer<AnyTask, TEST_ORCH_RING_SIZE>;

namespace ring_test
{
    using namespace std;

    swss::DBConnector appl_db("APPL_DB", 0);

    class RingTest : public ::testing::Test
    {
        public:

            TestRing* ring = TestRing::Get();
            OrchRing* gRingBuffer = OrchRing::Get();
            std::unique_ptr<Consumer> consumer;
            std::unique_ptr<Orch> orch;

            void SetUp() override {
                Orch::gRingBuffer = gRingBuffer;
                Executor::gRingBuffer = gRingBuffer;
                // clear the rings
                while (!gRingBuffer->IsEmpty()) {
                    AnyTask task;
                    gRingBuffer->pop(task);
                }
                while (!ring->IsEmpty()) {
                    AnyTask task;
                    ring->pop(task);
                }
            }

            void TearDown() override {
                Orch::gRingBuffer = nullptr;
                Executor::gRingBuffer = nullptr;
            }
    };

    TEST_F(RingTest, basics)
    {
        AnyTask task = []() { };
        EXPECT_TRUE(ring->push(task));

        AnyTask poppedTask;
        EXPECT_TRUE(ring->pop(poppedTask));
        EXPECT_TRUE(ring->IsEmpty());
    }

    TEST_F(RingTest, bufferFull) {
        AnyTask task = []() { };

        // Fill the buffer
        for (int i = 0; i < TEST_ORCH_RING_SIZE - 1; i++) {
            EXPECT_TRUE(ring->push(task));
        }

        // Fail to push when full
        EXPECT_FALSE(ring->push(task));
    }

    TEST_F(RingTest, bufferEmpty) {
        AnyTask task;
        EXPECT_TRUE(ring->IsEmpty());
        EXPECT_FALSE(ring->pop(task));
    }

    TEST_F(RingTest, bufferOverflow) {
        AnyTask task = []() { };

        int halfSize = TEST_ORCH_RING_SIZE / 2;

        for (int i = 0; i < halfSize; i++) {
            EXPECT_TRUE(ring->push(task));
        }
        for (int i = 0; i < halfSize; i++) {
            AnyTask poppedTask;
            EXPECT_TRUE(ring->pop(poppedTask));
        }

        for (int i = 0; i < halfSize; i++) {
            EXPECT_TRUE(ring->push(task));
        }

        for (int i = 0; i < halfSize; i++) {
            AnyTask poppedTask;
            EXPECT_TRUE(ring->pop(poppedTask));
        }
    }

    TEST_F(RingTest, ringBasics)
    {
        orch = make_unique<Orch>(&appl_db, "ROUTE_TABLE", 0);
        consumer = make_unique<Consumer>(new swss::ConsumerStateTable(&appl_db, "ROUTE_TABLE", 128, 1), orch.get(), "ROUTE_TABLE");

        EXPECT_TRUE(gRingBuffer->Serves("ROUTE_TABLE"));
        EXPECT_FALSE(gRingBuffer->Serves("OTHER_TABLE"));
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

        gRingBuffer->threadCreated = true;
        consumer->pushRingBuffer([&](){x=4;});
        EXPECT_TRUE(x==3);

        gRingBuffer->pop(t3);
        t3();
        EXPECT_TRUE(x==4);
    }

    TEST_F(RingTest, threadPauseAndNotify) {
        bool threadFinished = false;

        std::thread t([this, &threadFinished]() {
            ring->Idle = true;
            ring->pause_thread();
            threadFinished = true;
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        AnyTask task = []() { };
        EXPECT_TRUE(ring->push(task));
        ring->notify();

        t.join();
        EXPECT_TRUE(threadFinished);
    }

    TEST_F(RingTest, multiThread) {
        std::vector<std::thread> producers;
        std::vector<std::thread> consumers;

        for (int i = 0; i < 3; i++) {
            producers.emplace_back([this]() {
                AnyTask task = []() { };
                for (int j = 0; j < 10; j++) {
                    ring->push(task);
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
            });
        }

        for (int i = 0; i < 3; i++) {
            consumers.emplace_back([this]() {
                for (int j = 0; j < 10; j++) {
                    AnyTask task;
                    while (!ring->pop(task)) {
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

    TEST_F(RingTest, notify) {
        ring->Idle = true;
        EXPECT_NO_THROW(ring->notify());
        AnyTask task = []() { };
        ring->push(task);
        EXPECT_NO_THROW(ring->notify());
    }

    TEST_F(RingTest, edgeCases) {
        AnyTask task = []() { };
        
        for (int cycle = 0; cycle < 3; cycle++) {
            for (int i = 0; i < TEST_ORCH_RING_SIZE - 1; i++) {
                EXPECT_TRUE(ring->push(task));
            }
            EXPECT_TRUE(ring->IsFull());
            for (int i = 0; i < TEST_ORCH_RING_SIZE - 1; i++) {
                AnyTask poppedTask;
                EXPECT_TRUE(ring->pop(poppedTask));
            }
            EXPECT_TRUE(ring->IsEmpty());
        }
    }

    TEST_F(RingTest, invalidSize) {
        auto badRing = RingBuffer<AnyTask, 1>::Get();
        AnyTask task = []() { };

        EXPECT_TRUE(badRing->IsEmpty());
        EXPECT_TRUE(badRing->IsFull());

        EXPECT_FALSE(badRing->push(task));
        AnyTask poppedTask;
        EXPECT_FALSE(badRing->pop(poppedTask));
    }
}
