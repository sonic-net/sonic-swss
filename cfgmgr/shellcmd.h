#ifndef __SHELLCMD__
#define __SHELLCMD__

#define IP_CMD               "/sbin/ip"
#define BRIDGE_CMD           "/sbin/bridge"
#define ECHO_CMD             "/bin/echo"
#define REDIS_CLI_CMD        "/usr/bin/redis-cli"
#define XARGS_CMD            "/usr/bin/xargs"
#define GREP_CMD             "/bin/grep"
#define AWK_CMD              "/usr/bin/awk"
#define LS_CMD               "/bin/ls"
#define PASTE_CMD            "/usr/bin/paste"
#define SED_CMD              "/bin/sed"

#define EXEC_WITH_ERROR_THROW(cmd, res)   ({    \
    int ret = swss::exec(cmd, res);             \
    if (ret != 0)                               \
    {                                           \
        throw runtime_error(cmd + " : " + res); \
    }                                           \
})

#endif /* __SHELLCMD__ */
