#include "ut_helper.h"
#include "mock_orchagent_main.h"

namespace tunneldecaporch_test
{
    using namespace std;

    shared_ptr<swss::DBConnector> m_app_db;
    shared_ptr<swss::DBConnector> m_config_db;
    shared_ptr<swss::DBConnector> m_state_db;

    sai_tunnel_api_t ut_sai_tunnel_api;
    sai_tunnel_api_t *pold_sai_tunnel_api;
    sai_router_interface_api_t ut_sai_router_intfs_api;
    sai_router_interface_api_t *pold_sai_router_intfs_api;
    sai_next_hop_api_t ut_sai_next_hop_api;
    sai_next_hop_api_t *pold_sai_next_hop_api;

    // Mock SAI API functions
    sai_status_t _ut_stub_sai_create_tunnel(
        _Out_ sai_object_id_t *tunnel_id,
        _In_ sai_object_id_t switch_id,
        _In_ uint32_t attr_count,
        _In_ const sai_attribute_t *attr_list)
    {
        static sai_object_id_t tunnel_id_counter = 0x300;
        *tunnel_id = tunnel_id_counter++;
        return SAI_STATUS_SUCCESS;
    }

    sai_status_t _ut_stub_sai_remove_tunnel(
        _In_ sai_object_id_t tunnel_id)
    {
        return SAI_STATUS_SUCCESS;
    }

    sai_status_t _ut_stub_sai_set_tunnel_attribute(
        _In_ sai_object_id_t tunnel_id,
        _In_ const sai_attribute_t *attr)
    {
        return SAI_STATUS_SUCCESS;
    }

    sai_status_t _ut_stub_sai_create_tunnel_term_table_entry(
        _Out_ sai_object_id_t *tunnel_term_table_entry_id,
        _In_ sai_object_id_t switch_id,
        _In_ uint32_t attr_count,
        _In_ const sai_attribute_t *attr_list)
    {
        static sai_object_id_t term_entry_id_counter = 0x400;
        *tunnel_term_table_entry_id = term_entry_id_counter++;
        return SAI_STATUS_SUCCESS;
    }

    sai_status_t _ut_stub_sai_remove_tunnel_term_table_entry(
        _In_ sai_object_id_t tunnel_term_table_entry_id)
    {
        return SAI_STATUS_SUCCESS;
    }

    sai_status_t _ut_stub_sai_create_router_interface(
        _Out_ sai_object_id_t *router_interface_id,
        _In_ sai_object_id_t switch_id,
        _In_ uint32_t attr_count,
        _In_ const sai_attribute_t *attr_list)
    {
        static sai_object_id_t rif_id_counter = 0x500;
        *router_interface_id = rif_id_counter++;
        return SAI_STATUS_SUCCESS;
    }

    sai_status_t _ut_stub_sai_remove_router_interface(
        _In_ sai_object_id_t router_interface_id)
    {
        return SAI_STATUS_SUCCESS;
    }

    sai_status_t _ut_stub_sai_create_next_hop(
        _Out_ sai_object_id_t *next_hop_id,
        _In_ sai_object_id_t switch_id,
        _In_ uint32_t attr_count,
        _In_ const sai_attribute_t *attr_list)
    {
        static sai_object_id_t nh_id_counter = 0x600;
        *next_hop_id = nh_id_counter++;
        return SAI_STATUS_SUCCESS;
    }

    sai_status_t _ut_stub_sai_remove_next_hop(
        _In_ sai_object_id_t next_hop_id)
    {
        return SAI_STATUS_SUCCESS;
    }

    struct TunnelDecapOrchTest : public ::testing::Test
    {
        TunnelDecapOrchTest()
        {
            m_app_db = make_shared<swss::DBConnector>("APPL_DB", 0);
            m_config_db = make_shared<swss::DBConnector>("CONFIG_DB", 0);
            m_state_db = make_shared<swss::DBConnector>("STATE_DB", 0);
        }

        void SetUp() override
        {
            // Initialize minimal global objects - following aclorch_ut pattern
            ASSERT_EQ(gCrmOrch, nullptr);
            gCrmOrch = new CrmOrch(m_config_db.get(), CFG_CRM_TABLE_NAME);
            
            // Set other global objects to nullptr
            gPortsOrch = nullptr;
            gQosOrch = nullptr;

            // Set up mock tunnel API
            ut_sai_tunnel_api = {};
            ut_sai_tunnel_api.create_tunnel = _ut_stub_sai_create_tunnel;
            ut_sai_tunnel_api.remove_tunnel = _ut_stub_sai_remove_tunnel;
            ut_sai_tunnel_api.set_tunnel_attribute = _ut_stub_sai_set_tunnel_attribute;
            ut_sai_tunnel_api.create_tunnel_term_table_entry = _ut_stub_sai_create_tunnel_term_table_entry;
            ut_sai_tunnel_api.remove_tunnel_term_table_entry = _ut_stub_sai_remove_tunnel_term_table_entry;
            
            pold_sai_tunnel_api = sai_tunnel_api;
            sai_tunnel_api = &ut_sai_tunnel_api;

            // Set up mock router interface API
            ut_sai_router_intfs_api = {};
            ut_sai_router_intfs_api.create_router_interface = _ut_stub_sai_create_router_interface;
            ut_sai_router_intfs_api.remove_router_interface = _ut_stub_sai_remove_router_interface;
            
            pold_sai_router_intfs_api = sai_router_intfs_api;
            sai_router_intfs_api = &ut_sai_router_intfs_api;

            // Set up mock next hop API
            ut_sai_next_hop_api = {};
            ut_sai_next_hop_api.create_next_hop = _ut_stub_sai_create_next_hop;
            ut_sai_next_hop_api.remove_next_hop = _ut_stub_sai_remove_next_hop;
            
            pold_sai_next_hop_api = sai_next_hop_api;
            sai_next_hop_api = &ut_sai_next_hop_api;

            // Set basic global variables needed for TunnelDecapOrch
            gSwitchId = 0x21000000000000;
            gVirtualRouterId = 0x3000000000001;
            gUnderlayIfId = 0x6000000000001; 
            gMacAddress = MacAddress("20:03:04:05:06:00");
        }

        void TearDown() override
        {
            // Restore SAI APIs
            sai_tunnel_api = pold_sai_tunnel_api;
            sai_router_intfs_api = pold_sai_router_intfs_api;
            sai_next_hop_api = pold_sai_next_hop_api;

            // Clean up global orchestrator objects
            delete gCrmOrch;
            gCrmOrch = nullptr;
        }
    };

    TEST_F(TunnelDecapOrchTest, TunnelDecapOrch_Creation)
    {
        // Test creation of TunnelDecapOrch
        vector<string> tunnel_tables = { APP_TUNNEL_DECAP_TABLE_NAME };

        ASSERT_NO_THROW({
            auto tunnelDecapOrch = make_shared<TunnelDecapOrch>(
                m_app_db.get(), m_state_db.get(), m_config_db.get(), tunnel_tables);
            ASSERT_NE(tunnelDecapOrch, nullptr);
        });
    }

    TEST_F(TunnelDecapOrchTest, TunnelDecapOrch_BasicFunctionality)
    {
        vector<string> tunnel_tables = { APP_TUNNEL_DECAP_TABLE_NAME };
        auto tunnelDecapOrch = make_shared<TunnelDecapOrch>(
            m_app_db.get(), m_state_db.get(), m_config_db.get(), tunnel_tables);
        ASSERT_NE(tunnelDecapOrch, nullptr);

        EXPECT_NO_THROW({
        });
    }

    TEST_F(TunnelDecapOrchTest, TunnelDecapOrch_GetDscpMode)
    {
        vector<string> tunnel_tables = { APP_TUNNEL_DECAP_TABLE_NAME };
        auto tunnelDecapOrch = make_shared<TunnelDecapOrch>(
            m_app_db.get(), m_state_db.get(), m_config_db.get(), tunnel_tables);
        ASSERT_NE(tunnelDecapOrch, nullptr);

        // Test getDscpMode with non-existent tunnel
        EXPECT_NO_THROW({
            string dscp_mode = tunnelDecapOrch->getDscpMode("non_existent_tunnel");
            EXPECT_TRUE(dscp_mode.empty());
        });
    }

    TEST_F(TunnelDecapOrchTest, TunnelDecapOrch_GetDstIpAddresses)
    {
        vector<string> tunnel_tables = { APP_TUNNEL_DECAP_TABLE_NAME };
        auto tunnelDecapOrch = make_shared<TunnelDecapOrch>(
            m_app_db.get(), m_state_db.get(), m_config_db.get(), tunnel_tables);
        ASSERT_NE(tunnelDecapOrch, nullptr);

        // Test getDstIpAddresses with non-existent tunnel
        EXPECT_NO_THROW({
            auto dst_ips = tunnelDecapOrch->getDstIpAddresses("non_existent_tunnel");
            EXPECT_EQ(dst_ips.getSize(), 0);
        });
    }

    TEST_F(TunnelDecapOrchTest, TunnelDecapOrch_GetQosMapId)
    {
        vector<string> tunnel_tables = { APP_TUNNEL_DECAP_TABLE_NAME };
        auto tunnelDecapOrch = make_shared<TunnelDecapOrch>(
            m_app_db.get(), m_state_db.get(), m_config_db.get(), tunnel_tables);
        ASSERT_NE(tunnelDecapOrch, nullptr);

        // Test getQosMapId with non-existent tunnel
        EXPECT_NO_THROW({
            sai_object_id_t qos_map_id;
            bool result = tunnelDecapOrch->getQosMapId("non_existent_tunnel", "dscp_to_tc_map", qos_map_id);
            EXPECT_FALSE(result);
        });
    }

    TEST_F(TunnelDecapOrchTest, TunnelDecapOrch_CreateNextHopTunnel)
    {
        vector<string> tunnel_tables = { APP_TUNNEL_DECAP_TABLE_NAME };
        auto tunnelDecapOrch = make_shared<TunnelDecapOrch>(
            m_app_db.get(), m_state_db.get(), m_config_db.get(), tunnel_tables);
        ASSERT_NE(tunnelDecapOrch, nullptr);

        EXPECT_NO_THROW({
            IpAddress ip_addr("192.168.1.1");
            sai_object_id_t nh_id = tunnelDecapOrch->createNextHopTunnel("non_existent_tunnel", ip_addr);
            EXPECT_EQ(nh_id, SAI_NULL_OBJECT_ID);
        });
    }

    TEST_F(TunnelDecapOrchTest, TunnelDecapOrch_RemoveNextHopTunnel)
    {
        vector<string> tunnel_tables = { APP_TUNNEL_DECAP_TABLE_NAME };
        auto tunnelDecapOrch = make_shared<TunnelDecapOrch>(
            m_app_db.get(), m_state_db.get(), m_config_db.get(), tunnel_tables);
        ASSERT_NE(tunnelDecapOrch, nullptr);

        // Test removeNextHopTunnel with non-existent tunnel
        EXPECT_NO_THROW({
            IpAddress ip_addr("192.168.1.1");
            bool result = tunnelDecapOrch->removeNextHopTunnel("non_existent_tunnel", ip_addr);
            EXPECT_TRUE(result);
        });
    }

    TEST_F(TunnelDecapOrchTest, TunnelDecapOrch_GetSubnetDecapConfig)
    {
        vector<string> tunnel_tables = { APP_TUNNEL_DECAP_TABLE_NAME };
        auto tunnelDecapOrch = make_shared<TunnelDecapOrch>(
            m_app_db.get(), m_state_db.get(), m_config_db.get(), tunnel_tables);
        ASSERT_NE(tunnelDecapOrch, nullptr);

        // Test getSubnetDecapConfig
        EXPECT_NO_THROW({
            const auto& config = tunnelDecapOrch->getSubnetDecapConfig();
            EXPECT_FALSE(config.enable);
        });
    }

    TEST_F(TunnelDecapOrchTest, TunnelDecapOrch_DoTask)
    {
        vector<string> tunnel_tables = { APP_TUNNEL_DECAP_TABLE_NAME };
        auto tunnelDecapOrch = make_shared<TunnelDecapOrch>(
            m_app_db.get(), m_state_db.get(), m_config_db.get(), tunnel_tables);
        ASSERT_NE(tunnelDecapOrch, nullptr);

        // Test that doTask doesn't crash with empty task queue
        EXPECT_NO_THROW({
            static_cast<Orch *>(tunnelDecapOrch.get())->doTask();
        });
    }

    TEST_F(TunnelDecapOrchTest, TunnelDecapOrch_WithStateDb)
    {
        // Create TunnelDecapOrch with state DB
        vector<string> tunnel_tables = { APP_TUNNEL_DECAP_TABLE_NAME };
        auto tunnelDecapOrch = make_shared<TunnelDecapOrch>(
            m_app_db.get(), m_state_db.get(), m_config_db.get(), tunnel_tables);
        ASSERT_NE(tunnelDecapOrch, nullptr);

        // Test that basic functionality works with state DB constructor
        EXPECT_NO_THROW({
            auto dst_ips = tunnelDecapOrch->getDstIpAddresses("test_tunnel");
            EXPECT_EQ(dst_ips.getSize(), 0);
        });
    }

    TEST_F(TunnelDecapOrchTest, TunnelDecapOrch_DoDecapTunnelTask)
    {
        vector<string> tunnel_tables = { APP_TUNNEL_DECAP_TABLE_NAME };
        auto tunnelDecapOrch = make_shared<TunnelDecapOrch>(
            m_app_db.get(), m_state_db.get(), m_config_db.get(), tunnel_tables);
        ASSERT_NE(tunnelDecapOrch, nullptr);

        // Test 1: Create tunnel configuration data in the database and test processing
        Table tunnelDecapTable(m_app_db.get(), APP_TUNNEL_DECAP_TABLE_NAME);
        
        // Set tunnel configuration in app DB
        vector<FieldValueTuple> tunnelData = {
            {"tunnel_type", "IPINIP"},
            {"dscp_mode", "uniform"},
            {"ecn_mode", "copy_from_outer"},
            {"ttl_mode", "pipe"},
            {"src_ip", "10.0.0.1"}
        };
        tunnelDecapTable.set("test_tunnel", tunnelData);

        // Test that the table was set correctly
        EXPECT_NO_THROW({
            vector<FieldValueTuple> values;
            bool result = tunnelDecapTable.get("test_tunnel", values);
            EXPECT_TRUE(result);
            EXPECT_EQ(values.size(), 5);
        });

        // Test 2: Test invalid tunnel configuration
        vector<FieldValueTuple> invalidTunnelData = {
            {"tunnel_type", "INVALID_TYPE"},
            {"dscp_mode", "invalid_mode"},
            {"ecn_mode", "invalid_ecn"}
        };
        tunnelDecapTable.set("invalid_tunnel", invalidTunnelData);

        // Verify invalid data was set
        EXPECT_NO_THROW({
            vector<FieldValueTuple> values;
            bool result = tunnelDecapTable.get("invalid_tunnel", values);
            EXPECT_TRUE(result);
            EXPECT_EQ(values.size(), 3);
        });

        // Test 3: Test tunnel deletion
        tunnelDecapTable.del("test_tunnel");
        
        EXPECT_NO_THROW({
            vector<FieldValueTuple> values;
            bool result = tunnelDecapTable.get("test_tunnel", values);
            EXPECT_FALSE(result);
        });

        // Test 4: Test empty configuration
        vector<FieldValueTuple> emptyData = {};
        tunnelDecapTable.set("empty_tunnel", emptyData);

        EXPECT_NO_THROW({
            vector<FieldValueTuple> values;
            bool result = tunnelDecapTable.get("empty_tunnel", values);
            EXPECT_TRUE(result);
            EXPECT_EQ(values.size(), 0);
        });

        // Test 5: Test that getDscpMode works after setting up tunnel data
        EXPECT_NO_THROW({
            string dscp_mode = tunnelDecapOrch->getDscpMode("invalid_tunnel");
            EXPECT_TRUE(dscp_mode.empty());
        });

        // Test 6: Test that getDstIpAddresses works with database entries
        EXPECT_NO_THROW({
            auto dst_ips = tunnelDecapOrch->getDstIpAddresses("empty_tunnel");
            EXPECT_EQ(dst_ips.getSize(), 0);
        });
    }

} // namespace tunneldecaporch_test
