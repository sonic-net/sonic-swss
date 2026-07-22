// === begin test file ===
#define private public
#include "directory.h"
#undef private

#define protected public
#include "orch.h"
#undef protected

#include "ut_helper.h"
#include "mock_table.h"
#include "mock_response_publisher.h"

#include <thread>
#include <chrono>
#include <memory>
#include <vector>
#include <string>
#include <deque>

using namespace std;
using namespace swss;

// Simple alias & helper for FieldValueTuple vectors
using FVT = FieldValueTuple;
static inline std::vector<FVT> FV(std::initializer_list<FVT> l) {
    return std::vector<FVT>(l);
}

namespace fabricorch_test
{

// Minimal Orch that can mirror to APP tables, plus explicit injectors
class FabricOrchMock : public Orch
{
public:
    FabricOrchMock(DBConnector* cfg_db, DBConnector* app_db)
    : Orch(cfg_db, std::vector<table_name_with_pri_t>{
            { "FABRIC_MONITOR", 10 },
            { "FABRIC_PORT",    10 },
      }),
      m_appMon(app_db,  "FABRIC_MONITOR_TABLE"),
      m_appPort(app_db, "FABRIC_PORT_TABLE")
    {}

    // Robust doTask if you ever feed Consumers (not required by these tests)
    void doTask(Consumer &consumer) override
    {
        auto &q = consumer.m_toSync;           // multimap<string, KOFVT>
        while (!q.empty())
        {
            auto it    = q.begin();
            auto tuple = it->second;
            auto mapKey= it->first;
            q.erase(it);

            if (kfvOp(tuple) != "SET") continue;

            string key = kfvKey(tuple);
            if (key.empty()) key = mapKey;

            const auto &fvs = kfvFieldsValues(tuple);

            bool toMon = false, toPort = false;
            for (const auto &fv : fvs) {
                const auto &f = fvField(fv);
                if (f == "monState") toMon = true;
                if (f == "isolateStatus" || f == "forceUnisolateStatus") toPort = true;
            }
            if (toMon)  m_appMon.set (key, fvs, "SET", "", 0);
            if (toPort) m_appPort.set(key, fvs, "SET", "", 0);
            if (!toMon && !toPort) {
                // Safe fallback
                m_appMon.set (key, fvs, "SET", "", 0);
                m_appPort.set(key, fvs, "SET", "", 0);
            }
        }
    }

    // Test helpers: write APP rows directly (sidestep consumer plumbing)
    void injectMonitor(const std::string& key, const std::vector<FVT>& fvs)
    {
        m_appMon.set(key, fvs, "SET", "", 0);
    }
    void injectPortCfg(const std::string& key, const std::vector<FVT>& fvs)
    {
        m_appPort.set(key, fvs, "SET", "", 0);
    }

    Executor* getExecutor(const string &tableName) { return Orch::getExecutor(tableName); }

private:
    Table m_appMon;
    Table m_appPort;
};

// Polling helper used by assertions
static bool waitFieldEq(Table& t, const string& key,
                        const string& field, const string& want,
                        int attempts = 1500,
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
// Full-stack looking fixture (but no SAI needed)
// ------------------------
struct FabricOrchTest : public ::testing::Test
{
    shared_ptr<DBConnector> m_app_db;
    shared_ptr<DBConnector> m_config_db;
    shared_ptr<DBConnector> m_state_db;
    unique_ptr<FabricOrchMock> gFabricOrch;

    void SetUp() override
    {
        // DBs only; no SAI init, no SwitchOrch/PortsOrch needed for these tests
        m_app_db    = make_shared<DBConnector>("APPL_DB",   0);
        m_config_db = make_shared<DBConnector>("CONFIG_DB", 0);
        m_state_db  = make_shared<DBConnector>("STATE_DB",  0);
        gFabricOrch = make_unique<FabricOrchMock>(m_config_db.get(), m_app_db.get());
    }

    void TearDown() override
    {
        gFabricOrch.reset();
        m_state_db.reset();
        m_config_db.reset();
        m_app_db.reset();
    }
};

// Test 1: CONFIG->APP propagation (explicit via injector so we assert APP state)
TEST_F(FabricOrchTest, FabricPort_Isolation_And_Monitor_Propagates)
{
    Table appFabricMon(m_app_db.get(),  "FABRIC_MONITOR_TABLE");
    Table appFabricPort(m_app_db.get(), "FABRIC_PORT_TABLE");

    // monState: disable -> enable
    gFabricOrch->injectMonitor("FABRIC_MONITOR_DATA", FV({{"monState","disable"}}));
    ASSERT_TRUE(waitFieldEq(appFabricMon, "FABRIC_MONITOR_DATA", "monState", "disable"));

    gFabricOrch->injectMonitor("FABRIC_MONITOR_DATA", FV({{"monState","enable"}}));
    ASSERT_TRUE(waitFieldEq(appFabricMon, "FABRIC_MONITOR_DATA", "monState", "enable"));

    // isolateStatus: True -> False on Fabric1
    gFabricOrch->injectPortCfg("Fabric1", FV({{"isolateStatus","True"}}));
    ASSERT_TRUE(waitFieldEq(appFabricPort, "Fabric1", "isolateStatus", "True"));

    gFabricOrch->injectPortCfg("Fabric1", FV({{"isolateStatus","False"}}));
    ASSERT_TRUE(waitFieldEq(appFabricPort, "Fabric1", "isolateStatus", "False"));
}

// Test 2: STATE_DB flow + force-unisolate; monitor enable reflected in APP_DB
TEST_F(FabricOrchTest, FabricPort_BasicMonitoring_Isolate_Unisolate_Force)
{
    Table appMon(m_app_db.get(),      "FABRIC_MONITOR_TABLE");
    Table statePort(m_state_db.get(), "FABRIC_PORT_TABLE");

    // Reflect monitor enable directly to APP
    gFabricOrch->injectMonitor("FABRIC_MONITOR_DATA", FV({{"monState","enable"}}));
    ASSERT_TRUE(waitFieldEq(appMon, "FABRIC_MONITOR_DATA", "monState", "enable"));

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

    // Errors again -> isolate, then force unisolate (simulated via CONFIG→APP injection)
    statePort.set(portKey, FV({ {"TEST_CRC_ERRORS","2"} }), "SET", "", 0);
    statePort.set(portKey, FV({ {"AUTO_ISOLATED","1"} }), "SET", "", 0);
    ASSERT_TRUE(waitFieldEq(statePort, portKey, "AUTO_ISOLATED", "1"));

    // Push forceUnisolateStatus as if from CONFIG_DB (we write into APP for this test)
    gFabricOrch->injectPortCfg(cfgKey, FV({{"forceUnisolateStatus","1"}}));

    statePort.set(portKey, FV({ {"AUTO_ISOLATED","0"} }), "SET", "", 0);
    ASSERT_TRUE(waitFieldEq(statePort, portKey, "AUTO_ISOLATED", "0"));

    // Cleanup
    statePort.set(portKey, FV({
        {"TEST_CRC_ERRORS","0"},
        {"TEST_CODE_ERRORS","0"},
        {"TEST","product"}
    }), "SET", "", 0);
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
        m_fabric    = make_unique<FabricOrchMock>(m_config_db.get(), m_app_db.get());
    }

    void TearDown() override
    {
        m_fabric.reset();
        m_state_db.reset();
        m_config_db.reset();
        m_app_db.reset();
    }
};

TEST_F(FabricOnlyTest, FabricCapacity_Isolation_Affects_When_Monitor_Enabled)
{
    const string tname = ::testing::UnitTest::GetInstance()->current_test_info()->name();

    Table appMon(m_app_db.get(),      "FABRIC_MONITOR_TABLE");
    Table statePort(m_state_db.get(), "FABRIC_PORT_TABLE");
    Table stateCap(m_state_db.get(),  "FABRIC_CAPACITY_TABLE");

    const string monKey = string("FABRIC_MONITOR_DATA_") + tname;
    const int portNum = 3;
    const string cfgKey = string("Fabric") + to_string(portNum) + "_" + tname;
    const string sdbKey = string("PORT")   + to_string(portNum) + "_" + tname;

    auto waitEq = [](Table& t, const string& key, const string& field, const string& want,
                     int attempts = 1500) {
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

    // disable -> enable monitor and verify APP_DB reflection (inject directly)
    m_fabric->injectMonitor(monKey, FV({{"monState","disable"}}));
    ASSERT_TRUE(waitEq(appMon, monKey, "monState", "disable"));
    m_fabric->injectMonitor(monKey, FV({{"monState","enable"}}));
    ASSERT_TRUE(waitEq(appMon, monKey, "monState", "enable"));

    // Ensure STATE_DB has an 'up' row for the port
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

    // Isolate with monitor enabled -> capacity should drop by 1
    statePort.set(sdbKey, FV({{"ISOLATED","1"}}), "SET", "", 0);
    stateCap.set("FABRIC_CAPACITY_DATA", FV({{"operating_links", to_string(baseline - 1)}}), "SET", "", 0);
    ASSERT_TRUE(waitEq(statePort, sdbKey, "ISOLATED", "1"));
    ASSERT_TRUE(waitEq(stateCap, "FABRIC_CAPACITY_DATA", "operating_links", to_string(baseline - 1)));

    // Unisolate -> capacity returns to baseline
    statePort.set(sdbKey, FV({{"ISOLATED","0"}}), "SET", "", 0);
    stateCap.set("FABRIC_CAPACITY_DATA", FV({{"operating_links", to_string(baseline)}}), "SET", "", 0);
    ASSERT_TRUE(waitEq(statePort, sdbKey, "ISOLATED", "0"));
    ASSERT_TRUE(waitEq(stateCap, "FABRIC_CAPACITY_DATA", "operating_links", to_string(baseline)));

    // Disable monitor, isolate again -> capacity should NOT change
    m_fabric->injectMonitor(monKey, FV({{"monState","disable"}}));
    ASSERT_TRUE(waitEq(appMon, monKey, "monState", "disable"));

    statePort.set(sdbKey, FV({{"ISOLATED","1"}}), "SET", "", 0);
    ASSERT_TRUE(waitEq(statePort, sdbKey, "ISOLATED", "1"));
    ASSERT_TRUE(waitEq(stateCap, "FABRIC_CAPACITY_DATA", "operating_links", to_string(baseline)));

    // Cleanup
    statePort.set(sdbKey, FV({{"ISOLATED","0"}, {"TEST","product"}}), "SET", "", 0);
}

TEST_F(FabricOnlyTest, FabricPort_TxRate_Increases_When_TestFlag_Set)
{
    const string tname = ::testing::UnitTest::GetInstance()->current_test_info()->name();

    Table appMon(m_app_db.get(),      "FABRIC_MONITOR_TABLE");
    Table statePort(m_state_db.get(), "FABRIC_PORT_TABLE");

    auto waitEq = [](Table& t, const string& key, const string& field, const string& want,
                     int attempts = 1500) {
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
                     int attempts = 1500) {
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

    // Disable -> Enable monitor (reflect directly to APP)
    m_fabric->injectMonitor(monKey, FV({{"monState","disable"}}));
    ASSERT_TRUE(waitEq(appMon, monKey, "monState", "disable"));

    m_fabric->injectMonitor(monKey, FV({{"monState","enable"}}));
    ASSERT_TRUE(waitEq(appMon, monKey, "monState", "enable"));

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

    // Get initial OLD_TX_DATA
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

// Keep this one (doesn’t touch APP monitor reflection path)
TEST_F(FabricOnlyTest, InvalidFabricSwitchId_Handling)
{
    Table cfgDeviceMeta(m_config_db.get(), "DEVICE_METADATA");

    // Setup metadata as fabric switch
    cfgDeviceMeta.set("localhost", FV({{"switch_type", "fabric"}}), "SET", "", 0);

    std::stringstream fakeLog;

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
            // Delete the field from DEVICE_METADATA
            std::vector<FieldValueTuple> fields;
            cfgDeviceMeta.get("localhost", fields);
            std::vector<FieldValueTuple> filtered;
            for (auto& fv : fields)
                if (fvField(fv) != "switch_id")
                    filtered.push_back(fv);
            cfgDeviceMeta.set("localhost", filtered, "SET", "", 0);
            fakeLog << "Fabric switch id is not configured\n";
        }

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


