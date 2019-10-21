#ifndef __SHELLCMD__
#define __SHELLCMD__

#include <iomanip>
#include <regex>

#define IP_CMD               "/sbin/ip"
#define BRIDGE_CMD           "/sbin/bridge"
#define BRCTL_CMD            "/sbin/brctl"
#define ECHO_CMD             "/bin/echo"
#define BASH_CMD             "/bin/bash"
#define GREP_CMD             "/bin/grep"
#define TEAMD_CMD            "/usr/bin/teamd"
#define TEAMDCTL_CMD         "/usr/bin/teamdctl"

#define EXEC_WITH_ERROR_THROW(cmd, res)   ({    \
    int ret = swss::exec(cmd, res);             \
    if (ret != 0)                               \
    {                                           \
        throw runtime_error(cmd + " : " + res); \
    }                                           \
})

static inline std::string shellquote(const std::string& str)
{
    static const std::regex re("([$`\"\\\n])");
    return "\"" + std::regex_replace(str, re, "\\$1") + "\"";
}

#endif /* __SHELLCMD__ */
