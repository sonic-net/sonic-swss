#include <iostream>
#include <sstream>

#include <unistd.h>
#include <getopt.h>

#include "notificationproducer.h"
#include "notificationconsumer.h"
#include "select.h"
#include "logger.h"


void printUsage()
{
    SWSS_LOG_ENTER();

    std::cout << "Usage: fpmsyncd_restart_check [options]" << std::endl;
    std::cout << "    -w --waitTime" << std::endl;
    std::cout << "        Per-attempt wait for reply on FPMSYNCD_RESTARTCHECKREPLY, in milliseconds." << std::endl;
    std::cout << "        fpmsyncd typically replies in <1 ms; this is a backstop. Default: 500" << std::endl;
    std::cout << "    -r --retryCount" << std::endl;
    std::cout << "        Number of additional attempts after the first. Default: 19 (20 total)." << std::endl;
    std::cout << "    -i --interSleep" << std::endl;
    std::cout << "        Sleep between attempts, in milliseconds. Paces the polling so" << std::endl;
    std::cout << "        AsyncDBUpdater has time to drain between checks. Default: 500" << std::endl;
    std::cout << "    -t --autoResumeTimeoutSec" << std::endl;
    std::cout << "        Auto-resume timeout, in seconds. If warm-reboot is aborted after" << std::endl;
    std::cout << "        fpmsyncd entered drain mode, fpmsyncd auto-clears the drain flag" << std::endl;
    std::cout << "        and forces an FPM reconnect this many seconds after the last" << std::endl;
    std::cout << "        notification. Sent to fpmsyncd in the notification payload." << std::endl;
    std::cout << "        Default: 20" << std::endl;
    std::cout << "    -h --help:" << std::endl;
    std::cout << "        Print out this message" << std::endl;
    std::cout << "" << std::endl;
    std::cout << "Defaults give a ~10s drain budget under happy path (20 attempts * 500ms sleep)." << std::endl;
}


/*
 * Before warm reboot freezes orchagent, this tool asks fpmsyncd to prepare for
 * warm reboot. With the ZMQ route fast path enabled, fpmsyncd's
 * ZmqProducerStateTable mirrors APPL_DB asynchronously via AsyncDBUpdater;
 * queued entries are lost at SIGKILL, leaving APPL_DB out of sync with ASIC_DB
 * so post-warm-boot apply_view removes valid routes. This tool sends a
 * notification on FPMSYNCD_RESTARTCHECK; fpmsyncd's handler updates STATE_DB
 * (WARM_RESTART_TABLE|fpmsyncd: state=ready) and replies READY on
 * FPMSYNCD_RESTARTCHECKREPLY.
 *
 * Iteration 1: notification round-trip + STATE_DB update + log markers only.
 * No drain flag, no AsyncDBUpdater queue check, no auto-resume timer.
 */
int main(int argc, char **argv)
{
    swss::Logger::getInstance().setMinPrio(swss::Logger::SWSS_INFO);
    SWSS_LOG_ENTER();

    /* Defaults: 20 attempts (retryCount=19), 500ms reply wait per attempt,
     * 500ms sleep between attempts, 20s auto-resume timeout. Happy-path
     * drain budget = 10 s. */
    int waitTime = 500;
    int retryCount = 19;
    int interSleepMs = 500;
    int autoResumeTimeoutSec = 20;

    const char* const optstring = "w:r:i:t:h";
    while (true)
    {
        static struct option long_options[] =
        {
            { "waitTime",             required_argument, 0, 'w' },
            { "retryCount",           required_argument, 0, 'r' },
            { "interSleep",           required_argument, 0, 'i' },
            { "autoResumeTimeoutSec", required_argument, 0, 't' },
            { "help",                 no_argument,       0, 'h' },
            { 0, 0, 0, 0 }
        };

        int option_index = 0;
        int c = getopt_long(argc, argv, optstring, long_options, &option_index);

        if (c == -1)
        {
            break;
        }

        switch (c)
        {
            case 'w':
                SWSS_LOG_NOTICE("Wait time for response from fpmsyncd set to %s milliseconds", optarg);
                waitTime = atoi(optarg);
                break;
            case 'r':
                SWSS_LOG_NOTICE("Number of retries for the request to fpmsyncd is set to %s", optarg);
                retryCount = atoi(optarg);
                break;
            case 'i':
                SWSS_LOG_NOTICE("Inter-attempt sleep set to %s milliseconds", optarg);
                interSleepMs = atoi(optarg);
                break;
            case 't':
                SWSS_LOG_NOTICE("Auto-resume timeout set to %s seconds", optarg);
                autoResumeTimeoutSec = atoi(optarg);
                break;
            case 'h':
                printUsage();
                exit(EXIT_SUCCESS);

            case '?':
                SWSS_LOG_WARN("unknown option %c", optopt);
                printUsage();
                exit(EXIT_FAILURE);

            default:
                SWSS_LOG_ERROR("getopt_long failure");
                exit(EXIT_FAILURE);
        }
    }

    swss::DBConnector db("APPL_DB", 0);
    swss::NotificationProducer restartQuery(&db, "FPMSYNCD_RESTARTCHECK");
    swss::NotificationConsumer restartQueryReply(&db, "FPMSYNCD_RESTARTCHECKREPLY");

    swss::Select s;
    s.addSelectable(&restartQueryReply);
    swss::Selectable *sel;

    std::vector<swss::FieldValueTuple> values{
        swss::FieldValueTuple{"autoResumeTimeoutSec", std::to_string(autoResumeTimeoutSec)}
    };
    std::string op = "fpmsyncd";

    auto findValue = [](const std::vector<swss::FieldValueTuple>& v, const std::string& key) -> std::string {
        for (const auto& fv : v)
        {
            if (fvField(fv) == key) return fvValue(fv);
        }
        return std::string();
    };

    int retries = 0;
    while (retries <= retryCount)
    {
        SWSS_LOG_NOTICE("requested %s warm-restart preparation, retry count: %d", op.c_str(), retries);
        restartQuery.send(op, op, values);

        std::string op_ret, data;
        std::vector<swss::FieldValueTuple> values_ret;
        int result = s.select(&sel, waitTime);
        if (result == swss::Select::OBJECT)
        {
            restartQueryReply.pop(op_ret, data, values_ret);
            const std::string qs = findValue(values_ret, "queueSize");
            const std::string qsStr = qs.empty() ? "" : (" queueSize=" + qs);
            std::cout << "FPMSYNCD_RESTARTCHECK retry " << retries
                      << ": " << data << qsStr << std::endl;
            if (data == "READY")
            {
                SWSS_LOG_NOTICE("FPMSYNCD_RESTARTCHECK success, %s is ready for warm restart (queueSize=%s)",
                                op_ret.c_str(), qs.empty() ? "0" : qs.c_str());
                std::cout << "FPMSYNCD_RESTARTCHECK succeeded" << std::endl;
                return EXIT_SUCCESS;
            }
            else
            {
                SWSS_LOG_NOTICE("FPMSYNCD_RESTARTCHECK retry %d: %s reports status %s queueSize=%s",
                                retries, op_ret.c_str(), data.c_str(), qs.empty() ? "?" : qs.c_str());
            }
        }
        else if (result == swss::Select::TIMEOUT)
        {
            std::cout << "FPMSYNCD_RESTARTCHECK retry " << retries << ": TIMEOUT" << std::endl;
            SWSS_LOG_NOTICE("FPMSYNCD_RESTARTCHECK for %s timed out", op.c_str());
        }
        else
        {
            std::cout << "FPMSYNCD_RESTARTCHECK retry " << retries << ": ERROR" << std::endl;
            SWSS_LOG_NOTICE("FPMSYNCD_RESTARTCHECK for %s error", op.c_str());
        }
        retries++;
        values_ret.clear();

        /* Pace the next attempt so AsyncDBUpdater has time to drain.
         * Skip the sleep before returning failure (last iteration). */
        if (retries <= retryCount && interSleepMs > 0)
        {
            usleep(static_cast<useconds_t>(interSleepMs) * 1000);
        }
    }
    std::cout << "FPMSYNCD_RESTARTCHECK failed" << std::endl;
    return EXIT_FAILURE;
}
