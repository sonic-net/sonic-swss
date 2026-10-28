#include <deque>
#include <algorithm>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "gtest/gtest.h"
#include "linklocalresyncstate.h"
#include "linklocalconfigconsumer.h"
#include "schema.h"
#include "table.h"

namespace
{

using swss::FieldValueTuple;
using swss::KeyOpFieldsValuesTuple;
using swss::LinkLocalResyncState;

KeyOpFieldsValuesTuple makeEntry(
    std::string key,
    std::string operation,
    std::vector<FieldValueTuple> fields = {})
{
    return {std::move(key), std::move(operation), std::move(fields)};
}

std::deque<KeyOpFieldsValuesTuple> entries(KeyOpFieldsValuesTuple entry)
{
    return {std::move(entry)};
}

TEST(LinkLocalResyncStateTest, IgnoresWrongTableBeforeAuthoritativeEnable)
{
    for (const std::string interface : {"Ethernet0.100", "PortChannel1.100", "Eth0.100", "Po1.100"})
    {
        for (const std::string table : {CFG_INTF_TABLE_NAME, CFG_LAG_INTF_TABLE_NAME, CFG_VLAN_INTF_TABLE_NAME})
        {
            SCOPED_TRACE(interface + " from " + table);
            LinkLocalResyncState state;
            const auto update = entries(makeEntry(
                interface, SET_COMMAND, {{"ipv6_use_link_local_only", "enable"}}));
            const auto wrong = state.process(table, update);
            EXPECT_TRUE(wrong.enabled.empty());
            EXPECT_TRUE(state.getPendingInterfaces().empty());
            state.markResyncComplete(interface);
            const auto correct = state.process(CFG_VLAN_SUB_INTF_TABLE_NAME, update);
            EXPECT_EQ(correct.enabled, std::set<std::string>({interface}));
            EXPECT_EQ(state.getPendingInterfaces(), std::set<std::string>({interface}));
        }
    }
}

TEST(LinkLocalResyncStateTest, InitialSnapshotEstablishesStateWithoutReplay)
{
    LinkLocalResyncState state;

    state.initialize(CFG_INTF_TABLE_NAME, entries(makeEntry(
        "Ethernet0", SET_COMMAND, {{"ipv6_use_link_local_only", "enable"}})));
    state.initialize(CFG_LAG_INTF_TABLE_NAME, entries(makeEntry("PortChannel0", SET_COMMAND, {{"mtu", "9100"}})));

    EXPECT_TRUE(state.getPendingInterfaces().empty());

    state.process(CFG_INTF_TABLE_NAME, entries(makeEntry(
        "Ethernet0", SET_COMMAND, {{"ipv6_use_link_local_only", "enable"}, {"mtu", "9100"}})));
    EXPECT_TRUE(state.getPendingInterfaces().empty());

    state.process(CFG_LAG_INTF_TABLE_NAME, entries(makeEntry(
        "PortChannel0", SET_COMMAND, {{"ipv6_use_link_local_only", "enable"}})));
    EXPECT_EQ(state.getPendingInterfaces(), std::set<std::string>({"PortChannel0"}));
}

TEST(LinkLocalResyncStateTest, EnabledInterfaceFullRowUpdateDoesNotScheduleReplay)
{
    LinkLocalResyncState state;

    state.initialize(CFG_INTF_TABLE_NAME, entries(makeEntry(
        "Ethernet0", SET_COMMAND, {{"ipv6_use_link_local_only", "enable"}})));
    state.process(CFG_INTF_TABLE_NAME, entries(makeEntry(
        "Ethernet0", SET_COMMAND, {{"ipv6_use_link_local_only", "enable"}, {"mtu", "9100"}})));

    EXPECT_TRUE(state.getPendingInterfaces().empty());
}

TEST(LinkLocalResyncStateTest, DuplicateSetPreservesPendingUntilReplayCompletes)
{
    LinkLocalResyncState state;

    state.process(CFG_INTF_TABLE_NAME, entries(makeEntry(
        "Ethernet0", SET_COMMAND, {{"ipv6_use_link_local_only", "enable"}})));
    EXPECT_EQ(state.getPendingInterfaces(), std::set<std::string>({"Ethernet0"}));

    state.process(CFG_INTF_TABLE_NAME, entries(makeEntry(
        "Ethernet0", SET_COMMAND, {{"ipv6_use_link_local_only", "enable"}, {"mtu", "9100"}})));
    EXPECT_EQ(state.getPendingInterfaces(), std::set<std::string>({"Ethernet0"}));

    state.markResyncComplete("Ethernet0");
    EXPECT_TRUE(state.getPendingInterfaces().empty());

    state.process(CFG_INTF_TABLE_NAME, entries(makeEntry(
        "Ethernet0", SET_COMMAND, {{"ipv6_use_link_local_only", "enable"}})));
    EXPECT_TRUE(state.getPendingInterfaces().empty());
}

TEST(LinkLocalResyncStateTest, ExplicitDisableCancelsPendingAndAllowsReenable)
{
    LinkLocalResyncState state;

    state.process(CFG_INTF_TABLE_NAME, entries(makeEntry(
        "Ethernet0", SET_COMMAND, {{"ipv6_use_link_local_only", "enable"}})));
    state.process(CFG_INTF_TABLE_NAME, entries(makeEntry(
        "Ethernet0", SET_COMMAND, {{"ipv6_use_link_local_only", "disable"}, {"mtu", "9100"}})));
    EXPECT_TRUE(state.getPendingInterfaces().empty());

    state.process(CFG_INTF_TABLE_NAME, entries(makeEntry(
        "Ethernet0", SET_COMMAND, {{"ipv6_use_link_local_only", "enable"}})));
    EXPECT_EQ(state.getPendingInterfaces(), std::set<std::string>({"Ethernet0"}));
}

TEST(LinkLocalResyncStateTest, DeletedRowCancelsPendingAndAllowsReenable)
{
    LinkLocalResyncState state;

    state.process(CFG_INTF_TABLE_NAME, entries(makeEntry(
        "Ethernet0", SET_COMMAND, {{"ipv6_use_link_local_only", "enable"}})));
    state.process(CFG_INTF_TABLE_NAME, entries(makeEntry("Ethernet0", DEL_COMMAND)));
    EXPECT_TRUE(state.getPendingInterfaces().empty());

    state.process(CFG_INTF_TABLE_NAME, entries(makeEntry(
        "Ethernet0", SET_COMMAND, {{"ipv6_use_link_local_only", "enable"}})));
    EXPECT_EQ(state.getPendingInterfaces(), std::set<std::string>({"Ethernet0"}));
}

TEST(LinkLocalResyncStateTest, AddressEntryDoesNotAffectInterfaceState)
{
    LinkLocalResyncState state;

    // The mode field is intentionally synthetic: address rows must be ignored
    // based on their key before any fields are interpreted.
    state.process(CFG_INTF_TABLE_NAME, entries(makeEntry(
        "Ethernet0|2001:db8::1/64",
        SET_COMMAND,
        {{"ipv6_use_link_local_only", "enable"}})));
    EXPECT_TRUE(state.getPendingInterfaces().empty());

    state.process(CFG_INTF_TABLE_NAME, entries(makeEntry(
        "Ethernet0", SET_COMMAND, {{"ipv6_use_link_local_only", "enable"}})));
    EXPECT_EQ(state.getPendingInterfaces(), std::set<std::string>({"Ethernet0"}));
}

TEST(LinkLocalResyncStateTest, CompletesPendingInterfacesIndependently)
{
    LinkLocalResyncState state;

    state.process(CFG_INTF_TABLE_NAME, {
        makeEntry("Ethernet0", SET_COMMAND, {{"ipv6_use_link_local_only", "enable"}}),
        makeEntry("Ethernet4", SET_COMMAND, {{"ipv6_use_link_local_only", "enable"}}),
    });

    state.markResyncComplete("Ethernet0");
    EXPECT_EQ(state.getPendingInterfaces(), std::set<std::string>({"Ethernet4"}));

    state.markResyncComplete("Ethernet4");
    EXPECT_TRUE(state.getPendingInterfaces().empty());
}

TEST(LinkLocalResyncStateTest, ReportsDisableAndReenableInOneBatch)
{
    LinkLocalResyncState state;

    state.process(CFG_INTF_TABLE_NAME, entries(makeEntry(
        "Ethernet0", SET_COMMAND, {{"ipv6_use_link_local_only", "enable"}})));
    const auto transitions = state.process(CFG_INTF_TABLE_NAME, {
        makeEntry("Ethernet0", SET_COMMAND, {{"ipv6_use_link_local_only", "disable"}}),
        makeEntry("Ethernet0", SET_COMMAND, {{"ipv6_use_link_local_only", "enable"}}),
    });

    EXPECT_EQ(transitions.disabled, std::set<std::string>({"Ethernet0"}));
    EXPECT_EQ(transitions.enabled, std::set<std::string>({"Ethernet0"}));
    EXPECT_EQ(state.getPendingInterfaces(), std::set<std::string>({"Ethernet0"}));
}

TEST(LinkLocalResyncStateTest, ReportsRepeatedAuthoritativeDisable)
{
    LinkLocalResyncState state;

    state.process(CFG_INTF_TABLE_NAME, entries(makeEntry("Ethernet0", SET_COMMAND, {{"mtu", "9100"}})));
    const auto transitions =
        state.process(CFG_INTF_TABLE_NAME, entries(makeEntry("Ethernet0", SET_COMMAND, {{"mtu", "9000"}})));

    EXPECT_EQ(transitions.disabled, std::set<std::string>({"Ethernet0"}));
    EXPECT_TRUE(transitions.enabled.empty());
    EXPECT_TRUE(state.getPendingInterfaces().empty());
}

TEST(LinkLocalResyncStateTest, SubPortsDoNotInheritParentModeOrSiblingTransitions)
{
    LinkLocalResyncState state;
    state.initialize(CFG_INTF_TABLE_NAME, entries(makeEntry(
        "Ethernet0", SET_COMMAND, {{"ipv6_use_link_local_only", "enable"}})));
    state.process(CFG_VLAN_SUB_INTF_TABLE_NAME, {
        makeEntry("Ethernet0.100", SET_COMMAND, {{"ipv6_use_link_local_only", "enable"}}),
        makeEntry("Ethernet0.200", SET_COMMAND, {{"ipv6_use_link_local_only", "enable"}}),
    });
    EXPECT_EQ(state.getPendingInterfaces(),
              std::set<std::string>({"Ethernet0.100", "Ethernet0.200"}));
    state.process(CFG_VLAN_SUB_INTF_TABLE_NAME, entries(makeEntry("Ethernet0.100", DEL_COMMAND)));
    EXPECT_EQ(state.getPendingInterfaces(), std::set<std::string>({"Ethernet0.200"}));
    state.process(CFG_INTF_TABLE_NAME, entries(makeEntry("Ethernet0", DEL_COMMAND)));
    EXPECT_EQ(state.getPendingInterfaces(), std::set<std::string>({"Ethernet0.200"}));
    state.markResyncComplete("Ethernet0.200");
    state.process(CFG_VLAN_SUB_INTF_TABLE_NAME, entries(makeEntry(
        "Ethernet0.200", SET_COMMAND,
        {{"ipv6_use_link_local_only", "enable"}, {"admin_status", "up"}})));
    EXPECT_TRUE(state.getPendingInterfaces().empty());
}

TEST(LinkLocalResyncStateTest, SubPortAddressUpdateAndDisableReenable)
{
    LinkLocalResyncState state;
    state.process(CFG_VLAN_SUB_INTF_TABLE_NAME, entries(makeEntry(
        "Po1.100", SET_COMMAND, {{"ipv6_use_link_local_only", "enable"}})));
    state.process(CFG_VLAN_SUB_INTF_TABLE_NAME, entries(makeEntry("Po1.100|2001:db8::1/64", DEL_COMMAND)));
    EXPECT_EQ(state.getPendingInterfaces(), std::set<std::string>({"Po1.100"}));
    state.process(CFG_VLAN_SUB_INTF_TABLE_NAME, entries(makeEntry(
        "Po1.100", SET_COMMAND, {{"ipv6_use_link_local_only", "disable"}})));
    EXPECT_TRUE(state.getPendingInterfaces().empty());
    state.process(CFG_VLAN_SUB_INTF_TABLE_NAME, entries(makeEntry(
        "Po1.100", SET_COMMAND, {{"ipv6_use_link_local_only", "enable"}})));
    EXPECT_EQ(state.getPendingInterfaces(), std::set<std::string>({"Po1.100"}));
}

struct StartupCase
{
    std::string interface;
    std::string wrongTable;
    bool enabled;
    bool wrongFirst;
};

std::vector<StartupCase> startupCases()
{
    std::vector<StartupCase> cases;
    for (const std::string interface : {"Ethernet0.100", "PortChannel1.100", "Eth0.100", "Po1.100"})
    {
        for (const std::string table : {CFG_INTF_TABLE_NAME, CFG_LAG_INTF_TABLE_NAME, CFG_VLAN_INTF_TABLE_NAME})
        {
            for (const bool enabled : {false, true})
            {
                for (const bool wrongFirst : {false, true})
                {
                    cases.push_back({interface, table, enabled, wrongFirst});
                }
            }
        }
    }
    return cases;
}

class LinkLocalStartupTest : public ::testing::TestWithParam<StartupCase>
{
};

TEST_P(LinkLocalStartupTest, UsesAuthoritativeSnapshotRegardlessOfTableOrder)
{
    const auto &param = GetParam();
    LinkLocalResyncState state;
    const auto correct = entries(makeEntry(param.interface, SET_COMMAND,
        {{"ipv6_use_link_local_only", param.enabled ? "enable" : "disable"}}));
    const auto wrong = entries(makeEntry(param.interface, SET_COMMAND,
        {{"ipv6_use_link_local_only", param.enabled ? "disable" : "enable"}}));
    if (param.wrongFirst)
    {
        state.initialize(param.wrongTable, wrong);
        EXPECT_TRUE(state.getPendingInterfaces().empty());
        state.initialize(CFG_VLAN_SUB_INTF_TABLE_NAME, correct);
    }
    else
    {
        state.initialize(CFG_VLAN_SUB_INTF_TABLE_NAME, correct);
        EXPECT_TRUE(state.getPendingInterfaces().empty());
        state.initialize(param.wrongTable, wrong);
    }
    EXPECT_TRUE(state.getPendingInterfaces().empty());
    const auto transitions = state.process(CFG_VLAN_SUB_INTF_TABLE_NAME,
        entries(makeEntry(param.interface, SET_COMMAND, {{"ipv6_use_link_local_only", "enable"}})));
    const auto expected = param.enabled ? std::set<std::string>{} : std::set<std::string>{param.interface};
    EXPECT_EQ(transitions.enabled, expected);
    EXPECT_TRUE(transitions.disabled.empty());
    EXPECT_EQ(state.getPendingInterfaces(), expected);
}

INSTANTIATE_TEST_SUITE_P(SourceAndOrder, LinkLocalStartupTest,
    ::testing::ValuesIn(startupCases()),
    [](const ::testing::TestParamInfo<StartupCase> &info) {
        std::string name = info.param.interface + "_" + info.param.wrongTable;
        std::replace(name.begin(), name.end(), '.', '_');
        name += info.param.enabled ? "_Enabled" : "_Disabled";
        name += info.param.wrongFirst ? "_WrongFirst" : "_AuthoritativeFirst";
        return name;
    });

TEST(LinkLocalResyncStateTest, WrongDisableOrDeletePreservesEnabledAndPending)
{
    for (const std::string interface : {"Ethernet0.100", "PortChannel1.100", "Eth0.100", "Po1.100"})
    {
        for (const std::string table : {CFG_INTF_TABLE_NAME, CFG_LAG_INTF_TABLE_NAME, CFG_VLAN_INTF_TABLE_NAME})
        {
            for (const std::string operation : {SET_COMMAND, DEL_COMMAND})
            {
                SCOPED_TRACE(interface + " from " + table + " " + operation);
                LinkLocalResyncState state;
                const auto enable = entries(makeEntry(interface, SET_COMMAND,
                    {{"ipv6_use_link_local_only", "enable"}}));
                state.process(CFG_VLAN_SUB_INTF_TABLE_NAME, enable);
                const auto wrong = state.process(table, entries(makeEntry(interface, operation,
                    {{"ipv6_use_link_local_only", "disable"}})));
                EXPECT_TRUE(wrong.enabled.empty());
                EXPECT_TRUE(wrong.disabled.empty());
                EXPECT_EQ(state.getPendingInterfaces(), std::set<std::string>({interface}));
                const auto duplicate = state.process(CFG_VLAN_SUB_INTF_TABLE_NAME, enable);
                EXPECT_TRUE(duplicate.enabled.empty());
                state.markResyncComplete(interface);
                EXPECT_TRUE(state.process(CFG_VLAN_SUB_INTF_TABLE_NAME, enable).enabled.empty());
                EXPECT_TRUE(state.getPendingInterfaces().empty());
            }
        }
    }
}

struct FakeConfigTable
{
    std::string name;
    std::deque<KeyOpFieldsValuesTuple> batch;
    int popCount = 0;

    const std::string &getTableName() const { return name; }
    void pops(std::deque<KeyOpFieldsValuesTuple> &updates)
    {
        ++popCount;
        updates.swap(batch);
    }
};

TEST(LinkLocalResyncStateTest, ConsumersPreserveBatchSourceAtStartupAndRuntime)
{
    const std::vector<std::pair<std::string, std::string>> sources = {
        {CFG_INTF_TABLE_NAME, "Ethernet0"},
        {CFG_LAG_INTF_TABLE_NAME, "PortChannel1"},
        {CFG_VLAN_INTF_TABLE_NAME, "Vlan100"},
        {CFG_VLAN_SUB_INTF_TABLE_NAME, "Ethernet0.100"},
    };
    for (const auto &source : sources)
    {
        for (const auto &row : sources)
        {
            SCOPED_TRACE(row.second + " from " + source.first);
            const auto enable = entries(makeEntry(row.second, SET_COMMAND,
                {{"ipv6_use_link_local_only", "enable"}}));
            FakeConfigTable initial{source.first, enable};
            LinkLocalResyncState state;
            swss::initializeLinkLocalConfigTable(initial, state);
            EXPECT_EQ(initial.popCount, 1);
            EXPECT_TRUE(initial.batch.empty());
            EXPECT_TRUE(state.getPendingInterfaces().empty());
            const auto correct = state.process(row.first, enable);
            const auto expected = source.first == row.first ? std::set<std::string>{} :
                std::set<std::string>{row.second};
            EXPECT_EQ(correct.enabled, expected);
            EXPECT_EQ(state.getPendingInterfaces(), expected);

            LinkLocalResyncState liveState;
            FakeConfigTable live{source.first, enable};
            const auto transitions = swss::processLinkLocalConfigTable(live, liveState);
            EXPECT_EQ(live.popCount, 1);
            EXPECT_TRUE(live.batch.empty());
            const auto liveExpected = source.first == row.first ? std::set<std::string>{row.second} :
                std::set<std::string>{};
            EXPECT_EQ(transitions.enabled, liveExpected);
            EXPECT_TRUE(transitions.disabled.empty());
            EXPECT_EQ(liveState.getPendingInterfaces(), liveExpected);
        }
    }
}

TEST(LinkLocalResyncStateTest, UnsupportedInterfacesDoNotAcquireState)
{
    LinkLocalResyncState state;
    const auto enable = entries(makeEntry("lo", SET_COMMAND, {{"ipv6_use_link_local_only", "enable"}}));
    for (const std::string table : {CFG_INTF_TABLE_NAME, CFG_LAG_INTF_TABLE_NAME,
                                   CFG_VLAN_INTF_TABLE_NAME, CFG_VLAN_SUB_INTF_TABLE_NAME})
    {
        state.initialize(table, enable);
        const auto transitions = state.process(table, enable);
        EXPECT_TRUE(transitions.enabled.empty());
        EXPECT_TRUE(transitions.disabled.empty());
        EXPECT_TRUE(state.getPendingInterfaces().empty());
    }
}

}
