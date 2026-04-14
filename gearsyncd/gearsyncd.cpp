/*
 * Copyright 2020 Broadcom Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <getopt.h>
#include <iostream>
#include <vector>
#include <map>
#include "dbconnector.h"
#include "producerstatetable.h"
#include "warm_restart.h"
#include "gearboxparser.h"
#include "gearboxutils.h"
#include "schema.h"

#include <unistd.h>

using namespace std;
using namespace swss;

void usage()
{
    cout << "Usage: gearsyncd [-p gear_config.json]" << endl;
    cout << "       -p gearbox_config.json: import gearbox config" << endl;
    cout << "          use configDB data if not specified" << endl;
}

bool handleGearboxConfigFile(string file, bool warm);
bool handleGearboxConfigFromConfigDB(ProducerStateTable &p, DBConnector &cfgDb, bool warm);

static void notifyGearboxConfigDone(ProducerStateTable &p, bool success)
{
    /* Notify that gearbox config successfully written */

    FieldValueTuple finish_notice("success", to_string(success));
    std::vector<FieldValueTuple> attrs = { finish_notice };

    p.set("GearboxConfigDone", attrs);
}

int main(int argc, char **argv)
{
    Logger::linkToDbNative("gearsyncd");
    int opt;
    string gearbox_config_file;
    map<string, KeyOpFieldsValuesTuple> gearbox_cfg_map;
    GearboxUtils utils;

    while ((opt = getopt(argc, argv, "p:ht")) != -1 )
    {
        switch (opt)
        {
        case 'p':
            gearbox_config_file.assign(optarg);
            break;
        case 'h':
            usage();
            return EXIT_FAILURE;
        default: /* '?' */
            usage();
            return EXIT_FAILURE;
        }
    }

    DBConnector cfgDb("CONFIG_DB", 0);
    DBConnector applDb("APPL_DB", 0);
    ProducerStateTable producerStateTable(&applDb, APP_GEARBOX_TABLE_NAME);

    WarmStart::initialize("gearsyncd", "swss");
    WarmStart::checkWarmStart("gearsyncd", "swss");
    const bool warm = WarmStart::isWarmStart();

    try
    {
        if (utils.platformHasGearbox() == false) 
        {
          // no gearbox, move on

          notifyGearboxConfigDone(producerStateTable, true);
        } 
        else if (!handleGearboxConfigFromConfigDB(producerStateTable, cfgDb, warm))
        {
            // if gearbox config is missing in ConfigDB
            // attempt to use gearbox_config.json
            if (!gearbox_config_file.empty())
            {
                handleGearboxConfigFile(gearbox_config_file, warm);
            }
        }
    }
    catch (const std::exception& e)
    {
        cerr << "Exception \"" << e.what() << "\" had been thrown in gearsyncd daemon" << endl;
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}

bool handleGearboxConfigFromConfigDB(ProducerStateTable &p, DBConnector &cfgDb, bool warm)
{
    cout << "Get gearbox configuration from ConfigDB..." << endl;

    Table table(&cfgDb, CFG_GEARBOX_TABLE_NAME);
    std::vector<FieldValueTuple> ovalues;
    std::vector<string> keys;
    table.getKeys(keys);

    if (keys.empty())
    {
        cout << "No gearbox configuration in ConfigDB" << endl;
        return false;
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
    }
    if (!warm)
    {
        notifyGearboxConfigDone(p, true);
    }

    return true;
}

bool handleGearboxConfigFile(string file, bool warm)
{
    GearboxParser parser;
    bool ret;

    parser.setWriteToDb(true);
    parser.setConfigPath(file);
    ret = parser.parse();
    parser.notifyGearboxConfigDone(ret);   // if (!warm....)
    return ret;
}
