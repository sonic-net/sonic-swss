#include <iostream>
#include "logger.h"
#include "select.h"
#include "netdispatcher.h"
#include "fpmsyncd/fpmlink.h"
#include "fpmsyncd/routesync.h"

using namespace std;
using namespace swss;

int main(int argc, char **argv)
{
    DBConnector db(APPL_DB, "localhost", 6379, 0);
    RouteSync sync(&db);

    NetDispatcher::getInstance().registerMessageHandler(RTM_NEWROUTE, &sync);
    NetDispatcher::getInstance().registerMessageHandler(RTM_DELROUTE, &sync);

    while (1)
    {
        try
        {
            FpmLink fpm;
            Select s;

            cout << "Waiting for connection..." << endl;
            fpm.accept();
            cout << "Connected!" << endl;

            s.addSelectable(&fpm);
            while (true)
            {
                Selectable *temps;
                int tempfd;
                /* Reading FPM messages forever (and calling "readMe" to read them) */
                s.select(&temps, &tempfd);
            }
        }
        catch (FpmLink::FpmConnectionClosedException &e)
        {
            cout << "Connection lost, reconnecting..." << endl;
        }
        catch (...)
        {
            cout << "Exception had been thrown in deamon" << endl;
            return 0;
        }
    }

    return 1;
}
