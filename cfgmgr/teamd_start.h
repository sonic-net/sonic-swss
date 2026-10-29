#pragma once

#include <chrono>
#include <functional>
#include <string>

namespace swss
{

enum class TeamdStartResult
{
    success = 0,
    // No child launched yet, or the failed attempt was completely cleaned up.
    retry = 75,
    blocked = 76
};

struct TeamdStartTimeouts
{
    std::chrono::milliseconds launcher{60000};
    std::chrono::milliseconds terminate{5000};
    std::chrono::milliseconds kill{5000};
    std::chrono::milliseconds cleanup{5000};
    std::chrono::milliseconds poll{10};
};

// Run only in a dedicated, single-threaded helper with no existing children.
// The launcher normally execs teamd -d. The helper temporarily adopts its
// double-forked daemon, so failed-start cleanup never trusts a PID file.
TeamdStartResult superviseTeamdStart(
    const std::string &alias,
    const std::string &runtimeDirectory,
    const std::string &fenceDirectory,
    const std::function<int()> &launcher,
    const std::function<bool()> &interfaceAbsent,
    const TeamdStartTimeouts &timeouts = TeamdStartTimeouts{});

bool teamInterfaceAbsent(const std::string &alias);

}
