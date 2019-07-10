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

    typedef struct VxlanInfo
    {
        std::string m_vxlanTunnel;
        std::string m_sourceIp;
        std::string m_vnet;
        std::string m_vni;
        std::string m_vxlan;
        std::string m_vxlanIf;
    } VxlanInfo;
    ~VxlanMgr();
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

    bool createVxlan(const VxlanInfo & info);
    bool deleteVxlan(const VxlanInfo & info);

    void clearAllVxlanDevices();

    ProducerStateTable m_appVxlanTunnelTable,m_appVxlanTunnelMapTable;
    Table m_cfgVxlanTunnelTable,m_cfgVnetTable,m_stateVrfTable,m_stateVxlanTable;

    /*
    * Vxlan Tunnel Cache
    * Key: tunnel name
    * Value: Field Value pairs of vxlan tunnel
    */
    std::map<std::string, std::vector<FieldValueTuple> > m_vxlanTunnelCache;
    /*
    * Vnet Cache
    * Key: Vnet name
    * Value: Vxlan information of this vnet
    */
    std::map<std::string, VxlanInfo> m_vnetCache;
};

}

#endif
