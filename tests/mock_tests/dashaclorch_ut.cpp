#define private public
#include "directory.h"
#undef private
#define protected public
#include "orch.h"
#undef protected
#include "ut_helper.h"
#include "mock_orchagent_main.h"
#include "mock_sai_api.h"
#include "mock_dash_orch_test.h"
#include "crmorch.h"
#include "dash/dashaclorch.h"

EXTERN_MOCK_FNS

extern sai_dash_acl_api_t *sai_dash_acl_api;

namespace dashaclorch_test
{
    DEFINE_SAI_GENERIC_APIS_MOCK(dash_acl, dash_acl_group, dash_acl_rule)

    using namespace mock_orch_test;
    using ::testing::Return;

    class DashAclOrchTest : public MockDashOrchTest
    {
    protected:
        const std::string kTestAclGroupName = "ACL_GROUP_1";
        std::unique_ptr<DashAclOrch> m_dashAclOrch;

        void ApplySaiMock() override
        {
            INIT_SAI_API_MOCK(dash_acl);
            MockSaiApis();
        }

        void PostSetUp() override
        {
            std::vector<std::string> dash_acl_table_names = {
                APP_DASH_PREFIX_TAG_TABLE_NAME,
                APP_DASH_ACL_IN_TABLE_NAME,
                APP_DASH_ACL_OUT_TABLE_NAME,
                APP_DASH_ACL_GROUP_TABLE_NAME,
                APP_DASH_ACL_RULE_TABLE_NAME
            };
            m_dashAclOrch = std::make_unique<DashAclOrch>(
                m_app_db.get(), dash_acl_table_names, m_DashOrch, m_dpu_app_state_db.get(), nullptr);
        }

        void PreTearDown() override
        {
            m_dashAclOrch.reset();
            RestoreSaiApis();
            DEINIT_SAI_API_MOCK(dash_acl);
        }

        DashAclGroup buildTestIpv4AclGroup()
        {
            DashAclGroup test_group;
            test_group.m_ip_version = SAI_IP_ADDR_FAMILY_IPV4;
            return test_group;
        }

        uint32_t getCrmIpv4AclGroupUsedCount()
        {
            return gCrmOrch->m_resourcesMap.at(CrmResourceType::CRM_DASH_IPV4_ACL_GROUP)
                .countersMap["STATS"].usedCounter;
        }
    };

    TEST_F(DashAclOrchTest, CreateAclGroup_OnNullOidStatus_ReturnsTaskFailedWithoutUpdatingCrm)
    {
        auto &acl_group_manager = m_dashAclOrch->getDashAclGroupMgr();
        auto test_group = buildTestIpv4AclGroup();
        const uint32_t baseline_crm_group_count = getCrmIpv4AclGroupUsedCount();

        EXPECT_CALL(*mock_sai_dash_acl_api, create_dash_acl_group)
            .WillOnce(Return(SAI_STATUS_ITEM_ALREADY_EXISTS));

        EXPECT_EQ(acl_group_manager.create(kTestAclGroupName, test_group), task_failed);
        EXPECT_FALSE(acl_group_manager.exists(kTestAclGroupName));
        EXPECT_EQ(test_group.m_dash_acl_group_id, SAI_NULL_OBJECT_ID);
        EXPECT_EQ(getCrmIpv4AclGroupUsedCount(), baseline_crm_group_count);
    }
}
