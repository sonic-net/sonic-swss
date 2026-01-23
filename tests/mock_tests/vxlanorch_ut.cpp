#define private public
#include "directory.h"
#undef private
#define protected public
#include "orch.h"
#undef protected
#define private public
#include "vxlanorch.h"
#undef private
#include "ut_helper.h"
#include "mock_orchagent_main.h"
#include "mock_sai_api.h"
#include "mock_sai_tunnel.h"


namespace vxlanorch_test
{
    using namespace std;
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

    shared_ptr<swss::DBConnector> m_app_db;
    shared_ptr<swss::DBConnector> m_config_db;
    shared_ptr<swss::DBConnector> m_state_db;
    VxlanTunnelOrch *vxlan_tunnel_orch;

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


            void SetUp() override
            {
                // Init switch and create dependencies
                m_app_db = make_shared<swss::DBConnector>("APPL_DB", 0);
                m_config_db = make_shared<swss::DBConnector>("CONFIG_DB", 0);
                m_state_db = make_shared<swss::DBConnector>("STATE_DB", 0);

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
            }

            void initVxlanOrch()
            {
                vxlan_tunnel_orch = new VxlanTunnelOrch(m_state_db.get(), m_app_db.get(), APP_VXLAN_TUNNEL_TABLE_NAME);
                gDirectory.set(vxlan_tunnel_orch);
            }

            void TearDown() override
            {
                _unhook_sai_apis();
                gDirectory.m_values.erase(typeid(VxlanTunnelOrch*).name());
                // delete vxlan_tunnel_orch;
                vxlan_tunnel_orch = nullptr;
                delete gSwitchOrch;
                gSwitchOrch = nullptr;
                ut_helper::uninitSaiApi();
            }
    };


    TEST_F(VxlanOrchTest, TunnelCreationFailure)
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
                vxlan_orch->createVxlanTunnelMap("vxlan_tunnel_1", TUNNEL_MAP_T_VIRTUAL_ROUTER, 1000, 0x1001, 0x1002, 64);
                });
        vxlan_orch->delTunnel("vxlan_tunnel_1");
    }

    TEST_F(VxlanOrchTest, TunnelMapCreationFailure)
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
                vxlan_orch->createVxlanTunnelMap("vxlan_tunnel_1", TUNNEL_MAP_T_VIRTUAL_ROUTER, 1000, 0x1001, 0x1002, 64);
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
                        Return(SAI_STATUS_SUCCESS)
                        ));
        EXPECT_CALL(mock_sai_tunnel_, remove_tunnel_map(_))
            .Times(4)
            .WillRepeatedly(DoAll(
                        Return(SAI_STATUS_SUCCESS)
                        ));

        EXPECT_NO_THROW({
                vxlan_orch->createVxlanTunnelMap("vxlan_tunnel_1", TUNNEL_MAP_T_VIRTUAL_ROUTER, 1000, 0x1001, 0x1002, 64);
                });
        vxlan_orch->delTunnel("vxlan_tunnel_1");
    }
}
