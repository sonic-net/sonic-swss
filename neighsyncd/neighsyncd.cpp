#include <iostream>
#include <stdlib.h>
#include <unistd.h>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <map>
#include "logger.h"
#include "select.h"
#include "netdispatcher.h"
#include "netlink.h"
#include "neighsyncd/linklocalresyncstate.h"
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
            s.addSelectable(sync.getCfgEvpnNvoTable());
            // Match the configuration tables read by isLinkLocalEnabled().
            SubscriberStateTable interfaces(&cfgDb, CFG_INTF_TABLE_NAME);
            SubscriberStateTable lags(&cfgDb, CFG_LAG_INTF_TABLE_NAME);
            SubscriberStateTable vlans(&cfgDb, CFG_VLAN_INTF_TABLE_NAME);
            SubscriberStateTable *interfaceTables[] = {&interfaces, &lags, &vlans};
            LinkLocalResyncState resyncState;
            for (auto *table : interfaceTables)
            {
                deque<KeyOpFieldsValuesTuple> entries;
                table->pops(entries);
                // Existing rows initialize the observed state. The startup
                // RTM_GETNEIGH request already covers their kernel neighbors.
                resyncState.initialize(entries);
                s.addSelectable(table);
            }
            map<string, steady_clock::time_point> retryAfter;
            while (true)
            {
                const auto &pendingInterfaces = resyncState.getPendingInterfaces();
                for (auto retry = retryAfter.begin(); retry != retryAfter.end();)
                {
                    if (pendingInterfaces.count(retry->first) == 0)
                    {
                        retry = retryAfter.erase(retry);
                    }
                    else
                    {
                        ++retry;
                    }
                }

                int selectTimeout = -1;
                const auto now = steady_clock::now();
                for (const auto &interface : pendingInterfaces)
                {
                    const auto retry = retryAfter.find(interface);
                    if (retry == retryAfter.end() || retry->second <= now)
                    {
                        selectTimeout = 0;
                        break;
                    }

                    const auto wait = duration_cast<milliseconds>(retry->second - now).count();
                    const int waitMilliseconds = static_cast<int>(max<int64_t>(1, wait));
                    if (selectTimeout < 0 || waitMilliseconds < selectTimeout)
                    {
                        selectTimeout = waitMilliseconds;
                    }
                }

                Selectable *temps = nullptr;
                const int selectResult = s.select(&temps, selectTimeout);
                if (selectResult == Select::ERROR)
                {
                    SWSS_LOG_ERROR("Select failed while waiting for neighbor or interface updates");
                    continue;
                }
                if (selectResult != Select::OBJECT && selectResult != Select::TIMEOUT)
                {
                    continue;
                }

                if (selectResult == Select::OBJECT &&
                    temps == (Selectable *)sync.getCfgEvpnNvoTable())
                {
                    sync.processCfgEvpnNvo();
                }
                for (auto *table : interfaceTables)
                {
                    if (selectResult != Select::OBJECT || temps != table)
                    {
                        continue;
                    }
                    deque<KeyOpFieldsValuesTuple> entries;
                    table->pops(entries);
                    const auto transitions = resyncState.process(entries);
                    for (const auto &interface : transitions.enabled)
                    {
                        retryAfter.erase(interface);
                    }
                    for (const auto &interface : transitions.disabled)
                    {
                        retryAfter.erase(interface);
                    }
                }

                const auto retryNow = steady_clock::now();
                const auto &currentPendingInterfaces = resyncState.getPendingInterfaces();
                const auto readyInterface = find_if(
                    currentPendingInterfaces.begin(), currentPendingInterfaces.end(),
                    [&retryAfter, retryNow](const string &interface) {
                        const auto retry = retryAfter.find(interface);
                        return retry == retryAfter.end() || retry->second <= retryNow;
                    });
                if (readyInterface != currentPendingInterfaces.end())
                {
                    const string interface = *readyInterface;
                    if (sync.resyncLinkLocalNeighbors(interface))
                    {
                        resyncState.markResyncComplete(interface);
                        retryAfter.erase(interface);
                    }
                    else
                    {
                        retryAfter[interface] = steady_clock::now() + seconds(1);
                    }
                }
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
