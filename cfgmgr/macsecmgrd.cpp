#include <unistd.h>
#include <signal.h>
#include <vector>
#include <sstream>
#include <fstream>
#include <iostream>
#include <mutex>
#include <algorithm>

#include <logger.h>
#include <producerstatetable.h>
#include <macaddress.h>
#include <exec.h>
#include <tokenize.h>
#include <shellcmd.h>
#include <warm_restart.h>
#include <select.h>

#include "macsecmgr.h"
#include "macsecpost.h"

using namespace std;
using namespace swss;

/* select() function timeout retry time, in millisecond */
#define SELECT_TIMEOUT 1000

MacAddress gMacAddress;

static volatile sig_atomic_t shutdown_requested = 0;

static void sig_handler(int)
{
    shutdown_requested = 1;
}

int main(int argc, char **argv)
{

    try
    {
        Logger::linkToDbNative("macsecmgrd");
        SWSS_LOG_NOTICE("--- Starting macsecmgrd ---");

        /* Leave cleanup to the main thread and MACsecMgr destructor. */
        struct sigaction sigact = {};
        sigact.sa_handler = sig_handler;
        if (sigemptyset(&sigact.sa_mask) ||
            sigaction(SIGTERM, &sigact, nullptr) ||
            sigaction(SIGINT, &sigact, nullptr))
        {
            SWSS_LOG_ERROR("failed to setup MACsec shutdown signal handlers");
            exit(EXIT_FAILURE);
        }

        swss::DBConnector cfgDb("CONFIG_DB", 0);
        swss::DBConnector stateDb("STATE_DB", 0);

        std::vector<std::string> cfg_macsec_tables = {
            CFG_MACSEC_PROFILE_TABLE_NAME,
            CFG_PORT_TABLE_NAME,
        };

        MACsecMgr macsecmgr(&cfgDb, &stateDb, cfg_macsec_tables);

        std::vector<Orch *> cfgOrchList = {&macsecmgr};

        swss::Select s;
        for (Orch *o : cfgOrchList)
        {
            s.addSelectables(o->getSelectables());
        }

        bool isPostStateReady = false;

        SWSS_LOG_NOTICE("starting main loop");
        while (!shutdown_requested)
        {
            /* Don't process any config until POST state is ready */
            if (!isPostStateReady)
            {
                std::string state = getMacsecPostState(&stateDb);
                if (state == "pass" || state == "disabled")
                {
                    SWSS_LOG_NOTICE("FIPS MACSec POST ready: state %s", state.c_str());
                    isPostStateReady = true;
                }
                else
                {
                    /* Yield before retry */
                    sleep(1);
                    continue;
                }
            }

            Selectable *sel;
            int ret;

            ret = s.select(&sel, SELECT_TIMEOUT);
            if (shutdown_requested)
            {
                break;
            }
            if (ret == Select::ERROR)
            {
                SWSS_LOG_NOTICE("Error: %s!", strerror(errno));
                continue;
            }
            if (ret == Select::TIMEOUT)
            {
                macsecmgr.doTask();
                continue;
            }

            auto *c = (Executor *)sel;
            c->execute();
        }
        return 0;
    }
    catch(const std::exception &e)
    {
        SWSS_LOG_ERROR("Runtime error: %s", e.what());
    }
    return -1;
}
