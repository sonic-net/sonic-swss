#include <iostream>
#include <inttypes.h>
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

// Wait 3 seconds after detecting EOIU reached state
// TODO: support eoiu hold interval config
const uint32_t DEFAULT_EOIU_HOLD_INTERVAL = 3;

// Check if eoiu state reached by both ipv4 and ipv6
static bool eoiuFlagsSet(Table &bgpStateTable)
{
    string value;

    bgpStateTable.hget("IPv4|eoiu", "state", value);
    if (value != "reached")
    {
        SWSS_LOG_DEBUG("IPv4|eoiu state: %s", value.c_str());
        return false;
    }
    bgpStateTable.hget("IPv6|eoiu", "state", value);
    if (value != "reached")
    {
        SWSS_LOG_DEBUG("IPv6|eoiu state: %s", value.c_str());
        return false;
    }
    SWSS_LOG_NOTICE("Warm-Restart bgp eoiu reached for both ipv4 and ipv6");
    return true;
}

int main(int argc, char **argv)
{
    swss::Logger::linkToDbNative("fpmsyncd");
    DBConnector db("APPL_DB", 0);
    RedisPipeline pipeline(&db);
    RouteSync sync(&pipeline);

    DBConnector stateDb("STATE_DB", 0);
    Table bgpStateTable(&stateDb, STATE_BGP_TABLE_NAME);

    NetDispatcher::getInstance().registerMessageHandler(RTM_NEWROUTE, &sync);
    NetDispatcher::getInstance().registerMessageHandler(RTM_DELROUTE, &sync);

    while (true)
    {
        try
        {
            FpmLink fpm;
            Select s;
            SelectableTimer warmStartTimer(timespec{0, 0});
            // Before eoiu flags detected, check them periodically. It also stop upon detection of reconciliation done.
            SelectableTimer eoiuCheckTimer(timespec{0, 0});
            // After eoiu flags are detected, start a hold timer before starting reconciliation.
            SelectableTimer eoiuHoldTimer(timespec{0, 0});
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
                time_t warmRestartIval = sync.m_warmStartHelper.getRestartTimer();
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
                    SWSS_LOG_NOTICE("Warm-Restart timer started.");
                }

                // Also start periodic eoiu check timer, first wait 5 seconds, then check every 1 second
                eoiuCheckTimer.setInterval(timespec{5, 0});
                eoiuCheckTimer.start();
                s.addSelectable(&eoiuCheckTimer);
                SWSS_LOG_NOTICE("Warm-Restart eoiuCheckTimer timer started.");
            }

            while (true)
            {
                Selectable *temps;

                /* Reading FPM messages forever (and calling "readMe" to read them) */
                s.select(&temps);

                /*
                 * Upon expiration of the warm-restart timer or eoiu Hold Timer, proceed to run the
                 * reconciliation process if not done yet and remove the timer from
                 * select() loop.
                 * Note:  route reconciliation always succeeds, it will not be done twice.
                 */
                if (temps == &warmStartTimer || temps == &eoiuHoldTimer)
                {
                    if (temps == &warmStartTimer)
                    {
                        SWSS_LOG_NOTICE("Warm-Restart timer expired.");
                    }
                    else
                    {
                        SWSS_LOG_NOTICE("Warm-Restart EOIU hold timer expired.");
                    }
                    if (sync.m_warmStartHelper.inProgress())
                    {
                        sync.m_warmStartHelper.reconcile();
                        SWSS_LOG_NOTICE("Warm-Restart reconciliation processed.");
                    }
                    // remove the one-shot timer.
                    s.removeSelectable(temps);
                    pipeline.flush();
                    SWSS_LOG_DEBUG("Pipeline flushed");
                }
                else if (temps == &eoiuCheckTimer)
                {
                    if (sync.m_warmStartHelper.inProgress())
                    {
                        if (eoiuFlagsSet(bgpStateTable))
                        {
                            /* Obtain eoiu hold timer defined for bgp docker */
                            uintmax_t eoiuHoldIval = WarmStart::getWarmStartTimer("eoiu_hold", "bgp");
                            if (!eoiuHoldIval)
                            {
                                eoiuHoldTimer.setInterval(timespec{DEFAULT_EOIU_HOLD_INTERVAL, 0});
                                eoiuHoldIval = DEFAULT_EOIU_HOLD_INTERVAL;
                            }
                            else
                            {
                                eoiuHoldTimer.setInterval(timespec{(time_t)eoiuHoldIval, 0});
                            }
                            eoiuHoldTimer.start();
                            s.addSelectable(&eoiuHoldTimer);
                            SWSS_LOG_NOTICE("Warm-Restart started EOIU hold timer which is to expire in %" PRIuMAX " seconds.", eoiuHoldIval);
                            s.removeSelectable(&eoiuCheckTimer);
                            continue;
                        }
                        eoiuCheckTimer.setInterval(timespec{1, 0});
                        // re-start eoiu check timer
                        eoiuCheckTimer.start();
                        SWSS_LOG_DEBUG("Warm-Restart eoiuCheckTimer restarted");
                    }
                    else
                    {
                        s.removeSelectable(&eoiuCheckTimer);
                    }
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
