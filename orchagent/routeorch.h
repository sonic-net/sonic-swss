#ifndef SWSS_ROUTEORCH_H
#define SWSS_ROUTEORCH_H

#include "orch.h"
#include "observer.h"
#include "intfsorch.h"
#include "neighorch.h"

#include "ipaddress.h"
#include "ipaddresses.h"
#include "ipprefix.h"

#include <map>

/* Maximum next hop group number */
#define NHGRP_MAX_SIZE 128

typedef std::map<IpAddress, sai_object_id_t> NextHopGroupMembers;

struct NextHopGroupEntry
{
    sai_object_id_t         next_hop_group_id;      // next hop group id
    int                     ref_count;              // reference count
    NextHopGroupMembers     nhopgroup_members;      // ids of members indexed by ip address
};

struct NextHopUpdate
{
    IpPrefix prefix;
    IpAddresses nexthopGroup;
};

struct NextHopObserverEntry;

/* NextHopGroupTable: next hop group IP addersses, NextHopGroupEntry */
typedef std::map<IpAddresses, NextHopGroupEntry> NextHopGroupTable;
/* RouteTable: destination network, next hop IP address(es) */
typedef std::map<IpPrefix, IpAddresses> RouteTable;
/* NextHopObserverTable: Destination IP address, next hop observer entry */
typedef std::map<IpAddress, NextHopObserverEntry> NextHopObserverTable;

struct NextHopObserverEntry
{
    RouteTable routeTable;
    list<Observer *> observers;
};

class RouteOrch : public Orch, public Subject
{
public:
    RouteOrch(DBConnector *db, string tableName, NeighOrch *neighOrch);

    bool hasNextHopGroup(const IpAddresses&) const;
    sai_object_id_t getNextHopGroupId(const IpAddresses&);

    void attach(Observer *, const IpAddress&);
    void detach(Observer *, const IpAddress&);

    void increaseNextHopRefCount(IpAddresses);
    void decreaseNextHopRefCount(IpAddresses);
    bool isRefCounterZero(const IpAddresses&) const;

    bool addNextHopGroup(IpAddresses);
    bool removeNextHopGroup(IpAddresses);

    bool validnexthopinNextHopGroup(const IpAddress &);
    bool invalidnexthopinNextHopGroup(const IpAddress &);

private:
    NeighOrch *m_neighOrch;

    int m_nextHopGroupCount;
    int m_maxNextHopGroupCount;
    bool m_resync;

    RouteTable m_syncdRoutes;
    NextHopGroupTable m_syncdNextHopGroups;

    NextHopObserverTable m_nextHopObservers;

    void addTempRoute(IpPrefix, IpAddresses);
    bool addRoute(IpPrefix, IpAddresses);
    bool removeRoute(IpPrefix);

    void doTask(Consumer& consumer);

    void notifyNextHopChangeObservers(IpPrefix, IpAddresses, bool);
};

#endif /* SWSS_ROUTEORCH_H */
