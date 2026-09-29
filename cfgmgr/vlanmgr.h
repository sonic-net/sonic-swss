#ifndef __VLANMGR__
#define __VLANMGR__

#include "dbconnector.h"
#include "producerstatetable.h"
#include "orch.h"

#include <set>
#include <map>
#include <string>

namespace swss {

class VlanMgr : public Orch
{
public:
    VlanMgr(DBConnector *cfgDb, DBConnector *appDb, DBConnector *stateDb, const std::vector<std::string> &tableNames,
        const std::vector<std::string> &stateTableNames);
    using Orch::doTask;

private:
    ProducerStateTable m_appVlanTableProducer, m_appVlanMemberTableProducer;
    ProducerStateTable m_appFdbTableProducer, m_appPortTableProducer;
    Table m_cfgVlanTable, m_cfgVlanMemberTable;
    Table m_statePortTable, m_stateLagTable;
    Table m_stateVlanTable, m_stateVlanMemberTable;
    std::set<std::string> m_vlans;
    std::set<std::string> m_vlanReplay;
    std::set<std::string> m_vlanMemberReplay;
    /* Ports PAC has left unauthenticated: kernel bridge port kept locked (ingress guard) and
     * with flood/mcast_flood/bcast_flood off (egress hygiene) */
    std::set<std::string> m_pacLockedPorts;
    /* PAC-authorized clients (OPER_FDB entries with discard=false), keyed by the APP_DB FDB key
     * "Vlan<vid>:<mac>". While their port is locked they are mirrored into the kernel bridge
     * FDB as static entries, so that locked admits their CPU-bound frames. */
    struct PacAuthFdb
    {
        std::string port;
        int vlan_id;
        std::string mac;
    };
    std::map<std::string, PacAuthFdb> m_pacAuthFdb;
    bool replayDone;
    std::unordered_map<std::string, std::unordered_map<std::string, std::string>> m_PortVlanMember;
    
    void doTask(Consumer &consumer);
    void doVlanTask(Consumer &consumer);
    void doVlanMemberTask(Consumer &consumer);
    void processUntaggedVlanMembers(std::string vlan, const std::string &members);

    bool addHostVlan(int vlan_id);
    bool removeHostVlan(int vlan_id);
    bool setHostVlanAdminState(int vlan_id, const std::string &admin_status);
    bool setHostVlanMtu(int vlan_id, uint32_t mtu);
    bool setHostVlanMac(int vlan_id, const std::string &mac);
    bool addHostVlanMember(int vlan_id, const std::string &port_alias, const std::string& tagging_mode);
    bool removeHostVlanMember(int vlan_id, const std::string &port_alias);
    bool setHostPortBridgeLocked(const std::string &port_alias, bool locked);
    void setHostPacFdbMirror(const PacAuthFdb &entry, bool add);
    bool isMemberStateOk(const std::string &alias);
    bool isVlanStateOk(const std::string &alias);
    bool isVlanMacOk();
    bool isVlanMemberStateOk(const std::string &vlanMemberKey);
    void doVlanPacPortTask(Consumer &consumer);
    void doVlanPacFdbTask(Consumer &consumer);
    void doVlanPacVlanMemberTask(Consumer &consumer);
    void addPortToVlan(const std::string& port_alias, const std::string& vlan_alias, const std::string& tagging_mode);
    void removePortFromVlan(const std::string& port_alias, const std::string& vlan_alias);
};

}

#endif
