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
#include "warm_restart.h"

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
    Logger::linkToDbNative("tunnelmgrd");

    SWSS_LOG_NOTICE("--- Starting Tunnelmgrd ---");

    try
    {
        vector<string> cfgTunTables = {
            CFG_TUNNEL_TABLE_NAME,
            CFG_LOOPBACK_INTERFACE_TABLE_NAME
        };

        DBConnector cfgDb("CONFIG_DB", 0);
        DBConnector appDb("APPL_DB", 0);

        WarmStart::initialize("tunnelmgrd", "swss");
        WarmStart::checkWarmStart("tunnelmgrd", "swss");

        TunnelMgr tunnelmgr(&cfgDb, &appDb, cfgTunTables);

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
