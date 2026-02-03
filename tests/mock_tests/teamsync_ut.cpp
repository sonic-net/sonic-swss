#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include <dlfcn.h>
#include <stdexcept>
#include <team.h>
#include "teamsync.h"

static unsigned int (*callback_sleep)(unsigned int seconds) = NULL;
static int (*callback_team_init)(struct team_handle *th, uint32_t ifindex) = NULL;
static int (*callback_team_change_handler)(struct team_handle *th, struct team_change_handler *handler, void *priv) = NULL;

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
        return cb_team_init(th, ifindex);
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
        return cb_team_change_handler(th, handler, priv);
    }
    int (*realfunc)(struct team_handle *, struct team_change_handler*, void*) =
        (int    (*)(struct team_handle *, struct team_change_handler*, void*))(dlsym (RTLD_NEXT, "team_change_handler"));
    return realfunc(th, handler, priv);
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
        }

        virtual void TearDown() override
        {
            callback_sleep = NULL;
            callback_team_init = NULL;
            callback_team_change_handler = NULL;
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

    TEST_F(TeamPortSyncTest, TeamdctlConnectFailure)
    {
        callback_team_init = cb_team_init;
        callback_team_change_handler = cb_team_change_handler;
        try {
            swss::TeamSync::TeamPortSync("testLag", 4, NULL);
            FAIL();
        } catch (std::runtime_error &exception) {
            EXPECT_THAT(exception.what(), testing::HasSubstr("Unable to connect to teamd"));
        }
    }
}
