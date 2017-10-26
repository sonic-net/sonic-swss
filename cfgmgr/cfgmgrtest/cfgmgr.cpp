#include "cfgmgr.h"

using namespace std;

[[ noreturn ]] void usage()
{
    std::cout << "Usage: cfgmgr OBJECT { COMMAND | help }" << std::endl
              << "\t  where  OBJECT := { vlan | intf | switch }\n" << std::endl;
    exit(0);
}

void incomplete_command(void)
{
    std::cout << "Command line is not complete. Try option \"help\" " << std::endl;
    exit(-1);
}

int matches(const char *cmd, const char *pattern)
{
    size_t len = strlen(cmd);

    if (len > strlen(pattern))
        return -1;
    return memcmp(pattern, cmd, len);
}

static int do_help(int argc, char **argv)
{
    usage();
}

static const struct cmd {
    const char *cmd;
    int (*func)(int argc, char **argv);
} cmds[] = {
    { "intf",   do_intf },
    { "vlan",   do_vlan },
    { "switch", do_switch },
    { "help",   do_help },
    { NULL,        NULL }
};

static int do_cmd(const char *argv0, int argc, char **argv)
{
    const struct cmd *c;

    for (c = cmds; c->cmd; ++c) {
        if (matches(argv0, c->cmd) == 0)
            return c->func(argc-1, argv+1);
    }

    cout << "Object " << argv0 << " is unknown, try \"cfgmgr help\".\n" << std::endl;
    return -1;
}

int main(int argc, char **argv)
{
    if (argc > 1)
        return do_cmd(argv[1], argc-1, argv+1);

    usage();
}
