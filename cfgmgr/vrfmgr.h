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

class VrfMgr : public Orch
{
public:
    VrfMgr(DBConnector *cfgDb, DBConnector *appDb, DBConnector *stateDb, const std::vector<std::string> &tableNames);
    using Orch::doTask;
    std::string m_evpnVxlanTunnel;

    uint32_t getVRFmappedVNI(const std::string& vrf_name);
    void retryPendingKernelVrfs();

private:
    bool delLink(const std::string& vrfName);
    bool setLink(const std::string& vrfName);
    bool parseKernelVrfFallbackState(const std::vector<FieldValueTuple>& values);
    void loadKernelVrfFallbackState();
    void handleKernelVrfFallbackConfig(const KeyOpFieldsValuesTuple& t);
    bool runKernelVrfRouteCommand(uint32_t table, bool ipv6);
    bool reconcileKernelVrf(const std::string& vrfName);
    void reconcileAllKernelVrfs();
    bool isVrfObjExist(const std::string& vrfName);
    void recycleTable(uint32_t table);
    uint32_t getFreeTable(void);
    void handleVnetConfigSet(KeyOpFieldsValuesTuple &t);
    bool doVrfEvpnNvoAddTask(const KeyOpFieldsValuesTuple & t);
    bool doVrfEvpnNvoDelTask(const KeyOpFieldsValuesTuple & t);
    bool doVrfVxlanTableCreateTask(const KeyOpFieldsValuesTuple & t);
    bool doVrfVxlanTableRemoveTask(const KeyOpFieldsValuesTuple & t);
    bool doVrfVxlanTableUpdate(const string& vrf_name, const string& vni, bool add);
    void VrfVxlanTableSync(bool add);
    void doTask(Consumer &consumer);

    std::map<std::string, uint32_t> m_vrfTableMap;
    std::set<uint32_t> m_freeTables;
    std::set<std::string> m_pendingKernelVrfReconcile;
    VRFNameVNIMapTable m_vrfVniMapTable;
    bool m_kernelVrfFallbackEnabled = false;

    Table m_cfgKernelVrfFallbackTable, m_stateVrfTable, m_stateVrfObjectTable;
    ProducerStateTable m_appVrfTableProducer, m_appVnetTableProducer, m_appVxlanVrfTableProducer;
};

}

#endif
