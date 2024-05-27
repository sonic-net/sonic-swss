#define private public
#include "directory.h"
#undef private
#define protected public
#include "orch.h"
#undef protected
#include "ut_helper.h"
#include "mock_orchagent_main.h"
#include "gtest/gtest.h"
#include <string>

using namespace std;
using ::testing::Return;
using ::testing::Throw;

namespace mock_orch_test
{
    static const string PEER_SWITCH_HOSTNAME = "peer_hostname";
    static const string PEER_IPV4_ADDRESS = "1.1.1.1";
    static const string ACTIVE_INTERFACE = "Ethernet4";
    static const string STANDBY_INTERFACE = "Ethernet8";
    static const string ACTIVE_STATE = "active";
    static const string STANDBY_STATE = "standby";
    static const string STATE = "state";
    static const string VLAN_1000 = "Vlan1000";
    static const string VLAN_2000 = "Vlan2000";
    static const string SERVER_IP1 = "192.168.0.2";
    static const string SERVER_IP2 = "192.168.0.3";
    static const string MAC1 = "62:f9:65:10:2f:01";
    static const string MAC2 = "62:f9:65:10:2f:02";
    static const string MAC3 = "62:f9:65:10:2f:03";

    class MockOrchTest: public ::testing::Test
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

        virtual void ApplyInitialConfigs() {}

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

        virtual void PostSetUp() {};

        void SetUp() override
        {
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

            m_TunnelDecapOrch = new TunnelDecapOrch(m_app_db.get(), APP_TUNNEL_DECAP_TABLE_NAME);
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

            vector<string> srv6_tables = {
                APP_SRV6_SID_LIST_TABLE_NAME,
                APP_SRV6_MY_SID_TABLE_NAME
            };
            gSrv6Orch = new Srv6Orch(m_app_db.get(), srv6_tables, gSwitchOrch, gVrfOrch, gNeighOrch);
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

            ApplyInitialConfigs();
            PostSetUp();
        }

        virtual void PreTearDown() {};

        void TearDown() override
        {
            PreTearDown();
            for (std::vector<Orch **>::reverse_iterator rit = ut_orch_list.rbegin(); rit != ut_orch_list.rend(); ++rit)
            {
                Orch **orch = *rit;
                delete *orch;
                *orch = nullptr;
            }

            gDirectory.m_values.clear();

            ut_helper::uninitSaiApi();
        }
    };
}
