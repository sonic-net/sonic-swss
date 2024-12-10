#include <iostream>
#include <stdlib.h>
#include <unistd.h>
#include <chrono>
#include "logger.h"
#include "select.h"
#include "netdispatcher.h"
#include "netlink.h"
#include "fdbsyncd/fdbsync.h"
#include "warm_restart.h"

using namespace std;
using namespace swss;

int main(int argc, char **argv)
{
    Logger::linkToDbNative("fdbsyncd");

    DBConnector appDb("APPL_DB", 0);
    RedisPipeline pipelineAppDB(&appDb);
    DBConnector stateDb("STATE_DB", 0);
    DBConnector config_db("CONFIG_DB", 0);

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
            SelectableTimer replayCheckTimer(timespec{0, 0});

            using namespace std::chrono;

            /*
             * If WarmStart is enabled, restore the VXLAN-FDB and VNI 
             * tables and start a reconcillation timer
             */
            if (sync.getRestartAssist()->isWarmStartInProgress())
            {
                sync.getRestartAssist()->readTablesToMap();
                
                steady_clock::time_point starttime = steady_clock::now();
                while (!sync.isIntfRestoreDone())
                {
                    duration<double> time_span =
                        duration_cast<duration<double>>(steady_clock::now() - starttime);
                    int pasttime = int(time_span.count());

                    if (pasttime > INTF_RESTORE_MAX_WAIT_TIME)
                    {
                        SWSS_LOG_INFO("timed-out before all interface data was replayed to kernel!!!");
                        throw runtime_error("fdbsyncd: timedout on interface data replay");
                    }
                    sleep(1);
                }
                replayCheckTimer.setInterval(timespec{1, 0});
                replayCheckTimer.start();
                s.addSelectable(&replayCheckTimer);
            }
            else
            {
                sync.getRestartAssist()->warmStartDisabled();
                sync.m_reconcileDone = true;
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

                if (temps == (Selectable *)sync.getFdbStateTable())
                {
                    sync.processStateFdb();
                }
                else if (temps == (Selectable *)sync.getMclagRemoteFdbStateTable())
                {
                    sync.processStateMclagRemoteFdb();
                }
                else if (temps == (Selectable *)sync.getCfgEvpnNvoTable())
                {
                    sync.processCfgEvpnNvo();
                }
                else if (temps == &replayCheckTimer)
                {
                    if (sync.getFdbStateTable()->empty() && sync.getCfgEvpnNvoTable()->empty())
                    {
                        sync.getRestartAssist()->appDataReplayed();
                        SWSS_LOG_NOTICE("FDB Replay Complete");
                        s.removeSelectable(&replayCheckTimer);

                        /* Obtain warm-restart timer defined for routing application */
                        uint32_t warmRestartIval = WarmStart::getWarmStartTimer("bgp","bgp");
                        if (warmRestartIval)
                        {
                            sync.getRestartAssist()->setReconcileInterval(warmRestartIval);
                        }
                        //Else the interval is already set to default value

                        //TODO: Optimise the reconcillation time using eoiu - issue#1657
                        SWSS_LOG_NOTICE("Starting ReconcileTimer");
                        sync.getRestartAssist()->startReconcileTimer(s);
                    }
                    else
                    {
                        replayCheckTimer.setInterval(timespec{1, 0});
                        // re-start replay check timer
                        replayCheckTimer.start();
                    }
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
                            SWSS_LOG_NOTICE("VXLAN FDB VNI Reconcillation Complete");
                        }
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
