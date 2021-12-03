#include "p4orch/mirror_session_manager.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "json.hpp"
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
constexpr sai_object_id_t kPort3Oid = 0xaabbccdd;
constexpr char *kSrcIp1 = "10.206.196.31";
constexpr char *kSrcIp2 = "10.206.196.32";
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
        return mirror_session_manager_.enqueue(entry);
    }

    void Drain()
    {
        return mirror_session_manager_.drain();
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

    ReturnCode ProcessDeleteRequest(const std::string &mirror_session_key)
    {
        return mirror_session_manager_.processDeleteRequest(mirror_session_key);
    }

    void AddDefaultMirrorSection()
    {
        P4MirrorSessionAppDbEntry app_db_entry;
        app_db_entry.mirror_session_id = kMirrorSessionId;
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
        ASSERT_EQ(StatusCode::SWSS_RC_SUCCESS, ProcessAddRequest(app_db_entry));
    }

    StrictMock<MockSaiMirror> mock_sai_mirror_;
    MockResponsePublisher publisher_;
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
    Drain();

    // Check the added entry.
    auto mirror_entry = GetMirrorSessionEntry(KeyGenerator::generateMirrorSessionKey(kMirrorSessionId));
    ASSERT_NE(mirror_entry, nullptr);
    p4orch::P4MirrorSessionEntry expected_mirror_entry(
        KeyGenerator::generateMirrorSessionKey(kMirrorSessionId), kMirrorSessionOid, kMirrorSessionId, kPort1,
        swss::IpAddress(kSrcIp1), swss::IpAddress(kDstIp1), swss::MacAddress(kSrcMac1), swss::MacAddress(kDstMac1),
        kTtl1Num, kTos1Num);
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

    Drain();

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
    Drain();

    mirror_entry = GetMirrorSessionEntry(KeyGenerator::generateMirrorSessionKey(kMirrorSessionId));
    EXPECT_EQ(mirror_entry, nullptr);

    EXPECT_FALSE(p4_oid_mapper_.existsOID(SAI_OBJECT_TYPE_MIRROR_SESSION,
                                          KeyGenerator::generateMirrorSessionKey(kMirrorSessionId)));
}

TEST_F(MirrorSessionManagerTest, DrainShouldFailForInvalidAppDbEntryMatchFiled)
{
    nlohmann::json j;
    j[prependMatchField("invalid_match_field")] = kMirrorSessionId;

    std::vector<swss::FieldValueTuple> fvs{
        {p4orch::kAction, p4orch::kMirrorAsIpv4Erspan}, {prependParamField(p4orch::kPort), kPort1},
        {prependParamField(p4orch::kSrcIp), kSrcIp1},   {prependParamField(p4orch::kDstIp), kDstIp1},
        {prependParamField(p4orch::kSrcMac), kSrcMac1}, {prependParamField(p4orch::kDstMac), kDstMac1},
        {prependParamField(p4orch::kTtl), kTtl1},       {prependParamField(p4orch::kTos), kTos1}};

    swss::KeyOpFieldsValuesTuple app_db_entry(
        std::string(APP_P4RT_MIRROR_SESSION_TABLE_NAME) + kTableKeyDelimiter + j.dump(), SET_COMMAND, fvs);

    Enqueue(app_db_entry);
    Drain();

    // Check the added entry.
    auto mirror_entry = GetMirrorSessionEntry(KeyGenerator::generateMirrorSessionKey(kMirrorSessionId));
    EXPECT_EQ(mirror_entry, nullptr);
}

TEST_F(MirrorSessionManagerTest, DrainShouldFailForUnknownOp)
{
    nlohmann::json j;
    j[prependMatchField(p4orch::kMirrorSessionId)] = kMirrorSessionId;

    std::vector<swss::FieldValueTuple> fvs{
        {p4orch::kAction, p4orch::kMirrorAsIpv4Erspan}, {prependParamField(p4orch::kPort), kPort1},
        {prependParamField(p4orch::kSrcIp), kSrcIp1},   {prependParamField(p4orch::kDstIp), kDstIp1},
        {prependParamField(p4orch::kSrcMac), kSrcMac1}, {prependParamField(p4orch::kDstMac), kDstMac1},
        {prependParamField(p4orch::kTtl), kTtl1},       {prependParamField(p4orch::kTos), kTos1}};

    swss::KeyOpFieldsValuesTuple app_db_entry(
        std::string(APP_P4RT_MIRROR_SESSION_TABLE_NAME) + kTableKeyDelimiter + j.dump(), "unknown_op", fvs);

    Enqueue(app_db_entry);
    Drain();

    // Check the added entry.
    auto mirror_entry = GetMirrorSessionEntry(KeyGenerator::generateMirrorSessionKey(kMirrorSessionId));
    EXPECT_EQ(mirror_entry, nullptr);
}

TEST_F(MirrorSessionManagerTest, DrainShouldFailForInvalidAppDbEntryFieldValue)
{
    nlohmann::json j;
    j[prependMatchField(p4orch::kMirrorSessionId)] = kMirrorSessionId;

    std::vector<swss::FieldValueTuple> fvs{
        {p4orch::kAction, p4orch::kMirrorAsIpv4Erspan},    {prependParamField(p4orch::kPort), kPort1},
        {prependParamField(p4orch::kSrcIp), "0123456789"}, {prependParamField(p4orch::kDstIp), kDstIp1},
        {prependParamField(p4orch::kSrcMac), kSrcMac1},    {prependParamField(p4orch::kDstMac), kDstMac1},
        {prependParamField(p4orch::kTtl), kTtl1},          {prependParamField(p4orch::kTos), kTos1}};

    swss::KeyOpFieldsValuesTuple app_db_entry(
        std::string(APP_P4RT_MIRROR_SESSION_TABLE_NAME) + kTableKeyDelimiter + j.dump(), SET_COMMAND, fvs);

    Enqueue(app_db_entry);
    Drain();

    // Check the added entry.
    auto mirror_entry = GetMirrorSessionEntry(KeyGenerator::generateMirrorSessionKey(kMirrorSessionId));
    EXPECT_EQ(mirror_entry, nullptr);
}

TEST_F(MirrorSessionManagerTest, DrainShouldFailForUnknownAppDbEntryFieldValue)
{
    nlohmann::json j;
    j[prependMatchField(p4orch::kMirrorSessionId)] = kMirrorSessionId;

    std::vector<swss::FieldValueTuple> fvs{{p4orch::kAction, p4orch::kMirrorAsIpv4Erspan},
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
    Drain();

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
    Drain();

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
    Drain();

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
    Drain();

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
    Drain();

    fvs = {};
    app_db_entry = {std::string(APP_P4RT_MIRROR_SESSION_TABLE_NAME) + kTableKeyDelimiter + j.dump(), DEL_COMMAND, fvs};

    Enqueue(app_db_entry);
    EXPECT_CALL(mock_sai_mirror_, remove_mirror_session(Eq(kMirrorSessionOid))).WillOnce(Return(SAI_STATUS_FAILURE));
    Drain();

    // Check entry still exists.
    auto mirror_entry = GetMirrorSessionEntry(KeyGenerator::generateMirrorSessionKey(kMirrorSessionId));
    ASSERT_NE(mirror_entry, nullptr);
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

    std::vector<swss::FieldValueTuple> unsupported_port = {{prependParamField(p4orch::kPort), kPort2}};
    EXPECT_FALSE(DeserializeP4MirrorSessionAppDbEntry(kValidKey, unsupported_port).ok());

    std::vector<swss::FieldValueTuple> invalid_action_value = {{p4orch::kAction, "abc"}};
    EXPECT_FALSE(DeserializeP4MirrorSessionAppDbEntry(kValidKey, invalid_action_value).ok());
}

TEST_F(MirrorSessionManagerTest, CreateExistingMirrorSessionInMapperShouldFail)
{
    p4orch::P4MirrorSessionEntry mirror_session_entry(
        KeyGenerator::generateMirrorSessionKey(kMirrorSessionId), kMirrorSessionOid, kMirrorSessionId, kPort1,
        swss::IpAddress(kSrcIp1), swss::IpAddress(kDstIp1), swss::MacAddress(kSrcMac1), swss::MacAddress(kDstMac1),
        kTtl1Num, kTos1Num);

    // Add this mirror session's oid to centralized mapper.
    ASSERT_TRUE(p4_oid_mapper_.setOID(SAI_OBJECT_TYPE_MIRROR_SESSION, mirror_session_entry.mirror_session_key,
                                      mirror_session_entry.mirror_session_oid));

    // (TODO): Expect critical state.
    EXPECT_FALSE(CreateMirrorSession(mirror_session_entry).ok());
}

TEST_F(MirrorSessionManagerTest, CreateMirrorSessionWithInvalidPortShouldFail)
{
    // Non-existing port.
    p4orch::P4MirrorSessionEntry mirror_session_entry(
        KeyGenerator::generateMirrorSessionKey(kMirrorSessionId), kMirrorSessionOid, kMirrorSessionId,
        "Non-existing Port", swss::IpAddress(kSrcIp1), swss::IpAddress(kDstIp1), swss::MacAddress(kSrcMac1),
        swss::MacAddress(kDstMac1), kTtl1Num, kTos1Num);

    EXPECT_FALSE(CreateMirrorSession(mirror_session_entry).ok());

    // Unsupported management port.
    mirror_session_entry.port = kPort2;
    EXPECT_FALSE(CreateMirrorSession(mirror_session_entry).ok());
}

TEST_F(MirrorSessionManagerTest, UpdatingNonexistingMirrorSessionShouldFail)
{
    P4MirrorSessionAppDbEntry app_db_entry;
    // Fail because existing_mirror_session_entry is nullptr.
    // (TODO): Expect critical state.
    EXPECT_FALSE(ProcessUpdateRequest(app_db_entry,
                                      /*existing_mirror_session_entry=*/nullptr)
                     .ok());

    p4orch::P4MirrorSessionEntry existing_mirror_session_entry(
        KeyGenerator::generateMirrorSessionKey(kMirrorSessionId), kMirrorSessionOid, kMirrorSessionId, kPort1,
        swss::IpAddress(kSrcIp1), swss::IpAddress(kDstIp1), swss::MacAddress(kSrcMac1), swss::MacAddress(kDstMac1),
        kTtl1Num, kTos1Num);

    // Fail because the mirror session is not added into centralized mapper.
    // (TODO): Expect critical state.
    EXPECT_FALSE(ProcessUpdateRequest(app_db_entry, &existing_mirror_session_entry).ok());
}

TEST_F(MirrorSessionManagerTest, UpdatingPortFailureCases)
{
    p4orch::P4MirrorSessionEntry existing_mirror_session_entry(
        KeyGenerator::generateMirrorSessionKey(kMirrorSessionId), kMirrorSessionOid, kMirrorSessionId, kPort1,
        swss::IpAddress(kSrcIp1), swss::IpAddress(kDstIp1), swss::MacAddress(kSrcMac1), swss::MacAddress(kDstMac1),
        kTtl1Num, kTos1Num);
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
        KeyGenerator::generateMirrorSessionKey(kMirrorSessionId), kMirrorSessionOid, kMirrorSessionId, kPort1,
        swss::IpAddress(kSrcIp1), swss::IpAddress(kDstIp1), swss::MacAddress(kSrcMac1), swss::MacAddress(kDstMac1),
        kTtl1Num, kTos1Num);
    EXPECT_CALL(mock_sai_mirror_, set_mirror_session_attribute(_, _)).WillOnce(Return(SAI_STATUS_FAILURE));
    p4orch::P4MirrorSessionEntry existing_mirror_session_entry_before_update(existing_mirror_session_entry);
    EXPECT_FALSE(SetSrcIp(swss::IpAddress(kSrcIp2), &existing_mirror_session_entry).ok());
    EXPECT_EQ(existing_mirror_session_entry, existing_mirror_session_entry_before_update);
}

TEST_F(MirrorSessionManagerTest, UpdatingDstIpSaiFailure)
{
    p4orch::P4MirrorSessionEntry existing_mirror_session_entry(
        KeyGenerator::generateMirrorSessionKey(kMirrorSessionId), kMirrorSessionOid, kMirrorSessionId, kPort1,
        swss::IpAddress(kSrcIp1), swss::IpAddress(kDstIp1), swss::MacAddress(kSrcMac1), swss::MacAddress(kDstMac1),
        kTtl1Num, kTos1Num);
    EXPECT_CALL(mock_sai_mirror_, set_mirror_session_attribute(_, _)).WillOnce(Return(SAI_STATUS_FAILURE));
    p4orch::P4MirrorSessionEntry existing_mirror_session_entry_before_update(existing_mirror_session_entry);
    EXPECT_FALSE(SetDstIp(swss::IpAddress(kDstIp2), &existing_mirror_session_entry).ok());
    EXPECT_EQ(existing_mirror_session_entry, existing_mirror_session_entry_before_update);
}

TEST_F(MirrorSessionManagerTest, UpdatingSrcMacSaiFailure)
{
    p4orch::P4MirrorSessionEntry existing_mirror_session_entry(
        KeyGenerator::generateMirrorSessionKey(kMirrorSessionId), kMirrorSessionOid, kMirrorSessionId, kPort1,
        swss::IpAddress(kSrcIp1), swss::IpAddress(kDstIp1), swss::MacAddress(kSrcMac1), swss::MacAddress(kDstMac1),
        kTtl1Num, kTos1Num);
    EXPECT_CALL(mock_sai_mirror_, set_mirror_session_attribute(_, _)).WillOnce(Return(SAI_STATUS_FAILURE));
    p4orch::P4MirrorSessionEntry existing_mirror_session_entry_before_update(existing_mirror_session_entry);
    EXPECT_FALSE(SetSrcMac(swss::MacAddress(kSrcMac2), &existing_mirror_session_entry).ok());
    EXPECT_EQ(existing_mirror_session_entry, existing_mirror_session_entry_before_update);
}

TEST_F(MirrorSessionManagerTest, UpdatingDstMacSaiFailure)
{
    p4orch::P4MirrorSessionEntry existing_mirror_session_entry(
        KeyGenerator::generateMirrorSessionKey(kMirrorSessionId), kMirrorSessionOid, kMirrorSessionId, kPort1,
        swss::IpAddress(kSrcIp1), swss::IpAddress(kDstIp1), swss::MacAddress(kSrcMac1), swss::MacAddress(kDstMac1),
        kTtl1Num, kTos1Num);
    EXPECT_CALL(mock_sai_mirror_, set_mirror_session_attribute(_, _)).WillOnce(Return(SAI_STATUS_FAILURE));
    p4orch::P4MirrorSessionEntry existing_mirror_session_entry_before_update(existing_mirror_session_entry);
    EXPECT_FALSE(SetDstMac(swss::MacAddress(kDstMac2), &existing_mirror_session_entry).ok());
    EXPECT_EQ(existing_mirror_session_entry, existing_mirror_session_entry_before_update);
}

TEST_F(MirrorSessionManagerTest, UpdatingTtlSaiFailure)
{
    p4orch::P4MirrorSessionEntry existing_mirror_session_entry(
        KeyGenerator::generateMirrorSessionKey(kMirrorSessionId), kMirrorSessionOid, kMirrorSessionId, kPort1,
        swss::IpAddress(kSrcIp1), swss::IpAddress(kDstIp1), swss::MacAddress(kSrcMac1), swss::MacAddress(kDstMac1),
        kTtl1Num, kTos1Num);
    EXPECT_CALL(mock_sai_mirror_, set_mirror_session_attribute(_, _)).WillOnce(Return(SAI_STATUS_FAILURE));
    p4orch::P4MirrorSessionEntry existing_mirror_session_entry_before_update(existing_mirror_session_entry);
    EXPECT_FALSE(SetTtl(kTtl2Num, &existing_mirror_session_entry).ok());
    EXPECT_EQ(existing_mirror_session_entry, existing_mirror_session_entry_before_update);
}

TEST_F(MirrorSessionManagerTest, UpdatingTosSaiFailure)
{
    p4orch::P4MirrorSessionEntry existing_mirror_session_entry(
        KeyGenerator::generateMirrorSessionKey(kMirrorSessionId), kMirrorSessionOid, kMirrorSessionId, kPort1,
        swss::IpAddress(kSrcIp1), swss::IpAddress(kDstIp1), swss::MacAddress(kSrcMac1), swss::MacAddress(kDstMac1),
        kTtl1Num, kTos1Num);
    EXPECT_CALL(mock_sai_mirror_, set_mirror_session_attribute(_, _)).WillOnce(Return(SAI_STATUS_FAILURE));
    p4orch::P4MirrorSessionEntry existing_mirror_session_entry_before_update(existing_mirror_session_entry);
    EXPECT_FALSE(SetTos(kTos2Num, &existing_mirror_session_entry).ok());
    EXPECT_EQ(existing_mirror_session_entry, existing_mirror_session_entry_before_update);
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
    Drain();

    // Check the added entry.
    auto mirror_entry = GetMirrorSessionEntry(KeyGenerator::generateMirrorSessionKey(kMirrorSessionId));
    ASSERT_NE(mirror_entry, nullptr);
    p4orch::P4MirrorSessionEntry expected_mirror_entry(
        KeyGenerator::generateMirrorSessionKey(kMirrorSessionId), kMirrorSessionOid, kMirrorSessionId, kPort1,
        swss::IpAddress(kSrcIp1), swss::IpAddress(kDstIp1), swss::MacAddress(kSrcMac1), swss::MacAddress(kDstMac1),
        kTtl1Num, kTos1Num);
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

    Drain();

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
    Drain();

    // Check the added entry.
    auto mirror_entry = GetMirrorSessionEntry(KeyGenerator::generateMirrorSessionKey(kMirrorSessionId));
    ASSERT_NE(mirror_entry, nullptr);
    p4orch::P4MirrorSessionEntry expected_mirror_entry(
        KeyGenerator::generateMirrorSessionKey(kMirrorSessionId), kMirrorSessionOid, kMirrorSessionId, kPort1,
        swss::IpAddress(kSrcIp1), swss::IpAddress(kDstIp1), swss::MacAddress(kSrcMac1), swss::MacAddress(kDstMac1),
        kTtl1Num, kTos1Num);
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
    // (TODO): Expect critical state.

    Drain();

    mirror_entry = GetMirrorSessionEntry(KeyGenerator::generateMirrorSessionKey(kMirrorSessionId));
    ASSERT_NE(mirror_entry, nullptr);
}

TEST_F(MirrorSessionManagerTest, DeleteNonExistingMirrorSessionShouldFail)
{
    ASSERT_EQ(StatusCode::SWSS_RC_NOT_FOUND,
              ProcessDeleteRequest(KeyGenerator::generateMirrorSessionKey(kMirrorSessionId)));
}

TEST_F(MirrorSessionManagerTest, DeleteMirrorSessionWithNonZeroRefShouldFail)
{
    AddDefaultMirrorSection();
    p4_oid_mapper_.increaseRefCount(SAI_OBJECT_TYPE_MIRROR_SESSION,
                                    KeyGenerator::generateMirrorSessionKey(kMirrorSessionId));
    ASSERT_EQ(StatusCode::SWSS_RC_IN_USE,
              ProcessDeleteRequest(KeyGenerator::generateMirrorSessionKey(kMirrorSessionId)));
}

TEST_F(MirrorSessionManagerTest, DeleteMirrorSessionNotInMapperShouldFail)
{
    AddDefaultMirrorSection();
    p4_oid_mapper_.eraseOID(SAI_OBJECT_TYPE_MIRROR_SESSION, KeyGenerator::generateMirrorSessionKey(kMirrorSessionId));
    // (TODO): Expect critical state.
    ASSERT_EQ(StatusCode::SWSS_RC_INTERNAL,
              ProcessDeleteRequest(KeyGenerator::generateMirrorSessionKey(kMirrorSessionId)));
}

} // namespace test
} // namespace p4orch
