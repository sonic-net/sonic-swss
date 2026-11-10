#pragma once

#include "ut_helper.h"
#include "mock_orchagent_main.h"

#include <map>
#include <string>

/*
 * Owns the SAI API and one switch for a whole GTest suite. Create it in
 * SetUpTestCase() and destroy it in TearDownTestCase().
 */
class MockSaiSwitchEnv
{
public:
    MockSaiSwitchEnv()
    {
        std::map<std::string, std::string> profile = {
            { "SAI_VS_SWITCH_TYPE", "SAI_VS_SWITCH_TYPE_BCM56850" },
            { "KV_DEVICE_MAC_ADDRESS", "20:03:04:05:06:00" }
        };

        if (ut_helper::initSaiApi(profile) != SAI_STATUS_SUCCESS)
        {
            return;
        }
        m_saiInitialized = true;

        sai_attribute_t attr;
        attr.id = SAI_SWITCH_ATTR_INIT_SWITCH;
        attr.value.booldata = true;
        if (sai_switch_api->create_switch(&gSwitchId, 1, &attr) != SAI_STATUS_SUCCESS)
        {
            return;
        }
        m_switchId = gSwitchId;

        attr.id = SAI_SWITCH_ATTR_SRC_MAC_ADDRESS;
        if (sai_switch_api->get_switch_attribute(gSwitchId, 1, &attr) != SAI_STATUS_SUCCESS)
        {
            return;
        }
        gMacAddress = attr.value.mac;

        attr.id = SAI_SWITCH_ATTR_DEFAULT_VIRTUAL_ROUTER_ID;
        if (sai_switch_api->get_switch_attribute(gSwitchId, 1, &attr) != SAI_STATUS_SUCCESS)
        {
            return;
        }
        gVirtualRouterId = attr.value.oid;

        m_ready = true;
    }

    ~MockSaiSwitchEnv()
    {
        /* Uninitialize SAI even after a failure so later suites start clean. */
        if (m_switchId != SAI_NULL_OBJECT_ID)
        {
            EXPECT_EQ(sai_switch_api->remove_switch(m_switchId), SAI_STATUS_SUCCESS)
                << "Failed to remove the suite's SAI switch";
            gSwitchId = SAI_NULL_OBJECT_ID;
        }

        if (m_saiInitialized)
        {
            EXPECT_EQ(ut_helper::uninitSaiApi(), SAI_STATUS_SUCCESS);
        }
    }

    MockSaiSwitchEnv(const MockSaiSwitchEnv &) = delete;
    MockSaiSwitchEnv &operator=(const MockSaiSwitchEnv &) = delete;

    bool ready() const
    {
        return m_ready;
    }

private:
    bool m_saiInitialized = false;
    sai_object_id_t m_switchId = SAI_NULL_OBJECT_ID;
    bool m_ready = false;
};
