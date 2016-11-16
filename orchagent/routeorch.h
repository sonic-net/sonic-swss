#ifndef SWSS_ROUTEORCH_H
#define SWSS_ROUTEORCH_H

#include "orch.h"
#include "intfsorch.h"
#include "neighorch.h"

#include "ipaddress.h"
#include "ipaddresses.h"
#include "ipprefix.h"

#include <map>

using namespace std;
using namespace swss;

/* Maximum next hop group number */
#define NHGRP_MAX_SIZE 128

struct NextHopGroupEntry
{
    sai_object_id_t     next_hop_group_id;  // next hop group id
    int                 ref_count;          // reference count
};

/* NextHopGroupTable: next hop group IP addersses, NextHopGroupEntry */
typedef map<IpAddresses, NextHopGroupEntry> NextHopGroupTable;
/* RouteTable: destination network, next hop IP address(es) */
typedef map<IpPrefix, IpAddresses> RouteTable;
/* NextHopTriggerTable: next hop Ip address, RouteTable */
typedef map<IpAddress, RouteTable> NextHopTriggerTable;

class RouteOrch : public Orch
{
public:
    RouteOrch(DBConnector *db, string tableName, NeighOrch *neighOrch);

    bool hasNextHopGroup(IpAddresses);

private:
    NeighOrch *m_neighOrch;

    int m_nextHopGroupCount;
    bool m_resync;

    RouteTable m_syncdRoutes;
    NextHopGroupTable m_syncdNextHopGroups;

    /*
     * This table is used to store the route entries that cannot be synced due to
     * unresolved next hops. Once the next hop is resolved, the routes will be processed.
     */
    NextHopTriggerTable m_nextHopTriggerRoutes;

    void increaseNextHopRefCount(IpAddresses);
    void decreaseNextHopRefCount(IpAddresses);

    bool addNextHopGroup(IpPrefix, IpAddresses);
    bool removeNextHopGroup(IpAddresses);

    bool addTempRoute(IpPrefix, IpAddresses);
    bool addRoute(IpPrefix, IpAddresses);
    bool removeRoute(IpPrefix);

    void doTask(Consumer& consumer);
};

#endif /* SWSS_ROUTEORCH_H */
