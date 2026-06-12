#include "gtest/gtest.h"
#include "../mock_table.h"
#include "teammgr.h"
#include <dlfcn.h>
#include <net/if.h>
#include <netlink/addr.h>
#include <netlink/netlink.h>
#include <netlink/route/link.h>
#include <netlink/socket.h>

extern int (*callback)(const std::string &cmd, std::string &stdout);
extern std::vector<std::string> mockCallArgs;
static std::vector< std::pair<pid_t, int> > mockKillCommands;
static std::map<std::string, std::FILE*> pidFiles;

static int (*callback_kill)(pid_t pid, int sig) = NULL;
static std::pair<bool, FILE*> (*callback_fopen)(const char *pathname, const char *mode) = NULL;
static bool mock_nl_socket_alloc_fail = false;
static int mock_nl_connect_result = 0;
static bool mock_rtnl_link_alloc_fail = false;
static bool mock_nl_addr_build_fail = false;
static int mock_rtnl_link_get_kernel_result = 0;
static int mock_rtnl_link_change_result = 0;
static std::string mock_if_nametoindex_name;
static std::vector<std::string> mock_kernel_mac_updates;

extern "C" {

struct nl_sock *__wrap_nl_socket_alloc(void)
{
    if (mock_nl_socket_alloc_fail)
    {
        return nullptr;
    }
    static char fake_sock_mem[256];
    return reinterpret_cast<struct nl_sock *>(fake_sock_mem);
}

void __wrap_nl_socket_free(struct nl_sock *)
{
}

int __wrap_nl_connect(struct nl_sock *, int)
{
    return mock_nl_connect_result;
}

void __wrap_nl_close(struct nl_sock *)
{
}

unsigned int __wrap_if_nametoindex(const char *ifname)
{
    mock_if_nametoindex_name = ifname;
    return 17;
}

struct rtnl_link *__wrap_rtnl_link_alloc(void)
{
    if (mock_rtnl_link_alloc_fail)
    {
        return nullptr;
    }
    static char fake_link_mem[256];
    return reinterpret_cast<struct rtnl_link *>(fake_link_mem);
}

void __wrap_rtnl_link_put(struct rtnl_link *)
{
}

struct nl_addr *__wrap_nl_addr_build(int, const void *buf, size_t size)
{
    const auto *mac = reinterpret_cast<const uint8_t *>(buf);
    char text[18];
    snprintf(text, sizeof(text), "%02x:%02x:%02x:%02x:%02x:%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    mock_kernel_mac_updates.emplace_back(text);

    if (mock_nl_addr_build_fail)
    {
        return nullptr;
    }

    static char fake_addr_mem[256];
    return reinterpret_cast<struct nl_addr *>(fake_addr_mem);
}

void __wrap_nl_addr_put(struct nl_addr *)
{
}

void __wrap_rtnl_link_set_addr(struct rtnl_link *, struct nl_addr *)
{
}

int __wrap_rtnl_link_get_kernel(struct nl_sock *, int, const char *, struct rtnl_link **result)
{
    static char fake_orig_link_mem[256];
    *result = reinterpret_cast<struct rtnl_link *>(fake_orig_link_mem);
    return mock_rtnl_link_get_kernel_result;
}

int __wrap_rtnl_link_change(struct nl_sock *, struct rtnl_link *, struct rtnl_link *, int flags)
{
    EXPECT_EQ(flags, 0);
    return mock_rtnl_link_change_result;
}

}

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

// On 32-bit architectures, if 64-bit file offsets/support for large files is
// enabled, then fopen is a macro that maps to fopen64. Don't redefine fopen
// in that case.
#if not(defined _FILE_OFFSET_BITS && _FILE_OFFSET_BITS == 64)
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
#endif

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
            mock_nl_socket_alloc_fail = false;
            mock_nl_connect_result = 0;
            mock_rtnl_link_alloc_fail = false;
            mock_nl_addr_build_fail = false;
            mock_rtnl_link_get_kernel_result = 0;
            mock_rtnl_link_change_result = 0;
            mock_if_nametoindex_name.clear();
            mock_kernel_mac_updates.clear();
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

    TEST_F(TeamMgrTest, testSetLagSysmacUpdatesKernelAppAndState)
    {
        swss::TeamMgr teammgr(m_config_db.get(), m_app_db.get(), m_state_db.get(), cfg_lag_tables);

        std::string sys_mac = "02:03:04:05:06:07";
        EXPECT_TRUE(teammgr.setLagSysmac("PortChannel100", sys_mac));

        EXPECT_EQ(mock_if_nametoindex_name, "PortChannel100");
        ASSERT_EQ(mock_kernel_mac_updates.size(), 1u);
        EXPECT_EQ(mock_kernel_mac_updates[0], "02:03:04:05:06:07");

        swss::Table appLagTable(m_app_db.get(), APP_LAG_TABLE_NAME);
        swss::Table stateLagTable(m_state_db.get(), STATE_LAG_TABLE_NAME);
        std::vector<swss::FieldValueTuple> values;
        ASSERT_TRUE(appLagTable.get("PortChannel100", values));
        ASSERT_EQ(values.size(), 1u);
        EXPECT_EQ(fvField(values[0]), "system_mac");
        EXPECT_EQ(fvValue(values[0]), "02:03:04:05:06:07");

        values.clear();
        ASSERT_TRUE(stateLagTable.get("PortChannel100", values));
        ASSERT_EQ(values.size(), 1u);
        EXPECT_EQ(fvField(values[0]), "system_mac");
        EXPECT_EQ(fvValue(values[0]), "02:03:04:05:06:07");
    }

    TEST_F(TeamMgrTest, testSetLagSysmacNoneUsesDeviceMac)
    {
        swss::TeamMgr teammgr(m_config_db.get(), m_app_db.get(), m_state_db.get(), cfg_lag_tables);

        std::string sys_mac = "None";
        EXPECT_TRUE(teammgr.setLagSysmac("PortChannel101", sys_mac));
        EXPECT_EQ(sys_mac, "01:23:45:67:89:ab");

        ASSERT_EQ(mock_kernel_mac_updates.size(), 1u);
        EXPECT_EQ(mock_kernel_mac_updates[0], "01:23:45:67:89:ab");
    }

    TEST_F(TeamMgrTest, testSetLagSysmacKernelFailureDoesNotPublish)
    {
        mock_rtnl_link_change_result = -1;
        swss::TeamMgr teammgr(m_config_db.get(), m_app_db.get(), m_state_db.get(), cfg_lag_tables);

        std::string sys_mac = "02:03:04:05:06:07";
        EXPECT_FALSE(teammgr.setLagSysmac("PortChannel102", sys_mac));

        swss::Table appLagTable(m_app_db.get(), APP_LAG_TABLE_NAME);
        swss::Table stateLagTable(m_state_db.get(), STATE_LAG_TABLE_NAME);
        std::vector<swss::FieldValueTuple> values;
        EXPECT_FALSE(appLagTable.get("PortChannel102", values));
        EXPECT_FALSE(stateLagTable.get("PortChannel102", values));
    }

    TEST_F(TeamMgrTest, testSetLagSysmacKernelFailureModes)
    {
        swss::TeamMgr teammgr(m_config_db.get(), m_app_db.get(), m_state_db.get(), cfg_lag_tables);
        swss::Table appLagTable(m_app_db.get(), APP_LAG_TABLE_NAME);
        swss::Table stateLagTable(m_state_db.get(), STATE_LAG_TABLE_NAME);

        auto expectFailure = [&](const std::string &alias) {
            std::vector<swss::FieldValueTuple> values;
            std::string sys_mac = "02:03:04:05:06:07";
            EXPECT_FALSE(teammgr.setLagSysmac(alias, sys_mac));
            EXPECT_FALSE(appLagTable.get(alias, values));
            values.clear();
            EXPECT_FALSE(stateLagTable.get(alias, values));
        };

        mock_nl_socket_alloc_fail = true;
        expectFailure("PortChannel110");
        mock_nl_socket_alloc_fail = false;

        mock_nl_connect_result = -1;
        expectFailure("PortChannel111");
        mock_nl_connect_result = 0;

        mock_rtnl_link_alloc_fail = true;
        expectFailure("PortChannel112");
        mock_rtnl_link_alloc_fail = false;

        mock_nl_addr_build_fail = true;
        expectFailure("PortChannel113");
        mock_nl_addr_build_fail = false;

        mock_rtnl_link_get_kernel_result = -1;
        expectFailure("PortChannel114");
        mock_rtnl_link_get_kernel_result = 0;
    }

    TEST_F(TeamMgrTest, testDoLagTaskHandlesSystemMacFailure)
    {
        mock_rtnl_link_get_kernel_result = -1;
        swss::TeamMgr teammgr(m_config_db.get(), m_app_db.get(), m_state_db.get(), cfg_lag_tables);
        swss::Table cfg_lag_table(m_config_db.get(), CFG_LAG_TABLE_NAME);
        cfg_lag_table.set("PortChannel115", {
            {"admin_status", "up"},
            {"mtu", "9100"},
            {"system_mac", "02:03:04:05:06:07"}
        });

        teammgr.addExistingData(&cfg_lag_table);
        teammgr.doTask();

        swss::Table stateLagTable(m_state_db.get(), STATE_LAG_TABLE_NAME);
        std::vector<swss::FieldValueTuple> values;
        EXPECT_FALSE(stateLagTable.get("PortChannel115", values));
    }

    TEST_F(TeamMgrTest, testDoLagTaskProgramsSystemMac)
    {
        swss::TeamMgr teammgr(m_config_db.get(), m_app_db.get(), m_state_db.get(), cfg_lag_tables);
        swss::Table cfg_lag_table(m_config_db.get(), CFG_LAG_TABLE_NAME);
        cfg_lag_table.set("PortChannel103", {
            {"admin_status", "up"},
            {"mtu", "9100"},
            {"learn_mode", "drop"},
            {"tpid", "0x8100"},
            {"fast_rate", "true"},
            {"system_mac", "02:03:04:05:06:07"}
        });

        teammgr.addExistingData(&cfg_lag_table);
        teammgr.doTask();

        ASSERT_EQ(mock_kernel_mac_updates.size(), 1u);
        EXPECT_EQ(mock_kernel_mac_updates[0], "02:03:04:05:06:07");

        swss::Table appLagTable(m_app_db.get(), APP_LAG_TABLE_NAME);
        std::vector<swss::FieldValueTuple> values;
        ASSERT_TRUE(appLagTable.get("PortChannel103", values));
        EXPECT_TRUE(std::any_of(values.begin(), values.end(), [](const auto &fv) {
            return fvField(fv) == "system_mac" && fvValue(fv) == "02:03:04:05:06:07";
        }));
    }
}
