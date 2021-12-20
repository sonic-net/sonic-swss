#include <unistd.h>
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

int main(int argc, char **argv)
{
    Logger::linkToDbNative("vrfmgrd");
    bool isWarmStart = false;
    SWSS_LOG_ENTER();

    SWSS_LOG_NOTICE("--- Starting vrfmgrd ---");

    try
    {
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

        // TODO: add tables in stateDB which interface depends on to monitor list
        std::vector<Orch *> cfgOrchList = {&vrfmgr};

        swss::Select s;
        for (Orch *o : cfgOrchList)
        {
            s.addSelectables(o->getSelectables());
        }

        SWSS_LOG_NOTICE("starting main loop");
        while (true)
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
