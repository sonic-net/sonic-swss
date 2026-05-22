#pragma once

/*
 * Warm-reboot drain-barrier unit-test framework.
 *
 * Provides:
 *   - WarmRebootDrainFixture / WarmRebootDrainFixtureZmqOn — gtest fixtures
 *     that set up RouteSync against the mocked DB stack and give tests a
 *     consistent set of accessors and assertion helpers.
 *   - ut_drain::RestartCheckPayload — builder for the notification payload
 *     fpmsyncd_restart_check sends on FPMSYNCD_RESTARTCHECK.
 *   - State helpers for STATE_DB|WARM_RESTART_TABLE|fpmsyncd inspection.
 *
 * The fixture deliberately exposes a small, intention-revealing surface
 * ("did a route emission flow through?", "what's drain_state?"), keeping
 * individual tests short and reading at the level of behavior rather
 * than wiring.
 */

#include <gtest/gtest.h>

// Access RouteSync private members from tests (same pattern as
// FpmSyncdResponseTest in test_routesync.cpp).
#define private public
#define protected public
#include "fpmsyncd/routesync.h"
#undef protected
#undef private

#include "dbconnector.h"
#include "redispipeline.h"
#include "schema.h"
#include "table.h"

#include <memory>
#include <string>
#include <vector>

namespace ut_drain {

/*
 * Payload builder for the FPMSYNCD_RESTARTCHECK notification, mirroring
 * what fpmsyncd_restart_check sends in production. Each field has an
 * accompanying "has" bool so tests can simulate "tool default" (no field
 * present in the payload) vs an explicit value. We can't use std::optional
 * here — the mock_tests build uses -std=c++14.
 */
struct RestartCheckPayload
{
    int  autoResumeTimeoutSec     = 0;
    bool hasAutoResumeTimeoutSec  = false;
    bool resume                   = false;
    bool hasResume                = false;

    // Fluent setters keep tests terse:
    //   RestartCheckPayload().withAutoResumeTimeoutSec(30).withResume(true)
    RestartCheckPayload& withAutoResumeTimeoutSec(int s)
    {
        autoResumeTimeoutSec = s;
        hasAutoResumeTimeoutSec = true;
        return *this;
    }
    RestartCheckPayload& withResume(bool r)
    {
        resume = r;
        hasResume = true;
        return *this;
    }

    std::vector<swss::FieldValueTuple> toValues() const;
};

}  // namespace ut_drain


/*
 * Base fixture: RouteSync with ZMQ disabled (the default ctor wiring on
 * a non-ZMQ image). hasZmqProducerTables() is false; the drain barrier's
 * implicit gate makes the handler treat all paths as no-ops.
 */
class WarmRebootDrainFixture : public ::testing::Test
{
public:
    void SetUp() override;
    void TearDown() override;

protected:
    // ===== RouteSync state accessors (read-only views into private state) =====

    bool   drainFlag() const     { return m_routeSync.m_drainingForWarmRestart; }
    bool   hasZmqTables() const  { return m_routeSync.m_hasZmqProducerTables; }
    size_t totalQueueSize()      { return m_routeSync.totalDbUpdaterQueueSize(); }

    // ===== Producer-table emission counting =====
    //
    // After setRouteWithWarmRestart / delWithWarmRestart, the route key
    // either does (gate off) or does not (gate on) appear in the mocked
    // APPL_DB ROUTE_TABLE. routePresent() answers the question directly;
    // routeCount() counts all keys in the table.

    bool   routePresent(const std::string& prefix);
    size_t routeKeyCount();
    bool   labelRoutePresent(const std::string& label);

    // ===== Inject route operations through the gated APIs =====
    //
    // These call RouteSync::setRouteWithWarmRestart / delWithWarmRestart
    // directly with a minimal RouteTableFieldValueTupleWrapper, the same
    // way the real onRouteMsg path does. Gating happens at the entry of
    // those methods — tests assert behavior by checking routePresent()
    // after the call.

    void injectRouteSet(const std::string& prefix = "10.99.0.0/24",
                        const std::string& nexthop = "192.168.99.1",
                        const std::string& ifname  = "Loopback0");
    void injectRouteDel(const std::string& prefix = "10.99.0.0/24");

    // setTable path — used for VNET/srv6/nexthop-group writes, NOT gated
    // by the drain barrier. Used as a regression guard.
    void injectVnetSet(const std::string& key = "Vnet1:10.100.0.0/24");

    // ===== STATE_DB|WARM_RESTART_TABLE|fpmsyncd helpers =====
    //
    // Tests use these to assert visibility-row content set by the
    // handler (drain_state, drain_queue_size) and to verify the HDEL
    // cleanup at process startup.

    void   writeDrainStateRow(const std::string& drain_state,
                              const std::string& drain_queue_size);
    void   writeLegacyQueueSize(const std::string& v);  // legacy `queueSize` field for migration tests
    bool   drainStateRowEmpty();
    std::string getDrainStateField();
    std::string getDrainQueueSizeField();
    std::string getLegacyQueueSizeField();

    // Convenience: clear any existing drain_state/drain_queue_size/queueSize
    // (mirrors what fpmsyncd does at startup). Helpful for tests that
    // want a clean slate.
    void clearDrainStateFields();

    // ===== Fixture state =====
    //
    // Members are constructed in declaration order at fixture-ctor time
    // (NOT in SetUp). This matters: m_routeSync's ctor takes a real
    // pipeline pointer derived from m_db, so m_db / m_pipeline must be
    // listed before m_routeSync. SetUp is reserved for per-test mock-DB
    // reset, not member construction. (Same pattern as
    // FpmSyncdResponseTest in test_routesync.cpp.)

    std::shared_ptr<swss::DBConnector>   m_db
        { std::make_shared<swss::DBConnector>("APPL_DB",  0, true) };
    std::shared_ptr<swss::DBConnector>   m_stateDb
        { std::make_shared<swss::DBConnector>("STATE_DB", 0, true) };
    std::shared_ptr<swss::RedisPipeline> m_pipeline
        { std::make_shared<swss::RedisPipeline>(m_db.get()) };

    // RouteSync ctor reads ORCH_NORTHBOND_ROUTE_ZMQ_ENABLED via
    // create_local_zmq_client and constructs m_routeTable as either a
    // plain ProducerStateTable (default) or a ZmqProducerStateTable.
    // In the base fixture we don't tamper with that — ZMQ is off.
    swss::RouteSync m_routeSync{ m_pipeline.get() };

    // Direct Tables for inspection (separate from RouteSync's internal
    // producer tables, which write to the same Redis keys).
    std::unique_ptr<swss::Table> m_routeTableInspect
        { new swss::Table(m_db.get(),      APP_ROUTE_TABLE_NAME) };
    std::unique_ptr<swss::Table> m_labelRouteTableInspect
        { new swss::Table(m_db.get(),      APP_LABEL_ROUTE_TABLE_NAME) };
    std::unique_ptr<swss::Table> m_warmRestartTable
        { new swss::Table(m_stateDb.get(), STATE_WARM_RESTART_TABLE_NAME) };
};


/*
 * Variant: simulate ZMQ-on wiring without needing a real ZmqClient at
 * RouteSync ctor time. Pokes m_hasZmqProducerTables = true after the
 * base SetUp. (The drain barrier's runtime behavior is gated on that
 * boolean; the test doesn't need actual ZmqProducerStateTable instances
 * to exercise gating semantics.)
 */
class WarmRebootDrainFixtureZmqOn : public WarmRebootDrainFixture
{
public:
    void SetUp() override;
};
