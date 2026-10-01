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

        using set_fn = sai_status_t (*)(sai_object_id_t, const sai_attribute_t *);

        create_fn m_origCreateTable;
        create_fn m_origCreateEntry;
        set_fn m_origSetEntry;
        int m_tableCalls = 0;
        int m_entryCalls = 0;
        int m_inPortsSetCalls = 0;
        int m_failTableAt = 0;
        int m_failEntryAt = 0;
        int m_failInPortsSetAt = 0;

        shared_ptr<SaiSpyFunctor<SAI_API_ACL, SAI_OBJECT_TYPE_ACL_TABLE, sai_status_t,
            sai_object_id_t *, sai_object_id_t, uint32_t, const sai_attribute_t *>> m_tableSpy;
        shared_ptr<SaiSpyFunctor<SAI_API_ACL, SAI_OBJECT_TYPE_ACL_ENTRY, sai_status_t,
            sai_object_id_t *, sai_object_id_t, uint32_t, const sai_attribute_t *>> m_entrySpy;
        shared_ptr<SaiSpyFunctor<SAI_API_ACL, SAI_OBJECT_TYPE_ACL_ENTRY, sai_status_t,
            sai_object_id_t, const sai_attribute_t *>> m_setEntrySpy;

        AclCreateFailureInjector()
        {
            m_origCreateTable = sai_acl_api->create_acl_table;
            m_origCreateEntry = sai_acl_api->create_acl_entry;
            m_origSetEntry = sai_acl_api->set_acl_entry_attribute;

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

            // Only IN_PORTS writes are counted and failed: that is the one
            // attribute the batched flush rewrites.
            m_setEntrySpy = SpyOn<SAI_API_ACL, SAI_OBJECT_TYPE_ACL_ENTRY>(&sai_acl_api->set_acl_entry_attribute);
            m_setEntrySpy->callFake([this](sai_object_id_t oid, const sai_attribute_t *attr) -> sai_status_t {
                if (attr != nullptr && attr->id == SAI_ACL_ENTRY_ATTR_FIELD_IN_PORTS)
                {
                    ++m_inPortsSetCalls;
                    if (m_failInPortsSetAt != 0 && m_inPortsSetCalls == m_failInPortsSetAt)
                    {
                        return SAI_STATUS_FAILURE;
                    }
                }
                return m_origSetEntry(oid, attr);
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

            // The handler's table dict and pending-edit map are class-static;
            // start each test clean.
            PfcWdAclHandler::m_aclTables.clear();
            PfcWdAclHandler::m_pendingInPorts.clear();
        }

        void TearDown() override
        {
            PfcWdAclHandler::m_aclTables.clear();
            PfcWdAclHandler::m_pendingInPorts.clear();

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

        using WdOrch = PfcWdSwOrch<PfcWdAclHandler, PfcWdLossyHandler>;

        // A software watchdog orch with the given (port, queue 3) pairs
        // registered directly, bypassing the flex counter plumbing that
        // registerInWdDb() would need.
        unique_ptr<WdOrch> makeOrch(const vector<string> &aliases)
        {
            vector<string> pfc_wd_tables = { CFG_PFC_WD_TABLE_NAME };
            static const vector<sai_port_stat_t> portStatIds = { SAI_PORT_STAT_PFC_3_RX_PKTS };
            static const vector<sai_queue_stat_t> queueStatIds = { SAI_QUEUE_STAT_PACKETS };
            static const vector<sai_queue_attr_t> queueAttrIds = { SAI_QUEUE_ATTR_PAUSE_STATUS };
            auto orch = make_unique<WdOrch>(m_config_db.get(), pfc_wd_tables, portStatIds, queueStatIds, queueAttrIds, 100);
            for (const auto &alias : aliases)
            {
                Port port;
                EXPECT_TRUE(gPortsOrch->getPort(alias, port));
                EXPECT_GT(port.m_queue_ids.size(), 3U);
                orch->m_entryMap.emplace(port.m_queue_ids[3],
                    WdOrch::PfcWdQueueEntry(PfcWdAction::PFC_WD_ACTION_DROP, port.m_port_id, 3, port.m_alias));
            }
            return orch;
        }

        // One notification batch, as doTask(NotificationConsumer&) processes it.
        void batch(WdOrch &orch, const vector<pair<string, sai_object_id_t>> &events)
        {
            for (const auto &ev : events)
            {
                EXPECT_TRUE(orch.startWdActionOnQueue(ev.first, ev.second));
            }
            orch.flushPendingActions();
        }

        bool hasHandler(WdOrch &orch, sai_object_id_t queueOid)
        {
            auto it = orch.m_entryMap.find(queueOid);
            return it != orch.m_entryMap.end() && it->second.handler != nullptr;
        }

        // The field pfc_detect_*.lua gates on: only "operational" queues are
        // evaluated for a new storm.
        string queueStatus(sai_object_id_t queueOid)
        {
            string value;
            if (!m_countersTable->hget(sai_serialize_object_id(queueOid), "PFC_WD_STATUS", value))
            {
                return "";
            }
            return value;
        }
    };

    static string ingressRuleName(uint8_t queueId)
    {
        return "Rule_PfcWdAclHandler_" + to_string(queueId);
    }

    static set<sai_object_id_t> ingressRuleInPorts(uint8_t queueId)
    {
        AclRule *rule = gAclOrch->getAclRule(INGRESS_TABLE_DROP, ingressRuleName(queueId));
        if (rule == nullptr)
        {
            return {};
        }
        auto v = rule->getInPorts();
        return set<sai_object_id_t>(v.begin(), v.end());
    }

    // The handler constructor only queues its ingress change; flush() is what
    // PfcWdSwOrch calls at the end of a batch to program it.
    static set<sai_object_id_t> flushHandlers()
    {
        return PfcWdAclHandler::flush();
    }

    // Ingress DROP ACL table creation fails at flush time: the flush reports
    // the queue, leaves no half-built ingress state and drops the pending add.
    TEST_F(PfcActionHandlerTest, IngressAclTableCreateFailure)
    {
        AclCreateFailureInjector inject;
        inject.m_failTableAt = 2; // 1st = egress table (constructor), 2nd = ingress table (flush)
        {
            sai_object_id_t queueOid = getQueueOid("Ethernet0", 3);
            PfcWdAclHandler handler(getPortOid("Ethernet0"), queueOid, 3, m_countersTable);
            EXPECT_TRUE(handler.isValid());

            auto failed = flushHandlers();
            EXPECT_EQ(failed.count(queueOid), 1U);
            // No ingress table left behind, neither in AclOrch nor in the class dict.
            EXPECT_EQ(gAclOrch->getTableById(INGRESS_TABLE_DROP), SAI_NULL_OBJECT_ID);
            EXPECT_EQ(PfcWdAclHandler::m_aclTables.count(INGRESS_TABLE_DROP), 0U);
            // A create failure is not retried.
            EXPECT_TRUE(PfcWdAclHandler::m_pendingInPorts.empty());

            // PfcWdSwOrch invalidates the handler before dropping it; the
            // destructor of an invalid handler must queue no removal.
            handler.markInvalid();
            EXPECT_FALSE(handler.isValid());
        }
        EXPECT_TRUE(PfcWdAclHandler::m_pendingInPorts.empty());
    }

    // Ingress table creation succeeds but the ingress rule fails (first-time
    // path, at flush): the queue is reported, table kept, rule absent.
    TEST_F(PfcActionHandlerTest, IngressAclRuleCreateFailureFirstTime)
    {
        AclCreateFailureInjector inject;
        inject.m_failEntryAt = 2; // 1st = egress rule (constructor), 2nd = ingress rule (flush)
        {
            sai_object_id_t queueOid = getQueueOid("Ethernet0", 3);
            PfcWdAclHandler handler(getPortOid("Ethernet0"), queueOid, 3, m_countersTable);
            EXPECT_TRUE(handler.isValid());

            auto failed = flushHandlers();
            EXPECT_EQ(failed.count(queueOid), 1U);
            EXPECT_NE(gAclOrch->getTableById(INGRESS_TABLE_DROP), SAI_NULL_OBJECT_ID);
            EXPECT_EQ(gAclOrch->getAclRule(INGRESS_TABLE_DROP, ingressRuleName(3)), nullptr);
            handler.markInvalid();
        }
    }

    // Ingress rule creation fails when the ingress table already exists
    // (second handler, different queue).
    TEST_F(PfcActionHandlerTest, IngressAclRuleCreateFailureExistingTable)
    {
        AclCreateFailureInjector inject;

        // First handler is fully successful once flushed.
        PfcWdAclHandler good(getPortOid("Ethernet0"), getQueueOid("Ethernet0", 3), 3, m_countersTable);
        ASSERT_TRUE(good.isValid());
        ASSERT_TRUE(flushHandlers().empty());
        ASSERT_NE(gAclOrch->getAclRule(INGRESS_TABLE_DROP, ingressRuleName(3)), nullptr);

        // Second handler for another queue: its constructor creates the egress
        // rule (+1), the flush creates the ingress rule (+2) - fail that one.
        inject.m_failEntryAt = inject.m_entryCalls + 2;
        {
            sai_object_id_t queueOid = getQueueOid("Ethernet0", 4);
            PfcWdAclHandler bad(getPortOid("Ethernet0"), queueOid, 4, m_countersTable);
            EXPECT_TRUE(bad.isValid());
            auto failed = flushHandlers();
            EXPECT_EQ(failed.count(queueOid), 1U);
            EXPECT_EQ(gAclOrch->getAclRule(INGRESS_TABLE_DROP, ingressRuleName(4)), nullptr);
            bad.markInvalid();
        }

        // The first handler's state is untouched.
        EXPECT_NE(gAclOrch->getAclRule(INGRESS_TABLE_DROP, ingressRuleName(3)), nullptr);
    }

    // Egress (per-queue, non-shared) ACL table creation fails: the handler is
    // rolled back before it queues any ingress change, so a flush programs
    // nothing.
    TEST_F(PfcActionHandlerTest, EgressAclTableCreateFailureQueuesNoIngressChange)
    {
        AclCreateFailureInjector inject;
        inject.m_failTableAt = 1; // the egress table is the constructor's only table create
        {
            PfcWdAclHandler handler(getPortOid("Ethernet0"), getQueueOid("Ethernet0", 3), 3, m_countersTable);
            EXPECT_TRUE(handler.m_rolledBack);
            EXPECT_FALSE(handler.isValid());
            EXPECT_TRUE(PfcWdAclHandler::m_pendingInPorts.empty());
            EXPECT_TRUE(flushHandlers().empty());
            EXPECT_EQ(gAclOrch->getTableById(INGRESS_TABLE_DROP), SAI_NULL_OBJECT_ID);
            // No egress table was left behind.
            EXPECT_EQ(gAclOrch->getTableById("EgressTable_PfcWdAclHandler_3"), SAI_NULL_OBJECT_ID);
            EXPECT_EQ(PfcWdAclHandler::m_aclTables.count("EgressTable_PfcWdAclHandler_3"), 0U);
        }
    }

    // Egress (per-queue, non-shared) rule creation fails after the egress
    // table was created: rolled back, no ingress change queued. The table is
    // kept - tables are pre-provisioned state, a rule-less one is harmless.
    TEST_F(PfcActionHandlerTest, EgressAclRuleCreateFailureQueuesNoIngressChange)
    {
        AclCreateFailureInjector inject;
        inject.m_failEntryAt = 1; // the egress rule is the constructor's only entry create
        {
            PfcWdAclHandler handler(getPortOid("Ethernet0"), getQueueOid("Ethernet0", 3), 3, m_countersTable);
            EXPECT_TRUE(handler.m_rolledBack);
            EXPECT_FALSE(handler.isValid());
            EXPECT_TRUE(PfcWdAclHandler::m_pendingInPorts.empty());
            EXPECT_TRUE(flushHandlers().empty());
            EXPECT_EQ(gAclOrch->getAclRule(INGRESS_TABLE_DROP, ingressRuleName(3)), nullptr);
            EXPECT_EQ(gAclOrch->getAclRule("EgressTable_PfcWdAclHandler_3", ingressRuleName(3)), nullptr);
        }
    }

    // Shared egress table mode (BRCM DNX): shared egress table creation fails
    // on the first handler; nothing is queued for the ingress rule.
    TEST_F(PfcActionHandlerTest, SharedEgressAclTableCreateFailure)
    {
        EnvGuard platformGuard("platform", BRCM_PLATFORM_SUBSTRING);
        EnvGuard subPlatformGuard("sub_platform", BRCM_DNX_PLATFORM_SUBSTRING);
        AclCreateFailureInjector inject;
        inject.m_failTableAt = 1; // the shared egress table is the constructor's only table create
        {
            PfcWdAclHandler handler(getPortOid("Ethernet0"), getQueueOid("Ethernet0", 3), 3, m_countersTable);
            EXPECT_TRUE(handler.m_rolledBack);
            EXPECT_FALSE(handler.isValid());
            EXPECT_TRUE(PfcWdAclHandler::m_pendingInPorts.empty());
            EXPECT_TRUE(flushHandlers().empty());
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
        ASSERT_TRUE(flushHandlers().empty());
        ASSERT_NE(gAclOrch->getTableById("EgressTable_PfcWdAclHandler"), SAI_NULL_OBJECT_ID);
        ASSERT_NE(gAclOrch->getAclRule("EgressTable_PfcWdAclHandler", "Egress_Rule_PfcWdAclHandler_Ethernet0_3"), nullptr);
        ASSERT_NE(gAclOrch->getAclRule(INGRESS_TABLE_DROP, ingressRuleName(3)), nullptr);

        // Second handler on another port/queue: its egress rule is the next
        // entry create - fail it.
        inject.m_failEntryAt = inject.m_entryCalls + 1;
        {
            PfcWdAclHandler bad(getPortOid("Ethernet4"), getQueueOid("Ethernet4", 4), 4, m_countersTable);
            EXPECT_TRUE(bad.m_rolledBack);
            EXPECT_FALSE(bad.isValid());
            EXPECT_TRUE(PfcWdAclHandler::m_pendingInPorts.empty());
            EXPECT_TRUE(flushHandlers().empty());
            // Shared egress table survives, along with the good handler's rules.
            EXPECT_NE(gAclOrch->getTableById("EgressTable_PfcWdAclHandler"), SAI_NULL_OBJECT_ID);
            EXPECT_NE(gAclOrch->getAclRule("EgressTable_PfcWdAclHandler", "Egress_Rule_PfcWdAclHandler_Ethernet0_3"), nullptr);
            EXPECT_NE(gAclOrch->getAclRule(INGRESS_TABLE_DROP, ingressRuleName(3)), nullptr);
            // The failed handler installed nothing.
            EXPECT_EQ(gAclOrch->getAclRule("EgressTable_PfcWdAclHandler", "Egress_Rule_PfcWdAclHandler_Ethernet4_4"), nullptr);
            EXPECT_EQ(gAclOrch->getAclRule(INGRESS_TABLE_DROP, ingressRuleName(4)), nullptr);
        }
    }

    // PfcWdSwOrch consumes isValid() once the batch is flushed: a storm whose
    // drop handler failed to install its ACLs must be marked "failed" in the
    // PFC_WD_SW_STATE_TABLE and must not record INSTORM state; recovery and a
    // later successful storm go back to "configured".
    TEST_F(PfcActionHandlerTest, PfcWdSwOrchHandlesRolledBackDropHandler)
    {
        auto orch = makeOrch({ "Ethernet0" });
        sai_object_id_t queueOid = getQueueOid("Ethernet0", 3);

        Table stateTable(m_state_db.get(), "PFC_WD_SW_STATE_TABLE");
        string status;

        {
            // Storm hits while ACL table creation fails: no abort, queue marked failed.
            AclCreateFailureInjector inject;
            inject.m_failTableAt = 1;
            batch(*orch, { { "storm", queueOid } });
            auto &entry = orch->m_entryMap.at(queueOid);
            ASSERT_NE(entry.handler, nullptr);
            EXPECT_FALSE(entry.handler->isValid());
            EXPECT_TRUE(stateTable.hget("Ethernet0:3", "status", status));
            EXPECT_EQ(status, "failed");
        }

        // Storm restores: handler released, state back to configured.
        batch(*orch, { { "restore", queueOid } });
        EXPECT_EQ(orch->m_entryMap.at(queueOid).handler, nullptr);
        EXPECT_TRUE(stateTable.hget("Ethernet0:3", "status", status));
        EXPECT_EQ(status, "configured");

        // A storm with healthy SAI installs the drop action and records INSTORM.
        batch(*orch, { { "storm", queueOid } });
        {
            auto &entry = orch->m_entryMap.at(queueOid);
            ASSERT_NE(entry.handler, nullptr);
            EXPECT_TRUE(entry.handler->isValid());
        }
        EXPECT_TRUE(stateTable.hget("Ethernet0:3", "status", status));
        EXPECT_EQ(status, "configured");
        EXPECT_EQ(ingressRuleInPorts(3), set<sai_object_id_t>({ getPortOid("Ethernet0") }));

        // Restore before the orch (and gAclOrch) go away.
        batch(*orch, { { "restore", queueOid } });
        EXPECT_EQ(orch->m_entryMap.at(queueOid).handler, nullptr);

        // An unknown event is rejected without throwing.
        EXPECT_FALSE(orch->startWdActionOnQueue("bogus_event", queueOid));
    }

    // The ingress rule is created carrying every port of the batch, a later
    // port arrives with a single IN_PORTS write, and the rule is removed
    // rather than written empty when the last port recovers.
    TEST_F(PfcActionHandlerTest, StormBatchWritesRuleOnce)
    {
        AclCreateFailureInjector inject;
        auto orch = makeOrch({ "Ethernet0", "Ethernet4" });
        sai_object_id_t portA = getPortOid("Ethernet0"), portB = getPortOid("Ethernet4");
        sai_object_id_t queueA = getQueueOid("Ethernet0", 3), queueB = getQueueOid("Ethernet4", 3);

        batch(*orch, { { "storm", queueA } });
        EXPECT_EQ(ingressRuleInPorts(3), set<sai_object_id_t>({ portA }));
        EXPECT_TRUE(hasHandler(*orch, queueA));
        EXPECT_EQ(queueStatus(queueA), "stormed");
        EXPECT_EQ(inject.m_inPortsSetCalls, 0);

        batch(*orch, { { "storm", queueB } });
        EXPECT_EQ(ingressRuleInPorts(3), set<sai_object_id_t>({ portA, portB }));
        EXPECT_EQ(inject.m_inPortsSetCalls, 1);

        batch(*orch, { { "restore", queueA }, { "restore", queueB } });
        EXPECT_EQ(gAclOrch->getAclRule(INGRESS_TABLE_DROP, ingressRuleName(3)), nullptr);
        EXPECT_EQ(inject.m_inPortsSetCalls, 1);
        EXPECT_FALSE(hasHandler(*orch, queueA));
        EXPECT_FALSE(hasHandler(*orch, queueB));
        EXPECT_EQ(queueStatus(queueA), "operational");
        EXPECT_EQ(queueStatus(queueB), "operational");
    }

    // A batch that fails its IN_PORTS write retries only its removals. The
    // failed add's handler is invalidated and destroyed, and an invalid
    // handler never queues a removal, so retrying the add would leave the
    // port in the rule with nothing to take it out.
    TEST_F(PfcActionHandlerTest, FailedInPortsWriteRetriesOnlyRemovals)
    {
        AclCreateFailureInjector inject;
        auto orch = makeOrch({ "Ethernet0", "Ethernet4" });
        sai_object_id_t portA = getPortOid("Ethernet0");
        sai_object_id_t queueA = getQueueOid("Ethernet0", 3), queueB = getQueueOid("Ethernet4", 3);

        batch(*orch, { { "storm", queueA } });
        ASSERT_EQ(ingressRuleInPorts(3), set<sai_object_id_t>({ portA }));

        // A recovers and B storms in the same batch; the one write fails.
        inject.m_failInPortsSetAt = 1;
        batch(*orch, { { "restore", queueA }, { "storm", queueB } });

        EXPECT_EQ(inject.m_inPortsSetCalls, 1);
        EXPECT_FALSE(hasHandler(*orch, queueA));
        EXPECT_FALSE(hasHandler(*orch, queueB));
        // A's removal was retried and emptied the rule; B was not re-added.
        EXPECT_EQ(gAclOrch->getAclRule(INGRESS_TABLE_DROP, ingressRuleName(3)), nullptr);
    }

    // After a failed write the queue must go back to "operational" in the
    // counters table: pfc_detect_*.lua only evaluates queues in that state, so
    // a queue left "stormed" with no handler would never be detected again
    // until pfcwd is restarted on the port.
    TEST_F(PfcActionHandlerTest, FailedInPortsWriteReleasesQueueForRedetection)
    {
        AclCreateFailureInjector inject;
        auto orch = makeOrch({ "Ethernet0", "Ethernet4" });
        sai_object_id_t portA = getPortOid("Ethernet0"), portB = getPortOid("Ethernet4");
        sai_object_id_t queueA = getQueueOid("Ethernet0", 3), queueB = getQueueOid("Ethernet4", 3);

        batch(*orch, { { "storm", queueA } });
        ASSERT_EQ(ingressRuleInPorts(3), set<sai_object_id_t>({ portA }));

        inject.m_failInPortsSetAt = 1;
        batch(*orch, { { "storm", queueB } });

        EXPECT_FALSE(hasHandler(*orch, queueB));
        EXPECT_EQ(ingressRuleInPorts(3), set<sai_object_id_t>({ portA }));
        EXPECT_EQ(queueStatus(queueB), "operational");

        // A restore for a queue with no handler changes nothing.
        batch(*orch, { { "restore", queueB } });
        EXPECT_EQ(queueStatus(queueB), "operational");

        // The next detection retries through the normal path.
        batch(*orch, { { "storm", queueB } });
        EXPECT_TRUE(hasHandler(*orch, queueB));
        EXPECT_EQ(ingressRuleInPorts(3), set<sai_object_id_t>({ portA, portB }));
        EXPECT_EQ(queueStatus(queueB), "stormed");

        // Restore before the orch (and gAclOrch) go away.
        batch(*orch, { { "restore", queueA }, { "restore", queueB } });
        EXPECT_EQ(gAclOrch->getAclRule(INGRESS_TABLE_DROP, ingressRuleName(3)), nullptr);
    }
}
