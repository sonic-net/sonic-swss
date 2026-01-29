#include "ip_multicast_manager.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <functional>
#include <map>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#include "ipaddress.h"
#include "mock_response_publisher.h"
#include "mock_sai_ipmc.h"
#include "mock_sai_router_interface.h"
#include "mock_sai_rpf_group.h"
#include "p4orch.h"
#include "p4orch/p4orch_util.h"
#include "portsorch.h"
#include "return_code.h"
#include "swssnet.h"
#include "vrforch.h"

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
extern char* gVrfName;
extern size_t gMaxBulkSize;
extern sai_ipmc_api_t* sai_ipmc_api;
extern sai_router_interface_api_t* sai_router_intfs_api;
extern sai_rpf_group_api_t* sai_rpf_group_api;
extern PortsOrch* gPortsOrch;
extern VRFOrch* gVrfOrch;

namespace p4orch {

namespace {

constexpr char* kIpv4Address1 = "225.11.12.0";
constexpr char* kIpv6Address1 = "ff00::2001:db8:1";
constexpr char* kMulticastGroup1 = "0x1";
constexpr char* kMulticastGroup2 = "0x2";
constexpr char* kMulticastGroup3 = "0x3";
constexpr char* kMulticastGroup4 = "0x4";
constexpr sai_object_id_t kMulticastGroupOid1 = 0x101;
constexpr sai_object_id_t kMulticastGroupOid2 = 0x102;
constexpr sai_object_id_t kMulticastGroupOid3 = 0x103;
constexpr sai_object_id_t kMulticastGroupOid4 = 0x104;

constexpr sai_object_id_t kRpfGroupOid1 = 0x77;
constexpr sai_object_id_t kRpfGroupMemberOid1 = 0x88;
constexpr sai_object_id_t kRpfRouterInterfaceOid1 = 0x99;

bool AddressCmp(const sai_ip_address_t* x, const sai_ip_address_t* y) {
  if (x->addr_family != y->addr_family) {
    return false;
  }
  if (x->addr_family == SAI_IP_ADDR_FAMILY_IPV4) {
    return memcmp(&x->addr.ip4, &y->addr.ip4, sizeof(sai_ip4_t)) == 0;
  }
  return memcmp(&x->addr.ip6, &y->addr.ip6, sizeof(sai_ip6_t)) == 0;
}

// Matches two SAI attributes.
bool MatchSaiAttribute(const sai_attribute_t& attr,
                       const sai_attribute_t& exp_attr) {
  if (exp_attr.id == SAI_ROUTE_ENTRY_ATTR_PACKET_ACTION) {
    if (attr.id != SAI_ROUTE_ENTRY_ATTR_PACKET_ACTION ||
        attr.value.s32 != exp_attr.value.s32) {
      return false;
    }
  }
  if (exp_attr.id == SAI_IPMC_ENTRY_ATTR_OUTPUT_GROUP_ID) {
    if (attr.id != SAI_IPMC_ENTRY_ATTR_OUTPUT_GROUP_ID ||
        attr.value.oid != exp_attr.value.oid) {
      return false;
    }
  }
  if (exp_attr.id == SAI_IPMC_ENTRY_ATTR_RPF_GROUP_ID) {
    if (attr.id != SAI_IPMC_ENTRY_ATTR_RPF_GROUP_ID ||
        attr.value.oid != exp_attr.value.oid) {
      return false;
    }
  }
  return true;
}

MATCHER_P(ArrayEq, array, "") {
  for (size_t i = 0; i < array.size(); ++i) {
    if (arg[i] != array[i]) {
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

void VerifyP4IpMulticastEntryEqual(const P4IpMulticastEntry& x,
                                   const P4IpMulticastEntry& y) {
  EXPECT_EQ(x.ip_multicast_entry_key, y.ip_multicast_entry_key);
  EXPECT_EQ(x.vrf_id, y.vrf_id);
  EXPECT_EQ(x.ip_dst, y.ip_dst);
  EXPECT_EQ(x.action, y.action);
  EXPECT_EQ(x.multicast_group_id, y.multicast_group_id);
  EXPECT_EQ(x.controller_metadata, y.controller_metadata);
  EXPECT_TRUE(
      AddressCmp(&x.sai_ipmc_entry.destination, &y.sai_ipmc_entry.destination));
  EXPECT_TRUE(AddressCmp(&x.sai_ipmc_entry.source, &y.sai_ipmc_entry.source));
  EXPECT_EQ(x.sai_ipmc_entry.switch_id, x.sai_ipmc_entry.switch_id);
  EXPECT_EQ(x.sai_ipmc_entry.vr_id, x.sai_ipmc_entry.vr_id);
  EXPECT_EQ(x.sai_ipmc_entry.type, x.sai_ipmc_entry.type);
}

}  // namespace

class IpMulticastManagerTest : public ::testing::Test {
 protected:
  IpMulticastManagerTest()
      : ip_multicast_manager_(&p4_oid_mapper_, gVrfOrch, &publisher_) {}

  void SetUp() override {
    mock_sai_ipmc = &mock_sai_ipmc_;
    sai_ipmc_api->create_ipmc_entry = mock_create_ipmc_entry;
    sai_ipmc_api->remove_ipmc_entry = mock_remove_ipmc_entry;
    sai_ipmc_api->set_ipmc_entry_attribute = mock_set_ipmc_entry_attribute;
    sai_ipmc_api->get_ipmc_entry_attribute = mock_get_ipmc_entry_attribute;
    mock_sai_rpf_group = &mock_sai_rpf_group_;
    sai_rpf_group_api->create_rpf_group = mock_create_rpf_group;

    mock_sai_router_intf = &mock_sai_router_intf_;
    sai_router_intfs_api->create_router_interface =
        mock_create_router_interface;
  }

  ReturnCodeOr<P4IpMulticastEntry> DeserializeIpMulticastEntry(
      const std::string& key,
      const std::vector<swss::FieldValueTuple>& attributes,
      const std::string& table_name) {
    return ip_multicast_manager_.deserializeIpMulticastEntry(key, attributes,
                                                             table_name);
  }

  std::string VerifyState(const std::string& key,
                          const std::vector<swss::FieldValueTuple>& tuples) {
    return ip_multicast_manager_.verifyState(key, tuples);
  }

  void Enqueue(const std::string& table_name,
               const swss::KeyOpFieldsValuesTuple& entry) {
    ip_multicast_manager_.enqueue(table_name, entry);
  }

  ReturnCode Drain(bool failure_before) {
    if (failure_before) {
      ip_multicast_manager_.drainWithNotExecuted();
      return ReturnCode(StatusCode::SWSS_RC_NOT_EXECUTED);
    }
    return ip_multicast_manager_.drain();
  }

  P4IpMulticastEntry* GetIpMulticastEntry(
      const std::string& ip_multicast_entry_key) {
    return ip_multicast_manager_.getIpMulticastEntry(ip_multicast_entry_key);
  }

  // Function to fake adding a multicast group SAI object.
  void AddMulticastGroup(const std::string multicast_group_id,
                         const sai_object_id_t group_oid) {
    p4_oid_mapper_.setOID(SAI_OBJECT_TYPE_IPMC_GROUP, multicast_group_id,
                          group_oid);
  }

  std::vector<ReturnCode> CreateIpMulticastEntries(
      const std::vector<P4IpMulticastEntry>& ip_multicast_entries) {
    return ip_multicast_manager_.createIpMulticastEntries(ip_multicast_entries);
  }

  std::vector<ReturnCode> UpdateIpMulticastEntries(
      const std::vector<P4IpMulticastEntry>& ip_multicast_entries) {
    return ip_multicast_manager_.updateIpMulticastEntries(ip_multicast_entries);
  }

  std::vector<ReturnCode> DeleteIpMulticastEntries(
      const std::vector<P4IpMulticastEntry>& ip_multicast_entries) {
    return ip_multicast_manager_.deleteIpMulticastEntries(ip_multicast_entries);
  }

  // Generates a P4IpMulticastEntry.
  P4IpMulticastEntry GenerateP4IpMulticastEntry(
      const std::string& vrf_id, const swss::IpAddress& ip_dst,
      const std::string& action, const std::string& action_param,
      const std::string& metadata = "") {
    P4IpMulticastEntry ip_multicast_entry = {};
    ip_multicast_entry.vrf_id = vrf_id;
    ip_multicast_entry.ip_dst = ip_dst;
    ip_multicast_entry.action = action;
    if (action == p4orch::kSetMulticastGroupId) {
      ip_multicast_entry.multicast_group_id = action_param;
    }
    ip_multicast_entry.controller_metadata = metadata;
    ip_multicast_entry.ip_multicast_entry_key =
        KeyGenerator::generateIpMulticastKey(ip_multicast_entry.vrf_id,
                                             ip_multicast_entry.ip_dst);
    return ip_multicast_entry;
  }

  // Creates and adds an IP multicast entry for test.
  P4IpMulticastEntry SetupIpMulticastEntry(
      const std::string& vrf_id, const swss::IpAddress& ip_dst,
      const std::string& multicast_group_id,
      const sai_object_id_t multicast_group_oid,
      const std::string& metadata = "", bool expect_rpf = true) {
    auto ip_multicast_entry =
        GenerateP4IpMulticastEntry(vrf_id, ip_dst, p4orch::kSetMulticastGroupId,
                                   multicast_group_id, metadata);
    // Create artificial multicast group object.
    AddMulticastGroup(multicast_group_id, multicast_group_oid);
    if (expect_rpf) {
      EXPECT_CALL(mock_sai_router_intf_, create_router_interface(_, _, 7, _))
          .WillOnce(DoAll(SetArgPointee<0>(kRpfRouterInterfaceOid1),
                          Return(SAI_STATUS_SUCCESS)));
      EXPECT_CALL(mock_sai_rpf_group_, create_rpf_group(_, _, 0, _))
          .WillOnce(DoAll(SetArgPointee<0>(kRpfGroupOid1),
                          Return(SAI_STATUS_SUCCESS)));
      EXPECT_CALL(mock_sai_rpf_group_, create_rpf_group_member(_, _, 2, _))
          .WillOnce(DoAll(SetArgPointee<0>(kRpfGroupMemberOid1),
                          Return(SAI_STATUS_SUCCESS)));
    }

    std::vector<sai_attribute_t> exp_ipmc_attrs;
    sai_attribute_t attr;
    attr.id = SAI_IPMC_ENTRY_ATTR_PACKET_ACTION;
    attr.value.s32 = SAI_PACKET_ACTION_FORWARD;
    exp_ipmc_attrs.push_back(attr);
    attr.id = SAI_IPMC_ENTRY_ATTR_OUTPUT_GROUP_ID;
    attr.value.oid = multicast_group_oid;
    exp_ipmc_attrs.push_back(attr);
    attr.id = SAI_IPMC_ENTRY_ATTR_RPF_GROUP_ID;
    attr.value.oid = kRpfGroupOid1;
    exp_ipmc_attrs.push_back(attr);
    EXPECT_CALL(mock_sai_ipmc_,
                create_ipmc_entry(_, 3, AttrArrayEq(exp_ipmc_attrs)))
        .WillOnce(Return(SAI_STATUS_SUCCESS));

    EXPECT_THAT(CreateIpMulticastEntries(
                    std::vector<P4IpMulticastEntry>{ip_multicast_entry}),
                ArrayEq(std::vector<StatusCode>{StatusCode::SWSS_RC_SUCCESS}));
    return ip_multicast_entry;
  }

  StrictMock<MockSaiIpmc> mock_sai_ipmc_;
  StrictMock<MockSaiRouterInterface> mock_sai_router_intf_;
  StrictMock<MockSaiRpfGroup> mock_sai_rpf_group_;
  StrictMock<MockResponsePublisher> publisher_;
  P4OidMapper p4_oid_mapper_;
  IpMulticastManager ip_multicast_manager_;
};

TEST_F(IpMulticastManagerTest, DeserializeIpMulticastEntryIpv4Success) {
  std::string key = R"({"match/vrf_id":"ipv4_multicast",)"
                    R"("match/ipv4_dst":"224.2.3.4"})";
  std::vector<swss::FieldValueTuple> attributes;
  attributes.push_back(
      swss::FieldValueTuple{p4orch::kAction, p4orch::kSetMulticastGroupId});
  attributes.push_back(swss::FieldValueTuple{
      prependParamField(p4orch::kMulticastGroupId), "0x1"});
  attributes.push_back(
      swss::FieldValueTuple{p4orch::kControllerMetadata, "cmeta"});

  auto ip_multicast_entry_or = DeserializeIpMulticastEntry(
      key, attributes, APP_P4RT_IPV4_MULTICAST_TABLE_NAME);
  EXPECT_TRUE(ip_multicast_entry_or.ok());
  auto& ip_multicast_entry = *ip_multicast_entry_or;
  auto expect_entry =
      GenerateP4IpMulticastEntry("ipv4_multicast", swss::IpAddress("224.2.3.4"),
                                 kSetMulticastGroupId, "0x1", "cmeta");
  VerifyP4IpMulticastEntryEqual(expect_entry, ip_multicast_entry);
}

TEST_F(IpMulticastManagerTest, DeserializeIpMulticastEntryIpv6Success) {
  std::string key = R"({"match/vrf_id":"ipv6_multicast",)"
                    R"("match/ipv6_dst":"2001:db8:3:4:5:6:7:8"})";
  std::vector<swss::FieldValueTuple> attributes;
  attributes.push_back(
      swss::FieldValueTuple{p4orch::kAction, p4orch::kSetMulticastGroupId});
  attributes.push_back(swss::FieldValueTuple{
      prependParamField(p4orch::kMulticastGroupId), "0x1"});
  attributes.push_back(
      swss::FieldValueTuple{p4orch::kControllerMetadata, "cmeta"});

  auto ip_multicast_entry_or = DeserializeIpMulticastEntry(
      key, attributes, APP_P4RT_IPV6_MULTICAST_TABLE_NAME);
  EXPECT_TRUE(ip_multicast_entry_or.ok());
  auto& ip_multicast_entry = *ip_multicast_entry_or;
  auto expect_entry = GenerateP4IpMulticastEntry(
      "ipv6_multicast", swss::IpAddress("2001:db8:3:4:5:6:7:8"),
      kSetMulticastGroupId, "0x1", "cmeta");
  VerifyP4IpMulticastEntryEqual(expect_entry, ip_multicast_entry);
}

TEST_F(IpMulticastManagerTest, DeserializeIpMulticastEntryMissingAddressFails) {
  std::string key = R"({"match/vrf_id":"ipv4_multicast"})";
  std::vector<swss::FieldValueTuple> attributes;
  attributes.push_back(
      swss::FieldValueTuple{p4orch::kAction, p4orch::kSetMulticastGroupId});
  attributes.push_back(swss::FieldValueTuple{
      prependParamField(p4orch::kMulticastGroupId), "0x1"});

  auto ip_multicast_entry_or = DeserializeIpMulticastEntry(
      key, attributes, APP_P4RT_IPV4_MULTICAST_TABLE_NAME);
  EXPECT_FALSE(ip_multicast_entry_or.ok());
  EXPECT_EQ(StatusCode::SWSS_RC_INVALID_PARAM, ip_multicast_entry_or.status());
}

TEST_F(IpMulticastManagerTest, DeserializeIpMulticastEntryMissingVrfFails) {
  std::string key = R"({"match/ipv6_dst":"2001:db8:3:4:5:6:7:8"})";
  std::vector<swss::FieldValueTuple> attributes;
  attributes.push_back(
      swss::FieldValueTuple{p4orch::kAction, p4orch::kSetMulticastGroupId});
  attributes.push_back(swss::FieldValueTuple{
      prependParamField(p4orch::kMulticastGroupId), "0x1"});

  auto ip_multicast_entry_or = DeserializeIpMulticastEntry(
      key, attributes, APP_P4RT_IPV6_MULTICAST_TABLE_NAME);
  EXPECT_FALSE(ip_multicast_entry_or.ok());
  EXPECT_EQ(StatusCode::SWSS_RC_INVALID_PARAM, ip_multicast_entry_or.status());
}

TEST_F(IpMulticastManagerTest, DeserializeIpMulticastEntryInvalidAddressFails) {
  std::string key = R"({"match/vrf_id":"ipv4_multicast",)"
                    R"("match/ipv4_dst":"300.2.3.4"})";
  std::vector<swss::FieldValueTuple> attributes;
  attributes.push_back(
      swss::FieldValueTuple{p4orch::kAction, p4orch::kSetMulticastGroupId});
  attributes.push_back(swss::FieldValueTuple{
      prependParamField(p4orch::kMulticastGroupId), "0x1"});

  auto ip_multicast_entry_or = DeserializeIpMulticastEntry(
      key, attributes, APP_P4RT_IPV4_MULTICAST_TABLE_NAME);
  EXPECT_FALSE(ip_multicast_entry_or.ok());
  EXPECT_EQ(StatusCode::SWSS_RC_INVALID_PARAM, ip_multicast_entry_or.status());
}

TEST_F(IpMulticastManagerTest, DeserializeIpMulticastEntryExtraFieldFails) {
  std::string key = R"({"match/vrf_id":"ipv4_multicast",)"
                    R"("match/ipv4_dst":"224.2.3.4"})";
  std::vector<swss::FieldValueTuple> attributes;
  attributes.push_back(
      swss::FieldValueTuple{p4orch::kAction, p4orch::kSetMulticastGroupId});
  attributes.push_back(swss::FieldValueTuple{
      prependParamField(p4orch::kMulticastGroupId), "0x1"});
  attributes.push_back(
      swss::FieldValueTuple{p4orch::kControllerMetadata, "cmeta"});
  attributes.push_back(swss::FieldValueTuple{"extra", "unknown"});

  auto ip_multicast_entry_or = DeserializeIpMulticastEntry(
      key, attributes, APP_P4RT_IPV4_MULTICAST_TABLE_NAME);
  EXPECT_FALSE(ip_multicast_entry_or.ok());
  EXPECT_EQ(StatusCode::SWSS_RC_INVALID_PARAM, ip_multicast_entry_or.status());
}

TEST_F(IpMulticastManagerTest, CreateIpMulticastEntriesSuccess) {
  auto swss_ipv4_address = swss::IpAddress(kIpv4Address1);
  auto entry1 =
      SetupIpMulticastEntry(gVrfName, swss_ipv4_address, kMulticastGroup1,
                            kMulticastGroupOid1, "meta_ipv4");
  auto swss_ipv6_address = swss::IpAddress(kIpv6Address1);
  auto entry2 = SetupIpMulticastEntry(gVrfName, swss_ipv6_address,
                                      kMulticastGroup2, kMulticastGroupOid2,
                                      "meta_ipv6", /*expect_rpf=*/false);

  auto* entry1_ptr = GetIpMulticastEntry(entry1.ip_multicast_entry_key);
  auto* entry2_ptr = GetIpMulticastEntry(entry2.ip_multicast_entry_key);
  EXPECT_NE(entry1_ptr, nullptr);
  EXPECT_NE(entry2_ptr, nullptr);

  auto expect_ipv4 = GenerateP4IpMulticastEntry(gVrfName, swss_ipv4_address,
                                                p4orch::kSetMulticastGroupId,
                                                kMulticastGroup1, "meta_ipv4");
  expect_ipv4.sai_ipmc_entry.switch_id = gSwitchId;
  expect_ipv4.sai_ipmc_entry.vr_id = gVrfOrch->getVRFid(gVrfName);
  expect_ipv4.sai_ipmc_entry.type = SAI_IPMC_ENTRY_TYPE_XG;
  sai_ip_address_t sai_address_v4;
  copy(sai_address_v4, swss_ipv4_address);
  expect_ipv4.sai_ipmc_entry.destination.addr_family = SAI_IP_ADDR_FAMILY_IPV4;
  expect_ipv4.sai_ipmc_entry.destination.addr.ip4 = sai_address_v4.addr.ip4;
  expect_ipv4.sai_ipmc_entry.source.addr_family = SAI_IP_ADDR_FAMILY_IPV4;
  expect_ipv4.sai_ipmc_entry.source.addr.ip4 = 0;
  VerifyP4IpMulticastEntryEqual(expect_ipv4, *entry1_ptr);

  auto expect_ipv6 = GenerateP4IpMulticastEntry(gVrfName, swss_ipv6_address,
                                                p4orch::kSetMulticastGroupId,
                                                kMulticastGroup2, "meta_ipv6");
  expect_ipv6.sai_ipmc_entry.switch_id = gSwitchId;
  expect_ipv6.sai_ipmc_entry.vr_id = gVrfOrch->getVRFid(gVrfName);
  expect_ipv6.sai_ipmc_entry.type = SAI_IPMC_ENTRY_TYPE_XG;
  sai_ip_address_t sai_address_v6;
  copy(sai_address_v6, swss_ipv6_address);
  expect_ipv6.sai_ipmc_entry.destination.addr_family = SAI_IP_ADDR_FAMILY_IPV6;
  memcpy(&expect_ipv6.sai_ipmc_entry.destination.addr.ip6,
         &sai_address_v6.addr.ip6, sizeof(sai_ip6_t));
  expect_ipv6.sai_ipmc_entry.source.addr_family = SAI_IP_ADDR_FAMILY_IPV6;
  memset(&expect_ipv6.sai_ipmc_entry.source.addr.ip6, 0, sizeof(sai_ip6_t));
  VerifyP4IpMulticastEntryEqual(expect_ipv6, *entry2_ptr);

  uint32_t group1_ref_cnt = 777;
  uint32_t group2_ref_cnt = 777;
  EXPECT_TRUE(p4_oid_mapper_.getRefCount(SAI_OBJECT_TYPE_IPMC_GROUP,
                                         kMulticastGroup1, &group1_ref_cnt));
  EXPECT_TRUE(p4_oid_mapper_.getRefCount(SAI_OBJECT_TYPE_IPMC_GROUP,
                                         kMulticastGroup2, &group2_ref_cnt));
  EXPECT_EQ(group1_ref_cnt, 1);
  EXPECT_EQ(group2_ref_cnt, 1);
}

TEST_F(IpMulticastManagerTest, CreateIpMulticastEntriesFailToCreateRpfGroup) {
  auto swss_ipv4_address = swss::IpAddress(kIpv4Address1);
  auto ipv4_multicast_entry = GenerateP4IpMulticastEntry(
      gVrfName, swss_ipv4_address, p4orch::kSetMulticastGroupId,
      kMulticastGroup1);
  // Create artificial multicast group object.
  AddMulticastGroup(kMulticastGroup1, kMulticastGroupOid1);
  EXPECT_CALL(mock_sai_rpf_group_, create_rpf_group(_, _, 0, _))
      .WillOnce(Return(SAI_STATUS_FAILURE));

  EXPECT_THAT(CreateIpMulticastEntries(
                  std::vector<P4IpMulticastEntry>{ipv4_multicast_entry}),
              ArrayEq(std::vector<StatusCode>{StatusCode::SWSS_RC_UNKNOWN}));

  auto* ipv4_multicast_entry_ptr =
      GetIpMulticastEntry(ipv4_multicast_entry.ip_multicast_entry_key);
  EXPECT_EQ(ipv4_multicast_entry_ptr, nullptr);
}

TEST_F(IpMulticastManagerTest, CreateIpMulticastEntriesFailToCreateRpfRif) {
  auto swss_ipv4_address = swss::IpAddress(kIpv4Address1);
  auto ipv4_multicast_entry = GenerateP4IpMulticastEntry(
      gVrfName, swss_ipv4_address, p4orch::kSetMulticastGroupId,
      kMulticastGroup1);
  // Create artificial multicast group object.
  AddMulticastGroup(kMulticastGroup1, kMulticastGroupOid1);
  EXPECT_CALL(mock_sai_rpf_group_, create_rpf_group(_, _, 0, _))
      .WillOnce(
          DoAll(SetArgPointee<0>(kRpfGroupOid1), Return(SAI_STATUS_SUCCESS)));
  EXPECT_CALL(mock_sai_router_intf_, create_router_interface(_, _, 7, _))
      .WillOnce(Return(SAI_STATUS_FAILURE));

  EXPECT_THAT(CreateIpMulticastEntries(
                  std::vector<P4IpMulticastEntry>{ipv4_multicast_entry}),
              ArrayEq(std::vector<StatusCode>{StatusCode::SWSS_RC_UNKNOWN}));

  auto* ipv4_multicast_entry_ptr =
      GetIpMulticastEntry(ipv4_multicast_entry.ip_multicast_entry_key);
  EXPECT_EQ(ipv4_multicast_entry_ptr, nullptr);
}

TEST_F(IpMulticastManagerTest, CreateIpMulticastEntriesFailToCreateRpfMember) {
  auto swss_ipv4_address = swss::IpAddress(kIpv4Address1);
  auto ipv4_multicast_entry = GenerateP4IpMulticastEntry(
      gVrfName, swss_ipv4_address, p4orch::kSetMulticastGroupId,
      kMulticastGroup1);
  // Create artificial multicast group object.
  AddMulticastGroup(kMulticastGroup1, kMulticastGroupOid1);
  EXPECT_CALL(mock_sai_rpf_group_, create_rpf_group(_, _, 0, _))
      .WillOnce(
          DoAll(SetArgPointee<0>(kRpfGroupOid1), Return(SAI_STATUS_SUCCESS)));
  EXPECT_CALL(mock_sai_router_intf_, create_router_interface(_, _, 7, _))
      .WillOnce(DoAll(SetArgPointee<0>(kRpfRouterInterfaceOid1),
                      Return(SAI_STATUS_SUCCESS)));
  EXPECT_CALL(mock_sai_rpf_group_, create_rpf_group_member(_, _, 2, _))
      .WillOnce(Return(SAI_STATUS_FAILURE));

  EXPECT_THAT(CreateIpMulticastEntries(
                  std::vector<P4IpMulticastEntry>{ipv4_multicast_entry}),
              ArrayEq(std::vector<StatusCode>{StatusCode::SWSS_RC_UNKNOWN}));

  auto* ipv4_multicast_entry_ptr =
      GetIpMulticastEntry(ipv4_multicast_entry.ip_multicast_entry_key);
  EXPECT_EQ(ipv4_multicast_entry_ptr, nullptr);
}

TEST_F(IpMulticastManagerTest, CreateIpMulticastEntriesMissingMulticastGroup) {
  auto swss_ipv4_address = swss::IpAddress(kIpv4Address1);
  auto ipv4_multicast_entry = GenerateP4IpMulticastEntry(
      gVrfName, swss_ipv4_address, p4orch::kSetMulticastGroupId,
      kMulticastGroup1);
  // Don't add multicast group.
  EXPECT_CALL(mock_sai_rpf_group_, create_rpf_group(_, _, 0, _))
      .WillOnce(
          DoAll(SetArgPointee<0>(kRpfGroupOid1), Return(SAI_STATUS_SUCCESS)));
  EXPECT_CALL(mock_sai_router_intf_, create_router_interface(_, _, 7, _))
      .WillOnce(DoAll(SetArgPointee<0>(kRpfRouterInterfaceOid1),
                      Return(SAI_STATUS_SUCCESS)));
  EXPECT_CALL(mock_sai_rpf_group_, create_rpf_group_member(_, _, 2, _))
      .WillOnce(DoAll(SetArgPointee<0>(kRpfGroupMemberOid1),
                      Return(SAI_STATUS_SUCCESS)));

  EXPECT_THAT(CreateIpMulticastEntries(
                  std::vector<P4IpMulticastEntry>{ipv4_multicast_entry}),
              ArrayEq(std::vector<StatusCode>{StatusCode::SWSS_RC_NOT_FOUND}));

  auto* ipv4_multicast_entry_ptr =
      GetIpMulticastEntry(ipv4_multicast_entry.ip_multicast_entry_key);
  EXPECT_EQ(ipv4_multicast_entry_ptr, nullptr);
}

TEST_F(IpMulticastManagerTest, DeleteIpMulticastEntriesSuccess) {
  auto swss_ipv4_address = swss::IpAddress(kIpv4Address1);
  auto entry1 =
      SetupIpMulticastEntry(gVrfName, swss_ipv4_address, kMulticastGroup1,
                            kMulticastGroupOid1, "meta_ipv4");
  auto swss_ipv6_address = swss::IpAddress(kIpv6Address1);
  auto entry2 = SetupIpMulticastEntry(gVrfName, swss_ipv6_address,
                                      kMulticastGroup2, kMulticastGroupOid2,
                                      "meta_ipv6", /*expect_rpf=*/false);

  // Now delete those entries.
  EXPECT_CALL(mock_sai_ipmc_, remove_ipmc_entry(_))
      .WillOnce(Return(SAI_STATUS_SUCCESS))
      .WillOnce(Return(SAI_STATUS_SUCCESS));
  EXPECT_THAT(
      DeleteIpMulticastEntries(std::vector<P4IpMulticastEntry>{entry1, entry2}),
      ArrayEq(std::vector<StatusCode>{StatusCode::SWSS_RC_SUCCESS,
                                      StatusCode::SWSS_RC_SUCCESS}));

  // Expect entries to not be seen anymore.
  auto* entry1_ptr = GetIpMulticastEntry(entry1.ip_multicast_entry_key);
  auto* entry2_ptr = GetIpMulticastEntry(entry2.ip_multicast_entry_key);
  EXPECT_EQ(entry1_ptr, nullptr);
  EXPECT_EQ(entry2_ptr, nullptr);

  EXPECT_FALSE(p4_oid_mapper_.existsOID(SAI_OBJECT_TYPE_IPMC_ENTRY,
                                        entry1.ip_multicast_entry_key));
  EXPECT_FALSE(p4_oid_mapper_.existsOID(SAI_OBJECT_TYPE_IPMC_ENTRY,
                                        entry2.ip_multicast_entry_key));
}

TEST_F(IpMulticastManagerTest, DeleteIpMulticastEntriesSaiFailure) {
  auto swss_ipv4_address = swss::IpAddress(kIpv4Address1);
  auto entry1 =
      SetupIpMulticastEntry(gVrfName, swss_ipv4_address, kMulticastGroup1,
                            kMulticastGroupOid1, "meta_ipv4");
  auto swss_ipv6_address = swss::IpAddress(kIpv6Address1);
  auto entry2 = SetupIpMulticastEntry(gVrfName, swss_ipv6_address,
                                      kMulticastGroup2, kMulticastGroupOid2,
                                      "meta_ipv6", /*expect_rpf=*/false);

  // Now delete those entries, force a failure.
  EXPECT_CALL(mock_sai_ipmc_, remove_ipmc_entry(_))
      .WillOnce(Return(SAI_STATUS_FAILURE));
  EXPECT_THAT(
      DeleteIpMulticastEntries(std::vector<P4IpMulticastEntry>{entry1, entry2}),
      ArrayEq(std::vector<StatusCode>{StatusCode::SWSS_RC_UNKNOWN,
                                      StatusCode::SWSS_RC_NOT_EXECUTED}));

  // Since operation failed, expect entries to still be there.
  auto* entry1_ptr = GetIpMulticastEntry(entry1.ip_multicast_entry_key);
  auto* entry2_ptr = GetIpMulticastEntry(entry2.ip_multicast_entry_key);
  EXPECT_NE(entry1_ptr, nullptr);
  EXPECT_NE(entry2_ptr, nullptr);

  EXPECT_TRUE(p4_oid_mapper_.existsOID(SAI_OBJECT_TYPE_IPMC_ENTRY,
                                       entry1.ip_multicast_entry_key));
  EXPECT_TRUE(p4_oid_mapper_.existsOID(SAI_OBJECT_TYPE_IPMC_ENTRY,
                                       entry2.ip_multicast_entry_key));
}

TEST_F(IpMulticastManagerTest, DeleteIpMulticastEntriesMissingEntry) {
  auto swss_ipv4_address = swss::IpAddress(kIpv4Address1);
  auto entry1 = GenerateP4IpMulticastEntry(gVrfName, swss_ipv4_address,
                                           p4orch::kSetMulticastGroupId,
                                           kMulticastGroup1);

  auto swss_ipv6_address = swss::IpAddress(kIpv6Address1);
  auto entry2 = SetupIpMulticastEntry(gVrfName, swss_ipv6_address,
                                      kMulticastGroup2, kMulticastGroupOid2);

  EXPECT_THAT(
      DeleteIpMulticastEntries(std::vector<P4IpMulticastEntry>{entry1, entry2}),
      ArrayEq(std::vector<StatusCode>{StatusCode::SWSS_RC_NOT_FOUND,
                                      StatusCode::SWSS_RC_NOT_EXECUTED}));
}

TEST_F(IpMulticastManagerTest, UpdateIpMulticastEntriesSuccess) {
  auto swss_ipv4_address = swss::IpAddress(kIpv4Address1);
  auto entry1 =
      SetupIpMulticastEntry(gVrfName, swss_ipv4_address, kMulticastGroup1,
                            kMulticastGroupOid1, "meta_ipv4");
  auto swss_ipv6_address = swss::IpAddress(kIpv6Address1);
  auto entry2 = SetupIpMulticastEntry(gVrfName, swss_ipv6_address,
                                      kMulticastGroup2, kMulticastGroupOid2,
                                      "meta_ipv6", /*expect_rpf=*/false);

  // Now update those entries to point to new multicast groups.
  auto entry3 = GenerateP4IpMulticastEntry(gVrfName, swss_ipv4_address,
                                           p4orch::kSetMulticastGroupId,
                                           kMulticastGroup3, "meta_ipv4_2");
  auto entry4 = GenerateP4IpMulticastEntry(gVrfName, swss_ipv6_address,
                                           p4orch::kSetMulticastGroupId,
                                           kMulticastGroup4, "meta_ipv6_2");
  // Create fake multicast group OIDs.
  AddMulticastGroup(kMulticastGroup3, kMulticastGroupOid3);
  AddMulticastGroup(kMulticastGroup4, kMulticastGroupOid4);

  EXPECT_CALL(mock_sai_ipmc_, set_ipmc_entry_attribute(_, _))
      .WillOnce(Return(SAI_STATUS_SUCCESS))
      .WillOnce(Return(SAI_STATUS_SUCCESS));
  EXPECT_THAT(
      UpdateIpMulticastEntries(std::vector<P4IpMulticastEntry>{entry3, entry4}),
      ArrayEq(std::vector<StatusCode>{StatusCode::SWSS_RC_SUCCESS,
                                      StatusCode::SWSS_RC_SUCCESS}));

  // Expect entries to be associated with correct multicast group.
  auto* entry_ptr_v4 = GetIpMulticastEntry(entry3.ip_multicast_entry_key);
  auto* entry_ptr_v6 = GetIpMulticastEntry(entry4.ip_multicast_entry_key);
  EXPECT_NE(entry_ptr_v4, nullptr);
  EXPECT_NE(entry_ptr_v6, nullptr);

  uint32_t group1_ref_cnt = 777;
  uint32_t group2_ref_cnt = 777;
  uint32_t group3_ref_cnt = 777;
  uint32_t group4_ref_cnt = 777;

  EXPECT_TRUE(p4_oid_mapper_.getRefCount(SAI_OBJECT_TYPE_IPMC_GROUP,
                                         kMulticastGroup1, &group1_ref_cnt));
  EXPECT_TRUE(p4_oid_mapper_.getRefCount(SAI_OBJECT_TYPE_IPMC_GROUP,
                                         kMulticastGroup2, &group2_ref_cnt));
  EXPECT_TRUE(p4_oid_mapper_.getRefCount(SAI_OBJECT_TYPE_IPMC_GROUP,
                                         kMulticastGroup3, &group3_ref_cnt));
  EXPECT_TRUE(p4_oid_mapper_.getRefCount(SAI_OBJECT_TYPE_IPMC_GROUP,
                                         kMulticastGroup4, &group4_ref_cnt));
  EXPECT_EQ(group1_ref_cnt, 0);
  EXPECT_EQ(group2_ref_cnt, 0);
  EXPECT_EQ(group3_ref_cnt, 1);
  EXPECT_EQ(group4_ref_cnt, 1);

  auto expect_ipv4 = GenerateP4IpMulticastEntry(
      gVrfName, swss_ipv4_address, p4orch::kSetMulticastGroupId,
      kMulticastGroup3, "meta_ipv4_2");
  expect_ipv4.sai_ipmc_entry.switch_id = gSwitchId;
  expect_ipv4.sai_ipmc_entry.vr_id = gVrfOrch->getVRFid(gVrfName);
  expect_ipv4.sai_ipmc_entry.type = SAI_IPMC_ENTRY_TYPE_XG;
  sai_ip_address_t sai_address_v4;
  copy(sai_address_v4, swss_ipv4_address);
  expect_ipv4.sai_ipmc_entry.destination.addr_family = SAI_IP_ADDR_FAMILY_IPV4;
  expect_ipv4.sai_ipmc_entry.destination.addr.ip4 = sai_address_v4.addr.ip4;
  expect_ipv4.sai_ipmc_entry.source.addr_family = SAI_IP_ADDR_FAMILY_IPV4;
  expect_ipv4.sai_ipmc_entry.source.addr.ip4 = 0;
  VerifyP4IpMulticastEntryEqual(expect_ipv4, *entry_ptr_v4);
}

TEST_F(IpMulticastManagerTest, UpdateIpMulticastEntriesNoChangeSuccess) {
  auto swss_ipv4_address = swss::IpAddress(kIpv4Address1);
  auto entry1 =
      SetupIpMulticastEntry(gVrfName, swss_ipv4_address, kMulticastGroup1,
                            kMulticastGroupOid1, "meta_ipv4");

  // Now update the entry, but have no changes.
  EXPECT_THAT(UpdateIpMulticastEntries(std::vector<P4IpMulticastEntry>{entry1}),
              ArrayEq(std::vector<StatusCode>{StatusCode::SWSS_RC_SUCCESS}));

  // Expect entries to be associated with correct multicast group.
  auto* entry_ptr = GetIpMulticastEntry(entry1.ip_multicast_entry_key);
  EXPECT_NE(entry_ptr, nullptr);

  uint32_t group1_ref_cnt = 777;
  EXPECT_TRUE(p4_oid_mapper_.getRefCount(SAI_OBJECT_TYPE_IPMC_GROUP,
                                         kMulticastGroup1, &group1_ref_cnt));
  EXPECT_EQ(group1_ref_cnt, 1);
}

TEST_F(IpMulticastManagerTest, UpdateIpMulticastEntriesMissingEntry) {
  auto swss_ipv4_address = swss::IpAddress(kIpv4Address1);
  auto entry1 = GenerateP4IpMulticastEntry(gVrfName, swss_ipv4_address,
                                           p4orch::kSetMulticastGroupId,
                                           kMulticastGroup1);

  auto swss_ipv6_address = swss::IpAddress(kIpv6Address1);
  auto entry2 = SetupIpMulticastEntry(gVrfName, swss_ipv6_address,
                                      kMulticastGroup2, kMulticastGroupOid2);

  EXPECT_THAT(
      UpdateIpMulticastEntries(std::vector<P4IpMulticastEntry>{entry1, entry2}),
      ArrayEq(std::vector<StatusCode>{StatusCode::SWSS_RC_INTERNAL,
                                      StatusCode::SWSS_RC_NOT_EXECUTED}));
}

TEST_F(IpMulticastManagerTest, UpdateIpMulticastEntriesNoMulticastGroup) {
  auto swss_ipv4_address = swss::IpAddress(kIpv4Address1);
  auto entry1 =
      SetupIpMulticastEntry(gVrfName, swss_ipv4_address, kMulticastGroup1,
                            kMulticastGroupOid1, "meta_ipv4");
  auto swss_ipv6_address = swss::IpAddress(kIpv6Address1);
  auto entry2 = SetupIpMulticastEntry(gVrfName, swss_ipv6_address,
                                      kMulticastGroup2, kMulticastGroupOid2,
                                      "meta_ipv6", /*expect_rpf=*/false);

  // Now update those entries to point to new multicast groups.
  auto entry3 = GenerateP4IpMulticastEntry(gVrfName, swss_ipv4_address,
                                           p4orch::kSetMulticastGroupId,
                                           kMulticastGroup3, "meta_ipv4_2");
  auto entry4 = GenerateP4IpMulticastEntry(gVrfName, swss_ipv6_address,
                                           p4orch::kSetMulticastGroupId,
                                           kMulticastGroup4, "meta_ipv6_2");

  // Do not create multicast groups for updates.

  EXPECT_THAT(
      UpdateIpMulticastEntries(std::vector<P4IpMulticastEntry>{entry3, entry4}),
      ArrayEq(std::vector<StatusCode>{StatusCode::SWSS_RC_NOT_FOUND,
                                      StatusCode::SWSS_RC_NOT_EXECUTED}));

  // Expect no changes to entries.
  auto* entry_ptr_v4 = GetIpMulticastEntry(entry3.ip_multicast_entry_key);
  auto* entry_ptr_v6 = GetIpMulticastEntry(entry4.ip_multicast_entry_key);
  EXPECT_NE(entry_ptr_v4, nullptr);
  EXPECT_NE(entry_ptr_v6, nullptr);

  uint32_t group1_ref_cnt = 777;
  uint32_t group2_ref_cnt = 777;
  uint32_t group3_ref_cnt = 777;
  uint32_t group4_ref_cnt = 777;

  EXPECT_TRUE(p4_oid_mapper_.getRefCount(SAI_OBJECT_TYPE_IPMC_GROUP,
                                         kMulticastGroup1, &group1_ref_cnt));
  EXPECT_TRUE(p4_oid_mapper_.getRefCount(SAI_OBJECT_TYPE_IPMC_GROUP,
                                         kMulticastGroup2, &group2_ref_cnt));
  EXPECT_FALSE(p4_oid_mapper_.getRefCount(SAI_OBJECT_TYPE_IPMC_GROUP,
                                          kMulticastGroup3, &group3_ref_cnt));
  EXPECT_FALSE(p4_oid_mapper_.getRefCount(SAI_OBJECT_TYPE_IPMC_GROUP,
                                          kMulticastGroup4, &group4_ref_cnt));
  EXPECT_EQ(group1_ref_cnt, 1);
  EXPECT_EQ(group2_ref_cnt, 1);

  auto expect_ipv4 = GenerateP4IpMulticastEntry(gVrfName, swss_ipv4_address,
                                                p4orch::kSetMulticastGroupId,
                                                kMulticastGroup1, "meta_ipv4");
  expect_ipv4.sai_ipmc_entry.switch_id = gSwitchId;
  expect_ipv4.sai_ipmc_entry.vr_id = gVrfOrch->getVRFid(gVrfName);
  expect_ipv4.sai_ipmc_entry.type = SAI_IPMC_ENTRY_TYPE_XG;
  sai_ip_address_t sai_address_v4;
  copy(sai_address_v4, swss_ipv4_address);
  expect_ipv4.sai_ipmc_entry.destination.addr_family = SAI_IP_ADDR_FAMILY_IPV4;
  expect_ipv4.sai_ipmc_entry.destination.addr.ip4 = sai_address_v4.addr.ip4;
  expect_ipv4.sai_ipmc_entry.source.addr_family = SAI_IP_ADDR_FAMILY_IPV4;
  expect_ipv4.sai_ipmc_entry.source.addr.ip4 = 0;
  VerifyP4IpMulticastEntryEqual(expect_ipv4, *entry_ptr_v4);
}

TEST_F(IpMulticastManagerTest, UpdateIpMulticastEntriesSaiFailure) {
  auto swss_ipv4_address = swss::IpAddress(kIpv4Address1);
  auto entry1 =
      SetupIpMulticastEntry(gVrfName, swss_ipv4_address, kMulticastGroup1,
                            kMulticastGroupOid1, "meta_ipv4");
  auto swss_ipv6_address = swss::IpAddress(kIpv6Address1);
  auto entry2 = SetupIpMulticastEntry(gVrfName, swss_ipv6_address,
                                      kMulticastGroup2, kMulticastGroupOid2,
                                      "meta_ipv6", /*expect_rpf=*/false);

  // Now update those entries to point to new multicast groups.
  auto entry3 = GenerateP4IpMulticastEntry(gVrfName, swss_ipv4_address,
                                           p4orch::kSetMulticastGroupId,
                                           kMulticastGroup3, "meta_ipv4_2");
  auto entry4 = GenerateP4IpMulticastEntry(gVrfName, swss_ipv6_address,
                                           p4orch::kSetMulticastGroupId,
                                           kMulticastGroup4, "meta_ipv6_2");
  // Create fake multicast group OIDs.
  AddMulticastGroup(kMulticastGroup3, kMulticastGroupOid3);
  AddMulticastGroup(kMulticastGroup4, kMulticastGroupOid4);

  EXPECT_CALL(mock_sai_ipmc_, set_ipmc_entry_attribute(_, _))
      .WillOnce(Return(SAI_STATUS_FAILURE));
  EXPECT_THAT(
      UpdateIpMulticastEntries(std::vector<P4IpMulticastEntry>{entry3, entry4}),
      ArrayEq(std::vector<StatusCode>{StatusCode::SWSS_RC_UNKNOWN,
                                      StatusCode::SWSS_RC_NOT_EXECUTED}));

  // Expect entries to be associated with correct multicast group.
  auto* entry_ptr_v4 = GetIpMulticastEntry(entry3.ip_multicast_entry_key);
  auto* entry_ptr_v6 = GetIpMulticastEntry(entry4.ip_multicast_entry_key);
  EXPECT_NE(entry_ptr_v4, nullptr);
  EXPECT_NE(entry_ptr_v6, nullptr);

  uint32_t group1_ref_cnt = 777;
  uint32_t group2_ref_cnt = 777;
  uint32_t group3_ref_cnt = 777;
  uint32_t group4_ref_cnt = 777;

  EXPECT_TRUE(p4_oid_mapper_.getRefCount(SAI_OBJECT_TYPE_IPMC_GROUP,
                                         kMulticastGroup1, &group1_ref_cnt));
  EXPECT_TRUE(p4_oid_mapper_.getRefCount(SAI_OBJECT_TYPE_IPMC_GROUP,
                                         kMulticastGroup2, &group2_ref_cnt));
  EXPECT_TRUE(p4_oid_mapper_.getRefCount(SAI_OBJECT_TYPE_IPMC_GROUP,
                                         kMulticastGroup3, &group3_ref_cnt));
  EXPECT_TRUE(p4_oid_mapper_.getRefCount(SAI_OBJECT_TYPE_IPMC_GROUP,
                                         kMulticastGroup4, &group4_ref_cnt));
  EXPECT_EQ(group1_ref_cnt, 1);
  EXPECT_EQ(group2_ref_cnt, 1);
  EXPECT_EQ(group3_ref_cnt, 0);
  EXPECT_EQ(group4_ref_cnt, 0);

  auto expect_ipv4 = GenerateP4IpMulticastEntry(gVrfName, swss_ipv4_address,
                                                p4orch::kSetMulticastGroupId,
                                                kMulticastGroup1, "meta_ipv4");
  expect_ipv4.sai_ipmc_entry.switch_id = gSwitchId;
  expect_ipv4.sai_ipmc_entry.vr_id = gVrfOrch->getVRFid(gVrfName);
  expect_ipv4.sai_ipmc_entry.type = SAI_IPMC_ENTRY_TYPE_XG;
  sai_ip_address_t sai_address_v4;
  copy(sai_address_v4, swss_ipv4_address);
  expect_ipv4.sai_ipmc_entry.destination.addr_family = SAI_IP_ADDR_FAMILY_IPV4;
  expect_ipv4.sai_ipmc_entry.destination.addr.ip4 = sai_address_v4.addr.ip4;
  expect_ipv4.sai_ipmc_entry.source.addr_family = SAI_IP_ADDR_FAMILY_IPV4;
  expect_ipv4.sai_ipmc_entry.source.addr.ip4 = 0;
  VerifyP4IpMulticastEntryEqual(expect_ipv4, *entry_ptr_v4);
}

TEST_F(IpMulticastManagerTest, DrainNotImplemented) {
  EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS, Drain(/*failure_before=*/false));
  EXPECT_EQ(StatusCode::SWSS_RC_NOT_EXECUTED, Drain(/*failure_before=*/true));
}

TEST_F(IpMulticastManagerTest, VerifyStateNotImplemented) {
  std::vector<swss::FieldValueTuple> tuples = {};
  EXPECT_FALSE(VerifyState("key", tuples).empty());
}

}  // namespace p4orch
