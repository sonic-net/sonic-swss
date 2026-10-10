#include "mock_orch_test.h"
#include "mock_sai_api.h"

#define protected public
#include "orch.h"
#undef protected

#include "vrforch.h"
#include "vxlanorch.h"
#include "common/vxlan_ut_helpers.h"

extern VRFOrch *gVrfOrch;
extern sai_object_id_t gVirtualRouterId;

namespace vrforch_test
{
using namespace std;
using namespace mock_orch_test;
using namespace testing;
using namespace swss;

DEFINE_SAI_GENERIC_API_MOCK_WITH_SET(virtual_router, virtual_router);

class VrfOrchTest : public MockOrchTest
{
  protected:
    EvpnNvoOrch *m_evpn_nvo_orch = nullptr;
    VxlanTunnelMapOrch *m_vxlan_tunnel_map_orch = nullptr;
    VxlanVrfMapOrch *m_vxlan_vrf_map_orch = nullptr;

    void PostSetUp() override
    {
        INIT_SAI_API_MOCK(virtual_router);
        MockSaiApis();

        m_vxlan_tunnel_map_orch = new VxlanTunnelMapOrch(
            m_app_db.get(), APP_VXLAN_TUNNEL_MAP_TABLE_NAME);
        gDirectory.set(m_vxlan_tunnel_map_orch);
        ut_orch_list.push_back((Orch **)&m_vxlan_tunnel_map_orch);

        m_vxlan_vrf_map_orch = new VxlanVrfMapOrch(
            m_app_db.get(), APP_VXLAN_VRF_TABLE_NAME);
        gDirectory.set(m_vxlan_vrf_map_orch);
        ut_orch_list.push_back((Orch **)&m_vxlan_vrf_map_orch);
    }

    void PreTearDown() override
    {
        delete m_evpn_nvo_orch;
        m_evpn_nvo_orch = nullptr;
        RestoreSaiApis();
        DEINIT_SAI_API_MOCK(virtual_router);
    }

    void provisionEvpnVtep(bool create_vtep = true)
    {
        m_evpn_nvo_orch = new EvpnNvoOrch(m_app_db.get(), APP_VXLAN_EVPN_NVO_TABLE_NAME);
        gDirectory.set(m_evpn_nvo_orch);
        if (create_vtep)
        {
            vxlan_ut_helpers::setUpVxlanPort("2.2.2.2", 0x111);
        }
    }

    void addVrf(const string& name, uint32_t vni = 0)
    {
        auto consumer = dynamic_cast<Consumer *>(gVrfOrch->getExecutor(APP_VRF_TABLE_NAME));
        ASSERT_NE(consumer, nullptr);

        vector<FieldValueTuple> fields;
        if (vni != 0)
        {
            fields.emplace_back("vni", to_string(vni));
        }
        consumer->addToSync({{name, "SET", fields}});
        static_cast<Orch *>(gVrfOrch)->doTask();
        ASSERT_TRUE(gVrfOrch->isVRFexists(name));
    }

    Consumer *vxlanVrfConsumer()
    {
        return dynamic_cast<Consumer *>(
            m_vxlan_vrf_map_orch->getExecutor(APP_VXLAN_VRF_TABLE_NAME));
    }
};

TEST_F(VrfOrchTest, DefaultVrfUsesSwitchDefaultVirtualRouter)
{
    auto consumer = dynamic_cast<Consumer *>(gVrfOrch->getExecutor(APP_VRF_TABLE_NAME));
    ASSERT_NE(consumer, nullptr);
    EXPECT_CALL(*mock_sai_virtual_router_api, create_virtual_router(_, _, _, _)).Times(0);

    deque<KeyOpFieldsValuesTuple> entries = {
        {"default", "SET", {{"v4", "false"}}},
    };
    consumer->addToSync(entries);
    static_cast<Orch *>(gVrfOrch)->doTask();

    EXPECT_TRUE(gVrfOrch->isVRFexists("default"));
    EXPECT_EQ(gVrfOrch->getVRFid("default"), gVirtualRouterId);
    EXPECT_EQ(gVrfOrch->getVrfRefCount("default"), 0);
}

TEST_F(VrfOrchTest, DefaultVrfDeleteOnlyRemovesLogicalEntry)
{
    auto consumer = dynamic_cast<Consumer *>(gVrfOrch->getExecutor(APP_VRF_TABLE_NAME));
    ASSERT_NE(consumer, nullptr);
    EXPECT_CALL(*mock_sai_virtual_router_api, create_virtual_router(_, _, _, _)).Times(0);
    EXPECT_CALL(*mock_sai_virtual_router_api, remove_virtual_router(_)).Times(0);

    deque<KeyOpFieldsValuesTuple> entries = {
        {"default", "SET", {}},
    };
    consumer->addToSync(entries);
    static_cast<Orch *>(gVrfOrch)->doTask();
    ASSERT_TRUE(gVrfOrch->isVRFexists("default"));

    entries = {
        {"default", "DEL", {}},
    };
    consumer->addToSync(entries);
    static_cast<Orch *>(gVrfOrch)->doTask();

    EXPECT_FALSE(gVrfOrch->isVRFexists("default"));

    Table state_table(m_state_db.get(), STATE_VRF_OBJECT_TABLE_NAME);
    vector<FieldValueTuple> state;
    EXPECT_FALSE(state_table.get("default", state));

    // Re-adding must still resolve to the same switch-owned default VR.
    entries = {
        {"default", "SET", {}},
    };
    consumer->addToSync(entries);
    static_cast<Orch *>(gVrfOrch)->doTask();
    EXPECT_TRUE(gVrfOrch->isVRFexists("default"));
    EXPECT_EQ(gVrfOrch->getVRFid("default"), gVirtualRouterId);
}

TEST_F(VrfOrchTest, DefaultVrfLiveVniReplacementIsRejected)
{
    provisionEvpnVtep();
    auto consumer = dynamic_cast<Consumer *>(gVrfOrch->getExecutor(APP_VRF_TABLE_NAME));
    ASSERT_NE(consumer, nullptr);
    EXPECT_CALL(*mock_sai_virtual_router_api, create_virtual_router(_, _, _, _)).Times(0);

    deque<KeyOpFieldsValuesTuple> entries = {
        {"default", "SET", {{"vni", "1000"}}},
    };
    consumer->addToSync(entries);
    static_cast<Orch *>(gVrfOrch)->doTask();
    ASSERT_EQ(gVrfOrch->getVRFmappedVNI("default"), 1000u);
    ASSERT_TRUE(gVrfOrch->isL3VniVlan(1000));

    // A live SET to another VNI is invalid and must be consumed without
    // disturbing the programmed mapping.
    entries = {
        {"default", "SET", {{"vni", "2000"}}},
    };
    consumer->addToSync(entries);
    static_cast<Orch *>(gVrfOrch)->doTask();
    EXPECT_TRUE(consumer->m_toSync.empty());
    EXPECT_EQ(gVrfOrch->getVRFmappedVNI("default"), 1000u);
    EXPECT_TRUE(gVrfOrch->isL3VniVlan(1000));
    EXPECT_FALSE(gVrfOrch->isL3VniVlan(2000));
}

TEST_F(VrfOrchTest, DefaultVrfPendingVniCanBeDeleted)
{
    provisionEvpnVtep(false);

    auto consumer = dynamic_cast<Consumer *>(gVrfOrch->getExecutor(APP_VRF_TABLE_NAME));
    ASSERT_NE(consumer, nullptr);
    EXPECT_CALL(*mock_sai_virtual_router_api, create_virtual_router(_, _, _, _)).Times(0);
    EXPECT_CALL(*mock_sai_virtual_router_api, remove_virtual_router(_)).Times(0);

    deque<KeyOpFieldsValuesTuple> entries = {
        {"default", "SET", {{"vni", "3000"}}},
    };
    consumer->addToSync(entries);
    static_cast<Orch *>(gVrfOrch)->doTask();

    // The logical object is ready, but the VNI SET remains pending for VTEP.
    EXPECT_TRUE(gVrfOrch->isVRFexists("default"));
    EXPECT_EQ(gVrfOrch->getVRFmappedVNI("default"), 0u);
    ASSERT_EQ(consumer->m_toSync.size(), 1u);

    // A later DEL must replace the pending SET and remove the logical object.
    entries = {
        {"default", "DEL", {}},
    };
    consumer->addToSync(entries);
    static_cast<Orch *>(gVrfOrch)->doTask();

    EXPECT_TRUE(consumer->m_toSync.empty());
    EXPECT_FALSE(gVrfOrch->isVRFexists("default"));
    EXPECT_FALSE(gVrfOrch->isL3VniVlan(3000));

    Table state_table(m_state_db.get(), STATE_VRF_OBJECT_TABLE_NAME);
    vector<FieldValueTuple> state;
    EXPECT_FALSE(state_table.get("default", state));
}

TEST_F(VrfOrchTest, DefaultVrfUpdateWithoutVniPreservesMapping)
{
    provisionEvpnVtep();
    auto consumer = dynamic_cast<Consumer *>(gVrfOrch->getExecutor(APP_VRF_TABLE_NAME));
    ASSERT_NE(consumer, nullptr);
    EXPECT_CALL(*mock_sai_virtual_router_api, create_virtual_router(_, _, _, _)).Times(0);
    EXPECT_CALL(*mock_sai_virtual_router_api, remove_virtual_router(_)).Times(0);

    deque<KeyOpFieldsValuesTuple> entries = {
        {"default", "SET", {{"vni", "4000"}}},
    };
    consumer->addToSync(entries);
    static_cast<Orch *>(gVrfOrch)->doTask();
    ASSERT_EQ(gVrfOrch->getVRFmappedVNI("default"), 4000u);
    ASSERT_TRUE(gVrfOrch->isL3VniVlan(4000));

    entries = {
        {"default", "SET", {}},
    };
    consumer->addToSync(entries);
    static_cast<Orch *>(gVrfOrch)->doTask();

    EXPECT_TRUE(consumer->m_toSync.empty());
    EXPECT_TRUE(gVrfOrch->isVRFexists("default"));
    EXPECT_EQ(gVrfOrch->getVRFid("default"), gVirtualRouterId);
    EXPECT_EQ(gVrfOrch->getVRFmappedVNI("default"), 4000u);
    EXPECT_TRUE(gVrfOrch->isL3VniVlan(4000));
}

TEST_F(VrfOrchTest, DefaultVrfExplicitZeroRemovesAndAllowsReadd)
{
    provisionEvpnVtep();
    auto consumer = dynamic_cast<Consumer *>(gVrfOrch->getExecutor(APP_VRF_TABLE_NAME));
    ASSERT_NE(consumer, nullptr);
    EXPECT_CALL(*mock_sai_virtual_router_api, create_virtual_router(_, _, _, _)).Times(0);
    EXPECT_CALL(*mock_sai_virtual_router_api, remove_virtual_router(_)).Times(0);

    deque<KeyOpFieldsValuesTuple> entries = {
        {"default", "SET", {{"vni", "4100"}}},
    };
    consumer->addToSync(entries);
    static_cast<Orch *>(gVrfOrch)->doTask();
    ASSERT_EQ(gVrfOrch->getVRFmappedVNI("default"), 4100u);

    entries = {
        {"default", "SET", {{"vni", "0"}}},
    };
    consumer->addToSync(entries);
    static_cast<Orch *>(gVrfOrch)->doTask();

    EXPECT_TRUE(consumer->m_toSync.empty());
    EXPECT_TRUE(gVrfOrch->isVRFexists("default"));
    EXPECT_EQ(gVrfOrch->getVRFid("default"), gVirtualRouterId);
    EXPECT_EQ(gVrfOrch->getVRFmappedVNI("default"), 0u);
    EXPECT_FALSE(gVrfOrch->isL3VniVlan(4100));

    entries = {
        {"default", "SET", {{"vni", "4200"}}},
    };
    consumer->addToSync(entries);
    static_cast<Orch *>(gVrfOrch)->doTask();

    EXPECT_TRUE(consumer->m_toSync.empty());
    EXPECT_EQ(gVrfOrch->getVRFmappedVNI("default"), 4200u);
    EXPECT_TRUE(gVrfOrch->isL3VniVlan(4200));
}

TEST_F(VrfOrchTest, DefaultVrfSameVniReplayIsIdempotent)
{
    provisionEvpnVtep();
    auto consumer = dynamic_cast<Consumer *>(gVrfOrch->getExecutor(APP_VRF_TABLE_NAME));
    ASSERT_NE(consumer, nullptr);
    EXPECT_CALL(*mock_sai_virtual_router_api, create_virtual_router(_, _, _, _)).Times(0);

    deque<KeyOpFieldsValuesTuple> entries = {
        {"default", "SET", {{"vni", "4300"}}},
        {"default", "SET", {{"vni", "4300"}}},
    };
    consumer->addToSync(entries);
    static_cast<Orch *>(gVrfOrch)->doTask();

    EXPECT_TRUE(consumer->m_toSync.empty());
    EXPECT_EQ(gVrfOrch->getVRFmappedVNI("default"), 4300u);
    EXPECT_TRUE(gVrfOrch->isL3VniVlan(4300));
}

TEST_F(VrfOrchTest, NamedVrfVniLifecycleMatchesDefaultVrf)
{
    provisionEvpnVtep();
    auto consumer = dynamic_cast<Consumer *>(gVrfOrch->getExecutor(APP_VRF_TABLE_NAME));
    ASSERT_NE(consumer, nullptr);
    EXPECT_CALL(*mock_sai_virtual_router_api, create_virtual_router(_, _, _, _)).Times(1);
    EXPECT_CALL(*mock_sai_virtual_router_api, remove_virtual_router(_)).Times(0);

    deque<KeyOpFieldsValuesTuple> entries = {
        {"VrfTenant", "SET", {{"vni", "4400"}}},
    };
    consumer->addToSync(entries);
    static_cast<Orch *>(gVrfOrch)->doTask();
    ASSERT_EQ(gVrfOrch->getVRFmappedVNI("VrfTenant"), 4400u);

    entries = {
        {"VrfTenant", "SET", {{"v4", "false"}}},
    };
    consumer->addToSync(entries);
    static_cast<Orch *>(gVrfOrch)->doTask();
    EXPECT_EQ(gVrfOrch->getVRFmappedVNI("VrfTenant"), 4400u);

    entries = {
        {"VrfTenant", "SET", {{"vni", "0"}}},
    };
    consumer->addToSync(entries);
    static_cast<Orch *>(gVrfOrch)->doTask();
    EXPECT_EQ(gVrfOrch->getVRFmappedVNI("VrfTenant"), 0u);
    EXPECT_FALSE(gVrfOrch->isL3VniVlan(4400));

    entries = {
        {"VrfTenant", "SET", {{"vni", "4500"}}},
    };
    consumer->addToSync(entries);
    static_cast<Orch *>(gVrfOrch)->doTask();
    EXPECT_TRUE(consumer->m_toSync.empty());
    EXPECT_EQ(gVrfOrch->getVRFmappedVNI("VrfTenant"), 4500u);
    EXPECT_TRUE(gVrfOrch->isL3VniVlan(4500));
}

TEST_F(VrfOrchTest, DefaultVrfRejectsVniOwnedByTenantVrf)
{
    provisionEvpnVtep();
    auto consumer = dynamic_cast<Consumer *>(gVrfOrch->getExecutor(APP_VRF_TABLE_NAME));
    ASSERT_NE(consumer, nullptr);

    EXPECT_CALL(*mock_sai_virtual_router_api, create_virtual_router(_, _, _, _))
        .Times(1);

    deque<KeyOpFieldsValuesTuple> entries = {
        {"VrfTenant", "SET", {{"vni", "4500"}}},
    };
    consumer->addToSync(entries);
    static_cast<Orch *>(gVrfOrch)->doTask();
    ASSERT_EQ(gVrfOrch->getVRFmappedVNI("VrfTenant"), 4500u);

    entries = {
        {"default", "SET", {{"vni", "4500"}}},
    };
    consumer->addToSync(entries);
    static_cast<Orch *>(gVrfOrch)->doTask();

    EXPECT_TRUE(consumer->m_toSync.empty());
    EXPECT_FALSE(gVrfOrch->isVRFexists("default"));
    EXPECT_EQ(gVrfOrch->getVRFmappedVNI("VrfTenant"), 4500u);
    EXPECT_EQ(gVrfOrch->getVRFmappedVNI("default"), 0u);
}

TEST_F(VrfOrchTest, TenantVrfRejectsVniOwnedByDefaultVrf)
{
    provisionEvpnVtep();
    auto consumer = dynamic_cast<Consumer *>(gVrfOrch->getExecutor(APP_VRF_TABLE_NAME));
    ASSERT_NE(consumer, nullptr);
    EXPECT_CALL(*mock_sai_virtual_router_api, create_virtual_router(_, _, _, _)).Times(0);

    deque<KeyOpFieldsValuesTuple> entries = {
        {"default", "SET", {{"vni", "4600"}}},
    };
    consumer->addToSync(entries);
    static_cast<Orch *>(gVrfOrch)->doTask();
    ASSERT_EQ(gVrfOrch->getVRFmappedVNI("default"), 4600u);

    entries = {
        {"VrfTenant", "SET", {{"vni", "4600"}}},
    };
    consumer->addToSync(entries);
    static_cast<Orch *>(gVrfOrch)->doTask();

    EXPECT_TRUE(consumer->m_toSync.empty());
    EXPECT_FALSE(gVrfOrch->isVRFexists("VrfTenant"));
    EXPECT_EQ(gVrfOrch->getVRFmappedVNI("default"), 4600u);
    EXPECT_EQ(gVrfOrch->getVRFmappedVNI("VrfTenant"), 0u);
}

TEST_F(VrfOrchTest, DefaultVrfDeleteWaitsForLogicalReferences)
{
    auto consumer = dynamic_cast<Consumer *>(gVrfOrch->getExecutor(APP_VRF_TABLE_NAME));
    ASSERT_NE(consumer, nullptr);
    EXPECT_CALL(*mock_sai_virtual_router_api, create_virtual_router(_, _, _, _)).Times(0);
    EXPECT_CALL(*mock_sai_virtual_router_api, remove_virtual_router(_)).Times(0);

    deque<KeyOpFieldsValuesTuple> entries = {
        {"default", "SET", {}},
    };
    consumer->addToSync(entries);
    static_cast<Orch *>(gVrfOrch)->doTask();
    ASSERT_TRUE(gVrfOrch->isVRFexists("default"));

    gVrfOrch->increaseVrfRefCount("default");
    entries = {
        {"default", "DEL", {}},
    };
    consumer->addToSync(entries);
    static_cast<Orch *>(gVrfOrch)->doTask();

    EXPECT_TRUE(gVrfOrch->isVRFexists("default"));
    EXPECT_EQ(gVrfOrch->getVrfRefCount("default"), 1);
    ASSERT_EQ(consumer->m_toSync.size(), 1u);

    gVrfOrch->decreaseVrfRefCount("default");
    static_cast<Orch *>(gVrfOrch)->doTask();

    EXPECT_TRUE(consumer->m_toSync.empty());
    EXPECT_FALSE(gVrfOrch->isVRFexists("default"));
}

TEST_F(VrfOrchTest, ExistingDefaultVrfIgnoresNonVniAttributes)
{
    addVrf("default");
    auto consumer = dynamic_cast<Consumer *>(gVrfOrch->getExecutor(APP_VRF_TABLE_NAME));
    ASSERT_NE(consumer, nullptr);
    EXPECT_CALL(*mock_sai_virtual_router_api, set_virtual_router_attribute(_, _)).Times(0);

    consumer->addToSync({{"default", "SET", {{"v4", "false"}}}});
    static_cast<Orch *>(gVrfOrch)->doTask();

    EXPECT_TRUE(consumer->m_toSync.empty());
    EXPECT_TRUE(gVrfOrch->isVRFexists("default"));
    EXPECT_EQ(gVrfOrch->getVRFid("default"), gVirtualRouterId);
}

TEST_F(VrfOrchTest, ExistingDefaultVrfVniWaitsForEvpnVtep)
{
    addVrf("default");
    provisionEvpnVtep(false);
    auto consumer = dynamic_cast<Consumer *>(gVrfOrch->getExecutor(APP_VRF_TABLE_NAME));
    ASSERT_NE(consumer, nullptr);

    consumer->addToSync({{"default", "SET", {{"vni", "4900"}}}});
    static_cast<Orch *>(gVrfOrch)->doTask();

    EXPECT_TRUE(gVrfOrch->isVRFexists("default"));
    EXPECT_EQ(gVrfOrch->getVRFmappedVNI("default"), 0u);
    ASSERT_EQ(consumer->m_toSync.size(), 1u);
}

TEST_F(VrfOrchTest, NamedVrfAttributeUpdateSaiFailureIsConsumed)
{
    addVrf("VrfTenant");
    auto consumer = dynamic_cast<Consumer *>(gVrfOrch->getExecutor(APP_VRF_TABLE_NAME));
    ASSERT_NE(consumer, nullptr);

    EXPECT_CALL(*mock_sai_virtual_router_api, set_virtual_router_attribute(_, _))
        .WillOnce(Return(SAI_STATUS_FAILURE));

    consumer->addToSync({{"VrfTenant", "SET", {{"v4", "false"}}}});
    static_cast<Orch *>(gVrfOrch)->doTask();

    EXPECT_TRUE(consumer->m_toSync.empty());
    EXPECT_TRUE(gVrfOrch->isVRFexists("VrfTenant"));
}

TEST_F(VrfOrchTest, VxlanVrfMapRejectsDuplicateDefaultAndVniOwnership)
{
    provisionEvpnVtep();
    addVrf("default");

    auto consumer = vxlanVrfConsumer();
    ASSERT_NE(consumer, nullptr);

    const string first_map = "EVPN_2.2.2.2:map_default_5000";
    consumer->addToSync({{first_map, "SET", {{"vni", "5000"}, {"vrf", "default"}}}});
    static_cast<Orch *>(m_vxlan_vrf_map_orch)->doTask();
    ASSERT_TRUE(consumer->m_toSync.empty());
    ASSERT_TRUE(m_vxlan_vrf_map_orch->isVrfMapExists(first_map));

    // A second map for the default VRF is rejected even if it uses another VNI.
    const string duplicate_default = "EVPN_2.2.2.2:map_default_5001";
    consumer->addToSync({{duplicate_default, "SET", {{"vni", "5001"}, {"vrf", "default"}}}});
    static_cast<Orch *>(m_vxlan_vrf_map_orch)->doTask();
    EXPECT_TRUE(consumer->m_toSync.empty());
    EXPECT_FALSE(m_vxlan_vrf_map_orch->isVrfMapExists(duplicate_default));

    addVrf("VrfTenant");

    // The VNI owned by the default VRF cannot be assigned to another VRF.
    const string duplicate_vni = "EVPN_2.2.2.2:map_tenant_5000";
    consumer->addToSync({{duplicate_vni, "SET", {{"vni", "5000"}, {"vrf", "VrfTenant"}}}});
    static_cast<Orch *>(m_vxlan_vrf_map_orch)->doTask();
    EXPECT_TRUE(consumer->m_toSync.empty());
    EXPECT_FALSE(m_vxlan_vrf_map_orch->isVrfMapExists(duplicate_vni));
    EXPECT_TRUE(m_vxlan_vrf_map_orch->isVrfMapExists(first_map));
}

TEST_F(VrfOrchTest, VxlanVrfMapDeleteUsesStoredDefaultVrfName)
{
    provisionEvpnVtep();
    addVrf("default");

    auto consumer = vxlanVrfConsumer();
    ASSERT_NE(consumer, nullptr);

    // The map name intentionally contains no "Vrf" token. Deletion must use
    // the VRF name saved in the table entry rather than parsing the key.
    const string map_key = "EVPN_2.2.2.2:map_default_5100";
    consumer->addToSync({{map_key, "SET", {{"vni", "5100"}, {"vrf", "default"}}}});
    static_cast<Orch *>(m_vxlan_vrf_map_orch)->doTask();
    ASSERT_TRUE(consumer->m_toSync.empty());
    ASSERT_TRUE(m_vxlan_vrf_map_orch->isVrfMapExists(map_key));
    ASSERT_EQ(gVrfOrch->getVrfRefCount("default"), 2);

    consumer->addToSync({{map_key, "DEL", {}}});
    static_cast<Orch *>(m_vxlan_vrf_map_orch)->doTask();

    EXPECT_TRUE(consumer->m_toSync.empty());
    EXPECT_FALSE(m_vxlan_vrf_map_orch->isVrfMapExists(map_key));
    EXPECT_EQ(gVrfOrch->getVrfRefCount("default"), 0);
}

} // namespace vrforch_test
