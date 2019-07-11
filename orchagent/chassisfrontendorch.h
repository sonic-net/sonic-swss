
#ifndef SWSS_CHASSISFRONTENDORCH_H
#define SWSS_CHASSISFRONTENDORCH_H

#include <map>
#include <set>
#include <string>
#include <vector>

#include "dbconnector.h"
#include "producerstatetable.h"
#include "orch.h"
#include "observer.h"
#include "vnetorch.h"

#define CFG_PASS_THROUGH_ROUTE_TABLE_NAME "PASS_THROUGH_ROUTE_TABLE"
#define APP_PASS_THROUGH_ROUTE_TABLE_NAME "PASS_THROUGH_ROUTE_TABLE"

class ChassisFrontendOrch : public Orch, public Observer
{
public:
    ChassisFrontendOrch(
        DBConnector* cfgDb,
        DBConnector *applDb,
        const std::vector<std::string> &tableNames,
        VNetRouteOrch * vNetRouteOrch);

    void handleMirrorSessionMessage(const std::string & op, const IpAddress& ip);

private:

    void update(SubjectType, void*);
    void addRouteToChassisInterVrfForwardingIpTable(const IpPrefix& ipPfx);
    void deleteRouteFromChassisInterVrfForwardingIpTable(const IpPrefix& ipPfx);

    virtual void doTask(Consumer &consumer);

    std::set<IpPrefix>  m_vNetRouteTable;
    std::set<IpAddress> m_mirrorSessionIpInChassisTable;
    std::set<IpPrefix>  m_hasBroadcastedRoute;
    
    Table m_passThroughRouteTable;

    VNetRouteOrch* m_vNetRouteOrch;
};

#endif
