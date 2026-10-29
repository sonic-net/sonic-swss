#include <chrono>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <linux/if.h>
#include <netlink/route/link.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include "gtest/gtest.h"
#define protected public
#define private public
#include "teamsync.h"
#undef protected
#undef private
#include "mock_table.h"

namespace teamsync_test
{
    void setTeamSyncSuccessMocks();
    void resetTeamSyncMocks();
}

namespace teamsyncd_ut
{
    extern "C" bool team_change_handler_registered(team_handle *team);
    extern "C" int team_change_handler_dispatch(team_handle *team);

    static std::map<team_handle *, int> eventFds;
    static size_t handledEvents;

    extern "C" int team_get_event_fd(team_handle *team)
    {
        auto it = eventFds.find(team);
        if (it == eventFds.end())
        {
            it = eventFds.emplace(team, eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC)).first;
        }
        return it->second;
    }

    extern "C" int team_handle_events(team_handle *team)
    {
        eventfd_t value;
        int err = eventfd_read(team_get_event_fd(team), &value);
        if (err || !team_change_handler_registered(team))
        {
            return err;
        }
        ++handledEvents;
        return team_change_handler_dispatch(team);
    }

    struct TeamSyncdTest : public ::testing::Test
    {
        std::shared_ptr<swss::DBConnector> m_config_db;
        std::shared_ptr<swss::DBConnector> m_app_db;
        std::shared_ptr<swss::DBConnector> m_state_db;

        std::shared_ptr<swss::Table> m_stateWarmRestartTable;

        void SetUp() override
        {
            handledEvents = 0;
            testing_db::reset();
            teamsync_test::setTeamSyncSuccessMocks();
            m_config_db = std::make_shared<swss::DBConnector>("CONFIG_DB", 0);
            m_app_db = std::make_shared<swss::DBConnector>("APPL_DB", 0);
            m_state_db = std::make_shared<swss::DBConnector>("STATE_DB", 0);
        }

        void TearDown() override
        {
            for (const auto &entry : eventFds)
            {
                close(entry.second);
            }
            eventFds.clear();
            teamsync_test::resetTeamSyncMocks();
            testing_db::reset();
        }

        void linkEvent(swss::TeamSync &sync, int type, int ifindex,
                       unsigned int flags = IFF_UP | IFF_LOWER_UP, unsigned int mtu = 1500)
        {
            auto link = rtnl_link_alloc();
            ASSERT_NE(link, nullptr);
            rtnl_link_set_name(link, "PortChannel1");
            ASSERT_EQ(rtnl_link_set_type(link, "team"), 0);
            rtnl_link_set_ifindex(link, ifindex);
            rtnl_link_set_flags(link, flags);
            rtnl_link_set_mtu(link, mtu);
            sync.onMsg(type, reinterpret_cast<nl_object *>(link));
            rtnl_link_put(link);
        }

        void expectState(bool present)
        {
            swss::Table app(m_app_db.get(), APP_LAG_TABLE_NAME);
            swss::Table state(m_state_db.get(), STATE_LAG_TABLE_NAME);
            std::string value;
            EXPECT_EQ(app.hget("PortChannel1", "state", value), present);
            EXPECT_EQ(state.hget("PortChannel1", "state", value), present);
        }

    };

    TEST_F(TeamSyncdTest, testAddingLagOnWarmBootSetsStateDbFlag)
    {
        swss::TeamSync sync(m_app_db.get(), m_state_db.get(), nullptr);
        swss::Table stateLagTable(m_state_db.get(), STATE_LAG_TABLE_NAME);

        sync.m_warmstart = true;

        const bool admin_state = true;
        const bool oper_state = true;
        const int if_index = 1;
        const unsigned int mtu = 1500;
        const char* lag_name = "PortChannel1";
        sync.addLag({lag_name, if_index}, admin_state, oper_state, mtu);

        std::string okValue;
        const bool found = stateLagTable.hget(lag_name, "state", okValue);
        ASSERT_TRUE(found);
        ASSERT_EQ(okValue, "ok");
    }

    TEST_F(TeamSyncdTest, RecreateBeforePeriodicRetiresOnlyOldLag)
    {
        swss::Select select;
        swss::TeamSync sync(m_app_db.get(), m_state_db.get(), &select);
        linkEvent(sync, RTM_NEWLINK, 10);
        sync.periodic();
        auto old = sync.m_teamSelectables.at({"PortChannel1", 10});
        ASSERT_NE(old, nullptr);
        const int oldFd = old->getFd();
        ASSERT_GE(oldFd, 0);
        team_change_handler otherHandler{};
        team_change_handler_unregister(old->m_team, &otherHandler, old.get());
        EXPECT_TRUE(team_change_handler_registered(old->m_team));
        team_change_handler_unregister(
                old->m_team, &swss::TeamSync::TeamPortSync::gPortChangeHandler, nullptr);
        EXPECT_TRUE(team_change_handler_registered(old->m_team));

        linkEvent(sync, RTM_DELLINK, 10);
        linkEvent(sync, RTM_NEWLINK, 20);
        auto replacement = sync.m_teamSelectables.at({"PortChannel1", 20});
        ASSERT_NE(replacement, nullptr);
        ASSERT_NE(old, replacement);
        EXPECT_EQ(sync.m_teamSelectables.begin()->first.second, 20);
        EXPECT_FALSE(team_change_handler_registered(old->m_team));
        EXPECT_TRUE(team_change_handler_registered(replacement->m_team));

        // Consume an already-ready old fd without dispatching its unregistered callback.
        ASSERT_EQ(eventfd_write(oldFd, 1), 0);
        old->readData();
        EXPECT_EQ(handledEvents, 0u);

        std::weak_ptr<swss::TeamSync::TeamPortSync> retired = old;
        old.reset();
        sync.periodic();
        EXPECT_TRUE(retired.expired());
        expectState(true);
        ASSERT_EQ(eventfd_write(replacement->getFd(), 1), 0);
        swss::Selectable *ready = nullptr;
        EXPECT_EQ(select.select(&ready, 0), swss::Select::OBJECT);
        EXPECT_EQ(ready, replacement.get());
        EXPECT_EQ(handledEvents, 1u);
    }

    TEST_F(TeamSyncdTest, AddDeleteAddInOneBatchRegistersOnlyReplacement)
    {
        swss::Select select;
        swss::TeamSync sync(m_app_db.get(), m_state_db.get(), &select);
        linkEvent(sync, RTM_NEWLINK, 10);
        auto old = sync.m_teamSelectables.at({"PortChannel1", 10});
        ASSERT_NE(old, nullptr);
        linkEvent(sync, RTM_DELLINK, 10);
        linkEvent(sync, RTM_NEWLINK, 20);
        auto replacement = sync.m_teamSelectables.at({"PortChannel1", 20});
        ASSERT_NE(replacement, nullptr);
        ASSERT_NE(old, replacement);
        EXPECT_EQ(sync.m_selectablesToAdd.size(), 1u);
        EXPECT_EQ(sync.m_selectablesToAdd.count(replacement), 1u);

        sync.periodic();
        ASSERT_EQ(eventfd_write(old->getFd(), 1), 0);
        ASSERT_EQ(eventfd_write(replacement->getFd(), 1), 0);
        swss::Selectable *ready = nullptr;
        EXPECT_EQ(select.select(&ready, 0), swss::Select::OBJECT);
        EXPECT_EQ(ready, replacement.get());
        EXPECT_EQ(handledEvents, 1u);
        expectState(true);
    }

    TEST_F(TeamSyncdTest, OldDeleteCannotConsumeReplacementOrItsMembers)
    {
        swss::Select select;
        swss::TeamSync sync(m_app_db.get(), m_state_db.get(), &select);
        linkEvent(sync, RTM_NEWLINK, 10);
        linkEvent(sync, RTM_NEWLINK, 20);
        auto replacement = sync.m_teamSelectables.at({"PortChannel1", 20});
        ASSERT_NE(replacement, nullptr);
        replacement->m_lagMembers["Ethernet0"] = true;
        sync.m_lagMemberTable.set("PortChannel1:Ethernet0", {{"status", "enabled"}});

        linkEvent(sync, RTM_DELLINK, 10);
        EXPECT_EQ(sync.m_teamSelectables.at({"PortChannel1", 20}), replacement);
        EXPECT_EQ(sync.m_teamSelectables.begin()->first.second, 20);
        expectState(true);
        swss::Table members(m_app_db.get(), APP_LAG_MEMBER_TABLE_NAME);
        std::string value;
        EXPECT_TRUE(members.hget("PortChannel1:Ethernet0", "status", value));

        linkEvent(sync, RTM_DELLINK, 20);
        linkEvent(sync, RTM_DELLINK, 20);
        sync.periodic();
        EXPECT_TRUE(sync.m_teamSelectables.empty());
        EXPECT_FALSE(members.hget("PortChannel1:Ethernet0", "status", value));
        expectState(false);
    }

    TEST_F(TeamSyncdTest, ReusedIfindexAfterDeleteStillCreatesANewHandler)
    {
        swss::Select select;
        swss::TeamSync sync(m_app_db.get(), m_state_db.get(), &select);
        linkEvent(sync, RTM_NEWLINK, 10);
        sync.periodic();
        auto old = sync.m_teamSelectables.at({"PortChannel1", 10});
        ASSERT_NE(old, nullptr);
        linkEvent(sync, RTM_DELLINK, 10);
        linkEvent(sync, RTM_NEWLINK, 10);
        auto replacement = sync.m_teamSelectables.at({"PortChannel1", 10});
        ASSERT_NE(replacement, nullptr);
        ASSERT_NE(old, replacement);
        sync.periodic();
        EXPECT_EQ(sync.m_teamSelectables.at({"PortChannel1", 10}), replacement);
        expectState(true);
    }

    TEST_F(TeamSyncdTest, MatchingDeleteInvalidatesPreservedStateAfterFailedRegistration)
    {
        swss::Select select;
        swss::TeamSync sync(m_app_db.get(), m_state_db.get(), &select);
        swss::Table state(m_state_db.get(), STATE_LAG_TABLE_NAME);
        state.set("PortChannel1", {{"state", "ok"}});
        sync.m_warmstart = true;
        sync.m_start_time = steady_clock::now();
        sync.m_pending_timeout = DEFAULT_WR_PENDING_TIMEOUT;
        teamsync_test::resetTeamSyncMocks();

        // Zero is an invalid ifindex, so the real team_init must fail.
        linkEvent(sync, RTM_NEWLINK, 0);
        EXPECT_TRUE(sync.m_teamSelectables.empty());
        expectState(true);
        linkEvent(sync, RTM_DELLINK, 0);
        sync.periodic();
        EXPECT_TRUE(sync.m_teamSelectables.empty());
        expectState(false);
    }

    TEST_F(TeamSyncdTest, FailedRegistrationRetriesOnNextNewLink)
    {
        swss::Select select;
        swss::TeamSync sync(m_app_db.get(), m_state_db.get(), &select);
        teamsync_test::resetTeamSyncMocks();

        linkEvent(sync, RTM_NEWLINK, 0);
        EXPECT_TRUE(sync.m_teamSelectables.empty());
        swss::Table app(m_app_db.get(), APP_LAG_TABLE_NAME);
        swss::Table state(m_state_db.get(), STATE_LAG_TABLE_NAME);
        std::string value;
        EXPECT_TRUE(app.hget("PortChannel1", "state", value));
        EXPECT_FALSE(state.hget("PortChannel1", "state", value));

        teamsync_test::setTeamSyncSuccessMocks();
        linkEvent(sync, RTM_NEWLINK, 0);
        sync.periodic();
        ASSERT_EQ(sync.m_teamSelectables.size(), 1u);
        EXPECT_EQ(sync.m_teamSelectables.begin()->first.second, 0);
        EXPECT_NE(sync.m_teamSelectables.begin()->second, nullptr);
        expectState(true);
    }

    TEST_F(TeamSyncdTest, UntrackedDeleteClearsPreservedReadiness)
    {
        swss::Select select;
        swss::TeamSync sync(m_app_db.get(), m_state_db.get(), &select);
        swss::Table state(m_state_db.get(), STATE_LAG_TABLE_NAME);
        state.set("PortChannel1", {{"state", "ok"}});
        linkEvent(sync, RTM_DELLINK, 10);
        sync.periodic();
        expectState(false);
    }

    TEST_F(TeamSyncdTest, SameLagIdUpdateKeepsHandlerAndRefreshesState)
    {
        swss::Select select;
        swss::TeamSync sync(m_app_db.get(), m_state_db.get(), &select);
        linkEvent(sync, RTM_NEWLINK, 10);
        auto original = sync.m_teamSelectables.at({"PortChannel1", 10});
        ASSERT_NE(original, nullptr);
        EXPECT_TRUE(original->admin_state);
        EXPECT_TRUE(original->oper_state);
        EXPECT_EQ(original->mtu, 1500u);
        linkEvent(sync, RTM_NEWLINK, 10);
        linkEvent(sync, RTM_NEWLINK, 10, 0, 9100);
        sync.periodic();
        EXPECT_EQ(sync.m_teamSelectables.at({"PortChannel1", 10}), original);
        EXPECT_FALSE(original->admin_state);
        EXPECT_FALSE(original->oper_state);
        EXPECT_EQ(original->mtu, 9100u);
        EXPECT_TRUE(sync.m_selectablesToRemove.empty());
        swss::Table state(m_state_db.get(), STATE_LAG_TABLE_NAME);
        std::string value;
        ASSERT_TRUE(state.hget("PortChannel1", "admin_status", value));
        EXPECT_EQ(value, "down");
        ASSERT_TRUE(state.hget("PortChannel1", "mtu", value));
        EXPECT_EQ(value, "9100");
    }

    TEST_F(TeamSyncdTest, CleanTeamSyncRemovesActiveLags)
    {
        swss::Select select;
        swss::TeamSync sync(m_app_db.get(), m_state_db.get(), &select);
        linkEvent(sync, RTM_NEWLINK, 10);
        sync.addLag({"PortChannel2", 20}, true, true, 1500);
        sync.periodic();
        sync.cleanTeamSync();
        sync.periodic();
        EXPECT_TRUE(sync.m_teamSelectables.empty());
        swss::Table app(m_app_db.get(), APP_LAG_TABLE_NAME);
        swss::Table state(m_state_db.get(), STATE_LAG_TABLE_NAME);
        std::vector<std::string> keys;
        app.getKeys(keys);
        EXPECT_TRUE(keys.empty());
        state.getKeys(keys);
        EXPECT_TRUE(keys.empty());
    }
}
