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

using namespace std;
using namespace swss;

/* select() function timeout retry time, in millisecond */
#define SELECT_TIMEOUT 1000

MacAddress gMacAddress;

/*
 * Following global variables are defined here for the purpose of
 * using existing Orch class which is to be refactored soon to
 * eliminate the direct exposure of the global variables.
 *
 * Once Orch class refactoring is done, these global variables
 * should be removed from here.
 */
int gBatchSize = 0;
bool gSwssRecord = false;
bool gLogRotate = false;
ofstream gRecordOfs;
string gRecordFile;
bool gResponsePublisherRecord = false;
bool gResponsePublisherLogRotate = false;
ofstream gResponsePublisherRecordOfs;
string gResponsePublisherRecordFile;
/* Global database mutex */
mutex gDbMutex;

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
