#include <iostream>
#include <inttypes.h>
#include "logger.h"
#include "select.h"
#include "selectabletimer.h"
#include "netdispatcher.h"
#include "netlink.h"
#include "notificationconsumer.h"
#include "subscriberstatetable.h"
#include "warmRestartHelper.h"
#include "fpmsyncd/fpmlink.h"
#include "fpmsyncd/fpmsyncd.h"
#include "fpmsyncd/routesync.h"

#include <netlink/route/route.h>

using namespace std;
using namespace swss;

// gSelectTimeout specifies the maximum wait time in milliseconds (-1 == infinite)
static int gSelectTimeout;
#define INFINITE -1
#define FLUSH_TIMEOUT 500  // 500 milliseconds
static int gFlushTimeout = FLUSH_TIMEOUT;
// consider the traffic is small if pipeline contains < 500 entries
#define SMALL_TRAFFIC 500

/**
 * @brief fpmsyncd invokes redispipeline's flush with a timer
 * 
 * redispipeline would automatically flush itself when full,
 * but fpmsyncd can invoke pipeline's flush even if it's not full yet.
 * 
 * By setting gSelectTimeout, fpmsyncd controls the flush interval.
 * 
 * @param pipeline reference to the pipeline to be flushed
 */
void flushPipeline(RedisPipeline& pipeline);

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

    const auto routeResponseChannelName = std::string("APPL_DB_") + APP_ROUTE_TABLE_NAME + "_RESPONSE_CHANNEL";

    DBConnector db("APPL_DB", 0);
    DBConnector cfgDb("CONFIG_DB", 0);
    SubscriberStateTable deviceMetadataTableSubscriber(&cfgDb, CFG_DEVICE_METADATA_TABLE_NAME);
    Table deviceMetadataTable(&cfgDb, CFG_DEVICE_METADATA_TABLE_NAME);
    DBConnector applStateDb("APPL_STATE_DB", 0);
    std::unique_ptr<NotificationConsumer> routeResponseChannel;

    RedisPipeline pipeline(&db, ROUTE_SYNC_PPL_SIZE);
    RouteSync sync(&pipeline);

    DBConnector stateDb("STATE_DB", 0);
    Table bgpStateTable(&stateDb, STATE_BGP_TABLE_NAME);

    NetLink netlink;

    netlink.registerGroup(RTNLGRP_LINK);

    NetDispatcher::getInstance().registerMessageHandler(RTM_NEWROUTE, &sync);
    NetDispatcher::getInstance().registerMessageHandler(RTM_DELROUTE, &sync);
    NetDispatcher::getInstance().registerMessageHandler(RTM_NEWLINK, &sync);
    NetDispatcher::getInstance().registerMessageHandler(RTM_DELLINK, &sync);

    rtnl_route_read_protocol_names(DefaultRtProtoPath);
    nlmsg_set_default_size(FPM_MAX_MSG_LEN);

    std::string suppressionEnabledStr;
    deviceMetadataTable.hget("localhost", "suppress-fib-pending", suppressionEnabledStr);
    if (suppressionEnabledStr == "enabled")
    {
        routeResponseChannel = std::make_unique<NotificationConsumer>(&applStateDb, routeResponseChannelName);
        sync.setSuppressionEnabled(true);
    }

    while (true)
    {
        try
        {
            FpmLink fpm(&sync);

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
            s.addSelectable(&netlink);
            s.addSelectable(&deviceMetadataTableSubscriber);

            if (sync.isSuppressionEnabled())
            {
                s.addSelectable(routeResponseChannel.get());
            }

            /* If warm-restart feature is enabled, execute 'restoration' logic */
            bool warmStartEnabled = sync.m_warmStartHelper->checkAndStart();
            if (warmStartEnabled)
            {
                /* Obtain warm-restart timer defined for routing application */
                time_t warmRestartIval = sync.m_warmStartHelper->getRestartTimer();
                if (!warmRestartIval)
                {
                    warmStartTimer.setInterval(timespec{DEFAULT_ROUTING_RESTART_INTERVAL, 0});
                }
                else
                {
                    warmStartTimer.setInterval(timespec{warmRestartIval, 0});
                }

                /* Execute restoration instruction and kick off warm-restart timer */
                if (sync.m_warmStartHelper->runRestoration())
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
            else
            {
                sync.m_warmStartHelper->setState(WarmStart::WSDISABLED);
            }

            gSelectTimeout = INFINITE;

            while (true)
            {
                Selectable *temps;

                /* Reading FPM messages forever (and calling "readMe" to read them) */
                s.select(&temps, gSelectTimeout);

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

                    sync.onWarmStartEnd(applStateDb);

                    // remove the one-shot timer.
                    s.removeSelectable(temps);
                    pipeline.flush();
                    SWSS_LOG_DEBUG("Pipeline flushed");
                }
                else if (temps == &eoiuCheckTimer)
                {
                    if (sync.m_warmStartHelper->inProgress())
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
                else if (temps == &deviceMetadataTableSubscriber)
                {
                    std::deque<KeyOpFieldsValuesTuple> keyOpFvsQueue;
                    deviceMetadataTableSubscriber.pops(keyOpFvsQueue);

                    for (const auto& keyOpFvs: keyOpFvsQueue)
                    {
                        const auto& key = kfvKey(keyOpFvs);
                        const auto& op = kfvOp(keyOpFvs);
                        const auto& fvs = kfvFieldsValues(keyOpFvs);

                        if (op != SET_COMMAND)
                        {
                            continue;
                        }

                        if (key != "localhost")
                        {
                            continue;
                        }

                        for (const auto& fv: fvs)
                        {
                            const auto& field = fvField(fv);
                            const auto& value = fvValue(fv);

                            if (field != "suppress-fib-pending")
                            {
                                continue;
                            }

                            bool shouldEnable = (value == "enabled");

                            if (shouldEnable && !sync.isSuppressionEnabled())
                            {
                                routeResponseChannel = std::make_unique<NotificationConsumer>(&applStateDb, routeResponseChannelName);
                                sync.setSuppressionEnabled(true);
                                s.addSelectable(routeResponseChannel.get());
                            }
                            else if (!shouldEnable && sync.isSuppressionEnabled())
                            {
                                /* When disabling suppression we mark all existing routes offloaded in zebra
                                 * as there could be some transient routes which are pending response from
                                 * orchagent, thus such updates might be missing. Since we are disabling suppression
                                 * we no longer care about real HW offload status and can mark all routes as offloaded
                                 * to avoid routes stuck in suppressed state after transition. */
                                sync.markRoutesOffloaded(db);

                                sync.setSuppressionEnabled(false);
                                s.removeSelectable(routeResponseChannel.get());
                                routeResponseChannel.reset();
                            }
                        } // end for fvs
                    } // end for keyOpFvsQueue
                }
                else if (routeResponseChannel && (temps == routeResponseChannel.get()))
                {
                    std::deque<KeyOpFieldsValuesTuple> notifications;
                    routeResponseChannel->pops(notifications);

                    for (const auto& notification: notifications)
                    {
                        const auto& key = kfvKey(notification);
                        const auto& fieldValues = kfvFieldsValues(notification);

                        sync.onRouteResponse(key, fieldValues);
                    }
                }
                else if (!warmStartEnabled || sync.m_warmStartHelper->isReconciled())
                {
                    flushPipeline(pipeline);
                }
            }
        }
        catch (FpmLink::FpmConnectionClosedException &e)
        {
            cout << "Connection lost, reconnecting..." << endl;
        }
    }

    return 1;
}

void flushPipeline(RedisPipeline& pipeline) {

    size_t remaining = pipeline.size();

    if (remaining == 0) {
        gSelectTimeout = INFINITE;
        return;
    }

    int idle = pipeline.getIdleTime();

    // flush the pipeline if
    // 1. traffic is not scaled (only prevent fpmsyncd from flushing ppl too frequently in the scaled case)
    // 2. the idle time since last flush has exceeded gFlushTimeout
    // 3. idle <= 0, due to system clock drift, should not happen since we already use steady_clock for timing
    if (remaining < SMALL_TRAFFIC || idle >= gFlushTimeout || idle <= 0) {

        pipeline.flush();

        gSelectTimeout = INFINITE;

        SWSS_LOG_DEBUG("Pipeline flushed");
    }
    else
    {
        // skip flushing ppl and set the timeout of fpmsyncd select function to be (gFlushTimeout - idle)
        // so that fpmsyncd select function would block at most for (gFlushTimeout - idle)
        // by doing this, we make sure every entry eventually gets flushed
        gSelectTimeout = gFlushTimeout - idle;
    }
}
