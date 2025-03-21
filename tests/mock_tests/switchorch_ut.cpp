#define private public // make Directory::m_values available to clean it.
#include "orchagent/directory.h"
#undef private
#define protected public
#include "orchagent/orch.h"
#undef protected
#include "tests/mock_tests/ut_helper.h"
#include "tests/mock_tests/mock_orchagent_main.h"
#include "tests/mock_tests/mock_table.h"
#include "orchagent/p4orch/tests/mock_response_publisher.h"

extern void on_switch_asic_sdk_health_event(sai_object_id_t switch_id,
                                            sai_switch_asic_sdk_health_severity_t severity,
                                            sai_timespec_t timestamp,
                                            sai_switch_asic_sdk_health_category_t category,
                                            sai_switch_health_data_t data,
                                            const sai_u8_list_t description);

namespace switchorch_test
{
    using namespace std;

    sai_switch_api_t ut_sai_switch_api;
    sai_switch_api_t *pold_sai_switch_api;

    shared_ptr<swss::DBConnector> m_app_db;
    shared_ptr<swss::DBConnector> m_config_db;
    shared_ptr<swss::DBConnector> m_state_db;

    sai_switch_attr_t _ut_stub_asic_sdk_health_event_attribute_to_check;
    bool _ut_stub_asic_sdk_health_event_check_all;
    uint32_t _ut_stub_asic_sdk_health_event_call_count;
    map<sai_switch_attr_t, set<sai_switch_asic_sdk_health_category_t>> _ut_stub_asic_sdk_health_event_category_sets;
    set<sai_switch_asic_sdk_health_category_t> _ut_stub_asic_sdk_health_event_passed_categories;

    bool _ut_reg_event_unsupported;

    sai_status_t _ut_stub_sai_set_switch_attribute(
        _In_ sai_object_id_t switch_id,
        _In_ const sai_attribute_t *attr)
    {
        switch (attr[0].id)
        {
        case SAI_SWITCH_ATTR_SWITCH_ASIC_SDK_HEALTH_EVENT_NOTIFY:
            if (_ut_reg_event_unsupported)
            {
                return SAI_STATUS_NOT_IMPLEMENTED;
            }
            break;
        case SAI_SWITCH_ATTR_REG_FATAL_SWITCH_ASIC_SDK_HEALTH_CATEGORY:
        case SAI_SWITCH_ATTR_REG_WARNING_SWITCH_ASIC_SDK_HEALTH_CATEGORY:
        case SAI_SWITCH_ATTR_REG_NOTICE_SWITCH_ASIC_SDK_HEALTH_CATEGORY:
            if (_ut_stub_asic_sdk_health_event_check_all)
            {
                _ut_stub_asic_sdk_health_event_call_count++;
                auto *passed_category_list = reinterpret_cast<sai_switch_asic_sdk_health_category_t*>(attr[0].value.s32list.list);
                _ut_stub_asic_sdk_health_event_category_sets[(sai_switch_attr_t)attr[0].id] = set<sai_switch_asic_sdk_health_category_t>(passed_category_list, passed_category_list + attr[0].value.s32list.count);
            }
            return SAI_STATUS_SUCCESS;
        default:
            break;
        }
        return pold_sai_switch_api->set_switch_attribute(switch_id, attr);
    }

    void _hook_sai_apis()
    {
        ut_sai_switch_api = *sai_switch_api;
        pold_sai_switch_api = sai_switch_api;
        ut_sai_switch_api.set_switch_attribute = _ut_stub_sai_set_switch_attribute;
        sai_switch_api = &ut_sai_switch_api;
    }

    void _unhook_sai_apis()
    {
        sai_switch_api = pold_sai_switch_api;
    }

    struct SwitchOrchTest : public ::testing::Test
    {
        SwitchOrchTest()
        {
        }

        void SetUp() override
        {
            // Init switch and create dependencies
            m_app_db = make_shared<swss::DBConnector>("APPL_DB", 0);
            m_config_db = make_shared<swss::DBConnector>("CONFIG_DB", 0);
            m_state_db = make_shared<swss::DBConnector>("STATE_DB", 0);

            _ut_reg_event_unsupported = false;

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

        void checkAsicSdkHealthEvent(const sai_timespec_t &timestamp, const string &expected_key="")
        {
            initSwitchOrch();

            sai_switch_health_data_t data;
            memset(&data, 0, sizeof(data));
            data.data_type = SAI_HEALTH_DATA_TYPE_GENERAL;
            vector<uint8_t> data_from_sai({100, 101, 115, 99, 114, 105, 112, 116, 105, 245, 111, 110, 245, 10, 123, 125, 100, 100});
            sai_u8_list_t description;
            description.list = data_from_sai.data();
            description.count = (uint32_t)(data_from_sai.size() - 2);
            on_switch_asic_sdk_health_event(gSwitchId,
                                            SAI_SWITCH_ASIC_SDK_HEALTH_SEVERITY_FATAL,
                                            timestamp,
                                            SAI_SWITCH_ASIC_SDK_HEALTH_CATEGORY_FW,
                                            data,
                                            description);

            string key;
            if (expected_key.empty())
            {
                vector<string> keys;
                gSwitchOrch->m_asicSdkHealthEventTable->getKeys(keys);
                key = keys[0];
            }
            else
            {
                key = expected_key;
            }
            string value;
            gSwitchOrch->m_asicSdkHealthEventTable->hget(key, "category", value);
            ASSERT_EQ(value, "firmware");
            gSwitchOrch->m_asicSdkHealthEventTable->hget(key, "severity", value);
            ASSERT_EQ(value, "fatal");
            gSwitchOrch->m_asicSdkHealthEventTable->hget(key, "description", value);
            ASSERT_EQ(value, "description\n{}");
        }

        void TearDown() override
        {
            ::testing_db::reset();

            _ut_stub_asic_sdk_health_event_category_sets.clear();

            gDirectory.m_values.clear();

            delete gSwitchOrch;
            gSwitchOrch = nullptr;

            ut_helper::uninitSaiApi();
        }
    };

    TEST_F(SwitchOrchTest, SwitchOrchTestSuppressCategories)
    {
        Table suppressAsicSdkHealthEventTable = Table(m_config_db.get(), CFG_SUPPRESS_ASIC_SDK_HEALTH_EVENT_NAME);

        suppressAsicSdkHealthEventTable.set("fatal",
                                {
                                    {"max_events", "1000"}
                                });
        suppressAsicSdkHealthEventTable.set("warning",
                                {
                                    {"categories", "software,firmware,cpu_hw,asic_hw"}
                                });

        _ut_stub_asic_sdk_health_event_check_all = true;
        auto call_count = _ut_stub_asic_sdk_health_event_call_count;

        _hook_sai_apis();
        initSwitchOrch();

        vector<string> ts;
        std::deque<KeyOpFieldsValuesTuple> entries;
        set<sai_switch_asic_sdk_health_category_t> all_categories({
                SAI_SWITCH_ASIC_SDK_HEALTH_CATEGORY_SW,
                SAI_SWITCH_ASIC_SDK_HEALTH_CATEGORY_FW,
                SAI_SWITCH_ASIC_SDK_HEALTH_CATEGORY_CPU_HW,
                SAI_SWITCH_ASIC_SDK_HEALTH_CATEGORY_ASIC_HW});
        set<sai_switch_asic_sdk_health_category_t> empty_category;

        call_count += 2;
        ASSERT_EQ(_ut_stub_asic_sdk_health_event_call_count, call_count);
        ASSERT_EQ(_ut_stub_asic_sdk_health_event_category_sets[SAI_SWITCH_ATTR_REG_FATAL_SWITCH_ASIC_SDK_HEALTH_CATEGORY], all_categories);
        ASSERT_EQ(_ut_stub_asic_sdk_health_event_category_sets[SAI_SWITCH_ATTR_REG_WARNING_SWITCH_ASIC_SDK_HEALTH_CATEGORY], empty_category);
        ASSERT_EQ(_ut_stub_asic_sdk_health_event_category_sets[SAI_SWITCH_ATTR_REG_NOTICE_SWITCH_ASIC_SDK_HEALTH_CATEGORY], all_categories);

        // case: severity: fatal, operation: suppress all categories
        entries.push_back({"fatal", "SET",
                           {
                               {"categories", "software,firmware,cpu_hw,asic_hw"}
                           }});
        auto consumer = dynamic_cast<Consumer *>(gSwitchOrch->getExecutor(CFG_SUPPRESS_ASIC_SDK_HEALTH_EVENT_NAME));
        consumer->addToSync(entries);
        entries.clear();
        static_cast<Orch *>(gSwitchOrch)->doTask();
        ASSERT_EQ(_ut_stub_asic_sdk_health_event_category_sets[SAI_SWITCH_ATTR_REG_FATAL_SWITCH_ASIC_SDK_HEALTH_CATEGORY], empty_category);
        call_count++;
        ASSERT_EQ(_ut_stub_asic_sdk_health_event_call_count, call_count);

        // case: severity: warning, operation: suppress partial categories
        entries.push_back({"warning", "SET",
                           {
                               {"categories", "software,cpu_hw,invalid_category"}
                           }});
        consumer->addToSync(entries);
        entries.clear();
        static_cast<Orch *>(gSwitchOrch)->doTask();
        ASSERT_EQ(_ut_stub_asic_sdk_health_event_category_sets[SAI_SWITCH_ATTR_REG_WARNING_SWITCH_ASIC_SDK_HEALTH_CATEGORY],
                  set<sai_switch_asic_sdk_health_category_t>({
                          SAI_SWITCH_ASIC_SDK_HEALTH_CATEGORY_FW,
                          SAI_SWITCH_ASIC_SDK_HEALTH_CATEGORY_ASIC_HW}));
        call_count++;
        ASSERT_EQ(_ut_stub_asic_sdk_health_event_call_count, call_count);

        // case: invalid severity, nothing changed (to satisfy coverate)
        entries.push_back({"warninga", "SET",
                           {
                               {"categories", "software,cpu_hw,asic_hw"}
                           }});
        consumer->addToSync(entries);
        entries.clear();
        static_cast<Orch *>(gSwitchOrch)->doTask();
        // No SAI API called
        ASSERT_EQ(_ut_stub_asic_sdk_health_event_call_count, call_count);

        // case: severity: warning, operation: set max_events only, which means to remove suppress list
        entries.push_back({"warning", "SET",
                           {
                               {"max_events", "10"}
                           }});
        consumer->addToSync(entries);
        entries.clear();
        static_cast<Orch *>(gSwitchOrch)->doTask();
        call_count++;
        ASSERT_EQ(_ut_stub_asic_sdk_health_event_call_count, call_count);
        ASSERT_EQ(_ut_stub_asic_sdk_health_event_category_sets[SAI_SWITCH_ATTR_REG_WARNING_SWITCH_ASIC_SDK_HEALTH_CATEGORY], all_categories);

        // case: severity: notice, operation: suppress no category
        entries.push_back({"notice", "DEL", {}});
        consumer->addToSync(entries);
        entries.clear();
        static_cast<Orch *>(gSwitchOrch)->doTask();
        ASSERT_EQ(_ut_stub_asic_sdk_health_event_category_sets[SAI_SWITCH_ATTR_REG_NOTICE_SWITCH_ASIC_SDK_HEALTH_CATEGORY], all_categories);
        call_count++;
        ASSERT_EQ(_ut_stub_asic_sdk_health_event_call_count, call_count);

        _unhook_sai_apis();
    }

    TEST_F(SwitchOrchTest, SwitchOrchTestCheckCapability)
    {
        initSwitchOrch();

        string value;
        gSwitchOrch->m_switchTable.hget("switch", SWITCH_CAPABILITY_TABLE_ASIC_SDK_HEALTH_EVENT_CAPABLE, value);
        ASSERT_EQ(value, "true");
        gSwitchOrch->m_switchTable.hget("switch", SWITCH_CAPABILITY_TABLE_REG_FATAL_ASIC_SDK_HEALTH_CATEGORY, value);
        ASSERT_EQ(value, "true");
        gSwitchOrch->m_switchTable.hget("switch", SWITCH_CAPABILITY_TABLE_REG_WARNING_ASIC_SDK_HEALTH_CATEGORY, value);
        ASSERT_EQ(value, "true");
        gSwitchOrch->m_switchTable.hget("switch", SWITCH_CAPABILITY_TABLE_REG_NOTICE_ASIC_SDK_HEALTH_CATEGORY, value);
        ASSERT_EQ(value, "true");
    }

    TEST_F(SwitchOrchTest, SwitchOrchTestCheckCapabilityUnsupported)
    {
        _ut_reg_event_unsupported = true;
        _ut_stub_asic_sdk_health_event_check_all = true;

        _hook_sai_apis();
        initSwitchOrch();

        string value;
        gSwitchOrch->m_switchTable.hget("switch", SWITCH_CAPABILITY_TABLE_ASIC_SDK_HEALTH_EVENT_CAPABLE, value);
        ASSERT_EQ(value, "false");
        gSwitchOrch->m_switchTable.hget("switch", SWITCH_CAPABILITY_TABLE_REG_FATAL_ASIC_SDK_HEALTH_CATEGORY, value);
        ASSERT_EQ(value, "false");
        gSwitchOrch->m_switchTable.hget("switch", SWITCH_CAPABILITY_TABLE_REG_WARNING_ASIC_SDK_HEALTH_CATEGORY, value);
        ASSERT_EQ(value, "false");
        gSwitchOrch->m_switchTable.hget("switch", SWITCH_CAPABILITY_TABLE_REG_NOTICE_ASIC_SDK_HEALTH_CATEGORY, value);
        ASSERT_EQ(value, "false");

        // case: unsupported severity. To satisfy coverage.
        vector<string> ts;
        std::deque<KeyOpFieldsValuesTuple> entries;
        Table suppressAsicSdkHealthEventTable = Table(m_config_db.get(), CFG_SUPPRESS_ASIC_SDK_HEALTH_EVENT_NAME);
        entries.push_back({"fatal", "SET",
                           {
                               {"categories", "software,firmware,cpu_hw,asic_hw"}
                           }});
        set<sai_switch_asic_sdk_health_category_t> empty_category;
        auto consumer = dynamic_cast<Consumer *>(gSwitchOrch->getExecutor(CFG_SUPPRESS_ASIC_SDK_HEALTH_EVENT_NAME));
        consumer->addToSync(entries);
        entries.clear();
        auto call_count = _ut_stub_asic_sdk_health_event_call_count;
        static_cast<Orch *>(gSwitchOrch)->doTask();
        ASSERT_EQ(_ut_stub_asic_sdk_health_event_category_sets[SAI_SWITCH_ATTR_REG_FATAL_SWITCH_ASIC_SDK_HEALTH_CATEGORY], empty_category);
        ASSERT_EQ(_ut_stub_asic_sdk_health_event_call_count, call_count);
    }

    TEST_F(SwitchOrchTest, SwitchOrchTestHandleEvent)
    {
        sai_timespec_t timestamp = {.tv_sec = 1701160447, .tv_nsec = 538710245};
        checkAsicSdkHealthEvent(timestamp, "2023-11-28 08:34:07");
    }

    TEST_F(SwitchOrchTest, SwitchOrchTestHandleEventInvalidTimeStamp)
    {
        sai_timespec_t timestamp = {.tv_sec = 172479515853275099, .tv_nsec = 538710245};
        checkAsicSdkHealthEvent(timestamp);
    }
}
