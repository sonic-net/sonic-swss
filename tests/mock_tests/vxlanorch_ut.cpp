#include "table.h"
#include "producerstatetable.h"
#include "consumerstatetable.h"
#include <memory>

#define private public
#define protected public

#include "directory.h"
#include "orch.h"
#include "vxlanorch.h"
#include "portsorch.h"

#undef protected
#undef private

#include "ut_helper.h"
#include "common/vxlan_ut_helpers.h"
#include "mock_orchagent_main.h"
#include "mock_sai_api.h"
#include "mock_sai_tunnel.h"
#include "mock_orch_test.h"

extern Directory<Orch*> gDirectory;
extern PortsOrch *gPortsOrch;
extern string gMySwitchType;

namespace vxlanorch_test
{
    using namespace std;
    using namespace swss;
    using namespace mock_orch_test;
    using ::testing::_;
    using ::testing::Return;
    using ::testing::DoAll;
    using ::testing::SetArgPointee;
    using ::testing::Throw;
    using ::testing::StrictMock;

    constexpr sai_object_id_t vxlan_tunnel_oid = 0x1232;
    constexpr sai_object_id_t vxlan_tunnel_map_oid = 0x1240;
    constexpr sai_object_id_t vxlan_tunnel_term_table_entry_oid = 0x1248;
    constexpr sai_object_id_t vxlan_tunnel_map_entry_oid = 0x1256;

    class VxlanOrchTest : public ::testing::Test
    {
        public:
            VxlanOrchTest()
            {
            };

            ~VxlanOrchTest()
            {
            };


            void _hook_sai_apis()
            {
                mock_sai_tunnel = &mock_sai_tunnel_;

                saved_create_tunnel = sai_tunnel_api->create_tunnel;
                saved_create_tunnel_map = sai_tunnel_api->create_tunnel_map;
                saved_create_tunnel_map_entry = sai_tunnel_api->create_tunnel_map_entry;
                saved_create_tunnel_term_table_entry = sai_tunnel_api->create_tunnel_term_table_entry;
                saved_remove_tunnel = sai_tunnel_api->remove_tunnel;
                saved_remove_tunnel_map = sai_tunnel_api->remove_tunnel_map;
                saved_remove_tunnel_map_entry = sai_tunnel_api->remove_tunnel_map_entry;
                saved_remove_tunnel_term_table_entry = sai_tunnel_api->remove_tunnel_term_table_entry;

                sai_tunnel_api->create_tunnel = mock_create_tunnel;
                sai_tunnel_api->create_tunnel_map = mock_create_tunnel_map;
                sai_tunnel_api->create_tunnel_map_entry = mock_create_tunnel_map_entry;
                sai_tunnel_api->create_tunnel_term_table_entry = mock_create_tunnel_term_table_entry;
                sai_tunnel_api->remove_tunnel = mock_remove_tunnel;
                sai_tunnel_api->remove_tunnel_map = mock_remove_tunnel_map;
                sai_tunnel_api->remove_tunnel_map_entry = mock_remove_tunnel_map_entry;
                sai_tunnel_api->remove_tunnel_term_table_entry = mock_remove_tunnel_term_table_entry;
            }

            void _unhook_sai_apis()
            {
                if (sai_tunnel_api)
                {
                    sai_tunnel_api->create_tunnel = saved_create_tunnel;
                    sai_tunnel_api->create_tunnel_map = saved_create_tunnel_map;
                    sai_tunnel_api->create_tunnel_map_entry = saved_create_tunnel_map_entry;
                    sai_tunnel_api->create_tunnel_term_table_entry = saved_create_tunnel_term_table_entry;
                    sai_tunnel_api->remove_tunnel = saved_remove_tunnel;
                    sai_tunnel_api->remove_tunnel_map = saved_remove_tunnel_map;
                    sai_tunnel_api->remove_tunnel_map_entry = saved_remove_tunnel_map_entry;
                    sai_tunnel_api->remove_tunnel_term_table_entry = saved_remove_tunnel_term_table_entry;
                }

                mock_sai_tunnel = nullptr;
            }

        protected:
            StrictMock<MockSaiTunnel> mock_sai_tunnel_;
            decltype(sai_tunnel_api->create_tunnel) saved_create_tunnel{};
            decltype(sai_tunnel_api->create_tunnel_map) saved_create_tunnel_map{};
            decltype(sai_tunnel_api->create_tunnel_map_entry) saved_create_tunnel_map_entry{};
            decltype(sai_tunnel_api->create_tunnel_term_table_entry) saved_create_tunnel_term_table_entry{};
            decltype(sai_tunnel_api->remove_tunnel) saved_remove_tunnel{};
            decltype(sai_tunnel_api->remove_tunnel_map) saved_remove_tunnel_map{};
            decltype(sai_tunnel_api->remove_tunnel_map_entry) saved_remove_tunnel_map_entry{};
            decltype(sai_tunnel_api->remove_tunnel_term_table_entry) saved_remove_tunnel_term_table_entry{};

            shared_ptr<swss::DBConnector> m_app_db;
            shared_ptr<swss::DBConnector> m_config_db;
            shared_ptr<swss::DBConnector> m_state_db;
            shared_ptr<swss::DBConnector> m_chassis_app_db;
            VxlanTunnelOrch *m_vxlan_tunnel_orch = nullptr;
            VxlanTunnelMapOrch *m_vxlanTunnelMapOrch = nullptr;
            EvpnNvoOrch *m_evpnNvoOrch = nullptr;
            EvpnRemoteVnip2pOrch *m_evpnRemoteVnip2pOrch = nullptr;

            void SetUp() override
            {
                // Init switch and create dependencies
                m_app_db = make_shared<swss::DBConnector>("APPL_DB", 0);
                m_config_db = make_shared<swss::DBConnector>("CONFIG_DB", 0);
                m_state_db = make_shared<swss::DBConnector>("STATE_DB", 0);
                if(gMySwitchType == "voq")
                {
                    m_chassis_app_db = make_shared<swss::DBConnector>("CHASSIS_APP_DB", 0);
                }

                m_vxlanTunnelMapOrch = new VxlanTunnelMapOrch(m_app_db.get(), APP_VXLAN_TUNNEL_MAP_TABLE_NAME);
                m_evpnNvoOrch = new EvpnNvoOrch(m_app_db.get(), APP_VXLAN_EVPN_NVO_TABLE_NAME);
                m_evpnRemoteVnip2pOrch = new EvpnRemoteVnip2pOrch(m_app_db.get(), APP_VXLAN_REMOTE_VNI_TABLE_NAME);

                gDirectory.set(m_vxlanTunnelMapOrch);
                gDirectory.set(m_evpnNvoOrch);
                gDirectory.set(m_evpnRemoteVnip2pOrch);

                map<string, string> profile = {
                    { "SAI_VS_SWITCH_TYPE", "SAI_VS_SWITCH_TYPE_BCM56850" },
                    { "KV_DEVICE_MAC_ADDRESS", "20:03:04:05:06:00" }
                };

                ut_helper::initSaiApi(profile);
                _hook_sai_apis();

                sai_attribute_t attr;

                attr.id = SAI_SWITCH_ATTR_INIT_SWITCH;
                attr.value.booldata = true;

                auto status = sai_switch_api->create_switch(&gSwitchId, 1, &attr);
                ASSERT_EQ(status, SAI_STATUS_SUCCESS);
            }

            void initSwitchOrch()
            {
                TableConnector stateDbSwitchTable(m_state_db.get(), "SWITCH_CAPABILITY");
                TableConnector conf_asic_sensors(m_config_db.get(), CFG_ASIC_SENSORS_TABLE_NAME);
                TableConnector app_switch_table(m_app_db.get(),  APP_SWITCH_TABLE_NAME);
                TableConnector conf_suppress_asic_sdk_health_categories(m_config_db.get(), CFG_SUPPRESS_ASIC_SDK_HEALTH_EVENT_NAME);

                vector<TableConnector> switch_tables = {
                    conf_asic_sensors,
                    conf_suppress_asic_sdk_health_categories,
                    app_switch_table
                };

                ASSERT_EQ(gSwitchOrch, nullptr);
                gSwitchOrch = new SwitchOrch(m_app_db.get(), switch_tables, stateDbSwitchTable);

                // Create dependencies ...
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
}

            void initVxlanOrch()
            {
                m_vxlan_tunnel_orch = new VxlanTunnelOrch(m_state_db.get(), m_app_db.get(), APP_VXLAN_TUNNEL_TABLE_NAME);
                gDirectory.set(m_vxlan_tunnel_orch);
            }

            void TearDown() override
            {
                _unhook_sai_apis();

                delete gPortsOrch;
                gPortsOrch = nullptr;
                if (m_vxlan_tunnel_orch) {
                    m_vxlan_tunnel_orch->vxlan_tunnel_table_.clear();
                    delete m_vxlan_tunnel_orch;
                    m_vxlan_tunnel_orch = nullptr;
                }

                if (m_evpnRemoteVnip2pOrch) {
                    delete m_evpnRemoteVnip2pOrch;
                    m_evpnRemoteVnip2pOrch = nullptr;
                }
                if (m_evpnNvoOrch) {
                    delete m_evpnNvoOrch;
                    m_evpnNvoOrch = nullptr;
                }
                if (m_vxlanTunnelMapOrch) {
                    delete m_vxlanTunnelMapOrch;
                    m_vxlanTunnelMapOrch = nullptr;
                }

                gDirectory.m_values.clear();
                delete gSwitchOrch;
                gSwitchOrch = nullptr;

                auto status = sai_switch_api->remove_switch(gSwitchId);
                ASSERT_EQ(status, SAI_STATUS_SUCCESS);
                gSwitchId = 0;

                ut_helper::uninitSaiApi();
            }

            // Helper function to create a basic VXLAN tunnel
            void CreateBasicVxlanTunnel(const string& tunnel_name, const string& src_ip, const string& dst_ip = "")
            {
                // Create the tunnel entry directly in the orchestrator's internal structures
                // This simulates what would happen after database processing
                IpAddress srcIpAddr(src_ip);
                IpAddress dstIpAddr = dst_ip.empty() ? IpAddress() : IpAddress(dst_ip);

                // Create tunnel object directly
                auto tunnel = std::make_unique<VxlanTunnel>(tunnel_name, srcIpAddr, dstIpAddr, TNL_CREATION_SRC_CLI);
                m_vxlan_tunnel_orch->vxlan_tunnel_table_[tunnel_name] = std::move(tunnel);
            }

            // Helper function to create a VXLAN tunnel map
            void CreateVxlanTunnelMap(const string& tunnel_name, const string& map_name,
                                      const string& vni, const string& vlan)
            {
                // Create the tunnel map entry directly in the orchestrator's internal structures
                string full_key = tunnel_name + ":" + map_name;

                tunnel_map_entry_t map_entry;
                map_entry.vni_id = std::stoi(vni);
                map_entry.vlan_id = std::stoi(vlan.substr(4)); // Remove "Vlan" prefix
                map_entry.map_entry_id = SAI_NULL_OBJECT_ID; // For testing, we don't need actual SAI objects

                m_vxlanTunnelMapOrch->vxlan_tunnel_map_table_[full_key] = map_entry;
            }

            // Helper function to create EVPN NVO
            void CreateEvpnNvo(const string& nvo_name, const string& source_vtep)
            {
                // Set the source VTEP pointer directly
                if (m_vxlan_tunnel_orch->isTunnelExists(source_vtep)) {
                    m_evpnNvoOrch->source_vtep_ptr = m_vxlan_tunnel_orch->getVxlanTunnel(source_vtep);
                }
            }
    };


    TEST_F(VxlanOrchTest, TunnelCreateFailure)
    {
        initSwitchOrch();
        initVxlanOrch();
        VxlanTunnelOrch* vxlan_orch = gDirectory.get<VxlanTunnelOrch*>();
        VxlanTunnel* tunnel = nullptr;

        auto src_ip = IpAddress("10.1.0.1");
        auto dst_ip = IpAddress("20.1.0.1");
        tunnel = new VxlanTunnel("vxlan_tunnel_1", src_ip, dst_ip, TNL_CREATION_SRC_CLI);
        vxlan_orch->addTunnel("vxlan_tunnel_1", tunnel);

        EXPECT_CALL(mock_sai_tunnel_, create_tunnel_map(_, _, _, _))
            .Times(4)
            .WillRepeatedly(DoAll(
                        SetArgPointee<0>(vxlan_tunnel_map_oid),
                        Return(SAI_STATUS_SUCCESS)
                        ));
        EXPECT_CALL(mock_sai_tunnel_, create_tunnel(_, _, _, _))
            .WillOnce(DoAll(
                        SetArgPointee<0>(SAI_NULL_OBJECT_ID),
                        Return(SAI_STATUS_FAILURE)
                        ));
        EXPECT_CALL(mock_sai_tunnel_, remove_tunnel_map(_))
            .Times(4)
            .WillRepeatedly(DoAll(
                        Return(SAI_STATUS_SUCCESS)
                        ));
        EXPECT_NO_THROW({
                bool result = vxlan_orch->createVxlanTunnelMap("vxlan_tunnel_1", TUNNEL_MAP_T_VIRTUAL_ROUTER, 1000, 0x1001, 0x1002, 64);
                EXPECT_FALSE(result);
                });
        vxlan_orch->delTunnel("vxlan_tunnel_1");
    }

    TEST_F(VxlanOrchTest, TunnelMapCreateFailure)
    {
        initSwitchOrch();
        initVxlanOrch();
        VxlanTunnelOrch* vxlan_orch = gDirectory.get<VxlanTunnelOrch*>();
        VxlanTunnel* tunnel = nullptr;

        auto src_ip = IpAddress("10.1.0.1");
        auto dst_ip = IpAddress("20.1.0.1");
        tunnel = new VxlanTunnel("vxlan_tunnel_1", src_ip, dst_ip, TNL_CREATION_SRC_CLI);
        vxlan_orch->addTunnel("vxlan_tunnel_1", tunnel);

        EXPECT_CALL(mock_sai_tunnel_, create_tunnel_map(_, _, _, _))
            .Times(4)
            .WillRepeatedly(DoAll(
                        SetArgPointee<0>(SAI_NULL_OBJECT_ID),
                        Return(SAI_STATUS_FAILURE)
                        ));
        EXPECT_CALL(mock_sai_tunnel_, create_tunnel(_, _, _, _))
            .WillOnce(DoAll(
                        SetArgPointee<0>(SAI_NULL_OBJECT_ID),
                        Return(SAI_STATUS_FAILURE)
                        ));
        EXPECT_CALL(mock_sai_tunnel_, remove_tunnel_map(_))
            .Times(4)
            .WillRepeatedly(DoAll(
                        Return(SAI_STATUS_FAILURE)
                        ));

        EXPECT_NO_THROW({
                bool result = vxlan_orch->createVxlanTunnelMap("vxlan_tunnel_1", TUNNEL_MAP_T_VIRTUAL_ROUTER, 1000, 0x1001, 0x1002, 64);
                EXPECT_FALSE(result);
                });
        vxlan_orch->delTunnel("vxlan_tunnel_1");
    }

    TEST_F(VxlanOrchTest, TunnelTerminationCreateFailure)
    {
        initSwitchOrch();
        initVxlanOrch();
        VxlanTunnelOrch* vxlan_orch = gDirectory.get<VxlanTunnelOrch*>();
        VxlanTunnel* tunnel = nullptr;

        auto src_ip = IpAddress("10.1.0.1");
        auto dst_ip = IpAddress("20.1.0.1");
        tunnel = new VxlanTunnel("vxlan_tunnel_1", src_ip, dst_ip, TNL_CREATION_SRC_CLI);
        vxlan_orch->addTunnel("vxlan_tunnel_1", tunnel);

        EXPECT_CALL(mock_sai_tunnel_, create_tunnel_map(_, _, _, _))
            .Times(4)
            .WillRepeatedly(DoAll(
                        SetArgPointee<0>(vxlan_tunnel_map_oid),
                        Return(SAI_STATUS_SUCCESS)
                        ));
        EXPECT_CALL(mock_sai_tunnel_, create_tunnel(_, _, _, _))
            .WillOnce(DoAll(
                        SetArgPointee<0>(vxlan_tunnel_oid),
                        Return(SAI_STATUS_SUCCESS)
                        ));
        EXPECT_CALL(mock_sai_tunnel_, create_tunnel_term_table_entry(_, _, _, _))
            .WillOnce(DoAll(
                        SetArgPointee<0>(SAI_NULL_OBJECT_ID),
                        Return(SAI_STATUS_FAILURE)
                        ));
        EXPECT_CALL(mock_sai_tunnel_, remove_tunnel(_))
            .WillOnce(DoAll(
                        Return(SAI_STATUS_FAILURE)
                        ));
        EXPECT_CALL(mock_sai_tunnel_, remove_tunnel_map(_))
            .Times(4)
            .WillRepeatedly(DoAll(
                        Return(SAI_STATUS_SUCCESS)
                        ));

        EXPECT_NO_THROW({
                bool result = vxlan_orch->createVxlanTunnelMap("vxlan_tunnel_1", TUNNEL_MAP_T_VIRTUAL_ROUTER, 1000, 0x1001, 0x1002, 64);
                EXPECT_FALSE(result);
                });
        vxlan_orch->delTunnel("vxlan_tunnel_1");
    }

    TEST_F(VxlanOrchTest, TunnelMapEntryCreateFailure)
    {
        initSwitchOrch();
        initVxlanOrch();
        VxlanTunnelOrch* vxlan_orch = gDirectory.get<VxlanTunnelOrch*>();
        VxlanTunnel* tunnel = nullptr;

        auto src_ip = IpAddress("10.1.0.1");
        auto dst_ip = IpAddress("20.1.0.1");
        tunnel = new VxlanTunnel("vxlan_tunnel_1", src_ip, dst_ip, TNL_CREATION_SRC_CLI);
        vxlan_orch->addTunnel("vxlan_tunnel_1", tunnel);

        EXPECT_CALL(mock_sai_tunnel_, create_tunnel_map(_, _, _, _))
            .Times(4)
            .WillRepeatedly(DoAll(
                        SetArgPointee<0>(vxlan_tunnel_map_oid),
                        Return(SAI_STATUS_SUCCESS)
                        ));
        EXPECT_CALL(mock_sai_tunnel_, create_tunnel(_, _, _, _))
            .WillOnce(DoAll(
                        SetArgPointee<0>(vxlan_tunnel_oid),
                        Return(SAI_STATUS_SUCCESS)
                        ));
        EXPECT_CALL(mock_sai_tunnel_, create_tunnel_term_table_entry(_, _, _, _))
            .WillOnce(DoAll(
                        SetArgPointee<0>(vxlan_tunnel_term_table_entry_oid),
                        Return(SAI_STATUS_SUCCESS)
                        ));
        EXPECT_CALL(mock_sai_tunnel_, create_tunnel_map_entry(_, _, _, _))
            .Times(2)
            .WillRepeatedly(DoAll(
                        SetArgPointee<0>(SAI_NULL_OBJECT_ID),
                        Return(SAI_STATUS_FAILURE)
                        ));

        EXPECT_NO_THROW({
                bool result = vxlan_orch->createVxlanTunnelMap("vxlan_tunnel_1", TUNNEL_MAP_T_VIRTUAL_ROUTER, 1000, 0x1001, 0x1002, 64);
                /*
                 * Return value of create_tunnel_map_entry() is intentionally
                 * ignored in createVxlanTunnelMap(), so expect true here
                 */
                EXPECT_TRUE(result);
                });
        vxlan_orch->delTunnel("vxlan_tunnel_1");
    }

    TEST_F(VxlanOrchTest, TunnelAllSuccess)
    {
        initSwitchOrch();
        initVxlanOrch();
        VxlanTunnelOrch* vxlan_orch = gDirectory.get<VxlanTunnelOrch*>();
        VxlanTunnel* tunnel = nullptr;

        auto src_ip = IpAddress("10.1.0.1");
        auto dst_ip = IpAddress("20.1.0.1");
        tunnel = new VxlanTunnel("vxlan_tunnel_1", src_ip, dst_ip, TNL_CREATION_SRC_CLI);
        vxlan_orch->addTunnel("vxlan_tunnel_1", tunnel);

        EXPECT_CALL(mock_sai_tunnel_, create_tunnel_map(_, _, _, _))
            .Times(4)
            .WillRepeatedly(DoAll(
                        SetArgPointee<0>(vxlan_tunnel_map_oid),
                        Return(SAI_STATUS_SUCCESS)
                        ));
        EXPECT_CALL(mock_sai_tunnel_, create_tunnel(_, _, _, _))
            .WillOnce(DoAll(
                        SetArgPointee<0>(vxlan_tunnel_oid),
                        Return(SAI_STATUS_SUCCESS)
                        ));
        EXPECT_CALL(mock_sai_tunnel_, create_tunnel_term_table_entry(_, _, _, _))
            .WillOnce(DoAll(
                        SetArgPointee<0>(vxlan_tunnel_term_table_entry_oid),
                        Return(SAI_STATUS_SUCCESS)
                        ));
        EXPECT_CALL(mock_sai_tunnel_, create_tunnel_map_entry(_, _, _, _))
            .Times(2)
            .WillRepeatedly(DoAll(
                        SetArgPointee<0>(vxlan_tunnel_map_entry_oid),
                        Return(SAI_STATUS_SUCCESS)
                        ));
        EXPECT_CALL(mock_sai_tunnel_, remove_tunnel_map_entry(_))
            .Times(2)
            .WillRepeatedly(DoAll(
                        Return(SAI_STATUS_SUCCESS)
                        ));
        EXPECT_CALL(mock_sai_tunnel_, remove_tunnel_term_table_entry(_))
            .WillOnce(DoAll(
                        Return(SAI_STATUS_SUCCESS)
                        ));
        EXPECT_CALL(mock_sai_tunnel_, remove_tunnel(_))
            .WillOnce(DoAll(
                        Return(SAI_STATUS_SUCCESS)
                        ));
        EXPECT_CALL(mock_sai_tunnel_, remove_tunnel_map(_))
            .Times(4)
            .WillRepeatedly(DoAll(
                        Return(SAI_STATUS_SUCCESS)
                        ));

        EXPECT_NO_THROW({
                bool result1 = vxlan_orch->createVxlanTunnelMap("vxlan_tunnel_1", TUNNEL_MAP_T_VIRTUAL_ROUTER, 1000, 0x1001, 0x1002, 64);
                EXPECT_TRUE(result1);
                bool result2 = vxlan_orch->removeVxlanTunnelMap("vxlan_tunnel_1", 1000);
                EXPECT_TRUE(result2);
                });
        vxlan_orch->delTunnel("vxlan_tunnel_1");
    }

    TEST_F(VxlanOrchTest, TunnelAllRemoveFailure)
    {
        initSwitchOrch();
        initVxlanOrch();
        VxlanTunnelOrch* vxlan_orch = gDirectory.get<VxlanTunnelOrch*>();
        VxlanTunnel* tunnel = nullptr;

        auto src_ip = IpAddress("10.1.0.1");
        auto dst_ip = IpAddress("20.1.0.1");
        tunnel = new VxlanTunnel("vxlan_tunnel_1", src_ip, dst_ip, TNL_CREATION_SRC_CLI);
        vxlan_orch->addTunnel("vxlan_tunnel_1", tunnel);

        EXPECT_CALL(mock_sai_tunnel_, create_tunnel_map(_, _, _, _))
            .Times(4)
            .WillRepeatedly(DoAll(
                        SetArgPointee<0>(vxlan_tunnel_map_oid),
                        Return(SAI_STATUS_SUCCESS)
                        ));
        EXPECT_CALL(mock_sai_tunnel_, create_tunnel(_, _, _, _))
            .WillOnce(DoAll(
                        SetArgPointee<0>(vxlan_tunnel_oid),
                        Return(SAI_STATUS_SUCCESS)
                        ));
        EXPECT_CALL(mock_sai_tunnel_, create_tunnel_term_table_entry(_, _, _, _))
            .WillOnce(DoAll(
                        SetArgPointee<0>(vxlan_tunnel_term_table_entry_oid),
                        Return(SAI_STATUS_SUCCESS)
                        ));
        EXPECT_CALL(mock_sai_tunnel_, create_tunnel_map_entry(_, _, _, _))
            .Times(2)
            .WillRepeatedly(DoAll(
                        SetArgPointee<0>(vxlan_tunnel_map_entry_oid),
                        Return(SAI_STATUS_SUCCESS)
                        ));
        EXPECT_CALL(mock_sai_tunnel_, remove_tunnel_map_entry(_))
            .Times(2)
            .WillRepeatedly(DoAll(
                        Return(SAI_STATUS_FAILURE)
                        ));
        EXPECT_CALL(mock_sai_tunnel_, remove_tunnel_term_table_entry(_))
            .WillOnce(DoAll(
                        Return(SAI_STATUS_FAILURE)
                        ));
        EXPECT_CALL(mock_sai_tunnel_, remove_tunnel(_))
            .WillOnce(DoAll(
                        Return(SAI_STATUS_FAILURE)
                        ));
        EXPECT_CALL(mock_sai_tunnel_, remove_tunnel_map(_))
            .Times(4)
            .WillRepeatedly(DoAll(
                        Return(SAI_STATUS_FAILURE)
                        ));

        EXPECT_NO_THROW({
                bool result1 = vxlan_orch->createVxlanTunnelMap("vxlan_tunnel_1", TUNNEL_MAP_T_VIRTUAL_ROUTER, 1000, 0x1001, 0x1002, 64);
                EXPECT_TRUE(result1);
                bool result2 = vxlan_orch->removeVxlanTunnelMap("vxlan_tunnel_1", 1000);
                EXPECT_TRUE(result2);
                });
        vxlan_orch->delTunnel("vxlan_tunnel_1");
    }

    // Test basic VXLAN tunnel creation
    TEST_F(VxlanOrchTest, BasicTunnelCreation)
    {
        initSwitchOrch();
        initVxlanOrch();

        string tunnel_name = "tunnel1";
        string src_ip = "10.1.1.1";

        CreateBasicVxlanTunnel(tunnel_name, src_ip);

        // Verify tunnel exists
        EXPECT_TRUE(m_vxlan_tunnel_orch->isTunnelExists(tunnel_name));

        // Get tunnel object and verify properties
        VxlanTunnel* tunnel = m_vxlan_tunnel_orch->getVxlanTunnel(tunnel_name);
        ASSERT_NE(tunnel, nullptr);
        EXPECT_EQ(tunnel->getSrcIP().to_string(), src_ip);
        EXPECT_EQ(tunnel->getTunnelName(), tunnel_name);
    }

    // Test VXLAN tunnel deletion
    TEST_F(VxlanOrchTest, TunnelDeletion)
    {
        initSwitchOrch();
        initVxlanOrch();

        string tunnel_name = "tunnel1";
        string src_ip = "10.1.1.1";

        // Create tunnel
        CreateBasicVxlanTunnel(tunnel_name, src_ip);
        EXPECT_TRUE(m_vxlan_tunnel_orch->isTunnelExists(tunnel_name));

        // Delete tunnel directly from internal structures
        m_vxlan_tunnel_orch->vxlan_tunnel_table_.erase(tunnel_name);

        // Verify tunnel is deleted
        EXPECT_FALSE(m_vxlan_tunnel_orch->isTunnelExists(tunnel_name));
    }

    // Test VXLAN tunnel map creation
    TEST_F(VxlanOrchTest, TunnelMapCreation)
    {
        initSwitchOrch();
        initVxlanOrch();

        string tunnel_name = "tunnel1";
        string src_ip = "10.1.1.1";
        string map_name = "map1";
        string vni = "1000";
        string vlan = "Vlan100";

        // First create the tunnel
        CreateBasicVxlanTunnel(tunnel_name, src_ip);
        EXPECT_TRUE(m_vxlan_tunnel_orch->isTunnelExists(tunnel_name));

        // Create tunnel map
        CreateVxlanTunnelMap(tunnel_name, map_name, vni, vlan);

        // Verify tunnel map exists
        string full_key = tunnel_name + ":" + map_name;
        EXPECT_TRUE(m_vxlanTunnelMapOrch->isTunnelMapExists(full_key));
    }

    // Test next hop tunnel creation
    TEST_F(VxlanOrchTest, NextHopTunnelCreation)
    {
        initSwitchOrch();
        initVxlanOrch();

        string tunnel_name = "tunnel1";
        string src_ip = "10.1.1.1";
        string dst_ip = "10.1.1.2";

        CreateBasicVxlanTunnel(tunnel_name, src_ip);
        EXPECT_TRUE(m_vxlan_tunnel_orch->isTunnelExists(tunnel_name));

        VxlanTunnel* tunnel = m_vxlan_tunnel_orch->getVxlanTunnel(tunnel_name);
        ASSERT_NE(tunnel, nullptr);

        // Test tunnel properties
        EXPECT_EQ(tunnel->getSrcIP().to_string(), src_ip);
        EXPECT_EQ(tunnel->getTunnelName(), tunnel_name);

        // Test next hop tunnel methods with proper error handling
        IpAddress nh_ip(dst_ip);
        MacAddress nh_mac("00:11:22:33:44:55");
        uint32_t vni = 1000;

        // In mock environment, createNextHopTunnel will fail because SAI calls fail
        // We test that it handles the failure gracefully by catching the exception
        EXPECT_EQ(m_vxlan_tunnel_orch->createNextHopTunnel(tunnel_name, nh_ip, nh_mac, vni), SAI_NULL_OBJECT_ID);

        // Test that removeNextHopTunnel returns false for non-existent next hop
        bool result = m_vxlan_tunnel_orch->removeNextHopTunnel(tunnel_name, nh_ip, nh_mac, vni);
        EXPECT_FALSE(result);
    }

    // Test dynamic DIP tunnel creation and cleanup
    TEST_F(VxlanOrchTest, DynamicDipTunnelCleanup)
    {
        initSwitchOrch();
        initVxlanOrch();

        string tunnel_name = "tunnel1";
        string src_ip = "10.1.1.1";
        string remote_vtep = "10.1.1.2";

        // Create base tunnel
        CreateBasicVxlanTunnel(tunnel_name, src_ip);
        EXPECT_TRUE(m_vxlan_tunnel_orch->isTunnelExists(tunnel_name));

        VxlanTunnel* tunnel = m_vxlan_tunnel_orch->getVxlanTunnel(tunnel_name);
        ASSERT_NE(tunnel, nullptr);

        // Initially, remote VTEP should not exist in the reference count map
        int initial_ref_count = tunnel->getRemoteEndPointRefCnt(remote_vtep);
        EXPECT_EQ(initial_ref_count, -1); // -1 means not found

        // Manually setup tunnel user tracking for testing
        // This simulates what happens when createDynamicDIPTunnel is called successfully
        tunnel_refcnt_t ref_counts;
        memset(&ref_counts, 0, sizeof(tunnel_refcnt_t));
        ref_counts.imr_refcnt = 1; // Simulate one IMR reference

        // Access the private member to set up the test scenario
        tunnel->tnl_users_[remote_vtep] = ref_counts;

        // Now verify tunnel user count
        int ref_count = tunnel->getRemoteEndPointRefCnt(remote_vtep);
        EXPECT_EQ(ref_count, 1); // Should be 1 (imr_refcnt)

        // Test cleanup when reference count is not zero (should not cleanup)
        // The cleanupDynamicDIPTunnel method only does cleanup when ref count is 0
        tunnel->cleanupDynamicDIPTunnel(remote_vtep);
        int ref_count_after = tunnel->getRemoteEndPointRefCnt(remote_vtep);
        EXPECT_EQ(ref_count_after, ref_count); // Should remain the same

        // Simulate reference count going to zero
        ref_counts.imr_refcnt = 0;
        ref_counts.ip_refcnt = 0;
        tunnel->tnl_users_[remote_vtep] = ref_counts;

        // Verify reference count is zero
        ref_count = tunnel->getRemoteEndPointRefCnt(remote_vtep);
        EXPECT_EQ(ref_count, 0);

        // Note: We don't call cleanupDynamicDIPTunnel with ref_count 0 in mock tests
        // because it requires full SAI/PortsOrch infrastructure for tunnel port cleanup
    }

    // Test tunnel user management
    TEST_F(VxlanOrchTest, TunnelUserManagement)
    {
        initSwitchOrch();
        initVxlanOrch();

        string tunnel_name = "tunnel1";
        string src_ip = "10.1.1.1";
        string remote_vtep = "10.1.1.2";
        uint32_t vni = 1000;
        uint32_t vlan = 100;

        // Create EVPN NVO first
        CreateEvpnNvo("nvo1", tunnel_name);

        // Create base tunnel
        CreateBasicVxlanTunnel(tunnel_name, src_ip);
        EXPECT_TRUE(m_vxlan_tunnel_orch->isTunnelExists(tunnel_name));

        // Set up EVPN VTEP pointer
        VxlanTunnel* tunnel = m_vxlan_tunnel_orch->getVxlanTunnel(tunnel_name);
        ASSERT_NE(tunnel, nullptr);
        m_evpnNvoOrch->source_vtep_ptr = tunnel;

        // Set up tunnel user tracking manually for testing
        // This simulates what would happen when addTunnelUser is called successfully
        tunnel_refcnt_t ref_counts;
        memset(&ref_counts, 0, sizeof(tunnel_refcnt_t));
        ref_counts.imr_refcnt = 1; // Simulate one IMR reference
        ref_counts.ip_refcnt = 1;  // Simulate one IP reference

        // Access the private member to set up the test scenario
        tunnel->tnl_users_[remote_vtep] = ref_counts;

        // Test reference counting functions
        int imr_count = tunnel->getRemoteEndPointIMRRefCnt(remote_vtep);
        int ip_count = tunnel->getRemoteEndPointIPRefCnt(remote_vtep);
        int total_count = tunnel->getRemoteEndPointRefCnt(remote_vtep);

        EXPECT_EQ(imr_count, 1);
        EXPECT_EQ(ip_count, 1);
        EXPECT_EQ(total_count, imr_count + ip_count);

        // Test addTunnelUser interface (may fail due to missing VLAN setup, but we're testing the interface)
        bool added = m_vxlan_tunnel_orch->addTunnelUser(remote_vtep, vni, vlan, TUNNEL_USER_IMR);
        (void)added; // Suppress unused variable warning - result may vary in mock environment
    }

    // Test VXLAN tunnel port operations
    TEST_F(VxlanOrchTest, TunnelPortOperations)
    {
        initSwitchOrch();
        initVxlanOrch();

        string vtep_ip = "10.1.1.2";

        // Test getTunnelPortName
        string local_port_name = m_vxlan_tunnel_orch->getTunnelPortName(vtep_ip, true);
        string remote_port_name = m_vxlan_tunnel_orch->getTunnelPortName(vtep_ip, false);

        EXPECT_FALSE(local_port_name.empty());
        EXPECT_FALSE(remote_port_name.empty());
        EXPECT_NE(local_port_name, remote_port_name);

        // Test tunnel name extraction
        string tunnel_name;
        m_vxlan_tunnel_orch->getTunnelNameFromDIP(vtep_ip, tunnel_name);
        EXPECT_FALSE(tunnel_name.empty());

        // Test port name to tunnel name conversion
        string extracted_tunnel_name;
        m_vxlan_tunnel_orch->getTunnelNameFromPort(remote_port_name, extracted_tunnel_name);
        EXPECT_FALSE(extracted_tunnel_name.empty());
    }

    // Test VXLAN VNI to VLAN mapping
    TEST_F(VxlanOrchTest, VniVlanMapping)
    {
        initSwitchOrch();
        initVxlanOrch();

        uint32_t vni = 1000;
        uint16_t vlan_id = 100;

        // Add VNI to VLAN mapping
        m_vxlan_tunnel_orch->addVlanMappedToVni(vni, vlan_id);

        // Verify mapping
        uint16_t retrieved_vlan = m_vxlan_tunnel_orch->getVlanMappedToVni(vni);
        EXPECT_EQ(retrieved_vlan, vlan_id);

        // Test non-existent VNI
        uint16_t non_existent = m_vxlan_tunnel_orch->getVlanMappedToVni(9999);
        EXPECT_EQ(non_existent, 0);

        // Delete mapping
        m_vxlan_tunnel_orch->delVlanMappedToVni(vni);
        retrieved_vlan = m_vxlan_tunnel_orch->getVlanMappedToVni(vni);
        EXPECT_EQ(retrieved_vlan, 0);
    }

    // Test reference counting edge cases
    TEST_F(VxlanOrchTest, ReferenceCountingEdgeCases)
    {
        initSwitchOrch();
        initVxlanOrch();

        string tunnel_name = "tunnel1";
        string src_ip = "10.1.1.1";
        string remote_vtep = "10.1.1.2";

        CreateBasicVxlanTunnel(tunnel_name, src_ip);
        VxlanTunnel* tunnel = m_vxlan_tunnel_orch->getVxlanTunnel(tunnel_name);
        ASSERT_NE(tunnel, nullptr);

        // Test spurious IMR add/del tracking
        tunnel->increment_spurious_imr_add(remote_vtep);
        tunnel->increment_spurious_imr_del(remote_vtep);

        // Set up tunnel user tracking manually for testing IP reference operations
        tunnel_refcnt_t ref_counts;
        memset(&ref_counts, 0, sizeof(tunnel_refcnt_t));
        ref_counts.ip_refcnt = 1;  // Start with 1 IP reference

        // Access the private member to set up the test scenario
        tunnel->tnl_users_[remote_vtep] = ref_counts;

        // Test IP reference tracking
        tunnel->updateRemoteEndPointIpRef(remote_vtep, true);  // increment
        int ip_count = tunnel->getRemoteEndPointIPRefCnt(remote_vtep);
        EXPECT_EQ(ip_count, 2); // Should be 2 (1 initial + 1 increment)

        tunnel->updateRemoteEndPointIpRef(remote_vtep, false); // decrement
        int ip_count_after = tunnel->getRemoteEndPointIPRefCnt(remote_vtep);
        EXPECT_EQ(ip_count_after, ip_count - 1); // Should be 1 (2 - 1)

        // Test DIP tunnel count
        int dip_count = tunnel->getDipTunnelCnt();
        EXPECT_GE(dip_count, 0);
    }

    // Test error conditions and edge cases
    TEST_F(VxlanOrchTest, ErrorConditions)
    {
        initSwitchOrch();
        initVxlanOrch();

        // Test operations on non-existent tunnel
        EXPECT_FALSE(m_vxlan_tunnel_orch->isTunnelExists("non_existent"));

        // Test tunnel port retrieval for non-existent VTEP
        Port dummy_port;
        bool found = m_vxlan_tunnel_orch->getTunnelPort("192.168.1.1", dummy_port);
        EXPECT_FALSE(found);

        // Test createVxlanTunnelMap with invalid parameters
        bool result = m_vxlan_tunnel_orch->createVxlanTunnelMap("non_existent",
                                                              TUNNEL_MAP_T_VLAN,
                                                              1000,
                                                              SAI_NULL_OBJECT_ID,
                                                              SAI_NULL_OBJECT_ID);
        EXPECT_FALSE(result);

        // Test removeVxlanTunnelMap on non-existent tunnel
        result = m_vxlan_tunnel_orch->removeVxlanTunnelMap("non_existent", 1000);
        EXPECT_FALSE(result);
    }

} // namespace vxlanorch_test
