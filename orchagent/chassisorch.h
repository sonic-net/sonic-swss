
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

class ChassisOrch : public Orch, public Observer
{
public:
    ChassisOrch(
        DBConnector* cfgDb,
        DBConnector *applDb,
        const std::vector<std::string> &tableNames,
        VNetRouteOrch * vNetRouteOrch);

    void handleMirrorSessionMessage(const std::string & op, const IpAddress& ip);

private:

    void update(SubjectType, void*);
    void addRouteToPassThroughRouteTable(const VNetNextHopUpdate& update);
    void deleteRoutePassThroughRouteTable(const VNetNextHopUpdate& update);

    virtual void doTask(Consumer &consumer);

    Table m_passThroughRouteTable;
    VNetRouteOrch* m_vNetRouteOrch;
};

#endif
