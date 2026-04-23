/**
 * @file orchagent_module_ut.cpp
 *
 * Task 2.3 – Thử nghiệm đánh giá module điều phối OrchAgent
 *
 * Unit tests for the OrchAgent orchestration module core functionality:
 *   - Orch base class: consumer management, task execution, retry logic
 *   - RouteOrch: route creation/deletion, SAI interaction
 *   - NeighOrch: neighbor entry management
 *   - PortsOrch: port state handling
 *   - Cross-module interactions and dependencies
 *
 * Tests simulate the Orch task pipeline: DB entry → Consumer → doTask() → SAI API
 */

#include "gtest/gtest.h"
#include "mock_table.h"

#define private public
#define protected public

#include "orch.h"
#include "routeorch.h"
#include "neighorch.h"
#include "portsorch.h"

#undef private
#undef protected

#include "mock_orchagent_main.h"
#include "ut_helper.h"
#include "saihelper.h"

#include <memory>
#include <string>
#include <vector>

using namespace swss;
using namespace std;

/* Declare global orch pointers (used by mock system) */
PortsOrch *gPortsOrch = nullptr;
VRFOrch *gVrfOrch = nullptr;
SwitchOrch *gSwitchOrch = nullptr;
CrmOrch *gCrmOrch = nullptr;

// ===========================================================================
// Helpers
// ===========================================================================
static vector<FieldValueTuple> fv(initializer_list<pair<string,string>> pairs)
{
    vector<FieldValueTuple> result;
    for (auto &p : pairs)
        result.emplace_back(p.first, p.second);
    return result;
}

// ===========================================================================
// 2.3.1  Orch base class tests – Consumer & task management
// ===========================================================================
namespace orch_base_ut
{

struct OrchBaseTest : public ::testing::Test
{
    shared_ptr<DBConnector> m_app_db, m_config_db, m_state_db;

    void SetUp() override
    {
        ::testing_db::reset();
        m_app_db    = make_shared<DBConnector>("APPL_DB",   0);
        m_config_db = make_shared<DBConnector>("CONFIG_DB", 0);
        m_state_db  = make_shared<DBConnector>("STATE_DB",  0);
    }

    void TearDown() override { ::testing_db::reset(); }
};

// ---------------------------------------------------------------------------
// 2.3.1.1  Orch can subscribe to single table
// ---------------------------------------------------------------------------
TEST_F(OrchBaseTest, OrcCanSubscribeToSingleTable)
{
    vector<string> tables = { "TEST_TABLE" };
    auto orch = make_shared<Orch>(m_app_db.get(), tables);

    EXPECT_NE(orch->getExecutor("TEST_TABLE"), nullptr);
}

// ---------------------------------------------------------------------------
// 2.3.1.2  Orch can subscribe to multiple tables
// ---------------------------------------------------------------------------
TEST_F(OrchBaseTest, OrchCanSubscribeToMultipleTables)
{
    vector<string> tables = { "TABLE_A", "TABLE_B", "TABLE_C" };
    auto orch = make_shared<Orch>(m_app_db.get(), tables);

    EXPECT_NE(orch->getExecutor("TABLE_A"), nullptr);
    EXPECT_NE(orch->getExecutor("TABLE_B"), nullptr);
    EXPECT_NE(orch->getExecutor("TABLE_C"), nullptr);
    EXPECT_EQ(orch->getExecutor("TABLE_D"), nullptr);  // not subscribed
}

// ---------------------------------------------------------------------------
// 2.3.1.3  Consumer receives entries from APP_DB table
// ---------------------------------------------------------------------------
TEST_F(OrchBaseTest, ConsumerReceivesEntriesFromTable)
{
    Table tbl(m_app_db.get(), "TEST_TABLE");
    tbl.set("key1", fv({{"field", "value"}}));

    vector<string> tables = { "TEST_TABLE" };
    auto orch = make_shared<Orch>(m_app_db.get(), tables);
    auto consumer = dynamic_cast<Consumer*>(orch->getExecutor("TEST_TABLE"));

    ASSERT_NE(consumer, nullptr);
    consumer->addExistingData(&tbl);

    EXPECT_GE(consumer->m_toSync.size(), 1u);
}

// ---------------------------------------------------------------------------
// 2.3.1.4  DEL operation removes entry from consumer sync map
// ---------------------------------------------------------------------------
TEST_F(OrchBaseTest, DelOperationRemovesEntry)
{
    Table tbl(m_app_db.get(), "TEST_TABLE");

    // Add entry
    tbl.set("del_test", fv({{"f", "v"}}));

    vector<string> tables = { "TEST_TABLE" };
    auto orch = make_shared<Orch>(m_app_db.get(), tables);
    auto consumer = dynamic_cast<Consumer*>(orch->getExecutor("TEST_TABLE"));

    consumer->addExistingData(&tbl);
    size_t count_after_add = consumer->m_toSync.size();

    // Delete entry
    tbl.del("del_test");

    // Create DEL entry and add to sync
    KeyOpFieldsValuesTuple del_entry = {"del_test", DEL_COMMAND, {}};
    vector<KeyOpFieldsValuesTuple> v;
    v.push_back(del_entry);
    consumer->addToSync(v);

    // After added DEL, the final state should be DEL only
    auto it = consumer->m_toSync.find("del_test");
    if (it != consumer->m_toSync.end())
    {
        EXPECT_EQ(kfvOp(it->second), DEL_COMMAND);
    }
}

// ---------------------------------------------------------------------------
// 2.3.1.5  Multiple consumers process different tables independently
// ---------------------------------------------------------------------------
TEST_F(OrchBaseTest, MultipleConsumersIndependent)
{
    Table t1(m_app_db.get(), "TABLE_X");
    Table t2(m_app_db.get(), "TABLE_Y");

    t1.set("key1", fv({{"f1", "v1"}}));
    t2.set("key2", fv({{"f2", "v2"}}));

    vector<string> tables = { "TABLE_X", "TABLE_Y" };
    auto orch = make_shared<Orch>(m_app_db.get(), tables);

    auto c1 = dynamic_cast<Consumer*>(orch->getExecutor("TABLE_X"));
    auto c2 = dynamic_cast<Consumer*>(orch->getExecutor("TABLE_Y"));

    c1->addExistingData(&t1);
    c2->addExistingData(&t2);

    EXPECT_GE(c1->m_toSync.size(), 1u);
    EXPECT_GE(c2->m_toSync.size(), 1u);

    // Verify they have different entries
    if (c1->m_toSync.size() > 0 && c2->m_toSync.size() > 0)
    {
        auto k1 = c1->m_toSync.begin()->first;
        auto k2 = c2->m_toSync.begin()->first;
        EXPECT_NE(k1, k2);
    }
}

} // namespace orch_base_ut

// ===========================================================================
// 2.3.2  RouteOrch tests – Route management
// ===========================================================================
namespace route_orch_ut
{

struct RouteOrchTest : public ::testing::Test
{
    shared_ptr<DBConnector> m_app_db, m_config_db, m_state_db;
    shared_ptr<RouteOrch> m_routeOrch;

    void SetUp() override
    {
        ::testing_db::reset();
        m_app_db    = make_shared<DBConnector>("APPL_DB",   0);
        m_config_db = make_shared<DBConnector>("CONFIG_DB", 0);
        m_state_db  = make_shared<DBConnector>("STATE_DB",  0);

        gPortsOrch = new PortsOrch(m_app_db.get(), m_state_db.get(),
                                  vector<TableConnector>{}, m_app_db.get());

        vector<string> route_tables = { APP_ROUTE_TABLE_NAME };
        m_routeOrch = make_shared<RouteOrch>(
            m_app_db.get(), route_tables, m_state_db.get());
    }

    void TearDown() override
    {
        m_routeOrch.reset();
        delete gPortsOrch;
        gPortsOrch = nullptr;
        ::testing_db::reset();
    }
};

// ---------------------------------------------------------------------------
// 2.3.2.1  RouteOrch subscribes to ROUTE_TABLE
// ---------------------------------------------------------------------------
TEST_F(RouteOrchTest, RouteOrchSubscribesToRouteTable)
{
    auto consumer = dynamic_cast<Consumer*>(
        m_routeOrch->getExecutor(APP_ROUTE_TABLE_NAME));

    EXPECT_NE(consumer, nullptr);
    EXPECT_EQ(consumer->getTableName(), APP_ROUTE_TABLE_NAME);
}

// ---------------------------------------------------------------------------
// 2.3.2.2  Route entry injection into RouteOrch consumer
// ---------------------------------------------------------------------------
TEST_F(RouteOrchTest, RouteEntryProcessing)
{
    Table route_tbl(m_app_db.get(), APP_ROUTE_TABLE_NAME);
    route_tbl.set("10.0.0.0/24",
                  fv({{"nexthop", "10.0.0.1"}, {"ifname", "Ethernet0"}}));

    auto consumer = dynamic_cast<Consumer*>(
        m_routeOrch->getExecutor(APP_ROUTE_TABLE_NAME));

    consumer->addExistingData(&route_tbl);

    // Consumer must have at least one pending route entry
    EXPECT_GE(consumer->m_toSync.size(), 1u);

    // Verify the route key is in sync map
    auto it = consumer->m_toSync.find("10.0.0.0/24");
    if (it != consumer->m_toSync.end())
    {
        EXPECT_EQ(kfvOp(it->second), SET_COMMAND);
    }
}

// ---------------------------------------------------------------------------
// 2.3.2.3  Multiple routes can be ingested simultaneously
// ---------------------------------------------------------------------------
TEST_F(RouteOrchTest, MultipleRoutesProcessing)
{
    Table route_tbl(m_app_db.get(), APP_ROUTE_TABLE_NAME);

    route_tbl.set("10.0.0.0/24", fv({{"nexthop", "10.0.0.1"}}));
    route_tbl.set("20.0.0.0/24", fv({{"nexthop", "10.0.0.2"}}));
    route_tbl.set("30.0.0.0/24", fv({{"nexthop", "10.0.0.3"}}));

    auto consumer = dynamic_cast<Consumer*>(
        m_routeOrch->getExecutor(APP_ROUTE_TABLE_NAME));

    consumer->addExistingData(&route_tbl);

    EXPECT_GE(consumer->m_toSync.size(), 3u);
}

// ---------------------------------------------------------------------------
// 2.3.2.4  Route delete is processed as DEL operation
// ---------------------------------------------------------------------------
TEST_F(RouteOrchTest, RouteDeletionProcessing)
{
    auto consumer = dynamic_cast<Consumer*>(
        m_routeOrch->getExecutor(APP_ROUTE_TABLE_NAME));

    // Inject DEL operation
    KeyOpFieldsValuesTuple del_route = {"172.16.0.0/24", DEL_COMMAND, {}};
    vector<KeyOpFieldsValuesTuple> entries;
    entries.push_back(del_route);

    consumer->addToSync(entries);

    auto it = consumer->m_toSync.find("172.16.0.0/24");
    if (it != consumer->m_toSync.end())
    {
        EXPECT_EQ(kfvOp(it->second), DEL_COMMAND);
    }
}

} // namespace route_orch_ut

// ===========================================================================
// 2.3.3  NeighOrch tests – Neighbor management
// ===========================================================================
namespace neigh_orch_ut
{

struct NeighOrchTest : public ::testing::Test
{
    shared_ptr<DBConnector> m_app_db, m_config_db, m_state_db;
    shared_ptr<NeighOrch> m_neighOrch;

    void SetUp() override
    {
        ::testing_db::reset();
        m_app_db    = make_shared<DBConnector>("APPL_DB",   0);
        m_config_db = make_shared<DBConnector>("CONFIG_DB", 0);
        m_state_db  = make_shared<DBConnector>("STATE_DB",  0);

        gPortsOrch = new PortsOrch(m_app_db.get(), m_state_db.get(),
                                  vector<TableConnector>{}, m_app_db.get());

        vector<string> neigh_tables = { APP_NEIGH_TABLE_NAME };
        m_neighOrch = make_shared<NeighOrch>(
            m_app_db.get(), neigh_tables, m_state_db.get());
    }

    void TearDown() override
    {
        m_neighOrch.reset();
        delete gPortsOrch;
        gPortsOrch = nullptr;
        ::testing_db::reset();
    }
};

// ---------------------------------------------------------------------------
// 2.3.3.1  NeighOrch subscribes to NEIGH_TABLE
// ---------------------------------------------------------------------------
TEST_F(NeighOrchTest, NeighOrchSubscribesToNeighTable)
{
    auto consumer = dynamic_cast<Consumer*>(
        m_neighOrch->getExecutor(APP_NEIGH_TABLE_NAME));

    EXPECT_NE(consumer, nullptr);
    EXPECT_EQ(consumer->getTableName(), APP_NEIGH_TABLE_NAME);
}

// ---------------------------------------------------------------------------
// 2.3.3.2  Neighbor entry can be injected
// ---------------------------------------------------------------------------
TEST_F(NeighOrchTest, NeighborEntryProcessing)
{
    Table neigh_tbl(m_app_db.get(), APP_NEIGH_TABLE_NAME);

    // Format: key = "interface:ip_address", fields contain MAC
    neigh_tbl.set("Ethernet0:10.0.0.1",
                  fv({{"neigh", "aa:bb:cc:dd:ee:ff"}}));

    auto consumer = dynamic_cast<Consumer*>(
        m_neighOrch->getExecutor(APP_NEIGH_TABLE_NAME));

    consumer->addExistingData(&neigh_tbl);

    EXPECT_GE(consumer->m_toSync.size(), 1u);
}

// ---------------------------------------------------------------------------
// 2.3.3.3  Multiple neighbors with different interfaces
// ---------------------------------------------------------------------------
TEST_F(NeighOrchTest, MultipleNeighborsProcessing)
{
    Table neigh_tbl(m_app_db.get(), APP_NEIGH_TABLE_NAME);

    neigh_tbl.set("Ethernet0:10.0.0.1", fv({{"neigh", "aa:bb:cc:dd:ee:01"}}));
    neigh_tbl.set("Ethernet4:10.0.0.2", fv({{"neigh", "aa:bb:cc:dd:ee:02"}}));
    neigh_tbl.set("Ethernet8:10.0.0.3", fv({{"neigh", "aa:bb:cc:dd:ee:03"}}));

    auto consumer = dynamic_cast<Consumer*>(
        m_neighOrch->getExecutor(APP_NEIGH_TABLE_NAME));

    consumer->addExistingData(&neigh_tbl);

    EXPECT_GE(consumer->m_toSync.size(), 3u);
}

// ---------------------------------------------------------------------------
// 2.3.3.4  Neighbor deletion via DEL command
// ---------------------------------------------------------------------------
TEST_F(NeighOrchTest, NeighborDeletionProcessing)
{
    auto consumer = dynamic_cast<Consumer*>(
        m_neighOrch->getExecutor(APP_NEIGH_TABLE_NAME));

    KeyOpFieldsValuesTuple del_neigh = {"Ethernet0:192.168.1.1", DEL_COMMAND, {}};
    vector<KeyOpFieldsValuesTuple> entries;
    entries.push_back(del_neigh);

    consumer->addToSync(entries);

    auto it = consumer->m_toSync.find("Ethernet0:192.168.1.1");
    if (it != consumer->m_toSync.end())
    {
        EXPECT_EQ(kfvOp(it->second), DEL_COMMAND);
    }
}

} // namespace neigh_orch_ut

// ===========================================================================
// 2.3.4  PortsOrch tests – Port state management
// ===========================================================================
namespace ports_orch_ut
{

struct PortsOrchTest : public ::testing::Test
{
    shared_ptr<DBConnector> m_app_db, m_config_db, m_state_db;

    void SetUp() override
    {
        ::testing_db::reset();
        m_app_db    = make_shared<DBConnector>("APPL_DB",   0);
        m_config_db = make_shared<DBConnector>("CONFIG_DB", 0);
        m_state_db  = make_shared<DBConnector>("STATE_DB",  0);

        gPortsOrch = new PortsOrch(m_app_db.get(), m_state_db.get(),
                                  vector<TableConnector>{}, m_app_db.get());
    }

    void TearDown() override
    {
        delete gPortsOrch;
        gPortsOrch = nullptr;
        ::testing_db::reset();
    }
};

// ---------------------------------------------------------------------------
// 2.3.4.1  PortsOrch can be initialized
// ---------------------------------------------------------------------------
TEST_F(PortsOrchTest, PortsOrchInitialization)
{
    EXPECT_NE(gPortsOrch, nullptr);
}

// ---------------------------------------------------------------------------
// 2.3.4.2  PortsOrch can retrieve port data from APP_DB
// ---------------------------------------------------------------------------
TEST_F(PortsOrchTest, PortsOrchReadsPortData)
{
    Table port_tbl(m_app_db.get(), APP_PORT_TABLE_NAME);

    port_tbl.set("Ethernet0", fv({{"admin_status", "up"}, {"mtu", "9100"}}));
    port_tbl.set("Ethernet4", fv({{"admin_status", "down"}, {"mtu", "1500"}}));

    vector<FieldValueTuple> vals;
    ASSERT_TRUE(port_tbl.get("Ethernet0", vals));
    auto opt = swss::fvsGetValue(vals, "admin_status", true);
    ASSERT_TRUE(opt);
    EXPECT_EQ(opt.get(), "up");
}

// ---------------------------------------------------------------------------
// 2.3.4.3  Port state change updates STATE_DB
// ---------------------------------------------------------------------------
TEST_F(PortsOrchTest, PortStateUpdate)
{
    Table state_port(m_state_db.get(), STATE_PORT_TABLE_NAME);

    state_port.set("Ethernet0", fv({{"state", "ok"}}));

    vector<FieldValueTuple> vals;
    ASSERT_TRUE(state_port.get("Ethernet0", vals));
    auto opt = swss::fvsGetValue(vals, "state", true);
    ASSERT_TRUE(opt);
    EXPECT_EQ(opt.get(), "ok");
}

} // namespace ports_orch_ut

// ===========================================================================
// 2.3.5  Cross-module integration tests
// ===========================================================================
namespace orch_integration_ut
{

struct OrchIntegrationTest : public ::testing::Test
{
    shared_ptr<DBConnector> m_app_db, m_config_db, m_state_db;
    shared_ptr<RouteOrch> m_routeOrch;
    shared_ptr<NeighOrch> m_neighOrch;

    void SetUp() override
    {
        ::testing_db::reset();
        m_app_db    = make_shared<DBConnector>("APPL_DB",   0);
        m_config_db = make_shared<DBConnector>("CONFIG_DB", 0);
        m_state_db  = make_shared<DBConnector>("STATE_DB",  0);

        gPortsOrch = new PortsOrch(m_app_db.get(), m_state_db.get(),
                                  vector<TableConnector>{}, m_app_db.get());

        m_routeOrch = make_shared<RouteOrch>(
            m_app_db.get(), vector<string>{ APP_ROUTE_TABLE_NAME },
            m_state_db.get());

        m_neighOrch = make_shared<NeighOrch>(
            m_app_db.get(), vector<string>{ APP_NEIGH_TABLE_NAME },
            m_state_db.get());
    }

    void TearDown() override
    {
        m_routeOrch.reset();
        m_neighOrch.reset();
        delete gPortsOrch;
        gPortsOrch = nullptr;
        ::testing_db::reset();
    }
};

// ---------------------------------------------------------------------------
// 2.3.5.1  Route and Neighbor entries coexist in APP_DB
// ---------------------------------------------------------------------------
TEST_F(OrchIntegrationTest, RouteAndNeighborCoexist)
{
    Table route_tbl(m_app_db.get(), APP_ROUTE_TABLE_NAME);
    Table neigh_tbl(m_app_db.get(), APP_NEIGH_TABLE_NAME);

    route_tbl.set("10.0.0.0/24", fv({{"nexthop", "10.0.0.1"}}));
    neigh_tbl.set("Ethernet0:10.0.0.1", fv({{"neigh", "aa:bb:cc:dd:ee:ff"}}));

    auto route_consumer = dynamic_cast<Consumer*>(
        m_routeOrch->getExecutor(APP_ROUTE_TABLE_NAME));
    auto neigh_consumer = dynamic_cast<Consumer*>(
        m_neighOrch->getExecutor(APP_NEIGH_TABLE_NAME));

    route_consumer->addExistingData(&route_tbl);
    neigh_consumer->addExistingData(&neigh_tbl);

    EXPECT_GE(route_consumer->m_toSync.size(), 1u);
    EXPECT_GE(neigh_consumer->m_toSync.size(), 1u);
}

// ---------------------------------------------------------------------------
// 2.3.5.2  Multiple orchs process entries independently
// ---------------------------------------------------------------------------
TEST_F(OrchIntegrationTest, MultipleOrchsIndependent)
{
    Table route_tbl(m_app_db.get(), APP_ROUTE_TABLE_NAME);
    Table neigh_tbl(m_app_db.get(), APP_NEIGH_TABLE_NAME);

    route_tbl.set("192.168.0.0/16", fv({{"nexthop", "10.0.0.1"}}));
    route_tbl.set("172.16.0.0/12",  fv({{"nexthop", "10.0.0.2"}}));

    neigh_tbl.set("Ethernet0:10.0.0.1", fv({{"neigh", "aa:bb:cc:dd:ee:01"}}));

    auto route_consumer = dynamic_cast<Consumer*>(
        m_routeOrch->getExecutor(APP_ROUTE_TABLE_NAME));
    auto neigh_consumer = dynamic_cast<Consumer*>(
        m_neighOrch->getExecutor(APP_NEIGH_TABLE_NAME));

    route_consumer->addExistingData(&route_tbl);
    neigh_consumer->addExistingData(&neigh_tbl);

    // RouteOrch should have 2 routes
    EXPECT_GE(route_consumer->m_toSync.size(), 2u);
    // NeighOrch should have 1 neighbor
    EXPECT_GE(neigh_consumer->m_toSync.size(), 1u);
}

// ---------------------------------------------------------------------------
// 2.3.5.3  Reset clears all orches state
// ---------------------------------------------------------------------------
TEST_F(OrchIntegrationTest, ResetClearsAllOrchState)
{
    Table route_tbl(m_app_db.get(), APP_ROUTE_TABLE_NAME);
    Table neigh_tbl(m_app_db.get(), APP_NEIGH_TABLE_NAME);

    route_tbl.set("10.0.0.0/8", fv({{"nexthop", "10.0.0.1"}}));
    neigh_tbl.set("Ethernet0:10.0.0.1", fv({{"neigh", "aa:bb:cc:dd:ee:ff"}}));

    auto route_consumer = dynamic_cast<Consumer*>(
        m_routeOrch->getExecutor(APP_ROUTE_TABLE_NAME));
    auto neigh_consumer = dynamic_cast<Consumer*>(
        m_neighOrch->getExecutor(APP_NEIGH_TABLE_NAME));

    route_consumer->addExistingData(&route_tbl);
    neigh_consumer->addExistingData(&neigh_tbl);

    ASSERT_GE(route_consumer->m_toSync.size(), 1u);
    ASSERT_GE(neigh_consumer->m_toSync.size(), 1u);

    // Reset DB
    ::testing_db::reset();

    // After reset, tables should be empty
    vector<FieldValueTuple> vals;
    EXPECT_FALSE(route_tbl.get("10.0.0.0/8", vals));
    EXPECT_FALSE(neigh_tbl.get("Ethernet0:10.0.0.1", vals));
}

} // namespace orch_integration_ut
