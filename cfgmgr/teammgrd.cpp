#include <fstream>

#include "teammgr.h"
#include "netdispatcher.h"
#include "netlink.h"
#include "select.h"
#include "warm_restart.h"

using namespace std;
using namespace swss;

#define SELECT_TIMEOUT 1000

int gBatchSize = 0;
bool gSwssRecord = false;
bool gLogRotate = false;
ofstream gRecordOfs;
string gRecordFile;

int main(int argc, char **argv)
{
    Logger::linkToDbNative("teammgrd");
    SWSS_LOG_ENTER();

    SWSS_LOG_NOTICE("--- Starting teammrgd ---");

    try
    {
        DBConnector conf_db(CONFIG_DB, DBConnector::DEFAULT_UNIXSOCKET, 0);
        DBConnector app_db(APPL_DB, DBConnector::DEFAULT_UNIXSOCKET, 0);
        DBConnector state_db(STATE_DB, DBConnector::DEFAULT_UNIXSOCKET, 0);

        WarmStart::initialize("teammgrd");
        WarmStart::checkWarmStart("teammgrd");

        TableConnector conf_lag_table(&conf_db, CFG_LAG_TABLE_NAME);
        TableConnector conf_lag_member_table(&conf_db, CFG_LAG_MEMBER_TABLE_NAME);
        TableConnector state_port_table(&state_db, STATE_PORT_TABLE_NAME);

        vector<TableConnector> tables = {
            conf_lag_table,
            conf_lag_member_table,
            state_port_table
        };

        TeamMgr teammgr(&conf_db, &app_db, &state_db, tables);

        vector<Orch *> cfgOrchList = {&teammgr};

        Select s;
        for (Orch *o: cfgOrchList)
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
                teammgr.doTask();
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
