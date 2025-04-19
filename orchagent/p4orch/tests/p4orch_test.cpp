#include "p4orch.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <string>

#include "mock_response_publisher.h"
#include "mock_sai_hostif.h"
#include "mock_sai_neighbor.h"
#include "mock_sai_next_hop.h"
#include "mock_sai_route.h"
#include "mock_sai_router_interface.h"
#include "mock_sai_switch.h"

using ::p4orch::kTableKeyDelimiter;

extern P4Orch* gP4Orch;
extern VRFOrch* gVrfOrch;
extern std::unique_ptr<MockResponsePublisher> gMockResponsePublisher;
extern VRFOrch* gVrfOrch;
extern swss::DBConnector* gAppDb;
extern sai_hostif_api_t* sai_hostif_api;
extern sai_switch_api_t* sai_switch_api;
extern sai_router_interface_api_t* sai_router_intfs_api;
extern sai_neighbor_api_t* sai_neighbor_api;
extern sai_next_hop_api_t* sai_next_hop_api;
extern sai_route_api_t* sai_route_api;

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
    copp_orch_ = new CoppOrch(gAppDb, APP_COPP_TABLE_NAME);
    std::vector<std::string> p4_tables{APP_P4RT_TABLE_NAME};
    gP4Orch = new P4Orch(gAppDb, p4_tables, nullptr, gVrfOrch, copp_orch_);
    gMockResponsePublisher = std::make_unique<MockResponsePublisher>();
  }

  ~P4OrchTest() {
    delete gP4Orch;
    delete copp_orch_;
    gMockResponsePublisher.reset();
  }

  void DoTask(ConsumerBase& consumer) { gP4Orch->doTask(consumer); }

  NiceMock<MockSaiHostif> mock_sai_hostif_;
  NiceMock<MockSaiSwitch> mock_sai_switch_;
  NiceMock<MockSaiRouterInterface> mock_sai_router_intf_;
  NiceMock<MockSaiNeighbor> mock_sai_neighbor_;
  NiceMock<MockSaiNextHop> mock_sai_next_hop_;
  NiceMock<MockSaiRoute> mock_sai_route_;
  CoppOrch* copp_orch_;
};

TEST_F(P4OrchTest, ProcessInvalidEntry) {
  InSequence s;
  ZmqServer zmq_server("endpoint");
  DBConnector db("APPL_DB", 0);
  ZmqConsumerStateTable* table =
      new ZmqConsumerStateTable(&db, APP_P4RT_TABLE_NAME, zmq_server,
                                TableConsumable::DEFAULT_POP_BATCH_SIZE, 0,
                                /*dbPersistence=*/false);
  ZmqConsumer consumer(table, nullptr, APP_P4RT_TABLE_NAME,
                       /*orderedQueue=*/true);
  consumer.m_queue.push_back(swss::KeyOpFieldsValuesTuple{
      "invalid", DEL_COMMAND, std::vector<swss::FieldValueTuple>{}});
  consumer.m_queue.push_back(swss::KeyOpFieldsValuesTuple{
      "invalid:invalid", DEL_COMMAND, std::vector<swss::FieldValueTuple>{}});
  std::vector<swss::FieldValueTuple> exp_values;
  EXPECT_CALL(*gMockResponsePublisher,
              publish(Eq(APP_P4RT_TABLE_NAME), Eq("invalid"), Eq(exp_values),
                      Eq(StatusCode::SWSS_RC_INVALID_PARAM), Eq(true)));
  EXPECT_CALL(
      *gMockResponsePublisher,
      publish(Eq(APP_P4RT_TABLE_NAME), Eq("invalid:invalid"), Eq(exp_values),
              Eq(StatusCode::SWSS_RC_INVALID_PARAM), Eq(true)));
  DoTask(consumer);
}

TEST_F(P4OrchTest, ProcessP4Notification) {
  InSequence s;
  ZmqServer zmq_server("endpoint");
  DBConnector db("APPL_DB", 0);
  ZmqConsumerStateTable* table =
      new ZmqConsumerStateTable(&db, APP_P4RT_TABLE_NAME, zmq_server,
                                TableConsumable::DEFAULT_POP_BATCH_SIZE, 0,
                                /*dbPersistence=*/false);
  ZmqConsumer consumer(table, nullptr, APP_P4RT_TABLE_NAME,
                       /*orderedQueue=*/true);

  // Router interface
  const std::string ritf_key =
      std::string(APP_P4RT_ROUTER_INTERFACE_TABLE_NAME) + kTableKeyDelimiter +
      "{\"match/router_interface_id\":\"intf-3/4\"}";
  std::vector<swss::FieldValueTuple> ritf_attrs;
  ritf_attrs.push_back(
      swss::FieldValueTuple{prependParamField(p4orch::kPort), "Ethernet1"});
  ritf_attrs.push_back(swss::FieldValueTuple{prependParamField(p4orch::kSrcMac),
                                             "00:01:02:03:04:05"});

  consumer.m_queue.push_back(
      swss::KeyOpFieldsValuesTuple{ritf_key, SET_COMMAND, ritf_attrs});
  // Neighbor
  const std::string neighbor_key = std::string(APP_P4RT_NEIGHBOR_TABLE_NAME) +
                                   kTableKeyDelimiter +
                                   "{\"match/router_interface_id\":\"intf-3/"
                                   "4\",\"match/neighbor_id\":\"10.0.0.22\"}";
  std::vector<swss::FieldValueTuple> neighbor_attrs;
  neighbor_attrs.push_back(swss::FieldValueTuple{
      prependParamField(p4orch::kDstMac), "00:01:02:03:04:05"});
  consumer.m_queue.push_back(
      swss::KeyOpFieldsValuesTuple{neighbor_key, SET_COMMAND, neighbor_attrs});

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
  consumer.m_queue.push_back(
      swss::KeyOpFieldsValuesTuple{nexthop_key, SET_COMMAND, nexthop_attrs});

  // Route
  const std::string route_key =
      std::string(APP_P4RT_IPV4_TABLE_NAME) + kTableKeyDelimiter +
      "{\"match/vrf_id\":\"b4-traffic\",\"match/ipv4_dst\":\"10.11.12.0/24\"}";
  std::vector<swss::FieldValueTuple> route_attrs;
  route_attrs.push_back(
      swss::FieldValueTuple{p4orch::kAction, p4orch::kSetNexthopId});
  route_attrs.push_back(swss::FieldValueTuple{
      prependParamField(p4orch::kNexthopId), "ju1u32m1.atl11:qe-3/7"});
  consumer.m_queue.push_back(
      swss::KeyOpFieldsValuesTuple{route_key, SET_COMMAND, route_attrs});

  // Delete
  consumer.m_queue.push_back(swss::KeyOpFieldsValuesTuple{
      ritf_key, DEL_COMMAND, std::vector<swss::FieldValueTuple>{}});
  consumer.m_queue.push_back(swss::KeyOpFieldsValuesTuple{
      neighbor_key, DEL_COMMAND, std::vector<swss::FieldValueTuple>{}});
  consumer.m_queue.push_back(swss::KeyOpFieldsValuesTuple{
      nexthop_key, DEL_COMMAND, std::vector<swss::FieldValueTuple>{}});
  consumer.m_queue.push_back(swss::KeyOpFieldsValuesTuple{
      route_key, DEL_COMMAND, std::vector<swss::FieldValueTuple>{}});

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
  DoTask(consumer);
}
