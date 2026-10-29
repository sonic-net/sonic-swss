// Run with run_bpprotorch_configdb.py against its private Redis instance.
#include "bpprotorch.h"
#include "select.h"
#include <cstdlib>
#include <gtest/gtest.h>

using namespace swss;

// Production capability globals construct STATE_DB tables before main. Prime
// the private mapping first so those native objects also stay in the fixture.
__attribute__((constructor(101))) static void initializePrivateDatabase()
{
    const char *file = std::getenv("BP_PROTECTION_TEST_DB_CONFIG");
    if (!file) std::abort();
    SonicDBConfig::reset();
    SonicDBConfig::initialize(file);
}

TEST(BpProtConfigDbIntegration, EmptyObjectLifecycle)
{
    DBConnector config("CONFIG_DB", 0), state("STATE_DB", 0);
    Table configTable(&config, CFG_BRIDGE_PORT_PROTECTION_TABLE_NAME);
    Table stateTable(&state, STATE_BRIDGE_PORT_PROTECTION_TABLE_NAME);
    std::vector<FieldValueTuple> fields;
    ASSERT_TRUE(configTable.get("PortChannel0", fields));
    ASSERT_EQ(fields, (std::vector<FieldValueTuple>{{"NULL", "NULL"}}));

    BpProtOrch orch(&config, &state);
    // Native Redis policy is visible even before the CONFIG Consumer executes.
    EXPECT_EQ(orch.isProtectedBridgePort("PortChannel0"), BpProtectionStatus::PENDING);
    ASSERT_EQ(orch.addExistingData(&configTable), 1u);
    static_cast<Orch &>(orch).doTask();
    ASSERT_NE(orch.getEntry("PortChannel0"), nullptr);
    EXPECT_EQ(orch.getEntry("PortChannel0")->adminMode, BpProtAdminMode::AUTO);
    auto field = [&](const char *name) {
        std::string value;
        EXPECT_TRUE(stateTable.hget("PortChannel0", name, value));
        return value;
    };
    std::string absent;
    EXPECT_FALSE(stateTable.hget("PortChannel0", "protection_state", absent));
    EXPECT_FALSE(stateTable.hget("PortChannel0", "programmed_sai_admin_mode", absent));
    EXPECT_EQ(field("admin_mode"), "AUTO");

    Select select;
    for (auto selectable : orch.getSelectables()) select.addSelectable(selectable);
    auto drain = [&]() {
        Selectable *ready = nullptr;
        ASSERT_EQ(select.select(&ready, 2000), Select::OBJECT);
        auto executor = dynamic_cast<Executor *>(ready);
        ASSERT_NE(executor, nullptr);
        executor->execute();
        while (select.select(&ready, 20) == Select::OBJECT)
            dynamic_cast<Executor *>(ready)->execute();
    };
    // Incremental fields can coexist with the empty-object sentinel.
    configTable.set("PortChannel0", {{"admin_mode", "PROTECTION"},
        {"revertive", "true"}, {"wait_to_restore_time_ms", "5000"}});
    drain();
    EXPECT_EQ(field("admin_mode"), "PROTECTION");
    EXPECT_EQ(orch.getEntry("PortChannel0")->waitToRestoreTimeMs, 5000u);

    // A replacement with a defaults-only object clears the prior policy.
    configTable.del("PortChannel0");
    configTable.set("PortChannel0", {{"NULL", "NULL"}});
    drain();
    EXPECT_EQ(field("admin_mode"), "AUTO");
    EXPECT_FALSE(orch.getEntry("PortChannel0")->revertive);
    EXPECT_EQ(orch.getEntry("PortChannel0")->waitToRestoreTimeMs, 0u);
    configTable.set("PortChannel0", {{"NULL", "invalid"}});
    drain();
    EXPECT_EQ(field("admin_mode"), "AUTO");
    configTable.del("PortChannel0");
    EXPECT_EQ(orch.isProtectedBridgePort("PortChannel0"), BpProtectionStatus::PENDING);
    drain();
    EXPECT_EQ(orch.getEntry("PortChannel0"), nullptr);
    EXPECT_FALSE(stateTable.get("PortChannel0", fields));
    EXPECT_EQ(orch.isProtectedBridgePort("PortChannel0"), BpProtectionStatus::NONE);
}

int main(int argc, char **argv)
{
    if (argc != 2) return 2;
    if (std::string(argv[1]) != std::getenv("BP_PROTECTION_TEST_DB_CONFIG")) return 2;
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
