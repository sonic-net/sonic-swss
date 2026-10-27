#ifndef __VRFMGR__
#define __VRFMGR__

#include <string>
#include <map>
#include <set>
#include <unordered_map>
#include "dbconnector.h"
#include "producerstatetable.h"
#include "orch.h"

using namespace std;

namespace swss {

typedef std::unordered_map<std::string, uint32_t> VRFNameVNIMapTable;
typedef std::unordered_map<std::string, std::string> VtepNVOMapTable;

struct VxlanTunnelMapCacheEntry
{
    std::string tunnel_name;
    std::string vni;
};

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
    void syncVrfVxlanTableForVni(const std::string& tunnel_name, const std::string& vni);
    void addVxlanTunnelMapToCache(const std::string& key, const std::string& tunnel_name,
                                  const std::string& vni);
    bool removeVxlanTunnelMapFromCache(const std::string& key, VxlanTunnelMapCacheEntry& entry);
    std::vector<std::string> getVxlanTunnelsForVni(const std::string& vni);
    void doTask(Consumer &consumer);

    std::map<std::string, uint32_t> m_vrfTableMap;
    std::set<uint32_t> m_freeTables;
    std::set<std::string> m_publishedVrfVxlanMappings;
    VRFNameVNIMapTable m_vrfVniMapTable;
    std::unordered_map<std::string, std::string> m_vniVrfMapTable;
    std::unordered_map<std::string, VxlanTunnelMapCacheEntry> m_vxlanTunnelMapCache;
    std::unordered_map<std::string, std::unordered_map<std::string, size_t>> m_vniTunnelRefCount;

    Table m_stateVrfTable, m_stateVrfObjectTable, m_cfgVrfTable, m_cfgVxlanTunnelMapTable;
    ProducerStateTable m_appVrfTableProducer, m_appVnetTableProducer, m_appVxlanVrfTableProducer;
};

}

#endif
