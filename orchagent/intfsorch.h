#ifndef SWSS_INTFSORCH_H
#define SWSS_INTFSORCH_H

#include "orch.h"
#include "portsorch.h"

#include "ipaddresses.h"
#include "macaddress.h"

#include <map>

extern sai_object_id_t gVirtualRouterId;
extern MacAddress gMacAddress;

typedef map<string, IpAddresses> IntfsTable;

class IntfsOrch : public Orch
{
public:
    IntfsOrch(DBConnector *db, string tableName, PortsOrch *portsOrch);
private:
    PortsOrch *m_portsOrch;
    IntfsTable m_intfs;
    void doTask(Consumer &consumer);

    bool addRouterIntfs(Port &port, sai_object_id_t virtual_router_id = gVirtualRouterId,
            MacAddress mac_address = gMacAddress);
    bool removeRouterIntfs(Port &port);
};

#endif /* SWSS_INTFSORCH_H */
