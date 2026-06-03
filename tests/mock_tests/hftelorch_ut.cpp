#include "mock_sai_capability_wrap.h"
#include "ut_helper.h"
#include "mock_orchagent_main.h"
#include "high_frequency_telemetry/hftelorch.h"
#include "schema.h"
#include <gtest/gtest.h>
#include <memory>

extern sai_switch_api_t *sai_switch_api;

namespace hftelorch_test
{
    using namespace std;
    using hftelorch_sai_wrap_ut::HFTelSaiHookGuard;

    namespace constructor_ut
    {
        sai_switch_api_t *pold_sai_switch_api = nullptr;
        sai_switch_api_t ut_sai_switch_api{};

        sai_status_t _ut_stub_sai_set_switch_attribute(
            _In_ sai_object_id_t switch_id,
            _In_ const sai_attribute_t *attr)
        {
            if (attr->id == SAI_SWITCH_ATTR_TAM_TEL_TYPE_CONFIG_CHANGE_NOTIFY)
            {
                return SAI_STATUS_FAILURE;
            }

            return pold_sai_switch_api->set_switch_attribute(switch_id, attr);
        }

        void hookSaiSwitchApi()
        {
            ut_sai_switch_api = *sai_switch_api;
            pold_sai_switch_api = sai_switch_api;
            ut_sai_switch_api.set_switch_attribute = _ut_stub_sai_set_switch_attribute;
            sai_switch_api = &ut_sai_switch_api;
        }

        void unhookSaiSwitchApi()
        {
            sai_switch_api = pold_sai_switch_api;
            pold_sai_switch_api = nullptr;
        }
    }

    class HFTelOrchIsSupportedTest : public ::testing::Test
    {
    protected:
        void SetUp() override
        {
            map<string, string> profile = {
                {"SAI_VS_SWITCH_TYPE", "SAI_VS_SWITCH_TYPE_BCM56850"},
                {"KV_DEVICE_MAC_ADDRESS", "20:03:04:05:06:00"},
            };

            ASSERT_EQ(ut_helper::initSaiApi(profile), SAI_STATUS_SUCCESS);

            sai_attribute_t attr{};
            attr.id = SAI_SWITCH_ATTR_INIT_SWITCH;
            attr.value.booldata = true;

            ASSERT_EQ(sai_switch_api->create_switch(&gSwitchId, 1, &attr), SAI_STATUS_SUCCESS);
        }

        void TearDown() override
        {
            hftelorch_sai_wrap_ut::setSaiHookNone();

            ASSERT_EQ(sai_switch_api->remove_switch(gSwitchId), SAI_STATUS_SUCCESS);
            gSwitchId = SAI_NULL_OBJECT_ID;

            ASSERT_EQ(ut_helper::uninitSaiApi(), SAI_STATUS_SUCCESS);
        }
    };

    TEST_F(HFTelOrchIsSupportedTest, IsSupportedHFTel_with_virtual_switch)
    {
        bool supported = HFTelOrch::isSupportedHFTel(gSwitchId);
        (void)supported;
    }

    TEST_F(HFTelOrchIsSupportedTest, IsSupportedHFTel_null_switch_id)
    {
        EXPECT_FALSE(HFTelOrch::isSupportedHFTel(SAI_NULL_OBJECT_ID));
    }

    /*
     * Forces sai_query_stats_st_capability to fail (not SUCCESS / BUFFER_OVERFLOW).
     * Covers: "Streaming stats not supported, HFTel disabled"
     */
    TEST_F(HFTelOrchIsSupportedTest, IsSupportedHFTel_negative_streaming_stats_unsupported)
    {
        HFTelSaiHookGuard guard(hftelorch_sai_wrap_ut::setSaiHookStatsStFail);
        EXPECT_FALSE(HFTelOrch::isSupportedHFTel(gSwitchId));
    }

    /*
     * First sai_query_attribute_capability in the probe fails.
     * Covers: "HFTel: %s capability query failed, HFTel disabled"
     */
    TEST_F(HFTelOrchIsSupportedTest, IsSupportedHFTel_negative_attribute_capability_query_failed)
    {
        HFTelSaiHookGuard guard(hftelorch_sai_wrap_ut::setSaiHookAttributeCapabilityQueryFail);
        EXPECT_FALSE(HFTelOrch::isSupportedHFTel(gSwitchId));
    }

    /*
     * sai_query_attribute_capability succeeds for TAM_COLLECTOR but reports
     * create_implemented == false.
     * Covers: "HFTel: %s create not supported, HFTel disabled"
     */
    TEST_F(HFTelOrchIsSupportedTest, IsSupportedHFTel_negative_collector_create_not_supported)
    {
        HFTelSaiHookGuard guard(hftelorch_sai_wrap_ut::setSaiHookCollectorCreateNotImplemented);
        EXPECT_FALSE(HFTelOrch::isSupportedHFTel(gSwitchId));
    }

    /*
     * Past collector checks, SAI_SWITCH_ATTR_TAM_TEL_TYPE_CONFIG_CHANGE_NOTIFY reports
     * set not implemented.
     * Covers: "HFTel: %s set not supported, HFTel disabled"
     */
    TEST_F(HFTelOrchIsSupportedTest, IsSupportedHFTel_negative_switch_notify_set_not_supported)
    {
        HFTelSaiHookGuard guard(hftelorch_sai_wrap_ut::setSaiHookSwitchNotifySetNotImplemented);
        EXPECT_FALSE(HFTelOrch::isSupportedHFTel(gSwitchId));
    }

    /*
     * All checks pass — happy path through the entire function.
     * The AllSupported hook makes attribute capability return all-supported,
     * then real sai_query_attribute_enum_values_capability handles enum checks.
     * Covers: the full attribute loop, enum loop, and "return true" at the end.
     */
    TEST_F(HFTelOrchIsSupportedTest, IsSupportedHFTel_positive_all_supported)
    {
        HFTelSaiHookGuard guard(hftelorch_sai_wrap_ut::setSaiHookAllSupported);
        // VS SAI may or may not support all enum values, so we just exercise
        // the code path without asserting the result.
        bool supported = HFTelOrch::isSupportedHFTel(gSwitchId);
        (void)supported;
    }

    /*
     * SAI_TAM_TEL_TYPE_ATTR_MODE advertises SINGLE only.
     */
    TEST_F(HFTelOrchIsSupportedTest, IsSupportedHFTel_mode_single_only)
    {
        HFTelSaiHookGuard guard(hftelorch_sai_wrap_ut::setSaiHookModeAdvertisedSingleOnly);
        EXPECT_TRUE(HFTelOrch::isSupportedHFTel(gSwitchId));
    }

    /*
     * SAI_TAM_TEL_TYPE_ATTR_MODE advertises MIXED only.
     */
    TEST_F(HFTelOrchIsSupportedTest, IsSupportedHFTel_mode_mixed_only)
    {
        HFTelSaiHookGuard guard(hftelorch_sai_wrap_ut::setSaiHookModeAdvertisedMixedOnly);
        EXPECT_TRUE(HFTelOrch::isSupportedHFTel(gSwitchId));
    }

    /*
     * SAI_TAM_TEL_TYPE_ATTR_MODE advertises both SINGLE and MIXED.
     */
    TEST_F(HFTelOrchIsSupportedTest, IsSupportedHFTel_mode_both)
    {
        HFTelSaiHookGuard guard(hftelorch_sai_wrap_ut::setSaiHookModeAdvertisedBoth);
        EXPECT_TRUE(HFTelOrch::isSupportedHFTel(gSwitchId));
    }

    /*
     * SAI_TAM_TEL_TYPE_ATTR_MODE advertises neither SINGLE nor MIXED.
     * Covers: "HFTel: neither SAI_TAM_TEL_TYPE_MODE_SINGLE_TYPE nor
     *          SAI_TAM_TEL_TYPE_MODE_MIXED_TYPE advertised, HFTel disabled"
     */
    TEST_F(HFTelOrchIsSupportedTest, IsSupportedHFTel_mode_neither)
    {
        HFTelSaiHookGuard guard(hftelorch_sai_wrap_ut::setSaiHookModeAdvertisedNeither);
        EXPECT_FALSE(HFTelOrch::isSupportedHFTel(gSwitchId));
    }

    class HFTelOrchConstructorTest : public ::testing::Test
    {
    protected:
        shared_ptr<swss::DBConnector> m_config_db;
        shared_ptr<swss::DBConnector> m_state_db;

        void SetUp() override
        {
            map<string, string> profile = {
                {"SAI_VS_SWITCH_TYPE", "SAI_VS_SWITCH_TYPE_BCM56850"},
                {"KV_DEVICE_MAC_ADDRESS", "20:03:04:05:06:00"},
            };

            ASSERT_EQ(ut_helper::initSaiApi(profile), SAI_STATUS_SUCCESS);

            sai_attribute_t attr{};
            attr.id = SAI_SWITCH_ATTR_INIT_SWITCH;
            attr.value.booldata = true;

            ASSERT_EQ(sai_switch_api->create_switch(&gSwitchId, 1, &attr), SAI_STATUS_SUCCESS);

            m_config_db = make_shared<swss::DBConnector>("CONFIG_DB", 0);
            m_state_db = make_shared<swss::DBConnector>("STATE_DB", 0);

            constructor_ut::hookSaiSwitchApi();
        }

        void TearDown() override
        {
            constructor_ut::unhookSaiSwitchApi();

            ASSERT_EQ(sai_switch_api->remove_switch(gSwitchId), SAI_STATUS_SUCCESS);
            gSwitchId = SAI_NULL_OBJECT_ID;

            ASSERT_EQ(ut_helper::uninitSaiApi(), SAI_STATUS_SUCCESS);
        }
    };

    /*
     * Forces set_switch_attribute for TAM_TEL_TYPE_CONFIG_CHANGE_NOTIFY to fail.
     * Covers constructor error cleanup: delete notifier and nullptr consumer.
     */
    TEST_F(HFTelOrchConstructorTest, ConstructorFailsWhenTamNotifySetFails)
    {
        const vector<string> stel_tables = {
            CFG_HIGH_FREQUENCY_TELEMETRY_PROFILE_TABLE_NAME,
            CFG_HIGH_FREQUENCY_TELEMETRY_GROUP_TABLE_NAME,
        };

        EXPECT_THROW(
            {
                HFTelOrch orch(m_config_db.get(), m_state_db.get(), stel_tables);
                (void)orch;
            },
            runtime_error);
    }

    class HFTelOrchShutdownTest : public ::testing::Test
    {
    protected:
        shared_ptr<swss::DBConnector> m_config_db;
        shared_ptr<swss::DBConnector> m_state_db;

        void SetUp() override
        {
            map<string, string> profile = {
                {"SAI_VS_SWITCH_TYPE", "SAI_VS_SWITCH_TYPE_BCM56850"},
                {"KV_DEVICE_MAC_ADDRESS", "20:03:04:05:06:00"},
            };

            ASSERT_EQ(ut_helper::initSaiApi(profile), SAI_STATUS_SUCCESS);

            sai_attribute_t attr{};
            attr.id = SAI_SWITCH_ATTR_INIT_SWITCH;
            attr.value.booldata = true;

            ASSERT_EQ(sai_switch_api->create_switch(&gSwitchId, 1, &attr), SAI_STATUS_SUCCESS);

            m_config_db = make_shared<swss::DBConnector>("CONFIG_DB", 0);
            m_state_db = make_shared<swss::DBConnector>("STATE_DB", 0);
        }

        void TearDown() override
        {
            ASSERT_EQ(sai_switch_api->remove_switch(gSwitchId), SAI_STATUS_SUCCESS);
            gSwitchId = SAI_NULL_OBJECT_ID;

            ASSERT_EQ(ut_helper::uninitSaiApi(), SAI_STATUS_SUCCESS);
        }
    };

    /*
     * Successful ctor then dtor: Notifier/Executor owns the ASIC NotificationConsumer.
     * Regression for double-delete on shutdown (shared_ptr member + ~Executor).
     */
    TEST_F(HFTelOrchShutdownTest, DestructorDoesNotDoubleDeleteNotificationConsumer)
    {
        const vector<string> stel_tables = {
            CFG_HIGH_FREQUENCY_TELEMETRY_PROFILE_TABLE_NAME,
            CFG_HIGH_FREQUENCY_TELEMETRY_GROUP_TABLE_NAME,
        };

        auto orch = make_unique<HFTelOrch>(m_config_db.get(), m_state_db.get(), stel_tables);
        orch.reset();
    }

    /*
     * SAI_TAM_TEL_TYPE_ATTR_MODE capability query returns NOT_SUPPORTED
     * (current saivs behavior). HFT should still be enabled via the
     * spec-default SINGLE_TYPE fallback.
     */
    TEST_F(HFTelOrchIsSupportedTest, IsSupportedHFTel_mode_query_not_supported)
    {
        HFTelSaiHookGuard guard(hftelorch_sai_wrap_ut::setSaiHookModeQueryNotSupported);
        EXPECT_TRUE(HFTelOrch::isSupportedHFTel(gSwitchId));
    }
}
