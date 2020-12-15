#include <iostream>
#include <stdlib.h>
#include <unistd.h>
#include <chrono>
#include "logger.h"
#include "select.h"
#include "netdispatcher.h"
#include "netlink.h"
#include "neighsyncd/neighsync.h"
#include "subscriberstatetable.h"
#include "consumerstatetable.h"
#include <pthread.h>

using namespace std;
using namespace swss;

#define NETLINK_BUFFSIZE        134217728   //128MB

class NeighSync *g_neighsync;
void netlinkDumpErrorCb(void *cbData);

void* NeighSync::arpRefreshThread(void *arg)
{
    DBConnector cfgDb("CONFIG_DB", 0);
    DBConnector log_db("LOGLEVEL_DB", 0);
    DBConnector stateDb("STATE_DB", 0);


    SubscriberStateTable cfg_intf_table(&cfgDb, CFG_INTF_TABLE_NAME);
    SubscriberStateTable cfg_lag_intf_table(&cfgDb, CFG_LAG_INTF_TABLE_NAME);
    SubscriberStateTable cfg_loopbk_intf_table(&cfgDb, CFG_LOOPBACK_INTERFACE_TABLE_NAME);
    SubscriberStateTable cfg_vlan_intf_table(&cfgDb, CFG_VLAN_INTF_TABLE_NAME);
    SubscriberStateTable cfg_device_metadat_table(&cfgDb, CFG_DEVICE_METADATA_TABLE_NAME);
    SubscriberStateTable cfg_switch_table(&cfgDb, CFG_SWITCH_TABLE_NAME);
    SubscriberStateTable cfg_neigh_global_table(&cfgDb, CFG_NEIGH_GLOBAL_TABLE);
    SubscriberStateTable state_fdb_table(&stateDb, STATE_FDB_TABLE_NAME);
    SelectableTimer neighRefreshTimer(timespec{.tv_sec = NEIGH_REFRESH_TIMER_TICK, .tv_nsec = 0});

    SWSS_LOG_NOTICE("NeighSync::arpRefreshThread called");
    while (1)
    {
        Select s;

        s.addSelectable(&cfg_vlan_intf_table);
        s.addSelectable(&cfg_intf_table);
        s.addSelectable(&cfg_lag_intf_table);
        s.addSelectable(&cfg_loopbk_intf_table);
        s.addSelectable(&cfg_device_metadat_table);
        s.addSelectable(&cfg_switch_table);
        s.addSelectable(&cfg_neigh_global_table);
        s.addSelectable(&state_fdb_table);
        s.addSelectable(&neighRefreshTimer);

        neighRefreshTimer.start();

        while (true)
        {
            Selectable *temps;
            s.select(&temps);

            //Refresh Neighbor entries
            if (temps == &neighRefreshTimer)
            {
                g_neighsync->refreshTimer();
                neighRefreshTimer.reset();
                continue;
            }
            else if ( temps == (Selectable *)&cfg_vlan_intf_table)
            {
                std::deque<KeyOpFieldsValuesTuple> entries;
                cfg_vlan_intf_table.pops(entries);
                g_neighsync->processIpInterfaceTask(entries);
            }
            else if ( temps == (Selectable *)&cfg_intf_table)
            {
                std::deque<KeyOpFieldsValuesTuple> entries;
                cfg_intf_table.pops(entries);
                g_neighsync->processIpInterfaceTask(entries);
            }
            else if ( temps == (Selectable *)&cfg_lag_intf_table)
            {
                std::deque<KeyOpFieldsValuesTuple> entries;
                cfg_lag_intf_table.pops(entries);
                g_neighsync->processIpInterfaceTask(entries);
            }
            else if ( temps == (Selectable *)&cfg_loopbk_intf_table)
            {
                std::deque<KeyOpFieldsValuesTuple> entries;
                cfg_loopbk_intf_table.pops(entries);
                g_neighsync->processIpInterfaceTask(entries);
            }
            else if ( temps == (Selectable *)&cfg_device_metadat_table)
            {
                std::deque<KeyOpFieldsValuesTuple> entries;
                cfg_device_metadat_table.pops(entries);
                g_neighsync->doSystemMacTask(entries);
            }
            else if ( temps == (Selectable *)&cfg_switch_table)
            {
                std::deque<KeyOpFieldsValuesTuple> entries;
                cfg_switch_table.pops(entries);
                g_neighsync->doSwitchTask(entries);
            }
            else if ( temps == (Selectable *)&cfg_neigh_global_table)
            {
                std::deque<KeyOpFieldsValuesTuple> entries;
                cfg_neigh_global_table.pops(entries);
                g_neighsync->doNeighGlobalTask(entries);
            }
        }
    }
}

typedef void * (*THREADFUNCPTR)(void *);

void NeighSync::startArpRefreshThread()
{
    int ret;
    pthread_t thread_id;
    pthread_attr_t attr;

    ret = pthread_attr_init(&attr);
    ret = pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    ret = pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);
    ret = pthread_create(&thread_id, &attr, (THREADFUNCPTR)&arpRefreshThread, NULL);

    SWSS_LOG_NOTICE("ARP refresh thread created: ret(%d) thread-id(%ld)", ret, thread_id);
}

int main(int argc, char **argv)
{
    Logger::linkToDbNative("neighsyncd");

    DBConnector appDb("APPL_DB", 0);
    RedisPipeline pipelineAppDB(&appDb);
    DBConnector stateDb("STATE_DB", 0);
    SelectableTimer neighRefreshTimer(timespec{.tv_sec = NEIGH_REFRESH_INTERVAL, .tv_nsec = 0});
    NeighSync sync(&pipelineAppDB, &stateDb);

    NetDispatcher::getInstance().registerMessageHandler(RTM_NEWNEIGH, &sync);
    NetDispatcher::getInstance().registerMessageHandler(RTM_DELNEIGH, &sync);

    g_neighsync = &sync;

    sync.startArpRefreshThread();

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
            cout << "Exception \"" << e.what() << "\" had been thrown in deamon" << endl;
            return 0;
        }
    }

    return 1;
}
