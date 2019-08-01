#include <iostream>
#include "logger.h"
#include "select.h"
#include "selectabletimer.h"
#include "netdispatcher.h"
#include "warmRestartHelper.h"
#include "fpmsyncd/fpmlink.h"
#include "fpmsyncd/routesync.h"


using namespace std;
using namespace swss;


/*
 * Default warm-restart timer interval for routing-stack app. To be used only if
 * no explicit value has been defined in configuration.
 */
const uint32_t DEFAULT_ROUTING_RESTART_INTERVAL = 120;


int main(int argc, char **argv)
{
    swss::Logger::linkToDbNative("fpmsyncd");
    DBConnector db(APPL_DB, DBConnector::DEFAULT_UNIXSOCKET, 0);
    RedisPipeline pipeline(&db);
    RouteSync sync(&pipeline);

    NetDispatcher::getInstance().registerMessageHandler(RTM_NEWROUTE, &sync);
    NetDispatcher::getInstance().registerMessageHandler(RTM_DELROUTE, &sync);

    while (true)
    {
        try
        {
            FpmLink fpm;
            Select s;
            SelectableTimer warmStartTimer(timespec{0, 0});

            /*
             * Pipeline should be flushed right away to deal with state pending
             * from previous try/catch iterations.
             */
            pipeline.flush();

            cout << "Waiting for fpm-client connection..." << endl;
            fpm.accept();
            cout << "Connected!" << endl;

            s.addSelectable(&fpm);

            /* If warm-restart feature is enabled, execute 'restoration' logic */
            bool warmStartEnabled = sync.m_warmStartHelper.checkAndStart();
            if (warmStartEnabled)
            {
                /* Obtain warm-restart timer defined for routing application */
                time_t  warmRestartIval = sync.m_warmStartHelper.getRestartTimer();
                if (!warmRestartIval)
                {
                    warmStartTimer.setInterval(timespec{DEFAULT_ROUTING_RESTART_INTERVAL, 0});
                }
                else
                {
                    warmStartTimer.setInterval(timespec{warmRestartIval, 0});
                }

                /* Execute restoration instruction and kick off warm-restart timer */
                if (sync.m_warmStartHelper.runRestoration())
                {
                    warmStartTimer.start();
                    s.addSelectable(&warmStartTimer);
                }
            }

            while (true)
            {
                Selectable *temps;

                /* Reading FPM messages forever (and calling "readMe" to read them) */
                s.select(&temps);

                /*
                 * Upon expiration of the warm-restart timer, proceed to run the
                 * reconciliation process and remove warm-restart timer from
                 * select() loop.
                 */
                if (warmStartEnabled && temps == &warmStartTimer)
                {
                    SWSS_LOG_NOTICE("Warm-Restart timer expired.");
                    sync.m_warmStartHelper.reconcile();
                    s.removeSelectable(&warmStartTimer);

                    pipeline.flush();
                    SWSS_LOG_DEBUG("Pipeline flushed");
                }
                else if (!warmStartEnabled || sync.m_warmStartHelper.isReconciled())
                {
                    pipeline.flush();
                    SWSS_LOG_DEBUG("Pipeline flushed");
                }
            }
        }
        catch (FpmLink::FpmConnectionClosedException &e)
        {
            cout << "Connection lost, reconnecting..." << endl;
        }
        catch (const exception& e)
        {
            cout << "Exception \"" << e.what() << "\" had been thrown in deamon" << endl;
            return 0;
        }
    }

    return 1;
}
