#include "l3_multicast_manager.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <deque>
#include <functional>
#include <map>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#include "ipprefix.h"
#include "mock_response_publisher.h"
#include "mock_sai_ipmc_group.h"
#include "mock_sai_router_interface.h"
#include "p4orch.h"
#include "p4orch/p4orch_util.h"
#include "portsorch.h"
#include "return_code.h"
#include "swssnet.h"
#include "vrforch.h"

extern "C" {
#include "sai.h"
}

using ::p4orch::kTableKeyDelimiter;

using ::testing::_;
using ::testing::DoAll;
using ::testing::Eq;
using ::testing::Return;
using ::testing::SetArgPointee;
using ::testing::SetArrayArgument;
using ::testing::StrictMock;

extern sai_object_id_t gSwitchId;
extern sai_object_id_t gVirtualRouterId;
extern sai_object_id_t gVrfOid;
// extern sai_ipmc_api_t* sai_ipmc_api;
extern sai_ipmc_group_api_t* sai_ipmc_group_api;
extern sai_router_interface_api_t* sai_router_intfs_api;
extern char* gVrfName;
extern PortsOrch* gPortsOrch;
extern VRFOrch* gVrfOrch;

namespace p4orch {

namespace {
// Helpful place for constant and/or test functions
constexpr char* kSrcMac1 = "00:01:02:03:04:05";
constexpr char* kSrcMac2 = "00:0a:0b:0c:0d:0e";
constexpr char* kSrcMac3 = "10:20:30:40:50:60";
constexpr char* kSrcMac4 = "15:25:35:45:55:65";
constexpr char* kSrcMac5 = "10:20:30:40:50:60";

constexpr sai_object_id_t kRifOid1 = 0x123456;
constexpr sai_object_id_t kRifOid2 = 0x22789a;
constexpr sai_object_id_t kRifOid3 = 0x33feed;
constexpr sai_object_id_t kRifOid5 = 0x55abcd;

constexpr sai_object_id_t kGroupOid1 = 0x1;
constexpr sai_object_id_t kGroupOid2 = 0x2;
constexpr sai_object_id_t kGroupOid3 = 0x3;

constexpr sai_object_id_t kGroupMemberOid1 = 0x11;
constexpr sai_object_id_t kGroupMemberOid2 = 0x12;
constexpr sai_object_id_t kGroupMemberOid3 = 0x13;

}  // namespace

class L3MulticastManagerTest : public ::testing::Test {
 protected:
  L3MulticastManagerTest()
      : l3_multicast_manager_(&p4_oid_mapper_, gVrfOrch, &publisher_) {}

  P4MulticastRouterInterfaceEntry GenerateP4MulticastRouterInterfaceEntry(
      const std::string& multicast_replica_port,
      const std::string& multicast_replica_instance,
      const swss::MacAddress src_mac,
      const std::string& multicast_metadata = "") {
    P4MulticastRouterInterfaceEntry router_interface_entry = {};
    router_interface_entry.multicast_replica_port = multicast_replica_port;
    router_interface_entry.multicast_replica_instance =
        multicast_replica_instance;
    router_interface_entry.src_mac = src_mac;
    router_interface_entry.multicast_metadata = multicast_metadata;
    router_interface_entry.multicast_router_interface_entry_key =
        KeyGenerator::generateMulticastRouterInterfaceKey(
            router_interface_entry.multicast_replica_port,
            router_interface_entry.multicast_replica_instance);
    return router_interface_entry;
  }

  P4MulticastReplicationEntry GenerateP4MulticastReplicationEntry(
      const std::string& multicast_group_id,
      const std::string& multicast_replica_port,
      const std::string& multicast_replica_instance,
      const std::string& multicast_metadata = "") {
    P4MulticastReplicationEntry replication_entry = {};
    replication_entry.multicast_group_id = multicast_group_id;
    replication_entry.multicast_replica_port = multicast_replica_port;
    replication_entry.multicast_replica_instance = multicast_replica_instance;
    replication_entry.multicast_metadata = multicast_metadata;

    replication_entry.multicast_replication_key =
        KeyGenerator::generateMulticastReplicationKey(
            replication_entry.multicast_group_id,
            replication_entry.multicast_replica_port,
            replication_entry.multicast_replica_instance);
    return replication_entry;
  }

  void VerifyP4MulticastRouterInterfaceEntryEqual(
      const P4MulticastRouterInterfaceEntry& x,
      const P4MulticastRouterInterfaceEntry& y) {
    EXPECT_EQ(x.multicast_router_interface_entry_key,
              y.multicast_router_interface_entry_key);
    EXPECT_EQ(x.multicast_replica_port, y.multicast_replica_port);
    EXPECT_EQ(x.multicast_replica_instance, y.multicast_replica_instance);
    EXPECT_EQ(
        0, memcmp(x.src_mac.getMac(), y.src_mac.getMac(), sizeof(sai_mac_t)));
    EXPECT_EQ(x.router_interface_oid, y.router_interface_oid);
  }

  void VerifyP4MulticastReplicationEntryEqual(
      const P4MulticastReplicationEntry& x,
      const P4MulticastReplicationEntry& y) {
    EXPECT_EQ(x.multicast_replication_key, y.multicast_replication_key);
    EXPECT_EQ(x.multicast_group_id, y.multicast_group_id);
    EXPECT_EQ(x.multicast_replica_port, y.multicast_replica_port);
    EXPECT_EQ(x.multicast_replica_instance, y.multicast_replica_instance);
    EXPECT_EQ(x.multicast_group_oid, y.multicast_group_oid);
    EXPECT_EQ(x.multicast_group_member_oid, y.multicast_group_member_oid);
  }

  void SetUp() override {
    mock_sai_router_intf = &mock_sai_router_intf_;
    sai_router_intfs_api->create_router_interface =
        mock_create_router_interface;

    mock_sai_ipmc_group = &mock_sai_ipmc_group_;
    sai_ipmc_group_api->create_ipmc_group = mock_create_ipmc_group;
    sai_ipmc_group_api->remove_ipmc_group = mock_remove_ipmc_group;
    sai_ipmc_group_api->create_ipmc_group_member =
        mock_create_ipmc_group_member;
    sai_ipmc_group_api->remove_ipmc_group_member =
        mock_remove_ipmc_group_member;
    sai_ipmc_group_api->set_ipmc_group_member_attribute =
        mock_set_ipmc_group_member_attribute;
    sai_ipmc_group_api->get_ipmc_group_member_attribute =
        mock_get_ipmc_group_member_attribute;
  }

  void Enqueue(const std::string& table_name,
               const swss::KeyOpFieldsValuesTuple& entry) {
    l3_multicast_manager_.enqueue(table_name, entry);
  }

  ReturnCode Drain(bool failure_before) {
    if (failure_before) {
      l3_multicast_manager_.drainWithNotExecuted();
      return ReturnCode(StatusCode::SWSS_RC_NOT_EXECUTED);
    }
    return l3_multicast_manager_.drain();
  }

  std::string VerifyState(const std::string& key,
                          const std::vector<swss::FieldValueTuple>& tuple) {
    return l3_multicast_manager_.verifyState(key, tuple);
  }

  std::string VerifyMulticastRouterInterfaceStateCache(
      const P4MulticastRouterInterfaceEntry& app_db_entry,
      const P4MulticastRouterInterfaceEntry* multicast_router_interface_entry) {
    return l3_multicast_manager_.verifyMulticastRouterInterfaceStateCache(
        app_db_entry, multicast_router_interface_entry);
  }

  std::string VerifyMulticastReplicationStateCache(
      const P4MulticastReplicationEntry& app_db_entry,
      const P4MulticastReplicationEntry* multicast_replication_entry) {
    return l3_multicast_manager_.verifyMulticastReplicationStateCache(
        app_db_entry, multicast_replication_entry);
  }

  std::string VerifyMulticastReplicationStateAsicDb(
      const P4MulticastReplicationEntry* multicast_replication_entry) {
    return l3_multicast_manager_.verifyMulticastReplicationStateAsicDb(
        multicast_replication_entry);
  }

  ReturnCodeOr<P4MulticastRouterInterfaceEntry>
  DeserializeMulticastRouterInterfaceEntry(
      const std::string& key,
      const std::vector<swss::FieldValueTuple>& attributes,
      const std::string& table_name) {
    return l3_multicast_manager_.deserializeMulticastRouterInterfaceEntry(
        key, attributes, table_name);
  }

  ReturnCodeOr<P4MulticastReplicationEntry>
  DeserializeMulticastReplicationEntry(
      const std::string& key,
      const std::vector<swss::FieldValueTuple>& attributes,
      const std::string& table_name) {
    return l3_multicast_manager_.deserializeMulticastReplicationEntry(
        key, attributes, table_name);
  }

  ReturnCode ValidateMulticastRouterInterfaceEntry(
      const P4MulticastRouterInterfaceEntry& multicast_router_interface_entry,
      const std::string& operation) {
    return l3_multicast_manager_.validateMulticastRouterInterfaceEntry(
        multicast_router_interface_entry, operation);
  }

  ReturnCode ValidateMulticastReplicationEntry(
      const P4MulticastReplicationEntry& multicast_replication_entry,
      const std::string& operation) {
    return l3_multicast_manager_.validateMulticastReplicationEntry(
        multicast_replication_entry, operation);
  }

  ReturnCode ProcessMulticastRouterInterfaceEntries(
      std::vector<P4MulticastRouterInterfaceEntry>& entries,
      const std::deque<swss::KeyOpFieldsValuesTuple>& tuple_list,
      const std::string& op, bool update) {
    return l3_multicast_manager_.processMulticastRouterInterfaceEntries(
        entries, tuple_list, op, update);
  }

  ReturnCode CreateRouterInterface(const std::string& rif_key,
                                   P4MulticastRouterInterfaceEntry& entry,
                                   sai_object_id_t* rif_oid) {
    return l3_multicast_manager_.createRouterInterface(rif_key, entry, rif_oid);
  }

  ReturnCode DeleteRouterInterface(const std::string& rif_key,
                                   sai_object_id_t rif_oid) {
    return l3_multicast_manager_.deleteRouterInterface(rif_key, rif_oid);
  }

  ReturnCode CreateMulticastGroup(P4MulticastReplicationEntry& entry,
                                  sai_object_id_t* mcast_group_oid) {
    return l3_multicast_manager_.createMulticastGroup(entry, mcast_group_oid);
  }

  ReturnCode DeleteMulticastGroup(const std::string multicast_group_id,
                                  sai_object_id_t mcast_group_oid) {
    return l3_multicast_manager_.deleteMulticastGroup(multicast_group_id,
                                                      mcast_group_oid);
  }

  ReturnCode CreateMulticastGroupMember(
      const P4MulticastReplicationEntry& entry, const sai_object_id_t rif_oid,
      sai_object_id_t* mcast_group_member_oid) {
    return l3_multicast_manager_.createMulticastGroupMember(
        entry, rif_oid, mcast_group_member_oid);
  }

  std::vector<ReturnCode> AddMulticastRouterInterfaceEntries(
      std::vector<P4MulticastRouterInterfaceEntry>& entries) {
    return l3_multicast_manager_.addMulticastRouterInterfaceEntries(entries);
  }

  std::vector<ReturnCode> UpdateMulticastRouterInterfaceEntries(
      std::vector<P4MulticastRouterInterfaceEntry>& entries) {
    return l3_multicast_manager_.updateMulticastRouterInterfaceEntries(entries);
  }

  std::vector<ReturnCode> DeleteMulticastRouterInterfaceEntries(
      std::vector<P4MulticastRouterInterfaceEntry>& entries) {
    return l3_multicast_manager_.deleteMulticastRouterInterfaceEntries(entries);
  }

  std::vector<ReturnCode> AddMulticastReplicationEntries(
      std::vector<P4MulticastReplicationEntry>& entries) {
    return l3_multicast_manager_.addMulticastReplicationEntries(entries);
  }

  std::vector<ReturnCode> DeleteMulticastReplicationEntries(
      std::vector<P4MulticastReplicationEntry>& entries) {
    return l3_multicast_manager_.deleteMulticastReplicationEntries(entries);
  }

  P4MulticastRouterInterfaceEntry* GetMulticastRouterInterfaceEntry(
      const std::string& multicast_router_interface_entry_key) {
    return l3_multicast_manager_.getMulticastRouterInterfaceEntry(
        multicast_router_interface_entry_key);
  }

  P4MulticastReplicationEntry* GetMulticastReplicationEntry(
      const std::string& multicast_replication_entry_key) {
    return l3_multicast_manager_.getMulticastReplicationEntry(
        multicast_replication_entry_key);
  }

  sai_object_id_t GetRifOid(
      const P4MulticastRouterInterfaceEntry* multicast_router_interface_entry) {
    return l3_multicast_manager_.getRifOid(multicast_router_interface_entry);
  }

  // Unnatural function to force an internal error.
  void ForceRemoveRifKey(const std::string& rif_key) {
    l3_multicast_manager_.m_rifOids.erase(rif_key);
  }

  // Unnatural function to force an internal error.
  void ForceRemoveRifOid(const sai_object_id_t rif_oid) {
    l3_multicast_manager_.m_rifOidToRouterInterfaceEntries.erase(rif_oid);
  }

  // Unnatural function to force an internal error.
  void ForceClearRifVector(const sai_object_id_t rif_oid) {
    l3_multicast_manager_.m_rifOidToRouterInterfaceEntries[rif_oid].clear();
  }

  // Unnatural function to force an internal error.
  void ForceRemoveGroupMembers(const std::string multicast_group_id) {
    l3_multicast_manager_.m_multicastGroupMembers.erase(multicast_group_id);
  }

  // Unnatural function to force an internal error.
  void ForceRemoveSpecificGroupMember(const std::string multicast_group_id,
                                      const std::string key) {
    l3_multicast_manager_.m_multicastGroupMembers[multicast_group_id].erase(
        key);
  }

  StrictMock<MockSaiRouterInterface> mock_sai_router_intf_;
  StrictMock<MockSaiIpmcGroup> mock_sai_ipmc_group_;
  StrictMock<MockResponsePublisher> publisher_;
  P4OidMapper p4_oid_mapper_;
  L3MulticastManager l3_multicast_manager_;
};

TEST_F(L3MulticastManagerTest, DeserializeMulticastRouterInterfaceEntryTest) {
  std::string key = R"({"match/multicast_replica_port":"Ethernet2",)"
                    R"("match/multicast_replica_instance":"0x0"})";
  std::vector<swss::FieldValueTuple> attributes;
  attributes.push_back(
      swss::FieldValueTuple{p4orch::kAction, p4orch::kSetSrcMac});
  attributes.push_back(
      swss::FieldValueTuple{prependParamField(p4orch::kSrcMac), kSrcMac1});
  attributes.push_back(swss::FieldValueTuple{
      prependParamField(p4orch::kMulticastMetadata), "meta1"});
  attributes.push_back(
      swss::FieldValueTuple{p4orch::kControllerMetadata, "so_meta"});

  auto router_interface_entry_or = DeserializeMulticastRouterInterfaceEntry(
      key, attributes, APP_P4RT_MULTICAST_ROUTER_INTERFACE_TABLE_NAME);
  EXPECT_TRUE(router_interface_entry_or.ok());
  auto& router_interface_entry = *router_interface_entry_or;
  auto expect_entry = GenerateP4MulticastRouterInterfaceEntry(
      "Ethernet2", "0x0", swss::MacAddress(kSrcMac1), "meta1");
  VerifyP4MulticastRouterInterfaceEntryEqual(expect_entry,
                                             router_interface_entry);
}

TEST_F(L3MulticastManagerTest,
       DeserializeMulticastRouterInterfaceEntryMissingMatchFieldTest) {
  std::string key = R"({"match/multicast_replica_port":"Ethernet2"})";
  std::vector<swss::FieldValueTuple> attributes;
  attributes.push_back(
      swss::FieldValueTuple{p4orch::kAction, "unknown_action"});
  attributes.push_back(
      swss::FieldValueTuple{prependParamField(p4orch::kSrcMac), kSrcMac1});

  auto router_interface_entry_or = DeserializeMulticastRouterInterfaceEntry(
      key, attributes, APP_P4RT_MULTICAST_ROUTER_INTERFACE_TABLE_NAME);
  EXPECT_EQ(StatusCode::SWSS_RC_INVALID_PARAM,
            router_interface_entry_or.status());
}

TEST_F(L3MulticastManagerTest,
       DeserializeMulticastRouterInterfaceEntryUnknownActionTest) {
  std::string key = R"({"match/multicast_replica_port":"Ethernet2",)"
                    R"("match/multicast_replica_instance":"0x0"})";
  std::vector<swss::FieldValueTuple> attributes;
  attributes.push_back(
      swss::FieldValueTuple{p4orch::kAction, "unknown_action"});
  attributes.push_back(
      swss::FieldValueTuple{prependParamField(p4orch::kSrcMac), kSrcMac1});

  auto router_interface_entry_or = DeserializeMulticastRouterInterfaceEntry(
      key, attributes, APP_P4RT_MULTICAST_ROUTER_INTERFACE_TABLE_NAME);
  EXPECT_EQ(StatusCode::SWSS_RC_INVALID_PARAM,
            router_interface_entry_or.status());
}

TEST_F(L3MulticastManagerTest,
       DeserializeMulticastRouterInterfaceEntryExtraAttributeTest) {
  std::string key = R"({"match/multicast_replica_port":"Ethernet2",)"
                    R"("match/multicast_replica_instance":"0x0"})";
  std::vector<swss::FieldValueTuple> attributes;
  attributes.push_back(
      swss::FieldValueTuple{p4orch::kAction, p4orch::kSetSrcMac});
  attributes.push_back(
      swss::FieldValueTuple{prependParamField(p4orch::kSrcMac), kSrcMac1});
  attributes.push_back(swss::FieldValueTuple{"extra_attr", "extra_attr_val"});

  auto router_interface_entry_or = DeserializeMulticastRouterInterfaceEntry(
      key, attributes, APP_P4RT_MULTICAST_ROUTER_INTERFACE_TABLE_NAME);
  EXPECT_EQ(StatusCode::SWSS_RC_INVALID_PARAM,
            router_interface_entry_or.status());
}

TEST_F(L3MulticastManagerTest, DeserializeMulticastReplicationEntryTest) {
  std::string key = R"({"match/multicast_group_id":"0x1",)"
                    R"("match/multicast_replica_port":"Ethernet1",)"
                    R"("match/multicast_replica_instance":"0x0"})";
  std::vector<swss::FieldValueTuple> attributes;
  attributes.push_back(swss::FieldValueTuple{
      prependParamField(p4orch::kMulticastMetadata), "meta1"});
  attributes.push_back(
      swss::FieldValueTuple{p4orch::kControllerMetadata, "so_meta"});

  auto replication_entry_or = DeserializeMulticastReplicationEntry(
      key, attributes, APP_P4RT_REPLICATION_L2_MULTICAST_TABLE_NAME);
  EXPECT_TRUE(replication_entry_or.ok());
  auto& replication_entry = *replication_entry_or;
  auto expect_entry =
      GenerateP4MulticastReplicationEntry("0x1", "Ethernet1", "0x0", "meta1");
  VerifyP4MulticastReplicationEntryEqual(expect_entry, replication_entry);
}

TEST_F(L3MulticastManagerTest,
       DeserializeMulticastReplicationEntryMissingMatchFieldTest) {
  std::string key = R"({"match/multicast_group_id":"0x1",)"
                    R"("match/multicast_replica_instance":"0x0"})";
  std::vector<swss::FieldValueTuple> attributes;
  attributes.push_back(
      swss::FieldValueTuple{p4orch::kControllerMetadata, "so_meta"});

  auto replication_entry_or = DeserializeMulticastReplicationEntry(
      key, attributes, APP_P4RT_REPLICATION_L2_MULTICAST_TABLE_NAME);
  EXPECT_EQ(StatusCode::SWSS_RC_INVALID_PARAM, replication_entry_or.status());
}

TEST_F(L3MulticastManagerTest,
       DeserializeMulticastReplicationEntryExtraAttributeTest) {
  std::string key = R"({"match/multicast_group_id":"0x1",)"
                    R"("match/multicast_replica_port":"Ethernet1",)"
                    R"("match/multicast_replica_instance":"0x0"})";
  std::vector<swss::FieldValueTuple> attributes;
  attributes.push_back(
      swss::FieldValueTuple{prependParamField(p4orch::kSrcMac), kSrcMac1});

  auto replication_entry_or = DeserializeMulticastReplicationEntry(
      key, attributes, APP_P4RT_REPLICATION_L2_MULTICAST_TABLE_NAME);
  EXPECT_EQ(StatusCode::SWSS_RC_INVALID_PARAM, replication_entry_or.status());
}

TEST_F(L3MulticastManagerTest, CreateRouterInterfaceSuccess) {
  auto entry = GenerateP4MulticastRouterInterfaceEntry(
      "Ethernet5", "0x5", swss::MacAddress(kSrcMac5));
  std::string rif_key = KeyGenerator::generateMulticastRouterInterfaceRifKey(
      entry.multicast_replica_port, entry.src_mac);
  sai_object_id_t rif_oid;

  EXPECT_CALL(mock_sai_router_intf_, create_router_interface(_, _, _, _))
      .WillOnce(DoAll(SetArgPointee<0>(kRifOid5), Return(SAI_STATUS_SUCCESS)));

  EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS,
            CreateRouterInterface(rif_key, entry, &rif_oid));
  EXPECT_EQ(rif_oid, kRifOid5);
}

TEST_F(L3MulticastManagerTest, CreateRouterInterfaceFailure) {
  auto entry = GenerateP4MulticastRouterInterfaceEntry(
      "Ethernet5", "0x5", swss::MacAddress(kSrcMac5));
  std::string rif_key = KeyGenerator::generateMulticastRouterInterfaceRifKey(
      entry.multicast_replica_port, entry.src_mac);
  sai_object_id_t rif_oid;

  EXPECT_CALL(mock_sai_router_intf_, create_router_interface(_, _, _, _))
      .WillOnce(Return(SAI_STATUS_FAILURE));

  EXPECT_EQ(StatusCode::SWSS_RC_UNKNOWN,
            CreateRouterInterface(rif_key, entry, &rif_oid));
}

TEST_F(L3MulticastManagerTest, CreateRouterInterfaceAttributeFailures) {
  auto entry = GenerateP4MulticastRouterInterfaceEntry(
      "Ethernet7", "0x5", swss::MacAddress(kSrcMac5));
  std::string rif_key = KeyGenerator::generateMulticastRouterInterfaceRifKey(
      entry.multicast_replica_port, entry.src_mac);
  sai_object_id_t rif_oid;

  EXPECT_EQ(StatusCode::SWSS_RC_INVALID_PARAM,
            CreateRouterInterface(rif_key, entry, &rif_oid));

  auto entry2 = GenerateP4MulticastRouterInterfaceEntry(
      "Ethernet17", "0x1", swss::MacAddress(kSrcMac1));
  std::string rif_key2 = KeyGenerator::generateMulticastRouterInterfaceRifKey(
      entry.multicast_replica_port, entry.src_mac);
  sai_object_id_t rif_oid2;

  EXPECT_EQ(StatusCode::SWSS_RC_NOT_FOUND,
            CreateRouterInterface(rif_key2, entry2, &rif_oid2));
}

TEST_F(L3MulticastManagerTest, CreateRouterInterfaceFailureAlreadyInMapper) {
  auto entry = GenerateP4MulticastRouterInterfaceEntry(
      "Ethernet5", "0x5", swss::MacAddress(kSrcMac5));
  std::string rif_key = KeyGenerator::generateMulticastRouterInterfaceRifKey(
      entry.multicast_replica_port, entry.src_mac);
  sai_object_id_t rif_oid = kRifOid5;
  p4_oid_mapper_.setOID(SAI_OBJECT_TYPE_ROUTER_INTERFACE, rif_key, rif_oid);

  EXPECT_EQ(StatusCode::SWSS_RC_INTERNAL,
            CreateRouterInterface(rif_key, entry, &rif_oid));
}

TEST_F(L3MulticastManagerTest, AddMulticastRouterInterfaceEntriesSuccess) {
  std::vector<P4MulticastRouterInterfaceEntry> entries;
  entries.push_back(GenerateP4MulticastRouterInterfaceEntry(
      "Ethernet5", "0x5", swss::MacAddress(kSrcMac5)));

  EXPECT_CALL(mock_sai_router_intf_, create_router_interface(_, _, _, _))
      .WillOnce(DoAll(SetArgPointee<0>(kRifOid5), Return(SAI_STATUS_SUCCESS)));

  std::vector<ReturnCode> statuses =
      AddMulticastRouterInterfaceEntries(entries);
  EXPECT_EQ(statuses.size(), 1);
  for (size_t i = 0; i < statuses.size(); ++i) {
    EXPECT_TRUE(statuses[i].ok());
  }
  EXPECT_EQ(entries[0].router_interface_oid, kRifOid5);
}

TEST_F(L3MulticastManagerTest, AddMulticastRouterInterfaceEntriesSameRif) {
  // RIFs are allocated based on multicast_replica_port, mac address.
  std::vector<P4MulticastRouterInterfaceEntry> entries;
  entries.push_back(GenerateP4MulticastRouterInterfaceEntry(
      "Ethernet5", "0x5", swss::MacAddress(kSrcMac5)));
  entries.push_back(GenerateP4MulticastRouterInterfaceEntry(
      "Ethernet5", "0x6", swss::MacAddress(kSrcMac5)));

  EXPECT_CALL(mock_sai_router_intf_, create_router_interface(_, _, _, _))
      .WillOnce(DoAll(SetArgPointee<0>(kRifOid5), Return(SAI_STATUS_SUCCESS)));

  std::vector<ReturnCode> statuses =
      AddMulticastRouterInterfaceEntries(entries);
  EXPECT_EQ(statuses.size(), 2);
  for (size_t i = 0; i < statuses.size(); ++i) {
    EXPECT_TRUE(statuses[i].ok());
  }

  // Confirm both entries use the same rif oid.
  EXPECT_EQ(entries[0].router_interface_oid, kRifOid5);
  EXPECT_EQ(entries[1].router_interface_oid, kRifOid5);
}

TEST_F(L3MulticastManagerTest,
       AddMulticastRouterInterfaceEntriesCreateRifFails) {
  // RIFs are allocated based on multicast_replica_port, mac address.
  std::vector<P4MulticastRouterInterfaceEntry> entries;
  entries.push_back(GenerateP4MulticastRouterInterfaceEntry(
      "Ethernet5", "0x5", swss::MacAddress(kSrcMac5)));
  entries.push_back(GenerateP4MulticastRouterInterfaceEntry(
      "Ethernet5", "0x6", swss::MacAddress(kSrcMac5)));

  EXPECT_CALL(mock_sai_router_intf_, create_router_interface(_, _, _, _))
      .WillOnce(Return(SAI_STATUS_FAILURE));

  std::vector<ReturnCode> statuses =
      AddMulticastRouterInterfaceEntries(entries);
  EXPECT_EQ(statuses.size(), 2);
  // First entry fails, second should not be executed.
  EXPECT_EQ(statuses[0].code(), StatusCode::SWSS_RC_UNKNOWN);
  EXPECT_EQ(statuses[1].code(), StatusCode::SWSS_RC_NOT_EXECUTED);
}

// ---------- Temporary tests (implemented functions) -------------------------

TEST_F(L3MulticastManagerTest, NoGetMulticastRouterInterfaceEntry) {
  auto entry = GenerateP4MulticastRouterInterfaceEntry(
      "Ethernet1", "0x0", swss::MacAddress(kSrcMac1));
  P4MulticastRouterInterfaceEntry* actual_entry_ptr =
      GetMulticastRouterInterfaceEntry(
          entry.multicast_router_interface_entry_key);
  ASSERT_EQ(actual_entry_ptr, nullptr);
}

TEST_F(L3MulticastManagerTest, NoGetMulticastReplicationEntry) {
  auto entry = GenerateP4MulticastReplicationEntry("0x1", "Ethernet1", "0x1");
  P4MulticastReplicationEntry* actual_entry_ptr =
      GetMulticastReplicationEntry(entry.multicast_replication_key);
  ASSERT_EQ(actual_entry_ptr, nullptr);
}

TEST_F(L3MulticastManagerTest, NoGetRifOid) {
  auto entry = GenerateP4MulticastRouterInterfaceEntry(
      "Ethernet1", "0x0", swss::MacAddress(kSrcMac1));
  sai_object_id_t oid = GetRifOid(&entry);
  ASSERT_EQ(oid, SAI_NULL_OBJECT_ID);
}

// ---------- Temporary tests (unimplemented functions) -----------------------

TEST_F(L3MulticastManagerTest, NoDrain) {
  EXPECT_TRUE(Drain(/*failure_before=*/false).ok());
}

TEST_F(L3MulticastManagerTest, NoVerifyState) {
  EXPECT_FALSE(VerifyState(/*key=*/"1", /*tuple=*/{}).empty());
}

TEST_F(L3MulticastManagerTest, NoVerifyMulticastRouterInterfaceStateCache) {
  auto entry1 = GenerateP4MulticastRouterInterfaceEntry(
      "Ethernet1", "0x0", swss::MacAddress(kSrcMac1));
  auto entry2 = GenerateP4MulticastRouterInterfaceEntry(
      "Ethernet2", "0x0", swss::MacAddress(kSrcMac1));
  EXPECT_FALSE(
      VerifyMulticastRouterInterfaceStateCache(entry1, &entry2).empty());
}

TEST_F(L3MulticastManagerTest, NoVerifyMulticastReplicationStateCache) {
  auto entry1 = GenerateP4MulticastReplicationEntry("0x1", "Ethernet1", "0x1");
  auto entry2 = GenerateP4MulticastReplicationEntry("0x2", "Ethernet1", "0x1");
  EXPECT_FALSE(VerifyMulticastReplicationStateCache(entry1, &entry2).empty());
}

TEST_F(L3MulticastManagerTest, NoValidateMulticastRouterInterfaceEntry) {
  auto entry = GenerateP4MulticastRouterInterfaceEntry(
      "Ethernet1", "0x0", swss::MacAddress(kSrcMac1));
  EXPECT_FALSE(ValidateMulticastRouterInterfaceEntry(entry, SET_COMMAND).ok());
}

TEST_F(L3MulticastManagerTest, NoValidateMulticastReplicationEntry) {
  auto entry = GenerateP4MulticastReplicationEntry("0x1", "Ethernet1", "0x1");
  EXPECT_FALSE(ValidateMulticastReplicationEntry(entry, SET_COMMAND).ok());
}

TEST_F(L3MulticastManagerTest, NoProcessMulticastRouterInterfaceEntries) {
  std::vector<P4MulticastRouterInterfaceEntry> entries;
  std::deque<swss::KeyOpFieldsValuesTuple> tuple_list;
  EXPECT_FALSE(ProcessMulticastRouterInterfaceEntries(
                   /*entries=*/entries, /*tuple_list=*/tuple_list, SET_COMMAND,
                   /*update=*/false)
                   .ok());
}

TEST_F(L3MulticastManagerTest, NoDeleteRouterInterface) {
  EXPECT_FALSE(DeleteRouterInterface(
                   /*rif_key=*/"1", /*rif_oid=*/SAI_NULL_OBJECT_ID)
                   .ok());
}

TEST_F(L3MulticastManagerTest, NoCreateMulticastGroup) {
  auto entry = GenerateP4MulticastReplicationEntry("0x1", "Ethernet1", "0x1");
  sai_object_id_t oid = SAI_NULL_OBJECT_ID;
  EXPECT_FALSE(CreateMulticastGroup(entry, /*mcast_group_oid=*/&oid).ok());
}

TEST_F(L3MulticastManagerTest, NoDeleteMulticastGroup) {
  auto entry = GenerateP4MulticastReplicationEntry("0x1", "Ethernet1", "0x1");
  sai_object_id_t oid = SAI_NULL_OBJECT_ID;
  EXPECT_FALSE(DeleteMulticastGroup(
                   /*multicast_group_id=*/"1", /*mcast_group_oid=*/oid)
                   .ok());
}

TEST_F(L3MulticastManagerTest, NoCreateMulticastGroupMember) {
  auto entry = GenerateP4MulticastReplicationEntry("0x1", "Ethernet1", "0x1");
  sai_object_id_t oid = SAI_NULL_OBJECT_ID;
  EXPECT_FALSE(CreateMulticastGroupMember(entry, /*rif_oid=*/SAI_NULL_OBJECT_ID,
                                          /*mcast_group_member_oid=*/&oid)
                   .ok());
}

TEST_F(L3MulticastManagerTest, NoUpdateMulticastRouterInterfaceEntries) {
  std::vector<P4MulticastRouterInterfaceEntry> entries;
  auto rcs = UpdateMulticastRouterInterfaceEntries(entries);
  ASSERT_EQ(rcs.size(), 1);
  EXPECT_FALSE(rcs[0].ok());
}

TEST_F(L3MulticastManagerTest, NoDeleteMulticastRouterInterfaceEntries) {
  std::vector<P4MulticastRouterInterfaceEntry> entries;
  auto rcs = DeleteMulticastRouterInterfaceEntries(entries);
  ASSERT_EQ(rcs.size(), 1);
  EXPECT_FALSE(rcs[0].ok());
}

TEST_F(L3MulticastManagerTest, NoAddMulticastReplicationEntries) {
  std::vector<P4MulticastReplicationEntry> entries;
  auto rcs = AddMulticastReplicationEntries(entries);
  ASSERT_EQ(rcs.size(), 1);
  EXPECT_FALSE(rcs[0].ok());
}

TEST_F(L3MulticastManagerTest, NoDeleteMulticastReplicationEntries) {
  std::vector<P4MulticastReplicationEntry> entries;
  auto rcs = DeleteMulticastReplicationEntries(entries);
  ASSERT_EQ(rcs.size(), 1);
  EXPECT_FALSE(rcs[0].ok());
}

TEST_F(L3MulticastManagerTest, NoVerifyMulticastReplicationStateAsicDb) {
  auto entry = GenerateP4MulticastReplicationEntry("0x1", "Ethernet1", "0x1");
  EXPECT_FALSE(VerifyMulticastReplicationStateAsicDb(&entry).empty());
}

}  // namespace p4orch
