#include <vector>

#include "poemgr.h"
#include "dbconnector.h"
#include "select.h"
#include "warm_restart.h"

using namespace swss;

/* select() function timeout retry time, in millisecond */
#define SELECT_TIMEOUT 1000


int main(int argc, char **argv)
{
    Logger::linkToDbNative("poemgrd");
    SWSS_LOG_ENTER();

    SWSS_LOG_NOTICE("--- Starting poemgrd ---");

    try
    {
        std::vector<std::string> cfg_tables = {
            CFG_POE_TABLE_NAME,
        };

        DBConnector cfgDb("CONFIG_DB", 0);
        DBConnector appDb("APPL_DB", 0);

        WarmStart::initialize("poemgrd", "swss");
        WarmStart::checkWarmStart("poemgrd", "swss");

        PoeMgr manager(&appDb, &cfgDb, cfg_tables);

        std::vector<Orch *> cfgOrchList = {&manager};

        Select s;
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
                manager.doTask();
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
