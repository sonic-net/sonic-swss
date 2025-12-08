/**
 * @file portphyattr_ut.cpp
 * @brief Unit tests for PORT_PHY_ATTR flex counter orchestration
 *
 * Tests the end-to-end integration of PHY attribute collection from
 * FlexCounterOrch through PortsOrch to the FlexCounter infrastructure.
 */

#include "ut_helper.h"
#include "mock_orchagent_main.h"
#include "mock_orch_test.h"
#include "mock_table.h"

#include <memory>
#include <string>

extern SwitchOrch *gSwitchOrch;
extern PortsOrch *gPortsOrch;
extern BufferOrch *gBufferOrch;

namespace portphyattr_test
{
    using namespace std;

    struct PortAttrTest : public ::testing::Test
    {
        PortAttrTest() {}

        void SetUp() override
        {
            ::testing_db::reset();

            // Initialize database connections
            m_app_db = make_shared<swss::DBConnector>("APPL_DB", 0);
            m_config_db = make_shared<swss::DBConnector>("CONFIG_DB", 0);
            m_state_db = make_shared<swss::DBConnector>("STATE_DB", 0);
            m_chassis_app_db = make_shared<swss::DBConnector>("CHASSIS_APP_DB", 0);
            m_counters_db = make_shared<swss::DBConnector>("COUNTERS_DB", 0);

            // Create SwitchOrch dependencies
            // Required for SAI switch initialization in the mock environment
            TableConnector stateDbSwitchTable(m_state_db.get(), "SWITCH_CAPABILITY");
            TableConnector app_switch_table(m_app_db.get(), APP_SWITCH_TABLE_NAME);
            TableConnector conf_asic_sensors(m_config_db.get(), CFG_ASIC_SENSORS_TABLE_NAME);

            vector<TableConnector> switch_tables = {
                conf_asic_sensors,
                app_switch_table
            };

            ASSERT_EQ(gSwitchOrch, nullptr);
            gSwitchOrch = new SwitchOrch(m_app_db.get(), switch_tables, stateDbSwitchTable);

            // Create PortsOrch with all required table dependencies
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

            // Initialize ports using SAI default ports
            Table portTable(m_app_db.get(), APP_PORT_TABLE_NAME);
            auto ports = ut_helper::getInitialSaiPorts();
            for (const auto &it : ports)
            {
                portTable.set(it.first, it.second);
            }

            // Set PortConfigDone
            portTable.set("PortConfigDone", { { "count", to_string(ports.size()) } });
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
        shared_ptr<swss::DBConnector> m_flex_counter_db;

        FlexCounterOrch* m_flexCounterOrch = nullptr;
    };

    /**
     * PORT_PHY_ATTR flex counter enable/disable via doTask
     */
    TEST_F(PortAttrTest, EnablePortAttrFlexCounterDoTask)
    {
        ASSERT_NE(m_flexCounterOrch, nullptr);
        ASSERT_NE(gPortsOrch, nullptr); 

        bool initialState = m_flexCounterOrch->getPortAttrCountersState();
        EXPECT_FALSE(initialState);

        auto consumer = dynamic_cast<Consumer *>(m_flexCounterOrch->getExecutor(CFG_FLEX_COUNTER_TABLE_NAME));
        ASSERT_NE(consumer, nullptr);

        Table flexCounterTable(m_config_db.get(), CFG_FLEX_COUNTER_TABLE_NAME);
        vector<FieldValueTuple> fvs;
        fvs.push_back(FieldValueTuple("FLEX_COUNTER_STATUS", "enable"));
        fvs.push_back(FieldValueTuple("POLL_INTERVAL", "1000"));
        flexCounterTable.set("PORT_PHY_ATTR", fvs);
        std::cout << " CONFIG_DB configured: FLEX_COUNTER_STATUS=enable, POLL_INTERVAL=1000" << std::endl;

        std::deque<KeyOpFieldsValuesTuple> entries;
        entries.push_back({"PORT_PHY_ATTR", "SET", {
            {"FLEX_COUNTER_STATUS", "enable"},
            {"POLL_INTERVAL", "1000"}
        }});

        consumer->addToSync(entries);
        static_cast<Orch *>(m_flexCounterOrch)->doTask(*consumer);

        bool state = m_flexCounterOrch->getPortAttrCountersState();
        EXPECT_TRUE(state);
        std::cout << " PORT_PHY_ATTR enablement verified: state = " << (state ? "ENABLED" : "DISABLED") << std::endl;

        entries.clear();
        entries.push_back({"PORT_PHY_ATTR", "SET", {{"FLEX_COUNTER_STATUS", "disable"}}});

        consumer->addToSync(entries);
        static_cast<Orch *>(m_flexCounterOrch)->doTask(*consumer);

        bool disabledState = m_flexCounterOrch->getPortAttrCountersState();
        EXPECT_FALSE(disabledState);
        std::cout << " PORT_PHY_ATTR disablement verified: state = " << (disabledState ? "ENABLED" : "DISABLED") << std::endl;
    }

    /**
     * Validates that generatePortAttrCounterMap works with PORT_PHY_ATTR counter type
     */
    TEST_F(PortAttrTest, generatePortAttrCounterMap)
    {
      	// Directly set private members via friend access
      	gPortsOrch->m_supported_phy_attrs = {
            SAI_PORT_ATTR_RX_SIGNAL_DETECT,
            SAI_PORT_ATTR_FEC_ALIGNMENT_LOCK,
            SAI_PORT_ATTR_RX_SNR
      	};
      	gPortsOrch->m_phy_attr_capability_checked = true;

        // should complete without exceptions
        try {
            gPortsOrch->generatePortAttrCounterMap();
            EXPECT_TRUE(gPortsOrch->m_isPortAttrCounterMapGenerated);
            std::cout << " generatePortAttrCounterMap() completed successfully" << std::endl;
        } catch (const std::exception& e) {
            FAIL() << "generatePortAttrCounterMap() threw exception: " << e.what();
        } catch (...) {
            FAIL() << "generatePortAttrCounterMap() threw unknown exception";
        }

        // Test clear and regenerate 
        try {
            gPortsOrch->clearPortAttrCounterMap();
            EXPECT_FALSE(gPortsOrch->m_isPortAttrCounterMapGenerated);
            std::cout << " clearPortAttrCounterMap() completed successfully" << std::endl;

            gPortsOrch->generatePortAttrCounterMap();
            EXPECT_TRUE(gPortsOrch->m_isPortAttrCounterMapGenerated);
            std::cout << " Regenerate after clear completed successfully" << std::endl;
        } catch (const std::exception& e) {
            FAIL() << "Clear/regenerate cycle threw exception: " << e.what();
        } catch (...) {
            FAIL() << "Clear/regenerate cycle threw unknown exception";
        }

        // Verify the operations completed without crashes
        SUCCEED() << "All PORT_PHY_ATTR counter map operations completed successfully";
    }

    TEST_F(PortAttrTest, QueryPortAttrCapabilitiesWithMockedSAI)
    {
        ASSERT_NE(gPortsOrch, nullptr);

        EXPECT_FALSE(gPortsOrch->m_phy_attr_capability_checked);
        EXPECT_TRUE(gPortsOrch->m_supported_phy_attrs.empty());

        gPortsOrch->queryPortAttrCapabilities();

        EXPECT_TRUE(gPortsOrch->m_phy_attr_capability_checked);

        for (const auto& attr : gPortsOrch->m_supported_phy_attrs)
        {
            EXPECT_TRUE(attr == SAI_PORT_ATTR_RX_SIGNAL_DETECT ||
                       attr == SAI_PORT_ATTR_FEC_ALIGNMENT_LOCK ||
                       attr == SAI_PORT_ATTR_RX_SNR);
        }
    }
} // namespace portphyattr_test
