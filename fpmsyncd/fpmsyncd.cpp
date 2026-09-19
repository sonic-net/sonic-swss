#include <iostream>
#include <inttypes.h>
#include <unistd.h>
#include <cstdlib>
#include "logger.h"
#include "select.h"
#include "selectabletimer.h"
#include "netdispatcher.h"
#include "netlink.h"
#include "notificationconsumer.h"
#include "notificationproducer.h"
#include "warmRestartHelper.h"
#include "fpmsyncd/fpmlink.h"
#include "fpmsyncd/fpmsyncd.h"
#include "fpmsyncd/routesync.h"
#include "fpmsyncd/warmreboot_handler.h"

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

#define STATE_FIB_SUPPRESS_TABLE_NAME "FIB_SUPPRESS_TABLE"

/* Drain auto-resume timer fallback (seconds). Per-call override via the
 * tool's autoResumeTimeoutSec, CONFIG_DB override via
 * fpmsyncd_drain_auto_resume_sec. */
#define DRAIN_AUTO_RESUME_DEFAULT_INTERVAL_SECONDS 30

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

    // Parse command-line options
    int opt;
    while ((opt = getopt(argc, argv, "ep")) != -1)
    {
        switch (opt)
        {
        case 'e':
            // WS8: enable conflated-hash route channel (producer side).
            // Sets env var checked by createProducerStateTable factory.
            setenv("ROUTE_CONFLATED_CHANNEL_CLI", "true", 1);
            SWSS_LOG_NOTICE("Conflated-hash route channel enabled (-e)");
            break;
        case 'p':
            // WS2: enable msgpack on ASIC_DB (fpmsyncd doesn't use ASIC_DB,
            // but accept the flag for consistency — no-op here).
            break;
        default:
            break;
        }
    }

    const auto routeResponseChannelName = std::string("APPL_DB_") + APP_ROUTE_TABLE_NAME + "_RESPONSE_CHANNEL";

    DBConnector db("APPL_DB", 0);
    DBConnector cfgDb("CONFIG_DB", 0);
    Table deviceMetadataTable(&cfgDb, CFG_DEVICE_METADATA_TABLE_NAME);
    DBConnector applStateDb("APPL_STATE_DB", 0);
    std::unique_ptr<NotificationConsumer> routeResponseChannel;

    RedisPipeline pipeline(&db, ROUTE_SYNC_PPL_SIZE);
    RouteSync sync(&pipeline);

    DBConnector stateDb("STATE_DB", 0);
    Table bgpStateTable(&stateDb, STATE_BGP_TABLE_NAME);

    /* WARM_RESTART_TABLE|fpmsyncd drain_state / drain_queue_size are
     * written by the drain-barrier handler. The `state` field is owned
     * by WarmStartHelper and left alone. */
    Table warmRestartStateTable(&stateDb, STATE_WARM_RESTART_TABLE_NAME);
    fpmsyncd_warmreboot::clearStaleDrainStateFields(warmRestartStateTable);
    SWSS_LOG_NOTICE("fpmsyncd: cleared stale drain_state / drain_queue_size / queueSize from STATE_DB WARM_RESTART_TABLE|fpmsyncd");

    NotificationConsumer restartCheckConsumer(&db, "FPMSYNCD_RESTARTCHECK");
    NotificationProducer restartCheckReply(&db, "FPMSYNCD_RESTARTCHECKREPLY");

    NetLink netlink;

    netlink.registerGroup(RTNLGRP_LINK);

    NetDispatcher::getInstance().registerMessageHandler(RTM_NEWROUTE, &sync);
    NetDispatcher::getInstance().registerMessageHandler(RTM_DELROUTE, &sync);
    NetDispatcher::getInstance().registerMessageHandler(RTM_NEWLINK, &sync);
    NetDispatcher::getInstance().registerMessageHandler(RTM_DELLINK, &sync);

    rtnl_route_read_protocol_names(DefaultRtProtoPath);
    nlmsg_set_default_size(FPM_MAX_MSG_LEN);

    std::string suppressionEnabledStr;
    Table fibSuppressTable(&stateDb, STATE_FIB_SUPPRESS_TABLE_NAME);
    fibSuppressTable.hget("system", "oper_state", suppressionEnabledStr);
    if (suppressionEnabledStr == "enabled")
    {
        routeResponseChannel = std::make_unique<NotificationConsumer>(&applStateDb, routeResponseChannelName);
        sync.setSuppressionEnabled(true);
    }
    SWSS_LOG_NOTICE("FIB suppression oper_state: %s", suppressionEnabledStr.c_str());

    std::string flushTimeoutStr;
    deviceMetadataTable.hget("localhost", "fpmsyncd_flush_timeout", flushTimeoutStr);
    if (!flushTimeoutStr.empty() && flushTimeoutStr != "None")
    {
        try
        {
            int val = std::stoi(flushTimeoutStr);
            if (val >= 0 && val <= 5000)
            {
                gFlushTimeout = val;
                SWSS_LOG_NOTICE("fpmsyncd_flush_timeout set to %d ms from CONFIG_DB", val);
            }
        }
        catch (const std::exception& e)
        {
            SWSS_LOG_WARN("Invalid fpmsyncd_flush_timeout value: %s", flushTimeoutStr.c_str());
        }
    }

    /* WS6: fpmsyncd_batch_size — accumulate routes and flush as one batch.
     * 0 = disabled (old per-route path). Default: 512. Cap: 4096. */
    std::string batchSizeStr;
    deviceMetadataTable.hget("localhost", "fpmsyncd_batch_size", batchSizeStr);
    if (!batchSizeStr.empty() && batchSizeStr != "None")
    {
        try
        {
            int val = std::stoi(batchSizeStr);
            if (val == 0)
            {
                SWSS_LOG_NOTICE("fpmsyncd_batch_size=0: batching disabled (per-route path)");
            }
            else if (val >= 2 && val <= 4096)
            {
                sync.setBatchSize(val);
                SWSS_LOG_NOTICE("fpmsyncd_batch_size set to %d from CONFIG_DB", val);
            }
            else
            {
                SWSS_LOG_WARN("fpmsyncd_batch_size out of range [2,4096]: %d, using default (disabled)", val);
            }
        }
        catch (const std::exception& e)
        {
            SWSS_LOG_WARN("Invalid fpmsyncd_batch_size value: %s", batchSizeStr.c_str());
        }
    }

    /* Auto-resume default: tool -t > CONFIG_DB > compile-time. */
    int gDrainAutoResumeSec = DRAIN_AUTO_RESUME_DEFAULT_INTERVAL_SECONDS;
    {
        std::string drainAutoResumeStr;
        deviceMetadataTable.hget("localhost", "fpmsyncd_drain_auto_resume_sec", drainAutoResumeStr);
        auto p = fpmsyncd_warmreboot::parseDrainAutoResumeSec(
            drainAutoResumeStr, gDrainAutoResumeSec, /*minSec=*/1, /*maxSec=*/600);
        gDrainAutoResumeSec = p.value;
        using Source = fpmsyncd_warmreboot::DrainAutoResumeSecParse::Source;
        switch (p.source)
        {
        case Source::ConfigDb:
            SWSS_LOG_NOTICE("fpmsyncd_drain_auto_resume_sec set to %d s from CONFIG_DB", p.rawParsed);
            break;
        case Source::OutOfRange:
            SWSS_LOG_WARN("fpmsyncd_drain_auto_resume_sec out of range [1, 600]: %d (using default %d)",
                          p.rawParsed, gDrainAutoResumeSec);
            break;
        case Source::Invalid:
            SWSS_LOG_WARN("Invalid fpmsyncd_drain_auto_resume_sec value: %s", drainAutoResumeStr.c_str());
            break;
        case Source::Default:
            break;
        }
    }
    SWSS_LOG_NOTICE("fpmsyncd: drain auto-resume default = %ds (override per-call via FPMSYNCD_RESTARTCHECK autoResumeTimeoutSec)", gDrainAutoResumeSec);

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
            // Drain auto-resume timer; armed by the drain-barrier handler.
            SelectableTimer drainAutoResumeTimer(timespec{0, 0});
           
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
            s.addSelectable(&restartCheckConsumer);
            if (sync.isSuppressionEnabled())
            {
                s.addSelectable(routeResponseChannel.get());
            }

            /* If warm-restart feature is enabled, execute 'restoration' logic */
            bool warmStartEnabled = sync.getWarmStartHelper().checkAndStart();
            if (warmStartEnabled)
            {
                /* Obtain warm-restart timer defined for routing application */
                time_t warmRestartIval = sync.getWarmStartHelper().getRestartTimer();
                if (!warmRestartIval)
                {
                    warmStartTimer.setInterval(timespec{DEFAULT_ROUTING_RESTART_INTERVAL, 0});
                }
                else
                {
                    warmStartTimer.setInterval(timespec{warmRestartIval, 0});
                }

                /* Execute restoration instruction and kick off warm-restart timer */
                if (sync.getWarmStartHelper().runRestoration())
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
                sync.getWarmStartHelper().setState(WarmStart::WSDISABLED);
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

                    // WS6: flush pending batches before reconciliation
                    sync.flushPendingRoutes();
                    sync.flushRouteTables();
                    sync.onWarmStartEnd(applStateDb);

                    // remove the one-shot timer.
                    s.removeSelectable(temps);
                    pipeline.flush();
                    SWSS_LOG_DEBUG("Pipeline flushed");
                }
                else if (temps == &eoiuCheckTimer)
                {
                    if (sync.getWarmStartHelper().inProgress())
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
                else if (temps == &drainAutoResumeTimer)
                {
                    /* Aborted warm-reboot: SIGKILL never came. Apply the
                     * timer-fire outcome (clear flag, disarm, STATE_DB,
                     * disconnect+throw). */
                    SWSS_LOG_WARN("fpmsyncd: drain auto-resume timer fired — warm-reboot was probably aborted. Clearing drain flag and forcing FPM disconnect to trigger zebra full-FIB re-dump.");

                    auto tfOut = fpmsyncd_warmreboot::handleDrainAutoResumeTimerFire(
                        sync.totalDbUpdaterQueueSize());

                    if (tfOut.clearDrainFlag)
                    {
                        sync.setDrainingForWarmRestart(false);
                    }
                    if (tfOut.disarmTimer)
                    {
                        s.removeSelectable(&drainAutoResumeTimer);
                    }
                    if (!tfOut.drainState.empty())
                    {
                        warmRestartStateTable.hset("fpmsyncd", "drain_state", tfOut.drainState);
                        warmRestartStateTable.hset("fpmsyncd", "drain_queue_size",
                                                   std::to_string(tfOut.drainQueueSize));
                    }
                    if (tfOut.forceReconnect)
                    {
                        fpm.forceDisconnect();
                        /* Explicit throw: a closed fd doesn't reliably wake
                         * select(). Unwinds to the outer catch, which
                         * re-accepts and lets zebra reconnect + re-dump. */
                        throw FpmLink::FpmConnectionClosedException();
                    }
                }
                else if (temps == &restartCheckConsumer)
                {
                    /* Drain-barrier handler. Decision logic in
                     * fpmsyncd_warmreboot::handleRestartCheck; apply block
                     * below. */
                    std::string op, data;
                    std::vector<FieldValueTuple> values;
                    restartCheckConsumer.pop(op, data, values);

                    auto req = fpmsyncd_warmreboot::parseRestartCheckValues(values);

                    const bool   hasZmq = sync.hasZmqProducerTables();
                    const size_t qsize  = sync.totalDbUpdaterQueueSize();
                    const bool   wasDraining = sync.isDrainingForWarmRestart();

                    SWSS_LOG_NOTICE("fpmsyncd: %s (received %s on FPMSYNCD_RESTARTCHECK, hasZmqProducerTables=%s, queueSize=%zu, resume=%s)",
                                    req.resumeRequested ? "explicit resume requested" : "preparing for warm boot",
                                    op.c_str(), hasZmq ? "true" : "false", qsize,
                                    req.resumeRequested ? "true" : "false");

                    auto out = fpmsyncd_warmreboot::handleRestartCheck(
                        req, hasZmq, qsize, wasDraining, gDrainAutoResumeSec);

                    // Apply outcome: drain flag → timer → STATE_DB → reply → reconnect.
                    using DrainFlag   = fpmsyncd_warmreboot::RestartCheckOutcome::DrainFlag;
                    using TimerAction = fpmsyncd_warmreboot::RestartCheckOutcome::TimerAction;
                    if (out.drainFlag == DrainFlag::Set)
                    {
                        // WS6: flush pending batches before setting drain flag
                        sync.flushPendingRoutes();
                        sync.flushRouteTables();
                        sync.setDrainingForWarmRestart(true);
                        SWSS_LOG_NOTICE("fpmsyncd: drain flag set; new route SET/DEL via setRouteWithWarmRestart / delWithWarmRestart will be dropped");
                    }
                    else if (out.drainFlag == DrainFlag::Clear)
                    {
                        sync.setDrainingForWarmRestart(false);
                        SWSS_LOG_WARN("fpmsyncd: explicit resume — clearing drain flag and forcing FPM disconnect to trigger zebra full-FIB re-dump.");
                    }

                    if (out.timerAction == TimerAction::ArmFirst ||
                        out.timerAction == TimerAction::ReArm)
                    {
                        drainAutoResumeTimer.setInterval(timespec{out.autoResumeSec, 0});
                        drainAutoResumeTimer.reset();
                        drainAutoResumeTimer.start();
                        if (out.timerAction == TimerAction::ArmFirst)
                        {
                            s.addSelectable(&drainAutoResumeTimer);
                            SWSS_LOG_NOTICE("fpmsyncd: drain auto-resume timer armed for %ds", out.autoResumeSec);
                        }
                        else
                        {
                            SWSS_LOG_INFO("fpmsyncd: re-notification during drain; auto-resume timer re-armed for %ds", out.autoResumeSec);
                        }
                    }
                    else if (out.timerAction == TimerAction::Disarm)
                    {
                        s.removeSelectable(&drainAutoResumeTimer);
                    }

                    if (!out.drainState.empty())
                    {
                        warmRestartStateTable.hset("fpmsyncd", "drain_state",      out.drainState);
                        warmRestartStateTable.hset("fpmsyncd", "drain_queue_size", std::to_string(out.drainQueueSize));

                        if (out.drainState == "ready")
                        {
                            SWSS_LOG_NOTICE("fpmsyncd: ready for warm boot (STATE_DB WARM_RESTART_TABLE|fpmsyncd drain_state=ready drain_queue_size=0)");
                        }
                        else if (out.drainState == "draining")
                        {
                            SWSS_LOG_NOTICE("fpmsyncd: still draining (STATE_DB WARM_RESTART_TABLE|fpmsyncd drain_state=draining drain_queue_size=%zu)", out.drainQueueSize);
                        }
                    }
                    else if (req.resumeRequested)
                    {
                        // Resume no-op: handler returns empty drainState when not draining.
                        SWSS_LOG_NOTICE("fpmsyncd: explicit resume requested but not in drain mode — no-op (FPM connection preserved)");
                    }

                    restartCheckReply.send("fpmsyncd", out.replyOp, out.replyValues);

                    if (out.forceReconnect)
                    {
                        fpm.forceDisconnect();
                        throw FpmLink::FpmConnectionClosedException();
                    }
                }
                else if (!warmStartEnabled || sync.getWarmStartHelper().isReconciled())
                {
                    // WS6: flush pending route batches before the pipeline flush.
                    // Batched EVALSHAs land in the pipeline → pipeline flush ships them.
                    sync.flushPendingRoutes();
                    // WS10: route tables may be on a separate pipeline (APPL_CHANNEL_DB).
                    // flushPipeline only flushes the APPL_DB pipeline; this flushes
                    // each route table's own pipeline so channel writes reach redis.
                    sync.flushRouteTables();
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
