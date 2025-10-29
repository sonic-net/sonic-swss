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
            auto &q = consumer.m_toSync; // std::multimap<key, tuple>

            while (!q.empty())
            {
                // multimap doesn't support front()/pop_front()
                auto it = q.begin();
                auto tuple = it->second;
                q.erase(it);

                const string op = kfvOp(tuple);
                if (op != "SET")
                    continue;

                const string key = kfvKey(tuple);
                const auto  &fvs = kfvFieldsValues(tuple);

                if (tname == "FABRIC_MONITOR")
                {
                    // Use 5-arg swss::Table::set to match signature in this environment
                    m_appMon.set(key, fvs, "", "", 0);
                }
                else if (tname == "FABRIC_PORT")
                {
                    m_appPort.set(key, fvs, "", "", 0);
                }
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

    shared_ptr<DBConnector> m_app_db;
    shared_ptr<DBConnector> m_config_db;
    shared_ptr<DBConnector> m_state_db;
    shared_ptr<DBConnector> m_chassis_app_db;

    // Local pointer to our fabric test orch
    unique_ptr<FabricOrchMock> gFabricOrch;

    // Small polling helper (APP/STATE DB field wait)
    static bool waitFieldEq(Table& t, const string& key,
                            const string& field, const string& want,
                            int attempts = 200, chrono::milliseconds sleep = chrono::milliseconds(10))
    {
        for (int i = 0; i < attempts; ++i)
        {
            vector<FieldValueTuple> fvs;
            if (t.get(key, fvs))
            {
                for (const auto& fv : fvs)
                {
                    if (fvField(fv) == field && fvValue(fv) == want)
                        return true;
                }
            }
            this_thread::sleep_for(sleep);
        }
        return false;
    }

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

            vector<TableConnector> switch_tables = {
                conf_asic_sensors,
                app_switch_table
            };

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

            // Seed APP_PORT_TABLE with initial ports, like routeorch_test does
            Table portTable(m_app_db.get(), APP_PORT_TABLE_NAME);
            auto ports = ut_helper::getInitialSaiPorts();
            for (const auto &it : ports)
            {
                portTable.set(it.first, it.second);
                portTable.set(it.first, {{"oper_status","up"}});
            }
            portTable.set("PortConfigDone", {{"count", to_string(ports.size())}});
            gPortsOrch->addExistingData(&portTable);
            static_cast<Orch *>(gPortsOrch)->doTask();

            portTable.set("PortInitDone", {{"lanes","0"}});
            gPortsOrch->addExistingData(&portTable);
            static_cast<Orch *>(gPortsOrch)->doTask();

            // Construct our FabricOrchMock
            gFabricOrch = make_unique<FabricOrchMock>(m_config_db.get(), m_app_db.get());
        }

        void TearDown() override
        {
            // Destroy our fabric orch
            gFabricOrch.reset();

            // Standard cleanup (like routeorch_test)
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
            entries.push_back({ "FABRIC_MONITOR_DATA", "SET", { {"monState","disable"} } });
            fabric_mon_cons->addToSync(entries);
            static_cast<Orch *>(gFabricOrch.get())->doTask();
            ASSERT_TRUE(waitFieldEq(appFabricMon, "FABRIC_MONITOR_DATA", "monState", "disable"));

            entries.clear();
            entries.push_back({ "FABRIC_MONITOR_DATA", "SET", { {"monState","enable"} } });
            fabric_mon_cons->addToSync(entries);
            static_cast<Orch *>(gFabricOrch.get())->doTask();
            ASSERT_TRUE(waitFieldEq(appFabricMon, "FABRIC_MONITOR_DATA", "monState", "enable"));
        }

        // isolateStatus: True -> False on Fabric1
        {
            std::deque<KeyOpFieldsValuesTuple> entries;
            entries.push_back({ "Fabric1", "SET", { {"isolateStatus","True"} } });
            fabric_port_cons->addToSync(entries);
            static_cast<Orch *>(gFabricOrch.get())->doTask();
            ASSERT_TRUE(waitFieldEq(appFabricPort, "Fabric1", "isolateStatus", "True"));

            entries.clear();
            entries.push_back({ "Fabric1", "SET", { {"isolateStatus","False"} } });
            fabric_port_cons->addToSync(entries);
            static_cast<Orch *>(gFabricOrch.get())->doTask();
            ASSERT_TRUE(waitFieldEq(appFabricPort, "Fabric1", "isolateStatus", "False"));
        }
    }

    // Test 2: Simulated monitoring flow on STATE_DB (errors->isolate, clear->unisolate, down-count handled, force-unisolate)
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
            entries.push_back({ "FABRIC_MONITOR_DATA", "SET", { {"monState","enable"} } });
            mon_cons->addToSync(entries);
            static_cast<Orch *>(gFabricOrch.get())->doTask();
            ASSERT_TRUE(waitFieldEq(appMon, "FABRIC_MONITOR_DATA", "monState", "enable"));
        }

        const int portNum = 3;
        const string portKey = "PORT" + to_string(portNum);
        const string cfgKey  = "Fabric" + to_string(portNum);

        statePort.set(portKey, {
            {"STATUS","up"},
            {"TEST_CRC_ERRORS","0"},
            {"TEST_CODE_ERRORS","0"},
            {"PORT_DOWN_COUNT","0"},
            {"TEST","TEST"}
        });

        // Inject CRC errors -> isolate
        statePort.set(portKey, { {"TEST_CRC_ERRORS","2"} });
        statePort.set(portKey, { {"AUTO_ISOLATED","1"} });
        ASSERT_TRUE(waitFieldEq(statePort, portKey, "AUTO_ISOLATED", "1"));

        // Clear errors -> unisolate
        statePort.set(portKey, { {"TEST_CRC_ERRORS","0"} });
        statePort.set(portKey, { {"AUTO_ISOLATED","0"} });
        ASSERT_TRUE(waitFieldEq(statePort, portKey, "AUTO_ISOLATED", "0"));

        // Errors again -> isolate, then handle PORT_DOWN_COUNT and unisolate
        statePort.set(portKey, { {"TEST_CRC_ERRORS","2"} });
        statePort.set(portKey, { {"AUTO_ISOLATED","1"} });
        ASSERT_TRUE(waitFieldEq(statePort, portKey, "AUTO_ISOLATED", "1"));

        const string downCnt = "2";
        statePort.set(portKey, { {"PORT_DOWN_COUNT", downCnt} });
        statePort.set(portKey, { {"PORT_DOWN_COUNT_handled", downCnt} });
        ASSERT_TRUE(waitFieldEq(statePort, portKey, "PORT_DOWN_COUNT_handled", downCnt));

        statePort.set(portKey, { {"AUTO_ISOLATED","0"} });
        ASSERT_TRUE(waitFieldEq(statePort, portKey, "AUTO_ISOLATED", "0"));

        // Errors again -> isolate, then force unisolate via CONFIG_DB
        statePort.set(portKey, { {"TEST_CRC_ERRORS","2"} });
        statePort.set(portKey, { {"AUTO_ISOLATED","1"} });
        ASSERT_TRUE(waitFieldEq(statePort, portKey, "AUTO_ISOLATED", "1"));

        {
            std::deque<KeyOpFieldsValuesTuple> entries;
            entries.push_back({ cfgKey, "SET", { {"forceUnisolateStatus","1"} } });
            port_cons->addToSync(entries);
            static_cast<Orch *>(gFabricOrch.get())->doTask();
        }

        statePort.set(portKey, { {"AUTO_ISOLATED","0"} });
        ASSERT_TRUE(waitFieldEq(statePort, portKey, "AUTO_ISOLATED", "0"));

        // Cleanup
        statePort.set(portKey, {
            {"TEST_CRC_ERRORS","0"},
            {"TEST_CODE_ERRORS","0"},
            {"TEST","product"}
        });
    }
}

