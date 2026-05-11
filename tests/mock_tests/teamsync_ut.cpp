#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include <dlfcn.h>
#include <stdexcept>
#include <team.h>
#include <teamdctl.h>
#include "teamsync.h"
#include "mock_table.h"

static unsigned int (*callback_sleep)(unsigned int seconds) = NULL;
static int (*callback_team_init)(struct team_handle *th, uint32_t ifindex) = NULL;
static int (*callback_team_change_handler)(struct team_handle *th, struct team_change_handler *handler, void *priv) = NULL;
static int (*callback_teamdctl_connect)(struct teamdctl *tdc, const char *team_name, const char *addr, const char *cli_type) = NULL;
static int (*callback_teamdctl_config_get_raw_direct)(struct teamdctl *tdc, char **response) = NULL;
static void (*callback_teamdctl_disconnect)(struct teamdctl *tdc) = NULL;

static unsigned int cb_sleep(unsigned int seconds)
{
    return 0;
}

unsigned int sleep(unsigned int seconds)
{
    if (callback_sleep)
    {
        return callback_sleep(seconds);
    }
    unsigned int (*realfunc)(unsigned int) =
        (unsigned int    (*)(unsigned int))(dlsym (RTLD_NEXT, "sleep"));
    return realfunc(seconds);
}


static int cb_team_init(struct team_handle *th, uint32_t ifindex)
{
    return 0;
}

int team_init(struct team_handle *th, uint32_t ifindex)
{
    if (callback_team_init)
    {
        return callback_team_init(th, ifindex);
    }
    int (*realfunc)(struct team_handle *, uint32_t) =
        (int    (*)(struct team_handle *, uint32_t))(dlsym (RTLD_NEXT, "team_init"));
    return realfunc(th, ifindex);
}

static int cb_team_change_handler(struct team_handle *th, struct team_change_handler *handler, void *priv)
{
    return 0;
}

int team_change_handler(struct team_handle *th, struct team_change_handler *handler, void *priv)
{
    if (callback_team_change_handler)
    {
        return callback_team_change_handler(th, handler, priv);
    }
    int (*realfunc)(struct team_handle *, struct team_change_handler*, void*) =
        (int    (*)(struct team_handle *, struct team_change_handler*, void*))(dlsym (RTLD_NEXT, "team_change_handler"));
    return realfunc(th, handler, priv);
}

static int cb_teamdctl_connect(struct teamdctl *tdc, const char *team_name, const char *addr, const char *cli_type)
{
    return 0;
}

int teamdctl_connect(struct teamdctl *tdc, const char *team_name, const char *addr, const char *cli_type)
{
    if (callback_teamdctl_connect)
    {
        return callback_teamdctl_connect(tdc, team_name, addr, cli_type);
    }
    int (*realfunc)(struct teamdctl *, const char *, const char *, const char *) =
        (int    (*)(struct teamdctl *, const char *, const char *, const char *))(dlsym (RTLD_NEXT, "teamdctl_connect"));
    return realfunc(tdc, team_name, addr, cli_type);
}

static int cb_teamdctl_config_get_raw_direct_force_error(struct teamdctl *tdc, char **response)
{
    // Forced error
    return 1;
}

static int cb_teamdctl_config_get_raw_direct_success(struct teamdctl *tdc, char **response)
{
    return 0;
}

int teamdctl_config_get_raw_direct(struct teamdctl *tdc, char **response)
{
    if (callback_teamdctl_config_get_raw_direct)
    {
        return callback_teamdctl_config_get_raw_direct(tdc, response);
    }
    int (*realfunc)(struct teamdctl *, char **) =
        (int    (*)(struct teamdctl *, char **))(dlsym (RTLD_NEXT, "teamdctl_config_get_raw_direct"));
    return realfunc(tdc, response);
}

static void cb_teamdctl_disconnect(struct teamdctl *tdc)
{
}

void teamdctl_disconnect(struct teamdctl *tdc)
{
    if (callback_teamdctl_disconnect)
    {
        callback_teamdctl_disconnect(tdc);
        return;
    }
    int (*realfunc)(struct teamdctl *) =
        (int    (*)(struct teamdctl *))(dlsym (RTLD_NEXT, "teamdctl_disconnect"));
    realfunc(tdc);
}

namespace teamportsync_test
{
    struct TeamPortSyncTest : public ::testing::Test
    {
        virtual void SetUp() override
        {
            callback_sleep = cb_sleep;
            callback_team_init = NULL;
            callback_team_change_handler = NULL;
            callback_teamdctl_connect = NULL;
            callback_teamdctl_config_get_raw_direct = cb_teamdctl_config_get_raw_direct_force_error;
            callback_teamdctl_disconnect = cb_teamdctl_disconnect;
        }

        virtual void TearDown() override
        {
            callback_sleep = NULL;
            callback_team_init = NULL;
            callback_team_change_handler = NULL;
            callback_teamdctl_connect = NULL;
            callback_teamdctl_config_get_raw_direct = NULL;
            callback_teamdctl_disconnect = NULL;
        }
    };

    TEST_F(TeamPortSyncTest, TestInvalidIfIndex)
    {
        try {
            swss::TeamSync::TeamPortSync("testLag", 0, NULL);
            FAIL();
        } catch (std::runtime_error &exception) {
            EXPECT_THAT(exception.what(), testing::HasSubstr("Unable to initialize team socket"));
        }
    }

    TEST_F(TeamPortSyncTest, NoLagPresent)
    {
        try {
            swss::TeamSync::TeamPortSync("testLag", 4, NULL);
            FAIL();
        } catch (std::runtime_error &exception) {
            EXPECT_THAT(exception.what(), testing::HasSubstr("Unable to initialize team socket"));
        }
    }

    TEST_F(TeamPortSyncTest, TeamdctlNoConfig)
    {
        callback_team_init = cb_team_init;
        callback_team_change_handler = cb_team_change_handler;
        callback_teamdctl_connect = cb_teamdctl_connect;
        try {
            swss::TeamSync::TeamPortSync("testLag", 4, NULL);
            FAIL();
        } catch (std::runtime_error &exception) {
            EXPECT_THAT(exception.what(), testing::HasSubstr("Unable to get config from teamd"));
        }
    }

    TEST_F(TeamPortSyncTest, AllSuccess)
    {
        callback_team_init = cb_team_init;
        callback_team_change_handler = cb_team_change_handler;
        callback_teamdctl_connect = cb_teamdctl_connect;
        callback_teamdctl_config_get_raw_direct = cb_teamdctl_config_get_raw_direct_success;
        swss::TeamSync::TeamPortSync("testLag", 4, NULL);
    }
}

namespace teamsync_test
{
    /* Subclass to expose protected members for unit testing. */
    class TeamSyncUnderTest : public swss::TeamSync
    {
    public:
        TeamSyncUnderTest(swss::DBConnector *db, swss::DBConnector *stateDb, swss::Select *sel)
            : swss::TeamSync(db, stateDb, sel) {}
        using swss::TeamSync::addLag;
        using swss::TeamSync::removeLag;
    };

    struct TeamSyncTest : public ::testing::Test
    {
        virtual void SetUp() override
        {
            callback_sleep = cb_sleep;
            callback_team_init = NULL;
            callback_team_change_handler = NULL;
            callback_teamdctl_connect = NULL;
            callback_teamdctl_config_get_raw_direct = cb_teamdctl_config_get_raw_direct_force_error;
            callback_teamdctl_disconnect = cb_teamdctl_disconnect;
            testing_db::reset();
        }

        virtual void TearDown() override
        {
            callback_sleep = NULL;
            callback_team_init = NULL;
            callback_team_change_handler = NULL;
            callback_teamdctl_connect = NULL;
            callback_teamdctl_config_get_raw_direct = NULL;
            callback_teamdctl_disconnect = NULL;
            testing_db::reset();
        }
    };

    /* Verify that when TeamPortSync construction fails (team_init returns an
     * error for an invalid ifindex), addLag() catches the system_error
     * internally and does not propagate it to the caller. */
    TEST_F(TeamSyncTest, AddLagTeamInitFails)
    {
        swss::DBConnector db(0, "localhost", 0, 0);
        swss::DBConnector stateDb(1, "localhost", 0, 0);
        TeamSyncUnderTest ts(&db, &stateDb, nullptr);

        /* ifindex 0 is invalid; the real team_init() will fail, causing
         * TeamPortSync to throw system_error, which addLag() must catch. */
        ts.addLag("testLag", 0, true, true, 1500);
    }

    /* Verify that addLag() successfully creates the team instance and writes
     * the LAG state when all underlying calls succeed. */
    TEST_F(TeamSyncTest, AddLagSucceeds)
    {
        callback_team_init = cb_team_init;
        callback_team_change_handler = cb_team_change_handler;
        callback_teamdctl_connect = cb_teamdctl_connect;
        callback_teamdctl_config_get_raw_direct = cb_teamdctl_config_get_raw_direct_success;

        swss::DBConnector db(0, "localhost", 0, 0);
        swss::DBConnector stateDb(1, "localhost", 0, 0);
        TeamSyncUnderTest ts(&db, &stateDb, nullptr);

        ts.addLag("testLag", 4, true, true, 1500);
    }

    /* --- Queue-based event processing tests --- */

    /* TestEventQueueing: Verify that enqueueEvent() adds events to the queue */
    TEST_F(TeamSyncTest, TestEventQueueing)
    {
        swss::DBConnector db(0, "localhost", 0, 0);
        swss::DBConnector stateDb(1, "localhost", 0, 0);
        TeamSyncUnderTest ts(&db, &stateDb, nullptr);

        swss::NetlinkEvent event;
        event.nlmsg_type = RTM_NEWLINK;
        event.lagName = "PortChannel1";
        event.ifindex = 10;
        event.admin_state = true;
        event.oper_state = true;
        event.mtu = 1500;

        ts.enqueueEvent(event);
        EXPECT_EQ(ts.getEventQueue().size(), 1u);

        swss::NetlinkEvent event2;
        event2.nlmsg_type = RTM_DELLINK;
        event2.lagName = "PortChannel2";
        event2.ifindex = 11;
        event2.admin_state = false;
        event2.oper_state = false;
        event2.mtu = 1500;

        ts.enqueueEvent(event2);
        EXPECT_EQ(ts.getEventQueue().size(), 2u);

        /* Verify FIFO ordering */
        EXPECT_EQ(ts.getEventQueue().front().lagName, "PortChannel1");
        EXPECT_EQ(ts.getEventQueue().back().lagName, "PortChannel2");
    }

    /* TestStaleNewCancelledByDelete: RTM_NEWLINK followed by RTM_DELLINK for
     * the same lag should cancel each other during processEventQueue() */
    TEST_F(TeamSyncTest, TestStaleNewCancelledByDelete)
    {
        swss::DBConnector db(0, "localhost", 0, 0);
        swss::DBConnector stateDb(1, "localhost", 0, 0);
        TeamSyncUnderTest ts(&db, &stateDb, nullptr);

        swss::NetlinkEvent newEvent;
        newEvent.nlmsg_type = RTM_NEWLINK;
        newEvent.lagName = "PortChannel1";
        newEvent.ifindex = 10;
        newEvent.admin_state = true;
        newEvent.oper_state = true;
        newEvent.mtu = 1500;

        swss::NetlinkEvent delEvent;
        delEvent.nlmsg_type = RTM_DELLINK;
        delEvent.lagName = "PortChannel1";
        delEvent.ifindex = 10;
        delEvent.admin_state = false;
        delEvent.oper_state = false;
        delEvent.mtu = 1500;

        ts.enqueueEvent(newEvent);
        ts.enqueueEvent(delEvent);
        EXPECT_EQ(ts.getEventQueue().size(), 2u);

        ts.processEventQueue();

        /* Both events should be cancelled — queue empty */
        EXPECT_EQ(ts.getEventQueue().size(), 0u);
    }

    /* TestFailedAddLagStaysQueued: When addLag() fails and the ifindex is
     * still valid (sysfs exists), the event should be re-queued for retry.
     * Note: In the test environment /sys/class/net/<lag>/ifindex won't exist,
     * so the event will be dropped (ifindex invalid path). We test that the
     * event is NOT in the queue after processing (dropped due to invalid
     * ifindex). */
    TEST_F(TeamSyncTest, TestFailedAddLagDroppedWhenIfindexInvalid)
    {
        swss::DBConnector db(0, "localhost", 0, 0);
        swss::DBConnector stateDb(1, "localhost", 0, 0);
        TeamSyncUnderTest ts(&db, &stateDb, nullptr);

        /* team_init will fail (callback not set), addLag() catches exception.
         * /sys/class/net/PortChannel1/ifindex won't exist in test env,
         * so the event should be dropped. */
        swss::NetlinkEvent event;
        event.nlmsg_type = RTM_NEWLINK;
        event.lagName = "PortChannel1";
        event.ifindex = 99;
        event.admin_state = true;
        event.oper_state = true;
        event.mtu = 1500;

        ts.enqueueEvent(event);
        ts.processEventQueue();

        /* Event dropped because ifindex no longer valid */
        EXPECT_EQ(ts.getEventQueue().size(), 0u);
    }

    /* TestSuccessfulProcessing: Enqueue a valid RTM_NEWLINK, process queue,
     * verify lag is created (queue empty after processing) */
    TEST_F(TeamSyncTest, TestSuccessfulProcessing)
    {
        callback_team_init = cb_team_init;
        callback_team_change_handler = cb_team_change_handler;
        callback_teamdctl_connect = cb_teamdctl_connect;
        callback_teamdctl_config_get_raw_direct = cb_teamdctl_config_get_raw_direct_success;

        swss::DBConnector db(0, "localhost", 0, 0);
        swss::DBConnector stateDb(1, "localhost", 0, 0);
        TeamSyncUnderTest ts(&db, &stateDb, nullptr);

        swss::NetlinkEvent event;
        event.nlmsg_type = RTM_NEWLINK;
        event.lagName = "testLag";
        event.ifindex = 4;
        event.admin_state = true;
        event.oper_state = true;
        event.mtu = 1500;

        ts.enqueueEvent(event);
        EXPECT_EQ(ts.getEventQueue().size(), 1u);

        ts.processEventQueue();

        /* Event processed successfully — queue should be empty */
        EXPECT_EQ(ts.getEventQueue().size(), 0u);
    }

    /* TestDeleteProcessedImmediately: Enqueue RTM_DELLINK for a LAG that
     * exists, verify removeLag() is called (event consumed from queue) */
    TEST_F(TeamSyncTest, TestDeleteProcessedImmediately)
    {
        callback_team_init = cb_team_init;
        callback_team_change_handler = cb_team_change_handler;
        callback_teamdctl_connect = cb_teamdctl_connect;
        callback_teamdctl_config_get_raw_direct = cb_teamdctl_config_get_raw_direct_success;

        swss::DBConnector db(0, "localhost", 0, 0);
        swss::DBConnector stateDb(1, "localhost", 0, 0);
        TeamSyncUnderTest ts(&db, &stateDb, nullptr);

        /* First add the LAG so it exists */
        ts.addLag("testLag", 4, true, true, 1500);

        /* Now enqueue a delete event */
        swss::NetlinkEvent delEvent;
        delEvent.nlmsg_type = RTM_DELLINK;
        delEvent.lagName = "testLag";
        delEvent.ifindex = 4;
        delEvent.admin_state = false;
        delEvent.oper_state = false;
        delEvent.mtu = 1500;

        ts.enqueueEvent(delEvent);
        EXPECT_EQ(ts.getEventQueue().size(), 1u);

        ts.processEventQueue();

        /* Delete event should be consumed */
        EXPECT_EQ(ts.getEventQueue().size(), 0u);
    }

    /* TestFairness: Enqueue events for multiple LAGs where first one fails —
     * verify second LAG still gets processed */
    TEST_F(TeamSyncTest, TestFairness)
    {
        /* Set up so that team_init succeeds for all */
        callback_team_init = cb_team_init;
        callback_team_change_handler = cb_team_change_handler;
        callback_teamdctl_connect = cb_teamdctl_connect;
        /* teamdctl_config_get_raw_direct will fail, causing addLag to fail */
        callback_teamdctl_config_get_raw_direct = cb_teamdctl_config_get_raw_direct_force_error;

        swss::DBConnector db(0, "localhost", 0, 0);
        swss::DBConnector stateDb(1, "localhost", 0, 0);
        TeamSyncUnderTest ts(&db, &stateDb, nullptr);

        /* First LAG will fail (teamdctl_config fails) */
        swss::NetlinkEvent event1;
        event1.nlmsg_type = RTM_NEWLINK;
        event1.lagName = "PortChannel1";
        event1.ifindex = 10;
        event1.admin_state = true;
        event1.oper_state = true;
        event1.mtu = 1500;

        /* Second LAG will also fail but both should get a chance */
        swss::NetlinkEvent event2;
        event2.nlmsg_type = RTM_NEWLINK;
        event2.lagName = "PortChannel2";
        event2.ifindex = 11;
        event2.admin_state = true;
        event2.oper_state = true;
        event2.mtu = 1500;

        ts.enqueueEvent(event1);
        ts.enqueueEvent(event2);
        EXPECT_EQ(ts.getEventQueue().size(), 2u);

        ts.processEventQueue();

        /* Both events should have been attempted. Since both fail and
         * /sys/class/net/<lag>/ifindex doesn't exist in test env, both
         * are dropped (ifindex invalid). This proves fair processing —
         * the second event was reached even though the first failed. */
        EXPECT_EQ(ts.getEventQueue().size(), 0u);
    }
}
