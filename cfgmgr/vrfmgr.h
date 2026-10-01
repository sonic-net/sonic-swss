#ifndef __VRFMGR__
#define __VRFMGR__

#include <string>
#include <map>
#include <set>
#include "dbconnector.h"
#include "producerstatetable.h"
#include "orch.h"

using namespace std;

namespace swss {

typedef std::unordered_map<std::string, uint32_t> VRFNameVNIMapTable;
typedef std::unordered_map<std::string, std::string> VtepNVOMapTable;

class VrfMgr : public Orch
{
public:
    VrfMgr(DBConnector *cfgDb, DBConnector *appDb, DBConnector *stateDb, const std::vector<std::string> &tableNames);
    using Orch::doTask;
    VtepNVOMapTable m_evpnVxlanTunnel;

    uint32_t getVRFmappedVNI(const std::string& vrf_name);

private:
    bool delLink(const std::string& vrfName);
    bool setLink(const std::string& vrfName);
    bool isVrfObjExist(const std::string& vrfName);
    void recycleTable(uint32_t table);
    uint32_t getFreeTable(void);
    void handleVnetConfigSet(KeyOpFieldsValuesTuple &t);
    bool doVrfEvpnNvoAddTask(const KeyOpFieldsValuesTuple & t);
    bool doVrfEvpnNvoDelTask(const KeyOpFieldsValuesTuple & t);
    bool validateVrfVniConfig(const KeyOpFieldsValuesTuple & t, uint32_t& vni, std::string& s_vni);
    bool doVrfVxlanTableCreateTask(const KeyOpFieldsValuesTuple & t);
    bool doVrfVxlanTableRemoveTask(const KeyOpFieldsValuesTuple & t);
    bool doVrfVxlanTableUpdate(const string& vrf_name, const string& vni, bool add);
    void syncVrfVxlanTableForTunnel(const std::string& tunnel_name);
    std::vector<std::string> getVxlanTunnelsForVni(const std::string& vni);
    void doTask(Consumer &consumer);

    std::map<std::string, uint32_t> m_vrfTableMap;
    std::set<uint32_t> m_freeTables;
    VRFNameVNIMapTable m_vrfVniMapTable;

    Table m_stateVrfTable, m_stateVrfObjectTable, m_cfgVxlanTunnelMapTable;
    ProducerStateTable m_appVrfTableProducer, m_appVnetTableProducer, m_appVxlanVrfTableProducer;
};

}

#endif
