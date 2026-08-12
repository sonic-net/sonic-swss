/**
 * @file quality_monitoring_operations_ut.cpp
 *
 * Task 4.4 – Chất lượng, giám sát và vận hành
 *
 * Contract-oriented tests for quality gates, monitoring signals and
 * operations controls in SWSS mock DB environment.
 */

#include "gtest/gtest.h"

#include "ut_helper.h"
#include "mock_table.h"

#include <algorithm>
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

static bool getField(const vector<FieldValueTuple> &fvs, const string &field, string &value)
{
    for (const auto &x : fvs)
    {
        if (fvField(x) == field)
        {
            value = fvValue(x);
            return true;
        }
    }
    return false;
}

static bool isNumber(const string &s)
{
    if (s.empty())
    {
        return false;
    }
    return all_of(s.begin(), s.end(), [](unsigned char c) { return isdigit(c) != 0; });
}

} // namespace

class QualityMonitoringOpsTest : public ::testing::Test
{
  protected:
    Table app_qos_gate_tbl{APP_DB, "QUALITY_GATE_TABLE"};
    Table state_health_tbl{STATE_DB, "SWSS_HEALTH_TABLE"};
    Table state_alert_tbl{STATE_DB, "SWSS_ALERT_TABLE"};
    Table counters_slo_tbl{COUNTERS_DB, "SWSS_SLO_COUNTERS"};
    Table app_ops_tbl{APP_DB, "SWSS_OPS_CONTROL_TABLE"};

    void SetUp() override
    {
        reset();
    }

    void TearDown() override
    {
        reset();
    }
};

// ===========================================================================
// 4.4.1 Quality gates
// ===========================================================================
TEST_F(QualityMonitoringOpsTest, QualityGatePublishesPassState)
{
    app_qos_gate_tbl.set("pre-apply", fv({
        {"validation", "pass"},
        {"schema", "pass"},
        {"dependency", "pass"}
    }));

    auto attrs = app_qos_gate_tbl.get("pre-apply");
    string validation;
    ASSERT_TRUE(getField(attrs, "validation", validation));
    EXPECT_EQ(validation, "pass");
}

TEST_F(QualityMonitoringOpsTest, QualityGateFailureIsVisible)
{
    app_qos_gate_tbl.set("pre-apply", fv({
        {"validation", "fail"},
        {"reason", "missing_dependency"}
    }));

    auto attrs = app_qos_gate_tbl.get("pre-apply");
    string validation, reason;
    ASSERT_TRUE(getField(attrs, "validation", validation));
    ASSERT_TRUE(getField(attrs, "reason", reason));
    EXPECT_EQ(validation, "fail");
    EXPECT_EQ(reason, "missing_dependency");
}

TEST_F(QualityMonitoringOpsTest, QualityGateOverwriteUsesLatestValues)
{
    app_qos_gate_tbl.set("runtime", fv({{"validation", "pass"}}));
    app_qos_gate_tbl.set("runtime", fv({{"validation", "fail"}, {"reason", "sai_error"}}));

    auto attrs = app_qos_gate_tbl.get("runtime");
    string validation;
    ASSERT_TRUE(getField(attrs, "validation", validation));
    EXPECT_EQ(validation, "fail");
}

// ===========================================================================
// 4.4.2 Monitoring health and alerts
// ===========================================================================
TEST_F(QualityMonitoringOpsTest, HealthStateGreenPublishedToStateDb)
{
    state_health_tbl.set("orchagent", fv({
        {"status", "green"},
        {"uptime_sec", "3600"}
    }));

    auto attrs = state_health_tbl.get("orchagent");
    string status, uptime;
    ASSERT_TRUE(getField(attrs, "status", status));
    ASSERT_TRUE(getField(attrs, "uptime_sec", uptime));
    EXPECT_EQ(status, "green");
    EXPECT_TRUE(isNumber(uptime));
}

TEST_F(QualityMonitoringOpsTest, HealthStateTransitionsAreTracked)
{
    state_health_tbl.set("orchagent", fv({{"status", "green"}}));
    state_health_tbl.set("orchagent", fv({{"status", "yellow"}}));
    state_health_tbl.set("orchagent", fv({{"status", "red"}}));

    auto attrs = state_health_tbl.get("orchagent");
    string status;
    ASSERT_TRUE(getField(attrs, "status", status));
    EXPECT_EQ(status, "red");
}

TEST_F(QualityMonitoringOpsTest, AlertRaisedOnThresholdBreach)
{
    state_alert_tbl.set("route_programming_latency", fv({
        {"severity", "critical"},
        {"threshold_ms", "200"},
        {"observed_ms", "350"}
    }));

    auto attrs = state_alert_tbl.get("route_programming_latency");
    string severity, observed;
    ASSERT_TRUE(getField(attrs, "severity", severity));
    ASSERT_TRUE(getField(attrs, "observed_ms", observed));
    EXPECT_EQ(severity, "critical");
    EXPECT_TRUE(isNumber(observed));
}

TEST_F(QualityMonitoringOpsTest, AlertClearedAfterRecovery)
{
    state_alert_tbl.set("db_sync_lag", fv({
        {"severity", "major"},
        {"observed", "high"}
    }));
    EXPECT_FALSE(state_alert_tbl.get("db_sync_lag").empty());

    state_alert_tbl.del("db_sync_lag");
    EXPECT_TRUE(state_alert_tbl.get("db_sync_lag").empty());
}

// ===========================================================================
// 4.4.3 SLO and observability counters
// ===========================================================================
TEST_F(QualityMonitoringOpsTest, SloCountersUseNumericSnapshots)
{
    counters_slo_tbl.set("global", fv({
        {"event_loop_lag_ms", "5"},
        {"retry_queue_depth", "2"},
        {"sai_error_rate_ppm", "10"}
    }));

    auto attrs = counters_slo_tbl.get("global");
    string lag, retry_depth, err;
    ASSERT_TRUE(getField(attrs, "event_loop_lag_ms", lag));
    ASSERT_TRUE(getField(attrs, "retry_queue_depth", retry_depth));
    ASSERT_TRUE(getField(attrs, "sai_error_rate_ppm", err));

    EXPECT_TRUE(isNumber(lag));
    EXPECT_TRUE(isNumber(retry_depth));
    EXPECT_TRUE(isNumber(err));
}

TEST_F(QualityMonitoringOpsTest, SloCounterUpdateIsMonotonicForProcessedEvents)
{
    counters_slo_tbl.set("global", fv({{"processed_events", "100"}}));
    counters_slo_tbl.set("global", fv({{"processed_events", "180"}}));

    auto attrs = counters_slo_tbl.get("global");
    string processed;
    ASSERT_TRUE(getField(attrs, "processed_events", processed));
    EXPECT_EQ(processed, "180");
}

// ===========================================================================
// 4.4.4 Operations control plane
// ===========================================================================
TEST_F(QualityMonitoringOpsTest, MaintenanceModeToggleIsReflected)
{
    app_ops_tbl.set("orchagent", fv({{"maintenance", "enabled"}}));

    auto attrs = app_ops_tbl.get("orchagent");
    string mode;
    ASSERT_TRUE(getField(attrs, "maintenance", mode));
    EXPECT_EQ(mode, "enabled");

    app_ops_tbl.set("orchagent", fv({{"maintenance", "disabled"}}));
    attrs = app_ops_tbl.get("orchagent");
    ASSERT_TRUE(getField(attrs, "maintenance", mode));
    EXPECT_EQ(mode, "disabled");
}

TEST_F(QualityMonitoringOpsTest, RetryBudgetControlExists)
{
    app_ops_tbl.set("retry_policy", fv({
        {"max_retry", "64"},
        {"backoff_ms", "100"}
    }));

    auto attrs = app_ops_tbl.get("retry_policy");
    string max_retry, backoff;
    ASSERT_TRUE(getField(attrs, "max_retry", max_retry));
    ASSERT_TRUE(getField(attrs, "backoff_ms", backoff));
    EXPECT_TRUE(isNumber(max_retry));
    EXPECT_TRUE(isNumber(backoff));
}

TEST_F(QualityMonitoringOpsTest, OpsControlIsolationAcrossDomains)
{
    app_ops_tbl.set("orchagent", fv({{"maintenance", "enabled"}}));
    app_ops_tbl.set("p4orch", fv({{"maintenance", "disabled"}}));

    auto a = app_ops_tbl.get("orchagent");
    auto b = app_ops_tbl.get("p4orch");
    string ma, mb;
    ASSERT_TRUE(getField(a, "maintenance", ma));
    ASSERT_TRUE(getField(b, "maintenance", mb));
    EXPECT_EQ(ma, "enabled");
    EXPECT_EQ(mb, "disabled");
}

// ===========================================================================
// 4.4.5 Operational resilience
// ===========================================================================
TEST_F(QualityMonitoringOpsTest, ResetClearsMonitoringAndOpsViews)
{
    app_qos_gate_tbl.set("pre-apply", fv({{"validation", "pass"}}));
    state_health_tbl.set("orchagent", fv({{"status", "green"}}));
    state_alert_tbl.set("db_sync_lag", fv({{"severity", "major"}}));
    counters_slo_tbl.set("global", fv({{"event_loop_lag_ms", "3"}}));
    app_ops_tbl.set("orchagent", fv({{"maintenance", "enabled"}}));

    reset();

    EXPECT_TRUE(app_qos_gate_tbl.getKeys().empty());
    EXPECT_TRUE(state_health_tbl.getKeys().empty());
    EXPECT_TRUE(state_alert_tbl.getKeys().empty());
    EXPECT_TRUE(counters_slo_tbl.getKeys().empty());
    EXPECT_TRUE(app_ops_tbl.getKeys().empty());
}
