#include "teamd_start.h"

#include <exception>
#include <vector>
#include <syslog.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    openlog("teammgrd-start", LOG_PID, LOG_DAEMON);
    if (argc < 3)
        return static_cast<int>(swss::TeamdStartResult::blocked);

    try
    {
        const std::string alias(argv[1]);
        // Keep /usr/bin/teamd out of this helper's command line: warm/fast
        // shutdown targets the real teamd processes with pkill -f.
        std::vector<char *> args{const_cast<char *>("/usr/bin/teamd")};
        args.insert(args.end(), argv + 2, argv + argc);
        args.push_back(nullptr);
        const auto result = swss::superviseTeamdStart(
            alias, "/var/run/teamd", "/run/teamd-start",
            [&]() {
                execv(args.front(), args.data());
                return 127;
            },
            [&]() { return swss::teamInterfaceAbsent(alias); });
        if (result == swss::TeamdStartResult::blocked)
            syslog(LOG_ERR, "Port channel %s launch is fenced; explicit recovery is required",
                   alias.c_str());
        return static_cast<int>(result);
    }
    catch (const std::exception &e)
    {
        syslog(LOG_ERR, "teamd startup supervisor failed: %s", e.what());
        return static_cast<int>(swss::TeamdStartResult::blocked);
    }
}
