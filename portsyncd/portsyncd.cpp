#include <getopt.h>
#include <iostream>
#include <fstream>
#include <sstream>
#include <vector>
#include <set>
#include <map>
#include <list>
#include <sys/stat.h>
#include <signal.h>
#include "dbconnector.h"
#include "select.h"
#include "netdispatcher.h"
#include "netlink.h"
#include "producerstatetable.h"
#include "portsyncd/linksync.h"
#include "subscriberstatetable.h"
#include "exec.h"
#include "warm_restart.h"

using namespace std;
using namespace swss;

#define DEFAULT_SELECT_TIMEOUT 1000 /* ms */

bool gExit = false;
/*
 * This g_portSet contains all the front panel ports that the corresponding
 * host interfaces needed to be created. When this LinkSync class is
 * initialized, we check the database to see if some of the ports' host
 * interfaces are already created and remove them from this set. We will
 * remove the rest of the ports in the set when receiving the first netlink
 * message indicating that the host interfaces are created. After the set
 * is empty, we send out the signal PortInitDone. g_init is used to limit the
 * command to be run only once.
 */
set<string> g_portSet;
bool g_init = false;

void usage()
{
    cout << "Usage: portsyncd" << endl;
    cout << "       port lane mapping is from configDB" << endl;
    cout << "       this program will exit if configDB does not contain that info" << endl;
}

void handlePortConfigFile(ProducerStateTable &p, string file, bool warm);
void handlePortConfigFromConfigDB(ProducerStateTable &p, DBConnector &cfgDb, bool warm);
void handleVlanIntfFile(string file);
void handlePortConfig(ProducerStateTable &p, map<string, KeyOpFieldsValuesTuple> &port_cfg_map);
void checkPortInitDone(DBConnector *appl_db);

void sigterm_handler(int signo)
{
    gExit = true;
}

int main(int argc, char **argv)
{
    if (signal(SIGTERM, sigterm_handler) == SIG_ERR)
    {
        SWSS_LOG_ERROR("failed to setup SIGTERM action");
        exit(1);
    }

    Logger::linkToDbNative("portsyncd");
    int opt;
    map<string, KeyOpFieldsValuesTuple> port_cfg_map;

    while ((opt = getopt(argc, argv, "v:h")) != -1 )
    {
        switch (opt)
        {
        case 'h':
            usage();
            return 1;
        default: /* '?' */
            usage();
            return EXIT_FAILURE;
        }
    }

    DBConnector cfgDb("CONFIG_DB", 0);
    DBConnector appl_db("APPL_DB", 0);
    DBConnector state_db("STATE_DB", 0);
    ProducerStateTable p(&appl_db, APP_PORT_TABLE_NAME);
    SubscriberStateTable portCfg(&cfgDb, CFG_PORT_TABLE_NAME);

    WarmStart::initialize("portsyncd", "swss");
    WarmStart::checkWarmStart("portsyncd", "swss");
    const bool warm = WarmStart::isWarmStart();

    try
    {
        NetLink netlink;
        Select s;

        netlink.registerGroup(RTNLGRP_LINK);
        netlink.dumpRequest(RTM_GETLINK);
        cout << "Listen to link messages..." << endl;

        handlePortConfigFromConfigDB(p, cfgDb, warm);

        LinkSync sync(&appl_db, &state_db);
        NetDispatcher::getInstance().registerMessageHandler(RTM_NEWLINK, &sync);
        NetDispatcher::getInstance().registerMessageHandler(RTM_DELLINK, &sync);

        s.addSelectable(&netlink);
        s.addSelectable(&portCfg);

        while (!gExit)
        {
            Selectable *temps;
            int ret;
            ret = s.select(&temps, DEFAULT_SELECT_TIMEOUT);

            if (ret == Select::ERROR)
            {
                cerr << "Error had been returned in select" << endl;
                continue;
            }
            else if (ret == Select::TIMEOUT)
            {
                continue;
            }
            else if (ret != Select::OBJECT)
            {
                SWSS_LOG_ERROR("Unknown return value from Select %d", ret);
                continue;
            }

            if (temps == static_cast<Selectable*>(&netlink))
            {
                /* on netlink message, check if PortInitDone should be sent out */
                if (!g_init && g_portSet.empty())
                {
                    /*
                     * After finishing reading port configuration file and
                     * creating all host interfaces, this daemon shall send
                     * out a signal to orchagent indicating port initialization
                     * procedure is done and other application could start
                     * syncing.
                     */
                    FieldValueTuple finish_notice("lanes", "0");
                    vector<FieldValueTuple> attrs = { finish_notice };
                    p.set("PortInitDone", attrs);
                    SWSS_LOG_NOTICE("PortInitDone");

                    g_init = true;
                }
                if (!port_cfg_map.empty())
                {
                    handlePortConfig(p, port_cfg_map);
                }
            }
            else if (temps == (Selectable *)&portCfg)
            {
                std::deque<KeyOpFieldsValuesTuple> entries;
                portCfg.pops(entries);

                for (auto entry: entries)
                {
                    string key = kfvKey(entry);

                    if (port_cfg_map.find(key) != port_cfg_map.end())
                    {
                        /* For now we simply drop previous pending port config */
                        port_cfg_map.erase(key);
                    }
                    port_cfg_map[key] = entry;
                }
                handlePortConfig(p, port_cfg_map);
            }
            else
            {
                SWSS_LOG_ERROR("Unknown object returned by select");
                continue;
            }
        }
    }
    catch (const std::exception& e)
    {
        cerr << "Exception \"" << e.what() << "\" was thrown in daemon" << endl;
        return EXIT_FAILURE;
    }
    catch (...)
    {
        cerr << "Exception was thrown in daemon" << endl;
        return EXIT_FAILURE;
    }

    return 0;
}

static void notifyPortConfigDone(ProducerStateTable &p)
{
    /* Notify that all ports added */
    FieldValueTuple finish_notice("count", to_string(g_portSet.size()));
    vector<FieldValueTuple> attrs = { finish_notice };
    p.set("PortConfigDone", attrs);
}

void handlePortConfigFromConfigDB(ProducerStateTable &p, DBConnector &cfgDb, bool warm)
{
    SWSS_LOG_ENTER();

    SWSS_LOG_NOTICE("Getting port configuration from ConfigDB...");

    Table table(&cfgDb, CFG_PORT_TABLE_NAME);
    std::vector<FieldValueTuple> ovalues;
    std::vector<string> keys;
    table.getKeys(keys);

    if (keys.empty())
    {
        SWSS_LOG_NOTICE("ConfigDB does not have port information, "
                        "however ports can be added later on, continuing...");
    }

    for ( auto &k : keys )
    {
        table.get(k, ovalues);
        vector<FieldValueTuple> attrs;
        for ( auto &v : ovalues )
        {
            FieldValueTuple attr(v.first, v.second);
            attrs.push_back(attr);
        }
        if (!warm)
        {
            p.set(k, attrs);
        }
        g_portSet.insert(k);
    }
    if (!warm)
    {
        notifyPortConfigDone(p);
    }

}

void handlePortConfig(ProducerStateTable &p, map<string, KeyOpFieldsValuesTuple> &port_cfg_map)
{
    string autoneg;
    vector<FieldValueTuple> attrs;
    vector<FieldValueTuple> autoneg_attrs;
    vector<FieldValueTuple> force_attrs;

    auto it = port_cfg_map.begin();
    while (it != port_cfg_map.end())
    {
        KeyOpFieldsValuesTuple entry = it->second;
        string key = kfvKey(entry);
        string op  = kfvOp(entry);
        auto values = kfvFieldsValues(entry);

        /* only push down port config when port is not in hostif create pending state */
        if (g_portSet.find(key) == g_portSet.end())
        {
            /* No support for port delete yet */
            if (op == SET_COMMAND)
            {

                for (auto i : values)
                {
                    auto field = fvField(i);
                    if (field == "adv_speeds")
                    {
                        autoneg_attrs.push_back(i);
                    }
                    else if (field == "adv_interface_types")
                    {
                        autoneg_attrs.push_back(i);
                    }
                    else if (field == "speed")
                    {
                        force_attrs.push_back(i);
                    }
                    else if (field == "interface_type")
                    {
                        force_attrs.push_back(i);
                    }
                    else if (field == "autoneg")
                    {
                        autoneg = fvValue(i);
                        attrs.push_back(i);
                    }
                    else
                    {
                        attrs.push_back(i);
                    }
                }
                if (autoneg == "on") // autoneg is on, only put adv_speeds and adv_interface_types to APPL_DB
                {
                    attrs.insert(attrs.end(), autoneg_attrs.begin(), autoneg_attrs.end());
                }
                else if (autoneg == "off") // autoneg is off, only put speed and interface_type to APPL_DB
                {
                    attrs.insert(attrs.end(), force_attrs.begin(), force_attrs.end());
                }
                else // autoneg is not configured, put all attributes to APPL_DB
                {
                    attrs.insert(attrs.end(), autoneg_attrs.begin(), autoneg_attrs.end());
                    attrs.insert(attrs.end(), force_attrs.begin(), force_attrs.end());
                }
                p.set(key, attrs);
                attrs.clear();
                autoneg_attrs.clear();
                force_attrs.clear();
                autoneg.clear();
            }

            it = port_cfg_map.erase(it);
        }
        else
        {
            it++;
        }
    }
}
