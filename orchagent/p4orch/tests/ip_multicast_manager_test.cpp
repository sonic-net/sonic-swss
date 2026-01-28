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

bool AddressCmp(const sai_ip_address_t* x, const sai_ip_address_t* y) {
  if (x->addr_family != y->addr_family) {
    return false;
  }
  if (x->addr_family == SAI_IP_ADDR_FAMILY_IPV4) {
    return memcmp(&x->addr.ip4, &y->addr.ip4, sizeof(sai_ip4_t)) == 0;
  }
  return memcmp(&x->addr.ip6, &y->addr.ip6, sizeof(sai_ip6_t)) == 0;
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

TEST_F(IpMulticastManagerTest, DrainNotImplemented) {
  EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS, Drain(/*failure_before=*/false));
  EXPECT_EQ(StatusCode::SWSS_RC_NOT_EXECUTED, Drain(/*failure_before=*/true));
}

TEST_F(IpMulticastManagerTest, VerifyStateNotImplemented) {
  std::vector<swss::FieldValueTuple> tuples = {};
  EXPECT_FALSE(VerifyState("key", tuples).empty());
}

}  // namespace p4orch
