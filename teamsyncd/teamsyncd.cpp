#include <iostream>
#include <team.h>
#include <signal.h>
#include "logger.h"
#include "select.h"
#include "netdispatcher.h"
#include "netlink.h"
#include "teamsync.h"

using namespace std;
using namespace swss;

bool received_sigterm = false;

void sig_handler(int signo)
{
    received_sigterm = true;
    return;
}

int main(int argc, char **argv)
{
    swss::Logger::linkToDbNative(TEAMSYNCD_APP_NAME);
    DBConnector db("APPL_DB", 0);
    DBConnector stateDb("STATE_DB", 0);
    Select s;
    TeamSync sync(&db, &stateDb, &s);

    NetDispatcher::getInstance().registerMessageHandler(RTM_NEWLINK, &sync);
    NetDispatcher::getInstance().registerMessageHandler(RTM_DELLINK, &sync);

    /* Register the signal handler for SIGTERM */
    signal(SIGTERM, sig_handler);

    try
    {
        NetLink netlink;
        netlink.registerGroup(RTNLGRP_LINK);
        cout << "Listens to teamd events..." << endl;
        netlink.dumpRequest(RTM_GETLINK);

        s.addSelectable(&netlink);
        while (!received_sigterm)
        {
            Selectable *temps;
            s.select(&temps, 1000); // block for a second
            sync.periodic();
        }
        sync.cleanTeamSync();
        SWSS_LOG_NOTICE("Received SIGTERM Exiting");
    }
    catch (const std::exception& e)
    {
        cout << "Exception \"" << e.what() << "\" had been thrown in deamon" << endl;
        return 0;
    }

    return 0;
}
