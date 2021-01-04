#include <iostream>
#include <stdlib.h>
#include <unistd.h>
#include <chrono>
#include "logger.h"
#include "select.h"
#include "netdispatcher.h"
#include "netlink.h"
#include "fdbsyncd/fdbsync.h"

using namespace std;
using namespace swss;

int main(int argc, char **argv)
{
    Logger::linkToDbNative("fdbsyncd");

    DBConnector appDb(APPL_DB, DBConnector::DEFAULT_UNIXSOCKET, 0);
    RedisPipeline pipelineAppDB(&appDb);
    DBConnector stateDb(STATE_DB, DBConnector::DEFAULT_UNIXSOCKET, 0);
    DBConnector log_db(LOGLEVEL_DB, DBConnector::DEFAULT_UNIXSOCKET, 0);
    DBConnector config_db(CONFIG_DB, DBConnector::DEFAULT_UNIXSOCKET, 0);

    FdbSync sync(&pipelineAppDB, &stateDb, &config_db);

    NetDispatcher::getInstance().registerMessageHandler(RTM_NEWNEIGH, &sync);
    NetDispatcher::getInstance().registerMessageHandler(RTM_DELNEIGH, &sync);
    NetDispatcher::getInstance().registerMessageHandler(RTM_NEWLINK, &sync);

    while (1)
    {
        try
        {
            NetLink netlink;
            Selectable *temps;
            int ret;
            Select s;

            using namespace std::chrono;

            /*
             * If WarmStart is enabled, restore the VXLAN-FDB and VNI 
             * tables and start a reconcillation timer
             */
            if (sync.getRestartAssist()->isWarmStartInProgress())
            {
                sync.getRestartAssist()->readTablesToMap();
                SWSS_LOG_NOTICE("Starting ReconcileTimer");
            }

            netlink.registerGroup(RTNLGRP_LINK);
            netlink.registerGroup(RTNLGRP_NEIGH);
            SWSS_LOG_NOTICE("Listens to link and neigh messages...");
            netlink.dumpRequest(RTM_GETLINK);
            s.addSelectable(&netlink);
            ret = s.select(&temps, 1);
            if (ret == Select::ERROR)
            {
                SWSS_LOG_ERROR("Error in RTM_GETLINK dump");
            }

            netlink.dumpRequest(RTM_GETNEIGH);

            s.addSelectable(sync.getFdbStateTable());
            s.addSelectable(sync.getMclagRemoteFdbStateTable());
            s.addSelectable(sync.getCfgEvpnNvoTable());
            while (true)
            {
                s.select(&temps);

                if(temps == (Selectable *)sync.getFdbStateTable())
                {
                    sync.processStateFdb();
                }
                else if(temps == (Selectable *)sync.getMclagRemoteFdbStateTable())
                {
                    sync.processStateMclagRemoteFdb();
                }
                else if (temps == (Selectable *)sync.getCfgEvpnNvoTable())
                {
                    sync.processCfgEvpnNvo();
                }
                else
                {
                    /*
                     * If warmstart is in progress, we check the reconcile timer,
                     * if timer expired, we stop the timer and start the reconcile process
                     */
                    if (sync.getRestartAssist()->isWarmStartInProgress())
                    {
                        if (sync.getRestartAssist()->checkReconcileTimer(temps))
                        {
                            sync.m_reconcileDone = true;
                            sync.getRestartAssist()->stopReconcileTimer(s);
                            sync.getRestartAssist()->reconcile();
                            SWSS_LOG_NOTICE("VXLAN FDB VNI Reconcillation Complete (Timer)");
                        }
                    }
                }
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

