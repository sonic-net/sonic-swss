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


EXTERN_MOCK_FNS

namespace dashorch_test
{
    DEFINE_SAI_GENERIC_APIS_MOCK(dash_eni, eni)
    DEFINE_SAI_ENTRY_APIS_MOCK(dash_trusted_vni, global_trusted_vni)
    using namespace mock_orch_test;
    using ::testing::DoAll;
    using ::testing::Return;
    using ::testing::SetArgPointee;
    using ::testing::SaveArg;
    using ::testing::SaveArgPointee;
    using ::testing::Invoke;
    using ::testing::InSequence;
    class DashOrchTest : public MockDashOrchTest {
        void ApplySaiMock()
        {
            INIT_SAI_API_MOCK(dash_eni);
            INIT_SAI_API_MOCK(dash_trusted_vni);
            MockSaiApis();
        }

        void PreTearDown() override
        {
            RestoreSaiApis();
            DEINIT_SAI_API_MOCK(dash_trusted_vni);
            DEINIT_SAI_API_MOCK(dash_eni);
        }

        public:
            void VerifyEniMode(std::vector<sai_attribute_t> &actual_attrs, sai_dash_eni_mode_t expected_mode)
            {
                for (auto attr : actual_attrs) {
                    if (attr.id == SAI_ENI_ATTR_DASH_ENI_MODE) {
                        EXPECT_EQ(attr.value.u32, expected_mode);
                        return;
                    }
                }
                FAIL() << "SAI_ENI_ATTR_DASH_ENI_MODE not found in attributes";
            }
    };

        TEST_F(DashOrchTest, GetNonExistRoutingType)
    {   
        dash::route_type::RouteType route_type;
        bool success = m_DashOrch->getRouteTypeActions(dash::route_type::RoutingType::ROUTING_TYPE_DIRECT, route_type);
        EXPECT_FALSE(success);
    }

    TEST_F(DashOrchTest, DuplicateRoutingTypeEntry)
    {
        dash::route_type::RouteType route_type1;
        dash::route_type::RouteTypeItem *item1 = route_type1.add_items();
        item1->set_action_type(dash::route_type::ActionType::ACTION_TYPE_STATICENCAP);
        bool success = m_DashOrch->addRoutingTypeEntry(dash::route_type::RoutingType::ROUTING_TYPE_VNET, route_type1);
        EXPECT_TRUE(success);
        EXPECT_EQ(m_DashOrch->routing_type_entries_.size(), 1);
        EXPECT_EQ(m_DashOrch->routing_type_entries_[dash::route_type::RoutingType::ROUTING_TYPE_VNET].items()[0].action_type(), item1->action_type());

        dash::route_type::RouteType route_type2;
        dash::route_type::RouteTypeItem *item2 = route_type2.add_items();
        item2->set_action_type(dash::route_type::ActionType::ACTION_TYPE_DECAP);
        success = m_DashOrch->addRoutingTypeEntry(dash::route_type::RoutingType::ROUTING_TYPE_VNET, route_type2);
        EXPECT_TRUE(success);
        EXPECT_EQ(m_DashOrch->routing_type_entries_[dash::route_type::RoutingType::ROUTING_TYPE_VNET].items()[0].action_type(), item1->action_type());
    }

    TEST_F(DashOrchTest, RemoveNonExistRoutingType)
    {
        bool success = m_DashOrch->removeRoutingTypeEntry(dash::route_type::RoutingType::ROUTING_TYPE_DROP);
        EXPECT_TRUE(success);
    }

    TEST_F(DashOrchTest, SetEniMode)
    {
        CreateApplianceEntry();
        CreateVnet();

        Table eni_table = Table(m_app_db.get(), APP_DASH_ENI_TABLE_NAME);
        int num_attrs;
        const sai_attribute_t* attr_start;
        std::vector<sai_attribute_t> actual_attrs;

        dash::eni::Eni eni;
        std::string mac = "f4:93:9f:ef:c4:7e";
        eni.set_admin_state(dash::eni::State::STATE_ENABLED);
        eni.set_eni_id("eni1");
        eni.set_mac_address(mac);
        eni.set_vnet(vnet1);
        eni.mutable_underlay_ip()->set_ipv4(swss::IpAddress("1.2.3.4").getV4Addr());
        eni.set_eni_mode(dash::eni::MODE_VM);

        EXPECT_CALL(*mock_sai_dash_eni_api, create_eni).Times(3)
            .WillRepeatedly(
                DoAll(
                    SaveArg<2>(&num_attrs),
                    SaveArg<3>(&attr_start),
                    Invoke(old_sai_dash_eni_api, &sai_dash_eni_api_t::create_eni) // Call the original function
                )
            );

        SetDashTable(APP_DASH_ENI_TABLE_NAME, "eni1", eni);
        actual_attrs.assign(attr_start, attr_start + num_attrs);
        VerifyEniMode(actual_attrs, SAI_DASH_ENI_MODE_VM);
        SetDashTable(APP_DASH_ENI_TABLE_NAME, "eni1", eni, false);

        eni.set_eni_mode(dash::eni::MODE_FNIC);
        SetDashTable(APP_DASH_ENI_TABLE_NAME, "eni1", eni);
        actual_attrs.clear();
        actual_attrs.assign(attr_start, attr_start + num_attrs);
        VerifyEniMode(actual_attrs, SAI_DASH_ENI_MODE_FNIC);
        SetDashTable(APP_DASH_ENI_TABLE_NAME, "eni1", eni, false);

        eni.set_eni_mode(dash::eni::MODE_UNSPECIFIED);
        SetDashTable(APP_DASH_ENI_TABLE_NAME, "eni1", eni);
        actual_attrs.clear();
        actual_attrs.assign(attr_start, attr_start + num_attrs);
        VerifyEniMode(actual_attrs, SAI_DASH_ENI_MODE_VM); // Default
        SetDashTable(APP_DASH_ENI_TABLE_NAME, "eni1", eni, false);
    }

    TEST_F(DashOrchTest, CreateRemoveApplianceTrustedVnisSingle)
    {
        int trusted_vni = 100;
        dash::appliance::Appliance appliance = BuildApplianceEntry();
        appliance.mutable_trusted_vnis()->set_value(trusted_vni);

        sai_global_trusted_vni_entry_t actual_entry;

        EXPECT_CALL(*mock_sai_dash_trusted_vni_api, create_global_trusted_vni_entry)
            .WillOnce(
                DoAll(
                    SaveArgPointee<0>(&actual_entry),
                    Invoke(old_sai_dash_trusted_vni_api, &sai_dash_trusted_vni_api_t::create_global_trusted_vni_entry)
                )
            );
        EXPECT_CALL(*mock_sai_dash_trusted_vni_api, remove_global_trusted_vni_entry)
            .WillOnce(
                DoAll(
                    SaveArgPointee<0>(&actual_entry),
                    Invoke(old_sai_dash_trusted_vni_api, &sai_dash_trusted_vni_api_t::remove_global_trusted_vni_entry)
                )
            );

        SetDashTable(APP_DASH_APPLIANCE_TABLE_NAME, appliance1, appliance);
        EXPECT_EQ(actual_entry.vni_range.min, trusted_vni);
        EXPECT_EQ(actual_entry.vni_range.max, trusted_vni);

        SetDashTable(APP_DASH_APPLIANCE_TABLE_NAME, appliance1, dash::appliance::Appliance(), false);
        EXPECT_EQ(actual_entry.vni_range.min, trusted_vni);
        EXPECT_EQ(actual_entry.vni_range.max, trusted_vni);
    }

    TEST_F(DashOrchTest, CreateRemoveApplianceTrustedVnisRange)
    {
        int min_trusted_vni = 500;
        int max_trusted_vni = 600;
        dash::appliance::Appliance appliance = BuildApplianceEntry();
        appliance.mutable_trusted_vnis()->mutable_range()->set_min(min_trusted_vni);
        appliance.mutable_trusted_vnis()->mutable_range()->set_max(max_trusted_vni);

        sai_global_trusted_vni_entry_t actual_entry;

        EXPECT_CALL(*mock_sai_dash_trusted_vni_api, create_global_trusted_vni_entry)
            .WillOnce(
                DoAll(
                    SaveArgPointee<0>(&actual_entry),
                    Invoke(old_sai_dash_trusted_vni_api, &sai_dash_trusted_vni_api_t::create_global_trusted_vni_entry)
                )
            );

        EXPECT_CALL(*mock_sai_dash_trusted_vni_api, remove_global_trusted_vni_entry)
            .WillOnce(
                DoAll(
                    SaveArgPointee<0>(&actual_entry),
                    Invoke(old_sai_dash_trusted_vni_api, &sai_dash_trusted_vni_api_t::remove_global_trusted_vni_entry)
                )
            );

        SetDashTable(APP_DASH_APPLIANCE_TABLE_NAME, appliance1, appliance);
        EXPECT_EQ(actual_entry.vni_range.min, min_trusted_vni);
        EXPECT_EQ(actual_entry.vni_range.max, max_trusted_vni);

        SetDashTable(APP_DASH_APPLIANCE_TABLE_NAME, appliance1, dash::appliance::Appliance(), false);
        EXPECT_EQ(actual_entry.vni_range.min, min_trusted_vni);
        EXPECT_EQ(actual_entry.vni_range.max, max_trusted_vni);
    }
}