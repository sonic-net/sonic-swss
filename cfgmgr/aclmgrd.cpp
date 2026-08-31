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
#include "producerstatetable.h"
#include "aclmgr.h"
#include "shellcmd.h"
#include "warm_restart.h"

using namespace std;
using namespace swss;

/* select() function timeout retry time, in millisecond */
#define SELECT_TIMEOUT 1000

int main(int argc, char **argv)
{
    Logger::linkToDbNative("aclmgrd");
    SWSS_LOG_ENTER();

    SWSS_LOG_NOTICE("--- Starting aclmgrd ---");

    try
    {
        /* CONFIG_DB tables that aclmgrd subscribes to */
        vector<string> cfg_acl_tables = {
            CFG_ACL_TABLE_TABLE_NAME,
            CFG_ACL_RULE_TABLE_NAME,
            CFG_ACL_TABLE_TYPE_TABLE_NAME,
            CFG_POLICER_TABLE_NAME,
        };

        DBConnector cfgDb("CONFIG_DB", 0);
        DBConnector appDb("APPL_DB", 0);
        DBConnector stateDb("STATE_DB", 0);

        WarmStart::initialize("aclmgrd", "swss");
        WarmStart::checkWarmStart("aclmgrd", "swss");

        AclMgr aclmgr(&cfgDb, &appDb, &stateDb, cfg_acl_tables);

        std::vector<Orch *> cfgOrchList = {&aclmgr};

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
                aclmgr.doTask();
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
