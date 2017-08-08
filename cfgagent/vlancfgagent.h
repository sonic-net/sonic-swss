#ifndef __VLANCFGORCH__
#define __VLANCFGORCH__

#include "dbconnector.h"
#include "cfgorch.h"

#include <set>
#include <map>
#include <string>
#include "macaddress.h"

namespace swss {

class VlanCfgAgent : public CfgOrch
{
public:
    VlanCfgAgent(DBConnector *cfgDb, DBConnector *appDb, vector<string> tableNames);
    void SyncCfgDB();
private:
	ProducerStateTable m_vlanTableProducer, m_vlanMemberTableProducer;
    Table m_cfgVlanTableConsumer, m_cfgVlanMemberTableConsumer;
    std::set<std::string> m_vlans;

    void doTask(Consumer &consumer);
    void doVlanTask(Consumer &consumer);
    void doVlanMemberTask(Consumer &consumer);

    bool addHostVlan(int vlan_id);
    bool removeHostVlan(int vlan_id);
    bool setHostVlanAdminState(int vlan_id, string &admin_status);
    bool setHostVlanMtu(int vlan_id, uint32_t mtu);
    bool addHostVlanMember(int vlan_id, string &port_alias, string& tagging_mode);
    bool removeHostVlanMember(int vlan_id, string &port_alias, bool detach);
};

}

#endif
