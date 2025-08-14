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
#include "dash_api/appliance.pb.h"
#include "dash_api/route_type.pb.h"
#include "dash_api/eni.pb.h"
#include "dash_api/qos.pb.h"
#include "dash_api/eni_route.pb.h"
#include "gtest/gtest.h"
#include "crmorch.h"

EXTERN_MOCK_FNS

namespace dashvnetorch_test
{
    DEFINE_SAI_API_MOCK(dash_outbound_ca_to_pa, outbound_ca_to_pa);
    DEFINE_SAI_API_MOCK(dash_pa_validation, pa_validation);
    DEFINE_SAI_GENERIC_API_OBJECT_BULK_MOCK(dash_vnet, vnet)
    using namespace mock_orch_test;
    using ::testing::Return;
    using ::testing::Throw;
    using ::testing::DoAll;
    using ::testing::SetArrayArgument;
    using ::testing::SetArgPointee;

    class DashVnetOrchTest : public MockDashOrchTest
    {
    protected:
        int GetCrmUsedCount(CrmResourceType type)
        {
            CrmOrch::CrmResourceEntry entry = CrmOrch::CrmResourceEntry("", CrmThresholdType::CRM_PERCENTAGE, 0, 1);
            gCrmOrch->getResAvailability(type, entry);
            return entry.countersMap["STATS"].usedCounter;
        }

        void ApplySaiMock() override
        {
            INIT_SAI_API_MOCK(dash_vnet);
            INIT_SAI_API_MOCK(dash_outbound_ca_to_pa);
            INIT_SAI_API_MOCK(dash_pa_validation);
            MockSaiApis();
        }

        void PostSetUp() override
        {
            CreateApplianceEntry();
        }
        void PreTearDown() override
        {
            RestoreSaiApis();
            DEINIT_SAI_API_MOCK(dash_outbound_ca_to_pa);
            DEINIT_SAI_API_MOCK(dash_pa_validation);
            DEINIT_SAI_API_MOCK(dash_vnet);
        }

    };

    TEST_F(DashVnetOrchTest, AddRemoveVnet)
    {
        std::vector<sai_status_t> exp_status = {SAI_STATUS_SUCCESS};
        AddRoutingType(dash::route_type::ENCAP_TYPE_VXLAN);
        EXPECT_CALL(*mock_sai_dash_vnet_api, create_vnets)
            .Times(1).WillOnce(DoAll(
                SetArgPointee<5>(0x1234),
                SetArrayArgument<6>(exp_status.begin(), exp_status.end()),
                Return(SAI_STATUS_SUCCESS)
            )
            );
        CreateVnet();
        EXPECT_CALL(*mock_sai_dash_outbound_ca_to_pa_api, create_outbound_ca_to_pa_entries)
            .Times(1).WillOnce(DoAll(SetArrayArgument<5>(exp_status.begin(), exp_status.end()), Return(SAI_STATUS_SUCCESS)));
        EXPECT_CALL(*mock_sai_dash_pa_validation_api, create_pa_validation_entries)
            .Times(1).WillOnce(DoAll(SetArrayArgument<5>(exp_status.begin(), exp_status.end()), Return(SAI_STATUS_SUCCESS)));
        AddVnetMap();

        AddPortMap();
        AddVnetMapPL();

        EXPECT_CALL(*mock_sai_dash_outbound_ca_to_pa_api, remove_outbound_ca_to_pa_entries)
            .Times(1).WillOnce(DoAll(SetArrayArgument<3>(exp_status.begin(), exp_status.end()), Return(SAI_STATUS_SUCCESS)));

        RemoveVnetMap();
        EXPECT_CALL(*mock_sai_dash_pa_validation_api, remove_pa_validation_entries)
            .Times(1).WillOnce(DoAll(SetArrayArgument<3>(exp_status.begin(), exp_status.end()), Return(SAI_STATUS_SUCCESS)));
        EXPECT_CALL(*mock_sai_dash_vnet_api, remove_vnets)
            .Times(1).WillOnce(DoAll(SetArrayArgument<3>(exp_status.begin(), exp_status.end()), Return(SAI_STATUS_SUCCESS)));
        RemoveVnet();
    }

    TEST_F(DashVnetOrchTest, AddVnetMapMissingVnetFails)
    {
        EXPECT_CALL(*mock_sai_dash_outbound_ca_to_pa_api, create_outbound_ca_to_pa_entries)
            .Times(0);
        EXPECT_CALL(*mock_sai_dash_pa_validation_api, create_pa_validation_entries)
            .Times(0);
        AddRoutingType(dash::route_type::ENCAP_TYPE_VXLAN);
        AddVnetMap(false);
    }

    TEST_F(DashVnetOrchTest, AddExistingOutboundCaToPaSuccessful)
    {
        AddRoutingType(dash::route_type::ENCAP_TYPE_VXLAN); 
        CreateVnet();
        AddVnetMap();
        std::vector<sai_status_t> exp_status = {SAI_STATUS_ITEM_ALREADY_EXISTS};

        int expectedUsed = GetCrmUsedCount(CrmResourceType::CRM_DASH_IPV4_OUTBOUND_CA_TO_PA);
        EXPECT_CALL(*mock_sai_dash_outbound_ca_to_pa_api, create_outbound_ca_to_pa_entries)
            .Times(1).WillOnce(DoAll(SetArrayArgument<5>(exp_status.begin(), exp_status.end()), Return(SAI_STATUS_SUCCESS)));
        AddVnetMap(); 
        int actualUsed = GetCrmUsedCount(CrmResourceType::CRM_DASH_IPV4_OUTBOUND_CA_TO_PA);
        EXPECT_EQ(expectedUsed, actualUsed);
    }

    TEST_F(DashVnetOrchTest, RemoveNonexistVnetMapFails)
    {
        int expectedUsed = GetCrmUsedCount(CrmResourceType::CRM_DASH_IPV4_OUTBOUND_CA_TO_PA);
        std::vector<sai_status_t> exp_status = {SAI_STATUS_ITEM_NOT_FOUND};
        EXPECT_CALL(*mock_sai_dash_outbound_ca_to_pa_api, remove_outbound_ca_to_pa_entries)
            .Times(1).WillOnce(DoAll(SetArrayArgument<3>(exp_status.begin(), exp_status.end()), Return(SAI_STATUS_SUCCESS)));
        RemoveVnetMap(); 
        int actualUsed = GetCrmUsedCount(CrmResourceType::CRM_DASH_IPV4_OUTBOUND_CA_TO_PA);
        EXPECT_EQ(expectedUsed, actualUsed);
    }

    TEST_F(DashVnetOrchTest, InvalidEncapVnetMapFails)
    {
        AddRoutingType(dash::route_type::ENCAP_TYPE_UNSPECIFIED);
        CreateVnet();
        AddVnetMap();
        EXPECT_CALL(*mock_sai_dash_outbound_ca_to_pa_api, create_outbound_ca_to_pa_entries)
            .Times(0);
        AddVnetMap();
    }

    TEST_F(DashVnetOrchTest, AddExistPaValidationSuccessful)
    {
        AddRoutingType(dash::route_type::ENCAP_TYPE_VXLAN);
        CreateVnet();
        std::vector<sai_status_t> exp_status = {SAI_STATUS_ITEM_ALREADY_EXISTS};
        int expectedUsed = GetCrmUsedCount(CrmResourceType::CRM_DASH_IPV4_PA_VALIDATION);
        EXPECT_CALL(*mock_sai_dash_pa_validation_api, create_pa_validation_entries)
            .Times(1).WillOnce(DoAll(SetArrayArgument<5>(exp_status.begin(), exp_status.end()), Return(SAI_STATUS_SUCCESS)));
        AddVnetMap();
        int actualUsed = GetCrmUsedCount(CrmResourceType::CRM_DASH_IPV4_PA_VALIDATION);
        EXPECT_EQ(expectedUsed, actualUsed);
    }

    TEST_F(DashVnetOrchTest, RemovePaValidationInUseFails)
    {
        AddRoutingType(dash::route_type::ENCAP_TYPE_VXLAN);
        CreateVnet();
        AddVnetMap();

        int expectedUsed = GetCrmUsedCount(CrmResourceType::CRM_DASH_IPV4_PA_VALIDATION);
        std::vector<sai_status_t> exp_status = {SAI_STATUS_OBJECT_IN_USE};

        EXPECT_CALL(*mock_sai_dash_pa_validation_api, remove_pa_validation_entries)
            .Times(1).WillOnce(DoAll(SetArrayArgument<3>(exp_status.begin(), exp_status.end()), Return(SAI_STATUS_SUCCESS)));
        RemoveVnet(false);

        int actualUsed = GetCrmUsedCount(CrmResourceType::CRM_DASH_IPV4_PA_VALIDATION);
        EXPECT_EQ(expectedUsed, actualUsed);
    }
}
