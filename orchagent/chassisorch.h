#ifndef SWSS_CHASSISORCH_H
#define SWSS_CHASSISORCH_H

#include <map>
#include <set>
#include <string>
#include <vector>

#include "dbconnector.h"
#include "producerstatetable.h"
#include "orchagent/orch.h"
#include "orchagent/observer.h"
#include "orchagent/vnetorch.h"

class ChassisOrch : public Orch, public Observer
{
public:
    ChassisOrch(
        DBConnector* cfgDb,
        DBConnector *applDb,
        const std::vector<std::string> &tableNames,
        VNetRouteOrch * vNetRouteOrch);

private:

    void update(SubjectType, void*);
    void addRouteToPassThroughRouteTable(const VNetNextHopUpdate& update);
    void deleteRoutePassThroughRouteTable(const VNetNextHopUpdate& update);

    virtual void doTask(Consumer &consumer);

    Table m_passThroughRouteTable;
    VNetRouteOrch* m_vNetRouteOrch;
};

#endif