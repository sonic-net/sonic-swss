#define private public // make Directory::m_values available to clean it.
#include "directory.h"
#undef private
#include "gtest/gtest.h"
#include "ut_helper.h"
#include "mock_orchagent_main.h"
#include "mock_table.h"
#include <memory>
#include <vector>



namespace intfsorch_test
{
    using namespace std;

    int create_rif_count = 0;
    int remove_rif_count = 0;
    sai_router_interface_api_t *pold_sai_rif_api;
    sai_router_interface_api_t ut_sai_rif_api;

    sai_status_t _ut_create_router_interface(
            _Out_ sai_object_id_t *router_interface_id,
            _In_ sai_object_id_t switch_id,
            _In_ uint32_t attr_count,
            _In_ const sai_attribute_t *attr_list)
    {
        ++create_rif_count;
        return SAI_STATUS_SUCCESS;
    }

    sai_status_t _ut_remove_router_interface(
            _In_ sai_object_id_t router_interface_id)
    {
        ++remove_rif_count;
        return SAI_STATUS_SUCCESS;
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
}