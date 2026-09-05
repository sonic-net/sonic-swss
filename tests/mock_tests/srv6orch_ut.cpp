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

using ::testing::_;
using ::testing::AtLeast;
using ::testing::Return;
using namespace mock_orch_test;

class Srv6OrchMySidTest : public MockOrchTest
{
protected:
    void PostSetUp() override
    {
        INIT_SAI_API_MOCK(tunnel);
        INIT_SAI_API_MOCK(srv6);
        MockSaiApis();
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

    void addVrf(const string& vrf)
    {
        auto* consumer = dynamic_cast<Consumer*>(gVrfOrch->getExecutor(APP_VRF_TABLE_NAME));
        ASSERT_NE(consumer, nullptr);
        deque<KeyOpFieldsValuesTuple> entries = {{vrf, SET_COMMAND, {}}};
        consumer->addToSync(entries);
        static_cast<Orch*>(gVrfOrch)->doTask();
        ASSERT_TRUE(gVrfOrch->isVRFexists(vrf));
    }

    void addNextHop(const NextHopKey& nexthop, sai_object_id_t oid)
    {
        gNeighOrch->updateSrv6Nexthop(nexthop, oid);
    }

    void notifyNextHopAvailable(const NextHopKey& nexthop)
    {
        NeighborUpdate update = {nexthop, MacAddress("00:11:22:33:44:55"), true};
        gSrv6Orch->update(SUBJECT_TYPE_NEIGH_CHANGE, &update);
    }

    void retryMySidTasks()
    {
        static_cast<Orch*>(gSrv6Orch)->doTask();
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

TEST_F(Srv6OrchMySidTest, DuplicateEndReplayIsNoOp)
{
    const string key = "32:16:16:0:fc00:0:1:1::";

    EXPECT_CALL(*mock_sai_srv6_api, create_my_sid_entry(_, _, _)).Times(1);

    runAppMySidTask(key, "end", "", "");
    runAppMySidTask(key, "end", "", "");

    vector<string> pending;
    static_cast<Orch*>(gSrv6Orch)->dumpPendingTasks(pending);
    EXPECT_TRUE(pending.empty());
}

TEST_F(Srv6OrchMySidTest, CounterEnabledDuplicateReplayCreatesOneMapping)
{
    const string key = "32:16:16:0:fc00:0:1:30::";
    gSrv6Orch->setCountersState(true);

    EXPECT_CALL(*mock_sai_srv6_api, create_my_sid_entry(_, _, _)).Times(1);

    runAppMySidTask(key, "end", "", "");
    runAppMySidTask(key, "end", "", "");

    DBConnector counters_db("COUNTERS_DB", 0);
    Table name_map(&counters_db, COUNTERS_SRV6_NAME_MAP);
    vector<FieldValueTuple> mappings;
    ASSERT_TRUE(name_map.get("", mappings));
    EXPECT_EQ(mappings.size(), 1u);

    runAppMySidTask(key, "", "", "", false);
    EXPECT_FALSE(name_map.get("", mappings));
}

TEST_F(Srv6OrchMySidTest, MissingVrfRetriesAndDuplicateKeepsOneReference)
{
    const string key = "32:16:16:0:fc00:0:1:2::";
    const string vrf = "VrfBlue";

    EXPECT_CALL(*mock_sai_srv6_api, create_my_sid_entry(_, _, _)).Times(1);

    runAppMySidTask(key, "end.t", vrf, "");
    auto* retry = static_cast<Orch*>(gSrv6Orch)->getRetryCache(APP_SRV6_MY_SID_TABLE_NAME);
    ASSERT_NE(retry, nullptr);
    EXPECT_EQ(retry->getRetryMap().count(key), 1u);

    vector<string> pending;
    static_cast<Orch*>(gSrv6Orch)->dumpPendingTasks(pending);
    EXPECT_FALSE(pending.empty());

    addVrf(vrf);
    retryMySidTasks();

    EXPECT_EQ(retry->getRetryMap().count(key), 0u);
    EXPECT_EQ(gVrfOrch->getVrfRefCount(vrf), 1);

    runAppMySidTask(key, "end.t", vrf, "");
    EXPECT_EQ(gVrfOrch->getVrfRefCount(vrf), 1);
}

TEST_F(Srv6OrchMySidTest, MissingNeighborRetriesAndDuplicateKeepsOneReference)
{
    const string key = "32:16:16:0:fc00:0:1:3::";
    const NextHopKey nexthop("2001:db8::1");

    EXPECT_CALL(*mock_sai_srv6_api, create_my_sid_entry(_, _, _))
        .WillOnce(Return(SAI_STATUS_SUCCESS));

    runAppMySidTask(key, "end.x", "", nexthop.to_string());
    auto* retry = static_cast<Orch*>(gSrv6Orch)->getRetryCache(APP_SRV6_MY_SID_TABLE_NAME);
    ASSERT_NE(retry, nullptr);
    EXPECT_EQ(retry->getRetryMap().count(key), 1u);

    addNextHop(nexthop, 0x9001);
    notifyNextHopAvailable(nexthop);
    retryMySidTasks();

    EXPECT_EQ(retry->getRetryMap().count(key), 0u);
    EXPECT_EQ(gNeighOrch->getNextHopRefCount(nexthop), 1);

    runAppMySidTask(key, "end.x", "", nexthop.to_string());
    EXPECT_EQ(gNeighOrch->getNextHopRefCount(nexthop), 1);
}

TEST_F(Srv6OrchMySidTest, NeighborRetryIgnoresAliasResolutionChanges)
{
    const string key = "32:16:16:0:fc00:0:1:31::";
    const string neighbor_ip = "2001:db8::31";

    runAppMySidTask(key, "end.x", "", neighbor_ip);
    auto* retry = static_cast<Orch*>(gSrv6Orch)->getRetryCache(APP_SRV6_MY_SID_TABLE_NAME);
    ASSERT_NE(retry, nullptr);
    EXPECT_EQ(retry->getRetryMap().count(key), 1u);

    notifyNextHopAvailable(NextHopKey(IpAddress(neighbor_ip), "Ethernet0"));
    EXPECT_EQ(static_cast<Orch*>(gSrv6Orch)->retryToSync(APP_SRV6_MY_SID_TABLE_NAME), 1u);
    EXPECT_EQ(retry->getRetryMap().count(key), 0u);
}

TEST_F(Srv6OrchMySidTest, VrfAndActionReplacementBalancesReferences)
{
    const string key = "32:16:16:0:fc00:0:1:4::";
    addVrf("VrfRed");
    addVrf("VrfBlue");

    runAppMySidTask(key, "end.t", "VrfRed", "");
    EXPECT_EQ(gVrfOrch->getVrfRefCount("VrfRed"), 1);
    EXPECT_EQ(gVrfOrch->getVrfRefCount("VrfBlue"), 0);

    runAppMySidTask(key, "end.t", "VrfBlue", "");
    EXPECT_EQ(gVrfOrch->getVrfRefCount("VrfRed"), 0);
    EXPECT_EQ(gVrfOrch->getVrfRefCount("VrfBlue"), 1);

    runAppMySidTask(key, "end", "", "");
    EXPECT_EQ(gVrfOrch->getVrfRefCount("VrfBlue"), 0);
}

TEST_F(Srv6OrchMySidTest, AdjacencyReplacementBalancesReferences)
{
    const string key = "32:16:16:0:fc00:0:1:5::";
    const NextHopKey first("2001:db8::1");
    const NextHopKey second("2001:db8::2");
    addNextHop(first, 0x9001);
    addNextHop(second, 0x9002);

    EXPECT_CALL(*mock_sai_srv6_api, create_my_sid_entry(_, _, _))
        .WillOnce(Return(SAI_STATUS_SUCCESS));
    auto old_set = sai_srv6_api->set_my_sid_entry_attribute;
    sai_srv6_api->set_my_sid_entry_attribute = [](
        const sai_my_sid_entry_t*, const sai_attribute_t*) -> sai_status_t { return SAI_STATUS_SUCCESS; };

    runAppMySidTask(key, "end.x", "", first.to_string());
    EXPECT_EQ(gNeighOrch->getNextHopRefCount(first), 1);
    EXPECT_EQ(gNeighOrch->getNextHopRefCount(second), 0);

    runAppMySidTask(key, "end.x", "", second.to_string());
    EXPECT_EQ(gNeighOrch->getNextHopRefCount(first), 0);
    EXPECT_EQ(gNeighOrch->getNextHopRefCount(second), 1);

    sai_srv6_api->set_my_sid_entry_attribute = old_set;
}

TEST_F(Srv6OrchMySidTest, SharedDscpTunnelIsReleasedAfterLastMySid)
{
    const string locator = "loc1";
    const string first_key = "32:16:16:0:fc00:0:1:10::1";
    const string second_key = "32:16:16:0:fc00:0:1:10::2";

    addLocatorConfig(locator);
    runCfgMySidTask(locator + "|fc00:0:1:10::1/64", {{"decap_dscp_mode", "uniform"}});
    runCfgMySidTask(locator + "|fc00:0:1:10::2/64", {{"decap_dscp_mode", "uniform"}});

    EXPECT_CALL(*mock_sai_tunnel_api, create_tunnel(_, _, _, _)).Times(1);
    EXPECT_CALL(*mock_sai_tunnel_api, remove_tunnel(_)).Times(1);
    EXPECT_CALL(*mock_sai_srv6_api, create_my_sid_entry(_, _, _)).Times(2);

    runAppMySidTask(first_key, "un", "default", "");
    runAppMySidTask(first_key, "un", "default", "");
    runAppMySidTask(second_key, "un", "default", "");
    runAppMySidTask(first_key, "", "", "", false);
    runAppMySidTask(second_key, "", "", "", false);
}

TEST_F(Srv6OrchMySidTest, DscpTunnelReplacementBalancesReferences)
{
    const string locator = "loc1";
    const string address = "fc00:0:1:12::1";
    const string key = "32:16:16:0:" + address;
    const string config_key = locator + "|" + address + "/64";

    addLocatorConfig(locator);

    EXPECT_CALL(*mock_sai_tunnel_api, create_tunnel(_, _, _, _)).Times(2);
    EXPECT_CALL(*mock_sai_tunnel_api, remove_tunnel(_)).Times(2);
    EXPECT_CALL(*mock_sai_srv6_api, create_my_sid_entry(_, _, _)).Times(1);

    runCfgMySidTask(config_key, {{"decap_dscp_mode", "uniform"}});
    runAppMySidTask(key, "un", "default", "");

    runCfgMySidTask(config_key, {}, false);
    runCfgMySidTask(config_key, {{"decap_dscp_mode", "pipe"}});
    runAppMySidTask(key, "un", "default", "");

    runAppMySidTask(key, "", "", "", false);
}

TEST_F(Srv6OrchMySidTest, SaiCreateFailureRollsBackTunnelAndRemainsPending)
{
    const string locator = "loc1";
    const string key = "32:16:16:0:fc00:0:1:20::1";

    addLocatorConfig(locator);
    runCfgMySidTask(locator + "|fc00:0:1:20::1/64", {{"decap_dscp_mode", "uniform"}});

    EXPECT_CALL(*mock_sai_tunnel_api, create_tunnel(_, _, _, _)).Times(1);
    EXPECT_CALL(*mock_sai_tunnel_api, remove_tunnel(_)).Times(1);
    EXPECT_CALL(*mock_sai_srv6_api, create_my_sid_entry(_, _, _))
        .WillOnce(Return(SAI_STATUS_TABLE_FULL));

    runAppMySidTask(key, "un", "default", "");

    vector<string> pending;
    static_cast<Orch*>(gSrv6Orch)->dumpPendingTasks(pending);
    EXPECT_FALSE(pending.empty());
}

TEST_F(Srv6OrchMySidTest, SaiCreateAlreadyExistsPreservesEntryAndRollsBackResources)
{
    const string locator = "loc1";
    const string key = "32:16:16:0:fc00:0:1:23::1";

    addLocatorConfig(locator);
    runCfgMySidTask(locator + "|fc00:0:1:23::1/64", {{"decap_dscp_mode", "uniform"}});

    EXPECT_CALL(*mock_sai_tunnel_api, create_tunnel(_, _, _, _)).Times(1);
    EXPECT_CALL(*mock_sai_tunnel_api, remove_tunnel(_)).Times(1);
    EXPECT_CALL(*mock_sai_srv6_api, create_my_sid_entry(_, _, _))
        .WillOnce(Return(SAI_STATUS_ITEM_ALREADY_EXISTS));
    EXPECT_CALL(*mock_sai_srv6_api, remove_my_sid_entry(_)).Times(0);

    auto old_get = sai_srv6_api->get_my_sid_entry_attribute;
    sai_srv6_api->get_my_sid_entry_attribute = [](
        const sai_my_sid_entry_t*, uint32_t attr_count, sai_attribute_t* attrs) -> sai_status_t {
        for (uint32_t index = 0; index < attr_count; ++index)
        {
            attrs[index].value.oid = attrs[index].id == SAI_MY_SID_ENTRY_ATTR_TUNNEL_ID
                                        ? 0xdead
                                        : SAI_NULL_OBJECT_ID;
        }
        return SAI_STATUS_SUCCESS;
    };

    runAppMySidTask(key, "un", "default", "");
    sai_srv6_api->get_my_sid_entry_attribute = old_get;

    vector<string> pending;
    static_cast<Orch*>(gSrv6Orch)->dumpPendingTasks(pending);
    EXPECT_FALSE(pending.empty());
}

TEST_F(Srv6OrchMySidTest, SaiCreateAlreadyExistsReconcilesMatchingEntryInPlace)
{
    const string key = "32:16:16:0:fc00:0:1:24::";

    EXPECT_CALL(*mock_sai_srv6_api, create_my_sid_entry(_, _, _))
        .WillOnce(Return(SAI_STATUS_ITEM_ALREADY_EXISTS));
    EXPECT_CALL(*mock_sai_srv6_api, remove_my_sid_entry(_)).Times(0);

    auto old_get = sai_srv6_api->get_my_sid_entry_attribute;
    sai_srv6_api->get_my_sid_entry_attribute = [](
        const sai_my_sid_entry_t*, uint32_t attr_count, sai_attribute_t* attrs) -> sai_status_t {
        for (uint32_t index = 0; index < attr_count; ++index)
        {
            if (attrs[index].id == SAI_MY_SID_ENTRY_ATTR_ENDPOINT_BEHAVIOR)
            {
                attrs[index].value.s32 = SAI_MY_SID_ENTRY_ENDPOINT_BEHAVIOR_E;
            }
            else if (attrs[index].id == SAI_MY_SID_ENTRY_ATTR_ENDPOINT_BEHAVIOR_FLAVOR)
            {
                attrs[index].value.s32 = SAI_MY_SID_ENTRY_ENDPOINT_BEHAVIOR_FLAVOR_PSP_AND_USD;
            }
            else
            {
                attrs[index].value.oid = SAI_NULL_OBJECT_ID;
            }
        }
        return SAI_STATUS_SUCCESS;
    };

    runAppMySidTask(key, "end", "", "");
    runAppMySidTask(key, "end", "", "");
    sai_srv6_api->get_my_sid_entry_attribute = old_get;

    vector<string> pending;
    static_cast<Orch*>(gSrv6Orch)->dumpPendingTasks(pending);
    EXPECT_TRUE(pending.empty());
}

TEST_F(Srv6OrchMySidTest, SaiSetFailureKeepsOldReferenceUntilRetry)
{
    const string key = "32:16:16:0:fc00:0:1:21::";
    addVrf("VrfRed");
    addVrf("VrfBlue");
    runAppMySidTask(key, "end.t", "VrfRed", "");

    auto old_set = sai_srv6_api->set_my_sid_entry_attribute;
    sai_srv6_api->set_my_sid_entry_attribute = [](
        const sai_my_sid_entry_t*, const sai_attribute_t*) -> sai_status_t { return SAI_STATUS_TABLE_FULL; };

    runAppMySidTask(key, "end.t", "VrfBlue", "");

    EXPECT_EQ(gVrfOrch->getVrfRefCount("VrfRed"), 1);
    EXPECT_EQ(gVrfOrch->getVrfRefCount("VrfBlue"), 0);

    sai_srv6_api->set_my_sid_entry_attribute = old_set;
    retryMySidTasks();

    EXPECT_EQ(gVrfOrch->getVrfRefCount("VrfRed"), 0);
    EXPECT_EQ(gVrfOrch->getVrfRefCount("VrfBlue"), 1);
}

TEST_F(Srv6OrchMySidTest, SaiRemoveFailureRetriesWithoutDoubleCleanup)
{
    const string key = "32:16:16:0:fc00:0:1:22::";
    runAppMySidTask(key, "end", "", "");

    EXPECT_CALL(*mock_sai_srv6_api, remove_my_sid_entry(_))
        .WillOnce(Return(SAI_STATUS_OBJECT_IN_USE))
        .WillOnce([](const sai_my_sid_entry_t *entry) {
            return old_sai_srv6_api->remove_my_sid_entry(entry);
        });

    runAppMySidTask(key, "", "", "", false);
    retryMySidTasks();
    runAppMySidTask(key, "", "", "", false);

    vector<string> pending;
    static_cast<Orch*>(gSrv6Orch)->dumpPendingTasks(pending);
    EXPECT_TRUE(pending.empty());
}

} // namespace srv6orch_test
