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

    // Mock SAI port API
    sai_port_api_t *old_sai_port_api;
    sai_port_api_t ut_sai_port_api;

    sai_status_t mock_get_port_attribute(
        _In_ sai_object_id_t port_id,
        _In_ uint32_t attr_count,
        _Inout_ sai_attribute_t *attr_list)
    {
        if ((attr_list[0].id == SAI_PORT_ATTR_RX_SIGNAL_DETECT
             || attr_list[0].id == SAI_PORT_ATTR_FEC_ALIGNMENT_LOCK)
            && attr_list[0].value.portlanelatchstatuslist.count == 0)
        {
            attr_list[0].value.portlanelatchstatuslist.count = 8;
            return SAI_STATUS_BUFFER_OVERFLOW;
        }
        else if (attr_list[0].id == SAI_PORT_ATTR_RX_SNR &&
                 attr_list[0].value.portsnrlist.count == 0)
        {
            attr_list[0].value.portsnrlist.count = 8;
            return SAI_STATUS_BUFFER_OVERFLOW;
        }

        // For all other attributes, call the original SAI API
        return old_sai_port_api->get_port_attribute(port_id, attr_count, attr_list);
    }

    struct PortAttrTest : public ::testing::Test
    {
        PortAttrTest() {}

        void SetUp() override
        {
            ::testing_db::reset();

            // Hook SAI port API to mock PHY attribute queries
            old_sai_port_api = sai_port_api;
            ut_sai_port_api = *sai_port_api;
            sai_port_api = &ut_sai_port_api;
            sai_port_api->get_port_attribute = mock_get_port_attribute;

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

            // Restore original SAI port API
            sai_port_api = old_sai_port_api;
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

        bool initialState = m_flexCounterOrch->getPortPhyAttrCounterState();
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

        bool state = m_flexCounterOrch->getPortPhyAttrCounterState();
        EXPECT_TRUE(state);
        std::cout << " PORT_PHY_ATTR enablement verified: state = " << (state ? "ENABLED" : "DISABLED") << std::endl;

        entries.clear();
        entries.push_back({"PORT_PHY_ATTR", "SET", {{"FLEX_COUNTER_STATUS", "disable"}}});

        consumer->addToSync(entries);
        static_cast<Orch *>(m_flexCounterOrch)->doTask(*consumer);

        bool disabledState = m_flexCounterOrch->getPortPhyAttrCounterState();
        EXPECT_FALSE(disabledState);
        std::cout << " PORT_PHY_ATTR disablement verified: state = " << (disabledState ? "ENABLED" : "DISABLED") << std::endl;
    }

    TEST_F(PortAttrTest, NoAttributesSupported)
    {
        ASSERT_NE(gPortsOrch, nullptr);

        // Test with empty supported attributes list
        gPortsOrch->m_supported_phy_attrs.clear();
        try {
            gPortsOrch->generatePortPhyAttrCounterMap();
            std::cout << "generatePortPhyAttrCounterMap() returned early (expected for unsupported platform)" << std::endl;
        } catch (const std::exception& e) {
            FAIL() << "Should not throw exception on unsupported platform: " << e.what();
        }

        SUCCEED() << "Unsupported platform scenario handled gracefully";
    }
} // namespace portphyattr_test
