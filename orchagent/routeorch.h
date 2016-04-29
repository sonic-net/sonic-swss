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

/* RouteTable: destination network, next hop IP address(es) */
typedef map<IpPrefix, IpAddresses> RouteTable;

class RouteOrch : public Orch
{
public:
    RouteOrch(DBConnector *db, string tableName,
              PortsOrch *portsOrch, NeighOrch *neighOrch) :
        Orch(db, tableName),
        m_portsOrch(portsOrch),
        m_neighOrch(neighOrch),
        m_resync(false) {};

private:
    PortsOrch *m_portsOrch;
    NeighOrch *m_neighOrch;

    bool m_resync;

    RouteTable m_syncdRoutes;

    void doTask(Consumer& consumer);

    void addTempRoute(IpPrefix, IpAddresses);
    bool addRoute(IpPrefix, IpAddresses);
    bool removeRoute(IpPrefix);
};

#endif /* SWSS_ROUTEORCH_H */
