#include "teamd_start.h"

#include <algorithm>
#include <cerrno>
#include <climits>
#include <fstream>
#include <set>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <net/if.h>
#include <signal.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <syslog.h>
#include <unistd.h>

namespace swss
{
namespace
{

using Clock = std::chrono::steady_clock;

bool absent(const std::string &path)
{
    struct stat st;
    return lstat(path.c_str(), &st) < 0 && errno == ENOENT;
}

bool children(std::vector<pid_t> &pids)
{
    std::ifstream input("/proc/self/task/" + std::to_string(getpid()) + "/children");
    if (!input)
        return false;

    long value;
    while (input >> value)
    {
        if (value <= 1 || value > INT_MAX)
            return false;
        pids.push_back(static_cast<pid_t>(value));
    }
    return input.eof();
}

bool acknowledgedChild(const std::string &pidPath)
{
    std::vector<pid_t> pids;
    if (!children(pids) || pids.size() != 1)
        return false;

    std::ifstream input(pidPath);
    long value = 0;
    if (!(input >> value) || value != pids.front())
        return false;
    input >> std::ws;
    if (!input.eof())
        return false;

    // Only this helper reaps its children. An unreaped child's PID cannot be
    // reused between enumeration, this check, and signal delivery.
    int status;
    return waitpid(pids.front(), &status, WNOHANG) == 0;
}

enum class DrainResult
{
    empty,
    expired,
    error
};

DrainResult drainChildren(int signal, std::chrono::milliseconds duration,
                          std::chrono::milliseconds interval)
{
    const auto deadline = Clock::now() + duration;
    std::set<pid_t> signalled;
    do
    {
        for (;;)
        {
            int status;
            const pid_t pid = waitpid(-1, &status, WNOHANG);
            if (pid > 0)
            {
                signalled.erase(pid);
                continue;
            }
            if (pid < 0 && errno == ECHILD)
                return DrainResult::empty;
            if (pid < 0 && errno != EINTR)
                return DrainResult::error;
            break;
        }

        std::vector<pid_t> pids;
        if (!children(pids))
            return DrainResult::error;
        for (const auto pid : pids)
        {
            if (!signalled.insert(pid).second)
                continue;
            if (::kill(pid, signal) < 0 && errno != ESRCH)
                return DrainResult::error;
        }

        // Re-enumerate: terminating a launcher/intermediate process can adopt
        // another descendant. ECHILD, not one empty /proc snapshot, is the gate.
        if (Clock::now() >= deadline)
            break;
        std::this_thread::sleep_for(interval);
    } while (true);
    return DrainResult::expired;
}

}

bool teamInterfaceAbsent(const std::string &alias)
{
    errno = 0;
    if (if_nametoindex(alias.c_str()) != 0)
        return false;
    return errno == ENODEV || errno == ENXIO;
}

TeamdStartResult superviseTeamdStart(
    const std::string &alias,
    const std::string &runtimeDirectory,
    const std::string &fenceDirectory,
    const std::function<int()> &launcher,
    const std::function<bool()> &interfaceAbsent,
    const TeamdStartTimeouts &timeouts)
{
    if (alias.empty() || alias.size() >= IFNAMSIZ ||
        alias.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-")
            != std::string::npos)
        return TeamdStartResult::blocked;

    struct stat ns;
    if (stat("/proc/self/ns/pid", &ns) < 0 ||
        (mkdir(fenceDirectory.c_str(), 0700) < 0 && errno != EEXIST))
        return TeamdStartResult::blocked;

    // Survives helper/manager crashes within the same PID namespace. A normal
    // container replacement has a new namespace and cannot inherit live children.
    const auto fence = fenceDirectory + "/" + std::to_string(ns.st_ino) + "-" + alias;
    int fd = open(fence.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0)
        return TeamdStartResult::blocked;
    close(fd);

    const auto pidPath = runtimeDirectory + "/" + alias + ".pid";
    const auto sockPath = runtimeDirectory + "/" + alias + ".sock";
    if (!absent(pidPath) || !absent(sockPath))
    {
        // Normal DEL can still be removing the previous successful daemon's
        // runtime files. Nothing was launched, so defer without a new fence.
        // An existing uncertain-attempt fence was already rejected above.
        if (unlink(fence.c_str()) < 0)
            return TeamdStartResult::blocked;
        return TeamdStartResult::retry;
    }

    struct sigaction action = {};
    action.sa_handler = SIG_DFL;
    if (sigemptyset(&action.sa_mask) < 0 ||
        sigaction(SIGCHLD, &action, nullptr) < 0)
        return TeamdStartResult::blocked;

    int status;
    errno = 0;
    if (waitpid(-1, &status, WNOHANG) != -1 || errno != ECHILD ||
        prctl(PR_SET_CHILD_SUBREAPER, 1) < 0)
        return TeamdStartResult::blocked;

    const pid_t pid = fork();
    if (pid == 0)
    {
        // A child stuck before daemonization must not keep popen's output pipe
        // open after this helper's bounded cleanup deadline.
        const int nullfd = open("/dev/null", O_RDWR);
        if (nullfd < 0)
            _exit(127);
        for (const int target : {STDIN_FILENO, STDOUT_FILENO})
        {
            if (dup2(nullfd, target) < 0)
                _exit(127);
        }
        if (nullfd > 2)
            close(nullfd);
        _exit(launcher());
    }

    bool success = false;
    if (pid > 0)
    {
        const auto deadline = Clock::now() + timeouts.launcher;
        do
        {
            const auto result = waitpid(pid, &status, WNOHANG);
            if (result == pid)
            {
                success = WIFEXITED(status) && WEXITSTATUS(status) == 0;
                break;
            }
            if (result < 0 && errno != EINTR)
                return TeamdStartResult::blocked;
            if (Clock::now() >= deadline)
                break;
            std::this_thread::sleep_for(timeouts.poll);
        } while (true);
    }

    if (success && acknowledgedChild(pidPath))
    {
        if (unlink(fence.c_str()) < 0)
            return TeamdStartResult::blocked;
        return TeamdStartResult::success;
    }

    syslog(LOG_WARNING, "Port channel %s startup failed; terminating owned teamd children",
           alias.c_str());
    auto result = drainChildren(SIGTERM, timeouts.terminate, timeouts.poll);
    if (result == DrainResult::expired)
        result = drainChildren(SIGKILL, timeouts.kill, timeouts.poll);
    if (result != DrainResult::empty)
        return TeamdStartResult::blocked;

    const auto deadline = Clock::now() + timeouts.cleanup;
    do
    {
        if (interfaceAbsent() && absent(pidPath) && absent(sockPath))
        {
            if (unlink(fence.c_str()) < 0)
                return TeamdStartResult::blocked;
            return TeamdStartResult::retry;
        }
        if (Clock::now() >= deadline)
            break;
        std::this_thread::sleep_for(timeouts.poll);
    } while (true);

    syslog(LOG_ERR, "Port channel %s startup cleanup incomplete; leaving retry fenced",
           alias.c_str());
    return TeamdStartResult::blocked;
}

}
