#include <iostream>
#include "logger.h"
#include "select.h"
#include "netdispatcher.h"
#include "netlink.h"
#include "neighsyncd/neighsync.h"
#include "inc/common.h"

using namespace std;
using namespace swss;

int main(int argc, char **argv)
{
    DBConnector db(APPL_DB, REDIS_UNIXSOCKET, 0);
    NeighSync sync(&db);

    NetDispatcher::getInstance().registerMessageHandler(RTM_NEWNEIGH, &sync);
    NetDispatcher::getInstance().registerMessageHandler(RTM_DELNEIGH, &sync);

    while (1)
    {
        try
        {
            NetLink netlink;
            Select s;

            netlink.registerGroup(RTNLGRP_NEIGH);
            cout << "Listens to neigh messages..." << endl;
            netlink.dumpRequest(RTM_GETNEIGH);

            s.addSelectable(&netlink);
            while (true)
            {
                Selectable *temps;
                int tempfd;
                s.select(&temps, &tempfd);
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
