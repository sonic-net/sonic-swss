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
#include "mock_sai_bridge.h"
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
extern sai_bridge_api_t* sai_bridge_api;

extern char* gVrfName;
extern PortsOrch* gPortsOrch;
extern VRFOrch* gVrfOrch;

namespace p4orch {

namespace {
// Helpful place for constant and/or test functions
constexpr char* kSrcMac0 = "00:00:00:00:00:00";
constexpr char* kSrcMac1 = "00:01:02:03:04:05";
constexpr char* kSrcMac2 = "00:0a:0b:0c:0d:0e";
constexpr char* kSrcMac3 = "10:20:30:40:50:60";
constexpr char* kSrcMac4 = "15:25:35:45:55:65";
constexpr char* kSrcMac5 = "10:20:30:40:50:60";

constexpr sai_object_id_t kRifOid1 = 0x123456;
constexpr sai_object_id_t kRifOid2 = 0x22789a;
constexpr sai_object_id_t kRifOid3 = 0x33feed;
constexpr sai_object_id_t kRifOid4 = 0x44cafe;
constexpr sai_object_id_t kRifOid5 = 0x55abcd;

constexpr sai_object_id_t kGroupOid1 = 0x1;
constexpr sai_object_id_t kGroupOid2 = 0x2;
constexpr sai_object_id_t kGroupOid3 = 0x3;

constexpr sai_object_id_t kGroupMemberOid1 = 0x11;
constexpr sai_object_id_t kGroupMemberOid2 = 0x12;
constexpr sai_object_id_t kGroupMemberOid3 = 0x13;
constexpr sai_object_id_t kGroupMemberOid4 = 0x14;

constexpr sai_object_id_t kBridgePortOid1 = 0x101;
constexpr sai_object_id_t kBridgePortOid2 = 0x102;

// Matches two SAI attributes.
bool MatchSaiAttribute(const sai_attribute_t& attr,
                       const sai_attribute_t& exp_attr) {
  if (exp_attr.id == SAI_IPMC_GROUP_MEMBER_ATTR_IPMC_GROUP_ID) {
    if (attr.id != SAI_IPMC_GROUP_MEMBER_ATTR_IPMC_GROUP_ID ||
        attr.value.oid != exp_attr.value.oid) {
      return false;
    }
  }
  if (exp_attr.id == SAI_IPMC_GROUP_MEMBER_ATTR_IPMC_OUTPUT_ID) {
    if (attr.id != SAI_IPMC_GROUP_MEMBER_ATTR_IPMC_OUTPUT_ID ||
        attr.value.oid != exp_attr.value.oid) {
      return false;
    }
  }
  return true;
}

MATCHER_P(AttrArrayEq, array, "") {
  for (size_t i = 0; i < array.size(); ++i) {
    if (!MatchSaiAttribute(arg[i], array[i])) {
      return false;
    }
  }
  return true;
}

}  // namespace

class L3MulticastManagerTest : public ::testing::Test {
 protected:
  L3MulticastManagerTest()
      : l3_multicast_manager_(&p4_oid_mapper_, gVrfOrch, &publisher_) {}

  P4MulticastRouterInterfaceEntry GenerateP4MulticastRouterInterfaceEntry(
      const std::string& multicast_replica_port,
      const std::string& multicast_replica_instance,
      const swss::MacAddress src_mac,
      const std::string& multicast_metadata = "",
      const std::string& action = p4orch::kSetMulticastSrcMac) {
    P4MulticastRouterInterfaceEntry router_interface_entry = {};
    router_interface_entry.multicast_replica_port = multicast_replica_port;
    router_interface_entry.multicast_replica_instance =
        multicast_replica_instance;
    router_interface_entry.action = action;
    router_interface_entry.src_mac = src_mac;
    router_interface_entry.has_src_mac = action != p4orch::kNoAction;
    router_interface_entry.multicast_metadata = multicast_metadata;
    router_interface_entry.multicast_router_interface_entry_key =
        KeyGenerator::generateMulticastRouterInterfaceKey(
            router_interface_entry.multicast_replica_port,
            router_interface_entry.multicast_replica_instance);
    return router_interface_entry;
  }

  P4MulticastGroupEntry GenerateP4MulticastGroupEntry(
      const std::string& multicast_group_id,
      const std::vector<P4Replica>& replicas,
      const std::string& multicast_metadata = "",
      const std::string& controller_metadata = "") {
    P4MulticastGroupEntry group_entry = {};
    group_entry.multicast_group_id = multicast_group_id;
    for (auto& r : replicas) {
      group_entry.replicas.push_back(r);
      group_entry.member_oids[r.key] = SAI_NULL_OBJECT_ID;
    }
    group_entry.multicast_metadata = multicast_metadata;
    group_entry.controller_metadata = controller_metadata;
    return group_entry;
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

  P4MulticastRouterInterfaceEntry SetupP4MulticastRouterInterfaceNoActionEntry(
      const std::string& port, const std::string& instance,
      const sai_object_id_t bridge_port_oid, bool expect_mock = true) {
    std::vector<P4MulticastRouterInterfaceEntry> entries;
    auto entry = GenerateP4MulticastRouterInterfaceEntry(
        port, instance, swss::MacAddress(kSrcMac0), /*multicast_metadata=*/"",
        p4orch::kNoAction);
    entries.push_back(entry);

    if (expect_mock) {
      EXPECT_CALL(mock_sai_bridge_, create_bridge_port(_, _, Eq(2), _))
          .WillOnce(DoAll(SetArgPointee<0>(bridge_port_oid),
                          Return(SAI_STATUS_SUCCESS)));
    }

    std::vector<ReturnCode> statuses =
        AddMulticastRouterInterfaceEntries(entries);

    EXPECT_EQ(statuses.size(), 1);
    EXPECT_TRUE(statuses[0].ok());

    EXPECT_NE(GetMulticastRouterInterfaceEntry(
                  entries[0].multicast_router_interface_entry_key),
              nullptr);
    EXPECT_EQ(GetBridgePortOid(&entries[0]), bridge_port_oid);
    return entry;
  }

  P4MulticastGroupEntry SetupP4MulticastGroupEntry(
      const std::string& multicast_group_id,
      const std::vector<P4Replica>& replicas, const sai_object_id_t group_oid,
      const std::vector<sai_object_id_t>& group_member_oids,
      bool expect_group_mock = true) {
    auto entry = GenerateP4MulticastGroupEntry(multicast_group_id, replicas);
    std::vector<P4MulticastGroupEntry> entries = {entry};

    if (expect_group_mock) {
      EXPECT_CALL(mock_sai_ipmc_group_, create_ipmc_group(_, _, _, _))
          .WillOnce(
              DoAll(SetArgPointee<0>(group_oid), Return(SAI_STATUS_SUCCESS)));
    }

    if (group_member_oids.size() == 1) {
      EXPECT_CALL(mock_sai_ipmc_group_, create_ipmc_group_member(_, _, _, _))
          .WillOnce(DoAll(SetArgPointee<0>(group_member_oids.at(0)),
                          Return(SAI_STATUS_SUCCESS)));
    } else if (group_member_oids.size() == 2) {
      EXPECT_CALL(mock_sai_ipmc_group_, create_ipmc_group_member(_, _, _, _))
          .WillOnce(DoAll(SetArgPointee<0>(group_member_oids.at(0)),
                          Return(SAI_STATUS_SUCCESS)))
          .WillOnce(DoAll(SetArgPointee<0>(group_member_oids.at(1)),
                          Return(SAI_STATUS_SUCCESS)));
    }

    auto statuses = AddMulticastGroupEntries(entries);
    EXPECT_EQ(statuses.size(), 1);
    EXPECT_TRUE(statuses[0].ok());

    sai_object_id_t end_groupOid = SAI_NULL_OBJECT_ID;
    p4_oid_mapper_.getOID(SAI_OBJECT_TYPE_IPMC_GROUP,
                          entries[0].multicast_group_id, &end_groupOid);
    EXPECT_EQ(end_groupOid, group_oid);

    EXPECT_EQ(replicas.size(), group_member_oids.size());
    for (size_t i = 0; i < replicas.size(); ++i) {
      auto& replica = replicas.at(i);
      auto group_member_oid = group_member_oids.at(i);
      sai_object_id_t end_groupMemberOid = SAI_NULL_OBJECT_ID;
      p4_oid_mapper_.getOID(SAI_OBJECT_TYPE_IPMC_GROUP_MEMBER, replica.key,
                            &end_groupMemberOid);
      EXPECT_EQ(end_groupMemberOid, group_member_oid);
    }

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

  void VerifyP4MulticastGroupEntryEqual(const P4MulticastGroupEntry& x,
                                        const P4MulticastGroupEntry& y) {
    EXPECT_EQ(x.multicast_group_id, y.multicast_group_id);

    EXPECT_EQ(x.replicas.size(), y.replicas.size());
    if (x.replicas.size() == y.replicas.size()) {
      for (size_t i = 0; i < x.replicas.size(); ++i) {
        EXPECT_EQ(x.replicas.at(i).port, y.replicas.at(i).port);
        EXPECT_EQ(x.replicas.at(i).instance, y.replicas.at(i).instance);
      }
    }

    EXPECT_EQ(x.multicast_metadata, y.multicast_metadata);
    EXPECT_EQ(x.controller_metadata, y.controller_metadata);
    EXPECT_EQ(x.multicast_group_oid, y.multicast_group_oid);

    std::unordered_map<std::string, sai_object_id_t> member_oids;
    for (const auto& members : x.member_oids) {
      EXPECT_NE(y.member_oids.find(members.first), y.member_oids.end());
      if (y.member_oids.find(members.first) != y.member_oids.end()) {
        EXPECT_EQ(members.second, y.member_oids.at(members.first));
      }
    }
    for (auto& members : y.member_oids) {
      EXPECT_NE(x.member_oids.find(members.first), x.member_oids.end());
    }
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

    mock_sai_bridge = &mock_sai_bridge_;
    sai_bridge_api->create_bridge_port = mock_create_bridge_port;
    sai_bridge_api->remove_bridge_port = mock_remove_bridge_port;
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

  std::string VerifyMulticastRouterInterfaceStateAsicDb(
      const P4MulticastRouterInterfaceEntry* multicast_router_interface_entry) {
    return l3_multicast_manager_.verifyMulticastRouterInterfaceStateAsicDb(
        multicast_router_interface_entry);
  }

  std::string VerifyL2MulticastRouterInterfaceStateAsicDb(
      const P4MulticastRouterInterfaceEntry* multicast_router_interface_entry) {
    return l3_multicast_manager_.verifyL2MulticastRouterInterfaceStateAsicDb(
        multicast_router_interface_entry);
  }

  std::string VerifyMulticastGroupStateCache(
      const P4MulticastGroupEntry& app_db_entry,
      const P4MulticastGroupEntry* multicast_group_entry) {
    return l3_multicast_manager_.verifyMulticastGroupStateCache(
        app_db_entry, multicast_group_entry);
  }

  std::string VerifyMulticastGroupStateAsicDb(
      const P4MulticastGroupEntry* multicast_group_entry) {
    return l3_multicast_manager_.verifyMulticastGroupStateAsicDb(
        multicast_group_entry);
  }

  ReturnCodeOr<P4MulticastRouterInterfaceEntry>
  DeserializeMulticastRouterInterfaceEntry(
      const std::string& key,
      const std::vector<swss::FieldValueTuple>& attributes) {
    return l3_multicast_manager_.deserializeMulticastRouterInterfaceEntry(
        key, attributes);
  }

  ReturnCodeOr<P4MulticastGroupEntry> DeserializeMulticastGroupEntry(
      const std::string& key,
      const std::vector<swss::FieldValueTuple>& attributes) {
    return l3_multicast_manager_.deserializeMulticastGroupEntry(key,
                                                                attributes);
  }

  ReturnCode ValidateMulticastRouterInterfaceEntry(
      const P4MulticastRouterInterfaceEntry& multicast_router_interface_entry,
      const std::string& operation) {
    return l3_multicast_manager_.validateMulticastRouterInterfaceEntry(
        multicast_router_interface_entry, operation);
  }

  ReturnCode ValidateL2MulticastRouterInterfaceEntry(
      const P4MulticastRouterInterfaceEntry& multicast_router_interface_entry,
      const P4MulticastRouterInterfaceEntry* router_interface_entry_ptr) {
    return l3_multicast_manager_.validateL2MulticastRouterInterfaceEntry(
        multicast_router_interface_entry, router_interface_entry_ptr);
  }

  ReturnCode ValidateMulticastGroupEntry(
      const P4MulticastGroupEntry& multicast_group_entry,
      const std::string& operation) {
    return l3_multicast_manager_.validateMulticastGroupEntry(
        multicast_group_entry, operation);
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

  ReturnCode CreateBridgePort(P4MulticastRouterInterfaceEntry& entry,
                              sai_object_id_t* bridge_port_oid) {
    return l3_multicast_manager_.createBridgePort(entry, bridge_port_oid);
  }

  ReturnCode DeleteRouterInterface(const std::string& rif_key,
                                   sai_object_id_t rif_oid) {
    return l3_multicast_manager_.deleteRouterInterface(rif_key, rif_oid);
  }

  ReturnCode DeleteMulticastGroup(const std::string multicast_group_id,
                                  sai_object_id_t mcast_group_oid) {
    return l3_multicast_manager_.deleteMulticastGroup(multicast_group_id,
                                                      mcast_group_oid);
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

  std::vector<ReturnCode> AddMulticastGroupEntries(
      std::vector<P4MulticastGroupEntry>& entries) {
    return l3_multicast_manager_.addMulticastGroupEntries(entries);
  }

  std::vector<ReturnCode> DeleteMulticastGroupEntries(
      std::vector<P4MulticastGroupEntry>& entries) {
    return l3_multicast_manager_.deleteMulticastGroupEntries(entries);
  }

  std::vector<ReturnCode> UpdateMulticastGroupEntries(
      std::vector<P4MulticastGroupEntry>& entries) {
    return l3_multicast_manager_.updateMulticastGroupEntries(entries);
  }

  P4MulticastRouterInterfaceEntry* GetMulticastRouterInterfaceEntry(
      const std::string& multicast_router_interface_entry_key) {
    return l3_multicast_manager_.getMulticastRouterInterfaceEntry(
        multicast_router_interface_entry_key);
  }

  P4MulticastGroupEntry* GetMulticastGroupEntry(
      const std::string& multicast_group_id) {
    return l3_multicast_manager_.getMulticastGroupEntry(multicast_group_id);
  }

  sai_object_id_t GetRifOid(
      const P4MulticastRouterInterfaceEntry* multicast_router_interface_entry) {
    return l3_multicast_manager_.getRifOid(multicast_router_interface_entry);
  }

  sai_object_id_t GetBridgePortOid(
      const P4MulticastRouterInterfaceEntry* multicast_router_interface_entry) {
    return l3_multicast_manager_.getBridgePortOid(
        multicast_router_interface_entry);
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

  StrictMock<MockSaiRouterInterface> mock_sai_router_intf_;
  StrictMock<MockSaiIpmcGroup> mock_sai_ipmc_group_;
  StrictMock<MockSaiBridge> mock_sai_bridge_;
  StrictMock<MockResponsePublisher> publisher_;
  P4OidMapper p4_oid_mapper_;
  L3MulticastManager l3_multicast_manager_;
};

TEST_F(L3MulticastManagerTest, DeserializeMulticastRouterInterfaceEntryTest) {
  std::string key = R"({"match/multicast_replica_port":"Ethernet2",)"
                    R"("match/multicast_replica_instance":"0x0"})";
  std::vector<swss::FieldValueTuple> attributes;
  attributes.push_back(
      swss::FieldValueTuple{p4orch::kAction, p4orch::kSetMulticastSrcMac});
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
      swss::FieldValueTuple{p4orch::kAction, p4orch::kSetMulticastSrcMac});
  attributes.push_back(
      swss::FieldValueTuple{prependParamField(p4orch::kSrcMac), kSrcMac1});
  attributes.push_back(swss::FieldValueTuple{"extra_attr", "extra_attr_val"});

  auto router_interface_entry_or =
      DeserializeMulticastRouterInterfaceEntry(key, attributes);
  EXPECT_EQ(StatusCode::SWSS_RC_INVALID_PARAM,
            router_interface_entry_or.status());
}

TEST_F(L3MulticastManagerTest, DeserializeMulticastGroupEntryTest) {
  std::string key = "0x1";
  std::vector<swss::FieldValueTuple> attributes;
  const std::string json_array = R"([{"multicast_replica_instance":"0x0",)"
                                 R"("multicast_replica_port":"Ethernet1"},)"
                                 R"({"multicast_replica_instance":"0x0",)"
                                 R"("multicast_replica_port":"Ethernet2"}])";
  attributes.push_back(swss::FieldValueTuple{"replicas", json_array});
  attributes.push_back(
      swss::FieldValueTuple{p4orch::kMulticastMetadata, "meta1"});
  attributes.push_back(
      swss::FieldValueTuple{p4orch::kControllerMetadata, "so_meta"});

  auto replication_entry_or = DeserializeMulticastGroupEntry(key, attributes);
  ASSERT_TRUE(replication_entry_or.ok());
  auto& replication_entry = *replication_entry_or;
  auto expect_entry =
      GenerateP4MulticastGroupEntry("0x1",
                                    {P4Replica("0x1", "Ethernet1", "0x0"),
                                     P4Replica("0x1", "Ethernet2", "0x0")},
                                    "meta1", "so_meta");
  VerifyP4MulticastGroupEntryEqual(expect_entry, replication_entry);
}

TEST_F(L3MulticastManagerTest,
       DeserializeMulticastGroupEntryReplicasNotAnArray) {
  std::string key = "0x1";

  std::vector<swss::FieldValueTuple> attributes;
  attributes.push_back(swss::FieldValueTuple{"replicas", "{\"a\":\"b\"}"});
  attributes.push_back(
      swss::FieldValueTuple{p4orch::kControllerMetadata, "so_meta"});

  auto replication_entry_or = DeserializeMulticastGroupEntry(key, attributes);
  EXPECT_EQ(StatusCode::SWSS_RC_INVALID_PARAM, replication_entry_or.status());
}

TEST_F(L3MulticastManagerTest,
       DeserializeMulticastGroupEntryEmptyPortInReplicaTest) {
  std::string key = "0x1";

  std::vector<swss::FieldValueTuple> attributes;
  const std::string json_array = R"([{"multicast_replica_instance":"0x1",)"
                                 R"("multicast_replica_port":""}])";
  attributes.push_back(swss::FieldValueTuple{"replicas", json_array});
  attributes.push_back(
      swss::FieldValueTuple{p4orch::kControllerMetadata, "so_meta"});

  auto replication_entry_or = DeserializeMulticastGroupEntry(key, attributes);
  EXPECT_EQ(StatusCode::SWSS_RC_INVALID_PARAM, replication_entry_or.status());
}

TEST_F(L3MulticastManagerTest,
       DeserializeMulticastGroupEntryMissingPortInReplicaTest) {
  std::string key = "0x1";

  std::vector<swss::FieldValueTuple> attributes;
  const std::string json_array = R"([{"multicast_replica_instance":"0x1"}])";
  attributes.push_back(swss::FieldValueTuple{"replicas", json_array});
  attributes.push_back(
      swss::FieldValueTuple{p4orch::kControllerMetadata, "so_meta"});

  auto replication_entry_or = DeserializeMulticastGroupEntry(key, attributes);
  EXPECT_EQ(StatusCode::SWSS_RC_INVALID_PARAM, replication_entry_or.status());
}

TEST_F(L3MulticastManagerTest,
       DeserializeMulticastGroupEntryEmptyInstanceInReplicaTest) {
  std::string key = "0x1";

  std::vector<swss::FieldValueTuple> attributes;
  const std::string json_array = R"([{"multicast_replica_instance":"",)"
                                 R"("multicast_replica_port":"Ethernet1"}])";
  attributes.push_back(swss::FieldValueTuple{"replicas", json_array});
  attributes.push_back(
      swss::FieldValueTuple{p4orch::kControllerMetadata, "so_meta"});

  auto replication_entry_or = DeserializeMulticastGroupEntry(key, attributes);
  EXPECT_EQ(StatusCode::SWSS_RC_INVALID_PARAM, replication_entry_or.status());
}

TEST_F(L3MulticastManagerTest,
       DeserializeMulticastGroupEntryMissingInstanceInReplicaTest) {
  std::string key = "0x1";

  std::vector<swss::FieldValueTuple> attributes;
  const std::string json_array = R"([{"multicast_replica_port":"Ethernet1"}])";
  attributes.push_back(swss::FieldValueTuple{"replicas", json_array});
  attributes.push_back(
      swss::FieldValueTuple{p4orch::kControllerMetadata, "so_meta"});

  auto replication_entry_or = DeserializeMulticastGroupEntry(key, attributes);
  EXPECT_EQ(StatusCode::SWSS_RC_INVALID_PARAM, replication_entry_or.status());
}

TEST_F(L3MulticastManagerTest,
       DeserializeMulticastGroupEntryDuplicateReplicaTest) {
  std::string key = "0x1";

  std::vector<swss::FieldValueTuple> attributes;
  const std::string json_array = R"([{"multicast_replica_instance":"0x0",)"
                                 R"("multicast_replica_port":"Ethernet1"},)"
                                 R"({"multicast_replica_instance":"0x0",)"
                                 R"("multicast_replica_port":"Ethernet2"},)"
                                 R"({"multicast_replica_instance":"0x0",)"
                                 R"("multicast_replica_port":"Ethernet1"}])";
  attributes.push_back(swss::FieldValueTuple{"replicas", json_array});

  auto replication_entry_or = DeserializeMulticastGroupEntry(key, attributes);
  EXPECT_EQ(StatusCode::SWSS_RC_INVALID_PARAM, replication_entry_or.status());
}

TEST_F(L3MulticastManagerTest, DeserializeMulticastGroupEntryUnknownFieldTest) {
  std::string key = "0x1";

  std::vector<swss::FieldValueTuple> attributes;
  const std::string json_array = R"([{"multicast_replica_instance":"0x0",)"
                                 R"("multicast_replica_port":"Ethernet1"},)"
                                 R"({"multicast_replica_instance":"0x0",)"
                                 R"("multicast_replica_port":"Ethernet2"},)"
                                 R"({"multicast_replica_instance":"0x0",)"
                                 R"("multicast_replica_port":"Ethernet3"}])";
  attributes.push_back(swss::FieldValueTuple{"replicas", json_array});
  attributes.push_back(swss::FieldValueTuple{"extra", "unknown"});

  auto replication_entry_or = DeserializeMulticastGroupEntry(key, attributes);
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

TEST_F(L3MulticastManagerTest,
       AddMulticastRouterInterfaceEntryNoActionSuccess) {
  auto entry = SetupP4MulticastRouterInterfaceNoActionEntry(
      "Ethernet1", /*instance=*/"0x0", kBridgePortOid1);
  EXPECT_EQ(kBridgePortOid1, GetBridgePortOid(&entry));
}

TEST_F(L3MulticastManagerTest,
       AddMulticastRouterInterfaceEntryNoActionSaiFailure) {
  std::vector<P4MulticastRouterInterfaceEntry> entries;
  auto entry = GenerateP4MulticastRouterInterfaceEntry(
      "Ethernet1", /*instance=*/"0x0", swss::MacAddress(kSrcMac0),
      /*multicast_metadata=*/"", p4orch::kNoAction);
  auto entry2 = GenerateP4MulticastRouterInterfaceEntry(
      "Ethernet2", /*instance=*/"0x0", swss::MacAddress(kSrcMac0),
      /*multicast_metadata=*/"", p4orch::kNoAction);
  entries.push_back(entry);
  entries.push_back(entry2);

  EXPECT_CALL(mock_sai_bridge_, create_bridge_port(_, _, Eq(2), _))
      .WillOnce(Return(SAI_STATUS_FAILURE));

  std::vector<ReturnCode> statuses =
      AddMulticastRouterInterfaceEntries(entries);

  EXPECT_EQ(statuses.size(), 2);
  EXPECT_EQ(statuses[0].code(), StatusCode::SWSS_RC_UNKNOWN);
  EXPECT_EQ(statuses[1].code(), StatusCode::SWSS_RC_NOT_EXECUTED);

  EXPECT_EQ(GetMulticastRouterInterfaceEntry(
                entries[0].multicast_router_interface_entry_key),
            nullptr);
  EXPECT_EQ(GetMulticastRouterInterfaceEntry(
                entries[1].multicast_router_interface_entry_key),
            nullptr);
  EXPECT_EQ(GetBridgePortOid(&entries[0]), SAI_NULL_OBJECT_ID);
  EXPECT_EQ(GetBridgePortOid(&entries[1]), SAI_NULL_OBJECT_ID);
}

TEST_F(L3MulticastManagerTest,
       AddMulticastRouterInterfaceEntryNoActionInvalidPortFailure) {
  std::vector<P4MulticastRouterInterfaceEntry> entries;
  auto entry = GenerateP4MulticastRouterInterfaceEntry(
      "InvalidPort", /*instance=*/"0x0", swss::MacAddress(kSrcMac0),
      /*multicast_metadata=*/"", p4orch::kNoAction);
  auto entry2 = GenerateP4MulticastRouterInterfaceEntry(
      "Ethernet2", /*instance=*/"0x0", swss::MacAddress(kSrcMac0),
      /*multicast_metadata=*/"", p4orch::kNoAction);
  entries.push_back(entry);
  entries.push_back(entry2);

  std::vector<ReturnCode> statuses =
      AddMulticastRouterInterfaceEntries(entries);

  EXPECT_EQ(statuses.size(), 2);
  EXPECT_EQ(statuses[0].code(), StatusCode::SWSS_RC_NOT_FOUND);
  EXPECT_EQ(statuses[1].code(), StatusCode::SWSS_RC_NOT_EXECUTED);

  EXPECT_EQ(GetMulticastRouterInterfaceEntry(
                entries[0].multicast_router_interface_entry_key),
            nullptr);
  EXPECT_EQ(GetMulticastRouterInterfaceEntry(
                entries[1].multicast_router_interface_entry_key),
            nullptr);
  EXPECT_EQ(GetBridgePortOid(&entries[0]), SAI_NULL_OBJECT_ID);
  EXPECT_EQ(GetBridgePortOid(&entries[1]), SAI_NULL_OBJECT_ID);
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
       DeleteMulticastRouterInterfaceNoActionEntriesSuccess) {
  auto entry = SetupP4MulticastRouterInterfaceNoActionEntry(
      "Ethernet1", /*instance=*/"0x0", kBridgePortOid1);
  auto entry2 = SetupP4MulticastRouterInterfaceNoActionEntry(
      "Ethernet2", /*instance=*/"0x0", kBridgePortOid2);

  std::vector<ReturnCode> statuses;
  // Second, delete entries just added.  Expect success and no more references
  // to the old entries.
  std::vector<P4MulticastRouterInterfaceEntry> entries;
  entries.push_back(entry2);
  entries.push_back(entry);

  EXPECT_CALL(mock_sai_bridge_, remove_bridge_port(_))
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
  EXPECT_EQ(GetBridgePortOid(&entries[0]), SAI_NULL_OBJECT_ID);
  EXPECT_EQ(GetBridgePortOid(&entries[1]), SAI_NULL_OBJECT_ID);
}

TEST_F(L3MulticastManagerTest,
       DeleteMulticastRouterInterfaceNoActionEntriesSaiFailure) {
  auto entry = SetupP4MulticastRouterInterfaceNoActionEntry(
      "Ethernet1", /*instance=*/"0x0", kBridgePortOid1);
  auto entry2 = SetupP4MulticastRouterInterfaceNoActionEntry(
      "Ethernet2", /*instance=*/"0x0", kBridgePortOid2);

  std::vector<ReturnCode> statuses;
  // Second, delete entries just added.  Force SAI failure on first delete.
  std::vector<P4MulticastRouterInterfaceEntry> entries;
  entries.push_back(entry);
  entries.push_back(entry2);

  EXPECT_CALL(mock_sai_bridge_, remove_bridge_port(_))
      .WillOnce(Return(SAI_STATUS_FAILURE));
  statuses = DeleteMulticastRouterInterfaceEntries(entries);
  ASSERT_EQ(statuses.size(), 2);
  EXPECT_EQ(statuses[0].code(), StatusCode::SWSS_RC_UNKNOWN);
  EXPECT_EQ(statuses[1].code(), StatusCode::SWSS_RC_NOT_EXECUTED);

  // Expect entries to remain.
  EXPECT_NE(GetMulticastRouterInterfaceEntry(
                entries[0].multicast_router_interface_entry_key),
            nullptr);
  EXPECT_NE(GetMulticastRouterInterfaceEntry(
                entries[1].multicast_router_interface_entry_key),
            nullptr);
  EXPECT_EQ(GetBridgePortOid(&entries[0]), kBridgePortOid1);
  EXPECT_EQ(GetBridgePortOid(&entries[1]), kBridgePortOid2);
}

TEST_F(L3MulticastManagerTest,
       DeleteMulticastRouterInterfaceNoActionEntriesMissingOid) {
  auto entry = SetupP4MulticastRouterInterfaceNoActionEntry(
      "Ethernet1", /*instance=*/"0x0", kBridgePortOid1);
  auto entry2 = SetupP4MulticastRouterInterfaceNoActionEntry(
      "Ethernet2", /*instance=*/"0x0", kBridgePortOid2);

  std::vector<ReturnCode> statuses;
  // Second, delete entries just added.  Force OID to be missing from map.
  std::vector<P4MulticastRouterInterfaceEntry> entries;
  entries.push_back(entry);
  entries.push_back(entry2);

  // Force internal error.
  p4_oid_mapper_.decreaseRefCount(SAI_OBJECT_TYPE_BRIDGE_PORT,
                                  entry.multicast_replica_port);
  p4_oid_mapper_.eraseOID(SAI_OBJECT_TYPE_BRIDGE_PORT,
                          entry.multicast_replica_port);

  // Expect to enter critical state.

  statuses = DeleteMulticastRouterInterfaceEntries(entries);
  ASSERT_EQ(statuses.size(), 2);
  EXPECT_EQ(statuses[0].code(), StatusCode::SWSS_RC_INTERNAL);
  EXPECT_EQ(statuses[1].code(), StatusCode::SWSS_RC_NOT_EXECUTED);

  // Expect entries to remain.
  EXPECT_NE(GetMulticastRouterInterfaceEntry(
                entries[0].multicast_router_interface_entry_key),
            nullptr);
  EXPECT_NE(GetMulticastRouterInterfaceEntry(
                entries[1].multicast_router_interface_entry_key),
            nullptr);
  // The OID we didn't mess with should still be there.
  EXPECT_EQ(GetBridgePortOid(&entries[1]), kBridgePortOid2);
}

TEST_F(L3MulticastManagerTest,
       DeleteMulticastRouterInterfaceNoActionEntriesSamePortSuccess) {
  auto entry = SetupP4MulticastRouterInterfaceNoActionEntry(
      "Ethernet1", /*instance=*/"0x0", kBridgePortOid1);
  auto entry2 = SetupP4MulticastRouterInterfaceNoActionEntry(
      "Ethernet1", /*instance=*/"0x1", kBridgePortOid1, /*expect_mock=*/false);

  // Only delete one entry.  Expect bridge port to remain.
  std::vector<P4MulticastRouterInterfaceEntry> entries = {entry};
  std::vector<ReturnCode> statuses =
      DeleteMulticastRouterInterfaceEntries(entries);

  ASSERT_EQ(statuses.size(), 1);
  EXPECT_TRUE(statuses[0].ok());

  // Expect no more references to entries.
  EXPECT_EQ(GetMulticastRouterInterfaceEntry(
                entry.multicast_router_interface_entry_key),
            nullptr);
  EXPECT_NE(GetMulticastRouterInterfaceEntry(
                entry2.multicast_router_interface_entry_key),
            nullptr);
  EXPECT_EQ(GetBridgePortOid(&entry2), kBridgePortOid1);
}

TEST_F(L3MulticastManagerTest,
       UpdateMulticastRouterInterfaceNoActionEntriesSuccess) {
  auto entry = SetupP4MulticastRouterInterfaceNoActionEntry(
      "Ethernet1", /*instance=*/"0x0", kBridgePortOid1);
  auto entry2 = SetupP4MulticastRouterInterfaceNoActionEntry(
      "Ethernet2", /*instance=*/"0x0", kBridgePortOid2);

  std::vector<P4MulticastRouterInterfaceEntry> entries = {entry, entry2};
  std::vector<ReturnCode> statuses =
      UpdateMulticastRouterInterfaceEntries(entries);

  ASSERT_EQ(statuses.size(), 2);
  EXPECT_TRUE(statuses[0].ok());
  EXPECT_TRUE(statuses[1].ok());

  // Expect no more references to entries.
  EXPECT_NE(GetMulticastRouterInterfaceEntry(
                entry.multicast_router_interface_entry_key),
            nullptr);
  EXPECT_NE(GetMulticastRouterInterfaceEntry(
                entry2.multicast_router_interface_entry_key),
            nullptr);
  EXPECT_EQ(GetBridgePortOid(&entry), kBridgePortOid1);
  EXPECT_EQ(GetBridgePortOid(&entry2), kBridgePortOid2);
}

TEST_F(L3MulticastManagerTest,
       UpdateMulticastRouterInterfaceNoActionEntriesNoRaisesCritical) {
  auto entry = SetupP4MulticastRouterInterfaceNoActionEntry(
      "Ethernet1", /*instance=*/"0x0", kBridgePortOid1);
  auto entry2 = SetupP4MulticastRouterInterfaceNoActionEntry(
      "Ethernet2", /*instance=*/"0x0", kBridgePortOid2);
  auto entry3 = GenerateP4MulticastRouterInterfaceEntry(
      "Ethernet3", /*instance=*/"0x0", swss::MacAddress(kSrcMac0),
      /*multicast_metadata=*/"", p4orch::kNoAction);

  // Expect to enter critical state.

  std::vector<P4MulticastRouterInterfaceEntry> entries = {entry3, entry,
                                                          entry2};
  std::vector<ReturnCode> statuses =
      UpdateMulticastRouterInterfaceEntries(entries);

  ASSERT_EQ(statuses.size(), 3);
  EXPECT_EQ(statuses[0].code(), StatusCode::SWSS_RC_INTERNAL);
  EXPECT_EQ(statuses[1].code(), StatusCode::SWSS_RC_NOT_EXECUTED);
  EXPECT_EQ(statuses[2].code(), StatusCode::SWSS_RC_NOT_EXECUTED);
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
      swss::FieldValueTuple{p4orch::kAction, p4orch::kSetMulticastSrcMac});
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
       DrainMulticastRouterInterfaceEntryMissingSrcMac) {
  const std::string match_key =
      R"({"match/multicast_replica_port":"Ethernet1",)"
      R"("match/multicast_replica_instance":"0x0"})";
  const std::string appl_db_key =
      std::string(APP_P4RT_MULTICAST_ROUTER_INTERFACE_TABLE_NAME) +
      kTableKeyDelimiter + match_key;

  std::vector<swss::FieldValueTuple> attributes;
  attributes.push_back(
      swss::FieldValueTuple{p4orch::kAction, p4orch::kSetMulticastSrcMac});
  // Source MAC is missing on purpose.

  Enqueue(APP_P4RT_MULTICAST_ROUTER_INTERFACE_TABLE_NAME,
          swss::KeyOpFieldsValuesTuple(appl_db_key, SET_COMMAND, attributes));

  EXPECT_CALL(publisher_,
              publish(Eq(APP_P4RT_TABLE_NAME), Eq(appl_db_key), Eq(attributes),
                      Eq(StatusCode::SWSS_RC_INVALID_PARAM), Eq(true)));
  EXPECT_EQ(StatusCode::SWSS_RC_INVALID_PARAM, Drain(/*failure_before=*/false));

  const auto key =
      KeyGenerator::generateMulticastRouterInterfaceKey("Ethernet1", "0x0");
  auto* actual_entry = GetMulticastRouterInterfaceEntry(key);
  ASSERT_EQ(nullptr, actual_entry);
}

TEST_F(L3MulticastManagerTest, DrainMulticastRouterInterfaceNoActionEntry) {
  const std::string match_key =
      R"({"match/multicast_replica_port":"Ethernet1",)"
      R"("match/multicast_replica_instance":"0x0"})";
  const std::string appl_db_key =
      std::string(APP_P4RT_MULTICAST_ROUTER_INTERFACE_TABLE_NAME) +
      kTableKeyDelimiter + match_key;

  // Enqueue entry for create operation.
  auto expect_entry = GenerateP4MulticastRouterInterfaceEntry(
      "Ethernet1", /*instance=*/"0x0", swss::MacAddress(kSrcMac0),
      /*multicast_metadata=*/"", p4orch::kNoAction);

  std::vector<swss::FieldValueTuple> attributes;
  attributes.push_back(
      swss::FieldValueTuple{p4orch::kAction, p4orch::kNoAction});
  attributes.push_back(
      swss::FieldValueTuple{p4orch::kControllerMetadata, "so_meta"});

  Enqueue(APP_P4RT_MULTICAST_ROUTER_INTERFACE_TABLE_NAME,
          swss::KeyOpFieldsValuesTuple(appl_db_key, SET_COMMAND, attributes));

  EXPECT_CALL(mock_sai_bridge_, create_bridge_port(_, _, _, _))
      .WillOnce(
          DoAll(SetArgPointee<0>(kBridgePortOid1), Return(SAI_STATUS_SUCCESS)));
  EXPECT_CALL(publisher_,
              publish(Eq(APP_P4RT_TABLE_NAME), Eq(appl_db_key), Eq(attributes),
                      Eq(StatusCode::SWSS_RC_SUCCESS), Eq(true)));
  EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS, Drain(/*failure_before=*/false));
  auto* actual_entry = GetMulticastRouterInterfaceEntry(
      expect_entry.multicast_router_interface_entry_key);
  ASSERT_NE(nullptr, actual_entry);
  VerifyP4MulticastRouterInterfaceEntryEqual(expect_entry, *actual_entry);
  auto bridge_port_oid = GetBridgePortOid(actual_entry);
  EXPECT_EQ(bridge_port_oid, kBridgePortOid1);
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
      swss::FieldValueTuple{p4orch::kAction, p4orch::kSetMulticastSrcMac});
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
      swss::FieldValueTuple{p4orch::kAction, p4orch::kSetMulticastSrcMac});
  attributes1.push_back(
      swss::FieldValueTuple{prependParamField(p4orch::kSrcMac), kSrcMac1});
  attributes1.push_back(
      swss::FieldValueTuple{p4orch::kControllerMetadata, "so_meta"});

  std::vector<swss::FieldValueTuple> attributes2;
  attributes2.push_back(
      swss::FieldValueTuple{p4orch::kAction, p4orch::kSetMulticastSrcMac});
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
      swss::FieldValueTuple{p4orch::kAction, p4orch::kSetMulticastSrcMac});

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
      swss::FieldValueTuple{p4orch::kAction, p4orch::kSetMulticastSrcMac});
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
      swss::FieldValueTuple{p4orch::kAction, p4orch::kSetMulticastSrcMac});
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
      swss::FieldValueTuple{p4orch::kAction, p4orch::kSetMulticastSrcMac});
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
      swss::FieldValueTuple{p4orch::kAction, p4orch::kSetMulticastSrcMac});
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
      swss::FieldValueTuple{p4orch::kAction, p4orch::kSetMulticastSrcMac});

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
      swss::FieldValueTuple{p4orch::kAction, p4orch::kSetMulticastSrcMac});
  attributes.push_back(
      swss::FieldValueTuple{prependParamField(p4orch::kSrcMac), kSrcMac2});
  attributes.push_back(
      swss::FieldValueTuple{p4orch::kControllerMetadata, "so_meta"});

  const std::string group_match_key =
      R"({"match/multicast_group_id":"0x1",)"
      R"("match/multicast_replica_port":"Ethernet1",)"
      R"("match/multicast_replica_instance":"0x1"})";
  const std::string group_appl_db_key =
      std::string(APP_P4RT_REPLICATION_IP_MULTICAST_TABLE_NAME) +
      kTableKeyDelimiter + group_match_key;
  std::vector<swss::FieldValueTuple> group_attributes;
  group_attributes.push_back(
      swss::FieldValueTuple{p4orch::kControllerMetadata, "so_meta"});

  Enqueue(APP_P4RT_MULTICAST_ROUTER_INTERFACE_TABLE_NAME,
          swss::KeyOpFieldsValuesTuple(appl_db_key, SET_COMMAND, attributes));
  Enqueue(APP_P4RT_REPLICATION_IP_MULTICAST_TABLE_NAME,
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

TEST_F(L3MulticastManagerTest, ValidateL2MulticastRouterInterfaceEntrySuccess) {
  auto entry = SetupP4MulticastRouterInterfaceNoActionEntry(
      "Ethernet1", /*instance=*/"0x0", kBridgePortOid1);
  EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS,
            ValidateMulticastRouterInterfaceEntry(entry, SET_COMMAND));
  EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS,
            ValidateMulticastRouterInterfaceEntry(entry, DEL_COMMAND));
}

TEST_F(L3MulticastManagerTest,
       ValidateL2MulticastRouterInterfaceEntryNotInMapper) {
  auto entry = GenerateP4MulticastRouterInterfaceEntry(
      "Ethernet1", /*instance=*/"0x0", swss::MacAddress(kSrcMac0),
      /*multicast_metadata=*/"", p4orch::kNoAction);
  ReturnCode status = ValidateL2MulticastRouterInterfaceEntry(entry, &entry);
  EXPECT_EQ(StatusCode::SWSS_RC_NOT_FOUND, status);
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
       ValidateSetMulticastRouterInterfaceEntryEmptyActionTest) {
  auto entry = GenerateP4MulticastRouterInterfaceEntry(
      "Ethernet2", "0x0", swss::MacAddress(kSrcMac1));
  entry.action = "";
  ReturnCode status = ValidateMulticastRouterInterfaceEntry(entry, SET_COMMAND);
  EXPECT_EQ(StatusCode::SWSS_RC_INVALID_PARAM, status);
}

TEST_F(L3MulticastManagerTest,
       ValidateSetMulticastRouterInterfaceEntryActionChangeFails) {
  auto entry = SetupP4MulticastRouterInterfaceEntry(
      "Ethernet1", "0x0", swss::MacAddress(kSrcMac1), kRifOid1);
  entry.action = p4orch::kNoAction;
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
  const std::string db_key =
      std::string(APP_P4RT_TABLE_NAME) + kTableKeyDelimiter + appl_db_key;
  std::vector<swss::FieldValueTuple> attributes;
  attributes.push_back(
      swss::FieldValueTuple{p4orch::kAction, p4orch::kSetMulticastSrcMac});
  attributes.push_back(
      swss::FieldValueTuple{prependParamField(p4orch::kSrcMac), kSrcMac1});
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
  table.del("SAI_OBJECT_TYPE_ROUTER_INTERFACE:oid:0x123456");
}

TEST_F(L3MulticastManagerTest,
       VerifyStateMulticastRouterInterfaceNoActionSuccess) {
  auto entry = SetupP4MulticastRouterInterfaceNoActionEntry(
      "Ethernet1", /*instance=*/"0x0", kBridgePortOid1);

  const std::string match_key =
      R"({"match/multicast_replica_port":"Ethernet1",)"
      R"("match/multicast_replica_instance":"0x0"})";
  const std::string appl_db_key =
      std::string(APP_P4RT_MULTICAST_ROUTER_INTERFACE_TABLE_NAME) +
      kTableKeyDelimiter + match_key;
  const std::string db_key =
      std::string(APP_P4RT_TABLE_NAME) + kTableKeyDelimiter + appl_db_key;
  std::vector<swss::FieldValueTuple> attributes;
  attributes.push_back(
      swss::FieldValueTuple{p4orch::kAction, p4orch::kNoAction});

  // Setup ASIC DB.
  swss::Table table(nullptr, "ASIC_STATE");
  table.set("SAI_OBJECT_TYPE_BRIDGE_PORT:oid:0x101",
            std::vector<swss::FieldValueTuple>{
                swss::FieldValueTuple{"SAI_BRIDGE_PORT_ATTR_TYPE",
                                      "SAI_BRIDGE_PORT_TYPE_PORT"},
                swss::FieldValueTuple{"SAI_BRIDGE_PORT_ATTR_PORT_ID",
                                      "oid:0x112233"}});

  // Verification should succeed with vaild key and value.
  EXPECT_EQ(VerifyState(db_key, attributes), "");
  table.del("SAI_OBJECT_TYPE_BRIDGE_PORT:oid:0x101");
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
  const std::string db_key =
      std::string(APP_P4RT_TABLE_NAME) + kTableKeyDelimiter + appl_db_key;

  std::vector<swss::FieldValueTuple> attributes;
  attributes.push_back(
      swss::FieldValueTuple{p4orch::kAction, p4orch::kSetMulticastSrcMac});
  // Use wrong source mac so state cache fails.
  attributes.push_back(
      swss::FieldValueTuple{prependParamField(p4orch::kSrcMac), kSrcMac2});
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

TEST_F(L3MulticastManagerTest,
       VerifyStateMulticastRouterInterfaceTestNoActionAsicDbSuccess) {
  auto entry = SetupP4MulticastRouterInterfaceNoActionEntry(
      "Ethernet1", /*instance=*/"0x0", kBridgePortOid1);
  // Setup ASIC DB.
  swss::Table table(nullptr, "ASIC_STATE");
  table.set("SAI_OBJECT_TYPE_BRIDGE_PORT:oid:0x101",
            std::vector<swss::FieldValueTuple>{
                swss::FieldValueTuple{"SAI_BRIDGE_PORT_ATTR_TYPE",
                                      "SAI_BRIDGE_PORT_TYPE_PORT"},
                swss::FieldValueTuple{"SAI_BRIDGE_PORT_ATTR_PORT_ID",
                                      "oid:0x112233"}});
  EXPECT_TRUE(VerifyMulticastRouterInterfaceStateAsicDb(&entry).empty());
  table.del("SAI_OBJECT_TYPE_BRIDGE_PORT:oid:0x101");
}

TEST_F(L3MulticastManagerTest,
       VerifyStateMulticastRouterInterfaceTestNoActionAsicDbMissingKey) {
  auto entry = SetupP4MulticastRouterInterfaceNoActionEntry(
      "Ethernet1", /*instance=*/"0x0", kBridgePortOid1);
  // Setup ASIC DB.
  swss::Table table(nullptr, "ASIC_STATE");
  EXPECT_FALSE(VerifyMulticastRouterInterfaceStateAsicDb(&entry).empty());
}

TEST_F(L3MulticastManagerTest,
       VerifyStateMulticastRouterInterfaceTestNoActionAsicDbAttributeMismatch) {
  auto entry = SetupP4MulticastRouterInterfaceNoActionEntry(
      "Ethernet1", /*instance=*/"0x0", kBridgePortOid1);
  // Setup ASIC DB.
  swss::Table table(nullptr, "ASIC_STATE");
  table.set("SAI_OBJECT_TYPE_BRIDGE_PORT:oid:0x101",
            std::vector<swss::FieldValueTuple>{
                swss::FieldValueTuple{"SAI_BRIDGE_PORT_ATTR_TYPE",
                                      "SAI_BRIDGE_PORT_TYPE_PORT"},
                swss::FieldValueTuple{"SAI_BRIDGE_PORT_ATTR_PORT_ID",
                                      "oid:0x88888888888"}});
  EXPECT_FALSE(VerifyMulticastRouterInterfaceStateAsicDb(&entry).empty());
  table.del("SAI_OBJECT_TYPE_BRIDGE_PORT:oid:0x101");
}

TEST_F(L3MulticastManagerTest,
       VerifyStateMulticastRouterInterfaceTestNoActionUnknownPortFails) {
  auto entry = SetupP4MulticastRouterInterfaceNoActionEntry(
      "Ethernet1", /*instance=*/"0x0", kBridgePortOid1);
  entry.multicast_replica_port = "unknown";

  // Verification should fail, since the port is unknown.
  EXPECT_FALSE(VerifyMulticastRouterInterfaceStateAsicDb(&entry).empty());
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

  const std::string not_p4rt =
      std::string("Wrong") + kTableKeyDelimiter + appl_db_key;
  EXPECT_EQ(VerifyState(not_p4rt, attributes),
            "Invalid key, unexpected P4RT table: " + not_p4rt);

  const std::string bad_appl_db_key =
      std::string(APP_P4RT_TUNNEL_TABLE_NAME) + kTableKeyDelimiter + match_key;
  const std::string bad_db_key =
      std::string(APP_P4RT_TABLE_NAME) + kTableKeyDelimiter + bad_appl_db_key;
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
  const std::string bad_db_key =
      std::string(APP_P4RT_TABLE_NAME) + kTableKeyDelimiter + bad_appl_db_key;

  const std::string match_key =
      R"({"match/multicast_replica_port":"Ethernet1",)"
      R"("match/multicast_replica_instance":"0x1"})";
  const std::string appl_db_key =
      std::string(APP_P4RT_MULTICAST_ROUTER_INTERFACE_TABLE_NAME) +
      kTableKeyDelimiter + match_key;
  const std::string db_key =
      std::string(APP_P4RT_TABLE_NAME) + kTableKeyDelimiter + appl_db_key;
  std::vector<swss::FieldValueTuple> attributes;
  attributes.push_back(
      swss::FieldValueTuple{p4orch::kAction, p4orch::kSetMulticastSrcMac});
  attributes.push_back(
      swss::FieldValueTuple{prependParamField(p4orch::kSrcMac), kSrcMac1});
  attributes.push_back(
      swss::FieldValueTuple{p4orch::kControllerMetadata, "so_meta"});

  EXPECT_EQ(
      VerifyState(bad_db_key, attributes),
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
      GenerateP4MulticastRouterInterfaceEntry("", "0x1",
                                              swss::MacAddress(kSrcMac1));
  EXPECT_FALSE(VerifyMulticastRouterInterfaceStateCache(
                   missing_multicast_replica_port, &internal_entry)
                   .empty());

  // Mismatch on key.
  P4MulticastRouterInterfaceEntry key_mismatch =
      GenerateP4MulticastRouterInterfaceEntry(
          "Ethernet2", "0x1", swss::MacAddress(kSrcMac1), "meta1");
  EXPECT_FALSE(
      VerifyMulticastRouterInterfaceStateCache(key_mismatch, &internal_entry)
          .empty());

  // Mismatch on multicast_replica_port.
  P4MulticastRouterInterfaceEntry port_mismatch =
      GenerateP4MulticastRouterInterfaceEntry(
          "Ethernet1", "0x1", swss::MacAddress(kSrcMac1), "meta1");
  port_mismatch.multicast_replica_port = "Ethernet2";
  EXPECT_FALSE(
      VerifyMulticastRouterInterfaceStateCache(port_mismatch, &internal_entry)
          .empty());

  // Mismatch on multicast_replica_instance.
  P4MulticastRouterInterfaceEntry instance_mismatch =
      GenerateP4MulticastRouterInterfaceEntry(
          "Ethernet1", "0x1", swss::MacAddress(kSrcMac1), "meta1");
  instance_mismatch.multicast_replica_instance = "0x2";
  EXPECT_FALSE(VerifyMulticastRouterInterfaceStateCache(instance_mismatch,
                                                        &internal_entry)
                   .empty());

  // Mismatch on src_mac.
  P4MulticastRouterInterfaceEntry mac_mismatch =
      GenerateP4MulticastRouterInterfaceEntry(
          "Ethernet1", "0x1", swss::MacAddress(kSrcMac2), "meta1");
  EXPECT_FALSE(
      VerifyMulticastRouterInterfaceStateCache(mac_mismatch, &internal_entry)
          .empty());

  // Mismatch on multicast_metadata.
  P4MulticastRouterInterfaceEntry metadata_mismatch =
      GenerateP4MulticastRouterInterfaceEntry(
          "Ethernet1", "0x1", swss::MacAddress(kSrcMac1), "meta2");
  EXPECT_FALSE(VerifyMulticastRouterInterfaceStateCache(metadata_mismatch,
                                                        &internal_entry)
                   .empty());
}

TEST_F(L3MulticastManagerTest, DeleteMulticastGroupFailureNotInMapper) {
  EXPECT_EQ(StatusCode::SWSS_RC_INTERNAL,
            DeleteMulticastGroup("0x1", kGroupOid1));
}

TEST_F(L3MulticastManagerTest, AddMulticastGroupEntriesNoRifTest) {
  auto entry1 = GenerateP4MulticastGroupEntry(
      "0x1", {P4Replica("0x1", "Ethernet1", "0x0"),
              P4Replica("0x1", "Ethernet2", "0x0")});
  auto entry2 = GenerateP4MulticastGroupEntry(
      "0x2", {P4Replica("0x2", "Ethernet1", "0x0"),
              P4Replica("0x2", "Ethernet2", "0x0")});
  std::vector<P4MulticastGroupEntry> entries = {entry1, entry2};

  std::vector<ReturnCode> statuses = AddMulticastGroupEntries(entries);
  EXPECT_EQ(statuses.size(), 2);
  EXPECT_EQ(statuses[0], StatusCode::SWSS_RC_UNAVAIL);
  EXPECT_EQ(statuses[1], StatusCode::SWSS_RC_NOT_EXECUTED);
}

TEST_F(L3MulticastManagerTest, AddMulticastGroupEntriesOneAddTest) {
  // Add router interface entry so have RIF.
  auto rif_entry1 = SetupP4MulticastRouterInterfaceEntry(
      "Ethernet1", "0x0", swss::MacAddress(kSrcMac1), kRifOid1);
  auto rif_entry2 = SetupP4MulticastRouterInterfaceEntry(
      "Ethernet2", "0x0", swss::MacAddress(kSrcMac2), kRifOid2);

  P4Replica replica1 = P4Replica("0x1", "Ethernet1", "0x0");
  P4Replica replica2 = P4Replica("0x1", "Ethernet2", "0x0");

  SetupP4MulticastGroupEntry("0x1", {replica1, replica2}, kGroupOid1,
                             {kGroupMemberOid1, kGroupMemberOid2});

  auto* final_entry = GetMulticastGroupEntry("0x1");

  EXPECT_NE(final_entry, nullptr);
  EXPECT_EQ(final_entry->multicast_group_oid, kGroupOid1);
  EXPECT_EQ(final_entry->member_oids[replica1.key], kGroupMemberOid1);
  EXPECT_EQ(final_entry->member_oids[replica2.key], kGroupMemberOid2);
  EXPECT_TRUE(p4_oid_mapper_.existsOID(SAI_OBJECT_TYPE_IPMC_GROUP, "0x1"));

  sai_object_id_t end_groupOid1 = SAI_NULL_OBJECT_ID;
  sai_object_id_t end_groupOid2 = SAI_NULL_OBJECT_ID;
  p4_oid_mapper_.getOID(SAI_OBJECT_TYPE_IPMC_GROUP, "0x1", &end_groupOid1);
  p4_oid_mapper_.getOID(SAI_OBJECT_TYPE_IPMC_GROUP, "0x2", &end_groupOid2);
  EXPECT_EQ(end_groupOid1, kGroupOid1);
  EXPECT_EQ(end_groupOid2, SAI_NULL_OBJECT_ID);
  sai_object_id_t end_groupMemberOid1 = SAI_NULL_OBJECT_ID;
  sai_object_id_t end_groupMemberOid2 = SAI_NULL_OBJECT_ID;
  p4_oid_mapper_.getOID(SAI_OBJECT_TYPE_IPMC_GROUP_MEMBER, replica1.key,
                        &end_groupMemberOid1);
  p4_oid_mapper_.getOID(SAI_OBJECT_TYPE_IPMC_GROUP_MEMBER, replica2.key,
                        &end_groupMemberOid2);
  EXPECT_EQ(end_groupMemberOid1, kGroupMemberOid1);
  EXPECT_EQ(end_groupMemberOid2, kGroupMemberOid2);
}

TEST_F(L3MulticastManagerTest, AddMulticastGroupEntriesCreateGroupFailsTest) {
  // Add router interface entry so have RIF.
  auto rif_entry1 = SetupP4MulticastRouterInterfaceEntry(
      "Ethernet1", "0x2", swss::MacAddress(kSrcMac1), kRifOid1);
  auto rif_entry2 = SetupP4MulticastRouterInterfaceEntry(
      "Ethernet2", "0x2", swss::MacAddress(kSrcMac2), kRifOid2);

  P4Replica replica1 = P4Replica("0x1", "Ethernet1", "0x2");
  P4Replica replica2 = P4Replica("0x1", "Ethernet2", "0x2");
  P4Replica replica3 = P4Replica("0x2", "Ethernet1", "0x2");

  auto group_entry1 =
      GenerateP4MulticastGroupEntry("0x1", {replica1, replica2});
  auto group_entry2 = GenerateP4MulticastGroupEntry("0x2", {replica3});
  std::vector<P4MulticastGroupEntry> entries = {group_entry1, group_entry2};

  EXPECT_CALL(mock_sai_ipmc_group_, create_ipmc_group(_, _, _, _))
      .WillOnce(Return(SAI_STATUS_FAILURE));

  auto statuses = AddMulticastGroupEntries(entries);
  EXPECT_EQ(statuses.size(), 2);
  EXPECT_EQ(statuses[0], StatusCode::SWSS_RC_UNKNOWN);
  EXPECT_EQ(statuses[1], StatusCode::SWSS_RC_NOT_EXECUTED);
  EXPECT_FALSE(p4_oid_mapper_.existsOID(SAI_OBJECT_TYPE_IPMC_GROUP, "0x1"));
  EXPECT_FALSE(p4_oid_mapper_.existsOID(SAI_OBJECT_TYPE_IPMC_GROUP, "0x2"));
}

TEST_F(L3MulticastManagerTest,
       AddMulticastGroupEntriesCreateGroupMemberFailsTest) {
  // Add router interface entry so have RIF.
  auto rif_entry1 = SetupP4MulticastRouterInterfaceEntry(
      "Ethernet1", "0x1", swss::MacAddress(kSrcMac1), kRifOid1);
  auto rif_entry2 = SetupP4MulticastRouterInterfaceEntry(
      "Ethernet2", "0x2", swss::MacAddress(kSrcMac2), kRifOid2);

  P4Replica replica1 = P4Replica("0x1", "Ethernet1", "0x1");
  P4Replica replica2 = P4Replica("0x1", "Ethernet2", "0x2");
  P4Replica replica3 = P4Replica("0x2", "Ethernet1", "0x2");

  auto group_entry1 =
      GenerateP4MulticastGroupEntry("0x1", {replica1, replica2});
  auto group_entry2 = GenerateP4MulticastGroupEntry("0x2", {replica3});
  std::vector<P4MulticastGroupEntry> entries = {group_entry1, group_entry2};

  EXPECT_CALL(mock_sai_ipmc_group_, create_ipmc_group(_, _, _, _))
      .WillOnce(
          DoAll(SetArgPointee<0>(kGroupOid1), Return(SAI_STATUS_SUCCESS)));
  EXPECT_CALL(mock_sai_ipmc_group_, create_ipmc_group_member(_, _, _, _))
      .WillOnce(Return(SAI_STATUS_FAILURE));
  EXPECT_CALL(mock_sai_ipmc_group_, remove_ipmc_group(kGroupOid1))
      .WillOnce(Return(SAI_STATUS_SUCCESS));

  auto statuses = AddMulticastGroupEntries(entries);
  EXPECT_EQ(statuses.size(), 2);
  EXPECT_EQ(statuses[0], StatusCode::SWSS_RC_UNKNOWN);
  EXPECT_EQ(statuses[1], StatusCode::SWSS_RC_NOT_EXECUTED);
}

TEST_F(L3MulticastManagerTest,
       AddMulticastGroupEntriesCreateGroupMemberFailsBackoutFailsTest) {
  // Add router interface entry so have RIF.
  auto rif_entry1 = SetupP4MulticastRouterInterfaceEntry(
      "Ethernet1", "0x1", swss::MacAddress(kSrcMac1), kRifOid1);
  auto rif_entry2 = SetupP4MulticastRouterInterfaceEntry(
      "Ethernet2", "0x1", swss::MacAddress(kSrcMac2), kRifOid2);

  P4Replica replica1 = P4Replica("0x1", "Ethernet1", "0x1");
  P4Replica replica2 = P4Replica("0x1", "Ethernet2", "0x1");
  P4Replica replica3 = P4Replica("0x2", "Ethernet1", "0x1");

  auto group_entry1 =
      GenerateP4MulticastGroupEntry("0x1", {replica1, replica2});
  auto group_entry2 = GenerateP4MulticastGroupEntry("0x2", {replica3});
  std::vector<P4MulticastGroupEntry> entries = {group_entry1, group_entry2};

  EXPECT_CALL(mock_sai_ipmc_group_, create_ipmc_group(_, _, _, _))
      .WillOnce(
          DoAll(SetArgPointee<0>(kGroupOid1), Return(SAI_STATUS_SUCCESS)));
  EXPECT_CALL(mock_sai_ipmc_group_, create_ipmc_group_member(_, _, _, _))
      .WillOnce(Return(SAI_STATUS_FAILURE));
  EXPECT_CALL(mock_sai_ipmc_group_, remove_ipmc_group(kGroupOid1))
      .WillOnce(Return(SAI_STATUS_FAILURE));

  auto statuses = AddMulticastGroupEntries(entries);
  EXPECT_EQ(statuses.size(), 2);
  EXPECT_EQ(statuses[0], StatusCode::SWSS_RC_UNKNOWN);
  EXPECT_EQ(statuses[1], StatusCode::SWSS_RC_NOT_EXECUTED);
}

TEST_F(L3MulticastManagerTest,
       AddMulticastGroupEntriesCreateGroupMemberFailsBackoutSucceedsTest) {
  // Add router interface entry so have RIF.
  auto rif_entry1 = SetupP4MulticastRouterInterfaceEntry(
      "Ethernet1", "0x1", swss::MacAddress(kSrcMac1), kRifOid1);
  auto rif_entry2 = SetupP4MulticastRouterInterfaceEntry(
      "Ethernet2", "0x2", swss::MacAddress(kSrcMac2), kRifOid2);

  P4Replica replica1 = P4Replica("0x1", "Ethernet1", "0x1");
  P4Replica replica2 = P4Replica("0x1", "Ethernet2", "0x2");
  P4Replica replica3 = P4Replica("0x2", "Ethernet1", "0x2");

  auto group_entry1 =
      GenerateP4MulticastGroupEntry("0x1", {replica1, replica2});
  auto group_entry2 = GenerateP4MulticastGroupEntry("0x2", {replica3});
  std::vector<P4MulticastGroupEntry> entries = {group_entry1, group_entry2};

  EXPECT_CALL(mock_sai_ipmc_group_, create_ipmc_group(_, _, _, _))
      .WillOnce(
          DoAll(SetArgPointee<0>(kGroupOid1), Return(SAI_STATUS_SUCCESS)));
  EXPECT_CALL(mock_sai_ipmc_group_, create_ipmc_group_member(_, _, _, _))
      .WillOnce(
          DoAll(SetArgPointee<0>(kGroupMemberOid1), Return(SAI_STATUS_SUCCESS)))
      .WillOnce(Return(SAI_STATUS_FAILURE));
  EXPECT_CALL(mock_sai_ipmc_group_, remove_ipmc_group_member(kGroupMemberOid1))
      .WillOnce(Return(SAI_STATUS_SUCCESS));
  EXPECT_CALL(mock_sai_ipmc_group_, remove_ipmc_group(kGroupOid1))
      .WillOnce(Return(SAI_STATUS_SUCCESS));

  auto statuses = AddMulticastGroupEntries(entries);
  EXPECT_EQ(statuses.size(), 2);
  EXPECT_EQ(statuses[0], StatusCode::SWSS_RC_UNKNOWN);
  EXPECT_EQ(statuses[1], StatusCode::SWSS_RC_NOT_EXECUTED);
}

TEST_F(L3MulticastManagerTest,
       AddMulticastGroupEntriesCreateGroupMemberFailsBackoutMemberFailsTest) {
  // Add router interface entry so have RIF.
  auto rif_entry1 = SetupP4MulticastRouterInterfaceEntry(
      "Ethernet1", "0x1", swss::MacAddress(kSrcMac1), kRifOid1);
  auto rif_entry2 = SetupP4MulticastRouterInterfaceEntry(
      "Ethernet2", "0x2", swss::MacAddress(kSrcMac2), kRifOid2);

  P4Replica replica1 = P4Replica("0x1", "Ethernet1", "0x1");
  P4Replica replica2 = P4Replica("0x1", "Ethernet2", "0x2");
  P4Replica replica3 = P4Replica("0x2", "Ethernet1", "0x2");

  auto group_entry1 =
      GenerateP4MulticastGroupEntry("0x1", {replica1, replica2});
  auto group_entry2 = GenerateP4MulticastGroupEntry("0x2", {replica3});
  std::vector<P4MulticastGroupEntry> entries = {group_entry1, group_entry2};

  EXPECT_CALL(mock_sai_ipmc_group_, create_ipmc_group(_, _, _, _))
      .WillOnce(
          DoAll(SetArgPointee<0>(kGroupOid1), Return(SAI_STATUS_SUCCESS)));
  EXPECT_CALL(mock_sai_ipmc_group_, create_ipmc_group_member(_, _, _, _))
      .WillOnce(
          DoAll(SetArgPointee<0>(kGroupMemberOid1), Return(SAI_STATUS_SUCCESS)))
      .WillOnce(Return(SAI_STATUS_FAILURE));
  EXPECT_CALL(mock_sai_ipmc_group_, remove_ipmc_group_member(kGroupMemberOid1))
      .WillOnce(Return(SAI_STATUS_FAILURE));

  auto statuses = AddMulticastGroupEntries(entries);
  EXPECT_EQ(statuses.size(), 2);
  EXPECT_EQ(statuses[0], StatusCode::SWSS_RC_UNKNOWN);
  EXPECT_EQ(statuses[1], StatusCode::SWSS_RC_NOT_EXECUTED);
}

TEST_F(L3MulticastManagerTest, DeleteMulticastGroupEntriesNoEntry) {
  auto rif_entry1 = SetupP4MulticastRouterInterfaceEntry(
      "Ethernet1", "0x0", swss::MacAddress(kSrcMac1), kRifOid1);
  auto rif_entry2 = SetupP4MulticastRouterInterfaceEntry(
      "Ethernet2", "0x0", swss::MacAddress(kSrcMac2), kRifOid2);
  auto entry1 = GenerateP4MulticastGroupEntry(
      "0x1", {P4Replica("0x1", "Ethernet1", "0x0")});
  auto entry2 = GenerateP4MulticastGroupEntry(
      "0x2", {P4Replica("0x2", "Ethernet2", "0x0")});
  std::vector<P4MulticastGroupEntry> entries = {entry1, entry2};

  // Can't delete what isn't there.
  std::vector<ReturnCode> statuses = DeleteMulticastGroupEntries(entries);
  EXPECT_EQ(statuses.size(), 2);
  EXPECT_EQ(statuses[0], StatusCode::SWSS_RC_UNKNOWN);
  EXPECT_EQ(statuses[1], StatusCode::SWSS_RC_NOT_EXECUTED);
}

TEST_F(L3MulticastManagerTest, DeleteMulticastGroupEntriesNoRif) {
  // Add router interface entry so have RIF.
  auto rif_entry1 = SetupP4MulticastRouterInterfaceEntry(
      "Ethernet1", "0x0", swss::MacAddress(kSrcMac1), kRifOid1);
  auto rif_entry2 = SetupP4MulticastRouterInterfaceEntry(
      "Ethernet2", "0x0", swss::MacAddress(kSrcMac2), kRifOid2);

  auto group_entry1 = SetupP4MulticastGroupEntry(
      "0x1",
      {P4Replica("0x1", "Ethernet1", "0x0"),
       P4Replica("0x1", "Ethernet2", "0x0")},
      kGroupOid1, {kGroupMemberOid1, kGroupMemberOid2});
  auto group_entry2 =
      SetupP4MulticastGroupEntry("0x2", {P4Replica("0x2", "Ethernet1", "0x0")},
                                 kGroupOid2, {kGroupMemberOid3});

  // Unnaturally force RIFs to disappear.
  std::string rif_key = KeyGenerator::generateMulticastRouterInterfaceRifKey(
      "Ethernet1", swss::MacAddress(kSrcMac1));
  ForceRemoveRifKey(rif_key);

  // Attempt to delete.
  std::vector<P4MulticastGroupEntry> entries = {group_entry1, group_entry2};
  auto statuses = DeleteMulticastGroupEntries(entries);
  EXPECT_EQ(statuses.size(), 2);
  EXPECT_EQ(statuses[0], StatusCode::SWSS_RC_INTERNAL);
  EXPECT_EQ(statuses[1], StatusCode::SWSS_RC_NOT_EXECUTED);
}

TEST_F(L3MulticastManagerTest, DeleteMulticastGroupEntriesMissingGroupOid) {
  auto rif_entry1 = SetupP4MulticastRouterInterfaceEntry(
      "Ethernet1", "0x0", swss::MacAddress(kSrcMac1), kRifOid1);
  auto rif_entry2 = SetupP4MulticastRouterInterfaceEntry(
      "Ethernet2", "0x0", swss::MacAddress(kSrcMac2), kRifOid2);

  P4Replica replica1 = P4Replica("0x1", "Ethernet1", "0x0");
  P4Replica replica2 = P4Replica("0x1", "Ethernet2", "0x0");
  P4Replica replica3 = P4Replica("0x2", "Ethernet1", "0x0");

  auto group_entry1 =
      SetupP4MulticastGroupEntry("0x1", {replica1, replica2}, kGroupOid1,
                                 {kGroupMemberOid1, kGroupMemberOid2});
  auto group_entry2 = SetupP4MulticastGroupEntry("0x2", {replica3}, kGroupOid2,
                                                 {kGroupMemberOid3});

  // Unnaturally force multicast group disappear.
  p4_oid_mapper_.eraseOID(SAI_OBJECT_TYPE_IPMC_GROUP, "0x1");

  // Attempt to delete.
  std::vector<P4MulticastGroupEntry> entries = {group_entry1, group_entry2};
  auto statuses = DeleteMulticastGroupEntries(entries);
  EXPECT_EQ(statuses.size(), 2);
  EXPECT_EQ(statuses[0], StatusCode::SWSS_RC_INTERNAL);
  EXPECT_EQ(statuses[1], StatusCode::SWSS_RC_NOT_EXECUTED);
}

TEST_F(L3MulticastManagerTest, DeleteMulticastGroupEntriesNoGroupMembersFound) {
  auto rif_entry1 = SetupP4MulticastRouterInterfaceEntry(
      "Ethernet1", "0x0", swss::MacAddress(kSrcMac1), kRifOid1);
  auto rif_entry2 = SetupP4MulticastRouterInterfaceEntry(
      "Ethernet2", "0x0", swss::MacAddress(kSrcMac2), kRifOid2);

  P4Replica replica1 = P4Replica("0x1", "Ethernet1", "0x0");
  P4Replica replica2 = P4Replica("0x1", "Ethernet2", "0x0");
  P4Replica replica3 = P4Replica("0x2", "Ethernet1", "0x0");

  auto group_entry1 =
      SetupP4MulticastGroupEntry("0x1", {replica1, replica2}, kGroupOid1,
                                 {kGroupMemberOid1, kGroupMemberOid2});
  auto group_entry2 = SetupP4MulticastGroupEntry("0x2", {replica3}, kGroupOid2,
                                                 {kGroupMemberOid3});

  // Unnaturally force multicast group members to disappear.
  auto* actual_group_entry = GetMulticastGroupEntry("0x1");
  ASSERT_NE(actual_group_entry, nullptr);
  actual_group_entry->member_oids.erase(replica2.key);

  // Successfully remove first.
  EXPECT_CALL(mock_sai_ipmc_group_, remove_ipmc_group_member(kGroupMemberOid1))
      .WillOnce(Return(SAI_STATUS_SUCCESS));

  // Attempt to delete.
  std::vector<P4MulticastGroupEntry> entries = {group_entry1, group_entry2};
  auto statuses = DeleteMulticastGroupEntries(entries);
  EXPECT_EQ(statuses.size(), 2);
  EXPECT_EQ(statuses[0], StatusCode::SWSS_RC_INTERNAL);
  EXPECT_EQ(statuses[1], StatusCode::SWSS_RC_NOT_EXECUTED);
}

TEST_F(L3MulticastManagerTest,
       DeleteMulticastGroupEntriesDeleteMemberButNotGroupRestoreSucceeds) {
  auto rif_entry1 = SetupP4MulticastRouterInterfaceEntry(
      "Ethernet1", "0x0", swss::MacAddress(kSrcMac1), kRifOid1);
  auto rif_entry2 = SetupP4MulticastRouterInterfaceEntry(
      "Ethernet2", "0x0", swss::MacAddress(kSrcMac2), kRifOid2);

  P4Replica replica1 = P4Replica("0x1", "Ethernet1", "0x0");
  P4Replica replica2 = P4Replica("0x1", "Ethernet2", "0x0");
  P4Replica replica3 = P4Replica("0x2", "Ethernet1", "0x0");

  auto group_entry1 =
      SetupP4MulticastGroupEntry("0x1", {replica1, replica2}, kGroupOid1,
                                 {kGroupMemberOid1, kGroupMemberOid2});
  auto group_entry2 = SetupP4MulticastGroupEntry("0x2", {replica3}, kGroupOid2,
                                                 {kGroupMemberOid3});

  // Attempt to delete.
  EXPECT_CALL(mock_sai_ipmc_group_, remove_ipmc_group_member(kGroupMemberOid1))
      .WillOnce(Return(SAI_STATUS_SUCCESS));
  EXPECT_CALL(mock_sai_ipmc_group_, remove_ipmc_group_member(kGroupMemberOid2))
      .WillOnce(Return(SAI_STATUS_SUCCESS));
  EXPECT_CALL(mock_sai_ipmc_group_, remove_ipmc_group(kGroupOid1))
      .WillOnce(Return(SAI_STATUS_FAILURE));
  EXPECT_CALL(mock_sai_ipmc_group_, create_ipmc_group_member(_, _, _, _))
      .WillOnce(
          DoAll(SetArgPointee<0>(kGroupMemberOid1), Return(SAI_STATUS_SUCCESS)))
      .WillOnce(DoAll(SetArgPointee<0>(kGroupMemberOid2),
                      Return(SAI_STATUS_SUCCESS)));

  std::vector<P4MulticastGroupEntry> entries = {group_entry1, group_entry2};
  auto statuses = DeleteMulticastGroupEntries(entries);
  EXPECT_EQ(statuses.size(), 2);
  EXPECT_EQ(statuses[0], StatusCode::SWSS_RC_UNKNOWN);
  EXPECT_EQ(statuses[1], StatusCode::SWSS_RC_NOT_EXECUTED);
  EXPECT_TRUE(p4_oid_mapper_.existsOID(SAI_OBJECT_TYPE_IPMC_GROUP, "0x1"));
  // Since restored.
  EXPECT_TRUE(p4_oid_mapper_.existsOID(SAI_OBJECT_TYPE_IPMC_GROUP_MEMBER,
                                       replica1.key));
  EXPECT_TRUE(p4_oid_mapper_.existsOID(SAI_OBJECT_TYPE_IPMC_GROUP_MEMBER,
                                       replica2.key));
}

TEST_F(L3MulticastManagerTest,
       DeleteMulticastGroupEntriesDeleteMemberButNotGroupRestoreFails) {
  auto rif_entry1 = SetupP4MulticastRouterInterfaceEntry(
      "Ethernet1", "0x0", swss::MacAddress(kSrcMac1), kRifOid1);
  auto rif_entry2 = SetupP4MulticastRouterInterfaceEntry(
      "Ethernet2", "0x0", swss::MacAddress(kSrcMac2), kRifOid2);

  P4Replica replica1 = P4Replica("0x1", "Ethernet1", "0x0");
  P4Replica replica2 = P4Replica("0x1", "Ethernet2", "0x0");
  P4Replica replica3 = P4Replica("0x2", "Ethernet1", "0x0");

  auto group_entry1 =
      SetupP4MulticastGroupEntry("0x1", {replica1, replica2}, kGroupOid1,
                                 {kGroupMemberOid1, kGroupMemberOid2});
  auto group_entry2 = SetupP4MulticastGroupEntry("0x2", {replica3}, kGroupOid2,
                                                 {kGroupMemberOid3});

  // Attempt to delete.
  EXPECT_CALL(mock_sai_ipmc_group_, remove_ipmc_group_member(kGroupMemberOid1))
      .WillOnce(Return(SAI_STATUS_SUCCESS));
  EXPECT_CALL(mock_sai_ipmc_group_, remove_ipmc_group_member(kGroupMemberOid2))
      .WillOnce(Return(SAI_STATUS_SUCCESS));
  EXPECT_CALL(mock_sai_ipmc_group_, remove_ipmc_group(kGroupOid1))
      .WillOnce(Return(SAI_STATUS_FAILURE));
  EXPECT_CALL(mock_sai_ipmc_group_, create_ipmc_group_member(_, _, _, _))
      .WillOnce(DoAll(SetArgPointee<0>(kGroupMemberOid1),
                      Return(SAI_STATUS_SUCCESS)))
      .WillOnce(Return(SAI_STATUS_FAILURE));  // Last restore fails.

  // Expect to enter critical state.

  std::vector<P4MulticastGroupEntry> entries = {group_entry1, group_entry2};
  auto statuses = DeleteMulticastGroupEntries(entries);
  EXPECT_EQ(statuses.size(), 2);
  EXPECT_EQ(statuses[0], StatusCode::SWSS_RC_UNKNOWN);
  EXPECT_EQ(statuses[1], StatusCode::SWSS_RC_NOT_EXECUTED);
  EXPECT_TRUE(p4_oid_mapper_.existsOID(SAI_OBJECT_TYPE_IPMC_GROUP, "0x1"));
  EXPECT_TRUE(p4_oid_mapper_.existsOID(SAI_OBJECT_TYPE_IPMC_GROUP_MEMBER,
                                       replica1.key));
  EXPECT_FALSE(p4_oid_mapper_.existsOID(SAI_OBJECT_TYPE_IPMC_GROUP_MEMBER,
                                        replica2.key));
}

TEST_F(L3MulticastManagerTest,
       DeleteMulticastGroupEntriesWithActiveRouteEntriesFailure) {
  auto rif_entry1 = SetupP4MulticastRouterInterfaceEntry(
      "Ethernet1", "0x0", swss::MacAddress(kSrcMac1), kRifOid1);
  auto rif_entry2 = SetupP4MulticastRouterInterfaceEntry(
      "Ethernet2", "0x0", swss::MacAddress(kSrcMac2), kRifOid2);

  P4Replica replica1 = P4Replica("0x1", "Ethernet1", "0x0");
  P4Replica replica2 = P4Replica("0x1", "Ethernet2", "0x0");
  P4Replica replica3 = P4Replica("0x2", "Ethernet1", "0x0");

  auto group_entry1 =
      SetupP4MulticastGroupEntry("0x1", {replica1, replica2}, kGroupOid1,
                                 {kGroupMemberOid1, kGroupMemberOid2});
  auto group_entry2 = SetupP4MulticastGroupEntry("0x2", {replica3}, kGroupOid2,
                                                 {kGroupMemberOid3});

  // Register that Route Entries are using this multicast group.
  p4_oid_mapper_.increaseRefCount(SAI_OBJECT_TYPE_IPMC_GROUP, "0x1");

  // Attempt to delete.  Expect failure, since multicast group is referenced.
  std::vector<P4MulticastGroupEntry> entries = {group_entry1, group_entry2};
  auto statuses = DeleteMulticastGroupEntries(entries);
  EXPECT_EQ(statuses.size(), 2);
  EXPECT_EQ(statuses[0], StatusCode::SWSS_RC_IN_USE);
  EXPECT_EQ(statuses[1], StatusCode::SWSS_RC_NOT_EXECUTED);
  EXPECT_TRUE(p4_oid_mapper_.existsOID(SAI_OBJECT_TYPE_IPMC_GROUP, "0x1"));
}

TEST_F(L3MulticastManagerTest,
       DeleteMulticastGroupEntriesDeleteMemberAndGroupSuccess) {
  auto rif_entry1 = SetupP4MulticastRouterInterfaceEntry(
      "Ethernet1", "0x0", swss::MacAddress(kSrcMac1), kRifOid1);
  auto rif_entry2 = SetupP4MulticastRouterInterfaceEntry(
      "Ethernet2", "0x0", swss::MacAddress(kSrcMac2), kRifOid2);

  P4Replica replica1 = P4Replica("0x1", "Ethernet1", "0x0");
  P4Replica replica2 = P4Replica("0x1", "Ethernet2", "0x0");
  P4Replica replica3 = P4Replica("0x2", "Ethernet1", "0x0");

  auto group_entry1 =
      SetupP4MulticastGroupEntry("0x1", {replica1, replica2}, kGroupOid1,
                                 {kGroupMemberOid1, kGroupMemberOid2});
  auto group_entry2 = SetupP4MulticastGroupEntry("0x2", {replica3}, kGroupOid2,
                                                 {kGroupMemberOid3});

  // Attempt to delete.
  EXPECT_CALL(mock_sai_ipmc_group_, remove_ipmc_group_member(kGroupMemberOid1))
      .WillOnce(Return(SAI_STATUS_SUCCESS));
  EXPECT_CALL(mock_sai_ipmc_group_, remove_ipmc_group_member(kGroupMemberOid2))
      .WillOnce(Return(SAI_STATUS_SUCCESS));
  EXPECT_CALL(mock_sai_ipmc_group_, remove_ipmc_group_member(kGroupMemberOid3))
      .WillOnce(Return(SAI_STATUS_SUCCESS));
  EXPECT_CALL(mock_sai_ipmc_group_, remove_ipmc_group(kGroupOid1))
      .WillOnce(Return(SAI_STATUS_SUCCESS));
  EXPECT_CALL(mock_sai_ipmc_group_, remove_ipmc_group(kGroupOid2))
      .WillOnce(Return(SAI_STATUS_SUCCESS));

  std::vector<P4MulticastGroupEntry> entries = {group_entry1, group_entry2};
  auto statuses = DeleteMulticastGroupEntries(entries);
  EXPECT_EQ(statuses.size(), 2);
  EXPECT_TRUE(statuses[0].ok());
  EXPECT_TRUE(statuses[1].ok());
}

TEST_F(L3MulticastManagerTest,
       DeleteMulticastGroupEntriesDeleteGroupFailsReAddMemberSucceeds) {
  auto rif_entry1 = SetupP4MulticastRouterInterfaceEntry(
      "Ethernet1", "0x0", swss::MacAddress(kSrcMac1), kRifOid1);
  auto rif_entry2 = SetupP4MulticastRouterInterfaceEntry(
      "Ethernet2", "0x0", swss::MacAddress(kSrcMac2), kRifOid2);

  P4Replica replica1 = P4Replica("0x1", "Ethernet1", "0x0");
  P4Replica replica2 = P4Replica("0x1", "Ethernet2", "0x0");
  P4Replica replica3 = P4Replica("0x2", "Ethernet1", "0x0");

  auto group_entry1 =
      SetupP4MulticastGroupEntry("0x1", {replica1, replica2}, kGroupOid1,
                                 {kGroupMemberOid1, kGroupMemberOid2});
  auto group_entry2 = SetupP4MulticastGroupEntry("0x2", {replica3}, kGroupOid2,
                                                 {kGroupMemberOid3});

  // Attempt to delete.
  EXPECT_CALL(mock_sai_ipmc_group_, remove_ipmc_group_member(kGroupMemberOid1))
      .WillOnce(Return(SAI_STATUS_SUCCESS));
  EXPECT_CALL(mock_sai_ipmc_group_, remove_ipmc_group_member(kGroupMemberOid2))
      .WillOnce(Return(SAI_STATUS_FAILURE));
  EXPECT_CALL(mock_sai_ipmc_group_, create_ipmc_group_member(_, _, _, _))
      .WillOnce(DoAll(SetArgPointee<0>(kGroupMemberOid1),
                      Return(SAI_STATUS_SUCCESS)));

  std::vector<P4MulticastGroupEntry> entries = {group_entry1, group_entry2};
  auto statuses = DeleteMulticastGroupEntries(entries);
  EXPECT_EQ(statuses.size(), 2);
  EXPECT_EQ(statuses[0], StatusCode::SWSS_RC_UNKNOWN);
  EXPECT_EQ(statuses[1], StatusCode::SWSS_RC_NOT_EXECUTED);

  sai_object_id_t end_groupMemberOid = SAI_NULL_OBJECT_ID;
  p4_oid_mapper_.getOID(SAI_OBJECT_TYPE_IPMC_GROUP_MEMBER, replica1.key,
                        &end_groupMemberOid);
  EXPECT_EQ(end_groupMemberOid, kGroupMemberOid1);
}

TEST_F(L3MulticastManagerTest,
       DeleteMulticastGroupEntriesDeleteGroupFailsReAddMemberFails) {
  auto rif_entry1 = SetupP4MulticastRouterInterfaceEntry(
      "Ethernet1", "0x0", swss::MacAddress(kSrcMac1), kRifOid1);
  auto rif_entry2 = SetupP4MulticastRouterInterfaceEntry(
      "Ethernet2", "0x0", swss::MacAddress(kSrcMac2), kRifOid2);

  P4Replica replica1 = P4Replica("0x1", "Ethernet1", "0x0");
  P4Replica replica2 = P4Replica("0x1", "Ethernet2", "0x0");
  P4Replica replica3 = P4Replica("0x2", "Ethernet1", "0x0");

  auto group_entry1 =
      SetupP4MulticastGroupEntry("0x1", {replica1, replica2}, kGroupOid1,
                                 {kGroupMemberOid1, kGroupMemberOid2});
  auto group_entry2 = SetupP4MulticastGroupEntry("0x2", {replica3}, kGroupOid2,
                                                 {kGroupMemberOid3});

  // Attempt to delete.
  EXPECT_CALL(mock_sai_ipmc_group_, remove_ipmc_group_member(kGroupMemberOid1))
      .WillOnce(Return(SAI_STATUS_SUCCESS));
  EXPECT_CALL(mock_sai_ipmc_group_, remove_ipmc_group_member(kGroupMemberOid2))
      .WillOnce(Return(SAI_STATUS_FAILURE));
  EXPECT_CALL(mock_sai_ipmc_group_, create_ipmc_group_member(_, _, _, _))
      .WillOnce(Return(SAI_STATUS_FAILURE));
  // Expect to enter critical state.

  std::vector<P4MulticastGroupEntry> entries = {group_entry1, group_entry2};
  auto statuses = DeleteMulticastGroupEntries(entries);
  EXPECT_EQ(statuses.size(), 2);
  EXPECT_EQ(statuses[0], StatusCode::SWSS_RC_UNKNOWN);
  EXPECT_EQ(statuses[1], StatusCode::SWSS_RC_NOT_EXECUTED);

  EXPECT_TRUE(p4_oid_mapper_.existsOID(SAI_OBJECT_TYPE_IPMC_GROUP, "0x1"));
  EXPECT_FALSE(p4_oid_mapper_.existsOID(SAI_OBJECT_TYPE_IPMC_GROUP_MEMBER,
                                        replica1.key));
  EXPECT_TRUE(p4_oid_mapper_.existsOID(SAI_OBJECT_TYPE_IPMC_GROUP_MEMBER,
                                       replica2.key));  // since remove failed
}

TEST_F(L3MulticastManagerTest, UpdateMulticastGroupEntriesTestSuccess) {
  auto rif_entry1 = SetupP4MulticastRouterInterfaceEntry(
      "Ethernet1", "0x0", swss::MacAddress(kSrcMac1), kRifOid1);
  auto rif_entry2 = SetupP4MulticastRouterInterfaceEntry(
      "Ethernet2", "0x0", swss::MacAddress(kSrcMac2), kRifOid2);
  auto rif_entry3 = SetupP4MulticastRouterInterfaceEntry(
      "Ethernet3", "0x0", swss::MacAddress(kSrcMac3), kRifOid3);

  P4Replica replica1 = P4Replica("0x1", "Ethernet1", "0x0");
  P4Replica replica2 = P4Replica("0x1", "Ethernet2", "0x0");
  P4Replica replica3 = P4Replica("0x1", "Ethernet3", "0x0");

  auto group_entry1 =
      SetupP4MulticastGroupEntry("0x1", {replica1, replica2}, kGroupOid1,
                                 {kGroupMemberOid1, kGroupMemberOid2});

  // Should leave replica1 untouched, delete replica2, and add replica3.
  auto group_entry3 =
      GenerateP4MulticastGroupEntry("0x1", {replica1, replica3});

  EXPECT_CALL(mock_sai_ipmc_group_, remove_ipmc_group_member(kGroupMemberOid2))
      .WillOnce(Return(SAI_STATUS_SUCCESS));

  std::vector<sai_attribute_t> exp_member_attrs;
  sai_attribute_t attr;
  attr.id = SAI_IPMC_GROUP_MEMBER_ATTR_IPMC_GROUP_ID;
  attr.value.oid = kGroupOid1;
  exp_member_attrs.push_back(attr);
  attr.id = SAI_IPMC_GROUP_MEMBER_ATTR_IPMC_OUTPUT_ID;
  attr.value.oid = kRifOid3;
  exp_member_attrs.push_back(attr);
  EXPECT_CALL(mock_sai_ipmc_group_,
              create_ipmc_group_member(_, _, 2, AttrArrayEq(exp_member_attrs)))
      .WillOnce(DoAll(SetArgPointee<0>(kGroupMemberOid3),
                      Return(SAI_STATUS_SUCCESS)));

  std::vector<P4MulticastGroupEntry> entries = {group_entry3};
  auto statuses = UpdateMulticastGroupEntries(entries);

  EXPECT_EQ(statuses.size(), 1);
  EXPECT_EQ(statuses[0], StatusCode::SWSS_RC_SUCCESS);

  EXPECT_TRUE(p4_oid_mapper_.existsOID(SAI_OBJECT_TYPE_IPMC_GROUP, "0x1"));
  EXPECT_TRUE(p4_oid_mapper_.existsOID(SAI_OBJECT_TYPE_IPMC_GROUP_MEMBER,
                                       replica1.key));
  EXPECT_FALSE(p4_oid_mapper_.existsOID(SAI_OBJECT_TYPE_IPMC_GROUP_MEMBER,
                                        replica2.key));
  EXPECT_TRUE(p4_oid_mapper_.existsOID(SAI_OBJECT_TYPE_IPMC_GROUP_MEMBER,
                                       replica3.key));

  auto* group_entry_ptr = GetMulticastGroupEntry("0x1");
  ASSERT_NE(group_entry_ptr, nullptr);
  auto expect_entry =
      GenerateP4MulticastGroupEntry("0x1", {replica1, replica3});
  expect_entry.multicast_group_oid = kGroupOid1;
  expect_entry.member_oids[replica1.key] = kGroupMemberOid1;
  expect_entry.member_oids[replica3.key] = kGroupMemberOid3;
  VerifyP4MulticastGroupEntryEqual(expect_entry, *group_entry_ptr);
}

TEST_F(L3MulticastManagerTest, UpdateMulticastGroupEntriesNoEntryFound) {
  auto rif_entry1 = SetupP4MulticastRouterInterfaceEntry(
      "Ethernet1", "0x0", swss::MacAddress(kSrcMac1), kRifOid1);

  P4Replica replica1 = P4Replica("0x1", "Ethernet1", "0x0");
  P4Replica replica2 = P4Replica("0x2", "Ethernet1", "0x0");
  auto group_entry1 = GenerateP4MulticastGroupEntry("0x1", {replica1});
  auto group_entry2 = GenerateP4MulticastGroupEntry("0x2", {replica2});

  std::vector<P4MulticastGroupEntry> entries = {group_entry1, group_entry2};
  auto statuses = UpdateMulticastGroupEntries(entries);
  EXPECT_EQ(statuses.size(), 2);
  EXPECT_EQ(statuses[0], StatusCode::SWSS_RC_UNKNOWN);
  EXPECT_EQ(statuses[1], StatusCode::SWSS_RC_NOT_EXECUTED);
}

TEST_F(L3MulticastManagerTest, UpdateMulticastGroupEntriesMissingGroupOid) {
  auto rif_entry1 = SetupP4MulticastRouterInterfaceEntry(
      "Ethernet1", "0x0", swss::MacAddress(kSrcMac1), kRifOid1);
  auto rif_entry2 = SetupP4MulticastRouterInterfaceEntry(
      "Ethernet2", "0x0", swss::MacAddress(kSrcMac2), kRifOid2);
  auto rif_entry3 = SetupP4MulticastRouterInterfaceEntry(
      "Ethernet3", "0x0", swss::MacAddress(kSrcMac3), kRifOid3);

  P4Replica replica1 = P4Replica("0x1", "Ethernet1", "0x0");
  P4Replica replica2 = P4Replica("0x1", "Ethernet2", "0x0");
  P4Replica replica3 = P4Replica("0x1", "Ethernet3", "0x0");

  auto group_entry1 =
      SetupP4MulticastGroupEntry("0x1", {replica1, replica2}, kGroupOid1,
                                 {kGroupMemberOid1, kGroupMemberOid2});

  // Unnaturally delete multicast group OID to cause an error.
  p4_oid_mapper_.eraseOID(SAI_OBJECT_TYPE_IPMC_GROUP, "0x1");

  // Want to leave replica1 untouched, delete replica2, and add replica3.
  auto group_entry3 =
      GenerateP4MulticastGroupEntry("0x1", {replica1, replica3});

  // Expect to enter critical state.

  std::vector<P4MulticastGroupEntry> entries = {group_entry3, group_entry1};
  auto statuses = UpdateMulticastGroupEntries(entries);
  EXPECT_EQ(statuses.size(), 2);
  EXPECT_EQ(statuses[0], StatusCode::SWSS_RC_INTERNAL);
  EXPECT_EQ(statuses[1], StatusCode::SWSS_RC_NOT_EXECUTED);
}

TEST_F(L3MulticastManagerTest,
       UpdateMulticastGroupEntriesUpdateNoDiffMakesNoSaiCalls) {
  auto rif_entry1 = SetupP4MulticastRouterInterfaceEntry(
      "Ethernet1", "0x0", swss::MacAddress(kSrcMac1), kRifOid1);
  auto rif_entry2 = SetupP4MulticastRouterInterfaceEntry(
      "Ethernet2", "0x0", swss::MacAddress(kSrcMac2), kRifOid2);

  P4Replica replica1 = P4Replica("0x1", "Ethernet1", "0x0");
  P4Replica replica2 = P4Replica("0x1", "Ethernet2", "0x0");

  auto group_entry1 =
      SetupP4MulticastGroupEntry("0x1", {replica1, replica2}, kGroupOid1,
                                 {kGroupMemberOid1, kGroupMemberOid2});

  std::vector<P4MulticastGroupEntry> entries = {group_entry1};
  auto statuses = UpdateMulticastGroupEntries(entries);
  EXPECT_EQ(statuses.size(), 1);
  EXPECT_EQ(statuses[0], StatusCode::SWSS_RC_SUCCESS);
}

TEST_F(L3MulticastManagerTest, UpdateMulticastGroupEntriesMissingRifError) {
  auto rif_entry1 = SetupP4MulticastRouterInterfaceEntry(
      "Ethernet1", "0x0", swss::MacAddress(kSrcMac1), kRifOid1);
  auto rif_entry2 = SetupP4MulticastRouterInterfaceEntry(
      "Ethernet2", "0x0", swss::MacAddress(kSrcMac2), kRifOid2);
  auto rif_entry3 = SetupP4MulticastRouterInterfaceEntry(
      "Ethernet3", "0x0", swss::MacAddress(kSrcMac3), kRifOid3);

  P4Replica replica1 = P4Replica("0x1", "Ethernet1", "0x0");
  P4Replica replica2 = P4Replica("0x1", "Ethernet2", "0x0");
  P4Replica replica3 = P4Replica("0x1", "Ethernet3", "0x0");

  auto group_entry1 =
      SetupP4MulticastGroupEntry("0x1", {replica1, replica2}, kGroupOid1,
                                 {kGroupMemberOid1, kGroupMemberOid2});

  // Unnaturally force RIF for replica2 to disappear.
  std::string rif_key = KeyGenerator::generateMulticastRouterInterfaceRifKey(
      "Ethernet2", swss::MacAddress(kSrcMac2));
  ForceRemoveRifKey(rif_key);

  // Want to leave replica1 untouched, delete replica2, and add replica3.
  auto group_entry2 =
      GenerateP4MulticastGroupEntry("0x1", {replica1, replica3});

  // Expect to enter critical state.

  std::vector<P4MulticastGroupEntry> entries = {group_entry2};
  auto statuses = UpdateMulticastGroupEntries(entries);
  EXPECT_EQ(statuses.size(), 1);
  EXPECT_EQ(statuses[0], StatusCode::SWSS_RC_INTERNAL);

  EXPECT_TRUE(p4_oid_mapper_.existsOID(SAI_OBJECT_TYPE_IPMC_GROUP, "0x1"));
  EXPECT_TRUE(p4_oid_mapper_.existsOID(SAI_OBJECT_TYPE_IPMC_GROUP_MEMBER,
                                       replica1.key));
  EXPECT_TRUE(p4_oid_mapper_.existsOID(SAI_OBJECT_TYPE_IPMC_GROUP_MEMBER,
                                       replica2.key));
  EXPECT_FALSE(p4_oid_mapper_.existsOID(SAI_OBJECT_TYPE_IPMC_GROUP_MEMBER,
                                        replica3.key));
}

TEST_F(L3MulticastManagerTest,
       UpdateMulticastGroupEntriesMemberDeleteFailsRestoreSucceeds) {
  auto rif_entry1 = SetupP4MulticastRouterInterfaceEntry(
      "Ethernet1", "0x0", swss::MacAddress(kSrcMac1), kRifOid1);
  auto rif_entry2 = SetupP4MulticastRouterInterfaceEntry(
      "Ethernet2", "0x0", swss::MacAddress(kSrcMac2), kRifOid2);
  auto rif_entry3 = SetupP4MulticastRouterInterfaceEntry(
      "Ethernet3", "0x0", swss::MacAddress(kSrcMac3), kRifOid3);

  P4Replica replica1 = P4Replica("0x1", "Ethernet1", "0x0");
  P4Replica replica2 = P4Replica("0x1", "Ethernet2", "0x0");
  P4Replica replica3 = P4Replica("0x1", "Ethernet3", "0x0");

  auto group_entry1 =
      SetupP4MulticastGroupEntry("0x1", {replica1, replica2}, kGroupOid1,
                                 {kGroupMemberOid1, kGroupMemberOid2});

  // Want to delete replicas 1 and 2, and add replica3.
  auto group_entry2 = GenerateP4MulticastGroupEntry("0x1", {replica3});

  EXPECT_CALL(mock_sai_ipmc_group_, remove_ipmc_group_member(kGroupMemberOid1))
      .WillOnce(Return(SAI_STATUS_SUCCESS));
  EXPECT_CALL(mock_sai_ipmc_group_, remove_ipmc_group_member(kGroupMemberOid2))
      .WillOnce(Return(SAI_STATUS_FAILURE));

  std::vector<sai_attribute_t> exp_member_attrs;
  sai_attribute_t attr;
  attr.id = SAI_IPMC_GROUP_MEMBER_ATTR_IPMC_GROUP_ID;
  attr.value.oid = kGroupOid1;
  exp_member_attrs.push_back(attr);
  attr.id = SAI_IPMC_GROUP_MEMBER_ATTR_IPMC_OUTPUT_ID;
  attr.value.oid = kRifOid1;
  exp_member_attrs.push_back(attr);
  EXPECT_CALL(mock_sai_ipmc_group_,
              create_ipmc_group_member(_, _, 2, AttrArrayEq(exp_member_attrs)))
      .WillOnce(DoAll(SetArgPointee<0>(kGroupMemberOid1),
                      Return(SAI_STATUS_SUCCESS)));

  std::vector<P4MulticastGroupEntry> entries = {group_entry2};
  auto statuses = UpdateMulticastGroupEntries(entries);
  EXPECT_EQ(statuses.size(), 1);
  EXPECT_EQ(statuses[0], StatusCode::SWSS_RC_UNKNOWN);

  EXPECT_TRUE(p4_oid_mapper_.existsOID(SAI_OBJECT_TYPE_IPMC_GROUP, "0x1"));
  EXPECT_TRUE(p4_oid_mapper_.existsOID(SAI_OBJECT_TYPE_IPMC_GROUP_MEMBER,
                                       replica1.key));
  EXPECT_TRUE(p4_oid_mapper_.existsOID(SAI_OBJECT_TYPE_IPMC_GROUP_MEMBER,
                                       replica2.key));
  EXPECT_FALSE(p4_oid_mapper_.existsOID(SAI_OBJECT_TYPE_IPMC_GROUP_MEMBER,
                                        replica3.key));

  auto* group_entry_ptr = GetMulticastGroupEntry("0x1");
  ASSERT_NE(group_entry_ptr, nullptr);
  auto expect_entry =
      GenerateP4MulticastGroupEntry("0x1", {replica1, replica2});
  expect_entry.multicast_group_oid = kGroupOid1;
  expect_entry.member_oids[replica1.key] = kGroupMemberOid1;
  expect_entry.member_oids[replica2.key] = kGroupMemberOid2;
  VerifyP4MulticastGroupEntryEqual(expect_entry, *group_entry_ptr);
}

TEST_F(L3MulticastManagerTest,
       UpdateMulticastGroupEntriesMemberDeleteFailsRestoreFails) {
  auto rif_entry1 = SetupP4MulticastRouterInterfaceEntry(
      "Ethernet1", "0x0", swss::MacAddress(kSrcMac1), kRifOid1);
  auto rif_entry2 = SetupP4MulticastRouterInterfaceEntry(
      "Ethernet2", "0x0", swss::MacAddress(kSrcMac2), kRifOid2);
  auto rif_entry3 = SetupP4MulticastRouterInterfaceEntry(
      "Ethernet3", "0x0", swss::MacAddress(kSrcMac3), kRifOid3);

  P4Replica replica1 = P4Replica("0x1", "Ethernet1", "0x0");
  P4Replica replica2 = P4Replica("0x1", "Ethernet2", "0x0");
  P4Replica replica3 = P4Replica("0x1", "Ethernet3", "0x0");

  auto group_entry1 =
      SetupP4MulticastGroupEntry("0x1", {replica1, replica2}, kGroupOid1,
                                 {kGroupMemberOid1, kGroupMemberOid2});

  // Want to delete replicas 1 and 2, and add replica3.
  auto group_entry2 = GenerateP4MulticastGroupEntry("0x1", {replica3});

  EXPECT_CALL(mock_sai_ipmc_group_, remove_ipmc_group_member(kGroupMemberOid1))
      .WillOnce(Return(SAI_STATUS_SUCCESS));
  EXPECT_CALL(mock_sai_ipmc_group_, remove_ipmc_group_member(kGroupMemberOid2))
      .WillOnce(Return(SAI_STATUS_FAILURE));

  std::vector<sai_attribute_t> exp_member_attrs;
  sai_attribute_t attr;
  attr.id = SAI_IPMC_GROUP_MEMBER_ATTR_IPMC_GROUP_ID;
  attr.value.oid = kGroupOid1;
  exp_member_attrs.push_back(attr);
  attr.id = SAI_IPMC_GROUP_MEMBER_ATTR_IPMC_OUTPUT_ID;
  attr.value.oid = kRifOid1;
  exp_member_attrs.push_back(attr);
  EXPECT_CALL(mock_sai_ipmc_group_,
              create_ipmc_group_member(_, _, 2, AttrArrayEq(exp_member_attrs)))
      .WillOnce(Return(SAI_STATUS_FAILURE));

  // Expect to enter critical state.

  std::vector<P4MulticastGroupEntry> entries = {group_entry2};
  auto statuses = UpdateMulticastGroupEntries(entries);
  EXPECT_EQ(statuses.size(), 1);
  EXPECT_EQ(statuses[0], StatusCode::SWSS_RC_UNKNOWN);

  EXPECT_TRUE(p4_oid_mapper_.existsOID(SAI_OBJECT_TYPE_IPMC_GROUP, "0x1"));
  EXPECT_FALSE(p4_oid_mapper_.existsOID(SAI_OBJECT_TYPE_IPMC_GROUP_MEMBER,
                                        replica1.key));
  EXPECT_TRUE(p4_oid_mapper_.existsOID(SAI_OBJECT_TYPE_IPMC_GROUP_MEMBER,
                                       replica2.key));
  EXPECT_FALSE(p4_oid_mapper_.existsOID(SAI_OBJECT_TYPE_IPMC_GROUP_MEMBER,
                                        replica3.key));
}

TEST_F(L3MulticastManagerTest,
       UpdateMulticastGroupEntriesMemberAddCannotFindRif) {
  auto rif_entry1 = SetupP4MulticastRouterInterfaceEntry(
      "Ethernet1", "0x0", swss::MacAddress(kSrcMac1), kRifOid1);
  auto rif_entry2 = SetupP4MulticastRouterInterfaceEntry(
      "Ethernet2", "0x0", swss::MacAddress(kSrcMac2), kRifOid2);
  auto rif_entry3 = SetupP4MulticastRouterInterfaceEntry(
      "Ethernet3", "0x0", swss::MacAddress(kSrcMac3), kRifOid3);

  P4Replica replica1 = P4Replica("0x1", "Ethernet1", "0x0");
  P4Replica replica2 = P4Replica("0x1", "Ethernet2", "0x0");
  P4Replica replica3 = P4Replica("0x1", "Ethernet3", "0x0");

  auto group_entry1 =
      SetupP4MulticastGroupEntry("0x1", {replica1, replica2}, kGroupOid1,
                                 {kGroupMemberOid1, kGroupMemberOid2});

  // Want to add replica3.
  auto group_entry2 =
      GenerateP4MulticastGroupEntry("0x1", {replica1, replica2, replica3});

  // Unnaturally force RIFs to disappear.
  std::string rif_key = KeyGenerator::generateMulticastRouterInterfaceRifKey(
      "Ethernet3", swss::MacAddress(kSrcMac3));
  ForceRemoveRifKey(rif_key);

  // Expect to enter critical state.

  std::vector<P4MulticastGroupEntry> entries = {group_entry2};
  auto statuses = UpdateMulticastGroupEntries(entries);
  EXPECT_EQ(statuses.size(), 1);
  EXPECT_EQ(statuses[0], StatusCode::SWSS_RC_INTERNAL);

  EXPECT_TRUE(p4_oid_mapper_.existsOID(SAI_OBJECT_TYPE_IPMC_GROUP, "0x1"));
  EXPECT_TRUE(p4_oid_mapper_.existsOID(SAI_OBJECT_TYPE_IPMC_GROUP_MEMBER,
                                       replica1.key));
  EXPECT_TRUE(p4_oid_mapper_.existsOID(SAI_OBJECT_TYPE_IPMC_GROUP_MEMBER,
                                       replica2.key));
  EXPECT_FALSE(p4_oid_mapper_.existsOID(SAI_OBJECT_TYPE_IPMC_GROUP_MEMBER,
                                        replica3.key));
}

TEST_F(L3MulticastManagerTest,
       UpdateMulticastGroupEntriesMemberAddMemberFailsBackoutSucceeds) {
  auto rif_entry1 = SetupP4MulticastRouterInterfaceEntry(
      "Ethernet1", "0x0", swss::MacAddress(kSrcMac1), kRifOid1);
  auto rif_entry2 = SetupP4MulticastRouterInterfaceEntry(
      "Ethernet2", "0x0", swss::MacAddress(kSrcMac2), kRifOid2);
  auto rif_entry3 = SetupP4MulticastRouterInterfaceEntry(
      "Ethernet3", "0x0", swss::MacAddress(kSrcMac3), kRifOid3);
  auto rif_entry4 = SetupP4MulticastRouterInterfaceEntry(
      "Ethernet4", "0x0", swss::MacAddress(kSrcMac4), kRifOid4);

  P4Replica replica1 = P4Replica("0x1", "Ethernet1", "0x0");
  P4Replica replica2 = P4Replica("0x1", "Ethernet2", "0x0");
  P4Replica replica3 = P4Replica("0x1", "Ethernet3", "0x0");
  P4Replica replica4 = P4Replica("0x1", "Ethernet4", "0x0");

  auto group_entry1 =
      SetupP4MulticastGroupEntry("0x1", {replica1, replica2}, kGroupOid1,
                                 {kGroupMemberOid1, kGroupMemberOid2});

  // Want to delete replica1 and replica2.  Want to add replica3 and replica4.
  auto group_entry2 =
      GenerateP4MulticastGroupEntry("0x1", {replica3, replica4});

  // Remove replica1 and replica2.
  EXPECT_CALL(mock_sai_ipmc_group_, remove_ipmc_group_member(kGroupMemberOid1))
      .WillOnce(Return(SAI_STATUS_SUCCESS));
  EXPECT_CALL(mock_sai_ipmc_group_, remove_ipmc_group_member(kGroupMemberOid2))
      .WillOnce(Return(SAI_STATUS_SUCCESS));

  // Add replica3.
  std::vector<sai_attribute_t> exp_member_attrs;
  sai_attribute_t attr;
  attr.id = SAI_IPMC_GROUP_MEMBER_ATTR_IPMC_GROUP_ID;
  attr.value.oid = kGroupOid1;
  exp_member_attrs.push_back(attr);
  attr.id = SAI_IPMC_GROUP_MEMBER_ATTR_IPMC_OUTPUT_ID;
  attr.value.oid = kRifOid3;
  exp_member_attrs.push_back(attr);
  EXPECT_CALL(mock_sai_ipmc_group_,
              create_ipmc_group_member(_, _, 2, AttrArrayEq(exp_member_attrs)))
      .WillOnce(DoAll(SetArgPointee<0>(kGroupMemberOid3),
                      Return(SAI_STATUS_SUCCESS)));
  // Try to add replica4, but it fails.
  std::vector<sai_attribute_t> exp_member_attrs2;
  attr.id = SAI_IPMC_GROUP_MEMBER_ATTR_IPMC_GROUP_ID;
  attr.value.oid = kGroupOid1;
  exp_member_attrs2.push_back(attr);
  attr.id = SAI_IPMC_GROUP_MEMBER_ATTR_IPMC_OUTPUT_ID;
  attr.value.oid = kRifOid4;
  exp_member_attrs2.push_back(attr);
  EXPECT_CALL(mock_sai_ipmc_group_,
              create_ipmc_group_member(_, _, 2, AttrArrayEq(exp_member_attrs2)))
      .WillOnce(Return(SAI_STATUS_FAILURE));
  // Remove replica3.
  EXPECT_CALL(mock_sai_ipmc_group_, remove_ipmc_group_member(kGroupMemberOid3))
      .WillOnce(Return(SAI_STATUS_SUCCESS));
  // Add replica1 and replica2 back.
  std::vector<sai_attribute_t> exp_member_attrs3;
  attr.id = SAI_IPMC_GROUP_MEMBER_ATTR_IPMC_GROUP_ID;
  attr.value.oid = kGroupOid1;
  exp_member_attrs3.push_back(attr);
  attr.id = SAI_IPMC_GROUP_MEMBER_ATTR_IPMC_OUTPUT_ID;
  attr.value.oid = kRifOid1;
  exp_member_attrs3.push_back(attr);
  EXPECT_CALL(mock_sai_ipmc_group_,
              create_ipmc_group_member(_, _, 2, AttrArrayEq(exp_member_attrs3)))
      .WillOnce(DoAll(SetArgPointee<0>(kGroupMemberOid1),
                      Return(SAI_STATUS_SUCCESS)));
  std::vector<sai_attribute_t> exp_member_attrs4;
  attr.id = SAI_IPMC_GROUP_MEMBER_ATTR_IPMC_GROUP_ID;
  attr.value.oid = kGroupOid1;
  exp_member_attrs4.push_back(attr);
  attr.id = SAI_IPMC_GROUP_MEMBER_ATTR_IPMC_OUTPUT_ID;
  attr.value.oid = kRifOid2;
  exp_member_attrs4.push_back(attr);
  EXPECT_CALL(mock_sai_ipmc_group_,
              create_ipmc_group_member(_, _, 2, AttrArrayEq(exp_member_attrs4)))
      .WillOnce(DoAll(SetArgPointee<0>(kGroupMemberOid2),
                      Return(SAI_STATUS_SUCCESS)));

  std::vector<P4MulticastGroupEntry> entries = {group_entry2};
  auto statuses = UpdateMulticastGroupEntries(entries);
  EXPECT_EQ(statuses.size(), 1);
  EXPECT_EQ(statuses[0], StatusCode::SWSS_RC_UNKNOWN);

  EXPECT_TRUE(p4_oid_mapper_.existsOID(SAI_OBJECT_TYPE_IPMC_GROUP, "0x1"));
  EXPECT_TRUE(p4_oid_mapper_.existsOID(SAI_OBJECT_TYPE_IPMC_GROUP_MEMBER,
                                       replica1.key));
  EXPECT_TRUE(p4_oid_mapper_.existsOID(SAI_OBJECT_TYPE_IPMC_GROUP_MEMBER,
                                       replica2.key));
  EXPECT_FALSE(p4_oid_mapper_.existsOID(SAI_OBJECT_TYPE_IPMC_GROUP_MEMBER,
                                        replica3.key));
  EXPECT_FALSE(p4_oid_mapper_.existsOID(SAI_OBJECT_TYPE_IPMC_GROUP_MEMBER,
                                        replica4.key));

  auto* group_entry_ptr = GetMulticastGroupEntry("0x1");
  ASSERT_NE(group_entry_ptr, nullptr);
  auto expect_entry =
      GenerateP4MulticastGroupEntry("0x1", {replica1, replica2});
  expect_entry.multicast_group_oid = kGroupOid1;
  expect_entry.member_oids[replica1.key] = kGroupMemberOid1;
  expect_entry.member_oids[replica2.key] = kGroupMemberOid2;
  VerifyP4MulticastGroupEntryEqual(expect_entry, *group_entry_ptr);
}

TEST_F(L3MulticastManagerTest,
       UpdateMulticastGroupEntriesMemberAddMemberFailsBackoutDeleteFails) {
  auto rif_entry1 = SetupP4MulticastRouterInterfaceEntry(
      "Ethernet1", "0x0", swss::MacAddress(kSrcMac1), kRifOid1);
  auto rif_entry2 = SetupP4MulticastRouterInterfaceEntry(
      "Ethernet2", "0x0", swss::MacAddress(kSrcMac2), kRifOid2);
  auto rif_entry3 = SetupP4MulticastRouterInterfaceEntry(
      "Ethernet3", "0x0", swss::MacAddress(kSrcMac3), kRifOid3);
  auto rif_entry4 = SetupP4MulticastRouterInterfaceEntry(
      "Ethernet4", "0x0", swss::MacAddress(kSrcMac4), kRifOid4);

  P4Replica replica1 = P4Replica("0x1", "Ethernet1", "0x0");
  P4Replica replica2 = P4Replica("0x1", "Ethernet2", "0x0");
  P4Replica replica3 = P4Replica("0x1", "Ethernet3", "0x0");
  P4Replica replica4 = P4Replica("0x1", "Ethernet4", "0x0");

  auto group_entry1 =
      SetupP4MulticastGroupEntry("0x1", {replica1, replica2}, kGroupOid1,
                                 {kGroupMemberOid1, kGroupMemberOid2});

  // Want to delete replica1 and replica2.  Want to add replica3 and replica4.
  auto group_entry2 =
      GenerateP4MulticastGroupEntry("0x1", {replica3, replica4});

  // Remove replica1 and replica2.
  EXPECT_CALL(mock_sai_ipmc_group_, remove_ipmc_group_member(kGroupMemberOid1))
      .WillOnce(Return(SAI_STATUS_SUCCESS));
  EXPECT_CALL(mock_sai_ipmc_group_, remove_ipmc_group_member(kGroupMemberOid2))
      .WillOnce(Return(SAI_STATUS_SUCCESS));

  // Add replica3.
  std::vector<sai_attribute_t> exp_member_attrs;
  sai_attribute_t attr;
  attr.id = SAI_IPMC_GROUP_MEMBER_ATTR_IPMC_GROUP_ID;
  attr.value.oid = kGroupOid1;
  exp_member_attrs.push_back(attr);
  attr.id = SAI_IPMC_GROUP_MEMBER_ATTR_IPMC_OUTPUT_ID;
  attr.value.oid = kRifOid3;
  exp_member_attrs.push_back(attr);
  EXPECT_CALL(mock_sai_ipmc_group_,
              create_ipmc_group_member(_, _, 2, AttrArrayEq(exp_member_attrs)))
      .WillOnce(DoAll(SetArgPointee<0>(kGroupMemberOid3),
                      Return(SAI_STATUS_SUCCESS)));
  // Try to add replica4, but it fails.
  std::vector<sai_attribute_t> exp_member_attrs2;
  attr.id = SAI_IPMC_GROUP_MEMBER_ATTR_IPMC_GROUP_ID;
  attr.value.oid = kGroupOid1;
  exp_member_attrs2.push_back(attr);
  attr.id = SAI_IPMC_GROUP_MEMBER_ATTR_IPMC_OUTPUT_ID;
  attr.value.oid = kRifOid4;
  exp_member_attrs2.push_back(attr);
  EXPECT_CALL(mock_sai_ipmc_group_,
              create_ipmc_group_member(_, _, 2, AttrArrayEq(exp_member_attrs2)))
      .WillOnce(Return(SAI_STATUS_FAILURE));
  // Remove replica3, but it fails.
  EXPECT_CALL(mock_sai_ipmc_group_, remove_ipmc_group_member(kGroupMemberOid3))
      .WillOnce(Return(SAI_STATUS_FAILURE));

  // Expect to enter critical state.

  std::vector<P4MulticastGroupEntry> entries = {group_entry2};
  auto statuses = UpdateMulticastGroupEntries(entries);
  EXPECT_EQ(statuses.size(), 1);
  EXPECT_EQ(statuses[0], StatusCode::SWSS_RC_UNKNOWN);

  EXPECT_TRUE(p4_oid_mapper_.existsOID(SAI_OBJECT_TYPE_IPMC_GROUP, "0x1"));
  EXPECT_FALSE(p4_oid_mapper_.existsOID(SAI_OBJECT_TYPE_IPMC_GROUP_MEMBER,
                                        replica1.key));
  EXPECT_FALSE(p4_oid_mapper_.existsOID(SAI_OBJECT_TYPE_IPMC_GROUP_MEMBER,
                                        replica2.key));
  EXPECT_TRUE(p4_oid_mapper_.existsOID(SAI_OBJECT_TYPE_IPMC_GROUP_MEMBER,
                                       replica3.key));
  EXPECT_FALSE(p4_oid_mapper_.existsOID(SAI_OBJECT_TYPE_IPMC_GROUP_MEMBER,
                                        replica4.key));
}

TEST_F(L3MulticastManagerTest,
       UpdateMulticastGroupEntriesMemberAddMemberFailsBackoutAddFails) {
  auto rif_entry1 = SetupP4MulticastRouterInterfaceEntry(
      "Ethernet1", "0x0", swss::MacAddress(kSrcMac1), kRifOid1);
  auto rif_entry2 = SetupP4MulticastRouterInterfaceEntry(
      "Ethernet2", "0x0", swss::MacAddress(kSrcMac2), kRifOid2);
  auto rif_entry3 = SetupP4MulticastRouterInterfaceEntry(
      "Ethernet3", "0x0", swss::MacAddress(kSrcMac3), kRifOid3);
  auto rif_entry4 = SetupP4MulticastRouterInterfaceEntry(
      "Ethernet4", "0x0", swss::MacAddress(kSrcMac4), kRifOid4);

  P4Replica replica1 = P4Replica("0x1", "Ethernet1", "0x0");
  P4Replica replica2 = P4Replica("0x1", "Ethernet2", "0x0");
  P4Replica replica3 = P4Replica("0x1", "Ethernet3", "0x0");
  P4Replica replica4 = P4Replica("0x1", "Ethernet4", "0x0");

  auto group_entry1 =
      SetupP4MulticastGroupEntry("0x1", {replica1, replica2}, kGroupOid1,
                                 {kGroupMemberOid1, kGroupMemberOid2});

  // Want to delete replica1 and replica2.  Want to add replica3 and replica4.
  auto group_entry2 =
      GenerateP4MulticastGroupEntry("0x1", {replica3, replica4});

  // Remove replica1 and replica2.
  EXPECT_CALL(mock_sai_ipmc_group_, remove_ipmc_group_member(kGroupMemberOid1))
      .WillOnce(Return(SAI_STATUS_SUCCESS));
  EXPECT_CALL(mock_sai_ipmc_group_, remove_ipmc_group_member(kGroupMemberOid2))
      .WillOnce(Return(SAI_STATUS_SUCCESS));

  // Add replica3.
  std::vector<sai_attribute_t> exp_member_attrs;
  sai_attribute_t attr;
  attr.id = SAI_IPMC_GROUP_MEMBER_ATTR_IPMC_GROUP_ID;
  attr.value.oid = kGroupOid1;
  exp_member_attrs.push_back(attr);
  attr.id = SAI_IPMC_GROUP_MEMBER_ATTR_IPMC_OUTPUT_ID;
  attr.value.oid = kRifOid3;
  exp_member_attrs.push_back(attr);
  EXPECT_CALL(mock_sai_ipmc_group_,
              create_ipmc_group_member(_, _, 2, AttrArrayEq(exp_member_attrs)))
      .WillOnce(DoAll(SetArgPointee<0>(kGroupMemberOid3),
                      Return(SAI_STATUS_SUCCESS)));
  // Try to add replica4, but it fails.
  std::vector<sai_attribute_t> exp_member_attrs2;
  attr.id = SAI_IPMC_GROUP_MEMBER_ATTR_IPMC_GROUP_ID;
  attr.value.oid = kGroupOid1;
  exp_member_attrs2.push_back(attr);
  attr.id = SAI_IPMC_GROUP_MEMBER_ATTR_IPMC_OUTPUT_ID;
  attr.value.oid = kRifOid4;
  exp_member_attrs2.push_back(attr);
  EXPECT_CALL(mock_sai_ipmc_group_,
              create_ipmc_group_member(_, _, 2, AttrArrayEq(exp_member_attrs2)))
      .WillOnce(Return(SAI_STATUS_FAILURE));
  // Remove replica3.
  EXPECT_CALL(mock_sai_ipmc_group_, remove_ipmc_group_member(kGroupMemberOid3))
      .WillOnce(Return(SAI_STATUS_SUCCESS));

  // Try to add replica1 back.
  std::vector<sai_attribute_t> exp_member_attrs3;
  attr.id = SAI_IPMC_GROUP_MEMBER_ATTR_IPMC_GROUP_ID;
  attr.value.oid = kGroupOid1;
  exp_member_attrs3.push_back(attr);
  attr.id = SAI_IPMC_GROUP_MEMBER_ATTR_IPMC_OUTPUT_ID;
  attr.value.oid = kRifOid1;
  exp_member_attrs3.push_back(attr);
  EXPECT_CALL(mock_sai_ipmc_group_,
              create_ipmc_group_member(_, _, 2, AttrArrayEq(exp_member_attrs3)))
      .WillOnce(DoAll(SetArgPointee<0>(kGroupMemberOid1),
                      Return(SAI_STATUS_SUCCESS)));

  // Try to add replica2 back, but it fails.
  std::vector<sai_attribute_t> exp_member_attrs4;
  attr.id = SAI_IPMC_GROUP_MEMBER_ATTR_IPMC_GROUP_ID;
  attr.value.oid = kGroupOid1;
  exp_member_attrs4.push_back(attr);
  attr.id = SAI_IPMC_GROUP_MEMBER_ATTR_IPMC_OUTPUT_ID;
  attr.value.oid = kRifOid2;
  exp_member_attrs4.push_back(attr);
  EXPECT_CALL(mock_sai_ipmc_group_,
              create_ipmc_group_member(_, _, 2, AttrArrayEq(exp_member_attrs4)))
      .WillOnce(Return(SAI_STATUS_FAILURE));

  // Expect to enter critical state.

  std::vector<P4MulticastGroupEntry> entries = {group_entry2};
  auto statuses = UpdateMulticastGroupEntries(entries);
  EXPECT_EQ(statuses.size(), 1);
  EXPECT_EQ(statuses[0], StatusCode::SWSS_RC_UNKNOWN);

  EXPECT_TRUE(p4_oid_mapper_.existsOID(SAI_OBJECT_TYPE_IPMC_GROUP, "0x1"));
  EXPECT_TRUE(p4_oid_mapper_.existsOID(SAI_OBJECT_TYPE_IPMC_GROUP_MEMBER,
                                       replica1.key));
  EXPECT_FALSE(p4_oid_mapper_.existsOID(SAI_OBJECT_TYPE_IPMC_GROUP_MEMBER,
                                        replica2.key));
  EXPECT_FALSE(p4_oid_mapper_.existsOID(SAI_OBJECT_TYPE_IPMC_GROUP_MEMBER,
                                        replica3.key));
  EXPECT_FALSE(p4_oid_mapper_.existsOID(SAI_OBJECT_TYPE_IPMC_GROUP_MEMBER,
                                        replica4.key));
}

TEST_F(L3MulticastManagerTest, ValidateSetMulticastGroupEntryAddSuccessTest) {
  // Add router interface entries so have RIF.
  auto rif_entry = SetupP4MulticastRouterInterfaceEntry(
      "Ethernet1", "0x0", swss::MacAddress(kSrcMac1), kRifOid1);

  auto entry = GenerateP4MulticastGroupEntry(
      "0x1", {P4Replica("0x1", "Ethernet1", "0x0")});
  ReturnCode status = ValidateMulticastGroupEntry(entry, SET_COMMAND);
  EXPECT_TRUE(status.ok());
}

TEST_F(L3MulticastManagerTest,
       ValidateSetMulticastGroupEntryEmptyMulticastGroupTest) {
  // Add router interface entries so have RIF.
  auto rif_entry = SetupP4MulticastRouterInterfaceEntry(
      "Ethernet1", "0x0", swss::MacAddress(kSrcMac1), kRifOid1);

  auto entry =
      GenerateP4MulticastGroupEntry("", {P4Replica("0x1", "Ethernet1", "0x0")});
  ReturnCode status = ValidateMulticastGroupEntry(entry, SET_COMMAND);
  EXPECT_EQ(StatusCode::SWSS_RC_INVALID_PARAM, status);
}

TEST_F(L3MulticastManagerTest, ValidateSetMulticastGroupEntryNoRifTest) {
  // No RIF
  auto entry = GenerateP4MulticastGroupEntry(
      "0x1", {P4Replica("0x1", "Ethernet1", "0x0")});
  ReturnCode status = ValidateMulticastGroupEntry(entry, SET_COMMAND);
  EXPECT_EQ(StatusCode::SWSS_RC_NOT_FOUND, status);
}

TEST_F(L3MulticastManagerTest, ValidateSetMulticastGroupEntryUpdateTest) {
  // Add router interface entries so have RIF.
  auto rif_entry1 = SetupP4MulticastRouterInterfaceEntry(
      "Ethernet1", "0x0", swss::MacAddress(kSrcMac1), kRifOid1);
  auto rif_entry2 = SetupP4MulticastRouterInterfaceEntry(
      "Ethernet2", "0x0", swss::MacAddress(kSrcMac2), kRifOid2);
  // Setup multicast group entry.
  auto group_entry1 = SetupP4MulticastGroupEntry(
      "0x1",
      {P4Replica("0x1", "Ethernet1", "0x0"),
       P4Replica("0x1", "Ethernet2", "0x0")},
      kGroupOid1, {kGroupMemberOid1, kGroupMemberOid2});
  // Validate an existing multicast entry.
  ReturnCode status = ValidateMulticastGroupEntry(group_entry1, SET_COMMAND);
  EXPECT_TRUE(status.ok());
}

TEST_F(L3MulticastManagerTest,
       ValidateSetMulticastGroupEntryUpdateNoGroupOidInEntryTest) {
  // Add router interface entries so have RIF.
  auto rif_entry1 = SetupP4MulticastRouterInterfaceEntry(
      "Ethernet1", "0x0", swss::MacAddress(kSrcMac1), kRifOid1);
  auto rif_entry2 = SetupP4MulticastRouterInterfaceEntry(
      "Ethernet2", "0x0", swss::MacAddress(kSrcMac2), kRifOid2);
  // Setup multicast group entries.
  auto group_entry1 = SetupP4MulticastGroupEntry(
      "0x1",
      {P4Replica("0x1", "Ethernet1", "0x0"),
       P4Replica("0x1", "Ethernet2", "0x0")},
      kGroupOid1, {kGroupMemberOid1, kGroupMemberOid2});

  // Force clear the group OID from the entry to cause an error.
  auto* actual_entry = GetMulticastGroupEntry("0x1");
  ASSERT_NE(actual_entry, nullptr);
  actual_entry->multicast_group_oid = SAI_OBJECT_TYPE_NULL;

  // Validate an existing multicast entry.
  ReturnCode status = ValidateMulticastGroupEntry(group_entry1, SET_COMMAND);
  EXPECT_EQ(StatusCode::SWSS_RC_NOT_FOUND, status);
  // Delete also fails.
  status = ValidateMulticastGroupEntry(group_entry1, DEL_COMMAND);
  EXPECT_EQ(StatusCode::SWSS_RC_NOT_FOUND, status);
}

TEST_F(L3MulticastManagerTest,
       ValidateSetMulticastGroupEntryUpdateAddNewMemberButOidExistsTest) {
  // Add router interface entries so have RIF.
  auto rif_entry1 = SetupP4MulticastRouterInterfaceEntry(
      "Ethernet1", "0x0", swss::MacAddress(kSrcMac1), kRifOid1);
  auto rif_entry2 = SetupP4MulticastRouterInterfaceEntry(
      "Ethernet2", "0x0", swss::MacAddress(kSrcMac2), kRifOid2);
  // Setup multicast group entries.
  P4Replica replica1 = P4Replica("0x1", "Ethernet1", "0x0");
  P4Replica replica2 = P4Replica("0x1", "Ethernet2", "0x0");

  auto group_entry1 =
      SetupP4MulticastGroupEntry("0x1", {replica1, replica2}, kGroupOid1,
                                 {kGroupMemberOid1, kGroupMemberOid2});
  // Force remove the group member OID from the entry to cause an error that
  // it's already in the central map.
  auto* actual_entry = GetMulticastGroupEntry("0x1");
  ASSERT_NE(actual_entry, nullptr);
  actual_entry->member_oids.erase(replica1.key);

  // Validate an existing multicast entry.
  ReturnCode status = ValidateMulticastGroupEntry(group_entry1, SET_COMMAND);
  EXPECT_EQ(StatusCode::SWSS_RC_INTERNAL, status);
  // Delete also fails.
  status = ValidateMulticastGroupEntry(group_entry1, DEL_COMMAND);
  EXPECT_EQ(StatusCode::SWSS_RC_NOT_FOUND, status);
}

TEST_F(L3MulticastManagerTest,
       ValidateSetMulticastGroupEntryUpdateMemberButOidNotInMapperTest) {
  // Add router interface entries so have RIF.
  auto rif_entry1 = SetupP4MulticastRouterInterfaceEntry(
      "Ethernet1", "0x0", swss::MacAddress(kSrcMac1), kRifOid1);
  auto rif_entry2 = SetupP4MulticastRouterInterfaceEntry(
      "Ethernet2", "0x0", swss::MacAddress(kSrcMac2), kRifOid2);
  // Setup multicast group entries.
  P4Replica replica1 = P4Replica("0x1", "Ethernet1", "0x0");
  P4Replica replica2 = P4Replica("0x1", "Ethernet2", "0x0");

  auto group_entry1 =
      SetupP4MulticastGroupEntry("0x1", {replica1, replica2}, kGroupOid1,
                                 {kGroupMemberOid1, kGroupMemberOid2});
  // Force remove the group member OID from central map to cause an error.
  p4_oid_mapper_.eraseOID(SAI_OBJECT_TYPE_IPMC_GROUP_MEMBER, replica1.key);

  // Validate an existing multicast entry.
  ReturnCode status = ValidateMulticastGroupEntry(group_entry1, SET_COMMAND);
  EXPECT_EQ(StatusCode::SWSS_RC_INTERNAL, status);
  // Delete also fails.
  status = ValidateMulticastGroupEntry(group_entry1, DEL_COMMAND);
  EXPECT_EQ(StatusCode::SWSS_RC_NOT_FOUND, status);
}

TEST_F(L3MulticastManagerTest,
       ValidateSetMulticastGroupEntryUpdateNoGroupOidTest) {
  // Add router interface entries so have RIF.
  auto rif_entry1 = SetupP4MulticastRouterInterfaceEntry(
      "Ethernet1", "0x0", swss::MacAddress(kSrcMac1), kRifOid1);
  auto rif_entry2 = SetupP4MulticastRouterInterfaceEntry(
      "Ethernet2", "0x0", swss::MacAddress(kSrcMac2), kRifOid2);
  // Setup multicast group entries.
  P4Replica replica1 = P4Replica("0x1", "Ethernet1", "0x0");
  P4Replica replica2 = P4Replica("0x1", "Ethernet2", "0x0");
  auto group_entry1 =
      SetupP4MulticastGroupEntry("0x1", {replica1, replica2}, kGroupOid1,
                                 {kGroupMemberOid1, kGroupMemberOid2});

  // Force remove the multicast group OID to cause an error.
  p4_oid_mapper_.eraseOID(SAI_OBJECT_TYPE_IPMC_GROUP,
                          group_entry1.multicast_group_id);

  // Validate an existing multicast entry.
  ReturnCode status = ValidateMulticastGroupEntry(group_entry1, SET_COMMAND);
  EXPECT_EQ(StatusCode::SWSS_RC_NOT_FOUND, status);
  // Delete also fails.
  status = ValidateMulticastGroupEntry(group_entry1, DEL_COMMAND);
  EXPECT_EQ(StatusCode::SWSS_RC_NOT_FOUND, status);
}

TEST_F(L3MulticastManagerTest,
       ValidateDelMulticastGroupEntryDeleteUnknownTest) {
  // Add router interface entries so have RIF.
  auto rif_entry = SetupP4MulticastRouterInterfaceEntry(
      "Ethernet1", "0x0", swss::MacAddress(kSrcMac1), kRifOid1);
  auto entry = GenerateP4MulticastGroupEntry(
      "0x1", {P4Replica("0x1", "Ethernet1", "0x0")});
  ReturnCode status = ValidateMulticastGroupEntry(entry, DEL_COMMAND);
  EXPECT_EQ(StatusCode::SWSS_RC_NOT_FOUND, status);
}

TEST_F(L3MulticastManagerTest,
       ValidateSetMulticastGroupEntryUnknownCommandTest) {
  // Add router interface entries so have RIF.
  auto rif_entry = SetupP4MulticastRouterInterfaceEntry(
      "Ethernet1", "0x0", swss::MacAddress(kSrcMac1), kRifOid1);

  auto entry = GenerateP4MulticastGroupEntry(
      "0x1", {P4Replica("0x1", "Ethernet1", "0x0")});
  ReturnCode status = ValidateMulticastGroupEntry(entry, "do_things");
  EXPECT_EQ(StatusCode::SWSS_RC_INVALID_PARAM, status);
}

TEST_F(L3MulticastManagerTest, DrainMulticastGroupEntryAddSuccessTest) {
  const std::string mac_match_key =
      R"({"match/multicast_replica_port":"Ethernet1",)"
      R"("match/multicast_replica_instance":"0x1"})";
  const std::string mac_appl_db_key =
      std::string(APP_P4RT_MULTICAST_ROUTER_INTERFACE_TABLE_NAME) +
      kTableKeyDelimiter + mac_match_key;
  std::vector<swss::FieldValueTuple> mac_attributes;
  mac_attributes.push_back(
      swss::FieldValueTuple{p4orch::kAction, p4orch::kSetMulticastSrcMac});
  mac_attributes.push_back(
      swss::FieldValueTuple{prependParamField(p4orch::kSrcMac), kSrcMac1});
  mac_attributes.push_back(
      swss::FieldValueTuple{p4orch::kControllerMetadata, "so_meta"});

  const std::string group_match_key = "0x1";
  const std::string group_appl_db_key =
      std::string(APP_P4RT_REPLICATION_IP_MULTICAST_TABLE_NAME) +
      kTableKeyDelimiter + group_match_key;
  std::vector<swss::FieldValueTuple> group_attributes;
  const std::string json_array = R"([{"multicast_replica_instance":"0x1",)"
                                 R"("multicast_replica_port":"Ethernet1"}])";
  group_attributes.push_back(swss::FieldValueTuple{"replicas", json_array});
  group_attributes.push_back(
      swss::FieldValueTuple{p4orch::kControllerMetadata, "controller_meta"});
  group_attributes.push_back(
      swss::FieldValueTuple{p4orch::kMulticastMetadata, "multicast_meta"});

  // Enqueue RIF creation and group member creation.
  Enqueue(APP_P4RT_MULTICAST_ROUTER_INTERFACE_TABLE_NAME,
          swss::KeyOpFieldsValuesTuple(mac_appl_db_key, SET_COMMAND,
                                       mac_attributes));
  Enqueue(APP_P4RT_REPLICATION_IP_MULTICAST_TABLE_NAME,
          swss::KeyOpFieldsValuesTuple(group_appl_db_key, SET_COMMAND,
                                       group_attributes));

  EXPECT_CALL(mock_sai_router_intf_, create_router_interface(_, _, _, _))
      .WillOnce(DoAll(SetArgPointee<0>(kRifOid1), Return(SAI_STATUS_SUCCESS)));
  EXPECT_CALL(publisher_, publish(Eq(APP_P4RT_TABLE_NAME), Eq(mac_appl_db_key),
                                  Eq(mac_attributes),
                                  Eq(StatusCode::SWSS_RC_SUCCESS), Eq(true)));
  EXPECT_CALL(mock_sai_ipmc_group_, create_ipmc_group(_, _, _, _))
      .WillOnce(
          DoAll(SetArgPointee<0>(kGroupOid1), Return(SAI_STATUS_SUCCESS)));
  std::vector<sai_attribute_t> exp_member_attrs;
  sai_attribute_t attr;
  attr.id = SAI_IPMC_GROUP_MEMBER_ATTR_IPMC_GROUP_ID;
  attr.value.oid = kGroupOid1;
  exp_member_attrs.push_back(attr);
  attr.id = SAI_IPMC_GROUP_MEMBER_ATTR_IPMC_OUTPUT_ID;
  attr.value.oid = kRifOid1;
  exp_member_attrs.push_back(attr);

  EXPECT_CALL(mock_sai_ipmc_group_,
              create_ipmc_group_member(_, _, 2, AttrArrayEq(exp_member_attrs)))
      .WillOnce(DoAll(SetArgPointee<0>(kGroupMemberOid1),
                      Return(SAI_STATUS_SUCCESS)));
  EXPECT_CALL(publisher_, publish(Eq(APP_P4RT_TABLE_NAME),
                                  Eq(group_appl_db_key), Eq(group_attributes),
                                  Eq(StatusCode::SWSS_RC_SUCCESS), Eq(true)));
  EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS, Drain(/*failure_before=*/false));

  auto expect_mac_entry = GenerateP4MulticastRouterInterfaceEntry(
      "Ethernet1", "0x1", swss::MacAddress(kSrcMac1));
  expect_mac_entry.router_interface_oid = kRifOid1;
  P4Replica replica1 = P4Replica("0x1", "Ethernet1", "0x1");
  auto expect_group_entry = GenerateP4MulticastGroupEntry(
      "0x1", {replica1}, "multicast_meta", "controller_meta");
  expect_group_entry.multicast_group_oid = kGroupOid1;
  expect_group_entry.member_oids[replica1.key] = kGroupMemberOid1;

  auto* actual_mac_entry = GetMulticastRouterInterfaceEntry(
      expect_mac_entry.multicast_router_interface_entry_key);
  ASSERT_NE(nullptr, actual_mac_entry);
  VerifyP4MulticastRouterInterfaceEntryEqual(expect_mac_entry,
                                             *actual_mac_entry);
  auto end_rifOid = GetRifOid(actual_mac_entry);
  EXPECT_EQ(end_rifOid, kRifOid1);

  auto* actual_group_entry =
      GetMulticastGroupEntry(expect_group_entry.multicast_group_id);
  ASSERT_NE(nullptr, actual_group_entry);
  VerifyP4MulticastGroupEntryEqual(expect_group_entry, *actual_group_entry);

  sai_object_id_t end_groupOid = SAI_NULL_OBJECT_ID;
  p4_oid_mapper_.getOID(SAI_OBJECT_TYPE_IPMC_GROUP,
                        actual_group_entry->multicast_group_id, &end_groupOid);
  sai_object_id_t end_groupMemberOid = SAI_NULL_OBJECT_ID;
  p4_oid_mapper_.getOID(SAI_OBJECT_TYPE_IPMC_GROUP_MEMBER, replica1.key,
                        &end_groupMemberOid);
  EXPECT_EQ(end_groupOid, kGroupOid1);
  EXPECT_EQ(end_groupMemberOid, kGroupMemberOid1);
}

TEST_F(L3MulticastManagerTest, DrainMulticastGroupEntryAddInvalidEntryTest) {
  const std::string good_appl_db_key =
      std::string(APP_P4RT_REPLICATION_IP_MULTICAST_TABLE_NAME) +
      kTableKeyDelimiter + "0x1";

  std::vector<swss::FieldValueTuple> bad_group_attributes;
  // The port is missing.
  const std::string json_array_bad =
      R"([{"multicast_replica_instance":"0x1"}])";
  bad_group_attributes.push_back(
      swss::FieldValueTuple{"replicas", json_array_bad});
  std::vector<swss::FieldValueTuple> good_group_attributes;
  const std::string json_array_good =
      R"([{"multicast_replica_instance":"0x1",)"
      R"("multicast_replica_port":"Ethernet1"}])";
  good_group_attributes.push_back(
      swss::FieldValueTuple{"replicas", json_array_good});
  // Enqueue entry for create operation.
  Enqueue(APP_P4RT_REPLICATION_IP_MULTICAST_TABLE_NAME,
          swss::KeyOpFieldsValuesTuple(good_appl_db_key, SET_COMMAND,
                                       bad_group_attributes));
  Enqueue(APP_P4RT_REPLICATION_IP_MULTICAST_TABLE_NAME,
          swss::KeyOpFieldsValuesTuple(good_appl_db_key, SET_COMMAND,
                                       good_group_attributes));

  EXPECT_CALL(publisher_,
              publish(Eq(APP_P4RT_TABLE_NAME), Eq(good_appl_db_key),
                      Eq(bad_group_attributes),
                      Eq(StatusCode::SWSS_RC_INVALID_PARAM), Eq(true)));
  EXPECT_CALL(publisher_,
              publish(Eq(APP_P4RT_TABLE_NAME), Eq(good_appl_db_key),
                      Eq(good_group_attributes),
                      Eq(StatusCode::SWSS_RC_NOT_EXECUTED), Eq(true)));
  EXPECT_EQ(StatusCode::SWSS_RC_INVALID_PARAM, Drain(/*failure_before=*/false));
}

TEST_F(L3MulticastManagerTest, DrainMulticastGroupEntryBeforeRifAddedTest) {
  // If we do not add a RIF before using, the entry will be rejected as invalid.
  const std::string good_appl_db_key1 =
      std::string(APP_P4RT_REPLICATION_IP_MULTICAST_TABLE_NAME) +
      kTableKeyDelimiter + "0x1";

  const std::string good_appl_db_key2 =
      std::string(APP_P4RT_REPLICATION_IP_MULTICAST_TABLE_NAME) +
      kTableKeyDelimiter + "0x2";

  std::vector<swss::FieldValueTuple> good_group_attributes;
  const std::string json_array = R"([{"multicast_replica_instance":"0x1",)"
                                 R"("multicast_replica_port":"Ethernet1"}])";
  good_group_attributes.push_back(
      swss::FieldValueTuple{"replicas", json_array});

  // Enqueue entry for create operation.
  Enqueue(APP_P4RT_REPLICATION_IP_MULTICAST_TABLE_NAME,
          swss::KeyOpFieldsValuesTuple(good_appl_db_key1, SET_COMMAND,
                                       good_group_attributes));
  Enqueue(APP_P4RT_REPLICATION_IP_MULTICAST_TABLE_NAME,
          swss::KeyOpFieldsValuesTuple(good_appl_db_key2, SET_COMMAND,
                                       good_group_attributes));

  EXPECT_CALL(publisher_,
              publish(Eq(APP_P4RT_TABLE_NAME), Eq(good_appl_db_key1),
                      Eq(good_group_attributes),
                      Eq(StatusCode::SWSS_RC_NOT_FOUND), Eq(true)));
  EXPECT_CALL(publisher_,
              publish(Eq(APP_P4RT_TABLE_NAME), Eq(good_appl_db_key2),
                      Eq(good_group_attributes),
                      Eq(StatusCode::SWSS_RC_NOT_EXECUTED), Eq(true)));
  EXPECT_EQ(StatusCode::SWSS_RC_NOT_FOUND, Drain(/*failure_before=*/false));
}

TEST_F(L3MulticastManagerTest,
       DrainMulticastGroupEntryAddAndUpdateAndDeleteSuccessTest) {
  const std::string mac_match_key =
      R"({"match/multicast_replica_port":"Ethernet1",)"
      R"("match/multicast_replica_instance":"0x1"})";
  const std::string mac_appl_db_key =
      std::string(APP_P4RT_MULTICAST_ROUTER_INTERFACE_TABLE_NAME) +
      kTableKeyDelimiter + mac_match_key;
  const std::string mac_match_key2 =
      R"({"match/multicast_replica_port":"Ethernet2",)"
      R"("match/multicast_replica_instance":"0x1"})";
  const std::string mac_appl_db_key2 =
      std::string(APP_P4RT_MULTICAST_ROUTER_INTERFACE_TABLE_NAME) +
      kTableKeyDelimiter + mac_match_key2;
  std::vector<swss::FieldValueTuple> mac_attributes;
  mac_attributes.push_back(
      swss::FieldValueTuple{p4orch::kAction, p4orch::kSetMulticastSrcMac});
  mac_attributes.push_back(
      swss::FieldValueTuple{prependParamField(p4orch::kSrcMac), kSrcMac1});
  mac_attributes.push_back(
      swss::FieldValueTuple{p4orch::kControllerMetadata, "so_meta"});

  const std::string group_match_key = "0x1";
  const std::string group_appl_db_key =
      std::string(APP_P4RT_REPLICATION_IP_MULTICAST_TABLE_NAME) +
      kTableKeyDelimiter + group_match_key;
  std::vector<swss::FieldValueTuple> group_attributes;
  const std::string json_array = R"([{"multicast_replica_instance":"0x1",)"
                                 R"("multicast_replica_port":"Ethernet1"}])";
  group_attributes.push_back(swss::FieldValueTuple{"replicas", json_array});

  // Enqueue RIF creation and group member creation.
  Enqueue(APP_P4RT_MULTICAST_ROUTER_INTERFACE_TABLE_NAME,
          swss::KeyOpFieldsValuesTuple(mac_appl_db_key, SET_COMMAND,
                                       mac_attributes));
  Enqueue(APP_P4RT_MULTICAST_ROUTER_INTERFACE_TABLE_NAME,
          swss::KeyOpFieldsValuesTuple(mac_appl_db_key2, SET_COMMAND,
                                       mac_attributes));
  Enqueue(APP_P4RT_REPLICATION_IP_MULTICAST_TABLE_NAME,
          swss::KeyOpFieldsValuesTuple(group_appl_db_key, SET_COMMAND,
                                       group_attributes));

  EXPECT_CALL(mock_sai_router_intf_, create_router_interface(_, _, _, _))
      .WillOnce(DoAll(SetArgPointee<0>(kRifOid1), Return(SAI_STATUS_SUCCESS)))
      .WillOnce(DoAll(SetArgPointee<0>(kRifOid2), Return(SAI_STATUS_SUCCESS)));
  EXPECT_CALL(publisher_, publish(Eq(APP_P4RT_TABLE_NAME), Eq(mac_appl_db_key),
                                  Eq(mac_attributes),
                                  Eq(StatusCode::SWSS_RC_SUCCESS), Eq(true)));
  EXPECT_CALL(publisher_, publish(Eq(APP_P4RT_TABLE_NAME), Eq(mac_appl_db_key2),
                                  Eq(mac_attributes),
                                  Eq(StatusCode::SWSS_RC_SUCCESS), Eq(true)));
  EXPECT_CALL(mock_sai_ipmc_group_, create_ipmc_group(_, _, _, _))
      .WillOnce(
          DoAll(SetArgPointee<0>(kGroupOid1), Return(SAI_STATUS_SUCCESS)));
  EXPECT_CALL(mock_sai_ipmc_group_, create_ipmc_group_member(_, _, _, _))
      .WillOnce(DoAll(SetArgPointee<0>(kGroupMemberOid1),
                      Return(SAI_STATUS_SUCCESS)));
  EXPECT_CALL(publisher_, publish(Eq(APP_P4RT_TABLE_NAME),
                                  Eq(group_appl_db_key), Eq(group_attributes),
                                  Eq(StatusCode::SWSS_RC_SUCCESS), Eq(true)));
  EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS, Drain(/*failure_before=*/false));

  // Add another multicast group member (same group) and delete the first one.
  std::vector<swss::FieldValueTuple> group_attributes2;
  const std::string json_array2 = R"([{"multicast_replica_instance":"0x1",)"
                                  R"("multicast_replica_port":"Ethernet2"}])";
  group_attributes2.push_back(swss::FieldValueTuple{"replicas", json_array2});

  Enqueue(APP_P4RT_REPLICATION_IP_MULTICAST_TABLE_NAME,
          swss::KeyOpFieldsValuesTuple(group_appl_db_key, SET_COMMAND,
                                       group_attributes2));

  EXPECT_CALL(mock_sai_ipmc_group_, create_ipmc_group_member(_, _, _, _))
      .WillOnce(DoAll(SetArgPointee<0>(kGroupMemberOid2),
                      Return(SAI_STATUS_SUCCESS)));
  EXPECT_CALL(mock_sai_ipmc_group_, remove_ipmc_group_member(kGroupMemberOid1))
      .WillOnce(Return(SAI_STATUS_SUCCESS));
  EXPECT_CALL(publisher_, publish(Eq(APP_P4RT_TABLE_NAME),
                                  Eq(group_appl_db_key), Eq(group_attributes2),
                                  Eq(StatusCode::SWSS_RC_SUCCESS), Eq(true)));
  EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS, Drain(/*failure_before=*/false));

  P4Replica replica2 = P4Replica("0x1", "Ethernet2", "0x1");
  auto expect_group_entry = GenerateP4MulticastGroupEntry("0x1", {replica2});
  expect_group_entry.multicast_group_oid = kGroupOid1;  // same group used
  expect_group_entry.member_oids[replica2.key] = kGroupMemberOid2;

  auto* actual_group_entry = GetMulticastGroupEntry("0x1");
  ASSERT_NE(actual_group_entry, nullptr);

  VerifyP4MulticastGroupEntryEqual(expect_group_entry, *actual_group_entry);

  sai_object_id_t end_groupOid2 = SAI_NULL_OBJECT_ID;
  p4_oid_mapper_.getOID(SAI_OBJECT_TYPE_IPMC_GROUP,
                        actual_group_entry->multicast_group_id, &end_groupOid2);
  sai_object_id_t end_groupMemberOid2 = SAI_NULL_OBJECT_ID;
  p4_oid_mapper_.getOID(SAI_OBJECT_TYPE_IPMC_GROUP_MEMBER, replica2.key,
                        &end_groupMemberOid2);
  EXPECT_EQ(end_groupOid2, kGroupOid1);
  EXPECT_EQ(end_groupMemberOid2, kGroupMemberOid2);

  // Then delete the group.
  std::vector<swss::FieldValueTuple> group_attributes_del = {};
  Enqueue(APP_P4RT_REPLICATION_IP_MULTICAST_TABLE_NAME,
          swss::KeyOpFieldsValuesTuple(group_appl_db_key, DEL_COMMAND,
                                       group_attributes_del));
  EXPECT_CALL(mock_sai_ipmc_group_, remove_ipmc_group_member(kGroupMemberOid2))
      .WillOnce(Return(SAI_STATUS_SUCCESS));
  EXPECT_CALL(mock_sai_ipmc_group_, remove_ipmc_group(kGroupOid1))
      .WillOnce(Return(SAI_STATUS_SUCCESS));
  EXPECT_CALL(publisher_,
              publish(Eq(APP_P4RT_TABLE_NAME), Eq(group_appl_db_key),
                      Eq(group_attributes_del), Eq(StatusCode::SWSS_RC_SUCCESS),
                      Eq(true)));

  EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS, Drain(/*failure_before=*/false));

  auto* actual_group_entry2 = GetMulticastGroupEntry("0x1");
  ASSERT_EQ(actual_group_entry2, nullptr);
  EXPECT_FALSE(p4_oid_mapper_.existsOID(SAI_OBJECT_TYPE_IPMC_GROUP, "0x1"));
  EXPECT_FALSE(p4_oid_mapper_.existsOID(SAI_OBJECT_TYPE_IPMC_GROUP_MEMBER,
                                        replica2.key));
}

TEST_F(L3MulticastManagerTest, DrainMulticastGroupEntryAllOpsSuccessTest) {
  const std::string mac_match_key =
      R"({"match/multicast_replica_port":"Ethernet1",)"
      R"("match/multicast_replica_instance":"0x1"})";
  const std::string mac_appl_db_key =
      std::string(APP_P4RT_MULTICAST_ROUTER_INTERFACE_TABLE_NAME) +
      kTableKeyDelimiter + mac_match_key;
  const std::string mac_match_key2 =
      R"({"match/multicast_replica_port":"Ethernet2",)"
      R"("match/multicast_replica_instance":"0x1"})";
  const std::string mac_appl_db_key2 =
      std::string(APP_P4RT_MULTICAST_ROUTER_INTERFACE_TABLE_NAME) +
      kTableKeyDelimiter + mac_match_key2;
  const std::string mac_match_key3 =
      R"({"match/multicast_replica_port":"Ethernet3",)"
      R"("match/multicast_replica_instance":"0x1"})";
  const std::string mac_appl_db_key3 =
      std::string(APP_P4RT_MULTICAST_ROUTER_INTERFACE_TABLE_NAME) +
      kTableKeyDelimiter + mac_match_key3;
  const std::string mac_match_key4 =
      R"({"match/multicast_replica_port":"Ethernet4",)"
      R"("match/multicast_replica_instance":"0x1"})";
  const std::string mac_appl_db_key4 =
      std::string(APP_P4RT_MULTICAST_ROUTER_INTERFACE_TABLE_NAME) +
      kTableKeyDelimiter + mac_match_key4;
  std::vector<swss::FieldValueTuple> mac_attributes;
  mac_attributes.push_back(
      swss::FieldValueTuple{p4orch::kAction, p4orch::kSetMulticastSrcMac});
  mac_attributes.push_back(
      swss::FieldValueTuple{prependParamField(p4orch::kSrcMac), kSrcMac1});
  mac_attributes.push_back(
      swss::FieldValueTuple{p4orch::kControllerMetadata, "so_meta"});

  const std::string group_appl_db_key1 =
      std::string(APP_P4RT_REPLICATION_IP_MULTICAST_TABLE_NAME) +
      kTableKeyDelimiter + "0x1";
  std::vector<swss::FieldValueTuple> group_attributes1;
  const std::string json_array1 = R"([{"multicast_replica_instance":"0x1",)"
                                  R"("multicast_replica_port":"Ethernet1"}])";
  group_attributes1.push_back(swss::FieldValueTuple{"replicas", json_array1});

  const std::string group_appl_db_key2 =
      std::string(APP_P4RT_REPLICATION_IP_MULTICAST_TABLE_NAME) +
      kTableKeyDelimiter + "0x2";
  std::vector<swss::FieldValueTuple> group_attributes2;
  const std::string json_array2 = R"([{"multicast_replica_instance":"0x1",)"
                                  R"("multicast_replica_port":"Ethernet2"}])";
  group_attributes2.push_back(swss::FieldValueTuple{"replicas", json_array2});

  // Enqueue RIF creation and group member creation.
  Enqueue(APP_P4RT_MULTICAST_ROUTER_INTERFACE_TABLE_NAME,
          swss::KeyOpFieldsValuesTuple(mac_appl_db_key, SET_COMMAND,
                                       mac_attributes));
  Enqueue(APP_P4RT_MULTICAST_ROUTER_INTERFACE_TABLE_NAME,
          swss::KeyOpFieldsValuesTuple(mac_appl_db_key2, SET_COMMAND,
                                       mac_attributes));
  Enqueue(APP_P4RT_MULTICAST_ROUTER_INTERFACE_TABLE_NAME,
          swss::KeyOpFieldsValuesTuple(mac_appl_db_key3, SET_COMMAND,
                                       mac_attributes));
  Enqueue(APP_P4RT_MULTICAST_ROUTER_INTERFACE_TABLE_NAME,
          swss::KeyOpFieldsValuesTuple(mac_appl_db_key4, SET_COMMAND,
                                       mac_attributes));
  Enqueue(APP_P4RT_REPLICATION_IP_MULTICAST_TABLE_NAME,
          swss::KeyOpFieldsValuesTuple(group_appl_db_key1, SET_COMMAND,
                                       group_attributes1));
  Enqueue(APP_P4RT_REPLICATION_IP_MULTICAST_TABLE_NAME,
          swss::KeyOpFieldsValuesTuple(group_appl_db_key2, SET_COMMAND,
                                       group_attributes2));

  EXPECT_CALL(mock_sai_router_intf_, create_router_interface(_, _, _, _))
      .WillOnce(DoAll(SetArgPointee<0>(kRifOid1), Return(SAI_STATUS_SUCCESS)))
      .WillOnce(DoAll(SetArgPointee<0>(kRifOid2), Return(SAI_STATUS_SUCCESS)))
      .WillOnce(DoAll(SetArgPointee<0>(kRifOid3), Return(SAI_STATUS_SUCCESS)))
      .WillOnce(DoAll(SetArgPointee<0>(kRifOid4), Return(SAI_STATUS_SUCCESS)));
  EXPECT_CALL(publisher_, publish(Eq(APP_P4RT_TABLE_NAME), Eq(mac_appl_db_key),
                                  Eq(mac_attributes),
                                  Eq(StatusCode::SWSS_RC_SUCCESS), Eq(true)));
  EXPECT_CALL(publisher_, publish(Eq(APP_P4RT_TABLE_NAME), Eq(mac_appl_db_key2),
                                  Eq(mac_attributes),
                                  Eq(StatusCode::SWSS_RC_SUCCESS), Eq(true)));
  EXPECT_CALL(publisher_, publish(Eq(APP_P4RT_TABLE_NAME), Eq(mac_appl_db_key3),
                                  Eq(mac_attributes),
                                  Eq(StatusCode::SWSS_RC_SUCCESS), Eq(true)));
  EXPECT_CALL(publisher_, publish(Eq(APP_P4RT_TABLE_NAME), Eq(mac_appl_db_key4),
                                  Eq(mac_attributes),
                                  Eq(StatusCode::SWSS_RC_SUCCESS), Eq(true)));
  EXPECT_CALL(mock_sai_ipmc_group_, create_ipmc_group(_, _, _, _))
      .WillOnce(DoAll(SetArgPointee<0>(kGroupOid1), Return(SAI_STATUS_SUCCESS)))
      .WillOnce(
          DoAll(SetArgPointee<0>(kGroupOid2), Return(SAI_STATUS_SUCCESS)));
  EXPECT_CALL(mock_sai_ipmc_group_, create_ipmc_group_member(_, _, _, _))
      .WillOnce(
          DoAll(SetArgPointee<0>(kGroupMemberOid1), Return(SAI_STATUS_SUCCESS)))
      .WillOnce(DoAll(SetArgPointee<0>(kGroupMemberOid2),
                      Return(SAI_STATUS_SUCCESS)));
  EXPECT_CALL(publisher_, publish(Eq(APP_P4RT_TABLE_NAME),
                                  Eq(group_appl_db_key1), Eq(group_attributes1),
                                  Eq(StatusCode::SWSS_RC_SUCCESS), Eq(true)));
  EXPECT_CALL(publisher_, publish(Eq(APP_P4RT_TABLE_NAME),
                                  Eq(group_appl_db_key2), Eq(group_attributes2),
                                  Eq(StatusCode::SWSS_RC_SUCCESS), Eq(true)));
  EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS, Drain(/*failure_before=*/false));

  // Add another multicast group, update the first group, and
  // delete the second group.
  const std::string group_appl_db_key3 =
      std::string(APP_P4RT_REPLICATION_IP_MULTICAST_TABLE_NAME) +
      kTableKeyDelimiter + "0x3";
  std::vector<swss::FieldValueTuple> group_attributes3;
  const std::string json_array3 = R"([{"multicast_replica_instance":"0x1",)"
                                  R"("multicast_replica_port":"Ethernet3"}])";
  group_attributes3.push_back(swss::FieldValueTuple{"replicas", json_array3});

  // This is expected to delete replica1, since it is no longer in the entry.
  std::vector<swss::FieldValueTuple> group_attributes4;
  const std::string json_array4 = R"([{"multicast_replica_instance":"0x1",)"
                                  R"("multicast_replica_port":"Ethernet4"}])";
  group_attributes4.push_back(swss::FieldValueTuple{"replicas", json_array4});
  std::vector<swss::FieldValueTuple> group_attributes_del = {};

  Enqueue(APP_P4RT_REPLICATION_IP_MULTICAST_TABLE_NAME,
          swss::KeyOpFieldsValuesTuple(group_appl_db_key3, SET_COMMAND,
                                       group_attributes3));
  Enqueue(APP_P4RT_REPLICATION_IP_MULTICAST_TABLE_NAME,
          swss::KeyOpFieldsValuesTuple(group_appl_db_key1, SET_COMMAND,
                                       group_attributes4));
  Enqueue(APP_P4RT_REPLICATION_IP_MULTICAST_TABLE_NAME,
          swss::KeyOpFieldsValuesTuple(group_appl_db_key2, DEL_COMMAND,
                                       group_attributes_del));

  EXPECT_CALL(mock_sai_ipmc_group_, create_ipmc_group(_, _, _, _))
      .WillOnce(
          DoAll(SetArgPointee<0>(kGroupOid3), Return(SAI_STATUS_SUCCESS)));
  EXPECT_CALL(mock_sai_ipmc_group_, create_ipmc_group_member(_, _, _, _))
      .WillOnce(
          DoAll(SetArgPointee<0>(kGroupMemberOid3), Return(SAI_STATUS_SUCCESS)))
      .WillOnce(DoAll(SetArgPointee<0>(kGroupMemberOid4),
                      Return(SAI_STATUS_SUCCESS)));
  EXPECT_CALL(mock_sai_ipmc_group_, remove_ipmc_group_member(kGroupMemberOid1))
      .WillOnce(Return(SAI_STATUS_SUCCESS));
  EXPECT_CALL(mock_sai_ipmc_group_, remove_ipmc_group_member(kGroupMemberOid2))
      .WillOnce(Return(SAI_STATUS_SUCCESS));
  EXPECT_CALL(mock_sai_ipmc_group_, remove_ipmc_group(kGroupOid2))
      .WillOnce(Return(SAI_STATUS_SUCCESS));
  EXPECT_CALL(publisher_, publish(Eq(APP_P4RT_TABLE_NAME),
                                  Eq(group_appl_db_key3), Eq(group_attributes3),
                                  Eq(StatusCode::SWSS_RC_SUCCESS), Eq(true)));
  EXPECT_CALL(publisher_, publish(Eq(APP_P4RT_TABLE_NAME),
                                  Eq(group_appl_db_key1), Eq(group_attributes4),
                                  Eq(StatusCode::SWSS_RC_SUCCESS), Eq(true)));
  EXPECT_CALL(publisher_,
              publish(Eq(APP_P4RT_TABLE_NAME), Eq(group_appl_db_key2),
                      Eq(group_attributes_del), Eq(StatusCode::SWSS_RC_SUCCESS),
                      Eq(true)));
  EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS, Drain(/*failure_before=*/false));

  P4Replica replica1 = P4Replica("0x1", "Ethernet1", "0x1");
  P4Replica replica2 = P4Replica("0x2", "Ethernet2", "0x1");
  P4Replica replica3 = P4Replica("0x3", "Ethernet3", "0x1");
  P4Replica replica4 = P4Replica("0x1", "Ethernet4", "0x1");

  auto expect_group_entry1 =
      GenerateP4MulticastGroupEntry("0x1", {replica4});  // replica1 was deleted
  expect_group_entry1.multicast_group_oid = kGroupOid1;
  expect_group_entry1.member_oids[replica4.key] = kGroupMemberOid4;

  // entry 2 was deleted

  auto expect_group_entry3 = GenerateP4MulticastGroupEntry("0x3", {replica3});
  expect_group_entry3.multicast_group_oid = kGroupOid3;
  expect_group_entry3.member_oids[replica3.key] = kGroupMemberOid3;

  auto* actual_group_entry1 = GetMulticastGroupEntry("0x1");
  auto* actual_group_entry2 = GetMulticastGroupEntry("0x2");
  auto* actual_group_entry3 = GetMulticastGroupEntry("0x3");
  ASSERT_NE(actual_group_entry1, nullptr);
  EXPECT_EQ(actual_group_entry2, nullptr);
  ASSERT_NE(actual_group_entry3, nullptr);

  VerifyP4MulticastGroupEntryEqual(expect_group_entry1, *actual_group_entry1);
  VerifyP4MulticastGroupEntryEqual(expect_group_entry3, *actual_group_entry3);

  sai_object_id_t end_groupOid = SAI_NULL_OBJECT_ID;
  p4_oid_mapper_.getOID(SAI_OBJECT_TYPE_IPMC_GROUP, "0x1", &end_groupOid);
  sai_object_id_t end_groupMemberOid = SAI_NULL_OBJECT_ID;
  p4_oid_mapper_.getOID(SAI_OBJECT_TYPE_IPMC_GROUP_MEMBER, replica4.key,
                        &end_groupMemberOid);
  EXPECT_EQ(end_groupOid, kGroupOid1);
  EXPECT_EQ(end_groupMemberOid, kGroupMemberOid4);

  EXPECT_FALSE(p4_oid_mapper_.existsOID(SAI_OBJECT_TYPE_IPMC_GROUP, "0x2"));

  sai_object_id_t end_groupOid3 = SAI_NULL_OBJECT_ID;
  p4_oid_mapper_.getOID(SAI_OBJECT_TYPE_IPMC_GROUP, "0x3", &end_groupOid3);
  sai_object_id_t end_groupMemberOid3 = SAI_NULL_OBJECT_ID;
  p4_oid_mapper_.getOID(SAI_OBJECT_TYPE_IPMC_GROUP_MEMBER, replica3.key,
                        &end_groupMemberOid3);
  EXPECT_EQ(end_groupOid3, kGroupOid3);
  EXPECT_EQ(end_groupMemberOid3, kGroupMemberOid3);

  EXPECT_FALSE(p4_oid_mapper_.existsOID(SAI_OBJECT_TYPE_IPMC_GROUP_MEMBER,
                                        replica1.key));
  EXPECT_FALSE(p4_oid_mapper_.existsOID(SAI_OBJECT_TYPE_IPMC_GROUP_MEMBER,
                                        replica2.key));
}

TEST_F(L3MulticastManagerTest, DrainMulticastGroupEntryDeleteFailureTest) {
  const std::string mac_match_key =
      R"({"match/multicast_replica_port":"Ethernet1",)"
      R"("match/multicast_replica_instance":"0x1"})";
  const std::string mac_appl_db_key =
      std::string(APP_P4RT_MULTICAST_ROUTER_INTERFACE_TABLE_NAME) +
      kTableKeyDelimiter + mac_match_key;
  const std::string mac_match_key2 =
      R"({"match/multicast_replica_port":"Ethernet2",)"
      R"("match/multicast_replica_instance":"0x1"})";
  const std::string mac_appl_db_key2 =
      std::string(APP_P4RT_MULTICAST_ROUTER_INTERFACE_TABLE_NAME) +
      kTableKeyDelimiter + mac_match_key2;
  const std::string mac_match_key3 =
      R"({"match/multicast_replica_port":"Ethernet3",)"
      R"("match/multicast_replica_instance":"0x1"})";
  const std::string mac_appl_db_key3 =
      std::string(APP_P4RT_MULTICAST_ROUTER_INTERFACE_TABLE_NAME) +
      kTableKeyDelimiter + mac_match_key3;
  std::vector<swss::FieldValueTuple> mac_attributes;
  mac_attributes.push_back(
      swss::FieldValueTuple{p4orch::kAction, p4orch::kSetMulticastSrcMac});
  mac_attributes.push_back(
      swss::FieldValueTuple{prependParamField(p4orch::kSrcMac), kSrcMac1});
  mac_attributes.push_back(
      swss::FieldValueTuple{p4orch::kControllerMetadata, "so_meta"});

  const std::string group_appl_db_key1 =
      std::string(APP_P4RT_REPLICATION_IP_MULTICAST_TABLE_NAME) +
      kTableKeyDelimiter + "0x1";
  std::vector<swss::FieldValueTuple> group_attributes1;
  const std::string json_array1 = R"([{"multicast_replica_instance":"0x1",)"
                                  R"("multicast_replica_port":"Ethernet1"}])";
  group_attributes1.push_back(swss::FieldValueTuple{"replicas", json_array1});

  const std::string group_appl_db_key2 =
      std::string(APP_P4RT_REPLICATION_IP_MULTICAST_TABLE_NAME) +
      kTableKeyDelimiter + "0x2";
  std::vector<swss::FieldValueTuple> group_attributes2;
  const std::string json_array2 = R"([{"multicast_replica_instance":"0x1",)"
                                  R"("multicast_replica_port":"Ethernet2"}])";
  group_attributes2.push_back(swss::FieldValueTuple{"replicas", json_array1});

  // Enqueue RIF creation and group member creation.
  Enqueue(APP_P4RT_MULTICAST_ROUTER_INTERFACE_TABLE_NAME,
          swss::KeyOpFieldsValuesTuple(mac_appl_db_key, SET_COMMAND,
                                       mac_attributes));
  Enqueue(APP_P4RT_MULTICAST_ROUTER_INTERFACE_TABLE_NAME,
          swss::KeyOpFieldsValuesTuple(mac_appl_db_key2, SET_COMMAND,
                                       mac_attributes));
  Enqueue(APP_P4RT_MULTICAST_ROUTER_INTERFACE_TABLE_NAME,
          swss::KeyOpFieldsValuesTuple(mac_appl_db_key3, SET_COMMAND,
                                       mac_attributes));
  Enqueue(APP_P4RT_REPLICATION_IP_MULTICAST_TABLE_NAME,
          swss::KeyOpFieldsValuesTuple(group_appl_db_key1, SET_COMMAND,
                                       group_attributes1));
  Enqueue(APP_P4RT_REPLICATION_IP_MULTICAST_TABLE_NAME,
          swss::KeyOpFieldsValuesTuple(group_appl_db_key2, SET_COMMAND,
                                       group_attributes2));

  EXPECT_CALL(mock_sai_router_intf_, create_router_interface(_, _, _, _))
      .WillOnce(DoAll(SetArgPointee<0>(kRifOid1), Return(SAI_STATUS_SUCCESS)))
      .WillOnce(DoAll(SetArgPointee<0>(kRifOid2), Return(SAI_STATUS_SUCCESS)))
      .WillOnce(DoAll(SetArgPointee<0>(kRifOid3), Return(SAI_STATUS_SUCCESS)));
  EXPECT_CALL(publisher_, publish(Eq(APP_P4RT_TABLE_NAME), Eq(mac_appl_db_key),
                                  Eq(mac_attributes),
                                  Eq(StatusCode::SWSS_RC_SUCCESS), Eq(true)));
  EXPECT_CALL(publisher_, publish(Eq(APP_P4RT_TABLE_NAME), Eq(mac_appl_db_key2),
                                  Eq(mac_attributes),
                                  Eq(StatusCode::SWSS_RC_SUCCESS), Eq(true)));
  EXPECT_CALL(publisher_, publish(Eq(APP_P4RT_TABLE_NAME), Eq(mac_appl_db_key3),
                                  Eq(mac_attributes),
                                  Eq(StatusCode::SWSS_RC_SUCCESS), Eq(true)));
  EXPECT_CALL(mock_sai_ipmc_group_, create_ipmc_group(_, _, _, _))
      .WillOnce(DoAll(SetArgPointee<0>(kGroupOid1), Return(SAI_STATUS_SUCCESS)))
      .WillOnce(
          DoAll(SetArgPointee<0>(kGroupOid2), Return(SAI_STATUS_SUCCESS)));
  EXPECT_CALL(mock_sai_ipmc_group_, create_ipmc_group_member(_, _, _, _))
      .WillOnce(
          DoAll(SetArgPointee<0>(kGroupMemberOid1), Return(SAI_STATUS_SUCCESS)))
      .WillOnce(DoAll(SetArgPointee<0>(kGroupMemberOid2),
                      Return(SAI_STATUS_SUCCESS)));
  EXPECT_CALL(publisher_, publish(Eq(APP_P4RT_TABLE_NAME),
                                  Eq(group_appl_db_key1), Eq(group_attributes1),
                                  Eq(StatusCode::SWSS_RC_SUCCESS), Eq(true)));
  EXPECT_CALL(publisher_, publish(Eq(APP_P4RT_TABLE_NAME),
                                  Eq(group_appl_db_key2), Eq(group_attributes2),
                                  Eq(StatusCode::SWSS_RC_SUCCESS), Eq(true)));
  EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS, Drain(/*failure_before=*/false));

  // Update group 1 to add a new member.
  // Attempt to delete group 2 (has error).
  // Set group 1 again (should be no-op, but not executed).
  std::vector<swss::FieldValueTuple> group_attributes1b;
  const std::string json_array1b = R"([{"multicast_replica_instance":"0x1",)"
                                   R"("multicast_replica_port":"Ethernet1"},)"
                                   R"({"multicast_replica_instance":"0x1",)"
                                   R"("multicast_replica_port":"Ethernet3"}])";
  group_attributes1b.push_back(swss::FieldValueTuple{"replicas", json_array1b});
  std::vector<swss::FieldValueTuple> group_attributes_del = {};

  Enqueue(APP_P4RT_REPLICATION_IP_MULTICAST_TABLE_NAME,
          swss::KeyOpFieldsValuesTuple(group_appl_db_key1, SET_COMMAND,
                                       group_attributes1b));
  // SAI delete failure
  Enqueue(APP_P4RT_REPLICATION_IP_MULTICAST_TABLE_NAME,
          swss::KeyOpFieldsValuesTuple(group_appl_db_key2, DEL_COMMAND,
                                       group_attributes_del));
  Enqueue(APP_P4RT_REPLICATION_IP_MULTICAST_TABLE_NAME,
          swss::KeyOpFieldsValuesTuple(group_appl_db_key1, SET_COMMAND,
                                       group_attributes1b));

  EXPECT_CALL(mock_sai_ipmc_group_, create_ipmc_group_member(_, _, _, _))
      .WillOnce(DoAll(SetArgPointee<0>(kGroupMemberOid3),
                      Return(SAI_STATUS_SUCCESS)));
  EXPECT_CALL(mock_sai_ipmc_group_, remove_ipmc_group_member(kGroupMemberOid2))
      .WillOnce(Return(SAI_STATUS_FAILURE));
  EXPECT_CALL(publisher_,
              publish(Eq(APP_P4RT_TABLE_NAME), Eq(group_appl_db_key1),
                      Eq(group_attributes1b), Eq(StatusCode::SWSS_RC_SUCCESS),
                      Eq(true)));
  EXPECT_CALL(publisher_,
              publish(Eq(APP_P4RT_TABLE_NAME), Eq(group_appl_db_key2),
                      Eq(group_attributes_del), Eq(StatusCode::SWSS_RC_UNKNOWN),
                      Eq(true)));
  EXPECT_CALL(publisher_,
              publish(Eq(APP_P4RT_TABLE_NAME), Eq(group_appl_db_key1),
                      Eq(group_attributes1b),
                      Eq(StatusCode::SWSS_RC_NOT_EXECUTED), Eq(true)));
  EXPECT_EQ(StatusCode::SWSS_RC_UNKNOWN, Drain(/*failure_before=*/false));
}

TEST_F(L3MulticastManagerTest,
       DrainMulticastGroupEntryDeleteFailureOnLastTest) {
  const std::string mac_match_key =
      R"({"match/multicast_replica_port":"Ethernet1",)"
      R"("match/multicast_replica_instance":"0x1"})";
  const std::string mac_appl_db_key =
      std::string(APP_P4RT_MULTICAST_ROUTER_INTERFACE_TABLE_NAME) +
      kTableKeyDelimiter + mac_match_key;
  const std::string mac_match_key2 =
      R"({"match/multicast_replica_port":"Ethernet2",)"
      R"("match/multicast_replica_instance":"0x1"})";
  const std::string mac_appl_db_key2 =
      std::string(APP_P4RT_MULTICAST_ROUTER_INTERFACE_TABLE_NAME) +
      kTableKeyDelimiter + mac_match_key2;
  const std::string mac_match_key3 =
      R"({"match/multicast_replica_port":"Ethernet3",)"
      R"("match/multicast_replica_instance":"0x1"})";
  const std::string mac_appl_db_key3 =
      std::string(APP_P4RT_MULTICAST_ROUTER_INTERFACE_TABLE_NAME) +
      kTableKeyDelimiter + mac_match_key3;
  std::vector<swss::FieldValueTuple> mac_attributes;
  mac_attributes.push_back(
      swss::FieldValueTuple{p4orch::kAction, p4orch::kSetMulticastSrcMac});
  mac_attributes.push_back(
      swss::FieldValueTuple{prependParamField(p4orch::kSrcMac), kSrcMac1});
  mac_attributes.push_back(
      swss::FieldValueTuple{p4orch::kControllerMetadata, "so_meta"});

  const std::string group_appl_db_key1 =
      std::string(APP_P4RT_REPLICATION_IP_MULTICAST_TABLE_NAME) +
      kTableKeyDelimiter + "0x1";
  std::vector<swss::FieldValueTuple> group_attributes1;
  const std::string json_array1 = R"([{"multicast_replica_instance":"0x1",)"
                                  R"("multicast_replica_port":"Ethernet1"}])";
  group_attributes1.push_back(swss::FieldValueTuple{"replicas", json_array1});
  const std::string group_appl_db_key2 =
      std::string(APP_P4RT_REPLICATION_IP_MULTICAST_TABLE_NAME) +
      kTableKeyDelimiter + "0x2";
  std::vector<swss::FieldValueTuple> group_attributes2;
  const std::string json_array2 = R"([{"multicast_replica_instance":"0x1",)"
                                  R"("multicast_replica_port":"Ethernet2"}])";
  group_attributes2.push_back(swss::FieldValueTuple{"replicas", json_array1});

  // Enqueue RIF creation and group member creation.
  Enqueue(APP_P4RT_MULTICAST_ROUTER_INTERFACE_TABLE_NAME,
          swss::KeyOpFieldsValuesTuple(mac_appl_db_key, SET_COMMAND,
                                       mac_attributes));
  Enqueue(APP_P4RT_MULTICAST_ROUTER_INTERFACE_TABLE_NAME,
          swss::KeyOpFieldsValuesTuple(mac_appl_db_key2, SET_COMMAND,
                                       mac_attributes));
  Enqueue(APP_P4RT_MULTICAST_ROUTER_INTERFACE_TABLE_NAME,
          swss::KeyOpFieldsValuesTuple(mac_appl_db_key3, SET_COMMAND,
                                       mac_attributes));
  Enqueue(APP_P4RT_REPLICATION_IP_MULTICAST_TABLE_NAME,
          swss::KeyOpFieldsValuesTuple(group_appl_db_key1, SET_COMMAND,
                                       group_attributes1));
  Enqueue(APP_P4RT_REPLICATION_IP_MULTICAST_TABLE_NAME,
          swss::KeyOpFieldsValuesTuple(group_appl_db_key2, SET_COMMAND,
                                       group_attributes2));

  EXPECT_CALL(mock_sai_router_intf_, create_router_interface(_, _, _, _))
      .WillOnce(DoAll(SetArgPointee<0>(kRifOid1), Return(SAI_STATUS_SUCCESS)))
      .WillOnce(DoAll(SetArgPointee<0>(kRifOid2), Return(SAI_STATUS_SUCCESS)))
      .WillOnce(DoAll(SetArgPointee<0>(kRifOid3), Return(SAI_STATUS_SUCCESS)));
  EXPECT_CALL(publisher_, publish(Eq(APP_P4RT_TABLE_NAME), Eq(mac_appl_db_key),
                                  Eq(mac_attributes),
                                  Eq(StatusCode::SWSS_RC_SUCCESS), Eq(true)));
  EXPECT_CALL(publisher_, publish(Eq(APP_P4RT_TABLE_NAME), Eq(mac_appl_db_key2),
                                  Eq(mac_attributes),
                                  Eq(StatusCode::SWSS_RC_SUCCESS), Eq(true)));
  EXPECT_CALL(publisher_, publish(Eq(APP_P4RT_TABLE_NAME), Eq(mac_appl_db_key3),
                                  Eq(mac_attributes),
                                  Eq(StatusCode::SWSS_RC_SUCCESS), Eq(true)));
  EXPECT_CALL(mock_sai_ipmc_group_, create_ipmc_group(_, _, _, _))
      .WillOnce(DoAll(SetArgPointee<0>(kGroupOid1), Return(SAI_STATUS_SUCCESS)))
      .WillOnce(
          DoAll(SetArgPointee<0>(kGroupOid2), Return(SAI_STATUS_SUCCESS)));
  EXPECT_CALL(mock_sai_ipmc_group_, create_ipmc_group_member(_, _, _, _))
      .WillOnce(
          DoAll(SetArgPointee<0>(kGroupMemberOid1), Return(SAI_STATUS_SUCCESS)))
      .WillOnce(DoAll(SetArgPointee<0>(kGroupMemberOid2),
                      Return(SAI_STATUS_SUCCESS)));
  EXPECT_CALL(publisher_, publish(Eq(APP_P4RT_TABLE_NAME),
                                  Eq(group_appl_db_key1), Eq(group_attributes1),
                                  Eq(StatusCode::SWSS_RC_SUCCESS), Eq(true)));
  EXPECT_CALL(publisher_, publish(Eq(APP_P4RT_TABLE_NAME),
                                  Eq(group_appl_db_key2), Eq(group_attributes2),
                                  Eq(StatusCode::SWSS_RC_SUCCESS), Eq(true)));
  EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS, Drain(/*failure_before=*/false));

  // Add another multicast group member to group 1 (update).
  // Add third group.
  // Attempt to delete group 2, but force failure.
  std::vector<swss::FieldValueTuple> group_attributes1b;
  const std::string json_array1b = R"([{"multicast_replica_instance":"0x1",)"
                                   R"("multicast_replica_port":"Ethernet1"},)"
                                   R"({"multicast_replica_instance":"0x1",)"
                                   R"("multicast_replica_port":"Ethernet3"}])";
  group_attributes1b.push_back(swss::FieldValueTuple{"replicas", json_array1b});

  const std::string group_appl_db_key3 =
      std::string(APP_P4RT_REPLICATION_IP_MULTICAST_TABLE_NAME) +
      kTableKeyDelimiter + "0x3";
  std::vector<swss::FieldValueTuple> group_attributes3;
  const std::string json_array3 = R"([{"multicast_replica_instance":"0x1",)"
                                  R"("multicast_replica_port":"Ethernet3"}])";
  group_attributes3.push_back(swss::FieldValueTuple{"replicas", json_array3});

  std::vector<swss::FieldValueTuple> group_attributes_del = {};

  Enqueue(APP_P4RT_REPLICATION_IP_MULTICAST_TABLE_NAME,
          swss::KeyOpFieldsValuesTuple(group_appl_db_key1, SET_COMMAND,
                                       group_attributes1b));
  Enqueue(APP_P4RT_REPLICATION_IP_MULTICAST_TABLE_NAME,
          swss::KeyOpFieldsValuesTuple(group_appl_db_key3, SET_COMMAND,
                                       group_attributes3));
  // SAI delete failure
  Enqueue(APP_P4RT_REPLICATION_IP_MULTICAST_TABLE_NAME,
          swss::KeyOpFieldsValuesTuple(group_appl_db_key2, DEL_COMMAND,
                                       group_attributes_del));

  EXPECT_CALL(mock_sai_ipmc_group_, create_ipmc_group_member(_, _, _, _))
      .WillOnce(
          DoAll(SetArgPointee<0>(kGroupMemberOid3), Return(SAI_STATUS_SUCCESS)))
      .WillOnce(DoAll(SetArgPointee<0>(kGroupMemberOid4),
                      Return(SAI_STATUS_SUCCESS)));
  EXPECT_CALL(mock_sai_ipmc_group_, create_ipmc_group(_, _, _, _))
      .WillOnce(
          DoAll(SetArgPointee<0>(kGroupOid3), Return(SAI_STATUS_SUCCESS)));
  EXPECT_CALL(mock_sai_ipmc_group_, remove_ipmc_group_member(kGroupMemberOid2))
      .WillOnce(Return(SAI_STATUS_FAILURE));
  EXPECT_CALL(publisher_,
              publish(Eq(APP_P4RT_TABLE_NAME), Eq(group_appl_db_key1),
                      Eq(group_attributes1b), Eq(StatusCode::SWSS_RC_SUCCESS),
                      Eq(true)));
  EXPECT_CALL(publisher_, publish(Eq(APP_P4RT_TABLE_NAME),
                                  Eq(group_appl_db_key3), Eq(group_attributes3),
                                  Eq(StatusCode::SWSS_RC_SUCCESS), Eq(true)));
  EXPECT_CALL(publisher_,
              publish(Eq(APP_P4RT_TABLE_NAME), Eq(group_appl_db_key2),
                      Eq(group_attributes_del), Eq(StatusCode::SWSS_RC_UNKNOWN),
                      Eq(true)));
  EXPECT_EQ(StatusCode::SWSS_RC_UNKNOWN, Drain(/*failure_before=*/false));
}

TEST_F(L3MulticastManagerTest, DrainUnknownTable) {
  const std::string match_key =
      R"({"match/multicast_replica_port":"Ethernet4",)"
      R"("match/multicast_replica_instance":"0x1"})";
  // Unknown table (to this manager).
  const std::string appl_db_key =
      std::string(APP_P4RT_TUNNEL_TABLE_NAME) + kTableKeyDelimiter + match_key;
  std::vector<swss::FieldValueTuple> attributes;
  attributes.push_back(
      swss::FieldValueTuple{p4orch::kAction, p4orch::kSetMulticastSrcMac});
  attributes.push_back(
      swss::FieldValueTuple{prependParamField(p4orch::kSrcMac), kSrcMac2});
  attributes.push_back(
      swss::FieldValueTuple{p4orch::kControllerMetadata, "so_meta"});

  Enqueue(APP_P4RT_TUNNEL_TABLE_NAME,
          swss::KeyOpFieldsValuesTuple(appl_db_key, SET_COMMAND, attributes));
  EXPECT_CALL(publisher_,
              publish(Eq(APP_P4RT_TABLE_NAME), Eq(appl_db_key), Eq(attributes),
                      Eq(StatusCode::SWSS_RC_NOT_EXECUTED), Eq(true)));
  EXPECT_EQ(StatusCode::SWSS_RC_NOT_EXECUTED, Drain(/*failure_before=*/false));
}

TEST_F(L3MulticastManagerTest, DrainUnknownFirstTable) {
  const std::string match_key =
      R"({"match/multicast_replica_port":"Ethernet4",)"
      R"("match/multicast_replica_instance":"0x1"})";
  // Unknown table (to this manager).
  const std::string appl_db_key_unknown =
      std::string(APP_P4RT_TUNNEL_TABLE_NAME) + kTableKeyDelimiter + match_key;
  const std::string appl_db_key =
      std::string(APP_P4RT_MULTICAST_ROUTER_INTERFACE_TABLE_NAME) +
      kTableKeyDelimiter + match_key;
  std::vector<swss::FieldValueTuple> attributes;
  attributes.push_back(
      swss::FieldValueTuple{p4orch::kAction, p4orch::kSetMulticastSrcMac});
  attributes.push_back(
      swss::FieldValueTuple{prependParamField(p4orch::kSrcMac), kSrcMac2});
  attributes.push_back(
      swss::FieldValueTuple{p4orch::kControllerMetadata, "so_meta"});

  Enqueue(APP_P4RT_TUNNEL_TABLE_NAME,
          swss::KeyOpFieldsValuesTuple(appl_db_key_unknown, SET_COMMAND,
                                       attributes));
  Enqueue(APP_P4RT_MULTICAST_ROUTER_INTERFACE_TABLE_NAME,
          swss::KeyOpFieldsValuesTuple(appl_db_key, SET_COMMAND, attributes));

  EXPECT_CALL(
      publisher_,
      publish(Eq(APP_P4RT_TABLE_NAME), Eq(appl_db_key_unknown), Eq(attributes),
              Eq(StatusCode::SWSS_RC_NOT_EXECUTED), Eq(true)));
  EXPECT_CALL(publisher_,
              publish(Eq(APP_P4RT_TABLE_NAME), Eq(appl_db_key), Eq(attributes),
                      Eq(StatusCode::SWSS_RC_NOT_EXECUTED), Eq(true)));
  EXPECT_EQ(StatusCode::SWSS_RC_NOT_EXECUTED, Drain(/*failure_before=*/false));
}

TEST_F(L3MulticastManagerTest, VerifyStateMulticastGroupTestSuccess) {
  // Add router interface entry so have RIF.
  auto rif_entry = SetupP4MulticastRouterInterfaceEntry(
      "Ethernet1", "0x1", swss::MacAddress(kSrcMac1), kRifOid1);
  // Add multicast group.
  P4Replica replica1 = P4Replica("0x1", "Ethernet1", "0x1");
  auto group_entry = SetupP4MulticastGroupEntry("0x1", {replica1}, kGroupOid1,
                                                {kGroupMemberOid1});

  const std::string appl_db_key =
      std::string(APP_P4RT_REPLICATION_IP_MULTICAST_TABLE_NAME) +
      kTableKeyDelimiter + "0x1";
  const std::string db_key =
      std::string(APP_P4RT_TABLE_NAME) + kTableKeyDelimiter + appl_db_key;
  std::vector<swss::FieldValueTuple> attributes;
  const std::string json_array = R"([{"multicast_replica_instance":"0x1",)"
                                 R"("multicast_replica_port":"Ethernet1"}])";
  attributes.push_back(swss::FieldValueTuple{"replicas", json_array});

  // Setup ASIC DB.
  swss::Table table(nullptr, "ASIC_STATE");
  table.set("SAI_OBJECT_TYPE_IPMC_GROUP:oid:0x1",
            std::vector<swss::FieldValueTuple>{});
  table.set(
      "SAI_OBJECT_TYPE_IPMC_GROUP_MEMBER:oid:0x11",
      std::vector<swss::FieldValueTuple>{
          swss::FieldValueTuple{"SAI_IPMC_GROUP_MEMBER_ATTR_IPMC_GROUP_ID",
                                "oid:0x1"},
          swss::FieldValueTuple{"SAI_IPMC_GROUP_MEMBER_ATTR_IPMC_OUTPUT_ID",
                                "oid:0x123456"}});

  // Verification should succeed with vaild key and value.
  EXPECT_EQ(VerifyState(db_key, attributes), "");
  table.del("SAI_OBJECT_TYPE_IPMC_GROUP:oid:0x1");
  table.del("SAI_OBJECT_TYPE_IPMC_GROUP_MEMBER:oid:0x11");
}

TEST_F(L3MulticastManagerTest, VerifyStateMulticastGroupTestBadEntries) {
  const std::string appl_db_key =
      std::string(APP_P4RT_REPLICATION_IP_MULTICAST_TABLE_NAME) +
      kTableKeyDelimiter + "0x1";
  const std::string db_key =
      std::string(APP_P4RT_TABLE_NAME) + kTableKeyDelimiter + appl_db_key;

  std::vector<swss::FieldValueTuple> bad_attributes;
  const std::string json_array_bad = R"([{"multicast_replica_instance":"0x1"])";
  bad_attributes.push_back(swss::FieldValueTuple{"replicas", json_array_bad});

  std::vector<swss::FieldValueTuple> good_attributes;
  const std::string json_array_good =
      R"([{"multicast_replica_instance":"0x1",)"
      R"("multicast_replica_port":"Ethernet1"}])";
  good_attributes.push_back(swss::FieldValueTuple{"replicas", json_array_good});

  EXPECT_FALSE(VerifyState(db_key, bad_attributes).empty());
  // No entry found.
  EXPECT_FALSE(VerifyState(db_key, good_attributes).empty());
}

TEST_F(L3MulticastManagerTest, VerifyStateMulticastGroupTestStateCacheFails) {
  // Need RIFs to be able to validate entries.
  auto rif_entry1 = SetupP4MulticastRouterInterfaceEntry(
      "Ethernet1", "0x0", swss::MacAddress(kSrcMac1), kRifOid1);
  auto rif_entry2 = SetupP4MulticastRouterInterfaceEntry(
      "Ethernet1", "0x1", swss::MacAddress(kSrcMac1), kRifOid1,
      /*expect_mock=*/false);
  auto rif_entry3 = SetupP4MulticastRouterInterfaceEntry(
      "Ethernet2", "0x0", swss::MacAddress(kSrcMac1), kRifOid2);

  P4Replica replica1 = P4Replica("0x1", "Ethernet1", "0x0");
  P4Replica replica2 = P4Replica("0x1", "Ethernet2", "0x0");
  P4Replica replica3 = P4Replica("0x1", "Ethernet1", "0x1");
  P4MulticastGroupEntry internal_entry = GenerateP4MulticastGroupEntry(
      "0x1", {replica1, replica2}, "multi_meta", "controller_meta");
  internal_entry.multicast_group_oid = kGroupOid1;
  internal_entry.member_oids[replica1.key] = kGroupMemberOid1;
  internal_entry.member_oids[replica2.key] = kGroupMemberOid2;
  p4_oid_mapper_.setOID(SAI_OBJECT_TYPE_IPMC_GROUP, "0x1", kGroupOid1);
  p4_oid_mapper_.setOID(SAI_OBJECT_TYPE_IPMC_GROUP_MEMBER, replica1.key,
                        kGroupMemberOid1);
  p4_oid_mapper_.setOID(SAI_OBJECT_TYPE_IPMC_GROUP_MEMBER, replica2.key,
                        kGroupMemberOid2);

  // Empty multicast group ID.
  P4MulticastGroupEntry empty_group_id = GenerateP4MulticastGroupEntry(
      "", {replica1}, "multi_meta", "controller_meta");
  EXPECT_FALSE(
      VerifyMulticastGroupStateCache(empty_group_id, &internal_entry).empty());

  // Missing replica.
  P4MulticastGroupEntry missing_replica = GenerateP4MulticastGroupEntry(
      "0x1", {replica1}, "multi_meta", "controller_meta");
  EXPECT_FALSE(
      VerifyMulticastGroupStateCache(missing_replica, &internal_entry).empty());

  P4MulticastGroupEntry missing_replica2 = GenerateP4MulticastGroupEntry(
      "0x1", {replica1, replica2, replica3}, "multi_meta", "controller_meta");
  EXPECT_FALSE(VerifyMulticastGroupStateCache(missing_replica2, &internal_entry)
                   .empty());

  P4MulticastGroupEntry missing_replica3 = GenerateP4MulticastGroupEntry(
      "0x1", {replica1, replica2}, "multi_meta", "controller_meta");
  // Remove from OID map (and then put back).
  internal_entry.member_oids.erase(replica2.key);
  EXPECT_FALSE(VerifyMulticastGroupStateCache(missing_replica3, &internal_entry)
                   .empty());
  internal_entry.member_oids[replica2.key] = kGroupMemberOid2;
  // Change internal OID map (and then put restore).
  p4_oid_mapper_.eraseOID(SAI_OBJECT_TYPE_IPMC_GROUP_MEMBER, replica2.key);
  p4_oid_mapper_.setOID(SAI_OBJECT_TYPE_IPMC_GROUP_MEMBER, replica2.key,
                        kGroupMemberOid3);
  EXPECT_FALSE(VerifyMulticastGroupStateCache(missing_replica3, &internal_entry)
                   .empty());
  p4_oid_mapper_.eraseOID(SAI_OBJECT_TYPE_IPMC_GROUP_MEMBER, replica2.key);
  p4_oid_mapper_.setOID(SAI_OBJECT_TYPE_IPMC_GROUP_MEMBER, replica2.key,
                        kGroupMemberOid2);

  // Different replicas.
  P4MulticastGroupEntry different_replicas = GenerateP4MulticastGroupEntry(
      "0x1", {replica2, replica3}, "multi_meta", "controller_meta");
  EXPECT_FALSE(
      VerifyMulticastGroupStateCache(different_replicas, &internal_entry)
          .empty());

  // Mismatch on key.
  P4MulticastGroupEntry key_mismatch = GenerateP4MulticastGroupEntry(
      "0x2", {replica1, replica2}, "multi_meta", "controller_meta");
  EXPECT_FALSE(
      VerifyMulticastGroupStateCache(key_mismatch, &internal_entry).empty());

  // Mismatch on multicast_group_id.
  P4MulticastGroupEntry group_id_mismatch = GenerateP4MulticastGroupEntry(
      "0x1", {replica1, replica2}, "multi_meta", "controller_meta");
  group_id_mismatch.multicast_group_id = "0x2";
  EXPECT_FALSE(
      VerifyMulticastGroupStateCache(group_id_mismatch, &internal_entry)
          .empty());

  // Mismatch on multicast metadata.
  P4MulticastGroupEntry multicast_metadata_mismatch =
      GenerateP4MulticastGroupEntry("0x1", {replica1, replica2}, "mismatch",
                                    "controller_meta");
  EXPECT_FALSE(VerifyMulticastGroupStateCache(multicast_metadata_mismatch,
                                              &internal_entry)
                   .empty());

  // Mismatch on controller metadata.
  P4MulticastGroupEntry controller_metdata_mismatch =
      GenerateP4MulticastGroupEntry("0x1", {replica1, replica2}, "multi_meta",
                                    "mismatch");
  EXPECT_FALSE(VerifyMulticastGroupStateCache(controller_metdata_mismatch,
                                              &internal_entry)
                   .empty());
}

TEST_F(L3MulticastManagerTest, VerifyStateMulticastGroupMissingAsicDb) {
  // Need RIFs to be able to validate entries.
  auto rif_entry1 = SetupP4MulticastRouterInterfaceEntry(
      "Ethernet1", "0x1", swss::MacAddress(kSrcMac1), kRifOid1);
  // Add group entry.
  P4Replica replica1 = P4Replica("0x1", "Ethernet1", "0x1");
  auto group_entry = SetupP4MulticastGroupEntry("0x1", {replica1}, kGroupOid1,
                                                {kGroupMemberOid1});

  const std::string appl_db_key =
      std::string(APP_P4RT_REPLICATION_IP_MULTICAST_TABLE_NAME) +
      kTableKeyDelimiter + "0x1";
  const std::string db_key =
      std::string(APP_P4RT_TABLE_NAME) + kTableKeyDelimiter + appl_db_key;
  std::vector<swss::FieldValueTuple> attributes;
  const std::string json_array = R"([{"multicast_replica_instance":"0x1",)"
                                 R"("multicast_replica_port":"Ethernet1"}])";
  attributes.push_back(swss::FieldValueTuple{"replicas", json_array});

  // Setup ASIC DB.
  swss::Table table(nullptr, "ASIC_STATE");
  table.set("SAI_OBJECT_TYPE_IPMC_GROUP:oid:0x1",
            std::vector<swss::FieldValueTuple>{});
  table.set(
      "SAI_OBJECT_TYPE_IPMC_GROUP_MEMBER:oid:0x11",
      std::vector<swss::FieldValueTuple>{
          swss::FieldValueTuple{"SAI_IPMC_GROUP_MEMBER_ATTR_IPMC_GROUP_ID",
                                "oid:0x2"},  // this is wrong OID
          swss::FieldValueTuple{"SAI_IPMC_GROUP_MEMBER_ATTR_IPMC_OUTPUT_ID",
                                "oid:0x123456"}});

  // Verification should fail, since ASIC DB attribute is wrong.
  EXPECT_FALSE(VerifyState(db_key, attributes).empty());

  // No key should also fail.
  table.del("SAI_OBJECT_TYPE_IPMC_GROUP:oid:0x1");
  EXPECT_FALSE(VerifyState(db_key, attributes).empty());

  // Reset group, but delete group member
  table.set("SAI_OBJECT_TYPE_IPMC_GROUP:oid:0x1",
            std::vector<swss::FieldValueTuple>{});
  table.del("SAI_OBJECT_TYPE_IPMC_GROUP_MEMBER:oid:0x11");
  // No key should also fail.
  EXPECT_FALSE(VerifyState(db_key, attributes).empty());

  // Reset group member, add extra attribute for group.
  table.set("SAI_OBJECT_TYPE_IPMC_GROUP:oid:0x1",
            std::vector<swss::FieldValueTuple>{swss::FieldValueTuple{
                "SAI_IPMC_SOMETHING_EXTRA", "oid:0x123456"}});
  table.set(
      "SAI_OBJECT_TYPE_IPMC_GROUP_MEMBER:oid:0x11",
      std::vector<swss::FieldValueTuple>{
          swss::FieldValueTuple{"SAI_IPMC_GROUP_MEMBER_ATTR_IPMC_GROUP_ID",
                                "oid:0x1"},
          swss::FieldValueTuple{"SAI_IPMC_GROUP_MEMBER_ATTR_IPMC_OUTPUT_ID",
                                "oid:0x123456"}});
  EXPECT_FALSE(VerifyState(db_key, attributes).empty());
  table.del("SAI_OBJECT_TYPE_IPMC_GROUP:oid:0x1");
  table.del("SAI_OBJECT_TYPE_IPMC_GROUP_MEMBER:oid:0x11");
}

TEST_F(L3MulticastManagerTest, VerifyStateMulticastGroupAsicDbNoRif) {
  // Need RIFs to setup multicast group.
  auto rif_entry1 = SetupP4MulticastRouterInterfaceEntry(
      "Ethernet1", "0x1", swss::MacAddress(kSrcMac1), kRifOid1);
  P4Replica replica1 = P4Replica("0x1", "Ethernet1", "0x1");
  auto entry = SetupP4MulticastGroupEntry("0x1", {replica1}, kGroupOid1,
                                          {kGroupMemberOid1});

  // Force-remove the OID.
  std::string rif_key = KeyGenerator::generateMulticastRouterInterfaceRifKey(
      rif_entry1.multicast_replica_port, rif_entry1.src_mac);
  ForceRemoveRifKey(rif_key);

  // Setup ASIC DB.
  swss::Table table(nullptr, "ASIC_STATE");
  table.set("SAI_OBJECT_TYPE_IPMC_GROUP:oid:0x1",
            std::vector<swss::FieldValueTuple>{});

  auto* group_entry_ptr = GetMulticastGroupEntry("0x1");
  ASSERT_NE(group_entry_ptr, nullptr);
  EXPECT_FALSE(VerifyMulticastGroupStateAsicDb(group_entry_ptr).empty());
  table.del("SAI_OBJECT_TYPE_IPMC_GROUP:oid:0x1");
}

TEST_F(L3MulticastManagerTest, VerifyStateMulticastGroupFailures) {
  // Need RIFs to be able to validate entries.
  auto rif_entry1 = SetupP4MulticastRouterInterfaceEntry(
      "Ethernet1", "0x1", swss::MacAddress(kSrcMac1), kRifOid1);
  // Add group entry.
  P4Replica replica1 = P4Replica("0x1", "Ethernet1", "0x1");
  auto group_entry = SetupP4MulticastGroupEntry("0x1", {replica1}, kGroupOid1,
                                                {kGroupMemberOid1});

  const std::string appl_db_key =
      std::string(APP_P4RT_REPLICATION_IP_MULTICAST_TABLE_NAME) +
      kTableKeyDelimiter + "0x1";
  const std::string db_key =
      std::string(APP_P4RT_TABLE_NAME) + kTableKeyDelimiter + appl_db_key;
  std::vector<swss::FieldValueTuple> attributes;
  const std::string json_array = R"([{"multicast_replica_instance":"0x1",)"
                                 R"("multicast_replica_port":"Ethernet1"}])";
  attributes.push_back(swss::FieldValueTuple{"replicas", json_array});

  // Setup ASIC DB.
  swss::Table table(nullptr, "ASIC_STATE");
  table.set("SAI_OBJECT_TYPE_IPMC_GROUP:oid:0x1",
            std::vector<swss::FieldValueTuple>{});
  table.set(
      "SAI_OBJECT_TYPE_IPMC_GROUP_MEMBER:oid:0x11",
      std::vector<swss::FieldValueTuple>{
          swss::FieldValueTuple{"SAI_IPMC_GROUP_MEMBER_ATTR_IPMC_GROUP_ID",
                                "oid:0x1"},
          swss::FieldValueTuple{"SAI_IPMC_GROUP_MEMBER_ATTR_IPMC_OUTPUT_ID",
                                "oid:0x123456"}});

  // Force state cache failure by removing oid from mapper.
  p4_oid_mapper_.eraseOID(SAI_OBJECT_TYPE_IPMC_GROUP, "0x1");

  // Verification should fail, since forced invalid state cache, ASIC DB is ok.
  EXPECT_FALSE(VerifyState(db_key, attributes).empty());

  // Force ASIC DB to be bad also.
  table.del("SAI_OBJECT_TYPE_IPMC_GROUP:oid:0x1");
  EXPECT_FALSE(VerifyState(db_key, attributes).empty());
  table.del("SAI_OBJECT_TYPE_IPMC_GROUP_MEMBER:oid:0x11");
}

}  // namespace p4orch
