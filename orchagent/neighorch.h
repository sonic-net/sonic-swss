#ifndef SWSS_NEIGHORCH_H
#define SWSS_NEIGHORCH_H

#include "orch.h"
#include "portsorch.h"
#include "routeorch.h"

struct NeighborEntry
{
    IpAddress           ip_address;     // neighbor IP address
    string              alias;          // incoming interface alias

    bool operator<(const NeighborEntry &o) const
    {
        return tie(ip_address, alias) < tie(o.ip_address, o.alias);
    }
};


/* NeighborTable: NeighborEntry, neighbor MAC address */
typedef map<NeighborEntry, MacAddress> NeighborTable;

class NeighOrch : public Orch
{
public:
    NeighOrch(DBConnector *db, string tableName, PortsOrch *portsOrch, RouteOrch *routeOrch) :
        Orch(db, tableName), m_portsOrch(portsOrch), m_routeOrch(routeOrch) {};
    const char* const delimiter         = ";";
    const char* const neigh_field_name  = "neigh";
private:
    PortsOrch *m_portsOrch;
    RouteOrch *m_routeOrch;

    virtual void doTask(_in_ Consumer& consumer_info);

    NeighborTable m_syncdNeighbors;

    bool addNeighbor(NeighborEntry, MacAddress);
    bool removeNeighbor(NeighborEntry);
};

#endif /* SWSS_NEIGHORCH_H */
