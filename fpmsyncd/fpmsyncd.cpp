#include <iostream>
#include <inttypes.h>
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

/* Iteration 4: auto-resume timer. If warm-reboot is aborted after fpmsyncd
 * entered drain mode (e.g., orchagent_restart_check failed and --force
 * not set), this timer fires, clears the drain flag, removes itself from
 * the Select set, and triggers a forced FPM disconnect so zebra re-dumps
 * its full FIB. Without this, fpmsyncd would stay in drain mode forever
 * and silently drop route updates from FRR.
 *
 * Default value; can be overridden per-call by the tool via the
 * "autoResumeTimeoutSec" field in the FPMSYNCD_RESTARTCHECK notification
 * payload. */
#define DRAIN_AUTO_RESUME_DEFAULT_INTERVAL_SECONDS 20

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
    Table deviceMetadataTable(&cfgDb, CFG_DEVICE_METADATA_TABLE_NAME);
    DBConnector applStateDb("APPL_STATE_DB", 0);
    std::unique_ptr<NotificationConsumer> routeResponseChannel;

    RedisPipeline pipeline(&db, ROUTE_SYNC_PPL_SIZE);
    RouteSync sync(&pipeline);

    DBConnector stateDb("STATE_DB", 0);
    Table bgpStateTable(&stateDb, STATE_BGP_TABLE_NAME);

    /* Warm-restart drain-barrier channels and STATE_DB row. The tool
     * fpmsyncd_restart_check sends on FPMSYNCD_RESTARTCHECK; fpmsyncd
     * replies on FPMSYNCD_RESTARTCHECKREPLY. For operator visibility, the
     * handler also writes two fields into WARM_RESTART_TABLE|fpmsyncd:
     *   drain_state ∈ {draining, ready, auto_resumed}
     *   drain_queue_size = <integer>
     * These use a `drain_` prefix to namespace them away from the existing
     * `state` field that fpmsyncd's WarmStartHelper writes during a real
     * post-warm-boot reconciliation cycle (initialized/restored/reconciled).
     *
     * On fpmsyncd startup we HDEL any stale drain_* fields left over from
     * a prior process instance so operators don't see misleading data when
     * drain is not actually in progress.
     */
    Table warmRestartStateTable(&stateDb, STATE_WARM_RESTART_TABLE_NAME);
    warmRestartStateTable.hdel("fpmsyncd", "drain_state");
    warmRestartStateTable.hdel("fpmsyncd", "drain_queue_size");
    /* One-time migration cleanup: previous fpmsyncd builds wrote the
     * drain barrier's status into the bare `queueSize` field. Remove
     * that stale field too. The `state` field is intentionally left
     * alone — fpmsyncd's own WarmStartHelper owns it for the
     * initialized/restored/reconciled post-warm-boot reconciliation
     * state machine. */
    warmRestartStateTable.hdel("fpmsyncd", "queueSize");
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
            // Iteration 4: drain auto-resume timer. Armed when the
            // FPMSYNCD_RESTARTCHECK handler flips the drain flag.
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
                    /* Iteration 4: 60s elapsed since drain mode entered and
                     * fpmsyncd is still alive — warm-reboot must have been
                     * aborted somewhere between fpmsyncd_restart_check READY
                     * and SIGKILL. Clear the drain flag (so new route
                     * SET/DEL flow normally again) and force an FPM
                     * disconnect — zebra re-dumps its full FIB on reconnect,
                     * recovering whatever route updates were dropped while
                     * we were draining. */
                    SWSS_LOG_WARN("fpmsyncd: drain auto-resume timer fired — warm-reboot was probably aborted. Clearing drain flag and forcing FPM disconnect to trigger zebra full-FIB re-dump.");
                    sync.setDrainingForWarmRestart(false);
                    s.removeSelectable(&drainAutoResumeTimer);

                    /* Reflect the runtime change in STATE_DB so operators
                     * inspecting WARM_RESTART_TABLE|fpmsyncd see that an
                     * auto-resume happened rather than the stale last-seen
                     * drain state. */
                    size_t qsizeNow = sync.totalDbUpdaterQueueSize();
                    warmRestartStateTable.hset("fpmsyncd", "drain_state", "auto_resumed");
                    warmRestartStateTable.hset("fpmsyncd", "drain_queue_size", std::to_string(qsizeNow));

                    fpm.forceDisconnect();
                    /* Throw to enter the existing outer-catch reconnect path
                     * (constructs a new FpmLink, blocks on accept() until
                     * zebra reconnects, which causes zebra to re-dump its
                     * full FIB via FPM). Without this explicit throw, the
                     * closed fd doesn't reliably surface as readable to
                     * select(), so readData() is never called and the
                     * normal reconnection chain doesn't fire. */
                    throw FpmLink::FpmConnectionClosedException();
                }
                else if (temps == &restartCheckConsumer)
                {
                    /* Iteration 3: gate READY on AsyncDBUpdater queue actually
                     * draining to zero. Reply NOT_READY queueSize=N while
                     * pending; tool's -r/-w retry loop drives polling. STATE_DB
                     * reflects the same current state so 'show warm-restart
                     * state' and `sonic-db-cli` give operators visibility. */
                    std::string op, data;
                    std::vector<FieldValueTuple> values;
                    restartCheckConsumer.pop(op, data, values);

                    bool hasZmq = sync.hasZmqProducerTables();
                    size_t qsize = sync.totalDbUpdaterQueueSize();

                    /* Parse caller-supplied options from notification values:
                     *   autoResumeTimeoutSec — drain auto-resume interval
                     *   resume — if "true", immediately resume (skip drain,
                     *            force FPM reconnect now). Used by the tool's
                     *            -R / --resume flag for warm-reboot abort
                     *            recovery without waiting for the timer. */
                    int autoResumeSec = DRAIN_AUTO_RESUME_DEFAULT_INTERVAL_SECONDS;
                    bool resumeRequested = false;
                    for (const auto& fv : values)
                    {
                        if (fvField(fv) == "autoResumeTimeoutSec")
                        {
                            int parsed = atoi(fvValue(fv).c_str());
                            if (parsed > 0) autoResumeSec = parsed;
                        }
                        else if (fvField(fv) == "resume" && fvValue(fv) == "true")
                        {
                            resumeRequested = true;
                        }
                    }

                    SWSS_LOG_NOTICE("fpmsyncd: %s (received %s on FPMSYNCD_RESTARTCHECK, hasZmqProducerTables=%s, queueSize=%zu, resume=%s)",
                                    resumeRequested ? "explicit resume requested" : "preparing for warm boot",
                                    op.c_str(), hasZmq ? "true" : "false", qsize,
                                    resumeRequested ? "true" : "false");

                    /* Explicit resume path: caller (typically warm-reboot abort
                     * cleanup) wants us to immediately clear drain mode and
                     * force a zebra re-dump, instead of waiting up to
                     * autoResumeTimeoutSec for the timer to fire. Send the
                     * reply first, then re-use the same shutdown sequence
                     * as the auto-resume timer's branch. */
                    if (resumeRequested)
                    {
                        bool wasDraining = sync.isDrainingForWarmRestart();
                        sync.setDrainingForWarmRestart(false);
                        s.removeSelectable(&drainAutoResumeTimer);

                        warmRestartStateTable.hset("fpmsyncd", "drain_state", "resumed");
                        warmRestartStateTable.hset("fpmsyncd", "drain_queue_size", std::to_string(qsize));

                        SWSS_LOG_WARN("fpmsyncd: explicit resume — clearing drain flag (wasDraining=%s) and forcing FPM disconnect to trigger zebra full-FIB re-dump.",
                                      wasDraining ? "true" : "false");

                        std::vector<FieldValueTuple> reply{
                            FieldValueTuple{"queueSize", std::to_string(qsize)},
                            FieldValueTuple{"resumed", "true"}
                        };
                        restartCheckReply.send("fpmsyncd", "RESUMED", reply);

                        fpm.forceDisconnect();
                        throw FpmLink::FpmConnectionClosedException();
                    }

                    if (hasZmq && !sync.isDrainingForWarmRestart())
                    {
                        sync.setDrainingForWarmRestart(true);
                        SWSS_LOG_NOTICE("fpmsyncd: drain flag set; new route SET/DEL via setRouteWithWarmRestart / delWithWarmRestart will be dropped");

                        /* Iteration 4: arm the auto-resume timer. If warm-reboot
                         * is aborted before fpmsyncd is killed, this timer
                         * fires, clears the drain flag, and forces an FPM
                         * disconnect so zebra re-dumps its FIB. */
                        drainAutoResumeTimer.setInterval(timespec{autoResumeSec, 0});
                        drainAutoResumeTimer.reset();
                        drainAutoResumeTimer.start();
                        s.addSelectable(&drainAutoResumeTimer);
                        SWSS_LOG_NOTICE("fpmsyncd: drain auto-resume timer armed for %ds", autoResumeSec);
                    }
                    else if (hasZmq && sync.isDrainingForWarmRestart())
                    {
                        /* Re-notification during an active drain window:
                         * re-arm the timer so a fresh window starts.
                         * The timer is already in the Select set; just reset. */
                        drainAutoResumeTimer.setInterval(timespec{autoResumeSec, 0});
                        drainAutoResumeTimer.reset();
                        drainAutoResumeTimer.start();
                        SWSS_LOG_INFO("fpmsyncd: re-notification during drain; auto-resume timer re-armed for %ds", autoResumeSec);
                    }

                    const bool ready = (qsize == 0);
                    const char* drainState = ready ? "ready" : "draining";
                    warmRestartStateTable.hset("fpmsyncd", "drain_state", drainState);
                    warmRestartStateTable.hset("fpmsyncd", "drain_queue_size", std::to_string(qsize));

                    if (ready)
                    {
                        SWSS_LOG_NOTICE("fpmsyncd: ready for warm boot (STATE_DB WARM_RESTART_TABLE|fpmsyncd drain_state=ready drain_queue_size=0)");
                    }
                    else
                    {
                        SWSS_LOG_NOTICE("fpmsyncd: still draining (STATE_DB WARM_RESTART_TABLE|fpmsyncd drain_state=draining drain_queue_size=%zu)", qsize);
                    }

                    std::vector<FieldValueTuple> reply{
                        FieldValueTuple{"queueSize", std::to_string(qsize)}
                    };
                    restartCheckReply.send("fpmsyncd", ready ? "READY" : "NOT_READY", reply);
                }
                else if (!warmStartEnabled || sync.getWarmStartHelper().isReconciled())
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
