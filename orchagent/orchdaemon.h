#ifndef SWSS_ORCHDAEMON_H
#define SWSS_ORCHDAEMON_H

#include "dbconnector.h"
#include "producertable.h"
#include "consumertable.h"
#include "select.h"

#include "portsorch.h"
#include "intfsorch.h"
#include "neighorch.h"
#include "routeorch.h"
#include "qosorch.h"


using namespace swss;

class OrchDaemon
{
public:
    OrchDaemon();
    ~OrchDaemon();

    bool init();
    void start();
private:
    DBConnector *m_applDb;
    DBConnector *m_asicDb;

    PortsOrch *m_portsO;
    IntfsOrch *m_intfsO;
    NeighOrch *m_neighO;
    RouteOrch *m_routeO;
    QosOrch   *m_qosO;

    Select *m_select;

    Orch *getOrchByConsumer(ConsumerTable *c);
};

#endif /* SWSS_ORCHDAEMON_H */
