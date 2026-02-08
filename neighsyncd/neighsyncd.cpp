#include <iostream>
#include <stdlib.h>
#include <unistd.h>
#include <chrono>
#include "logger.h"
#include "select.h"
#include "netdispatcher.h"
#include "netlink.h"
#include "neighsyncd/neighsync.h"

using namespace std;
using namespace swss;

int main(int argc, char **argv)
{
    Logger::linkToDbNative("neighsyncd");

    DBConnector appDb("APPL_DB", 0);
    RedisPipeline pipelineAppDB(&appDb);
    DBConnector stateDb("STATE_DB", 0);
    DBConnector cfgDb("CONFIG_DB", 0);

    NeighSync sync(&pipelineAppDB, &stateDb, &cfgDb, &appDb);

    NetDispatcher::getInstance().registerMessageHandler(RTM_NEWNEIGH, &sync);
    NetDispatcher::getInstance().registerMessageHandler(RTM_DELNEIGH, &sync);

    while (1)
    {
        try
        {
            NetLink netlink;
            Select s;

            using namespace std::chrono;
            /*
             * If warmstart, read neighbor table to cache map.
             * Wait the kernel neighbor table restore to finish in case of warmreboot.
             * Regular swss docker warmstart should have marked the restore flag to true always.
             * Start reconcile timer once restore flag is set
             */
            if (sync.getRestartAssist()->isWarmStartInProgress())
            {
                sync.getRestartAssist()->readTablesToMap();

                steady_clock::time_point starttime = steady_clock::now();
                while (!sync.isNeighRestoreDone())
                {
                    duration<double> time_span =
                        duration_cast<duration<double>>(steady_clock::now() - starttime);
                    int pasttime = int(time_span.count());
                    SWSS_LOG_INFO("waited neighbor table to be restored to kernel"
                      " for %d seconds", pasttime);
                    if (pasttime > RESTORE_NEIGH_WAIT_TIME_OUT)
                    {
                        SWSS_LOG_ERROR("neighbor table restore is not finished"
                            " after timed-out, exit!!!");
                        exit(EXIT_FAILURE);
                    }
                    sleep(1);
                }
                sync.getRestartAssist()->startReconcileTimer(s);
            }

            netlink.registerGroup(RTNLGRP_NEIGH);
            cout << "Listens to neigh messages..." << endl;
            netlink.dumpRequest(RTM_GETNEIGH);

            s.addSelectable(&netlink);
            while (true)
            {
                Selectable *temps;
                s.select(&temps);
                /*
                 * If warmstart is in progress, we check the reconcile timer,
                 * if timer expired, we stop the timer and start the reconcile process
                 */
                if (sync.getRestartAssist()->isWarmStartInProgress())
                {
                    if (sync.getRestartAssist()->checkReconcileTimer(temps))
                    {
                        sync.getRestartAssist()->stopReconcileTimer(s);
                        sync.getRestartAssist()->reconcile();
                    }
                }
            }
        }
        catch (const std::exception& e)
        {
            cout << "Exception \"" << e.what() << "\" had been thrown in daemon" << endl;
            return 0;
        }
    }

    return 1;
}
