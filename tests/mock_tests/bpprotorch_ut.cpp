#include <memory>
#include <regex>

#include "bpprotorch.h"
#include "mock_table.h"
#include "subscriberstatetable.h"

#include <gtest/gtest.h>

using namespace swss;

class BpProtOrchTest : public ::testing::Test
{
protected:
    std::unique_ptr<DBConnector> configDb;
    std::unique_ptr<DBConnector> stateDb;
    std::unique_ptr<BpProtOrch> orch;
    Consumer *consumer = nullptr;

    void SetUp() override
    {
        testing_db::reset();
        BpProtLog::setPriority("bpProtOrch", "NOTICE");
        BpProtLog::setOutput("bpProtOrch", "SYSLOG");
        configDb = std::make_unique<DBConnector>("CONFIG_DB", 0);
        stateDb = std::make_unique<DBConnector>("STATE_DB", 0);
        orch = std::make_unique<BpProtOrch>(configDb.get(), stateDb.get());
        consumer = dynamic_cast<Consumer *>(orch->getConsumerBase(CFG_BRIDGE_PORT_PROTECTION_TABLE_NAME));
        ASSERT_NE(consumer, nullptr);
    }

    void TearDown() override
    {
        orch.reset();
        BpProtLog::setPriority("bpProtOrch", "NOTICE");
        BpProtLog::setOutput("bpProtOrch", "SYSLOG");
        stateDb.reset();
        configDb.reset();
        testing_db::reset();
    }

    void apply(const std::string &key, const std::string &op,
               const std::vector<FieldValueTuple> &fields = {})
    {
        Table table(configDb.get(), CFG_BRIDGE_PORT_PROTECTION_TABLE_NAME);
        if (op == SET_COMMAND)
        {
            table.del(key);
            table.set(key, fields);
        }
        else if (op == DEL_COMMAND)
        {
            table.del(key);
        }
        consumer->addToSync(KeyOpFieldsValuesTuple(key, op, fields));
        static_cast<Orch &>(*orch).doTask();
        EXPECT_TRUE(consumer->m_toSync.empty());
    }

    void expectDefaults(const std::string &key)
    {
        const auto entry = orch->getEntry(key);
        ASSERT_NE(entry, nullptr);
        EXPECT_EQ(entry->protectionMode, "SOFTWARE");
        EXPECT_EQ(entry->adminMode, BpProtAdminMode::AUTO);
        EXPECT_FALSE(entry->revertive);
        EXPECT_EQ(entry->waitToRestoreTimeMs, 0u);
    }

    std::map<std::string, std::string> state(const std::string &key)
    {
        Table table(stateDb.get(), STATE_BRIDGE_PORT_PROTECTION_TABLE_NAME);
        std::vector<FieldValueTuple> fields;
        table.get(key, fields);
        return {fields.begin(), fields.end()};
    }
};

TEST_F(BpProtOrchTest, ConfigSubscription)
{
    EXPECT_EQ(consumer->getDbName(), "CONFIG_DB");
    EXPECT_EQ(consumer->getTableName(), "BRIDGE_PORT_PROTECTION");
    EXPECT_NE(dynamic_cast<SubscriberStateTable *>(consumer->getConsumerTable()), nullptr);
    EXPECT_EQ(orch->getEntry("PortChannel0"), nullptr);
}

TEST_F(BpProtOrchTest, DefaultsBeforePortCreation)
{
    // No PortsOrch or SAI fixture: policy consumption has no hardware dependency.
    apply("PortChannel0", SET_COMMAND);
    expectDefaults("PortChannel0");
}

TEST_F(BpProtOrchTest, ConfigDbEmptyObjectSentinelPublishesDefaults)
{
    apply("PortChannel0", SET_COMMAND, {{"NULL", "NULL"}});
    expectDefaults("PortChannel0");
    EXPECT_EQ(state("PortChannel0").at("admin_mode"), "AUTO");
    EXPECT_EQ(state("PortChannel0").count("protection_state"), 0u);
    EXPECT_EQ(state("PortChannel0").count("programmed_sai_admin_mode"), 0u);
}

TEST_F(BpProtOrchTest, EmptyObjectReplacementResetsAcceptedPolicy)
{
    apply("PortChannel0", SET_COMMAND, {
        {"admin_mode", "PROTECTION"}, {"revertive", "true"},
        {"wait_to_restore_time_ms", "5000"}
    });
    apply("PortChannel0", SET_COMMAND, {{"NULL", "NULL"}});
    expectDefaults("PortChannel0");
    EXPECT_FALSE(orch->getEntry("PortChannel0")->revertive);
    EXPECT_EQ(orch->getEntry("PortChannel0")->waitToRestoreTimeMs, 0u);
}

TEST_F(BpProtOrchTest, EmptyObjectSentinelDoesNotMaskExplicitPolicy)
{
    apply("PortChannel0", SET_COMMAND, {
        {"NULL", "NULL"}, {"admin_mode", "PROTECTION"},
        {"revertive", "true"}, {"wait_to_restore_time_ms", "5000"}
    });
    ASSERT_NE(orch->getEntry("PortChannel0"), nullptr);
    EXPECT_EQ(orch->getEntry("PortChannel0")->adminMode, BpProtAdminMode::PROTECTION);
    EXPECT_TRUE(orch->getEntry("PortChannel0")->revertive);
    EXPECT_EQ(orch->getEntry("PortChannel0")->waitToRestoreTimeMs, 5000u);
}

TEST_F(BpProtOrchTest, MalformedSentinelRowsPreserveAcceptedPolicy)
{
    apply("PortChannel0", SET_COMMAND, {{"admin_mode", "PRIMARY"}});
    const auto before = state("PortChannel0");
    const std::vector<std::vector<FieldValueTuple>> invalidRows = {
        {{"NULL", ""}}, {{"NULL", "null"}},
        {{"NULL", "NULL"}, {"unknown", "NULL"}},
        {{"NULL", "NULL"}, {"admin_mode", "invalid"}},
        {{"NULL", "NULL"}, {"NULL", "NULL"}}
    };
    for (const auto &fields : invalidRows)
    {
        apply("PortChannel0", SET_COMMAND, fields);
        EXPECT_EQ(state("PortChannel0"), before);
        EXPECT_EQ(orch->getEntry("PortChannel0")->adminMode, BpProtAdminMode::PRIMARY);
        apply("PortChannel1", SET_COMMAND, fields);
        EXPECT_EQ(orch->getEntry("PortChannel1"), nullptr);
        EXPECT_TRUE(state("PortChannel1").empty());
    }
}

TEST_F(BpProtOrchTest, ExplicitSoftwarePolicy)
{
    apply("PortChannel0", SET_COMMAND, {
        {"protection_mode", "SOFTWARE"}, {"admin_mode", "PROTECTION"},
        {"revertive", "true"}, {"wait_to_restore_time_ms", "5000"}
    });
    const auto entry = orch->getEntry("PortChannel0");
    ASSERT_NE(entry, nullptr);
    EXPECT_EQ(entry->protectionMode, "SOFTWARE");
    EXPECT_EQ(entry->adminMode, BpProtAdminMode::PROTECTION);
    EXPECT_TRUE(entry->revertive);
    EXPECT_EQ(entry->waitToRestoreTimeMs, 5000u);
}

TEST_F(BpProtOrchTest, AdminChangesAndRemovedFieldsResetToDefaults)
{
    apply("PortChannel0", SET_COMMAND, {
        {"admin_mode", "PROTECTION"}, {"revertive", "true"},
        {"wait_to_restore_time_ms", "5000"}
    });
    apply("PortChannel0", SET_COMMAND, {{"admin_mode", "PRIMARY"}});
    const auto entry = orch->getEntry("PortChannel0");
    ASSERT_NE(entry, nullptr);
    EXPECT_EQ(entry->adminMode, BpProtAdminMode::PRIMARY);
    EXPECT_FALSE(entry->revertive);
    EXPECT_EQ(entry->waitToRestoreTimeMs, 0u);
    apply("PortChannel0", SET_COMMAND, {{"admin_mode", "AUTO"}, {"revertive", "false"}});
    expectDefaults("PortChannel0");
}

TEST_F(BpProtOrchTest, TimerBoundsAndNonRevertivePolicy)
{
    for (const auto &value : {"0", "4294967295"})
    {
        apply("PortChannel0", SET_COMMAND, {{"wait_to_restore_time_ms", value}});
        const auto entry = orch->getEntry("PortChannel0");
        ASSERT_NE(entry, nullptr);
        EXPECT_EQ(entry->waitToRestoreTimeMs, value[0] == '0' ? 0u : UINT32_MAX);
        EXPECT_FALSE(entry->revertive);
    }
}

TEST_F(BpProtOrchTest, InvalidRowsAreAtomicAndDoNotCreateEntries)
{
    const std::vector<FieldValueTuple> invalidFields = {
        {"protection_mode", "HARDWARE"}, {"protection_mode", "software"},
        {"admin_mode", "auto"}, {"admin_mode", "UNKNOWN"},
        {"revertive", "yes"}, {"revertive", "TRUE"},
        {"wait_to_restore_time_ms", ""}, {"wait_to_restore_time_ms", "-1"},
        {"wait_to_restore_time_ms", "+1"}, {"wait_to_restore_time_ms", "1.5"},
        {"wait_to_restore_time_ms", " 1"}, {"wait_to_restore_time_ms", "1 "},
        {"wait_to_restore_time_ms", "4294967296"},
        {"wait_to_restore_time_ms", "18446744073709551616"},
        {"convergence_mode", "AUTO"}, {"t_failover", "100"}
    };
    apply("PortChannel0", SET_COMMAND, {{"admin_mode", "PRIMARY"}});
    for (const auto &invalid : invalidFields)
    {
        SCOPED_TRACE(fvField(invalid) + "=" + fvValue(invalid));
        // A valid field before the invalid field must not be applied either.
        const FieldValueTuple valid = fvField(invalid) == "revertive" ?
            FieldValueTuple("admin_mode", "PROTECTION") : FieldValueTuple("revertive", "true");
        const std::vector<FieldValueTuple> fields = {valid, invalid};
        apply("PortChannel0", SET_COMMAND, fields);
        const auto entry = orch->getEntry("PortChannel0");
        ASSERT_NE(entry, nullptr);
        EXPECT_EQ(entry->adminMode, BpProtAdminMode::PRIMARY);
        EXPECT_FALSE(entry->revertive);
        EXPECT_EQ(entry->waitToRestoreTimeMs, 0u);
        apply("PortChannel1", SET_COMMAND, fields);
        EXPECT_EQ(orch->getEntry("PortChannel1"), nullptr);
    }
}

TEST_F(BpProtOrchTest, DuplicateFieldsRejected)
{
    // Insert the malformed tuple directly; Consumer normally merges fields.
    KeyOpFieldsValuesTuple task("PortChannel0", SET_COMMAND,
        {{"admin_mode", "PRIMARY"}, {"admin_mode", "PROTECTION"}});
    Table table(configDb.get(), CFG_BRIDGE_PORT_PROTECTION_TABLE_NAME);
    table.set("PortChannel0", kfvFieldsValues(task));
    consumer->m_toSync.emplace("PortChannel0", task);
    static_cast<Orch &>(*orch).doTask();
    EXPECT_TRUE(consumer->m_toSync.empty());
    EXPECT_EQ(orch->getEntry("PortChannel0"), nullptr);
}

TEST_F(BpProtOrchTest, InvalidInterfaceNames)
{
    for (const auto &name : {"", "Ethernet0", "portchannel0", "PortChannel",
                             "PortChannel12345", "PortChannel0.100", "PortChannelX"})
    {
        SCOPED_TRACE(name);
        apply(name, SET_COMMAND);
        EXPECT_EQ(orch->getEntry(name), nullptr);
    }
    apply("PortChannel1234", SET_COMMAND);
    expectDefaults("PortChannel1234");
}

TEST_F(BpProtOrchTest, IndependentEntriesAndIdempotentDelete)
{
    apply("PortChannel0", SET_COMMAND);
    apply("PortChannel1", SET_COMMAND, {{"admin_mode", "PROTECTION"}});
    apply("PortChannel1", SET_COMMAND, {{"admin_mode", "PROTECTION"}});
    apply("PortChannel0", DEL_COMMAND);
    apply("PortChannel0", DEL_COMMAND);
    EXPECT_EQ(orch->getEntry("PortChannel0"), nullptr);
    ASSERT_NE(orch->getEntry("PortChannel1"), nullptr);
    EXPECT_EQ(orch->getEntry("PortChannel1")->adminMode, BpProtAdminMode::PROTECTION);
    apply("PortChannel0", SET_COMMAND);
    expectDefaults("PortChannel0");
}

TEST_F(BpProtOrchTest, UnsupportedOperationDrained)
{
    apply("PortChannel0", SET_COMMAND);
    apply("PortChannel0", "UNKNOWN");
    expectDefaults("PortChannel0");
}

TEST_F(BpProtOrchTest, ExistingConfigLoadedThroughTableSnapshot)
{
    Table table(configDb.get(), CFG_BRIDGE_PORT_PROTECTION_TABLE_NAME);
    table.set("PortChannel0", {{"admin_mode", "PRIMARY"}});
    // Use the non-destructive snapshot path. The shared SubscriberStateTable
    // mock deletes Redis rows in pops(), unlike the production subscriber.
    EXPECT_EQ(orch->addExistingData(&table), 1u);
    static_cast<Orch &>(*orch).doTask();
    ASSERT_NE(orch->getEntry("PortChannel0"), nullptr);
    EXPECT_EQ(orch->getEntry("PortChannel0")->adminMode, BpProtAdminMode::PRIMARY);
}

TEST_F(BpProtOrchTest, CoalescedNotificationsUseCurrentRow)
{
    Table table(configDb.get(), CFG_BRIDGE_PORT_PROTECTION_TABLE_NAME);
    const std::vector<FieldValueTuple> oldFields = {
        {"admin_mode", "PROTECTION"}, {"revertive", "true"},
        {"wait_to_restore_time_ms", "5000"}
    };
    table.set("PortChannel0", oldFields);
    consumer->addToSync(KeyOpFieldsValuesTuple("PortChannel0", SET_COMMAND, oldFields));
    table.del("PortChannel0");
    table.set("PortChannel0", {{"admin_mode", "AUTO"}});
    consumer->addToSync(KeyOpFieldsValuesTuple("PortChannel0", SET_COMMAND, {{"admin_mode", "AUTO"}}));
    static_cast<Orch &>(*orch).doTask();
    expectDefaults("PortChannel0");
    EXPECT_TRUE(consumer->m_toSync.empty());
}

TEST_F(BpProtOrchTest, StaleSetForDeletedRowClearsPolicy)
{
    apply("PortChannel0", SET_COMMAND);
    Table table(configDb.get(), CFG_BRIDGE_PORT_PROTECTION_TABLE_NAME);
    table.del("PortChannel0");
    consumer->addToSync(KeyOpFieldsValuesTuple("PortChannel0", SET_COMMAND, {{"admin_mode", "PROTECTION"}}));
    static_cast<Orch &>(*orch).doTask();
    EXPECT_EQ(orch->getEntry("PortChannel0"), nullptr);
    EXPECT_TRUE(state("PortChannel0").empty());
    EXPECT_TRUE(consumer->m_toSync.empty());
}

TEST_F(BpProtOrchTest, StateReportsHldFieldsWithoutInventingAnUnobservedPath)
{
    apply("PortChannel0", SET_COMMAND);
    const std::map<std::string, std::string> expected = {
        {"protection_type", "sw_protection"}, {"admin_mode", "AUTO"},
        {"reconciliation_state", "INCONSISTENT"}, {"convergence_mode", "n/a"},
        {"t_reconcile_armed_at", ""}, {"last_arbitration_outcome", "n/a"},
        {"last_arbitration_source", "n/a"}, {"last_arbitration_ts", ""},
        {"last_cache_update_ts", ""}, {"last_resync_ts", ""}
    };
    EXPECT_EQ(state("PortChannel0"), expected);
    // Publish only to STATE_DB, without rewriting operator configuration.
    Table configState(configDb.get(), STATE_BRIDGE_PORT_PROTECTION_TABLE_NAME);
    std::vector<FieldValueTuple> fields;
    EXPECT_FALSE(configState.get("PortChannel0", fields));
}

TEST_F(BpProtOrchTest, StateTracksIntentWithoutClaimingCommittedPath)
{
    for (const auto &mode : {"PRIMARY", "PROTECTION", "AUTO"})
    {
        apply("PortChannel0", SET_COMMAND, {
            {"admin_mode", mode}, {"revertive", "true"},
            {"wait_to_restore_time_ms", "5000"}
        });
        EXPECT_EQ(state("PortChannel0").at("admin_mode"), mode);
        EXPECT_EQ(state("PortChannel0").count("protection_state"), 0u);
        EXPECT_TRUE(orch->getEntry("PortChannel0")->revertive);
        EXPECT_EQ(orch->getEntry("PortChannel0")->waitToRestoreTimeMs, 5000u);
    }
    apply("PortChannel0", SET_COMMAND);
    EXPECT_FALSE(orch->getEntry("PortChannel0")->revertive);
    EXPECT_EQ(orch->getEntry("PortChannel0")->waitToRestoreTimeMs, 0u);
}

TEST_F(BpProtOrchTest, HldPublicationRemovesLegacyDiagnosticsAndUnverifiedPath)
{
    Table old(stateDb.get(), STATE_BRIDGE_PORT_PROTECTION_TABLE_NAME);
    old.set("PortChannel0", {{"protection_state", "PRIMARY"}, {"reconciliation_state", "ERROR"},
        {"desired_sai_admin_mode", "PROTECTION"}, {"last_event", "old"}, {"wtr_state", "IDLE"},
        {"bridge_port_oid", "123"}, {"retry_attempts", "99"}});
    apply("PortChannel0", SET_COMMAND);
    const auto actual = state("PortChannel0");
    EXPECT_EQ(actual.size(), 10u); EXPECT_EQ(actual.at("reconciliation_state"), "INCONSISTENT");
    for (const string removed : {"protection_state", "desired_sai_admin_mode", "last_event", "wtr_state", "bridge_port_oid", "retry_attempts"})
        EXPECT_FALSE(actual.count(removed));
    EXPECT_TRUE(actual.at("last_resync_ts").empty());
}

TEST_F(BpProtOrchTest, HldStateKeepsAcceptedPolicySeparateFromUnavailableSaiPath)
{
    for (const string admin : {"AUTO", "PRIMARY", "PROTECTION"})
    {
        apply("PortChannel0", SET_COMMAND, {{"admin_mode", admin}, {"revertive", "true"}, {"wait_to_restore_time_ms", "5000"}});
        auto actual = state("PortChannel0"); EXPECT_EQ(actual.at("admin_mode"), admin);
        EXPECT_EQ(actual.at("reconciliation_state"), "INCONSISTENT"); EXPECT_FALSE(actual.count("protection_state"));
        EXPECT_FALSE(actual.count("revertive")); EXPECT_FALSE(actual.count("wait_to_restore_time_ms"));
        EXPECT_TRUE(orch->getEntry("PortChannel0")->revertive); EXPECT_EQ(orch->getEntry("PortChannel0")->waitToRestoreTimeMs, 5000u);
        apply("PortChannel0", SET_COMMAND, {{"admin_mode", "invalid"}}); EXPECT_EQ(state("PortChannel0"), actual);
    }
}

TEST_F(BpProtOrchTest, InvalidUpdatePreservesLastAcceptedState)
{
    apply("PortChannel0", SET_COMMAND, {{"admin_mode", "PRIMARY"}});
    const auto before = state("PortChannel0");
    apply("PortChannel0", SET_COMMAND, {{"protection_mode", "HARDWARE"}});
    EXPECT_EQ(state("PortChannel0"), before);
    apply("PortChannel1", SET_COMMAND, {{"protection_mode", "HARDWARE"}});
    EXPECT_TRUE(state("PortChannel1").empty());
}

TEST_F(BpProtOrchTest, DeleteClearsOnlyMatchingStateRow)
{
    apply("PortChannel0", SET_COMMAND);
    apply("PortChannel1", SET_COMMAND);
    apply("PortChannel0", DEL_COMMAND);
    apply("PortChannel0", DEL_COMMAND);
    EXPECT_TRUE(state("PortChannel0").empty());
    EXPECT_EQ(state("PortChannel1").at("admin_mode"), "AUTO");
}

TEST_F(BpProtOrchTest, ComponentLoggerLoadsConfigAndDoesNotChangeProcessLevel)
{
    const auto before = Logger::getMinPrio();
    Table table(configDb.get(), "LOGGER");
    table.set("bpProtOrch", {{"LOGLEVEL", "DEBUG"}, {"LOGOUTPUT", "STDOUT"}});
    BpProtLog::initialize();
    EXPECT_EQ(Logger::getMinPrio(), before);
    testing::internal::CaptureStdout();
    apply("PortChannel0", SET_COMMAND);
    const auto output = testing::internal::GetCapturedStdout();
    EXPECT_NE(output.find("bpProtOrch:"), std::string::npos);
    EXPECT_NE(output.find("enter"), std::string::npos);
    EXPECT_NE(output.find("event=config_set ifname=PortChannel0 config_admin_mode=AUTO"), std::string::npos);
    for (const auto &field : state("PortChannel0"))
        EXPECT_NE(output.find("field=" + field.first + " value=" + field.second), std::string::npos);
}

TEST_F(BpProtOrchTest, ComponentVerbosityControlsConfigAndStateEvents)
{
    BpProtLog::setOutput("bpProtOrch", "STDOUT");
    testing::internal::CaptureStdout();
    apply("PortChannel0", SET_COMMAND);
    apply("PortChannel0", DEL_COMMAND);
    auto output = testing::internal::GetCapturedStdout();
    EXPECT_NE(output.find("event=config_set"), std::string::npos);
    EXPECT_NE(output.find("event=config_del ifname=PortChannel0 state_deleted=true"), std::string::npos);
    EXPECT_EQ(output.find("event=state_publish"), std::string::npos);
    EXPECT_EQ(output.find("enter"), std::string::npos);

    BpProtLog::setPriority("bpProtOrch", "INFO");
    testing::internal::CaptureStdout();
    apply("PortChannel0", SET_COMMAND);
    output = testing::internal::GetCapturedStdout();
    EXPECT_NE(output.find("event=state_publish"), std::string::npos);
    EXPECT_EQ(output.find("enter"), std::string::npos);

    BpProtLog::setPriority("bpProtOrch", "ERROR");
    testing::internal::CaptureStdout();
    apply("PortChannel0", SET_COMMAND);
    apply("PortChannel1", SET_COMMAND, {{"admin_mode", "bad"}});
    apply("PortChannel0", "BAD_OP");
    output = testing::internal::GetCapturedStdout();
    EXPECT_EQ(output.find("event=config_set"), std::string::npos);
    EXPECT_NE(output.find("event=config_reject ifname=PortChannel1 reason=invalid_policy_or_interface"), std::string::npos);
    EXPECT_NE(output.find("reason=unsupported_operation op=BAD_OP"), std::string::npos);
}

TEST_F(BpProtOrchTest, LoggerDefaultsInvalidSettingsAndStaleSetEvent)
{
    BpProtLog::initialize();
    Table table(configDb.get(), "LOGGER");
    std::string value;
    ASSERT_TRUE(table.hget("bpProtOrch", "LOGLEVEL", value));
    EXPECT_EQ(value, "NOTICE");
    ASSERT_TRUE(table.hget("bpProtOrch", "LOGOUTPUT", value));
    EXPECT_EQ(value, "SYSLOG");
    BpProtLog::setPriority("bpProtOrch", "invalid");
    BpProtLog::setOutput("bpProtOrch", "invalid");
    apply("PortChannel0", SET_COMMAND); // Exercises default SYSLOG output.
    BpProtLog::setOutput("bpProtOrch", "STDERR");
    testing::internal::CaptureStderr();
    Table policyTable(configDb.get(), CFG_BRIDGE_PORT_PROTECTION_TABLE_NAME);
    policyTable.del("PortChannel0");
    consumer->addToSync(KeyOpFieldsValuesTuple("PortChannel0", SET_COMMAND, {}));
    static_cast<Orch &>(*orch).doTask();
    const auto output = testing::internal::GetCapturedStderr();
    EXPECT_NE(output.find("event=stale_set ifname=PortChannel0 state_deleted=true"), std::string::npos);
    EXPECT_TRUE(state("PortChannel0").empty());
}
