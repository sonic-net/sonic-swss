#ifndef SWSS_INTFSORCH_H
#define SWSS_INTFSORCH_H

#include "orch.h"
#include "portsorch.h"

#include "ipaddresses.h"
#include "ipprefix.h"
#include "macaddress.h"

#include <map>

extern sai_object_id_t gVirtualRouterId;
extern MacAddress gMacAddress;

struct IntfsEntry
{
    IpAddresses         ip_addresses;
    int                 ref_count;
};

typedef map<string, IntfsEntry> IntfsTable;

class IntfsOrch : public Orch
{
public:
    IntfsOrch(DBConnector *db, string tableName);

    sai_object_id_t getRouterIntfsId(string);

    void increaseRouterIntfsRefCount(const string);
    void decreaseRouterIntfsRefCount(const string);
private:
    IntfsTable m_syncdIntfses;
    void doTask(Consumer &consumer);

    int getRouterIntfsRefCount(string);

    bool addRouterIntfs(Port &port);
    bool removeRouterIntfs(Port &port);

    void addSubnetRoute(const Port &port, const IpPrefix &ip_prefix);
    void removeSubnetRoute(const Port &port, const IpPrefix &ip_prefix);

    void addIp2MeRoute(const Port &port, const IpPrefix &ip_prefix);
    void removeIp2MeRoute(const Port &port, const IpPrefix &ip_prefix);
};

#endif /* SWSS_INTFSORCH_H */
