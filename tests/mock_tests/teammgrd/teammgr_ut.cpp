#include "gtest/gtest.h"
#include "../mock_table.h"
#include "teammgr.h"
#include <dlfcn.h>

extern int (*callback)(const std::string &cmd, std::string &stdout);
extern std::vector<std::string> mockCallArgs;
static std::vector< std::pair<pid_t, int> > mockKillCommands;
static std::map<std::string, std::FILE*> pidFiles;

static int (*callback_kill)(pid_t pid, int sig) = NULL;
static FILE* (*callback_fopen)(const char *pathname, const char *mode) = NULL;

static int cb_kill(pid_t pid, int sig)
{
    mockKillCommands.push_back(std::make_pair(pid, sig));
    return 0;
}

int kill(pid_t pid, int sig)
{
    if (callback_kill) {
        return callback_kill(pid, sig);
    }
    int (*realfunc)(pid_t, int) =
        (int(*)(pid_t, int))(dlsym (RTLD_NEXT, "kill"));
    return realfunc(pid, sig);
}

static FILE* cb_fopen(const char *pathname, const char *mode)
{
    auto pidFileSearch = pidFiles.find(pathname);
    if (pidFileSearch != pidFiles.end()) {
        return pidFileSearch->second;
    } else {
        return NULL;
    }
}

FILE* fopen(const char *pathname, const char *mode)
{
    if (callback_fopen) {
        FILE *fd = callback_fopen(pathname, mode);
        if (fd) {
            return fd;
        }
    }
    FILE* (*realfunc)(const char *, const char *) =
        (FILE*  (*)(const char *, const char *))(dlsym (RTLD_NEXT, "fopen"));
    return realfunc(pathname, mode);
}

FILE* fopen64(const char *pathname, const char *mode)
{
    if (callback_fopen) {
        FILE *fd = callback_fopen(pathname, mode);
        if (fd) {
            return fd;
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
    else if (cmd.find("/usr/bin/teamd -r -t PortChannel495") != std::string::npos)
    {
        mkdir("/var/run/teamd", 0755);
        std::FILE* pidFile = std::tmpfile();
        std::fputs("5678", pidFile);
        std::rewind(pidFile);
        pidFiles["/var/run/teamd/PortChannel495.pid"] = pidFile;
        return 0;
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
        int exec_cmd_called = 0;
        for (auto cmd : mockCallArgs){
            if (cmd.find("/usr/bin/teamd -r -t PortChannel382") != std::string::npos) {
                exec_cmd_called++;
            }
        }
        ASSERT_EQ(exec_cmd_called, 1);
        int kill_cmd_called = 0;
        for (auto killedProcess : mockKillCommands) {
            if (killedProcess.first == 1234) {
                kill_cmd_called++;
            }
        }
        ASSERT_EQ(kill_cmd_called, 1);
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
        int exec_cmd_called = 0;
        for (auto cmd : mockCallArgs){
            if (cmd.find("/usr/bin/teamd -r -t PortChannel495") != std::string::npos) {
                exec_cmd_called++;
            }
        }
        ASSERT_EQ(exec_cmd_called, 1);
        teammgr.cleanTeamProcesses();
        int kill_cmd_called = 0;
        for (auto killedProcess : mockKillCommands) {
            if (killedProcess.first == 5678) {
                kill_cmd_called++;
            }
        }
        ASSERT_EQ(kill_cmd_called, 1);
    }
}
