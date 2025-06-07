#include <fstream>
#include <iostream>
#include <mutex>
#include <unistd.h>
#include <vector>

#include "exec.h"
#include "sflowmgr.h"
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
    Logger::linkToDbNative("sflowmgrd");
    SWSS_LOG_ENTER();

    SWSS_LOG_NOTICE("--- Starting sflowmgrd ---");

    try
    {
        DBConnector cfgDb("CONFIG_DB", 0);
        DBConnector appDb("APPL_DB", 0);
        DBConnector stateDb("STATE_DB", 0);

        TableConnector conf_port_table(&cfgDb, CFG_PORT_TABLE_NAME);
        TableConnector state_port_table(&stateDb, STATE_PORT_TABLE_NAME);
        TableConnector conf_sflow_table(&cfgDb, CFG_SFLOW_TABLE_NAME);
        TableConnector conf_sflow_session_table(&cfgDb, CFG_SFLOW_SESSION_TABLE_NAME);

        vector<TableConnector> sflow_tables = {
            conf_port_table,
            state_port_table,
            conf_sflow_table,
            conf_sflow_session_table
        };

        SflowMgr sflowmgr(&appDb, sflow_tables);
        /* During process startup, the ordering of config_db followed by state_db notifications cannot be guaranteed 
           and so handle the config events manually */
        sflowmgr.readPortConfig();

        vector<Orch *> orchList = {&sflowmgr};

        swss::Select s;
        for (Orch *o : orchList)
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
                sflowmgr.doTask();
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
