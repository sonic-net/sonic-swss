#ifndef __STPMGR__
#define __STPMGR__

#include <set>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <bitset>
#include <vector>
#include <string>
#include <unordered_set>

#include "dbconnector.h"
#include "netmsg.h"
#include "orch.h"
#include "producerstatetable.h"
#include <stddef.h>
#include <algorithm>
#include <stp_ipc.h>

#define STPMGRD_SOCK_NAME "/var/run/stpmgrd.sock"

#define TAGGED_MODE    1
#define UNTAGGED_MODE  0
#define INVALID_MODE  -1

#define MAX_VLANS 4096

// Maximum number of instances supported
#define L2_INSTANCE_MAX             MAX_VLANS
#define STP_DEFAULT_MAX_INSTANCES   255
#define INVALID_INSTANCE            -1

#define GET_FIRST_FREE_INST_ID(_idx) \
    while (_idx < (int)l2InstPool.size() && l2InstPool.test(_idx)) ++_idx; \
    l2InstPool.set(_idx)

#define FREE_INST_ID(_idx) l2InstPool.reset(_idx)
#define FREE_ALL_INST_ID() l2InstPool.reset()
#define IS_INST_ID_AVAILABLE() (l2InstPool.count() < max_stp_instances)

namespace swss {

class StpMgr : public Orch
{
public:
    StpMgr(DBConnector *cfgDb, DBConnector *appDb, DBConnector *stateDb,
           const std::vector<TableConnector> &tables);

    using Orch::doTask;
    void ipcInitStpd();
    int  sendMsgStpd(STP_MSG_TYPE msgType, uint32_t msgLen, void *data);
    MacAddress macAddress;
    bool isPortInitDone(DBConnector *app_db);
    uint16_t getStpMaxInstances(void);    

private:
    Table m_cfgStpGlobalTable;
    Table m_cfgStpVlanTable;
    Table m_cfgStpVlanPortTable;
    Table m_cfgStpPortTable;
    Table m_cfgLagMemberTable;
    Table m_cfgVlanMemberTable;
    Table m_stateVlanTable;
    Table m_stateVlanMemberTable;
    Table m_stateLagTable;
    Table m_stateStpTable;
    Table m_cfgMstGlobalTable;
    Table m_cfgMstInstTable;
    Table m_cfgMstInstPortTable;

    std::bitset<L2_INSTANCE_MAX> l2InstPool;
    int stpd_fd;
    enum L2_PROTO_MODE l2ProtoEnabled;
    int m_vlanInstMap[MAX_VLANS];
    bool portCfgDone;
    uint16_t max_stp_instances;
    std::map<std::string, int> m_lagMap;

    bool stpGlobalTask;
    bool stpVlanTask;
    bool stpVlanPortTask;
    bool stpPortTask;
    bool stpMstInstTask;

    void doTask(Consumer &consumer);
    void doStpGlobalTask(Consumer &consumer);
    void doStpVlanTask(Consumer &consumer);
    void doStpVlanPortTask(Consumer &consumer);
    void doStpPortTask(Consumer &consumer);
    void doVlanMemUpdateTask(Consumer &consumer);
    void doLagMemUpdateTask(Consumer &consumer);
    void doStpMstGlobalTask(Consumer &consumer);
    void doStpMstInstTask(Consumer &consumer);
    void doStpMstInstPortTask(Consumer &consumer);

    bool isVlanStateOk(const std::string &alias);
    bool isLagStateOk(const std::string &alias);
    bool isStpPortEmpty();
    bool isStpEnabled(const std::string &intf_name);
    int getAllVlanMem(const std::string &vlanKey, std::vector<PORT_ATTR>& port_list);
    int getAllPortVlan(const std::string &intfKey, std::vector<VLAN_ATTR>& vlan_list);
    int8_t getVlanMemMode(const std::string &key);
    int allocL2Instance(uint32_t vlan_id);
    void deallocL2Instance(uint32_t vlan_id);
    bool isLagEmpty(const std::string &key);
    void processStpPortAttr(const std::string op, std::vector<FieldValueTuple>&tupEntry, const std::string intfName);
    void processStpVlanPortAttr(const std::string op, uint32_t vlan_id, const std::string intfName,
                    std::vector<FieldValueTuple>&tupEntry);
    void processStpMstInstPortAttr(const std::string op, uint16_t mst_id, const std::string intfName,
                                       std::vector<FieldValueTuple>&tupEntry);
    std::vector<uint16_t> parseVlanList(const std::string &vlanStr);
    void updateVlanInstanceMap(int instance, const std::vector<uint16_t>&newVlanList, bool operation);
    bool isInstanceMapped(uint16_t instance);
    std::vector<std::string> getVlanAliasesForInstance(uint16_t instance);
    
    
};

}
#endif
