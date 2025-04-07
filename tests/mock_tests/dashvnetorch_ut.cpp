#define private public
#include "directory.h"
#undef private
#define protected public
#include "orch.h"
#undef protected
#include "ut_helper.h"
#include "mock_orchagent_main.h"
#include "mock_sai_api.h"
#include "mock_orch_test.h"
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
    using namespace mock_orch_test;
    using ::testing::Return;
    using ::testing::Throw;
    using ::testing::DoAll;
    using ::testing::SetArrayArgument;

    class DashVnetOrchTest : public MockOrchTest
    {
    protected:
        int GetCrmUsedCount(CrmResourceType type)
        {
            CrmOrch::CrmResourceEntry entry = CrmOrch::CrmResourceEntry("", CrmThresholdType::CRM_PERCENTAGE, 0, 1);
            gCrmOrch->getResAvailability(type, entry);
            return entry.countersMap["STATS"].usedCounter;
        }
        void CreateApplianceEntry()
        {
            swss::IpAddress sip("1.1.1.1");
            // dash::types::IpAddress sip_addr = dash::types::IpAddress();
            // sip_addr.set_ipv4(sip.getV4Addr());
            Table appliance_table = Table(m_app_db.get(), APP_DASH_APPLIANCE_TABLE_NAME);
            dash::appliance::Appliance appliance = dash::appliance::Appliance();
            appliance.mutable_sip()->set_ipv4(sip.getV4Addr());
            appliance.set_local_region_id(100);
            // appliance.set_allocated_sip(&sip_addr);
            appliance.set_vm_vni(9999);
            appliance_table.set("APPLIANCE_1", { { "pb", appliance.SerializeAsString() } });
            m_DashOrch->addExistingData(&appliance_table);
            static_cast<Orch *>(m_DashOrch)->doTask();
        }

        void AddRoutingType(dash::route_type::EncapType encap_type)
        {
            Table route_type_table = Table(m_app_db.get(), APP_DASH_ROUTING_TYPE_TABLE_NAME);
            dash::route_type::RouteType route_type = dash::route_type::RouteType();
            dash::route_type::RouteTypeItem *rt_item = route_type.add_items();
            rt_item->set_action_type(dash::route_type::ACTION_TYPE_STATICENCAP);
            rt_item->set_encap_type(encap_type);
            route_type_table.set("VNET_ENCAP", { { "pb", route_type.SerializeAsString() } });
            m_DashOrch->addExistingData(&route_type_table);
            static_cast<Orch *>(m_DashOrch)->doTask();
        }

        void CreateVnet()
        {
            Table vnet_table = Table(m_app_db.get(), APP_DASH_VNET_TABLE_NAME);
            dash::vnet::Vnet vnet = dash::vnet::Vnet();
            vnet.set_vni(5555);
            vnet_table.set("VNET_1", { { "pb", vnet.SerializeAsString() } });
            m_dashVnetOrch->addExistingData(&vnet_table);
            static_cast<Orch *>(m_dashVnetOrch)->doTask();
        }

        void AddVnetMap()
        {
            Table vnet_map_table = Table(m_app_db.get(), APP_DASH_VNET_MAPPING_TABLE_NAME);
            dash::vnet_mapping::VnetMapping vnet_map = dash::vnet_mapping::VnetMapping();
            vnet_map.set_routing_type(dash::route_type::ROUTING_TYPE_VNET_ENCAP);
            vnet_map.mutable_underlay_ip()->set_ipv4(swss::IpAddress("7.7.7.7").getV4Addr());
            vnet_map_table.set("VNET_1:2.2.2.2", { { "pb", vnet_map.SerializeAsString() } });
            m_dashVnetOrch->addExistingData(&vnet_map_table);
            static_cast<Orch *>(m_dashVnetOrch)->doTask();
        }

        void ApplySaiMock()
        {
            INIT_SAI_API_MOCK(dash_outbound_ca_to_pa);
            INIT_SAI_API_MOCK(dash_pa_validation);
            MockSaiApis();
        }

        void PostSetUp() override
        {
            CreateApplianceEntry();
            CreateVnet();
        }
        void PreTearDown() override
        {
            RestoreSaiApis();
            DEINIT_SAI_API_MOCK(dash_outbound_ca_to_pa);
            DEINIT_SAI_API_MOCK(dash_pa_validation);
        }

    };

    TEST_F(DashVnetOrchTest, AddExistingOutboundCaToPaSuccessful)
    {
        AddRoutingType(dash::route_type::ENCAP_TYPE_VXLAN); 
        std::vector<sai_status_t> exp_status = {SAI_STATUS_ITEM_ALREADY_EXISTS};

        int expectedUsed = GetCrmUsedCount(CrmResourceType::CRM_DASH_IPV4_OUTBOUND_CA_TO_PA);
        EXPECT_CALL(*mock_sai_dash_outbound_ca_to_pa_api, create_outbound_ca_to_pa_entries)
            .Times(1).WillOnce(DoAll(SetArrayArgument<5>(exp_status.begin(), exp_status.end()), Return(SAI_STATUS_SUCCESS)));
        AddVnetMap(); 
        int actualUsed = GetCrmUsedCount(CrmResourceType::CRM_DASH_IPV4_OUTBOUND_CA_TO_PA);
        EXPECT_EQ(expectedUsed, actualUsed);
    }

    TEST_F(DashVnetOrchTest, InvalidEncapVnetMapFails)
    {
        AddRoutingType(dash::route_type::ENCAP_TYPE_UNSPECIFIED);
        EXPECT_CALL(*mock_sai_dash_outbound_ca_to_pa_api, create_outbound_ca_to_pa_entries)
            .Times(0);
        AddVnetMap();
    }

    TEST_F(DashVnetOrchTest, AddExistPaValidationSuccessful)
    {
        AddRoutingType(dash::route_type::ENCAP_TYPE_VXLAN);
        std::vector<sai_status_t> exp_status = {SAI_STATUS_ITEM_ALREADY_EXISTS};
        int expectedUsed = GetCrmUsedCount(CrmResourceType::CRM_DASH_IPV4_PA_VALIDATION);
        EXPECT_CALL(*mock_sai_dash_pa_validation_api, create_pa_validation_entries)
            .Times(1).WillOnce(DoAll(SetArrayArgument<5>(exp_status.begin(), exp_status.end()), Return(SAI_STATUS_SUCCESS)));
        AddVnetMap();
        int actualUsed = GetCrmUsedCount(CrmResourceType::CRM_DASH_IPV4_PA_VALIDATION);
        EXPECT_EQ(expectedUsed, actualUsed);
    }
}
