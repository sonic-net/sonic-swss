#ifndef __VLANMGR__
#define __VLANMGR__

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
    void doVnetTask(Consumer &consumer);

    /*
     * Query the state of vrf by STATE_VRF_TABLE
     * Return
     *  true: The state of vrf is OK 
     *  false: the vrf hasn't been created
     */
    bool isVrfStateOk(const std::string & vrfName);
    /*
     * Get Vxlan information(vnet Name, vni, src_ip) by querying 
     * CFG_VXLAN_TUNNEL_TABLE and CFG_VNET_TABLE
     * Return
     *  true: all information can be got successfully.
     *  false: missing some information
     */
    bool getVxlanInfo(const std::string & vnetName, VxlanInfo & info);

    void createVxlan(VxlanInfo & info);
    void deleteVxlan(VxlanInfo & info);


    ProducerStateTable m_appVxlanTableProducer;
    Table m_cfgVxlanTunnelTable;
    Table m_cfgVnetTable;
    Table m_stateVrfTable;

};

}

#endif
