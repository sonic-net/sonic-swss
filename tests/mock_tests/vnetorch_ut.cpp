/* Pull the standard library in first: the access-specifier override below
 * reaches every header it opens, and libstdc++ internals do not survive it. */
#include <bits/stdc++.h>
#include <unistd.h>

/* vnetorch.h keeps the route tables, the observer table and doRouteTask()
 * private, and Directory::m_values is private too. */
#define private public
#define protected public
#include "directory.h"
#include "vnetorch.h"
#undef protected
#undef private

#include "gtest/gtest.h"
#include "ut_helper.h"
#include "mock_orchagent_main.h"
#include "mock_table.h"

namespace vnetorch_test
{
    using namespace std;

    static const string VNET_NAME = "Vnet_replay";
    static const string TUNNEL_NAME = "tunnel_replay";
    static const string IFNAME = "Ethernet0";
    static const string SUBNET = "10.10.0.8/31";

    /* Both addresses fall inside SUBNET, so the observer walk in delRoute()
     * visits them. */
    static const string OBSERVER_IP = "10.10.0.9";
    static const string STALE_OBSERVER_IP = "10.10.0.8";

    static const sai_object_id_t RIF_OID = 0x6000000000a1b;

    sai_route_api_t *pold_sai_route_api;
    sai_route_api_t ut_sai_route_api;

    int create_route_count;
    int set_route_count;
    int remove_route_count;
    bool route_already_exists;
    sai_object_id_t last_set_nh_id;

    sai_status_t _ut_create_route_entry(
        _In_ const sai_route_entry_t *route_entry,
        _In_ uint32_t attr_count,
        _In_ const sai_attribute_t *attr_list)
    {
        ++create_route_count;
        return route_already_exists ? SAI_STATUS_ITEM_ALREADY_EXISTS : SAI_STATUS_SUCCESS;
    }

    sai_status_t _ut_set_route_entry_attribute(
        _In_ const sai_route_entry_t *route_entry,
        _In_ const sai_attribute_t *attr)
    {
        ++set_route_count;
        if (attr != nullptr && attr->id == SAI_ROUTE_ENTRY_ATTR_NEXT_HOP_ID)
        {
            last_set_nh_id = attr->value.oid;
        }
        return SAI_STATUS_SUCCESS;
    }

    sai_status_t _ut_remove_route_entry(
        _In_ const sai_route_entry_t *route_entry)
    {
        ++remove_route_count;
        return SAI_STATUS_SUCCESS;
    }

    class NullObserver : public Observer
    {
    public:
        void update(SubjectType, void *) override {}
    };

    struct VNetOrchTest : public ::testing::Test
    {
        shared_ptr<swss::DBConnector> m_app_db;
        shared_ptr<swss::DBConnector> m_config_db;
        shared_ptr<swss::DBConnector> m_state_db;
        shared_ptr<swss::DBConnector> m_chassis_app_db;

        VNetOrch *m_vnetOrch = nullptr;
        VNetRouteOrch *m_vnetRouteOrch = nullptr;

        void SetUp() override
        {
            map<string, string> profile = {
                { "SAI_VS_SWITCH_TYPE", "SAI_VS_SWITCH_TYPE_BCM56850" },
                { "KV_DEVICE_MAC_ADDRESS", "20:03:04:05:06:00" }
            };

            ut_helper::initSaiApi(profile);

            pold_sai_route_api = sai_route_api;
            ut_sai_route_api = *sai_route_api;
            sai_route_api = &ut_sai_route_api;
            sai_route_api->create_route_entry = _ut_create_route_entry;
            sai_route_api->set_route_entry_attribute = _ut_set_route_entry_attribute;
            sai_route_api->remove_route_entry = _ut_remove_route_entry;

            create_route_count = 0;
            set_route_count = 0;
            remove_route_count = 0;
            route_already_exists = false;
            last_set_nh_id = SAI_NULL_OBJECT_ID;

            m_app_db = make_shared<swss::DBConnector>("APPL_DB", 0);
            m_config_db = make_shared<swss::DBConnector>("CONFIG_DB", 0);
            m_state_db = make_shared<swss::DBConnector>("STATE_DB", 0);
            m_chassis_app_db = make_shared<swss::DBConnector>("CHASSIS_APP_DB", 0);

            sai_attribute_t attr;

            attr.id = SAI_SWITCH_ATTR_INIT_SWITCH;
            attr.value.booldata = true;
            ASSERT_EQ(sai_switch_api->create_switch(&gSwitchId, 1, &attr), SAI_STATUS_SUCCESS);

            attr.id = SAI_SWITCH_ATTR_SRC_MAC_ADDRESS;
            ASSERT_EQ(sai_switch_api->get_switch_attribute(gSwitchId, 1, &attr), SAI_STATUS_SUCCESS);
            gMacAddress = attr.value.mac;

            attr.id = SAI_SWITCH_ATTR_DEFAULT_VIRTUAL_ROUTER_ID;
            ASSERT_EQ(sai_switch_api->get_switch_attribute(gSwitchId, 1, &attr), SAI_STATUS_SUCCESS);
            gVirtualRouterId = attr.value.oid;

            ASSERT_EQ(gCrmOrch, nullptr);
            gCrmOrch = new CrmOrch(m_config_db.get(), CFG_CRM_TABLE_NAME);

            TableConnector stateDbSwitchTable(m_state_db.get(), "SWITCH_CAPABILITY");
            TableConnector conf_asic_sensors(m_config_db.get(), CFG_ASIC_SENSORS_TABLE_NAME);
            TableConnector app_switch_table(m_app_db.get(), APP_SWITCH_TABLE_NAME);
            vector<TableConnector> switch_tables = { conf_asic_sensors, app_switch_table };

            ASSERT_EQ(gSwitchOrch, nullptr);
            gSwitchOrch = new SwitchOrch(m_app_db.get(), switch_tables, stateDbSwitchTable);

            /* VNetRouteOrch's constructor attaches itself to gBfdOrch. */
            TableConnector stateDbBfdSessionTable(m_state_db.get(), STATE_BFD_SESSION_TABLE_NAME);
            ASSERT_EQ(gBfdOrch, nullptr);
            gBfdOrch = new BfdOrch(m_app_db.get(), APP_BFD_SESSION_TABLE_NAME, stateDbBfdSessionTable);

            const int portsorch_base_pri = 40;
            vector<table_name_with_pri_t> ports_tables = {
                { APP_PORT_TABLE_NAME, portsorch_base_pri + 5 },
                { APP_VLAN_TABLE_NAME, portsorch_base_pri + 2 },
                { APP_VLAN_MEMBER_TABLE_NAME, portsorch_base_pri },
                { APP_LAG_TABLE_NAME, portsorch_base_pri + 4 },
                { APP_LAG_MEMBER_TABLE_NAME, portsorch_base_pri }
            };

            vector<string> flex_counter_tables = { CFG_FLEX_COUNTER_TABLE_NAME };
            gDirectory.set(new FlexCounterOrch(m_config_db.get(), flex_counter_tables));

            ASSERT_EQ(gPortsOrch, nullptr);
            gPortsOrch = new PortsOrch(m_app_db.get(), m_state_db.get(), ports_tables, m_chassis_app_db.get());

            m_vnetOrch = new VNetOrch(m_app_db.get(), APP_VNET_TABLE_NAME);
            gDirectory.set(m_vnetOrch);

            vector<string> cfg_vnet_tables = {
                CFG_VNET_RT_TABLE_NAME,
                CFG_VNET_RT_TUNNEL_TABLE_NAME
            };
            gDirectory.set(new VNetCfgRouteOrch(m_config_db.get(), m_app_db.get(), cfg_vnet_tables));

            vector<string> vnet_tables = {
                APP_VNET_RT_TABLE_NAME,
                APP_VNET_RT_TUNNEL_TABLE_NAME
            };
            m_vnetRouteOrch = new VNetRouteOrch(m_app_db.get(), vnet_tables, m_vnetOrch);
            gDirectory.set(m_vnetRouteOrch);

            ASSERT_EQ(gVrfOrch, nullptr);
            gVrfOrch = new VRFOrch(m_app_db.get(), APP_VRF_TABLE_NAME, m_state_db.get(), STATE_VRF_OBJECT_TABLE_NAME);
            gDirectory.set(gVrfOrch);

            ASSERT_EQ(gIntfsOrch, nullptr);
            gIntfsOrch = new IntfsOrch(m_app_db.get(), APP_INTF_TABLE_NAME, gVrfOrch, m_chassis_app_db.get());

            const int fdborch_pri = 20;
            vector<table_name_with_pri_t> app_fdb_tables = {
                { APP_FDB_TABLE_NAME,       FdbOrch::fdborch_pri },
                { APP_VXLAN_FDB_TABLE_NAME, FdbOrch::fdborch_pri },
                { APP_MCLAG_FDB_TABLE_NAME, fdborch_pri }
            };
            TableConnector stateDbFdb(m_state_db.get(), STATE_FDB_TABLE_NAME);
            TableConnector stateMclagDbFdb(m_state_db.get(), STATE_MCLAG_REMOTE_FDB_TABLE_NAME);
            ASSERT_EQ(gFdbOrch, nullptr);
            gFdbOrch = new FdbOrch(m_app_db.get(), app_fdb_tables, stateDbFdb, stateMclagDbFdb, gPortsOrch,
                                   m_config_db.get());

            ASSERT_EQ(gNeighOrch, nullptr);
            gNeighOrch = new NeighOrch(m_app_db.get(), APP_NEIGH_TABLE_NAME, gIntfsOrch, gFdbOrch, gPortsOrch,
                                       m_chassis_app_db.get());

            vector<string> tunnel_tables = {
                APP_TUNNEL_DECAP_TABLE_NAME,
                APP_TUNNEL_DECAP_TERM_TABLE_NAME
            };
            auto *tunnel_decap_orch = new TunnelDecapOrch(m_app_db.get(), m_state_db.get(), m_config_db.get(), tunnel_tables);
            vector<string> mux_tables = {
                CFG_MUX_CABLE_TABLE_NAME,
                CFG_PEER_SWITCH_TABLE_NAME
            };
            gDirectory.set(new MuxOrch(m_config_db.get(), mux_tables, tunnel_decap_orch, gNeighOrch, gFdbOrch));

            ASSERT_EQ(gFgNhgOrch, nullptr);
            const int fgnhgorch_pri = 15;
            vector<table_name_with_pri_t> fgnhg_tables = {
                { CFG_FG_NHG,        fgnhgorch_pri },
                { CFG_FG_NHG_PREFIX, fgnhgorch_pri },
                { CFG_FG_NHG_MEMBER, fgnhgorch_pri }
            };
            gFgNhgOrch = new FgNhgOrch(m_config_db.get(), m_app_db.get(), m_state_db.get(), fgnhg_tables,
                                       gNeighOrch, gIntfsOrch, gVrfOrch);

            ASSERT_EQ(gSrv6Orch, nullptr);
            TableConnector srv6_sid_list_table(m_app_db.get(), APP_SRV6_SID_LIST_TABLE_NAME);
            TableConnector srv6_my_sid_table(m_app_db.get(), APP_SRV6_MY_SID_TABLE_NAME);
            TableConnector srv6_my_sid_cfg_table(m_config_db.get(), CFG_SRV6_MY_SID_TABLE_NAME);
            vector<TableConnector> srv6_tables = { srv6_sid_list_table, srv6_my_sid_table, srv6_my_sid_cfg_table };
            gSrv6Orch = new Srv6Orch(m_config_db.get(), m_app_db.get(), srv6_tables, gSwitchOrch, gVrfOrch, gNeighOrch);

            static const vector<string> route_pattern_tables = {
                CFG_FLOW_COUNTER_ROUTE_PATTERN_TABLE_NAME,
            };
            gFlowCounterRouteOrch = new FlowCounterRouteOrch(m_config_db.get(), route_pattern_tables);

            ASSERT_EQ(gRouteOrch, nullptr);
            const int routeorch_pri = 5;
            vector<table_name_with_pri_t> route_tables = {
                { APP_ROUTE_TABLE_NAME,       routeorch_pri },
                { APP_LABEL_ROUTE_TABLE_NAME, routeorch_pri }
            };
            gRouteOrch = new RouteOrch(m_app_db.get(), route_tables, gSwitchOrch, gNeighOrch, gIntfsOrch, gVrfOrch,
                                       gFgNhgOrch, gSrv6Orch);
            gNhgOrch = new NhgOrch(m_app_db.get(), APP_NEXTHOP_GROUP_TABLE_NAME);

            vector<string> buffer_tables = { APP_BUFFER_POOL_TABLE_NAME,
                                             APP_BUFFER_PROFILE_TABLE_NAME,
                                             APP_BUFFER_QUEUE_TABLE_NAME,
                                             APP_BUFFER_PG_TABLE_NAME,
                                             APP_BUFFER_PORT_INGRESS_PROFILE_LIST_NAME,
                                             APP_BUFFER_PORT_EGRESS_PROFILE_LIST_NAME };
            gBufferOrch = new BufferOrch(m_app_db.get(), m_config_db.get(), m_state_db.get(), buffer_tables);

            Table portTable(m_app_db.get(), APP_PORT_TABLE_NAME);
            auto ports = ut_helper::getInitialSaiPorts();
            for (const auto &it : ports)
            {
                portTable.set(it.first, it.second);
            }
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

            delete m_vnetRouteOrch;
            m_vnetRouteOrch = nullptr;

            delete m_vnetOrch;
            m_vnetOrch = nullptr;

            delete gBufferOrch;
            gBufferOrch = nullptr;

            delete gNhgOrch;
            gNhgOrch = nullptr;

            delete gRouteOrch;
            gRouteOrch = nullptr;

            delete gFlowCounterRouteOrch;
            gFlowCounterRouteOrch = nullptr;

            delete gSrv6Orch;
            gSrv6Orch = nullptr;

            delete gFgNhgOrch;
            gFgNhgOrch = nullptr;

            delete gNeighOrch;
            gNeighOrch = nullptr;

            delete gFdbOrch;
            gFdbOrch = nullptr;

            delete gIntfsOrch;
            gIntfsOrch = nullptr;

            delete gVrfOrch;
            gVrfOrch = nullptr;

            delete gPortsOrch;
            gPortsOrch = nullptr;

            delete gBfdOrch;
            gBfdOrch = nullptr;

            delete gSwitchOrch;
            gSwitchOrch = nullptr;

            delete gCrmOrch;
            gCrmOrch = nullptr;

            sai_route_api = pold_sai_route_api;
            ut_helper::uninitSaiApi();
        }

        /* Bind Ethernet0 to a VNET the way config_db does. The interface keeps
         * a router interface, so a VNET_ROUTE for its own subnet resolves to a
         * subnet route and reaches add_route(). */
        void bindInterfaceToVnet()
        {
            VNetInfo vnet_info = {
                TUNNEL_NAME,
                static_cast<uint32_t>(5032),
                set<string>(),
                "default",
                false,
                swss::MacAddress()
            };
            vector<sai_attribute_t> attrs;
            m_vnetOrch->vnet_table_[VNET_NAME] =
                VNetObject_T(new VNetVrfObject(VNET_NAME, vnet_info, attrs));

            Port port;
            ASSERT_TRUE(gPortsOrch->getPort(IFNAME, port));
            port.m_rif_id = RIF_OID;
            port.m_vr_id = gVirtualRouterId;
            gPortsOrch->setPort(IFNAME, port);
        }

        /* Bringing the ports up programs routes of its own, so start counting
         * from zero once the fixture is settled. */
        static void resetRouteCounters()
        {
            create_route_count = 0;
            set_route_count = 0;
            remove_route_count = 0;
            last_set_nh_id = SAI_NULL_OBJECT_ID;
        }

        void seedReplayedRoute(NullObserver &observer, IpPrefix &prefix)
        {
            nextHop nh;
            nh.ifname = IFNAME;

            m_vnetRouteOrch->attach(&observer, IpAddress(OBSERVER_IP));
            m_vnetRouteOrch->addRoute(VNET_NAME, prefix, nh);
            ASSERT_EQ(m_vnetRouteOrch->syncd_routes_.count(prefix), 1u);
        }
    };

    /*
     * IntfsOrch owns the connected route for an interface assigned to a VNET.
     * A config reload replays that route to VNetRouteOrch, and SAI answers the
     * CREATE with SAI_STATUS_ITEM_ALREADY_EXISTS. VNetRouteOrch has to converge
     * onto the existing entry with a SET instead of logging a failure.
     */
    TEST_F(VNetOrchTest, ReplayedConnectedRouteConvergesOnExistingEntry)
    {
        bindInterfaceToVnet();

        IpPrefix prefix(SUBNET);
        nextHop nh;
        nh.ifname = IFNAME;
        string op = SET_COMMAND;

        resetRouteCounters();
        route_already_exists = true;

        ASSERT_TRUE(m_vnetRouteOrch->doRouteTask<VNetVrfObject>(VNET_NAME, prefix, nh, op));

        ASSERT_EQ(create_route_count, 1);
        ASSERT_EQ(set_route_count, 1)
            << "replay did not converge onto the route IntfsOrch already programmed";
        ASSERT_EQ(last_set_nh_id, RIF_OID);

        /* Every subsequent replay must behave the same way. */
        ASSERT_TRUE(m_vnetRouteOrch->doRouteTask<VNetVrfObject>(VNET_NAME, prefix, nh, op));
        ASSERT_EQ(create_route_count, 2);
        ASSERT_EQ(set_route_count, 2);
        ASSERT_EQ(remove_route_count, 0);
    }

    /* A route that SAI does not yet hold still takes the plain CREATE path. */
    TEST_F(VNetOrchTest, FirstConnectedRouteIsCreatedNotUpdated)
    {
        bindInterfaceToVnet();

        IpPrefix prefix(SUBNET);
        nextHop nh;
        nh.ifname = IFNAME;
        string op = SET_COMMAND;

        resetRouteCounters();

        ASSERT_TRUE(m_vnetRouteOrch->doRouteTask<VNetVrfObject>(VNET_NAME, prefix, nh, op));
        ASSERT_EQ(create_route_count, 1);
        ASSERT_EQ(set_route_count, 0);
    }

    /*
     * fpmsyncd sends DEL to both VNET route tables when it cannot tell which
     * one held the route, so a repeated delete has to be a no-op rather than an
     * error.
     */
    TEST_F(VNetOrchTest, DuplicateDeleteLeavesRouteTableClean)
    {
        IpPrefix prefix(SUBNET);
        NullObserver observer;
        seedReplayedRoute(observer, prefix);

        m_vnetRouteOrch->delRoute(prefix);
        m_vnetRouteOrch->delRoute(prefix);

        ASSERT_EQ(m_vnetRouteOrch->syncd_routes_.count(prefix), 0u);
    }

    /*
     * An observer can be covered by the prefix while holding no route state for
     * it, which is what a replay leaves behind. The walk has to step past that
     * observer; before the fix it hit `continue` without advancing the iterator
     * and spun forever. Run it in a child process under alarm(2) so a regression
     * surfaces as a failed expectation instead of hanging the suite.
     */
    TEST_F(VNetOrchTest, ReplayedDeleteDoesNotSpinInObserverWalk)
    {
        /* libsaivs keeps three threads alive behind the fixture, and the default
         * death test style forks without exec, which is unsafe when other
         * threads hold locks. Re-exec the child instead. */
        GTEST_FLAG_SET(death_test_style, "threadsafe");

        IpPrefix prefix(SUBNET);
        NullObserver observer;
        seedReplayedRoute(observer, prefix);

        m_vnetRouteOrch->next_hop_observers_[IpAddress(STALE_OBSERVER_IP)]
            .observers.push_back(&observer);

        EXPECT_EXIT(
            {
                alarm(10);
                m_vnetRouteOrch->delRoute(prefix);
                m_vnetRouteOrch->delRoute(prefix);
                _exit(0);
            },
            ::testing::ExitedWithCode(0), "");
    }
}
