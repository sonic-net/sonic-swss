#ifndef SWSS_NEIGHORCH_H
#define SWSS_NEIGHORCH_H

#include "orch.h"
#include "portsorch.h"

#include "ipaddress.h"
#include "ipaddresses.h"

#define NHGRP_MAX_SIZE 16

struct NeighborEntry
{
    IpAddress           ip_address;     // neighbor IP address
    string              alias;          // incoming interface alias

    bool operator<(const NeighborEntry &o) const
    {
        return tie(ip_address, alias) < tie(o.ip_address, o.alias);
    }
};

struct NextHopEntry
{
    sai_object_id_t     next_hop_id;    // next hop id or next hop group id
    int                 ref_count;      // reference count
};

/* NeighborTable: NeighborEntry, neighbor MAC address */
typedef map<NeighborEntry, MacAddress> NeighborTable;
/* NextHopTable: next hop IP address(es), NextHopEntry */
typedef map<IpAddresses, NextHopEntry> NextHopTable;

class NeighOrch : public Orch
{
public:
    NeighOrch(DBConnector *db, string tableName, PortsOrch *portsOrch) :
        Orch(db, tableName),
        m_portsOrch(portsOrch),
        m_nextHopGroupCount(0) {};

    bool contains(IpAddress);
    bool contains(IpAddresses);

    bool addNextHopGroup(IpAddresses);
    bool removeNextHopGroup(IpAddresses);

    sai_object_id_t getNextHopId(IpAddresses);

    void increaseNextHopRefCount(IpAddresses);
    void decreaseNextHopRefCount(IpAddresses);

private:
    PortsOrch *m_portsOrch;

    int m_nextHopGroupCount;

    NeighborTable m_syncdNeighbors;
    NextHopTable m_syncdNextHops;

    bool addNextHop(IpAddress, Port);
    bool removeNextHop(IpAddress);

    void doTask(Consumer &consumer);
    bool addNeighbor(NeighborEntry, MacAddress);
    bool removeNeighbor(NeighborEntry);
};

#endif /* SWSS_NEIGHORCH_H */
