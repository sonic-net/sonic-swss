#include <unistd.h>
#include <vector>
#include "dbconnector.h"
#include "select.h"
#include "exec.h"
#include "schema.h"
#include "intfconf.h"

using namespace std;
using namespace swss;

/* select() function timeout retry time, in millisecond */
#define SELECT_TIMEOUT 1000

int main(int argc, char **argv)
{
    Logger::linkToDbNative("intfconfd");
    SWSS_LOG_ENTER();

    SWSS_LOG_NOTICE("--- Starting intfconfd ---");

    try
    {
        vector<string> cfg_intf_tables = {
            CFG_INTF_TABLE_NAME,
            CFG_LAG_INTF_TABLE_NAME,
            CFG_VLAN_INTF_TABLE_NAME,
        };

        DBConnector cfgDb(CONFIG_DB, DBConnector::DEFAULT_UNIXSOCKET, 0);
        DBConnector appDb(APPL_DB, DBConnector::DEFAULT_UNIXSOCKET, 0);
        DBConnector stateDb(STATE_DB, DBConnector::DEFAULT_UNIXSOCKET, 0);

        IntfConf intfconf(&cfgDb, &appDb, &stateDb, cfg_intf_tables);

        // TODO: add tables in stateDB which interface depends on to monitor list
        std::vector<CfgOrch *> cfgOrchList = {&intfconf};

        swss::Select s;
        for (CfgOrch *o : cfgOrchList)
        {
            s.addSelectables(o->getSelectables());
        }

        SWSS_LOG_NOTICE("starting main loop");
        while (true)
        {
            Selectable *sel;
            int fd, ret;

            ret = s.select(&sel, &fd, SELECT_TIMEOUT);
            if (ret == Select::ERROR)
            {
                SWSS_LOG_NOTICE("Error: %s!\n", strerror(errno));
                continue;
            }
            if (ret == Select::TIMEOUT)
            {
               ((CfgOrch *)&intfconf)->doTask();
                continue;
            }

            for (CfgOrch *o : cfgOrchList)
            {
                TableConsumable *c = (TableConsumable *)sel;
                if (o->hasSelectable(c))
                {
                    o->execute(c->getTableName());
                }
            }
        }
    }
    catch(const std::exception &e)
    {
        SWSS_LOG_ERROR("Runtime error: %s", e.what());
    }
    return 0;
}
