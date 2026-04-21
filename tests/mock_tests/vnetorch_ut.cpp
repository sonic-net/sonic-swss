#define private public
#include "directory.h"
#undef private
#define protected public
#include "orch.h"
#undef protected
#include "ut_helper.h"
#define private public
#include "vnetorch.h"
#undef private
#include "mock_orchagent_main.h"
#include "mock_table.h"
#include "mock_sai_api.h"

extern string gMySwitchType;

EXTERN_MOCK_FNS

namespace vnetorch_test
{
    using namespace std;
    using ::testing::_;
    using ::testing::InSequence;
    using ::testing::Invoke;
    using ::testing::DoAll;
    using ::testing::SetArgPointee;
    using ::testing::Return;

    DEFINE_SAI_GENERIC_API_MOCK(next_hop, next_hop);
    DEFINE_SAI_GENERIC_APIS_MOCK(next_hop_group, next_hop_group, next_hop_group_member);

    shared_ptr<swss::DBConnector> m_app_db;
    shared_ptr<swss::DBConnector> m_config_db;
    shared_ptr<swss::DBConnector> m_state_db;
    shared_ptr<swss::DBConnector> m_chassis_app_db;

    struct VnetOrchTest : public ::testing::Test
    {
        VNetOrch *m_vnet_orch = nullptr;
        VNetCfgRouteOrch *m_cfg_vnet_rt_orch = nullptr;
        VNetRouteOrch *m_vnet_rt_orch = nullptr;
        VxlanTunnelOrch *m_vxlan_tunnel_orch = nullptr;
        FlexCounterOrch *m_flex_counter_orch = nullptr;

        void SetUp() override
        {
            ASSERT_EQ(sai_route_api, nullptr);
            map<string, string> profile = {
                { "SAI_VS_SWITCH_TYPE", "SAI_VS_SWITCH_TYPE_BCM56850" },
                { "KV_DEVICE_MAC_ADDRESS", "20:03:04:05:06:00" }
            };

            ut_helper::initSaiApi(profile);

            INIT_SAI_API_MOCK(next_hop);
            INIT_SAI_API_MOCK(next_hop_group);
            MockSaiApis();

            m_app_db = make_shared<swss::DBConnector>("APPL_DB", 0);
            m_config_db = make_shared<swss::DBConnector>("CONFIG_DB", 0);
            m_state_db = make_shared<swss::DBConnector>("STATE_DB", 0);
            if (gMySwitchType == "voq")
            {
                m_chassis_app_db = make_shared<swss::DBConnector>("CHASSIS_APP_DB", 0);
            }

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
            TableConnector app_switch_table(m_app_db.get(), APP_SWITCH_TABLE_NAME);
            vector<TableConnector> switch_tables = {
                conf_asic_sensors,
                app_switch_table
            };

            ASSERT_EQ(gSwitchOrch, nullptr);
            gSwitchOrch = new SwitchOrch(m_app_db.get(), switch_tables, stateDbSwitchTable);

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
            m_flex_counter_orch = new FlexCounterOrch(m_config_db.get(), flex_counter_tables);
            gDirectory.set(m_flex_counter_orch);

            ASSERT_EQ(gPortsOrch, nullptr);
            gPortsOrch = new PortsOrch(m_app_db.get(), m_state_db.get(), ports_tables, m_chassis_app_db.get());
            gDirectory.set(gPortsOrch);

            vector<string> vnet_tables = {
                APP_VNET_RT_TABLE_NAME,
                APP_VNET_RT_TUNNEL_TABLE_NAME
            };

            vector<string> cfg_vnet_tables = {
                CFG_VNET_RT_TABLE_NAME,
                CFG_VNET_RT_TUNNEL_TABLE_NAME
            };

            m_vnet_orch = new VNetOrch(m_app_db.get(), APP_VNET_TABLE_NAME);
            gDirectory.set(m_vnet_orch);
            m_cfg_vnet_rt_orch = new VNetCfgRouteOrch(m_config_db.get(), m_app_db.get(), cfg_vnet_tables);
            gDirectory.set(m_cfg_vnet_rt_orch);
            m_vnet_rt_orch = new VNetRouteOrch(m_app_db.get(), vnet_tables, m_vnet_orch);
            gDirectory.set(m_vnet_rt_orch);
            m_vxlan_tunnel_orch = new VxlanTunnelOrch(m_state_db.get(), m_app_db.get(), APP_VXLAN_TUNNEL_TABLE_NAME);
            gDirectory.set(m_vxlan_tunnel_orch);

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
            auto *mux_orch = new MuxOrch(m_config_db.get(), mux_tables, gTunneldecapOrch, gNeighOrch, gFdbOrch);
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

            static const vector<string> route_pattern_tables = {
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
            intfTable.set("Ethernet0", { {"NULL", "NULL" },
                                         {"mac_addr", "00:00:00:00:00:00" }});
            intfTable.set("Ethernet0:10.0.0.1/24", { { "scope", "global" },
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
        }

        void TearDown() override
        {
            RestoreSaiApis();
            DEINIT_SAI_API_MOCK(next_hop);
            DEINIT_SAI_API_MOCK(next_hop_group);

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

            delete gTunneldecapOrch;
            gTunneldecapOrch = nullptr;

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

            delete m_vnet_rt_orch;
            m_vnet_rt_orch = nullptr;

            delete m_vxlan_tunnel_orch;
            m_vxlan_tunnel_orch = nullptr;

            delete m_cfg_vnet_rt_orch;
            m_cfg_vnet_rt_orch = nullptr;

            delete m_vnet_orch;
            m_vnet_orch = nullptr;

            delete m_flex_counter_orch;
            m_flex_counter_orch = nullptr;

            ut_helper::uninitSaiApi();
        }
    };

    TEST_F(VnetOrchTest, VnetTunnelRouteEcmpMemberCreateFailureCleanup)
    {
        ASSERT_NE(m_vnet_rt_orch, nullptr);

        const string vnet = "VnetTest";
        NextHopGroupKey ecmp_nhg_key("10.0.0.2@Ethernet0,10.0.0.3@Ethernet0");

        m_vnet_rt_orch->vnet_tunnel_route_check_directly_connected[vnet] = true;

        const auto current_nhg_count = gRouteOrch->getNhgCount();

        auto create_first_member = [](sai_object_id_t *next_hop_group_member_id,
                                      sai_object_id_t switch_id,
                                      uint32_t attr_count,
                                      const sai_attribute_t *attr_list) -> sai_status_t {
            auto status = old_sai_next_hop_group_api->create_next_hop_group_member(
                next_hop_group_member_id, switch_id, attr_count, attr_list);
            EXPECT_EQ(status, SAI_STATUS_SUCCESS);
            EXPECT_NE(*next_hop_group_member_id, SAI_NULL_OBJECT_ID);
            return status;
        };

        auto create_second_member_fail = [](sai_object_id_t *next_hop_group_member_id,
                                            sai_object_id_t,
                                            uint32_t,
                                            const sai_attribute_t *) -> sai_status_t {
            *next_hop_group_member_id = SAI_NULL_OBJECT_ID;
            return SAI_STATUS_INSUFFICIENT_RESOURCES;
        };

        auto cleanup_member = [](sai_object_id_t next_hop_group_member_id) -> sai_status_t {
            EXPECT_NE(next_hop_group_member_id, SAI_NULL_OBJECT_ID);
            return old_sai_next_hop_group_api->remove_next_hop_group_member(next_hop_group_member_id);
        };

        auto cleanup_group = [](sai_object_id_t next_hop_group_id) -> sai_status_t {
            EXPECT_NE(next_hop_group_id, SAI_NULL_OBJECT_ID);
            return old_sai_next_hop_group_api->remove_next_hop_group(next_hop_group_id);
        };

        {
            InSequence seq;

            EXPECT_CALL(*mock_sai_next_hop_group_api, create_next_hop_group(_, _, _, _)).Times(1);
            EXPECT_CALL(*mock_sai_next_hop_group_api, create_next_hop_group_member(_, _, _, _))
                .Times(2)
                .WillOnce(Invoke(create_first_member))
                .WillOnce(Invoke(create_second_member_fail));
            EXPECT_CALL(*mock_sai_next_hop_group_api, remove_next_hop_group_member(_))
                .Times(1)
                .WillOnce(Invoke(cleanup_member));
            EXPECT_CALL(*mock_sai_next_hop_group_api, remove_next_hop_group(_))
                .Times(1)
                .WillOnce(Invoke(cleanup_group));
        }

        EXPECT_FALSE(m_vnet_rt_orch->addNextHopGroup(vnet, ecmp_nhg_key, nullptr, "", true));
        EXPECT_FALSE(m_vnet_rt_orch->hasNextHopGroup(vnet, ecmp_nhg_key));
        EXPECT_EQ(gRouteOrch->getNhgCount(), current_nhg_count);
    }
}
