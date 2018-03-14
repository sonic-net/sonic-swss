#ifndef SWSS_NEIGHORCH_H
#define SWSS_NEIGHORCH_H

#include "orch.h"
#include "observer.h"
#include "portsorch.h"
#include "intfsorch.h"

#include "ipaddress.h"

#define NHFLAGS_IFDOWN                  0x1 // nexthop's outbound i/f is down

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
    uint32_t            nh_flags;       // flags
    string              if_alias;       // i/f name alias
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

    sai_object_id_t getNextHopId(const IpAddress&);
    int getNextHopRefCount(const IpAddress&);

    void increaseNextHopRefCount(const IpAddress&);
    void decreaseNextHopRefCount(const IpAddress&);

    bool getNeighborEntry(const IpAddress&, NeighborEntry&, MacAddress&);

    bool ifChangeInformNextHop(const string &, bool);
    bool isNextHopFlagSet(const IpAddress &, const uint32_t);

private:
    IntfsOrch *m_intfsOrch;

    NeighborTable m_syncdNeighbors;
    NextHopTable m_syncdNextHops;

    bool addNextHop(IpAddress, string);
    bool removeNextHop(IpAddress, string);

    bool addNeighbor(NeighborEntry, MacAddress);
    bool removeNeighbor(NeighborEntry);

    bool setNextHopFlag(const IpAddress &, const uint32_t);
    bool clearNextHopFlag(const IpAddress &, const uint32_t);

    void doTask(Consumer &consumer);
};

#endif /* SWSS_NEIGHORCH_H */
