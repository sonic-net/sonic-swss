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
#include "dash_api/types.pb.h"

EXTERN_MOCK_FNS

namespace dashorch_test
{
    DEFINE_SAI_GENERIC_APIS_MOCK(dash_eni, eni)
    DEFINE_SAI_ENTRY_APIS_MOCK(dash_trusted_vni, global_trusted_vni, eni_trusted_vni)
    using namespace mock_orch_test;
    using ::testing::DoAll;
    using ::testing::Return;
    using ::testing::SetArgPointee;
    using ::testing::SaveArg;
    using ::testing::SaveArgPointee;
    using ::testing::Invoke;
    using ::testing::InSequence;
    using dash::types::ValueOrRange;

    ValueOrRange GenVni(int value)
    {
        ValueOrRange vni;
        vni.set_value(value);
        return vni;
    }
    ValueOrRange GenVni(int min, int max)
    {
        ValueOrRange vni;
        vni.mutable_range()->set_min(min);
        vni.mutable_range()->set_max(max);
        return vni;
    }

    ValueOrRange vni_value1 = GenVni(1000);
    ValueOrRange vni_value2 = GenVni(2000);
    ValueOrRange vni_range1 = GenVni(3000, 4000);
    ValueOrRange vni_range2 = GenVni(5000, 6000);

    std::string GetVniString(const ValueOrRange &vni)
    {
        if (vni.has_value())
        {
            return std::to_string(vni.value());
        }
        else if (vni.has_range())
        {
            return std::to_string(vni.range().min()) + "_" + std::to_string(vni.range().max());
        }
        else
        {
            return "Invalid VNI";
        }
    }
    class DashOrchTest : public MockDashOrchTest, public ::testing::WithParamInterface<std::tuple<ValueOrRange, ValueOrRange>>
    {

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
        void VerifyTrustedVniEntry(sai_u32_range_t &actual_entry, const ValueOrRange &expected_vni)
        {
            if (expected_vni.has_value())
            {
                EXPECT_EQ(actual_entry.min, expected_vni.value());
                EXPECT_EQ(actual_entry.max, expected_vni.value());
            }
            else if (expected_vni.has_range())
            {
                EXPECT_EQ(actual_entry.min, expected_vni.range().min());
                EXPECT_EQ(actual_entry.max, expected_vni.range().max());
            }
            else
            {
                FAIL() << "Invalid ValueOrRange provided";
            }
        }
        void VerifyEniMode(std::vector<sai_attribute_t> &actual_attrs, sai_dash_eni_mode_t expected_mode)
        {
            for (auto attr : actual_attrs)
            {
                if (attr.id == SAI_ENI_ATTR_DASH_ENI_MODE)
                {
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
        const sai_attribute_t *attr_start;
        std::vector<sai_attribute_t> actual_attrs;

        dash::eni::Eni eni = BuildEniEntry();

        EXPECT_CALL(*mock_sai_dash_eni_api, create_eni).Times(3).WillRepeatedly(DoAll(SaveArg<2>(&num_attrs), SaveArg<3>(&attr_start), Invoke(old_sai_dash_eni_api, &sai_dash_eni_api_t::create_eni) // Call the original function
                                                                                      ));

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
                    Invoke(old_sai_dash_trusted_vni_api, &sai_dash_trusted_vni_api_t::create_global_trusted_vni_entry)));
        EXPECT_CALL(*mock_sai_dash_trusted_vni_api, remove_global_trusted_vni_entry)
            .WillOnce(
                DoAll(
                    SaveArgPointee<0>(&actual_entry),
                    Invoke(old_sai_dash_trusted_vni_api, &sai_dash_trusted_vni_api_t::remove_global_trusted_vni_entry)));

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
                    Invoke(old_sai_dash_trusted_vni_api, &sai_dash_trusted_vni_api_t::create_global_trusted_vni_entry)));

        EXPECT_CALL(*mock_sai_dash_trusted_vni_api, remove_global_trusted_vni_entry)
            .WillOnce(
                DoAll(
                    SaveArgPointee<0>(&actual_entry),
                    Invoke(old_sai_dash_trusted_vni_api, &sai_dash_trusted_vni_api_t::remove_global_trusted_vni_entry)));

        SetDashTable(APP_DASH_APPLIANCE_TABLE_NAME, appliance1, appliance);
        EXPECT_EQ(actual_entry.vni_range.min, min_trusted_vni);
        EXPECT_EQ(actual_entry.vni_range.max, max_trusted_vni);

        SetDashTable(APP_DASH_APPLIANCE_TABLE_NAME, appliance1, dash::appliance::Appliance(), false);
        EXPECT_EQ(actual_entry.vni_range.min, min_trusted_vni);
        EXPECT_EQ(actual_entry.vni_range.max, max_trusted_vni);
    }

    TEST_F(DashOrchTest, CreateRemoveEniTrustedVnisSingle)
    {
        CreateApplianceEntry();
        CreateVnet();

        int trusted_vni = 200;
        dash::eni::Eni eni = BuildEniEntry();
        eni.mutable_trusted_vnis()->set_value(trusted_vni);

        sai_eni_trusted_vni_entry_t actual_entry;

        EXPECT_CALL(*mock_sai_dash_trusted_vni_api, create_eni_trusted_vni_entry)
            .WillOnce(
                DoAll(
                    SaveArgPointee<0>(&actual_entry),
                    Invoke(old_sai_dash_trusted_vni_api, &sai_dash_trusted_vni_api_t::create_eni_trusted_vni_entry)));
        EXPECT_CALL(*mock_sai_dash_trusted_vni_api, remove_eni_trusted_vni_entry)
            .WillOnce(
                DoAll(
                    SaveArgPointee<0>(&actual_entry),
                    Invoke(old_sai_dash_trusted_vni_api, &sai_dash_trusted_vni_api_t::remove_eni_trusted_vni_entry)));

        SetDashTable(APP_DASH_ENI_TABLE_NAME, eni1, eni);
        EXPECT_EQ(actual_entry.vni_range.min, trusted_vni);
        EXPECT_EQ(actual_entry.vni_range.max, trusted_vni);

        SetDashTable(APP_DASH_ENI_TABLE_NAME, eni1, dash::eni::Eni(), false);
        EXPECT_EQ(actual_entry.vni_range.min, trusted_vni);
        EXPECT_EQ(actual_entry.vni_range.max, trusted_vni);
    }

    TEST_F(DashOrchTest, CreateRemoveEniTrustedVnisRange)
    {
        CreateApplianceEntry();
        CreateVnet();

        int min_trusted_vni = 700;
        int max_trusted_vni = 800;
        dash::eni::Eni eni = BuildEniEntry();
        eni.mutable_trusted_vnis()->mutable_range()->set_min(min_trusted_vni);
        eni.mutable_trusted_vnis()->mutable_range()->set_max(max_trusted_vni);

        sai_eni_trusted_vni_entry_t actual_entry;

        EXPECT_CALL(*mock_sai_dash_trusted_vni_api, create_eni_trusted_vni_entry)
            .WillOnce(
                DoAll(
                    SaveArgPointee<0>(&actual_entry),
                    Invoke(old_sai_dash_trusted_vni_api, &sai_dash_trusted_vni_api_t::create_eni_trusted_vni_entry)));

        EXPECT_CALL(*mock_sai_dash_trusted_vni_api, remove_eni_trusted_vni_entry)
            .WillOnce(
                DoAll(
                    SaveArgPointee<0>(&actual_entry),
                    Invoke(old_sai_dash_trusted_vni_api, &sai_dash_trusted_vni_api_t::remove_eni_trusted_vni_entry)));

        SetDashTable(APP_DASH_ENI_TABLE_NAME, eni1, eni);
        EXPECT_EQ(actual_entry.vni_range.min, min_trusted_vni);
        EXPECT_EQ(actual_entry.vni_range.max, max_trusted_vni);

        SetDashTable(APP_DASH_ENI_TABLE_NAME, eni1, dash::eni::Eni(), false);
        EXPECT_EQ(actual_entry.vni_range.min, min_trusted_vni);
        EXPECT_EQ(actual_entry.vni_range.max, max_trusted_vni);
    }

    TEST_F(DashOrchTest, DuplicateSetEniTrustedVniSingle)
    {
        CreateApplianceEntry();
        CreateVnet();

        int trusted_vni = 300;
        dash::eni::Eni eni = BuildEniEntry();
        eni.mutable_trusted_vnis()->set_value(trusted_vni);

        EXPECT_CALL(*mock_sai_dash_trusted_vni_api, create_eni_trusted_vni_entry).Times(1);
        EXPECT_CALL(*mock_sai_dash_trusted_vni_api, remove_eni_trusted_vni_entry).Times(0);

        SetDashTable(APP_DASH_ENI_TABLE_NAME, eni1, eni);
        SetDashTable(APP_DASH_ENI_TABLE_NAME, eni1, eni);
    }

    TEST_F(DashOrchTest, DuplicateSetEniTrustedVniRange)
    {
        CreateApplianceEntry();
        CreateVnet();

        int min_trusted_vni = 900;
        int max_trusted_vni = 1000;
        dash::eni::Eni eni = BuildEniEntry();
        eni.mutable_trusted_vnis()->mutable_range()->set_min(min_trusted_vni);
        eni.mutable_trusted_vnis()->mutable_range()->set_max(max_trusted_vni);

        EXPECT_CALL(*mock_sai_dash_trusted_vni_api, create_eni_trusted_vni_entry).Times(1);
        EXPECT_CALL(*mock_sai_dash_trusted_vni_api, remove_eni_trusted_vni_entry).Times(0);

        SetDashTable(APP_DASH_ENI_TABLE_NAME, eni1, eni);
        SetDashTable(APP_DASH_ENI_TABLE_NAME, eni1, eni);
    }

    TEST_P(DashOrchTest, ChangeEniTrustedVni)
    {
        CreateApplianceEntry();
        CreateVnet();

        ValueOrRange orig_vni, changed_vni;
        std::tie(orig_vni, changed_vni) = GetParam();

        dash::eni::Eni eni = BuildEniEntry();
        sai_eni_trusted_vni_entry_t actual_entry;
        sai_eni_trusted_vni_entry_t removed_entry;
        to_sai(changed_vni, removed_entry.vni_range);

        {
            InSequence seq;

            // Initial set
            EXPECT_CALL(*mock_sai_dash_trusted_vni_api, create_eni_trusted_vni_entry)
                .WillOnce(
                    DoAll(
                        SaveArgPointee<0>(&actual_entry),
                        Invoke(old_sai_dash_trusted_vni_api, &sai_dash_trusted_vni_api_t::create_eni_trusted_vni_entry)));

            // We expect 3 additional changes, orig->changed, changed->orig, and orig->changed
            EXPECT_CALL(*mock_sai_dash_trusted_vni_api, remove_eni_trusted_vni_entry)
                .WillOnce(
                    DoAll(
                        SaveArgPointee<0>(&removed_entry),
                        Invoke(old_sai_dash_trusted_vni_api, &sai_dash_trusted_vni_api_t::remove_eni_trusted_vni_entry)));

            EXPECT_CALL(*mock_sai_dash_trusted_vni_api, create_eni_trusted_vni_entry)
                .WillOnce(
                    DoAll(
                        SaveArgPointee<0>(&actual_entry),
                        Invoke(old_sai_dash_trusted_vni_api, &sai_dash_trusted_vni_api_t::create_eni_trusted_vni_entry)));

            EXPECT_CALL(*mock_sai_dash_trusted_vni_api, remove_eni_trusted_vni_entry)
                .WillOnce(
                    DoAll(
                        SaveArgPointee<0>(&removed_entry),
                        Invoke(old_sai_dash_trusted_vni_api, &sai_dash_trusted_vni_api_t::remove_eni_trusted_vni_entry)));

            EXPECT_CALL(*mock_sai_dash_trusted_vni_api, create_eni_trusted_vni_entry)
                .WillOnce(
                    DoAll(
                        SaveArgPointee<0>(&actual_entry),
                        Invoke(old_sai_dash_trusted_vni_api, &sai_dash_trusted_vni_api_t::create_eni_trusted_vni_entry)));

            EXPECT_CALL(*mock_sai_dash_trusted_vni_api, remove_eni_trusted_vni_entry)
                .WillOnce(
                    DoAll(
                        SaveArgPointee<0>(&removed_entry),
                        Invoke(old_sai_dash_trusted_vni_api, &sai_dash_trusted_vni_api_t::remove_eni_trusted_vni_entry)));

            EXPECT_CALL(*mock_sai_dash_trusted_vni_api, create_eni_trusted_vni_entry)
                .WillOnce(
                    DoAll(
                        SaveArgPointee<0>(&actual_entry),
                        Invoke(old_sai_dash_trusted_vni_api, &sai_dash_trusted_vni_api_t::create_eni_trusted_vni_entry)));
        }

        for (int i = 0; i < 2; i++)
        {
            eni.mutable_trusted_vnis()->CopyFrom(orig_vni);
            SetDashTable(APP_DASH_ENI_TABLE_NAME, eni1, eni);
            VerifyTrustedVniEntry(removed_entry.vni_range, changed_vni);
            VerifyTrustedVniEntry(actual_entry.vni_range, orig_vni);

            eni.mutable_trusted_vnis()->CopyFrom(changed_vni);
            SetDashTable(APP_DASH_ENI_TABLE_NAME, eni1, eni);
            VerifyTrustedVniEntry(removed_entry.vni_range, orig_vni);
            VerifyTrustedVniEntry(actual_entry.vni_range, changed_vni);
        }
    }

    TEST_P(DashOrchTest, ChangeApplianceTrustedVni)
    {
        ValueOrRange orig_vni, changed_vni;
        std::tie(orig_vni, changed_vni) = GetParam();

        dash::appliance::Appliance appliance = BuildApplianceEntry();
        sai_global_trusted_vni_entry_t actual_entry;
        sai_global_trusted_vni_entry_t removed_entry;
        to_sai(changed_vni, removed_entry.vni_range);

        {
            InSequence seq;

            // Initial set
            EXPECT_CALL(*mock_sai_dash_trusted_vni_api, create_global_trusted_vni_entry)
                .WillOnce(
                    DoAll(
                        SaveArgPointee<0>(&actual_entry),
                        Invoke(old_sai_dash_trusted_vni_api, &sai_dash_trusted_vni_api_t::create_global_trusted_vni_entry)));

            // We expect 3 additional changes, orig->changed, changed->orig, and orig->changed
            EXPECT_CALL(*mock_sai_dash_trusted_vni_api, remove_global_trusted_vni_entry)
                .WillOnce(
                    DoAll(
                        SaveArgPointee<0>(&removed_entry),
                        Invoke(old_sai_dash_trusted_vni_api, &sai_dash_trusted_vni_api_t::remove_global_trusted_vni_entry)));

            EXPECT_CALL(*mock_sai_dash_trusted_vni_api, create_global_trusted_vni_entry)
                .WillOnce(
                    DoAll(
                        SaveArgPointee<0>(&actual_entry),
                        Invoke(old_sai_dash_trusted_vni_api, &sai_dash_trusted_vni_api_t::create_global_trusted_vni_entry)));

            EXPECT_CALL(*mock_sai_dash_trusted_vni_api, remove_global_trusted_vni_entry)
                .WillOnce(
                    DoAll(
                        SaveArgPointee<0>(&removed_entry),
                        Invoke(old_sai_dash_trusted_vni_api, &sai_dash_trusted_vni_api_t::remove_global_trusted_vni_entry)));

            EXPECT_CALL(*mock_sai_dash_trusted_vni_api, create_global_trusted_vni_entry)
                .WillOnce(
                    DoAll(
                        SaveArgPointee<0>(&actual_entry),
                        Invoke(old_sai_dash_trusted_vni_api, &sai_dash_trusted_vni_api_t::create_global_trusted_vni_entry)));

            EXPECT_CALL(*mock_sai_dash_trusted_vni_api, remove_global_trusted_vni_entry)
                .WillOnce(
                    DoAll(
                        SaveArgPointee<0>(&removed_entry),
                        Invoke(old_sai_dash_trusted_vni_api, &sai_dash_trusted_vni_api_t::remove_global_trusted_vni_entry)));

            EXPECT_CALL(*mock_sai_dash_trusted_vni_api, create_global_trusted_vni_entry)
                .WillOnce(
                    DoAll(
                        SaveArgPointee<0>(&actual_entry),
                        Invoke(old_sai_dash_trusted_vni_api, &sai_dash_trusted_vni_api_t::create_global_trusted_vni_entry)));
        }

        for (int i = 0; i < 2; i++)
        {
            appliance.mutable_trusted_vnis()->CopyFrom(orig_vni);
            SetDashTable(APP_DASH_APPLIANCE_TABLE_NAME, appliance1, appliance);
            VerifyTrustedVniEntry(removed_entry.vni_range, changed_vni);
            VerifyTrustedVniEntry(actual_entry.vni_range, orig_vni);

            appliance.mutable_trusted_vnis()->CopyFrom(changed_vni);
            SetDashTable(APP_DASH_APPLIANCE_TABLE_NAME, appliance1, appliance);
            VerifyTrustedVniEntry(removed_entry.vni_range, orig_vni);
            VerifyTrustedVniEntry(actual_entry.vni_range, changed_vni);
        }
    }

    INSTANTIATE_TEST_SUITE_P(
        DashOrchChangeTrustedVniTest,
        DashOrchTest,
        ::testing::Combine(
            ::testing::Values(vni_value1, vni_range1),
            ::testing::Values(vni_value2, vni_range2)),
        [](const testing::TestParamInfo<DashOrchTest::ParamType> &info) {
            const auto &vni1 = std::get<0>(info.param);
            const auto &vni2 = std::get<1>(info.param);
            return "EniTrustedVni_" + GetVniString(vni1) + "_to_" + GetVniString(vni2);
        });
}