#include <getopt.h>
#include <iostream>
#include <fstream>
#include <sstream>
#include <vector>
#include <set>
#include <map>
#include <list>
#include <sys/stat.h>
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
    cout << "Usage: portsyncd [-p port_config.ini]" << endl;
    cout << "       -p port_config.ini: import port lane mapping" << endl;
    cout << "                           use configDB data if not specified" << endl;
}

void handlePortConfigFile(ProducerStateTable &p, string file);
void handlePortConfigFromConfigDB(ProducerStateTable &p, DBConnector &cfgDb);
void handleVlanIntfFile(string file);
void handlePortConfig(ProducerStateTable &p, map<string, KeyOpFieldsValuesTuple> &port_cfg_map);
void checkPortInitDone(DBConnector *appl_db);

int main(int argc, char **argv)
{
    Logger::linkToDbNative("portsyncd");
    int opt;
    string port_config_file;
    map<string, KeyOpFieldsValuesTuple> port_cfg_map;

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

    DBConnector cfgDb(CONFIG_DB, DBConnector::DEFAULT_UNIXSOCKET, 0);
    DBConnector appl_db(APPL_DB, DBConnector::DEFAULT_UNIXSOCKET, 0);
    DBConnector state_db(STATE_DB, DBConnector::DEFAULT_UNIXSOCKET, 0);
    ProducerStateTable p(&appl_db, APP_PORT_TABLE_NAME);
    SubscriberStateTable portCfg(&cfgDb, CFG_PORT_TABLE_NAME);

    checkWarmStart(&appl_db, "portsyncd");
    if (isWarmStart())
    {
        checkPortInitDone(&appl_db);
        /* If PortInitDone already set, clear the init port config buffer */
        if(g_init)
        {
            deque<KeyOpFieldsValuesTuple> vkco;
            portCfg.pops(vkco);
        }

    }

    LinkSync sync(&appl_db, &state_db);
    NetDispatcher::getInstance().registerMessageHandler(RTM_NEWLINK, &sync);
    NetDispatcher::getInstance().registerMessageHandler(RTM_DELLINK, &sync);

    try
    {
        NetLink netlink;
        Select s;

        netlink.registerGroup(RTNLGRP_LINK);
        cout << "Listen to link messages..." << endl;

        /* For portsyncd warm start, don't process init port config again if PortInitDone set */
        if (!g_init)
        {
            if (!port_config_file.empty())
            {
                handlePortConfigFile(p, port_config_file);
            } else {
                handlePortConfigFromConfigDB(p, cfgDb);
            }
        }

        s.addSelectable(&netlink);
        s.addSelectable(&portCfg);

        if (isWarmStart() && !g_init)
        {
            // Dump existing Links
            cout << "Warm start: send netlink RTM_GETLINK request" << endl;
            netlink.dumpRequest(RTM_GETLINK);
        }
        while (true)
        {
            Selectable *temps;
            int ret;
            ret = s.select(&temps, 1);

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
                if (!port_cfg_map.empty())
                {
                    handlePortConfig(p, port_cfg_map);
                }
            }

            if (temps == (Selectable *)&portCfg)
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
        }
    }
    catch (const std::exception& e)
    {
        cerr << "Exception \"" << e.what() << "\" had been thrown in deamon" << endl;
        return EXIT_FAILURE;
    }

    return 1;
}

static void notifyPortConfigDone(ProducerStateTable &p)
{
    /* Notify that all ports added */
    FieldValueTuple finish_notice("count", to_string(g_portSet.size()));
    vector<FieldValueTuple> attrs = { finish_notice };
    p.set("PortConfigDone", attrs);
}

void handlePortConfigFromConfigDB(ProducerStateTable &p, DBConnector &cfgDb)
{
    cout << "Get port configuration from ConfigDB..." << endl;

    Table table(&cfgDb, CFG_PORT_TABLE_NAME);
    std::vector<FieldValueTuple> ovalues;
    std::vector<string> keys;
    table.getKeys(keys);
    for ( auto &k : keys )
    {
        table.get(k, ovalues);
        vector<FieldValueTuple> attrs;
        for ( auto &v : ovalues )
        {
            FieldValueTuple attr(v.first, v.second);
            attrs.push_back(attr);
        }
        p.set(k, attrs);
        g_portSet.insert(k);
    }
    notifyPortConfigDone(p);
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
            // Take this line as column header line
            istringstream iss_hdr(line.substr(1));
            string hdr;

            header.clear();
            while (! iss_hdr.eof()) {
                iss_hdr >> hdr;
                cout << "Adding column header '" << hdr << "'" << endl;
                header.push_back(hdr);
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
    notifyPortConfigDone(p);
}

void handlePortConfig(ProducerStateTable &p, map<string, KeyOpFieldsValuesTuple> &port_cfg_map)
{

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
                p.set(key, values);
                for (auto fv : values)
                {
                    string field = fvField(fv);
                    string value = fvValue(fv);

                    /* Update the mtu field on host interface */
                    if (field == "mtu")
                    {
                        string cmd, res;
                        cmd = "ip link set " + key + " mtu " + value;
                        swss::exec(cmd, res);
                     }
                }
            }

            it = port_cfg_map.erase(it);
        }
        else
        {
            it++;
        }
    }
}

void checkPortInitDone(DBConnector *appl_db)
{
    std::unique_ptr<Table>  portTable = std::unique_ptr<Table>(new Table(appl_db, APP_PORT_TABLE_NAME));
    std::vector<FieldValueTuple> vfv;

    if (portTable->get("PortInitDone", vfv))
    {
        if (!vfv.empty())
        {
            g_init = true;
        }
    }
}
