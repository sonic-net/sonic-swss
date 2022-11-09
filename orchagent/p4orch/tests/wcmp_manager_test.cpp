#include "wcmp_manager.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <string>

#include "json.hpp"
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
constexpr sai_object_id_t kWcmpGroupOid1 = 10;
constexpr char *kNexthopId1 = "ju1u32m1.atl11:qe-3/7";
constexpr sai_object_id_t kNexthopOid1 = 1;
constexpr sai_object_id_t kWcmpGroupMemberOid1 = 11;
constexpr char *kNexthopId2 = "ju1u32m2.atl11:qe-3/7";
constexpr sai_object_id_t kNexthopOid2 = 2;
constexpr sai_object_id_t kWcmpGroupMemberOid2 = 12;
constexpr char *kNexthopId3 = "ju1u32m3.atl11:qe-3/7";
constexpr sai_object_id_t kNexthopOid3 = 3;
constexpr sai_object_id_t kWcmpGroupMemberOid3 = 13;
constexpr sai_object_id_t kWcmpGroupMemberOid4 = 14;
constexpr sai_object_id_t kWcmpGroupMemberOid5 = 15;
const std::string kWcmpGroupKey1 = KeyGenerator::generateWcmpGroupKey(kWcmpGroupId1);
const std::string kNexthopKey1 = KeyGenerator::generateNextHopKey(kNexthopId1);
const std::string kNexthopKey2 = KeyGenerator::generateNextHopKey(kNexthopId2);
const std::string kNexthopKey3 = KeyGenerator::generateNextHopKey(kNexthopId3);

// Matches two SAI attributes.
bool MatchSaiAttribute(const sai_attribute_t &attr, const sai_attribute_t &exp_attr)
{
    if (exp_attr.id == SAI_NEXT_HOP_GROUP_MEMBER_ATTR_NEXT_HOP_GROUP_ID)
    {
        if (attr.id != SAI_NEXT_HOP_GROUP_MEMBER_ATTR_NEXT_HOP_GROUP_ID || attr.value.oid != exp_attr.value.oid)
        {
            return false;
        }
    }
    if (exp_attr.id == SAI_NEXT_HOP_GROUP_MEMBER_ATTR_NEXT_HOP_ID)
    {
        if (attr.id != SAI_NEXT_HOP_GROUP_MEMBER_ATTR_NEXT_HOP_ID || attr.value.oid != exp_attr.value.oid)
        {
            return false;
        }
    }
    if (exp_attr.id == SAI_NEXT_HOP_GROUP_MEMBER_ATTR_WEIGHT)
    {
        if (attr.id != SAI_NEXT_HOP_GROUP_MEMBER_ATTR_WEIGHT || attr.value.u32 != exp_attr.value.u32)
        {
            return false;
        }
    }
    return true;
}

MATCHER_P(ArrayEq, array, "")
{
    for (size_t i = 0; i < array.size(); ++i)
    {
        if (arg[i] != array[i])
        {
            return false;
        }
    }
    return true;
}

MATCHER_P(AttrArrayArrayEq, array, "")
{
    for (size_t i = 0; i < array.size(); ++i)
    {
        for (size_t j = 0; j < array[i].size(); j++)
        {
            if (!MatchSaiAttribute(arg[i][j], array[i][j]))
            {
                return false;
            }
        }
    }
    return true;
}

// Matches the next hop group type sai_attribute_t argument.
bool MatchSaiNextHopGroupAttribute(const sai_attribute_t *attr)
{
    if (attr == nullptr || attr->id != SAI_NEXT_HOP_GROUP_ATTR_TYPE || attr->value.s32 != SAI_NEXT_HOP_GROUP_TYPE_ECMP)
    {
        return false;
    }
    return true;
}

// Matches the action type sai_attribute_t argument.
bool MatchSaiNextHopGroupMemberAttribute(const sai_object_id_t expected_next_hop_oid, const int expected_weight,
                                         const sai_object_id_t expected_wcmp_group_oid,
                                         const sai_attribute_t *attr_list)
{
    if (attr_list == nullptr)
    {
        return false;
    }
    for (int i = 0; i < 3; ++i)
    {
        switch (attr_list[i].id)
        {
        case SAI_NEXT_HOP_GROUP_MEMBER_ATTR_NEXT_HOP_GROUP_ID:
            if (attr_list[i].value.oid != expected_wcmp_group_oid)
            {
                return false;
            }
            break;
        case SAI_NEXT_HOP_GROUP_MEMBER_ATTR_NEXT_HOP_ID:
            if (attr_list[i].value.oid != expected_next_hop_oid)
            {
                return false;
            }
            break;
        case SAI_NEXT_HOP_GROUP_MEMBER_ATTR_WEIGHT:
            if (attr_list[i].value.u32 != (uint32_t)expected_weight)
            {
                return false;
            }
            break;
        default:
            break;
        }
    }
    return true;
}

std::vector<sai_attribute_t> GetSaiNextHopGroupMemberAttribute(sai_object_id_t next_hop_oid, uint32_t weight,
                                                               sai_object_id_t group_oid)
{
    std::vector<sai_attribute_t> attrs;
    sai_attribute_t attr;
    attr.id = SAI_NEXT_HOP_GROUP_MEMBER_ATTR_NEXT_HOP_GROUP_ID;
    attr.value.oid = group_oid;
    attrs.push_back(attr);

    attr.id = SAI_NEXT_HOP_GROUP_MEMBER_ATTR_NEXT_HOP_ID;
    attr.value.oid = next_hop_oid;
    attrs.push_back(attr);

    attr.id = SAI_NEXT_HOP_GROUP_MEMBER_ATTR_WEIGHT;
    attr.value.u32 = weight;
    attrs.push_back(attr);

    return attrs;
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
        sai_next_hop_group_api->create_next_hop_group_member = create_next_hop_group_member;
        sai_next_hop_group_api->remove_next_hop_group_member = remove_next_hop_group_member;
        sai_next_hop_group_api->set_next_hop_group_member_attribute = set_next_hop_group_member_attribute;
        sai_next_hop_group_api->create_next_hop_group_members = create_next_hop_group_members;
        sai_next_hop_group_api->remove_next_hop_group_members = remove_next_hop_group_members;

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

        // init P4 orch
        std::vector<std::string> p4_tables;
        gP4Orch = new P4Orch(gAppDb, p4_tables, gVrfOrch, copp_orch_);
    }

    void Enqueue(const swss::KeyOpFieldsValuesTuple &entry)
    {
        wcmp_group_manager_->enqueue(entry);
    }

    void Drain()
    {
        wcmp_group_manager_->drain();
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
        wcmp_group_manager_->pruneNextHops(port);
    }

    void RestorePrunedNextHops(const std::string &port)
    {
        wcmp_group_manager_->restorePrunedNextHops(port);
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
    app_db_entry.wcmp_group_members.push_back(gm1);
    p4_oid_mapper_->setOID(SAI_OBJECT_TYPE_NEXT_HOP, kNexthopKey1, kNexthopOid1);
    EXPECT_CALL(mock_sai_next_hop_group_,
                create_next_hop_group(_, Eq(gSwitchId), Eq(1),
                                      Truly(std::bind(MatchSaiNextHopGroupAttribute, std::placeholders::_1))))
        .WillOnce(DoAll(SetArgPointee<0>(kWcmpGroupOid1), Return(SAI_STATUS_SUCCESS)));
    // For members with non empty watchport field, member creation in SAI happens
    // for operationally up ports only..
    std::vector<sai_object_id_t> return_oids{kWcmpGroupMemberOid1};
    std::vector<sai_status_t> exp_status{SAI_STATUS_SUCCESS};
    if (oper_up)
    {
        EXPECT_CALL(
            mock_sai_next_hop_group_,
            create_next_hop_group_members(Eq(gSwitchId), Eq(1), ArrayEq(std::vector<uint32_t>{3}),
                                          AttrArrayArrayEq(std::vector<std::vector<sai_attribute_t>>{
                                              GetSaiNextHopGroupMemberAttribute(kNexthopOid1, 2, kWcmpGroupOid1)}),
                                          Eq(SAI_BULK_OP_ERROR_MODE_STOP_ON_ERROR), _, _))
            .WillOnce(DoAll(SetArrayArgument<5>(return_oids.begin(), return_oids.end()),
                            SetArrayArgument<6>(exp_status.begin(), exp_status.end()), Return(SAI_STATUS_SUCCESS)));
    }
    EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS, ProcessAddRequest(&app_db_entry));
    EXPECT_NE(nullptr, GetWcmpGroupEntry(kWcmpGroupId1));
    return app_db_entry;
}

P4WcmpGroupEntry WcmpManagerTest::AddWcmpGroupEntry1()
{
    P4WcmpGroupEntry app_db_entry = getDefaultWcmpGroupEntryForTest();
    p4_oid_mapper_->setOID(SAI_OBJECT_TYPE_NEXT_HOP, kNexthopKey1, kNexthopOid1);
    p4_oid_mapper_->setOID(SAI_OBJECT_TYPE_NEXT_HOP, kNexthopKey2, kNexthopOid2);
    p4_oid_mapper_->setOID(SAI_OBJECT_TYPE_NEXT_HOP, kNexthopKey3, kNexthopOid3);
    EXPECT_CALL(mock_sai_next_hop_group_,
                create_next_hop_group(_, Eq(gSwitchId), Eq(1),
                                      Truly(std::bind(MatchSaiNextHopGroupAttribute, std::placeholders::_1))))
        .WillOnce(DoAll(SetArgPointee<0>(kWcmpGroupOid1), Return(SAI_STATUS_SUCCESS)));
    std::vector<sai_object_id_t> return_oids{kWcmpGroupMemberOid1, kWcmpGroupMemberOid2};
    std::vector<sai_status_t> exp_status{SAI_STATUS_SUCCESS, SAI_STATUS_SUCCESS};
    EXPECT_CALL(mock_sai_next_hop_group_,
                create_next_hop_group_members(Eq(gSwitchId), Eq(2), ArrayEq(std::vector<uint32_t>{3, 3}),
                                              AttrArrayArrayEq(std::vector<std::vector<sai_attribute_t>>{
                                                  GetSaiNextHopGroupMemberAttribute(kNexthopOid1, 2, kWcmpGroupOid1),
                                                  GetSaiNextHopGroupMemberAttribute(kNexthopOid2, 1, kWcmpGroupOid1)}),
                                              Eq(SAI_BULK_OP_ERROR_MODE_STOP_ON_ERROR), _, _))
        .WillOnce(DoAll(SetArrayArgument<5>(return_oids.begin(), return_oids.end()),
                        SetArrayArgument<6>(exp_status.begin(), exp_status.end()), Return(SAI_STATUS_SUCCESS)));
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
    P4WcmpGroupEntry expect_entry = {.wcmp_group_id = kWcmpGroupId1, .wcmp_group_members = {}};
    std::shared_ptr<P4WcmpGroupMemberEntry> gm_entry1 = createWcmpGroupMemberEntry(kNexthopId1, 2);
    expect_entry.wcmp_group_members.push_back(gm_entry1);
    std::shared_ptr<P4WcmpGroupMemberEntry> gm_entry2 = createWcmpGroupMemberEntry(kNexthopId2, 1);
    expect_entry.wcmp_group_members.push_back(gm_entry2);
    VerifyWcmpGroupEntry(expect_entry, *GetWcmpGroupEntry(kWcmpGroupId1));
}

TEST_F(WcmpManagerTest, CreateWcmpGroupFailsWhenCreateGroupMemberSaiCallFails)
{
    P4WcmpGroupEntry app_db_entry = getDefaultWcmpGroupEntryForTest();
    p4_oid_mapper_->setOID(SAI_OBJECT_TYPE_NEXT_HOP, kNexthopKey1, kNexthopOid1);
    p4_oid_mapper_->setOID(SAI_OBJECT_TYPE_NEXT_HOP, kNexthopKey2, kNexthopOid2);
    // WCMP group creation fails when one of the group member creation fails
    EXPECT_CALL(mock_sai_next_hop_group_, create_next_hop_group(_, _, _, _))
        .WillOnce(DoAll(SetArgPointee<0>(kWcmpGroupOid1), Return(SAI_STATUS_SUCCESS)));
    std::vector<sai_object_id_t> return_oids{kWcmpGroupMemberOid1, SAI_NULL_OBJECT_ID};
    std::vector<sai_status_t> exp_create_status{SAI_STATUS_SUCCESS, SAI_STATUS_ITEM_NOT_FOUND};
    EXPECT_CALL(mock_sai_next_hop_group_,
                create_next_hop_group_members(Eq(gSwitchId), Eq(2), ArrayEq(std::vector<uint32_t>{3, 3}),
                                              AttrArrayArrayEq(std::vector<std::vector<sai_attribute_t>>{
                                                  GetSaiNextHopGroupMemberAttribute(kNexthopOid1, 2, kWcmpGroupOid1),
                                                  GetSaiNextHopGroupMemberAttribute(kNexthopOid2, 1, kWcmpGroupOid1)}),
                                              Eq(SAI_BULK_OP_ERROR_MODE_STOP_ON_ERROR), _, _))
        .WillOnce(DoAll(SetArrayArgument<5>(return_oids.begin(), return_oids.end()),
                        SetArrayArgument<6>(exp_create_status.begin(), exp_create_status.end()),
                        Return(SAI_STATUS_ITEM_NOT_FOUND)));
    std::vector<sai_status_t> exp_remove_status{SAI_STATUS_SUCCESS};
    EXPECT_CALL(mock_sai_next_hop_group_,
                remove_next_hop_group_members(Eq(1), ArrayEq(std::vector<sai_object_id_t>{kWcmpGroupMemberOid1}),
                                              Eq(SAI_BULK_OP_ERROR_MODE_STOP_ON_ERROR), _))
        .WillOnce(
            DoAll(SetArrayArgument<3>(exp_remove_status.begin(), exp_remove_status.end()), Return(SAI_STATUS_SUCCESS)));
    EXPECT_CALL(mock_sai_next_hop_group_, remove_next_hop_group(Eq(kWcmpGroupOid1)))
        .WillOnce(Return(SAI_STATUS_SUCCESS));
    EXPECT_EQ(StatusCode::SWSS_RC_UNKNOWN, ProcessAddRequest(&app_db_entry));
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

TEST_F(WcmpManagerTest, CreateWcmpGroupFailsWhenCreateGroupMemberSaiCallFailsPlusGroupMemberRecoveryFails)
{
    P4WcmpGroupEntry app_db_entry = getDefaultWcmpGroupEntryForTest();
    p4_oid_mapper_->setOID(SAI_OBJECT_TYPE_NEXT_HOP, kNexthopKey1, kNexthopOid1);
    p4_oid_mapper_->setOID(SAI_OBJECT_TYPE_NEXT_HOP, kNexthopKey2, kNexthopOid2);
    // WCMP group creation fails when one of the group member creation fails
    EXPECT_CALL(mock_sai_next_hop_group_, create_next_hop_group(_, _, _, _))
        .WillOnce(DoAll(SetArgPointee<0>(kWcmpGroupOid1), Return(SAI_STATUS_SUCCESS)));
    std::vector<sai_object_id_t> return_oids{kWcmpGroupMemberOid1, SAI_NULL_OBJECT_ID};
    std::vector<sai_status_t> exp_create_status{SAI_STATUS_SUCCESS, SAI_STATUS_ITEM_NOT_FOUND};
    EXPECT_CALL(mock_sai_next_hop_group_,
                create_next_hop_group_members(Eq(gSwitchId), Eq(2), ArrayEq(std::vector<uint32_t>{3, 3}),
                                              AttrArrayArrayEq(std::vector<std::vector<sai_attribute_t>>{
                                                  GetSaiNextHopGroupMemberAttribute(kNexthopOid1, 2, kWcmpGroupOid1),
                                                  GetSaiNextHopGroupMemberAttribute(kNexthopOid2, 1, kWcmpGroupOid1)}),
                                              Eq(SAI_BULK_OP_ERROR_MODE_STOP_ON_ERROR), _, _))
        .WillOnce(DoAll(SetArrayArgument<5>(return_oids.begin(), return_oids.end()),
                        SetArrayArgument<6>(exp_create_status.begin(), exp_create_status.end()),
                        Return(SAI_STATUS_ITEM_NOT_FOUND)));
    std::vector<sai_status_t> exp_remove_status{SAI_STATUS_FAILURE};
    EXPECT_CALL(mock_sai_next_hop_group_,
                remove_next_hop_group_members(Eq(1), ArrayEq(std::vector<sai_object_id_t>{kWcmpGroupMemberOid1}),
                                              Eq(SAI_BULK_OP_ERROR_MODE_STOP_ON_ERROR), _))
        .WillOnce(
            DoAll(SetArrayArgument<3>(exp_remove_status.begin(), exp_remove_status.end()), Return(SAI_STATUS_FAILURE)));
    EXPECT_CALL(mock_sai_next_hop_group_, remove_next_hop_group(Eq(kWcmpGroupOid1)))
        .WillOnce(Return(SAI_STATUS_SUCCESS));

    // TODO: Expect critical state.
    EXPECT_EQ(StatusCode::SWSS_RC_UNKNOWN, ProcessAddRequest(&app_db_entry));
}

TEST_F(WcmpManagerTest, CreateWcmpGroupFailsWhenCreateGroupMemberSaiCallFailsPlusGroupRecoveryFails)
{
    P4WcmpGroupEntry app_db_entry = getDefaultWcmpGroupEntryForTest();
    p4_oid_mapper_->setOID(SAI_OBJECT_TYPE_NEXT_HOP, kNexthopKey1, kNexthopOid1);
    p4_oid_mapper_->setOID(SAI_OBJECT_TYPE_NEXT_HOP, kNexthopKey2, kNexthopOid2);
    // WCMP group creation fails when one of the group member creation fails
    EXPECT_CALL(mock_sai_next_hop_group_, create_next_hop_group(_, _, _, _))
        .WillOnce(DoAll(SetArgPointee<0>(kWcmpGroupOid1), Return(SAI_STATUS_SUCCESS)));
    std::vector<sai_object_id_t> return_oids{kWcmpGroupMemberOid1, SAI_NULL_OBJECT_ID};
    std::vector<sai_status_t> exp_create_status{SAI_STATUS_SUCCESS, SAI_STATUS_ITEM_NOT_FOUND};
    EXPECT_CALL(mock_sai_next_hop_group_,
                create_next_hop_group_members(Eq(gSwitchId), Eq(2), ArrayEq(std::vector<uint32_t>{3, 3}),
                                              AttrArrayArrayEq(std::vector<std::vector<sai_attribute_t>>{
                                                  GetSaiNextHopGroupMemberAttribute(kNexthopOid1, 2, kWcmpGroupOid1),
                                                  GetSaiNextHopGroupMemberAttribute(kNexthopOid2, 1, kWcmpGroupOid1)}),
                                              Eq(SAI_BULK_OP_ERROR_MODE_STOP_ON_ERROR), _, _))
        .WillOnce(DoAll(SetArrayArgument<5>(return_oids.begin(), return_oids.end()),
                        SetArrayArgument<6>(exp_create_status.begin(), exp_create_status.end()),
                        Return(SAI_STATUS_ITEM_NOT_FOUND)));
    std::vector<sai_status_t> exp_remove_status{SAI_STATUS_SUCCESS};
    EXPECT_CALL(mock_sai_next_hop_group_,
                remove_next_hop_group_members(Eq(1), ArrayEq(std::vector<sai_object_id_t>{kWcmpGroupMemberOid1}),
                                              Eq(SAI_BULK_OP_ERROR_MODE_STOP_ON_ERROR), _))
        .WillOnce(
            DoAll(SetArrayArgument<3>(exp_remove_status.begin(), exp_remove_status.end()), Return(SAI_STATUS_SUCCESS)));
    EXPECT_CALL(mock_sai_next_hop_group_, remove_next_hop_group(Eq(kWcmpGroupOid1)))
        .WillOnce(Return(SAI_STATUS_FAILURE));

    // TODO: Expect critical state.
    EXPECT_EQ(StatusCode::SWSS_RC_UNKNOWN, ProcessAddRequest(&app_db_entry));
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
    std::vector<sai_status_t> exp_remove_status{SAI_STATUS_SUCCESS, SAI_STATUS_SUCCESS};
    EXPECT_CALL(mock_sai_next_hop_group_,
                remove_next_hop_group_members(
                    Eq(2), ArrayEq(std::vector<sai_object_id_t>{kWcmpGroupMemberOid2, kWcmpGroupMemberOid1}),
                    Eq(SAI_BULK_OP_ERROR_MODE_STOP_ON_ERROR), _))
        .WillOnce(
            DoAll(SetArrayArgument<3>(exp_remove_status.begin(), exp_remove_status.end()), Return(SAI_STATUS_SUCCESS)));
    EXPECT_CALL(mock_sai_next_hop_group_, remove_next_hop_group(Eq(kWcmpGroupOid1)))
        .WillOnce(Return(SAI_STATUS_FAILURE));
    std::vector<sai_object_id_t> return_oids{kWcmpGroupMemberOid1, kWcmpGroupMemberOid2};
    std::vector<sai_status_t> exp_create_status{SAI_STATUS_SUCCESS, SAI_STATUS_SUCCESS};
    EXPECT_CALL(mock_sai_next_hop_group_,
                create_next_hop_group_members(Eq(gSwitchId), Eq(2), ArrayEq(std::vector<uint32_t>{3, 3}),
                                              AttrArrayArrayEq(std::vector<std::vector<sai_attribute_t>>{
                                                  GetSaiNextHopGroupMemberAttribute(kNexthopOid1, 2, kWcmpGroupOid1),
                                                  GetSaiNextHopGroupMemberAttribute(kNexthopOid2, 1, kWcmpGroupOid1)}),
                                              Eq(SAI_BULK_OP_ERROR_MODE_STOP_ON_ERROR), _, _))
        .WillOnce(DoAll(SetArrayArgument<5>(return_oids.begin(), return_oids.end()),
                        SetArrayArgument<6>(exp_create_status.begin(), exp_create_status.end()),
                        Return(SAI_STATUS_SUCCESS)));
    EXPECT_EQ(StatusCode::SWSS_RC_UNKNOWN, RemoveWcmpGroup(kWcmpGroupId1));
}

TEST_F(WcmpManagerTest, RemoveWcmpGroupFailsWhenMemberRemovalFails)
{
    P4WcmpGroupEntry app_db_entry = AddWcmpGroupEntry1();
    std::vector<sai_status_t> exp_remove_status{SAI_STATUS_FAILURE, SAI_STATUS_SUCCESS};
    EXPECT_CALL(mock_sai_next_hop_group_,
                remove_next_hop_group_members(
                    Eq(2), ArrayEq(std::vector<sai_object_id_t>{kWcmpGroupMemberOid2, kWcmpGroupMemberOid1}),
                    Eq(SAI_BULK_OP_ERROR_MODE_STOP_ON_ERROR), _))
        .WillOnce(
            DoAll(SetArrayArgument<3>(exp_remove_status.begin(), exp_remove_status.end()), Return(SAI_STATUS_FAILURE)));
    std::vector<sai_object_id_t> return_oids{kWcmpGroupMemberOid1};
    std::vector<sai_status_t> exp_create_status{SAI_STATUS_SUCCESS};
    EXPECT_CALL(mock_sai_next_hop_group_,
                create_next_hop_group_members(Eq(gSwitchId), Eq(1), ArrayEq(std::vector<uint32_t>{3}),
                                              AttrArrayArrayEq(std::vector<std::vector<sai_attribute_t>>{
                                                  GetSaiNextHopGroupMemberAttribute(kNexthopOid1, 2, kWcmpGroupOid1)}),
                                              Eq(SAI_BULK_OP_ERROR_MODE_STOP_ON_ERROR), _, _))
        .WillOnce(DoAll(SetArrayArgument<5>(return_oids.begin(), return_oids.end()),
                        SetArrayArgument<6>(exp_create_status.begin(), exp_create_status.end()),
                        Return(SAI_STATUS_SUCCESS)));
    EXPECT_EQ(StatusCode::SWSS_RC_UNKNOWN, RemoveWcmpGroup(kWcmpGroupId1));
}

TEST_F(WcmpManagerTest, RemoveWcmpGroupFailsWhenMemberRemovalFailsPlusRecoveryFails)
{
    P4WcmpGroupEntry app_db_entry = AddWcmpGroupEntry1();
    std::vector<sai_status_t> exp_remove_status{SAI_STATUS_FAILURE, SAI_STATUS_SUCCESS};
    EXPECT_CALL(mock_sai_next_hop_group_,
                remove_next_hop_group_members(
                    Eq(2), ArrayEq(std::vector<sai_object_id_t>{kWcmpGroupMemberOid2, kWcmpGroupMemberOid1}),
                    Eq(SAI_BULK_OP_ERROR_MODE_STOP_ON_ERROR), _))
        .WillOnce(
            DoAll(SetArrayArgument<3>(exp_remove_status.begin(), exp_remove_status.end()), Return(SAI_STATUS_FAILURE)));
    std::vector<sai_object_id_t> return_oids{SAI_NULL_OBJECT_ID};
    std::vector<sai_status_t> exp_create_status{SAI_STATUS_FAILURE};
    EXPECT_CALL(mock_sai_next_hop_group_,
                create_next_hop_group_members(Eq(gSwitchId), Eq(1), ArrayEq(std::vector<uint32_t>{3}),
                                              AttrArrayArrayEq(std::vector<std::vector<sai_attribute_t>>{
                                                  GetSaiNextHopGroupMemberAttribute(kNexthopOid1, 2, kWcmpGroupOid1)}),
                                              Eq(SAI_BULK_OP_ERROR_MODE_STOP_ON_ERROR), _, _))
        .WillOnce(DoAll(SetArrayArgument<5>(return_oids.begin(), return_oids.end()),
                        SetArrayArgument<6>(exp_create_status.begin(), exp_create_status.end()),
                        Return(SAI_STATUS_FAILURE)));
    // TODO: Expect critical state.
    EXPECT_EQ(StatusCode::SWSS_RC_UNKNOWN, RemoveWcmpGroup(kWcmpGroupId1));
}

TEST_F(WcmpManagerTest, UpdateWcmpGroupMembersSucceed)
{
    AddWcmpGroupEntry1();
    // Update WCMP group member with nexthop_id=kNexthopId1 weight to 3,
    // nexthop_id=kNexthopId2 weight to 15.
    P4WcmpGroupEntry wcmp_group = {.wcmp_group_id = kWcmpGroupId1, .wcmp_group_members = {}};
    std::shared_ptr<P4WcmpGroupMemberEntry> gm1 = createWcmpGroupMemberEntry(kNexthopId1, 3);
    std::shared_ptr<P4WcmpGroupMemberEntry> gm2 = createWcmpGroupMemberEntry(kNexthopId2, 15);
    wcmp_group.wcmp_group_members.push_back(gm1);
    wcmp_group.wcmp_group_members.push_back(gm2);
    std::vector<sai_status_t> exp_remove_status{SAI_STATUS_SUCCESS};
    EXPECT_CALL(mock_sai_next_hop_group_,
                remove_next_hop_group_members(Eq(1), ArrayEq(std::vector<sai_object_id_t>{kWcmpGroupMemberOid1}),
                                              Eq(SAI_BULK_OP_ERROR_MODE_STOP_ON_ERROR), _))
        .WillOnce(
            DoAll(SetArrayArgument<3>(exp_remove_status.begin(), exp_remove_status.end()), Return(SAI_STATUS_SUCCESS)));
    EXPECT_CALL(mock_sai_next_hop_group_,
                remove_next_hop_group_members(Eq(1), ArrayEq(std::vector<sai_object_id_t>{kWcmpGroupMemberOid2}),
                                              Eq(SAI_BULK_OP_ERROR_MODE_STOP_ON_ERROR), _))
        .WillOnce(
            DoAll(SetArrayArgument<3>(exp_remove_status.begin(), exp_remove_status.end()), Return(SAI_STATUS_SUCCESS)));
    std::vector<sai_object_id_t> return_oids_4{kWcmpGroupMemberOid4};
    std::vector<sai_object_id_t> return_oids_5{kWcmpGroupMemberOid5};
    std::vector<sai_status_t> exp_create_status{SAI_STATUS_SUCCESS};
    EXPECT_CALL(mock_sai_next_hop_group_,
                create_next_hop_group_members(Eq(gSwitchId), Eq(1), ArrayEq(std::vector<uint32_t>{3}),
                                              AttrArrayArrayEq(std::vector<std::vector<sai_attribute_t>>{
                                                  GetSaiNextHopGroupMemberAttribute(kNexthopOid1, 3, kWcmpGroupOid1)}),
                                              Eq(SAI_BULK_OP_ERROR_MODE_STOP_ON_ERROR), _, _))
        .WillOnce(DoAll(SetArrayArgument<5>(return_oids_4.begin(), return_oids_4.end()),
                        SetArrayArgument<6>(exp_create_status.begin(), exp_create_status.end()),
                        Return(SAI_STATUS_SUCCESS)));
    EXPECT_CALL(mock_sai_next_hop_group_,
                create_next_hop_group_members(Eq(gSwitchId), Eq(1), ArrayEq(std::vector<uint32_t>{3}),
                                              AttrArrayArrayEq(std::vector<std::vector<sai_attribute_t>>{
                                                  GetSaiNextHopGroupMemberAttribute(kNexthopOid2, 15, kWcmpGroupOid1)}),
                                              Eq(SAI_BULK_OP_ERROR_MODE_STOP_ON_ERROR), _, _))
        .WillOnce(DoAll(SetArrayArgument<5>(return_oids_5.begin(), return_oids_5.end()),
                        SetArrayArgument<6>(exp_create_status.begin(), exp_create_status.end()),
                        Return(SAI_STATUS_SUCCESS)));
    EXPECT_TRUE(ProcessUpdateRequest(&wcmp_group).ok());
    VerifyWcmpGroupEntry(wcmp_group, *GetWcmpGroupEntry(kWcmpGroupId1));
    uint32_t wcmp_group_refcount = 0;
    uint32_t nexthop_refcount = 0;
    ASSERT_TRUE(p4_oid_mapper_->getRefCount(SAI_OBJECT_TYPE_NEXT_HOP_GROUP, kWcmpGroupKey1, &wcmp_group_refcount));
    EXPECT_EQ(2, wcmp_group_refcount);
    ASSERT_TRUE(p4_oid_mapper_->getRefCount(SAI_OBJECT_TYPE_NEXT_HOP, kNexthopKey1, &nexthop_refcount));
    EXPECT_EQ(1, nexthop_refcount);
    ASSERT_TRUE(p4_oid_mapper_->getRefCount(SAI_OBJECT_TYPE_NEXT_HOP, kNexthopKey2, &nexthop_refcount));
    EXPECT_EQ(1, nexthop_refcount);
    // Remove group member with nexthop_id=kNexthopId1
    wcmp_group.wcmp_group_members.clear();
    gm2 = createWcmpGroupMemberEntry(kNexthopId2, 15);
    wcmp_group.wcmp_group_members.push_back(gm2);
    exp_remove_status = {SAI_STATUS_SUCCESS};
    EXPECT_CALL(mock_sai_next_hop_group_,
                remove_next_hop_group_members(Eq(1), ArrayEq(std::vector<sai_object_id_t>{kWcmpGroupMemberOid4}),
                                              Eq(SAI_BULK_OP_ERROR_MODE_STOP_ON_ERROR), _))
        .WillOnce(
            DoAll(SetArrayArgument<3>(exp_remove_status.begin(), exp_remove_status.end()), Return(SAI_STATUS_SUCCESS)));
    EXPECT_CALL(mock_sai_next_hop_group_,
                remove_next_hop_group_members(Eq(1), ArrayEq(std::vector<sai_object_id_t>{kWcmpGroupMemberOid5}),
                                              Eq(SAI_BULK_OP_ERROR_MODE_STOP_ON_ERROR), _))
        .WillOnce(
            DoAll(SetArrayArgument<3>(exp_remove_status.begin(), exp_remove_status.end()), Return(SAI_STATUS_SUCCESS)));
    std::vector<sai_object_id_t> return_oids_2{kWcmpGroupMemberOid2};
    exp_create_status = {SAI_STATUS_SUCCESS};
    EXPECT_CALL(mock_sai_next_hop_group_,
                create_next_hop_group_members(Eq(gSwitchId), Eq(1), ArrayEq(std::vector<uint32_t>{3}),
                                              AttrArrayArrayEq(std::vector<std::vector<sai_attribute_t>>{
                                                  GetSaiNextHopGroupMemberAttribute(kNexthopOid2, 15, kWcmpGroupOid1)}),
                                              Eq(SAI_BULK_OP_ERROR_MODE_STOP_ON_ERROR), _, _))
        .WillOnce(DoAll(SetArrayArgument<5>(return_oids_2.begin(), return_oids_2.end()),
                        SetArrayArgument<6>(exp_create_status.begin(), exp_create_status.end()),
                        Return(SAI_STATUS_SUCCESS)));
    EXPECT_TRUE(ProcessUpdateRequest(&wcmp_group).ok());
    VerifyWcmpGroupEntry(wcmp_group, *GetWcmpGroupEntry(kWcmpGroupId1));
    ASSERT_TRUE(p4_oid_mapper_->getRefCount(SAI_OBJECT_TYPE_NEXT_HOP_GROUP, kWcmpGroupKey1, &wcmp_group_refcount));
    EXPECT_EQ(1, wcmp_group_refcount);
    ASSERT_TRUE(p4_oid_mapper_->getRefCount(SAI_OBJECT_TYPE_NEXT_HOP, kNexthopKey1, &nexthop_refcount));
    EXPECT_EQ(0, nexthop_refcount);
    ASSERT_TRUE(p4_oid_mapper_->getRefCount(SAI_OBJECT_TYPE_NEXT_HOP, kNexthopKey2, &nexthop_refcount));
    EXPECT_EQ(1, nexthop_refcount);
    // Add group member with nexthop_id=kNexthopId1 and weight=20
    wcmp_group.wcmp_group_members.clear();
    std::shared_ptr<P4WcmpGroupMemberEntry> updated_gm2 = createWcmpGroupMemberEntry(kNexthopId2, 15);
    std::shared_ptr<P4WcmpGroupMemberEntry> updated_gm1 = createWcmpGroupMemberEntry(kNexthopId1, 20);
    wcmp_group.wcmp_group_members.push_back(updated_gm1);
    wcmp_group.wcmp_group_members.push_back(updated_gm2);
    exp_remove_status = {SAI_STATUS_SUCCESS};
    EXPECT_CALL(mock_sai_next_hop_group_,
                remove_next_hop_group_members(Eq(1), ArrayEq(std::vector<sai_object_id_t>{kWcmpGroupMemberOid2}),
                                              Eq(SAI_BULK_OP_ERROR_MODE_STOP_ON_ERROR), _))
        .WillOnce(
            DoAll(SetArrayArgument<3>(exp_remove_status.begin(), exp_remove_status.end()), Return(SAI_STATUS_SUCCESS)));
    std::vector<sai_object_id_t> return_oids_1{kWcmpGroupMemberOid1};
    exp_create_status = {SAI_STATUS_SUCCESS};
    EXPECT_CALL(mock_sai_next_hop_group_,
                create_next_hop_group_members(Eq(gSwitchId), Eq(1), ArrayEq(std::vector<uint32_t>{3}),
                                              AttrArrayArrayEq(std::vector<std::vector<sai_attribute_t>>{
                                                  GetSaiNextHopGroupMemberAttribute(kNexthopOid1, 20, kWcmpGroupOid1)}),
                                              Eq(SAI_BULK_OP_ERROR_MODE_STOP_ON_ERROR), _, _))
        .WillOnce(DoAll(SetArrayArgument<5>(return_oids_1.begin(), return_oids_1.end()),
                        SetArrayArgument<6>(exp_create_status.begin(), exp_create_status.end()),
                        Return(SAI_STATUS_SUCCESS)));
    EXPECT_CALL(mock_sai_next_hop_group_,
                create_next_hop_group_members(Eq(gSwitchId), Eq(1), ArrayEq(std::vector<uint32_t>{3}),
                                              AttrArrayArrayEq(std::vector<std::vector<sai_attribute_t>>{
                                                  GetSaiNextHopGroupMemberAttribute(kNexthopOid2, 15, kWcmpGroupOid1)}),
                                              Eq(SAI_BULK_OP_ERROR_MODE_STOP_ON_ERROR), _, _))
        .WillOnce(DoAll(SetArrayArgument<5>(return_oids_5.begin(), return_oids_5.end()),
                        SetArrayArgument<6>(exp_create_status.begin(), exp_create_status.end()),
                        Return(SAI_STATUS_SUCCESS)));
    EXPECT_TRUE(ProcessUpdateRequest(&wcmp_group).ok());
    VerifyWcmpGroupEntry(wcmp_group, *GetWcmpGroupEntry(kWcmpGroupId1));
    ASSERT_TRUE(p4_oid_mapper_->getRefCount(SAI_OBJECT_TYPE_NEXT_HOP_GROUP, kWcmpGroupKey1, &wcmp_group_refcount));
    EXPECT_EQ(2, wcmp_group_refcount);
    ASSERT_TRUE(p4_oid_mapper_->getRefCount(SAI_OBJECT_TYPE_NEXT_HOP, kNexthopKey1, &nexthop_refcount));
    EXPECT_EQ(1, nexthop_refcount);
    ASSERT_TRUE(p4_oid_mapper_->getRefCount(SAI_OBJECT_TYPE_NEXT_HOP, kNexthopKey2, &nexthop_refcount));
    EXPECT_EQ(1, nexthop_refcount);

    // Update WCMP without group members
    wcmp_group.wcmp_group_members.clear();
    exp_remove_status = {SAI_STATUS_SUCCESS};
    EXPECT_CALL(mock_sai_next_hop_group_,
                remove_next_hop_group_members(Eq(1), ArrayEq(std::vector<sai_object_id_t>{kWcmpGroupMemberOid1}),
                                              Eq(SAI_BULK_OP_ERROR_MODE_STOP_ON_ERROR), _))
        .WillOnce(
            DoAll(SetArrayArgument<3>(exp_remove_status.begin(), exp_remove_status.end()), Return(SAI_STATUS_SUCCESS)));
    EXPECT_CALL(mock_sai_next_hop_group_,
                remove_next_hop_group_members(Eq(1), ArrayEq(std::vector<sai_object_id_t>{kWcmpGroupMemberOid5}),
                                              Eq(SAI_BULK_OP_ERROR_MODE_STOP_ON_ERROR), _))
        .WillOnce(
            DoAll(SetArrayArgument<3>(exp_remove_status.begin(), exp_remove_status.end()), Return(SAI_STATUS_SUCCESS)));
    EXPECT_TRUE(ProcessUpdateRequest(&wcmp_group).ok());
    VerifyWcmpGroupEntry(wcmp_group, *GetWcmpGroupEntry(kWcmpGroupId1));
    ASSERT_TRUE(p4_oid_mapper_->getRefCount(SAI_OBJECT_TYPE_NEXT_HOP_GROUP, kWcmpGroupKey1, &wcmp_group_refcount));
    EXPECT_EQ(0, wcmp_group_refcount);
    ASSERT_TRUE(p4_oid_mapper_->getRefCount(SAI_OBJECT_TYPE_NEXT_HOP, kNexthopKey1, &nexthop_refcount));
    EXPECT_EQ(0, nexthop_refcount);
    ASSERT_TRUE(p4_oid_mapper_->getRefCount(SAI_OBJECT_TYPE_NEXT_HOP, kNexthopKey2, &nexthop_refcount));
    EXPECT_EQ(0, nexthop_refcount);
}

TEST_F(WcmpManagerTest, UpdateWcmpGroupFailsWhenRemoveGroupMemberSaiCallFails)
{
    AddWcmpGroupEntry1();
    // Add WCMP group member with nexthop_id=kNexthopId1, weight=3 and
    // nexthop_id=kNexthopId3, weight=30, update nexthop_id=kNexthopId2
    // weight to 10.
    P4WcmpGroupEntry wcmp_group = {.wcmp_group_id = kWcmpGroupId1, .wcmp_group_members = {}};
    std::shared_ptr<P4WcmpGroupMemberEntry> gm1 = createWcmpGroupMemberEntry(kNexthopId1, 3);
    std::shared_ptr<P4WcmpGroupMemberEntry> gm2 = createWcmpGroupMemberEntry(kNexthopId2, 10);
    std::shared_ptr<P4WcmpGroupMemberEntry> gm3 = createWcmpGroupMemberEntry(kNexthopId3, 30);

    wcmp_group.wcmp_group_members.push_back(gm1);
    wcmp_group.wcmp_group_members.push_back(gm2);
    wcmp_group.wcmp_group_members.push_back(gm3);
    std::vector<sai_object_id_t> return_oids_4{kWcmpGroupMemberOid4};
    std::vector<sai_object_id_t> return_oids_5_6{kWcmpGroupMemberOid5, kWcmpGroupMemberOid3};
    std::vector<sai_status_t> exp_create_status_1{SAI_STATUS_SUCCESS};
    std::vector<sai_status_t> exp_create_status_2{SAI_STATUS_SUCCESS, SAI_STATUS_SUCCESS};
    EXPECT_CALL(mock_sai_next_hop_group_,
                create_next_hop_group_members(Eq(gSwitchId), Eq(1), ArrayEq(std::vector<uint32_t>{3}),
                                              AttrArrayArrayEq(std::vector<std::vector<sai_attribute_t>>{
                                                  GetSaiNextHopGroupMemberAttribute(kNexthopOid1, 3, kWcmpGroupOid1)}),
                                              Eq(SAI_BULK_OP_ERROR_MODE_STOP_ON_ERROR), _, _))
        .WillOnce(DoAll(SetArrayArgument<5>(return_oids_4.begin(), return_oids_4.end()),
                        SetArrayArgument<6>(exp_create_status_1.begin(), exp_create_status_1.end()),
                        Return(SAI_STATUS_SUCCESS)));
    EXPECT_CALL(mock_sai_next_hop_group_,
                create_next_hop_group_members(Eq(gSwitchId), Eq(2), ArrayEq(std::vector<uint32_t>{3, 3}),
                                              AttrArrayArrayEq(std::vector<std::vector<sai_attribute_t>>{
                                                  GetSaiNextHopGroupMemberAttribute(kNexthopOid2, 10, kWcmpGroupOid1),
                                                  GetSaiNextHopGroupMemberAttribute(kNexthopOid3, 30, kWcmpGroupOid1)}),
                                              Eq(SAI_BULK_OP_ERROR_MODE_STOP_ON_ERROR), _, _))
        .WillOnce(DoAll(SetArrayArgument<5>(return_oids_5_6.begin(), return_oids_5_6.end()),
                        SetArrayArgument<6>(exp_create_status_2.begin(), exp_create_status_2.end()),
                        Return(SAI_STATUS_SUCCESS)));
    std::vector<sai_status_t> exp_remove_status{SAI_STATUS_SUCCESS};
    EXPECT_CALL(mock_sai_next_hop_group_,
                remove_next_hop_group_members(Eq(1), ArrayEq(std::vector<sai_object_id_t>{kWcmpGroupMemberOid1}),
                                              Eq(SAI_BULK_OP_ERROR_MODE_STOP_ON_ERROR), _))
        .WillOnce(
            DoAll(SetArrayArgument<3>(exp_remove_status.begin(), exp_remove_status.end()), Return(SAI_STATUS_SUCCESS)));
    EXPECT_CALL(mock_sai_next_hop_group_,
                remove_next_hop_group_members(Eq(1), ArrayEq(std::vector<sai_object_id_t>{kWcmpGroupMemberOid2}),
                                              Eq(SAI_BULK_OP_ERROR_MODE_STOP_ON_ERROR), _))
        .WillOnce(
            DoAll(SetArrayArgument<3>(exp_remove_status.begin(), exp_remove_status.end()), Return(SAI_STATUS_SUCCESS)));
    EXPECT_TRUE(ProcessUpdateRequest(&wcmp_group).ok());
    VerifyWcmpGroupEntry(wcmp_group, *GetWcmpGroupEntry(kWcmpGroupId1));
    uint32_t wcmp_group_refcount = 0;
    uint32_t nexthop_refcount = 0;
    ASSERT_TRUE(p4_oid_mapper_->getRefCount(SAI_OBJECT_TYPE_NEXT_HOP_GROUP, kWcmpGroupKey1, &wcmp_group_refcount));
    EXPECT_EQ(3, wcmp_group_refcount);
    ASSERT_TRUE(p4_oid_mapper_->getRefCount(SAI_OBJECT_TYPE_NEXT_HOP, kNexthopKey1, &nexthop_refcount));
    EXPECT_EQ(1, nexthop_refcount);
    ASSERT_TRUE(p4_oid_mapper_->getRefCount(SAI_OBJECT_TYPE_NEXT_HOP, kNexthopKey2, &nexthop_refcount));
    EXPECT_EQ(1, nexthop_refcount);
    ASSERT_TRUE(p4_oid_mapper_->getRefCount(SAI_OBJECT_TYPE_NEXT_HOP, kNexthopKey3, &nexthop_refcount));
    EXPECT_EQ(1, nexthop_refcount);
    // Remove WCMP group member with nexthop_id=kNexthopId1 and
    // nexthop_id=kNexthopId3(fail) - succeed to clean up
    wcmp_group.wcmp_group_members.clear();
    wcmp_group.wcmp_group_members.push_back(gm1);
    wcmp_group.wcmp_group_members.push_back(gm3);
    exp_remove_status = {SAI_STATUS_OBJECT_IN_USE, SAI_STATUS_SUCCESS};
    EXPECT_CALL(mock_sai_next_hop_group_,
                remove_next_hop_group_members(
                    Eq(2), ArrayEq(std::vector<sai_object_id_t>{kWcmpGroupMemberOid3, kWcmpGroupMemberOid5}),
                    Eq(SAI_BULK_OP_ERROR_MODE_STOP_ON_ERROR), _))
        .WillOnce(DoAll(SetArrayArgument<3>(exp_remove_status.begin(), exp_remove_status.end()),
                        Return(SAI_STATUS_OBJECT_IN_USE)));
    // Clean up - revert deletions -success
    std::vector<sai_object_id_t> return_oids_5{kWcmpGroupMemberOid5};
    EXPECT_CALL(mock_sai_next_hop_group_,
                create_next_hop_group_members(Eq(gSwitchId), Eq(1), ArrayEq(std::vector<uint32_t>{3}),
                                              AttrArrayArrayEq(std::vector<std::vector<sai_attribute_t>>{
                                                  GetSaiNextHopGroupMemberAttribute(kNexthopOid2, 10, kWcmpGroupOid1)}),
                                              Eq(SAI_BULK_OP_ERROR_MODE_STOP_ON_ERROR), _, _))
        .WillOnce(DoAll(SetArrayArgument<5>(return_oids_5.begin(), return_oids_5.end()),
                        SetArrayArgument<6>(exp_create_status_1.begin(), exp_create_status_1.end()),
                        Return(SAI_STATUS_SUCCESS)));
    EXPECT_EQ(StatusCode::SWSS_RC_IN_USE, ProcessUpdateRequest(&wcmp_group));
    P4WcmpGroupEntry expected_wcmp_group = {.wcmp_group_id = kWcmpGroupId1, .wcmp_group_members = {}};
    expected_wcmp_group.wcmp_group_members.push_back(gm1);
    expected_wcmp_group.wcmp_group_members.push_back(gm2);
    expected_wcmp_group.wcmp_group_members.push_back(gm3);
    // WCMP group remains as the old one
    VerifyWcmpGroupEntry(expected_wcmp_group, *GetWcmpGroupEntry(kWcmpGroupId1));
    ASSERT_TRUE(p4_oid_mapper_->getRefCount(SAI_OBJECT_TYPE_NEXT_HOP_GROUP, kWcmpGroupKey1, &wcmp_group_refcount));
    EXPECT_EQ(3, wcmp_group_refcount);
    ASSERT_TRUE(p4_oid_mapper_->getRefCount(SAI_OBJECT_TYPE_NEXT_HOP, kNexthopKey1, &nexthop_refcount));
    EXPECT_EQ(1, nexthop_refcount);
    ASSERT_TRUE(p4_oid_mapper_->getRefCount(SAI_OBJECT_TYPE_NEXT_HOP, kNexthopKey2, &nexthop_refcount));
    EXPECT_EQ(1, nexthop_refcount);
    ASSERT_TRUE(p4_oid_mapper_->getRefCount(SAI_OBJECT_TYPE_NEXT_HOP, kNexthopKey3, &nexthop_refcount));
    EXPECT_EQ(1, nexthop_refcount);

    // Remove WCMP group member with nexthop_id=kNexthopId1 and
    // nexthop_id=kNexthopId3(fail) - fail to clean up
    exp_remove_status = {SAI_STATUS_OBJECT_IN_USE, SAI_STATUS_SUCCESS};
    EXPECT_CALL(mock_sai_next_hop_group_,
                remove_next_hop_group_members(
                    Eq(2), ArrayEq(std::vector<sai_object_id_t>{kWcmpGroupMemberOid3, kWcmpGroupMemberOid5}),
                    Eq(SAI_BULK_OP_ERROR_MODE_STOP_ON_ERROR), _))
        .WillOnce(DoAll(SetArrayArgument<3>(exp_remove_status.begin(), exp_remove_status.end()),
                        Return(SAI_STATUS_OBJECT_IN_USE)));
    // Clean up - revert deletions -failure
    std::vector<sai_object_id_t> return_oids{SAI_NULL_OBJECT_ID};
    std::vector<sai_status_t> exp_create_status{SAI_STATUS_TABLE_FULL};
    EXPECT_CALL(mock_sai_next_hop_group_,
                create_next_hop_group_members(Eq(gSwitchId), Eq(1), ArrayEq(std::vector<uint32_t>{3}),
                                              AttrArrayArrayEq(std::vector<std::vector<sai_attribute_t>>{
                                                  GetSaiNextHopGroupMemberAttribute(kNexthopOid2, 10, kWcmpGroupOid1)}),
                                              Eq(SAI_BULK_OP_ERROR_MODE_STOP_ON_ERROR), _, _))
        .WillOnce(DoAll(SetArrayArgument<5>(return_oids.begin(), return_oids.end()),
                        SetArrayArgument<6>(exp_create_status.begin(), exp_create_status.end()),
                        Return(SAI_STATUS_TABLE_FULL)));
    // TODO: Expect critical state.
    EXPECT_EQ("Failed to delete WCMP group member: 'ju1u32m3.atl11:qe-3/7'",
              ProcessUpdateRequest(&wcmp_group).message());
    // WCMP group is as expected, but refcounts are not
    VerifyWcmpGroupEntry(expected_wcmp_group, *GetWcmpGroupEntry(kWcmpGroupId1));
    ASSERT_TRUE(p4_oid_mapper_->getRefCount(SAI_OBJECT_TYPE_NEXT_HOP_GROUP, kWcmpGroupKey1, &wcmp_group_refcount));
    EXPECT_EQ(2, wcmp_group_refcount);
    // WCMP group is corrupt due to clean up failure
    ASSERT_TRUE(p4_oid_mapper_->getRefCount(SAI_OBJECT_TYPE_NEXT_HOP, kNexthopKey1, &nexthop_refcount));
    EXPECT_EQ(1, nexthop_refcount);
    ASSERT_TRUE(p4_oid_mapper_->getRefCount(SAI_OBJECT_TYPE_NEXT_HOP, kNexthopKey2, &nexthop_refcount));
    EXPECT_EQ(0, nexthop_refcount);
    ASSERT_TRUE(p4_oid_mapper_->getRefCount(SAI_OBJECT_TYPE_NEXT_HOP, kNexthopKey3, &nexthop_refcount));
    EXPECT_EQ(1, nexthop_refcount);
}

TEST_F(WcmpManagerTest, UpdateWcmpGroupFailsWhenCreateNewGroupMemberSaiCallFails)
{
    AddWcmpGroupEntry1();
    P4WcmpGroupEntry wcmp_group = {.wcmp_group_id = kWcmpGroupId1, .wcmp_group_members = {}};

    // Remove group member with nexthop_id=kNexthopId1
    wcmp_group.wcmp_group_members.clear();
    std::shared_ptr<P4WcmpGroupMemberEntry> gm = createWcmpGroupMemberEntry(kNexthopId2, 15);
    wcmp_group.wcmp_group_members.push_back(gm);
    std::vector<sai_status_t> exp_remove_status{SAI_STATUS_SUCCESS};
    EXPECT_CALL(mock_sai_next_hop_group_,
                remove_next_hop_group_members(Eq(1), ArrayEq(std::vector<sai_object_id_t>{kWcmpGroupMemberOid1}),
                                              Eq(SAI_BULK_OP_ERROR_MODE_STOP_ON_ERROR), _))
        .WillOnce(
            DoAll(SetArrayArgument<3>(exp_remove_status.begin(), exp_remove_status.end()), Return(SAI_STATUS_SUCCESS)));
    EXPECT_CALL(mock_sai_next_hop_group_,
                remove_next_hop_group_members(Eq(1), ArrayEq(std::vector<sai_object_id_t>{kWcmpGroupMemberOid2}),
                                              Eq(SAI_BULK_OP_ERROR_MODE_STOP_ON_ERROR), _))
        .WillOnce(
            DoAll(SetArrayArgument<3>(exp_remove_status.begin(), exp_remove_status.end()), Return(SAI_STATUS_SUCCESS)));
    std::vector<sai_object_id_t> return_oids_5{kWcmpGroupMemberOid5};
    std::vector<sai_status_t> exp_create_status{SAI_STATUS_SUCCESS};
    EXPECT_CALL(mock_sai_next_hop_group_,
                create_next_hop_group_members(Eq(gSwitchId), Eq(1), ArrayEq(std::vector<uint32_t>{3}),
                                              AttrArrayArrayEq(std::vector<std::vector<sai_attribute_t>>{
                                                  GetSaiNextHopGroupMemberAttribute(kNexthopOid2, 15, kWcmpGroupOid1)}),
                                              Eq(SAI_BULK_OP_ERROR_MODE_STOP_ON_ERROR), _, _))
        .WillOnce(DoAll(SetArrayArgument<5>(return_oids_5.begin(), return_oids_5.end()),
                        SetArrayArgument<6>(exp_create_status.begin(), exp_create_status.end()),
                        Return(SAI_STATUS_SUCCESS)));
    EXPECT_TRUE(ProcessUpdateRequest(&wcmp_group).ok());
    VerifyWcmpGroupEntry(wcmp_group, *GetWcmpGroupEntry(kWcmpGroupId1));
    uint32_t wcmp_group_refcount = 0;
    uint32_t nexthop_refcount = 0;
    ASSERT_TRUE(p4_oid_mapper_->getRefCount(SAI_OBJECT_TYPE_NEXT_HOP_GROUP, kWcmpGroupKey1, &wcmp_group_refcount));
    EXPECT_EQ(1, wcmp_group_refcount);
    ASSERT_TRUE(p4_oid_mapper_->getRefCount(SAI_OBJECT_TYPE_NEXT_HOP, kNexthopKey1, &nexthop_refcount));
    EXPECT_EQ(0, nexthop_refcount);
    ASSERT_TRUE(p4_oid_mapper_->getRefCount(SAI_OBJECT_TYPE_NEXT_HOP, kNexthopKey2, &nexthop_refcount));
    EXPECT_EQ(1, nexthop_refcount);
    ASSERT_TRUE(p4_oid_mapper_->getRefCount(SAI_OBJECT_TYPE_NEXT_HOP, kNexthopKey3, &nexthop_refcount));
    EXPECT_EQ(0, nexthop_refcount);
    // Add WCMP group member with nexthop_id=kNexthopId1, weight=3 and
    // nexthop_id=kNexthopId3, weight=30(fail), update nexthop_id=kNexthopId2
    // weight to 10.
    P4WcmpGroupEntry updated_wcmp_group = {.wcmp_group_id = kWcmpGroupId1, .wcmp_group_members = {}};
    std::shared_ptr<P4WcmpGroupMemberEntry> updated_gm1 = createWcmpGroupMemberEntry(kNexthopId1, 3);
    std::shared_ptr<P4WcmpGroupMemberEntry> updated_gm2 = createWcmpGroupMemberEntry(kNexthopId2, 20);
    std::shared_ptr<P4WcmpGroupMemberEntry> updated_gm3 = createWcmpGroupMemberEntry(kNexthopId3, 30);
    updated_wcmp_group.wcmp_group_members.push_back(updated_gm1);
    updated_wcmp_group.wcmp_group_members.push_back(updated_gm2);
    updated_wcmp_group.wcmp_group_members.push_back(updated_gm3);
    EXPECT_CALL(mock_sai_next_hop_group_,
                remove_next_hop_group_members(Eq(1), ArrayEq(std::vector<sai_object_id_t>{kWcmpGroupMemberOid5}),
                                              Eq(SAI_BULK_OP_ERROR_MODE_STOP_ON_ERROR), _))
        .WillOnce(
            DoAll(SetArrayArgument<3>(exp_remove_status.begin(), exp_remove_status.end()), Return(SAI_STATUS_SUCCESS)));
    std::vector<sai_object_id_t> return_oids_1{kWcmpGroupMemberOid1};
    std::vector<sai_status_t> exp_create_status_fail{SAI_STATUS_SUCCESS, SAI_STATUS_TABLE_FULL};
    std::vector<sai_object_id_t> return_oids_2_null{kWcmpGroupMemberOid2, SAI_NULL_OBJECT_ID};
    EXPECT_CALL(mock_sai_next_hop_group_,
                create_next_hop_group_members(Eq(gSwitchId), Eq(1), ArrayEq(std::vector<uint32_t>{3}),
                                              AttrArrayArrayEq(std::vector<std::vector<sai_attribute_t>>{
                                                  GetSaiNextHopGroupMemberAttribute(kNexthopOid1, 3, kWcmpGroupOid1)}),
                                              Eq(SAI_BULK_OP_ERROR_MODE_STOP_ON_ERROR), _, _))
        .WillOnce(DoAll(SetArrayArgument<5>(return_oids_1.begin(), return_oids_1.end()),
                        SetArrayArgument<6>(exp_create_status.begin(), exp_create_status.end()),
                        Return(SAI_STATUS_SUCCESS)));
    EXPECT_CALL(mock_sai_next_hop_group_,
                create_next_hop_group_members(Eq(gSwitchId), Eq(2), ArrayEq(std::vector<uint32_t>{3, 3}),
                                              AttrArrayArrayEq(std::vector<std::vector<sai_attribute_t>>{
                                                  GetSaiNextHopGroupMemberAttribute(kNexthopOid2, 20, kWcmpGroupOid1),
                                                  GetSaiNextHopGroupMemberAttribute(kNexthopOid3, 30, kWcmpGroupOid1)}),
                                              Eq(SAI_BULK_OP_ERROR_MODE_STOP_ON_ERROR), _, _))
        .WillOnce(DoAll(SetArrayArgument<5>(return_oids_2_null.begin(), return_oids_2_null.end()),
                        SetArrayArgument<6>(exp_create_status_fail.begin(), exp_create_status_fail.end()),
                        Return(SAI_STATUS_TABLE_FULL)));
    // Clean up - success
    std::vector<sai_status_t> exp_remove_status_2{SAI_STATUS_SUCCESS, SAI_STATUS_SUCCESS};
    EXPECT_CALL(mock_sai_next_hop_group_,
                remove_next_hop_group_members(
                    Eq(2), ArrayEq(std::vector<sai_object_id_t>{kWcmpGroupMemberOid2, kWcmpGroupMemberOid1}),
                    Eq(SAI_BULK_OP_ERROR_MODE_STOP_ON_ERROR), _))
        .WillOnce(DoAll(SetArrayArgument<3>(exp_remove_status_2.begin(), exp_remove_status_2.end()),
                        Return(SAI_STATUS_SUCCESS)));
    EXPECT_CALL(mock_sai_next_hop_group_,
                create_next_hop_group_members(Eq(gSwitchId), Eq(1), ArrayEq(std::vector<uint32_t>{3}),
                                              AttrArrayArrayEq(std::vector<std::vector<sai_attribute_t>>{
                                                  GetSaiNextHopGroupMemberAttribute(kNexthopOid2, 15, kWcmpGroupOid1)}),
                                              Eq(SAI_BULK_OP_ERROR_MODE_STOP_ON_ERROR), _, _))
        .WillOnce(DoAll(SetArrayArgument<5>(return_oids_5.begin(), return_oids_5.end()),
                        SetArrayArgument<6>(exp_create_status.begin(), exp_create_status.end()),
                        Return(SAI_STATUS_SUCCESS)));
    EXPECT_FALSE(ProcessUpdateRequest(&updated_wcmp_group).ok());
    P4WcmpGroupEntry expected_wcmp_group = {.wcmp_group_id = kWcmpGroupId1, .wcmp_group_members = {}};
    std::shared_ptr<P4WcmpGroupMemberEntry> expected_gm = createWcmpGroupMemberEntry(kNexthopId2, 15);
    expected_wcmp_group.wcmp_group_members.push_back(expected_gm);
    VerifyWcmpGroupEntry(expected_wcmp_group, *GetWcmpGroupEntry(kWcmpGroupId1));
    ASSERT_TRUE(p4_oid_mapper_->getRefCount(SAI_OBJECT_TYPE_NEXT_HOP_GROUP, kWcmpGroupKey1, &wcmp_group_refcount));
    EXPECT_EQ(1, wcmp_group_refcount);
    ASSERT_TRUE(p4_oid_mapper_->getRefCount(SAI_OBJECT_TYPE_NEXT_HOP, kNexthopKey1, &nexthop_refcount));
    EXPECT_EQ(0, nexthop_refcount);
    ASSERT_TRUE(p4_oid_mapper_->getRefCount(SAI_OBJECT_TYPE_NEXT_HOP, kNexthopKey2, &nexthop_refcount));
    EXPECT_EQ(1, nexthop_refcount);
    ASSERT_TRUE(p4_oid_mapper_->getRefCount(SAI_OBJECT_TYPE_NEXT_HOP, kNexthopKey3, &nexthop_refcount));
    EXPECT_EQ(0, nexthop_refcount);

    // Try again, but this time clean up failed to remove created group member
    exp_remove_status = {SAI_STATUS_SUCCESS};
    EXPECT_CALL(mock_sai_next_hop_group_,
                remove_next_hop_group_members(Eq(1), ArrayEq(std::vector<sai_object_id_t>{kWcmpGroupMemberOid5}),
                                              Eq(SAI_BULK_OP_ERROR_MODE_STOP_ON_ERROR), _))
        .WillOnce(
            DoAll(SetArrayArgument<3>(exp_remove_status.begin(), exp_remove_status.end()), Return(SAI_STATUS_SUCCESS)));
    EXPECT_CALL(mock_sai_next_hop_group_,
                create_next_hop_group_members(Eq(gSwitchId), Eq(1), ArrayEq(std::vector<uint32_t>{3}),
                                              AttrArrayArrayEq(std::vector<std::vector<sai_attribute_t>>{
                                                  GetSaiNextHopGroupMemberAttribute(kNexthopOid1, 3, kWcmpGroupOid1)}),
                                              Eq(SAI_BULK_OP_ERROR_MODE_STOP_ON_ERROR), _, _))
        .WillOnce(DoAll(SetArrayArgument<5>(return_oids_1.begin(), return_oids_1.end()),
                        SetArrayArgument<6>(exp_create_status.begin(), exp_create_status.end()),
                        Return(SAI_STATUS_SUCCESS)));
    EXPECT_CALL(mock_sai_next_hop_group_,
                create_next_hop_group_members(Eq(gSwitchId), Eq(2), ArrayEq(std::vector<uint32_t>{3, 3}),
                                              AttrArrayArrayEq(std::vector<std::vector<sai_attribute_t>>{
                                                  GetSaiNextHopGroupMemberAttribute(kNexthopOid2, 20, kWcmpGroupOid1),
                                                  GetSaiNextHopGroupMemberAttribute(kNexthopOid3, 30, kWcmpGroupOid1)}),
                                              Eq(SAI_BULK_OP_ERROR_MODE_STOP_ON_ERROR), _, _))
        .WillOnce(DoAll(SetArrayArgument<5>(return_oids_2_null.begin(), return_oids_2_null.end()),
                        SetArrayArgument<6>(exp_create_status_fail.begin(), exp_create_status_fail.end()),
                        Return(SAI_STATUS_TABLE_FULL)));
    // Clean up - revert creation - failure
    std::vector<sai_status_t> exp_remove_status_fail{SAI_STATUS_OBJECT_IN_USE, SAI_STATUS_SUCCESS};
    EXPECT_CALL(mock_sai_next_hop_group_,
                remove_next_hop_group_members(
                    Eq(2), ArrayEq(std::vector<sai_object_id_t>{kWcmpGroupMemberOid2, kWcmpGroupMemberOid1}),
                    Eq(SAI_BULK_OP_ERROR_MODE_STOP_ON_ERROR), _))
        .WillOnce(DoAll(SetArrayArgument<3>(exp_remove_status_fail.begin(), exp_remove_status_fail.end()),
                        Return(SAI_STATUS_OBJECT_IN_USE)));
    // TODO: Expect critical state.
    EXPECT_EQ("Fail to create wcmp group member: 'ju1u32m3.atl11:qe-3/7'",
              ProcessUpdateRequest(&updated_wcmp_group).message());
    //  WCMP group is as expected, but refcounts are not
    VerifyWcmpGroupEntry(expected_wcmp_group, *GetWcmpGroupEntry(kWcmpGroupId1));
    ASSERT_TRUE(p4_oid_mapper_->getRefCount(SAI_OBJECT_TYPE_NEXT_HOP_GROUP, kWcmpGroupKey1, &wcmp_group_refcount));
    EXPECT_EQ(1, wcmp_group_refcount);
    ASSERT_TRUE(p4_oid_mapper_->getRefCount(SAI_OBJECT_TYPE_NEXT_HOP, kNexthopKey1, &nexthop_refcount));
    EXPECT_EQ(0, nexthop_refcount); // Corrupt status due to clean up failure
    ASSERT_TRUE(p4_oid_mapper_->getRefCount(SAI_OBJECT_TYPE_NEXT_HOP, kNexthopKey2, &nexthop_refcount));
    EXPECT_EQ(1, nexthop_refcount);
    ASSERT_TRUE(p4_oid_mapper_->getRefCount(SAI_OBJECT_TYPE_NEXT_HOP, kNexthopKey3, &nexthop_refcount));
    EXPECT_EQ(0, nexthop_refcount);
}

TEST_F(WcmpManagerTest, UpdateWcmpGroupFailsWhenReduceGroupMemberWeightSaiCallFails)
{
    AddWcmpGroupEntry1();
    P4WcmpGroupEntry wcmp_group = {.wcmp_group_id = kWcmpGroupId1, .wcmp_group_members = {}};
    // Update WCMP group member to nexthop_id=kNexthopId1, weight=1(reduce) and
    // nexthop_id=kNexthopId2, weight=10(increase), update nexthop_id=kNexthopId1
    // weight=1(fail).
    std::shared_ptr<P4WcmpGroupMemberEntry> gm1 = createWcmpGroupMemberEntry(kNexthopId1, 1);
    std::shared_ptr<P4WcmpGroupMemberEntry> gm2 = createWcmpGroupMemberEntry(kNexthopId2, 10);
    wcmp_group.wcmp_group_members.push_back(gm1);
    wcmp_group.wcmp_group_members.push_back(gm2);
    std::vector<sai_status_t> exp_remove_status{SAI_STATUS_SUCCESS};
    EXPECT_CALL(mock_sai_next_hop_group_,
                remove_next_hop_group_members(Eq(1), ArrayEq(std::vector<sai_object_id_t>{kWcmpGroupMemberOid1}),
                                              Eq(SAI_BULK_OP_ERROR_MODE_STOP_ON_ERROR), _))
        .WillOnce(
            DoAll(SetArrayArgument<3>(exp_remove_status.begin(), exp_remove_status.end()), Return(SAI_STATUS_SUCCESS)));
    std::vector<sai_object_id_t> return_oids_1{kWcmpGroupMemberOid1};
    std::vector<sai_object_id_t> return_oids_null{SAI_NULL_OBJECT_ID};
    std::vector<sai_status_t> exp_create_status{SAI_STATUS_SUCCESS};
    std::vector<sai_status_t> exp_create_status_fail{SAI_STATUS_NOT_SUPPORTED};
    EXPECT_CALL(mock_sai_next_hop_group_,
                create_next_hop_group_members(Eq(gSwitchId), Eq(1), ArrayEq(std::vector<uint32_t>{3}),
                                              AttrArrayArrayEq(std::vector<std::vector<sai_attribute_t>>{
                                                  GetSaiNextHopGroupMemberAttribute(kNexthopOid1, 1, kWcmpGroupOid1)}),
                                              Eq(SAI_BULK_OP_ERROR_MODE_STOP_ON_ERROR), _, _))
        .WillOnce(DoAll(SetArrayArgument<5>(return_oids_null.begin(), return_oids_null.end()),
                        SetArrayArgument<6>(exp_create_status_fail.begin(), exp_create_status_fail.end()),
                        Return(SAI_STATUS_NOT_SUPPORTED)));
    EXPECT_CALL(mock_sai_next_hop_group_,
                create_next_hop_group_members(Eq(gSwitchId), Eq(1), ArrayEq(std::vector<uint32_t>{3}),
                                              AttrArrayArrayEq(std::vector<std::vector<sai_attribute_t>>{
                                                  GetSaiNextHopGroupMemberAttribute(kNexthopOid1, 2, kWcmpGroupOid1)}),
                                              Eq(SAI_BULK_OP_ERROR_MODE_STOP_ON_ERROR), _, _))
        .WillOnce(DoAll(SetArrayArgument<5>(return_oids_1.begin(), return_oids_1.end()),
                        SetArrayArgument<6>(exp_create_status.begin(), exp_create_status.end()),
                        Return(SAI_STATUS_NOT_SUPPORTED)));
    EXPECT_FALSE(ProcessUpdateRequest(&wcmp_group).ok());
    P4WcmpGroupEntry expected_wcmp_group = {.wcmp_group_id = kWcmpGroupId1, .wcmp_group_members = {}};
    std::shared_ptr<P4WcmpGroupMemberEntry> expected_gm1 = createWcmpGroupMemberEntry(kNexthopId1, 2);
    std::shared_ptr<P4WcmpGroupMemberEntry> expected_gm2 = createWcmpGroupMemberEntry(kNexthopId2, 1);
    expected_wcmp_group.wcmp_group_members.push_back(expected_gm1);
    expected_wcmp_group.wcmp_group_members.push_back(expected_gm2);
    VerifyWcmpGroupEntry(expected_wcmp_group, *GetWcmpGroupEntry(kWcmpGroupId1));
    uint32_t wcmp_group_refcount = 0;
    uint32_t nexthop_refcount = 0;
    ASSERT_TRUE(p4_oid_mapper_->getRefCount(SAI_OBJECT_TYPE_NEXT_HOP_GROUP, kWcmpGroupKey1, &wcmp_group_refcount));
    EXPECT_EQ(2, wcmp_group_refcount);
    ASSERT_TRUE(p4_oid_mapper_->getRefCount(SAI_OBJECT_TYPE_NEXT_HOP, kNexthopKey1, &nexthop_refcount));
    EXPECT_EQ(1, nexthop_refcount);
    ASSERT_TRUE(p4_oid_mapper_->getRefCount(SAI_OBJECT_TYPE_NEXT_HOP, kNexthopKey2, &nexthop_refcount));
    EXPECT_EQ(1, nexthop_refcount);
}

TEST_F(WcmpManagerTest, UpdateWcmpGroupFailsWhenIncreaseGroupMemberWeightSaiCallFails)
{
    AddWcmpGroupEntry1();
    P4WcmpGroupEntry wcmp_group = {.wcmp_group_id = kWcmpGroupId1, .wcmp_group_members = {}};
    // Update WCMP group member to nexthop_id=kNexthopId1, weight=1(reduce) and
    // nexthop_id=kNexthopId2, weight=10(increase), update nexthop_id=kNexthopId2
    // weight=10(fail).
    std::shared_ptr<P4WcmpGroupMemberEntry> gm1 = createWcmpGroupMemberEntry(kNexthopId1, 1);
    std::shared_ptr<P4WcmpGroupMemberEntry> gm2 = createWcmpGroupMemberEntry(kNexthopId2, 10);
    wcmp_group.wcmp_group_members.push_back(gm1);
    wcmp_group.wcmp_group_members.push_back(gm2);
    std::vector<sai_status_t> exp_remove_status{SAI_STATUS_SUCCESS};
    EXPECT_CALL(mock_sai_next_hop_group_,
                remove_next_hop_group_members(Eq(1), ArrayEq(std::vector<sai_object_id_t>{kWcmpGroupMemberOid1}),
                                              Eq(SAI_BULK_OP_ERROR_MODE_STOP_ON_ERROR), _))
        .WillOnce(
            DoAll(SetArrayArgument<3>(exp_remove_status.begin(), exp_remove_status.end()), Return(SAI_STATUS_SUCCESS)));
    EXPECT_CALL(mock_sai_next_hop_group_,
                remove_next_hop_group_members(Eq(1), ArrayEq(std::vector<sai_object_id_t>{kWcmpGroupMemberOid2}),
                                              Eq(SAI_BULK_OP_ERROR_MODE_STOP_ON_ERROR), _))
        .WillOnce(
            DoAll(SetArrayArgument<3>(exp_remove_status.begin(), exp_remove_status.end()), Return(SAI_STATUS_SUCCESS)));
    std::vector<sai_object_id_t> return_oids_4{kWcmpGroupMemberOid4};
    std::vector<sai_object_id_t> return_oids_null{SAI_NULL_OBJECT_ID};
    std::vector<sai_status_t> exp_create_status{SAI_STATUS_SUCCESS};
    std::vector<sai_status_t> exp_create_status_fail{SAI_STATUS_NOT_SUPPORTED};
    EXPECT_CALL(mock_sai_next_hop_group_,
                create_next_hop_group_members(Eq(gSwitchId), Eq(1), ArrayEq(std::vector<uint32_t>{3}),
                                              AttrArrayArrayEq(std::vector<std::vector<sai_attribute_t>>{
                                                  GetSaiNextHopGroupMemberAttribute(kNexthopOid1, 1, kWcmpGroupOid1)}),
                                              Eq(SAI_BULK_OP_ERROR_MODE_STOP_ON_ERROR), _, _))
        .WillOnce(DoAll(SetArrayArgument<5>(return_oids_4.begin(), return_oids_4.end()),
                        SetArrayArgument<6>(exp_create_status.begin(), exp_create_status.end()),
                        Return(SAI_STATUS_SUCCESS)));
    EXPECT_CALL(mock_sai_next_hop_group_,
                create_next_hop_group_members(Eq(gSwitchId), Eq(1), ArrayEq(std::vector<uint32_t>{3}),
                                              AttrArrayArrayEq(std::vector<std::vector<sai_attribute_t>>{
                                                  GetSaiNextHopGroupMemberAttribute(kNexthopOid2, 10, kWcmpGroupOid1)}),
                                              Eq(SAI_BULK_OP_ERROR_MODE_STOP_ON_ERROR), _, _))
        .WillOnce(DoAll(SetArrayArgument<5>(return_oids_null.begin(), return_oids_null.end()),
                        SetArrayArgument<6>(exp_create_status_fail.begin(), exp_create_status_fail.end()),
                        Return(SAI_STATUS_NOT_SUPPORTED)));
    // Clean up modified members - success
    EXPECT_CALL(mock_sai_next_hop_group_,
                remove_next_hop_group_members(Eq(1), ArrayEq(std::vector<sai_object_id_t>{kWcmpGroupMemberOid4}),
                                              Eq(SAI_BULK_OP_ERROR_MODE_STOP_ON_ERROR), _))
        .WillOnce(
            DoAll(SetArrayArgument<3>(exp_remove_status.begin(), exp_remove_status.end()), Return(SAI_STATUS_SUCCESS)));
    std::vector<sai_object_id_t> return_oids_1_2{kWcmpGroupMemberOid1, kWcmpGroupMemberOid2};
    std::vector<sai_status_t> exp_create_status_2{SAI_STATUS_SUCCESS, SAI_STATUS_SUCCESS};
    EXPECT_CALL(mock_sai_next_hop_group_,
                create_next_hop_group_members(Eq(gSwitchId), Eq(2), ArrayEq(std::vector<uint32_t>{3, 3}),
                                              AttrArrayArrayEq(std::vector<std::vector<sai_attribute_t>>{
                                                  GetSaiNextHopGroupMemberAttribute(kNexthopOid1, 2, kWcmpGroupOid1),
                                                  GetSaiNextHopGroupMemberAttribute(kNexthopOid2, 1, kWcmpGroupOid1)}),
                                              Eq(SAI_BULK_OP_ERROR_MODE_STOP_ON_ERROR), _, _))
        .WillOnce(DoAll(SetArrayArgument<5>(return_oids_1_2.begin(), return_oids_1_2.end()),
                        SetArrayArgument<6>(exp_create_status_2.begin(), exp_create_status_2.end()),
                        Return(SAI_STATUS_SUCCESS)));
    EXPECT_FALSE(ProcessUpdateRequest(&wcmp_group).ok());
    P4WcmpGroupEntry expected_wcmp_group = {.wcmp_group_id = kWcmpGroupId1, .wcmp_group_members = {}};
    std::shared_ptr<P4WcmpGroupMemberEntry> expected_gm1 = createWcmpGroupMemberEntry(kNexthopId1, 2);
    std::shared_ptr<P4WcmpGroupMemberEntry> expected_gm2 = createWcmpGroupMemberEntry(kNexthopId2, 1);
    expected_wcmp_group.wcmp_group_members.push_back(expected_gm1);
    expected_wcmp_group.wcmp_group_members.push_back(expected_gm2);
    VerifyWcmpGroupEntry(expected_wcmp_group, *GetWcmpGroupEntry(kWcmpGroupId1));
    uint32_t wcmp_group_refcount = 0;
    uint32_t nexthop_refcount = 0;
    ASSERT_TRUE(p4_oid_mapper_->getRefCount(SAI_OBJECT_TYPE_NEXT_HOP_GROUP, kWcmpGroupKey1, &wcmp_group_refcount));
    EXPECT_EQ(2, wcmp_group_refcount);
    ASSERT_TRUE(p4_oid_mapper_->getRefCount(SAI_OBJECT_TYPE_NEXT_HOP, kNexthopKey1, &nexthop_refcount));
    EXPECT_EQ(1, nexthop_refcount);
    ASSERT_TRUE(p4_oid_mapper_->getRefCount(SAI_OBJECT_TYPE_NEXT_HOP, kNexthopKey2, &nexthop_refcount));
    EXPECT_EQ(1, nexthop_refcount);
    // Try again, the same error happens when update and new error during clean up
    EXPECT_CALL(mock_sai_next_hop_group_,
                remove_next_hop_group_members(Eq(1), ArrayEq(std::vector<sai_object_id_t>{kWcmpGroupMemberOid1}),
                                              Eq(SAI_BULK_OP_ERROR_MODE_STOP_ON_ERROR), _))
        .WillOnce(
            DoAll(SetArrayArgument<3>(exp_remove_status.begin(), exp_remove_status.end()), Return(SAI_STATUS_SUCCESS)));
    EXPECT_CALL(mock_sai_next_hop_group_,
                remove_next_hop_group_members(Eq(1), ArrayEq(std::vector<sai_object_id_t>{kWcmpGroupMemberOid2}),
                                              Eq(SAI_BULK_OP_ERROR_MODE_STOP_ON_ERROR), _))
        .WillOnce(
            DoAll(SetArrayArgument<3>(exp_remove_status.begin(), exp_remove_status.end()), Return(SAI_STATUS_SUCCESS)));
    EXPECT_CALL(mock_sai_next_hop_group_,
                create_next_hop_group_members(Eq(gSwitchId), Eq(1), ArrayEq(std::vector<uint32_t>{3}),
                                              AttrArrayArrayEq(std::vector<std::vector<sai_attribute_t>>{
                                                  GetSaiNextHopGroupMemberAttribute(kNexthopOid1, 1, kWcmpGroupOid1)}),
                                              Eq(SAI_BULK_OP_ERROR_MODE_STOP_ON_ERROR), _, _))
        .WillOnce(DoAll(SetArrayArgument<5>(return_oids_4.begin(), return_oids_4.end()),
                        SetArrayArgument<6>(exp_create_status.begin(), exp_create_status.end()),
                        Return(SAI_STATUS_SUCCESS)));
    EXPECT_CALL(mock_sai_next_hop_group_,
                create_next_hop_group_members(Eq(gSwitchId), Eq(1), ArrayEq(std::vector<uint32_t>{3}),
                                              AttrArrayArrayEq(std::vector<std::vector<sai_attribute_t>>{
                                                  GetSaiNextHopGroupMemberAttribute(kNexthopOid2, 10, kWcmpGroupOid1)}),
                                              Eq(SAI_BULK_OP_ERROR_MODE_STOP_ON_ERROR), _, _))
        .WillOnce(DoAll(SetArrayArgument<5>(return_oids_null.begin(), return_oids_null.end()),
                        SetArrayArgument<6>(exp_create_status_fail.begin(), exp_create_status_fail.end()),
                        Return(SAI_STATUS_NOT_SUPPORTED)));
    // Clean up modified members - failure
    EXPECT_CALL(mock_sai_next_hop_group_,
                remove_next_hop_group_members(Eq(1), ArrayEq(std::vector<sai_object_id_t>{kWcmpGroupMemberOid4}),
                                              Eq(SAI_BULK_OP_ERROR_MODE_STOP_ON_ERROR), _))
        .WillOnce(
            DoAll(SetArrayArgument<3>(exp_remove_status.begin(), exp_remove_status.end()), Return(SAI_STATUS_SUCCESS)));
    std::vector<sai_object_id_t> return_oids_2_null{SAI_NULL_OBJECT_ID, kWcmpGroupMemberOid2};
    std::vector<sai_status_t> exp_create_status_2_fail{SAI_STATUS_NOT_SUPPORTED, SAI_STATUS_SUCCESS};
    EXPECT_CALL(mock_sai_next_hop_group_,
                create_next_hop_group_members(Eq(gSwitchId), Eq(2), ArrayEq(std::vector<uint32_t>{3, 3}),
                                              AttrArrayArrayEq(std::vector<std::vector<sai_attribute_t>>{
                                                  GetSaiNextHopGroupMemberAttribute(kNexthopOid1, 2, kWcmpGroupOid1),
                                                  GetSaiNextHopGroupMemberAttribute(kNexthopOid2, 1, kWcmpGroupOid1)}),
                                              Eq(SAI_BULK_OP_ERROR_MODE_STOP_ON_ERROR), _, _))
        .WillOnce(DoAll(SetArrayArgument<5>(return_oids_2_null.begin(), return_oids_2_null.end()),
                        SetArrayArgument<6>(exp_create_status_2_fail.begin(), exp_create_status_2_fail.end()),
                        Return(SAI_STATUS_NOT_SUPPORTED)));

    // TODO: Expect critical state.
    EXPECT_EQ("Fail to create wcmp group member: 'ju1u32m2.atl11:qe-3/7'", ProcessUpdateRequest(&wcmp_group).message());
    // weight of wcmp_group_members[kNexthopId1] unable to revert
    // SAI object in ASIC DB: missing group member with
    // next_hop_id=kNexthopId1
    expected_gm1->weight = 2;
    VerifyWcmpGroupEntry(expected_wcmp_group, *GetWcmpGroupEntry(kWcmpGroupId1));
    ASSERT_TRUE(p4_oid_mapper_->getRefCount(SAI_OBJECT_TYPE_NEXT_HOP_GROUP, kWcmpGroupKey1, &wcmp_group_refcount));
    EXPECT_EQ(1, wcmp_group_refcount);
    ASSERT_TRUE(p4_oid_mapper_->getRefCount(SAI_OBJECT_TYPE_NEXT_HOP, kNexthopKey1, &nexthop_refcount));
    EXPECT_EQ(0, nexthop_refcount);
    ASSERT_TRUE(p4_oid_mapper_->getRefCount(SAI_OBJECT_TYPE_NEXT_HOP, kNexthopKey2, &nexthop_refcount));
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
    Drain();
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
    Drain();
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
    Drain();
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
    Drain();
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
        .WillOnce(DoAll(SetArgPointee<0>(kWcmpGroupOid1), Return(SAI_STATUS_SUCCESS)));

    std::vector<sai_object_id_t> return_oids{kWcmpGroupMemberOid1, kWcmpGroupMemberOid2};
    std::vector<sai_status_t> exp_create_status{SAI_STATUS_SUCCESS, SAI_STATUS_SUCCESS};
    EXPECT_CALL(mock_sai_next_hop_group_,
                create_next_hop_group_members(Eq(gSwitchId), Eq(2), ArrayEq(std::vector<uint32_t>{3, 3}),
                                              AttrArrayArrayEq(std::vector<std::vector<sai_attribute_t>>{
                                                  GetSaiNextHopGroupMemberAttribute(kNexthopOid1, 1, kWcmpGroupOid1),
                                                  GetSaiNextHopGroupMemberAttribute(kNexthopOid2, 1, kWcmpGroupOid1)}),
                                              Eq(SAI_BULK_OP_ERROR_MODE_STOP_ON_ERROR), _, _))
        .WillOnce(DoAll(SetArrayArgument<5>(return_oids.begin(), return_oids.end()),
                        SetArrayArgument<6>(exp_create_status.begin(), exp_create_status.end()),
                        Return(SAI_STATUS_SUCCESS)));
    Drain();
    std::string key = KeyGenerator::generateWcmpGroupKey(kWcmpGroupId1);
    auto *wcmp_group_entry_ptr = GetWcmpGroupEntry(kWcmpGroupId1);
    EXPECT_NE(nullptr, wcmp_group_entry_ptr);
    EXPECT_TRUE(p4_oid_mapper_->existsOID(SAI_OBJECT_TYPE_NEXT_HOP_GROUP, key));
    uint32_t ref_cnt;
    EXPECT_TRUE(p4_oid_mapper_->getRefCount(SAI_OBJECT_TYPE_NEXT_HOP, kNexthopKey1, &ref_cnt));
    EXPECT_EQ(1, ref_cnt);
    EXPECT_TRUE(p4_oid_mapper_->getRefCount(SAI_OBJECT_TYPE_NEXT_HOP, kNexthopKey2, &ref_cnt));
    EXPECT_EQ(1, ref_cnt);

    std::vector<sai_status_t> exp_remove_status{SAI_STATUS_SUCCESS, SAI_STATUS_SUCCESS};
    EXPECT_CALL(mock_sai_next_hop_group_,
                remove_next_hop_group_members(
                    Eq(2), ArrayEq(std::vector<sai_object_id_t>{kWcmpGroupMemberOid2, kWcmpGroupMemberOid1}),
                    Eq(SAI_BULK_OP_ERROR_MODE_STOP_ON_ERROR), _))
        .WillOnce(
            DoAll(SetArrayArgument<3>(exp_remove_status.begin(), exp_remove_status.end()), Return(SAI_STATUS_SUCCESS)));
    EXPECT_CALL(mock_sai_next_hop_group_, remove_next_hop_group(Eq(kWcmpGroupOid1)))
        .WillOnce(Return(SAI_STATUS_SUCCESS));
    attributes.clear();
    Enqueue(swss::KeyOpFieldsValuesTuple(kKeyPrefix + j.dump(), DEL_COMMAND, attributes));
    Drain();
    wcmp_group_entry_ptr = GetWcmpGroupEntry(kWcmpGroupId1);
    EXPECT_EQ(nullptr, wcmp_group_entry_ptr);
    EXPECT_FALSE(p4_oid_mapper_->existsOID(SAI_OBJECT_TYPE_NEXT_HOP_GROUP, key));
    EXPECT_TRUE(p4_oid_mapper_->getRefCount(SAI_OBJECT_TYPE_NEXT_HOP, kNexthopKey1, &ref_cnt));
    EXPECT_EQ(0, ref_cnt);
    EXPECT_TRUE(p4_oid_mapper_->getRefCount(SAI_OBJECT_TYPE_NEXT_HOP, kNexthopKey2, &ref_cnt));
    EXPECT_EQ(0, ref_cnt);
}

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
        .WillOnce(DoAll(SetArgPointee<0>(kWcmpGroupOid1), Return(SAI_STATUS_SUCCESS)));
    std::vector<sai_object_id_t> return_oids{kWcmpGroupMemberOid1};
    std::vector<sai_status_t> exp_create_status{SAI_STATUS_SUCCESS};
    EXPECT_CALL(mock_sai_next_hop_group_,
                create_next_hop_group_members(Eq(gSwitchId), Eq(1), ArrayEq(std::vector<uint32_t>{3}),
                                              AttrArrayArrayEq(std::vector<std::vector<sai_attribute_t>>{
                                                  GetSaiNextHopGroupMemberAttribute(kNexthopOid1, 1, kWcmpGroupOid1)}),
                                              Eq(SAI_BULK_OP_ERROR_MODE_STOP_ON_ERROR), _, _))
        .WillOnce(DoAll(SetArrayArgument<5>(return_oids.begin(), return_oids.end()),
                        SetArrayArgument<6>(exp_create_status.begin(), exp_create_status.end()),
                        Return(SAI_STATUS_SUCCESS)));
    Drain();
    std::string key = KeyGenerator::generateWcmpGroupKey(kWcmpGroupId1);
    auto *wcmp_group_entry_ptr = GetWcmpGroupEntry(kWcmpGroupId1);
    EXPECT_NE(nullptr, wcmp_group_entry_ptr);
    EXPECT_EQ(1, wcmp_group_entry_ptr->wcmp_group_members.size());
    VerifyWcmpGroupMemberEntry(kNexthopId1, 1, wcmp_group_entry_ptr->wcmp_group_members[0]);
    EXPECT_EQ(kWcmpGroupMemberOid1, wcmp_group_entry_ptr->wcmp_group_members[0]->member_oid);
    EXPECT_TRUE(p4_oid_mapper_->existsOID(SAI_OBJECT_TYPE_NEXT_HOP_GROUP, key));
    uint32_t ref_cnt;
    EXPECT_TRUE(p4_oid_mapper_->getRefCount(SAI_OBJECT_TYPE_NEXT_HOP, kNexthopKey1, &ref_cnt));
    EXPECT_EQ(1, ref_cnt);

    // Update WCMP group with exact same members, the same entry will be removed
    // and created again
    Enqueue(swss::KeyOpFieldsValuesTuple(kKeyPrefix + j.dump(), SET_COMMAND, attributes));
    return_oids = {kWcmpGroupMemberOid3};
    exp_create_status = {SAI_STATUS_SUCCESS};
    EXPECT_CALL(mock_sai_next_hop_group_,
                create_next_hop_group_members(Eq(gSwitchId), Eq(1), ArrayEq(std::vector<uint32_t>{3}),
                                              AttrArrayArrayEq(std::vector<std::vector<sai_attribute_t>>{
                                                  GetSaiNextHopGroupMemberAttribute(kNexthopOid1, 1, kWcmpGroupOid1)}),
                                              Eq(SAI_BULK_OP_ERROR_MODE_STOP_ON_ERROR), _, _))
        .WillOnce(DoAll(SetArrayArgument<5>(return_oids.begin(), return_oids.end()),
                        SetArrayArgument<6>(exp_create_status.begin(), exp_create_status.end()),
                        Return(SAI_STATUS_SUCCESS)));
    std::vector<sai_status_t> exp_remove_status{SAI_STATUS_SUCCESS};
    EXPECT_CALL(mock_sai_next_hop_group_,
                remove_next_hop_group_members(Eq(1), ArrayEq(std::vector<sai_object_id_t>{kWcmpGroupMemberOid1}),
                                              Eq(SAI_BULK_OP_ERROR_MODE_STOP_ON_ERROR), _))
        .WillOnce(
            DoAll(SetArrayArgument<3>(exp_remove_status.begin(), exp_remove_status.end()), Return(SAI_STATUS_SUCCESS)));
    Drain();
    wcmp_group_entry_ptr = GetWcmpGroupEntry(kWcmpGroupId1);
    EXPECT_NE(nullptr, wcmp_group_entry_ptr);
    EXPECT_EQ(1, wcmp_group_entry_ptr->wcmp_group_members.size());
    VerifyWcmpGroupMemberEntry(kNexthopId1, 1, wcmp_group_entry_ptr->wcmp_group_members[0]);
    EXPECT_EQ(kWcmpGroupMemberOid3, wcmp_group_entry_ptr->wcmp_group_members[0]->member_oid);
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
    return_oids = {kWcmpGroupMemberOid2};
    exp_create_status = {SAI_STATUS_SUCCESS};
    EXPECT_CALL(mock_sai_next_hop_group_,
                create_next_hop_group_members(Eq(gSwitchId), Eq(1), ArrayEq(std::vector<uint32_t>{3}),
                                              AttrArrayArrayEq(std::vector<std::vector<sai_attribute_t>>{
                                                  GetSaiNextHopGroupMemberAttribute(kNexthopOid2, 1, kWcmpGroupOid1)}),
                                              Eq(SAI_BULK_OP_ERROR_MODE_STOP_ON_ERROR), _, _))
        .WillOnce(DoAll(SetArrayArgument<5>(return_oids.begin(), return_oids.end()),
                        SetArrayArgument<6>(exp_create_status.begin(), exp_create_status.end()),
                        Return(SAI_STATUS_SUCCESS)));
    exp_remove_status = {SAI_STATUS_SUCCESS};
    EXPECT_CALL(mock_sai_next_hop_group_,
                remove_next_hop_group_members(Eq(1), ArrayEq(std::vector<sai_object_id_t>{kWcmpGroupMemberOid3}),
                                              Eq(SAI_BULK_OP_ERROR_MODE_STOP_ON_ERROR), _))
        .WillOnce(
            DoAll(SetArrayArgument<3>(exp_remove_status.begin(), exp_remove_status.end()), Return(SAI_STATUS_SUCCESS)));
    Drain();
    wcmp_group_entry_ptr = GetWcmpGroupEntry(kWcmpGroupId1);
    EXPECT_NE(nullptr, wcmp_group_entry_ptr);
    EXPECT_EQ(1, wcmp_group_entry_ptr->wcmp_group_members.size());
    VerifyWcmpGroupMemberEntry(kNexthopId2, 1, wcmp_group_entry_ptr->wcmp_group_members[0]);
    EXPECT_EQ(kWcmpGroupMemberOid2, wcmp_group_entry_ptr->wcmp_group_members[0]->member_oid);
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
    return_oids = {kWcmpGroupMemberOid4};
    exp_create_status = {SAI_STATUS_SUCCESS};
    EXPECT_CALL(mock_sai_next_hop_group_,
                create_next_hop_group_members(Eq(gSwitchId), Eq(1), ArrayEq(std::vector<uint32_t>{3}),
                                              AttrArrayArrayEq(std::vector<std::vector<sai_attribute_t>>{
                                                  GetSaiNextHopGroupMemberAttribute(kNexthopOid2, 2, kWcmpGroupOid1)}),
                                              Eq(SAI_BULK_OP_ERROR_MODE_STOP_ON_ERROR), _, _))
        .WillOnce(DoAll(SetArrayArgument<5>(return_oids.begin(), return_oids.end()),
                        SetArrayArgument<6>(exp_create_status.begin(), exp_create_status.end()),
                        Return(SAI_STATUS_SUCCESS)));
    exp_remove_status = {SAI_STATUS_SUCCESS};
    EXPECT_CALL(mock_sai_next_hop_group_,
                remove_next_hop_group_members(Eq(1), ArrayEq(std::vector<sai_object_id_t>{kWcmpGroupMemberOid2}),
                                              Eq(SAI_BULK_OP_ERROR_MODE_STOP_ON_ERROR), _))
        .WillOnce(
            DoAll(SetArrayArgument<3>(exp_remove_status.begin(), exp_remove_status.end()), Return(SAI_STATUS_SUCCESS)));
    Drain();
    wcmp_group_entry_ptr = GetWcmpGroupEntry(kWcmpGroupId1);
    EXPECT_NE(nullptr, wcmp_group_entry_ptr);
    EXPECT_EQ(1, wcmp_group_entry_ptr->wcmp_group_members.size());
    VerifyWcmpGroupMemberEntry(kNexthopId2, 2, wcmp_group_entry_ptr->wcmp_group_members[0]);
    EXPECT_EQ(kWcmpGroupMemberOid4, wcmp_group_entry_ptr->wcmp_group_members[0]->member_oid);
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
    Drain();
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
    EXPECT_CALL(mock_sai_next_hop_group_, remove_next_hop_group_member(Eq(kWcmpGroupMemberOid1)))
        .WillOnce(Return(SAI_STATUS_SUCCESS));
    // Prune next hops associated with port
    PruneNextHops(port_name);
    EXPECT_TRUE(VerifyWcmpGroupMemberInPortMap(app_db_entry.wcmp_group_members[0], true, 1));
    EXPECT_TRUE(app_db_entry.wcmp_group_members[0]->pruned);
}

TEST_F(WcmpManagerTest, PruneNextHopFailsWithNextHopRemovalFailure)
{
    // Add member with operationally up watch port
    std::string port_name = "Ethernet6";
    P4WcmpGroupEntry app_db_entry = AddWcmpGroupEntryWithWatchport(port_name, true);
    EXPECT_TRUE(VerifyWcmpGroupMemberInPortMap(app_db_entry.wcmp_group_members[0], true, 1));
    EXPECT_FALSE(app_db_entry.wcmp_group_members[0]->pruned);
    EXPECT_CALL(mock_sai_next_hop_group_, remove_next_hop_group_member(Eq(kWcmpGroupMemberOid1)))
        .WillOnce(Return(SAI_STATUS_FAILURE));
    // TODO: Expect critical state.
    // Prune next hops associated with port (fails)
    PruneNextHops(port_name);
    EXPECT_TRUE(VerifyWcmpGroupMemberInPortMap(app_db_entry.wcmp_group_members[0], true, 1));
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
    EXPECT_CALL(mock_sai_next_hop_group_,
                create_next_hop_group_member(_, Eq(gSwitchId), Eq(3),
                                             Truly(std::bind(MatchSaiNextHopGroupMemberAttribute, kNexthopOid1, 2,
                                                             kWcmpGroupOid1, std::placeholders::_1))))
        .WillOnce(DoAll(SetArgPointee<0>(kWcmpGroupMemberOid1), Return(SAI_STATUS_SUCCESS)));

    // Restore next hops associated with port
    RestorePrunedNextHops(port_name);
    EXPECT_TRUE(VerifyWcmpGroupMemberInPortMap(app_db_entry.wcmp_group_members[0], true, 1));
    EXPECT_FALSE(app_db_entry.wcmp_group_members[0]->pruned);
}

TEST_F(WcmpManagerTest, RestorePrunedNextHopFailsWithNoOidMappingForWcmpGroup)
{
    // Add member with operationally down watch port. Since associated watchport
    // is operationally down, member will not be created in SAI but will be
    // directly added to the pruned set of WCMP group members.
    std::string port_name = "Ethernet1";
    P4WcmpGroupEntry app_db_entry = AddWcmpGroupEntryWithWatchport(port_name);
    EXPECT_TRUE(VerifyWcmpGroupMemberInPortMap(app_db_entry.wcmp_group_members[0], true, 1));
    EXPECT_TRUE(app_db_entry.wcmp_group_members[0]->pruned);
    p4_oid_mapper_->eraseOID(SAI_OBJECT_TYPE_NEXT_HOP_GROUP, KeyGenerator::generateWcmpGroupKey(kWcmpGroupId1));
    // TODO: Expect critical state.
    RestorePrunedNextHops(port_name);
    EXPECT_TRUE(VerifyWcmpGroupMemberInPortMap(app_db_entry.wcmp_group_members[0], true, 1));
    EXPECT_TRUE(app_db_entry.wcmp_group_members[0]->pruned);
}

TEST_F(WcmpManagerTest, RestorePrunedNextHopFailsWithNextHopCreationFailure)
{
    // Add member with operationally down watch port. Since associated watchport
    // is operationally down, member will not be created in SAI but will be
    // directly added to the pruned set of WCMP group members.
    std::string port_name = "Ethernet1";
    P4WcmpGroupEntry app_db_entry = AddWcmpGroupEntryWithWatchport(port_name);
    EXPECT_TRUE(VerifyWcmpGroupMemberInPortMap(app_db_entry.wcmp_group_members[0], true, 1));
    EXPECT_TRUE(app_db_entry.wcmp_group_members[0]->pruned);
    EXPECT_CALL(mock_sai_next_hop_group_,
                create_next_hop_group_member(_, Eq(gSwitchId), Eq(3),
                                             Truly(std::bind(MatchSaiNextHopGroupMemberAttribute, kNexthopOid1, 2,
                                                             kWcmpGroupOid1, std::placeholders::_1))))
        .WillOnce(DoAll(SetArgPointee<0>(kWcmpGroupMemberOid1), Return(SAI_STATUS_FAILURE)));
    // TODO: Expect critical state.
    RestorePrunedNextHops(port_name);
    EXPECT_TRUE(VerifyWcmpGroupMemberInPortMap(app_db_entry.wcmp_group_members[0], true, 1));
    EXPECT_TRUE(app_db_entry.wcmp_group_members[0]->pruned);
}

TEST_F(WcmpManagerTest, CreateGroupWithWatchportFailsWithNextHopCreationFailure)
{
    // Add member with operationally up watch port
    // Create WCMP group with members kNexthopId1 and kNexthopId2 (fails)
    std::string port_name = "Ethernet6";
    P4WcmpGroupEntry app_db_entry = {.wcmp_group_id = kWcmpGroupId1, .wcmp_group_members = {}};
    std::shared_ptr<P4WcmpGroupMemberEntry> gm1 =
        createWcmpGroupMemberEntryWithWatchport(kNexthopId1, 1, port_name, kWcmpGroupId1, kNexthopOid1);
    app_db_entry.wcmp_group_members.push_back(gm1);
    std::shared_ptr<P4WcmpGroupMemberEntry> gm2 =
        createWcmpGroupMemberEntryWithWatchport(kNexthopId2, 1, port_name, kWcmpGroupId1, kNexthopOid2);
    app_db_entry.wcmp_group_members.push_back(gm2);
    EXPECT_CALL(mock_sai_next_hop_group_,
                create_next_hop_group(_, Eq(gSwitchId), Eq(1),
                                      Truly(std::bind(MatchSaiNextHopGroupAttribute, std::placeholders::_1))))
        .WillOnce(DoAll(SetArgPointee<0>(kWcmpGroupOid1), Return(SAI_STATUS_SUCCESS)));
    std::vector<sai_object_id_t> return_oids{kWcmpGroupMemberOid1, SAI_NULL_OBJECT_ID};
    std::vector<sai_status_t> exp_create_status{SAI_STATUS_SUCCESS, SAI_STATUS_FAILURE};
    EXPECT_CALL(mock_sai_next_hop_group_,
                create_next_hop_group_members(Eq(gSwitchId), Eq(2), ArrayEq(std::vector<uint32_t>{3, 3}),
                                              AttrArrayArrayEq(std::vector<std::vector<sai_attribute_t>>{
                                                  GetSaiNextHopGroupMemberAttribute(kNexthopOid1, 1, kWcmpGroupOid1),
                                                  GetSaiNextHopGroupMemberAttribute(kNexthopOid2, 1, kWcmpGroupOid1)}),
                                              Eq(SAI_BULK_OP_ERROR_MODE_STOP_ON_ERROR), _, _))
        .WillOnce(DoAll(SetArrayArgument<5>(return_oids.begin(), return_oids.end()),
                        SetArrayArgument<6>(exp_create_status.begin(), exp_create_status.end()),
                        Return(SAI_STATUS_FAILURE)));
    // Clean up created members
    std::vector<sai_status_t> exp_remove_status{SAI_STATUS_SUCCESS};
    EXPECT_CALL(mock_sai_next_hop_group_,
                remove_next_hop_group_members(Eq(1), ArrayEq(std::vector<sai_object_id_t>{kWcmpGroupMemberOid1}),
                                              Eq(SAI_BULK_OP_ERROR_MODE_STOP_ON_ERROR), _))
        .WillOnce(
            DoAll(SetArrayArgument<3>(exp_remove_status.begin(), exp_remove_status.end()), Return(SAI_STATUS_SUCCESS)));
    EXPECT_CALL(mock_sai_next_hop_group_, remove_next_hop_group(Eq(kWcmpGroupOid1)))
        .WillOnce(Return(SAI_STATUS_SUCCESS));
    EXPECT_EQ(StatusCode::SWSS_RC_UNKNOWN, ProcessAddRequest(&app_db_entry));
    EXPECT_TRUE(VerifyWcmpGroupMemberInPortMap(gm1, false, 0));
    EXPECT_TRUE(VerifyWcmpGroupMemberInPortMap(gm2, false, 0));
    EXPECT_FALSE(gm1->pruned);
    EXPECT_FALSE(gm2->pruned);
}

TEST_F(WcmpManagerTest, RemoveWcmpGroupAfterPruningSucceeds)
{
    // Add member with operationally up watch port
    std::string port_name = "Ethernet6";
    P4WcmpGroupEntry app_db_entry = AddWcmpGroupEntryWithWatchport(port_name, true);
    EXPECT_TRUE(VerifyWcmpGroupMemberInPortMap(app_db_entry.wcmp_group_members[0], true, 1));
    EXPECT_FALSE(app_db_entry.wcmp_group_members[0]->pruned);
    EXPECT_CALL(mock_sai_next_hop_group_, remove_next_hop_group_member(Eq(kWcmpGroupMemberOid1)))
        .WillOnce(Return(SAI_STATUS_SUCCESS));
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
    EXPECT_CALL(mock_sai_next_hop_group_,
                create_next_hop_group_member(_, Eq(gSwitchId), Eq(3),
                                             Truly(std::bind(MatchSaiNextHopGroupMemberAttribute, kNexthopOid1, 2,
                                                             kWcmpGroupOid1, std::placeholders::_1))))
        .WillOnce(DoAll(SetArgPointee<0>(kWcmpGroupMemberOid1), Return(SAI_STATUS_SUCCESS)));
    RestorePrunedNextHops(port_name);
    EXPECT_TRUE(VerifyWcmpGroupMemberInPortMap(app_db_entry.wcmp_group_members[0], true, 1));
    EXPECT_FALSE(app_db_entry.wcmp_group_members[0]->pruned);

    // Verify that next hop reference count remains the same after restore.
    EXPECT_TRUE(p4_oid_mapper_->getRefCount(SAI_OBJECT_TYPE_NEXT_HOP, kNexthopKey1, &ref_cnt));
    EXPECT_EQ(1, ref_cnt);

    // Remove Wcmp group.
    std::vector<sai_status_t> exp_remove_status{SAI_STATUS_SUCCESS};
    EXPECT_CALL(mock_sai_next_hop_group_,
                remove_next_hop_group_members(Eq(1), ArrayEq(std::vector<sai_object_id_t>{kWcmpGroupMemberOid1}),
                                              Eq(SAI_BULK_OP_ERROR_MODE_STOP_ON_ERROR), _))
        .WillOnce(
            DoAll(SetArrayArgument<3>(exp_remove_status.begin(), exp_remove_status.end()), Return(SAI_STATUS_SUCCESS)));
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
    EXPECT_CALL(mock_sai_next_hop_group_, remove_next_hop_group_member(Eq(kWcmpGroupMemberOid1)))
        .WillOnce(Return(SAI_STATUS_SUCCESS));
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
    std::vector<sai_object_id_t> return_oids{kWcmpGroupMemberOid2};
    std::vector<sai_status_t> exp_create_status{SAI_STATUS_SUCCESS};
    EXPECT_CALL(mock_sai_next_hop_group_,
                create_next_hop_group_members(Eq(gSwitchId), Eq(1), ArrayEq(std::vector<uint32_t>{3}),
                                              AttrArrayArrayEq(std::vector<std::vector<sai_attribute_t>>{
                                                  GetSaiNextHopGroupMemberAttribute(kNexthopOid2, 1, kWcmpGroupOid1)}),
                                              Eq(SAI_BULK_OP_ERROR_MODE_STOP_ON_ERROR), _, _))
        .WillOnce(DoAll(SetArrayArgument<5>(return_oids.begin(), return_oids.end()),
                        SetArrayArgument<6>(exp_create_status.begin(), exp_create_status.end()),
                        Return(SAI_STATUS_SUCCESS)));
    std::vector<sai_status_t> exp_remove_status{SAI_STATUS_SUCCESS};
    EXPECT_CALL(mock_sai_next_hop_group_,
                remove_next_hop_group_members(Eq(1), ArrayEq(std::vector<sai_object_id_t>{kWcmpGroupMemberOid1}),
                                              Eq(SAI_BULK_OP_ERROR_MODE_STOP_ON_ERROR), _))
        .WillOnce(
            DoAll(SetArrayArgument<3>(exp_remove_status.begin(), exp_remove_status.end()), Return(SAI_STATUS_SUCCESS)));
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

    // Update WCMP group to remove kNexthopId1 and add kNexthopId2. No SAI calls
    // are expected as the associated watch port is operationally down.
    P4WcmpGroupEntry updated_app_db_entry;
    updated_app_db_entry.wcmp_group_id = kWcmpGroupId1;
    std::shared_ptr<P4WcmpGroupMemberEntry> updated_gm =
        createWcmpGroupMemberEntryWithWatchport(kNexthopId2, 1, port_name, kWcmpGroupId1, kNexthopOid2);
    updated_app_db_entry.wcmp_group_members.push_back(updated_gm);
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
    std::vector<sai_status_t> exp_remove_status{SAI_STATUS_SUCCESS};
    EXPECT_CALL(mock_sai_next_hop_group_,
                remove_next_hop_group_members(Eq(1), ArrayEq(std::vector<sai_object_id_t>{kWcmpGroupMemberOid1}),
                                              Eq(SAI_BULK_OP_ERROR_MODE_STOP_ON_ERROR), _))
        .WillOnce(
            DoAll(SetArrayArgument<3>(exp_remove_status.begin(), exp_remove_status.end()), Return(SAI_STATUS_SUCCESS)));
    std::vector<sai_object_id_t> return_oids{kWcmpGroupMemberOid1};
    std::vector<sai_status_t> exp_create_status{SAI_STATUS_SUCCESS};
    EXPECT_CALL(mock_sai_next_hop_group_,
                create_next_hop_group_members(Eq(gSwitchId), Eq(1), ArrayEq(std::vector<uint32_t>{3}),
                                              AttrArrayArrayEq(std::vector<std::vector<sai_attribute_t>>{
                                                  GetSaiNextHopGroupMemberAttribute(kNexthopOid1, 10, kWcmpGroupOid1)}),
                                              Eq(SAI_BULK_OP_ERROR_MODE_STOP_ON_ERROR), _, _))
        .WillOnce(DoAll(SetArrayArgument<5>(return_oids.begin(), return_oids.end()),
                        SetArrayArgument<6>(exp_create_status.begin(), exp_create_status.end()),
                        Return(SAI_STATUS_SUCCESS)));
    EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS, ProcessUpdateRequest(&updated_app_db_entry));
    EXPECT_TRUE(VerifyWcmpGroupMemberInPortMap(app_db_entry.wcmp_group_members[0], false, 1));
    EXPECT_TRUE(VerifyWcmpGroupMemberInPortMap(updated_app_db_entry.wcmp_group_members[0], true, 1));
    EXPECT_FALSE(updated_app_db_entry.wcmp_group_members[0]->pruned);

    // Prune members associated with port.
    EXPECT_CALL(mock_sai_next_hop_group_, remove_next_hop_group_member(Eq(kWcmpGroupMemberOid1)))
        .WillOnce(Return(SAI_STATUS_SUCCESS));
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
    EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS, ProcessUpdateRequest(&updated_app_db_entry));
    EXPECT_TRUE(VerifyWcmpGroupMemberInPortMap(app_db_entry.wcmp_group_members[0], false, 1));
    EXPECT_TRUE(VerifyWcmpGroupMemberInPortMap(updated_app_db_entry.wcmp_group_members[0], true, 1));
    EXPECT_TRUE(updated_app_db_entry.wcmp_group_members[0]->pruned);

    // Restore members associated with port.
    // Verify that the weight of the restored member is updated.
    EXPECT_CALL(mock_sai_next_hop_group_,
                create_next_hop_group_member(_, Eq(gSwitchId), Eq(3),
                                             Truly(std::bind(MatchSaiNextHopGroupMemberAttribute, kNexthopOid1, 10,
                                                             kWcmpGroupOid1, std::placeholders::_1))))
        .WillOnce(DoAll(SetArgPointee<0>(kWcmpGroupMemberOid1), Return(SAI_STATUS_SUCCESS)));
    RestorePrunedNextHops(port_name);
    EXPECT_TRUE(VerifyWcmpGroupMemberInPortMap(updated_app_db_entry.wcmp_group_members[0], true, 1));
    EXPECT_FALSE(updated_app_db_entry.wcmp_group_members[0]->pruned);
}

TEST_F(WcmpManagerTest, UpdateWcmpGroupWithOperationallyUpWatchportMemberFailsWithMemberRemovalFailure)
{
    // Add member with operationally up watch port
    std::string port_name = "Ethernet6";
    P4WcmpGroupEntry app_db_entry = AddWcmpGroupEntryWithWatchport(port_name, true);
    EXPECT_TRUE(VerifyWcmpGroupMemberInPortMap(app_db_entry.wcmp_group_members[0], true, 1));
    EXPECT_FALSE(app_db_entry.wcmp_group_members[0]->pruned);

    // Update WCMP group to remove kNexthopId1(fails) and add kNexthopId2
    P4WcmpGroupEntry updated_app_db_entry;
    updated_app_db_entry.wcmp_group_id = kWcmpGroupId1;
    std::shared_ptr<P4WcmpGroupMemberEntry> updated_gm2 =
        createWcmpGroupMemberEntryWithWatchport(kNexthopId2, 10, port_name, kWcmpGroupId1, kNexthopOid2);
    std::shared_ptr<P4WcmpGroupMemberEntry> updated_gm1 =
        createWcmpGroupMemberEntryWithWatchport(kNexthopId1, 1, port_name, kWcmpGroupId1, kNexthopOid1);
    updated_app_db_entry.wcmp_group_members.push_back(updated_gm1);
    updated_app_db_entry.wcmp_group_members.push_back(updated_gm2);
    std::vector<sai_status_t> exp_remove_status{SAI_STATUS_SUCCESS};
    EXPECT_CALL(mock_sai_next_hop_group_,
                remove_next_hop_group_members(Eq(1), ArrayEq(std::vector<sai_object_id_t>{kWcmpGroupMemberOid1}),
                                              Eq(SAI_BULK_OP_ERROR_MODE_STOP_ON_ERROR), _))
        .WillOnce(
            DoAll(SetArrayArgument<3>(exp_remove_status.begin(), exp_remove_status.end()), Return(SAI_STATUS_SUCCESS)));
    std::vector<sai_object_id_t> return_oids_4{kWcmpGroupMemberOid4};
    std::vector<sai_object_id_t> return_oids_null{SAI_NULL_OBJECT_ID};
    std::vector<sai_status_t> exp_create_status{SAI_STATUS_SUCCESS};
    std::vector<sai_status_t> exp_create_status_fail{SAI_STATUS_INSUFFICIENT_RESOURCES};
    EXPECT_CALL(mock_sai_next_hop_group_,
                create_next_hop_group_members(Eq(gSwitchId), Eq(1), ArrayEq(std::vector<uint32_t>{3}),
                                              AttrArrayArrayEq(std::vector<std::vector<sai_attribute_t>>{
                                                  GetSaiNextHopGroupMemberAttribute(kNexthopOid1, 1, kWcmpGroupOid1)}),
                                              Eq(SAI_BULK_OP_ERROR_MODE_STOP_ON_ERROR), _, _))
        .WillOnce(DoAll(SetArrayArgument<5>(return_oids_4.begin(), return_oids_4.end()),
                        SetArrayArgument<6>(exp_create_status.begin(), exp_create_status.end()),
                        Return(SAI_STATUS_SUCCESS)));
    EXPECT_CALL(mock_sai_next_hop_group_,
                create_next_hop_group_members(Eq(gSwitchId), Eq(1), ArrayEq(std::vector<uint32_t>{3}),
                                              AttrArrayArrayEq(std::vector<std::vector<sai_attribute_t>>{
                                                  GetSaiNextHopGroupMemberAttribute(kNexthopOid2, 10, kWcmpGroupOid1)}),
                                              Eq(SAI_BULK_OP_ERROR_MODE_STOP_ON_ERROR), _, _))
        .WillOnce(DoAll(SetArrayArgument<5>(return_oids_null.begin(), return_oids_null.end()),
                        SetArrayArgument<6>(exp_create_status_fail.begin(), exp_create_status_fail.end()),
                        Return(SAI_STATUS_INSUFFICIENT_RESOURCES)));
    // Clean up created member-succeeds
    std::vector<sai_object_id_t> return_oids_1{kWcmpGroupMemberOid1};
    EXPECT_CALL(mock_sai_next_hop_group_,
                create_next_hop_group_members(Eq(gSwitchId), Eq(1), ArrayEq(std::vector<uint32_t>{3}),
                                              AttrArrayArrayEq(std::vector<std::vector<sai_attribute_t>>{
                                                  GetSaiNextHopGroupMemberAttribute(kNexthopOid1, 2, kWcmpGroupOid1)}),
                                              Eq(SAI_BULK_OP_ERROR_MODE_STOP_ON_ERROR), _, _))
        .WillOnce(DoAll(SetArrayArgument<5>(return_oids_1.begin(), return_oids_1.end()),
                        SetArrayArgument<6>(exp_create_status.begin(), exp_create_status.end()),
                        Return(SAI_STATUS_SUCCESS)));
    EXPECT_CALL(mock_sai_next_hop_group_,
                remove_next_hop_group_members(Eq(1), ArrayEq(std::vector<sai_object_id_t>{kWcmpGroupMemberOid4}),
                                              Eq(SAI_BULK_OP_ERROR_MODE_STOP_ON_ERROR), _))
        .WillOnce(
            DoAll(SetArrayArgument<3>(exp_remove_status.begin(), exp_remove_status.end()), Return(SAI_STATUS_SUCCESS)));
    EXPECT_EQ(StatusCode::SWSS_RC_UNKNOWN, ProcessUpdateRequest(&updated_app_db_entry));
    EXPECT_TRUE(VerifyWcmpGroupMemberInPortMap(app_db_entry.wcmp_group_members[0], true, 1));
    EXPECT_TRUE(VerifyWcmpGroupMemberInPortMap(updated_gm2, false, 1));
    EXPECT_FALSE(app_db_entry.wcmp_group_members[0]->pruned);
    EXPECT_FALSE(updated_gm2->pruned);

    // Update again, this time clean up fails
    EXPECT_CALL(mock_sai_next_hop_group_,
                remove_next_hop_group_members(Eq(1), ArrayEq(std::vector<sai_object_id_t>{kWcmpGroupMemberOid1}),
                                              Eq(SAI_BULK_OP_ERROR_MODE_STOP_ON_ERROR), _))
        .WillOnce(
            DoAll(SetArrayArgument<3>(exp_remove_status.begin(), exp_remove_status.end()), Return(SAI_STATUS_SUCCESS)));
    EXPECT_CALL(mock_sai_next_hop_group_,
                create_next_hop_group_members(Eq(gSwitchId), Eq(1), ArrayEq(std::vector<uint32_t>{3}),
                                              AttrArrayArrayEq(std::vector<std::vector<sai_attribute_t>>{
                                                  GetSaiNextHopGroupMemberAttribute(kNexthopOid1, 1, kWcmpGroupOid1)}),
                                              Eq(SAI_BULK_OP_ERROR_MODE_STOP_ON_ERROR), _, _))
        .WillOnce(DoAll(SetArrayArgument<5>(return_oids_4.begin(), return_oids_4.end()),
                        SetArrayArgument<6>(exp_create_status.begin(), exp_create_status.end()),
                        Return(SAI_STATUS_SUCCESS)));
    EXPECT_CALL(mock_sai_next_hop_group_,
                create_next_hop_group_members(Eq(gSwitchId), Eq(1), ArrayEq(std::vector<uint32_t>{3}),
                                              AttrArrayArrayEq(std::vector<std::vector<sai_attribute_t>>{
                                                  GetSaiNextHopGroupMemberAttribute(kNexthopOid2, 10, kWcmpGroupOid1)}),
                                              Eq(SAI_BULK_OP_ERROR_MODE_STOP_ON_ERROR), _, _))
        .WillOnce(DoAll(SetArrayArgument<5>(return_oids_null.begin(), return_oids_null.end()),
                        SetArrayArgument<6>(exp_create_status_fail.begin(), exp_create_status_fail.end()),
                        Return(SAI_STATUS_INSUFFICIENT_RESOURCES)));
    // Clean up created member(fails)
    EXPECT_CALL(mock_sai_next_hop_group_,
                remove_next_hop_group_members(Eq(1), ArrayEq(std::vector<sai_object_id_t>{kWcmpGroupMemberOid4}),
                                              Eq(SAI_BULK_OP_ERROR_MODE_STOP_ON_ERROR), _))
        .WillOnce(
            DoAll(SetArrayArgument<3>(exp_remove_status.begin(), exp_remove_status.end()), Return(SAI_STATUS_SUCCESS)));
    EXPECT_CALL(mock_sai_next_hop_group_,
                create_next_hop_group_members(Eq(gSwitchId), Eq(1), ArrayEq(std::vector<uint32_t>{3}),
                                              AttrArrayArrayEq(std::vector<std::vector<sai_attribute_t>>{
                                                  GetSaiNextHopGroupMemberAttribute(kNexthopOid1, 2, kWcmpGroupOid1)}),
                                              Eq(SAI_BULK_OP_ERROR_MODE_STOP_ON_ERROR), _, _))
        .WillOnce(DoAll(SetArrayArgument<5>(return_oids_null.begin(), return_oids_null.end()),
                        SetArrayArgument<6>(exp_create_status_fail.begin(), exp_create_status_fail.end()),
                        Return(SAI_STATUS_INSUFFICIENT_RESOURCES)));
    // TODO: Expect critical state.
    EXPECT_EQ("Fail to create wcmp group member: 'ju1u32m2.atl11:qe-3/7'",
              ProcessUpdateRequest(&updated_app_db_entry).message());
    EXPECT_TRUE(VerifyWcmpGroupMemberInPortMap(app_db_entry.wcmp_group_members[0], false, 0));
    EXPECT_TRUE(VerifyWcmpGroupMemberInPortMap(updated_gm2, false, 0));
    EXPECT_FALSE(app_db_entry.wcmp_group_members[0]->pruned);
    EXPECT_FALSE(updated_gm2->pruned);
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
                       "STATUS_DOWN\"}]";
    EXPECT_CALL(mock_sai_next_hop_group_, remove_next_hop_group_member(Eq(kWcmpGroupMemberOid1)))
        .WillOnce(Return(SAI_STATUS_SUCCESS));
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
                       "STATUS_UP\"}]";
    EXPECT_CALL(mock_sai_next_hop_group_,
                create_next_hop_group_member(_, Eq(gSwitchId), Eq(3),
                                             Truly(std::bind(MatchSaiNextHopGroupMemberAttribute, kNexthopOid1, 2,
                                                             kWcmpGroupOid1, std::placeholders::_1))))
        .WillOnce(DoAll(SetArgPointee<0>(kWcmpGroupMemberOid1), Return(SAI_STATUS_SUCCESS)));
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
                       "STATUS_DOWN\"}]";
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
    table.set("SAI_OBJECT_TYPE_NEXT_HOP_GROUP:oid:0xa",
              std::vector<swss::FieldValueTuple>{swss::FieldValueTuple{
                  "SAI_NEXT_HOP_GROUP_ATTR_TYPE", "SAI_NEXT_HOP_GROUP_TYPE_DYNAMIC_UNORDERED_ECMP"}});
    table.set("SAI_OBJECT_TYPE_NEXT_HOP_GROUP_MEMBER:oid:0xb",
              std::vector<swss::FieldValueTuple>{
                  swss::FieldValueTuple{"SAI_NEXT_HOP_GROUP_MEMBER_ATTR_NEXT_HOP_GROUP_ID", "oid:0xa"},
                  swss::FieldValueTuple{"SAI_NEXT_HOP_GROUP_MEMBER_ATTR_NEXT_HOP_ID", "oid:0x1"},
                  swss::FieldValueTuple{"SAI_NEXT_HOP_GROUP_MEMBER_ATTR_WEIGHT", "2"}});

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

    // Verification should fail if member OID mapper mismatches.
    p4_oid_mapper_->eraseOID(SAI_OBJECT_TYPE_NEXT_HOP_GROUP_MEMBER,
                             kWcmpGroupKey1 + kTableKeyDelimiter + sai_serialize_object_id(kWcmpGroupMemberOid1));
    EXPECT_FALSE(VerifyState(db_key, attributes).empty());
    p4_oid_mapper_->setOID(SAI_OBJECT_TYPE_NEXT_HOP_GROUP_MEMBER,
                           kWcmpGroupKey1 + kTableKeyDelimiter + sai_serialize_object_id(kWcmpGroupMemberOid1),
                           kWcmpGroupMemberOid1);
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
    table.set("SAI_OBJECT_TYPE_NEXT_HOP_GROUP:oid:0xa",
              std::vector<swss::FieldValueTuple>{swss::FieldValueTuple{
                  "SAI_NEXT_HOP_GROUP_ATTR_TYPE", "SAI_NEXT_HOP_GROUP_TYPE_DYNAMIC_UNORDERED_ECMP"}});
    table.set("SAI_OBJECT_TYPE_NEXT_HOP_GROUP_MEMBER:oid:0xb",
              std::vector<swss::FieldValueTuple>{
                  swss::FieldValueTuple{"SAI_NEXT_HOP_GROUP_MEMBER_ATTR_NEXT_HOP_GROUP_ID", "oid:0xa"},
                  swss::FieldValueTuple{"SAI_NEXT_HOP_GROUP_MEMBER_ATTR_NEXT_HOP_ID", "oid:0x1"},
                  swss::FieldValueTuple{"SAI_NEXT_HOP_GROUP_MEMBER_ATTR_WEIGHT", "2"}});

    // Verification should succeed with correct ASIC DB values.
    EXPECT_EQ(VerifyState(db_key, attributes), "");

    // Verification should fail if group values mismatch.
    table.set("SAI_OBJECT_TYPE_NEXT_HOP_GROUP:oid:0xa",
              std::vector<swss::FieldValueTuple>{swss::FieldValueTuple{"SAI_NEXT_HOP_GROUP_ATTR_TYPE", "invalid"}});
    EXPECT_FALSE(VerifyState(db_key, attributes).empty());

    // Verification should fail if group table is missing.
    table.del("SAI_OBJECT_TYPE_NEXT_HOP_GROUP:oid:0xa");
    EXPECT_FALSE(VerifyState(db_key, attributes).empty());
    table.set("SAI_OBJECT_TYPE_NEXT_HOP_GROUP:oid:0xa",
              std::vector<swss::FieldValueTuple>{swss::FieldValueTuple{
                  "SAI_NEXT_HOP_GROUP_ATTR_TYPE", "SAI_NEXT_HOP_GROUP_TYPE_DYNAMIC_UNORDERED_ECMP"}});

    // Verification should fail if member values mismatch.
    table.set("SAI_OBJECT_TYPE_NEXT_HOP_GROUP_MEMBER:oid:0xb",
              std::vector<swss::FieldValueTuple>{swss::FieldValueTuple{"SAI_NEXT_HOP_GROUP_MEMBER_ATTR_WEIGHT", "1"}});
    EXPECT_FALSE(VerifyState(db_key, attributes).empty());

    // Verification should fail if member table is missing.
    table.del("SAI_OBJECT_TYPE_NEXT_HOP_GROUP_MEMBER:oid:0xb");
    EXPECT_FALSE(VerifyState(db_key, attributes).empty());
    table.set("SAI_OBJECT_TYPE_NEXT_HOP_GROUP_MEMBER:oid:0xb",
              std::vector<swss::FieldValueTuple>{
                  swss::FieldValueTuple{"SAI_NEXT_HOP_GROUP_MEMBER_ATTR_NEXT_HOP_GROUP_ID", "oid:0xa"},
                  swss::FieldValueTuple{"SAI_NEXT_HOP_GROUP_MEMBER_ATTR_NEXT_HOP_ID", "oid:0x1"},
                  swss::FieldValueTuple{"SAI_NEXT_HOP_GROUP_MEMBER_ATTR_WEIGHT", "2"}});
}

} // namespace test
} // namespace p4orch
