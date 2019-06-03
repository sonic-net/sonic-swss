#ifndef SWSS_ROUTEORCH_H
#define SWSS_ROUTEORCH_H

#include "orch.h"
#include "observer.h"
#include "intfsorch.h"
#include "neighorch.h"

#include "ipaddress.h"
#include "ipaddresses.h"
#include "ipprefix.h"
#include "tokenize.h"

#include <map>

/* Maximum next hop group number */
#define NHGRP_MAX_SIZE 128

class NextHopGroupKey
{
public:
    NextHopGroupKey() = default;

    /* ip_string|if_alias separated by ',' */
    NextHopGroupKey(const std::string &nexthops)
    {
        auto nhv = tokenize(nexthops, ',');
        for (const auto &nh : nhv)
        {
            m_nexthops.insert(nh);
        }
    }

    inline const std::set<NextHopKey> &getNextHops() const
    {
        return m_nexthops;
    }

    inline size_t getSize() const
    {
        return m_nexthops.size();
    }

    inline bool operator<(const NextHopGroupKey &o) const
    {
        return m_nexthops < o.m_nexthops;
    }

    inline bool operator==(const NextHopGroupKey &o) const
    {
        return m_nexthops == o.m_nexthops;
    }

    inline bool operator!=(const NextHopGroupKey &o) const
    {
        return !(*this == o);
    }

    void add(const std::string &ip, const std::string &alias)
    {
        m_nexthops.emplace(ip, alias);
    }

    void add(const std::string &nh)
    {
        m_nexthops.insert(nh);
    }

    void add(const NextHopKey &nh)
    {
        m_nexthops.insert(nh);
    }

    bool contains(const std::string &ip, const std::string &alias) const
    {
        NextHopKey nh(ip, alias);
        return m_nexthops.find(nh) != m_nexthops.end();
    }

    bool contains(const std::string &nh) const
    {
        return m_nexthops.find(nh) != m_nexthops.end();
    }

    bool contains(const NextHopKey &nh) const
    {
        return m_nexthops.find(nh) != m_nexthops.end();
    }

    bool contains(const NextHopGroupKey &nhs) const
    {
        for (const auto &nh : nhs.getNextHops())
        {
            if (!contains(nh))
            {
                return false;
            }
        }
        return true;
    }

    void remove(const std::string &ip, const std::string &alias)
    {
        NextHopKey nh(ip, alias);
        m_nexthops.erase(nh);
    }

    void remove(const std::string &nh)
    {
        m_nexthops.erase(nh);
    }

    void remove(const NextHopKey &nh)
    {
        m_nexthops.erase(nh);
    }

    const std::string to_string() const
    {
        string nhs_str;

        for (auto it = m_nexthops.begin(); it != m_nexthops.end(); ++it)
        {
            if (it != m_nexthops.begin())
            {
                nhs_str += ",";
            }

            nhs_str += it->to_string();
        }

        return nhs_str;
    }

private:
    std::set<NextHopKey> m_nexthops;
};

typedef std::map<NextHopKey, sai_object_id_t> NextHopGroupMembers;

struct NextHopGroupEntry
{
    sai_object_id_t         next_hop_group_id;      // next hop group id
    int                     ref_count;              // reference count
    NextHopGroupMembers     nhopgroup_members;      // ids of members indexed by <ip_address, if_alias>
};

struct NextHopUpdate
{
    sai_object_id_t vrf_id;
    IpAddress destination;
    IpPrefix prefix;
    NextHopGroupKey nexthopGroup;
};

struct NextHopObserverEntry;

/* NextHopGroupTable: NextHopGroupKey, NextHopGroupEntry */
typedef std::map<NextHopGroupKey, NextHopGroupEntry> NextHopGroupTable;
/* RouteTable: destination network, NextHopGroupKey */
typedef std::map<IpPrefix, NextHopGroupKey> RouteTable;
/* RouteTables: vrf_id, RouteTable */
typedef std::map<sai_object_id_t, RouteTable> RouteTables;
/* Host: vrf_id, IpAddress */
typedef std::pair<sai_object_id_t, IpAddress> Host;
/* NextHopObserverTable: Host, next hop observer entry */
typedef std::map<Host, NextHopObserverEntry> NextHopObserverTable;

struct NextHopObserverEntry
{
    RouteTable routeTable;
    list<Observer *> observers;
};

class RouteOrch : public Orch, public Subject
{
public:
    RouteOrch(DBConnector *db, string tableName, NeighOrch *neighOrch, IntfsOrch *intfsOrch, VRFOrch *vrfOrch);

    bool hasNextHopGroup(const NextHopGroupKey&) const;
    sai_object_id_t getNextHopGroupId(const NextHopGroupKey&);

    void attach(Observer *, const IpAddress&, sai_object_id_t vrf_id = gVirtualRouterId);
    void detach(Observer *, const IpAddress&, sai_object_id_t vrf_id = gVirtualRouterId);

    void increaseNextHopRefCount(const NextHopGroupKey&);
    void decreaseNextHopRefCount(const NextHopGroupKey&);
    bool isRefCounterZero(const NextHopGroupKey&) const;

    bool addNextHopGroup(const NextHopGroupKey&);
    bool removeNextHopGroup(const NextHopGroupKey&);

    bool validnexthopinNextHopGroup(const NextHopKey&);
    bool invalidnexthopinNextHopGroup(const NextHopKey&);

    void notifyNextHopChangeObservers(sai_object_id_t, const IpPrefix&, const NextHopGroupKey&, bool);
private:
    NeighOrch *m_neighOrch;
    IntfsOrch *m_intfsOrch;
    VRFOrch *m_vrfOrch;

    int m_nextHopGroupCount;
    int m_maxNextHopGroupCount;
    bool m_resync;

    RouteTables m_syncdRoutes;
    NextHopGroupTable m_syncdNextHopGroups;

    NextHopObserverTable m_nextHopObservers;

    void addTempRoute(sai_object_id_t, const IpPrefix&, const NextHopGroupKey&);
    bool addRoute(sai_object_id_t, const IpPrefix&, const NextHopGroupKey&);
    bool removeRoute(sai_object_id_t, const IpPrefix&);

    void doTask(Consumer& consumer);
};

#endif /* SWSS_ROUTEORCH_H */
