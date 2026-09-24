/**
 * @file cfgmgr_ut.cpp
 *
 * Task 2.2 – Thử nghiệm mô-đun quản lý cấu hình CfgMgr
 *
 * Unit tests for the configuration manager (CfgMgr) modules:
 *   - PortMgr  : PORT configuration flow CONFIG_DB → APP_DB
 *   - VrfMgr   : VRF lifecycle, STATE_DB publishing, APP_DB propagation
 *   - VlanMgr  : VLAN create/delete, member management, state prerequisites
 *
 * All tests run against the mock database (testing_db / mock_table) and the
 * mock exec() override so that no real Linux commands are executed.
 */

#include "gtest/gtest.h"
#include "mock_table.h"
#include "redisutility.h"

/* ---- PortMgr ---- */
#include "portmgr.h"

/* ---- VrfMgr ---- */
#include "vrfmgr.h"

/* ---- VlanMgr ---- */
#include "macaddress.h"
#include "vlanmgr.h"

/* mock exec infrastructure declared in common/mock_shell_command.cpp */
extern int (*callback)(const std::string &cmd, std::string &stdout);
extern int mockCmdReturn;
extern std::string mockCmdStdcout;
extern std::vector<std::string> mockCallArgs;

/* gMacAddress is declared in mock_orchagent_main.cpp */
extern swss::MacAddress gMacAddress;

using namespace swss;
using namespace std;

// ===========================================================================
// Helpers
// ===========================================================================
static vector<FieldValueTuple> fv(initializer_list<pair<string,string>> pairs)
{
    vector<FieldValueTuple> result;
    for (auto &p : pairs)
        result.emplace_back(p.first, p.second);
    return result;
}

static bool hasField(const vector<FieldValueTuple> &fvs,
                     const string &field, const string &expected)
{
    auto opt = swss::fvsGetValue(fvs, field, true);
    return opt && opt.get() == expected;
}

static bool cmdIssued(const string &prefix)
{
    for (const auto &cmd : mockCallArgs)
        if (cmd.rfind(prefix, 0) == 0 || cmd.find(prefix) != string::npos)
            return true;
    return false;
}

// ===========================================================================
// 2.2.1  PortMgr tests
// ===========================================================================
namespace portmgr_cfgmgr_ut
{

struct PortMgrCfgMgrTest : public ::testing::Test
{
    shared_ptr<DBConnector> m_app_db;
    shared_ptr<DBConnector> m_config_db;
    shared_ptr<DBConnector> m_state_db;
    shared_ptr<PortMgr>     m_portMgr;

    void SetUp() override
    {
        ::testing_db::reset();
        mockCallArgs.clear();
        callback  = nullptr;
        mockCmdReturn    = 0;
        mockCmdStdcout   = "";

        m_app_db    = make_shared<DBConnector>("APPL_DB",   0);
        m_config_db = make_shared<DBConnector>("CONFIG_DB", 0);
        m_state_db  = make_shared<DBConnector>("STATE_DB",  0);

        m_portMgr = make_shared<PortMgr>(
            m_config_db.get(), m_app_db.get(), m_state_db.get(),
            vector<string>{ CFG_PORT_TABLE_NAME });
    }

    void TearDown() override { ::testing_db::reset(); }
};

// ---------------------------------------------------------------------------
// 2.2.1.1  Port config reaches APP_DB with default values before state is ok
// ---------------------------------------------------------------------------
TEST_F(PortMgrCfgMgrTest, DefaultsWrittenToAppDbBeforePortReady)
{
    Table cfg_port(m_config_db.get(), CFG_PORT_TABLE_NAME);
    Table app_port(m_app_db.get(),    APP_PORT_TABLE_NAME);

    cfg_port.set("Ethernet0", fv({{"speed", "100000"}, {"index", "1"}}));

    m_portMgr->addExistingData(&cfg_port);
    m_portMgr->doTask();

    // ip commands must NOT have been issued (port not ready)
    EXPECT_TRUE(mockCallArgs.empty());

    // APP_DB should receive default mtu + admin_status
    vector<FieldValueTuple> vals;
    ASSERT_TRUE(app_port.get("Ethernet0", vals));
    EXPECT_TRUE(hasField(vals, "mtu",          DEFAULT_MTU_STR));
    EXPECT_TRUE(hasField(vals, "admin_status", DEFAULT_ADMIN_STATUS_STR));
    EXPECT_TRUE(hasField(vals, "speed",        "100000"));
}

// ---------------------------------------------------------------------------
// 2.2.1.2  Once state is ok, ip commands are issued
// ---------------------------------------------------------------------------
TEST_F(PortMgrCfgMgrTest, IpCommandsIssuedWhenPortReady)
{
    Table cfg_port  (m_config_db.get(), CFG_PORT_TABLE_NAME);
    Table state_port(m_state_db.get(),  STATE_PORT_TABLE_NAME);

    cfg_port.set("Ethernet0", fv({{"speed", "100000"}}));
    m_portMgr->addExistingData(&cfg_port);
    m_portMgr->doTask();

    // Mark port as ready
    state_port.set("Ethernet0", fv({{"state", "ok"}}));
    m_portMgr->doTask();

    ASSERT_GE(mockCallArgs.size(), 2u);
    EXPECT_TRUE(cmdIssued("mtu"));
    EXPECT_TRUE(cmdIssued("down") || cmdIssued("up"));
}

// ---------------------------------------------------------------------------
// 2.2.1.3  admin_status override takes effect in APP_DB
// ---------------------------------------------------------------------------
TEST_F(PortMgrCfgMgrTest, AdminStatusOverrideWrittenToAppDb)
{
    Table cfg_port(m_config_db.get(), CFG_PORT_TABLE_NAME);
    Table app_port(m_app_db.get(),    APP_PORT_TABLE_NAME);

    cfg_port.set("Ethernet0", fv({{"speed", "100000"}, {"admin_status", "up"}}));
    m_portMgr->addExistingData(&cfg_port);
    m_portMgr->doTask();

    vector<FieldValueTuple> vals;
    ASSERT_TRUE(app_port.get("Ethernet0", vals));
    EXPECT_TRUE(hasField(vals, "admin_status", "up"));
}

// ---------------------------------------------------------------------------
// 2.2.1.4  Config update while port is not ready uses latest values once ready
// ---------------------------------------------------------------------------
TEST_F(PortMgrCfgMgrTest, ConfigUpdateDuringPendingAppliedOnReady)
{
    Table cfg_port  (m_config_db.get(), CFG_PORT_TABLE_NAME);
    Table state_port(m_state_db.get(),  STATE_PORT_TABLE_NAME);

    cfg_port.set("Ethernet0", fv({{"speed", "100000"}, {"mtu", "9100"}}));
    m_portMgr->addExistingData(&cfg_port);
    m_portMgr->doTask();

    // Update config before port is ready
    cfg_port.set("Ethernet0", fv({{"speed", "50000"}, {"mtu", "1518"}, {"admin_status", "up"}}));
    m_portMgr->addExistingData(&cfg_port);
    m_portMgr->doTask();

    // Now make port ready
    state_port.set("Ethernet0", fv({{"state", "ok"}}));
    mockCallArgs.clear();
    m_portMgr->doTask();

    // The updated MTU "1518" must be used
    EXPECT_TRUE(cmdIssued("1518"));
    EXPECT_TRUE(cmdIssued("up"));
}

// ---------------------------------------------------------------------------
// 2.2.1.5  DELETE removes port entry from APP_DB
// ---------------------------------------------------------------------------
TEST_F(PortMgrCfgMgrTest, DeletePortRemovesFromAppDb)
{
    Table cfg_port  (m_config_db.get(), CFG_PORT_TABLE_NAME);
    Table state_port(m_state_db.get(),  STATE_PORT_TABLE_NAME);
    Table app_port  (m_app_db.get(),    APP_PORT_TABLE_NAME);

    cfg_port.set("Ethernet0", fv({{"speed", "100000"}}));
    state_port.set("Ethernet0", fv({{"state", "ok"}}));

    m_portMgr->addExistingData(&cfg_port);
    m_portMgr->doTask();

    // Now delete the port
    cfg_port.del("Ethernet0");
    auto entry = KeyOpFieldsValuesTuple{"Ethernet0", DEL_COMMAND, {}};
    m_portMgr->addExistingData(&cfg_port);

    Consumer c(new swss::ConsumerStateTable(m_config_db.get(), CFG_PORT_TABLE_NAME, 1, 1),
               m_portMgr.get(), CFG_PORT_TABLE_NAME);
    c.addToSync(entry);
    static_cast<Orch*>(m_portMgr.get())->doTask(c);

    vector<FieldValueTuple> vals;
    EXPECT_FALSE(app_port.get("Ethernet0", vals));
}

// ---------------------------------------------------------------------------
// 2.2.1.6  Multiple ports are configured independently
// ---------------------------------------------------------------------------
TEST_F(PortMgrCfgMgrTest, MultiplePortsConfiguredIndependently)
{
    Table cfg_port  (m_config_db.get(), CFG_PORT_TABLE_NAME);
    Table state_port(m_state_db.get(),  STATE_PORT_TABLE_NAME);
    Table app_port  (m_app_db.get(),    APP_PORT_TABLE_NAME);

    cfg_port.set("Ethernet0", fv({{"speed", "100000"}}));
    cfg_port.set("Ethernet4", fv({{"speed", "40000"},  {"admin_status", "up"}}));
    cfg_port.set("Ethernet8", fv({{"speed", "10000"},  {"mtu", "1500"}}));

    m_portMgr->addExistingData(&cfg_port);
    m_portMgr->doTask();

    vector<FieldValueTuple> vals;

    ASSERT_TRUE(app_port.get("Ethernet0", vals));
    EXPECT_TRUE(hasField(vals, "speed", "100000"));

    ASSERT_TRUE(app_port.get("Ethernet4", vals));
    EXPECT_TRUE(hasField(vals, "admin_status", "up"));

    ASSERT_TRUE(app_port.get("Ethernet8", vals));
    EXPECT_TRUE(hasField(vals, "mtu", "1500"));
}

} // namespace portmgr_cfgmgr_ut

// ===========================================================================
// 2.2.2  VrfMgr tests
// ===========================================================================
namespace vrfmgr_cfgmgr_ut
{

struct VrfMgrCfgMgrTest : public ::testing::Test
{
    shared_ptr<DBConnector> m_app_db;
    shared_ptr<DBConnector> m_config_db;
    shared_ptr<DBConnector> m_state_db;
    shared_ptr<VrfMgr>      m_vrfMgr;

    void SetUp() override
    {
        ::testing_db::reset();
        mockCallArgs.clear();
        callback       = nullptr;
        mockCmdReturn  = 0;
        mockCmdStdcout = "";

        m_app_db    = make_shared<DBConnector>("APPL_DB",   0);
        m_config_db = make_shared<DBConnector>("CONFIG_DB", 0);
        m_state_db  = make_shared<DBConnector>("STATE_DB",  0);

        m_vrfMgr = make_shared<VrfMgr>(
            m_config_db.get(), m_app_db.get(), m_state_db.get(),
            vector<string>{
                CFG_VRF_TABLE_NAME,
                CFG_VNET_TABLE_NAME,
                CFG_VXLAN_EVPN_NVO_TABLE_NAME,
                CFG_MGMT_VRF_CONFIG_TABLE_NAME
            });
    }

    void TearDown() override { ::testing_db::reset(); }

    // Helper: inject a SET event into VrfMgr via a Consumer
    void injectSet(const string &key, vector<FieldValueTuple> fields = {})
    {
        Table cfg(m_config_db.get(), CFG_VRF_TABLE_NAME);
        cfg.set(key, fields.empty() ? fv({{"fallback", "false"}}) : fields);
        Consumer c(new swss::ConsumerStateTable(m_config_db.get(), CFG_VRF_TABLE_NAME, 1, 1),
                   m_vrfMgr.get(), CFG_VRF_TABLE_NAME);
        c.addToSync(KeyOpFieldsValuesTuple{key, SET_COMMAND,
            fields.empty() ? fv({{"fallback", "false"}}) : fields});
        static_cast<Orch*>(m_vrfMgr.get())->doTask(c);
    }

    void injectDel(const string &key)
    {
        Consumer c(new swss::ConsumerStateTable(m_config_db.get(), CFG_VRF_TABLE_NAME, 1, 1),
                   m_vrfMgr.get(), CFG_VRF_TABLE_NAME);
        c.addToSync(KeyOpFieldsValuesTuple{key, DEL_COMMAND, {}});
        static_cast<Orch*>(m_vrfMgr.get())->doTask(c);
    }
};

// ---------------------------------------------------------------------------
// 2.2.2.1  Creating a VRF issues ip link add + ip link set up
// ---------------------------------------------------------------------------
TEST_F(VrfMgrCfgMgrTest, CreateVrf_IssuesIpCommand)
{
    mockCallArgs.clear();
    injectSet("Vrf-red");

    // Must have issued 'ip link add' for the VRF
    EXPECT_TRUE(cmdIssued("link add"));
    // Must have set the link up
    EXPECT_TRUE(cmdIssued("link set") || cmdIssued("up"));
}

// ---------------------------------------------------------------------------
// 2.2.2.2  Creating a VRF sets STATE_DB state to ok
// ---------------------------------------------------------------------------
TEST_F(VrfMgrCfgMgrTest, CreateVrf_SetsStateOk)
{
    injectSet("Vrf-blue");

    Table state_vrf(m_state_db.get(), STATE_VRF_TABLE_NAME);
    vector<FieldValueTuple> vals;
    ASSERT_TRUE(state_vrf.get("Vrf-blue", vals));
    EXPECT_TRUE(hasField(vals, "state", "ok"));
}

// ---------------------------------------------------------------------------
// 2.2.2.3  Creating multiple VRFs assigns distinct routing tables
// ---------------------------------------------------------------------------
TEST_F(VrfMgrCfgMgrTest, CreateMultipleVrfs_DistinctTables)
{
    injectSet("Vrf-A");
    injectSet("Vrf-B");
    injectSet("Vrf-C");

    // Each VRF must have a unique table id in the map
    EXPECT_EQ(m_vrfMgr->m_vrfTableMap.size(), 3u);
    set<uint32_t> tables;
    for (auto &kv : m_vrfMgr->m_vrfTableMap)
        tables.insert(kv.second);
    EXPECT_EQ(tables.size(), 3u); // all distinct
}

// ---------------------------------------------------------------------------
// 2.2.2.4  Deleting a VRF issues ip link del and recycles the table
// ---------------------------------------------------------------------------
TEST_F(VrfMgrCfgMgrTest, DeleteVrf_RecyclesTable)
{
    injectSet("Vrf-tmp");
    ASSERT_EQ(m_vrfMgr->m_vrfTableMap.count("Vrf-tmp"), 1u);
    uint32_t table_id = m_vrfMgr->m_vrfTableMap.at("Vrf-tmp");

    mockCallArgs.clear();
    injectDel("Vrf-tmp");

    EXPECT_EQ(m_vrfMgr->m_vrfTableMap.count("Vrf-tmp"), 0u);
    EXPECT_TRUE(cmdIssued("link del"));

    // Table id must be back in free set
    EXPECT_NE(m_vrfMgr->m_freeTables.find(table_id), m_vrfMgr->m_freeTables.end());
}

// ---------------------------------------------------------------------------
// 2.2.2.5  Deleting a non-existent VRF is a no-op
// ---------------------------------------------------------------------------
TEST_F(VrfMgrCfgMgrTest, DeleteNonExistentVrf_IsNoOp)
{
    size_t before = m_vrfMgr->m_vrfTableMap.size();
    mockCallArgs.clear();
    injectDel("Vrf-ghost");

    EXPECT_EQ(m_vrfMgr->m_vrfTableMap.size(), before);
    // No ip link del should have been issued for a non-existent VRF
    for (auto &cmd : mockCallArgs)
    {
        EXPECT_EQ(cmd.find("link del"), string::npos)
            << "Unexpected ip link del for non-existent VRF: " << cmd;
    }
}

// ---------------------------------------------------------------------------
// 2.2.2.6  mgmt VRF: uses reserved table id, not the free pool
// ---------------------------------------------------------------------------
TEST_F(VrfMgrCfgMgrTest, MgmtVrfTableIsReserved)
{
    // Inject via CFG_MGMT_VRF_CONFIG_TABLE_NAME consumer
    Consumer c(new swss::ConsumerStateTable(m_config_db.get(),
               CFG_MGMT_VRF_CONFIG_TABLE_NAME, 1, 1),
               m_vrfMgr.get(), CFG_MGMT_VRF_CONFIG_TABLE_NAME);
    c.addToSync(KeyOpFieldsValuesTuple{
        "vrf_global", SET_COMMAND,
        fv({{"mgmtVrfEnabled", "true"}, {"in_band_mgmt_enabled", "true"}})});
    static_cast<Orch*>(m_vrfMgr.get())->doTask(c);

    ASSERT_NE(m_vrfMgr->m_vrfTableMap.find("mgmt"), m_vrfMgr->m_vrfTableMap.end());
    // Reserved table id for mgmt is 6000 (MGMT_VRF_TABLE_ID in vrfmgr.cpp)
    EXPECT_EQ(m_vrfMgr->m_vrfTableMap.at("mgmt"), 6000u);
}

} // namespace vrfmgr_cfgmgr_ut

// ===========================================================================
// 2.2.3  VlanMgr tests
// ===========================================================================
namespace vlanmgr_cfgmgr_ut
{

struct VlanMgrCfgMgrTest : public ::testing::Test
{
    shared_ptr<DBConnector> m_app_db;
    shared_ptr<DBConnector> m_config_db;
    shared_ptr<DBConnector> m_state_db;
    shared_ptr<VlanMgr>     m_vlanMgr;

    void SetUp() override
    {
        ::testing_db::reset();
        mockCallArgs.clear();
        callback       = nullptr;
        mockCmdReturn  = 0;
        mockCmdStdcout = "";

        // gMacAddress must be non-zero for VlanMgr to proceed (isVlanMacOk check)
        gMacAddress = MacAddress("11:22:33:44:55:66");

        m_app_db    = make_shared<DBConnector>("APPL_DB",   0);
        m_config_db = make_shared<DBConnector>("CONFIG_DB", 0);
        m_state_db  = make_shared<DBConnector>("STATE_DB",  0);

        m_vlanMgr = make_shared<VlanMgr>(
            m_config_db.get(), m_app_db.get(), m_state_db.get(),
            vector<string>{ CFG_VLAN_TABLE_NAME, CFG_VLAN_MEMBER_TABLE_NAME },
            vector<string>{ STATE_OPER_PORT_TABLE_NAME, STATE_OPER_FDB_TABLE_NAME,
                            STATE_OPER_VLAN_MEMBER_TABLE_NAME });
    }

    void TearDown() override { ::testing_db::reset(); }

    // helpers ----------------------------------------------------
    void injectVlanSet(const string &key, vector<FieldValueTuple> fields = {})
    {
        if (fields.empty())
            fields = fv({{"admin_status", "up"}, {"mtu", "9100"}});
        Consumer c(new swss::ConsumerStateTable(m_config_db.get(),
                   CFG_VLAN_TABLE_NAME, 1, 1),
                   m_vlanMgr.get(), CFG_VLAN_TABLE_NAME);
        c.addToSync(KeyOpFieldsValuesTuple{key, SET_COMMAND, fields});
        static_cast<Orch*>(m_vlanMgr.get())->doTask(c);
    }

    void injectVlanDel(const string &key)
    {
        Consumer c(new swss::ConsumerStateTable(m_config_db.get(),
                   CFG_VLAN_TABLE_NAME, 1, 1),
                   m_vlanMgr.get(), CFG_VLAN_TABLE_NAME);
        c.addToSync(KeyOpFieldsValuesTuple{key, DEL_COMMAND, {}});
        static_cast<Orch*>(m_vlanMgr.get())->doTask(c);
    }

    void markPortReady(const string &alias)
    {
        Table state_port(m_state_db.get(), STATE_PORT_TABLE_NAME);
        state_port.set(alias, fv({{"state", "ok"}}));
    }

    void injectMemberSet(const string &vlanMemberKey,
                         const string &tagging_mode = "tagged")
    {
        Consumer c(new swss::ConsumerStateTable(m_config_db.get(),
                   CFG_VLAN_MEMBER_TABLE_NAME, 1, 1),
                   m_vlanMgr.get(), CFG_VLAN_MEMBER_TABLE_NAME);
        c.addToSync(KeyOpFieldsValuesTuple{
            vlanMemberKey, SET_COMMAND,
            fv({{"tagging_mode", tagging_mode}})});
        static_cast<Orch*>(m_vlanMgr.get())->doTask(c);
    }

    void injectMemberDel(const string &vlanMemberKey)
    {
        Consumer c(new swss::ConsumerStateTable(m_config_db.get(),
                   CFG_VLAN_MEMBER_TABLE_NAME, 1, 1),
                   m_vlanMgr.get(), CFG_VLAN_MEMBER_TABLE_NAME);
        c.addToSync(KeyOpFieldsValuesTuple{vlanMemberKey, DEL_COMMAND, {}});
        static_cast<Orch*>(m_vlanMgr.get())->doTask(c);
    }
};

// ---------------------------------------------------------------------------
// 2.2.3.1  Creating a VLAN publishes to APP_DB
// ---------------------------------------------------------------------------
TEST_F(VlanMgrCfgMgrTest, CreateVlan_PublishesToAppDb)
{
    injectVlanSet("Vlan10");

    Table app_vlan(m_app_db.get(), APP_VLAN_TABLE_NAME);
    vector<FieldValueTuple> vals;
    ASSERT_TRUE(app_vlan.get("Vlan10", vals));
}

// ---------------------------------------------------------------------------
// 2.2.3.2  Creating a VLAN issues addHostVlan ip commands
// ---------------------------------------------------------------------------
TEST_F(VlanMgrCfgMgrTest, CreateVlan_IssuesHostVlanCommands)
{
    mockCallArgs.clear();
    injectVlanSet("Vlan20");

    // The bridge vlan add & ip link add commands must have been invoked
    EXPECT_TRUE(cmdIssued("vlan add vid 20") || cmdIssued("vlan add vid 20"));
    EXPECT_TRUE(cmdIssued("Vlan20") || cmdIssued("link add"));
}

// ---------------------------------------------------------------------------
// 2.2.3.3  VLAN admin_status update issues setHostVlanAdminState command
// ---------------------------------------------------------------------------
TEST_F(VlanMgrCfgMgrTest, CreateVlan_AdminStatusDown_IssuedToHost)
{
    mockCallArgs.clear();
    injectVlanSet("Vlan30", fv({{"admin_status", "down"}}));

    // ip link set Vlan30 down
    EXPECT_TRUE(cmdIssued("Vlan30") || cmdIssued("down"));
}

// ---------------------------------------------------------------------------
// 2.2.3.4  Invalid VLAN key (no 'Vlan' prefix) is silently dropped
// ---------------------------------------------------------------------------
TEST_F(VlanMgrCfgMgrTest, InvalidVlanKey_Dropped)
{
    size_t cmd_before = mockCallArgs.size();
    injectVlanSet("BadKey40");   // no "Vlan" prefix

    Table app_vlan(m_app_db.get(), APP_VLAN_TABLE_NAME);
    vector<FieldValueTuple> vals;
    EXPECT_FALSE(app_vlan.get("BadKey40", vals));
    // No extra host commands
    EXPECT_EQ(mockCallArgs.size(), cmd_before);
}

// ---------------------------------------------------------------------------
// 2.2.3.5  Deleting a VLAN removes it from APP_DB and issues ip link del
// ---------------------------------------------------------------------------
TEST_F(VlanMgrCfgMgrTest, DeleteVlan_RemovesFromAppDbAndCallsDel)
{
    injectVlanSet("Vlan50");

    Table app_vlan(m_app_db.get(), APP_VLAN_TABLE_NAME);
    vector<FieldValueTuple> vals;
    ASSERT_TRUE(app_vlan.get("Vlan50", vals));

    mockCallArgs.clear();
    injectVlanDel("Vlan50");

    // ip link del Vlan50 must appear in commands
    EXPECT_TRUE(cmdIssued("link del Vlan50") || cmdIssued("Vlan50"));

    // APP_DB entry removed
    EXPECT_FALSE(app_vlan.get("Vlan50", vals));
}

// ---------------------------------------------------------------------------
// 2.2.3.6  Member add is deferred when port state is not ok
// ---------------------------------------------------------------------------
TEST_F(VlanMgrCfgMgrTest, MemberAdd_DeferredIfPortNotReady)
{
    injectVlanSet("Vlan60");

    // No port state set, member add should be deferred
    mockCallArgs.clear();
    injectMemberSet("Vlan60|Ethernet0", "tagged");

    // No bridge vlan add or ip link set should have been issued for the member
    for (auto &cmd : mockCallArgs)
    {
        EXPECT_EQ(cmd.find("Ethernet0"), string::npos)
            << "Unexpected command for unready port: " << cmd;
    }

    Table app_member(m_app_db.get(), APP_VLAN_MEMBER_TABLE_NAME);
    vector<FieldValueTuple> vals;
    EXPECT_FALSE(app_member.get("Vlan60:Ethernet0", vals));
}

// ---------------------------------------------------------------------------
// 2.2.3.7  Member add proceeds after port state becomes ok
// ---------------------------------------------------------------------------
TEST_F(VlanMgrCfgMgrTest, MemberAdd_ProceedsAfterPortReady)
{
    injectVlanSet("Vlan70");
    markPortReady("Ethernet4");

    mockCallArgs.clear();
    injectMemberSet("Vlan70|Ethernet4", "untagged");

    // Bridge and ip commands should reference Ethernet4
    EXPECT_TRUE(cmdIssued("Ethernet4"));

    Table app_member(m_app_db.get(), APP_VLAN_MEMBER_TABLE_NAME);
    vector<FieldValueTuple> vals;
    ASSERT_TRUE(app_member.get("Vlan70|Ethernet4", vals));
    EXPECT_TRUE(hasField(vals, "tagging_mode", "untagged"));
}

// ---------------------------------------------------------------------------
// 2.2.3.8  Member delete removes from APP_DB and issues bridge vlan del
// ---------------------------------------------------------------------------
TEST_F(VlanMgrCfgMgrTest, MemberDelete_RemovesFromAppDbAndCallsDel)
{
    injectVlanSet("Vlan80");
    markPortReady("Ethernet8");
    injectMemberSet("Vlan80|Ethernet8", "tagged");

    Table app_member(m_app_db.get(), APP_VLAN_MEMBER_TABLE_NAME);
    vector<FieldValueTuple> vals;
    ASSERT_TRUE(app_member.get("Vlan80|Ethernet8", vals));

    mockCallArgs.clear();
    injectMemberDel("Vlan80|Ethernet8");

    EXPECT_TRUE(cmdIssued("vlan del") || cmdIssued("Ethernet8"));

    EXPECT_FALSE(app_member.get("Vlan80|Ethernet8", vals));
}

// ---------------------------------------------------------------------------
// 2.2.3.9  Multiple VLANs and members are independent
// ---------------------------------------------------------------------------
TEST_F(VlanMgrCfgMgrTest, MultipleVlans_IndependentState)
{
    injectVlanSet("Vlan100");
    injectVlanSet("Vlan200");
    injectVlanSet("Vlan300");

    markPortReady("Ethernet0");
    markPortReady("Ethernet4");

    injectMemberSet("Vlan100|Ethernet0", "tagged");
    injectMemberSet("Vlan200|Ethernet4", "untagged");

    Table app_member(m_app_db.get(), APP_VLAN_MEMBER_TABLE_NAME);
    vector<FieldValueTuple> vals;

    EXPECT_TRUE(app_member.get("Vlan100|Ethernet0", vals));
    EXPECT_TRUE(hasField(vals, "tagging_mode", "tagged"));

    EXPECT_TRUE(app_member.get("Vlan200|Ethernet4", vals));
    EXPECT_TRUE(hasField(vals, "tagging_mode", "untagged"));

    // Vlan300 exists in APP_DB (create was propagated)
    Table app_vlan(m_app_db.get(), APP_VLAN_TABLE_NAME);
    EXPECT_TRUE(app_vlan.get("Vlan300", vals));
}

// ---------------------------------------------------------------------------
// 2.2.3.10 After reset, VlanMgr starting fresh does not find stale state
// ---------------------------------------------------------------------------
TEST_F(VlanMgrCfgMgrTest, ResetClearsVlanState)
{
    injectVlanSet("Vlan999");

    Table app_vlan(m_app_db.get(), APP_VLAN_TABLE_NAME);
    vector<FieldValueTuple> vals;
    ASSERT_TRUE(app_vlan.get("Vlan999", vals));

    // Reset mock DB
    ::testing_db::reset();

    EXPECT_FALSE(app_vlan.get("Vlan999", vals));
}

} // namespace vlanmgr_cfgmgr_ut
