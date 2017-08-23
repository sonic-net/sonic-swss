#include <iostream>
#include <iomanip>
#include <vector>
#include <functional>
#include <algorithm>
#include <unistd.h>
#include <net/if.h>
#include <sys/stat.h>
#include <cerrno>
#include <cstring>
#include "schema.h"
#include "exec.h"
#include "cfgmgr.h"
#include "switchcfgmgr.h"

using namespace std;
using namespace swss;

#define CFG_SWITCH_FLOOD_CONTROL_KEY_NAME "FLOOD_CONTROL"

[[ noreturn ]] static void usage(std::string program, int status, std::string message)
{
    if (message.size() != 0)
    {
        std::cout << message << std::endl << std::endl;
    }
    std::cout << "Usage:  " << program << " switch update\n" << std::endl
              << "\t\t [ unicast_miss_flood     { true | false } ] \n"
              << "\t\t [ multicast_miss_flood   { true | false } ] \n"
              << "\t\t [ broadcast_flood        { true | false } ] \n"
              << "\t" << program << " switch show  [ dev DEV ]" << std::endl;

    exit(status);
}

int SwitchCfgMgr::switch_update(int argc, char **argv)
{
    string key;
    string unicast_miss_flood = "true";
    string multicast_miss_flood  = "true";
    string broadcast_flood  = "true";

    if (argc <= 0) {
    	usage("cfgmgr", -1, "Invalid option");
    }

    while (argc > 0) {
        if (matches(*argv, "unicast_miss_flood") == 0) {
            NEXT_ARG();
            unicast_miss_flood = *argv;
        } else if (matches(*argv, "multicast_miss_flood") == 0) {
            NEXT_ARG();
            multicast_miss_flood = *argv;
        } else if (matches(*argv, "broadcast_flood") == 0) {
            NEXT_ARG();
            broadcast_flood = *argv;
        } else {
            if (matches(*argv, "help") == 0) {
                usage("cfgmgr", 0, "");
            }
        }
        argc--; argv++;
    }

    key = CFG_SWITCH_FLOOD_CONTROL_KEY_NAME;

    std::vector<FieldValueTuple> fvVector;
    FieldValueTuple u("unicast_miss_flood", unicast_miss_flood);
    FieldValueTuple m("multicast_miss_flood", multicast_miss_flood);
    FieldValueTuple b("broadcast_flood", broadcast_flood);
    fvVector.push_back(u);
    fvVector.push_back(m);
    fvVector.push_back(b);
	m_switchTableProducer.set(key, fvVector);

    return 0;
}

int SwitchCfgMgr::switch_show(int argc, char **argv)
{
    char *filter_dev = NULL;
    unsigned int if_index;
    string redis_cmd_db = "redis-cli -n ";
    string redis_cmd_keys = "\\*SWITCH\\*";
    string redis_cmd;
    string cmd ="ls /sys/class/net/Bridge/brif/*/*flood \
        | xargs -n 1 -I % sh -c 'echo -n  % \"    :    \"; cat %;'";

    while (argc > 0) {
        if (matches(*argv, "dev") == 0) {
            NEXT_ARG();
            filter_dev = *argv;
        }
        argc--; argv++;
    }

    redis_cmd_db += std::to_string(CONFIG_DB) + " ";

    if (filter_dev) {
        if_index = if_nametoindex(filter_dev);
        if (if_index == 0) {
            cout << "Cannot find bridge device " << filter_dev << " : " << std::strerror(errno) << endl;
            return -1;
        }
        cmd += " | grep ";
        cmd += filter_dev;

     //   redis_cmd_keys += filter_dev;
     //   redis_cmd_keys += "\\*";
    }

    redis_cmd = redis_cmd_db + " KEYS " + redis_cmd_keys;
    redis_cmd += " | xargs -n 1  -I %   sh -c 'echo %; ";
    redis_cmd += redis_cmd_db + "hgetall %'";

    cout << "-----Redis CFGDB data---" << endl;
    cout << redis_cmd <<endl;
    cout << swss::exec(redis_cmd.c_str()) << endl;

    cout << "----Linux hostenv data----" << endl;
    struct stat sb;
    if (stat("/sys/class/net/Bridge/brif", &sb) || !S_ISDIR(sb.st_mode))
    {
       cout << "Path: /sys/class/net/Bridge/brif doens't exist" << endl;
       return -1;
    }
    cout << swss::exec(cmd.c_str()) << endl;

    return 0;
}

SwitchCfgMgr::SwitchCfgMgr(DBConnector *db) :
    m_switchTableProducer(db, CFG_SWITCH_TABLE_NAME),
    m_switchTableConsumer(db, CFG_SWITCH_TABLE_NAME)
{

}

int do_switch(int argc, char **argv)
{
    auto exitWithUsage = std::bind(usage, "cfgmgr", std::placeholders::_1, std::placeholders::_2);

    DBConnector db(CONFIG_DB, DBConnector::DEFAULT_UNIXSOCKET, 0);
    SwitchCfgMgr cfgmgr(&db);

    if (argc > 0)
    {
        if (matches(*argv, "update") == 0)
            return cfgmgr.switch_update(argc-1, argv+1);
        if (matches(*argv, "show") == 0)
            return cfgmgr.switch_show(argc-1, argv+1);
        if (matches(*argv, "help") == 0)
            exitWithUsage(EXIT_SUCCESS, "");
        else
            exitWithUsage(EXIT_FAILURE, "Invalid option");
    }
    else {
        exitWithUsage(EXIT_FAILURE, "Invalid option");
    }

    return EXIT_SUCCESS;
}