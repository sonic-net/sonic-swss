#include "gtest/gtest.h"
#include "../mock_table.h"
#include "teammgr.h"
#include <dlfcn.h>

extern int (*callback)(const std::string &cmd, std::string &stdout);
extern std::vector<std::string> mockCallArgs;
static std::vector< std::pair<pid_t, int> > mockKillCommands;
static std::map<std::string, std::FILE*> pidFiles;

static int (*callback_kill)(pid_t pid, int sig) = NULL;
static std::pair<bool, FILE*> (*callback_fopen)(const char *pathname, const char *mode) = NULL;

static int cb_kill(pid_t pid, int sig)
{
    mockKillCommands.push_back(std::make_pair(pid, sig));
    if (!sig)
    {
        errno = ESRCH;
        return -1;
    }
    else
    {
        return 0;
    }
}

int kill(pid_t pid, int sig)
{
    if (callback_kill)
    {
        return callback_kill(pid, sig);
    }
    int (*realfunc)(pid_t, int) =
        (int(*)(pid_t, int))(dlsym (RTLD_NEXT, "kill"));
    return realfunc(pid, sig);
}

static std::pair<bool, FILE*> cb_fopen(const char *pathname, const char *mode)
{
    auto pidFileSearch = pidFiles.find(pathname);
    if (pidFileSearch != pidFiles.end())
    {
        if (!pidFileSearch->second)
        {
            errno = ENOENT;
        }
        return std::make_pair(true, pidFileSearch->second);
    }
    else
    {
        return std::make_pair(false, (FILE*)NULL);
    }
}

FILE* fopen(const char *pathname, const char *mode)
{
    if (callback_fopen)
    {
        std::pair<bool, FILE*> callback_fd = callback_fopen(pathname, mode);
        if (callback_fd.first)
        {
            return callback_fd.second;
        }
    }
    FILE* (*realfunc)(const char *, const char *) =
        (FILE*  (*)(const char *, const char *))(dlsym (RTLD_NEXT, "fopen"));
    return realfunc(pathname, mode);
}

FILE* fopen64(const char *pathname, const char *mode)
{
    if (callback_fopen)
    {
        std::pair<bool, FILE*> callback_fd = callback_fopen(pathname, mode);
        if (callback_fd.first)
        {
            return callback_fd.second;
        }
    }
    FILE* (*realfunc)(const char *, const char *) =
        (FILE*  (*)(const char *, const char *))(dlsym (RTLD_NEXT, "fopen64"));
    return realfunc(pathname, mode);
}

int cb(const std::string &cmd, std::string &stdout)
{
    mockCallArgs.push_back(cmd);
    if (cmd.find("/usr/bin/teamd -r -t PortChannel382") != std::string::npos)
    {
        mkdir("/var/run/teamd", 0755);
        std::FILE* pidFile = std::tmpfile();
        std::fputs("1234", pidFile);
        std::rewind(pidFile);
        pidFiles["/var/run/teamd/PortChannel382.pid"] = pidFile;
        return 1;
    }
    else if (cmd.find("/usr/bin/teamd -r -t PortChannel812") != std::string::npos)
    {
        pidFiles["/var/run/teamd/PortChannel812.pid"] = NULL;
        return 1;
    }
    else if (cmd.find("/usr/bin/teamd -r -t PortChannel495") != std::string::npos)
    {
        mkdir("/var/run/teamd", 0755);
        std::FILE* pidFile = std::tmpfile();
        std::fputs("5678", pidFile);
        std::rewind(pidFile);
        pidFiles["/var/run/teamd/PortChannel495.pid"] = pidFile;
        return 0;
    }
    else if (cmd.find("/usr/bin/teamd -r -t PortChannel198") != std::string::npos)
    {
        pidFiles["/var/run/teamd/PortChannel198.pid"] = NULL;
    }
    else
    {
        for (int i = 600; i < 620; i++)
        {
            if (cmd.find(std::string("/usr/bin/teamd -r -t PortChannel") + std::to_string(i)) != std::string::npos)
            {
                pidFiles[std::string("/var/run/teamd/PortChannel") + std::to_string(i) + std::string(".pid")] = NULL;
            }
        }
    }
    return 0;
}

namespace teammgr_ut
{
    struct TeamMgrTest : public ::testing::Test
    {
        std::shared_ptr<swss::DBConnector> m_config_db;
        std::shared_ptr<swss::DBConnector> m_app_db;
        std::shared_ptr<swss::DBConnector> m_state_db;
        std::vector<TableConnector> cfg_lag_tables;

        virtual void SetUp() override
        {
            testing_db::reset();
            m_config_db = std::make_shared<swss::DBConnector>("CONFIG_DB", 0);
            m_app_db = std::make_shared<swss::DBConnector>("APPL_DB", 0);
            m_state_db = std::make_shared<swss::DBConnector>("STATE_DB", 0);

            swss::Table metadata_table = swss::Table(m_config_db.get(), CFG_DEVICE_METADATA_TABLE_NAME);
            std::vector<swss::FieldValueTuple> vec;
            vec.emplace_back("mac", "01:23:45:67:89:ab");
            metadata_table.set("localhost", vec);

            TableConnector conf_lag_table(m_config_db.get(), CFG_LAG_TABLE_NAME);
            TableConnector conf_lag_member_table(m_config_db.get(), CFG_LAG_MEMBER_TABLE_NAME);
            TableConnector state_port_table(m_state_db.get(), STATE_PORT_TABLE_NAME);

            std::vector<TableConnector> tables = {
                conf_lag_table,
                conf_lag_member_table,
                state_port_table
            };

            cfg_lag_tables = tables;
            mockCallArgs.clear();
            mockKillCommands.clear();
            pidFiles.clear();
            callback = cb;
            callback_kill = cb_kill;
            callback_fopen = cb_fopen;
        }

        virtual void TearDown() override
        {
            callback = NULL;
            callback_kill = NULL;
            callback_fopen = NULL;
        }
    };

    TEST_F(TeamMgrTest, testProcessKilledAfterAddLagFailure)
    {
        swss::TeamMgr teammgr(m_config_db.get(), m_app_db.get(), m_state_db.get(), cfg_lag_tables);
        swss::Table cfg_lag_table = swss::Table(m_config_db.get(), CFG_LAG_TABLE_NAME);
        cfg_lag_table.set("PortChannel382", { { "admin_status", "up" },
                                            { "mtu", "9100" },
                                            { "lacp_key", "auto" },
                                            { "min_links", "2" } });
        teammgr.addExistingData(&cfg_lag_table);
        teammgr.doTask();
        ASSERT_NE(mockCallArgs.size(), 0);
        EXPECT_NE(mockCallArgs.front().find("/usr/bin/teamd -r -t PortChannel382"), std::string::npos);
        EXPECT_EQ(mockCallArgs.size(), 1);
        EXPECT_EQ(mockKillCommands.size(), 1);
        EXPECT_EQ(mockKillCommands.front().first, 1234);
        EXPECT_EQ(mockKillCommands.front().second, SIGTERM);
    }

    TEST_F(TeamMgrTest, testProcessPidFileMissingAfterAddLagFailure)
    {
        swss::TeamMgr teammgr(m_config_db.get(), m_app_db.get(), m_state_db.get(), cfg_lag_tables);
        swss::Table cfg_lag_table = swss::Table(m_config_db.get(), CFG_LAG_TABLE_NAME);
        cfg_lag_table.set("PortChannel812", { { "admin_status", "up" },
                                            { "mtu", "9100" },
                                            { "fallback", "true" },
                                            { "lacp_key", "auto" },
                                            { "min_links", "1" } });
        teammgr.addExistingData(&cfg_lag_table);
        teammgr.doTask();
        ASSERT_NE(mockCallArgs.size(), 0);
        EXPECT_NE(mockCallArgs.front().find("/usr/bin/teamd -r -t PortChannel812"), std::string::npos);
        EXPECT_EQ(mockCallArgs.size(), 1);
        EXPECT_EQ(mockKillCommands.size(), 0);
    }

    TEST_F(TeamMgrTest, testProcessCleanupAfterAddLag)
    {
        swss::TeamMgr teammgr(m_config_db.get(), m_app_db.get(), m_state_db.get(), cfg_lag_tables);
        swss::Table cfg_lag_table = swss::Table(m_config_db.get(), CFG_LAG_TABLE_NAME);
        cfg_lag_table.set("PortChannel495", { { "admin_status", "up" },
                                            { "mtu", "9100" },
                                            { "lacp_key", "auto" },
                                            { "min_links", "2" } });
        teammgr.addExistingData(&cfg_lag_table);
        teammgr.doTask();
        ASSERT_EQ(mockCallArgs.size(), 3);
        ASSERT_NE(mockCallArgs.front().find("/usr/bin/teamd -r -t PortChannel495"), std::string::npos);
        teammgr.cleanTeamProcesses();
        EXPECT_EQ(mockKillCommands.size(), 2);
        EXPECT_EQ(mockKillCommands.front().first, 5678);
        EXPECT_EQ(mockKillCommands.front().second, SIGTERM);
    }

    TEST_F(TeamMgrTest, testProcessPidFileMissingDuringCleanup)
    {
        swss::TeamMgr teammgr(m_config_db.get(), m_app_db.get(), m_state_db.get(), cfg_lag_tables);
        swss::Table cfg_lag_table = swss::Table(m_config_db.get(), CFG_LAG_TABLE_NAME);
        cfg_lag_table.set("PortChannel198", { { "admin_status", "up" },
                                            { "mtu", "9100" },
                                            { "fallback", "true" },
                                            { "lacp_key", "auto" },
                                            { "min_links", "1" } });
        teammgr.addExistingData(&cfg_lag_table);
        teammgr.doTask();
        ASSERT_NE(mockCallArgs.size(), 0);
        EXPECT_NE(mockCallArgs.front().find("/usr/bin/teamd -r -t PortChannel198"), std::string::npos);
        EXPECT_EQ(mockCallArgs.size(), 3);
        teammgr.cleanTeamProcesses();
        EXPECT_EQ(mockKillCommands.size(), 0);
    }

    TEST_F(TeamMgrTest, testSleepDuringCleanup)
    {
        swss::TeamMgr teammgr(m_config_db.get(), m_app_db.get(), m_state_db.get(), cfg_lag_tables);
        swss::Table cfg_lag_table = swss::Table(m_config_db.get(), CFG_LAG_TABLE_NAME);
        for (int i = 600; i < 620; i++)
        {
            cfg_lag_table.set(std::string("PortChannel") + std::to_string(i), { { "admin_status", "up" },
                    { "mtu", "9100" },
                    { "lacp_key", "auto" } });
        }
        teammgr.addExistingData(&cfg_lag_table);
        teammgr.doTask();
        ASSERT_EQ(mockCallArgs.size(), 60);
        std::chrono::steady_clock::time_point begin = std::chrono::steady_clock::now();
        teammgr.cleanTeamProcesses();
        std::chrono::steady_clock::time_point end = std::chrono::steady_clock::now();
        EXPECT_EQ(mockKillCommands.size(), 0);
        EXPECT_GE(std::chrono::duration_cast<std::chrono::milliseconds>(end - begin).count(), 200);
    }
}
