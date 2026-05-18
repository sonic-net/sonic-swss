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
    using ::testing::InSequence;

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
        AddVnetEncapRoutingType(dash::route_type::ENCAP_TYPE_VXLAN);
        AddPLRoutingType();
        {
            InSequence seq;
            EXPECT_CALL(*mock_sai_dash_vnet_api, create_vnets).Times(1);
            EXPECT_CALL(*mock_sai_dash_outbound_ca_to_pa_api, create_outbound_ca_to_pa_entries).WillOnce(DoAll(
                Return(SAI_STATUS_SUCCESS)
            ));
            EXPECT_CALL(*mock_sai_dash_pa_validation_api, create_pa_validation_entries).Times(1);
            EXPECT_CALL(*mock_sai_dash_outbound_ca_to_pa_api, create_outbound_ca_to_pa_entries).WillOnce(DoAll(
                Return(SAI_STATUS_SUCCESS)
            ));
            EXPECT_CALL(*mock_sai_dash_outbound_ca_to_pa_api, remove_outbound_ca_to_pa_entries).Times(2).WillRepeatedly(DoAll(
                Return(SAI_STATUS_SUCCESS)
            ));
            EXPECT_CALL(*mock_sai_dash_pa_validation_api, remove_pa_validation_entries).Times(1);
            EXPECT_CALL(*mock_sai_dash_vnet_api, remove_vnets).Times(1);
        }

        CreateVnet();
        AddVnetMap();
        AddPortMap();
        AddVnetMapPL();

        RemoveVnetMap();
        RemoveVnetMapPL();
        RemoveVnet();
    }

    TEST_F(DashVnetOrchTest, AddVnetMapMissingVnetFails)
    {
        EXPECT_CALL(*mock_sai_dash_outbound_ca_to_pa_api, create_outbound_ca_to_pa_entries)
            .Times(0);
        EXPECT_CALL(*mock_sai_dash_pa_validation_api, create_pa_validation_entries)
            .Times(0);
        AddVnetEncapRoutingType(dash::route_type::ENCAP_TYPE_VXLAN);
        AddVnetMap(true);
    }

    TEST_F(DashVnetOrchTest, AddExistingOutboundCaToPaSuccessful)
    {
        AddVnetEncapRoutingType(dash::route_type::ENCAP_TYPE_VXLAN);
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
        AddVnetEncapRoutingType(dash::route_type::ENCAP_TYPE_UNSPECIFIED);
        CreateVnet();
        AddVnetMap();
        EXPECT_CALL(*mock_sai_dash_outbound_ca_to_pa_api, create_outbound_ca_to_pa_entries)
            .Times(0);
        AddVnetMap();
    }

    TEST_F(DashVnetOrchTest, AddExistPaValidationSuccessful)
    {
        AddVnetEncapRoutingType(dash::route_type::ENCAP_TYPE_VXLAN);
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
        AddVnetEncapRoutingType(dash::route_type::ENCAP_TYPE_VXLAN);
        CreateVnet();
        AddVnetMap();

        int expectedUsed = GetCrmUsedCount(CrmResourceType::CRM_DASH_IPV4_PA_VALIDATION);
        std::vector<sai_status_t> exp_status = {SAI_STATUS_OBJECT_IN_USE};

        EXPECT_CALL(*mock_sai_dash_pa_validation_api, remove_pa_validation_entries)
            .Times(1).WillOnce(DoAll(SetArrayArgument<3>(exp_status.begin(), exp_status.end()), Return(SAI_STATUS_SUCCESS)));
        RemoveVnet(true);

        int actualUsed = GetCrmUsedCount(CrmResourceType::CRM_DASH_IPV4_PA_VALIDATION);
        EXPECT_EQ(expectedUsed, actualUsed);
    }

    TEST_F(DashVnetOrchTest, VnetSaiCreateFailureNotRetried)
    {
        std::vector<sai_object_id_t> exp_oids = {SAI_NULL_OBJECT_ID};
        EXPECT_CALL(*mock_sai_dash_vnet_api, create_vnets)
            .WillOnce(DoAll(SetArrayArgument<5>(exp_oids.begin(), exp_oids.end()), Return(SAI_STATUS_INSUFFICIENT_RESOURCES)));
        CreateVnet();
    }

    TEST_F(DashVnetOrchTest, VnetMapSaiCreateInvalidParameterNotRetried)
    {
        AddVnetEncapRoutingType(dash::route_type::ENCAP_TYPE_VXLAN);
        CreateVnet();
        std::vector<sai_status_t> exp_status = {SAI_STATUS_INVALID_PARAMETER};
        EXPECT_CALL(*mock_sai_dash_outbound_ca_to_pa_api, create_outbound_ca_to_pa_entries)
            .WillOnce(DoAll(SetArrayArgument<5>(exp_status.begin(), exp_status.end()), Return(SAI_STATUS_SUCCESS)));
        AddVnetMap(true);
    }

    TEST_F(DashVnetOrchTest, RemoveNonExistentVnet)
    {
        EXPECT_CALL(*mock_sai_dash_vnet_api, remove_vnets).Times(0);
        RemoveVnet(true);
    }

    class DashVnetOrchNoApplianceTest : public MockDashOrchTest
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
            // Do NOT create appliance — tests need to verify behavior without it
        }
        void PreTearDown() override
        {
            RestoreSaiApis();
            DEINIT_SAI_API_MOCK(dash_outbound_ca_to_pa);
            DEINIT_SAI_API_MOCK(dash_pa_validation);
            DEINIT_SAI_API_MOCK(dash_vnet);
        }
    };

    TEST_F(DashVnetOrchNoApplianceTest, CreateVnetMissingApplianceNotRetried)
    {
        EXPECT_CALL(*mock_sai_dash_vnet_api, create_vnets).Times(0);
        dash::vnet::Vnet vnet = dash::vnet::Vnet();
        vnet.set_vni(5555);
        SetDashTable(APP_DASH_VNET_TABLE_NAME, "VNET_1", vnet, true, true);
    }

    TEST_F(DashVnetOrchTest, MissingProtobufVnet)
    {
        EXPECT_CALL(*mock_sai_dash_vnet_api, create_vnets).Times(0);
        SetDashTableRaw(APP_DASH_VNET_TABLE_NAME, "VNET_TEST", {}, true, true);
    }

    TEST_F(DashVnetOrchTest, InvalidProtobufVnetMap)
    {
        EXPECT_CALL(*mock_sai_dash_outbound_ca_to_pa_api, create_outbound_ca_to_pa_entries).Times(0);
        CreateVnet();
        SetDashTableRaw(APP_DASH_VNET_MAPPING_TABLE_NAME, vnet1 + ":1.2.3.4", {{ "pb", "garbage" }}, true, true);
    }

    TEST_F(DashVnetOrchTest, InvalidKeyVnetMap)
    {
        // Invalid keys should be caught per-item and consumed without throwing.
        CreateVnet();
        dash::vnet_mapping::VnetMapping vnet_map = dash::vnet_mapping::VnetMapping();
        vnet_map.set_routing_type(dash::route_type::ROUTING_TYPE_VNET_ENCAP);
        vnet_map.mutable_underlay_ip()->set_ipv4(swss::IpAddress("7.7.7.7").getV4Addr());
        EXPECT_CALL(*mock_sai_dash_outbound_ca_to_pa_api, create_outbound_ca_to_pa_entries).Times(0);
        EXPECT_NO_THROW(
            SetDashTable(APP_DASH_VNET_MAPPING_TABLE_NAME, vnet1 + ":not_an_ip", vnet_map, true, true));
    }

    TEST_F(DashVnetOrchTest, VnetCreateDeleteChurn)
    {
        for (int i = 0; i < 3; i++)
        {
            EXPECT_CALL(*mock_sai_dash_vnet_api, create_vnets).Times(1);
            CreateVnet();

            EXPECT_CALL(*mock_sai_dash_vnet_api, remove_vnets).Times(1);
            RemoveVnet();
        }
    }

    TEST_F(DashVnetOrchTest, VnetMapCreateDeleteChurn)
    {
        AddVnetEncapRoutingType(dash::route_type::ENCAP_TYPE_VXLAN);
        CreateVnet();

        // PA validation is per-VNET underlay IP, only created on first add
        EXPECT_CALL(*mock_sai_dash_pa_validation_api, create_pa_validation_entries).Times(1);

        for (int i = 0; i < 3; i++)
        {
            EXPECT_CALL(*mock_sai_dash_outbound_ca_to_pa_api, create_outbound_ca_to_pa_entries).Times(1);
            AddVnetMap();

            EXPECT_CALL(*mock_sai_dash_outbound_ca_to_pa_api, remove_outbound_ca_to_pa_entries).Times(1);
            RemoveVnetMap();
        }
    }

    TEST_F(DashVnetOrchTest, VnetMapKeyMissingIp)
    {
        // Key should be "vnet:ip" — send just vnet without IP
        CreateVnet();
        dash::vnet_mapping::VnetMapping vnet_map = dash::vnet_mapping::VnetMapping();
        vnet_map.set_routing_type(dash::route_type::ROUTING_TYPE_VNET_ENCAP);
        vnet_map.mutable_underlay_ip()->set_ipv4(swss::IpAddress("7.7.7.7").getV4Addr());
        EXPECT_CALL(*mock_sai_dash_outbound_ca_to_pa_api, create_outbound_ca_to_pa_entries).Times(0);
        EXPECT_NO_THROW(
            SetDashTable(APP_DASH_VNET_MAPPING_TABLE_NAME, vnet1, vnet_map, true, true));
    }
}
