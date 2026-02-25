#include "p4orch/mirror_session_manager.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#include "mock_response_publisher.h"
#include "mock_sai_mirror.h"
#include "p4oidmapper.h"
#include "p4orch_util.h"
#include "portsorch.h"
#include "swss/ipaddress.h"
#include "swss/macaddress.h"
#include "swssnet.h"
extern "C"
{
#include "sai.h"
}

using ::p4orch::kTableKeyDelimiter;

extern sai_mirror_api_t *sai_mirror_api;
extern sai_object_id_t gSwitchId;
extern PortsOrch *gPortsOrch;

using ::testing::_;
using ::testing::DoAll;
using ::testing::Eq;
using ::testing::Return;
using ::testing::SetArgPointee;
using ::testing::StrictMock;
using ::testing::Truly;

namespace p4orch
{
namespace test
{
namespace
{

constexpr char *kMirrorSessionId = "mirror_session1";
constexpr sai_object_id_t kMirrorSessionOid = 0x445566;
// A physical port set up in test_main.cpp
constexpr char *kPort1 = "Ethernet1";
constexpr sai_object_id_t kPort1Oid = 0x112233;
// A management port set up in test_main.cpp
constexpr char *kPort2 = "Ethernet8";
// A physical port set up in test_main.cpp
constexpr char *kPort3 = "Ethernet3";
constexpr char* kFailoverPort0 = "";
constexpr sai_object_id_t kPort3Oid = 0xaabbccdd;
constexpr char *kSrcIp1 = "10.206.196.31";
constexpr char *kSrcIp2 = "10.206.196.32";
constexpr char* kSrcIpv6 = "1200::2001:db8:1";
constexpr char* kDstIpv6 = "1300::2001:db8:2";
constexpr char* kSrcIpv6_2 = "2200::2001:db8:11";
constexpr char* kDstIpv6_2 = "2300::2001:db8:12";
constexpr char *kDstIp1 = "172.20.0.203";
constexpr char *kDstIp2 = "172.20.0.204";
constexpr char *kSrcMac1 = "00:02:03:04:05:06";
constexpr char *kSrcMac2 = "00:02:03:04:05:07";
constexpr char *kDstMac1 = "00:1a:11:17:5f:80";
constexpr char *kDstMac2 = "00:1a:11:17:5f:81";
constexpr char *kTtl1 = "0x40";
constexpr char *kTtl2 = "0x41";
constexpr uint8_t kTtl1Num = 0x40;
constexpr uint8_t kTtl2Num = 0x41;
constexpr char *kTos1 = "0x00";
constexpr char *kTos2 = "0x01";
constexpr uint8_t kTos1Num = 0x00;
constexpr uint8_t kTos2Num = 0x01;
constexpr char* kVlanId1 = "0x1234";
constexpr char* kVlanId2 = "0x5678";
constexpr uint16_t kVlanIdNum0 = 0;
constexpr uint16_t kVlanIdNum1 = 0x1234;
constexpr uint16_t kVlanIdNum2 = 0x5678;
constexpr char* kUdpSrcPort1 = "0x80";
constexpr char* kUdpSrcPort2 = "0x90";
constexpr uint16_t kUdpSrcPortNum0 = 0;
constexpr uint16_t kUdpSrcPortNum1 = 0x80;
constexpr uint16_t kUdpSrcPortNum2 = 0x90;
constexpr char* kUdpDstPort1 = "0x110";
constexpr char* kUdpDstPort2 = "0x120";
constexpr uint16_t kUdpDstPortNum0 = 0;
constexpr uint16_t kUdpDstPortNum1 = 0x110;
constexpr uint16_t kUdpDstPortNum2 = 0x120;

// Generates attribute list for create_mirror_session().
std::vector<sai_attribute_t> GenerateAttrListForCreate(sai_object_id_t port_oid, uint8_t ttl, uint8_t tos,
                                                       const swss::IpAddress &src_ip, const swss::IpAddress &dst_ip,
                                                       const swss::MacAddress &src_mac, const swss::MacAddress &dst_mac)
{
    std::vector<sai_attribute_t> attrs;
    sai_attribute_t attr;

    attr.id = SAI_MIRROR_SESSION_ATTR_MONITOR_PORT;
    attr.value.oid = port_oid;
    attrs.push_back(attr);

    attr.id = SAI_MIRROR_SESSION_ATTR_TYPE;
    attr.value.s32 = SAI_MIRROR_SESSION_TYPE_ENHANCED_REMOTE;
    attrs.push_back(attr);

    attr.id = SAI_MIRROR_SESSION_ATTR_ERSPAN_ENCAPSULATION_TYPE;
    attr.value.s32 = SAI_ERSPAN_ENCAPSULATION_TYPE_MIRROR_L3_GRE_TUNNEL;
    attrs.push_back(attr);

    attr.id = SAI_MIRROR_SESSION_ATTR_IPHDR_VERSION;
    attr.value.u8 = MIRROR_SESSION_DEFAULT_IP_HDR_VER;
    attrs.push_back(attr);

    attr.id = SAI_MIRROR_SESSION_ATTR_TOS;
    attr.value.u8 = tos;
    attrs.push_back(attr);

    attr.id = SAI_MIRROR_SESSION_ATTR_TTL;
    attr.value.u8 = ttl;
    attrs.push_back(attr);

    attr.id = SAI_MIRROR_SESSION_ATTR_SRC_IP_ADDRESS;
    swss::copy(attr.value.ipaddr, src_ip);
    attrs.push_back(attr);

    attr.id = SAI_MIRROR_SESSION_ATTR_DST_IP_ADDRESS;
    swss::copy(attr.value.ipaddr, dst_ip);
    attrs.push_back(attr);

    attr.id = SAI_MIRROR_SESSION_ATTR_SRC_MAC_ADDRESS;
    memcpy(attr.value.mac, src_mac.getMac(), sizeof(sai_mac_t));
    attrs.push_back(attr);

    attr.id = SAI_MIRROR_SESSION_ATTR_DST_MAC_ADDRESS;
    memcpy(attr.value.mac, dst_mac.getMac(), sizeof(sai_mac_t));
    attrs.push_back(attr);

    attr.id = SAI_MIRROR_SESSION_ATTR_GRE_PROTOCOL_TYPE;
    attr.value.u16 = GRE_PROTOCOL_ERSPAN;
    attrs.push_back(attr);

    return attrs;
}

std::vector<sai_attribute_t> GenerateAttrListForIpfixCreate(
    sai_object_id_t port_oid, sai_object_id_t failover_port_oid,
    const swss::IpAddress& src_ip, const swss::IpAddress& dst_ip,
    const swss::MacAddress& src_mac, const swss::MacAddress& dst_mac,
    uint16_t vlan_id, uint16_t udp_src_port, uint16_t udp_dst_port) {
  std::vector<sai_attribute_t> attrs;
  sai_attribute_t attr;

  #ifdef SAI_MIRROR_SESSION_TYPE_IPFIX
      attr.id = SAI_MIRROR_SESSION_ATTR_TYPE;
      attr.value.s32 = SAI_MIRROR_SESSION_TYPE_IPFIX;
      attrs.push_back(attr);

      attr.id = SAI_MIRROR_SESSION_ATTR_IPFIX_ENCAPSULATION_TYPE;
      attr.value.s32 = SAI_IPFIX_ENCAPSULATION_TYPE_EXTENDED;
      attrs.push_back(attr);

  #endif

  attr.id = SAI_MIRROR_SESSION_ATTR_IPHDR_VERSION;
  attr.value.u8 = 6;
  attrs.push_back(attr);

  attr.id = SAI_MIRROR_SESSION_ATTR_TOS;
  attr.value.u8 = 0;
  attrs.push_back(attr);

  attr.id = SAI_MIRROR_SESSION_ATTR_MONITOR_PORT;
  attr.value.oid = port_oid;
  attrs.push_back(attr);

  // Not supported yet.
  /*
  attr.id = SAI_MIRROR_SESSION_ATTR_MONITOR_FAILOVER_PORT;
  attr.value.oid = failover_port.m_port_id;
  attrs.push_back(attr);
  */

  attr.id = SAI_MIRROR_SESSION_ATTR_SRC_MAC_ADDRESS;
  memcpy(attr.value.mac, src_mac.getMac(), sizeof(sai_mac_t));
  attrs.push_back(attr);

  attr.id = SAI_MIRROR_SESSION_ATTR_DST_MAC_ADDRESS;
  memcpy(attr.value.mac, dst_mac.getMac(), sizeof(sai_mac_t));
  attrs.push_back(attr);

  attr.id = SAI_MIRROR_SESSION_ATTR_VLAN_TPID;
  attr.value.u16 = MIRROR_SESSION_DEFAULT_VLAN_TPID;
  attrs.push_back(attr);

  attr.id = SAI_MIRROR_SESSION_ATTR_VLAN_ID;
  attr.value.u16 = vlan_id;
  attrs.push_back(attr);

  attr.id = SAI_MIRROR_SESSION_ATTR_SRC_IP_ADDRESS;
  swss::copy(attr.value.ipaddr, src_ip);
  attrs.push_back(attr);

  attr.id = SAI_MIRROR_SESSION_ATTR_DST_IP_ADDRESS;
  swss::copy(attr.value.ipaddr, dst_ip);
  attrs.push_back(attr);

  attr.id = SAI_MIRROR_SESSION_ATTR_UDP_SRC_PORT;
  attr.value.u16 = udp_src_port;
  attrs.push_back(attr);

  attr.id = SAI_MIRROR_SESSION_ATTR_UDP_DST_PORT;
  attr.value.u16 = udp_dst_port;
  attrs.push_back(attr);

  return attrs;
}

// Matcher for attribute list in SAI mirror call.
// Returns true if attribute lists have the same values in the same order.
bool MatchSaiCallAttrList(const sai_attribute_t *attr_list, const std::vector<sai_attribute_t> &expected_attr_list)
{
    if (attr_list == nullptr)
    {
        return false;
    }

    for (uint i = 0; i < expected_attr_list.size(); ++i)
    {
        switch (attr_list[i].id)
        {
        case SAI_MIRROR_SESSION_ATTR_MONITOR_PORT:
            if (attr_list[i].value.oid != expected_attr_list[i].value.oid)
            {
                return false;
            }
            break;

        case SAI_MIRROR_SESSION_ATTR_TYPE:
        case SAI_MIRROR_SESSION_ATTR_ERSPAN_ENCAPSULATION_TYPE:
      #ifdef SAI_MIRROR_SESSION_ATTR_IPFIX_ENCAPSULATION_TYPE
        case SAI_MIRROR_SESSION_ATTR_IPFIX_ENCAPSULATION_TYPE:
      #endif
            if (attr_list[i].value.s32 != expected_attr_list[i].value.s32)
            {
                return false;
            }
            break;

        case SAI_MIRROR_SESSION_ATTR_IPHDR_VERSION:
        case SAI_MIRROR_SESSION_ATTR_TOS:
        case SAI_MIRROR_SESSION_ATTR_TTL:
            if (attr_list[i].value.u8 != expected_attr_list[i].value.u8)
            {
                return false;
            }
            break;

        case SAI_MIRROR_SESSION_ATTR_GRE_PROTOCOL_TYPE:
        case SAI_MIRROR_SESSION_ATTR_UDP_SRC_PORT:
        case SAI_MIRROR_SESSION_ATTR_UDP_DST_PORT:
        case SAI_MIRROR_SESSION_ATTR_VLAN_ID:
        case SAI_MIRROR_SESSION_ATTR_VLAN_TPID:
            if (attr_list[i].value.u16 != expected_attr_list[i].value.u16)
            {
                return false;
            }
            break;

        case SAI_MIRROR_SESSION_ATTR_SRC_IP_ADDRESS:
        case SAI_MIRROR_SESSION_ATTR_DST_IP_ADDRESS:
            if (attr_list[i].value.ipaddr.addr_family != expected_attr_list[i].value.ipaddr.addr_family ||
                (attr_list[i].value.ipaddr.addr_family == SAI_IP_ADDR_FAMILY_IPV4 &&
                 attr_list[i].value.ipaddr.addr.ip4 != expected_attr_list[i].value.ipaddr.addr.ip4) ||
                (attr_list[i].value.ipaddr.addr_family == SAI_IP_ADDR_FAMILY_IPV6 &&
                 memcmp(&attr_list[i].value.ipaddr.addr.ip6, &expected_attr_list[i].value.ipaddr.addr.ip6,
                        sizeof(sai_ip6_t)) != 0))
            {
                return false;
            }
            break;

        case SAI_MIRROR_SESSION_ATTR_SRC_MAC_ADDRESS:
        case SAI_MIRROR_SESSION_ATTR_DST_MAC_ADDRESS:
            if (memcmp(&attr_list[i].value.mac, &expected_attr_list[i].value.mac, sizeof(sai_mac_t)) != 0)
            {
                return false;
            }
            break;

        default:
            return false;
        }
    }

    return true;
}

} // namespace

class MirrorSessionManagerTest : public ::testing::Test
{
  protected:
    MirrorSessionManagerTest() : mirror_session_manager_(&p4_oid_mapper_, &publisher_)
    {
    }

    void SetUp() override
    {
        // Set up mock stuff for SAI mirror API structure.
        mock_sai_mirror = &mock_sai_mirror_;
        sai_mirror_api->create_mirror_session = mock_create_mirror_session;
        sai_mirror_api->remove_mirror_session = mock_remove_mirror_session;
        sai_mirror_api->set_mirror_session_attribute = mock_set_mirror_session_attribute;
        sai_mirror_api->get_mirror_session_attribute = mock_get_mirror_session_attribute;
    }

    void Enqueue(const swss::KeyOpFieldsValuesTuple &entry)
    {
        return mirror_session_manager_.enqueue(APP_P4RT_MIRROR_SESSION_TABLE_NAME, entry);
    }

    ReturnCode Drain(bool failure_before) {
      if (failure_before) {
        mirror_session_manager_.drainWithNotExecuted();
        return ReturnCode(StatusCode::SWSS_RC_NOT_EXECUTED);
      }
      return mirror_session_manager_.drain();
    }

    std::string VerifyState(const std::string &key, const std::vector<swss::FieldValueTuple> &tuple)
    {
        return mirror_session_manager_.verifyState(key, tuple);
    }

    ReturnCodeOr<P4MirrorSessionAppDbEntry> DeserializeP4MirrorSessionAppDbEntry(
        const std::string &key, const std::vector<swss::FieldValueTuple> &attributes)
    {
        return mirror_session_manager_.deserializeP4MirrorSessionAppDbEntry(key, attributes);
    }

    p4orch::P4MirrorSessionEntry *GetMirrorSessionEntry(const std::string &mirror_session_key)
    {
        return mirror_session_manager_.getMirrorSessionEntry(mirror_session_key);
    }

    ReturnCode ValidateMirrorSessionEntry(
        const P4MirrorSessionAppDbEntry& mirror_session_entry,
        const std::string& operation) {
        return mirror_session_manager_.validateMirrorSessionEntry(
            mirror_session_entry, operation);
    }

    ReturnCode ProcessAddRequest(const P4MirrorSessionAppDbEntry &app_db_entry)
    {
        return mirror_session_manager_.processAddRequest(app_db_entry);
    }

    ReturnCode CreateMirrorSession(p4orch::P4MirrorSessionEntry mirror_session_entry)
    {
        return mirror_session_manager_.createMirrorSession(mirror_session_entry);
    }

    ReturnCode ProcessUpdateRequest(const P4MirrorSessionAppDbEntry &app_db_entry,
                                    p4orch::P4MirrorSessionEntry *existing_mirror_session_entry)
    {
        return mirror_session_manager_.processUpdateRequest(app_db_entry, existing_mirror_session_entry);
    }

    ReturnCode SetPort(const std::string &new_port, p4orch::P4MirrorSessionEntry *existing_mirror_session_entry)
    {
        return mirror_session_manager_.setPort(new_port, existing_mirror_session_entry);
    }

    ReturnCode SetSrcIp(const swss::IpAddress &new_src_ip, p4orch::P4MirrorSessionEntry *existing_mirror_session_entry)
    {
        return mirror_session_manager_.setSrcIp(new_src_ip, existing_mirror_session_entry);
    }

    ReturnCode SetDstIp(const swss::IpAddress &new_dst_ip, p4orch::P4MirrorSessionEntry *existing_mirror_session_entry)
    {
        return mirror_session_manager_.setDstIp(new_dst_ip, existing_mirror_session_entry);
    }

    ReturnCode SetSrcMac(const swss::MacAddress &new_src_mac,
                         p4orch::P4MirrorSessionEntry *existing_mirror_session_entry)
    {
        return mirror_session_manager_.setSrcMac(new_src_mac, existing_mirror_session_entry);
    }

    ReturnCode SetDstMac(const swss::MacAddress &new_dst_mac,
                         p4orch::P4MirrorSessionEntry *existing_mirror_session_entry)
    {
        return mirror_session_manager_.setDstMac(new_dst_mac, existing_mirror_session_entry);
    }

    ReturnCode SetTtl(uint8_t new_ttl, p4orch::P4MirrorSessionEntry *existing_mirror_session_entry)
    {
        return mirror_session_manager_.setTtl(new_ttl, existing_mirror_session_entry);
    }

    ReturnCode SetTos(uint8_t new_tos, p4orch::P4MirrorSessionEntry *existing_mirror_session_entry)
    {
        return mirror_session_manager_.setTos(new_tos, existing_mirror_session_entry);
    }

    ReturnCode SetVlanId(
        uint16_t new_vlan_id,
        p4orch::P4MirrorSessionEntry* existing_mirror_session_entry) {
        return mirror_session_manager_.setVlanId(new_vlan_id,
                                                 existing_mirror_session_entry);
    }

    ReturnCode SetUdpSrcPort(
        uint16_t new_udp_src_port,
        p4orch::P4MirrorSessionEntry* existing_mirror_session_entry) {
        return mirror_session_manager_.setUdpSrcPort(new_udp_src_port,
                                                     existing_mirror_session_entry);
    }

    ReturnCode SetUdpDstPort(
        uint16_t new_udp_dst_port,
        p4orch::P4MirrorSessionEntry* existing_mirror_session_entry) {
      return mirror_session_manager_.setUdpDstPort(new_udp_dst_port,
                                                 existing_mirror_session_entry);
    }

    ReturnCode ProcessDeleteRequest(const std::string &mirror_session_key)
    {
        return mirror_session_manager_.processDeleteRequest(mirror_session_key);
    }

    void VerifyP4MirrorSessionAppDbEntryEqual(
        const P4MirrorSessionAppDbEntry& x, const P4MirrorSessionAppDbEntry& y) {
      EXPECT_EQ(x.mirror_session_id, y.mirror_session_id);
      EXPECT_EQ(x.action, y.action);
      EXPECT_EQ(x.port, y.port);
      EXPECT_EQ(x.src_ip, y.src_ip);
      EXPECT_EQ(x.dst_ip, y.dst_ip);
      EXPECT_EQ(
          0, memcmp(x.src_mac.getMac(), y.src_mac.getMac(), sizeof(sai_mac_t)));
      EXPECT_EQ(
          0, memcmp(x.dst_mac.getMac(), y.dst_mac.getMac(), sizeof(sai_mac_t)));
      EXPECT_EQ(x.ttl, y.ttl);
      EXPECT_EQ(x.tos, y.tos);
      EXPECT_EQ(x.failover_port, y.failover_port);
      EXPECT_EQ(x.vlan_id, y.vlan_id);
      EXPECT_EQ(x.udp_src_port, y.udp_src_port);
      EXPECT_EQ(x.udp_dst_port, y.udp_dst_port);
    }

    P4MirrorSessionAppDbEntry AddDefaultMirrorSession() {
        P4MirrorSessionAppDbEntry app_db_entry;
        app_db_entry.mirror_session_id = kMirrorSessionId;
        app_db_entry.has_action = true;
        app_db_entry.action = p4orch::kMirrorAsIpv4Erspan;
        app_db_entry.has_port = true;
        app_db_entry.port = kPort1;
        app_db_entry.has_src_ip = true;
        app_db_entry.src_ip = swss::IpAddress(kSrcIp1);
        app_db_entry.has_dst_ip = true;
        app_db_entry.dst_ip = swss::IpAddress(kDstIp1);
        app_db_entry.has_src_mac = true;
        app_db_entry.src_mac = swss::MacAddress(kSrcMac1);
        app_db_entry.has_dst_mac = true;
        app_db_entry.dst_mac = swss::MacAddress(kDstMac1);
        app_db_entry.has_ttl = true;
        app_db_entry.ttl = kTtl1Num;
        app_db_entry.has_tos = true;
        app_db_entry.tos = kTos1Num;
        EXPECT_CALL(mock_sai_mirror_,
                    create_mirror_session(::testing::NotNull(), Eq(gSwitchId), Eq(11),
                                          Truly(std::bind(MatchSaiCallAttrList, std::placeholders::_1,
                                                          GenerateAttrListForCreate(
                                                              kPort1Oid, kTtl1Num, kTos1Num, swss::IpAddress(kSrcIp1),
                                                              swss::IpAddress(kDstIp1), swss::MacAddress(kSrcMac1),
                                                              swss::MacAddress(kDstMac1))))))
            .WillOnce(DoAll(SetArgPointee<0>(kMirrorSessionOid), Return(SAI_STATUS_SUCCESS)));
        EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS, ProcessAddRequest(app_db_entry));
        return app_db_entry;
    }

    P4MirrorSessionAppDbEntry AddDefaultIpfixMirrorEntry() {
      P4MirrorSessionAppDbEntry app_db_entry;
      app_db_entry.mirror_session_id = kMirrorSessionId;
      app_db_entry.has_action = true;
      app_db_entry.action = p4orch::kMirrorWithVlanTagAndIpfixEncapsulation;
      app_db_entry.has_port = true;
      app_db_entry.port = kPort1;
      app_db_entry.has_failover_port = true;
      app_db_entry.failover_port = kPort3;
      app_db_entry.has_src_ip = true;
      app_db_entry.src_ip = swss::IpAddress(kSrcIpv6);
      app_db_entry.has_dst_ip = true;
      app_db_entry.dst_ip = swss::IpAddress(kDstIpv6);
      app_db_entry.has_src_mac = true;
      app_db_entry.src_mac = swss::MacAddress(kSrcMac1);
      app_db_entry.has_dst_mac = true;
      app_db_entry.dst_mac = swss::MacAddress(kDstMac1);
      app_db_entry.has_vlan_id = true;
      app_db_entry.vlan_id = kVlanIdNum1;
      app_db_entry.has_udp_src_port = true;
      app_db_entry.udp_src_port = kUdpSrcPortNum1;
      app_db_entry.has_udp_dst_port = true;
      app_db_entry.udp_dst_port = kUdpDstPortNum1;
      app_db_entry.tos = kTos1Num;
      app_db_entry.has_tos = false;

      EXPECT_CALL(
          mock_sai_mirror_,
          create_mirror_session(
              ::testing::NotNull(), Eq(gSwitchId), Eq(13),
              Truly(std::bind(
                  MatchSaiCallAttrList, std::placeholders::_1,
                  GenerateAttrListForIpfixCreate(
                      kPort1Oid, kPort3Oid, swss::IpAddress(kSrcIpv6),
                      swss::IpAddress(kDstIpv6), swss::MacAddress(kSrcMac1),
                      swss::MacAddress(kDstMac1), kVlanIdNum1, kUdpSrcPortNum1,
                      kUdpDstPortNum1)))))
          .WillOnce(DoAll(SetArgPointee<0>(kMirrorSessionOid),
                          Return(SAI_STATUS_SUCCESS)));
      EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS, ProcessAddRequest(app_db_entry));
      return app_db_entry;
      }

    StrictMock<MockSaiMirror> mock_sai_mirror_;
    StrictMock<MockResponsePublisher> publisher_;
    P4OidMapper p4_oid_mapper_;
    p4orch::MirrorSessionManager mirror_session_manager_;
};

// Do add, update and delete serially.
TEST_F(MirrorSessionManagerTest, SuccessfulEnqueueAndDrain)
{
    // 1. Add a new entry.
    nlohmann::json j;
    j[prependMatchField(p4orch::kMirrorSessionId)] = kMirrorSessionId;

    std::vector<swss::FieldValueTuple> fvs{
        {p4orch::kAction, p4orch::kMirrorAsIpv4Erspan}, {prependParamField(p4orch::kPort), kPort1},
        {prependParamField(p4orch::kSrcIp), kSrcIp1},   {prependParamField(p4orch::kDstIp), kDstIp1},
        {prependParamField(p4orch::kSrcMac), kSrcMac1}, {prependParamField(p4orch::kDstMac), kDstMac1},
        {prependParamField(p4orch::kTtl), kTtl1},       {prependParamField(p4orch::kTos), kTos1}};

    swss::KeyOpFieldsValuesTuple app_db_entry(
        std::string(APP_P4RT_MIRROR_SESSION_TABLE_NAME) + kTableKeyDelimiter + j.dump(), SET_COMMAND, fvs);

    Enqueue(app_db_entry);
    // Set up mock call.
    EXPECT_CALL(mock_sai_mirror_,
                create_mirror_session(
                    ::testing::NotNull(), Eq(gSwitchId), Eq(11),
                    Truly(std::bind(MatchSaiCallAttrList, std::placeholders::_1,
                                    GenerateAttrListForCreate(kPort1Oid, kTtl1Num, kTos1Num, swss::IpAddress(kSrcIp1),
                                                              swss::IpAddress(kDstIp1), swss::MacAddress(kSrcMac1),
                                                              swss::MacAddress(kDstMac1))))))
        .WillOnce(DoAll(SetArgPointee<0>(kMirrorSessionOid), Return(SAI_STATUS_SUCCESS)));
    EXPECT_CALL(publisher_,
                publish(Eq(APP_P4RT_TABLE_NAME), Eq(kfvKey(app_db_entry)),
                        Eq(kfvFieldsValues(app_db_entry)),
                        Eq(StatusCode::SWSS_RC_SUCCESS), Eq(true)));
    EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS, Drain(/*failure_before=*/false));

    // Check the added entry.
    auto mirror_entry = GetMirrorSessionEntry(KeyGenerator::generateMirrorSessionKey(kMirrorSessionId));
    ASSERT_NE(mirror_entry, nullptr);
    p4orch::P4MirrorSessionEntry expected_mirror_entry(
        KeyGenerator::generateMirrorSessionKey(kMirrorSessionId),
        kMirrorSessionOid, kMirrorSessionId, p4orch::kMirrorAsIpv4Erspan, kPort1,
        swss::IpAddress(kSrcIp1), swss::IpAddress(kDstIp1),
        swss::MacAddress(kSrcMac1), swss::MacAddress(kDstMac1), kTtl1Num,
        kTos1Num, kFailoverPort0, kVlanIdNum0, kUdpSrcPortNum0, kUdpDstPortNum0);
    EXPECT_EQ(*mirror_entry, expected_mirror_entry);

    sai_object_id_t oid_in_mapper = 0;
    EXPECT_TRUE(p4_oid_mapper_.getOID(SAI_OBJECT_TYPE_MIRROR_SESSION,
                                      KeyGenerator::generateMirrorSessionKey(kMirrorSessionId), &oid_in_mapper));
    EXPECT_EQ(kMirrorSessionOid, oid_in_mapper);

    // 2. Update the added entry.
    fvs = {{p4orch::kAction, p4orch::kMirrorAsIpv4Erspan}, {prependParamField(p4orch::kPort), kPort3},
           {prependParamField(p4orch::kSrcIp), kSrcIp2},   {prependParamField(p4orch::kDstIp), kDstIp2},
           {prependParamField(p4orch::kSrcMac), kSrcMac2}, {prependParamField(p4orch::kDstMac), kDstMac2},
           {prependParamField(p4orch::kTtl), kTtl2},       {prependParamField(p4orch::kTos), kTos2}};

    app_db_entry = {std::string(APP_P4RT_MIRROR_SESSION_TABLE_NAME) + kTableKeyDelimiter + j.dump(), SET_COMMAND, fvs};

    Enqueue(app_db_entry);
    // Set up mock call.
    sai_attribute_t attr;
    attr.id = SAI_MIRROR_SESSION_ATTR_MONITOR_PORT;
    attr.value.oid = kPort3Oid;
    EXPECT_CALL(
        mock_sai_mirror_,
        set_mirror_session_attribute(Eq(kMirrorSessionOid), Truly(std::bind(MatchSaiCallAttrList, std::placeholders::_1,
                                                                            std::vector<sai_attribute_t>({attr})))))
        .WillOnce(Return(SAI_STATUS_SUCCESS));

    attr.id = SAI_MIRROR_SESSION_ATTR_SRC_IP_ADDRESS;
    swss::copy(attr.value.ipaddr, swss::IpAddress(kSrcIp2));
    EXPECT_CALL(
        mock_sai_mirror_,
        set_mirror_session_attribute(Eq(kMirrorSessionOid), Truly(std::bind(MatchSaiCallAttrList, std::placeholders::_1,
                                                                            std::vector<sai_attribute_t>({attr})))))
        .WillOnce(Return(SAI_STATUS_SUCCESS));

    attr.id = SAI_MIRROR_SESSION_ATTR_DST_IP_ADDRESS;
    swss::copy(attr.value.ipaddr, swss::IpAddress(kDstIp2));
    EXPECT_CALL(
        mock_sai_mirror_,
        set_mirror_session_attribute(Eq(kMirrorSessionOid), Truly(std::bind(MatchSaiCallAttrList, std::placeholders::_1,
                                                                            std::vector<sai_attribute_t>({attr})))))
        .WillOnce(Return(SAI_STATUS_SUCCESS));

    attr.id = SAI_MIRROR_SESSION_ATTR_SRC_MAC_ADDRESS;
    memcpy(attr.value.mac, swss::MacAddress(kSrcMac2).getMac(), sizeof(sai_mac_t));
    EXPECT_CALL(
        mock_sai_mirror_,
        set_mirror_session_attribute(Eq(kMirrorSessionOid), Truly(std::bind(MatchSaiCallAttrList, std::placeholders::_1,
                                                                            std::vector<sai_attribute_t>({attr})))))
        .WillOnce(Return(SAI_STATUS_SUCCESS));

    attr.id = SAI_MIRROR_SESSION_ATTR_DST_MAC_ADDRESS;
    memcpy(attr.value.mac, swss::MacAddress(kDstMac2).getMac(), sizeof(sai_mac_t));
    EXPECT_CALL(
        mock_sai_mirror_,
        set_mirror_session_attribute(Eq(kMirrorSessionOid), Truly(std::bind(MatchSaiCallAttrList, std::placeholders::_1,
                                                                            std::vector<sai_attribute_t>({attr})))))
        .WillOnce(Return(SAI_STATUS_SUCCESS));

    attr.id = SAI_MIRROR_SESSION_ATTR_TTL;
    attr.value.u8 = kTtl2Num;
    EXPECT_CALL(
        mock_sai_mirror_,
        set_mirror_session_attribute(Eq(kMirrorSessionOid), Truly(std::bind(MatchSaiCallAttrList, std::placeholders::_1,
                                                                            std::vector<sai_attribute_t>({attr})))))
        .WillOnce(Return(SAI_STATUS_SUCCESS));

    attr.id = SAI_MIRROR_SESSION_ATTR_TOS;
    attr.value.u8 = kTos2Num;
    EXPECT_CALL(
        mock_sai_mirror_,
        set_mirror_session_attribute(Eq(kMirrorSessionOid), Truly(std::bind(MatchSaiCallAttrList, std::placeholders::_1,
                                                                            std::vector<sai_attribute_t>({attr})))))
        .WillOnce(Return(SAI_STATUS_SUCCESS));

    EXPECT_CALL(publisher_,
                publish(Eq(APP_P4RT_TABLE_NAME), Eq(kfvKey(app_db_entry)),
                        Eq(kfvFieldsValues(app_db_entry)),
                        Eq(StatusCode::SWSS_RC_SUCCESS), Eq(true)));
    EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS, Drain(/*failure_before=*/false));

    mirror_entry = GetMirrorSessionEntry(KeyGenerator::generateMirrorSessionKey(kMirrorSessionId));
    ASSERT_NE(mirror_entry, nullptr);
    expected_mirror_entry.port = kPort3;
    expected_mirror_entry.src_ip = swss::IpAddress(kSrcIp2);
    expected_mirror_entry.dst_ip = swss::IpAddress(kDstIp2);
    expected_mirror_entry.src_mac = swss::MacAddress(kSrcMac2);
    expected_mirror_entry.dst_mac = swss::MacAddress(kDstMac2);
    expected_mirror_entry.ttl = kTtl2Num;
    expected_mirror_entry.tos = kTos2Num;
    EXPECT_EQ(*mirror_entry, expected_mirror_entry);

    // 3. Delete the entry.
    fvs = {};
    app_db_entry = {std::string(APP_P4RT_MIRROR_SESSION_TABLE_NAME) + kTableKeyDelimiter + j.dump(), DEL_COMMAND, fvs};

    Enqueue(app_db_entry);
    // Set up mock call.
    EXPECT_CALL(mock_sai_mirror_, remove_mirror_session(Eq(kMirrorSessionOid))).WillOnce(Return(SAI_STATUS_SUCCESS));
    EXPECT_CALL(publisher_,
                publish(Eq(APP_P4RT_TABLE_NAME), Eq(kfvKey(app_db_entry)),
                        Eq(kfvFieldsValues(app_db_entry)),
                        Eq(StatusCode::SWSS_RC_SUCCESS), Eq(true)));
    EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS, Drain(/*failure_before=*/false));

    mirror_entry = GetMirrorSessionEntry(KeyGenerator::generateMirrorSessionKey(kMirrorSessionId));
    EXPECT_EQ(mirror_entry, nullptr);

    EXPECT_FALSE(p4_oid_mapper_.existsOID(SAI_OBJECT_TYPE_MIRROR_SESSION,
                                          KeyGenerator::generateMirrorSessionKey(kMirrorSessionId)));
}

TEST_F(MirrorSessionManagerTest, SuccessfulEnqueueAndDrainIpfixAction) {
    // 1. Add a new entry.
    nlohmann::json j;
    j[prependMatchField(p4orch::kMirrorSessionId)] = kMirrorSessionId;

    std::vector<swss::FieldValueTuple> fvs{
        {p4orch::kAction, p4orch::kMirrorWithVlanTagAndIpfixEncapsulation},
        {prependParamField(p4orch::kMonitorPort), kPort1},
        {prependParamField(p4orch::kMonitorFailoverPort), kPort3},
        {prependParamField(p4orch::kMirrorEncapSrcIp), kSrcIpv6},
        {prependParamField(p4orch::kMirrorEncapDstIp), kDstIpv6},
        {prependParamField(p4orch::kMirrorEncapSrcMac), kSrcMac1},
        {prependParamField(p4orch::kMirrorEncapDstMac), kDstMac1},
        {prependParamField(p4orch::kMirrorEncapVlanId), kVlanId1},
        {prependParamField(p4orch::kMirrorEncapUdpSrcPort), kUdpSrcPort1},
        {prependParamField(p4orch::kMirrorEncapUdpDstPort), kUdpDstPort1}};

    swss::KeyOpFieldsValuesTuple app_db_entry(
        std::string(APP_P4RT_MIRROR_SESSION_TABLE_NAME) + kTableKeyDelimiter + j.dump(), SET_COMMAND, fvs);

    Enqueue(app_db_entry);
    // Set up mock call.
    EXPECT_CALL(mock_sai_mirror_,
              create_mirror_session(
                  ::testing::NotNull(), Eq(gSwitchId), Eq(13),
                  Truly(std::bind(
                      MatchSaiCallAttrList, std::placeholders::_1,
                      GenerateAttrListForIpfixCreate(
                          kPort1Oid, kPort3Oid, swss::IpAddress(kSrcIpv6),
                          swss::IpAddress(kDstIpv6), swss::MacAddress(kSrcMac1),
                          swss::MacAddress(kDstMac1), kVlanIdNum1,
                          kUdpSrcPortNum1, kUdpDstPortNum1)))))
        .WillOnce(DoAll(SetArgPointee<0>(kMirrorSessionOid),
                        Return(SAI_STATUS_SUCCESS)));
    EXPECT_CALL(publisher_,
                publish(Eq(APP_P4RT_TABLE_NAME), Eq(kfvKey(app_db_entry)),
                        Eq(kfvFieldsValues(app_db_entry)),
                        Eq(StatusCode::SWSS_RC_SUCCESS), Eq(true)));
    EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS, Drain(/*failure_before=*/false));

    // Check the added entry.
    auto mirror_entry = GetMirrorSessionEntry(KeyGenerator::generateMirrorSessionKey(kMirrorSessionId));
    ASSERT_NE(mirror_entry, nullptr);
    p4orch::P4MirrorSessionEntry expected_mirror_entry(
        KeyGenerator::generateMirrorSessionKey(kMirrorSessionId),
        kMirrorSessionOid, kMirrorSessionId,
        p4orch::kMirrorWithVlanTagAndIpfixEncapsulation, kPort1,
        swss::IpAddress(kSrcIpv6), swss::IpAddress(kDstIpv6),
        swss::MacAddress(kSrcMac1), swss::MacAddress(kDstMac1), /*ttl=*/0,
        /*tos=*/0, kPort3, kVlanIdNum1, kUdpSrcPortNum1, kUdpDstPortNum1);
    EXPECT_EQ(*mirror_entry, expected_mirror_entry);

    sai_object_id_t oid_in_mapper = 0;
    EXPECT_TRUE(p4_oid_mapper_.getOID(
        SAI_OBJECT_TYPE_MIRROR_SESSION,
        KeyGenerator::generateMirrorSessionKey(kMirrorSessionId),
        &oid_in_mapper));
    EXPECT_EQ(kMirrorSessionOid, oid_in_mapper);

    // 2. Update the added entry.
    fvs = {{p4orch::kAction, p4orch::kMirrorWithVlanTagAndIpfixEncapsulation},
           {prependParamField(p4orch::kMonitorPort), kPort3},
           {prependParamField(p4orch::kMonitorFailoverPort), kPort1},
           {prependParamField(p4orch::kMirrorEncapSrcIp), kSrcIpv6_2},
           {prependParamField(p4orch::kMirrorEncapDstIp), kDstIpv6_2},
           {prependParamField(p4orch::kMirrorEncapSrcMac), kSrcMac2},
           {prependParamField(p4orch::kMirrorEncapDstMac), kDstMac2},
           {prependParamField(p4orch::kMirrorEncapVlanId), kVlanId2},
           {prependParamField(p4orch::kMirrorEncapUdpSrcPort), kUdpSrcPort2},
           {prependParamField(p4orch::kMirrorEncapUdpDstPort), kUdpDstPort2}};

    app_db_entry = {std::string(APP_P4RT_MIRROR_SESSION_TABLE_NAME) +
                        kTableKeyDelimiter + j.dump(),
                    SET_COMMAND, fvs};

    Enqueue(app_db_entry);
    // Set up mock call.
    sai_attribute_t attr;
    attr.id = SAI_MIRROR_SESSION_ATTR_MONITOR_PORT;
    attr.value.oid = kPort3Oid;
    EXPECT_CALL(mock_sai_mirror_,
                set_mirror_session_attribute(
                    Eq(kMirrorSessionOid),
                    Truly(std::bind(MatchSaiCallAttrList, std::placeholders::_1,
                                    std::vector<sai_attribute_t>({attr})))))
        .WillOnce(Return(SAI_STATUS_SUCCESS));

    attr.id = SAI_MIRROR_SESSION_ATTR_SRC_IP_ADDRESS;
    swss::copy(attr.value.ipaddr, swss::IpAddress(kSrcIpv6_2));
    EXPECT_CALL(mock_sai_mirror_,
                set_mirror_session_attribute(
                    Eq(kMirrorSessionOid),
                    Truly(std::bind(MatchSaiCallAttrList, std::placeholders::_1,
                                    std::vector<sai_attribute_t>({attr})))))
        .WillOnce(Return(SAI_STATUS_SUCCESS));

    attr.id = SAI_MIRROR_SESSION_ATTR_DST_IP_ADDRESS;
    swss::copy(attr.value.ipaddr, swss::IpAddress(kDstIpv6_2));
    EXPECT_CALL(mock_sai_mirror_,
                set_mirror_session_attribute(
                    Eq(kMirrorSessionOid),
                    Truly(std::bind(MatchSaiCallAttrList, std::placeholders::_1,
                                    std::vector<sai_attribute_t>({attr})))))
        .WillOnce(Return(SAI_STATUS_SUCCESS));

    attr.id = SAI_MIRROR_SESSION_ATTR_SRC_MAC_ADDRESS;
    memcpy(attr.value.mac, swss::MacAddress(kSrcMac2).getMac(),
           sizeof(sai_mac_t));
    EXPECT_CALL(mock_sai_mirror_,
                set_mirror_session_attribute(
                    Eq(kMirrorSessionOid),
                    Truly(std::bind(MatchSaiCallAttrList, std::placeholders::_1,
                                    std::vector<sai_attribute_t>({attr})))))
        .WillOnce(Return(SAI_STATUS_SUCCESS));

    attr.id = SAI_MIRROR_SESSION_ATTR_DST_MAC_ADDRESS;
    memcpy(attr.value.mac, swss::MacAddress(kDstMac2).getMac(),
           sizeof(sai_mac_t));
    EXPECT_CALL(mock_sai_mirror_,
                set_mirror_session_attribute(
                    Eq(kMirrorSessionOid),
                    Truly(std::bind(MatchSaiCallAttrList, std::placeholders::_1,
                                    std::vector<sai_attribute_t>({attr})))))
        .WillOnce(Return(SAI_STATUS_SUCCESS));

    attr.id = SAI_MIRROR_SESSION_ATTR_VLAN_ID;
    attr.value.u16 = kVlanIdNum2;
    EXPECT_CALL(mock_sai_mirror_,
                set_mirror_session_attribute(
                    Eq(kMirrorSessionOid),
                    Truly(std::bind(MatchSaiCallAttrList, std::placeholders::_1,
                                    std::vector<sai_attribute_t>({attr})))))
        .WillOnce(Return(SAI_STATUS_SUCCESS));

    attr.id = SAI_MIRROR_SESSION_ATTR_UDP_SRC_PORT;
    attr.value.u16 = kUdpSrcPortNum2;
    EXPECT_CALL(mock_sai_mirror_,
                set_mirror_session_attribute(
                    Eq(kMirrorSessionOid),
                    Truly(std::bind(MatchSaiCallAttrList, std::placeholders::_1,
                                    std::vector<sai_attribute_t>({attr})))))
        .WillOnce(Return(SAI_STATUS_SUCCESS));

    attr.id = SAI_MIRROR_SESSION_ATTR_UDP_DST_PORT;
    attr.value.u16 = kUdpDstPortNum2;
    EXPECT_CALL(mock_sai_mirror_,
                set_mirror_session_attribute(
                    Eq(kMirrorSessionOid),
                    Truly(std::bind(MatchSaiCallAttrList, std::placeholders::_1,
                                    std::vector<sai_attribute_t>({attr})))))
        .WillOnce(Return(SAI_STATUS_SUCCESS));

    EXPECT_CALL(publisher_,
                publish(Eq(APP_P4RT_TABLE_NAME), Eq(kfvKey(app_db_entry)),
                        Eq(kfvFieldsValues(app_db_entry)),
                        Eq(StatusCode::SWSS_RC_SUCCESS), Eq(true)));
    EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS, Drain(/*failure_before=*/false));

    mirror_entry = GetMirrorSessionEntry(
        KeyGenerator::generateMirrorSessionKey(kMirrorSessionId));
    ASSERT_NE(mirror_entry, nullptr);
    expected_mirror_entry.port = kPort3;
    expected_mirror_entry.failover_port = kPort1;
    expected_mirror_entry.src_ip = swss::IpAddress(kSrcIpv6_2);
    expected_mirror_entry.dst_ip = swss::IpAddress(kDstIpv6_2);
    expected_mirror_entry.src_mac = swss::MacAddress(kSrcMac2);
    expected_mirror_entry.dst_mac = swss::MacAddress(kDstMac2);
    expected_mirror_entry.vlan_id = kVlanIdNum2;
    expected_mirror_entry.udp_src_port = kUdpSrcPortNum2;
    expected_mirror_entry.udp_dst_port = kUdpDstPortNum2;
    expected_mirror_entry.tos = kTos1Num;

    EXPECT_EQ(mirror_entry->mirror_session_key,
              expected_mirror_entry.mirror_session_key);
    EXPECT_EQ(mirror_entry->mirror_session_id,
              expected_mirror_entry.mirror_session_id);
    EXPECT_EQ(mirror_entry->action, expected_mirror_entry.action);
    EXPECT_EQ(mirror_entry->port, expected_mirror_entry.port);
    EXPECT_EQ(mirror_entry->src_ip, expected_mirror_entry.src_ip);
    EXPECT_EQ(mirror_entry->dst_ip, expected_mirror_entry.dst_ip);
    EXPECT_EQ(mirror_entry->src_mac, expected_mirror_entry.src_mac);
    EXPECT_EQ(mirror_entry->dst_mac, expected_mirror_entry.dst_mac);
    EXPECT_EQ(mirror_entry->ttl, expected_mirror_entry.ttl);
    EXPECT_EQ(mirror_entry->tos, expected_mirror_entry.tos);
    EXPECT_EQ(mirror_entry->vlan_id, expected_mirror_entry.vlan_id);
    EXPECT_EQ(mirror_entry->udp_src_port, expected_mirror_entry.udp_src_port);
    EXPECT_EQ(mirror_entry->udp_dst_port, expected_mirror_entry.udp_dst_port);

    // 3. Delete the entry.
    fvs = {};
    app_db_entry = {std::string(APP_P4RT_MIRROR_SESSION_TABLE_NAME) +
                        kTableKeyDelimiter + j.dump(),
                    DEL_COMMAND, fvs};

    Enqueue(app_db_entry);
    // Set up mock call.
    EXPECT_CALL(mock_sai_mirror_, remove_mirror_session(Eq(kMirrorSessionOid)))
        .WillOnce(Return(SAI_STATUS_SUCCESS));
    EXPECT_CALL(publisher_,
                publish(Eq(APP_P4RT_TABLE_NAME), Eq(kfvKey(app_db_entry)),
                        Eq(kfvFieldsValues(app_db_entry)),
                        Eq(StatusCode::SWSS_RC_SUCCESS), Eq(true)));
    EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS, Drain(/*failure_before=*/false));

    mirror_entry = GetMirrorSessionEntry(
        KeyGenerator::generateMirrorSessionKey(kMirrorSessionId));
    EXPECT_EQ(mirror_entry, nullptr);

    EXPECT_FALSE(p4_oid_mapper_.existsOID(
        SAI_OBJECT_TYPE_MIRROR_SESSION,
        KeyGenerator::generateMirrorSessionKey(kMirrorSessionId)));
}

TEST_F(MirrorSessionManagerTest,
       DrainShouldFailForInvalidAppDbEntryMatchFiled) {
  nlohmann::json j;
  j[prependMatchField("invalid_match_field")] = kMirrorSessionId;

  std::vector<swss::FieldValueTuple> fvs{
      {p4orch::kAction, p4orch::kMirrorAsIpv4Erspan},
      {prependParamField(p4orch::kPort), kPort1},
      {prependParamField(p4orch::kSrcIp), kSrcIp1},
      {prependParamField(p4orch::kDstIp), kDstIp1},
      {prependParamField(p4orch::kSrcMac), kSrcMac1},
      {prependParamField(p4orch::kDstMac), kDstMac1},
      {prependParamField(p4orch::kTtl), kTtl1},
      {prependParamField(p4orch::kTos), kTos1}};

  swss::KeyOpFieldsValuesTuple app_db_entry(
      std::string(APP_P4RT_MIRROR_SESSION_TABLE_NAME) + kTableKeyDelimiter +
          j.dump(),
      SET_COMMAND, fvs);

  Enqueue(app_db_entry);
  EXPECT_CALL(publisher_,
              publish(Eq(APP_P4RT_TABLE_NAME), Eq(kfvKey(app_db_entry)),
                      Eq(kfvFieldsValues(app_db_entry)),
                      Eq(StatusCode::SWSS_RC_INVALID_PARAM), Eq(true)));
  EXPECT_EQ(StatusCode::SWSS_RC_INVALID_PARAM, Drain(/*failure_before=*/false));

  // Check the added entry.
  auto mirror_entry = GetMirrorSessionEntry(
      KeyGenerator::generateMirrorSessionKey(kMirrorSessionId));
  EXPECT_EQ(mirror_entry, nullptr);
}

TEST_F(MirrorSessionManagerTest, DrainShouldFailForUnknownOp) {
  nlohmann::json j;
  j[prependMatchField(p4orch::kMirrorSessionId)] = kMirrorSessionId;

  std::vector<swss::FieldValueTuple> fvs{
      {p4orch::kAction, p4orch::kMirrorAsIpv4Erspan},
      {prependParamField(p4orch::kPort), kPort1},
      {prependParamField(p4orch::kSrcIp), kSrcIp1},
      {prependParamField(p4orch::kDstIp), kDstIp1},
      {prependParamField(p4orch::kSrcMac), kSrcMac1},
      {prependParamField(p4orch::kDstMac), kDstMac1},
      {prependParamField(p4orch::kTtl), kTtl1},
      {prependParamField(p4orch::kTos), kTos1}};

  swss::KeyOpFieldsValuesTuple app_db_entry(
      std::string(APP_P4RT_MIRROR_SESSION_TABLE_NAME) + kTableKeyDelimiter +
          j.dump(),
      "unknown_op", fvs);

  Enqueue(app_db_entry);
  EXPECT_CALL(publisher_,
              publish(Eq(APP_P4RT_TABLE_NAME), Eq(kfvKey(app_db_entry)),
                      Eq(kfvFieldsValues(app_db_entry)),
                      Eq(StatusCode::SWSS_RC_INVALID_PARAM), Eq(true)));
  EXPECT_EQ(StatusCode::SWSS_RC_INVALID_PARAM, Drain(/*failure_before=*/false));

  // Check the added entry.
  auto mirror_entry = GetMirrorSessionEntry(
      KeyGenerator::generateMirrorSessionKey(kMirrorSessionId));
  EXPECT_EQ(mirror_entry, nullptr);
}

TEST_F(MirrorSessionManagerTest,
       DrainShouldFailForInvalidAppDbEntryFieldValue) {
  nlohmann::json j;
  j[prependMatchField(p4orch::kMirrorSessionId)] = kMirrorSessionId;

  std::vector<swss::FieldValueTuple> fvs{
      {p4orch::kAction, p4orch::kMirrorAsIpv4Erspan},
      {prependParamField(p4orch::kPort), kPort1},
      {prependParamField(p4orch::kSrcIp), "0123456789"},
      {prependParamField(p4orch::kDstIp), kDstIp1},
      {prependParamField(p4orch::kSrcMac), kSrcMac1},
      {prependParamField(p4orch::kDstMac), kDstMac1},
      {prependParamField(p4orch::kTtl), kTtl1},
      {prependParamField(p4orch::kTos), kTos1}};

  swss::KeyOpFieldsValuesTuple app_db_entry(
      std::string(APP_P4RT_MIRROR_SESSION_TABLE_NAME) + kTableKeyDelimiter +
          j.dump(),
      SET_COMMAND, fvs);

  Enqueue(app_db_entry);
  EXPECT_CALL(publisher_,
              publish(Eq(APP_P4RT_TABLE_NAME), Eq(kfvKey(app_db_entry)),
                      Eq(kfvFieldsValues(app_db_entry)),
                      Eq(StatusCode::SWSS_RC_INVALID_PARAM), Eq(true)));
  EXPECT_EQ(StatusCode::SWSS_RC_INVALID_PARAM, Drain(/*failure_before=*/false));

  // Check the added entry.
  auto mirror_entry = GetMirrorSessionEntry(
      KeyGenerator::generateMirrorSessionKey(kMirrorSessionId));
  EXPECT_EQ(mirror_entry, nullptr);
}

TEST_F(MirrorSessionManagerTest,
       DrainShouldFailForUnknownAppDbEntryFieldValue) {
  nlohmann::json j;
  j[prependMatchField(p4orch::kMirrorSessionId)] = kMirrorSessionId;

  std::vector<swss::FieldValueTuple> fvs{
                                           {p4orch::kAction, p4orch::kMirrorAsIpv4Erspan},
                                           {prependParamField(p4orch::kPort), kPort1},
                                           {prependParamField(p4orch::kSrcIp), kSrcIp1},
                                           {prependParamField(p4orch::kDstIp), kDstIp1},
                                           {prependParamField(p4orch::kSrcMac), kSrcMac1},
                                           {prependParamField(p4orch::kDstMac), kDstMac1},
                                           {prependParamField(p4orch::kTtl), kTtl1},
                                           {prependParamField(p4orch::kTos), kTos1},
                                           {"unknown_field", "unknown_value"}};

    swss::KeyOpFieldsValuesTuple app_db_entry(
        std::string(APP_P4RT_MIRROR_SESSION_TABLE_NAME) + kTableKeyDelimiter + j.dump(), SET_COMMAND, fvs);

    Enqueue(app_db_entry);
    EXPECT_CALL(publisher_,
                publish(Eq(APP_P4RT_TABLE_NAME), Eq(kfvKey(app_db_entry)),
                        Eq(kfvFieldsValues(app_db_entry)),
                        Eq(StatusCode::SWSS_RC_INVALID_PARAM), Eq(true)));
    EXPECT_EQ(StatusCode::SWSS_RC_INVALID_PARAM,
              Drain(/*failure_before=*/false));

    // Check the added entry.
    auto mirror_entry = GetMirrorSessionEntry(KeyGenerator::generateMirrorSessionKey(kMirrorSessionId));
    EXPECT_EQ(mirror_entry, nullptr);
}

TEST_F(MirrorSessionManagerTest, DrainShouldFailForIncompleteAppDbEntry)
{
    nlohmann::json j;
    j[prependMatchField(p4orch::kMirrorSessionId)] = kMirrorSessionId;

    std::vector<swss::FieldValueTuple> fvs_missing_tos{
        {p4orch::kAction, p4orch::kMirrorAsIpv4Erspan}, {prependParamField(p4orch::kPort), kPort1},
        {prependParamField(p4orch::kSrcIp), kSrcIp1},   {prependParamField(p4orch::kDstIp), kDstIp1},
        {prependParamField(p4orch::kSrcMac), kSrcMac1}, {prependParamField(p4orch::kDstMac), kDstMac1},
        {prependParamField(p4orch::kTtl), kTtl1}};

    swss::KeyOpFieldsValuesTuple app_db_entry(
        std::string(APP_P4RT_MIRROR_SESSION_TABLE_NAME) + kTableKeyDelimiter + j.dump(), SET_COMMAND, fvs_missing_tos);

    Enqueue(app_db_entry);
    EXPECT_CALL(publisher_,
                publish(Eq(APP_P4RT_TABLE_NAME), Eq(kfvKey(app_db_entry)),
                        Eq(kfvFieldsValues(app_db_entry)),
                        Eq(StatusCode::SWSS_RC_INVALID_PARAM), Eq(true)));
    EXPECT_EQ(StatusCode::SWSS_RC_INVALID_PARAM,
              Drain(/*failure_before=*/false));

    // Check the added entry.
    auto mirror_entry = GetMirrorSessionEntry(KeyGenerator::generateMirrorSessionKey(kMirrorSessionId));
    EXPECT_EQ(mirror_entry, nullptr);
}

TEST_F(MirrorSessionManagerTest, DrainShouldFailForUnknownPort)
{
    nlohmann::json j;
    j[prependMatchField(p4orch::kMirrorSessionId)] = kMirrorSessionId;

    std::vector<swss::FieldValueTuple> fvs{
        {p4orch::kAction, p4orch::kMirrorAsIpv4Erspan}, {prependParamField(p4orch::kPort), "unknown_port"},
        {prependParamField(p4orch::kSrcIp), kSrcIp1},   {prependParamField(p4orch::kDstIp), kDstIp1},
        {prependParamField(p4orch::kSrcMac), kSrcMac1}, {prependParamField(p4orch::kDstMac), kDstMac1},
        {prependParamField(p4orch::kTtl), kTtl1},       {prependParamField(p4orch::kTos), kTos1}};

    swss::KeyOpFieldsValuesTuple app_db_entry(
        std::string(APP_P4RT_MIRROR_SESSION_TABLE_NAME) + kTableKeyDelimiter + j.dump(), SET_COMMAND, fvs);

    Enqueue(app_db_entry);
    EXPECT_CALL(publisher_,
                publish(Eq(APP_P4RT_TABLE_NAME), Eq(kfvKey(app_db_entry)),
                        Eq(kfvFieldsValues(app_db_entry)),
                        Eq(StatusCode::SWSS_RC_NOT_FOUND), Eq(true)));
    EXPECT_EQ(StatusCode::SWSS_RC_NOT_FOUND, Drain(/*failure_before=*/false));

    // Check the added entry.
    auto mirror_entry = GetMirrorSessionEntry(KeyGenerator::generateMirrorSessionKey(kMirrorSessionId));
    EXPECT_EQ(mirror_entry, nullptr);
}

TEST_F(MirrorSessionManagerTest, DrainShouldFailWhenCreateSaiCallFails)
{
    nlohmann::json j;
    j[prependMatchField(p4orch::kMirrorSessionId)] = kMirrorSessionId;

    std::vector<swss::FieldValueTuple> fvs{
        {p4orch::kAction, p4orch::kMirrorAsIpv4Erspan}, {prependParamField(p4orch::kPort), kPort1},
        {prependParamField(p4orch::kSrcIp), kSrcIp1},   {prependParamField(p4orch::kDstIp), kDstIp1},
        {prependParamField(p4orch::kSrcMac), kSrcMac1}, {prependParamField(p4orch::kDstMac), kDstMac1},
        {prependParamField(p4orch::kTtl), kTtl1},       {prependParamField(p4orch::kTos), kTos1}};

    swss::KeyOpFieldsValuesTuple app_db_entry(
        std::string(APP_P4RT_MIRROR_SESSION_TABLE_NAME) + kTableKeyDelimiter + j.dump(), SET_COMMAND, fvs);

    Enqueue(app_db_entry);
    EXPECT_CALL(mock_sai_mirror_,
                create_mirror_session(
                    ::testing::NotNull(), Eq(gSwitchId), Eq(11),
                    Truly(std::bind(MatchSaiCallAttrList, std::placeholders::_1,
                                    GenerateAttrListForCreate(kPort1Oid, kTtl1Num, kTos1Num, swss::IpAddress(kSrcIp1),
                                                              swss::IpAddress(kDstIp1), swss::MacAddress(kSrcMac1),
                                                              swss::MacAddress(kDstMac1))))))
        .WillOnce(Return(SAI_STATUS_FAILURE));
    EXPECT_CALL(publisher_,
                publish(Eq(APP_P4RT_TABLE_NAME), Eq(kfvKey(app_db_entry)),
                        Eq(kfvFieldsValues(app_db_entry)),
                        Eq(StatusCode::SWSS_RC_UNKNOWN), Eq(true)));
    EXPECT_EQ(StatusCode::SWSS_RC_UNKNOWN, Drain(/*failure_before=*/false));

    // Check the added entry.
    auto mirror_entry = GetMirrorSessionEntry(KeyGenerator::generateMirrorSessionKey(kMirrorSessionId));
    EXPECT_EQ(mirror_entry, nullptr);
}

TEST_F(MirrorSessionManagerTest, DrainShouldFailWhenDeleteSaiCallFails)
{
    nlohmann::json j;
    j[prependMatchField(p4orch::kMirrorSessionId)] = kMirrorSessionId;

    std::vector<swss::FieldValueTuple> fvs{
        {p4orch::kAction, p4orch::kMirrorAsIpv4Erspan}, {prependParamField(p4orch::kPort), kPort1},
        {prependParamField(p4orch::kSrcIp), kSrcIp1},   {prependParamField(p4orch::kDstIp), kDstIp1},
        {prependParamField(p4orch::kSrcMac), kSrcMac1}, {prependParamField(p4orch::kDstMac), kDstMac1},
        {prependParamField(p4orch::kTtl), kTtl1},       {prependParamField(p4orch::kTos), kTos1}};

    swss::KeyOpFieldsValuesTuple app_db_entry(
        std::string(APP_P4RT_MIRROR_SESSION_TABLE_NAME) + kTableKeyDelimiter + j.dump(), SET_COMMAND, fvs);

    Enqueue(app_db_entry);
    EXPECT_CALL(mock_sai_mirror_,
                create_mirror_session(
                    ::testing::NotNull(), Eq(gSwitchId), Eq(11),
                    Truly(std::bind(MatchSaiCallAttrList, std::placeholders::_1,
                                    GenerateAttrListForCreate(kPort1Oid, kTtl1Num, kTos1Num, swss::IpAddress(kSrcIp1),
                                                              swss::IpAddress(kDstIp1), swss::MacAddress(kSrcMac1),
                                                              swss::MacAddress(kDstMac1))))))
        .WillOnce(DoAll(SetArgPointee<0>(kMirrorSessionOid), Return(SAI_STATUS_SUCCESS)));
    EXPECT_CALL(publisher_,
                publish(Eq(APP_P4RT_TABLE_NAME), Eq(kfvKey(app_db_entry)),
                        Eq(kfvFieldsValues(app_db_entry)),
                        Eq(StatusCode::SWSS_RC_SUCCESS), Eq(true)));
    EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS, Drain(/*failure_before=*/false));

    fvs = {};
    app_db_entry = {std::string(APP_P4RT_MIRROR_SESSION_TABLE_NAME) + kTableKeyDelimiter + j.dump(), DEL_COMMAND, fvs};

    Enqueue(app_db_entry);
    EXPECT_CALL(mock_sai_mirror_, remove_mirror_session(Eq(kMirrorSessionOid))).WillOnce(Return(SAI_STATUS_FAILURE));
    EXPECT_CALL(publisher_,
                publish(Eq(APP_P4RT_TABLE_NAME), Eq(kfvKey(app_db_entry)),
                        Eq(kfvFieldsValues(app_db_entry)),
                        Eq(StatusCode::SWSS_RC_UNKNOWN), Eq(true)));
    EXPECT_EQ(StatusCode::SWSS_RC_UNKNOWN, Drain(/*failure_before=*/false));

    // Check entry still exists.
    auto mirror_entry = GetMirrorSessionEntry(KeyGenerator::generateMirrorSessionKey(kMirrorSessionId));
    ASSERT_NE(mirror_entry, nullptr);
}

TEST_F(MirrorSessionManagerTest, DeserializeMirrorAsIpv4ErspanActionSuccess) {
  const std::string key = R"({"match/mirror_session_id":"mirror_session1"})";
  std::vector<swss::FieldValueTuple> fvs{
      {p4orch::kAction, p4orch::kMirrorAsIpv4Erspan},
      {prependParamField(p4orch::kPort), kPort1},
      {prependParamField(p4orch::kSrcIp), kSrcIp1},
      {prependParamField(p4orch::kDstIp), kDstIp1},
      {prependParamField(p4orch::kSrcMac), kSrcMac1},
      {prependParamField(p4orch::kDstMac), kDstMac1},
      {prependParamField(p4orch::kTtl), kTtl1},
      {prependParamField(p4orch::kTos), kTos1}};

  auto mirror_entry_or = DeserializeP4MirrorSessionAppDbEntry(key, fvs);
  EXPECT_TRUE(mirror_entry_or.ok());

  P4MirrorSessionAppDbEntry expected_app_db_entry = {};
  expected_app_db_entry.mirror_session_id = kMirrorSessionId;
  ;
  expected_app_db_entry.action = p4orch::kMirrorAsIpv4Erspan;
  expected_app_db_entry.port = kPort1;
  expected_app_db_entry.src_ip = swss::IpAddress(kSrcIp1);
  expected_app_db_entry.dst_ip = swss::IpAddress(kDstIp1);
  expected_app_db_entry.src_mac = swss::MacAddress(kSrcMac1);
  expected_app_db_entry.dst_mac = swss::MacAddress(kDstMac1);
  expected_app_db_entry.ttl = kTtl1Num;
  expected_app_db_entry.tos = kTos1Num;

  VerifyP4MirrorSessionAppDbEntryEqual(*mirror_entry_or, expected_app_db_entry);
}

TEST_F(MirrorSessionManagerTest,
       DeserializeMirrorWithVlanTagAndIpfixEncapsulationSuccess) {
  const std::string key = R"({"match/mirror_session_id":"mirror_session1"})";
  std::vector<swss::FieldValueTuple> fvs{
      {p4orch::kAction, p4orch::kMirrorWithVlanTagAndIpfixEncapsulation},
      {prependParamField(p4orch::kMonitorPort), kPort1},
      {prependParamField(p4orch::kMonitorFailoverPort), kPort3},
      {prependParamField(p4orch::kMirrorEncapSrcIp), kSrcIpv6},
      {prependParamField(p4orch::kMirrorEncapDstIp), kDstIpv6},
      {prependParamField(p4orch::kMirrorEncapSrcMac), kSrcMac1},
      {prependParamField(p4orch::kMirrorEncapDstMac), kDstMac1},
      {prependParamField(p4orch::kMirrorEncapVlanId), kVlanId1},
      {prependParamField(p4orch::kMirrorEncapUdpSrcPort), kUdpSrcPort1},
      {prependParamField(p4orch::kMirrorEncapUdpDstPort), kUdpDstPort1}};

  auto mirror_entry_or = DeserializeP4MirrorSessionAppDbEntry(key, fvs);
  EXPECT_TRUE(mirror_entry_or.ok());

  P4MirrorSessionAppDbEntry expected_app_db_entry = {};
  expected_app_db_entry.mirror_session_id = kMirrorSessionId;
  ;
  expected_app_db_entry.action =
      p4orch::kMirrorWithVlanTagAndIpfixEncapsulation;
  expected_app_db_entry.port = kPort1;
  expected_app_db_entry.failover_port = kPort3;
  expected_app_db_entry.src_ip = swss::IpAddress(kSrcIpv6);
  expected_app_db_entry.dst_ip = swss::IpAddress(kDstIpv6);
  expected_app_db_entry.src_mac = swss::MacAddress(kSrcMac1);
  expected_app_db_entry.dst_mac = swss::MacAddress(kDstMac1);
  expected_app_db_entry.vlan_id = kVlanIdNum1;
  expected_app_db_entry.udp_src_port = kUdpSrcPortNum1;
  expected_app_db_entry.udp_dst_port = kUdpDstPortNum1;
  expected_app_db_entry.tos = kTos1Num;

  VerifyP4MirrorSessionAppDbEntryEqual(*mirror_entry_or, expected_app_db_entry);
}

TEST_F(MirrorSessionManagerTest, DeserializeInvalidFailoverPortFails) {
  const std::string key = R"({"match/mirror_session_id":"mirror_session1"})";
  std::vector<swss::FieldValueTuple> fvs{
      {p4orch::kAction, p4orch::kMirrorWithVlanTagAndIpfixEncapsulation},
      {prependParamField(p4orch::kMonitorPort), kPort1},
      {prependParamField(p4orch::kMonitorFailoverPort), kPort2},
      {prependParamField(p4orch::kMirrorEncapSrcIp), kSrcIpv6},
      {prependParamField(p4orch::kMirrorEncapDstIp), kDstIpv6},
      {prependParamField(p4orch::kMirrorEncapSrcMac), kSrcMac1},
      {prependParamField(p4orch::kMirrorEncapDstMac), kDstMac1},
      {prependParamField(p4orch::kMirrorEncapVlanId), kVlanId1},
      {prependParamField(p4orch::kMirrorEncapUdpSrcPort), kUdpSrcPort1},
      {prependParamField(p4orch::kMirrorEncapUdpDstPort), kUdpDstPort1}};

  auto mirror_entry_or = DeserializeP4MirrorSessionAppDbEntry(key, fvs);

  EXPECT_FALSE(mirror_entry_or.ok());
  EXPECT_EQ(StatusCode::SWSS_RC_INVALID_PARAM, mirror_entry_or.status());

  std::vector<swss::FieldValueTuple> fvs2{
      {p4orch::kAction, p4orch::kMirrorWithVlanTagAndIpfixEncapsulation},
      {prependParamField(p4orch::kMonitorPort), kPort1},
      {prependParamField(p4orch::kMonitorFailoverPort), "unknown"},
      {prependParamField(p4orch::kMirrorEncapSrcIp), kSrcIpv6},
      {prependParamField(p4orch::kMirrorEncapDstIp), kDstIpv6},
      {prependParamField(p4orch::kMirrorEncapSrcMac), kSrcMac1},
      {prependParamField(p4orch::kMirrorEncapDstMac), kDstMac1},
      {prependParamField(p4orch::kMirrorEncapVlanId), kVlanId1},
      {prependParamField(p4orch::kMirrorEncapUdpSrcPort), kUdpSrcPort1},
      {prependParamField(p4orch::kMirrorEncapUdpDstPort), kUdpDstPort1}};

  auto mirror_entry_2_or = DeserializeP4MirrorSessionAppDbEntry(key, fvs2);

  EXPECT_FALSE(mirror_entry_2_or.ok());
  EXPECT_EQ(StatusCode::SWSS_RC_NOT_FOUND, mirror_entry_2_or.status());
}

TEST_F(MirrorSessionManagerTest, DeserializeInvalidValueShouldFail)
{
    constexpr char *kInalidKey = R"({"invalid_key"})";
    std::vector<swss::FieldValueTuple> fvs{{p4orch::kAction, p4orch::kMirrorAsIpv4Erspan}};
    EXPECT_FALSE(DeserializeP4MirrorSessionAppDbEntry(kInalidKey, fvs).ok());

    constexpr char *kValidKey = R"({"match/mirror_session_id":"mirror_session1"})";

    std::vector<swss::FieldValueTuple> invalid_src_ip_value = {{prependParamField(p4orch::kSrcIp), "0123456789"}};
    EXPECT_FALSE(DeserializeP4MirrorSessionAppDbEntry(kInalidKey, invalid_src_ip_value).ok());

    std::vector<swss::FieldValueTuple> invalid_dst_ip_value = {{prependParamField(p4orch::kDstIp), "0123456789"}};
    EXPECT_FALSE(DeserializeP4MirrorSessionAppDbEntry(kValidKey, invalid_dst_ip_value).ok());

    std::vector<swss::FieldValueTuple> invalid_src_mac_value = {{prependParamField(p4orch::kSrcMac), "0123456789"}};
    EXPECT_FALSE(DeserializeP4MirrorSessionAppDbEntry(kValidKey, invalid_src_mac_value).ok());

    std::vector<swss::FieldValueTuple> invalid_dst_mac_value = {{prependParamField(p4orch::kDstMac), "0123456789"}};
    EXPECT_FALSE(DeserializeP4MirrorSessionAppDbEntry(kValidKey, invalid_dst_mac_value).ok());

    std::vector<swss::FieldValueTuple> invalid_ttl_value = {{prependParamField(p4orch::kTtl), "gpins"}};
    EXPECT_FALSE(DeserializeP4MirrorSessionAppDbEntry(kValidKey, invalid_ttl_value).ok());

    std::vector<swss::FieldValueTuple> invalid_tos_value = {{prependParamField(p4orch::kTos), "xyz"}};
    EXPECT_FALSE(DeserializeP4MirrorSessionAppDbEntry(kValidKey, invalid_tos_value).ok());

    std::vector<swss::FieldValueTuple> invalid_vlan_id_value = {
        {prependParamField(p4orch::kMirrorEncapVlanId), "xyz"}};
    EXPECT_FALSE(
        DeserializeP4MirrorSessionAppDbEntry(kValidKey, invalid_vlan_id_value)
            .ok());

    std::vector<swss::FieldValueTuple> invalid_udp_src_port_value = {
        {prependParamField(p4orch::kMirrorEncapUdpSrcPort), "xyz"}};
    EXPECT_FALSE(DeserializeP4MirrorSessionAppDbEntry(kValidKey,
                                                      invalid_udp_src_port_value)
                     .ok());

    std::vector<swss::FieldValueTuple> invalid_udp_dst_port_value = {
        {prependParamField(p4orch::kMirrorEncapUdpDstPort), "xyz"}};
    EXPECT_FALSE(DeserializeP4MirrorSessionAppDbEntry(kValidKey,
                                                      invalid_udp_dst_port_value)
                     .ok());

    std::vector<swss::FieldValueTuple> unsupported_port = {{prependParamField(p4orch::kPort), kPort2}};
    EXPECT_FALSE(DeserializeP4MirrorSessionAppDbEntry(kValidKey, unsupported_port).ok());

    std::vector<swss::FieldValueTuple> invalid_action_value = {{p4orch::kAction, "abc"}};
    EXPECT_FALSE(DeserializeP4MirrorSessionAppDbEntry(kValidKey, invalid_action_value).ok());
}

TEST_F(MirrorSessionManagerTest, ValidateMirrorSessionEntrySuccess) {
  auto app_db_entry = AddDefaultMirrorSession();
  EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS,
            ValidateMirrorSessionEntry(app_db_entry, SET_COMMAND));
  EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS,
            ValidateMirrorSessionEntry(app_db_entry, DEL_COMMAND));
}

TEST_F(MirrorSessionManagerTest, ValidateMirrorSessionEntryIpfixSuccess) {
  auto app_db_entry = AddDefaultIpfixMirrorEntry();
  EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS,
            ValidateMirrorSessionEntry(app_db_entry, SET_COMMAND));
  EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS,
            ValidateMirrorSessionEntry(app_db_entry, DEL_COMMAND));
}

TEST_F(MirrorSessionManagerTest, ValidateMirrorSessionEntryUnknownOperation) {
  auto app_db_entry = AddDefaultIpfixMirrorEntry();
  EXPECT_EQ(StatusCode::SWSS_RC_INVALID_PARAM,
            ValidateMirrorSessionEntry(app_db_entry, "unknown"));
}

TEST_F(MirrorSessionManagerTest, ValidateMirrorSessionEntryDeleteUnknownFails) {
  P4MirrorSessionAppDbEntry app_db_entry;
  app_db_entry.mirror_session_id = kMirrorSessionId;
  EXPECT_EQ(StatusCode::SWSS_RC_NOT_FOUND,
            ValidateMirrorSessionEntry(app_db_entry, DEL_COMMAND));
}

TEST_F(MirrorSessionManagerTest,
       ValidateMirrorSessionEntryDeleteNotInMapperRaisesCritical) {
  auto app_db_entry = AddDefaultIpfixMirrorEntry();
  p4_oid_mapper_.eraseOID(
      SAI_OBJECT_TYPE_MIRROR_SESSION,
      KeyGenerator::generateMirrorSessionKey(kMirrorSessionId));
  EXPECT_EQ(StatusCode::SWSS_RC_INTERNAL,
            ValidateMirrorSessionEntry(app_db_entry, DEL_COMMAND));
}

TEST_F(MirrorSessionManagerTest,
       ValidateMirrorSessionEntryDeleteWhenReferencedFails) {
  auto app_db_entry = AddDefaultIpfixMirrorEntry();
  p4_oid_mapper_.increaseRefCount(
      SAI_OBJECT_TYPE_MIRROR_SESSION,
      KeyGenerator::generateMirrorSessionKey(kMirrorSessionId));
  EXPECT_EQ(StatusCode::SWSS_RC_IN_USE,
            ValidateMirrorSessionEntry(app_db_entry, DEL_COMMAND));
}

TEST_F(MirrorSessionManagerTest, ValidateMirrorSessionEntryMissingIdFails) {
  auto app_db_entry = AddDefaultIpfixMirrorEntry();
  app_db_entry.mirror_session_id = "";
  EXPECT_EQ(StatusCode::SWSS_RC_INVALID_PARAM,
            ValidateMirrorSessionEntry(app_db_entry, SET_COMMAND));
}

TEST_F(MirrorSessionManagerTest,
       ValidateMirrorSessionEntryIpfixMissingActionParamsFails) {
  auto app_db_entry = AddDefaultIpfixMirrorEntry();

  app_db_entry.has_action = false;
  EXPECT_EQ(StatusCode::SWSS_RC_INVALID_PARAM,
            ValidateMirrorSessionEntry(app_db_entry, SET_COMMAND));
  app_db_entry.has_action = true;

  app_db_entry.has_port = false;
  EXPECT_EQ(StatusCode::SWSS_RC_INVALID_PARAM,
            ValidateMirrorSessionEntry(app_db_entry, SET_COMMAND));
  app_db_entry.has_port = true;

  app_db_entry.has_failover_port = false;
  EXPECT_EQ(StatusCode::SWSS_RC_INVALID_PARAM,
            ValidateMirrorSessionEntry(app_db_entry, SET_COMMAND));
  app_db_entry.has_failover_port = true;

  app_db_entry.has_src_ip = false;
  EXPECT_EQ(StatusCode::SWSS_RC_INVALID_PARAM,
            ValidateMirrorSessionEntry(app_db_entry, SET_COMMAND));
  app_db_entry.has_src_ip = true;

  auto saved_src_ip = app_db_entry.src_ip;
  app_db_entry.src_ip = swss::IpAddress(kSrcIp1);
  EXPECT_EQ(StatusCode::SWSS_RC_INVALID_PARAM,
            ValidateMirrorSessionEntry(app_db_entry, SET_COMMAND));
  app_db_entry.src_ip = saved_src_ip;

  app_db_entry.has_dst_ip = false;
  EXPECT_EQ(StatusCode::SWSS_RC_INVALID_PARAM,
            ValidateMirrorSessionEntry(app_db_entry, SET_COMMAND));
  app_db_entry.has_dst_ip = true;

  auto saved_dst_ip = app_db_entry.dst_ip;
  app_db_entry.dst_ip = swss::IpAddress(kSrcIp1);
  EXPECT_EQ(StatusCode::SWSS_RC_INVALID_PARAM,
            ValidateMirrorSessionEntry(app_db_entry, SET_COMMAND));
  app_db_entry.dst_ip = saved_dst_ip;

  app_db_entry.has_src_mac = false;
  EXPECT_EQ(StatusCode::SWSS_RC_INVALID_PARAM,
            ValidateMirrorSessionEntry(app_db_entry, SET_COMMAND));
  app_db_entry.has_src_mac = true;

  app_db_entry.has_dst_mac = false;
  EXPECT_EQ(StatusCode::SWSS_RC_INVALID_PARAM,
            ValidateMirrorSessionEntry(app_db_entry, SET_COMMAND));
  app_db_entry.has_dst_mac = true;

  app_db_entry.has_vlan_id = false;
  EXPECT_EQ(StatusCode::SWSS_RC_INVALID_PARAM,
            ValidateMirrorSessionEntry(app_db_entry, SET_COMMAND));
  app_db_entry.has_vlan_id = true;

  app_db_entry.has_udp_src_port = false;
  EXPECT_EQ(StatusCode::SWSS_RC_INVALID_PARAM,
            ValidateMirrorSessionEntry(app_db_entry, SET_COMMAND));
  app_db_entry.has_udp_src_port = true;

  app_db_entry.has_udp_dst_port = false;
  EXPECT_EQ(StatusCode::SWSS_RC_INVALID_PARAM,
            ValidateMirrorSessionEntry(app_db_entry, SET_COMMAND));
  app_db_entry.has_udp_dst_port = true;
}

TEST_F(MirrorSessionManagerTest,
       ValidateMirrorSessionEntryIpfixMissingInMapperRaisesCritical) {
  auto app_db_entry = AddDefaultIpfixMirrorEntry();
  p4_oid_mapper_.eraseOID(
      SAI_OBJECT_TYPE_MIRROR_SESSION,
      KeyGenerator::generateMirrorSessionKey(kMirrorSessionId));
    EXPECT_EQ(StatusCode::SWSS_RC_INTERNAL,
            ValidateMirrorSessionEntry(app_db_entry, SET_COMMAND));
}

TEST_F(MirrorSessionManagerTest,
       ValidateMirrorSessionEntryIpfixAlreadyInMapperRaisesCritical) {
  P4MirrorSessionAppDbEntry app_db_entry;
  app_db_entry.mirror_session_id = kMirrorSessionId;
  app_db_entry.has_action = true;
  app_db_entry.action = p4orch::kMirrorWithVlanTagAndIpfixEncapsulation;
  app_db_entry.has_port = true;
  app_db_entry.port = kPort1;
  app_db_entry.has_failover_port = true;
  app_db_entry.failover_port = kPort3;
  app_db_entry.has_src_ip = true;
  app_db_entry.src_ip = swss::IpAddress(kSrcIpv6);
  app_db_entry.has_dst_ip = true;
  app_db_entry.dst_ip = swss::IpAddress(kDstIpv6);
  app_db_entry.has_src_mac = true;
  app_db_entry.src_mac = swss::MacAddress(kSrcMac1);
  app_db_entry.has_dst_mac = true;
  app_db_entry.dst_mac = swss::MacAddress(kDstMac1);
  app_db_entry.has_vlan_id = true;
  app_db_entry.vlan_id = kVlanIdNum1;
  app_db_entry.has_udp_src_port = true;
  app_db_entry.udp_src_port = kUdpSrcPortNum1;
  app_db_entry.has_udp_dst_port = true;
  app_db_entry.udp_dst_port = kUdpDstPortNum1;
  app_db_entry.tos = kTos1Num;
  app_db_entry.has_tos = true;

  p4_oid_mapper_.setOID(
      SAI_OBJECT_TYPE_MIRROR_SESSION,
      KeyGenerator::generateMirrorSessionKey(kMirrorSessionId),
      kMirrorSessionOid);
    EXPECT_EQ(StatusCode::SWSS_RC_INTERNAL,
            ValidateMirrorSessionEntry(app_db_entry, SET_COMMAND));
}

TEST_F(MirrorSessionManagerTest,
       ValidateMirrorSessionEntryErspanMissingActionParamsFails) {
  auto app_db_entry = AddDefaultMirrorSession();

  app_db_entry.has_action = false;
  EXPECT_EQ(StatusCode::SWSS_RC_INVALID_PARAM,
            ValidateMirrorSessionEntry(app_db_entry, SET_COMMAND));
  app_db_entry.has_action = true;

  app_db_entry.has_port = false;
  EXPECT_EQ(StatusCode::SWSS_RC_INVALID_PARAM,
            ValidateMirrorSessionEntry(app_db_entry, SET_COMMAND));
  app_db_entry.has_port = true;

  app_db_entry.has_src_ip = false;
  EXPECT_EQ(StatusCode::SWSS_RC_INVALID_PARAM,
            ValidateMirrorSessionEntry(app_db_entry, SET_COMMAND));
  app_db_entry.has_src_ip = true;

  app_db_entry.has_src_ip = false;
  EXPECT_EQ(StatusCode::SWSS_RC_INVALID_PARAM,
            ValidateMirrorSessionEntry(app_db_entry, SET_COMMAND));
  app_db_entry.has_src_ip = true;

  app_db_entry.has_src_mac = false;
  EXPECT_EQ(StatusCode::SWSS_RC_INVALID_PARAM,
            ValidateMirrorSessionEntry(app_db_entry, SET_COMMAND));
  app_db_entry.has_src_mac = true;

  app_db_entry.has_dst_mac = false;
  EXPECT_EQ(StatusCode::SWSS_RC_INVALID_PARAM,
            ValidateMirrorSessionEntry(app_db_entry, SET_COMMAND));
  app_db_entry.has_dst_mac = true;

  app_db_entry.has_tos = false;
  EXPECT_EQ(StatusCode::SWSS_RC_INVALID_PARAM,
            ValidateMirrorSessionEntry(app_db_entry, SET_COMMAND));
  app_db_entry.has_tos = true;

  app_db_entry.has_ttl = false;
  EXPECT_EQ(StatusCode::SWSS_RC_INVALID_PARAM,
            ValidateMirrorSessionEntry(app_db_entry, SET_COMMAND));
  app_db_entry.has_ttl = true;
}

TEST_F(MirrorSessionManagerTest,
       ValidateMirrorSessionEntryIpfixActionChangeFails) {
  auto app_db_entry = AddDefaultIpfixMirrorEntry();
  app_db_entry.has_ttl = true;
  app_db_entry.ttl = 32;
  app_db_entry.has_tos = true;
  app_db_entry.tos = 0;
  app_db_entry.action = p4orch::kMirrorAsIpv4Erspan;
  EXPECT_EQ(StatusCode::SWSS_RC_UNIMPLEMENTED,
            ValidateMirrorSessionEntry(app_db_entry, SET_COMMAND));
}

TEST_F(MirrorSessionManagerTest, CreateMirrorSessionWithInvalidPortShouldFail) {
  // Non-existing port.
  p4orch::P4MirrorSessionEntry mirror_session_entry(
      KeyGenerator::generateMirrorSessionKey(kMirrorSessionId),
      kMirrorSessionOid, kMirrorSessionId, p4orch::kMirrorAsIpv4Erspan,
      "Non-existing Port", swss::IpAddress(kSrcIp1), swss::IpAddress(kDstIp1),
      swss::MacAddress(kSrcMac1), swss::MacAddress(kDstMac1), kTtl1Num,
      kTos1Num, kFailoverPort0, kVlanIdNum0, kUdpSrcPortNum0, kUdpDstPortNum0);

  EXPECT_FALSE(CreateMirrorSession(mirror_session_entry).ok());

  // Unsupported management port.
  mirror_session_entry.port = kPort2;
  EXPECT_FALSE(CreateMirrorSession(mirror_session_entry).ok());
}

TEST_F(MirrorSessionManagerTest, UpdatingPortFailureCases)
{
    p4orch::P4MirrorSessionEntry existing_mirror_session_entry(
        KeyGenerator::generateMirrorSessionKey(kMirrorSessionId),
        kMirrorSessionOid, kMirrorSessionId, p4orch::kMirrorAsIpv4Erspan, kPort1,
        swss::IpAddress(kSrcIp1), swss::IpAddress(kDstIp1),
        swss::MacAddress(kSrcMac1), swss::MacAddress(kDstMac1), kTtl1Num,
        kTos1Num, kFailoverPort0, kVlanIdNum0, kUdpSrcPortNum0, kUdpDstPortNum0);
    // Case 1: non-existing port.
    EXPECT_FALSE(SetPort("invalid_port", &existing_mirror_session_entry).ok());

    // Case 2: kPort2 is an unsupported management port.
    EXPECT_FALSE(SetPort(kPort2, &existing_mirror_session_entry).ok());

    // Case 3: SAI call failure.
    EXPECT_CALL(mock_sai_mirror_, set_mirror_session_attribute(_, _)).WillOnce(Return(SAI_STATUS_FAILURE));
    p4orch::P4MirrorSessionEntry existing_mirror_session_entry_before_update(existing_mirror_session_entry);
    EXPECT_FALSE(SetPort(kPort3, &existing_mirror_session_entry).ok());
    EXPECT_EQ(existing_mirror_session_entry, existing_mirror_session_entry_before_update);
}

TEST_F(MirrorSessionManagerTest, UpdatingSrcIpSaiFailure)
{
    p4orch::P4MirrorSessionEntry existing_mirror_session_entry(
        KeyGenerator::generateMirrorSessionKey(kMirrorSessionId),
        kMirrorSessionOid, kMirrorSessionId, p4orch::kMirrorAsIpv4Erspan, kPort1,
        swss::IpAddress(kSrcIp1), swss::IpAddress(kDstIp1),
        swss::MacAddress(kSrcMac1), swss::MacAddress(kDstMac1), kTtl1Num,
        kTos1Num, kFailoverPort0, kVlanIdNum0, kUdpSrcPortNum0, kUdpDstPortNum0);
    EXPECT_CALL(mock_sai_mirror_, set_mirror_session_attribute(_, _)).WillOnce(Return(SAI_STATUS_FAILURE));
    p4orch::P4MirrorSessionEntry existing_mirror_session_entry_before_update(existing_mirror_session_entry);
    EXPECT_TRUE(
        SetSrcIp(swss::IpAddress(kSrcIp1), &existing_mirror_session_entry).ok());
    EXPECT_FALSE(SetSrcIp(swss::IpAddress(kSrcIp2), &existing_mirror_session_entry).ok());
    EXPECT_EQ(existing_mirror_session_entry, existing_mirror_session_entry_before_update);
}

TEST_F(MirrorSessionManagerTest, UpdatingDstIpSaiFailure)
{
    p4orch::P4MirrorSessionEntry existing_mirror_session_entry(
        KeyGenerator::generateMirrorSessionKey(kMirrorSessionId),
        kMirrorSessionOid, kMirrorSessionId, p4orch::kMirrorAsIpv4Erspan, kPort1,
        swss::IpAddress(kSrcIp1), swss::IpAddress(kDstIp1),
        swss::MacAddress(kSrcMac1), swss::MacAddress(kDstMac1), kTtl1Num,
        kTos1Num, kFailoverPort0, kVlanIdNum0, kUdpSrcPortNum0, kUdpDstPortNum0);
    EXPECT_CALL(mock_sai_mirror_, set_mirror_session_attribute(_, _)).WillOnce(Return(SAI_STATUS_FAILURE));
    p4orch::P4MirrorSessionEntry existing_mirror_session_entry_before_update(existing_mirror_session_entry);
    EXPECT_TRUE(
        SetDstIp(swss::IpAddress(kDstIp1), &existing_mirror_session_entry).ok());
    EXPECT_FALSE(SetDstIp(swss::IpAddress(kDstIp2), &existing_mirror_session_entry).ok());
    EXPECT_EQ(existing_mirror_session_entry, existing_mirror_session_entry_before_update);
}

TEST_F(MirrorSessionManagerTest, UpdatingSrcMacSaiFailure)
{
    p4orch::P4MirrorSessionEntry existing_mirror_session_entry(
        KeyGenerator::generateMirrorSessionKey(kMirrorSessionId),
        kMirrorSessionOid, kMirrorSessionId, p4orch::kMirrorAsIpv4Erspan, kPort1,
        swss::IpAddress(kSrcIp1), swss::IpAddress(kDstIp1),
        swss::MacAddress(kSrcMac1), swss::MacAddress(kDstMac1), kTtl1Num,
        kTos1Num, kFailoverPort0, kVlanIdNum0, kUdpSrcPortNum0, kUdpDstPortNum0);
    EXPECT_CALL(mock_sai_mirror_, set_mirror_session_attribute(_, _)).WillOnce(Return(SAI_STATUS_FAILURE));
    p4orch::P4MirrorSessionEntry existing_mirror_session_entry_before_update(existing_mirror_session_entry);
    EXPECT_TRUE(
        SetSrcMac(swss::MacAddress(kSrcMac1), &existing_mirror_session_entry)
            .ok());
    EXPECT_FALSE(SetSrcMac(swss::MacAddress(kSrcMac2), &existing_mirror_session_entry).ok());
    EXPECT_EQ(existing_mirror_session_entry, existing_mirror_session_entry_before_update);
}

TEST_F(MirrorSessionManagerTest, UpdatingDstMacSaiFailure)
{
    p4orch::P4MirrorSessionEntry existing_mirror_session_entry(
        KeyGenerator::generateMirrorSessionKey(kMirrorSessionId),
        kMirrorSessionOid, kMirrorSessionId, p4orch::kMirrorAsIpv4Erspan, kPort1,
        swss::IpAddress(kSrcIp1), swss::IpAddress(kDstIp1),
        swss::MacAddress(kSrcMac1), swss::MacAddress(kDstMac1), kTtl1Num,
        kTos1Num, kFailoverPort0, kVlanIdNum0, kUdpSrcPortNum0, kUdpDstPortNum0);
    EXPECT_CALL(mock_sai_mirror_, set_mirror_session_attribute(_, _)).WillOnce(Return(SAI_STATUS_FAILURE));
    p4orch::P4MirrorSessionEntry existing_mirror_session_entry_before_update(existing_mirror_session_entry);
    EXPECT_TRUE(
        SetDstMac(swss::MacAddress(kDstMac1), &existing_mirror_session_entry)
            .ok());
    EXPECT_FALSE(SetDstMac(swss::MacAddress(kDstMac2), &existing_mirror_session_entry).ok());
    EXPECT_EQ(existing_mirror_session_entry, existing_mirror_session_entry_before_update);
}

TEST_F(MirrorSessionManagerTest, UpdatingTtlSaiFailure)
{
    p4orch::P4MirrorSessionEntry existing_mirror_session_entry(
        KeyGenerator::generateMirrorSessionKey(kMirrorSessionId),
        kMirrorSessionOid, kMirrorSessionId, p4orch::kMirrorAsIpv4Erspan, kPort1,
        swss::IpAddress(kSrcIp1), swss::IpAddress(kDstIp1),
        swss::MacAddress(kSrcMac1), swss::MacAddress(kDstMac1), kTtl1Num,
        kTos1Num, kFailoverPort0, kVlanIdNum0, kUdpSrcPortNum0, kUdpDstPortNum0);
    EXPECT_CALL(mock_sai_mirror_, set_mirror_session_attribute(_, _)).WillOnce(Return(SAI_STATUS_FAILURE));
    p4orch::P4MirrorSessionEntry existing_mirror_session_entry_before_update(existing_mirror_session_entry);
    EXPECT_TRUE(SetTtl(kTtl1Num, &existing_mirror_session_entry).ok());
    EXPECT_FALSE(SetTtl(kTtl2Num, &existing_mirror_session_entry).ok());
    EXPECT_EQ(existing_mirror_session_entry, existing_mirror_session_entry_before_update);
}

TEST_F(MirrorSessionManagerTest, UpdatingTosSaiFailure)
{
    p4orch::P4MirrorSessionEntry existing_mirror_session_entry(
        KeyGenerator::generateMirrorSessionKey(kMirrorSessionId),
        kMirrorSessionOid, kMirrorSessionId, p4orch::kMirrorAsIpv4Erspan, kPort1,
        swss::IpAddress(kSrcIp1), swss::IpAddress(kDstIp1),
        swss::MacAddress(kSrcMac1), swss::MacAddress(kDstMac1), kTtl1Num,
        kTos1Num, kFailoverPort0, kVlanIdNum0, kUdpSrcPortNum0, kUdpDstPortNum0);
    EXPECT_CALL(mock_sai_mirror_, set_mirror_session_attribute(_, _)).WillOnce(Return(SAI_STATUS_FAILURE));
    p4orch::P4MirrorSessionEntry existing_mirror_session_entry_before_update(existing_mirror_session_entry);
    EXPECT_TRUE(SetTos(kTos1Num, &existing_mirror_session_entry).ok());
    EXPECT_FALSE(SetTos(kTos2Num, &existing_mirror_session_entry).ok());
    EXPECT_EQ(existing_mirror_session_entry, existing_mirror_session_entry_before_update);
}

TEST_F(MirrorSessionManagerTest, UpdatingVlanSaiFailure) {
  p4orch::P4MirrorSessionEntry existing_mirror_session_entry(
      KeyGenerator::generateMirrorSessionKey(kMirrorSessionId),
      kMirrorSessionOid, kMirrorSessionId,
      p4orch::kMirrorWithVlanTagAndIpfixEncapsulation, kPort1,
      swss::IpAddress(kSrcIpv6), swss::IpAddress(kDstIpv6),
      swss::MacAddress(kSrcMac1), swss::MacAddress(kDstMac1), /*ttl=*/0,
      /*tos=*/0, kPort3, kVlanIdNum1, kUdpSrcPortNum1, kUdpDstPortNum1);
  EXPECT_CALL(mock_sai_mirror_, set_mirror_session_attribute(_, _))
      .WillOnce(Return(SAI_STATUS_FAILURE));
  p4orch::P4MirrorSessionEntry existing_mirror_session_entry_before_update(
      existing_mirror_session_entry);
  EXPECT_TRUE(SetVlanId(kVlanIdNum1, &existing_mirror_session_entry).ok());
  EXPECT_FALSE(SetVlanId(kVlanIdNum2, &existing_mirror_session_entry).ok());
  EXPECT_EQ(existing_mirror_session_entry,
            existing_mirror_session_entry_before_update);
}

TEST_F(MirrorSessionManagerTest, UpdatingUdpSrcPortSaiFailure) {
  p4orch::P4MirrorSessionEntry existing_mirror_session_entry(
      KeyGenerator::generateMirrorSessionKey(kMirrorSessionId),
      kMirrorSessionOid, kMirrorSessionId,
      p4orch::kMirrorWithVlanTagAndIpfixEncapsulation, kPort1,
      swss::IpAddress(kSrcIpv6), swss::IpAddress(kDstIpv6),
      swss::MacAddress(kSrcMac1), swss::MacAddress(kDstMac1), /*ttl=*/0,
      /*tos=*/0, kPort3, kVlanIdNum1, kUdpSrcPortNum1, kUdpDstPortNum1);
  EXPECT_CALL(mock_sai_mirror_, set_mirror_session_attribute(_, _))
      .WillOnce(Return(SAI_STATUS_FAILURE));
  p4orch::P4MirrorSessionEntry existing_mirror_session_entry_before_update(
      existing_mirror_session_entry);
  EXPECT_TRUE(
      SetUdpSrcPort(kUdpSrcPortNum1, &existing_mirror_session_entry).ok());
  EXPECT_FALSE(
      SetUdpSrcPort(kUdpSrcPortNum2, &existing_mirror_session_entry).ok());
  EXPECT_EQ(existing_mirror_session_entry,
            existing_mirror_session_entry_before_update);
}

TEST_F(MirrorSessionManagerTest, UpdatingUdpDstPortSaiFailure) {
  p4orch::P4MirrorSessionEntry existing_mirror_session_entry(
      KeyGenerator::generateMirrorSessionKey(kMirrorSessionId),
      kMirrorSessionOid, kMirrorSessionId,
      p4orch::kMirrorWithVlanTagAndIpfixEncapsulation, kPort1,
      swss::IpAddress(kSrcIpv6), swss::IpAddress(kDstIpv6),
      swss::MacAddress(kSrcMac1), swss::MacAddress(kDstMac1), /*ttl=*/0,
      /*tos=*/0, kPort3, kVlanIdNum1, kUdpSrcPortNum1, kUdpDstPortNum1);
  EXPECT_CALL(mock_sai_mirror_, set_mirror_session_attribute(_, _))
      .WillOnce(Return(SAI_STATUS_FAILURE));
  p4orch::P4MirrorSessionEntry existing_mirror_session_entry_before_update(
      existing_mirror_session_entry);
  EXPECT_TRUE(
      SetUdpDstPort(kUdpDstPortNum1, &existing_mirror_session_entry).ok());
  EXPECT_FALSE(
      SetUdpDstPort(kUdpDstPortNum2, &existing_mirror_session_entry).ok());
  EXPECT_EQ(existing_mirror_session_entry,
            existing_mirror_session_entry_before_update);
}

// The update operation should be atomic -- it either succeeds or fails without
// changing anything. This test case verifies that failed update operation
// doesn't change existing entry.
TEST_F(MirrorSessionManagerTest, UpdateFailureShouldNotChangeExistingEntry)
{
    // 1. Add a new entry.
    nlohmann::json j;
    j[prependMatchField(p4orch::kMirrorSessionId)] = kMirrorSessionId;

    std::vector<swss::FieldValueTuple> fvs{
        {p4orch::kAction, p4orch::kMirrorAsIpv4Erspan}, {prependParamField(p4orch::kPort), kPort1},
        {prependParamField(p4orch::kSrcIp), kSrcIp1},   {prependParamField(p4orch::kDstIp), kDstIp1},
        {prependParamField(p4orch::kSrcMac), kSrcMac1}, {prependParamField(p4orch::kDstMac), kDstMac1},
        {prependParamField(p4orch::kTtl), kTtl1},       {prependParamField(p4orch::kTos), kTos1}};

    swss::KeyOpFieldsValuesTuple app_db_entry(
        std::string(APP_P4RT_MIRROR_SESSION_TABLE_NAME) + kTableKeyDelimiter + j.dump(), SET_COMMAND, fvs);

    Enqueue(app_db_entry);
    // Set up mock call.
    EXPECT_CALL(mock_sai_mirror_, create_mirror_session(_, _, _, _))
        .WillOnce(DoAll(SetArgPointee<0>(kMirrorSessionOid), Return(SAI_STATUS_SUCCESS)));
    EXPECT_CALL(publisher_,
                publish(Eq(APP_P4RT_TABLE_NAME), Eq(kfvKey(app_db_entry)),
                        Eq(kfvFieldsValues(app_db_entry)),
                        Eq(StatusCode::SWSS_RC_SUCCESS), Eq(true)));
    EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS, Drain(/*failure_before=*/false));

    // Check the added entry.
    auto mirror_entry = GetMirrorSessionEntry(KeyGenerator::generateMirrorSessionKey(kMirrorSessionId));
    ASSERT_NE(mirror_entry, nullptr);
    p4orch::P4MirrorSessionEntry expected_mirror_entry(
        KeyGenerator::generateMirrorSessionKey(kMirrorSessionId),
        kMirrorSessionOid, kMirrorSessionId, p4orch::kMirrorAsIpv4Erspan, kPort1,
        swss::IpAddress(kSrcIp1), swss::IpAddress(kDstIp1),
        swss::MacAddress(kSrcMac1), swss::MacAddress(kDstMac1), kTtl1Num,
        kTos1Num, kFailoverPort0, kVlanIdNum0, kUdpSrcPortNum0, kUdpDstPortNum0);
    EXPECT_EQ(*mirror_entry, expected_mirror_entry);

    sai_object_id_t oid_in_mapper = 0;
    EXPECT_TRUE(p4_oid_mapper_.getOID(SAI_OBJECT_TYPE_MIRROR_SESSION,
                                      KeyGenerator::generateMirrorSessionKey(kMirrorSessionId), &oid_in_mapper));
    EXPECT_EQ(kMirrorSessionOid, oid_in_mapper);

    // 2. Update the added entry.
    fvs = {{p4orch::kAction, p4orch::kMirrorAsIpv4Erspan}, {prependParamField(p4orch::kPort), kPort3},
           {prependParamField(p4orch::kSrcIp), kSrcIp2},   {prependParamField(p4orch::kDstIp), kDstIp2},
           {prependParamField(p4orch::kSrcMac), kSrcMac2}, {prependParamField(p4orch::kDstMac), kDstMac2},
           {prependParamField(p4orch::kTtl), kTtl2},       {prependParamField(p4orch::kTos), kTos2}};

    app_db_entry = {std::string(APP_P4RT_MIRROR_SESSION_TABLE_NAME) + kTableKeyDelimiter + j.dump(), SET_COMMAND, fvs};

    Enqueue(app_db_entry);
    // Set up mock calls. Update entry will trigger 7 attribute updates and each
    // attribute update requires a seperate SAI call. Let's pass the first 6 SAI
    // calls and fail the last one. When update fails in the middle, 6 successful
    // attribute updates will be reverted one by one. So the set SAI call wil be
    // called 13 times and actions are 6 successes, 1 failure, 6successes.
    EXPECT_CALL(mock_sai_mirror_, set_mirror_session_attribute(_, _))
        .Times(13)
        .WillOnce(Return(SAI_STATUS_SUCCESS))
        .WillOnce(Return(SAI_STATUS_SUCCESS))
        .WillOnce(Return(SAI_STATUS_SUCCESS))
        .WillOnce(Return(SAI_STATUS_SUCCESS))
        .WillOnce(Return(SAI_STATUS_SUCCESS))
        .WillOnce(Return(SAI_STATUS_SUCCESS))
        .WillOnce(Return(SAI_STATUS_FAILURE))
        .WillRepeatedly(Return(SAI_STATUS_SUCCESS));

    EXPECT_CALL(publisher_,
                publish(Eq(APP_P4RT_TABLE_NAME), Eq(kfvKey(app_db_entry)),
                        Eq(kfvFieldsValues(app_db_entry)),
                        Eq(StatusCode::SWSS_RC_UNKNOWN), Eq(true)));
    EXPECT_EQ(StatusCode::SWSS_RC_UNKNOWN, Drain(/*failure_before=*/false));

    mirror_entry = GetMirrorSessionEntry(KeyGenerator::generateMirrorSessionKey(kMirrorSessionId));
    ASSERT_NE(mirror_entry, nullptr);
    EXPECT_EQ(*mirror_entry, expected_mirror_entry);
}

TEST_F(MirrorSessionManagerTest, UpdateRecoveryFailureShouldRaiseCriticalState)
{
    // 1. Add a new entry.
    nlohmann::json j;
    j[prependMatchField(p4orch::kMirrorSessionId)] = kMirrorSessionId;

    std::vector<swss::FieldValueTuple> fvs{
        {p4orch::kAction, p4orch::kMirrorAsIpv4Erspan}, {prependParamField(p4orch::kPort), kPort1},
        {prependParamField(p4orch::kSrcIp), kSrcIp1},   {prependParamField(p4orch::kDstIp), kDstIp1},
        {prependParamField(p4orch::kSrcMac), kSrcMac1}, {prependParamField(p4orch::kDstMac), kDstMac1},
        {prependParamField(p4orch::kTtl), kTtl1},       {prependParamField(p4orch::kTos), kTos1}};

    swss::KeyOpFieldsValuesTuple app_db_entry(
        std::string(APP_P4RT_MIRROR_SESSION_TABLE_NAME) + kTableKeyDelimiter + j.dump(), SET_COMMAND, fvs);

    Enqueue(app_db_entry);
    // Set up mock call.
    EXPECT_CALL(mock_sai_mirror_, create_mirror_session(_, _, _, _))
        .WillOnce(DoAll(SetArgPointee<0>(kMirrorSessionOid), Return(SAI_STATUS_SUCCESS)));
    EXPECT_CALL(publisher_,
                publish(Eq(APP_P4RT_TABLE_NAME), Eq(kfvKey(app_db_entry)),
                        Eq(kfvFieldsValues(app_db_entry)),
                        Eq(StatusCode::SWSS_RC_SUCCESS), Eq(true)));
    EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS, Drain(/*failure_before=*/false));

    // Check the added entry.
    auto mirror_entry = GetMirrorSessionEntry(KeyGenerator::generateMirrorSessionKey(kMirrorSessionId));
    ASSERT_NE(mirror_entry, nullptr);
    p4orch::P4MirrorSessionEntry expected_mirror_entry(
      KeyGenerator::generateMirrorSessionKey(kMirrorSessionId),
      kMirrorSessionOid, kMirrorSessionId, p4orch::kMirrorAsIpv4Erspan, kPort1,
      swss::IpAddress(kSrcIp1), swss::IpAddress(kDstIp1),
      swss::MacAddress(kSrcMac1), swss::MacAddress(kDstMac1), kTtl1Num,
      kTos1Num, kFailoverPort0, kVlanIdNum0, kUdpSrcPortNum0, kUdpDstPortNum0);
  EXPECT_EQ(*mirror_entry, expected_mirror_entry);

  sai_object_id_t oid_in_mapper = 0;
  EXPECT_TRUE(p4_oid_mapper_.getOID(
      SAI_OBJECT_TYPE_MIRROR_SESSION,
      KeyGenerator::generateMirrorSessionKey(kMirrorSessionId),
      &oid_in_mapper));
  EXPECT_EQ(kMirrorSessionOid, oid_in_mapper);

  // 2. Update the added entry.
  fvs = {{p4orch::kAction, p4orch::kMirrorAsIpv4Erspan},
         {prependParamField(p4orch::kPort), kPort3},
         {prependParamField(p4orch::kSrcIp), kSrcIp2},
         {prependParamField(p4orch::kDstIp), kDstIp2},
         {prependParamField(p4orch::kSrcMac), kSrcMac2},
         {prependParamField(p4orch::kDstMac), kDstMac2},
         {prependParamField(p4orch::kTtl), kTtl2},
         {prependParamField(p4orch::kTos), kTos2}};

  app_db_entry = {std::string(APP_P4RT_MIRROR_SESSION_TABLE_NAME) +
                      kTableKeyDelimiter + j.dump(),
                  SET_COMMAND, fvs};

  Enqueue(app_db_entry);
  // Set up mock calls. Update entry will trigger 7 attribute updates and each
  // attribute update requires a seperate SAI call. Let's pass the first 6 SAI
  // calls and fail the last one. When update fails in the middle, 6 successful
  // attribute updates will be reverted one by one. We will fail the recovery by
  // failing the last revert.
  EXPECT_CALL(mock_sai_mirror_, set_mirror_session_attribute(_, _))
      .Times(13)
      .WillOnce(Return(SAI_STATUS_SUCCESS))
      .WillOnce(Return(SAI_STATUS_SUCCESS))
      .WillOnce(Return(SAI_STATUS_SUCCESS))
      .WillOnce(Return(SAI_STATUS_SUCCESS))
      .WillOnce(Return(SAI_STATUS_SUCCESS))
      .WillOnce(Return(SAI_STATUS_SUCCESS))
      .WillOnce(Return(SAI_STATUS_FAILURE))
      .WillOnce(Return(SAI_STATUS_SUCCESS))
      .WillOnce(Return(SAI_STATUS_SUCCESS))
      .WillOnce(Return(SAI_STATUS_SUCCESS))
      .WillOnce(Return(SAI_STATUS_SUCCESS))
      .WillOnce(Return(SAI_STATUS_SUCCESS))
      .WillOnce(Return(SAI_STATUS_FAILURE));

  EXPECT_CALL(publisher_,
              publish(Eq(APP_P4RT_TABLE_NAME), Eq(kfvKey(app_db_entry)),
                      Eq(kfvFieldsValues(app_db_entry)),
                      Eq(StatusCode::SWSS_RC_UNKNOWN), Eq(true)));
  EXPECT_EQ(StatusCode::SWSS_RC_UNKNOWN, Drain(/*failure_before=*/false));

  mirror_entry = GetMirrorSessionEntry(
      KeyGenerator::generateMirrorSessionKey(kMirrorSessionId));
  ASSERT_NE(mirror_entry, nullptr);
}

TEST_F(MirrorSessionManagerTest,
       UpdateRecoveryFailureIpFixShouldRaiseCriticalState) {
  // 1. Add a new entry.
  nlohmann::json j;
  j[prependMatchField(p4orch::kMirrorSessionId)] = kMirrorSessionId;

  std::vector<swss::FieldValueTuple> fvs{
      {p4orch::kAction, p4orch::kMirrorWithVlanTagAndIpfixEncapsulation},
      {prependParamField(p4orch::kMonitorPort), kPort1},
      {prependParamField(p4orch::kMonitorFailoverPort), kPort3},
      {prependParamField(p4orch::kMirrorEncapSrcIp), kSrcIpv6},
      {prependParamField(p4orch::kMirrorEncapDstIp), kDstIpv6},
      {prependParamField(p4orch::kMirrorEncapSrcMac), kSrcMac1},
      {prependParamField(p4orch::kMirrorEncapDstMac), kDstMac1},
      {prependParamField(p4orch::kMirrorEncapVlanId), kVlanId1},
      {prependParamField(p4orch::kMirrorEncapUdpSrcPort), kUdpSrcPort1},
      {prependParamField(p4orch::kMirrorEncapUdpDstPort), kUdpDstPort1}};

  swss::KeyOpFieldsValuesTuple app_db_entry(
      std::string(APP_P4RT_MIRROR_SESSION_TABLE_NAME) + kTableKeyDelimiter +
          j.dump(),
      SET_COMMAND, fvs);

  Enqueue(app_db_entry);
  // Set up mock call.
  EXPECT_CALL(mock_sai_mirror_, create_mirror_session(_, _, _, _))
      .WillOnce(DoAll(SetArgPointee<0>(kMirrorSessionOid),
                      Return(SAI_STATUS_SUCCESS)));
  EXPECT_CALL(publisher_,
              publish(Eq(APP_P4RT_TABLE_NAME), Eq(kfvKey(app_db_entry)),
                      Eq(kfvFieldsValues(app_db_entry)),
                      Eq(StatusCode::SWSS_RC_SUCCESS), Eq(true)));
  EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS, Drain(/*failure_before=*/false));

  // Check the added entry.
  auto mirror_entry = GetMirrorSessionEntry(
      KeyGenerator::generateMirrorSessionKey(kMirrorSessionId));
  ASSERT_NE(mirror_entry, nullptr);

    sai_object_id_t oid_in_mapper = 0;
    EXPECT_TRUE(p4_oid_mapper_.getOID(SAI_OBJECT_TYPE_MIRROR_SESSION,
                                      KeyGenerator::generateMirrorSessionKey(kMirrorSessionId), &oid_in_mapper));
    EXPECT_EQ(kMirrorSessionOid, oid_in_mapper);

    // 2. Update the added entry.
    fvs = {{p4orch::kAction, p4orch::kMirrorWithVlanTagAndIpfixEncapsulation},
         {prependParamField(p4orch::kMonitorPort), kPort3},
         {prependParamField(p4orch::kMonitorFailoverPort), kPort1},
         {prependParamField(p4orch::kMirrorEncapSrcIp), kSrcIpv6_2},
         {prependParamField(p4orch::kMirrorEncapDstIp), kDstIpv6_2},
         {prependParamField(p4orch::kMirrorEncapSrcMac), kSrcMac2},
         {prependParamField(p4orch::kMirrorEncapDstMac), kDstMac2},
         {prependParamField(p4orch::kMirrorEncapVlanId), kVlanId2},
         {prependParamField(p4orch::kMirrorEncapUdpSrcPort), kUdpSrcPort2},
         {prependParamField(p4orch::kMirrorEncapUdpDstPort), kUdpDstPort2}};

    app_db_entry = {std::string(APP_P4RT_MIRROR_SESSION_TABLE_NAME) + kTableKeyDelimiter + j.dump(), SET_COMMAND, fvs};

    Enqueue(app_db_entry);
    // Set up mock calls. Update entry will trigger 8 attribute updates and each
    // attribute update requires a seperate SAI call. Let's pass the first 7 SAI
    // calls and fail the last one. When update fails in the middle, 7 successful
    // attribute updates will be reverted one by one. We will fail the recovery by
    // failing the last revert.
    EXPECT_CALL(mock_sai_mirror_, set_mirror_session_attribute(_, _))
        .Times(15)
        .WillOnce(Return(SAI_STATUS_SUCCESS))
        .WillOnce(Return(SAI_STATUS_SUCCESS))
        .WillOnce(Return(SAI_STATUS_SUCCESS))
        .WillOnce(Return(SAI_STATUS_SUCCESS))
        .WillOnce(Return(SAI_STATUS_SUCCESS))
        .WillOnce(Return(SAI_STATUS_SUCCESS))
        .WillOnce(Return(SAI_STATUS_SUCCESS))
        .WillOnce(Return(SAI_STATUS_FAILURE))
        // Now reverts.
        .WillOnce(Return(SAI_STATUS_SUCCESS))
        .WillOnce(Return(SAI_STATUS_SUCCESS))
        .WillOnce(Return(SAI_STATUS_SUCCESS))
        .WillOnce(Return(SAI_STATUS_SUCCESS))
        .WillOnce(Return(SAI_STATUS_SUCCESS))
        .WillOnce(Return(SAI_STATUS_SUCCESS))
        .WillOnce(Return(SAI_STATUS_FAILURE));

    EXPECT_CALL(publisher_,
                publish(Eq(APP_P4RT_TABLE_NAME), Eq(kfvKey(app_db_entry)),
                        Eq(kfvFieldsValues(app_db_entry)),
                        Eq(StatusCode::SWSS_RC_UNKNOWN), Eq(true)));
    EXPECT_EQ(StatusCode::SWSS_RC_UNKNOWN, Drain(/*failure_before=*/false));

    mirror_entry = GetMirrorSessionEntry(KeyGenerator::generateMirrorSessionKey(kMirrorSessionId));
    ASSERT_NE(mirror_entry, nullptr);
}

TEST_F(MirrorSessionManagerTest, DeleteNonExistingMirrorSessionShouldFail)
{
    ASSERT_EQ(StatusCode::SWSS_RC_NOT_FOUND,
              ProcessDeleteRequest(KeyGenerator::generateMirrorSessionKey(kMirrorSessionId)));
}

TEST_F(MirrorSessionManagerTest, AddEntryWithIpfixActionSuccess) {
  AddDefaultIpfixMirrorEntry();

  // Check the added entry.
  auto mirror_entry = GetMirrorSessionEntry(
      KeyGenerator::generateMirrorSessionKey(kMirrorSessionId));
  ASSERT_NE(mirror_entry, nullptr);
  p4orch::P4MirrorSessionEntry expected_mirror_entry(
      KeyGenerator::generateMirrorSessionKey(kMirrorSessionId),
      kMirrorSessionOid, kMirrorSessionId,
      p4orch::kMirrorWithVlanTagAndIpfixEncapsulation, kPort1,
      swss::IpAddress(kSrcIpv6), swss::IpAddress(kDstIpv6),
      swss::MacAddress(kSrcMac1), swss::MacAddress(kDstMac1), /*ttl=*/0,
      /*tos=*/0, kPort3, kVlanIdNum1, kUdpSrcPortNum1, kUdpDstPortNum1);
  EXPECT_EQ(*mirror_entry, expected_mirror_entry);

  sai_object_id_t oid_in_mapper = 0;
  EXPECT_TRUE(p4_oid_mapper_.getOID(
      SAI_OBJECT_TYPE_MIRROR_SESSION,
      KeyGenerator::generateMirrorSessionKey(kMirrorSessionId),
      &oid_in_mapper));
  EXPECT_EQ(kMirrorSessionOid, oid_in_mapper);

}

TEST_F(MirrorSessionManagerTest, DrainNotExecuted) {
  std::vector<swss::FieldValueTuple> fvs{
      {p4orch::kAction, p4orch::kMirrorAsIpv4Erspan},
      {prependParamField(p4orch::kPort), kPort1},
      {prependParamField(p4orch::kSrcIp), kSrcIp1},
      {prependParamField(p4orch::kDstIp), kDstIp1},
      {prependParamField(p4orch::kSrcMac), kSrcMac1},
      {prependParamField(p4orch::kDstMac), kDstMac1},
      {prependParamField(p4orch::kTtl), kTtl1},
      {prependParamField(p4orch::kTos), kTos1}};

  nlohmann::json j;
  j[prependMatchField(p4orch::kMirrorSessionId)] = "1";
  swss::KeyOpFieldsValuesTuple app_db_entry_1(
      std::string(APP_P4RT_MIRROR_SESSION_TABLE_NAME) + kTableKeyDelimiter +
          j.dump(),
      SET_COMMAND, fvs);
  j[prependMatchField(p4orch::kMirrorSessionId)] = "2";
  swss::KeyOpFieldsValuesTuple app_db_entry_2(
      std::string(APP_P4RT_MIRROR_SESSION_TABLE_NAME) + kTableKeyDelimiter +
          j.dump(),
      SET_COMMAND, fvs);
  j[prependMatchField(p4orch::kMirrorSessionId)] = "3";
  swss::KeyOpFieldsValuesTuple app_db_entry_3(
      std::string(APP_P4RT_MIRROR_SESSION_TABLE_NAME) + kTableKeyDelimiter +
          j.dump(),
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
  EXPECT_EQ(nullptr,
            GetMirrorSessionEntry(KeyGenerator::generateMirrorSessionKey("1")));
  EXPECT_EQ(nullptr,
            GetMirrorSessionEntry(KeyGenerator::generateMirrorSessionKey("2")));
  EXPECT_EQ(nullptr,
            GetMirrorSessionEntry(KeyGenerator::generateMirrorSessionKey("3")));
}

TEST_F(MirrorSessionManagerTest, DrainStopOnFirstFailure) {
  std::vector<swss::FieldValueTuple> fvs{
      {p4orch::kAction, p4orch::kMirrorAsIpv4Erspan},
      {prependParamField(p4orch::kPort), kPort1},
      {prependParamField(p4orch::kSrcIp), kSrcIp1},
      {prependParamField(p4orch::kDstIp), kDstIp1},
      {prependParamField(p4orch::kSrcMac), kSrcMac1},
      {prependParamField(p4orch::kDstMac), kDstMac1},
      {prependParamField(p4orch::kTtl), kTtl1},
      {prependParamField(p4orch::kTos), kTos1}};

  nlohmann::json j;
  j[prependMatchField(p4orch::kMirrorSessionId)] = "1";
  swss::KeyOpFieldsValuesTuple app_db_entry_1(
      std::string(APP_P4RT_MIRROR_SESSION_TABLE_NAME) + kTableKeyDelimiter +
          j.dump(),
      SET_COMMAND, fvs);
  j[prependMatchField(p4orch::kMirrorSessionId)] = "2";
  swss::KeyOpFieldsValuesTuple app_db_entry_2(
      std::string(APP_P4RT_MIRROR_SESSION_TABLE_NAME) + kTableKeyDelimiter +
          j.dump(),
      SET_COMMAND, fvs);
  j[prependMatchField(p4orch::kMirrorSessionId)] = "3";
  swss::KeyOpFieldsValuesTuple app_db_entry_3(
      std::string(APP_P4RT_MIRROR_SESSION_TABLE_NAME) + kTableKeyDelimiter +
          j.dump(),
      SET_COMMAND, fvs);

  Enqueue(app_db_entry_1);
  Enqueue(app_db_entry_2);
  Enqueue(app_db_entry_3);

  EXPECT_CALL(mock_sai_mirror_, create_mirror_session(_, _, _, _))
      .WillOnce(DoAll(SetArgPointee<0>(kMirrorSessionOid),
                      Return(SAI_STATUS_SUCCESS)))
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
  EXPECT_NE(nullptr,
            GetMirrorSessionEntry(KeyGenerator::generateMirrorSessionKey("1")));
  EXPECT_EQ(nullptr,
            GetMirrorSessionEntry(KeyGenerator::generateMirrorSessionKey("2")));
  EXPECT_EQ(nullptr,
            GetMirrorSessionEntry(KeyGenerator::generateMirrorSessionKey("3")));
}

TEST_F(MirrorSessionManagerTest, VerifyStateTest)
{
    AddDefaultMirrorSession();
    nlohmann::json j;
    j[prependMatchField(p4orch::kMirrorSessionId)] = kMirrorSessionId;
    const std::string db_key = std::string(APP_P4RT_TABLE_NAME) + kTableKeyDelimiter +
                               APP_P4RT_MIRROR_SESSION_TABLE_NAME + kTableKeyDelimiter + j.dump();
    // Setup ASIC DB.
    swss::Table table(nullptr, "ASIC_STATE");
    table.set("SAI_OBJECT_TYPE_MIRROR_SESSION:oid:0x445566",
              std::vector<swss::FieldValueTuple>{
                  swss::FieldValueTuple{"SAI_MIRROR_SESSION_ATTR_MONITOR_PORT", "oid:0x112233"},
                  swss::FieldValueTuple{"SAI_MIRROR_SESSION_ATTR_TYPE", "SAI_MIRROR_SESSION_TYPE_ENHANCED_REMOTE"},
                  swss::FieldValueTuple{"SAI_MIRROR_SESSION_ATTR_ERSPAN_ENCAPSULATION_TYPE",
                                        "SAI_ERSPAN_ENCAPSULATION_TYPE_MIRROR_L3_GRE_TUNNEL"},
                  swss::FieldValueTuple{"SAI_MIRROR_SESSION_ATTR_IPHDR_VERSION", "4"},
                  swss::FieldValueTuple{"SAI_MIRROR_SESSION_ATTR_TOS", "0"},
                  swss::FieldValueTuple{"SAI_MIRROR_SESSION_ATTR_TTL", "64"},
                  swss::FieldValueTuple{"SAI_MIRROR_SESSION_ATTR_SRC_IP_ADDRESS", "10.206.196.31"},
                  swss::FieldValueTuple{"SAI_MIRROR_SESSION_ATTR_DST_IP_ADDRESS", "172.20.0.203"},
                  swss::FieldValueTuple{"SAI_MIRROR_SESSION_ATTR_SRC_MAC_ADDRESS", "00:02:03:04:05:06"},
                  swss::FieldValueTuple{"SAI_MIRROR_SESSION_ATTR_DST_MAC_ADDRESS", "00:1A:11:17:5F:80"},
                  swss::FieldValueTuple{"SAI_MIRROR_SESSION_ATTR_GRE_PROTOCOL_TYPE", "35006"},
              });
    std::vector<swss::FieldValueTuple> attributes;

    // Verification should succeed with vaild key and value.
    attributes.push_back(swss::FieldValueTuple{p4orch::kAction, p4orch::kMirrorAsIpv4Erspan});
    attributes.push_back(swss::FieldValueTuple{prependParamField(p4orch::kPort), kPort1});
    attributes.push_back(swss::FieldValueTuple{prependParamField(p4orch::kSrcIp), kSrcIp1});
    attributes.push_back(swss::FieldValueTuple{prependParamField(p4orch::kDstIp), kDstIp1});
    attributes.push_back(swss::FieldValueTuple{prependParamField(p4orch::kSrcMac), kSrcMac1});
    attributes.push_back(swss::FieldValueTuple{prependParamField(p4orch::kDstMac), kDstMac1});
    attributes.push_back(swss::FieldValueTuple{prependParamField(p4orch::kTtl), kTtl1});
    attributes.push_back(swss::FieldValueTuple{prependParamField(p4orch::kTos), kTos1});
    EXPECT_EQ(VerifyState(db_key, attributes), "");

    // Invalid key should fail verification.
    EXPECT_FALSE(VerifyState("invalid", attributes).empty());
    EXPECT_FALSE(VerifyState("invalid:invalid", attributes).empty());
    EXPECT_FALSE(VerifyState(std::string(APP_P4RT_TABLE_NAME) + ":invalid", attributes).empty());
    EXPECT_FALSE(VerifyState(std::string(APP_P4RT_TABLE_NAME) + ":invalid:invalid", attributes).empty());
    EXPECT_FALSE(
        VerifyState(std::string(APP_P4RT_TABLE_NAME) + ":FIXED_MIRROR_SESSION_TABLE:invalid", attributes).empty());

    // Verification should fail if entry does not exist.
    j[prependMatchField(p4orch::kMirrorSessionId)] = "invalid";
    EXPECT_FALSE(VerifyState(std::string(APP_P4RT_TABLE_NAME) + kTableKeyDelimiter +
                                 APP_P4RT_MIRROR_SESSION_TABLE_NAME + kTableKeyDelimiter + j.dump(),
                             attributes)
                     .empty());

    auto *mirror_entry = GetMirrorSessionEntry(KeyGenerator::generateMirrorSessionKey(kMirrorSessionId));
    ASSERT_NE(mirror_entry, nullptr);

    // Verification should fail if mirror session key mismatches.
  auto saved_mirror_session_key = mirror_entry->mirror_session_key;
  mirror_entry->mirror_session_key = "invalid";
  EXPECT_FALSE(VerifyState(db_key, attributes).empty());
  mirror_entry->mirror_session_key = saved_mirror_session_key;

  // Verification should fail if mirror session ID mismatches.
  auto saved_mirror_session_id = mirror_entry->mirror_session_id;
  mirror_entry->mirror_session_id = "invalid";
  EXPECT_FALSE(VerifyState(db_key, attributes).empty());
  mirror_entry->mirror_session_id = saved_mirror_session_id;

  // Verification should fail if action mismatches.
  auto saved_action = mirror_entry->action;
  mirror_entry->action = p4orch::kMirrorWithVlanTagAndIpfixEncapsulation;
  EXPECT_FALSE(VerifyState(db_key, attributes).empty());
  mirror_entry->action = saved_action;

  // Verification should fail if port mismatches.
  auto saved_port = mirror_entry->port;
  mirror_entry->port = kPort2;
  EXPECT_FALSE(VerifyState(db_key, attributes).empty());
  mirror_entry->port = saved_port;

  // Verification should fail if failover_port mismatches.
  auto saved_failover_port = mirror_entry->failover_port;
  mirror_entry->failover_port = kPort3;
  EXPECT_FALSE(VerifyState(db_key, attributes).empty());
  mirror_entry->failover_port = saved_failover_port;

  // Verification should fail if source IP mismatches.
  auto saved_src_ip = mirror_entry->src_ip;
  mirror_entry->src_ip = swss::IpAddress(kSrcIp2);
  EXPECT_FALSE(VerifyState(db_key, attributes).empty());
  mirror_entry->src_ip = saved_src_ip;

  // Verification should fail if dest IP mismatches.
  auto saved_dst_ip = mirror_entry->dst_ip;
  mirror_entry->dst_ip = swss::IpAddress(kDstIp2);
  EXPECT_FALSE(VerifyState(db_key, attributes).empty());
  mirror_entry->dst_ip = saved_dst_ip;

  // Verification should fail if source MAC mismatches.
  auto saved_src_mac = mirror_entry->src_mac;
  mirror_entry->src_mac = swss::MacAddress(kSrcMac2);
  EXPECT_FALSE(VerifyState(db_key, attributes).empty());
  mirror_entry->src_mac = saved_src_mac;

  // Verification should fail if dest MAC mismatches.
  auto saved_dst_mac = mirror_entry->dst_mac;
  mirror_entry->dst_mac = swss::MacAddress(kDstMac2);
  EXPECT_FALSE(VerifyState(db_key, attributes).empty());
  mirror_entry->dst_mac = saved_dst_mac;

  // Verification should fail if ttl mismatches.
  auto saved_ttl = mirror_entry->ttl;
  mirror_entry->ttl = kTtl2Num;
  EXPECT_FALSE(VerifyState(db_key, attributes).empty());
  mirror_entry->ttl = saved_ttl;

  // Verification should fail if tos mismatches.
  auto saved_tos = mirror_entry->tos;
  mirror_entry->tos = kTos2Num;
  EXPECT_FALSE(VerifyState(db_key, attributes).empty());
  mirror_entry->tos = saved_tos;

  // Verification should fail if VLAN ID mismatches.
  auto saved_vlan_id = mirror_entry->vlan_id;
  mirror_entry->vlan_id = kVlanIdNum2;
  EXPECT_FALSE(VerifyState(db_key, attributes).empty());
  mirror_entry->vlan_id = saved_vlan_id;

  // Verification should fail if UDP source port mismatches.
  auto saved_udp_src_port = mirror_entry->udp_src_port;
  mirror_entry->udp_src_port = kUdpSrcPortNum2;
  EXPECT_FALSE(VerifyState(db_key, attributes).empty());
  mirror_entry->udp_src_port = saved_udp_src_port;

  // Verification should fail if UDP destination port mismatches.
  auto saved_udp_dst_port = mirror_entry->udp_dst_port;
  mirror_entry->udp_dst_port = kUdpDstPortNum2;
  EXPECT_FALSE(VerifyState(db_key, attributes).empty());
  mirror_entry->udp_dst_port = saved_udp_dst_port;

  // Verification should fail if OID mapper mismatches.
  p4_oid_mapper_.eraseOID(
      SAI_OBJECT_TYPE_MIRROR_SESSION,
      KeyGenerator::generateMirrorSessionKey(kMirrorSessionId));
  EXPECT_FALSE(VerifyState(db_key, attributes).empty());
  p4_oid_mapper_.setOID(
      SAI_OBJECT_TYPE_MIRROR_SESSION,
      KeyGenerator::generateMirrorSessionKey(kMirrorSessionId),
      kMirrorSessionOid);
}

TEST_F(MirrorSessionManagerTest, VerifyStateIpfixTest) {
  AddDefaultIpfixMirrorEntry();
  nlohmann::json j;
  j[prependMatchField(p4orch::kMirrorSessionId)] = kMirrorSessionId;
  const std::string db_key =
      std::string(APP_P4RT_TABLE_NAME) + kTableKeyDelimiter +
      APP_P4RT_MIRROR_SESSION_TABLE_NAME + kTableKeyDelimiter + j.dump();
  // Setup ASIC DB.
  swss::Table table(nullptr, "ASIC_STATE");
  // Delete the ASIC DB entry that may exist.
  table.del("SAI_OBJECT_TYPE_MIRROR_SESSION:oid:0x445566");
  table.set(
      "SAI_OBJECT_TYPE_MIRROR_SESSION:oid:0x445566",
      std::vector<swss::FieldValueTuple>{
          swss::FieldValueTuple{"SAI_MIRROR_SESSION_ATTR_MONITOR_PORT",
                                "oid:0x112233"},
          swss::FieldValueTuple{"SAI_MIRROR_SESSION_ATTR_TYPE",
                                "SAI_MIRROR_SESSION_TYPE_IPFIX"},
          swss::FieldValueTuple{
              "SAI_MIRROR_SESSION_ATTR_IPFIX_ENCAPSULATION_TYPE",
              "SAI_IPFIX_ENCAPSULATION_TYPE_EXTENDED"},
          swss::FieldValueTuple{"SAI_MIRROR_SESSION_ATTR_IPHDR_VERSION", "6"},
          swss::FieldValueTuple{"SAI_MIRROR_SESSION_ATTR_TOS", "0"},
          swss::FieldValueTuple{"SAI_MIRROR_SESSION_ATTR_SRC_IP_ADDRESS",
                                kSrcIpv6},
          swss::FieldValueTuple{"SAI_MIRROR_SESSION_ATTR_DST_IP_ADDRESS",
                                kDstIpv6},
          swss::FieldValueTuple{"SAI_MIRROR_SESSION_ATTR_SRC_MAC_ADDRESS",
                                kSrcMac1},
          swss::FieldValueTuple{"SAI_MIRROR_SESSION_ATTR_DST_MAC_ADDRESS",
                                "00:1A:11:17:5F:80"},
          swss::FieldValueTuple{"SAI_MIRROR_SESSION_ATTR_VLAN_TPID", "33024"},
          swss::FieldValueTuple{"SAI_MIRROR_SESSION_ATTR_VLAN_ID", "4660"},
          swss::FieldValueTuple{"SAI_MIRROR_SESSION_ATTR_UDP_SRC_PORT", "128"},
          swss::FieldValueTuple{"SAI_MIRROR_SESSION_ATTR_UDP_DST_PORT", "272"},
      });
  std::vector<swss::FieldValueTuple> attributes;

  // Verification should succeed with vaild key and value.
  attributes.push_back(swss::FieldValueTuple{
      p4orch::kAction, p4orch::kMirrorWithVlanTagAndIpfixEncapsulation});
  attributes.push_back(
      swss::FieldValueTuple{prependParamField(p4orch::kMonitorPort), kPort1});
  attributes.push_back(swss::FieldValueTuple{
      prependParamField(p4orch::kMonitorFailoverPort), kPort3});
  attributes.push_back(swss::FieldValueTuple{
      prependParamField(p4orch::kMirrorEncapSrcIp), kSrcIpv6});
  attributes.push_back(swss::FieldValueTuple{
      prependParamField(p4orch::kMirrorEncapDstIp), kDstIpv6});
  attributes.push_back(swss::FieldValueTuple{
      prependParamField(p4orch::kMirrorEncapSrcMac), kSrcMac1});
  attributes.push_back(swss::FieldValueTuple{
      prependParamField(p4orch::kMirrorEncapDstMac), kDstMac1});
  attributes.push_back(swss::FieldValueTuple{
      prependParamField(p4orch::kMirrorEncapVlanId), kVlanId1});
  attributes.push_back(swss::FieldValueTuple{
      prependParamField(p4orch::kMirrorEncapUdpSrcPort), kUdpSrcPort1});
  attributes.push_back(swss::FieldValueTuple{
      prependParamField(p4orch::kMirrorEncapUdpDstPort), kUdpDstPort1});

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
                               ":FIXED_MIRROR_SESSION_TABLE:invalid",
                           attributes)
                   .empty());

  // Verification should fail if entry does not exist.
  j[prependMatchField(p4orch::kMirrorSessionId)] = "invalid";
  EXPECT_FALSE(VerifyState(std::string(APP_P4RT_TABLE_NAME) +
                               kTableKeyDelimiter +
                               APP_P4RT_MIRROR_SESSION_TABLE_NAME +
                               kTableKeyDelimiter + j.dump(),
                           attributes)
                   .empty());

  auto* mirror_entry = GetMirrorSessionEntry(
      KeyGenerator::generateMirrorSessionKey(kMirrorSessionId));
  ASSERT_NE(mirror_entry, nullptr);

    // Verification should fail if mirror section key mismatches.
    auto saved_mirror_session_key = mirror_entry->mirror_session_key;
    mirror_entry->mirror_session_key = "invalid";
    EXPECT_FALSE(VerifyState(db_key, attributes).empty());
    mirror_entry->mirror_session_key = saved_mirror_session_key;

    // Verification should fail if mirror section ID mismatches.
    auto saved_mirror_session_id = mirror_entry->mirror_session_id;
    mirror_entry->mirror_session_id = "invalid";
    EXPECT_FALSE(VerifyState(db_key, attributes).empty());
    mirror_entry->mirror_session_id = saved_mirror_session_id;

    // Verification should fail if action mismatches.
    auto saved_action = mirror_entry->action;
    mirror_entry->action = p4orch::kMirrorAsIpv4Erspan;
    EXPECT_FALSE(VerifyState(db_key, attributes).empty());
    mirror_entry->action = saved_action;

    // Verification should fail if port mismatches.
    auto saved_port = mirror_entry->port;
    mirror_entry->port = kPort2;
    EXPECT_FALSE(VerifyState(db_key, attributes).empty());
    mirror_entry->port = saved_port;

    // Verification should fail if failover_port mismatches.
    auto saved_failover_port = mirror_entry->failover_port;
    mirror_entry->failover_port = kPort1;
    EXPECT_FALSE(VerifyState(db_key, attributes).empty());
    mirror_entry->failover_port = saved_failover_port;

    // Verification should fail if source IP mismatches.
    auto saved_src_ip = mirror_entry->src_ip;
    mirror_entry->src_ip = swss::IpAddress(kSrcIp2);
    EXPECT_FALSE(VerifyState(db_key, attributes).empty());
    mirror_entry->src_ip = saved_src_ip;

    // Verification should fail if dest IP mismatches.
    auto saved_dst_ip = mirror_entry->dst_ip;
    mirror_entry->dst_ip = swss::IpAddress(kDstIp2);
    EXPECT_FALSE(VerifyState(db_key, attributes).empty());
    mirror_entry->dst_ip = saved_dst_ip;

    // Verification should fail if source MAC mismatches.
    auto saved_src_mac = mirror_entry->src_mac;
    mirror_entry->src_mac = swss::MacAddress(kSrcMac2);
    EXPECT_FALSE(VerifyState(db_key, attributes).empty());
    mirror_entry->src_mac = saved_src_mac;

    // Verification should fail if dest MAC mismatches.
    auto saved_dst_mac = mirror_entry->dst_mac;
    mirror_entry->dst_mac = swss::MacAddress(kDstMac2);
    EXPECT_FALSE(VerifyState(db_key, attributes).empty());
    mirror_entry->dst_mac = saved_dst_mac;

    // Verification should fail if ttl mismatches.
    auto saved_ttl = mirror_entry->ttl;
    mirror_entry->ttl = kTtl2Num;
    EXPECT_FALSE(VerifyState(db_key, attributes).empty());
    mirror_entry->ttl = saved_ttl;

    // Verification should fail if tos mismatches.
    auto saved_tos = mirror_entry->tos;
    mirror_entry->tos = kTos2Num;
    EXPECT_FALSE(VerifyState(db_key, attributes).empty());
    mirror_entry->tos = saved_tos;

    // Verification should fail if VLAN ID mismatches.
    auto saved_vlan_id = mirror_entry->vlan_id;
    mirror_entry->vlan_id = kVlanIdNum2;
    EXPECT_FALSE(VerifyState(db_key, attributes).empty());
    mirror_entry->vlan_id = saved_vlan_id;

    // Verification should fail if UDP source port mismatches.
    auto saved_udp_src_port = mirror_entry->udp_src_port;
    mirror_entry->udp_src_port = kUdpSrcPortNum2;
    EXPECT_FALSE(VerifyState(db_key, attributes).empty());
    mirror_entry->udp_src_port = saved_udp_src_port;

    // Verification should fail if UDP destination port mismatches.
    auto saved_udp_dst_port = mirror_entry->udp_dst_port;
    mirror_entry->udp_dst_port = kUdpDstPortNum2;
    EXPECT_FALSE(VerifyState(db_key, attributes).empty());
    mirror_entry->udp_dst_port = saved_udp_dst_port;

    // Verification should fail if OID mapper mismatches.
    p4_oid_mapper_.eraseOID(SAI_OBJECT_TYPE_MIRROR_SESSION, KeyGenerator::generateMirrorSessionKey(kMirrorSessionId));
    EXPECT_FALSE(VerifyState(db_key, attributes).empty());
    p4_oid_mapper_.setOID(SAI_OBJECT_TYPE_MIRROR_SESSION, KeyGenerator::generateMirrorSessionKey(kMirrorSessionId),
                          kMirrorSessionOid);
}

TEST_F(MirrorSessionManagerTest, VerifyStateAsicDbTest)
{
    AddDefaultMirrorSession();
    nlohmann::json j;
    j[prependMatchField(p4orch::kMirrorSessionId)] = kMirrorSessionId;
    const std::string db_key = std::string(APP_P4RT_TABLE_NAME) + kTableKeyDelimiter +
                               APP_P4RT_MIRROR_SESSION_TABLE_NAME + kTableKeyDelimiter + j.dump();
    // Setup ASIC DB.
    swss::Table table(nullptr, "ASIC_STATE");
    // Delete the ASIC DB entry that may exist.
    table.del("SAI_OBJECT_TYPE_MIRROR_SESSION:oid:0x445566");
    table.set("SAI_OBJECT_TYPE_MIRROR_SESSION:oid:0x445566",
              std::vector<swss::FieldValueTuple>{
                  swss::FieldValueTuple{"SAI_MIRROR_SESSION_ATTR_MONITOR_PORT", "oid:0x112233"},
                  swss::FieldValueTuple{"SAI_MIRROR_SESSION_ATTR_TYPE", "SAI_MIRROR_SESSION_TYPE_ENHANCED_REMOTE"},
                  swss::FieldValueTuple{"SAI_MIRROR_SESSION_ATTR_ERSPAN_ENCAPSULATION_TYPE",
                                        "SAI_ERSPAN_ENCAPSULATION_TYPE_MIRROR_L3_GRE_TUNNEL"},
                  swss::FieldValueTuple{"SAI_MIRROR_SESSION_ATTR_IPHDR_VERSION", "4"},
                  swss::FieldValueTuple{"SAI_MIRROR_SESSION_ATTR_TOS", "0"},
                  swss::FieldValueTuple{"SAI_MIRROR_SESSION_ATTR_TTL", "64"},
                  swss::FieldValueTuple{"SAI_MIRROR_SESSION_ATTR_SRC_IP_ADDRESS", "10.206.196.31"},
                  swss::FieldValueTuple{"SAI_MIRROR_SESSION_ATTR_DST_IP_ADDRESS", "172.20.0.203"},
                  swss::FieldValueTuple{"SAI_MIRROR_SESSION_ATTR_SRC_MAC_ADDRESS", "00:02:03:04:05:06"},
                  swss::FieldValueTuple{"SAI_MIRROR_SESSION_ATTR_DST_MAC_ADDRESS", "00:1A:11:17:5F:80"},
                  swss::FieldValueTuple{"SAI_MIRROR_SESSION_ATTR_GRE_PROTOCOL_TYPE", "35006"},
              });

    std::vector<swss::FieldValueTuple> attributes;

    // Verification should succeed with vaild key and value.
    attributes.push_back(swss::FieldValueTuple{p4orch::kAction, p4orch::kMirrorAsIpv4Erspan});
    attributes.push_back(swss::FieldValueTuple{prependParamField(p4orch::kPort), kPort1});
    attributes.push_back(swss::FieldValueTuple{prependParamField(p4orch::kSrcIp), kSrcIp1});
    attributes.push_back(swss::FieldValueTuple{prependParamField(p4orch::kDstIp), kDstIp1});
    attributes.push_back(swss::FieldValueTuple{prependParamField(p4orch::kSrcMac), kSrcMac1});
    attributes.push_back(swss::FieldValueTuple{prependParamField(p4orch::kDstMac), kDstMac1});
    attributes.push_back(swss::FieldValueTuple{prependParamField(p4orch::kTtl), kTtl1});
    attributes.push_back(swss::FieldValueTuple{prependParamField(p4orch::kTos), kTos1});

    EXPECT_EQ(VerifyState(db_key, attributes), "");

    // set differenet SRC IP ADDR and expect the VerifyState to fail
    table.set("SAI_OBJECT_TYPE_MIRROR_SESSION:oid:0x445566",
              std::vector<swss::FieldValueTuple>{
                  swss::FieldValueTuple{"SAI_MIRROR_SESSION_ATTR_SRC_IP_ADDRESS", "10.206.196.32"}});
    EXPECT_FALSE(VerifyState(db_key, attributes).empty());

    // Delete the ASIC DB entry and expect the VerifyState to fail
    table.del("SAI_OBJECT_TYPE_MIRROR_SESSION:oid:0x445566");
    EXPECT_FALSE(VerifyState(db_key, attributes).empty());

    // Restore the ASIC DB entry
    table.set("SAI_OBJECT_TYPE_MIRROR_SESSION:oid:0x445566",
              std::vector<swss::FieldValueTuple>{
                  swss::FieldValueTuple{"SAI_MIRROR_SESSION_ATTR_MONITOR_PORT", "oid:0x112233"},
                  swss::FieldValueTuple{"SAI_MIRROR_SESSION_ATTR_TYPE", "SAI_MIRROR_SESSION_TYPE_ENHANCED_REMOTE"},
                  swss::FieldValueTuple{"SAI_MIRROR_SESSION_ATTR_ERSPAN_ENCAPSULATION_TYPE",
                                        "SAI_ERSPAN_ENCAPSULATION_TYPE_MIRROR_L3_GRE_TUNNEL"},
                  swss::FieldValueTuple{"SAI_MIRROR_SESSION_ATTR_IPHDR_VERSION", "4"},
                  swss::FieldValueTuple{"SAI_MIRROR_SESSION_ATTR_TOS", "0"},
                  swss::FieldValueTuple{"SAI_MIRROR_SESSION_ATTR_TTL", "64"},
                  swss::FieldValueTuple{"SAI_MIRROR_SESSION_ATTR_SRC_IP_ADDRESS", "10.206.196.31"},
                  swss::FieldValueTuple{"SAI_MIRROR_SESSION_ATTR_DST_IP_ADDRESS", "172.20.0.203"},
                  swss::FieldValueTuple{"SAI_MIRROR_SESSION_ATTR_SRC_MAC_ADDRESS", "00:02:03:04:05:06"},
                  swss::FieldValueTuple{"SAI_MIRROR_SESSION_ATTR_DST_MAC_ADDRESS", "00:1A:11:17:5F:80"},
                  swss::FieldValueTuple{"SAI_MIRROR_SESSION_ATTR_GRE_PROTOCOL_TYPE", "35006"},
              });
}

TEST_F(MirrorSessionManagerTest, VerifyStateAsicDbIpfixTest) {
  AddDefaultIpfixMirrorEntry();
  nlohmann::json j;
  j[prependMatchField(p4orch::kMirrorSessionId)] = kMirrorSessionId;
  const std::string db_key =
      std::string(APP_P4RT_TABLE_NAME) + kTableKeyDelimiter +
      APP_P4RT_MIRROR_SESSION_TABLE_NAME + kTableKeyDelimiter + j.dump();
  // Setup ASIC DB.
  swss::Table table(nullptr, "ASIC_STATE");
  // Delete the ASIC DB entry that may exist.
  table.del("SAI_OBJECT_TYPE_MIRROR_SESSION:oid:0x445566");
  table.set(
      "SAI_OBJECT_TYPE_MIRROR_SESSION:oid:0x445566",
      std::vector<swss::FieldValueTuple>{
          swss::FieldValueTuple{"SAI_MIRROR_SESSION_ATTR_MONITOR_PORT",
                                "oid:0x112233"},
          swss::FieldValueTuple{"SAI_MIRROR_SESSION_ATTR_TYPE",
                                "SAI_MIRROR_SESSION_TYPE_IPFIX"},
          swss::FieldValueTuple{
              "SAI_MIRROR_SESSION_ATTR_IPFIX_ENCAPSULATION_TYPE",
              "SAI_IPFIX_ENCAPSULATION_TYPE_EXTENDED"},
          swss::FieldValueTuple{"SAI_MIRROR_SESSION_ATTR_IPHDR_VERSION", "6"},
          swss::FieldValueTuple{"SAI_MIRROR_SESSION_ATTR_TOS", "0"},
          swss::FieldValueTuple{"SAI_MIRROR_SESSION_ATTR_SRC_IP_ADDRESS",
                                kSrcIpv6},
          swss::FieldValueTuple{"SAI_MIRROR_SESSION_ATTR_DST_IP_ADDRESS",
                                kDstIpv6},
          swss::FieldValueTuple{"SAI_MIRROR_SESSION_ATTR_SRC_MAC_ADDRESS",
                                kSrcMac1},
          swss::FieldValueTuple{"SAI_MIRROR_SESSION_ATTR_DST_MAC_ADDRESS",
                                "00:1A:11:17:5F:80"},
          swss::FieldValueTuple{"SAI_MIRROR_SESSION_ATTR_VLAN_TPID", "33024"},
          swss::FieldValueTuple{"SAI_MIRROR_SESSION_ATTR_VLAN_ID", "4660"},
          swss::FieldValueTuple{"SAI_MIRROR_SESSION_ATTR_UDP_SRC_PORT", "128"},
          swss::FieldValueTuple{"SAI_MIRROR_SESSION_ATTR_UDP_DST_PORT", "272"},
      });

  std::vector<swss::FieldValueTuple> attributes;

  // Verification should succeed with vaild key and value.
  attributes.push_back(swss::FieldValueTuple{
      p4orch::kAction, p4orch::kMirrorWithVlanTagAndIpfixEncapsulation});
  attributes.push_back(
      swss::FieldValueTuple{prependParamField(p4orch::kMonitorPort), kPort1});
  attributes.push_back(swss::FieldValueTuple{
      prependParamField(p4orch::kMonitorFailoverPort), kPort3});
  attributes.push_back(swss::FieldValueTuple{
      prependParamField(p4orch::kMirrorEncapSrcIp), kSrcIpv6});
  attributes.push_back(swss::FieldValueTuple{
      prependParamField(p4orch::kMirrorEncapDstIp), kDstIpv6});
  attributes.push_back(swss::FieldValueTuple{
      prependParamField(p4orch::kMirrorEncapSrcMac), kSrcMac1});
  attributes.push_back(swss::FieldValueTuple{
      prependParamField(p4orch::kMirrorEncapDstMac), kDstMac1});
  attributes.push_back(swss::FieldValueTuple{
      prependParamField(p4orch::kMirrorEncapVlanId), kVlanId1});
  attributes.push_back(swss::FieldValueTuple{
      prependParamField(p4orch::kMirrorEncapUdpSrcPort), kUdpSrcPort1});
  attributes.push_back(swss::FieldValueTuple{
      prependParamField(p4orch::kMirrorEncapUdpDstPort), kUdpDstPort1});

  EXPECT_EQ(VerifyState(db_key, attributes), "");

  // set differenet UDP src port and expect the VerifyState to fail
  table.set("SAI_OBJECT_TYPE_MIRROR_SESSION:oid:0x445566",
            std::vector<swss::FieldValueTuple>{swss::FieldValueTuple{
                "SAI_MIRROR_SESSION_ATTR_UDP_SRC_PORT", "129"}});
  EXPECT_FALSE(VerifyState(db_key, attributes).empty());

  // Delete the ASIC DB entry and expect the VerifyState to fail
  table.del("SAI_OBJECT_TYPE_MIRROR_SESSION:oid:0x445566");
  EXPECT_FALSE(VerifyState(db_key, attributes).empty());

  // Restore the ASIC DB entry
  table.set(
      "SAI_OBJECT_TYPE_MIRROR_SESSION:oid:0x445566",
      std::vector<swss::FieldValueTuple>{
          swss::FieldValueTuple{"SAI_MIRROR_SESSION_ATTR_MONITOR_PORT",
                                "oid:0x112233"},
          swss::FieldValueTuple{"SAI_MIRROR_SESSION_ATTR_TYPE",
                                "SAI_MIRROR_SESSION_TYPE_IPFIX"},
          swss::FieldValueTuple{
              "SAI_MIRROR_SESSION_ATTR_IPFIX_ENCAPSULATION_TYPE",
              "SAI_IPFIX_ENCAPSULATION_TYPE_EXTENDED"},
          swss::FieldValueTuple{"SAI_MIRROR_SESSION_ATTR_IPHDR_VERSION", "6"},
          swss::FieldValueTuple{"SAI_MIRROR_SESSION_ATTR_TOS", "0"},
          swss::FieldValueTuple{"SAI_MIRROR_SESSION_ATTR_SRC_IP_ADDRESS",
                                kSrcIpv6},
          swss::FieldValueTuple{"SAI_MIRROR_SESSION_ATTR_DST_IP_ADDRESS",
                                kDstIpv6},
          swss::FieldValueTuple{"SAI_MIRROR_SESSION_ATTR_SRC_MAC_ADDRESS",
                                kSrcMac1},
          swss::FieldValueTuple{"SAI_MIRROR_SESSION_ATTR_DST_MAC_ADDRESS",
                                "00:1A:11:17:5F:80"},
          swss::FieldValueTuple{"SAI_MIRROR_SESSION_ATTR_VLAN_TPID", "33024"},
          swss::FieldValueTuple{"SAI_MIRROR_SESSION_ATTR_VLAN_ID", "4660"},
          swss::FieldValueTuple{"SAI_MIRROR_SESSION_ATTR_UDP_SRC_PORT", "128"},
          swss::FieldValueTuple{"SAI_MIRROR_SESSION_ATTR_UDP_DST_PORT", "272"},
      });
  EXPECT_EQ(VerifyState(db_key, attributes), "");
}

} // namespace test
} // namespace p4orch
