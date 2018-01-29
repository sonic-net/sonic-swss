#ifndef __SHELLCMD__
#define __SHELLCMD__

#define IP_CMD               "/sbin/ip"
#define BRIDGE_CMD           "/sbin/bridge"
#define ECHO_CMD             "/bin/echo"
#define BASH_CMD             "/bin/bash"
#define GREP_CMD             "/bin/grep"

#define EXEC_WITH_ERROR_THROW(cmd, res)   ({    \
    int ret = swss::exec(cmd, res);             \
    if (ret != 0)                               \
    {                                           \
        throw runtime_error(cmd + " : " + res); \
    }                                           \
})

#endif /* __SHELLCMD__ */
