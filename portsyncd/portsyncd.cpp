#include <getopt.h>
#include <iostream>
#include <fstream>
#include <sstream>
#include <vector>
#include <set>
#include <map>
#include <list>
#include "dbconnector.h"
#include "select.h"
#include "netdispatcher.h"
#include "netlink.h"
#include "producerstatetable.h"
#include "portsyncd/linksync.h"

#define DEFAULT_PORT_CONFIG_FILE     "port_config.ini"

using namespace std;
using namespace swss;

/*
 * This g_portSet contains all the front panel ports that the corresponding
 * host interfaces needed to be created. When this LinkSync class is
 * initialized, we check the database to see if some of the ports' host
 * interfaces are already created and remove them from this set. We will
 * remove the rest of the ports in the set when receiving the first netlink
 * message indicating that the host interfaces are created. After the set
 * is empty, we send out the signal ConfigDone. g_init is used to limit the
 * command to be run only once.
 */
set<string> g_portSet;
map<string, set<string>> g_vlanMap;
bool g_init = false;

void usage()
{
    cout << "Usage: portsyncd [-p port_config.ini]" << endl;
    cout << "       -p port_config.ini: MANDATORY import port lane mapping" << endl;
    cout << "                           default: port_config.ini" << endl;
}

void handlePortConfigFile(ProducerStateTable &p, string file);
void handleVlanIntfFile(string file);

int main(int argc, char **argv)
{
    Logger::linkToDbNative("portsyncd");
    int opt;
    string port_config_file = DEFAULT_PORT_CONFIG_FILE;

    while ((opt = getopt(argc, argv, "p:v:h")) != -1 )
    {
        switch (opt)
        {
        case 'p':
            port_config_file.assign(optarg);
            break;
        case 'h':
            usage();
            return 1;
        default: /* '?' */
            usage();
            return EXIT_FAILURE;
        }
    }

    DBConnector db(0, DBConnector::DEFAULT_UNIXSOCKET, 0);
    ProducerStateTable p(&db, APP_PORT_TABLE_NAME);

    LinkSync sync(&db);
    NetDispatcher::getInstance().registerMessageHandler(RTM_NEWLINK, &sync);
    NetDispatcher::getInstance().registerMessageHandler(RTM_DELLINK, &sync);

    try
    {
        NetLink netlink;
        Select s;

        netlink.registerGroup(RTNLGRP_LINK);
        cout << "Listen to link messages..." << endl;
        netlink.dumpRequest(RTM_GETLINK);

        handlePortConfigFile(p, port_config_file);

        s.addSelectable(&netlink);
        while (true)
        {
            Selectable *temps;
            int tempfd, ret;
            ret = s.select(&temps, &tempfd, 1);

            if (ret == Select::ERROR)
            {
                cerr << "Error had been returned in select" << endl;
                continue;
            }

            if (ret == Select::TIMEOUT)
            {
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

                    g_init = true;
                }
            }
        }
    }
    catch (const std::exception& e)
    {
        cerr << "Exception \"" << e.what() << "\" had been thrown in deamon" << endl;
        return EXIT_FAILURE;
    }

    return 1;
}

void handlePortConfigFile(ProducerStateTable &p, string file)
{
    cout << "Read port configuration file..." << endl;

    ifstream infile(file);
    if (!infile.is_open())
    {
        usage();
        throw "Port configuration file not found!";
    }

    list<string> header = {"name", "lanes", "alias", "speed", "autoneg", "fec"};
    string line;
    while (getline(infile, line))
    {
        if (line.at(0) == '#')
        {
            /* Find out what info is specified in the configuration file */
            for (auto it = header.begin(); it != header.end();)
            {
                if (line.find(*it) == string::npos)
                {
                    it = header.erase(it);
                }
                else
                {
                    ++it;
                }
            }

            continue;
         }
 
         istringstream iss(line);
        map<string, string> entry;
 
        /* Read port configuration entry */
        for (auto column : header)
         {
            iss >> entry[column];
         }

        /* If port has no alias, then use its name as alias */
        string alias;
        if ((entry.find("alias") != entry.end()) && (entry["alias"] != ""))
        {
            alias = entry["alias"];
        }
        else
        {
            alias = entry["name"];
        }

        FieldValueTuple lanes_attr("lanes", entry["lanes"]);
         FieldValueTuple alias_attr("alias", alias);
 
         vector<FieldValueTuple> attrs;
         attrs.push_back(lanes_attr);
         attrs.push_back(alias_attr);
 
        if ((entry.find("speed") != entry.end()) && (entry["speed"] != ""))
        {
            FieldValueTuple speed_attr("speed", entry["speed"]);
            attrs.push_back(speed_attr);
        }
 
        if ((entry.find("autoneg") != entry.end()) && (entry["autoneg"] != ""))
        {
            FieldValueTuple autoneg_attr("autoneg", entry["autoneg"]);
            attrs.push_back(autoneg_attr);
        }
 
        if ((entry.find("fec") != entry.end()) && (entry["fec"] != ""))
        {
            FieldValueTuple fec_attr("fec", entry["fec"]);
            attrs.push_back(fec_attr);
        }
 
        p.set(entry["name"], attrs);

        g_portSet.insert(entry["name"]);
     }
 
     infile.close();

    /* Notify that all ports added */
    FieldValueTuple finish_notice("count", to_string(g_portSet.size()));
    vector<FieldValueTuple> attrs = { finish_notice };
    p.set("PortConfigDone", attrs);
}

