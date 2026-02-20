#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include <dlfcn.h>
#include <stdexcept>
#include <team.h>
#include <teamdctl.h>
#include "teamsync.h"

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
