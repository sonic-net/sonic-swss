#include <unistd.h>
#include <vector>
#include "dbconnector.h"
#include "select.h"
#include "schema.h"
#include "mirrormgr.h"
#include "shellcmd.h"
#include "warm_restart.h"

using namespace std;
using namespace swss;

/* select() function timeout retry time, in millisecond */
#define SELECT_TIMEOUT 1000

int main(int argc, char **argv)
{
    Logger::linkToDbNative("mirrormgrd");
    SWSS_LOG_ENTER();

    SWSS_LOG_NOTICE("--- Starting mirrormgrd ---");

    try
    {
        /* CONFIG_DB tables that mirrormgrd subscribes to */
        vector<string> cfg_mirror_tables = {
            CFG_MIRROR_SESSION_TABLE_NAME,
        };

        DBConnector cfgDb("CONFIG_DB", 0);
        DBConnector stateDb("STATE_DB", 0);

        WarmStart::initialize("mirrormgrd", "swss");
        WarmStart::checkWarmStart("mirrormgrd", "swss");

        MirrorMgr mirrormgr(&cfgDb, &stateDb, cfg_mirror_tables);

        std::vector<Orch *> cfgOrchList = {&mirrormgr};

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
                mirrormgr.doTask();
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
