#include <fstream>
#include <iostream>
#include <mutex>
#include <unistd.h>
#include <vector>

#include "exec.h"
#include "portmgr.h"
#include "schema.h"
#include "select.h"

using namespace std;
using namespace swss;

/* select() function timeout retry time, in millisecond */
#define SELECT_TIMEOUT 1000

int main(int argc, char **argv)
{
    Logger::linkToDbNative("portmgrd");
    SWSS_LOG_ENTER();

    SWSS_LOG_NOTICE("--- Starting portmgrd ---");

    try
    {
        vector<string> cfg_port_tables = {
            CFG_PORT_TABLE_NAME,
            CFG_SEND_TO_INGRESS_PORT_TABLE_NAME,
        };

        DBConnector cfgDb("CONFIG_DB", 0);
        DBConnector appDb("APPL_DB", 0);
        DBConnector stateDb("STATE_DB", 0);

        PortMgr portmgr(&cfgDb, &appDb, &stateDb, cfg_port_tables);
        vector<Orch *> cfgOrchList = {&portmgr};

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
                portmgr.doTask();
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
