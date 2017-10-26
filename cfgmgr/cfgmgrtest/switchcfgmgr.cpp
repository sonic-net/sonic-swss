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
#include "macaddress.h"
#include "cfgmgr.h"
#include "switchcfgmgr.h"
#include "sai.h"
#include "shellcmd.h"

using namespace std;
using namespace swss;

#define CFG_SWITCH_ATTR_NAME "SWITCH_ATTR"

const map<string, sai_switch_attr_t> switch_attribute_map =
{
    {"switch_mac",                          SAI_SWITCH_ATTR_SRC_MAC_ADDRESS},
    {"fdb_unicast_miss_packet_action",      SAI_SWITCH_ATTR_FDB_UNICAST_MISS_PACKET_ACTION},
    {"fdb_broadcast_miss_packet_action",    SAI_SWITCH_ATTR_FDB_BROADCAST_MISS_PACKET_ACTION},
    {"fdb_multicast_miss_packet_action",    SAI_SWITCH_ATTR_FDB_MULTICAST_MISS_PACKET_ACTION},
    {"ecmp_hash_seed",                      SAI_SWITCH_ATTR_ECMP_DEFAULT_HASH_SEED},
    {"lag_hash_seed",                       SAI_SWITCH_ATTR_LAG_DEFAULT_HASH_SEED}
};

const map<string, sai_packet_action_t> packet_action_map =
{
    {"drop",    SAI_PACKET_ACTION_DROP},
    {"forward", SAI_PACKET_ACTION_FORWARD},
    {"trap",    SAI_PACKET_ACTION_TRAP}
};


[[ noreturn ]] static void usage(std::string program, int status, std::string message)
{
    if (message.size() != 0)
    {
        std::cout << message << std::endl << std::endl;
    }
    std::cout << "Usage:  " << program << " switch update\n"
              << "\t\t [ switch_mac             {xx:xx:xx:xx:xx:xx}       ] \n"
              << "\t\t [ unicast_miss_flood     { forward | drop | trap } ] \n"
              << "\t\t [ multicast_miss_flood   { forward | drop | trap } ] \n"
              << "\t\t [ broadcast_flood        { forward | drop | trap } ] \n"
              << "\t" << program << " switch show <config | state>  [ dev DEV ]" << std::endl;

    exit(status);
}

static void get_packet_action(string &attr, string action)
{
    if (packet_action_map.find(action) == packet_action_map.end())
    {
        cout << "Unsupported packet action: " << action << std::endl;
        usage("cfgmgr", 0, "");
    }
    attr = action;
}

int SwitchCfgMgr::switch_update(int argc, char **argv)
{
    string key;
    string unicast_miss_flood = "forward";
    string multicast_miss_flood  = "forward";
    string broadcast_flood  = "forward";
    MacAddress mac;

    if (argc <= 0) {
        usage("cfgmgr", -1, "Invalid option");
    }

    while (argc > 0) {
        if (matches(*argv, "switch_mac") == 0) {
            NEXT_ARG();
            mac = MacAddress(*argv);
            if (!mac)
            {
                cout << "invalid mac format: " << *argv << endl;
                usage("cfgmgr", 0, "");
            }
        }
        else if (matches(*argv, "unicast_miss_flood") == 0) {
            NEXT_ARG();
            get_packet_action(unicast_miss_flood , *argv);
        }
        else if (matches(*argv, "multicast_miss_flood") == 0) {
            NEXT_ARG();
            get_packet_action(multicast_miss_flood , *argv);
        }
        else if (matches(*argv, "broadcast_flood") == 0) {
            NEXT_ARG();
            get_packet_action(broadcast_flood , *argv);
        }
        else if (matches(*argv, "help") == 0) {
                usage("cfgmgr", 0, "");
        }
        argc--; argv++;
    }

    key = CFG_SWITCH_ATTR_NAME;

    std::vector<FieldValueTuple> fvVector;
    if (mac)
    {
        FieldValueTuple m("switch_mac", mac.to_string());
        fvVector.push_back(m);
    }
    FieldValueTuple u("fdb_unicast_miss_packet_action", unicast_miss_flood);
    FieldValueTuple m("fdb_multicast_miss_packet_action", multicast_miss_flood);
    FieldValueTuple b("fdb_broadcast_miss_packet_action", broadcast_flood);
    fvVector.push_back(u);
    fvVector.push_back(m);
    fvVector.push_back(b);
    m_cfgSwitchTable.set(key, fvVector);

    return 0;
}

int SwitchCfgMgr::switch_show(int argc, char **argv)
{
    char *filter_dev = NULL;
    unsigned int if_index;
    string redis_cmd_db = "redis-cli -n ";
    string redis_cmd_keys = "\\*SWITCH\\|\\*";
    string redis_cmd;
    ShowOpEnum show_op = SHOW_NONE;
    string cmd ="ls /sys/class/net/Bridge/brif/*/*flood \
        | xargs -n 1 -I % sh -c 'echo -n  \"%\" \"    :    \"; cat \"%\";'";

    while (argc > 0) {
        if (matches(*argv, "dev") == 0) {
            NEXT_ARG();
            filter_dev = *argv;
        }
        else if (matches(*argv, "config") == 0) {
            show_op = SHOW_CONFIG;
        }
        else if (matches(*argv, "state") == 0) {
            show_op = SHOW_STATE;
        }

        argc--; argv++;
    }

    if (show_op == SHOW_NONE)
    {
        usage("cfgmgr", 0, "");
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
    }

    redis_cmd = redis_cmd_db + " KEYS " + redis_cmd_keys;
    redis_cmd += " | xargs -n 1  -I %   sh -c 'echo \"%\"; ";
    redis_cmd += redis_cmd_db + "hgetall \"%\" | paste -d '='  - - | sed  's/^/$/'; echo'";

    string res;
    if (show_op == SHOW_CONFIG)
    {
        cout << "-----Redis ConfigDB data---" << endl;
        //cout << redis_cmd <<endl;
        EXEC_WITH_ERROR_THROW(redis_cmd, res);
        cout << res;
        return 0;
    }

    cout << "----Linux hostenv data----" << endl;
    struct stat sb;
    if (stat("/sys/class/net/Bridge", &sb) || !S_ISDIR(sb.st_mode))
    {
       cout << "Path: /sys/class/net/Bridge doens't exist" << endl;
       return -1;
    }
    EXEC_WITH_ERROR_THROW(cmd, res);
    cout << res << endl;

    return 0;
}

SwitchCfgMgr::SwitchCfgMgr(DBConnector *db) :
    m_cfgSwitchTable(db, CFG_SWITCH_TABLE_NAME, CONFIGDB_TABLE_NAME_SEPARATOR)
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
