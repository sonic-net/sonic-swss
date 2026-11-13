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
DEFINE_SAI_API_MOCK_SPECIFY_ENTRY_WITH_SET(route, route);

class VrfOrchTest : public MockOrchTest
{
  protected:
    EvpnNvoOrch *m_evpn_nvo_orch = nullptr;
    VxlanTunnelMapOrch *m_vxlan_tunnel_map_orch = nullptr;
    VxlanVrfMapOrch *m_vxlan_vrf_map_orch = nullptr;

    void PostSetUp() override
    {
        INIT_SAI_API_MOCK(virtual_router);
        INIT_SAI_API_MOCK(route);
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
        DEINIT_SAI_API_MOCK(route);
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

TEST_F(VrfOrchTest, DefaultVrfCoalescedVniReplacementUnmapsOldBeforeMappingNew)
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

    // APP_DB can coalesce an explicit CONFIG_DB delete followed by an add
    // into a single old-to-new SET. OrchAgent must preserve that supported
    // lifecycle by unmapping the old VNI before mapping the new one.
    entries = {
        {"default", "SET", {{"vni", "2000"}}},
    };
    consumer->addToSync(entries);
    static_cast<Orch *>(gVrfOrch)->doTask();
    EXPECT_TRUE(consumer->m_toSync.empty());
    EXPECT_EQ(gVrfOrch->getVRFmappedVNI("default"), 2000u);
    EXPECT_FALSE(gVrfOrch->isL3VniVlan(1000));
    EXPECT_TRUE(gVrfOrch->isL3VniVlan(2000));
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

TEST_F(VrfOrchTest, DefaultVrfVniLifecyclePreservesRouteOrchLinkLocalRoute)
{
    provisionEvpnVtep();
    auto consumer = dynamic_cast<Consumer *>(gVrfOrch->getExecutor(APP_VRF_TABLE_NAME));
    ASSERT_NE(consumer, nullptr);

    EXPECT_CALL(*mock_sai_virtual_router_api, create_virtual_router(_, _, _, _)).Times(0);
    EXPECT_CALL(*mock_sai_virtual_router_api, remove_virtual_router(_)).Times(0);
    EXPECT_CALL(*mock_sai_route_api, create_route_entry(_, _, _)).Times(0);
    EXPECT_CALL(*mock_sai_route_api, remove_route_entry(_)).Times(0);

    consumer->addToSync({{"default", "SET", {{"vni", "4250"}}}});
    static_cast<Orch *>(gVrfOrch)->doTask();
    ASSERT_TRUE(consumer->m_toSync.empty());
    ASSERT_TRUE(gVrfOrch->isVRFexists("default"));
    ASSERT_EQ(gVrfOrch->getVRFmappedVNI("default"), 4250u);

    consumer->addToSync({{"default", "SET", {{"vni", "0"}}}});
    static_cast<Orch *>(gVrfOrch)->doTask();
    ASSERT_TRUE(consumer->m_toSync.empty());
    ASSERT_TRUE(gVrfOrch->isVRFexists("default"));
    ASSERT_EQ(gVrfOrch->getVRFmappedVNI("default"), 0u);

    consumer->addToSync({{"default", "DEL", {}}});
    static_cast<Orch *>(gVrfOrch)->doTask();
    EXPECT_TRUE(consumer->m_toSync.empty());
    EXPECT_FALSE(gVrfOrch->isVRFexists("default"));
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

    // The same APP_DB coalescing rule applies to named VRFs: clean the old
    // mapping before accepting the final desired VNI.
    entries = {
        {"VrfTenant", "SET", {{"vni", "4550"}}},
    };
    consumer->addToSync(entries);
    static_cast<Orch *>(gVrfOrch)->doTask();
    EXPECT_TRUE(consumer->m_toSync.empty());
    EXPECT_EQ(gVrfOrch->getVRFmappedVNI("VrfTenant"), 4550u);
    EXPECT_FALSE(gVrfOrch->isL3VniVlan(4500));
    EXPECT_TRUE(gVrfOrch->isL3VniVlan(4550));
}

TEST_F(VrfOrchTest, DefaultVrfWaitsWhileVniIsOwnedByTenantVrf)
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

    EXPECT_EQ(consumer->m_toSync.size(), 1u);
    EXPECT_FALSE(gVrfOrch->isVRFexists("default"));
    EXPECT_EQ(gVrfOrch->getVRFmappedVNI("VrfTenant"), 4500u);
    EXPECT_EQ(gVrfOrch->getVRFmappedVNI("default"), 0u);

    consumer->addToSync({{"default", "DEL", {}}});
    static_cast<Orch *>(gVrfOrch)->doTask();
    EXPECT_TRUE(consumer->m_toSync.empty());
}

TEST_F(VrfOrchTest, TenantVrfWaitsWhileVniIsOwnedByDefaultVrf)
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

    EXPECT_EQ(consumer->m_toSync.size(), 1u);
    EXPECT_FALSE(gVrfOrch->isVRFexists("VrfTenant"));
    EXPECT_EQ(gVrfOrch->getVRFmappedVNI("default"), 4600u);
    EXPECT_EQ(gVrfOrch->getVRFmappedVNI("VrfTenant"), 0u);

    consumer->addToSync({{"VrfTenant", "DEL", {}}});
    static_cast<Orch *>(gVrfOrch)->doTask();
    EXPECT_TRUE(consumer->m_toSync.empty());
}

TEST_F(VrfOrchTest, VrfVniMoveWaitsForOldOwnerRelease)
{
    provisionEvpnVtep();
    auto consumer = dynamic_cast<Consumer *>(gVrfOrch->getExecutor(APP_VRF_TABLE_NAME));
    ASSERT_NE(consumer, nullptr);
    EXPECT_CALL(*mock_sai_virtual_router_api, create_virtual_router(_, _, _, _)).Times(1);

    consumer->addToSync({{"default", "SET", {{"vni", "4700"}}}});
    static_cast<Orch *>(gVrfOrch)->doTask();
    ASSERT_EQ(gVrfOrch->getVRFmappedVNI("default"), 4700u);

    // Lexicographic key ordering processes VrfTenant before default. The new
    // owner must remain pending until the old owner releases the VNI later in
    // the same pass; consuming the SET loses valid CONFIG_DB intent.
    consumer->addToSync({
        {"VrfTenant", "SET", {{"vni", "4700"}}},
        {"default", "SET", {{"vni", "0"}}},
    });
    static_cast<Orch *>(gVrfOrch)->doTask();

    EXPECT_EQ(gVrfOrch->getVRFmappedVNI("default"), 0u);
    EXPECT_FALSE(gVrfOrch->isVRFexists("VrfTenant"));
    ASSERT_EQ(consumer->m_toSync.size(), 1u);
    EXPECT_NE(consumer->m_toSync.find("VrfTenant"), consumer->m_toSync.end());

    static_cast<Orch *>(gVrfOrch)->doTask();

    EXPECT_TRUE(consumer->m_toSync.empty());
    EXPECT_EQ(gVrfOrch->getVRFmappedVNI("VrfTenant"), 4700u);
    EXPECT_TRUE(gVrfOrch->isL3VniVlan(4700));
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

    // A second map for the default VRF is retained for retry because it can be
    // the SET half of an explicit delete-then-add replacement whose old DEL
    // has not been processed yet.
    const string duplicate_default = "EVPN_2.2.2.2:map_default_5001";
    consumer->addToSync({{duplicate_default, "SET", {{"vni", "5001"}, {"vrf", "default"}}}});
    static_cast<Orch *>(m_vxlan_vrf_map_orch)->doTask();
    ASSERT_EQ(consumer->m_toSync.size(), 1u);
    EXPECT_FALSE(m_vxlan_vrf_map_orch->isVrfMapExists(duplicate_default));

    // A newer DEL cancels a replacement that is no longer desired.
    consumer->addToSync({{duplicate_default, "DEL", {}}});
    static_cast<Orch *>(m_vxlan_vrf_map_orch)->doTask();
    EXPECT_TRUE(consumer->m_toSync.empty());

    addVrf("VrfTenant");

    // A conflicting VNI remains pending. Normal configuration paths reject
    // permanent duplicates, while retry permits a valid SET-before-DEL move.
    const string duplicate_vni = "EVPN_2.2.2.2:map_tenant_5000";
    consumer->addToSync({{duplicate_vni, "SET", {{"vni", "5000"}, {"vrf", "VrfTenant"}}}});
    static_cast<Orch *>(m_vxlan_vrf_map_orch)->doTask();
    EXPECT_EQ(consumer->m_toSync.size(), 1u);
    EXPECT_FALSE(m_vxlan_vrf_map_orch->isVrfMapExists(duplicate_vni));
    EXPECT_TRUE(m_vxlan_vrf_map_orch->isVrfMapExists(first_map));

    consumer->addToSync({{duplicate_vni, "DEL", {}}});
    static_cast<Orch *>(m_vxlan_vrf_map_orch)->doTask();
    EXPECT_TRUE(consumer->m_toSync.empty());
}

TEST_F(VrfOrchTest, VxlanVrfMapSameKeyReplayIsIdempotent)
{
    provisionEvpnVtep();
    addVrf("default");

    auto consumer = vxlanVrfConsumer();
    ASSERT_NE(consumer, nullptr);

    const string map_key = "EVPN_2.2.2.2:map_default_5050";
    const deque<KeyOpFieldsValuesTuple> entry = {
        {map_key, "SET", {{"vni", "5050"}, {"vrf", "default"}}},
    };

    consumer->addToSync(entry);
    static_cast<Orch *>(m_vxlan_vrf_map_orch)->doTask();
    ASSERT_TRUE(consumer->m_toSync.empty());
    ASSERT_TRUE(m_vxlan_vrf_map_orch->isVrfMapExists(map_key));
    ASSERT_EQ(gVrfOrch->getVrfRefCount("default"), 2);
    auto tunnel = m_VxlanTunnelOrch->getVxlanTunnel("EVPN_2.2.2.2");
    ASSERT_EQ(tunnel->vlan_vrf_vni_count, 1u);

    // The exact same APP_DB key is handled by the initial existence check.
    // It must not reach duplicate-default validation or create another pair
    // of encap/decap references.
    consumer->addToSync(entry);
    static_cast<Orch *>(m_vxlan_vrf_map_orch)->doTask();

    EXPECT_TRUE(consumer->m_toSync.empty());
    EXPECT_TRUE(m_vxlan_vrf_map_orch->isVrfMapExists(map_key));
    EXPECT_EQ(gVrfOrch->getVrfRefCount("default"), 2);
    EXPECT_EQ(tunnel->vlan_vrf_vni_count, 1u);
}

TEST_F(VrfOrchTest, VxlanVrfMapReplacementWaitsForOldDefaultDelete)
{
    provisionEvpnVtep();
    addVrf("default");

    auto consumer = vxlanVrfConsumer();
    ASSERT_NE(consumer, nullptr);

    // Use a replacement key that sorts before the existing key. When both
    // operations are queued, Orch processes the replacement SET while the
    // old mapping still exists, followed by the old mapping DEL.
    const string old_map = "EVPN_2.2.2.2:map_default_6000";
    const string replacement_map = "EVPN_2.2.2.2:map_default_5000";

    consumer->addToSync(
        {{old_map, "SET", {{"vni", "6000"}, {"vrf", "default"}}}});
    static_cast<Orch *>(m_vxlan_vrf_map_orch)->doTask();
    ASSERT_TRUE(consumer->m_toSync.empty());
    ASSERT_TRUE(m_vxlan_vrf_map_orch->isVrfMapExists(old_map));

    consumer->addToSync({
        {replacement_map, "SET", {{"vni", "5000"}, {"vrf", "default"}}},
        {old_map, "DEL", {}},
    });
    static_cast<Orch *>(m_vxlan_vrf_map_orch)->doTask();

    // The transient duplicate must remain pending while the old DEL succeeds.
    ASSERT_FALSE(m_vxlan_vrf_map_orch->isVrfMapExists(old_map));
    ASSERT_FALSE(m_vxlan_vrf_map_orch->isVrfMapExists(replacement_map));
    ASSERT_EQ(consumer->m_toSync.size(), 1u);
    EXPECT_NE(consumer->m_toSync.find(replacement_map),
              consumer->m_toSync.end());

    static_cast<Orch *>(m_vxlan_vrf_map_orch)->doTask();

    EXPECT_TRUE(consumer->m_toSync.empty());
    EXPECT_TRUE(m_vxlan_vrf_map_orch->isVrfMapExists(replacement_map));
    EXPECT_EQ(gVrfOrch->getVrfRefCount("default"), 2);
}

TEST_F(VrfOrchTest, VxlanVrfMapReplacementWaitsForOldNamedVrfDelete)
{
    provisionEvpnVtep();
    addVrf("VrfTenant");

    auto consumer = vxlanVrfConsumer();
    ASSERT_NE(consumer, nullptr);

    // Named and default VRFs must use the same replacement lifecycle.  Make
    // the replacement key sort before the existing key so its SET is seen
    // before the old DEL in the same consumer pass.
    const string old_map = "EVPN_2.2.2.2:map_VrfTenant_6000";
    const string replacement_map = "EVPN_2.2.2.2:map_VrfTenant_5000";

    consumer->addToSync(
        {{old_map, "SET", {{"vni", "6000"}, {"vrf", "VrfTenant"}}}});
    static_cast<Orch *>(m_vxlan_vrf_map_orch)->doTask();
    ASSERT_TRUE(consumer->m_toSync.empty());
    ASSERT_TRUE(m_vxlan_vrf_map_orch->isVrfMapExists(old_map));

    consumer->addToSync({
        {replacement_map, "SET", {{"vni", "5000"}, {"vrf", "VrfTenant"}}},
        {old_map, "DEL", {}},
    });
    static_cast<Orch *>(m_vxlan_vrf_map_orch)->doTask();

    EXPECT_FALSE(m_vxlan_vrf_map_orch->isVrfMapExists(old_map));
    EXPECT_FALSE(m_vxlan_vrf_map_orch->isVrfMapExists(replacement_map));
    ASSERT_EQ(consumer->m_toSync.size(), 1u);
    EXPECT_NE(consumer->m_toSync.find(replacement_map),
              consumer->m_toSync.end());

    static_cast<Orch *>(m_vxlan_vrf_map_orch)->doTask();

    EXPECT_TRUE(consumer->m_toSync.empty());
    EXPECT_TRUE(m_vxlan_vrf_map_orch->isVrfMapExists(replacement_map));
    EXPECT_EQ(gVrfOrch->getVrfRefCount("VrfTenant"), 2);
}

TEST_F(VrfOrchTest, VxlanVrfMapMoveWaitsForOldOwnerDelete)
{
    provisionEvpnVtep();
    addVrf("default");
    addVrf("VrfTenant");

    auto consumer = vxlanVrfConsumer();
    ASSERT_NE(consumer, nullptr);

    const string old_map = "EVPN_2.2.2.2:map_5100_default";
    const string new_map = "EVPN_2.2.2.2:map_5100_VrfTenant";

    consumer->addToSync(
        {{old_map, "SET", {{"vni", "5100"}, {"vrf", "default"}}}});
    static_cast<Orch *>(m_vxlan_vrf_map_orch)->doTask();
    ASSERT_TRUE(consumer->m_toSync.empty());
    ASSERT_TRUE(m_vxlan_vrf_map_orch->isVrfMapExists(old_map));

    // The new key sorts before the old key. Keep its SET pending until the
    // old owner's DEL is processed, then accept it on the next pass.
    consumer->addToSync({
        {new_map, "SET", {{"vni", "5100"}, {"vrf", "VrfTenant"}}},
        {old_map, "DEL", {}},
    });
    static_cast<Orch *>(m_vxlan_vrf_map_orch)->doTask();

    EXPECT_FALSE(m_vxlan_vrf_map_orch->isVrfMapExists(old_map));
    EXPECT_FALSE(m_vxlan_vrf_map_orch->isVrfMapExists(new_map));
    ASSERT_EQ(consumer->m_toSync.size(), 1u);
    EXPECT_NE(consumer->m_toSync.find(new_map), consumer->m_toSync.end());

    static_cast<Orch *>(m_vxlan_vrf_map_orch)->doTask();

    EXPECT_TRUE(consumer->m_toSync.empty());
    EXPECT_TRUE(m_vxlan_vrf_map_orch->isVrfMapExists(new_map));
    EXPECT_EQ(gVrfOrch->getVrfRefCount("default"), 0);
    EXPECT_EQ(gVrfOrch->getVrfRefCount("VrfTenant"), 2);
}

TEST_F(VrfOrchTest, DefaultAndNamedVrfMapsShareTunnelLifetimeAccounting)
{
    provisionEvpnVtep();
    addVrf("default");
    addVrf("VrfTenant");

    auto consumer = vxlanVrfConsumer();
    ASSERT_NE(consumer, nullptr);
    auto tunnel = m_VxlanTunnelOrch->getVxlanTunnel("EVPN_2.2.2.2");

    // provisionEvpnVtep() adds a synthetic remote-DIP reference.  Release it
    // so this test isolates tunnel lifetime accounting for the two VRF maps.
    tunnel->updateRemoteEndPointIpRef("2.2.2.2", false);
    tunnel->eraseRemoteEndPoint("2.2.2.2");
    ASSERT_EQ(tunnel->getDipTunnelCnt(), 0);

    const string default_map = "EVPN_2.2.2.2:map_default_5200";
    const string tenant_map = "EVPN_2.2.2.2:map_VrfTenant_5300";
    consumer->addToSync({
        {default_map, "SET", {{"vni", "5200"}, {"vrf", "default"}}},
        {tenant_map, "SET", {{"vni", "5300"}, {"vrf", "VrfTenant"}}},
    });
    static_cast<Orch *>(m_vxlan_vrf_map_orch)->doTask();

    ASSERT_TRUE(consumer->m_toSync.empty());
    EXPECT_TRUE(m_vxlan_vrf_map_orch->isVrfMapExists(default_map));
    EXPECT_TRUE(m_vxlan_vrf_map_orch->isVrfMapExists(tenant_map));
    EXPECT_EQ(tunnel->vlan_vrf_vni_count, 2u);
    EXPECT_EQ(gVrfOrch->getVrfRefCount("default"), 2);
    EXPECT_EQ(gVrfOrch->getVrfRefCount("VrfTenant"), 2);

    consumer->addToSync({{default_map, "DEL", {}}});
    static_cast<Orch *>(m_vxlan_vrf_map_orch)->doTask();

    ASSERT_TRUE(consumer->m_toSync.empty());
    EXPECT_FALSE(m_vxlan_vrf_map_orch->isVrfMapExists(default_map));
    EXPECT_TRUE(m_vxlan_vrf_map_orch->isVrfMapExists(tenant_map));
    EXPECT_EQ(tunnel->vlan_vrf_vni_count, 1u);
    EXPECT_EQ(gVrfOrch->getVrfRefCount("default"), 0);
    EXPECT_EQ(gVrfOrch->getVrfRefCount("VrfTenant"), 2);

    consumer->addToSync({{tenant_map, "DEL", {}}});
    static_cast<Orch *>(m_vxlan_vrf_map_orch)->doTask();

    EXPECT_TRUE(consumer->m_toSync.empty());
    EXPECT_FALSE(m_vxlan_vrf_map_orch->isVrfMapExists(tenant_map));
    EXPECT_EQ(tunnel->vlan_vrf_vni_count, 0u);
    EXPECT_FALSE(tunnel->isActive());
    EXPECT_EQ(gVrfOrch->getVrfRefCount("VrfTenant"), 0);
}

TEST_F(VrfOrchTest, VxlanVrfMapReaddSurvivesDeferredTunnelCleanup)
{
    provisionEvpnVtep();
    addVrf("default");

    auto consumer = vxlanVrfConsumer();
    ASSERT_NE(consumer, nullptr);
    auto tunnel = m_VxlanTunnelOrch->getVxlanTunnel("EVPN_2.2.2.2");
    ASSERT_NE(tunnel, nullptr);

    const string old_map = "EVPN_2.2.2.2:map_default_5400";
    const string replacement_map = "EVPN_2.2.2.2:map_default_5500";

    consumer->addToSync(
        {{old_map, "SET", {{"vni", "5400"}, {"vrf", "default"}}}});
    static_cast<Orch *>(m_vxlan_vrf_map_orch)->doTask();
    ASSERT_TRUE(consumer->m_toSync.empty());
    ASSERT_TRUE(m_vxlan_vrf_map_orch->isVrfMapExists(old_map));
    ASSERT_EQ(tunnel->vlan_vrf_vni_count, 1u);

    // The synthetic remote-DIP reference installed by provisionEvpnVtep()
    // postpones hardware tunnel deletion after the last local map is removed.
    consumer->addToSync({{old_map, "DEL", {}}});
    static_cast<Orch *>(m_vxlan_vrf_map_orch)->doTask();
    ASSERT_TRUE(consumer->m_toSync.empty());
    ASSERT_FALSE(m_vxlan_vrf_map_orch->isVrfMapExists(old_map));
    ASSERT_EQ(tunnel->vlan_vrf_vni_count, 0u);
    ASSERT_TRUE(tunnel->del_tnl_hw_pending);
    ASSERT_TRUE(tunnel->isActive());

    // Recreate an L3 map while deletion is postponed. The replacement takes
    // ownership of the still-active tunnel and must survive when the old
    // remote reference is subsequently released.
    consumer->addToSync(
        {{replacement_map, "SET", {{"vni", "5500"}, {"vrf", "default"}}}});
    static_cast<Orch *>(m_vxlan_vrf_map_orch)->doTask();
    ASSERT_TRUE(consumer->m_toSync.empty());
    ASSERT_TRUE(m_vxlan_vrf_map_orch->isVrfMapExists(replacement_map));
    ASSERT_EQ(tunnel->vlan_vrf_vni_count, 1u);

    tunnel->updateRemoteEndPointIpRef("2.2.2.2", false);
    tunnel->eraseRemoteEndPoint("2.2.2.2");
    ASSERT_FALSE(tunnel->isTunnelReferenced());
    tunnel->deletePendingSIPTunnel();

    EXPECT_TRUE(m_vxlan_vrf_map_orch->isVrfMapExists(replacement_map));
    EXPECT_EQ(tunnel->vlan_vrf_vni_count, 1u);
    EXPECT_TRUE(tunnel->isActive());
    EXPECT_FALSE(tunnel->del_tnl_hw_pending);
}

TEST_F(VrfOrchTest, VxlanVrfMapReaddCompletesReadyPendingDeletion)
{
    provisionEvpnVtep();
    addVrf("default");

    auto consumer = vxlanVrfConsumer();
    ASSERT_NE(consumer, nullptr);
    auto tunnel = m_VxlanTunnelOrch->getVxlanTunnel("EVPN_2.2.2.2");
    ASSERT_NE(tunnel, nullptr);

    const string old_map = "EVPN_2.2.2.2:map_default_5600";
    const string replacement_map = "EVPN_2.2.2.2:map_default_5700";

    consumer->addToSync(
        {{old_map, "SET", {{"vni", "5600"}, {"vrf", "default"}}}});
    static_cast<Orch *>(m_vxlan_vrf_map_orch)->doTask();
    ASSERT_TRUE(consumer->m_toSync.empty());

    consumer->addToSync({{old_map, "DEL", {}}});
    static_cast<Orch *>(m_vxlan_vrf_map_orch)->doTask();
    ASSERT_TRUE(consumer->m_toSync.empty());
    ASSERT_TRUE(tunnel->del_tnl_hw_pending);
    ASSERT_TRUE(tunnel->isActive());
    ASSERT_EQ(tunnel->vlan_vrf_vni_count, 0u);

    // Model the point after the final remote reference drains but before its
    // deferred cleanup callback runs. The replacement SET must wait without
    // taking over a tunnel that is now ready for deletion.
    tunnel->updateRemoteEndPointIpRef("2.2.2.2", false);
    tunnel->eraseRemoteEndPoint("2.2.2.2");
    ASSERT_FALSE(tunnel->isTunnelReferenced());

    consumer->addToSync(
        {{replacement_map, "SET", {{"vni", "5700"}, {"vrf", "default"}}}});
    static_cast<Orch *>(m_vxlan_vrf_map_orch)->doTask();

    EXPECT_FALSE(m_vxlan_vrf_map_orch->isVrfMapExists(replacement_map));
    EXPECT_EQ(consumer->m_toSync.size(), 1u);
    EXPECT_TRUE(tunnel->isActive());
    EXPECT_TRUE(tunnel->del_tnl_hw_pending);
    EXPECT_EQ(tunnel->vlan_vrf_vni_count, 0u);

    // The reference-release owner completes the old tunnel cleanup. The
    // retained SET then retries and recreates a clean tunnel.
    tunnel->deletePendingSIPTunnel();
    ASSERT_FALSE(tunnel->isActive());
    ASSERT_FALSE(tunnel->del_tnl_hw_pending);

    static_cast<Orch *>(m_vxlan_vrf_map_orch)->doTask();

    EXPECT_TRUE(consumer->m_toSync.empty());
    EXPECT_TRUE(m_vxlan_vrf_map_orch->isVrfMapExists(replacement_map));
    EXPECT_TRUE(tunnel->isActive());
    EXPECT_FALSE(tunnel->del_tnl_hw_pending);
    EXPECT_EQ(tunnel->vlan_vrf_vni_count, 1u);
}

TEST_F(VrfOrchTest, VxlanVrfMapReaddRecreatesAfterCompletedDeletion)
{
    provisionEvpnVtep();
    addVrf("default");

    auto consumer = vxlanVrfConsumer();
    ASSERT_NE(consumer, nullptr);
    auto tunnel = m_VxlanTunnelOrch->getVxlanTunnel("EVPN_2.2.2.2");
    ASSERT_NE(tunnel, nullptr);

    const string old_map = "EVPN_2.2.2.2:map_default_5800";
    const string replacement_map = "EVPN_2.2.2.2:map_default_5900";

    consumer->addToSync(
        {{old_map, "SET", {{"vni", "5800"}, {"vrf", "default"}}}});
    static_cast<Orch *>(m_vxlan_vrf_map_orch)->doTask();
    ASSERT_TRUE(consumer->m_toSync.empty());

    consumer->addToSync({{old_map, "DEL", {}}});
    static_cast<Orch *>(m_vxlan_vrf_map_orch)->doTask();
    ASSERT_TRUE(tunnel->del_tnl_hw_pending);

    tunnel->updateRemoteEndPointIpRef("2.2.2.2", false);
    tunnel->eraseRemoteEndPoint("2.2.2.2");
    tunnel->deletePendingSIPTunnel();
    ASSERT_FALSE(tunnel->isActive());
    ASSERT_FALSE(tunnel->del_tnl_hw_pending);

    consumer->addToSync(
        {{replacement_map, "SET", {{"vni", "5900"}, {"vrf", "default"}}}});
    static_cast<Orch *>(m_vxlan_vrf_map_orch)->doTask();

    EXPECT_TRUE(consumer->m_toSync.empty());
    EXPECT_TRUE(m_vxlan_vrf_map_orch->isVrfMapExists(replacement_map));
    EXPECT_TRUE(tunnel->isActive());
    EXPECT_EQ(tunnel->vlan_vrf_vni_count, 1u);
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
    auto tunnel = m_VxlanTunnelOrch->getVxlanTunnel("EVPN_2.2.2.2");
    ASSERT_EQ(tunnel->vlan_vrf_vni_count, 1u);

    consumer->addToSync({{map_key, "DEL", {}}});
    static_cast<Orch *>(m_vxlan_vrf_map_orch)->doTask();

    EXPECT_TRUE(consumer->m_toSync.empty());
    EXPECT_FALSE(m_vxlan_vrf_map_orch->isVrfMapExists(map_key));
    EXPECT_EQ(gVrfOrch->getVrfRefCount("default"), 0);
    EXPECT_EQ(tunnel->vlan_vrf_vni_count, 0u);
}

TEST_F(VrfOrchTest, VxlanVrfMapNewerSetSupersedesPendingDelete)
{
    provisionEvpnVtep();
    addVrf("default");

    auto consumer = vxlanVrfConsumer();
    ASSERT_NE(consumer, nullptr);

    const string map_key = "EVPN_2.2.2.2:map_default_5200";

    // A delete for a map that was never published is an idempotent success;
    // it must not remain queued ahead of a later SET for the same key.
    consumer->addToSync({{map_key, "DEL", {}}});
    static_cast<Orch *>(m_vxlan_vrf_map_orch)->doTask();
    ASSERT_TRUE(consumer->m_toSync.empty());

    // A newer SET is the desired final state. The older pending DEL must not
    // remove the mapping after the SET succeeds.
    consumer->addToSync(
        {{map_key, "SET", {{"vni", "5200"}, {"vrf", "default"}}}});
    static_cast<Orch *>(m_vxlan_vrf_map_orch)->doTask();
    static_cast<Orch *>(m_vxlan_vrf_map_orch)->doTask();

    EXPECT_TRUE(consumer->m_toSync.empty());
    EXPECT_TRUE(m_vxlan_vrf_map_orch->isVrfMapExists(map_key));
}

} // namespace vrforch_test
