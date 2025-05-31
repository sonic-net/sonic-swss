#include <fstream>
#include <iostream>
#include <mutex>
#include <unistd.h>
#include <vector>

#include "exec.h"
#include "fabricmgr.h"
#include "schema.h"
#include "select.h"

using namespace std;
using namespace swss;

bool gSwssRecord = false;
bool gLogRotate = false;
ofstream gRecordOfs;
string gRecordFile;

/* select() function timeout retry time, in millisecond */
#define SELECT_TIMEOUT 1000

int main(int argc, char **argv)
{
    Logger::linkToDbNative("fabricmgrd");
    SWSS_LOG_ENTER();

    SWSS_LOG_NOTICE("--- Starting fabricmgrd ---");

    try
    {
        vector<string> cfg_fabric_tables = {
            CFG_FABRIC_MONITOR_DATA_TABLE_NAME,
            CFG_FABRIC_MONITOR_PORT_TABLE_NAME,
        };

        DBConnector cfgDb("CONFIG_DB", 0);
        DBConnector appDb("APPL_DB", 0);

        FabricMgr fabricmgr(&cfgDb, &appDb, cfg_fabric_tables);

        // TODO: add tables in stateDB which interface depends on to monitor list
        vector<Orch *> cfgOrchList = {&fabricmgr};

        swss::Select s;
        for (Orch *o : cfgOrchList)
        {
            s.addSelectables(o->getSelectables());
        }

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
                fabricmgr.doTask();
                continue;
            }

            auto *c = (Executor *)sel;
            c->execute();
        }
    }
    catch (const exception &e)
    {
        SWSS_LOG_ERROR("Runtime error: %s", e.what());
    }
    return -1;
}

