#ifndef __CFGMGR__
#define __CFGMGR__

#include <iostream>
#include <functional>
#include <algorithm>
#include <unistd.h>
#include <net/if.h>
#include <cerrno>
#include <cstring>

extern int do_port(int argc, char **argv);
extern int do_lag(int argc, char **argv);
extern int do_intf(int argc, char **argv);
extern int do_vlan(int argc, char **argv);
extern int do_switch(int argc, char **argv);

#define NEXT_ARG() do { argv++; if (--argc <= 0) incomplete_command(); } while(0)
#define NEXT_ARG_OK() (argc - 1 > 0)
#define NEXT_ARG_FWD() do { argv++; argc--; } while(0)
#define PREV_ARG() do { argv--; argc++; } while(0)

extern void incomplete_command(void);
extern int matches(const char *cmd, const char *pattern);

#endif