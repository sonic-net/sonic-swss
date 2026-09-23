#define private public
#define protected public
#include "directory.h"
#include "orch.h"
#include "ut_helper.h"
#include "mock_orchagent_main.h"
#include "mock_table.h"
#include "mock_response_publisher.h"
#include "mock_sai_api.h"
#include "bulker.h"
#undef protected
#undef private

extern string gMySwitchType;
extern bool gEnableFibSuppress;

extern std::unique_ptr<MockResponsePublisher> gMockResponsePublisher;

using ::testing::_;

EXTERN_MOCK_FNS

extern bool gOrchUnhealthy;

namespace routebulk_race_test
{
    using namespace std;
    using ::testing::SetArrayArgument;
    using ::testing::Return;
    using ::testing::DoAll;

    DEFINE_SAI_API_MOCK_SPECIFY_ENTRY_WITH_SET(route, route);

    shared_ptr<swss::DBConnector> m_app_db;
    shared_ptr<swss::DBConnector> m_config_db;
    shared_ptr<swss::DBConnector> m_state_db;
    shared_ptr<swss::DBConnector> m_chassis_app_db;

    int create_route_count = 0;
    int set_route_count = 0;
    int remove_route_count = 0;

    sai_route_api_t *pold_sai_route_api;

    sai_bulk_create_route_entry_fn              old_create_route_entries;
    sai_bulk_remove_route_entry_fn              old_remove_route_entries;
    sai_bulk_set_route_entry_attribute_fn       old_set_route_entries_attribute;

    sai_status_t _ut_stub_sai_bulk_create_route_entry(
        _In_ uint32_t object_count,
        _In_ const sai_route_entry_t *route_entry,
        _In_ const uint32_t *attr_count,
        _In_ const sai_attribute_t **attr_list,
        _In_ sai_bulk_op_error_mode_t mode,
        _Out_ sai_status_t *object_statuses)
    {
        create_route_count++;
        return old_create_route_entries(object_count, route_entry, attr_count, attr_list, mode, object_statuses);
    }

    sai_status_t _ut_stub_sai_bulk_remove_route_entry(
        _In_ uint32_t object_count,
        _In_ const sai_route_entry_t *route_entry,
        _In_ sai_bulk_op_error_mode_t mode,
        _Out_ sai_status_t *object_statuses)
    {
        remove_route_count++;
        return old_remove_route_entries(object_count, route_entry, mode, object_statuses);
    }

    sai_status_t _ut_stub_sai_bulk_set_route_entry_attribute(
        _In_ uint32_t object_count,
        _In_ const sai_route_entry_t *route_entry,
        _In_ const sai_attribute_t *attr_list,
        _In_ sai_bulk_op_error_mode_t mode,
        _Out_ sai_status_t *object_statuses)
    {
        set_route_count++;
        return old_set_route_entries_attribute(object_count, route_entry, attr_list, mode, object_statuses);
    }

    struct BulkRaceTest : public ::testing::Test
    {
        BulkRaceTest()
        {
        }

        void SetUp() override
        {
            ASSERT_EQ(sai_route_api, nullptr);
            map<string, string> profile = {
                { "SAI_VS_SWITCH_TYPE", "SAI_VS_SWITCH_TYPE_BCM56850" },
                { "KV_DEVICE_MAC_ADDRESS", "20:03:04:05:06:00" }
            };

            ut_helper::initSaiApi(profile);

            INIT_SAI_API_MOCK(route);
            MockSaiApis();

            old_create_route_entries = sai_route_api->create_route_entries;
            old_remove_route_entries = sai_route_api->remove_route_entries;
            old_set_route_entries_attribute = sai_route_api->set_route_entries_attribute;

            pold_sai_route_api = sai_route_api;
            sai_route_api = &ut_sai_route_api;

            sai_route_api->create_route_entries = _ut_stub_sai_bulk_create_route_entry;
            sai_route_api->remove_route_entries = _ut_stub_sai_bulk_remove_route_entry;
            sai_route_api->set_route_entries_attribute = _ut_stub_sai_bulk_set_route_entry_attribute;

            m_app_db = make_shared<swss::DBConnector>("APPL_DB", 0);
            m_config_db = make_shared<swss::DBConnector>("CONFIG_DB", 0);
            m_state_db = make_shared<swss::DBConnector>("STATE_DB", 0);
            if(gMySwitchType == "voq")
                m_chassis_app_db = make_shared<swss::DBConnector>("CHASSIS_APP_DB", 0);

            sai_attribute_t attr;

            attr.id = SAI_SWITCH_ATTR_INIT_SWITCH;
            attr.value.booldata = true;

            auto status = sai_switch_api->create_switch(&gSwitchId, 1, &attr);
            ASSERT_EQ(status, SAI_STATUS_SUCCESS);

            attr.id = SAI_SWITCH_ATTR_SRC_MAC_ADDRESS;
            status = sai_switch_api->get_switch_attribute(gSwitchId, 1, &attr);
            ASSERT_EQ(status, SAI_STATUS_SUCCESS);
            gMacAddress = attr.value.mac;

            attr.id = SAI_SWITCH_ATTR_DEFAULT_VIRTUAL_ROUTER_ID;
            status = sai_switch_api->get_switch_attribute(gSwitchId, 1, &attr);
            ASSERT_EQ(status, SAI_STATUS_SUCCESS);
            gVirtualRouterId = attr.value.oid;

            ASSERT_EQ(gCrmOrch, nullptr);
            gCrmOrch = new CrmOrch(m_config_db.get(), CFG_CRM_TABLE_NAME);

            TableConnector stateDbSwitchTable(m_state_db.get(), "SWITCH_CAPABILITY");
            TableConnector conf_asic_sensors(m_config_db.get(), CFG_ASIC_SENSORS_TABLE_NAME);
            TableConnector app_switch_table(m_app_db.get(),  APP_SWITCH_TABLE_NAME);

            vector<TableConnector> switch_tables = {
                conf_asic_sensors,
                app_switch_table
            };

            ASSERT_EQ(gSwitchOrch, nullptr);
            gSwitchOrch = new SwitchOrch(m_app_db.get(), switch_tables, stateDbSwitchTable);

            const int portsorch_base_pri = 40;

            vector<table_name_with_pri_t> ports_tables = {
                { APP_PORT_TABLE_NAME, portsorch_base_pri + 5 },
                { APP_VLAN_TABLE_NAME, portsorch_base_pri + 2 },
                { APP_VLAN_MEMBER_TABLE_NAME, portsorch_base_pri },
                { APP_LAG_TABLE_NAME, portsorch_base_pri + 4 },
                { APP_LAG_MEMBER_TABLE_NAME, portsorch_base_pri }
            };

            ASSERT_EQ(gPortsOrch, nullptr);
            gPortsOrch = new PortsOrch(m_app_db.get(), m_state_db.get(), ports_tables, m_chassis_app_db.get());
            gDirectory.set(gPortsOrch);

            vector<string> flex_counter_tables = {
                CFG_FLEX_COUNTER_TABLE_NAME
            };
            auto* flexCounterOrch = new FlexCounterOrch(m_config_db.get(), flex_counter_tables);
            gDirectory.set(flexCounterOrch);

            static const  vector<string> route_pattern_tables = {
                CFG_FLOW_COUNTER_ROUTE_PATTERN_TABLE_NAME,
            };
            gFlowCounterRouteOrch = new FlowCounterRouteOrch(m_config_db.get(), route_pattern_tables);
            gDirectory.set(gFlowCounterRouteOrch);

            ASSERT_EQ(gVrfOrch, nullptr);
            gVrfOrch = new VRFOrch(m_app_db.get(), APP_VRF_TABLE_NAME, m_state_db.get(), STATE_VRF_OBJECT_TABLE_NAME);
            gDirectory.set(gVrfOrch);

            EvpnNvoOrch *evpn_orch = new EvpnNvoOrch(m_app_db.get(), APP_VXLAN_EVPN_NVO_TABLE_NAME);
            gDirectory.set(evpn_orch);

            ASSERT_EQ(gIntfsOrch, nullptr);
            gIntfsOrch = new IntfsOrch(m_app_db.get(), APP_INTF_TABLE_NAME, gVrfOrch, m_chassis_app_db.get());

            const int fdborch_pri = 20;

            vector<table_name_with_pri_t> app_fdb_tables = {
                { APP_FDB_TABLE_NAME,        FdbOrch::fdborch_pri},
                { APP_VXLAN_FDB_TABLE_NAME,  FdbOrch::fdborch_pri},
                { APP_MCLAG_FDB_TABLE_NAME,  fdborch_pri}
            };

            TableConnector stateDbFdb(m_state_db.get(), STATE_FDB_TABLE_NAME);
            TableConnector stateMclagDbFdb(m_state_db.get(), STATE_MCLAG_REMOTE_FDB_TABLE_NAME);
            ASSERT_EQ(gFdbOrch, nullptr);
            gFdbOrch = new FdbOrch(m_app_db.get(), app_fdb_tables, stateDbFdb, stateMclagDbFdb, gPortsOrch);

            ASSERT_EQ(gNeighOrch, nullptr);
            gNeighOrch = new NeighOrch(m_app_db.get(), APP_NEIGH_TABLE_NAME, gIntfsOrch, gFdbOrch, gPortsOrch, m_chassis_app_db.get());

            ASSERT_EQ(gTunneldecapOrch, nullptr);
            vector<string> tunnel_tables = {
                APP_TUNNEL_DECAP_TABLE_NAME,
                APP_TUNNEL_DECAP_TERM_TABLE_NAME
            };
            gTunneldecapOrch = new TunnelDecapOrch(m_app_db.get(), m_state_db.get(), m_config_db.get(), tunnel_tables);

            vector<string> mux_tables = {
                CFG_MUX_CABLE_TABLE_NAME,
                CFG_PEER_SWITCH_TABLE_NAME
            };
            MuxOrch *mux_orch = new MuxOrch(m_config_db.get(), mux_tables, gTunneldecapOrch, gNeighOrch, gFdbOrch);
            gDirectory.set(mux_orch);

            ASSERT_EQ(gFgNhgOrch, nullptr);
            const int fgnhgorch_pri = 15;

            vector<table_name_with_pri_t> fgnhg_tables = {
                { CFG_FG_NHG,                 fgnhgorch_pri },
                { CFG_FG_NHG_PREFIX,          fgnhgorch_pri },
                { CFG_FG_NHG_MEMBER,          fgnhgorch_pri }
            };
            gFgNhgOrch = new FgNhgOrch(m_config_db.get(), m_app_db.get(), m_state_db.get(), fgnhg_tables, gNeighOrch, gIntfsOrch, gVrfOrch);

            ASSERT_EQ(gSrv6Orch, nullptr);
            TableConnector srv6_sid_list_table(m_app_db.get(), APP_SRV6_SID_LIST_TABLE_NAME);
            TableConnector srv6_my_sid_table(m_app_db.get(), APP_SRV6_MY_SID_TABLE_NAME);
            TableConnector srv6_my_sid_cfg_table(m_config_db.get(), CFG_SRV6_MY_SID_TABLE_NAME);

            vector<TableConnector> srv6_tables = {
                srv6_sid_list_table,
                srv6_my_sid_table,
                srv6_my_sid_cfg_table
            };
            gSrv6Orch = new Srv6Orch(m_config_db.get(), m_app_db.get(), srv6_tables, gSwitchOrch, gVrfOrch, gNeighOrch);

            ASSERT_EQ(gRouteOrch, nullptr);
            const int routeorch_pri = 5;
            vector<table_name_with_pri_t> route_tables = {
                { APP_ROUTE_TABLE_NAME,        routeorch_pri },
                { APP_LABEL_ROUTE_TABLE_NAME,  routeorch_pri }
            };
            gRouteOrch = new RouteOrch(m_app_db.get(), route_tables, gSwitchOrch, gNeighOrch, gIntfsOrch, gVrfOrch, gFgNhgOrch, gSrv6Orch);
            gNhgOrch = new NhgOrch(m_app_db.get(), APP_NEXTHOP_GROUP_TABLE_NAME);

            vector<string> buffer_tables = { APP_BUFFER_POOL_TABLE_NAME,
                                             APP_BUFFER_PROFILE_TABLE_NAME,
                                             APP_BUFFER_QUEUE_TABLE_NAME,
                                             APP_BUFFER_PG_TABLE_NAME,
                                             APP_BUFFER_PORT_INGRESS_PROFILE_LIST_NAME,
                                             APP_BUFFER_PORT_EGRESS_PROFILE_LIST_NAME };

            gBufferOrch = new BufferOrch(m_app_db.get(), m_config_db.get(), m_state_db.get(), buffer_tables);

            Table portTable = Table(m_app_db.get(), APP_PORT_TABLE_NAME);

            auto ports = ut_helper::getInitialSaiPorts();

            for (const auto &it : ports)
            {
                portTable.set(it.first, it.second);
                portTable.set(it.first, {{ "oper_status", "up" }});
            }

            portTable.set("PortConfigDone", { { "count", to_string(ports.size()) } });
            gPortsOrch->addExistingData(&portTable);
            static_cast<Orch *>(gPortsOrch)->doTask();

            portTable.set("PortInitDone", { { "lanes", "0" } });
            gPortsOrch->addExistingData(&portTable);
            static_cast<Orch *>(gPortsOrch)->doTask();

            Table intfTable = Table(m_app_db.get(), APP_INTF_TABLE_NAME);
            intfTable.set("Loopback0", { {"NULL", "NULL" },
                                         {"mac_addr", "00:00:00:00:00:00" }});
            intfTable.set("Loopback0:10.1.0.32/32", { { "scope", "global" },
                                                      { "family", "IPv4" }});
            intfTable.set("Ethernet0", { {"NULL", "NULL" },
                                         {"mac_addr", "00:00:00:00:00:00" }});
            intfTable.set("Ethernet0:10.0.0.1/24", { { "scope", "global" },
                                                     { "family", "IPv4" }});
            intfTable.set("Ethernet4", { {"NULL", "NULL" },
                                         {"mac_addr", "00:00:00:00:00:00" }});
            intfTable.set("Ethernet4:11.0.0.1/32", { { "scope", "global" },
                                                     { "family", "IPv4" }});
            intfTable.set("Ethernet8", { {"NULL", "NULL" },
                                         {"vrf_name", "Vrf1"},
                                         {"mac_addr", "00:00:00:00:00:00" }});
            intfTable.set("Ethernet8:20.0.0.1/24", { { "scope", "global" },
                                                     { "family", "IPv4" }});
            gIntfsOrch->addExistingData(&intfTable);
            static_cast<Orch *>(gIntfsOrch)->doTask();

            Table neighborTable = Table(m_app_db.get(), APP_NEIGH_TABLE_NAME);

            map<string, string> neighborIp2Mac = {{"10.0.0.2", "00:00:0a:00:00:02" },
                                                  {"10.0.0.3", "00:00:0a:00:00:03" } };
            neighborTable.set("Ethernet0:10.0.0.2", { {"neigh", neighborIp2Mac["10.0.0.2"]},
                                                      {"family", "IPv4" }});
            neighborTable.set("Ethernet0:10.0.0.3", { {"neigh", neighborIp2Mac["10.0.0.3"]},
                                                      {"family", "IPv4" }});
            gNeighOrch->addExistingData(&neighborTable);
            static_cast<Orch *>(gNeighOrch)->doTask();

            Table routeTable = Table(m_app_db.get(), APP_ROUTE_TABLE_NAME);
            routeTable.set("1.1.1.0/24", { {"ifname", "Ethernet0" },
                                           {"nexthop", "10.0.0.2" }});
            routeTable.set("0.0.0.0/0", { {"ifname", "Ethernet0" },
                                           {"nexthop", "10.0.0.2" }});
            gRouteOrch->addExistingData(&routeTable);
            static_cast<Orch *>(gRouteOrch)->doTask();
        }

        void TearDown() override
        {
            gEnableFibSuppress = false;

            RestoreSaiApis();
            DEINIT_SAI_API_MOCK(route);

            gDirectory.m_values.clear();

            delete gCrmOrch;
            gCrmOrch = nullptr;

            delete gSwitchOrch;
            gSwitchOrch = nullptr;

            delete gVrfOrch;
            gVrfOrch = nullptr;

            delete gIntfsOrch;
            gIntfsOrch = nullptr;

            delete gSrv6Orch;
            gSrv6Orch = nullptr;

            delete gNeighOrch;
            gNeighOrch = nullptr;

            delete gTunneldecapOrch;
            gTunneldecapOrch = nullptr;

            delete gFdbOrch;
            gFdbOrch = nullptr;

            delete gFgNhgOrch;
            gFgNhgOrch = nullptr;

            delete gRouteOrch;
            gRouteOrch = nullptr;

            delete gPortsOrch;
            gPortsOrch = nullptr;

            delete gBufferOrch;
            gBufferOrch = nullptr;

            sai_route_api = pold_sai_route_api;
            ut_helper::uninitSaiApi();
        }
    };

    TEST_F(BulkRaceTest, StaleFvsPreservesEntry)
    {
        auto *routeConsumer = dynamic_cast<Consumer *>(gRouteOrch->getExecutor(APP_ROUTE_TABLE_NAME));
        ASSERT_NE(routeConsumer, nullptr);

        // Step 1: Create route 2.2.2.0/24 via normal doTask so SAI state exists.
        {
            std::deque<KeyOpFieldsValuesTuple> entries;
            entries.push_back({ "2.2.2.0/24", "SET",
                            { {"ifname", "Ethernet0"}, {"nexthop", "10.0.0.2"} }});
            routeConsumer->addToSync(entries);
            static_cast<Orch *>(gRouteOrch)->doTask();
        }
        ASSERT_EQ(routeConsumer->m_toSync.count("2.2.2.0/24"), 0u);

        // Step 2: Construct a stale BulkMap simulating a chunk that was built
        // with the original field-values {nexthop=10.0.0.2}.
        RouteOrch::BulkMap staleBulk;
        staleBulk.emplace(std::piecewise_construct,
                          std::forward_as_tuple("2.2.2.0/24", std::string("SET")),
                          std::forward_as_tuple("2.2.2.0/24", true));
        auto &ctx = staleBulk.begin()->second;
        ctx.captured_fvs = { {"ifname", "Ethernet0"}, {"nexthop", "10.0.0.2"} };

        // Step 3: Inject a newer entry in m_toSync with different field-values.
        // This simulates a new update arriving after the chunk was built.
        {
            std::deque<KeyOpFieldsValuesTuple> entries;
            entries.push_back({ "2.2.2.0/24", "SET",
                            { {"ifname", "Ethernet0"}, {"nexthop", "10.0.0.3"} }});
            routeConsumer->addToSync(entries);
        }
        ASSERT_EQ(routeConsumer->m_toSync.count("2.2.2.0/24"), 1u);

        // Step 4: Call processRouteBulkResults with the stale bulk.
        // The guard should detect the FVS mismatch and NOT erase the newer entry.
        gRouteOrch->processRouteBulkResults(*routeConsumer, staleBulk);

        // The newer entry must still be in m_toSync — the guard prevented erasure.
        ASSERT_EQ(routeConsumer->m_toSync.count("2.2.2.0/24"), 1u);

        // Verify the surviving entry has the newer nexthop.
        auto it = routeConsumer->m_toSync.find("2.2.2.0/24");
        ASSERT_NE(it, routeConsumer->m_toSync.end());
        auto &fvs = kfvFieldsValues(it->second);
        bool found_nh = false;
        for (const auto &fv : fvs)
        {
            if (fvField(fv) == "nexthop")
            {
                EXPECT_EQ(fvValue(fv), "10.0.0.3");
                found_nh = true;
            }
        }
        EXPECT_TRUE(found_nh);

        // Step 5: Run doTask to program the newer entry.
        auto base_set = set_route_count;
        static_cast<Orch *>(gRouteOrch)->doTask();
        EXPECT_GE(set_route_count, base_set + 1);
        EXPECT_EQ(routeConsumer->m_toSync.count("2.2.2.0/24"), 0u);
    }

    TEST_F(BulkRaceTest, MatchingFvsAllowsErase)
    {
        auto *routeConsumer = dynamic_cast<Consumer *>(gRouteOrch->getExecutor(APP_ROUTE_TABLE_NAME));
        ASSERT_NE(routeConsumer, nullptr);

        // Create route 3.3.3.0/24
        {
            std::deque<KeyOpFieldsValuesTuple> entries;
            entries.push_back({ "3.3.3.0/24", "SET",
                            { {"ifname", "Ethernet0"}, {"nexthop", "10.0.0.2"} }});
            routeConsumer->addToSync(entries);
            static_cast<Orch *>(gRouteOrch)->doTask();
        }
        ASSERT_EQ(routeConsumer->m_toSync.count("3.3.3.0/24"), 0u);

        // Re-add the same entry to m_toSync (same FVs as the chunk).
        {
            std::deque<KeyOpFieldsValuesTuple> entries;
            entries.push_back({ "3.3.3.0/24", "SET",
                            { {"ifname", "Ethernet0"}, {"nexthop", "10.0.0.2"} }});
            routeConsumer->addToSync(entries);
        }
        ASSERT_EQ(routeConsumer->m_toSync.count("3.3.3.0/24"), 1u);

        // Build a BulkMap with matching FVs and a successful object_status.
        RouteOrch::BulkMap bulk;
        bulk.emplace(std::piecewise_construct,
                     std::forward_as_tuple("3.3.3.0/24", std::string("SET")),
                     std::forward_as_tuple("3.3.3.0/24", true));
        auto &ctx = bulk.begin()->second;
        ctx.captured_fvs = { {"ifname", "Ethernet0"}, {"nexthop", "10.0.0.2"} };
        ctx.vrf_id = gVirtualRouterId;
        ctx.ip_prefix = IpPrefix("3.3.3.0/24");
        ctx.object_statuses.push_back(SAI_STATUS_SUCCESS);

        gRouteOrch->processRouteBulkResults(*routeConsumer, bulk);

        // Matching FVs + successful status → entry erased from m_toSync.
        EXPECT_EQ(routeConsumer->m_toSync.count("3.3.3.0/24"), 0u);
    }

    TEST_F(BulkRaceTest, DrainPendingBulkSettlesBothMaps)
    {
        auto *routeConsumer = dynamic_cast<Consumer *>(gRouteOrch->getExecutor(APP_ROUTE_TABLE_NAME));
        ASSERT_NE(routeConsumer, nullptr);

        // Create routes 4.4.4.0/24 and 5.5.5.0/24 via normal doTask.
        {
            std::deque<KeyOpFieldsValuesTuple> entries;
            entries.push_back({ "4.4.4.0/24", "SET",
                            { {"ifname", "Ethernet0"}, {"nexthop", "10.0.0.2"} }});
            entries.push_back({ "5.5.5.0/24", "SET",
                            { {"ifname", "Ethernet0"}, {"nexthop", "10.0.0.2"} }});
            routeConsumer->addToSync(entries);
            static_cast<Orch *>(gRouteOrch)->doTask();
        }
        ASSERT_EQ(routeConsumer->m_toSync.count("4.4.4.0/24"), 0u);
        ASSERT_EQ(routeConsumer->m_toSync.count("5.5.5.0/24"), 0u);

        // Populate m_prevPendingToBulk with 4.4.4.0/24 (already-reaped results).
        gRouteOrch->m_prevPendingToBulk.emplace(
            std::piecewise_construct,
            std::forward_as_tuple("4.4.4.0/24", std::string("SET")),
            std::forward_as_tuple("4.4.4.0/24", true));
        {
            auto &ctx = gRouteOrch->m_prevPendingToBulk.begin()->second;
            ctx.captured_fvs = { {"ifname", "Ethernet0"}, {"nexthop", "10.0.0.2"} };
            ctx.vrf_id = gVirtualRouterId;
            ctx.ip_prefix = IpPrefix("4.4.4.0/24");
            ctx.object_statuses.push_back(SAI_STATUS_SUCCESS);
        }
        gRouteOrch->m_hasPrevResults = true;

        // Populate m_pendingToBulk with 5.5.5.0/24 (flush still outstanding).
        gRouteOrch->m_pendingToBulk.emplace(
            std::piecewise_construct,
            std::forward_as_tuple("5.5.5.0/24", std::string("SET")),
            std::forward_as_tuple("5.5.5.0/24", true));
        {
            auto &ctx = gRouteOrch->m_pendingToBulk.begin()->second;
            ctx.captured_fvs = { {"ifname", "Ethernet0"}, {"nexthop", "10.0.0.2"} };
            ctx.vrf_id = gVirtualRouterId;
            ctx.ip_prefix = IpPrefix("5.5.5.0/24");
            ctx.object_statuses.push_back(SAI_STATUS_SUCCESS);
        }
        gRouteOrch->m_hasPendingBulk = true;

        // Attach a submitter — waitForFlush returns immediately when idle.
        gRouteOrch->m_submitter = std::make_unique<RouteBulkSubmitter>();

        // Re-add both routes to m_toSync with matching FVs.
        {
            std::deque<KeyOpFieldsValuesTuple> entries;
            entries.push_back({ "4.4.4.0/24", "SET",
                            { {"ifname", "Ethernet0"}, {"nexthop", "10.0.0.2"} }});
            entries.push_back({ "5.5.5.0/24", "SET",
                            { {"ifname", "Ethernet0"}, {"nexthop", "10.0.0.2"} }});
            routeConsumer->addToSync(entries);
        }
        ASSERT_EQ(routeConsumer->m_toSync.count("4.4.4.0/24"), 1u);
        ASSERT_EQ(routeConsumer->m_toSync.count("5.5.5.0/24"), 1u);

        // Warm-restart freeze path: drain all pending bulk work.
        gRouteOrch->drainPendingBulk();

        EXPECT_FALSE(gRouteOrch->m_hasPrevResults);
        EXPECT_FALSE(gRouteOrch->m_hasPendingBulk);
        EXPECT_TRUE(gRouteOrch->m_prevPendingToBulk.empty());
        EXPECT_TRUE(gRouteOrch->m_pendingToBulk.empty());
        EXPECT_EQ(routeConsumer->m_toSync.count("4.4.4.0/24"), 0u);
        EXPECT_EQ(routeConsumer->m_toSync.count("5.5.5.0/24"), 0u);
    }
}
