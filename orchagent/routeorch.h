#ifndef SWSS_ROUTEORCH_H
#define SWSS_ROUTEORCH_H

#include "orch.h"
#include "intfsorch.h"

#include "ipaddress.h"
#include "ipaddresses.h"
#include "ipprefix.h"

#include <map>

using namespace std;
using namespace swss;

#define NHGRP_MAX_SIZE 16

struct NextHopEntry
{
    sai_object_id_t     next_hop_id;    // next hop id or next hop group id
    int                 ref_count;      // reference count
};

/* RouteTable: destination network, next hop IP address(es) */
typedef map<IpPrefix, IpAddresses> RouteTable;
/* NextHopTable: next hop IP address(es), NextHopEntry */
typedef map<IpAddresses, NextHopEntry> NextHopTable;

class RouteOrch : public Orch
{
public:
    RouteOrch(DBConnector *db, string tableName, PortsOrch *portsOrch) :
        Orch(db, tableName),
        m_portsOrch(portsOrch),
        m_nextHopGroupCount(0),
        m_resync(false) {};

    bool createNextHopEntry(IpAddress, sai_object_id_t);
    bool createNextHopEntry(IpAddresses, sai_object_id_t);
    bool removeNextHopEntry(IpAddress);
    bool removeNextHopEntry(IpAddresses);
    NextHopEntry getNextHopEntry(IpAddress);
    NextHopEntry getNextHopEntry(IpAddresses);
    int getNextHopRefCount(IpAddress);
    int getNextHopRefCount(IpAddresses);

private:
    virtual void doTask(Consumer& consumer_info);

private:
    PortsOrch *m_portsOrch;

    int m_nextHopGroupCount;
    bool m_resync;

    RouteTable m_syncdRoutes;
    NextHopTable m_syncdNextHops;

    bool addRoute(IpPrefix, IpAddresses);
    bool removeRoute(IpPrefix);
};

#endif /* SWSS_ROUTEORCH_H */
