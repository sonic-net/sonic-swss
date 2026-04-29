#define private public // make Directory::m_values available to clean it.
#include "directory.h"
#undef private
#define protected public
#include "orch.h"
#undef protected

#include "ut_helper.h"
#include "mock_orchagent_main.h"
#include "mock_table.h"
#include "flexcounterorch.h"
#include "muxorch.h"
#include "fgnhgorch.h"


#define private public
#include "tamorch.h"
#undef private


extern std::string gMySwitchType;
extern sai_object_id_t gSwitchId;

namespace tamorch_test
{
using namespace std;
using namespace swss;

// Simple counters to verify SAI TAM usage for flow-unaware MOD
static int g_tam_create_count;
static int g_tam_remove_count;
static int g_switch_bind_tam_count;
static int g_switch_unbind_tam_count;
static int g_port_bind_tam_count;
static int g_port_unbind_tam_count;

static sai_tam_api_t      ut_sai_tam_api;
static sai_tam_api_t     *pold_sai_tam_api;
static sai_switch_api_t   ut_sai_switch_api;
static sai_switch_api_t  *pold_sai_switch_api;
static sai_port_api_t     ut_sai_port_api;
static sai_port_api_t    *pold_sai_port_api;

// --- SAI stubs ---

static sai_status_t _ut_stub_sai_create_tam(
    _Out_ sai_object_id_t *tam_id,
    _In_ sai_object_id_t switch_id,
    _In_ uint32_t attr_count,
    _In_ const sai_attribute_t *attr_list)
{
    (void)switch_id;
    (void)attr_count;
    (void)attr_list;
    static int tam_inst = 0;
    *tam_id = (sai_object_id_t)(0x1234 + tam_inst++);
    ++g_tam_create_count;
    return SAI_STATUS_SUCCESS;
}

static sai_status_t _ut_stub_sai_remove_tam(
    _In_ sai_object_id_t tam_id)
{
    (void)tam_id;
    ++g_tam_remove_count;
    return SAI_STATUS_SUCCESS;
}

static sai_status_t _ut_stub_sai_set_switch_attribute(
    _In_ sai_object_id_t switch_id,
    _In_ const sai_attribute_t *attr)
{
    (void)switch_id;
    if (attr->id == SAI_SWITCH_ATTR_TAM_OBJECT_ID)
    {
        if (attr->value.objlist.count == 0)
        {
            ++g_switch_unbind_tam_count;
        }
        else
        {
            ++g_switch_bind_tam_count;
        }
        return SAI_STATUS_SUCCESS;
    }
    return pold_sai_switch_api->set_switch_attribute(switch_id, attr);
}

static sai_status_t _ut_stub_sai_set_port_attribute(
    _In_ sai_object_id_t port_id,
    _In_ const sai_attribute_t *attr)
{
    (void)port_id;
    if (attr->id == SAI_PORT_ATTR_TAM_OBJECT)
    {
        if (attr->value.objlist.count == 0)
        {
            ++g_port_unbind_tam_count;
        }
        else
        {
            ++g_port_bind_tam_count;
        }
        return SAI_STATUS_SUCCESS;
    }
    return pold_sai_port_api->set_port_attribute(port_id, attr);
}

static void hook_sai_tam_apis()
{
    // TAM API
    ut_sai_tam_api = *sai_tam_api;
    pold_sai_tam_api = sai_tam_api;
    ut_sai_tam_api.create_tam = _ut_stub_sai_create_tam;
    ut_sai_tam_api.remove_tam = _ut_stub_sai_remove_tam;
    sai_tam_api = &ut_sai_tam_api;

    // SWITCH API
    ut_sai_switch_api = *sai_switch_api;
    pold_sai_switch_api = sai_switch_api;
    ut_sai_switch_api.set_switch_attribute = _ut_stub_sai_set_switch_attribute;
    sai_switch_api = &ut_sai_switch_api;

    // PORT API
    ut_sai_port_api = *sai_port_api;
    pold_sai_port_api = sai_port_api;
    ut_sai_port_api.set_port_attribute = _ut_stub_sai_set_port_attribute;
    sai_port_api = &ut_sai_port_api;
}

static void unhook_sai_tam_apis()
{
    sai_tam_api = pold_sai_tam_api;
    sai_switch_api = pold_sai_switch_api;
    sai_port_api = pold_sai_port_api;
}

class TamOrchFlowUnawareModTest : public ::testing::Test
{
protected:
    shared_ptr<DBConnector> m_app_db;
    shared_ptr<DBConnector> m_config_db;
    shared_ptr<DBConnector> m_state_db;
    shared_ptr<DBConnector> m_chassis_app_db;

    unique_ptr<TamOrch> m_tam_orch;

    void SetUp() override
    {
        // Init SAI and switch similar to other orch tests
        map<string, string> profile = {
            {"SAI_VS_SWITCH_TYPE", "SAI_VS_SWITCH_TYPE_BCM56850"},
            {"KV_DEVICE_MAC_ADDRESS", "20:03:04:05:06:00"}
        };
        ut_helper::initSaiApi(profile);

        m_app_db = make_shared<DBConnector>("APPL_DB", 0);
        m_config_db = make_shared<DBConnector>("CONFIG_DB", 0);
        m_state_db = make_shared<DBConnector>("STATE_DB", 0);
        if (gMySwitchType == "voq")
        {
            m_chassis_app_db = make_shared<DBConnector>("CHASSIS_APP_DB", 0);
        }

        // Create switch
        sai_attribute_t attr;
        attr.id = SAI_SWITCH_ATTR_INIT_SWITCH;
        attr.value.booldata = true;
        auto status = sai_switch_api->create_switch(&gSwitchId, 1, &attr);
        ASSERT_EQ(status, SAI_STATUS_SUCCESS);

        // Initialize globals used by RouteOrch (align with routeorch_ut)
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

        // Basic global orchestrators (borrow from routeorch_ut / portsorch_ut style init)
        TableConnector stateDbSwitchTable(m_state_db.get(), "SWITCH_CAPABILITY");
        TableConnector conf_asic_sensors(m_config_db.get(), CFG_ASIC_SENSORS_TABLE_NAME);
        TableConnector app_switch_table(m_app_db.get(), APP_SWITCH_TABLE_NAME);
        vector<TableConnector> switch_tables = { conf_asic_sensors, app_switch_table };
        ASSERT_EQ(gSwitchOrch, nullptr);
        gSwitchOrch = new SwitchOrch(m_app_db.get(), switch_tables, stateDbSwitchTable);

        const int portsorch_base_pri = 40;
        vector<table_name_with_pri_t> ports_tables = {
            { APP_PORT_TABLE_NAME,                 portsorch_base_pri + 5 },
            { APP_SEND_TO_INGRESS_PORT_TABLE_NAME, portsorch_base_pri + 5 },
            { APP_VLAN_TABLE_NAME,                 portsorch_base_pri + 2 },
            { APP_VLAN_MEMBER_TABLE_NAME,          portsorch_base_pri },
            { APP_LAG_TABLE_NAME,                  portsorch_base_pri + 4 },
            { APP_LAG_MEMBER_TABLE_NAME,           portsorch_base_pri },
        };
        ASSERT_EQ(gPortsOrch, nullptr);
        gPortsOrch = new PortsOrch(m_app_db.get(), m_state_db.get(), ports_tables, m_chassis_app_db.get());
        gDirectory.set(gPortsOrch);

        // Buffer orch is required because PortsOrch::doPortTask() calls gBufferOrch->isPortReady()
        vector<string> buffer_tables = {
            APP_BUFFER_POOL_TABLE_NAME,
            APP_BUFFER_PROFILE_TABLE_NAME,
            APP_BUFFER_QUEUE_TABLE_NAME,
            APP_BUFFER_PG_TABLE_NAME,
            APP_BUFFER_PORT_INGRESS_PROFILE_LIST_NAME,
            APP_BUFFER_PORT_EGRESS_PROFILE_LIST_NAME,
        };
        ASSERT_EQ(gBufferOrch, nullptr);
        gBufferOrch = new BufferOrch(m_app_db.get(), m_config_db.get(), m_state_db.get(), buffer_tables);

        // Create and register FlexCounterOrch so PortsOrch::registerPort() can use it
        vector<string> flex_counter_tables = {
            CFG_FLEX_COUNTER_TABLE_NAME
        };
        auto *flexCounterOrch = new FlexCounterOrch(m_config_db.get(), flex_counter_tables);
        gDirectory.set(flexCounterOrch);

        // Start FlowCounterRouteOrch so RouteOrch can account for special routes
        static const vector<string> route_pattern_tables = {
            CFG_FLOW_COUNTER_ROUTE_PATTERN_TABLE_NAME,
        };
        ASSERT_EQ(gFlowCounterRouteOrch, nullptr);
        gFlowCounterRouteOrch = new FlowCounterRouteOrch(m_config_db.get(), route_pattern_tables);
        gDirectory.set(gFlowCounterRouteOrch);

        // Populate APP_PORT_TABLE using SAI default ports (similar to routeorch_ut / portsorch_ut)
        Table portTable = Table(m_app_db.get(), APP_PORT_TABLE_NAME);
        auto ports = ut_helper::getInitialSaiPorts();
        for (const auto &it : ports)
        {
            portTable.set(it.first, it.second);
            portTable.set(it.first, {{ "oper_status", "up" }});
        }

        // First, indicate that port configuration is done and let PortsOrch process it.
        portTable.set("PortConfigDone", { { "count", to_string(ports.size()) } });
        gPortsOrch->addExistingData(&portTable);
        static_cast<Orch *>(gPortsOrch)->doTask();

        // Let BufferOrch see the populated ports (keeps UT behavior closer to production).
        gBufferOrch->addExistingData(&portTable);
        static_cast<Orch *>(gBufferOrch)->doTask();

        // Finally, send PortInitDone so gPortsOrch->allPortsReady() becomes true, allowing
        // IntfsOrch/NeighOrch/RouteOrch to process their APP_* tables.
        portTable.set("PortInitDone", { { "lanes", "0" } });
        gPortsOrch->addExistingData(&portTable);
        static_cast<Orch *>(gPortsOrch)->doTask();

        // Minimal VRF/route/neigh/fdb orchs used by TamOrch
        ASSERT_EQ(gVrfOrch, nullptr);
        gVrfOrch = new VRFOrch(m_app_db.get(), APP_VRF_TABLE_NAME,
                               m_state_db.get(), STATE_VRF_OBJECT_TABLE_NAME);

        ASSERT_EQ(gIntfsOrch, nullptr);
        gIntfsOrch = new IntfsOrch(m_app_db.get(), APP_INTF_TABLE_NAME,
                                   gVrfOrch, m_chassis_app_db.get());

        ASSERT_EQ(gFdbOrch, nullptr);
        vector<table_name_with_pri_t> fdb_tables = {
            { APP_FDB_TABLE_NAME,        FdbOrch::fdborch_pri },
            { APP_VXLAN_FDB_TABLE_NAME,  FdbOrch::fdborch_pri },
            { APP_MCLAG_FDB_TABLE_NAME,  20 }
        };
        TableConnector stateDbFdb(m_state_db.get(), STATE_FDB_TABLE_NAME);
        TableConnector stateMclagDbFdb(m_state_db.get(), STATE_MCLAG_REMOTE_FDB_TABLE_NAME);
        gFdbOrch = new FdbOrch(m_app_db.get(), fdb_tables, stateDbFdb, stateMclagDbFdb, gPortsOrch);

        ASSERT_EQ(gNeighOrch, nullptr);
        gNeighOrch = new NeighOrch(m_app_db.get(), APP_NEIGH_TABLE_NAME,
                                   gIntfsOrch, gFdbOrch, gPortsOrch, m_chassis_app_db.get());

        // Minimal MuxOrch plumbing so NeighOrch::addNeighbor() can safely
        // query mux_orch->isNeighborActive(). With no MUX cables configured,
        // mux_cable_tb_ stays empty and all neighbors are treated as active.
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
        MuxOrch *mux_orch = new MuxOrch(m_config_db.get(), mux_tables, gTunneldecapOrch, gNeighOrch, gFdbOrch);
        gDirectory.set(mux_orch);

        // Minimal FgNhgOrch so NeighOrch can safely notify it about nexthops.
        // With no FG_NHG configuration programmed in this test, it will simply
        // see an empty state and return quickly.
        ASSERT_EQ(gFgNhgOrch, nullptr);
        const int fgnhgorch_pri = 15;
        vector<table_name_with_pri_t> fgnhg_tables = {
            { CFG_FG_NHG,        fgnhgorch_pri },
            { CFG_FG_NHG_PREFIX, fgnhgorch_pri },
            { CFG_FG_NHG_MEMBER, fgnhgorch_pri }
        };
        gFgNhgOrch = new FgNhgOrch(m_config_db.get(), m_app_db.get(), m_state_db.get(), fgnhg_tables,
                                   gNeighOrch, gIntfsOrch, gVrfOrch);

        ASSERT_EQ(gRouteOrch, nullptr);
        const int routeorch_pri = 5;
        vector<table_name_with_pri_t> route_tables = {
            { APP_ROUTE_TABLE_NAME,       routeorch_pri },
            { APP_LABEL_ROUTE_TABLE_NAME, routeorch_pri }
        };
        gRouteOrch = new RouteOrch(m_app_db.get(), route_tables,
                                   gSwitchOrch, gNeighOrch, gIntfsOrch,
                                   gVrfOrch, gFgNhgOrch, gSrv6Orch);

        // Create TamOrch on CONFIG_DB tables
        vector<string> tam_tables = {
            CFG_TAM_TABLE_NAME,
            CFG_TAM_COLLECTOR_TABLE_NAME,
            CFG_TAM_FLOW_GROUP_TABLE_NAME,
            CFG_TAM_SESSION_TABLE_NAME
        };
        TableConnector stateDbTamTable(m_state_db.get(), STATE_TAM_DROP_MONITOR_SESSION_TABLE_NAME);
        m_tam_orch = make_unique<TamOrch>(m_config_db.get(), tam_tables, stateDbTamTable,
                                          gAclOrch, gPortsOrch, gVrfOrch,
                                          gRouteOrch, gNeighOrch, gFdbOrch);

        // Reset counters and hook SAI TAM related APIs
        g_tam_create_count = g_tam_remove_count = 0;
        g_switch_bind_tam_count = g_switch_unbind_tam_count = 0;
        g_port_bind_tam_count = g_port_unbind_tam_count = 0;
        hook_sai_tam_apis();
    }

    void TearDown() override
    {
        unhook_sai_tam_apis();

        ::testing_db::reset();

        gDirectory.m_values.clear();

        delete gCrmOrch;
        gCrmOrch = nullptr;

        delete gSwitchOrch;
        gSwitchOrch = nullptr;

        delete gVrfOrch;
        gVrfOrch = nullptr;

        delete gIntfsOrch;
        gIntfsOrch = nullptr;

        delete gNeighOrch;
        gNeighOrch = nullptr;

        delete gTunneldecapOrch;
        gTunneldecapOrch = nullptr;

        delete gFdbOrch;
        gFdbOrch = nullptr;

        delete gFgNhgOrch;
        gFgNhgOrch = nullptr;

        delete gRouteOrch;
        gRouteOrch = nullptr;

        delete gFlowCounterRouteOrch;
        gFlowCounterRouteOrch = nullptr;

        delete gPortsOrch;
        gPortsOrch = nullptr;

        delete gBufferOrch;
        gBufferOrch = nullptr;

        ut_helper::uninitSaiApi();
    }

    // Helper to push entries into a given config table and run TamOrch::doTask
    void processConfigEntries(const string &tableName,
                              const deque<KeyOpFieldsValuesTuple> &entries)
    {
        auto consumer = dynamic_cast<Consumer *>(m_tam_orch->getExecutor(tableName));
        ASSERT_NE(consumer, nullptr);
        consumer->addToSync(entries);
        static_cast<Orch *>(m_tam_orch.get())->doTask(*consumer);
    }

    std::shared_ptr<TamCollectorEntry> getCollector(const std::string &name)
    {
        auto it = m_tam_orch->m_collectorTables.find(name);
        if (it == m_tam_orch->m_collectorTables.end())
        {
            return nullptr;
        }
        return it->second;
    }

    std::shared_ptr<TamSessionEntry> getSession(const std::string &name)
    {
        auto it = m_tam_orch->m_sessionTables.find(name);
        if (it == m_tam_orch->m_sessionTables.end())
        {
            return nullptr;
        }
        return it->second;
    }

    // Helper to get session state from STATE_DB
    bool getStateDbSessionEntry(const std::string &sessionName,
                                std::vector<FieldValueTuple> &fvs)
    {
        Table stateTable(m_state_db.get(), STATE_TAM_DROP_MONITOR_SESSION_TABLE_NAME);
        return stateTable.get(sessionName, fvs);
    }

    // Helper to get a specific field value from STATE_DB session entry
    std::string getStateDbSessionField(const std::string &sessionName,
                                       const std::string &fieldName)
    {
        std::vector<FieldValueTuple> fvs;
        if (!getStateDbSessionEntry(sessionName, fvs))
        {
            return "";
        }
        for (const auto &fv : fvs)
        {
            if (fvField(fv) == fieldName)
            {
                return fvValue(fv);
            }
        }
        return "";
    }

    // Helper to check if session exists in STATE_DB
    bool sessionExistsInStateDb(const std::string &sessionName)
    {
        std::vector<FieldValueTuple> fvs;
        return getStateDbSessionEntry(sessionName, fvs);
    }
};

TEST_F(TamOrchFlowUnawareModTest, FlowUnawareDropMonitorSessionBasic)
{
    // 1) Configure a collector with valid IPs/port and default VRF
    deque<KeyOpFieldsValuesTuple> collector_entries;
    vector<FieldValueTuple> collector_fvs = {
        { COLLECTOR_SRC_IP, "10.0.0.1" },
        { COLLECTOR_DST_IP, "10.0.0.10" },
        { COLLECTOR_L4_DST_PORT, "4739" },
        { COLLECTOR_DSCP_VALUE, "16" },
        { COLLECTOR_VRF, "default" },
    };
    collector_entries.emplace_back("COLLECTOR0", SET_COMMAND, collector_fvs);
    processConfigEntries(CFG_TAM_COLLECTOR_TABLE_NAME, collector_entries);

    auto collectorEntry = getCollector("COLLECTOR0");
    ASSERT_NE(collectorEntry, nullptr);

    // At this point we should have exactly one collector entry with refCount == 0.
    EXPECT_EQ(m_tam_orch->m_collectorTables.size(), 1u);
    EXPECT_EQ(collectorEntry->refCount, 0);

    // 2) Program APP_INTF and APP_NEIGH entries so that IntfsOrch/NeighOrch
    //    can resolve the collector destination IP via the real observer path.
    Table intfTable(m_app_db.get(), APP_INTF_TABLE_NAME);
    intfTable.set("Ethernet0", { {"NULL", "NULL"},
                                  {"mac_addr", "00:00:00:00:00:00"} });
    intfTable.set("Ethernet0:10.0.0.1/24", { {"scope", "global"},
                                              {"family", "IPv4"} });
    gIntfsOrch->addExistingData(&intfTable);
    static_cast<Orch *>(gIntfsOrch)->doTask();

    Table neighborTable(m_app_db.get(), APP_NEIGH_TABLE_NAME);
    neighborTable.set("Ethernet0:10.0.0.10", { {"neigh", "00:00:0a:00:00:10"},
                                                {"family", "IPv4"} });
    gNeighOrch->addExistingData(&neighborTable);
    static_cast<Orch *>(gNeighOrch)->doTask();

    // TamOrch is attached to NeighOrch as an observer. The neighbor update
    // above should drive getNeighborInfo()/updateCollector(), which in turn
    // calls tamTransportCreate()/tamCollectorCreate() and populates the
    // collector/transport state just as in production.
    collectorEntry = getCollector("COLLECTOR0");
    ASSERT_NE(collectorEntry, nullptr);

    EXPECT_TRUE(collectorEntry->resolved);
    EXPECT_NE(collectorEntry->neighborInfo.portId, SAI_NULL_OBJECT_ID);
    EXPECT_NE(collectorEntry->collectorObjId, SAI_NULL_OBJECT_ID);
    EXPECT_NE(collectorEntry->transportObjId, SAI_NULL_OBJECT_ID);

    EXPECT_EQ(m_tam_orch->m_transportTables.size(), 1u);
    auto transportEntry = m_tam_orch->m_transportTables.begin()->second;
    ASSERT_NE(transportEntry, nullptr);
    EXPECT_GE(transportEntry->refCount, 1);

    const int baseline_collector_refcount = collectorEntry->refCount;
    const int baseline_transport_refcount = transportEntry->refCount;

    // 3) Configure a flow-unaware drop-monitor session referencing COLLECTOR0
    deque<KeyOpFieldsValuesTuple> session_entries;
    vector<FieldValueTuple> session_fvs = {
        { SESSION_TYPE, SESSION_TYPE_DROP_MONITOR },
        { SESSION_REPORT_TYPE, SESSION_REPORT_TYPE_IPFIX },
        { SESSION_COLLECTOR, "COLLECTOR0" },
        // No SESSION_FLOW_GROUP => flow-unaware
    };
    session_entries.emplace_back("DROP_SESSION0", SET_COMMAND, session_fvs);
    processConfigEntries(CFG_TAM_SESSION_TABLE_NAME, session_entries);

    auto session = getSession("DROP_SESSION0");
    ASSERT_NE(session, nullptr);
    ASSERT_TRUE(session->active);

    // Verify add/bind behavior
    EXPECT_GE(g_tam_create_count, 1);
    EXPECT_EQ(g_switch_bind_tam_count, 1);
    EXPECT_GE(g_port_bind_tam_count, 1);

    // Collector/transport refcounts should have increased due to
    // tamDropEventCreate() and handleTamCreate() paths.
    EXPECT_GT(collectorEntry->refCount, baseline_collector_refcount);
    EXPECT_EQ(m_tam_orch->m_transportTables.size(), 1u);
    EXPECT_GE(transportEntry->refCount, baseline_transport_refcount);

    // 4) Delete the session and verify unbind/remove and refcount rollback
    deque<KeyOpFieldsValuesTuple> del_entries;
    del_entries.emplace_back("DROP_SESSION0", DEL_COMMAND, vector<FieldValueTuple>{});
    processConfigEntries(CFG_TAM_SESSION_TABLE_NAME, del_entries);

    EXPECT_EQ(getSession("DROP_SESSION0"), nullptr);
    EXPECT_GE(g_tam_remove_count, 1);
    EXPECT_EQ(g_switch_unbind_tam_count, 1);
    EXPECT_GE(g_port_unbind_tam_count, 1);

    // After deleting the session, collector/transport refcounts should have
    // returned to their baseline values.
    EXPECT_EQ(collectorEntry->refCount, baseline_collector_refcount);
    EXPECT_EQ(transportEntry->refCount, baseline_transport_refcount);

    // 5) Finally delete the collector itself and ensure all internal tables are
    //    cleaned up and refcounts drop to zero.
    deque<KeyOpFieldsValuesTuple> collector_del_entries;
    collector_del_entries.emplace_back("COLLECTOR0", DEL_COMMAND, vector<FieldValueTuple>{});
    processConfigEntries(CFG_TAM_COLLECTOR_TABLE_NAME, collector_del_entries);

    EXPECT_EQ(getCollector("COLLECTOR0"), nullptr);
    EXPECT_TRUE(m_tam_orch->m_collectorTables.empty());
    EXPECT_TRUE(m_tam_orch->m_transportTables.empty());
}

TEST_F(TamOrchFlowUnawareModTest, FlowUnawareDropMonitorCollectorFlap)
{
    // 1) Configure a collector in CONFIG_DB. It starts unresolved until
    //    NeighOrch learns the neighbor.
    deque<KeyOpFieldsValuesTuple> collector_entries;
    vector<FieldValueTuple> collector_fvs = {
        { COLLECTOR_SRC_IP, "10.0.0.1" },
        { COLLECTOR_DST_IP, "10.0.0.10" },
        { COLLECTOR_DSCP_VALUE, "8" },
        { COLLECTOR_L4_DST_PORT, "31337" },
        { COLLECTOR_VRF, "default" },
    };
    collector_entries.emplace_back("COLLECTOR0", SET_COMMAND, collector_fvs);
    processConfigEntries(CFG_TAM_COLLECTOR_TABLE_NAME, collector_entries);

    auto collectorEntry = getCollector("COLLECTOR0");
    ASSERT_NE(collectorEntry, nullptr);
    EXPECT_FALSE(collectorEntry->resolved);

    // 2) Program APP_INTF and APP_NEIGH so IntfsOrch/NeighOrch can resolve the
    //    collector destination IP and drive TamOrch via observer callbacks.
    Table intfTable(m_app_db.get(), APP_INTF_TABLE_NAME);
    intfTable.set("Ethernet0", { {"NULL", "NULL"},
                                  {"mac_addr", "00:00:00:00:00:00"} });
    intfTable.set("Ethernet0:10.0.0.1/24", { {"scope", "global"},
                                              {"family", "IPv4"} });
    gIntfsOrch->addExistingData(&intfTable);
    static_cast<Orch *>(gIntfsOrch)->doTask();

    Table neighborTable(m_app_db.get(), APP_NEIGH_TABLE_NAME);
    neighborTable.set("Ethernet0:10.0.0.10",
                      { {"neigh", "00:00:0a:00:00:10"}, {"family", "IPv4"} });
    gNeighOrch->addExistingData(&neighborTable);
    static_cast<Orch *>(gNeighOrch)->doTask();

    collectorEntry = getCollector("COLLECTOR0");
    ASSERT_NE(collectorEntry, nullptr);
    EXPECT_TRUE(collectorEntry->resolved);
    EXPECT_NE(collectorEntry->neighborInfo.portId, SAI_NULL_OBJECT_ID);
    EXPECT_NE(collectorEntry->collectorObjId, SAI_NULL_OBJECT_ID);
    EXPECT_NE(collectorEntry->transportObjId, SAI_NULL_OBJECT_ID);

    ASSERT_EQ(m_tam_orch->m_transportTables.size(), 1u);
    auto transportEntry = m_tam_orch->m_transportTables.begin()->second;
    ASSERT_NE(transportEntry, nullptr);

    const int baseline_collector_refcount = collectorEntry->refCount;
    const int baseline_transport_refcount = transportEntry->refCount;

    // 3) Create a flow-unaware drop-monitor session that uses this collector.
    deque<KeyOpFieldsValuesTuple> session_entries;
    vector<FieldValueTuple> session_fvs = {
        { SESSION_TYPE, SESSION_TYPE_DROP_MONITOR },
        { SESSION_REPORT_TYPE, SESSION_REPORT_TYPE_IPFIX },
        { SESSION_COLLECTOR, "COLLECTOR0" },
    };
    session_entries.emplace_back("DROP_SESSION0", SET_COMMAND, session_fvs);
    processConfigEntries(CFG_TAM_SESSION_TABLE_NAME, session_entries);

    auto session = getSession("DROP_SESSION0");
    ASSERT_NE(session, nullptr);
    ASSERT_TRUE(session->active);
    EXPECT_NE(session->dropSession.tamObjId, SAI_NULL_OBJECT_ID);
    EXPECT_NE(session->dropSession.ingressEventObjId, SAI_NULL_OBJECT_ID);
    EXPECT_NE(session->dropSession.egressEventObjId, SAI_NULL_OBJECT_ID);
    EXPECT_NE(session->dropSession.tmEventObjId, SAI_NULL_OBJECT_ID);

    const int tam_create_before_flap = g_tam_create_count;
    const int tam_remove_before_flap = g_tam_remove_count;
    const int switch_bind_before_flap = g_switch_bind_tam_count;
    const int port_bind_before_flap = g_port_bind_tam_count;

    // 4) Simulate collector going from resolved -> unresolved by sending a
    //    DEL to APP_NEIGH. This should cause TamOrch to tear down the session
    //    and associated TAM objects.
    neighborTable.del("Ethernet0:10.0.0.10");

    deque<KeyOpFieldsValuesTuple> neigh_del_entries;
    neigh_del_entries.emplace_back("Ethernet0:10.0.0.10", DEL_COMMAND, vector<FieldValueTuple>{});
    auto neigh_consumer = dynamic_cast<Consumer *>(gNeighOrch->getExecutor(APP_NEIGH_TABLE_NAME));
    ASSERT_NE(neigh_consumer, nullptr);
    neigh_consumer->addToSync(neigh_del_entries);
    static_cast<Orch *>(gNeighOrch)->doTask(*neigh_consumer);

    collectorEntry = getCollector("COLLECTOR0");
    ASSERT_NE(collectorEntry, nullptr);
    EXPECT_FALSE(collectorEntry->resolved);
    EXPECT_EQ(collectorEntry->collectorObjId, SAI_NULL_OBJECT_ID);
    EXPECT_EQ(collectorEntry->transportObjId, SAI_NULL_OBJECT_ID);

    session = getSession("DROP_SESSION0");
    ASSERT_NE(session, nullptr);
    EXPECT_FALSE(session->active);
    EXPECT_EQ(session->dropSession.tamObjId, SAI_NULL_OBJECT_ID);
    EXPECT_EQ(session->dropSession.ingressEventObjId, SAI_NULL_OBJECT_ID);
    EXPECT_EQ(session->dropSession.egressEventObjId, SAI_NULL_OBJECT_ID);
    EXPECT_EQ(session->dropSession.tmEventObjId, SAI_NULL_OBJECT_ID);

    EXPECT_GT(g_tam_remove_count, tam_remove_before_flap);
    EXPECT_GE(g_switch_unbind_tam_count, 1);
    EXPECT_GE(g_port_unbind_tam_count, 1);
    EXPECT_TRUE(m_tam_orch->m_transportTables.empty());

    // 5) Simulate collector becoming resolved again by re-adding the neighbor.
    //    TamOrch should recreate the collector/transport and re-activate the
    //    drop-monitor session.
    neighborTable.set("Ethernet0:10.0.0.10",
                      { {"neigh", "00:00:0a:00:00:10"}, {"family", "IPv4"} });
    gNeighOrch->addExistingData(&neighborTable);
    static_cast<Orch *>(gNeighOrch)->doTask();

    collectorEntry = getCollector("COLLECTOR0");
    ASSERT_NE(collectorEntry, nullptr);
    EXPECT_TRUE(collectorEntry->resolved);
    EXPECT_NE(collectorEntry->neighborInfo.portId, SAI_NULL_OBJECT_ID);
    EXPECT_NE(collectorEntry->collectorObjId, SAI_NULL_OBJECT_ID);
    EXPECT_NE(collectorEntry->transportObjId, SAI_NULL_OBJECT_ID);

    ASSERT_EQ(m_tam_orch->m_transportTables.size(), 1u);
    transportEntry = m_tam_orch->m_transportTables.begin()->second;
    ASSERT_NE(transportEntry, nullptr);

    EXPECT_GE(collectorEntry->refCount, baseline_collector_refcount);
    EXPECT_GE(transportEntry->refCount, baseline_transport_refcount);

    session = getSession("DROP_SESSION0");
    ASSERT_NE(session, nullptr);
    EXPECT_TRUE(session->active);
    EXPECT_NE(session->dropSession.tamObjId, SAI_NULL_OBJECT_ID);
    EXPECT_NE(session->dropSession.ingressEventObjId, SAI_NULL_OBJECT_ID);
    EXPECT_NE(session->dropSession.egressEventObjId, SAI_NULL_OBJECT_ID);
    EXPECT_NE(session->dropSession.tmEventObjId, SAI_NULL_OBJECT_ID);

    EXPECT_GT(g_tam_create_count, tam_create_before_flap);
    EXPECT_GE(g_switch_bind_tam_count, switch_bind_before_flap + 1);
    EXPECT_GE(g_port_bind_tam_count, port_bind_before_flap + 1);

    // 6) Clean up: delete the session and collector to keep the fixture state
    //    consistent with the basic test.
    deque<KeyOpFieldsValuesTuple> del_entries;
    del_entries.emplace_back("DROP_SESSION0", DEL_COMMAND, vector<FieldValueTuple>{});
    processConfigEntries(CFG_TAM_SESSION_TABLE_NAME, del_entries);
    EXPECT_EQ(getSession("DROP_SESSION0"), nullptr);

    deque<KeyOpFieldsValuesTuple> collector_del_entries;
    collector_del_entries.emplace_back("COLLECTOR0", DEL_COMMAND, vector<FieldValueTuple>{});
    processConfigEntries(CFG_TAM_COLLECTOR_TABLE_NAME, collector_del_entries);

    EXPECT_EQ(getCollector("COLLECTOR0"), nullptr);
    EXPECT_TRUE(m_tam_orch->m_collectorTables.empty());
    EXPECT_TRUE(m_tam_orch->m_transportTables.empty());
}

// Test partial update support for TAM device table: updating only one field preserves others
TEST_F(TamOrchFlowUnawareModTest, DeviceTablePartialUpdate)
{
    // 1) Configure initial device settings with both device-id and enterprise-id
    deque<KeyOpFieldsValuesTuple> device_entries;
    vector<FieldValueTuple> device_fvs = {
        { "device-id", "100" },
        { "enterprise-id", "200" },
        { "ifa", "enabled" },
    };
    device_entries.emplace_back("device", SET_COMMAND, device_fvs);
    processConfigEntries(CFG_TAM_TABLE_NAME, device_entries);

    // Verify all settings are stored
    EXPECT_EQ(m_tam_orch->globalSettings["device-id"], "100");
    EXPECT_EQ(m_tam_orch->globalSettings["enterprise-id"], "200");
    EXPECT_EQ(m_tam_orch->globalSettings["ifa"], "enabled");

    // 2) Update only device-id - enterprise-id should be preserved
    deque<KeyOpFieldsValuesTuple> partial_entries;
    vector<FieldValueTuple> partial_fvs = {
        { "device-id", "150" },
    };
    partial_entries.emplace_back("device", SET_COMMAND, partial_fvs);
    processConfigEntries(CFG_TAM_TABLE_NAME, partial_entries);

    // Verify device-id changed but enterprise-id and ifa are preserved
    EXPECT_EQ(m_tam_orch->globalSettings["device-id"], "150");
    EXPECT_EQ(m_tam_orch->globalSettings["enterprise-id"], "200");
    EXPECT_EQ(m_tam_orch->globalSettings["ifa"], "enabled");

    // 3) Update only enterprise-id - device-id and ifa should be preserved
    deque<KeyOpFieldsValuesTuple> partial_entries2;
    vector<FieldValueTuple> partial_fvs2 = {
        { "enterprise-id", "300" },
    };
    partial_entries2.emplace_back("device", SET_COMMAND, partial_fvs2);
    processConfigEntries(CFG_TAM_TABLE_NAME, partial_entries2);

    // Verify enterprise-id changed but device-id and ifa are preserved
    EXPECT_EQ(m_tam_orch->globalSettings["device-id"], "150");
    EXPECT_EQ(m_tam_orch->globalSettings["enterprise-id"], "300");
    EXPECT_EQ(m_tam_orch->globalSettings["ifa"], "enabled");

    // 4) DEL should clear all settings
    deque<KeyOpFieldsValuesTuple> del_entries;
    del_entries.emplace_back("device", DEL_COMMAND, vector<FieldValueTuple>{});
    processConfigEntries(CFG_TAM_TABLE_NAME, del_entries);

    EXPECT_TRUE(m_tam_orch->globalSettings.empty());
}

// Test that device-id/enterprise-id changes trigger session recreation
TEST_F(TamOrchFlowUnawareModTest, DeviceTableChangeRecreatesSessions)
{
    // 1) Configure initial device settings
    deque<KeyOpFieldsValuesTuple> device_entries;
    vector<FieldValueTuple> device_fvs = {
        { "device-id", "100" },
        { "enterprise-id", "200" },
    };
    device_entries.emplace_back("device", SET_COMMAND, device_fvs);
    processConfigEntries(CFG_TAM_TABLE_NAME, device_entries);

    // 2) Configure a collector with valid IPs/port and default VRF
    deque<KeyOpFieldsValuesTuple> collector_entries;
    vector<FieldValueTuple> collector_fvs = {
        { COLLECTOR_SRC_IP, "10.0.0.1" },
        { COLLECTOR_DST_IP, "10.0.0.10" },
        { COLLECTOR_L4_DST_PORT, "4739" },
        { COLLECTOR_DSCP_VALUE, "16" },
        { COLLECTOR_VRF, "default" },
    };
    collector_entries.emplace_back("COLLECTOR0", SET_COMMAND, collector_fvs);
    processConfigEntries(CFG_TAM_COLLECTOR_TABLE_NAME, collector_entries);

    // 3) Program APP_INTF and APP_NEIGH entries to resolve the collector
    Table intfTable(m_app_db.get(), APP_INTF_TABLE_NAME);
    intfTable.set("Ethernet0", { {"NULL", "NULL"},
                                  {"mac_addr", "00:00:00:00:00:00"} });
    intfTable.set("Ethernet0:10.0.0.1/24", { {"scope", "global"},
                                              {"family", "IPv4"} });
    gIntfsOrch->addExistingData(&intfTable);
    static_cast<Orch *>(gIntfsOrch)->doTask();

    Table neighborTable(m_app_db.get(), APP_NEIGH_TABLE_NAME);
    neighborTable.set("Ethernet0:10.0.0.10", { {"neigh", "00:00:0a:00:00:10"},
                                                {"family", "IPv4"} });
    gNeighOrch->addExistingData(&neighborTable);
    static_cast<Orch *>(gNeighOrch)->doTask();

    // 4) Create a flow-unaware drop-monitor session
    deque<KeyOpFieldsValuesTuple> session_entries;
    vector<FieldValueTuple> session_fvs = {
        { SESSION_TYPE, SESSION_TYPE_DROP_MONITOR },
        { SESSION_REPORT_TYPE, SESSION_REPORT_TYPE_IPFIX },
        { SESSION_COLLECTOR, "COLLECTOR0" },
    };
    session_entries.emplace_back("DROP_SESSION0", SET_COMMAND, session_fvs);
    processConfigEntries(CFG_TAM_SESSION_TABLE_NAME, session_entries);

    auto session = getSession("DROP_SESSION0");
    ASSERT_NE(session, nullptr);
    ASSERT_TRUE(session->active);

    // Record SAI counters before device-id change
    int tam_create_before = g_tam_create_count;
    int tam_remove_before = g_tam_remove_count;
    int switch_bind_before = g_switch_bind_tam_count;
    int switch_unbind_before = g_switch_unbind_tam_count;

    // 5) Update device-id - this should trigger session recreation
    deque<KeyOpFieldsValuesTuple> device_update;
    vector<FieldValueTuple> device_update_fvs = {
        { "device-id", "999" },
    };
    device_update.emplace_back("device", SET_COMMAND, device_update_fvs);
    processConfigEntries(CFG_TAM_TABLE_NAME, device_update);

    // Verify device-id was updated and enterprise-id preserved
    EXPECT_EQ(m_tam_orch->globalSettings["device-id"], "999");
    EXPECT_EQ(m_tam_orch->globalSettings["enterprise-id"], "200");

    // Session should still be active after recreation
    session = getSession("DROP_SESSION0");
    ASSERT_NE(session, nullptr);
    EXPECT_TRUE(session->active);

    // Verify TAM objects were removed and recreated (session recreation)
    EXPECT_GT(g_tam_remove_count, tam_remove_before);
    EXPECT_GT(g_tam_create_count, tam_create_before);
    EXPECT_GT(g_switch_unbind_tam_count, switch_unbind_before);
    EXPECT_GT(g_switch_bind_tam_count, switch_bind_before);

    // 6) Update enterprise-id - this should also trigger session recreation
    tam_create_before = g_tam_create_count;
    tam_remove_before = g_tam_remove_count;
    switch_bind_before = g_switch_bind_tam_count;
    switch_unbind_before = g_switch_unbind_tam_count;

    deque<KeyOpFieldsValuesTuple> device_update2;
    vector<FieldValueTuple> device_update_fvs2 = {
        { "enterprise-id", "888" },
    };
    device_update2.emplace_back("device", SET_COMMAND, device_update_fvs2);
    processConfigEntries(CFG_TAM_TABLE_NAME, device_update2);

    // Verify enterprise-id was updated and device-id preserved
    EXPECT_EQ(m_tam_orch->globalSettings["device-id"], "999");
    EXPECT_EQ(m_tam_orch->globalSettings["enterprise-id"], "888");

    // Session should still be active after recreation
    session = getSession("DROP_SESSION0");
    ASSERT_NE(session, nullptr);
    EXPECT_TRUE(session->active);

    // Verify TAM objects were removed and recreated (session recreation)
    EXPECT_GT(g_tam_remove_count, tam_remove_before);
    EXPECT_GT(g_tam_create_count, tam_create_before);
    EXPECT_GT(g_switch_unbind_tam_count, switch_unbind_before);
    EXPECT_GT(g_switch_bind_tam_count, switch_bind_before);

    // 7) Clean up
    deque<KeyOpFieldsValuesTuple> del_entries;
    del_entries.emplace_back("DROP_SESSION0", DEL_COMMAND, vector<FieldValueTuple>{});
    processConfigEntries(CFG_TAM_SESSION_TABLE_NAME, del_entries);
    EXPECT_EQ(getSession("DROP_SESSION0"), nullptr);

    deque<KeyOpFieldsValuesTuple> collector_del_entries;
    collector_del_entries.emplace_back("COLLECTOR0", DEL_COMMAND, vector<FieldValueTuple>{});
    processConfigEntries(CFG_TAM_COLLECTOR_TABLE_NAME, collector_del_entries);

    deque<KeyOpFieldsValuesTuple> device_del_entries;
    device_del_entries.emplace_back("device", DEL_COMMAND, vector<FieldValueTuple>{});
    processConfigEntries(CFG_TAM_TABLE_NAME, device_del_entries);
}

// Test that unchanged device-id/enterprise-id does NOT trigger session recreation
TEST_F(TamOrchFlowUnawareModTest, SameDeviceTableDoesNotRecreateSessions)
{
    // 1) Configure initial device settings
    deque<KeyOpFieldsValuesTuple> device_entries;
    vector<FieldValueTuple> device_fvs = {
        { "device-id", "100" },
        { "enterprise-id", "200" },
    };
    device_entries.emplace_back("device", SET_COMMAND, device_fvs);
    processConfigEntries(CFG_TAM_TABLE_NAME, device_entries);

    // 2) Configure a collector with valid IPs/port and default VRF
    deque<KeyOpFieldsValuesTuple> collector_entries;
    vector<FieldValueTuple> collector_fvs = {
        { COLLECTOR_SRC_IP, "10.0.0.1" },
        { COLLECTOR_DST_IP, "10.0.0.10" },
        { COLLECTOR_L4_DST_PORT, "4739" },
        { COLLECTOR_DSCP_VALUE, "16" },
        { COLLECTOR_VRF, "default" },
    };
    collector_entries.emplace_back("COLLECTOR0", SET_COMMAND, collector_fvs);
    processConfigEntries(CFG_TAM_COLLECTOR_TABLE_NAME, collector_entries);

    // 3) Program APP_INTF and APP_NEIGH entries to resolve the collector
    Table intfTable(m_app_db.get(), APP_INTF_TABLE_NAME);
    intfTable.set("Ethernet0", { {"NULL", "NULL"},
                                  {"mac_addr", "00:00:00:00:00:00"} });
    intfTable.set("Ethernet0:10.0.0.1/24", { {"scope", "global"},
                                              {"family", "IPv4"} });
    gIntfsOrch->addExistingData(&intfTable);
    static_cast<Orch *>(gIntfsOrch)->doTask();

    Table neighborTable(m_app_db.get(), APP_NEIGH_TABLE_NAME);
    neighborTable.set("Ethernet0:10.0.0.10", { {"neigh", "00:00:0a:00:00:10"},
                                                {"family", "IPv4"} });
    gNeighOrch->addExistingData(&neighborTable);
    static_cast<Orch *>(gNeighOrch)->doTask();

    // 4) Create a flow-unaware drop-monitor session
    deque<KeyOpFieldsValuesTuple> session_entries;
    vector<FieldValueTuple> session_fvs = {
        { SESSION_TYPE, SESSION_TYPE_DROP_MONITOR },
        { SESSION_REPORT_TYPE, SESSION_REPORT_TYPE_IPFIX },
        { SESSION_COLLECTOR, "COLLECTOR0" },
    };
    session_entries.emplace_back("DROP_SESSION0", SET_COMMAND, session_fvs);
    processConfigEntries(CFG_TAM_SESSION_TABLE_NAME, session_entries);

    auto session = getSession("DROP_SESSION0");
    ASSERT_NE(session, nullptr);
    ASSERT_TRUE(session->active);

    // Record SAI counters before "update"
    int tam_create_before = g_tam_create_count;
    int tam_remove_before = g_tam_remove_count;

    // 5) Send the same device-id value - should NOT trigger recreation
    {
        deque<KeyOpFieldsValuesTuple> device_update;
        vector<FieldValueTuple> device_update_fvs = {
            { "device-id", "100" },  // Same value as before
        };
        device_update.emplace_back("device", SET_COMMAND, device_update_fvs);
        processConfigEntries(CFG_TAM_TABLE_NAME, device_update);

        // Session should still be active and TAM objects should NOT have been recreated
        session = getSession("DROP_SESSION0");
        ASSERT_NE(session, nullptr);
        EXPECT_TRUE(session->active);

        // No TAM remove/create should have happened
        EXPECT_EQ(g_tam_remove_count, tam_remove_before);
        EXPECT_EQ(g_tam_create_count, tam_create_before);
    }

    // 6) Send the same enterprise-id value - should NOT trigger recreation
    tam_create_before = g_tam_create_count;
    tam_remove_before = g_tam_remove_count;

    {
        deque<KeyOpFieldsValuesTuple> device_update;
        vector<FieldValueTuple> device_update_fvs = {
            { "enterprise-id", "200" },  // Same value as before
        };
        device_update.emplace_back("device", SET_COMMAND, device_update_fvs);
        processConfigEntries(CFG_TAM_TABLE_NAME, device_update);

        // Session should still be active and TAM objects should NOT have been recreated
        session = getSession("DROP_SESSION0");
        ASSERT_NE(session, nullptr);
        EXPECT_TRUE(session->active);

        // No TAM remove/create should have happened
        EXPECT_EQ(g_tam_remove_count, tam_remove_before);
        EXPECT_EQ(g_tam_create_count, tam_create_before);
    }

    // 7) Clean up
    deque<KeyOpFieldsValuesTuple> del_entries;
    del_entries.emplace_back("DROP_SESSION0", DEL_COMMAND, vector<FieldValueTuple>{});
    processConfigEntries(CFG_TAM_SESSION_TABLE_NAME, del_entries);

    deque<KeyOpFieldsValuesTuple> collector_del_entries;
    collector_del_entries.emplace_back("COLLECTOR0", DEL_COMMAND, vector<FieldValueTuple>{});
    processConfigEntries(CFG_TAM_COLLECTOR_TABLE_NAME, collector_del_entries);

    deque<KeyOpFieldsValuesTuple> device_del_entries;
    device_del_entries.emplace_back("device", DEL_COMMAND, vector<FieldValueTuple>{});
    processConfigEntries(CFG_TAM_TABLE_NAME, device_del_entries);
}

// Test that no active sessions means no recreation is attempted
TEST_F(TamOrchFlowUnawareModTest, DeviceTableChangeNoActiveSessions)
{
    // 1) Configure initial device settings
    deque<KeyOpFieldsValuesTuple> device_entries;
    vector<FieldValueTuple> device_fvs = {
        { "device-id", "100" },
        { "enterprise-id", "200" },
    };
    device_entries.emplace_back("device", SET_COMMAND, device_fvs);
    processConfigEntries(CFG_TAM_TABLE_NAME, device_entries);

    // Record SAI counters - no sessions exist
    int tam_create_before = g_tam_create_count;
    int tam_remove_before = g_tam_remove_count;

    // 2) Update device-id - no sessions to recreate
    {
        deque<KeyOpFieldsValuesTuple> device_update;
        vector<FieldValueTuple> device_update_fvs = {
            { "device-id", "999" },
        };
        device_update.emplace_back("device", SET_COMMAND, device_update_fvs);
        processConfigEntries(CFG_TAM_TABLE_NAME, device_update);

        // Verify device-id was updated
        EXPECT_EQ(m_tam_orch->globalSettings["device-id"], "999");
        EXPECT_EQ(m_tam_orch->globalSettings["enterprise-id"], "200");

        // No TAM operations should have occurred since no sessions exist
        EXPECT_EQ(g_tam_remove_count, tam_remove_before);
        EXPECT_EQ(g_tam_create_count, tam_create_before);
    }

    // 3) Update enterprise-id - no sessions to recreate
    tam_create_before = g_tam_create_count;
    tam_remove_before = g_tam_remove_count;

    {
        deque<KeyOpFieldsValuesTuple> device_update;
        vector<FieldValueTuple> device_update_fvs = {
            { "enterprise-id", "888" },
        };
        device_update.emplace_back("device", SET_COMMAND, device_update_fvs);
        processConfigEntries(CFG_TAM_TABLE_NAME, device_update);

        // Verify enterprise-id was updated
        EXPECT_EQ(m_tam_orch->globalSettings["device-id"], "999");
        EXPECT_EQ(m_tam_orch->globalSettings["enterprise-id"], "888");

        // No TAM operations should have occurred since no sessions exist
        EXPECT_EQ(g_tam_remove_count, tam_remove_before);
        EXPECT_EQ(g_tam_create_count, tam_create_before);
    }

    // Clean up
    deque<KeyOpFieldsValuesTuple> device_del_entries;
    device_del_entries.emplace_back("device", DEL_COMMAND, vector<FieldValueTuple>{});
    processConfigEntries(CFG_TAM_TABLE_NAME, device_del_entries);
}

// Test STATE_DB is updated correctly when session becomes active
TEST_F(TamOrchFlowUnawareModTest, StateDbActiveSession)
{
    // 1) Configure a collector with valid IPs/port and default VRF
    deque<KeyOpFieldsValuesTuple> collector_entries;
    vector<FieldValueTuple> collector_fvs = {
        { COLLECTOR_SRC_IP, "10.0.0.1" },
        { COLLECTOR_DST_IP, "10.0.0.10" },
        { COLLECTOR_L4_DST_PORT, "4739" },
        { COLLECTOR_DSCP_VALUE, "16" },
        { COLLECTOR_VRF, "default" },
    };
    collector_entries.emplace_back("COLLECTOR0", SET_COMMAND, collector_fvs);
    processConfigEntries(CFG_TAM_COLLECTOR_TABLE_NAME, collector_entries);

    // 2) Program APP_INTF and APP_NEIGH to resolve the collector
    Table intfTable(m_app_db.get(), APP_INTF_TABLE_NAME);
    intfTable.set("Ethernet0", { {"NULL", "NULL"},
                                  {"mac_addr", "00:00:00:00:00:00"} });
    intfTable.set("Ethernet0:10.0.0.1/24", { {"scope", "global"},
                                              {"family", "IPv4"} });
    gIntfsOrch->addExistingData(&intfTable);
    static_cast<Orch *>(gIntfsOrch)->doTask();

    Table neighborTable(m_app_db.get(), APP_NEIGH_TABLE_NAME);
    neighborTable.set("Ethernet0:10.0.0.10", { {"neigh", "00:00:0a:00:00:10"},
                                                {"family", "IPv4"} });
    gNeighOrch->addExistingData(&neighborTable);
    static_cast<Orch *>(gNeighOrch)->doTask();

    // 3) Configure a drop-monitor session
    deque<KeyOpFieldsValuesTuple> session_entries;
    vector<FieldValueTuple> session_fvs = {
        { SESSION_TYPE, SESSION_TYPE_DROP_MONITOR },
        { SESSION_REPORT_TYPE, SESSION_REPORT_TYPE_IPFIX },
        { SESSION_COLLECTOR, "COLLECTOR0" },
    };
    session_entries.emplace_back("DROP_SESSION0", SET_COMMAND, session_fvs);
    processConfigEntries(CFG_TAM_SESSION_TABLE_NAME, session_entries);

    // 4) Verify session is active
    auto session = getSession("DROP_SESSION0");
    ASSERT_NE(session, nullptr);
    ASSERT_TRUE(session->active);

    // 5) Verify STATE_DB has correct values
    EXPECT_TRUE(sessionExistsInStateDb("DROP_SESSION0"));
    EXPECT_EQ(getStateDbSessionField("DROP_SESSION0", "status"), "active");
    EXPECT_EQ(getStateDbSessionField("DROP_SESSION0", "report_type"), "ipfix");
    EXPECT_EQ(getStateDbSessionField("DROP_SESSION0", "event_type"), "packet-drop-stateless");
    EXPECT_EQ(getStateDbSessionField("DROP_SESSION0", "collectors"), "COLLECTOR0");
    // drop_stages should include ingress, egress, mmu (default)
    std::string dropStages = getStateDbSessionField("DROP_SESSION0", "drop_stages");
    EXPECT_FALSE(dropStages.empty());
    // flow_group should be empty when not configured
    EXPECT_EQ(getStateDbSessionField("DROP_SESSION0", "flow_group"), "");
    // status_detail should not be present for active sessions
    EXPECT_EQ(getStateDbSessionField("DROP_SESSION0", "status_detail"), "");

    // 6) Delete session and verify STATE_DB entry is removed
    deque<KeyOpFieldsValuesTuple> del_entries;
    del_entries.emplace_back("DROP_SESSION0", DEL_COMMAND, vector<FieldValueTuple>{});
    processConfigEntries(CFG_TAM_SESSION_TABLE_NAME, del_entries);

    EXPECT_EQ(getSession("DROP_SESSION0"), nullptr);
    EXPECT_FALSE(sessionExistsInStateDb("DROP_SESSION0"));

    // Clean up collector
    deque<KeyOpFieldsValuesTuple> collector_del_entries;
    collector_del_entries.emplace_back("COLLECTOR0", DEL_COMMAND, vector<FieldValueTuple>{});
    processConfigEntries(CFG_TAM_COLLECTOR_TABLE_NAME, collector_del_entries);
}

// Test STATE_DB shows inactive status when collector is not reachable
TEST_F(TamOrchFlowUnawareModTest, StateDbInactiveCollectorNotReachable)
{
    // 1) Configure a collector but do NOT resolve it (no neighbor entry)
    deque<KeyOpFieldsValuesTuple> collector_entries;
    vector<FieldValueTuple> collector_fvs = {
        { COLLECTOR_SRC_IP, "10.0.0.1" },
        { COLLECTOR_DST_IP, "10.0.0.10" },
        { COLLECTOR_L4_DST_PORT, "4739" },
        { COLLECTOR_DSCP_VALUE, "16" },
        { COLLECTOR_VRF, "default" },
    };
    collector_entries.emplace_back("COLLECTOR0", SET_COMMAND, collector_fvs);
    processConfigEntries(CFG_TAM_COLLECTOR_TABLE_NAME, collector_entries);

    auto collectorEntry = getCollector("COLLECTOR0");
    ASSERT_NE(collectorEntry, nullptr);
    EXPECT_FALSE(collectorEntry->resolved);  // Collector is NOT resolved

    // 2) Configure a drop-monitor session - should fail validation
    deque<KeyOpFieldsValuesTuple> session_entries;
    vector<FieldValueTuple> session_fvs = {
        { SESSION_TYPE, SESSION_TYPE_DROP_MONITOR },
        { SESSION_REPORT_TYPE, SESSION_REPORT_TYPE_IPFIX },
        { SESSION_COLLECTOR, "COLLECTOR0" },
    };
    session_entries.emplace_back("DROP_SESSION0", SET_COMMAND, session_fvs);
    processConfigEntries(CFG_TAM_SESSION_TABLE_NAME, session_entries);

    // 3) Verify session exists but is NOT active
    auto session = getSession("DROP_SESSION0");
    ASSERT_NE(session, nullptr);
    EXPECT_FALSE(session->active);

    // 4) Verify STATE_DB shows inactive status with "Collector not reachable"
    EXPECT_TRUE(sessionExistsInStateDb("DROP_SESSION0"));
    EXPECT_EQ(getStateDbSessionField("DROP_SESSION0", "status"), "inactive");
    EXPECT_EQ(getStateDbSessionField("DROP_SESSION0", "status_detail"), "Collector not reachable");
    EXPECT_EQ(getStateDbSessionField("DROP_SESSION0", "report_type"), "ipfix");
    EXPECT_EQ(getStateDbSessionField("DROP_SESSION0", "collectors"), "COLLECTOR0");
    EXPECT_EQ(getStateDbSessionField("DROP_SESSION0", "flow_group"), "");

    // 5) Clean up
    deque<KeyOpFieldsValuesTuple> del_entries;
    del_entries.emplace_back("DROP_SESSION0", DEL_COMMAND, vector<FieldValueTuple>{});
    processConfigEntries(CFG_TAM_SESSION_TABLE_NAME, del_entries);

    deque<KeyOpFieldsValuesTuple> collector_del_entries;
    collector_del_entries.emplace_back("COLLECTOR0", DEL_COMMAND, vector<FieldValueTuple>{});
    processConfigEntries(CFG_TAM_COLLECTOR_TABLE_NAME, collector_del_entries);
}

// Test STATE_DB shows inactive with "Configuration error" when no collectors configured
TEST_F(TamOrchFlowUnawareModTest, StateDbInactiveConfigurationError)
{
    // 1) Configure a drop-monitor session WITHOUT any collector
    deque<KeyOpFieldsValuesTuple> session_entries;
    vector<FieldValueTuple> session_fvs = {
        { SESSION_TYPE, SESSION_TYPE_DROP_MONITOR },
        { SESSION_REPORT_TYPE, SESSION_REPORT_TYPE_IPFIX },
        // No SESSION_COLLECTOR - missing required config
    };
    session_entries.emplace_back("DROP_SESSION0", SET_COMMAND, session_fvs);
    processConfigEntries(CFG_TAM_SESSION_TABLE_NAME, session_entries);

    // 2) Verify session exists but is NOT active
    auto session = getSession("DROP_SESSION0");
    ASSERT_NE(session, nullptr);
    EXPECT_FALSE(session->active);

    // 3) Verify STATE_DB shows inactive status with "Configuration error"
    EXPECT_TRUE(sessionExistsInStateDb("DROP_SESSION0"));
    EXPECT_EQ(getStateDbSessionField("DROP_SESSION0", "status"), "inactive");
    EXPECT_EQ(getStateDbSessionField("DROP_SESSION0", "status_detail"), "Configuration error");
    EXPECT_EQ(getStateDbSessionField("DROP_SESSION0", "flow_group"), "");

    // 4) Clean up
    deque<KeyOpFieldsValuesTuple> del_entries;
    del_entries.emplace_back("DROP_SESSION0", DEL_COMMAND, vector<FieldValueTuple>{});
    processConfigEntries(CFG_TAM_SESSION_TABLE_NAME, del_entries);

    EXPECT_FALSE(sessionExistsInStateDb("DROP_SESSION0"));
}

// Test STATE_DB transitions from inactive to active when collector becomes reachable
TEST_F(TamOrchFlowUnawareModTest, StateDbTransitionInactiveToActive)
{
    // 1) Configure a collector but do NOT resolve it yet
    deque<KeyOpFieldsValuesTuple> collector_entries;
    vector<FieldValueTuple> collector_fvs = {
        { COLLECTOR_SRC_IP, "10.0.0.1" },
        { COLLECTOR_DST_IP, "10.0.0.10" },
        { COLLECTOR_L4_DST_PORT, "4739" },
        { COLLECTOR_DSCP_VALUE, "16" },
        { COLLECTOR_VRF, "default" },
    };
    collector_entries.emplace_back("COLLECTOR0", SET_COMMAND, collector_fvs);
    processConfigEntries(CFG_TAM_COLLECTOR_TABLE_NAME, collector_entries);

    // 2) Configure a drop-monitor session - should be inactive
    deque<KeyOpFieldsValuesTuple> session_entries;
    vector<FieldValueTuple> session_fvs = {
        { SESSION_TYPE, SESSION_TYPE_DROP_MONITOR },
        { SESSION_REPORT_TYPE, SESSION_REPORT_TYPE_IPFIX },
        { SESSION_COLLECTOR, "COLLECTOR0" },
    };
    session_entries.emplace_back("DROP_SESSION0", SET_COMMAND, session_fvs);
    processConfigEntries(CFG_TAM_SESSION_TABLE_NAME, session_entries);

    // 3) Verify session is inactive
    auto session = getSession("DROP_SESSION0");
    ASSERT_NE(session, nullptr);
    EXPECT_FALSE(session->active);
    EXPECT_EQ(getStateDbSessionField("DROP_SESSION0", "status"), "inactive");
    EXPECT_EQ(getStateDbSessionField("DROP_SESSION0", "status_detail"), "Collector not reachable");
    EXPECT_EQ(getStateDbSessionField("DROP_SESSION0", "flow_group"), "");

    // 4) Now resolve the collector by adding interface and neighbor
    Table intfTable(m_app_db.get(), APP_INTF_TABLE_NAME);
    intfTable.set("Ethernet0", { {"NULL", "NULL"},
                                  {"mac_addr", "00:00:00:00:00:00"} });
    intfTable.set("Ethernet0:10.0.0.1/24", { {"scope", "global"},
                                              {"family", "IPv4"} });
    gIntfsOrch->addExistingData(&intfTable);
    static_cast<Orch *>(gIntfsOrch)->doTask();

    Table neighborTable(m_app_db.get(), APP_NEIGH_TABLE_NAME);
    neighborTable.set("Ethernet0:10.0.0.10", { {"neigh", "00:00:0a:00:00:10"},
                                                {"family", "IPv4"} });
    gNeighOrch->addExistingData(&neighborTable);
    static_cast<Orch *>(gNeighOrch)->doTask();

    // 5) Verify session is now active
    session = getSession("DROP_SESSION0");
    ASSERT_NE(session, nullptr);
    EXPECT_TRUE(session->active);

    // 6) Verify STATE_DB now shows active status
    EXPECT_EQ(getStateDbSessionField("DROP_SESSION0", "status"), "active");
    EXPECT_EQ(getStateDbSessionField("DROP_SESSION0", "status_detail"), "");
    EXPECT_EQ(getStateDbSessionField("DROP_SESSION0", "flow_group"), "");

    // 7) Clean up
    deque<KeyOpFieldsValuesTuple> del_entries;
    del_entries.emplace_back("DROP_SESSION0", DEL_COMMAND, vector<FieldValueTuple>{});
    processConfigEntries(CFG_TAM_SESSION_TABLE_NAME, del_entries);

    deque<KeyOpFieldsValuesTuple> collector_del_entries;
    collector_del_entries.emplace_back("COLLECTOR0", DEL_COMMAND, vector<FieldValueTuple>{});
    processConfigEntries(CFG_TAM_COLLECTOR_TABLE_NAME, collector_del_entries);
}

// Test STATE_DB includes flow_group when configured
TEST_F(TamOrchFlowUnawareModTest, StateDbWithFlowGroup)
{
    // 1) Configure a collector with valid IPs/port and default VRF
    deque<KeyOpFieldsValuesTuple> collector_entries;
    vector<FieldValueTuple> collector_fvs = {
        { COLLECTOR_SRC_IP, "10.0.0.1" },
        { COLLECTOR_DST_IP, "10.0.0.10" },
        { COLLECTOR_L4_DST_PORT, "4739" },
        { COLLECTOR_DSCP_VALUE, "16" },
        { COLLECTOR_VRF, "default" },
    };
    collector_entries.emplace_back("COLLECTOR0", SET_COMMAND, collector_fvs);
    processConfigEntries(CFG_TAM_COLLECTOR_TABLE_NAME, collector_entries);

    // 2) Program APP_INTF and APP_NEIGH to resolve the collector
    Table intfTable(m_app_db.get(), APP_INTF_TABLE_NAME);
    intfTable.set("Ethernet0", { {"NULL", "NULL"},
                                  {"mac_addr", "00:00:00:00:00:00"} });
    intfTable.set("Ethernet0:10.0.0.1/24", { {"scope", "global"},
                                              {"family", "IPv4"} });
    gIntfsOrch->addExistingData(&intfTable);
    static_cast<Orch *>(gIntfsOrch)->doTask();

    Table neighborTable(m_app_db.get(), APP_NEIGH_TABLE_NAME);
    neighborTable.set("Ethernet0:10.0.0.10", { {"neigh", "00:00:0a:00:00:10"},
                                                {"family", "IPv4"} });
    gNeighOrch->addExistingData(&neighborTable);
    static_cast<Orch *>(gNeighOrch)->doTask();

    // 3) Configure a drop-monitor session WITH a flow group
    deque<KeyOpFieldsValuesTuple> session_entries;
    vector<FieldValueTuple> session_fvs = {
        { SESSION_TYPE, SESSION_TYPE_DROP_MONITOR },
        { SESSION_REPORT_TYPE, SESSION_REPORT_TYPE_IPFIX },
        { SESSION_COLLECTOR, "COLLECTOR0" },
        { SESSION_FLOW_GROUP, "FG1" },  // Include flow group
    };
    session_entries.emplace_back("DROP_SESSION0", SET_COMMAND, session_fvs);
    processConfigEntries(CFG_TAM_SESSION_TABLE_NAME, session_entries);

    // 4) Verify session is active
    auto session = getSession("DROP_SESSION0");
    ASSERT_NE(session, nullptr);
    ASSERT_TRUE(session->active);

    // 5) Verify STATE_DB has flow_group field
    EXPECT_TRUE(sessionExistsInStateDb("DROP_SESSION0"));
    EXPECT_EQ(getStateDbSessionField("DROP_SESSION0", "status"), "active");
    EXPECT_EQ(getStateDbSessionField("DROP_SESSION0", "flow_group"), "FG1");

    // 6) Clean up
    deque<KeyOpFieldsValuesTuple> del_entries;
    del_entries.emplace_back("DROP_SESSION0", DEL_COMMAND, vector<FieldValueTuple>{});
    processConfigEntries(CFG_TAM_SESSION_TABLE_NAME, del_entries);

    deque<KeyOpFieldsValuesTuple> collector_del_entries;
    collector_del_entries.emplace_back("COLLECTOR0", DEL_COMMAND, vector<FieldValueTuple>{});
    processConfigEntries(CFG_TAM_COLLECTOR_TABLE_NAME, collector_del_entries);
}

// Test STATE_DB stale entries are cleaned up when TamOrch is created (startup/config reload)
TEST_F(TamOrchFlowUnawareModTest, StateDbCleanupOnStartup)
{
    // 1) Add stale entries directly to STATE_DB (simulating leftover from previous run)
    Table stateTable(m_state_db.get(), STATE_TAM_DROP_MONITOR_SESSION_TABLE_NAME);
    vector<FieldValueTuple> stale_fvs = {
        {"status", "active"},
        {"report_type", "ipfix"},
        {"event_type", "packet-drop-stateless"},
        {"drop_stages", "ingress,egress"},
        {"collectors", "OLD_COLLECTOR"},
        {"flow_group", ""},
        {"status_detail", ""}
    };
    stateTable.set("STALE_SESSION1", stale_fvs);
    stateTable.set("STALE_SESSION2", stale_fvs);

    // 2) Verify stale entries exist
    EXPECT_TRUE(sessionExistsInStateDb("STALE_SESSION1"));
    EXPECT_TRUE(sessionExistsInStateDb("STALE_SESSION2"));

    // 3) Create a new TamOrch instance (simulating startup/config reload)
    // The TamOrch constructor should clean up all stale STATE_DB entries
    vector<string> tam_tables = {
        CFG_TAM_TABLE_NAME,
        CFG_TAM_COLLECTOR_TABLE_NAME,
        CFG_TAM_FLOW_GROUP_TABLE_NAME,
        CFG_TAM_SESSION_TABLE_NAME
    };
    TableConnector stateDbTamTable(m_state_db.get(), STATE_TAM_DROP_MONITOR_SESSION_TABLE_NAME);
    auto new_tam_orch = make_unique<TamOrch>(m_config_db.get(), tam_tables, stateDbTamTable,
                                              gAclOrch, gPortsOrch, gVrfOrch,
                                              gRouteOrch, gNeighOrch, gFdbOrch);

    // 4) Verify stale entries are removed by the constructor
    EXPECT_FALSE(sessionExistsInStateDb("STALE_SESSION1"));
    EXPECT_FALSE(sessionExistsInStateDb("STALE_SESSION2"));
}

} // namespace tamorch_test

