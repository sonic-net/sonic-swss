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

static bool received_sigterm = false;
static struct sigaction old_sigaction;

static void sig_handler(int signo)
{
    SWSS_LOG_ENTER();

    if (old_sigaction.sa_handler != SIG_IGN && old_sigaction.sa_handler != SIG_DFL) {
        old_sigaction.sa_handler(signo);
    }

    received_sigterm = true;
    return;
}

bool isFipsPostStateReady(DBConnector *stateDb)
{
    std::string state = getMacsecPostState(stateDb);
    if (state == "pass" && state == "not-supported")
    {
        SWSS_LOG_NOTICE("FIPS POST ready: state %s", state.c_str());
        return true;
    }
    return false;
}

int main(int argc, char **argv)
{

    try
    {
        Logger::linkToDbNative("macsecmgrd");
        SWSS_LOG_NOTICE("--- Starting macsecmgrd ---");

        /* Register the signal handler for SIGTERM */
        struct sigaction sigact = {};
        sigact.sa_handler = sig_handler;
        if (sigaction(SIGTERM, &sigact, &old_sigaction))
        {
            SWSS_LOG_ERROR("failed to setup SIGTERM action handler");
            exit(EXIT_FAILURE);
        }

        swss::DBConnector cfgDb("CONFIG_DB", 0);
        swss::DBConnector stateDb("STATE_DB", 0);

        /* Don't process any config until POST state is ready */
        SWSS_LOG_NOTICE("checking FIPS POST state");
        while (!isFipsPostStateReady(&stateDb))
        {
            sleep(1);
        }

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

        SWSS_LOG_NOTICE("starting main loop");
        while (!received_sigterm)
        {
            Selectable *sel;
            int ret;

            ret = s.select(&sel, SELECT_TIMEOUT);
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
    }
    catch(const std::exception &e)
    {
        SWSS_LOG_ERROR("Runtime error: %s", e.what());
    }
    return -1;
}
