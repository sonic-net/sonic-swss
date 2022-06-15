#include <fstream>

#include "teammgr.h"
#include "netdispatcher.h"
#include "netlink.h"
#include "select.h"
#include "warm_restart.h"
#include <signal.h>

using namespace std;
using namespace swss;

#define SELECT_TIMEOUT 1000

int gBatchSize = 0;
bool gSwssRecord = false;
bool gLogRotate = false;
ofstream gRecordOfs;
string gRecordFile;
bool gResponsePublisherRecord = false;
bool gResponsePublisherLogRotate = false;
ofstream gResponsePublisherRecordOfs;
string gResponsePublisherRecordFile;

bool received_sigterm = false;
static struct sigaction old_sigaction;

void sig_handler(int signo)
{
    SWSS_LOG_ENTER();

    if (old_sigaction.sa_handler != SIG_IGN && old_sigaction.sa_handler != SIG_DFL) {
        old_sigaction.sa_handler(signo);
    }

    received_sigterm = true;
    return;
}

int main(int argc, char **argv)
{
    Logger::linkToDbNative("teammgrd");
    SWSS_LOG_ENTER();

    SWSS_LOG_NOTICE("--- Starting teammrgd ---");

    /* Register the signal handler for SIGTERM */
    struct sigaction sigact = {};
    sigact.sa_handler = sig_handler;
    if (sigaction(SIGTERM, &sigact, &old_sigaction))
    {
        SWSS_LOG_ERROR("failed to setup SIGTERM action handler");
        exit(EXIT_FAILURE);
    }

    try
    {
        DBConnector conf_db("CONFIG_DB", 0);
        DBConnector app_db("APPL_DB", 0);
        DBConnector state_db("STATE_DB", 0);

        WarmStart::initialize("teammgrd", "teamd");
        WarmStart::checkWarmStart("teammgrd", "teamd");

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

        while (!received_sigterm)
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
        teammgr.cleanTeamProcesses();
        SWSS_LOG_NOTICE("Exiting");
    }
    catch (const exception &e)
    {
        SWSS_LOG_ERROR("Runtime error: %s", e.what());
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
