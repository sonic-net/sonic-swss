/*
 * Copyright 2019 Broadcom. All rights reserved.
 * The term "Broadcom" refers to Broadcom Inc. and/or its subsidiaries.
 * 
 *  cfgmgr/l2mcdmgrd.cpp
 */

#include <fstream>
#include "l2mcmgr.h"
#include "netdispatcher.h"
#include "netlink.h"
#include "select.h"
#include "warm_restart.h"
#include "schema.h"

using namespace std;
using namespace swss;

/* select() function timout retry time in milli-seconds */
#define SELECT_TIMEOUT 1000

int gBatchSize = 0;
bool gSwssRecord = false;
bool gLogRotate = false;
ofstream gRecordOfs;
string gRecordFile;
/* Global database mutex */
mutex gDbMutex;

int main(int argc, char **argv)
{
    Logger::linkToDbNative("l2mcdmgrd");
    SWSS_LOG_ENTER();

    SWSS_LOG_NOTICE("--- Starting l2mcmgrd ---");

    try
    {
        DBConnector conf_db(CONFIG_DB, DBConnector::DEFAULT_UNIXSOCKET, 0);
        DBConnector app_db(APPL_DB, DBConnector::DEFAULT_UNIXSOCKET, 0);
        DBConnector state_db(STATE_DB, DBConnector::DEFAULT_UNIXSOCKET, 0);

        WarmStart::initialize("l2mcmgrd", "l2mcmgrd");
        WarmStart::checkWarmStart("l2mcmgrd", "l2mcmgrd");

        // Config DB Tables
        TableConnector conf_l2mc_global_table(&conf_db, CFG_L2MC_TABLE_NAME);
        TableConnector conf_l2mc_static_table(&conf_db, CFG_L2MC_STATIC_TABLE_NAME);
        TableConnector conf_l2mc_mrouter_table(&conf_db, CFG_L2MC_MROUTER_TABLE_NAME);
        TableConnector conf_lag_member_table(&conf_db,  CFG_LAG_MEMBER_TABLE_NAME);
        TableConnector state_interface_table(&state_db, STATE_INTERFACE_TABLE_NAME);
        TableConnector state_vlan_member_table(&state_db,  STATE_VLAN_MEMBER_TABLE_NAME);
        TableConnector state_lag_table(&state_db,  STATE_LAG_TABLE_NAME);
        TableConnector state_port_table(&state_db,  STATE_PORT_TABLE_NAME);

        vector<TableConnector> tables = {
            conf_l2mc_global_table,
            conf_l2mc_static_table,
            conf_l2mc_mrouter_table,
            conf_lag_member_table,
            state_interface_table,
            state_vlan_member_table,
            state_lag_table,
        };

        L2McMgr l2mcmgr(&conf_db, &app_db, &state_db, tables);
        l2mcmgr.ipcInitL2McMgr();
        l2mcmgr.isPortInitComplete(&state_db);
        l2mcmgr.getL2McPortList(&state_db);
        l2mcmgr.getL2McCfgParams(&conf_db);
        vector<Orch *> cfgOrchList = {&l2mcmgr};
        Select s;
        for (Orch *o: cfgOrchList)
        {
            s.addSelectables(o->getSelectables());
        }
        SWSS_LOG_NOTICE("L2MCMGrd Start main loop log_debug:%d",swss::Logger::getInstance().getMinPrio());
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
                l2mcmgr.doTask();
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
