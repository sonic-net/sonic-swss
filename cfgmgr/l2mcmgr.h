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
#include <sstream>
#include <thread>
#include <vector>

#include "../../sonic-l2mcd/include/l2mcd_ipc.h" 
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

#define IGMP_VERSION_2 2
#define IGMP_VERSION_3 3

using namespace std;
using namespace swss;

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
    ProducerStateTable m_appL2mcSuppressTableProducer;
    Table m_cfgL2McGlobalTable;
    Table m_cfgL2McStaticTable;
    Table m_stateVlanTable;
    Table m_cfgL2McMrouterTable;
    Table m_stateVlanMemberTable;
    Table m_stateInterfaceTableName;
    Table m_cfgLagMemberTable;
    Table m_stateLagTable;
    Table m_appPortTable;
    Table m_appLagTable;
    Table m_appL2mcVlanTable;
    Table m_appL2mcGrpMemTable;
    Table m_appL2mcMrouterTable;
    Table m_statel2mcdLocalMemberTable;
    Table m_statel2mcdLocalMrouterTable;
    NotificationConsumer* m_l2mcNotificationConsumer;
    NotificationConsumer* m_l2mcMrouterNotificationConsumer;
    NotificationConsumer* m_l2mcCfgparaDoneNotificationConsumer;
    
    bool m_warmstart;
    map<string, string> m_l2mcentry;
    map<string, string> m_l2mcVlanentry;
    map<string, string> m_l2mcGrpMementry;
    map<string, string> m_l2mcMrouterentry;
    bool l2mcVlanReplayDone;
    bool l2mcGrpMemReplayDone;
    bool l2mcMouterReplayDone;

    int l2mcd_fd;
    void doTask(Consumer &consumer);
    void doTask(NotificationConsumer &consumer);
    void doL2McGlobalTask(Consumer &consumer);
    void doL2McStaticEntryTask(Consumer &consumer);
    void doVlanUpdateTask (Consumer &consumer);
    void doL2McMrouterUpdateTask(Consumer &consumer);
    void doL2McVlanMemUpdateTask(Consumer &consumer);
    void updateVlanMember(const string vlan_id);
    void updateMrouterEntry(const string vlan_id, const string ifname);
    void updateGrpStaticEntry(const string vlan_id, const string ifname);
    void doL2McSuppressUpdateTask(Consumer &consumer);
    void doL2McL3InterfaceUpdateTask(Consumer &consumer);
    void doL2McLagMemberUpdateTask(Consumer &consumer);
    void doL2McInterfaceUpdateTask(Consumer &consumer);
    void doL2McProcRemoteEntries(string op, string key, string key_seperator, int type);
    void doL2McProcRemoteMrouterEntries(string op, string key, string key_seperator);
    int getVlanMembers(const string &vlanKey, vector<PORT_ATTR>&port_list);
    int getPortOperState(string if_name);
    void getL2mcVlanEntry(map<string, string> &entry);
    void getStateMrouterEntry(map<string, string> &entry);
    void getStateGrpMemEntry(map<string, string> &entry);
    bool findStateMrouterEntry(string key);
    bool findStateGrpMemEntry(string key);
    bool findL2mcVlanEntry(string key);
    void removeL2mcMrouterEntry(string key);
    void removeL2mcGrpMemEntry(string key);
    void removeL2mcVlanEntry(string key);
};

}
