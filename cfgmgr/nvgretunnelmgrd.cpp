#include <unistd.h>
#include <vector>
#include "dbconnector.h"
#include "select.h"
#include "schema.h"
#include "nvgretunnelmgr.h"
#include "shellcmd.h"
#include "warm_restart.h"

using namespace std;
using namespace swss;

#define SELECT_TIMEOUT 1000

int main(int argc, char **argv)
{
    Logger::linkToDbNative("nvgretunnelmgrd");
    SWSS_LOG_ENTER();

    SWSS_LOG_NOTICE("--- Starting nvgretunnelmgrd ---");

    try
    {
        vector<string> cfg_nvgre_tables = {
            CFG_NVGRE_TUNNEL_TABLE_NAME,
            CFG_NVGRE_TUNNEL_MAP_TABLE_NAME,
        };

        DBConnector cfgDb("CONFIG_DB", 0);
        DBConnector stateDb("STATE_DB", 0);

        WarmStart::initialize("nvgretunnelmgrd", "swss");
        WarmStart::checkWarmStart("nvgretunnelmgrd", "swss");

        NvgreTunnelMgr nvgremgr(&cfgDb, &stateDb, cfg_nvgre_tables);

        std::vector<Orch *> cfgOrchList = {&nvgremgr};

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
                nvgremgr.doTask();
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
