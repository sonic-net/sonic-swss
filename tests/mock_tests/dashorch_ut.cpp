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
    class MockDashHaOrch : public DashHaOrch
    {
    public:
        MockDashHaOrch(DBConnector *db, const std::vector<std::string> &tableNames, DashOrch *dash_orch, BfdOrch *bfd_orch, DBConnector *app_state_db, ZmqServer *zmqServer)
            : DashHaOrch(db, tableNames, dash_orch, bfd_orch, app_state_db, zmqServer), m_ha_role_for_eni(dash::types::HA_ROLE_ACTIVE) {}

        void setHaRoleForEni(dash::types::HaRole role) { m_ha_role_for_eni = role; }

        HaScopeEntry getHaScopeForEni(const std::string& eni) override
        {
            HaScopeEntry entry;

            entry.ha_scope_id = 0x123456789ABCDEF0ULL;
            entry.metadata.set_ha_role(m_ha_role_for_eni);
            entry.metadata.set_disabled(false);

            return entry;
        }

    private:
        dash::types::HaRole m_ha_role_for_eni;
    };

    class TestableDashOrch : public DashOrch
    {
    public:
        TestableDashOrch(swss::DBConnector* db, std::vector<std::string>& tables, swss::DBConnector* app_state_db, swss::ZmqServer* zmqServer)
            : DashOrch(db, tables, app_state_db, zmqServer) {}
    };

    DEFINE_SAI_GENERIC_APIS_MOCK(dash_appliance, dash_appliance)
    DEFINE_SAI_API_COMBINED_MOCK(dash_eni, eni, eni_ether_address_map)
    DEFINE_SAI_ENTRY_APIS_MOCK(dash_vip, vip)
    DEFINE_SAI_ENTRY_APIS_MOCK(dash_trusted_vni, global_trusted_vni, eni_trusted_vni)
    DEFINE_SAI_ENTRY_APIS_MOCK(dash_direction_lookup, direction_lookup)
    using namespace mock_orch_test;
    using ::testing::DoAll;
    using ::testing::Return;
    using ::testing::SetArgPointee;
    using ::testing::SaveArg;
    using ::testing::SaveArgPointee;
    using ::testing::Invoke;
    using ::testing::InSequence;
    using ::testing::Throw;
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
        if (vni.has_value()) {
            return std::to_string(vni.value());
        } else if (vni.has_range()) {
            return std::to_string(vni.range().min()) + "_" + std::to_string(vni.range().max());
        } else {
            return "Invalid VNI";
        }
    }
    class DashOrchTest : public MockDashOrchTest, public ::testing::WithParamInterface<std::tuple<ValueOrRange, ValueOrRange>> {
    protected:
        std::unique_ptr<MockDashHaOrch> m_mock_dash_ha_orch;

    private:
        void ApplySaiMock()
        {
            INIT_SAI_API_MOCK(dash_appliance);
            INIT_SAI_API_MOCK(dash_eni);
            INIT_SAI_API_MOCK(dash_vip);
            INIT_SAI_API_MOCK(dash_trusted_vni);
            INIT_SAI_API_MOCK(dash_direction_lookup);
            MockSaiApis();
        }

        void PostSetUp()
        {
            // Mock is not created here so tests that only need DashOrch (e.g. trusted VNI tests) are not
            // affected by the dummy getHaScopeForEni(). The two tests that need the mock create it locally.
        }

        void PreTearDown() override
        {
            RestoreSaiApis();
            DEINIT_SAI_API_MOCK(dash_direction_lookup);
            DEINIT_SAI_API_MOCK(dash_trusted_vni);
            DEINIT_SAI_API_MOCK(dash_vip);
            DEINIT_SAI_API_MOCK(dash_eni);
            DEINIT_SAI_API_MOCK(dash_appliance);
        }

        public:
            void VerifyTrustedVniEntry(sai_u32_range_t &actual_entry, const ValueOrRange &expected_vni)
            {
                if (expected_vni.has_value()) {
                    EXPECT_EQ(actual_entry.min, expected_vni.value());
                    EXPECT_EQ(actual_entry.max, expected_vni.value());
                } else if (expected_vni.has_range()) {
                    EXPECT_EQ(actual_entry.min, expected_vni.range().min());
                    EXPECT_EQ(actual_entry.max, expected_vni.range().max());
                } else {
                    FAIL() << "Invalid ValueOrRange provided";
                }
            }
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
            void VerifyDirectionLookup(std::vector<sai_attribute_t> &actual_attrs, sai_direction_lookup_entry_action_t expected_lookup)
            {
                for (auto attr : actual_attrs) {
                    if (attr.id == SAI_DIRECTION_LOOKUP_ENTRY_ATTR_ACTION) {
                        EXPECT_EQ(attr.value.u32, expected_lookup);
                        return;
                    }
                }
                FAIL() << "SAI_DIRECTION_LOOKUP_ENTRY_ATTR_ACTION not found in attributes";
            }
            void VerifyNoAttribute(std::vector<sai_attribute_t> &actual_attrs, sai_object_id_t attr_id)
            {
                for (auto attr : actual_attrs) {
                    if (attr.id == attr_id) {
                        FAIL() << "Unexpected attribute found in attributes";
                    }
                }
                return ;
            }
            void VerifyHaFlowOwner(std::vector<sai_attribute_t> &actual_attrs, bool expected_value)
            {
                for (auto attr : actual_attrs) {
                    if (attr.id == SAI_ENI_ATTR_IS_HA_FLOW_OWNER) {
                        EXPECT_EQ(attr.value.booldata, expected_value);
                        return;
                    }
                }
                FAIL() << "SAI_ENI_ATTR_IS_HA_FLOW_OWNER not found in attributes";
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
        std::vector<sai_attribute_t> actual_attrs;

        dash::eni::Eni eni = BuildEniEntry();
        
        EXPECT_CALL(*mock_sai_dash_eni_api, create_eni).Times(3)
            .WillRepeatedly(
                DoAll(
                    [&actual_attrs](sai_object_id_t *eni_id, sai_object_id_t switch_id, uint32_t attr_count, const sai_attribute_t *attr_list) {
                        actual_attrs.assign(attr_list, attr_list + attr_count);
                    },
                    Invoke(old_sai_dash_eni_api, &sai_dash_eni_api_t::create_eni) // Call the original function
                )
            );

        SetDashTable(APP_DASH_ENI_TABLE_NAME, "eni1", eni);
        VerifyEniMode(actual_attrs, SAI_DASH_ENI_MODE_VM);
        SetDashTable(APP_DASH_ENI_TABLE_NAME, "eni1", eni, false);

        eni.set_eni_mode(dash::eni::MODE_FNIC);
        SetDashTable(APP_DASH_ENI_TABLE_NAME, "eni1", eni);
        VerifyEniMode(actual_attrs, SAI_DASH_ENI_MODE_FNIC);
        SetDashTable(APP_DASH_ENI_TABLE_NAME, "eni1", eni, false);

        eni.set_eni_mode(dash::eni::MODE_UNSPECIFIED);
        SetDashTable(APP_DASH_ENI_TABLE_NAME, "eni1", eni);
        VerifyEniMode(actual_attrs, SAI_DASH_ENI_MODE_VM); // Default
        SetDashTable(APP_DASH_ENI_TABLE_NAME, "eni1", eni, false);
    }

    TEST_F(DashOrchTest, RemoveNonExistentEni)
    {
        CreateApplianceEntry();
        CreateVnet();
        EXPECT_CALL(*mock_sai_dash_eni_api, remove_eni).Times(0);
        SetDashTable(APP_DASH_ENI_TABLE_NAME, eni1, dash::eni::Eni(), false, true);
    }

    TEST_F(DashOrchTest, RemoveNonExistentAppliance)
    {
        EXPECT_CALL(*mock_sai_dash_appliance_api, remove_dash_appliance).Times(0);
        SetDashTable(APP_DASH_APPLIANCE_TABLE_NAME, appliance1, dash::appliance::Appliance(), false, true);
    }

    TEST_F(DashOrchTest, CreateRemoveApplianceTrustedVnisSingleValue)
    {
        int trusted_vni = 100;
        dash::appliance::Appliance appliance = BuildApplianceEntry();
        appliance.mutable_trusted_vnis_list()->Add()->set_value(trusted_vni);

        sai_global_trusted_vni_entry_t actual_entry;
        sai_global_trusted_vni_entry_t removed_entry;

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

        SetDashTable(APP_DASH_APPLIANCE_TABLE_NAME, appliance1, appliance);
        EXPECT_EQ(actual_entry.vni_range.min, trusted_vni);
        EXPECT_EQ(actual_entry.vni_range.max, trusted_vni);

        SetDashTable(APP_DASH_APPLIANCE_TABLE_NAME, appliance1, dash::appliance::Appliance(), false);
        EXPECT_EQ(removed_entry.vni_range.min, trusted_vni);
        EXPECT_EQ(removed_entry.vni_range.max, trusted_vni);
    }

    TEST_F(DashOrchTest, CreateRemoveApplianceTrustedVnisSingleRange)
    {
        int min_trusted_vni = 500;
        int max_trusted_vni = 600;
        dash::appliance::Appliance appliance = BuildApplianceEntry();
        appliance.mutable_trusted_vnis_list()->Add()->CopyFrom(GenVni(min_trusted_vni, max_trusted_vni));

        sai_global_trusted_vni_entry_t actual_entry;
        sai_global_trusted_vni_entry_t removed_entry;

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

        SetDashTable(APP_DASH_APPLIANCE_TABLE_NAME, appliance1, appliance);
        EXPECT_EQ(actual_entry.vni_range.min, min_trusted_vni);
        EXPECT_EQ(actual_entry.vni_range.max, max_trusted_vni);

        SetDashTable(APP_DASH_APPLIANCE_TABLE_NAME, appliance1, dash::appliance::Appliance(), false);
        EXPECT_EQ(removed_entry.vni_range.min, min_trusted_vni);
        EXPECT_EQ(removed_entry.vni_range.max, max_trusted_vni);
    }

    TEST_F(DashOrchTest, CreateRemoveApplianceTrustedVniCreateFail)
    {
        dash::appliance::Appliance appliance = BuildApplianceEntry();
        appliance.mutable_trusted_vnis_list()->Add()->set_value(100);

        {
            InSequence seq;
            EXPECT_CALL(*mock_sai_dash_appliance_api, create_dash_appliance).Times(1);
            EXPECT_CALL(*mock_sai_dash_trusted_vni_api, create_global_trusted_vni_entry)
                .WillOnce(Return(SAI_STATUS_FAILURE));

            EXPECT_CALL(*mock_sai_dash_trusted_vni_api, remove_global_trusted_vni_entry)
                .Times(0);
            EXPECT_CALL(*mock_sai_dash_appliance_api, remove_dash_appliance)
                .Times(1);
        }

        SetDashTable(APP_DASH_APPLIANCE_TABLE_NAME, appliance1, appliance, true, true);
    }

    TEST_F(DashOrchTest, CreateRemoveApplianceTrustedVniRemoveFail)
    {
        dash::appliance::Appliance appliance = BuildApplianceEntry();
        appliance.mutable_trusted_vnis_list()->Add()->set_value(100);
        EXPECT_CALL(*mock_sai_dash_appliance_api, remove_dash_appliance).Times(0);
        {
            InSequence seq;
            EXPECT_CALL(*mock_sai_dash_appliance_api, create_dash_appliance).Times(1);

            EXPECT_CALL(*mock_sai_dash_trusted_vni_api, create_global_trusted_vni_entry)
                .Times(1);

            EXPECT_CALL(*mock_sai_dash_trusted_vni_api, remove_global_trusted_vni_entry)
                .WillOnce(Return(SAI_STATUS_FAILURE));

        }

        SetDashTable(APP_DASH_APPLIANCE_TABLE_NAME, appliance1, appliance);
        SetDashTable(APP_DASH_APPLIANCE_TABLE_NAME, appliance1, dash::appliance::Appliance(), false, true);
    }

    TEST_F(DashOrchTest, CreateRemoveApplianceTrustedVnisMixed)
    {
        int vni1 = 700;
        int vni2_min = 800;
        int vni2_max = 810;
        int vni3 = 900;
        int vni4_min = 1000;
        int vni4_max = 1100;
        dash::appliance::Appliance appliance = BuildApplianceEntry();
        appliance.mutable_trusted_vnis_list()->Add()->set_value(vni1);
        appliance.mutable_trusted_vnis_list()->Add()->CopyFrom(GenVni(vni2_min, vni2_max));
        appliance.mutable_trusted_vnis_list()->Add()->set_value(vni3);
        appliance.mutable_trusted_vnis_list()->Add()->CopyFrom(GenVni(vni4_min, vni4_max));

        std::vector<sai_global_trusted_vni_entry_t> created_entries(4);
        std::vector<sai_global_trusted_vni_entry_t> removed_entries(4);

        {
            InSequence seq;

            EXPECT_CALL(*mock_sai_dash_trusted_vni_api, create_global_trusted_vni_entry)
                .WillOnce(
                    DoAll(
                        SaveArgPointee<0>(&created_entries[0]),
                        Invoke(old_sai_dash_trusted_vni_api, &sai_dash_trusted_vni_api_t::create_global_trusted_vni_entry)));

            EXPECT_CALL(*mock_sai_dash_trusted_vni_api, create_global_trusted_vni_entry)
                .WillOnce(
                    DoAll(
                        SaveArgPointee<0>(&created_entries[1]),
                        Invoke(old_sai_dash_trusted_vni_api, &sai_dash_trusted_vni_api_t::create_global_trusted_vni_entry)));

            EXPECT_CALL(*mock_sai_dash_trusted_vni_api, create_global_trusted_vni_entry)
                .WillOnce(
                    DoAll(
                        SaveArgPointee<0>(&created_entries[2]),
                        Invoke(old_sai_dash_trusted_vni_api, &sai_dash_trusted_vni_api_t::create_global_trusted_vni_entry)));

            EXPECT_CALL(*mock_sai_dash_trusted_vni_api, create_global_trusted_vni_entry)
                .WillOnce(
                    DoAll(
                        SaveArgPointee<0>(&created_entries[3]),
                        Invoke(old_sai_dash_trusted_vni_api, &sai_dash_trusted_vni_api_t::create_global_trusted_vni_entry)));

            // orchagent removes trusted VNIs in reverse order so we set the expectation in reverse order as well
            EXPECT_CALL(*mock_sai_dash_trusted_vni_api, remove_global_trusted_vni_entry)
                .WillOnce(
                    DoAll(
                        SaveArgPointee<0>(&removed_entries[3]),
                        Invoke(old_sai_dash_trusted_vni_api, &sai_dash_trusted_vni_api_t::remove_global_trusted_vni_entry)));

            EXPECT_CALL(*mock_sai_dash_trusted_vni_api, remove_global_trusted_vni_entry)
                .WillOnce(
                    DoAll(
                        SaveArgPointee<0>(&removed_entries[2]),
                        Invoke(old_sai_dash_trusted_vni_api, &sai_dash_trusted_vni_api_t::remove_global_trusted_vni_entry)));

            EXPECT_CALL(*mock_sai_dash_trusted_vni_api, remove_global_trusted_vni_entry)
                .WillOnce(
                    DoAll(
                        SaveArgPointee<0>(&removed_entries[1]),
                        Invoke(old_sai_dash_trusted_vni_api, &sai_dash_trusted_vni_api_t::remove_global_trusted_vni_entry)));

            EXPECT_CALL(*mock_sai_dash_trusted_vni_api, remove_global_trusted_vni_entry)
                .WillOnce(
                    DoAll(
                        SaveArgPointee<0>(&removed_entries[0]),
                        Invoke(old_sai_dash_trusted_vni_api, &sai_dash_trusted_vni_api_t::remove_global_trusted_vni_entry)));
        }

        SetDashTable(APP_DASH_APPLIANCE_TABLE_NAME, appliance1, appliance);
        EXPECT_EQ(created_entries[0].vni_range.min, vni1);
        EXPECT_EQ(created_entries[0].vni_range.max, vni1);
        EXPECT_EQ(created_entries[1].vni_range.min, vni2_min);
        EXPECT_EQ(created_entries[1].vni_range.max, vni2_max);
        EXPECT_EQ(created_entries[2].vni_range.min, vni3);
        EXPECT_EQ(created_entries[2].vni_range.max, vni3);
        EXPECT_EQ(created_entries[3].vni_range.min, vni4_min);
        EXPECT_EQ(created_entries[3].vni_range.max, vni4_max);

        SetDashTable(APP_DASH_APPLIANCE_TABLE_NAME, appliance1, dash::appliance::Appliance(), false);
        EXPECT_EQ(removed_entries[0].vni_range.min, vni1);
        EXPECT_EQ(removed_entries[0].vni_range.max, vni1);
        EXPECT_EQ(removed_entries[1].vni_range.min, vni2_min);
        EXPECT_EQ(removed_entries[1].vni_range.max, vni2_max);
        EXPECT_EQ(removed_entries[2].vni_range.min, vni3);
        EXPECT_EQ(removed_entries[2].vni_range.max, vni3);
        EXPECT_EQ(removed_entries[3].vni_range.min, vni4_min);
        EXPECT_EQ(removed_entries[3].vni_range.max, vni4_max);
    }

    TEST_F(DashOrchTest, CreateRemoveEniTrustedVnisSingleValue)
    {
        CreateApplianceEntry();
        CreateVnet();

        int trusted_vni = 200;
        dash::eni::Eni eni = BuildEniEntry();
        eni.mutable_trusted_vnis_list()->Add()->set_value(trusted_vni);

        sai_eni_trusted_vni_entry_t actual_entry;
        sai_eni_trusted_vni_entry_t removed_entry;

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

        SetDashTable(APP_DASH_ENI_TABLE_NAME, eni1, eni);
        EXPECT_EQ(actual_entry.vni_range.min, trusted_vni);
        EXPECT_EQ(actual_entry.vni_range.max, trusted_vni);

        SetDashTable(APP_DASH_ENI_TABLE_NAME, eni1, dash::eni::Eni(), false);
        EXPECT_EQ(removed_entry.vni_range.min, trusted_vni);
        EXPECT_EQ(removed_entry.vni_range.max, trusted_vni);
    }

    TEST_F(DashOrchTest, CreateRemoveEniTrustedVnisSingleRange)
    {
        CreateApplianceEntry();
        CreateVnet();

        int min_trusted_vni = 700;
        int max_trusted_vni = 800;
        dash::eni::Eni eni = BuildEniEntry();
        eni.mutable_trusted_vnis_list()->Add()->CopyFrom(GenVni(min_trusted_vni, max_trusted_vni));

        sai_eni_trusted_vni_entry_t actual_entry;
        sai_eni_trusted_vni_entry_t removed_entry;

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

        SetDashTable(APP_DASH_ENI_TABLE_NAME, eni1, eni);
        EXPECT_EQ(actual_entry.vni_range.min, min_trusted_vni);
        EXPECT_EQ(actual_entry.vni_range.max, max_trusted_vni);

        SetDashTable(APP_DASH_ENI_TABLE_NAME, eni1, dash::eni::Eni(), false);
        EXPECT_EQ(removed_entry.vni_range.min, min_trusted_vni);
        EXPECT_EQ(removed_entry.vni_range.max, max_trusted_vni);
    }

    TEST_F(DashOrchTest, CreateRemoveEniTrustedVniCreateFail)
    {
        CreateApplianceEntry();
        CreateVnet();

        dash::eni::Eni eni = BuildEniEntry();
        eni.mutable_trusted_vnis_list()->Add()->set_value(200);
        EXPECT_CALL(*mock_sai_dash_trusted_vni_api, remove_eni_trusted_vni_entry).Times(0);

        {
            InSequence seq;
            EXPECT_CALL(*mock_sai_dash_eni_api, create_eni).Times(1);
            EXPECT_CALL(*mock_sai_dash_trusted_vni_api, create_eni_trusted_vni_entry)
                .WillOnce(Return(SAI_STATUS_FAILURE));

            EXPECT_CALL(*mock_sai_dash_eni_api, remove_eni)
                .Times(1);
        }

        SetDashTable(APP_DASH_ENI_TABLE_NAME, eni1, eni, true, true);
    }

    TEST_F(DashOrchTest, CreateRemoveEniTrustedVniRemoveFail)
    {
        CreateApplianceEntry();
        CreateVnet();

        dash::eni::Eni eni = BuildEniEntry();
        eni.mutable_trusted_vnis_list()->Add()->set_value(200);
        EXPECT_CALL(*mock_sai_dash_eni_api, remove_eni).Times(0);

        {
            InSequence seq;
            EXPECT_CALL(*mock_sai_dash_eni_api, create_eni).Times(1);

            EXPECT_CALL(*mock_sai_dash_trusted_vni_api, create_eni_trusted_vni_entry)
                .Times(1);

            EXPECT_CALL(*mock_sai_dash_trusted_vni_api, remove_eni_trusted_vni_entry)
                .WillOnce(Return(SAI_STATUS_FAILURE));

        }

        SetDashTable(APP_DASH_ENI_TABLE_NAME, eni1, eni);
        SetDashTable(APP_DASH_ENI_TABLE_NAME, eni1, dash::eni::Eni(), false, true);
    }

    TEST_F(DashOrchTest, CreateRemoveEniTrustedVnisMixed)
    {
        CreateApplianceEntry();
        CreateVnet();

        int vni1 = 900;
        int vni2_min = 1000;
        int vni2_max = 1100;
        int vni3 = 1200;
        int vni4_min = 1300;
        int vni4_max = 1400;
        dash::eni::Eni eni = BuildEniEntry();
        eni.mutable_trusted_vnis_list()->Add()->set_value(vni1);
        eni.mutable_trusted_vnis_list()->Add()->CopyFrom(GenVni(vni2_min, vni2_max));
        eni.mutable_trusted_vnis_list()->Add()->set_value(vni3);
        eni.mutable_trusted_vnis_list()->Add()->CopyFrom(GenVni(vni4_min, vni4_max));

        std::vector<sai_eni_trusted_vni_entry_t> created_entries(4);
        std::vector<sai_eni_trusted_vni_entry_t> removed_entries(4);

        {
            InSequence seq;

            EXPECT_CALL(*mock_sai_dash_trusted_vni_api, create_eni_trusted_vni_entry)
                .WillOnce(
                    DoAll(
                        SaveArgPointee<0>(&created_entries[0]),
                        Invoke(old_sai_dash_trusted_vni_api, &sai_dash_trusted_vni_api_t::create_eni_trusted_vni_entry)));

            EXPECT_CALL(*mock_sai_dash_trusted_vni_api, create_eni_trusted_vni_entry)
                .WillOnce(
                    DoAll(
                        SaveArgPointee<0>(&created_entries[1]),
                        Invoke(old_sai_dash_trusted_vni_api, &sai_dash_trusted_vni_api_t::create_eni_trusted_vni_entry)));

            EXPECT_CALL(*mock_sai_dash_trusted_vni_api, create_eni_trusted_vni_entry)
                .WillOnce(
                    DoAll(
                        SaveArgPointee<0>(&created_entries[2]),
                        Invoke(old_sai_dash_trusted_vni_api, &sai_dash_trusted_vni_api_t::create_eni_trusted_vni_entry)));

            EXPECT_CALL(*mock_sai_dash_trusted_vni_api, create_eni_trusted_vni_entry)
                .WillOnce(
                    DoAll(
                        SaveArgPointee<0>(&created_entries[3]),
                        Invoke(old_sai_dash_trusted_vni_api, &sai_dash_trusted_vni_api_t::create_eni_trusted_vni_entry)));

            // orchagent removes trusted VNIs in reverse order so we set the expectation in reverse order as well
            EXPECT_CALL(*mock_sai_dash_trusted_vni_api, remove_eni_trusted_vni_entry)
                .WillOnce(
                    DoAll(
                        SaveArgPointee<0>(&removed_entries[3]),
                        Invoke(old_sai_dash_trusted_vni_api, &sai_dash_trusted_vni_api_t::remove_eni_trusted_vni_entry)));

            EXPECT_CALL(*mock_sai_dash_trusted_vni_api, remove_eni_trusted_vni_entry)
                .WillOnce(
                    DoAll(
                        SaveArgPointee<0>(&removed_entries[2]),
                        Invoke(old_sai_dash_trusted_vni_api, &sai_dash_trusted_vni_api_t::remove_eni_trusted_vni_entry)));

            EXPECT_CALL(*mock_sai_dash_trusted_vni_api, remove_eni_trusted_vni_entry)
                .WillOnce(
                    DoAll(
                        SaveArgPointee<0>(&removed_entries[1]),
                        Invoke(old_sai_dash_trusted_vni_api, &sai_dash_trusted_vni_api_t::remove_eni_trusted_vni_entry)));

            EXPECT_CALL(*mock_sai_dash_trusted_vni_api, remove_eni_trusted_vni_entry)
                .WillOnce(
                    DoAll(
                        SaveArgPointee<0>(&removed_entries[0]),
                        Invoke(old_sai_dash_trusted_vni_api, &sai_dash_trusted_vni_api_t::remove_eni_trusted_vni_entry)));
        }

        SetDashTable(APP_DASH_ENI_TABLE_NAME, eni1, eni);
        EXPECT_EQ(created_entries[0].vni_range.min, vni1);
        EXPECT_EQ(created_entries[0].vni_range.max, vni1);
        EXPECT_EQ(created_entries[1].vni_range.min, vni2_min);
        EXPECT_EQ(created_entries[1].vni_range.max, vni2_max);
        EXPECT_EQ(created_entries[2].vni_range.min, vni3);
        EXPECT_EQ(created_entries[2].vni_range.max, vni3);
        EXPECT_EQ(created_entries[3].vni_range.min, vni4_min);
        EXPECT_EQ(created_entries[3].vni_range.max, vni4_max);

        SetDashTable(APP_DASH_ENI_TABLE_NAME, eni1, dash::eni::Eni(), false);
        EXPECT_EQ(removed_entries[0].vni_range.min, vni1);
        EXPECT_EQ(removed_entries[0].vni_range.max, vni1);
        EXPECT_EQ(removed_entries[1].vni_range.min, vni2_min);
        EXPECT_EQ(removed_entries[1].vni_range.max, vni2_max);
        EXPECT_EQ(removed_entries[2].vni_range.min, vni3);
        EXPECT_EQ(removed_entries[2].vni_range.max, vni3);
        EXPECT_EQ(removed_entries[3].vni_range.min, vni4_min);
        EXPECT_EQ(removed_entries[3].vni_range.max, vni4_max);
    }

    TEST_F(DashOrchTest, DuplicateSetEniTrustedVniSingle)
    {
        CreateApplianceEntry();
        CreateVnet();

        int trusted_vni = 300;
        dash::eni::Eni eni = BuildEniEntry();
        eni.mutable_trusted_vnis_list()->Add()->set_value(trusted_vni);

        EXPECT_CALL(*mock_sai_dash_trusted_vni_api, create_eni_trusted_vni_entry).Times(1);

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
        dash::types::ValueOrRange *vni_range_pb = eni.mutable_trusted_vnis_list()->Add();
        vni_range_pb->mutable_range()->set_min(min_trusted_vni);
        vni_range_pb->mutable_range()->set_max(max_trusted_vni);

        EXPECT_CALL(*mock_sai_dash_trusted_vni_api, create_eni_trusted_vni_entry).Times(1);

        SetDashTable(APP_DASH_ENI_TABLE_NAME, eni1, eni);
        SetDashTable(APP_DASH_ENI_TABLE_NAME, eni1, eni);
    }

    TEST_F(DashOrchTest, SetApplianceOutboundLookup)
    {
        dash::appliance::Appliance appliance = BuildApplianceEntry();
        appliance.set_outbound_direction_lookup("dst_mac");

        std::vector<sai_attribute_t> actual_attrs;
        actual_attrs.clear();

        EXPECT_CALL(*mock_sai_dash_direction_lookup_api, create_direction_lookup_entry).Times(2)
            .WillRepeatedly(
                DoAll(
                    [&actual_attrs](const sai_direction_lookup_entry_t *entry, uint32_t count, const sai_attribute_t *attr_list) {
                        actual_attrs.assign(attr_list, attr_list + count);
                    },
                    Invoke(old_sai_dash_direction_lookup_api, &sai_dash_direction_lookup_api_t::create_direction_lookup_entry) // Call the original function
                )
            );

        SetDashTable(APP_DASH_APPLIANCE_TABLE_NAME, appliance1, appliance);
        VerifyDirectionLookup(actual_attrs, SAI_DIRECTION_LOOKUP_ENTRY_ACTION_SET_INBOUND_DIRECTION);
        VerifyNoAttribute(actual_attrs, SAI_DIRECTION_LOOKUP_ENTRY_ATTR_DASH_ENI_MAC_OVERRIDE_TYPE);
        actual_attrs.clear();
        
        SetDashTable(APP_DASH_APPLIANCE_TABLE_NAME, appliance1, dash::appliance::Appliance(), false);
        appliance.set_outbound_direction_lookup("src_mac");
        SetDashTable(APP_DASH_APPLIANCE_TABLE_NAME, appliance1, appliance);
        VerifyDirectionLookup(actual_attrs, SAI_DIRECTION_LOOKUP_ENTRY_ACTION_SET_OUTBOUND_DIRECTION);
    }

    TEST_F(DashOrchTest, CreateEniWithHaScopeStandbyRole)
    {
        m_mock_dash_ha_orch = std::make_unique<MockDashHaOrch>(m_dpu_app_db.get(), std::vector<std::string>{APP_DASH_HA_SET_TABLE_NAME, APP_DASH_HA_SCOPE_TABLE_NAME}, m_DashOrch, nullptr, m_dpu_app_state_db.get(), nullptr);
        m_DashOrch->setDashHaOrch(m_mock_dash_ha_orch.get());

        CreateApplianceEntry();
        CreateVnet();
        m_mock_dash_ha_orch->setHaRoleForEni(dash::types::HA_ROLE_STANDBY);

        std::vector<sai_attribute_t> actual_attrs;
        EXPECT_CALL(*mock_sai_dash_eni_api, create_eni).Times(1).WillOnce(
            DoAll(
                [&actual_attrs](sai_object_id_t *eni_id, sai_object_id_t switch_id, uint32_t attr_count, const sai_attribute_t *attr_list) {
                    actual_attrs.assign(attr_list, attr_list + attr_count);
                },
                Invoke(old_sai_dash_eni_api, &sai_dash_eni_api_t::create_eni)));

        SetDashTable(APP_DASH_ENI_TABLE_NAME, eni1, BuildEniEntry());
        VerifyHaFlowOwner(actual_attrs, false);

        m_DashOrch->setDashHaOrch(nullptr);
    }

    TEST_F(DashOrchTest, CreateEniWithHaScopeOtherRole)
    {
        m_mock_dash_ha_orch = std::make_unique<MockDashHaOrch>(m_dpu_app_db.get(), std::vector<std::string>{APP_DASH_HA_SET_TABLE_NAME, APP_DASH_HA_SCOPE_TABLE_NAME}, m_DashOrch, nullptr, m_dpu_app_state_db.get(), nullptr);
        m_DashOrch->setDashHaOrch(m_mock_dash_ha_orch.get());

        CreateApplianceEntry();
        CreateVnet();
        m_mock_dash_ha_orch->setHaRoleForEni(dash::types::HA_ROLE_SWITCHING_TO_ACTIVE);

        std::vector<sai_attribute_t> actual_attrs;
        EXPECT_CALL(*mock_sai_dash_eni_api, create_eni).Times(1).WillOnce(
            DoAll(
                [&actual_attrs](sai_object_id_t *eni_id, sai_object_id_t switch_id, uint32_t attr_count, const sai_attribute_t *attr_list) {
                    actual_attrs.assign(attr_list, attr_list + attr_count);
                },
                Invoke(old_sai_dash_eni_api, &sai_dash_eni_api_t::create_eni)));

        SetDashTable(APP_DASH_ENI_TABLE_NAME, eni1, BuildEniEntry());
        VerifyHaFlowOwner(actual_attrs, false);

        m_DashOrch->setDashHaOrch(nullptr);
    }

    TEST_F(DashOrchTest, CreateEniMissingVnetNotRetried)
    {
        CreateApplianceEntry();
        // Build ENI referencing a VNET that doesn't exist
        dash::eni::Eni eni = BuildEniEntry();
        eni.set_vnet("NON_EXISTENT_VNET");
        EXPECT_CALL(*mock_sai_dash_eni_api, create_eni).Times(0);
        SetDashTable(APP_DASH_ENI_TABLE_NAME, eni1, eni, true, true);
    }

    TEST_F(DashOrchTest, CreateEniMissingApplianceNotRetried)
    {
        // Do NOT create appliance — ENI requires appliance to exist
        CreateVnet();
        EXPECT_CALL(*mock_sai_dash_eni_api, create_eni).Times(0);
        SetDashTable(APP_DASH_ENI_TABLE_NAME, eni1, BuildEniEntry(), true, true);
    }

    TEST_F(DashOrchTest, CreateEniSaiFailureNotRetried)
    {
        CreateApplianceEntry();
        CreateVnet();
        EXPECT_CALL(*mock_sai_dash_eni_api, create_eni)
            .WillOnce(Return(SAI_STATUS_INSUFFICIENT_RESOURCES));
        SetDashTable(APP_DASH_ENI_TABLE_NAME, eni1, BuildEniEntry(), true, true);
    }

    TEST_F(DashOrchTest, EniRouteMissingEniNotRetried)
    {
        CreateApplianceEntry();
        // Do NOT create ENI — ENI route references eni1 which doesn't exist
        dash::eni_route::EniRoute eni_route;
        eni_route.set_group_id(route_group1);
        SetDashTable(APP_DASH_ENI_ROUTE_TABLE_NAME, eni1, eni_route, true, true);
    }

    TEST_F(DashOrchTest, EniRouteMissingRouteGroupNotRetried)
    {
        CreateApplianceEntry();
        CreateVnet();
        auto eni = BuildEniEntry();
        SetDashTable(APP_DASH_ENI_TABLE_NAME, eni1, eni);
        // Do NOT create route group — ENI route references route_group1 which doesn't exist
        dash::eni_route::EniRoute eni_route;
        eni_route.set_group_id(route_group1);
        SetDashTable(APP_DASH_ENI_ROUTE_TABLE_NAME, eni1, eni_route, true, true);
    }

    TEST_F(DashOrchTest, ApplianceTrustedVniFailCleanupAllowsRetry)
    {
        dash::appliance::Appliance appliance = BuildApplianceEntry();
        appliance.mutable_trusted_vnis_list()->Add()->set_value(100);

        {
            InSequence seq;
            // First attempt: appliance created, VNI fails, appliance removed
            EXPECT_CALL(*mock_sai_dash_appliance_api, create_dash_appliance).Times(1);
            EXPECT_CALL(*mock_sai_dash_trusted_vni_api, create_global_trusted_vni_entry)
                .WillOnce(Return(SAI_STATUS_FAILURE));
            EXPECT_CALL(*mock_sai_dash_appliance_api, remove_dash_appliance).Times(1);
            // Second attempt: all SAI calls re-issued (cache was cleared)
            EXPECT_CALL(*mock_sai_dash_appliance_api, create_dash_appliance).Times(1);
            EXPECT_CALL(*mock_sai_dash_trusted_vni_api, create_global_trusted_vni_entry).Times(1);
        }

        // First attempt fails
        SetDashTable(APP_DASH_APPLIANCE_TABLE_NAME, appliance1, appliance, true, true);
        // Second attempt succeeds — verifies cache was not populated by failed first attempt
        SetDashTable(APP_DASH_APPLIANCE_TABLE_NAME, appliance1, appliance, true, true);
    }

    TEST_F(DashOrchTest, EniTrustedVniFailCleanupAllowsRetry)
    {
        CreateApplianceEntry();
        CreateVnet();

        dash::eni::Eni eni = BuildEniEntry();
        eni.mutable_trusted_vnis_list()->Add()->set_value(200);

        {
            InSequence seq;
            // First attempt: ENI created, VNI fails, ENI removed
            EXPECT_CALL(*mock_sai_dash_eni_api, create_eni).Times(1);
            EXPECT_CALL(*mock_sai_dash_trusted_vni_api, create_eni_trusted_vni_entry)
                .WillOnce(Return(SAI_STATUS_FAILURE));
            EXPECT_CALL(*mock_sai_dash_eni_api, remove_eni).Times(1);
            // Second attempt: all SAI calls re-issued (cache was cleared)
            EXPECT_CALL(*mock_sai_dash_eni_api, create_eni).Times(1);
            EXPECT_CALL(*mock_sai_dash_trusted_vni_api, create_eni_trusted_vni_entry).Times(1);
        }

        // First attempt fails
        SetDashTable(APP_DASH_ENI_TABLE_NAME, eni1, eni, true, true);
        // Second attempt succeeds — verifies cache was not populated by failed first attempt
        SetDashTable(APP_DASH_ENI_TABLE_NAME, eni1, eni, true, true);
    }

    TEST_F(DashOrchTest, AddRemoveEniRoute)
    {
        CreateApplianceEntry();
        CreateVnet();
        auto eni = BuildEniEntry();
        SetDashTable(APP_DASH_ENI_TABLE_NAME, eni1, eni);
        AddOutboundRoutingGroup();

        // SET ENI route — binds ENI to route group
        dash::eni_route::EniRoute eni_route;
        eni_route.set_group_id(route_group1);
        SetDashTable(APP_DASH_ENI_ROUTE_TABLE_NAME, eni1, eni_route, true, true);

        // DEL ENI route — unbinds
        SetDashTable(APP_DASH_ENI_ROUTE_TABLE_NAME, eni1, dash::eni_route::EniRoute(), false, true);
    }

    TEST_F(DashOrchTest, AddRemoveTunnel)
    {
        CreateApplianceEntry();
        AddTunnel();
        RemoveTunnel();
    }

    TEST_F(DashOrchTest, RemoveEniPartialFailurePreservesCache)
    {
        CreateApplianceEntry();
        CreateVnet();
        auto eni = BuildEniEntry();
        SetDashTable(APP_DASH_ENI_TABLE_NAME, eni1, eni);

        {
            InSequence seq;
            // First remove attempt: remove_eni fails
            EXPECT_CALL(*mock_sai_dash_eni_api, remove_eni)
                .WillOnce(Return(SAI_STATUS_OBJECT_IN_USE));
            // Second remove attempt: remove_eni succeeds (cache was preserved)
            EXPECT_CALL(*mock_sai_dash_eni_api, remove_eni).Times(1);
        }

        // First attempt — SAI failure, consumer still emptied but cache preserved
        SetDashTable(APP_DASH_ENI_TABLE_NAME, eni1, dash::eni::Eni(), false, true);
        // Second attempt — succeeds because cache was preserved, so remove_eni is called again
        SetDashTable(APP_DASH_ENI_TABLE_NAME, eni1, dash::eni::Eni(), false, true);
    }

    TEST_F(DashOrchTest, RemoveApplianceDirectionLookupFailurePreservesCache)
    {
        CreateApplianceEntry();

        {
            InSequence seq;
            // First remove attempt: direction_lookup remove fails
            EXPECT_CALL(*mock_sai_dash_direction_lookup_api, remove_direction_lookup_entry)
                .WillOnce(Return(SAI_STATUS_FAILURE));
            // Second remove attempt: direction_lookup and appliance removes succeed
            EXPECT_CALL(*mock_sai_dash_direction_lookup_api, remove_direction_lookup_entry).Times(1);
            EXPECT_CALL(*mock_sai_dash_appliance_api, remove_dash_appliance).Times(1);
        }

        // First attempt — direction lookup removal fails, cache preserved
        SetDashTable(APP_DASH_APPLIANCE_TABLE_NAME, appliance1, dash::appliance::Appliance(), false, true);
        // Second attempt — succeeds, all SAI remove calls re-issued
        SetDashTable(APP_DASH_APPLIANCE_TABLE_NAME, appliance1, dash::appliance::Appliance(), false, true);
    }

    TEST_F(DashOrchTest, RemoveApplianceTrustedVniFailurePreservesCache)
    {
        dash::appliance::Appliance appliance = BuildApplianceEntry();
        appliance.mutable_trusted_vnis_list()->Add()->set_value(100);
        SetDashTable(APP_DASH_APPLIANCE_TABLE_NAME, appliance1, appliance);

        {
            InSequence seq;
            // First remove attempt: trusted VNI removal fails
            EXPECT_CALL(*mock_sai_dash_trusted_vni_api, remove_global_trusted_vni_entry)
                .WillOnce(Return(SAI_STATUS_FAILURE));
            // Second remove attempt: all removals succeed
            EXPECT_CALL(*mock_sai_dash_trusted_vni_api, remove_global_trusted_vni_entry).Times(1);
            EXPECT_CALL(*mock_sai_dash_appliance_api, remove_dash_appliance).Times(1);
        }

        // First attempt — VNI removal fails, cache preserved
        SetDashTable(APP_DASH_APPLIANCE_TABLE_NAME, appliance1, dash::appliance::Appliance(), false, true);
        // Second attempt — succeeds
        SetDashTable(APP_DASH_APPLIANCE_TABLE_NAME, appliance1, dash::appliance::Appliance(), false, true);
    }

    TEST_F(DashOrchTest, MissingProtobufAppliance)
    {
        // SET with no pb field — should be consumed without creating anything
        EXPECT_CALL(*mock_sai_dash_appliance_api, create_dash_appliance).Times(0);
        SetDashTableRaw(APP_DASH_APPLIANCE_TABLE_NAME, appliance1, {}, true, true);
    }

    TEST_F(DashOrchTest, InvalidProtobufAppliance)
    {
        // SET with garbage pb field — should be consumed without creating anything
        EXPECT_CALL(*mock_sai_dash_appliance_api, create_dash_appliance).Times(0);
        SetDashTableRaw(APP_DASH_APPLIANCE_TABLE_NAME, appliance1, {{ "pb", "not_valid_protobuf" }}, true, true);
    }

    TEST_F(DashOrchTest, MissingProtobufEni)
    {
        CreateApplianceEntry();
        CreateVnet();
        EXPECT_CALL(*mock_sai_dash_eni_api, create_eni).Times(0);
        SetDashTableRaw(APP_DASH_ENI_TABLE_NAME, eni1, {}, true, true);
    }

    TEST_F(DashOrchTest, MissingProtobufEniRoute)
    {
        CreateApplianceEntry();
        CreateVnet();
        SetDashTableRaw(APP_DASH_ENI_ROUTE_TABLE_NAME, eni1, {}, true, true);
    }

    TEST_F(DashOrchTest, InvalidRoutingTypeKey)
    {
        // Invalid routing type string that cannot be parsed
        SetDashTableRaw(APP_DASH_ROUTING_TYPE_TABLE_NAME, "INVALID_NOT_A_REAL_TYPE",
                        {{ "pb", dash::route_type::RouteType().SerializeAsString() }}, true, true);
    }

    TEST_F(DashOrchTest, EniCreateDeleteChurn)
    {
        CreateApplianceEntry();
        CreateVnet();
        auto eni = BuildEniEntry();

        // Cycle ENI create/delete multiple times
        for (int i = 0; i < 3; i++)
        {
            EXPECT_CALL(*mock_sai_dash_eni_api, create_eni).Times(1);
            SetDashTable(APP_DASH_ENI_TABLE_NAME, eni1, eni);

            EXPECT_CALL(*mock_sai_dash_eni_api, remove_eni).Times(1);
            SetDashTable(APP_DASH_ENI_TABLE_NAME, eni1, dash::eni::Eni(), false, true);
        }
    }

    TEST_F(DashOrchTest, EniCreateFailThenSucceed)
    {
        CreateApplianceEntry();
        CreateVnet();
        auto eni = BuildEniEntry();

        {
            InSequence seq;
            // First create fails
            EXPECT_CALL(*mock_sai_dash_eni_api, create_eni)
                .WillOnce(Return(SAI_STATUS_INSUFFICIENT_RESOURCES));
            // Second create succeeds
            EXPECT_CALL(*mock_sai_dash_eni_api, create_eni).Times(1);
        }

        SetDashTable(APP_DASH_ENI_TABLE_NAME, eni1, eni, true, true);
        SetDashTable(APP_DASH_ENI_TABLE_NAME, eni1, eni, true, true);
    }

    TEST_F(DashOrchTest, MultipleEniCreateDelete)
    {
        CreateApplianceEntry();
        CreateVnet();
        auto eni = BuildEniEntry();
        std::string eni2 = "ENI_2";

        // Create two different ENIs
        EXPECT_CALL(*mock_sai_dash_eni_api, create_eni).Times(2);
        SetDashTable(APP_DASH_ENI_TABLE_NAME, eni1, eni);
        SetDashTable(APP_DASH_ENI_TABLE_NAME, eni2, eni);

        // Delete both
        EXPECT_CALL(*mock_sai_dash_eni_api, remove_eni).Times(2);
        SetDashTable(APP_DASH_ENI_TABLE_NAME, eni1, dash::eni::Eni(), false, true);
        SetDashTable(APP_DASH_ENI_TABLE_NAME, eni2, dash::eni::Eni(), false, true);
    }

    TEST_F(DashOrchTest, EniRouteBindUnbindChurn)
    {
        CreateApplianceEntry();
        CreateVnet();
        auto eni = BuildEniEntry();
        SetDashTable(APP_DASH_ENI_TABLE_NAME, eni1, eni);
        AddOutboundRoutingGroup();

        dash::eni_route::EniRoute eni_route;
        eni_route.set_group_id(route_group1);

        // Bind/unbind multiple times
        for (int i = 0; i < 3; i++)
        {
            SetDashTable(APP_DASH_ENI_ROUTE_TABLE_NAME, eni1, eni_route, true, true);
            SetDashTable(APP_DASH_ENI_ROUTE_TABLE_NAME, eni1, dash::eni_route::EniRoute(), false, true);
        }
    }

    TEST_F(DashOrchTest, ApplianceVipCreateFailCleansUpAppliance)
    {
        dash::appliance::Appliance appliance = BuildApplianceEntry();

        {
            InSequence seq;
            EXPECT_CALL(*mock_sai_dash_appliance_api, create_dash_appliance).Times(1);
            EXPECT_CALL(*mock_sai_dash_vip_api, create_vip_entry)
                .WillOnce(Return(SAI_STATUS_FAILURE));
            EXPECT_CALL(*mock_sai_dash_appliance_api, remove_dash_appliance).Times(1);
        }

        SetDashTable(APP_DASH_APPLIANCE_TABLE_NAME, appliance1, appliance, true, true);
        EXPECT_TRUE(m_DashOrch->appliance_entries_.empty());
    }

    TEST_F(DashOrchTest, ApplianceDirectionLookupFailCleansUpVipAndAppliance)
    {
        dash::appliance::Appliance appliance = BuildApplianceEntry();

        {
            InSequence seq;
            EXPECT_CALL(*mock_sai_dash_appliance_api, create_dash_appliance).Times(1);
            EXPECT_CALL(*mock_sai_dash_vip_api, create_vip_entry).Times(1);
            EXPECT_CALL(*mock_sai_dash_direction_lookup_api, create_direction_lookup_entry)
                .WillOnce(Return(SAI_STATUS_FAILURE));
            EXPECT_CALL(*mock_sai_dash_vip_api, remove_vip_entry).Times(1);
            EXPECT_CALL(*mock_sai_dash_appliance_api, remove_dash_appliance).Times(1);
        }

        SetDashTable(APP_DASH_APPLIANCE_TABLE_NAME, appliance1, appliance, true, true);
        EXPECT_TRUE(m_DashOrch->appliance_entries_.empty());
    }

    TEST_F(DashOrchTest, AddRemoveRoutingType)
    {
        dash::route_type::RouteType route_type;
        route_type.add_items()->set_action_type(dash::route_type::ACTION_TYPE_STATICENCAP);

        SetDashTable(APP_DASH_ROUTING_TYPE_TABLE_NAME, "VNET", route_type);
        ASSERT_EQ(m_DashOrch->routing_type_entries_.count(dash::route_type::ROUTING_TYPE_VNET), 1u);

        SetDashTable(APP_DASH_ROUTING_TYPE_TABLE_NAME, "VNET", dash::route_type::RouteType(), false);
        EXPECT_TRUE(m_DashOrch->routing_type_entries_.empty());
    }

    TEST_F(DashOrchTest, RemoveNonExistentRoutingType)
    {
        SetDashTable(APP_DASH_ROUTING_TYPE_TABLE_NAME, "VNET", dash::route_type::RouteType(), false);
        EXPECT_TRUE(m_DashOrch->routing_type_entries_.empty());
    }

    TEST_F(DashOrchTest, RoutingTypeMissingProtobuf)
    {
        SetDashTableRaw(APP_DASH_ROUTING_TYPE_TABLE_NAME, "VNET", {}, true, true);
        EXPECT_TRUE(m_DashOrch->routing_type_entries_.empty());
    }

    TEST_F(DashOrchTest, RoutingTypeUnknownOp)
    {
        auto consumer = make_unique<Consumer>(
            new swss::ConsumerStateTable(m_app_db.get(), APP_DASH_ROUTING_TYPE_TABLE_NAME),
            m_DashOrch, APP_DASH_ROUTING_TYPE_TABLE_NAME);
        consumer->addToSync(swss::KeyOpFieldsValuesTuple(
            "VNET", "UNKNOWN", {{"pb", dash::route_type::RouteType().SerializeAsString()}}));

        m_DashOrch->doTask(*consumer);
        EXPECT_EQ(consumer->m_toSync.begin(), consumer->m_toSync.end());
        EXPECT_TRUE(m_DashOrch->routing_type_entries_.empty());
    }

    TEST_F(DashOrchTest, AddRemoveQos)
    {
        dash::qos::Qos qos;
        qos.set_bw(100);
        qos.set_cps(200);
        qos.set_flows(300);

        SetDashTable(APP_DASH_QOS_TABLE_NAME, "QOS_1", qos);
        ASSERT_EQ(m_DashOrch->qos_entries_.count("QOS_1"), 1u);
        EXPECT_EQ(m_DashOrch->qos_entries_["QOS_1"].bw(), 100);

        SetDashTable(APP_DASH_QOS_TABLE_NAME, "QOS_1", dash::qos::Qos(), false);
        EXPECT_TRUE(m_DashOrch->qos_entries_.empty());
    }

    TEST_F(DashOrchTest, QosMissingProtobuf)
    {
        SetDashTableRaw(APP_DASH_QOS_TABLE_NAME, "QOS_1", {}, true, true);
        EXPECT_TRUE(m_DashOrch->qos_entries_.empty());
    }

    TEST_F(DashOrchTest, QosUnknownOp)
    {
        auto consumer = make_unique<Consumer>(
            new swss::ConsumerStateTable(m_app_db.get(), APP_DASH_QOS_TABLE_NAME),
            m_DashOrch, APP_DASH_QOS_TABLE_NAME);
        consumer->addToSync(swss::KeyOpFieldsValuesTuple(
            "QOS_1", "UNKNOWN", {{"pb", dash::qos::Qos().SerializeAsString()}}));

        m_DashOrch->doTask(*consumer);
        EXPECT_EQ(consumer->m_toSync.begin(), consumer->m_toSync.end());
        EXPECT_TRUE(m_DashOrch->qos_entries_.empty());
    }

    TEST_F(DashOrchTest, QosCreateDeleteChurn)
    {
        dash::qos::Qos qos;
        qos.set_bw(100);
        qos.set_cps(200);
        qos.set_flows(300);

        for (int i = 0; i < 3; i++)
        {
            SetDashTable(APP_DASH_QOS_TABLE_NAME, "QOS_1", qos);
            ASSERT_EQ(m_DashOrch->qos_entries_.count("QOS_1"), 1u);
            SetDashTable(APP_DASH_QOS_TABLE_NAME, "QOS_1", dash::qos::Qos(), false);
            EXPECT_TRUE(m_DashOrch->qos_entries_.empty());
        }
    }

    TEST_F(DashOrchTest, EniAdminStateUpdateSaiFailure)
    {
        CreateApplianceEntry();
        CreateVnet();
        auto eni = BuildEniEntry();
        SetDashTable(APP_DASH_ENI_TABLE_NAME, eni1, eni);

        eni.set_admin_state(dash::eni::STATE_DISABLED);
        EXPECT_CALL(*mock_sai_dash_eni_api, set_eni_attribute)
            .WillOnce(Return(SAI_STATUS_FAILURE));

        SetDashTable(APP_DASH_ENI_TABLE_NAME, eni1, eni, true, true);
        ASSERT_EQ(m_DashOrch->eni_entries_.count(eni1), 1u);
        EXPECT_EQ(m_DashOrch->eni_entries_[eni1].metadata.admin_state(), dash::eni::STATE_ENABLED);
    }

    TEST_F(DashOrchTest, EniMissingV6MeterPolicy)
    {
        CreateApplianceEntry();
        CreateVnet();

        auto eni = BuildEniEntry();
        eni.set_v6_meter_policy_id("MISSING_V6_POLICY");

        EXPECT_CALL(*mock_sai_dash_eni_api, create_eni).Times(0);
        SetDashTable(APP_DASH_ENI_TABLE_NAME, eni1, eni, true, true);
        EXPECT_TRUE(m_DashOrch->eni_entries_.empty());
    }

    TEST_F(DashOrchTest, EniAddrMapCreateFailCleansUpEniObject)
    {
        CreateApplianceEntry();
        CreateVnet();
        auto eni = BuildEniEntry();

        {
            InSequence seq;
            EXPECT_CALL(*mock_sai_dash_eni_api, create_eni).Times(1);
            EXPECT_CALL(*mock_sai_dash_eni_api, create_eni_ether_address_map_entry)
                .WillOnce(Return(SAI_STATUS_FAILURE));
            EXPECT_CALL(*mock_sai_dash_eni_api, remove_eni).Times(1);
        }

        SetDashTable(APP_DASH_ENI_TABLE_NAME, eni1, eni, true, true);
        EXPECT_TRUE(m_DashOrch->eni_entries_.empty());
    }

    TEST_F(DashOrchTest, EniRouteSetSaiFailure)
    {
        CreateApplianceEntry();
        CreateVnet();
        SetDashTable(APP_DASH_ENI_TABLE_NAME, eni1, BuildEniEntry());
        AddOutboundRoutingGroup();

        dash::eni_route::EniRoute eni_route;
        eni_route.set_group_id(route_group1);

        EXPECT_CALL(*mock_sai_dash_eni_api, set_eni_attribute)
            .WillOnce(Return(SAI_STATUS_FAILURE));
        SetDashTable(APP_DASH_ENI_ROUTE_TABLE_NAME, eni1, eni_route, true, true);
        EXPECT_TRUE(m_DashOrch->eni_route_entries_.empty());
    }

    TEST_F(DashOrchTest, EniRouteRemoveSaiFailure)
    {
        CreateApplianceEntry();
        CreateVnet();
        SetDashTable(APP_DASH_ENI_TABLE_NAME, eni1, BuildEniEntry());
        AddOutboundRoutingGroup();

        dash::eni_route::EniRoute eni_route;
        eni_route.set_group_id(route_group1);
        SetDashTable(APP_DASH_ENI_ROUTE_TABLE_NAME, eni1, eni_route, true, true);
        ASSERT_EQ(m_DashOrch->eni_route_entries_.count(eni1), 1u);

        EXPECT_CALL(*mock_sai_dash_eni_api, set_eni_attribute)
            .WillOnce(Return(SAI_STATUS_FAILURE));
        SetDashTable(APP_DASH_ENI_ROUTE_TABLE_NAME, eni1, dash::eni_route::EniRoute(), false, true);
        EXPECT_EQ(m_DashOrch->eni_route_entries_.count(eni1), 1u);
    }

    TEST_F(DashOrchTest, EniRouteUnknownOp)
    {
        auto consumer = make_unique<Consumer>(
            new swss::ConsumerStateTable(m_app_db.get(), APP_DASH_ENI_ROUTE_TABLE_NAME),
            m_DashOrch, APP_DASH_ENI_ROUTE_TABLE_NAME);
        consumer->addToSync(swss::KeyOpFieldsValuesTuple(
            eni1, "UNKNOWN", {{"pb", dash::eni_route::EniRoute().SerializeAsString()}}));

        m_DashOrch->doTask(*consumer);
        EXPECT_EQ(consumer->m_toSync.begin(), consumer->m_toSync.end());
        EXPECT_TRUE(m_DashOrch->eni_route_entries_.empty());
    }

    TEST_F(DashOrchTest, ApplianceInvalidSipCleansUpAppliance)
    {
        // Appliance entry with local_region_id set (triggers create_dash_appliance)
        // but no SIP set — to_sai(entry.sip()) fails in createApplianceSaiObjects,
        // and the created appliance must be cleaned up.
        dash::appliance::Appliance appliance;
        appliance.set_local_region_id(100);
        appliance.set_vm_vni(9999);
        // Intentionally do not set sip — to_sai will fail

        // create_dash_appliance is called; assign a non-null OID so cleanup path is exercised
        sai_object_id_t fake_oid = 0x1234ABCD;
        {
            InSequence seq;
            EXPECT_CALL(*mock_sai_dash_appliance_api, create_dash_appliance)
                .WillOnce(DoAll(SetArgPointee<0>(fake_oid), Return(SAI_STATUS_SUCCESS)));
            EXPECT_CALL(*mock_sai_dash_appliance_api, remove_dash_appliance).Times(1);
        }
        EXPECT_CALL(*mock_sai_dash_vip_api, create_vip_entry).Times(0);

        SetDashTable(APP_DASH_APPLIANCE_TABLE_NAME, appliance1, appliance, true, true);
        EXPECT_TRUE(m_DashOrch->appliance_entries_.empty());
    }

    TEST_F(DashOrchTest, ApplianceVipFailureCleansUpApplianceWithLocalRegionId)
    {
        // Variation of ApplianceVipCreateFailCleansUpAppliance, but with local_region_id set
        // so create_dash_appliance is called and produces a non-null OID.  This exercises
        // the cleanup path that frees sai_appliance_id (lines 220-224 in dashorch.cpp).
        dash::appliance::Appliance appliance = BuildApplianceEntry();
        appliance.set_local_region_id(123);

        sai_object_id_t fake_oid = 0x99999;
        sai_object_id_t removed_oid = SAI_NULL_OBJECT_ID;
        {
            InSequence seq;
            EXPECT_CALL(*mock_sai_dash_appliance_api, create_dash_appliance)
                .WillOnce(DoAll(SetArgPointee<0>(fake_oid), Return(SAI_STATUS_SUCCESS)));
            EXPECT_CALL(*mock_sai_dash_vip_api, create_vip_entry)
                .WillOnce(Return(SAI_STATUS_FAILURE));
            EXPECT_CALL(*mock_sai_dash_appliance_api, remove_dash_appliance)
                .WillOnce(DoAll(SaveArg<0>(&removed_oid), Return(SAI_STATUS_SUCCESS)));
        }

        SetDashTable(APP_DASH_APPLIANCE_TABLE_NAME, appliance1, appliance, true, true);
        EXPECT_TRUE(m_DashOrch->appliance_entries_.empty());
        EXPECT_EQ(removed_oid, fake_oid);
    }

    TEST_F(DashOrchTest, ApplianceDirectionLookupFailureCleansUpApplianceWithLocalRegionId)
    {
        // Variation that exercises the direction-lookup-failure cleanup with a non-null
        // sai_appliance_id, covering lines 258-262 in dashorch.cpp.
        dash::appliance::Appliance appliance = BuildApplianceEntry();
        appliance.set_local_region_id(456);

        sai_object_id_t fake_oid = 0xDEADBEEF;
        sai_object_id_t removed_oid = SAI_NULL_OBJECT_ID;
        {
            InSequence seq;
            EXPECT_CALL(*mock_sai_dash_appliance_api, create_dash_appliance)
                .WillOnce(DoAll(SetArgPointee<0>(fake_oid), Return(SAI_STATUS_SUCCESS)));
            EXPECT_CALL(*mock_sai_dash_vip_api, create_vip_entry).Times(1);
            EXPECT_CALL(*mock_sai_dash_direction_lookup_api, create_direction_lookup_entry)
                .WillOnce(Return(SAI_STATUS_FAILURE));
            EXPECT_CALL(*mock_sai_dash_vip_api, remove_vip_entry).Times(1);
            EXPECT_CALL(*mock_sai_dash_appliance_api, remove_dash_appliance)
                .WillOnce(DoAll(SaveArg<0>(&removed_oid), Return(SAI_STATUS_SUCCESS)));
        }

        SetDashTable(APP_DASH_APPLIANCE_TABLE_NAME, appliance1, appliance, true, true);
        EXPECT_TRUE(m_DashOrch->appliance_entries_.empty());
        EXPECT_EQ(removed_oid, fake_oid);
    }

    TEST_F(DashOrchTest, RemoveApplianceWithLocalRegionId)
    {
        // Create appliance with local_region_id so create_dash_appliance is called and
        // a non-null SAI ID is cached.  Removal should then call remove_dash_appliance
        // (covering lines 365-377 of dashorch.cpp).
        dash::appliance::Appliance appliance = BuildApplianceEntry();
        appliance.set_local_region_id(789);

        sai_object_id_t fake_oid = 0x1111ABCD;
        sai_object_id_t removed_oid = SAI_NULL_OBJECT_ID;
        {
            InSequence seq;
            EXPECT_CALL(*mock_sai_dash_appliance_api, create_dash_appliance)
                .WillOnce(DoAll(SetArgPointee<0>(fake_oid), Return(SAI_STATUS_SUCCESS)));
            EXPECT_CALL(*mock_sai_dash_appliance_api, remove_dash_appliance)
                .WillOnce(DoAll(SaveArg<0>(&removed_oid), Return(SAI_STATUS_SUCCESS)));
        }
        SetDashTable(APP_DASH_APPLIANCE_TABLE_NAME, appliance1, appliance);
        SetDashTable(APP_DASH_APPLIANCE_TABLE_NAME, appliance1, dash::appliance::Appliance(), false, true);

        EXPECT_TRUE(m_DashOrch->appliance_entries_.empty());
        EXPECT_EQ(removed_oid, fake_oid);
    }

    TEST_F(DashOrchTest, RemoveApplianceSaiAppliancesRemoveFailurePreservesCache)
    {
        // Variation of RemoveApplianceWithLocalRegionId where the SAI appliance remove
        // returns a non-recoverable error.  Cache must be preserved (covers line 374).
        dash::appliance::Appliance appliance = BuildApplianceEntry();
        appliance.set_local_region_id(789);

        sai_object_id_t fake_oid = 0x12345678;
        {
            InSequence seq;
            EXPECT_CALL(*mock_sai_dash_appliance_api, create_dash_appliance)
                .WillOnce(DoAll(SetArgPointee<0>(fake_oid), Return(SAI_STATUS_SUCCESS)));
            EXPECT_CALL(*mock_sai_dash_appliance_api, remove_dash_appliance)
                .WillOnce(Return(SAI_STATUS_FAILURE));
            // Second remove succeeds (explicitly mocked because fake_oid is not a real saivs OID)
            EXPECT_CALL(*mock_sai_dash_appliance_api, remove_dash_appliance)
                .WillOnce(Return(SAI_STATUS_SUCCESS));
        }
        SetDashTable(APP_DASH_APPLIANCE_TABLE_NAME, appliance1, appliance);
        // First DEL — remove fails, cache preserved
        SetDashTable(APP_DASH_APPLIANCE_TABLE_NAME, appliance1, dash::appliance::Appliance(), false, true);
        EXPECT_EQ(m_DashOrch->appliance_entries_.count(appliance1), 1u);
        // Second DEL — succeeds
        SetDashTable(APP_DASH_APPLIANCE_TABLE_NAME, appliance1, dash::appliance::Appliance(), false, true);
        EXPECT_TRUE(m_DashOrch->appliance_entries_.empty());
    }

    TEST_F(DashOrchTest, RemoveApplianceWithInvalidCachedSip)
    {
        // Create an appliance normally, then mutate the cached entry to have an invalid
        // SIP so the to_sai() call in removeApplianceEntry fails — exercising the
        // "skipping VIP cleanup" warning path (line 345 in dashorch.cpp).
        CreateApplianceEntry();
        ASSERT_EQ(m_DashOrch->appliance_entries_.count(appliance1), 1u);

        // Replace the cached metadata's sip with an empty one (neither ipv4 nor ipv6 set)
        m_DashOrch->appliance_entries_[appliance1].metadata.clear_sip();

        // VIP cleanup should be skipped; direction lookup remove is still called.
        EXPECT_CALL(*mock_sai_dash_vip_api, remove_vip_entry).Times(0);
        EXPECT_CALL(*mock_sai_dash_direction_lookup_api, remove_direction_lookup_entry).Times(1);
        SetDashTable(APP_DASH_APPLIANCE_TABLE_NAME, appliance1, dash::appliance::Appliance(), false, true);
        EXPECT_TRUE(m_DashOrch->appliance_entries_.empty());
    }

    TEST_F(DashOrchTest, ApplianceExceptionInDoTaskConsumed)
    {
        // A SAI exception during appliance processing should be caught, an ERROR-level
        // failure result written, and the consumer entry removed (covers lines 475-482).
        dash::appliance::Appliance appliance = BuildApplianceEntry();
        EXPECT_CALL(*mock_sai_dash_appliance_api, create_dash_appliance)
            .WillOnce(Throw(std::runtime_error("simulated SAI exception")));

        SetDashTable(APP_DASH_APPLIANCE_TABLE_NAME, appliance1, appliance, true, true);
        EXPECT_TRUE(m_DashOrch->appliance_entries_.empty());
    }

    TEST_F(DashOrchTest, EniExceptionInDoTaskConsumed)
    {
        // A SAI exception during ENI add should be caught and the consumer entry removed
        // (covers lines 1160-1167 in dashorch.cpp).
        CreateApplianceEntry();
        CreateVnet();
        auto eni = BuildEniEntry();

        EXPECT_CALL(*mock_sai_dash_eni_api, create_eni)
            .WillOnce(Throw(std::runtime_error("simulated SAI exception")));

        SetDashTable(APP_DASH_ENI_TABLE_NAME, eni1, eni, true, true);
        EXPECT_TRUE(m_DashOrch->eni_entries_.empty());
    }

    TEST_F(DashOrchTest, EniRouteExceptionInDoTaskConsumed)
    {
        // A SAI exception during ENI route set should be caught and the consumer entry
        // removed (covers lines 1411-1418 in dashorch.cpp).
        CreateApplianceEntry();
        CreateVnet();
        SetDashTable(APP_DASH_ENI_TABLE_NAME, eni1, BuildEniEntry());
        AddOutboundRoutingGroup();

        dash::eni_route::EniRoute eni_route;
        eni_route.set_group_id(route_group1);

        EXPECT_CALL(*mock_sai_dash_eni_api, set_eni_attribute)
            .WillOnce(Throw(std::runtime_error("simulated SAI exception")));
        SetDashTable(APP_DASH_ENI_ROUTE_TABLE_NAME, eni1, eni_route, true, true);
        EXPECT_TRUE(m_DashOrch->eni_route_entries_.empty());
    }

    TEST_F(DashOrchTest, AddRemoveEniWritesAndClearsResult)
    {
        // Ensure that a full ENI create+remove cycle writes a SUCCESS result on add
        // and removes the result on delete.  Exercises the post-write/post-remove
        // paths in doTaskEniTable and the corresponding meter-counter and CRM cleanup
        // (line 982 / line 1032 in dashorch.cpp).
        CreateApplianceEntry();
        CreateVnet();
        SetDashTable(APP_DASH_ENI_TABLE_NAME, eni1, BuildEniEntry());
        ASSERT_EQ(m_DashOrch->eni_entries_.count(eni1), 1u);

        // Now remove — covers the success path through removeEniAddrMapEntry which
        // decrements the CRM counter for ETHER_ADDRESS_MAP.
        SetDashTable(APP_DASH_ENI_TABLE_NAME, eni1, dash::eni::Eni(), false, true);
        EXPECT_TRUE(m_DashOrch->eni_entries_.empty());
    }

    TEST_F(DashOrchTest, RoutingTypeWritesResultOnSet)
    {
        // After a successful SET, the routing-type result table must contain a
        // SUCCESS entry (covers line 562 in dashorch.cpp).
        dash::route_type::RouteType route_type;
        route_type.add_items()->set_action_type(dash::route_type::ACTION_TYPE_STATICENCAP);
        SetDashTable(APP_DASH_ROUTING_TYPE_TABLE_NAME, "VNET", route_type);

        swss::Table result_table(m_dpu_app_state_db.get(), APP_DASH_ROUTING_TYPE_TABLE_NAME);
        std::vector<swss::FieldValueTuple> fvs;
        EXPECT_TRUE(result_table.get("ROUTING_TYPE_VNET", fvs));
    }

    TEST_F(DashOrchTest, QosWritesResultOnSet)
    {
        // After a successful SET, the QOS result table must contain a SUCCESS entry
        // (covers line 1230 in dashorch.cpp).
        dash::qos::Qos qos;
        qos.set_bw(100);
        SetDashTable(APP_DASH_QOS_TABLE_NAME, "QOS_1", qos);

        swss::Table result_table(m_dpu_app_state_db.get(), APP_DASH_QOS_TABLE_NAME);
        std::vector<swss::FieldValueTuple> fvs;
        EXPECT_TRUE(result_table.get("QOS_1", fvs));
    }

}