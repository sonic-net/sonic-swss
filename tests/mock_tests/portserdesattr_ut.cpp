/**
 * @file portserdesattr_ut.cpp
 * @brief Integration tests for PORT_SERDES_ATTR flex counter orchestration
 *
 * Tests the end-to-end integration of SERDES attribute collection from
 * FlexCounterOrch through PortsOrch to the FlexCounter infrastructure.
 */

#include "ut_helper.h"
#include "mock_orchagent_main.h"
#include "mock_orch_test.h"
#include "mock_table.h"

#include <memory>
#include <string>

// Forward declarations for global orchestrators used by the test
// These globals are needed for inter-orchestrator communication in SONiC
extern SwitchOrch *gSwitchOrch;
extern PortsOrch *gPortsOrch;
extern BufferOrch *gBufferOrch;

FlexCounterOrch *gFlexCounterOrch = nullptr;

namespace portserdesattr_test
{
    using namespace std;

    struct PortSerdesAttrTest : public ::testing::Test
    {
        PortSerdesAttrTest() {}

        void SetUp() override
        {
            // Reset the testing database to ensure clean state for each test
            ::testing_db::reset();

            // Initialize database connections
            m_app_db = make_shared<swss::DBConnector>("APPL_DB", 0);
            m_config_db = make_shared<swss::DBConnector>("CONFIG_DB", 0);
            m_state_db = make_shared<swss::DBConnector>("STATE_DB", 0);
            m_chassis_app_db = make_shared<swss::DBConnector>("CHASSIS_APP_DB", 0);
            m_counters_db = make_shared<swss::DBConnector>("COUNTERS_DB", 0);

            // Create SwitchOrch dependencies
            // SwitchOrch manages switch-level configuration and capabilities
            // Required for SAI switch initialization in the mock environment
            TableConnector stateDbSwitchTable(m_state_db.get(), "SWITCH_CAPABILITY");
            TableConnector app_switch_table(m_app_db.get(), APP_SWITCH_TABLE_NAME);
            TableConnector conf_asic_sensors(m_config_db.get(), CFG_ASIC_SENSORS_TABLE_NAME);

            vector<TableConnector> switch_tables = {
                conf_asic_sensors,
                app_switch_table
            };

            // Create SwitchOrch - required for SAI environment initialization
            ASSERT_EQ(gSwitchOrch, nullptr);
            gSwitchOrch = new SwitchOrch(m_app_db.get(), switch_tables, stateDbSwitchTable);

            // Create PortsOrch with all required table dependencies
            // PortsOrch manages port configuration and is essential for PORT_SERDES_ATTR functionality
            // The priority values determine processing order when multiple tables have updates
            const int portsorch_base_pri = 40;
            vector<table_name_with_pri_t> port_tables = {
                { APP_PORT_TABLE_NAME, portsorch_base_pri + 5 },                // Physical port config (highest priority)
                { APP_SEND_TO_INGRESS_PORT_TABLE_NAME, portsorch_base_pri + 5 }, // Ingress port forwarding
                { APP_VLAN_TABLE_NAME, portsorch_base_pri + 2 },                // VLAN configuration
                { APP_VLAN_MEMBER_TABLE_NAME, portsorch_base_pri },             // VLAN membership (lowest priority)
                { APP_LAG_TABLE_NAME, portsorch_base_pri + 4 },                 // Link aggregation groups
                { APP_LAG_MEMBER_TABLE_NAME, portsorch_base_pri }               // LAG membership
            };

            ASSERT_EQ(gPortsOrch, nullptr);
            gPortsOrch = new PortsOrch(m_app_db.get(), m_state_db.get(), port_tables, m_chassis_app_db.get());

            vector<string> flex_counter_tables = {CFG_FLEX_COUNTER_TABLE_NAME};
            m_flexCounterOrch = new FlexCounterOrch(m_config_db.get(), flex_counter_tables);
            //TODO: do we really need gFlexCounterOrch?
            gFlexCounterOrch = m_flexCounterOrch;

            // Register FlexCounterOrch in gDirectory for PortsOrch to access via gDirectory.get<FlexCounterOrch*>()
            gDirectory.set(m_flexCounterOrch);

            // Create BufferOrch - required by PortsOrch for port initialization
            vector<string> buffer_tables = { APP_BUFFER_POOL_TABLE_NAME,
                                             APP_BUFFER_PROFILE_TABLE_NAME,
                                             APP_BUFFER_QUEUE_TABLE_NAME,
                                             APP_BUFFER_PG_TABLE_NAME,
                                             APP_BUFFER_PORT_INGRESS_PROFILE_LIST_NAME,
                                             APP_BUFFER_PORT_EGRESS_PROFILE_LIST_NAME };
            gBufferOrch = new BufferOrch(m_app_db.get(), m_config_db.get(), m_state_db.get(), buffer_tables);

            // Initialize port readiness state to allow FlexCounterOrch processing
            Table portTable(m_app_db.get(), APP_PORT_TABLE_NAME);
            portTable.set("PortConfigDone", { { "count", "0" } }); // No actual ports needed for this test
            gPortsOrch->addExistingData(&portTable);
            static_cast<Orch *>(gPortsOrch)->doTask();

            // Signal that port initialization is complete
            portTable.set("PortInitDone", { { "lanes", "0" } });
            gPortsOrch->addExistingData(&portTable);
            static_cast<Orch *>(gPortsOrch)->doTask();
        }

        void TearDown() override
        {
            ::testing_db::reset();

            gDirectory.m_values.clear();

            // Clean up global references first
            gFlexCounterOrch = nullptr;
            delete m_flexCounterOrch;
            m_flexCounterOrch = nullptr;

            delete gBufferOrch;
            gBufferOrch = nullptr;

            delete gPortsOrch;
            gPortsOrch = nullptr;

            delete gSwitchOrch;
            gSwitchOrch = nullptr;
        }

        static void SetUpTestCase()
        {
            // Initialize the SAI virtual switch environment for unit testing
            map<string, string> profile = {
                { "SAI_VS_SWITCH_TYPE", "SAI_VS_SWITCH_TYPE_BCM56850" },  // Simulate Broadcom switch
                { "KV_DEVICE_MAC_ADDRESS", "20:03:04:05:06:00" }         // Test MAC address
            };

            // Initialize the SAI API with virtual switch support
            auto status = ut_helper::initSaiApi(profile);
            ASSERT_EQ(status, SAI_STATUS_SUCCESS);

            sai_attribute_t attr;

            // Create the virtual switch instance
            attr.id = SAI_SWITCH_ATTR_INIT_SWITCH;
            attr.value.booldata = true;
            status = sai_switch_api->create_switch(&gSwitchId, 1, &attr);
            ASSERT_EQ(status, SAI_STATUS_SUCCESS);

#if 0
            //enable this block iff needed.
            // Retrieve switch MAC address (required for proper initialization)
            attr.id = SAI_SWITCH_ATTR_SRC_MAC_ADDRESS;
            status = sai_switch_api->get_switch_attribute(gSwitchId, 1, &attr);
            ASSERT_EQ(status, SAI_STATUS_SUCCESS);
            gMacAddress = attr.value.mac;

            // Retrieve the default virtual router ID (required for routing functionality)
            attr.id = SAI_SWITCH_ATTR_DEFAULT_VIRTUAL_ROUTER_ID;
            status = sai_switch_api->get_switch_attribute(gSwitchId, 1, &attr);
            ASSERT_EQ(status, SAI_STATUS_SUCCESS);
            gVirtualRouterId = attr.value.oid;
#endif
        }

        static void TearDownTestCase()
        {
            auto status = sai_switch_api->remove_switch(gSwitchId);
            ASSERT_EQ(status, SAI_STATUS_SUCCESS);
            gSwitchId = 0;

            ut_helper::uninitSaiApi();
        }

        shared_ptr<swss::DBConnector> m_app_db;
        shared_ptr<swss::DBConnector> m_config_db;
        shared_ptr<swss::DBConnector> m_state_db;
        shared_ptr<swss::DBConnector> m_chassis_app_db;
        shared_ptr<swss::DBConnector> m_counters_db;

        FlexCounterOrch* m_flexCounterOrch = nullptr;
    };

    /**
     * PORT_SERDES_ATTR flex counter enable/disable via doTask
     */
    TEST_F(PortSerdesAttrTest, EnablePortSerdesAttrFlexCounterDoTask)
    {
        ASSERT_NE(m_flexCounterOrch, nullptr);
        ASSERT_NE(gFlexCounterOrch, nullptr);
        ASSERT_NE(gPortsOrch, nullptr); 
        std::cout << " Orchestrator initialization verified" << std::endl;

        bool initialState = m_flexCounterOrch->getPortSerdesAttrCountersState();
        EXPECT_FALSE(initialState);
        std::cout << " Initial state verified: PORT_SERDES_ATTR disabled (expected)" << std::endl;

        auto consumer = dynamic_cast<Consumer *>(m_flexCounterOrch->getExecutor(CFG_FLEX_COUNTER_TABLE_NAME));
        ASSERT_NE(consumer, nullptr);

        Table flexCounterTable(m_config_db.get(), CFG_FLEX_COUNTER_TABLE_NAME);
        vector<FieldValueTuple> fvs;
        fvs.push_back(FieldValueTuple("FLEX_COUNTER_STATUS", "enable"));
        fvs.push_back(FieldValueTuple("POLL_INTERVAL", "1000"));
        flexCounterTable.set("PORT_SERDES_ATTR", fvs);
        std::cout << " CONFIG_DB configured: FLEX_COUNTER_STATUS=enable, POLL_INTERVAL=1000" << std::endl;

        std::deque<KeyOpFieldsValuesTuple> entries;
        entries.push_back({"PORT_SERDES_ATTR", "SET", {
            {"FLEX_COUNTER_STATUS", "enable"},
            {"POLL_INTERVAL", "1000"}
        }});

        consumer->addToSync(entries);
        static_cast<Orch *>(m_flexCounterOrch)->doTask(*consumer);

        bool state = m_flexCounterOrch->getPortSerdesAttrCountersState();
        EXPECT_TRUE(state);
        std::cout << " PORT_SERDES_ATTR enablement verified: state = " << (state ? "ENABLED" : "DISABLED") << std::endl;

        vector<FieldValueTuple> result;
        bool exists = flexCounterTable.get("PORT_SERDES_ATTR", result);
        ASSERT_TRUE(exists);

        string status, interval;
        for (const auto& fv : result) {
            if (fvField(fv) == "FLEX_COUNTER_STATUS") {
                status = fvValue(fv);
            } else if (fvField(fv) == "POLL_INTERVAL") {
                interval = fvValue(fv);
            }
        }
        EXPECT_EQ(status, "enable");
        EXPECT_EQ(interval, "1000");
        std::cout << " Configuration values verified: STATUS=" << status << ", INTERVAL=" << interval << std::endl;

        entries.clear();
        entries.push_back({"PORT_SERDES_ATTR", "SET", {{"FLEX_COUNTER_STATUS", "disable"}}});

        consumer->addToSync(entries);
        static_cast<Orch *>(m_flexCounterOrch)->doTask(*consumer);

        bool disabledState = m_flexCounterOrch->getPortSerdesAttrCountersState();
        EXPECT_FALSE(disabledState);
        std::cout << " PORT_SERDES_ATTR disablement verified: state = " << (disabledState ? "ENABLED" : "DISABLED") << std::endl;
    }

    /**
     * Validates that generateCounterStats works with SERDES attributes and produces correct output
     */
    TEST_F(PortSerdesAttrTest, GenerateCounterStatsFunction)
    {
        const auto& serdes_attr_ids = gPortsOrch->getPortSerdesAttrIds();
        ASSERT_FALSE(serdes_attr_ids.empty());
        ASSERT_EQ(serdes_attr_ids.size(), 3); 

        try {
            auto result = gPortsOrch->generateCounterStats(serdes_attr_ids, sai_serialize_port_attr);
            ASSERT_EQ(result.size(), 3) << "generateCounterStats() should return 3 serialized SERDES attributes";

            std::string expectedRxSignalDetect = std::to_string(static_cast<uint32_t>(SAI_PORT_ATTR_RX_SIGNAL_DETECT));
            std::string expectedFecAlignmentLock = std::to_string(static_cast<uint32_t>(SAI_PORT_ATTR_FEC_ALIGNMENT_LOCK));
            std::string expectedRxSnr = std::to_string(static_cast<uint32_t>(SAI_PORT_ATTR_RX_SNR));

            bool foundRxSignalDetect = false;
            bool foundFecAlignmentLock = false;
            bool foundRxSnr = false;

            for (const auto& serialized_attr : result) {
                if (serialized_attr == expectedRxSignalDetect) {
                    foundRxSignalDetect = true;
                }
                else if (serialized_attr == expectedFecAlignmentLock) {
                    foundFecAlignmentLock = true;
                }
                else if (serialized_attr == expectedRxSnr) {
                    foundRxSnr = true;
                }
            }

            EXPECT_TRUE(foundRxSignalDetect) << "Serialized RX_SIGNAL_DETECT (" << expectedRxSignalDetect << ") not found in output";
            EXPECT_TRUE(foundFecAlignmentLock) << "Serialized FEC_ALIGNMENT_LOCK (" << expectedFecAlignmentLock << ") not found in output";
            EXPECT_TRUE(foundRxSnr) << "Serialized RX_SNR (" << expectedRxSnr << ") not found in output";
            std::cout << " All expected SERDES attributes found in serialized output" << std::endl;

        } catch (const std::exception& e) {
            FAIL() << "generateCounterStats() failed with exception: " << e.what();
        } catch (...) {
            FAIL() << "generateCounterStats() failed with unknown exception";
        }
    }

    /**
     * Validates that generatePortSerdesAttrCounterMap works with PORT_SERDES_ATTR counter type
     */
    TEST_F(PortSerdesAttrTest, generatePortSerdesAttrCounterMap)
    {
        // Create a test port in the database
        Table portTable(m_app_db.get(), APP_PORT_TABLE_NAME);
        vector<FieldValueTuple> fvs;
        fvs.push_back(FieldValueTuple("lanes", "65,66,67,68"));
        fvs.push_back(FieldValueTuple("speed", "100000"));
        fvs.push_back(FieldValueTuple("admin_status", "up"));
        portTable.set("Ethernet0", fvs);

        // Process port creation
        gPortsOrch->addExistingData(&portTable);
        static_cast<Orch *>(gPortsOrch)->doTask();
        std::cout << " Test port Ethernet0 configured" << std::endl;

        // Test setCounterIdList through generatePortSerdesAttrCounterMap
        // This function calls setCounterIdList for each PHY port with CounterType::PORT_SERDES_ATTR
        // TODO:: this is still incomplete need to figure out a way to check setCounterIdList did its job as expected.
        // We can probably check the FLEX_COUNTERS_DB
        try {
            gPortsOrch->generatePortSerdesAttrCounterMap();
            std::cout << " setCounterIdList() executed successfully with PORT_SERDES_ATTR counter type" << std::endl;
        } catch (...) {
            FAIL() << "setCounterIdList() failed with PORT_SERDES_ATTR counter type";
        }

        // Add another port to test multiple port handling
        fvs.clear();
        fvs.push_back(FieldValueTuple("lanes", "69,70,71,72"));
        fvs.push_back(FieldValueTuple("speed", "100000"));
        fvs.push_back(FieldValueTuple("admin_status", "up"));
        portTable.set("Ethernet4", fvs);

        gPortsOrch->addExistingData(&portTable);
        static_cast<Orch *>(gPortsOrch)->doTask();

        // Test setCounterIdList with multiple ports
        try {
            gPortsOrch->clearPortSerdesAttrCounterMap(); // Clear first
            gPortsOrch->generatePortSerdesAttrCounterMap(); // Regenerate for both ports
            std::cout << " setCounterIdList() handles multiple ports correctly" << std::endl;
        } catch (...) {
            FAIL() << "setCounterIdList() failed with multiple ports";
        }

        // Clean up the ports to avoid SAI reference counting issues
        portTable.del("Ethernet0");
        portTable.del("Ethernet4");
        gPortsOrch->addExistingData(&portTable);
        static_cast<Orch *>(gPortsOrch)->doTask();
        std::cout << " Test ports cleaned up" << std::endl;
    }
} // namespace portserdesattr_test
