#include <iostream>
#include <inttypes.h>
#include <getopt.h>
#include "logger.h"
#include "select.h"
#include "selectabletimer.h"
#include "netdispatcher.h"
#include "netlink.h"
#include "notificationconsumer.h"
#include "subscriberstatetable.h"
#include "zmqclient.h"
#include "warmRestartHelper.h"
#include "fpmsyncd/fpmlink.h"
#include "fpmsyncd/routesync.h"

#include <netlink/route/route.h>

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

struct Options
{
    int m_zmq_port = 0;
};

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

void parseArguments(
    int argc,
    char** argv,
    Options &options)
{
    // Parse argument with getopt https://man7.org/linux/man-pages/man3/getopt.3.html
    const char* short_options = "z";
    static struct option long_options[] = {
       {"zmqport",     optional_argument, NULL,  'z' },
       // The last element of the array has to be filled with zeros.
       {0,          0,       0,  0 }
    };
    
    // prevent getopt_long print "invalid option" message.
    opterr = 0;
    while(optind < argc)
    {
        int opt = getopt_long(argc, argv, short_options, long_options, NULL);
        if (opt == -1)
        {
            continue;
        }

        switch (opt) {
            case 'z':
                if (optind < argc)
                {
                    options.m_zmq_port = atoi(argv[optind]);
                    optind++;
                }
                else
                {
                    throw invalid_argument("zmq port value is missing.");
                }
                break;

            default:
                // argv contains unknown argument
                throw invalid_argument("Unknown argument:" + string(argv[optind]));
        }
    }
}

int main(int argc, char **argv)
{
    swss::Logger::linkToDbNative("fpmsyncd");

    // parse parameter
    Options options;
    try
    {
        parseArguments(argc, argv, options);
    }
    catch (invalid_argument const& e)
    {
        cerr << "Paramet: " << e.what() << endl;
        return -1;
    }
    catch (logic_error const& e)
    {
        // getopt_long throw logic_error when found a unknown option without value.
        cerr << "Unknown option without value: "  << e.what() << endl;
        return -1;
    }

    const auto routeResponseChannelName = std::string("APPL_DB_") + APP_ROUTE_TABLE_NAME + "_RESPONSE_CHANNEL";

    DBConnector db("APPL_DB", 0);
    DBConnector cfgDb("CONFIG_DB", 0);
    SubscriberStateTable deviceMetadataTableSubscriber(&cfgDb, CFG_DEVICE_METADATA_TABLE_NAME);
    Table deviceMetadataTable(&cfgDb, CFG_DEVICE_METADATA_TABLE_NAME);
    DBConnector applStateDb("APPL_STATE_DB", 0);
    std::unique_ptr<NotificationConsumer> routeResponseChannel;

    std::shared_ptr<ZmqClient> zmqClient = nullptr;
    if (options.m_zmq_port != 0)
    {
        char address[100];
        snprintf(address, sizeof(address), "tcp://127.0.0.1:%d", options.m_zmq_port);
        SWSS_LOG_NOTICE("fpmsyncd start with ZMQ enabled, zmq address: %s", address);
        zmqClient = std::make_shared<ZmqClient>(address);
    }

    RedisPipeline pipeline(&db);
    RouteSync sync(&pipeline, zmqClient);


    DBConnector stateDb("STATE_DB", 0);
    Table bgpStateTable(&stateDb, STATE_BGP_TABLE_NAME);

    NetLink netlink;

    netlink.registerGroup(RTNLGRP_LINK);

    NetDispatcher::getInstance().registerMessageHandler(RTM_NEWROUTE, &sync);
    NetDispatcher::getInstance().registerMessageHandler(RTM_DELROUTE, &sync);
    NetDispatcher::getInstance().registerMessageHandler(RTM_NEWLINK, &sync);
    NetDispatcher::getInstance().registerMessageHandler(RTM_DELLINK, &sync);

    rtnl_route_read_protocol_names(DefaultRtProtoPath);

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
            cout << "Exception \"" << e.what() << "\" had been thrown in daemon" << endl;
            return 0;
        }
    }

    return 1;
}
