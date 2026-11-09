// Tests for the PFCWD ACL create-failure handling in PfcWdAclHandler and the
// m_rolledBack/isValid() consumption in PfcWdSwOrch::startWdActionOnQueue.
//
// Expose the internals under test (m_rolledBack, m_aclTables, PfcWdSwOrch
// entry map) the same way portal.h exposes AclOrch internals.
// Pull in std/system headers before the access-override block below; defining
// private/public around libstdc++ headers breaks them (e.g. <sstream>).
#include "ut_helper.h"
#include "mock_orchagent_main.h"
#include "mock_table.h"

#define private public
#define protected public
#include "pfcactionhandler.h"
#include "pfcwdsworch.h"
#undef protected
#undef private

extern sai_object_id_t gSwitchId;
extern sai_acl_api_t *sai_acl_api;
extern sai_switch_api_t *sai_switch_api;

extern SwitchOrch *gSwitchOrch;
extern CrmOrch *gCrmOrch;
extern PortsOrch *gPortsOrch;
extern RouteOrch *gRouteOrch;
extern FlowCounterRouteOrch *gFlowCounterRouteOrch;
extern IntfsOrch *gIntfsOrch;
extern NeighOrch *gNeighOrch;
extern FgNhgOrch *gFgNhgOrch;
extern Srv6Orch *gSrv6Orch;
extern FdbOrch *gFdbOrch;
extern MirrorOrch *gMirrorOrch;
extern PolicerOrch *gPolicerOrch;
extern VRFOrch *gVrfOrch;
extern QosOrch *gQosOrch;
extern BufferOrch *gBufferOrch;
extern AclOrch *gAclOrch;
extern Directory<Orch*> gDirectory;

namespace pfcactionhandler_test
{
    using namespace std;

    // Restores an environment variable on scope exit.
    struct EnvGuard
    {
        string m_name;
        string m_oldValue;
        bool m_wasSet;

        EnvGuard(const string &name, const string &value) : m_name(name)
        {
            const char *old = getenv(name.c_str());
            m_wasSet = (old != nullptr);
            if (m_wasSet)
            {
                m_oldValue = old;
            }
            setenv(name.c_str(), value.c_str(), 1);
        }

        ~EnvGuard()
        {
            if (m_wasSet)
            {
                setenv(m_name.c_str(), m_oldValue.c_str(), 1);
            }
            else
            {
                unsetenv(m_name.c_str());
            }
        }
    };

    // Injects SAI create failures for ACL tables/entries. failTableAt/failEntryAt
    // are 1-based call indexes (0 = never fail). All other calls go to the real
    // (virtual switch) implementation.
    struct AclCreateFailureInjector
    {
        using create_fn = sai_status_t (*)(sai_object_id_t *, sai_object_id_t,
                                           uint32_t, const sai_attribute_t *);

        create_fn m_origCreateTable;
        create_fn m_origCreateEntry;
        int m_tableCalls = 0;
        int m_entryCalls = 0;
        int m_failTableAt = 0;
        int m_failEntryAt = 0;

        shared_ptr<SaiSpyFunctor<SAI_API_ACL, SAI_OBJECT_TYPE_ACL_TABLE, sai_status_t,
            sai_object_id_t *, sai_object_id_t, uint32_t, const sai_attribute_t *>> m_tableSpy;
        shared_ptr<SaiSpyFunctor<SAI_API_ACL, SAI_OBJECT_TYPE_ACL_ENTRY, sai_status_t,
            sai_object_id_t *, sai_object_id_t, uint32_t, const sai_attribute_t *>> m_entrySpy;

        AclCreateFailureInjector()
        {
            m_origCreateTable = sai_acl_api->create_acl_table;
            m_origCreateEntry = sai_acl_api->create_acl_entry;

            m_tableSpy = SpyOn<SAI_API_ACL, SAI_OBJECT_TYPE_ACL_TABLE>(&sai_acl_api->create_acl_table);
            m_tableSpy->callFake([this](sai_object_id_t *oid, sai_object_id_t sw,
                                        uint32_t count, const sai_attribute_t *attrs) -> sai_status_t {
                ++m_tableCalls;
                if (m_failTableAt != 0 && m_tableCalls == m_failTableAt)
                {
                    return SAI_STATUS_INSUFFICIENT_RESOURCES;
                }
                return m_origCreateTable(oid, sw, count, attrs);
            });

            m_entrySpy = SpyOn<SAI_API_ACL, SAI_OBJECT_TYPE_ACL_ENTRY>(&sai_acl_api->create_acl_entry);
            m_entrySpy->callFake([this](sai_object_id_t *oid, sai_object_id_t sw,
                                        uint32_t count, const sai_attribute_t *attrs) -> sai_status_t {
                ++m_entryCalls;
                if (m_failEntryAt != 0 && m_entryCalls == m_failEntryAt)
                {
                    return SAI_STATUS_INSUFFICIENT_RESOURCES;
                }
                return m_origCreateEntry(oid, sw, count, attrs);
            });
        }
    };

    struct PfcActionHandlerTest : public ::testing::Test
    {
        shared_ptr<swss::DBConnector> m_app_db;
        shared_ptr<swss::DBConnector> m_config_db;
        shared_ptr<swss::DBConnector> m_state_db;
        shared_ptr<swss::DBConnector> m_counters_db;
        shared_ptr<swss::DBConnector> m_chassis_app_db;
        shared_ptr<Table> m_countersTable;

        void SetUp() override
        {
            ::testing_db::reset();

            m_app_db = make_shared<swss::DBConnector>("APPL_DB", 0);
            m_config_db = make_shared<swss::DBConnector>("CONFIG_DB", 0);
            m_state_db = make_shared<swss::DBConnector>("STATE_DB", 0);
            m_counters_db = make_shared<swss::DBConnector>("COUNTERS_DB", 0);
            m_countersTable = make_shared<Table>(m_counters_db.get(), "COUNTERS");

            map<string, string> profile = {
                { "SAI_VS_SWITCH_TYPE", "SAI_VS_SWITCH_TYPE_BCM56850" },
                { "KV_DEVICE_MAC_ADDRESS", "20:03:04:05:06:00" }
            };

            auto status = ut_helper::initSaiApi(profile);
            ASSERT_EQ(status, SAI_STATUS_SUCCESS);

            sai_attribute_t attr;
            attr.id = SAI_SWITCH_ATTR_INIT_SWITCH;
            attr.value.booldata = true;
            status = sai_switch_api->create_switch(&gSwitchId, 1, &attr);
            ASSERT_EQ(status, SAI_STATUS_SUCCESS);

            attr.id = SAI_SWITCH_ATTR_SRC_MAC_ADDRESS;
            status = sai_switch_api->get_switch_attribute(gSwitchId, 1, &attr);
            ASSERT_EQ(status, SAI_STATUS_SUCCESS);
            gMacAddress = attr.value.mac;

            attr.id = SAI_SWITCH_ATTR_DEFAULT_VIRTUAL_ROUTER_ID;
            status = sai_switch_api->get_switch_attribute(gSwitchId, 1, &attr);
            ASSERT_EQ(status, SAI_STATUS_SUCCESS);
            gVirtualRouterId = attr.value.oid;

            // Orch dependency chain (same as aclorch_ut plus the pieces
            // portsorch_ut/qosorch_ut need for real port creation).
            ASSERT_EQ(gCrmOrch, nullptr);
            gCrmOrch = new CrmOrch(m_config_db.get(), CFG_CRM_TABLE_NAME);

            TableConnector stateDbSwitchTable(m_state_db.get(), "SWITCH_CAPABILITY");
            TableConnector conf_asic_sensors(m_config_db.get(), CFG_ASIC_SENSORS_TABLE_NAME);
            TableConnector app_switch_table(m_app_db.get(), APP_SWITCH_TABLE_NAME);
            vector<TableConnector> switch_tables = { conf_asic_sensors, app_switch_table };

            ASSERT_EQ(gSwitchOrch, nullptr);
            gSwitchOrch = new SwitchOrch(m_app_db.get(), switch_tables, stateDbSwitchTable);

            vector<string> flex_counter_tables = { CFG_FLEX_COUNTER_TABLE_NAME };
            auto* flexCounterOrch = new FlexCounterOrch(m_config_db.get(), flex_counter_tables);
            gDirectory.set(flexCounterOrch);

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

            vector<string> buffer_tables = { APP_BUFFER_POOL_TABLE_NAME,
                                             APP_BUFFER_PROFILE_TABLE_NAME,
                                             APP_BUFFER_QUEUE_TABLE_NAME,
                                             APP_BUFFER_PG_TABLE_NAME,
                                             APP_BUFFER_PORT_INGRESS_PROFILE_LIST_NAME,
                                             APP_BUFFER_PORT_EGRESS_PROFILE_LIST_NAME };
            ASSERT_EQ(gBufferOrch, nullptr);
            gBufferOrch = new BufferOrch(m_app_db.get(), m_config_db.get(), m_state_db.get(), buffer_tables);

            vector<string> qos_tables = {
                CFG_TC_TO_QUEUE_MAP_TABLE_NAME,
                CFG_SCHEDULER_TABLE_NAME,
                CFG_DSCP_TO_TC_MAP_TABLE_NAME,
                CFG_MPLS_TC_TO_TC_MAP_TABLE_NAME,
                CFG_DOT1P_TO_TC_MAP_TABLE_NAME,
                CFG_QUEUE_TABLE_NAME,
                CFG_PORT_QOS_MAP_TABLE_NAME,
                CFG_WRED_PROFILE_TABLE_NAME,
                CFG_TC_TO_PRIORITY_GROUP_MAP_TABLE_NAME,
                CFG_PFC_PRIORITY_TO_PRIORITY_GROUP_MAP_TABLE_NAME,
                CFG_PFC_PRIORITY_TO_QUEUE_MAP_TABLE_NAME,
                CFG_DSCP_TO_FC_MAP_TABLE_NAME,
                CFG_EXP_TO_FC_MAP_TABLE_NAME,
                CFG_TC_TO_DSCP_MAP_TABLE_NAME
            };
            ASSERT_EQ(gQosOrch, nullptr);
            gQosOrch = new QosOrch(m_config_db.get(), qos_tables);

            static const vector<string> route_pattern_tables = {
                CFG_FLOW_COUNTER_ROUTE_PATTERN_TABLE_NAME,
            };
            gFlowCounterRouteOrch = new FlowCounterRouteOrch(m_config_db.get(), route_pattern_tables);

            ASSERT_EQ(gVrfOrch, nullptr);
            gVrfOrch = new VRFOrch(m_app_db.get(), APP_VRF_TABLE_NAME, m_state_db.get(), STATE_VRF_OBJECT_TABLE_NAME);

            vector<table_name_with_pri_t> intf_tables = {
                { APP_INTF_TABLE_NAME, IntfsOrch::intfsorch_pri },
                { APP_SAG_TABLE_NAME,  IntfsOrch::intfsorch_pri }
            };
            ASSERT_EQ(gIntfsOrch, nullptr);
            gIntfsOrch = new IntfsOrch(m_app_db.get(), intf_tables, gVrfOrch, m_chassis_app_db.get());

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
            gNeighOrch = new NeighOrch(m_app_db.get(), APP_NEIGH_TABLE_NAME, gIntfsOrch, gFdbOrch, gPortsOrch, m_chassis_app_db.get());

            ASSERT_EQ(gFgNhgOrch, nullptr);
            const int fgnhgorch_pri = 15;
            vector<table_name_with_pri_t> fgnhg_tables = {
                { CFG_FG_NHG,        fgnhgorch_pri },
                { CFG_FG_NHG_PREFIX, fgnhgorch_pri },
                { CFG_FG_NHG_MEMBER, fgnhgorch_pri }
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

            ASSERT_EQ(gRouteOrch, nullptr);
            const int routeorch_pri = 5;
            vector<table_name_with_pri_t> route_tables = {
                { APP_ROUTE_TABLE_NAME,       routeorch_pri },
                { APP_LABEL_ROUTE_TABLE_NAME, routeorch_pri }
            };
            gRouteOrch = new RouteOrch(m_app_db.get(), route_tables, gSwitchOrch, gNeighOrch, gIntfsOrch, gVrfOrch, gFgNhgOrch, gSrv6Orch);

            vector<TableConnector> policer_tables = {
                TableConnector(m_config_db.get(), CFG_POLICER_TABLE_NAME),
                TableConnector(m_config_db.get(), CFG_PORT_STORM_CONTROL_TABLE_NAME)
            };
            ASSERT_EQ(gPolicerOrch, nullptr);
            gPolicerOrch = new PolicerOrch(policer_tables, gPortsOrch);

            TableConnector stateDbMirrorSession(m_state_db.get(), STATE_MIRROR_SESSION_TABLE_NAME);
            TableConnector confDbMirrorSession(m_config_db.get(), CFG_MIRROR_SESSION_TABLE_NAME);
            ASSERT_EQ(gMirrorOrch, nullptr);
            gMirrorOrch = new MirrorOrch(stateDbMirrorSession, confDbMirrorSession,
                                         gPortsOrch, gRouteOrch, gNeighOrch, gFdbOrch, gPolicerOrch, gSwitchOrch);

            // Populate real ports so PfcWdAclHandler can resolve aliases and
            // the ingress DROP rule can match on IN_PORTS.
            Table portTable = Table(m_app_db.get(), APP_PORT_TABLE_NAME);
            auto ports = ut_helper::getInitialSaiPorts();
            ASSERT_FALSE(ports.empty());
            for (const auto &it : ports)
            {
                portTable.set(it.first, it.second);
            }
            portTable.set("PortConfigDone", { { "count", to_string(ports.size()) } });
            portTable.set("PortInitDone", { { "lanes", "0" } });
            gPortsOrch->addExistingData(&portTable);
            static_cast<Orch *>(gPortsOrch)->doTask();

            // AclOrch last: it queries switch ACL capabilities.
            TableConnector confDbAclTable(m_config_db.get(), CFG_ACL_TABLE_TABLE_NAME);
            TableConnector confDbAclRuleTable(m_config_db.get(), CFG_ACL_RULE_TABLE_NAME);
            vector<TableConnector> acl_table_connectors = { confDbAclTable, confDbAclRuleTable };
            ASSERT_EQ(gAclOrch, nullptr);
            gAclOrch = new AclOrch(acl_table_connectors, m_state_db.get(), gSwitchOrch, gPortsOrch,
                                   gMirrorOrch, gNeighOrch, gRouteOrch);

            // The handler's table dict is class-static; start each test clean.
            PfcWdAclHandler::m_aclTables.clear();
        }

        void TearDown() override
        {
            PfcWdAclHandler::m_aclTables.clear();

            delete gAclOrch;
            gAclOrch = nullptr;
            delete gMirrorOrch;
            gMirrorOrch = nullptr;
            delete gPolicerOrch;
            gPolicerOrch = nullptr;
            delete gRouteOrch;
            gRouteOrch = nullptr;
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
            delete gFlowCounterRouteOrch;
            gFlowCounterRouteOrch = nullptr;
            delete gQosOrch;
            gQosOrch = nullptr;
            delete gBufferOrch;
            gBufferOrch = nullptr;
            delete gPortsOrch;
            gPortsOrch = nullptr;
            delete gSwitchOrch;
            gSwitchOrch = nullptr;
            delete gCrmOrch;
            gCrmOrch = nullptr;
            gDirectory.m_values.clear();

            auto status = sai_switch_api->remove_switch(gSwitchId);
            ASSERT_EQ(status, SAI_STATUS_SUCCESS);
            gSwitchId = 0;

            ut_helper::uninitSaiApi();
        }

        sai_object_id_t getPortOid(const string &alias)
        {
            Port p;
            EXPECT_TRUE(gPortsOrch->getPort(alias, p));
            return p.m_port_id;
        }

        sai_object_id_t getQueueOid(const string &alias, size_t queueIdx)
        {
            Port p;
            EXPECT_TRUE(gPortsOrch->getPort(alias, p));
            EXPECT_GT(p.m_queue_ids.size(), queueIdx);
            return p.m_queue_ids[queueIdx];
        }
    };

    static string ingressRuleName(uint8_t queueId)
    {
        return "Rule_PfcWdAclHandler_" + to_string(queueId);
    }

    // Ingress DROP ACL table creation fails: the handler must mark itself
    // rolled back, leave no half-built state and destroy cleanly.
    TEST_F(PfcActionHandlerTest, IngressAclTableCreateFailure)
    {
        AclCreateFailureInjector inject;
        inject.m_failTableAt = 1;

        {
            PfcWdAclHandler handler(getPortOid("Ethernet0"), getQueueOid("Ethernet0", 3), 3, m_countersTable);

            EXPECT_TRUE(handler.m_rolledBack);
            EXPECT_FALSE(handler.isValid());

            // No table left behind, neither in AclOrch nor in the class dict.
            EXPECT_EQ(gAclOrch->getTableById(INGRESS_TABLE_DROP), SAI_NULL_OBJECT_ID);
            EXPECT_TRUE(PfcWdAclHandler::m_aclTables.empty());
        }
        // Destructor of a rolled-back handler must not throw (it used to
        // SWSS_LOG_THROW on the missing ingress rule).
    }

    // Ingress table creation succeeds but the ingress rule fails (first-time
    // path): rolled back, table kept, rule absent.
    TEST_F(PfcActionHandlerTest, IngressAclRuleCreateFailureFirstTime)
    {
        AclCreateFailureInjector inject;
        inject.m_failEntryAt = 1;

        {
            PfcWdAclHandler handler(getPortOid("Ethernet0"), getQueueOid("Ethernet0", 3), 3, m_countersTable);

            EXPECT_TRUE(handler.m_rolledBack);
            EXPECT_FALSE(handler.isValid());

            EXPECT_NE(gAclOrch->getTableById(INGRESS_TABLE_DROP), SAI_NULL_OBJECT_ID);
            EXPECT_EQ(gAclOrch->getAclRule(INGRESS_TABLE_DROP, ingressRuleName(3)), nullptr);
        }
    }

    // Ingress rule creation fails when the ingress table already exists
    // (second handler, different queue).
    TEST_F(PfcActionHandlerTest, IngressAclRuleCreateFailureExistingTable)
    {
        AclCreateFailureInjector inject;

        // First handler is fully successful.
        PfcWdAclHandler good(getPortOid("Ethernet0"), getQueueOid("Ethernet0", 3), 3, m_countersTable);
        ASSERT_TRUE(good.isValid());
        ASSERT_NE(gAclOrch->getAclRule(INGRESS_TABLE_DROP, ingressRuleName(3)), nullptr);

        // Second handler for another queue: fail its ingress rule creation.
        inject.m_failEntryAt = inject.m_entryCalls + 1;
        {
            PfcWdAclHandler bad(getPortOid("Ethernet0"), getQueueOid("Ethernet0", 4), 4, m_countersTable);

            EXPECT_TRUE(bad.m_rolledBack);
            EXPECT_FALSE(bad.isValid());
            EXPECT_EQ(gAclOrch->getAclRule(INGRESS_TABLE_DROP, ingressRuleName(4)), nullptr);
        }

        // The first handler's state is untouched.
        EXPECT_NE(gAclOrch->getAclRule(INGRESS_TABLE_DROP, ingressRuleName(3)), nullptr);
    }

    // Egress (per-queue, non-shared) ACL table creation fails: the ingress
    // drop rule installed just before must be rolled back.
    TEST_F(PfcActionHandlerTest, EgressAclTableCreateFailureRollsBackIngress)
    {
        AclCreateFailureInjector inject;
        inject.m_failTableAt = 2; // 1st = ingress table, 2nd = egress table

        {
            PfcWdAclHandler handler(getPortOid("Ethernet0"), getQueueOid("Ethernet0", 3), 3, m_countersTable);

            EXPECT_TRUE(handler.m_rolledBack);
            EXPECT_FALSE(handler.isValid());

            // Ingress table remains, but the ingress rule was rolled back.
            EXPECT_NE(gAclOrch->getTableById(INGRESS_TABLE_DROP), SAI_NULL_OBJECT_ID);
            EXPECT_EQ(gAclOrch->getAclRule(INGRESS_TABLE_DROP, ingressRuleName(3)), nullptr);

            // No egress table was left behind.
            EXPECT_EQ(gAclOrch->getTableById("EgressTable_PfcWdAclHandler_3"), SAI_NULL_OBJECT_ID);
            EXPECT_EQ(PfcWdAclHandler::m_aclTables.count("EgressTable_PfcWdAclHandler_3"), 0U);
        }
    }

    // Egress (per-queue, non-shared) rule creation fails after the egress
    // table was created: the fresh egress table and the ingress rule are both
    // rolled back.
    TEST_F(PfcActionHandlerTest, EgressAclRuleCreateFailureRollsBackTableAndIngress)
    {
        AclCreateFailureInjector inject;
        inject.m_failEntryAt = 2; // 1st = ingress rule, 2nd = egress rule

        {
            PfcWdAclHandler handler(getPortOid("Ethernet0"), getQueueOid("Ethernet0", 3), 3, m_countersTable);

            EXPECT_TRUE(handler.m_rolledBack);
            EXPECT_FALSE(handler.isValid());

            EXPECT_EQ(gAclOrch->getAclRule(INGRESS_TABLE_DROP, ingressRuleName(3)), nullptr);
            EXPECT_EQ(gAclOrch->getTableById("EgressTable_PfcWdAclHandler_3"), SAI_NULL_OBJECT_ID);
            EXPECT_EQ(PfcWdAclHandler::m_aclTables.count("EgressTable_PfcWdAclHandler_3"), 0U);
        }
    }

    // Shared egress table mode (BRCM DNX): shared egress table creation fails
    // on the first handler; ingress rule is rolled back.
    TEST_F(PfcActionHandlerTest, SharedEgressAclTableCreateFailure)
    {
        EnvGuard platformGuard("platform", BRCM_PLATFORM_SUBSTRING);
        EnvGuard subPlatformGuard("sub_platform", BRCM_DNX_PLATFORM_SUBSTRING);

        AclCreateFailureInjector inject;
        inject.m_failTableAt = 2; // 1st = ingress table, 2nd = shared egress table

        {
            PfcWdAclHandler handler(getPortOid("Ethernet0"), getQueueOid("Ethernet0", 3), 3, m_countersTable);

            EXPECT_TRUE(handler.m_rolledBack);
            EXPECT_FALSE(handler.isValid());

            EXPECT_EQ(gAclOrch->getAclRule(INGRESS_TABLE_DROP, ingressRuleName(3)), nullptr);
            EXPECT_EQ(gAclOrch->getTableById("EgressTable_PfcWdAclHandler"), SAI_NULL_OBJECT_ID);
        }
        // Rolled-back destructor in shared mode also exercises the
        // missing-egress-rule notice path instead of throwing.
    }

    // Shared egress table mode: the shared table already exists (created by a
    // successful handler); a later handler fails its egress rule creation. The
    // shared table must be left in place, only that handler is rolled back.
    TEST_F(PfcActionHandlerTest, SharedEgressAclRuleCreateFailureKeepsSharedTable)
    {
        EnvGuard platformGuard("platform", BRCM_PLATFORM_SUBSTRING);
        EnvGuard subPlatformGuard("sub_platform", BRCM_DNX_PLATFORM_SUBSTRING);

        AclCreateFailureInjector inject;

        PfcWdAclHandler good(getPortOid("Ethernet0"), getQueueOid("Ethernet0", 3), 3, m_countersTable);
        ASSERT_TRUE(good.isValid());
        ASSERT_NE(gAclOrch->getTableById("EgressTable_PfcWdAclHandler"), SAI_NULL_OBJECT_ID);
        ASSERT_NE(gAclOrch->getAclRule("EgressTable_PfcWdAclHandler", "Egress_Rule_PfcWdAclHandler_Ethernet0_3"), nullptr);

        // Second handler on another port/queue: ingress rule (1st entry
        // create) succeeds, egress rule (2nd) fails.
        inject.m_failEntryAt = inject.m_entryCalls + 2;
        {
            PfcWdAclHandler bad(getPortOid("Ethernet4"), getQueueOid("Ethernet4", 4), 4, m_countersTable);

            EXPECT_TRUE(bad.m_rolledBack);
            EXPECT_FALSE(bad.isValid());

            // Shared egress table survives, along with the good handler's rule.
            EXPECT_NE(gAclOrch->getTableById("EgressTable_PfcWdAclHandler"), SAI_NULL_OBJECT_ID);
            EXPECT_NE(gAclOrch->getAclRule("EgressTable_PfcWdAclHandler", "Egress_Rule_PfcWdAclHandler_Ethernet0_3"), nullptr);

            // The failed handler's rules were rolled back.
            EXPECT_EQ(gAclOrch->getAclRule("EgressTable_PfcWdAclHandler", "Egress_Rule_PfcWdAclHandler_Ethernet4_4"), nullptr);
            EXPECT_EQ(gAclOrch->getAclRule(INGRESS_TABLE_DROP, ingressRuleName(4)), nullptr);
        }
    }

    // PfcWdSwOrch consumes isValid(): a storm whose drop handler failed to
    // install its ACLs must be marked "failed" in the PFC_WD_SW_STATE_TABLE
    // and must not record INSTORM state; recovery and a later successful
    // storm go back to "configured".
    TEST_F(PfcActionHandlerTest, PfcWdSwOrchHandlesRolledBackDropHandler)
    {
        vector<string> pfc_wd_tables = { CFG_PFC_WD_TABLE_NAME };
        static const vector<sai_port_stat_t> portStatIds = {
            SAI_PORT_STAT_PFC_3_RX_PKTS,
        };
        static const vector<sai_queue_stat_t> queueStatIds = {
            SAI_QUEUE_STAT_PACKETS,
        };
        static const vector<sai_queue_attr_t> queueAttrIds = {
            SAI_QUEUE_ATTR_PAUSE_STATUS,
        };

        auto orch = make_unique<PfcWdSwOrch<PfcWdAclHandler, PfcWdLossyHandler>>(
            m_config_db.get(), pfc_wd_tables, portStatIds, queueStatIds, queueAttrIds, 100);

        Port port;
        ASSERT_TRUE(gPortsOrch->getPort("Ethernet0", port));
        ASSERT_GT(port.m_queue_ids.size(), 3U);
        sai_object_id_t queueOid = port.m_queue_ids[3];

        // Register the queue directly (bypasses the flex counter plumbing that
        // registerInWdDb would need).
        orch->m_entryMap.emplace(queueOid,
            PfcWdSwOrch<PfcWdAclHandler, PfcWdLossyHandler>::PfcWdQueueEntry(
                PfcWdAction::PFC_WD_ACTION_DROP, port.m_port_id, 3, port.m_alias));

        Table stateTable(m_state_db.get(), "PFC_WD_SW_STATE_TABLE");
        string status;

        {
            // Storm hits while ACL table creation fails: no abort, queue marked failed.
            AclCreateFailureInjector inject;
            inject.m_failTableAt = 1;

            EXPECT_TRUE(orch->startWdActionOnQueue("storm", queueOid));

            auto &entry = orch->m_entryMap.at(queueOid);
            ASSERT_NE(entry.handler, nullptr);
            EXPECT_FALSE(entry.handler->isValid());

            EXPECT_TRUE(stateTable.hget("Ethernet0:3", "status", status));
            EXPECT_EQ(status, "failed");
        }

        // Storm restores: handler released, state back to configured.
        EXPECT_TRUE(orch->startWdActionOnQueue("restore", queueOid));
        EXPECT_EQ(orch->m_entryMap.at(queueOid).handler, nullptr);
        EXPECT_TRUE(stateTable.hget("Ethernet0:3", "status", status));
        EXPECT_EQ(status, "configured");

        // A storm with healthy SAI installs the drop action and records INSTORM.
        EXPECT_TRUE(orch->startWdActionOnQueue("storm", queueOid));
        {
            auto &entry = orch->m_entryMap.at(queueOid);
            ASSERT_NE(entry.handler, nullptr);
            EXPECT_TRUE(entry.handler->isValid());
        }
        EXPECT_TRUE(stateTable.hget("Ethernet0:3", "status", status));
        EXPECT_EQ(status, "configured");

        // Restore before the orch (and gAclOrch) go away.
        EXPECT_TRUE(orch->startWdActionOnQueue("restore", queueOid));
        EXPECT_EQ(orch->m_entryMap.at(queueOid).handler, nullptr);

        // An unknown event is rejected without throwing.
        EXPECT_FALSE(orch->startWdActionOnQueue("bogus_event", queueOid));
    }
}
