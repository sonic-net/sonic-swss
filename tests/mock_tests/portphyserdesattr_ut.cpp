/**
 * @file portphyserdesattr_ut.cpp
 * @brief Unit tests for PORT_SERDES_ATTR flex counter orchestration
 *
 * Tests the end-to-end integration of PHY SERDES attribute collection from
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

namespace portphyserdesattr_test
{
    using namespace std;

    // Mock flex counter infrastructure
    shared_ptr<swss::DBConnector> mockFlexCounterDb;
    shared_ptr<swss::Table> mockFlexCounterTable;
    sai_switch_api_t ut_sai_switch_api;
    sai_switch_api_t *pold_sai_switch_api;
    sai_port_api_t ut_sai_port_api;
    sai_port_api_t *pold_sai_port_api;

    // Mock SAI get_port_serdes_attribute to simulate SERDES capability checks
    sai_status_t _ut_stub_sai_get_port_serdes_attribute(
        _In_ sai_object_id_t port_serdes_id,
        _In_ uint32_t attr_count,
        _Inout_ sai_attribute_t *attr_list)
    {
        // Simulate successful capability check by returning SAI_STATUS_BUFFER_OVERFLOW
        // This indicates the attribute is supported but we need to know the buffer size
        if (attr_count > 0)
        {
            if (attr_list[0].id == SAI_PORT_SERDES_ATTR_RX_VGA)
            {
                // Simulate that RX_VGA is supported with 4 lanes
                attr_list[0].value.u32list.count = 4;
                return SAI_STATUS_BUFFER_OVERFLOW;
            }
            else if (attr_list[0].id == SAI_PORT_SERDES_ATTR_TX_FIR_TAPS_LIST)
            {
                // Simulate that TX_FIR_TAPS_LIST is supported with 4 lanes
                attr_list[0].value.portserdestaps.count = 4;
                return SAI_STATUS_BUFFER_OVERFLOW;
            }
        }

        // Call original implementation for other attributes
        return pold_sai_port_api->get_port_serdes_attribute(port_serdes_id, attr_count, attr_list);
    }

    // Mock SAI set_switch_attribute to intercept flex counter operations
    sai_status_t mockFlexCounterOperation(sai_object_id_t objectId, const sai_attribute_t *attr)
    {
        if (objectId != gSwitchId)
        {
            return SAI_STATUS_FAILURE;
        }

        auto *param = reinterpret_cast<sai_redis_flex_counter_parameter_t*>(attr->value.ptr);
        std::vector<swss::FieldValueTuple> entries;
        std::string key((const char*)param->counter_key.list);

        // Extract group name and OID(s) from key (format: "GROUP_NAME:oid1,oid2,...")
        auto delimiter = key.find_first_of(":");
        if (delimiter == std::string::npos)
        {
            return SAI_STATUS_FAILURE;
        }

        std::string groupName = key.substr(0, delimiter);
        std::string strOids = key.substr(delimiter + 1);

        // Split OIDs by comma (mimics syncd behavior in Syncd.cpp:3144)
        std::vector<std::string> oidVector;
        size_t start = 0;
        size_t end = strOids.find(',');
        while (end != std::string::npos)
        {
            oidVector.push_back(strOids.substr(start, end - start));
            start = end + 1;
            end = strOids.find(',', start);
        }
        oidVector.push_back(strOids.substr(start));

        if (param->counter_ids.list != nullptr)
        {
            entries.push_back({(const char*)param->counter_field_name.list, (const char*)param->counter_ids.list});

            if (param->stats_mode.list != nullptr)
            {
                entries.push_back({STATS_MODE_FIELD, (const char*)param->stats_mode.list});
            }

            // Create individual entries for each OID (mimics syncd behavior in Syncd.cpp:3174-3177)
            for (const auto& oid : oidVector)
            {
                std::string singleKey = groupName + ":" + oid;
                mockFlexCounterTable->set(singleKey, entries);
            }
        }
        else
        {
            // Delete individual entries for each OID
            for (const auto& oid : oidVector)
            {
                std::string singleKey = groupName + ":" + oid;
                mockFlexCounterTable->del(singleKey);
            }
        }

        return SAI_STATUS_SUCCESS;
    }

    sai_status_t _ut_stub_sai_set_switch_attribute(sai_object_id_t switch_id, const sai_attribute_t *attr)
    {
        if (attr[0].id == SAI_REDIS_SWITCH_ATTR_FLEX_COUNTER)
        {
            return mockFlexCounterOperation(switch_id, attr);
        }
        return pold_sai_switch_api->set_switch_attribute(switch_id, attr);
    }

    void _hook_sai_switch_api()
    {
        mockFlexCounterDb = make_shared<swss::DBConnector>("FLEX_COUNTER_DB", 0);
        mockFlexCounterTable = make_shared<swss::Table>(mockFlexCounterDb.get(), "FLEX_COUNTER_TABLE");

        // Hook switch API for flex counter operations
        ut_sai_switch_api = *sai_switch_api;
        pold_sai_switch_api = sai_switch_api;
        ut_sai_switch_api.set_switch_attribute = _ut_stub_sai_set_switch_attribute;
        sai_switch_api = &ut_sai_switch_api;

        // Hook port API for port serdes attribute capability checks
        ut_sai_port_api = *sai_port_api;
        pold_sai_port_api = sai_port_api;
        ut_sai_port_api.get_port_serdes_attribute = _ut_stub_sai_get_port_serdes_attribute;
        sai_port_api = &ut_sai_port_api;
    }

    void _unhook_sai_switch_api()
    {
        sai_switch_api = pold_sai_switch_api;
        sai_port_api = pold_sai_port_api;
    }

    struct PortSerdesAttrTest : public ::testing::Test
    {
        PortSerdesAttrTest() {}

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

            // Hook SAI switch API to intercept flex counter operations
            _hook_sai_switch_api();
        }

        static void TearDownTestCase()
        {
            _unhook_sai_switch_api();

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
     * PORT_SERDES_ATTR flex counter enable/disable via doTask
     */
    TEST_F(PortSerdesAttrTest, EnablePortSerdesAttrFlexCounterDoTask)
    {
        ASSERT_NE(m_flexCounterOrch, nullptr);
        ASSERT_NE(gPortsOrch, nullptr);

        bool initialState = m_flexCounterOrch->getPortPhySerdesAttrCountersState();
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

        bool state = m_flexCounterOrch->getPortPhySerdesAttrCountersState();
        EXPECT_TRUE(state);
        std::cout << " PORT_PHY_SERDES_ATTR enablement verified: state = " << (state ? "ENABLED" : "DISABLED") << std::endl;

        entries.clear();
        entries.push_back({"PORT_PHY_ATTR", "SET", {{"FLEX_COUNTER_STATUS", "disable"}}});

        consumer->addToSync(entries);
        static_cast<Orch *>(m_flexCounterOrch)->doTask(*consumer);

        bool disabledState = m_flexCounterOrch->getPortPhySerdesAttrCountersState();
        EXPECT_FALSE(disabledState);
        std::cout << " PORT_PHY_SERDES_ATTR disablement verified: state = " << (disabledState ? "ENABLED" : "DISABLED") << std::endl;
    }

    /**
     * Validates that generatePortPhySerdesAttrCounterMap works with PORT_SERDES_ATTR counter type
     */
    TEST_F(PortSerdesAttrTest, generatePortPhySerdesAttrCounterMap)
    {
      	// Directly set private members via friend access
      	gPortsOrch->m_supported_phy_serdes_attrs = {
            SAI_PORT_SERDES_ATTR_RX_VGA,
            SAI_PORT_SERDES_ATTR_TX_FIR_TAPS_LIST
      	};
      	gPortsOrch->m_phy_serdes_attr_capability_checked = true;

        // should complete without exceptions
        try {
            gPortsOrch->generatePortPhySerdesAttrCounterMap();
            EXPECT_TRUE(gPortsOrch->m_isPortPhySerdesAttrCounterMapGenerated);
            std::cout << " generatePortPhySerdesAttrCounterMap() completed successfully" << std::endl;
        } catch (const std::exception& e) {
            FAIL() << "generatePortPhySerdesAttrCounterMap() threw exception: " << e.what();
        } catch (...) {
            FAIL() << "generatePortPhySerdesAttrCounterMap() threw unknown exception";
        }

        // Test clear and regenerate
        try {
            gPortsOrch->clearPortPhySerdesAttrCounterMap();
            EXPECT_FALSE(gPortsOrch->m_isPortPhySerdesAttrCounterMapGenerated);
            std::cout << " clearPortPhySerdesAttrCounterMap() completed successfully" << std::endl;

            gPortsOrch->generatePortPhySerdesAttrCounterMap();
            EXPECT_TRUE(gPortsOrch->m_isPortPhySerdesAttrCounterMapGenerated);
            std::cout << " Regenerate after clear completed successfully" << std::endl;
        } catch (const std::exception& e) {
            FAIL() << "Clear/regenerate cycle threw exception: " << e.what();
        } catch (...) {
            FAIL() << "Clear/regenerate cycle threw unknown exception";
        }

        // Verify the operations completed without crashes
        SUCCEED() << "All PORT_PHY_SERDES_ATTR counter map operations completed successfully";
    }

    TEST_F(PortSerdesAttrTest, QueryPortSerdesAttrCapabilitiesWithMockedSAI)
    {
        ASSERT_NE(gPortsOrch, nullptr);

        EXPECT_FALSE(gPortsOrch->m_phy_serdes_attr_capability_checked);
        EXPECT_TRUE(gPortsOrch->m_supported_phy_serdes_attrs.empty());

        gPortsOrch->queryPortPhySerdesAttrCapabilities();

        EXPECT_TRUE(gPortsOrch->m_phy_serdes_attr_capability_checked);
        ASSERT_FALSE(gPortsOrch->m_supported_phy_serdes_attrs.empty());

        for (const auto& attr : gPortsOrch->m_supported_phy_serdes_attrs)
        {
            EXPECT_TRUE(attr == SAI_PORT_SERDES_ATTR_RX_VGA ||
                       attr == SAI_PORT_SERDES_ATTR_TX_FIR_TAPS_LIST);
        }
    }

    TEST_F(PortSerdesAttrTest, VerifyFlexCountersDBEntriesAfterGenerate)
    {
        ASSERT_NE(gPortsOrch, nullptr);

        auto flexCounterDb = make_shared<swss::DBConnector>("FLEX_COUNTER_DB", 0);
        auto flexCounterTable = make_shared<swss::Table>(flexCounterDb.get(), "FLEX_COUNTER_TABLE");

        gPortsOrch->m_supported_phy_serdes_attrs = {
            SAI_PORT_SERDES_ATTR_RX_VGA,
            SAI_PORT_SERDES_ATTR_TX_FIR_TAPS_LIST
        };
        gPortsOrch->m_phy_serdes_attr_capability_checked = true;

        gPortsOrch->generatePortPhySerdesAttrCounterMap();
        EXPECT_TRUE(gPortsOrch->m_isPortPhySerdesAttrCounterMapGenerated);

        // Flush cached flex counters to trigger the mock SAI API which writes to FLEX_COUNTER_DB
        gPortsOrch->flushCounters();

        Port port;
        ASSERT_TRUE(gPortsOrch->getPort("Ethernet0", port));

        sai_object_id_t port_serdes_id = gPortsOrch->getPortSerdesIdFromPortId(port.m_port_id);

        if (port_serdes_id == SAI_NULL_OBJECT_ID)
        {
            SUCCEED() << "Port does not have a valid SERDES ID, skipping verification";
            return;
        }

        if (!gPortsOrch->supportsPortPhySerdesAttr(port_serdes_id, port.m_alias.c_str()))
        {
            SUCCEED();
            return;
        }

        std::string key = "PORT_PHY_SERDES_ATTR:" + sai_serialize_object_id(port_serdes_id);

        std::vector<FieldValueTuple> fieldValues;
        bool entryExists = flexCounterTable->get(key, fieldValues);

	EXPECT_TRUE(entryExists);

        if (entryExists)
        {
            bool foundCounterList = false;
            for (const auto &fv : fieldValues)
            {
                if (fvField(fv) == "PORT_PHY_SERDES_ATTR_ID_LIST")
                {
                    foundCounterList = true;
                    std::string counterList = fvValue(fv);
                    EXPECT_TRUE(counterList.find("SAI_PORT_SERDES_ATTR_RX_VGA") != std::string::npos);
                    EXPECT_TRUE(counterList.find("SAI_PORT_SERDES_ATTR_TX_FIR_TAPS_LIST") != std::string::npos);
                }
            }
            EXPECT_TRUE(foundCounterList);
        }

        gPortsOrch->clearPortPhySerdesAttrCounterMap();
        EXPECT_FALSE(gPortsOrch->m_isPortPhySerdesAttrCounterMapGenerated);

        fieldValues.clear();
        bool entryExistsAfterClear = flexCounterTable->get(key, fieldValues);
        EXPECT_FALSE(entryExistsAfterClear);
    }
} // namespace portphyserdesattr_test

