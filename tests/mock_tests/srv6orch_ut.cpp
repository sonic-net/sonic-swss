#include "mock_orch_test.h"
#include "mock_orchagent_main.h"
#include "mock_sai_api.h"
#include "ut_helper.h"
#include "warm_restart.h"

#include <gtest/gtest.h>
#include <deque>

using namespace std;
using namespace swss;

EXTERN_MOCK_FNS

namespace srv6orch_test
{

DEFINE_SAI_GENERIC_API_MOCK(tunnel, tunnel);
DEFINE_SAI_API_MOCK(srv6, my_sid);
DEFINE_SAI_GENERIC_API_MOCK(counter, counter);

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
        INIT_SAI_API_MOCK(counter);
        MockSaiApis();
        initSaiFailureTable();
        setSaiFailureStatus(false);
    }

    void PreTearDown() override
    {
        setSaiFailureStatus(false);
        RestoreSaiApis();
        DEINIT_SAI_API_MOCK(counter);
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

    void runAppMySidRawTask(const string& key, const string& op, const vector<FieldValueTuple>& fvs)
    {
        auto* executor = static_cast<Orch*>(gSrv6Orch)->getExecutor(APP_SRV6_MY_SID_TABLE_NAME);
        auto* consumer = dynamic_cast<Consumer*>(executor);
        ASSERT_NE(consumer, nullptr);
        deque<KeyOpFieldsValuesTuple> entries = {{key, op, fvs}};
        consumer->addToSync(entries);
        static_cast<Orch*>(gSrv6Orch)->doTask(*consumer);
    }

    void runAppMySidTask(const string& key, const string& action, const string& vrf,
                        const string& adj, bool is_set = true)
    {
        vector<FieldValueTuple> fvs = {{"action", action}};
        if (!vrf.empty())
            fvs.push_back({"vrf", vrf});
        if (!adj.empty())
            fvs.push_back({"adj", adj});
        runAppMySidRawTask(key, is_set ? SET_COMMAND : DEL_COMMAND, fvs);
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

TEST_F(Srv6OrchMySidTest, SaiCreateAlreadyExistsResourceConflictFailsWithoutRetry)
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
            if (attrs[index].id == SAI_MY_SID_ENTRY_ATTR_ENDPOINT_BEHAVIOR)
            {
                attrs[index].value.s32 = SAI_MY_SID_ENTRY_ENDPOINT_BEHAVIOR_UN;
            }
            else if (attrs[index].id == SAI_MY_SID_ENTRY_ATTR_ENDPOINT_BEHAVIOR_FLAVOR)
            {
                attrs[index].value.s32 = SAI_MY_SID_ENTRY_ENDPOINT_BEHAVIOR_FLAVOR_USD;
            }
            else
            {
                attrs[index].value.oid = attrs[index].id == SAI_MY_SID_ENTRY_ATTR_TUNNEL_ID
                                            ? 0xdead
                                            : SAI_NULL_OBJECT_ID;
            }
        }
        return SAI_STATUS_SUCCESS;
    };

    runAppMySidTask(key, "un", "default", "");
    retryMySidTasks();
    sai_srv6_api->get_my_sid_entry_attribute = old_get;

    vector<string> pending;
    static_cast<Orch*>(gSrv6Orch)->dumpPendingTasks(pending);
    EXPECT_TRUE(pending.empty());
    string error;
    EXPECT_TRUE(getSaiFailureStatus(error));
    EXPECT_NE(error.find(key), string::npos);
    EXPECT_NE(error.find("retained tunnel oid:0xdead"), string::npos);
}

class Srv6OrchMySidRecoveryTest : public Srv6OrchMySidTest, public testing::WithParamInterface<bool>
{
protected:
    void PostSetUp() override
    {
        Srv6OrchMySidTest::PostSetUp();
        WarmStart::initialize("orchagent", "swss");
        Table enable_table(m_state_db.get(), "WARM_RESTART_ENABLE_TABLE");
        enable_table.hset("swss", "enable", GetParam() ? "true" : "false");
        Table warm_restart_table(m_state_db.get(), STATE_WARM_RESTART_TABLE_NAME);
        warm_restart_table.hset("orchagent", "restore_count", "0");
        WarmStart::checkWarmStart("orchagent", "swss");
        WarmStart::setWarmStartState("orchagent", WarmStart::INITIALIZED);
        ASSERT_EQ(WarmStart::isWarmStart(), GetParam());
    }

    void PreTearDown() override
    {
        Table enable_table(m_state_db.get(), "WARM_RESTART_ENABLE_TABLE");
        enable_table.hset("swss", "enable", "false");
        Table warm_restart_table(m_state_db.get(), STATE_WARM_RESTART_TABLE_NAME);
        warm_restart_table.hset("orchagent", "restore_count", "");
        WarmStart::checkWarmStart("orchagent", "swss");
        EXPECT_FALSE(WarmStart::isWarmStart());
        WarmStart::setWarmStartState("orchagent", WarmStart::RECONCILED);
        Srv6OrchMySidTest::PreTearDown();
    }
};

TEST_P(Srv6OrchMySidRecoveryTest, InvalidMySidRequestsAreDiscarded)
{
    EXPECT_CALL(*mock_sai_srv6_api, create_my_sid_entry(_, _, _)).Times(0);

    runAppMySidTask("32:16:16:0:fc00:0:1:40::", "invalid", "", "");
    runAppMySidTask("32:16:16:0:fc00:0:1:41::", "end.t", "", "");
    runAppMySidTask("32:16:16:0:fc00:0:1:42::", "end.x", "", "");
    runAppMySidRawTask("32:16:16:0:fc00:0:1:43::", "INVALID", {});
    runAppMySidTask("32:16:16", "end", "", "");
    runAppMySidTask("129:16:16:0:fc00:0:1:44::", "end", "", "");
    runAppMySidTask("32:16:16:0:192.0.2.1", "end", "", "");
    runAppMySidTask("32:16:16:0:not-an-ip", "end", "", "");
    runAppMySidTask("64:32:32:1:fc00:0:1:45::", "end", "", "");
    retryMySidTasks();
    retryMySidTasks();

    vector<string> pending;
    static_cast<Orch*>(gSrv6Orch)->dumpPendingTasks(pending);
    EXPECT_TRUE(pending.empty());
}

TEST_P(Srv6OrchMySidRecoveryTest, PermanentSaiCreateFailureIsDiscarded)
{
    const string key = "32:16:16:0:fc00:0:1:46::";
    EXPECT_CALL(*mock_sai_srv6_api, create_my_sid_entry(_, _, _))
        .WillOnce(Return(SAI_STATUS_INVALID_PARAMETER));
    EXPECT_CALL(*mock_sai_srv6_api, remove_my_sid_entry(_)).Times(0);

    runAppMySidTask(key, "end", "", "");
    retryMySidTasks();
    retryMySidTasks();

    vector<string> pending;
    static_cast<Orch*>(gSrv6Orch)->dumpPendingTasks(pending);
    EXPECT_TRUE(pending.empty());
    string error;
    EXPECT_TRUE(getSaiFailureStatus(error));
    EXPECT_NE(error.find("SAI_STATUS_INVALID_PARAMETER"), string::npos);
}

TEST_P(Srv6OrchMySidRecoveryTest, RetryableSaiCreateFailureRemainsPending)
{
    const string key = "32:16:16:0:fc00:0:1:47::";
    EXPECT_CALL(*mock_sai_srv6_api, create_my_sid_entry(_, _, _))
        .WillOnce(Return(SAI_STATUS_TABLE_FULL))
        .WillOnce(Return(SAI_STATUS_SUCCESS));

    runAppMySidTask(key, "end", "", "");
    vector<string> pending;
    static_cast<Orch*>(gSrv6Orch)->dumpPendingTasks(pending);
    EXPECT_EQ(pending.size(), 1u);

    retryMySidTasks();
    retryMySidTasks();
    pending.clear();
    static_cast<Orch*>(gSrv6Orch)->dumpPendingTasks(pending);
    EXPECT_TRUE(pending.empty());
}

TEST_P(Srv6OrchMySidRecoveryTest, CounterConflictPreservesMappingWithoutRepeatedAllocation)
{
    const string key = "32:16:16:0:fc00:0:1:27::";
    const string counter_key = "fc00:0:1:27::/64";
    static sai_object_id_t retained_counter;
    sai_attribute_t counter_type{};
    counter_type.id = SAI_COUNTER_ATTR_TYPE;
    counter_type.value.s32 = SAI_COUNTER_TYPE_REGULAR;
    ASSERT_EQ(sai_counter_api->create_counter(&retained_counter, gSwitchId, 1, &counter_type), SAI_STATUS_SUCCESS);

    DBConnector counters_db("COUNTERS_DB", 0);
    Table name_map(&counters_db, COUNTERS_SRV6_NAME_MAP);
    name_map.hset("", counter_key, sai_serialize_object_id(retained_counter));
    gSrv6Orch->setCountersState(true);

    EXPECT_CALL(*mock_sai_counter_api, create_counter(_, _, _, _)).Times(1);
    EXPECT_CALL(*mock_sai_counter_api, remove_counter(testing::Ne(retained_counter))).Times(1);
    EXPECT_CALL(*mock_sai_counter_api, remove_counter(retained_counter)).Times(0);
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
                attrs[index].value.oid = attrs[index].id == SAI_MY_SID_ENTRY_ATTR_COUNTER_ID
                                            ? retained_counter : SAI_NULL_OBJECT_ID;
            }
        }
        return SAI_STATUS_SUCCESS;
    };
    auto old_set = sai_srv6_api->set_my_sid_entry_attribute;
    sai_srv6_api->set_my_sid_entry_attribute = [](
        const sai_my_sid_entry_t*, const sai_attribute_t*) -> sai_status_t {
        ADD_FAILURE() << "Retained MySID must not be modified on resource conflict";
        return SAI_STATUS_FAILURE;
    };

    runAppMySidTask(key, "end", "", "");
    retryMySidTasks();
    retryMySidTasks();
    sai_srv6_api->set_my_sid_entry_attribute = old_set;
    sai_srv6_api->get_my_sid_entry_attribute = old_get;

    string mapping;
    EXPECT_TRUE(name_map.hget("", counter_key, mapping));
    EXPECT_EQ(mapping, sai_serialize_object_id(retained_counter));
    EXPECT_EQ(sai_counter_api->get_counter_attribute(retained_counter, 1, &counter_type), SAI_STATUS_SUCCESS);
    EXPECT_EQ(counter_type.value.s32, SAI_COUNTER_TYPE_REGULAR);

    vector<string> pending;
    static_cast<Orch*>(gSrv6Orch)->dumpPendingTasks(pending);
    EXPECT_TRUE(pending.empty());
    string error;
    EXPECT_TRUE(getSaiFailureStatus(error));
    EXPECT_NE(error.find(key), string::npos);
    EXPECT_NE(error.find("retained counter " + mapping), string::npos);
    EXPECT_NE(error.find("resource ownership is unknown"), string::npos);

    EXPECT_EQ(old_sai_counter_api->remove_counter(retained_counter), SAI_STATUS_SUCCESS);
    name_map.hdel("", counter_key);
}

INSTANTIATE_TEST_SUITE_P(RestoreModes, Srv6OrchMySidRecoveryTest, testing::Bool());

TEST_F(Srv6OrchMySidTest, SaiCreateAlreadyExistsReconcilesMatchingEntryInPlace)
{
    const string key = "32:16:16:0:fc00:0:1:24::";

    EXPECT_CALL(*mock_sai_srv6_api, create_my_sid_entry(_, _, _))
        .WillOnce(Return(SAI_STATUS_ITEM_ALREADY_EXISTS));
    EXPECT_CALL(*mock_sai_srv6_api, remove_my_sid_entry(_)).Times(0);

    static vector<vector<sai_attr_id_t>> get_requests;
    get_requests.clear();
    auto old_get = sai_srv6_api->get_my_sid_entry_attribute;
    sai_srv6_api->get_my_sid_entry_attribute = [](
        const sai_my_sid_entry_t*, uint32_t attr_count, sai_attribute_t* attrs) -> sai_status_t {
        vector<sai_attr_id_t> requested;
        for (uint32_t index = 0; index < attr_count; ++index)
        {
            requested.push_back(attrs[index].id);
            EXPECT_NE(attrs[index].id, SAI_MY_SID_ENTRY_ATTR_VRF);
            EXPECT_NE(attrs[index].id, SAI_MY_SID_ENTRY_ATTR_NEXT_HOP_ID);
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
        get_requests.push_back(requested);
        return SAI_STATUS_SUCCESS;
    };

    runAppMySidTask(key, "end", "", "");
    runAppMySidTask(key, "end", "", "");
    sai_srv6_api->get_my_sid_entry_attribute = old_get;

    ASSERT_GE(get_requests.size(), 2u);
    EXPECT_EQ(get_requests[0], vector<sai_attr_id_t>({SAI_MY_SID_ENTRY_ATTR_ENDPOINT_BEHAVIOR}));
    EXPECT_EQ(get_requests[1], vector<sai_attr_id_t>({SAI_MY_SID_ENTRY_ATTR_ENDPOINT_BEHAVIOR_FLAVOR}));
    vector<string> pending;
    static_cast<Orch*>(gSrv6Orch)->dumpPendingTasks(pending);
    EXPECT_TRUE(pending.empty());
}

TEST_F(Srv6OrchMySidTest, SaiCreateAlreadyExistsReadsRetainedAttributes)
{
    struct RetainedEntry
    {
        sai_my_sid_entry_endpoint_behavior_t behavior;
        sai_my_sid_entry_endpoint_behavior_flavor_t flavor;
        bool read_flavor;
        bool read_vrf;
        bool read_next_hop;
        bool read_tunnel;
    };
    const vector<RetainedEntry> entries = {
        {SAI_MY_SID_ENTRY_ENDPOINT_BEHAVIOR_E, SAI_MY_SID_ENTRY_ENDPOINT_BEHAVIOR_FLAVOR_NONE, true, false, false, false},
        {SAI_MY_SID_ENTRY_ENDPOINT_BEHAVIOR_E, SAI_MY_SID_ENTRY_ENDPOINT_BEHAVIOR_FLAVOR_PSP, true, false, false, false},
        {SAI_MY_SID_ENTRY_ENDPOINT_BEHAVIOR_E, SAI_MY_SID_ENTRY_ENDPOINT_BEHAVIOR_FLAVOR_PSP_AND_USD, true, false, false, true},
        {SAI_MY_SID_ENTRY_ENDPOINT_BEHAVIOR_T, SAI_MY_SID_ENTRY_ENDPOINT_BEHAVIOR_FLAVOR_NONE, true, true, false, false},
        {SAI_MY_SID_ENTRY_ENDPOINT_BEHAVIOR_X, SAI_MY_SID_ENTRY_ENDPOINT_BEHAVIOR_FLAVOR_PSP_AND_USD, true, false, true, true},
        {SAI_MY_SID_ENTRY_ENDPOINT_BEHAVIOR_DX6, SAI_MY_SID_ENTRY_ENDPOINT_BEHAVIOR_FLAVOR_NONE, false, false, true, false},
        {SAI_MY_SID_ENTRY_ENDPOINT_BEHAVIOR_DT46, SAI_MY_SID_ENTRY_ENDPOINT_BEHAVIOR_FLAVOR_NONE, false, true, false, true},
        {SAI_MY_SID_ENTRY_ENDPOINT_BEHAVIOR_B6_ENCAPS, SAI_MY_SID_ENTRY_ENDPOINT_BEHAVIOR_FLAVOR_NONE, false, false, true, false},
        {SAI_MY_SID_ENTRY_ENDPOINT_BEHAVIOR_UN, SAI_MY_SID_ENTRY_ENDPOINT_BEHAVIOR_FLAVOR_NONE, true, false, false, false},
        {SAI_MY_SID_ENTRY_ENDPOINT_BEHAVIOR_UN, SAI_MY_SID_ENTRY_ENDPOINT_BEHAVIOR_FLAVOR_USD, true, false, false, true},
        {SAI_MY_SID_ENTRY_ENDPOINT_BEHAVIOR_UA, SAI_MY_SID_ENTRY_ENDPOINT_BEHAVIOR_FLAVOR_USD, true, false, true, true},
        {SAI_MY_SID_ENTRY_ENDPOINT_BEHAVIOR_UDT46, SAI_MY_SID_ENTRY_ENDPOINT_BEHAVIOR_FLAVOR_NONE, false, true, false, true},
    };

    EXPECT_CALL(*mock_sai_srv6_api, create_my_sid_entry(_, _, _))
        .Times(static_cast<int>(entries.size()))
        .WillRepeatedly(Return(SAI_STATUS_ITEM_ALREADY_EXISTS));
    EXPECT_CALL(*mock_sai_srv6_api, remove_my_sid_entry(_)).Times(0);

    static RetainedEntry retained;
    static vector<sai_attr_id_t> expected_reads;
    static size_t read_count;
    auto old_get = sai_srv6_api->get_my_sid_entry_attribute;
    sai_srv6_api->get_my_sid_entry_attribute = [](
        const sai_my_sid_entry_t*, uint32_t attr_count, sai_attribute_t* attrs) -> sai_status_t {
        EXPECT_EQ(attr_count, 1u);
        if (attr_count != 1)
        {
            return SAI_STATUS_FAILURE;
        }
        if (attrs[0].id == SAI_MY_SID_ENTRY_ATTR_COUNTER_ID)
        {
            EXPECT_EQ(read_count, expected_reads.size());
            attrs[0].value.oid = SAI_NULL_OBJECT_ID;
            return SAI_STATUS_SUCCESS;
        }
        if (read_count >= expected_reads.size() || attrs[0].id != expected_reads[read_count])
        {
            ADD_FAILURE() << "Unexpected read of attribute " << attrs[0].id;
            return SAI_STATUS_INVALID_ATTRIBUTE_0;
        }
        ++read_count;
        if (attrs[0].id == SAI_MY_SID_ENTRY_ATTR_ENDPOINT_BEHAVIOR)
        {
            attrs[0].value.s32 = retained.behavior;
        }
        else if (attrs[0].id == SAI_MY_SID_ENTRY_ATTR_ENDPOINT_BEHAVIOR_FLAVOR)
        {
            attrs[0].value.s32 = retained.flavor;
        }
        else
        {
            attrs[0].value.oid = SAI_NULL_OBJECT_ID;
        }
        return SAI_STATUS_SUCCESS;
    };
    auto old_set = sai_srv6_api->set_my_sid_entry_attribute;
    sai_srv6_api->set_my_sid_entry_attribute = [](
        const sai_my_sid_entry_t*, const sai_attribute_t*) -> sai_status_t {
        return SAI_STATUS_SUCCESS;
    };

    for (size_t index = 0; index < entries.size(); ++index)
    {
        SCOPED_TRACE(index);
        retained = entries[index];
        read_count = 0;
        expected_reads = {SAI_MY_SID_ENTRY_ATTR_ENDPOINT_BEHAVIOR};
        if (retained.read_flavor)
        {
            expected_reads.push_back(SAI_MY_SID_ENTRY_ATTR_ENDPOINT_BEHAVIOR_FLAVOR);
        }
        if (retained.read_vrf)
        {
            expected_reads.push_back(SAI_MY_SID_ENTRY_ATTR_VRF);
        }
        if (retained.read_next_hop)
        {
            expected_reads.push_back(SAI_MY_SID_ENTRY_ATTR_NEXT_HOP_ID);
        }
        if (retained.read_tunnel)
        {
            expected_reads.push_back(SAI_MY_SID_ENTRY_ATTR_TUNNEL_ID);
        }
        const string action = retained.behavior == SAI_MY_SID_ENTRY_ENDPOINT_BEHAVIOR_E ? "end.dt46" : "end";
        runAppMySidTask("32:16:16:0:fc00:0:1:50::" + to_string(index), action, "default", "");
        EXPECT_EQ(read_count, expected_reads.size());

        vector<string> pending;
        static_cast<Orch*>(gSrv6Orch)->dumpPendingTasks(pending);
        EXPECT_TRUE(pending.empty());
    }
    sai_srv6_api->set_my_sid_entry_attribute = old_set;
    sai_srv6_api->get_my_sid_entry_attribute = old_get;
}

TEST_F(Srv6OrchMySidTest, SaiCreateAlreadyExistsReadbackFailureRemainsPending)
{
    const string key = "32:16:16:0:fc00:0:1:25::";
    const vector<sai_attr_id_t> failed_attributes = {
        SAI_MY_SID_ENTRY_ATTR_ENDPOINT_BEHAVIOR,
        SAI_MY_SID_ENTRY_ATTR_ENDPOINT_BEHAVIOR_FLAVOR,
        SAI_MY_SID_ENTRY_ATTR_VRF,
        SAI_MY_SID_ENTRY_ATTR_TUNNEL_ID,
    };
    EXPECT_CALL(*mock_sai_srv6_api, create_my_sid_entry(_, _, _))
        .Times(static_cast<int>(failed_attributes.size()))
        .WillRepeatedly(Return(SAI_STATUS_ITEM_ALREADY_EXISTS));
    EXPECT_CALL(*mock_sai_srv6_api, remove_my_sid_entry(_)).Times(0);

    static sai_attr_id_t failed_attribute;
    static bool failure_seen;
    auto old_get = sai_srv6_api->get_my_sid_entry_attribute;
    sai_srv6_api->get_my_sid_entry_attribute = [](
        const sai_my_sid_entry_t*, uint32_t attr_count, sai_attribute_t* attrs) -> sai_status_t {
        EXPECT_EQ(attr_count, 1u);
        EXPECT_FALSE(failure_seen);
        if (attr_count != 1)
        {
            return SAI_STATUS_FAILURE;
        }
        if (attrs[0].id == failed_attribute)
        {
            failure_seen = true;
            return SAI_STATUS_FAILURE;
        }
        if (attrs[0].id == SAI_MY_SID_ENTRY_ATTR_ENDPOINT_BEHAVIOR)
        {
            attrs[0].value.s32 = SAI_MY_SID_ENTRY_ENDPOINT_BEHAVIOR_T;
        }
        else if (attrs[0].id == SAI_MY_SID_ENTRY_ATTR_ENDPOINT_BEHAVIOR_FLAVOR)
        {
            attrs[0].value.s32 = SAI_MY_SID_ENTRY_ENDPOINT_BEHAVIOR_FLAVOR_USD;
        }
        else
        {
            attrs[0].value.oid = SAI_NULL_OBJECT_ID;
        }
        return SAI_STATUS_SUCCESS;
    };

    for (auto attr_id : failed_attributes)
    {
        SCOPED_TRACE(attr_id);
        failed_attribute = attr_id;
        failure_seen = false;
        runAppMySidTask(key, "end", "", "");
        EXPECT_TRUE(failure_seen);

        vector<string> pending;
        static_cast<Orch*>(gSrv6Orch)->dumpPendingTasks(pending);
        EXPECT_FALSE(pending.empty());
        runAppMySidRawTask(key, DEL_COMMAND, {});
    }
    sai_srv6_api->get_my_sid_entry_attribute = old_get;
}

TEST_F(Srv6OrchMySidTest, SaiCreateAlreadyExistsSetFailureRollsBackInPlace)
{
    const string key = "32:16:16:0:fc00:0:1:26::";

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
                attrs[index].value.s32 = SAI_MY_SID_ENTRY_ENDPOINT_BEHAVIOR_T;
            }
            else if (attrs[index].id == SAI_MY_SID_ENTRY_ATTR_ENDPOINT_BEHAVIOR_FLAVOR)
            {
                attrs[index].value.s32 = SAI_MY_SID_ENTRY_ENDPOINT_BEHAVIOR_FLAVOR_NONE;
            }
            else
            {
                attrs[index].value.oid = SAI_NULL_OBJECT_ID;
            }
        }
        return SAI_STATUS_SUCCESS;
    };

    static vector<pair<sai_attr_id_t, int32_t>> set_attributes;
    set_attributes.clear();
    auto old_set = sai_srv6_api->set_my_sid_entry_attribute;
    sai_srv6_api->set_my_sid_entry_attribute = [](
        const sai_my_sid_entry_t*, const sai_attribute_t* attr) -> sai_status_t {
        set_attributes.emplace_back(attr->id, attr->value.s32);
        return set_attributes.size() == 2 ? SAI_STATUS_INVALID_ATTR_VALUE_0 : SAI_STATUS_SUCCESS;
    };

    runAppMySidTask(key, "end", "", "");
    sai_srv6_api->set_my_sid_entry_attribute = old_set;
    sai_srv6_api->get_my_sid_entry_attribute = old_get;

    vector<pair<sai_attr_id_t, int32_t>> expected = {
        {SAI_MY_SID_ENTRY_ATTR_ENDPOINT_BEHAVIOR, SAI_MY_SID_ENTRY_ENDPOINT_BEHAVIOR_E},
        {SAI_MY_SID_ENTRY_ATTR_ENDPOINT_BEHAVIOR_FLAVOR,
         SAI_MY_SID_ENTRY_ENDPOINT_BEHAVIOR_FLAVOR_PSP_AND_USD},
        {SAI_MY_SID_ENTRY_ATTR_ENDPOINT_BEHAVIOR, SAI_MY_SID_ENTRY_ENDPOINT_BEHAVIOR_T},
    };
    EXPECT_EQ(set_attributes, expected);

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
