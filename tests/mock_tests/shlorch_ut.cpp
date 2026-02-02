#define private public // make Directory::m_values available to clean it.
#include "directory.h"
#undef private
#define protected public
#include "orch.h"
#undef protected

#include "ut_helper.h"
#include "mock_orchagent_main.h"
#include "mock_table.h"
#include "bulker.h"
#include "common/vxlan_ut_helpers.h"

namespace vuh = vxlan_ut_helpers;

#include "gtest/gtest.h"

#include "shlorch.h"

#define VXLAN_REMOTE_2 "EVPN_2.2.2.2"
#define VXLAN_REMOTE_3 "EVPN_3.3.3.3"
#define VXLAN_REMOTE_4 "EVPN_4.4.4.4"
#define VXLAN_REMOTE_5 "EVPN_5.5.5.5"

#define VTEP_REMOTE_IP_2 "2.2.2.2"
#define VTEP_REMOTE_IP_3 "3.3.3.3"
#define VTEP_REMOTE_IP_4 "4.4.4.4"
#define VTEP_REMOTE_IP_5 "5.5.5.5"

extern ShlOrch *gShlOrch;
extern EvpnMhOrch *gEvpnMhOrch;
extern sai_isolation_group_api_t*  sai_isolation_group_api;

namespace shlorch_test
{
    using namespace std;

    static const string PEER_SWITCH_HOSTNAME = "peer_hostname";
    static const string PEER_IPV4_ADDRESS = "1.1.1.1";
    static const string TEST_INTERFACE_1 = "Ethernet4";
    static const string TEST_INTERFACE_2 = "Ethernet5";
    static const string ACTIVE = "active";
    static const string STANDBY = "standby";
    static const string STATE = "state";
    static const string VLAN_NAME_1 = "Vlan10";
    static const string VLAN_NAME_2 = "Vlan20";
    static const string SERVER_IP = "192.168.0.2";

    sai_isolation_group_api_t ut_sai_isolation_group_api, *pold_sai_isolation_group_api;
    sai_bridge_api_t ut_sai_bridge_api, *pold_sai_bridge_api;

    map<string, sai_object_id_t> tunnel_object_ids = {
        {VTEP_REMOTE_IP_2, 0x10000000002a7},
        {VTEP_REMOTE_IP_3, 0x10000000003a7},
        {VTEP_REMOTE_IP_4, 0x10000000004a7},
        {VTEP_REMOTE_IP_5, 0x10000000005a7},
    };

    map<string, sai_object_id_t> tunnel_bridge_port_ids = {
        {VTEP_REMOTE_IP_2, 0x3a000000002c78},
        {VTEP_REMOTE_IP_3, 0x3a000000003c78},
        {VTEP_REMOTE_IP_4, 0x3a000000004c78},
        {VTEP_REMOTE_IP_5, 0x3a000000005c78},
    };

    map<string, sai_object_id_t> isolation_group_ids = {
        {VTEP_REMOTE_IP_2, 0x4b000000002c78},
        {VTEP_REMOTE_IP_3, 0x4b000000003c78},
        {VTEP_REMOTE_IP_4, 0x4b000000004c78},
        {VTEP_REMOTE_IP_5, 0x4b000000005c78},
    };

    map<string, sai_object_id_t> isolation_group_member_ids_1 = {
        {VTEP_REMOTE_IP_2, 0x5c000000002c78},
        {VTEP_REMOTE_IP_3, 0x5c000000003c78},
        {VTEP_REMOTE_IP_4, 0x5c000000004c78},
        {VTEP_REMOTE_IP_5, 0x5c000000005c78},
    };

    map<string, sai_object_id_t> isolation_group_member_ids_2 = {
        {VTEP_REMOTE_IP_2, 0x6d000000002c78},
    };

    sai_object_id_t oid_values[2];
    int alloc_index;
    bool simulate_sai_failure = false;
    bool simulate_bridge_failure = false;

    void checkIsolationGroup(uint32_t attr_count, const sai_attribute_t *attr_list) {
        ASSERT_TRUE(attr_count == 1);
        ASSERT_TRUE(attr_list[0].id == SAI_ISOLATION_GROUP_ATTR_TYPE);
        ASSERT_TRUE(attr_list[0].value.s32 == SAI_ISOLATION_GROUP_TYPE_BRIDGE_PORT);
    }

    sai_status_t _ut_stub_sai_create_isolation_group(
    _Out_ sai_object_id_t *grp_id,
    _In_ sai_object_id_t switch_id,
    _In_ uint32_t attr_count,
    _In_ const sai_attribute_t *attr_list)
    {
        checkIsolationGroup(attr_count, attr_list);
        if (simulate_sai_failure) {
            return SAI_STATUS_FAILURE;
        }
        *grp_id = (sai_object_id_t)oid_values[alloc_index++];
        return SAI_STATUS_SUCCESS;
    }

    sai_status_t _ut_stub_sai_remove_isolation_group(
    _In_ sai_object_id_t grp_id)
    {
        if (simulate_sai_failure) {
            return SAI_STATUS_FAILURE;
        }
        return SAI_STATUS_SUCCESS;
    }

    sai_status_t _ut_stub_sai_create_isolation_group_member(
    _Out_ sai_object_id_t *grp_member_id,
    _In_ sai_object_id_t switch_id,
    _In_ uint32_t attr_count,
    _In_ const sai_attribute_t *attr_list)
    {
        if (simulate_sai_failure) {
            return SAI_STATUS_FAILURE;
        }
        *grp_member_id = (sai_object_id_t)(0x2);
        return SAI_STATUS_SUCCESS;
    }

    sai_status_t _ut_stub_sai_remove_isolation_group_member(
    _In_ sai_object_id_t grp_member_id)
    {
        if (simulate_sai_failure) {
            return SAI_STATUS_FAILURE;
        }
        return SAI_STATUS_SUCCESS;
    }

    void checkBridgePortAttribute(sai_attr_id_t attr)
    {
        ASSERT_TRUE(attr == SAI_BRIDGE_PORT_ATTR_ISOLATION_GROUP);
    }

    sai_status_t _ut_stub_sai_set_bridge_port_attribute(
        _In_ sai_object_id_t bridge_port_id,
        _In_ const sai_attribute_t *attr)
    {
        checkBridgePortAttribute(attr->id);
        if (simulate_bridge_failure) {
            return SAI_STATUS_FAILURE;
        }
        return SAI_STATUS_SUCCESS;
    }

    struct ShlOrchTest : public ::testing::Test
    {
    protected:
        std::vector<Orch **> ut_orch_list;
        shared_ptr<swss::DBConnector> m_app_db;
        shared_ptr<swss::DBConnector> m_config_db;
        shared_ptr<swss::DBConnector> m_state_db;
        shared_ptr<swss::DBConnector> m_chassis_app_db;
        MuxOrch *m_MuxOrch;
        MuxCableOrch *m_MuxCableOrch;
        MuxCable *m_MuxCable;
        TunnelDecapOrch *m_TunnelDecapOrch;
        MuxStateOrch *m_MuxStateOrch;
        FlexCounterOrch *m_FlexCounterOrch;
        VxlanTunnelOrch *m_VxlanTunnelOrch;
        EvpnNvoOrch *m_EvpnNvoOrch;

        ShlOrchTest()
        {
        }

        void ApplyDualTorConfigs()
        {
            Table peer_switch_table = Table(m_config_db.get(), CFG_PEER_SWITCH_TABLE_NAME);
            Table tunnel_table = Table(m_app_db.get(), APP_TUNNEL_DECAP_TABLE_NAME);
            Table port_table = Table(m_app_db.get(), APP_PORT_TABLE_NAME);
            Table vlan_table = Table(m_app_db.get(), APP_VLAN_TABLE_NAME);
            Table vlan_member_table = Table(m_app_db.get(), APP_VLAN_MEMBER_TABLE_NAME);
            Table neigh_table = Table(m_app_db.get(), APP_NEIGH_TABLE_NAME);
            Table intf_table = Table(m_app_db.get(), APP_INTF_TABLE_NAME);

            auto ports = ut_helper::getInitialSaiPorts();
            port_table.set(TEST_INTERFACE_1, ports[TEST_INTERFACE_1]);
            port_table.set(TEST_INTERFACE_2, ports[TEST_INTERFACE_2]);
            port_table.set("PortConfigDone", { { "count", to_string(1) } });
            port_table.set("PortInitDone", { {} });

            neigh_table.set(
                VLAN_NAME_1 + neigh_table.getTableNameSeparator() + SERVER_IP, { { "neigh", "62:f9:65:10:2f:04" },
                                                                               { "family", "IPv4" } });

            vlan_table.set(VLAN_NAME_1, { { "admin_status", "up" },
                                        { "mtu", "9100" },
                                        { "mac", "00:aa:bb:cc:dd:ee" } });
            vlan_member_table.set(
                VLAN_NAME_1 + vlan_member_table.getTableNameSeparator() + TEST_INTERFACE_1,
                { { "tagging_mode", "untagged" } });

            intf_table.set(VLAN_NAME_1, { { "grat_arp", "enabled" },
                                        { "proxy_arp", "enabled" },
                                        { "mac_addr", "00:00:00:00:00:00" } });
            intf_table.set(
                VLAN_NAME_1 + neigh_table.getTableNameSeparator() + "192.168.0.1/21", {
                                                                                        { "scope", "global" },
                                                                                        { "family", "IPv4" },
                                                                                    });

            peer_switch_table.set(PEER_SWITCH_HOSTNAME, { { "address_ipv4", PEER_IPV4_ADDRESS } });

            gPortsOrch->addExistingData(&port_table);
            gPortsOrch->addExistingData(&vlan_table);
            gPortsOrch->addExistingData(&vlan_member_table);
            static_cast<Orch *>(gPortsOrch)->doTask();

            gIntfsOrch->addExistingData(&intf_table);
            static_cast<Orch *>(gIntfsOrch)->doTask();

            m_TunnelDecapOrch->addExistingData(&tunnel_table);
            static_cast<Orch *>(m_TunnelDecapOrch)->doTask();

            gNeighOrch->addExistingData(&neigh_table);
            static_cast<Orch *>(gNeighOrch)->doTask();
        }

        void PrepareSai()
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
        }

        void ApplyVlanConfigs()
        {
            sai_bridge_api = pold_sai_bridge_api;

            Table vlan_table = Table(m_app_db.get(), APP_VLAN_TABLE_NAME);
            Table vlan_member_table = Table(m_app_db.get(), APP_VLAN_MEMBER_TABLE_NAME);

            vlan_table.set(VLAN_NAME_2, { { "admin_status", "up" },
                                        { "mtu", "9100" },
                                        { "mac", "00:aa:bb:cc:dd:ff" } });
            vlan_member_table.set(
                VLAN_NAME_2 + vlan_member_table.getTableNameSeparator() + TEST_INTERFACE_2,
                { { "tagging_mode", "untagged" } });

            gPortsOrch->addExistingData(&vlan_table);
            gPortsOrch->addExistingData(&vlan_member_table);
            static_cast<Orch *>(gPortsOrch)->doTask();

            sai_bridge_api = &ut_sai_bridge_api;
        }

        void SetUp() override
        {
            // Reset failure simulation flags
            simulate_sai_failure = false;
            simulate_bridge_failure = false;

            map<string, string> profile = {
                { "SAI_VS_SWITCH_TYPE", "SAI_VS_SWITCH_TYPE_BCM56850" },
                { "KV_DEVICE_MAC_ADDRESS", "20:03:04:05:06:00" }
            };

            ut_helper::initSaiApi(profile);
            m_app_db = make_shared<swss::DBConnector>("APPL_DB", 0);
            m_config_db = make_shared<swss::DBConnector>("CONFIG_DB", 0);
            m_state_db = make_shared<swss::DBConnector>("STATE_DB", 0);
            m_chassis_app_db = make_shared<swss::DBConnector>("CHASSIS_APP_DB", 0);

            PrepareSai();

            TableConnector appDbDfTable(m_app_db.get(), "EVPN_DF_TABLE");
            TableConnector confDbEvpnEsTable(m_config_db.get(), "EVPN_ETHERNET_SEGMENT");

            vector<TableConnector> evpn_df_es_table_connectors = {
                appDbDfTable,
                confDbEvpnEsTable,
            };

            gEvpnMhOrch = new EvpnMhOrch(evpn_df_es_table_connectors);
            gDirectory.set(gEvpnMhOrch);
            ut_orch_list.push_back((Orch **)&gEvpnMhOrch);

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

            m_FlexCounterOrch = new FlexCounterOrch(m_config_db.get(), flex_counter_tables);
            gDirectory.set(m_FlexCounterOrch);
            ut_orch_list.push_back((Orch **)&m_FlexCounterOrch);

            static const vector<string> route_pattern_tables = {
                CFG_FLOW_COUNTER_ROUTE_PATTERN_TABLE_NAME,
            };
            gFlowCounterRouteOrch = new FlowCounterRouteOrch(m_config_db.get(), route_pattern_tables);
            gDirectory.set(gFlowCounterRouteOrch);
            ut_orch_list.push_back((Orch **)&gFlowCounterRouteOrch);

            gVrfOrch = new VRFOrch(m_app_db.get(), APP_VRF_TABLE_NAME, m_state_db.get(), STATE_VRF_OBJECT_TABLE_NAME);
            gDirectory.set(gVrfOrch);
            ut_orch_list.push_back((Orch **)&gVrfOrch);

            gIntfsOrch = new IntfsOrch(m_app_db.get(), APP_INTF_TABLE_NAME, gVrfOrch, m_chassis_app_db.get());
            gDirectory.set(gIntfsOrch);
            ut_orch_list.push_back((Orch **)&gIntfsOrch);

            TableConnector stateDbSwitchTable(m_state_db.get(), "SWITCH_CAPABILITY");
            TableConnector app_switch_table(m_app_db.get(), APP_SWITCH_TABLE_NAME);
            TableConnector conf_asic_sensors(m_config_db.get(), CFG_ASIC_SENSORS_TABLE_NAME);

            vector<TableConnector> switch_tables = {
                conf_asic_sensors,
                app_switch_table
            };
            gSwitchOrch = new SwitchOrch(m_app_db.get(), switch_tables, stateDbSwitchTable);
            gDirectory.set(gSwitchOrch);
            ut_orch_list.push_back((Orch **)&gSwitchOrch);

            gPortsOrch = new PortsOrch(m_app_db.get(), m_state_db.get(), ports_tables, m_chassis_app_db.get());
            gDirectory.set(gPortsOrch);
            ut_orch_list.push_back((Orch **)&gPortsOrch);

            const int fgnhgorch_pri = 15;

            vector<table_name_with_pri_t> fgnhg_tables = {
                { CFG_FG_NHG, fgnhgorch_pri },
                { CFG_FG_NHG_PREFIX, fgnhgorch_pri },
                { CFG_FG_NHG_MEMBER, fgnhgorch_pri }
            };

            gFgNhgOrch = new FgNhgOrch(m_config_db.get(), m_app_db.get(), m_state_db.get(), fgnhg_tables, gNeighOrch, gIntfsOrch, gVrfOrch);
            gDirectory.set(gFgNhgOrch);
            ut_orch_list.push_back((Orch **)&gFgNhgOrch);

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

            gNeighOrch = new NeighOrch(m_app_db.get(), APP_NEIGH_TABLE_NAME, gIntfsOrch, gFdbOrch, gPortsOrch, m_chassis_app_db.get());
            gDirectory.set(gNeighOrch);
            ut_orch_list.push_back((Orch **)&gNeighOrch);

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
            gDirectory.set(gBufferOrch);
            ut_orch_list.push_back((Orch **)&gBufferOrch);

            vector<TableConnector> policer_tables = {
                TableConnector(m_config_db.get(), CFG_POLICER_TABLE_NAME),
                TableConnector(m_config_db.get(), CFG_PORT_STORM_CONTROL_TABLE_NAME)
            };

            TableConnector stateDbStorm(m_state_db.get(), STATE_BUM_STORM_CAPABILITY_TABLE_NAME);
            gPolicerOrch = new PolicerOrch(policer_tables, gPortsOrch);
            gDirectory.set(gPolicerOrch);
            ut_orch_list.push_back((Orch **)&gPolicerOrch);

            gNhgOrch = new NhgOrch(m_app_db.get(), APP_NEXTHOP_GROUP_TABLE_NAME);
            gDirectory.set(gNhgOrch);
            ut_orch_list.push_back((Orch **)&gNhgOrch);

            TableConnector srv6_sid_list_table(m_app_db.get(), APP_SRV6_SID_LIST_TABLE_NAME);
            TableConnector srv6_my_sid_table(m_app_db.get(), APP_SRV6_MY_SID_TABLE_NAME);

            vector<TableConnector> srv6_tables = {
                srv6_sid_list_table,
                srv6_my_sid_table
            };
            gSrv6Orch = new Srv6Orch(m_config_db.get(), m_app_db.get(), srv6_tables, gSwitchOrch, gVrfOrch, gNeighOrch);
            gDirectory.set(gSrv6Orch);
            ut_orch_list.push_back((Orch **)&gSrv6Orch);
            gCrmOrch = new CrmOrch(m_config_db.get(), CFG_CRM_TABLE_NAME);
            gDirectory.set(gCrmOrch);
            ut_orch_list.push_back((Orch **)&gCrmOrch);

            const int routeorch_pri = 5;
            vector<table_name_with_pri_t> route_tables = {
                { APP_ROUTE_TABLE_NAME, routeorch_pri },
                { APP_LABEL_ROUTE_TABLE_NAME, routeorch_pri }
            };
            gRouteOrch = new RouteOrch(m_app_db.get(), route_tables, gSwitchOrch, gNeighOrch, gIntfsOrch, gVrfOrch, gFgNhgOrch, gSrv6Orch);
            gDirectory.set(gRouteOrch);
            ut_orch_list.push_back((Orch **)&gRouteOrch);
            TableConnector stateDbMirrorSession(m_state_db.get(), STATE_MIRROR_SESSION_TABLE_NAME);
            TableConnector confDbMirrorSession(m_config_db.get(), CFG_MIRROR_SESSION_TABLE_NAME);
            gMirrorOrch = new MirrorOrch(stateDbMirrorSession, confDbMirrorSession, gPortsOrch, gRouteOrch, gNeighOrch, gFdbOrch, gPolicerOrch);
            gDirectory.set(gMirrorOrch);
            ut_orch_list.push_back((Orch **)&gMirrorOrch);

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

            m_EvpnNvoOrch = new EvpnNvoOrch(m_app_db.get(), APP_VXLAN_EVPN_NVO_TABLE_NAME);
            gDirectory.set(m_EvpnNvoOrch);
            ut_orch_list.push_back((Orch **)&m_EvpnNvoOrch);

            ApplyDualTorConfigs();

            vuh::setUpVxlanPort(VTEP_REMOTE_IP_2, tunnel_object_ids[VTEP_REMOTE_IP_2]);
            vuh::setUpVxlanPort(VTEP_REMOTE_IP_4, tunnel_object_ids[VTEP_REMOTE_IP_4]);
            vuh::setUpVxlanPort(VTEP_REMOTE_IP_5, tunnel_object_ids[VTEP_REMOTE_IP_5]);
            vuh::setUpVxlanMember(VTEP_REMOTE_IP_2, tunnel_object_ids[VTEP_REMOTE_IP_2], VLAN_NAME_1);
            vuh::setUpVxlanMember(VTEP_REMOTE_IP_4, tunnel_object_ids[VTEP_REMOTE_IP_4], VLAN_NAME_1);
            vuh::setUpVxlanMember(VTEP_REMOTE_IP_5, tunnel_object_ids[VTEP_REMOTE_IP_5], VLAN_NAME_1);

            ASSERT_EQ(gShlOrch, nullptr);

            TableConnector appDbShlTbl(m_app_db.get(), APP_EVPN_SPLIT_HORIZON_TABLE_NAME);
            vector<TableConnector> shl_tbl_ctrs = {
                appDbShlTbl
            };
            gShlOrch = new ShlOrch(shl_tbl_ctrs);
            gDirectory.set(gShlOrch);
            ut_orch_list.push_back((Orch **)&gShlOrch);

            // Mock isolation group API
            pold_sai_isolation_group_api = sai_isolation_group_api;
            sai_isolation_group_api = &ut_sai_isolation_group_api;
            ut_sai_isolation_group_api.create_isolation_group = _ut_stub_sai_create_isolation_group;
            ut_sai_isolation_group_api.create_isolation_group_member = _ut_stub_sai_create_isolation_group_member;
            ut_sai_isolation_group_api.remove_isolation_group = _ut_stub_sai_remove_isolation_group;
            ut_sai_isolation_group_api.remove_isolation_group_member = _ut_stub_sai_remove_isolation_group_member;

            // Mock bridge port attribute API
            pold_sai_bridge_api = sai_bridge_api;
            sai_bridge_api = &ut_sai_bridge_api;
            ut_sai_bridge_api.set_bridge_port_attribute = _ut_stub_sai_set_bridge_port_attribute;
        }

        void TearDown() override
        {
            for (std::vector<Orch **>::reverse_iterator rit = ut_orch_list.rbegin(); rit != ut_orch_list.rend(); ++rit)
            {
                Orch **orch = *rit;
                delete *orch;
                *orch = nullptr;
            }

            gDirectory.m_values.clear();

            sai_isolation_group_api = pold_sai_isolation_group_api;
            sai_bridge_api = pold_sai_bridge_api;

            ut_helper::uninitSaiApi();
        }
    };

    TEST_F(ShlOrchTest, ShlAddUpdateDeleteTest)
    {
        Table shlTable = Table(m_app_db.get(), APP_EVPN_SPLIT_HORIZON_TABLE_NAME);
        auto consumer = dynamic_cast<Consumer *>(gShlOrch->getExecutor(APP_EVPN_SPLIT_HORIZON_TABLE_NAME));

        std::deque<KeyOpFieldsValuesTuple> entries;

        /*
         * This DB entry will cause the following operations:
         * 1. Create Isolation group for VTEP "2.2.2.2"
         *    with member as Ethernet4 and bind port as Tunnel port for VTEP "2.2.2.2"
         * 2. Create Isolation group for VTEP "3.3.3.3" with member as Ethernet4
         *    and Tunnel port is not create yet, so Tunnel port for VTEP "3.3.3.3" is pendind bind port
         */
        shlTable.set("Vlan10:Ethernet4", {
            {"vteps", "2.2.2.2,3.3.3.3"}
        });

        alloc_index = 0;
        oid_values[0] = isolation_group_ids[VTEP_REMOTE_IP_2];
        oid_values[1] = isolation_group_ids[VTEP_REMOTE_IP_3];

        gShlOrch->addExistingData(&shlTable);
        static_cast<Orch *>(gShlOrch)->doTask();

        // check sai Isolation group object ids
        ASSERT_EQ(isolation_group_ids[VTEP_REMOTE_IP_2], gShlOrch->getIsolationGroup(VTEP_REMOTE_IP_2)->getIsolationGroupOid());
        ASSERT_EQ(isolation_group_ids[VTEP_REMOTE_IP_3], gShlOrch->getIsolationGroup(VTEP_REMOTE_IP_3)->getIsolationGroupOid());

        ASSERT_EQ((gShlOrch->getVtepsListCount()), 1);
        ASSERT_EQ((gShlOrch->getIsolationGroupCount()), 2);

        // Check the Isolation Group parameters created for VTEP "2.2.2.2"
        ASSERT_EQ((gShlOrch->getIsolationGroup(VTEP_REMOTE_IP_2)->getNumOfMembers()), 1);
        ASSERT_EQ((gShlOrch->getIsolationGroup(VTEP_REMOTE_IP_2)->getNumOfPendingMembers()), 0);
        ASSERT_EQ((gShlOrch->getIsolationGroup(VTEP_REMOTE_IP_2)->getNumOfBindPorts()), 1);
        ASSERT_EQ((gShlOrch->getIsolationGroup(VTEP_REMOTE_IP_2)->getNumOfPendingBindports()), 0);

        // Check the Isolation Group parameters created for VTEP "3.3.3.3"
        ASSERT_EQ((gShlOrch->getIsolationGroup(VTEP_REMOTE_IP_3)->getNumOfMembers()), 1);
        ASSERT_EQ((gShlOrch->getIsolationGroup(VTEP_REMOTE_IP_3)->getNumOfPendingMembers()), 0);
        ASSERT_EQ((gShlOrch->getIsolationGroup(VTEP_REMOTE_IP_3)->getNumOfBindPorts()), 0);
        ASSERT_EQ((gShlOrch->getIsolationGroup(VTEP_REMOTE_IP_3)->getNumOfPendingBindports()), 1);

        // Created Tunnel port for VTEP "3.3.3.3" to trigger update operation on Isolation group
        vuh::setUpVxlanPort(VTEP_REMOTE_IP_3, tunnel_object_ids[VTEP_REMOTE_IP_3]);
        vuh::setUpVxlanMember(VTEP_REMOTE_IP_3, tunnel_object_ids[VTEP_REMOTE_IP_3], VLAN_NAME_1);

        // Check Isolation group for VTEP "3.3.3.3" has the bind port
        ASSERT_EQ((gShlOrch->getIsolationGroup(VTEP_REMOTE_IP_3)->getNumOfBindPorts()), 1);
        ASSERT_EQ((gShlOrch->getIsolationGroup(VTEP_REMOTE_IP_3)->getNumOfPendingBindports()), 0);

        /*
         * This DB entry will cause the following operations:
         * 1. Use existing Isolation group for VTEP "2.2.2.2" and
         *    Ethernet5 bridge port is not up, so added to pending member port
         * 2. Create Isolation group for VTEP "4.4.4.4" and
         *    Ethernet5 bridge port is not up, so added to pending member port and
         *    and bind port as Tunnel port for VTEP "4.4.4.4"
         */
        entries.push_back({"Vlan10:Ethernet5", "SET", {
            {"vteps", "2.2.2.2,4.4.4.4"}
        }});
        consumer->addToSync(entries);
        static_cast<Orch *>(gShlOrch)->doTask();

        ASSERT_EQ((gShlOrch->getVtepsListCount()), 2);
        ASSERT_EQ((gShlOrch->getIsolationGroupCount()), 3);

        // Check the usage of existing Isolation Group parameters with new pending member
        ASSERT_EQ((gShlOrch->getIsolationGroup(VTEP_REMOTE_IP_2)->getNumOfMembers()), 1);
        ASSERT_EQ((gShlOrch->getIsolationGroup(VTEP_REMOTE_IP_2)->getNumOfPendingMembers()), 1);
        ASSERT_EQ((gShlOrch->getIsolationGroup(VTEP_REMOTE_IP_2)->getNumOfBindPorts()), 1);
        ASSERT_EQ((gShlOrch->getIsolationGroup(VTEP_REMOTE_IP_2)->getNumOfPendingBindports()), 0);

        // Check the creation of Isolation Group parameters with new pending member
        ASSERT_EQ((gShlOrch->getIsolationGroup(VTEP_REMOTE_IP_4)->getNumOfMembers()), 0);
        ASSERT_EQ((gShlOrch->getIsolationGroup(VTEP_REMOTE_IP_4)->getNumOfPendingMembers()), 1);
        ASSERT_EQ((gShlOrch->getIsolationGroup(VTEP_REMOTE_IP_4)->getNumOfBindPorts()), 1);
        ASSERT_EQ((gShlOrch->getIsolationGroup(VTEP_REMOTE_IP_4)->getNumOfPendingBindports()), 0);

        // Bring the bridge port up for Ethernet5
        ApplyVlanConfigs();

        // Check if pending member is converted as member port for Isolation group of VTEP "2.2.2.2"
        ASSERT_EQ((gShlOrch->getIsolationGroup(VTEP_REMOTE_IP_2)->getNumOfMembers()), 2);
        ASSERT_EQ((gShlOrch->getIsolationGroup(VTEP_REMOTE_IP_2)->getNumOfPendingMembers()), 0);

        // Check if pending member is converted as member port for Isolation group of VTEP "2.2.2.2"
        ASSERT_EQ((gShlOrch->getIsolationGroup(VTEP_REMOTE_IP_4)->getNumOfMembers()), 1);
        ASSERT_EQ((gShlOrch->getIsolationGroup(VTEP_REMOTE_IP_4)->getNumOfPendingMembers()), 0);

        /*
         * This DB entry will cause the following operations as an Update opeartion:
         * 1. Its a no-op for Isolation group for VTEP "2.2.2.2"
         * 2. Delete the Isolation group created for VTEP "4.4.4.4"
         * 3. Create Isolation group for VTEP "5.5.5.5"
         *    with member as Ethernet5 and bind port as Tunnel port for VTEP "5.5.5.5"
         */
        entries.push_back({"Vlan10:Ethernet5", "SET", {
            {"vteps", "2.2.2.2,5.5.5.5"}
        }});
        consumer->addToSync(entries);
        static_cast<Orch *>(gShlOrch)->doTask();

        ASSERT_EQ((gShlOrch->getVtepsListCount()), 2);
        ASSERT_EQ((gShlOrch->getIsolationGroupCount()), 3);

        // Check the Isolation Group parameters for VTEP "2.2.2.2" has not changed
        ASSERT_EQ((gShlOrch->getIsolationGroup(VTEP_REMOTE_IP_2)->getNumOfMembers()), 2);
        ASSERT_EQ((gShlOrch->getIsolationGroup(VTEP_REMOTE_IP_2)->getNumOfPendingMembers()), 0);
        ASSERT_EQ((gShlOrch->getIsolationGroup(VTEP_REMOTE_IP_2)->getNumOfBindPorts()), 1);
        ASSERT_EQ((gShlOrch->getIsolationGroup(VTEP_REMOTE_IP_2)->getNumOfPendingBindports()), 0);

        // Check Isolation group for VTEP "4.4.4.4" is deleted
        ASSERT_EQ(gShlOrch->getIsolationGroup(VTEP_REMOTE_IP_4), nullptr);

        // Check the Isolation Group parameters created for VTEP "5.5.5.5"
        ASSERT_EQ((gShlOrch->getIsolationGroup(VTEP_REMOTE_IP_5)->getNumOfMembers()), 1);
        ASSERT_EQ((gShlOrch->getIsolationGroup(VTEP_REMOTE_IP_5)->getNumOfPendingMembers()), 0);
        ASSERT_EQ((gShlOrch->getIsolationGroup(VTEP_REMOTE_IP_5)->getNumOfBindPorts()), 1);
        ASSERT_EQ((gShlOrch->getIsolationGroup(VTEP_REMOTE_IP_5)->getNumOfPendingBindports()), 0);

        /*
         * This DB entry will cause the following operations as delete opeartion:
         * 1. Member port deletion of "Ethernet5" of Isolation group for VTEP "2.2.2.2"
         * 2. Delete the Isolation group created for VTEP "3.3.3.3"
         */
        entries.push_back({"Vlan10:Ethernet4", "DEL", { {} }});
        consumer->addToSync(entries);
        static_cast<Orch *>(gShlOrch)->doTask();

        ASSERT_EQ((gShlOrch->getVtepsListCount()), 1);
        ASSERT_EQ((gShlOrch->getIsolationGroupCount()), 2);

        // Check the Isolation Group parameters for VTEP "2.2.2.2" has one member port deleted
        ASSERT_EQ((gShlOrch->getIsolationGroup(VTEP_REMOTE_IP_2)->getNumOfMembers()), 1);
        ASSERT_EQ((gShlOrch->getIsolationGroup(VTEP_REMOTE_IP_2)->getNumOfPendingMembers()), 0);
        ASSERT_EQ((gShlOrch->getIsolationGroup(VTEP_REMOTE_IP_2)->getNumOfBindPorts()), 1);
        ASSERT_EQ((gShlOrch->getIsolationGroup(VTEP_REMOTE_IP_2)->getNumOfPendingBindports()), 0);

        // Check Isolation group for VTEP "3.3.3.3" is deleted
        ASSERT_EQ(gShlOrch->getIsolationGroup(VTEP_REMOTE_IP_3), nullptr);

        /*
         * This DB entry will cause the following operations as delete opeartion:
         * 1. Delete the Isolation group created for VTEP "2.2.2.2"
         * 2. Delete the Isolation group created for VTEP "5.5.5.5"
         */
        entries.push_back({"Vlan10:Ethernet5", "DEL", { {} }});
        consumer->addToSync(entries);
        static_cast<Orch *>(gShlOrch)->doTask();

        // Check Isolation groups for VTEP "2.2.2.2" and "5.5.5.5" are deleted
        ASSERT_EQ(gShlOrch->getIsolationGroup(VTEP_REMOTE_IP_2), nullptr);
        ASSERT_EQ(gShlOrch->getIsolationGroup(VTEP_REMOTE_IP_5), nullptr);

        // Check VtepList cache and Isolation group count is zero
        ASSERT_EQ((gShlOrch->getVtepsListCount()), 0);
        ASSERT_EQ((gShlOrch->getIsolationGroupCount()), 0);
    }

    TEST_F(ShlOrchTest, ShlUnknownAttributeTest)
    {
        Table shlTable = Table(m_app_db.get(), APP_EVPN_SPLIT_HORIZON_TABLE_NAME);

        // Test unknown attribute handling
        shlTable.set("Vlan10:Ethernet4", {
            {"unknown_attr", "some_value"},
            {"vteps", "2.2.2.2"}
        });

        alloc_index = 0;
        oid_values[0] = isolation_group_ids[VTEP_REMOTE_IP_2];

        gShlOrch->addExistingData(&shlTable);
        static_cast<Orch *>(gShlOrch)->doTask();

        // Should still create isolation group despite unknown attribute
        ASSERT_EQ((gShlOrch->getIsolationGroupCount()), 1);
        ASSERT_NE(gShlOrch->getIsolationGroup(VTEP_REMOTE_IP_2), nullptr);

        // Clean up - delete the created entry
        auto consumer = dynamic_cast<Consumer *>(gShlOrch->getExecutor(APP_EVPN_SPLIT_HORIZON_TABLE_NAME));
        std::deque<KeyOpFieldsValuesTuple> cleanup_entries;
        cleanup_entries.push_back({"Vlan10:Ethernet4", "DEL", { {} }});
        consumer->addToSync(cleanup_entries);
        static_cast<Orch *>(gShlOrch)->doTask();

        // Verify cleanup
        ASSERT_EQ((gShlOrch->getIsolationGroupCount()), 0);
    }

    TEST_F(ShlOrchTest, ShlPortNotReadyTest)
    {
        // Clean up any existing isolation groups first
        auto consumer = dynamic_cast<Consumer *>(gShlOrch->getExecutor(APP_EVPN_SPLIT_HORIZON_TABLE_NAME));

        // COMPLETELY clear the sync queue to avoid interference from previous tests
        consumer->m_toSync.clear();

        // Also clear any existing entries in the actual table
        Table shlTable = Table(m_app_db.get(), APP_EVPN_SPLIT_HORIZON_TABLE_NAME);
        shlTable.del("Vlan10:Ethernet4");  // Clean up potential leftover
        shlTable.del("Vlan10:Ethernet5");  // Clean up potential leftover

        // Process any existing deletions first
        gShlOrch->addExistingData(&shlTable);
        static_cast<Orch *>(gShlOrch)->doTask();

        // Ensure we start with clean state
        ASSERT_EQ((gShlOrch->getIsolationGroupCount()), 0);

        // Test with a truly non-existent port - use a completely different name pattern
        std::string nonExistentPort = "Ethernet999";

        // Verify the port doesn't exist
        Port testPort;
        ASSERT_FALSE(gPortsOrch->getPort(nonExistentPort, testPort));

        // Now try to create isolation group with this non-existent port
        shlTable.set("Vlan10:" + nonExistentPort, {
            {"vteps", "2.2.2.2"}
        });

        alloc_index = 0;
        oid_values[0] = isolation_group_ids[VTEP_REMOTE_IP_2];

        gShlOrch->addExistingData(&shlTable);
        static_cast<Orch *>(gShlOrch)->doTask();

        // Should not create isolation group due to port not being ready
        // Entry should remain in sync queue for retry
        ASSERT_EQ((gShlOrch->getIsolationGroupCount()), 0);

        // The entry should remain in sync queue since port doesn't exist
        ASSERT_GT(consumer->m_toSync.size(), 0);
    }

    TEST_F(ShlOrchTest, ShlObserverUpdateTest)
    {
        Table shlTable = Table(m_app_db.get(), APP_EVPN_SPLIT_HORIZON_TABLE_NAME);

        // Set up isolation group first
        shlTable.set("Vlan10:Ethernet4", {
            {"vteps", "2.2.2.2"}
        });

        alloc_index = 0;
        oid_values[0] = isolation_group_ids[VTEP_REMOTE_IP_2];

        gShlOrch->addExistingData(&shlTable);
        static_cast<Orch *>(gShlOrch)->doTask();

        ASSERT_EQ((gShlOrch->getIsolationGroupCount()), 1);

        // Test ShlOrch::update() - observer pattern
        // This tests the update method that handles bridge port changes
        SubjectType type = SUBJECT_TYPE_BRIDGE_PORT_CHANGE;
        PortUpdate portUpdate;
        Port port;
        gPortsOrch->getPort("Ethernet4", port);
        portUpdate.port = port;
        portUpdate.add = false; // Port removal

        // Call the update method
        gShlOrch->update(type, &portUpdate);

        // Validate that the update was processed successfully
        // The isolation group should still exist since port removal doesn't automatically delete it
        ASSERT_EQ((gShlOrch->getIsolationGroupCount()), 1);
        auto isolationGroup = gShlOrch->getIsolationGroup(VTEP_REMOTE_IP_2);
        ASSERT_NE(isolationGroup, nullptr);
        // The update should have been processed without errors (no crash/exception)

        // Test with non-bridge port change type (should be ignored)
        gShlOrch->update(SUBJECT_TYPE_NEXTHOP_CHANGE, nullptr);

        // Clean up - delete the created entry
        auto consumer = dynamic_cast<Consumer *>(gShlOrch->getExecutor(APP_EVPN_SPLIT_HORIZON_TABLE_NAME));
        std::deque<KeyOpFieldsValuesTuple> cleanup_entries;
        cleanup_entries.push_back({"Vlan10:Ethernet4", "DEL", { {} }});
        consumer->addToSync(cleanup_entries);
        static_cast<Orch *>(gShlOrch)->doTask();

        // Verify cleanup
        ASSERT_EQ((gShlOrch->getIsolationGroupCount()), 0);
    }

    TEST_F(ShlOrchTest, ShlIsolationGroupGettersTest)
    {
        Table shlTable = Table(m_app_db.get(), APP_EVPN_SPLIT_HORIZON_TABLE_NAME);

        // Set up isolation group
        shlTable.set("Vlan10:Ethernet4", {
            {"vteps", "2.2.2.2"}
        });

        alloc_index = 0;
        oid_values[0] = isolation_group_ids[VTEP_REMOTE_IP_2];

        gShlOrch->addExistingData(&shlTable);
        static_cast<Orch *>(gShlOrch)->doTask();

        auto isolationGroup = gShlOrch->getIsolationGroup(VTEP_REMOTE_IP_2);
        ASSERT_NE(isolationGroup, nullptr);

        // Test getter methods
        ASSERT_EQ(isolationGroup->getIsolationGroupOid(), isolation_group_ids[VTEP_REMOTE_IP_2]);

        // Test bind ports getter
        auto bindPorts = isolationGroup->getBindPorts();
        ASSERT_EQ(bindPorts.size(), 1);

        // Test member ports getter
        auto memberPorts = isolationGroup->getMemberPorts();
        ASSERT_EQ(memberPorts.size(), 1);

        // Test isolation group member OID getter
        sai_object_id_t memberOid = isolationGroup->getIsolationGroupMemberOid("Ethernet4");
        ASSERT_NE(memberOid, SAI_NULL_OBJECT_ID);

        // Test ShlOrch getIsolationGroupsList
        auto groupsList = gShlOrch->getIsolationGroupsList();
        ASSERT_EQ(groupsList.size(), 1);
        ASSERT_NE(groupsList.find(VTEP_REMOTE_IP_2), groupsList.end());

        // Clean up - delete the created entry
        auto consumer = dynamic_cast<Consumer *>(gShlOrch->getExecutor(APP_EVPN_SPLIT_HORIZON_TABLE_NAME));
        std::deque<KeyOpFieldsValuesTuple> cleanup_entries;
        cleanup_entries.push_back({"Vlan10:Ethernet4", "DEL", { {} }});
        consumer->addToSync(cleanup_entries);
        static_cast<Orch *>(gShlOrch)->doTask();

        // Verify cleanup
        ASSERT_EQ((gShlOrch->getIsolationGroupCount()), 0);
    }

    TEST_F(ShlOrchTest, ShlErrorHandlingTest)
    {
        Table shlTable = Table(m_app_db.get(), APP_EVPN_SPLIT_HORIZON_TABLE_NAME);

        // Test delete operation on non-existent interface
        shlTable.set("Vlan10:Ethernet4", {
            {"vteps", "2.2.2.2"}
        });

        gShlOrch->addExistingData(&shlTable);
        static_cast<Orch *>(gShlOrch)->doTask();

        // Now try to delete non-existent entry
        auto consumer = dynamic_cast<Consumer *>(gShlOrch->getExecutor(APP_EVPN_SPLIT_HORIZON_TABLE_NAME));
        std::deque<KeyOpFieldsValuesTuple> entries;

        entries.push_back({"Vlan10:EthernetNonExist", "DEL", { {} }});
        consumer->addToSync(entries);
        static_cast<Orch *>(gShlOrch)->doTask();

        // Should handle gracefully without crashing
        ASSERT_EQ((gShlOrch->getIsolationGroupCount()), 1); // Original group should remain

        // Clean up - delete the created entry
        std::deque<KeyOpFieldsValuesTuple> cleanup_entries;
        cleanup_entries.push_back({"Vlan10:Ethernet4", "DEL", { {} }});
        consumer->addToSync(cleanup_entries);
        static_cast<Orch *>(gShlOrch)->doTask();

        // Verify cleanup
        ASSERT_EQ((gShlOrch->getIsolationGroupCount()), 0);
    }

    TEST_F(ShlOrchTest, ShlPendingMemberHandlingTest)
    {
        Table shlTable = Table(m_app_db.get(), APP_EVPN_SPLIT_HORIZON_TABLE_NAME);

        // Test isolation group with port that has no bridge port ID initially
        shlTable.set("Vlan10:Ethernet4", {
            {"vteps", "3.3.3.3"}  // Use VTEP that has pending bind port
        });

        alloc_index = 0;
        oid_values[0] = isolation_group_ids[VTEP_REMOTE_IP_3];

        gShlOrch->addExistingData(&shlTable);
        static_cast<Orch *>(gShlOrch)->doTask();

        auto isolationGroup = gShlOrch->getIsolationGroup(VTEP_REMOTE_IP_3);
        ASSERT_NE(isolationGroup, nullptr);

        // Should have pending bind port since tunnel for 3.3.3.3 is not set up initially
        ASSERT_EQ(isolationGroup->getNumOfPendingBindports(), 1);

        // Test delMember with do_fwd_ref parameter
        Port port;
        gPortsOrch->getPort("Ethernet4", port);

        // Test delMember with forward reference (moves to pending)
        isolationGroup->delMember(port, true);
        ASSERT_EQ(isolationGroup->getNumOfPendingMembers(), 1);

        // Test unbind with forward reference
        isolationGroup->unbind(VTEP_REMOTE_IP_3, true);

        // Clean up - delete the created entry
        auto consumer = dynamic_cast<Consumer *>(gShlOrch->getExecutor(APP_EVPN_SPLIT_HORIZON_TABLE_NAME));
        std::deque<KeyOpFieldsValuesTuple> cleanup_entries;
        cleanup_entries.push_back({"Vlan10:Ethernet4", "DEL", { {} }});
        consumer->addToSync(cleanup_entries);
        static_cast<Orch *>(gShlOrch)->doTask();

        // Verify cleanup
        ASSERT_EQ((gShlOrch->getIsolationGroupCount()), 0);
    }

    TEST_F(ShlOrchTest, ShlSaiFailureTest)
    {
        Table shlTable = Table(m_app_db.get(), APP_EVPN_SPLIT_HORIZON_TABLE_NAME);

        // Test SAI isolation group creation failure
        simulate_sai_failure = true;

        shlTable.set("Vlan10:Ethernet4", {
            {"vteps", "2.2.2.2"}
        });

        alloc_index = 0;
        oid_values[0] = isolation_group_ids[VTEP_REMOTE_IP_2];

        gShlOrch->addExistingData(&shlTable);
        static_cast<Orch *>(gShlOrch)->doTask();

        // Should not create isolation group due to SAI failure
        ASSERT_EQ((gShlOrch->getIsolationGroupCount()), 0);

        simulate_sai_failure = false; // Reset for other tests
    }

    TEST_F(ShlOrchTest, ShlBridgePortFailureTest)
    {
        Table shlTable = Table(m_app_db.get(), APP_EVPN_SPLIT_HORIZON_TABLE_NAME);

        // First create isolation group successfully
        shlTable.set("Vlan10:Ethernet4", {
            {"vteps", "2.2.2.2"}
        });

        alloc_index = 0;
        oid_values[0] = isolation_group_ids[VTEP_REMOTE_IP_2];

        gShlOrch->addExistingData(&shlTable);
        static_cast<Orch *>(gShlOrch)->doTask();

        ASSERT_EQ((gShlOrch->getIsolationGroupCount()), 1);

        // Now simulate bridge port attribute failure
        simulate_bridge_failure = true;

        auto isolationGroup = gShlOrch->getIsolationGroup(VTEP_REMOTE_IP_2);
        ASSERT_NE(isolationGroup, nullptr);

        Port port;
        gPortsOrch->getPort("Ethernet4", port);

        // Record bind port count before the failing bind attempt
        auto bindPortCountBeforeFailure = isolationGroup->getNumOfBindPorts();

        // This should fail due to bridge API failure but doesn't return error status
        // The error will be logged internally
        isolationGroup->bind(port);

        // Verify that the bind operation failed - bind port count should not increase
        ASSERT_EQ(isolationGroup->getNumOfBindPorts(), bindPortCountBeforeFailure);

        simulate_bridge_failure = false; // Reset for other tests

        // Now test that bind works successfully after resetting the failure flag
        // Use a different port to avoid duplicate binding
        Port port2;
        gPortsOrch->getPort("Ethernet5", port2);

        // First bring up Ethernet5 bridge port
        ApplyVlanConfigs();

        // Now bind the second port successfully
        isolationGroup->bind(port2);

        // Verify that the bind port count increased by 1 from the original count
        ASSERT_EQ(isolationGroup->getNumOfBindPorts(), bindPortCountBeforeFailure + 1);        // Clean up - delete the created entry
        auto consumer = dynamic_cast<Consumer *>(gShlOrch->getExecutor(APP_EVPN_SPLIT_HORIZON_TABLE_NAME));
        std::deque<KeyOpFieldsValuesTuple> cleanup_entries;
        cleanup_entries.push_back({"Vlan10:Ethernet4", "DEL", { {} }});
        consumer->addToSync(cleanup_entries);
        static_cast<Orch *>(gShlOrch)->doTask();

        // Verify cleanup
        ASSERT_EQ((gShlOrch->getIsolationGroupCount()), 0);
    }

    TEST_F(ShlOrchTest, ShlMemberFailureTest)
    {
        // Clean up any existing isolation groups first
        auto consumer = dynamic_cast<Consumer *>(gShlOrch->getExecutor(APP_EVPN_SPLIT_HORIZON_TABLE_NAME));

        // COMPLETELY clear the sync queue to avoid interference from previous tests
        consumer->m_toSync.clear();

        // Ensure we start with clean state
        ASSERT_EQ((gShlOrch->getIsolationGroupCount()), 0);

        Table shlTable = Table(m_app_db.get(), APP_EVPN_SPLIT_HORIZON_TABLE_NAME);

        // Create isolation group first successfully
        shlTable.set("Vlan10:Ethernet4", {
            {"vteps", "2.2.2.2"}
        });

        alloc_index = 0;
        oid_values[0] = isolation_group_ids[VTEP_REMOTE_IP_2];

        gShlOrch->addExistingData(&shlTable);
        static_cast<Orch *>(gShlOrch)->doTask();

        auto isolationGroup = gShlOrch->getIsolationGroup(VTEP_REMOTE_IP_2);
        ASSERT_NE(isolationGroup, nullptr);

        // Now simulate member operation failure for NEW operations
        simulate_sai_failure = true;

        Port port;
        gPortsOrch->getPort("Ethernet4", port);

        // Test addMember failure - try to add the same member again (should fail due to SAI error)
        auto result = isolationGroup->addMember(port);
        // Since the member already exists, it should just log debug message and return success
        // Validate that adding an existing member returns success (duplicate member handling)
        ASSERT_EQ(result, SHL_ISO_GRP_STATUS_SUCCESS);
        // Verify that the member count hasn't changed (no duplicate addition)
        ASSERT_EQ(isolationGroup->getNumOfMembers(), 1);

        // But if we simulate SAI failure and try to add a different port member, it should fail
        // Let's try with a different approach - test delMember failure first
        result = isolationGroup->delMember(port);
        ASSERT_EQ(result, SHL_ISO_GRP_STATUS_FAIL);

        // Reset SAI failure and try addMember with a port that has NULL bridge port ID
        simulate_sai_failure = false;

        // Create a port with NULL bridge port ID to test pending member scenario
        Port nullPort;
        nullPort.m_alias = "EthernetNull";
        nullPort.m_bridge_port_id = SAI_NULL_OBJECT_ID;

        // This should add to pending members
        result = isolationGroup->addMember(nullPort);
        ASSERT_EQ(result, SHL_ISO_GRP_STATUS_SUCCESS);
        ASSERT_EQ(isolationGroup->getNumOfPendingMembers(), 1);

        // Now simulate failure and try to delete from pending members
        simulate_sai_failure = true;
        result = isolationGroup->delMember(nullPort);
        ASSERT_EQ(result, SHL_ISO_GRP_STATUS_SUCCESS); // Should succeed since it's just removing from pending

        simulate_sai_failure = false; // Reset for other tests
    }
}
