// Exercise per-source sampled bindings independently of export configuration.
#define private public
#include "directory.h"
#undef private
#define protected public
#include "orch.h"
#undef protected
#define private public
#include "switchorch.h"
#include "mirrororch.h"
#include "sfloworch.h"
#undef private
#include "mock_orch_test.h"
#include "mirrororch_sample_port_sai_wrap.h"

extern sai_samplepacket_api_t *sai_samplepacket_api;
extern sai_port_api_t *sai_port_api;

namespace mirrororch_test
{
using namespace mock_orch_test;
using namespace mirror_sample_port_wrap_ut;

class MirrorOrchTest : public MockOrchTest {};

TEST_F(MirrorOrchTest, RejectsIngressWhenUnsupported)
{
    gSwitchOrch->m_portIngressMirrorSupported = false;
    Port port;
    EXPECT_FALSE(gMirrorOrch->setUnsetPortMirror(port, true, true, SAI_NULL_OBJECT_ID));
}

TEST_F(MirrorOrchTest, RejectsEgressWhenUnsupported)
{
    gSwitchOrch->m_portEgressMirrorSupported = false;
    Port port;
    EXPECT_FALSE(gMirrorOrch->setUnsetPortMirror(port, false, true, SAI_NULL_OBJECT_ID));
}

class SampleBindingTest : public MockOrchTest
{
protected:
    std::unique_ptr<PortSampleSaiGuard> guard;
    void ApplyInitialConfigs() override
    {
        Table ports(m_app_db.get(), APP_PORT_TABLE_NAME);
        auto initial = ut_helper::getInitialSaiPorts();
        for (const auto& port : initial) ports.set(port.first, port.second);
        ports.set("PortConfigDone", {{"count", to_string(initial.size())}});
        ports.set("PortInitDone", {{}});
        gPortsOrch->addExistingData(&ports);
        static_cast<Orch*>(gPortsOrch)->doTask();
    }
    void PostSetUp() override
    {
        guard.reset(new PortSampleSaiGuard());
        gSwitchOrch->m_portIngressSampleMirrorSupported = true;
        gSwitchOrch->m_portEgressSampleMirrorSupported = true;
        gSwitchOrch->m_samplepacketTruncationSupported = true;
        // The port API wrapper verifies the mirror OID is non-null. Export
        // creation/route resolution is exercised by the DVS tests separately.
        MirrorEntry entry("");
        entry.type = "ERSPAN";
        entry.status = true;
        entry.sessionId = 0x1234;
        gMirrorOrch->m_syncdMirrors.emplace("export1", entry);
    }
    void PreTearDown() override
    {
        g_fail_mirror_session_set = false;
        g_fail_samplepacket_enable_set = false;
        g_fail_samplepacket_remove = false;
        g_fail_samplepacket_create = false;
        for (auto& item : gMirrorOrch->m_sampleBindings) item.second.deleting = true;
        gMirrorOrch->syncSampleBindings();
        EXPECT_TRUE(gMirrorOrch->m_sampleBindings.empty());
        gMirrorOrch->m_syncdMirrors.clear();
        guard.reset();
    }
    vector<FieldValueTuple> fields(string rate = "5000", string admin = "up", string truncate = "")
    {
        vector<FieldValueTuple> values{{"mirror_session", "export1"}, {"sample_rate", rate}};
        if (!admin.empty()) values.emplace_back("admin_state", admin);
        if (!truncate.empty()) values.emplace_back("truncate_size", truncate);
        return values;
    }
    sai_object_id_t portOid(const string& name)
    {
        Port port;
        EXPECT_TRUE(gPortsOrch->getPort(name, port));
        return port.m_port_id;
    }
    sai_object_id_t sampleOn(const string& port, bool ingress = true)
    {
        sai_attribute_t attr{};
        attr.id = ingress ? SAI_PORT_ATTR_INGRESS_SAMPLEPACKET_ENABLE : SAI_PORT_ATTR_EGRESS_SAMPLEPACKET_ENABLE;
        EXPECT_EQ(sai_port_api->get_port_attribute(portOid(port), 1, &attr), SAI_STATUS_SUCCESS);
        return attr.value.oid;
    }
    void remove(const string& key)
    {
        auto consumer = dynamic_cast<Consumer*>(gMirrorOrch->getExecutor("PORT_MIRROR_SAMPLE"));
        ASSERT_NE(consumer, nullptr);
        consumer->m_toSync.emplace(key, KeyOpFieldsValuesTuple(key, DEL_COMMAND, {}));
        gMirrorOrch->doTask(*consumer);
    }
};

TEST_F(SampleBindingTest, ConsumerCreatesBindingAndDeletionPreservesExport)
{
    auto consumer = dynamic_cast<Consumer*>(gMirrorOrch->getExecutor("PORT_MIRROR_SAMPLE"));
    ASSERT_NE(consumer, nullptr);
    consumer->m_toSync.emplace("Ethernet0|rx", KeyOpFieldsValuesTuple("Ethernet0|rx", SET_COMMAND, fields()));
    gMirrorOrch->doTask(*consumer);
    EXPECT_TRUE(consumer->m_toSync.empty());
    EXPECT_NE(sampleOn("Ethernet0"), SAI_NULL_OBJECT_ID);
    EXPECT_EQ(sampleOn("Ethernet0", false), SAI_NULL_OBJECT_ID);
    EXPECT_EQ(gMirrorOrch->m_syncdMirrors.at("export1").refCount, 1);
    sai_object_id_t mirror = SAI_NULL_OBJECT_ID;
    sai_attribute_t attr{};
    attr.id = SAI_PORT_ATTR_INGRESS_SAMPLE_MIRROR_SESSION;
    attr.value.objlist.count = 1;
    attr.value.objlist.list = &mirror;
    ASSERT_EQ(sai_port_api->get_port_attribute(portOid("Ethernet0"), 1, &attr), SAI_STATUS_SUCCESS);
    EXPECT_EQ(attr.value.objlist.count, 1u);
    EXPECT_EQ(mirror, gMirrorOrch->m_syncdMirrors.at("export1").sessionId);
    remove("Ethernet0|rx");
    EXPECT_EQ(sampleOn("Ethernet0"), SAI_NULL_OBJECT_ID);
    EXPECT_TRUE(gMirrorOrch->m_sampleBindings.empty());
    EXPECT_TRUE(gMirrorOrch->sessionExists("export1"));
    EXPECT_EQ(gMirrorOrch->m_syncdMirrors.at("export1").refCount, 0);
}

TEST_F(SampleBindingTest, IndependentDirectionsAndPoliciesShareExport)
{
    EXPECT_EQ(gMirrorOrch->createSampleBinding("Ethernet0|rx", fields("2", "up", "64")), task_success);
    EXPECT_EQ(gMirrorOrch->createSampleBinding("Ethernet0|tx", fields("4294967295", "up", "9216")), task_success);
    auto ingress = sampleOn("Ethernet0");
    auto egress = sampleOn("Ethernet0", false);
    EXPECT_NE(ingress, SAI_NULL_OBJECT_ID);
    EXPECT_NE(egress, SAI_NULL_OBJECT_ID);
    EXPECT_NE(ingress, egress);
    sai_attribute_t attr{};
    attr.id = SAI_SAMPLEPACKET_ATTR_SAMPLE_RATE;
    ASSERT_EQ(sai_samplepacket_api->get_samplepacket_attribute(ingress, 1, &attr), SAI_STATUS_SUCCESS);
    EXPECT_EQ(attr.value.u32, 2u);
    ASSERT_EQ(sai_samplepacket_api->get_samplepacket_attribute(egress, 1, &attr), SAI_STATUS_SUCCESS);
    EXPECT_EQ(attr.value.u32, 0xffffffffu);
    remove("Ethernet0|rx");
    EXPECT_EQ(sampleOn("Ethernet0", false), egress);
    EXPECT_EQ(gMirrorOrch->m_syncdMirrors.at("export1").refCount, 1);
}

TEST_F(SampleBindingTest, DisabledAndDefaultDownAllocateNothing)
{
    EXPECT_EQ(gMirrorOrch->createSampleBinding("Ethernet0|rx", fields("5000", "down")), task_success);
    EXPECT_EQ(gMirrorOrch->createSampleBinding("Ethernet4|tx", fields("5000", "")), task_success);
    for (const auto& item : gMirrorOrch->m_sampleBindings)
    {
        EXPECT_FALSE(item.second.enabled);
        EXPECT_EQ(item.second.samplepacketId, SAI_NULL_OBJECT_ID);
        EXPECT_TRUE(item.second.attachments.empty());
    }
    EXPECT_EQ(gMirrorOrch->deleteEntry("export1"), task_need_retry);
}

TEST_F(SampleBindingTest, DuplicateSetDoesNotChangePolicyOrState)
{
    ASSERT_EQ(gMirrorOrch->createSampleBinding("Ethernet0|rx", fields()), task_success);
    auto oid = sampleOn("Ethernet0");
    EXPECT_EQ(gMirrorOrch->createSampleBinding("Ethernet0|rx", fields("1000", "down", "128")), task_duplicated);
    EXPECT_EQ(sampleOn("Ethernet0"), oid);
    EXPECT_EQ(gMirrorOrch->m_sampleBindings.at("Ethernet0|rx").rate, 5000u);
    EXPECT_TRUE(gMirrorOrch->m_sampleBindings.at("Ethernet0|rx").enabled);
}

TEST_F(SampleBindingTest, InvalidConfigurationAllocatesNothing)
{
    for (const auto& key : {"Ethernet0", "Ethernet0|both", "Ethernet0|RX", "|rx", "Ethernet0|rx|extra"})
        EXPECT_EQ(gMirrorOrch->createSampleBinding(key, fields()), task_invalid_entry);
    for (const auto& rate : {"0", "1", "4294967296", "bad"})
        EXPECT_EQ(gMirrorOrch->createSampleBinding("Ethernet0|rx", fields(rate)), task_invalid_entry);
    for (const auto& truncate : {"0", "63", "9217", "bad"})
        EXPECT_EQ(gMirrorOrch->createSampleBinding("Ethernet0|rx", fields("5000", "up", truncate)), task_invalid_entry);
    EXPECT_EQ(gMirrorOrch->createSampleBinding("Ethernet0|rx", fields("5000", "invalid")), task_invalid_entry);
    EXPECT_EQ(gMirrorOrch->createSampleBinding("Ethernet0|rx", {{"mirror_session", "export1"}}), task_invalid_entry);
    EXPECT_EQ(gMirrorOrch->createSampleBinding("Ethernet0|rx", {{"sample_rate", "5000"}}), task_invalid_entry);
    EXPECT_TRUE(gMirrorOrch->m_sampleBindings.empty());
}

TEST_F(SampleBindingTest, RejectsSpanAndExportsWithSource)
{
    auto& session = gMirrorOrch->m_syncdMirrors.at("export1");
    session.type = "SPAN";
    EXPECT_EQ(gMirrorOrch->createSampleBinding("Ethernet0|rx", fields()), task_invalid_entry);
    session.type = "ERSPAN";
    session.src_port = "Ethernet4";
    EXPECT_EQ(gMirrorOrch->createSampleBinding("Ethernet0|rx", fields()), task_invalid_entry);
}

TEST_F(SampleBindingTest, WaitsForExportAndDestination)
{
    auto exportEntry = gMirrorOrch->m_syncdMirrors.at("export1");
    gMirrorOrch->m_syncdMirrors.erase("export1");
    ASSERT_EQ(gMirrorOrch->createSampleBinding("Ethernet0|rx", fields()), task_success);
    EXPECT_EQ(sampleOn("Ethernet0"), SAI_NULL_OBJECT_ID);
    exportEntry.status = false;
    gMirrorOrch->m_syncdMirrors.emplace("export1", exportEntry);
    gMirrorOrch->syncSampleBindings();
    EXPECT_EQ(sampleOn("Ethernet0"), SAI_NULL_OBJECT_ID);
    gMirrorOrch->m_syncdMirrors.at("export1").status = true;
    gMirrorOrch->syncSampleBindings();
    EXPECT_NE(sampleOn("Ethernet0"), SAI_NULL_OBJECT_ID);
    gMirrorOrch->m_sampleSuspendedExports.insert("export1");
    gMirrorOrch->syncSampleBindings();
    EXPECT_EQ(sampleOn("Ethernet0"), SAI_NULL_OBJECT_ID);
    gMirrorOrch->m_sampleSuspendedExports.erase("export1");
    gMirrorOrch->syncSampleBindings();
    EXPECT_NE(sampleOn("Ethernet0"), SAI_NULL_OBJECT_ID);
}

TEST_F(SampleBindingTest, UnsupportedCapabilitiesLeaveBindingUnattached)
{
    gSwitchOrch->m_portIngressSampleMirrorSupported = false;
    EXPECT_EQ(gMirrorOrch->createSampleBinding("Ethernet0|rx", fields()), task_success);
    EXPECT_EQ(sampleOn("Ethernet0"), SAI_NULL_OBJECT_ID);
    gSwitchOrch->m_samplepacketTruncationSupported = false;
    EXPECT_EQ(gMirrorOrch->createSampleBinding("Ethernet4|tx", fields("5000", "up", "128")), task_success);
    EXPECT_EQ(sampleOn("Ethernet4", false), SAI_NULL_OBJECT_ID);
}

TEST_F(SampleBindingTest, ExplicitSflowDownConflictsSystemWide)
{
    Table sflow(m_config_db.get(), "SFLOW");
    sflow.set("global", {{"admin_state", "down"}});
    EXPECT_EQ(gMirrorOrch->createSampleBinding("Ethernet0|rx", fields("5000", "down")), task_need_retry);
    EXPECT_TRUE(gMirrorOrch->m_sampleBindings.empty());
    sflow.del("global");
    EXPECT_EQ(gMirrorOrch->createSampleBinding("Ethernet0|rx", fields()), task_success);
}

TEST_F(SampleBindingTest, ExistingSamplePacketNotOverwritten)
{
    sai_attribute_t attr{};
    attr.id = SAI_PORT_ATTR_INGRESS_SAMPLEPACKET_ENABLE;
    attr.value.oid = 0x9876;
    ASSERT_EQ(sai_port_api->set_port_attribute(portOid("Ethernet0"), &attr), SAI_STATUS_SUCCESS);
    EXPECT_EQ(gMirrorOrch->createSampleBinding("Ethernet0|rx", fields()), task_success);
    EXPECT_EQ(sampleOn("Ethernet0"), 0x9876u);
    EXPECT_TRUE(gMirrorOrch->m_sampleOwners.empty());
    attr.value.oid = SAI_NULL_OBJECT_ID;
    ASSERT_EQ(sai_port_api->set_port_attribute(portOid("Ethernet0"), &attr), SAI_STATUS_SUCCESS);
    gMirrorOrch->syncSampleBindings();
    EXPECT_NE(sampleOn("Ethernet0"), SAI_NULL_OBJECT_ID);
}

TEST_F(SampleBindingTest, CreateAndAttachFailuresRetryWithoutLeaking)
{
    g_fail_samplepacket_create = true;
    EXPECT_EQ(gMirrorOrch->createSampleBinding("Ethernet0|rx", fields()), task_success);
    EXPECT_EQ(gMirrorOrch->m_sampleBindings.at("Ethernet0|rx").samplepacketId, SAI_NULL_OBJECT_ID);
    g_fail_samplepacket_create = false;
    g_fail_mirror_session_set = true;
    gMirrorOrch->syncSampleBindings();
    EXPECT_EQ(sampleOn("Ethernet0"), SAI_NULL_OBJECT_ID);
    EXPECT_TRUE(gMirrorOrch->m_sampleOwners.empty());
    g_fail_mirror_session_set = false;
    gMirrorOrch->syncSampleBindings();
    EXPECT_NE(sampleOn("Ethernet0"), SAI_NULL_OBJECT_ID);
}

TEST_F(SampleBindingTest, FailedDetachRetainsOwnershipAndExportReference)
{
    ASSERT_EQ(gMirrorOrch->createSampleBinding("Ethernet0|rx", fields()), task_success);
    auto oid = sampleOn("Ethernet0");
    g_fail_mirror_session_set = true;
    remove("Ethernet0|rx");
    EXPECT_EQ(sampleOn("Ethernet0"), oid);
    EXPECT_EQ(gMirrorOrch->m_syncdMirrors.at("export1").refCount, 1);
    EXPECT_FALSE(gMirrorOrch->m_sampleOwners.empty());
    EXPECT_EQ(gMirrorOrch->createSampleBinding("Ethernet0|rx", fields("1000")), task_need_retry);
    g_fail_mirror_session_set = false;
    remove("Ethernet0|rx");
    EXPECT_TRUE(gMirrorOrch->m_sampleBindings.empty());
}

TEST_F(SampleBindingTest, FailedSamplePacketDetachAndRemovalRetried)
{
    ASSERT_EQ(gMirrorOrch->createSampleBinding("Ethernet0|rx", fields()), task_success);
    auto oid = sampleOn("Ethernet0");

    g_fail_samplepacket_enable_set = true;
    remove("Ethernet0|rx");
    EXPECT_FALSE(gMirrorOrch->m_sampleBindings.at("Ethernet0|rx").attachments.at(portOid("Ethernet0")).mirrorSet);
    EXPECT_TRUE(gMirrorOrch->m_sampleBindings.at("Ethernet0|rx").attachments.at(portOid("Ethernet0")).sampleSet);
    sai_object_id_t mirror = SAI_NULL_OBJECT_ID;
    sai_attribute_t attr{};
    attr.id = SAI_PORT_ATTR_INGRESS_SAMPLE_MIRROR_SESSION;
    attr.value.objlist.count = 1;
    attr.value.objlist.list = &mirror;
    ASSERT_EQ(sai_port_api->get_port_attribute(portOid("Ethernet0"), 1, &attr), SAI_STATUS_SUCCESS);
    EXPECT_EQ(attr.value.objlist.count, 0u);
    EXPECT_EQ(sampleOn("Ethernet0"), oid);
    EXPECT_EQ(gMirrorOrch->deleteEntry("export1"), task_need_retry);
    EXPECT_TRUE(gMirrorOrch->hasSampleBindings());

    g_fail_samplepacket_enable_set = false;
    g_fail_samplepacket_remove = true;
    static_cast<Orch*>(gMirrorOrch)->doTask();
    EXPECT_TRUE(gMirrorOrch->m_sampleBindings.at("Ethernet0|rx").attachments.empty());
    EXPECT_EQ(sampleOn("Ethernet0"), SAI_NULL_OBJECT_ID);
    EXPECT_NE(gMirrorOrch->m_sampleBindings.at("Ethernet0|rx").samplepacketId, SAI_NULL_OBJECT_ID);
    EXPECT_EQ(gMirrorOrch->m_syncdMirrors.at("export1").refCount, 1);

    // Successful removal releases the remaining binding resources.
    g_fail_samplepacket_remove = false;
    static_cast<Orch*>(gMirrorOrch)->doTask();
    EXPECT_TRUE(gMirrorOrch->m_sampleBindings.empty());
    EXPECT_FALSE(gMirrorOrch->hasSampleBindings());
    EXPECT_EQ(gMirrorOrch->m_syncdMirrors.at("export1").refCount, 0);
}

TEST_F(SampleBindingTest, PortChannelMembershipAndDirectMemberConflict)
{
    Port lag;
    lag.m_alias = "PortChannel10";
    lag.m_type = Port::LAG;
    lag.m_members.insert("Ethernet0");
    gPortsOrch->setPort(lag.m_alias, lag);
    EXPECT_EQ(gMirrorOrch->createSampleBinding("PortChannel10|rx", fields()), task_success);
    auto oid = sampleOn("Ethernet0");
    EXPECT_NE(oid, SAI_NULL_OBJECT_ID);
    lag.m_members.insert("Ethernet4");
    gPortsOrch->setPort(lag.m_alias, lag);
    Port member;
    ASSERT_TRUE(gPortsOrch->getPort("Ethernet4", member));
    gMirrorOrch->updateLagMember({lag, member, true});
    EXPECT_EQ(sampleOn("Ethernet4"), oid);
    EXPECT_EQ(gMirrorOrch->createSampleBinding("Ethernet4|rx", fields("1000")), task_success);
    EXPECT_EQ(sampleOn("Ethernet4"), oid);
    lag.m_members.erase("Ethernet4");
    gPortsOrch->setPort(lag.m_alias, lag);
    gMirrorOrch->updateLagMember({lag, member, false});
    gMirrorOrch->syncSampleBindings();
    EXPECT_NE(sampleOn("Ethernet4"), SAI_NULL_OBJECT_ID);
    EXPECT_NE(sampleOn("Ethernet4"), oid);
    remove("PortChannel10|rx");
    EXPECT_EQ(sampleOn("Ethernet0"), SAI_NULL_OBJECT_ID);
    EXPECT_NE(sampleOn("Ethernet4"), SAI_NULL_OBJECT_ID);
}
TEST_F(SampleBindingTest, TruncationAttributesAndExclusiveMode)
{
    ASSERT_EQ(gMirrorOrch->createSampleBinding("Ethernet0|rx", fields("5000", "up", "128")), task_success);
    auto oid = sampleOn("Ethernet0");
    sai_attribute_t attr{};
    attr.id = SAI_SAMPLEPACKET_ATTR_TRUNCATE_ENABLE;
    ASSERT_EQ(sai_samplepacket_api->get_samplepacket_attribute(oid, 1, &attr), SAI_STATUS_SUCCESS);
    EXPECT_TRUE(attr.value.booldata);
    attr.id = SAI_SAMPLEPACKET_ATTR_TRUNCATE_SIZE;
    ASSERT_EQ(sai_samplepacket_api->get_samplepacket_attribute(oid, 1, &attr), SAI_STATUS_SUCCESS);
    EXPECT_EQ(attr.value.u32, 128u);
    attr.id = SAI_SAMPLEPACKET_ATTR_MODE;
    ASSERT_EQ(sai_samplepacket_api->get_samplepacket_attribute(oid, 1, &attr), SAI_STATUS_SUCCESS);
    EXPECT_EQ(attr.value.s32, SAI_SAMPLEPACKET_MODE_EXCLUSIVE);
}

TEST_F(SampleBindingTest, EmptyLagWaitsThenFollowsMembers)
{
    Port lag;
    lag.m_alias = "PortChannel20";
    lag.m_type = Port::LAG;
    gPortsOrch->setPort(lag.m_alias, lag);
    ASSERT_EQ(gMirrorOrch->createSampleBinding("PortChannel20|rx", fields()), task_success);
    EXPECT_EQ(gMirrorOrch->m_sampleBindings.at("PortChannel20|rx").samplepacketId, SAI_NULL_OBJECT_ID);
    lag.m_members.insert("Ethernet0");
    gPortsOrch->setPort(lag.m_alias, lag);
    Port member;
    ASSERT_TRUE(gPortsOrch->getPort("Ethernet0", member));
    gMirrorOrch->updateLagMember({lag, member, true});
    EXPECT_NE(sampleOn("Ethernet0"), SAI_NULL_OBJECT_ID);
    lag.m_members.clear();
    gPortsOrch->setPort(lag.m_alias, lag);
    gMirrorOrch->updateLagMember({lag, member, false});
    EXPECT_EQ(sampleOn("Ethernet0"), SAI_NULL_OBJECT_ID);
    EXPECT_EQ(gMirrorOrch->m_sampleBindings.at("PortChannel20|rx").samplepacketId, SAI_NULL_OBJECT_ID);
    EXPECT_TRUE(gMirrorOrch->sessionExists("export1"));
}

TEST_F(SampleBindingTest, SflowCannotBindWhileDisabledSampleBindingExists)
{
    vector<string> tables{APP_SFLOW_TABLE_NAME, APP_SFLOW_SESSION_TABLE_NAME};
    SflowOrch sflow(m_app_db.get(), tables);
    ASSERT_EQ(gMirrorOrch->createSampleBinding("Ethernet4|rx", fields("5000", "down")), task_success);
    EXPECT_FALSE(sflow.sflowAddPort(0x9876, portOid("Ethernet0"), "rx"));
    EXPECT_EQ(sampleOn("Ethernet0"), SAI_NULL_OBJECT_ID);
    remove("Ethernet4|rx");
    EXPECT_TRUE(sflow.sflowAddPort(0x9876, portOid("Ethernet0"), "rx"));
    EXPECT_TRUE(sflow.sflowDelPort(portOid("Ethernet0"), "rx"));
}

TEST_F(SampleBindingTest, ExportDeactivateWaitsForAttachmentCleanupAndReactivates)
{
    auto& session = gMirrorOrch->m_syncdMirrors.at("export1");
    session.status = false;
    session.sessionId = SAI_NULL_OBJECT_ID;
    session.srcIp = IpAddress("10.0.0.1");
    session.dstIp = IpAddress("10.0.0.2");
    session.neighborInfo.portId = portOid("Ethernet4");
    ASSERT_TRUE(gPortsOrch->getPort("Ethernet4", session.neighborInfo.port));
    session.neighborInfo.mac = MacAddress("00:11:22:33:44:55");
    session.greType = 0x8949;
    ASSERT_TRUE(gMirrorOrch->activateSession("export1", session));
    ASSERT_EQ(gMirrorOrch->createSampleBinding("Ethernet0|rx", fields()), task_success);
    g_fail_mirror_session_set = true;
    EXPECT_FALSE(gMirrorOrch->deactivateSession("export1", session));
    EXPECT_TRUE(session.status);
    EXPECT_EQ(session.refCount, 1);
    g_fail_mirror_session_set = false;
    ASSERT_TRUE(gMirrorOrch->deactivateSession("export1", session));
    EXPECT_EQ(session.refCount, 0);
    EXPECT_EQ(sampleOn("Ethernet0"), SAI_NULL_OBJECT_ID);
    ASSERT_TRUE(gMirrorOrch->activateSession("export1", session));
    EXPECT_NE(sampleOn("Ethernet0"), SAI_NULL_OBJECT_ID);
    remove("Ethernet0|rx");
    ASSERT_TRUE(gMirrorOrch->deactivateSession("export1", session));
}

}
