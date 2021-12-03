#include <unistd.h>
#include <vector>
#include <mutex>
#include <fstream>
#include <iostream>
#include <chrono>

#include "select.h"
#include "exec.h"
#include "schema.h"
#include "nbrmgr.h"
#include "warm_restart.h"

using namespace std;
using namespace swss;

#define RESTORE_NEIGH_WAIT_TIME_OUT 120
#define RESTORE_NEIGH_WAIT_TIME_INT 10

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
    Logger::linkToDbNative("nbrmgrd");
    SWSS_LOG_ENTER();

    SWSS_LOG_NOTICE("--- Starting nbrmgrd ---");

    try
    {
        vector<string> cfg_nbr_tables = {
            CFG_NEIGH_TABLE_NAME,
        };

        DBConnector cfgDb("CONFIG_DB", 0);
        DBConnector appDb("APPL_DB", 0);
        DBConnector stateDb("STATE_DB", 0);

        NbrMgr nbrmgr(&cfgDb, &appDb, &stateDb, cfg_nbr_tables);

        WarmStart::initialize("nbrmgrd", "swss");
        WarmStart::checkWarmStart("nbrmgrd", "swss");

        if (WarmStart::isWarmStart())
        {
            chrono::steady_clock::time_point starttime = chrono::steady_clock::now();
            while (!nbrmgr.isNeighRestoreDone())
            {
                chrono::duration<double> time_span = chrono::duration_cast<chrono::duration<double>>
                                                     (chrono::steady_clock::now() - starttime);
                int pasttime = int(time_span.count());
                SWSS_LOG_INFO("Kernel neighbor table restoration waited for %d seconds", pasttime);
                if (pasttime > RESTORE_NEIGH_WAIT_TIME_OUT)
                {
                    SWSS_LOG_WARN("Kernel neighbor table restore is not finished!");
                    break;
                }
                sleep(RESTORE_NEIGH_WAIT_TIME_INT);
            }
        }

        std::vector<Orch *> cfgOrchList = {&nbrmgr};

        swss::Select s;
        for (Orch *o : cfgOrchList)
        {
            s.addSelectables(o->getSelectables());
        }

        SWSS_LOG_NOTICE("starting main loop");
        while (true)
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
                nbrmgr.doTask();
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
