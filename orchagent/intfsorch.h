#ifndef SWSS_INTFSORCH_H
#define SWSS_INTFSORCH_H

#include "orch.h"
#include "portsorch.h"
#include "vrforch.h"
#include "timer.h"

#include "ipaddresses.h"
#include "ipprefix.h"
#include "macaddress.h"

#include <map>
#include <set>

extern sai_object_id_t gVirtualRouterId;
extern MacAddress gMacAddress;

#define RIF_STAT_COUNTER_FLEX_COUNTER_GROUP "RIF_STAT_COUNTER"

struct IntfsEntry
{
    std::set<IpPrefix>  ip_addresses;
    int                 ref_count;
};

typedef map<string, IntfsEntry> IntfsTable;

class IntfsOrch : public Orch
{
public:
    IntfsOrch(DBConnector *db, string tableName, VRFOrch *vrf_orch);

    sai_object_id_t getRouterIntfsId(const string&);

    void increaseRouterIntfsRefCount(const string&);
    void decreaseRouterIntfsRefCount(const string&);

    bool setRouterIntfsMtu(Port &port);
    std::set<IpPrefix> getSubnetRoutes();

    void generateInterfaceMap();
    void addRifToFlexCounter(const string&, const string&, const string&);
    void removeRifFromFlexCounter(const string&, const string&);

    bool setIntf(const string& alias, sai_object_id_t vrf_id = gVirtualRouterId, const IpPrefix *ip_prefix = nullptr);
    bool removeIntf(const string& alias, sai_object_id_t vrf_id = gVirtualRouterId, const IpPrefix *ip_prefix = nullptr);

    void addIp2MeRoute(sai_object_id_t vrf_id, const IpPrefix &ip_prefix);
    void removeIp2MeRoute(sai_object_id_t vrf_id, const IpPrefix &ip_prefix);

    const IntfsTable& getSyncdIntfses(void)
    {
        return m_syncdIntfses;
    }

private:

    SelectableTimer* m_updateMapsTimer = nullptr;
    std::vector<Port> m_rifsToAdd;

    VRFOrch *m_vrfOrch;
    IntfsTable m_syncdIntfses;
    map<string, string> m_vnetInfses;
    void doTask(Consumer &consumer);
    void doTask(SelectableTimer &timer);

    shared_ptr<DBConnector> m_counter_db;
    shared_ptr<DBConnector> m_flex_db;
    shared_ptr<DBConnector> m_asic_db;
    unique_ptr<Table> m_rifNameTable;
    unique_ptr<Table> m_rifTypeTable;
    unique_ptr<Table> m_vidToRidTable;
    unique_ptr<ProducerTable> m_flexCounterTable;
    unique_ptr<ProducerTable> m_flexCounterGroupTable;

    std::string getRifFlexCounterTableKey(std::string s);

    int getRouterIntfsRefCount(const string&);

    bool addRouterIntfs(sai_object_id_t vrf_id, Port &port);
    bool removeRouterIntfs(Port &port);

    void addSubnetRoute(const Port &port, const IpPrefix &ip_prefix);
    void removeSubnetRoute(const Port &port, const IpPrefix &ip_prefix);

    void addDirectedBroadcast(const Port &port, const IpPrefix &ip_prefix);
    void removeDirectedBroadcast(const Port &port, const IpPrefix &ip_prefix);
};

#endif /* SWSS_INTFSORCH_H */
