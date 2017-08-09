#include <getopt.h>
#include <unistd.h>
#include <iostream>
#include <fstream>
#include <sstream>
#include <vector>
#include <set>
#include <map>
#include <mutex>
#include "dbconnector.h"
#include "select.h"
#include "exec.h"
#include "netdispatcher.h"
#include "producerstatetable.h"
#include "cfgagent/vlancfgagent.h"
#include "cfgagent/portcfgagent.h"
#include "cfgagent/intfcfgagent.h"

using namespace std;
using namespace swss;

/* select() function timeout retry time, in millisecond */
#define SELECT_TIMEOUT 1000

#define DEFAULT_BATCH_SIZE  128
int gBatchSize = DEFAULT_BATCH_SIZE;

MacAddress gMacAddress;
bool gInitDone = false;
bool gSwssCfgRecord = true;
ofstream gCfgRecordOfs;
string gCfgRecordFile;
/* Global database mutex */
mutex gDbMutex;

void usage()
{
    cout << "usage: cfgagent [-h] [-r record_type] [-d record_location] [-b batch_size]" << endl;
    cout << "    -h: display this message" << endl;
    cout << "    -r record_type: record orchagent logs with type (default 3)" << endl;
    cout << "                    0: do not record logs" << endl;
    cout << "                    1: record SwSS task sequence as swss.cfg.*.rec" << endl;
    cout << "    -d record_location: set record logs folder location (default .)" << endl;
    cout << "    -b batch_size: set consumer table pop operation batch size (default 128)" << endl;
}

int main(int argc, char **argv)
{
    Logger::linkToDbNative("cfgagent");
    SWSS_LOG_ENTER();

    int opt;
    string record_location = ".";

    while ((opt = getopt(argc, argv, "b:r:d:h")) != -1)
    {
        switch (opt)
        {
        case 'b':
            gBatchSize = atoi(optarg);
            break;
        case 'r':
            if (!strcmp(optarg, "0"))
            {
                gSwssCfgRecord = false;
            }
            else if (!strcmp(optarg, "1"))
            {
                continue; /* default behavior */
            }
            else
            {
                usage();
                exit(EXIT_FAILURE);
            }
            break;
        case 'd':
            record_location = optarg;
            if (access(record_location.c_str(), W_OK))
            {
                SWSS_LOG_ERROR("Failed to access writable directory %s", record_location.c_str());
                exit(EXIT_FAILURE);
            }
            break;
        case 'h':
            usage();
            exit(EXIT_SUCCESS);
        default: /* '?' */
            exit(EXIT_FAILURE);
        }
    }

    SWSS_LOG_NOTICE("--- Starting cfgagentd ---");

    /* Disable/enable SwSS cfg recording */
    if (gSwssCfgRecord)
    {
        gCfgRecordFile = "swss.cfg." + getTimestamp() + ".rec";
        gCfgRecordOfs.open(record_location + "/" + gCfgRecordFile);
        if (!gCfgRecordOfs.is_open())
        {
            SWSS_LOG_ERROR("Failed to open SwSS recording file %s", gCfgRecordFile.c_str());
            exit(EXIT_FAILURE);
        }
    }
    gMacAddress = swss::exec("ip link show eth0 | grep ether | awk '{print $2}'");

    try
    {
        vector<string> cfg_vlan_tables = {
            CFG_VLAN_TABLE_NAME,
            CFG_VLAN_MEMBER_TABLE_NAME,
        };

        DBConnector cfgDb(CONFIG_DB, DBConnector::DEFAULT_UNIXSOCKET, 0);
        DBConnector appDb(APPL_DB, DBConnector::DEFAULT_UNIXSOCKET, 0);

        Table porttableconsumer(&appDb, APP_PORT_TABLE_NAME);

        PortCfgAgent portcfgagent(&cfgDb, &appDb, CFG_PORT_TABLE_NAME);
        VlanCfgAgent vlancfgagent(&cfgDb, &appDb, cfg_vlan_tables);
        IntfCfgAgent intfcfgagent(&cfgDb, &appDb, CFG_INTF_TABLE_NAME);
        std::vector<CfgOrch *> cfgOrchList = {&vlancfgagent, &portcfgagent, &intfcfgagent};

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
                if (gInitDone == false)
                {
                    vector<FieldValueTuple> temp;
                    if (porttableconsumer.get("ConfigDone", temp))
                    {
                        gInitDone = true;
                        SWSS_LOG_NOTICE("Physical ports hostif init done\n");
                        vlancfgagent.SyncCfgDB();
                        /* Sync interface config after VLAN */
                        intfcfgagent.SyncCfgDB();
                    }
                }
                continue;
            }

            for (CfgOrch *o : cfgOrchList)
            {
                if (o->hasSelectable((ConsumerStateTable *)sel))
                {
                    o->execute(((ConsumerStateTable *)sel)->getTableName());
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
