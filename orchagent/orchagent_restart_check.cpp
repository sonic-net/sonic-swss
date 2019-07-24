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

    std::cout << "Usage: orchagent_restart_check [-s] " << std::endl;
    std::cout << "    -n --noFreeze" << std::endl;
    std::cout << "        Don't freeze orchagent even if check succeeded" << std::endl;
    std::cout << "    -s --skipPendingTaskCheck" << std::endl;
    std::cout << "        Skip pending task dependency check for orchagent" << std::endl;
    std::cout << "    -w --waitTime" << std::endl;
    std::cout << "        Wait time for response from orchagent, in milliseconds. Default value: 1000" << std::endl;
    std::cout << "    -r --retryCount" << std::endl;
    std::cout << "        Number of retries for the request to orchagent. Default value: 0" << std::endl;
    std::cout << "    -h --help:" << std::endl;
    std::cout << "        Print out this message" << std::endl;
}


/*
 * Before stopping orchagent for warm restart, basic state check is preferred to
 * ensure orchagent is not in transient state, so a deterministic state may be restored after restart.
 *
 * Here is to implement orchagent_restart_check binary which may talk to orchagent and
 * ask it to do self-check, return "READY " signal and freeze if everything is ok,
 * otherwise "NOT_READY" signal should be returned.
 *
 * Optionally:
 *            if --noFreeze option is provided, orchagent won't freeze.
 *            if --skipPendingTaskCheck option is provided, orchagent won't use
 *                 whether there is pending task existing as state check criterion.
 */
int main(int argc, char **argv)
{
    swss::Logger::getInstance().setMinPrio(swss::Logger::SWSS_INFO);
    SWSS_LOG_ENTER();

    std::string skipPendingTaskCheck = "false";
    std::string noFreeze            = "false";
    /* Default wait time is 1000 millisecond */
    int waitTime = 1000;
    int retryCount = 0;

    const char* const optstring = "nsw:r:";
    while(true)
    {
        static struct option long_options[] =
        {
            { "noFreeze",                no_argument,       0, 'n' },
            { "skipPendingTaskCheck",    no_argument,       0, 's' },
            { "retryCount",              required_argument, 0, 'r' },
            { "waitTime",                required_argument, 0, 'w' }
        };

        int option_index = 0;

        int c = getopt_long(argc, argv, optstring, long_options, &option_index);

        if (c == -1)
        {
            break;
        }

        switch (c)
        {
            case 'n':
                SWSS_LOG_NOTICE("Won't freeze orchagent even if check succeeded");
                noFreeze = "true";
                break;
            case 's':
                SWSS_LOG_NOTICE("Skipping pending task check for orchagent");
                skipPendingTaskCheck = "true";
                break;
            case 'w':
                SWSS_LOG_NOTICE("Wait time for response from orchagent set to %s milliseconds", optarg);
                waitTime = atoi(optarg);
                break;
            case 'r':
                SWSS_LOG_NOTICE("Number of retries for the request to orchagent is set to %s", optarg);
                retryCount = atoi(optarg);
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

    swss::DBConnector db(APPL_DB, swss::DBConnector::DEFAULT_UNIXSOCKET, 0);
    // Send warm restart query via "RESTARTCHECK" notification channel
    swss::NotificationProducer restartQuery(&db, "RESTARTCHECK");
    // Will listen for the reply on "RESTARTCHECKREPLY" channel
    swss::NotificationConsumer restartQueryReply(&db, "RESTARTCHECKREPLY");

    swss::Select s;
    s.addSelectable(&restartQueryReply);
    swss::Selectable *sel;

    std::vector<swss::FieldValueTuple> values;
    values.emplace_back("NoFreeze", noFreeze);
    values.emplace_back("SkipPendingTaskCheck", skipPendingTaskCheck);
    std::string op = "orchagent";

    int retries = 0;

    while (retries <= retryCount)
    {
        SWSS_LOG_NOTICE("requested %s to do warm restart state check, retry count: %d", op.c_str(), retries);
        restartQuery.send(op, op, values);

        std::string op_ret, data;
        std::vector<swss::FieldValueTuple> values_ret;
        int result = s.select(&sel, waitTime);
        if (result == swss::Select::OBJECT)
        {
            restartQueryReply.pop(op_ret, data, values_ret);
            if (data == "READY")
            {
                SWSS_LOG_NOTICE("RESTARTCHECK success, %s is frozen and ready for warm restart", op_ret.c_str());
                std::cout << "RESTARTCHECK succeeded" << std::endl;
                return EXIT_SUCCESS;
            }
            else
            {
                SWSS_LOG_NOTICE("RESTARTCHECK failed, %s is not ready for warm restart with status %s",
                        op_ret.c_str(), data.c_str());
            }
        }
        else if (result == swss::Select::TIMEOUT)
        {
            SWSS_LOG_NOTICE("RESTARTCHECK for %s timed out", op_ret.c_str());
        }
        else
        {
            SWSS_LOG_NOTICE("RESTARTCHECK for %s error", op_ret.c_str());
        }
        retries++;
        values_ret.clear();
    }
    std::cout << "RESTARTCHECK failed" << std::endl;
    return EXIT_FAILURE;
}
