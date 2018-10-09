#ifndef SWSS_INTFSORCH_H
#define SWSS_INTFSORCH_H

#include "orch.h"
#include "portsorch.h"
#include "vrforch.h"

#include "ipaddresses.h"
#include "ipprefix.h"
#include "macaddress.h"

#include <map>
#include <set>

extern sai_object_id_t gVirtualRouterId;
extern MacAddress gMacAddress;

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
private:
    VRFOrch *m_vrfOrch;
    IntfsTable m_syncdIntfses;
    void doTask(Consumer &consumer);

    int getRouterIntfsRefCount(const string&);

    bool addRouterIntfs(sai_object_id_t vrf_id, Port &port);
    bool removeRouterIntfs(Port &port);

    void addSubnetRoute(const Port &port, const IpPrefix &ip_prefix);
    void removeSubnetRoute(const Port &port, const IpPrefix &ip_prefix);

    void addIp2MeRoute(sai_object_id_t vrf_id, const IpPrefix &ip_prefix);
    void removeIp2MeRoute(sai_object_id_t vrf_id, const IpPrefix &ip_prefix);

    void addDirectedBroadcast(const Port &port, const IpAddress &ip_addr);
    void removeDirectedBroadcast(const Port &port, const IpAddress &ip_addr);
};

#endif /* SWSS_INTFSORCH_H */
