/*
 * Unit tests: RouteSync accessors / state added for the warm-reboot drain barrier.
 *
 * Covers the trivial-but-load-bearing facts:
 *   - drain flag default and set/get round-trip
 *   - hasZmqProducerTables() reflects the ctor-time wiring
 *     (false for plain ProducerStateTable, true when we simulate ZMQ on)
 *   - totalDbUpdaterQueueSize() returns 0 when no ZmqProducerStateTable
 *     is present (the only case we can exercise without a real ZmqClient)
 *
 * Test file scope deliberately narrow: state accessors only. Behavior
 * tests live in test_drain_barrier_gate.cpp (route-flow gating) and the
 * upcoming test_drain_barrier_handler.cpp (handler logic, after refactor).
 */

#include "ut_helpers_drain_barrier.h"

#include <gtest/gtest.h>

// ---------- drain flag ----------

TEST_F(WarmRebootDrainFixture, DrainFlag_DefaultIsFalse)
{
    EXPECT_FALSE(drainFlag());
}

TEST_F(WarmRebootDrainFixture, DrainFlag_SetThenGet_True)
{
    m_routeSync.setDrainingForWarmRestart(true);
    EXPECT_TRUE(drainFlag());
    EXPECT_TRUE(m_routeSync.isDrainingForWarmRestart());
}

TEST_F(WarmRebootDrainFixture, DrainFlag_SetThenClear)
{
    m_routeSync.setDrainingForWarmRestart(true);
    m_routeSync.setDrainingForWarmRestart(false);
    EXPECT_FALSE(drainFlag());
    EXPECT_FALSE(m_routeSync.isDrainingForWarmRestart());
}

TEST_F(WarmRebootDrainFixture, DrainFlag_SetTrueTwice_StillTrue)
{
    m_routeSync.setDrainingForWarmRestart(true);
    m_routeSync.setDrainingForWarmRestart(true);
    EXPECT_TRUE(drainFlag());
}

// ---------- hasZmqProducerTables ----------

TEST_F(WarmRebootDrainFixture, HasZmqProducerTables_FalseWhenZmqOff)
{
    // Default ctor wiring: ORCH_NORTHBOND_ROUTE_ZMQ_ENABLED is false in
    // the test environment, so both m_routeTable and m_label_routeTable
    // are plain ProducerStateTable — dynamic_pointer_cast<ZmqProducerStateTable>
    // returns nullptr for both → m_hasZmqProducerTables = false.
    EXPECT_FALSE(hasZmqTables());
    EXPECT_FALSE(m_routeSync.hasZmqProducerTables());
}

TEST_F(WarmRebootDrainFixtureZmqOn, HasZmqProducerTables_TrueWhenZmqOnSimulated)
{
    // The Zmq-on variant pokes m_hasZmqProducerTables in SetUp().
    EXPECT_TRUE(hasZmqTables());
    EXPECT_TRUE(m_routeSync.hasZmqProducerTables());
}

// ---------- totalDbUpdaterQueueSize ----------

TEST_F(WarmRebootDrainFixture, TotalDbUpdaterQueueSize_ZeroWhenNoZmqTables)
{
    // With ZMQ off, both producer tables are plain ProducerStateTable;
    // dynamic_pointer_cast<ZmqProducerStateTable> returns nullptr for
    // both, and the method's defensive accumulator stays at 0.
    EXPECT_EQ(0u, totalQueueSize());
}

TEST_F(WarmRebootDrainFixtureZmqOn, TotalDbUpdaterQueueSize_ZeroEvenWhenFlagTrueButNoRealZmqTables)
{
    // The fixture only flips m_hasZmqProducerTables; the underlying
    // shared_ptrs are still plain ProducerStateTable. The
    // dynamic_pointer_cast inside totalDbUpdaterQueueSize() still
    // returns nullptr, so we still get 0.
    //
    // This is intentional: it documents that hasZmqProducerTables() and
    // totalDbUpdaterQueueSize() answer different questions and shouldn't
    // be conflated. The handler is responsible for using the right one
    // (hasZmqProducerTables for "should I even consider draining",
    // totalDbUpdaterQueueSize for "is there anything to drain right now").
    EXPECT_TRUE(hasZmqTables());
    EXPECT_EQ(0u, totalQueueSize());
}
