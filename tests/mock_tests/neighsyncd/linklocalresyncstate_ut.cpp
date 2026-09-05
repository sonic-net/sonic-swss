#include <deque>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "gtest/gtest.h"
#include "linklocalresyncstate.h"
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

TEST(LinkLocalResyncStateTest, InitialSnapshotEstablishesStateWithoutReplay)
{
    LinkLocalResyncState state;

    state.initialize(entries(makeEntry(
        "Ethernet0", SET_COMMAND, {{"ipv6_use_link_local_only", "enable"}})));
    state.initialize(entries(makeEntry("PortChannel0", SET_COMMAND, {{"mtu", "9100"}})));

    EXPECT_TRUE(state.getPendingInterfaces().empty());

    state.process(entries(makeEntry(
        "Ethernet0", SET_COMMAND, {{"ipv6_use_link_local_only", "enable"}, {"mtu", "9100"}})));
    EXPECT_TRUE(state.getPendingInterfaces().empty());

    state.process(entries(makeEntry(
        "PortChannel0", SET_COMMAND, {{"ipv6_use_link_local_only", "enable"}})));
    EXPECT_EQ(state.getPendingInterfaces(), std::set<std::string>({"PortChannel0"}));
}

TEST(LinkLocalResyncStateTest, EnabledInterfaceFullRowUpdateDoesNotScheduleReplay)
{
    LinkLocalResyncState state;

    state.initialize(entries(makeEntry(
        "Ethernet0", SET_COMMAND, {{"ipv6_use_link_local_only", "enable"}})));
    state.process(entries(makeEntry(
        "Ethernet0", SET_COMMAND, {{"ipv6_use_link_local_only", "enable"}, {"mtu", "9100"}})));

    EXPECT_TRUE(state.getPendingInterfaces().empty());
}

TEST(LinkLocalResyncStateTest, DuplicateSetPreservesPendingUntilReplayCompletes)
{
    LinkLocalResyncState state;

    state.process(entries(makeEntry(
        "Ethernet0", SET_COMMAND, {{"ipv6_use_link_local_only", "enable"}})));
    EXPECT_EQ(state.getPendingInterfaces(), std::set<std::string>({"Ethernet0"}));

    state.process(entries(makeEntry(
        "Ethernet0", SET_COMMAND, {{"ipv6_use_link_local_only", "enable"}, {"mtu", "9100"}})));
    EXPECT_EQ(state.getPendingInterfaces(), std::set<std::string>({"Ethernet0"}));

    state.markResyncComplete();
    EXPECT_TRUE(state.getPendingInterfaces().empty());

    state.process(entries(makeEntry(
        "Ethernet0", SET_COMMAND, {{"ipv6_use_link_local_only", "enable"}})));
    EXPECT_TRUE(state.getPendingInterfaces().empty());
}

TEST(LinkLocalResyncStateTest, ExplicitDisableCancelsPendingAndAllowsReenable)
{
    LinkLocalResyncState state;

    state.process(entries(makeEntry(
        "Ethernet0", SET_COMMAND, {{"ipv6_use_link_local_only", "enable"}})));
    state.process(entries(makeEntry(
        "Ethernet0", SET_COMMAND, {{"ipv6_use_link_local_only", "disable"}, {"mtu", "9100"}})));
    EXPECT_TRUE(state.getPendingInterfaces().empty());

    state.process(entries(makeEntry(
        "Ethernet0", SET_COMMAND, {{"ipv6_use_link_local_only", "enable"}})));
    EXPECT_EQ(state.getPendingInterfaces(), std::set<std::string>({"Ethernet0"}));
}

TEST(LinkLocalResyncStateTest, DeletedRowCancelsPendingAndAllowsReenable)
{
    LinkLocalResyncState state;

    state.process(entries(makeEntry(
        "Ethernet0", SET_COMMAND, {{"ipv6_use_link_local_only", "enable"}})));
    state.process(entries(makeEntry("Ethernet0", DEL_COMMAND)));
    EXPECT_TRUE(state.getPendingInterfaces().empty());

    state.process(entries(makeEntry(
        "Ethernet0", SET_COMMAND, {{"ipv6_use_link_local_only", "enable"}})));
    EXPECT_EQ(state.getPendingInterfaces(), std::set<std::string>({"Ethernet0"}));
}

TEST(LinkLocalResyncStateTest, AddressEntryDoesNotAffectInterfaceState)
{
    LinkLocalResyncState state;

    // The mode field is intentionally synthetic: address rows must be ignored
    // based on their key before any fields are interpreted.
    state.process(entries(makeEntry(
        "Ethernet0|2001:db8::1/64",
        SET_COMMAND,
        {{"ipv6_use_link_local_only", "enable"}})));
    EXPECT_TRUE(state.getPendingInterfaces().empty());

    state.process(entries(makeEntry(
        "Ethernet0", SET_COMMAND, {{"ipv6_use_link_local_only", "enable"}})));
    EXPECT_EQ(state.getPendingInterfaces(), std::set<std::string>({"Ethernet0"}));
}

}
