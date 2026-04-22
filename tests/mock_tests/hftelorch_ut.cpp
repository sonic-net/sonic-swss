#include "hftelorch_is_supported_sai_wrap.h"
#include "ut_helper.h"
#include "mock_orchagent_main.h"
#include "high_frequency_telemetry/hftelorch.h"
#include <gtest/gtest.h>

namespace hftelorch_test
{
    using namespace std;
    using hftel_is_supported_ut::SaiHookGuard;

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
            hftel_is_supported_ut::setSaiHookNone();

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
        SaiHookGuard guard(hftel_is_supported_ut::setSaiHookStatsStFail);
        EXPECT_FALSE(HFTelOrch::isSupportedHFTel(gSwitchId));
    }

    /*
     * First sai_query_attribute_capability in the probe fails.
     * Covers: "HFTel: %s capability query failed, HFTel disabled"
     */
    TEST_F(HFTelOrchIsSupportedTest, IsSupportedHFTel_negative_attribute_capability_query_failed)
    {
        SaiHookGuard guard(hftel_is_supported_ut::setSaiHookAttributeCapabilityQueryFail);
        EXPECT_FALSE(HFTelOrch::isSupportedHFTel(gSwitchId));
    }

    /*
     * sai_query_attribute_capability succeeds for TAM_COLLECTOR but reports
     * create_implemented == false.
     * Covers: "HFTel: %s create not supported, HFTel disabled"
     */
    TEST_F(HFTelOrchIsSupportedTest, IsSupportedHFTel_negative_collector_create_not_supported)
    {
        SaiHookGuard guard(hftel_is_supported_ut::setSaiHookCollectorCreateNotImplemented);
        EXPECT_FALSE(HFTelOrch::isSupportedHFTel(gSwitchId));
    }

    /*
     * Past collector checks, SAI_SWITCH_ATTR_TAM_TEL_TYPE_CONFIG_CHANGE_NOTIFY reports
     * set not implemented.
     * Covers: "HFTel: %s set not supported, HFTel disabled"
     */
    TEST_F(HFTelOrchIsSupportedTest, IsSupportedHFTel_negative_switch_notify_set_not_supported)
    {
        SaiHookGuard guard(hftel_is_supported_ut::setSaiHookSwitchNotifySetNotImplemented);
        EXPECT_FALSE(HFTelOrch::isSupportedHFTel(gSwitchId));
    }
}
