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
#include <unordered_map>


extern sai_object_id_t gVirtualRouterId;
extern MacAddress gMacAddress;

#define RIF_STAT_COUNTER_FLEX_COUNTER_GROUP "RIF_STAT_COUNTER"


struct IntfsEntry
{
    std::set<IpPrefix>  ip_addresses;
    int                 ref_count;

    IntfsEntry(int rc = 0) : ref_count(rc) {};
    IntfsEntry(IpPrefix p)
    {
        ip_addresses.insert(p);
        ref_count++;
    }
};

typedef map<string, IntfsEntry> IntfsTable;

struct IntfRouteEntry
{
    IpPrefix prefix;
    string   ifName;
    string   type;

    IntfRouteEntry(const IpPrefix &p, string n, string t = "subnet")
        : prefix(p), ifName(n), type(t) {};

    inline bool operator==(const IntfRouteEntry &x) const
    {
        return (prefix == x.prefix && ifName == x.ifName);
    }
};

/*
 * Hashmap to keep track of all interface-specific routes in the system.
 * Indexed by the string associated to each interface-route (either ip2me or
 * subnet or bcast). Values are formed by a list of elements that keep track
 * of each route IpPrefix, as well as the interface on which it was configured.
 *
 * Example:
 *
 *    Key                                        Value
 * ----------             -------------------------------------------------------
 * 10.1.1.0/24 (subnet)   10.1.1.0/24 eth1, 10.1.1.0/24 eth2, 10.1.1.0/24 eth3
 * 10.1.1.1/32 (ip2me)    10.1.1.1/32 eth1, 10.1.1.10/32 eth2, 10.1.1.255/32 eth3
 * 10.1.1.255/32 (bcast)  10.1.1.255/32 eth1, 10.1.1.255/32 eth2
 * ...
 * fe80:1:1/64 (subnet)   fe80:1:1/64 eth1, fe80:1:1/64 eth2
 * fe80:1::1/128 (ip2me)  fe80:1:1::1/128 eth2, fe80:1:1::1/128 eth1
 * fe80:1::5/128 (ip2me)  fe80:1:1::5/128 eth3
 *
 */
typedef unordered_map<string, list<IntfRouteEntry>> IntfRoutesTable;

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

    bool createDefaultIntfRoutes(Port &port);

    bool createIntf(Port            &port,
                    sai_object_id_t  vrf_id = gVirtualRouterId,
                    const IpPrefix  *ip_prefix = nullptr);

    bool deleteIntf(Port            &port,
                    sai_object_id_t  vrf_id,
                    const IpPrefix  *ip_prefix);


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
    IntfRoutesTable m_intfRoutes;

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

    void createIntfRoutes(const IntfRouteEntry &ifRoute, const Port &port);
    void deleteIntfRoutes(const IntfRouteEntry &intRoute, const Port &port);
    void deleteIntfRoute(const IntfRouteEntry &ifRoute, const Port &port);
    void resurrectIntfRoute(const IntfRouteEntry &ifRoute);
    bool trackIntfRouteOverlap(const IntfRouteEntry &ifRoute);

    IpPrefix getIp2mePrefix(const IpPrefix &ip_prefix);
    IpPrefix getBcastPrefix(const IpPrefix &ip_prefix);
};

#endif /* SWSS_INTFSORCH_H */
