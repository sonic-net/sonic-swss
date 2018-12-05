#include <iostream>
#include <chrono>
#include <team.h>
#include "logger.h"
#include "select.h"
#include "netdispatcher.h"
#include "netlink.h"
#include "warm_restart.h"
#include "teamsync.h"


using namespace std;
using namespace swss;
using namespace std::chrono;

int main(int argc, char **argv)
{
    swss::Logger::linkToDbNative("teamsyncd");
    WarmStart::initialize("teamsyncd", "teamd");
    WarmStart::checkWarmStart("teamsyncd", "teamd");
    DBConnector db(APPL_DB, DBConnector::DEFAULT_UNIXSOCKET, 0);
    DBConnector stateDb(STATE_DB, DBConnector::DEFAULT_UNIXSOCKET, 0);
    Select s;
    TeamSync sync(&db, &stateDb, &s);

    NetDispatcher::getInstance().registerMessageHandler(RTM_NEWLINK, &sync);
    NetDispatcher::getInstance().registerMessageHandler(RTM_DELLINK, &sync);

    steady_clock::time_point start_time = steady_clock::now();

    while (1)
    {
        try
        {
            NetLink netlink;

            netlink.registerGroup(RTNLGRP_LINK);
            cout << "Listens to teamd events..." << endl;
            netlink.dumpRequest(RTM_GETLINK);

            s.addSelectable(&netlink);
            while (true)
            {
                Selectable *temps;
                s.select(&temps, 1000); // check every second

                auto diff = duration_cast<seconds>(steady_clock::now() - start_time);
                if(diff.count() > WR_PENDING_TIME)
                {
                    sync.applyState();
                }

                sync.doSelectableTask();
            }
        }
        catch (const std::exception& e)
        {
            cout << "Exception \"" << e.what() << "\" had been thrown in deamon" << endl;
            return 0;
        }
    }

    return 1;
}
