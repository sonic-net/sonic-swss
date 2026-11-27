#define private public
#include "directory.h"
#undef private
#define protected public
#include "orch.h"
#undef protected

#include "ut_helper.h"
#include "mock_orchagent_main.h"
#include "mock_table.h"

#include "gtest/gtest.h"
#include "isolationgrouporch.h"
#include "vxlanorch.h"

extern IsoGrpOrch *gIsoGrpOrch;
extern EvpnMhOrch *gEvpnMhOrch;
extern sai_isolation_group_api_t *sai_isolation_group_api;

namespace isolationgrouporch_test
{
    using namespace std;

    static const string PEER_LINK_PORT = "Ethernet0";
    static const string MCLAG_MEMBER_PORT = "Ethernet4";
    static const string VLAN_NAME_1 = "Vlan10";
    static const string VTEP_LOCAL_IP = "1.1.1.1";
    static const string VTEP_REMOTE_IP_1 = "2.2.2.2";
    static const string VTEP_REMOTE_IP_2 = "3.3.3.3";

    sai_isolation_group_api_t ut_sai_isolation_group_api, *pold_sai_isolation_group_api;
    sai_bridge_api_t ut_sai_bridge_api, *pold_sai_bridge_api;

    sai_status_t tunnel_member_status = SAI_STATUS_SUCCESS;
    uint64_t next_iso_oid = 1;
    vector<sai_object_id_t> removed_members;
    vector<pair<sai_object_id_t, sai_object_id_t>> bound_bridge_ports;
    vector<sai_object_id_t> removed_bridge_ports;
    vector<string> sai_call_order;

    static string evpnTunnelAlias(const string &vtep_ip)
    {
        return string(EVPN_TUNNEL_PORT_PREFIX) + vtep_ip;
    }

    static sai_object_id_t addTunnelBridgePort(const string &alias)
    {
        vector<sai_attribute_t> tunnel_attrs(2);
        tunnel_attrs[0].id = SAI_TUNNEL_ATTR_TYPE;
        tunnel_attrs[0].value.s32 = SAI_TUNNEL_TYPE_VXLAN;
        tunnel_attrs[1].id = SAI_TUNNEL_ATTR_UNDERLAY_INTERFACE;
        tunnel_attrs[1].value.oid = gUnderlayIfId;

        sai_object_id_t tunnel_oid = SAI_NULL_OBJECT_ID;
        EXPECT_EQ(SAI_STATUS_SUCCESS,
                  sai_tunnel_api->create_tunnel(&tunnel_oid, gSwitchId,
                                                static_cast<uint32_t>(tunnel_attrs.size()),
                                                tunnel_attrs.data()));
        EXPECT_TRUE(gPortsOrch->addTunnel(alias, tunnel_oid, false));

        Port tunnel_port;
        EXPECT_TRUE(gPortsOrch->getPort(alias, tunnel_port));
        EXPECT_TRUE(gPortsOrch->addBridgePort(tunnel_port));
        return tunnel_oid;
    }

    sai_status_t _ut_create_isolation_group(
        _Out_ sai_object_id_t *grp_id,
        _In_ sai_object_id_t switch_id,
        _In_ uint32_t attr_count,
        _In_ const sai_attribute_t *attr_list)
    {
        *grp_id = static_cast<sai_object_id_t>(0x4b000000000000ULL + next_iso_oid++);
        return SAI_STATUS_SUCCESS;
    }

    sai_status_t _ut_remove_isolation_group(
        _In_ sai_object_id_t grp_id)
    {
        return SAI_STATUS_SUCCESS;
    }

    sai_status_t _ut_create_isolation_group_member(
        _Out_ sai_object_id_t *grp_member_id,
        _In_ sai_object_id_t switch_id,
        _In_ uint32_t attr_count,
        _In_ const sai_attribute_t *attr_list)
    {
        if (tunnel_member_status != SAI_STATUS_SUCCESS)
        {
            Port tunnel_port;
            if (gPortsOrch->getPort(evpnTunnelAlias(VTEP_REMOTE_IP_1), tunnel_port))
            {
                for (uint32_t i = 0; i < attr_count; ++i)
                {
                    if (attr_list[i].id == SAI_ISOLATION_GROUP_MEMBER_ATTR_ISOLATION_OBJECT &&
                        attr_list[i].value.oid == tunnel_port.m_bridge_port_id)
                    {
                        return tunnel_member_status;
                    }
                }
            }
        }
        *grp_member_id = static_cast<sai_object_id_t>(0x5c000000000000ULL + next_iso_oid++);
        return SAI_STATUS_SUCCESS;
    }

    sai_status_t _ut_remove_isolation_group_member(
        _In_ sai_object_id_t grp_member_id)
    {
        removed_members.push_back(grp_member_id);
        sai_call_order.push_back("remove_isolation_group_member");
        return SAI_STATUS_SUCCESS;
    }

    sai_status_t _ut_set_bridge_port_attribute(
        _In_ sai_object_id_t bridge_port_id,
        _In_ const sai_attribute_t *attr)
    {
        if (attr->id == SAI_BRIDGE_PORT_ATTR_ISOLATION_GROUP)
        {
            bound_bridge_ports.emplace_back(bridge_port_id, attr->value.oid);
            return SAI_STATUS_SUCCESS;
        }
        return pold_sai_bridge_api->set_bridge_port_attribute(bridge_port_id, attr);
    }

    sai_status_t _ut_remove_bridge_port(
        _In_ sai_object_id_t bridge_port_id)
    {
        removed_bridge_ports.push_back(bridge_port_id);
        sai_call_order.push_back("remove_bridge_port");
        return pold_sai_bridge_api->remove_bridge_port(bridge_port_id);
    }

    struct IsoGrpOrchTest : public ::testing::Test
    {
    protected:
        vector<Orch **> ut_orch_list;
        shared_ptr<swss::DBConnector> m_app_db;
        shared_ptr<swss::DBConnector> m_config_db;
        shared_ptr<swss::DBConnector> m_state_db;
        shared_ptr<swss::DBConnector> m_chassis_app_db;
        FlexCounterOrch *m_FlexCounterOrch = nullptr;
        TunnelDecapOrch *m_TunnelDecapOrch = nullptr;
        VxlanTunnelOrch *m_VxlanTunnelOrch = nullptr;
        EvpnNvoOrch *m_EvpnNvoOrch = nullptr;

        void SetUp() override
        {
            tunnel_member_status = SAI_STATUS_SUCCESS;
            next_iso_oid = 1;
            removed_members.clear();
            bound_bridge_ports.clear();
            removed_bridge_ports.clear();
            sai_call_order.clear();

            map<string, string> profile = {
                { "SAI_VS_SWITCH_TYPE", "SAI_VS_SWITCH_TYPE_BCM56850" },
                { "KV_DEVICE_MAC_ADDRESS", "20:03:04:05:06:00" }
            };

            ut_helper::initSaiApi(profile);
            m_app_db = make_shared<swss::DBConnector>("APPL_DB", 0);
            m_config_db = make_shared<swss::DBConnector>("CONFIG_DB", 0);
            m_state_db = make_shared<swss::DBConnector>("STATE_DB", 0);
            m_chassis_app_db = make_shared<swss::DBConnector>("CHASSIS_APP_DB", 0);

            sai_attribute_t attr;
            attr.id = SAI_SWITCH_ATTR_INIT_SWITCH;
            attr.value.booldata = true;
            ASSERT_EQ(SAI_STATUS_SUCCESS, sai_switch_api->create_switch(&gSwitchId, 1, &attr));

            attr.id = SAI_SWITCH_ATTR_SRC_MAC_ADDRESS;
            ASSERT_EQ(SAI_STATUS_SUCCESS, sai_switch_api->get_switch_attribute(gSwitchId, 1, &attr));
            gMacAddress = attr.value.mac;

            attr.id = SAI_SWITCH_ATTR_DEFAULT_VIRTUAL_ROUTER_ID;
            ASSERT_EQ(SAI_STATUS_SUCCESS, sai_switch_api->get_switch_attribute(gSwitchId, 1, &attr));
            gVirtualRouterId = attr.value.oid;

            vector<sai_attribute_t> underlay_attrs(3);
            underlay_attrs[0].id = SAI_ROUTER_INTERFACE_ATTR_VIRTUAL_ROUTER_ID;
            underlay_attrs[0].value.oid = gVirtualRouterId;
            underlay_attrs[1].id = SAI_ROUTER_INTERFACE_ATTR_TYPE;
            underlay_attrs[1].value.s32 = SAI_ROUTER_INTERFACE_TYPE_LOOPBACK;
            underlay_attrs[2].id = SAI_ROUTER_INTERFACE_ATTR_MTU;
            underlay_attrs[2].value.u32 = 9100;
            ASSERT_EQ(SAI_STATUS_SUCCESS,
                      sai_router_intfs_api->create_router_interface(&gUnderlayIfId, gSwitchId, 3, underlay_attrs.data()));

            const int portsorch_base_pri = 40;
            vector<table_name_with_pri_t> ports_tables = {
                { APP_PORT_TABLE_NAME, portsorch_base_pri + 5 },
                { APP_VLAN_TABLE_NAME, portsorch_base_pri + 2 },
                { APP_VLAN_MEMBER_TABLE_NAME, portsorch_base_pri },
                { APP_LAG_TABLE_NAME, portsorch_base_pri + 4 },
                { APP_LAG_MEMBER_TABLE_NAME, portsorch_base_pri }
            };

            vector<string> flex_counter_tables = { CFG_FLEX_COUNTER_TABLE_NAME };
            m_FlexCounterOrch = new FlexCounterOrch(m_config_db.get(), flex_counter_tables);
            gDirectory.set(m_FlexCounterOrch);
            ut_orch_list.push_back((Orch **)&m_FlexCounterOrch);

            static const vector<string> route_pattern_tables = { CFG_FLOW_COUNTER_ROUTE_PATTERN_TABLE_NAME };
            gFlowCounterRouteOrch = new FlowCounterRouteOrch(m_config_db.get(), route_pattern_tables);
            gDirectory.set(gFlowCounterRouteOrch);
            ut_orch_list.push_back((Orch **)&gFlowCounterRouteOrch);

            gVrfOrch = new VRFOrch(m_app_db.get(), APP_VRF_TABLE_NAME, m_state_db.get(), STATE_VRF_OBJECT_TABLE_NAME);
            gDirectory.set(gVrfOrch);
            ut_orch_list.push_back((Orch **)&gVrfOrch);

            vector<table_name_with_pri_t> intf_tables = { { APP_INTF_TABLE_NAME, IntfsOrch::intfsorch_pri } };
            gIntfsOrch = new IntfsOrch(m_app_db.get(), intf_tables, gVrfOrch, m_chassis_app_db.get());
            gDirectory.set(gIntfsOrch);
            ut_orch_list.push_back((Orch **)&gIntfsOrch);

            TableConnector stateDbSwitchTable(m_state_db.get(), "SWITCH_CAPABILITY");
            vector<TableConnector> switch_tables = {
                TableConnector(m_config_db.get(), CFG_ASIC_SENSORS_TABLE_NAME),
                TableConnector(m_app_db.get(), APP_SWITCH_TABLE_NAME)
            };
            gSwitchOrch = new SwitchOrch(m_app_db.get(), switch_tables, stateDbSwitchTable);
            gDirectory.set(gSwitchOrch);
            ut_orch_list.push_back((Orch **)&gSwitchOrch);

            gPortsOrch = new PortsOrch(m_app_db.get(), m_state_db.get(), ports_tables, m_chassis_app_db.get());
            gDirectory.set(gPortsOrch);
            ut_orch_list.push_back((Orch **)&gPortsOrch);

            vector<TableConnector> evpn_df_es_tables = {
                TableConnector(m_app_db.get(), "EVPN_DF_TABLE"),
                TableConnector(m_config_db.get(), "EVPN_ETHERNET_SEGMENT")
            };
            gEvpnMhOrch = new EvpnMhOrch(evpn_df_es_tables);
            gDirectory.set(gEvpnMhOrch);
            ut_orch_list.push_back((Orch **)&gEvpnMhOrch);

            const int fgnhgorch_pri = 15;
            vector<table_name_with_pri_t> fgnhg_tables = {
                { CFG_FG_NHG, fgnhgorch_pri },
                { CFG_FG_NHG_PREFIX, fgnhgorch_pri },
                { CFG_FG_NHG_MEMBER, fgnhgorch_pri }
            };
            gFgNhgOrch = new FgNhgOrch(m_config_db.get(), m_app_db.get(), m_state_db.get(), fgnhg_tables, gNeighOrch, gIntfsOrch, gVrfOrch);
            gDirectory.set(gFgNhgOrch);
            ut_orch_list.push_back((Orch **)&gFgNhgOrch);

            vector<table_name_with_pri_t> app_fdb_tables = {
                { APP_FDB_TABLE_NAME, FdbOrch::fdborch_pri },
                { APP_VXLAN_FDB_TABLE_NAME, FdbOrch::fdborch_pri },
                { APP_MCLAG_FDB_TABLE_NAME, 20 }
            };
            gFdbOrch = new FdbOrch(m_app_db.get(), app_fdb_tables,
                                   TableConnector(m_state_db.get(), STATE_FDB_TABLE_NAME),
                                   TableConnector(m_state_db.get(), STATE_MCLAG_REMOTE_FDB_TABLE_NAME),
                                   gPortsOrch, m_config_db.get());
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
            gPolicerOrch = new PolicerOrch(policer_tables, gPortsOrch);
            gDirectory.set(gPolicerOrch);
            ut_orch_list.push_back((Orch **)&gPolicerOrch);

            gNhgOrch = new NhgOrch(m_app_db.get(), APP_NEXTHOP_GROUP_TABLE_NAME);
            gDirectory.set(gNhgOrch);
            ut_orch_list.push_back((Orch **)&gNhgOrch);

            vector<TableConnector> srv6_tables = {
                TableConnector(m_app_db.get(), APP_SRV6_SID_LIST_TABLE_NAME),
                TableConnector(m_app_db.get(), APP_SRV6_MY_SID_TABLE_NAME)
            };
            gSrv6Orch = new Srv6Orch(m_config_db.get(), m_app_db.get(), srv6_tables, gSwitchOrch, gVrfOrch, gNeighOrch);
            gDirectory.set(gSrv6Orch);
            ut_orch_list.push_back((Orch **)&gSrv6Orch);

            gCrmOrch = new CrmOrch(m_config_db.get(), CFG_CRM_TABLE_NAME);
            gDirectory.set(gCrmOrch);
            ut_orch_list.push_back((Orch **)&gCrmOrch);

            vector<table_name_with_pri_t> route_tables = {
                { APP_ROUTE_TABLE_NAME, 5 },
                { APP_LABEL_ROUTE_TABLE_NAME, 5 }
            };
            gRouteOrch = new RouteOrch(m_app_db.get(), route_tables, gSwitchOrch, gNeighOrch, gIntfsOrch, gVrfOrch, gFgNhgOrch, gSrv6Orch);
            gDirectory.set(gRouteOrch);
            ut_orch_list.push_back((Orch **)&gRouteOrch);

            TableConnector stateDbMirrorSession(m_state_db.get(), STATE_MIRROR_SESSION_TABLE_NAME);
            TableConnector confDbMirrorSession(m_config_db.get(), CFG_MIRROR_SESSION_TABLE_NAME);
            gMirrorOrch = new MirrorOrch(stateDbMirrorSession, confDbMirrorSession, gPortsOrch, gRouteOrch, gNeighOrch, gFdbOrch, gPolicerOrch, gSwitchOrch);
            gDirectory.set(gMirrorOrch);
            ut_orch_list.push_back((Orch **)&gMirrorOrch);

            vector<TableConnector> acl_table_connectors = {
                TableConnector(m_config_db.get(), CFG_ACL_TABLE_TYPE_TABLE_NAME),
                TableConnector(m_config_db.get(), CFG_ACL_TABLE_TABLE_NAME),
                TableConnector(m_config_db.get(), CFG_ACL_RULE_TABLE_NAME),
                TableConnector(m_app_db.get(), APP_ACL_TABLE_TABLE_NAME),
                TableConnector(m_app_db.get(), APP_ACL_RULE_TABLE_NAME),
                TableConnector(m_app_db.get(), APP_ACL_TABLE_TYPE_TABLE_NAME)
            };
            gAclOrch = new AclOrch(acl_table_connectors, m_state_db.get(),
                                   gSwitchOrch, gPortsOrch, gMirrorOrch, gNeighOrch, gRouteOrch, NULL);
            gDirectory.set(gAclOrch);
            ut_orch_list.push_back((Orch **)&gAclOrch);

            vector<string> stp_tables = {
                "STP_TABLE",
                "STP_VLAN_INSTANCE_TABLE",
                "STP_PORT_TABLE",
                "STP_PORT_STATE_TABLE",
                "STP_FASTAGEING_FLUSH_TABLE",
                "STP_INST_PORT_FLUSH_TABLE"
            };
            gStpOrch = new StpOrch(m_app_db.get(), m_state_db.get(), stp_tables);
            gDirectory.set(gStpOrch);
            ut_orch_list.push_back((Orch **)&gStpOrch);

            m_VxlanTunnelOrch = new VxlanTunnelOrch(m_state_db.get(), m_app_db.get(), APP_VXLAN_TUNNEL_TABLE_NAME);
            gDirectory.set(m_VxlanTunnelOrch);
            ut_orch_list.push_back((Orch **)&m_VxlanTunnelOrch);

            m_EvpnNvoOrch = new EvpnNvoOrch(m_app_db.get(), APP_VXLAN_EVPN_NVO_TABLE_NAME);
            gDirectory.set(m_EvpnNvoOrch);
            ut_orch_list.push_back((Orch **)&m_EvpnNvoOrch);

            Table port_table(m_app_db.get(), APP_PORT_TABLE_NAME);
            Table vlan_table(m_app_db.get(), APP_VLAN_TABLE_NAME);
            Table vlan_member_table(m_app_db.get(), APP_VLAN_MEMBER_TABLE_NAME);

            auto ports = ut_helper::getInitialSaiPorts();
            port_table.set(PEER_LINK_PORT, ports[PEER_LINK_PORT]);
            port_table.set(MCLAG_MEMBER_PORT, ports[MCLAG_MEMBER_PORT]);
            port_table.set("PortConfigDone", { { "count", "2" } });
            port_table.set("PortInitDone", { {} });

            vlan_table.set(VLAN_NAME_1, { { "admin_status", "up" }, { "mtu", "9100" }, { "mac", "00:aa:bb:cc:dd:ee" } });
            vlan_member_table.set(VLAN_NAME_1 + ":" + PEER_LINK_PORT, { { "tagging_mode", "tagged" } });
            vlan_member_table.set(VLAN_NAME_1 + ":" + MCLAG_MEMBER_PORT, { { "tagging_mode", "tagged" } });

            gPortsOrch->addExistingData(&port_table);
            gPortsOrch->addExistingData(&vlan_table);
            gPortsOrch->addExistingData(&vlan_member_table);
            static_cast<Orch *>(gPortsOrch)->doTask();

            vector<TableConnector> iso_tbl_ctrs = { TableConnector(m_app_db.get(), APP_ISOLATION_GROUP_TABLE_NAME) };
            gIsoGrpOrch = new IsoGrpOrch(iso_tbl_ctrs);
            gDirectory.set(gIsoGrpOrch);
            ut_orch_list.push_back((Orch **)&gIsoGrpOrch);

            pold_sai_isolation_group_api = sai_isolation_group_api;
            ut_sai_isolation_group_api = {};
            ut_sai_isolation_group_api.create_isolation_group = _ut_create_isolation_group;
            ut_sai_isolation_group_api.remove_isolation_group = _ut_remove_isolation_group;
            ut_sai_isolation_group_api.create_isolation_group_member = _ut_create_isolation_group_member;
            ut_sai_isolation_group_api.remove_isolation_group_member = _ut_remove_isolation_group_member;
            sai_isolation_group_api = &ut_sai_isolation_group_api;

            pold_sai_bridge_api = sai_bridge_api;
            ut_sai_bridge_api = *pold_sai_bridge_api;
            ut_sai_bridge_api.set_bridge_port_attribute = _ut_set_bridge_port_attribute;
            ut_sai_bridge_api.remove_bridge_port = _ut_remove_bridge_port;
            sai_bridge_api = &ut_sai_bridge_api;
        }

        void TearDown() override
        {
            for (auto rit = ut_orch_list.rbegin(); rit != ut_orch_list.rend(); ++rit)
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

    TEST_F(IsoGrpOrchTest, MclagIsolationGroupIncludesEvpnTunnelBridgePortsAndCleansUpBeforeRemove)
    {
        addTunnelBridgePort(string(LOCAL_TUNNEL_PORT_PREFIX) + VTEP_LOCAL_IP);
        addTunnelBridgePort(evpnTunnelAlias(VTEP_REMOTE_IP_1));

        auto consumer = dynamic_cast<Consumer *>(gIsoGrpOrch->getExecutor(APP_ISOLATION_GROUP_TABLE_NAME));
        deque<KeyOpFieldsValuesTuple> entries = {
            { "MCLAG_ISO_GRP", "SET", {
                { "DESCRIPTION", "Isolation group for MCLAG" },
                { "TYPE", "bridge-port" },
                { "PORTS", PEER_LINK_PORT },
                { "MEMBERS", MCLAG_MEMBER_PORT }
            } }
        };
        consumer->addToSync(entries);
        static_cast<Orch *>(gIsoGrpOrch)->doTask();

        auto grp = gIsoGrpOrch->getIsolationGroup("MCLAG_ISO_GRP");
        ASSERT_NE(grp, nullptr);
        // Ethernet4 and Port_EVPN_2.2.2.2 are members; Port_SRC_VTEP_1.1.1.1 is excluded
        EXPECT_EQ(2u, grp->getNumOfMembers());
        EXPECT_EQ(1u, grp->getNumOfBindPorts());

        // Dynamic EVPN tunnel added later joins MCLAG_ISO_GRP automatically
        addTunnelBridgePort(evpnTunnelAlias(VTEP_REMOTE_IP_2));
        EXPECT_EQ(3u, grp->getNumOfMembers());

        // Empty MEMBERS update while ICCP stays up retains EVPN tunnel members
        entries = {
            { "MCLAG_ISO_GRP", "SET", {
                { "DESCRIPTION", "Isolation group for MCLAG" },
                { "TYPE", "bridge-port" },
                { "PORTS", PEER_LINK_PORT },
                { "MEMBERS", "" }
            } }
        };
        consumer->addToSync(entries);
        static_cast<Orch *>(gIsoGrpOrch)->doTask();
        EXPECT_EQ(2u, grp->getNumOfMembers());

        // Removing a dynamic tunnel bridge port detaches its isolation member
        // before remove_bridge_port() and does not leak into m_pending_members
        Port tunnel2;
        ASSERT_TRUE(gPortsOrch->getPort(evpnTunnelAlias(VTEP_REMOTE_IP_2), tunnel2));
        sai_object_id_t tunnel2_bp = tunnel2.m_bridge_port_id;
        removed_members.clear();
        removed_bridge_ports.clear();
        sai_call_order.clear();
        ASSERT_TRUE(gPortsOrch->removeBridgePort(tunnel2));
        EXPECT_EQ(1u, removed_members.size());
        ASSERT_EQ(1u, removed_bridge_ports.size());
        EXPECT_EQ(tunnel2_bp, removed_bridge_ports[0]);
        EXPECT_EQ((vector<string>{ "remove_isolation_group_member", "remove_bridge_port" }), sai_call_order);
        EXPECT_EQ(1u, grp->getNumOfMembers());
        EXPECT_EQ(0u, grp->getNumOfPendingMembers());
    }

    TEST_F(IsoGrpOrchTest, MclagIsolationGroupSkipsUnsupportedTunnelMemberGracefully)
    {
        tunnel_member_status = SAI_STATUS_NOT_SUPPORTED;
        addTunnelBridgePort(evpnTunnelAlias(VTEP_REMOTE_IP_1));

        auto consumer = dynamic_cast<Consumer *>(gIsoGrpOrch->getExecutor(APP_ISOLATION_GROUP_TABLE_NAME));
        deque<KeyOpFieldsValuesTuple> entries = {
            { "MCLAG_ISO_GRP", "SET", {
                { "DESCRIPTION", "Isolation group for MCLAG" },
                { "TYPE", "bridge-port" },
                { "PORTS", PEER_LINK_PORT },
                { "MEMBERS", MCLAG_MEMBER_PORT }
            } }
        };
        consumer->addToSync(entries);
        static_cast<Orch *>(gIsoGrpOrch)->doTask();

        auto grp = gIsoGrpOrch->getIsolationGroup("MCLAG_ISO_GRP");
        ASSERT_NE(grp, nullptr);
        EXPECT_EQ(1u, grp->getNumOfMembers());
    }
}
