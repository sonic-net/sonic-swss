#ifndef SWSS_ORCHDAEMON_H
#define SWSS_ORCHDAEMON_H

#include "dbconnector.h"
#include "producerstatetable.h"
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
#include "mirrororch.h"
#include "fdborch.h"
#include "aclorch.h"

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

    Orch *getOrchByConsumer(ConsumerStateTable *c);
    void flush();
};

#endif /* SWSS_ORCHDAEMON_H */
