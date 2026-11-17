#define private public // make Directory::m_values available to clean it.
#include "directory.h"
#undef private
#define protected public
#include "orch.h"
#undef protected
/* Reach RouteOrch's drain state and its bulker; must precede
 * mock_orchagent_main.h, which includes routeorch.h. */
#define private public
#include "routeorch.h"
#undef private
#include "ut_helper.h"
#include "mock_orchagent_main.h"
#include "swssnet.h"
#include "mock_table.h"
#include "mock_response_publisher.h"
#include "mock_sai_api.h"
#include "bulker.h"
#include "select.h"
#include "zmqclient.h"
#include "zmqproducerstatetable.h"

#include <chrono>
#include <thread>

extern string gMySwitchType;
extern bool gEnableFibSuppress;

extern std::unique_ptr<MockResponsePublisher> gMockResponsePublisher;

using ::testing::_;

EXTERN_MOCK_FNS

namespace routeorch_test
{
    using namespace std;
    using ::testing::SetArrayArgument;
    using ::testing::Return;
    using ::testing::DoAll;

    static bool stateRouteStateFieldExists(swss::DBConnector* state_db,
                                       const std::string& prefix)
    {
        Table stateRoute(state_db, "ROUTE_TABLE");
        std::vector<FieldValueTuple> fvs;
        stateRoute.get(prefix, fvs);
        for (const auto &fv : fvs)
            if (fvField(fv) == "state")
                return true;
        return false;
    }

    static bool waitStateRouteState(swss::DBConnector* state_db,
                                const std::string& prefix,
                                const std::string& want,
                                int attempts = 30)
    {
        Table stateRoute(state_db, "ROUTE_TABLE");
        for (int i = 0; i < attempts; ++i)
        {
            std::vector<FieldValueTuple> fvs;
            stateRoute.get(prefix, fvs);
            for (const auto &fv : fvs)
                if (fvField(fv) == "state" && fvValue(fv) == want)
                    return true;

            // Let orch process any pending work again
            static_cast<Orch *>(gRouteOrch)->doTask();
        }
        return false;
    }

    DEFINE_SAI_API_MOCK_SPECIFY_ENTRY_WITH_SET(route, route);
    // Mock next hop group generic API to control NHG creation behavior
    DEFINE_SAI_GENERIC_API_MOCK(next_hop_group, next_hop_group);

    shared_ptr<swss::DBConnector> m_app_db;
    shared_ptr<swss::DBConnector> m_config_db;
    shared_ptr<swss::DBConnector> m_state_db;
    shared_ptr<swss::DBConnector> m_chassis_app_db;

    int create_route_count = 0;
    int set_route_count = 0;
    int remove_route_count = 0;
    int sai_fail_count = 0;
    int drop_set_count = 0;

    // sai_route_api_t ut_sai_route_api;
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

        // Make sure there is not conflict settings
        bool drop = false;
        bool valid_nexthop = false;
        for (uint32_t i = 0; i < object_count; i++)
        {
            if (route_entry[i].destination.mask.ip4 == 0)
            {
                if (attr_list[i].id == SAI_ROUTE_ENTRY_ATTR_PACKET_ACTION)
                {
                    drop = (attr_list[i].value.s32 == SAI_PACKET_ACTION_DROP);
                }
                else if (attr_list[i].id == SAI_ROUTE_ENTRY_ATTR_NEXT_HOP_ID)
                {
                    valid_nexthop = (attr_list[i].value.oid != SAI_NULL_OBJECT_ID);
                }
            }
        }

        if(drop) {
            drop_set_count++;
        }

        // Drop and a valid nexthop can not be provided for the same prefix
        if (drop && valid_nexthop)
            sai_fail_count++;

        return old_set_route_entries_attribute(object_count, route_entry, attr_list, mode, object_statuses);
    }

    struct RouteOrchTest : public ::testing::Test
    {
        FlexCounterOrch *m_flexCounterOrch = nullptr;
        EvpnNvoOrch *m_evpnNvoOrch = nullptr;
        MuxOrch *m_muxOrch = nullptr;

        RouteOrchTest()
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
            INIT_SAI_API_MOCK(next_hop_group);
            MockSaiApis();

            // Hack the route create function
            old_create_route_entries = sai_route_api->create_route_entries;
            old_remove_route_entries = sai_route_api->remove_route_entries;
            old_set_route_entries_attribute = sai_route_api->set_route_entries_attribute;

            pold_sai_route_api = sai_route_api;
            sai_route_api = &ut_sai_route_api;

            sai_route_api->create_route_entries = _ut_stub_sai_bulk_create_route_entry;
            sai_route_api->remove_route_entries = _ut_stub_sai_bulk_remove_route_entry;
            sai_route_api->set_route_entries_attribute = _ut_stub_sai_bulk_set_route_entry_attribute;

            // Init switch and create dependencies
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

            // Get switch source MAC address
            attr.id = SAI_SWITCH_ATTR_SRC_MAC_ADDRESS;
            status = sai_switch_api->get_switch_attribute(gSwitchId, 1, &attr);

            ASSERT_EQ(status, SAI_STATUS_SUCCESS);

            gMacAddress = attr.value.mac;

            // Get the default virtual router ID
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

            // Create dependencies ...

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

            TableConnector appDbDfTable(m_app_db.get(), "EVPN_DF_TABLE");
            TableConnector confDbEvpnEsTable(m_config_db.get(), "EVPN_ETHERNET_SEGMENT");

            vector<TableConnector> evpn_df_es_table_connectors = {
                appDbDfTable,
                confDbEvpnEsTable,
            };

            gEvpnMhOrch = new EvpnMhOrch(evpn_df_es_table_connectors);
            gDirectory.set(gEvpnMhOrch);

            vector<string> flex_counter_tables = {
                CFG_FLEX_COUNTER_TABLE_NAME
            };
            m_flexCounterOrch = new FlexCounterOrch(m_config_db.get(), flex_counter_tables);
            gDirectory.set(m_flexCounterOrch);

            static const  vector<string> route_pattern_tables = {
                CFG_FLOW_COUNTER_ROUTE_PATTERN_TABLE_NAME,
            };
            gFlowCounterRouteOrch = new FlowCounterRouteOrch(m_config_db.get(), route_pattern_tables);
            gDirectory.set(gFlowCounterRouteOrch);

            ASSERT_EQ(gVrfOrch, nullptr);
            gVrfOrch = new VRFOrch(m_app_db.get(), APP_VRF_TABLE_NAME, m_state_db.get(), STATE_VRF_OBJECT_TABLE_NAME);
            gDirectory.set(gVrfOrch);

            m_evpnNvoOrch = new EvpnNvoOrch(m_app_db.get(), APP_VXLAN_EVPN_NVO_TABLE_NAME);
            gDirectory.set(m_evpnNvoOrch);

            vector<table_name_with_pri_t> intf_tables = {
                { APP_INTF_TABLE_NAME,  IntfsOrch::intfsorch_pri},
                { APP_SAG_TABLE_NAME,   IntfsOrch::intfsorch_pri}
            };
            ASSERT_EQ(gIntfsOrch, nullptr);
            gIntfsOrch = new IntfsOrch(m_app_db.get(), intf_tables, gVrfOrch, m_chassis_app_db.get());

            const int fdborch_pri = 20;

            vector<table_name_with_pri_t> app_fdb_tables = {
                { APP_FDB_TABLE_NAME,        FdbOrch::fdborch_pri},
                { APP_VXLAN_FDB_TABLE_NAME,  FdbOrch::fdborch_pri},
                { APP_MCLAG_FDB_TABLE_NAME,  fdborch_pri}
            };

            TableConnector stateDbFdb(m_state_db.get(), STATE_FDB_TABLE_NAME);
            TableConnector stateMclagDbFdb(m_state_db.get(), STATE_MCLAG_REMOTE_FDB_TABLE_NAME);
            ASSERT_EQ(gFdbOrch, nullptr);
            gFdbOrch = new FdbOrch(m_app_db.get(), app_fdb_tables, stateDbFdb, stateMclagDbFdb, gPortsOrch,
                                   m_config_db.get());

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
            m_muxOrch = new MuxOrch(m_config_db.get(), mux_tables, gTunneldecapOrch, gNeighOrch, gFdbOrch);
            gDirectory.set(m_muxOrch);

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

            // Recreate buffer orch to read populated data
            vector<string> buffer_tables = { APP_BUFFER_POOL_TABLE_NAME,
                                             APP_BUFFER_PROFILE_TABLE_NAME,
                                             APP_BUFFER_QUEUE_TABLE_NAME,
                                             APP_BUFFER_PG_TABLE_NAME,
                                             APP_BUFFER_PORT_INGRESS_PROFILE_LIST_NAME,
                                             APP_BUFFER_PORT_EGRESS_PROFILE_LIST_NAME };

            gBufferOrch = new BufferOrch(m_app_db.get(), m_config_db.get(), m_state_db.get(), buffer_tables);

            Table portTable = Table(m_app_db.get(), APP_PORT_TABLE_NAME);

            // Get SAI default ports to populate DB
            auto ports = ut_helper::getInitialSaiPorts();

            // Populate pot table with SAI ports
            for (const auto &it : ports)
            {
                portTable.set(it.first, it.second);
                portTable.set(it.first, {{ "oper_status", "up" }});
            }

            // Set PortConfigDone
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
            DEINIT_SAI_API_MOCK(next_hop_group);

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

            delete gNhgOrch;
            gNhgOrch = nullptr;

            delete gFlowCounterRouteOrch;
            gFlowCounterRouteOrch = nullptr;

            delete gRouteOrch;
            gRouteOrch = nullptr;

            delete gPortsOrch;
            gPortsOrch = nullptr;

            delete gBufferOrch;
            gBufferOrch = nullptr;

            delete gEvpnMhOrch;
            gEvpnMhOrch = nullptr;

            delete m_muxOrch;
            m_muxOrch = nullptr;

            delete m_evpnNvoOrch;
            m_evpnNvoOrch = nullptr;

            delete m_flexCounterOrch;
            m_flexCounterOrch = nullptr;

            sai_route_api = pold_sai_route_api;
            ut_helper::uninitSaiApi();
        }
    };

    TEST_F(RouteOrchTest, RouteOrchTempRouteUniformSelection)
    {
        // --- Step 1: Setup resolved neighbors ---
        Table neighborTable(m_app_db.get(), APP_NEIGH_TABLE_NAME);

        std::map<std::string, std::string> neighborIp2Mac = {
            {"10.0.0.4", "00:00:0a:00:00:04"},
            {"10.0.0.5", "00:00:0a:00:00:05"},
            {"10.0.0.6", "00:00:0a:00:00:06"}
        };

        // Use Ethernet0 which is already configured with 10.0.0.1/24
        neighborTable.set("Ethernet0:10.0.0.4", {{"neigh", neighborIp2Mac["10.0.0.4"]}, {"family", "IPv4"}});
        neighborTable.set("Ethernet0:10.0.0.5", {{"neigh", neighborIp2Mac["10.0.0.5"]}, {"family", "IPv4"}});
        neighborTable.set("Ethernet0:10.0.0.6", {{"neigh", neighborIp2Mac["10.0.0.6"]}, {"family", "IPv4"}});

        gNeighOrch->addExistingData(&neighborTable);
        static_cast<Orch *>(gNeighOrch)->doTask();

        // --- Step 2: Prepare NextHopGroupKey ---
        NextHopGroupKey nhg_key("10.0.0.4,10.0.0.5,10.0.0.6");

        // --- Step 3: Capture programmed nexthop IDs ---
        std::set<sai_object_id_t> programmed_nh_oids;

        EXPECT_CALL(*mock_sai_route_api,
                    create_route_entries(::testing::_, ::testing::_, ::testing::_, ::testing::_, ::testing::_, ::testing::_))
            .WillRepeatedly(::testing::Invoke(
                [&](uint32_t object_count,
                    const sai_route_entry_t * /*route_entries*/,
                    const uint32_t *attr_count,
                    const sai_attribute_t **attr_list,
                    sai_bulk_op_error_mode_t /*mode*/,
                    sai_status_t *object_statuses) -> sai_status_t
                {
                    for (uint32_t i = 0; i < object_count; ++i)
                    {
                        for (uint32_t j = 0; j < attr_count[i]; ++j)
                        {
                            if (attr_list[i][j].id == SAI_ROUTE_ENTRY_ATTR_NEXT_HOP_ID)
                            {
                                // Mark the NEXT_HOP_ID as programmed
                                programmed_nh_oids.insert(attr_list[i][j].value.oid);
                            }
                        }
                        // **Simulate success** so addTempRoute thinks the route was installed
                        object_statuses[i] = SAI_STATUS_SUCCESS;
                    }
                    return SAI_STATUS_SUCCESS;
                }));

        // --- Step 4: Run 100 iterations ---
        constexpr int kIterations = 100;
        for (int i = 0; i < kIterations; ++i)
        {
            RouteBulkContext ctx("3.3.3.0/24", true);
            ctx.vrf_id = gVirtualRouterId;
            ctx.ip_prefix = IpPrefix("3.3.3.0/24");
            gRouteOrch->addTempRoute(ctx, nhg_key);

            // Flush the bulker to trigger SAI API calls
            gRouteOrch->gRouteBulker.flush();
        }

        // --- Step 5: Verify at least 3 distinct next hops were picked ---
        ASSERT_GE(programmed_nh_oids.size(), 3u);

    }

    TEST_F(RouteOrchTest, NhgOrchTempNhgUniformSelection)
    {
        // Test NhgOrch::createTempNhg() randomization
        // This covers the MT19937 randomization code in nhgorch.cpp

        // --- Step 1: Setup resolved neighbors ---
        Table neighborTable(m_app_db.get(), APP_NEIGH_TABLE_NAME);

        std::map<std::string, std::string> neighborIp2Mac = {
            {"10.0.0.7", "00:00:0a:00:00:07"},
            {"10.0.0.8", "00:00:0a:00:00:08"},
            {"10.0.0.9", "00:00:0a:00:00:09"}
        };

        neighborTable.set("Ethernet0:10.0.0.7", {{"neigh", neighborIp2Mac["10.0.0.7"]}, {"family", "IPv4"}});
        neighborTable.set("Ethernet0:10.0.0.8", {{"neigh", neighborIp2Mac["10.0.0.8"]}, {"family", "IPv4"}});
        neighborTable.set("Ethernet0:10.0.0.9", {{"neigh", neighborIp2Mac["10.0.0.9"]}, {"family", "IPv4"}});

        gNeighOrch->addExistingData(&neighborTable);
        static_cast<Orch *>(gNeighOrch)->doTask();

        // --- Step 2: Create NextHopGroupKey with 3 next hops ---
        NextHopGroupKey nhg_key("10.0.0.7,10.0.0.8,10.0.0.9");

        // --- Step 3: Call createTempNhg() 100 times and collect selected NHs ---
        std::set<std::string> selected_nhs;
        constexpr int kIterations = 100;
        
        for (int i = 0; i < kIterations; ++i)
        {
            NextHopGroup temp_nhg = gNhgOrch->createTempNhg(nhg_key);
            
            // Get the single NH that was selected
            const auto nhs = temp_nhg.getNhgKey().getNextHops();
            ASSERT_EQ(nhs.size(), 1u); // Temp NHG should have exactly one NH
            
            // Record which NH was selected
            selected_nhs.insert(nhs.begin()->to_string());
        }

        // --- Step 4: Verify at least 3 distinct next hops were selected ---
        ASSERT_GE(selected_nhs.size(), 3u) 
            << "Expected all 3 next hops to be selected at least once across " 
            << kIterations << " iterations, but only " << selected_nhs.size() 
            << " were selected";
    }

    TEST_F(RouteOrchTest, RouteOrch_AddDeleteIPv6)
    {
        // Add IPv6 interface IPs (like the pytest does) and an IPv6 neighbor.
        {
            Table intfTable(m_app_db.get(), APP_INTF_TABLE_NAME);
            intfTable.set("Ethernet0:2000::1/64", { {"scope","global"}, {"family","IPv6"} });
            intfTable.set("Ethernet4:2001::1/64", { {"scope","global"}, {"family","IPv6"} });
            gIntfsOrch->addExistingData(&intfTable);
            static_cast<Orch *>(gIntfsOrch)->doTask();

            Table neighborTable(m_app_db.get(), APP_NEIGH_TABLE_NAME);
            neighborTable.set("Ethernet0:2000::2", { {"neigh","00:00:00:00:00:22"}, {"family","IPv6"} });
            gNeighOrch->addExistingData(&neighborTable);
            static_cast<Orch *>(gNeighOrch)->doTask();
        }

        auto *routeConsumer = dynamic_cast<Consumer *>(gRouteOrch->getExecutor(APP_ROUTE_TABLE_NAME));
        ASSERT_NE(routeConsumer, nullptr);

        // PART A: Add/Remove IPv6 prefix 3000::/64 via 2000::2 on Ethernet0
        {
            std::deque<KeyOpFieldsValuesTuple> entries;
            entries.push_back({ "3000::/64", "SET",
                            { {"ifname","Ethernet0"}, {"nexthop","2000::2"} }});
            routeConsumer->addToSync(entries);

            auto base_create = create_route_count;
            auto base_set    = set_route_count;
            auto base_remove = remove_route_count;

            static_cast<Orch *>(gRouteOrch)->doTask();

            // Expect CREATE +1 for new route, no SET/REMOVE yet
            ASSERT_EQ(base_create + 1, create_route_count);
            ASSERT_EQ(base_set,        set_route_count);
            ASSERT_EQ(base_remove,     remove_route_count);

            // Remove the route
            entries.clear();
            entries.push_back({ "3000::/64", "DEL", {} });
            routeConsumer->addToSync(entries);

            base_create = create_route_count;
            base_set    = set_route_count;
            base_remove = remove_route_count;

            static_cast<Orch *>(gRouteOrch)->doTask();

            // Expect REMOVE +1, create/set unchanged
            ASSERT_EQ(base_create,         create_route_count);
            ASSERT_EQ(base_set,            set_route_count);
            ASSERT_EQ(base_remove + 1,     remove_route_count);
        }

        // PART B: IPv6 default route (::/0): SET to add (state -> ok), DEL to remove (state -> na) 
        {
            const std::string def6 = "::/0";
            const bool hasStateField = stateRouteStateFieldExists(m_state_db.get(), def6);

            // Add default v6 route (::/0) via SET path
            std::deque<KeyOpFieldsValuesTuple> entries;
            entries.push_back({ def6, "SET", { {"ifname","Ethernet0"}, {"nexthop","2000::2"} }});
            routeConsumer->addToSync(entries);

            auto base_create = create_route_count;
            auto base_set    = set_route_count;
            auto base_remove = remove_route_count;

            static_cast<Orch *>(gRouteOrch)->doTask();

            // Default route typically programs via attribute SET (no create/remove)
            ASSERT_EQ(base_create,         create_route_count);
            ASSERT_EQ(base_remove,         remove_route_count);
            ASSERT_EQ(base_set + 1,        set_route_count);
            ASSERT_EQ(sai_fail_count, 0);

            if (hasStateField)
            {
                ASSERT_TRUE(waitStateRouteState(m_state_db.get(), def6, "ok"))
                    << "Expected IPv6 default-route state to become 'ok' after SET.";
            }

            // Now delete the default v6 route
            entries.clear();
            entries.push_back({ def6, "DEL", {} });
            routeConsumer->addToSync(entries);

            base_create = create_route_count;
            base_set    = set_route_count;
            base_remove = remove_route_count;

            static_cast<Orch *>(gRouteOrch)->doTask();

            // Expect another SET (no create/remove), and no invalid SAI programming
            ASSERT_EQ(base_create,         create_route_count);
            ASSERT_EQ(base_remove,         remove_route_count);
            ASSERT_EQ(base_set + 1,        set_route_count);
            ASSERT_EQ(sai_fail_count, 0);

            if (hasStateField)
            {
                ASSERT_TRUE(waitStateRouteState(m_state_db.get(), def6, "na"))
                    << "Expected IPv6 default-route state to become 'na' after DEL.";
            }
        }
    }

    TEST_F(RouteOrchTest, RouteOrch_AddDeleteIPv4)
    {
        auto *routeConsumer = dynamic_cast<Consumer *>(gRouteOrch->getExecutor(APP_ROUTE_TABLE_NAME));
        ASSERT_NE(routeConsumer, nullptr);

        // PART A: Regular prefix add/remove (2.2.2.0/24)
        {
            std::deque<KeyOpFieldsValuesTuple> entries;
            entries.push_back({ "2.2.2.0/24", "SET",
                            { {"ifname","Ethernet0"}, {"nexthop","10.0.0.2"} }});
            routeConsumer->addToSync(entries);

            auto base_create = create_route_count;
            auto base_set    = set_route_count;
            auto base_remove = remove_route_count;

            static_cast<Orch *>(gRouteOrch)->doTask();

            // Expect create +1, set unchanged, remove unchanged
            ASSERT_EQ(base_create + 1, create_route_count);
            ASSERT_EQ(base_set,        set_route_count);
            ASSERT_EQ(base_remove,     remove_route_count);

            // Now remove the route
            entries.clear();
            entries.push_back({ "2.2.2.0/24", "DEL", {} });
            routeConsumer->addToSync(entries);

            base_create = create_route_count;
            base_set    = set_route_count;
            base_remove = remove_route_count;

            static_cast<Orch *>(gRouteOrch)->doTask();

            // Expect remove +1, create/set unchanged
            ASSERT_EQ(base_create,         create_route_count);
            ASSERT_EQ(base_set,            set_route_count);
            ASSERT_EQ(base_remove + 1,     remove_route_count);
        }

        // PART B: Default route DEL -> state 'na' -> SET -> state 'ok'
        {
            const std::string def = "0.0.0.0/0";
            ASSERT_TRUE(stateRouteStateFieldExists(m_state_db.get(), def))
        << "Expected STATE_DB:ROUTE_TABLE to expose 'state' for the default route.";

            // SetUp() seeds a default route; if state is exposed, it should become 'ok'
        
            ASSERT_TRUE(waitStateRouteState(m_state_db.get(), def, "ok"))
                << "Expected initial default-route state to become 'ok'.";
        

            // DEL default route
            std::deque<KeyOpFieldsValuesTuple> entries;
            entries.push_back({ def, "DEL", {} });
            routeConsumer->addToSync(entries);

            auto base_create = create_route_count;
            auto base_set    = set_route_count;
            auto base_remove = remove_route_count;

            static_cast<Orch *>(gRouteOrch)->doTask();

            // For default route, expect attribute SET path (no create/remove), set +1
            ASSERT_EQ(base_create,         create_route_count);
            ASSERT_EQ(base_remove,         remove_route_count);
            ASSERT_EQ(base_set + 1,        set_route_count);
            ASSERT_EQ(sai_fail_count, 0);

            ASSERT_TRUE(waitStateRouteState(m_state_db.get(), def, "na"))
                    << "Expected default-route state to become 'na' after DEL.";
            

            // Re-SET default route
            entries.clear();
            entries.push_back({ def, "SET", { {"ifname","Ethernet0"}, {"nexthop","10.0.0.2"} }});
            routeConsumer->addToSync(entries);

            base_create = create_route_count;
            base_set    = set_route_count;
            base_remove = remove_route_count;

            static_cast<Orch *>(gRouteOrch)->doTask();

            // Expect another SET (no create/remove)
            ASSERT_EQ(base_create,         create_route_count);
            ASSERT_EQ(base_remove,         remove_route_count);
            ASSERT_EQ(base_set + 1,        set_route_count);
            ASSERT_EQ(sai_fail_count, 0);

            ASSERT_TRUE(waitStateRouteState(m_state_db.get(), def, "ok"))
                    << "Expected default-route state to return to 'ok' after re-SET.";
        
        }
    }

    TEST_F(RouteOrchTest, RouteOrchTestDelSetSameNexthop)
    {
        std::deque<KeyOpFieldsValuesTuple> entries;

        // Setting route with same next hop but after a DEL in the same bulk
        entries.push_back({"1.1.1.0/24", "DEL", { {} }});
        entries.push_back({"1.1.1.0/24", "SET", { {"ifname", "Ethernet0"},
                                                  {"nexthop", "10.0.0.2"}}});
        auto consumer = dynamic_cast<Consumer *>(gRouteOrch->getExecutor(APP_ROUTE_TABLE_NAME));
        consumer->addToSync(entries);
        auto current_create_count = create_route_count;
        auto current_remove_count = remove_route_count;
        auto current_set_count = set_route_count;

        static_cast<Orch *>(gRouteOrch)->doTask();
        // Make sure both create and set has been called
        ASSERT_EQ(current_create_count + 1, create_route_count);
        ASSERT_EQ(current_remove_count + 1, remove_route_count);
        ASSERT_EQ(current_set_count, set_route_count);

        entries.clear();

        // Make sure SAI API won't be called if setting it for second time with the same next hop
        entries.push_back({"1.1.1.0/24", "SET", { {"ifname", "Ethernet0"},
                                                  {"nexthop", "10.0.0.2"}}});
        consumer = dynamic_cast<Consumer *>(gRouteOrch->getExecutor(APP_ROUTE_TABLE_NAME));
        consumer->addToSync(entries);
        current_create_count = create_route_count;
        current_remove_count = remove_route_count;
        current_set_count = set_route_count;

        static_cast<Orch *>(gRouteOrch)->doTask();
        // Make sure both create and set has been called
        ASSERT_EQ(current_create_count, create_route_count);
        ASSERT_EQ(current_remove_count, remove_route_count);
        ASSERT_EQ(current_set_count, set_route_count);
    }

    TEST_F(RouteOrchTest, RouteOrchTestDelSetDiffNexthop)
    {
        std::deque<KeyOpFieldsValuesTuple> entries;
        entries.push_back({"1.1.1.0/24", "DEL", { {} }});
        entries.push_back({"1.1.1.0/24", "SET", { {"ifname", "Ethernet0"},
                                                  {"nexthop", "10.0.0.3"}}});

        auto consumer = dynamic_cast<Consumer *>(gRouteOrch->getExecutor(APP_ROUTE_TABLE_NAME));
        consumer->addToSync(entries);
        auto current_create_count = create_route_count;
        auto current_remove_count = remove_route_count;
        auto current_set_count = set_route_count;

        static_cast<Orch *>(gRouteOrch)->doTask();
        // Make sure both create and remove has been called
        ASSERT_EQ(current_create_count + 1, create_route_count);
        ASSERT_EQ(current_remove_count + 1, remove_route_count);
        ASSERT_EQ(current_set_count, set_route_count);
    }

    TEST_F(RouteOrchTest, RouteOrchTestDelSetDefaultRoute)
    {
        std::deque<KeyOpFieldsValuesTuple> entries;
        entries.push_back({"0.0.0.0/0", "DEL", { {} }});
        entries.push_back({"0.0.0.0/0", "SET", { {"ifname", "Ethernet0"},
                                                  {"nexthop", "10.0.0.3"}}});

        auto consumer = dynamic_cast<Consumer *>(gRouteOrch->getExecutor(APP_ROUTE_TABLE_NAME));
        consumer->addToSync(entries);
        auto current_create_count = create_route_count;
        auto current_remove_count = remove_route_count;
        auto current_set_count = set_route_count;

        static_cast<Orch *>(gRouteOrch)->doTask();
        // Make sure both create and set has been called
        ASSERT_EQ(current_create_count, create_route_count);
        ASSERT_EQ(current_remove_count, remove_route_count);
        ASSERT_EQ(current_set_count + 1, set_route_count);
        ASSERT_EQ(sai_fail_count, 0);
    }

    TEST_F(RouteOrchTest, RouteOrchTestSetDelResponse)
    {
        gEnableFibSuppress = true;
        gMockResponsePublisher = std::make_unique<MockResponsePublisher>();

        std::deque<KeyOpFieldsValuesTuple> entries;
        std::string key = "2.2.2.0/24";
        std::vector<FieldValueTuple> fvs{{"ifname", "Ethernet0,Ethernet0"}, {"nexthop", "10.0.0.2,10.0.0.3"}, {"protocol", "bgp"}};
        entries.push_back({key, "SET", fvs});

        auto consumer = dynamic_cast<Consumer *>(gRouteOrch->getExecutor(APP_ROUTE_TABLE_NAME));
        consumer->addToSync(entries);

        EXPECT_CALL(*gMockResponsePublisher, publish(APP_ROUTE_TABLE_NAME, key, std::vector<FieldValueTuple>{{"protocol", "bgp"}}, ReturnCode(SAI_STATUS_SUCCESS), false)).Times(1);
        static_cast<Orch *>(gRouteOrch)->doTask();

        // add entries again to the consumer queue (in case of rapid DEL/SET operations from fpmsyncd, routeorch just gets the last SET update)
        consumer->addToSync(entries);

        EXPECT_CALL(*gMockResponsePublisher, publish(APP_ROUTE_TABLE_NAME, key, std::vector<FieldValueTuple>{{"protocol", "bgp"}}, ReturnCode(SAI_STATUS_SUCCESS), false)).Times(1);
        static_cast<Orch *>(gRouteOrch)->doTask();

        entries.clear();

        // Route deletion

        entries.clear();
        entries.push_back({key, "DEL", {}});

        consumer->addToSync(entries);

        EXPECT_CALL(*gMockResponsePublisher, publish(APP_ROUTE_TABLE_NAME, key, std::vector<FieldValueTuple>{}, ReturnCode(SAI_STATUS_SUCCESS), false)).Times(1);
        static_cast<Orch *>(gRouteOrch)->doTask();

        gMockResponsePublisher.reset();
    }

    TEST_F(RouteOrchTest, RouteOrchSetFullMaskSubnetPrefix)
    {
        gEnableFibSuppress = true;
        gMockResponsePublisher = std::make_unique<MockResponsePublisher>();

        std::deque<KeyOpFieldsValuesTuple> entries;
        std::string key = "11.0.0.1/32";
        std::vector<FieldValueTuple> fvs{{"ifname", "Ethernet4"}, {"nexthop", "0.0.0.0"}, {"protocol", "bgp"}};
        entries.push_back({key, "SET", fvs});

        auto consumer = dynamic_cast<Consumer *>(gRouteOrch->getExecutor(APP_ROUTE_TABLE_NAME));
        consumer->addToSync(entries);

        EXPECT_CALL(*gMockResponsePublisher, publish(APP_ROUTE_TABLE_NAME, key, std::vector<FieldValueTuple>{{"protocol", "bgp"}}, ReturnCode(SAI_STATUS_SUCCESS), false)).Times(1);
        static_cast<Orch *>(gRouteOrch)->doTask();

        gMockResponsePublisher.reset();
    }

    TEST_F(RouteOrchTest, RouteOrchLoopbackRoute)
    {
        gEnableFibSuppress = true;
        gMockResponsePublisher = std::make_unique<MockResponsePublisher>();

        std::deque<KeyOpFieldsValuesTuple> entries;
        std::string key = "fc00:1::/64";
        std::vector<FieldValueTuple> fvs{{"ifname", "Loopback"}, {"nexthop", "::"}, {"protocol", "static"}};
        entries.push_back({key, "SET", fvs});

        auto consumer = dynamic_cast<Consumer *>(gRouteOrch->getExecutor(APP_ROUTE_TABLE_NAME));
        consumer->addToSync(entries);

        EXPECT_CALL(*gMockResponsePublisher, publish(APP_ROUTE_TABLE_NAME, key, std::vector<FieldValueTuple>{{"protocol", "static"}}, ReturnCode(SAI_STATUS_SUCCESS), false)).Times(1);
        static_cast<Orch *>(gRouteOrch)->doTask();

        gMockResponsePublisher.reset();
    }

    TEST_F(RouteOrchTest, RouteOrchTestInvalidEvpnRoute)
    {
        std::deque<KeyOpFieldsValuesTuple> entries;
        entries.push_back({"Vrf1", "SET", { {"vni", "500100"}, {"v4", "true"}}});
        auto consumer = dynamic_cast<Consumer *>(gVrfOrch->getExecutor(APP_VRF_TABLE_NAME));
        consumer->addToSync(entries);
        static_cast<Orch *>(gVrfOrch)->doTask();

        entries.clear();
        entries.push_back({"Vrf1:1.1.1.0/24", "SET", { {"ifname", "Ethernet0,Ethernet0"},
                                                  {"nexthop", "10.0.0.2,10.0.0.3"},
                                                  {"vni_label", "500100"},
                                                  {"router_mac", "7e:f0:c0:e4:b2:5a,7e:f0:c0:e4:b2:5b"}}});
        entries.push_back({"Vrf1:2.1.1.0/24", "SET", { {"ifname", "Ethernet0,Ethernet0"},
                                                  {"nexthop", "10.0.0.2,10.0.0.3"},
                                                  {"vni_label", "500100,500100"},
                                                  {"router_mac", "7e:f0:c0:e4:b2:5b"}}});
        consumer = dynamic_cast<Consumer *>(gRouteOrch->getExecutor(APP_ROUTE_TABLE_NAME));
        consumer->addToSync(entries);

        auto current_create_count = create_route_count;
        auto current_set_count = set_route_count;

        static_cast<Orch *>(gRouteOrch)->doTask();
        ASSERT_EQ(current_create_count, create_route_count);
        ASSERT_EQ(current_set_count, set_route_count);
    }

    TEST_F(RouteOrchTest, RouteOrchTestVrfRoute)
    {
        std::deque<KeyOpFieldsValuesTuple> entries;
        entries.push_back({"Vrf2", "SET", { {"vni", "500200"}}});
        auto vrfConsumer = dynamic_cast<Consumer *>(gVrfOrch->getExecutor(APP_VRF_TABLE_NAME));
        vrfConsumer->addToSync(entries);
        static_cast<Orch *>(gVrfOrch)->doTask();
        entries.clear();
        entries.push_back({"Ethernet8", "SET", { {"vrf_name", "Vrf2"}}});
        auto intfConsumer = dynamic_cast<Consumer *>(gIntfsOrch->getExecutor(APP_INTF_TABLE_NAME));
        intfConsumer->addToSync(entries);
        static_cast<Orch *>(gIntfsOrch)->doTask();
        auto routeConsumer = dynamic_cast<Consumer *>(gRouteOrch->getExecutor(APP_ROUTE_TABLE_NAME));
        entries.clear();
        entries.push_back({"Vrf2:fe80::/64", "DEL", {}});
        entries.push_back({"Vrf2:20.0.0.0/24", "DEL", {}});
        entries.push_back({"Vrf2:fe80::/64", "SET", { {"protocol", "kernel"},
                                                      {"nexthop", "::"},
                                                      {"ifname", "Ethernet8"}}});
        entries.push_back({"Vrf2:20.0.0.0/24", "SET", { {"protocol", "kernel"},
                                                        {"nexthop", "0.0.0.0"},
                                                        {"ifname", "Ethernet8"}}});
        routeConsumer->addToSync(entries);
        static_cast<Orch *>(gRouteOrch)->doTask();
    }

    /* A bulk that removes the last route of a VRF and adds another route to the
     * same VRF must record the new route, so that a later DEL removes it. */
    TEST_F(RouteOrchTest, RouteOrchVrfLastRouteDelAndSetSameBulk)
    {
        std::deque<KeyOpFieldsValuesTuple> entries;
        entries.push_back({"Vrf2", "SET", { {"vni", "500200"}}});
        auto vrfConsumer = dynamic_cast<Consumer *>(gVrfOrch->getExecutor(APP_VRF_TABLE_NAME));
        vrfConsumer->addToSync(entries);
        static_cast<Orch *>(gVrfOrch)->doTask();
        entries.clear();
        entries.push_back({"Ethernet8", "SET", { {"vrf_name", "Vrf2"}}});
        auto intfConsumer = dynamic_cast<Consumer *>(gIntfsOrch->getExecutor(APP_INTF_TABLE_NAME));
        intfConsumer->addToSync(entries);
        static_cast<Orch *>(gIntfsOrch)->doTask();

        sai_object_id_t vrf_id = gVrfOrch->getVRFid("Vrf2");
        IpPrefix v4_prefix("20.0.0.0/24");
        IpPrefix v6_prefix("fc00:1::/64");

        auto routeConsumer = dynamic_cast<Consumer *>(gRouteOrch->getExecutor(APP_ROUTE_TABLE_NAME));
        entries.clear();
        entries.push_back({"Vrf2:20.0.0.0/24", "SET", { {"protocol", "kernel"},
                                                        {"nexthop", "0.0.0.0"},
                                                        {"ifname", "Ethernet8"}}});
        routeConsumer->addToSync(entries);
        static_cast<Orch *>(gRouteOrch)->doTask();
        ASSERT_EQ(gRouteOrch->m_syncdRoutes.count(vrf_id), 1u);
        ASSERT_EQ(gRouteOrch->m_syncdRoutes.at(vrf_id).size(), 1u);
        ASSERT_EQ(gRouteOrch->m_syncdRoutes.at(vrf_id).count(v4_prefix), 1u);
        int vrf_ref_count = gVrfOrch->getVrfRefCount("Vrf2");

        /* The DEL sorts before the SET, so its post processing runs first. */
        auto create_count = create_route_count;
        auto remove_count = remove_route_count;
        entries.clear();
        entries.push_back({"Vrf2:20.0.0.0/24", "DEL", {}});
        entries.push_back({"Vrf2:fc00:1::/64", "SET", { {"protocol", "kernel"},
                                                        {"nexthop", "::"},
                                                        {"ifname", "Ethernet8"}}});
        routeConsumer->addToSync(entries);
        static_cast<Orch *>(gRouteOrch)->doTask();
        ASSERT_TRUE(routeConsumer->m_toSync.empty());
        ASSERT_EQ(create_route_count, create_count + 1);
        ASSERT_EQ(remove_route_count, remove_count + 1);
        ASSERT_EQ(gRouteOrch->m_syncdRoutes.count(vrf_id), 1u);
        ASSERT_EQ(gRouteOrch->m_syncdRoutes.at(vrf_id).size(), 1u);
        ASSERT_EQ(gRouteOrch->m_syncdRoutes.at(vrf_id).count(v6_prefix), 1u);
        ASSERT_EQ(gVrfOrch->getVrfRefCount("Vrf2"), vrf_ref_count);

        entries.clear();
        entries.push_back({"Vrf2:fc00:1::/64", "DEL", {}});
        routeConsumer->addToSync(entries);
        static_cast<Orch *>(gRouteOrch)->doTask();
        ASSERT_TRUE(routeConsumer->m_toSync.empty());
        ASSERT_EQ(remove_route_count, remove_count + 2);
        ASSERT_EQ(gRouteOrch->m_syncdRoutes.count(vrf_id), 0u);
        ASSERT_EQ(gVrfOrch->getVrfRefCount("Vrf2"), vrf_ref_count - 1);
    }

    /* Tests SAI_STATUS_ITEM_NOT_FOUND error handling for setting route */
    TEST_F(RouteOrchTest, RouteOrchSetItemNotFound)
    {
        IpPrefix prefix("1.1.1.0/32");
        NextHopGroupKey nhg_key("10.0.0.2");
        RouteNhg route_nhg(nhg_key, "");

        gRouteOrch->m_syncdRoutes[gVirtualRouterId][prefix] = route_nhg;

        std::deque<KeyOpFieldsValuesTuple> entries;
        entries.push_back({"1.1.1.0/32", "SET", { {"ifname", "Ethernet0"},
                                                  {"nexthop", "10.0.0.3"}}});

        auto consumer = dynamic_cast<Consumer *>(gRouteOrch->getExecutor(APP_ROUTE_TABLE_NAME));
        consumer->addToSync(entries);

        std::vector<sai_status_t> exp_status{SAI_STATUS_ITEM_NOT_FOUND};
        EXPECT_CALL(*mock_sai_route_api, set_route_entries_attribute)
            .WillOnce(DoAll(SetArrayArgument<4>(exp_status.begin(), exp_status.end()), Return(SAI_STATUS_ITEM_NOT_FOUND)));
        static_cast<Orch *>(gRouteOrch)->doTask();

        exp_status = {SAI_STATUS_SUCCESS};
        EXPECT_CALL(*mock_sai_route_api, create_route_entries)
            .WillOnce(DoAll(SetArrayArgument<5>(exp_status.begin(), exp_status.end()), Return(SAI_STATUS_SUCCESS)));
        static_cast<Orch *>(gRouteOrch)->doTask();
    }

    /* Test default route DEL followed by SET scenario to verify bulker state handling */
    TEST_F(RouteOrchTest, RouteOrchTestDefaultRouteDelSetBulkerState)
    {
        // This test verifies the fix for the default route race condition where:
        // 1. A DEL event occurs and automatically adds a DROP action (creating a setting_entry in bulker)
        // 2. A subsequent SET operation needs to check for both pending removals AND pending sets
        // 3. The bulk_entry_pending_removal_or_set() method should detect the pending operation

        std::deque<KeyOpFieldsValuesTuple> entries;

        // First, delete the default route (0.0.0.0/0) that was set up in SetUp()
        // This simulates a scenario where the default route is being removed
        entries.push_back({"0.0.0.0/0", "DEL", {}});

        auto consumer = dynamic_cast<Consumer *>(gRouteOrch->getExecutor(APP_ROUTE_TABLE_NAME));
        consumer->addToSync(entries);

        auto current_create_count = create_route_count;
        auto current_remove_count = remove_route_count;
        auto current_set_count = set_route_count;
        auto current_drop_set_count = drop_set_count;

        // Process the DEL operation
        static_cast<Orch *>(gRouteOrch)->doTask();

        // Verify that remove translated to a set
        ASSERT_EQ(current_create_count, create_route_count);
        ASSERT_EQ(current_remove_count, remove_route_count);
        ASSERT_EQ(current_set_count + 1, set_route_count);

        // Verify that we set a DROP action
        ASSERT_EQ(current_drop_set_count + 1, drop_set_count);

        // Now immediately SET the default route with a new nexthop
        // This simulates a rapid DEL/SET sequence that can happen in production
        entries.clear();
        entries.push_back({"0.0.0.0/0", "SET", { {"ifname", "Ethernet0"},
                                                  {"nexthop", "10.0.0.3"}}});

        consumer->addToSync(entries);
        current_create_count = create_route_count;
        current_remove_count = remove_route_count;
        current_set_count = set_route_count;
        current_drop_set_count = drop_set_count;

        // Process the SET operation
        static_cast<Orch *>(gRouteOrch)->doTask();

        // Verify that create was not called for the new route, instead orchagent
        // would only set the pre-existing route
        ASSERT_EQ(current_create_count, create_route_count);
        ASSERT_EQ(current_remove_count, remove_route_count);
        ASSERT_EQ(current_set_count + 1, set_route_count);
        // Verify that we do not set a DROP action
        ASSERT_EQ(current_drop_set_count, drop_set_count);

        // Verify the bulker state is clean after processing
        // The bulker should have flushed all pending operations
        ASSERT_EQ(gRouteOrch->gRouteBulker.creating_entries_count(), 0);
        ASSERT_EQ(gRouteOrch->gRouteBulker.setting_entries_count(), 0);
        ASSERT_EQ(gRouteOrch->gRouteBulker.removing_entries_count(), 0);
    }

    /* Test default route DEL and SET in same bulk operation */
    TEST_F(RouteOrchTest, RouteOrchTestDefaultRouteDelSetSameBulk)
    {
        // This test verifies that when DEL and SET for default route come in the same bulk,
        // the bulker correctly handles the pending operations using bulk_entry_pending_removal_or_set()

        std::deque<KeyOpFieldsValuesTuple> entries;

        // Add both DEL and SET for default route in the same bulk
        entries.push_back({"0.0.0.0/0", "DEL", {}});
        entries.push_back({"0.0.0.0/0", "SET", { {"ifname", "Ethernet0"},
                                                  {"nexthop", "10.0.0.3"}}});

        auto consumer = dynamic_cast<Consumer *>(gRouteOrch->getExecutor(APP_ROUTE_TABLE_NAME));
        consumer->addToSync(entries);

        auto current_create_count = create_route_count;
        auto current_remove_count = remove_route_count;
        auto current_set_count = set_route_count;
        auto current_drop_set_count = drop_set_count;

        // Process both operations in one doTask() call
        static_cast<Orch *>(gRouteOrch)->doTask();

        // Verify that set was called
        ASSERT_EQ(current_create_count, create_route_count);
        ASSERT_EQ(current_remove_count, remove_route_count);
        ASSERT_EQ(current_set_count + 1, set_route_count);
        // Verify that we do not set a DROP action
        ASSERT_EQ(current_drop_set_count, drop_set_count);

        // Verify the bulker state is clean after processing
        ASSERT_EQ(gRouteOrch->gRouteBulker.creating_entries_count(), 0);
        ASSERT_EQ(gRouteOrch->gRouteBulker.setting_entries_count(), 0);
        ASSERT_EQ(gRouteOrch->gRouteBulker.removing_entries_count(), 0);
    }

    /* Test IPv6 default route DEL followed by SET */
    TEST_F(RouteOrchTest, RouteOrchTestIPv6DefaultRouteDelSet)
    {
        // Test the same scenario with IPv6 default route (::/0)
        // to ensure the fix works for both address families

        std::deque<KeyOpFieldsValuesTuple> entries;

        // First, create an IPv6 default route
        entries.push_back({"::/0", "SET", { {"ifname", "Ethernet0"},
                                            {"nexthop", "fc00::2"}}});

        auto consumer = dynamic_cast<Consumer *>(gRouteOrch->getExecutor(APP_ROUTE_TABLE_NAME));
        consumer->addToSync(entries);

        auto current_create_count = create_route_count;
        auto current_remove_count = remove_route_count;
        auto current_set_count = set_route_count;
        auto current_drop_set_count = drop_set_count;

        // Process the initial SET
        static_cast<Orch *>(gRouteOrch)->doTask();

        ASSERT_EQ(current_create_count, create_route_count);
        ASSERT_EQ(current_remove_count, remove_route_count);
        ASSERT_EQ(current_set_count, set_route_count);
        // Verify that we do not set a DROP action
        ASSERT_EQ(current_drop_set_count, drop_set_count);

        // Now test DEL followed by SET
        entries.clear();
        entries.push_back({"::/0", "DEL", {}});
        entries.push_back({"::/0", "SET", { {"ifname", "Ethernet0"},
                                            {"nexthop", "fc00::3"}}});

        consumer->addToSync(entries);
        current_create_count = create_route_count;
        current_remove_count = remove_route_count;
        current_set_count = set_route_count;
        current_drop_set_count = drop_set_count;

        // Process both operations
        static_cast<Orch *>(gRouteOrch)->doTask();

        // Verify that both remove and create were called
        ASSERT_EQ(current_remove_count, remove_route_count);
        ASSERT_EQ(current_create_count, create_route_count);
        ASSERT_EQ(current_set_count + 1, set_route_count);
        // Verify that DROP action happens. This is because the nexthop used
        // ("fc00::3") is not known to m_neighOrch.
        ASSERT_EQ(current_drop_set_count + 1, drop_set_count);

        // Verify the bulker state is clean
        ASSERT_EQ(gRouteOrch->gRouteBulker.creating_entries_count(), 0);
        ASSERT_EQ(gRouteOrch->gRouteBulker.setting_entries_count(), 0);
        ASSERT_EQ(gRouteOrch->gRouteBulker.removing_entries_count(), 0);
    }

    TEST_F(RouteOrchTest, RouteOrchReachMaxNhgLimit)
    {
        // Test that covers the SWSS_LOG_INFO when NHG limit is reached (line 1488)
        
        // --- Step 1: Setup neighbors for ECMP route ---
        Table neighborTable(m_app_db.get(), APP_NEIGH_TABLE_NAME);
        neighborTable.set("Ethernet0:10.0.0.20", {{"neigh", "00:00:0a:00:00:14"}, {"family", "IPv4"}});
        neighborTable.set("Ethernet0:10.0.0.21", {{"neigh", "00:00:0a:00:00:15"}, {"family", "IPv4"}});
        
        gNeighOrch->addExistingData(&neighborTable);
        static_cast<Orch *>(gNeighOrch)->doTask();
        
        // --- Step 2: Artificially lower the max NHG count to current count ---
        // This will force the next ECMP route creation to hit the limit
        auto current_count = gRouteOrch->m_nextHopGroupCount + NhgOrch::getSyncedNhgCount();
        auto saved_max = gRouteOrch->m_maxNextHopGroupCount;
        gRouteOrch->m_maxNextHopGroupCount = current_count;
        
        // --- Step 3: Try to create an ECMP route (should fail and log) ---
        auto consumer = dynamic_cast<Consumer *>(gRouteOrch->getExecutor(APP_ROUTE_TABLE_NAME));
        std::deque<KeyOpFieldsValuesTuple> entries;
        entries.push_back({"5.5.5.0/24", "SET", {
            {"ifname", "Ethernet0,Ethernet0"},
            {"nexthop", "10.0.0.20,10.0.0.21"}
        }});
        consumer->addToSync(entries);
        
        // This should trigger the log at line 1488
        static_cast<Orch *>(gRouteOrch)->doTask();
        
        // --- Step 4: Restore the original max count ---
        gRouteOrch->m_maxNextHopGroupCount = saved_max;
    }

    TEST_F(RouteOrchTest, RouteOrchTestTempRouteDesiredNhgKeyAssignment)
    {
        // Test that desired_nhg_key is properly assigned when a temp route is created
        
        // --- Step 1: Setup resolved neighbors ---
        Table neighborTable(m_app_db.get(), APP_NEIGH_TABLE_NAME);

        neighborTable.set("Ethernet0:10.0.0.10", {{"neigh", "00:00:0a:00:00:0a"}, {"family", "IPv4"}});
        neighborTable.set("Ethernet0:10.0.0.11", {{"neigh", "00:00:0a:00:00:0b"}, {"family", "IPv4"}});

        gNeighOrch->addExistingData(&neighborTable);
        static_cast<Orch *>(gNeighOrch)->doTask();

        // --- Step 2: Create a temp route with multiple nexthops ---
        NextHopGroupKey desired_nhg("10.0.0.10,10.0.0.11");
        
        RouteBulkContext ctx("5.5.5.0/24", true);
        ctx.vrf_id = gVirtualRouterId;
        ctx.ip_prefix = IpPrefix("5.5.5.0/24");
        ctx.nhg = desired_nhg;

        // Mock SAI to always succeed
        EXPECT_CALL(*mock_sai_route_api,
                    create_route_entries(::testing::_, ::testing::_, ::testing::_, ::testing::_, ::testing::_, ::testing::_))
            .WillOnce(::testing::Invoke(
                [](uint32_t object_count,
                   const sai_route_entry_t * /*route_entries*/,
                   const uint32_t * /*attr_count*/,
                   const sai_attribute_t ** /*attr_list*/,
                   sai_bulk_op_error_mode_t /*mode*/,
                   sai_status_t *object_statuses) -> sai_status_t
                {
                    for (uint32_t i = 0; i < object_count; ++i)
                    {
                        object_statuses[i] = SAI_STATUS_SUCCESS;
                    }
                    return SAI_STATUS_SUCCESS;
                }));

        // Create temp route
        gRouteOrch->addTempRoute(ctx, desired_nhg);
        gRouteOrch->gRouteBulker.flush();
        // Use the original desired NHG key (matches production doTask pattern);
        // addRoutePost will recurse into the tmp_next_hop path internally when NHG doesn't exist.
        gRouteOrch->addRoutePost(ctx, desired_nhg);

        // --- Step 3: Verify desired_nhg_key was recorded ---
        auto it = gRouteOrch->m_syncdRoutes[gVirtualRouterId].find(IpPrefix("5.5.5.0/24"));
        ASSERT_NE(it, gRouteOrch->m_syncdRoutes[gVirtualRouterId].end());
        
        const RouteNhg& route_nhg = it->second;
        
        // The actual programmed nexthop should be a single nexthop (temp route)
        ASSERT_EQ(route_nhg.nhg_key.getSize(), 1);
        
        // The desired_nhg_key should contain the full original NHG
        ASSERT_EQ(route_nhg.desired_nhg_key.getSize(), 2);
        ASSERT_TRUE(route_nhg.desired_nhg_key == desired_nhg);
    }

    TEST_F(RouteOrchTest, RouteOrchTestTempRouteNoReRandomizeWhenUnchanged)
    {
        // Test that re-randomization doesn't occur when:
        // 1. Current temp nexthop is still valid
        // 2. Desired NHG membership hasn't changed
        
        // --- Step 1: Setup resolved neighbors ---
        Table neighborTable(m_app_db.get(), APP_NEIGH_TABLE_NAME);

        neighborTable.set("Ethernet0:10.0.0.20", {{"neigh", "00:00:0a:00:00:14"}, {"family", "IPv4"}});
        neighborTable.set("Ethernet0:10.0.0.21", {{"neigh", "00:00:0a:00:00:15"}, {"family", "IPv4"}});
        neighborTable.set("Ethernet0:10.0.0.22", {{"neigh", "00:00:0a:00:00:16"}, {"family", "IPv4"}});

        gNeighOrch->addExistingData(&neighborTable);
        static_cast<Orch *>(gNeighOrch)->doTask();

        // --- Step 2: Create initial temp route ---
        NextHopGroupKey desired_nhg("10.0.0.20,10.0.0.21,10.0.0.22");
        IpPrefix prefix("6.6.6.0/24");
        
        RouteBulkContext ctx(prefix.to_string(), true);
        ctx.vrf_id = gVirtualRouterId;
        ctx.ip_prefix = prefix;
        ctx.nhg = desired_nhg;

        sai_object_id_t first_nh_oid = 0;

        EXPECT_CALL(*mock_sai_route_api,
                    create_route_entries(::testing::_, ::testing::_, ::testing::_, ::testing::_, ::testing::_, ::testing::_))
            .WillOnce(::testing::Invoke(
                [&first_nh_oid](uint32_t object_count,
                                const sai_route_entry_t * /*route_entries*/,
                                const uint32_t *attr_count,
                                const sai_attribute_t **attr_list,
                                sai_bulk_op_error_mode_t /*mode*/,
                                sai_status_t *object_statuses) -> sai_status_t
                {
                    for (uint32_t i = 0; i < object_count; ++i)
                    {
                        // Capture the first nexthop OID
                        for (uint32_t j = 0; j < attr_count[i]; ++j)
                        {
                            if (attr_list[i][j].id == SAI_ROUTE_ENTRY_ATTR_NEXT_HOP_ID)
                            {
                                first_nh_oid = attr_list[i][j].value.oid;
                            }
                        }
                        object_statuses[i] = SAI_STATUS_SUCCESS;
                    }
                    return SAI_STATUS_SUCCESS;
                }));

        gRouteOrch->addTempRoute(ctx, desired_nhg);
        gRouteOrch->gRouteBulker.flush();
        gRouteOrch->addRoutePost(ctx, desired_nhg);

        ASSERT_NE(first_nh_oid, 0u);

        // Verify initial state of the temp route
        auto it_before = gRouteOrch->m_syncdRoutes[gVirtualRouterId].find(prefix);
        ASSERT_NE(it_before, gRouteOrch->m_syncdRoutes[gVirtualRouterId].end());
        
        // Capture the state before retry
        NextHopGroupKey nhg_before = it_before->second.nhg_key;
        NextHopGroupKey desired_nhg_before = it_before->second.desired_nhg_key;
        
        ASSERT_EQ(nhg_before.getSize(), 1);  // Should be single nexthop (temp route)
        ASSERT_EQ(desired_nhg_before.getSize(), 3);  // Desired should be all 3

        // --- Step 3: Retry with same NHG (simulating NHG creation still failing) ---
        RouteBulkContext ctx2(prefix.to_string(), true);
        ctx2.vrf_id = gVirtualRouterId;
        ctx2.ip_prefix = prefix;
        ctx2.nhg = desired_nhg;

        // If re-randomization occurred, addRoute would be called and SAI mock would fire
        // We expect NO new SAI call because the guard should prevent re-randomization
        // Force NHG creation to fail so the guard-check code path is actually reached
        // (otherwise addNextHopGroup would succeed via vslib, bypassing the guard entirely)
        EXPECT_CALL(*mock_sai_next_hop_group_api, create_next_hop_group(_, _, _, _))
            .WillRepeatedly(Return(SAI_STATUS_TABLE_FULL));
        EXPECT_CALL(*mock_sai_route_api,
                    create_route_entries(::testing::_, ::testing::_, ::testing::_, ::testing::_, ::testing::_, ::testing::_))
            .Times(0);
        EXPECT_CALL(*mock_sai_route_api,
                    set_route_entries_attribute(::testing::_, ::testing::_, ::testing::_, ::testing::_, ::testing::_))
            .Times(0);

        bool result = gRouteOrch->addRoute(ctx2, desired_nhg);
        gRouteOrch->gRouteBulker.flush();

        // Should return false (route not added) without changing the temp route
        ASSERT_FALSE(result);

        // --- Step 4: Verify that addTempRoute was NOT called again ---
        // The route state should remain completely unchanged
        auto it_after = gRouteOrch->m_syncdRoutes[gVirtualRouterId].find(prefix);
        ASSERT_NE(it_after, gRouteOrch->m_syncdRoutes[gVirtualRouterId].end());
        
        // nhg_key (actual programmed route) should be identical
        ASSERT_TRUE(it_after->second.nhg_key == nhg_before);
        ASSERT_EQ(it_after->second.nhg_key.getSize(), 1);
        
        // desired_nhg_key should be identical
        ASSERT_TRUE(it_after->second.desired_nhg_key == desired_nhg_before);
        ASSERT_EQ(it_after->second.desired_nhg_key.getSize(), 3);
        
        // Verify the route is still pointing to the same single nexthop
        // (if addTempRoute was called again, it might have picked a different one)
        NextHopKey nh_after = *it_after->second.nhg_key.getNextHops().begin();
        NextHopKey nh_before = *nhg_before.getNextHops().begin();
        ASSERT_TRUE(nh_after == nh_before);
    }

    TEST_F(RouteOrchTest, RouteOrchTestTempRouteReRandomizeWhenMembershipChanges)
    {
        // Test that re-randomization DOES occur when NHG membership changes
        // (e.g., new nexthop becomes available)
        
        // --- Step 1: Setup initial resolved neighbors ---
        Table neighborTable(m_app_db.get(), APP_NEIGH_TABLE_NAME);

        neighborTable.set("Ethernet0:10.0.0.30", {{"neigh", "00:00:0a:00:00:1e"}, {"family", "IPv4"}});
        neighborTable.set("Ethernet0:10.0.0.31", {{"neigh", "00:00:0a:00:00:1f"}, {"family", "IPv4"}});

        gNeighOrch->addExistingData(&neighborTable);
        static_cast<Orch *>(gNeighOrch)->doTask();

        // --- Step 2: Create initial temp route with 2 nexthops ---
        NextHopGroupKey initial_nhg("10.0.0.30,10.0.0.31");
        IpPrefix prefix("7.7.7.0/24");
        
        RouteBulkContext ctx(prefix.to_string(), true);
        ctx.vrf_id = gVirtualRouterId;
        ctx.ip_prefix = prefix;
        ctx.nhg = initial_nhg;

        sai_object_id_t first_nh_oid = 0;

        EXPECT_CALL(*mock_sai_route_api,
                    create_route_entries(::testing::_, ::testing::_, ::testing::_, ::testing::_, ::testing::_, ::testing::_))
            .WillOnce(::testing::Invoke(
                [&first_nh_oid](uint32_t object_count,
                                const sai_route_entry_t * /*route_entries*/,
                                const uint32_t *attr_count,
                                const sai_attribute_t **attr_list,
                                sai_bulk_op_error_mode_t /*mode*/,
                                sai_status_t *object_statuses) -> sai_status_t
                {
                    for (uint32_t i = 0; i < object_count; ++i)
                    {
                        for (uint32_t j = 0; j < attr_count[i]; ++j)
                        {
                            if (attr_list[i][j].id == SAI_ROUTE_ENTRY_ATTR_NEXT_HOP_ID)
                            {
                                first_nh_oid = attr_list[i][j].value.oid;
                            }
                        }
                        object_statuses[i] = SAI_STATUS_SUCCESS;
                    }
                    return SAI_STATUS_SUCCESS;
                }));

        gRouteOrch->addTempRoute(ctx, initial_nhg);
        gRouteOrch->gRouteBulker.flush();
        gRouteOrch->addRoutePost(ctx, initial_nhg);

        ASSERT_NE(first_nh_oid, 0u);

        // Verify initial state
        auto it = gRouteOrch->m_syncdRoutes[gVirtualRouterId].find(prefix);
        ASSERT_NE(it, gRouteOrch->m_syncdRoutes[gVirtualRouterId].end());
        ASSERT_EQ(it->second.desired_nhg_key.getSize(), 2);

        // --- Step 3: Add a new neighbor (simulating new nexthop coming up) ---
        neighborTable.set("Ethernet0:10.0.0.32", {{"neigh", "00:00:0a:00:00:20"}, {"family", "IPv4"}});
        gNeighOrch->addExistingData(&neighborTable);
        static_cast<Orch *>(gNeighOrch)->doTask();

        // --- Step 4: Retry with EXPANDED NHG (membership changed) ---
        NextHopGroupKey expanded_nhg("10.0.0.30,10.0.0.31,10.0.0.32");
        
        RouteBulkContext ctx2(prefix.to_string(), true);
        ctx2.vrf_id = gVirtualRouterId;
        ctx2.ip_prefix = prefix;
        ctx2.nhg = expanded_nhg;

        sai_object_id_t second_nh_oid = 0;
        bool sai_called = false;

        // Force NHG creation to fail so re-randomization via addTempRoute is triggered
        // (otherwise addNextHopGroup would succeed via vslib, bypassing addTempRoute)
        EXPECT_CALL(*mock_sai_next_hop_group_api, create_next_hop_group(_, _, _, _))
            .WillRepeatedly(Return(SAI_STATUS_TABLE_FULL));

        // This time, SAI should be called because membership changed
        EXPECT_CALL(*mock_sai_route_api,
                    set_route_entries_attribute(::testing::_, ::testing::_, ::testing::_, ::testing::_, ::testing::_))
            .WillOnce(::testing::Invoke(
                [&second_nh_oid, &sai_called](uint32_t object_count,
                                               const sai_route_entry_t * /*route_entries*/,
                                               const sai_attribute_t *attr_list,
                                               sai_bulk_op_error_mode_t /*mode*/,
                                               sai_status_t *object_statuses) -> sai_status_t
                {
                    sai_called = true;
                    for (uint32_t i = 0; i < object_count; ++i)
                    {
                        if (attr_list[i].id == SAI_ROUTE_ENTRY_ATTR_NEXT_HOP_ID)
                        {
                            second_nh_oid = attr_list[i].value.oid;
                        }
                        object_statuses[i] = SAI_STATUS_SUCCESS;
                    }
                    return SAI_STATUS_SUCCESS;
                }));

        bool result = gRouteOrch->addRoute(ctx2, expanded_nhg);
        gRouteOrch->gRouteBulker.flush();
        gRouteOrch->addRoutePost(ctx2, expanded_nhg);

        // Verify that SAI was called (re-randomization occurred)
        ASSERT_TRUE(sai_called);
        // addRoute should return false since NHG creation still fails
        ASSERT_FALSE(result);
        
        // Verify that the desired_nhg_key was updated to reflect the new membership
        it = gRouteOrch->m_syncdRoutes[gVirtualRouterId].find(prefix);
        ASSERT_NE(it, gRouteOrch->m_syncdRoutes[gVirtualRouterId].end());
        ASSERT_EQ(it->second.desired_nhg_key.getSize(), 3);
        ASSERT_TRUE(it->second.desired_nhg_key == expanded_nhg);
    }

    TEST_F(RouteOrchTest, RouteOrchTestTempRouteToFullNhgLifecycle)
    {
        // Test the complete lifecycle:
        // 1. Initial temp route with 3 nexthops (NHG creation fails)
        // 2. Previously selected nexthop goes down - membership changes, re-randomization occurs
        // 3. NHG creation succeeds - route ends up pointing to full 2-member NHG (remaining NHs)
        
        // --- Step 1: Setup initial 3 resolved neighbors ---
        Table neighborTable(m_app_db.get(), APP_NEIGH_TABLE_NAME);

        neighborTable.set("Ethernet0:10.0.0.40", {{"neigh", "00:00:0a:00:00:28"}, {"family", "IPv4"}});
        neighborTable.set("Ethernet0:10.0.0.41", {{"neigh", "00:00:0a:00:00:29"}, {"family", "IPv4"}});
        neighborTable.set("Ethernet0:10.0.0.42", {{"neigh", "00:00:0a:00:00:2a"}, {"family", "IPv4"}});

        gNeighOrch->addExistingData(&neighborTable);
        static_cast<Orch *>(gNeighOrch)->doTask();

        // --- Step 2: Create initial temp route with 3 nexthops ---
        NextHopGroupKey initial_nhg("10.0.0.40,10.0.0.41,10.0.0.42");
        IpPrefix prefix("8.8.8.0/24");
        
        RouteBulkContext ctx(prefix.to_string(), true);
        ctx.vrf_id = gVirtualRouterId;
        ctx.ip_prefix = prefix;
        ctx.nhg = initial_nhg;

        sai_object_id_t initial_nh_oid = 0;

        EXPECT_CALL(*mock_sai_route_api,
                    create_route_entries(::testing::_, ::testing::_, ::testing::_, ::testing::_, ::testing::_, ::testing::_))
            .WillOnce(::testing::Invoke(
                [&initial_nh_oid](uint32_t object_count,
                                  const sai_route_entry_t * /*route_entries*/,
                                  const uint32_t *attr_count,
                                  const sai_attribute_t **attr_list,
                                  sai_bulk_op_error_mode_t /*mode*/,
                                  sai_status_t *object_statuses) -> sai_status_t
                {
                    for (uint32_t i = 0; i < object_count; ++i)
                    {
                        // Capture which nexthop was selected
                        for (uint32_t j = 0; j < attr_count[i]; ++j)
                        {
                            if (attr_list[i][j].id == SAI_ROUTE_ENTRY_ATTR_NEXT_HOP_ID)
                            {
                                initial_nh_oid = attr_list[i][j].value.oid;
                            }
                        }
                        object_statuses[i] = SAI_STATUS_SUCCESS;
                    }
                    return SAI_STATUS_SUCCESS;
                }));

        gRouteOrch->addTempRoute(ctx, initial_nhg);
        gRouteOrch->gRouteBulker.flush();
        gRouteOrch->addRoutePost(ctx, initial_nhg);

        // Verify temp route was created
        auto it = gRouteOrch->m_syncdRoutes[gVirtualRouterId].find(prefix);
        ASSERT_NE(it, gRouteOrch->m_syncdRoutes[gVirtualRouterId].end());
        ASSERT_EQ(it->second.nhg_key.getSize(), 1);  // Single nexthop (temp)
        ASSERT_EQ(it->second.desired_nhg_key.getSize(), 3);  // Desired has 3

        // Capture which nexthop was initially selected. Keep a copy because route state
        // is updated later and references into nhg_key can become stale.
        NextHopKey selected_nh = *it->second.nhg_key.getNextHops().begin();
        
        // --- Step 3: Simulate the selected nexthop going down ---
        // Delete the neighbor that was selected for the temp route
        neighborTable.del("Ethernet0:" + selected_nh.ip_address.to_string());
        gNeighOrch->addExistingData(&neighborTable);
        static_cast<Orch *>(gNeighOrch)->doTask();

        // --- Step 4: Retry with reduced NHG (selected NH removed - triggers re-randomization) ---
        // Build the reduced NHG by removing the selected nexthop
        std::vector<std::string> remaining_nhs;
        for (const auto& nh : initial_nhg.getNextHops())
        {
            if (nh.ip_address.to_string() != selected_nh.ip_address.to_string())
            {
                remaining_nhs.push_back(nh.ip_address.to_string());
            }
        }
        
        NextHopGroupKey reduced_nhg(remaining_nhs[0] + "," + remaining_nhs[1]);
        
        RouteBulkContext ctx2(prefix.to_string(), true);
        ctx2.vrf_id = gVirtualRouterId;
        ctx2.ip_prefix = prefix;
        ctx2.nhg = reduced_nhg;

        // Force NHG creation to fail so re-randomization via addTempRoute is triggered
        // (otherwise addNextHopGroup would succeed via vslib, bypassing addTempRoute)
        EXPECT_CALL(*mock_sai_next_hop_group_api, create_next_hop_group(_, _, _, _))
            .WillRepeatedly(Return(SAI_STATUS_TABLE_FULL));

        // Re-randomization should occur because the currently selected NH is no longer valid
        EXPECT_CALL(*mock_sai_route_api,
                    set_route_entries_attribute(::testing::_, ::testing::_, ::testing::_, ::testing::_, ::testing::_))
            .WillOnce(::testing::Invoke(
                [initial_nh_oid](uint32_t object_count,
                                 const sai_route_entry_t * /*route_entries*/,
                                 const sai_attribute_t *attr_list,
                                 sai_bulk_op_error_mode_t /*mode*/,
                                 sai_status_t *object_statuses) -> sai_status_t
                {
                    for (uint32_t i = 0; i < object_count; ++i)
                    {
                        // Verify a different nexthop was selected (not the one that went down)
                        if (attr_list[i].id == SAI_ROUTE_ENTRY_ATTR_NEXT_HOP_ID)
                        {
                            EXPECT_NE(attr_list[i].value.oid, initial_nh_oid);
                        }
                        object_statuses[i] = SAI_STATUS_SUCCESS;
                    }
                    return SAI_STATUS_SUCCESS;
                }));

        gRouteOrch->addRoute(ctx2, reduced_nhg);
        gRouteOrch->gRouteBulker.flush();
        gRouteOrch->addRoutePost(ctx2, reduced_nhg);

        // Verify re-randomization occurred and desired_nhg_key was updated
        it = gRouteOrch->m_syncdRoutes[gVirtualRouterId].find(prefix);
        ASSERT_NE(it, gRouteOrch->m_syncdRoutes[gVirtualRouterId].end());
        ASSERT_EQ(it->second.nhg_key.getSize(), 1);  // Still temp route (single NH)
        ASSERT_EQ(it->second.desired_nhg_key.getSize(), 2);  // Desired now has 2 (reduced)
        ASSERT_TRUE(it->second.desired_nhg_key == reduced_nhg);
        
        // Verify that the nexthop that went down is NOT in the new desired_nhg_key
        bool found_removed_nh = false;
        for (const auto& nh : it->second.desired_nhg_key.getNextHops())
        {
            if (nh.ip_address.to_string() == selected_nh.ip_address.to_string())
            {
                found_removed_nh = true;
                break;
            }
        }
        ASSERT_FALSE(found_removed_nh) << "Nexthop that went down should not be in desired_nhg_key";

        // --- Step 5: Simulate successful NHG creation ---
        // Manually add the NHG to m_syncdNextHopGroups to simulate successful creation
        sai_object_id_t mock_nhg_id = 0x999999;  // Mock NHG SAI object ID
        NextHopGroupEntry nhg_entry;
        nhg_entry.next_hop_group_id = mock_nhg_id;
        
        // Add nexthop members to the group (only the remaining 2)
        uint32_t seq_id = 0;
        for (const auto& nh : reduced_nhg.getNextHops())
        {
            sai_object_id_t nh_id = gNeighOrch->getNextHopId(nh);
            NextHopGroupMemberEntry member_entry;
            member_entry.next_hop_id = nh_id;
            member_entry.seq_id = seq_id++;
            nhg_entry.nhopgroup_members[nh] = member_entry;
        }
        
        gRouteOrch->m_syncdNextHopGroups[reduced_nhg] = nhg_entry;

        // --- Step 6: Call addRoute again - this time it should succeed with full NHG ---
        RouteBulkContext ctx3(prefix.to_string(), true);
        ctx3.vrf_id = gVirtualRouterId;
        ctx3.ip_prefix = prefix;

        // Should update route to point to the NHG (not a temp single nexthop)
        EXPECT_CALL(*mock_sai_route_api,
                    set_route_entries_attribute(::testing::_, ::testing::_, ::testing::_, ::testing::_, ::testing::_))
            .WillOnce(::testing::Invoke(
                [mock_nhg_id](uint32_t object_count,
                              const sai_route_entry_t * /*route_entries*/,
                              const sai_attribute_t *attr_list,
                              sai_bulk_op_error_mode_t /*mode*/,
                              sai_status_t *object_statuses) -> sai_status_t
                {
                    for (uint32_t i = 0; i < object_count; ++i)
                    {
                        // Verify it's setting the NHG ID, not a single nexthop
                        if (attr_list[i].id == SAI_ROUTE_ENTRY_ATTR_NEXT_HOP_ID)
                        {
                            EXPECT_EQ(attr_list[i].value.oid, mock_nhg_id);
                        }
                        object_statuses[i] = SAI_STATUS_SUCCESS;
                    }
                    return SAI_STATUS_SUCCESS;
                }));

        bool result = gRouteOrch->addRoute(ctx3, reduced_nhg);
        gRouteOrch->gRouteBulker.flush();
        gRouteOrch->addRoutePost(ctx3, reduced_nhg);

        // addRoute returns false even on success (it's an internal method that queues operations)
        // Success is verified by checking m_syncdRoutes instead
        ASSERT_FALSE(result);

        // --- Step 7: Verify final state - route points to full 2-member NHG ---
        it = gRouteOrch->m_syncdRoutes[gVirtualRouterId].find(prefix);
        ASSERT_NE(it, gRouteOrch->m_syncdRoutes[gVirtualRouterId].end());
        
        // Route should now point to the full 2-member NHG, not a temp single nexthop
        ASSERT_EQ(it->second.nhg_key.getSize(), 2);
        ASSERT_TRUE(it->second.nhg_key == reduced_nhg);
        
        // desired_nhg_key is empty: route now directly points to NHG (no longer a temp route)
        ASSERT_EQ(it->second.desired_nhg_key.getSize(), 0);
    }

    TEST_F(RouteOrchTest, RouteOrch_RemoveRoutePrefixRunsAsyncPublisherFlush)
    {
        auto *routeConsumer = dynamic_cast<Consumer *>(gRouteOrch->getExecutor(APP_ROUTE_TABLE_NAME));
        ASSERT_NE(routeConsumer, nullptr);

        std::deque<KeyOpFieldsValuesTuple> entries;
        entries.push_back({"7.7.7.0/24", "SET",
                           {{"ifname", "Ethernet0"}, {"nexthop", "10.0.0.2"}}});
        routeConsumer->addToSync(entries);
        static_cast<Orch *>(gRouteOrch)->doTask();

        (void)gRouteOrch->removeRoutePrefix(IpPrefix("7.7.7.0/24"));
        ASSERT_TRUE(gRouteOrch->removeRoutePrefix(IpPrefix("7.7.7.0/24")));
    }

    /* Resync on the non-ZMQ consumer. The batch touches no default route, so
     * the drain stops after it; the resync DELs must still be merged. */
    TEST_F(RouteOrchTest, RouteOrch_ResyncStagesDelsForSyncedRoutes)
    {
        auto *routeConsumer = dynamic_cast<Consumer *>(gRouteOrch->getExecutor(APP_ROUTE_TABLE_NAME));
        ASSERT_NE(routeConsumer, nullptr);

        /* Program one route so m_syncdRoutes has an entry to mark. */
        std::deque<KeyOpFieldsValuesTuple> entries;
        entries.push_back({"8.8.8.0/24", "SET",
                           {{"ifname", "Ethernet0"}, {"nexthop", "10.0.0.2"}}});
        routeConsumer->addToSync(entries);
        static_cast<Orch *>(gRouteOrch)->doTask();
        ASSERT_TRUE(routeConsumer->m_toSync.empty());

        entries.clear();
        entries.push_back({"resync", "SET", {}});
        routeConsumer->addToSync(entries);
        static_cast<Orch *>(gRouteOrch)->doTask();

        bool delStaged = false;
        for (const auto &kv : routeConsumer->m_toSync)
        {
            if (kv.first == "8.8.8.0/24" && kfvOp(kv.second) == DEL_COMMAND)
            {
                delStaged = true;
            }
        }
        ASSERT_TRUE(delStaged);

        /* Advertise the route again and end the resync. The first pass clears
         * m_resync; "8.8.8.0/24" sorts before "resync", so its DEL and SET wait
         * for the second pass, which removes and adds the route again. */
        entries.clear();
        entries.push_back({"8.8.8.0/24", "SET",
                           {{"ifname", "Ethernet0"}, {"nexthop", "10.0.0.2"}}});
        entries.push_back({"resync", "DEL", {}});
        routeConsumer->addToSync(entries);
        static_cast<Orch *>(gRouteOrch)->doTask();
        static_cast<Orch *>(gRouteOrch)->doTask();

        ASSERT_TRUE(routeConsumer->m_toSync.empty());
        ASSERT_TRUE(gRouteOrch->removeRoutePrefix(IpPrefix("8.8.8.0/24")));
    }

    TEST_F(RouteOrchTest, RemoveNhgAfterAllMembersInvalidatedWithoutReplacement)
    {
        auto *routeConsumer = dynamic_cast<Consumer *>(gRouteOrch->getExecutor(APP_ROUTE_TABLE_NAME));
        ASSERT_NE(routeConsumer, nullptr);

        // Add ECMP route with 2 nexthops (both already resolved in SetUp)
        std::deque<KeyOpFieldsValuesTuple> entries;
        entries.push_back({"9.9.9.0/24", "SET",
                           {{"ifname", "Ethernet0,Ethernet0"},
                            {"nexthop", "10.0.0.2,10.0.0.3"}}});
        routeConsumer->addToSync(entries);
        static_cast<Orch *>(gRouteOrch)->doTask();

        // Verify the NHG was created
        NextHopGroupKey nhg_key("10.0.0.2@Ethernet0,10.0.0.3@Ethernet0");
        ASSERT_NE(gRouteOrch->m_syncdNextHopGroups.find(nhg_key),
                  gRouteOrch->m_syncdNextHopGroups.end())
            << "NHG not found after adding ECMP route";

        // Invalidate both nexthops without calling validnexthop afterward.
        // This is the exact sequence MuxPrefixBasedNbrHandler::disable() performs:
        // it removes NHG members from SAI but does not add tunnel replacements.
        NextHopKey nh1(IpAddress("10.0.0.2"), "Ethernet0");
        NextHopKey nh2(IpAddress("10.0.0.3"), "Ethernet0");
        uint32_t count = 0;

        // Capture nexthop refcounts before invalidation
        auto refcount1_before = gNeighOrch->getNextHopRefCount(nh1);
        auto refcount2_before = gNeighOrch->getNextHopRefCount(nh2);
        ASSERT_GT(refcount1_before, 0);
        ASSERT_GT(refcount2_before, 0);

        ASSERT_TRUE(gRouteOrch->invalidnexthopinNextHopGroup(nh1, count));
        ASSERT_EQ(count, 1u);
        ASSERT_TRUE(gRouteOrch->invalidnexthopinNextHopGroup(nh2, count));
        ASSERT_EQ(count, 1u);

        // invalidnexthopinNextHopGroup only removes SAI members;
        // NeighOrch refcounts must remain unchanged until the NHG is destroyed.
        ASSERT_EQ(gNeighOrch->getNextHopRefCount(nh1), refcount1_before);
        ASSERT_EQ(gNeighOrch->getNextHopRefCount(nh2), refcount2_before);

        // Delete the route — triggers removeNextHopGroup which decrements refcounts
        auto fail_before = sai_fail_count;

        entries.clear();
        entries.push_back({"9.9.9.0/24", "DEL", {}});
        routeConsumer->addToSync(entries);
        static_cast<Orch *>(gRouteOrch)->doTask();

        ASSERT_EQ(sai_fail_count, fail_before)
            << "Route deletion should not trigger any SAI failures";

        ASSERT_EQ(gRouteOrch->m_syncdNextHopGroups.find(nhg_key),
                  gRouteOrch->m_syncdNextHopGroups.end())
            << "NHG should be removed after route deletion";

        // After NHG destruction, refcounts must be decremented
        ASSERT_EQ(gNeighOrch->getNextHopRefCount(nh1), refcount1_before - 1);
        ASSERT_EQ(gNeighOrch->getNextHopRefCount(nh2), refcount2_before - 1);
    }

    /*
     * A NEXTHOP_GROUP_TABLE SET that grows a single next hop into a group
     * changes its SAI ID, and the route on its index follows. Later SETs that
     * shrink the group to one next hop and grow it again keep the ID.
     */
    TEST_F(RouteOrchTest, NhgSetBetweenOneAndSeveralNextHopsMovesItsRoute)
    {
        const string index = "nhg_replace";
        const IpPrefix prefix("2.2.3.0/24");

        auto nhg_consumer = dynamic_cast<Consumer *>(gNhgOrch->getExecutor(APP_NEXTHOP_GROUP_TABLE_NAME));
        auto route_consumer = dynamic_cast<Consumer *>(gRouteOrch->getExecutor(APP_ROUTE_TABLE_NAME));
        ASSERT_NE(nhg_consumer, nullptr);
        ASSERT_NE(route_consumer, nullptr);

        auto feed = [](Consumer *consumer, Orch *orch, const KeyOpFieldsValuesTuple &entry) {
            deque<KeyOpFieldsValuesTuple> entries{entry};
            consumer->addToSync(entries);
            orch->doTask();
        };
        auto routeNextHopId = [&prefix]() {
            sai_route_entry_t route_entry;
            route_entry.vr_id = gVirtualRouterId;
            route_entry.switch_id = gSwitchId;
            swss::copy(route_entry.destination, prefix);
            sai_attribute_t attr;
            attr.id = SAI_ROUTE_ENTRY_ATTR_NEXT_HOP_ID;
            attr.value.oid = SAI_NULL_OBJECT_ID;
            EXPECT_EQ(sai_route_api->get_route_entry_attribute(&route_entry, 1, &attr), SAI_STATUS_SUCCESS);
            return attr.value.oid;
        };

        feed(nhg_consumer, gNhgOrch, {index, "SET", {{"nexthop", "10.0.0.2"}, {"ifname", "Ethernet0"}}});
        ASSERT_TRUE(gNhgOrch->hasNhg(index));
        const sai_object_id_t nh2_id = gNeighOrch->getNextHopId(NextHopKey("10.0.0.2@Ethernet0"));
        ASSERT_EQ(gNhgOrch->getNhg(index).getId(), nh2_id);

        feed(route_consumer, gRouteOrch, {prefix.to_string(), "SET", {{"nexthop_group", index}}});
        ASSERT_EQ(routeNextHopId(), nh2_id);

        feed(nhg_consumer, gNhgOrch, {index, "SET", {{"nexthop", "10.0.0.2,10.0.0.3"}, {"ifname", "Ethernet0,Ethernet0"}}});
        EXPECT_TRUE(nhg_consumer->m_toSync.empty());
        const sai_object_id_t group_id = gNhgOrch->getNhg(index).getId();
        EXPECT_NE(group_id, nh2_id);
        EXPECT_EQ(routeNextHopId(), group_id);

        /* Back to one next hop, and to two again: the SAI group stays, so the
         * route is not touched. */
        feed(nhg_consumer, gNhgOrch, {index, "SET", {{"nexthop", "10.0.0.3"}, {"ifname", "Ethernet0"}}});
        EXPECT_TRUE(nhg_consumer->m_toSync.empty());
        EXPECT_EQ(gNhgOrch->getNhg(index).getId(), group_id);
        EXPECT_EQ(routeNextHopId(), group_id);

        feed(nhg_consumer, gNhgOrch, {index, "SET", {{"nexthop", "10.0.0.2,10.0.0.3"}, {"ifname", "Ethernet0,Ethernet0"}}});
        EXPECT_TRUE(nhg_consumer->m_toSync.empty());
        EXPECT_EQ(gNhgOrch->getNhg(index).getId(), group_id);
        EXPECT_EQ(routeNextHopId(), group_id);

        feed(route_consumer, gRouteOrch, {prefix.to_string(), "DEL", {}});
        feed(nhg_consumer, gNhgOrch, {index, "DEL", {}});
        ASSERT_FALSE(gNhgOrch->hasNhg(index));
    }

    /*
     * A router interface cannot be a group member, so a SAI group that shrinks
     * to a router interface next hop is replaced: the route moves to the
     * router interface and the SAI group is removed.
     */
    TEST_F(RouteOrchTest, NhgSetFromSeveralNextHopsToARouterInterfaceMovesItsRoute)
    {
        const string index = "nhg_to_rif";
        const IpPrefix prefix("2.2.4.0/24");

        auto nhg_consumer = dynamic_cast<Consumer *>(gNhgOrch->getExecutor(APP_NEXTHOP_GROUP_TABLE_NAME));
        auto route_consumer = dynamic_cast<Consumer *>(gRouteOrch->getExecutor(APP_ROUTE_TABLE_NAME));
        ASSERT_NE(nhg_consumer, nullptr);
        ASSERT_NE(route_consumer, nullptr);

        auto feed = [](Consumer *consumer, Orch *orch, const KeyOpFieldsValuesTuple &entry) {
            deque<KeyOpFieldsValuesTuple> entries{entry};
            consumer->addToSync(entries);
            orch->doTask();
        };
        auto routeNextHopId = [&prefix]() {
            sai_route_entry_t route_entry;
            route_entry.vr_id = gVirtualRouterId;
            route_entry.switch_id = gSwitchId;
            swss::copy(route_entry.destination, prefix);
            sai_attribute_t attr;
            attr.id = SAI_ROUTE_ENTRY_ATTR_NEXT_HOP_ID;
            attr.value.oid = SAI_NULL_OBJECT_ID;
            EXPECT_EQ(sai_route_api->get_route_entry_attribute(&route_entry, 1, &attr), SAI_STATUS_SUCCESS);
            return attr.value.oid;
        };
        auto groupExists = [](sai_object_id_t group_id) {
            sai_attribute_t attr;
            attr.id = SAI_NEXT_HOP_GROUP_ATTR_TYPE;
            return sai_next_hop_group_api->get_next_hop_group_attribute(group_id, 1, &attr) == SAI_STATUS_SUCCESS;
        };

        feed(nhg_consumer, gNhgOrch, {index, "SET", {{"nexthop", "10.0.0.2,10.0.0.3"}, {"ifname", "Ethernet0,Ethernet0"}}});
        ASSERT_TRUE(gNhgOrch->hasNhg(index));
        const sai_object_id_t group_id = gNhgOrch->getNhg(index).getId();
        ASSERT_TRUE(groupExists(group_id));

        feed(route_consumer, gRouteOrch, {prefix.to_string(), "SET", {{"nexthop_group", index}}});
        ASSERT_EQ(routeNextHopId(), group_id);

        feed(nhg_consumer, gNhgOrch, {index, "SET", {{"nexthop", "0.0.0.0"}, {"ifname", "Ethernet0"}}});
        EXPECT_TRUE(nhg_consumer->m_toSync.empty());
        const sai_object_id_t rif_id = gIntfsOrch->getRouterIntfsId("Ethernet0");
        ASSERT_NE(rif_id, SAI_NULL_OBJECT_ID);
        EXPECT_EQ(gNhgOrch->getNhg(index).getId(), rif_id);
        EXPECT_EQ(routeNextHopId(), rif_id);
        EXPECT_FALSE(groupExists(group_id));

        feed(route_consumer, gRouteOrch, {prefix.to_string(), "DEL", {}});
        feed(nhg_consumer, gNhgOrch, {index, "DEL", {}});
        ASSERT_FALSE(gNhgOrch->hasNhg(index));
    }

    /* Restores the drain quantum, clock and warm start flag on scope exit, so
     * a test body aborted by an ASSERT cannot leak them into later tests. */
    struct DrainClockGuard
    {
        std::chrono::milliseconds quantum;
        std::function<std::chrono::steady_clock::time_point()> now;
        bool warmStartDrainToCompletion;
        DrainClockGuard()
            : quantum(gRouteOrch->m_drainQuantum), now(gRouteOrch->m_drainNow),
              warmStartDrainToCompletion(gRouteOrch->m_warmStartDrainToCompletion) {}
        ~DrainClockGuard()
        {
            gRouteOrch->m_drainQuantum = quantum;
            gRouteOrch->m_drainNow = now;
            gRouteOrch->m_warmStartDrainToCompletion = warmStartDrainToCompletion;
        }
    };

    /* With time left in the quantum, a small batch runs all phases -- stage,
     * flush, handle responses -- in one doTask call, leaving nothing in
     * flight and nothing re-armed. */
    TEST_F(RouteOrchTest, RouteOrch_YieldingDrainSmallBatchSingleInvocation)
    {
        auto *zorch = static_cast<ZmqRouteOrch *>(gRouteOrch);
        DrainClockGuard drainStateGuard;
        /* A long quantum, so a CI scheduling stall cannot fake an expiry. */
        gRouteOrch->m_drainQuantum = std::chrono::hours(1);
        /* Never bound: no mqPollThread, no real traffic needed. */
        swss::ZmqRouteServer server("tcp://*:1281", "", /*lazyBind=*/true);
        ZmqRouteConsumer consumer(
            new swss::ZmqRouteConsumerStateTable(m_app_db.get(), APP_ROUTE_TABLE_NAME,
                                                 server, 5, /*dbPersistence=*/false),
            zorch, APP_ROUTE_TABLE_NAME);

        swss::Select sel;
        sel.addSelectable(&consumer);
        swss::Selectable *out = nullptr;

        std::deque<KeyOpFieldsValuesTuple> entries;
        entries.push_back({"9.9.9.0/24", "SET",
                           {{"ifname", "Ethernet0"}, {"nexthop", "10.0.0.2"}}});
        consumer.addToSync(entries);

        auto base_create = create_route_count;

        zorch->doTask(consumer);
        ASSERT_TRUE(consumer.m_toSync.empty());
        ASSERT_FALSE(zorch->drainInFlight(APP_ROUTE_TABLE_NAME));
        ASSERT_EQ(base_create + 1, create_route_count);
        ASSERT_EQ(sel.select(&out, 0), swss::Select::TIMEOUT);

        gRouteOrch->m_drainQuantum = std::chrono::milliseconds(20);
        ASSERT_TRUE(gRouteOrch->removeRoutePrefix(IpPrefix("9.9.9.0/24")));
    }

    /* Quantum 0 yields at every check: each call advances one unit of work
     * (phase change, staged entry, bulk chunk, handled status) and re-arms
     * the selectable. */
    TEST_F(RouteOrchTest, RouteOrch_YieldingDrainQuantumZeroStepsUnits)
    {
        auto *zorch = static_cast<ZmqRouteOrch *>(gRouteOrch);
        DrainClockGuard drainStateGuard;
        gRouteOrch->m_drainQuantum = std::chrono::milliseconds(0);
        swss::ZmqRouteServer server("tcp://*:1288", "", /*lazyBind=*/true);
        ZmqRouteConsumer consumer(
            new swss::ZmqRouteConsumerStateTable(m_app_db.get(), APP_ROUTE_TABLE_NAME,
                                                 server, 5, /*dbPersistence=*/false),
            zorch, APP_ROUTE_TABLE_NAME);

        swss::Select sel;
        sel.addSelectable(&consumer);
        swss::Selectable *out = nullptr;

        std::deque<KeyOpFieldsValuesTuple> entries;
        entries.push_back({"9.9.9.0/24", "SET",
                           {{"ifname", "Ethernet0"}, {"nexthop", "10.0.0.2"}}});
        consumer.addToSync(entries);

        auto base_create = create_route_count;

        /* Call 1: kIdle -> kStaging only. */
        zorch->doTask(consumer);
        ASSERT_FALSE(consumer.m_toSync.empty());
        ASSERT_TRUE(zorch->drainInFlight(APP_ROUTE_TABLE_NAME));
        ASSERT_EQ(sel.select(&out, 0), swss::Select::OBJECT);

        /* Call 2: stages the only entry; the scan ends. */
        zorch->doTask(consumer);
        ASSERT_TRUE(consumer.m_toSync.empty());
        ASSERT_TRUE(zorch->drainInFlight(APP_ROUTE_TABLE_NAME));
        ASSERT_EQ(base_create, create_route_count);
        ASSERT_EQ(sel.select(&out, 0), swss::Select::OBJECT);

        /* Call 3: flushes the batch to SAI. */
        zorch->doTask(consumer);
        ASSERT_TRUE(zorch->drainInFlight(APP_ROUTE_TABLE_NAME));
        ASSERT_EQ(base_create + 1, create_route_count);
        ASSERT_EQ(sel.select(&out, 0), swss::Select::OBJECT);

        /* Call 4: handles the status, runs the epilogue and ends the batch.
         * Nothing is pending, so nothing is re-armed. */
        zorch->doTask(consumer);
        ASSERT_FALSE(zorch->drainInFlight(APP_ROUTE_TABLE_NAME));
        ASSERT_EQ(sel.select(&out, 0), swss::Select::TIMEOUT);

        gRouteOrch->m_drainQuantum = std::chrono::milliseconds(20);
        ASSERT_TRUE(gRouteOrch->removeRoutePrefix(IpPrefix("9.9.9.0/24")));
    }

    /* Warm restore drains to completion whatever the quantum: with quantum 0,
     * one doTask stages, flushes and ends the whole backlog, because
     * warmRestoreAndSyncUp() has no select loop to resume a yielded drain. */
    TEST_F(RouteOrchTest, RouteOrch_WarmStartDrainsToCompletionIgnoringQuantum)
    {
        auto *zorch = static_cast<ZmqRouteOrch *>(gRouteOrch);
        DrainClockGuard drainStateGuard;
        gRouteOrch->m_drainQuantum = std::chrono::milliseconds(0);
        gRouteOrch->setWarmStartDrainToCompletion(true);

        swss::ZmqRouteServer server("tcp://*:1291", "", /*lazyBind=*/true);
        ZmqRouteConsumer consumer(
            new swss::ZmqRouteConsumerStateTable(m_app_db.get(), APP_ROUTE_TABLE_NAME,
                                                 server, 5, /*dbPersistence=*/false),
            zorch, APP_ROUTE_TABLE_NAME);

        swss::Select sel;
        sel.addSelectable(&consumer);
        swss::Selectable *out = nullptr;

        std::deque<KeyOpFieldsValuesTuple> entries;
        entries.push_back({"9.9.9.0/24", "SET",
                           {{"ifname", "Ethernet0"}, {"nexthop", "10.0.0.2"}}});
        entries.push_back({"9.9.10.0/24", "SET",
                           {{"ifname", "Ethernet0"}, {"nexthop", "10.0.0.2"}}});
        entries.push_back({"9.9.11.0/24", "SET",
                           {{"ifname", "Ethernet0"}, {"nexthop", "10.0.0.2"}}});
        consumer.addToSync(entries);

        auto base_create = create_route_count;

        zorch->doTask(consumer);
        ASSERT_TRUE(consumer.m_toSync.empty());
        ASSERT_FALSE(zorch->drainInFlight(APP_ROUTE_TABLE_NAME));
        /* One batch, one bulk create call. */
        ASSERT_EQ(base_create + 1, create_route_count);
        ASSERT_EQ(sel.select(&out, 0), swss::Select::TIMEOUT);

        gRouteOrch->setWarmStartDrainToCompletion(false);
        gRouteOrch->m_drainQuantum = std::chrono::milliseconds(20);
        ASSERT_TRUE(gRouteOrch->removeRoutePrefix(IpPrefix("9.9.9.0/24")));
        ASSERT_TRUE(gRouteOrch->removeRoutePrefix(IpPrefix("9.9.10.0/24")));
        ASSERT_TRUE(gRouteOrch->removeRoutePrefix(IpPrefix("9.9.11.0/24")));
    }

    /* A quantum that expires mid-scan pauses staging, and the next call
     * stages into the same batch: one flush, one response pass, one
     * epilogue. */
    TEST_F(RouteOrchTest, RouteOrch_YieldingDrainStagingResumesSameBatch)
    {
        auto *zorch = static_cast<ZmqRouteOrch *>(gRouteOrch);
        DrainClockGuard drainStateGuard;
        gRouteOrch->m_drainQuantum = std::chrono::milliseconds(0);
        swss::ZmqRouteServer server("tcp://*:1289", "", /*lazyBind=*/true);
        ZmqRouteConsumer consumer(
            new swss::ZmqRouteConsumerStateTable(m_app_db.get(), APP_ROUTE_TABLE_NAME,
                                                 server, 5, /*dbPersistence=*/false),
            zorch, APP_ROUTE_TABLE_NAME);

        std::deque<KeyOpFieldsValuesTuple> entries;
        entries.push_back({"9.9.9.0/24", "SET",
                           {{"ifname", "Ethernet0"}, {"nexthop", "10.0.0.2"}}});
        entries.push_back({"9.9.10.0/24", "SET",
                           {{"ifname", "Ethernet0"}, {"nexthop", "10.0.0.2"}}});
        entries.push_back({"9.9.11.0/24", "SET",
                           {{"ifname", "Ethernet0"}, {"nexthop", "10.0.0.2"}}});
        consumer.addToSync(entries);

        auto base_create = create_route_count;

        /* kIdle -> kStaging. */
        zorch->doTask(consumer);
        /* One staged entry per call; the batch grows across quanta. */
        for (size_t staged = 1; staged <= 3; staged++)
        {
            zorch->doTask(consumer);
            ASSERT_EQ(consumer.m_toSync.size(), 3 - staged);
            ASSERT_EQ(gRouteOrch->m_zmqDrainBatch.entries.size(), staged);
            ASSERT_TRUE(zorch->drainInFlight(APP_ROUTE_TABLE_NAME));
            ASSERT_EQ(base_create, create_route_count);
        }

        /* One call flushes all three in one bulk call. */
        zorch->doTask(consumer);
        ASSERT_EQ(base_create + 1, create_route_count);
        ASSERT_TRUE(zorch->drainInFlight(APP_ROUTE_TABLE_NAME));

        /* One handled status per call; the epilogue runs once, with the
         * last one. */
        for (size_t responded = 1; responded <= 3; responded++)
        {
            ASSERT_TRUE(zorch->drainInFlight(APP_ROUTE_TABLE_NAME));
            ASSERT_EQ(gRouteOrch->m_zmqDrainBatch.respondCursor, responded - 1);
            zorch->doTask(consumer);
        }
        ASSERT_FALSE(zorch->drainInFlight(APP_ROUTE_TABLE_NAME));

        gRouteOrch->m_drainQuantum = std::chrono::milliseconds(20);
        ASSERT_TRUE(gRouteOrch->removeRoutePrefix(IpPrefix("9.9.9.0/24")));
        ASSERT_TRUE(gRouteOrch->removeRoutePrefix(IpPrefix("9.9.10.0/24")));
        ASSERT_TRUE(gRouteOrch->removeRoutePrefix(IpPrefix("9.9.11.0/24")));
    }

    /* Expiry through the injectable clock: with a 20ms quantum and a clock
     * that advances 11ms per reading, the deadline is read at T, the check
     * after kIdle at T+11 and the check after the first staged entry at T+22,
     * so the first call stops with exactly one entry staged. */
    TEST_F(RouteOrchTest, RouteOrch_YieldingDrainClockDrivenExpiry)
    {
        auto *zorch = static_cast<ZmqRouteOrch *>(gRouteOrch);
        DrainClockGuard drainStateGuard;
        gRouteOrch->m_drainQuantum = std::chrono::milliseconds(20);
        auto fakeNow = std::make_shared<std::chrono::steady_clock::time_point>(
            std::chrono::steady_clock::time_point{});
        gRouteOrch->m_drainNow = [fakeNow]() {
            auto t = *fakeNow;
            *fakeNow += std::chrono::milliseconds(11);
            return t;
        };

        swss::ZmqRouteServer server("tcp://*:1290", "", /*lazyBind=*/true);
        ZmqRouteConsumer consumer(
            new swss::ZmqRouteConsumerStateTable(m_app_db.get(), APP_ROUTE_TABLE_NAME,
                                                 server, 5, /*dbPersistence=*/false),
            zorch, APP_ROUTE_TABLE_NAME);

        std::deque<KeyOpFieldsValuesTuple> entries;
        for (int i = 0; i < 5; i++)
        {
            entries.push_back({"9.9." + std::to_string(20 + i) + ".0/24", "SET",
                               {{"ifname", "Ethernet0"}, {"nexthop", "10.0.0.2"}}});
        }
        consumer.addToSync(entries);

        zorch->doTask(consumer);
        ASSERT_TRUE(zorch->drainInFlight(APP_ROUTE_TABLE_NAME));
        ASSERT_EQ(gRouteOrch->m_zmqDrainBatch.entries.size(), 1u);
        ASSERT_EQ(consumer.m_toSync.size(), 4u);

        for (int i = 0; i < 32 && zorch->drainInFlight(APP_ROUTE_TABLE_NAME); i++)
        {
            zorch->doTask(consumer);
        }
        ASSERT_FALSE(zorch->drainInFlight(APP_ROUTE_TABLE_NAME));
        ASSERT_TRUE(consumer.m_toSync.empty());

        gRouteOrch->m_drainNow = []() { return std::chrono::steady_clock::now(); };
        for (int i = 0; i < 5; i++)
        {
            ASSERT_TRUE(gRouteOrch->removeRoutePrefix(
                IpPrefix("9.9." + std::to_string(20 + i) + ".0/24")));
        }
    }

    /* While a batch is in flight, execute() does not merge m_ingress into
     * m_toSync. When the batch ends with ingress pending, the drain re-arms
     * the selectable, so data whose notification an execute() used while it
     * could not merge is not left behind. */
    TEST_F(RouteOrchTest, RouteOrch_YieldingDrainGatesIngressUntilBatchDone)
    {
        auto *zorch = static_cast<ZmqRouteOrch *>(gRouteOrch);
        DrainClockGuard drainStateGuard;
        swss::ZmqRouteServer server("tcp://*:1282", "", /*lazyBind=*/true);
        ZmqRouteConsumer consumer(
            new swss::ZmqRouteConsumerStateTable(m_app_db.get(), APP_ROUTE_TABLE_NAME,
                                                 server, 5, /*dbPersistence=*/false),
            zorch, APP_ROUTE_TABLE_NAME);
        server.bind();

        swss::ZmqClient client("tcp://localhost:1282", 0);
        swss::ZmqProducerStateTable producer(m_app_db.get(), APP_ROUTE_TABLE_NAME,
                                             client, /*dbPersistence=*/false);

        /* One unit per call, so the batch stays in flight across several
         * execute() calls. */
        gRouteOrch->m_drainQuantum = std::chrono::milliseconds(0);

        /* Start a batch (kIdle -> kStaging). */
        std::deque<KeyOpFieldsValuesTuple> entries;
        entries.push_back({"9.9.8.0/24", "SET",
                           {{"ifname", "Ethernet0"}, {"nexthop", "10.0.0.2"}}});
        consumer.addToSync(entries);
        zorch->doTask(consumer);
        ASSERT_TRUE(zorch->drainInFlight(APP_ROUTE_TABLE_NAME));

        /* A route that arrives over ZMQ mid-batch stays in m_ingress. */
        producer.set("9.9.7.0/24", {{"ifname", "Ethernet0"}, {"nexthop", "10.0.0.2"}});
        for (int i = 0; i < 100 && !consumer.ingressPending(); ++i)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        ASSERT_TRUE(consumer.ingressPending());

        /* execute() stages the batch entry and merges nothing. */
        consumer.execute();
        ASSERT_TRUE(consumer.ingressPending());
        ASSERT_TRUE(consumer.m_toSync.empty());
        ASSERT_TRUE(zorch->drainInFlight(APP_ROUTE_TABLE_NAME));

        /* execute() flushes; still in flight, still not merging. */
        consumer.execute();
        ASSERT_TRUE(consumer.ingressPending());
        ASSERT_TRUE(zorch->drainInFlight(APP_ROUTE_TABLE_NAME));

        /* execute() ends the batch, which re-arms the selectable because
         * ingress is pending. */
        consumer.execute();
        ASSERT_FALSE(zorch->drainInFlight(APP_ROUTE_TABLE_NAME));
        ASSERT_TRUE(consumer.ingressPending());

        swss::Select sel;
        sel.addSelectable(&consumer);
        swss::Selectable *out = nullptr;
        ASSERT_EQ(sel.select(&out, 0), swss::Select::OBJECT);

        /* With no batch in flight, execute() merges ingress and starts the
         * next batch. */
        consumer.execute();
        ASSERT_FALSE(consumer.ingressPending());
        ASSERT_TRUE(zorch->drainInFlight(APP_ROUTE_TABLE_NAME));

        for (int i = 0; i < 8 && zorch->drainInFlight(APP_ROUTE_TABLE_NAME); i++)
        {
            zorch->doTask(consumer);
        }
        ASSERT_FALSE(zorch->drainInFlight(APP_ROUTE_TABLE_NAME));

        gRouteOrch->m_drainQuantum = std::chrono::milliseconds(20);
        ASSERT_TRUE(gRouteOrch->removeRoutePrefix(IpPrefix("9.9.8.0/24")));
        ASSERT_TRUE(gRouteOrch->removeRoutePrefix(IpPrefix("9.9.7.0/24")));
    }

    /* The DELs a resync marker generates merge into m_toSync when the same
     * scan ends, so a batch left in flight does not lose them. */
    TEST_F(RouteOrchTest, RouteOrch_YieldingDrainResyncStagesDels)
    {
        auto *zorch = static_cast<ZmqRouteOrch *>(gRouteOrch);
        DrainClockGuard drainStateGuard;
        swss::ZmqRouteServer server("tcp://*:1283", "", /*lazyBind=*/true);
        ZmqRouteConsumer consumer(
            new swss::ZmqRouteConsumerStateTable(m_app_db.get(), APP_ROUTE_TABLE_NAME,
                                                 server, 5, /*dbPersistence=*/false),
            zorch, APP_ROUTE_TABLE_NAME);

        gRouteOrch->m_drainQuantum = std::chrono::hours(1);

        std::deque<KeyOpFieldsValuesTuple> entries;
        entries.push_back({"9.9.6.0/24", "SET",
                           {{"ifname", "Ethernet0"}, {"nexthop", "10.0.0.2"}}});
        consumer.addToSync(entries);
        zorch->doTask(consumer);
        ASSERT_FALSE(zorch->drainInFlight(APP_ROUTE_TABLE_NAME));

        entries.clear();
        entries.push_back({"resync", "SET", {}});
        consumer.addToSync(entries);
        zorch->doTask(consumer);

        bool delStaged = false;
        for (const auto &kv : consumer.m_toSync)
        {
            if (kv.first == "9.9.6.0/24" && kfvOp(kv.second) == DEL_COMMAND)
            {
                delStaged = true;
            }
        }
        ASSERT_TRUE(delStaged);

        /* Complete the resync and replay the DEL. */
        entries.clear();
        entries.push_back({"resync", "DEL", {}});
        consumer.addToSync(entries);
        for (int i = 0;
             i < 8 && (!consumer.m_toSync.empty() ||
                       zorch->drainInFlight(APP_ROUTE_TABLE_NAME));
             ++i)
        {
            zorch->doTask(consumer);
        }
        ASSERT_FALSE(zorch->drainInFlight(APP_ROUTE_TABLE_NAME));
        ASSERT_TRUE(consumer.m_toSync.empty());
        gRouteOrch->m_drainQuantum = std::chrono::milliseconds(20);
    }

    /* A backlog of entries that are all not ready must not stall the
     * yielding drain. The scan restarts at m_toSync.begin() after each
     * yield; entries skipped in place would be walked again every quantum
     * and the scan, with ingress held, would never end. They are moved out
     * of m_toSync instead and come back when the batch ends. */
    TEST_F(RouteOrchTest, RouteOrch_YieldingDrainDeferredSkipsTerminate)
    {
        auto *zorch = static_cast<ZmqRouteOrch *>(gRouteOrch);
        DrainClockGuard drainStateGuard;
        gRouteOrch->m_drainQuantum = std::chrono::milliseconds(20);
        auto fakeNow = std::make_shared<std::chrono::steady_clock::time_point>(
            std::chrono::steady_clock::time_point{});
        gRouteOrch->m_drainNow = [fakeNow]() {
            auto t = *fakeNow;
            *fakeNow += std::chrono::milliseconds(11);
            return t;
        };

        swss::ZmqRouteServer server("tcp://*:1284", "", /*lazyBind=*/true);
        ZmqRouteConsumer consumer(
            new swss::ZmqRouteConsumerStateTable(m_app_db.get(), APP_ROUTE_TABLE_NAME,
                                                 server, 5, /*dbPersistence=*/false),
            zorch, APP_ROUTE_TABLE_NAME);

        /* Seven routes, none ready: six in a VRF that does not exist, and
         * one on a next hop group index that has not been created. */
        std::deque<KeyOpFieldsValuesTuple> entries;
        for (int i = 0; i < 6; i++)
        {
            entries.push_back({"VrfMissing:9.9.3" + std::to_string(i) + ".0/24", "SET",
                               {{"ifname", "Ethernet0"}, {"nexthop", "10.0.0.2"}}});
        }
        entries.push_back({"9.9.36.0/24", "SET",
                           {{"nexthop_group", "nhg_not_created_yet"}}});
        consumer.addToSync(entries);

        auto base_create = create_route_count;

        /* The quantum expires after the first entry, which is deferred, not
         * staged. */
        zorch->doTask(consumer);
        ASSERT_TRUE(zorch->drainInFlight(APP_ROUTE_TABLE_NAME));
        ASSERT_TRUE(gRouteOrch->m_zmqDrainBatch.entries.empty());
        ASSERT_EQ(gRouteOrch->m_zmqDrainBatch.deferred.size(), 1u);
        ASSERT_EQ(consumer.m_toSync.size(), 6u);

        for (int i = 0; i < 32 && zorch->drainInFlight(APP_ROUTE_TABLE_NAME); i++)
        {
            zorch->doTask(consumer);
        }
        ASSERT_FALSE(zorch->drainInFlight(APP_ROUTE_TABLE_NAME));

        /* Nothing programmed, nothing lost: all seven are back in m_toSync,
         * and no context of theirs is left behind. */
        ASSERT_EQ(base_create, create_route_count);
        ASSERT_EQ(consumer.m_toSync.size(), 7u);
        ASSERT_TRUE(gRouteOrch->m_zmqDrainBatch.deferred.empty());
        ASSERT_TRUE(gRouteOrch->m_zmqDrainBatch.toBulk.empty());

        gRouteOrch->m_drainNow = []() { return std::chrono::steady_clock::now(); };
        gRouteOrch->m_drainQuantum = std::chrono::milliseconds(20);
    }

    /* A deferred route programs once what it waits for appears: a route in a
     * VRF that does not exist yet is deferred, and after the VRF is created
     * and the interface bound, the next drain programs it. */
    TEST_F(RouteOrchTest, RouteOrch_YieldingDrainDeferredVrfRouteProgramsLater)
    {
        auto *zorch = static_cast<ZmqRouteOrch *>(gRouteOrch);
        DrainClockGuard drainStateGuard;
        gRouteOrch->m_drainQuantum = std::chrono::hours(1);

        swss::ZmqRouteServer server("tcp://*:1285", "", /*lazyBind=*/true);
        ZmqRouteConsumer consumer(
            new swss::ZmqRouteConsumerStateTable(m_app_db.get(), APP_ROUTE_TABLE_NAME,
                                                 server, 5, /*dbPersistence=*/false),
            zorch, APP_ROUTE_TABLE_NAME);

        std::deque<KeyOpFieldsValuesTuple> entries;
        entries.push_back({"VrfBlue:9.9.40.0/24", "SET", {{"protocol", "kernel"},
                                                          {"nexthop", "0.0.0.0"},
                                                          {"ifname", "Ethernet12"}}});
        consumer.addToSync(entries);

        auto base_create = create_route_count;

        zorch->doTask(consumer);
        ASSERT_FALSE(zorch->drainInFlight(APP_ROUTE_TABLE_NAME));
        ASSERT_EQ(base_create, create_route_count);
        ASSERT_EQ(consumer.m_toSync.size(), 1u);

        entries.clear();
        entries.push_back({"VrfBlue", "SET", {{"vni", "500300"}}});
        auto vrfConsumer = dynamic_cast<Consumer *>(gVrfOrch->getExecutor(APP_VRF_TABLE_NAME));
        vrfConsumer->addToSync(entries);
        static_cast<Orch *>(gVrfOrch)->doTask();
        entries.clear();
        entries.push_back({"Ethernet12", "SET", {{"vrf_name", "VrfBlue"}}});
        auto intfConsumer = dynamic_cast<Consumer *>(gIntfsOrch->getExecutor(APP_INTF_TABLE_NAME));
        intfConsumer->addToSync(entries);
        static_cast<Orch *>(gIntfsOrch)->doTask();

        zorch->doTask(consumer);
        ASSERT_FALSE(zorch->drainInFlight(APP_ROUTE_TABLE_NAME));
        ASSERT_TRUE(consumer.m_toSync.empty());
        ASSERT_EQ(base_create + 1, create_route_count);

        gRouteOrch->m_drainQuantum = std::chrono::milliseconds(20);
    }

    /* clear() resets a context for a fresh attempt at the same route: key
     * and is_set stay, every per-attempt field resets. */
    TEST(RouteBulkContextClear, PreservesIdentityResetsScratch)
    {
        RouteBulkContext ctx("Vrf2:1.2.3.0/24", /*is_set=*/true);

        ctx.object_statuses.push_back(SAI_STATUS_SUCCESS);
        ctx.nhg_index = "nhg1";
        ctx.context_index = "ctx1";
        ctx.ipv = {"10.0.0.1"};
        ctx.alsv = {"Ethernet0"};
        ctx.vni_labelv = {"500100"};
        ctx.rmacv = {"aa:bb:cc:dd:ee:ff"};
        ctx.vrf_id = 0x1234;
        ctx.ip_prefix = IpPrefix("1.2.3.0/24");
        ctx.excp_intfs_flag = true;
        ctx.using_temp_nhg = true;
        ctx.vrf_group_flag = true;
        ctx.protocol = "bgp";
        ctx.fallback_to_default_route = true;
        ctx.staged = StagedNextHop::kNextHops;

        ctx.clear();

        EXPECT_EQ(ctx.key, "Vrf2:1.2.3.0/24");
        EXPECT_TRUE(ctx.is_set);

        EXPECT_TRUE(ctx.object_statuses.empty());
        EXPECT_TRUE(ctx.nhg_index.empty());
        EXPECT_TRUE(ctx.context_index.empty());
        EXPECT_TRUE(ctx.ipv.empty());
        EXPECT_TRUE(ctx.alsv.empty());
        EXPECT_TRUE(ctx.vni_labelv.empty());
        EXPECT_TRUE(ctx.rmacv.empty());
        EXPECT_EQ(ctx.vrf_id, SAI_NULL_OBJECT_ID);
        /* IpPrefix has no operator!=, so check the old value is gone. */
        EXPECT_FALSE(ctx.ip_prefix == IpPrefix("1.2.3.0/24"));
        EXPECT_FALSE(ctx.excp_intfs_flag);
        EXPECT_FALSE(ctx.using_temp_nhg);
        EXPECT_FALSE(ctx.vrf_group_flag);
        EXPECT_TRUE(ctx.protocol.empty());
        EXPECT_FALSE(ctx.fallback_to_default_route);
        EXPECT_EQ(ctx.staged, StagedNextHop::kNone);
    }

    TEST(RouteBulkContextClear, LabelContextResetsScratch)
    {
        LabelRouteBulkContext ctx;
        ctx.object_statuses.push_back(SAI_STATUS_SUCCESS);
        ctx.nhg_index = "nhg1";
        ctx.vrf_id = 0x1234;
        ctx.pop_count = 3;
        ctx.excp_intfs_flag = true;
        ctx.using_temp_nhg = true;

        ctx.clear();

        EXPECT_TRUE(ctx.object_statuses.empty());
        EXPECT_TRUE(ctx.nhg_index.empty());
        EXPECT_EQ(ctx.vrf_id, SAI_NULL_OBJECT_ID);
        EXPECT_EQ(ctx.pop_count, 0);
        EXPECT_FALSE(ctx.excp_intfs_flag);
        EXPECT_FALSE(ctx.using_temp_nhg);
    }

    /* A second m_toSync entry for a (key, op) whose context this batch has
     * already staged must not reset that context, since the bulker holds
     * pointers into its statuses. It is deferred to the next drain. */
    TEST_F(RouteOrchTest, RouteOrch_YieldingDrainDuplicateOfInFlightEntryDeferred)
    {
        auto *zorch = static_cast<ZmqRouteOrch *>(gRouteOrch);
        DrainClockGuard drainStateGuard;
        gRouteOrch->m_drainQuantum = std::chrono::hours(1);

        swss::ZmqRouteServer server("tcp://*:1286", "", /*lazyBind=*/true);
        ZmqRouteConsumer consumer(
            new swss::ZmqRouteConsumerStateTable(m_app_db.get(), APP_ROUTE_TABLE_NAME,
                                                 server, 5, /*dbPersistence=*/false),
            zorch, APP_ROUTE_TABLE_NAME);

        /* A batch in kStaging that already staged this key. */
        auto& batch = gRouteOrch->m_zmqDrainBatch;
        auto rc = batch.toBulk.emplace(std::piecewise_construct,
                std::forward_as_tuple("9.9.50.0/24", "SET"),
                std::forward_as_tuple("9.9.50.0/24", true));
        rc.first->second.object_statuses.push_back(SAI_STATUS_SUCCESS);
        batch.phase = RouteOrch::RouteDrainBatch::Phase::kStaging;

        std::deque<KeyOpFieldsValuesTuple> entries;
        entries.push_back({"9.9.50.0/24", "SET",
                           {{"ifname", "Ethernet0"}, {"nexthop", "10.0.0.2"}}});
        consumer.addToSync(entries);

        auto base_create = create_route_count;

        /* The duplicate is deferred and the batch ends; the planted context
         * has no batch entry, so nothing is programmed from it. */
        zorch->doTask(consumer);
        ASSERT_FALSE(zorch->drainInFlight(APP_ROUTE_TABLE_NAME));
        ASSERT_EQ(base_create, create_route_count);
        ASSERT_EQ(consumer.m_toSync.size(), 1u);

        consumer.m_toSync.clear();
        gRouteOrch->m_drainQuantum = std::chrono::milliseconds(20);
    }

    /* Teardowns a yielded batch has collected live in the batch. A label or
     * VNET drain running between quanta clears the shared
     * m_bulkNhgReducedRefCnt / m_bulkSrv6NhgReducedVec for its own pass; if
     * the batch kept its teardowns there, the cleared groups would never be
     * removed. */
    TEST_F(RouteOrchTest, RouteOrch_YieldingDrainRespondYieldSurvivesForeignAccumulatorClear)
    {
        auto *zorch = static_cast<ZmqRouteOrch *>(gRouteOrch);
        DrainClockGuard drainStateGuard;
        swss::ZmqRouteServer server("tcp://*:1287", "", /*lazyBind=*/true);
        ZmqRouteConsumer consumer(
            new swss::ZmqRouteConsumerStateTable(m_app_db.get(), APP_ROUTE_TABLE_NAME,
                                                 server, 5, /*dbPersistence=*/false),
            zorch, APP_ROUTE_TABLE_NAME);

        /* Resolve 10.0.0.4 so the two routes get distinct groups (SetUp
         * resolves only .2 and .3). */
        Table neighborTable(m_app_db.get(), APP_NEIGH_TABLE_NAME);
        neighborTable.set("Ethernet0:10.0.0.4",
                          {{"neigh", "00:00:0a:00:00:04"}, {"family", "IPv4"}});
        gNeighOrch->addExistingData(&neighborTable);
        static_cast<Orch *>(gNeighOrch)->doTask();

        auto base_nhg_count = gRouteOrch->m_nextHopGroupCount;

        gRouteOrch->m_drainQuantum = std::chrono::hours(1);
        std::deque<KeyOpFieldsValuesTuple> entries;
        entries.push_back({"9.9.60.0/24", "SET",
                           {{"ifname", "Ethernet0,Ethernet0"},
                            {"nexthop", "10.0.0.2,10.0.0.3"}}});
        entries.push_back({"9.9.61.0/24", "SET",
                           {{"ifname", "Ethernet0,Ethernet0"},
                            {"nexthop", "10.0.0.2,10.0.0.4"}}});
        consumer.addToSync(entries);
        zorch->doTask(consumer);
        ASSERT_FALSE(zorch->drainInFlight(APP_ROUTE_TABLE_NAME));
        ASSERT_EQ(gRouteOrch->m_nextHopGroupCount, base_nhg_count + 2);

        /* Delete both, one unit per call, and stop once the first status is
         * handled: the first group's teardown is then pending. */
        gRouteOrch->m_drainQuantum = std::chrono::milliseconds(0);
        entries.clear();
        entries.push_back({"9.9.60.0/24", "DEL", {}});
        entries.push_back({"9.9.61.0/24", "DEL", {}});
        consumer.addToSync(entries);

        bool reachedMidResponse = false;
        for (int i = 0; i < 16; i++)
        {
            zorch->doTask(consumer);
            ASSERT_TRUE(zorch->drainInFlight(APP_ROUTE_TABLE_NAME))
                << "batch completed without pausing between responses";
            if (gRouteOrch->m_zmqDrainBatch.phase == RouteOrch::RouteDrainBatch::Phase::kResponding &&
                gRouteOrch->m_zmqDrainBatch.respondCursor == 1)
            {
                reachedMidResponse = true;
                break;
            }
        }
        ASSERT_TRUE(reachedMidResponse);
        ASSERT_FALSE(gRouteOrch->m_zmqDrainBatch.nhgReducedRefCnt.empty());

        /* What a label or VNET drain does between quanta. */
        gRouteOrch->m_bulkNhgReducedRefCnt.clear();
        gRouteOrch->m_bulkSrv6NhgReducedVec.clear();

        for (int i = 0; i < 8 && zorch->drainInFlight(APP_ROUTE_TABLE_NAME); i++)
        {
            zorch->doTask(consumer);
        }
        ASSERT_FALSE(zorch->drainInFlight(APP_ROUTE_TABLE_NAME));
        ASSERT_EQ(gRouteOrch->m_nextHopGroupCount, base_nhg_count);

        gRouteOrch->m_drainQuantum = std::chrono::milliseconds(20);
    }

    /* The batch epilogue yields too. Deleting two ECMP routes with distinct
     * groups in one batch leaves two groups to remove; at quantum 0 the
     * epilogue removes one group per call, so the batch stays in flight for
     * one more call after the last status is handled. */
    TEST_F(RouteOrchTest, RouteOrch_YieldingDrainEpilogueYields)
    {
        auto *zorch = static_cast<ZmqRouteOrch *>(gRouteOrch);
        DrainClockGuard drainStateGuard;
        swss::ZmqRouteServer server("tcp://*:1292", "", /*lazyBind=*/true);
        ZmqRouteConsumer consumer(
            new swss::ZmqRouteConsumerStateTable(m_app_db.get(), APP_ROUTE_TABLE_NAME,
                                                 server, 5, /*dbPersistence=*/false),
            zorch, APP_ROUTE_TABLE_NAME);

        /* Resolve 10.0.0.4 so the two routes get distinct groups (SetUp
         * resolves only .2 and .3). */
        Table neighborTable(m_app_db.get(), APP_NEIGH_TABLE_NAME);
        neighborTable.set("Ethernet0:10.0.0.4",
                          {{"neigh", "00:00:0a:00:00:04"}, {"family", "IPv4"}});
        gNeighOrch->addExistingData(&neighborTable);
        static_cast<Orch *>(gNeighOrch)->doTask();

        /* Program both routes in one call. */
        gRouteOrch->m_drainQuantum = std::chrono::hours(1);
        auto base_create = create_route_count;
        std::deque<KeyOpFieldsValuesTuple> entries;
        entries.push_back({"9.9.40.0/24", "SET",
                           {{"ifname", "Ethernet0,Ethernet0"},
                            {"nexthop", "10.0.0.2,10.0.0.3"}}});
        entries.push_back({"9.9.41.0/24", "SET",
                           {{"ifname", "Ethernet0,Ethernet0"},
                            {"nexthop", "10.0.0.2,10.0.0.4"}}});
        consumer.addToSync(entries);
        zorch->doTask(consumer);
        ASSERT_FALSE(zorch->drainInFlight(APP_ROUTE_TABLE_NAME));
        /* Both routes go out in one bulk create, or the DEL batch below has
         * nothing to tear down. */
        ASSERT_EQ(base_create + 1, create_route_count);
        ASSERT_TRUE(consumer.m_toSync.empty());

        /* Delete both in one batch, one unit per call. */
        gRouteOrch->m_drainQuantum = std::chrono::milliseconds(0);
        entries.clear();
        entries.push_back({"9.9.40.0/24", "DEL", {}});
        entries.push_back({"9.9.41.0/24", "DEL", {}});
        consumer.addToSync(entries);

        /* Step until both statuses are handled. A batch that ends without
         * stopping in the epilogue fails here. */
        bool reachedMidEpilogue = false;
        for (int i = 0; i < 16; i++)
        {
            zorch->doTask(consumer);
            if (zorch->drainInFlight(APP_ROUTE_TABLE_NAME) &&
                gRouteOrch->m_zmqDrainBatch.respondCursor == 2)
            {
                reachedMidEpilogue = true;
                break;
            }
            ASSERT_TRUE(zorch->drainInFlight(APP_ROUTE_TABLE_NAME))
                << "batch completed without pausing mid-epilogue";
        }
        ASSERT_TRUE(reachedMidEpilogue);

        /* The epilogue removed one group and yielded with the other left.
         * One more call removes it and ends the batch. */
        zorch->doTask(consumer);
        ASSERT_FALSE(zorch->drainInFlight(APP_ROUTE_TABLE_NAME));

        gRouteOrch->m_drainQuantum = std::chrono::milliseconds(20);
    }

    /* Same as RouteOrch_YieldingDrainRespondYieldSurvivesForeignAccumulatorClear,
     * with the label or VNET drain running while the epilogue is yielded. */
    TEST_F(RouteOrchTest, RouteOrch_YieldingDrainEpilogueSurvivesForeignAccumulatorClear)
    {
        auto *zorch = static_cast<ZmqRouteOrch *>(gRouteOrch);
        DrainClockGuard drainStateGuard;
        swss::ZmqRouteServer server("tcp://*:1293", "", /*lazyBind=*/true);
        ZmqRouteConsumer consumer(
            new swss::ZmqRouteConsumerStateTable(m_app_db.get(), APP_ROUTE_TABLE_NAME,
                                                 server, 5, /*dbPersistence=*/false),
            zorch, APP_ROUTE_TABLE_NAME);

        /* Resolve 10.0.0.4 so the two routes get distinct groups (SetUp
         * resolves only .2 and .3). */
        Table neighborTable(m_app_db.get(), APP_NEIGH_TABLE_NAME);
        neighborTable.set("Ethernet0:10.0.0.4",
                          {{"neigh", "00:00:0a:00:00:04"}, {"family", "IPv4"}});
        gNeighOrch->addExistingData(&neighborTable);
        static_cast<Orch *>(gNeighOrch)->doTask();

        auto base_nhg_count = gRouteOrch->m_nextHopGroupCount;

        gRouteOrch->m_drainQuantum = std::chrono::hours(1);
        std::deque<KeyOpFieldsValuesTuple> entries;
        entries.push_back({"9.9.62.0/24", "SET",
                           {{"ifname", "Ethernet0,Ethernet0"},
                            {"nexthop", "10.0.0.2,10.0.0.3"}}});
        entries.push_back({"9.9.63.0/24", "SET",
                           {{"ifname", "Ethernet0,Ethernet0"},
                            {"nexthop", "10.0.0.2,10.0.0.4"}}});
        consumer.addToSync(entries);
        zorch->doTask(consumer);
        ASSERT_FALSE(zorch->drainInFlight(APP_ROUTE_TABLE_NAME));
        ASSERT_EQ(gRouteOrch->m_nextHopGroupCount, base_nhg_count + 2);

        /* Delete both, one unit per call, and stop once both statuses are
         * handled: the epilogue has then removed one group and yielded with
         * the other left. */
        gRouteOrch->m_drainQuantum = std::chrono::milliseconds(0);
        entries.clear();
        entries.push_back({"9.9.62.0/24", "DEL", {}});
        entries.push_back({"9.9.63.0/24", "DEL", {}});
        consumer.addToSync(entries);

        bool reachedMidEpilogue = false;
        for (int i = 0; i < 16; i++)
        {
            zorch->doTask(consumer);
            if (zorch->drainInFlight(APP_ROUTE_TABLE_NAME) &&
                gRouteOrch->m_zmqDrainBatch.respondCursor == 2)
            {
                reachedMidEpilogue = true;
                break;
            }
            ASSERT_TRUE(zorch->drainInFlight(APP_ROUTE_TABLE_NAME))
                << "batch completed without pausing mid-epilogue";
        }
        ASSERT_TRUE(reachedMidEpilogue);
        ASSERT_FALSE(gRouteOrch->m_zmqDrainBatch.nhgReducedRefCnt.empty());

        /* What a label or VNET drain does between quanta. */
        gRouteOrch->m_bulkNhgReducedRefCnt.clear();
        gRouteOrch->m_bulkSrv6NhgReducedVec.clear();

        for (int i = 0; i < 8 && zorch->drainInFlight(APP_ROUTE_TABLE_NAME); i++)
        {
            zorch->doTask(consumer);
        }
        ASSERT_FALSE(zorch->drainInFlight(APP_ROUTE_TABLE_NAME));
        ASSERT_EQ(gRouteOrch->m_nextHopGroupCount, base_nhg_count);

        gRouteOrch->m_drainQuantum = std::chrono::milliseconds(20);
    }

    /*
     * Routes the yielding drain has staged and not answered yet pin what they
     * were staged on. The helpers below drive the drain to a chosen point.
     */
    namespace inflight
    {
        using Phase = RouteOrch::RouteDrainBatch::Phase;

        const NextHopKey nh2("10.0.0.2@Ethernet0");
        const NextHopKey nh3("10.0.0.3@Ethernet0");

        struct Zmq
        {
            swss::ZmqRouteServer server;
            ZmqRouteConsumer consumer;

            Zmq(swss::DBConnector *db, int port)
                : server("tcp://*:" + std::to_string(port), "", /*lazyBind=*/true),
                  consumer(new swss::ZmqRouteConsumerStateTable(db, APP_ROUTE_TABLE_NAME, server, 5,
                                                                /*dbPersistence=*/false),
                           static_cast<ZmqRouteOrch *>(gRouteOrch), APP_ROUTE_TABLE_NAME)
            {
            }

            void add(const std::vector<KeyOpFieldsValuesTuple> &kofvs)
            {
                std::deque<KeyOpFieldsValuesTuple> entries(kofvs.begin(), kofvs.end());
                consumer.addToSync(entries);
            }

            void step()
            {
                static_cast<ZmqRouteOrch *>(gRouteOrch)->doTask(consumer);
            }

            bool stepUntil(const std::function<bool()> &done, int max = 200)
            {
                for (int i = 0; i < max && !done(); i++)
                {
                    step();
                }
                return done();
            }

            /* Runs the batch in flight, and any work it leaves, to the end. */
            void finish()
            {
                gRouteOrch->m_drainQuantum = std::chrono::hours(1);
                for (int i = 0; i < 20 && (consumer.hasPendingTasks() ||
                                           gRouteOrch->drainInFlight(APP_ROUTE_TABLE_NAME)); i++)
                {
                    step();
                }
            }
        };

        RouteOrch::RouteDrainBatch &batch()
        {
            return gRouteOrch->m_zmqDrainBatch;
        }

        /* Every route of the batch staged, none flushed. */
        bool staged()
        {
            return batch().phase == Phase::kFlushing;
        }

        /* Every route of the batch flushed, none answered. */
        bool flushed()
        {
            return batch().phase == Phase::kResponding && batch().respondCursor == 0 &&
                   !batch().entries.empty();
        }

        /* The context of the SET of `prefix` in the batch in flight. */
        RouteBulkContext *stagedContext(const IpPrefix &prefix)
        {
            for (auto &it : batch().toBulk)
            {
                if (it.second.is_set && it.second.ip_prefix == prefix)
                {
                    return &it.second;
                }
            }
            return nullptr;
        }

        void feedNhg(const KeyOpFieldsValuesTuple &entry)
        {
            auto consumer = dynamic_cast<Consumer *>(gNhgOrch->getExecutor(APP_NEXTHOP_GROUP_TABLE_NAME));
            ASSERT_NE(consumer, nullptr);
            std::deque<KeyOpFieldsValuesTuple> entries{entry};
            consumer->addToSync(entries);
            static_cast<Orch *>(gNhgOrch)->doTask();
        }

        void setNhg(const string &index, const string &nexthops, const string &ifnames)
        {
            feedNhg({index, "SET", {{"nexthop", nexthops}, {"ifname", ifnames}}});
        }

        sai_object_id_t nhgId(const string &index)
        {
            return gNhgOrch->getNhg(index).getId();
        }

        /* NhgOrchCommon keeps its groups protected; a member pointer taken in
         * a derived class reaches them. */
        struct NhgPeek : NhgOrch
        {
            static unsigned refs(NhgOrch *orch, const string &index)
            {
                return (orch->*(&NhgPeek::m_syncdNextHopGroups)).at(index).ref_count;
            }
        };

        unsigned nhgRefs(const string &index)
        {
            return NhgPeek::refs(gNhgOrch, index);
        }

        const RouteNhg *recorded(const IpPrefix &prefix)
        {
            auto table = gRouteOrch->m_syncdRoutes.find(gVirtualRouterId);
            if (table == gRouteOrch->m_syncdRoutes.end())
            {
                return nullptr;
            }
            auto route = table->second.find(prefix);
            return route == table->second.end() ? nullptr : &route->second;
        }

        sai_object_id_t routeNextHopIdOf(const IpPrefix &prefix)
        {
            sai_route_entry_t route_entry;
            route_entry.vr_id = gVirtualRouterId;
            route_entry.switch_id = gSwitchId;
            swss::copy(route_entry.destination, prefix);
            sai_attribute_t attr;
            attr.id = SAI_ROUTE_ENTRY_ATTR_NEXT_HOP_ID;
            attr.value.oid = SAI_NULL_OBJECT_ID;
            if (sai_route_api->get_route_entry_attribute(&route_entry, 1, &attr) != SAI_STATUS_SUCCESS)
            {
                return SAI_NULL_OBJECT_ID;
            }
            return attr.value.oid;
        }

        IpPrefix prefixOf(const sai_route_entry_t &entry)
        {
            IpAddress ip(entry.destination.addr.ip4);
            int len = __builtin_popcount(entry.destination.mask.ip4);
            return IpPrefix(ip.to_string() + "/" + std::to_string(len));
        }

        /* One bulk route remove of this prefix fails with `status`; every
         * other entry of that call goes to SAI. */
        IpPrefix fail_prefix;
        sai_status_t fail_remove_status = SAI_STATUS_SUCCESS;
        sai_bulk_remove_route_entry_fn saved_bulk_remove = nullptr;

        sai_status_t failOnceBulkRemove(uint32_t count, const sai_route_entry_t *entries,
                                        sai_bulk_op_error_mode_t mode, sai_status_t *statuses)
        {
            std::vector<sai_route_entry_t> pass_entries;
            std::vector<uint32_t> pass_index;
            bool failed = false;
            for (uint32_t i = 0; i < count; i++)
            {
                if (fail_remove_status != SAI_STATUS_SUCCESS && prefixOf(entries[i]) == fail_prefix)
                {
                    statuses[i] = fail_remove_status;
                    failed = true;
                    continue;
                }
                pass_entries.push_back(entries[i]);
                pass_index.push_back(i);
            }
            if (failed)
            {
                fail_remove_status = SAI_STATUS_SUCCESS;
            }
            if (!pass_entries.empty())
            {
                std::vector<sai_status_t> pass_statuses(pass_entries.size());
                saved_bulk_remove(static_cast<uint32_t>(pass_entries.size()), pass_entries.data(), mode,
                                  pass_statuses.data());
                for (size_t i = 0; i < pass_index.size(); i++)
                {
                    statuses[pass_index[i]] = pass_statuses[i];
                }
            }
            return failed ? SAI_STATUS_FAILURE : SAI_STATUS_SUCCESS;
        }

        struct FailRemoveOnce
        {
            FailRemoveOnce(const IpPrefix &prefix, sai_status_t status)
            {
                fail_prefix = prefix;
                fail_remove_status = status;
                /* The drain's bulker copied the API's functions when it was
                 * built, so the failure goes into its copy. */
                saved_bulk_remove = gRouteOrch->gRouteBulker.remove_entries;
                gRouteOrch->gRouteBulker.remove_entries = failOnceBulkRemove;
            }
            ~FailRemoveOnce()
            {
                gRouteOrch->gRouteBulker.remove_entries = saved_bulk_remove;
                fail_remove_status = SAI_STATUS_SUCCESS;
            }
        };
    }

    /* A group DEL arriving while a route on it is staged or flushed waits for
     * the route; the route ends on the group's ID. */
    TEST_F(RouteOrchTest, InFlightGroupDelWaitsForTheStagedRoute)
    {
        using namespace inflight;
        DrainClockGuard clock;
        Zmq z(m_app_db.get(), 1301);
        const string x = "nhg_inflight_x", y = "nhg_inflight_y";
        const IpPrefix prefix("40.1.0.0/24");

        for (bool update : {false, true})
        {
            for (bool at_flush : {false, true})
            {
                SCOPED_TRACE(string(update ? "update" : "create") + (at_flush ? ", flushed" : ", staged"));
                setNhg(x, "10.0.0.2", "Ethernet0");
                setNhg(y, "10.0.0.3", "Ethernet0");
                if (update)
                {
                    z.add({{prefix.to_string(), "SET", {{"nexthop_group", y}}}});
                    z.finish();
                    ASSERT_EQ(routeNextHopIdOf(prefix), nhgId(y));
                }

                gRouteOrch->m_drainQuantum = std::chrono::milliseconds(0);
                z.add({{prefix.to_string(), "SET", {{"nexthop_group", x}}}});
                ASSERT_TRUE(z.stepUntil(at_flush ? flushed : staged));
                EXPECT_EQ(nhgRefs(x), 1u) << "the staged route pins its group";

                feedNhg({x, "DEL", {}});
                ASSERT_TRUE(gNhgOrch->hasNhg(x)) << "a pinned group was deleted";

                z.finish();
                ASSERT_NE(recorded(prefix), nullptr);
                EXPECT_EQ(recorded(prefix)->nhg_index, x);
                EXPECT_EQ(routeNextHopIdOf(prefix), nhgId(x));
                EXPECT_EQ(nhgRefs(x), 1u) << "the pin was not given back";

                z.add({{prefix.to_string(), "DEL", {}}});
                z.finish();
                static_cast<Orch *>(gNhgOrch)->doTask();
                EXPECT_FALSE(gNhgOrch->hasNhg(x)) << "the refused DEL did not complete";
                feedNhg({y, "DEL", {}});
                EXPECT_FALSE(gNhgOrch->hasNhg(y));
            }
        }
    }

    /* A DEL fails and a SET of the same prefix to the same group follows in
     * the batch, so the recorded route equals the staged one and
     * addRoutePost() is skipped. The staging pin is still given back. */
    TEST_F(RouteOrchTest, InFlightPinIsReleasedWhenPostIsSkipped)
    {
        using namespace inflight;
        const string x = "nhg_inflight_x";
        const IpPrefix prefix("40.7.0.0/24");
        auto consumer = dynamic_cast<Consumer *>(gRouteOrch->getExecutor(APP_ROUTE_TABLE_NAME));
        ASSERT_NE(consumer, nullptr);

        setNhg(x, "10.0.0.2", "Ethernet0");
        std::deque<KeyOpFieldsValuesTuple> entries{{prefix.to_string(), "SET", {{"nexthop_group", x}}}};
        consumer->addToSync(entries);
        static_cast<Orch *>(gRouteOrch)->doTask();
        ASSERT_EQ(nhgRefs(x), 1u);

        {
            FailRemoveOnce fail(prefix, SAI_STATUS_OBJECT_IN_USE);
            entries = {{prefix.to_string(), "DEL", {}}, {prefix.to_string(), "SET", {{"nexthop_group", x}}}};
            consumer->addToSync(entries);
            static_cast<Orch *>(gRouteOrch)->doTask();
        }

        ASSERT_NE(recorded(prefix), nullptr);
        EXPECT_EQ(recorded(prefix)->nhg_index, x);
        EXPECT_EQ(nhgRefs(x), 1u) << "the staging pin was not given back";

        entries = {{prefix.to_string(), "DEL", {}}};
        consumer->addToSync(entries);
        static_cast<Orch *>(gRouteOrch)->doTask();
        static_cast<Orch *>(gRouteOrch)->doTask();
        EXPECT_EQ(recorded(prefix), nullptr);
        EXPECT_EQ(nhgRefs(x), 0u);
        feedNhg({x, "DEL", {}});
        EXPECT_FALSE(gNhgOrch->hasNhg(x));
    }

    /* With the group table full, the route goes on a temporary next hop.
     * That next hop is pinned and given back, not the group. */
    TEST_F(RouteOrchTest, InFlightTemporaryRoutePinsItsNextHop)
    {
        using namespace inflight;
        DrainClockGuard clock;
        Zmq z(m_app_db.get(), 1308);
        const IpPrefix prefix("40.8.0.0/24");
        const NextHopGroupKey members("10.0.0.2@Ethernet0,10.0.0.3@Ethernet0");
        const int nh2_refs = gNeighOrch->getNextHopRefCount(nh2);
        const int nh3_refs = gNeighOrch->getNextHopRefCount(nh3);

        struct MaxGuard
        {
            unsigned max = gRouteOrch->m_maxNextHopGroupCount;
            ~MaxGuard() { gRouteOrch->m_maxNextHopGroupCount = max; }
        } max_guard;
        gRouteOrch->m_maxNextHopGroupCount = gRouteOrch->m_nextHopGroupCount + NhgOrch::getSyncedNhgCount();

        gRouteOrch->m_drainQuantum = std::chrono::milliseconds(0);
        z.add({{prefix.to_string(), "SET", {{"nexthop", "10.0.0.2,10.0.0.3"}, {"ifname", "Ethernet0,Ethernet0"}}}});
        ASSERT_TRUE(z.stepUntil(staged));

        RouteBulkContext *ctx = stagedContext(prefix);
        ASSERT_NE(ctx, nullptr);
        EXPECT_EQ(ctx->staged, StagedNextHop::kTempNextHop);
        ASSERT_EQ(ctx->tmp_next_hop.getSize(), 1u);
        const NextHopKey tmp = *ctx->tmp_next_hop.getNextHops().begin();
        const NextHopKey other = tmp == nh2 ? nh3 : nh2;
        const int tmp_refs = tmp == nh2 ? nh2_refs : nh3_refs;
        const int other_refs = tmp == nh2 ? nh3_refs : nh2_refs;
        EXPECT_FALSE(gRouteOrch->hasNextHopGroup(members));
        EXPECT_EQ(gNeighOrch->getNextHopRefCount(tmp), tmp_refs + 1) << "the temporary next hop is not pinned";
        EXPECT_EQ(gNeighOrch->getNextHopRefCount(other), other_refs);

        gRouteOrch->m_drainQuantum = std::chrono::hours(1);
        ASSERT_TRUE(z.stepUntil([]() { return !gRouteOrch->drainInFlight(APP_ROUTE_TABLE_NAME); }));
        EXPECT_EQ(routeNextHopIdOf(prefix), gNeighOrch->getNextHopId(tmp));
        EXPECT_EQ(gNeighOrch->getNextHopRefCount(tmp), tmp_refs + 1) << "only the route's own reference stays";
        EXPECT_EQ(gNeighOrch->getNextHopRefCount(other), other_refs);

        z.add({{prefix.to_string(), "DEL", {}}});
        z.finish();
        EXPECT_EQ(gNeighOrch->getNextHopRefCount(nh2), nh2_refs);
        EXPECT_EQ(gNeighOrch->getNextHopRefCount(nh3), nh3_refs);
    }

    /* A RouteOrch group created for a staged route has no reference but the
     * pin. Removing unused groups, as the label drain's epilogue does between
     * quanta, must keep it. */
    TEST_F(RouteOrchTest, InFlightGroupRemovalKeepsTheStagedRoutesGroup)
    {
        using namespace inflight;
        DrainClockGuard clock;
        Zmq z(m_app_db.get(), 1309);
        const IpPrefix prefix("40.9.0.0/24");
        const NextHopGroupKey members("10.0.0.2@Ethernet0,10.0.0.3@Ethernet0");

        gRouteOrch->m_drainQuantum = std::chrono::milliseconds(0);
        z.add({{prefix.to_string(), "SET", {{"nexthop", "10.0.0.2,10.0.0.3"}, {"ifname", "Ethernet0,Ethernet0"}}}});
        ASSERT_TRUE(z.stepUntil(staged));
        ASSERT_TRUE(gRouteOrch->hasNextHopGroup(members));
        EXPECT_EQ(gRouteOrch->m_syncdNextHopGroups.at(members).ref_count, 1) << "the staged route pins its group";

        gRouteOrch->removeNextHopGroup(members);
        ASSERT_TRUE(gRouteOrch->hasNextHopGroup(members)) << "a pinned group was removed";

        z.finish();
        EXPECT_EQ(routeNextHopIdOf(prefix), gRouteOrch->m_syncdNextHopGroups.at(members).next_hop_group_id);
        EXPECT_EQ(gRouteOrch->m_syncdNextHopGroups.at(members).ref_count, 1);

        z.add({{prefix.to_string(), "DEL", {}}});
        z.finish();
        EXPECT_FALSE(gRouteOrch->hasNextHopGroup(members));
    }

    /* A box with mux runs the yielding drain with no quantum. */
    TEST_F(RouteOrchTest, InFlightMuxBoxDrainsToCompletion)
    {
        using namespace inflight;
        DrainClockGuard clock;
        Table mux_table(m_config_db.get(), CFG_MUX_CABLE_TABLE_NAME);
        mux_table.set("Ethernet4", {{"server_ipv4", "192.168.0.2/32"}, {"state", "auto"}});
        Zmq z(m_app_db.get(), 1311);

        gRouteOrch->m_drainQuantum = std::chrono::milliseconds(0);
        vector<IpPrefix> prefixes = {IpPrefix("40.11.1.0/24"), IpPrefix("40.11.2.0/24"), IpPrefix("40.11.3.0/24")};
        for (const auto &prefix : prefixes)
        {
            z.add({{prefix.to_string(), "SET", {{"nexthop", "10.0.0.2"}, {"ifname", "Ethernet0"}}}});
        }
        z.step();
        EXPECT_TRUE(gRouteOrch->m_muxDrainToCompletion);
        EXPECT_FALSE(gRouteOrch->drainInFlight(APP_ROUTE_TABLE_NAME)) << "the drain yielded on a mux box";
        EXPECT_TRUE(z.consumer.m_toSync.empty());
        for (const auto &prefix : prefixes)
        {
            EXPECT_EQ(routeNextHopIdOf(prefix), gNeighOrch->getNextHopId(nh2));
        }

        for (const auto &prefix : prefixes)
        {
            z.add({{prefix.to_string(), "DEL", {}}});
        }
        z.finish();
        mux_table.del("Ethernet4");
    }
}
