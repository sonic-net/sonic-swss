#define private public // make Directory::m_values available to clean it.
#include "directory.h"
#undef private
#define protected public
#include "orch.h"
#undef protected
#include "ut_helper.h"
#include "mock_orchagent_main.h"
#include "mock_table.h"
#include "mock_response_publisher.h"
#include "bulker.h"

extern string gMySwitchType;

extern std::unique_ptr<MockResponsePublisher> gMockResponsePublisher;

using ::testing::_;

namespace routeorch_test
{
    using namespace std;

    shared_ptr<swss::DBConnector> m_app_db;
    shared_ptr<swss::DBConnector> m_config_db;
    shared_ptr<swss::DBConnector> m_state_db;
    shared_ptr<swss::DBConnector> m_chassis_app_db;

    int create_route_count;
    int set_route_count;
    int remove_route_count;
    int sai_fail_count;

    sai_route_api_t ut_sai_route_api;
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

        // Drop and a valid nexthop can not be provided for the same prefix
        if (drop && valid_nexthop)
            sai_fail_count++;

        return old_set_route_entries_attribute(object_count, route_entry, attr_list, mode, object_statuses);
    }

    struct RouteOrchTest : public ::testing::Test
    {
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

            // Hack the route create function
            old_create_route_entries = sai_route_api->create_route_entries;
            old_remove_route_entries = sai_route_api->remove_route_entries;
            old_set_route_entries_attribute = sai_route_api->set_route_entries_attribute;

            pold_sai_route_api = sai_route_api;
            ut_sai_route_api = *sai_route_api;
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
            vector<string> srv6_tables = {
                APP_SRV6_SID_LIST_TABLE_NAME,
                APP_SRV6_MY_SID_TABLE_NAME
            };
            gSrv6Orch = new Srv6Orch(m_app_db.get(), srv6_tables, gSwitchOrch, gVrfOrch, gNeighOrch);

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
}
