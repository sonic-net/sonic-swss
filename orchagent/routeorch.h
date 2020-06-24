#ifndef SWSS_ROUTEORCH_H
#define SWSS_ROUTEORCH_H

#include "orch.h"
#include "observer.h"
#include "intfsorch.h"
#include "neighorch.h"
#include "vnetorch.h"
#include "vxlanorch.h"

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

    bool createRemoteVtep(sai_object_id_t, const NextHopKey&);
    bool deleteRemoteVtep(sai_object_id_t, const NextHopKey&);
    bool removeOverlayNextHops(sai_object_id_t, const NextHopGroupKey&);

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

    std::string getLinkLocalEui64Addr(void);
    void        addLinkLocalRouteToMe(sai_object_id_t vrf_id, IpPrefix linklocal_prefix);

    void doTask(Consumer& consumer);
};

#endif /* SWSS_ROUTEORCH_H */
