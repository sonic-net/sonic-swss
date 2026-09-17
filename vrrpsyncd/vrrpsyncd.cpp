#include <iostream>
#include <stdlib.h>
#include <unistd.h>
#include <string>
#include "logger.h"
#include "select.h"
#include "netdispatcher.h"
#include "netlink.h"
#include "vrrpsyncd/vrrpsync.h"

using namespace std;
using namespace swss;

int main(int argc, char **argv)
{
    Logger::linkToDbNative("vrrpsyncd");

    DBConnector appDb("APPL_DB", 0);
    DBConnector cfgDb("CONFIG_DB", 0);
    RedisPipeline pipelineAppDB(&appDb);

    try {
        VrrpSync sync(&pipelineAppDB, &cfgDb);

        NetDispatcher::getInstance().registerMessageHandler(RTM_NEWADDR, &sync);
        NetDispatcher::getInstance().registerMessageHandler(RTM_DELADDR, &sync);

        NetDispatcher::getInstance().registerMessageHandler(RTM_NEWLINK, &sync);
        NetDispatcher::getInstance().registerMessageHandler(RTM_DELLINK, &sync);

        while (1)
        {
            try
            {
                NetLink netlink;
                Select s;
                Selectable *temps;
                int ret;

                pipelineAppDB.flush();
                
                /*
                 * If warmstart, read VRRP IF table to cache map.
                 * Wait the kernel IFADDR table restore to finish in case of warmreboot.
                 * Regular swss docker warmstart should have marked the restore flag to true always.
                 * Start reconcile timer once restore flag is set
                 */
                if (sync.getRestartAssist()->isWarmStartInProgress())
                {
                    vector<FieldValueTuple> fv;
                    FieldValueTuple sag("sag","yes");
                    fv.push_back(sag);

                    // Ignore SAG intfs while reading App DB VRRP Table
                    sync.getRestartAssist()->readTablesToMap();
                    sync.getRestartAssist()->startReconcileTimer(s);
                }

                netlink.registerGroup(RTNLGRP_LINK);
                netlink.registerGroup(RTNLGRP_IPV4_IFADDR);          
                netlink.registerGroup(RTNLGRP_IPV6_IFADDR); 
                
                SWSS_LOG_NOTICE("Listens to ifaddr and link messages...");
                netlink.dumpRequest(RTM_GETLINK);            

                s.addSelectable(&netlink);
                ret = s.select(&temps, 1);
                if (ret == Select::ERROR)
                {
                    SWSS_LOG_ERROR("Error in RTM_GETLINK dump");
                }

                netlink.dumpRequest(RTM_GETADDR);

                while (true)
                {
                    ret = s.select(&temps, 1);

                    if (ret == Select::ERROR)
                    {
                        SWSS_LOG_ERROR("Error had been returned in select");
                        continue;
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
                            pipelineAppDB.flush();						
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
    }
    catch (const std::exception& e)
    {
        cout << "Exception \"" << e.what() << "\" had been thrown in deamon" << endl;
        return 0;
    }

    return 1;
}


