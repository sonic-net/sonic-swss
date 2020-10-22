#include <unistd.h>
#include <vector>
#include <sstream>
#include <fstream>
#include <iostream>
#include <mutex>
#include <algorithm>

#include "dbconnector.h"
#include "select.h"
#include "exec.h"
#include "schema.h"
#include "tunnelmgr.h"

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
/* Global database mutex */
mutex gDbMutex;

int main(int argc, char **argv)
{
    Logger::linkToDbNative("tunnelmgrd");

    SWSS_LOG_NOTICE("--- Starting Tunnelmgrd ---");

    try
    {

        DBConnector cfgDb("CONFIG_DB", 0);
        DBConnector appDb("APPL_DB", 0);

        vector<std::string> cfg_tunnel_tables = {
            CFG_TUNNEL_TABLE_NAME,
        };

        TunnelMgr tunnelmgr(&cfgDb, &appDb, cfg_tunnel_tables);

        std::vector<Orch *> cfgOrchList = {&tunnelmgr};

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
                tunnelmgr.doTask();
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
