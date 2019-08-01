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
#include "pfcwdorch.h"
#include "switchorch.h"
#include "crmorch.h"
#include "vrforch.h"
#include "vxlanorch.h"
#include "vnetorch.h"
#include "countercheckorch.h"
#include "flexcounterorch.h"
#include "watermarkorch.h"
#include "policerorch.h"
#include "directory.h"

using namespace swss;

class OrchDaemon
{
public:
    OrchDaemon(DBConnector *, DBConnector *, DBConnector *);
    ~OrchDaemon();

    bool init();
    void start();
    bool warmRestoreAndSyncUp();
    void getTaskToSync(vector<string> &ts);
    bool warmRestoreValidation();

    bool warmRestartCheck();
private:
    DBConnector *m_applDb;
    DBConnector *m_configDb;
    DBConnector *m_stateDb;

    std::vector<Orch *> m_orchList;
    Select *m_select;

    void flush();
};

#endif /* SWSS_ORCHDAEMON_H */
