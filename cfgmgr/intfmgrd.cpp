#include <unistd.h>
#include <vector>
#include <mutex>
#include "dbconnector.h"
#include "select.h"
#include "exec.h"
#include "schema.h"
#include "intfmgr.h"
#include <fstream>
#include <iostream>

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
    Logger::linkToDbNative("intfmgrd");
    SWSS_LOG_ENTER();

    SWSS_LOG_NOTICE("--- Starting intfmgrd ---");

    try
    {
        vector<string> cfg_intf_tables = {
            CFG_INTF_TABLE_NAME,
            CFG_LAG_INTF_TABLE_NAME,
            CFG_VLAN_INTF_TABLE_NAME,
        };

        DBConnector cfgDb(CONFIG_DB, DBConnector::DEFAULT_UNIXSOCKET, 0);
        DBConnector appDb(APPL_DB, DBConnector::DEFAULT_UNIXSOCKET, 0);
        DBConnector stateDb(STATE_DB, DBConnector::DEFAULT_UNIXSOCKET, 0);

        IntfMgr intfmgr(&cfgDb, &appDb, &stateDb, cfg_intf_tables);

        // TODO: add tables in stateDB which interface depends on to monitor list
        std::vector<Orch *> cfgOrchList = {&intfmgr};

        swss::Select s;
        for (Orch *o : cfgOrchList)
        {
            s.addSelectables(o->getSelectables());
        }

        SWSS_LOG_NOTICE("starting main loop");
        while (true)
        {
            Selectable *sel;
            int fd, ret;

            ret = s.select(&sel, &fd, SELECT_TIMEOUT);
            if (ret == Select::ERROR)
            {
                SWSS_LOG_NOTICE("Error: %s!", strerror(errno));
                continue;
            }
            if (ret == Select::TIMEOUT)
            {
               ((Orch *)&intfmgr)->doTask();
                continue;
            }

            for (Orch *o : cfgOrchList)
            {
                TableConsumable *c = (TableConsumable *)sel;
                if (o->hasSelectable(c))
                {
                    o->execute(c->getTableName());
                }
            }
        }
    }
    catch(const std::exception &e)
    {
        SWSS_LOG_ERROR("Runtime error: %s", e.what());
    }
    return -1;
}
