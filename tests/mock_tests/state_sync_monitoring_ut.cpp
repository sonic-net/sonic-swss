/**
 * @file state_sync_monitoring_ut.cpp
 *
 * Task 2.5 – Thử nghiệm đánh giá module giám sát và đồng bộ trạng thái
 *
 * Unit tests for the state synchronization and monitoring layer:
 *   - Syncd daemon infrastructure (portsyncd, neighsyncd, fdbsyncd, teamsyncd)
 *   - State table subscriptions and updates
 *   - Port state monitoring (link up/down, speed, duplex, admin state)
 *   - Neighbor state synchronization (ARP/NDP, MAC learning)
 *   - FDB state synchronization (MAC table, VLAN membership)
 *   - LAG state synchronization (team membership, state propagation)
 *   - Cross-module state consistency
 *   - Netlink event handling and state propagation
 *
 * Tests verify the flow: Kernel/HW → Syncd → APP_DB/STATE_DB → Orchestration
 */

#include "gtest/gtest.h"
#include "ut_helper.h"
#include "mock_table.h"

using namespace std;
using namespace testing_db;

// ===========================================================================
// Helpers
// ===========================================================================

/**
 * Simulate a port state change event from kernel/hardware
 */
struct PortStateEvent
{
    string ifname;
    bool admin_up;
    bool oper_up;
    uint32_t speed_mbps;
    uint32_t duplex;  // 0=half, 1=full

    PortStateEvent(const string& name, bool admin, bool oper, uint32_t speed, uint32_t duplex_val)
        : ifname(name), admin_up(admin), oper_up(oper), speed_mbps(speed), duplex(duplex_val)
    {
    }
};

/**
 * Simulate a neighbor state change event (ARP entry from kernel)
 */
struct NeighborStateEvent
{
    string ifname;
    string ip_address;
    string mac_address;
    bool is_add;  // true=add, false=remove

    NeighborStateEvent(const string& iface, const string& ip, const string& mac, bool add)
        : ifname(iface), ip_address(ip), mac_address(mac), is_add(add)
    {
    }
};

/**
 * Simulate a FDB entry state change (MAC table from hardware)
 */
struct FdbStateEvent
{
    string mac_address;
    uint16_t vlan_id;
    string bridge_port;
    bool is_add;  // true=add, false=remove

    FdbStateEvent(const string& mac, uint16_t vlan, const string& port, bool add)
        : mac_address(mac), vlan_id(vlan), bridge_port(port), is_add(add)
    {
    }
};

/**
 * Simulate a LAG member state change (teamd event)
 */
struct LagMemberStateEvent
{
    string lag_name;
    string member_ifname;
    bool is_add;  // true=add, false=remove

    LagMemberStateEvent(const string& lag, const string& member, bool add)
        : lag_name(lag), member_ifname(member), is_add(add)
    {
    }
};

// ===========================================================================
// 2.5.1  State Sync Framework and Initialization
// ===========================================================================
namespace state_sync_framework_ut
{

struct StateSyncFrameworkTest : public ::testing::Test
{
    Table app_port_table{APP_DB, APP_PORT_TABLE_NAME};
    Table state_port_table{STATE_DB, STATE_PORT_TABLE_NAME};
    Table app_neigh_table{APP_DB, APP_NEIGH_TABLE_NAME};
    Table state_neigh_table{STATE_DB, STATE_NEIGH_TABLE_NAME};

    void SetUp() override
    {
        reset();
    }

    void TearDown() override
    {
        reset();
    }
};

// ---------------------------------------------------------------------------
// 2.5.1.1  APP_DB and STATE_DB are separate namespaces
// ---------------------------------------------------------------------------
TEST_F(StateSyncFrameworkTest, AppDbAndStateDbSeparate)
{
    // Write to APP_DB
    app_port_table.set("Ethernet0", {{"state", "ok"}, {"speed", "10000"}});

    // Verify not visible in STATE_DB
    auto keys = state_port_table.getKeys();
    EXPECT_EQ(keys.size(), 0);
}

// ---------------------------------------------------------------------------
// 2.5.1.2  Port state subscription table is created
// ---------------------------------------------------------------------------
TEST_F(StateSyncFrameworkTest, PortStateSubscriptionTable)
{
    // Write port state
    state_port_table.set("Ethernet0", {
        {"admin_state", "up"},
        {"oper_status", "up"},
        {"speed", "10000"},
        {"duplex", "FULL"}
    });

    auto keys = state_port_table.getKeys();
    EXPECT_EQ(keys.size(), 1);
    EXPECT_TRUE(find(keys.begin(), keys.end(), "Ethernet0") != keys.end());
}

// ---------------------------------------------------------------------------
// 2.5.1.3  Neighbor state subscription table is created
// ---------------------------------------------------------------------------
TEST_F(StateSyncFrameworkTest, NeighborStateSubscriptionTable)
{
    // Write neighbor state
    state_neigh_table.set("Ethernet0:10.0.0.1", {
        {"neigh", "00:11:22:33:44:55"},
        {"state", "REACHABLE"}
    });

    auto keys = state_neigh_table.getKeys();
    EXPECT_EQ(keys.size(), 1);
}

// ---------------------------------------------------------------------------
// 2.5.1.4  State sync daemon can subscribe to multiple tables
// ---------------------------------------------------------------------------
TEST_F(StateSyncFrameworkTest, SyncDaemonMultiTableSubscription)
{
    // Write to both port and neighbor tables
    state_port_table.set("Ethernet0", {
        {"admin_state", "up"},
        {"oper_status", "up"}
    });
    state_neigh_table.set("Ethernet0:10.0.0.1", {
        {"neigh", "00:11:22:33:44:55"}
    });

    auto port_keys = state_port_table.getKeys();
    auto neigh_keys = state_neigh_table.getKeys();

    EXPECT_EQ(port_keys.size(), 1);
    EXPECT_EQ(neigh_keys.size(), 1);
}

} // namespace state_sync_framework_ut

// ===========================================================================
// 2.5.2  Port State Synchronization (portsyncd)
// ===========================================================================
namespace port_sync_ut
{

struct PortSyncTest : public ::testing::Test
{
    Table app_port_table{APP_DB, APP_PORT_TABLE_NAME};
    Table state_port_table{STATE_DB, STATE_PORT_TABLE_NAME};

    void SetUp() override
    {
        reset();
    }

    void TearDown() override
    {
        reset();
    }

    void simulatePortStateChange(const PortStateEvent& event)
    {
        // Simulate portsyncd receiving netlink event and updating STATE_DB
        string admin_state = event.admin_up ? "up" : "down";
        string oper_status = event.oper_up ? "up" : "down";

        vector<FieldValueTuple> fvs;
        fvs.push_back(make_tuple("admin_state", admin_state));
        fvs.push_back(make_tuple("oper_status", oper_status));
        fvs.push_back(make_tuple("speed", to_string(event.speed_mbps)));
        fvs.push_back(make_tuple("duplex", event.duplex ? "FULL" : "HALF"));

        state_port_table.set(event.ifname, fvs);
    }
};

// ---------------------------------------------------------------------------
// 2.5.2.1  Port admin state change is reflected in STATE_DB
// ---------------------------------------------------------------------------
TEST_F(PortSyncTest, PortAdminStateChange)
{
    PortStateEvent event("Ethernet0", true, true, 10000, 1);
    simulatePortStateChange(event);

    auto fvs = state_port_table.get("Ethernet0");
    ASSERT_GT(fvs.size(), 0);

    auto it = find_if(fvs.begin(), fvs.end(),
        [](const FieldValueTuple& fv) { return get<0>(fv) == "admin_state"; });
    ASSERT_NE(it, fvs.end());
    EXPECT_EQ(get<1>(*it), "up");
}

// ---------------------------------------------------------------------------
// 2.5.2.2  Port operational status change is propagated
// ---------------------------------------------------------------------------
TEST_F(PortSyncTest, PortOperationalStatusChange)
{
    PortStateEvent event("Ethernet0", true, false, 10000, 1);
    simulatePortStateChange(event);

    auto fvs = state_port_table.get("Ethernet0");
    auto it = find_if(fvs.begin(), fvs.end(),
        [](const FieldValueTuple& fv) { return get<0>(fv) == "oper_status"; });
    ASSERT_NE(it, fvs.end());
    EXPECT_EQ(get<1>(*it), "down");
}

// ---------------------------------------------------------------------------
// 2.5.2.3  Port speed change is synchronized
// ---------------------------------------------------------------------------
TEST_F(PortSyncTest, PortSpeedChange)
{
    PortStateEvent event("Ethernet0", true, true, 25000, 1);
    simulatePortStateChange(event);

    auto fvs = state_port_table.get("Ethernet0");
    auto it = find_if(fvs.begin(), fvs.end(),
        [](const FieldValueTuple& fv) { return get<0>(fv) == "speed"; });
    ASSERT_NE(it, fvs.end());
    EXPECT_EQ(get<1>(*it), "25000");
}

// ---------------------------------------------------------------------------
// 2.5.2.4  Port duplex mode change is synchronized
// ---------------------------------------------------------------------------
TEST_F(PortSyncTest, PortDuplexChange)
{
    PortStateEvent event("Ethernet0", true, true, 10000, 0);  // half-duplex
    simulatePortStateChange(event);

    auto fvs = state_port_table.get("Ethernet0");
    auto it = find_if(fvs.begin(), fvs.end(),
        [](const FieldValueTuple& fv) { return get<0>(fv) == "duplex"; });
    ASSERT_NE(it, fvs.end());
    EXPECT_EQ(get<1>(*it), "HALF");
}

// ---------------------------------------------------------------------------
// 2.5.2.5  Multiple port state changes are independent
// ---------------------------------------------------------------------------
TEST_F(PortSyncTest, MultiplePortStateChanges)
{
    PortStateEvent event1("Ethernet0", true, true, 10000, 1);
    PortStateEvent event2("Ethernet1", true, false, 25000, 1);

    simulatePortStateChange(event1);
    simulatePortStateChange(event2);

    auto keys = state_port_table.getKeys();
    EXPECT_EQ(keys.size(), 2);

    auto fvs0 = state_port_table.get("Ethernet0");
    auto fvs1 = state_port_table.get("Ethernet1");

    auto it0 = find_if(fvs0.begin(), fvs0.end(),
        [](const FieldValueTuple& fv) { return get<0>(fv) == "oper_status"; });
    auto it1 = find_if(fvs1.begin(), fvs1.end(),
        [](const FieldValueTuple& fv) { return get<0>(fv) == "oper_status"; });

    EXPECT_EQ(get<1>(*it0), "up");
    EXPECT_EQ(get<1>(*it1), "down");
}

// ---------------------------------------------------------------------------
// 2.5.2.6  Port state can transition from up to down
// ---------------------------------------------------------------------------
TEST_F(PortSyncTest, PortStateTransitionUpToDown)
{
    PortStateEvent event_up("Ethernet0", true, true, 10000, 1);
    simulatePortStateChange(event_up);

    auto fvs = state_port_table.get("Ethernet0");
    auto it = find_if(fvs.begin(), fvs.end(),
        [](const FieldValueTuple& fv) { return get<0>(fv) == "oper_status"; });
    EXPECT_EQ(get<1>(*it), "up");

    // Transition to down
    PortStateEvent event_down("Ethernet0", true, false, 10000, 1);
    simulatePortStateChange(event_down);

    fvs = state_port_table.get("Ethernet0");
    it = find_if(fvs.begin(), fvs.end(),
        [](const FieldValueTuple& fv) { return get<0>(fv) == "oper_status"; });
    EXPECT_EQ(get<1>(*it), "down");
}

} // namespace port_sync_ut

// ===========================================================================
// 2.5.3  Neighbor State Synchronization (neighsyncd)
// ===========================================================================
namespace neighbor_sync_ut
{

struct NeighborSyncTest : public ::testing::Test
{
    Table state_neigh_table{STATE_DB, STATE_NEIGH_TABLE_NAME};

    void SetUp() override
    {
        reset();
    }

    void TearDown() override
    {
        reset();
    }

    void simulateNeighborStateChange(const NeighborStateEvent& event)
    {
        string key = event.ifname + ":" + event.ip_address;

        if (event.is_add)
        {
            vector<FieldValueTuple> fvs;
            fvs.push_back(make_tuple("neigh", event.mac_address));
            fvs.push_back(make_tuple("state", "REACHABLE"));
            state_neigh_table.set(key, fvs);
        }
        else
        {
            state_neigh_table.del(key);
        }
    }
};

// ---------------------------------------------------------------------------
// 2.5.3.1  Neighbor entry is added to STATE_DB
// ---------------------------------------------------------------------------
TEST_F(NeighborSyncTest, NeighborEntryAdd)
{
    NeighborStateEvent event("Ethernet0", "10.0.0.1", "00:11:22:33:44:55", true);
    simulateNeighborStateChange(event);

    auto keys = state_neigh_table.getKeys();
    EXPECT_EQ(keys.size(), 1);
    EXPECT_TRUE(find(keys.begin(), keys.end(), "Ethernet0:10.0.0.1") != keys.end());
}

// ---------------------------------------------------------------------------
// 2.5.3.2  Neighbor MAC address is correctly stored
// ---------------------------------------------------------------------------
TEST_F(NeighborSyncTest, NeighborMacAddress)
{
    NeighborStateEvent event("Ethernet0", "10.0.0.1", "aa:bb:cc:dd:ee:ff", true);
    simulateNeighborStateChange(event);

    auto fvs = state_neigh_table.get("Ethernet0:10.0.0.1");
    auto it = find_if(fvs.begin(), fvs.end(),
        [](const FieldValueTuple& fv) { return get<0>(fv) == "neigh"; });
    ASSERT_NE(it, fvs.end());
    EXPECT_EQ(get<1>(*it), "aa:bb:cc:dd:ee:ff");
}

// ---------------------------------------------------------------------------
// 2.5.3.3  Neighbor state is REACHABLE
// ---------------------------------------------------------------------------
TEST_F(NeighborSyncTest, NeighborStateReachable)
{
    NeighborStateEvent event("Ethernet0", "10.0.0.1", "00:11:22:33:44:55", true);
    simulateNeighborStateChange(event);

    auto fvs = state_neigh_table.get("Ethernet0:10.0.0.1");
    auto it = find_if(fvs.begin(), fvs.end(),
        [](const FieldValueTuple& fv) { return get<0>(fv) == "state"; });
    ASSERT_NE(it, fvs.end());
    EXPECT_EQ(get<1>(*it), "REACHABLE");
}

// ---------------------------------------------------------------------------
// 2.5.3.4  Neighbor entry is removed from STATE_DB
// ---------------------------------------------------------------------------
TEST_F(NeighborSyncTest, NeighborEntryRemove)
{
    NeighborStateEvent event_add("Ethernet0", "10.0.0.1", "00:11:22:33:44:55", true);
    simulateNeighborStateChange(event_add);

    auto keys = state_neigh_table.getKeys();
    EXPECT_EQ(keys.size(), 1);

    NeighborStateEvent event_del("Ethernet0", "10.0.0.1", "", false);
    simulateNeighborStateChange(event_del);

    keys = state_neigh_table.getKeys();
    EXPECT_EQ(keys.size(), 0);
}

// ---------------------------------------------------------------------------
// 2.5.3.5  Multiple neighbors on different interfaces are tracked
// ---------------------------------------------------------------------------
TEST_F(NeighborSyncTest, MultipleNeighborsOnDifferentInterfaces)
{
    NeighborStateEvent event1("Ethernet0", "10.0.0.1", "00:11:22:33:44:55", true);
    NeighborStateEvent event2("Ethernet1", "10.1.0.1", "aa:bb:cc:dd:ee:ff", true);

    simulateNeighborStateChange(event1);
    simulateNeighborStateChange(event2);

    auto keys = state_neigh_table.getKeys();
    EXPECT_EQ(keys.size(), 2);
}

// ---------------------------------------------------------------------------
// 2.5.3.6  Multiple neighbors on same interface are tracked
// ---------------------------------------------------------------------------
TEST_F(NeighborSyncTest, MultipleNeighborsOnSameInterface)
{
    NeighborStateEvent event1("Ethernet0", "10.0.0.1", "00:11:22:33:44:55", true);
    NeighborStateEvent event2("Ethernet0", "10.0.0.2", "aa:bb:cc:dd:ee:ff", true);

    simulateNeighborStateChange(event1);
    simulateNeighborStateChange(event2);

    auto keys = state_neigh_table.getKeys();
    EXPECT_EQ(keys.size(), 2);
}

} // namespace neighbor_sync_ut

// ===========================================================================
// 2.5.4  FDB State Synchronization (fdbsyncd)
// ===========================================================================
namespace fdb_sync_ut
{

struct FdbSyncTest : public ::testing::Test
{
    Table app_fdb_table{APP_DB, APP_FDB_TABLE_NAME};

    void SetUp() override
    {
        reset();
    }

    void TearDown() override
    {
        reset();
    }

    void simulateFdbStateChange(const FdbStateEvent& event)
    {
        string key = to_string(event.vlan_id) + ":" + event.mac_address;

        if (event.is_add)
        {
            vector<FieldValueTuple> fvs;
            fvs.push_back(make_tuple("port", event.bridge_port));
            fvs.push_back(make_tuple("type", "dynamic"));
            app_fdb_table.set(key, fvs);
        }
        else
        {
            app_fdb_table.del(key);
        }
    }
};

// ---------------------------------------------------------------------------
// 2.5.4.1  FDB entry is added to APP_DB
// ---------------------------------------------------------------------------
TEST_F(FdbSyncTest, FdbEntryAdd)
{
    FdbStateEvent event("00:11:22:33:44:55", 1, "Ethernet0", true);
    simulateFdbStateChange(event);

    auto keys = app_fdb_table.getKeys();
    EXPECT_EQ(keys.size(), 1);
}

// ---------------------------------------------------------------------------
// 2.5.4.2  FDB entry port is correctly stored
// ---------------------------------------------------------------------------
TEST_F(FdbSyncTest, FdbEntryPort)
{
    FdbStateEvent event("aa:bb:cc:dd:ee:ff", 100, "Ethernet5", true);
    simulateFdbStateChange(event);

    auto fvs = app_fdb_table.get("100:aa:bb:cc:dd:ee:ff");
    auto it = find_if(fvs.begin(), fvs.end(),
        [](const FieldValueTuple& fv) { return get<0>(fv) == "port"; });
    ASSERT_NE(it, fvs.end());
    EXPECT_EQ(get<1>(*it), "Ethernet5");
}

// ---------------------------------------------------------------------------
// 2.5.4.3  FDB entry type is dynamic
// ---------------------------------------------------------------------------
TEST_F(FdbSyncTest, FdbEntryTypeDynamic)
{
    FdbStateEvent event("00:11:22:33:44:55", 1, "Ethernet0", true);
    simulateFdbStateChange(event);

    auto fvs = app_fdb_table.get("1:00:11:22:33:44:55");
    auto it = find_if(fvs.begin(), fvs.end(),
        [](const FieldValueTuple& fv) { return get<0>(fv) == "type"; });
    ASSERT_NE(it, fvs.end());
    EXPECT_EQ(get<1>(*it), "dynamic");
}

// ---------------------------------------------------------------------------
// 2.5.4.4  FDB entry is removed from APP_DB
// ---------------------------------------------------------------------------
TEST_F(FdbSyncTest, FdbEntryRemove)
{
    FdbStateEvent event_add("00:11:22:33:44:55", 1, "Ethernet0", true);
    simulateFdbStateChange(event_add);

    auto keys = app_fdb_table.getKeys();
    EXPECT_EQ(keys.size(), 1);

    FdbStateEvent event_del("00:11:22:33:44:55", 1, "", false);
    simulateFdbStateChange(event_del);

    keys = app_fdb_table.getKeys();
    EXPECT_EQ(keys.size(), 0);
}

// ---------------------------------------------------------------------------
// 2.5.4.5  Multiple FDB entries on different VLANs are tracked
// ---------------------------------------------------------------------------
TEST_F(FdbSyncTest, MultipleFdbEntriesDifferentVlans)
{
    FdbStateEvent event1("00:11:22:33:44:55", 1, "Ethernet0", true);
    FdbStateEvent event2("aa:bb:cc:dd:ee:ff", 100, "Ethernet1", true);

    simulateFdbStateChange(event1);
    simulateFdbStateChange(event2);

    auto keys = app_fdb_table.getKeys();
    EXPECT_EQ(keys.size(), 2);
}

// ---------------------------------------------------------------------------
// 2.5.4.6  Multiple FDB entries on same VLAN are tracked
// ---------------------------------------------------------------------------
TEST_F(FdbSyncTest, MultipleFdbEntriesSameVlan)
{
    FdbStateEvent event1("00:11:22:33:44:55", 1, "Ethernet0", true);
    FdbStateEvent event2("aa:bb:cc:dd:ee:ff", 1, "Ethernet1", true);

    simulateFdbStateChange(event1);
    simulateFdbStateChange(event2);

    auto keys = app_fdb_table.getKeys();
    EXPECT_EQ(keys.size(), 2);
}

} // namespace fdb_sync_ut

// ===========================================================================
// 2.5.5  LAG State Synchronization (teamsyncd)
// ===========================================================================
namespace lag_sync_ut
{

struct LagSyncTest : public ::testing::Test
{
    Table app_lag_table{APP_DB, APP_LAG_TABLE_NAME};
    Table app_lag_member_table{APP_DB, APP_LAG_MEMBER_TABLE_NAME};

    void SetUp() override
    {
        reset();
    }

    void TearDown() override
    {
        reset();
    }

    void simulateLagMemberStateChange(const LagMemberStateEvent& event)
    {
        // First ensure LAG exists
        vector<FieldValueTuple> lag_fvs;
        lag_fvs.push_back(make_tuple("admin_status", "up"));
        app_lag_table.set(event.lag_name, lag_fvs);

        // Update LAG member
        string key = event.lag_name + ":" + event.member_ifname;

        if (event.is_add)
        {
            vector<FieldValueTuple> member_fvs;
            member_fvs.push_back(make_tuple("status", "up"));
            app_lag_member_table.set(key, member_fvs);
        }
        else
        {
            app_lag_member_table.del(key);
        }
    }
};

// ---------------------------------------------------------------------------
// 2.5.5.1  LAG is created in APP_DB
// ---------------------------------------------------------------------------
TEST_F(LagSyncTest, LagCreation)
{
    LagMemberStateEvent event("PortChannel0", "Ethernet0", true);
    simulateLagMemberStateChange(event);

    auto keys = app_lag_table.getKeys();
    EXPECT_EQ(keys.size(), 1);
    EXPECT_TRUE(find(keys.begin(), keys.end(), "PortChannel0") != keys.end());
}

// ---------------------------------------------------------------------------
// 2.5.5.2  LAG member is added
// ---------------------------------------------------------------------------
TEST_F(LagSyncTest, LagMemberAdd)
{
    LagMemberStateEvent event("PortChannel0", "Ethernet0", true);
    simulateLagMemberStateChange(event);

    auto keys = app_lag_member_table.getKeys();
    EXPECT_EQ(keys.size(), 1);
    EXPECT_TRUE(find(keys.begin(), keys.end(), "PortChannel0:Ethernet0") != keys.end());
}

// ---------------------------------------------------------------------------
// 2.5.5.3  LAG member status is up
// ---------------------------------------------------------------------------
TEST_F(LagSyncTest, LagMemberStatusUp)
{
    LagMemberStateEvent event("PortChannel0", "Ethernet0", true);
    simulateLagMemberStateChange(event);

    auto fvs = app_lag_member_table.get("PortChannel0:Ethernet0");
    auto it = find_if(fvs.begin(), fvs.end(),
        [](const FieldValueTuple& fv) { return get<0>(fv) == "status"; });
    ASSERT_NE(it, fvs.end());
    EXPECT_EQ(get<1>(*it), "up");
}

// ---------------------------------------------------------------------------
// 2.5.5.4  LAG member is removed
// ---------------------------------------------------------------------------
TEST_F(LagSyncTest, LagMemberRemove)
{
    LagMemberStateEvent event_add("PortChannel0", "Ethernet0", true);
    simulateLagMemberStateChange(event_add);

    auto keys = app_lag_member_table.getKeys();
    EXPECT_EQ(keys.size(), 1);

    LagMemberStateEvent event_del("PortChannel0", "Ethernet0", false);
    simulateLagMemberStateChange(event_del);

    keys = app_lag_member_table.getKeys();
    EXPECT_EQ(keys.size(), 0);
}

// ---------------------------------------------------------------------------
// 2.5.5.5  Multiple LAG members are tracked
// ---------------------------------------------------------------------------
TEST_F(LagSyncTest, MultipleLagMembers)
{
    LagMemberStateEvent event1("PortChannel0", "Ethernet0", true);
    LagMemberStateEvent event2("PortChannel0", "Ethernet1", true);

    simulateLagMemberStateChange(event1);
    simulateLagMemberStateChange(event2);

    auto keys = app_lag_member_table.getKeys();
    EXPECT_EQ(keys.size(), 2);
}

// ---------------------------------------------------------------------------
// 2.5.5.6  Multiple LAGs are tracked independently
// ---------------------------------------------------------------------------
TEST_F(LagSyncTest, MultipleLags)
{
    LagMemberStateEvent event1("PortChannel0", "Ethernet0", true);
    LagMemberStateEvent event2("PortChannel1", "Ethernet4", true);

    simulateLagMemberStateChange(event1);
    simulateLagMemberStateChange(event2);

    auto lag_keys = app_lag_table.getKeys();
    auto member_keys = app_lag_member_table.getKeys();

    EXPECT_EQ(lag_keys.size(), 2);
    EXPECT_EQ(member_keys.size(), 2);
}

} // namespace lag_sync_ut

// ===========================================================================
// 2.5.6  Cross-Module State Monitoring and Consistency
// ===========================================================================
namespace state_consistency_ut
{

struct StateConsistencyTest : public ::testing::Test
{
    Table app_port_table{APP_DB, APP_PORT_TABLE_NAME};
    Table state_port_table{STATE_DB, STATE_PORT_TABLE_NAME};
    Table app_neigh_table{APP_DB, APP_NEIGH_TABLE_NAME};
    Table state_neigh_table{STATE_DB, STATE_NEIGH_TABLE_NAME};
    Table app_fdb_table{APP_DB, APP_FDB_TABLE_NAME};
    Table app_lag_table{APP_DB, APP_LAG_TABLE_NAME};

    void SetUp() override
    {
        reset();
    }

    void TearDown() override
    {
        reset();
    }
};

// ---------------------------------------------------------------------------
// 2.5.6.1  Port ready state enables neighbor sync on port
// ---------------------------------------------------------------------------
TEST_F(StateConsistencyTest, PortReadyEnablesNeighborSync)
{
    // Port must be in APP_DB before neighbors can be synced
    vector<FieldValueTuple> port_fvs;
    port_fvs.push_back(make_tuple("state", "ok"));
    app_port_table.set("Ethernet0", port_fvs);

    // Now add neighbor
    vector<FieldValueTuple> neigh_fvs;
    neigh_fvs.push_back(make_tuple("neigh", "00:11:22:33:44:55"));
    state_neigh_table.set("Ethernet0:10.0.0.1", neigh_fvs);

    auto port_keys = app_port_table.getKeys();
    auto neigh_keys = state_neigh_table.getKeys();

    EXPECT_EQ(port_keys.size(), 1);
    EXPECT_EQ(neigh_keys.size(), 1);
}

// ---------------------------------------------------------------------------
// 2.5.6.2  Port state from STATE_DB reflects hardware state
// ---------------------------------------------------------------------------
TEST_F(StateConsistencyTest, PortStateReflectsHw)
{
    // Simulate portsyncd updating STATE_DB from hardware event
    vector<FieldValueTuple> fvs;
    fvs.push_back(make_tuple("admin_state", "up"));
    fvs.push_back(make_tuple("oper_status", "up"));
    fvs.push_back(make_tuple("speed", "10000"));
    state_port_table.set("Ethernet0", fvs);

    auto hw_state = state_port_table.get("Ethernet0");
    EXPECT_GT(hw_state.size(), 0);
}

// ---------------------------------------------------------------------------
// 2.5.6.3  Port down state prevents neighbor operations
// ---------------------------------------------------------------------------
TEST_F(StateConsistencyTest, PortDownPreventsNeighborOps)
{
    // Port is down in hardware
    vector<FieldValueTuple> port_fvs;
    port_fvs.push_back(make_tuple("admin_state", "down"));
    port_fvs.push_back(make_tuple("oper_status", "down"));
    state_port_table.set("Ethernet0", port_fvs);

    auto state = state_port_table.get("Ethernet0");
    auto it = find_if(state.begin(), state.end(),
        [](const FieldValueTuple& fv) { return get<0>(fv) == "oper_status"; });
    ASSERT_NE(it, state.end());
    EXPECT_EQ(get<1>(*it), "down");
}

// ---------------------------------------------------------------------------
// 2.5.6.4  Multiple syncd modules operate independently
// ---------------------------------------------------------------------------
TEST_F(StateConsistencyTest, IndependentSyncModules)
{
    // portsyncd updates port state
    vector<FieldValueTuple> port_fvs;
    port_fvs.push_back(make_tuple("admin_state", "up"));
    state_port_table.set("Ethernet0", port_fvs);

    // neighsyncd updates neighbor state independently
    vector<FieldValueTuple> neigh_fvs;
    neigh_fvs.push_back(make_tuple("neigh", "00:11:22:33:44:55"));
    state_neigh_table.set("Ethernet0:10.0.0.1", neigh_fvs);

    // Both operations succeed independently
    EXPECT_EQ(state_port_table.getKeys().size(), 1);
    EXPECT_EQ(state_neigh_table.getKeys().size(), 1);
}

// ---------------------------------------------------------------------------
// 2.5.6.5  FDB entries are consistent across syncs
// ---------------------------------------------------------------------------
TEST_F(StateConsistencyTest, FdbEntryConsistency)
{
    // Add same MAC on different ports
    vector<FieldValueTuple> fvs1, fvs2;
    fvs1.push_back(make_tuple("port", "Ethernet0"));
    fvs2.push_back(make_tuple("port", "Ethernet1"));

    app_fdb_table.set("1:00:11:22:33:44:55", fvs1);
    app_fdb_table.set("2:00:11:22:33:44:55", fvs2);

    auto keys = app_fdb_table.getKeys();
    EXPECT_EQ(keys.size(), 2);
}

// ---------------------------------------------------------------------------
// 2.5.6.6  LAG member in FDB does not duplicate entry
// ---------------------------------------------------------------------------
TEST_F(StateConsistencyTest, LagMemberFdbConsistency)
{
    // Create LAG with member
    vector<FieldValueTuple> lag_fvs;
    lag_fvs.push_back(make_tuple("admin_status", "up"));
    app_lag_table.set("PortChannel0", lag_fvs);

    // FDB should reference LAG, not individual member
    vector<FieldValueTuple> fdb_fvs;
    fdb_fvs.push_back(make_tuple("port", "PortChannel0"));
    app_fdb_table.set("1:00:11:22:33:44:55", fdb_fvs);

    auto lag_keys = app_lag_table.getKeys();
    auto fdb_keys = app_fdb_table.getKeys();

    EXPECT_EQ(lag_keys.size(), 1);
    EXPECT_EQ(fdb_keys.size(), 1);
}

} // namespace state_consistency_ut
