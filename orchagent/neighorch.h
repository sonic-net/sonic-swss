#ifndef SWSS_NEIGHORCH_H
#define SWSS_NEIGHORCH_H

#include "orch.h"
#include "observer.h"
#include "portsorch.h"
#include "intfsorch.h"

#include "ipaddress.h"
#include "tokenize.h"

#define NHFLAGS_IFDOWN                  0x1 // nexthop's outbound i/f is down

#define VRF_PREFIX "Vrf"
extern IntfsOrch *gIntfsOrch;

struct NextHopKey
{
    IpAddress           ip_address;     // neighbor IP address
    string              alias;          // incoming interface alias

    NextHopKey() = default;
    NextHopKey(const std::string &ipstr, const std::string &alias) : ip_address(ipstr), alias(alias) {}
    NextHopKey(const IpAddress &ip, const std::string &alias) : ip_address(ip), alias(alias) {}
    NextHopKey(const std::string &str)
    {
        if (str.find(',') != string::npos)
        {
            std::string err = "Error converting " + str + " to NextHop";
            throw std::invalid_argument(err);
        }
        auto keys = tokenize(str, '|');
        if (keys.size() == 1)
        {
            ip_address = keys[0];
            alias = gIntfsOrch->getRouterIntfsAlias(ip_address);
        }
        else if (keys.size() == 2)
        {
            ip_address = keys[0];
            alias = keys[1];
            if (!alias.compare(0, strlen(VRF_PREFIX), VRF_PREFIX))
            {
                alias = gIntfsOrch->getRouterIntfsAlias(ip_address, alias);
            }
        }
        else
        {
            std::string err = "Error converting " + str + " to NextHop";
            throw std::invalid_argument(err);
        }
    }
    const std::string to_string() const
    {
        return ip_address.to_string() + "|" + alias;
    }

    bool operator<(const NextHopKey &o) const
    {
        return tie(ip_address, alias) < tie(o.ip_address, o.alias);
    }

    bool operator==(const NextHopKey &o) const
    {
        return (ip_address == o.ip_address) && (alias == o.alias);
    }

    bool operator!=(const NextHopKey &o) const
    {
        return !(*this == o);
    }
};

typedef NextHopKey NeighborEntry;

struct NextHopEntry
{
    sai_object_id_t     next_hop_id;    // next hop id
    int                 ref_count;      // reference count
    uint32_t            nh_flags;       // flags
};

/* NeighborTable: NeighborEntry, neighbor MAC address */
typedef map<NeighborEntry, MacAddress> NeighborTable;
/* NextHopTable: NextHopKey, NextHopEntry */
typedef map<NextHopKey, NextHopEntry> NextHopTable;

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

    bool hasNextHop(const NextHopKey&);

    sai_object_id_t getNextHopId(const NextHopKey&);
    int getNextHopRefCount(const NextHopKey&);

    void increaseNextHopRefCount(const NextHopKey&);
    void decreaseNextHopRefCount(const NextHopKey&);

    bool getNeighborEntry(const NextHopKey&, NeighborEntry&, MacAddress&);
    bool getNeighborEntry(const IpAddress&, NeighborEntry&, MacAddress&);

    bool ifChangeInformNextHop(const string &, bool);
    bool isNextHopFlagSet(const NextHopKey &, const uint32_t);

private:
    IntfsOrch *m_intfsOrch;

    NeighborTable m_syncdNeighbors;
    NextHopTable m_syncdNextHops;

    bool addNextHop(const IpAddress&, const string&);
    bool removeNextHop(const IpAddress&, const string&);

    bool addNeighbor(const NeighborEntry&, const MacAddress&);
    bool removeNeighbor(const NeighborEntry&);

    bool setNextHopFlag(const NextHopKey &, const uint32_t);
    bool clearNextHopFlag(const NextHopKey &, const uint32_t);

    void doTask(Consumer &consumer);
};

#endif /* SWSS_NEIGHORCH_H */
