#include <iostream>
#include <iomanip>
#include <vector>
#include <functional>
#include <algorithm>
#include <unistd.h>
#include <net/if.h>
#include <cerrno>
#include <cstring>
#include "schema.h"
#include "exec.h"
#include "cfgmgr.h"
#include "intfcfgmgr.h"

using namespace std;
using namespace swss;

[[ noreturn ]] static void usage(std::string program, int status, std::string message)
{
    if (message.size() != 0)
    {
        std::cout << message << std::endl << std::endl;
    }
    std::cout << "Usage:  " << program << " intf { add | del } PREFIX  dev IFNAME\n" << std::endl
              << "\t" << program << " intf show [ dev IFNAME ]" << std::endl;

    exit(status);
}

int IntfCfgMgr::intf_modify(Operation cmd, int argc, char **argv)
{
    char *dev = NULL;
    string key;
    string prefix;
    string scope = "global";
    string family = IPV4_NAME;

    if (argc <= 0) {
    	usage("cfgmgr", -1, "Invalid option");
    }

    // TODO: more validity check on prefix.
    prefix = *argv;

    NEXT_ARG();
    while (argc > 0) {
        if (matches(*argv, "dev") == 0) {
            NEXT_ARG();
            dev = *argv;
        } else {
            if (matches(*argv, "help") == 0) {
                usage("cfgmgr", 0, "");
            }
        }
        argc--; argv++;
    }

    if (dev == NULL) {
        cout << "dev IFNAME is required arguments" << endl;
        usage("cfgmgr", -1, "Invalid option");
    }

    if (if_nametoindex(dev) == 0) {
        cout << "Cannot find device " << dev << " : " << std::strerror(errno) << endl;
        return -1;
    }

    key = dev;
    key += ":";
    key += prefix;

    if (cmd == IntfCfgMgr::DELETE)
    {
        m_intfTableProducer.del(key);
    }
    else
    {
	    std::vector<FieldValueTuple> fvVector;
	    FieldValueTuple f("family", family);
	    FieldValueTuple s("scope", scope);
	    fvVector.push_back(s);
	    fvVector.push_back(f);
	    m_intfTableProducer.set(key, fvVector);
	}
    return 0;
}

int IntfCfgMgr::intf_show(int argc, char **argv)
{
    char *filter_dev = NULL;
    unsigned int if_index;
    string cmd = "ip address show ";
    string redis_cmd_db = "redis-cli -n ";
    string redis_cmd_keys = "\\*INTF\\*";
    string redis_cmd;
    short vid = -1;

    while (argc > 0) {
        if (matches(*argv, "dev") == 0) {
            NEXT_ARG();
            filter_dev = *argv;
        }
        argc--; argv++;
    }

    redis_cmd_db += std::to_string(CONFIG_DB) + " ";

    if (vid >= 0) {
        redis_cmd_keys += std::to_string(vid) + "\\*";
    }

    if (filter_dev) {
        if_index = if_nametoindex(filter_dev);
        if (if_index == 0) {
            cout << "Cannot find bridge device " << filter_dev << " : " << std::strerror(errno) << endl;
            return -1;
        }
        cmd += " dev ";
        cmd += filter_dev;

        redis_cmd_keys += filter_dev;
        redis_cmd_keys += "\\*";
    }
    redis_cmd = redis_cmd_db + " KEYS " + redis_cmd_keys;
    redis_cmd += " | xargs -n 1  -I %   sh -c 'echo %; ";
    redis_cmd += redis_cmd_db + "hgetall %; echo'";

    cout << "-----Redis CFGDB data---" << endl;
    cout << redis_cmd <<endl;
    cout << swss::exec(redis_cmd.c_str()) << endl;

    cout << "----Linux hostenv data----" << endl;
    cout << swss::exec(cmd.c_str()) << endl;
    return 0;
}

IntfCfgMgr::IntfCfgMgr(DBConnector *db) :
    m_intfTableProducer(db, CFG_INTF_TABLE_NAME),
    m_intfTableConsumer(db, CFG_INTF_TABLE_NAME)
{

}

int do_intf(int argc, char **argv)
{
    auto exitWithUsage = std::bind(usage, "cfgmgr", std::placeholders::_1, std::placeholders::_2);

    DBConnector db(CONFIG_DB, DBConnector::DEFAULT_UNIXSOCKET, 0);
    IntfCfgMgr cfgmgr(&db);

    if (argc > 0)
    {
        if (matches(*argv, "add") == 0)
            return cfgmgr.intf_modify(IntfCfgMgr::ADD, argc-1, argv+1);
        if (matches(*argv, "delete") == 0)
            return cfgmgr.intf_modify(IntfCfgMgr::DELETE, argc-1, argv+1);
        if (matches(*argv, "show") == 0)
            return cfgmgr.intf_show(argc-1, argv+1);
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