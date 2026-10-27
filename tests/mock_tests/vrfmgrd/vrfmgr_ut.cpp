#include "gtest/gtest.h"

#include "mock_table.h"
#include "redisutility.h"
#include "vrfmgr.h"
#include "warm_restart.h"

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
            CFG_VXLAN_EVPN_NVO_TABLE_NAME,
            CFG_VXLAN_TUNNEL_MAP_TABLE_NAME,
        };
        m_vrf_mgr = make_unique<TestableVrfMgr>(
            m_config_db.get(), m_app_db.get(), m_state_db.get(), tables);
        testing_db::resetOperationCounters();
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

    void addTunnelMap(const string &tunnel_name,
                      const string &map_name,
                      const string &vni)
    {
        const string key = tunnel_name + config_db_key_delimiter + map_name;
        Table table(m_config_db.get(), CFG_VXLAN_TUNNEL_MAP_TABLE_NAME);
        table.set(key, {{"vni", vni}, {"vlan", "Vlan" + vni}});
        process(CFG_VXLAN_TUNNEL_MAP_TABLE_NAME,
                key, SET_COMMAND, {{"vni", vni}, {"vlan", "Vlan" + vni}});
    }

    void removeTunnelMap(const string &tunnel_name, const string &map_name)
    {
        const string key = tunnel_name + config_db_key_delimiter + map_name;
        Table table(m_config_db.get(), CFG_VXLAN_TUNNEL_MAP_TABLE_NAME);
        table.del(key);
        process(CFG_VXLAN_TUNNEL_MAP_TABLE_NAME, key, DEL_COMMAND, {});
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

TEST_F(VrfMgrTest, UnrelatedTunnelMapDoesNotRepublishExistingVrfMap)
{
    addNvo("tunnel0");
    addTunnelMap("tunnel0", "map5000", "5000");
    addDefaultVrf("5000");

    testing_db::resetOperationCounters();
    addTunnelMap("tunnel0", "map6000", "6000");

    EXPECT_EQ(testing_db::getProducerSetCount(APP_VXLAN_VRF_TABLE_NAME), 0u);
    EXPECT_EQ(testing_db::getProducerDelCount(APP_VXLAN_VRF_TABLE_NAME), 0u);
}

TEST_F(VrfMgrTest, UnrelatedTunnelMapRemovalDoesNotRepublishExistingVrfMap)
{
    addNvo("tunnel0");
    addTunnelMap("tunnel0", "map5000", "5000");
    addTunnelMap("tunnel0", "map6000", "6000");
    addDefaultVrf("5000");

    testing_db::resetOperationCounters();
    removeTunnelMap("tunnel0", "map6000");

    EXPECT_EQ(testing_db::getTableGetKeysCount(CFG_VXLAN_TUNNEL_MAP_TABLE_NAME), 0u);
    EXPECT_EQ(testing_db::getTableGetCount(CFG_VXLAN_TUNNEL_MAP_TABLE_NAME), 0u);
    EXPECT_EQ(testing_db::getProducerSetCount(APP_VXLAN_VRF_TABLE_NAME), 0u);
    EXPECT_EQ(testing_db::getProducerDelCount(APP_VXLAN_VRF_TABLE_NAME), 0u);
}

TEST_F(VrfMgrTest, UnrelatedTunnelMapDoesNotDeleteUnpublishedVrfMap)
{
    addNvo("tunnel0");
    addDefaultVrf("5000");

    testing_db::resetOperationCounters();
    addTunnelMap("tunnel0", "map6000", "6000");

    EXPECT_EQ(testing_db::getProducerSetCount(APP_VXLAN_VRF_TABLE_NAME), 0u);
    EXPECT_EQ(testing_db::getProducerDelCount(APP_VXLAN_VRF_TABLE_NAME), 0u);
}

TEST_F(VrfMgrTest, VniFieldRemovalDoesNotLeaveStaleAppDbVni)
{
    addNvo("tunnel0");
    addTunnelMap("tunnel0", "map5000", "5000");
    addDefaultVrf("5000");

    Table config_vrf(m_config_db.get(), CFG_VRF_TABLE_NAME);
    config_vrf.hdel("default", "vni");
    config_vrf.hset("default", "v4", "false");
    process(CFG_VRF_TABLE_NAME, "default", SET_COMMAND, {{"v4", "false"}});

    Table app_vrf(m_app_db.get(), APP_VRF_TABLE_NAME);
    vector<FieldValueTuple> values;
    ASSERT_TRUE(app_vrf.get("default", values));
    const auto vni = fvsGetValue(values, "vni", true);
    ASSERT_TRUE(vni);
    EXPECT_EQ(vni.get(), "0");
}

TEST_F(VrfMgrTest, NamedVrfVniFieldRemovalDoesNotLeaveStaleAppDbVni)
{
    const string vrf_name = "VrfTenant";
    const string map_key = "tunnel0:evpn_map_5000_" + vrf_name;

    addNvo("tunnel0");
    addTunnelMap("tunnel0", "map5000", "5000");
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
    addTunnelMap("tunnel0", "map5000", "5000");
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

TEST_F(VrfMgrTest, TunnelMapEventDoesNotScanConfigDb)
{
    addNvo("tunnel0");
    addTunnelMap("tunnel0", "map5000", "5000");
    addDefaultVrf("5000");

    testing_db::resetOperationCounters();
    addTunnelMap("tunnel0", "map6000", "6000");

    EXPECT_EQ(testing_db::getTableGetKeysCount(CFG_VXLAN_TUNNEL_MAP_TABLE_NAME), 0u);
    EXPECT_EQ(testing_db::getTableGetCount(CFG_VXLAN_TUNNEL_MAP_TABLE_NAME), 0u);
    EXPECT_EQ(testing_db::getProducerSetCount(APP_VXLAN_VRF_TABLE_NAME), 0u);
    EXPECT_EQ(testing_db::getProducerDelCount(APP_VXLAN_VRF_TABLE_NAME), 0u);
}

TEST_F(VrfMgrTest, TunnelMapDeleteUsesReverseCacheWithoutConfigDbScan)
{
    addNvo("tunnel0");
    addTunnelMap("tunnel0", "map5000", "5000");
    addDefaultVrf("5000");

    testing_db::resetOperationCounters();
    removeTunnelMap("tunnel0", "map5000");

    EXPECT_EQ(testing_db::getTableGetKeysCount(CFG_VXLAN_TUNNEL_MAP_TABLE_NAME), 0u);
    EXPECT_EQ(testing_db::getTableGetCount(CFG_VXLAN_TUNNEL_MAP_TABLE_NAME), 0u);
    EXPECT_EQ(testing_db::getProducerSetCount(APP_VXLAN_VRF_TABLE_NAME), 0u);
    EXPECT_EQ(testing_db::getProducerDelCount(APP_VXLAN_VRF_TABLE_NAME), 1u);
}

TEST_F(VrfMgrTest, NvoArrivalReconcilesCachedTunnelMap)
{
    addTunnelMap("tunnel0", "map5000", "5000");
    addDefaultVrf("5000");

    testing_db::resetOperationCounters();
    addNvo("tunnel0");

    EXPECT_EQ(testing_db::getTableGetKeysCount(CFG_VXLAN_TUNNEL_MAP_TABLE_NAME), 0u);
    EXPECT_EQ(testing_db::getTableGetCount(CFG_VXLAN_TUNNEL_MAP_TABLE_NAME), 0u);
    EXPECT_EQ(testing_db::getProducerSetCount(APP_VXLAN_VRF_TABLE_NAME), 1u);
    EXPECT_EQ(testing_db::getProducerDelCount(APP_VXLAN_VRF_TABLE_NAME), 0u);
}

TEST_F(VrfMgrTest, TunnelMapCacheReferenceCountsDuplicateVniTunnelEntries)
{
    addNvo("tunnel0");
    addTunnelMap("tunnel0", "map5000-a", "5000");
    addTunnelMap("tunnel0", "map5000-b", "5000");
    addDefaultVrf("5000");

    testing_db::resetOperationCounters();
    removeTunnelMap("tunnel0", "map5000-a");

    EXPECT_EQ(testing_db::getProducerDelCount(APP_VXLAN_VRF_TABLE_NAME), 0u);
    EXPECT_EQ(testing_db::getTableGetKeysCount(CFG_VXLAN_TUNNEL_MAP_TABLE_NAME), 0u);

    removeTunnelMap("tunnel0", "map5000-b");

    EXPECT_EQ(testing_db::getProducerDelCount(APP_VXLAN_VRF_TABLE_NAME), 1u);
    EXPECT_EQ(testing_db::getTableGetKeysCount(CFG_VXLAN_TUNNEL_MAP_TABLE_NAME), 0u);
}

TEST_F(VrfMgrTest, TunnelMapEventOnlyReconcilesVrfUsingChangedVni)
{
    const string default_key = "tunnel0:evpn_map_5000_default";
    const string tenant_key = "tunnel0:evpn_map_6000_VrfTenant";

    addNvo("tunnel0");
    addTunnelMap("tunnel0", "map5000", "5000");
    addTunnelMap("tunnel0", "map6000", "6000");
    addDefaultVrf("5000");
    addVrf("VrfTenant", "6000");

    Table app_vxlan_vrf(m_app_db.get(), APP_VXLAN_VRF_TABLE_NAME);
    vector<FieldValueTuple> values;
    ASSERT_TRUE(app_vxlan_vrf.get(default_key, values));
    values.clear();
    ASSERT_TRUE(app_vxlan_vrf.get(tenant_key, values));

    testing_db::resetOperationCounters();
    removeTunnelMap("tunnel0", "map5000");

    EXPECT_EQ(testing_db::getTableGetKeysCount(CFG_VXLAN_TUNNEL_MAP_TABLE_NAME), 0u);
    EXPECT_EQ(testing_db::getTableGetCount(CFG_VXLAN_TUNNEL_MAP_TABLE_NAME), 0u);
    EXPECT_EQ(testing_db::getProducerSetCount(APP_VXLAN_VRF_TABLE_NAME), 0u);
    EXPECT_EQ(testing_db::getProducerDelCount(APP_VXLAN_VRF_TABLE_NAME), 1u);
    values.clear();
    EXPECT_FALSE(app_vxlan_vrf.get(default_key, values));
    values.clear();
    EXPECT_TRUE(app_vxlan_vrf.get(tenant_key, values));

    testing_db::resetOperationCounters();
    addTunnelMap("tunnel0", "map5000", "5000");

    EXPECT_EQ(testing_db::getTableGetKeysCount(CFG_VXLAN_TUNNEL_MAP_TABLE_NAME), 0u);
    EXPECT_EQ(testing_db::getTableGetCount(CFG_VXLAN_TUNNEL_MAP_TABLE_NAME), 0u);
    EXPECT_EQ(testing_db::getProducerSetCount(APP_VXLAN_VRF_TABLE_NAME), 1u);
    EXPECT_EQ(testing_db::getProducerDelCount(APP_VXLAN_VRF_TABLE_NAME), 0u);
    values.clear();
    EXPECT_TRUE(app_vxlan_vrf.get(default_key, values));
    values.clear();
    EXPECT_TRUE(app_vxlan_vrf.get(tenant_key, values));
}

} // namespace vrfmgr_ut
