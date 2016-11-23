#ifndef SWSS_NEIGHORCH_H
#define SWSS_NEIGHORCH_H

#include "orch.h"
#include "observer.h"
#include "portsorch.h"
#include "intfsorch.h"

#include "ipaddress.h"

struct NeighborEntry
{
    IpAddress           ip_address;     // neighbor IP address
    string              alias;          // incoming interface alias

    bool operator<(const NeighborEntry &o) const
    {
        return tie(ip_address, alias) < tie(o.ip_address, o.alias);
    }

    bool operator==(const NeighborEntry &o) const
    {
        return (ip_address == o.ip_address) && (alias == o.alias);
    }

    bool operator!=(const NeighborEntry &o) const
    {
        return !(*this == o);
    }
};

struct NextHopEntry
{
    sai_object_id_t     next_hop_id;    // next hop id
    int                 ref_count;      // reference count
};

/* NeighborTable: NeighborEntry, neighbor MAC address */
typedef map<NeighborEntry, MacAddress> NeighborTable;
/* NextHopTable: next hop IP address, NextHopEntry */
typedef map<IpAddress, NextHopEntry> NextHopTable;

struct NeighborUpdate
{
    NeighborEntry entry;
    MacAddress mac;
    bool add;
};

class NeighOrch : public Orch, public Subject
{
public:
    NeighOrch(DBConnector *db, string tableName, IntfsOrch *intfsOrch);

    bool hasNextHop(IpAddress);

    sai_object_id_t getNextHopId(IpAddress);
    int getNextHopRefCount(IpAddress);

    void increaseNextHopRefCount(IpAddress);
    void decreaseNextHopRefCount(IpAddress);

    bool getNeighborEntry(const IpAddress&, NeighborEntry&, MacAddress&);

private:
    IntfsOrch *m_intfsOrch;

    NeighborTable m_syncdNeighbors;
    NextHopTable m_syncdNextHops;

    bool addNextHop(IpAddress, string);
    bool removeNextHop(IpAddress, string);

    bool addNeighbor(NeighborEntry, MacAddress);
    bool removeNeighbor(NeighborEntry);

    void doTask(Consumer &consumer);
};

#endif /* SWSS_NEIGHORCH_H */
