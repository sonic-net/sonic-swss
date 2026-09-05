#include <unistd.h>
#include <vector>
#include "dbconnector.h"
#include "select.h"
#include "schema.h"
#include "qosmgr.h"
#include "shellcmd.h"
#include "warm_restart.h"

using namespace std;
using namespace swss;

/* select() function timeout retry time, in millisecond */
#define SELECT_TIMEOUT 1000

int main(int argc, char **argv)
{
    Logger::linkToDbNative("qosmgrd");
    SWSS_LOG_ENTER();

    SWSS_LOG_NOTICE("--- Starting qosmgrd ---");

    try
    {
        /* CONFIG_DB tables that qosmgrd subscribes to */
        vector<string> cfg_qos_tables = {
            CFG_DSCP_TO_TC_MAP_TABLE_NAME,
            CFG_DOT1P_TO_TC_MAP_TABLE_NAME,
            CFG_TC_TO_QUEUE_MAP_TABLE_NAME,
            CFG_TC_TO_DSCP_MAP_TABLE_NAME,
            CFG_SCHEDULER_TABLE_NAME,
            CFG_WRED_PROFILE_TABLE_NAME,
            CFG_QUEUE_TABLE_NAME,
            CFG_PORT_QOS_MAP_TABLE_NAME,
        };

        DBConnector cfgDb("CONFIG_DB", 0);
        DBConnector stateDb("STATE_DB", 0);

        WarmStart::initialize("qosmgrd", "swss");
        WarmStart::checkWarmStart("qosmgrd", "swss");

        QosMgr qosmgr(&cfgDb, &stateDb, cfg_qos_tables);

        std::vector<Orch *> cfgOrchList = {&qosmgr};

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
                qosmgr.doTask();
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
