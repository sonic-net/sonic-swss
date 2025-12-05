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
#define MLD_VERSION_1 1
#define MLD_VERSION_2 2
#define RESTORE_MAX_WAIT_TIME 120

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
    bool isRestoreDone();
  

private:
    ProducerStateTable m_appL2mcSuppressTableProducer;
    Table m_cfgL2McGlobalTable;
    Table m_cfgL2McStaticTable;
    Table m_cfgVlanMemberTable;
    Table m_stateVlanTable;
    Table m_cfgL2McMrouterTable;
    Table m_stateVlanMemberTable;
    Table m_stateInterfaceTableName;
    Table m_cfgLagMemberTable;
    Table m_stateLagTable;
    Table m_appPortTable;
    Table m_appLagTable;
    Table m_statePortTable;
    Table m_appL2mcVlanTable;
    Table m_appL2mcGrpMemTable;
    Table m_appL2mcMrouterTable;
    Table m_statel2mcdLocalMemberTable;
    Table m_statel2mcdLocalMrouterTable;
    Table m_cfgL2McMldGlobalTable;
    Table m_cfgL2McMldStaticTable;
    Table m_cfgL2McMldMrouterTable;

    NotificationConsumer* m_l2mcNotificationConsumer;
    NotificationConsumer* m_l2mcMrouterNotificationConsumer;
    NotificationConsumer* m_l2mcCfgparaDoneNotificationConsumer;
    
    bool m_warmstart;
    map<string, string> m_l2mcentry;
    map<string, string> m_l2mcVlanentry;
    map<string, string> m_l2mcGrpMementry;
    map<string, string> m_l2mcMrouterentry;
    map<int, bool> m_vlanIgmpSnoopMap;
    map<int, bool> m_vlanMldSnoopMap;
    bool l2mcVlanReplayDone;
    bool l2mcGrpMemReplayDone;
    bool l2mcMouterReplayDone;

    int l2mcd_fd;
    void doTask(Consumer &consumer);
    void doTask(NotificationConsumer &consumer);
    void doL2McGlobalTask(Consumer &consumer);
    void doL2McMldGlobalTask(Consumer &consumer);
    void doL2McStaticEntryTask(Consumer &consumer);
    void doL2McMldStaticEntryTask(Consumer &consumer);
    void doVlanUpdateTask (Consumer &consumer);
    void doL2McMrouterUpdateTask(Consumer &consumer);
    void doL2McMldMrouterUpdateTask(Consumer &consumer);
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
    void sendL2McSnoopConfig(const string &key,int vlan_id,int afi,const string &op,const vector<FieldValueTuple> &tuples);

};

}
