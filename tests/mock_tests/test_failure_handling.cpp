#include "orchagent/saihelper.h"
#include "tests/mock_tests/ut_helper.h"
#include <sys/mman.h>

extern sai_switch_api_t *sai_switch_api;

namespace saifailure_test
{
    struct SaiFailureTest : public ::testing::Test
    {
    };
    uint32_t *_sai_syncd_notifications_count;
    int32_t *_sai_syncd_notification_event;
    sai_switch_api_t *pold_sai_switch_api;
    sai_switch_api_t ut_sai_switch_api;

        sai_status_t _ut_stub_sai_set_switch_attribute(
        _In_ sai_object_id_t switch_id,
        _In_ const sai_attribute_t *attr)
    {
        if (attr[0].id == SAI_REDIS_SWITCH_ATTR_NOTIFY_SYNCD)
        {
            *_sai_syncd_notifications_count = *_sai_syncd_notifications_count + 1;
            *_sai_syncd_notification_event = attr[0].value.s32;
        }
        return pold_sai_switch_api->set_switch_attribute(switch_id, attr);
    }

    void _hook_sai_switch_api()
    {
        map<string, string> profile = {
            { "SAI_VS_SWITCH_TYPE", "SAI_VS_SWITCH_TYPE_BCM56850" },
            { "KV_DEVICE_MAC_ADDRESS", "20:03:04:05:06:00" }
        };

        ut_helper::initSaiApi(profile);
        ut_sai_switch_api = *sai_switch_api;
        pold_sai_switch_api = sai_switch_api;
        
        ut_sai_switch_api.set_switch_attribute = _ut_stub_sai_set_switch_attribute;
        sai_switch_api = &ut_sai_switch_api;
    }

    void _unhook_sai_switch_api()
    {
        sai_switch_api = pold_sai_switch_api;
        ut_helper::uninitSaiApi();
    }

    TEST_F(SaiFailureTest, handleSaiFailure)
    {
        _hook_sai_switch_api();
        _sai_syncd_notifications_count = (uint32_t*)mmap(NULL, sizeof(int), PROT_READ | PROT_WRITE,
                    MAP_SHARED | MAP_ANONYMOUS, -1, 0);

        _sai_syncd_notification_event = (int32_t*)mmap(NULL, sizeof(int), PROT_READ | PROT_WRITE,
                    MAP_SHARED | MAP_ANONYMOUS, -1, 0);
        *_sai_syncd_notifications_count = 0;
        uint32_t notif_count = *_sai_syncd_notifications_count;

        ASSERT_DEATH({handleSaiCreateStatus(SAI_API_FDB, SAI_STATUS_FAILURE);}, "");
        ASSERT_EQ(*_sai_syncd_notifications_count, ++notif_count);
        ASSERT_EQ(*_sai_syncd_notification_event, SAI_REDIS_NOTIFY_SYNCD_INVOKE_DUMP);

        ASSERT_DEATH({handleSaiCreateStatus(SAI_API_HOSTIF, SAI_STATUS_INVALID_PARAMETER);}, "");
        ASSERT_EQ(*_sai_syncd_notifications_count, ++notif_count);
        ASSERT_EQ(*_sai_syncd_notification_event, SAI_REDIS_NOTIFY_SYNCD_INVOKE_DUMP);

        ASSERT_DEATH({handleSaiCreateStatus(SAI_API_PORT, SAI_STATUS_FAILURE);}, "");
        ASSERT_EQ(*_sai_syncd_notifications_count, ++notif_count);
        ASSERT_EQ(*_sai_syncd_notification_event, SAI_REDIS_NOTIFY_SYNCD_INVOKE_DUMP);

        ASSERT_DEATH({handleSaiSetStatus(SAI_API_HOSTIF, SAI_STATUS_FAILURE);}, "");
        ASSERT_EQ(*_sai_syncd_notifications_count, ++notif_count);
        ASSERT_EQ(*_sai_syncd_notification_event, SAI_REDIS_NOTIFY_SYNCD_INVOKE_DUMP);

        ASSERT_DEATH({handleSaiSetStatus(SAI_API_PORT, SAI_STATUS_FAILURE);}, "");
        ASSERT_EQ(*_sai_syncd_notifications_count, ++notif_count);
        ASSERT_EQ(*_sai_syncd_notification_event, SAI_REDIS_NOTIFY_SYNCD_INVOKE_DUMP);

        ASSERT_DEATH({handleSaiSetStatus(SAI_API_TUNNEL, SAI_STATUS_FAILURE);}, "");
        ASSERT_EQ(*_sai_syncd_notifications_count, ++notif_count);
        ASSERT_EQ(*_sai_syncd_notification_event, SAI_REDIS_NOTIFY_SYNCD_INVOKE_DUMP);

        ASSERT_DEATH({handleSaiRemoveStatus(SAI_API_LAG, SAI_STATUS_FAILURE);}, "");
        ASSERT_EQ(*_sai_syncd_notifications_count, ++notif_count);
        ASSERT_EQ(*_sai_syncd_notification_event, SAI_REDIS_NOTIFY_SYNCD_INVOKE_DUMP);

        _unhook_sai_switch_api();
    }
}
