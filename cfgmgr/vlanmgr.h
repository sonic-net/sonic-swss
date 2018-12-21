#ifndef __VLANMGR__
#define __VLANMGR__

#include "dbconnector.h"
#include "producerstatetable.h"
#include "orch.h"

#include <set>
#include <map>
#include <string>

namespace swss {

#define VLAN_STATUS_INITIALIZED(admin_status) (!(admin_status).empty())

class VlanMgr : public Orch
{
public:
    VlanMgr(DBConnector *cfgDb, DBConnector *appDb, DBConnector *stateDb, const vector<string> &tableNames);
    using Orch::doTask;

private:
    ProducerStateTable m_appVlanTableProducer, m_appVlanMemberTableProducer;
    Table m_cfgVlanTable, m_cfgVlanMemberTable;
    Table m_statePortTable, m_stateLagTable;
    Table m_stateVlanTable, m_stateVlanMemberTable;
    Table m_appVlanTable;
    std::set<std::string> m_vlans;

    void doTask(Consumer &consumer);
    void doVlanTask(Consumer &consumer);
    void doVlanMemberTask(Consumer &consumer);
    void processUntaggedVlanMembers(string vlan, const string &members);

    bool addHostVlan(int vlan_id);
    bool removeHostVlan(int vlan_id);

	bool updateVlanStatusInit(vector<FieldValueTuple> &fvVector, const int vlan_id);
    bool updateVlanStatusByAdmin(vector<FieldValueTuple> &fvVector, const int vlan_id, const string &admin_status);

    bool setHostVlanAdminState(int vlan_id, const string &admin_status);
    bool setHostVlanMtu(int vlan_id, uint32_t mtu);
    bool addHostVlanMember(int vlan_id, const string &port_alias, const string& tagging_mode);
    bool removeHostVlanMember(int vlan_id, const string &port_alias);
    bool isMemberStateOk(const string &alias);
    bool isVlanStateOk(const string &alias);
    bool isVlanMacOk();
    bool isVlanMemberStateOk(const string &vlanMemberKey);
};

}

#endif
