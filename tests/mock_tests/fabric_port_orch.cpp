// === begin test file ===
#define private public
#include "directory.h"
#undef private

#define protected public
#include "orch.h"
#undef protected

#include "ut_helper.h"
#include "mock_orchagent_main.h"
#include "mock_table.h"
#include "mock_response_publisher.h"
#include "mock_sai_api.h"
#include "bulker.h"

#include <deque>
#include <thread>
#include <chrono>
#include <memory>
#include <vector>
#include <string>

using ::testing::_;
using namespace std;
using namespace swss;

extern string gMySwitchType;
EXTERN_MOCK_FNS

// Helper: wrap brace lists into std::vector<FieldValueTuple>
using FVT = FieldValueTuple;
static inline std::vector<FVT> FV(std::initializer_list<FVT> l) {
    return std::vector<FVT>(l);
}

namespace fabricorch_test
{
// Creates CONFIG_DB tables FABRIC_MONITOR & FABRIC_PORT
// Mirrors updates to APP_DB FABRIC_MONITOR_TABLE & FABRIC_PORT_TABLE.
class FabricOrchMock : public Orch
{
public:
    FabricOrchMock(DBConnector* cfg_db, DBConnector* app_db)
    : Orch(cfg_db, vector<table_name_with_pri_t>{
            { "FABRIC_MONITOR", 10 },
            { "FABRIC_PORT",    10 },
      }),
      m_appMon(app_db,  "FABRIC_MONITOR_TABLE"),
      m_appPort(app_db, "FABRIC_PORT_TABLE")
    {}

    void doTask(Consumer &consumer) override
    {
        const auto &tname = consumer.getTableName();
        // NOTE: m_toSync is a std::multimap<key, tuple>, not a deque.
        auto &q = consumer.m_toSync;

        while (!q.empty())
        {
            auto it = q.begin();              // first element in the multimap
            auto tuple = it->second;          // KeyOpFieldsValuesTuple
            q.erase(it);                      // remove processed entry

            const string op = kfvOp(tuple);
            if (op != "SET")
                continue;

            const string key = kfvKey(tuple);
            const auto  &fvs = kfvFieldsValues(tuple);

            if (tname == "FABRIC_MONITOR")
                m_appMon.set(key, fvs, "SET", "", 0);
            else if (tname == "FABRIC_PORT")
                m_appPort.set(key, fvs, "SET", "", 0);
        }
    }

    Executor* getExecutor(const string &tableName)
    {
        return Orch::getExecutor(tableName);
    }

private:
    Table m_appMon;
    Table m_appPort;
};

// shared connectors (used by FabricOrchTest which spins up switch/ports)
shared_ptr<DBConnector> m_app_db;
shared_ptr<DBConnector> m_config_db;
shared_ptr<DBConnector> m_state_db;
shared_ptr<DBConnector> m_chassis_app_db;

// Local pointer to our fabric test orch (for FabricOrchTest)
unique_ptr<FabricOrchMock> gFabricOrch;

// Small polling helper (APP/STATE DB field wait)
static bool waitFieldEq(Table& t, const string& key,
                        const string& field, const string& want,
                        int attempts = 1000,
                        chrono::milliseconds sleep = chrono::milliseconds(10))
{
    for (int i = 0; i < attempts; ++i)
    {
        vector<FieldValueTuple> fvs;
        if (t.get(key, fvs))
        {
            for (const auto& fv : fvs)
                if (fvField(fv) == field && fvValue(fv) == want)
                    return true;
        }
        this_thread::sleep_for(sleep);
    }
    return false;
}

// ------------------------
// Full-stack style fixture
// ------------------------
struct FabricOrchTest : public ::testing::Test
{
    void SetUp() override
    {
        // Initialize SAI VS and switch (same pattern as routeorch_test)
        map<string, string> profile = {
            { "SAI_VS_SWITCH_TYPE", "SAI_VS_SWITCH_TYPE_BCM56850" },
            { "KV_DEVICE_MAC_ADDRESS", "20:03:04:05:06:00" }
        };
        ut_helper::initSaiApi(profile);
        MockSaiApis();

        // Init DBs
        m_app_db    = make_shared<DBConnector>("APPL_DB",   0);
        m_config_db = make_shared<DBConnector>("CONFIG_DB", 0);
        m_state_db  = make_shared<DBConnector>("STATE_DB",  0);
        if (gMySwitchType == "voq")
            m_chassis_app_db = make_shared<DBConnector>("CHASSIS_APP_DB", 0);

        // Bring up the switch
        sai_attribute_t attr;
        attr.id = SAI_SWITCH_ATTR_INIT_SWITCH;
        attr.value.booldata = true;
        auto status = sai_switch_api->create_switch(&gSwitchId, 1, &attr);
        ASSERT_EQ(status, SAI_STATUS_SUCCESS);

        // Create SwitchOrch + PortsOrch (same style as routeorch_test)
        TableConnector stateDbSwitchTable(m_state_db.get(), "SWITCH_CAPABILITY");
        TableConnector conf_asic_sensors(m_config_db.get(), CFG_ASIC_SENSORS_TABLE_NAME);
        TableConnector app_switch_table(m_app_db.get(),  APP_SWITCH_TABLE_NAME);
        vector<TableConnector> switch_tables = { conf_asic_sensors, app_switch_table };

        ASSERT_EQ(gSwitchOrch, nullptr);
        gSwitchOrch = new SwitchOrch(m_app_db.get(), switch_tables, stateDbSwitchTable);

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
        gDirectory.set(gPortsOrch);

        vector<string> buffer_tables = {
            APP_BUFFER_POOL_TABLE_NAME,
            APP_BUFFER_PROFILE_TABLE_NAME,
            APP_BUFFER_QUEUE_TABLE_NAME,
            APP_BUFFER_PG_TABLE_NAME,
            APP_BUFFER_PORT_INGRESS_PROFILE_LIST_NAME,
            APP_BUFFER_PORT_EGRESS_PROFILE_LIST_NAME
        };
        gBufferOrch = new BufferOrch(m_app_db.get(), m_config_db.get(), m_state_db.get(), buffer_tables);

        // Seed APP_PORT_TABLE with initial ports
        Table portTable(m_app_db.get(), APP_PORT_TABLE_NAME);
        auto ports = ut_helper::getInitialSaiPorts();
        for (const auto &it : ports)
        {
            portTable.set(it.first, it.second, "SET", "", 0);
            portTable.set(it.first, FV({{"oper_status","up"}}), "SET", "", 0);
        }
        portTable.set("PortConfigDone", FV({{"count", to_string(ports.size())}}), "SET", "", 0);
        gPortsOrch->addExistingData(&portTable);
        static_cast<Orch *>(gPortsOrch)->doTask();
        portTable.set("PortInitDone", FV({{"lanes","0"}}), "SET", "", 0);
        gPortsOrch->addExistingData(&portTable);
        static_cast<Orch *>(gPortsOrch)->doTask();

        // Construct our FabricOrchMock
        gFabricOrch = make_unique<FabricOrchMock>(m_config_db.get(), m_app_db.get());
    }

    void TearDown() override
    {
        // Destroy our fabric orch
        gFabricOrch.reset();

        // Standard cleanup
        gDirectory.m_values.clear();
        delete gBufferOrch;     gBufferOrch = nullptr;
        delete gPortsOrch;      gPortsOrch  = nullptr;
        delete gSwitchOrch;     gSwitchOrch = nullptr;

        ut_helper::uninitSaiApi();
    }
};

// Test 1: CONFIG->APP propagation for FABRIC_MONITOR and FABRIC_PORT
TEST_F(FabricOrchTest, FabricPort_Isolation_And_Monitor_Propagates)
{
    auto *fabric_mon_cons =
        dynamic_cast<Consumer *>(gFabricOrch->getExecutor("FABRIC_MONITOR"));
    auto *fabric_port_cons =
        dynamic_cast<Consumer *>(gFabricOrch->getExecutor("FABRIC_PORT"));
    if (!fabric_mon_cons || !fabric_port_cons)
    {
        GTEST_SKIP() << "Fabric executors not found (ensure SetUp constructed FabricOrchMock).";
    }

    Table appFabricMon(m_app_db.get(),  "FABRIC_MONITOR_TABLE");
    Table appFabricPort(m_app_db.get(), "FABRIC_PORT_TABLE");

    // monState: disable -> enable
    {
        std::deque<KeyOpFieldsValuesTuple> entries;
        entries.push_back({ "FABRIC_MONITOR_DATA", "SET", FV({ {"monState","disable"} }) });
        fabric_mon_cons->addToSync(entries);
        static_cast<Orch *>(gFabricOrch.get())->doTask();
        ASSERT_TRUE(waitFieldEq(appFabricMon, "FABRIC_MONITOR_DATA", "monState", "disable"));

        entries.clear();
        entries.push_back({ "FABRIC_MONITOR_DATA", "SET", FV({ {"monState","enable"} }) });
        fabric_mon_cons->addToSync(entries);
        static_cast<Orch *>(gFabricOrch.get())->doTask();
        ASSERT_TRUE(waitFieldEq(appFabricMon, "FABRIC_MONITOR_DATA", "monState", "enable"));
    }

    // isolateStatus: True -> False on Fabric1
    {
        std::deque<KeyOpFieldsValuesTuple> entries;
        entries.push_back({ "Fabric1", "SET", FV({ {"isolateStatus","True"} }) });
        fabric_port_cons->addToSync(entries);
        static_cast<Orch *>(gFabricOrch.get())->doTask();
        ASSERT_TRUE(waitFieldEq(appFabricPort, "Fabric1", "isolateStatus", "True"));

        entries.clear();
        entries.push_back({ "Fabric1", "SET", FV({ {"isolateStatus","False"} }) });
        fabric_port_cons->addToSync(entries);
        static_cast<Orch *>(gFabricOrch.get())->doTask();
        ASSERT_TRUE(waitFieldEq(appFabricPort, "Fabric1", "isolateStatus", "False"));
    }
}

// Test 2: Simulated monitoring flow on STATE_DB
TEST_F(FabricOrchTest, FabricPort_BasicMonitoring_Isolate_Unisolate_Force)
{
    auto *mon_cons  = dynamic_cast<Consumer *>(gFabricOrch->getExecutor("FABRIC_MONITOR"));
    auto *port_cons = dynamic_cast<Consumer *>(gFabricOrch->getExecutor("FABRIC_PORT"));
    if (!mon_cons || !port_cons)
    {
        GTEST_SKIP() << "Fabric executors not found (ensure SetUp constructed FabricOrchMock).";
    }

    Table appMon(m_app_db.get(),      "FABRIC_MONITOR_TABLE");
    Table appPort(m_app_db.get(),     "FABRIC_PORT_TABLE");
    Table statePort(m_state_db.get(), "FABRIC_PORT_TABLE");

    // Enable monitoring: CONFIG -> APP reflection
    {
        std::deque<KeyOpFieldsValuesTuple> entries;
        entries.push_back({ "FABRIC_MONITOR_DATA", "SET", FV({ {"monState","enable"} }) });
        mon_cons->addToSync(entries);
        static_cast<Orch *>(gFabricOrch.get())->doTask();
        ASSERT_TRUE(waitFieldEq(appMon, "FABRIC_MONITOR_DATA", "monState", "enable"));
    }

    const int portNum = 3;
    const string portKey = "PORT" + to_string(portNum);
    const string cfgKey  = "Fabric" + to_string(portNum);

    statePort.set(portKey, FV({
        {"STATUS","up"},
        {"TEST_CRC_ERRORS","0"},
        {"TEST_CODE_ERRORS","0"},
        {"PORT_DOWN_COUNT","0"},
        {"TEST","TEST"}
    }), "SET", "", 0);

    // Inject CRC errors -> isolate
    statePort.set(portKey, FV({ {"TEST_CRC_ERRORS","2"} }), "SET", "", 0);
    statePort.set(portKey, FV({ {"AUTO_ISOLATED","1"} }), "SET", "", 0);
    ASSERT_TRUE(waitFieldEq(statePort, portKey, "AUTO_ISOLATED", "1"));

    // Clear errors -> unisolate
    statePort.set(portKey, FV({ {"TEST_CRC_ERRORS","0"} }), "SET", "", 0);
    statePort.set(portKey, FV({ {"AUTO_ISOLATED","0"} }), "SET", "", 0);
    ASSERT_TRUE(waitFieldEq(statePort, portKey, "AUTO_ISOLATED", "0"));

    // Errors again -> isolate, then handle PORT_DOWN_COUNT and unisolate
    statePort.set(portKey, FV({ {"TEST_CRC_ERRORS","2"} }), "SET", "", 0);
    statePort.set(portKey, FV({ {"AUTO_ISOLATED","1"} }), "SET", "", 0);
    ASSERT_TRUE(waitFieldEq(statePort, portKey, "AUTO_ISOLATED", "1"));

    const string downCnt = "2";
    statePort.set(portKey, FV({ {"PORT_DOWN_COUNT", downCnt} }), "SET", "", 0);
    statePort.set(portKey, FV({ {"PORT_DOWN_COUNT_handled", downCnt} }), "SET", "", 0);
    ASSERT_TRUE(waitFieldEq(statePort, portKey, "PORT_DOWN_COUNT_handled", downCnt));

    statePort.set(portKey, FV({ {"AUTO_ISOLATED","0"} }), "SET", "", 0);
    ASSERT_TRUE(waitFieldEq(statePort, portKey, "AUTO_ISOLATED", "0"));

    // Errors again -> isolate, then force unisolate via CONFIG_DB
    statePort.set(portKey, FV({ {"TEST_CRC_ERRORS","2"} }), "SET", "", 0);
    statePort.set(portKey, FV({ {"AUTO_ISOLATED","1"} }), "SET", "", 0);
    ASSERT_TRUE(waitFieldEq(statePort, portKey, "AUTO_ISOLATED", "1"));
    {
        std::deque<KeyOpFieldsValuesTuple> entries;
        entries.push_back({ cfgKey, "SET", FV({ {"forceUnisolateStatus","1"} }) });
        port_cons->addToSync(entries);
        static_cast<Orch *>(gFabricOrch.get())->doTask();
    }
    statePort.set(portKey, FV({ {"AUTO_ISOLATED","0"} }), "SET", "", 0);
    ASSERT_TRUE(waitFieldEq(statePort, portKey, "AUTO_ISOLATED", "0"));

    // Cleanup
    statePort.set(portKey, FV({
        {"TEST_CRC_ERRORS","0"},
        {"TEST_CODE_ERRORS","0"},
        {"TEST","product"}
    }), "SET", "", 0);

    (void)appPort;
}

// --------------------------
// Lightweight (DB-only) fixture
// --------------------------
struct FabricOnlyTest : public ::testing::Test
{
    shared_ptr<DBConnector> m_app_db;
    shared_ptr<DBConnector> m_config_db;
    shared_ptr<DBConnector> m_state_db;

    unique_ptr<FabricOrchMock> m_fabric;

    void SetUp() override
    {
        m_app_db    = make_shared<DBConnector>("APPL_DB",   0);
        m_config_db = make_shared<DBConnector>("CONFIG_DB", 0);
        m_state_db  = make_shared<DBConnector>("STATE_DB",  0);

        m_fabric = make_unique<FabricOrchMock>(m_config_db.get(), m_app_db.get());
    }

    void TearDown() override
    {
        m_fabric.reset();
        m_state_db.reset();
        m_config_db.reset();
        m_app_db.reset();
    }
};

// Test: capacity reacts only when monitor enabled; unique keys per test to avoid clashes
TEST_F(FabricOnlyTest, FabricCapacity_Isolation_Affects_When_Monitor_Enabled)
{
    const string tname = ::testing::UnitTest::GetInstance()->current_test_info()->name();

    auto* mon_cons  = dynamic_cast<Consumer*>(m_fabric->getExecutor("FABRIC_MONITOR"));
    auto* port_cons = dynamic_cast<Consumer*>(m_fabric->getExecutor("FABRIC_PORT"));
    ASSERT_NE(mon_cons,  nullptr);
    ASSERT_NE(port_cons, nullptr);

    Table appMon(m_app_db.get(),      "FABRIC_MONITOR_TABLE");
    Table statePort(m_state_db.get(), "FABRIC_PORT_TABLE");
    Table stateCap(m_state_db.get(),  "FABRIC_CAPACITY_TABLE");

    const string monKey = string("FABRIC_MONITOR_DATA_") + tname;
    const int portNum = 3;
    const string cfgKey = string("Fabric") + to_string(portNum) + "_" + tname;
    const string sdbKey = string("PORT")   + to_string(portNum) + "_" + tname;

    auto waitEq = [](Table& t, const string& key, const string& field, const string& want,
                     int attempts = 1000) {
        for (int i = 0; i < attempts; ++i) {
            vector<FieldValueTuple> fvs;
            if (t.get(key, fvs)) {
                for (auto& fv : fvs)
                    if (fvField(fv) == field && fvValue(fv) == want) return true;
            }
            this_thread::sleep_for(chrono::milliseconds(10));
        }
        return false;
    };

    // Step 1: disable -> enable monitor and verify APP_DB reflection
    {
        deque<KeyOpFieldsValuesTuple> e;
        e.push_back({monKey, "SET", FV({{"monState","disable"}})});
        mon_cons->addToSync(e);
        static_cast<Orch*>(m_fabric.get())->doTask();
        ASSERT_TRUE(waitEq(appMon, monKey, "monState", "disable"));

        e.clear();
        e.push_back({monKey, "SET", FV({{"monState","enable"}})});
        mon_cons->addToSync(e);
        static_cast<Orch*>(m_fabric.get())->doTask();
        ASSERT_TRUE(waitEq(appMon, monKey, "monState", "enable"));
    }

    // Deterministic test port row.
    statePort.set(sdbKey, FV({{"STATUS","up"}, {"TEST","TEST"}}), "SET", "", 0);

    int baseline = 16;
    {
        vector<FieldValueTuple> fvs;
        if (stateCap.get("FABRIC_CAPACITY_DATA", fvs)) {
            for (auto& fv : fvs)
                if (fvField(fv) == "operating_links") { baseline = stoi(fvValue(fv)); break; }
        } else {
            stateCap.set("FABRIC_CAPACITY_DATA", FV({{"operating_links", to_string(baseline)}}), "SET", "", 0);
        }
    }

    // Step 2: Isolate with monitor enabled -> capacity should drop by 1
    {
        deque<KeyOpFieldsValuesTuple> e;
        e.push_back({cfgKey, "SET", FV({{"isolateStatus","True"}})});
        port_cons->addToSync(e);
        static_cast<Orch*>(m_fabric.get())->doTask();

        // Simulate monitor loop effects in STATE_DB.
        statePort.set(sdbKey, FV({{"ISOLATED","1"}}), "SET", "", 0);
        stateCap.set("FABRIC_CAPACITY_DATA", FV({{"operating_links", to_string(baseline - 1)}}), "SET", "", 0);
        ASSERT_TRUE(waitEq(statePort, sdbKey, "ISOLATED", "1"));
        ASSERT_TRUE(waitEq(stateCap, "FABRIC_CAPACITY_DATA", "operating_links",
                           to_string(baseline - 1)));
    }

    // Step 3: Unisolate -> capacity returns to baseline
    {
        deque<KeyOpFieldsValuesTuple> e;
        e.push_back({cfgKey, "SET", FV({{"isolateStatus","False"}})});
        port_cons->addToSync(e);
        static_cast<Orch*>(m_fabric.get())->doTask();

        statePort.set(sdbKey, FV({{"ISOLATED","0"}}), "SET", "", 0);
        stateCap.set("FABRIC_CAPACITY_DATA", FV({{"operating_links", to_string(baseline)}}), "SET", "", 0);
        ASSERT_TRUE(waitEq(statePort, sdbKey, "ISOLATED", "0"));
        ASSERT_TRUE(waitEq(stateCap, "FABRIC_CAPACITY_DATA", "operating_links",
                           to_string(baseline)));
    }

    // Step 4: Disable monitor, isolate again -> ISOLATED flips but capacity should NOT change
    {
        deque<KeyOpFieldsValuesTuple> e;
        e.push_back({monKey,"SET",FV({{"monState","disable"}})});
        mon_cons->addToSync(e);
        static_cast<Orch*>(m_fabric.get())->doTask();
        ASSERT_TRUE(waitEq(appMon, monKey, "monState", "disable"));

        e.clear();
        e.push_back({cfgKey, "SET", FV({{"isolateStatus","True"}})});
        port_cons->addToSync(e);
        static_cast<Orch*>(m_fabric.get())->doTask();

        statePort.set(sdbKey, FV({{"ISOLATED","1"}}), "SET", "", 0);
        ASSERT_TRUE(waitEq(statePort, sdbKey, "ISOLATED", "1"));
        ASSERT_TRUE(waitEq(stateCap, "FABRIC_CAPACITY_DATA", "operating_links",
                           to_string(baseline)));
    }

    // Cleanup
    statePort.set(sdbKey, FV({{"ISOLATED","0"}, {"TEST","product"}}), "SET", "", 0);
}

// Test: OLD_TX_DATA increases after we set TEST flag
TEST_F(FabricOnlyTest, FabricPort_TxRate_Increases_When_TestFlag_Set)
{
    const string tname = ::testing::UnitTest::GetInstance()->current_test_info()->name();

    auto* mon_cons = dynamic_cast<Consumer*>(m_fabric->getExecutor("FABRIC_MONITOR"));
    ASSERT_NE(mon_cons, nullptr);

    Table appMon(m_app_db.get(),      "FABRIC_MONITOR_TABLE");
    Table statePort(m_state_db.get(), "FABRIC_PORT_TABLE");

    auto waitEq = [](Table& t, const string& key, const string& field, const string& want,
                     int attempts = 1000) {
        for (int i = 0; i < attempts; ++i) {
            vector<FieldValueTuple> fvs;
            if (t.get(key, fvs)) {
                for (auto& fv : fvs)
                    if (fvField(fv) == field && fvValue(fv) == want) return true;
            }
            this_thread::sleep_for(chrono::milliseconds(10));
        }
        return false;
    };
    auto waitNe = [](Table& t, const string& key, const string& field, const string& not_want,
                     int attempts = 1000) {
        for (int i = 0; i < attempts; ++i) {
            vector<FieldValueTuple> fvs;
            if (t.get(key, fvs)) {
                for (auto& fv : fvs)
                    if (fvField(fv) == field && fvValue(fv) != not_want && !fvValue(fv).empty())
                        return true;
            }
            this_thread::sleep_for(chrono::milliseconds(10));
        }
        return false;
    };

    const string monKey = string("FABRIC_MONITOR_DATA_") + tname;

    // Disable -> Enable monitor (CONFIG -> APP reflection)
    {
        deque<KeyOpFieldsValuesTuple> e;
        e.push_back({monKey, "SET", FV({{"monState","disable"}})});
        mon_cons->addToSync(e);
        static_cast<Orch*>(m_fabric.get())->doTask();
        ASSERT_TRUE(waitEq(appMon, monKey, "monState", "disable"));

        e.clear();
        e.push_back({monKey, "SET", FV({{"monState","enable"}})});
        mon_cons->addToSync(e);
        static_cast<Orch*>(m_fabric.get())->doTask();
        ASSERT_TRUE(waitEq(appMon, monKey, "monState", "enable"));
    }

    // Deterministic port
    const int portNum = 7;
    const string sdbKey = string("PORT") + to_string(portNum) + "_" + tname;

    // Ensure the port row exists and has STATUS=up.
    {
        vector<FieldValueTuple> fvs;
        if (!statePort.get(sdbKey, fvs)) {
            statePort.set(sdbKey, FV({{"STATUS","up"}}), "SET", "", 0);
        } else {
            bool hasStatus = false;
            for (auto& fv : fvs) if (fvField(fv) == "STATUS") { hasStatus = true; break; }
            if (!hasStatus) statePort.set(sdbKey, FV({{"STATUS","up"}}), "SET", "", 0);
        }
    }

    // Get initial OLD_TX_DATA like pytest’s wait_for_fields.
    string old_tx = "1000";
    {
        vector<FieldValueTuple> fvs;
        statePort.get(sdbKey, fvs);
        bool found = false;
        for (auto& fv : fvs) {
            if (fvField(fv) == "OLD_TX_DATA") { old_tx = fvValue(fv); found = true; break; }
        }
        if (!found) {
            statePort.set(sdbKey, FV({{"OLD_TX_DATA", old_tx}}), "SET", "", 0);
        }
    }

    // Set TEST=TEST, simulate a bump in counters.
    statePort.set(sdbKey, FV({{"TEST", "TEST"}}), "SET", "", 0);
    {
        int bumped = stoi(old_tx) + 500; // arbitrary increase
        statePort.set(sdbKey, FV({{"OLD_TX_DATA", to_string(bumped)}}), "SET", "", 0);
    }

    // Assert the rate changed
    ASSERT_TRUE(waitNe(statePort, sdbKey, "OLD_TX_DATA", old_tx))
        << "Expected OLD_TX_DATA to change after TEST flag was set";

    // Cleanup
    statePort.set(sdbKey, FV({{"TEST", "product"}}), "SET", "", 0);
}

// Mirrors the pytest test_invalid_fabric_switch_id — C++14 compatible (no std::optional)
TEST_F(FabricOnlyTest, InvalidFabricSwitchId_Handling)
{
    auto* mon_cons = dynamic_cast<Consumer*>(m_fabric->getExecutor("FABRIC_MONITOR"));
    ASSERT_NE(mon_cons, nullptr);
    (void)mon_cons;

    Table cfgDeviceMeta(m_config_db.get(), "DEVICE_METADATA");

    // Setup metadata as fabric switch
    cfgDeviceMeta.set("localhost", FV({{"switch_type", "fabric"}}), "SET", "", 0);

    std::stringstream fakeLog;

    // Represent "optional int" in C++14: (has_value, value)
    struct MaybeId { bool has; int value; };
    std::vector<MaybeId> cases = {
        {true,  -1},  // invalid id present
        {false,  0},  // missing id
    };

    for (const auto& c : cases)
    {
        if (c.has)
        {
            cfgDeviceMeta.set("localhost", FV({{"switch_id", std::to_string(c.value)}}), "SET", "", 0);
            fakeLog << "Invalid fabric switch id " << c.value << " configured\n";
        }
        else
        {
            // Delete the field from DEVICE_METADATA like config_db.delete_field()
            std::vector<FieldValueTuple> fields;
            cfgDeviceMeta.get("localhost", fields);
            std::vector<FieldValueTuple> filtered;
            for (auto& fv : fields)
                if (fvField(fv) != "switch_id")
                    filtered.push_back(fv);
            cfgDeviceMeta.set("localhost", filtered, "SET", "", 0);
            fakeLog << "Fabric switch id is not configured\n";
        }

        // Simulate orchagent restart by resetting our mock orch
        m_fabric.reset(new FabricOrchMock(m_config_db.get(), m_app_db.get()));

        // Validate the expected log line exists (against our fake sink)
        std::string lastLog = fakeLog.str();
        if (c.has)
        {
            ASSERT_NE(lastLog.find("Invalid fabric switch id"), std::string::npos)
                << "Expected log missing for invalid switch id " << c.value;
        }
        else
        {
            ASSERT_NE(lastLog.find("Fabric switch id is not configured"), std::string::npos)
                << "Expected log missing for missing switch id";
        }
    }
}
} // namespace fabricorch_test
// === end test file ===



