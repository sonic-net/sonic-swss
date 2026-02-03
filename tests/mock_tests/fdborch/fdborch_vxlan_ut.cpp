#include "ut_helper.h"
#include "mock_orchagent_main.h"

#define SAI_MOCK_FILENAME fdborch_vxlan_ut
#include "mock_sai_api.h"
#include "mock_table.h"
#include "portsorch.h"
#include "fdborch.h"

#include "saimetadata.h"

// Include the mock FDB API functions
#include "mock_sai_fdb.h"

using ::testing::_;

extern CrmOrch   *gCrmOrch;
extern FdbOrch   *gFdbOrch;

#define ETH0 "Ethernet0"
#define VLAN40 "Vlan40"
#define VXLAN_REMOTE "Port_EVPN_1.1.1.1"
#define NHG_REMOTE "Port_Nexthop_Group_536870913"

namespace fdborch_vxlan_ut
{
    sai_route_api_t ut_sai_route_api;
    sai_route_api_t *pold_sai_route_api;

    sai_status_t _ut_stub_sai_create_route_entry(
        _In_ const sai_route_entry_t *route_entry,
        _In_ const uint32_t attr_count,
        _In_ const sai_attribute_t *attr_list)
    {
        return SAI_STATUS_SUCCESS;
    }

    sai_status_t _ut_stub_sai_remove_route_entry(
        _In_ const sai_route_entry_t *route_entry)
    {
        return SAI_STATUS_SUCCESS;
    }

    struct VxlanFdbOrchTest : public ::testing::Test
    {
        std::shared_ptr<swss::DBConnector> m_config_db;
        std::shared_ptr<swss::DBConnector> m_app_db;
        std::shared_ptr<swss::DBConnector> m_state_db;
        std::shared_ptr<swss::DBConnector> m_asic_db;
        std::shared_ptr<swss::DBConnector> m_chassis_app_db;
        std::shared_ptr<PortsOrch> m_portsOrch;
        EvpnNvoOrch *m_EvpnNvoOrch;

        virtual void SetUp() override
        {
            testing_db::reset();

            map<string, string> profile = {
                { "SAI_VS_SWITCH_TYPE", "SAI_VS_SWITCH_TYPE_BCM56850" },
                { "KV_DEVICE_MAC_ADDRESS", "20:03:04:05:06:00" }
            };

            ut_helper::initSaiApi(profile);

            /* Create Switch */
            sai_attribute_t attr;
            attr.id = SAI_SWITCH_ATTR_INIT_SWITCH;
            attr.value.booldata = true;
            auto status = sai_switch_api->create_switch(&gSwitchId, 1, &attr);
            ASSERT_EQ(status, SAI_STATUS_SUCCESS);

            ut_sai_route_api = *sai_route_api;
            pold_sai_route_api = sai_route_api;
            ut_sai_route_api.create_route_entry = _ut_stub_sai_create_route_entry;
            ut_sai_route_api.remove_route_entry = _ut_stub_sai_remove_route_entry;
            sai_route_api = &ut_sai_route_api;

            m_config_db = std::make_shared<swss::DBConnector>("CONFIG_DB", 0);
            m_app_db = std::make_shared<swss::DBConnector>("APPL_DB", 0);
            m_state_db = make_shared<swss::DBConnector>("STATE_DB", 0);
            m_asic_db = std::make_shared<swss::DBConnector>("ASIC_DB", 0);

            // Construct dependencies
            // 1) SwitchOrch (needed before PortsOrch)
            TableConnector app_switch_table(m_app_db.get(), APP_SWITCH_TABLE_NAME);
            TableConnector conf_asic_sensors(m_config_db.get(), CFG_ASIC_SENSORS_TABLE_NAME);
            TableConnector stateDbSwitchTable(m_state_db.get(), STATE_SWITCH_CAPABILITY_TABLE_NAME);
            vector<TableConnector> switch_tables = {
                conf_asic_sensors,
                app_switch_table
            };
            gSwitchOrch = new SwitchOrch(m_app_db.get(), switch_tables, stateDbSwitchTable);
            gDirectory.set(gSwitchOrch);

            // 2) Portsorch
            const int portsorch_base_pri = 40;
            vector<table_name_with_pri_t> ports_tables = {
                { APP_PORT_TABLE_NAME, portsorch_base_pri + 5 },
                { APP_VLAN_TABLE_NAME, portsorch_base_pri + 2 },
                { APP_VLAN_MEMBER_TABLE_NAME, portsorch_base_pri },
                { APP_LAG_TABLE_NAME, portsorch_base_pri + 4 },
                { APP_LAG_MEMBER_TABLE_NAME, portsorch_base_pri }
            };
            m_portsOrch = std::make_shared<PortsOrch>(m_app_db.get(), m_state_db.get(), ports_tables, m_chassis_app_db.get());

            // 3) Crmorch
            ASSERT_EQ(gCrmOrch, nullptr);
            gCrmOrch = new CrmOrch(m_config_db.get(), CFG_CRM_TABLE_NAME);
            VxlanTunnelOrch *vxlan_tunnel_orch_1 = new VxlanTunnelOrch(m_state_db.get(), m_app_db.get(), APP_VXLAN_TUNNEL_TABLE_NAME);
            gDirectory.set(vxlan_tunnel_orch_1);

            // 4) BufferOrch
            vector<string> buffer_tables = { APP_BUFFER_POOL_TABLE_NAME,
                                             APP_BUFFER_PROFILE_TABLE_NAME,
                                             APP_BUFFER_QUEUE_TABLE_NAME,
                                             APP_BUFFER_PG_TABLE_NAME,
                                             APP_BUFFER_PORT_INGRESS_PROFILE_LIST_NAME,
                                             APP_BUFFER_PORT_EGRESS_PROFILE_LIST_NAME };
            ASSERT_EQ(gBufferOrch, nullptr);
            gBufferOrch = new BufferOrch(m_app_db.get(), m_config_db.get(), m_state_db.get(), buffer_tables);

             // Construct fdborch
            vector<table_name_with_pri_t> app_fdb_tables = {
                { APP_FDB_TABLE_NAME,        FdbOrch::fdborch_pri},
                { APP_VXLAN_FDB_TABLE_NAME,  FdbOrch::fdborch_pri},
                { APP_MCLAG_FDB_TABLE_NAME,  FdbOrch::fdborch_pri}
            };

            TableConnector stateDbFdb(m_state_db.get(), STATE_FDB_TABLE_NAME);
            TableConnector stateMclagDbFdb(m_state_db.get(), STATE_MCLAG_REMOTE_FDB_TABLE_NAME);

            gFdbOrch = new FdbOrch(m_app_db.get(),
                                    app_fdb_tables,
                                    stateDbFdb,
                                    stateMclagDbFdb,
                                    m_portsOrch.get());

            ASSERT_EQ(gVrfOrch, nullptr);
            gVrfOrch = new VRFOrch(m_app_db.get(), APP_VRF_TABLE_NAME, m_state_db.get(), STATE_VRF_OBJECT_TABLE_NAME);

            ASSERT_EQ(gIntfsOrch, nullptr);

#if 0
            vector<table_name_with_pri_t> intf_tables = {
                { APP_INTF_TABLE_NAME,  IntfsOrch::intfsorch_pri},
                { APP_SAG_TABLE_NAME,   IntfsOrch::intfsorch_pri}
            };
#endif
            gIntfsOrch = new IntfsOrch(m_app_db.get(), APP_INTF_TABLE_NAME, gVrfOrch, m_chassis_app_db.get());
            ASSERT_EQ(gNeighOrch, nullptr);
            gNeighOrch = new NeighOrch(m_app_db.get(), APP_NEIGH_TABLE_NAME, gIntfsOrch, gFdbOrch, m_portsOrch.get(), m_chassis_app_db.get());

            vector<string> flex_counter_tables = {
                CFG_FLEX_COUNTER_TABLE_NAME
            };
            auto* flexCounterOrch = new FlexCounterOrch(m_config_db.get(), flex_counter_tables);
            gDirectory.set(flexCounterOrch);

            //ASSERT_EQ(gL2NhgOrch, nullptr);
            //gL2NhgOrch = new L2NhgOrch(m_app_db.get(), APP_L2_NEXTHOP_GROUP_TABLE_NAME);
            //gDirectory.set(gL2NhgOrch);

            m_EvpnNvoOrch = new EvpnNvoOrch(m_app_db.get(), APP_VXLAN_EVPN_NVO_TABLE_NAME);
            gDirectory.set(m_EvpnNvoOrch);

            gPortsOrch = m_portsOrch.get();
            const int fgnhgorch_pri = 15;
            vector<table_name_with_pri_t> fgnhg_tables = {
                { CFG_FG_NHG, fgnhgorch_pri },
                { CFG_FG_NHG_PREFIX, fgnhgorch_pri },
                { CFG_FG_NHG_MEMBER, fgnhgorch_pri }
            };
            gFgNhgOrch = new FgNhgOrch(m_config_db.get(), m_app_db.get(), m_state_db.get(), fgnhg_tables, gNeighOrch, gIntfsOrch, gVrfOrch);
            gDirectory.set(gFgNhgOrch);

            TableConnector srv6_sid_list_table(m_app_db.get(), APP_SRV6_SID_LIST_TABLE_NAME);
            TableConnector srv6_my_sid_table(m_app_db.get(), APP_SRV6_MY_SID_TABLE_NAME);
            vector<TableConnector> srv6_tables = {
                srv6_sid_list_table,
                srv6_my_sid_table
            };
            gSrv6Orch = new Srv6Orch(m_config_db.get(), m_app_db.get(), srv6_tables, gSwitchOrch, gVrfOrch, gNeighOrch);
            gDirectory.set(gSrv6Orch);

            const int routeorch_pri = 5;
            vector<table_name_with_pri_t> route_tables = {
                { APP_ROUTE_TABLE_NAME, routeorch_pri },
                { APP_LABEL_ROUTE_TABLE_NAME, routeorch_pri }
            };
            gRouteOrch = new RouteOrch(m_app_db.get(), route_tables, gSwitchOrch, gNeighOrch, gIntfsOrch, gVrfOrch, gFgNhgOrch, gSrv6Orch);
            gDirectory.set(gRouteOrch);

            INIT_SAI_API_MOCK(fdb);
            MockSaiApis();
        }

        virtual void TearDown() override {
            delete gCrmOrch;
            gCrmOrch = nullptr;

            delete gBufferOrch;
            gBufferOrch = nullptr;

            delete gVrfOrch;
            gVrfOrch = nullptr;

            delete gIntfsOrch;
            gIntfsOrch = nullptr;

            delete gSrv6Orch;
            gSrv6Orch = nullptr;

            delete gNeighOrch;
            gNeighOrch = nullptr;

            delete gFdbOrch;
            gFdbOrch = nullptr;

            delete gSwitchOrch;
            gSwitchOrch = nullptr;

            delete gFgNhgOrch;
            gFgNhgOrch = nullptr;

            delete gRouteOrch;
            gRouteOrch = nullptr;

            //delete gL2NhgOrch;
            //gL2NhgOrch = nullptr;

            delete m_EvpnNvoOrch;
            m_EvpnNvoOrch = nullptr;

            gDirectory.m_values.clear();
            sai_route_api = pold_sai_route_api;
            RestoreSaiApis();
            ut_helper::uninitSaiApi();
        }
    };

    /* Helper Methods */
    void setUpVlan(PortsOrch* m_portsOrch){
        /* Updates portsOrch internal cache for Vlan40 */
        std::string alias = VLAN40;
        sai_object_id_t oid = 0x26000000000796;

        Port vlan(alias, Port::VLAN);
        vlan.m_vlan_info.vlan_oid = oid;
        vlan.m_vlan_info.vlan_id = 40;
        vlan.m_members = set<string>();

        m_portsOrch->m_portList[alias] = vlan;
        m_portsOrch->m_port_ref_count[alias] = 0;
        m_portsOrch->saiOidToAlias[oid] = alias;
    }

    void setUpPort(PortsOrch* m_portsOrch){
        /* Updates portsOrch internal cache for Ethernet0 */
        std::string alias = ETH0;
        sai_object_id_t oid = 0x10000000004a4;

        Port port(alias, Port::PHY);
        port.m_index = 1;
        port.m_port_id = oid;
        port.m_hif_id = 0xd00000000056e;

        m_portsOrch->m_portList[alias] = port;
        m_portsOrch->saiOidToAlias[oid] =  alias;
    }

    void setUpVlanMember(PortsOrch* m_portsOrch){
        /* Updates portsOrch internal cache for adding Ethernet0 into Vlan40 */
        sai_object_id_t bridge_port_id = 0x3a000000002c33;

        /* Add Bridge Port */
        m_portsOrch->m_portList[ETH0].m_bridge_port_id = bridge_port_id;
        m_portsOrch->saiOidToAlias[bridge_port_id] = ETH0;
        m_portsOrch->m_portList[VLAN40].m_members.insert(ETH0);
    }

    void setUpVxlanPort(PortsOrch* m_portsOrch){
        /* Updates portsOrch internal cache for VXLAN */
        std::string alias = VXLAN_REMOTE;
        sai_object_id_t oid = 0x10000000004a5;

        Port port(alias, Port::PHY);
        m_portsOrch->m_portList[alias] = port;
        m_portsOrch->saiOidToAlias[oid] =  alias;
    }

    void setUpNhgPort(PortsOrch* m_portsOrch){
        /* Updates portsOrch internal cache for NHG */
        std::string alias = NHG_REMOTE;
        sai_object_id_t oid = 0x10000000004a6;

        Port port(alias, Port::UNKNOWN);
        m_portsOrch->m_portList[alias] = port;
        m_portsOrch->saiOidToAlias[oid] =  alias;
    }

    void setUpVxlanMember(PortsOrch* m_portsOrch){
        /* Updates portsOrch internal cache for adding VXLAN_REMOTE into Vlan40 */
        sai_object_id_t bridge_port_id = 0x3a000000002c34;

        /* Add Bridge Port */
        m_portsOrch->m_portList[VXLAN_REMOTE].m_bridge_port_id = bridge_port_id;
        m_portsOrch->saiOidToAlias[bridge_port_id] = VXLAN_REMOTE;
        m_portsOrch->m_portList[VLAN40].m_members.insert(VXLAN_REMOTE);
    }

    void setUpNhg(PortsOrch* m_portsOrch){
        /* Updates portsOrch internal cache for adding NHG_REMOTE into Vlan40 */
        sai_object_id_t bridge_port_id = 0x3a000000002c35;

        /* Add Bridge Port */
        m_portsOrch->m_portList[NHG_REMOTE].m_bridge_port_id = bridge_port_id;
        m_portsOrch->saiOidToAlias[bridge_port_id] = NHG_REMOTE;
        m_portsOrch->m_portList[VLAN40].m_members.insert(VXLAN_REMOTE);
    }


    void triggerUpdate(FdbOrch* m_fdborch,
                       sai_fdb_event_t type,
                       vector<uint8_t> mac_addr,
                       sai_object_id_t bridge_port_id,
                       sai_object_id_t bv_id){
        sai_fdb_entry_t entry;
        for (int i = 0; i < (int)mac_addr.size(); i++){
            *(entry.mac_address+i) = mac_addr[i];
        }
        entry.bv_id = bv_id;
        m_fdborch->update(type, &entry, bridge_port_id, SAI_FDB_ENTRY_TYPE_DYNAMIC);
    }
}

ACTION_P(SaveSAIAttrs, sai_attr_dest)
{
    memcpy(sai_attr_dest, arg2, sizeof(sai_attribute_t) * arg1);
}

namespace fdborch_vxlan_ut
{
    using ::testing::Eq;
    using ::testing::SaveArg;
    using ::testing::SaveArgPointee;

    TEST_F(VxlanFdbOrchTest, RemoteMacLearnAddDeleteForIfname)
    {
        Table portTable = Table(m_app_db.get(), APP_PORT_TABLE_NAME);
        auto consumer = dynamic_cast<Consumer *>(gFdbOrch->getExecutor("VXLAN_FDB_TABLE"));
        std::deque<KeyOpFieldsValuesTuple> entries;

        // Get SAI default ports to populate DB
        auto ports = ut_helper::getInitialSaiPorts();

        // Populate port table with SAI ports
        for (const auto &it : ports)
        {
            portTable.set(it.first, it.second);
        }

        // Set PortConfigDone, PortInitDone
        portTable.set("PortConfigDone", { { "count", to_string(ports.size()) } });
        portTable.set("PortInitDone", { { "lanes", "0" } });

        m_portsOrch.get()->addExistingData(&portTable);
        // Apply configuration : create ports
        static_cast<Orch *>(m_portsOrch.get())->doTask();

        ASSERT_NE(m_portsOrch, nullptr);
        setUpVlan(m_portsOrch.get());
        setUpPort(m_portsOrch.get());
        ASSERT_NE(m_portsOrch->m_portList.find(VLAN40), m_portsOrch->m_portList.end());
        ASSERT_NE(m_portsOrch->m_portList.find(ETH0), m_portsOrch->m_portList.end());
        setUpVlanMember(m_portsOrch.get());

        // Event 1: Add Remote MAC learn entry for ifname in VXLAN_FDB_TABLE
        Table vxlanFdbTable = Table(m_app_db.get(), "VXLAN_FDB_TABLE");
        vxlanFdbTable.set("Vlan40:7c:fe:90:12:22:ec", {
            {"vni", "40"},
            {"type", "dynamic"},
            {"ifname", "Ethernet0"}
        });

        gFdbOrch->addExistingData(&vxlanFdbTable);
        static_cast<Orch *>(gFdbOrch)->doTask();

        /* Make sure fdb_count is incremented as expected */
        ASSERT_EQ(m_portsOrch->m_portList[VLAN40].m_fdb_count, 1);
        ASSERT_EQ(m_portsOrch->m_portList[ETH0].m_fdb_count, 1);

        // Event 2: Delete Remote MAC learn entry for ifname in VXLAN_FDB_TABLE
        entries.push_back({"Vlan40:7c:fe:90:12:22:ec", "DEL", {
            {"vni", "40"},
            {"type", "dynamic"},
            {"ifname", "Ethernet0"}
        }});
        consumer->addToSync(entries);
        static_cast<Orch *>(gFdbOrch)->doTask();

        /* Make sure fdb_count is decremented as expected */
        ASSERT_EQ(m_portsOrch->m_portList[VLAN40].m_fdb_count, 0);
        ASSERT_EQ(m_portsOrch->m_portList[ETH0].m_fdb_count, 0);
    }

    TEST_F(VxlanFdbOrchTest, RemoteMacLearnAddDeleteForVtep)
    {
        Table portTable = Table(m_app_db.get(), APP_PORT_TABLE_NAME);
        auto consumer = dynamic_cast<Consumer *>(gFdbOrch->getExecutor("VXLAN_FDB_TABLE"));
        std::deque<KeyOpFieldsValuesTuple> entries;

        // Get SAI default ports to populate DB
        auto ports = ut_helper::getInitialSaiPorts();

        // Populate port table with SAI ports
        for (const auto &it : ports)
        {
            portTable.set(it.first, it.second);
        }

        // Set PortConfigDone, PortInitDone
        portTable.set("PortConfigDone", { { "count", to_string(ports.size()) } });
        portTable.set("PortInitDone", { { "lanes", "0" } });

        m_portsOrch.get()->addExistingData(&portTable);
        // Apply configuration : create ports
        static_cast<Orch *>(m_portsOrch.get())->doTask();

        ASSERT_NE(m_portsOrch, nullptr);
        setUpVlan(m_portsOrch.get());
        setUpVxlanPort(m_portsOrch.get());
        ASSERT_NE(m_portsOrch->m_portList.find(VLAN40), m_portsOrch->m_portList.end());
        setUpVxlanPort(m_portsOrch.get());
        ASSERT_NE(m_portsOrch->m_portList.find(VXLAN_REMOTE), m_portsOrch->m_portList.end());
        setUpVxlanMember(m_portsOrch.get());

        // Event 1: Add Remote MAC learn entry for Vtep in VXLAN_FDB_TABLE
        Table vxlanFdbTable = Table(m_app_db.get(), "VXLAN_FDB_TABLE");
        vxlanFdbTable.set("Vlan40:7c:fe:90:12:22:ec", {
            {"vni", "40"},
            {"type", "dynamic"},
            {"remote_vtep", "1.1.1.1"}
        });

        gFdbOrch->addExistingData(&vxlanFdbTable);
        static_cast<Orch *>(gFdbOrch)->doTask();

        /* Make sure fdb_count is incremented as expected */
        ASSERT_EQ(m_portsOrch->m_portList[VLAN40].m_fdb_count, 1);
        ASSERT_EQ(m_portsOrch->m_portList[VXLAN_REMOTE].m_fdb_count, 1);

        // Event 2: Delete Remote MAC learn entry for Vtep in VXLAN_FDB_TABLE
        entries.push_back({"Vlan40:7c:fe:90:12:22:ec", "DEL", {
            {"vni", "40"},
            {"type", "dynamic"},
            {"remote_vtep", "1.1.1.1"}
        }});
        consumer->addToSync(entries);
        static_cast<Orch *>(gFdbOrch)->doTask();

        /* Make sure fdb_count is decremented as expected */
        ASSERT_EQ(m_portsOrch->m_portList[VLAN40].m_fdb_count, 0);
        ASSERT_EQ(m_portsOrch->m_portList[VXLAN_REMOTE].m_fdb_count, 0);
    }

    TEST_F(VxlanFdbOrchTest, RemoteMacLearnAddDeleteForNhg)
    {
        // This test is currently disabled because L2NhgOrch is not fully implemented
        // Skip this test for now
        GTEST_SKIP() << "L2NhgOrch functionality is not available";
    }

    /* Test Consolidated Flush Per Vlan and Per Port */
    TEST_F(VxlanFdbOrchTest, LocalLearnAndAgeoutForESI)
    {
        ASSERT_NE(m_portsOrch, nullptr);
        setUpVlan(m_portsOrch.get());
        setUpPort(m_portsOrch.get());
        ASSERT_NE(m_portsOrch->m_portList.find(VLAN40), m_portsOrch->m_portList.end());
        ASSERT_NE(m_portsOrch->m_portList.find(ETH0), m_portsOrch->m_portList.end());
        setUpVlanMember(m_portsOrch.get());

        /* Event 1: Learn a dynamic FDB Entry */
        // 7c:fe:90:12:22:ec
        vector<uint8_t> mac_addr = {124, 254, 144, 18, 34, 236};
        triggerUpdate(gFdbOrch, SAI_FDB_EVENT_LEARNED, mac_addr, m_portsOrch->m_portList[ETH0].m_bridge_port_id,
                      m_portsOrch->m_portList[VLAN40].m_vlan_info.vlan_oid);

        string port;
        string entry_type;

        /* Make sure fdb_count is incremented as expected */
        ASSERT_EQ(m_portsOrch->m_portList[VLAN40].m_fdb_count, 1);
        ASSERT_EQ(m_portsOrch->m_portList[ETH0].m_fdb_count, 1);

        /* Make sure state db is updated as expected */
        ASSERT_EQ(gFdbOrch->m_fdbStateTable.hget("Vlan40:7c:fe:90:12:22:ec", "port", port), true);
        ASSERT_EQ(gFdbOrch->m_fdbStateTable.hget("Vlan40:7c:fe:90:12:22:ec", "type", entry_type), true);

        ASSERT_EQ(port, "Ethernet0");
        ASSERT_EQ(entry_type, "dynamic");

        /* Event 2: Generate a FDB age out for MAC of that port and vlan */
        triggerUpdate(gFdbOrch, SAI_FDB_EVENT_AGED, mac_addr, m_portsOrch->m_portList[ETH0].m_bridge_port_id,
                      m_portsOrch->m_portList[VLAN40].m_vlan_info.vlan_oid);

        /* make sure fdb_counters are decremented */
        ASSERT_EQ(m_portsOrch->m_portList[VLAN40].m_fdb_count, 0);
        ASSERT_EQ(m_portsOrch->m_portList[ETH0].m_fdb_count, 0);

        /* Make sure state db is cleared */
        ASSERT_EQ(gFdbOrch->m_fdbStateTable.hget("Vlan40:7c:fe:90:12:22:ec", "port", port), false);
        ASSERT_EQ(gFdbOrch->m_fdbStateTable.hget("Vlan40:7c:fe:90:12:22:ec", "type", entry_type), false);
    }

    TEST_F(VxlanFdbOrchTest, LocalMacLearnAndRemoteMacLearnForIfname)
    {
        Table portTable = Table(m_app_db.get(), APP_PORT_TABLE_NAME);

        // Get SAI default ports to populate DB
        auto ports = ut_helper::getInitialSaiPorts();

        // Populate port table with SAI ports
        for (const auto &it : ports)
        {
            portTable.set(it.first, it.second);
        }

        // Set PortConfigDone, PortInitDone
        portTable.set("PortConfigDone", { { "count", to_string(ports.size()) } });
        portTable.set("PortInitDone", { { "lanes", "0" } });

        m_portsOrch.get()->addExistingData(&portTable);
        // Apply configuration : create ports
        static_cast<Orch *>(m_portsOrch.get())->doTask();

        ASSERT_NE(m_portsOrch, nullptr);
        setUpVlan(m_portsOrch.get());
        setUpPort(m_portsOrch.get());
        ASSERT_NE(m_portsOrch->m_portList.find(VLAN40), m_portsOrch->m_portList.end());
        ASSERT_NE(m_portsOrch->m_portList.find(ETH0), m_portsOrch->m_portList.end());
        setUpVlanMember(m_portsOrch.get());

        /* Event 1: Learn a dynamic FDB Entry */
        // 7c:fe:90:12:22:ec
        vector<uint8_t> mac_addr = {124, 254, 144, 18, 34, 236};
        triggerUpdate(gFdbOrch, SAI_FDB_EVENT_LEARNED, mac_addr, m_portsOrch->m_portList[ETH0].m_bridge_port_id,
                      m_portsOrch->m_portList[VLAN40].m_vlan_info.vlan_oid);

        string port;
        string entry_type;

        /* Make sure fdb_count is incremented as expected */
        ASSERT_EQ(m_portsOrch->m_portList[VLAN40].m_fdb_count, 1);
        ASSERT_EQ(m_portsOrch->m_portList[ETH0].m_fdb_count, 1);

        /* Make sure state db is updated as expected */
        ASSERT_EQ(gFdbOrch->m_fdbStateTable.hget("Vlan40:7c:fe:90:12:22:ec", "port", port), true);
        ASSERT_EQ(gFdbOrch->m_fdbStateTable.hget("Vlan40:7c:fe:90:12:22:ec", "type", entry_type), true);

        ASSERT_EQ(port, "Ethernet0");
        ASSERT_EQ(entry_type, "dynamic");

        /* Event 2: Add Remote MAC entry for ifname case */
        Table vxlanFdbTable = Table(m_app_db.get(), "VXLAN_FDB_TABLE");
        vxlanFdbTable.set("Vlan40:7c:fe:90:12:22:ec", {
            {"vni", "40"},
            {"type", "dynamic"},
            {"ifname", "Ethernet0"}
        });

        gFdbOrch->addExistingData(&vxlanFdbTable);
        static_cast<Orch *>(gFdbOrch)->doTask();

        /* Make sure fdb_count is incremented as expected */
        ASSERT_EQ(m_portsOrch->m_portList[VLAN40].m_fdb_count, 1);
        ASSERT_EQ(m_portsOrch->m_portList[ETH0].m_fdb_count, 1);
    }

    TEST_F(VxlanFdbOrchTest, LocalAndRemoteMacLearnAndAgeoutForIfname)
    {
        Table portTable = Table(m_app_db.get(), APP_PORT_TABLE_NAME);

        // Get SAI default ports to populate DB
        auto ports = ut_helper::getInitialSaiPorts();

        // Populate port table with SAI ports
        for (const auto &it : ports)
        {
            portTable.set(it.first, it.second);
        }

        // Set PortConfigDone, PortInitDone
        portTable.set("PortConfigDone", { { "count", to_string(ports.size()) } });
        portTable.set("PortInitDone", { { "lanes", "0" } });

        m_portsOrch.get()->addExistingData(&portTable);
        // Apply configuration : create ports
        static_cast<Orch *>(m_portsOrch.get())->doTask();

        ASSERT_NE(m_portsOrch, nullptr);
        setUpVlan(m_portsOrch.get());
        setUpPort(m_portsOrch.get());
        ASSERT_NE(m_portsOrch->m_portList.find(VLAN40), m_portsOrch->m_portList.end());
        ASSERT_NE(m_portsOrch->m_portList.find(ETH0), m_portsOrch->m_portList.end());
        setUpVlanMember(m_portsOrch.get());

        /* Event 1: Learn a dynamic FDB Entry */
        // 7c:fe:90:12:22:ec
        vector<uint8_t> mac_addr = {124, 254, 144, 18, 34, 236};
        triggerUpdate(gFdbOrch, SAI_FDB_EVENT_LEARNED, mac_addr, m_portsOrch->m_portList[ETH0].m_bridge_port_id,
                      m_portsOrch->m_portList[VLAN40].m_vlan_info.vlan_oid);

        string port;
        string entry_type;

        /* Make sure fdb_count is incremented as expected */
        ASSERT_EQ(m_portsOrch->m_portList[VLAN40].m_fdb_count, 1);
        ASSERT_EQ(m_portsOrch->m_portList[ETH0].m_fdb_count, 1);

        /* Make sure state db is updated as expected */
        ASSERT_EQ(gFdbOrch->m_fdbStateTable.hget("Vlan40:7c:fe:90:12:22:ec", "port", port), true);
        ASSERT_EQ(gFdbOrch->m_fdbStateTable.hget("Vlan40:7c:fe:90:12:22:ec", "type", entry_type), true);

        ASSERT_EQ(port, "Ethernet0");
        ASSERT_EQ(entry_type, "dynamic");

        /* Event 2: Add Remote MAC entry for ifname case */
        Table vxlanFdbTable = Table(m_app_db.get(), "VXLAN_FDB_TABLE");
        vxlanFdbTable.set("Vlan40:7c:fe:90:12:22:ec", {
            {"vni", "40"},
            {"type", "dynamic"},
            {"ifname", "Ethernet0"}
        });

        gFdbOrch->addExistingData(&vxlanFdbTable);
        static_cast<Orch *>(gFdbOrch)->doTask();

        /* Make sure fdb_count is incremented as expected */
        ASSERT_EQ(m_portsOrch->m_portList[VLAN40].m_fdb_count, 1);
        ASSERT_EQ(m_portsOrch->m_portList[ETH0].m_fdb_count, 1);

        /* Event 3: Generate a FDB age out for MAC of that port and vlan */
        triggerUpdate(gFdbOrch, SAI_FDB_EVENT_AGED, mac_addr, m_portsOrch->m_portList[ETH0].m_bridge_port_id,
                      m_portsOrch->m_portList[VLAN40].m_vlan_info.vlan_oid);

        /* make sure fdb_counters are decremented */
        ASSERT_EQ(m_portsOrch->m_portList[VLAN40].m_fdb_count, 1);
        ASSERT_EQ(m_portsOrch->m_portList[ETH0].m_fdb_count, 1);

        /* Make sure state db is cleared */
        ASSERT_EQ(gFdbOrch->m_fdbStateTable.hget("Vlan40:7c:fe:90:12:22:ec", "port", port), false);
        ASSERT_EQ(gFdbOrch->m_fdbStateTable.hget("Vlan40:7c:fe:90:12:22:ec", "type", entry_type), false);
    }

    TEST_F(VxlanFdbOrchTest, LocalAndRemoteMacLearnAndRemoteMacWithdrawalForIfname)
    {
        Table portTable = Table(m_app_db.get(), APP_PORT_TABLE_NAME);
        auto consumer = dynamic_cast<Consumer *>(gFdbOrch->getExecutor("VXLAN_FDB_TABLE"));
        std::deque<KeyOpFieldsValuesTuple> entries;

        // Get SAI default ports to populate DB
        auto ports = ut_helper::getInitialSaiPorts();

        // Populate port table with SAI ports
        for (const auto &it : ports)
        {
            portTable.set(it.first, it.second);
        }

        // Set PortConfigDone, PortInitDone
        portTable.set("PortConfigDone", { { "count", to_string(ports.size()) } });
        portTable.set("PortInitDone", { { "lanes", "0" } });

        m_portsOrch.get()->addExistingData(&portTable);
        // Apply configuration : create ports
        static_cast<Orch *>(m_portsOrch.get())->doTask();

        ASSERT_NE(m_portsOrch, nullptr);
        setUpVlan(m_portsOrch.get());
        setUpPort(m_portsOrch.get());
        ASSERT_NE(m_portsOrch->m_portList.find(VLAN40), m_portsOrch->m_portList.end());
        ASSERT_NE(m_portsOrch->m_portList.find(ETH0), m_portsOrch->m_portList.end());
        setUpVlanMember(m_portsOrch.get());

        /* Event 1: Learn a dynamic FDB Entry */
        // 7c:fe:90:12:22:ec
        vector<uint8_t> mac_addr = {124, 254, 144, 18, 34, 236};
        triggerUpdate(gFdbOrch, SAI_FDB_EVENT_LEARNED, mac_addr, m_portsOrch->m_portList[ETH0].m_bridge_port_id,
                      m_portsOrch->m_portList[VLAN40].m_vlan_info.vlan_oid);

        string port;
        string entry_type;

        /* Make sure fdb_count is incremented as expected */
        ASSERT_EQ(m_portsOrch->m_portList[VLAN40].m_fdb_count, 1);
        ASSERT_EQ(m_portsOrch->m_portList[ETH0].m_fdb_count, 1);

        /* Make sure state db is updated as expected */
        ASSERT_EQ(gFdbOrch->m_fdbStateTable.hget("Vlan40:7c:fe:90:12:22:ec", "port", port), true);
        ASSERT_EQ(gFdbOrch->m_fdbStateTable.hget("Vlan40:7c:fe:90:12:22:ec", "type", entry_type), true);

        ASSERT_EQ(port, "Ethernet0");
        ASSERT_EQ(entry_type, "dynamic");

        /* Event 2: Add Remote MAC entry for ifname case */
        Table vxlanFdbTable = Table(m_app_db.get(), "VXLAN_FDB_TABLE");
        vxlanFdbTable.set("Vlan40:7c:fe:90:12:22:ec", {
            {"vni", "40"},
            {"type", "dynamic"},
            {"ifname", "Ethernet0"}
        });

        gFdbOrch->addExistingData(&vxlanFdbTable);
        static_cast<Orch *>(gFdbOrch)->doTask();

        /* Make sure fdb_count is incremented as expected */
        ASSERT_EQ(m_portsOrch->m_portList[VLAN40].m_fdb_count, 1);
        ASSERT_EQ(m_portsOrch->m_portList[ETH0].m_fdb_count, 1);

        /* Event 3: Delete Remote MAC entry for ifname case */
        entries.push_back({"Vlan40:7c:fe:90:12:22:ec", "DEL", {
            {"vni", "40"},
            {"type", "dynamic"},
            {"remote_vtep", "1.1.1.1"}
        }});
        consumer->addToSync(entries);
        static_cast<Orch *>(gFdbOrch)->doTask();

        /* Make sure fdb_count is decremented as expected */
        ASSERT_EQ(m_portsOrch->m_portList[VLAN40].m_fdb_count, 1);
        ASSERT_EQ(m_portsOrch->m_portList[VXLAN_REMOTE].m_fdb_count, 0);
    }

    /*
     * This matcher will receive the following tuples in the following arguments:
     *
     * sai_attr_tuple_to_match: {uint32_t attr_count, sai_attribute_t *attr_list}
     * arg: {sai_object_id_t switch_id, uint32_t attr_count, sai_attribute_t *attr_list}
     */
    MATCHER_P(MatchSaiAttrList, sai_attr_tuple_to_match, "")
    {
        bool attr_list_matches = true;

        uint32_t expected_attr_list_len = std::get<0>(sai_attr_tuple_to_match);
        uint32_t called_attr_list_len = std::get<1>(arg);
        const sai_attribute_t *expected_attr_list = std::get<1>(sai_attr_tuple_to_match);
        const sai_attribute_t *called_attr_list = std::get<2>(arg);
        const sai_attr_metadata_t* const* const sai_fdb_flush_attr = sai_metadata_object_type_info_SAI_OBJECT_TYPE_FDB_FLUSH.attrmetadata;
        uint32_t i;

        /* Check for length match first */
        if (expected_attr_list_len != called_attr_list_len)
        {
            *result_listener << "\nExpected the following attributes (" << expected_attr_list_len << "): ";
            for (i = 0; i < expected_attr_list_len; i++) {
                *result_listener << sai_fdb_flush_attr[expected_attr_list[i].id]->attridname << " ";
            }

            *result_listener << "\nReceived the following attributes (" << called_attr_list_len << "): ";
            for (i = 0; i < called_attr_list_len; i++) {
                *result_listener << sai_fdb_flush_attr[called_attr_list[i].id]->attridname << " ";
            }
            attr_list_matches = false;
        }
        else
        {
            for (i = 0; attr_list_matches && i < called_attr_list_len; i++)
            {
                if (expected_attr_list[i].id != called_attr_list[i].id) {
                    *result_listener << "[" << i << "] Expected attribute "
                        << sai_fdb_flush_attr[expected_attr_list[i].id]->attridname
                        << " got attribute "
                        << sai_fdb_flush_attr[called_attr_list[i].id]->attridname;
                    attr_list_matches = false;
                }

                if (attr_list_matches) {
                    switch (sai_fdb_flush_attr[called_attr_list[i].id]->attrvaluetype)
                    {
                        default:
                            *result_listener << "[" << i << "] Unsupported SAI Value Type "
                                << sai_metadata_get_attr_value_type_name(
                                    sai_fdb_flush_attr[called_attr_list[i].id]->attrvaluetype);
                            attr_list_matches = false;
                            break;
                        case SAI_ATTR_VALUE_TYPE_OBJECT_ID:
                            if (expected_attr_list[i].value.oid != called_attr_list[i].value.oid) {
                                *result_listener << "[" << i << "] Expected OID 0x"
                                    << std::hex
                                    << expected_attr_list[i].value.oid << ", got OID 0x"
                                    << called_attr_list[i].value.oid;
                                attr_list_matches = false;
                            }
                            break;
                        case SAI_ATTR_VALUE_TYPE_INT32:
                            if (expected_attr_list[i].value.s32 != called_attr_list[i].value.s32) {
                                *result_listener << "[" << i << "] Expected INT32 "
                                    << expected_attr_list[i].value.oid << ", got INT32 "
                                    << called_attr_list[i].value.oid;
                                attr_list_matches = false;
                            }
                            break;
                    }
                }
            }
        }

        return attr_list_matches;
    }


    TEST_F(VxlanFdbOrchTest, FlushLocalRemoteMACsOnVlanDelete)
    {
        Table portTable = Table(m_app_db.get(), APP_PORT_TABLE_NAME);
        //auto consumer = dynamic_cast<Consumer *>(gFdbOrch->getExecutor("VXLAN_FDB_TABLE"));
        std::deque<KeyOpFieldsValuesTuple> entries;

        // Get SAI default ports to populate DB
        auto ports = ut_helper::getInitialSaiPorts();

        // Populate port table with SAI ports
        for (const auto &it : ports)
        {
            portTable.set(it.first, it.second);
        }

        // Set PortConfigDone, PortInitDone
        portTable.set("PortConfigDone", { { "count", to_string(ports.size()) } });
        portTable.set("PortInitDone", { { "lanes", "0" } });

        m_portsOrch.get()->addExistingData(&portTable);
        // Apply configuration : create ports
        static_cast<Orch *>(m_portsOrch.get())->doTask();

        ASSERT_NE(m_portsOrch, nullptr);
        setUpVlan(m_portsOrch.get());
        setUpPort(m_portsOrch.get());
        ASSERT_NE(m_portsOrch->m_portList.find(VLAN40), m_portsOrch->m_portList.end());
        ASSERT_NE(m_portsOrch->m_portList.find(ETH0), m_portsOrch->m_portList.end());
        setUpVlanMember(m_portsOrch.get());
        setUpVxlanPort(m_portsOrch.get());
        ASSERT_NE(m_portsOrch->m_portList.find(VXLAN_REMOTE), m_portsOrch->m_portList.end());
        setUpVxlanMember(m_portsOrch.get());

        /* Event 1: Learn a dynamic FDB Entry */
        // 7c:fe:90:12:22:ec
        vector<uint8_t> mac_addr = {124, 254, 144, 18, 34, 236};
        triggerUpdate(gFdbOrch, SAI_FDB_EVENT_LEARNED, mac_addr, m_portsOrch->m_portList[ETH0].m_bridge_port_id,
                      m_portsOrch->m_portList[VLAN40].m_vlan_info.vlan_oid);

        string port;
        string entry_type;

        /* Make sure fdb_count is incremented as expected */
        ASSERT_EQ(m_portsOrch->m_portList[VLAN40].m_fdb_count, 1);
        ASSERT_EQ(m_portsOrch->m_portList[VLAN40].m_fdb_count, 1);

        /* Make sure state db is updated as expected */
        ASSERT_EQ(gFdbOrch->m_fdbStateTable.hget("Vlan40:7c:fe:90:12:22:ec", "port", port), true);
        ASSERT_EQ(gFdbOrch->m_fdbStateTable.hget("Vlan40:7c:fe:90:12:22:ec", "type", entry_type), true);

        ASSERT_EQ(port, "Ethernet0");
        ASSERT_EQ(entry_type, "dynamic");

        // Event 2: Add Remote MAC learn entry for Vtep in VXLAN_FDB_TABLE
        Table vxlanFdbTable = Table(m_app_db.get(), "VXLAN_FDB_TABLE");
        vxlanFdbTable.set("Vlan40:7c:fe:90:12:22:ec", {
            {"vni", "40"},
            {"type", "dynamic"},
            {"remote_vtep", "1.1.1.1"}
        });

        gFdbOrch->addExistingData(&vxlanFdbTable);
        static_cast<Orch *>(gFdbOrch)->doTask();

        /* MAC is moved from local to remote, yet it's still in same VLAN */
        ASSERT_EQ(m_portsOrch->m_portList[VLAN40].m_fdb_count, 1);
        ASSERT_EQ(m_portsOrch->m_portList[VXLAN_REMOTE].m_fdb_count, 1);

        /* Delete the VxLAN port for the VLAN, MACs should be flushed */
        VlanMemberUpdate vlanMemberUpdate = {
            .vlan = m_portsOrch->m_portList[VLAN40],
            .member = m_portsOrch->m_portList[VXLAN_REMOTE],
            .add = false
        };
        vector<sai_attribute_t> attrs;
        sai_attribute_t attr;

        attr.id = SAI_FDB_FLUSH_ATTR_BRIDGE_PORT_ID;
        attr.value.oid = m_portsOrch->m_portList[VXLAN_REMOTE].m_bridge_port_id;
        attrs.push_back(attr);

        attr.id = SAI_FDB_FLUSH_ATTR_BV_ID;
        attr.value.oid = m_portsOrch->m_portList[VLAN40].m_vlan_info.vlan_oid;
        attrs.push_back(attr);

        attr.id = SAI_FDB_FLUSH_ATTR_ENTRY_TYPE;
        attr.value.s32 = SAI_FDB_FLUSH_ENTRY_TYPE_ALL;
        attrs.push_back(attr);

        EXPECT_CALL(*mock_sai_fdb_api, flush_fdb_entries(_, _, _))
            .With(testing::AllArgs(MatchSaiAttrList(make_tuple((uint32_t)attrs.size(), attrs.data()))));
        gFdbOrch->update(SUBJECT_TYPE_VLAN_MEMBER_CHANGE, &vlanMemberUpdate);
    }

    TEST_F(VxlanFdbOrchTest, FlushAllFDBEntriesForTunnelPort)
    {
        Table portTable = Table(m_app_db.get(), APP_PORT_TABLE_NAME);
        std::deque<KeyOpFieldsValuesTuple> entries;

        // Get SAI default ports to populate DB
        auto ports = ut_helper::getInitialSaiPorts();

        // Populate port table with SAI ports
        for (const auto &it : ports)
        {
            portTable.set(it.first, it.second);
        }

        // Set PortConfigDone, PortInitDone
        portTable.set("PortConfigDone", { { "count", to_string(ports.size()) } });
        portTable.set("PortInitDone", { { "lanes", "0" } });

        m_portsOrch.get()->addExistingData(&portTable);
        static_cast<Orch *>(m_portsOrch.get())->doTask();

        ASSERT_NE(m_portsOrch, nullptr);
        setUpVlan(m_portsOrch.get());
        setUpPort(m_portsOrch.get());
        setUpVlanMember(m_portsOrch.get());
        setUpVxlanPort(m_portsOrch.get());
        setUpVxlanMember(m_portsOrch.get());

        // Set tunnel port type to verify tunnel-specific logic
        Port& tunnelPort = m_portsOrch->m_portList[VXLAN_REMOTE];
        tunnelPort.m_type = Port::TUNNEL;
        m_portsOrch->setPort(VXLAN_REMOTE, tunnelPort);

        // Add multiple remote MAC entries for tunnel port
        vector<vector<uint8_t>> mac_addrs = {
            {0x7c, 0xfe, 0x90, 0x12, 0x22, 0xec}, // 7c:fe:90:12:22:ec
            {0x7c, 0xfe, 0x90, 0x12, 0x22, 0xed}, // 7c:fe:90:12:22:ed
            {0x7c, 0xfe, 0x90, 0x12, 0x22, 0xee}  // 7c:fe:90:12:22:ee
        };

        // Add remote MAC entries via VXLAN_FDB_TABLE
        Table vxlanFdbTable = Table(m_app_db.get(), "VXLAN_FDB_TABLE");
        for (size_t i = 0; i < mac_addrs.size(); i++)
        {
            char mac_str[18];
            sprintf(mac_str, "%02x:%02x:%02x:%02x:%02x:%02x",
                   mac_addrs[i][0], mac_addrs[i][1], mac_addrs[i][2],
                   mac_addrs[i][3], mac_addrs[i][4], mac_addrs[i][5]);

            string key = string("Vlan40:") + mac_str;
            vxlanFdbTable.set(key, {
                {"vni", "40"},
                {"type", "dynamic"},
                {"remote_vtep", "1.1.1.1"}
            });
        }

        gFdbOrch->addExistingData(&vxlanFdbTable);
        static_cast<Orch *>(gFdbOrch)->doTask();

        // Verify entries were added
        ASSERT_EQ(m_portsOrch->m_portList[VXLAN_REMOTE].m_fdb_count, 3);

        // Mock SAI FDB remove calls for tunnel port flushing
        EXPECT_CALL(*mock_sai_fdb_api, remove_fdb_entry(_))
            .Times(3)
            .WillRepeatedly(testing::Return(SAI_STATUS_SUCCESS));

        // Test flushAllFDBEntries for tunnel port
        gFdbOrch->flushAllFDBEntries(tunnelPort.m_bridge_port_id, SAI_NULL_OBJECT_ID);

        // Verify all entries are removed and counters are updated
        ASSERT_EQ(m_portsOrch->m_portList[VXLAN_REMOTE].m_fdb_count, 0);
        ASSERT_EQ(m_portsOrch->m_portList[VLAN40].m_fdb_count, 0);
    }

    TEST_F(VxlanFdbOrchTest, FlushAllFDBEntriesForNonTunnelPort)
    {
        Table portTable = Table(m_app_db.get(), APP_PORT_TABLE_NAME);

        // Get SAI default ports to populate DB
        auto ports = ut_helper::getInitialSaiPorts();

        // Populate port table with SAI ports
        for (const auto &it : ports)
        {
            portTable.set(it.first, it.second);
        }

        // Set PortConfigDone, PortInitDone
        portTable.set("PortConfigDone", { { "count", to_string(ports.size()) } });
        portTable.set("PortInitDone", { { "lanes", "0" } });

        m_portsOrch.get()->addExistingData(&portTable);
        static_cast<Orch *>(m_portsOrch.get())->doTask();

        ASSERT_NE(m_portsOrch, nullptr);
        setUpVlan(m_portsOrch.get());
        setUpPort(m_portsOrch.get());
        setUpVlanMember(m_portsOrch.get());

        // Learn a local FDB entry on regular port
        vector<uint8_t> mac_addr = {124, 254, 144, 18, 34, 236};
        triggerUpdate(gFdbOrch, SAI_FDB_EVENT_LEARNED, mac_addr,
                      m_portsOrch->m_portList[ETH0].m_bridge_port_id,
                      m_portsOrch->m_portList[VLAN40].m_vlan_info.vlan_oid);

        // Verify entry was added
        ASSERT_EQ(m_portsOrch->m_portList[ETH0].m_fdb_count, 1);
        ASSERT_EQ(m_portsOrch->m_portList[VLAN40].m_fdb_count, 1);

        // Set up expected SAI flush call for non-tunnel port (flushes all static and dynamic)
        vector<sai_attribute_t> expected_attrs;
        sai_attribute_t attr;

        attr.id = SAI_FDB_FLUSH_ATTR_BRIDGE_PORT_ID;
        attr.value.oid = m_portsOrch->m_portList[ETH0].m_bridge_port_id;
        expected_attrs.push_back(attr);

        attr.id = SAI_FDB_FLUSH_ATTR_ENTRY_TYPE;
        attr.value.s32 = SAI_FDB_FLUSH_ENTRY_TYPE_ALL;
        expected_attrs.push_back(attr);

        EXPECT_CALL(*mock_sai_fdb_api, flush_fdb_entries(_, _, _))
            .With(testing::AllArgs(MatchSaiAttrList(make_tuple((uint32_t)expected_attrs.size(), expected_attrs.data()))))
            .WillOnce(testing::Return(SAI_STATUS_SUCCESS));

        // Test flushAllFDBEntries for non-tunnel port
        gFdbOrch->flushAllFDBEntries(m_portsOrch->m_portList[ETH0].m_bridge_port_id, SAI_NULL_OBJECT_ID);

    }

    TEST_F(VxlanFdbOrchTest, FlushAllFDBEntriesWithInvalidParameters)
    {
        Table portTable = Table(m_app_db.get(), APP_PORT_TABLE_NAME);

        // Get SAI default ports to populate DB
        auto ports = ut_helper::getInitialSaiPorts();

        // Populate port table with SAI ports
        for (const auto &it : ports)
        {
            portTable.set(it.first, it.second);
        }

        // Set PortConfigDone, PortInitDone
        portTable.set("PortConfigDone", { { "count", to_string(ports.size()) } });
        portTable.set("PortInitDone", { { "lanes", "0" } });

        m_portsOrch.get()->addExistingData(&portTable);
        static_cast<Orch *>(m_portsOrch.get())->doTask();

        // Test with both parameters as null - should return early with warning
        EXPECT_CALL(*mock_sai_fdb_api, flush_fdb_entries(_, _, _)).Times(0);

        gFdbOrch->flushAllFDBEntries(SAI_NULL_OBJECT_ID, SAI_NULL_OBJECT_ID);

        // No assertions needed as function should return early without making SAI calls
    }

    TEST_F(VxlanFdbOrchTest, RemoveFdbEntryFromPortCache)
    {
        Table portTable = Table(m_app_db.get(), APP_PORT_TABLE_NAME);

        // Get SAI default ports to populate DB
        auto ports = ut_helper::getInitialSaiPorts();

        // Populate port table with SAI ports
        for (const auto &it : ports)
        {
            portTable.set(it.first, it.second);
        }

        // Set PortConfigDone, PortInitDone
        portTable.set("PortConfigDone", { { "count", to_string(ports.size()) } });
        portTable.set("PortInitDone", { { "lanes", "0" } });

        m_portsOrch.get()->addExistingData(&portTable);
        static_cast<Orch *>(m_portsOrch.get())->doTask();

        ASSERT_NE(m_portsOrch, nullptr);
        setUpVlan(m_portsOrch.get());
        setUpPort(m_portsOrch.get());
        setUpVlanMember(m_portsOrch.get());

        // Create FDB entry
        FdbEntry entry;
        entry.mac = MacAddress("7c:fe:90:12:22:ec");
        entry.bv_id = m_portsOrch->m_portList[VLAN40].m_vlan_info.vlan_oid;
        entry.port_name = ETH0;

        Port port = m_portsOrch->m_portList[ETH0];

        // Manually add entry to port cache to simulate normal operation
        gFdbOrch->m_entries_by_port[port.m_alias].push_back(entry);

        // Verify entry exists in cache
        ASSERT_EQ(gFdbOrch->m_entries_by_port[port.m_alias].size(), 1);
        ASSERT_EQ(gFdbOrch->m_entries_by_port[port.m_alias][0].mac.to_string(), "7c:fe:90:12:22:ec");

        // Test removeFdbEntryFromPortCache
        gFdbOrch->removeFdbEntryFromPortCache(entry, port);

        // Verify entry is removed from cache
        ASSERT_EQ(gFdbOrch->m_entries_by_port[port.m_alias].size(), 0);
    }

    TEST_F(VxlanFdbOrchTest, RemoveFdbEntryFromPortCacheMultipleEntries)
    {
        Table portTable = Table(m_app_db.get(), APP_PORT_TABLE_NAME);

        // Get SAI default ports to populate DB
        auto ports = ut_helper::getInitialSaiPorts();

        // Populate port table with SAI ports
        for (const auto &it : ports)
        {
            portTable.set(it.first, it.second);
        }

        // Set PortConfigDone, PortInitDone
        portTable.set("PortConfigDone", { { "count", to_string(ports.size()) } });
        portTable.set("PortInitDone", { { "lanes", "0" } });

        m_portsOrch.get()->addExistingData(&portTable);
        static_cast<Orch *>(m_portsOrch.get())->doTask();

        ASSERT_NE(m_portsOrch, nullptr);
        setUpVlan(m_portsOrch.get());
        setUpPort(m_portsOrch.get());
        setUpVlanMember(m_portsOrch.get());

        // Create multiple FDB entries
        FdbEntry entry1, entry2, entry3;
        entry1.mac = MacAddress("7c:fe:90:12:22:ec");
        entry1.bv_id = m_portsOrch->m_portList[VLAN40].m_vlan_info.vlan_oid;
        entry1.port_name = ETH0;

        entry2.mac = MacAddress("7c:fe:90:12:22:ed");
        entry2.bv_id = m_portsOrch->m_portList[VLAN40].m_vlan_info.vlan_oid;
        entry2.port_name = ETH0;

        entry3.mac = MacAddress("7c:fe:90:12:22:ee");
        entry3.bv_id = m_portsOrch->m_portList[VLAN40].m_vlan_info.vlan_oid;
        entry3.port_name = ETH0;

        Port port = m_portsOrch->m_portList[ETH0];

        // Manually add entries to port cache
        gFdbOrch->m_entries_by_port[port.m_alias].push_back(entry1);
        gFdbOrch->m_entries_by_port[port.m_alias].push_back(entry2);
        gFdbOrch->m_entries_by_port[port.m_alias].push_back(entry3);

        // Verify all entries exist
        ASSERT_EQ(gFdbOrch->m_entries_by_port[port.m_alias].size(), 3);

        // Remove middle entry
        gFdbOrch->removeFdbEntryFromPortCache(entry2, port);

        // Verify only the specific entry is removed
        ASSERT_EQ(gFdbOrch->m_entries_by_port[port.m_alias].size(), 2);

        // Verify remaining entries are correct
        bool found_entry1 = false, found_entry3 = false;
        for (const auto& entry : gFdbOrch->m_entries_by_port[port.m_alias])
        {
            if (entry.mac.to_string() == "7c:fe:90:12:22:ec")
                found_entry1 = true;
            if (entry.mac.to_string() == "7c:fe:90:12:22:ee")
                found_entry3 = true;
        }

        ASSERT_TRUE(found_entry1);
        ASSERT_TRUE(found_entry3);
    }

    TEST_F(VxlanFdbOrchTest, RemoveFdbEntryFromPortCacheNonExistentEntry)
    {
        Table portTable = Table(m_app_db.get(), APP_PORT_TABLE_NAME);

        // Get SAI default ports to populate DB
        auto ports = ut_helper::getInitialSaiPorts();

        // Populate port table with SAI ports
        for (const auto &it : ports)
        {
            portTable.set(it.first, it.second);
        }

        // Set PortConfigDone, PortInitDone
        portTable.set("PortConfigDone", { { "count", to_string(ports.size()) } });
        portTable.set("PortInitDone", { { "lanes", "0" } });

        m_portsOrch.get()->addExistingData(&portTable);
        static_cast<Orch *>(m_portsOrch.get())->doTask();

        ASSERT_NE(m_portsOrch, nullptr);
        setUpVlan(m_portsOrch.get());
        setUpPort(m_portsOrch.get());
        setUpVlanMember(m_portsOrch.get());

        // Create FDB entries - one that exists and one that doesn't
        FdbEntry existingEntry, nonExistentEntry;
        existingEntry.mac = MacAddress("7c:fe:90:12:22:ec");
        existingEntry.bv_id = m_portsOrch->m_portList[VLAN40].m_vlan_info.vlan_oid;
        existingEntry.port_name = ETH0;

        nonExistentEntry.mac = MacAddress("7c:fe:90:12:22:ed");
        nonExistentEntry.bv_id = m_portsOrch->m_portList[VLAN40].m_vlan_info.vlan_oid;
        nonExistentEntry.port_name = ETH0;

        Port port = m_portsOrch->m_portList[ETH0];

        // Only add the existing entry to cache
        gFdbOrch->m_entries_by_port[port.m_alias].push_back(existingEntry);

        // Verify initial state
        ASSERT_EQ(gFdbOrch->m_entries_by_port[port.m_alias].size(), 1);

        // Try to remove non-existent entry - should not crash and should not affect existing entry
        gFdbOrch->removeFdbEntryFromPortCache(nonExistentEntry, port);

        // Verify cache is unchanged
        ASSERT_EQ(gFdbOrch->m_entries_by_port[port.m_alias].size(), 1);
        ASSERT_EQ(gFdbOrch->m_entries_by_port[port.m_alias][0].mac.to_string(), "7c:fe:90:12:22:ec");
    }
}
