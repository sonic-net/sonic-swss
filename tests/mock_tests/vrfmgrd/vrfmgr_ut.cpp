#include "gtest/gtest.h"

#include "mock_table.h"
#include "redisutility.h"
#include "vrfmgr.h"
#include "warm_restart.h"

extern std::vector<std::string> mockCallArgs;

namespace vrfmgr_ut
{
using namespace std;
using namespace swss;

class TestableVrfMgr : public VrfMgr
{
  public:
    using VrfMgr::VrfMgr;
    using Orch::getExecutor;
};

class VrfMgrTest : public ::testing::Test
{
  protected:
    shared_ptr<DBConnector> m_app_db;
    shared_ptr<DBConnector> m_config_db;
    shared_ptr<DBConnector> m_state_db;
    unique_ptr<TestableVrfMgr> m_vrf_mgr;

    void SetUp() override
    {
        testing_db::reset();
        m_app_db = make_shared<DBConnector>("APPL_DB", 0);
        m_config_db = make_shared<DBConnector>("CONFIG_DB", 0);
        m_state_db = make_shared<DBConnector>("STATE_DB", 0);
        WarmStart::initialize("vrfmgrd", "swss");

        vector<string> tables = {
            CFG_VRF_TABLE_NAME,
            CFG_VNET_TABLE_NAME,
            CFG_VXLAN_EVPN_NVO_TABLE_NAME,
        };
        m_vrf_mgr = make_unique<TestableVrfMgr>(
            m_config_db.get(), m_app_db.get(), m_state_db.get(), tables);
        testing_db::resetOperationCounters();
        mockCallArgs.clear();
    }

    void process(const string &table_name,
                 const string &key,
                 const string &op,
                 const vector<FieldValueTuple> &fields)
    {
        auto consumer = dynamic_cast<Consumer *>(m_vrf_mgr->getExecutor(table_name));
        ASSERT_NE(consumer, nullptr);
        consumer->addToSync({key, op, fields});
        m_vrf_mgr->doTask();
        ASSERT_TRUE(consumer->m_toSync.empty());
    }

    void addNvo(const string &tunnel_name)
    {
        process(CFG_VXLAN_EVPN_NVO_TABLE_NAME,
                "nvo0", SET_COMMAND, {{"source_vtep", tunnel_name}});
    }

    void removeNvo()
    {
        process(CFG_VXLAN_EVPN_NVO_TABLE_NAME, "nvo0", DEL_COMMAND, {});
    }

    void addVrf(const string &vrf_name, const string &vni)
    {
        Table table(m_config_db.get(), CFG_VRF_TABLE_NAME);
        table.set(vrf_name, {{"vni", vni}});
        process(CFG_VRF_TABLE_NAME, vrf_name, SET_COMMAND, {{"vni", vni}});
    }

    void addDefaultVrf(const string &vni)
    {
        addVrf("default", vni);
    }
};

TEST_F(VrfMgrTest, VnetNamedDefaultRetainsLinuxVrfLifecycle)
{
    const vector<FieldValueTuple> fields = {
        {"vxlan_tunnel", "tunnel0"},
        {"vni", "5000"},
    };

    process(CFG_VNET_TABLE_NAME, "default", SET_COMMAND, fields);

    ASSERT_EQ(mockCallArgs.size(), 2u);
    EXPECT_EQ(mockCallArgs[0],
              "/sbin/ip link add \"default\" type vrf table 1001");
    EXPECT_EQ(mockCallArgs[1], "/sbin/ip link set \"default\" up");

    Table app_vnet(m_app_db.get(), APP_VNET_TABLE_NAME);
    vector<FieldValueTuple> values;
    ASSERT_TRUE(app_vnet.get("default", values));
    EXPECT_EQ(fvsGetValue(values, "vxlan_tunnel", true).get(), "tunnel0");
    EXPECT_EQ(fvsGetValue(values, "vni", true).get(), "5000");

    mockCallArgs.clear();
    process(CFG_VNET_TABLE_NAME, "default", DEL_COMMAND, {});

    ASSERT_EQ(mockCallArgs.size(), 1u);
    EXPECT_EQ(mockCallArgs[0], "/sbin/ip link del \"default\"");
    EXPECT_FALSE(app_vnet.get("default", values));
}

TEST_F(VrfMgrTest, DefaultVrfSkipsLinuxVrfCreation)
{
    addDefaultVrf("5000");

    EXPECT_TRUE(mockCallArgs.empty());
}

TEST_F(VrfMgrTest, VniRemovalWithoutNvoIsSuccessfulNoOp)
{
    addDefaultVrf("5000");

    Table config_vrf(m_config_db.get(), CFG_VRF_TABLE_NAME);
    config_vrf.hdel("default", "vni");
    config_vrf.hset("default", "v4", "false");

    testing_db::resetOperationCounters();
    process(CFG_VRF_TABLE_NAME, "default", SET_COMMAND, {{"v4", "false"}});

    EXPECT_EQ(testing_db::getProducerSetCount(APP_VXLAN_VRF_TABLE_NAME), 0u);
    EXPECT_EQ(testing_db::getProducerDelCount(APP_VXLAN_VRF_TABLE_NAME), 0u);

    Table app_vrf(m_app_db.get(), APP_VRF_TABLE_NAME);
    vector<FieldValueTuple> values;
    ASSERT_TRUE(app_vrf.get("default", values));
    const auto default_vni = fvsGetValue(values, "vni", true);
    ASSERT_TRUE(default_vni);
    EXPECT_EQ(default_vni.get(), "0");

    addVrf("VrfTenant", "5000");
    values.clear();
    ASSERT_TRUE(app_vrf.get("VrfTenant", values));
    const auto tenant_vni = fvsGetValue(values, "vni", true);
    ASSERT_TRUE(tenant_vni);
    EXPECT_EQ(tenant_vni.get(), "5000");
}

TEST_F(VrfMgrTest, VniFieldRemovalDeletesPublishedMapping)
{
    const string map_key = "tunnel0:evpn_map_5000_default";

    addNvo("tunnel0");
    addDefaultVrf("5000");

    Table app_vxlan_vrf(m_app_db.get(), APP_VXLAN_VRF_TABLE_NAME);
    vector<FieldValueTuple> values;
    ASSERT_TRUE(app_vxlan_vrf.get(map_key, values));

    Table config_vrf(m_config_db.get(), CFG_VRF_TABLE_NAME);
    config_vrf.hdel("default", "vni");
    config_vrf.hset("default", "v4", "false");
    process(CFG_VRF_TABLE_NAME, "default", SET_COMMAND, {{"v4", "false"}});

    Table app_vrf(m_app_db.get(), APP_VRF_TABLE_NAME);
    values.clear();
    ASSERT_TRUE(app_vrf.get("default", values));
    const auto vni = fvsGetValue(values, "vni", true);
    ASSERT_TRUE(vni);
    EXPECT_EQ(vni.get(), "0");

    values.clear();
    EXPECT_FALSE(app_vxlan_vrf.get(map_key, values));
}

TEST_F(VrfMgrTest, NamedVrfVniFieldRemovalDeletesPublishedMapping)
{
    const string vrf_name = "VrfTenant";
    const string map_key = "tunnel0:evpn_map_5000_" + vrf_name;

    addNvo("tunnel0");
    addVrf(vrf_name, "5000");

    Table app_vxlan_vrf(m_app_db.get(), APP_VXLAN_VRF_TABLE_NAME);
    vector<FieldValueTuple> values;
    ASSERT_TRUE(app_vxlan_vrf.get(map_key, values));

    Table config_vrf(m_config_db.get(), CFG_VRF_TABLE_NAME);
    config_vrf.hdel(vrf_name, "vni");
    config_vrf.hset(vrf_name, "v4", "false");
    process(CFG_VRF_TABLE_NAME, vrf_name, SET_COMMAND, {{"v4", "false"}});

    Table app_vrf(m_app_db.get(), APP_VRF_TABLE_NAME);
    values.clear();
    ASSERT_TRUE(app_vrf.get(vrf_name, values));
    const auto vni = fvsGetValue(values, "vni", true);
    ASSERT_TRUE(vni);
    EXPECT_EQ(vni.get(), "0");

    values.clear();
    EXPECT_FALSE(app_vxlan_vrf.get(map_key, values));
}

TEST_F(VrfMgrTest, PartialVrfUpdatePreservesConfiguredVni)
{
    addNvo("tunnel0");
    addDefaultVrf("5000");

    Table config_vrf(m_config_db.get(), CFG_VRF_TABLE_NAME);
    config_vrf.hset("default", "v4", "false");

    testing_db::resetOperationCounters();
    process(CFG_VRF_TABLE_NAME, "default", SET_COMMAND, {{"v4", "false"}});

    EXPECT_EQ(testing_db::getProducerSetCount(APP_VXLAN_VRF_TABLE_NAME), 0u);
    EXPECT_EQ(testing_db::getProducerDelCount(APP_VXLAN_VRF_TABLE_NAME), 0u);

    Table app_vrf(m_app_db.get(), APP_VRF_TABLE_NAME);
    vector<FieldValueTuple> values;
    ASSERT_TRUE(app_vrf.get("default", values));
    const auto vni = fvsGetValue(values, "vni", true);
    ASSERT_TRUE(vni);
    EXPECT_EQ(vni.get(), "5000");
}

TEST_F(VrfMgrTest, NvoArrivalPublishesExistingVrfMap)
{
    const string map_key = "tunnel0:evpn_map_5000_default";

    addDefaultVrf("5000");
    testing_db::resetOperationCounters();
    addNvo("tunnel0");

    EXPECT_EQ(testing_db::getProducerSetCount(APP_VXLAN_VRF_TABLE_NAME), 1u);
    EXPECT_EQ(testing_db::getProducerDelCount(APP_VXLAN_VRF_TABLE_NAME), 0u);

    Table app_vxlan_vrf(m_app_db.get(), APP_VXLAN_VRF_TABLE_NAME);
    vector<FieldValueTuple> values;
    EXPECT_TRUE(app_vxlan_vrf.get(map_key, values));
}

TEST_F(VrfMgrTest, ExistingNvoUpdateDoesNotMoveVrfMapping)
{
    const string old_key = "tunnel0:evpn_map_5000_default";
    const string new_key = "tunnel1:evpn_map_5000_default";

    addNvo("tunnel0");
    addDefaultVrf("5000");

    Table app_vxlan_vrf(m_app_db.get(), APP_VXLAN_VRF_TABLE_NAME);
    vector<FieldValueTuple> values;
    ASSERT_TRUE(app_vxlan_vrf.get(old_key, values));

    testing_db::resetOperationCounters();
    addNvo("tunnel1");

    EXPECT_EQ(testing_db::getProducerSetCount(APP_VXLAN_VRF_TABLE_NAME), 0u);
    EXPECT_EQ(testing_db::getProducerDelCount(APP_VXLAN_VRF_TABLE_NAME), 0u);
    values.clear();
    EXPECT_TRUE(app_vxlan_vrf.get(old_key, values));
    values.clear();
    EXPECT_FALSE(app_vxlan_vrf.get(new_key, values));
}

TEST_F(VrfMgrTest, NvoRemovalWithdrawsPublishedMapping)
{
    const string map_key = "tunnel0:evpn_map_5000_default";

    addNvo("tunnel0");
    addDefaultVrf("5000");

    testing_db::resetOperationCounters();
    removeNvo();

    EXPECT_EQ(testing_db::getProducerSetCount(APP_VXLAN_VRF_TABLE_NAME), 0u);
    EXPECT_EQ(testing_db::getProducerDelCount(APP_VXLAN_VRF_TABLE_NAME), 1u);

    Table app_vxlan_vrf(m_app_db.get(), APP_VXLAN_VRF_TABLE_NAME);
    vector<FieldValueTuple> values;
    EXPECT_FALSE(app_vxlan_vrf.get(map_key, values));
}

TEST_F(VrfMgrTest, InactiveNvoDeletionDoesNotWithdrawActiveNvoMappings)
{
    const string default_map_key = "tunnel0:evpn_map_5000_default";
    const string named_map_key = "tunnel0:evpn_map_6000_VrfTenant";

    addNvo("tunnel0");
    addDefaultVrf("5000");
    addVrf("VrfTenant", "6000");

    /*
     * vrfmgrd keeps the first NVO as the source of its VRF mappings. A later
     * DEL for another key must not tear down the active NVO's mappings.
     */
    process(CFG_VXLAN_EVPN_NVO_TABLE_NAME,
            "nvo1", SET_COMMAND, {{"source_vtep", "tunnel1"}});

    testing_db::resetOperationCounters();
    process(CFG_VXLAN_EVPN_NVO_TABLE_NAME, "nvo1", DEL_COMMAND, {});

    EXPECT_EQ(testing_db::getProducerDelCount(APP_VXLAN_VRF_TABLE_NAME), 0u);
    EXPECT_EQ(m_vrf_mgr->m_evpnVxlanTunnel, "tunnel0");

    Table app_vxlan_vrf(m_app_db.get(), APP_VXLAN_VRF_TABLE_NAME);
    vector<FieldValueTuple> values;
    EXPECT_TRUE(app_vxlan_vrf.get(default_map_key, values));
    values.clear();
    EXPECT_TRUE(app_vxlan_vrf.get(named_map_key, values));
}

TEST_F(VrfMgrTest, NumericVniSpellingIsCanonicalized)
{
    const string map_key = "tunnel0:evpn_map_5000_default";

    addNvo("tunnel0");
    addDefaultVrf("05000");

    Table app_vxlan_vrf(m_app_db.get(), APP_VXLAN_VRF_TABLE_NAME);
    vector<FieldValueTuple> values;
    EXPECT_TRUE(app_vxlan_vrf.get(map_key, values));

    Table app_vrf(m_app_db.get(), APP_VRF_TABLE_NAME);
    values.clear();
    ASSERT_TRUE(app_vrf.get("default", values));
    const auto app_vni = fvsGetValue(values, "vni", true);
    ASSERT_TRUE(app_vni);
    EXPECT_EQ(app_vni.get(), "5000");
}

TEST_F(VrfMgrTest, VniOwnershipMoveWaitsForOldOwnerRelease)
{
    const string old_map_key = "tunnel0:evpn_map_5000_default";
    const string new_map_key = "tunnel0:evpn_map_5000_VrfANew";

    addNvo("tunnel0");
    addDefaultVrf("5000");

    Table config_vrf(m_config_db.get(), CFG_VRF_TABLE_NAME);
    config_vrf.set("default", {{"vni", "0"}});
    config_vrf.set("VrfANew", {{"vni", "5000"}});

    auto consumer = dynamic_cast<Consumer *>(
        m_vrf_mgr->getExecutor(CFG_VRF_TABLE_NAME));
    ASSERT_NE(consumer, nullptr);

    // Consumer keys are ordered, so the new owner is examined before the old
    // owner's removal.  The new SET must remain pending rather than being
    // consumed as a permanent duplicate.
    consumer->addToSync({
        {"VrfANew", SET_COMMAND, {{"vni", "5000"}}},
        {"default", SET_COMMAND, {{"vni", "0"}}},
    });
    m_vrf_mgr->doTask();

    ASSERT_EQ(consumer->m_toSync.size(), 1u);
    EXPECT_NE(consumer->m_toSync.find("VrfANew"),
              consumer->m_toSync.end());

    Table app_vrf(m_app_db.get(), APP_VRF_TABLE_NAME);
    Table app_vxlan_vrf(m_app_db.get(), APP_VXLAN_VRF_TABLE_NAME);
    vector<FieldValueTuple> values;
    EXPECT_FALSE(app_vxlan_vrf.get(old_map_key, values));
    values.clear();
    EXPECT_FALSE(app_vxlan_vrf.get(new_map_key, values));
    values.clear();
    EXPECT_FALSE(app_vrf.get("VrfANew", values));

    m_vrf_mgr->doTask();

    EXPECT_TRUE(consumer->m_toSync.empty());
    values.clear();
    ASSERT_TRUE(app_vrf.get("VrfANew", values));
    const auto app_vni = fvsGetValue(values, "vni", true);
    ASSERT_TRUE(app_vni);
    EXPECT_EQ(app_vni.get(), "5000");
    values.clear();
    EXPECT_TRUE(app_vxlan_vrf.get(new_map_key, values));
}

TEST_F(VrfMgrTest, CoalescedDefaultVrfVniReplacementRemovesOldMappingBeforeAdd)
{
    const string old_map_key = "tunnel0:evpn_map_6000_default";
    const string new_map_key = "tunnel0:evpn_map_5000_default";

    addNvo("tunnel0");
    addDefaultVrf("6000");

    Table config_vrf(m_config_db.get(), CFG_VRF_TABLE_NAME);
    config_vrf.set("default", {{"vni", "5000"}});

    testing_db::resetOperationCounters();
    process(CFG_VRF_TABLE_NAME, "default", SET_COMMAND, {{"vni", "5000"}});

    EXPECT_EQ(testing_db::getProducerDelCount(APP_VXLAN_VRF_TABLE_NAME), 1u);
    EXPECT_EQ(testing_db::getProducerSetCount(APP_VXLAN_VRF_TABLE_NAME), 1u);

    Table app_vrf(m_app_db.get(), APP_VRF_TABLE_NAME);
    Table app_vxlan_vrf(m_app_db.get(), APP_VXLAN_VRF_TABLE_NAME);
    vector<FieldValueTuple> values;
    EXPECT_FALSE(app_vxlan_vrf.get(old_map_key, values));
    values.clear();
    EXPECT_TRUE(app_vxlan_vrf.get(new_map_key, values));
    values.clear();
    ASSERT_TRUE(app_vrf.get("default", values));
    const auto app_vni = fvsGetValue(values, "vni", true);
    ASSERT_TRUE(app_vni);
    EXPECT_EQ(app_vni.get(), "5000");
    EXPECT_EQ(m_vrf_mgr->getVRFmappedVNI("default"), 5000u);
}

TEST_F(VrfMgrTest, CoalescedNamedVrfVniReplacementRemovesOldMappingBeforeAdd)
{
    const string vrf_name = "VrfTenant";
    const string old_map_key = "tunnel0:evpn_map_6000_" + vrf_name;
    const string new_map_key = "tunnel0:evpn_map_5000_" + vrf_name;

    addNvo("tunnel0");
    addVrf(vrf_name, "6000");

    Table config_vrf(m_config_db.get(), CFG_VRF_TABLE_NAME);
    config_vrf.set(vrf_name, {{"vni", "5000"}});

    testing_db::resetOperationCounters();
    process(CFG_VRF_TABLE_NAME, vrf_name, SET_COMMAND, {{"vni", "5000"}});

    EXPECT_EQ(testing_db::getProducerDelCount(APP_VXLAN_VRF_TABLE_NAME), 1u);
    EXPECT_EQ(testing_db::getProducerSetCount(APP_VXLAN_VRF_TABLE_NAME), 1u);

    Table app_vrf(m_app_db.get(), APP_VRF_TABLE_NAME);
    Table app_vxlan_vrf(m_app_db.get(), APP_VXLAN_VRF_TABLE_NAME);
    vector<FieldValueTuple> values;
    EXPECT_FALSE(app_vxlan_vrf.get(old_map_key, values));
    values.clear();
    EXPECT_TRUE(app_vxlan_vrf.get(new_map_key, values));
    values.clear();
    ASSERT_TRUE(app_vrf.get(vrf_name, values));
    const auto app_vni = fvsGetValue(values, "vni", true);
    ASSERT_TRUE(app_vni);
    EXPECT_EQ(app_vni.get(), "5000");
    EXPECT_EQ(m_vrf_mgr->getVRFmappedVNI(vrf_name), 5000u);
}

} // namespace vrfmgr_ut
