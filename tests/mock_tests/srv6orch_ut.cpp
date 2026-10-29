#define private public
#include "directory.h"
#undef private
#define protected public
#include "orch.h"
#undef protected
#define private public
#include "neighorch.h"
#include "srv6orch.h"
#undef private

#include "mock_orch_test.h"
#include "mock_orchagent_main.h"
#include "mock_sai_api.h"
#include "ut_helper.h"

#include <gtest/gtest.h>
#include <deque>

using namespace std;
using namespace swss;

EXTERN_MOCK_FNS

namespace srv6orch_test
{

DEFINE_SAI_GENERIC_API_MOCK(tunnel, tunnel);
DEFINE_SAI_API_MOCK(srv6, my_sid);

// The legacy entry mock covers create/remove but not the singular setter.
static size_t my_sid_set_calls;
static sai_status_t countMySidSet(const sai_my_sid_entry_t*, const sai_attribute_t*)
{
    ++my_sid_set_calls;
    return SAI_STATUS_SUCCESS;
}

using ::testing::_;
using ::testing::AtLeast;
using namespace mock_orch_test;

class Srv6OrchMySidTest : public MockOrchTest
{
protected:
    void PostSetUp() override
    {
        INIT_SAI_API_MOCK(tunnel);
        INIT_SAI_API_MOCK(srv6);
        MockSaiApis();
        my_sid_set_calls = 0;
        sai_srv6_api->set_my_sid_entry_attribute = countMySidSet;
    }

    void PreTearDown() override
    {
        RestoreSaiApis();
        DEINIT_SAI_API_MOCK(srv6);
        DEINIT_SAI_API_MOCK(tunnel);
    }

    void addLocatorConfig(const string& locator_name)
    {
        Table locator_table(m_config_db.get(), CFG_SRV6_MY_LOCATOR_TABLE_NAME);
        vector<FieldValueTuple> fvs = {
            {"block_len", "32"},
            {"node_len", "16"},
            {"func_len", "16"},
            {"arg_len", "0"}
        };
        locator_table.set(locator_name, fvs);
    }

    void runCfgMySidTask(const string& key, const vector<FieldValueTuple>& fvs, bool is_set = true)
    {
        auto* executor = static_cast<Orch*>(gSrv6Orch)->getExecutor(CFG_SRV6_MY_SID_TABLE_NAME);
        auto* consumer = dynamic_cast<Consumer*>(executor);
        ASSERT_NE(consumer, nullptr);
        deque<KeyOpFieldsValuesTuple> entries;
        entries.push_back({key, is_set ? SET_COMMAND : DEL_COMMAND, fvs});
        consumer->addToSync(entries);
        static_cast<Orch*>(gSrv6Orch)->doTask(*consumer);
    }

    void runAppMySidTask(const string& key, const string& action, const string& vrf,
                        const string& adj, bool is_set = true)
    {
        auto* executor = static_cast<Orch*>(gSrv6Orch)->getExecutor(APP_SRV6_MY_SID_TABLE_NAME);
        auto* consumer = dynamic_cast<Consumer*>(executor);
        ASSERT_NE(consumer, nullptr);
        vector<FieldValueTuple> fvs = {{"action", action}};
        if (!vrf.empty())
            fvs.push_back({"vrf", vrf});
        if (!adj.empty())
            fvs.push_back({"adj", adj});
        deque<KeyOpFieldsValuesTuple> entries;
        entries.push_back({key, is_set ? SET_COMMAND : DEL_COMMAND, fvs});
        consumer->addToSync(entries);
        static_cast<Orch*>(gSrv6Orch)->doTask(*consumer);
    }

    void resolveNeighbor(const string& adj)
    {
        NextHopKey next_hop(adj);
        gNeighOrch->m_syncdNextHops[next_hop] = {0x1234, 0, 0};
        NeighborUpdate update{next_hop, MacAddress("00:11:22:33:44:55"), true};
        gSrv6Orch->updateNeighbor(update);
    }
};

TEST_F(Srv6OrchMySidTest, MySidEntryCreation_WithDecapDscpMode)
{
    ASSERT_NE(gSrv6Orch, nullptr);

    const string locator = "loc1";
    const string my_sid_prefix = "fc00:0:1:1::/64";
    const string cfg_key = locator + "|" + my_sid_prefix;
    const string app_key = "32:16:16:0:fc00:0:1:1::";

    addLocatorConfig(locator);

    EXPECT_CALL(*mock_sai_tunnel_api, create_tunnel(_, _, _, _)).Times(AtLeast(1));

    runCfgMySidTask(cfg_key, {{"decap_dscp_mode", "uniform"}});
    runAppMySidTask(app_key, "un", "default", "");
}

TEST_F(Srv6OrchMySidTest, MySidEntryCreation_WithoutDecapDscpMode)
{
    ASSERT_NE(gSrv6Orch, nullptr);

    const string locator = "loc1";
    const string my_sid_prefix = "fc00:0:1:1::/64";
    const string cfg_key = locator + "|" + my_sid_prefix;
    const string app_key = "32:16:16:0:fc00:0:1:1::";

    addLocatorConfig(locator);

    EXPECT_CALL(*mock_sai_tunnel_api, create_tunnel(_, _, _, _)).Times(0);

    runCfgMySidTask(cfg_key, {});
    runAppMySidTask(app_key, "un", "default", "");
}

TEST_F(Srv6OrchMySidTest, PendingDeleteDoesNotCreateOnNeighborAdd)
{
    const string key = "32:16:16:0:fc00:0:1:1::";
    const string adj = "2001:db8::1@Ethernet0";
    EXPECT_CALL(*mock_sai_srv6_api, create_my_sid_entry(_, _, _)).Times(0);
    EXPECT_CALL(*mock_sai_srv6_api, remove_my_sid_entry(_)).Times(0);

    runAppMySidTask(key, "ua", "", adj);
    ASSERT_EQ(gSrv6Orch->m_pendingSRv6MySIDEntries.size(), 1u);
    runAppMySidTask(key, "", "", "", false);
    EXPECT_TRUE(gSrv6Orch->m_pendingSRv6MySIDEntries.empty());
    // The second DEL must be a successful no-op, with no SAI remove.
    runAppMySidTask(key, "", "", "", false);
    resolveNeighbor(adj);
    EXPECT_FALSE(gSrv6Orch->mySidExists(key));
    EXPECT_EQ(gNeighOrch->getNextHopRefCount(NextHopKey(adj)), 0);
}

TEST_F(Srv6OrchMySidTest, PendingRequestCreatesOnNeighborAdd)
{
    const string key = "32:16:16:0:fc00:0:1:1::";
    const string adj = "2001:db8::1@Ethernet0";
    EXPECT_CALL(*mock_sai_srv6_api, create_my_sid_entry(_, _, _))
        .Times(1).WillOnce(::testing::Return(SAI_STATUS_SUCCESS));
    EXPECT_CALL(*mock_sai_srv6_api, remove_my_sid_entry(_))
        .Times(1).WillOnce(::testing::Return(SAI_STATUS_SUCCESS));

    runAppMySidTask(key, "ua", "", adj);
    resolveNeighbor(adj);
    EXPECT_TRUE(gSrv6Orch->mySidExists(key));
    EXPECT_TRUE(gSrv6Orch->m_pendingSRv6MySIDEntries.empty());
    EXPECT_EQ(gNeighOrch->getNextHopRefCount(NextHopKey(adj)), 1);
    runAppMySidTask(key, "", "", "", false);
    EXPECT_EQ(gNeighOrch->getNextHopRefCount(NextHopKey(adj)), 0);
}

TEST_F(Srv6OrchMySidTest, PendingDeletePreservesOtherSidOnSameNeighbor)
{
    const string deleted_key = "32:16:16:0:fc00:0:1:1::";
    const string retained_key = "32:16:16:0:fc00:0:1:2::";
    const string adj = "2001:db8::1@Ethernet0";
    EXPECT_CALL(*mock_sai_srv6_api, create_my_sid_entry(_, _, _))
        .Times(1).WillOnce(::testing::Return(SAI_STATUS_SUCCESS));
    EXPECT_CALL(*mock_sai_srv6_api, remove_my_sid_entry(_))
        .Times(1).WillOnce(::testing::Return(SAI_STATUS_SUCCESS));

    runAppMySidTask(deleted_key, "ua", "", adj);
    runAppMySidTask(retained_key, "end.x", "", adj);
    runAppMySidTask(deleted_key, "", "", "", false);
    ASSERT_EQ(gSrv6Orch->m_pendingSRv6MySIDEntries.at(NextHopKey(adj)).size(), 1u);
    resolveNeighbor(adj);
    EXPECT_FALSE(gSrv6Orch->mySidExists(deleted_key));
    EXPECT_TRUE(gSrv6Orch->mySidExists(retained_key));
    runAppMySidTask(retained_key, "", "", "", false);
}

TEST_F(Srv6OrchMySidTest, PendingReplacementUsesOnlyLatestNeighbor)
{
    const string key = "32:16:16:0:fc00:0:1:1::";
    const string old_adj = "2001:db8::1@Ethernet0";
    const string new_adj = "2001:db8::2@Ethernet4";
    EXPECT_CALL(*mock_sai_srv6_api, create_my_sid_entry(_, _, _))
        .Times(1).WillOnce(::testing::Return(SAI_STATUS_SUCCESS));
    EXPECT_CALL(*mock_sai_srv6_api, remove_my_sid_entry(_))
        .Times(1).WillOnce(::testing::Return(SAI_STATUS_SUCCESS));

    runAppMySidTask(key, "ua", "", old_adj);
    runAppMySidTask(key, "ua", "", new_adj);
    EXPECT_EQ(gSrv6Orch->m_pendingSRv6MySIDEntries.count(NextHopKey(old_adj)), 0u);
    resolveNeighbor(old_adj);
    EXPECT_FALSE(gSrv6Orch->mySidExists(key));
    resolveNeighbor(new_adj);
    ASSERT_TRUE(gSrv6Orch->mySidExists(key));
    EXPECT_EQ(gSrv6Orch->srv6_my_sid_table_.at(key).endAdjString, new_adj);
    EXPECT_EQ(gNeighOrch->getNextHopRefCount(NextHopKey(old_adj)), 0);
    runAppMySidTask(key, "", "", "", false);
}

TEST_F(Srv6OrchMySidTest, DeleteCancelsAllLegacyPendingRequestsForSid)
{
    const string key = "32:16:16:0:fc00:0:1:1::";
    const string adj1 = "2001:db8::1@Ethernet0";
    const string adj2 = "2001:db8::2@Ethernet4";
    // Simulate duplicate pending tuples left by the older SET path.
    for (const auto& adj : {adj1, adj2})
        gSrv6Orch->m_pendingSRv6MySIDEntries[NextHopKey(adj)].insert(
            make_tuple(key, "", adj, "ua"));
    EXPECT_CALL(*mock_sai_srv6_api, create_my_sid_entry(_, _, _)).Times(0);
    EXPECT_CALL(*mock_sai_srv6_api, remove_my_sid_entry(_)).Times(0);
    runAppMySidTask(key, "", "", "", false);
    EXPECT_TRUE(gSrv6Orch->m_pendingSRv6MySIDEntries.empty());
    resolveNeighbor(adj1);
    resolveNeighbor(adj2);
    EXPECT_FALSE(gSrv6Orch->mySidExists(key));
}

TEST_F(Srv6OrchMySidTest, InstalledDeleteDoesNotRecreateOnNeighborAdd)
{
    const string key = "32:16:16:0:fc00:0:1:1::";
    const string adj = "2001:db8::1@Ethernet0";
    EXPECT_CALL(*mock_sai_srv6_api, create_my_sid_entry(_, _, _))
        .Times(1).WillOnce(::testing::Return(SAI_STATUS_SUCCESS));
    EXPECT_CALL(*mock_sai_srv6_api, remove_my_sid_entry(_))
        .Times(1).WillOnce(::testing::Return(SAI_STATUS_SUCCESS));
    resolveNeighbor(adj);
    runAppMySidTask(key, "end.x", "", adj);
    ASSERT_TRUE(gSrv6Orch->mySidExists(key));
    runAppMySidTask(key, "", "", "", false);
    resolveNeighbor(adj);
    EXPECT_FALSE(gSrv6Orch->mySidExists(key));
    EXPECT_TRUE(gSrv6Orch->m_pendingSRv6MySIDEntries.empty());
}

TEST_F(Srv6OrchMySidTest, InstalledRemoveFailureKeepsStateAndReferences)
{
    const string key = "32:16:16:0:fc00:0:1:1::";
    const string adj = "2001:db8::1@Ethernet0";
    EXPECT_CALL(*mock_sai_srv6_api, create_my_sid_entry(_, _, _))
        .Times(1).WillOnce(::testing::Return(SAI_STATUS_SUCCESS));
    EXPECT_CALL(*mock_sai_srv6_api, remove_my_sid_entry(_))
        .WillOnce(::testing::Return(SAI_STATUS_FAILURE))
        .WillOnce(::testing::Return(SAI_STATUS_SUCCESS));
    resolveNeighbor(adj);
    runAppMySidTask(key, "ua", "", adj);
    runAppMySidTask(key, "", "", "", false);
    EXPECT_TRUE(gSrv6Orch->mySidExists(key));
    EXPECT_EQ(gNeighOrch->getNextHopRefCount(NextHopKey(adj)), 1);
    // Explicit second DEL: do not claim this patch adds generic SAI retry.
    runAppMySidTask(key, "", "", "", false);
    EXPECT_FALSE(gSrv6Orch->mySidExists(key));
    EXPECT_EQ(gNeighOrch->getNextHopRefCount(NextHopKey(adj)), 0);
}

TEST_F(Srv6OrchMySidTest, InvalidReplacementDoesNotReplaySupersededRequest)
{
    const string key = "32:16:16:0:fc00:0:1:1::";
    const string adj = "2001:db8::1@Ethernet0";
    EXPECT_CALL(*mock_sai_srv6_api, create_my_sid_entry(_, _, _)).Times(0);
    EXPECT_CALL(*mock_sai_srv6_api, remove_my_sid_entry(_)).Times(0);

    runAppMySidTask(key, "ua", "", adj);
    ASSERT_EQ(gSrv6Orch->m_pendingSRv6MySIDEntries.size(), 1u);
    // APPL_DB SET supersedes the old request, even if its new action is rejected.
    runAppMySidTask(key, "invalid-action", "", adj);
    EXPECT_TRUE(gSrv6Orch->m_pendingSRv6MySIDEntries.empty());
    resolveNeighbor(adj);
    EXPECT_FALSE(gSrv6Orch->mySidExists(key));
    EXPECT_EQ(gNeighOrch->getNextHopRefCount(NextHopKey(adj)), 0);
}

TEST_F(Srv6OrchMySidTest, InvalidReplacementPreservesInstalledEntry)
{
    const string key = "32:16:16:0:fc00:0:1:1::";
    const string adj = "2001:db8::1@Ethernet0";
    EXPECT_CALL(*mock_sai_srv6_api, create_my_sid_entry(_, _, _))
        .Times(1).WillOnce(::testing::Return(SAI_STATUS_SUCCESS));
    EXPECT_CALL(*mock_sai_srv6_api, remove_my_sid_entry(_))
        .Times(1).WillOnce(::testing::Return(SAI_STATUS_SUCCESS));

    resolveNeighbor(adj);
    runAppMySidTask(key, "end.x", "", adj);
    ASSERT_TRUE(gSrv6Orch->mySidExists(key));
    runAppMySidTask(key, "invalid-action", "", adj);
    EXPECT_TRUE(gSrv6Orch->mySidExists(key));
    EXPECT_TRUE(gSrv6Orch->m_pendingSRv6MySIDEntries.empty());
    EXPECT_EQ(gSrv6Orch->srv6_my_sid_table_.at(key).endAdjString, adj);
    EXPECT_EQ(gNeighOrch->getNextHopRefCount(NextHopKey(adj)), 1);
    EXPECT_EQ(my_sid_set_calls, 0u);
    runAppMySidTask(key, "", "", "", false);
    EXPECT_EQ(gNeighOrch->getNextHopRefCount(NextHopKey(adj)), 0);
}

TEST_F(Srv6OrchMySidTest, DeleteInstalledEntryCancelsPendingReplacement)
{
    const string key = "32:16:16:0:fc00:0:1:1::";
    const string old_adj = "2001:db8::1@Ethernet0";
    const string new_adj = "2001:db8::2@Ethernet4";
    EXPECT_CALL(*mock_sai_srv6_api, create_my_sid_entry(_, _, _))
        .Times(1).WillOnce(::testing::Return(SAI_STATUS_SUCCESS));
    EXPECT_CALL(*mock_sai_srv6_api, remove_my_sid_entry(_))
        .Times(1).WillOnce(::testing::Return(SAI_STATUS_SUCCESS));

    resolveNeighbor(old_adj);
    runAppMySidTask(key, "end.x", "", old_adj);
    runAppMySidTask(key, "end.x", "", new_adj);
    ASSERT_EQ(gSrv6Orch->m_pendingSRv6MySIDEntries.count(NextHopKey(new_adj)), 1u);
    ASSERT_TRUE(gSrv6Orch->mySidExists(key));
    EXPECT_EQ(gSrv6Orch->srv6_my_sid_table_.at(key).endAdjString, old_adj);
    EXPECT_EQ(gNeighOrch->getNextHopRefCount(NextHopKey(old_adj)), 1);
    runAppMySidTask(key, "", "", "", false);
    EXPECT_TRUE(gSrv6Orch->m_pendingSRv6MySIDEntries.empty());
    EXPECT_EQ(gNeighOrch->getNextHopRefCount(NextHopKey(old_adj)), 0);
    resolveNeighbor(new_adj);
    EXPECT_FALSE(gSrv6Orch->mySidExists(key));
    EXPECT_EQ(gNeighOrch->getNextHopRefCount(NextHopKey(new_adj)), 0);
    EXPECT_EQ(my_sid_set_calls, 0u);
}

TEST_F(Srv6OrchMySidTest, PendingActionReplacementDoesNotReplayOldAdjacency)
{
    const string key = "32:16:16:0:fc00:0:1:1::";
    const string adj = "2001:db8::1@Ethernet0";
    EXPECT_CALL(*mock_sai_srv6_api, create_my_sid_entry(_, _, _))
        .Times(1).WillOnce(::testing::Return(SAI_STATUS_SUCCESS));
    EXPECT_CALL(*mock_sai_srv6_api, remove_my_sid_entry(_))
        .Times(1).WillOnce(::testing::Return(SAI_STATUS_SUCCESS));

    runAppMySidTask(key, "ua", "", adj);
    ASSERT_EQ(gSrv6Orch->m_pendingSRv6MySIDEntries.size(), 1u);
    runAppMySidTask(key, "end", "", "");
    ASSERT_TRUE(gSrv6Orch->mySidExists(key));
    EXPECT_TRUE(gSrv6Orch->m_pendingSRv6MySIDEntries.empty());
    resolveNeighbor(adj);
    EXPECT_EQ(gSrv6Orch->srv6_my_sid_table_.at(key).endBehavior,
              SAI_MY_SID_ENTRY_ENDPOINT_BEHAVIOR_E);
    EXPECT_EQ(gNeighOrch->getNextHopRefCount(NextHopKey(adj)), 0);
    EXPECT_EQ(my_sid_set_calls, 0u);
    runAppMySidTask(key, "", "", "", false);
}

TEST_F(Srv6OrchMySidTest, PendingReplacementToDefaultVrfDoesNotReplayOldAdjacency)
{
    const string key = "32:16:16:0:fc00:0:1:1::";
    const string adj = "2001:db8::1@Ethernet0";
    EXPECT_CALL(*mock_sai_srv6_api, create_my_sid_entry(_, _, _))
        .Times(1).WillOnce(::testing::Return(SAI_STATUS_SUCCESS));
    EXPECT_CALL(*mock_sai_srv6_api, remove_my_sid_entry(_))
        .Times(1).WillOnce(::testing::Return(SAI_STATUS_SUCCESS));

    runAppMySidTask(key, "end.x", "", adj);
    ASSERT_EQ(gSrv6Orch->m_pendingSRv6MySIDEntries.size(), 1u);
    runAppMySidTask(key, "end.t", "default", "");
    ASSERT_TRUE(gSrv6Orch->mySidExists(key));
    EXPECT_TRUE(gSrv6Orch->m_pendingSRv6MySIDEntries.empty());
    resolveNeighbor(adj);
    const auto& entry = gSrv6Orch->srv6_my_sid_table_.at(key);
    EXPECT_EQ(entry.endBehavior, SAI_MY_SID_ENTRY_ENDPOINT_BEHAVIOR_T);
    EXPECT_EQ(entry.endVrfString, "default");
    EXPECT_EQ(gNeighOrch->getNextHopRefCount(NextHopKey(adj)), 0);
    EXPECT_EQ(my_sid_set_calls, 0u);
    runAppMySidTask(key, "", "", "", false);
}

} // namespace srv6orch_test
