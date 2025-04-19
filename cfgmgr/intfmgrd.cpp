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
    Logger::linkToDbNative("intfmgrd");
    SWSS_LOG_ENTER();

    SWSS_LOG_NOTICE("--- Starting intfmgrd ---");

    try
    {
        vector<string> cfg_intf_tables = {
            CFG_INTF_TABLE_NAME,
            CFG_LAG_INTF_TABLE_NAME,
            CFG_VLAN_INTF_TABLE_NAME,
            CFG_LOOPBACK_INTERFACE_TABLE_NAME,
            CFG_VLAN_SUB_INTF_TABLE_NAME,
            CFG_VOQ_INBAND_INTERFACE_TABLE_NAME,
        };

        DBConnector cfgDb("CONFIG_DB", 0);
        DBConnector appDb("APPL_DB", 0);
        DBConnector stateDb("STATE_DB", 0);

        WarmStart::initialize("intfmgrd", "swss");
        WarmStart::checkWarmStart("intfmgrd", "swss");

        IntfMgr intfmgr(&cfgDb, &appDb, &stateDb, cfg_intf_tables);
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
            int ret;

            ret = s.select(&sel, SELECT_TIMEOUT);
            if (ret == Select::ERROR)
            {
                SWSS_LOG_NOTICE("Error: %s!", strerror(errno));
                continue;
            }
            if (ret == Select::TIMEOUT)
            {
                intfmgr.doTask();
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
