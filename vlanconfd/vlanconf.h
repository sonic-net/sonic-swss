#ifndef __VLANCONF__
#define __VLANCONF__

#include "dbconnector.h"
#include "producerstatetable.h"
#include "cfgorch.h"

#include <set>
#include <map>
#include <string>

namespace swss {

class VlanConf : public CfgOrch
{
public:
    VlanConf(DBConnector *cfgDb, DBConnector *appDb, DBConnector *stateDb, vector<string> tableNames);
    void syncCfgDB();
private:
	ProducerStateTable m_appVlanTableProducer, m_appVlanMemberTableProducer;
    Table m_cfgVlanTable, m_cfgVlanMemberTable;
    Table m_statePortTable, m_stateLagTable;
    Table m_stateVlanTable;
    std::set<std::string> m_vlans;

    void doTask(Consumer &consumer);
    void doVlanTask(Consumer &consumer);
    void doVlanMemberTask(Consumer &consumer);
    void processUntaggedVlanMembers(string vlan, string &members);

    bool addHostVlan(int vlan_id);
    bool removeHostVlan(int vlan_id);
    bool setHostVlanAdminState(int vlan_id, string &admin_status);
    bool setHostVlanMtu(int vlan_id, uint32_t mtu);
    bool addHostVlanMember(int vlan_id, string &port_alias, string& tagging_mode);
    bool removeHostVlanMember(int vlan_id, string &port_alias);
    bool isMemberStateOk(string &alias);
    bool isVlanStateOk(string &alias);
    bool isVlanMacOk();
};

}

#endif
