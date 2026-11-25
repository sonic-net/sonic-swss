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
#include "aclorch.h"
#include "mirrororch_sample_port_sai_wrap.h"
#include "mock_sai_capability_wrap.h"
#include <arpa/inet.h>
#include <cstring>

extern sai_samplepacket_api_t *sai_samplepacket_api;
extern sai_port_api_t *sai_port_api;
extern sai_mirror_api_t *sai_mirror_api;

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
        g_fail_egress_samplepacket_enable_set = false;
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
    void verifyIngressSampleMirror(const string& port, sai_object_id_t expected)
    {
        sai_object_id_t mirror = SAI_NULL_OBJECT_ID;
        sai_attribute_t attr{};
        attr.id = SAI_PORT_ATTR_INGRESS_SAMPLE_MIRROR_SESSION;
        attr.value.objlist = {1, &mirror};
        ASSERT_EQ(sai_port_api->get_port_attribute(portOid(port), 1, &attr), SAI_STATUS_SUCCESS);
        EXPECT_EQ(attr.value.objlist.count, expected == SAI_NULL_OBJECT_ID ? 0u : 1u);
        if (expected != SAI_NULL_OBJECT_ID)
        {
            EXPECT_EQ(mirror, expected);
        }
    }
    void remove(const string& key)
    {
        Table(m_config_db.get(), "PORT_MIRROR_SAMPLE").del(key);
        auto consumer = dynamic_cast<Consumer*>(gMirrorOrch->getExecutor("PORT_MIRROR_SAMPLE"));
        ASSERT_NE(consumer, nullptr);
        consumer->m_toSync.emplace(key, KeyOpFieldsValuesTuple(key, DEL_COMMAND, {}));
        gMirrorOrch->doTask(*consumer);
    }
    void setAdmin(const string& key, const string& state)
    {
        auto consumer = dynamic_cast<Consumer*>(gMirrorOrch->getExecutor("PORT_MIRROR_SAMPLE"));
        ASSERT_NE(consumer, nullptr);
        Table cfg(m_config_db.get(), "PORT_MIRROR_SAMPLE");
        cfg.hset(key, "admin_state", state);
        vector<FieldValueTuple> row;
        ASSERT_TRUE(cfg.get(key, row));
        consumer->addToSync(KeyOpFieldsValuesTuple(key, SET_COMMAND, row));
        gMirrorOrch->doTask(*consumer);
        EXPECT_TRUE(consumer->m_toSync.empty());
    }
    void sflowTask(SflowOrch& sflow, const string& table, const string& key, const string& op,
                   const vector<FieldValueTuple>& values = {})
    {
        auto consumer = dynamic_cast<Consumer*>(sflow.getExecutor(table));
        ASSERT_NE(consumer, nullptr);
        consumer->addToSync(KeyOpFieldsValuesTuple(key, op, values));
        sflow.doTask(*consumer);
        EXPECT_TRUE(consumer->m_toSync.empty());
    }
};

TEST_F(SampleBindingTest, ConsumerCreatesBindingAndDeletionPreservesExport)
{
    auto consumer = dynamic_cast<Consumer*>(gMirrorOrch->getExecutor("PORT_MIRROR_SAMPLE"));
    ASSERT_NE(consumer, nullptr);
    Table(m_config_db.get(), "PORT_MIRROR_SAMPLE").set("Ethernet0|rx", fields());
    consumer->m_toSync.emplace("Ethernet0|rx", KeyOpFieldsValuesTuple("Ethernet0|rx", SET_COMMAND, fields()));
    gMirrorOrch->doTask(*consumer);
    EXPECT_TRUE(consumer->m_toSync.empty());
    EXPECT_NE(sampleOn("Ethernet0"), SAI_NULL_OBJECT_ID);
    EXPECT_EQ(sampleOn("Ethernet0", false), SAI_NULL_OBJECT_ID);
    EXPECT_EQ(gMirrorOrch->m_syncdMirrors.at("export1").refCount, 1);
    verifyIngressSampleMirror("Ethernet0", gMirrorOrch->m_syncdMirrors.at("export1").sessionId);
    remove("Ethernet0|rx");
    EXPECT_EQ(sampleOn("Ethernet0"), SAI_NULL_OBJECT_ID);
    EXPECT_TRUE(gMirrorOrch->m_sampleBindings.empty());
    EXPECT_TRUE(gMirrorOrch->sessionExists("export1"));
    EXPECT_EQ(gMirrorOrch->m_syncdMirrors.at("export1").refCount, 0);
}

TEST_F(SampleBindingTest, IndependentDirectionsAndPoliciesShareExport)
{
    EXPECT_EQ(gMirrorOrch->setSampleBinding("Ethernet0|rx", fields("2", "up", "64")), task_success);
    EXPECT_EQ(gMirrorOrch->setSampleBinding("Ethernet0|tx", fields("4294967295", "up", "9216")), task_success);
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
    EXPECT_EQ(gMirrorOrch->setSampleBinding("Ethernet0|rx", fields("5000", "down")), task_success);
    EXPECT_EQ(gMirrorOrch->setSampleBinding("Ethernet4|tx", fields("5000", "")), task_success);
    for (const auto& item : gMirrorOrch->m_sampleBindings)
    {
        EXPECT_FALSE(item.second.enabled);
        EXPECT_EQ(item.second.samplepacketId, SAI_NULL_OBJECT_ID);
        EXPECT_TRUE(item.second.attachments.empty());
    }
    EXPECT_EQ(gMirrorOrch->deleteEntry("export1"), task_need_retry);
}

TEST_F(SampleBindingTest, AdminStateUpdatesPreservePolicyAndSharedExport)
{
    Table(m_config_db.get(), "PORT_MIRROR_SAMPLE").set("Ethernet0|rx", fields("5000", "down", "128"));
    ASSERT_EQ(gMirrorOrch->setSampleBinding("Ethernet0|rx", fields("5000", "down", "128")), task_success);
    ASSERT_EQ(gMirrorOrch->setSampleBinding("Ethernet4|tx", fields()), task_success);
    auto otherOid = sampleOn("Ethernet4", false);
    auto exportOid = gMirrorOrch->m_syncdMirrors.at("export1").sessionId;
    setAdmin("Ethernet0|rx", "up");
    auto oid = sampleOn("Ethernet0");
    EXPECT_NE(oid, SAI_NULL_OBJECT_ID);
    setAdmin("Ethernet0|rx", "up");
    EXPECT_EQ(sampleOn("Ethernet0"), oid);
    setAdmin("Ethernet0|rx", "down");
    setAdmin("Ethernet0|rx", "down");
    EXPECT_EQ(sampleOn("Ethernet0"), SAI_NULL_OBJECT_ID);
    const auto& binding = gMirrorOrch->m_sampleBindings.at("Ethernet0|rx");
    EXPECT_FALSE(binding.enabled);
    EXPECT_FALSE(binding.deleting);
    EXPECT_EQ(binding.rate, 5000u);
    EXPECT_EQ(binding.truncate, 128u);
    EXPECT_EQ(binding.session, "export1");
    EXPECT_EQ(binding.samplepacketId, SAI_NULL_OBJECT_ID);
    EXPECT_EQ(sampleOn("Ethernet4", false), otherOid);
    EXPECT_EQ(gMirrorOrch->m_syncdMirrors.at("export1").sessionId, exportOid);
    EXPECT_EQ(gMirrorOrch->m_syncdMirrors.at("export1").refCount, 1);
}

TEST_F(SampleBindingTest, PolicyUpdatesRejectedWithoutChangingAdminState)
{
    const string key = "Ethernet0|rx";
    ASSERT_EQ(gMirrorOrch->setSampleBinding(key, fields("5000", "up", "128")), task_success);
    const auto oid = sampleOn("Ethernet0");
    ASSERT_NE(oid, SAI_NULL_OBJECT_ID);
    for (const auto& row : {fields("1000", "down", "128"), fields("5000", "down"),
                            fields("5000", "down", "256"), fields("5000", "invalid", "128"),
                            vector<FieldValueTuple>{{"mirror_session", "export2"}, {"sample_rate", "5000"},
                                                    {"truncate_size", "128"}, {"admin_state", "down"}},
                            vector<FieldValueTuple>{{"mirror_session", "export1"}, {"truncate_size", "128"},
                                                    {"admin_state", "down"}}})
        EXPECT_EQ(gMirrorOrch->setSampleBinding(key, row), task_invalid_entry);
    EXPECT_EQ(gMirrorOrch->setSampleBinding(key, fields("5000", "up", "128")), task_duplicated);
    EXPECT_EQ(sampleOn("Ethernet0"), oid);
    EXPECT_TRUE(gMirrorOrch->m_sampleBindings.at(key).enabled);
    EXPECT_EQ(gMirrorOrch->setSampleBinding(key, fields("5000", "down", "128")), task_success);
    EXPECT_EQ(sampleOn("Ethernet0"), SAI_NULL_OBJECT_ID);
}

TEST_F(SampleBindingTest, CoalescedSetsUseCurrentConfigRow)
{
    const string key = "Ethernet0|rx";
    Table cfg(m_config_db.get(), "PORT_MIRROR_SAMPLE");
    auto consumer = dynamic_cast<Consumer*>(gMirrorOrch->getExecutor("PORT_MIRROR_SAMPLE"));
    ASSERT_NE(consumer, nullptr);
    auto queueSnapshot = [&] {
        vector<FieldValueTuple> row;
        ASSERT_TRUE(cfg.get(key, row));
        consumer->addToSync(KeyOpFieldsValuesTuple(key, SET_COMMAND, row));
    };

    cfg.set(key, fields("5000", "up", "128"));
    queueSnapshot();
    cfg.hdel(key, "truncate_size");
    queueSnapshot();
    gMirrorOrch->doTask(*consumer);
    ASSERT_TRUE(consumer->m_toSync.empty());
    ASSERT_NE(sampleOn("Ethernet0"), SAI_NULL_OBJECT_ID);
    EXPECT_EQ(gMirrorOrch->m_sampleBindings.at(key).truncate, 0u);

    queueSnapshot();
    cfg.hdel(key, "admin_state");
    queueSnapshot();
    gMirrorOrch->doTask(*consumer);
    EXPECT_TRUE(consumer->m_toSync.empty());
    EXPECT_FALSE(gMirrorOrch->m_sampleBindings.at(key).enabled);
    EXPECT_EQ(sampleOn("Ethernet0"), SAI_NULL_OBJECT_ID);
    remove(key);

    cfg.set(key, fields());
    queueSnapshot();
    cfg.del(key);
    gMirrorOrch->doTask(*consumer);
    EXPECT_TRUE(consumer->m_toSync.empty());
    EXPECT_FALSE(gMirrorOrch->m_sampleBindings.count(key));
}

TEST_F(SampleBindingTest, InvalidConfigurationAllocatesNothing)
{
    for (const auto& key : {"Ethernet0", "Ethernet0|both", "Ethernet0|RX", "|rx", "Ethernet0|rx|extra"})
        EXPECT_EQ(gMirrorOrch->setSampleBinding(key, fields()), task_invalid_entry);
    for (const auto& rate : {"0", "1", "-1", "4294967296", "bad"})
        EXPECT_EQ(gMirrorOrch->setSampleBinding("Ethernet0|rx", fields(rate)), task_invalid_entry);
    for (const auto& truncate : {"-1", "4294967296", "bad"})
        EXPECT_EQ(gMirrorOrch->setSampleBinding("Ethernet0|rx", fields("5000", "up", truncate)), task_invalid_entry);
    EXPECT_EQ(gMirrorOrch->setSampleBinding("Ethernet0|rx", fields("5000", "invalid")), task_invalid_entry);
    EXPECT_EQ(gMirrorOrch->setSampleBinding("Ethernet0|rx", {{"mirror_session", "export1"}}), task_invalid_entry);
    for (const auto& rate : {"0", "1"})
        EXPECT_EQ(gMirrorOrch->setSampleBinding("Ethernet0|rx", fields(rate, "up", "128")), task_invalid_entry);
    EXPECT_EQ(gMirrorOrch->setSampleBinding("Ethernet0|rx", {{"sample_rate", "5000"}}), task_invalid_entry);
    EXPECT_TRUE(gMirrorOrch->m_sampleBindings.empty());
}

TEST_F(SampleBindingTest, RejectsSpanAndExportsWithSource)
{
    auto& session = gMirrorOrch->m_syncdMirrors.at("export1");
    session.type = "SPAN";
    EXPECT_EQ(gMirrorOrch->setSampleBinding("Ethernet0|rx", fields()), task_invalid_entry);
    session.type = "ERSPAN";
    session.src_port = "Ethernet4";
    EXPECT_EQ(gMirrorOrch->setSampleBinding("Ethernet0|rx", fields()), task_invalid_entry);
}

TEST_F(SampleBindingTest, RejectedExportReleasesPolicerReference)
{
    gPolicerOrch->m_syncdPolicers["policer1"] = 0x1;
    gPolicerOrch->m_policerRefCounts["policer1"] = 0;
    vector<FieldValueTuple> span{{"type", "SPAN"}, {"dst_port", "Ethernet4"}, {"policer", "policer1"}};

    Table sample(m_config_db.get(), "PORT_MIRROR_SAMPLE");
    sample.set("Ethernet0|rx", {{"mirror_session", "export2"}, {"sample_rate", "5000"}});
    EXPECT_EQ(gMirrorOrch->createEntry("export2", span), task_invalid_entry);
    EXPECT_EQ(gPolicerOrch->m_policerRefCounts["policer1"], 0);
    sample.del("Ethernet0|rx");

    ASSERT_EQ(gMirrorOrch->setSampleBinding("Ethernet0|rx",
                                               {{"mirror_session", "export2"}, {"sample_rate", "5000"}}),
              task_success);
    EXPECT_EQ(gMirrorOrch->createEntry("export2", span), task_invalid_entry);
    EXPECT_EQ(gPolicerOrch->m_policerRefCounts["policer1"], 0);
}

TEST_F(SampleBindingTest, WaitsForExportAndDestination)
{
    auto exportEntry = gMirrorOrch->m_syncdMirrors.at("export1");
    gMirrorOrch->m_syncdMirrors.erase("export1");
    ASSERT_EQ(gMirrorOrch->setSampleBinding("Ethernet0|rx", fields()), task_success);
    const auto& binding = gMirrorOrch->m_sampleBindings.at("Ethernet0|rx");
    auto expectUnprogrammed = [&] {
        EXPECT_EQ(binding.samplepacketId, SAI_NULL_OBJECT_ID);
        EXPECT_TRUE(binding.attachments.empty());
        EXPECT_EQ(sampleOn("Ethernet0"), SAI_NULL_OBJECT_ID);
    };
    expectUnprogrammed();
    exportEntry.status = false;
    exportEntry.sessionId = SAI_NULL_OBJECT_ID;
    auto& session = gMirrorOrch->m_syncdMirrors.emplace("export1", exportEntry).first->second;
    gMirrorOrch->doTask();
    expectUnprogrammed();
    session.status = true;
    ASSERT_EQ(gMirrorOrch->setSampleBinding("Ethernet4|tx", fields()), task_success);
    expectUnprogrammed();
    EXPECT_EQ(gMirrorOrch->m_sampleBindings.at("Ethernet4|tx").samplepacketId, SAI_NULL_OBJECT_ID);
    EXPECT_EQ(session.refCount, 0);

    session.status = false;
    session.srcIp = IpAddress("10.0.0.1");
    session.dstIp = IpAddress("10.0.0.2");
    const NextHopKey nextHop(session.dstIp, "Ethernet8");
    session.nexthopInfo.nexthop = nextHop;
    const MacAddress mac("00:11:22:33:44:55");
    gNeighOrch->m_syncdNeighbors[nextHop].mac = mac;
    gNeighOrch->m_syncdNextHops[nextHop] = {SAI_NULL_OBJECT_ID, 0, 0};
    gMirrorOrch->updateNeighbor({nextHop, mac, true});
    ASSERT_TRUE(session.status);
    EXPECT_NE(sampleOn("Ethernet0"), SAI_NULL_OBJECT_ID);
    EXPECT_NE(sampleOn("Ethernet4", false), SAI_NULL_OBJECT_ID);
    ASSERT_TRUE(gMirrorOrch->deactivateSession("export1", session));
}

TEST_F(SampleBindingTest, UnsupportedCapabilitiesLeaveBindingUnattached)
{
    gSwitchOrch->m_portIngressSampleMirrorSupported = false;
    EXPECT_EQ(gMirrorOrch->setSampleBinding("Ethernet0|rx", fields()), task_success);
    EXPECT_EQ(sampleOn("Ethernet0"), SAI_NULL_OBJECT_ID);
    gSwitchOrch->m_samplepacketTruncationSupported = false;
    EXPECT_EQ(gMirrorOrch->setSampleBinding("Ethernet4|tx", fields("5000", "up", "128")), task_success);
    EXPECT_EQ(sampleOn("Ethernet4", false), SAI_NULL_OBJECT_ID);
}

TEST_F(SampleBindingTest, ExplicitSflowDownSelectsModeBeforeAppDbReplay)
{
    Table sflow(m_config_db.get(), "SFLOW");
    sflow.set("global", {{"admin_state", "down"}});
    EXPECT_EQ(gMirrorOrch->setSampleBinding("Ethernet0|rx", fields("5000", "down")), task_invalid_entry);
    EXPECT_TRUE(gMirrorOrch->m_sampleBindings.empty());
    sflow.del("global");
    EXPECT_EQ(gMirrorOrch->setSampleBinding("Ethernet0|rx", fields()), task_invalid_entry);
}

class ConflictingSamplingConfigTest : public SampleBindingTest
{
protected:
    void ApplySaiMock() override
    {
        Table(m_config_db.get(), "SFLOW").set("global", {{"admin_state", "down"}});
        Table(m_config_db.get(), "PORT_MIRROR_SAMPLE").set("Ethernet0|rx", fields("5000", "down"));
    }

};

TEST_F(ConflictingSamplingConfigTest, CorrectingConfigDoesNotChangeModeUntilRestart)
{
    Table(m_config_db.get(), "SFLOW").del("global");
    EXPECT_FALSE(gMirrorOrch->claimSflowMode());
    EXPECT_EQ(gMirrorOrch->setSampleBinding("Ethernet0|rx", fields()), task_invalid_entry);
    EXPECT_EQ(sampleOn("Ethernet0"), SAI_NULL_OBJECT_ID);
    Table(m_config_db.get(), "PORT_MIRROR_SAMPLE").del("Ethernet0|rx");
}

TEST_F(SampleBindingTest, SflowGlobalDoesNotTakeOverAttachedBinding)
{
    ASSERT_EQ(gMirrorOrch->setSampleBinding("Ethernet0|rx", fields()), task_success);
    auto oid = sampleOn("Ethernet0");
    ASSERT_NE(oid, SAI_NULL_OBJECT_ID);
    Table sflow(m_config_db.get(), "SFLOW");
    sflow.set("global", {{"admin_state", "up"}});
    gMirrorOrch->doTask();
    EXPECT_EQ(sampleOn("Ethernet0"), oid);
    sflow.del("global");
}

TEST_F(SampleBindingTest, ExistingSamplePacketNotOverwritten)
{
    sai_attribute_t attr{};
    attr.id = SAI_PORT_ATTR_INGRESS_SAMPLEPACKET_ENABLE;
    attr.value.oid = 0x9876;
    ASSERT_EQ(sai_port_api->set_port_attribute(portOid("Ethernet0"), &attr), SAI_STATUS_SUCCESS);
    EXPECT_EQ(gMirrorOrch->setSampleBinding("Ethernet0|rx", fields()), task_success);
    EXPECT_EQ(sampleOn("Ethernet0"), 0x9876u);
    EXPECT_TRUE(gMirrorOrch->m_sampleOwners.empty());
    attr.value.oid = SAI_NULL_OBJECT_ID;
    ASSERT_EQ(sai_port_api->set_port_attribute(portOid("Ethernet0"), &attr), SAI_STATUS_SUCCESS);
    gMirrorOrch->doTask();
    EXPECT_NE(sampleOn("Ethernet0"), SAI_NULL_OBJECT_ID);
}

TEST_F(SampleBindingTest, PreRebootSamplePacketIsReplacedOnlyDuringWarmRestore)
{
    sai_attribute_t attr{};
    attr.id = SAI_PORT_ATTR_INGRESS_SAMPLEPACKET_ENABLE;
    attr.value.oid = 0x9876;
    ASSERT_EQ(sai_port_api->set_port_attribute(portOid("Ethernet0"), &attr), SAI_STATUS_SUCCESS);
    gMirrorOrch->m_isWarmRestoreStage = true;
    ASSERT_EQ(gMirrorOrch->setSampleBinding("Ethernet0|rx", fields()), task_success);
    auto restored = sampleOn("Ethernet0");
    EXPECT_NE(restored, SAI_NULL_OBJECT_ID);
    EXPECT_NE(restored, 0x9876u);

    gMirrorOrch->onWarmBootEnd();
    ASSERT_EQ(sai_port_api->set_port_attribute(portOid("Ethernet4"), &attr), SAI_STATUS_SUCCESS);
    ASSERT_EQ(gMirrorOrch->setSampleBinding("Ethernet4|rx", fields()), task_success);
    EXPECT_EQ(sampleOn("Ethernet4"), 0x9876u);
}

TEST_F(SampleBindingTest, CreateAndAttachFailuresRetryWithoutLeaking)
{
    g_fail_samplepacket_create = true;
    EXPECT_EQ(gMirrorOrch->setSampleBinding("Ethernet0|rx", fields()), task_success);
    EXPECT_EQ(gMirrorOrch->m_sampleBindings.at("Ethernet0|rx").samplepacketId, SAI_NULL_OBJECT_ID);
    g_fail_samplepacket_create = false;
    g_fail_mirror_session_set = true;
    gMirrorOrch->doTask();
    EXPECT_EQ(sampleOn("Ethernet0"), SAI_NULL_OBJECT_ID);
    EXPECT_TRUE(gMirrorOrch->m_sampleOwners.empty());
    g_fail_mirror_session_set = false;
    gMirrorOrch->doTask();
    EXPECT_NE(sampleOn("Ethernet0"), SAI_NULL_OBJECT_ID);
}

TEST_F(SampleBindingTest, FailedDetachRetainsOwnershipAndExportReference)
{
    Table(m_config_db.get(), "PORT_MIRROR_SAMPLE").set("Ethernet0|rx", fields());
    ASSERT_EQ(gMirrorOrch->setSampleBinding("Ethernet0|rx", fields()), task_success);
    auto oid = sampleOn("Ethernet0");
    g_fail_mirror_session_set = true;
    setAdmin("Ethernet0|rx", "down");
    EXPECT_EQ(sampleOn("Ethernet0"), oid);
    EXPECT_EQ(gMirrorOrch->m_syncdMirrors.at("export1").refCount, 1);
    EXPECT_FALSE(gMirrorOrch->m_sampleOwners.empty());
    EXPECT_TRUE(gMirrorOrch->m_sampleBindings.at("Ethernet0|rx").cleanupPending);
    gMirrorOrch->doTask();
    EXPECT_TRUE(gMirrorOrch->m_sampleBindings.at("Ethernet0|rx").cleanupPending);
    g_fail_mirror_session_set = false;
    gMirrorOrch->doTask();
    EXPECT_FALSE(gMirrorOrch->m_sampleBindings.at("Ethernet0|rx").cleanupPending);
    EXPECT_EQ(sampleOn("Ethernet0"), SAI_NULL_OBJECT_ID);
    EXPECT_EQ(gMirrorOrch->m_syncdMirrors.at("export1").refCount, 0);
    setAdmin("Ethernet0|rx", "up");
    EXPECT_NE(sampleOn("Ethernet0"), SAI_NULL_OBJECT_ID);
    remove("Ethernet0|rx");
    EXPECT_TRUE(gMirrorOrch->m_sampleBindings.empty());
}

TEST_F(SampleBindingTest, FailedSamplePacketDetachAndRemovalRetried)
{
    ASSERT_EQ(gMirrorOrch->setSampleBinding("Ethernet0|rx", fields()), task_success);
    auto oid = sampleOn("Ethernet0");

    g_fail_samplepacket_enable_set = true;
    remove("Ethernet0|rx");
    EXPECT_FALSE(gMirrorOrch->m_sampleBindings.at("Ethernet0|rx").attachments.at(portOid("Ethernet0")).mirrorSet);
    EXPECT_TRUE(gMirrorOrch->m_sampleBindings.at("Ethernet0|rx").attachments.at(portOid("Ethernet0")).sampleSet);
    verifyIngressSampleMirror("Ethernet0", SAI_NULL_OBJECT_ID);
    EXPECT_EQ(sampleOn("Ethernet0"), oid);
    EXPECT_EQ(gMirrorOrch->deleteEntry("export1"), task_need_retry);
    EXPECT_EQ(gMirrorOrch->setSampleBinding("Ethernet0|rx", fields("1000")), task_need_retry);

    g_fail_samplepacket_enable_set = false;
    g_fail_samplepacket_remove = true;
    static_cast<Orch*>(gMirrorOrch)->doTask();
    EXPECT_TRUE(gMirrorOrch->m_sampleBindings.at("Ethernet0|rx").attachments.empty());
    EXPECT_EQ(sampleOn("Ethernet0"), SAI_NULL_OBJECT_ID);
    EXPECT_NE(gMirrorOrch->m_sampleBindings.at("Ethernet0|rx").samplepacketId, SAI_NULL_OBJECT_ID);
    EXPECT_EQ(gMirrorOrch->m_syncdMirrors.at("export1").refCount, 1);

    g_fail_samplepacket_remove = false;
    static_cast<Orch*>(gMirrorOrch)->doTask();
    EXPECT_TRUE(gMirrorOrch->m_sampleBindings.empty());
    EXPECT_EQ(gMirrorOrch->m_syncdMirrors.at("export1").refCount, 0);
}

TEST_F(SampleBindingTest, PortChannelMembershipAndDirectMemberConflict)
{
    Port lag;
    lag.m_alias = "PortChannel10";
    lag.m_type = Port::LAG;
    lag.m_members.insert("Ethernet0");
    gPortsOrch->setPort(lag.m_alias, lag);
    EXPECT_EQ(gMirrorOrch->setSampleBinding("PortChannel10|rx", fields()), task_success);
    auto oid = sampleOn("Ethernet0");
    EXPECT_NE(oid, SAI_NULL_OBJECT_ID);
    lag.m_members.insert("Ethernet4");
    gPortsOrch->setPort(lag.m_alias, lag);
    Port member;
    ASSERT_TRUE(gPortsOrch->getPort("Ethernet4", member));
    gMirrorOrch->updateLagMember({lag, member, true});
    EXPECT_EQ(sampleOn("Ethernet4"), oid);
    EXPECT_EQ(gMirrorOrch->setSampleBinding("Ethernet4|rx", fields("1000")), task_success);
    EXPECT_EQ(sampleOn("Ethernet4"), oid);
    lag.m_members.erase("Ethernet4");
    gPortsOrch->setPort(lag.m_alias, lag);
    gMirrorOrch->updateLagMember({lag, member, false});
    gMirrorOrch->doTask();
    EXPECT_NE(sampleOn("Ethernet4"), SAI_NULL_OBJECT_ID);
    EXPECT_NE(sampleOn("Ethernet4"), oid);
    remove("PortChannel10|rx");
    EXPECT_EQ(sampleOn("Ethernet0"), SAI_NULL_OBJECT_ID);
    EXPECT_NE(sampleOn("Ethernet4"), SAI_NULL_OBJECT_ID);
}
TEST_F(SampleBindingTest, TruncationOnlyProgramsEveryPacket)
{
    const vector<FieldValueTuple> policy{{"mirror_session", "export1"}, {"truncate_size", "128"}};
    Table(m_config_db.get(), "PORT_MIRROR_SAMPLE").set("Ethernet0|rx", policy);
    ASSERT_EQ(gMirrorOrch->setSampleBinding("Ethernet0|rx", policy), task_success);
    setAdmin("Ethernet0|rx", "up");
    auto oid = sampleOn("Ethernet0");
    ASSERT_NE(oid, SAI_NULL_OBJECT_ID);
    sai_attribute_t attr{};
    attr.id = SAI_SAMPLEPACKET_ATTR_SAMPLE_RATE;
    ASSERT_EQ(sai_samplepacket_api->get_samplepacket_attribute(oid, 1, &attr), SAI_STATUS_SUCCESS);
    EXPECT_EQ(attr.value.u32, 1u);
    attr.id = SAI_SAMPLEPACKET_ATTR_TRUNCATE_ENABLE;
    ASSERT_EQ(sai_samplepacket_api->get_samplepacket_attribute(oid, 1, &attr), SAI_STATUS_SUCCESS);
    EXPECT_TRUE(attr.value.booldata);
    attr.id = SAI_SAMPLEPACKET_ATTR_TRUNCATE_SIZE;
    ASSERT_EQ(sai_samplepacket_api->get_samplepacket_attribute(oid, 1, &attr), SAI_STATUS_SUCCESS);
    EXPECT_EQ(attr.value.u32, 128u);
    EXPECT_EQ(gMirrorOrch->setSampleBinding("Ethernet0|rx", fields("1", "up", "128")), task_invalid_entry);
    remove("Ethernet0|rx");
}

TEST_F(SampleBindingTest, EmptyLagWaitsThenFollowsMembers)
{
    Port lag;
    lag.m_alias = "PortChannel20";
    lag.m_type = Port::LAG;
    gPortsOrch->setPort(lag.m_alias, lag);
    ASSERT_EQ(gMirrorOrch->setSampleBinding("PortChannel20|rx", fields()), task_success);
    EXPECT_EQ(gMirrorOrch->m_sampleBindings.at("PortChannel20|rx").samplepacketId, SAI_NULL_OBJECT_ID);
    lag.m_members.insert("Ethernet0");
    gPortsOrch->setPort(lag.m_alias, lag);
    Port member;
    ASSERT_TRUE(gPortsOrch->getPort("Ethernet0", member));
    gMirrorOrch->doTask();
    EXPECT_EQ(sampleOn("Ethernet0"), SAI_NULL_OBJECT_ID);
    gMirrorOrch->updateLagMember({lag, member, true});
    EXPECT_NE(sampleOn("Ethernet0"), SAI_NULL_OBJECT_ID);
    lag.m_members.clear();
    gPortsOrch->setPort(lag.m_alias, lag);
    gMirrorOrch->updateLagMember({lag, member, false});
    EXPECT_EQ(sampleOn("Ethernet0"), SAI_NULL_OBJECT_ID);
    EXPECT_EQ(gMirrorOrch->m_sampleBindings.at("PortChannel20|rx").samplepacketId, SAI_NULL_OBJECT_ID);
    EXPECT_TRUE(gMirrorOrch->sessionExists("export1"));
}

TEST_F(SampleBindingTest, MissingSourceWaitsForPhysicalPortAdd)
{
    Port port;
    ASSERT_TRUE(gPortsOrch->getPort("Ethernet4", port));
    port.m_alias = "Ethernet200";
    ASSERT_EQ(gMirrorOrch->setSampleBinding("Ethernet200|rx", fields()), task_success);
    gPortsOrch->setPort(port.m_alias, port);
    gPortsOrch->m_port_ref_count[port.m_alias] = 0;
    gMirrorOrch->doTask();
    EXPECT_EQ(sampleOn(port.m_alias), SAI_NULL_OBJECT_ID);
    PortUpdate added{port, true};
    gMirrorOrch->update(SUBJECT_TYPE_PORT_CHANGE, &added);
    EXPECT_NE(sampleOn(port.m_alias), SAI_NULL_OBJECT_ID);
}

TEST_F(SampleBindingTest, PartialLagMemberDetachIsCompletedOnRejoin)
{
    Port lag;
    lag.m_alias = "PortChannel10";
    lag.m_type = Port::LAG;
    lag.m_members.insert("Ethernet0");
    lag.m_members.insert("Ethernet4");
    gPortsOrch->setPort(lag.m_alias, lag);
    ASSERT_EQ(gMirrorOrch->setSampleBinding("PortChannel10|rx", fields()), task_success);
    Port member;
    ASSERT_TRUE(gPortsOrch->getPort("Ethernet4", member));

    lag.m_members.erase("Ethernet4");
    gPortsOrch->setPort(lag.m_alias, lag);
    g_fail_samplepacket_enable_set = true;
    gMirrorOrch->updateLagMember({lag, member, false});
    EXPECT_FALSE(gMirrorOrch->m_sampleBindings.at("PortChannel10|rx").attachments.at(member.m_port_id).mirrorSet);

    g_fail_samplepacket_enable_set = false;
    lag.m_members.insert("Ethernet4");
    gPortsOrch->setPort(lag.m_alias, lag);
    gMirrorOrch->updateLagMember({lag, member, true});
    EXPECT_NE(sampleOn("Ethernet4"), SAI_NULL_OBJECT_ID);
    verifyIngressSampleMirror(member.m_alias, gMirrorOrch->m_syncdMirrors.at("export1").sessionId);
    EXPECT_NE(sampleOn("Ethernet0"), SAI_NULL_OBJECT_ID);
}

TEST_F(SampleBindingTest, RemovingLastDisabledBindingDoesNotReleaseMirrorMode)
{
    vector<string> tables{APP_SFLOW_TABLE_NAME};
    SflowOrch sflow(m_app_db.get(), tables);
    ASSERT_EQ(gMirrorOrch->setSampleBinding("Ethernet4|rx", fields("5000", "down")), task_success);
    remove("Ethernet4|rx");
    ASSERT_TRUE(gMirrorOrch->m_sampleBindings.empty());
    sflowTask(sflow, APP_SFLOW_TABLE_NAME, "global", SET_COMMAND, {{"admin_state", "up"}});
    EXPECT_FALSE(sflow.m_sflowStatus);
}

TEST_F(SampleBindingTest, RemovingSflowGlobalAndRetryingPortCleanupDoesNotReleaseMode)
{
    vector<string> tables{APP_SFLOW_TABLE_NAME, APP_SFLOW_SESSION_TABLE_NAME};
    SflowOrch sflow(m_app_db.get(), tables);
    sflowTask(sflow, APP_SFLOW_TABLE_NAME, "global", SET_COMMAND, {{"admin_state", "up"}});
    sflowTask(sflow, APP_SFLOW_SESSION_TABLE_NAME, "Ethernet0", SET_COMMAND,
              {{"sample_rate", "5000"}, {"sample_direction", "both"}});
    auto oid = sampleOn("Ethernet0");
    ASSERT_NE(oid, SAI_NULL_OBJECT_ID);
    sflowTask(sflow, APP_SFLOW_TABLE_NAME, "global", DEL_COMMAND);
    auto consumer = dynamic_cast<Consumer*>(sflow.getExecutor(APP_SFLOW_SESSION_TABLE_NAME));
    ASSERT_NE(consumer, nullptr);
    consumer->addToSync(KeyOpFieldsValuesTuple("Ethernet0", DEL_COMMAND, {}));

    g_fail_egress_samplepacket_enable_set = true;
    sflow.doTask(*consumer);
    EXPECT_FALSE(consumer->m_toSync.empty());
    EXPECT_EQ(sampleOn("Ethernet0"), SAI_NULL_OBJECT_ID);
    EXPECT_EQ(sampleOn("Ethernet0", false), oid);

    EXPECT_EQ(gMirrorOrch->setSampleBinding("Ethernet0|rx", fields()), task_invalid_entry);
    EXPECT_EQ(sampleOn("Ethernet0"), SAI_NULL_OBJECT_ID);

    g_fail_egress_samplepacket_enable_set = false;
    sflow.doTask(*consumer);
    EXPECT_TRUE(consumer->m_toSync.empty());
    EXPECT_EQ(sampleOn("Ethernet0", false), SAI_NULL_OBJECT_ID);
    EXPECT_EQ(gMirrorOrch->setSampleBinding("Ethernet0|rx", fields()), task_invalid_entry);
    EXPECT_TRUE(gMirrorOrch->m_sampleBindings.empty());
}

TEST_F(SampleBindingTest, RetriedExportWithdrawalBlocksBindingsUntilNeighborRecovery)
{
    auto& session = gMirrorOrch->m_syncdMirrors.at("export1");
    session.status = false;
    session.sessionId = SAI_NULL_OBJECT_ID;
    session.srcIp = IpAddress("10.0.0.1");
    session.dstIp = IpAddress("10.0.0.2");
    const NextHopKey nextHop(session.dstIp, "Ethernet4");
    session.nexthopInfo.nexthop = nextHop;
    session.neighborInfo.portId = portOid("Ethernet4");
    ASSERT_TRUE(gPortsOrch->getPort("Ethernet4", session.neighborInfo.port));
    session.neighborInfo.mac = MacAddress("00:11:22:33:44:55");
    session.greType = 0x8949;
    ASSERT_TRUE(gMirrorOrch->activateSession("export1", session));
    ASSERT_EQ(gMirrorOrch->setSampleBinding("Ethernet0|tx", fields()), task_success);
    ASSERT_EQ(gMirrorOrch->setSampleBinding("Ethernet4|rx", fields()), task_success);
    g_fail_egress_samplepacket_enable_set = true;
    gMirrorOrch->updateNeighbor({nextHop, session.neighborInfo.mac, false});
    EXPECT_TRUE(session.status);
    EXPECT_EQ(session.refCount, 1);
    EXPECT_EQ(sampleOn("Ethernet4"), SAI_NULL_OBJECT_ID);
    ASSERT_EQ(gMirrorOrch->setSampleBinding("Ethernet8|rx", fields()), task_success);
    EXPECT_EQ(sampleOn("Ethernet8"), SAI_NULL_OBJECT_ID);

    g_fail_egress_samplepacket_enable_set = false;
    gMirrorOrch->doTask();
    EXPECT_FALSE(session.status);
    EXPECT_EQ(session.sessionId, SAI_NULL_OBJECT_ID);
    EXPECT_EQ(session.refCount, 0);
    EXPECT_EQ(sampleOn("Ethernet0", false), SAI_NULL_OBJECT_ID);
    EXPECT_EQ(sampleOn("Ethernet8"), SAI_NULL_OBJECT_ID);

    gNeighOrch->m_syncdNeighbors[nextHop].mac = session.neighborInfo.mac;
    gNeighOrch->m_syncdNextHops[nextHop] = {SAI_NULL_OBJECT_ID, 0, 0};
    gMirrorOrch->updateNeighbor({nextHop, session.neighborInfo.mac, true});
    ASSERT_TRUE(session.status);
    EXPECT_NE(sampleOn("Ethernet0", false), SAI_NULL_OBJECT_ID);
    EXPECT_NE(sampleOn("Ethernet4"), SAI_NULL_OBJECT_ID);
    EXPECT_NE(sampleOn("Ethernet8"), SAI_NULL_OBJECT_ID);
    ASSERT_TRUE(gMirrorOrch->deactivateSession("export1", session));
}

TEST_F(SampleBindingTest, LagMemberRemovalSkipsInactiveExport)
{
    Port lag;
    lag.m_alias = "PortChannel10";
    lag.m_type = Port::LAG;
    lag.m_members.insert("Ethernet4");
    lag.m_members.insert("Ethernet8");
    gPortsOrch->setPort(lag.m_alias, lag);
    auto& session = gMirrorOrch->m_syncdMirrors.at("export1");
    session.status = false;
    session.sessionId = SAI_NULL_OBJECT_ID;
    session.srcIp = IpAddress("10.0.0.1");
    session.dstIp = IpAddress("10.0.0.2");
    session.neighborInfo.port = lag;
    session.neighborInfo.portId = portOid("Ethernet4");
    session.neighborInfo.mac = MacAddress("00:11:22:33:44:55");
    session.greType = 0x8949;
    ASSERT_TRUE(gMirrorOrch->activateSession("export1", session));

    gMirrorOrch->updateNeighbor({NextHopKey(string("10.0.0.2"), lag.m_alias), session.neighborInfo.mac, false});
    ASSERT_FALSE(session.status);

    Port member;
    ASSERT_TRUE(gPortsOrch->getPort("Ethernet4", member));
    lag.m_members.erase("Ethernet4");
    gPortsOrch->setPort(lag.m_alias, lag);
    setSaiFailureStatus(false);
    gMirrorOrch->updateLagMember({lag, member, false});
    EXPECT_FALSE(session.status);
    EXPECT_EQ(session.neighborInfo.portId, portOid("Ethernet8"));
    string error;
    EXPECT_FALSE(getSaiFailureStatus(error)) << error;
}

class VlanFdbRecoveryTest : public SampleBindingTest
{
protected:
    Port vlan;
    Port oldMonitor;
    Port newMonitor;
    FdbEntry fdbEntry;
    const NextHopKey nextHop{string("10.0.0.2"), string("Vlan1000")};

    void setUpVlanExport()
    {
        ASSERT_TRUE(gPortsOrch->getPort("Ethernet4", oldMonitor));
        ASSERT_TRUE(gPortsOrch->getPort("Ethernet8", newMonitor));

        vlan = Port("Vlan1000", Port::VLAN);
        vlan.m_vlan_info.vlan_id = 1000;
        vlan.m_vlan_info.vlan_oid = 0x260000000001ULL;
        vlan.m_members.insert(oldMonitor.m_alias);
        vlan.m_members.insert(newMonitor.m_alias);
        gPortsOrch->setPort(vlan.m_alias, vlan);
        gPortsOrch->saiOidToAlias[vlan.m_vlan_info.vlan_oid] = vlan.m_alias;

        oldMonitor.m_bridge_port_id = 0x260000000002ULL;
        newMonitor.m_bridge_port_id = 0x260000000003ULL;
        gPortsOrch->setPort(oldMonitor.m_alias, oldMonitor);
        gPortsOrch->setPort(newMonitor.m_alias, newMonitor);
        gPortsOrch->saiOidToAlias[oldMonitor.m_bridge_port_id] = oldMonitor.m_alias;
        gPortsOrch->saiOidToAlias[newMonitor.m_bridge_port_id] = newMonitor.m_alias;

        fdbEntry.mac = MacAddress("00:11:22:33:44:55");
        fdbEntry.bv_id = vlan.m_vlan_info.vlan_oid;
        fdbEntry.port_name = oldMonitor.m_alias;
        FdbData data;
        data.bridge_port_id = oldMonitor.m_bridge_port_id;
        data.is_flush_pending = false;
        gFdbOrch->m_entries[fdbEntry] = data;

        gNeighOrch->m_syncdNeighbors[nextHop].mac = fdbEntry.mac;
        gNeighOrch->m_syncdNextHops[nextHop] = {SAI_NULL_OBJECT_ID, 0, 0};

        auto& session = gMirrorOrch->m_syncdMirrors.at("export1");
        session.status = false;
        session.sessionId = SAI_NULL_OBJECT_ID;
        session.srcIp = IpAddress("10.0.0.1");
        session.dstIp = IpAddress("10.0.0.2");
        session.greType = 0x8949;
        session.nexthopInfo.nexthop = nextHop;
        session.neighborInfo.neighbor = nextHop;
        session.neighborInfo.mac = fdbEntry.mac;
        session.neighborInfo.port = vlan;
        session.neighborInfo.portId = oldMonitor.m_port_id;
        ASSERT_TRUE(gMirrorOrch->activateSession("export1", session));
        ASSERT_EQ(gMirrorOrch->setSampleBinding("Ethernet0|rx", fields()), task_success);
        ASSERT_NE(sampleOn("Ethernet0"), SAI_NULL_OBJECT_ID);

        Port resolved;
        ASSERT_TRUE(gFdbOrch->getPort(fdbEntry.mac, vlan.m_vlan_info.vlan_id, resolved));
        ASSERT_EQ(resolved.m_port_id, oldMonitor.m_port_id);
    }

    void removeOldMemberWithFailedDetach()
    {
        vlan.m_members.erase(oldMonitor.m_alias);
        gPortsOrch->setPort(vlan.m_alias, vlan);
        gFdbOrch->m_entries[fdbEntry].is_flush_pending = true;
        g_fail_mirror_session_set = true;
        gMirrorOrch->updateVlanMember({vlan, oldMonitor, false});
        EXPECT_TRUE(gMirrorOrch->m_syncdMirrors.at("export1").status);
    }

    void addFreshFdb()
    {
        FdbData data;
        data.bridge_port_id = newMonitor.m_bridge_port_id;
        data.is_flush_pending = false;
        gFdbOrch->m_entries[fdbEntry] = data;
        FdbUpdate update{};
        update.entry = fdbEntry;
        update.entry.port_name = newMonitor.m_alias;
        update.port = newMonitor;
        update.add = true;
        gMirrorOrch->updateFdb(update);
    }

    void verifyMonitorPort(sai_object_id_t expected)
    {
        const auto& session = gMirrorOrch->m_syncdMirrors.at("export1");
        sai_attribute_t attr{};
        attr.id = SAI_MIRROR_SESSION_ATTR_MONITOR_PORT;
        ASSERT_EQ(sai_mirror_api->get_mirror_session_attribute(session.sessionId, 1, &attr), SAI_STATUS_SUCCESS);
        EXPECT_EQ(attr.value.oid, expected);
    }
};

TEST_F(VlanFdbRecoveryTest, RetryWaitsForFreshFdbAfterMonitorMemberRemoval)
{
    setUpVlanExport();
    removeOldMemberWithFailedDetach();
    g_fail_mirror_session_set = false;

    gMirrorOrch->doTask();
    auto& session = gMirrorOrch->m_syncdMirrors.at("export1");
    EXPECT_FALSE(session.status);

    gMirrorOrch->updateNeighbor({nextHop, fdbEntry.mac, true});
    EXPECT_FALSE(session.status);
    EXPECT_EQ(sampleOn("Ethernet0"), SAI_NULL_OBJECT_ID);

    FdbUpdate removed{};
    removed.entry = fdbEntry;
    removed.add = false;
    gMirrorOrch->updateFdb(removed);
    EXPECT_FALSE(session.status);

    addFreshFdb();
    EXPECT_TRUE(session.status);
    EXPECT_EQ(session.neighborInfo.portId, newMonitor.m_port_id);
    verifyMonitorPort(newMonitor.m_port_id);
    EXPECT_NE(sampleOn("Ethernet0"), SAI_NULL_OBJECT_ID);
    remove("Ethernet0|rx");
    if (session.status)
    {
        EXPECT_TRUE(gMirrorOrch->deactivateSession("export1", session));
    }
}

struct MirrorApiOverride
{
    sai_mirror_api_t* original = sai_mirror_api;
    sai_mirror_api_t api = *original;
    MirrorApiOverride() { sai_mirror_api = &api; }
    ~MirrorApiOverride() { sai_mirror_api = original; }
};

TEST_F(VlanFdbRecoveryTest, FreshFdbBeforeCleanupCompletesIsNotLost)
{
    setUpVlanExport();
    const auto mirror = gMirrorOrch->m_syncdMirrors.at("export1").sessionId;
    removeOldMemberWithFailedDetach();
    gMirrorOrch->updateNeighbor({nextHop, fdbEntry.mac, true});
    EXPECT_EQ(gMirrorOrch->m_syncdMirrors.at("export1").neighborInfo.portId, SAI_NULL_OBJECT_ID);
    addFreshFdb();
    EXPECT_TRUE(gMirrorOrch->m_syncdMirrors.at("export1").status);
    EXPECT_EQ(gMirrorOrch->m_syncdMirrors.at("export1").neighborInfo.portId, newMonitor.m_port_id);

    g_fail_mirror_session_set = false;
    {
        MirrorApiOverride probe;
        probe.api.set_mirror_session_attribute = [](sai_object_id_t, const sai_attribute_t* attr) -> sai_status_t {
            ADD_FAILURE() << "Fresh FDB already programmed the destination; unexpected write to " << attr->id;
            return SAI_STATUS_OBJECT_IN_USE;
        };
        gMirrorOrch->doTask();
    }
    auto& session = gMirrorOrch->m_syncdMirrors.at("export1");
    ASSERT_TRUE(session.status);
    EXPECT_EQ(session.sessionId, mirror);
    EXPECT_EQ(session.neighborInfo.portId, newMonitor.m_port_id);
    verifyMonitorPort(newMonitor.m_port_id);
    EXPECT_NE(sampleOn("Ethernet0"), SAI_NULL_OBJECT_ID);
    remove("Ethernet0|rx");
    if (session.status)
    {
        EXPECT_TRUE(gMirrorOrch->deactivateSession("export1", session));
    }
}

TEST_F(VlanFdbRecoveryTest, FailedMonitorUpdateCannotResumeSamplingFromCachedPort)
{
    setUpVlanExport();
    auto& session = gMirrorOrch->m_syncdMirrors.at("export1");
    const auto mirror = session.sessionId;
    removeOldMemberWithFailedDetach();
    g_fail_mirror_session_set = false;

    {
        MirrorApiOverride blocked;
        static bool monitorWriteAttempted;
        blocked.api.set_mirror_session_attribute = [](sai_object_id_t, const sai_attribute_t* attr) -> sai_status_t {
            EXPECT_EQ(attr->id, SAI_MIRROR_SESSION_ATTR_MONITOR_PORT);
            monitorWriteAttempted = true;
            return SAI_STATUS_OBJECT_IN_USE;
        };
        addFreshFdb();
        monitorWriteAttempted = false;
        gMirrorOrch->doTask();
        EXPECT_TRUE(monitorWriteAttempted);
    }

    ASSERT_EQ(session.sessionId, mirror);
    verifyMonitorPort(oldMonitor.m_port_id);
    EXPECT_EQ(sampleOn("Ethernet0"), SAI_NULL_OBJECT_ID);
    gMirrorOrch->doTask();
    ASSERT_EQ(session.sessionId, mirror);
    verifyMonitorPort(newMonitor.m_port_id);
    EXPECT_NE(sampleOn("Ethernet0"), SAI_NULL_OBJECT_ID);
    ASSERT_TRUE(gMirrorOrch->deactivateSession("export1", session));
}

TEST_F(VlanFdbRecoveryTest, DestinationRecoveryDuringRemovalRestoresSharedAclAndBinding)
{
    setUpVlanExport();
    auto& session = gMirrorOrch->m_syncdMirrors.at("export1");
    const auto oldExport = session.sessionId;
    const string table = "SHARED_MIRROR";
    auto addAclConfig = [&](const string& name, const string& key, const vector<FieldValueTuple>& values) {
        auto* consumer = dynamic_cast<Consumer*>(gAclOrch->getExecutor(name));
        ASSERT_NE(consumer, nullptr);
        consumer->addToSync(KeyOpFieldsValuesTuple(key, SET_COMMAND, values));
        consumer->drain();
        ASSERT_TRUE(consumer->m_toSync.empty());
    };
    addAclConfig(CFG_ACL_TABLE_TABLE_NAME, table, {{"type", "MIRROR"}, {"ports", "Ethernet0"}});
    addAclConfig(CFG_ACL_RULE_TABLE_NAME, table + "|rule", {{"DSCP", "8"}, {"MIRROR_INGRESS_ACTION", "export1"}});
    auto* rule = gAclOrch->getAclRule(table, "rule");
    ASSERT_NE(rule, nullptr);
    auto verifyAclDestination = [&](sai_object_id_t expected) {
        ASSERT_NE(rule->getOid(), SAI_NULL_OBJECT_ID);
        sai_object_id_t oid = SAI_NULL_OBJECT_ID;
        sai_attribute_t attr{};
        attr.id = SAI_ACL_ENTRY_ATTR_ACTION_MIRROR_INGRESS;
        attr.value.aclaction.parameter.objlist = {1, &oid};
        ASSERT_EQ(sai_acl_api->get_acl_entry_attribute(rule->getOid(), 1, &attr), SAI_STATUS_SUCCESS);
        EXPECT_TRUE(attr.value.aclaction.enable);
        EXPECT_EQ(attr.value.aclaction.parameter.objlist.count, 1u);
        EXPECT_EQ(oid, expected);
    };
    verifyAclDestination(oldExport);
    ASSERT_EQ(session.refCount, 2);

    removeOldMemberWithFailedDetach();
    g_fail_mirror_session_set = false;
    {
        MirrorApiOverride blocked;
        blocked.api.remove_mirror_session = [](sai_object_id_t) -> sai_status_t { return SAI_STATUS_OBJECT_IN_USE; };
        gMirrorOrch->doTask();
        ASSERT_EQ(session.sessionId, oldExport);
        ASSERT_TRUE(session.status);
        bool activeForAcl = true;
        ASSERT_TRUE(gMirrorOrch->getSessionStatus("export1", activeForAcl));
        EXPECT_FALSE(activeForAcl);
        ASSERT_EQ(rule->getOid(), SAI_NULL_OBJECT_ID);
        ASSERT_EQ(session.refCount, 0);
        addFreshFdb();
        gMirrorOrch->doTask();
        EXPECT_EQ(session.sessionId, oldExport);
        EXPECT_EQ(rule->getOid(), SAI_NULL_OBJECT_ID);
        EXPECT_EQ(sampleOn("Ethernet0"), SAI_NULL_OBJECT_ID);
        verifyMonitorPort(oldMonitor.m_port_id);
    }

    gMirrorOrch->doTask();
    ASSERT_TRUE(session.status);
    ASSERT_NE(session.sessionId, oldExport);
    verifyMonitorPort(newMonitor.m_port_id);
    verifyAclDestination(session.sessionId);
    verifyIngressSampleMirror("Ethernet0", session.sessionId);
    EXPECT_NE(sampleOn("Ethernet0"), SAI_NULL_OBJECT_ID);
    EXPECT_EQ(session.refCount, 2);
    EXPECT_TRUE(gAclOrch->removeAclRule(table, "rule"));
    EXPECT_TRUE(gAclOrch->removeAclTable(table));
    EXPECT_TRUE(gMirrorOrch->deactivateSession("export1", session));
}

    class MirrorOrchHwResourcesTest : public MirrorOrchTest
    {
    protected:
        static bool noMirrorSessionAvailable(sai_object_type_t objectType, uint64_t *count,
                                             sai_status_t *status)
        {
            if (objectType != SAI_OBJECT_TYPE_MIRROR_SESSION)
            {
                return false;
            }

            *count = 0;
            *status = SAI_STATUS_SUCCESS;
            return true;
        }

        vector<FieldValueTuple> erspanSession() const
        {
            vector<FieldValueTuple> data;
            data.emplace_back("type", "ERSPAN");
            data.emplace_back("src_ip", "10.0.0.1");
            data.emplace_back("dst_ip", "10.0.0.2");
            return data;
        }

        void eraseCreatedSession(const string& key)
        {
            gMirrorOrch->m_syncdMirrors.erase(key);
            swss::Table(m_state_db.get(), STATE_MIRROR_SESSION_TABLE_NAME).del(key);
        }

        void PreTearDown() override
        {
            gMirrorOrch->m_recoverySessionMap.clear();
        }
    };

    TEST_F(MirrorOrchHwResourcesTest, CreateEntryRejectsNewSessionWhenNoHwResources)
    {
        sai_availability_ut::ObjectTypeAvailabilityOverrideGuard guard(noMirrorSessionAvailable);

        // A session not in the recovery map must still be rejected when no HW resources are free
        ASSERT_EQ(gMirrorOrch->createEntry("new_session", erspanSession()),
                  task_process_status::task_failed);
        ASSERT_EQ(gMirrorOrch->m_syncdMirrors.count("new_session"), 0u);
    }

    TEST_F(MirrorOrchHwResourcesTest, CreateEntryAcceptsRecoveredSessionWhenNoHwResources)
    {
        sai_availability_ut::ObjectTypeAvailabilityOverrideGuard guard(noMirrorSessionAvailable);

        // Recovered sessions are the ones bake() found active before warm reboot
        gMirrorOrch->m_recoverySessionMap.emplace("recovered_session", "Ethernet0|10.0.0.3");

        ASSERT_EQ(gMirrorOrch->createEntry("recovered_session", erspanSession()),
                  task_process_status::task_success);
        ASSERT_EQ(gMirrorOrch->m_syncdMirrors.count("recovered_session"), 1u);

        eraseCreatedSession("recovered_session");
    }

}
