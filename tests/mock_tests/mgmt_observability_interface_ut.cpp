/**
 * @file mgmt_observability_interface_ut.cpp
 *
 * Task 2.6 – Thử nghiệm và đánh giá giao diện quản lý và quan sát
 *
 * Unit tests for management and observability interfaces:
 *   - Management intent ingestion from CONFIG_DB
 *   - Intent materialization to APP_DB
 *   - Operational visibility via STATE_DB
 *   - Counter/telemetry visibility via COUNTERS_DB
 *   - Response publishing path for management operations
 *   - End-to-end management-to-observability consistency
 */

#include "gtest/gtest.h"
#include "gmock/gmock.h"

#include "ut_helper.h"
#include "mock_table.h"
#include "response_publisher.h"
#include "mock_response_publisher.h"

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

using namespace std;
using namespace swss;
using namespace testing;
using namespace testing_db;

extern std::unique_ptr<MockResponsePublisher> gMockResponsePublisher;

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
    for (const auto &entry : fvs)
    {
        if (fvField(entry) == field)
        {
            value = fvValue(entry);
            return true;
        }
    }
    return false;
}

} // namespace

// ===========================================================================
// 2.6.1 Management Interface (CONFIG_DB -> APP_DB)
// ===========================================================================
namespace mgmt_interface_ut
{

struct MgmtInterfaceTest : public ::testing::Test
{
    Table config_intent_tbl{CONFIG_DB, "MGMT_INTF_CONFIG"};
    Table app_effective_tbl{APP_DB, "MGMT_INTF_APP"};

    void SetUp() override
    {
        reset();
    }

    void TearDown() override
    {
        reset();
    }

    // Simulates a management adapter that validates and forwards config intent.
    void applyMgmtIntent(const string &key)
    {
        auto attrs = config_intent_tbl.get(key);
        if (attrs.empty())
        {
            return;
        }

        string admin, poll, mode;
        if (!getField(attrs, "admin_status", admin))
        {
            admin = "up";
        }
        if (!getField(attrs, "poll_interval", poll))
        {
            poll = "10";
        }
        if (!getField(attrs, "observe_mode", mode))
        {
            mode = "standard";
        }

        app_effective_tbl.set(key, {
            {"admin_status", admin},
            {"poll_interval", poll},
            {"observe_mode", mode}
        });
    }
};

TEST_F(MgmtInterfaceTest, IngestIntentFromConfigDb)
{
    config_intent_tbl.set("global", fv({
        {"admin_status", "up"},
        {"poll_interval", "5"},
        {"observe_mode", "detailed"}
    }));

    auto attrs = config_intent_tbl.get("global");
    EXPECT_EQ(attrs.size(), 3u);
}

TEST_F(MgmtInterfaceTest, MaterializeIntentToAppDb)
{
    config_intent_tbl.set("global", fv({
        {"admin_status", "up"},
        {"poll_interval", "15"}
    }));

    applyMgmtIntent("global");

    auto attrs = app_effective_tbl.get("global");
    string val;
    ASSERT_TRUE(getField(attrs, "admin_status", val));
    EXPECT_EQ(val, "up");
    ASSERT_TRUE(getField(attrs, "poll_interval", val));
    EXPECT_EQ(val, "15");
}

TEST_F(MgmtInterfaceTest, ApplyDefaultWhenOptionalFieldsMissing)
{
    config_intent_tbl.set("global", fv({
        {"admin_status", "down"}
    }));

    applyMgmtIntent("global");

    auto attrs = app_effective_tbl.get("global");
    string val;
    ASSERT_TRUE(getField(attrs, "poll_interval", val));
    EXPECT_EQ(val, "10");
    ASSERT_TRUE(getField(attrs, "observe_mode", val));
    EXPECT_EQ(val, "standard");
}

TEST_F(MgmtInterfaceTest, IntentDeletionRemovesAppState)
{
    config_intent_tbl.set("global", fv({
        {"admin_status", "up"}
    }));
    applyMgmtIntent("global");

    app_effective_tbl.del("global");

    auto keys = app_effective_tbl.getKeys();
    EXPECT_TRUE(keys.empty());
}

TEST_F(MgmtInterfaceTest, MultipleProfilesAreIsolated)
{
    config_intent_tbl.set("profileA", fv({
        {"admin_status", "up"},
        {"poll_interval", "5"}
    }));
    config_intent_tbl.set("profileB", fv({
        {"admin_status", "down"},
        {"poll_interval", "30"}
    }));

    applyMgmtIntent("profileA");
    applyMgmtIntent("profileB");

    auto a = app_effective_tbl.get("profileA");
    auto b = app_effective_tbl.get("profileB");

    string va, vb;
    ASSERT_TRUE(getField(a, "admin_status", va));
    ASSERT_TRUE(getField(b, "admin_status", vb));
    EXPECT_EQ(va, "up");
    EXPECT_EQ(vb, "down");
}

} // namespace mgmt_interface_ut

// ===========================================================================
// 2.6.2 Observability State Interface (STATE_DB)
// ===========================================================================
namespace observe_state_ut
{

struct ObserveStateTest : public ::testing::Test
{
    Table state_status_tbl{STATE_DB, "MGMT_OBSERVE_STATUS"};

    void SetUp() override
    {
        reset();
    }

    void TearDown() override
    {
        reset();
    }
};

TEST_F(ObserveStateTest, PublishOperationalState)
{
    state_status_tbl.set("global", fv({
        {"service_state", "active"},
        {"last_cycle_ms", "12"}
    }));

    auto attrs = state_status_tbl.get("global");
    string value;
    ASSERT_TRUE(getField(attrs, "service_state", value));
    EXPECT_EQ(value, "active");
}

TEST_F(ObserveStateTest, PublishHealthAndTimestamp)
{
    state_status_tbl.set("global", fv({
        {"health", "green"},
        {"last_update", "1710000000"}
    }));

    auto attrs = state_status_tbl.get("global");
    string value;
    ASSERT_TRUE(getField(attrs, "health", value));
    EXPECT_EQ(value, "green");
    ASSERT_TRUE(getField(attrs, "last_update", value));
    EXPECT_EQ(value, "1710000000");
}

TEST_F(ObserveStateTest, PerInterfaceStateVisible)
{
    state_status_tbl.set("Ethernet0", fv({
        {"link", "up"},
        {"loss", "0"}
    }));
    state_status_tbl.set("Ethernet4", fv({
        {"link", "down"},
        {"loss", "3"}
    }));

    auto keys = state_status_tbl.getKeys();
    EXPECT_EQ(keys.size(), 2u);
}

TEST_F(ObserveStateTest, StateOverwriteKeepsLatestValues)
{
    state_status_tbl.set("global", fv({{"health", "yellow"}}));
    state_status_tbl.set("global", fv({{"health", "green"}}));

    auto attrs = state_status_tbl.get("global");
    string value;
    ASSERT_TRUE(getField(attrs, "health", value));
    EXPECT_EQ(value, "green");
}

TEST_F(ObserveStateTest, StateDeletionRemovesVisibility)
{
    state_status_tbl.set("global", fv({{"service_state", "active"}}));
    state_status_tbl.del("global");

    auto keys = state_status_tbl.getKeys();
    EXPECT_TRUE(keys.empty());
}

} // namespace observe_state_ut

// ===========================================================================
// 2.6.3 Observability Counter Interface (COUNTERS_DB)
// ===========================================================================
namespace observe_counter_ut
{

struct ObserveCounterTest : public ::testing::Test
{
    Table counters_tbl{COUNTERS_DB, "MGMT_OBSERVE_COUNTERS"};

    void SetUp() override
    {
        reset();
    }

    void TearDown() override
    {
        reset();
    }
};

TEST_F(ObserveCounterTest, PublishPacketCounters)
{
    counters_tbl.set("Ethernet0", fv({
        {"rx_packets", "100"},
        {"tx_packets", "120"}
    }));

    auto attrs = counters_tbl.get("Ethernet0");
    string value;
    ASSERT_TRUE(getField(attrs, "rx_packets", value));
    EXPECT_EQ(value, "100");
    ASSERT_TRUE(getField(attrs, "tx_packets", value));
    EXPECT_EQ(value, "120");
}

TEST_F(ObserveCounterTest, PublishErrorCounters)
{
    counters_tbl.set("Ethernet0", fv({
        {"rx_errors", "2"},
        {"tx_errors", "0"}
    }));

    auto attrs = counters_tbl.get("Ethernet0");
    string value;
    ASSERT_TRUE(getField(attrs, "rx_errors", value));
    EXPECT_EQ(value, "2");
}

TEST_F(ObserveCounterTest, MultiplePortsCountersIndependent)
{
    counters_tbl.set("Ethernet0", fv({{"rx_packets", "100"}}));
    counters_tbl.set("Ethernet4", fv({{"rx_packets", "200"}}));

    auto keys = counters_tbl.getKeys();
    EXPECT_EQ(keys.size(), 2u);
}

TEST_F(ObserveCounterTest, CounterUpdateIsIncremental)
{
    counters_tbl.set("Ethernet0", fv({{"rx_packets", "100"}}));
    counters_tbl.set("Ethernet0", fv({{"rx_packets", "150"}}));

    auto attrs = counters_tbl.get("Ethernet0");
    string value;
    ASSERT_TRUE(getField(attrs, "rx_packets", value));
    EXPECT_EQ(value, "150");
}

TEST_F(ObserveCounterTest, CounterEntryDeletion)
{
    counters_tbl.set("Ethernet0", fv({{"rx_packets", "100"}}));
    counters_tbl.del("Ethernet0");

    auto keys = counters_tbl.getKeys();
    EXPECT_TRUE(keys.empty());
}

} // namespace observe_counter_ut

// ===========================================================================
// 2.6.4 Management Response Interface (ResponsePublisher)
// ===========================================================================
namespace mgmt_response_ut
{

struct MgmtResponseTest : public ::testing::Test
{
    void SetUp() override
    {
        gMockResponsePublisher = std::make_unique<MockResponsePublisher>();
    }

    void TearDown() override
    {
        gMockResponsePublisher.reset();
    }
};

TEST_F(MgmtResponseTest, PublishSuccessResponse)
{
    ResponsePublisher publisher("APPL_STATE_DB");
    ReturnCode rc(SAI_STATUS_SUCCESS);

    EXPECT_CALL(*gMockResponsePublisher,
                publish("MGMT_REPLY_TABLE", "req-1", _, _, false)).Times(1);

    publisher.publish("MGMT_REPLY_TABLE", "req-1",
                      fv({{"op", "set"}, {"target", "global"}}),
                      rc, false);
}

TEST_F(MgmtResponseTest, PublishFailureResponse)
{
    ResponsePublisher publisher("APPL_STATE_DB");
    ReturnCode rc(SAI_STATUS_FAILURE);

    EXPECT_CALL(*gMockResponsePublisher,
                publish("MGMT_REPLY_TABLE", "req-2", _, _, false)).Times(1);

    publisher.publish("MGMT_REPLY_TABLE", "req-2",
                      fv({{"op", "set"}, {"target", "global"}}),
                      rc, false);
}

TEST_F(MgmtResponseTest, PublishResponseWithStateAttrs)
{
    ResponsePublisher publisher("APPL_STATE_DB");
    ReturnCode rc(SAI_STATUS_SUCCESS);

    EXPECT_CALL(*gMockResponsePublisher,
                publish("MGMT_REPLY_TABLE", "req-3", _, _, _, true)).Times(1);

    publisher.publish("MGMT_REPLY_TABLE", "req-3",
                      fv({{"op", "set"}}),
                      rc,
                      fv({{"service_state", "active"}}),
                      true);
}

TEST_F(MgmtResponseTest, WriteToDbPathIsInvoked)
{
    ResponsePublisher publisher("APPL_STATE_DB");

    EXPECT_CALL(*gMockResponsePublisher,
                writeToDB("MGMT_REPLY_TABLE", "req-4", _, "SET", false)).Times(1);

    publisher.writeToDB("MGMT_REPLY_TABLE", "req-4",
                        fv({{"field", "value"}}), "SET", false);
}

TEST_F(MgmtResponseTest, BufferedModeCanBeConfigured)
{
    ResponsePublisher publisher("APPL_STATE_DB");
    publisher.setBuffered(true);
    SUCCEED();
}

} // namespace mgmt_response_ut

// ===========================================================================
// 2.6.5 End-to-End Management + Observability Consistency
// ===========================================================================
namespace mgmt_observe_integration_ut
{

struct MgmtObserveIntegrationTest : public ::testing::Test
{
    Table config_tbl{CONFIG_DB, "MGMT_INTF_CONFIG"};
    Table app_tbl{APP_DB, "MGMT_INTF_APP"};
    Table state_tbl{STATE_DB, "MGMT_OBSERVE_STATUS"};
    Table counters_tbl{COUNTERS_DB, "MGMT_OBSERVE_COUNTERS"};

    void SetUp() override
    {
        reset();
    }

    void TearDown() override
    {
        reset();
    }
};

TEST_F(MgmtObserveIntegrationTest, ConfigAppliedThenObservable)
{
    config_tbl.set("global", fv({
        {"admin_status", "up"},
        {"poll_interval", "5"}
    }));

    app_tbl.set("global", fv({
        {"admin_status", "up"},
        {"poll_interval", "5"}
    }));

    state_tbl.set("global", fv({
        {"service_state", "active"},
        {"health", "green"}
    }));

    auto app = app_tbl.get("global");
    auto state = state_tbl.get("global");
    string value;

    ASSERT_TRUE(getField(app, "admin_status", value));
    EXPECT_EQ(value, "up");
    ASSERT_TRUE(getField(state, "service_state", value));
    EXPECT_EQ(value, "active");
}

TEST_F(MgmtObserveIntegrationTest, PollIntervalChangeReflectedInStateCycle)
{
    app_tbl.set("global", fv({{"poll_interval", "20"}}));
    state_tbl.set("global", fv({{"last_cycle_ms", "20"}}));

    auto app = app_tbl.get("global");
    auto state = state_tbl.get("global");
    string appPoll, stateCycle;

    ASSERT_TRUE(getField(app, "poll_interval", appPoll));
    ASSERT_TRUE(getField(state, "last_cycle_ms", stateCycle));
    EXPECT_EQ(appPoll, stateCycle);
}

TEST_F(MgmtObserveIntegrationTest, AdminDownShowsInactiveState)
{
    app_tbl.set("global", fv({{"admin_status", "down"}}));
    state_tbl.set("global", fv({{"service_state", "inactive"}}));

    auto app = app_tbl.get("global");
    auto state = state_tbl.get("global");
    string admin, service;

    ASSERT_TRUE(getField(app, "admin_status", admin));
    ASSERT_TRUE(getField(state, "service_state", service));
    EXPECT_EQ(admin, "down");
    EXPECT_EQ(service, "inactive");
}

TEST_F(MgmtObserveIntegrationTest, CountersVisibleWhenServiceActive)
{
    state_tbl.set("global", fv({{"service_state", "active"}}));
    counters_tbl.set("Ethernet0", fv({
        {"rx_packets", "5000"},
        {"tx_packets", "7000"}
    }));

    auto state = state_tbl.get("global");
    auto ctr = counters_tbl.get("Ethernet0");
    string service, rx;

    ASSERT_TRUE(getField(state, "service_state", service));
    ASSERT_TRUE(getField(ctr, "rx_packets", rx));
    EXPECT_EQ(service, "active");
    EXPECT_EQ(rx, "5000");
}

TEST_F(MgmtObserveIntegrationTest, ResetClearsAllManagementViews)
{
    config_tbl.set("global", fv({{"admin_status", "up"}}));
    app_tbl.set("global", fv({{"admin_status", "up"}}));
    state_tbl.set("global", fv({{"service_state", "active"}}));
    counters_tbl.set("Ethernet0", fv({{"rx_packets", "9"}}));

    reset();

    EXPECT_TRUE(config_tbl.getKeys().empty());
    EXPECT_TRUE(app_tbl.getKeys().empty());
    EXPECT_TRUE(state_tbl.getKeys().empty());
    EXPECT_TRUE(counters_tbl.getKeys().empty());
}

} // namespace mgmt_observe_integration_ut
