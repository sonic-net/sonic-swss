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

  P4MulticastRouterInterfaceEntry SetupP4MulticastRouterInterfaceEntry(
      const std::string& port, const std::string& instance,
      const swss::MacAddress mac, const sai_object_id_t rif_oid,
      bool expect_mock = true) {
    std::vector<P4MulticastRouterInterfaceEntry> entries;
    auto entry = GenerateP4MulticastRouterInterfaceEntry(port, instance, mac);
    entries.push_back(entry);

    if (expect_mock) {
      EXPECT_CALL(mock_sai_router_intf_, create_router_interface(_, _, _, _))
          .WillOnce(
              DoAll(SetArgPointee<0>(rif_oid), Return(SAI_STATUS_SUCCESS)));
    }

    std::vector<ReturnCode> statuses =
        AddMulticastRouterInterfaceEntries(entries);

    EXPECT_EQ(statuses.size(), 1);
    EXPECT_TRUE(statuses[0].ok());

    EXPECT_EQ(entries[0].router_interface_oid, rif_oid);
    EXPECT_NE(GetMulticastRouterInterfaceEntry(
                  entries[0].multicast_router_interface_entry_key),
              nullptr);
    EXPECT_EQ(GetRifOid(&entries[0]), rif_oid);
    return entry;
  }

  P4MulticastReplicationEntry SetupP4MulticastReplicationEntry(
      const std::string& multicast_group_id, const std::string& port,
      const std::string& instance, const sai_object_id_t group_oid,
      const sai_object_id_t group_member_oid, bool expect_group_mock = true) {
    std::vector<P4MulticastReplicationEntry> entries;
    auto entry =
        GenerateP4MulticastReplicationEntry(multicast_group_id, port, instance);
    entries.push_back(entry);

    if (expect_group_mock) {
      EXPECT_CALL(mock_sai_ipmc_group_, create_ipmc_group(_, _, _, _))
          .WillOnce(
              DoAll(SetArgPointee<0>(group_oid), Return(SAI_STATUS_SUCCESS)));
    }
    EXPECT_CALL(mock_sai_ipmc_group_, create_ipmc_group_member(_, _, _, _))
        .WillOnce(DoAll(SetArgPointee<0>(group_member_oid),
                        Return(SAI_STATUS_SUCCESS)));

    auto statuses = AddMulticastReplicationEntries(entries);
    EXPECT_EQ(statuses.size(), 1);
    EXPECT_TRUE(statuses[0].ok());

    sai_object_id_t end_groupOid = SAI_NULL_OBJECT_ID;
    p4_oid_mapper_.getOID(SAI_OBJECT_TYPE_IPMC_GROUP,
                          entries[0].multicast_group_id, &end_groupOid);
    sai_object_id_t end_groupMemberOid = SAI_NULL_OBJECT_ID;
    p4_oid_mapper_.getOID(SAI_OBJECT_TYPE_IPMC_GROUP_MEMBER,
                          entries[0].multicast_replication_key,
                          &end_groupMemberOid);
    EXPECT_EQ(end_groupOid, group_oid);
    EXPECT_EQ(end_groupMemberOid, group_member_oid);
    return entry;
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
      const std::vector<swss::FieldValueTuple>& attributes) {
    return l3_multicast_manager_.deserializeMulticastRouterInterfaceEntry(
        key, attributes);
  }

  ReturnCodeOr<P4MulticastReplicationEntry>
  DeserializeMulticastReplicationEntry(
      const std::string& key,
      const std::vector<swss::FieldValueTuple>& attributes) {
    return l3_multicast_manager_.deserializeMulticastReplicationEntry(
        key, attributes);
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

  std::vector<ReturnCode> UpdateMulticastReplicationEntries(
      std::vector<P4MulticastReplicationEntry>& entries) {
    return l3_multicast_manager_.updateMulticastReplicationEntries(entries);
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

  // Unnatural function to add multicast replication entry reference to a RIF.
  void ForceAddMulticastGroupMember(
      const sai_object_id_t rif_oid,
      const std::string& multicast_replication_key) {
    l3_multicast_manager_.m_rifOidToMulticastGroupMembers[rif_oid].insert(
        multicast_replication_key);
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

  auto router_interface_entry_or =
      DeserializeMulticastRouterInterfaceEntry(key, attributes);
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

  auto router_interface_entry_or =
      DeserializeMulticastRouterInterfaceEntry(key, attributes);
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

  auto router_interface_entry_or =
      DeserializeMulticastRouterInterfaceEntry(key, attributes);
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

  auto router_interface_entry_or =
      DeserializeMulticastRouterInterfaceEntry(key, attributes);
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

  auto replication_entry_or =
      DeserializeMulticastReplicationEntry(key, attributes);
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

  auto replication_entry_or =
      DeserializeMulticastReplicationEntry(key, attributes);
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

  auto replication_entry_or =
      DeserializeMulticastReplicationEntry(key, attributes);
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

TEST_F(L3MulticastManagerTest, DeleteRouterInterfaceSuccess) {
  std::string rif_key = KeyGenerator::generateMulticastRouterInterfaceRifKey(
      "Ethernet1", swss::MacAddress(kSrcMac1));
  // Add default value to map to avoid error.
  sai_object_id_t rif_oid = 1;
  p4_oid_mapper_.setOID(SAI_OBJECT_TYPE_ROUTER_INTERFACE, rif_key, rif_oid);
  EXPECT_CALL(mock_sai_router_intf_, remove_router_interface(rif_oid))
      .WillOnce(Return(SAI_STATUS_SUCCESS));
  EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS,
            DeleteRouterInterface(rif_key, rif_oid));
}

TEST_F(L3MulticastManagerTest, DeleteRouterInterfaceFailureNotInMap) {
  std::string rif_key = KeyGenerator::generateMulticastRouterInterfaceRifKey(
      "Ethernet1", swss::MacAddress(kSrcMac1));
  sai_object_id_t rif_oid = 1;
  EXPECT_EQ(StatusCode::SWSS_RC_INTERNAL,
            DeleteRouterInterface(rif_key, rif_oid));
}

TEST_F(L3MulticastManagerTest, DeleteRouterInterfaceFailureSaiFailure) {
  std::string rif_key = KeyGenerator::generateMulticastRouterInterfaceRifKey(
      "Ethernet1", swss::MacAddress(kSrcMac1));
  // Add default value to map to avoid error.
  sai_object_id_t rif_oid = 1;
  p4_oid_mapper_.setOID(SAI_OBJECT_TYPE_ROUTER_INTERFACE, rif_key, rif_oid);
  EXPECT_CALL(mock_sai_router_intf_, remove_router_interface(rif_oid))
      .WillOnce(Return(SAI_STATUS_FAILURE));
  EXPECT_EQ(StatusCode::SWSS_RC_UNKNOWN,
            DeleteRouterInterface(rif_key, rif_oid));
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

TEST_F(L3MulticastManagerTest, DeleteMulticastRouterInterfaceEntriesSuccess) {
  auto entry1 = SetupP4MulticastRouterInterfaceEntry(
      "Ethernet1", "0x1", swss::MacAddress(kSrcMac1), kRifOid1);
  auto entry2 = SetupP4MulticastRouterInterfaceEntry(
      "Ethernet2", "0x2", swss::MacAddress(kSrcMac2), kRifOid2);

  std::vector<ReturnCode> statuses;
  // Second, delete entries just added.  Expect success and no more references
  // to the old entries.
  // Permute order of delete.
  std::vector<P4MulticastRouterInterfaceEntry> entries;
  entries.push_back(entry2);
  entries.push_back(entry1);

  EXPECT_CALL(mock_sai_router_intf_, remove_router_interface(_))
      .WillOnce(Return(SAI_STATUS_SUCCESS))
      .WillOnce(Return(SAI_STATUS_SUCCESS));
  statuses = DeleteMulticastRouterInterfaceEntries(entries);
  ASSERT_EQ(statuses.size(), 2);
  for (size_t i = 0; i < statuses.size(); ++i) {
    EXPECT_TRUE(statuses[i].ok());
  }
  // Expect no more references to entries.
  EXPECT_EQ(GetMulticastRouterInterfaceEntry(
                entries[0].multicast_router_interface_entry_key),
            nullptr);
  EXPECT_EQ(GetMulticastRouterInterfaceEntry(
                entries[1].multicast_router_interface_entry_key),
            nullptr);
  EXPECT_EQ(GetRifOid(&entries[0]), SAI_NULL_OBJECT_ID);
  EXPECT_EQ(GetRifOid(&entries[1]), SAI_NULL_OBJECT_ID);
}

TEST_F(L3MulticastManagerTest,
       DeleteMulticastRouterInterfaceEntriesSuccessSingleRif) {
  auto entry1 = SetupP4MulticastRouterInterfaceEntry(
      "Ethernet1", "0x1", swss::MacAddress(kSrcMac1), kRifOid1);
  auto entry2 = SetupP4MulticastRouterInterfaceEntry(
      "Ethernet1", "0x2", swss::MacAddress(kSrcMac1), kRifOid1,
      /*expect_mock=*/false);
  std::vector<P4MulticastRouterInterfaceEntry> entries = {entry1, entry2};

  std::vector<ReturnCode> statuses;
  // Second, delete entries just added.  Expect success and no more references
  // to the old entries.
  EXPECT_CALL(mock_sai_router_intf_, remove_router_interface(_))
      .WillOnce(Return(SAI_STATUS_SUCCESS));
  statuses = DeleteMulticastRouterInterfaceEntries(entries);
  EXPECT_EQ(statuses.size(), 2);
  for (size_t i = 0; i < statuses.size(); ++i) {
    EXPECT_TRUE(statuses[i].ok());
  }
  // Expect no more references to entries.
  EXPECT_EQ(GetMulticastRouterInterfaceEntry(
                entries[0].multicast_router_interface_entry_key),
            nullptr);
  EXPECT_EQ(GetMulticastRouterInterfaceEntry(
                entries[1].multicast_router_interface_entry_key),
            nullptr);
  EXPECT_EQ(GetRifOid(&entries[0]), SAI_NULL_OBJECT_ID);
  EXPECT_EQ(GetRifOid(&entries[1]), SAI_NULL_OBJECT_ID);
}

TEST_F(L3MulticastManagerTest,
       DeleteMulticastRouterInterfaceEntriesFailUnknownEntry) {
  std::vector<P4MulticastRouterInterfaceEntry> entries;
  entries.push_back(GenerateP4MulticastRouterInterfaceEntry(
      "Ethernet1", "0x1", swss::MacAddress(kSrcMac1)));
  entries.push_back(GenerateP4MulticastRouterInterfaceEntry(
      "Ethernet2", "0x2", swss::MacAddress(kSrcMac2)));

  auto statuses = DeleteMulticastRouterInterfaceEntries(entries);
  EXPECT_EQ(statuses.size(), 2);
  EXPECT_EQ(statuses[0], StatusCode::SWSS_RC_UNKNOWN);
  EXPECT_EQ(statuses[1], StatusCode::SWSS_RC_NOT_EXECUTED);
}

TEST_F(L3MulticastManagerTest,
       DeleteMulticastRouterInterfaceEntriesFailMissingOid) {
  // First, add an entry that we will delete.  Expect success.
  auto entry1 = SetupP4MulticastRouterInterfaceEntry(
      "Ethernet1", "0x1", swss::MacAddress(kSrcMac1), kRifOid1);
  std::vector<P4MulticastRouterInterfaceEntry> entries = {entry1};

  // Force-remove the OID.
  std::string rif_key = KeyGenerator::generateMulticastRouterInterfaceRifKey(
      entry1.multicast_replica_port, entry1.src_mac);
  ForceRemoveRifKey(rif_key);

  // Second, delete entry just added.  Expect internal failure.
  // Add extra entry to exercise not executed.
  entries.push_back(GenerateP4MulticastRouterInterfaceEntry(
      "Ethernet2", "0x2", swss::MacAddress(kSrcMac2)));

  auto statuses = DeleteMulticastRouterInterfaceEntries(entries);
  EXPECT_EQ(statuses.size(), 2);
  EXPECT_EQ(statuses[0], StatusCode::SWSS_RC_INTERNAL);
  EXPECT_EQ(statuses[1], StatusCode::SWSS_RC_NOT_EXECUTED);
}

TEST_F(L3MulticastManagerTest,
       DeleteMulticastRouterInterfaceEntriesFailNoAssociatedEntries) {
  // First, add an entry that we will delete.  Expect success.
  auto entry1 = SetupP4MulticastRouterInterfaceEntry(
      "Ethernet1", "0x1", swss::MacAddress(kSrcMac1), kRifOid1);
  std::vector<P4MulticastRouterInterfaceEntry> entries = {entry1};

  // Force-remove the OID, so there are no associated router interface table
  // entries.
  ForceRemoveRifOid(kRifOid1);

  // Second, delete entry just added.  Expect internal failure.
  // Add extra entry to exercise not executed.
  entries.push_back(GenerateP4MulticastRouterInterfaceEntry(
      "Ethernet2", "0x2", swss::MacAddress(kSrcMac2)));
  auto statuses = DeleteMulticastRouterInterfaceEntries(entries);
  EXPECT_EQ(statuses.size(), 2);
  EXPECT_EQ(statuses[0], StatusCode::SWSS_RC_INTERNAL);
  EXPECT_EQ(statuses[1], StatusCode::SWSS_RC_NOT_EXECUTED);
}

TEST_F(L3MulticastManagerTest,
       DeleteMulticastRouterInterfaceEntriesFailMissingRifEntries) {
  // First, add an entry that we will delete.  Expect success.
  auto entry1 = SetupP4MulticastRouterInterfaceEntry(
      "Ethernet1", "0x1", swss::MacAddress(kSrcMac1), kRifOid1);
  std::vector<P4MulticastRouterInterfaceEntry> entries = {entry1};

  // Force clear the vector container of RIF entries.
  ForceClearRifVector(kRifOid1);

  // Second, delete entry just added.  Expect internal failure.
  // Add extra entry to exercise not executed.
  entries.push_back(GenerateP4MulticastRouterInterfaceEntry(
      "Ethernet2", "0x2", swss::MacAddress(kSrcMac2)));

  auto statuses = DeleteMulticastRouterInterfaceEntries(entries);
  EXPECT_EQ(statuses.size(), 2);
  EXPECT_EQ(statuses[0], StatusCode::SWSS_RC_INTERNAL);
  EXPECT_EQ(statuses[1], StatusCode::SWSS_RC_NOT_EXECUTED);
}

TEST_F(L3MulticastManagerTest, DeleteMulticastRouterInterfaceEntriesSaiFails) {
  // First, add an entry that we will delete.  Expect success.
  auto entry1 = SetupP4MulticastRouterInterfaceEntry(
      "Ethernet1", "0x1", swss::MacAddress(kSrcMac1), kRifOid1);
  auto entry2 = SetupP4MulticastRouterInterfaceEntry(
      "Ethernet2", "0x2", swss::MacAddress(kSrcMac2), kRifOid2);
  std::vector<P4MulticastRouterInterfaceEntry> entries = {entry1, entry2};

  // Second, delete entries just added.  Expect failure, since SAI call fails.
  EXPECT_CALL(mock_sai_router_intf_, remove_router_interface(_))
      .WillOnce(Return(SAI_STATUS_FAILURE));
  auto statuses = DeleteMulticastRouterInterfaceEntries(entries);
  EXPECT_EQ(statuses.size(), 2);
  EXPECT_EQ(statuses[0], StatusCode::SWSS_RC_UNKNOWN);
  EXPECT_EQ(statuses[1], StatusCode::SWSS_RC_NOT_EXECUTED);

  // Since SAI call failed, internal state should still have references.
  EXPECT_EQ(GetRifOid(&entries[0]), kRifOid1);
  EXPECT_EQ(GetRifOid(&entries[1]), kRifOid2);
}

TEST_F(L3MulticastManagerTest,
       DeleteMulticastRouterInterfaceEntriesRifStillInUse) {
  // First, add an entry that we will delete.  Expect success.
  auto entry1 = SetupP4MulticastRouterInterfaceEntry(
      "Ethernet1", "0x1", swss::MacAddress(kSrcMac1), kRifOid1);
  auto entry2 = SetupP4MulticastRouterInterfaceEntry(
      "Ethernet2", "0x2", swss::MacAddress(kSrcMac2), kRifOid2);
  std::vector<P4MulticastRouterInterfaceEntry> entries = {entry1, entry2};

  // Unnaturally add reference to multicast replication entry.
  ForceAddMulticastGroupMember(kRifOid1, "some_key");

  // Second, delete entries just added.  Expect failure, RIF still in use.
  auto statuses = DeleteMulticastRouterInterfaceEntries(entries);
  EXPECT_EQ(statuses.size(), 2);
  EXPECT_EQ(statuses[0], StatusCode::SWSS_RC_IN_USE);
  EXPECT_EQ(statuses[1], StatusCode::SWSS_RC_NOT_EXECUTED);

  // Since SAI call failed, internal state should still have references.
  EXPECT_EQ(GetRifOid(&entries[0]), kRifOid1);
  EXPECT_EQ(GetRifOid(&entries[1]), kRifOid2);
}

TEST_F(L3MulticastManagerTest, UpdateMulticastRouterInterfaceEntriesSuccess) {
  // First, add an entry that we will update.  Expect success.
  auto entry1 = SetupP4MulticastRouterInterfaceEntry(
      "Ethernet1", "0x1", swss::MacAddress(kSrcMac1), kRifOid1);
  std::vector<P4MulticastRouterInterfaceEntry> entries = {entry1};

  // Second, update entry just added.  Expect success and no more references
  // to the old entry.
  std::vector<P4MulticastRouterInterfaceEntry> entries2;
  entries2.push_back(GenerateP4MulticastRouterInterfaceEntry(
      "Ethernet1", "0x1", swss::MacAddress(kSrcMac2)));

  EXPECT_CALL(mock_sai_router_intf_, remove_router_interface(_))
      .WillOnce(Return(SAI_STATUS_SUCCESS));
  EXPECT_CALL(mock_sai_router_intf_, create_router_interface(_, _, _, _))
      .WillOnce(DoAll(SetArgPointee<0>(kRifOid2), Return(SAI_STATUS_SUCCESS)));
  auto statuses = UpdateMulticastRouterInterfaceEntries(entries2);
  EXPECT_EQ(statuses.size(), 1);
  for (size_t i = 0; i < statuses.size(); ++i) {
    EXPECT_TRUE(statuses[i].ok());
  }
  // Expect entry to have been updated.
  EXPECT_NE(GetMulticastRouterInterfaceEntry(
                entries2[0].multicast_router_interface_entry_key),
            nullptr);
  EXPECT_EQ(GetRifOid(&entries2[0]), kRifOid2);
}

TEST_F(L3MulticastManagerTest,
       UpdateMulticastRouterInterfaceEntriesNoChangeSuccess) {
  // First, add entries that we will update.
  auto entry1 = SetupP4MulticastRouterInterfaceEntry(
      "Ethernet1", "0x1", swss::MacAddress(kSrcMac1), kRifOid1);
  auto entry2 = SetupP4MulticastRouterInterfaceEntry(
      "Ethernet2", "0x2", swss::MacAddress(kSrcMac2), kRifOid2);
  std::vector<P4MulticastRouterInterfaceEntry> original_entries = {entry1,
                                                                   entry2};

  // Second, "update" entries to use same src mac.  Expect success.
  auto statuses = UpdateMulticastRouterInterfaceEntries(original_entries);
  EXPECT_EQ(statuses.size(), 2);
  for (size_t i = 0; i < statuses.size(); ++i) {
    EXPECT_TRUE(statuses[i].ok());
  }
  // Expect original RIF OIDs.
  EXPECT_EQ(GetRifOid(&original_entries[0]), kRifOid1);
  EXPECT_EQ(GetRifOid(&original_entries[1]), kRifOid2);
}

TEST_F(L3MulticastManagerTest,
       UpdateMulticastRouterInterfaceEntriesCannotRemoveRif) {
  // First, add entries that we will update.
  auto entry1 = SetupP4MulticastRouterInterfaceEntry(
      "Ethernet1", "0x1", swss::MacAddress(kSrcMac1), kRifOid1);
  auto entry2 = SetupP4MulticastRouterInterfaceEntry(
      "Ethernet2", "0x2", swss::MacAddress(kSrcMac2), kRifOid2);
  std::vector<P4MulticastRouterInterfaceEntry> original_entries = {entry1,
                                                                   entry2};

  // Unnaturally add reference to multicast replication entry.
  ForceAddMulticastGroupMember(kRifOid1, "some_key");

  // Second, update entries just added.  Expect failure, since RIF is in use.
  std::vector<P4MulticastRouterInterfaceEntry> entries2;
  entries2.push_back(GenerateP4MulticastRouterInterfaceEntry(
      "Ethernet1", "0x1", swss::MacAddress(kSrcMac3)));
  entries2.push_back(GenerateP4MulticastRouterInterfaceEntry(
      "Ethernet2", "0x2", swss::MacAddress(kSrcMac4)));

  auto statuses = UpdateMulticastRouterInterfaceEntries(entries2);
  EXPECT_EQ(statuses.size(), 2);
  EXPECT_EQ(statuses[0], StatusCode::SWSS_RC_IN_USE);
  EXPECT_EQ(statuses[1], StatusCode::SWSS_RC_NOT_EXECUTED);

  // Expect entries to remain unchanged.
  EXPECT_EQ(GetRifOid(&entry1), kRifOid1);
  EXPECT_EQ(GetRifOid(&entry2), kRifOid2);
}

TEST_F(L3MulticastManagerTest,
       UpdateMulticastRouterInterfaceEntriesMissingEntry) {
  // First, add entries that we will update.  Expect success.
  auto entry1 = SetupP4MulticastRouterInterfaceEntry(
      "Ethernet1", "0x1", swss::MacAddress(kSrcMac1), kRifOid1);
  auto entry2 = SetupP4MulticastRouterInterfaceEntry(
      "Ethernet2", "0x2", swss::MacAddress(kSrcMac2), kRifOid2);
  std::vector<P4MulticastRouterInterfaceEntry> original_entries = {entry1,
                                                                   entry2};

  // Attempt to update both entries, but remove the first entry to cause an
  // error.
  std::vector<P4MulticastRouterInterfaceEntry> delete_entries;
  delete_entries.push_back(entry1);

  EXPECT_CALL(mock_sai_router_intf_, remove_router_interface(_))
      .WillOnce(Return(SAI_STATUS_SUCCESS));
  auto statuses = DeleteMulticastRouterInterfaceEntries(delete_entries);
  EXPECT_EQ(statuses.size(), 1);
  EXPECT_TRUE(statuses[0].ok());

  // Attempt to update the original entries, which should result in an error.
  std::vector<P4MulticastRouterInterfaceEntry> update_entries;
  update_entries.push_back(GenerateP4MulticastRouterInterfaceEntry(
      "Ethernet1", "0x1", swss::MacAddress(kSrcMac3)));
  update_entries.push_back(GenerateP4MulticastRouterInterfaceEntry(
      "Ethernet2", "0x2", swss::MacAddress(kSrcMac4)));

  statuses = UpdateMulticastRouterInterfaceEntries(update_entries);
  EXPECT_EQ(statuses.size(), 2);
  EXPECT_EQ(statuses[0], StatusCode::SWSS_RC_INTERNAL);
  EXPECT_EQ(statuses[1], StatusCode::SWSS_RC_NOT_EXECUTED);
}

TEST_F(L3MulticastManagerTest,
       UpdateMulticastRouterInterfaceEntriesMissingOid) {
  // First, add entries that we will update.  Expect success.
  auto entry1 = SetupP4MulticastRouterInterfaceEntry(
      "Ethernet1", "0x1", swss::MacAddress(kSrcMac1), kRifOid1);
  auto entry2 = SetupP4MulticastRouterInterfaceEntry(
      "Ethernet2", "0x2", swss::MacAddress(kSrcMac2), kRifOid2);
  std::vector<P4MulticastRouterInterfaceEntry> original_entries = {entry1,
                                                                   entry2};

  // Force clear RIF key, to cause an internal error.
  std::string rif_key0 = KeyGenerator::generateMulticastRouterInterfaceRifKey(
      original_entries[0].multicast_replica_port, original_entries[0].src_mac);
  ForceRemoveRifKey(rif_key0);

  // Attempt to update the original entries, which should result in an error.
  std::vector<P4MulticastRouterInterfaceEntry> update_entries;
  update_entries.push_back(GenerateP4MulticastRouterInterfaceEntry(
      "Ethernet1", "0x1", swss::MacAddress(kSrcMac3)));
  update_entries.push_back(GenerateP4MulticastRouterInterfaceEntry(
      "Ethernet2", "0x2", swss::MacAddress(kSrcMac4)));

  auto statuses = UpdateMulticastRouterInterfaceEntries(update_entries);
  EXPECT_EQ(statuses.size(), 2);
  EXPECT_EQ(statuses[0], StatusCode::SWSS_RC_INTERNAL);
  EXPECT_EQ(statuses[1], StatusCode::SWSS_RC_NOT_EXECUTED);
}

TEST_F(L3MulticastManagerTest,
       UpdateMulticastRouterInterfaceEntriesMissingRifOidInMap) {
  // First, add entries that we will update.  Expect success.
  auto entry1 = SetupP4MulticastRouterInterfaceEntry(
      "Ethernet1", "0x1", swss::MacAddress(kSrcMac1), kRifOid1);
  auto entry2 = SetupP4MulticastRouterInterfaceEntry(
      "Ethernet2", "0x2", swss::MacAddress(kSrcMac2), kRifOid2);
  std::vector<P4MulticastRouterInterfaceEntry> original_entries = {entry1,
                                                                   entry2};

  // Force clear internal RIF OID mapping to cause an error.
  ForceRemoveRifOid(kRifOid1);

  // Attempt to update the original entries, which should result in an error.
  std::vector<P4MulticastRouterInterfaceEntry> update_entries;
  update_entries.push_back(GenerateP4MulticastRouterInterfaceEntry(
      "Ethernet1", "0x1", swss::MacAddress(kSrcMac3)));
  update_entries.push_back(GenerateP4MulticastRouterInterfaceEntry(
      "Ethernet2", "0x2", swss::MacAddress(kSrcMac4)));

  auto statuses = UpdateMulticastRouterInterfaceEntries(update_entries);
  EXPECT_EQ(statuses.size(), 2);
  EXPECT_EQ(statuses[0], StatusCode::SWSS_RC_INTERNAL);
  EXPECT_EQ(statuses[1], StatusCode::SWSS_RC_NOT_EXECUTED);
}

TEST_F(L3MulticastManagerTest,
       UpdateMulticastRouterInterfaceEntriesMissingEntryInMap) {
  // First, add entries that we will update.  Expect success.
  auto entry1 = SetupP4MulticastRouterInterfaceEntry(
      "Ethernet1", "0x1", swss::MacAddress(kSrcMac1), kRifOid1);
  auto entry2 = SetupP4MulticastRouterInterfaceEntry(
      "Ethernet2", "0x2", swss::MacAddress(kSrcMac2), kRifOid2);
  std::vector<P4MulticastRouterInterfaceEntry> original_entries = {entry1,
                                                                   entry2};

  // Force delete entry key from vector to cause an error.
  ForceClearRifVector(kRifOid1);

  // Attempt to update the original entries, which should result in an error.
  std::vector<P4MulticastRouterInterfaceEntry> update_entries;
  update_entries.push_back(GenerateP4MulticastRouterInterfaceEntry(
      "Ethernet1", "0x1", swss::MacAddress(kSrcMac3)));
  update_entries.push_back(GenerateP4MulticastRouterInterfaceEntry(
      "Ethernet2", "0x2", swss::MacAddress(kSrcMac4)));

  auto statuses = UpdateMulticastRouterInterfaceEntries(update_entries);
  EXPECT_EQ(statuses.size(), 2);
  EXPECT_EQ(statuses[0], StatusCode::SWSS_RC_INTERNAL);
  EXPECT_EQ(statuses[1], StatusCode::SWSS_RC_NOT_EXECUTED);
}

TEST_F(L3MulticastManagerTest,
       UpdateMulticastRouterInterfaceEntriesUpdateCreateFails) {
  // First, add entries that we will update.  Expect success.
  auto entry1 = SetupP4MulticastRouterInterfaceEntry(
      "Ethernet1", "0x1", swss::MacAddress(kSrcMac1), kRifOid1);
  auto entry2 = SetupP4MulticastRouterInterfaceEntry(
      "Ethernet2", "0x2", swss::MacAddress(kSrcMac2), kRifOid2);
  std::vector<P4MulticastRouterInterfaceEntry> original_entries = {entry1,
                                                                   entry2};

  // Attempt to update the original entries, which is set to result in an error.
  std::vector<P4MulticastRouterInterfaceEntry> update_entries;
  update_entries.push_back(GenerateP4MulticastRouterInterfaceEntry(
      "Ethernet1", "0x1", swss::MacAddress(kSrcMac3)));
  update_entries.push_back(GenerateP4MulticastRouterInterfaceEntry(
      "Ethernet2", "0x2", swss::MacAddress(kSrcMac4)));

  EXPECT_CALL(mock_sai_router_intf_, create_router_interface(_, _, _, _))
      .WillOnce(Return(SAI_STATUS_FAILURE));
  auto statuses = UpdateMulticastRouterInterfaceEntries(update_entries);
  EXPECT_EQ(statuses.size(), 2);
  EXPECT_EQ(statuses[0], StatusCode::SWSS_RC_UNKNOWN);
  EXPECT_EQ(statuses[1], StatusCode::SWSS_RC_NOT_EXECUTED);
}

TEST_F(L3MulticastManagerTest,
       UpdateMulticastRouterInterfaceEntriesUpdateDeleteFails) {
  // First, add entries that we will update.  Expect success.
  auto entry1 = SetupP4MulticastRouterInterfaceEntry(
      "Ethernet1", "0x1", swss::MacAddress(kSrcMac1), kRifOid1);
  auto entry2 = SetupP4MulticastRouterInterfaceEntry(
      "Ethernet2", "0x2", swss::MacAddress(kSrcMac2), kRifOid2);
  std::vector<P4MulticastRouterInterfaceEntry> original_entries = {entry1,
                                                                   entry2};

  // Attempt to update the original entries, which should result in an error.
  std::vector<P4MulticastRouterInterfaceEntry> update_entries;
  update_entries.push_back(GenerateP4MulticastRouterInterfaceEntry(
      "Ethernet1", "0x1", swss::MacAddress(kSrcMac3)));
  update_entries.push_back(GenerateP4MulticastRouterInterfaceEntry(
      "Ethernet2", "0x2", swss::MacAddress(kSrcMac4)));

  // Expect first entry to be able to create RIF, but delete of old RIF to fail.
  EXPECT_CALL(mock_sai_router_intf_, create_router_interface(_, _, _, _))
      .WillOnce(DoAll(SetArgPointee<0>(kRifOid3), Return(SAI_STATUS_SUCCESS)));
  EXPECT_CALL(mock_sai_router_intf_, remove_router_interface(_))
      .WillOnce(Return(SAI_STATUS_FAILURE));
  auto statuses = UpdateMulticastRouterInterfaceEntries(update_entries);
  EXPECT_EQ(statuses.size(), 2);
  EXPECT_EQ(statuses[0], StatusCode::SWSS_RC_UNKNOWN);
  EXPECT_EQ(statuses[1], StatusCode::SWSS_RC_NOT_EXECUTED);
}

TEST_F(L3MulticastManagerTest,
       UpdateMulticastRouterInterfaceEntriesUpdateLeaveOldRifSuccess) {
  // First, add entries that we will update.  Expect success.
  // One RIF created.
  auto entry1 = SetupP4MulticastRouterInterfaceEntry(
      "Ethernet1", "0x1", swss::MacAddress(kSrcMac1), kRifOid1);
  auto entry2 = SetupP4MulticastRouterInterfaceEntry(
      "Ethernet1", "0x2", swss::MacAddress(kSrcMac1), kRifOid1,
      /*expect_mock=*/false);
  std::vector<P4MulticastRouterInterfaceEntry> original_entries = {entry1,
                                                                   entry2};

  // Attempt to update an original entry, resulting in new RIF being created.
  std::vector<P4MulticastRouterInterfaceEntry> update_entries;
  update_entries.push_back(GenerateP4MulticastRouterInterfaceEntry(
      "Ethernet1", "0x1", swss::MacAddress(kSrcMac2)));

  // Expect first entry to be able to create RIF, but old RIF remains.
  EXPECT_CALL(mock_sai_router_intf_, create_router_interface(_, _, _, _))
      .WillOnce(DoAll(SetArgPointee<0>(kRifOid2), Return(SAI_STATUS_SUCCESS)));
  auto statuses = UpdateMulticastRouterInterfaceEntries(update_entries);
  EXPECT_EQ(statuses.size(), 1);
  EXPECT_TRUE(statuses[0].ok());
}

TEST_F(L3MulticastManagerTest, DrainMulticastRouterInterfaceEntryAdd) {
  const std::string match_key =
      R"({"match/multicast_replica_port":"Ethernet4",)"
      R"("match/multicast_replica_instance":"0x1"})";
  const std::string appl_db_key =
      std::string(APP_P4RT_MULTICAST_ROUTER_INTERFACE_TABLE_NAME) +
      kTableKeyDelimiter + match_key;

  // Enqueue entry for create operation.
  auto expect_entry = GenerateP4MulticastRouterInterfaceEntry(
      "Ethernet4", "0x1", swss::MacAddress(kSrcMac2));
  expect_entry.router_interface_oid = kRifOid1;
  auto start_rifOid = GetRifOid(&expect_entry);
  EXPECT_EQ(start_rifOid, SAI_NULL_OBJECT_ID);

  std::vector<swss::FieldValueTuple> attributes;
  attributes.push_back(
      swss::FieldValueTuple{p4orch::kAction, p4orch::kSetSrcMac});
  attributes.push_back(
      swss::FieldValueTuple{prependParamField(p4orch::kSrcMac), kSrcMac2});
  attributes.push_back(
      swss::FieldValueTuple{p4orch::kControllerMetadata, "so_meta"});

  Enqueue(APP_P4RT_MULTICAST_ROUTER_INTERFACE_TABLE_NAME,
          swss::KeyOpFieldsValuesTuple(appl_db_key, SET_COMMAND, attributes));

  EXPECT_CALL(mock_sai_router_intf_, create_router_interface(_, _, _, _))
      .WillOnce(DoAll(SetArgPointee<0>(kRifOid1), Return(SAI_STATUS_SUCCESS)));
  EXPECT_CALL(publisher_,
              publish(Eq(APP_P4RT_TABLE_NAME), Eq(appl_db_key), Eq(attributes),
                      Eq(StatusCode::SWSS_RC_SUCCESS), Eq(true)));
  EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS, Drain(/*failure_before=*/false));
  auto* actual_entry = GetMulticastRouterInterfaceEntry(
      expect_entry.multicast_router_interface_entry_key);
  ASSERT_NE(nullptr, actual_entry);
  VerifyP4MulticastRouterInterfaceEntryEqual(expect_entry, *actual_entry);
  auto end_rifOid = GetRifOid(actual_entry);
  EXPECT_EQ(end_rifOid, kRifOid1);
}

TEST_F(L3MulticastManagerTest,
       DrainMulticastRouterInterfaceEntryMultiOpSameRif) {
  // We will create 2 router interface entries that will share a RIF (meaning
  // that all that differs is the "multicast_replica_instance".)
  const std::string match_key1 =
      R"({"match/multicast_replica_port":"Ethernet1",)"
      R"("match/multicast_replica_instance":"0x1"})";
  const std::string appl_db_key1 =
      std::string(APP_P4RT_MULTICAST_ROUTER_INTERFACE_TABLE_NAME) +
      kTableKeyDelimiter + match_key1;
  const std::string match_key2 =
      R"({"match/multicast_replica_port":"Ethernet1",)"
      R"("match/multicast_replica_instance":"0x2"})";
  const std::string appl_db_key2 =
      std::string(APP_P4RT_MULTICAST_ROUTER_INTERFACE_TABLE_NAME) +
      kTableKeyDelimiter + match_key2;

  // Enqueue entry for create operation.
  auto expect_entry1 = GenerateP4MulticastRouterInterfaceEntry(
      "Ethernet1", "0x1", swss::MacAddress(kSrcMac1));
  expect_entry1.router_interface_oid = kRifOid1;
  auto expect_entry2 = GenerateP4MulticastRouterInterfaceEntry(
      "Ethernet1", "0x2", swss::MacAddress(kSrcMac1));
  expect_entry2.router_interface_oid = kRifOid1;

  std::vector<swss::FieldValueTuple> attributes;
  attributes.push_back(
      swss::FieldValueTuple{p4orch::kAction, p4orch::kSetSrcMac});
  attributes.push_back(
      swss::FieldValueTuple{prependParamField(p4orch::kSrcMac), kSrcMac1});
  attributes.push_back(
      swss::FieldValueTuple{p4orch::kControllerMetadata, "so_meta"});

  // Enqueue add operation.
  Enqueue(APP_P4RT_MULTICAST_ROUTER_INTERFACE_TABLE_NAME,
          swss::KeyOpFieldsValuesTuple(appl_db_key1, SET_COMMAND, attributes));

  EXPECT_CALL(mock_sai_router_intf_, create_router_interface(_, _, _, _))
      .WillOnce(DoAll(SetArgPointee<0>(kRifOid1), Return(SAI_STATUS_SUCCESS)));
  EXPECT_CALL(publisher_,
              publish(Eq(APP_P4RT_TABLE_NAME), Eq(appl_db_key1), Eq(attributes),
                      Eq(StatusCode::SWSS_RC_SUCCESS), Eq(true)));
  EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS, Drain(/*failure_before=*/false));

  // Enqueue second add and then a delete operation of previous.
  // RIF shouldn't be impacted.
  Enqueue(APP_P4RT_MULTICAST_ROUTER_INTERFACE_TABLE_NAME,
          swss::KeyOpFieldsValuesTuple(appl_db_key2, SET_COMMAND, attributes));
  Enqueue(APP_P4RT_MULTICAST_ROUTER_INTERFACE_TABLE_NAME,
          swss::KeyOpFieldsValuesTuple(appl_db_key1, DEL_COMMAND, attributes));
  EXPECT_CALL(publisher_,
              publish(Eq(APP_P4RT_TABLE_NAME), Eq(appl_db_key2), Eq(attributes),
                      Eq(StatusCode::SWSS_RC_SUCCESS), Eq(true)));
  EXPECT_CALL(publisher_,
              publish(Eq(APP_P4RT_TABLE_NAME), Eq(appl_db_key1), Eq(attributes),
                      Eq(StatusCode::SWSS_RC_SUCCESS), Eq(true)));
  EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS, Drain(/*failure_before=*/false));

  auto* actual_entry1 = GetMulticastRouterInterfaceEntry(
      expect_entry1.multicast_router_interface_entry_key);
  auto* actual_entry2 = GetMulticastRouterInterfaceEntry(
      expect_entry2.multicast_router_interface_entry_key);
  ASSERT_EQ(nullptr, actual_entry1);  // since deleted
  ASSERT_NE(nullptr, actual_entry2);
  VerifyP4MulticastRouterInterfaceEntryEqual(expect_entry2, *actual_entry2);
  auto end_rifOid = GetRifOid(actual_entry2);
  EXPECT_EQ(end_rifOid, kRifOid1);
}

TEST_F(L3MulticastManagerTest,
       DrainMulticastRouterInterfaceEntryMultiOpWithFailure) {
  // Create 2 router interface entries requiring 2 RIFs, but have second one
  // fail.
  const std::string match_key1 =
      R"({"match/multicast_replica_port":"Ethernet1",)"
      R"("match/multicast_replica_instance":"0x1"})";
  const std::string appl_db_key1 =
      std::string(APP_P4RT_MULTICAST_ROUTER_INTERFACE_TABLE_NAME) +
      kTableKeyDelimiter + match_key1;
  const std::string match_key2 =
      R"({"match/multicast_replica_port":"Ethernet2",)"
      R"("match/multicast_replica_instance":"0x2"})";
  const std::string appl_db_key2 =
      std::string(APP_P4RT_MULTICAST_ROUTER_INTERFACE_TABLE_NAME) +
      kTableKeyDelimiter + match_key2;

  // Enqueue entry for create operation.
  auto expect_entry1 = GenerateP4MulticastRouterInterfaceEntry(
      "Ethernet1", "0x1", swss::MacAddress(kSrcMac1));
  expect_entry1.router_interface_oid = kRifOid1;
  auto expect_entry2 = GenerateP4MulticastRouterInterfaceEntry(
      "Ethernet2", "0x2", swss::MacAddress(kSrcMac1));
  expect_entry2.router_interface_oid = kRifOid2;

  std::vector<swss::FieldValueTuple> attributes1;
  attributes1.push_back(
      swss::FieldValueTuple{p4orch::kAction, p4orch::kSetSrcMac});
  attributes1.push_back(
      swss::FieldValueTuple{prependParamField(p4orch::kSrcMac), kSrcMac1});
  attributes1.push_back(
      swss::FieldValueTuple{p4orch::kControllerMetadata, "so_meta"});

  std::vector<swss::FieldValueTuple> attributes2;
  attributes2.push_back(
      swss::FieldValueTuple{p4orch::kAction, p4orch::kSetSrcMac});
  attributes2.push_back(
      swss::FieldValueTuple{prependParamField(p4orch::kSrcMac), kSrcMac2});
  attributes2.push_back(
      swss::FieldValueTuple{p4orch::kControllerMetadata, "so_meta"});

  // Enqueue add operation.
  Enqueue(APP_P4RT_MULTICAST_ROUTER_INTERFACE_TABLE_NAME,
          swss::KeyOpFieldsValuesTuple(appl_db_key1, SET_COMMAND, attributes1));

  EXPECT_CALL(mock_sai_router_intf_, create_router_interface(_, _, _, _))
      .WillOnce(DoAll(SetArgPointee<0>(kRifOid1), Return(SAI_STATUS_SUCCESS)));
  EXPECT_CALL(publisher_, publish(Eq(APP_P4RT_TABLE_NAME), Eq(appl_db_key1),
                                  Eq(attributes1),
                                  Eq(StatusCode::SWSS_RC_SUCCESS), Eq(true)));
  EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS, Drain(/*failure_before=*/false));

  // Enqueue second add that will fail and a delete operation of previous.
  Enqueue(APP_P4RT_MULTICAST_ROUTER_INTERFACE_TABLE_NAME,
          swss::KeyOpFieldsValuesTuple(appl_db_key2, SET_COMMAND, attributes2));
  Enqueue(APP_P4RT_MULTICAST_ROUTER_INTERFACE_TABLE_NAME,
          swss::KeyOpFieldsValuesTuple(appl_db_key1, DEL_COMMAND, attributes1));
  // Have SAI fail.
  EXPECT_CALL(mock_sai_router_intf_, create_router_interface(_, _, _, _))
      .WillOnce(Return(SAI_STATUS_FAILURE));
  EXPECT_CALL(publisher_, publish(Eq(APP_P4RT_TABLE_NAME), Eq(appl_db_key2),
                                  Eq(attributes2),
                                  Eq(StatusCode::SWSS_RC_UNKNOWN), Eq(true)));
  EXPECT_CALL(
      publisher_,
      publish(Eq(APP_P4RT_TABLE_NAME), Eq(appl_db_key1), Eq(attributes1),
              Eq(StatusCode::SWSS_RC_NOT_EXECUTED), Eq(true)));
  EXPECT_EQ(StatusCode::SWSS_RC_UNKNOWN, Drain(/*failure_before=*/false));

  auto* actual_entry1 = GetMulticastRouterInterfaceEntry(
      expect_entry1.multicast_router_interface_entry_key);
  auto* actual_entry2 = GetMulticastRouterInterfaceEntry(
      expect_entry2.multicast_router_interface_entry_key);
  ASSERT_NE(nullptr, actual_entry1);  // since delete was not executed
  ASSERT_EQ(nullptr, actual_entry2);  // since create failed
  VerifyP4MulticastRouterInterfaceEntryEqual(expect_entry1, *actual_entry1);
  auto end_rifOid = GetRifOid(actual_entry1);
  EXPECT_EQ(end_rifOid, kRifOid1);
}

TEST_F(L3MulticastManagerTest, DrainMulticastRouterInterfaceEntryInvalidAdd) {
  // Missing multicast_replica_instance makes this invalid.
  const std::string match_key =
      R"({"match/multicast_replica_port":"Ethernet4"})";
  const std::string appl_db_key =
      std::string(APP_P4RT_MULTICAST_ROUTER_INTERFACE_TABLE_NAME) +
      kTableKeyDelimiter + match_key;

  std::vector<swss::FieldValueTuple> attributes;
  attributes.push_back(
      swss::FieldValueTuple{p4orch::kAction, p4orch::kSetSrcMac});

  attributes.push_back(
      swss::FieldValueTuple{prependParamField(p4orch::kSrcMac), kSrcMac2});
  attributes.push_back(
      swss::FieldValueTuple{p4orch::kControllerMetadata, "so_meta"});

  // Enqueue entry for create operation.
  Enqueue(APP_P4RT_MULTICAST_ROUTER_INTERFACE_TABLE_NAME,
          swss::KeyOpFieldsValuesTuple(appl_db_key, SET_COMMAND, attributes));

  EXPECT_CALL(publisher_,
              publish(Eq(APP_P4RT_TABLE_NAME), Eq(appl_db_key), Eq(attributes),
                      Eq(StatusCode::SWSS_RC_INVALID_PARAM), Eq(true)));
  EXPECT_EQ(StatusCode::SWSS_RC_INVALID_PARAM, Drain(/*failure_before=*/false));
}

TEST_F(L3MulticastManagerTest,
       DrainMulticastRouterInterfaceEntryAddCreateFails) {
  const std::string match_key =
      R"({"match/multicast_replica_port":"Ethernet4",)"
      R"("match/multicast_replica_instance":"0x1"})";
  const std::string appl_db_key =
      std::string(APP_P4RT_MULTICAST_ROUTER_INTERFACE_TABLE_NAME) +
      kTableKeyDelimiter + match_key;

  // Enqueue entry for create operation.
  auto expect_entry = GenerateP4MulticastRouterInterfaceEntry(
      "Ethernet4", "0x1", swss::MacAddress(kSrcMac2));
  expect_entry.router_interface_oid = kRifOid1;
  auto start_rifOid = GetRifOid(&expect_entry);
  EXPECT_EQ(start_rifOid, SAI_NULL_OBJECT_ID);

  std::vector<swss::FieldValueTuple> attributes;
  attributes.push_back(
      swss::FieldValueTuple{p4orch::kAction, p4orch::kSetSrcMac});
  attributes.push_back(
      swss::FieldValueTuple{prependParamField(p4orch::kSrcMac), kSrcMac2});
  attributes.push_back(
      swss::FieldValueTuple{p4orch::kControllerMetadata, "so_meta"});

  Enqueue(APP_P4RT_MULTICAST_ROUTER_INTERFACE_TABLE_NAME,
          swss::KeyOpFieldsValuesTuple(appl_db_key, SET_COMMAND, attributes));

  // SAI fails.
  EXPECT_CALL(mock_sai_router_intf_, create_router_interface(_, _, _, _))
      .WillOnce(Return(SAI_STATUS_FAILURE));
  EXPECT_CALL(publisher_,
              publish(Eq(APP_P4RT_TABLE_NAME), Eq(appl_db_key), Eq(attributes),
                      Eq(StatusCode::SWSS_RC_UNKNOWN), Eq(true)));
  EXPECT_EQ(StatusCode::SWSS_RC_UNKNOWN, Drain(/*failure_before=*/false));
}

TEST_F(L3MulticastManagerTest,
       DrainMulticastRouterInterfaceEntryUpdateToSameValue) {
  const std::string match_key =
      R"({"match/multicast_replica_port":"Ethernet2",)"
      R"("match/multicast_replica_instance":"0x2"})";
  const std::string appl_db_key =
      std::string(APP_P4RT_MULTICAST_ROUTER_INTERFACE_TABLE_NAME) +
      kTableKeyDelimiter + match_key;

  // Enqueue entry for create operation.
  auto expect_entry = GenerateP4MulticastRouterInterfaceEntry(
      "Ethernet2", "0x2", swss::MacAddress(kSrcMac2));
  expect_entry.router_interface_oid = kRifOid2;

  std::vector<swss::FieldValueTuple> attributes;
  attributes.push_back(
      swss::FieldValueTuple{p4orch::kAction, p4orch::kSetSrcMac});
  attributes.push_back(
      swss::FieldValueTuple{prependParamField(p4orch::kSrcMac), kSrcMac2});
  attributes.push_back(
      swss::FieldValueTuple{p4orch::kControllerMetadata, "so_meta"});

  // Add first entry.
  Enqueue(APP_P4RT_MULTICAST_ROUTER_INTERFACE_TABLE_NAME,
          swss::KeyOpFieldsValuesTuple(appl_db_key, SET_COMMAND, attributes));

  // Called once for first entry.
  EXPECT_CALL(mock_sai_router_intf_, create_router_interface(_, _, _, _))
      .WillOnce(DoAll(SetArgPointee<0>(kRifOid2), Return(SAI_STATUS_SUCCESS)));
  EXPECT_CALL(publisher_,
              publish(Eq(APP_P4RT_TABLE_NAME), Eq(appl_db_key), Eq(attributes),
                      Eq(StatusCode::SWSS_RC_SUCCESS), Eq(true)));
  EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS, Drain(/*failure_before=*/false));

  // Enqueue the same entry.  Operation should be a successful no-op.
  Enqueue(APP_P4RT_MULTICAST_ROUTER_INTERFACE_TABLE_NAME,
          swss::KeyOpFieldsValuesTuple(appl_db_key, SET_COMMAND, attributes));
  EXPECT_CALL(publisher_,
              publish(Eq(APP_P4RT_TABLE_NAME), Eq(appl_db_key), Eq(attributes),
                      Eq(StatusCode::SWSS_RC_SUCCESS), Eq(true)));
  EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS, Drain(/*failure_before=*/false));

  // Confirm entries exist and are as expected.
  auto* actual_entry = GetMulticastRouterInterfaceEntry(
      expect_entry.multicast_router_interface_entry_key);
  ASSERT_NE(nullptr, actual_entry);
  VerifyP4MulticastRouterInterfaceEntryEqual(expect_entry, *actual_entry);
  auto end_rifOid = GetRifOid(actual_entry);
  EXPECT_EQ(end_rifOid, kRifOid2);
}

TEST_F(L3MulticastManagerTest,
       DrainMulticastRouterInterfaceEntryUpdateSuccess) {
  const std::string match_key =
      R"({"match/multicast_replica_port":"Ethernet2",)"
      R"("match/multicast_replica_instance":"0x2"})";
  const std::string appl_db_key =
      std::string(APP_P4RT_MULTICAST_ROUTER_INTERFACE_TABLE_NAME) +
      kTableKeyDelimiter + match_key;

  // Enqueue entry for create operation.
  std::vector<swss::FieldValueTuple> attributes;
  attributes.push_back(
      swss::FieldValueTuple{p4orch::kAction, p4orch::kSetSrcMac});
  attributes.push_back(
      swss::FieldValueTuple{prependParamField(p4orch::kSrcMac), kSrcMac2});
  attributes.push_back(
      swss::FieldValueTuple{p4orch::kControllerMetadata, "so_meta"});

  // Add first entry.
  Enqueue(APP_P4RT_MULTICAST_ROUTER_INTERFACE_TABLE_NAME,
          swss::KeyOpFieldsValuesTuple(appl_db_key, SET_COMMAND, attributes));

  // Called once for first entry.
  EXPECT_CALL(mock_sai_router_intf_, create_router_interface(_, _, _, _))
      .WillOnce(DoAll(SetArgPointee<0>(kRifOid2), Return(SAI_STATUS_SUCCESS)));
  EXPECT_CALL(publisher_,
              publish(Eq(APP_P4RT_TABLE_NAME), Eq(appl_db_key), Eq(attributes),
                      Eq(StatusCode::SWSS_RC_SUCCESS), Eq(true)));
  EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS, Drain(/*failure_before=*/false));

  // Enqueue change to entry.  Operation should be successful change.
  auto expect_entry = GenerateP4MulticastRouterInterfaceEntry(
      "Ethernet2", "0x2", swss::MacAddress(kSrcMac3));
  expect_entry.router_interface_oid = kRifOid3;

  std::vector<swss::FieldValueTuple> attributes2;
  attributes2.push_back(
      swss::FieldValueTuple{p4orch::kAction, p4orch::kSetSrcMac});
  attributes2.push_back(
      swss::FieldValueTuple{prependParamField(p4orch::kSrcMac), kSrcMac3});
  attributes2.push_back(
      swss::FieldValueTuple{p4orch::kControllerMetadata, "so_meta"});

  Enqueue(APP_P4RT_MULTICAST_ROUTER_INTERFACE_TABLE_NAME,
          swss::KeyOpFieldsValuesTuple(appl_db_key, SET_COMMAND, attributes2));
  EXPECT_CALL(mock_sai_router_intf_, create_router_interface(_, _, _, _))
      .WillOnce(DoAll(SetArgPointee<0>(kRifOid3), Return(SAI_STATUS_SUCCESS)));
  EXPECT_CALL(mock_sai_router_intf_, remove_router_interface(kRifOid2))
      .WillOnce(Return(SAI_STATUS_SUCCESS));
  EXPECT_CALL(publisher_,
              publish(Eq(APP_P4RT_TABLE_NAME), Eq(appl_db_key), Eq(attributes2),
                      Eq(StatusCode::SWSS_RC_SUCCESS), Eq(true)));
  EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS, Drain(/*failure_before=*/false));

  // Confirm entry was successfully updated.
  auto* actual_entry = GetMulticastRouterInterfaceEntry(
      expect_entry.multicast_router_interface_entry_key);
  ASSERT_NE(nullptr, actual_entry);
  VerifyP4MulticastRouterInterfaceEntryEqual(expect_entry, *actual_entry);
  auto end_rifOid = GetRifOid(actual_entry);
  EXPECT_EQ(end_rifOid, kRifOid3);
}

TEST_F(L3MulticastManagerTest,
       DrainMulticastRouterInterfaceEntryDeleteInvalid) {
  // Missing multicast_replica_instance makes this invalid.
  const std::string match_key =
      R"({"match/multicast_replica_port":"Ethernet1",)"
      R"("match/multicast_replica_instance":"0x1"})";
  const std::string appl_db_key =
      std::string(APP_P4RT_MULTICAST_ROUTER_INTERFACE_TABLE_NAME) +
      kTableKeyDelimiter + match_key;

  std::vector<swss::FieldValueTuple> attributes;
  attributes.push_back(
      swss::FieldValueTuple{p4orch::kAction, p4orch::kSetSrcMac});

  attributes.push_back(
      swss::FieldValueTuple{prependParamField(p4orch::kSrcMac), kSrcMac1});
  attributes.push_back(
      swss::FieldValueTuple{p4orch::kControllerMetadata, "so_meta"});

  // Enqueue entry for create operation.
  Enqueue(APP_P4RT_MULTICAST_ROUTER_INTERFACE_TABLE_NAME,
          swss::KeyOpFieldsValuesTuple(appl_db_key, DEL_COMMAND, attributes));

  EXPECT_CALL(publisher_,
              publish(Eq(APP_P4RT_TABLE_NAME), Eq(appl_db_key), Eq(attributes),
                      Eq(StatusCode::SWSS_RC_NOT_FOUND), Eq(true)));
  EXPECT_EQ(StatusCode::SWSS_RC_NOT_FOUND, Drain(/*failure_before=*/false));
}

TEST_F(L3MulticastManagerTest, DrainFirstEntryFailurePublishesCorrectNumber) {
  const std::string match_key =
      R"({"match/multicast_replica_port":"Ethernet4",)"
      R"("match/multicast_replica_instance":"0x1"})";
  const std::string appl_db_key =
      std::string(APP_P4RT_MULTICAST_ROUTER_INTERFACE_TABLE_NAME) +
      kTableKeyDelimiter + match_key;

  std::vector<swss::FieldValueTuple> attributes;
  attributes.push_back(
      swss::FieldValueTuple{p4orch::kAction, p4orch::kSetSrcMac});
  attributes.push_back(
      swss::FieldValueTuple{prependParamField(p4orch::kSrcMac), kSrcMac2});
  attributes.push_back(
      swss::FieldValueTuple{p4orch::kControllerMetadata, "so_meta"});

  const std::string group_match_key =
      R"({"match/multicast_group_id":"0x1",)"
      R"("match/multicast_replica_port":"Ethernet1",)"
      R"("match/multicast_replica_instance":"0x1"})";
  const std::string group_appl_db_key =
      std::string(APP_P4RT_REPLICATION_L2_MULTICAST_TABLE_NAME) +
      kTableKeyDelimiter + group_match_key;
  std::vector<swss::FieldValueTuple> group_attributes;
  group_attributes.push_back(
      swss::FieldValueTuple{p4orch::kControllerMetadata, "so_meta"});

  Enqueue(APP_P4RT_MULTICAST_ROUTER_INTERFACE_TABLE_NAME,
          swss::KeyOpFieldsValuesTuple(appl_db_key, SET_COMMAND, attributes));
  Enqueue(APP_P4RT_MULTICAST_ROUTER_INTERFACE_TABLE_NAME,
          swss::KeyOpFieldsValuesTuple(group_appl_db_key, SET_COMMAND,
                                       group_attributes));

  // Create operation fails, which forces second entries to be unexecuted.
  EXPECT_CALL(mock_sai_router_intf_, create_router_interface(_, _, _, _))
      .WillOnce(Return(SAI_STATUS_FAILURE));
  EXPECT_CALL(publisher_,
              publish(Eq(APP_P4RT_TABLE_NAME), Eq(appl_db_key), Eq(attributes),
                      Eq(StatusCode::SWSS_RC_UNKNOWN), Eq(true)));
  EXPECT_CALL(publisher_,
              publish(Eq(APP_P4RT_TABLE_NAME), Eq(group_appl_db_key),
                      Eq(group_attributes),
                      Eq(StatusCode::SWSS_RC_NOT_EXECUTED), Eq(true)));
  EXPECT_EQ(StatusCode::SWSS_RC_UNKNOWN, Drain(/*failure_before=*/false));
}

TEST_F(L3MulticastManagerTest, ValidateSetMulticastRouterInterfaceEntryTest) {
  auto entry = GenerateP4MulticastRouterInterfaceEntry(
      "Ethernet2", "0x0", swss::MacAddress(kSrcMac1));
  ReturnCode status = ValidateMulticastRouterInterfaceEntry(entry, SET_COMMAND);
  EXPECT_TRUE(status.ok());
}

TEST_F(L3MulticastManagerTest, ValidateDelMulticastRouterInterfaceEntryNoOid) {
  auto entry = SetupP4MulticastRouterInterfaceEntry(
      "Ethernet2", "0x0", swss::MacAddress(kSrcMac1), kRifOid1);
  // Force delete OID.
  ForceRemoveRifOid(kRifOid1);
  ReturnCode status = ValidateMulticastRouterInterfaceEntry(entry, DEL_COMMAND);
  EXPECT_EQ(StatusCode::SWSS_RC_NOT_FOUND, status);
}

TEST_F(L3MulticastManagerTest,
       ValidateSetMulticastRouterInterfaceEntryEmptyPortTest) {
  auto entry = GenerateP4MulticastRouterInterfaceEntry(
      "", "0x0", swss::MacAddress(kSrcMac1));
  ReturnCode status = ValidateMulticastRouterInterfaceEntry(entry, SET_COMMAND);
  EXPECT_EQ(StatusCode::SWSS_RC_INVALID_PARAM, status);
}

TEST_F(L3MulticastManagerTest,
       ValidateSetMulticastRouterInterfaceEntryEmptyInstanceTest) {
  auto entry = GenerateP4MulticastRouterInterfaceEntry(
      "Ethernet2", "", swss::MacAddress(kSrcMac1));
  ReturnCode status = ValidateMulticastRouterInterfaceEntry(entry, SET_COMMAND);
  EXPECT_EQ(StatusCode::SWSS_RC_INVALID_PARAM, status);
}

TEST_F(L3MulticastManagerTest,
       ValidateSetMulticastRouterInterfaceEntryUpdateTestNoOid) {
  auto entry = SetupP4MulticastRouterInterfaceEntry(
      "Ethernet1", "0x0", swss::MacAddress(kSrcMac1), kRifOid1);
  // Force clear OID.
  P4MulticastRouterInterfaceEntry* actual_entry_ptr =
      GetMulticastRouterInterfaceEntry(
          entry.multicast_router_interface_entry_key);
  actual_entry_ptr->router_interface_oid = SAI_OBJECT_TYPE_NULL;

  auto entry2 = GenerateP4MulticastRouterInterfaceEntry(
      "Ethernet1", "0x0", swss::MacAddress(kSrcMac2));
  ReturnCode status =
      ValidateMulticastRouterInterfaceEntry(entry2, SET_COMMAND);
  EXPECT_EQ(StatusCode::SWSS_RC_NOT_FOUND, status);
}

TEST_F(L3MulticastManagerTest,
       ValidateSetMulticastRouterInterfaceEntryUpdateTestMissinginMapper) {
  auto entry = SetupP4MulticastRouterInterfaceEntry(
      "Ethernet1", "0x0", swss::MacAddress(kSrcMac1), kRifOid1);
  // Force delete OID from central mapper.
  std::string rif_key = KeyGenerator::generateMulticastRouterInterfaceRifKey(
      entry.multicast_replica_port, entry.src_mac);
  p4_oid_mapper_.eraseOID(SAI_OBJECT_TYPE_ROUTER_INTERFACE, rif_key);

  auto entry2 = GenerateP4MulticastRouterInterfaceEntry(
      "Ethernet1", "0x0", swss::MacAddress(kSrcMac2));
  ReturnCode status =
      ValidateMulticastRouterInterfaceEntry(entry2, SET_COMMAND);
  EXPECT_EQ(StatusCode::SWSS_RC_NOT_FOUND, status);
}

TEST_F(L3MulticastManagerTest,
       ValidateSetMulticastRouterInterfaceEntryUpdateTestNoOidInEntry) {
  auto entry = SetupP4MulticastRouterInterfaceEntry(
      "Ethernet1", "0x0", swss::MacAddress(kSrcMac1), kRifOid1);

  // Force clear the router interface OID from the entry.
  auto* actual_entry = GetMulticastRouterInterfaceEntry(
      entry.multicast_router_interface_entry_key);
  ASSERT_NE(actual_entry, nullptr);
  actual_entry->router_interface_oid = SAI_OBJECT_TYPE_NULL;

  auto entry2 = GenerateP4MulticastRouterInterfaceEntry(
      "Ethernet1", "0x0", swss::MacAddress(kSrcMac2));
  ReturnCode status =
      ValidateMulticastRouterInterfaceEntry(entry2, SET_COMMAND);
  EXPECT_EQ(StatusCode::SWSS_RC_NOT_FOUND, status);
}

TEST_F(L3MulticastManagerTest,
       ValidateSetMulticastRouterInterfaceEntryUpdateTestNoOidList) {
  auto entry = SetupP4MulticastRouterInterfaceEntry(
      "Ethernet1", "0x0", swss::MacAddress(kSrcMac1), kRifOid1);
  // Force clear the router interface OID internal map to cause an error.
  ForceRemoveRifOid(kRifOid1);

  auto entry2 = GenerateP4MulticastRouterInterfaceEntry(
      "Ethernet1", "0x0", swss::MacAddress(kSrcMac2));
  ReturnCode status =
      ValidateMulticastRouterInterfaceEntry(entry2, SET_COMMAND);
  EXPECT_EQ(StatusCode::SWSS_RC_NOT_FOUND, status);
}

TEST_F(L3MulticastManagerTest,
       ValidateDelMulticastRouterInterfaceEntryNoEntryTest) {
  auto entry = GenerateP4MulticastRouterInterfaceEntry(
      "Ethernet2", "0x0", swss::MacAddress(kSrcMac1));
  ReturnCode status = ValidateMulticastRouterInterfaceEntry(entry, DEL_COMMAND);
  EXPECT_EQ(StatusCode::SWSS_RC_NOT_FOUND, status);
}

TEST_F(L3MulticastManagerTest,
       ValidateDelMulticastRouterInterfaceEntryForceCritical) {
  auto entry = SetupP4MulticastRouterInterfaceEntry(
      "Ethernet2", "0x0", swss::MacAddress(kSrcMac1), kRifOid1);
  // Force delete OID from mapper.
  std::string rif_key = KeyGenerator::generateMulticastRouterInterfaceRifKey(
      entry.multicast_replica_port, entry.src_mac);
  p4_oid_mapper_.eraseOID(SAI_OBJECT_TYPE_ROUTER_INTERFACE, rif_key);

  ReturnCode status = ValidateMulticastRouterInterfaceEntry(entry, DEL_COMMAND);
  EXPECT_EQ(StatusCode::SWSS_RC_INTERNAL, status);
}

TEST_F(L3MulticastManagerTest,
       ValidateMulticastRouterInterfaceEntryUnknownOperationTest) {
  auto entry = GenerateP4MulticastRouterInterfaceEntry(
      "Ethernet2", "0x0", swss::MacAddress(kSrcMac1));
  ReturnCode status =
      ValidateMulticastRouterInterfaceEntry(entry, "unknown_op");
  EXPECT_EQ(StatusCode::SWSS_RC_INVALID_PARAM, status);
}

TEST_F(L3MulticastManagerTest, VerifyStateMulticastRouterInterfaceTestSuccess) {
  auto entry = SetupP4MulticastRouterInterfaceEntry(
      "Ethernet1", "0x1", swss::MacAddress(kSrcMac1), kRifOid1);

  const std::string match_key =
      R"({"match/multicast_replica_port":"Ethernet1",)"
      R"("match/multicast_replica_instance":"0x1"})";
  const std::string appl_db_key =
      std::string(APP_P4RT_MULTICAST_ROUTER_INTERFACE_TABLE_NAME) +
      kTableKeyDelimiter + match_key;
  const std::string db_key = std::string(APP_P4RT_TABLE_NAME) +
                           kTableKeyDelimiter + appl_db_key;
  std::vector<swss::FieldValueTuple> attributes;
  attributes.push_back(
      swss::FieldValueTuple{p4orch::kAction, p4orch::kSetSrcMac});
  attributes.push_back(swss::FieldValueTuple{
      prependParamField(p4orch::kSrcMac), kSrcMac1});
  attributes.push_back(
      swss::FieldValueTuple{p4orch::kControllerMetadata, "so_meta"});

  // Setup ASIC DB.
  swss::Table table(nullptr, "ASIC_STATE");
  table.set(
      "SAI_OBJECT_TYPE_ROUTER_INTERFACE:oid:0x123456",
      std::vector<swss::FieldValueTuple>{
          swss::FieldValueTuple{"SAI_ROUTER_INTERFACE_ATTR_VIRTUAL_ROUTER_ID",
                                "oid:0x0"},
          swss::FieldValueTuple{"SAI_ROUTER_INTERFACE_ATTR_SRC_MAC_ADDRESS",
                                "00:01:02:03:04:05"},
          swss::FieldValueTuple{"SAI_ROUTER_INTERFACE_ATTR_TYPE",
                                "SAI_ROUTER_INTERFACE_TYPE_PORT"},
          swss::FieldValueTuple{"SAI_ROUTER_INTERFACE_ATTR_PORT_ID",
                                "oid:0x112233"},
          swss::FieldValueTuple{"SAI_ROUTER_INTERFACE_ATTR_V4_MCAST_ENABLE",
                                "true"},
          swss::FieldValueTuple{"SAI_ROUTER_INTERFACE_ATTR_V6_MCAST_ENABLE",
                                "true"},
          swss::FieldValueTuple{"SAI_ROUTER_INTERFACE_ATTR_MTU", "1500"}});

  // Verification should succeed with vaild key and value.
  EXPECT_EQ(VerifyState(db_key, attributes), "");
}

TEST_F(L3MulticastManagerTest,
       VerifyStateMulticastRouterInterfaceTestMissingAsicDb) {
  auto entry = SetupP4MulticastRouterInterfaceEntry(
      "Ethernet1", "0x1", swss::MacAddress(kSrcMac1), kRifOid1);

  const std::string match_key =
      R"({"match/multicast_replica_port":"Ethernet1",)"
      R"("match/multicast_replica_instance":"0x1"})";
  const std::string appl_db_key =
      std::string(APP_P4RT_MULTICAST_ROUTER_INTERFACE_TABLE_NAME) +
      kTableKeyDelimiter + match_key;
  const std::string db_key = std::string(APP_P4RT_TABLE_NAME) +
                           kTableKeyDelimiter + appl_db_key;

  std::vector<swss::FieldValueTuple> attributes;
  attributes.push_back(
      swss::FieldValueTuple{p4orch::kAction, p4orch::kSetSrcMac});
  // Use wrong source mac so state cache fails.
  attributes.push_back(swss::FieldValueTuple{
      prependParamField(p4orch::kSrcMac), kSrcMac2});
  attributes.push_back(
      swss::FieldValueTuple{p4orch::kControllerMetadata, "so_meta"});

  // Setup ASIC DB.
  swss::Table table(nullptr, "ASIC_STATE");
  table.set(
      "SAI_OBJECT_TYPE_ROUTER_INTERFACE:oid:0x123456",
      std::vector<swss::FieldValueTuple>{
          swss::FieldValueTuple{"SAI_ROUTER_INTERFACE_ATTR_VIRTUAL_ROUTER_ID",
                                "oid:0x0"},
          swss::FieldValueTuple{"SAI_ROUTER_INTERFACE_ATTR_SRC_MAC_ADDRESS",
                                "00:01:02:03:04:05"},
          swss::FieldValueTuple{"SAI_ROUTER_INTERFACE_ATTR_TYPE",
                                "SAI_ROUTER_INTERFACE_TYPE_PORT"},
          swss::FieldValueTuple{"SAI_ROUTER_INTERFACE_ATTR_PORT_ID",
                                "oid:0x112233"},
          // These should be true.
          swss::FieldValueTuple{"SAI_ROUTER_INTERFACE_ATTR_V4_MCAST_ENABLE",
                               "false"},
          swss::FieldValueTuple{"SAI_ROUTER_INTERFACE_ATTR_V6_MCAST_ENABLE",
                                "false"},
          swss::FieldValueTuple{"SAI_ROUTER_INTERFACE_ATTR_MTU", "1500"}});

  // Verification should fail, since values do not match.
  EXPECT_FALSE(VerifyState(db_key, attributes).empty());

  // No key should also fail.
  table.del("SAI_OBJECT_TYPE_ROUTER_INTERFACE:oid:0x123456");
  EXPECT_FALSE(VerifyState(db_key, attributes).empty());
}

TEST_F(L3MulticastManagerTest, VerifyStateMulticastRouterInterfaceTestBadKeys) {
  const std::string match_key =
      R"({"match/multicast_replica_port":"Ethernet1",)"
      R"("match/multicast_replica_instance":"0x1"})";
  const std::string appl_db_key =
      std::string(APP_P4RT_MULTICAST_ROUTER_INTERFACE_TABLE_NAME) +
      kTableKeyDelimiter + match_key;
  std::vector<swss::FieldValueTuple> attributes;

  const std::string no_delim = "p4rttable";
  EXPECT_EQ(VerifyState(no_delim, attributes),
            "Invalid key, missing delimiter: p4rttable");

  const std::string not_p4rt = std::string("Wrong") +
                               kTableKeyDelimiter + appl_db_key;
  EXPECT_EQ(VerifyState(not_p4rt, attributes),
            "Invalid key, unexpected P4RT table: " + not_p4rt);

  const std::string bad_appl_db_key =
      std::string(APP_P4RT_TUNNEL_TABLE_NAME) + kTableKeyDelimiter + match_key;
  const std::string bad_db_key = std::string(APP_P4RT_TABLE_NAME) +
      kTableKeyDelimiter + bad_appl_db_key;
  EXPECT_EQ(VerifyState(bad_db_key, attributes),
            "Invalid key, unexpected table name: " + bad_db_key);
}

TEST_F(L3MulticastManagerTest,
       VerifyStateMulticastRouterInterfaceTestBadEntries) {
  const std::string bad_match_key =
      R"({"match/multicast_replica_port":"Ethernet1"})";
  const std::string bad_appl_db_key =
      std::string(APP_P4RT_MULTICAST_ROUTER_INTERFACE_TABLE_NAME) +
      kTableKeyDelimiter + bad_match_key;
  const std::string bad_db_key = std::string(APP_P4RT_TABLE_NAME) +
                           kTableKeyDelimiter + bad_appl_db_key;

  const std::string match_key =
      R"({"match/multicast_replica_port":"Ethernet1",)"
      R"("match/multicast_replica_instance":"0x1"})";
  const std::string appl_db_key =
      std::string(APP_P4RT_MULTICAST_ROUTER_INTERFACE_TABLE_NAME) +
      kTableKeyDelimiter + match_key;
  const std::string db_key = std::string(APP_P4RT_TABLE_NAME) +
                           kTableKeyDelimiter + appl_db_key;
  std::vector<swss::FieldValueTuple> attributes;
  attributes.push_back(
      swss::FieldValueTuple{p4orch::kAction, p4orch::kSetSrcMac});
  attributes.push_back(swss::FieldValueTuple{
      prependParamField(p4orch::kSrcMac), kSrcMac1});
  attributes.push_back(
      swss::FieldValueTuple{p4orch::kControllerMetadata, "so_meta"});

  EXPECT_EQ(VerifyState(bad_db_key, attributes),
            "Unable to deserialize key '" + bad_match_key +
            "': Failed to deserialize multicast router interface table key");

  EXPECT_EQ(VerifyState(db_key, attributes),
            "No entry found with key '" + match_key + "'");
}

TEST_F(L3MulticastManagerTest,
       VerifyStateMulticastRouterInterfaceTestStateCacheFails) {
  P4MulticastRouterInterfaceEntry internal_entry =
      GenerateP4MulticastRouterInterfaceEntry(
          "Ethernet1", "0x1", swss::MacAddress(kSrcMac1), "meta1");
  internal_entry.router_interface_oid = kRifOid1;

  // Bad app db entry.
  P4MulticastRouterInterfaceEntry missing_multicast_replica_port =
      GenerateP4MulticastRouterInterfaceEntry(
          "", "0x1", swss::MacAddress(kSrcMac1));
  EXPECT_FALSE(VerifyMulticastRouterInterfaceStateCache(
      missing_multicast_replica_port, &internal_entry).empty());

  // Mismatch on key.
  P4MulticastRouterInterfaceEntry key_mismatch =
      GenerateP4MulticastRouterInterfaceEntry(
          "Ethernet2", "0x1", swss::MacAddress(kSrcMac1), "meta1");
  EXPECT_FALSE(VerifyMulticastRouterInterfaceStateCache(
      key_mismatch, &internal_entry).empty());

  // Mismatch on multicast_replica_port.
  P4MulticastRouterInterfaceEntry port_mismatch =
      GenerateP4MulticastRouterInterfaceEntry(
          "Ethernet1", "0x1", swss::MacAddress(kSrcMac1), "meta1");
  port_mismatch.multicast_replica_port = "Ethernet2";
  EXPECT_FALSE(VerifyMulticastRouterInterfaceStateCache(
      port_mismatch, &internal_entry).empty());

  // Mismatch on multicast_replica_instance.
  P4MulticastRouterInterfaceEntry instance_mismatch =
      GenerateP4MulticastRouterInterfaceEntry(
          "Ethernet1", "0x1", swss::MacAddress(kSrcMac1), "meta1");
  instance_mismatch.multicast_replica_instance = "0x2";
  EXPECT_FALSE(VerifyMulticastRouterInterfaceStateCache(
      instance_mismatch, &internal_entry).empty());

  // Mismatch on src_mac.
  P4MulticastRouterInterfaceEntry mac_mismatch =
      GenerateP4MulticastRouterInterfaceEntry(
          "Ethernet1", "0x1", swss::MacAddress(kSrcMac2), "meta1");
  EXPECT_FALSE(VerifyMulticastRouterInterfaceStateCache(
      mac_mismatch, &internal_entry).empty());

  // Mismatch on multicast_metadata.
  P4MulticastRouterInterfaceEntry metadata_mismatch =
      GenerateP4MulticastRouterInterfaceEntry(
          "Ethernet1", "0x1", swss::MacAddress(kSrcMac1), "meta2");
  EXPECT_FALSE(VerifyMulticastRouterInterfaceStateCache(
      metadata_mismatch, &internal_entry).empty());
}

TEST_F(L3MulticastManagerTest, CreateMulticastGroupFailureAlreadyInMapper) {
  auto entry = GenerateP4MulticastReplicationEntry("0x1", "Ethernet1", "0x1");
  sai_object_id_t group_oid = kGroupOid1;
  p4_oid_mapper_.setOID(SAI_OBJECT_TYPE_IPMC_GROUP, "0x1", group_oid);

  EXPECT_EQ(StatusCode::SWSS_RC_INTERNAL,
            CreateMulticastGroup(entry, &group_oid));
}

TEST_F(L3MulticastManagerTest, DeleteMulticastGroupFailureNotInMapper) {
  EXPECT_EQ(StatusCode::SWSS_RC_INTERNAL,
            DeleteMulticastGroup("0x1", kGroupOid1));
}

TEST_F(L3MulticastManagerTest,
       CreateMulticastGroupMemberFailureAlreadyInMapper) {
  auto entry = GenerateP4MulticastReplicationEntry("0x1", "Ethernet1", "0x1");
  sai_object_id_t group_member_oid = kGroupMemberOid1;
  p4_oid_mapper_.setOID(SAI_OBJECT_TYPE_IPMC_GROUP_MEMBER,
                        entry.multicast_replication_key, group_member_oid);

  EXPECT_EQ(StatusCode::SWSS_RC_INTERNAL,
            CreateMulticastGroupMember(entry, kRifOid1, &group_member_oid));
}

TEST_F(L3MulticastManagerTest, CreateMulticastGroupMemberFailureNullRif) {
  auto entry = GenerateP4MulticastReplicationEntry("0x1", "Ethernet1", "0x1");
  sai_object_id_t group_member_oid;

  EXPECT_EQ(StatusCode::SWSS_RC_UNAVAIL,
            CreateMulticastGroupMember(entry, SAI_OBJECT_TYPE_NULL,
                                       &group_member_oid));
}

TEST_F(L3MulticastManagerTest, AddMulticastReplicationEntriesNoRifTest) {
  std::vector<P4MulticastReplicationEntry> entries;
  entries.push_back(
      GenerateP4MulticastReplicationEntry("0x1", "Ethernet1", "0x1"));
  entries.push_back(
      GenerateP4MulticastReplicationEntry("0x2", "Ethernet2", "0x2"));

  std::vector<ReturnCode> statuses = AddMulticastReplicationEntries(entries);
  EXPECT_EQ(statuses.size(), 2);
  EXPECT_EQ(statuses[0], StatusCode::SWSS_RC_UNAVAIL);
  EXPECT_EQ(statuses[1], StatusCode::SWSS_RC_NOT_EXECUTED);
}

TEST_F(L3MulticastManagerTest, AddMulticastReplicationEntriesOneAddTest) {
  // Add router interface entry so have RIF.
  auto rif_entry = SetupP4MulticastRouterInterfaceEntry(
      "Ethernet1", "0x1", swss::MacAddress(kSrcMac1), kRifOid1);

  std::vector<P4MulticastReplicationEntry> entries;
  entries.push_back(
      GenerateP4MulticastReplicationEntry("0x1", "Ethernet1", "0x1"));

  EXPECT_CALL(mock_sai_ipmc_group_, create_ipmc_group(_, _, _, _))
      .WillOnce(
          DoAll(SetArgPointee<0>(kGroupOid1), Return(SAI_STATUS_SUCCESS)));
  EXPECT_CALL(mock_sai_ipmc_group_, create_ipmc_group_member(_, _, _, _))
      .WillOnce(DoAll(SetArgPointee<0>(kGroupMemberOid1),
                      Return(SAI_STATUS_SUCCESS)));

  auto statuses = AddMulticastReplicationEntries(entries);
  EXPECT_EQ(statuses.size(), 1);
  for (size_t i = 0; i < statuses.size(); ++i) {
    EXPECT_TRUE(statuses[i].ok());
  }
  EXPECT_EQ(entries[0].multicast_group_oid, kGroupOid1);
  EXPECT_EQ(entries[0].multicast_group_member_oid, kGroupMemberOid1);
  EXPECT_TRUE(p4_oid_mapper_.existsOID(SAI_OBJECT_TYPE_IPMC_GROUP, "0x1"));

  sai_object_id_t end_groupOid1 = SAI_NULL_OBJECT_ID;
  p4_oid_mapper_.getOID(SAI_OBJECT_TYPE_IPMC_GROUP, "0x1", &end_groupOid1);
  sai_object_id_t end_groupOid2 = SAI_NULL_OBJECT_ID;
  p4_oid_mapper_.getOID(SAI_OBJECT_TYPE_IPMC_GROUP, "0x2", &end_groupOid2);
  EXPECT_EQ(end_groupOid1, kGroupOid1);
  EXPECT_EQ(end_groupOid2, SAI_NULL_OBJECT_ID);
}

TEST_F(L3MulticastManagerTest,
       AddMulticastReplicationEntriesCreateGroupFailsTest) {
  // Add router interface entry so have RIF.
  auto rif_entry = SetupP4MulticastRouterInterfaceEntry(
      "Ethernet1", "0x1", swss::MacAddress(kSrcMac1), kRifOid1);

  std::vector<P4MulticastReplicationEntry> entries;
  entries.push_back(
      GenerateP4MulticastReplicationEntry("0x1", "Ethernet1", "0x1"));
  entries.push_back(
      GenerateP4MulticastReplicationEntry("0x2", "Ethernet2", "0x2"));

  EXPECT_CALL(mock_sai_ipmc_group_, create_ipmc_group(_, _, _, _))
      .WillOnce(Return(SAI_STATUS_FAILURE));

  auto statuses = AddMulticastReplicationEntries(entries);
  EXPECT_EQ(statuses.size(), 2);
  EXPECT_EQ(statuses[0], StatusCode::SWSS_RC_UNKNOWN);
  EXPECT_EQ(statuses[1], StatusCode::SWSS_RC_NOT_EXECUTED);
  EXPECT_FALSE(p4_oid_mapper_.existsOID(SAI_OBJECT_TYPE_IPMC_GROUP, "0x1"));
  EXPECT_FALSE(p4_oid_mapper_.existsOID(SAI_OBJECT_TYPE_IPMC_GROUP, "0x2"));
}

TEST_F(L3MulticastManagerTest,
       AddMulticastReplicationEntriesCreateGroupMemberFailsTest) {
  // Add router interface entry so have RIF.
  auto rif_entry = SetupP4MulticastRouterInterfaceEntry(
      "Ethernet1", "0x1", swss::MacAddress(kSrcMac1), kRifOid1);

  std::vector<P4MulticastReplicationEntry> entries;
  entries.push_back(
      GenerateP4MulticastReplicationEntry("0x1", "Ethernet1", "0x1"));
  entries.push_back(
      GenerateP4MulticastReplicationEntry("0x2", "Ethernet2", "0x2"));

  EXPECT_CALL(mock_sai_ipmc_group_, create_ipmc_group(_, _, _, _))
      .WillOnce(
          DoAll(SetArgPointee<0>(kGroupOid1), Return(SAI_STATUS_SUCCESS)));
  EXPECT_CALL(mock_sai_ipmc_group_, create_ipmc_group_member(_, _, _, _))
      .WillOnce(Return(SAI_STATUS_FAILURE));
  EXPECT_CALL(mock_sai_ipmc_group_, remove_ipmc_group(kGroupOid1))
      .WillOnce(Return(SAI_STATUS_SUCCESS));

  auto statuses = AddMulticastReplicationEntries(entries);
  EXPECT_EQ(statuses.size(), 2);
  EXPECT_EQ(statuses[0], StatusCode::SWSS_RC_UNKNOWN);
  EXPECT_EQ(statuses[1], StatusCode::SWSS_RC_NOT_EXECUTED);
}

TEST_F(L3MulticastManagerTest,
       AddMulticastReplicationEntriesCreateGroupMemberFailsBackoutFailsTest) {
  // Add router interface entry so have RIF.
  auto rif_entry = SetupP4MulticastRouterInterfaceEntry(
      "Ethernet1", "0x1", swss::MacAddress(kSrcMac1), kRifOid1);

  std::vector<P4MulticastReplicationEntry> entries;
  entries.push_back(
      GenerateP4MulticastReplicationEntry("0x1", "Ethernet1", "0x1"));
  entries.push_back(
      GenerateP4MulticastReplicationEntry("0x2", "Ethernet2", "0x2"));

  EXPECT_CALL(mock_sai_ipmc_group_, create_ipmc_group(_, _, _, _))
      .WillOnce(
          DoAll(SetArgPointee<0>(kGroupOid1), Return(SAI_STATUS_SUCCESS)));
  EXPECT_CALL(mock_sai_ipmc_group_, create_ipmc_group_member(_, _, _, _))
      .WillOnce(Return(SAI_STATUS_FAILURE));
  EXPECT_CALL(mock_sai_ipmc_group_, remove_ipmc_group(kGroupOid1))
      .WillOnce(Return(SAI_STATUS_FAILURE));

  auto statuses = AddMulticastReplicationEntries(entries);
  EXPECT_EQ(statuses.size(), 2);
  EXPECT_EQ(statuses[0], StatusCode::SWSS_RC_UNKNOWN);
  EXPECT_EQ(statuses[1], StatusCode::SWSS_RC_NOT_EXECUTED);
}

TEST_F(L3MulticastManagerTest, DeleteMulticastReplicationEntriesNoEntry) {
  std::vector<P4MulticastReplicationEntry> entries;
  entries.push_back(
      GenerateP4MulticastReplicationEntry("0x1", "Ethernet1", "0x1"));
  entries.push_back(
      GenerateP4MulticastReplicationEntry("0x2", "Ethernet2", "0x2"));

  // Can't delete what isn't there.
  std::vector<ReturnCode> statuses = DeleteMulticastReplicationEntries(entries);
  EXPECT_EQ(statuses.size(), 2);
  EXPECT_EQ(statuses[0], StatusCode::SWSS_RC_UNKNOWN);
  EXPECT_EQ(statuses[1], StatusCode::SWSS_RC_NOT_EXECUTED);
}

TEST_F(L3MulticastManagerTest, DeleteMulticastReplicationEntriesNoRif) {
  // Add router interface entry so have RIF.
  auto rif_entry = SetupP4MulticastRouterInterfaceEntry(
      "Ethernet1", "0x1", swss::MacAddress(kSrcMac1), kRifOid1);
  // Add entries to then be deleted.
  auto group_entry1 = SetupP4MulticastReplicationEntry(
      "0x1", "Ethernet1", "0x1", kGroupOid1, kGroupMemberOid1);
  auto group_entry2 = SetupP4MulticastReplicationEntry(
      "0x2", "Ethernet1", "0x1", kGroupOid2, kGroupMemberOid2);

  // Unnaturally force RIFs to disappear.
  std::string rif_key = KeyGenerator::generateMulticastRouterInterfaceRifKey(
      "Ethernet1", swss::MacAddress(kSrcMac1));
  ForceRemoveRifKey(rif_key);

  // Attempt to delete.
  std::vector<P4MulticastReplicationEntry> entries = {group_entry1,
                                                      group_entry2};
  auto statuses = DeleteMulticastReplicationEntries(entries);
  EXPECT_EQ(statuses.size(), 2);
  EXPECT_EQ(statuses[0], StatusCode::SWSS_RC_INTERNAL);
  EXPECT_EQ(statuses[1], StatusCode::SWSS_RC_NOT_EXECUTED);
}

TEST_F(L3MulticastManagerTest, DeleteMulticastReplicationEntriesNoGroupOid) {
  // Add router interface entry so have RIF.
  auto rif_entry = SetupP4MulticastRouterInterfaceEntry(
      "Ethernet1", "0x1", swss::MacAddress(kSrcMac1), kRifOid1);
  // Add entries to then be deleted.
  auto group_entry1 = SetupP4MulticastReplicationEntry(
      "0x1", "Ethernet1", "0x1", kGroupOid1, kGroupMemberOid1);
  auto group_entry2 = SetupP4MulticastReplicationEntry(
      "0x2", "Ethernet1", "0x1", kGroupOid2, kGroupMemberOid2);

  // Unnaturally force multicast group OIDs to disappear.
  p4_oid_mapper_.eraseOID(SAI_OBJECT_TYPE_IPMC_GROUP,
                          group_entry1.multicast_group_id);

  // Attempt to delete.
  std::vector<P4MulticastReplicationEntry> entries = {group_entry1,
                                                      group_entry2};
  auto statuses = DeleteMulticastReplicationEntries(entries);
  EXPECT_EQ(statuses.size(), 2);
  EXPECT_EQ(statuses[0], StatusCode::SWSS_RC_INTERNAL);
  EXPECT_EQ(statuses[1], StatusCode::SWSS_RC_NOT_EXECUTED);
}

TEST_F(L3MulticastManagerTest,
       DeleteMulticastReplicationEntriesNoGroupMembersFound) {
  // Add router interface entry so have RIF.
  auto rif_entry = SetupP4MulticastRouterInterfaceEntry(
      "Ethernet1", "0x1", swss::MacAddress(kSrcMac1), kRifOid1);
  // Add entries to then be deleted.
  auto group_entry1 = SetupP4MulticastReplicationEntry(
      "0x1", "Ethernet1", "0x1", kGroupOid1, kGroupMemberOid1);
  auto group_entry2 = SetupP4MulticastReplicationEntry(
      "0x2", "Ethernet1", "0x1", kGroupOid2, kGroupMemberOid2);

  // Unnaturally force multicast group members to disappear.
  ForceRemoveGroupMembers(group_entry1.multicast_group_id);

  // Attempt to delete.
  std::vector<P4MulticastReplicationEntry> entries = {group_entry1,
                                                      group_entry2};
  auto statuses = DeleteMulticastReplicationEntries(entries);
  EXPECT_EQ(statuses.size(), 2);
  EXPECT_EQ(statuses[0], StatusCode::SWSS_RC_INTERNAL);
  EXPECT_EQ(statuses[1], StatusCode::SWSS_RC_NOT_EXECUTED);
}

TEST_F(L3MulticastManagerTest,
       DeleteMulticastReplicationEntriesSpecificGroupMemberNotFound) {
  // Add router interface entry so have RIF.
  auto rif_entry = SetupP4MulticastRouterInterfaceEntry(
      "Ethernet1", "0x1", swss::MacAddress(kSrcMac1), kRifOid1);
  // Add entries to then be deleted.
  auto group_entry1 = SetupP4MulticastReplicationEntry(
      "0x1", "Ethernet1", "0x1", kGroupOid1, kGroupMemberOid1);
  auto group_entry2 = SetupP4MulticastReplicationEntry(
      "0x2", "Ethernet1", "0x1", kGroupOid2, kGroupMemberOid2);

  // Unnaturally force multicast group members to disappear.
  ForceRemoveSpecificGroupMember(group_entry1.multicast_group_id,
                                 group_entry1.multicast_replication_key);

  // Attempt to delete.
  std::vector<P4MulticastReplicationEntry> entries = {group_entry1,
                                                      group_entry2};
  auto statuses = DeleteMulticastReplicationEntries(entries);
  EXPECT_EQ(statuses.size(), 2);
  EXPECT_EQ(statuses[0], StatusCode::SWSS_RC_INTERNAL);
  EXPECT_EQ(statuses[1], StatusCode::SWSS_RC_NOT_EXECUTED);
}

TEST_F(L3MulticastManagerTest,
       DeleteMulticastReplicationEntriesDeleteMemberButNotGroupSuccess) {
  // Add router interface entries so have RIF.
  auto rif_entry1 = SetupP4MulticastRouterInterfaceEntry(
      "Ethernet1", "0x1", swss::MacAddress(kSrcMac1), kRifOid1);
  auto rif_entry2 = SetupP4MulticastRouterInterfaceEntry(
      "Ethernet1", "0x2", swss::MacAddress(kSrcMac2), kRifOid2);
  // Add entries to then be deleted.
  auto group_entry1 = SetupP4MulticastReplicationEntry(
      "0x1", "Ethernet1", "0x1", kGroupOid1, kGroupMemberOid1);
  auto group_entry2 = SetupP4MulticastReplicationEntry(
      "0x1", "Ethernet1", "0x2", kGroupOid1, kGroupMemberOid2,
      /*expect_group_mock=*/false);

  // Attempt to delete.
  EXPECT_CALL(mock_sai_ipmc_group_, remove_ipmc_group_member(kGroupMemberOid1))
      .WillOnce(Return(SAI_STATUS_SUCCESS));

  std::vector<P4MulticastReplicationEntry> entries = {group_entry1};
  auto statuses = DeleteMulticastReplicationEntries(entries);
  EXPECT_EQ(statuses.size(), 1);
  EXPECT_TRUE(statuses[0].ok());
  EXPECT_TRUE(p4_oid_mapper_.existsOID(SAI_OBJECT_TYPE_IPMC_GROUP, "0x1"));
}

TEST_F(L3MulticastManagerTest,
       DeleteMulticastReplicationEntriesWithActiveRouteEntriesFailure) {
  // Add router interface entries so have RIF.
  auto rif_entry1 = SetupP4MulticastRouterInterfaceEntry(
      "Ethernet1", "0x1", swss::MacAddress(kSrcMac1), kRifOid1);
  auto rif_entry2 = SetupP4MulticastRouterInterfaceEntry(
      "Ethernet2", "0x2", swss::MacAddress(kSrcMac2), kRifOid2);
  // Add entries to then be deleted.
  auto group_entry1 = SetupP4MulticastReplicationEntry(
      "0x1", "Ethernet1", "0x1", kGroupOid1, kGroupMemberOid1);
  auto group_entry2 = SetupP4MulticastReplicationEntry(
      "0x2", "Ethernet2", "0x2", kGroupOid2, kGroupMemberOid2);

  // Register that Route Entries are using this multicast group.
  p4_oid_mapper_.increaseRefCount(SAI_OBJECT_TYPE_IPMC_GROUP, "0x1");

  // Attempt to delete.  Expect failure, since multicast group is referenced.
  std::vector<P4MulticastReplicationEntry> entries = {group_entry1,
                                                      group_entry2};
  auto statuses = DeleteMulticastReplicationEntries(entries);
  EXPECT_EQ(statuses.size(), 2);
  EXPECT_EQ(statuses[0], StatusCode::SWSS_RC_IN_USE);
  EXPECT_EQ(statuses[1], StatusCode::SWSS_RC_NOT_EXECUTED);
  EXPECT_TRUE(p4_oid_mapper_.existsOID(SAI_OBJECT_TYPE_IPMC_GROUP, "0x1"));
}

TEST_F(L3MulticastManagerTest,
       DeleteMulticastReplicationEntriesDeleteMemberAndGroupSuccess) {
  // Add router interface entries so have RIF.
  auto rif_entry1 = SetupP4MulticastRouterInterfaceEntry(
      "Ethernet1", "0x1", swss::MacAddress(kSrcMac1), kRifOid1);
  auto rif_entry2 = SetupP4MulticastRouterInterfaceEntry(
      "Ethernet1", "0x2", swss::MacAddress(kSrcMac2), kRifOid2);
  // Add entries to then be deleted.
  auto group_entry1 = SetupP4MulticastReplicationEntry(
      "0x1", "Ethernet1", "0x1", kGroupOid1, kGroupMemberOid1);
  auto group_entry2 = SetupP4MulticastReplicationEntry(
      "0x2", "Ethernet1", "0x2", kGroupOid2, kGroupMemberOid2);

  // Attempt to delete.
  EXPECT_CALL(mock_sai_ipmc_group_, remove_ipmc_group_member(kGroupMemberOid1))
      .WillOnce(Return(SAI_STATUS_SUCCESS));
  EXPECT_CALL(mock_sai_ipmc_group_, remove_ipmc_group(kGroupOid1))
      .WillOnce(Return(SAI_STATUS_SUCCESS));

  std::vector<P4MulticastReplicationEntry> entries = {group_entry1};
  auto statuses = DeleteMulticastReplicationEntries(entries);
  EXPECT_EQ(statuses.size(), 1);
  EXPECT_TRUE(statuses[0].ok());
}

TEST_F(L3MulticastManagerTest,
       DeleteMulticastReplicationEntriesDeleteMemberFails) {
  // Add router interface entries so have RIF.
  auto rif_entry1 = SetupP4MulticastRouterInterfaceEntry(
      "Ethernet1", "0x1", swss::MacAddress(kSrcMac1), kRifOid1);
  auto rif_entry2 = SetupP4MulticastRouterInterfaceEntry(
      "Ethernet1", "0x2", swss::MacAddress(kSrcMac2), kRifOid2);
  // Add entries to then be deleted.
  auto group_entry1 = SetupP4MulticastReplicationEntry(
      "0x1", "Ethernet1", "0x1", kGroupOid1, kGroupMemberOid1);
  auto group_entry2 = SetupP4MulticastReplicationEntry(
      "0x1", "Ethernet1", "0x2", kGroupOid1, kGroupMemberOid2,
      /*expect_group_mock=*/false);

  // Attempt to delete.
  EXPECT_CALL(mock_sai_ipmc_group_, remove_ipmc_group_member(kGroupMemberOid1))
      .WillOnce(Return(SAI_STATUS_FAILURE));

  std::vector<P4MulticastReplicationEntry> entries = {group_entry1,
                                                      group_entry2};
  auto statuses = DeleteMulticastReplicationEntries(entries);
  EXPECT_EQ(statuses.size(), 2);
  EXPECT_EQ(statuses[0], StatusCode::SWSS_RC_UNKNOWN);
  EXPECT_EQ(statuses[1], StatusCode::SWSS_RC_NOT_EXECUTED);
}

// MIKE
TEST_F(L3MulticastManagerTest,
       DeleteMulticastReplicationEntriesDeleteGroupFailsReAddMemberSucceeds) {
  // Add router interface entries so have RIF.
  auto rif_entry1 = SetupP4MulticastRouterInterfaceEntry(
      "Ethernet1", "0x1", swss::MacAddress(kSrcMac1), kRifOid1);
  auto rif_entry2 = SetupP4MulticastRouterInterfaceEntry(
      "Ethernet1", "0x2", swss::MacAddress(kSrcMac2), kRifOid2);
  // Add entries to then be deleted.
  auto group_entry1 = SetupP4MulticastReplicationEntry(
      "0x1", "Ethernet1", "0x1", kGroupOid1, kGroupMemberOid1);
  auto group_entry2 = SetupP4MulticastReplicationEntry(
      "0x2", "Ethernet1", "0x2", kGroupOid1, kGroupMemberOid2);

  // Attempt to delete.
  EXPECT_CALL(mock_sai_ipmc_group_, remove_ipmc_group_member(kGroupMemberOid1))
      .WillOnce(Return(SAI_STATUS_SUCCESS));
  EXPECT_CALL(mock_sai_ipmc_group_, remove_ipmc_group(kGroupOid1))
      .WillOnce(Return(SAI_STATUS_FAILURE));
  // We will change the group member OID to confirm it is updated properly.
  EXPECT_CALL(mock_sai_ipmc_group_, create_ipmc_group_member(_, _, _, _))
      .WillOnce(DoAll(SetArgPointee<0>(kGroupMemberOid3),
                      Return(SAI_STATUS_SUCCESS)));

  std::vector<P4MulticastReplicationEntry> entries = {group_entry1,
                                                      group_entry2};
  auto statuses = DeleteMulticastReplicationEntries(entries);
  EXPECT_EQ(statuses.size(), 2);
  EXPECT_EQ(statuses[0], StatusCode::SWSS_RC_UNKNOWN);
  EXPECT_EQ(statuses[1], StatusCode::SWSS_RC_NOT_EXECUTED);

  sai_object_id_t end_groupMemberOid = SAI_NULL_OBJECT_ID;
  p4_oid_mapper_.getOID(SAI_OBJECT_TYPE_IPMC_GROUP_MEMBER,
                        group_entry1.multicast_replication_key,
                        &end_groupMemberOid);
  EXPECT_EQ(end_groupMemberOid, kGroupMemberOid3);
}

TEST_F(L3MulticastManagerTest,
       DeleteMulticastReplicationEntriesDeleteGroupFailsReAddMemberFails) {
  // Add router interface entries so have RIF.
  auto rif_entry1 = SetupP4MulticastRouterInterfaceEntry(
      "Ethernet1", "0x1", swss::MacAddress(kSrcMac1), kRifOid1);
  auto rif_entry2 = SetupP4MulticastRouterInterfaceEntry(
      "Ethernet1", "0x2", swss::MacAddress(kSrcMac2), kRifOid2);
  // Add entries to then be deleted.
  auto group_entry1 = SetupP4MulticastReplicationEntry(
      "0x1", "Ethernet1", "0x1", kGroupOid1, kGroupMemberOid1);
  auto group_entry2 = SetupP4MulticastReplicationEntry(
      "0x2", "Ethernet1", "0x2", kGroupOid1, kGroupMemberOid2);

  // Attempt to delete.
  EXPECT_CALL(mock_sai_ipmc_group_, remove_ipmc_group_member(kGroupMemberOid1))
      .WillOnce(Return(SAI_STATUS_SUCCESS));
  EXPECT_CALL(mock_sai_ipmc_group_, remove_ipmc_group(kGroupOid1))
      .WillOnce(Return(SAI_STATUS_FAILURE));
  EXPECT_CALL(mock_sai_ipmc_group_, create_ipmc_group_member(_, _, _, _))
      .WillOnce(Return(SAI_STATUS_FAILURE));

  std::vector<P4MulticastReplicationEntry> entries = {group_entry1,
                                                      group_entry2};
  auto statuses = DeleteMulticastReplicationEntries(entries);
  EXPECT_EQ(statuses.size(), 2);
  EXPECT_EQ(statuses[0], StatusCode::SWSS_RC_UNKNOWN);
  EXPECT_EQ(statuses[1], StatusCode::SWSS_RC_NOT_EXECUTED);
}

TEST_F(L3MulticastManagerTest, UpdateMulticastReplicationEntriesTestSuccess) {
  auto entry1 = GenerateP4MulticastReplicationEntry("0x1", "Ethernet1", "0x1");
  auto entry2 = GenerateP4MulticastReplicationEntry("0x2", "Ethernet1", "0x1");
  std::vector<P4MulticastReplicationEntry> entries = {entry1, entry2};
  auto statuses = UpdateMulticastReplicationEntries(entries);
  EXPECT_EQ(statuses.size(), 2);
  EXPECT_EQ(statuses[0], StatusCode::SWSS_RC_SUCCESS);
  EXPECT_EQ(statuses[1], StatusCode::SWSS_RC_SUCCESS);
}

TEST_F(L3MulticastManagerTest,
       ValidateSetMulticastReplicationEntryEmptyMulticastGroupTest) {
  // Add router interface entries so have RIF.
  auto rif_entry = SetupP4MulticastRouterInterfaceEntry(
      "Ethernet1", "0x0", swss::MacAddress(kSrcMac1), kRifOid1);

  auto entry = GenerateP4MulticastReplicationEntry("", "Ethernet1", "0x0");
  ReturnCode status = ValidateMulticastReplicationEntry(entry, SET_COMMAND);
  EXPECT_EQ(StatusCode::SWSS_RC_INVALID_PARAM, status);
}

TEST_F(L3MulticastManagerTest,
       ValidateSetMulticastReplicationEntryEmptyPortTest) {
  // Add router interface entries so have RIF.
  auto rif_entry = SetupP4MulticastRouterInterfaceEntry(
      "Ethernet1", "0x0", swss::MacAddress(kSrcMac1), kRifOid1);

  auto entry = GenerateP4MulticastReplicationEntry("0x1", "", "0x0");
  ReturnCode status = ValidateMulticastReplicationEntry(entry, SET_COMMAND);
  EXPECT_EQ(StatusCode::SWSS_RC_INVALID_PARAM, status);
}

TEST_F(L3MulticastManagerTest,
       ValidateSetMulticastReplicationEntryEmptyEgressInstanceTest) {
  // Add router interface entries so have RIF.
  auto rif_entry = SetupP4MulticastRouterInterfaceEntry(
      "Ethernet1", "0x0", swss::MacAddress(kSrcMac1), kRifOid1);

  auto entry = GenerateP4MulticastReplicationEntry("0x1", "Ethernet1", "");
  ReturnCode status = ValidateMulticastReplicationEntry(entry, SET_COMMAND);
  EXPECT_EQ(StatusCode::SWSS_RC_INVALID_PARAM, status);
}

TEST_F(L3MulticastManagerTest,
       ValidateSetMulticastReplicationEntryAddSuccessTest) {
  // Add router interface entries so have RIF.
  auto rif_entry = SetupP4MulticastRouterInterfaceEntry(
      "Ethernet1", "0x0", swss::MacAddress(kSrcMac1), kRifOid1);

  auto entry = GenerateP4MulticastReplicationEntry("0x1", "Ethernet1", "0x0");
  ReturnCode status = ValidateMulticastReplicationEntry(entry, SET_COMMAND);
  EXPECT_TRUE(status.ok());
}

TEST_F(L3MulticastManagerTest, ValidateSetMulticastReplicationEntryNoRifTest) {
  // No RIF
  auto entry = GenerateP4MulticastReplicationEntry("0x1", "Ethernet1", "0x0");
  ReturnCode status = ValidateMulticastReplicationEntry(entry, SET_COMMAND);
  EXPECT_EQ(StatusCode::SWSS_RC_NOT_FOUND, status);
}

TEST_F(L3MulticastManagerTest, ValidateSetMulticastReplicationEntryUpdateTest) {
  // Add router interface entries so have RIF.
  auto rif_entry1 = SetupP4MulticastRouterInterfaceEntry(
      "Ethernet1", "0x0", swss::MacAddress(kSrcMac1), kRifOid1);
  auto rif_entry2 = SetupP4MulticastRouterInterfaceEntry(
      "Ethernet2", "0x0", swss::MacAddress(kSrcMac2), kRifOid2);
  // Setup multicast group entries.
  auto group_entry1 = SetupP4MulticastReplicationEntry(
      "0x1", "Ethernet1", "0x0", kGroupOid1, kGroupMemberOid1);
  auto group_entry2 = SetupP4MulticastReplicationEntry(
      "0x1", "Ethernet2", "0x0", kGroupOid1, kGroupMemberOid2,
      /*expect_group_mock=*/false);

  // Validate an existing multicast entry.
  ReturnCode status =
      ValidateMulticastReplicationEntry(group_entry1, SET_COMMAND);
  EXPECT_TRUE(status.ok());
}

TEST_F(L3MulticastManagerTest,
       ValidateSetMulticastReplicationEntryUpdateNoGroupOidInEntryTest) {
  // Add router interface entries so have RIF.
  auto rif_entry1 = SetupP4MulticastRouterInterfaceEntry(
      "Ethernet1", "0x0", swss::MacAddress(kSrcMac1), kRifOid1);
  auto rif_entry2 = SetupP4MulticastRouterInterfaceEntry(
      "Ethernet2", "0x0", swss::MacAddress(kSrcMac2), kRifOid2);
  // Setup multicast group entries.
  auto group_entry1 = SetupP4MulticastReplicationEntry(
      "0x1", "Ethernet1", "0x0", kGroupOid1, kGroupMemberOid1);
  auto group_entry2 = SetupP4MulticastReplicationEntry(
      "0x1", "Ethernet2", "0x0", kGroupOid1, kGroupMemberOid2,
      /*expect_group_mock=*/false);

  // Force clear the group OID from the entry to cause an error.
  auto* actual_entry =
      GetMulticastReplicationEntry(group_entry1.multicast_replication_key);
  ASSERT_NE(actual_entry, nullptr);
  actual_entry->multicast_group_oid = SAI_OBJECT_TYPE_NULL;

  // Validate an existing multicast entry.
  ReturnCode status =
      ValidateMulticastReplicationEntry(group_entry1, SET_COMMAND);
  EXPECT_EQ(StatusCode::SWSS_RC_NOT_FOUND, status);
  // Delete also fails.
  status = ValidateMulticastReplicationEntry(group_entry1, DEL_COMMAND);
  EXPECT_EQ(StatusCode::SWSS_RC_NOT_FOUND, status);
}

TEST_F(L3MulticastManagerTest,
       ValidateSetMulticastReplicationEntryUpdateNoGroupMemberOidInEntryTest) {
  // Add router interface entries so have RIF.
  auto rif_entry1 = SetupP4MulticastRouterInterfaceEntry(
      "Ethernet1", "0x0", swss::MacAddress(kSrcMac1), kRifOid1);
  auto rif_entry2 = SetupP4MulticastRouterInterfaceEntry(
      "Ethernet2", "0x0", swss::MacAddress(kSrcMac2), kRifOid2);
  // Setup multicast group entries.
  auto group_entry1 = SetupP4MulticastReplicationEntry(
      "0x1", "Ethernet1", "0x0", kGroupOid1, kGroupMemberOid1);
  auto group_entry2 = SetupP4MulticastReplicationEntry(
      "0x1", "Ethernet2", "0x0", kGroupOid1, kGroupMemberOid2,
      /*expect_group_mock=*/false);

  // Force clear the group member OID from the entry to cause an error.
  auto* actual_entry =
      GetMulticastReplicationEntry(group_entry1.multicast_replication_key);
  ASSERT_NE(actual_entry, nullptr);
  actual_entry->multicast_group_member_oid = SAI_OBJECT_TYPE_NULL;

  // Validate an existing multicast entry.
  ReturnCode status =
      ValidateMulticastReplicationEntry(group_entry1, SET_COMMAND);
  EXPECT_EQ(StatusCode::SWSS_RC_NOT_FOUND, status);
  // Delete also fails.
  status = ValidateMulticastReplicationEntry(group_entry1, DEL_COMMAND);
  EXPECT_EQ(StatusCode::SWSS_RC_NOT_FOUND, status);
}

TEST_F(L3MulticastManagerTest,
       ValidateSetMulticastReplicationEntryUpdateNoGroupOidTest) {
  // Add router interface entries so have RIF.
  auto rif_entry1 = SetupP4MulticastRouterInterfaceEntry(
      "Ethernet1", "0x0", swss::MacAddress(kSrcMac1), kRifOid1);
  auto rif_entry2 = SetupP4MulticastRouterInterfaceEntry(
      "Ethernet2", "0x0", swss::MacAddress(kSrcMac2), kRifOid2);
  // Setup multicast group entries.
  auto group_entry1 = SetupP4MulticastReplicationEntry(
      "0x1", "Ethernet1", "0x0", kGroupOid1, kGroupMemberOid1);
  auto group_entry2 = SetupP4MulticastReplicationEntry(
      "0x1", "Ethernet2", "0x0", kGroupOid1, kGroupMemberOid2,
      /*expect_group_mock=*/false);

  // Force remove the multicast group OID to cause an error.
  p4_oid_mapper_.eraseOID(SAI_OBJECT_TYPE_IPMC_GROUP,
                          group_entry1.multicast_group_id);

  // Validate an existing multicast entry.
  ReturnCode status =
      ValidateMulticastReplicationEntry(group_entry1, SET_COMMAND);
  EXPECT_EQ(StatusCode::SWSS_RC_NOT_FOUND, status);
}

TEST_F(L3MulticastManagerTest,
       ValidateSetMulticastReplicationEntryUpdateNoGroupMemberOidTest) {
  // Add router interface entries so have RIF.
  auto rif_entry1 = SetupP4MulticastRouterInterfaceEntry(
      "Ethernet1", "0x0", swss::MacAddress(kSrcMac1), kRifOid1);
  auto rif_entry2 = SetupP4MulticastRouterInterfaceEntry(
      "Ethernet2", "0x0", swss::MacAddress(kSrcMac2), kRifOid2);
  // Setup multicast group entries.
  auto group_entry1 = SetupP4MulticastReplicationEntry(
      "0x1", "Ethernet1", "0x0", kGroupOid1, kGroupMemberOid1);
  auto group_entry2 = SetupP4MulticastReplicationEntry(
      "0x1", "Ethernet2", "0x0", kGroupOid1, kGroupMemberOid2,
      /*expect_group_mock=*/false);

  // Force remove the multicast group member OID to cause an error.
  ForceRemoveGroupMembers(group_entry1.multicast_group_id);

  // Validate an existing multicast entry.
  ReturnCode status =
      ValidateMulticastReplicationEntry(group_entry1, SET_COMMAND);
  EXPECT_EQ(StatusCode::SWSS_RC_NOT_FOUND, status);
}

TEST_F(L3MulticastManagerTest,
       ValidateSetMulticastReplicationEntryUpdateNoGroupInMapperTest) {
  // Add router interface entries so have RIF.
  auto rif_entry1 = SetupP4MulticastRouterInterfaceEntry(
      "Ethernet1", "0x0", swss::MacAddress(kSrcMac1), kRifOid1);
  auto rif_entry2 = SetupP4MulticastRouterInterfaceEntry(
      "Ethernet2", "0x0", swss::MacAddress(kSrcMac2), kRifOid2);
  // Setup multicast group entries.
  auto group_entry1 = SetupP4MulticastReplicationEntry(
      "0x1", "Ethernet1", "0x0", kGroupOid1, kGroupMemberOid1);
  auto group_entry2 = SetupP4MulticastReplicationEntry(
      "0x1", "Ethernet2", "0x0", kGroupOid1, kGroupMemberOid2,
      /*expect_group_mock=*/false);

  // Force remove the multicast group OID from mapper to cause an error.
  p4_oid_mapper_.eraseOID(SAI_OBJECT_TYPE_IPMC_GROUP,
                          group_entry1.multicast_group_id);

  // Validate an existing multicast entry.
  ReturnCode status =
      ValidateMulticastReplicationEntry(group_entry1, SET_COMMAND);
  EXPECT_EQ(StatusCode::SWSS_RC_NOT_FOUND, status);
  // Delete also fails.
  status = ValidateMulticastReplicationEntry(group_entry1, DEL_COMMAND);
  EXPECT_EQ(StatusCode::SWSS_RC_NOT_FOUND, status);
}

TEST_F(L3MulticastManagerTest,
       ValidateSetMulticastReplicationEntryUpdateNoGroupMemberInMapperTest) {
  // Add router interface entries so have RIF.
  auto rif_entry1 = SetupP4MulticastRouterInterfaceEntry(
      "Ethernet1", "0x0", swss::MacAddress(kSrcMac1), kRifOid1);
  auto rif_entry2 = SetupP4MulticastRouterInterfaceEntry(
      "Ethernet2", "0x0", swss::MacAddress(kSrcMac2), kRifOid2);
  // Setup multicast group entries.
  auto group_entry1 = SetupP4MulticastReplicationEntry(
      "0x1", "Ethernet1", "0x0", kGroupOid1, kGroupMemberOid1);
  auto group_entry2 = SetupP4MulticastReplicationEntry(
      "0x1", "Ethernet2", "0x0", kGroupOid1, kGroupMemberOid2,
      /*expect_group_mock=*/false);

  // Force remove the multicast group member OID from mapper to cause an error.
  p4_oid_mapper_.eraseOID(SAI_OBJECT_TYPE_IPMC_GROUP_MEMBER,
                          group_entry1.multicast_replication_key);

  // Validate an existing multicast entry.
  ReturnCode status =
      ValidateMulticastReplicationEntry(group_entry1, SET_COMMAND);
  EXPECT_EQ(StatusCode::SWSS_RC_NOT_FOUND, status);
  // Delete also fails.
  status = ValidateMulticastReplicationEntry(group_entry1, DEL_COMMAND);
  EXPECT_EQ(StatusCode::SWSS_RC_NOT_FOUND, status);
}

TEST_F(L3MulticastManagerTest,
       ValidateDelMulticastReplicationEntryDeleteUnknownTest) {
  // Add router interface entries so have RIF.
  auto rif_entry = SetupP4MulticastRouterInterfaceEntry(
      "Ethernet1", "0x0", swss::MacAddress(kSrcMac1), kRifOid1);

  auto entry = GenerateP4MulticastReplicationEntry("0x1", "Ethernet1", "0x0");
  ReturnCode status = ValidateMulticastReplicationEntry(entry, DEL_COMMAND);
  EXPECT_EQ(StatusCode::SWSS_RC_NOT_FOUND, status);
}

TEST_F(L3MulticastManagerTest,
       ValidateSetMulticastReplicationEntryUnknownCommandTest) {
  // Add router interface entries so have RIF.
  auto rif_entry = SetupP4MulticastRouterInterfaceEntry(
      "Ethernet1", "0x0", swss::MacAddress(kSrcMac1), kRifOid1);

  auto entry = GenerateP4MulticastReplicationEntry("0x1", "Ethernet1", "0x0");
  ReturnCode status = ValidateMulticastReplicationEntry(entry, "do_things");
  EXPECT_EQ(StatusCode::SWSS_RC_INVALID_PARAM, status);
}

// ---------- Temporary tests (unimplemented functions) -----------------------

TEST_F(L3MulticastManagerTest, NoVerifyStateMulticastReplication) {
  const std::string match_key =
      R"({"match/multicast_group_id":"0x1",)"
      R"("match/multicast_replica_port":"Ethernet1",)"
      R"("match/multicast_replica_instance":"0x1"})";
  const std::string appl_db_key =
      std::string(APP_P4RT_REPLICATION_L2_MULTICAST_TABLE_NAME) +
      kTableKeyDelimiter + match_key;
  const std::string db_key = std::string(APP_P4RT_TABLE_NAME) +
                           kTableKeyDelimiter + appl_db_key;

  std::vector<swss::FieldValueTuple> attributes;
  attributes.push_back(
      swss::FieldValueTuple{p4orch::kControllerMetadata, "so_meta"});

  EXPECT_FALSE(VerifyState(db_key, attributes).empty());
}

TEST_F(L3MulticastManagerTest, NoVerifyMulticastReplicationStateCache) {
  auto entry1 = GenerateP4MulticastReplicationEntry("0x1", "Ethernet1", "0x1");
  auto entry2 = GenerateP4MulticastReplicationEntry("0x2", "Ethernet1", "0x1");
  EXPECT_FALSE(VerifyMulticastReplicationStateCache(entry1, &entry2).empty());
}

TEST_F(L3MulticastManagerTest, NoVerifyMulticastReplicationStateAsicDb) {
  auto entry = GenerateP4MulticastReplicationEntry("0x1", "Ethernet1", "0x1");
  EXPECT_FALSE(VerifyMulticastReplicationStateAsicDb(&entry).empty());
}

TEST_F(L3MulticastManagerTest, DrainUnimplmentedPacketReplication) {
  const std::string match_key =
      R"({"match/multicast_replica_port":"Ethernet1",)"
      R"("match/multicast_replica_instance":"0x1"})";
  const std::string appl_db_key =
      std::string(APP_P4RT_MULTICAST_ROUTER_INTERFACE_TABLE_NAME) +
      kTableKeyDelimiter + match_key;

  // Enqueue entry for create operation.
  auto expect_entry = GenerateP4MulticastRouterInterfaceEntry(
      "Ethernet1", "0x1", swss::MacAddress(kSrcMac1));
  expect_entry.router_interface_oid = kRifOid1;
  auto start_rifOid = GetRifOid(&expect_entry);
  EXPECT_EQ(start_rifOid, SAI_NULL_OBJECT_ID);

  std::vector<swss::FieldValueTuple> attributes;
  attributes.push_back(
      swss::FieldValueTuple{p4orch::kAction, p4orch::kSetSrcMac});
  attributes.push_back(
      swss::FieldValueTuple{prependParamField(p4orch::kSrcMac), kSrcMac1});
  attributes.push_back(
      swss::FieldValueTuple{p4orch::kControllerMetadata, "so_meta"});

  const std::string group_match_key =
      R"({"match/multicast_group_id":"0x1",)"
      R"("match/multicast_replica_port":"Ethernet1",)"
      R"("match/multicast_replica_instance":"0x1"})";
  const std::string group_appl_db_key =
      std::string(APP_P4RT_REPLICATION_L2_MULTICAST_TABLE_NAME) +
      kTableKeyDelimiter + group_match_key;
  std::vector<swss::FieldValueTuple> group_attributes;
  group_attributes.push_back(
      swss::FieldValueTuple{p4orch::kControllerMetadata, "so_meta"});

  Enqueue(APP_P4RT_MULTICAST_ROUTER_INTERFACE_TABLE_NAME,
          swss::KeyOpFieldsValuesTuple(appl_db_key, SET_COMMAND, attributes));
  Enqueue(APP_P4RT_MULTICAST_ROUTER_INTERFACE_TABLE_NAME,
          swss::KeyOpFieldsValuesTuple(group_appl_db_key, SET_COMMAND,
                                       group_attributes));

  EXPECT_CALL(mock_sai_router_intf_, create_router_interface(_, _, _, _))
      .WillOnce(DoAll(SetArgPointee<0>(kRifOid1), Return(SAI_STATUS_SUCCESS)));
  EXPECT_CALL(publisher_,
              publish(Eq(APP_P4RT_TABLE_NAME), Eq(appl_db_key), Eq(attributes),
                      Eq(StatusCode::SWSS_RC_SUCCESS), Eq(true)));
  EXPECT_EQ(StatusCode::SWSS_RC_UNIMPLEMENTED, Drain(/*failure_before=*/false));
  auto* actual_entry = GetMulticastRouterInterfaceEntry(
      expect_entry.multicast_router_interface_entry_key);
  ASSERT_NE(nullptr, actual_entry);
  VerifyP4MulticastRouterInterfaceEntryEqual(expect_entry, *actual_entry);
  auto end_rifOid = GetRifOid(actual_entry);
  EXPECT_EQ(end_rifOid, kRifOid1);
}

}  // namespace p4orch
