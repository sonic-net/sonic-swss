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
#include "macaddress.h"
#include "producerstatetable.h"
#include "vnetmgr.h"
#include "shellcmd.h"
#include "warm_restart.h"

using namespace std;
using namespace swss;

/* select() function timeout retry time, in millisecond */
#define SELECT_TIMEOUT 1000

int main(int argc, char **argv)
{
    swss::Logger::linkToDbNative("vnetmgrd");

    SWSS_LOG_NOTICE("--- Starting vnetmgrd ---");

    try
    {

        DBConnector cfgDb("CONFIG_DB", 0);
        DBConnector appDb("APPL_DB", 0);

        WarmStart::initialize("vnetmgrd", "swss");
        WarmStart::checkWarmStart("vnetmgrd", "swss");
        if (WarmStart::isWarmStart())
        {
            WarmStart::setWarmStartState("vnetmgrd", WarmStart::INITIALIZED);
        }

        vector<std::string> cfg_vnet_tables = {
            CFG_VNET_TABLE_NAME,
            CFG_VXLAN_TUNNEL_TABLE_NAME,
            CFG_VNET_RT_TUNNEL_TABLE_NAME,
            CFG_VNET_RT_TABLE_NAME
        };

        VNetMgr vnetmgr(&cfgDb, &appDb, cfg_vnet_tables);

        std::vector<Orch *> cfgOrchList = {&vnetmgr};

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
                vnetmgr.doTask();
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
