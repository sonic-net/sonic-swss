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
    DEFINE_SAI_API_COMBINED_MOCK(dash_outbound_routing, outbound_routing_group, outbound_routing);
    DEFINE_SAI_API_MOCK(dash_inbound_routing, inbound_routing);
    using namespace mock_orch_test;
    using ::testing::_;
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

        std::unique_ptr<Consumer> CreateDashRouteConsumer(const std::string &tableName)
        {
            return std::make_unique<Consumer>(
                new swss::ConsumerStateTable(m_app_db.get(), tableName),
                m_DashRouteOrch,
                tableName);
        }

        void PreTearDown()
        {
            RestoreSaiApis();
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
        AddOutboundRoutingEntry(true);
        
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

    TEST_F(DashRouteOrchTest, AddRemoveInboundRoutingEntry)
    {
        AddInboundRoutingEntry();
        RemoveInboundRoutingEntry();
    }

    TEST_F(DashRouteOrchTest, AddRemoveRouteGroup)
    {
        AddOutboundRoutingGroup();
        SetDashTable(APP_DASH_ROUTE_GROUP_TABLE_NAME, route_group1, dash::route_group::RouteGroup(), false, true);
    }

    TEST_F(DashRouteOrchTest, InboundRouteSaiCreateFailureNotRetried)
    {
        std::vector<sai_status_t> exp_status = {SAI_STATUS_INVALID_PARAMETER};
        EXPECT_CALL(*mock_sai_dash_inbound_routing_api, create_inbound_routing_entries)
            .WillOnce(DoAll(SetArrayArgument<5>(exp_status.begin(), exp_status.end()), Return(SAI_STATUS_SUCCESS)));
        AddInboundRoutingEntry(true);
    }

    TEST_F(DashRouteOrchTest, RemoveNonexistInboundRoutingDoesNotDecrementCrm)
    {
        uint32_t baselineUsed = GetCrmUsedCount(CrmResourceType::CRM_DASH_IPV4_INBOUND_ROUTING);
        // Remove non-existent inbound routing entry should return SAI_STATUS_ITEM_NOT_FOUND and not decrement the CRM used count
        std::vector<sai_status_t> exp_status = {SAI_STATUS_ITEM_NOT_FOUND};
        EXPECT_CALL(*mock_sai_dash_inbound_routing_api, remove_inbound_routing_entries)
            .Times(1).WillOnce(DoAll(SetArrayArgument<3>(exp_status.begin(), exp_status.end()), Return(SAI_STATUS_SUCCESS)));
        RemoveInboundRoutingEntry();
        EXPECT_EQ(GetCrmUsedCount(CrmResourceType::CRM_DASH_IPV4_INBOUND_ROUTING), baselineUsed);
    }

    TEST_F(DashRouteOrchTest, OutboundRouteMissingVnetNotRetried)
    {
        // Route references VNET but VNET doesn't exist (PostSetUp creates it, so we need a route referencing a different VNET)
        dash::route::Route route = dash::route::Route();
        route.set_routing_type(dash::route_type::ROUTING_TYPE_VNET);
        route.set_vnet("NON_EXISTENT_VNET");
        EXPECT_CALL(*mock_sai_dash_outbound_routing_api, create_outbound_routing_entries).Times(0);
        AddOutboundRoutingGroup();
        SetDashTable(APP_DASH_ROUTE_TABLE_NAME, route_group1 + ":1.2.3.4/32", route, true, true);
    }

    TEST_F(DashRouteOrchTest, OutboundRouteSaiInvalidParameterNotRetried)
    {
        AddOutboundRoutingGroup();
        AddTunnel();
        std::vector<sai_status_t> exp_status = {SAI_STATUS_INVALID_PARAMETER};
        EXPECT_CALL(*mock_sai_dash_outbound_routing_api, create_outbound_routing_entries)
            .WillOnce(DoAll(SetArrayArgument<5>(exp_status.begin(), exp_status.end()), Return(SAI_STATUS_SUCCESS)));
        AddOutboundRoutingEntry(true);
    }

    TEST_F(DashRouteOrchTest, OutboundRouteSaiInsufficientResourcesNotRetried)
    {
        AddOutboundRoutingGroup();
        AddTunnel();
        std::vector<sai_status_t> exp_status = {SAI_STATUS_INSUFFICIENT_RESOURCES};
        EXPECT_CALL(*mock_sai_dash_outbound_routing_api, create_outbound_routing_entries)
            .WillOnce(DoAll(SetArrayArgument<5>(exp_status.begin(), exp_status.end()), Return(SAI_STATUS_SUCCESS)));
        AddOutboundRoutingEntry(true);
    }

    TEST_F(DashRouteOrchTest, OutboundRouteMissingRouteGroupNotRetried)
    {
        EXPECT_CALL(*mock_sai_dash_outbound_routing_api, create_outbound_routing_entries).Times(0);
        dash::route::Route route = dash::route::Route();
        route.set_routing_type(dash::route_type::ROUTING_TYPE_VNET);
        route.set_vnet(vnet1);
        SetDashTable(APP_DASH_ROUTE_TABLE_NAME, route_group1 + ":1.2.3.4/32", route, true, true);
    }

    TEST_F(DashRouteOrchTest, InboundRouteMissingEniNotRetried)
    {
        // Inbound route references an ENI that doesn't exist
        dash::route_rule::RouteRule rule = dash::route_rule::RouteRule();
        rule.set_pa_validation(true);
        EXPECT_CALL(*mock_sai_dash_inbound_routing_api, create_inbound_routing_entries).Times(0);
        SetDashTable(APP_DASH_ROUTE_RULE_TABLE_NAME, "NON_EXISTENT_ENI:5555:10.0.0.0/24", rule, true, true);
    }

    TEST_F(DashRouteOrchTest, RouteGroupSaiRemoveInUseNotRetried)
    {
        AddOutboundRoutingGroup();
        // Route group remove with bound routes should not retry.
        // Bind a route to the group, then try to remove the group
        AddTunnel();
        AddOutboundRoutingEntry();
        // Removing the route group while routes are bound should consume the notification
        SetDashTable(APP_DASH_ROUTE_GROUP_TABLE_NAME, route_group1, dash::route_group::RouteGroup(), false, true);
    }

    TEST_F(DashRouteOrchTest, OutboundRouteSaiRemoveNotExecutedNotRetried)
    {
        AddOutboundRoutingGroup();
        AddTunnel();
        AddOutboundRoutingEntry();
        std::vector<sai_status_t> exp_status = {SAI_STATUS_NOT_EXECUTED};
        EXPECT_CALL(*mock_sai_dash_outbound_routing_api, remove_outbound_routing_entries)
            .WillOnce(DoAll(SetArrayArgument<3>(exp_status.begin(), exp_status.end()), Return(SAI_STATUS_SUCCESS)));
        RemoveOutboundRoutingEntry(true);
    }

    TEST_F(DashRouteOrchTest, InboundRouteSaiRemoveNotExecutedNotRetried)
    {
        AddInboundRoutingEntry();
        std::vector<sai_status_t> exp_status = {SAI_STATUS_NOT_EXECUTED};
        EXPECT_CALL(*mock_sai_dash_inbound_routing_api, remove_inbound_routing_entries)
            .WillOnce(DoAll(SetArrayArgument<3>(exp_status.begin(), exp_status.end()), Return(SAI_STATUS_SUCCESS)));
        RemoveInboundRoutingEntry(true);
    }

    TEST_F(DashRouteOrchTest, RemoveNonExistentRouteGroup)
    {
        SetDashTable(APP_DASH_ROUTE_GROUP_TABLE_NAME, route_group1, dash::route_group::RouteGroup(), false, true);
    }

    TEST_F(DashRouteOrchTest, MissingProtobufOutboundRoute)
    {
        AddOutboundRoutingGroup();
        EXPECT_CALL(*mock_sai_dash_outbound_routing_api, create_outbound_routing_entries).Times(0);
        SetDashTableRaw(APP_DASH_ROUTE_TABLE_NAME, route_group1 + ":1.2.3.4/32", {}, true, true);
    }

    TEST_F(DashRouteOrchTest, InvalidProtobufOutboundRoute)
    {
        AddOutboundRoutingGroup();
        EXPECT_CALL(*mock_sai_dash_outbound_routing_api, create_outbound_routing_entries).Times(0);
        SetDashTableRaw(APP_DASH_ROUTE_TABLE_NAME, route_group1 + ":1.2.3.4/32", {{ "pb", "garbage" }}, true, true);
    }

    TEST_F(DashRouteOrchTest, MissingProtobufInboundRoute)
    {
        EXPECT_CALL(*mock_sai_dash_inbound_routing_api, create_inbound_routing_entries).Times(0);
        SetDashTableRaw(APP_DASH_ROUTE_RULE_TABLE_NAME, eni1 + ":5555:10.0.0.0/24", {}, true, true);
    }

    TEST_F(DashRouteOrchTest, MissingProtobufRouteGroup)
    {
        SetDashTableRaw(APP_DASH_ROUTE_GROUP_TABLE_NAME, route_group1, {}, true, true);
    }

    TEST_F(DashRouteOrchTest, InvalidKeyOutboundRoute)
    {
        // Invalid keys should be caught per-item and consumed without throwing.
        AddOutboundRoutingGroup();
        dash::route::Route route = dash::route::Route();
        route.set_routing_type(dash::route_type::ROUTING_TYPE_VNET);
        route.set_vnet(vnet1);
        EXPECT_CALL(*mock_sai_dash_outbound_routing_api, create_outbound_routing_entries).Times(0);
        EXPECT_NO_THROW(
            SetDashTable(APP_DASH_ROUTE_TABLE_NAME, route_group1 + ":not_an_ip", route, true, true));
    }

    TEST_F(DashRouteOrchTest, InvalidKeyInboundRoute)
    {
        // Invalid keys should be caught per-item and consumed without throwing.
        dash::route_rule::RouteRule rule = dash::route_rule::RouteRule();
        EXPECT_CALL(*mock_sai_dash_inbound_routing_api, create_inbound_routing_entries).Times(0);
        EXPECT_NO_THROW(
            SetDashTable(APP_DASH_ROUTE_RULE_TABLE_NAME, eni1 + ":not_a_vni:10.0.0.0/24", rule, true, true));
    }

    TEST_F(DashRouteOrchTest, OutboundRouteCreateDeleteChurn)
    {
        AddOutboundRoutingGroup();
        AddTunnel();

        for (int i = 0; i < 3; i++)
        {
            EXPECT_CALL(*mock_sai_dash_outbound_routing_api, create_outbound_routing_entries).Times(1);
            AddOutboundRoutingEntry();

            EXPECT_CALL(*mock_sai_dash_outbound_routing_api, remove_outbound_routing_entries).Times(1);
            RemoveOutboundRoutingEntry();
        }
    }

    TEST_F(DashRouteOrchTest, InboundRouteCreateDeleteChurn)
    {
        for (int i = 0; i < 3; i++)
        {
            EXPECT_CALL(*mock_sai_dash_inbound_routing_api, create_inbound_routing_entries).Times(1);
            AddInboundRoutingEntry();

            EXPECT_CALL(*mock_sai_dash_inbound_routing_api, remove_inbound_routing_entries).Times(1);
            RemoveInboundRoutingEntry();
        }
    }

    TEST_F(DashRouteOrchTest, RouteGroupCreateDeleteChurn)
    {
        for (int i = 0; i < 3; i++)
        {
            AddOutboundRoutingGroup();
            SetDashTable(APP_DASH_ROUTE_GROUP_TABLE_NAME, route_group1, dash::route_group::RouteGroup(), false, true);
        }
    }

    TEST_F(DashRouteOrchTest, OutboundRouteCreateFailThenSucceed)
    {
        AddOutboundRoutingGroup();
        AddTunnel();

        {
            InSequence seq;
            // First create returns INVALID_PARAMETER
            EXPECT_CALL(*mock_sai_dash_outbound_routing_api, create_outbound_routing_entries)
                .WillOnce([](uint32_t count, const sai_outbound_routing_entry_t*, const uint32_t*, const sai_attribute_t**, sai_bulk_op_error_mode_t, sai_status_t* statuses) {
                    statuses[0] = SAI_STATUS_INVALID_PARAMETER;
                    return SAI_STATUS_SUCCESS;
                });
            // Second create succeeds
            EXPECT_CALL(*mock_sai_dash_outbound_routing_api, create_outbound_routing_entries).Times(1);
        }

        AddOutboundRoutingEntry(true);
        AddOutboundRoutingEntry();
    }

    TEST_F(DashRouteOrchTest, OutboundRouteKeyMissingPrefix)
    {
        // Key should be "route_group:ip_prefix" — send just route group without prefix
        AddOutboundRoutingGroup();
        dash::route::Route route = dash::route::Route();
        route.set_routing_type(dash::route_type::ROUTING_TYPE_VNET);
        route.set_vnet(vnet1);
        EXPECT_CALL(*mock_sai_dash_outbound_routing_api, create_outbound_routing_entries).Times(0);
        EXPECT_NO_THROW(
            SetDashTable(APP_DASH_ROUTE_TABLE_NAME, route_group1, route, true, true));
    }

    TEST_F(DashRouteOrchTest, InboundRouteKeyMissingVniAndPrefix)
    {
        // Key should be "eni:vni:prefix" — send just ENI without vni/prefix
        dash::route_rule::RouteRule rule = dash::route_rule::RouteRule();
        EXPECT_CALL(*mock_sai_dash_inbound_routing_api, create_inbound_routing_entries).Times(0);
        EXPECT_NO_THROW(
            SetDashTable(APP_DASH_ROUTE_RULE_TABLE_NAME, eni1, rule, true, true));
    }

    TEST_F(DashRouteOrchTest, InboundRouteKeyMissingPrefix)
    {
        // Key should be "eni:vni:prefix" — send eni:vni without prefix
        dash::route_rule::RouteRule rule = dash::route_rule::RouteRule();
        EXPECT_CALL(*mock_sai_dash_inbound_routing_api, create_inbound_routing_entries).Times(0);
        EXPECT_NO_THROW(
            SetDashTable(APP_DASH_ROUTE_RULE_TABLE_NAME, eni1 + ":5555", rule, true, true));
    }

    TEST_F(DashRouteOrchTest, OutboundRouteAlreadyExistsInSai)
    {
        AddOutboundRoutingGroup();
        AddTunnel();
        // First create succeeds normally
        AddOutboundRoutingEntry();
        // Second create returns ITEM_ALREADY_EXISTS from bulker — should be treated as success
        std::vector<sai_status_t> exp_status = {SAI_STATUS_ITEM_ALREADY_EXISTS};
        EXPECT_CALL(*mock_sai_dash_outbound_routing_api, create_outbound_routing_entries)
            .WillOnce(DoAll(SetArrayArgument<5>(exp_status.begin(), exp_status.end()), Return(SAI_STATUS_SUCCESS)));
        AddOutboundRoutingEntry(true);
    }

    TEST_F(DashRouteOrchTest, InboundRouteAlreadyExistsInSai)
    {
        // First create succeeds normally
        AddInboundRoutingEntry();
        // Second create returns ITEM_ALREADY_EXISTS from bulker — should be treated as success
        std::vector<sai_status_t> exp_status = {SAI_STATUS_ITEM_ALREADY_EXISTS};
        EXPECT_CALL(*mock_sai_dash_inbound_routing_api, create_inbound_routing_entries)
            .WillOnce(DoAll(SetArrayArgument<5>(exp_status.begin(), exp_status.end()), Return(SAI_STATUS_SUCCESS)));
        AddInboundRoutingEntry(true);
    }

    TEST_F(DashRouteOrchTest, OutboundRouteBoundRouteGroupNotRetried)
    {
        AddOutboundRoutingGroup();
        m_DashRouteOrch->bindRouteGroup(route_group1);

        dash::route::Route route;
        route.set_routing_type(dash::route_type::ROUTING_TYPE_VNET);
        route.set_vnet(vnet1);

        EXPECT_CALL(*mock_sai_dash_outbound_routing_api, create_outbound_routing_entries).Times(0);
        SetDashTable(APP_DASH_ROUTE_TABLE_NAME, route_group1 + ":5.6.7.8/32", route, true, true);

        m_DashRouteOrch->unbindRouteGroup(route_group1);
    }

    TEST_F(DashRouteOrchTest, OutboundRouteVnetDirectMissingVnet)
    {
        m_DashRouteOrch->route_group_oid_map_[route_group1] = 0x1234;

        dash::route::Route route;
        route.set_routing_type(dash::route_type::ROUTING_TYPE_VNET_DIRECT);
        route.mutable_vnet_direct()->mutable_overlay_ip()->set_ipv4(swss::IpAddress("5.6.7.8").getV4Addr());

        OutboundRoutingBulkContext ctxt;
        ctxt.route_group = route_group1;
        ctxt.destination = swss::IpPrefix("1.2.3.4/32");
        ctxt.metadata = route;

        EXPECT_TRUE(m_DashRouteOrch->addOutboundRouting(route_group1 + ":1.2.3.4/32", ctxt));
        EXPECT_EQ(ctxt.pre_op_result, DASH_RESULT_FAILURE);
        EXPECT_TRUE(ctxt.object_statuses.empty());
    }

    TEST_F(DashRouteOrchTest, OutboundRouteInvalidRoutingType)
    {
        m_DashRouteOrch->route_group_oid_map_[route_group1] = 0x1234;

        dash::route::Route route;
        route.set_routing_type(static_cast<dash::route_type::RoutingType>(999));

        OutboundRoutingBulkContext ctxt;
        ctxt.route_group = route_group1;
        ctxt.destination = swss::IpPrefix("1.2.3.4/32");
        ctxt.metadata = route;

        EXPECT_TRUE(m_DashRouteOrch->addOutboundRouting(route_group1 + ":1.2.3.4/32", ctxt));
        EXPECT_EQ(ctxt.pre_op_result, DASH_RESULT_FAILURE);
        EXPECT_TRUE(ctxt.object_statuses.empty());
    }

    TEST_F(DashRouteOrchTest, OutboundRouteVnetDirectMissingOverlayIp)
    {
        m_DashRouteOrch->route_group_oid_map_[route_group1] = 0x1234;

        dash::route::Route route;
        route.set_routing_type(dash::route_type::ROUTING_TYPE_VNET_DIRECT);
        route.mutable_vnet_direct()->set_vnet(vnet1);

        OutboundRoutingBulkContext ctxt;
        ctxt.route_group = route_group1;
        ctxt.destination = swss::IpPrefix("1.2.3.4/32");
        ctxt.metadata = route;

        EXPECT_TRUE(m_DashRouteOrch->addOutboundRouting(route_group1 + ":1.2.3.4/32", ctxt));
        EXPECT_EQ(ctxt.pre_op_result, DASH_RESULT_FAILURE);
        EXPECT_TRUE(ctxt.object_statuses.empty());
    }

    TEST_F(DashRouteOrchTest, RemoveOutboundRouteBoundRouteGroup)
    {
        AddOutboundRoutingGroup();
        m_DashRouteOrch->bindRouteGroup(route_group1);

        EXPECT_CALL(*mock_sai_dash_outbound_routing_api, remove_outbound_routing_entries).Times(0);
        RemoveOutboundRoutingEntry();

        m_DashRouteOrch->unbindRouteGroup(route_group1);
    }

    TEST_F(DashRouteOrchTest, OutboundRouteDeprecatedActionTypeFallback)
    {
        AddOutboundRoutingGroup();

        dash::route::Route route;
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
        route.set_action_type(dash::route_type::ROUTING_TYPE_VNET);
#pragma GCC diagnostic pop
        route.set_vnet(vnet1);

        EXPECT_CALL(*mock_sai_dash_outbound_routing_api, create_outbound_routing_entries).Times(1);
        SetDashTable(APP_DASH_ROUTE_TABLE_NAME, route_group1 + ":9.9.9.9/32", route, true, true);
    }

    TEST_F(DashRouteOrchTest, OutboundRouteUnknownOp)
    {
        auto consumer = CreateDashRouteConsumer(APP_DASH_ROUTE_TABLE_NAME);
        consumer->m_toSync.emplace(
            route_group1 + ":1.2.3.4/32",
            swss::KeyOpFieldsValuesTuple(route_group1 + ":1.2.3.4/32", "UNKNOWN", {}));

        m_DashRouteOrch->doTaskRouteTable(*consumer);

        EXPECT_TRUE(consumer->m_toSync.empty());
    }

    TEST_F(DashRouteOrchTest, OutboundRouteDuplicateSetClearsContext)
    {
        AddOutboundRoutingGroup();

        auto consumer = CreateDashRouteConsumer(APP_DASH_ROUTE_TABLE_NAME);
        std::string key = route_group1 + ":1.2.3.4/32";

        dash::route::Route validRoute;
        validRoute.set_routing_type(dash::route_type::ROUTING_TYPE_VNET);
        validRoute.set_vnet(vnet1);

        dash::route::Route invalidRoute;
        invalidRoute.set_routing_type(dash::route_type::ROUTING_TYPE_VNET_DIRECT);
        invalidRoute.mutable_vnet_direct()->set_vnet(vnet1);

        consumer->m_toSync.emplace(key, swss::KeyOpFieldsValuesTuple(key, SET_COMMAND, {{"pb", validRoute.SerializeAsString()}}));
        consumer->m_toSync.emplace(key, swss::KeyOpFieldsValuesTuple(key, SET_COMMAND, {{"pb", invalidRoute.SerializeAsString()}}));

        EXPECT_CALL(*mock_sai_dash_outbound_routing_api, create_outbound_routing_entries).Times(1);
        m_DashRouteOrch->doTaskRouteTable(*consumer);

        EXPECT_TRUE(consumer->m_toSync.empty());
    }

    TEST_F(DashRouteOrchTest, InboundRouteMissingVnet)
    {
        dash::route_rule::RouteRule rule;
        rule.set_pa_validation(true);
        rule.set_vnet("NON_EXISTENT_VNET");

        EXPECT_CALL(*mock_sai_dash_inbound_routing_api, create_inbound_routing_entries).Times(0);
        SetDashTable(APP_DASH_ROUTE_RULE_TABLE_NAME, eni1 + ":5555:10.0.1.0/24", rule, true, true);
    }

    TEST_F(DashRouteOrchTest, InboundRouteSaiRemoveFailureNotRetried)
    {
        AddInboundRoutingEntry();

        std::vector<sai_status_t> exp_status = {SAI_STATUS_INVALID_PARAMETER};
        EXPECT_CALL(*mock_sai_dash_inbound_routing_api, remove_inbound_routing_entries)
            .WillOnce(DoAll(SetArrayArgument<3>(exp_status.begin(), exp_status.end()), Return(SAI_STATUS_SUCCESS)));
        RemoveInboundRoutingEntry(true);
    }

    TEST_F(DashRouteOrchTest, InboundRouteUnknownOp)
    {
        auto consumer = CreateDashRouteConsumer(APP_DASH_ROUTE_RULE_TABLE_NAME);
        consumer->m_toSync.emplace(
            eni1 + ":5555:10.0.0.0/24",
            swss::KeyOpFieldsValuesTuple(eni1 + ":5555:10.0.0.0/24", "UNKNOWN", {}));

        m_DashRouteOrch->doTaskRouteRuleTable(*consumer);

        EXPECT_TRUE(consumer->m_toSync.empty());
    }

    TEST_F(DashRouteOrchTest, InboundRouteDuplicateSetClearsContext)
    {
        auto consumer = CreateDashRouteConsumer(APP_DASH_ROUTE_RULE_TABLE_NAME);
        std::string key = eni1 + ":5555:10.0.0.0/24";

        dash::route_rule::RouteRule validRule;
        validRule.set_pa_validation(true);

        dash::route_rule::RouteRule invalidRule;
        invalidRule.set_pa_validation(true);
        invalidRule.set_vnet("NON_EXISTENT_VNET");

        consumer->m_toSync.emplace(key, swss::KeyOpFieldsValuesTuple(key, SET_COMMAND, {{"pb", validRule.SerializeAsString()}}));
        consumer->m_toSync.emplace(key, swss::KeyOpFieldsValuesTuple(key, SET_COMMAND, {{"pb", invalidRule.SerializeAsString()}}));

        EXPECT_CALL(*mock_sai_dash_inbound_routing_api, create_inbound_routing_entries).Times(1);
        m_DashRouteOrch->doTaskRouteRuleTable(*consumer);

        EXPECT_TRUE(consumer->m_toSync.empty());
    }

    TEST_F(DashRouteOrchTest, RouteGroupSaiCreateFailure)
    {
        dash::route_group::RouteGroup route_group;
        route_group.set_version("1");
        route_group.set_guid("group_guid");

        EXPECT_CALL(*mock_sai_dash_outbound_routing_api, create_outbound_routing_group).WillOnce(Return(SAI_STATUS_INVALID_PARAMETER));
        SetDashTable(APP_DASH_ROUTE_GROUP_TABLE_NAME, route_group1, route_group, true, true);

        EXPECT_EQ(m_DashRouteOrch->getRouteGroupOid(route_group1), SAI_NULL_OBJECT_ID);
    }

    TEST_F(DashRouteOrchTest, RouteGroupRemoveSaiFailureNotInUse)
    {
        AddOutboundRoutingGroup();

        EXPECT_CALL(*mock_sai_dash_outbound_routing_api, remove_outbound_routing_group(_)).WillOnce(Return(SAI_STATUS_INVALID_PARAMETER));
        SetDashTable(APP_DASH_ROUTE_GROUP_TABLE_NAME, route_group1, dash::route_group::RouteGroup(), false, true);

        EXPECT_NE(m_DashRouteOrch->getRouteGroupOid(route_group1), SAI_NULL_OBJECT_ID);
    }

    TEST_F(DashRouteOrchTest, RemoveBoundRouteGroup)
    {
        AddOutboundRoutingGroup();
        m_DashRouteOrch->bindRouteGroup(route_group1);

        EXPECT_CALL(*mock_sai_dash_outbound_routing_api, remove_outbound_routing_group(_)).Times(0);
        SetDashTable(APP_DASH_ROUTE_GROUP_TABLE_NAME, route_group1, dash::route_group::RouteGroup(), false, true);

        EXPECT_NE(m_DashRouteOrch->getRouteGroupOid(route_group1), SAI_NULL_OBJECT_ID);
        m_DashRouteOrch->unbindRouteGroup(route_group1);
    }

    TEST_F(DashRouteOrchTest, RouteGroupUnknownOp)
    {
        auto consumer = CreateDashRouteConsumer(APP_DASH_ROUTE_GROUP_TABLE_NAME);
        consumer->m_toSync.emplace(route_group1, swss::KeyOpFieldsValuesTuple(route_group1, "UNKNOWN", {}));

        m_DashRouteOrch->doTaskRouteGroupTable(*consumer);

        EXPECT_TRUE(consumer->m_toSync.empty());
    }
}
