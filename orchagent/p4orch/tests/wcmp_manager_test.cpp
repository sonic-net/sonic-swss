#include "wcmp_manager.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <nlohmann/json.hpp>
#include <string>

#include "mock_response_publisher.h"
#include "mock_sai_acl.h"
#include "mock_sai_hostif.h"
#include "mock_sai_next_hop_group.h"
#include "mock_sai_serialize.h"
#include "mock_sai_switch.h"
#include "p4oidmapper.h"
#include "p4orch.h"
#include "p4orch/p4orch_util.h"
#include "p4orch_util.h"
#include "return_code.h"
#include "sai_serialize.h"
extern "C"
{
#include "sai.h"
}

using ::p4orch::kTableKeyDelimiter;

extern P4Orch *gP4Orch;
extern VRFOrch *gVrfOrch;
extern std::unique_ptr<MockResponsePublisher> gMockResponsePublisher;
extern swss::DBConnector *gAppDb;
extern sai_object_id_t gSwitchId;
extern sai_next_hop_group_api_t *sai_next_hop_group_api;
extern sai_hostif_api_t *sai_hostif_api;
extern sai_switch_api_t *sai_switch_api;
extern sai_object_id_t gSwitchId;
extern sai_acl_api_t *sai_acl_api;

namespace p4orch
{
namespace test
{

using ::testing::_;
using ::testing::DoAll;
using ::testing::Eq;
using ::testing::Return;
using ::testing::SetArgPointee;
using ::testing::SetArrayArgument;
using ::testing::StrictMock;
using ::testing::Truly;

namespace
{

constexpr char *kWcmpGroupId1 = "group-1";
constexpr char *kWcmpGroupId2 = "group-2";
constexpr char* kWcmpGroupId3 = "group-3";
constexpr sai_object_id_t kWcmpGroupOid1 = 10;
constexpr char *kNexthopId1 = "ju1u32m1.atl11:qe-3/7";
constexpr sai_object_id_t kNexthopOid1 = 1;
constexpr char *kNexthopId2 = "ju1u32m2.atl11:qe-3/7";
constexpr sai_object_id_t kNexthopOid2 = 2;
constexpr char *kNexthopId3 = "ju1u32m3.atl11:qe-3/7";
constexpr sai_object_id_t kNexthopOid3 = 3;
const std::string kWcmpGroupKey1 = KeyGenerator::generateWcmpGroupKey(kWcmpGroupId1);
const std::string kNexthopKey1 = KeyGenerator::generateNextHopKey(kNexthopId1);
const std::string kNexthopKey2 = KeyGenerator::generateNextHopKey(kNexthopId2);
const std::string kNexthopKey3 = KeyGenerator::generateNextHopKey(kNexthopId3);

MATCHER_P(ArrayEq, array, "") {
  for (size_t i = 0; i < array.size(); ++i) {
    if (arg[i] != array[i]) {
      return false;
    }
  }
  return true;
}

// Matches the next hop group type sai_attribute_t argument.
bool MatchSaiNextHopGroupAttribute(const sai_attribute_t* attr,
                                   const P4WcmpGroupEntry& entry, bool update) {
  if (attr == nullptr) {
    return false;
  }
  int attr_size = 2;
  if (!update) {
    attr_size = 3;
    if (attr[0].id != SAI_NEXT_HOP_GROUP_ATTR_TYPE ||
        attr[0].value.s32 != SAI_NEXT_HOP_GROUP_TYPE_ECMP_WITH_MEMBERS) {
      return false;
    }
  }
  if (attr[attr_size - 2].id != SAI_NEXT_HOP_GROUP_ATTR_NEXT_HOP_LIST ||
      attr[attr_size - 1].id !=
          SAI_NEXT_HOP_GROUP_ATTR_NEXT_HOP_MEMBER_WEIGHT_LIST) {
    return false;
  }

  uint32_t count = 0;
  std::vector<sai_object_id_t> nexthop_ids;
  std::vector<uint32_t> nexthop_weights;
  for (const auto& member : entry.wcmp_group_members) {
    if (!member->pruned) {
      nexthop_ids.push_back(member->next_hop_oid);
      nexthop_weights.push_back(member->weight);
      count++;
    }
  }
  if (attr[attr_size - 2].value.objlist.count != count ||
      attr[attr_size - 1].value.u32list.count != count) {
    return false;
  }
  for (uint32_t i = 0; i < count; ++i) {
    if (attr[attr_size - 2].value.objlist.list[i] != nexthop_ids[i] ||
        attr[attr_size - 1].value.u32list.list[i] != nexthop_weights[i]) {
      return false;
    }
  }
  return true;
}

void VerifyWcmpGroupMemberEntry(const std::string &expected_next_hop_id, const int expected_weight,
                                std::shared_ptr<p4orch::P4WcmpGroupMemberEntry> wcmp_gm_entry)
{
    EXPECT_EQ(expected_next_hop_id, wcmp_gm_entry->next_hop_id);
    EXPECT_EQ(expected_weight, (int)wcmp_gm_entry->weight);
}

void VerifyWcmpGroupEntry(const P4WcmpGroupEntry &expect_entry, const P4WcmpGroupEntry &wcmp_entry)
{
    EXPECT_EQ(expect_entry.wcmp_group_id, wcmp_entry.wcmp_group_id);
    ASSERT_EQ(expect_entry.wcmp_group_members.size(), wcmp_entry.wcmp_group_members.size());
    for (size_t i = 0; i < expect_entry.wcmp_group_members.size(); i++)
    {
        ASSERT_LE(i, wcmp_entry.wcmp_group_members.size());
        auto gm = expect_entry.wcmp_group_members[i];
        VerifyWcmpGroupMemberEntry(gm->next_hop_id, gm->weight, wcmp_entry.wcmp_group_members[i]);
    }
}
} // namespace

class WcmpManagerTest : public ::testing::Test
{
  protected:
    WcmpManagerTest()
    {
        setUpMockApi();
        setUpP4Orch();
        wcmp_group_manager_ = gP4Orch->getWcmpManager();
        p4_oid_mapper_ = wcmp_group_manager_->m_p4OidMapper;
    }

    ~WcmpManagerTest()
    {
        EXPECT_CALL(mock_sai_switch_, set_switch_attribute(Eq(gSwitchId), _))
            .WillRepeatedly(Return(SAI_STATUS_SUCCESS));
        EXPECT_CALL(mock_sai_acl_, remove_acl_table_group(_)).WillRepeatedly(Return(SAI_STATUS_SUCCESS));
        delete gP4Orch;
        delete copp_orch_;
        gMockResponsePublisher.reset();
    }

    void setUpMockApi()
    {
        // Set up mock stuff for SAI next hop group API structure.
        mock_sai_next_hop_group = &mock_sai_next_hop_group_;
        mock_sai_switch = &mock_sai_switch_;
        mock_sai_hostif = &mock_sai_hostif_;
        mock_sai_serialize = &mock_sai_serialize_;
        mock_sai_acl = &mock_sai_acl_;

        sai_next_hop_group_api->create_next_hop_group = create_next_hop_group;
        sai_next_hop_group_api->remove_next_hop_group = remove_next_hop_group;
        sai_next_hop_group_api->set_next_hop_groups_attribute =
            set_next_hop_groups_attribute;
        sai_next_hop_group_api->set_next_hop_group_member_attribute =
            set_next_hop_group_member_attribute;

        sai_hostif_api->create_hostif_table_entry = mock_create_hostif_table_entry;
        sai_hostif_api->create_hostif_trap = mock_create_hostif_trap;
        sai_switch_api->get_switch_attribute = mock_get_switch_attribute;
        sai_switch_api->set_switch_attribute = mock_set_switch_attribute;
        sai_acl_api->create_acl_table_group = create_acl_table_group;
        sai_acl_api->remove_acl_table_group = remove_acl_table_group;
    }

    void setUpP4Orch()
    {
        // init copp orch
        EXPECT_CALL(mock_sai_hostif_, create_hostif_table_entry(_, _, _, _)).WillRepeatedly(Return(SAI_STATUS_SUCCESS));
        EXPECT_CALL(mock_sai_hostif_, create_hostif_trap(_, _, _, _)).WillOnce(Return(SAI_STATUS_SUCCESS));
        EXPECT_CALL(mock_sai_switch_, get_switch_attribute(_, _, _)).WillOnce(Return(SAI_STATUS_SUCCESS));
        copp_orch_ = new CoppOrch(gAppDb, APP_COPP_TABLE_NAME);

        std::vector<std::string> p4_tables{APP_P4RT_TABLE_NAME};
        gP4Orch = new P4Orch(gAppDb, p4_tables, gVrfOrch, copp_orch_);
        gMockResponsePublisher = std::make_unique<MockResponsePublisher>();
    }

    void Enqueue(const swss::KeyOpFieldsValuesTuple &entry)
    {
        wcmp_group_manager_->enqueue(APP_P4RT_WCMP_GROUP_TABLE_NAME, entry);
    }

    ReturnCode Drain(bool failure_before) {
      if (failure_before) {
        wcmp_group_manager_->drainWithNotExecuted();
        return ReturnCode(StatusCode::SWSS_RC_NOT_EXECUTED);
      }
      return wcmp_group_manager_->drain();
    }

    std::string VerifyState(const std::string &key, const std::vector<swss::FieldValueTuple> &tuple)
    {
        return wcmp_group_manager_->verifyState(key, tuple);
    }

    ReturnCode ProcessAddRequest(P4WcmpGroupEntry *app_db_entry)
    {
        return wcmp_group_manager_->processAddRequest(app_db_entry);
    }

    void HandlePortStatusChangeNotification(const std::string &op, const std::string &data)
    {
        gP4Orch->handlePortStatusChangeNotification(op, data);
    }

    void PruneNextHops(const std::string &port)
    {
      wcmp_group_manager_->updateWatchPort(port, true);
    }

    void RestorePrunedNextHops(const std::string &port)
    {
      wcmp_group_manager_->updateWatchPort(port, false);
    }

    bool VerifyWcmpGroupMemberInPortMap(std::shared_ptr<P4WcmpGroupMemberEntry> gm, bool expected_member_present,
                                        long unsigned int expected_set_size)
    {
        auto it = wcmp_group_manager_->port_name_to_wcmp_group_member_map.find(gm->watch_port);
        if (it != wcmp_group_manager_->port_name_to_wcmp_group_member_map.end())
        {
            auto &s = wcmp_group_manager_->port_name_to_wcmp_group_member_map[gm->watch_port];
            if (s.size() != expected_set_size)
                return false;
            return expected_member_present ? (s.count(gm) > 0) : (s.count(gm) == 0);
        }
        else
        {
            return !expected_member_present;
        }
        return false;
    }

    ReturnCode ProcessUpdateRequest(P4WcmpGroupEntry *app_db_entry)
    {
        return wcmp_group_manager_->processUpdateRequest(app_db_entry);
    }

    ReturnCode RemoveWcmpGroup(const std::string &wcmp_group_id)
    {
        return wcmp_group_manager_->removeWcmpGroup(wcmp_group_id);
    }

    P4WcmpGroupEntry *GetWcmpGroupEntry(const std::string &wcmp_group_id)
    {
        return wcmp_group_manager_->getWcmpGroupEntry(wcmp_group_id);
    }

    ReturnCodeOr<P4WcmpGroupEntry> DeserializeP4WcmpGroupAppDbEntry(
        const std::string &key, const std::vector<swss::FieldValueTuple> &attributes)
    {
        return wcmp_group_manager_->deserializeP4WcmpGroupAppDbEntry(key, attributes);
    }

    // Adds the WCMP group entry via WcmpManager::ProcessAddRequest(). This
    // function also takes care of all the dependencies of the WCMP group entry.
    // Returns a valid pointer to WCMP group entry on success.
    P4WcmpGroupEntry AddWcmpGroupEntry1();
    P4WcmpGroupEntry AddWcmpGroupEntryWithWatchport(const std::string &port, const bool oper_up = false);
    P4WcmpGroupEntry getDefaultWcmpGroupEntryForTest();
    std::shared_ptr<P4WcmpGroupMemberEntry> createWcmpGroupMemberEntry(const std::string &next_hop_id,
                                                                       const int weight);
    std::shared_ptr<P4WcmpGroupMemberEntry> createWcmpGroupMemberEntryWithWatchport(const std::string &next_hop_id,
                                                                                    const int weight,
                                                                                    const std::string &watch_port,
                                                                                    const std::string &wcmp_group_id,
                                                                                    const sai_object_id_t next_hop_oid);

    StrictMock<MockSaiNextHopGroup> mock_sai_next_hop_group_;
    StrictMock<MockSaiSwitch> mock_sai_switch_;
    StrictMock<MockSaiHostif> mock_sai_hostif_;
    StrictMock<MockSaiSerialize> mock_sai_serialize_;
    StrictMock<MockSaiAcl> mock_sai_acl_;
    P4OidMapper *p4_oid_mapper_;
    WcmpManager *wcmp_group_manager_;
    CoppOrch *copp_orch_;
};

P4WcmpGroupEntry WcmpManagerTest::getDefaultWcmpGroupEntryForTest()
{
    P4WcmpGroupEntry app_db_entry;
    app_db_entry.wcmp_group_id = kWcmpGroupId1;
    std::shared_ptr<P4WcmpGroupMemberEntry> gm1 = std::make_shared<P4WcmpGroupMemberEntry>();
    gm1->wcmp_group_id = kWcmpGroupId1;
    gm1->next_hop_id = kNexthopId1;
    gm1->weight = 2;
    app_db_entry.wcmp_group_members.push_back(gm1);
    std::shared_ptr<P4WcmpGroupMemberEntry> gm2 = std::make_shared<P4WcmpGroupMemberEntry>();
    gm2->wcmp_group_id = kWcmpGroupId1;
    gm2->next_hop_id = kNexthopId2;
    gm2->weight = 1;
    app_db_entry.wcmp_group_members.push_back(gm2);
    return app_db_entry;
}

P4WcmpGroupEntry WcmpManagerTest::AddWcmpGroupEntryWithWatchport(const std::string &port, const bool oper_up)
{
    P4WcmpGroupEntry app_db_entry;
    app_db_entry.wcmp_group_id = kWcmpGroupId1;
    std::shared_ptr<P4WcmpGroupMemberEntry> gm1 = std::make_shared<P4WcmpGroupMemberEntry>();
    gm1->next_hop_id = kNexthopId1;
    gm1->weight = 2;
    gm1->watch_port = port;
    gm1->wcmp_group_id = kWcmpGroupId1;
    if (!oper_up) {
      gm1->pruned = true;
    }
    app_db_entry.wcmp_group_members.push_back(gm1);
    p4_oid_mapper_->setOID(SAI_OBJECT_TYPE_NEXT_HOP, kNexthopKey1, kNexthopOid1);
    EXPECT_CALL(
        mock_sai_next_hop_group_,
        create_next_hop_group(_, Eq(gSwitchId), Eq(3),
                              Truly(std::bind(MatchSaiNextHopGroupAttribute,
                                              std::placeholders::_1,
                                              app_db_entry, /*update=*/false))))
        .WillOnce(DoAll(SetArgPointee<0>(kWcmpGroupOid1),
                        Return(SAI_STATUS_SUCCESS)));
    EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS, ProcessAddRequest(&app_db_entry));
    EXPECT_NE(nullptr, GetWcmpGroupEntry(kWcmpGroupId1));
    return app_db_entry;
}

P4WcmpGroupEntry WcmpManagerTest::AddWcmpGroupEntry1()
{
    P4WcmpGroupEntry app_db_entry = getDefaultWcmpGroupEntryForTest();
    p4_oid_mapper_->setOID(SAI_OBJECT_TYPE_NEXT_HOP, kNexthopKey1, kNexthopOid1);
    p4_oid_mapper_->setOID(SAI_OBJECT_TYPE_NEXT_HOP, kNexthopKey2, kNexthopOid2);
    p4_oid_mapper_->setOID(SAI_OBJECT_TYPE_NEXT_HOP, kNexthopKey3,
                           kNexthopOid3);
    EXPECT_CALL(
        mock_sai_next_hop_group_,
        create_next_hop_group(_, Eq(gSwitchId), Eq(3),
                              Truly(std::bind(MatchSaiNextHopGroupAttribute,
                                              std::placeholders::_1,
                                              app_db_entry, /*update=*/false))))
        .WillOnce(DoAll(SetArgPointee<0>(kWcmpGroupOid1),
                        Return(SAI_STATUS_SUCCESS)));
    EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS, ProcessAddRequest(&app_db_entry));
    EXPECT_NE(nullptr, GetWcmpGroupEntry(kWcmpGroupId1));
    return app_db_entry;
}

// Create a WCMP group member with the requested attributes
std::shared_ptr<P4WcmpGroupMemberEntry> WcmpManagerTest::createWcmpGroupMemberEntry(const std::string &next_hop_id,
                                                                                    const int weight)
{
    std::shared_ptr<P4WcmpGroupMemberEntry> gm = std::make_shared<P4WcmpGroupMemberEntry>();
    gm->next_hop_id = next_hop_id;
    gm->weight = weight;
    return gm;
}

// Create a WCMP group member that uses a watchport with the requested
// attributes
std::shared_ptr<P4WcmpGroupMemberEntry> WcmpManagerTest::createWcmpGroupMemberEntryWithWatchport(
    const std::string &next_hop_id, const int weight, const std::string &watch_port, const std::string &wcmp_group_id,
    const sai_object_id_t next_hop_oid)
{
    std::shared_ptr<P4WcmpGroupMemberEntry> gm = std::make_shared<P4WcmpGroupMemberEntry>();
    gm->next_hop_id = next_hop_id;
    gm->weight = weight;
    gm->watch_port = watch_port;
    gm->wcmp_group_id = wcmp_group_id;
    p4_oid_mapper_->setOID(SAI_OBJECT_TYPE_NEXT_HOP, KeyGenerator::generateNextHopKey(next_hop_id), next_hop_oid);
    return gm;
}

TEST_F(WcmpManagerTest, CreateWcmpGroup)
{
    AddWcmpGroupEntry1();
    P4WcmpGroupEntry expect_entry = {.wcmp_group_id = kWcmpGroupId1,
                                     .wcmp_group_members = {},
                                     .nexthop_ids = {},
                                     .nexthop_weights = {}};
    std::shared_ptr<P4WcmpGroupMemberEntry> gm_entry1 = createWcmpGroupMemberEntry(kNexthopId1, 2);
    expect_entry.wcmp_group_members.push_back(gm_entry1);
    std::shared_ptr<P4WcmpGroupMemberEntry> gm_entry2 = createWcmpGroupMemberEntry(kNexthopId2, 1);
    expect_entry.wcmp_group_members.push_back(gm_entry2);
    VerifyWcmpGroupEntry(expect_entry, *GetWcmpGroupEntry(kWcmpGroupId1));
}

TEST_F(WcmpManagerTest, CreateWcmpGroupFailsWhenCreateGroupSaiCallFails)
{
    P4WcmpGroupEntry app_db_entry = getDefaultWcmpGroupEntryForTest();
    app_db_entry.wcmp_group_members.pop_back();
    p4_oid_mapper_->setOID(SAI_OBJECT_TYPE_NEXT_HOP, kNexthopKey1, kNexthopOid1);
    // WCMP group creation fails when one of the group member creation fails
    EXPECT_CALL(mock_sai_next_hop_group_, create_next_hop_group(_, _, _, _)).WillOnce(Return(SAI_STATUS_TABLE_FULL));

    EXPECT_EQ(StatusCode::SWSS_RC_FULL, ProcessAddRequest(&app_db_entry));
    std::string key = KeyGenerator::generateWcmpGroupKey(kWcmpGroupId1);
    auto *wcmp_group_entry_ptr = GetWcmpGroupEntry(kWcmpGroupId1);
    EXPECT_EQ(nullptr, wcmp_group_entry_ptr);
    EXPECT_FALSE(p4_oid_mapper_->existsOID(SAI_OBJECT_TYPE_NEXT_HOP_GROUP, key));
    uint32_t ref_cnt;
    EXPECT_TRUE(p4_oid_mapper_->getRefCount(SAI_OBJECT_TYPE_NEXT_HOP, kNexthopKey1, &ref_cnt));
    EXPECT_EQ(0, ref_cnt);
}

TEST_F(WcmpManagerTest, RemoveWcmpGroupFailsWhenRefcountIsGtThanZero)
{
    AddWcmpGroupEntry1();
    p4_oid_mapper_->increaseRefCount(SAI_OBJECT_TYPE_NEXT_HOP_GROUP, KeyGenerator::generateWcmpGroupKey(kWcmpGroupId1));
    EXPECT_EQ(StatusCode::SWSS_RC_IN_USE, RemoveWcmpGroup(kWcmpGroupId1));
    EXPECT_NE(nullptr, GetWcmpGroupEntry(kWcmpGroupId1));
}

TEST_F(WcmpManagerTest, RemoveWcmpGroupFailsWhenNotExist)
{
    EXPECT_EQ(StatusCode::SWSS_RC_NOT_FOUND, RemoveWcmpGroup(kWcmpGroupId1));
}

TEST_F(WcmpManagerTest, RemoveWcmpGroupFailsWhenSaiCallFails)
{
  P4WcmpGroupEntry app_db_entry = AddWcmpGroupEntry1();
  EXPECT_CALL(mock_sai_next_hop_group_,
              remove_next_hop_group(Eq(kWcmpGroupOid1)))
      .WillOnce(Return(SAI_STATUS_FAILURE));
  EXPECT_EQ(StatusCode::SWSS_RC_UNKNOWN, RemoveWcmpGroup(kWcmpGroupId1));
}

TEST_F(WcmpManagerTest, UpdateWcmpGroupMembersSucceed)
{
  AddWcmpGroupEntry1();
  // Update WCMP group member with nexthop_id=kNexthopId1 weight to 3,
  // nexthop_id=kNexthopId2 weight to 15.
  P4WcmpGroupEntry wcmp_group = {.wcmp_group_id = kWcmpGroupId1,
                                 .wcmp_group_members = {},
                                 .nexthop_ids = {},
                                 .nexthop_weights = {}};
  std::shared_ptr<P4WcmpGroupMemberEntry> gm1 =
      createWcmpGroupMemberEntry(kNexthopId1, 3);
  std::shared_ptr<P4WcmpGroupMemberEntry> gm2 =
      createWcmpGroupMemberEntry(kNexthopId2, 15);
  wcmp_group.wcmp_group_members.push_back(gm1);
  wcmp_group.wcmp_group_members.push_back(gm2);

  std::vector<sai_status_t> exp_status{SAI_STATUS_SUCCESS, SAI_STATUS_SUCCESS};

  EXPECT_CALL(
      mock_sai_next_hop_group_,
      set_next_hop_groups_attribute(
          Eq(2),
          ArrayEq(std::vector<sai_object_id_t>{kWcmpGroupOid1, kWcmpGroupOid1}),
          Truly(std::bind(MatchSaiNextHopGroupAttribute, std::placeholders::_1,
                          wcmp_group, /*update=*/true)),
          _, _))
      .WillOnce(DoAll(SetArrayArgument<4>(exp_status.begin(), exp_status.end()),
                      Return(SAI_STATUS_SUCCESS)));

  EXPECT_TRUE(ProcessUpdateRequest(&wcmp_group).ok());
  VerifyWcmpGroupEntry(wcmp_group, *GetWcmpGroupEntry(kWcmpGroupId1));
  uint32_t wcmp_group_refcount = 0;
  uint32_t nexthop_refcount = 0;
  ASSERT_TRUE(p4_oid_mapper_->getRefCount(
      SAI_OBJECT_TYPE_NEXT_HOP_GROUP, kWcmpGroupKey1, &wcmp_group_refcount));
  EXPECT_EQ(0, wcmp_group_refcount);
  ASSERT_TRUE(p4_oid_mapper_->getRefCount(SAI_OBJECT_TYPE_NEXT_HOP,
                                          kNexthopKey1, &nexthop_refcount));
  EXPECT_EQ(1, nexthop_refcount);
  ASSERT_TRUE(p4_oid_mapper_->getRefCount(SAI_OBJECT_TYPE_NEXT_HOP,
                                          kNexthopKey2, &nexthop_refcount));
  EXPECT_EQ(1, nexthop_refcount);
  // Remove group member with nexthop_id=kNexthopId1
  wcmp_group.wcmp_group_members.clear();
  gm2 = createWcmpGroupMemberEntry(kNexthopId2, 15);
  wcmp_group.wcmp_group_members.push_back(gm2);

  EXPECT_CALL(
      mock_sai_next_hop_group_,
      set_next_hop_groups_attribute(
          Eq(2),
          ArrayEq(std::vector<sai_object_id_t>{kWcmpGroupOid1, kWcmpGroupOid1}),
          Truly(std::bind(MatchSaiNextHopGroupAttribute, std::placeholders::_1,
                          wcmp_group, /*update=*/true)),
          _, _))
      .WillOnce(DoAll(SetArrayArgument<4>(exp_status.begin(), exp_status.end()),
                      Return(SAI_STATUS_SUCCESS)));

  EXPECT_TRUE(ProcessUpdateRequest(&wcmp_group).ok());
  VerifyWcmpGroupEntry(wcmp_group, *GetWcmpGroupEntry(kWcmpGroupId1));
  ASSERT_TRUE(p4_oid_mapper_->getRefCount(
      SAI_OBJECT_TYPE_NEXT_HOP_GROUP, kWcmpGroupKey1, &wcmp_group_refcount));
  EXPECT_EQ(0, wcmp_group_refcount);
  ASSERT_TRUE(p4_oid_mapper_->getRefCount(SAI_OBJECT_TYPE_NEXT_HOP,
                                          kNexthopKey1, &nexthop_refcount));
  EXPECT_EQ(0, nexthop_refcount);
  ASSERT_TRUE(p4_oid_mapper_->getRefCount(SAI_OBJECT_TYPE_NEXT_HOP,
                                          kNexthopKey2, &nexthop_refcount));
  EXPECT_EQ(1, nexthop_refcount);
  // Add group member with nexthop_id=kNexthopId1 and weight=20
  wcmp_group.wcmp_group_members.clear();
  std::shared_ptr<P4WcmpGroupMemberEntry> updated_gm2 =
      createWcmpGroupMemberEntry(kNexthopId2, 15);
  std::shared_ptr<P4WcmpGroupMemberEntry> updated_gm1 =
      createWcmpGroupMemberEntry(kNexthopId1, 20);
  wcmp_group.wcmp_group_members.push_back(updated_gm1);
  wcmp_group.wcmp_group_members.push_back(updated_gm2);

  EXPECT_CALL(
      mock_sai_next_hop_group_,
      set_next_hop_groups_attribute(
          Eq(2),
          ArrayEq(std::vector<sai_object_id_t>{kWcmpGroupOid1, kWcmpGroupOid1}),
          Truly(std::bind(MatchSaiNextHopGroupAttribute, std::placeholders::_1,
                          wcmp_group, /*update=*/true)),
          _, _))
      .WillOnce(DoAll(SetArrayArgument<4>(exp_status.begin(), exp_status.end()),
                      Return(SAI_STATUS_SUCCESS)));

  EXPECT_TRUE(ProcessUpdateRequest(&wcmp_group).ok());
  VerifyWcmpGroupEntry(wcmp_group, *GetWcmpGroupEntry(kWcmpGroupId1));
  ASSERT_TRUE(p4_oid_mapper_->getRefCount(
      SAI_OBJECT_TYPE_NEXT_HOP_GROUP, kWcmpGroupKey1, &wcmp_group_refcount));
  EXPECT_EQ(0, wcmp_group_refcount);
  ASSERT_TRUE(p4_oid_mapper_->getRefCount(SAI_OBJECT_TYPE_NEXT_HOP,
                                          kNexthopKey1, &nexthop_refcount));
  EXPECT_EQ(1, nexthop_refcount);
  ASSERT_TRUE(p4_oid_mapper_->getRefCount(SAI_OBJECT_TYPE_NEXT_HOP,
                                          kNexthopKey2, &nexthop_refcount));
  EXPECT_EQ(1, nexthop_refcount);

  // Update WCMP without group members
  wcmp_group.wcmp_group_members.clear();

  EXPECT_CALL(
      mock_sai_next_hop_group_,
      set_next_hop_groups_attribute(
          Eq(2),
          ArrayEq(std::vector<sai_object_id_t>{kWcmpGroupOid1, kWcmpGroupOid1}),
          Truly(std::bind(MatchSaiNextHopGroupAttribute, std::placeholders::_1,
                          wcmp_group, /*update=*/true)),
          _, _))
      .WillOnce(DoAll(SetArrayArgument<4>(exp_status.begin(), exp_status.end()),
                      Return(SAI_STATUS_SUCCESS)));

  EXPECT_TRUE(ProcessUpdateRequest(&wcmp_group).ok());
  VerifyWcmpGroupEntry(wcmp_group, *GetWcmpGroupEntry(kWcmpGroupId1));
  ASSERT_TRUE(p4_oid_mapper_->getRefCount(
      SAI_OBJECT_TYPE_NEXT_HOP_GROUP, kWcmpGroupKey1, &wcmp_group_refcount));
  EXPECT_EQ(0, wcmp_group_refcount);
  ASSERT_TRUE(p4_oid_mapper_->getRefCount(SAI_OBJECT_TYPE_NEXT_HOP,
                                          kNexthopKey1, &nexthop_refcount));
  EXPECT_EQ(0, nexthop_refcount);
  ASSERT_TRUE(p4_oid_mapper_->getRefCount(SAI_OBJECT_TYPE_NEXT_HOP,
                                          kNexthopKey2, &nexthop_refcount));
  EXPECT_EQ(0, nexthop_refcount);
}

TEST_F(WcmpManagerTest, UpdateWcmpGroupFailsWhenSaiCallFails) {
  AddWcmpGroupEntry1();
  // Add WCMP group member with nexthop_id=kNexthopId1, weight=3 and
  // nexthop_id=kNexthopId3, weight=30, update nexthop_id=kNexthopId2
  // weight to 10.
  P4WcmpGroupEntry wcmp_group = {.wcmp_group_id = kWcmpGroupId1,
                                 .wcmp_group_members = {},
                                 .nexthop_ids = {},
                                 .nexthop_weights = {}};
  std::shared_ptr<P4WcmpGroupMemberEntry> gm1 =
      createWcmpGroupMemberEntry(kNexthopId1, 3);
  std::shared_ptr<P4WcmpGroupMemberEntry> gm2 =
      createWcmpGroupMemberEntry(kNexthopId2, 10);
  std::shared_ptr<P4WcmpGroupMemberEntry> gm3 =
      createWcmpGroupMemberEntry(kNexthopId3, 30);

  wcmp_group.wcmp_group_members.push_back(gm1);
  wcmp_group.wcmp_group_members.push_back(gm2);
  wcmp_group.wcmp_group_members.push_back(gm3);

  std::vector<sai_status_t> exp_status{SAI_STATUS_SUCCESS, SAI_STATUS_SUCCESS};

  EXPECT_CALL(
      mock_sai_next_hop_group_,
      set_next_hop_groups_attribute(
          Eq(2),
          ArrayEq(std::vector<sai_object_id_t>{kWcmpGroupOid1, kWcmpGroupOid1}),
          Truly(std::bind(MatchSaiNextHopGroupAttribute, std::placeholders::_1,
                          wcmp_group, /*update=*/true)),
          _, _))
      .WillOnce(DoAll(SetArrayArgument<4>(exp_status.begin(), exp_status.end()),
                      Return(SAI_STATUS_SUCCESS)));

  EXPECT_TRUE(ProcessUpdateRequest(&wcmp_group).ok());
  VerifyWcmpGroupEntry(wcmp_group, *GetWcmpGroupEntry(kWcmpGroupId1));
  uint32_t wcmp_group_refcount = 0;
  uint32_t nexthop_refcount = 0;
  ASSERT_TRUE(p4_oid_mapper_->getRefCount(
      SAI_OBJECT_TYPE_NEXT_HOP_GROUP, kWcmpGroupKey1, &wcmp_group_refcount));
  EXPECT_EQ(0, wcmp_group_refcount);
  ASSERT_TRUE(p4_oid_mapper_->getRefCount(SAI_OBJECT_TYPE_NEXT_HOP,
                                          kNexthopKey1, &nexthop_refcount));
  EXPECT_EQ(1, nexthop_refcount);
  ASSERT_TRUE(p4_oid_mapper_->getRefCount(SAI_OBJECT_TYPE_NEXT_HOP,
                                          kNexthopKey2, &nexthop_refcount));
  EXPECT_EQ(1, nexthop_refcount);
  ASSERT_TRUE(p4_oid_mapper_->getRefCount(SAI_OBJECT_TYPE_NEXT_HOP,
                                          kNexthopKey3, &nexthop_refcount));
  EXPECT_EQ(1, nexthop_refcount);
  // Remove WCMP group member with nexthop_id=kNexthopId1 and
  // nexthop_id=kNexthopId3(fail) - succeed to clean up
  wcmp_group.wcmp_group_members.clear();
  wcmp_group.wcmp_group_members.push_back(gm1);
  wcmp_group.wcmp_group_members.push_back(gm3);

  exp_status = std::vector<sai_status_t>{SAI_STATUS_OBJECT_IN_USE,
                                         SAI_STATUS_OBJECT_IN_USE};
  EXPECT_CALL(
      mock_sai_next_hop_group_,
      set_next_hop_groups_attribute(
          Eq(2),
          ArrayEq(std::vector<sai_object_id_t>{kWcmpGroupOid1, kWcmpGroupOid1}),
          Truly(std::bind(MatchSaiNextHopGroupAttribute, std::placeholders::_1,
                          wcmp_group, /*update=*/true)),
          _, _))
      .WillOnce(DoAll(SetArrayArgument<4>(exp_status.begin(), exp_status.end()),
                      Return(SAI_STATUS_OBJECT_IN_USE)));
  EXPECT_EQ(StatusCode::SWSS_RC_IN_USE, ProcessUpdateRequest(&wcmp_group));
  P4WcmpGroupEntry expected_wcmp_group = {.wcmp_group_id = kWcmpGroupId1,
                                          .wcmp_group_members = {},
                                          .nexthop_ids = {},
                                          .nexthop_weights = {}};
  expected_wcmp_group.wcmp_group_members.push_back(gm1);
  expected_wcmp_group.wcmp_group_members.push_back(gm2);
  expected_wcmp_group.wcmp_group_members.push_back(gm3);
  // WCMP group remains as the old one
  VerifyWcmpGroupEntry(expected_wcmp_group, *GetWcmpGroupEntry(kWcmpGroupId1));
  ASSERT_TRUE(p4_oid_mapper_->getRefCount(
      SAI_OBJECT_TYPE_NEXT_HOP_GROUP, kWcmpGroupKey1, &wcmp_group_refcount));
  EXPECT_EQ(0, wcmp_group_refcount);
  ASSERT_TRUE(p4_oid_mapper_->getRefCount(SAI_OBJECT_TYPE_NEXT_HOP,
                                          kNexthopKey1, &nexthop_refcount));
  EXPECT_EQ(1, nexthop_refcount);
  ASSERT_TRUE(p4_oid_mapper_->getRefCount(SAI_OBJECT_TYPE_NEXT_HOP,
                                          kNexthopKey2, &nexthop_refcount));
  EXPECT_EQ(1, nexthop_refcount);
  ASSERT_TRUE(p4_oid_mapper_->getRefCount(SAI_OBJECT_TYPE_NEXT_HOP,
                                          kNexthopKey3, &nexthop_refcount));
  EXPECT_EQ(1, nexthop_refcount);
}

TEST_F(WcmpManagerTest, ValidateWcmpGroupEntryFailsWhenNextHopDoesNotExist)
{
    const std::string kKeyPrefix = std::string(APP_P4RT_WCMP_GROUP_TABLE_NAME) + kTableKeyDelimiter;
    p4_oid_mapper_->setOID(SAI_OBJECT_TYPE_NEXT_HOP, kNexthopKey1, kNexthopOid1);
    nlohmann::json j;
    j[prependMatchField(p4orch::kWcmpGroupId)] = kWcmpGroupId1;
    std::vector<swss::FieldValueTuple> attributes;
    nlohmann::json actions;
    nlohmann::json action;
    action[p4orch::kAction] = p4orch::kSetNexthopId;
    action[prependParamField(p4orch::kNexthopId)] = kNexthopId1;
    actions.push_back(action);
    action[prependParamField(p4orch::kNexthopId)] = kNexthopId2;
    actions.push_back(action);
    attributes.push_back(swss::FieldValueTuple{p4orch::kActions, actions.dump()});
    Enqueue(swss::KeyOpFieldsValuesTuple(kKeyPrefix + j.dump(), SET_COMMAND, attributes));
    EXPECT_CALL(
        *gMockResponsePublisher,
        publish(Eq(APP_P4RT_TABLE_NAME), Eq(kKeyPrefix + j.dump()),
                Eq(attributes), Eq(StatusCode::SWSS_RC_NOT_FOUND), Eq(true)));
    EXPECT_EQ(StatusCode::SWSS_RC_NOT_FOUND, Drain(/*failure_before=*/false));
    std::string key = KeyGenerator::generateWcmpGroupKey(kWcmpGroupId1);
    auto *wcmp_group_entry_ptr = GetWcmpGroupEntry(kWcmpGroupId1);
    EXPECT_EQ(nullptr, wcmp_group_entry_ptr);
    EXPECT_FALSE(p4_oid_mapper_->existsOID(SAI_OBJECT_TYPE_NEXT_HOP_GROUP, key));
    uint32_t ref_cnt;
    EXPECT_TRUE(p4_oid_mapper_->getRefCount(SAI_OBJECT_TYPE_NEXT_HOP, kNexthopKey1, &ref_cnt));
    EXPECT_EQ(0, ref_cnt);
}

TEST_F(WcmpManagerTest, ValidateWcmpGroupEntryFailsWhenWeightLessThanOne)
{
    const std::string kKeyPrefix = std::string(APP_P4RT_WCMP_GROUP_TABLE_NAME) + kTableKeyDelimiter;
    p4_oid_mapper_->setOID(SAI_OBJECT_TYPE_NEXT_HOP, kNexthopKey1, kNexthopOid1);
    p4_oid_mapper_->setOID(SAI_OBJECT_TYPE_NEXT_HOP, kNexthopKey2, kNexthopOid2);
    nlohmann::json j;
    j[prependMatchField(p4orch::kWcmpGroupId)] = kWcmpGroupId1;
    std::vector<swss::FieldValueTuple> attributes;
    nlohmann::json actions;
    nlohmann::json action;
    action[p4orch::kAction] = p4orch::kSetNexthopId;
    action[prependParamField(p4orch::kNexthopId)] = kNexthopId1;
    actions.push_back(action);
    action[p4orch::kWeight] = -1;
    action[prependParamField(p4orch::kNexthopId)] = kNexthopId2;
    actions.push_back(action);
    attributes.push_back(swss::FieldValueTuple{p4orch::kActions, actions.dump()});
    Enqueue(swss::KeyOpFieldsValuesTuple(kKeyPrefix + j.dump(), SET_COMMAND, attributes));
    EXPECT_CALL(*gMockResponsePublisher,
                publish(Eq(APP_P4RT_TABLE_NAME), Eq(kKeyPrefix + j.dump()),
                        Eq(attributes), Eq(StatusCode::SWSS_RC_INVALID_PARAM),
                        Eq(true)));
    EXPECT_EQ(StatusCode::SWSS_RC_INVALID_PARAM,
              Drain(/*failure_before=*/false));
    std::string key = KeyGenerator::generateWcmpGroupKey(kWcmpGroupId1);
    auto *wcmp_group_entry_ptr = GetWcmpGroupEntry(kWcmpGroupId1);
    EXPECT_EQ(nullptr, wcmp_group_entry_ptr);
    EXPECT_FALSE(p4_oid_mapper_->existsOID(SAI_OBJECT_TYPE_NEXT_HOP_GROUP, key));
    uint32_t ref_cnt;
    EXPECT_TRUE(p4_oid_mapper_->getRefCount(SAI_OBJECT_TYPE_NEXT_HOP, kNexthopKey1, &ref_cnt));
    EXPECT_EQ(0, ref_cnt);
    EXPECT_TRUE(p4_oid_mapper_->getRefCount(SAI_OBJECT_TYPE_NEXT_HOP, kNexthopKey2, &ref_cnt));
    EXPECT_EQ(0, ref_cnt);
}

TEST_F(WcmpManagerTest, WcmpGroupInvalidOperationInDrainFails)
{
    const std::string kKeyPrefix = std::string(APP_P4RT_WCMP_GROUP_TABLE_NAME) + kTableKeyDelimiter;
    p4_oid_mapper_->setOID(SAI_OBJECT_TYPE_NEXT_HOP, kNexthopKey1, kNexthopOid1);
    p4_oid_mapper_->setOID(SAI_OBJECT_TYPE_NEXT_HOP, kNexthopKey2, kNexthopOid2);
    nlohmann::json j;
    j[prependMatchField(p4orch::kWcmpGroupId)] = kWcmpGroupId1;
    std::vector<swss::FieldValueTuple> attributes;
    // If weight is omitted in the action, then it is set to 1 by default(ECMP)
    nlohmann::json actions;
    nlohmann::json action;
    action[p4orch::kAction] = p4orch::kSetNexthopId;
    action[prependParamField(p4orch::kNexthopId)] = kNexthopId1;
    actions.push_back(action);
    action[prependParamField(p4orch::kNexthopId)] = kNexthopId2;
    actions.push_back(action);
    attributes.push_back(swss::FieldValueTuple{p4orch::kActions, actions.dump()});

    // Invalid Operation string. Only SET and DEL are allowed
    Enqueue(swss::KeyOpFieldsValuesTuple(kKeyPrefix + j.dump(), "Update", attributes));
    EXPECT_CALL(*gMockResponsePublisher,
                publish(Eq(APP_P4RT_TABLE_NAME), Eq(kKeyPrefix + j.dump()),
                        Eq(attributes), Eq(StatusCode::SWSS_RC_INVALID_PARAM),
                        Eq(true)));
    EXPECT_EQ(StatusCode::SWSS_RC_INVALID_PARAM,
              Drain(/*failure_before=*/false));
    std::string key = KeyGenerator::generateWcmpGroupKey(kWcmpGroupId1);
    auto *wcmp_group_entry_ptr = GetWcmpGroupEntry(kWcmpGroupId1);
    EXPECT_EQ(nullptr, wcmp_group_entry_ptr);
    EXPECT_FALSE(p4_oid_mapper_->existsOID(SAI_OBJECT_TYPE_NEXT_HOP_GROUP, key));
    uint32_t ref_cnt;
    EXPECT_TRUE(p4_oid_mapper_->getRefCount(SAI_OBJECT_TYPE_NEXT_HOP, kNexthopKey1, &ref_cnt));
    EXPECT_EQ(0, ref_cnt);
    EXPECT_TRUE(p4_oid_mapper_->getRefCount(SAI_OBJECT_TYPE_NEXT_HOP, kNexthopKey2, &ref_cnt));
    EXPECT_EQ(0, ref_cnt);
}

TEST_F(WcmpManagerTest, WcmpGroupUndefinedAttributesInDrainFails)
{
    const std::string kKeyPrefix = std::string(APP_P4RT_WCMP_GROUP_TABLE_NAME) + kTableKeyDelimiter;
    nlohmann::json j;
    j[prependMatchField(p4orch::kWcmpGroupId)] = kWcmpGroupId1;
    std::vector<swss::FieldValueTuple> attributes;
    attributes.push_back(swss::FieldValueTuple{"Undefined", "Invalid"});
    Enqueue(swss::KeyOpFieldsValuesTuple(kKeyPrefix + j.dump(), SET_COMMAND, attributes));
    EXPECT_CALL(*gMockResponsePublisher,
                publish(Eq(APP_P4RT_TABLE_NAME), Eq(kKeyPrefix + j.dump()),
                        Eq(attributes), Eq(StatusCode::SWSS_RC_INVALID_PARAM),
                        Eq(true)));
    EXPECT_EQ(StatusCode::SWSS_RC_INVALID_PARAM,
              Drain(/*failure_before=*/false));
    std::string key = KeyGenerator::generateWcmpGroupKey(kWcmpGroupId1);
    auto *wcmp_group_entry_ptr = GetWcmpGroupEntry(kWcmpGroupId1);
    EXPECT_EQ(nullptr, wcmp_group_entry_ptr);
    EXPECT_FALSE(p4_oid_mapper_->existsOID(SAI_OBJECT_TYPE_NEXT_HOP_GROUP, key));
}

TEST_F(WcmpManagerTest, WcmpGroupCreateAndDeleteInDrainSucceeds)
{
    const std::string kKeyPrefix = std::string(APP_P4RT_WCMP_GROUP_TABLE_NAME) + kTableKeyDelimiter;
    p4_oid_mapper_->setOID(SAI_OBJECT_TYPE_NEXT_HOP, kNexthopKey1, kNexthopOid1);
    p4_oid_mapper_->setOID(SAI_OBJECT_TYPE_NEXT_HOP, kNexthopKey2, kNexthopOid2);
    nlohmann::json j;
    j[prependMatchField(p4orch::kWcmpGroupId)] = kWcmpGroupId1;
    std::vector<swss::FieldValueTuple> attributes;
    // If weight is omitted in the action, then it is set to 1 by default(ECMP)
    nlohmann::json actions;
    nlohmann::json action;
    action[p4orch::kAction] = p4orch::kSetNexthopId;
    action[prependParamField(p4orch::kNexthopId)] = kNexthopId1;
    actions.push_back(action);
    action[prependParamField(p4orch::kNexthopId)] = kNexthopId2;
    actions.push_back(action);
    attributes.push_back(swss::FieldValueTuple{p4orch::kActions, actions.dump()});

    Enqueue(swss::KeyOpFieldsValuesTuple(kKeyPrefix + j.dump(), SET_COMMAND, attributes));

    EXPECT_CALL(mock_sai_next_hop_group_, create_next_hop_group(_, _, _, _))
        .WillOnce(DoAll(SetArgPointee<0>(kWcmpGroupOid1),
                        Return(SAI_STATUS_SUCCESS)));
    EXPECT_CALL(
        *gMockResponsePublisher,
        publish(Eq(APP_P4RT_TABLE_NAME), Eq(kKeyPrefix + j.dump()),
                Eq(attributes), Eq(StatusCode::SWSS_RC_SUCCESS), Eq(true)));
    EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS, Drain(/*failure_before=*/false));
    std::string key = KeyGenerator::generateWcmpGroupKey(kWcmpGroupId1);
    auto *wcmp_group_entry_ptr = GetWcmpGroupEntry(kWcmpGroupId1);
    EXPECT_NE(nullptr, wcmp_group_entry_ptr);
    EXPECT_TRUE(p4_oid_mapper_->existsOID(SAI_OBJECT_TYPE_NEXT_HOP_GROUP, key));
    uint32_t ref_cnt;
    EXPECT_TRUE(p4_oid_mapper_->getRefCount(SAI_OBJECT_TYPE_NEXT_HOP, kNexthopKey1, &ref_cnt));
    EXPECT_EQ(1, ref_cnt);
    EXPECT_TRUE(p4_oid_mapper_->getRefCount(SAI_OBJECT_TYPE_NEXT_HOP, kNexthopKey2, &ref_cnt));
    EXPECT_EQ(1, ref_cnt);

    EXPECT_CALL(mock_sai_next_hop_group_, remove_next_hop_group(Eq(kWcmpGroupOid1)))
        .WillOnce(Return(SAI_STATUS_SUCCESS));
    attributes.clear();
    Enqueue(swss::KeyOpFieldsValuesTuple(kKeyPrefix + j.dump(), DEL_COMMAND, attributes));
    EXPECT_CALL(
        *gMockResponsePublisher,
        publish(Eq(APP_P4RT_TABLE_NAME), Eq(kKeyPrefix + j.dump()),
                Eq(attributes), Eq(StatusCode::SWSS_RC_SUCCESS), Eq(true)));
    EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS, Drain(/*failure_before=*/false));
    wcmp_group_entry_ptr = GetWcmpGroupEntry(kWcmpGroupId1);
    EXPECT_EQ(nullptr, wcmp_group_entry_ptr);
    EXPECT_FALSE(p4_oid_mapper_->existsOID(SAI_OBJECT_TYPE_NEXT_HOP_GROUP, key));
    EXPECT_TRUE(p4_oid_mapper_->getRefCount(SAI_OBJECT_TYPE_NEXT_HOP, kNexthopKey1, &ref_cnt));
    EXPECT_EQ(0, ref_cnt);
    EXPECT_TRUE(p4_oid_mapper_->getRefCount(SAI_OBJECT_TYPE_NEXT_HOP, kNexthopKey2, &ref_cnt));
    EXPECT_EQ(0, ref_cnt);
}
// divya: start from here.
TEST_F(WcmpManagerTest, WcmpGroupCreateAndUpdateInDrainSucceeds)
{
    const std::string kKeyPrefix = std::string(APP_P4RT_WCMP_GROUP_TABLE_NAME) + kTableKeyDelimiter;
    p4_oid_mapper_->setOID(SAI_OBJECT_TYPE_NEXT_HOP, kNexthopKey1, kNexthopOid1);
    p4_oid_mapper_->setOID(SAI_OBJECT_TYPE_NEXT_HOP, kNexthopKey2, kNexthopOid2);
    nlohmann::json j;
    j[prependMatchField(p4orch::kWcmpGroupId)] = kWcmpGroupId1;
    std::vector<swss::FieldValueTuple> attributes;
    nlohmann::json actions;
    nlohmann::json action;
    action[p4orch::kAction] = p4orch::kSetNexthopId;
    action[p4orch::kWeight] = 1;
    action[prependParamField(p4orch::kNexthopId)] = kNexthopId1;
    actions.push_back(action);
    attributes.push_back(swss::FieldValueTuple{p4orch::kActions, actions.dump()});
    // Create WCMP group with member {next_hop_id=kNexthopId1, weight=1}
    Enqueue(swss::KeyOpFieldsValuesTuple(kKeyPrefix + j.dump(), SET_COMMAND, attributes));
    EXPECT_CALL(mock_sai_next_hop_group_, create_next_hop_group(_, _, _, _))
        .WillOnce(DoAll(SetArgPointee<0>(kWcmpGroupOid1),
                        Return(SAI_STATUS_SUCCESS)));
    EXPECT_CALL(
        *gMockResponsePublisher,
        publish(Eq(APP_P4RT_TABLE_NAME), Eq(kKeyPrefix + j.dump()),
                Eq(attributes), Eq(StatusCode::SWSS_RC_SUCCESS), Eq(true)));
    EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS, Drain(/*failure_before=*/false));
    std::string key = KeyGenerator::generateWcmpGroupKey(kWcmpGroupId1);
    auto *wcmp_group_entry_ptr = GetWcmpGroupEntry(kWcmpGroupId1);
    EXPECT_NE(nullptr, wcmp_group_entry_ptr);
    EXPECT_EQ(1, wcmp_group_entry_ptr->wcmp_group_members.size());
    VerifyWcmpGroupMemberEntry(kNexthopId1, 1,
                               wcmp_group_entry_ptr->wcmp_group_members[0]);
    EXPECT_TRUE(p4_oid_mapper_->existsOID(SAI_OBJECT_TYPE_NEXT_HOP_GROUP, key));
    uint32_t ref_cnt;
    EXPECT_TRUE(p4_oid_mapper_->getRefCount(SAI_OBJECT_TYPE_NEXT_HOP, kNexthopKey1, &ref_cnt));
    EXPECT_EQ(1, ref_cnt);

    // Update WCMP group with exact same members.
    Enqueue(swss::KeyOpFieldsValuesTuple(kKeyPrefix + j.dump(), SET_COMMAND, attributes));

    std::vector<sai_status_t> exp_status{SAI_STATUS_SUCCESS,
                                         SAI_STATUS_SUCCESS};
    EXPECT_CALL(mock_sai_next_hop_group_,
                set_next_hop_groups_attribute(
                    Eq(2),
                    ArrayEq(std::vector<sai_object_id_t>{kWcmpGroupOid1,
                                                         kWcmpGroupOid1}),
                    Truly(std::bind(MatchSaiNextHopGroupAttribute,
                                    std::placeholders::_1,
                                    *wcmp_group_entry_ptr, /*update=*/true)),
                    _, _))
        .WillOnce(
            DoAll(SetArrayArgument<4>(exp_status.begin(), exp_status.end()),
                  Return(SAI_STATUS_SUCCESS)));
    EXPECT_CALL(
        *gMockResponsePublisher,
        publish(Eq(APP_P4RT_TABLE_NAME), Eq(kKeyPrefix + j.dump()),
                Eq(attributes), Eq(StatusCode::SWSS_RC_SUCCESS), Eq(true)));
    EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS, Drain(/*failure_before=*/false));
    wcmp_group_entry_ptr = GetWcmpGroupEntry(kWcmpGroupId1);
    EXPECT_NE(nullptr, wcmp_group_entry_ptr);
    EXPECT_EQ(1, wcmp_group_entry_ptr->wcmp_group_members.size());
    VerifyWcmpGroupMemberEntry(kNexthopId1, 1,
                               wcmp_group_entry_ptr->wcmp_group_members[0]);
    EXPECT_TRUE(p4_oid_mapper_->existsOID(SAI_OBJECT_TYPE_NEXT_HOP_GROUP, key));
    EXPECT_TRUE(p4_oid_mapper_->getRefCount(SAI_OBJECT_TYPE_NEXT_HOP, kNexthopKey1, &ref_cnt));
    EXPECT_EQ(1, ref_cnt);

    // Update WCMP group with member {next_hop_id=kNexthopId2, weight=1}
    actions.clear();
    action[prependParamField(p4orch::kNexthopId)] = kNexthopId2;
    actions.push_back(action);
    attributes.clear();
    attributes.push_back(swss::FieldValueTuple{p4orch::kActions, actions.dump()});
    Enqueue(swss::KeyOpFieldsValuesTuple(kKeyPrefix + j.dump(), SET_COMMAND, attributes));

    auto exp_group = *wcmp_group_entry_ptr;
    exp_group.wcmp_group_members =
        std::vector<std::shared_ptr<P4WcmpGroupMemberEntry>>{
            std::make_shared<P4WcmpGroupMemberEntry>()};
    exp_group.wcmp_group_members[0]->next_hop_id = kNexthopId2;
    exp_group.wcmp_group_members[0]->weight = 1;
    exp_group.wcmp_group_members[0]->next_hop_oid = kNexthopOid2;
    EXPECT_CALL(
        mock_sai_next_hop_group_,
        set_next_hop_groups_attribute(
            Eq(2),
            ArrayEq(
                std::vector<sai_object_id_t>{kWcmpGroupOid1, kWcmpGroupOid1}),
            Truly(std::bind(MatchSaiNextHopGroupAttribute,
                            std::placeholders::_1, exp_group, /*update=*/true)),
            _, _))
        .WillOnce(
            DoAll(SetArrayArgument<4>(exp_status.begin(), exp_status.end()),
                  Return(SAI_STATUS_SUCCESS)));
    EXPECT_CALL(
        *gMockResponsePublisher,
        publish(Eq(APP_P4RT_TABLE_NAME), Eq(kKeyPrefix + j.dump()),
                Eq(attributes), Eq(StatusCode::SWSS_RC_SUCCESS), Eq(true)));
    EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS, Drain(/*failure_before=*/false));
    wcmp_group_entry_ptr = GetWcmpGroupEntry(kWcmpGroupId1);
    EXPECT_NE(nullptr, wcmp_group_entry_ptr);
    EXPECT_EQ(1, wcmp_group_entry_ptr->wcmp_group_members.size());
    VerifyWcmpGroupMemberEntry(kNexthopId2, 1,
                               wcmp_group_entry_ptr->wcmp_group_members[0]);
    EXPECT_TRUE(p4_oid_mapper_->existsOID(SAI_OBJECT_TYPE_NEXT_HOP_GROUP, key));
    EXPECT_TRUE(p4_oid_mapper_->getRefCount(SAI_OBJECT_TYPE_NEXT_HOP, kNexthopKey1, &ref_cnt));
    EXPECT_EQ(0, ref_cnt);
    EXPECT_TRUE(p4_oid_mapper_->getRefCount(SAI_OBJECT_TYPE_NEXT_HOP, kNexthopKey2, &ref_cnt));
    EXPECT_EQ(1, ref_cnt);
    // Update WCMP group with member {next_hop_id=kNexthopId2, weight=2}
    actions.clear();
    action[p4orch::kWeight] = 2;
    actions.push_back(action);
    attributes.clear();
    attributes.push_back(swss::FieldValueTuple{p4orch::kActions, actions.dump()});
    Enqueue(swss::KeyOpFieldsValuesTuple(kKeyPrefix + j.dump(), SET_COMMAND, attributes));

    exp_group = *wcmp_group_entry_ptr;
    exp_group.wcmp_group_members =
        std::vector<std::shared_ptr<P4WcmpGroupMemberEntry>>{
            std::make_shared<P4WcmpGroupMemberEntry>()};
    exp_group.wcmp_group_members[0]->next_hop_id = kNexthopId2;
    exp_group.wcmp_group_members[0]->weight = 2;
    exp_group.wcmp_group_members[0]->next_hop_oid = kNexthopOid2;
    EXPECT_CALL(
        mock_sai_next_hop_group_,
        set_next_hop_groups_attribute(
            Eq(2),
            ArrayEq(
                std::vector<sai_object_id_t>{kWcmpGroupOid1, kWcmpGroupOid1}),
            Truly(std::bind(MatchSaiNextHopGroupAttribute,
                            std::placeholders::_1, exp_group, /*update=*/true)),
            _, _))
        .WillOnce(
            DoAll(SetArrayArgument<4>(exp_status.begin(), exp_status.end()),
                  Return(SAI_STATUS_SUCCESS)));
    EXPECT_CALL(
        *gMockResponsePublisher,
        publish(Eq(APP_P4RT_TABLE_NAME), Eq(kKeyPrefix + j.dump()),
                Eq(attributes), Eq(StatusCode::SWSS_RC_SUCCESS), Eq(true)));
    EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS, Drain(/*failure_before=*/false));
    wcmp_group_entry_ptr = GetWcmpGroupEntry(kWcmpGroupId1);
    EXPECT_NE(nullptr, wcmp_group_entry_ptr);
    EXPECT_EQ(1, wcmp_group_entry_ptr->wcmp_group_members.size());
    VerifyWcmpGroupMemberEntry(kNexthopId2, 2,
                               wcmp_group_entry_ptr->wcmp_group_members[0]);
    EXPECT_TRUE(p4_oid_mapper_->existsOID(SAI_OBJECT_TYPE_NEXT_HOP_GROUP, key));
    EXPECT_TRUE(p4_oid_mapper_->getRefCount(SAI_OBJECT_TYPE_NEXT_HOP, kNexthopKey2, &ref_cnt));
    EXPECT_EQ(1, ref_cnt);
}

TEST_F(WcmpManagerTest, DeserializeWcmpGroup)
{
    std::string key = R"({"match/wcmp_group_id":"group-a"})";
    std::vector<swss::FieldValueTuple> attributes;
    nlohmann::json actions;
    nlohmann::json action;
    action[p4orch::kAction] = p4orch::kSetNexthopId;
    action[p4orch::kWeight] = 2;
    action[prependParamField(p4orch::kNexthopId)] = kNexthopId1;
    actions.push_back(action);
    action[p4orch::kWeight] = 1;
    action[prependParamField(p4orch::kNexthopId)] = kNexthopId2;
    actions.push_back(action);
    attributes.push_back(swss::FieldValueTuple{p4orch::kActions, actions.dump()});
    auto wcmp_group_entry_or = DeserializeP4WcmpGroupAppDbEntry(key, attributes);
    EXPECT_TRUE(wcmp_group_entry_or.ok());
    auto &wcmp_group_entry = *wcmp_group_entry_or;
    P4WcmpGroupEntry expect_entry = {};
    expect_entry.wcmp_group_id = "group-a";
    std::shared_ptr<P4WcmpGroupMemberEntry> gm_entry1 = createWcmpGroupMemberEntry(kNexthopId1, 2);
    expect_entry.wcmp_group_members.push_back(gm_entry1);
    std::shared_ptr<P4WcmpGroupMemberEntry> gm_entry2 = createWcmpGroupMemberEntry(kNexthopId2, 1);
    expect_entry.wcmp_group_members.push_back(gm_entry2);
    VerifyWcmpGroupEntry(expect_entry, wcmp_group_entry);
}

TEST_F(WcmpManagerTest, DeserializeWcmpGroupDuplicateGroupMembers)
{
    std::string key = R"({"match/wcmp_group_id":"group-a"})";
    std::vector<swss::FieldValueTuple> attributes;
    nlohmann::json actions;
    nlohmann::json action;
    action[p4orch::kAction] = p4orch::kSetNexthopId;
    action[p4orch::kWeight] = 1;
    action[prependParamField(p4orch::kNexthopId)] = kNexthopId1;
    actions.push_back(action);
    action[prependParamField(p4orch::kNexthopId)] = kNexthopId2;
    actions.push_back(action);
    action[prependParamField(p4orch::kNexthopId)] = kNexthopId1;
    actions.push_back(action);
    attributes.push_back(swss::FieldValueTuple{p4orch::kActions, actions.dump()});
    auto return_code_or = DeserializeP4WcmpGroupAppDbEntry(key, attributes);
    EXPECT_TRUE(return_code_or.ok());
}

TEST_F(WcmpManagerTest, DeserializeWcmpGroupFailsWhenGroupKeyIsInvalidJson)
{
    std::vector<swss::FieldValueTuple> attributes;
    nlohmann::json actions;
    nlohmann::json action;
    action[p4orch::kAction] = p4orch::kSetNexthopId;
    action[p4orch::kWeight] = 1;
    action[prependParamField(p4orch::kNexthopId)] = kNexthopId1;
    actions.push_back(action);
    attributes.push_back(swss::FieldValueTuple{p4orch::kActions, actions.dump()});
    // Invalid JSON
    std::string key = R"("match/wcmp_group_id":"group-a"})";
    EXPECT_FALSE(DeserializeP4WcmpGroupAppDbEntry(key, attributes).ok());
    // Is string not JSON
    key = R"("group-a")";
    EXPECT_FALSE(DeserializeP4WcmpGroupAppDbEntry(key, attributes).ok());
}

TEST_F(WcmpManagerTest, DeserializeWcmpGroupFailsWhenActionsStringIsInvalid)
{
    std::string key = R"({"match/wcmp_group_id":"group-a"})";
    std::vector<swss::FieldValueTuple> attributes;
    // Actions field is an invalid JSON
    attributes.push_back(swss::FieldValueTuple{p4orch::kActions, "Undefied"});
    EXPECT_FALSE(DeserializeP4WcmpGroupAppDbEntry(key, attributes).ok());

    attributes.clear();
    nlohmann::json action;
    action[p4orch::kAction] = kSetNexthopId;
    action[p4orch::kWeight] = 1;
    action[prependParamField(p4orch::kNexthopId)] = kNexthopId1;
    // Actions field is not an array
    attributes.push_back(swss::FieldValueTuple{p4orch::kActions, action.dump()});
    EXPECT_FALSE(DeserializeP4WcmpGroupAppDbEntry(key, attributes).ok());

    attributes.clear();
    nlohmann::json actions;
    action[p4orch::kAction] = "Undefined";
    actions.push_back(action);
    // Actions field has undefiend action
    attributes.push_back(swss::FieldValueTuple{p4orch::kActions, actions.dump()});
    EXPECT_FALSE(DeserializeP4WcmpGroupAppDbEntry(key, attributes).ok());

    attributes.clear();
    actions.clear();
    action.clear();
    action[p4orch::kAction] = kSetNexthopId;
    action[p4orch::kWeight] = 1;
    actions.push_back(action);
    // Actions field has the group member without next_hop_id field
    attributes.push_back(swss::FieldValueTuple{p4orch::kActions, actions.dump()});
    EXPECT_FALSE(DeserializeP4WcmpGroupAppDbEntry(key, attributes).ok());
    attributes.clear();
    actions.clear();
    action[p4orch::kAction] = kSetNexthopId;
    action[p4orch::kWeight] = 1;
    action[prependParamField(p4orch::kNexthopId)] = kNexthopId1;
    actions.push_back(action);
    actions.push_back(action);
    // Actions field has multiple group members have the same next_hop_id
    attributes.push_back(swss::FieldValueTuple{p4orch::kActions, actions.dump()});
    EXPECT_TRUE(DeserializeP4WcmpGroupAppDbEntry(key, attributes).ok());
}

TEST_F(WcmpManagerTest, DeserializeWcmpGroupFailsWithUndefinedAttributes)
{
    std::string key = R"({"match/wcmp_group_id":"group-a"})";
    std::vector<swss::FieldValueTuple> attributes;
    // Undefined field in attribute list
    attributes.push_back(swss::FieldValueTuple{"Undefined", "Undefined"});
    EXPECT_FALSE(DeserializeP4WcmpGroupAppDbEntry(key, attributes).ok());
}

TEST_F(WcmpManagerTest, ValidateWcmpGroupEntryWithInvalidWatchportAttributeFails)
{
    const std::string kKeyPrefix = std::string(APP_P4RT_WCMP_GROUP_TABLE_NAME) + kTableKeyDelimiter;
    p4_oid_mapper_->setOID(SAI_OBJECT_TYPE_NEXT_HOP, kNexthopKey1, kNexthopOid1);
    nlohmann::json j;
    j[prependMatchField(p4orch::kWcmpGroupId)] = kWcmpGroupId1;
    std::vector<swss::FieldValueTuple> attributes;
    nlohmann::json actions;
    nlohmann::json action;
    action[p4orch::kAction] = p4orch::kSetNexthopId;
    action[p4orch::kWatchPort] = "EthernetXX";
    action[prependParamField(p4orch::kNexthopId)] = kNexthopId1;
    actions.push_back(action);
    attributes.push_back(swss::FieldValueTuple{p4orch::kActions, actions.dump()});
    Enqueue(swss::KeyOpFieldsValuesTuple(kKeyPrefix + j.dump(), SET_COMMAND, attributes));
    EXPECT_CALL(*gMockResponsePublisher,
                publish(Eq(APP_P4RT_TABLE_NAME), Eq(kKeyPrefix + j.dump()),
                        Eq(attributes), Eq(StatusCode::SWSS_RC_INVALID_PARAM),
                        Eq(true)));
    EXPECT_EQ(StatusCode::SWSS_RC_INVALID_PARAM,
              Drain(/*failure_before=*/false));
    std::string key = KeyGenerator::generateWcmpGroupKey(kWcmpGroupId1);
    auto *wcmp_group_entry_ptr = GetWcmpGroupEntry(kWcmpGroupId1);
    EXPECT_EQ(nullptr, wcmp_group_entry_ptr);
    EXPECT_FALSE(p4_oid_mapper_->existsOID(SAI_OBJECT_TYPE_NEXT_HOP_GROUP, key));
    uint32_t ref_cnt;
    EXPECT_TRUE(p4_oid_mapper_->getRefCount(SAI_OBJECT_TYPE_NEXT_HOP, kNexthopKey1, &ref_cnt));
    EXPECT_EQ(0, ref_cnt);
}

TEST_F(WcmpManagerTest, PruneNextHopSucceeds)
{
    // Add member with operationally up watch port
    std::string port_name = "Ethernet6";
    P4WcmpGroupEntry app_db_entry = AddWcmpGroupEntryWithWatchport(port_name, true);
    EXPECT_TRUE(VerifyWcmpGroupMemberInPortMap(app_db_entry.wcmp_group_members[0], true, 1));
    EXPECT_FALSE(app_db_entry.wcmp_group_members[0]->pruned);

    bool pruned = app_db_entry.wcmp_group_members[0]->pruned;
    app_db_entry.wcmp_group_members[0]->pruned = true;
    std::vector<sai_status_t> exp_status{SAI_STATUS_SUCCESS,
                                         SAI_STATUS_SUCCESS};
    EXPECT_CALL(mock_sai_next_hop_group_,
                set_next_hop_groups_attribute(
                    Eq(2),
                    ArrayEq(std::vector<sai_object_id_t>{kWcmpGroupOid1,
                                                         kWcmpGroupOid1}),
                    Truly(std::bind(MatchSaiNextHopGroupAttribute,
                                    std::placeholders::_1, app_db_entry,
                                    /*update=*/true)),
                    _, _))
        .WillOnce(
            DoAll(SetArrayArgument<4>(exp_status.begin(), exp_status.end()),
                  Return(SAI_STATUS_SUCCESS)));
    app_db_entry.wcmp_group_members[0]->pruned = pruned;

    // Prune next hops associated with port
    PruneNextHops(port_name);
    EXPECT_TRUE(VerifyWcmpGroupMemberInPortMap(app_db_entry.wcmp_group_members[0], true, 1));
    EXPECT_TRUE(app_db_entry.wcmp_group_members[0]->pruned);
}

TEST_F(WcmpManagerTest, PruneNextHopFails) {
  // Add member with operationally up watch port
  std::string port_name = "Ethernet6";
  P4WcmpGroupEntry app_db_entry =
      AddWcmpGroupEntryWithWatchport(port_name, true);
  EXPECT_TRUE(VerifyWcmpGroupMemberInPortMap(app_db_entry.wcmp_group_members[0],
                                             true, 1));
  EXPECT_FALSE(app_db_entry.wcmp_group_members[0]->pruned);

  bool pruned = app_db_entry.wcmp_group_members[0]->pruned;
  app_db_entry.wcmp_group_members[0]->pruned = true;
  std::vector<sai_status_t> exp_status{SAI_STATUS_FAILURE, SAI_STATUS_FAILURE};
  EXPECT_CALL(
      mock_sai_next_hop_group_,
      set_next_hop_groups_attribute(
          Eq(2),
          ArrayEq(std::vector<sai_object_id_t>{kWcmpGroupOid1, kWcmpGroupOid1}),
          Truly(std::bind(MatchSaiNextHopGroupAttribute, std::placeholders::_1,
                          app_db_entry, /*update=*/true)),
          _, _))
      .WillOnce(DoAll(SetArrayArgument<4>(exp_status.begin(), exp_status.end()),
                      Return(SAI_STATUS_FAILURE)));
  app_db_entry.wcmp_group_members[0]->pruned = pruned;

  // TODO: Expect critical state.
  // Prune next hops associated with port (fails)
  PruneNextHops(port_name);
  EXPECT_TRUE(VerifyWcmpGroupMemberInPortMap(app_db_entry.wcmp_group_members[0],
                                             true, 1));
  EXPECT_FALSE(app_db_entry.wcmp_group_members[0]->pruned);
}

TEST_F(WcmpManagerTest, RestorePrunedNextHopSucceeds)
{
    // Add member with operationally down watch port. Since associated watchport
    // is operationally down, member will not be created in SAI but will be
    // directly added to the pruned set of WCMP group members.
    std::string port_name = "Ethernet1";
    P4WcmpGroupEntry app_db_entry = AddWcmpGroupEntryWithWatchport(port_name);
    EXPECT_TRUE(VerifyWcmpGroupMemberInPortMap(app_db_entry.wcmp_group_members[0], true, 1));
    EXPECT_TRUE(app_db_entry.wcmp_group_members[0]->pruned);

    bool pruned = app_db_entry.wcmp_group_members[0]->pruned;
    app_db_entry.wcmp_group_members[0]->pruned = false;
    std::vector<sai_status_t> exp_status{SAI_STATUS_SUCCESS,
                                         SAI_STATUS_SUCCESS};
    EXPECT_CALL(mock_sai_next_hop_group_,
                set_next_hop_groups_attribute(
                    Eq(2),
                    ArrayEq(std::vector<sai_object_id_t>{kWcmpGroupOid1,
                                                         kWcmpGroupOid1}),
                    Truly(std::bind(MatchSaiNextHopGroupAttribute,
                                    std::placeholders::_1, app_db_entry,
                                    /*update=*/true)),
                    _, _))
        .WillOnce(
            DoAll(SetArrayArgument<4>(exp_status.begin(), exp_status.end()),
                  Return(SAI_STATUS_SUCCESS)));
    app_db_entry.wcmp_group_members[0]->pruned = pruned;

    // Restore next hops associated with port
    RestorePrunedNextHops(port_name);
    EXPECT_TRUE(VerifyWcmpGroupMemberInPortMap(app_db_entry.wcmp_group_members[0], true, 1));
    EXPECT_FALSE(app_db_entry.wcmp_group_members[0]->pruned);
}

TEST_F(WcmpManagerTest, RestorePrunedNextHopFails) {
  // Add member with operationally down watch port. Since associated watchport
  // is operationally down, member will not be created in SAI but will be
  // directly added to the pruned set of WCMP group members.
  std::string port_name = "Ethernet1";
  P4WcmpGroupEntry app_db_entry = AddWcmpGroupEntryWithWatchport(port_name);
  EXPECT_TRUE(VerifyWcmpGroupMemberInPortMap(app_db_entry.wcmp_group_members[0],
                                             true, 1));
  EXPECT_TRUE(app_db_entry.wcmp_group_members[0]->pruned);

  bool pruned = app_db_entry.wcmp_group_members[0]->pruned;
  app_db_entry.wcmp_group_members[0]->pruned = false;
  std::vector<sai_status_t> exp_status{SAI_STATUS_FAILURE, SAI_STATUS_FAILURE};

  EXPECT_CALL(
      mock_sai_next_hop_group_,
      set_next_hop_groups_attribute(
          Eq(2),
          ArrayEq(std::vector<sai_object_id_t>{kWcmpGroupOid1, kWcmpGroupOid1}),
          Truly(std::bind(MatchSaiNextHopGroupAttribute, std::placeholders::_1,
                          app_db_entry, /*update=*/true)),
          _, _))
      .WillOnce(DoAll(SetArrayArgument<4>(exp_status.begin(), exp_status.end()),
                      Return(SAI_STATUS_FAILURE)));
  app_db_entry.wcmp_group_members[0]->pruned = pruned;

  // TODO: Expect critical state.
  RestorePrunedNextHops(port_name);
  EXPECT_TRUE(VerifyWcmpGroupMemberInPortMap(app_db_entry.wcmp_group_members[0],
                                             true, 1));
  EXPECT_TRUE(app_db_entry.wcmp_group_members[0]->pruned);
}

TEST_F(WcmpManagerTest, RemoveWcmpGroupAfterPruningSucceeds)
{
    // Add member with operationally up watch port
    std::string port_name = "Ethernet6";
    P4WcmpGroupEntry app_db_entry = AddWcmpGroupEntryWithWatchport(port_name, true);
    EXPECT_TRUE(VerifyWcmpGroupMemberInPortMap(app_db_entry.wcmp_group_members[0], true, 1));
    EXPECT_FALSE(app_db_entry.wcmp_group_members[0]->pruned);

    bool pruned = app_db_entry.wcmp_group_members[0]->pruned;
    app_db_entry.wcmp_group_members[0]->pruned = true;
    std::vector<sai_status_t> exp_status{SAI_STATUS_SUCCESS,
                                         SAI_STATUS_SUCCESS};
    EXPECT_CALL(mock_sai_next_hop_group_,
                set_next_hop_groups_attribute(
                    Eq(2),
                    ArrayEq(std::vector<sai_object_id_t>{kWcmpGroupOid1,
                                                         kWcmpGroupOid1}),
                    Truly(std::bind(MatchSaiNextHopGroupAttribute,
                                    std::placeholders::_1, app_db_entry,
                                    /*update=*/true)),
                    _, _))
        .WillOnce(
            DoAll(SetArrayArgument<4>(exp_status.begin(), exp_status.end()),
                  Return(SAI_STATUS_SUCCESS)));
    app_db_entry.wcmp_group_members[0]->pruned = pruned;

    PruneNextHops(port_name);
    EXPECT_TRUE(VerifyWcmpGroupMemberInPortMap(app_db_entry.wcmp_group_members[0], true, 1));
    EXPECT_TRUE(app_db_entry.wcmp_group_members[0]->pruned);

    // Remove Wcmp group. No SAI call for member removal is expected as it is
    // already pruned.
    EXPECT_CALL(mock_sai_next_hop_group_, remove_next_hop_group(Eq(kWcmpGroupOid1)))
        .WillOnce(Return(SAI_STATUS_SUCCESS));
    EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS, RemoveWcmpGroup(kWcmpGroupId1));
    EXPECT_TRUE(VerifyWcmpGroupMemberInPortMap(app_db_entry.wcmp_group_members[0], false, 0));
}

TEST_F(WcmpManagerTest, RemoveWcmpGroupWithOperationallyDownWatchportSucceeds)
{
    // Add member with operationally down watch port. Since associated watchport
    // is operationally down, member will not be created in SAI but will be
    // directly added to the pruned set of WCMP group members.
    P4WcmpGroupEntry app_db_entry = AddWcmpGroupEntryWithWatchport("Ethernet1");
    EXPECT_TRUE(VerifyWcmpGroupMemberInPortMap(app_db_entry.wcmp_group_members[0], true, 1));
    EXPECT_TRUE(app_db_entry.wcmp_group_members[0]->pruned);

    // Remove Wcmp group. No SAI call for member removal is expected as it is
    // already pruned.
    EXPECT_CALL(mock_sai_next_hop_group_, remove_next_hop_group(Eq(kWcmpGroupOid1)))
        .WillOnce(Return(SAI_STATUS_SUCCESS));
    EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS, RemoveWcmpGroup(kWcmpGroupId1));
    EXPECT_TRUE(VerifyWcmpGroupMemberInPortMap(app_db_entry.wcmp_group_members[0], false, 0));
}

TEST_F(WcmpManagerTest, RemoveNextHopWithPrunedMember)
{
    // Add member with operationally down watch port. Since associated watchport
    // is operationally down, member will not be created in SAI but will be
    // directly added to the pruned set of WCMP group members.
    P4WcmpGroupEntry app_db_entry = AddWcmpGroupEntryWithWatchport("Ethernet1");
    EXPECT_TRUE(VerifyWcmpGroupMemberInPortMap(app_db_entry.wcmp_group_members[0], true, 1));
    EXPECT_TRUE(app_db_entry.wcmp_group_members[0]->pruned);

    // Verify that next hop reference count is incremented due to the member.
    uint32_t ref_cnt;
    EXPECT_TRUE(p4_oid_mapper_->getRefCount(SAI_OBJECT_TYPE_NEXT_HOP, kNexthopKey1, &ref_cnt));
    EXPECT_EQ(1, ref_cnt);

    // Remove Wcmp group. No SAI call for member removal is expected as it is
    // already pruned.
    EXPECT_CALL(mock_sai_next_hop_group_, remove_next_hop_group(Eq(kWcmpGroupOid1)))
        .WillOnce(Return(SAI_STATUS_SUCCESS));
    EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS, RemoveWcmpGroup(kWcmpGroupId1));
    EXPECT_TRUE(VerifyWcmpGroupMemberInPortMap(app_db_entry.wcmp_group_members[0], false, 0));

    // Verify that the next hop reference count is now 0.
    EXPECT_TRUE(p4_oid_mapper_->getRefCount(SAI_OBJECT_TYPE_NEXT_HOP, kNexthopKey1, &ref_cnt));
    EXPECT_EQ(0, ref_cnt);
}

TEST_F(WcmpManagerTest, RemoveNextHopWithRestoredPrunedMember)
{
    // Add member with operationally down watch port. Since associated watchport
    // is operationally down, member will not be created in SAI but will be
    // directly added to the pruned set of WCMP group members.
    std::string port_name = "Ethernet1";
    P4WcmpGroupEntry app_db_entry = AddWcmpGroupEntryWithWatchport(port_name);
    EXPECT_TRUE(VerifyWcmpGroupMemberInPortMap(app_db_entry.wcmp_group_members[0], true, 1));
    EXPECT_TRUE(app_db_entry.wcmp_group_members[0]->pruned);

    // Verify that next hop reference count is incremented due to the member.
    uint32_t ref_cnt;
    EXPECT_TRUE(p4_oid_mapper_->getRefCount(SAI_OBJECT_TYPE_NEXT_HOP, kNexthopKey1, &ref_cnt));
    EXPECT_EQ(1, ref_cnt);

    // Restore member associated with port.
    bool pruned = app_db_entry.wcmp_group_members[0]->pruned;
    app_db_entry.wcmp_group_members[0]->pruned = false;
    std::vector<sai_status_t> exp_status{SAI_STATUS_SUCCESS,
                                         SAI_STATUS_SUCCESS};
    EXPECT_CALL(mock_sai_next_hop_group_,
                set_next_hop_groups_attribute(
                    Eq(2),
                    ArrayEq(std::vector<sai_object_id_t>{kWcmpGroupOid1,
                                                         kWcmpGroupOid1}),
                    Truly(std::bind(MatchSaiNextHopGroupAttribute,
                                    std::placeholders::_1, app_db_entry,
                                    /*update=*/true)),
                    _, _))
        .WillOnce(
            DoAll(SetArrayArgument<4>(exp_status.begin(), exp_status.end()),
                  Return(SAI_STATUS_SUCCESS)));
    app_db_entry.wcmp_group_members[0]->pruned = pruned;

    RestorePrunedNextHops(port_name);
    EXPECT_TRUE(VerifyWcmpGroupMemberInPortMap(app_db_entry.wcmp_group_members[0], true, 1));
    EXPECT_FALSE(app_db_entry.wcmp_group_members[0]->pruned);

    // Verify that next hop reference count remains the same after restore.
    EXPECT_TRUE(p4_oid_mapper_->getRefCount(SAI_OBJECT_TYPE_NEXT_HOP, kNexthopKey1, &ref_cnt));
    EXPECT_EQ(1, ref_cnt);

    // Remove Wcmp group.
    EXPECT_CALL(mock_sai_next_hop_group_, remove_next_hop_group(Eq(kWcmpGroupOid1)))
        .WillOnce(Return(SAI_STATUS_SUCCESS));
    EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS, RemoveWcmpGroup(kWcmpGroupId1));
    EXPECT_TRUE(VerifyWcmpGroupMemberInPortMap(app_db_entry.wcmp_group_members[0], false, 0));

    // Verify that the next hop reference count is now 0.
    EXPECT_TRUE(p4_oid_mapper_->getRefCount(SAI_OBJECT_TYPE_NEXT_HOP, kNexthopKey1, &ref_cnt));
    EXPECT_EQ(0, ref_cnt);
}

TEST_F(WcmpManagerTest, VerifyNextHopRefCountWhenMemberPruned)
{
    // Add member with operationally up watch port
    std::string port_name = "Ethernet6";
    P4WcmpGroupEntry app_db_entry = AddWcmpGroupEntryWithWatchport(port_name, true);
    EXPECT_TRUE(VerifyWcmpGroupMemberInPortMap(app_db_entry.wcmp_group_members[0], true, 1));
    EXPECT_FALSE(app_db_entry.wcmp_group_members[0]->pruned);

    // Verify that next hop reference count is incremented due to the member.
    uint32_t ref_cnt;
    EXPECT_TRUE(p4_oid_mapper_->getRefCount(SAI_OBJECT_TYPE_NEXT_HOP, kNexthopKey1, &ref_cnt));
    EXPECT_EQ(1, ref_cnt);

    // Prune member associated with port.
    bool pruned = app_db_entry.wcmp_group_members[0]->pruned;
    app_db_entry.wcmp_group_members[0]->pruned = true;
    std::vector<sai_status_t> exp_status{SAI_STATUS_SUCCESS,
                                         SAI_STATUS_SUCCESS};
    EXPECT_CALL(mock_sai_next_hop_group_,
                set_next_hop_groups_attribute(
                    Eq(2),
                    ArrayEq(std::vector<sai_object_id_t>{kWcmpGroupOid1,
                                                         kWcmpGroupOid1}),
                    Truly(std::bind(MatchSaiNextHopGroupAttribute,
                                    std::placeholders::_1, app_db_entry,
                                    /*update=*/true)),
                    _, _))
        .WillOnce(
            DoAll(SetArrayArgument<4>(exp_status.begin(), exp_status.end()),
                  Return(SAI_STATUS_SUCCESS)));
    app_db_entry.wcmp_group_members[0]->pruned = pruned;

    PruneNextHops(port_name);
    EXPECT_TRUE(VerifyWcmpGroupMemberInPortMap(app_db_entry.wcmp_group_members[0], true, 1));
    EXPECT_TRUE(app_db_entry.wcmp_group_members[0]->pruned);

    // Verify that next hop reference count does not change on pruning.
    EXPECT_TRUE(p4_oid_mapper_->getRefCount(SAI_OBJECT_TYPE_NEXT_HOP, kNexthopKey1, &ref_cnt));
    EXPECT_EQ(1, ref_cnt);
}

TEST_F(WcmpManagerTest, UpdateWcmpGroupWithOperationallyUpWatchportMemberSucceeds)
{
    // Add member with operationally up watch port
    std::string port_name = "Ethernet6";
    P4WcmpGroupEntry app_db_entry = AddWcmpGroupEntryWithWatchport(port_name, true);
    EXPECT_TRUE(VerifyWcmpGroupMemberInPortMap(app_db_entry.wcmp_group_members[0], true, 1));
    EXPECT_FALSE(app_db_entry.wcmp_group_members[0]->pruned);

    // Update WCMP group to remove kNexthopId1 and add kNexthopId2
    P4WcmpGroupEntry updated_app_db_entry;
    updated_app_db_entry.wcmp_group_id = kWcmpGroupId1;
    std::shared_ptr<P4WcmpGroupMemberEntry> updated_gm =
        createWcmpGroupMemberEntryWithWatchport(kNexthopId2, 1, port_name, kWcmpGroupId1, kNexthopOid2);
    updated_app_db_entry.wcmp_group_members.push_back(updated_gm);

    std::vector<sai_status_t> exp_status{SAI_STATUS_SUCCESS,
                                         SAI_STATUS_SUCCESS};

    EXPECT_CALL(mock_sai_next_hop_group_,
                set_next_hop_groups_attribute(
                    Eq(2),
                    ArrayEq(std::vector<sai_object_id_t>{kWcmpGroupOid1,
                                                         kWcmpGroupOid1}),
                    Truly(std::bind(MatchSaiNextHopGroupAttribute,
                                    std::placeholders::_1, updated_app_db_entry,
                                    /*update=*/true)),
                    _, _))
        .WillOnce(
            DoAll(SetArrayArgument<4>(exp_status.begin(), exp_status.end()),
                  Return(SAI_STATUS_SUCCESS)));

    EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS, ProcessUpdateRequest(&updated_app_db_entry));
    EXPECT_TRUE(VerifyWcmpGroupMemberInPortMap(app_db_entry.wcmp_group_members[0], false, 1));
    EXPECT_TRUE(VerifyWcmpGroupMemberInPortMap(updated_gm, true, 1));
    EXPECT_FALSE(app_db_entry.wcmp_group_members[0]->pruned);
    EXPECT_FALSE(updated_gm->pruned);
}

TEST_F(WcmpManagerTest, UpdateWcmpGroupWithOperationallyDownWatchportMemberSucceeds)
{
    // Add member with operationally down watch port. Since associated watchport
    // is operationally down, member will not be created in SAI but will be
    // directly added to the pruned set of WCMP group members.
    std::string port_name = "Ethernet1";
    P4WcmpGroupEntry app_db_entry = AddWcmpGroupEntryWithWatchport(port_name);
    EXPECT_TRUE(VerifyWcmpGroupMemberInPortMap(app_db_entry.wcmp_group_members[0], true, 1));
    EXPECT_TRUE(app_db_entry.wcmp_group_members[0]->pruned);

    // Update WCMP group to remove kNexthopId1 and add kNexthopId2.
    P4WcmpGroupEntry updated_app_db_entry;
    updated_app_db_entry.wcmp_group_id = kWcmpGroupId1;
    std::shared_ptr<P4WcmpGroupMemberEntry> updated_gm =
        createWcmpGroupMemberEntryWithWatchport(kNexthopId2, 1, port_name, kWcmpGroupId1, kNexthopOid2);
    updated_app_db_entry.wcmp_group_members.push_back(updated_gm);

    std::vector<sai_status_t> exp_status{SAI_STATUS_SUCCESS,
                                         SAI_STATUS_SUCCESS};
    EXPECT_CALL(mock_sai_next_hop_group_,
                set_next_hop_groups_attribute(
                    Eq(2),
                    ArrayEq(std::vector<sai_object_id_t>{kWcmpGroupOid1,
                                                         kWcmpGroupOid1}),
                    Truly(std::bind(MatchSaiNextHopGroupAttribute,
                                    std::placeholders::_1, updated_app_db_entry,
                                    /*update=*/true)),
                    _, _))
        .WillOnce(
            DoAll(SetArrayArgument<4>(exp_status.begin(), exp_status.end()),
                  Return(SAI_STATUS_SUCCESS)));

    EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS, ProcessUpdateRequest(&updated_app_db_entry));
    EXPECT_TRUE(VerifyWcmpGroupMemberInPortMap(app_db_entry.wcmp_group_members[0], false, 1));
    EXPECT_TRUE(VerifyWcmpGroupMemberInPortMap(updated_gm, true, 1));
    EXPECT_TRUE(updated_gm->pruned);
}

TEST_F(WcmpManagerTest, PruneAfterWcmpGroupUpdateSucceeds)
{
    // Add member with operationally up watch port
    std::string port_name = "Ethernet6";
    P4WcmpGroupEntry app_db_entry = AddWcmpGroupEntryWithWatchport(port_name, true);
    EXPECT_TRUE(VerifyWcmpGroupMemberInPortMap(app_db_entry.wcmp_group_members[0], true, 1));
    EXPECT_FALSE(app_db_entry.wcmp_group_members[0]->pruned);

    // Update WCMP group to modify weight of kNexthopId1.
    P4WcmpGroupEntry updated_app_db_entry;
    updated_app_db_entry.wcmp_group_id = kWcmpGroupId1;
    std::shared_ptr<P4WcmpGroupMemberEntry> updated_gm =
        createWcmpGroupMemberEntryWithWatchport(kNexthopId1, 10, port_name, kWcmpGroupId1, kNexthopOid1);
    updated_app_db_entry.wcmp_group_members.push_back(updated_gm);
    std::vector<sai_status_t> exp_status{SAI_STATUS_SUCCESS,
                                         SAI_STATUS_SUCCESS};
    EXPECT_CALL(mock_sai_next_hop_group_,
                set_next_hop_groups_attribute(
                    Eq(2),
                    ArrayEq(std::vector<sai_object_id_t>{kWcmpGroupOid1,
                                                         kWcmpGroupOid1}),
                    Truly(std::bind(MatchSaiNextHopGroupAttribute,
                                    std::placeholders::_1, updated_app_db_entry,
                                    /*update=*/true)),
                    _, _))
        .WillOnce(
            DoAll(SetArrayArgument<4>(exp_status.begin(), exp_status.end()),
                  Return(SAI_STATUS_SUCCESS)));

    EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS, ProcessUpdateRequest(&updated_app_db_entry));
    EXPECT_TRUE(VerifyWcmpGroupMemberInPortMap(app_db_entry.wcmp_group_members[0], false, 1));
    EXPECT_TRUE(VerifyWcmpGroupMemberInPortMap(updated_app_db_entry.wcmp_group_members[0], true, 1));
    EXPECT_FALSE(updated_app_db_entry.wcmp_group_members[0]->pruned);

    // Prune members associated with port.
    bool pruned = updated_app_db_entry.wcmp_group_members[0]->pruned;
    updated_app_db_entry.wcmp_group_members[0]->pruned = true;
    EXPECT_CALL(mock_sai_next_hop_group_,
                set_next_hop_groups_attribute(
                    Eq(2),
                    ArrayEq(std::vector<sai_object_id_t>{kWcmpGroupOid1,
                                                         kWcmpGroupOid1}),
                    Truly(std::bind(MatchSaiNextHopGroupAttribute,
                                    std::placeholders::_1, updated_app_db_entry,
                                    /*update=*/true)),
                    _, _))
        .WillOnce(
            DoAll(SetArrayArgument<4>(exp_status.begin(), exp_status.end()),
                  Return(SAI_STATUS_SUCCESS)));
    updated_app_db_entry.wcmp_group_members[0]->pruned = pruned;

    PruneNextHops(port_name);
    EXPECT_TRUE(VerifyWcmpGroupMemberInPortMap(updated_app_db_entry.wcmp_group_members[0], true, 1));
    EXPECT_TRUE(updated_app_db_entry.wcmp_group_members[0]->pruned);

    // Remove Wcmp group. No SAI call for member removal is expected as it is
    // already pruned.
    // RemoveWcmpGroupWithOperationallyDownWatchportSucceeds verfies that SAI call
    // for pruned member is not made on group removal. Hence, the member must be
    // removed from SAI during prune.
    EXPECT_CALL(mock_sai_next_hop_group_, remove_next_hop_group(Eq(kWcmpGroupOid1)))
        .WillOnce(Return(SAI_STATUS_SUCCESS));
    EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS, RemoveWcmpGroup(kWcmpGroupId1));
    EXPECT_TRUE(VerifyWcmpGroupMemberInPortMap(updated_app_db_entry.wcmp_group_members[0], false, 0));
}

TEST_F(WcmpManagerTest, PrunedMemberUpdateOnRestoreSucceeds)
{
    // Add member with operationally down watch port. Since associated watchport
    // is operationally down, member will not be created in SAI but will be
    // directly added to the pruned set of WCMP group members.
    std::string port_name = "Ethernet1";
    P4WcmpGroupEntry app_db_entry = AddWcmpGroupEntryWithWatchport(port_name);
    EXPECT_TRUE(VerifyWcmpGroupMemberInPortMap(app_db_entry.wcmp_group_members[0], true, 1));
    EXPECT_TRUE(app_db_entry.wcmp_group_members[0]->pruned);

    // Update WCMP group to modify weight of kNexthopId1.
    P4WcmpGroupEntry updated_app_db_entry;
    updated_app_db_entry.wcmp_group_id = kWcmpGroupId1;
    std::shared_ptr<P4WcmpGroupMemberEntry> updated_gm =
        createWcmpGroupMemberEntryWithWatchport(kNexthopId1, 10, port_name, kWcmpGroupId1, kNexthopOid1);
    updated_app_db_entry.wcmp_group_members.push_back(updated_gm);

    std::vector<sai_status_t> exp_status{SAI_STATUS_SUCCESS,
                                         SAI_STATUS_SUCCESS};
    EXPECT_CALL(mock_sai_next_hop_group_,
                set_next_hop_groups_attribute(
                    Eq(2),
                    ArrayEq(std::vector<sai_object_id_t>{kWcmpGroupOid1,
                                                         kWcmpGroupOid1}),
                    Truly(std::bind(MatchSaiNextHopGroupAttribute,
                                    std::placeholders::_1, updated_app_db_entry,
                                    /*update=*/true)),
                    _, _))
        .WillOnce(
            DoAll(SetArrayArgument<4>(exp_status.begin(), exp_status.end()),
                  Return(SAI_STATUS_SUCCESS)));

    EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS, ProcessUpdateRequest(&updated_app_db_entry));
    EXPECT_TRUE(VerifyWcmpGroupMemberInPortMap(app_db_entry.wcmp_group_members[0], false, 1));
    EXPECT_TRUE(VerifyWcmpGroupMemberInPortMap(updated_app_db_entry.wcmp_group_members[0], true, 1));
    EXPECT_TRUE(updated_app_db_entry.wcmp_group_members[0]->pruned);

    // Restore members associated with port.
    // Verify that the weight of the restored member is updated.
    bool pruned = updated_app_db_entry.wcmp_group_members[0]->pruned;
    updated_app_db_entry.wcmp_group_members[0]->pruned = false;
    EXPECT_CALL(mock_sai_next_hop_group_,
                set_next_hop_groups_attribute(
                    Eq(2),
                    ArrayEq(std::vector<sai_object_id_t>{kWcmpGroupOid1,
                                                         kWcmpGroupOid1}),
                    Truly(std::bind(MatchSaiNextHopGroupAttribute,
                                    std::placeholders::_1, updated_app_db_entry,
                                    /*update=*/true)),
                    _, _))
        .WillOnce(
            DoAll(SetArrayArgument<4>(exp_status.begin(), exp_status.end()),
                  Return(SAI_STATUS_SUCCESS)));
    updated_app_db_entry.wcmp_group_members[0]->pruned = pruned;

    RestorePrunedNextHops(port_name);
    EXPECT_TRUE(VerifyWcmpGroupMemberInPortMap(updated_app_db_entry.wcmp_group_members[0], true, 1));
    EXPECT_FALSE(updated_app_db_entry.wcmp_group_members[0]->pruned);
}

TEST_F(WcmpManagerTest, WatchportStateChangetoOperDownSucceeds)
{
    // Add member with operationally up watch port
    std::string port_name = "Ethernet6";
    P4WcmpGroupEntry app_db_entry = AddWcmpGroupEntryWithWatchport(port_name, true);
    EXPECT_TRUE(VerifyWcmpGroupMemberInPortMap(app_db_entry.wcmp_group_members[0], true, 1));
    EXPECT_FALSE(app_db_entry.wcmp_group_members[0]->pruned);

    // Send port down signal
    // Verify that the next hop member associated with the port is pruned.
    std::string op = "port_state_change";
    std::string data = "[{\"port_id\":\"oid:0x56789abcdff\",\"port_state\":\"SAI_PORT_OPER_"
                       "STATUS_DOWN\",\"port_error_status\":\"0\"}]";

    bool pruned = app_db_entry.wcmp_group_members[0]->pruned;
    app_db_entry.wcmp_group_members[0]->pruned = false;
    std::vector<sai_status_t> exp_status{SAI_STATUS_SUCCESS,
                                         SAI_STATUS_SUCCESS};
    EXPECT_CALL(mock_sai_next_hop_group_,
                set_next_hop_groups_attribute(
                    Eq(2),
                    ArrayEq(std::vector<sai_object_id_t>{kWcmpGroupOid1,
                                                         kWcmpGroupOid1}),
                    Truly(std::bind(MatchSaiNextHopGroupAttribute,
                                    std::placeholders::_1, app_db_entry,
                                    /*update=*/true)),
                    _, _))
        .WillOnce(
            DoAll(SetArrayArgument<4>(exp_status.begin(), exp_status.end()),
                  Return(SAI_STATUS_SUCCESS)));
    app_db_entry.wcmp_group_members[0]->pruned = pruned;

    HandlePortStatusChangeNotification(op, data);
    EXPECT_TRUE(VerifyWcmpGroupMemberInPortMap(app_db_entry.wcmp_group_members[0], true, 1));
    EXPECT_TRUE(app_db_entry.wcmp_group_members[0]->pruned);
}

TEST_F(WcmpManagerTest, WatchportStateChangeToOperUpSucceeds)
{
    // Add member with operationally down watch port. Since associated watchport
    // is operationally down, member will not be created in SAI but will be
    // directly added to the pruned set of WCMP group members.
    std::string port_name = "Ethernet1";
    P4WcmpGroupEntry app_db_entry = AddWcmpGroupEntryWithWatchport(port_name);
    EXPECT_TRUE(VerifyWcmpGroupMemberInPortMap(app_db_entry.wcmp_group_members[0], true, 1));
    EXPECT_TRUE(app_db_entry.wcmp_group_members[0]->pruned);

    // Send port up signal.
    // Verify that the pruned next hop member associated with the port is
    // restored.
    std::string op = "port_state_change";
    std::string data = "[{\"port_id\":\"oid:0x112233\",\"port_state\":\"SAI_PORT_OPER_"
                       "STATUS_UP\",\"port_error_status\":\"0\"}]";

    bool pruned = app_db_entry.wcmp_group_members[0]->pruned;
    app_db_entry.wcmp_group_members[0]->pruned = true;
    std::vector<sai_status_t> exp_status{SAI_STATUS_SUCCESS,
                                         SAI_STATUS_SUCCESS};
    EXPECT_CALL(mock_sai_next_hop_group_,
                set_next_hop_groups_attribute(
                    Eq(2),
                    ArrayEq(std::vector<sai_object_id_t>{kWcmpGroupOid1,
                                                         kWcmpGroupOid1}),
                    Truly(std::bind(MatchSaiNextHopGroupAttribute,
                                    std::placeholders::_1, app_db_entry,
                                    /*update=*/true)),
                    _, _))
        .WillOnce(
            DoAll(SetArrayArgument<4>(exp_status.begin(), exp_status.end()),
                  Return(SAI_STATUS_SUCCESS)));
    app_db_entry.wcmp_group_members[0]->pruned = pruned;

    HandlePortStatusChangeNotification(op, data);
    EXPECT_TRUE(VerifyWcmpGroupMemberInPortMap(app_db_entry.wcmp_group_members[0], true, 1));
    EXPECT_FALSE(app_db_entry.wcmp_group_members[0]->pruned);
}

TEST_F(WcmpManagerTest, WatchportStateChangeFromOperUnknownToDownPrunesMemberOnlyOnceSuceeds)
{
    // Add member with operationally unknown watch port. Since associated
    // watchport is not operationally up, member will not be created in SAI but
    // will be directly added to the pruned set of WCMP group members.
    std::string port_name = "Ethernet1";
    P4WcmpGroupEntry app_db_entry = AddWcmpGroupEntryWithWatchport(port_name);
    EXPECT_TRUE(VerifyWcmpGroupMemberInPortMap(app_db_entry.wcmp_group_members[0], true, 1));
    EXPECT_TRUE(app_db_entry.wcmp_group_members[0]->pruned);

    // Send port down signal.
    // Verify that the pruned next hop member is not pruned again.
    std::string op = "port_state_change";
    std::string data = "[{\"port_id\":\"oid:0x56789abcfff\",\"port_state\":\"SAI_PORT_OPER_"
                       "STATUS_DOWN\",\"port_error_status\":\"0\"}]";
    HandlePortStatusChangeNotification(op, data);
    EXPECT_TRUE(VerifyWcmpGroupMemberInPortMap(app_db_entry.wcmp_group_members[0], true, 1));
    EXPECT_TRUE(app_db_entry.wcmp_group_members[0]->pruned);
}

TEST_F(WcmpManagerTest, VerifyStateTest)
{
    AddWcmpGroupEntryWithWatchport("Ethernet6", true);
    nlohmann::json j;
    j[prependMatchField(p4orch::kWcmpGroupId)] = kWcmpGroupId1;
    const std::string db_key = std::string(APP_P4RT_TABLE_NAME) + kTableKeyDelimiter + APP_P4RT_WCMP_GROUP_TABLE_NAME +
                               kTableKeyDelimiter + j.dump();
    std::vector<swss::FieldValueTuple> attributes;

    // Setup ASIC DB.
    swss::Table table(nullptr, "ASIC_STATE");
    table.set(
        "SAI_OBJECT_TYPE_NEXT_HOP_GROUP:oid:0xa",
        std::vector<swss::FieldValueTuple>{
            swss::FieldValueTuple{"SAI_NEXT_HOP_GROUP_ATTR_TYPE",
                                  "SAI_NEXT_HOP_GROUP_TYPE_ECMP_WITH_MEMBERS"},
            swss::FieldValueTuple{"SAI_NEXT_HOP_GROUP_ATTR_NEXT_HOP_LIST",
                                  "1:oid:0x1"},
            swss::FieldValueTuple{
                "SAI_NEXT_HOP_GROUP_ATTR_NEXT_HOP_MEMBER_WEIGHT_LIST", "1:2"}});

    // Verification should succeed with vaild key and value.
    nlohmann::json actions;
    nlohmann::json action;
    action[p4orch::kAction] = p4orch::kSetNexthopId;
    action[p4orch::kWeight] = 2;
    action[p4orch::kWatchPort] = "Ethernet6";
    action[prependParamField(p4orch::kNexthopId)] = kNexthopId1;
    actions.push_back(action);
    attributes.push_back(swss::FieldValueTuple{p4orch::kActions, actions.dump()});
    EXPECT_EQ(VerifyState(db_key, attributes), "");

    // Invalid key should fail verification.
    EXPECT_FALSE(VerifyState("invalid", attributes).empty());
    EXPECT_FALSE(VerifyState("invalid:invalid", attributes).empty());
    EXPECT_FALSE(VerifyState(std::string(APP_P4RT_TABLE_NAME) + ":invalid", attributes).empty());
    EXPECT_FALSE(VerifyState(std::string(APP_P4RT_TABLE_NAME) + ":invalid:invalid", attributes).empty());
    EXPECT_FALSE(VerifyState(std::string(APP_P4RT_TABLE_NAME) + ":FIXED_WCMP_GROUP_TABLE:invalid", attributes).empty());

    // Non-existing entry should fail verification.
    j[prependMatchField(p4orch::kWcmpGroupId)] = kWcmpGroupId2;
    EXPECT_FALSE(VerifyState(std::string(APP_P4RT_TABLE_NAME) + kTableKeyDelimiter + APP_P4RT_WCMP_GROUP_TABLE_NAME +
                                 kTableKeyDelimiter + j.dump(),
                             attributes)
                     .empty());

    // Non-existing nexthop should fail verification.
    actions.clear();
    attributes.clear();
    action[prependParamField(p4orch::kNexthopId)] = "invalid";
    actions.push_back(action);
    attributes.push_back(swss::FieldValueTuple{p4orch::kActions, actions.dump()});
    EXPECT_FALSE(VerifyState(db_key, attributes).empty());
    actions.clear();
    attributes.clear();
    action[p4orch::kAction] = p4orch::kSetNexthopId;
    action[p4orch::kWeight] = 2;
    action[p4orch::kWatchPort] = "Ethernet6";
    action[prependParamField(p4orch::kNexthopId)] = kNexthopId1;
    actions.push_back(action);
    attributes.push_back(swss::FieldValueTuple{p4orch::kActions, actions.dump()});

    auto *wcmp_group_entry_ptr = GetWcmpGroupEntry(kWcmpGroupId1);
    EXPECT_NE(nullptr, wcmp_group_entry_ptr);

    // Verification should fail if WCMP group ID mismatches.
    auto saved_wcmp_group_id = wcmp_group_entry_ptr->wcmp_group_id;
    wcmp_group_entry_ptr->wcmp_group_id = kWcmpGroupId2;
    EXPECT_FALSE(VerifyState(db_key, attributes).empty());
    wcmp_group_entry_ptr->wcmp_group_id = saved_wcmp_group_id;

    // Verification should fail if WCMP group ID mismatches.
    auto saved_wcmp_group_oid = wcmp_group_entry_ptr->wcmp_group_oid;
    wcmp_group_entry_ptr->wcmp_group_oid = 1111;
    EXPECT_FALSE(VerifyState(db_key, attributes).empty());
    wcmp_group_entry_ptr->wcmp_group_oid = saved_wcmp_group_oid;

    // Verification should fail if group size mismatches.
    wcmp_group_entry_ptr->wcmp_group_members.push_back(std::make_shared<P4WcmpGroupMemberEntry>());
    EXPECT_FALSE(VerifyState(db_key, attributes).empty());
    wcmp_group_entry_ptr->wcmp_group_members.pop_back();

    // Verification should fail if member nexthop ID mismatches.
    auto saved_next_hop_id = wcmp_group_entry_ptr->wcmp_group_members[0]->next_hop_id;
    wcmp_group_entry_ptr->wcmp_group_members[0]->next_hop_id = kNexthopId3;
    EXPECT_FALSE(VerifyState(db_key, attributes).empty());
    wcmp_group_entry_ptr->wcmp_group_members[0]->next_hop_id = saved_next_hop_id;

    // Verification should fail if member weight mismatches.
    auto saved_weight = wcmp_group_entry_ptr->wcmp_group_members[0]->weight;
    wcmp_group_entry_ptr->wcmp_group_members[0]->weight = 3;
    EXPECT_FALSE(VerifyState(db_key, attributes).empty());
    wcmp_group_entry_ptr->wcmp_group_members[0]->weight = saved_weight;

    // Verification should fail if member watch port mismatches.
    auto saved_watch_port = wcmp_group_entry_ptr->wcmp_group_members[0]->watch_port;
    wcmp_group_entry_ptr->wcmp_group_members[0]->watch_port = "invalid";
    EXPECT_FALSE(VerifyState(db_key, attributes).empty());
    wcmp_group_entry_ptr->wcmp_group_members[0]->watch_port = saved_watch_port;

    // Verification should fail if member WCMP group ID mismatches.
    auto saved_member_wcmp_group_id = wcmp_group_entry_ptr->wcmp_group_members[0]->wcmp_group_id;
    wcmp_group_entry_ptr->wcmp_group_members[0]->wcmp_group_id = kWcmpGroupId2;
    EXPECT_FALSE(VerifyState(db_key, attributes).empty());
    wcmp_group_entry_ptr->wcmp_group_members[0]->wcmp_group_id = saved_member_wcmp_group_id;

    // Verification should fail if nexthop OID mismatches.
    p4_oid_mapper_->decreaseRefCount(SAI_OBJECT_TYPE_NEXT_HOP, kNexthopKey1);
    p4_oid_mapper_->eraseOID(SAI_OBJECT_TYPE_NEXT_HOP, kNexthopKey1);

    EXPECT_FALSE(VerifyState(db_key, attributes).empty());
    p4_oid_mapper_->setOID(SAI_OBJECT_TYPE_NEXT_HOP, kNexthopKey1,
                           kNexthopOid1);
    p4_oid_mapper_->increaseRefCount(SAI_OBJECT_TYPE_NEXT_HOP, kNexthopKey1);
}

TEST_F(WcmpManagerTest, VerifyStateAsicDbTest)
{
    AddWcmpGroupEntryWithWatchport("Ethernet6", true);
    nlohmann::json j;
    j[prependMatchField(p4orch::kWcmpGroupId)] = kWcmpGroupId1;
    const std::string db_key = std::string(APP_P4RT_TABLE_NAME) + kTableKeyDelimiter + APP_P4RT_WCMP_GROUP_TABLE_NAME +
                               kTableKeyDelimiter + j.dump();
    std::vector<swss::FieldValueTuple> attributes;
    nlohmann::json actions;
    nlohmann::json action;
    action[p4orch::kAction] = p4orch::kSetNexthopId;
    action[p4orch::kWeight] = 2;
    action[p4orch::kWatchPort] = "Ethernet6";
    action[prependParamField(p4orch::kNexthopId)] = kNexthopId1;
    actions.push_back(action);
    attributes.push_back(swss::FieldValueTuple{p4orch::kActions, actions.dump()});

    // Setup ASIC DB.
    swss::Table table(nullptr, "ASIC_STATE");
    table.set(
        "SAI_OBJECT_TYPE_NEXT_HOP_GROUP:oid:0xa",
        std::vector<swss::FieldValueTuple>{
            swss::FieldValueTuple{"SAI_NEXT_HOP_GROUP_ATTR_TYPE",
                                  "SAI_NEXT_HOP_GROUP_TYPE_ECMP_WITH_MEMBERS"},
            swss::FieldValueTuple{"SAI_NEXT_HOP_GROUP_ATTR_NEXT_HOP_LIST",
                                  "1:oid:0x1"},
            swss::FieldValueTuple{
                "SAI_NEXT_HOP_GROUP_ATTR_NEXT_HOP_MEMBER_WEIGHT_LIST", "1:2"}});

    // Verification should succeed with correct ASIC DB values.
    EXPECT_EQ(VerifyState(db_key, attributes), "");

    // Verification should fail if group values mismatch.
    table.set("SAI_OBJECT_TYPE_NEXT_HOP_GROUP:oid:0xa",
              std::vector<swss::FieldValueTuple>{swss::FieldValueTuple{"SAI_NEXT_HOP_GROUP_ATTR_TYPE", "invalid"}});
    EXPECT_FALSE(VerifyState(db_key, attributes).empty());
    table.set("SAI_OBJECT_TYPE_NEXT_HOP_GROUP:oid:0xa",
              std::vector<swss::FieldValueTuple>{swss::FieldValueTuple{
                  "SAI_NEXT_HOP_GROUP_ATTR_TYPE",
                  "SAI_NEXT_HOP_GROUP_TYPE_ECMP_WITH_MEMBERS"}});

    // Verification should fail if member OID list mismatch.
    table.set("SAI_OBJECT_TYPE_NEXT_HOP_GROUP:oid:0xa",
              std::vector<swss::FieldValueTuple>{swss::FieldValueTuple{
                  "SAI_NEXT_HOP_GROUP_ATTR_NEXT_HOP_LIST", "1:oid:0x2"}});
    EXPECT_FALSE(VerifyState(db_key, attributes).empty());
    table.set("SAI_OBJECT_TYPE_NEXT_HOP_GROUP:oid:0xa",
              std::vector<swss::FieldValueTuple>{swss::FieldValueTuple{
                  "SAI_NEXT_HOP_GROUP_ATTR_NEXT_HOP_LIST", "1:oid:0x1"}});

    // Verification should fail if member weight list mismatch.
    table.set(
        "SAI_OBJECT_TYPE_NEXT_HOP_GROUP:oid:0xa",
        std::vector<swss::FieldValueTuple>{swss::FieldValueTuple{
            "SAI_NEXT_HOP_GROUP_ATTR_NEXT_HOP_MEMBER_WEIGHT_LIST", "1:1"}});
    EXPECT_FALSE(VerifyState(db_key, attributes).empty());
    table.set(
        "SAI_OBJECT_TYPE_NEXT_HOP_GROUP:oid:0xa",
        std::vector<swss::FieldValueTuple>{swss::FieldValueTuple{
            "SAI_NEXT_HOP_GROUP_ATTR_NEXT_HOP_MEMBER_WEIGHT_LIST", "1:2"}});

    // Verification should fail if group table is missing.
    table.del("SAI_OBJECT_TYPE_NEXT_HOP_GROUP:oid:0xa");
    EXPECT_FALSE(VerifyState(db_key, attributes).empty());
    table.set(
        "SAI_OBJECT_TYPE_NEXT_HOP_GROUP:oid:0xa",
        std::vector<swss::FieldValueTuple>{
            swss::FieldValueTuple{"SAI_NEXT_HOP_GROUP_ATTR_TYPE",
                                  "SAI_NEXT_HOP_GROUP_TYPE_ECMP_WITH_MEMBERS"},
            swss::FieldValueTuple{"SAI_NEXT_HOP_GROUP_ATTR_NEXT_HOP_LIST",
                                  "1:oid:0x1"},
            swss::FieldValueTuple{
                "SAI_NEXT_HOP_GROUP_ATTR_NEXT_HOP_MEMBER_WEIGHT_LIST", "1:2"}});
}

} // namespace test
} // namespace p4orch
