#include "p4orch.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <string>

#include "mock_response_publisher.h"
#include "mock_sai_bridge.h"
#include "mock_sai_hostif.h"
#include "mock_sai_ipmc.h"
#include "mock_sai_neighbor.h"
#include "mock_sai_next_hop.h"
#include "mock_sai_route.h"
#include "mock_sai_router_interface.h"
#include "mock_sai_rpf_group.h"
#include "mock_sai_switch.h"

using ::p4orch::kTableKeyDelimiter;

extern P4Orch* gP4Orch;
extern VRFOrch* gVrfOrch;
extern std::unique_ptr<MockResponsePublisher> gMockResponsePublisher;
extern VRFOrch* gVrfOrch;
extern swss::DBConnector* gAppDb;
extern sai_hostif_api_t* sai_hostif_api;
extern sai_switch_api_t* sai_switch_api;
extern sai_bridge_api_t* sai_bridge_api;
extern sai_ipmc_api_t* sai_ipmc_api;
extern sai_router_interface_api_t* sai_router_intfs_api;
extern sai_neighbor_api_t* sai_neighbor_api;
extern sai_next_hop_api_t* sai_next_hop_api;
extern sai_route_api_t* sai_route_api;
extern sai_rpf_group_api_t* sai_rpf_group_api;

using ::testing::_;
using ::testing::DoAll;
using ::testing::Eq;
using ::testing::InSequence;
using ::testing::NiceMock;
using ::testing::Return;
using ::testing::SetArrayArgument;
using ::testing::StrictMock;

class P4OrchTest : public ::testing::Test {
 protected:
  P4OrchTest() {
    mock_sai_hostif = &mock_sai_hostif_;
    sai_hostif_api->create_hostif_trap = mock_create_hostif_trap;
    sai_hostif_api->create_hostif_table_entry = mock_create_hostif_table_entry;
    mock_sai_switch = &mock_sai_switch_;
    sai_switch_api->get_switch_attribute = mock_get_switch_attribute;
    mock_sai_router_intf = &mock_sai_router_intf_;
    sai_router_intfs_api->create_router_interface =
        mock_create_router_interface;
    sai_router_intfs_api->remove_router_interface =
        mock_remove_router_interface;
    sai_router_intfs_api->set_router_interface_attribute =
        mock_set_router_interface_attribute;
    sai_router_intfs_api->get_router_interface_attribute =
        mock_get_router_interface_attribute;
    mock_sai_neighbor = &mock_sai_neighbor_;
    sai_neighbor_api->create_neighbor_entry = mock_create_neighbor_entry;
    sai_neighbor_api->remove_neighbor_entry = mock_remove_neighbor_entry;
    sai_neighbor_api->set_neighbor_entry_attribute =
        mock_set_neighbor_entry_attribute;
    sai_neighbor_api->get_neighbor_entry_attribute =
        mock_get_neighbor_entry_attribute;
    mock_sai_next_hop = &mock_sai_next_hop_;
    sai_next_hop_api->create_next_hop = mock_create_next_hop;
    sai_next_hop_api->remove_next_hop = mock_remove_next_hop;
    sai_next_hop_api->set_next_hop_attribute = mock_set_next_hop_attribute;
    sai_next_hop_api->get_next_hop_attribute = mock_get_next_hop_attribute;
    mock_sai_route = &mock_sai_route_;
    sai_route_api->create_route_entry = create_route_entry;
    sai_route_api->remove_route_entry = remove_route_entry;
    sai_route_api->set_route_entry_attribute = set_route_entry_attribute;
    sai_route_api->get_route_entry_attribute = get_route_entry_attribute;
    sai_route_api->create_route_entries = create_route_entries;
    sai_route_api->remove_route_entries = remove_route_entries;
    sai_route_api->set_route_entries_attribute = set_route_entries_attribute;
    sai_route_api->get_route_entries_attribute = get_route_entries_attribute;
    mock_sai_ipmc = &mock_sai_ipmc_;
    sai_ipmc_api->create_ipmc_entry = mock_create_ipmc_entry;
    sai_ipmc_api->remove_ipmc_entry = mock_remove_ipmc_entry;
    sai_ipmc_api->set_ipmc_entry_attribute = mock_set_ipmc_entry_attribute;
    sai_ipmc_api->get_ipmc_entry_attribute = mock_get_ipmc_entry_attribute;
    mock_sai_rpf_group = &mock_sai_rpf_group_;
    sai_rpf_group_api->create_rpf_group = mock_create_rpf_group;
    sai_rpf_group_api->remove_rpf_group = mock_remove_rpf_group;
    sai_rpf_group_api->set_rpf_group_attribute = mock_set_rpf_group_attribute;
    sai_rpf_group_api->get_rpf_group_attribute = mock_get_rpf_group_attribute;
    sai_rpf_group_api->create_rpf_group_member = mock_create_rpf_group_member;
    sai_rpf_group_api->remove_rpf_group_member = mock_remove_rpf_group_member;
    sai_rpf_group_api->set_rpf_group_member_attribute =
        mock_set_rpf_group_member_attribute;
    sai_rpf_group_api->get_rpf_group_member_attribute =
        mock_get_rpf_group_member_attribute;
    mock_sai_bridge = &mock_sai_bridge_;
    sai_bridge_api->create_bridge = mock_create_bridge;
    sai_bridge_api->remove_bridge = mock_remove_bridge;
    sai_bridge_api->set_bridge_attribute = mock_set_bridge_attribute;
    sai_bridge_api->get_bridge_attribute = mock_get_bridge_attribute;
    sai_bridge_api->get_bridge_stats = mock_get_bridge_stats;
    sai_bridge_api->get_bridge_stats_ext = mock_get_bridge_stats_ext;
    sai_bridge_api->clear_bridge_stats = mock_clear_bridge_stats;
    sai_bridge_api->create_bridge_port = mock_create_bridge_port;
    sai_bridge_api->remove_bridge_port = mock_remove_bridge_port;
    sai_bridge_api->set_bridge_port_attribute = mock_set_bridge_port_attribute;
    sai_bridge_api->get_bridge_port_attribute = mock_get_bridge_port_attribute;
    sai_bridge_api->get_bridge_port_stats = mock_get_bridge_port_stats;
    sai_bridge_api->get_bridge_port_stats_ext = mock_get_bridge_port_stats_ext;
    sai_bridge_api->clear_bridge_port_stats = mock_clear_bridge_port_stats;

    copp_orch_ = new CoppOrch(gAppDb, APP_COPP_TABLE_NAME);
    std::vector<std::string> p4_tables{APP_P4RT_TABLE_NAME};
    gP4Orch = new P4Orch(gAppDb, p4_tables, gVrfOrch, copp_orch_);
    gMockResponsePublisher = std::make_unique<MockResponsePublisher>();
  }

  ~P4OrchTest() {
    delete gP4Orch;
    delete copp_orch_;
    gMockResponsePublisher.reset();
  }

  void HandleP4rtNotification(
      const std::vector<swss::FieldValueTuple>& values) {
    gP4Orch->handleP4rtNotification(values);
  }

  NiceMock<MockSaiHostif> mock_sai_hostif_;
  NiceMock<MockSaiSwitch> mock_sai_switch_;
  NiceMock<MockSaiRouterInterface> mock_sai_router_intf_;
  NiceMock<MockSaiNeighbor> mock_sai_neighbor_;
  NiceMock<MockSaiNextHop> mock_sai_next_hop_;
  NiceMock<MockSaiRoute> mock_sai_route_;
  NiceMock<MockSaiIpmc> mock_sai_ipmc_;
  NiceMock<MockSaiRpfGroup> mock_sai_rpf_group_;
  NiceMock<MockSaiBridge> mock_sai_bridge_;
  CoppOrch* copp_orch_;
};

TEST_F(P4OrchTest, ProcessInvalidEntry) {
  InSequence s;
  std::vector<swss::FieldValueTuple> values;
  values.push_back(swss::FieldValueTuple{"invalid", ""});
  values.push_back(swss::FieldValueTuple{"invalid:invalid", ""});
  std::vector<swss::FieldValueTuple> exp_values;
  EXPECT_CALL(*gMockResponsePublisher,
              publish(Eq(APP_P4RT_TABLE_NAME), Eq("invalid"), Eq(exp_values),
                      Eq(StatusCode::SWSS_RC_INVALID_PARAM), Eq(true)));
  EXPECT_CALL(
      *gMockResponsePublisher,
      publish(Eq(APP_P4RT_TABLE_NAME), Eq("invalid:invalid"), Eq(exp_values),
              Eq(StatusCode::SWSS_RC_INVALID_PARAM), Eq(true)));
  HandleP4rtNotification(values);
}

TEST_F(P4OrchTest, ProcessP4Notification) {
  InSequence s;
  std::vector<swss::FieldValueTuple> values;

  // Router interface
  const std::string ritf_key =
      std::string(APP_P4RT_ROUTER_INTERFACE_TABLE_NAME) + kTableKeyDelimiter +
      "{\"match/router_interface_id\":\"intf-3/4\"}";
  std::vector<swss::FieldValueTuple> ritf_attrs;
  ritf_attrs.push_back(
      swss::FieldValueTuple{prependParamField(p4orch::kPort), "Ethernet1"});
  ritf_attrs.push_back(swss::FieldValueTuple{prependParamField(p4orch::kSrcMac),
                                             "00:01:02:03:04:05"});
  values.push_back(
      swss::FieldValueTuple{ritf_key, swss::JSon::buildJson(ritf_attrs)});

  // Neighbor
  const std::string neighbor_key = std::string(APP_P4RT_NEIGHBOR_TABLE_NAME) +
                                   kTableKeyDelimiter +
                                   "{\"match/router_interface_id\":\"intf-3/"
                                   "4\",\"match/neighbor_id\":\"10.0.0.22\"}";
  std::vector<swss::FieldValueTuple> neighbor_attrs;
  neighbor_attrs.push_back(swss::FieldValueTuple{
      prependParamField(p4orch::kDstMac), "00:01:02:03:04:05"});
  values.push_back(swss::FieldValueTuple{
      neighbor_key, swss::JSon::buildJson(neighbor_attrs)});

  // Nexthop
  const std::string nexthop_key =
      std::string(APP_P4RT_NEXTHOP_TABLE_NAME) + kTableKeyDelimiter +
      "{\"match/nexthop_id\":\"ju1u32m1.atl11:qe-3/7\"}";
  std::vector<swss::FieldValueTuple> nexthop_attrs;
  nexthop_attrs.push_back(
      swss::FieldValueTuple{p4orch::kAction, p4orch::kSetIpNexthop});
  nexthop_attrs.push_back(swss::FieldValueTuple{
      prependParamField(p4orch::kNeighborId), "10.0.0.22"});
  nexthop_attrs.push_back(swss::FieldValueTuple{
      prependParamField(p4orch::kRouterInterfaceId), "intf-3/4"});
  values.push_back(
      swss::FieldValueTuple{nexthop_key, swss::JSon::buildJson(nexthop_attrs)});

  // Route
  const std::string route_key =
      std::string(APP_P4RT_IPV4_TABLE_NAME) + kTableKeyDelimiter +
      "{\"match/vrf_id\":\"b4-traffic\",\"match/ipv4_dst\":\"10.11.12.0/24\"}";
  std::vector<swss::FieldValueTuple> route_attrs;
  route_attrs.push_back(
      swss::FieldValueTuple{p4orch::kAction, p4orch::kSetNexthopId});
  route_attrs.push_back(swss::FieldValueTuple{
      prependParamField(p4orch::kNexthopId), "ju1u32m1.atl11:qe-3/7"});
  values.push_back(
      swss::FieldValueTuple{route_key, swss::JSon::buildJson(route_attrs)});

  // Delete
  values.push_back(swss::FieldValueTuple{ritf_key, ""});
  values.push_back(swss::FieldValueTuple{neighbor_key, ""});
  values.push_back(swss::FieldValueTuple{nexthop_key, ""});
  values.push_back(swss::FieldValueTuple{route_key, ""});

  EXPECT_CALL(*gMockResponsePublisher,
              publish(Eq(APP_P4RT_TABLE_NAME), Eq(ritf_key), Eq(ritf_attrs),
                      Eq(StatusCode::SWSS_RC_SUCCESS), Eq(true)));
  EXPECT_CALL(
      *gMockResponsePublisher,
      publish(Eq(APP_P4RT_TABLE_NAME), Eq(neighbor_key), Eq(neighbor_attrs),
              Eq(StatusCode::SWSS_RC_SUCCESS), Eq(true)));
  EXPECT_CALL(
      *gMockResponsePublisher,
      publish(Eq(APP_P4RT_TABLE_NAME), Eq(nexthop_key), Eq(nexthop_attrs),
              Eq(StatusCode::SWSS_RC_SUCCESS), Eq(true)));
  EXPECT_CALL(*gMockResponsePublisher,
              publish(Eq(APP_P4RT_TABLE_NAME), Eq(route_key), Eq(route_attrs),
                      Eq(StatusCode::SWSS_RC_SUCCESS), Eq(true)));
  std::vector<swss::FieldValueTuple> exp_values;
  EXPECT_CALL(*gMockResponsePublisher,
              publish(Eq(APP_P4RT_TABLE_NAME), Eq(route_key), Eq(exp_values),
                      Eq(StatusCode::SWSS_RC_SUCCESS), Eq(true)));
  EXPECT_CALL(*gMockResponsePublisher,
              publish(Eq(APP_P4RT_TABLE_NAME), Eq(nexthop_key), Eq(exp_values),
                      Eq(StatusCode::SWSS_RC_SUCCESS), Eq(true)));
  EXPECT_CALL(*gMockResponsePublisher,
              publish(Eq(APP_P4RT_TABLE_NAME), Eq(neighbor_key), Eq(exp_values),
                      Eq(StatusCode::SWSS_RC_SUCCESS), Eq(true)));
  EXPECT_CALL(*gMockResponsePublisher,
              publish(Eq(APP_P4RT_TABLE_NAME), Eq(ritf_key), Eq(exp_values),
                      Eq(StatusCode::SWSS_RC_SUCCESS), Eq(true)));
  HandleP4rtNotification(values);
}

TEST_F(P4OrchTest, ProcessP4NotificationStopOnFirstFailure) {
  InSequence s;
  std::vector<swss::FieldValueTuple> values;

  // Router interface
  const std::string ritf_key =
      std::string(APP_P4RT_ROUTER_INTERFACE_TABLE_NAME) + kTableKeyDelimiter +
      "{\"match/router_interface_id\":\"intf-3/4\"}";
  std::vector<swss::FieldValueTuple> ritf_attrs;
  ritf_attrs.push_back(
      swss::FieldValueTuple{prependParamField(p4orch::kPort), "Ethernet1"});
  ritf_attrs.push_back(swss::FieldValueTuple{prependParamField(p4orch::kSrcMac),
                                             "00:01:02:03:04:05"});
  values.push_back(
      swss::FieldValueTuple{ritf_key, swss::JSon::buildJson(ritf_attrs)});

  // Neighbor
  const std::string neighbor_key = std::string(APP_P4RT_NEIGHBOR_TABLE_NAME) +
                                   kTableKeyDelimiter +
                                   "{\"match/router_interface_id\":\"intf-3/"
                                   "4\",\"match/neighbor_id\":\"10.0.0.22\"}";
  std::vector<swss::FieldValueTuple> neighbor_attrs;
  neighbor_attrs.push_back(swss::FieldValueTuple{
      prependParamField(p4orch::kDstMac), "00:01:02:03:04:05"});
  values.push_back(swss::FieldValueTuple{
      neighbor_key, swss::JSon::buildJson(neighbor_attrs)});

  // Nexthop
  const std::string nexthop_key =
      std::string(APP_P4RT_NEXTHOP_TABLE_NAME) + kTableKeyDelimiter +
      "{\"match/nexthop_id\":\"ju1u32m1.atl11:qe-3/7\"}";
  std::vector<swss::FieldValueTuple> nexthop_attrs;
  nexthop_attrs.push_back(
      swss::FieldValueTuple{p4orch::kAction, p4orch::kSetIpNexthop});
  nexthop_attrs.push_back(swss::FieldValueTuple{
      prependParamField(p4orch::kNeighborId), "10.0.0.22"});
  nexthop_attrs.push_back(swss::FieldValueTuple{
      prependParamField(p4orch::kRouterInterfaceId), "intf-3/4"});
  values.push_back(
      swss::FieldValueTuple{nexthop_key, swss::JSon::buildJson(nexthop_attrs)});

  // Route
  const std::string route_key =
      std::string(APP_P4RT_IPV4_TABLE_NAME) + kTableKeyDelimiter +
      "{\"match/vrf_id\":\"b4-traffic\",\"match/ipv4_dst\":\"10.11.12.0/24\"}";
  std::vector<swss::FieldValueTuple> route_attrs;
  route_attrs.push_back(
      swss::FieldValueTuple{p4orch::kAction, p4orch::kSetNexthopId});
  route_attrs.push_back(swss::FieldValueTuple{
      prependParamField(p4orch::kNexthopId), "ju1u32m1.atl11:qe-3/7"});
  values.push_back(
      swss::FieldValueTuple{route_key, swss::JSon::buildJson(route_attrs)});

  // Delete
  values.push_back(swss::FieldValueTuple{ritf_key, ""});
  values.push_back(swss::FieldValueTuple{neighbor_key, ""});
  values.push_back(swss::FieldValueTuple{nexthop_key, ""});
  values.push_back(swss::FieldValueTuple{route_key, ""});

  EXPECT_CALL(*gMockResponsePublisher,
              publish(Eq(APP_P4RT_TABLE_NAME), Eq(ritf_key), Eq(ritf_attrs),
                      Eq(StatusCode::SWSS_RC_SUCCESS), Eq(true)));
  EXPECT_CALL(
      *gMockResponsePublisher,
      publish(Eq(APP_P4RT_TABLE_NAME), Eq(neighbor_key), Eq(neighbor_attrs),
              Eq(StatusCode::SWSS_RC_SUCCESS), Eq(true)));
  EXPECT_CALL(
      *gMockResponsePublisher,
      publish(Eq(APP_P4RT_TABLE_NAME), Eq(nexthop_key), Eq(nexthop_attrs),
              Eq(StatusCode::SWSS_RC_SUCCESS), Eq(true)));
  std::vector<sai_status_t> exp_status{SAI_STATUS_FAILURE};
  EXPECT_CALL(mock_sai_route_, create_route_entries(_, _, _, _, _, _))
      .WillOnce(DoAll(SetArrayArgument<5>(exp_status.begin(), exp_status.end()),
                      Return(SAI_STATUS_FAILURE)));
  EXPECT_CALL(*gMockResponsePublisher,
              publish(Eq(APP_P4RT_TABLE_NAME), Eq(route_key), Eq(route_attrs),
                      Eq(StatusCode::SWSS_RC_UNKNOWN), Eq(true)));
  std::vector<swss::FieldValueTuple> exp_values;
  EXPECT_CALL(*gMockResponsePublisher,
              publish(Eq(APP_P4RT_TABLE_NAME), Eq(ritf_key), Eq(exp_values),
                      Eq(StatusCode::SWSS_RC_NOT_EXECUTED), Eq(true)));
  EXPECT_CALL(*gMockResponsePublisher,
              publish(Eq(APP_P4RT_TABLE_NAME), Eq(neighbor_key), Eq(exp_values),
                      Eq(StatusCode::SWSS_RC_NOT_EXECUTED), Eq(true)));
  EXPECT_CALL(*gMockResponsePublisher,
              publish(Eq(APP_P4RT_TABLE_NAME), Eq(nexthop_key), Eq(exp_values),
                      Eq(StatusCode::SWSS_RC_NOT_EXECUTED), Eq(true)));
  EXPECT_CALL(*gMockResponsePublisher,
              publish(Eq(APP_P4RT_TABLE_NAME), Eq(route_key), Eq(exp_values),
                      Eq(StatusCode::SWSS_RC_NOT_EXECUTED), Eq(true)));
  HandleP4rtNotification(values);
}

TEST_F(P4OrchTest, ProcessP4NotificationStopOnFirstFailureDifferentTypes) {
  InSequence s;
  std::vector<swss::FieldValueTuple> values;

  // Router interface
  const std::string ritf_key_1 =
      std::string(APP_P4RT_ROUTER_INTERFACE_TABLE_NAME) + kTableKeyDelimiter +
      "{\"match/router_interface_id\":\"intf-1\"}";
  const std::string ritf_key_2 =
      std::string(APP_P4RT_ROUTER_INTERFACE_TABLE_NAME) + kTableKeyDelimiter +
      "{\"match/router_interface_id\":\"intf-2\"}";
  std::vector<swss::FieldValueTuple> ritf_attrs;
  ritf_attrs.push_back(
      swss::FieldValueTuple{prependParamField(p4orch::kPort), "Ethernet1"});
  ritf_attrs.push_back(swss::FieldValueTuple{prependParamField(p4orch::kSrcMac),
                                             "00:01:02:03:04:05"});

  // Add
  values.push_back(
      swss::FieldValueTuple{ritf_key_1, swss::JSon::buildJson(ritf_attrs)});
  values.push_back(
      swss::FieldValueTuple{ritf_key_2, swss::JSon::buildJson(ritf_attrs)});

  // Delete
  values.push_back(swss::FieldValueTuple{ritf_key_1, ""});
  values.push_back(swss::FieldValueTuple{ritf_key_2, ""});

  // Add
  values.push_back(
      swss::FieldValueTuple{ritf_key_1, swss::JSon::buildJson(ritf_attrs)});
  values.push_back(
      swss::FieldValueTuple{ritf_key_2, swss::JSon::buildJson(ritf_attrs)});

  // Delete
  values.push_back(swss::FieldValueTuple{ritf_key_1, ""});
  values.push_back(swss::FieldValueTuple{ritf_key_2, ""});

  EXPECT_CALL(*gMockResponsePublisher,
              publish(Eq(APP_P4RT_TABLE_NAME), Eq(ritf_key_1), Eq(ritf_attrs),
                      Eq(StatusCode::SWSS_RC_SUCCESS), Eq(true)));
  EXPECT_CALL(*gMockResponsePublisher,
              publish(Eq(APP_P4RT_TABLE_NAME), Eq(ritf_key_2), Eq(ritf_attrs),
                      Eq(StatusCode::SWSS_RC_SUCCESS), Eq(true)));
  EXPECT_CALL(mock_sai_router_intf_, remove_router_interface(_))
      .WillOnce(Return(SAI_STATUS_FAILURE));
  std::vector<swss::FieldValueTuple> exp_values;
  EXPECT_CALL(*gMockResponsePublisher,
              publish(Eq(APP_P4RT_TABLE_NAME), Eq(ritf_key_1), Eq(exp_values),
                      Eq(StatusCode::SWSS_RC_UNKNOWN), Eq(true)));
  EXPECT_CALL(*gMockResponsePublisher,
              publish(Eq(APP_P4RT_TABLE_NAME), Eq(ritf_key_2), Eq(exp_values),
                      Eq(StatusCode::SWSS_RC_NOT_EXECUTED), Eq(true)));
  EXPECT_CALL(*gMockResponsePublisher,
              publish(Eq(APP_P4RT_TABLE_NAME), Eq(ritf_key_1), Eq(ritf_attrs),
                      Eq(StatusCode::SWSS_RC_NOT_EXECUTED), Eq(true)));
  EXPECT_CALL(*gMockResponsePublisher,
              publish(Eq(APP_P4RT_TABLE_NAME), Eq(ritf_key_2), Eq(ritf_attrs),
                      Eq(StatusCode::SWSS_RC_NOT_EXECUTED), Eq(true)));
  EXPECT_CALL(*gMockResponsePublisher,
              publish(Eq(APP_P4RT_TABLE_NAME), Eq(ritf_key_1), Eq(exp_values),
                      Eq(StatusCode::SWSS_RC_NOT_EXECUTED), Eq(true)));
  EXPECT_CALL(*gMockResponsePublisher,
              publish(Eq(APP_P4RT_TABLE_NAME), Eq(ritf_key_2), Eq(exp_values),
                      Eq(StatusCode::SWSS_RC_NOT_EXECUTED), Eq(true)));
  HandleP4rtNotification(values);
}
