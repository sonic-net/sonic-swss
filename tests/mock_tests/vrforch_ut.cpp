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

DEFINE_SAI_GENERIC_API_MOCK(virtual_router, virtual_router);

class VrfOrchTest : public MockOrchTest
{
  protected:
    EvpnNvoOrch *m_evpn_nvo_orch = nullptr;

    void PostSetUp() override
    {
        INIT_SAI_API_MOCK(virtual_router);
        MockSaiApis();
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

} // namespace vrforch_test
