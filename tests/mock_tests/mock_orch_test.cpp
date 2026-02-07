#include "mock_orch_test.h"

using namespace std;

namespace mock_orch_test
{

void MockOrchTest::ApplyInitialConfigs() {}
void MockOrchTest::PostSetUp() {}
void MockOrchTest::PreTearDown() {}
void MockOrchTest::ApplySaiMock() {}

void MockOrchTest::PrepareSai()
{
    sai_attribute_t attr;

    attr.id = SAI_SWITCH_ATTR_INIT_SWITCH;
    attr.value.booldata = true;

    sai_status_t status = sai_switch_api->create_switch(&gSwitchId, 1, &attr);
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

    /* Create a loopback underlay router interface */
    vector<sai_attribute_t> underlay_intf_attrs;

    sai_attribute_t underlay_intf_attr;
    underlay_intf_attr.id = SAI_ROUTER_INTERFACE_ATTR_VIRTUAL_ROUTER_ID;
    underlay_intf_attr.value.oid = gVirtualRouterId;
    underlay_intf_attrs.push_back(underlay_intf_attr);

    underlay_intf_attr.id = SAI_ROUTER_INTERFACE_ATTR_TYPE;
    underlay_intf_attr.value.s32 = SAI_ROUTER_INTERFACE_TYPE_LOOPBACK;
    underlay_intf_attrs.push_back(underlay_intf_attr);

    underlay_intf_attr.id = SAI_ROUTER_INTERFACE_ATTR_MTU;
    underlay_intf_attr.value.u32 = 9100;
    underlay_intf_attrs.push_back(underlay_intf_attr);

    status = sai_router_intfs_api->create_router_interface(&gUnderlayIfId, gSwitchId, (uint32_t)underlay_intf_attrs.size(), underlay_intf_attrs.data());
    ASSERT_EQ(status, SAI_STATUS_SUCCESS);

    // Bulkers will use the SAI implementation that exists when they are created in Orch constructors
    // so we need to apply the mock SAI API before any Orchs are created
    ApplySaiMock(); 
}

void MockOrchTest::SetUp()
{
    map<string, string> profile = {
        { "SAI_VS_SWITCH_TYPE", "SAI_VS_SWITCH_TYPE_BCM56850" },
        { "KV_DEVICE_MAC_ADDRESS", "20:03:04:05:06:00" }
    };

    ut_helper::initSaiApi(profile);
    m_app_db = make_shared<swss::DBConnector>("APPL_DB", 0);
    m_config_db = make_shared<swss::DBConnector>("CONFIG_DB", 0);
    m_state_db = make_shared<swss::DBConnector>("STATE_DB", 0);
    m_dpu_app_db = make_shared<swss::DBConnector>("DPU_APPL_DB", 0);
    m_dpu_app_state_db = make_shared<swss::DBConnector>("DPU_APPL_STATE_DB", 0);
    m_chassis_app_db = make_shared<swss::DBConnector>("CHASSIS_APP_DB", 0);

    PrepareSai();

    const int portsorch_base_pri = 40;
    vector<table_name_with_pri_t> ports_tables = {
        { APP_PORT_TABLE_NAME, portsorch_base_pri + 5 },
        { APP_VLAN_TABLE_NAME, portsorch_base_pri + 2 },
        { APP_VLAN_MEMBER_TABLE_NAME, portsorch_base_pri },
        { APP_LAG_TABLE_NAME, portsorch_base_pri + 4 },
        { APP_LAG_MEMBER_TABLE_NAME, portsorch_base_pri }
    };

    TableConnector stateDbSwitchTable(m_state_db.get(), STATE_SWITCH_CAPABILITY_TABLE_NAME);
    TableConnector app_switch_table(m_app_db.get(), APP_SWITCH_TABLE_NAME);
    TableConnector conf_asic_sensors(m_config_db.get(), CFG_ASIC_SENSORS_TABLE_NAME);

    vector<TableConnector> switch_tables = {
        conf_asic_sensors,
        app_switch_table
    };

    gSwitchOrch = new SwitchOrch(m_app_db.get(), switch_tables, stateDbSwitchTable);
    gDirectory.set(gSwitchOrch);
    ut_orch_list.push_back((Orch **)&gSwitchOrch);
    global_orch_list.insert((Orch **)&gSwitchOrch);

    vector<string> flex_counter_tables = {
        CFG_FLEX_COUNTER_TABLE_NAME
    };

    m_FlexCounterOrch = new FlexCounterOrch(m_config_db.get(), flex_counter_tables);
    gDirectory.set(m_FlexCounterOrch);
    ut_orch_list.push_back((Orch **)&m_FlexCounterOrch);

    static const vector<string> route_pattern_tables = {
        CFG_FLOW_COUNTER_ROUTE_PATTERN_TABLE_NAME,
    };
    gFlowCounterRouteOrch = new FlowCounterRouteOrch(m_config_db.get(), route_pattern_tables);
    gDirectory.set(gFlowCounterRouteOrch);
    ut_orch_list.push_back((Orch **)&gFlowCounterRouteOrch);
    global_orch_list.insert((Orch **)&gFlowCounterRouteOrch);

    gVrfOrch = new VRFOrch(m_app_db.get(), APP_VRF_TABLE_NAME, m_state_db.get(), STATE_VRF_OBJECT_TABLE_NAME);
    gDirectory.set(gVrfOrch);
    ut_orch_list.push_back((Orch **)&gVrfOrch);
    global_orch_list.insert((Orch **)&gVrfOrch);

    gIntfsOrch = new IntfsOrch(m_app_db.get(), APP_INTF_TABLE_NAME, gVrfOrch, m_chassis_app_db.get());
    gDirectory.set(gIntfsOrch);
    ut_orch_list.push_back((Orch **)&gIntfsOrch);
    global_orch_list.insert((Orch **)&gIntfsOrch);

    gPortsOrch = new PortsOrch(m_app_db.get(), m_state_db.get(), ports_tables, m_chassis_app_db.get());
    gDirectory.set(gPortsOrch);
    ut_orch_list.push_back((Orch **)&gPortsOrch);
    global_orch_list.insert((Orch **)&gPortsOrch);

    const int fgnhgorch_pri = 15;

    vector<table_name_with_pri_t> fgnhg_tables = {
        { CFG_FG_NHG, fgnhgorch_pri },
        { CFG_FG_NHG_PREFIX, fgnhgorch_pri },
        { CFG_FG_NHG_MEMBER, fgnhgorch_pri }
    };

    gFgNhgOrch = new FgNhgOrch(m_config_db.get(), m_app_db.get(), m_state_db.get(), fgnhg_tables, gNeighOrch, gIntfsOrch, gVrfOrch);
    gDirectory.set(gFgNhgOrch);
    ut_orch_list.push_back((Orch **)&gFgNhgOrch);
    global_orch_list.insert((Orch **)&gFgNhgOrch);

    const int fdborch_pri = 20;

    vector<table_name_with_pri_t> app_fdb_tables = {
        { APP_FDB_TABLE_NAME, FdbOrch::fdborch_pri },
        { APP_VXLAN_FDB_TABLE_NAME, FdbOrch::fdborch_pri },
        { APP_MCLAG_FDB_TABLE_NAME, fdborch_pri }
    };

    TableConnector stateDbFdb(m_state_db.get(), STATE_FDB_TABLE_NAME);
    TableConnector stateMclagDbFdb(m_state_db.get(), STATE_MCLAG_REMOTE_FDB_TABLE_NAME);
    gFdbOrch = new FdbOrch(m_app_db.get(), app_fdb_tables, stateDbFdb, stateMclagDbFdb, gPortsOrch);
    gDirectory.set(gFdbOrch);
    ut_orch_list.push_back((Orch **)&gFdbOrch);
    global_orch_list.insert((Orch **)&gFdbOrch);

    gNeighOrch = new NeighOrch(m_app_db.get(), APP_NEIGH_TABLE_NAME, gIntfsOrch, gFdbOrch, gPortsOrch, m_chassis_app_db.get());
    gDirectory.set(gNeighOrch);
    ut_orch_list.push_back((Orch **)&gNeighOrch);
    global_orch_list.insert((Orch **)&gNeighOrch);

    vector<string> tunnel_tables = {
        APP_TUNNEL_DECAP_TABLE_NAME,
        APP_TUNNEL_DECAP_TERM_TABLE_NAME
    };
    m_TunnelDecapOrch = new TunnelDecapOrch(m_app_db.get(), m_state_db.get(), m_config_db.get(), tunnel_tables);
    gDirectory.set(m_TunnelDecapOrch);
    ut_orch_list.push_back((Orch **)&m_TunnelDecapOrch);
    vector<string> mux_tables = {
        CFG_MUX_CABLE_TABLE_NAME,
        CFG_PEER_SWITCH_TABLE_NAME
    };

    vector<string> buffer_tables = {
        APP_BUFFER_POOL_TABLE_NAME,
        APP_BUFFER_PROFILE_TABLE_NAME,
        APP_BUFFER_QUEUE_TABLE_NAME,
        APP_BUFFER_PG_TABLE_NAME,
        APP_BUFFER_PORT_INGRESS_PROFILE_LIST_NAME,
        APP_BUFFER_PORT_EGRESS_PROFILE_LIST_NAME
    };
    gBufferOrch = new BufferOrch(m_app_db.get(), m_config_db.get(), m_state_db.get(), buffer_tables);
    ut_orch_list.push_back((Orch **)&gBufferOrch);
    global_orch_list.insert((Orch **)&gBufferOrch);

    vector<TableConnector> policer_tables = {
        TableConnector(m_config_db.get(), CFG_POLICER_TABLE_NAME),
        TableConnector(m_config_db.get(), CFG_PORT_STORM_CONTROL_TABLE_NAME)
    };

    TableConnector stateDbStorm(m_state_db.get(), STATE_BUM_STORM_CAPABILITY_TABLE_NAME);
    gPolicerOrch = new PolicerOrch(policer_tables, gPortsOrch);
    gDirectory.set(gPolicerOrch);
    ut_orch_list.push_back((Orch **)&gPolicerOrch);
    global_orch_list.insert((Orch **)&gPolicerOrch);

    gNhgOrch = new NhgOrch(m_app_db.get(), APP_NEXTHOP_GROUP_TABLE_NAME);
    gDirectory.set(gNhgOrch);
    ut_orch_list.push_back((Orch **)&gNhgOrch);
    global_orch_list.insert((Orch **)&gNhgOrch);

    TableConnector srv6_sid_list_table(m_app_db.get(), APP_SRV6_SID_LIST_TABLE_NAME);
    TableConnector srv6_my_sid_table(m_app_db.get(), APP_SRV6_MY_SID_TABLE_NAME);
    TableConnector srv6_my_sid_cfg_table(m_config_db.get(), CFG_SRV6_MY_SID_TABLE_NAME);

    vector<TableConnector> srv6_tables = {
        srv6_sid_list_table,
        srv6_my_sid_table,
        srv6_my_sid_cfg_table
    };
    gSrv6Orch = new Srv6Orch(m_config_db.get(), m_app_db.get(), srv6_tables, gSwitchOrch, gVrfOrch, gNeighOrch);
    gDirectory.set(gSrv6Orch);
    ut_orch_list.push_back((Orch **)&gSrv6Orch);
    global_orch_list.insert((Orch **)&gSrv6Orch);

    gCrmOrch = new CrmOrch(m_config_db.get(), CFG_CRM_TABLE_NAME);
    gDirectory.set(gCrmOrch);
    ut_orch_list.push_back((Orch **)&gCrmOrch);
    global_orch_list.insert((Orch **)&gCrmOrch);

    vector<string> ars_tables = {
        CFG_ARS_PROFILE_TABLE_NAME,
        CFG_ARS_INTERFACE_TABLE_NAME,
        CFG_ARS_OBJECT_TABLE_NAME,
        CFG_ARS_NEXTHOP_TABLE_NAME
    };
    gArsOrch = new ArsOrch(m_config_db.get(), m_app_db.get(), m_state_db.get(), ars_tables, gVrfOrch);
    gDirectory.set(gArsOrch);
    ut_orch_list.push_back((Orch **)&gArsOrch);
    global_orch_list.insert((Orch **)&gArsOrch);

    const int routeorch_pri = 5;
    vector<table_name_with_pri_t> route_tables = {
        { APP_ROUTE_TABLE_NAME, routeorch_pri },
        { APP_LABEL_ROUTE_TABLE_NAME, routeorch_pri }
    };
    gRouteOrch = new RouteOrch(m_app_db.get(), route_tables, gSwitchOrch, gNeighOrch, gIntfsOrch, gVrfOrch, gFgNhgOrch, gSrv6Orch, gArsOrch);
    gDirectory.set(gRouteOrch);
    ut_orch_list.push_back((Orch **)&gRouteOrch);
    global_orch_list.insert((Orch **)&gRouteOrch);

    TableConnector stateDbMirrorSession(m_state_db.get(), STATE_MIRROR_SESSION_TABLE_NAME);
    TableConnector confDbMirrorSession(m_config_db.get(), CFG_MIRROR_SESSION_TABLE_NAME);
    gMirrorOrch = new MirrorOrch(stateDbMirrorSession, confDbMirrorSession, gPortsOrch, gRouteOrch, gNeighOrch, gFdbOrch, gPolicerOrch, gSwitchOrch);
    gDirectory.set(gMirrorOrch);
    ut_orch_list.push_back((Orch **)&gMirrorOrch);
    global_orch_list.insert((Orch **)&gMirrorOrch);

    vector<string> dash_tables = {
        APP_DASH_APPLIANCE_TABLE_NAME,
        APP_DASH_ROUTING_TYPE_TABLE_NAME,
        APP_DASH_ENI_TABLE_NAME,
        APP_DASH_ENI_ROUTE_TABLE_NAME,
        APP_DASH_QOS_TABLE_NAME
    };

    m_DashOrch = new DashOrch(m_app_db.get(), dash_tables, m_dpu_app_state_db.get(), nullptr);
    gDirectory.set(m_DashOrch);
    ut_orch_list.push_back((Orch **)&m_DashOrch);

    vector<string> dash_meter_tables = {
        APP_DASH_METER_POLICY_TABLE_NAME,
        APP_DASH_METER_RULE_TABLE_NAME
    };

    m_DashMeterOrch = new DashMeterOrch(m_app_db.get(), dash_meter_tables, m_dpu_app_state_db.get(), nullptr);
    gDirectory.set(m_DashMeterOrch);
    ut_orch_list.push_back((Orch **)&m_DashMeterOrch);

    TableConnector confDbAclTable(m_config_db.get(), CFG_ACL_TABLE_TABLE_NAME);
    TableConnector confDbAclTableType(m_config_db.get(), CFG_ACL_TABLE_TYPE_TABLE_NAME);
    TableConnector confDbAclRuleTable(m_config_db.get(), CFG_ACL_RULE_TABLE_NAME);
    TableConnector appDbAclTable(m_app_db.get(), APP_ACL_TABLE_TABLE_NAME);
    TableConnector appDbAclTableType(m_app_db.get(), APP_ACL_TABLE_TYPE_TABLE_NAME);
    TableConnector appDbAclRuleTable(m_app_db.get(), APP_ACL_RULE_TABLE_NAME);

    vector<TableConnector> acl_table_connectors = {
        confDbAclTableType,
        confDbAclTable,
        confDbAclRuleTable,
        appDbAclTable,
        appDbAclRuleTable,
        appDbAclTableType,
    };
    gAclOrch = new AclOrch(acl_table_connectors, m_state_db.get(),
                            gSwitchOrch, gPortsOrch, gMirrorOrch, gNeighOrch, gRouteOrch, NULL);
    gDirectory.set(gAclOrch);
    ut_orch_list.push_back((Orch **)&gAclOrch);
    global_orch_list.insert((Orch **)&gAclOrch);

    m_MuxOrch = new MuxOrch(m_config_db.get(), mux_tables, m_TunnelDecapOrch, gNeighOrch, gFdbOrch);
    gDirectory.set(m_MuxOrch);
    ut_orch_list.push_back((Orch **)&m_MuxOrch);

    m_MuxCableOrch = new MuxCableOrch(m_app_db.get(), m_state_db.get(), APP_MUX_CABLE_TABLE_NAME);
    gDirectory.set(m_MuxCableOrch);
    ut_orch_list.push_back((Orch **)&m_MuxCableOrch);

    m_MuxStateOrch = new MuxStateOrch(m_state_db.get(), STATE_HW_MUX_CABLE_TABLE_NAME);
    gDirectory.set(m_MuxStateOrch);
    ut_orch_list.push_back((Orch **)&m_MuxStateOrch);

    m_VxlanTunnelOrch = new VxlanTunnelOrch(m_state_db.get(), m_app_db.get(), APP_VXLAN_TUNNEL_TABLE_NAME);
    gDirectory.set(m_VxlanTunnelOrch);
    ut_orch_list.push_back((Orch **)&m_VxlanTunnelOrch);

    m_vnetOrch = new VNetOrch(m_app_db.get(), APP_VNET_TABLE_NAME);
    gDirectory.set(m_vnetOrch);
    ut_orch_list.push_back((Orch **)&m_vnetOrch);

    vector<string> dash_vnet_tables = {
        APP_DASH_VNET_TABLE_NAME,
        APP_DASH_VNET_MAPPING_TABLE_NAME
    };

    m_dashVnetOrch = new DashVnetOrch(m_app_db.get(), dash_vnet_tables, m_dpu_app_state_db.get(), nullptr);
    gDirectory.set(m_dashVnetOrch);
    ut_orch_list.push_back((Orch **)&m_dashVnetOrch);

    vector<string> dash_route_tables = {
        APP_DASH_ROUTE_TABLE_NAME,
        APP_DASH_ROUTE_RULE_TABLE_NAME,
        APP_DASH_ROUTE_GROUP_TABLE_NAME
    };

    m_DashRouteOrch = new DashRouteOrch(m_app_db.get(), dash_route_tables, m_DashOrch, m_dpu_app_state_db.get(), nullptr);
    gDirectory.set(m_DashRouteOrch);
    ut_orch_list.push_back((Orch **)&m_DashRouteOrch);

    vector<string> dash_tunnel_tables = {
        APP_DASH_TUNNEL_TABLE_NAME
    };
    m_DashTunnelOrch= new DashTunnelOrch(m_app_db.get(), dash_tunnel_tables, m_dpu_app_state_db.get(), nullptr);
    gDirectory.set(m_DashTunnelOrch);
    ut_orch_list.push_back((Orch **)&m_DashTunnelOrch);

    vector<string> dash_port_map_tables = {
        APP_DASH_OUTBOUND_PORT_MAP_TABLE_NAME,
        APP_DASH_OUTBOUND_PORT_MAP_RANGE_TABLE_NAME
    };
    m_dashPortMapOrch = new DashPortMapOrch(m_app_db.get(), dash_port_map_tables, m_dpu_app_state_db.get(), nullptr);
    gDirectory.set(m_dashPortMapOrch);
    ut_orch_list.push_back((Orch **)&m_dashPortMapOrch);

    ApplyInitialConfigs();
    PostSetUp();
}

void MockOrchTest::TearDown()
{
    PreTearDown();
    for (std::vector<Orch **>::reverse_iterator rit = ut_orch_list.rbegin(); rit != ut_orch_list.rend(); ++rit)
    {
        Orch **orch = *rit;
        delete *orch;
        if (global_orch_list.find(orch) != global_orch_list.end())
        {
            *orch = nullptr;
        }
    }

    gDirectory.m_values.clear();

    ut_helper::uninitSaiApi();
}
}
