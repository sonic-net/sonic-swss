#include "tunnel_decap_group_manager.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <functional>
#include <nlohmann/json.hpp>
#include <string>
#include <unordered_map>

#include "ipprefix.h"
#include "mock_response_publisher.h"
#include "mock_sai_serialize.h"
#include "mock_sai_tunnel.h"
#include "p4oidmapper.h"
#include "p4orch/p4orch_util.h"
#include "p4orch_util.h"
#include "return_code.h"
#include "swssnet.h"
extern "C" {
#include "sai.h"
}

using ::p4orch::kTableKeyDelimiter;

using ::testing::_;
using ::testing::ContainerEq;
using ::testing::DoAll;
using ::testing::Eq;
using ::testing::Pointee;
using ::testing::Return;
using ::testing::SetArgPointee;
using ::testing::SetArrayArgument;
using ::testing::StrictMock;
using ::testing::Truly;

using sai_attrs_array_t =
    std::vector<std::unordered_map<sai_attr_id_t, sai_attribute_value_t>>;

extern sai_object_id_t gSwitchId;
extern sai_object_id_t gUnderlayIfId;
extern sai_object_id_t gVrfOid;
extern char* gVrfName;
extern VRFOrch* gVrfOrch;
extern sai_tunnel_api_t* sai_tunnel_api;
extern sai_router_interface_api_t* sai_router_intfs_api;
extern MockSaiTunnel* mock_sai_tunnel;

namespace {

constexpr sai_object_id_t kDummyTunnelOid = 0x10;
constexpr sai_object_id_t kIpv6TunnelTermEntryOid1 = 0x11;
constexpr sai_object_id_t kIpv6TunnelTermEntryOid2 = 0x12;
constexpr sai_object_id_t kIpv6TunnelTermEntryOid3 = 0x13;

constexpr char* kIpv6TunnelTermAppDbKey1 =
 R"({"match/src_ipv6":"4001:db8:3c4d:19::&ffff:ffff:ffff:ffff::",)"
  R"("match/dst_ipv6":"2001:db8:3c4d:15::&ffff:ffff:ffff:ffff::"})";
constexpr char* kIpv6TunnelTermAppDbSrcIp1 = "4001:db8:3c4d:19::";
constexpr char* kIpv6TunnelTermAppDbSrcMask1 = "ffff:ffff:ffff:ffff::";
constexpr char* kIpv6TunnelTermAppDbSrcIpMask1 =
    "4001:db8:3c4d:19::&ffff:ffff:ffff:ffff::";
constexpr char* kIpv6TunnelTermAppDbDstIp1 = "2001:db8:3c4d:15::";
constexpr char* kIpv6TunnelTermAppDbDstMask1 = "ffff:ffff:ffff:ffff::";
constexpr char* kIpv6TunnelTermAppDbDstIpMask1 =
    "2001:db8:3c4d:15::&ffff:ffff:ffff:ffff::";
constexpr char* kIpv6TunnelTermAppDbSrcIp2 = "6001:db8:3c4d::";
constexpr char* kIpv6TunnelTermAppDbSrcMask2 = "ffff:ffff:ffff::";
constexpr char* kIpv6TunnelTermAppDbSrcIpMask2 =
    "6001:db8:3c4d::&ffff:ffff:ffff::";
constexpr char* kIpv6TunnelTermAppDbDstIp2 = "2001:db8:3c4d::";
constexpr char* kIpv6TunnelTermAppDbDstMask2 = "ffff:ffff:ffff::";
constexpr char* kIpv6TunnelTermAppDbDstIpMask2 =
    "2001:db8:3c4d::&ffff:ffff:ffff::";
constexpr char* kIpv6TunnelTermAppDbSrcIp3 = "5001:db8::";
constexpr char* kIpv6TunnelTermAppDbSrcMask3 = "ffff:ffff::";
constexpr char* kIpv6TunnelTermAppDbSrcIpMask3 = "5001:db8::&ffff:ffff::";
constexpr char* kIpv6TunnelTermAppDbDstIp3 = "2001:db8::";
constexpr char* kIpv6TunnelTermAppDbDstMask3 = "ffff:ffff::";
constexpr char* kIpv6TunnelTermAppDbDstIpMask3 = "2001:db8::&ffff:ffff::";

MATCHER_P(ArrayEq, array, "") {
  for (size_t i = 0; i < array.size(); ++i) {
    if (arg[i] != array[i]) {
      return false;
    }
  }
  return true;
}

// APP DB entries for Add request.
const Ipv6TunnelTermAppDbEntry kIpv6TunnelTermAppDbEntry1{
    /*src_ipv6_ip=*/swss::IpAddress(kIpv6TunnelTermAppDbSrcIp1),
    /*src_ipv6_mask=*/swss::IpAddress(kIpv6TunnelTermAppDbSrcMask1),
    /*dst_ipv6_ip=*/swss::IpAddress(kIpv6TunnelTermAppDbDstIp1),
    /*dst_ipv6_mask=*/swss::IpAddress(kIpv6TunnelTermAppDbDstMask1),
    /*vrf_id=*/gVrfName,
    /*action_str=*/"mark_for_tunnel_decap_and_set_vrf"};
const Ipv6TunnelTermAppDbEntry kIpv6TunnelTermAppDbEntry2{
    /*src_ipv6_ip=*/swss::IpAddress(kIpv6TunnelTermAppDbSrcIp2),
    /*src_ipv6_mask=*/swss::IpAddress(kIpv6TunnelTermAppDbSrcMask2),
    /*dst_ipv6_ip=*/swss::IpAddress(kIpv6TunnelTermAppDbDstIp2),
    /*dst_ipv6_mask=*/swss::IpAddress(kIpv6TunnelTermAppDbDstMask2),
    /*vrf_id=*/gVrfName,
    /*action_str=*/"mark_for_tunnel_decap_and_set_vrf"};
const Ipv6TunnelTermAppDbEntry kIpv6TunnelTermAppDbEntry3{
    /*src_ipv6_ip=*/swss::IpAddress(kIpv6TunnelTermAppDbSrcIp3),
    /*src_ipv6_mask=*/swss::IpAddress(kIpv6TunnelTermAppDbSrcMask3),
    /*dst_ipv6_ip=*/swss::IpAddress(kIpv6TunnelTermAppDbDstIp3),
    /*dst_ipv6_mask=*/swss::IpAddress(kIpv6TunnelTermAppDbDstMask3),
    /*vrf_id=*/gVrfName,
    /*action_str=*/"mark_for_tunnel_decap_and_set_vrf"};

bool MatchSaiAttrList(const sai_attribute_t* attr_list,
                      const std::vector<sai_attribute_t>& expected_attr_list) {
  for (size_t i = 0; i < expected_attr_list.size(); ++i) {
    switch (attr_list[i].id) {
      case SAI_TUNNEL_TERM_TABLE_ENTRY_ATTR_TUNNEL_TYPE: {
        if (attr_list[i].value.s32 != expected_attr_list[i].value.s32) {
          return false;
        }
        break;
      }
      case SAI_TUNNEL_TERM_TABLE_ENTRY_ATTR_TYPE: {
        if (attr_list[i].value.s32 != expected_attr_list[i].value.s32) {
          return false;
        }
        break;
      }
      case SAI_TUNNEL_TERM_TABLE_ENTRY_ATTR_SRC_IP:
      case SAI_TUNNEL_TERM_TABLE_ENTRY_ATTR_DST_IP: {
        if (attr_list[i].value.ipaddr.addr_family !=
                expected_attr_list[i].value.ipaddr.addr_family ||
            memcmp(attr_list[i].value.ipaddr.addr.ip6,
                   expected_attr_list[i].value.ipaddr.addr.ip6,
                   sizeof(sai_ip6_t)) != 0) {
          return false;
        }
        break;
      }
      case SAI_TUNNEL_TERM_TABLE_ENTRY_ATTR_SRC_IP_MASK:
      case SAI_TUNNEL_TERM_TABLE_ENTRY_ATTR_DST_IP_MASK: {
        if (attr_list[i].value.ipaddr.addr_family !=
                expected_attr_list[i].value.ipaddr.addr_family ||
            memcmp(attr_list[i].value.ipaddr.addr.ip6,
                   expected_attr_list[i].value.ipaddr.addr.ip6,
                   sizeof(sai_ip6_t)) != 0) {
          return false;
        }
        break;
      }
      case SAI_TUNNEL_TERM_TABLE_ENTRY_ATTR_VR_ID: {
        if (attr_list[i].value.oid != expected_attr_list[i].value.oid) {
          return false;
        }
        break;
      }
      case SAI_TUNNEL_TERM_TABLE_ENTRY_ATTR_ACTION_TUNNEL_ID: {
        if (attr_list[i].value.oid != expected_attr_list[i].value.oid) {
          return false;
        }
        break;
      }
      default:
        return false;
    }
  }

  return true;
}

MATCHER_P(AttrListEq, array, "") { return MatchSaiAttrList(arg, array); }

sai_status_t mock_create_dummy_tunnel(_Out_ sai_object_id_t* tunnel_id,
                                      _In_ sai_object_id_t switch_id,
                                      _In_ uint32_t attr_count,
                                      _In_ const sai_attribute_t* attr_list) {
  *tunnel_id = kDummyTunnelOid;
  return SAI_STATUS_SUCCESS;
}

}  // namespace

std::vector<sai_attribute_t> CreateSaiAttrs(
    const Ipv6TunnelTermAppDbEntry& app_entry) {
  std::vector<sai_attribute_t> attrs;
  sai_attribute_t attr;

  attr.id = SAI_TUNNEL_TERM_TABLE_ENTRY_ATTR_TUNNEL_TYPE;
  attr.value.s32 = SAI_TUNNEL_TYPE_IPINIP;
  attrs.push_back(attr);

  attr.id = SAI_TUNNEL_TERM_TABLE_ENTRY_ATTR_TYPE;
  attr.value.s32 = SAI_TUNNEL_TERM_TABLE_ENTRY_TYPE_MP2MP;
  attrs.push_back(attr);

  attr.id = SAI_TUNNEL_TERM_TABLE_ENTRY_ATTR_SRC_IP;
  swss::copy(attr.value.ipaddr, app_entry.src_ipv6_ip);
  attrs.push_back(attr);

  attr.id = SAI_TUNNEL_TERM_TABLE_ENTRY_ATTR_SRC_IP_MASK;
  swss::copy(attr.value.ipaddr, app_entry.src_ipv6_mask);
  attrs.push_back(attr);

  attr.id = SAI_TUNNEL_TERM_TABLE_ENTRY_ATTR_DST_IP;
  swss::copy(attr.value.ipaddr, app_entry.dst_ipv6_ip);
  attrs.push_back(attr);

  attr.id = SAI_TUNNEL_TERM_TABLE_ENTRY_ATTR_DST_IP_MASK;
  swss::copy(attr.value.ipaddr, app_entry.dst_ipv6_mask);
  attrs.push_back(attr);

  attr.id = SAI_TUNNEL_TERM_TABLE_ENTRY_ATTR_VR_ID;
  attr.value.oid = gVrfOid;
  attrs.push_back(attr);

  attr.id = SAI_TUNNEL_TERM_TABLE_ENTRY_ATTR_ACTION_TUNNEL_ID;
  attr.value.oid = kDummyTunnelOid;
  attrs.push_back(attr);

  return attrs;
}

class TunnelDecapGroupManagerTest : public ::testing::Test {
 protected:
  TunnelDecapGroupManagerTest()
      : tunnel_decap_group_manager_(&p4_oid_mapper_, gVrfOrch, &publisher_) {}

  void SetUp() override {
    // Set up mock stuff for SAI tunnel API structure.
    mock_sai_tunnel = &mock_sai_tunnel_;
    sai_tunnel_api->create_tunnel_term_table_entry =
        mock_create_tunnel_term_table_entry;
    sai_tunnel_api->remove_tunnel_term_table_entry =
        mock_remove_tunnel_term_table_entry;
    sai_tunnel_api->create_tunnel = mock_create_dummy_tunnel;

    mock_sai_serialize = &mock_sai_serialize_;
  }

  void Enqueue(const swss::KeyOpFieldsValuesTuple& entry) {
    tunnel_decap_group_manager_.enqueue(
        APP_P4RT_IPV6_TUNNEL_TERMINATION_TABLE_NAME, entry);
  }

  ReturnCode Drain(bool failure_before) {
    if (failure_before) {
      tunnel_decap_group_manager_.drainWithNotExecuted();
      return ReturnCode(StatusCode::SWSS_RC_NOT_EXECUTED);
    }
    return tunnel_decap_group_manager_.drain();
  }

  std::string VerifyState(const std::string& key,
                          const std::vector<swss::FieldValueTuple>& tuple) {
    return tunnel_decap_group_manager_.verifyState(key, tuple);
  }

  Ipv6TunnelTermTableEntry* GetIpv6TunnelTermEntry(
      const std::string& ipv6_tunnel_term_key) {
    return tunnel_decap_group_manager_.getIpv6TunnelTermEntry(
        ipv6_tunnel_term_key);
  }

  bool ValidateIpv6TunnelTermEntryAdd(
      const Ipv6TunnelTermAppDbEntry& app_db_entry);

  std::vector<ReturnCode> CreateIpv6TunnelTermEntries(
      const std::vector<Ipv6TunnelTermAppDbEntry>& entries) {
    return tunnel_decap_group_manager_.createIpv6TunnelTermEntries(entries);
  }

  std::vector<ReturnCode> RemoveIpv6TunnelTermEntries(
      const std::vector<Ipv6TunnelTermAppDbEntry>& entries) {
    return tunnel_decap_group_manager_.removeIpv6TunnelTermEntries(entries);
  }

  ReturnCode processEntries(
      const std::vector<Ipv6TunnelTermAppDbEntry>& entries,
      const std::vector<swss::KeyOpFieldsValuesTuple>& tuple_list,
      const std::string& op, bool update) {
    return tunnel_decap_group_manager_.processEntries(entries, tuple_list, op,
                                                      update);
  }

  ReturnCodeOr<Ipv6TunnelTermAppDbEntry> DeserializeIpv6TunnelTermAppDbEntry(
      const std::string& key,
      const std::vector<swss::FieldValueTuple>& attributes) {
    return tunnel_decap_group_manager_.deserializeIpv6TunnelTermAppDbEntry(
        key, attributes);
  }

  Ipv6TunnelTermTableEntry* AddIpv6TunnelTermAppDbEntry1();

  ReturnCode ValidateIpv6TunnelTermAppDbEntry(
      const Ipv6TunnelTermAppDbEntry& app_db_entry,
      const std::string& operation) {
    return tunnel_decap_group_manager_.validateIpv6TunnelTermAppDbEntry(
        app_db_entry, operation);
  }

  StrictMock<MockSaiTunnel> mock_sai_tunnel_;
  StrictMock<MockSaiSerialize> mock_sai_serialize_;
  StrictMock<MockResponsePublisher> publisher_;
  P4OidMapper p4_oid_mapper_;
  TunnelDecapGroupManager tunnel_decap_group_manager_;
};

Ipv6TunnelTermTableEntry*
TunnelDecapGroupManagerTest::AddIpv6TunnelTermAppDbEntry1() {
  uint32_t vrf_prev_ref_count = gVrfOrch->getVrfRefCount(gVrfName);

  // Set up mock call.
  EXPECT_CALL(mock_sai_tunnel_,
              create_tunnel_term_table_entry(
                  ::testing::NotNull(), Eq(gSwitchId), Eq(8),
                  AttrListEq(CreateSaiAttrs(kIpv6TunnelTermAppDbEntry1))))
      .WillOnce(DoAll(SetArgPointee<0>(kIpv6TunnelTermEntryOid1),
                      Return(SAI_STATUS_SUCCESS)));

  EXPECT_THAT(CreateIpv6TunnelTermEntries(std::vector<Ipv6TunnelTermAppDbEntry>{
                  kIpv6TunnelTermAppDbEntry1}),
              ArrayEq(std::vector<StatusCode>{StatusCode::SWSS_RC_SUCCESS}));

  EXPECT_TRUE(ValidateIpv6TunnelTermEntryAdd(kIpv6TunnelTermAppDbEntry1));

  const std::string ipv6_tunnel_term_entry_key =
      KeyGenerator::generateIpv6TunnelTermKey(
          kIpv6TunnelTermAppDbEntry1.src_ipv6_ip,
          kIpv6TunnelTermAppDbEntry1.src_ipv6_mask,
          kIpv6TunnelTermAppDbEntry1.dst_ipv6_ip,
          kIpv6TunnelTermAppDbEntry1.dst_ipv6_mask);

  EXPECT_TRUE(p4_oid_mapper_.existsOID(SAI_OBJECT_TYPE_TUNNEL_TERM_TABLE_ENTRY,
                                       ipv6_tunnel_term_entry_key));

  EXPECT_THAT(gVrfOrch->getVrfRefCount(gVrfName), Eq(vrf_prev_ref_count + 1));

  return GetIpv6TunnelTermEntry(ipv6_tunnel_term_entry_key);
}

bool TunnelDecapGroupManagerTest::ValidateIpv6TunnelTermEntryAdd(
    const Ipv6TunnelTermAppDbEntry& app_db_entry) {
  const std::string ipv6_tunnel_term_entry_key =
       KeyGenerator::generateIpv6TunnelTermKey(
          app_db_entry.src_ipv6_ip, app_db_entry.src_ipv6_mask,
          app_db_entry.dst_ipv6_ip, app_db_entry.dst_ipv6_mask);
  const auto* ipv6_tunnel_term_entry =
      GetIpv6TunnelTermEntry(ipv6_tunnel_term_entry_key);

  if (ipv6_tunnel_term_entry == nullptr ||
      ipv6_tunnel_term_entry->dst_ipv6_ip != app_db_entry.dst_ipv6_ip ||
      ipv6_tunnel_term_entry->dst_ipv6_mask != app_db_entry.dst_ipv6_mask ||
      ipv6_tunnel_term_entry->vrf_id != app_db_entry.vrf_id) {
    return false;
  }

  return true;
}

TEST_F(TunnelDecapGroupManagerTest, DrainValidAppEntryShouldSucceed) {
  nlohmann::json j;
  j[prependMatchField(p4orch::kDecapSrcIpv6)] = kIpv6TunnelTermAppDbSrcIpMask1;
  j[prependMatchField(p4orch::kDecapDstIpv6)] = kIpv6TunnelTermAppDbDstIpMask1;

  uint32_t vrf_prev_ref_count = gVrfOrch->getVrfRefCount(gVrfName);

  std::vector<swss::FieldValueTuple> fvs{
      {p4orch::kAction, p4orch::kIpv6TunnelTermAction},
      {prependParamField(p4orch::kVrfId), kIpv6TunnelTermAppDbEntry1.vrf_id}};

  swss::KeyOpFieldsValuesTuple app_db_entry(
      std::string(APP_P4RT_IPV6_TUNNEL_TERMINATION_TABLE_NAME) +
          kTableKeyDelimiter + j.dump(),
      SET_COMMAND, fvs);

  Enqueue(app_db_entry);

  // Set up mock call.
  EXPECT_CALL(mock_sai_tunnel_,
              create_tunnel_term_table_entry(
                  ::testing::NotNull(), Eq(gSwitchId), Eq(8),
                  AttrListEq(CreateSaiAttrs(kIpv6TunnelTermAppDbEntry1))))
      .WillOnce(DoAll(SetArgPointee<0>(kIpv6TunnelTermEntryOid1),
                      Return(SAI_STATUS_SUCCESS)));

  EXPECT_CALL(publisher_,
              publish(Eq(APP_P4RT_TABLE_NAME), Eq(kfvKey(app_db_entry)),
                      Eq(kfvFieldsValues(app_db_entry)),
                      Eq(StatusCode::SWSS_RC_SUCCESS), Eq(true)));
  EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS, Drain(/*failure_before=*/false));

  EXPECT_TRUE(ValidateIpv6TunnelTermEntryAdd(kIpv6TunnelTermAppDbEntry1));

  const std::string ipv6_tunnel_term_entry_key =
      KeyGenerator::generateIpv6TunnelTermKey(
          kIpv6TunnelTermAppDbEntry1.src_ipv6_ip,
          kIpv6TunnelTermAppDbEntry1.src_ipv6_mask,
          kIpv6TunnelTermAppDbEntry1.dst_ipv6_ip,
          kIpv6TunnelTermAppDbEntry1.dst_ipv6_mask);

  EXPECT_TRUE(p4_oid_mapper_.existsOID(SAI_OBJECT_TYPE_TUNNEL_TERM_TABLE_ENTRY,
                                       ipv6_tunnel_term_entry_key));

  EXPECT_THAT(gVrfOrch->getVrfRefCount(gVrfName), Eq(vrf_prev_ref_count + 1));
}

TEST_F(TunnelDecapGroupManagerTest, DrainDuplicateSetRequestShouldFail) {
  auto* ipv6_tunnel_term_table_entry = AddIpv6TunnelTermAppDbEntry1();
  ASSERT_NE(ipv6_tunnel_term_table_entry, nullptr);

  nlohmann::json j;
  j[prependMatchField(p4orch::kDecapSrcIpv6)] = kIpv6TunnelTermAppDbSrcIpMask1;
  j[prependMatchField(p4orch::kDecapDstIpv6)] = kIpv6TunnelTermAppDbDstIpMask1;

  uint32_t vrf_prev_ref_count = gVrfOrch->getVrfRefCount(gVrfName);

  std::vector<swss::FieldValueTuple> fvs{
      {p4orch::kAction, p4orch::kIpv6TunnelTermAction},
      {prependParamField(p4orch::kVrfId), kIpv6TunnelTermAppDbEntry1.vrf_id}};

  swss::KeyOpFieldsValuesTuple app_db_entry(
      std::string(APP_P4RT_IPV6_TUNNEL_TERMINATION_TABLE_NAME) +
          kTableKeyDelimiter + j.dump(),
      SET_COMMAND, fvs);

  Enqueue(app_db_entry);

  EXPECT_CALL(publisher_,
              publish(Eq(APP_P4RT_TABLE_NAME), Eq(kfvKey(app_db_entry)),
                      Eq(kfvFieldsValues(app_db_entry)),
                      Eq(StatusCode::SWSS_RC_UNIMPLEMENTED), Eq(true)));

  EXPECT_EQ(StatusCode::SWSS_RC_UNIMPLEMENTED, Drain(/*failure_before=*/false));

  EXPECT_TRUE(ValidateIpv6TunnelTermEntryAdd(kIpv6TunnelTermAppDbEntry1));

  const std::string ipv6_tunnel_term_entry_key =
      KeyGenerator::generateIpv6TunnelTermKey(
          kIpv6TunnelTermAppDbEntry1.src_ipv6_ip,
          kIpv6TunnelTermAppDbEntry1.src_ipv6_mask,
          kIpv6TunnelTermAppDbEntry1.dst_ipv6_ip,
          kIpv6TunnelTermAppDbEntry1.dst_ipv6_mask);

  EXPECT_TRUE(p4_oid_mapper_.existsOID(SAI_OBJECT_TYPE_TUNNEL_TERM_TABLE_ENTRY,
                                       ipv6_tunnel_term_entry_key));

  EXPECT_THAT(gVrfOrch->getVrfRefCount(gVrfName), Eq(vrf_prev_ref_count));
}

TEST_F(TunnelDecapGroupManagerTest, DrainEntryDeserializeFail) {
  nlohmann::json j;
  j[prependMatchField(p4orch::kDecapDstIpv6)] =
      R"({"match/dst_ipv6":"2001:db8:3c4d:15::"})";

  std::vector<swss::FieldValueTuple> fvs{
      {p4orch::kAction, p4orch::kIpv6TunnelTermAction},
      {prependParamField(p4orch::kVrfId), kIpv6TunnelTermAppDbEntry1.vrf_id}};

  swss::KeyOpFieldsValuesTuple app_db_entry(
      std::string(APP_P4RT_IPV6_TUNNEL_TERMINATION_TABLE_NAME) +
          kTableKeyDelimiter + j.dump(),
      SET_COMMAND, fvs);

  Enqueue(app_db_entry);

  EXPECT_CALL(publisher_,
              publish(Eq(APP_P4RT_TABLE_NAME), Eq(kfvKey(app_db_entry)),
                      Eq(kfvFieldsValues(app_db_entry)),
                      Eq(StatusCode::SWSS_RC_INVALID_PARAM), Eq(true)));
  EXPECT_EQ(StatusCode::SWSS_RC_INVALID_PARAM, Drain(/*failure_before=*/false));
}

TEST_F(TunnelDecapGroupManagerTest, DrainEntryValidateFail) {
  nlohmann::json j;
  j[prependMatchField(p4orch::kDecapSrcIpv6)] = kIpv6TunnelTermAppDbSrcIpMask1;
  j[prependMatchField(p4orch::kDecapDstIpv6)] = kIpv6TunnelTermAppDbDstIpMask1;

  std::vector<swss::FieldValueTuple> fvs{
      {p4orch::kAction, "invalid action"},
      {prependParamField(p4orch::kVrfId), kIpv6TunnelTermAppDbEntry1.vrf_id}};

  swss::KeyOpFieldsValuesTuple app_db_entry(
      std::string(APP_P4RT_IPV6_TUNNEL_TERMINATION_TABLE_NAME) +
          kTableKeyDelimiter + j.dump(),
      SET_COMMAND, fvs);

  Enqueue(app_db_entry);

  EXPECT_CALL(publisher_,
              publish(Eq(APP_P4RT_TABLE_NAME), Eq(kfvKey(app_db_entry)),
                      Eq(kfvFieldsValues(app_db_entry)),
                      Eq(StatusCode::SWSS_RC_INVALID_PARAM), Eq(true)));
  EXPECT_EQ(StatusCode::SWSS_RC_INVALID_PARAM, Drain(/*failure_before=*/false));
}

TEST_F(TunnelDecapGroupManagerTest,
       DrainDeleteRequestShouldSucceedForExistingEntry) {
  auto* ipv6_tunnel_term_table_entry = AddIpv6TunnelTermAppDbEntry1();
  ASSERT_NE(ipv6_tunnel_term_table_entry, nullptr);

  nlohmann::json j;
  j[prependMatchField(p4orch::kDecapSrcIpv6)] = kIpv6TunnelTermAppDbSrcIpMask1;
  j[prependMatchField(p4orch::kDecapDstIpv6)] = kIpv6TunnelTermAppDbDstIpMask1;

  uint32_t vrf_prev_ref_count = gVrfOrch->getVrfRefCount(gVrfName);

  std::vector<swss::FieldValueTuple> fvs{
      {p4orch::kAction, p4orch::kIpv6TunnelTermAction},
      {prependParamField(p4orch::kVrfId), kIpv6TunnelTermAppDbEntry1.vrf_id}};

  swss::KeyOpFieldsValuesTuple app_db_entry(
      std::string(APP_P4RT_IPV6_TUNNEL_TERMINATION_TABLE_NAME) +
          kTableKeyDelimiter + j.dump(),
      DEL_COMMAND, fvs);

  // Set up mock call.
  EXPECT_CALL(mock_sai_tunnel_,
              remove_tunnel_term_table_entry(Eq(kIpv6TunnelTermEntryOid1)))
      .WillOnce(Return(SAI_STATUS_SUCCESS));

  Enqueue(app_db_entry);

  EXPECT_CALL(publisher_,
              publish(Eq(APP_P4RT_TABLE_NAME), Eq(kfvKey(app_db_entry)),
                      Eq(kfvFieldsValues(app_db_entry)),
                      Eq(StatusCode::SWSS_RC_SUCCESS), Eq(true)));
  EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS, Drain(/*failure_before=*/false));

  const std::string ipv6_tunnel_term_entry_key =
      KeyGenerator::generateIpv6TunnelTermKey(
          kIpv6TunnelTermAppDbEntry1.src_ipv6_ip,
          kIpv6TunnelTermAppDbEntry1.src_ipv6_mask,
          kIpv6TunnelTermAppDbEntry1.dst_ipv6_ip,
          kIpv6TunnelTermAppDbEntry1.dst_ipv6_mask);

  EXPECT_FALSE(p4_oid_mapper_.existsOID(SAI_OBJECT_TYPE_TUNNEL_TERM_TABLE_ENTRY,
                                        ipv6_tunnel_term_entry_key));

  EXPECT_THAT(gVrfOrch->getVrfRefCount(gVrfName), Eq(vrf_prev_ref_count - 1));
}

TEST_F(TunnelDecapGroupManagerTest, DrainInvalidAppEntryShouldFail) {
  nlohmann::json j;
  j[prependMatchField(p4orch::kDecapSrcIpv6)] = kIpv6TunnelTermAppDbSrcIpMask1;
  j[prependMatchField(p4orch::kDecapDstIpv6)] = kIpv6TunnelTermAppDbDstIpMask1;

  uint32_t vrf_prev_ref_count = gVrfOrch->getVrfRefCount(gVrfName);

  std::vector<swss::FieldValueTuple> fvs = {
      {p4orch::kAction, "invalid action"},
      {prependParamField(p4orch::kVrfId), kIpv6TunnelTermAppDbEntry1.vrf_id}};

  swss::KeyOpFieldsValuesTuple app_db_entry = {
      std::string(APP_P4RT_IPV6_TUNNEL_TERMINATION_TABLE_NAME) +
          kTableKeyDelimiter + j.dump(),
      SET_COMMAND, fvs};

  Enqueue(app_db_entry);

  EXPECT_CALL(publisher_,
              publish(Eq(APP_P4RT_TABLE_NAME), Eq(kfvKey(app_db_entry)),
                      Eq(kfvFieldsValues(app_db_entry)),
                      Eq(StatusCode::SWSS_RC_INVALID_PARAM), Eq(true)));
  EXPECT_EQ(StatusCode::SWSS_RC_INVALID_PARAM, Drain(/*failure_before=*/false));

  const std::string ipv6_tunnel_term_entry_key =
      KeyGenerator::generateIpv6TunnelTermKey(
          kIpv6TunnelTermAppDbEntry1.src_ipv6_ip,
          kIpv6TunnelTermAppDbEntry1.src_ipv6_mask,
          kIpv6TunnelTermAppDbEntry1.dst_ipv6_ip,
          kIpv6TunnelTermAppDbEntry1.dst_ipv6_mask);

  EXPECT_FALSE(p4_oid_mapper_.existsOID(SAI_OBJECT_TYPE_TUNNEL_TERM_TABLE_ENTRY,
                                        ipv6_tunnel_term_entry_key));

  EXPECT_THAT(gVrfOrch->getVrfRefCount(gVrfName), Eq(vrf_prev_ref_count));

  // Invalid match field.
  j[prependMatchField(p4orch::kDecapDstIpv6)] = "0.0.0.0";

  fvs = {
      {p4orch::kAction, p4orch::kIpv6TunnelTermAction},
      {prependParamField(p4orch::kVrfId), kIpv6TunnelTermAppDbEntry1.vrf_id}};

  app_db_entry = {std::string(APP_P4RT_IPV6_TUNNEL_TERMINATION_TABLE_NAME) +
                      kTableKeyDelimiter + j.dump(),
                  SET_COMMAND, fvs};

  Enqueue(app_db_entry);

  EXPECT_CALL(publisher_,
              publish(Eq(APP_P4RT_TABLE_NAME), Eq(kfvKey(app_db_entry)),
                      Eq(kfvFieldsValues(app_db_entry)),
                      Eq(StatusCode::SWSS_RC_INVALID_PARAM), Eq(true)));
  EXPECT_EQ(StatusCode::SWSS_RC_INVALID_PARAM, Drain(/*failure_before=*/false));

  EXPECT_FALSE(p4_oid_mapper_.existsOID(SAI_OBJECT_TYPE_TUNNEL_TERM_TABLE_ENTRY,
                                        ipv6_tunnel_term_entry_key));

  EXPECT_THAT(gVrfOrch->getVrfRefCount(gVrfName), Eq(vrf_prev_ref_count));
}

TEST_F(TunnelDecapGroupManagerTest, DrainNotExecuted) {
  uint32_t vrf_prev_ref_count = gVrfOrch->getVrfRefCount(gVrfName);

  std::vector<swss::FieldValueTuple> fvs{
      {p4orch::kAction, p4orch::kIpv6TunnelTermAction},
      {prependParamField(p4orch::kVrfId), gVrfName}};

  nlohmann::json j;
  j[prependMatchField(p4orch::kDecapSrcIpv6)] = kIpv6TunnelTermAppDbSrcIpMask1;
  j[prependMatchField(p4orch::kDecapDstIpv6)] = kIpv6TunnelTermAppDbDstIpMask1;
  swss::KeyOpFieldsValuesTuple app_db_entry_1(
      std::string(APP_P4RT_IPV6_TUNNEL_TERMINATION_TABLE_NAME) +
          kTableKeyDelimiter + j.dump(),
      SET_COMMAND, fvs);
  j[prependMatchField(p4orch::kDecapSrcIpv6)] = kIpv6TunnelTermAppDbSrcIpMask2;
  j[prependMatchField(p4orch::kDecapDstIpv6)] = kIpv6TunnelTermAppDbDstIpMask2;
  swss::KeyOpFieldsValuesTuple app_db_entry_2(
      std::string(APP_P4RT_IPV6_TUNNEL_TERMINATION_TABLE_NAME) +
          kTableKeyDelimiter + j.dump(),
      SET_COMMAND, fvs);
 j[prependMatchField(p4orch::kDecapSrcIpv6)] = kIpv6TunnelTermAppDbSrcIpMask3;
 j[prependMatchField(p4orch::kDecapDstIpv6)] = kIpv6TunnelTermAppDbDstIpMask3;
  swss::KeyOpFieldsValuesTuple app_db_entry_3(
      std::string(APP_P4RT_IPV6_TUNNEL_TERMINATION_TABLE_NAME) +
          kTableKeyDelimiter + j.dump(),
      SET_COMMAND, fvs);

  Enqueue(app_db_entry_1);
  Enqueue(app_db_entry_2);
  Enqueue(app_db_entry_3);

  EXPECT_CALL(publisher_,
              publish(Eq(APP_P4RT_TABLE_NAME), Eq(kfvKey(app_db_entry_1)),
                      Eq(kfvFieldsValues(app_db_entry_1)),
                      Eq(StatusCode::SWSS_RC_NOT_EXECUTED), Eq(true)));
  EXPECT_CALL(publisher_,
              publish(Eq(APP_P4RT_TABLE_NAME), Eq(kfvKey(app_db_entry_2)),
                      Eq(kfvFieldsValues(app_db_entry_2)),
                      Eq(StatusCode::SWSS_RC_NOT_EXECUTED), Eq(true)));
  EXPECT_CALL(publisher_,
              publish(Eq(APP_P4RT_TABLE_NAME), Eq(kfvKey(app_db_entry_3)),
                      Eq(kfvFieldsValues(app_db_entry_3)),
                      Eq(StatusCode::SWSS_RC_NOT_EXECUTED), Eq(true)));
  EXPECT_EQ(StatusCode::SWSS_RC_NOT_EXECUTED, Drain(/*failure_before=*/true));

  const std::string ipv6_tunnel_term_entry_key_1 =
      KeyGenerator::generateIpv6TunnelTermKey(
          swss::IpAddress(kIpv6TunnelTermAppDbSrcIp1),
          swss::IpAddress(kIpv6TunnelTermAppDbSrcMask1),
          swss::IpAddress(kIpv6TunnelTermAppDbDstIp1),
          swss::IpAddress(kIpv6TunnelTermAppDbDstMask1));
  const std::string ipv6_tunnel_term_entry_key_2 =
      KeyGenerator::generateIpv6TunnelTermKey(
          swss::IpAddress(kIpv6TunnelTermAppDbSrcIp2),
          swss::IpAddress(kIpv6TunnelTermAppDbSrcMask2),
          swss::IpAddress(kIpv6TunnelTermAppDbDstIp2),
          swss::IpAddress(kIpv6TunnelTermAppDbDstMask2));
  const std::string ipv6_tunnel_term_entry_key_3 =
      KeyGenerator::generateIpv6TunnelTermKey(
          swss::IpAddress(kIpv6TunnelTermAppDbSrcIp3),
          swss::IpAddress(kIpv6TunnelTermAppDbSrcMask3),
          swss::IpAddress(kIpv6TunnelTermAppDbDstIp3),
          swss::IpAddress(kIpv6TunnelTermAppDbDstMask3));
  EXPECT_EQ(nullptr, GetIpv6TunnelTermEntry(ipv6_tunnel_term_entry_key_1));
  EXPECT_EQ(nullptr, GetIpv6TunnelTermEntry(ipv6_tunnel_term_entry_key_2));
  EXPECT_EQ(nullptr, GetIpv6TunnelTermEntry(ipv6_tunnel_term_entry_key_3));

  EXPECT_FALSE(p4_oid_mapper_.existsOID(SAI_OBJECT_TYPE_TUNNEL_TERM_TABLE_ENTRY,
                                        ipv6_tunnel_term_entry_key_1));
  EXPECT_FALSE(p4_oid_mapper_.existsOID(SAI_OBJECT_TYPE_TUNNEL_TERM_TABLE_ENTRY,
                                        ipv6_tunnel_term_entry_key_2));
  EXPECT_FALSE(p4_oid_mapper_.existsOID(SAI_OBJECT_TYPE_TUNNEL_TERM_TABLE_ENTRY,
                                        ipv6_tunnel_term_entry_key_3));

  EXPECT_THAT(gVrfOrch->getVrfRefCount(gVrfName), Eq(vrf_prev_ref_count));
}

TEST_F(TunnelDecapGroupManagerTest, DrainStopOnFirstFailureCreate) {
  uint32_t vrf_prev_ref_count = gVrfOrch->getVrfRefCount(gVrfName);

  std::vector<swss::FieldValueTuple> fvs{
      {p4orch::kAction, p4orch::kIpv6TunnelTermAction},
      {prependParamField(p4orch::kVrfId), gVrfName}};

  nlohmann::json j;
  j[prependMatchField(p4orch::kDecapSrcIpv6)] = kIpv6TunnelTermAppDbSrcIpMask1;
  j[prependMatchField(p4orch::kDecapDstIpv6)] = kIpv6TunnelTermAppDbDstIpMask1;
  swss::KeyOpFieldsValuesTuple app_db_entry_1(
      std::string(APP_P4RT_IPV6_TUNNEL_TERMINATION_TABLE_NAME) +
          kTableKeyDelimiter + j.dump(),
      SET_COMMAND, fvs);
  j[prependMatchField(p4orch::kDecapSrcIpv6)] = kIpv6TunnelTermAppDbSrcIpMask2;
  j[prependMatchField(p4orch::kDecapDstIpv6)] = kIpv6TunnelTermAppDbDstIpMask2;
  swss::KeyOpFieldsValuesTuple app_db_entry_2(
      std::string(APP_P4RT_IPV6_TUNNEL_TERMINATION_TABLE_NAME) +
          kTableKeyDelimiter + j.dump(),
      SET_COMMAND, fvs);
 j[prependMatchField(p4orch::kDecapSrcIpv6)] = kIpv6TunnelTermAppDbSrcIpMask3;
 j[prependMatchField(p4orch::kDecapDstIpv6)] = kIpv6TunnelTermAppDbDstIpMask3;
  swss::KeyOpFieldsValuesTuple app_db_entry_3(
      std::string(APP_P4RT_IPV6_TUNNEL_TERMINATION_TABLE_NAME) +
          kTableKeyDelimiter + j.dump(),
      SET_COMMAND, fvs);

  Enqueue(app_db_entry_1);
  Enqueue(app_db_entry_2);
  Enqueue(app_db_entry_3);

  // Set up mock call.
  EXPECT_CALL(mock_sai_tunnel_,
              create_tunnel_term_table_entry(
                  ::testing::NotNull(), Eq(gSwitchId), Eq(8),
                  AttrListEq(CreateSaiAttrs(kIpv6TunnelTermAppDbEntry1))))
      .WillOnce(DoAll(SetArgPointee<0>(kIpv6TunnelTermEntryOid1),
                      Return(SAI_STATUS_SUCCESS)));
  EXPECT_CALL(mock_sai_tunnel_,
              create_tunnel_term_table_entry(
                  ::testing::NotNull(), Eq(gSwitchId), Eq(8),
                  AttrListEq(CreateSaiAttrs(kIpv6TunnelTermAppDbEntry2))))
      .WillOnce(Return(SAI_STATUS_FAILURE));
  EXPECT_CALL(publisher_,
              publish(Eq(APP_P4RT_TABLE_NAME), Eq(kfvKey(app_db_entry_1)),
                      Eq(kfvFieldsValues(app_db_entry_1)),
                      Eq(StatusCode::SWSS_RC_SUCCESS), Eq(true)));
  EXPECT_CALL(publisher_,
              publish(Eq(APP_P4RT_TABLE_NAME), Eq(kfvKey(app_db_entry_2)),
                      Eq(kfvFieldsValues(app_db_entry_2)),
                      Eq(StatusCode::SWSS_RC_UNKNOWN), Eq(true)));
  EXPECT_CALL(publisher_,
              publish(Eq(APP_P4RT_TABLE_NAME), Eq(kfvKey(app_db_entry_3)),
                      Eq(kfvFieldsValues(app_db_entry_3)),
                      Eq(StatusCode::SWSS_RC_NOT_EXECUTED), Eq(true)));
  EXPECT_EQ(StatusCode::SWSS_RC_UNKNOWN, Drain(/*failure_before=*/false));

  const std::string ipv6_tunnel_term_entry_key_1 =
      KeyGenerator::generateIpv6TunnelTermKey(
          swss::IpAddress(kIpv6TunnelTermAppDbSrcIp1),
          swss::IpAddress(kIpv6TunnelTermAppDbSrcMask1),
          swss::IpAddress(kIpv6TunnelTermAppDbDstIp1),
          swss::IpAddress(kIpv6TunnelTermAppDbDstMask1));
  const std::string ipv6_tunnel_term_entry_key_2 =
      KeyGenerator::generateIpv6TunnelTermKey(
          swss::IpAddress(kIpv6TunnelTermAppDbSrcIp2),
          swss::IpAddress(kIpv6TunnelTermAppDbSrcMask2),
          swss::IpAddress(kIpv6TunnelTermAppDbDstIp2),
          swss::IpAddress(kIpv6TunnelTermAppDbDstMask2));
  const std::string ipv6_tunnel_term_entry_key_3 =
      KeyGenerator::generateIpv6TunnelTermKey(
          swss::IpAddress(kIpv6TunnelTermAppDbSrcIp3),
          swss::IpAddress(kIpv6TunnelTermAppDbSrcMask3),
          swss::IpAddress(kIpv6TunnelTermAppDbDstIp3),
          swss::IpAddress(kIpv6TunnelTermAppDbDstMask3));
  EXPECT_NE(nullptr, GetIpv6TunnelTermEntry(ipv6_tunnel_term_entry_key_1));
  EXPECT_EQ(nullptr, GetIpv6TunnelTermEntry(ipv6_tunnel_term_entry_key_2));
  EXPECT_EQ(nullptr, GetIpv6TunnelTermEntry(ipv6_tunnel_term_entry_key_3));

  EXPECT_TRUE(p4_oid_mapper_.existsOID(SAI_OBJECT_TYPE_TUNNEL_TERM_TABLE_ENTRY,
                                       ipv6_tunnel_term_entry_key_1));
  EXPECT_FALSE(p4_oid_mapper_.existsOID(SAI_OBJECT_TYPE_TUNNEL_TERM_TABLE_ENTRY,
                                        ipv6_tunnel_term_entry_key_2));
  EXPECT_FALSE(p4_oid_mapper_.existsOID(SAI_OBJECT_TYPE_TUNNEL_TERM_TABLE_ENTRY,
                                        ipv6_tunnel_term_entry_key_3));

  EXPECT_THAT(gVrfOrch->getVrfRefCount(gVrfName), Eq(vrf_prev_ref_count + 1));
}

TEST_F(TunnelDecapGroupManagerTest, DrainStopOnFirstFailureDel) {
  uint32_t vrf_prev_ref_count = gVrfOrch->getVrfRefCount(gVrfName);

  // Set up mock call.
  EXPECT_CALL(mock_sai_tunnel_,
              create_tunnel_term_table_entry(
                  ::testing::NotNull(), Eq(gSwitchId), Eq(8),
                  AttrListEq(CreateSaiAttrs(kIpv6TunnelTermAppDbEntry1))))
      .WillOnce(DoAll(SetArgPointee<0>(kIpv6TunnelTermEntryOid1),
                      Return(SAI_STATUS_SUCCESS)));

  EXPECT_THAT(CreateIpv6TunnelTermEntries(std::vector<Ipv6TunnelTermAppDbEntry>{
                  kIpv6TunnelTermAppDbEntry1}),
              ArrayEq(std::vector<StatusCode>{StatusCode::SWSS_RC_SUCCESS}));

  EXPECT_CALL(mock_sai_tunnel_,
              create_tunnel_term_table_entry(
                  ::testing::NotNull(), Eq(gSwitchId), Eq(8),
                  AttrListEq(CreateSaiAttrs(kIpv6TunnelTermAppDbEntry2))))
      .WillOnce(DoAll(SetArgPointee<0>(kIpv6TunnelTermEntryOid2),
                      Return(SAI_STATUS_SUCCESS)));

  EXPECT_THAT(CreateIpv6TunnelTermEntries(std::vector<Ipv6TunnelTermAppDbEntry>{
                  kIpv6TunnelTermAppDbEntry2}),
              ArrayEq(std::vector<StatusCode>{StatusCode::SWSS_RC_SUCCESS}));

  EXPECT_CALL(mock_sai_tunnel_,
              create_tunnel_term_table_entry(
                  ::testing::NotNull(), Eq(gSwitchId), Eq(8),
                  AttrListEq(CreateSaiAttrs(kIpv6TunnelTermAppDbEntry3))))
      .WillOnce(DoAll(SetArgPointee<0>(kIpv6TunnelTermEntryOid3),
                      Return(SAI_STATUS_SUCCESS)));

  EXPECT_THAT(CreateIpv6TunnelTermEntries(std::vector<Ipv6TunnelTermAppDbEntry>{
                  kIpv6TunnelTermAppDbEntry3}),
              ArrayEq(std::vector<StatusCode>{StatusCode::SWSS_RC_SUCCESS}));

  const std::string ipv6_tunnel_term_entry_key_1 =
      KeyGenerator::generateIpv6TunnelTermKey(
          swss::IpAddress(kIpv6TunnelTermAppDbSrcIp1),
          swss::IpAddress(kIpv6TunnelTermAppDbSrcMask1),
          swss::IpAddress(kIpv6TunnelTermAppDbDstIp1),
          swss::IpAddress(kIpv6TunnelTermAppDbDstMask1));
  const std::string ipv6_tunnel_term_entry_key_2 =
      KeyGenerator::generateIpv6TunnelTermKey(
          swss::IpAddress(kIpv6TunnelTermAppDbSrcIp2),
          swss::IpAddress(kIpv6TunnelTermAppDbSrcMask2),
          swss::IpAddress(kIpv6TunnelTermAppDbDstIp2),
          swss::IpAddress(kIpv6TunnelTermAppDbDstMask2));
  const std::string ipv6_tunnel_term_entry_key_3 =
      KeyGenerator::generateIpv6TunnelTermKey(
          swss::IpAddress(kIpv6TunnelTermAppDbSrcIp3),
          swss::IpAddress(kIpv6TunnelTermAppDbSrcMask3),
          swss::IpAddress(kIpv6TunnelTermAppDbDstIp3),
          swss::IpAddress(kIpv6TunnelTermAppDbDstMask3));
  EXPECT_NE(nullptr, GetIpv6TunnelTermEntry(ipv6_tunnel_term_entry_key_1));
  EXPECT_NE(nullptr, GetIpv6TunnelTermEntry(ipv6_tunnel_term_entry_key_2));
  EXPECT_NE(nullptr, GetIpv6TunnelTermEntry(ipv6_tunnel_term_entry_key_3));
  EXPECT_TRUE(p4_oid_mapper_.existsOID(SAI_OBJECT_TYPE_TUNNEL_TERM_TABLE_ENTRY,
                                       ipv6_tunnel_term_entry_key_1));
  EXPECT_TRUE(p4_oid_mapper_.existsOID(SAI_OBJECT_TYPE_TUNNEL_TERM_TABLE_ENTRY,
                                       ipv6_tunnel_term_entry_key_2));
  EXPECT_TRUE(p4_oid_mapper_.existsOID(SAI_OBJECT_TYPE_TUNNEL_TERM_TABLE_ENTRY,
                                       ipv6_tunnel_term_entry_key_3));

  std::vector<swss::FieldValueTuple> fvs{
      {p4orch::kAction, p4orch::kIpv6TunnelTermAction},
      {prependParamField(p4orch::kVrfId), gVrfName}};

  nlohmann::json j;
  j[prependMatchField(p4orch::kDecapSrcIpv6)] = kIpv6TunnelTermAppDbSrcIpMask1;
  j[prependMatchField(p4orch::kDecapDstIpv6)] = kIpv6TunnelTermAppDbDstIpMask1;
  swss::KeyOpFieldsValuesTuple app_db_entry_1(
      std::string(APP_P4RT_IPV6_TUNNEL_TERMINATION_TABLE_NAME) +
          kTableKeyDelimiter + j.dump(),
      DEL_COMMAND, fvs);
  j[prependMatchField(p4orch::kDecapSrcIpv6)] = kIpv6TunnelTermAppDbSrcIpMask2;
  j[prependMatchField(p4orch::kDecapDstIpv6)] = kIpv6TunnelTermAppDbDstIpMask2;
  swss::KeyOpFieldsValuesTuple app_db_entry_2(
      std::string(APP_P4RT_IPV6_TUNNEL_TERMINATION_TABLE_NAME) +
          kTableKeyDelimiter + j.dump(),
      DEL_COMMAND, fvs);
  j[prependMatchField(p4orch::kDecapSrcIpv6)] = kIpv6TunnelTermAppDbSrcIpMask3;
  j[prependMatchField(p4orch::kDecapDstIpv6)] = kIpv6TunnelTermAppDbDstIpMask3;
  swss::KeyOpFieldsValuesTuple app_db_entry_3(
      std::string(APP_P4RT_IPV6_TUNNEL_TERMINATION_TABLE_NAME) +
          kTableKeyDelimiter + j.dump(),
      DEL_COMMAND, fvs);

  Enqueue(app_db_entry_1);
  Enqueue(app_db_entry_2);
  Enqueue(app_db_entry_3);

  // Set up mock call.
  EXPECT_CALL(mock_sai_tunnel_,
              remove_tunnel_term_table_entry(Eq(kIpv6TunnelTermEntryOid1)))
      .WillOnce(Return(SAI_STATUS_SUCCESS));
  EXPECT_CALL(mock_sai_tunnel_,
              remove_tunnel_term_table_entry(Eq(kIpv6TunnelTermEntryOid2)))
      .WillOnce(Return(SAI_STATUS_FAILURE));
  EXPECT_CALL(publisher_,
              publish(Eq(APP_P4RT_TABLE_NAME), Eq(kfvKey(app_db_entry_1)),
                      Eq(kfvFieldsValues(app_db_entry_1)),
                      Eq(StatusCode::SWSS_RC_SUCCESS), Eq(true)));
  EXPECT_CALL(publisher_,
              publish(Eq(APP_P4RT_TABLE_NAME), Eq(kfvKey(app_db_entry_2)),
                      Eq(kfvFieldsValues(app_db_entry_2)),
                      Eq(StatusCode::SWSS_RC_UNKNOWN), Eq(true)));
  EXPECT_CALL(publisher_,
              publish(Eq(APP_P4RT_TABLE_NAME), Eq(kfvKey(app_db_entry_3)),
                      Eq(kfvFieldsValues(app_db_entry_3)),
                      Eq(StatusCode::SWSS_RC_NOT_EXECUTED), Eq(true)));
  EXPECT_EQ(StatusCode::SWSS_RC_UNKNOWN, Drain(/*failure_before=*/false));

  EXPECT_EQ(nullptr, GetIpv6TunnelTermEntry(ipv6_tunnel_term_entry_key_1));
  EXPECT_NE(nullptr, GetIpv6TunnelTermEntry(ipv6_tunnel_term_entry_key_2));
  EXPECT_NE(nullptr, GetIpv6TunnelTermEntry(ipv6_tunnel_term_entry_key_3));
  EXPECT_FALSE(p4_oid_mapper_.existsOID(SAI_OBJECT_TYPE_TUNNEL_TERM_TABLE_ENTRY,
                                        ipv6_tunnel_term_entry_key_1));
  EXPECT_TRUE(p4_oid_mapper_.existsOID(SAI_OBJECT_TYPE_TUNNEL_TERM_TABLE_ENTRY,
                                       ipv6_tunnel_term_entry_key_2));
  EXPECT_TRUE(p4_oid_mapper_.existsOID(SAI_OBJECT_TYPE_TUNNEL_TERM_TABLE_ENTRY,
                                       ipv6_tunnel_term_entry_key_3));

  EXPECT_THAT(gVrfOrch->getVrfRefCount(gVrfName), Eq(vrf_prev_ref_count + 2));
}

TEST_F(TunnelDecapGroupManagerTest, DrainStopOnFirstFailureDifferentTypes) {
  uint32_t vrf_prev_ref_count = gVrfOrch->getVrfRefCount(gVrfName);

  std::vector<swss::FieldValueTuple> fvs{
      {p4orch::kAction, p4orch::kIpv6TunnelTermAction},
      {prependParamField(p4orch::kVrfId), gVrfName}};

  nlohmann::json j;
  j[prependMatchField(p4orch::kDecapSrcIpv6)] = kIpv6TunnelTermAppDbSrcIpMask1;
  j[prependMatchField(p4orch::kDecapDstIpv6)] = kIpv6TunnelTermAppDbDstIpMask1;
  swss::KeyOpFieldsValuesTuple app_db_entry_1(
      std::string(APP_P4RT_IPV6_TUNNEL_TERMINATION_TABLE_NAME) +
          kTableKeyDelimiter + j.dump(),
      SET_COMMAND, fvs);
  j[prependMatchField(p4orch::kDecapSrcIpv6)] = kIpv6TunnelTermAppDbSrcIpMask2;
  j[prependMatchField(p4orch::kDecapDstIpv6)] = kIpv6TunnelTermAppDbDstIpMask2;
  swss::KeyOpFieldsValuesTuple app_db_entry_2(
      std::string(APP_P4RT_IPV6_TUNNEL_TERMINATION_TABLE_NAME) +
          kTableKeyDelimiter + j.dump(),
      SET_COMMAND, fvs);

  Enqueue(app_db_entry_2);
  Enqueue(app_db_entry_1);
  Enqueue(app_db_entry_2);

  // Set up mock call.
  EXPECT_CALL(mock_sai_tunnel_,
              create_tunnel_term_table_entry(
                  ::testing::NotNull(), Eq(gSwitchId), Eq(8),
                  AttrListEq(CreateSaiAttrs(kIpv6TunnelTermAppDbEntry2))))
      .WillOnce(DoAll(SetArgPointee<0>(kIpv6TunnelTermEntryOid2),
                      Return(SAI_STATUS_SUCCESS)));
  EXPECT_CALL(mock_sai_tunnel_,
              create_tunnel_term_table_entry(
                  ::testing::NotNull(), Eq(gSwitchId), Eq(8),
                  AttrListEq(CreateSaiAttrs(kIpv6TunnelTermAppDbEntry1))))
      .WillOnce(Return(SAI_STATUS_FAILURE));
  EXPECT_CALL(publisher_,
              publish(Eq(APP_P4RT_TABLE_NAME), Eq(kfvKey(app_db_entry_2)),
                      Eq(kfvFieldsValues(app_db_entry_2)),
                      Eq(StatusCode::SWSS_RC_SUCCESS), Eq(true)));
  EXPECT_CALL(publisher_,
              publish(Eq(APP_P4RT_TABLE_NAME), Eq(kfvKey(app_db_entry_1)),
                      Eq(kfvFieldsValues(app_db_entry_1)),
                      Eq(StatusCode::SWSS_RC_UNKNOWN), Eq(true)));
  EXPECT_CALL(publisher_,
              publish(Eq(APP_P4RT_TABLE_NAME), Eq(kfvKey(app_db_entry_2)),
                      Eq(kfvFieldsValues(app_db_entry_2)),
                      Eq(StatusCode::SWSS_RC_NOT_EXECUTED), Eq(true)));
  EXPECT_EQ(StatusCode::SWSS_RC_UNKNOWN, Drain(/*failure_before=*/false));

  const std::string ipv6_tunnel_term_entry_key_1 =
      KeyGenerator::generateIpv6TunnelTermKey(
          swss::IpAddress(kIpv6TunnelTermAppDbSrcIp1),
          swss::IpAddress(kIpv6TunnelTermAppDbSrcMask1),
          swss::IpAddress(kIpv6TunnelTermAppDbDstIp1),
          swss::IpAddress(kIpv6TunnelTermAppDbDstMask1));
  const std::string ipv6_tunnel_term_entry_key_2 =
      KeyGenerator::generateIpv6TunnelTermKey(
          swss::IpAddress(kIpv6TunnelTermAppDbSrcIp2),
          swss::IpAddress(kIpv6TunnelTermAppDbSrcMask2),
          swss::IpAddress(kIpv6TunnelTermAppDbDstIp2),
          swss::IpAddress(kIpv6TunnelTermAppDbDstMask2));
      
  EXPECT_EQ(nullptr, GetIpv6TunnelTermEntry(ipv6_tunnel_term_entry_key_1));
  EXPECT_NE(nullptr, GetIpv6TunnelTermEntry(ipv6_tunnel_term_entry_key_2));

  EXPECT_FALSE(p4_oid_mapper_.existsOID(SAI_OBJECT_TYPE_TUNNEL_TERM_TABLE_ENTRY,
                                        ipv6_tunnel_term_entry_key_1));
  EXPECT_TRUE(p4_oid_mapper_.existsOID(SAI_OBJECT_TYPE_TUNNEL_TERM_TABLE_ENTRY,
                                       ipv6_tunnel_term_entry_key_2));

  EXPECT_THAT(gVrfOrch->getVrfRefCount(gVrfName), Eq(vrf_prev_ref_count + 1));
}

TEST_F(TunnelDecapGroupManagerTest, DrainDifferentTypesWithDuplicateSetFails) {
  auto* ipv6_tunnel_term_table_entry = AddIpv6TunnelTermAppDbEntry1();
  ASSERT_NE(ipv6_tunnel_term_table_entry, nullptr);

  uint32_t vrf_prev_ref_count = gVrfOrch->getVrfRefCount(gVrfName);

  std::vector<swss::FieldValueTuple> fvs{
      {p4orch::kAction, p4orch::kIpv6TunnelTermAction},
      {prependParamField(p4orch::kVrfId), gVrfName}};

  nlohmann::json j;
  j[prependMatchField(p4orch::kDecapSrcIpv6)] = kIpv6TunnelTermAppDbSrcIpMask1;
  j[prependMatchField(p4orch::kDecapDstIpv6)] = kIpv6TunnelTermAppDbDstIpMask1;
  swss::KeyOpFieldsValuesTuple app_db_entry_1(
      std::string(APP_P4RT_IPV6_TUNNEL_TERMINATION_TABLE_NAME) +
          kTableKeyDelimiter + j.dump(),
      SET_COMMAND, fvs);
  j[prependMatchField(p4orch::kDecapSrcIpv6)] = kIpv6TunnelTermAppDbSrcIpMask2;
  j[prependMatchField(p4orch::kDecapDstIpv6)] = kIpv6TunnelTermAppDbDstIpMask2;
  swss::KeyOpFieldsValuesTuple app_db_entry_2(
      std::string(APP_P4RT_IPV6_TUNNEL_TERMINATION_TABLE_NAME) +
          kTableKeyDelimiter + j.dump(),
      SET_COMMAND, fvs);

  Enqueue(app_db_entry_1);
  Enqueue(app_db_entry_2);

  EXPECT_CALL(publisher_,
              publish(Eq(APP_P4RT_TABLE_NAME), Eq(kfvKey(app_db_entry_1)),
                      Eq(kfvFieldsValues(app_db_entry_1)),
                      Eq(StatusCode::SWSS_RC_UNIMPLEMENTED), Eq(true)));
  EXPECT_CALL(publisher_,
              publish(Eq(APP_P4RT_TABLE_NAME), Eq(kfvKey(app_db_entry_2)),
                      Eq(kfvFieldsValues(app_db_entry_2)),
                      Eq(StatusCode::SWSS_RC_NOT_EXECUTED), Eq(true)));
  EXPECT_EQ(StatusCode::SWSS_RC_UNIMPLEMENTED, Drain(/*failure_before=*/false));

  const std::string ipv6_tunnel_term_entry_key_1 =
      KeyGenerator::generateIpv6TunnelTermKey(
          swss::IpAddress(kIpv6TunnelTermAppDbSrcIp1),
          swss::IpAddress(kIpv6TunnelTermAppDbSrcMask1),
          swss::IpAddress(kIpv6TunnelTermAppDbDstIp1),
          swss::IpAddress(kIpv6TunnelTermAppDbDstMask1));
  const std::string ipv6_tunnel_term_entry_key_2 =
      KeyGenerator::generateIpv6TunnelTermKey(
          swss::IpAddress(kIpv6TunnelTermAppDbSrcIp2),
          swss::IpAddress(kIpv6TunnelTermAppDbSrcMask2),
          swss::IpAddress(kIpv6TunnelTermAppDbDstIp2),
          swss::IpAddress(kIpv6TunnelTermAppDbDstMask2));
  EXPECT_NE(nullptr, GetIpv6TunnelTermEntry(ipv6_tunnel_term_entry_key_1));
  EXPECT_EQ(nullptr, GetIpv6TunnelTermEntry(ipv6_tunnel_term_entry_key_2));

  EXPECT_TRUE(p4_oid_mapper_.existsOID(SAI_OBJECT_TYPE_TUNNEL_TERM_TABLE_ENTRY,
                                       ipv6_tunnel_term_entry_key_1));
  EXPECT_FALSE(p4_oid_mapper_.existsOID(SAI_OBJECT_TYPE_TUNNEL_TERM_TABLE_ENTRY,
                                        ipv6_tunnel_term_entry_key_2));

  EXPECT_THAT(gVrfOrch->getVrfRefCount(gVrfName), Eq(vrf_prev_ref_count));
}

TEST_F(TunnelDecapGroupManagerTest, GetIpv6TunnelTermEntrySucceed) {
  EXPECT_EQ(nullptr,
            GetIpv6TunnelTermEntry(KeyGenerator::generateIpv6TunnelTermKey(
                 swss::IpAddress("::2"), swss::IpAddress("::2"),
                swss::IpAddress("::1"), swss::IpAddress("::1"))));
}

TEST_F(TunnelDecapGroupManagerTest, CreateIpv6TunnelTermEntriesSucceed) {
  uint32_t vrf_prev_ref_count = gVrfOrch->getVrfRefCount(gVrfName);

  // Set up mock call.
  EXPECT_CALL(mock_sai_tunnel_,
              create_tunnel_term_table_entry(
                  ::testing::NotNull(), Eq(gSwitchId), Eq(8),
                  AttrListEq(CreateSaiAttrs(kIpv6TunnelTermAppDbEntry1))))
      .WillOnce(DoAll(SetArgPointee<0>(kIpv6TunnelTermEntryOid1),
                      Return(SAI_STATUS_SUCCESS)));

  EXPECT_THAT(CreateIpv6TunnelTermEntries(std::vector<Ipv6TunnelTermAppDbEntry>{
                  kIpv6TunnelTermAppDbEntry1}),
              ArrayEq(std::vector<StatusCode>{StatusCode::SWSS_RC_SUCCESS}));

  EXPECT_TRUE(ValidateIpv6TunnelTermEntryAdd(kIpv6TunnelTermAppDbEntry1));

  const std::string ipv6_tunnel_term_entry_key =
      KeyGenerator::generateIpv6TunnelTermKey(
          kIpv6TunnelTermAppDbEntry1.src_ipv6_ip,
          kIpv6TunnelTermAppDbEntry1.src_ipv6_mask,
          kIpv6TunnelTermAppDbEntry1.dst_ipv6_ip,
          kIpv6TunnelTermAppDbEntry1.dst_ipv6_mask);

  EXPECT_TRUE(p4_oid_mapper_.existsOID(SAI_OBJECT_TYPE_TUNNEL_TERM_TABLE_ENTRY,
                                       ipv6_tunnel_term_entry_key));

  EXPECT_THAT(gVrfOrch->getVrfRefCount(gVrfName), Eq(vrf_prev_ref_count + 1));
}

TEST_F(TunnelDecapGroupManagerTest,
       CreateIpv6TunnelTermEntriesShouldFailWhenTunnelSaiCallFails) {
  uint32_t vrf_prev_ref_count = gVrfOrch->getVrfRefCount(gVrfName);

  // Set up mock call.
  EXPECT_CALL(mock_sai_tunnel_,
              create_tunnel_term_table_entry(
                  ::testing::NotNull(), Eq(gSwitchId), Eq(8),
                  AttrListEq(CreateSaiAttrs(kIpv6TunnelTermAppDbEntry1))))
      .WillOnce(DoAll(SetArgPointee<0>(kIpv6TunnelTermEntryOid1),
                      Return(SAI_STATUS_FAILURE)));

  EXPECT_THAT(CreateIpv6TunnelTermEntries(std::vector<Ipv6TunnelTermAppDbEntry>{
                  kIpv6TunnelTermAppDbEntry1}),
              ArrayEq(std::vector<StatusCode>{StatusCode::SWSS_RC_UNKNOWN}));

  EXPECT_FALSE(ValidateIpv6TunnelTermEntryAdd(kIpv6TunnelTermAppDbEntry1));

  const std::string ipv6_tunnel_term_entry_key =
      KeyGenerator::generateIpv6TunnelTermKey(
          kIpv6TunnelTermAppDbEntry1.src_ipv6_ip,
          kIpv6TunnelTermAppDbEntry1.src_ipv6_mask,
          kIpv6TunnelTermAppDbEntry1.dst_ipv6_ip,
          kIpv6TunnelTermAppDbEntry1.dst_ipv6_mask);

  EXPECT_FALSE(p4_oid_mapper_.existsOID(SAI_OBJECT_TYPE_TUNNEL_TERM_TABLE_ENTRY,
                                        ipv6_tunnel_term_entry_key));

  EXPECT_THAT(gVrfOrch->getVrfRefCount(gVrfName), Eq(vrf_prev_ref_count));
}

TEST_F(TunnelDecapGroupManagerTest, RemoveIpv6TunnelTermEntriesSucceed) {
  auto* ipv6_tunnel_term_table_entry = AddIpv6TunnelTermAppDbEntry1();
  ASSERT_NE(ipv6_tunnel_term_table_entry, nullptr);

  uint32_t vrf_prev_ref_count = gVrfOrch->getVrfRefCount(gVrfName);

  // Set up mock call.
  EXPECT_CALL(mock_sai_tunnel_,
              remove_tunnel_term_table_entry(Eq(kIpv6TunnelTermEntryOid1)))
      .WillOnce(Return(SAI_STATUS_SUCCESS));

  EXPECT_THAT(RemoveIpv6TunnelTermEntries(std::vector<Ipv6TunnelTermAppDbEntry>{
                  kIpv6TunnelTermAppDbEntry1}),
              ArrayEq(std::vector<StatusCode>{StatusCode::SWSS_RC_SUCCESS}));

  const std::string ipv6_tunnel_term_entry_key =
      KeyGenerator::generateIpv6TunnelTermKey(
          kIpv6TunnelTermAppDbEntry1.src_ipv6_ip,
          kIpv6TunnelTermAppDbEntry1.src_ipv6_mask,
          kIpv6TunnelTermAppDbEntry1.dst_ipv6_ip,
          kIpv6TunnelTermAppDbEntry1.dst_ipv6_mask);

  EXPECT_FALSE(p4_oid_mapper_.existsOID(SAI_OBJECT_TYPE_TUNNEL_TERM_TABLE_ENTRY,
                                        ipv6_tunnel_term_entry_key));

  EXPECT_THAT(gVrfOrch->getVrfRefCount(gVrfName), Eq(vrf_prev_ref_count - 1));
}

TEST_F(TunnelDecapGroupManagerTest,
       RemoveIpv6TunnelTermEntriesShouldFailWhenTunnelSaiCallFails) {
  auto* ipv6_tunnel_term_table_entry = AddIpv6TunnelTermAppDbEntry1();
  ASSERT_NE(ipv6_tunnel_term_table_entry, nullptr);

  uint32_t vrf_prev_ref_count = gVrfOrch->getVrfRefCount(gVrfName);

  // Set up mock call.
  EXPECT_CALL(mock_sai_tunnel_,
              remove_tunnel_term_table_entry(Eq(kIpv6TunnelTermEntryOid1)))
      .WillOnce(Return(SAI_STATUS_FAILURE));

  EXPECT_THAT(RemoveIpv6TunnelTermEntries(std::vector<Ipv6TunnelTermAppDbEntry>{
                  kIpv6TunnelTermAppDbEntry1}),
              ArrayEq(std::vector<StatusCode>{StatusCode::SWSS_RC_UNKNOWN}));

  const std::string ipv6_tunnel_term_entry_key =
      KeyGenerator::generateIpv6TunnelTermKey(
          kIpv6TunnelTermAppDbEntry1.src_ipv6_ip,
          kIpv6TunnelTermAppDbEntry1.src_ipv6_mask,
          kIpv6TunnelTermAppDbEntry1.dst_ipv6_ip,
          kIpv6TunnelTermAppDbEntry1.dst_ipv6_mask);

  EXPECT_TRUE(p4_oid_mapper_.existsOID(SAI_OBJECT_TYPE_TUNNEL_TERM_TABLE_ENTRY,
                                       ipv6_tunnel_term_entry_key));

  EXPECT_THAT(gVrfOrch->getVrfRefCount(gVrfName), Eq(vrf_prev_ref_count));
}

TEST_F(TunnelDecapGroupManagerTest,
       DeserializeIpv6TunnelTermAppDbEntrySucceedForValidEntry) {
  std::vector<swss::FieldValueTuple> attributes = {
      swss::FieldValueTuple(p4orch::kAction, p4orch::kIpv6TunnelTermAction),
      swss::FieldValueTuple(prependParamField(p4orch::kVrfId), gVrfName)};

  auto result_or =
      DeserializeIpv6TunnelTermAppDbEntry(kIpv6TunnelTermAppDbKey1, attributes);
  ASSERT_TRUE(result_or.ok());
  auto result = *result_or;
  EXPECT_EQ(result.src_ipv6_ip, swss::IpAddress(kIpv6TunnelTermAppDbSrcIp1));
  EXPECT_EQ(result.src_ipv6_mask,
            swss::IpAddress(kIpv6TunnelTermAppDbSrcMask1));
  EXPECT_EQ(result.dst_ipv6_ip, swss::IpAddress(kIpv6TunnelTermAppDbDstIp1));
  EXPECT_EQ(result.dst_ipv6_mask,
            swss::IpAddress(kIpv6TunnelTermAppDbDstMask1));
}

TEST_F(TunnelDecapGroupManagerTest,
       DeserializeIpv6TunnelTermAppDbEntrySucceedForEmptyMatchField) {
  std::vector<swss::FieldValueTuple> attributes = {
      swss::FieldValueTuple(p4orch::kAction, p4orch::kIpv6TunnelTermAction),
      swss::FieldValueTuple(prependParamField(p4orch::kVrfId), gVrfName)};

  auto result_or = DeserializeIpv6TunnelTermAppDbEntry("{}", attributes);
  ASSERT_TRUE(result_or.ok());
  auto result = *result_or;
  EXPECT_EQ(result.src_ipv6_ip, swss::IpAddress("0:0:0:0:0:0:0:0"));
  EXPECT_EQ(result.src_ipv6_mask, swss::IpAddress("0:0:0:0:0:0:0:0"));
  EXPECT_EQ(result.dst_ipv6_ip, swss::IpAddress("0:0:0:0:0:0:0:0"));
  EXPECT_EQ(result.dst_ipv6_mask, swss::IpAddress("0:0:0:0:0:0:0:0"));
}

TEST_F(TunnelDecapGroupManagerTest,
       DeserializeIpv6TunnelTermAppDbEntryInvalidMatchKeyFormat) {
  std::vector<swss::FieldValueTuple> attributes = {
      swss::FieldValueTuple(p4orch::kAction, p4orch::kIpv6TunnelTermAction),
      swss::FieldValueTuple(prependParamField(p4orch::kVrfId), gVrfName)};

  EXPECT_FALSE(DeserializeIpv6TunnelTermAppDbEntry(
                   R"({"match/dst_ipv6":"2001:db8:3c4d:15::"})", attributes)
                   .ok());
}
TEST_F(TunnelDecapGroupManagerTest,
       DeserializeIpv6TunnelTermAppDbEntryInvalidMatchKeyFormat2) {
  std::vector<swss::FieldValueTuple> attributes = {
      swss::FieldValueTuple(p4orch::kAction, p4orch::kIpv6TunnelTermAction),
      swss::FieldValueTuple(prependParamField(p4orch::kVrfId), gVrfName)};
  // Missing the mask.
  EXPECT_FALSE(DeserializeIpv6TunnelTermAppDbEntry(
                   R"({"match/src_ipv6":"2001:db8:3c4d:15::"})", attributes)
                   .ok());
}

TEST_F(TunnelDecapGroupManagerTest,
       DeserializeIpv6TunnelTermAppDbEntryInvalidField) {
  std::vector<swss::FieldValueTuple> attributes = {
      swss::FieldValueTuple(p4orch::kAction, p4orch::kIpv6TunnelTermAction),
      swss::FieldValueTuple("UNKNOWN_FIELD", "UNKOWN")};

  EXPECT_FALSE(
      DeserializeIpv6TunnelTermAppDbEntry(kIpv6TunnelTermAppDbKey1, attributes)
          .ok());
}

TEST_F(TunnelDecapGroupManagerTest,
       ValidateIpv6TunnelTermAppDbEntryValidSetEntry) {
  EXPECT_TRUE(
      ValidateIpv6TunnelTermAppDbEntry(kIpv6TunnelTermAppDbEntry1, SET_COMMAND)
          .ok());
}

// If vrf_id is empty, the default VRF will be used.
TEST_F(TunnelDecapGroupManagerTest,
       ValidateIpv6TunnelTermAppDbEntryEmptyVrfID) {
  const Ipv6TunnelTermAppDbEntry app_db_entry{
       /*src_ipv6_ip=*/swss::IpAddress(kIpv6TunnelTermAppDbSrcIp1),
      /*src_ipv6_mask=*/swss::IpAddress(kIpv6TunnelTermAppDbSrcMask1),
      /*dst_ipv6_ip=*/swss::IpAddress("2001:db8:3c4d:15::"),
      /*dst_ipv6_mask=*/swss::IpAddress("ffff:ffff:ffff:ffff::"),
      /*vrf_id=*/"",
      /*action_str=*/"mark_for_tunnel_decap_and_set_vrf"};

  EXPECT_TRUE(ValidateIpv6TunnelTermAppDbEntry(app_db_entry, SET_COMMAND).ok());
}

TEST_F(TunnelDecapGroupManagerTest,
       ValidateIpv6TunnelTermAppDbEntryInvalidAction) {
  const Ipv6TunnelTermAppDbEntry app_db_entry{
       /*src_ipv6_ip=*/swss::IpAddress(kIpv6TunnelTermAppDbSrcIp1),
      /*src_ipv6_mask=*/swss::IpAddress(kIpv6TunnelTermAppDbSrcMask1),
      /*dst_ipv6_ip=*/swss::IpAddress("2001:db8:3c4d:15::"),
      /*dst_ipv6_mask=*/swss::IpAddress("ffff:ffff:ffff:ffff::"),
      /*vrf_id=*/gVrfName,
      /*action_str=*/"invalid_action"};

  EXPECT_FALSE(
      ValidateIpv6TunnelTermAppDbEntry(app_db_entry, SET_COMMAND).ok());
}

TEST_F(TunnelDecapGroupManagerTest,
       ValidateIpv6TunnelTermAppDbEntryNonxistVrf) {
  const Ipv6TunnelTermAppDbEntry app_db_entry{
      /*src_ipv6_ip=*/swss::IpAddress(kIpv6TunnelTermAppDbSrcIp1),
      /*src_ipv6_mask=*/swss::IpAddress(kIpv6TunnelTermAppDbSrcMask1),
      /*dst_ipv6_ip=*/swss::IpAddress("2001:db8:3c4d:15::"),
      /*dst_ipv6_mask=*/swss::IpAddress("ffff:ffff:ffff:ffff::"),
      /*vrf_id=*/"nonexist_vrf_id",
      /*action_str=*/"mark_for_tunnel_decap_and_set_vrf"};

  EXPECT_FALSE(
      ValidateIpv6TunnelTermAppDbEntry(app_db_entry, SET_COMMAND).ok());
}

TEST_F(TunnelDecapGroupManagerTest, ValidateIpv6TunnelTermAppDbEntryDstIPisV4) {
  const Ipv6TunnelTermAppDbEntry app_db_entry{
      /*src_ipv6_ip=*/swss::IpAddress(kIpv6TunnelTermAppDbSrcIp1),
      /*src_ipv6_mask=*/swss::IpAddress(kIpv6TunnelTermAppDbSrcMask1),
      /*dst_ipv6_ip=*/swss::IpAddress("0.0.0.1"),
      /*dst_ipv6_mask=*/swss::IpAddress("ffff:ffff:ffff:ffff::"),
      /*vrf_id=*/gVrfName,
      /*action_str=*/"mark_for_tunnel_decap_and_set_vrf"};

  EXPECT_FALSE(
      ValidateIpv6TunnelTermAppDbEntry(app_db_entry, SET_COMMAND).ok());
}

TEST_F(TunnelDecapGroupManagerTest,
       ValidateIpv6TunnelTermAppDbEntryDstMaskisV4) {
  const Ipv6TunnelTermAppDbEntry app_db_entry{
      /*src_ipv6_ip=*/swss::IpAddress(kIpv6TunnelTermAppDbSrcIp1),
      /*src_ipv6_mask=*/swss::IpAddress(kIpv6TunnelTermAppDbSrcMask1),
      /*dst_ipv6_ip=*/swss::IpAddress("2001:db8:3c4d:15::"),
      /*dst_ipv6_mask=*/swss::IpAddress("0.0.0.1"),
      /*vrf_id=*/gVrfName,
      /*action_str=*/"mark_for_tunnel_decap_and_set_vrf"};

  EXPECT_FALSE(
      ValidateIpv6TunnelTermAppDbEntry(app_db_entry, SET_COMMAND).ok());
}
TEST_F(TunnelDecapGroupManagerTest, ValidateIpv6TunnelTermAppDbEntrySrcIPisV4) {
  const Ipv6TunnelTermAppDbEntry app_db_entry{
      /*src_ipv6_ip=*/swss::IpAddress("0.0.0.1"),
      /*src_ipv6_mask=*/swss::IpAddress(kIpv6TunnelTermAppDbSrcMask1),
      /*dst_ipv6_ip=*/swss::IpAddress(kIpv6TunnelTermAppDbSrcIp1),
      /*dst_ipv6_mask=*/swss::IpAddress("ffff:ffff:ffff:ffff::"),
      /*vrf_id=*/gVrfName,
      /*action_str=*/"mark_for_tunnel_decap_and_set_vrf"};
  EXPECT_FALSE(
      ValidateIpv6TunnelTermAppDbEntry(app_db_entry, SET_COMMAND).ok());
}
TEST_F(TunnelDecapGroupManagerTest,
       ValidateIpv6TunnelTermAppDbEntrySrcMaskisV4) {
  const Ipv6TunnelTermAppDbEntry app_db_entry{
      /*src_ipv6_ip=*/swss::IpAddress(kIpv6TunnelTermAppDbSrcIp1),
      /*src_ipv6_mask=*/swss::IpAddress("255.255.255.255"),
      /*dst_ipv6_ip=*/swss::IpAddress("2001:db8:3c4d:15::"),
      /*dst_ipv6_mask=*/swss::IpAddress(kIpv6TunnelTermAppDbSrcMask1),
      /*vrf_id=*/gVrfName,
      /*action_str=*/"mark_for_tunnel_decap_and_set_vrf"};
  EXPECT_FALSE(
      ValidateIpv6TunnelTermAppDbEntry(app_db_entry, SET_COMMAND).ok());
}


TEST_F(TunnelDecapGroupManagerTest,
       ValidateIpv6TunnelTermAppDbEntryDefaulIpMask) {
  const Ipv6TunnelTermAppDbEntry app_db_entry{
     /*src_ipv6_ip=*/swss::IpAddress(kIpv6TunnelTermAppDbSrcIp1),
      /*src_ipv6_mask=*/swss::IpAddress(kIpv6TunnelTermAppDbSrcMask1),  
     /*dst_ipv6_ip=*/swss::IpAddress("0:0:0:0:0:0:0:0"),
      /*dst_ipv6_mask=*/swss::IpAddress("0:0:0:0:0:0:0:0"),
      /*vrf_id=*/gVrfName,
      /*action_str=*/"mark_for_tunnel_decap_and_set_vrf"};

  EXPECT_TRUE(ValidateIpv6TunnelTermAppDbEntry(app_db_entry, SET_COMMAND).ok());
}

TEST_F(TunnelDecapGroupManagerTest,
       ValidateIpv6TunnelTermAppDbEntryDefaulSrcIpMask) {
  const Ipv6TunnelTermAppDbEntry app_db_entry{
      /*src_ipv6_ip=*/swss::IpAddress("0:0:0:0:0:0:0:0"),
      /*src_ipv6_mask=*/swss::IpAddress("0:0:0:0:0:0:0:0"),
      /*dst_ipv6_ip=*/swss::IpAddress(kIpv6TunnelTermAppDbSrcIp1),
      /*dst_ipv6_mask=*/swss::IpAddress(kIpv6TunnelTermAppDbSrcMask1),
      /*vrf_id=*/gVrfName,
      /*action_str=*/"mark_for_tunnel_decap_and_set_vrf"};
  EXPECT_TRUE(ValidateIpv6TunnelTermAppDbEntry(app_db_entry, SET_COMMAND).ok());
}


TEST_F(TunnelDecapGroupManagerTest,
       ValidateIpv6TunnelTermAppDbEntryMapperOidExistsForCreate) {
  const std::string ipv6_tunnel_term_entry_key =
      KeyGenerator::generateIpv6TunnelTermKey(
          kIpv6TunnelTermAppDbEntry1.src_ipv6_ip,
          kIpv6TunnelTermAppDbEntry1.src_ipv6_mask,
          kIpv6TunnelTermAppDbEntry1.dst_ipv6_ip,
          kIpv6TunnelTermAppDbEntry1.dst_ipv6_mask);

  ASSERT_TRUE(p4_oid_mapper_.setOID(SAI_OBJECT_TYPE_TUNNEL_TERM_TABLE_ENTRY,
                                    ipv6_tunnel_term_entry_key,
                                    kIpv6TunnelTermEntryOid1));

  EXPECT_FALSE(
      ValidateIpv6TunnelTermAppDbEntry(kIpv6TunnelTermAppDbEntry1, SET_COMMAND)
          .ok());
}

TEST_F(TunnelDecapGroupManagerTest,
       ValidateIpv6TunnelTermAppDbEntryValidDelEntry) {
  auto* ipv6_tunnel_term_table_entry = AddIpv6TunnelTermAppDbEntry1();
  ASSERT_NE(ipv6_tunnel_term_table_entry, nullptr);

  EXPECT_TRUE(
      ValidateIpv6TunnelTermAppDbEntry(kIpv6TunnelTermAppDbEntry1, DEL_COMMAND)
          .ok());
}

TEST_F(TunnelDecapGroupManagerTest,
       ValidateIpv6TunnelTermAppDbEntryDelEntryNotFound) {
  EXPECT_FALSE(
      ValidateIpv6TunnelTermAppDbEntry(kIpv6TunnelTermAppDbEntry1, DEL_COMMAND)
          .ok());
}

TEST_F(TunnelDecapGroupManagerTest,
       ValidateIpv6TunnelTermAppDbEntryOidMapperEntryNotFoundInDel) {
  auto* ipv6_tunnel_term_table_entry = AddIpv6TunnelTermAppDbEntry1();
  ASSERT_NE(ipv6_tunnel_term_table_entry, nullptr);

  ASSERT_TRUE(p4_oid_mapper_.eraseOID(
      SAI_OBJECT_TYPE_TUNNEL_TERM_TABLE_ENTRY,
      ipv6_tunnel_term_table_entry->ipv6_tunnel_term_key));

  EXPECT_FALSE(
      ValidateIpv6TunnelTermAppDbEntry(kIpv6TunnelTermAppDbEntry1, DEL_COMMAND)
          .ok());
}

TEST_F(TunnelDecapGroupManagerTest,
       ValidateIpv6TunnelTermAppDbEntryRefCntNotZeroInDel) {
  auto* ipv6_tunnel_term_table_entry = AddIpv6TunnelTermAppDbEntry1();
  ASSERT_NE(ipv6_tunnel_term_table_entry, nullptr);

  ASSERT_TRUE(p4_oid_mapper_.increaseRefCount(
      SAI_OBJECT_TYPE_TUNNEL_TERM_TABLE_ENTRY,
      ipv6_tunnel_term_table_entry->ipv6_tunnel_term_key));

  EXPECT_FALSE(
      ValidateIpv6TunnelTermAppDbEntry(kIpv6TunnelTermAppDbEntry1, DEL_COMMAND)
          .ok());
}

TEST_F(TunnelDecapGroupManagerTest, VerifyStateTest) {
  auto* ipv6_tunnel_term_table_entry = AddIpv6TunnelTermAppDbEntry1();
  ASSERT_NE(ipv6_tunnel_term_table_entry, nullptr);

  // Setup ASIC DB.
  swss::Table table(nullptr, "ASIC_STATE");
  table.set(
      "SAI_OBJECT_TYPE_TUNNEL_TERM_TABLE_ENTRY:oid:0x11",
      std::vector<swss::FieldValueTuple>{
          swss::FieldValueTuple{"SAI_TUNNEL_TERM_TABLE_ENTRY_ATTR_TUNNEL_TYPE",
                                "SAI_TUNNEL_TYPE_IPINIP"},
          swss::FieldValueTuple{"SAI_TUNNEL_TERM_TABLE_ENTRY_ATTR_TYPE",
                                "SAI_TUNNEL_TERM_TABLE_ENTRY_TYPE_MP2MP"},
          swss::FieldValueTuple{"SAI_TUNNEL_TERM_TABLE_ENTRY_ATTR_SRC_IP",
                                "4001:db8:3c4d:19::"},
          swss::FieldValueTuple{"SAI_TUNNEL_TERM_TABLE_ENTRY_ATTR_SRC_IP_MASK",
                                "ffff:ffff:ffff:ffff::"},
          swss::FieldValueTuple{"SAI_TUNNEL_TERM_TABLE_ENTRY_ATTR_DST_IP",
                                "2001:db8:3c4d:15::"},
          swss::FieldValueTuple{"SAI_TUNNEL_TERM_TABLE_ENTRY_ATTR_DST_IP_MASK",
                                "ffff:ffff:ffff:ffff::"},
          swss::FieldValueTuple{"SAI_TUNNEL_TERM_TABLE_ENTRY_ATTR_VR_ID",
                                "oid:0x6f"},
          swss::FieldValueTuple{
              "SAI_TUNNEL_TERM_TABLE_ENTRY_ATTR_ACTION_TUNNEL_ID",
              "oid:0x10"}});

  nlohmann::json j;
  j[prependMatchField(p4orch::kDecapSrcIpv6)] = kIpv6TunnelTermAppDbSrcIpMask1;
  j[prependMatchField(p4orch::kDecapDstIpv6)] = kIpv6TunnelTermAppDbDstIpMask1;
  const std::string db_key = std::string(APP_P4RT_TABLE_NAME) +
                             kTableKeyDelimiter +
                             APP_P4RT_IPV6_TUNNEL_TERMINATION_TABLE_NAME +
                             kTableKeyDelimiter + j.dump();

  std::vector<swss::FieldValueTuple> attributes = {
      swss::FieldValueTuple(p4orch::kAction, p4orch::kIpv6TunnelTermAction),
      swss::FieldValueTuple(prependParamField(p4orch::kVrfId), gVrfName)};

  EXPECT_EQ(VerifyState(db_key, attributes), "");

  // Invalid key should fail verification.
  EXPECT_FALSE(VerifyState("invalid", attributes).empty());
  EXPECT_FALSE(VerifyState("invalid:invalid", attributes).empty());
  EXPECT_FALSE(
      VerifyState(std::string(APP_P4RT_TABLE_NAME) + ":invalid", attributes)
          .empty());
  EXPECT_FALSE(
      VerifyState(std::string(APP_P4RT_TABLE_NAME) + ":invalid:invalid",
                  attributes)
          .empty());
  EXPECT_FALSE(VerifyState(std::string(APP_P4RT_TABLE_NAME) +
                               ":FIXED_IPV6_TUNNEL_TERMINATION_TABLE:invalid",
                           attributes)
                   .empty());

  // Verification should fail if entry does not exist.
  j[prependMatchField(p4orch::kDecapDstIpv6)] = "invalid";
  EXPECT_FALSE(VerifyState(std::string(APP_P4RT_TABLE_NAME) +
                               kTableKeyDelimiter +
                               APP_P4RT_IPV6_TUNNEL_TERMINATION_TABLE_NAME +
                               kTableKeyDelimiter + j.dump(),
                           attributes)
                   .empty());

  auto saved_ipv6_tunnel_term_key =
      ipv6_tunnel_term_table_entry->ipv6_tunnel_term_key;
  ipv6_tunnel_term_table_entry->ipv6_tunnel_term_key = "invalid";
  EXPECT_FALSE(VerifyState(db_key, attributes).empty());
  ipv6_tunnel_term_table_entry->ipv6_tunnel_term_key =
      saved_ipv6_tunnel_term_key;
  auto saved_src_ipv6_ip = ipv6_tunnel_term_table_entry->src_ipv6_ip;
  ipv6_tunnel_term_table_entry->src_ipv6_ip = swss::IpAddress("1.1.1.1");
  EXPECT_FALSE(VerifyState(db_key, attributes).empty());
  ipv6_tunnel_term_table_entry->src_ipv6_ip = saved_src_ipv6_ip;
  auto saved_src_ipv6_mask = ipv6_tunnel_term_table_entry->src_ipv6_mask;
  ipv6_tunnel_term_table_entry->src_ipv6_mask = swss::IpAddress("1.1.1.1");
  EXPECT_FALSE(VerifyState(db_key, attributes).empty());
  ipv6_tunnel_term_table_entry->src_ipv6_mask = saved_src_ipv6_mask;  

  auto saved_dst_ipv6_ip = ipv6_tunnel_term_table_entry->dst_ipv6_ip;
  ipv6_tunnel_term_table_entry->dst_ipv6_ip = swss::IpAddress("1.1.1.1");
  EXPECT_FALSE(VerifyState(db_key, attributes).empty());
  ipv6_tunnel_term_table_entry->dst_ipv6_ip = saved_dst_ipv6_ip;

  auto saved_dst_ipv6_mask = ipv6_tunnel_term_table_entry->dst_ipv6_mask;
  ipv6_tunnel_term_table_entry->dst_ipv6_mask = swss::IpAddress("1.1.1.1");
  EXPECT_FALSE(VerifyState(db_key, attributes).empty());
  ipv6_tunnel_term_table_entry->dst_ipv6_mask = saved_dst_ipv6_mask;

  auto saved_vrf_id = ipv6_tunnel_term_table_entry->vrf_id;
  ipv6_tunnel_term_table_entry->vrf_id = "invalid";
  EXPECT_FALSE(VerifyState(db_key, attributes).empty());
  ipv6_tunnel_term_table_entry->vrf_id = saved_vrf_id;

  const std::string ipv6_tunnel_term_entry_key =
       KeyGenerator::generateIpv6TunnelTermKey(
          kIpv6TunnelTermAppDbEntry1.src_ipv6_ip,
          kIpv6TunnelTermAppDbEntry1.src_ipv6_mask,
          kIpv6TunnelTermAppDbEntry1.dst_ipv6_ip,
          kIpv6TunnelTermAppDbEntry1.dst_ipv6_mask);
 
  p4_oid_mapper_.eraseOID(SAI_OBJECT_TYPE_TUNNEL_TERM_TABLE_ENTRY,
                          ipv6_tunnel_term_entry_key);
  EXPECT_FALSE(VerifyState(db_key, attributes).empty());
  p4_oid_mapper_.setOID(SAI_OBJECT_TYPE_TUNNEL_TERM_TABLE_ENTRY,
                        ipv6_tunnel_term_entry_key, kIpv6TunnelTermEntryOid1);
}

TEST_F(TunnelDecapGroupManagerTest, VerifyStateAsicDbTest) {
  auto* ipv6_tunnel_term_table_entry = AddIpv6TunnelTermAppDbEntry1();
  ASSERT_NE(ipv6_tunnel_term_table_entry, nullptr);

  // Setup ASIC DB.
  swss::Table table(nullptr, "ASIC_STATE");
  table.set(
      "SAI_OBJECT_TYPE_TUNNEL_TERM_TABLE_ENTRY:oid:0x11",
      std::vector<swss::FieldValueTuple>{
          swss::FieldValueTuple{"SAI_TUNNEL_TERM_TABLE_ENTRY_ATTR_TUNNEL_TYPE",
                                "SAI_TUNNEL_TYPE_IPINIP"},
          swss::FieldValueTuple{"SAI_TUNNEL_TERM_TABLE_ENTRY_ATTR_TYPE",
                                "SAI_TUNNEL_TERM_TABLE_ENTRY_TYPE_MP2MP"},
          swss::FieldValueTuple{"SAI_TUNNEL_TERM_TABLE_ENTRY_ATTR_SRC_IP",
                                "4001:db8:3c4d:19::"},
          swss::FieldValueTuple{"SAI_TUNNEL_TERM_TABLE_ENTRY_ATTR_SRC_IP_MASK",
                                "ffff:ffff:ffff:ffff::"},
          swss::FieldValueTuple{"SAI_TUNNEL_TERM_TABLE_ENTRY_ATTR_DST_IP",
                                "2001:db8:3c4d:15::"},
          swss::FieldValueTuple{"SAI_TUNNEL_TERM_TABLE_ENTRY_ATTR_DST_IP_MASK",
                                "ffff:ffff:ffff:ffff::"},
          swss::FieldValueTuple{"SAI_TUNNEL_TERM_TABLE_ENTRY_ATTR_VR_ID",
                                "oid:0x6f"},
          swss::FieldValueTuple{
              "SAI_TUNNEL_TERM_TABLE_ENTRY_ATTR_ACTION_TUNNEL_ID",
              "oid:0x10"}});

  nlohmann::json j;
  j[prependMatchField(p4orch::kDecapSrcIpv6)] = kIpv6TunnelTermAppDbSrcIpMask1; 
  j[prependMatchField(p4orch::kDecapDstIpv6)] = kIpv6TunnelTermAppDbDstIpMask1;
  const std::string db_key = std::string(APP_P4RT_TABLE_NAME) +
                             kTableKeyDelimiter +
                             APP_P4RT_IPV6_TUNNEL_TERMINATION_TABLE_NAME +
                             kTableKeyDelimiter + j.dump();

  std::vector<swss::FieldValueTuple> attributes = {
      swss::FieldValueTuple(p4orch::kAction, p4orch::kIpv6TunnelTermAction),
      swss::FieldValueTuple(prependParamField(p4orch::kVrfId), gVrfName)};

  EXPECT_EQ(VerifyState(db_key, attributes), "");

  table.set("SAI_OBJECT_TYPE_TUNNEL_TERM_TABLE_ENTRY:oid:0x11",
            std::vector<swss::FieldValueTuple>{swss::FieldValueTuple{
                "SAI_TUNNEL_TERM_TABLE_ENTRY_ATTR_DST_IP", "0.0.0.0"}});
  EXPECT_FALSE(VerifyState(db_key, attributes).empty());

  table.del("SAI_OBJECT_TYPE_TUNNEL_TERM_TABLE_ENTRY:oid:0x11");
  EXPECT_FALSE(VerifyState(db_key, attributes).empty());

  table.set(
      "SAI_OBJECT_TYPE_TUNNEL_TERM_TABLE_ENTRY:oid:0x11",
      std::vector<swss::FieldValueTuple>{
          swss::FieldValueTuple{"SAI_TUNNEL_TERM_TABLE_ENTRY_ATTR_TUNNEL_TYPE",
                                "SAI_TUNNEL_TYPE_IPINIP"},
          swss::FieldValueTuple{"SAI_TUNNEL_TERM_TABLE_ENTRY_ATTR_TYPE",
                                "SAI_TUNNEL_TERM_TABLE_ENTRY_TYPE_MP2MP"},
          swss::FieldValueTuple{"SAI_TUNNEL_TERM_TABLE_ENTRY_ATTR_SRC_IP",
                                "4001:db8:3c4d:15::"},
          swss::FieldValueTuple{"SAI_TUNNEL_TERM_TABLE_ENTRY_ATTR_SRC_IP_MASK",
                                "ffff:ffff:ffff:ffff::"},
          swss::FieldValueTuple{"SAI_TUNNEL_TERM_TABLE_ENTRY_ATTR_DST_IP",
                                "2001:db8:3c4d:15::"},
          swss::FieldValueTuple{"SAI_TUNNEL_TERM_TABLE_ENTRY_ATTR_DST_IP_MASK",
                                "ffff:ffff:ffff:ffff::"},
          swss::FieldValueTuple{"SAI_TUNNEL_TERM_TABLE_ENTRY_ATTR_VR_ID",
                                "oid:0x6f"},
          swss::FieldValueTuple{
              "SAI_TUNNEL_TERM_TABLE_ENTRY_ATTR_ACTION_TUNNEL_ID",
              "oid:0x10"}});

  ipv6_tunnel_term_table_entry->dst_ipv6_ip = swss::IpAddress("1.2.3.4");
  EXPECT_FALSE(VerifyState(db_key, attributes).empty());
}

