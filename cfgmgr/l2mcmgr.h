/*
 * Copyright 2019 Broadcom. All rights reserved.
 * The term "Broadcom" refers to Broadcom Inc. and/or its subsidiaries.
 * 
 * cfgmgr/l2mcmgr.h
 */ 

#pragma once

#include <set>
#include <string>
#include <unistd.h>
#include <bitset>
#include <iostream>
#include <algorithm>
#include <sstream>
#include <thread>
#include "l2mcd_ipc.h" 
#include <sys/stat.h>
#include "dbconnector.h"
#include "netmsg.h"
#include "orch.h"
#include "producerstatetable.h"
#include <stddef.h>
#include <algorithm>
#include "logger.h"
#include "tokenize.h"
#include "warm_restart.h"
#include "notifier.h"

#define L2MCMGR_IPC_SOCK_NAME "/var/run/l2mcdmgr_ipc.sock"

namespace swss {

class L2McMgr : public Orch
{
public:
    L2McMgr(DBConnector *cfgDb, DBConnector *appDb, DBConnector *staDb,
            const vector<TableConnector> &tables);

    using Orch::doTask;
	void ipcInitL2McMgr();
    int sendMsgL2Mcd(L2MCD_MSG_TYPE msgType, uint32_t msgLen, void *data);
    int isPortInitComplete(DBConnector *app_db);
    int getL2McPortList(DBConnector *state_db);
    int getL2McCfgParams(DBConnector *cfgDb);
  

private:
    Table m_cfgL2McGlobalTable;
    Table m_cfgL2McStaticTable;
    Table m_cfgL2McMrouterTable;
    Table m_stateVlanMemberTable;
    Table m_stateInterfaceTableName;
    Table m_cfgLagMemberTable;
    Table m_stateLagTable;
    Table m_appPortTable;
    Table m_appLagTable;
    Table m_statel2mcdLocalMemberTable;
    NotificationConsumer* m_l2mcNotificationConsumer;
    NotificationConsumer* m_l2mcMrouterNotificationConsumer;
    
    int l2mcd_fd;
    void doTask(Consumer &consumer);
    void doTask(NotificationConsumer &consumer);
    void doL2McGlobalTask(Consumer &consumer);
    void doL2McStaticEntryTask(Consumer &consumer);
    void doL2McMrouterUpdateTask(Consumer &consumer);
    void doL2McVlanMemUpdateTask(Consumer &consumer);
    void doL2McL3InterfaceUpdateTask(Consumer &consumer);
    void doL2McLagMemberUpdateTask(Consumer &consumer);
    void doL2McInterfaceUpdateTask(Consumer &consumer);
    void doL2McProcRemoteEntries(string op, string key, string key_seperator, int type);
    void doL2McProcRemoteMrouterEntries(string op, string key, string key_seperator);
    int getVlanMembers(const string &vlanKey, vector<PORT_ATTR>&port_list);
    int getPortOperState(string if_name);
};

}
