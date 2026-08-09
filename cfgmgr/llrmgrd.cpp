// LLR Manager daemon - initial skeleton

#include <unistd.h>
#include <getopt.h>
#include <iostream>
#include <string>
#include <memory>
#include <stdexcept>
#include <fstream>
#include "select.h"

#include "logger.h"

using namespace std;

#include "llrmgr.h"

using namespace swss;

#define SELECT_TIMEOUT 1000

static void usage()
{
    cout << "Usage: llrmgrd -l <llr_profile_lookup.ini> [-h]\n";
    cout << "  -l <file> : Vendor specific LLR profile lookup INI file\n";
    cout << "  -h        : Print this help message\n";
}

int main(int argc, char **argv)
{
    int opt;
    string lookupFile;

    Logger::linkToDbNative("llrmgrd");
    SWSS_LOG_ENTER();

    while ((opt = getopt(argc, argv, "hl:")) != -1) {
        switch (opt) {
        case 'h':
            usage();
            return 0;
        case 'l':
            if (optarg) lookupFile = optarg;
            break;
        default:
            usage();
            return 1;
        }
    }

    if (!lookupFile.empty())
    {
        ifstream file(lookupFile);
        if (!file.good())
        {
            SWSS_LOG_WARN("LLR profile lookup file '%s' not found or inaccessible; auto-profile generation disabled",
                          lookupFile.c_str());
        }
    }

    try 
    {
        DBConnector cfgDb("CONFIG_DB", 0);
        DBConnector stateDb("STATE_DB", 0);
        DBConnector applDb("APPL_DB", 0);

        vector<TableConnector> cfg_llr_table_connectors = {
            TableConnector(&cfgDb, CFG_LLR_PORT_TABLE_NAME),
            TableConnector(&cfgDb, CFG_LLR_PROFILE_TABLE_NAME),
            TableConnector(&cfgDb, CFG_PORT_CABLE_LEN_TABLE_NAME),
            TableConnector(&stateDb, STATE_PORT_TABLE_NAME),
        };

        auto llrmgr = std::make_unique<LlrMgr>(&cfgDb, &stateDb, &applDb, lookupFile, cfg_llr_table_connectors);

        swss::Select s;
        s.addSelectables(llrmgr->getSelectables());

        SWSS_LOG_NOTICE("starting main loop");
        while (true)
        {
            Selectable *sel;
            int ret;

            ret = s.select(&sel, SELECT_TIMEOUT);
            if (ret == Select::ERROR)
            {
                SWSS_LOG_NOTICE("Error: %s!", strerror(errno));
                continue;
            }
            if (ret == Select::TIMEOUT)
            {
                llrmgr->doTask();
                continue;
            }

            auto *c = (Executor *)sel;
            c->execute();
        }
    }
    catch (const exception& e)
    {
        SWSS_LOG_ERROR("llrmgrd fatal exception: %s", e.what());
        return EXIT_FAILURE;
    }

    return 0;
}