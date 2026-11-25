#define private public // make Directory::m_values available to clean it.
#include "directory.h"
#undef private
#include "gtest/gtest.h"
#include "ut_helper.h"
#define private public
#include "mock_orchagent_main.h"
#undef private
#include "mock_table.h"
#include <memory>
#include <limits>
#include <vector>
#define protected public
#include "isolationgrouporch.h"
#undef protected

extern sai_isolation_group_api_t *sai_isolation_group_api;



namespace intfsorch_test
{
    using namespace std;

    int create_rif_count = 0;
    int remove_rif_count = 0;
    bool saw_loopback_action = false;
    bool fail_next_rif_set = false;
    int loopback_action_set_count = 0;
    sai_packet_action_t last_loopback_action = SAI_PACKET_ACTION_FORWARD;
    sai_router_interface_api_t *pold_sai_rif_api;
    sai_router_interface_api_t ut_sai_rif_api;

    sai_status_t _ut_create_router_interface(
            _Out_ sai_object_id_t *router_interface_id,
            _In_ sai_object_id_t switch_id,
            _In_ uint32_t attr_count,
            _In_ const sai_attribute_t *attr_list)
    {
        ++create_rif_count;
        return pold_sai_rif_api->create_router_interface(
            router_interface_id, switch_id, attr_count, attr_list);
    }

    sai_status_t _ut_remove_router_interface(
            _In_ sai_object_id_t router_interface_id)
    {
        ++remove_rif_count;
        return pold_sai_rif_api->remove_router_interface(router_interface_id);
    }

    sai_status_t _ut_set_router_interface_attribute(
            _In_ sai_object_id_t router_interface_id,
            _In_ const sai_attribute_t *attr)
    {
        if (attr->id == SAI_ROUTER_INTERFACE_ATTR_LOOPBACK_PACKET_ACTION)
        {
            ++loopback_action_set_count;
            last_loopback_action = static_cast<sai_packet_action_t>(attr->value.s32);
            if (fail_next_rif_set)
            {
                fail_next_rif_set = false;
                return SAI_STATUS_INSUFFICIENT_RESOURCES;
            }
            saw_loopback_action = true;
            return SAI_STATUS_SUCCESS;
        }
        return pold_sai_rif_api->set_router_interface_attribute(
            router_interface_id, attr);
    }

    struct IntfsOrchTest : public ::testing::Test
    {
        shared_ptr<swss::DBConnector> m_app_db;
        shared_ptr<swss::DBConnector> m_config_db;
        shared_ptr<swss::DBConnector> m_state_db;
        shared_ptr<swss::DBConnector> m_chassis_app_db;

        //sai_router_interface_api_t *old_sai_rif_api_ptr;

        //sai_create_router_interface_fn old_create_rif;
        //sai_remove_router_interface_fn old_remove_rif;
        void SetUp() override
        {
            map<string, string> profile = {
                { "SAI_VS_SWITCH_TYPE", "SAI_VS_SWITCH_TYPE_BCM56850" },
                { "KV_DEVICE_MAC_ADDRESS", "20:03:04:05:06:00" }
            };

            ut_helper::initSaiApi(profile);
            pold_sai_rif_api = sai_router_intfs_api;
            ut_sai_rif_api = *sai_router_intfs_api;
            sai_router_intfs_api = &ut_sai_rif_api;

            sai_router_intfs_api->create_router_interface = _ut_create_router_interface;
            sai_router_intfs_api->remove_router_interface = _ut_remove_router_interface;
            sai_router_intfs_api->set_router_interface_attribute = _ut_set_router_interface_attribute;
            saw_loopback_action = false;
            fail_next_rif_set = false;
            loopback_action_set_count = 0;
            last_loopback_action = SAI_PACKET_ACTION_FORWARD;

            m_app_db = make_shared<swss::DBConnector>("APPL_DB", 0);
            m_config_db = make_shared<swss::DBConnector>("CONFIG_DB", 0);
            m_state_db = make_shared<swss::DBConnector>("STATE_DB", 0);
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
            TableConnector stateDbBfdSessionTable(m_state_db.get(), STATE_BFD_SESSION_TABLE_NAME);
            gBfdOrch = new BfdOrch(m_app_db.get(), APP_BFD_SESSION_TABLE_NAME, stateDbBfdSessionTable);

            const int portsorch_base_pri = 40;
            vector<table_name_with_pri_t> ports_tables = {
                { APP_PORT_TABLE_NAME, portsorch_base_pri + 5 },
                { APP_VLAN_TABLE_NAME, portsorch_base_pri + 2 },
                { APP_VLAN_MEMBER_TABLE_NAME, portsorch_base_pri },
                { APP_LAG_TABLE_NAME, portsorch_base_pri + 4 },
                { APP_LAG_MEMBER_TABLE_NAME, portsorch_base_pri }
            };

            vector<string> flex_counter_tables = {
                CFG_FLEX_COUNTER_TABLE_NAME
            };
            auto* flexCounterOrch = new FlexCounterOrch(m_config_db.get(), flex_counter_tables);
            gDirectory.set(flexCounterOrch);

            ASSERT_EQ(gPortsOrch, nullptr);
            gPortsOrch = new PortsOrch(m_app_db.get(), m_state_db.get(), ports_tables, m_chassis_app_db.get());

            vector<string> vnet_tables = {
                APP_VNET_RT_TABLE_NAME,
                APP_VNET_RT_TUNNEL_TABLE_NAME
            };

            vector<string> cfg_vnet_tables = {
                CFG_VNET_RT_TABLE_NAME,
                CFG_VNET_RT_TUNNEL_TABLE_NAME
            };

            auto* vnet_orch = new VNetOrch(m_app_db.get(), APP_VNET_TABLE_NAME);
            gDirectory.set(vnet_orch);
            auto* cfg_vnet_rt_orch = new VNetCfgRouteOrch(m_config_db.get(), m_app_db.get(), cfg_vnet_tables);
            gDirectory.set(cfg_vnet_rt_orch);
            auto* vnet_rt_orch = new VNetRouteOrch(m_app_db.get(), vnet_tables, vnet_orch);
            gDirectory.set(vnet_rt_orch);
            ASSERT_EQ(gVrfOrch, nullptr);
            gVrfOrch = new VRFOrch(m_app_db.get(), APP_VRF_TABLE_NAME, m_state_db.get(), STATE_VRF_OBJECT_TABLE_NAME);
            gDirectory.set(gVrfOrch);

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

            vector<string> tunnel_tables = {
                APP_TUNNEL_DECAP_TABLE_NAME,
                APP_TUNNEL_DECAP_TERM_TABLE_NAME
            };
            auto* tunnel_decap_orch = new TunnelDecapOrch(m_app_db.get(), m_state_db.get(), m_config_db.get(), tunnel_tables);
            vector<string> mux_tables = {
                CFG_MUX_CABLE_TABLE_NAME,
                CFG_PEER_SWITCH_TABLE_NAME
            };
            auto* mux_orch = new MuxOrch(m_config_db.get(), mux_tables, tunnel_decap_orch, gNeighOrch, gFdbOrch);
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

            // Start FlowCounterRouteOrch
            static const  vector<string> route_pattern_tables = {
                CFG_FLOW_COUNTER_ROUTE_PATTERN_TABLE_NAME,
            };
            gFlowCounterRouteOrch = new FlowCounterRouteOrch(m_config_db.get(), route_pattern_tables);

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
            }
            // Set PortConfigDone
            portTable.set("PortConfigDone", { { "count", to_string(ports.size()) } });
            gPortsOrch->addExistingData(&portTable);
            static_cast<Orch *>(gPortsOrch)->doTask();
            portTable.set("PortInitDone", { { "lanes", "0" } });
            gPortsOrch->addExistingData(&portTable);
            static_cast<Orch *>(gPortsOrch)->doTask();
        }

        void TearDown() override
        {
            gDirectory.m_values.clear();

            delete gCrmOrch;
            gCrmOrch = nullptr;

            delete gSwitchOrch;
            gSwitchOrch = nullptr;

            delete gBfdOrch;
            gBfdOrch = nullptr;

            delete gSrv6Orch;
            gSrv6Orch = nullptr;

            delete gNeighOrch;
            gNeighOrch = nullptr;

            delete gFdbOrch;
            gFdbOrch = nullptr;

            delete gPortsOrch;
            gPortsOrch = nullptr;

            delete gIntfsOrch;
            gIntfsOrch = nullptr;

            delete gFgNhgOrch;
            gFgNhgOrch = nullptr;

            delete gRouteOrch;
            gRouteOrch = nullptr;

            delete gNhgOrch;
            gNhgOrch = nullptr;

            delete gBufferOrch;
            gBufferOrch = nullptr;

            delete gVrfOrch;
            gVrfOrch = nullptr;

            delete gFlowCounterRouteOrch;
            gFlowCounterRouteOrch = nullptr;

            sai_router_intfs_api = pold_sai_rif_api;
            ut_helper::uninitSaiApi();
        }
    };

    TEST_F(IntfsOrchTest, IntfsOrchDeleteCreateRetry)
    {
        // create a interface
        std::deque<KeyOpFieldsValuesTuple> entries;
        entries.push_back({"Ethernet0", "SET", { {"mtu", "9100"}}});
        auto consumer = dynamic_cast<Consumer *>(gIntfsOrch->getExecutor(APP_INTF_TABLE_NAME));
        consumer->addToSync(entries);
        auto current_create_count = create_rif_count;
        static_cast<Orch *>(gIntfsOrch)->doTask();
        ASSERT_EQ(current_create_count + 1, create_rif_count);

        // create dependency to the interface
        gIntfsOrch->increaseRouterIntfsRefCount("Ethernet0");

        // delete the interface, expect retry because dependency exists
        entries.clear();
        entries.push_back({"Ethernet0", "DEL", { {} }});
        consumer = dynamic_cast<Consumer *>(gIntfsOrch->getExecutor(APP_INTF_TABLE_NAME));
        consumer->addToSync(entries);
        auto current_remove_count = remove_rif_count;
        static_cast<Orch *>(gIntfsOrch)->doTask();
        ASSERT_EQ(current_remove_count, remove_rif_count);

        // create the interface again, expect retry because interface is in removing
        entries.clear();
        entries.push_back({"Ethernet0", "SET", { {"mtu", "9100"}}});
        consumer = dynamic_cast<Consumer *>(gIntfsOrch->getExecutor(APP_INTF_TABLE_NAME));
        consumer->addToSync(entries);
        current_create_count = create_rif_count;
        static_cast<Orch *>(gIntfsOrch)->doTask();
        ASSERT_EQ(current_create_count, create_rif_count);

        // remove the dependency, expect delete and create a new one
        gIntfsOrch->decreaseRouterIntfsRefCount("Ethernet0");
        current_create_count = create_rif_count;
        current_remove_count = remove_rif_count;
        static_cast<Orch *>(gIntfsOrch)->doTask();
        ASSERT_EQ(current_create_count + 1, create_rif_count);
        ASSERT_EQ(current_remove_count + 1, remove_rif_count);
    };

    TEST_F(IntfsOrchTest, IntfsOrchVrfUpdate)
    {
        //create a new vrf
        std::deque<KeyOpFieldsValuesTuple> entries;
        entries.push_back({"Vrf-Blue", "SET", { {"NULL", "NULL"}}});
        auto consumer = dynamic_cast<Consumer *>(gVrfOrch->getExecutor(APP_VRF_TABLE_NAME));
        consumer->addToSync(entries);
        static_cast<Orch *>(gVrfOrch)->doTask(); 
        ASSERT_TRUE(gVrfOrch->isVRFexists("Vrf-Blue"));
        auto new_vrf_reference_count = gVrfOrch->getVrfRefCount("Vrf-Blue");
        ASSERT_EQ(new_vrf_reference_count, 0);

        // create an interface
        entries.clear();
        entries.push_back({"Loopback2", "SET", {}});
        consumer = dynamic_cast<Consumer *>(gIntfsOrch->getExecutor(APP_INTF_TABLE_NAME));
        consumer->addToSync(entries);
        static_cast<Orch *>(gIntfsOrch)->doTask();
        IntfsTable m_syncdIntfses = gIntfsOrch->getSyncdIntfses();
        ASSERT_EQ(m_syncdIntfses["Loopback2"].vrf_id, gVirtualRouterId);

        // change vrf and check if it worked
        entries.clear();
        entries.push_back({"Loopback2", "SET", { {"vrf_name", "Vrf-Blue"}}});
        consumer = dynamic_cast<Consumer *>(gIntfsOrch->getExecutor(APP_INTF_TABLE_NAME));
        consumer->addToSync(entries);
        static_cast<Orch *>(gIntfsOrch)->doTask();
        auto new_vrf_updated_reference_count = gVrfOrch->getVrfRefCount("Vrf-Blue");
        ASSERT_EQ(new_vrf_reference_count + 1, new_vrf_updated_reference_count);
        m_syncdIntfses = gIntfsOrch->getSyncdIntfses();
        ASSERT_EQ(m_syncdIntfses["Loopback2"].vrf_id, gVrfOrch->getVRFid("Vrf-Blue"));

        // create an interface
        entries.clear();
        entries.push_back({"Loopback3", "SET", {}});
        consumer = dynamic_cast<Consumer *>(gIntfsOrch->getExecutor(APP_INTF_TABLE_NAME));
        consumer->addToSync(entries);
        static_cast<Orch *>(gIntfsOrch)->doTask();
        m_syncdIntfses = gIntfsOrch->getSyncdIntfses();
        ASSERT_EQ(m_syncdIntfses["Loopback3"].vrf_id, gVirtualRouterId);

        // Add IP address to the interface
        entries.clear();
        entries.push_back({"Loopback3:3.3.3.3/32", "SET", {{"scope", "global"},{"family", "IPv4"}}});
        consumer = dynamic_cast<Consumer *>(gIntfsOrch->getExecutor(APP_INTF_TABLE_NAME));
        consumer->addToSync(entries);
        static_cast<Orch *>(gIntfsOrch)->doTask();

        // change vrf and check it doesn't affect the interface due to existing IP
        entries.clear();
        entries.push_back({"Loopback3", "SET", { {"vrf_name", "Vrf-Blue"}}});
        consumer = dynamic_cast<Consumer *>(gIntfsOrch->getExecutor(APP_INTF_TABLE_NAME));
        consumer->addToSync(entries);
        static_cast<Orch *>(gIntfsOrch)->doTask();
        m_syncdIntfses = gIntfsOrch->getSyncdIntfses();
        ASSERT_EQ(m_syncdIntfses["Loopback3"].vrf_id, gVirtualRouterId);    
    }

    TEST_F(IntfsOrchTest, IntfsOrchSameInterfaceOverlapRetry)
    {
        // Create an interface (RIF) on Ethernet0
        std::deque<KeyOpFieldsValuesTuple> entries;
        entries.push_back({"Ethernet0", "SET", { {"mtu", "9100"}}});
        auto consumer = dynamic_cast<Consumer *>(gIntfsOrch->getExecutor(APP_INTF_TABLE_NAME));
        consumer->addToSync(entries);
        auto current_create_count = create_rif_count;
        static_cast<Orch *>(gIntfsOrch)->doTask();
        ASSERT_EQ(current_create_count + 1, create_rif_count);

        // Simulate the temporary /8 address installed during a netmask change.
        entries.clear();
        entries.push_back({"Ethernet0:10.0.0.1/8", "SET", {{"scope", "global"},{"family", "IPv4"}}});
        consumer = dynamic_cast<Consumer *>(gIntfsOrch->getExecutor(APP_INTF_TABLE_NAME));
        consumer->addToSync(entries);
        static_cast<Orch *>(gIntfsOrch)->doTask();

        auto m_syncdIntfses = gIntfsOrch->getSyncdIntfses();
        ASSERT_EQ(m_syncdIntfses["Ethernet0"].ip_addresses.count(IpPrefix("10.0.0.1/8")), 1);

        // Retry a same-interface overlap until the original address is removed.
        entries.clear();
        entries.push_back({"Ethernet0:10.0.0.1/24", "SET", {{"scope", "global"},{"family", "IPv4"}}});
        consumer = dynamic_cast<Consumer *>(gIntfsOrch->getExecutor(APP_INTF_TABLE_NAME));
        consumer->addToSync(entries);
        static_cast<Orch *>(gIntfsOrch)->doTask();

        ASSERT_EQ(consumer->m_toSync.count("Ethernet0:10.0.0.1/24"), 1);

        m_syncdIntfses = gIntfsOrch->getSyncdIntfses();
        ASSERT_EQ(m_syncdIntfses["Ethernet0"].ip_addresses.count(IpPrefix("10.0.0.1/24")), 0);
        ASSERT_EQ(m_syncdIntfses["Ethernet0"].ip_addresses.count(IpPrefix("10.0.0.1/8")), 1);

        static_cast<Orch *>(gIntfsOrch)->doTask();
        ASSERT_EQ(consumer->m_toSync.count("Ethernet0:10.0.0.1/24"), 1);

        entries.clear();
        entries.push_back({"Ethernet0:10.0.0.1/8", "DEL", {}});
        consumer->addToSync(entries);
        static_cast<Orch *>(gIntfsOrch)->doTask();
        static_cast<Orch *>(gIntfsOrch)->doTask();

        ASSERT_TRUE(consumer->m_toSync.empty());
        m_syncdIntfses = gIntfsOrch->getSyncdIntfses();
        ASSERT_EQ(m_syncdIntfses["Ethernet0"].ip_addresses.count(IpPrefix("10.0.0.1/8")), 0);
        ASSERT_EQ(m_syncdIntfses["Ethernet0"].ip_addresses.count(IpPrefix("10.0.0.1/24")), 1);
    }

    struct OverlapTestCase
    {
        string first_alias;
        string first_prefix;
        string second_alias;
        string second_prefix;
    };

    struct IntfsOrchCrossInterfaceOverlapTest :
        public IntfsOrchTest,
        public ::testing::WithParamInterface<OverlapTestCase>
    {
    };

    TEST_P(IntfsOrchCrossInterfaceOverlapTest, RejectOverlapWithoutRetry)
    {
        const auto &test = GetParam();
        auto consumer = dynamic_cast<Consumer *>(gIntfsOrch->getExecutor(APP_INTF_TABLE_NAME));
        ASSERT_NE(consumer, nullptr);

        if (test.first_alias == "Vlan100" || test.second_alias == "Vlan100")
        {
            Table vlan_table(m_app_db.get(), APP_VLAN_TABLE_NAME);
            vlan_table.set("Vlan100", {{"admin_status", "up"}, {"mtu", "9100"}});
            gPortsOrch->addExistingData(&vlan_table);
            static_cast<Orch *>(gPortsOrch)->doTask();
            Port port;
            ASSERT_TRUE(gPortsOrch->getPort("Vlan100", port));
        }

        std::deque<KeyOpFieldsValuesTuple> entries = {
            {test.first_alias, "SET", {{"mtu", "9100"}}},
            {test.second_alias, "SET", {{"mtu", "9100"}}}
        };
        consumer->addToSync(entries);
        static_cast<Orch *>(gIntfsOrch)->doTask();
        ASSERT_TRUE(consumer->m_toSync.empty());

        const string family = IpPrefix(test.first_prefix).isV4() ? "IPv4" : "IPv6";
        entries = {{test.first_alias + ":" + test.first_prefix, "SET",
                    {{"scope", "global"}, {"family", family}}}};
        consumer->addToSync(entries);
        static_cast<Orch *>(gIntfsOrch)->doTask();
        ASSERT_TRUE(consumer->m_toSync.empty());
        ASSERT_EQ(gIntfsOrch->getSyncdIntfses().at(test.first_alias).ip_addresses.count(IpPrefix(test.first_prefix)), 1);

        entries = {{test.second_alias + ":" + test.second_prefix, "SET",
                    {{"scope", "global"}, {"family", family}}}};
        consumer->addToSync(entries);
        static_cast<Orch *>(gIntfsOrch)->doTask();

        ASSERT_TRUE(consumer->m_toSync.empty());
        ASSERT_EQ(gIntfsOrch->getSyncdIntfses().at(test.second_alias).ip_addresses.size(), 0);
        ASSERT_EQ(gIntfsOrch->getSyncdIntfses().at(test.first_alias).ip_addresses.count(IpPrefix(test.first_prefix)), 1);

        static_cast<Orch *>(gIntfsOrch)->doTask();
        ASSERT_TRUE(consumer->m_toSync.empty());
        ASSERT_EQ(gIntfsOrch->getSyncdIntfses().at(test.second_alias).ip_addresses.size(), 0);

        // Removing the accepted address does not resurrect the rejected SET.
        entries = {{test.first_alias + ":" + test.first_prefix, "DEL", {}}};
        consumer->addToSync(entries);
        static_cast<Orch *>(gIntfsOrch)->doTask();
        ASSERT_TRUE(consumer->m_toSync.empty());
        ASSERT_TRUE(gIntfsOrch->getSyncdIntfses().at(test.first_alias).ip_addresses.empty());
        ASSERT_TRUE(gIntfsOrch->getSyncdIntfses().at(test.second_alias).ip_addresses.empty());

        entries = {{test.second_alias + ":" + test.second_prefix, "SET",
                    {{"scope", "global"}, {"family", family}}}};
        consumer->addToSync(entries);
        static_cast<Orch *>(gIntfsOrch)->doTask();
        ASSERT_TRUE(consumer->m_toSync.empty());
        ASSERT_EQ(gIntfsOrch->getSyncdIntfses().at(test.second_alias).ip_addresses.count(IpPrefix(test.second_prefix)), 1);
    }

    INSTANTIATE_TEST_CASE_P(
        IntfsOrchOverlap,
        IntfsOrchCrossInterfaceOverlapTest,
        ::testing::Values(
            OverlapTestCase{"Ethernet0", "10.0.0.1/24", "Ethernet4", "10.0.0.2/24"},
            OverlapTestCase{"Ethernet4", "10.0.0.2/24", "Ethernet0", "10.0.0.1/24"},
            OverlapTestCase{"Ethernet0", "2603:10b2:50a:1801:0:207c:200:1/126",
                           "Vlan100", "2603:10b2:50a:1801:0:207c:200:1/104"},
            OverlapTestCase{"Vlan100", "2603:10b2:50a:1801:0:207c:200:1/104",
                           "Ethernet0", "2603:10b2:50a:1801:0:207c:200:1/126"}));

    TEST_F(IntfsOrchTest, IntfsOrchOverlapAcrossVrfs)
    {
        std::deque<KeyOpFieldsValuesTuple> entries = {
            {"Vrf-Blue", "SET", {{"NULL", "NULL"}}}
        };
        auto vrf_consumer = dynamic_cast<Consumer *>(gVrfOrch->getExecutor(APP_VRF_TABLE_NAME));
        ASSERT_NE(vrf_consumer, nullptr);
        vrf_consumer->addToSync(entries);
        static_cast<Orch *>(gVrfOrch)->doTask();
        ASSERT_TRUE(gVrfOrch->isVRFexists("Vrf-Blue"));

        auto consumer = dynamic_cast<Consumer *>(gIntfsOrch->getExecutor(APP_INTF_TABLE_NAME));
        ASSERT_NE(consumer, nullptr);
        entries = {
            {"Ethernet0", "SET", {{"mtu", "9100"}}},
            {"Ethernet4", "SET", {{"mtu", "9100"}, {"vrf_name", "Vrf-Blue"}}}
        };
        consumer->addToSync(entries);
        static_cast<Orch *>(gIntfsOrch)->doTask();
        ASSERT_TRUE(consumer->m_toSync.empty());
        ASSERT_NE(gIntfsOrch->getSyncdIntfses().at("Ethernet0").vrf_id,
                  gIntfsOrch->getSyncdIntfses().at("Ethernet4").vrf_id);

        entries = {
            {"Ethernet0:10.0.0.1/24", "SET", {{"scope", "global"}, {"family", "IPv4"}}},
            {"Ethernet4:10.0.0.1/24", "SET",
             {{"scope", "global"}, {"family", "IPv4"}, {"vrf_name", "Vrf-Blue"}}}
        };
        consumer->addToSync(entries);
        static_cast<Orch *>(gIntfsOrch)->doTask();
        ASSERT_TRUE(consumer->m_toSync.empty());
        ASSERT_EQ(gIntfsOrch->getSyncdIntfses().at("Ethernet0").ip_addresses.count(IpPrefix("10.0.0.1/24")), 1);
        ASSERT_EQ(gIntfsOrch->getSyncdIntfses().at("Ethernet4").ip_addresses.count(IpPrefix("10.0.0.1/24")), 1);

        // An exact duplicate SET must remain idempotent.
        consumer->addToSync(entries);
        static_cast<Orch *>(gIntfsOrch)->doTask();
        ASSERT_TRUE(consumer->m_toSync.empty());
        ASSERT_EQ(gIntfsOrch->getSyncdIntfses().at("Ethernet0").ip_addresses.size(), 1);
        ASSERT_EQ(gIntfsOrch->getSyncdIntfses().at("Ethernet4").ip_addresses.size(), 1);
    }

    TEST_F(IntfsOrchTest, IntfsOrchSagEnableDisable)
    {
        std::deque<KeyOpFieldsValuesTuple> entries;

        entries.push_back({"GLOBAL", "SET", {{"gateway_mac", "02:03:04:05:06:07"}}});
        auto sagConsumer = dynamic_cast<Consumer *>(gIntfsOrch->getExecutor(APP_SAG_TABLE_NAME));
        ASSERT_NE(sagConsumer, nullptr);
        sagConsumer->addToSync(entries);
        static_cast<Orch *>(gIntfsOrch)->doTask();

        entries.clear();
        entries.push_back({"Ethernet0", "SET", {{"mtu", "9100"}, {"static_anycast_gateway", "true"}}});
        auto intfConsumer = dynamic_cast<Consumer *>(gIntfsOrch->getExecutor(APP_INTF_TABLE_NAME));
        ASSERT_NE(intfConsumer, nullptr);
        intfConsumer->addToSync(entries);
        static_cast<Orch *>(gIntfsOrch)->doTask();

        auto syncdIntfses = gIntfsOrch->getSyncdIntfses();
        ASSERT_NE(syncdIntfses.find("Ethernet0"), syncdIntfses.end());
        EXPECT_TRUE(syncdIntfses.at("Ethernet0").sag_enabled);

        entries.clear();
        entries.push_back({"Ethernet0", "SET", {{"static_anycast_gateway", "false"}}});
        intfConsumer->addToSync(entries);
        static_cast<Orch *>(gIntfsOrch)->doTask();

        syncdIntfses = gIntfsOrch->getSyncdIntfses();
        ASSERT_NE(syncdIntfses.find("Ethernet0"), syncdIntfses.end());
        EXPECT_FALSE(syncdIntfses.at("Ethernet0").sag_enabled);

        entries.clear();
        entries.push_back({"GLOBAL", "DEL", {}});
        sagConsumer->addToSync(entries);
        static_cast<Orch *>(gIntfsOrch)->doTask();
    }

    // Regression test for the batched IP-removal + VRF-bind race.
    // m_toSync is an ordered multimap, so within a single drain the bare interface
    // key ("Loopback6") is processed before the per-IP key ("Loopback6:6.6.6.6/32").
    // When a config sequence removes the loopback IPs and rebinds the interface to a
    // VRF back-to-back, both land in one batch and the VRF-change SET is evaluated
    // while the IP is still present. The fix defers (retains) that SET instead of
    // logging an error and dropping it, so the bind converges on a later drain.
    TEST_F(IntfsOrchTest, IntfsOrchVrfBindDeferredUntilIpRemoved)
    {
        // create a new vrf
        std::deque<KeyOpFieldsValuesTuple> entries;
        entries.push_back({"Vrf-Blue", "SET", { {"NULL", "NULL"}}});
        auto vrfConsumer = dynamic_cast<Consumer *>(gVrfOrch->getExecutor(APP_VRF_TABLE_NAME));
        vrfConsumer->addToSync(entries);
        static_cast<Orch *>(gVrfOrch)->doTask();
        ASSERT_TRUE(gVrfOrch->isVRFexists("Vrf-Blue"));
        auto base_vrf_ref = gVrfOrch->getVrfRefCount("Vrf-Blue");

        auto intfConsumer = dynamic_cast<Consumer *>(gIntfsOrch->getExecutor(APP_INTF_TABLE_NAME));

        // create a loopback in the default vrf and give it an IP address
        entries.clear();
        entries.push_back({"Loopback6", "SET", {}});
        entries.push_back({"Loopback6:6.6.6.6/32", "SET", {{"scope", "global"}, {"family", "IPv4"}}});
        intfConsumer->addToSync(entries);
        static_cast<Orch *>(gIntfsOrch)->doTask();
        auto syncd = gIntfsOrch->getSyncdIntfses();
        ASSERT_EQ(syncd["Loopback6"].vrf_id, gVirtualRouterId);
        ASSERT_EQ(syncd["Loopback6"].ip_addresses.size(), static_cast<size_t>(1));

        // Single batch: remove the IP AND rebind the interface to Vrf-Blue.
        // The bare "Loopback6" SET sorts before "Loopback6:6.6.6.6/32" DEL, so the
        // bind is evaluated first (IP still present) and must be deferred, not dropped.
        entries.clear();
        entries.push_back({"Loopback6", "SET", { {"vrf_name", "Vrf-Blue"}}});
        entries.push_back({"Loopback6:6.6.6.6/32", "DEL", {}});
        intfConsumer->addToSync(entries);
        static_cast<Orch *>(gIntfsOrch)->doTask();

        // After the first drain: IP removed, interface still present and still in the
        // default vrf (the bind is pending, not lost).
        syncd = gIntfsOrch->getSyncdIntfses();
        ASSERT_NE(syncd.find("Loopback6"), syncd.end());
        ASSERT_EQ(syncd["Loopback6"].ip_addresses.size(), static_cast<size_t>(0));
        ASSERT_EQ(syncd["Loopback6"].vrf_id, gVirtualRouterId);

        // The deferred bind is retried on the next drain and now succeeds.
        static_cast<Orch *>(gIntfsOrch)->doTask();
        syncd = gIntfsOrch->getSyncdIntfses();
        ASSERT_EQ(syncd["Loopback6"].vrf_id, gVrfOrch->getVRFid("Vrf-Blue"));
        ASSERT_EQ(gVrfOrch->getVrfRefCount("Vrf-Blue"), base_vrf_ref + 1);
    }

    TEST_F(IntfsOrchTest, IntfsOrchRetriesLoopbackActionSetFailure)
    {
        std::deque<KeyOpFieldsValuesTuple> entries{
            {"Ethernet0", "SET", {{"mtu", "9100"}}}
        };
        auto consumer = dynamic_cast<Consumer *>(gIntfsOrch->getExecutor(APP_INTF_TABLE_NAME));
        ASSERT_NE(consumer, nullptr);
        consumer->addToSync(entries);
        static_cast<Orch *>(gIntfsOrch)->doTask();

        fail_next_rif_set = true;
        entries = {
            {"Ethernet0", "SET", {{"loopback_action", "drop"}}}
        };
        consumer->addToSync(entries);
        static_cast<Orch *>(gIntfsOrch)->doTask();

        ASSERT_EQ(consumer->m_toSync.size(), 1u);
        ASSERT_EQ(loopback_action_set_count, 1);
        ASSERT_FALSE(saw_loopback_action);

        static_cast<Orch *>(gIntfsOrch)->doTask();

        ASSERT_TRUE(consumer->m_toSync.empty());
        ASSERT_EQ(loopback_action_set_count, 2);
        ASSERT_TRUE(saw_loopback_action);
        ASSERT_EQ(last_loopback_action, SAI_PACKET_ACTION_DROP);
    }

    TEST_F(IntfsOrchTest, IntfsOrchIgnoresInvalidLoopbackActionField)
    {
        std::deque<KeyOpFieldsValuesTuple> entries{
            {"Ethernet0", "SET", {{"mtu", "9100"}}}
        };
        auto consumer = dynamic_cast<Consumer *>(gIntfsOrch->getExecutor(APP_INTF_TABLE_NAME));
        ASSERT_NE(consumer, nullptr);
        consumer->addToSync(entries);
        static_cast<Orch *>(gIntfsOrch)->doTask();

        entries = {
            {"Ethernet0", "SET", {
                {"loopback_action", "invalid"},
                {"nat_zone", "7"}
            }}
        };
        consumer->addToSync(entries);
        static_cast<Orch *>(gIntfsOrch)->doTask();

        ASSERT_TRUE(consumer->m_toSync.empty());
        ASSERT_EQ(loopback_action_set_count, 0);
        ASSERT_FALSE(saw_loopback_action);

        Port port;
        ASSERT_TRUE(gPortsOrch->getPort("Ethernet0", port));
        ASSERT_EQ(port.m_nat_zone_id, 7u);
    }

    /*
     * vlanmgrd writes the VLAN_MEMBER DEL and intfmgrd writes the INTF SET
     * independently, so orchagent can see the router interface request for a
     * port before the port has left its VLAN. removeBridgePort() calls into
     * gStpOrch, which the base fixture does not create.
     */
    struct IntfsOrchVlanMemberRaceTest : IntfsOrchTest
    {
        const string m_port = "Ethernet0";
        const string m_vlan = "Vlan1000";

        void SetUp() override
        {
            IntfsOrchTest::SetUp();
            vector<string> stp_tables = {
                "STP_TABLE",
                "STP_VLAN_INSTANCE_TABLE",
                "STP_PORT_TABLE",
                "STP_PORT_STATE_TABLE",
                "STP_FASTAGEING_FLUSH_TABLE",
                "STP_INST_PORT_FLUSH_TABLE"
            };
            gStpOrch = new StpOrch(m_app_db.get(), m_state_db.get(), stp_tables);
        }

        void TearDown() override
        {
            delete gStpOrch;
            gStpOrch = nullptr;
            IntfsOrchTest::TearDown();
        }

        Consumer *consumer(Orch *orch, const string &table)
        {
            return dynamic_cast<Consumer *>(orch->getExecutor(table));
        }

        void addUntaggedMember()
        {
            consumer(gPortsOrch, APP_VLAN_TABLE_NAME)->addToSync(
                std::deque<KeyOpFieldsValuesTuple>{{m_vlan, SET_COMMAND, {{"admin_status", "up"}, {"mtu", "9100"}}}});
            consumer(gPortsOrch, APP_VLAN_MEMBER_TABLE_NAME)->addToSync(
                std::deque<KeyOpFieldsValuesTuple>{{m_vlan + ":" + m_port, SET_COMMAND, {{"tagging_mode", "untagged"}}}});
            static_cast<Orch *>(gPortsOrch)->doTask();

            Port port, vlan;
            ASSERT_TRUE(gPortsOrch->getPort(m_port, port));
            ASSERT_TRUE(gPortsOrch->getPort(m_vlan, vlan));
            ASSERT_TRUE(gPortsOrch->isVlanMember(vlan, port));
            ASSERT_NE(port.m_bridge_port_id, SAI_NULL_OBJECT_ID);
            ASSERT_EQ(port.m_port_vlan_id, 1000);
        }

        void queueMemberDel()
        {
            consumer(gPortsOrch, APP_VLAN_MEMBER_TABLE_NAME)->addToSync(
                std::deque<KeyOpFieldsValuesTuple>{{m_vlan + ":" + m_port, DEL_COMMAND, {}}});
        }

        void queueIpAdd()
        {
            consumer(gIntfsOrch, APP_INTF_TABLE_NAME)->addToSync(
                std::deque<KeyOpFieldsValuesTuple>{
                    {m_port, SET_COMMAND, {{"mtu", "9100"}}},
                    {m_port + ":100.100.93.254/24", SET_COMMAND, {{"scope", "global"}, {"family", "IPv4"}}}});
        }

        /* The port ends up routed only: out of the VLAN, no bridge port, default PVID, RIF with its IP. */
        void expectRoutedPort()
        {
            ASSERT_TRUE(consumer(gPortsOrch, APP_VLAN_MEMBER_TABLE_NAME)->m_toSync.empty());
            ASSERT_TRUE(consumer(gIntfsOrch, APP_INTF_TABLE_NAME)->m_toSync.empty());

            Port port, vlan;
            ASSERT_TRUE(gPortsOrch->getPort(m_port, port));
            ASSERT_TRUE(gPortsOrch->getPort(m_vlan, vlan));
            ASSERT_FALSE(gPortsOrch->isVlanMember(vlan, port));
            ASSERT_EQ(port.m_bridge_port_id, SAI_NULL_OBJECT_ID);
            ASSERT_NE(port.m_rif_id, SAI_NULL_OBJECT_ID);
            ASSERT_EQ(port.m_port_vlan_id, DEFAULT_PORT_VLAN_ID);

            sai_attribute_t attr;
            attr.id = SAI_PORT_ATTR_PORT_VLAN_ID;
            ASSERT_EQ(sai_port_api->get_port_attribute(port.m_port_id, 1, &attr), SAI_STATUS_SUCCESS);
            ASSERT_EQ(attr.value.u16, DEFAULT_PORT_VLAN_ID);

            auto syncd = gIntfsOrch->getSyncdIntfses();
            ASSERT_EQ(syncd[m_port].ip_addresses.size(), static_cast<size_t>(1));
        }
    };

    TEST_F(IntfsOrchVlanMemberRaceTest, VlanMemberDelProcessedBeforeIpAdd)
    {
        ASSERT_NO_FATAL_FAILURE(addUntaggedMember());
        queueMemberDel();
        queueIpAdd();

        auto rif_creates = create_rif_count;
        static_cast<Orch *>(gPortsOrch)->doTask();
        static_cast<Orch *>(gIntfsOrch)->doTask();
        ASSERT_EQ(create_rif_count, rif_creates + 1);

        ASSERT_NO_FATAL_FAILURE(expectRoutedPort());
    }

    TEST_F(IntfsOrchVlanMemberRaceTest, IpAddProcessedBeforeVlanMemberDel)
    {
        ASSERT_NO_FATAL_FAILURE(addUntaggedMember());
        queueMemberDel();
        queueIpAdd();

        /* IntfsOrch drains first: the router interface must wait for the port to leave the VLAN. */
        auto rif_creates = create_rif_count;
        static_cast<Orch *>(gIntfsOrch)->doTask();
        ASSERT_EQ(create_rif_count, rif_creates);
        ASSERT_FALSE(consumer(gIntfsOrch, APP_INTF_TABLE_NAME)->m_toSync.empty());

        static_cast<Orch *>(gPortsOrch)->doTask();
        ASSERT_TRUE(consumer(gPortsOrch, APP_VLAN_MEMBER_TABLE_NAME)->m_toSync.empty());

        static_cast<Orch *>(gIntfsOrch)->doTask();
        ASSERT_EQ(create_rif_count, rif_creates + 1);

        ASSERT_NO_FATAL_FAILURE(expectRoutedPort());
    }

    // Keep real SAI objects, but control the final bridge-port removal result.
    // This extends the VLAN/IP ordering tests to the interval after membership
    // is gone and before the standalone bridge port has actually retired.
    struct IntfsOrchBridgeRetirementTest : IntfsOrchVlanMemberRaceTest
    {
        static IntfsOrchBridgeRetirementTest *active;
        sai_bridge_api_t *savedBridge = nullptr;
        sai_hostif_api_t *savedHostif = nullptr;
        sai_fdb_api_t *savedFdb = nullptr;
        sai_lag_api_t *savedLag = nullptr;
        sai_stp_api_t *savedStp = nullptr;
        sai_bridge_api_t bridgeApi{};
        sai_hostif_api_t hostifApi{};
        sai_fdb_api_t fdbApi{};
        sai_lag_api_t lagApi{};
        sai_stp_api_t stpApi{};
        sai_object_id_t bridge = SAI_NULL_OBJECT_ID;
        sai_object_id_t hostif = SAI_NULL_OBJECT_ID;
        sai_object_id_t physicalPort = SAI_NULL_OBJECT_ID;
        sai_status_t removalStatus = SAI_STATUS_OBJECT_IN_USE;
        sai_status_t stripStatus = SAI_STATUS_SUCCESS;
        sai_status_t adminStatus = SAI_STATUS_SUCCESS;
        sai_status_t flushStatus = SAI_STATUS_SUCCESS;
        sai_status_t stpStatus = SAI_STATUS_SUCCESS;
        sai_object_id_t stpPort = SAI_NULL_OBJECT_ID;
        std::map<sai_object_id_t, unsigned> stpRemovals;
        std::function<void()> afterAdminDown;
        unsigned removals = 0;
        unsigned adminDowns = 0;
        unsigned tagStrips = 0;
        unsigned bridgeFlushes = 0;
        unsigned lagMemberCreates = 0;
        uint64_t now = 1000;

        void SetUp() override
        {
            IntfsOrchVlanMemberRaceTest::SetUp();
            gPortsOrch->m_bridgePortRemovalNow = [this] { return now; };
            savedBridge = sai_bridge_api;
            savedHostif = sai_hostif_api;
            savedFdb = sai_fdb_api;
            savedLag = sai_lag_api;
            savedStp = sai_stp_api;
            bridgeApi = *savedBridge;
            hostifApi = *savedHostif;
            fdbApi = *savedFdb;
            lagApi = *savedLag;
            stpApi = *savedStp;
            active = this;
            bridgeApi.remove_bridge_port = removeBridge;
            bridgeApi.set_bridge_port_attribute = setBridge;
            hostifApi.set_hostif_attribute = setHostif;
            fdbApi.flush_fdb_entries = flushFdb;
            lagApi.create_lag_member = createLagMember;
            stpApi.remove_stp_port = removeStpPort;
            sai_bridge_api = &bridgeApi;
            sai_hostif_api = &hostifApi;
            sai_fdb_api = &fdbApi;
            sai_lag_api = &lagApi;
            sai_stp_api = &stpApi;
        }

        void TearDown() override
        {
            sai_bridge_api = savedBridge;
            sai_hostif_api = savedHostif;
            sai_fdb_api = savedFdb;
            sai_lag_api = savedLag;
            sai_stp_api = savedStp;
            active = nullptr;
            IntfsOrchVlanMemberRaceTest::TearDown();
        }

        static sai_status_t removeBridge(sai_object_id_t oid)
        {
            if (oid == active->bridge)
            {
                ++active->removals;
                if (active->removalStatus != SAI_STATUS_SUCCESS)
                    return active->removalStatus;
            }
            return active->savedBridge->remove_bridge_port(oid);
        }

        static sai_status_t setBridge(sai_object_id_t oid, const sai_attribute_t *attr)
        {
            if (oid == active->bridge && attr->id == SAI_BRIDGE_PORT_ATTR_ADMIN_STATE && !attr->value.booldata)
            {
                ++active->adminDowns;
                if (active->adminStatus != SAI_STATUS_SUCCESS) return active->adminStatus;
            }
            const auto status = active->savedBridge->set_bridge_port_attribute(oid, attr);
            if (status == SAI_STATUS_SUCCESS && oid == active->bridge &&
                attr->id == SAI_BRIDGE_PORT_ATTR_ADMIN_STATE && !attr->value.booldata && active->afterAdminDown)
            {
                auto action = std::move(active->afterAdminDown);
                action();
            }
            return status;
        }

        static sai_status_t setHostif(sai_object_id_t oid, const sai_attribute_t *attr)
        {
            if (oid == active->hostif && attr->id == SAI_HOSTIF_ATTR_VLAN_TAG &&
                attr->value.s32 == SAI_HOSTIF_VLAN_TAG_STRIP)
            {
                ++active->tagStrips;
                if (active->stripStatus != SAI_STATUS_SUCCESS) return active->stripStatus;
            }
            return active->savedHostif->set_hostif_attribute(oid, attr);
        }

        static sai_status_t flushFdb(sai_object_id_t switchId, uint32_t count, const sai_attribute_t *attrs)
        {
            bool targetBridge = false, vlanScoped = false;
            for (uint32_t i = 0; i < count; ++i)
            {
                if (attrs[i].id == SAI_FDB_FLUSH_ATTR_BRIDGE_PORT_ID && attrs[i].value.oid == active->bridge)
                    targetBridge = true;
                if (attrs[i].id == SAI_FDB_FLUSH_ATTR_BV_ID)
                    vlanScoped = true;
            }
            if (targetBridge && !vlanScoped)
            {
                ++active->bridgeFlushes;
                if (active->flushStatus != SAI_STATUS_SUCCESS) return active->flushStatus;
            }
            return active->savedFdb->flush_fdb_entries(switchId, count, attrs);
        }

        static sai_status_t removeStpPort(sai_object_id_t oid)
        {
            ++active->stpRemovals[oid];
            if (oid == active->stpPort && active->stpStatus != SAI_STATUS_SUCCESS)
                return active->stpStatus;
            return active->savedStp->remove_stp_port(oid);
        }

        static sai_status_t createLagMember(sai_object_id_t *oid, sai_object_id_t switchId,
                                           uint32_t count, const sai_attribute_t *attrs)
        {
            for (uint32_t i = 0; i < count; ++i)
            {
                if (attrs[i].id == SAI_LAG_MEMBER_ATTR_PORT_ID && attrs[i].value.oid == active->physicalPort)
                {
                    ++active->lagMemberCreates;
                    Port port;
                    if (gPortsOrch->getPort(active->m_port, port) && port.m_bridge_port_id != SAI_NULL_OBJECT_ID)
                        // Record forbidden admission without mutating an invalid
                        // bridged/LAG object combination in the real SAI model.
                        return SAI_STATUS_INSUFFICIENT_RESOURCES;
                }
            }
            return active->savedLag->create_lag_member(oid, switchId, count, attrs);
        }

        void watchBridge(sai_status_t status = SAI_STATUS_OBJECT_IN_USE)
        {
            Port port;
            ASSERT_TRUE(gPortsOrch->getPort(m_port, port));
            bridge = port.m_bridge_port_id;
            hostif = port.m_hif_id;
            physicalPort = port.m_port_id;
            ASSERT_NE(bridge, SAI_NULL_OBJECT_ID);
            removalStatus = status;
        }

        void beginRetirement(sai_status_t status = SAI_STATUS_OBJECT_IN_USE)
        {
            ASSERT_NO_FATAL_FAILURE(watchBridge(status));
            queueMemberDel();
            static_cast<Orch *>(gPortsOrch)->doTask();
            ASSERT_EQ(removals, 1u);
            Port port;
            ASSERT_TRUE(gPortsOrch->getPort(m_port, port));
            EXPECT_EQ(port.m_bridge_port_id, bridge);
            vlan_members_t members;
            gPortsOrch->getPortVlanMembers(port, members);
            ASSERT_TRUE(members.empty());
        }

        void addStpOwner(sai_object_id_t instance, uint16_t slot, sai_object_id_t &owner)
        {
            sai_attribute_t attrs[3]{};
            attrs[0].id = SAI_STP_PORT_ATTR_STP;
            attrs[0].value.oid = instance;
            attrs[1].id = SAI_STP_PORT_ATTR_BRIDGE_PORT;
            attrs[1].value.oid = bridge;
            attrs[2].id = SAI_STP_PORT_ATTR_STATE;
            attrs[2].value.s32 = SAI_STP_PORT_STATE_FORWARDING;
            ASSERT_EQ(savedStp->create_stp_port(&owner, gSwitchId, 3, attrs), SAI_STATUS_SUCCESS);
            Port port;
            ASSERT_TRUE(gPortsOrch->getPort(m_port, port));
            port.m_stp_port_ids[slot] = owner;
            gPortsOrch->setPort(m_port, port);
        }

        bool waitForBridgeRemoval()
        {
            for (unsigned i = 0; i < 20; ++i)
            {
                now += 5000;
                static_cast<Orch *>(gPortsOrch)->doTask();
                Port port;
                if (gPortsOrch->getPort(m_port, port) && port.m_bridge_port_id == SAI_NULL_OBJECT_ID)
                    return true;
            }
            return false;
        }
    };

    IntfsOrchBridgeRetirementTest *IntfsOrchBridgeRetirementTest::active = nullptr;

    struct IntfsOrchIsolationRetirementTest : IntfsOrchBridgeRetirementTest,
        ::testing::WithParamInterface<unsigned>
    {
        sai_isolation_group_api_t *previousIsolation = nullptr;

        void SetUp() override
        {
            IntfsOrchBridgeRetirementTest::SetUp();
            // The common native fixture does not query the isolation API.
            previousIsolation = sai_isolation_group_api;
            ASSERT_EQ(sai_api_query(SAI_API_ISOLATION_GROUP,
                reinterpret_cast<void **>(&sai_isolation_group_api)), SAI_STATUS_SUCCESS);
        }

        void TearDown() override
        {
            sai_isolation_group_api = previousIsolation;
            IntfsOrchBridgeRetirementTest::TearDown();
        }
    };

    TEST_P(IntfsOrchIsolationRetirementTest, RealSaiReferencesRetireAndRestoreAcrossBridgeRecreation)
    {
        ASSERT_NO_FATAL_FAILURE(addUntaggedMember());
        ASSERT_NO_FATAL_FAILURE(watchBridge());
        const bool bind = GetParam() & 1u;
        const bool member = GetParam() & 2u;
        vector<TableConnector> connectors{{m_app_db.get(), APP_ISOLATION_GROUP_TABLE_NAME}};
        IsoGrpOrch isolation(connectors);
        struct Cleanup
        {
            IsoGrpOrch &isolation;
            ~Cleanup()
            {
                gPortsOrch->detach(&isolation);
                isolation.delIsolationGroup("BridgeRetirement");
            }
        } cleanup{isolation};
        ASSERT_EQ(isolation.addIsolationGroup("BridgeRetirement", ISOLATION_GROUP_TYPE_BRIDGE_PORT,
            "", bind ? m_port : "", member ? m_port : ""), ISO_GRP_STATUS_SUCCESS);
        auto group = isolation.getIsolationGroup("BridgeRetirement");
        ASSERT_NE(group, nullptr);
        ASSERT_EQ(group->m_members.size(), member ? 1u : 0u);
        ASSERT_EQ(group->m_bind_ports.size(), bind ? 1u : 0u);
        const auto groupOid = group->m_oid;

        Port port, vlan;
        ASSERT_TRUE(gPortsOrch->getPort(m_port, port));
        ASSERT_TRUE(gPortsOrch->getPort(m_vlan, vlan));
        ASSERT_TRUE(gPortsOrch->removeVlanMember(vlan, port));
        ASSERT_TRUE(gStpOrch->removeStpPorts(port));
        if (member)
        {
            // Real metadata enforces the member -> bridge reference: this is
            // not an injected OBJECT_IN_USE result from the retirement stub.
            EXPECT_EQ(savedBridge->remove_bridge_port(bridge), SAI_STATUS_OBJECT_IN_USE);
        }
        EXPECT_FALSE(gPortsOrch->removeBridgePort(port));
        EXPECT_EQ(group->m_members.size(), member ? 1u : 0u);
        EXPECT_EQ(group->m_bind_ports.size(), bind ? 1u : 0u);

        removalStatus = SAI_STATUS_SUCCESS;
        now += 5000;
        ASSERT_TRUE(gPortsOrch->removeBridgePort(port));
        EXPECT_TRUE(group->m_members.empty());
        EXPECT_TRUE(group->m_bind_ports.empty());
        EXPECT_EQ(group->m_pending_members.size(), member ? 1u : 0u);
        EXPECT_EQ(group->m_pending_bind_ports.size(), bind ? 1u : 0u);
        EXPECT_EQ(group->m_oid, groupOid);

        ASSERT_NO_FATAL_FAILURE(addUntaggedMember());
        ASSERT_TRUE(gPortsOrch->getPort(m_port, port));
        EXPECT_EQ(group->m_members.size(), member ? 1u : 0u);
        EXPECT_EQ(group->m_bind_ports.size(), bind ? 1u : 0u);
        EXPECT_TRUE(group->m_pending_members.empty());
        EXPECT_TRUE(group->m_pending_bind_ports.empty());
        if (member)
        {
            sai_attribute_t attr{};
            attr.id = SAI_ISOLATION_GROUP_MEMBER_ATTR_ISOLATION_OBJECT;
            ASSERT_EQ(sai_isolation_group_api->get_isolation_group_member_attribute(
                group->m_members.at(m_port), 1, &attr), SAI_STATUS_SUCCESS);
            EXPECT_EQ(attr.value.oid, port.m_bridge_port_id);
        }

        // Re-add can cancel retirement before a later isolation observer sees
        // PRE_REMOVE. Its late releases must still be restored afterwards.
        struct ReaddListener : Observer
        {
            void update(SubjectType type, void *context) override
            {
                if (type != SUBJECT_TYPE_BRIDGE_PORT_PRE_REMOVE) return;
                auto &event = *static_cast<BridgePortRemovalUpdate *>(context);
                EXPECT_TRUE(gPortsOrch->addBridgePort(event.port));
            }
        } listener;
        gPortsOrch->detach(&isolation);
        gPortsOrch->attach(&listener);
        gPortsOrch->attach(&isolation);
        EXPECT_FALSE(gPortsOrch->removeBridgePort(port));
        gPortsOrch->detach(&listener);
        EXPECT_EQ(gPortsOrch->m_bridgePortRemovalPending.count(m_port), 0u);
        EXPECT_EQ(group->m_members.size(), member ? 1u : 0u);
        EXPECT_EQ(group->m_bind_ports.size(), bind ? 1u : 0u);
        EXPECT_TRUE(group->m_pending_members.empty());
        EXPECT_TRUE(group->m_pending_bind_ports.empty());
    }

    INSTANTIATE_TEST_SUITE_P(IsolationOwners, IntfsOrchIsolationRetirementTest,
        ::testing::Values(1u, 2u, 3u));

    struct IntfsOrchIsolationStaleRetirementTest : IntfsOrchBridgeRetirementTest,
        ::testing::WithParamInterface<string>
    {
    };

    TEST_P(IntfsOrchIsolationStaleRetirementTest, ObsoleteRetirementDoesNotRestoreOntoChangedCache)
    {
        ASSERT_NO_FATAL_FAILURE(addUntaggedMember());
        ASSERT_NO_FATAL_FAILURE(watchBridge());
        Port original;
        ASSERT_TRUE(gPortsOrch->getPort(m_port, original));
        unsigned restorations = 0;
        struct Listener : Observer
        {
            std::function<void(SubjectType, void *)> action;
            void update(SubjectType type, void *context) override { action(type, context); }
        } listener;
        listener.action = [&](SubjectType type, void *) {
            if (type == SUBJECT_TYPE_BRIDGE_PORT_REMOVE_FAILED) ++restorations;
            if (type != SUBJECT_TYPE_BRIDGE_PORT_PRE_REMOVE) return;
            if (GetParam() == "missing")
                gPortsOrch->m_portList.erase(m_port);
            else if (GetParam() == "oid")
                ++gPortsOrch->m_portList.at(m_port).m_bridge_port_id;
            else
                ++gPortsOrch->m_portList.at(m_port).m_bridge_port_generation;
        };
        gPortsOrch->attach(&listener);
        Port retiring = original;
        EXPECT_FALSE(gPortsOrch->removeBridgePort(retiring));
        gPortsOrch->detach(&listener);
        EXPECT_EQ(restorations, 0u);
        EXPECT_EQ(removals, 0u);
        EXPECT_EQ(adminDowns, 0u);
        // The replacement is a cache snapshot only; no SAI operation may use it.
        gPortsOrch->m_portList[m_port] = original;
        gPortsOrch->m_bridgePortRemovalPending.erase(m_port);
    }

    INSTANTIATE_TEST_SUITE_P(ChangedBridgeCache, IntfsOrchIsolationStaleRetirementTest,
        ::testing::Values("missing", "oid", "generation"));

    TEST_F(IntfsOrchBridgeRetirementTest, UncachedLegacyBridgeRestorationUsesTheOriginalIdentity)
    {
        ASSERT_NO_FATAL_FAILURE(addUntaggedMember());
        ASSERT_NO_FATAL_FAILURE(watchBridge());
        Port original;
        ASSERT_TRUE(gPortsOrch->getPort(m_port, original));
        Port retiring = original;
        // Legacy callers can supply an unregistered, generation-zero port.
        retiring.m_bridge_port_generation = 0;
        gPortsOrch->m_portList.erase(m_port);
        unsigned restorations = 0;
        struct Listener : Observer
        {
            std::function<void(SubjectType, void *)> action;
            void update(SubjectType type, void *context) override { action(type, context); }
        } listener;
        listener.action = [&](SubjectType type, void *context) {
            if (type != SUBJECT_TYPE_BRIDGE_PORT_REMOVE_FAILED) return;
            ++restorations;
            const auto &event = *static_cast<PortUpdate *>(context);
            EXPECT_TRUE(event.add);
            EXPECT_EQ(event.port.m_bridge_port_id, bridge);
            EXPECT_EQ(event.port.m_bridge_port_generation, 0u);
            EXPECT_EQ(event.port.m_alias, m_port);
        };
        gPortsOrch->attach(&listener);
        EXPECT_FALSE(gPortsOrch->removeBridgePort(retiring));
        gPortsOrch->detach(&listener);
        EXPECT_EQ(restorations, 1u);
        EXPECT_EQ(removals, 1u);
        EXPECT_EQ(gPortsOrch->m_portList.count(m_port), 0u);
        gPortsOrch->m_portList[m_port] = original;
        gPortsOrch->m_bridgePortRemovalPending.erase(m_port);
    }

    TEST_F(IntfsOrchBridgeRetirementTest, ObjectInUseBlocksRifUntilBridgeRetires)
    {
        ASSERT_NO_FATAL_FAILURE(addUntaggedMember());
        ASSERT_NO_FATAL_FAILURE(beginRetirement());
        const auto creates = create_rif_count;
        queueIpAdd();
        static_cast<Orch *>(gIntfsOrch)->doTask();
        ASSERT_EQ(create_rif_count, creates) << "RIF creation must wait for final bridge-port removal";
        ASSERT_FALSE(consumer(gIntfsOrch, APP_INTF_TABLE_NAME)->m_toSync.empty());

        removalStatus = SAI_STATUS_SUCCESS;
        ASSERT_TRUE(waitForBridgeRemoval());
        static_cast<Orch *>(gIntfsOrch)->doTask();
        EXPECT_EQ(create_rif_count, creates + 1);
        ASSERT_NO_FATAL_FAILURE(expectRoutedPort());
    }

    TEST_F(IntfsOrchBridgeRetirementTest, VlanDeletionWhileRemovalPendingStillBlocksRif)
    {
        ASSERT_NO_FATAL_FAILURE(addUntaggedMember());
        ASSERT_NO_FATAL_FAILURE(beginRetirement());
        consumer(gPortsOrch, APP_VLAN_TABLE_NAME)->addToSync(
            std::deque<KeyOpFieldsValuesTuple>{{m_vlan, DEL_COMMAND, {}}});
        static_cast<Orch *>(gPortsOrch)->doTask();
        Port port;
        ASSERT_FALSE(gPortsOrch->getPort(m_vlan, port));
        ASSERT_TRUE(gPortsOrch->getPort(m_port, port));
        ASSERT_EQ(port.m_bridge_port_id, bridge);
        ASSERT_FALSE(consumer(gPortsOrch, APP_VLAN_MEMBER_TABLE_NAME)->m_toSync.empty());

        const auto creates = create_rif_count;
        queueIpAdd();
        static_cast<Orch *>(gIntfsOrch)->doTask();
        EXPECT_EQ(create_rif_count, creates) << "Deleting the VLAN does not retire its remaining bridge port";
        EXPECT_FALSE(consumer(gIntfsOrch, APP_INTF_TABLE_NAME)->m_toSync.empty());
    }

    TEST_F(IntfsOrchBridgeRetirementTest, HardRemoveFailureDoesNotCompleteVlanMemberDelete)
    {
        ASSERT_NO_FATAL_FAILURE(addUntaggedMember());
        ASSERT_NO_FATAL_FAILURE(beginRetirement(SAI_STATUS_FAILURE));
        EXPECT_FALSE(consumer(gPortsOrch, APP_VLAN_MEMBER_TABLE_NAME)->m_toSync.empty())
            << "A failed remove must not acknowledge the DEL and orphan the retained bridge port";
        removalStatus = SAI_STATUS_SUCCESS;
        EXPECT_TRUE(waitForBridgeRemoval());
    }

    TEST_F(IntfsOrchBridgeRetirementTest, RemovalBackoffPreventsRetryOnImmediateTaskPass)
    {
        ASSERT_NO_FATAL_FAILURE(addUntaggedMember());
        ASSERT_NO_FATAL_FAILURE(beginRetirement());
        const auto attempts = removals;
        static_cast<Orch *>(gPortsOrch)->doTask();
        EXPECT_EQ(removals, attempts) << "A retry deadline must prevent immediate polling of OBJECT_IN_USE";
    }

    TEST_F(IntfsOrchBridgeRetirementTest, RetryDoesNotRepeatCompletedDestructiveCleanup)
    {
        ASSERT_NO_FATAL_FAILURE(addUntaggedMember());
        ASSERT_NO_FATAL_FAILURE(beginRetirement());
        ASSERT_GT(adminDowns, 0u);
        ASSERT_GT(tagStrips, 0u);
        ASSERT_GT(bridgeFlushes, 0u);
        const auto downs = adminDowns, strips = tagStrips, flushes = bridgeFlushes, attempts = removals;
        // Wait for a real second attempt without specifying an exact backoff
        // interval. Completed teardown must not be replayed on that attempt.
        for (unsigned i = 0; i < 20; ++i)
        {
            now += 5000;
            static_cast<Orch *>(gPortsOrch)->doTask();
            if (removals > attempts) break;
        }
        ASSERT_GT(removals, attempts);
        EXPECT_EQ(adminDowns, downs);
        EXPECT_EQ(tagStrips, strips);
        EXPECT_EQ(bridgeFlushes, flushes);
    }

    TEST_F(IntfsOrchBridgeRetirementTest, LagAdmissionWaitsForStandaloneBridgeRetirement)
    {
        ASSERT_NO_FATAL_FAILURE(addUntaggedMember());
        const string lag = "PortChannel1";
        consumer(gPortsOrch, APP_LAG_TABLE_NAME)->addToSync(
            std::deque<KeyOpFieldsValuesTuple>{{lag, SET_COMMAND, {{"mtu", "9100"}}}});
        static_cast<Orch *>(gPortsOrch)->doTask();
        Port port;
        ASSERT_TRUE(gPortsOrch->getPort(lag, port));
        ASSERT_NE(port.m_lag_id, SAI_NULL_OBJECT_ID);
        ASSERT_NO_FATAL_FAILURE(beginRetirement());

        consumer(gPortsOrch, APP_LAG_MEMBER_TABLE_NAME)->addToSync(
            std::deque<KeyOpFieldsValuesTuple>{{lag + ":" + m_port, SET_COMMAND, {{"status", "enabled"}}}});
        static_cast<Orch *>(gPortsOrch)->doTask();
        ASSERT_EQ(lagMemberCreates, 0u) << "No LAG member create is allowed while the standalone bridge remains";
        ASSERT_FALSE(consumer(gPortsOrch, APP_LAG_MEMBER_TABLE_NAME)->m_toSync.empty());

        removalStatus = SAI_STATUS_SUCCESS;
        ASSERT_TRUE(waitForBridgeRemoval());
        static_cast<Orch *>(gPortsOrch)->doTask();
        ASSERT_TRUE(gPortsOrch->getPort(m_port, port));
        EXPECT_NE(port.m_lag_member_id, SAI_NULL_OBJECT_ID);
        EXPECT_TRUE(consumer(gPortsOrch, APP_LAG_MEMBER_TABLE_NAME)->m_toSync.empty());
    }

    TEST_F(IntfsOrchBridgeRetirementTest, ObjectInUseRetryEventuallyRemovesTheRealBridge)
    {
        ASSERT_NO_FATAL_FAILURE(addUntaggedMember());
        ASSERT_NO_FATAL_FAILURE(beginRetirement());
        ASSERT_FALSE(consumer(gPortsOrch, APP_VLAN_MEMBER_TABLE_NAME)->m_toSync.empty());
        removalStatus = SAI_STATUS_SUCCESS;
        ASSERT_TRUE(waitForBridgeRemoval());
        EXPECT_TRUE(consumer(gPortsOrch, APP_VLAN_MEMBER_TABLE_NAME)->m_toSync.empty());
        EXPECT_GE(removals, 2u);
    }

    TEST_F(IntfsOrchBridgeRetirementTest, FailedTagStripDoesNotAdvanceToBridgeRemoval)
    {
        ASSERT_NO_FATAL_FAILURE(addUntaggedMember());
        ASSERT_NO_FATAL_FAILURE(watchBridge());
        stripStatus = SAI_STATUS_FAILURE;
        queueMemberDel();
        static_cast<Orch *>(gPortsOrch)->doTask();
        EXPECT_EQ(removals, 0u);
        EXPECT_FALSE(consumer(gPortsOrch, APP_VLAN_MEMBER_TABLE_NAME)->m_toSync.empty());
        stripStatus = removalStatus = SAI_STATUS_SUCCESS;
        EXPECT_TRUE(waitForBridgeRemoval());
    }

    TEST_F(IntfsOrchBridgeRetirementTest, FailedBridgeFlushIsRetriedBeforeBridgeRemoval)
    {
        ASSERT_NO_FATAL_FAILURE(addUntaggedMember());
        ASSERT_NO_FATAL_FAILURE(watchBridge());
        flushStatus = SAI_STATUS_FAILURE;
        queueMemberDel();
        static_cast<Orch *>(gPortsOrch)->doTask();
        EXPECT_EQ(removals, 0u);
        EXPECT_EQ(bridgeFlushes, 1u);
        flushStatus = removalStatus = SAI_STATUS_SUCCESS;
        EXPECT_TRUE(waitForBridgeRemoval());
        EXPECT_EQ(bridgeFlushes, 2u);
    }

    TEST_F(IntfsOrchBridgeRetirementTest, FailedStpReleaseRetainsItsOwnerAndBlocksBridgeRemoval)
    {
        ASSERT_NO_FATAL_FAILURE(addUntaggedMember());
        ASSERT_NO_FATAL_FAILURE(watchBridge());
        sai_attribute_t instance{};
        instance.id = SAI_SWITCH_ATTR_DEFAULT_STP_INST_ID;
        ASSERT_EQ(sai_switch_api->get_switch_attribute(gSwitchId, 1, &instance), SAI_STATUS_SUCCESS);
        ASSERT_NO_FATAL_FAILURE(addStpOwner(instance.value.oid, 1, stpPort));
        Port port;
        stpStatus = SAI_STATUS_OBJECT_IN_USE;
        queueMemberDel();
        static_cast<Orch *>(gPortsOrch)->doTask();
        EXPECT_EQ(removals, 0u);
        ASSERT_TRUE(gPortsOrch->getPort(m_port, port));
        EXPECT_EQ(port.m_stp_port_ids.count(1), 1u);
        stpStatus = removalStatus = SAI_STATUS_SUCCESS;
        EXPECT_TRUE(waitForBridgeRemoval());
    }

    struct IntfsOrchBridgeRetirementUnsupportedTest : IntfsOrchBridgeRetirementTest,
        ::testing::WithParamInterface<sai_status_t> {};

    TEST_P(IntfsOrchBridgeRetirementUnsupportedTest, UnsupportedRemovalRetainsIntentWithoutPollingAndNewAddCanCancel)
    {
        ASSERT_NO_FATAL_FAILURE(addUntaggedMember());
        ASSERT_NO_FATAL_FAILURE(beginRetirement(GetParam()));
        for (unsigned i = 0; i < 4; ++i)
        {
            now += 10000;
            static_cast<Orch *>(gPortsOrch)->doTask();
        }
        EXPECT_EQ(removals, 1u);
        ASSERT_FALSE(consumer(gPortsOrch, APP_VLAN_MEMBER_TABLE_NAME)->m_toSync.empty());
        consumer(gPortsOrch, APP_VLAN_MEMBER_TABLE_NAME)->addToSync(
            std::deque<KeyOpFieldsValuesTuple>{{m_vlan + ":" + m_port, SET_COMMAND,
                                               {{"tagging_mode", "untagged"}}}});
        static_cast<Orch *>(gPortsOrch)->doTask();
        Port port;
        ASSERT_TRUE(gPortsOrch->getPort(m_port, port));
        EXPECT_EQ(port.m_bridge_port_id, bridge);
        EXPECT_EQ(gPortsOrch->m_bridgePortRemovalPending.count(m_port), 0u);
        EXPECT_TRUE(consumer(gPortsOrch, APP_VLAN_MEMBER_TABLE_NAME)->m_toSync.empty());
        removalStatus = SAI_STATUS_SUCCESS;
        queueMemberDel();
        EXPECT_TRUE(waitForBridgeRemoval());
    }

    INSTANTIATE_TEST_SUITE_P(TerminalBridgeRemoval, IntfsOrchBridgeRetirementUnsupportedTest, ::testing::Values(
        SAI_STATUS_NOT_SUPPORTED, SAI_STATUS_NOT_IMPLEMENTED,
        SAI_STATUS_ATTR_NOT_SUPPORTED_0, SAI_STATUS_ATTR_NOT_SUPPORTED_MAX,
        SAI_STATUS_ATTR_NOT_IMPLEMENTED_0, SAI_STATUS_ATTR_NOT_IMPLEMENTED_MAX,
        SAI_STATUS_UNKNOWN_ATTRIBUTE_0, SAI_STATUS_UNKNOWN_ATTRIBUTE_MAX));

    TEST_F(IntfsOrchBridgeRetirementTest, AlreadyRemovedBridgeCompletesThePendingDelete)
    {
        ASSERT_NO_FATAL_FAILURE(addUntaggedMember());
        ASSERT_NO_FATAL_FAILURE(beginRetirement());
        ASSERT_EQ(savedBridge->remove_bridge_port(bridge), SAI_STATUS_SUCCESS);
        removalStatus = SAI_STATUS_ITEM_NOT_FOUND;
        ASSERT_TRUE(waitForBridgeRemoval());
        EXPECT_TRUE(consumer(gPortsOrch, APP_VLAN_MEMBER_TABLE_NAME)->m_toSync.empty());
        EXPECT_EQ(gPortsOrch->m_bridgePortRemovalPending.count(m_port), 0u);
    }

    TEST_F(IntfsOrchBridgeRetirementTest, ReentrantPreRemoveCannotStartAnotherTeardown)
    {
        struct Listener : Observer
        {
            std::function<void(SubjectType, void *)> action;
            void update(SubjectType type, void *context) override { action(type, context); }
        } listener;
        ASSERT_NO_FATAL_FAILURE(addUntaggedMember());
        ASSERT_NO_FATAL_FAILURE(watchBridge());
        unsigned notifications = 0;
        listener.action = [&](SubjectType type, void *context) {
            if (type != SUBJECT_TYPE_BRIDGE_PORT_PRE_REMOVE) return;
            auto &event = *static_cast<BridgePortRemovalUpdate *>(context);
            if (++notifications == 1)
            {
                EXPECT_FALSE(gPortsOrch->removeBridgePort(event.port));
            }
            event.ready = false;
        };
        gPortsOrch->attach(&listener);
        queueMemberDel();
        static_cast<Orch *>(gPortsOrch)->doTask();
        gPortsOrch->detach(&listener);
        EXPECT_EQ(notifications, 1u);
        EXPECT_EQ(removals, 0u);
        EXPECT_EQ(adminDowns, 0u);
        EXPECT_EQ(bridgeFlushes, 0u);
    }

    TEST_F(IntfsOrchBridgeRetirementTest, AdminDownFailureDoesNotAdvanceCleanupAndCanRecover)
    {
        ASSERT_NO_FATAL_FAILURE(addUntaggedMember());
        ASSERT_NO_FATAL_FAILURE(watchBridge());
        adminStatus = SAI_STATUS_FAILURE;
        queueMemberDel();
        static_cast<Orch *>(gPortsOrch)->doTask();
        EXPECT_EQ(removals, 0u);
        EXPECT_EQ(tagStrips, 0u);
        EXPECT_EQ(bridgeFlushes, 0u);
        EXPECT_FALSE(consumer(gPortsOrch, APP_VLAN_MEMBER_TABLE_NAME)->m_toSync.empty());
        adminStatus = removalStatus = SAI_STATUS_SUCCESS;
        EXPECT_TRUE(waitForBridgeRemoval());
    }

    TEST_F(IntfsOrchBridgeRetirementTest, RetryBackoffGrowsAndStopsAtTheConfiguredBound)
    {
        ASSERT_NO_FATAL_FAILURE(addUntaggedMember());
        ASSERT_NO_FATAL_FAILURE(beginRetirement());
        const std::vector<uint64_t> delays{100, 200, 400, 800, 1600, 3200, 5000, 5000};
        unsigned attempts = removals;
        for (const auto delay : delays)
        {
            now += delay - 1;
            static_cast<Orch *>(gPortsOrch)->doTask();
            EXPECT_EQ(removals, attempts);
            ++now;
            static_cast<Orch *>(gPortsOrch)->doTask();
            EXPECT_EQ(removals, ++attempts);
        }
        EXPECT_EQ(adminDowns, 1u);
        EXPECT_EQ(tagStrips, 1u);
        EXPECT_EQ(bridgeFlushes, 1u);
    }

    TEST_F(IntfsOrchBridgeRetirementTest, NewAddDuringPreRemoveCancelsDestructiveWork)
    {
        struct Listener : Observer
        {
            std::function<void(SubjectType, void *)> action;
            void update(SubjectType type, void *context) override { action(type, context); }
        } listener;
        ASSERT_NO_FATAL_FAILURE(addUntaggedMember());
        ASSERT_NO_FATAL_FAILURE(watchBridge());
        listener.action = [&](SubjectType type, void *context) {
            if (type != SUBJECT_TYPE_BRIDGE_PORT_PRE_REMOVE) return;
            auto &event = *static_cast<BridgePortRemovalUpdate *>(context);
            EXPECT_TRUE(gPortsOrch->addBridgePort(event.port));
        };
        gPortsOrch->attach(&listener);
        queueMemberDel();
        static_cast<Orch *>(gPortsOrch)->doTask();
        gPortsOrch->detach(&listener);
        EXPECT_EQ(removals, 0u);
        EXPECT_EQ(adminDowns, 0u);
        EXPECT_EQ(bridgeFlushes, 0u);
        EXPECT_EQ(gPortsOrch->m_bridgePortRemovalPending.count(m_port), 0u);
    }

    TEST_F(IntfsOrchBridgeRetirementTest, HostifTagResultReportsUnsupportedPortTypes)
    {
        Port unsupported("VlanUnsupported", Port::VLAN);
        sai_status_t status = SAI_STATUS_SUCCESS;
        EXPECT_FALSE(gPortsOrch->setHostIntfsStripTag(unsupported, SAI_HOSTIF_VLAN_TAG_STRIP, &status));
        EXPECT_EQ(status, SAI_STATUS_NOT_SUPPORTED);
        EXPECT_FALSE(gPortsOrch->setHostIntfsStripTag(unsupported, SAI_HOSTIF_VLAN_TAG_STRIP));
    }

    TEST_F(IntfsOrchBridgeRetirementTest, RetryDeadlineAndAttemptCountCannotWrap)
    {
        ASSERT_NO_FATAL_FAILURE(addUntaggedMember());
        ASSERT_NO_FATAL_FAILURE(beginRetirement());
        auto retirement = gPortsOrch->m_bridgePortRemovalPending.at(m_port);
        retirement->attempts = std::numeric_limits<unsigned>::max();
        now = std::numeric_limits<uint64_t>::max() - 50;
        retirement->retryAt = now;
        static_cast<Orch *>(gPortsOrch)->doTask();
        EXPECT_EQ(retirement->attempts, std::numeric_limits<unsigned>::max());
        EXPECT_EQ(retirement->retryAt, std::numeric_limits<uint64_t>::max());
        const auto calls = removals;
        now = std::numeric_limits<uint64_t>::max() - 1;
        static_cast<Orch *>(gPortsOrch)->doTask();
        EXPECT_EQ(removals, calls);
    }

    TEST_F(IntfsOrchBridgeRetirementTest, NewAddAfterAdminDownCancelsRemainingTeardown)
    {
        ASSERT_NO_FATAL_FAILURE(addUntaggedMember());
        ASSERT_NO_FATAL_FAILURE(watchBridge());
        afterAdminDown = [&] {
            Port port;
            ASSERT_TRUE(gPortsOrch->getPort(m_port, port));
            EXPECT_TRUE(gPortsOrch->addBridgePort(port));
        };
        queueMemberDel();
        static_cast<Orch *>(gPortsOrch)->doTask();
        EXPECT_EQ(removals, 0u);
        EXPECT_EQ(tagStrips, 0u);
        EXPECT_EQ(bridgeFlushes, 0u);
        EXPECT_EQ(gPortsOrch->m_bridgePortRemovalPending.count(m_port), 0u);
        sai_attribute_t admin{};
        admin.id = SAI_BRIDGE_PORT_ATTR_ADMIN_STATE;
        ASSERT_EQ(savedBridge->get_bridge_port_attribute(bridge, 1, &admin), SAI_STATUS_SUCCESS);
        EXPECT_TRUE(admin.value.booldata);
    }

    TEST_F(IntfsOrchBridgeRetirementTest, StpCleanupRetainsFailedOwnerAndReleasesOtherOwners)
    {
        ASSERT_NO_FATAL_FAILURE(addUntaggedMember());
        ASSERT_NO_FATAL_FAILURE(watchBridge());
        sai_attribute_t defaultInstance{};
        defaultInstance.id = SAI_SWITCH_ATTR_DEFAULT_STP_INST_ID;
        ASSERT_EQ(sai_switch_api->get_switch_attribute(gSwitchId, 1, &defaultInstance), SAI_STATUS_SUCCESS);
        sai_object_id_t secondInstance = SAI_NULL_OBJECT_ID;
        ASSERT_EQ(savedStp->create_stp(&secondInstance, gSwitchId, 0, nullptr), SAI_STATUS_SUCCESS);
        sai_object_id_t secondOwner = SAI_NULL_OBJECT_ID;
        ASSERT_NO_FATAL_FAILURE(addStpOwner(defaultInstance.value.oid, 1, stpPort));
        ASSERT_NO_FATAL_FAILURE(addStpOwner(secondInstance, 2, secondOwner));
        Port port;
        ASSERT_TRUE(gPortsOrch->getPort(m_port, port));
        port.m_stp_port_ids[3] = SAI_NULL_OBJECT_ID;
        gPortsOrch->setPort(m_port, port);
        stpStatus = SAI_STATUS_OBJECT_IN_USE;
        queueMemberDel();
        static_cast<Orch *>(gPortsOrch)->doTask();
        ASSERT_TRUE(gPortsOrch->getPort(m_port, port));
        EXPECT_EQ(port.m_stp_port_ids.size(), 1u);
        EXPECT_EQ(port.m_stp_port_ids.count(1), 1u);
        EXPECT_EQ(stpRemovals[stpPort], 1u);
        EXPECT_EQ(stpRemovals[secondOwner], 1u);
        EXPECT_EQ(removals, 0u);
        stpStatus = removalStatus = SAI_STATUS_SUCCESS;
        EXPECT_TRUE(waitForBridgeRemoval());
        EXPECT_EQ(stpRemovals[stpPort], 2u);
        EXPECT_EQ(stpRemovals[secondOwner], 1u);
        EXPECT_EQ(savedStp->remove_stp(secondInstance), SAI_STATUS_SUCCESS);
    }

    struct IntfsOrchBridgeRetirementMissingStpTest : IntfsOrchBridgeRetirementTest,
        ::testing::WithParamInterface<sai_status_t> {};

    TEST_P(IntfsOrchBridgeRetirementMissingStpTest, AlreadyGoneStpOwnerDoesNotBlockRetirement)
    {
        ASSERT_NO_FATAL_FAILURE(addUntaggedMember());
        ASSERT_NO_FATAL_FAILURE(watchBridge(SAI_STATUS_SUCCESS));
        sai_attribute_t instance{};
        instance.id = SAI_SWITCH_ATTR_DEFAULT_STP_INST_ID;
        ASSERT_EQ(sai_switch_api->get_switch_attribute(gSwitchId, 1, &instance), SAI_STATUS_SUCCESS);
        ASSERT_NO_FATAL_FAILURE(addStpOwner(instance.value.oid, 1, stpPort));
        ASSERT_EQ(savedStp->remove_stp_port(stpPort), SAI_STATUS_SUCCESS);
        stpStatus = GetParam();
        queueMemberDel();
        ASSERT_TRUE(waitForBridgeRemoval());
        Port port;
        ASSERT_TRUE(gPortsOrch->getPort(m_port, port));
        EXPECT_TRUE(port.m_stp_port_ids.empty());
        EXPECT_EQ(stpRemovals[stpPort], 1u);
        EXPECT_TRUE(consumer(gPortsOrch, APP_VLAN_MEMBER_TABLE_NAME)->m_toSync.empty());
    }

    INSTANTIATE_TEST_SUITE_P(MissingStpOwner, IntfsOrchBridgeRetirementMissingStpTest,
        ::testing::Values(SAI_STATUS_ITEM_NOT_FOUND, SAI_STATUS_ADDR_NOT_FOUND));
}
