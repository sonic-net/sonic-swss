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
#include "dash_api/route.pb.h"
#include "dash_api/route_rule.pb.h"
#include "dash_api/eni.pb.h"
#include "dash_api/qos.pb.h"
#include "dash_api/eni_route.pb.h"
#include "swssnet.h"
#include "crmorch.h"

EXTERN_MOCK_FNS
namespace dashrouteorch_test
{
    DEFINE_SAI_API_MOCK(dash_outbound_routing, outbound_routing);
    DEFINE_SAI_API_MOCK(dash_inbound_routing, inbound_routing);
    using namespace mock_orch_test;
    using ::testing::InSequence;
    using ::testing::DoAll;
    using ::testing::SaveArgPointee;
    using ::testing::Invoke;
    using ::testing::Return;
    using ::testing::SetArrayArgument;
    class DashRouteOrchTest : public MockDashOrchTest, public ::testing::WithParamInterface<std::tuple<swss::IpPrefix, int>>
    {
    protected:
        uint32_t GetCrmUsedCount(CrmResourceType type)
        {
            return gCrmOrch->m_resourcesMap.at(type).countersMap["STATS"].usedCounter;
        }

        void PostSetUp()
        {
            CreateApplianceEntry();
            CreateVnet();
            auto eni = BuildEniEntry();
            SetDashTable(APP_DASH_ENI_TABLE_NAME, eni1, eni);
        }

        void ApplySaiMock()
        {
            INIT_SAI_API_MOCK(dash_outbound_routing);
            INIT_SAI_API_MOCK(dash_inbound_routing);
            MockSaiApis();
        }

        void PreTearDown()
        {
            RestoreSaiApis();
            DEINIT_SAI_API_MOCK(dash_inbound_routing);
            DEINIT_SAI_API_MOCK(dash_outbound_routing);
            DEINIT_SAI_API_MOCK(dash_inbound_routing);
        }
        public:
            void VerifyInboundRoutingEntry(sai_inbound_routing_entry_t actual_entry, uint32_t expected_vni, swss::IpPrefix expected_prefix, uint32_t expected_priority)
            {
                EXPECT_EQ(actual_entry.vni, expected_vni);
                EXPECT_EQ(actual_entry.priority, expected_priority);

                sai_ip_address_t expected_sip, expected_sip_mask;
                swss::copy(expected_sip, expected_prefix.getIp());
                swss::copy(expected_sip_mask, expected_prefix.getMask());
                EXPECT_EQ(actual_entry.sip, expected_sip);
                EXPECT_EQ(actual_entry.sip_mask, expected_sip_mask);
            }
            void VerifyInboundRoutingAction(std::vector<sai_attribute_t> &actual_attrs, sai_inbound_routing_entry_action_t expected_action)
            {
                for (auto &attr : actual_attrs)
                {
                    if (attr.id == SAI_INBOUND_ROUTING_ENTRY_ATTR_ACTION)
                    {
                        EXPECT_EQ(attr.value.u32, expected_action);
                        return;
                    }
                }
                FAIL() << "SAI_INBOUND_ROUTING_ENTRY_ATTR_ACTION not found in attributes";
            }
    };

    TEST_F(DashRouteOrchTest, RouteWithMissingTunnelNotAdded)
    {
        {
            InSequence seq;
            EXPECT_CALL(*mock_sai_dash_outbound_routing_api, create_outbound_routing_entries).Times(0);
            EXPECT_CALL(*mock_sai_dash_outbound_routing_api, create_outbound_routing_entries).Times(1);
        }
        AddOutboundRoutingGroup();
        AddOutboundRoutingEntry(false);
        
        AddTunnel();
        AddOutboundRoutingEntry();
    }

    TEST_P(DashRouteOrchTest, AddRemoveInboundRouting)
    {
        int vni = 5555;
        swss::IpPrefix prefix;
        int priority;
        std::tie(prefix, priority) = GetParam();
        uint32_t expected_priority = priority >= 0 ? static_cast<uint32_t>(priority) : 0;

        sai_inbound_routing_entry_t created_entry;
        sai_inbound_routing_entry_t removed_entry;
        std::vector<sai_attribute_t> actual_attrs;
        
        {
            InSequence seq;
            EXPECT_CALL(*mock_sai_dash_inbound_routing_api, create_inbound_routing_entries)
                .WillOnce(
                    DoAll(
                        SaveArgPointee<1>(&created_entry),
                        [&actual_attrs] (uint32_t object_count, const sai_inbound_routing_entry_t *inbound_routing_entry, const uint32_t *attr_count, const sai_attribute_t **attr_list, sai_bulk_op_error_mode_t mode, sai_status_t *object_statuses) {
                            actual_attrs.assign(*attr_list, *attr_list + *attr_count);
                        },
                        Invoke(old_sai_dash_inbound_routing_api, &sai_dash_inbound_routing_api_t::create_inbound_routing_entries)
                    )
                );
            EXPECT_CALL(*mock_sai_dash_inbound_routing_api, remove_inbound_routing_entries)
                .WillOnce(
                    DoAll(
                        SaveArgPointee<1>(&removed_entry),
                        Invoke(old_sai_dash_inbound_routing_api, &sai_dash_inbound_routing_api_t::remove_inbound_routing_entries)
                    )
                );
        }
        std::stringstream key_stream;
        if (priority >= 0)
            key_stream << eni1 << ":" << vni << ":" << prefix.to_string() << ":" << priority;
        else
            key_stream << eni1 << ":" << vni << ":" << prefix.to_string();
        SetDashTable(APP_DASH_ROUTE_RULE_TABLE_NAME, key_stream.str(), dash::route_rule::RouteRule());

        VerifyInboundRoutingEntry(created_entry, vni, prefix, expected_priority);
        VerifyInboundRoutingAction(actual_attrs, SAI_INBOUND_ROUTING_ENTRY_ACTION_TUNNEL_DECAP);

        SetDashTable(APP_DASH_ROUTE_RULE_TABLE_NAME, key_stream.str(), dash::route_rule::RouteRule(), false);
        VerifyInboundRoutingEntry(removed_entry, vni, prefix, expected_priority);
    }

    INSTANTIATE_TEST_SUITE_P(
        DashRouteOrchInboundRoutingTest,
        DashRouteOrchTest,
        ::testing::Combine(
            ::testing::Values(swss::IpPrefix("100.200.1.2/32"), swss::IpPrefix("2001:db8::1/128")),
            ::testing::Values(0, 101, -1)), // Use -1 to test the case where priority is not set and should default to 0
        [](const testing::TestParamInfo<DashRouteOrchTest::ParamType> &info) {
            const auto &prefix = std::get<0>(info.param);
            const auto &priority = std::get<1>(info.param);
            const std::string addr_family = prefix.isV4() ? "IPv4" : "IPv6";
            const std::string priority_str = (priority >= 0) ? std::to_string(priority) : "None";
            return "InboundRouting_" + addr_family + "_Priority_" + priority_str;
        });

    TEST_F(DashRouteOrchTest, RemoveNonexistOutboundRoutingDoesNotDecrementCrm)
    {
        uint32_t baselineUsed = GetCrmUsedCount(CrmResourceType::CRM_DASH_IPV4_OUTBOUND_ROUTING);
        // Remove non-existent outbound routing entry should return SAI_STATUS_ITEM_NOT_FOUND and not decrement the CRM used count
        std::vector<sai_status_t> exp_status = {SAI_STATUS_ITEM_NOT_FOUND};
        EXPECT_CALL(*mock_sai_dash_outbound_routing_api, remove_outbound_routing_entries)
            .Times(1).WillOnce(DoAll(SetArrayArgument<3>(exp_status.begin(), exp_status.end()), Return(SAI_STATUS_SUCCESS)));
        RemoveOutboundRoutingEntry();
        EXPECT_EQ(GetCrmUsedCount(CrmResourceType::CRM_DASH_IPV4_OUTBOUND_ROUTING), baselineUsed);
    }

    TEST_F(DashRouteOrchTest, RemoveNonexistInboundRoutingDoesNotDecrementCrm)
    {
        // Create an ENI entry to test removal of inbound routing entry
        SetDashTable(APP_DASH_ENI_TABLE_NAME, eni1, BuildEniEntry());
        uint32_t baselineUsed = GetCrmUsedCount(CrmResourceType::CRM_DASH_IPV4_INBOUND_ROUTING);
        // Remove non-existent inbound routing entry should return SAI_STATUS_ITEM_NOT_FOUND and not decrement the CRM used count
        std::vector<sai_status_t> exp_status = {SAI_STATUS_ITEM_NOT_FOUND};
        EXPECT_CALL(*mock_sai_dash_inbound_routing_api, remove_inbound_routing_entries)
            .Times(1).WillOnce(DoAll(SetArrayArgument<3>(exp_status.begin(), exp_status.end()), Return(SAI_STATUS_SUCCESS)));
        RemoveInboundRoutingEntry();
        EXPECT_EQ(GetCrmUsedCount(CrmResourceType::CRM_DASH_IPV4_INBOUND_ROUTING), baselineUsed);
    }
}