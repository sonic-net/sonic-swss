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
#include "logger.h"
#include "dbconnector.h"
#include "redisclient.h"
#include "producerstatetable.h"
#include "exec.h"
#include "cfgmgr.h"
#include "vlancfgmgr.h"

using namespace std;
using namespace swss;

#define VLAN_PREFIX         "Vlan"

[[ noreturn ]] void usage(std::string program, int status, std::string message)
{
    if (message.size() != 0)
    {
        std::cout << message << std::endl << std::endl;
    }
    //add/del vlan,  add/del vlan member
    std::cout << "Usage:  " << program << " vlan { add | del } vlan VLAN_ID [ { down | up } ] \n"
              << "\t\t [ mtu MTU ] \n"
              << "\t\t [ unicast_miss_flood { true | false } ] \n"
              << "\t\t [ multicast_miss_flood { true | false } ] \n"
              << "\t\t [ broadcast_miss_flood { true | false } ] \n"
              << "\t\t [ desc DESCRIPTION] " << std::endl
              << "\t" << program << " vlan { add | del } vlan VLAN_ID dev DEV [ pvid ] [ untagged ]" << std::endl
              << "\t" << program << " vlan show [ dev DEV ] [ vlan VLAN_ID ]" << std::endl;

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
    short vid = -1;
    unsigned short flags = 0;
    unsigned int if_index;
    string key = VLAN_PREFIX;
    string admin = "up";
    unsigned int mtu = 1500; // for VLAN router interface.
    string unicast_miss_flood = "false";
    string multicast_miss_flood  = "false";
    string broadcast_miss_flood  = "false";
    string desc = "";

    while (argc > 0) {
        if (matches(*argv, "dev") == 0) {
            NEXT_ARG();
            dev = *argv;
        } else if (matches(*argv, "vlan") == 0) {
            NEXT_ARG();
            vid = atoi(*argv);
        }  else if (matches(*argv, "mtu") == 0) {
            NEXT_ARG();
            mtu = atoi(*argv);
        } else if (matches(*argv, "pvid") == 0) {
            flags |= BRIDGE_VLAN_INFO_PVID;
        } else if (matches(*argv, "untagged") == 0) {
            flags |= BRIDGE_VLAN_INFO_UNTAGGED;
        } else if (matches(*argv, "down") == 0) {
            admin = "down";
        }  else if (matches(*argv, "unicast_miss_flood") == 0) {
            NEXT_ARG();
            unicast_miss_flood = *argv;
        } else if (matches(*argv, "multicast_miss_flood") == 0) {
            NEXT_ARG();
            multicast_miss_flood = *argv;
        } else if (matches(*argv, "broadcast_miss_flood") == 0) {
            NEXT_ARG();
            broadcast_miss_flood = *argv;
        } else if (matches(*argv, "desc") == 0) {
            NEXT_ARG();
            desc = *argv;
        } else {
            if (matches(*argv, "help") == 0) {
                usage("cfgmgr", 0, "");
            }
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
            m_vlanTableProducer.del(key);
        }
        else
        {
            vector<FieldValueTuple> fvVector;
            FieldValueTuple a("admin_status", admin);
            fvVector.push_back(a);
            FieldValueTuple m("mtu", std::to_string(mtu));
            fvVector.push_back(m);
            FieldValueTuple o("autostate", "disabled");
            fvVector.push_back(o);

            FieldValueTuple uf("unicast_miss_flood", unicast_miss_flood);
            fvVector.push_back(uf);
            FieldValueTuple mf("multicast_miss_flood", multicast_miss_flood);
            fvVector.push_back(mf);
            FieldValueTuple bf("broadcast_miss_flood", broadcast_miss_flood);
            fvVector.push_back(bf);

            FieldValueTuple d("description", desc);
            fvVector.push_back(d);

            FieldValueTuple s("config_status_code", "unknown");
            fvVector.push_back(s);

            m_vlanTableProducer.set(key, fvVector);
        }
        return 0;
    }

    if_index = if_nametoindex(dev);
    if (if_index == 0) {
        cout << "Cannot find bridge device " << dev << " : " << std::strerror(errno) << endl;
        return -1;
    }

    key = key + ":" + dev;

    if( (flags & BRIDGE_VLAN_INFO_PVID) && !(flags & BRIDGE_VLAN_INFO_UNTAGGED ))
    {
        cout << "For Now: pvid should be set together with untagged mode!" << endl;
        return -1;
    }

    if (cmd == VlanCfgMgr::DELETE)
    {
        m_vlanMemberTableProducer.del(key);
    }
    else
    {
        vector<FieldValueTuple> fvVector;
        FieldValueTuple t("tagging_mode",
                (flags & BRIDGE_VLAN_INFO_UNTAGGED)? "untagged":"tagged");
        fvVector.push_back(t);
        FieldValueTuple s("config_status_code", "unknown");
        fvVector.push_back(s);

        m_vlanMemberTableProducer.set(key, fvVector);
    }
    return 0;
}

int VlanCfgMgr::vlan_show(int argc, char **argv)
{
    char *filter_dev = NULL;
    unsigned int if_index;
    string cmd = "bridge vlan show";
    string redis_cmd_db = "redis-cli -n ";
    string redis_cmd_keys = "\\*VLAN\\*";
    string redis_cmd;
    short vid = -1;

    while (argc > 0) {
        if (matches(*argv, "dev") == 0) {
            NEXT_ARG();
            filter_dev = *argv;
        } else if (matches(*argv, "vlan") == 0) {
            NEXT_ARG();
            vid = atoi(*argv);
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

    cout << "----Linux bridge vlan data----" << endl;
    cout << swss::exec(cmd.c_str()) << endl;
    return 0;
}

VlanCfgMgr::VlanCfgMgr(DBConnector *db) :
    m_vlanTableProducer(db, CFG_VLAN_TABLE_NAME),
    m_vlanTableConsumer(db, CFG_VLAN_TABLE_NAME),
    m_vlanMemberTableProducer(db, CFG_VLAN_MEMBER_TABLE_NAME),
    m_vlanMemberTableConsumer(db, CFG_VLAN_MEMBER_TABLE_NAME)
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
