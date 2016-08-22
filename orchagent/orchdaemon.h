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
#include "copporch.h"
#include "tunneldecaporch.h"
#include "qosorch.h"
#include "bufferorch.h"

using namespace swss;

class OrchDaemon
{
public:
    OrchDaemon(DBConnector *);
    ~OrchDaemon();

    bool init();
    void start();
private:
    DBConnector *m_applDb;

    std::vector<Orch *> m_orchList;
    Select *m_select;

    Orch *getOrchByConsumer(ConsumerTable *c);
};

#endif /* SWSS_ORCHDAEMON_H */
