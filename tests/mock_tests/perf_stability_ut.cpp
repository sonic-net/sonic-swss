/**
 * @file perf_stability_ut.cpp
 *
 * Task 2.7 – Thử nghiệm hiệu năng và độ ổn định
 *
 * Unit tests for performance and stability characteristics in mock environment:
 *   - High-volume DB write/read throughput checks
 *   - Batched producer operation behavior under load
 *   - Stability under repeated updates and churn (add/del)
 *   - Retry resilience for transient failures
 *   - Cross-DB consistency under sustained stress
 */

#include "gtest/gtest.h"

#include "ut_helper.h"
#include "mock_table.h"

#include <chrono>
#include <string>
#include <vector>

using namespace std;
using namespace swss;
using namespace testing_db;

namespace
{

static vector<FieldValueTuple> fv(initializer_list<pair<string, string>> pairs)
{
    vector<FieldValueTuple> out;
    for (const auto &p : pairs)
    {
        out.emplace_back(p.first, p.second);
    }
    return out;
}

static bool findField(const vector<FieldValueTuple> &fvs, const string &field, string &value)
{
    for (const auto &item : fvs)
    {
        if (fvField(item) == field)
        {
            value = fvValue(item);
            return true;
        }
    }
    return false;
}

} // namespace

// ===========================================================================
// 2.7.1 Throughput under load
// ===========================================================================
namespace perf_throughput_ut
{

struct PerfThroughputTest : public ::testing::Test
{
    shared_ptr<DBConnector> app_db;

    void SetUp() override
    {
        reset();
        app_db = make_shared<DBConnector>("APPL_DB", 0);
    }

    void TearDown() override
    {
        reset();
    }
};

TEST_F(PerfThroughputTest, HighVolumeTableSetAndGet)
{
    Table tbl(app_db.get(), "PERF_TABLE");
    const int total = 5000;

    auto start = chrono::steady_clock::now();
    for (int i = 0; i < total; ++i)
    {
        tbl.set("k" + to_string(i), fv({
            {"seq", to_string(i)},
            {"state", "ok"}
        }));
    }

    vector<string> keys;
    tbl.getKeys(keys);
    auto end = chrono::steady_clock::now();

    auto elapsed_ms = chrono::duration_cast<chrono::milliseconds>(end - start).count();
    EXPECT_EQ(static_cast<int>(keys.size()), total);

    // Loose upper bound to detect pathological regressions in mock DB path.
    EXPECT_LT(elapsed_ms, 10000);
}

TEST_F(PerfThroughputTest, ProducerStateTableBatchLoad)
{
    ProducerStateTable pst(app_db.get(), "PERF_BATCH_TABLE");
    const int total = 3000;

    vector<KeyOpFieldsValuesTuple> ops;
    ops.reserve(total);

    for (int i = 0; i < total; ++i)
    {
        ops.emplace_back("bk" + to_string(i), SET_COMMAND,
                         fv({{"field", "v" + to_string(i)}}));
    }

    auto start = chrono::steady_clock::now();
    pst.set(ops);
    auto end = chrono::steady_clock::now();

    Table tbl(app_db.get(), "PERF_BATCH_TABLE");
    vector<string> keys;
    tbl.getKeys(keys);

    auto elapsed_ms = chrono::duration_cast<chrono::milliseconds>(end - start).count();
    EXPECT_EQ(static_cast<int>(keys.size()), total);
    EXPECT_LT(elapsed_ms, 10000);
}

} // namespace perf_throughput_ut

// ===========================================================================
// 2.7.2 Stability under repeated operations
// ===========================================================================
namespace perf_stability_loop_ut
{

struct PerfStabilityLoopTest : public ::testing::Test
{
    shared_ptr<DBConnector> app_db;

    void SetUp() override
    {
        reset();
        app_db = make_shared<DBConnector>("APPL_DB", 0);
    }

    void TearDown() override
    {
        reset();
    }
};

TEST_F(PerfStabilityLoopTest, RepeatedUpdateKeepsLastValue)
{
    Table tbl(app_db.get(), "STABLE_TABLE");

    for (int i = 0; i < 2000; ++i)
    {
        tbl.set("service", fv({
            {"generation", to_string(i)},
            {"health", (i % 2 == 0) ? "green" : "yellow"}
        }));
    }

    auto attrs = tbl.get("service");
    string generation;
    ASSERT_TRUE(findField(attrs, "generation", generation));
    EXPECT_EQ(generation, "1999");
}

TEST_F(PerfStabilityLoopTest, HighChurnAddDeleteNoLeaks)
{
    Table tbl(app_db.get(), "CHURN_TABLE");
    const int rounds = 100;
    const int batch = 200;

    for (int r = 0; r < rounds; ++r)
    {
        for (int i = 0; i < batch; ++i)
        {
            tbl.set("k" + to_string(r) + "_" + to_string(i), fv({
                {"round", to_string(r)},
                {"idx", to_string(i)}
            }));
        }

        for (int i = 0; i < batch; ++i)
        {
            tbl.del("k" + to_string(r) + "_" + to_string(i));
        }
    }

    vector<string> keys;
    tbl.getKeys(keys);
    EXPECT_TRUE(keys.empty());
}

TEST_F(PerfStabilityLoopTest, RepeatedResetCycleStable)
{
    Table t_app(APP_DB, "RESET_APP_TABLE");
    Table t_state(STATE_DB, "RESET_STATE_TABLE");

    for (int i = 0; i < 50; ++i)
    {
        t_app.set("g", fv({{"v", to_string(i)}}));
        t_state.set("g", fv({{"s", "ok"}}));

        reset();

        EXPECT_TRUE(t_app.getKeys().empty());
        EXPECT_TRUE(t_state.getKeys().empty());
    }
}

} // namespace perf_stability_loop_ut

// ===========================================================================
// 2.7.3 Retry resilience under transient failures
// ===========================================================================
namespace perf_retry_resilience_ut
{

struct RetryResilienceTest : public ::testing::Test
{
    Table app_tbl{APP_DB, "RETRY_TABLE"};

    void SetUp() override
    {
        reset();
    }

    void TearDown() override
    {
        reset();
    }

    bool applyWithTransientFailure(const string &key,
                                   const vector<FieldValueTuple> &attrs,
                                   int fail_before_success,
                                   int max_retry,
                                   int &attempts)
    {
        attempts = 0;
        while (attempts < max_retry)
        {
            if (attempts >= fail_before_success)
            {
                app_tbl.set(key, attrs);
                return true;
            }
            ++attempts;
        }
        return false;
    }
};

TEST_F(RetryResilienceTest, SucceedsBeforeRetryBudget)
{
    int attempts = 0;
    bool ok = applyWithTransientFailure("job1", fv({{"state", "done"}}),
                                        3, 10, attempts);

    EXPECT_TRUE(ok);
    auto attrs = app_tbl.get("job1");
    string state;
    ASSERT_TRUE(findField(attrs, "state", state));
    EXPECT_EQ(state, "done");
}

TEST_F(RetryResilienceTest, FailsWhenRetryBudgetExceeded)
{
    int attempts = 0;
    bool ok = applyWithTransientFailure("job2", fv({{"state", "done"}}),
                                        10, 5, attempts);

    EXPECT_FALSE(ok);
    EXPECT_TRUE(app_tbl.get("job2").empty());
}

TEST_F(RetryResilienceTest, IndependentKeysHaveIndependentRetryOutcome)
{
    int a1 = 0, a2 = 0;
    bool ok1 = applyWithTransientFailure("jobA", fv({{"state", "doneA"}}), 1, 5, a1);
    bool ok2 = applyWithTransientFailure("jobB", fv({{"state", "doneB"}}), 6, 5, a2);

    EXPECT_TRUE(ok1);
    EXPECT_FALSE(ok2);
    EXPECT_FALSE(app_tbl.get("jobA").empty());
    EXPECT_TRUE(app_tbl.get("jobB").empty());
}

} // namespace perf_retry_resilience_ut

// ===========================================================================
// 2.7.4 Cross-DB consistency under sustained stress
// ===========================================================================
namespace perf_cross_db_consistency_ut
{

struct CrossDbConsistencyTest : public ::testing::Test
{
    Table config_tbl{CONFIG_DB, "PERF_CFG_TABLE"};
    Table app_tbl{APP_DB, "PERF_APP_TABLE"};
    Table state_tbl{STATE_DB, "PERF_STATE_TABLE"};
    Table counters_tbl{COUNTERS_DB, "PERF_COUNTERS_TABLE"};

    void SetUp() override
    {
        reset();
    }

    void TearDown() override
    {
        reset();
    }
};

TEST_F(CrossDbConsistencyTest, MultiDbStressPopulation)
{
    const int total = 1500;

    for (int i = 0; i < total; ++i)
    {
        string key = "Ethernet" + to_string(i);
        config_tbl.set(key, fv({{"admin", "up"}, {"poll", "5"}}));
        app_tbl.set(key, fv({{"admin", "up"}, {"effective", "true"}}));
        state_tbl.set(key, fv({{"oper", (i % 2 == 0) ? "up" : "down"}}));
        counters_tbl.set(key, fv({{"rx", to_string(i * 10)}, {"tx", to_string(i * 12)}}));
    }

    EXPECT_EQ(static_cast<int>(config_tbl.getKeys().size()), total);
    EXPECT_EQ(static_cast<int>(app_tbl.getKeys().size()), total);
    EXPECT_EQ(static_cast<int>(state_tbl.getKeys().size()), total);
    EXPECT_EQ(static_cast<int>(counters_tbl.getKeys().size()), total);
}

TEST_F(CrossDbConsistencyTest, PartialStateFailureDoesNotCorruptAppDb)
{
    const int total = 400;
    int skipped_state = 0;

    for (int i = 0; i < total; ++i)
    {
        string key = "Port" + to_string(i);
        app_tbl.set(key, fv({{"intent", "set"}}));

        if (i % 10 == 0)
        {
            ++skipped_state;
            continue;
        }

        state_tbl.set(key, fv({{"status", "ok"}}));
    }

    EXPECT_EQ(static_cast<int>(app_tbl.getKeys().size()), total);
    EXPECT_EQ(static_cast<int>(state_tbl.getKeys().size()), total - skipped_state);
}

TEST_F(CrossDbConsistencyTest, StressReadBackSpotChecks)
{
    const int total = 1000;
    for (int i = 0; i < total; ++i)
    {
        counters_tbl.set("Eth" + to_string(i), fv({{"rx", to_string(i)}}));
    }

    for (int i = 0; i < total; i += 137)
    {
        auto attrs = counters_tbl.get("Eth" + to_string(i));
        string rx;
        ASSERT_TRUE(findField(attrs, "rx", rx));
        EXPECT_EQ(rx, to_string(i));
    }
}

} // namespace perf_cross_db_consistency_ut
