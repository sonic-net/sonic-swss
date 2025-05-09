#define private public // make Directory::m_values available to clean it.
#include "directory.h"
#undef private
#define protected public
#include "orch.h"
#undef protected
#include "ut_helper.h"
#include "mock_orchagent_main.h"
#include "mock_table.h"
#include "mock_response_publisher.h"
#include "saihelper.h"

namespace saihelper_test
{
    using namespace std;

    sai_switch_api_t ut_sai_switch_api;
    sai_switch_api_t *old_sai_switch_api;

    shared_ptr<swss::DBConnector> m_app_db;
    shared_ptr<swss::DBConnector> m_config_db;
    shared_ptr<swss::DBConnector> m_state_db;

    bool set_comm_mode_not_supported;
    bool use_pipeline_not_supported;

    sai_status_t _ut_stub_sai_set_switch_attribute(
        _In_ sai_object_id_t switch_id,
        _In_ const sai_attribute_t *attr)
    {
        switch (attr[0].id)
        {
            case SAI_REDIS_SWITCH_ATTR_REDIS_COMMUNICATION_MODE:
                if (set_comm_mode_not_supported)
                {
                    return SAI_STATUS_NOT_SUPPORTED;
                }
                else
                {
                    return SAI_STATUS_SUCCESS;
                }
                break;
            case SAI_REDIS_SWITCH_ATTR_USE_PIPELINE:
                if (use_pipeline_not_supported)
                {
                    return SAI_STATUS_NOT_SUPPORTED;
                }
                else
                {
                    return SAI_STATUS_SUCCESS;
                }
                break;
            default:
                break;
        }
        return SAI_STATUS_SUCCESS;
    }

    void _hook_sai_apis()
    {
        ut_sai_switch_api = *sai_switch_api;
        old_sai_switch_api = sai_switch_api;
        ut_sai_switch_api.set_switch_attribute = _ut_stub_sai_set_switch_attribute;
        sai_switch_api = &ut_sai_switch_api;
    }

    void _unhook_sai_apis()
    {
        sai_switch_api = old_sai_switch_api;
    }

    class SaihelperTest : public ::testing::Test
    {
        public:

            SaihelperTest()
            {
            };

            ~SaihelperTest()
            {
            };

            void SetUp() override
            {
                // Init switch and create dependencies
                m_app_db = make_shared<swss::DBConnector>("APPL_DB", 0);
                m_config_db = make_shared<swss::DBConnector>("CONFIG_DB", 0);
                m_state_db = make_shared<swss::DBConnector>("STATE_DB", 0);

                set_comm_mode_not_supported = false;
                use_pipeline_not_supported = false;

                map<string, string> profile = {
                    { "SAI_VS_SWITCH_TYPE", "SAI_VS_SWITCH_TYPE_BCM56850" },
                    { "KV_DEVICE_MAC_ADDRESS", "20:03:04:05:06:00" }
                };

                ut_helper::initSaiApi(profile);

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

            void TearDown() override
            {
                ::testing_db::reset();

                gDirectory.m_values.clear();

                delete gSwitchOrch;
                gSwitchOrch = nullptr;

                ut_helper::uninitSaiApi();
            }
    };

    TEST_F(SaihelperTest, TestSetCommunicationModeFailure) {
        set_comm_mode_not_supported = true;
        _hook_sai_apis();
        initSwitchOrch();

        // Assert that the program terminates after initSaiRedis() call
        ASSERT_DEATH({initSaiRedis();}, "");
        set_comm_mode_not_supported = false;
        _unhook_sai_apis();
    }

    TEST_F(SaihelperTest, TestSetRedisPipelineFailure) {
        use_pipeline_not_supported = true;
        _hook_sai_apis();
        initSwitchOrch();

        // Assert that the program terminates after initSaiRedis() call
        ASSERT_DEATH({initSaiRedis();}, "");
        use_pipeline_not_supported = false;
        _unhook_sai_apis();
    }
}

