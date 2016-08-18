#include <iostream>
#include "logger.h"
#include "select.h"
#include "netdispatcher.h"
#include "netlink.h"
#include "intfsyncd/intfsync.h"

using namespace std;
using namespace swss;

int main(int argc, char **argv)
{
    DBConnector db(APPL_DB, "localhost", 6379, 0);
    IntfSync sync(&db);

    NetDispatcher::getInstance().registerMessageHandler(RTM_NEWADDR, &sync);
    NetDispatcher::getInstance().registerMessageHandler(RTM_DELADDR, &sync);

    while (1)
    {
        try
        {
            NetLink netlink;
            Select s;

            netlink.registerGroup(RTNLGRP_IPV4_IFADDR);
            cout << "Listens to interface messages..." << endl;
            netlink.dumpRequest(RTM_GETADDR);

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
