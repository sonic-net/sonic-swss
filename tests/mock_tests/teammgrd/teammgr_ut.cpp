#include "gtest/gtest.h"
#include "../mock_table.h"
#include "teammgr.h"
#include "subscriberstatetable.h"
#include <dlfcn.h>
#include <memory>
#include <set>
#include <cerrno>
#include <sys/stat.h>
#include <net/if.h>
#include <netlink/addr.h>
#include <netlink/netlink.h>
#include <netlink/route/link.h>
#include <netlink/socket.h>

extern int (*callback)(const std::string &cmd, std::string &stdout);
extern std::vector<std::string> mockCallArgs;
static std::vector< std::pair<pid_t, int> > mockKillCommands;
static std::map<std::string, std::FILE*> pidFiles;
// Force the teamdctl macsec_gate push to fail, to exercise the retry path in
// applyMacsecMemberGate.
static bool failMacsecGatePush = false;
// Ports isPortEnslaved() should report as enslaved (see the lstat override).
static std::set<std::string> mockEnslavedPorts;

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

extern "C" {
// Interposes libc's lstat for the whole test binary (same technique as the
// kill/fopen overrides below). isPortEnslaved() probes
// /sys/class/net/<member>/master; there is no sysfs in the mock, so answer
// from mockEnslavedPorts: present => enslaved (0), absent => not enslaved
// (-1). Any other path falls through to the real lstat.
int lstat(const char *pathname, struct stat *statbuf)
{
    const std::string p(pathname);
    static const std::string prefix = "/sys/class/net/";
    static const std::string suffix = "/master";
    if (p.size() > prefix.size() + suffix.size() &&
        p.compare(0, prefix.size(), prefix) == 0 &&
        p.compare(p.size() - suffix.size(), suffix.size(), suffix) == 0)
    {
        std::string member = p.substr(prefix.size(),
                                      p.size() - prefix.size() - suffix.size());
        if (mockEnslavedPorts.count(member))
        {
            return 0;
        }
        errno = ENOENT;
        return -1;
    }
    int (*realfunc)(const char *, struct stat *) =
        (int (*)(const char *, struct stat *))(dlsym(RTLD_NEXT, "lstat"));
    return realfunc(pathname, statbuf);
}
} // extern "C"

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
    if (failMacsecGatePush && cmd.find("runner.macsec_gate") != std::string::npos)
    {
        return 1;
    }
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
            failMacsecGatePush = false;
            mockEnslavedPorts.clear();
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

    // ------------------------------------------------------------------------
    // MACsec member gate (teamd runner.macsec_gate)
    // ------------------------------------------------------------------------

    // True if some executed command contains every pattern.
    static bool findCommandWith(const std::vector<std::string> &patterns)
    {
        for (const auto &cmd : mockCallArgs)
        {
            bool all = true;
            for (const auto &p : patterns)
            {
                if (cmd.find(p) == std::string::npos) { all = false; break; }
            }
            if (all) return true;
        }
        return false;
    }

    static int countCommandsWith(const std::vector<std::string> &patterns)
    {
        int n = 0;
        for (const auto &cmd : mockCallArgs)
        {
            bool all = true;
            for (const auto &p : patterns)
            {
                if (cmd.find(p) == std::string::npos) { all = false; break; }
            }
            if (all) n++;
        }
        return n;
    }

    static int countTeamdStarts(const std::string &alias)
    {
        return countCommandsWith({ "/usr/bin/teamd -r -t " + alias });
    }

    // Feed one notification straight into the manager. The DB row is updated
    // first, as in the real flow: by the time the consumer sees the
    // notification the table already reflects the change and the manager
    // re-reads it rather than trusting the row itself.
    static void pushRow(swss::TeamMgr &teammgr, swss::DBConnector *db,
                        const std::string &table, const std::string &key,
                        const std::string &op,
                        const std::vector<swss::FieldValueTuple> &fvs)
    {
        swss::Table t(db, table);
        if (op == SET_COMMAND)
        {
            t.set(key, fvs);
        }
        else
        {
            t.del(key);
        }
        auto consumer = std::unique_ptr<Consumer>(new Consumer(
            new swss::SubscriberStateTable(db, table, 1, 1), &teammgr, table));
        consumer->addToSync({ { key, op, fvs } });
        static_cast<Orch *>(&teammgr)->doTask(*consumer.get());
    }

    static const char *kSci = "5254001122330001";

    static std::string saKey(const std::string &member, int an)
    {
        return member + "|" + kSci + "|" + std::to_string(an);
    }

    // A PortChannel with teamd started and one configured member that is
    // already a teamd port (enslaved). `macsec` attaches a profile to the
    // member in CONFIG_DB; `sa_present` seeds one ingress SA and the MACsec
    // port row in STATE_DB so the member starts with its session up.
    static void setUpMacsecLag(swss::DBConnector *config_db, swss::DBConnector *state_db,
                               swss::TeamMgr &teammgr, const std::string &pc,
                               const std::string &member, bool macsec, bool sa_present,
                               bool enslaved = true)
    {
        swss::Table cfg_port(config_db, CFG_PORT_TABLE_NAME);
        std::vector<swss::FieldValueTuple> pfvs = { { "admin_status", "up" }, { "mtu", "9100" } };
        if (macsec)
        {
            pfvs.emplace_back("macsec", "macsec-profile-1");
        }
        cfg_port.set(member, pfvs);

        swss::Table cfg_lag_member(config_db, CFG_LAG_MEMBER_TABLE_NAME);
        cfg_lag_member.set(pc + "|" + member, { { "NULL", "NULL" } });

        if (sa_present)
        {
            swss::Table sa(state_db, STATE_MACSEC_INGRESS_SA_TABLE_NAME);
            sa.set(saKey(member, 0), { { "state", "ok" } });
            swss::Table mp(state_db, STATE_MACSEC_PORT_TABLE_NAME);
            mp.set(member, { { "state", "ok" } });
        }

        if (enslaved)
        {
            mockEnslavedPorts.insert(member);
        }

        swss::Table cfg_lag(config_db, CFG_LAG_TABLE_NAME);
        cfg_lag.set(pc, { { "admin_status", "up" },
                          { "mtu", "9100" },
                          { "lacp_key", "auto" },
                          { "min_links", "1" } });
        teammgr.addExistingData(&cfg_lag);
        teammgr.doTask();
    }

    TEST_F(TeamMgrTest, testMacsecGate_LastIngressSaDeleteClosesGate)
    {
        swss::TeamMgr teammgr(m_config_db.get(), m_app_db.get(), m_state_db.get(), cfg_lag_tables);
        setUpMacsecLag(m_config_db.get(), m_state_db.get(), teammgr, "PortChannel116", "Ethernet1", true, true);
        mockCallArgs.clear();
        mockKillCommands.clear();

        // MKA timed out: macsecorch deleted the member's last ingress SA.
        pushRow(teammgr, m_state_db.get(), STATE_MACSEC_INGRESS_SA_TABLE_NAME,
                saKey("Ethernet1", 0), DEL_COMMAND, {});

        EXPECT_TRUE(findCommandWith({"teamdctl", "PortChannel116", "state item set",
                                     "ports.Ethernet1.runner.macsec_gate", "false"}));
        // The pull must never restart teamd, recreate the netdev or flap a link.
        EXPECT_EQ(mockKillCommands.size(), 0);
        EXPECT_EQ(countTeamdStarts("PortChannel116"), 0);
        EXPECT_FALSE(findCommandWith({"ip link set", "Ethernet1", "down"}));
    }

    TEST_F(TeamMgrTest, testMacsecGate_SaReturnOpensGate)
    {
        swss::TeamMgr teammgr(m_config_db.get(), m_app_db.get(), m_state_db.get(), cfg_lag_tables);
        setUpMacsecLag(m_config_db.get(), m_state_db.get(), teammgr, "PortChannel117", "Ethernet1", true, true);

        pushRow(teammgr, m_state_db.get(), STATE_MACSEC_INGRESS_SA_TABLE_NAME,
                saKey("Ethernet1", 0), DEL_COMMAND, {});
        ASSERT_TRUE(findCommandWith({"PortChannel117", "ports.Ethernet1.runner.macsec_gate", "false"}));
        mockCallArgs.clear();

        // MKA re-established: the first ingress SA is back.
        pushRow(teammgr, m_state_db.get(), STATE_MACSEC_INGRESS_SA_TABLE_NAME,
                saKey("Ethernet1", 1), SET_COMMAND, { { "state", "ok" } });

        EXPECT_TRUE(findCommandWith({"teamdctl", "PortChannel117", "state item set",
                                     "ports.Ethernet1.runner.macsec_gate", "true"}));
    }

    TEST_F(TeamMgrTest, testMacsecGate_RekeyOverlapKeepsGateOpen)
    {
        swss::TeamMgr teammgr(m_config_db.get(), m_app_db.get(), m_state_db.get(), cfg_lag_tables);
        setUpMacsecLag(m_config_db.get(), m_state_db.get(), teammgr, "PortChannel118", "Ethernet1", true, true);

        // Rekey: the new AN is installed before the old one is removed.
        pushRow(teammgr, m_state_db.get(), STATE_MACSEC_INGRESS_SA_TABLE_NAME,
                saKey("Ethernet1", 1), SET_COMMAND, { { "state", "ok" } });
        mockCallArgs.clear();

        pushRow(teammgr, m_state_db.get(), STATE_MACSEC_INGRESS_SA_TABLE_NAME,
                saKey("Ethernet1", 0), DEL_COMMAND, {});
        // One SA still present: the port can still decrypt, no pull.
        EXPECT_FALSE(findCommandWith({"PortChannel118", "ports.Ethernet1.runner.macsec_gate"}));

        // Only when the last one goes does the gate close.
        pushRow(teammgr, m_state_db.get(), STATE_MACSEC_INGRESS_SA_TABLE_NAME,
                saKey("Ethernet1", 1), DEL_COMMAND, {});
        EXPECT_TRUE(findCommandWith({"PortChannel118", "ports.Ethernet1.runner.macsec_gate", "false"}));
    }

    TEST_F(TeamMgrTest, testMacsecGate_NoDuplicatePush)
    {
        swss::TeamMgr teammgr(m_config_db.get(), m_app_db.get(), m_state_db.get(), cfg_lag_tables);
        setUpMacsecLag(m_config_db.get(), m_state_db.get(), teammgr, "PortChannel119", "Ethernet1", true, true);
        mockCallArgs.clear();

        pushRow(teammgr, m_state_db.get(), STATE_MACSEC_INGRESS_SA_TABLE_NAME,
                saKey("Ethernet1", 0), DEL_COMMAND, {});
        // A second DEL for an AN that is already gone (SC teardown cascade)
        // re-evaluates to the same answer and must not fork teamdctl again.
        pushRow(teammgr, m_state_db.get(), STATE_MACSEC_INGRESS_SA_TABLE_NAME,
                saKey("Ethernet1", 3), DEL_COMMAND, {});

        EXPECT_EQ(countCommandsWith({"PortChannel119", "ports.Ethernet1.runner.macsec_gate", "false"}), 1);
    }

    TEST_F(TeamMgrTest, testMacsecGate_ProfileRemovalReleases)
    {
        swss::TeamMgr teammgr(m_config_db.get(), m_app_db.get(), m_state_db.get(), cfg_lag_tables);
        setUpMacsecLag(m_config_db.get(), m_state_db.get(), teammgr, "PortChannel116", "Ethernet1", true, true);

        pushRow(teammgr, m_state_db.get(), STATE_MACSEC_INGRESS_SA_TABLE_NAME,
                saKey("Ethernet1", 0), DEL_COMMAND, {});
        ASSERT_TRUE(findCommandWith({"PortChannel116", "ports.Ethernet1.runner.macsec_gate", "false"}));
        mockCallArgs.clear();

        /* The operator removes the MACsec profile while the session is down.
         * CONFIG_DB loses the macsec field first; the SA rows are already gone
         * so no SA event will ever come -- the MACSEC_PORT_TABLE DEL that ends
         * the teardown is the only trigger that can hand the member back. */
        swss::Table cfg_port(m_config_db.get(), CFG_PORT_TABLE_NAME);
        cfg_port.hdel("Ethernet1", "macsec");
        pushRow(teammgr, m_state_db.get(), STATE_MACSEC_PORT_TABLE_NAME,
                "Ethernet1", DEL_COMMAND, {});

        EXPECT_TRUE(findCommandWith({"teamdctl", "PortChannel116", "state item set",
                                     "ports.Ethernet1.runner.macsec_gate", "true"}));
    }

    TEST_F(TeamMgrTest, testMacsecGate_NotAttachedNeverGated)
    {
        swss::TeamMgr teammgr(m_config_db.get(), m_app_db.get(), m_state_db.get(), cfg_lag_tables);
        setUpMacsecLag(m_config_db.get(), m_state_db.get(), teammgr, "PortChannel117", "Ethernet1", false, false);
        mockCallArgs.clear();

        // A stray SA DEL for a member without MACsec configured: not our concern.
        pushRow(teammgr, m_state_db.get(), STATE_MACSEC_INGRESS_SA_TABLE_NAME,
                saKey("Ethernet1", 0), DEL_COMMAND, {});

        EXPECT_FALSE(findCommandWith({"ports.Ethernet1.runner.macsec_gate"}));
    }

    TEST_F(TeamMgrTest, testMacsecGate_MacsecEnabledOnEnslavedMemberCloses)
    {
        swss::TeamMgr teammgr(m_config_db.get(), m_app_db.get(), m_state_db.get(), cfg_lag_tables);
        setUpMacsecLag(m_config_db.get(), m_state_db.get(), teammgr, "PortChannel118", "Ethernet1", false, false);
        mockCallArgs.clear();

        /* MACsec attached to a member that is already in the LAG: the hardware
         * drops cleartext until SAs exist, so the member is pulled as soon as
         * macsecorch creates the MACsec port, and returned on its first SA. */
        swss::Table cfg_port(m_config_db.get(), CFG_PORT_TABLE_NAME);
        cfg_port.set("Ethernet1", { { "admin_status", "up" }, { "mtu", "9100" },
                                    { "macsec", "macsec-profile-1" } });
        pushRow(teammgr, m_state_db.get(), STATE_MACSEC_PORT_TABLE_NAME,
                "Ethernet1", SET_COMMAND, { { "state", "ok" } });
        EXPECT_TRUE(findCommandWith({"PortChannel118", "ports.Ethernet1.runner.macsec_gate", "false"}));

        mockCallArgs.clear();
        pushRow(teammgr, m_state_db.get(), STATE_MACSEC_INGRESS_SA_TABLE_NAME,
                saKey("Ethernet1", 0), SET_COMMAND, { { "state", "ok" } });
        EXPECT_TRUE(findCommandWith({"PortChannel118", "ports.Ethernet1.runner.macsec_gate", "true"}));
    }

    TEST_F(TeamMgrTest, testMacsecGate_FreshAddWithoutSaPushesNoGate)
    {
        /* Fresh add with MACsec attached and no SA: the add stays deferred until
         * an SA appears, and no gate is pushed because the member is not a
         * teamd port yet. */
        swss::TeamMgr teammgr(m_config_db.get(), m_app_db.get(), m_state_db.get(), cfg_lag_tables);
        setUpMacsecLag(m_config_db.get(), m_state_db.get(), teammgr, "PortChannel119", "Ethernet1", true, false, false);

        swss::Table state_port(m_state_db.get(), STATE_PORT_TABLE_NAME);
        state_port.set("Ethernet1", { { "state", "ok" } });
        swss::Table state_lag(m_state_db.get(), STATE_LAG_TABLE_NAME);
        state_lag.set("PortChannel119", { { "state", "ok" } });
        swss::Table cfg_lag_member(m_config_db.get(), CFG_LAG_MEMBER_TABLE_NAME);
        mockCallArgs.clear();

        teammgr.addExistingData(&cfg_lag_member);
        teammgr.doTask();

        EXPECT_FALSE(findCommandWith({"ports.Ethernet1.runner.macsec_gate"}));
        EXPECT_FALSE(findCommandWith({"PortChannel119", "port add", "Ethernet1"}));

        // A stray SA DEL for the not-yet-added member pushes nothing either.
        pushRow(teammgr, m_state_db.get(), STATE_MACSEC_INGRESS_SA_TABLE_NAME,
                saKey("Ethernet1", 0), DEL_COMMAND, {});
        EXPECT_FALSE(findCommandWith({"ports.Ethernet1.runner.macsec_gate"}));
    }

    TEST_F(TeamMgrTest, testMacsecGate_EnslavedMemberWithoutSaIsHeld)
    {
        /* teammgrd comes back with the member already enslaved and MACsec down:
         * no SA event is coming to release a deferred add, so creating the LAG
         * has to hold the member through the gate. */
        swss::TeamMgr teammgr(m_config_db.get(), m_app_db.get(), m_state_db.get(), cfg_lag_tables);
        setUpMacsecLag(m_config_db.get(), m_state_db.get(), teammgr, "PortChannel120", "Ethernet1", true, false);

        EXPECT_TRUE(findCommandWith({"teamdctl", "PortChannel120", "state item set",
                                     "ports.Ethernet1.runner.macsec_gate", "false"}));

        swss::Table state_port(m_state_db.get(), STATE_PORT_TABLE_NAME);
        state_port.set("Ethernet1", { { "state", "ok" } });
        swss::Table state_lag(m_state_db.get(), STATE_LAG_TABLE_NAME);
        state_lag.set("PortChannel120", { { "state", "ok" } });
        swss::Table cfg_lag_member(m_config_db.get(), CFG_LAG_MEMBER_TABLE_NAME);
        mockCallArgs.clear();

        teammgr.addExistingData(&cfg_lag_member);
        teammgr.doTask();

        EXPECT_FALSE(findCommandWith({"ports.Ethernet1.runner.macsec_gate"}));
        EXPECT_FALSE(findCommandWith({"PortChannel120", "port add", "Ethernet1"}));
    }

    TEST_F(TeamMgrTest, testMacsecGate_EventAfterFailedPushRetries)
    {
        /* A failed teamdctl push must not be recorded as done: the next event
         * for the port has to try again. */
        swss::TeamMgr teammgr(m_config_db.get(), m_app_db.get(), m_state_db.get(), cfg_lag_tables);
        setUpMacsecLag(m_config_db.get(), m_state_db.get(), teammgr, "PortChannel121", "Ethernet1", true, true);

        failMacsecGatePush = true;
        pushRow(teammgr, m_state_db.get(), STATE_MACSEC_INGRESS_SA_TABLE_NAME,
                saKey("Ethernet1", 0), DEL_COMMAND, {});
        ASSERT_EQ(countCommandsWith({"PortChannel121", "ports.Ethernet1.runner.macsec_gate", "false"}), 1);
        mockCallArgs.clear();

        failMacsecGatePush = false;
        // Another event for the same port (an SC-teardown cascade DEL).
        pushRow(teammgr, m_state_db.get(), STATE_MACSEC_INGRESS_SA_TABLE_NAME,
                saKey("Ethernet1", 1), DEL_COMMAND, {});
        EXPECT_TRUE(findCommandWith({"PortChannel121", "ports.Ethernet1.runner.macsec_gate", "false"}));
    }

    TEST_F(TeamMgrTest, testMacsecGate_SweepRetriesFailedPush)
    {
        /* A failed teamdctl on the last SA delete must be retried from the
         * one-second doTask() sweep, with no further STATE_DB event. */
        swss::TeamMgr teammgr(m_config_db.get(), m_app_db.get(), m_state_db.get(), cfg_lag_tables);
        setUpMacsecLag(m_config_db.get(), m_state_db.get(), teammgr, "PortChannel131", "Ethernet1", true, true);

        failMacsecGatePush = true;
        pushRow(teammgr, m_state_db.get(), STATE_MACSEC_INGRESS_SA_TABLE_NAME,
                saKey("Ethernet1", 0), DEL_COMMAND, {});
        ASSERT_EQ(countCommandsWith({"PortChannel131", "ports.Ethernet1.runner.macsec_gate", "false"}), 1);
        mockCallArgs.clear();

        failMacsecGatePush = false;
        teammgr.doTask();
        EXPECT_TRUE(findCommandWith({"PortChannel131", "ports.Ethernet1.runner.macsec_gate", "false"}));

        mockCallArgs.clear();
        teammgr.doTask();
        EXPECT_FALSE(findCommandWith({"ports.Ethernet1.runner.macsec_gate"}));
    }

    TEST_F(TeamMgrTest, testMacsecGate_SaEventBeforeLagCreateIsAppliedAfter)
    {
        /* An SA DEL for an already-enslaved member can be processed before the
         * LAG is in m_lagList. Creating the LAG must still close the gate. */
        swss::TeamMgr teammgr(m_config_db.get(), m_app_db.get(), m_state_db.get(), cfg_lag_tables);

        swss::Table cfg_port(m_config_db.get(), CFG_PORT_TABLE_NAME);
        cfg_port.set("Ethernet1", { { "admin_status", "up" }, { "mtu", "9100" },
                                    { "macsec", "macsec-profile-1" } });
        swss::Table cfg_lag_member(m_config_db.get(), CFG_LAG_MEMBER_TABLE_NAME);
        cfg_lag_member.set("PortChannel132|Ethernet1", { { "NULL", "NULL" } });
        mockEnslavedPorts.insert("Ethernet1");

        pushRow(teammgr, m_state_db.get(), STATE_MACSEC_INGRESS_SA_TABLE_NAME,
                saKey("Ethernet1", 0), DEL_COMMAND, {});
        EXPECT_FALSE(findCommandWith({"ports.Ethernet1.runner.macsec_gate"}));

        mockCallArgs.clear();
        swss::Table cfg_lag(m_config_db.get(), CFG_LAG_TABLE_NAME);
        cfg_lag.set("PortChannel132", { { "admin_status", "up" }, { "mtu", "9100" },
                                        { "lacp_key", "auto" }, { "min_links", "1" } });
        teammgr.addExistingData(&cfg_lag);
        teammgr.doTask();
        EXPECT_TRUE(findCommandWith({"PortChannel132", "ports.Ethernet1.runner.macsec_gate", "false"}));
    }

    TEST_F(TeamMgrTest, testMacsecGate_LagDeleteForgetsState)
    {
        swss::TeamMgr teammgr(m_config_db.get(), m_app_db.get(), m_state_db.get(), cfg_lag_tables);
        setUpMacsecLag(m_config_db.get(), m_state_db.get(), teammgr, "PortChannel117", "Ethernet1", true, true);

        pushRow(teammgr, m_state_db.get(), STATE_MACSEC_INGRESS_SA_TABLE_NAME,
                saKey("Ethernet1", 0), DEL_COMMAND, {});
        ASSERT_TRUE(findCommandWith({"PortChannel117", "ports.Ethernet1.runner.macsec_gate", "false"}));

        // Delete the PortChannel outright: no teamd left to un-gate. The DEL
        // has to be fed as a notification; addExistingData() only replays SETs.
        pushRow(teammgr, m_config_db.get(), CFG_LAG_TABLE_NAME, "PortChannel117", DEL_COMMAND, {});
        mockCallArgs.clear();

        // A late SA event for the orphaned member must not push anything.
        pushRow(teammgr, m_state_db.get(), STATE_MACSEC_INGRESS_SA_TABLE_NAME,
                saKey("Ethernet1", 0), SET_COMMAND, { { "state", "ok" } });
        EXPECT_FALSE(findCommandWith({"ports.Ethernet1.runner.macsec_gate"}));
    }

    TEST_F(TeamMgrTest, testMacsecGate_LagDeleteThenRecreateStartsFresh)
    {
        /* The LAG delete must actually drop the bookkeeping: after a delete and
         * recreate, a fresh close has to be pushed rather than suppressed by a
         * stale "already false" from the previous life. */
        swss::TeamMgr teammgr(m_config_db.get(), m_app_db.get(), m_state_db.get(), cfg_lag_tables);
        setUpMacsecLag(m_config_db.get(), m_state_db.get(), teammgr, "PortChannel124", "Ethernet1", true, true);

        pushRow(teammgr, m_state_db.get(), STATE_MACSEC_INGRESS_SA_TABLE_NAME,
                saKey("Ethernet1", 0), DEL_COMMAND, {});
        ASSERT_TRUE(findCommandWith({"PortChannel124", "ports.Ethernet1.runner.macsec_gate", "false"}));

        pushRow(teammgr, m_config_db.get(), CFG_LAG_TABLE_NAME, "PortChannel124", DEL_COMMAND, {});

        // Recreate the same LAG; the member is still enslaved with no SA, so
        // the post-create re-evaluation must close the gate rather than
        // inherit a stale "already false" from the previous life.
        swss::Table cfg_lag(m_config_db.get(), CFG_LAG_TABLE_NAME);
        cfg_lag.set("PortChannel124", { { "admin_status", "up" }, { "mtu", "9100" },
                                        { "lacp_key", "auto" }, { "min_links", "1" } });
        mockCallArgs.clear();
        teammgr.addExistingData(&cfg_lag);
        teammgr.doTask();
        EXPECT_TRUE(findCommandWith({"PortChannel124", "ports.Ethernet1.runner.macsec_gate", "false"}));
    }

    TEST_F(TeamMgrTest, testMacsecGate_EventPathRunsWithMixedMembers)
    {
        /* Two members, one with MACsec and one without: the MACsec member with a
         * downed SA is gated while the plain member is untouched. */
        swss::TeamMgr teammgr(m_config_db.get(), m_app_db.get(), m_state_db.get(), cfg_lag_tables);
        setUpMacsecLag(m_config_db.get(), m_state_db.get(), teammgr, "PortChannel125", "Ethernet1", true, true);
        swss::Table cfg_port(m_config_db.get(), CFG_PORT_TABLE_NAME);
        cfg_port.set("Ethernet2", { { "admin_status", "up" }, { "mtu", "9100" } });
        swss::Table cfg_lag_member(m_config_db.get(), CFG_LAG_MEMBER_TABLE_NAME);
        cfg_lag_member.set("PortChannel125|Ethernet2", { { "NULL", "NULL" } });
        mockEnslavedPorts.insert("Ethernet2");
        mockCallArgs.clear();

        pushRow(teammgr, m_state_db.get(), STATE_MACSEC_INGRESS_SA_TABLE_NAME,
                saKey("Ethernet1", 0), DEL_COMMAND, {});
        EXPECT_TRUE(findCommandWith({"PortChannel125", "ports.Ethernet1.runner.macsec_gate", "false"}));
        EXPECT_FALSE(findCommandWith({"ports.Ethernet2.runner.macsec_gate"}));
    }

    TEST_F(TeamMgrTest, testMacsecGate_PerMemberKeying)
    {
        /* Gate state is kept per member: one member's events must not move
         * another's gate. */
        swss::TeamMgr teammgr(m_config_db.get(), m_app_db.get(), m_state_db.get(), cfg_lag_tables);
        setUpMacsecLag(m_config_db.get(), m_state_db.get(), teammgr, "PortChannel126", "Ethernet1", true, true);
        swss::Table cfg_port(m_config_db.get(), CFG_PORT_TABLE_NAME);
        cfg_port.set("Ethernet2", { { "admin_status", "up" }, { "mtu", "9100" },
                                    { "macsec", "macsec-profile-1" } });
        swss::Table cfg_lag_member(m_config_db.get(), CFG_LAG_MEMBER_TABLE_NAME);
        cfg_lag_member.set("PortChannel126|Ethernet2", { { "NULL", "NULL" } });
        swss::Table sa(m_state_db.get(), STATE_MACSEC_INGRESS_SA_TABLE_NAME);
        sa.set(saKey("Ethernet2", 0), { { "state", "ok" } });
        swss::Table mp(m_state_db.get(), STATE_MACSEC_PORT_TABLE_NAME);
        mp.set("Ethernet2", { { "state", "ok" } });
        mockEnslavedPorts.insert("Ethernet2");
        mockCallArgs.clear();

        // Close Ethernet1's gate.
        pushRow(teammgr, m_state_db.get(), STATE_MACSEC_INGRESS_SA_TABLE_NAME,
                saKey("Ethernet1", 0), DEL_COMMAND, {});
        EXPECT_TRUE(findCommandWith({"PortChannel126", "ports.Ethernet1.runner.macsec_gate", "false"}));
        EXPECT_FALSE(findCommandWith({"ports.Ethernet2.runner.macsec_gate"}));
        mockCallArgs.clear();

        // An SA event on Ethernet2 must not reopen Ethernet1.
        pushRow(teammgr, m_state_db.get(), STATE_MACSEC_INGRESS_SA_TABLE_NAME,
                saKey("Ethernet2", 1), SET_COMMAND, { { "state", "ok" } });
        EXPECT_FALSE(findCommandWith({"ports.Ethernet1.runner.macsec_gate"}));
        EXPECT_FALSE(findCommandWith({"ports.Ethernet2.runner.macsec_gate"}));
    }

    TEST_F(TeamMgrTest, testMacsecGate_NonLagMemberSaEventIgnored)
    {
        /* An SA event for a port that is not a PortChannel member at all must
         * push nothing. */
        swss::TeamMgr teammgr(m_config_db.get(), m_app_db.get(), m_state_db.get(), cfg_lag_tables);
        setUpMacsecLag(m_config_db.get(), m_state_db.get(), teammgr, "PortChannel127", "Ethernet1", true, true);
        swss::Table cfg_port(m_config_db.get(), CFG_PORT_TABLE_NAME);
        cfg_port.set("Ethernet9", { { "admin_status", "up" }, { "mtu", "9100" },
                                    { "macsec", "macsec-profile-1" } });
        mockEnslavedPorts.insert("Ethernet9");
        mockCallArgs.clear();

        pushRow(teammgr, m_state_db.get(), STATE_MACSEC_INGRESS_SA_TABLE_NAME,
                saKey("Ethernet9", 0), DEL_COMMAND, {});
        EXPECT_FALSE(findCommandWith({"ports.Ethernet9.runner.macsec_gate"}));
    }

    TEST_F(TeamMgrTest, testMacsecGate_PortUpdateTaskGatesEnslavedMember)
    {
        /* doPortUpdateTask (STATE_PORT_TABLE, the port-recreate path) must gate
         * an enslaved MACsec member whose SA is down instead of retaining the
         * update forever. The SA is removed from STATE_DB without a
         * notification so this path, not the SA consumer, is what closes. */
        swss::TeamMgr teammgr(m_config_db.get(), m_app_db.get(), m_state_db.get(), cfg_lag_tables);
        setUpMacsecLag(m_config_db.get(), m_state_db.get(), teammgr, "PortChannel129", "Ethernet1", true, true);
        swss::Table sa(m_state_db.get(), STATE_MACSEC_INGRESS_SA_TABLE_NAME);
        sa.del(saKey("Ethernet1", 0));
        mockCallArgs.clear();

        pushRow(teammgr, m_state_db.get(), STATE_PORT_TABLE_NAME,
                "Ethernet1", SET_COMMAND, { { "state", "ok" } });
        EXPECT_TRUE(findCommandWith({"PortChannel129", "ports.Ethernet1.runner.macsec_gate", "false"}));
        EXPECT_FALSE(findCommandWith({"PortChannel129", "port add", "Ethernet1"}));
    }

    TEST_F(TeamMgrTest, testMacsecGate_MemberRemovalForgets)
    {
        /* removeLagMember must forget the member's gate: after removal a late SA
         * event for the (now non-member) port pushes nothing. */
        swss::TeamMgr teammgr(m_config_db.get(), m_app_db.get(), m_state_db.get(), cfg_lag_tables);
        setUpMacsecLag(m_config_db.get(), m_state_db.get(), teammgr, "PortChannel130", "Ethernet1", true, true);

        pushRow(teammgr, m_state_db.get(), STATE_MACSEC_INGRESS_SA_TABLE_NAME,
                saKey("Ethernet1", 0), DEL_COMMAND, {});
        ASSERT_TRUE(findCommandWith({"PortChannel130", "ports.Ethernet1.runner.macsec_gate", "false"}));

        // Remove the member config (DEL notification) -> removeLagMember -> forget.
        pushRow(teammgr, m_config_db.get(), CFG_LAG_MEMBER_TABLE_NAME, "PortChannel130|Ethernet1", DEL_COMMAND, {});
        mockEnslavedPorts.erase("Ethernet1");
        mockCallArgs.clear();

        // A late SA event for the removed member must push nothing.
        pushRow(teammgr, m_state_db.get(), STATE_MACSEC_INGRESS_SA_TABLE_NAME,
                saKey("Ethernet1", 0), SET_COMMAND, { { "state", "ok" } });
        EXPECT_FALSE(findCommandWith({"ports.Ethernet1.runner.macsec_gate"}));
    }
}
