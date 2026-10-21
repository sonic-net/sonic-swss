#include "gtest/gtest.h"
#include "teamd_start.h"

#include <cerrno>
#include <dirent.h>
#include <fstream>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

namespace
{

volatile sig_atomic_t terminateRequested = 0;

void requestTermination(int)
{
    terminateRequested = 1;
}

void writeFile(const std::string &path, const std::string &value = "1")
{
    std::ofstream output(path);
    output << value;
    output.close();
    if (!output)
        _exit(120);
}

bool fileAbsent(const std::string &path)
{
    struct stat st;
    return lstat(path.c_str(), &st) < 0 && errno == ENOENT;
}

void removeTree(const std::string &path)
{
    DIR *dir = opendir(path.c_str());
    if (!dir)
    {
        unlink(path.c_str());
        return;
    }
    while (auto *entry = readdir(dir))
    {
        const std::string name(entry->d_name);
        if (name == "." || name == "..")
            continue;
        removeTree(path + "/" + name);
    }
    closedir(dir);
    rmdir(path.c_str());
}

enum class DaemonMode
{
    normal,
    lateInterface,
    ignoresTerm,
    missingPid,
    residualInterface
};

class TeamdStartupTest : public ::testing::Test
{
protected:
    std::string root;
    std::string runtime;
    std::string fences;
    const std::string alias = "PortChannel1";
    swss::TeamdStartTimeouts timeouts;

    void SetUp() override
    {
        static unsigned int sequence = 0;
        root = "teamd-start-test-" + std::to_string(getpid()) + "-" +
               std::to_string(++sequence);
        runtime = root + "/runtime";
        fences = root + "/fences";
        ASSERT_EQ(mkdir(root.c_str(), 0700), 0);
        ASSERT_EQ(mkdir(runtime.c_str(), 0700), 0);
        timeouts.launcher = std::chrono::milliseconds(300);
        timeouts.terminate = std::chrono::milliseconds(100);
        timeouts.kill = std::chrono::milliseconds(300);
        timeouts.cleanup = std::chrono::milliseconds(50);
        timeouts.poll = std::chrono::milliseconds(1);
    }

    void TearDown() override
    {
        removeTree(root);
    }

    int daemonLauncher(DaemonMode mode, int launcherStatus)
    {
        int ready[2];
        if (pipe(ready) < 0)
            return 121;
        const auto intermediate = fork();
        if (intermediate < 0)
            return 121;
        if (intermediate == 0)
        {
            const auto daemon = fork();
            if (daemon < 0)
                _exit(121);
            if (daemon > 0)
                _exit(0);

            close(ready[0]);
            terminateRequested = 0;
            signal(SIGTERM, mode == DaemonMode::ignoresTerm ? SIG_IGN : requestTermination);
            if (mode != DaemonMode::missingPid)
                writeFile(runtime + "/" + alias + ".pid", std::to_string(getpid()));
            if (write(ready[1], "r", 1) != 1)
                _exit(121);
            close(ready[1]);

            while (!terminateRequested)
                usleep(1000);
            writeFile(root + "/term");
            if (mode == DaemonMode::lateInterface || mode == DaemonMode::residualInterface)
            {
                writeFile(root + "/link");
                writeFile(root + "/created");
                usleep(20000);
            }
            if (mode != DaemonMode::residualInterface)
                unlink((root + "/link").c_str());
            unlink((runtime + "/" + alias + ".pid").c_str());
            writeFile(root + "/exited");
            _exit(0);
        }
        close(ready[1]);
        int status;
        if (waitpid(intermediate, &status, 0) != intermediate)
            return 121;
        char value;
        const auto count = read(ready[0], &value, 1);
        close(ready[0]);
        return count == 1 ? launcherStatus : 121;
    }

    int run(const std::function<int()> &launcher,
            const std::function<bool()> &interfaceAbsent = {})
    {
        const auto helper = fork();
        if (helper < 0)
            return -1;
        if (helper == 0)
        {
            const auto probe = interfaceAbsent ? interfaceAbsent : [this]() {
                int status;
                // Kernel cleanup is never checked while a child remains.
                if (waitpid(-1, &status, WNOHANG) != -1 || errno != ECHILD)
                {
                    writeFile(root + "/early-probe");
                    return false;
                }
                return fileAbsent(root + "/link");
            };
            const auto result = swss::superviseTeamdStart(
                alias, runtime, fences, launcher, probe, timeouts);

            if (result == swss::TeamdStartResult::success)
            {
                // The production helper exits and releases this healthy daemon
                // to init. The test still owns it and reaps it before returning.
                std::ifstream input(runtime + "/" + alias + ".pid");
                pid_t daemon = -1;
                input >> daemon;
                if (daemon <= 1 || kill(daemon, SIGTERM) < 0)
                    _exit(122);
                int status;
                if (waitpid(daemon, &status, 0) != daemon)
                    _exit(122);
            }

            int status;
            if (waitpid(-1, &status, WNOHANG) != -1 || errno != ECHILD)
            {
                // A blocked result must not let the SIGKILL test pass with a
                // leaked daemon. Clean up only this test helper's own children.
                std::ifstream input("/proc/self/task/" + std::to_string(getpid()) + "/children");
                pid_t child;
                while (input >> child)
                {
                    if (child > 1)
                    {
                        kill(child, SIGKILL);
                        waitpid(child, &status, 0);
                    }
                }
                _exit(123);
            }
            _exit(static_cast<int>(result));
        }
        int status;
        if (waitpid(helper, &status, 0) != helper)
            return -1;
        return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
    }

    int start(DaemonMode mode, int launcherStatus = 1)
    {
        return run([=]() { return daemonLauncher(mode, launcherStatus); });
    }
};

TEST_F(TeamdStartupTest, AcknowledgedDaemonKeepsExistingStartupContract)
{
    EXPECT_EQ(start(DaemonMode::normal, 0), 0);
    EXPECT_EQ(start(DaemonMode::normal, 0), 0);
}

TEST_F(TeamdStartupTest, FailedLauncherWaitsForLateInterfaceCleanup)
{
    EXPECT_EQ(start(DaemonMode::lateInterface), 75);
    EXPECT_FALSE(fileAbsent(root + "/created"));
    EXPECT_FALSE(fileAbsent(root + "/exited"));
    EXPECT_TRUE(fileAbsent(root + "/link"));
    EXPECT_TRUE(fileAbsent(root + "/early-probe"));
    EXPECT_EQ(start(DaemonMode::normal, 0), 0);
}

TEST_F(TeamdStartupTest, MissingPidFileDoesNotLoseTheOwnedDaemon)
{
    EXPECT_EQ(start(DaemonMode::missingPid), 75);
    EXPECT_FALSE(fileAbsent(root + "/term"));
    EXPECT_FALSE(fileAbsent(root + "/exited"));
}

TEST_F(TeamdStartupTest, SuccessWithoutMatchingOwnedPidIsNotAccepted)
{
    EXPECT_EQ(start(DaemonMode::missingPid, 0), 75);
    EXPECT_FALSE(fileAbsent(root + "/term"));
}

TEST_F(TeamdStartupTest, SigkillLeavesResidualRuntimeFilesFenced)
{
    const auto unrelated = fork();
    ASSERT_GE(unrelated, 0);
    if (unrelated == 0)
    {
        for (;;)
            pause();
    }

    EXPECT_EQ(start(DaemonMode::ignoresTerm), 76);
    EXPECT_EQ(kill(unrelated, 0), 0);
    EXPECT_EQ(kill(unrelated, SIGKILL), 0);
    int status;
    EXPECT_EQ(waitpid(unrelated, &status, 0), unrelated);

    EXPECT_EQ(run([&]() { writeFile(root + "/second-launch"); return 0; }), 76);
    EXPECT_TRUE(fileAbsent(root + "/second-launch"));
}

TEST_F(TeamdStartupTest, ResidualInterfaceBlocksRetryAcrossHelperRestart)
{
    EXPECT_EQ(start(DaemonMode::residualInterface), 76);
    EXPECT_FALSE(fileAbsent(root + "/exited"));
    EXPECT_FALSE(fileAbsent(root + "/link"));
    unlink((root + "/link").c_str());
    EXPECT_EQ(run([&]() { writeFile(root + "/second-launch"); return 0; }), 76);
    EXPECT_TRUE(fileAbsent(root + "/second-launch"));
}

TEST_F(TeamdStartupTest, HelperCrashRetainsTheFence)
{
    EXPECT_EQ(run([]() { return 1; }, []() {
        kill(getpid(), SIGKILL);
        return false;
    }), 128 + SIGKILL);
    EXPECT_EQ(run([&]() { writeFile(root + "/second-launch"); return 0; }), 76);
    EXPECT_TRUE(fileAbsent(root + "/second-launch"));
}

TEST_F(TeamdStartupTest, LauncherWatchdogAlsoReapsAnUnresponsiveLauncher)
{
    EXPECT_EQ(run([]() {
        signal(SIGTERM, SIG_IGN);
        for (;;)
            pause();
        return 0;
    }), 75);
}

TEST_F(TeamdStartupTest, ExistingRuntimePidIsNotSignalledOrReplaced)
{
    writeFile(runtime + "/" + alias + ".pid", std::to_string(getpid()));
    EXPECT_EQ(run([&]() { writeFile(root + "/launched"); return 0; }), 75);
    EXPECT_TRUE(fileAbsent(root + "/launched"));
}

TEST_F(TeamdStartupTest, ExistingRuntimeSocketBlocksLaunch)
{
    writeFile(runtime + "/" + alias + ".sock");
    EXPECT_EQ(run([&]() { writeFile(root + "/launched"); return 0; }), 75);
    EXPECT_TRUE(fileAbsent(root + "/launched"));
}

TEST_F(TeamdStartupTest, CleanupLookupFailureIsNotAbsence)
{
    EXPECT_EQ(run([]() { return 1; }, []() { return false; }), 76);
}

TEST_F(TeamdStartupTest, FailedExecWithoutChildrenCanRetryAfterCleanup)
{
    EXPECT_EQ(run([]() { return 127; }), 75);
    EXPECT_EQ(run([]() { return 127; }), 75);
}

TEST_F(TeamdStartupTest, NormalTeardownDefersWithoutPermanentlyFencingRecreation)
{
    writeFile(runtime + "/" + alias + ".pid", std::to_string(getpid()));
    writeFile(runtime + "/" + alias + ".sock");
    EXPECT_EQ(run([&]() { writeFile(root + "/launched"); return 0; }), 75);
    unlink((runtime + "/" + alias + ".pid").c_str());
    EXPECT_EQ(run([&]() { writeFile(root + "/launched"); return 0; }), 75);
    EXPECT_TRUE(fileAbsent(root + "/launched"));
    unlink((runtime + "/" + alias + ".sock").c_str());
    EXPECT_EQ(start(DaemonMode::normal, 0), 0);
}

TEST_F(TeamdStartupTest, ConcurrentHelperCannotLaunchTheSameAlias)
{
    const auto first = fork();
    ASSERT_GE(first, 0);
    if (first == 0)
    {
        _exit(run([&]() {
            writeFile(root + "/first-launch");
            while (fileAbsent(root + "/release"))
                usleep(1000);
            return 1;
        }));
    }
    for (unsigned int i = 0; i != 200 && fileAbsent(root + "/first-launch"); ++i)
        usleep(1000);

    EXPECT_FALSE(fileAbsent(root + "/first-launch"));
    EXPECT_EQ(run([&]() { writeFile(root + "/second-launch"); return 1; }), 76);
    EXPECT_TRUE(fileAbsent(root + "/second-launch"));
    writeFile(root + "/release");
    int status;
    ASSERT_EQ(waitpid(first, &status, 0), first);
    ASSERT_TRUE(WIFEXITED(status));
    EXPECT_EQ(WEXITSTATUS(status), 75);
}

}
