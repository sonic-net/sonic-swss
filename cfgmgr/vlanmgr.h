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
