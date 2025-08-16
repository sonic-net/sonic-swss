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
    using ::testing::DoDefault;
    using ::testing::SetArrayArgument;
    using ::testing::SetArgPointee;
    using ::testing::InSequence;

    class DashVnetOrchTest : public MockDashOrchTest
    {
    protected:
        uint32_t GetCrmUsedCount(CrmResourceType type)
        {
            uint32_t count;
            if (gCrmOrch->getCrmResUsedCounter(type, count))
            {
                return count;
            }
            return 0;
        }

        std::pair<uint32_t, uint32_t> GetVnetMapCrm()
        {
            uint32_t used_ca_to_pa = GetCrmUsedCount(CrmResourceType::CRM_DASH_IPV4_OUTBOUND_CA_TO_PA);
            uint32_t used_pa_validation = GetCrmUsedCount(CrmResourceType::CRM_DASH_IPV4_PA_VALIDATION);
            return std::make_pair(used_ca_to_pa, used_pa_validation);
        }

        bool CheckExpectedCrmMatch(uint32_t expected_ca_to_pa, uint32_t expected_pa_validation)
        {
            uint32_t actual_ca_to_pa, actual_pa_validation;
            std::tie(actual_ca_to_pa, actual_pa_validation) = GetVnetMapCrm();
            bool matches = actual_ca_to_pa == expected_ca_to_pa && actual_pa_validation == expected_pa_validation;
            if (!matches)
            {
                std::cout << "Expected CA to PA: " << expected_ca_to_pa << ", Actual CA to PA: " << actual_ca_to_pa << std::endl;
                std::cout << "Expected PA Validation: " << expected_pa_validation << ", Actual PA Validation: " << actual_pa_validation << std::endl;
            }
            return matches;
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
            std::tie(orig_ca_to_pa, orig_pa_validation) = GetVnetMapCrm();
        }
        void PreTearDown() override
        {
            RestoreSaiApis();
            DEINIT_SAI_API_MOCK(dash_outbound_ca_to_pa);
            DEINIT_SAI_API_MOCK(dash_pa_validation);
            DEINIT_SAI_API_MOCK(dash_vnet);
        }

        int orig_ca_to_pa, orig_pa_validation, expected_ca_to_pa, expected_pa_validation;
    };

    TEST_F(DashVnetOrchTest, AddRemoveVnet)
    {
        expected_ca_to_pa = orig_ca_to_pa + 1;
        expected_pa_validation = orig_pa_validation + 1;

        AddRoutingType(dash::route_type::ENCAP_TYPE_VXLAN);

        {
            InSequence seq;
            EXPECT_CALL(*mock_sai_dash_vnet_api, create_vnets).Times(1);
            EXPECT_CALL(*mock_sai_dash_outbound_ca_to_pa_api, create_outbound_ca_to_pa_entries).Times(1);
            EXPECT_CALL(*mock_sai_dash_pa_validation_api, create_pa_validation_entries).Times(1);
            EXPECT_CALL(*mock_sai_dash_outbound_ca_to_pa_api, remove_outbound_ca_to_pa_entries).Times(1);
            EXPECT_CALL(*mock_sai_dash_pa_validation_api, remove_pa_validation_entries).Times(1);
            EXPECT_CALL(*mock_sai_dash_vnet_api, remove_vnets).Times(1);
        }
        CreateVnet();
        AddVnetMap();

        EXPECT_TRUE(CheckExpectedCrmMatch(expected_ca_to_pa, expected_pa_validation));

        RemoveVnetMap();

        // Removing all VNET maps using a specific PA validation entry should also result in the removal
        // of the PA validation entry, even if the VNET still exists
        EXPECT_TRUE(CheckExpectedCrmMatch(orig_ca_to_pa, orig_pa_validation));

        RemoveVnet();

        EXPECT_TRUE(CheckExpectedCrmMatch(orig_ca_to_pa, orig_pa_validation));
    }

    TEST_F(DashVnetOrchTest, AddVnetMapMissingVnetFails)
    {
        AddRoutingType(dash::route_type::ENCAP_TYPE_VXLAN);
        EXPECT_CALL(*mock_sai_dash_outbound_ca_to_pa_api, create_outbound_ca_to_pa_entries)
            .Times(0);
        EXPECT_CALL(*mock_sai_dash_pa_validation_api, create_pa_validation_entries)
            .Times(0);
        AddVnetMap(false);
        EXPECT_TRUE(CheckExpectedCrmMatch(orig_ca_to_pa, orig_pa_validation));
    }

    TEST_F(DashVnetOrchTest, AddExistingOutboundCaToPaSuccessful)
    {
        AddRoutingType(dash::route_type::ENCAP_TYPE_VXLAN);
        CreateVnet();

        EXPECT_CALL(*mock_sai_dash_outbound_ca_to_pa_api, create_outbound_ca_to_pa_entries)
            .Times(2);
        AddVnetMap();
        std::tie(expected_ca_to_pa, expected_pa_validation) = GetVnetMapCrm();
        AddVnetMap();
        EXPECT_TRUE(CheckExpectedCrmMatch(expected_ca_to_pa, expected_pa_validation));
    }

    TEST_F(DashVnetOrchTest, RemoveNonexistVnetMapFails)
    {
        CreateVnet();
        EXPECT_CALL(*mock_sai_dash_outbound_ca_to_pa_api, remove_outbound_ca_to_pa_entries)
            .Times(1);
        RemoveVnetMap();
        EXPECT_TRUE(CheckExpectedCrmMatch(orig_ca_to_pa, orig_pa_validation));
    }

    TEST_F(DashVnetOrchTest, InvalidEncapVnetMapFails)
    {
        AddRoutingType(dash::route_type::ENCAP_TYPE_UNSPECIFIED);
        CreateVnet();
        EXPECT_CALL(*mock_sai_dash_outbound_ca_to_pa_api, create_outbound_ca_to_pa_entries)
            .Times(0);
        AddVnetMap();
        AddVnetMap();
        EXPECT_TRUE(CheckExpectedCrmMatch(orig_ca_to_pa, orig_pa_validation));
    }

    TEST_F(DashVnetOrchTest, AddExistPaValidationSuccessful)
    {
        AddRoutingType(dash::route_type::ENCAP_TYPE_VXLAN);
        CreateVnet();

        // First add operation will propogate to SAI normally
        // Second add operation will not propogate to SAI since orchagent caches PA validation entries and can check for duplicates
        EXPECT_CALL(*mock_sai_dash_pa_validation_api, create_pa_validation_entries)
            .Times(1);
        AddVnetMap();
        std::tie(expected_ca_to_pa, expected_pa_validation) = GetVnetMapCrm();
        AddVnetMap();
        EXPECT_TRUE(CheckExpectedCrmMatch(expected_ca_to_pa, expected_pa_validation));
    }

    TEST_F(DashVnetOrchTest, RemovePaValidationInUseFails)
    {
        AddRoutingType(dash::route_type::ENCAP_TYPE_VXLAN);
        CreateVnet();
        AddVnetMap();

        std::vector<sai_status_t> exp_status = { SAI_STATUS_OBJECT_IN_USE };

        EXPECT_CALL(*mock_sai_dash_pa_validation_api, remove_pa_validation_entries)
            .Times(1)
            .WillOnce(DoAll(SetArrayArgument<3>(exp_status.begin(), exp_status.end()), Return(SAI_STATUS_SUCCESS)));
        std::tie(expected_ca_to_pa, expected_pa_validation) = GetVnetMapCrm();
        RemoveVnet(false);

        EXPECT_TRUE(CheckExpectedCrmMatch(expected_ca_to_pa, expected_pa_validation));
    }

    TEST_F(DashVnetOrchTest, AddRemoveMultipleVnetMapsWithSameUnderlayIp)
    {
        AddRoutingType(dash::route_type::ENCAP_TYPE_VXLAN);
        CreateVnet();
        EXPECT_CALL(*mock_sai_dash_outbound_ca_to_pa_api, create_outbound_ca_to_pa_entries).Times(2);
        EXPECT_CALL(*mock_sai_dash_pa_validation_api, create_pa_validation_entries).Times(1); 
        EXPECT_CALL(*mock_sai_dash_outbound_ca_to_pa_api, remove_outbound_ca_to_pa_entries).Times(2);
        EXPECT_CALL(*mock_sai_dash_pa_validation_api, remove_pa_validation_entries).Times(1); 

        expected_ca_to_pa = orig_ca_to_pa + 1;
        expected_pa_validation = orig_pa_validation + 1;
        AddVnetMap();
        EXPECT_TRUE(CheckExpectedCrmMatch(expected_ca_to_pa, expected_pa_validation));

        expected_ca_to_pa++;
        AddVnetMap(true, vnet_map_ip2);
        EXPECT_TRUE(CheckExpectedCrmMatch(expected_ca_to_pa, expected_pa_validation));

        expected_ca_to_pa--;
        RemoveVnetMap();
        EXPECT_TRUE(CheckExpectedCrmMatch(expected_ca_to_pa, expected_pa_validation));

        expected_ca_to_pa--;
        expected_pa_validation--;
        RemoveVnetMap(true, vnet_map_ip2);
        EXPECT_TRUE(CheckExpectedCrmMatch(orig_ca_to_pa, orig_pa_validation));
    }
}
