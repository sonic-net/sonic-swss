#ifndef SWSS_ROUTEORCH_H
#define SWSS_ROUTEORCH_H

#include "orch.h"
#include "observer.h"
#include "intfsorch.h"
#include "neighorch.h"

#include "ipaddress.h"
#include "ipaddresses.h"
#include "ipprefix.h"
#include "nexthopgroupkey.h"

#include <map>

/* Maximum next hop group number */
#define NHGRP_MAX_SIZE 128
/* Length of the Interface Id value in EUI64 format */
#define EUI64_INTF_ID_LEN 8

typedef std::map<NextHopKey, sai_object_id_t> NextHopGroupMembers;

struct NextHopGroupEntry
{
    sai_object_id_t         next_hop_group_id;      // next hop group id
    int                     ref_count;              // reference count
    NextHopGroupMembers     nhopgroup_members;      // ids of members indexed by <ip_address, if_alias>
};

struct NextHopUpdate
{
    IpAddress destination;
    IpPrefix prefix;
    NextHopGroupKey nexthopGroup;
};

struct NextHopObserverEntry;

/* NextHopGroupTable: NextHopGroupKey, NextHopGroupEntry */
typedef std::map<NextHopGroupKey, NextHopGroupEntry> NextHopGroupTable;
/* RouteTable: destination network, NextHopGroupKey */
typedef std::map<IpPrefix, NextHopGroupKey> RouteTable;
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

    bool hasNextHopGroup(const NextHopGroupKey&) const;
    sai_object_id_t getNextHopGroupId(const NextHopGroupKey&);

    void attach(Observer *, const IpAddress&);
    void detach(Observer *, const IpAddress&);

    void increaseNextHopRefCount(const NextHopGroupKey&);
    void decreaseNextHopRefCount(const NextHopGroupKey&);
    bool isRefCounterZero(const NextHopGroupKey&) const;

    bool addNextHopGroup(const NextHopGroupKey&);
    bool removeNextHopGroup(const NextHopGroupKey&);

    bool validnexthopinNextHopGroup(const NextHopKey&);
    bool invalidnexthopinNextHopGroup(const NextHopKey&);

    void notifyNextHopChangeObservers(const IpPrefix&, const NextHopGroupKey&, bool);
private:
    NeighOrch *m_neighOrch;

    int m_nextHopGroupCount;
    int m_maxNextHopGroupCount;
    bool m_resync;

    RouteTable m_syncdRoutes;
    NextHopGroupTable m_syncdNextHopGroups;

    NextHopObserverTable m_nextHopObservers;

    void addTempRoute(const IpPrefix&, const NextHopGroupKey&);
    bool addRoute(const IpPrefix&, const NextHopGroupKey&);
    bool removeRoute(const IpPrefix&);

    std::string getLinkLocalEui64Addr(void);
    void        addLinkLocalRouteToMe(sai_object_id_t vrf_id, IpPrefix linklocal_prefix);

    void doTask(Consumer& consumer);
};

#endif /* SWSS_ROUTEORCH_H */
