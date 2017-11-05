#include <iostream>
#include <fstream>
#include <sstream>
#include <vector>
#include <unistd.h>
#include <stdlib.h>
#include <net/if.h>
#include <cerrno>
#include <cstring>
#include "schema.h"
#include "redisclient.h"
#include "exec.h"
#include "cfgmgr.h"
#include "vlancfgmgr.h"
#include "orch.h"
#include "shellcmd.h"

using namespace std;
using namespace swss;

#define VLAN_PREFIX         "Vlan"

[[ noreturn ]] static void usage(std::string program, int status, std::string message)
{
    if (message.size() != 0)
    {
        std::cout << message << std::endl << std::endl;
    }
    //add/del vlan,  add/del vlan member
    std::cout << "Usage:  " << program << " vlan { add | del } vlan VLAN_ID [ { down | up } ] \n"
              << "\t\t [ mtu MTU ] \n"
              << "\t\t [ desc DESCRIPTION] " << std::endl
              << "\t" << program << " vlan { add | del } vlan VLAN_ID dev DEV [ untagged ]" << std::endl
              << "\t" << program << " vlan show <config | state> [ dev DEV ] [ vlan VLAN_ID ]" << std::endl;

    exit(status);
}


#define BRIDGE_VLAN_INFO_MASTER     (1<<0)  /* Operate on Bridge device as well */
#define BRIDGE_VLAN_INFO_PVID       (1<<1)  /* VLAN is PVID, ingress untagged */
#define BRIDGE_VLAN_INFO_UNTAGGED   (1<<2)  /* VLAN egresses untagged */
#define BRIDGE_VLAN_INFO_RANGE_BEGIN    (1<<3) /* VLAN is start of vlan range */
#define BRIDGE_VLAN_INFO_RANGE_END  (1<<4) /* VLAN is end of vlan range */
#define BRIDGE_VLAN_INFO_BRENTRY    (1<<5) /* Global bridge VLAN entry */

int VlanCfgMgr::vlan_modify(Operation cmd, int argc, char **argv)
{
    char *dev = NULL;
    int vid = -1;
    unsigned short flags = 0;
    unsigned int if_index;
    string key = VLAN_PREFIX;
    string admin = "up";
    unsigned int mtu = 9100; // for VLAN host interface.
    string desc = "";

    while (argc > 0) {
        if (matches(*argv, "dev") == 0) {
            NEXT_ARG();
            dev = *argv;
        }
        else if (matches(*argv, "vlan") == 0) {
            NEXT_ARG();
            vid = atoi(*argv);
        }
        else if (matches(*argv, "mtu") == 0) {
            NEXT_ARG();
            mtu = atoi(*argv);
        }
        else if (matches(*argv, "untagged") == 0) {
            flags |= BRIDGE_VLAN_INFO_UNTAGGED;
        }
        else if (matches(*argv, "down") == 0) {
            admin = "down";
        }
        else if (matches(*argv, "desc") == 0) {
            NEXT_ARG();
            desc = *argv;
        }
        else if (matches(*argv, "help") == 0) {
            usage("cfgmgr", 0, "");
        }
        argc--; argv++;
    }

    if (vid == -1) {
        cout << "VLAN ID are required arguments" << endl;
        return -1;
    }

    if (vid >= 4096) {
        cout << "Invalid VLAN ID "<< vid << endl;
        return -1;
    }

    key += std::to_string(vid);
    if (dev == NULL)
    {
        if (cmd == VlanCfgMgr::DELETE)
        {
            m_cfgVlanTable.del(key);
        }
        else
        {
            vector<FieldValueTuple> fvVector;
            FieldValueTuple a("admin_status", admin);
            fvVector.push_back(a);
            FieldValueTuple m("mtu", std::to_string(mtu));
            fvVector.push_back(m);
            FieldValueTuple d("description", desc);
            fvVector.push_back(d);

            m_cfgVlanTable.set(key, fvVector);
        }
        return 0;
    }

    if_index = if_nametoindex(dev);
    if (if_index == 0) {
        cout << "Cannot find bridge device " << dev << " : " << std::strerror(errno) << endl;
        return -1;
    }

    key = key + CONFIGDB_KEY_SEPARATOR + dev;

    if (cmd == VlanCfgMgr::DELETE)
    {
        m_cfgVlanMemberTable.del(key);
    }
    else
    {
        vector<FieldValueTuple> fvVector;
        FieldValueTuple t("tagging_mode",
                (flags & BRIDGE_VLAN_INFO_UNTAGGED)? "untagged":"tagged");
        fvVector.push_back(t);

        m_cfgVlanMemberTable.set(key, fvVector);
    }
    return 0;
}

void VlanCfgMgr::vlan_redis_show(string &redis_cmd_keys, int vid, string filter_dev)
{
    stringstream redis_cmd_db, redis_cmd;
    unsigned int if_index;
    string res;

    redis_cmd_db << REDIS_CLI_CMD << " -n " << CONFIG_DB << " ";

    if (vid >= 0) {
        redis_cmd_keys += std::to_string(vid) + "\\*";
    }

    if (!filter_dev.empty()) {
        if_index = if_nametoindex(filter_dev.c_str());
        if (if_index == 0) {
            cout << "Cannot find device " << filter_dev << " : " << std::strerror(errno) << endl;
            return;
        }

        redis_cmd_keys += filter_dev;
        redis_cmd_keys += "\\*";
    }

    redis_cmd << redis_cmd_db.str() << " KEYS " << redis_cmd_keys
              << " | " << XARGS_CMD << " -n 1  -I %   sh -c 'echo \"%\"; "
              << redis_cmd_db.str() << "hgetall \"%\" | " << PASTE_CMD
              <<  " -d '='  - - | " << SED_CMD << " 's/^/$/'; echo'";

    //cout << redis_cmd <<endl;
    EXEC_WITH_ERROR_THROW(redis_cmd.str(), res);
    cout << res;
}

int VlanCfgMgr::vlan_show(int argc, char **argv)
{
    string filter_dev;
    unsigned int if_index;
    string cmd = "bridge vlan show";
    string redis_cmd_vlan_keys = "\\*VLAN\\|\\*";
    string redis_cmd_vlan_member_keys = "\\*VLAN_MEMBER\\|\\*";
    string res;
    ShowOpEnum show_op = SHOW_NONE;
    int vid = -1;

    while (argc > 0) {
        if (matches(*argv, "dev") == 0) {
            NEXT_ARG();
            filter_dev = *argv;
        }
        else if (matches(*argv, "vlan") == 0) {
            NEXT_ARG();
            vid = atoi(*argv);
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

    if (show_op == SHOW_CONFIG)
    {
        cout << "-----Redis ConfigDB data---" << endl;
        vlan_redis_show(redis_cmd_vlan_keys, vid, filter_dev);
        vlan_redis_show(redis_cmd_vlan_member_keys, vid, filter_dev);
        return 0;
    }

    cout << "----Linux hostenv data----" << endl;
    if (!filter_dev.empty()) {
        if_index = if_nametoindex(filter_dev.c_str());
        if (if_index == 0) {
            cout << "Cannot find bridge device " << filter_dev << " : " << std::strerror(errno) << endl;
            return -1;
        }
        cmd += " dev ";
        cmd += filter_dev;
    }
    EXEC_WITH_ERROR_THROW(cmd, res);
    cout << res << endl;
    return 0;
}

VlanCfgMgr::VlanCfgMgr(DBConnector *db) :
    m_cfgVlanTable(db, CFG_VLAN_TABLE_NAME, CONFIGDB_TABLE_NAME_SEPARATOR),
    m_cfgVlanMemberTable(db, CFG_VLAN_MEMBER_TABLE_NAME, CONFIGDB_TABLE_NAME_SEPARATOR)
{

}

int do_vlan(int argc, char **argv)
{
    auto exitWithUsage = std::bind(usage, "cfgmgr", std::placeholders::_1, std::placeholders::_2);

    DBConnector db(CONFIG_DB, DBConnector::DEFAULT_UNIXSOCKET, 0);
    VlanCfgMgr cfgmgr(&db);

    if (argc > 0)
    {
        if (matches(*argv, "add") == 0)
            return cfgmgr.vlan_modify(VlanCfgMgr::ADD, argc-1, argv+1);
        if (matches(*argv, "delete") == 0)
            return cfgmgr.vlan_modify(VlanCfgMgr::DELETE, argc-1, argv+1);
        if (matches(*argv, "show") == 0)
            return cfgmgr.vlan_show(argc-1, argv+1);
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
