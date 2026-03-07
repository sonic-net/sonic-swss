#include "router_interface_manager.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <functional>
#include <string>

#include "mock_response_publisher.h"
#include "mock_sai_router_interface.h"
#include "p4orch.h"
#include "p4orch/p4orch_util.h"
#include "portsorch.h"
#include "return_code.h"
#include "swssnet.h"

using ::p4orch::kTableKeyDelimiter;

using ::testing::_;
using ::testing::DoAll;
using ::testing::Eq;
using ::testing::NotNull;
using ::testing::Return;
using ::testing::SetArgPointee;
using ::testing::SetArrayArgument;
using ::testing::StrictMock;

extern PortsOrch *gPortsOrch;

extern sai_object_id_t gSwitchId;
extern sai_object_id_t gVirtualRouterId;
extern sai_router_interface_api_t *sai_router_intfs_api;

namespace
{

constexpr char *kPortName1 = "Ethernet1";
constexpr sai_object_id_t kPortOid1 = 0x112233;
constexpr uint32_t kMtu1 = 1500;

constexpr char *kPortName2 = "Ethernet2";
constexpr sai_object_id_t kPortOid2 = 0x1fed3;
constexpr uint32_t kMtu2 = 4500;

constexpr char* kPortName10 = "Ethernet10";
constexpr sai_object_id_t kPortOid10 = 0xabcfff;
constexpr uint32_t kMtu10 = 9100;

constexpr char *kRouterInterfaceId1 = "intf-3/4";
constexpr sai_object_id_t kRouterInterfaceOid1 = 0x295100;
const swss::MacAddress kMacAddress1("00:01:02:03:04:05");

constexpr char *kRouterInterfaceId2 = "Ethernet20";
constexpr sai_object_id_t kRouterInterfaceOid2 = 0x51411;
const swss::MacAddress kMacAddress2("00:ff:ee:dd:cc:bb");

const swss::MacAddress kZeroMacAddress("00:00:00:00:00:00");

constexpr uint16_t kVlanId0 = 0;
constexpr uint16_t kVlanId1 = 0x123;
constexpr uint16_t kVlanId2 = 0x2;

constexpr char *kRouterIntfAppDbKey = R"({"match/router_interface_id":"intf-3/4"})";

bool MatchSaiAttribute(const sai_attribute_t& attr,
                       const sai_attribute_t& exp_attr) {
  if (attr.id == SAI_ROUTER_INTERFACE_ATTR_VIRTUAL_ROUTER_ID) {
    if (exp_attr.id != SAI_ROUTER_INTERFACE_ATTR_VIRTUAL_ROUTER_ID ||
        attr.value.oid != exp_attr.value.oid) {
      return false;
    }
  } else if (attr.id == SAI_ROUTER_INTERFACE_ATTR_SRC_MAC_ADDRESS) {
    if (exp_attr.id != SAI_ROUTER_INTERFACE_ATTR_SRC_MAC_ADDRESS ||
        memcmp(attr.value.mac, exp_attr.value.mac, sizeof(sai_mac_t))) {
      return false;
    }
  } else if (attr.id == SAI_ROUTER_INTERFACE_ATTR_TYPE) {
    if (exp_attr.id != SAI_ROUTER_INTERFACE_ATTR_TYPE ||
        attr.value.s32 != exp_attr.value.s32) {
      return false;
    }
  } else if (attr.id == SAI_ROUTER_INTERFACE_ATTR_PORT_ID) {
    if (exp_attr.id != SAI_ROUTER_INTERFACE_ATTR_PORT_ID ||
        attr.value.oid != exp_attr.value.oid) {
      return false;
    }
  } else if (attr.id == SAI_ROUTER_INTERFACE_ATTR_MTU) {
    if (exp_attr.id != SAI_ROUTER_INTERFACE_ATTR_MTU ||
        attr.value.u32 != exp_attr.value.u32) {
      return false;
    }
  } else if (attr.id == SAI_ROUTER_INTERFACE_ATTR_OUTER_VLAN_ID) {
    if (exp_attr.id != SAI_ROUTER_INTERFACE_ATTR_OUTER_VLAN_ID ||
        attr.value.u16 != exp_attr.value.u16) {
      return false;
    }
  } else if (attr.id == SAI_ROUTER_INTERFACE_ATTR_V4_MCAST_ENABLE) {
    if (exp_attr.id != SAI_ROUTER_INTERFACE_ATTR_V4_MCAST_ENABLE ||
        attr.value.booldata != exp_attr.value.booldata) {
      return false;
    }
  } else if (attr.id == SAI_ROUTER_INTERFACE_ATTR_V6_MCAST_ENABLE) {
    if (exp_attr.id != SAI_ROUTER_INTERFACE_ATTR_V6_MCAST_ENABLE ||
        attr.value.booldata != exp_attr.value.booldata) {
      return false;
    }
  } else {
    return false;
  }
  return true;
}

MATCHER_P(AttrEq, attr, "") { return MatchSaiAttribute(*arg, *attr); }

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

std::vector<sai_attribute_t> CreateRouterInterfaceAttributeList(
    const sai_object_id_t& virtual_router_oid,
    const swss::MacAddress mac_address, const sai_object_id_t& port_oid,
    const uint32_t mtu, const bool sub_port = false,
    const uint16_t vlan_id = 0) {
  std::vector<sai_attribute_t> attrs;
  sai_attribute_t attr;

  attr.id = SAI_ROUTER_INTERFACE_ATTR_VIRTUAL_ROUTER_ID;
  attr.value.oid = virtual_router_oid;
  attrs.push_back(attr);

  if (mac_address != kZeroMacAddress) {
    attr.id = SAI_ROUTER_INTERFACE_ATTR_SRC_MAC_ADDRESS;
    memcpy(attr.value.mac, mac_address.getMac(), sizeof(sai_mac_t));
    attrs.push_back(attr);
  }
  if (sub_port) {
    attr.value.s32 = SAI_ROUTER_INTERFACE_TYPE_SUB_PORT;
  } else {
    attr.value.s32 = SAI_ROUTER_INTERFACE_TYPE_PORT;
  }
  attr.id = SAI_ROUTER_INTERFACE_ATTR_TYPE;
  attrs.push_back(attr);

  attr.id = SAI_ROUTER_INTERFACE_ATTR_PORT_ID;
  attr.value.oid = port_oid;
  attrs.push_back(attr);

  if (sub_port) {
    attr.id = SAI_ROUTER_INTERFACE_ATTR_OUTER_VLAN_ID;
    attr.value.u16 = vlan_id;
    attrs.push_back(attr);
  }

  attr.id = SAI_ROUTER_INTERFACE_ATTR_V4_MCAST_ENABLE;
  attr.value.booldata = true;
  attrs.push_back(attr);

  attr.id = SAI_ROUTER_INTERFACE_ATTR_V6_MCAST_ENABLE;
  attr.value.booldata = true;
  attrs.push_back(attr);

  attr.id = SAI_ROUTER_INTERFACE_ATTR_MTU;
  attr.value.u32 = mtu;
  attrs.push_back(attr);

  return attrs;
}

} // namespace

class RouterInterfaceManagerTest : public ::testing::Test
{
  protected:
    RouterInterfaceManagerTest() : router_intf_manager_(&p4_oid_mapper_, &publisher_)
    {
    }

    void SetUp() override
    {
        mock_sai_router_intf = &mock_sai_router_intf_;
        sai_router_intfs_api->create_router_interface = mock_create_router_interface;
        sai_router_intfs_api->remove_router_interface = mock_remove_router_interface;
        sai_router_intfs_api->set_router_interface_attribute = mock_set_router_interface_attribute;
        sai_router_intfs_api->get_router_interface_attribute = mock_get_router_interface_attribute;
	sai_router_intfs_api->create_router_interfaces = mock_create_router_interfaces;
        sai_router_intfs_api->remove_router_interfaces = mock_remove_router_interfaces;
        sai_router_intfs_api->set_router_interfaces_attribute = mock_set_router_interfaces_attribute;
        sai_router_intfs_api->get_router_interfaces_attribute = mock_get_router_interfaces_attribute;
    }

    void Enqueue(const swss::KeyOpFieldsValuesTuple &entry)
    {
        router_intf_manager_.enqueue(APP_P4RT_ROUTER_INTERFACE_TABLE_NAME, entry);
    }

    ReturnCode Drain(bool failure_before) {
      if (failure_before) {
        router_intf_manager_.drainWithNotExecuted();
        return ReturnCode(StatusCode::SWSS_RC_NOT_EXECUTED);
      }
      return router_intf_manager_.drain();
    }

    std::string VerifyState(const std::string &key, const std::vector<swss::FieldValueTuple> &tuple)
    {
        return router_intf_manager_.verifyState(key, tuple);
    }

    ReturnCodeOr<P4RouterInterfaceAppDbEntry> DeserializeRouterIntfEntry(
        const std::string &key, const std::vector<swss::FieldValueTuple> &attributes)
    {
        return router_intf_manager_.deserializeRouterIntfEntry(key, attributes);
    }

    ReturnCode ValidateRouterInterfaceEntryOperation(
      const P4RouterInterfaceAppDbEntry& app_db_entry,
      const std::string& operation) {
    return router_intf_manager_.validateRouterInterfaceEntryOperation(
        app_db_entry, operation);
  }

  std::vector<ReturnCode> CreateRouterInterfaces(
      const std::vector<P4RouterInterfaceAppDbEntry>& router_intf_entries) {
    return router_intf_manager_.createRouterInterfaces(router_intf_entries);
  }

  std::vector<ReturnCode> RemoveRouterInterfaces(
      const std::vector<P4RouterInterfaceAppDbEntry>& router_intf_entries) {
    return router_intf_manager_.removeRouterInterfaces(router_intf_entries);
  }

  std::vector<ReturnCode> UpdateRouterInterfaces(
      const std::vector<P4RouterInterfaceAppDbEntry>& router_intf_entries) {
    return router_intf_manager_.updateRouterInterfaces(router_intf_entries);
  }

  ReturnCode ProcessEntries(
      const std::vector<P4RouterInterfaceAppDbEntry>& entries,
      const std::vector<swss::KeyOpFieldsValuesTuple>& tuple_list,
      const std::string& op, bool update) {
    return router_intf_manager_.processEntries(entries, tuple_list, op, update);
  }

    P4RouterInterfaceEntry *GetRouterInterfaceEntry(const std::string &router_intf_key)
    {
        return router_intf_manager_.getRouterInterfaceEntry(router_intf_key);
    }

    void ValidateRouterInterfaceEntry(const P4RouterInterfaceEntry &expected_entry)
    {
        const std::string router_intf_key =
            KeyGenerator::generateRouterInterfaceKey(expected_entry.router_interface_id);
        auto router_intf_entry = GetRouterInterfaceEntry(router_intf_key);

        EXPECT_NE(nullptr, router_intf_entry);
        EXPECT_EQ(expected_entry.router_interface_id, router_intf_entry->router_interface_id);
        EXPECT_EQ(expected_entry.port_name, router_intf_entry->port_name);
        EXPECT_EQ(expected_entry.src_mac_address, router_intf_entry->src_mac_address);
        EXPECT_EQ(expected_entry.router_interface_oid, router_intf_entry->router_interface_oid);

        sai_object_id_t p4_mapper_oid;
        ASSERT_TRUE(p4_oid_mapper_.getOID(SAI_OBJECT_TYPE_ROUTER_INTERFACE, router_intf_key, &p4_mapper_oid));
        EXPECT_EQ(expected_entry.router_interface_oid, p4_mapper_oid);
    }

    void ValidateRouterInterfaceEntryNotPresent(const std::string router_interface_id)
    {
        const std::string router_intf_key = KeyGenerator::generateRouterInterfaceKey(router_interface_id);
        auto current_entry = GetRouterInterfaceEntry(router_intf_key);
        EXPECT_EQ(current_entry, nullptr);
        EXPECT_FALSE(p4_oid_mapper_.existsOID(SAI_OBJECT_TYPE_ROUTER_INTERFACE, router_intf_key));
    }

    void AddRouterInterfaceEntry(
      const P4RouterInterfaceAppDbEntry& router_intf_entry,
      const sai_object_id_t ritf_oid, const sai_object_id_t port_oid,
      const uint32_t mtu, const bool sub_port = false,
      const uint16_t vlan_id = 0) {
    auto attrs = CreateRouterInterfaceAttributeList(
        gVirtualRouterId, router_intf_entry.src_mac_address, port_oid, mtu,
        sub_port, vlan_id);
    std::vector<sai_object_id_t> oids{ritf_oid};
    std::vector<sai_status_t> exp_status{SAI_STATUS_SUCCESS};
    EXPECT_CALL(
        mock_sai_router_intf_,
        create_router_interfaces(
            Eq(gSwitchId), Eq(1),
            ArrayEq(std::vector<uint32_t>{static_cast<uint32_t>(attrs.size())}),
            AttrArrayArrayEq(std::vector<std::vector<sai_attribute_t>>{attrs}),
            Eq(SAI_BULK_OP_ERROR_MODE_STOP_ON_ERROR), NotNull(), NotNull()))
        .WillOnce(
            DoAll(SetArrayArgument<5>(oids.begin(), oids.end()),
                  SetArrayArgument<6>(exp_status.begin(), exp_status.end()),
                  Return(SAI_STATUS_SUCCESS)));

    EXPECT_THAT(CreateRouterInterfaces(std::vector<P4RouterInterfaceAppDbEntry>{
                    router_intf_entry}),
                ArrayEq(std::vector<StatusCode>{StatusCode::SWSS_RC_SUCCESS}));
  }

  StrictMock<MockSaiRouterInterface> mock_sai_router_intf_;
  StrictMock<MockResponsePublisher> publisher_;
  P4OidMapper p4_oid_mapper_;
  RouterInterfaceManager router_intf_manager_;
};

TEST_F(RouterInterfaceManagerTest, CreateRouterInterfaceValidAttributes) {
  P4RouterInterfaceAppDbEntry router_intf_entry{
      .router_interface_id = kRouterInterfaceId1,
      .port_name = kPortName1,
      .src_mac_address = kMacAddress1,
      .vlan_id = kVlanId0,
      .is_set_port_name = true,
      .is_set_src_mac = true,
      .is_set_vlan_id = false,
  };
  AddRouterInterfaceEntry(router_intf_entry, kRouterInterfaceOid1, kPortOid1,
                          kMtu1);

  P4RouterInterfaceEntry entry(kRouterInterfaceId1, kPortName1, kMacAddress1,
                               kVlanId0,
                               /*has_vlan=*/false);
  entry.router_interface_oid = kRouterInterfaceOid1;
  ValidateRouterInterfaceEntry(entry);
}

TEST_F(RouterInterfaceManagerTest, CreateRouterInterfaceWithSubport) {
  P4RouterInterfaceAppDbEntry router_intf_entry{
      .router_interface_id = kRouterInterfaceId1,
      .port_name = kPortName10,
      .src_mac_address = kMacAddress1,
      .vlan_id = kVlanId0,
      .is_set_port_name = true,
      .is_set_src_mac = true,
      .is_set_vlan_id = false,
  };
  AddRouterInterfaceEntry(router_intf_entry, kRouterInterfaceOid1, kPortOid10,
                          kMtu10, true, kVlanId2);

  P4RouterInterfaceEntry entry(kRouterInterfaceId1, kPortName10, kMacAddress1,
                               kVlanId0,
                               /*has_vlan=*/false);
  entry.router_interface_oid = kRouterInterfaceOid1;
  ValidateRouterInterfaceEntry(entry);
}

TEST_F(RouterInterfaceManagerTest,
       ValidateRouterInterfaceEntryExistsInP4OidMapper) {
  const std::string router_intf_key =
      KeyGenerator::generateRouterInterfaceKey(kRouterInterfaceId2);
  p4_oid_mapper_.setOID(SAI_OBJECT_TYPE_ROUTER_INTERFACE, router_intf_key,
                        kRouterInterfaceOid2);
  P4RouterInterfaceAppDbEntry router_intf_entry{
      .router_interface_id = kRouterInterfaceId2,
      .port_name = kPortName2,
      .src_mac_address = kMacAddress2,
      .vlan_id = kVlanId0,
      .is_set_port_name = true,
      .is_set_src_mac = true,
      .is_set_vlan_id = false,
  };

  EXPECT_EQ(StatusCode::SWSS_RC_INTERNAL, ValidateRouterInterfaceEntryOperation(
                                              router_intf_entry, SET_COMMAND));
}

TEST_F(RouterInterfaceManagerTest, ValidateRouterInterfacePortNameNotSet) {
  P4RouterInterfaceAppDbEntry router_intf_entry{
      .router_interface_id = kRouterInterfaceId2,
      .port_name = "",
      .src_mac_address = kMacAddress2,
      .vlan_id = kVlanId0,
      .is_set_port_name = false,
      .is_set_src_mac = true,
      .is_set_vlan_id = false,
  };
  EXPECT_EQ(
      StatusCode::SWSS_RC_INVALID_PARAM,
      ValidateRouterInterfaceEntryOperation(router_intf_entry, SET_COMMAND));
}

TEST_F(RouterInterfaceManagerTest, CreateRouterInterfaceInvalidPort) {
  const std::string invalid_port_name = "xyz";
  P4RouterInterfaceAppDbEntry router_intf_entry{
      .router_interface_id = kRouterInterfaceId2,
      .port_name = invalid_port_name,
      .src_mac_address = kMacAddress2,
      .vlan_id = kVlanId0,
      .is_set_port_name = true,
      .is_set_src_mac = true,
      .is_set_vlan_id = false,
  };

  EXPECT_THAT(CreateRouterInterfaces(
                  std::vector<P4RouterInterfaceAppDbEntry>{router_intf_entry}),
              ArrayEq(std::vector<StatusCode>{StatusCode::SWSS_RC_NOT_FOUND}));

  ValidateRouterInterfaceEntryNotPresent(kRouterInterfaceId2);
}

TEST_F(RouterInterfaceManagerTest, CreateRouterInterfaceNoMacAddress) {
  P4RouterInterfaceAppDbEntry router_intf_entry{
      .router_interface_id = kRouterInterfaceId1,
      .port_name = kPortName1,
      .src_mac_address = kZeroMacAddress,
      .vlan_id = kVlanId0,
      .is_set_port_name = true,
      .is_set_src_mac = false,
      .is_set_vlan_id = false,
  };

  auto attrs = CreateRouterInterfaceAttributeList(
      gVirtualRouterId, router_intf_entry.src_mac_address, kPortOid1, kMtu1);
  std::vector<sai_object_id_t> oids{kRouterInterfaceOid1};
  std::vector<sai_status_t> exp_status{SAI_STATUS_SUCCESS};
  EXPECT_CALL(
      mock_sai_router_intf_,
      create_router_interfaces(
          Eq(gSwitchId), Eq(1),
          ArrayEq(std::vector<uint32_t>{static_cast<uint32_t>(attrs.size())}),
          AttrArrayArrayEq(std::vector<std::vector<sai_attribute_t>>{attrs}),
          Eq(SAI_BULK_OP_ERROR_MODE_STOP_ON_ERROR), NotNull(), NotNull()))
      .WillOnce(DoAll(SetArrayArgument<5>(oids.begin(), oids.end()),
                      SetArrayArgument<6>(exp_status.begin(), exp_status.end()),
                      Return(SAI_STATUS_SUCCESS)));
  EXPECT_THAT(CreateRouterInterfaces(
                  std::vector<P4RouterInterfaceAppDbEntry>{router_intf_entry}),
              ArrayEq(std::vector<StatusCode>{StatusCode::SWSS_RC_SUCCESS}));

  P4RouterInterfaceEntry ritf_entry;
  ritf_entry.router_interface_id = kRouterInterfaceId1;
  ritf_entry.port_name = kPortName1;
  ritf_entry.router_interface_oid = kRouterInterfaceOid1;
  ValidateRouterInterfaceEntry(ritf_entry);
}

TEST_F(RouterInterfaceManagerTest, CreateRouterInterfaceSaiApiFails) {
  P4RouterInterfaceAppDbEntry router_intf_entry{
      .router_interface_id = kRouterInterfaceId1,
      .port_name = kPortName1,
      .src_mac_address = kMacAddress1,
      .vlan_id = kVlanId0,
      .is_set_port_name = true,
      .is_set_src_mac = true,
      .is_set_vlan_id = false,
  };
  auto attrs = CreateRouterInterfaceAttributeList(
      gVirtualRouterId, router_intf_entry.src_mac_address, kPortOid1, kMtu1);
  std::vector<sai_status_t> exp_status{SAI_STATUS_FAILURE};
  EXPECT_CALL(
      mock_sai_router_intf_,
      create_router_interfaces(
          Eq(gSwitchId), Eq(1),
          ArrayEq(std::vector<uint32_t>{static_cast<uint32_t>(attrs.size())}),
          AttrArrayArrayEq(std::vector<std::vector<sai_attribute_t>>{attrs}),
          Eq(SAI_BULK_OP_ERROR_MODE_STOP_ON_ERROR), NotNull(), NotNull()))
      .WillOnce(DoAll(SetArrayArgument<6>(exp_status.begin(), exp_status.end()),
                      Return(SAI_STATUS_FAILURE)));
  EXPECT_THAT(CreateRouterInterfaces(
                  std::vector<P4RouterInterfaceAppDbEntry>{router_intf_entry}),
              ArrayEq(std::vector<StatusCode>{StatusCode::SWSS_RC_UNKNOWN}));

  ValidateRouterInterfaceEntryNotPresent(router_intf_entry.router_interface_id);
}

TEST_F(RouterInterfaceManagerTest, RemoveRouterInterfaceExistingInterface) {
  P4RouterInterfaceAppDbEntry router_intf_entry{
      .router_interface_id = kRouterInterfaceId2,
      .port_name = kPortName2,
      .src_mac_address = kMacAddress2,
      .vlan_id = kVlanId0,
      .is_set_port_name = true,
      .is_set_src_mac = true,
      .is_set_vlan_id = false,
  };
  AddRouterInterfaceEntry(router_intf_entry, kRouterInterfaceOid2, kPortOid2,
                          kMtu2);

  std::vector<sai_status_t> exp_status{SAI_STATUS_SUCCESS};
  EXPECT_CALL(
      mock_sai_router_intf_,
      remove_router_interfaces(
          Eq(1), ArrayEq(std::vector<sai_object_id_t>{kRouterInterfaceOid2}),
          Eq(SAI_BULK_OP_ERROR_MODE_STOP_ON_ERROR), NotNull()))
      .WillOnce(DoAll(SetArrayArgument<3>(exp_status.begin(), exp_status.end()),
                      Return(SAI_STATUS_SUCCESS)));
  EXPECT_THAT(RemoveRouterInterfaces(
                  std::vector<P4RouterInterfaceAppDbEntry>{router_intf_entry}),
              ArrayEq(std::vector<StatusCode>{StatusCode::SWSS_RC_SUCCESS}));

  ValidateRouterInterfaceEntryNotPresent(router_intf_entry.router_interface_id);
}

TEST_F(RouterInterfaceManagerTest,
       ValideDelRouterInterfaceNonExistingInterface) {
  P4RouterInterfaceAppDbEntry router_intf_entry{
      .router_interface_id = kRouterInterfaceId2,
      .port_name = kPortName2,
      .src_mac_address = kMacAddress2,
      .vlan_id = kVlanId0,
      .is_set_port_name = true,
      .is_set_src_mac = true,
      .is_set_vlan_id = false,
  };
  EXPECT_EQ(
      StatusCode::SWSS_RC_NOT_FOUND,
      ValidateRouterInterfaceEntryOperation(router_intf_entry, DEL_COMMAND));
}

TEST_F(RouterInterfaceManagerTest, ValideDelRouterInterfaceNonZeroRefCount) {
  P4RouterInterfaceAppDbEntry router_intf_entry{
      .router_interface_id = kRouterInterfaceId2,
      .port_name = kPortName2,
      .src_mac_address = kMacAddress2,
      .vlan_id = kVlanId0,
      .is_set_port_name = true,
      .is_set_src_mac = true,
      .is_set_vlan_id = false,
  };
  AddRouterInterfaceEntry(router_intf_entry, kRouterInterfaceOid2, kPortOid2,
                          kMtu2);

  const std::string router_intf_key = KeyGenerator::generateRouterInterfaceKey(
      router_intf_entry.router_interface_id);
  ASSERT_TRUE(p4_oid_mapper_.increaseRefCount(SAI_OBJECT_TYPE_ROUTER_INTERFACE,
                                              router_intf_key));

  EXPECT_EQ(
      StatusCode::SWSS_RC_INVALID_PARAM,
      ValidateRouterInterfaceEntryOperation(router_intf_entry, DEL_COMMAND));

  P4RouterInterfaceEntry entry(kRouterInterfaceId2, kPortName2, kMacAddress2,
                               kVlanId0,
                               /*has_vlan=*/false);
  entry.router_interface_oid = kRouterInterfaceOid2;
  ValidateRouterInterfaceEntry(entry);
}

TEST_F(RouterInterfaceManagerTest, RemoveRouterInterfaceSaiApiFails) {
  P4RouterInterfaceAppDbEntry router_intf_entry{
      .router_interface_id = kRouterInterfaceId2,
      .port_name = kPortName2,
      .src_mac_address = kMacAddress2,
      .vlan_id = kVlanId0,
      .is_set_port_name = true,
      .is_set_src_mac = true,
      .is_set_vlan_id = false,
  };
  AddRouterInterfaceEntry(router_intf_entry, kRouterInterfaceOid2, kPortOid2,
                          kMtu2);

  std::vector<sai_status_t> exp_status{SAI_STATUS_FAILURE};
  EXPECT_CALL(
      mock_sai_router_intf_,
      remove_router_interfaces(
          Eq(1), ArrayEq(std::vector<sai_object_id_t>{kRouterInterfaceOid2}),
          Eq(SAI_BULK_OP_ERROR_MODE_STOP_ON_ERROR), NotNull()))
      .WillOnce(DoAll(SetArrayArgument<3>(exp_status.begin(), exp_status.end()),
                      Return(SAI_STATUS_FAILURE)));
  EXPECT_THAT(RemoveRouterInterfaces(
                  std::vector<P4RouterInterfaceAppDbEntry>{router_intf_entry}),
              ArrayEq(std::vector<StatusCode>{StatusCode::SWSS_RC_UNKNOWN}));

  P4RouterInterfaceEntry entry(kRouterInterfaceId2, kPortName2, kMacAddress2,
                               kVlanId0,
                               /*has_vlan=*/false);
  entry.router_interface_oid = kRouterInterfaceOid2;
  ValidateRouterInterfaceEntry(entry);
}

TEST_F(RouterInterfaceManagerTest, SetSourceMacAddressModifyMacAddress) {
  P4RouterInterfaceAppDbEntry router_intf_entry{
      .router_interface_id = kRouterInterfaceId1,
      .port_name = kPortName1,
      .src_mac_address = kMacAddress1,
      .vlan_id = kVlanId0,
      .is_set_port_name = true,
      .is_set_src_mac = true,
      .is_set_vlan_id = false,
  };
  AddRouterInterfaceEntry(router_intf_entry, kRouterInterfaceOid1, kPortOid1,
                          kMtu1);

  router_intf_entry.src_mac_address = kMacAddress2;
  sai_attribute_t attr;
  attr.id = SAI_ROUTER_INTERFACE_ATTR_SRC_MAC_ADDRESS;
  memcpy(attr.value.mac, kMacAddress2.getMac(), sizeof(sai_mac_t));
  std::vector<sai_status_t> exp_status{SAI_STATUS_SUCCESS};
  EXPECT_CALL(
      mock_sai_router_intf_,
      set_router_interfaces_attribute(
          Eq(1), ArrayEq(std::vector<sai_object_id_t>{kRouterInterfaceOid1}),
          AttrArrayEq(std::vector<sai_attribute_t>{attr}),
          Eq(SAI_BULK_OP_ERROR_MODE_STOP_ON_ERROR), NotNull()))
      .WillOnce(DoAll(SetArrayArgument<4>(exp_status.begin(), exp_status.end()),
                      Return(SAI_STATUS_SUCCESS)));
  EXPECT_THAT(UpdateRouterInterfaces(
                  std::vector<P4RouterInterfaceAppDbEntry>{router_intf_entry}),
              ArrayEq(std::vector<StatusCode>{StatusCode::SWSS_RC_SUCCESS}));

  auto* current_entry =
      GetRouterInterfaceEntry(KeyGenerator::generateRouterInterfaceKey(
          router_intf_entry.router_interface_id));
  ASSERT_NE(current_entry, nullptr);
  EXPECT_EQ(current_entry->src_mac_address, kMacAddress2);
}

TEST_F(RouterInterfaceManagerTest, SetSourceMacAddressIdempotent) {
  P4RouterInterfaceAppDbEntry router_intf_entry{
      .router_interface_id = kRouterInterfaceId1,
      .port_name = kPortName1,
      .src_mac_address = kMacAddress1,
      .vlan_id = kVlanId0,
      .is_set_port_name = true,
      .is_set_src_mac = true,
      .is_set_vlan_id = false,
  };
  AddRouterInterfaceEntry(router_intf_entry, kRouterInterfaceOid1, kPortOid1,
                          kMtu1);

  // SAI API not being called makes the operation idempotent.
  EXPECT_THAT(UpdateRouterInterfaces(
                  std::vector<P4RouterInterfaceAppDbEntry>{router_intf_entry}),
              ArrayEq(std::vector<StatusCode>{StatusCode::SWSS_RC_SUCCESS}));

  auto* current_entry =
      GetRouterInterfaceEntry(KeyGenerator::generateRouterInterfaceKey(
          router_intf_entry.router_interface_id));
  ASSERT_NE(current_entry, nullptr);
  EXPECT_EQ(current_entry->src_mac_address, kMacAddress1);
}

TEST_F(RouterInterfaceManagerTest, SetSourceMacAddressSaiApiFails) {
  P4RouterInterfaceAppDbEntry router_intf_entry{
      .router_interface_id = kRouterInterfaceId1,
      .port_name = kPortName1,
      .src_mac_address = kMacAddress1,
      .vlan_id = kVlanId0,
      .is_set_port_name = true,
      .is_set_src_mac = true,
      .is_set_vlan_id = false,
  };
  AddRouterInterfaceEntry(router_intf_entry, kRouterInterfaceOid1, kPortOid1,
                          kMtu1);

  router_intf_entry.src_mac_address = kMacAddress2;
  sai_attribute_t attr;
  attr.id = SAI_ROUTER_INTERFACE_ATTR_SRC_MAC_ADDRESS;
  memcpy(attr.value.mac, kMacAddress2.getMac(), sizeof(sai_mac_t));
  std::vector<sai_status_t> exp_status{SAI_STATUS_FAILURE};
  EXPECT_CALL(
      mock_sai_router_intf_,
      set_router_interfaces_attribute(
          Eq(1), ArrayEq(std::vector<sai_object_id_t>{kRouterInterfaceOid1}),
          AttrArrayEq(std::vector<sai_attribute_t>{attr}),
          Eq(SAI_BULK_OP_ERROR_MODE_STOP_ON_ERROR), NotNull()))
      .WillOnce(DoAll(SetArrayArgument<4>(exp_status.begin(), exp_status.end()),
                      Return(SAI_STATUS_FAILURE)));
  EXPECT_THAT(UpdateRouterInterfaces(
                  std::vector<P4RouterInterfaceAppDbEntry>{router_intf_entry}),
              ArrayEq(std::vector<StatusCode>{StatusCode::SWSS_RC_UNKNOWN}));

  auto* current_entry =
      GetRouterInterfaceEntry(KeyGenerator::generateRouterInterfaceKey(
          router_intf_entry.router_interface_id));
  ASSERT_NE(current_entry, nullptr);
  EXPECT_EQ(current_entry->src_mac_address, kMacAddress1);
}

TEST_F(RouterInterfaceManagerTest, ProcessAddRequestValidAppDbParams) {
  const P4RouterInterfaceAppDbEntry app_db_entry = {
      .router_interface_id = kRouterInterfaceId1,
      .port_name = kPortName1,
      .src_mac_address = kMacAddress1,
      .is_set_port_name = true,
      .is_set_src_mac = true};

  auto attrs = CreateRouterInterfaceAttributeList(
      gVirtualRouterId, app_db_entry.src_mac_address, kPortOid1, kMtu1);
  std::vector<sai_object_id_t> oids{kRouterInterfaceOid1};
  std::vector<sai_status_t> exp_status{SAI_STATUS_SUCCESS};
  EXPECT_CALL(
      mock_sai_router_intf_,
      create_router_interfaces(
          Eq(gSwitchId), Eq(1),
          ArrayEq(std::vector<uint32_t>{static_cast<uint32_t>(attrs.size())}),
          AttrArrayArrayEq(std::vector<std::vector<sai_attribute_t>>{attrs}),
          Eq(SAI_BULK_OP_ERROR_MODE_STOP_ON_ERROR), NotNull(), NotNull()))
      .WillOnce(DoAll(SetArrayArgument<5>(oids.begin(), oids.end()),
                      SetArrayArgument<6>(exp_status.begin(), exp_status.end()),
                      Return(SAI_STATUS_SUCCESS)));

  const std::string appl_db_key =
      std::string(APP_P4RT_ROUTER_INTERFACE_TABLE_NAME) + kTableKeyDelimiter +
      std::string(kRouterIntfAppDbKey);
  std::vector<swss::FieldValueTuple> attributes;
  attributes.push_back(
      swss::FieldValueTuple{prependParamField(p4orch::kPort), kPortName1});
  attributes.push_back(swss::FieldValueTuple{prependParamField(p4orch::kSrcMac),
                                             kMacAddress1.to_string()});
  EXPECT_CALL(publisher_,
              publish(Eq(APP_P4RT_TABLE_NAME), Eq(appl_db_key), Eq(attributes),
                      Eq(StatusCode::SWSS_RC_SUCCESS), Eq(true)));
  EXPECT_EQ(
      StatusCode::SWSS_RC_SUCCESS,
      ProcessEntries(std::vector<P4RouterInterfaceAppDbEntry>{app_db_entry},
                     std::vector<swss::KeyOpFieldsValuesTuple>{
                         swss::KeyOpFieldsValuesTuple(appl_db_key, SET_COMMAND,
                                                      attributes)},
                     /*op=*/SET_COMMAND, /*update=*/false));

  P4RouterInterfaceEntry router_intf_entry(
      app_db_entry.router_interface_id, app_db_entry.port_name,
      app_db_entry.src_mac_address, kVlanId0, /*has_vlan=*/false);
  router_intf_entry.router_interface_oid = kRouterInterfaceOid1;
  ValidateRouterInterfaceEntry(router_intf_entry);
}

TEST_F(RouterInterfaceManagerTest, ProcessUpdateRequestSetSourceMacAddress) {
  P4RouterInterfaceAppDbEntry router_intf_entry{
      .router_interface_id = kRouterInterfaceId1,
      .port_name = kPortName1,
      .src_mac_address = kMacAddress1,
      .vlan_id = kVlanId0,
      .is_set_port_name = true,
      .is_set_src_mac = true,
      .is_set_vlan_id = false,
  };
  AddRouterInterfaceEntry(router_intf_entry, kRouterInterfaceOid1, kPortOid1,
                          kMtu1);

  sai_attribute_t attr;
  attr.id = SAI_ROUTER_INTERFACE_ATTR_SRC_MAC_ADDRESS;
  memcpy(attr.value.mac, kMacAddress2.getMac(), sizeof(sai_mac_t));
  std::vector<sai_status_t> exp_status{SAI_STATUS_SUCCESS};
  EXPECT_CALL(
      mock_sai_router_intf_,
      set_router_interfaces_attribute(
          Eq(1), ArrayEq(std::vector<sai_object_id_t>{kRouterInterfaceOid1}),
          AttrArrayEq(std::vector<sai_attribute_t>{attr}),
          Eq(SAI_BULK_OP_ERROR_MODE_STOP_ON_ERROR), NotNull()))
      .WillOnce(DoAll(SetArrayArgument<4>(exp_status.begin(), exp_status.end()),
                      Return(SAI_STATUS_SUCCESS)));

  const std::string appl_db_key =
      std::string(APP_P4RT_ROUTER_INTERFACE_TABLE_NAME) + kTableKeyDelimiter +
      std::string(kRouterIntfAppDbKey);
  std::vector<swss::FieldValueTuple> attributes;
  attributes.push_back(
      swss::FieldValueTuple{prependParamField(p4orch::kPort), kPortName1});
  attributes.push_back(swss::FieldValueTuple{prependParamField(p4orch::kSrcMac),
                                             kMacAddress2.to_string()});
  EXPECT_CALL(publisher_,
              publish(Eq(APP_P4RT_TABLE_NAME), Eq(appl_db_key), Eq(attributes),
                      Eq(StatusCode::SWSS_RC_SUCCESS), Eq(true)));
  router_intf_entry.src_mac_address = kMacAddress2;
  EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS,
            ProcessEntries(
                std::vector<P4RouterInterfaceAppDbEntry>{router_intf_entry},
                std::vector<swss::KeyOpFieldsValuesTuple>{
                    swss::KeyOpFieldsValuesTuple(appl_db_key, SET_COMMAND,
                                                 attributes)},
                /*op=*/SET_COMMAND, /*update=*/true));

  // Validate that router interface entry present in the Manager has the updated
  // MacAddress.
  P4RouterInterfaceEntry entry(kRouterInterfaceId1, kPortName1, kMacAddress2,
                               kVlanId0,
                               /*has_vlan=*/false);
  entry.router_interface_oid = kRouterInterfaceOid1;
  ValidateRouterInterfaceEntry(entry);
}

TEST_F(RouterInterfaceManagerTest, ProcessUpdateRequestSetPortNameIdempotent) {
  P4RouterInterfaceAppDbEntry router_intf_entry{
      .router_interface_id = kRouterInterfaceId1,
      .port_name = kPortName1,
      .src_mac_address = kMacAddress1,
      .vlan_id = kVlanId0,
      .is_set_port_name = true,
      .is_set_src_mac = true,
      .is_set_vlan_id = false,
  };
  AddRouterInterfaceEntry(router_intf_entry, kRouterInterfaceOid1, kPortOid1,
                          kMtu1);

  const std::string appl_db_key =
      std::string(APP_P4RT_ROUTER_INTERFACE_TABLE_NAME) + kTableKeyDelimiter +
      std::string(kRouterIntfAppDbKey);
  std::vector<swss::FieldValueTuple> attributes;
  attributes.push_back(
      swss::FieldValueTuple{prependParamField(p4orch::kPort), kPortName1});
  attributes.push_back(swss::FieldValueTuple{prependParamField(p4orch::kSrcMac),
                                             kMacAddress1.to_string()});
  EXPECT_CALL(publisher_,
              publish(Eq(APP_P4RT_TABLE_NAME), Eq(appl_db_key), Eq(attributes),
                      Eq(StatusCode::SWSS_RC_SUCCESS), Eq(true)));
  EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS,
            ProcessEntries(
                std::vector<P4RouterInterfaceAppDbEntry>{router_intf_entry},
                std::vector<swss::KeyOpFieldsValuesTuple>{
                    swss::KeyOpFieldsValuesTuple(appl_db_key, SET_COMMAND,
                                                 attributes)},
                /*op=*/SET_COMMAND, /*update=*/true));

  // Validate that router interface entry present in the Manager has not
  // changed.
  P4RouterInterfaceEntry entry(kRouterInterfaceId1, kPortName1, kMacAddress1,
                               kVlanId0,
                               /*has_vlan=*/false);
  entry.router_interface_oid = kRouterInterfaceOid1;
  ValidateRouterInterfaceEntry(entry);
}

TEST_F(RouterInterfaceManagerTest, ValidateUpdateRequestSetPortName) {
  P4RouterInterfaceAppDbEntry router_intf_entry{
      .router_interface_id = kRouterInterfaceId1,
      .port_name = kPortName1,
      .src_mac_address = kMacAddress1,
      .vlan_id = kVlanId0,
      .is_set_port_name = true,
      .is_set_src_mac = true,
      .is_set_vlan_id = false,
  };
  AddRouterInterfaceEntry(router_intf_entry, kRouterInterfaceOid1, kPortOid1,
                          kMtu1);

  router_intf_entry.port_name = kPortName2;
  EXPECT_EQ(
      StatusCode::SWSS_RC_UNIMPLEMENTED,
      ValidateRouterInterfaceEntryOperation(router_intf_entry, SET_COMMAND));
}

TEST_F(RouterInterfaceManagerTest, ValidateUpdateRequestMacAddrAndPort) {
  P4RouterInterfaceAppDbEntry router_intf_entry{
      .router_interface_id = kRouterInterfaceId1,
      .port_name = kPortName1,
      .src_mac_address = kMacAddress1,
      .vlan_id = kVlanId0,
      .is_set_port_name = true,
      .is_set_src_mac = true,
      .is_set_vlan_id = false,
  };
  AddRouterInterfaceEntry(router_intf_entry, kRouterInterfaceOid1, kPortOid1,
                          kMtu1);

  router_intf_entry.port_name = kPortName2;
  router_intf_entry.src_mac_address = kMacAddress2;
  EXPECT_EQ(
      StatusCode::SWSS_RC_UNIMPLEMENTED,
      ValidateRouterInterfaceEntryOperation(router_intf_entry, SET_COMMAND));
}

TEST_F(RouterInterfaceManagerTest, ValidateUpdateRequestVlanId) {
  P4RouterInterfaceAppDbEntry router_intf_entry{
      .router_interface_id = kRouterInterfaceId1,
      .port_name = kPortName1,
      .src_mac_address = kMacAddress1,
      .vlan_id = kVlanId0,
      .is_set_port_name = true,
      .is_set_src_mac = true,
      .is_set_vlan_id = false,
  };
  AddRouterInterfaceEntry(router_intf_entry, kRouterInterfaceOid1, kPortOid1,
                          kMtu1);

  router_intf_entry.vlan_id = kVlanId1;
  router_intf_entry.is_set_vlan_id = true;
  EXPECT_EQ(
      StatusCode::SWSS_RC_UNIMPLEMENTED,
      ValidateRouterInterfaceEntryOperation(router_intf_entry, SET_COMMAND));
}

TEST_F(RouterInterfaceManagerTest, ProcessDeleteRequestExistingInterface) {
  P4RouterInterfaceAppDbEntry router_intf_entry{
      .router_interface_id = kRouterInterfaceId1,
      .port_name = kPortName1,
      .src_mac_address = kMacAddress1,
      .vlan_id = kVlanId0,
      .is_set_port_name = true,
      .is_set_src_mac = true,
      .is_set_vlan_id = false,
  };
  AddRouterInterfaceEntry(router_intf_entry, kRouterInterfaceOid1, kPortOid1,
                          kMtu1);

  std::vector<sai_status_t> exp_status{SAI_STATUS_SUCCESS};
  EXPECT_CALL(
      mock_sai_router_intf_,
      remove_router_interfaces(
          Eq(1), ArrayEq(std::vector<sai_object_id_t>{kRouterInterfaceOid1}),
          Eq(SAI_BULK_OP_ERROR_MODE_STOP_ON_ERROR), NotNull()))
      .WillOnce(DoAll(SetArrayArgument<3>(exp_status.begin(), exp_status.end()),
                      Return(SAI_STATUS_SUCCESS)));

  const std::string appl_db_key =
      std::string(APP_P4RT_ROUTER_INTERFACE_TABLE_NAME) + kTableKeyDelimiter +
      std::string(kRouterIntfAppDbKey);
  std::vector<swss::FieldValueTuple> attributes;
  EXPECT_CALL(publisher_,
              publish(Eq(APP_P4RT_TABLE_NAME), Eq(appl_db_key), Eq(attributes),
                      Eq(StatusCode::SWSS_RC_SUCCESS), Eq(true)));
  EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS,
            ProcessEntries(
                std::vector<P4RouterInterfaceAppDbEntry>{router_intf_entry},
                std::vector<swss::KeyOpFieldsValuesTuple>{
                    swss::KeyOpFieldsValuesTuple(appl_db_key, DEL_COMMAND,
                                                 attributes)},
                /*op=*/DEL_COMMAND, /*update=*/false));

  ValidateRouterInterfaceEntryNotPresent(router_intf_entry.router_interface_id);
}

TEST_F(RouterInterfaceManagerTest, ValidateDeleteRequestNonExistingInterface) {
  P4RouterInterfaceAppDbEntry router_intf_entry{
      .router_interface_id = kRouterInterfaceId1,
      .port_name = kPortName1,
      .src_mac_address = kMacAddress1,
      .vlan_id = kVlanId0,
      .is_set_port_name = true,
      .is_set_src_mac = true,
      .is_set_vlan_id = false,
  };
  EXPECT_EQ(
      StatusCode::SWSS_RC_NOT_FOUND,
      ValidateRouterInterfaceEntryOperation(router_intf_entry, DEL_COMMAND));
}

TEST_F(RouterInterfaceManagerTest,
       ValidateDeleteRequestInterfaceNotExistInMapper) {
  P4RouterInterfaceAppDbEntry router_intf_entry{
      .router_interface_id = kRouterInterfaceId1,
      .port_name = kPortName1,
      .src_mac_address = kMacAddress1,
      .vlan_id = kVlanId0,
      .is_set_port_name = true,
      .is_set_src_mac = true,
      .is_set_vlan_id = false,
  };
  AddRouterInterfaceEntry(router_intf_entry, kRouterInterfaceOid1, kPortOid1,
                          kMtu1);

  p4_oid_mapper_.eraseOID(
      SAI_OBJECT_TYPE_ROUTER_INTERFACE,
      KeyGenerator::generateRouterInterfaceKey(kRouterInterfaceId1));
  EXPECT_EQ(StatusCode::SWSS_RC_INTERNAL, ValidateRouterInterfaceEntryOperation(
                                              router_intf_entry, DEL_COMMAND));
}

TEST_F(RouterInterfaceManagerTest, DeserializeRouterIntfEntryValidAttributes)
{
    const std::vector<swss::FieldValueTuple> attributes = {
        swss::FieldValueTuple(p4orch::kAction, "set_port_and_src_mac"),
        swss::FieldValueTuple(prependParamField(p4orch::kPort), kPortName1),
        swss::FieldValueTuple(prependParamField(p4orch::kSrcMac), kMacAddress1.to_string()),
    };

    auto app_db_entry_or = DeserializeRouterIntfEntry(kRouterIntfAppDbKey, attributes);
    EXPECT_TRUE(app_db_entry_or.ok());
    auto &app_db_entry = *app_db_entry_or;
    EXPECT_EQ(app_db_entry.router_interface_id, kRouterInterfaceId1);
    EXPECT_EQ(app_db_entry.port_name, kPortName1);
    EXPECT_EQ(app_db_entry.src_mac_address, kMacAddress1);
    EXPECT_TRUE(app_db_entry.is_set_port_name);
    EXPECT_TRUE(app_db_entry.is_set_src_mac);
    EXPECT_FALSE(app_db_entry.is_set_vlan_id);
}

TEST_F(RouterInterfaceManagerTest,
       DeserializeRouterIntfEntryWithVlanValidAttributes) {
  const std::vector<swss::FieldValueTuple> attributes = {
      swss::FieldValueTuple(p4orch::kAction,
                            "set_port_and_src_mac_and_vlan_id"),
      swss::FieldValueTuple(prependParamField(p4orch::kPort), kPortName1),
      swss::FieldValueTuple(prependParamField(p4orch::kSrcMac),
                            kMacAddress1.to_string()),
      swss::FieldValueTuple(prependParamField(p4orch::kVlanId), "0x123"),
  };

  auto app_db_entry_or =
      DeserializeRouterIntfEntry(kRouterIntfAppDbKey, attributes);
  EXPECT_TRUE(app_db_entry_or.ok());
  auto& app_db_entry = *app_db_entry_or;
  EXPECT_EQ(app_db_entry.router_interface_id, kRouterInterfaceId1);
  EXPECT_EQ(app_db_entry.port_name, kPortName1);
  EXPECT_EQ(app_db_entry.src_mac_address, kMacAddress1);
  EXPECT_EQ(app_db_entry.vlan_id, kVlanId1);
  EXPECT_TRUE(app_db_entry.is_set_port_name);
  EXPECT_TRUE(app_db_entry.is_set_src_mac);
  EXPECT_TRUE(app_db_entry.is_set_vlan_id);
}

TEST_F(RouterInterfaceManagerTest, DeserializeRouterIntfEntryWithInvalidVlan) {
  const std::vector<swss::FieldValueTuple> attributes = {
      swss::FieldValueTuple(p4orch::kAction,
                            "set_port_and_src_mac_and_vlan_id"),
      swss::FieldValueTuple(prependParamField(p4orch::kPort), kPortName1),
      swss::FieldValueTuple(prependParamField(p4orch::kSrcMac),
                            kMacAddress1.to_string()),
      swss::FieldValueTuple(prependParamField(p4orch::kVlanId), "invalid"),
  };

  auto app_db_entry_or =
      DeserializeRouterIntfEntry(kRouterIntfAppDbKey, attributes);
  EXPECT_FALSE(app_db_entry_or.ok());
}

TEST_F(RouterInterfaceManagerTest, DeserializeRouterIntfEntryInvalidKeyFormat)
{
    const std::vector<swss::FieldValueTuple> attributes = {
        swss::FieldValueTuple(p4orch::kAction, "set_port_and_src_mac"),
        swss::FieldValueTuple(prependParamField(p4orch::kPort), kPortName1),
        swss::FieldValueTuple(prependParamField(p4orch::kSrcMac), kMacAddress1.to_string()),
    };

    // Invalid json format.
    std::string invalid_key = R"({"match/router_interface_id:intf-3/4"})";
    auto app_db_entry_or = DeserializeRouterIntfEntry(invalid_key, attributes);
    EXPECT_FALSE(app_db_entry_or.ok());

    // Invalid json format.
    invalid_key = R"([{"match/router_interface_id":"intf-3/4"}])";
    app_db_entry_or = DeserializeRouterIntfEntry(invalid_key, attributes);
    EXPECT_FALSE(app_db_entry_or.ok());

    // Invalid json format.
    invalid_key = R"(["match/router_interface_id","intf-3/4"])";
    app_db_entry_or = DeserializeRouterIntfEntry(invalid_key, attributes);
    EXPECT_FALSE(app_db_entry_or.ok());

    // Invalid field name.
    invalid_key = R"({"router_interface_id":"intf-3/4"})";
    app_db_entry_or = DeserializeRouterIntfEntry(invalid_key, attributes);
    EXPECT_FALSE(app_db_entry_or.ok());
}

TEST_F(RouterInterfaceManagerTest, DeserializeRouterIntfEntryMissingAction)
{
    const std::vector<swss::FieldValueTuple> attributes = {
        swss::FieldValueTuple(prependParamField(p4orch::kPort), kPortName1),
        swss::FieldValueTuple(prependParamField(p4orch::kSrcMac), kMacAddress1.to_string()),
    };

    auto app_db_entry_or = DeserializeRouterIntfEntry(kRouterIntfAppDbKey, attributes);
    EXPECT_TRUE(app_db_entry_or.ok());
    auto &app_db_entry = *app_db_entry_or;
    EXPECT_EQ(app_db_entry.router_interface_id, kRouterInterfaceId1);
    EXPECT_EQ(app_db_entry.port_name, kPortName1);
    EXPECT_EQ(app_db_entry.src_mac_address, kMacAddress1);
    EXPECT_TRUE(app_db_entry.is_set_port_name);
    EXPECT_TRUE(app_db_entry.is_set_src_mac);
}

TEST_F(RouterInterfaceManagerTest, DeserializeRouterIntfEntryOnlyPortNameAttribute)
{
    const std::vector<swss::FieldValueTuple> attributes = {
        swss::FieldValueTuple(prependParamField(p4orch::kPort), kPortName1)};

    auto app_db_entry_or = DeserializeRouterIntfEntry(kRouterIntfAppDbKey, attributes);
    EXPECT_TRUE(app_db_entry_or.ok());
    auto &app_db_entry = *app_db_entry_or;
    EXPECT_EQ(app_db_entry.router_interface_id, kRouterInterfaceId1);
    EXPECT_EQ(app_db_entry.port_name, kPortName1);
    EXPECT_EQ(app_db_entry.src_mac_address, kZeroMacAddress);
    EXPECT_TRUE(app_db_entry.is_set_port_name);
    EXPECT_FALSE(app_db_entry.is_set_src_mac);
}

TEST_F(RouterInterfaceManagerTest, DeserializeRouterIntfEntryOnlyMacAddrAttribute)
{
    const std::vector<swss::FieldValueTuple> attributes = {
        swss::FieldValueTuple(prependParamField(p4orch::kSrcMac), kMacAddress1.to_string())};

    auto app_db_entry_or = DeserializeRouterIntfEntry(kRouterIntfAppDbKey, attributes);
    EXPECT_TRUE(app_db_entry_or.ok());
    auto &app_db_entry = *app_db_entry_or;
    EXPECT_EQ(app_db_entry.router_interface_id, kRouterInterfaceId1);
    EXPECT_EQ(app_db_entry.port_name, "");
    EXPECT_EQ(app_db_entry.src_mac_address, kMacAddress1);
    EXPECT_FALSE(app_db_entry.is_set_port_name);
    EXPECT_TRUE(app_db_entry.is_set_src_mac);
}

TEST_F(RouterInterfaceManagerTest, DeserializeRouterIntfEntryNoAttributes)
{
    const std::vector<swss::FieldValueTuple> attributes;

    auto app_db_entry_or = DeserializeRouterIntfEntry(kRouterIntfAppDbKey, attributes);
    EXPECT_TRUE(app_db_entry_or.ok());
    auto &app_db_entry = *app_db_entry_or;
    EXPECT_EQ(app_db_entry.router_interface_id, kRouterInterfaceId1);
    EXPECT_EQ(app_db_entry.port_name, "");
    EXPECT_EQ(app_db_entry.src_mac_address, kZeroMacAddress);
    EXPECT_FALSE(app_db_entry.is_set_port_name);
    EXPECT_FALSE(app_db_entry.is_set_src_mac);
}

TEST_F(RouterInterfaceManagerTest, DeserializeRouterIntfEntryInvalidField)
{
    const std::vector<swss::FieldValueTuple> attributes = {swss::FieldValueTuple("invalid_field", "invalid_value")};

    auto app_db_entry_or = DeserializeRouterIntfEntry(kRouterIntfAppDbKey, attributes);
    EXPECT_FALSE(app_db_entry_or.ok());
}

TEST_F(RouterInterfaceManagerTest, DeserializeRouterIntfEntryInvalidMacAddrValue)
{
    const std::vector<swss::FieldValueTuple> attributes = {
        swss::FieldValueTuple(prependParamField(p4orch::kSrcMac), "00:11:22:33:44")};

    auto app_db_entry_or = DeserializeRouterIntfEntry(kRouterIntfAppDbKey, attributes);
    EXPECT_FALSE(app_db_entry_or.ok());
}

TEST_F(RouterInterfaceManagerTest, DrainValidAttributes)
{
    const std::string appl_db_key =
        std::string(APP_P4RT_ROUTER_INTERFACE_TABLE_NAME) + kTableKeyDelimiter + std::string(kRouterIntfAppDbKey);

    // Enqueue entry for create operation.
    std::vector<swss::FieldValueTuple> attributes;
    attributes.push_back(swss::FieldValueTuple{prependParamField(p4orch::kPort), kPortName1});
    attributes.push_back(swss::FieldValueTuple{prependParamField(p4orch::kSrcMac), kMacAddress1.to_string()});
    Enqueue(swss::KeyOpFieldsValuesTuple(appl_db_key, SET_COMMAND, attributes));
    std::vector<sai_object_id_t> oids{kRouterInterfaceOid1};
    std::vector<sai_status_t> exp_status{SAI_STATUS_SUCCESS};
    EXPECT_CALL(mock_sai_router_intf_,
              create_router_interfaces(_, _, _, _, _, _, _))
      .WillOnce(DoAll(SetArrayArgument<5>(oids.begin(), oids.end()),
                      SetArrayArgument<6>(exp_status.begin(), exp_status.end()),
                      Return(SAI_STATUS_SUCCESS)));
    EXPECT_CALL(publisher_,
              publish(Eq(APP_P4RT_TABLE_NAME), Eq(appl_db_key), Eq(attributes),
                      Eq(StatusCode::SWSS_RC_SUCCESS), Eq(true)));
    EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS, Drain(/*failure_before=*/false));

    P4RouterInterfaceEntry router_intf_entry(kRouterInterfaceId1, kPortName1,
                                             kMacAddress1, kVlanId0,
                                             /*has_vlan=*/false);
    router_intf_entry.router_interface_oid = kRouterInterfaceOid1;
    ValidateRouterInterfaceEntry(router_intf_entry);

    // Enqueue entry for update operation.
    attributes.clear();
    attributes.push_back(swss::FieldValueTuple{prependParamField(p4orch::kSrcMac), kMacAddress2.to_string()});
    Enqueue(swss::KeyOpFieldsValuesTuple(appl_db_key, SET_COMMAND, attributes));

    EXPECT_CALL(mock_sai_router_intf_,
              set_router_interfaces_attribute(_, _, _, _, _))
      .WillOnce(DoAll(SetArrayArgument<4>(exp_status.begin(), exp_status.end()),
                      Return(SAI_STATUS_SUCCESS)));
    EXPECT_CALL(publisher_, publish(Eq(APP_P4RT_TABLE_NAME), Eq(appl_db_key),
                                    Eq(attributes),
                                    Eq(StatusCode::SWSS_RC_SUCCESS), Eq(true)));
    EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS, Drain(/*failure_before=*/false));

    router_intf_entry.src_mac_address = kMacAddress2;
    ValidateRouterInterfaceEntry(router_intf_entry);

    // Enqueue entry for delete operation.
    attributes.clear();
    Enqueue(swss::KeyOpFieldsValuesTuple(appl_db_key, DEL_COMMAND, attributes));

    EXPECT_CALL(mock_sai_router_intf_, remove_router_interfaces(_, _, _, _))
      .WillOnce(DoAll(SetArrayArgument<3>(exp_status.begin(), exp_status.end()),
                      Return(SAI_STATUS_SUCCESS)));
    EXPECT_CALL(publisher_, publish(Eq(APP_P4RT_TABLE_NAME), Eq(appl_db_key),
                                    Eq(attributes),
                                    Eq(StatusCode::SWSS_RC_SUCCESS), Eq(true)));
    EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS, Drain(/*failure_before=*/false));

    ValidateRouterInterfaceEntryNotPresent(router_intf_entry.router_interface_id);
}

TEST_F(RouterInterfaceManagerTest, DrainValidAttributesWithVlan) {
  const std::string appl_db_key =
      std::string(APP_P4RT_ROUTER_INTERFACE_TABLE_NAME) + kTableKeyDelimiter +
      std::string(kRouterIntfAppDbKey);

  // Enqueue entry for create operation.
  std::vector<swss::FieldValueTuple> attributes;
  attributes.push_back(
      swss::FieldValueTuple{prependParamField(p4orch::kPort), kPortName1});
  attributes.push_back(swss::FieldValueTuple{prependParamField(p4orch::kSrcMac),
                                             kMacAddress1.to_string()});
  attributes.push_back(
      swss::FieldValueTuple{prependParamField(p4orch::kVlanId), "0x123"});
  Enqueue(swss::KeyOpFieldsValuesTuple(appl_db_key, SET_COMMAND, attributes));

  std::vector<sai_object_id_t> oids{kRouterInterfaceOid1};
  std::vector<sai_status_t> exp_status{SAI_STATUS_SUCCESS};
  EXPECT_CALL(mock_sai_router_intf_,
              create_router_interfaces(_, _, _, _, _, _, _))
      .WillOnce(DoAll(SetArrayArgument<5>(oids.begin(), oids.end()),
                      SetArrayArgument<6>(exp_status.begin(), exp_status.end()),
                      Return(SAI_STATUS_SUCCESS)));

  EXPECT_CALL(publisher_,
              publish(Eq(APP_P4RT_TABLE_NAME), Eq(appl_db_key), Eq(attributes),
                      Eq(StatusCode::SWSS_RC_SUCCESS), Eq(true)));
  EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS, Drain(/*failure_before=*/false));

  P4RouterInterfaceEntry router_intf_entry(kRouterInterfaceId1, kPortName1,
                                           kMacAddress1, kVlanId1,
                                           /*has_vlan=*/true);
  router_intf_entry.router_interface_oid = kRouterInterfaceOid1;
  ValidateRouterInterfaceEntry(router_intf_entry);

  // Enqueue entry for update operation, but don't change VLAN ID.
  attributes.clear();
  attributes.push_back(
      swss::FieldValueTuple{prependParamField(p4orch::kPort), kPortName1});
  attributes.push_back(swss::FieldValueTuple{prependParamField(p4orch::kSrcMac),
                                             kMacAddress2.to_string()});
  attributes.push_back(
      swss::FieldValueTuple{prependParamField(p4orch::kVlanId), "0x123"});
  Enqueue(swss::KeyOpFieldsValuesTuple(appl_db_key, SET_COMMAND, attributes));

  EXPECT_CALL(mock_sai_router_intf_,
              set_router_interfaces_attribute(_, _, _, _, _))
      .WillOnce(DoAll(SetArrayArgument<4>(exp_status.begin(), exp_status.end()),
                      Return(SAI_STATUS_SUCCESS)));
  EXPECT_CALL(publisher_,
              publish(Eq(APP_P4RT_TABLE_NAME), Eq(appl_db_key), Eq(attributes),
                      Eq(StatusCode::SWSS_RC_SUCCESS), Eq(true)));
  EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS, Drain(/*failure_before=*/false));

  router_intf_entry.src_mac_address = kMacAddress2;
  ValidateRouterInterfaceEntry(router_intf_entry);

  // Enqueue entry for delete operation.
  attributes.clear();
  Enqueue(swss::KeyOpFieldsValuesTuple(appl_db_key, DEL_COMMAND, attributes));

  EXPECT_CALL(mock_sai_router_intf_, remove_router_interfaces(_, _, _, _))
      .WillOnce(DoAll(SetArrayArgument<3>(exp_status.begin(), exp_status.end()),
                      Return(SAI_STATUS_SUCCESS)));
  EXPECT_CALL(publisher_,
              publish(Eq(APP_P4RT_TABLE_NAME), Eq(appl_db_key), Eq(attributes),
                      Eq(StatusCode::SWSS_RC_SUCCESS), Eq(true)));
  EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS, Drain(/*failure_before=*/false));

  ValidateRouterInterfaceEntryNotPresent(router_intf_entry.router_interface_id);
}

TEST_F(RouterInterfaceManagerTest, DrainInvalidAttributesWithVlan) {
  const std::string appl_db_key =
      std::string(APP_P4RT_ROUTER_INTERFACE_TABLE_NAME) + kTableKeyDelimiter +
      std::string(kRouterIntfAppDbKey);

  // Enqueue entry for create operation.
  std::vector<swss::FieldValueTuple> attributes;
  attributes.push_back(
      swss::FieldValueTuple{prependParamField(p4orch::kPort), kPortName1});
  // Missing src mac should result in failure.
  attributes.push_back(
      swss::FieldValueTuple{prependParamField(p4orch::kVlanId), "0x123"});
  Enqueue(swss::KeyOpFieldsValuesTuple(appl_db_key, SET_COMMAND, attributes));

  EXPECT_CALL(publisher_,
              publish(Eq(APP_P4RT_TABLE_NAME), Eq(appl_db_key), Eq(attributes),
                      Eq(StatusCode::SWSS_RC_INVALID_PARAM), Eq(true)));

  EXPECT_EQ(StatusCode::SWSS_RC_INVALID_PARAM, Drain(/*failure_before=*/false));
}

TEST_F(RouterInterfaceManagerTest, DrainInvalidAppDbEntryKey)
{
    // Create invalid json key with router interface id as kRouterInterfaceId1.
    const std::string invalid_router_intf_key = R"({"match/router_interface_id:intf-3/4"})";
    const std::string appl_db_key =
        std::string(APP_P4RT_ROUTER_INTERFACE_TABLE_NAME) + kTableKeyDelimiter + invalid_router_intf_key;

    // Enqueue entry for create operation.
    std::vector<swss::FieldValueTuple> attributes;
    Enqueue(swss::KeyOpFieldsValuesTuple(appl_db_key, SET_COMMAND, attributes));
    EXPECT_CALL(
        publisher_,
        publish(Eq(APP_P4RT_TABLE_NAME), Eq(appl_db_key), Eq(attributes),
                Eq(StatusCode::SWSS_RC_INVALID_PARAM), Eq(true)));
    EXPECT_EQ(StatusCode::SWSS_RC_INVALID_PARAM,
              Drain(/*failure_before=*/false));

    ValidateRouterInterfaceEntryNotPresent(kRouterInterfaceId1);
}

TEST_F(RouterInterfaceManagerTest, DrainInvalidAppDbEntryAttributes)
{
    const std::string appl_db_key =
        std::string(APP_P4RT_ROUTER_INTERFACE_TABLE_NAME) + kTableKeyDelimiter + std::string(kRouterIntfAppDbKey);

    // Invalid port attribute.
    std::vector<swss::FieldValueTuple> attributes;
    attributes.push_back(swss::FieldValueTuple{prependParamField(p4orch::kPort), "xyz"});
    Enqueue(swss::KeyOpFieldsValuesTuple(appl_db_key, SET_COMMAND, attributes));
    EXPECT_CALL(
        publisher_,
        publish(Eq(APP_P4RT_TABLE_NAME), Eq(appl_db_key), Eq(attributes),
                Eq(StatusCode::SWSS_RC_NOT_FOUND), Eq(true)));
    EXPECT_EQ(StatusCode::SWSS_RC_NOT_FOUND, Drain(/*failure_before=*/false));
    ValidateRouterInterfaceEntryNotPresent(kRouterInterfaceId1);

    // Zero mac address attribute.
    attributes.clear();
    attributes.push_back(swss::FieldValueTuple{prependParamField(p4orch::kPort), kPortName1});
    attributes.push_back(swss::FieldValueTuple{prependParamField(p4orch::kSrcMac), kZeroMacAddress.to_string()});
    Enqueue(swss::KeyOpFieldsValuesTuple(appl_db_key, SET_COMMAND, attributes));
    EXPECT_CALL(
        publisher_,
        publish(Eq(APP_P4RT_TABLE_NAME), Eq(appl_db_key), Eq(attributes),
                Eq(StatusCode::SWSS_RC_INVALID_PARAM), Eq(true)));
    EXPECT_EQ(StatusCode::SWSS_RC_INVALID_PARAM,
              Drain(/*failure_before=*/false));
    ValidateRouterInterfaceEntryNotPresent(kRouterInterfaceId1);
}

TEST_F(RouterInterfaceManagerTest, DrainInvalidOperation)
{
    const std::string appl_db_key =
        std::string(APP_P4RT_ROUTER_INTERFACE_TABLE_NAME) + kTableKeyDelimiter + std::string(kRouterIntfAppDbKey);

    // Enqueue entry for invalid operation.
    std::vector<swss::FieldValueTuple> attributes;
    attributes.push_back(swss::FieldValueTuple{prependParamField(p4orch::kPort), kPortName1});
    attributes.push_back(swss::FieldValueTuple{prependParamField(p4orch::kSrcMac), kMacAddress1.to_string()});
    Enqueue(swss::KeyOpFieldsValuesTuple(appl_db_key, "INVALID", attributes));
    EXPECT_CALL(
        publisher_,
        publish(Eq(APP_P4RT_TABLE_NAME), Eq(appl_db_key), Eq(attributes),
                Eq(StatusCode::SWSS_RC_INVALID_PARAM), Eq(true)));
    EXPECT_EQ(StatusCode::SWSS_RC_INVALID_PARAM,
              Drain(/*failure_before=*/false));

    ValidateRouterInterfaceEntryNotPresent(kRouterInterfaceId1);
}

TEST_F(RouterInterfaceManagerTest, DrainNotExecuted) {
  std::vector<swss::FieldValueTuple> attributes;
  attributes.push_back(
      swss::FieldValueTuple{prependParamField(p4orch::kPort), kPortName1});
  attributes.push_back(swss::FieldValueTuple{prependParamField(p4orch::kSrcMac),
                                             kMacAddress1.to_string()});

  const std::string appl_db_key_1 =
      std::string(APP_P4RT_ROUTER_INTERFACE_TABLE_NAME) + kTableKeyDelimiter +
      "{\"match/router_interface_id\":\"intf-3/4\"}";
  const std::string appl_db_key_2 =
      std::string(APP_P4RT_ROUTER_INTERFACE_TABLE_NAME) + kTableKeyDelimiter +
      "{\"match/router_interface_id\":\"intf-3/5\"}";
  const std::string appl_db_key_3 =
      std::string(APP_P4RT_ROUTER_INTERFACE_TABLE_NAME) + kTableKeyDelimiter +
      "{\"match/router_interface_id\":\"intf-3/6\"}";

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
  EXPECT_EQ(nullptr, GetRouterInterfaceEntry(
                         KeyGenerator::generateRouterInterfaceKey("intf-3/4")));
  EXPECT_EQ(nullptr, GetRouterInterfaceEntry(
                         KeyGenerator::generateRouterInterfaceKey("intf-3/5")));
  EXPECT_EQ(nullptr, GetRouterInterfaceEntry(
                         KeyGenerator::generateRouterInterfaceKey("intf-3/6")));
}

TEST_F(RouterInterfaceManagerTest, DrainStopOnFirstFailure) {
  std::vector<swss::FieldValueTuple> attributes;
  attributes.push_back(
      swss::FieldValueTuple{prependParamField(p4orch::kPort), kPortName1});
  attributes.push_back(swss::FieldValueTuple{prependParamField(p4orch::kSrcMac),
                                             kMacAddress1.to_string()});

  const std::string appl_db_key_1 =
      std::string(APP_P4RT_ROUTER_INTERFACE_TABLE_NAME) + kTableKeyDelimiter +
      "{\"match/router_interface_id\":\"intf-3/4\"}";
  const std::string appl_db_key_2 =
      std::string(APP_P4RT_ROUTER_INTERFACE_TABLE_NAME) + kTableKeyDelimiter +
      "{\"match/router_interface_id\":\"intf-3/5\"}";
  const std::string appl_db_key_3 =
      std::string(APP_P4RT_ROUTER_INTERFACE_TABLE_NAME) + kTableKeyDelimiter +
      "{\"match/router_interface_id\":\"intf-3/6\"}";

  Enqueue(swss::KeyOpFieldsValuesTuple(appl_db_key_1, SET_COMMAND, attributes));
  Enqueue(swss::KeyOpFieldsValuesTuple(appl_db_key_2, SET_COMMAND, attributes));
  Enqueue(swss::KeyOpFieldsValuesTuple(appl_db_key_3, SET_COMMAND, attributes));

  std::vector<sai_object_id_t> oids{kRouterInterfaceOid1, kRouterInterfaceOid2,
                                    kRouterInterfaceOid2};
  std::vector<sai_status_t> exp_status{SAI_STATUS_SUCCESS, SAI_STATUS_FAILURE,
                                       SAI_STATUS_NOT_EXECUTED};
  EXPECT_CALL(mock_sai_router_intf_,
              create_router_interfaces(_, _, _, _, _, _, _))
      .WillOnce(DoAll(SetArrayArgument<5>(oids.begin(), oids.end()),
                      SetArrayArgument<6>(exp_status.begin(), exp_status.end()),
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
  EXPECT_NE(nullptr, GetRouterInterfaceEntry(
                         KeyGenerator::generateRouterInterfaceKey("intf-3/4")));
  EXPECT_EQ(nullptr, GetRouterInterfaceEntry(
                         KeyGenerator::generateRouterInterfaceKey("intf-3/5")));
  EXPECT_EQ(nullptr, GetRouterInterfaceEntry(
                         KeyGenerator::generateRouterInterfaceKey("intf-3/6")));
}

TEST_F(RouterInterfaceManagerTest, VerifyStateTest) {
  P4RouterInterfaceAppDbEntry router_intf_entry{
      .router_interface_id = kRouterInterfaceId1,
      .port_name = kPortName1,
      .src_mac_address = kMacAddress1,
      .vlan_id = kVlanId0,
      .is_set_port_name = true,
      .is_set_src_mac = true,
      .is_set_vlan_id = false,
  };
  AddRouterInterfaceEntry(router_intf_entry, kRouterInterfaceOid1, kPortOid1,
                          kMtu1);

    // Setup ASIC DB.
    swss::Table table(nullptr, "ASIC_STATE");
    table.set("SAI_OBJECT_TYPE_ROUTER_INTERFACE:oid:0x295100",
              std::vector<swss::FieldValueTuple>{
                  swss::FieldValueTuple{"SAI_ROUTER_INTERFACE_ATTR_VIRTUAL_ROUTER_ID", "oid:0x0"},
                  swss::FieldValueTuple{"SAI_ROUTER_INTERFACE_ATTR_SRC_MAC_ADDRESS", "00:01:02:03:04:05"},
                  swss::FieldValueTuple{"SAI_ROUTER_INTERFACE_ATTR_TYPE", "SAI_ROUTER_INTERFACE_TYPE_PORT"},
                  swss::FieldValueTuple{"SAI_ROUTER_INTERFACE_ATTR_PORT_ID", "oid:0x112233"},
                  swss::FieldValueTuple{"SAI_ROUTER_INTERFACE_ATTR_MTU", "1500"}});

    const std::string db_key = std::string(APP_P4RT_TABLE_NAME) + kTableKeyDelimiter +
                               APP_P4RT_ROUTER_INTERFACE_TABLE_NAME + kTableKeyDelimiter + kRouterIntfAppDbKey;
    std::vector<swss::FieldValueTuple> attributes;

    // Verification should succeed with vaild key and value.
    attributes.push_back(swss::FieldValueTuple{prependParamField(p4orch::kPort), kPortName1});
    attributes.push_back(swss::FieldValueTuple{prependParamField(p4orch::kSrcMac), kMacAddress1.to_string()});
    EXPECT_EQ(VerifyState(db_key, attributes), "");

    // Invalid key should fail verification.
    EXPECT_FALSE(VerifyState("invalid", attributes).empty());
    EXPECT_FALSE(VerifyState("invalid:invalid", attributes).empty());
    EXPECT_FALSE(VerifyState(std::string(APP_P4RT_TABLE_NAME) + ":invalid", attributes).empty());
    EXPECT_FALSE(VerifyState(std::string(APP_P4RT_TABLE_NAME) + ":invalid:invalid", attributes).empty());
    EXPECT_FALSE(
        VerifyState(std::string(APP_P4RT_TABLE_NAME) + ":FIXED_ROUTER_INTERFACE_TABLE:invalid", attributes).empty());
    EXPECT_FALSE(VerifyState(std::string(APP_P4RT_TABLE_NAME) + ":FIXED_ROUTER_INTERFACE_TABLE:{\"match/"
                                                                "router_interface_id\":\"invalid\"}",
                             attributes)
                     .empty());

    // Invalid attributes should fail verification.
    attributes.clear();
    attributes.push_back(swss::FieldValueTuple{prependParamField(p4orch::kPort), kPortName2});
    attributes.push_back(swss::FieldValueTuple{prependParamField(p4orch::kSrcMac), kMacAddress1.to_string()});
    EXPECT_FALSE(VerifyState(db_key, attributes).empty());

    attributes.clear();
    attributes.push_back(swss::FieldValueTuple{prependParamField(p4orch::kPort), kPortName1});
    attributes.push_back(swss::FieldValueTuple{prependParamField(p4orch::kSrcMac), kMacAddress2.to_string()});
    EXPECT_FALSE(VerifyState(db_key, attributes).empty());

    // Invalid port should fail verification.
    attributes.clear();
    attributes.push_back(swss::FieldValueTuple{prependParamField(p4orch::kPort), "invalid"});
    attributes.push_back(swss::FieldValueTuple{prependParamField(p4orch::kSrcMac), kMacAddress1.to_string()});
    EXPECT_FALSE(VerifyState(db_key, attributes).empty());

    // Verification should fail if interface IDs mismatch.
    attributes.clear();
    attributes.push_back(swss::FieldValueTuple{prependParamField(p4orch::kPort), kPortName1});
    attributes.push_back(swss::FieldValueTuple{prependParamField(p4orch::kSrcMac), kMacAddress1.to_string()});
    auto *router_intf_entry_ptr =
        GetRouterInterfaceEntry(KeyGenerator::generateRouterInterfaceKey(router_intf_entry.router_interface_id));
    auto saved_ritf_id = router_intf_entry_ptr->router_interface_id;
    router_intf_entry_ptr->router_interface_id = "invalid";
    EXPECT_FALSE(VerifyState(db_key, attributes).empty());
    router_intf_entry_ptr->router_interface_id = saved_ritf_id;

    // Verification should fail if OID mapper mismatches.
    p4_oid_mapper_.eraseOID(SAI_OBJECT_TYPE_ROUTER_INTERFACE,
                            KeyGenerator::generateRouterInterfaceKey(router_intf_entry.router_interface_id));
    EXPECT_FALSE(VerifyState(db_key, attributes).empty());
}

TEST_F(RouterInterfaceManagerTest, VerifyStateWithVlanTest) {
  P4RouterInterfaceAppDbEntry router_intf_entry{
      .router_interface_id = kRouterInterfaceId1,
      .port_name = kPortName1,
      .src_mac_address = kMacAddress1,
      .vlan_id = kVlanId1,
      .is_set_port_name = true,
      .is_set_src_mac = true,
      .is_set_vlan_id = true,
  };
  AddRouterInterfaceEntry(router_intf_entry, kRouterInterfaceOid1, kPortOid1,
                          kMtu1, /*subport=*/true, kVlanId1);

  // Setup ASIC DB.
  swss::Table table(nullptr, "ASIC_STATE");
  table.set(
      "SAI_OBJECT_TYPE_ROUTER_INTERFACE:oid:0x295100",
      std::vector<swss::FieldValueTuple>{
          swss::FieldValueTuple{"SAI_ROUTER_INTERFACE_ATTR_VIRTUAL_ROUTER_ID",
                                "oid:0x0"},
          swss::FieldValueTuple{"SAI_ROUTER_INTERFACE_ATTR_SRC_MAC_ADDRESS",
                                "00:01:02:03:04:05"},
          swss::FieldValueTuple{"SAI_ROUTER_INTERFACE_ATTR_TYPE",
                                "SAI_ROUTER_INTERFACE_TYPE_SUB_PORT"},
          swss::FieldValueTuple{"SAI_ROUTER_INTERFACE_ATTR_OUTER_VLAN_ID",
                                "291"},
          swss::FieldValueTuple{"SAI_ROUTER_INTERFACE_ATTR_PORT_ID",
                                "oid:0x112233"},
          swss::FieldValueTuple{"SAI_ROUTER_INTERFACE_ATTR_V4_MCAST_ENABLE",
                                "true"},
          swss::FieldValueTuple{"SAI_ROUTER_INTERFACE_ATTR_V6_MCAST_ENABLE",
                                "true"},
          swss::FieldValueTuple{"SAI_ROUTER_INTERFACE_ATTR_MTU", "1500"}});

  const std::string db_key = std::string(APP_P4RT_TABLE_NAME) +
                             kTableKeyDelimiter +
                             APP_P4RT_ROUTER_INTERFACE_TABLE_NAME +
                             kTableKeyDelimiter + kRouterIntfAppDbKey;
  std::vector<swss::FieldValueTuple> attributes;

  // Verification should succeed with vaild key and value.
  attributes.push_back(
      swss::FieldValueTuple{prependParamField(p4orch::kPort), kPortName1});
  attributes.push_back(swss::FieldValueTuple{prependParamField(p4orch::kSrcMac),
                                             kMacAddress1.to_string()});
  attributes.push_back(
      swss::FieldValueTuple{prependParamField(p4orch::kVlanId), "0x123"});
  EXPECT_EQ(VerifyState(db_key, attributes), "");

  // Verification should fail if ASIC DB table has a mismatch on VLAN.
  table.del("SAI_OBJECT_TYPE_ROUTER_INTERFACE:oid:0x295100");
  table.set(
      "SAI_OBJECT_TYPE_ROUTER_INTERFACE:oid:0x295100",
      std::vector<swss::FieldValueTuple>{
          swss::FieldValueTuple{"SAI_ROUTER_INTERFACE_ATTR_VIRTUAL_ROUTER_ID",
                                "oid:0x0"},
          swss::FieldValueTuple{"SAI_ROUTER_INTERFACE_ATTR_SRC_MAC_ADDRESS",
                                "00:01:02:03:04:05"},
          swss::FieldValueTuple{"SAI_ROUTER_INTERFACE_ATTR_TYPE",
                                "SAI_ROUTER_INTERFACE_TYPE_SUB_PORT"},
          swss::FieldValueTuple{"SAI_ROUTER_INTERFACE_ATTR_OUTER_VLAN_ID",
                                "700"},  // This should be 291.
          swss::FieldValueTuple{"SAI_ROUTER_INTERFACE_ATTR_PORT_ID",
                                "oid:0x112233"},
          swss::FieldValueTuple{"SAI_ROUTER_INTERFACE_ATTR_V4_MCAST_ENABLE",
                                "true"},
          swss::FieldValueTuple{"SAI_ROUTER_INTERFACE_ATTR_V6_MCAST_ENABLE",
                                "true"},
          swss::FieldValueTuple{"SAI_ROUTER_INTERFACE_ATTR_MTU", "1500"}});
  EXPECT_FALSE(VerifyState(db_key, attributes).empty());

  // Missing VLAN ID should fail verification.
  attributes.clear();
  attributes.push_back(
      swss::FieldValueTuple{prependParamField(p4orch::kPort), kPortName1});
  attributes.push_back(swss::FieldValueTuple{prependParamField(p4orch::kSrcMac),
                                             kMacAddress1.to_string()});
  EXPECT_FALSE(VerifyState(db_key, attributes).empty());

  // Different VLAN ID should fail verification.
  attributes.clear();
  attributes.push_back(
      swss::FieldValueTuple{prependParamField(p4orch::kPort), kPortName1});
  attributes.push_back(swss::FieldValueTuple{prependParamField(p4orch::kSrcMac),
                                             kMacAddress1.to_string()});
  attributes.push_back(
      swss::FieldValueTuple{prependParamField(p4orch::kVlanId), "0x321"});
  EXPECT_FALSE(VerifyState(db_key, attributes).empty());

  table.del("SAI_OBJECT_TYPE_ROUTER_INTERFACE:oid:0x295100");
}

TEST_F(RouterInterfaceManagerTest, VerifyStateAsicDbTest) {
  P4RouterInterfaceAppDbEntry router_intf_entry{
      .router_interface_id = kRouterInterfaceId1,
      .port_name = "Ethernet7",
      .src_mac_address = kMacAddress1,
      .vlan_id = kVlanId0,
      .is_set_port_name = true,
      .is_set_src_mac = true,
      .is_set_vlan_id = false,
  };
  AddRouterInterfaceEntry(router_intf_entry, kRouterInterfaceOid1, 0x1234,
                          9100);

    // Setup ASIC DB.
    swss::Table table(nullptr, "ASIC_STATE");
    table.set("SAI_OBJECT_TYPE_ROUTER_INTERFACE:oid:0x295100",
              std::vector<swss::FieldValueTuple>{
                  swss::FieldValueTuple{"SAI_ROUTER_INTERFACE_ATTR_VIRTUAL_ROUTER_ID", "oid:0x0"},
                  swss::FieldValueTuple{"SAI_ROUTER_INTERFACE_ATTR_SRC_MAC_ADDRESS", "00:01:02:03:04:05"},
                  swss::FieldValueTuple{"SAI_ROUTER_INTERFACE_ATTR_TYPE", "SAI_ROUTER_INTERFACE_TYPE_PORT"},
                  swss::FieldValueTuple{"SAI_ROUTER_INTERFACE_ATTR_PORT_ID", "oid:0x1234"},
                  swss::FieldValueTuple{"SAI_ROUTER_INTERFACE_ATTR_MTU", "9100"}});

    const std::string db_key = std::string(APP_P4RT_TABLE_NAME) + kTableKeyDelimiter +
                               APP_P4RT_ROUTER_INTERFACE_TABLE_NAME + kTableKeyDelimiter + kRouterIntfAppDbKey;
    std::vector<swss::FieldValueTuple> attributes;
    attributes.push_back(swss::FieldValueTuple{prependParamField(p4orch::kPort), "Ethernet7"});
    attributes.push_back(swss::FieldValueTuple{prependParamField(p4orch::kSrcMac), kMacAddress1.to_string()});

    // Verification should succeed with correct ASIC DB values.
    EXPECT_EQ(VerifyState(db_key, attributes), "");

    // Verification should fail if ASIC DB values mismatch.
    table.set("SAI_OBJECT_TYPE_ROUTER_INTERFACE:oid:0x295100",
              std::vector<swss::FieldValueTuple>{swss::FieldValueTuple{"SAI_ROUTER_INTERFACE_ATTR_MTU", "1500"}});
    EXPECT_FALSE(VerifyState(db_key, attributes).empty());

    // Verification should fail if ASIC DB table is missing.
    table.del("SAI_OBJECT_TYPE_ROUTER_INTERFACE:oid:0x295100");
    EXPECT_FALSE(VerifyState(db_key, attributes).empty());
    table.set("SAI_OBJECT_TYPE_ROUTER_INTERFACE:oid:0x295100",
              std::vector<swss::FieldValueTuple>{
                  swss::FieldValueTuple{"SAI_ROUTER_INTERFACE_ATTR_VIRTUAL_ROUTER_ID", "oid:0x0"},
                  swss::FieldValueTuple{"SAI_ROUTER_INTERFACE_ATTR_SRC_MAC_ADDRESS", "00:01:02:03:04:05"},
                  swss::FieldValueTuple{"SAI_ROUTER_INTERFACE_ATTR_TYPE", "SAI_ROUTER_INTERFACE_TYPE_PORT"},
                  swss::FieldValueTuple{"SAI_ROUTER_INTERFACE_ATTR_PORT_ID", "oid:0x1234"},
                  swss::FieldValueTuple{"SAI_ROUTER_INTERFACE_ATTR_MTU", "9100"}});

    // Verification should fail if SAI attr cannot be constructed.
    auto *router_intf_entry_ptr =
        GetRouterInterfaceEntry(KeyGenerator::generateRouterInterfaceKey(router_intf_entry.router_interface_id));
    router_intf_entry_ptr->port_name = "Ethernet8";
    EXPECT_FALSE(VerifyState(db_key, attributes).empty());
    router_intf_entry_ptr->port_name = "Ethernet7";
}
