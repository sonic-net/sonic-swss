#ifndef SWSS_ORCHDAEMON_H
#define SWSS_ORCHDAEMON_H

#include "dbconnector.h"
#include "producerstatetable.h"
#include "consumertable.h"
#include "select.h"

#include "portsorch.h"
#include "fabricportsorch.h"
#include "intfsorch.h"
#include "neighorch.h"
#include "routeorch.h"
#include "nhgorch.h"
#include "cbf/cbfnhgorch.h"
#include "cbf/nhgmaporch.h"
#include "copporch.h"
#include "tunneldecaporch.h"
#include "qosorch.h"
#include "bufferorch.h"
#include "mirrororch.h"
#include "fdborch.h"
#include "aclorch.h"
#include "pbhorch.h"
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
#include "sfloworch.h"
#include "debugcounterorch.h"
#include "directory.h"
#include "natorch.h"
#include "isolationgrouporch.h"
#include "mlagorch.h"
#include "muxorch.h"
#include "macsecorch.h"
#include "p4orch/p4orch.h"
#include "bfdorch.h"
#include "srv6orch.h"

using namespace swss;

class OrchDaemon
{
public:
    OrchDaemon(DBConnector *, DBConnector *, DBConnector *, DBConnector *);
    ~OrchDaemon();

    virtual bool init();
    void start();
    bool warmRestoreAndSyncUp();
    void getTaskToSync(vector<string> &ts);
    bool warmRestoreValidation();

    bool warmRestartCheck();

    void addOrchList(Orch* o);
    void setFabricEnabled(bool enabled)
    {
        m_fabricEnabled = enabled;
    }
private:
    DBConnector *m_applDb;
    DBConnector *m_configDb;
    DBConnector *m_stateDb;
    DBConnector *m_chassisAppDb;

    bool m_fabricEnabled = false;

    std::vector<Orch *> m_orchList;
    Select *m_select;

    void flush();
};

class FabricOrchDaemon : public OrchDaemon
{
public:
    FabricOrchDaemon(DBConnector *, DBConnector *, DBConnector *, DBConnector *);
    bool init() override;
private:
    DBConnector *m_applDb;
    DBConnector *m_configDb;
};

#endif /* SWSS_ORCHDAEMON_H */
