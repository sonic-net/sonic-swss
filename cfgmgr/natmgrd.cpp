/*
 * Copyright 2019 Broadcom Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <unistd.h>
#include <vector>
#include <sstream>
#include <fstream>
#include <iostream>
#include <mutex>
#include <algorithm>
#include <signal.h>
#include "dbconnector.h"
#include "select.h"
#include "exec.h"
#include "schema.h"
#include "macaddress.h"
#include "producerstatetable.h"
#include "notificationproducer.h"
#include "natmgr.h"
#include "shellcmd.h"
#include "warm_restart.h"

using namespace std;
using namespace swss;

/* select() function timeout retry time, in millisecond */
#define SELECT_TIMEOUT 1000

/*
 * Following global variables are defined here for the purpose of
 * using existing Orch class which is to be refactored soon to
 * eliminate the direct exposure of the global variables.
 *
 * Once Orch class refactoring is done, these global variables
 * should be removed from here.
 */
int       gBatchSize = 0;
bool      gSwssRecord = false;
bool      gLogRotate = false;
ofstream  gRecordOfs;
string    gRecordFile;
bool      gResponsePublisherRecord = false;
bool      gResponsePublisherLogRotate = false;
ofstream  gResponsePublisherRecordOfs;
string    gResponsePublisherRecordFile;
mutex     gDbMutex;
NatMgr    *natmgr = NULL;

NotificationConsumer   *timeoutNotificationsConsumer = NULL;
NotificationConsumer   *flushNotificationsConsumer = NULL;

static volatile sig_atomic_t gExit = 0;

std::shared_ptr<swss::NotificationProducer> cleanupNotifier;

static struct sigaction old_sigaction;

void sigterm_handler(int signo)
{
    SWSS_LOG_ENTER();

    if (old_sigaction.sa_handler != SIG_IGN && old_sigaction.sa_handler != SIG_DFL) {
        old_sigaction.sa_handler(signo);
    }

    gExit = 1;
}

void cleanup()
{
    int ret = 0;
    std::string res;
    const std::string conntrackFlush            = "conntrack -F";

    SWSS_LOG_ENTER();

    /*If there are any conntrack entries, clean them */
    ret = swss::exec(conntrackFlush, res);
    if (ret)
    {
        SWSS_LOG_ERROR("Command '%s' failed with rc %d", conntrackFlush.c_str(), ret);
    }

    /* Send notification to Orchagent to clean up the REDIS and ASIC database */
    if (cleanupNotifier != NULL)
    {
        SWSS_LOG_NOTICE("Sending notification to orchagent to cleanup NAT entries in REDIS/ASIC");

        std::vector<swss::FieldValueTuple> entry;

        cleanupNotifier->send("nat_cleanup", "all", entry);
    }
    
    if (natmgr)
    {
        natmgr->removeStaticNatIptables();
        natmgr->removeStaticNaptIptables();
        natmgr->removeDynamicNatRules();

        natmgr->cleanupMangleIpTables();
        natmgr->cleanupPoolIpTable();
    }
}

int main(int argc, char **argv)
{
    Logger::linkToDbNative("natmgrd");
    SWSS_LOG_ENTER();

    SWSS_LOG_NOTICE("--- Starting natmgrd ---");

    try
    {
        vector<string> cfg_tables = {
            CFG_STATIC_NAT_TABLE_NAME,
            CFG_STATIC_NAPT_TABLE_NAME,
            CFG_NAT_POOL_TABLE_NAME,
            CFG_NAT_BINDINGS_TABLE_NAME,
            CFG_NAT_GLOBAL_TABLE_NAME,
            CFG_INTF_TABLE_NAME,
            CFG_LAG_INTF_TABLE_NAME,
            CFG_VLAN_INTF_TABLE_NAME,
            CFG_LOOPBACK_INTERFACE_TABLE_NAME,
            CFG_ACL_TABLE_TABLE_NAME,
            CFG_ACL_RULE_TABLE_NAME
        };

        DBConnector cfgDb("CONFIG_DB", 0);
        DBConnector appDb("APPL_DB", 0);
        DBConnector stateDb("STATE_DB", 0);

        cleanupNotifier = std::make_shared<swss::NotificationProducer>(&appDb, "NAT_DB_CLEANUP_NOTIFICATION");

        struct sigaction sigact = {};
        sigact.sa_handler = sigterm_handler;
        if (sigaction(SIGTERM, &sigact, &old_sigaction))
        {
            SWSS_LOG_ERROR("failed to setup SIGTERM action handler");
            exit(EXIT_FAILURE);
        }

        natmgr = new NatMgr(&cfgDb, &appDb, &stateDb, cfg_tables);

        natmgr->isPortInitDone(&appDb);
        
        std::vector<Orch *> cfgOrchList = {natmgr};

        swss::Select s;
        for (Orch *o : cfgOrchList)
        {
            s.addSelectables(o->getSelectables());
        }

        timeoutNotificationsConsumer = new NotificationConsumer(&appDb, "SETTIMEOUTNAT");
        s.addSelectable(timeoutNotificationsConsumer);

        flushNotificationsConsumer = new NotificationConsumer(&appDb, "FLUSHNATENTRIES");
        s.addSelectable(flushNotificationsConsumer);

        SWSS_LOG_NOTICE("starting main loop");
        while (!gExit)
        {
            Selectable *sel;
            int ret;

            ret = s.select(&sel, SELECT_TIMEOUT);
            if (ret == Select::ERROR)
            {
                SWSS_LOG_NOTICE("Error: %s!", strerror(errno));
                continue;
            }

            if (sel == timeoutNotificationsConsumer)
            {
               std::string op;
               std::string data;
               std::vector<swss::FieldValueTuple> values;

               timeoutNotificationsConsumer->pop(op, data, values);
               natmgr->timeoutNotifications(op, data);
               continue;
            }

            if (sel == flushNotificationsConsumer)
            {
               std::string op;
               std::string data;
               std::vector<swss::FieldValueTuple> values;

               flushNotificationsConsumer->pop(op, data, values);
               natmgr->flushNotifications(op, data);
               continue;
            }

            if (ret == Select::TIMEOUT)
            {
                natmgr->doTask();
                continue;
            }

            auto *c = (Executor *)sel;
            c->execute();
        }

        cleanup();
    }
    catch(const std::exception &e)
    {
        SWSS_LOG_ERROR("Runtime error: %s", e.what());
        return EXIT_FAILURE;
    }

    return 0;
}
