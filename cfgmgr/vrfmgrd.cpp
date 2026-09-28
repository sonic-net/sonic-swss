#include <unistd.h>
#include <signal.h>
#include <vector>
#include <mutex>
#include "dbconnector.h"
#include "select.h"
#include "exec.h"
#include "schema.h"
#include "vrfmgr.h"
#include <fstream>
#include <iostream>
#include "warm_restart.h"

using namespace std;
using namespace swss;

/* select() function timeout retry time, in millisecond */
#define SELECT_TIMEOUT 1000

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
    Logger::linkToDbNative("vrfmgrd");
    bool isWarmStart = false;
    SWSS_LOG_ENTER();

    SWSS_LOG_NOTICE("--- Starting vrfmgrd ---");

    try
    {
        struct sigaction sigact = {};
        sigact.sa_handler = sig_handler;
        if (sigaction(SIGTERM, &sigact, &old_sigaction))
        {
            SWSS_LOG_ERROR("failed to setup SIGTERM action handler");
            exit(EXIT_FAILURE);
        }
        vector<string> cfg_vrf_tables = {
            CFG_VRF_TABLE_NAME,
            CFG_VNET_TABLE_NAME,
            CFG_VXLAN_EVPN_NVO_TABLE_NAME,
            CFG_MGMT_VRF_CONFIG_TABLE_NAME
        };

        DBConnector cfgDb("CONFIG_DB", 0);
        DBConnector appDb("APPL_DB", 0);
        DBConnector stateDb("STATE_DB", 0);

        WarmStart::initialize("vrfmgrd", "swss");
        WarmStart::checkWarmStart("vrfmgrd", "swss");

        VrfMgr vrfmgr(&cfgDb, &appDb, &stateDb, cfg_vrf_tables);

        isWarmStart = WarmStart::isWarmStart();

        std::vector<Orch *> cfgOrchList = {&vrfmgr};

        swss::Select s;
        for (Orch *o : cfgOrchList)
        {
            s.addSelectables(o->getSelectables());
        }

        SWSS_LOG_NOTICE("starting main loop");
        while (!received_sigterm)
        {
            Selectable *sel;
            static bool firstReadTimeout = true;
            int ret;

            ret = s.select(&sel, SELECT_TIMEOUT);
            if (ret == Select::ERROR)
            {
                SWSS_LOG_NOTICE("Error: %s!", strerror(errno));
                continue;
            }
            if (ret == Select::TIMEOUT)
            {
                vrfmgr.doTask();
                if (isWarmStart && firstReadTimeout)
                {
                    firstReadTimeout = false;
                    WarmStart::setWarmStartState("vrfmgrd", WarmStart::REPLAYED);
                    // There is no operation to be performed for vrfmgrd reconcillation
                    // Hence mark it reconciled right away
                    WarmStart::setWarmStartState("vrfmgrd", WarmStart::RECONCILED);
                }
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
