#include "neighbor_manager.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <nlohmann/json.hpp>
#include <string>
#include <unordered_set>

#include "mock_response_publisher.h"
#include "mock_sai_neighbor.h"
#include "p4orch.h"
#include "p4orch/p4orch_util.h"
#include "return_code.h"
#include "swssnet.h"

using ::p4orch::kTableKeyDelimiter;

using ::testing::_;
using ::testing::DoAll;
using ::testing::Eq;
using ::testing::Return;
using ::testing::SetArrayArgument;
using ::testing::StrictMock;
using ::testing::Truly;

extern sai_object_id_t gSwitchId;
extern sai_neighbor_api_t *sai_neighbor_api;

namespace
{

constexpr char *kRouterInterfaceId1 = "intf-3/4";
constexpr sai_object_id_t kRouterInterfaceOid1 = 0x295100;

constexpr char *kRouterInterfaceId2 = "Ethernet20";
constexpr sai_object_id_t kRouterInterfaceOid2 = 0x51411;

constexpr char* kRouterInterfaceId3 = "Ethernet21";
constexpr sai_object_id_t kRouterInterfaceOid3 = 0x51412;

const swss::IpAddress kNeighborId1("10.0.0.22");
const swss::MacAddress kMacAddress1("00:01:02:03:04:05");

const swss::IpAddress kNeighborId2("fe80::21a:11ff:fe17:5f80");
const swss::MacAddress kMacAddress2("00:ff:ee:dd:cc:bb");

bool MatchNeighborEntry(const sai_neighbor_entry_t& neigh_entry,
                        const sai_neighbor_entry_t& expected_neigh_entry)
{
    if ((neigh_entry.switch_id != expected_neigh_entry.switch_id) ||
        (neigh_entry.rif_id != expected_neigh_entry.rif_id) ||
        (neigh_entry.ip_address.addr_family !=
             expected_neigh_entry.ip_address.addr_family))
          return false;

    if ((neigh_entry.ip_address.addr_family == SAI_IP_ADDR_FAMILY_IPV4) &&
        (neigh_entry.ip_address.addr.ip4 != expected_neigh_entry.ip_address.addr.ip4))
    {
        return false;
    }
    else if ((neigh_entry.ip_address.addr_family == SAI_IP_ADDR_FAMILY_IPV6) &&
             (memcmp(neigh_entry.ip_address.addr.ip6, expected_neigh_entry.ip_address.addr.ip6, 16)))
    {
        return false;
    }

    return true;
}

bool MatchSaiAttribute(const sai_attribute_t& attr,
                       const sai_attribute_t& exp_attr) {
  if (exp_attr.id == SAI_NEIGHBOR_ENTRY_ATTR_DST_MAC_ADDRESS) {
    if (attr.id != SAI_NEIGHBOR_ENTRY_ATTR_DST_MAC_ADDRESS ||
        memcmp(attr.value.mac, exp_attr.value.mac, sizeof(sai_mac_t))) {
      return false;
    }
  }
  if (exp_attr.id == SAI_NEIGHBOR_ENTRY_ATTR_NO_HOST_ROUTE) {
    if (attr.id != SAI_NEIGHBOR_ENTRY_ATTR_NO_HOST_ROUTE ||
        attr.value.booldata != exp_attr.value.booldata) {
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

MATCHER_P(EntryArrayEq, array, "") {
  for (size_t i = 0; i < array.size(); ++i) {
    if (!MatchNeighborEntry(arg[i], array[i])) {
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

MATCHER_P(AttrArrayArrayEq, array, "") {
  for (size_t i = 0; i < array.size(); ++i) {
    for (size_t j = 0; j < array[i].size(); j++) {
      if (!MatchSaiAttribute(arg[i][j], array[i][j])) {
        return false;
      }
    }
  }
  return true;
}

}   // namespace

class NeighborManagerTest : public ::testing::Test
{
  protected:
    NeighborManagerTest() : neighbor_manager_(&p4_oid_mapper_, &publisher_)
    {
    }

    void SetUp() override
    {
        mock_sai_neighbor = &mock_sai_neighbor_;
        sai_neighbor_api->create_neighbor_entry = mock_create_neighbor_entry;
        sai_neighbor_api->remove_neighbor_entry = mock_remove_neighbor_entry;
        sai_neighbor_api->create_neighbor_entries = mock_create_neighbor_entries;
        sai_neighbor_api->remove_neighbor_entries = mock_remove_neighbor_entries;
        sai_neighbor_api->set_neighbor_entry_attribute = mock_set_neighbor_entry_attribute;
        sai_neighbor_api->get_neighbor_entry_attribute = mock_get_neighbor_entry_attribute;
	sai_neighbor_api->create_neighbor_entries = mock_create_neighbor_entries;
        sai_neighbor_api->remove_neighbor_entries = mock_remove_neighbor_entries;
        sai_neighbor_api->set_neighbor_entries_attribute =
            mock_set_neighbor_entries_attribute;
        sai_neighbor_api->get_neighbor_entries_attribute =
            mock_get_neighbor_entries_attribute;
    }

    void Enqueue(const swss::KeyOpFieldsValuesTuple &entry)
    {
        neighbor_manager_.enqueue(APP_P4RT_NEIGHBOR_TABLE_NAME, entry);
    }

    ReturnCode Drain(bool failure_before) {
      if (failure_before) {
        neighbor_manager_.drainWithNotExecuted();
        return ReturnCode(StatusCode::SWSS_RC_NOT_EXECUTED);
      }
      return neighbor_manager_.drain();
    }

    std::string VerifyState(const std::string &key, const std::vector<swss::FieldValueTuple> &tuple)
    {
        return neighbor_manager_.verifyState(key, tuple);
    }

    ReturnCodeOr<P4NeighborAppDbEntry> DeserializeNeighborEntry(const std::string &key,
                                                                const std::vector<swss::FieldValueTuple> &attributes)
    {
        return neighbor_manager_.deserializeNeighborEntry(key, attributes);
    }

    ReturnCode ValidateNeighborEntryOperation(
      const P4NeighborAppDbEntry& app_db_entry, const std::string& operation) {
    	return neighbor_manager_.validateNeighborEntryOperation(app_db_entry,
        operation);
    }

    std::vector<ReturnCode> CreateNeighbors(
      const std::vector<P4NeighborAppDbEntry>& neighbor_entries) {
        return neighbor_manager_.createNeighbors(neighbor_entries);
    }

    std::vector<ReturnCode> RemoveNeighbors(
      const std::vector<P4NeighborAppDbEntry>& neighbor_entries) {
         return neighbor_manager_.removeNeighbors(neighbor_entries);
    }

    std::vector<ReturnCode> UpdateNeighbors(
      const std::vector<P4NeighborAppDbEntry>& neighbor_entries) {
         return neighbor_manager_.updateNeighbors(neighbor_entries);
    }

    ReturnCode ProcessEntries(
      const std::vector<P4NeighborAppDbEntry>& entries,
      const std::vector<swss::KeyOpFieldsValuesTuple>& tuple_list,
      const std::string& op, bool update) {
    	return neighbor_manager_.processEntries(entries, tuple_list, op, update);
    }

    P4NeighborEntry *GetNeighborEntry(const std::string &neighbor_key)
    {
        return neighbor_manager_.getNeighborEntry(neighbor_key);
    }

    void ValidateNeighborEntry(const P4NeighborEntry &expected_entry, const uint32_t router_intf_ref_count)
    {
        auto neighbor_entry = GetNeighborEntry(expected_entry.neighbor_key);

        EXPECT_NE(nullptr, neighbor_entry);
        EXPECT_EQ(expected_entry.router_intf_id, neighbor_entry->router_intf_id);
        EXPECT_EQ(expected_entry.neighbor_id, neighbor_entry->neighbor_id);
        EXPECT_EQ(expected_entry.dst_mac_address, neighbor_entry->dst_mac_address);
        EXPECT_EQ(expected_entry.router_intf_key, neighbor_entry->router_intf_key);
        EXPECT_EQ(expected_entry.neighbor_key, neighbor_entry->neighbor_key);

        EXPECT_TRUE(MatchNeighborEntry(neighbor_entry->neigh_entry, expected_entry.neigh_entry));

        EXPECT_TRUE(p4_oid_mapper_.existsOID(SAI_OBJECT_TYPE_NEIGHBOR_ENTRY, expected_entry.neighbor_key));

        uint32_t ref_count;
        ASSERT_TRUE(
            p4_oid_mapper_.getRefCount(SAI_OBJECT_TYPE_ROUTER_INTERFACE, expected_entry.router_intf_key, &ref_count));
        EXPECT_EQ(router_intf_ref_count, ref_count);
    }

    void ValidateNeighborEntryNotPresent(const P4NeighborEntry &neighbor_entry, bool check_ref_count,
                                         const uint32_t router_intf_ref_count = 0)
    {
        auto current_entry = GetNeighborEntry(neighbor_entry.neighbor_key);
        EXPECT_EQ(current_entry, nullptr);
        EXPECT_FALSE(p4_oid_mapper_.existsOID(SAI_OBJECT_TYPE_NEIGHBOR_ENTRY, neighbor_entry.neighbor_key));

        if (check_ref_count)
        {
            uint32_t ref_count;
            ASSERT_TRUE(p4_oid_mapper_.getRefCount(SAI_OBJECT_TYPE_ROUTER_INTERFACE, neighbor_entry.router_intf_key,
                                                   &ref_count));
            EXPECT_EQ(ref_count, router_intf_ref_count);
        }
    }

    void AddNeighborEntry(const P4NeighborAppDbEntry& neighbor_entry,
                        const sai_object_id_t router_intf_oid) {
      P4NeighborEntry entry(neighbor_entry.router_intf_id,
                            neighbor_entry.neighbor_id,
                            neighbor_entry.dst_mac_address);

      sai_neighbor_entry_t neigh_entry;
      neigh_entry.switch_id = gSwitchId;
      copy(neigh_entry.ip_address, entry.neighbor_id);
      neigh_entry.rif_id = router_intf_oid;

      std::vector<sai_attribute_t> attrs;
      sai_attribute_t attr;
      attr.id = SAI_NEIGHBOR_ENTRY_ATTR_DST_MAC_ADDRESS;
      memcpy(attr.value.mac, entry.dst_mac_address.getMac(), sizeof(sai_mac_t));
      attrs.push_back(attr);
      attr.id = SAI_NEIGHBOR_ENTRY_ATTR_NO_HOST_ROUTE;
      attr.value.booldata = true;
      attrs.push_back(attr);
  
      std::vector<sai_status_t> exp_status{SAI_STATUS_SUCCESS};
      EXPECT_CALL(
          mock_sai_neighbor_,
          create_neighbor_entries(
              Eq(1), EntryArrayEq(std::vector<sai_neighbor_entry_t>{neigh_entry}),
              ArrayEq(std::vector<uint32_t>{2}),
              AttrArrayArrayEq(std::vector<std::vector<sai_attribute_t>>{attrs}),
              Eq(SAI_BULK_OP_ERROR_MODE_STOP_ON_ERROR), _))
          .WillOnce(
              DoAll(SetArrayArgument<5>(exp_status.begin(), exp_status.end()),
                    Return(SAI_STATUS_SUCCESS)));
  
      ASSERT_TRUE(p4_oid_mapper_.setOID(SAI_OBJECT_TYPE_ROUTER_INTERFACE,
                                        entry.router_intf_key, router_intf_oid));
      EXPECT_THAT(
          CreateNeighbors(std::vector<P4NeighborAppDbEntry>{neighbor_entry}),
          ArrayEq(std::vector<StatusCode>{StatusCode::SWSS_RC_SUCCESS}));
    }

    std::string CreateNeighborAppDbKey(const std::string router_interface_id, const swss::IpAddress neighbor_id)
    {
        nlohmann::json j;
        j[prependMatchField(p4orch::kRouterInterfaceId)] = router_interface_id;
        j[prependMatchField(p4orch::kNeighborId)] = neighbor_id.to_string();
        return j.dump();
    }

    StrictMock<MockSaiNeighbor> mock_sai_neighbor_;
    StrictMock<MockResponsePublisher> publisher_;
    P4OidMapper p4_oid_mapper_;
    NeighborManager neighbor_manager_;
};

TEST_F(NeighborManagerTest, CreateNeighborValidAttributes) {
  const P4NeighborAppDbEntry app_db_entry = {
      .router_intf_id = kRouterInterfaceId1,
      .neighbor_id = kNeighborId1,
      .dst_mac_address = kMacAddress1,
      .is_set_dst_mac = true};
  P4NeighborEntry neighbor_entry(app_db_entry.router_intf_id,
                                 app_db_entry.neighbor_id,
                                 app_db_entry.dst_mac_address);
  neighbor_entry.neigh_entry.switch_id = gSwitchId;
  copy(neighbor_entry.neigh_entry.ip_address, neighbor_entry.neighbor_id);
  neighbor_entry.neigh_entry.rif_id = kRouterInterfaceOid1;
  AddNeighborEntry(app_db_entry, kRouterInterfaceOid1);
  ValidateNeighborEntry(neighbor_entry, /*router_intf_ref_count=*/1);
}

TEST_F(NeighborManagerTest, CreateNeighborSaiApiFails)
{
    const P4NeighborAppDbEntry app_db_entry = {
        .router_intf_id = kRouterInterfaceId1,
        .neighbor_id = kNeighborId1,
        .dst_mac_address = kMacAddress1,
        .is_set_dst_mac = true};
    P4NeighborEntry neighbor_entry(app_db_entry.router_intf_id,
                                   app_db_entry.neighbor_id,
                                   app_db_entry.dst_mac_address);

    ASSERT_TRUE(
        p4_oid_mapper_.setOID(SAI_OBJECT_TYPE_ROUTER_INTERFACE, neighbor_entry.router_intf_key, kRouterInterfaceOid1));

    std::vector<sai_status_t> exp_status{SAI_STATUS_FAILURE};
    EXPECT_CALL(mock_sai_neighbor_, create_neighbor_entries(_, _, _, _, _, _))
        .WillOnce(DoAll(SetArrayArgument<5>(exp_status.begin(), exp_status.end()),
            Return(SAI_STATUS_FAILURE)));

    EXPECT_THAT(CreateNeighbors(std::vector<P4NeighborAppDbEntry>{app_db_entry}),
       ArrayEq(std::vector<StatusCode>{StatusCode::SWSS_RC_UNKNOWN}));

    ValidateNeighborEntryNotPresent(neighbor_entry, /*check_ref_count=*/true);
}

TEST_F(NeighborManagerTest, RemoveNeighborExistingNeighborEntry)
{

    const P4NeighborAppDbEntry app_db_entry = {
        .router_intf_id = kRouterInterfaceId2,
        .neighbor_id = kNeighborId2,
        .dst_mac_address = kMacAddress2,
        .is_set_dst_mac = true};
    P4NeighborEntry neighbor_entry(app_db_entry.router_intf_id,
                                  app_db_entry.neighbor_id,
                                  app_db_entry.dst_mac_address);
    AddNeighborEntry(app_db_entry, kRouterInterfaceOid2);

    sai_neighbor_entry_t neigh_entry;
    neigh_entry.switch_id = gSwitchId;
    copy(neigh_entry.ip_address, neighbor_entry.neighbor_id);
    neigh_entry.rif_id = kRouterInterfaceOid2;

    std::vector<sai_status_t> exp_status{SAI_STATUS_SUCCESS};
    EXPECT_CALL(
      mock_sai_neighbor_,
      remove_neighbor_entries(
          Eq(1), EntryArrayEq(std::vector<sai_neighbor_entry_t>{neigh_entry}),
          Eq(SAI_BULK_OP_ERROR_MODE_STOP_ON_ERROR), _))
      .WillOnce(DoAll(SetArrayArgument<3>(exp_status.begin(), exp_status.end()),
           Return(SAI_STATUS_SUCCESS)));

    EXPECT_THAT(RemoveNeighbors(std::vector<P4NeighborAppDbEntry>{app_db_entry}),
      ArrayEq(std::vector<StatusCode>{StatusCode::SWSS_RC_SUCCESS}));

    ValidateNeighborEntryNotPresent(neighbor_entry, /*check_ref_count=*/true);
}

TEST_F(NeighborManagerTest, RemoveNeighborSaiApiFails)
{
    const P4NeighborAppDbEntry app_db_entry = {
        .router_intf_id = kRouterInterfaceId2,
        .neighbor_id = kNeighborId2,
        .dst_mac_address = kMacAddress2,
        .is_set_dst_mac = true};
    P4NeighborEntry neighbor_entry(app_db_entry.router_intf_id,
                                   app_db_entry.neighbor_id,
                                   app_db_entry.dst_mac_address);
    neighbor_entry.neigh_entry.switch_id = gSwitchId;
    copy(neighbor_entry.neigh_entry.ip_address, neighbor_entry.neighbor_id);
    neighbor_entry.neigh_entry.rif_id = kRouterInterfaceOid2;
    AddNeighborEntry(app_db_entry, kRouterInterfaceOid2);

    std::vector<sai_status_t> exp_status{SAI_STATUS_FAILURE};
    EXPECT_CALL(mock_sai_neighbor_, remove_neighbor_entries(_, _, _, _))
        .WillOnce(DoAll(SetArrayArgument<3>(exp_status.begin(), exp_status.end()),
                        Return(SAI_STATUS_FAILURE)));

    EXPECT_THAT(RemoveNeighbors(std::vector<P4NeighborAppDbEntry>{app_db_entry}),
                ArrayEq(std::vector<StatusCode>{StatusCode::SWSS_RC_UNKNOWN}));
    ValidateNeighborEntry(neighbor_entry, /*router_intf_ref_count=*/1);
}

TEST_F(NeighborManagerTest, SetDstMacAddressModifyMacAddress)
{
    const P4NeighborAppDbEntry app_db_entry = {
        .router_intf_id = kRouterInterfaceId2,
        .neighbor_id = kNeighborId2,
        .dst_mac_address = kMacAddress2,
        .is_set_dst_mac = true};
    P4NeighborEntry neighbor_entry(app_db_entry.router_intf_id,
                                   app_db_entry.neighbor_id,
                                   app_db_entry.dst_mac_address);
    AddNeighborEntry(app_db_entry, kRouterInterfaceOid2);

    sai_neighbor_entry_t neigh_entry;
    neigh_entry.switch_id = gSwitchId;
    copy(neigh_entry.ip_address, neighbor_entry.neighbor_id);
    neigh_entry.rif_id = kRouterInterfaceOid2;

    sai_attribute_t attr;
    attr.id = SAI_NEIGHBOR_ENTRY_ATTR_DST_MAC_ADDRESS;
    memcpy(attr.value.mac, kMacAddress1.getMac(), sizeof(sai_mac_t));
    std::vector<sai_status_t> exp_status{SAI_STATUS_SUCCESS};
    EXPECT_CALL(
      mock_sai_neighbor_,
      set_neighbor_entries_attribute(
          Eq(1), EntryArrayEq(std::vector<sai_neighbor_entry_t>{neigh_entry}),
          AttrArrayEq(std::vector<sai_attribute_t>{attr}),
          Eq(SAI_BULK_OP_ERROR_MODE_STOP_ON_ERROR), _))
      .WillOnce(DoAll(SetArrayArgument<4>(exp_status.begin(), exp_status.end()),
                      Return(SAI_STATUS_SUCCESS)));

    const P4NeighborAppDbEntry new_app_db_entry = {
        .router_intf_id = kRouterInterfaceId2,
        .neighbor_id = kNeighborId2,
        .dst_mac_address = kMacAddress1,
        .is_set_dst_mac = true};
    EXPECT_THAT(
        UpdateNeighbors(std::vector<P4NeighborAppDbEntry>{new_app_db_entry}),
        ArrayEq(std::vector<StatusCode>{StatusCode::SWSS_RC_SUCCESS}));

    auto current_entry = GetNeighborEntry(neighbor_entry.neighbor_key);
    ASSERT_NE(current_entry, nullptr);
    EXPECT_EQ(current_entry->dst_mac_address, kMacAddress1);
}

TEST_F(NeighborManagerTest, SetDstMacAddressIdempotent)
{
    const P4NeighborAppDbEntry app_db_entry = {
        .router_intf_id = kRouterInterfaceId2,
        .neighbor_id = kNeighborId2,
        .dst_mac_address = kMacAddress2,
        .is_set_dst_mac = true};
    P4NeighborEntry neighbor_entry(app_db_entry.router_intf_id,
                                   app_db_entry.neighbor_id,
                                   app_db_entry.dst_mac_address);
    AddNeighborEntry(app_db_entry, kRouterInterfaceOid2);

    // SAI API not being called makes the operation idempotent.
    EXPECT_THAT(UpdateNeighbors(std::vector<P4NeighborAppDbEntry>{app_db_entry}),
        ArrayEq(std::vector<StatusCode>{StatusCode::SWSS_RC_SUCCESS}));

    auto current_entry = GetNeighborEntry(neighbor_entry.neighbor_key);
    ASSERT_NE(current_entry, nullptr);
    EXPECT_EQ(current_entry->dst_mac_address, kMacAddress2);
}

TEST_F(NeighborManagerTest, SetDstMacAddressSaiApiFails)
{
    const P4NeighborAppDbEntry app_db_entry = {
        .router_intf_id = kRouterInterfaceId2,
        .neighbor_id = kNeighborId2,
        .dst_mac_address = kMacAddress2,
        .is_set_dst_mac = true};
    P4NeighborEntry neighbor_entry(app_db_entry.router_intf_id,
                                   app_db_entry.neighbor_id,
                                   app_db_entry.dst_mac_address);
    AddNeighborEntry(app_db_entry, kRouterInterfaceOid2);

    std::vector<sai_status_t> exp_status{SAI_STATUS_FAILURE};
    EXPECT_CALL(mock_sai_neighbor_, set_neighbor_entries_attribute(_, _, _, _, _))
        .WillOnce(DoAll(SetArrayArgument<4>(exp_status.begin(), exp_status.end()),
                        Return(SAI_STATUS_FAILURE)));

    const P4NeighborAppDbEntry new_app_db_entry = {
        .router_intf_id = kRouterInterfaceId2,
        .neighbor_id = kNeighborId2,
        .dst_mac_address = kMacAddress1,
        .is_set_dst_mac = true};
    EXPECT_THAT(
        UpdateNeighbors(std::vector<P4NeighborAppDbEntry>{new_app_db_entry}),
        ArrayEq(std::vector<StatusCode>{StatusCode::SWSS_RC_UNKNOWN}));

    auto current_entry = GetNeighborEntry(neighbor_entry.neighbor_key);
    ASSERT_NE(current_entry, nullptr);
    EXPECT_EQ(current_entry->dst_mac_address, kMacAddress2);
}

TEST_F(NeighborManagerTest, ProcessAddRequestValidAppDbParams)
{
    const P4NeighborAppDbEntry app_db_entry = {.router_intf_id = kRouterInterfaceId1,
                                               .neighbor_id = kNeighborId1,
                                               .dst_mac_address = kMacAddress1,
                                               .is_set_dst_mac = true};

    P4NeighborEntry neighbor_entry(app_db_entry.router_intf_id, app_db_entry.neighbor_id, app_db_entry.dst_mac_address);
    neighbor_entry.neigh_entry.switch_id = gSwitchId;
    copy(neighbor_entry.neigh_entry.ip_address, app_db_entry.neighbor_id);
    neighbor_entry.neigh_entry.rif_id = kRouterInterfaceOid1;

    std::vector<sai_attribute_t> attrs;
    sai_attribute_t attr;
    attr.id = SAI_NEIGHBOR_ENTRY_ATTR_DST_MAC_ADDRESS;
    memcpy(attr.value.mac, neighbor_entry.dst_mac_address.getMac(),
           sizeof(sai_mac_t));
    attrs.push_back(attr);
    attr.id = SAI_NEIGHBOR_ENTRY_ATTR_NO_HOST_ROUTE;
    attr.value.booldata = true;
    attrs.push_back(attr);

    std::vector<sai_status_t> exp_status{SAI_STATUS_SUCCESS};
    EXPECT_CALL(
        mock_sai_neighbor_,
        create_neighbor_entries(
            Eq(1),
            EntryArrayEq(
                std::vector<sai_neighbor_entry_t>{neighbor_entry.neigh_entry}),
            ArrayEq(std::vector<uint32_t>{2}),
            AttrArrayArrayEq(std::vector<std::vector<sai_attribute_t>>{attrs}),
            Eq(SAI_BULK_OP_ERROR_MODE_STOP_ON_ERROR), _))
        .WillOnce(DoAll(SetArrayArgument<5>(exp_status.begin(), exp_status.end()),
                Return(SAI_STATUS_SUCCESS)));
    ASSERT_TRUE(p4_oid_mapper_.setOID(SAI_OBJECT_TYPE_ROUTER_INTERFACE,
        KeyGenerator::generateRouterInterfaceKey(app_db_entry.router_intf_id),
        neighbor_entry.neigh_entry.rif_id));

    const std::string appl_db_key =
        std::string(APP_P4RT_NEIGHBOR_TABLE_NAME) + kTableKeyDelimiter +
        CreateNeighborAppDbKey(kRouterInterfaceId1, kNeighborId1);
    std::vector<swss::FieldValueTuple> attributes;
    attributes.push_back(swss::FieldValueTuple{prependParamField(p4orch::kDstMac),
         kMacAddress1.to_string()});
    EXPECT_CALL(publisher_,
              publish(Eq(APP_P4RT_TABLE_NAME), Eq(appl_db_key), Eq(attributes),
              Eq(StatusCode::SWSS_RC_SUCCESS), Eq(true)));

    EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS,
              ProcessEntries(std::vector<P4NeighborAppDbEntry>{app_db_entry},
              std::vector<swss::KeyOpFieldsValuesTuple>{
                                 swss::KeyOpFieldsValuesTuple(
                                     appl_db_key, SET_COMMAND, attributes)},
                             /*op=*/SET_COMMAND,
                             /*update=*/false));

    ValidateNeighborEntry(neighbor_entry, /*router_intf_ref_count=*/1);
}

TEST_F(NeighborManagerTest, ProcessUpdateRequestSetDstMacAddress)
{
    const P4NeighborAppDbEntry app_db_entry = {
        .router_intf_id = kRouterInterfaceId1,
        .neighbor_id = kNeighborId1,
        .dst_mac_address = kMacAddress1,
        .is_set_dst_mac = true};

    P4NeighborEntry neighbor_entry(app_db_entry.router_intf_id,
                                   app_db_entry.neighbor_id,
                                   app_db_entry.dst_mac_address);
    AddNeighborEntry(app_db_entry, kRouterInterfaceOid1);
  
    neighbor_entry.neigh_entry.switch_id = gSwitchId;
    copy(neighbor_entry.neigh_entry.ip_address, neighbor_entry.neighbor_id);
    neighbor_entry.neigh_entry.rif_id = kRouterInterfaceOid1;
  
    sai_attribute_t attr;
    attr.id = SAI_NEIGHBOR_ENTRY_ATTR_DST_MAC_ADDRESS;
    memcpy(attr.value.mac, kMacAddress2.getMac(), sizeof(sai_mac_t));
  
    std::vector<sai_status_t> exp_status{SAI_STATUS_SUCCESS};
    EXPECT_CALL(mock_sai_neighbor_,
                set_neighbor_entries_attribute(
                    Eq(1),
                    EntryArrayEq(std::vector<sai_neighbor_entry_t>{
                        neighbor_entry.neigh_entry}),
                    AttrArrayEq(std::vector<sai_attribute_t>{attr}),
                    Eq(SAI_BULK_OP_ERROR_MODE_STOP_ON_ERROR), _))
        .WillOnce(DoAll(SetArrayArgument<4>(exp_status.begin(), exp_status.end()),
                        Return(SAI_STATUS_SUCCESS)));
  
    const P4NeighborAppDbEntry new_app_db_entry = {
        .router_intf_id = kRouterInterfaceId1,
        .neighbor_id = kNeighborId1,
        .dst_mac_address = kMacAddress2,
        .is_set_dst_mac = true};
  
    const std::string appl_db_key =
        std::string(APP_P4RT_NEIGHBOR_TABLE_NAME) + kTableKeyDelimiter +
        CreateNeighborAppDbKey(kRouterInterfaceId1, kNeighborId1);
    std::vector<swss::FieldValueTuple> attributes;
    attributes.push_back(swss::FieldValueTuple{prependParamField(p4orch::kDstMac),
              kMacAddress2.to_string()});
  
    EXPECT_CALL(publisher_,
                publish(Eq(APP_P4RT_TABLE_NAME), Eq(appl_db_key), Eq(attributes),
                Eq(StatusCode::SWSS_RC_SUCCESS), Eq(true)));
  
    // Update neighbor entry present in the Manager.
    auto current_entry = GetNeighborEntry(neighbor_entry.neighbor_key);
    ASSERT_NE(current_entry, nullptr);
    EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS,
              ProcessEntries(std::vector<P4NeighborAppDbEntry>{new_app_db_entry},
                             std::vector<swss::KeyOpFieldsValuesTuple>{
                                 swss::KeyOpFieldsValuesTuple(
                                     appl_db_key, SET_COMMAND, attributes)},
                             /*op=*/SET_COMMAND,
                             /*update=*/true));
  
    // Validate that neighbor entry present in the Manager has the updated
    // MacAddress.
    neighbor_entry.dst_mac_address = kMacAddress2;
    ValidateNeighborEntry(neighbor_entry, /*router_intf_ref_count=*/1);
}

TEST_F(NeighborManagerTest, ProcessUpdateRequestSetDstMacAddressFails)
{
    const P4NeighborAppDbEntry app_db_entry = {
        .router_intf_id = kRouterInterfaceId1,
        .neighbor_id = kNeighborId1,
        .dst_mac_address = kMacAddress1,
        .is_set_dst_mac = true};
    P4NeighborEntry neighbor_entry(app_db_entry.router_intf_id,
                                   app_db_entry.neighbor_id,
                                   app_db_entry.dst_mac_address);
    neighbor_entry.neigh_entry.switch_id = gSwitchId;
    copy(neighbor_entry.neigh_entry.ip_address, neighbor_entry.neighbor_id);
    neighbor_entry.neigh_entry.rif_id = kRouterInterfaceOid1;
    AddNeighborEntry(app_db_entry, kRouterInterfaceOid1);
  
    std::vector<sai_status_t> exp_status{SAI_STATUS_FAILURE};
    EXPECT_CALL(mock_sai_neighbor_, set_neighbor_entries_attribute(_, _, _, _, _))
        .WillOnce(DoAll(SetArrayArgument<4>(exp_status.begin(), exp_status.end()),
                        Return(SAI_STATUS_FAILURE)));
  
    const P4NeighborAppDbEntry new_app_db_entry = {
        .router_intf_id = kRouterInterfaceId1,
        .neighbor_id = kNeighborId1,
        .dst_mac_address = kMacAddress2,
        .is_set_dst_mac = true};
  
    const std::string appl_db_key =
        std::string(APP_P4RT_NEIGHBOR_TABLE_NAME) + kTableKeyDelimiter +
        CreateNeighborAppDbKey(kRouterInterfaceId1, kNeighborId1);
    std::vector<swss::FieldValueTuple> attributes;
    attributes.push_back(swss::FieldValueTuple{prependParamField(p4orch::kDstMac),
       kMacAddress2.to_string()});
  
    EXPECT_CALL(publisher_,
               publish(Eq(APP_P4RT_TABLE_NAME), Eq(appl_db_key), Eq(attributes),
               Eq(StatusCode::SWSS_RC_UNKNOWN), Eq(true)));
  
    // Update neighbor entry present in the Manager.
    auto current_entry = GetNeighborEntry(neighbor_entry.neighbor_key);
    ASSERT_NE(current_entry, nullptr);
    EXPECT_EQ(StatusCode::SWSS_RC_UNKNOWN,
              ProcessEntries(std::vector<P4NeighborAppDbEntry>{new_app_db_entry},
              std::vector<swss::KeyOpFieldsValuesTuple>{
                         swss::KeyOpFieldsValuesTuple(
                         appl_db_key, SET_COMMAND, attributes)},
                         /*op=*/SET_COMMAND,
                         /*update=*/true));

    // Validate that neighbor entry present in the Manager has not changed.
    ValidateNeighborEntry(neighbor_entry, /*router_intf_ref_count=*/1);
}

TEST_F(NeighborManagerTest, ProcessDeleteRequestExistingNeighborEntry)
{
    const P4NeighborAppDbEntry app_db_entry = {
        .router_intf_id = kRouterInterfaceId1,
        .neighbor_id = kNeighborId1,
        .dst_mac_address = kMacAddress1,
        .is_set_dst_mac = true};
    P4NeighborEntry neighbor_entry(app_db_entry.router_intf_id,
                                   app_db_entry.neighbor_id,
                                   app_db_entry.dst_mac_address);
    AddNeighborEntry(app_db_entry, kRouterInterfaceOid1);
  
    neighbor_entry.neigh_entry.switch_id = gSwitchId;
    copy(neighbor_entry.neigh_entry.ip_address, neighbor_entry.neighbor_id);
    neighbor_entry.neigh_entry.rif_id = kRouterInterfaceOid1;
  
    std::vector<sai_status_t> exp_status{SAI_STATUS_SUCCESS};
    EXPECT_CALL(
        mock_sai_neighbor_,
        remove_neighbor_entries(Eq(1),
        EntryArrayEq(std::vector<sai_neighbor_entry_t>{
                    neighbor_entry.neigh_entry}),
                    Eq(SAI_BULK_OP_ERROR_MODE_STOP_ON_ERROR), _))
        .WillOnce(DoAll(SetArrayArgument<3>(exp_status.begin(), exp_status.end()),
                    Return(SAI_STATUS_SUCCESS)));
  
    const std::string appl_db_key =
        std::string(APP_P4RT_NEIGHBOR_TABLE_NAME) + kTableKeyDelimiter +
        CreateNeighborAppDbKey(kRouterInterfaceId1, kNeighborId1);
  
    EXPECT_CALL(publisher_, publish(Eq(APP_P4RT_TABLE_NAME), Eq(appl_db_key),
                    Eq(std::vector<swss::FieldValueTuple>{}),
                    Eq(StatusCode::SWSS_RC_SUCCESS), Eq(true)));
  
    EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS,
              ProcessEntries(std::vector<P4NeighborAppDbEntry>{app_db_entry},
                  std::vector<swss::KeyOpFieldsValuesTuple>{
                              swss::KeyOpFieldsValuesTuple(
                              appl_db_key, DEL_COMMAND,
                              std::vector<swss::FieldValueTuple>{})},
                             /*op=*/DEL_COMMAND,
                             /*update=*/true));
  
    ValidateNeighborEntryNotPresent(neighbor_entry, /*check_ref_count=*/true);
}

TEST_F(NeighborManagerTest, DeserializeNeighborEntryValidAttributes)
{
    const std::vector<swss::FieldValueTuple> attributes = {
        swss::FieldValueTuple(p4orch::kAction, "set_dst_mac"),
        swss::FieldValueTuple(prependParamField(p4orch::kDstMac), kMacAddress2.to_string()),
    };

    auto app_db_entry_or =
        DeserializeNeighborEntry(CreateNeighborAppDbKey(kRouterInterfaceId2, kNeighborId2), attributes);
    EXPECT_TRUE(app_db_entry_or.ok());
    auto &app_db_entry = *app_db_entry_or;
    EXPECT_EQ(app_db_entry.router_intf_id, kRouterInterfaceId2);
    EXPECT_EQ(app_db_entry.neighbor_id, kNeighborId2);
    EXPECT_EQ(app_db_entry.dst_mac_address, kMacAddress2);
    EXPECT_TRUE(app_db_entry.is_set_dst_mac);
}

TEST_F(NeighborManagerTest, DeserializeNeighborEntryInvalidKeyFormat)
{
    const std::vector<swss::FieldValueTuple> attributes = {
        swss::FieldValueTuple(p4orch::kAction, "set_dst_mac"),
        swss::FieldValueTuple(prependParamField(p4orch::kDstMac), kMacAddress2.to_string()),
    };

    // Ensure that the following key is valid. It shall be modified to construct
    // invalid key in rest of the test case.
    std::string valid_key = R"({"match/router_interface_id":"intf-3/4","match/neighbor_id":"10.0.0.1"})";
    EXPECT_TRUE(DeserializeNeighborEntry(valid_key, attributes).ok());

    // Invalid json format.
    std::string invalid_key = R"({"match/router_interface_id:intf-3/4,match/neighbor_id:10.0.0.1"})";
    EXPECT_FALSE(DeserializeNeighborEntry(invalid_key, attributes).ok());

    // Invalid json format.
    invalid_key = R"([{"match/router_interface_id":"intf-3/4","match/neighbor_id":"10.0.0.1"}])";
    EXPECT_FALSE(DeserializeNeighborEntry(invalid_key, attributes).ok());

    // Invalid json format.
    invalid_key = R"(["match/router_interface_id","intf-3/4","match/neighbor_id","10.0.0.1"])";
    EXPECT_FALSE(DeserializeNeighborEntry(invalid_key, attributes).ok());

    // Invalid json format.
    invalid_key = R"({"match/router_interface_id":"intf-3/4","match/neighbor_id:10.0.0.1"})";
    EXPECT_FALSE(DeserializeNeighborEntry(invalid_key, attributes).ok());

    // Invalid router interface id field name.
    invalid_key = R"({"match/router_interface":"intf-3/4","match/neighbor_id":"10.0.0.1"})";
    EXPECT_FALSE(DeserializeNeighborEntry(invalid_key, attributes).ok());

    // Invalid neighbor id field name.
    invalid_key = R"({"match/router_interface_id":"intf-3/4","match/neighbor":"10.0.0.1"})";
    EXPECT_FALSE(DeserializeNeighborEntry(invalid_key, attributes).ok());
}

TEST_F(NeighborManagerTest, DeserializeNeighborEntryMissingAction)
{
    const std::vector<swss::FieldValueTuple> attributes = {
        swss::FieldValueTuple(prependParamField(p4orch::kDstMac), kMacAddress2.to_string()),
    };

    auto app_db_entry_or =
        DeserializeNeighborEntry(CreateNeighborAppDbKey(kRouterInterfaceId2, kNeighborId2), attributes);
    EXPECT_TRUE(app_db_entry_or.ok());
    auto &app_db_entry = *app_db_entry_or;
    EXPECT_EQ(app_db_entry.router_intf_id, kRouterInterfaceId2);
    EXPECT_EQ(app_db_entry.neighbor_id, kNeighborId2);
    EXPECT_EQ(app_db_entry.dst_mac_address, kMacAddress2);
    EXPECT_TRUE(app_db_entry.is_set_dst_mac);
}

TEST_F(NeighborManagerTest, DeserializeNeighborEntryNoAttributes)
{
    const std::vector<swss::FieldValueTuple> attributes;

    auto app_db_entry_or =
        DeserializeNeighborEntry(CreateNeighborAppDbKey(kRouterInterfaceId2, kNeighborId2), attributes);
    EXPECT_TRUE(app_db_entry_or.ok());
    auto &app_db_entry = *app_db_entry_or;
    EXPECT_EQ(app_db_entry.router_intf_id, kRouterInterfaceId2);
    EXPECT_EQ(app_db_entry.neighbor_id, kNeighborId2);
    EXPECT_EQ(app_db_entry.dst_mac_address, swss::MacAddress());
    EXPECT_FALSE(app_db_entry.is_set_dst_mac);
}

TEST_F(NeighborManagerTest, DeserializeNeighborEntryInvalidField)
{
    const std::vector<swss::FieldValueTuple> attributes = {swss::FieldValueTuple("invalid_field", "invalid_value")};

    EXPECT_FALSE(DeserializeNeighborEntry(CreateNeighborAppDbKey(kRouterInterfaceId2, kNeighborId2), attributes).ok());
}

TEST_F(NeighborManagerTest, DeserializeNeighborEntryInvalidIpAddrValue)
{
    const std::vector<swss::FieldValueTuple> attributes;

    // Invalid IPv4 address.
    std::string invalid_key = R"({"match/router_interface_id":"intf-3/4","match/neighbor_id":"10.0.0.x"})";
    EXPECT_FALSE(DeserializeNeighborEntry(invalid_key, attributes).ok());

    // Invalid IPv6 address.
    invalid_key = R"({"match/router_interface_id":"intf-3/4","match/neighbor_id":"fe80::fe17:5f8g"})";
    EXPECT_FALSE(DeserializeNeighborEntry(invalid_key, attributes).ok());
}

TEST_F(NeighborManagerTest, DeserializeNeighborEntryInvalidMacAddrValue)
{
    const std::vector<swss::FieldValueTuple> attributes = {
        swss::FieldValueTuple(prependParamField(p4orch::kDstMac), "11:22:33:44:55")};

    EXPECT_FALSE(DeserializeNeighborEntry(CreateNeighborAppDbKey(kRouterInterfaceId2, kNeighborId2), attributes).ok());
}

TEST_F(NeighborManagerTest, ValidateNeighborAppDbEntryValidEntry)
{
    const P4NeighborAppDbEntry app_db_entry = {.router_intf_id = kRouterInterfaceId1,
                                               .neighbor_id = kNeighborId1,
                                               .dst_mac_address = kMacAddress1,
                                               .is_set_dst_mac = true};

    ASSERT_TRUE(p4_oid_mapper_.setOID(SAI_OBJECT_TYPE_ROUTER_INTERFACE,
                                      KeyGenerator::generateRouterInterfaceKey(app_db_entry.router_intf_id),
                                      kRouterInterfaceOid1));

    EXPECT_TRUE(ValidateNeighborEntryOperation(app_db_entry, SET_COMMAND).ok());
}

TEST_F(NeighborManagerTest, ValidateNeighborAppDbEntryValidDelEntry) {
    const P4NeighborAppDbEntry app_db_entry = {
        .router_intf_id = kRouterInterfaceId1,
        .neighbor_id = kNeighborId1,
        .dst_mac_address = kMacAddress1,
        .is_set_dst_mac = true};
    AddNeighborEntry(app_db_entry, kRouterInterfaceOid1);
  
    const P4NeighborAppDbEntry new_app_db_entry = {
        .router_intf_id = kRouterInterfaceId1,
        .neighbor_id = kNeighborId1,
        .dst_mac_address = swss::MacAddress(),
        .is_set_dst_mac = false};
  
    EXPECT_TRUE(
        ValidateNeighborEntryOperation(new_app_db_entry, DEL_COMMAND).ok());
}

TEST_F(NeighborManagerTest, ValidateNeighborAppDbEntryNonExistentRouterInterface)
{
    const P4NeighborAppDbEntry app_db_entry = {.router_intf_id = kRouterInterfaceId1,
                                               .neighbor_id = kNeighborId1,
                                               .dst_mac_address = kMacAddress1,
                                               .is_set_dst_mac = true};

    EXPECT_FALSE(ValidateNeighborEntryOperation(app_db_entry, SET_COMMAND).ok());
}

TEST_F(NeighborManagerTest, ValidateNeighborAppDbEntryZeroMacAddress)
{
    const P4NeighborAppDbEntry app_db_entry = {.router_intf_id = kRouterInterfaceId1,
                                               .neighbor_id = kNeighborId1,
                                               .dst_mac_address = swss::MacAddress(),
                                               .is_set_dst_mac = true};

    ASSERT_TRUE(p4_oid_mapper_.setOID(SAI_OBJECT_TYPE_ROUTER_INTERFACE,
                                      KeyGenerator::generateRouterInterfaceKey(app_db_entry.router_intf_id),
                                      kRouterInterfaceOid1));

    EXPECT_FALSE(ValidateNeighborEntryOperation(app_db_entry, SET_COMMAND).ok());
}

TEST_F(NeighborManagerTest, ValidateNeighborAppDbEntryMacAddressNotPresent)
{
    const P4NeighborAppDbEntry app_db_entry = {.router_intf_id = kRouterInterfaceId1,
                                               .neighbor_id = kNeighborId1,
                                               .dst_mac_address = swss::MacAddress(),
                                               .is_set_dst_mac = false};

    ASSERT_TRUE(p4_oid_mapper_.setOID(SAI_OBJECT_TYPE_ROUTER_INTERFACE,
                                      KeyGenerator::generateRouterInterfaceKey(app_db_entry.router_intf_id),
                                      kRouterInterfaceOid1));

    EXPECT_FALSE(ValidateNeighborEntryOperation(app_db_entry, SET_COMMAND).ok());
}

TEST_F(NeighborManagerTest,
       ValidateNeighborAppDbEntryMapperOidExistsForCreate) {
    const P4NeighborAppDbEntry app_db_entry = {
        .router_intf_id = kRouterInterfaceId1,
        .neighbor_id = kNeighborId1,
        .dst_mac_address = kMacAddress1,
        .is_set_dst_mac = true};
  
    ASSERT_TRUE(p4_oid_mapper_.setOID(
        SAI_OBJECT_TYPE_ROUTER_INTERFACE,
        KeyGenerator::generateRouterInterfaceKey(app_db_entry.router_intf_id),
        kRouterInterfaceOid1));
    ASSERT_TRUE(p4_oid_mapper_.setOID(
        SAI_OBJECT_TYPE_NEIGHBOR_ENTRY,
        KeyGenerator::generateNeighborKey(app_db_entry.router_intf_id,
                                          app_db_entry.neighbor_id),
        kRouterInterfaceOid1));
  
    EXPECT_FALSE(ValidateNeighborEntryOperation(app_db_entry, SET_COMMAND).ok());
}

TEST_F(NeighborManagerTest, ValidateNeighborAppDbEntryNotFoundInDel) {
    const P4NeighborAppDbEntry app_db_entry = {
        .router_intf_id = kRouterInterfaceId1,
        .neighbor_id = kNeighborId1,
        .dst_mac_address = swss::MacAddress(),
        .is_set_dst_mac = false};
  
    ASSERT_TRUE(p4_oid_mapper_.setOID(
        SAI_OBJECT_TYPE_ROUTER_INTERFACE,
        KeyGenerator::generateRouterInterfaceKey(app_db_entry.router_intf_id),
        kRouterInterfaceOid1));
  
    EXPECT_FALSE(ValidateNeighborEntryOperation(app_db_entry, DEL_COMMAND).ok());
}

TEST_F(NeighborManagerTest, ValidateNeighborAppDbOidMapperEntryNotFoundInDel) {
    const P4NeighborAppDbEntry app_db_entry = {
        .router_intf_id = kRouterInterfaceId1,
        .neighbor_id = kNeighborId1,
        .dst_mac_address = kMacAddress1,
        .is_set_dst_mac = true};
    AddNeighborEntry(app_db_entry, kRouterInterfaceOid1);
  
    ASSERT_TRUE(p4_oid_mapper_.eraseOID(
        SAI_OBJECT_TYPE_NEIGHBOR_ENTRY,
        KeyGenerator::generateNeighborKey(app_db_entry.router_intf_id,
                                          app_db_entry.neighbor_id)));
  
    const P4NeighborAppDbEntry new_app_db_entry = {
        .router_intf_id = kRouterInterfaceId1,
        .neighbor_id = kNeighborId1,
        .dst_mac_address = swss::MacAddress(),
        .is_set_dst_mac = false};
  
    EXPECT_FALSE(
        ValidateNeighborEntryOperation(new_app_db_entry, DEL_COMMAND).ok());
}

TEST_F(NeighborManagerTest, ValidateNeighborAppDbRefCntNotZeroInDel) {
    const P4NeighborAppDbEntry app_db_entry = {
        .router_intf_id = kRouterInterfaceId1,
        .neighbor_id = kNeighborId1,
        .dst_mac_address = kMacAddress1,
        .is_set_dst_mac = true};
    P4NeighborEntry neighbor_entry(app_db_entry.router_intf_id,
                                   app_db_entry.neighbor_id,
                                   app_db_entry.dst_mac_address);
    AddNeighborEntry(app_db_entry, kRouterInterfaceOid1);
  
    ASSERT_TRUE(p4_oid_mapper_.increaseRefCount(
        SAI_OBJECT_TYPE_NEIGHBOR_ENTRY,
        KeyGenerator::generateNeighborKey(app_db_entry.router_intf_id,
                                          app_db_entry.neighbor_id)));
  
    const P4NeighborAppDbEntry new_app_db_entry = {
        .router_intf_id = kRouterInterfaceId1,
        .neighbor_id = kNeighborId1,
        .dst_mac_address = swss::MacAddress(),
        .is_set_dst_mac = false};
  
    EXPECT_FALSE(
        ValidateNeighborEntryOperation(new_app_db_entry, DEL_COMMAND).ok());
}

TEST_F(NeighborManagerTest, DrainValidAttributes)
{
    ASSERT_TRUE(p4_oid_mapper_.setOID(SAI_OBJECT_TYPE_ROUTER_INTERFACE,
                                      KeyGenerator::generateRouterInterfaceKey(kRouterInterfaceId1),
                                      kRouterInterfaceOid1));

    const std::string appl_db_key = std::string(APP_P4RT_NEIGHBOR_TABLE_NAME) + kTableKeyDelimiter +
                                    CreateNeighborAppDbKey(kRouterInterfaceId1, kNeighborId1);

    // Enqueue entry for create operation.
    std::vector<swss::FieldValueTuple> attributes;
    attributes.push_back(swss::FieldValueTuple{prependParamField(p4orch::kDstMac), kMacAddress1.to_string()});
    Enqueue(swss::KeyOpFieldsValuesTuple(appl_db_key, SET_COMMAND, attributes));

    std::vector<sai_status_t> exp_status{SAI_STATUS_SUCCESS};
    EXPECT_CALL(mock_sai_neighbor_, create_neighbor_entries(_, _, _, _, _, _))
        .WillOnce(DoAll(SetArrayArgument<5>(exp_status.begin(), exp_status.end()),
             Return(SAI_STATUS_SUCCESS)));
    EXPECT_CALL(publisher_, publish(Eq(APP_P4RT_TABLE_NAME), Eq(appl_db_key),
                                    Eq(attributes),
                                    Eq(StatusCode::SWSS_RC_SUCCESS), Eq(true)));
    EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS, Drain(/*failure_before=*/false));

    P4NeighborEntry neighbor_entry(kRouterInterfaceId1, kNeighborId1, kMacAddress1);
    neighbor_entry.neigh_entry.switch_id = gSwitchId;
    copy(neighbor_entry.neigh_entry.ip_address, neighbor_entry.neighbor_id);
    neighbor_entry.neigh_entry.rif_id = kRouterInterfaceOid1;
    ValidateNeighborEntry(neighbor_entry, /*router_intf_ref_count=*/1);

    // Enqueue entry for update operation.
    attributes.clear();
    attributes.push_back(swss::FieldValueTuple{prependParamField(p4orch::kDstMac), kMacAddress2.to_string()});
    Enqueue(swss::KeyOpFieldsValuesTuple(appl_db_key, SET_COMMAND, attributes));

    EXPECT_CALL(mock_sai_neighbor_, set_neighbor_entries_attribute(_, _, _, _, _))
        .WillOnce(DoAll(SetArrayArgument<4>(exp_status.begin(), exp_status.end()),
                 Return(SAI_STATUS_SUCCESS)));
    EXPECT_CALL(publisher_, publish(Eq(APP_P4RT_TABLE_NAME), Eq(appl_db_key),
                                    Eq(attributes),
                                    Eq(StatusCode::SWSS_RC_SUCCESS), Eq(true)));
    EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS, Drain(/*failure_before=*/false));

    neighbor_entry.dst_mac_address = kMacAddress2;
    ValidateNeighborEntry(neighbor_entry, /*router_intf_ref_count=*/1);

    // Enqueue entry for delete operation.
    attributes.clear();
    Enqueue(swss::KeyOpFieldsValuesTuple(appl_db_key, DEL_COMMAND, attributes));

    EXPECT_CALL(mock_sai_neighbor_, remove_neighbor_entries(_, _, _, _))
        .WillOnce(DoAll(SetArrayArgument<3>(exp_status.begin(), exp_status.end()),
                Return(SAI_STATUS_SUCCESS)));
    EXPECT_CALL(publisher_, publish(Eq(APP_P4RT_TABLE_NAME), Eq(appl_db_key),
                                    Eq(attributes),
                                    Eq(StatusCode::SWSS_RC_SUCCESS), Eq(true)));
    EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS, Drain(/*failure_before=*/false));

    ValidateNeighborEntryNotPresent(neighbor_entry, /*check_ref_count=*/true);
}

TEST_F(NeighborManagerTest, DrainInvalidAppDbEntryKey)
{
    ASSERT_TRUE(p4_oid_mapper_.setOID(SAI_OBJECT_TYPE_ROUTER_INTERFACE,
                                      KeyGenerator::generateRouterInterfaceKey(kRouterInterfaceId1),
                                      kRouterInterfaceOid1));

    // create invalid neighbor key with router interface id as kRouterInterfaceId1
    // and neighbor id as kNeighborId1
    const std::string invalid_neighbor_key = R"({"match/router_interface_id:intf-3/4,match/neighbor_id:10.0.0.22"})";
    const std::string appl_db_key =
        std::string(APP_P4RT_NEIGHBOR_TABLE_NAME) + kTableKeyDelimiter + invalid_neighbor_key;

    // Enqueue entry for create operation.
    std::vector<swss::FieldValueTuple> attributes;
    Enqueue(swss::KeyOpFieldsValuesTuple(appl_db_key, SET_COMMAND, attributes));
    EXPECT_CALL(
        publisher_,
        publish(Eq(APP_P4RT_TABLE_NAME), Eq(appl_db_key), Eq(attributes),
                Eq(StatusCode::SWSS_RC_INVALID_PARAM), Eq(true)));
    EXPECT_EQ(StatusCode::SWSS_RC_INVALID_PARAM,
              Drain(/*failure_before=*/false));

    P4NeighborEntry neighbor_entry(kRouterInterfaceId1, kNeighborId1, kMacAddress1);
    ValidateNeighborEntryNotPresent(neighbor_entry, /*check_ref_count=*/true);
}

TEST_F(NeighborManagerTest, DrainInvalidAppDbEntryAttributes)
{
    ASSERT_TRUE(p4_oid_mapper_.setOID(SAI_OBJECT_TYPE_ROUTER_INTERFACE,
                                      KeyGenerator::generateRouterInterfaceKey(kRouterInterfaceId1),
                                      kRouterInterfaceOid1));

    // Non-existent router interface id in neighbor key.
    std::string appl_db_key = std::string(APP_P4RT_NEIGHBOR_TABLE_NAME) + kTableKeyDelimiter +
                              CreateNeighborAppDbKey(kRouterInterfaceId2, kNeighborId1);

    std::vector<swss::FieldValueTuple> attributes;
    Enqueue(swss::KeyOpFieldsValuesTuple(appl_db_key, SET_COMMAND, attributes));
    EXPECT_CALL(
        publisher_,
        publish(Eq(APP_P4RT_TABLE_NAME), Eq(appl_db_key), Eq(attributes),
                Eq(StatusCode::SWSS_RC_NOT_FOUND), Eq(true)));
    EXPECT_EQ(StatusCode::SWSS_RC_NOT_FOUND, Drain(/*failure_before=*/false));

    appl_db_key = std::string(APP_P4RT_NEIGHBOR_TABLE_NAME) + kTableKeyDelimiter +
                  CreateNeighborAppDbKey(kRouterInterfaceId1, kNeighborId1);
    // Invalid destination mac address attribute.
    attributes.clear();
    attributes.push_back(swss::FieldValueTuple{p4orch::kDstMac, swss::MacAddress().to_string()});
    Enqueue(swss::KeyOpFieldsValuesTuple(appl_db_key, SET_COMMAND, attributes));
    EXPECT_CALL(
        publisher_,
        publish(Eq(APP_P4RT_TABLE_NAME), Eq(appl_db_key), Eq(attributes),
                Eq(StatusCode::SWSS_RC_INVALID_PARAM), Eq(true)));
    EXPECT_EQ(StatusCode::SWSS_RC_INVALID_PARAM,
              Drain(/*failure_before=*/false));

    // Validate that first create operation did not create a neighbor entry.
    P4NeighborEntry neighbor_entry1(kRouterInterfaceId2, kNeighborId1, kMacAddress1);
    ValidateNeighborEntryNotPresent(neighbor_entry1, /*check_ref_count=*/false);

    // Validate that second create operation did not create a neighbor entry.
    P4NeighborEntry neighbor_entry2(kRouterInterfaceId1, kNeighborId1, kMacAddress1);
    ValidateNeighborEntryNotPresent(neighbor_entry2, /*check_ref_count=*/true);
}

TEST_F(NeighborManagerTest, DrainInvalidOperation)
{
    ASSERT_TRUE(p4_oid_mapper_.setOID(SAI_OBJECT_TYPE_ROUTER_INTERFACE,
                                      KeyGenerator::generateRouterInterfaceKey(kRouterInterfaceId1),
                                      kRouterInterfaceOid1));

    const std::string appl_db_key = std::string(APP_P4RT_NEIGHBOR_TABLE_NAME) + kTableKeyDelimiter +
                                    CreateNeighborAppDbKey(kRouterInterfaceId1, kNeighborId1);

    std::vector<swss::FieldValueTuple> attributes;
    Enqueue(swss::KeyOpFieldsValuesTuple(appl_db_key, "INVALID", attributes));
    EXPECT_CALL(
        publisher_,
        publish(Eq(APP_P4RT_TABLE_NAME), Eq(appl_db_key), Eq(attributes),
                Eq(StatusCode::SWSS_RC_INVALID_PARAM), Eq(true)));
    EXPECT_EQ(StatusCode::SWSS_RC_INVALID_PARAM,
              Drain(/*failure_before=*/false));

    P4NeighborEntry neighbor_entry(kRouterInterfaceId1, kNeighborId1, kMacAddress1);
    ValidateNeighborEntryNotPresent(neighbor_entry, /*check_ref_count=*/true);
}

TEST_F(NeighborManagerTest, DrainNotExecuted) {
  ASSERT_TRUE(p4_oid_mapper_.setOID(
      SAI_OBJECT_TYPE_ROUTER_INTERFACE,
      KeyGenerator::generateRouterInterfaceKey(kRouterInterfaceId1),
      kRouterInterfaceOid1));
  ASSERT_TRUE(p4_oid_mapper_.setOID(
      SAI_OBJECT_TYPE_ROUTER_INTERFACE,
      KeyGenerator::generateRouterInterfaceKey(kRouterInterfaceId2),
      kRouterInterfaceOid2));
  ASSERT_TRUE(p4_oid_mapper_.setOID(
      SAI_OBJECT_TYPE_ROUTER_INTERFACE,
      KeyGenerator::generateRouterInterfaceKey(kRouterInterfaceId3),
      kRouterInterfaceOid3));

  const std::string appl_db_key_1 =
      std::string(APP_P4RT_NEIGHBOR_TABLE_NAME) + kTableKeyDelimiter +
      CreateNeighborAppDbKey(kRouterInterfaceId1, kNeighborId1);
  const std::string appl_db_key_2 =
      std::string(APP_P4RT_NEIGHBOR_TABLE_NAME) + kTableKeyDelimiter +
      CreateNeighborAppDbKey(kRouterInterfaceId2, kNeighborId1);
  const std::string appl_db_key_3 =
      std::string(APP_P4RT_NEIGHBOR_TABLE_NAME) + kTableKeyDelimiter +
      CreateNeighborAppDbKey(kRouterInterfaceId3, kNeighborId1);

  std::vector<swss::FieldValueTuple> attributes;
  attributes.push_back(swss::FieldValueTuple{prependParamField(p4orch::kDstMac),
                                             kMacAddress1.to_string()});
  Enqueue(swss::KeyOpFieldsValuesTuple(appl_db_key_1, SET_COMMAND, attributes));
  Enqueue(swss::KeyOpFieldsValuesTuple(appl_db_key_2, SET_COMMAND, attributes));
  Enqueue(swss::KeyOpFieldsValuesTuple(appl_db_key_3, SET_COMMAND, attributes));

  EXPECT_CALL(
      publisher_,
      publish(Eq(APP_P4RT_TABLE_NAME), Eq(appl_db_key_1), Eq(attributes),
              Eq(StatusCode::SWSS_RC_NOT_EXECUTED), Eq(true)));
  EXPECT_CALL(
      publisher_,
      publish(Eq(APP_P4RT_TABLE_NAME), Eq(appl_db_key_2), Eq(attributes),
              Eq(StatusCode::SWSS_RC_NOT_EXECUTED), Eq(true)));
  EXPECT_CALL(
      publisher_,
      publish(Eq(APP_P4RT_TABLE_NAME), Eq(appl_db_key_3), Eq(attributes),
              Eq(StatusCode::SWSS_RC_NOT_EXECUTED), Eq(true)));
  EXPECT_EQ(StatusCode::SWSS_RC_NOT_EXECUTED, Drain(/*failure_before=*/true));
  EXPECT_EQ(nullptr, GetNeighborEntry(KeyGenerator::generateNeighborKey(
                         kRouterInterfaceId1, kNeighborId1)));
  EXPECT_EQ(nullptr, GetNeighborEntry(KeyGenerator::generateNeighborKey(
                         kRouterInterfaceId2, kNeighborId1)));
  EXPECT_EQ(nullptr, GetNeighborEntry(KeyGenerator::generateNeighborKey(
                         kRouterInterfaceId3, kNeighborId1)));
}

TEST_F(NeighborManagerTest, DrainStopOnFirstFailureCreate) {
  ASSERT_TRUE(p4_oid_mapper_.setOID(
      SAI_OBJECT_TYPE_ROUTER_INTERFACE,
      KeyGenerator::generateRouterInterfaceKey(kRouterInterfaceId1),
      kRouterInterfaceOid1));
  ASSERT_TRUE(p4_oid_mapper_.setOID(
      SAI_OBJECT_TYPE_ROUTER_INTERFACE,
      KeyGenerator::generateRouterInterfaceKey(kRouterInterfaceId2),
      kRouterInterfaceOid2));
  ASSERT_TRUE(p4_oid_mapper_.setOID(
      SAI_OBJECT_TYPE_ROUTER_INTERFACE,
      KeyGenerator::generateRouterInterfaceKey(kRouterInterfaceId3),
      kRouterInterfaceOid3));

  const std::string appl_db_key_1 =
      std::string(APP_P4RT_NEIGHBOR_TABLE_NAME) + kTableKeyDelimiter +
      CreateNeighborAppDbKey(kRouterInterfaceId1, kNeighborId1);
  const std::string appl_db_key_2 =
      std::string(APP_P4RT_NEIGHBOR_TABLE_NAME) + kTableKeyDelimiter +
      CreateNeighborAppDbKey(kRouterInterfaceId2, kNeighborId1);
  const std::string appl_db_key_3 =
      std::string(APP_P4RT_NEIGHBOR_TABLE_NAME) + kTableKeyDelimiter +
      CreateNeighborAppDbKey(kRouterInterfaceId3, kNeighborId1);

  std::vector<swss::FieldValueTuple> attributes;
  attributes.push_back(swss::FieldValueTuple{prependParamField(p4orch::kDstMac),
                                             kMacAddress1.to_string()});
  Enqueue(swss::KeyOpFieldsValuesTuple(appl_db_key_1, SET_COMMAND, attributes));
  Enqueue(swss::KeyOpFieldsValuesTuple(appl_db_key_2, SET_COMMAND, attributes));
  Enqueue(swss::KeyOpFieldsValuesTuple(appl_db_key_3, SET_COMMAND, attributes));

  sai_neighbor_entry_t neigh_entry_1;
  neigh_entry_1.switch_id = gSwitchId;
  copy(neigh_entry_1.ip_address, kNeighborId1);
  neigh_entry_1.rif_id = kRouterInterfaceOid1;
  sai_neighbor_entry_t neigh_entry_2;
  neigh_entry_2.switch_id = gSwitchId;
  copy(neigh_entry_2.ip_address, kNeighborId1);
  neigh_entry_2.rif_id = kRouterInterfaceOid2;
  sai_neighbor_entry_t neigh_entry_3;
  neigh_entry_3.switch_id = gSwitchId;
  copy(neigh_entry_3.ip_address, kNeighborId1);
  neigh_entry_3.rif_id = kRouterInterfaceOid3;

  std::vector<sai_attribute_t> attrs;
  sai_attribute_t attr;
  attr.id = SAI_NEIGHBOR_ENTRY_ATTR_DST_MAC_ADDRESS;
  memcpy(attr.value.mac, kMacAddress1.getMac(), sizeof(sai_mac_t));
  attrs.push_back(attr);
  attr.id = SAI_NEIGHBOR_ENTRY_ATTR_NO_HOST_ROUTE;
  attr.value.booldata = true;
  attrs.push_back(attr);

  std::vector<sai_status_t> exp_status{SAI_STATUS_SUCCESS, SAI_STATUS_FAILURE,
                                       SAI_STATUS_NOT_EXECUTED};
  EXPECT_CALL(mock_sai_neighbor_,
              create_neighbor_entries(
                  Eq(3),
                  EntryArrayEq(std::vector<sai_neighbor_entry_t>{
                      neigh_entry_1, neigh_entry_2, neigh_entry_3}),
                  ArrayEq(std::vector<uint32_t>{2, 2, 2}),
                  AttrArrayArrayEq(std::vector<std::vector<sai_attribute_t>>{
                      attrs, attrs, attrs}),
                  Eq(SAI_BULK_OP_ERROR_MODE_STOP_ON_ERROR), _))
      .WillOnce(DoAll(SetArrayArgument<5>(exp_status.begin(), exp_status.end()),
                      Return(SAI_STATUS_FAILURE)));
  EXPECT_CALL(publisher_, publish(Eq(APP_P4RT_TABLE_NAME), Eq(appl_db_key_1),
                                  Eq(attributes),
                                  Eq(StatusCode::SWSS_RC_SUCCESS), Eq(true)));
  EXPECT_CALL(publisher_, publish(Eq(APP_P4RT_TABLE_NAME), Eq(appl_db_key_2),
                                  Eq(attributes),
                                  Eq(StatusCode::SWSS_RC_UNKNOWN), Eq(true)));
  EXPECT_CALL(
      publisher_,
      publish(Eq(APP_P4RT_TABLE_NAME), Eq(appl_db_key_3), Eq(attributes),
              Eq(StatusCode::SWSS_RC_NOT_EXECUTED), Eq(true)));
  EXPECT_EQ(StatusCode::SWSS_RC_UNKNOWN, Drain(/*failure_before=*/false));
  EXPECT_NE(nullptr, GetNeighborEntry(KeyGenerator::generateNeighborKey(
                         kRouterInterfaceId1, kNeighborId1)));
  EXPECT_EQ(nullptr, GetNeighborEntry(KeyGenerator::generateNeighborKey(
                         kRouterInterfaceId2, kNeighborId1)));
  EXPECT_EQ(nullptr, GetNeighborEntry(KeyGenerator::generateNeighborKey(
                         kRouterInterfaceId3, kNeighborId1)));
}

TEST_F(NeighborManagerTest, DrainStopOnFirstFailureUpdate) {
  const P4NeighborAppDbEntry app_db_entry_1 = {
      .router_intf_id = kRouterInterfaceId1,
      .neighbor_id = kNeighborId1,
      .dst_mac_address = kMacAddress1,
      .is_set_dst_mac = true};
  AddNeighborEntry(app_db_entry_1, kRouterInterfaceOid1);
  const P4NeighborAppDbEntry app_db_entry_2 = {
      .router_intf_id = kRouterInterfaceId2,
      .neighbor_id = kNeighborId2,
      .dst_mac_address = kMacAddress1,
      .is_set_dst_mac = true};
  AddNeighborEntry(app_db_entry_2, kRouterInterfaceOid1);
  const P4NeighborAppDbEntry app_db_entry_3 = {
      .router_intf_id = kRouterInterfaceId3,
      .neighbor_id = kNeighborId2,
      .dst_mac_address = kMacAddress1,
      .is_set_dst_mac = true};
  AddNeighborEntry(app_db_entry_3, kRouterInterfaceOid1);

  const std::string appl_db_key_1 =
      std::string(APP_P4RT_NEIGHBOR_TABLE_NAME) + kTableKeyDelimiter +
      CreateNeighborAppDbKey(kRouterInterfaceId1, kNeighborId1);
  const std::string appl_db_key_2 =
      std::string(APP_P4RT_NEIGHBOR_TABLE_NAME) + kTableKeyDelimiter +
      CreateNeighborAppDbKey(kRouterInterfaceId2, kNeighborId2);
  const std::string appl_db_key_3 =
      std::string(APP_P4RT_NEIGHBOR_TABLE_NAME) + kTableKeyDelimiter +
      CreateNeighborAppDbKey(kRouterInterfaceId3, kNeighborId2);

  std::vector<swss::FieldValueTuple> attributes;
  attributes.push_back(swss::FieldValueTuple{prependParamField(p4orch::kDstMac),
                                             kMacAddress2.to_string()});
  Enqueue(swss::KeyOpFieldsValuesTuple(appl_db_key_1, SET_COMMAND, attributes));
  Enqueue(swss::KeyOpFieldsValuesTuple(appl_db_key_2, SET_COMMAND, attributes));
  Enqueue(swss::KeyOpFieldsValuesTuple(appl_db_key_3, SET_COMMAND, attributes));

  sai_neighbor_entry_t neigh_entry_1;
  neigh_entry_1.switch_id = gSwitchId;
  copy(neigh_entry_1.ip_address, kNeighborId1);
  neigh_entry_1.rif_id = kRouterInterfaceOid1;
  sai_neighbor_entry_t neigh_entry_2;
  neigh_entry_2.switch_id = gSwitchId;
  copy(neigh_entry_2.ip_address, kNeighborId2);
  neigh_entry_2.rif_id = kRouterInterfaceOid1;
  sai_neighbor_entry_t neigh_entry_3;
  neigh_entry_3.switch_id = gSwitchId;
  copy(neigh_entry_3.ip_address, kNeighborId2);
  neigh_entry_3.rif_id = kRouterInterfaceOid1;

  sai_attribute_t attr;
  attr.id = SAI_NEIGHBOR_ENTRY_ATTR_DST_MAC_ADDRESS;
  memcpy(attr.value.mac, kMacAddress2.getMac(), sizeof(sai_mac_t));

  std::vector<sai_status_t> exp_status{SAI_STATUS_SUCCESS, SAI_STATUS_FAILURE,
                                       SAI_STATUS_NOT_EXECUTED};
  EXPECT_CALL(mock_sai_neighbor_,
              set_neighbor_entries_attribute(
                  Eq(3),
                  EntryArrayEq(std::vector<sai_neighbor_entry_t>{
                      neigh_entry_1, neigh_entry_2, neigh_entry_3}),
                  AttrArrayEq(std::vector<sai_attribute_t>{attr, attr, attr}),
                  Eq(SAI_BULK_OP_ERROR_MODE_STOP_ON_ERROR), _))
      .WillOnce(DoAll(SetArrayArgument<4>(exp_status.begin(), exp_status.end()),
                      Return(SAI_STATUS_FAILURE)));

  EXPECT_CALL(publisher_, publish(Eq(APP_P4RT_TABLE_NAME), Eq(appl_db_key_1),
                                  Eq(attributes),
                                  Eq(StatusCode::SWSS_RC_SUCCESS), Eq(true)));
  EXPECT_CALL(publisher_, publish(Eq(APP_P4RT_TABLE_NAME), Eq(appl_db_key_2),
                                  Eq(attributes),
                                  Eq(StatusCode::SWSS_RC_UNKNOWN), Eq(true)));
  EXPECT_CALL(
      publisher_,
      publish(Eq(APP_P4RT_TABLE_NAME), Eq(appl_db_key_3), Eq(attributes),
              Eq(StatusCode::SWSS_RC_NOT_EXECUTED), Eq(true)));
  EXPECT_EQ(StatusCode::SWSS_RC_UNKNOWN, Drain(/*failure_before=*/false));

  auto* entry_1 = GetNeighborEntry(
      KeyGenerator::generateNeighborKey(kRouterInterfaceId1, kNeighborId1));
  EXPECT_NE(nullptr, entry_1);
  EXPECT_EQ(entry_1->dst_mac_address, kMacAddress2);
  auto* entry_2 = GetNeighborEntry(
      KeyGenerator::generateNeighborKey(kRouterInterfaceId2, kNeighborId2));
  EXPECT_NE(nullptr, entry_2);
  EXPECT_EQ(entry_2->dst_mac_address, kMacAddress1);
  auto* entry_3 = GetNeighborEntry(
      KeyGenerator::generateNeighborKey(kRouterInterfaceId3, kNeighborId2));
  EXPECT_NE(nullptr, entry_3);
  EXPECT_EQ(entry_3->dst_mac_address, kMacAddress1);
}

TEST_F(NeighborManagerTest, DrainStopOnFirstFailureDel) {
  const P4NeighborAppDbEntry app_db_entry_1 = {
      .router_intf_id = kRouterInterfaceId1,
      .neighbor_id = kNeighborId1,
      .dst_mac_address = kMacAddress1,
      .is_set_dst_mac = true};
  AddNeighborEntry(app_db_entry_1, kRouterInterfaceOid1);
  const P4NeighborAppDbEntry app_db_entry_2 = {
      .router_intf_id = kRouterInterfaceId2,
      .neighbor_id = kNeighborId2,
      .dst_mac_address = kMacAddress1,
      .is_set_dst_mac = true};
  AddNeighborEntry(app_db_entry_2, kRouterInterfaceOid1);
  const P4NeighborAppDbEntry app_db_entry_3 = {
      .router_intf_id = kRouterInterfaceId3,
      .neighbor_id = kNeighborId2,
      .dst_mac_address = kMacAddress1,
      .is_set_dst_mac = true};
  AddNeighborEntry(app_db_entry_3, kRouterInterfaceOid1);

  const std::string appl_db_key_1 =
      std::string(APP_P4RT_NEIGHBOR_TABLE_NAME) + kTableKeyDelimiter +
      CreateNeighborAppDbKey(kRouterInterfaceId1, kNeighborId1);
  const std::string appl_db_key_2 =
      std::string(APP_P4RT_NEIGHBOR_TABLE_NAME) + kTableKeyDelimiter +
      CreateNeighborAppDbKey(kRouterInterfaceId2, kNeighborId2);
  const std::string appl_db_key_3 =
      std::string(APP_P4RT_NEIGHBOR_TABLE_NAME) + kTableKeyDelimiter +
      CreateNeighborAppDbKey(kRouterInterfaceId3, kNeighborId2);

  std::vector<swss::FieldValueTuple> attributes;
  Enqueue(swss::KeyOpFieldsValuesTuple(appl_db_key_1, DEL_COMMAND, attributes));
  Enqueue(swss::KeyOpFieldsValuesTuple(appl_db_key_2, DEL_COMMAND, attributes));
  Enqueue(swss::KeyOpFieldsValuesTuple(appl_db_key_3, DEL_COMMAND, attributes));

  sai_neighbor_entry_t neigh_entry_1;
  neigh_entry_1.switch_id = gSwitchId;
  copy(neigh_entry_1.ip_address, kNeighborId1);
  neigh_entry_1.rif_id = kRouterInterfaceOid1;
  sai_neighbor_entry_t neigh_entry_2;
  neigh_entry_2.switch_id = gSwitchId;
  copy(neigh_entry_2.ip_address, kNeighborId2);
  neigh_entry_2.rif_id = kRouterInterfaceOid1;
  sai_neighbor_entry_t neigh_entry_3;
  neigh_entry_3.switch_id = gSwitchId;
  copy(neigh_entry_3.ip_address, kNeighborId2);
  neigh_entry_3.rif_id = kRouterInterfaceOid1;

  std::vector<sai_status_t> exp_status{SAI_STATUS_SUCCESS, SAI_STATUS_FAILURE,
                                       SAI_STATUS_NOT_EXECUTED};
  EXPECT_CALL(
      mock_sai_neighbor_,
      remove_neighbor_entries(Eq(3),
                              EntryArrayEq(std::vector<sai_neighbor_entry_t>{
                                  neigh_entry_1, neigh_entry_2, neigh_entry_3}),
                              Eq(SAI_BULK_OP_ERROR_MODE_STOP_ON_ERROR), _))
      .WillOnce(DoAll(SetArrayArgument<3>(exp_status.begin(), exp_status.end()),
                      Return(SAI_STATUS_FAILURE)));

  EXPECT_CALL(publisher_, publish(Eq(APP_P4RT_TABLE_NAME), Eq(appl_db_key_1),
                                  Eq(attributes),
                                  Eq(StatusCode::SWSS_RC_SUCCESS), Eq(true)));
  EXPECT_CALL(publisher_, publish(Eq(APP_P4RT_TABLE_NAME), Eq(appl_db_key_2),
                                  Eq(attributes),
                                  Eq(StatusCode::SWSS_RC_UNKNOWN), Eq(true)));
  EXPECT_CALL(
      publisher_,
      publish(Eq(APP_P4RT_TABLE_NAME), Eq(appl_db_key_3), Eq(attributes),
              Eq(StatusCode::SWSS_RC_NOT_EXECUTED), Eq(true)));
  EXPECT_EQ(StatusCode::SWSS_RC_UNKNOWN, Drain(/*failure_before=*/false));

  EXPECT_EQ(nullptr, GetNeighborEntry(KeyGenerator::generateNeighborKey(
                         kRouterInterfaceId1, kNeighborId1)));
  EXPECT_NE(nullptr, GetNeighborEntry(KeyGenerator::generateNeighborKey(
                         kRouterInterfaceId2, kNeighborId2)));
  EXPECT_NE(nullptr, GetNeighborEntry(KeyGenerator::generateNeighborKey(
                         kRouterInterfaceId3, kNeighborId2)));
}

TEST_F(NeighborManagerTest, DrainStopOnFirstFailureDifferentTypes) {
  const P4NeighborAppDbEntry app_db_entry_1 = {
      .router_intf_id = kRouterInterfaceId1,
      .neighbor_id = kNeighborId1,
      .dst_mac_address = kMacAddress1,
      .is_set_dst_mac = true};
  AddNeighborEntry(app_db_entry_1, kRouterInterfaceOid1);
  const P4NeighborAppDbEntry app_db_entry_2 = {
      .router_intf_id = kRouterInterfaceId2,
      .neighbor_id = kNeighborId2,
      .dst_mac_address = kMacAddress2,
      .is_set_dst_mac = true};
  AddNeighborEntry(app_db_entry_2, kRouterInterfaceOid1);

  ASSERT_TRUE(p4_oid_mapper_.setOID(
      SAI_OBJECT_TYPE_ROUTER_INTERFACE,
      KeyGenerator::generateRouterInterfaceKey(kRouterInterfaceId3),
      kRouterInterfaceOid3));

  const std::string appl_db_key_1 =
      std::string(APP_P4RT_NEIGHBOR_TABLE_NAME) + kTableKeyDelimiter +
      CreateNeighborAppDbKey(kRouterInterfaceId1, kNeighborId1);
  const std::string appl_db_key_2 =
      std::string(APP_P4RT_NEIGHBOR_TABLE_NAME) + kTableKeyDelimiter +
      CreateNeighborAppDbKey(kRouterInterfaceId2, kNeighborId2);
  const std::string appl_db_key_3 =
      std::string(APP_P4RT_NEIGHBOR_TABLE_NAME) + kTableKeyDelimiter +
      CreateNeighborAppDbKey(kRouterInterfaceId3, kNeighborId1);

  std::vector<swss::FieldValueTuple> attributes;
  attributes.push_back(swss::FieldValueTuple{prependParamField(p4orch::kDstMac),
                                             kMacAddress2.to_string()});
  Enqueue(swss::KeyOpFieldsValuesTuple(appl_db_key_3, SET_COMMAND, attributes));
  Enqueue(swss::KeyOpFieldsValuesTuple(appl_db_key_1, SET_COMMAND, attributes));
  Enqueue(swss::KeyOpFieldsValuesTuple(appl_db_key_2, DEL_COMMAND,
                                       std::vector<swss::FieldValueTuple>{}));

  sai_neighbor_entry_t neigh_entry_1;
  neigh_entry_1.switch_id = gSwitchId;
  copy(neigh_entry_1.ip_address, kNeighborId1);
  neigh_entry_1.rif_id = kRouterInterfaceOid1;
  sai_neighbor_entry_t neigh_entry_2;
  neigh_entry_2.switch_id = gSwitchId;
  copy(neigh_entry_2.ip_address, kNeighborId2);
  neigh_entry_2.rif_id = kRouterInterfaceOid2;
  sai_neighbor_entry_t neigh_entry_3;
  neigh_entry_3.switch_id = gSwitchId;
  copy(neigh_entry_3.ip_address, kNeighborId1);
  neigh_entry_3.rif_id = kRouterInterfaceOid3;

  std::vector<sai_attribute_t> attrs;
  sai_attribute_t mac_attr;
  mac_attr.id = SAI_NEIGHBOR_ENTRY_ATTR_DST_MAC_ADDRESS;
  memcpy(mac_attr.value.mac, kMacAddress2.getMac(), sizeof(sai_mac_t));
  attrs.push_back(mac_attr);
  sai_attribute_t host_route_attr;
  host_route_attr.id = SAI_NEIGHBOR_ENTRY_ATTR_NO_HOST_ROUTE;
  host_route_attr.value.booldata = true;
  attrs.push_back(host_route_attr);

  std::vector<sai_status_t> exp_status{SAI_STATUS_SUCCESS};
  EXPECT_CALL(
      mock_sai_neighbor_,
      create_neighbor_entries(
          Eq(1), EntryArrayEq(std::vector<sai_neighbor_entry_t>{neigh_entry_3}),
          ArrayEq(std::vector<uint32_t>{2}),
          AttrArrayArrayEq(std::vector<std::vector<sai_attribute_t>>{attrs}),
          Eq(SAI_BULK_OP_ERROR_MODE_STOP_ON_ERROR), _))
      .WillOnce(DoAll(SetArrayArgument<5>(exp_status.begin(), exp_status.end()),
                      Return(SAI_STATUS_SUCCESS)));

  std::vector<sai_status_t> exp_status_failure{SAI_STATUS_FAILURE};
  EXPECT_CALL(
      mock_sai_neighbor_,
      set_neighbor_entries_attribute(
          Eq(1), EntryArrayEq(std::vector<sai_neighbor_entry_t>{neigh_entry_1}),
          AttrArrayEq(std::vector<sai_attribute_t>{mac_attr}),
          Eq(SAI_BULK_OP_ERROR_MODE_STOP_ON_ERROR), _))
      .WillOnce(DoAll(SetArrayArgument<4>(exp_status_failure.begin(),
                                          exp_status_failure.end()),
                      Return(SAI_STATUS_FAILURE)));

  EXPECT_CALL(publisher_, publish(Eq(APP_P4RT_TABLE_NAME), Eq(appl_db_key_3),
                                  Eq(attributes),
                                  Eq(StatusCode::SWSS_RC_SUCCESS), Eq(true)));
  EXPECT_CALL(publisher_, publish(Eq(APP_P4RT_TABLE_NAME), Eq(appl_db_key_1),
                                  Eq(attributes),
                                  Eq(StatusCode::SWSS_RC_UNKNOWN), Eq(true)));
  EXPECT_CALL(publisher_,
              publish(Eq(APP_P4RT_TABLE_NAME), Eq(appl_db_key_2),
                      Eq(std::vector<swss::FieldValueTuple>{}),
                      Eq(StatusCode::SWSS_RC_NOT_EXECUTED), Eq(true)));
  EXPECT_EQ(StatusCode::SWSS_RC_UNKNOWN, Drain(/*failure_before=*/false));
  EXPECT_NE(nullptr, GetNeighborEntry(KeyGenerator::generateNeighborKey(
                         kRouterInterfaceId3, kNeighborId1)));
  EXPECT_NE(nullptr, GetNeighborEntry(KeyGenerator::generateNeighborKey(
                         kRouterInterfaceId1, kNeighborId1)));
  EXPECT_NE(nullptr, GetNeighborEntry(KeyGenerator::generateNeighborKey(
                         kRouterInterfaceId2, kNeighborId2)));
}

TEST_F(NeighborManagerTest, VerifyStateTest)
{
    const P4NeighborAppDbEntry app_db_entry = {
        .router_intf_id = kRouterInterfaceId1,
        .neighbor_id = kNeighborId1,
        .dst_mac_address = kMacAddress1,
        .is_set_dst_mac = true};
    AddNeighborEntry(app_db_entry, kRouterInterfaceOid1);

    // Setup ASIC DB.
    swss::Table table(nullptr, "ASIC_STATE");
    table.set("SAI_OBJECT_TYPE_NEIGHBOR_ENTRY:{\"ip\":\"10.0.0.22\",\"rif\":\"oid:"
              "0x295100\",\"switch_id\":\"oid:0x0\"}",
              std::vector<swss::FieldValueTuple>{
                  swss::FieldValueTuple{"SAI_NEIGHBOR_ENTRY_ATTR_DST_MAC_ADDRESS", "00:01:02:03:04:05"},
                  swss::FieldValueTuple{"SAI_NEIGHBOR_ENTRY_ATTR_NO_HOST_ROUTE", "true"}});

    const std::string db_key = std::string(APP_P4RT_TABLE_NAME) + kTableKeyDelimiter + APP_P4RT_NEIGHBOR_TABLE_NAME +
                               kTableKeyDelimiter + CreateNeighborAppDbKey(kRouterInterfaceId1, kNeighborId1);
    std::vector<swss::FieldValueTuple> attributes;

    // Verification should succeed with vaild key and value.
    attributes.push_back(swss::FieldValueTuple{prependParamField(p4orch::kDstMac), kMacAddress1.to_string()});
    EXPECT_EQ(VerifyState(db_key, attributes), "");

    // Invalid key should fail verification.
    EXPECT_FALSE(VerifyState("invalid", attributes).empty());
    EXPECT_FALSE(VerifyState("invalid:invalid", attributes).empty());
    EXPECT_FALSE(VerifyState(std::string(APP_P4RT_TABLE_NAME) + ":invalid", attributes).empty());
    EXPECT_FALSE(VerifyState(std::string(APP_P4RT_TABLE_NAME) + ":invalid:invalid", attributes).empty());
    EXPECT_FALSE(VerifyState(std::string(APP_P4RT_TABLE_NAME) + ":FIXED_NEIGHBOR_TABLE:invalid", attributes).empty());

    // Non-existing router intf should fail verification.
    EXPECT_FALSE(VerifyState(std::string(APP_P4RT_TABLE_NAME) + kTableKeyDelimiter + APP_P4RT_NEIGHBOR_TABLE_NAME +
                                 kTableKeyDelimiter + CreateNeighborAppDbKey(kRouterInterfaceId2, kNeighborId1),
                             attributes)
                     .empty());

    // Non-existing entry should fail verification.
    EXPECT_FALSE(VerifyState(std::string(APP_P4RT_TABLE_NAME) + kTableKeyDelimiter + APP_P4RT_NEIGHBOR_TABLE_NAME +
                                 kTableKeyDelimiter + CreateNeighborAppDbKey(kRouterInterfaceId1, kNeighborId2),
                             attributes)
                     .empty());

    auto *current_entry = GetNeighborEntry(KeyGenerator::generateNeighborKey(kRouterInterfaceId1, kNeighborId1));
    EXPECT_NE(current_entry, nullptr);

    // Verification should fail if ritf ID mismatches.
    auto saved_router_intf_id = current_entry->router_intf_id;
    current_entry->router_intf_id = kRouterInterfaceId2;
    EXPECT_FALSE(VerifyState(db_key, attributes).empty());
    current_entry->router_intf_id = saved_router_intf_id;

    // Verification should fail if neighbor ID mismatches.
    auto saved_neighbor_id = current_entry->neighbor_id;
    current_entry->neighbor_id = kNeighborId2;
    EXPECT_FALSE(VerifyState(db_key, attributes).empty());
    current_entry->neighbor_id = saved_neighbor_id;

    // Verification should fail if dest MAC mismatches.
    auto saved_dst_mac_address = current_entry->dst_mac_address;
    current_entry->dst_mac_address = kMacAddress2;
    EXPECT_FALSE(VerifyState(db_key, attributes).empty());
    current_entry->dst_mac_address = saved_dst_mac_address;

    // Verification should fail if router intf key mismatches.
    auto saved_router_intf_key = current_entry->router_intf_key;
    current_entry->router_intf_key = "invalid";
    EXPECT_FALSE(VerifyState(db_key, attributes).empty());
    current_entry->router_intf_key = saved_router_intf_key;

    // Verification should fail if neighbor key mismatches.
    auto saved_neighbor_key = current_entry->neighbor_key;
    current_entry->neighbor_key = "invalid";
    EXPECT_FALSE(VerifyState(db_key, attributes).empty());
    current_entry->neighbor_key = saved_neighbor_key;
}

TEST_F(NeighborManagerTest, VerifyStateAsicDbTest)
{
    const P4NeighborAppDbEntry app_db_entry = {
       .router_intf_id = kRouterInterfaceId1,
       .neighbor_id = kNeighborId1,
       .dst_mac_address = kMacAddress1,
       .is_set_dst_mac = true};
    AddNeighborEntry(app_db_entry, kRouterInterfaceOid1);

    // Setup ASIC DB.
    swss::Table table(nullptr, "ASIC_STATE");
    table.set("SAI_OBJECT_TYPE_NEIGHBOR_ENTRY:{\"ip\":\"10.0.0.22\",\"rif\":\"oid:"
              "0x295100\",\"switch_id\":\"oid:0x0\"}",
              std::vector<swss::FieldValueTuple>{
                  swss::FieldValueTuple{"SAI_NEIGHBOR_ENTRY_ATTR_DST_MAC_ADDRESS", "00:01:02:03:04:05"},
                  swss::FieldValueTuple{"SAI_NEIGHBOR_ENTRY_ATTR_NO_HOST_ROUTE", "true"}});

    const std::string db_key = std::string(APP_P4RT_TABLE_NAME) + kTableKeyDelimiter + APP_P4RT_NEIGHBOR_TABLE_NAME +
                               kTableKeyDelimiter + CreateNeighborAppDbKey(kRouterInterfaceId1, kNeighborId1);
    std::vector<swss::FieldValueTuple> attributes;
    attributes.push_back(swss::FieldValueTuple{prependParamField(p4orch::kDstMac), kMacAddress1.to_string()});

    // Verification should succeed with correct ASIC DB values.
    EXPECT_EQ(VerifyState(db_key, attributes), "");

    // Verification should fail if ASIC DB values mismatch.
    table.set("SAI_OBJECT_TYPE_NEIGHBOR_ENTRY:{\"ip\":\"10.0.0.22\",\"rif\":\"oid:"
              "0x295100\",\"switch_id\":\"oid:0x0\"}",
              std::vector<swss::FieldValueTuple>{
                  swss::FieldValueTuple{"SAI_NEIGHBOR_ENTRY_ATTR_DST_MAC_ADDRESS", "00:ff:ee:dd:cc:bb"}});
    EXPECT_FALSE(VerifyState(db_key, attributes).empty());

    // Verification should fail if ASIC DB table is missing.
    table.del("SAI_OBJECT_TYPE_NEIGHBOR_ENTRY:{\"ip\":\"10.0.0.22\",\"rif\":\"oid:"
              "0x295100\",\"switch_id\":\"oid:0x0\"}");
    EXPECT_FALSE(VerifyState(db_key, attributes).empty());
    table.set("SAI_OBJECT_TYPE_NEIGHBOR_ENTRY:{\"ip\":\"10.0.0.22\",\"rif\":\"oid:"
              "0x295100\",\"switch_id\":\"oid:0x0\"}",
              std::vector<swss::FieldValueTuple>{
                  swss::FieldValueTuple{"SAI_NEIGHBOR_ENTRY_ATTR_DST_MAC_ADDRESS", "00:01:02:03:04:05"},
                  swss::FieldValueTuple{"SAI_NEIGHBOR_ENTRY_ATTR_NO_HOST_ROUTE", "true"}});
}
