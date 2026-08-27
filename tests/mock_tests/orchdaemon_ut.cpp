#define protected public
#include "orch.h"
#include "orchdaemon.h"
#undef protected
#include "dbconnector.h"
#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include <random>
#include <thread>
#include "mock_sai_switch.h"
#include "saihelper.h"
#include "json.h"

#include <csignal>

extern sai_switch_api_t* sai_switch_api;
sai_switch_api_t test_sai_switch;

extern volatile sig_atomic_t gOrchShutdownRequested;

// Global mock-hiredis reply used to feed pub/sub notification messages into a
// NotificationConsumer (see tests/mock_tests/mock_hiredis.cpp).
extern redisReply *mockReply;

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

    TEST(ExecutorStatTest, RecordAndReset)
    {
        ExecutorStat s;

        // Empty state.
        EXPECT_EQ(s.count, 0u);
        EXPECT_EQ(s.total_ns, 0u);
        EXPECT_EQ(s.max_ns, 0u);

        // First sample sets count/total/max.
        s.record(50);
        EXPECT_EQ(s.count, 1u);
        EXPECT_EQ(s.total_ns, 50u);
        EXPECT_EQ(s.max_ns, 50u);

        // Subsequent samples track the running max.
        s.record(10);
        s.record(200);
        s.record(75);
        EXPECT_EQ(s.count, 4u);
        EXPECT_EQ(s.total_ns, 50u + 10u + 200u + 75u);
        EXPECT_EQ(s.max_ns, 200u);

        // Reset returns to empty state.
        s.reset();
        EXPECT_EQ(s.count, 0u);
        EXPECT_EQ(s.total_ns, 0u);
        EXPECT_EQ(s.max_ns, 0u);

        // Recording after reset behaves like a fresh slot.
        s.record(7);
        EXPECT_EQ(s.count, 1u);
        EXPECT_EQ(s.max_ns, 7u);
    }

    TEST(ExecutorStatTest, FieldValueTupleRoundTrip)
    {
        // Serialize → deserialize using the same 14-field pipe-separated
        // format OrchDaemon::handleTaskStatsQuery() / show orchagent
        // tasks use:
        //   count | total_run_ns
        //   | median_run_ns | q1_run_ns | q3_run_ns | max_run_ns
        //   | high_outliers | low_outliers
        //   | sched_count | total_sched_ns
        //   | median_sched_ns | q1_sched_ns | q3_sched_ns | max_sched_ns
        std::unordered_map<std::string, ExecutorStat> stats;

        auto &port = stats["PortsOrch"];
        // Seven run samples so median P² is bootstrapped (>=5).
        for (uint64_t v : {100ull, 110ull, 105ull, 95ull, 100ull, 102ull, 98ull})
            port.record_run(v);
        // Six sched samples so the sched P² is also bootstrapped.
        for (uint64_t v : {50ull, 60ull, 55ull, 45ull, 52ull, 58ull})
            port.record_sched(v);

        auto &flush = stats["flush"];
        flush.record_run(1000);

        // Empty slot to confirm count==0 path: sentinels must come out
        // as 0 on the wire (otherwise the CLI would print UINT64_MAX).
        stats["NeverRan"];

        auto round_p2 = [](double v) -> uint64_t
        {
            return static_cast<uint64_t>(v < 0.0 ? 0.0 : v + 0.5);
        };

        std::vector<swss::FieldValueTuple> fvs;
        for (auto &kv : stats)
        {
            auto &s = kv.second;
            uint64_t med_run = (s.count == 0) ? 0 : round_p2(s.median.value());
            uint64_t q1_run  = (s.count == 0) ? 0 : round_p2(s.q1.value());
            uint64_t q3_run  = (s.count == 0) ? 0 : round_p2(s.q3.value());
            uint64_t med_sc  = (s.sched_count == 0) ? 0 : round_p2(s.sched_median.value());
            uint64_t q1_sc   = (s.sched_count == 0) ? 0 : round_p2(s.sched_q1.value());
            uint64_t q3_sc   = (s.sched_count == 0) ? 0 : round_p2(s.sched_q3.value());

            std::string v = std::to_string(s.count) + "|"
                          + std::to_string(s.total_ns) + "|"
                          + std::to_string(med_run) + "|"
                          + std::to_string(q1_run) + "|"
                          + std::to_string(q3_run) + "|"
                          + std::to_string(s.max_ns) + "|"
                          + std::to_string(s.high_outliers) + "|"
                          + std::to_string(s.low_outliers) + "|"
                          + std::to_string(s.sched_count) + "|"
                          + std::to_string(s.total_sched_ns) + "|"
                          + std::to_string(med_sc) + "|"
                          + std::to_string(q1_sc) + "|"
                          + std::to_string(q3_sc) + "|"
                          + std::to_string(s.sched_max_ns);
            fvs.emplace_back(kv.first, v);
        }

        struct Parsed {
            uint64_t count, total_ns;
            uint64_t median_run_ns, q1_run_ns, q3_run_ns, max_run_ns;
            uint64_t high_outliers, low_outliers;
            uint64_t sched_count, total_sched_ns;
            uint64_t median_sched_ns, q1_sched_ns, q3_sched_ns, max_sched_ns;
        };
        std::unordered_map<std::string, Parsed> parsed;
        for (const auto &fv : fvs)
        {
            const std::string &k = fvField(fv);
            const std::string &v = fvValue(fv);

            std::vector<std::string> parts;
            size_t start = 0;
            while (start <= v.size())
            {
                size_t end = v.find('|', start);
                if (end == std::string::npos) end = v.size();
                parts.emplace_back(v.substr(start, end - start));
                start = end + 1;
            }
            ASSERT_EQ(parts.size(), 14u);

            Parsed p;
            p.count           = std::stoull(parts[0]);
            p.total_ns        = std::stoull(parts[1]);
            p.median_run_ns   = std::stoull(parts[2]);
            p.q1_run_ns       = std::stoull(parts[3]);
            p.q3_run_ns       = std::stoull(parts[4]);
            p.max_run_ns      = std::stoull(parts[5]);
            p.high_outliers   = std::stoull(parts[6]);
            p.low_outliers    = std::stoull(parts[7]);
            p.sched_count     = std::stoull(parts[8]);
            p.total_sched_ns  = std::stoull(parts[9]);
            p.median_sched_ns = std::stoull(parts[10]);
            p.q1_sched_ns     = std::stoull(parts[11]);
            p.q3_sched_ns     = std::stoull(parts[12]);
            p.max_sched_ns    = std::stoull(parts[13]);
            parsed[k] = p;
        }

        ASSERT_EQ(parsed.size(), stats.size());

        EXPECT_EQ(parsed["PortsOrch"].count, 7u);
        EXPECT_EQ(parsed["PortsOrch"].total_ns, 100u + 110u + 105u + 95u + 100u + 102u + 98u);
        EXPECT_EQ(parsed["PortsOrch"].max_run_ns, 110u);
        // Median of {95, 98, 100, 100, 102, 105, 110} is 100. With only
        // 5–7 samples P² is rough; allow a generous band.
        EXPECT_GE(parsed["PortsOrch"].median_run_ns, 90u);
        EXPECT_LE(parsed["PortsOrch"].median_run_ns, 110u);
        // <30 samples (OUTLIER_WARMUP) -> classifier disabled.
        EXPECT_EQ(parsed["PortsOrch"].high_outliers, 0u);
        EXPECT_EQ(parsed["PortsOrch"].low_outliers,  0u);

        EXPECT_EQ(parsed["PortsOrch"].sched_count, 6u);
        EXPECT_EQ(parsed["PortsOrch"].total_sched_ns,
                  50u + 60u + 55u + 45u + 52u + 58u);
        EXPECT_EQ(parsed["PortsOrch"].max_sched_ns, 60u);
        EXPECT_GE(parsed["PortsOrch"].median_sched_ns, 40u);
        EXPECT_LE(parsed["PortsOrch"].median_sched_ns, 65u);

        EXPECT_EQ(parsed["flush"].count, 1u);
        EXPECT_EQ(parsed["flush"].total_ns, 1000u);
        EXPECT_EQ(parsed["flush"].max_run_ns, 1000u);
        EXPECT_EQ(parsed["flush"].median_run_ns, 1000u);
        EXPECT_EQ(parsed["flush"].sched_count, 0u);
        EXPECT_EQ(parsed["flush"].total_sched_ns, 0u);
        EXPECT_EQ(parsed["flush"].median_sched_ns, 0u);

        EXPECT_EQ(parsed["NeverRan"].count, 0u);
        EXPECT_EQ(parsed["NeverRan"].total_ns, 0u);
        EXPECT_EQ(parsed["NeverRan"].median_run_ns, 0u);
        EXPECT_EQ(parsed["NeverRan"].max_run_ns, 0u);
        EXPECT_EQ(parsed["NeverRan"].high_outliers, 0u);
        EXPECT_EQ(parsed["NeverRan"].low_outliers,  0u);
        EXPECT_EQ(parsed["NeverRan"].sched_count, 0u);
        EXPECT_EQ(parsed["NeverRan"].total_sched_ns, 0u);
        EXPECT_EQ(parsed["NeverRan"].max_sched_ns, 0u);
    }

    TEST(TaskTimerTest, RecordsSchedLatencyAfterFirstRun)
    {
        // First TaskTimer instance: no prior return, so sched_count
        // stays 0. Second instance arrives some time later and records
        // a non-zero sched-latency sample.
        ExecutorStat s;

        {
            TaskTimer t(s);
            std::this_thread::sleep_for(std::chrono::microseconds(50));
        }
        EXPECT_EQ(s.count, 1u);
        EXPECT_EQ(s.sched_count, 0u);   // first run has no "previous"
        EXPECT_GT(s.last_return_ns, 0u);

        // Sleep so there's a measurable gap between dtor and ctor.
        std::this_thread::sleep_for(std::chrono::microseconds(200));

        {
            TaskTimer t(s);
            std::this_thread::sleep_for(std::chrono::microseconds(50));
        }
        EXPECT_EQ(s.count, 2u);
        EXPECT_EQ(s.sched_count, 1u);
        EXPECT_GT(s.sched_max_ns, 0u);
    }

    TEST(P2QuantileTest, MedianConvergesOnUniform)
    {
        // P² (Jain & Chlamtac) is approximate. Feed a deterministic,
        // pseudo-random uniform stream over [0, 9999] and check the
        // median estimate is within a sensible tolerance of the true
        // median (~4999.5).
        P2Quantile median(0.5);
        std::mt19937 gen(42);
        std::uniform_int_distribution<int> dist(0, 9999);
        for (int i = 0; i < 10000; ++i)
            median.add(static_cast<double>(dist(gen)));
        EXPECT_TRUE(median.ready());
        EXPECT_NEAR(median.value(), 5000.0, 200.0);
    }

    TEST(P2QuantileTest, MedianRobustToOutliers)
    {
        // 1000 samples uniformly in [95, 105] establish the median ~100.
        // Add 20 moderate outliers (3x the typical value) — far outside
        // any reasonable IQR but not pathological. Arithmetic mean would
        // shift to ~104; the P² median should stay close to 100.
        //
        // Note: P² with extreme outliers (e.g. 1e9 in a stream that
        // averages 100) is known to drift because parabolic
        // interpolation of q[4] propagates inward. The Tukey-rule
        // outlier counters handle that scenario; this test pins the
        // contract that for realistic tail tasks the median tracks the
        // body of the distribution, not the tail.
        P2Quantile median(0.5);
        std::mt19937 gen(42);
        std::uniform_int_distribution<int> dist(95, 105);
        for (int i = 0; i < 1000; ++i) median.add(static_cast<double>(dist(gen)));
        for (int i = 0; i < 20;   ++i) median.add(300.0);
        EXPECT_TRUE(median.ready());
        EXPECT_NEAR(median.value(), 100.0, 10.0);
    }

    TEST(ExecutorStatTest, OutlierClassification)
    {
        // 200 in-range samples around 100 establish Q1 ~95, Q3 ~105,
        // IQR ~10, so high cutoff is ~120 and low cutoff is ~80.
        ExecutorStat s;
        for (int i = 0; i < 200; ++i)
        {
            s.record(static_cast<uint64_t>(95 + (i % 11)));   // 95..105
        }
        const uint64_t high_before = s.high_outliers;
        const uint64_t low_before  = s.low_outliers;

        // Five clear high outliers (way past Q3 + 1.5*IQR).
        for (int i = 0; i < 5; ++i) s.record(10'000);
        // Four clear low outliers (well below Q1 - 1.5*IQR; needs to be
        // negative-ish, but ns is unsigned so use 0).
        for (int i = 0; i < 4; ++i) s.record(0);

        EXPECT_GE(s.high_outliers - high_before, 5u);
        EXPECT_GE(s.low_outliers  - low_before,  4u);
    }

    TEST(ExecutorStatTest, ResetClearsP2)
    {
        ExecutorStat s;
        for (int i = 0; i < 10; ++i) s.record(100);
        ASSERT_TRUE(s.median.ready());
        s.reset();
        EXPECT_FALSE(s.median.ready());
        EXPECT_EQ(s.high_outliers, 0u);
        EXPECT_EQ(s.low_outliers,  0u);
    }

    TEST(TaskTimerTest, RecordsOnDestruction)
    {
        ExecutorStat s;
        EXPECT_EQ(s.count, 0u);
        {
            TaskTimer t(s);
            // Burn a few microseconds so we can assert the timer
            // recorded a non-zero duration.
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
        EXPECT_EQ(s.count, 1u);
        EXPECT_GT(s.total_ns, 0u);
        EXPECT_EQ(s.max_ns, s.total_ns);
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

    static int gMockExitCallCount;
    static int gMockExitStatus;

    static void mock_exit_fn(int status)
    {
        gMockExitCallCount++;
        gMockExitStatus = status;
    }

    class GracefulShutdownExitTest : public ::testing::Test
    {
        protected:
            void SetUp() override
            {
                gMockExitCallCount = 0;
                gMockExitStatus = -1;
                gOrchShutdownRequested = 0;
            }

            void TearDown() override
            {
                gOrchShutdownRequested = 0;
                Recorder::Instance().swss.setAsync(false);
            }
    };

    TEST_F(GracefulShutdownExitTest, NoShutdownRequestedDoesNotExit)
    {
        Recorder::Instance().swss.setAsync(true);

        exit_if_graceful_shutdown_requested(mock_exit_fn);

        EXPECT_EQ(gMockExitCallCount, 0);
        // The recorder must be left untouched when no shutdown was requested.
        EXPECT_TRUE(Recorder::Instance().swss.isAsyncEnabled());
    }

    TEST_F(GracefulShutdownExitTest, ShutdownRequestDrainsRecorderAndExits)
    {
        Recorder::Instance().swss.setAsync(true);
        gOrchShutdownRequested = SIGTERM;

        exit_if_graceful_shutdown_requested(mock_exit_fn);

        EXPECT_EQ(gMockExitCallCount, 1);
        EXPECT_EQ(gMockExitStatus, 0);
        EXPECT_FALSE(Recorder::Instance().swss.isAsyncEnabled());
    }

    // Recursively free a UT-only redisReply tree allocated with calloc().
    // mock_hiredis::redisGetReply() hands out a copy per call (freed by the
    // RedisReply wrapper), so the original mockReply tree is owned by the test
    // and must be released here to avoid leaking across the test binary.
    static void freeMockReplyTree(redisReply *reply)
    {
        if (reply == nullptr)
        {
            return;
        }
        for (size_t i = 0; i < reply->elements; i++)
        {
            freeMockReplyTree(reply->element[i]);
        }
        free(reply->element);
        free(reply->str);
        free(reply);
    }

    // Feed a single "<op>" notification (no extra field/value pairs) into a
    // NotificationConsumer's internal queue via the mock-hiredis global reply,
    // exactly as the other mock_tests inject pub/sub messages. After this
    // returns, OrchDaemon::handleTaskStatsQuery() can pop() the message
    // straight off the queue.
    static void feedTaskStatsQuery(swss::NotificationConsumer *nc, const std::string &op)
    {
        // NotificationProducer::send() prepends (op, data) to the value list;
        // mirror that framing so NotificationConsumer::pop() extracts op.
        std::vector<swss::FieldValueTuple> framed;
        framed.emplace_back(op, "");
        std::string msg = swss::JSon::buildJson(framed);

        mockReply = (redisReply *)calloc(1, sizeof(redisReply));
        mockReply->type = REDIS_REPLY_ARRAY;
        mockReply->elements = 3;
        mockReply->element = (redisReply **)calloc(mockReply->elements, sizeof(redisReply *));
        mockReply->element[2] = (redisReply *)calloc(1, sizeof(redisReply));
        mockReply->element[2]->type = REDIS_REPLY_STRING;
        mockReply->element[2]->str = (char *)calloc(msg.length() + 1, sizeof(char));
        memcpy(mockReply->element[2]->str, msg.c_str(), msg.length());
        mockReply->element[2]->len = (int)msg.length();

        nc->readData();

        // Release the test-owned tree and clear the global so the subsequent
        // reply-producer PUBLISH (which also routes through the mocked
        // redisGetReply) does not re-parse it.
        freeMockReplyTree(mockReply);
        mockReply = nullptr;
    }

    // initTaskStatsChannel() must wire up the APPL_DB notification channels and
    // register the query consumer with the daemon's Select. Exercises the
    // channel-setup path that OrchDaemon::start() runs before the select loop.
    TEST_F(OrchDaemonTest, TaskStatsChannelInit)
    {
        orchd->initTaskStatsChannel();

        // The DB handle and both notification channels must be wired up, and
        // the query consumer registered with the daemon's Select (addSelectable
        // is exercised inside initTaskStatsChannel()).
        ASSERT_NE(orchd->m_taskStatsDb, nullptr);
        ASSERT_NE(orchd->m_taskStatsQuery, nullptr);
        ASSERT_NE(orchd->m_taskStatsReply, nullptr);
    }

    // "show": handleTaskStatsQuery() must serialize every slot in m_taskStats
    // (both populated and empty) into the reply without mutating the stats.
    TEST_F(OrchDaemonTest, TaskStatsQueryShow)
    {
        orchd->initTaskStatsChannel();

        // A populated slot (>=5 run samples so the P^2 estimators bootstrap and
        // the median.value() serialization branch runs) plus sched samples...
        auto &ports = orchd->m_taskStats["PortsOrch"];
        for (uint64_t v : {100ull, 110ull, 105ull, 95ull, 100ull, 102ull, 98ull})
            ports.record_run(v);
        for (uint64_t v : {50ull, 60ull, 55ull, 45ull, 52ull, 58ull})
            ports.record_sched(v);
        // ...and an empty slot to cover the count==0 serialization branch.
        orchd->m_taskStats["NeverRan"];

        feedTaskStatsQuery(orchd->m_taskStatsQuery.get(), "show");
        orchd->handleTaskStatsQuery();

        // "show" is read-only: the accumulated stats must be untouched.
        EXPECT_EQ(orchd->m_taskStats["PortsOrch"].count, 7u);
        EXPECT_EQ(orchd->m_taskStats["PortsOrch"].sched_count, 6u);
        EXPECT_EQ(orchd->m_taskStats["NeverRan"].count, 0u);
    }

    // "clear": handleTaskStatsQuery() must reset every slot in place, keeping
    // the keys but zeroing the counters and P^2 estimators.
    TEST_F(OrchDaemonTest, TaskStatsQueryClear)
    {
        orchd->initTaskStatsChannel();

        auto &ports = orchd->m_taskStats["PortsOrch"];
        for (int i = 0; i < 10; ++i) ports.record_run(100);
        ASSERT_EQ(orchd->m_taskStats["PortsOrch"].count, 10u);
        ASSERT_TRUE(orchd->m_taskStats["PortsOrch"].median.ready());

        feedTaskStatsQuery(orchd->m_taskStatsQuery.get(), "clear");
        orchd->handleTaskStatsQuery();

        // Slot key retained but fully reset.
        ASSERT_NE(orchd->m_taskStats.find("PortsOrch"), orchd->m_taskStats.end());
        EXPECT_EQ(orchd->m_taskStats["PortsOrch"].count, 0u);
        EXPECT_EQ(orchd->m_taskStats["PortsOrch"].total_ns, 0u);
        EXPECT_FALSE(orchd->m_taskStats["PortsOrch"].median.ready());
    }

    // An unrecognized op must hit the error branch and leave stats untouched.
    TEST_F(OrchDaemonTest, TaskStatsQueryUnknownOp)
    {
        orchd->initTaskStatsChannel();

        auto &ports = orchd->m_taskStats["PortsOrch"];
        for (int i = 0; i < 3; ++i) ports.record_run(100);

        feedTaskStatsQuery(orchd->m_taskStatsQuery.get(), "bogus");
        orchd->handleTaskStatsQuery();

        // Unknown op is a no-op against the stats.
        EXPECT_EQ(orchd->m_taskStats["PortsOrch"].count, 3u);
    }

}
