#include "l3_admit_manager.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <functional>
#include <string>
#include <unordered_map>

#include <nlohmann/json.hpp>
#include "mock_response_publisher.h"
#include "mock_sai_my_mac.h"
#include "p4oidmapper.h"
#include "p4orch/p4orch_util.h"
#include "p4orch_util.h"
#include "return_code.h"
extern "C"
{
#include "sai.h"
}

using ::p4orch::kTableKeyDelimiter;

using ::testing::_;
using ::testing::DoAll;
using ::testing::Eq;
using ::testing::Return;
using ::testing::SetArgPointee;
using ::testing::StrictMock;
using ::testing::Truly;

extern sai_object_id_t gSwitchId;
extern sai_my_mac_api_t *sai_my_mac_api;
extern MockSaiMyMac *mock_sai_my_mac;

namespace
{
constexpr char *kPortName1 = "Ethernet1";
constexpr sai_object_id_t kPortOid1 = 0x112233;
constexpr uint32_t kMtu1 = 1500;

constexpr char *kPortName2 = "Ethernet2";
constexpr sai_object_id_t kPortOid2 = 0x1fed3;
constexpr uint32_t kMtu2 = 4500;

constexpr char *kL3AdmitP4AppDbKey1 = R"({"match/dst_mac":"00:02:03:04:00:00&ff:ff:ff:ff:00:00","priority":2030})";
constexpr sai_object_id_t kL3AdmitOid1 = 0x1;
constexpr sai_object_id_t kL3AdmitOid2 = 0x2;

// APP DB entries for Add request.
const P4L3AdmitAppDbEntry kP4L3AdmitAppDbEntry1{/*port_name=*/"",
                                                /*mac_address_data=*/swss::MacAddress("00:02:03:04:00:00"),
                                                /*mac_address_mask=*/swss::MacAddress("ff:ff:ff:ff:00:00"),
                                                /*priority=*/2030};

const P4L3AdmitAppDbEntry kP4L3AdmitAppDbEntry2{/*port_name=*/kPortName1,
                                                /*mac_address_data=*/swss::MacAddress("00:02:03:04:05:00"),
                                                /*mac_address_mask=*/swss::MacAddress("ff:ff:ff:ff:ff:00"),
                                                /*priority=*/2030};

std::unordered_map<sai_attr_id_t, sai_attribute_value_t> CreateAttributeListForL3AdmitObject(
    const P4L3AdmitAppDbEntry &app_entry, const sai_object_id_t &port_oid)
{
    std::unordered_map<sai_attr_id_t, sai_attribute_value_t> my_mac_attrs;
    sai_attribute_t my_mac_attr;

    my_mac_attr.id = SAI_MY_MAC_ATTR_PRIORITY;
    my_mac_attr.value.u32 = app_entry.priority;
    my_mac_attrs.insert({my_mac_attr.id, my_mac_attr.value});

    my_mac_attr.id = SAI_MY_MAC_ATTR_MAC_ADDRESS;
    memcpy(my_mac_attr.value.mac, app_entry.mac_address_data.getMac(), sizeof(sai_mac_t));
    my_mac_attrs.insert({my_mac_attr.id, my_mac_attr.value});

    my_mac_attr.id = SAI_MY_MAC_ATTR_MAC_ADDRESS_MASK;
    memcpy(my_mac_attr.value.mac, app_entry.mac_address_mask.getMac(), sizeof(sai_mac_t));
    my_mac_attrs.insert({my_mac_attr.id, my_mac_attr.value});

    if (port_oid != SAI_NULL_OBJECT_ID)
    {
        my_mac_attr.id = SAI_MY_MAC_ATTR_PORT_ID;
        my_mac_attr.value.oid = port_oid;
        my_mac_attrs.insert({my_mac_attr.id, my_mac_attr.value});
    }

    return my_mac_attrs;
}

// Verifies whether the attribute list is the same as expected.
// Returns true if they match; otherwise, false.
bool MatchCreateL3AdmitArgAttrList(const sai_attribute_t *attr_list,
                                   const std::unordered_map<sai_attr_id_t, sai_attribute_value_t> &expected_attr_list)
{
    if (attr_list == nullptr)
    {
        return false;
    }

    // Sanity check for expected_attr_list.
    const auto end = expected_attr_list.end();
    if (expected_attr_list.size() < 3 || expected_attr_list.find(SAI_MY_MAC_ATTR_PRIORITY) == end ||
        expected_attr_list.find(SAI_MY_MAC_ATTR_MAC_ADDRESS) == end ||
        expected_attr_list.find(SAI_MY_MAC_ATTR_MAC_ADDRESS_MASK) == end)
    {
        return false;
    }

    size_t valid_attrs_num = 0;
    for (size_t i = 0; i < expected_attr_list.size(); ++i)
    {
        switch (attr_list[i].id)
        {
        case SAI_MY_MAC_ATTR_PRIORITY: {
            if (attr_list[i].value.u32 != expected_attr_list.at(SAI_MY_MAC_ATTR_PRIORITY).u32)
            {
                return false;
            }
            valid_attrs_num++;
            break;
        }
        case SAI_MY_MAC_ATTR_MAC_ADDRESS: {
            auto macaddr = swss::MacAddress(attr_list[i].value.mac);
            auto expected_macaddr = swss::MacAddress(expected_attr_list.at(SAI_MY_MAC_ATTR_MAC_ADDRESS).mac);
            if (macaddr != expected_macaddr)
            {
                return false;
            }
            valid_attrs_num++;
            break;
        }
        case SAI_MY_MAC_ATTR_MAC_ADDRESS_MASK: {
            auto macaddr = swss::MacAddress(attr_list[i].value.mac);
            auto expected_macaddr = swss::MacAddress(expected_attr_list.at(SAI_MY_MAC_ATTR_MAC_ADDRESS_MASK).mac);
            if (macaddr != expected_macaddr)
            {
                return false;
            }
            valid_attrs_num++;
            break;
        }
        case SAI_MY_MAC_ATTR_PORT_ID: {
            if (expected_attr_list.find(SAI_MY_MAC_ATTR_PORT_ID) == end ||
                expected_attr_list.at(SAI_MY_MAC_ATTR_PORT_ID).oid != attr_list[i].value.oid)
            {
                return false;
            }
            valid_attrs_num++;
            break;
        }
        default:
            return false;
        }
    }

    if (expected_attr_list.size() != valid_attrs_num)
    {
        return false;
    }

    return true;
}
} // namespace

class L3AdmitManagerTest : public ::testing::Test
{
  protected:
    L3AdmitManagerTest() : l3_admit_manager_(&p4_oid_mapper_, &publisher_)
    {
    }

    void SetUp() override
    {
        // Set up mock stuff for SAI l3 admit API structure.
        mock_sai_my_mac = &mock_sai_my_mac_;
        sai_my_mac_api->create_my_mac = mock_create_my_mac;
        sai_my_mac_api->remove_my_mac = mock_remove_my_mac;
    }

    void Enqueue(const swss::KeyOpFieldsValuesTuple &entry)
    {
        l3_admit_manager_.enqueue(APP_P4RT_L3_ADMIT_TABLE_NAME, entry);
    }

    void Drain()
    {
        l3_admit_manager_.drain();
    }

    std::string VerifyState(const std::string &key, const std::vector<swss::FieldValueTuple> &tuple)
    {
        return l3_admit_manager_.verifyState(key, tuple);
    }

    ReturnCode ProcessAddRequest(const P4L3AdmitAppDbEntry &app_db_entry, const std::string &l3_admit_key)
    {
        return l3_admit_manager_.processAddRequest(app_db_entry, l3_admit_key);
    }

    ReturnCode ProcessDeleteRequest(const std::string &my_mac_key)
    {
        return l3_admit_manager_.processDeleteRequest(my_mac_key);
    }

    P4L3AdmitEntry *GetL3AdmitEntry(const std::string &my_mac_key)
    {
        return l3_admit_manager_.getL3AdmitEntry(my_mac_key);
    }

    ReturnCodeOr<P4L3AdmitAppDbEntry> DeserializeP4L3AdmitAppDbEntry(
        const std::string &key, const std::vector<swss::FieldValueTuple> &attributes)
    {
        return l3_admit_manager_.deserializeP4L3AdmitAppDbEntry(key, attributes);
    }

    // Adds the l3 admit entry -- kP4L3AdmitAppDbEntry1, via l3 admit manager's
    // ProcessAddRequest (). This function also takes care of all the dependencies
    // of the l3 admit entry.
    // Returns a valid pointer to l3 admit entry on success.
    P4L3AdmitEntry *AddL3AdmitEntry1();

    // Validates that a P4 App l3 admit entry is correctly added in l3 admit
    // manager and centralized mapper. Returns true on success.
    bool ValidateL3AdmitEntryAdd(const P4L3AdmitAppDbEntry &app_db_entry);

    // Return true if the specified the object has the expected number of
    // reference.
    bool ValidateRefCnt(sai_object_type_t object_type, const std::string &key, uint32_t expected_ref_count)
    {
        uint32_t ref_count;
        if (!p4_oid_mapper_.getRefCount(object_type, key, &ref_count))
            return false;
        return ref_count == expected_ref_count;
    }

    StrictMock<MockSaiMyMac> mock_sai_my_mac_;
    MockResponsePublisher publisher_;
    P4OidMapper p4_oid_mapper_;
    L3AdmitManager l3_admit_manager_;
};

P4L3AdmitEntry *L3AdmitManagerTest::AddL3AdmitEntry1()
{
    const auto l3admit_key =
        KeyGenerator::generateL3AdmitKey(kP4L3AdmitAppDbEntry1.mac_address_data, kP4L3AdmitAppDbEntry1.mac_address_mask,
                                         kP4L3AdmitAppDbEntry1.port_name, kP4L3AdmitAppDbEntry1.priority);

    // Set up mock call.
    EXPECT_CALL(
        mock_sai_my_mac_,
        create_my_mac(::testing::NotNull(), Eq(gSwitchId), Eq(3),
                      Truly(std::bind(MatchCreateL3AdmitArgAttrList, std::placeholders::_1,
                                      CreateAttributeListForL3AdmitObject(kP4L3AdmitAppDbEntry1, SAI_NULL_OBJECT_ID)))))
        .WillOnce(DoAll(SetArgPointee<0>(kL3AdmitOid1), Return(SAI_STATUS_SUCCESS)));

    EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS, ProcessAddRequest(kP4L3AdmitAppDbEntry1, l3admit_key));

    return GetL3AdmitEntry(l3admit_key);
}

bool L3AdmitManagerTest::ValidateL3AdmitEntryAdd(const P4L3AdmitAppDbEntry &app_db_entry)
{
    const auto *p4_l3_admit_entry = GetL3AdmitEntry(KeyGenerator::generateL3AdmitKey(
        app_db_entry.mac_address_data, app_db_entry.mac_address_mask, app_db_entry.port_name, app_db_entry.priority));
    if (p4_l3_admit_entry == nullptr || p4_l3_admit_entry->mac_address_data != app_db_entry.mac_address_data ||
        p4_l3_admit_entry->mac_address_mask != app_db_entry.mac_address_mask ||
        p4_l3_admit_entry->port_name != app_db_entry.port_name || p4_l3_admit_entry->priority != app_db_entry.priority)
    {
        return false;
    }

    return true;
}

TEST_F(L3AdmitManagerTest, ProcessAddRequestShouldSucceedAddingNewL3Admit)
{
    AddL3AdmitEntry1();
    EXPECT_TRUE(ValidateL3AdmitEntryAdd(kP4L3AdmitAppDbEntry1));
}

TEST_F(L3AdmitManagerTest, ProcessAddRequestShouldFailWhenL3AdmitExistInCentralMapper)
{
    const auto l3admit_key =
        KeyGenerator::generateL3AdmitKey(kP4L3AdmitAppDbEntry1.mac_address_data, kP4L3AdmitAppDbEntry1.mac_address_mask,
                                         kP4L3AdmitAppDbEntry1.port_name, kP4L3AdmitAppDbEntry1.priority);
    ASSERT_EQ(l3admit_key, "match/dst_mac=00:02:03:04:00:00&ff:ff:ff:ff:00:00:priority=2030");
    ASSERT_TRUE(p4_oid_mapper_.setOID(SAI_OBJECT_TYPE_MY_MAC, l3admit_key, kL3AdmitOid1));
    // TODO: Expect critical state.
    EXPECT_EQ(StatusCode::SWSS_RC_INTERNAL, ProcessAddRequest(kP4L3AdmitAppDbEntry1, l3admit_key));
}

TEST_F(L3AdmitManagerTest, ProcessAddRequestShouldFailWhenDependingPortIsNotPresent)
{
    const P4L3AdmitAppDbEntry kAppDbEntry{/*port_name=*/"Ethernet100",
                                          /*mac_address_data=*/swss::MacAddress("00:02:03:04:00:00"),
                                          /*mac_address_mask=*/swss::MacAddress("ff:ff:ff:ff:00:00"),
                                          /*priority=*/2030};
    const auto l3admit_key = KeyGenerator::generateL3AdmitKey(
        kAppDbEntry.mac_address_data, kAppDbEntry.mac_address_mask, kAppDbEntry.port_name, kAppDbEntry.priority);

    EXPECT_EQ(StatusCode::SWSS_RC_NOT_FOUND, ProcessAddRequest(kAppDbEntry, l3admit_key));

    EXPECT_EQ(GetL3AdmitEntry(l3admit_key), nullptr);
}

TEST_F(L3AdmitManagerTest, ProcessAddRequestShouldFailWhenSaiCallFails)
{
    const auto l3admit_key =
        KeyGenerator::generateL3AdmitKey(kP4L3AdmitAppDbEntry1.mac_address_data, kP4L3AdmitAppDbEntry1.mac_address_mask,
                                         kP4L3AdmitAppDbEntry1.port_name, kP4L3AdmitAppDbEntry1.priority);
    // Set up mock call.
    EXPECT_CALL(
        mock_sai_my_mac_,
        create_my_mac(::testing::NotNull(), Eq(gSwitchId), Eq(3),
                      Truly(std::bind(MatchCreateL3AdmitArgAttrList, std::placeholders::_1,
                                      CreateAttributeListForL3AdmitObject(kP4L3AdmitAppDbEntry1, SAI_NULL_OBJECT_ID)))))
        .WillOnce(Return(SAI_STATUS_FAILURE));

    EXPECT_EQ(StatusCode::SWSS_RC_UNKNOWN, ProcessAddRequest(kP4L3AdmitAppDbEntry1, l3admit_key));

    // The add request failed for the l3 admit entry.
    EXPECT_EQ(GetL3AdmitEntry(l3admit_key), nullptr);
}

TEST_F(L3AdmitManagerTest, ProcessDeleteRequestShouldFailForNonExistingL3Admit)
{
    const auto l3admit_key =
        KeyGenerator::generateL3AdmitKey(kP4L3AdmitAppDbEntry1.mac_address_data, kP4L3AdmitAppDbEntry1.mac_address_mask,
                                         kP4L3AdmitAppDbEntry1.port_name, kP4L3AdmitAppDbEntry1.priority);
    EXPECT_EQ(StatusCode::SWSS_RC_NOT_FOUND, ProcessDeleteRequest(l3admit_key));
}

TEST_F(L3AdmitManagerTest, ProcessDeleteRequestShouldFailIfL3AdmitEntryIsAbsentInCentralMapper)
{
    auto *p4_my_mac_entry = AddL3AdmitEntry1();
    ASSERT_NE(p4_my_mac_entry, nullptr);

    const auto l3admit_key =
        KeyGenerator::generateL3AdmitKey(kP4L3AdmitAppDbEntry1.mac_address_data, kP4L3AdmitAppDbEntry1.mac_address_mask,
                                         kP4L3AdmitAppDbEntry1.port_name, kP4L3AdmitAppDbEntry1.priority);

    ASSERT_TRUE(p4_oid_mapper_.eraseOID(SAI_OBJECT_TYPE_MY_MAC, l3admit_key));

    // TODO: Expect critical state.
    EXPECT_EQ(StatusCode::SWSS_RC_INTERNAL, ProcessDeleteRequest(l3admit_key));

    // Validate the l3 admit entry is not deleted in P4 l3 admit manager.
    p4_my_mac_entry = GetL3AdmitEntry(l3admit_key);
    ASSERT_NE(p4_my_mac_entry, nullptr);
}

TEST_F(L3AdmitManagerTest, ProcessDeleteRequestShouldFailIfL3AdmitEntryIsStillReferenced)
{
    auto *p4_my_mac_entry = AddL3AdmitEntry1();
    ASSERT_NE(p4_my_mac_entry, nullptr);

    const auto l3admit_key =
        KeyGenerator::generateL3AdmitKey(kP4L3AdmitAppDbEntry1.mac_address_data, kP4L3AdmitAppDbEntry1.mac_address_mask,
                                         kP4L3AdmitAppDbEntry1.port_name, kP4L3AdmitAppDbEntry1.priority);
    ASSERT_TRUE(p4_oid_mapper_.increaseRefCount(SAI_OBJECT_TYPE_MY_MAC, l3admit_key));

    EXPECT_EQ(StatusCode::SWSS_RC_INVALID_PARAM, ProcessDeleteRequest(l3admit_key));

    // Validate the l3 admit entry is not deleted in either P4 l3 admit manager
    // or central mapper.
    p4_my_mac_entry = GetL3AdmitEntry(l3admit_key);
    ASSERT_NE(p4_my_mac_entry, nullptr);
    EXPECT_TRUE(ValidateRefCnt(SAI_OBJECT_TYPE_MY_MAC, l3admit_key, 1));
}

TEST_F(L3AdmitManagerTest, ProcessDeleteRequestShouldFailIfSaiCallFails)
{
    auto *p4_my_mac_entry = AddL3AdmitEntry1();
    ASSERT_NE(p4_my_mac_entry, nullptr);

    const auto l3admit_key =
        KeyGenerator::generateL3AdmitKey(kP4L3AdmitAppDbEntry1.mac_address_data, kP4L3AdmitAppDbEntry1.mac_address_mask,
                                         kP4L3AdmitAppDbEntry1.port_name, kP4L3AdmitAppDbEntry1.priority);

    // Set up mock call.
    EXPECT_CALL(mock_sai_my_mac_, remove_my_mac(Eq(p4_my_mac_entry->l3_admit_oid)))
        .WillOnce(Return(SAI_STATUS_FAILURE));

    EXPECT_EQ(StatusCode::SWSS_RC_UNKNOWN, ProcessDeleteRequest(l3admit_key));

    // Validate the l3 admit entry is not deleted in either P4 l3 admit manager
    // or central mapper.
    p4_my_mac_entry = GetL3AdmitEntry(l3admit_key);
    ASSERT_NE(p4_my_mac_entry, nullptr);
    EXPECT_TRUE(p4_oid_mapper_.existsOID(SAI_OBJECT_TYPE_MY_MAC, l3admit_key));
}

TEST_F(L3AdmitManagerTest, GetL3AdmitEntryShouldReturnNullPointerForNonexistingL3Admit)
{
    const auto l3admit_key =
        KeyGenerator::generateL3AdmitKey(kP4L3AdmitAppDbEntry1.mac_address_data, kP4L3AdmitAppDbEntry1.mac_address_mask,
                                         kP4L3AdmitAppDbEntry1.port_name, kP4L3AdmitAppDbEntry1.priority);
    EXPECT_EQ(GetL3AdmitEntry(l3admit_key), nullptr);
}

TEST_F(L3AdmitManagerTest, DeserializeP4L3AdmitAppDbEntryShouldReturnNullPointerForInvalidAction)
{
    std::vector<swss::FieldValueTuple> attributes = {
        swss::FieldValueTuple(p4orch::kAction, "set_nexthop")}; // Invalid action.

    EXPECT_FALSE(DeserializeP4L3AdmitAppDbEntry(kL3AdmitP4AppDbKey1, attributes).ok());
}

TEST_F(L3AdmitManagerTest, DeserializeP4L3AdmitAppDbEntryShouldReturnNullPointerForInvalidField)
{
    std::vector<swss::FieldValueTuple> attributes = {swss::FieldValueTuple(p4orch::kAction, p4orch::kL3AdmitAction),
                                                     swss::FieldValueTuple("UNKNOWN_FIELD", "UNKOWN")};

    EXPECT_FALSE(DeserializeP4L3AdmitAppDbEntry(kL3AdmitP4AppDbKey1, attributes).ok());
}

TEST_F(L3AdmitManagerTest, DeserializeP4L3AdmitAppDbEntryShouldReturnNullPointerForInvalidMac)
{
    std::vector<swss::FieldValueTuple> attributes = {swss::FieldValueTuple(p4orch::kAction, p4orch::kL3AdmitAction)};
    constexpr char *kValidAppDbKey = R"({"match/dst_mac":"00:02:03:04:00:00","priority":2030})";
    EXPECT_TRUE(DeserializeP4L3AdmitAppDbEntry(kValidAppDbKey, attributes).ok());
    constexpr char *kInvalidAppDbKey = R"({"match/dst_mac":"123.123.123.123","priority":2030})";
    EXPECT_FALSE(DeserializeP4L3AdmitAppDbEntry(kInvalidAppDbKey, attributes).ok());
}

TEST_F(L3AdmitManagerTest, DeserializeP4L3AdmitAppDbEntryShouldReturnNullPointerForInvalidPriority)
{
    std::vector<swss::FieldValueTuple> attributes = {swss::FieldValueTuple(p4orch::kAction, p4orch::kL3AdmitAction)};
    constexpr char *kInvalidAppDbKey = R"({"match/dst_mac":"00:02:03:04:00:00","priority":-1})";
    EXPECT_FALSE(DeserializeP4L3AdmitAppDbEntry(kInvalidAppDbKey, attributes).ok());
}

TEST_F(L3AdmitManagerTest, DeserializeP4L3AdmitAppDbEntryShouldSucceedWithoutDstMac)
{
    std::vector<swss::FieldValueTuple> attributes = {swss::FieldValueTuple(p4orch::kAction, p4orch::kL3AdmitAction)};
    constexpr char *kValidAppDbKey = R"({"priority":1})";
    EXPECT_TRUE(DeserializeP4L3AdmitAppDbEntry(kValidAppDbKey, attributes).ok());
}

TEST_F(L3AdmitManagerTest, DrainDuplicateSetRequestShouldSucceed)
{
    auto *p4_my_mac_entry = AddL3AdmitEntry1();
    ASSERT_NE(p4_my_mac_entry, nullptr);

    nlohmann::json j;
    j[prependMatchField(p4orch::kDstMac)] = kP4L3AdmitAppDbEntry1.mac_address_data.to_string() +
                                            p4orch::kDataMaskDelimiter +
                                            kP4L3AdmitAppDbEntry1.mac_address_mask.to_string();
    j[p4orch::kPriority] = kP4L3AdmitAppDbEntry1.priority;

    std::vector<swss::FieldValueTuple> fvs{{p4orch::kAction, p4orch::kL3AdmitAction}};

    swss::KeyOpFieldsValuesTuple app_db_entry(std::string(APP_P4RT_L3_ADMIT_TABLE_NAME) + kTableKeyDelimiter + j.dump(),
                                              SET_COMMAND, fvs);

    Enqueue(app_db_entry);
    Drain();

    // Expect that the update call will fail, so l3 admit entry's fields stay
    // the same.
    EXPECT_TRUE(ValidateL3AdmitEntryAdd(kP4L3AdmitAppDbEntry1));
}

TEST_F(L3AdmitManagerTest, DrainDeleteRequestShouldSucceedForExistingL3Admit)
{
    auto *p4_my_mac_entry = AddL3AdmitEntry1();
    ASSERT_NE(p4_my_mac_entry, nullptr);

    nlohmann::json j;
    j[prependMatchField(p4orch::kDstMac)] = kP4L3AdmitAppDbEntry1.mac_address_data.to_string() +
                                            p4orch::kDataMaskDelimiter +
                                            kP4L3AdmitAppDbEntry1.mac_address_mask.to_string();
    j[p4orch::kPriority] = kP4L3AdmitAppDbEntry1.priority;

    std::vector<swss::FieldValueTuple> fvs;
    swss::KeyOpFieldsValuesTuple app_db_entry(std::string(APP_P4RT_NEXTHOP_TABLE_NAME) + kTableKeyDelimiter + j.dump(),
                                              DEL_COMMAND, fvs);
    EXPECT_CALL(mock_sai_my_mac_, remove_my_mac(Eq(p4_my_mac_entry->l3_admit_oid)))
        .WillOnce(Return(SAI_STATUS_SUCCESS));

    Enqueue(app_db_entry);
    Drain();

    // Validate the l3 admit entry has been deleted in both P4 l3 admit
    // manager
    // and centralized mapper.
    const auto l3admit_key =
        KeyGenerator::generateL3AdmitKey(kP4L3AdmitAppDbEntry1.mac_address_data, kP4L3AdmitAppDbEntry1.mac_address_mask,
                                         kP4L3AdmitAppDbEntry1.port_name, kP4L3AdmitAppDbEntry1.priority);
    p4_my_mac_entry = GetL3AdmitEntry(l3admit_key);
    EXPECT_EQ(p4_my_mac_entry, nullptr);
    EXPECT_FALSE(p4_oid_mapper_.existsOID(SAI_OBJECT_TYPE_MY_MAC, l3admit_key));
}

TEST_F(L3AdmitManagerTest, DrainValidAppEntryShouldSucceed)
{
    nlohmann::json j;
    j[prependMatchField(p4orch::kDstMac)] = kP4L3AdmitAppDbEntry2.mac_address_data.to_string() +
                                            p4orch::kDataMaskDelimiter +
                                            kP4L3AdmitAppDbEntry2.mac_address_mask.to_string();
    j[p4orch::kPriority] = kP4L3AdmitAppDbEntry2.priority;
    j[prependMatchField(p4orch::kInPort)] = kP4L3AdmitAppDbEntry2.port_name;

    std::vector<swss::FieldValueTuple> fvs{{p4orch::kAction, p4orch::kL3AdmitAction}};

    swss::KeyOpFieldsValuesTuple app_db_entry(std::string(APP_P4RT_L3_ADMIT_TABLE_NAME) + kTableKeyDelimiter + j.dump(),
                                              SET_COMMAND, fvs);

    Enqueue(app_db_entry);
    EXPECT_CALL(mock_sai_my_mac_, create_my_mac(_, _, _, _))
        .WillOnce(DoAll(SetArgPointee<0>(kL3AdmitOid2), Return(SAI_STATUS_SUCCESS)));

    Drain();

    EXPECT_TRUE(ValidateL3AdmitEntryAdd(kP4L3AdmitAppDbEntry2));
}

TEST_F(L3AdmitManagerTest, DrainInValidAppEntryShouldSucceed)
{
    nlohmann::json j;
    j[prependMatchField(p4orch::kDstMac)] = "1"; // Invalid Mac
    j[p4orch::kPriority] = 1000;

    std::vector<swss::FieldValueTuple> fvs{{p4orch::kAction, p4orch::kL3AdmitAction}};

    swss::KeyOpFieldsValuesTuple app_db_entry(std::string(APP_P4RT_L3_ADMIT_TABLE_NAME) + kTableKeyDelimiter + j.dump(),
                                              SET_COMMAND, fvs);

    Enqueue(app_db_entry);

    Drain();
    constexpr char *kL3AdmitKey = R"({"match/dst_mac":"1","priority":1000})";
    EXPECT_EQ(GetL3AdmitEntry(kL3AdmitKey), nullptr);
    EXPECT_FALSE(p4_oid_mapper_.existsOID(SAI_OBJECT_TYPE_MY_MAC, kL3AdmitKey));
}

TEST_F(L3AdmitManagerTest, VerifyStateTest)
{
    auto *p4_my_mac_entry = AddL3AdmitEntry1();
    ASSERT_NE(p4_my_mac_entry, nullptr);
    nlohmann::json j;
    j[prependMatchField(p4orch::kDstMac)] = kP4L3AdmitAppDbEntry1.mac_address_data.to_string() +
                                            p4orch::kDataMaskDelimiter +
                                            kP4L3AdmitAppDbEntry1.mac_address_mask.to_string();
    j[p4orch::kPriority] = kP4L3AdmitAppDbEntry1.priority;
    const std::string db_key = std::string(APP_P4RT_TABLE_NAME) + kTableKeyDelimiter + APP_P4RT_L3_ADMIT_TABLE_NAME +
                               kTableKeyDelimiter + j.dump();
    std::vector<swss::FieldValueTuple> attributes;

    // Verification should succeed with vaild key and value.
    attributes.push_back(swss::FieldValueTuple{p4orch::kAction, p4orch::kL3AdmitAction});

    // Setup ASIC DB.
    swss::Table table(nullptr, "ASIC_STATE");
    table.set(
        "SAI_OBJECT_TYPE_MY_MAC:oid:0x1",
        std::vector<swss::FieldValueTuple>{
            swss::FieldValueTuple{"SAI_MY_MAC_ATTR_MAC_ADDRESS", kP4L3AdmitAppDbEntry1.mac_address_data.to_string()},
            swss::FieldValueTuple{"SAI_MY_MAC_ATTR_MAC_ADDRESS_MASK", "FF:FF:FF:FF:00:00"},
            swss::FieldValueTuple{"SAI_MY_MAC_ATTR_PRIORITY", "2030"}});
    EXPECT_EQ(VerifyState(db_key, attributes), "");

    // Invalid key should fail verification.
    EXPECT_FALSE(VerifyState("invalid", attributes).empty());
    EXPECT_FALSE(VerifyState("invalid:invalid", attributes).empty());
    EXPECT_FALSE(VerifyState(std::string(APP_P4RT_TABLE_NAME) + ":invalid", attributes).empty());
    EXPECT_FALSE(VerifyState(std::string(APP_P4RT_TABLE_NAME) + ":invalid:invalid", attributes).empty());
    EXPECT_FALSE(VerifyState(std::string(APP_P4RT_TABLE_NAME) + ":FIXED_L3_ADMIT_TABLE:invalid", attributes).empty());

    // Verification should fail if MAC does not exist.
    j[prependMatchField(p4orch::kDstMac)] = kP4L3AdmitAppDbEntry2.mac_address_data.to_string() +
                                            p4orch::kDataMaskDelimiter +
                                            kP4L3AdmitAppDbEntry2.mac_address_mask.to_string();
    j[p4orch::kPriority] = kP4L3AdmitAppDbEntry1.priority;
    EXPECT_FALSE(VerifyState(std::string(APP_P4RT_TABLE_NAME) + kTableKeyDelimiter + APP_P4RT_L3_ADMIT_TABLE_NAME +
                                 kTableKeyDelimiter + j.dump(),
                             attributes)
                     .empty());

    // Verification should fail if port name mismatches.
    auto saved_port_name = p4_my_mac_entry->port_name;
    p4_my_mac_entry->port_name = kP4L3AdmitAppDbEntry2.port_name;
    EXPECT_FALSE(VerifyState(db_key, attributes).empty());
    p4_my_mac_entry->port_name = saved_port_name;

    // Verification should fail if MAC mismatches.
    auto saved_mac_address_data = p4_my_mac_entry->mac_address_data;
    p4_my_mac_entry->mac_address_data = kP4L3AdmitAppDbEntry2.mac_address_data;
    EXPECT_FALSE(VerifyState(db_key, attributes).empty());
    p4_my_mac_entry->mac_address_data = saved_mac_address_data;

    // Verification should fail if MAC mask mismatches.
    auto saved_mac_address_mask = p4_my_mac_entry->mac_address_mask;
    p4_my_mac_entry->mac_address_mask = kP4L3AdmitAppDbEntry2.mac_address_mask;
    EXPECT_FALSE(VerifyState(db_key, attributes).empty());
    p4_my_mac_entry->mac_address_mask = saved_mac_address_mask;

    // Verification should fail if priority mismatches.
    auto saved_priority = p4_my_mac_entry->priority;
    p4_my_mac_entry->priority = 1111;
    EXPECT_FALSE(VerifyState(db_key, attributes).empty());
    p4_my_mac_entry->priority = saved_priority;

    // Verification should fail if OID mapper mismatches.
    const auto l3admit_key =
        KeyGenerator::generateL3AdmitKey(kP4L3AdmitAppDbEntry1.mac_address_data, kP4L3AdmitAppDbEntry1.mac_address_mask,
                                         kP4L3AdmitAppDbEntry1.port_name, kP4L3AdmitAppDbEntry1.priority);
    p4_oid_mapper_.eraseOID(SAI_OBJECT_TYPE_MY_MAC, l3admit_key);
    EXPECT_FALSE(VerifyState(db_key, attributes).empty());
    p4_oid_mapper_.setOID(SAI_OBJECT_TYPE_MY_MAC, l3admit_key, kL3AdmitOid1);
}

TEST_F(L3AdmitManagerTest, VerifyStateAsicDbTest)
{
    auto *p4_my_mac_entry = AddL3AdmitEntry1();
    ASSERT_NE(p4_my_mac_entry, nullptr);
    nlohmann::json j;
    j[prependMatchField(p4orch::kDstMac)] = kP4L3AdmitAppDbEntry1.mac_address_data.to_string() +
                                            p4orch::kDataMaskDelimiter +
                                            kP4L3AdmitAppDbEntry1.mac_address_mask.to_string();
    j[p4orch::kPriority] = kP4L3AdmitAppDbEntry1.priority;
    const std::string db_key = std::string(APP_P4RT_TABLE_NAME) + kTableKeyDelimiter + APP_P4RT_L3_ADMIT_TABLE_NAME +
                               kTableKeyDelimiter + j.dump();
    std::vector<swss::FieldValueTuple> attributes;

    // Setup ASIC DB.
    swss::Table table(nullptr, "ASIC_STATE");
    table.set(
        "SAI_OBJECT_TYPE_MY_MAC:oid:0x1",
        std::vector<swss::FieldValueTuple>{
            swss::FieldValueTuple{"SAI_MY_MAC_ATTR_MAC_ADDRESS", kP4L3AdmitAppDbEntry1.mac_address_data.to_string()},
            swss::FieldValueTuple{"SAI_MY_MAC_ATTR_MAC_ADDRESS_MASK", "FF:FF:FF:FF:00:00"},
            swss::FieldValueTuple{"SAI_MY_MAC_ATTR_PRIORITY", "2030"}});

    // Verification should succeed with vaild key and value.
    attributes.push_back(swss::FieldValueTuple{p4orch::kAction, p4orch::kL3AdmitAction});

    EXPECT_EQ(VerifyState(db_key, attributes), "");

    // Verification should fail if ASIC DB values mismatch.
    table.set("SAI_OBJECT_TYPE_MY_MAC:oid:0x1",
              std::vector<swss::FieldValueTuple>{swss::FieldValueTuple{"SAI_MY_MAC_ATTR_PRIORITY", "1000"}});
    EXPECT_FALSE(VerifyState(db_key, attributes).empty());

    // Verification should fail if ASIC DB table is missing.
    table.del("SAI_OBJECT_TYPE_MY_MAC:oid:0x1");
    EXPECT_FALSE(VerifyState(db_key, attributes).empty());
    table.set(
        "SAI_OBJECT_TYPE_MY_MAC:oid:0x1",
        std::vector<swss::FieldValueTuple>{
            swss::FieldValueTuple{"SAI_MY_MAC_ATTR_MAC_ADDRESS", kP4L3AdmitAppDbEntry1.mac_address_data.to_string()},
            swss::FieldValueTuple{"SAI_MY_MAC_ATTR_MAC_ADDRESS_MASK", "FF:FF:FF:FF:00:00"},
            swss::FieldValueTuple{"SAI_MY_MAC_ATTR_PRIORITY", "2030"}});
}
