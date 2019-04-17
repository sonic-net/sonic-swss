#ifndef __VXLANMGR__
#define __VXLANMGR__

#include "dbconnector.h"
#include "producerstatetable.h"
#include "orch.h"

#include <map>
#include <memory>
#include <string>

namespace swss {

class VxlanMgr : public Orch
{
public:
    VxlanMgr(DBConnector *cfgDb, DBConnector *appDb, DBConnector *stateDb, const vector<std::string> &tableNames);
    using Orch::doTask;

     typedef std::map<std::string, std::string> VxlanInfo;
private:
    void doTask(Consumer &consumer);

    bool doVxlanCreateTask(const KeyOpFieldsValuesTuple & t);
    bool doVxlanDeleteTask(const KeyOpFieldsValuesTuple & t);

    bool doVxlanTunnelCreateTask(const KeyOpFieldsValuesTuple & t);
    bool doVxlanTunnelDeleteTask(const KeyOpFieldsValuesTuple & t);

    bool doVxlanTunnelMapCreateTask(const KeyOpFieldsValuesTuple & t);
    bool doVxlanTunnelMapDeleteTask(const KeyOpFieldsValuesTuple & t);

    /*
    * Query the state of vrf by STATE_VRF_TABLE
    * Return
    *  true: The state of vrf is OK 
    *  false: the vrf hasn't been created
    */
    bool isVrfStateOk(const std::string & vrfName);
    bool isVxlanStateOk(const std::string & vxlanName);
    /*
    * Get Vxlan information(vnet Name, vni, src_ip) by querying 
    * CFG_VXLAN_TUNNEL_TABLE and CFG_VNET_TABLE
    * Return
    *  true: all information can be got successfully.
    *  false: missing some information
    */
    bool getVxlanInfo(const std::string & vnetName, VxlanInfo & info);


    bool createVxlan(const VxlanInfo & info);
    bool deleteVxlan(const VxlanInfo & info);


    ProducerStateTable m_appVxlanTunnelTable,m_appVxlanTunnelMapTable;
    Table m_cfgVxlanTunnelTable,m_cfgVnetTable,m_stateVrfTable,m_stateVxlanTable;

    std::map<std::string, VxlanInfo> m_vnetVxlanInfoMapping;
};

}

#endif
