#include <iostream>
#include <unistd.h>
#include <chrono>

#include "logger.h"
#include "select.h"
#include "selectabletimer.h"
#include "netdispatcher.h"
#include "netlink.h"
#include "vrrpsyncd/vrrpsync.h"

using namespace std;
using namespace swss;

#define DEFAULT_SELECT_TIMEOUT 1000 /* ms */
/*
 * Default warm-restart timer interval for vrrp app. To be used only if
 * no explicit value has been defined in configuration.
 */
const uint32_t DEFAULT_VRRP_RESTART_INTERVAL = 120;

int main(int argc, char **argv)
{
    Logger::linkToDbNative("vrrpsyncd");

    DBConnector appDb("APPL_DB", 0);
    RedisPipeline pipelineAppDB(&appDb);
    DBConnector stateDb("STATE_DB", 0);
    DBConnector cfgDb("CONFIG_DB", 0);

    VrrpSync sync(&pipelineAppDB, &cfgDb, &appDb, &stateDb);

    NetDispatcher::getInstance().registerMessageHandler(RTM_NEWLINK, &sync);
    NetDispatcher::getInstance().registerMessageHandler(RTM_DELLINK, &sync);

    while (1)
    {
        try
        {
            NetLink netlink;
            Select s;

            /*
             * Pipeline should be flushed right away to deal with state pending
             * from previous try/catch iterations.
             */
            pipelineAppDB.flush();
            netlink.registerGroup(RTNLGRP_LINK);
            cout << "Listens to link messages..." << endl;
            netlink.dumpRequest(RTM_GETLINK);
            
            s.addSelectable(&netlink);
            s.addSelectable(sync.getCfgVrrpTable());
            while (true)
            {
                Selectable *temps;
                s.select(&temps, DEFAULT_SELECT_TIMEOUT);

                if (temps == (Selectable *)sync.getCfgVrrpTable())
                {
                    sync.processCfgVrrp();
                }
            }
        }
        catch (const std::exception& e)
        {
            cout << "Exception \"" << e.what() << "\" had been thrown in daemon" << endl;
            return EXIT_FAILURE;
        }
    }

    return EXIT_SUCCESS;
}
