#ifndef SWSS_ORCHDAEMON_H
#define SWSS_ORCHDAEMON_H

#include "dbconnector.h"
#include "producerstatetable.h"
#include "consumertable.h"
#include "zmqserver.h"
#include "select.h"

#include "portsorch.h"
#include "fabricportsorch.h"
#include "intfsorch.h"
#include "neighorch.h"
#include "routeorch.h"
#include "flowcounterrouteorch.h"
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
#include "icmporch.h"
#include "srv6orch.h"
#include "nvgreorch.h"
#include "twamporch.h"
#include "stporch.h"
#include "dash/dashenifwdorch.h"
#include "dash/dashaclorch.h"
#include "dash/dashorch.h"
#include "dash/dashrouteorch.h"
#include "dash/dashtunnelorch.h"
#include "dash/dashvnetorch.h"
#include "dash/dashhaorch.h"
#include "dash/dashmeterorch.h"
#include "dash/dashportmaporch.h"
#include "l2mcorch.h"
#include "high_frequency_telemetry/hftelorch.h"
#include <sairedis.h>

using namespace swss;

class OrchDaemon
{
public:
    OrchDaemon(DBConnector *, DBConnector *, DBConnector *, DBConnector *, ZmqServer *);
    virtual ~OrchDaemon();

    virtual bool init();
    void start(long heartBeatInterval);
    bool warmRestoreAndSyncUp();
    void getTaskToSync(vector<string> &ts);
    bool warmRestoreValidation();

    bool warmRestartCheck();

    void addOrchList(Orch* o);
    void setFabricEnabled(bool enabled)
    {
        m_fabricEnabled = enabled;
    }
    void setFabricPortStatEnabled(bool enabled)
    {
        m_fabricPortStatEnabled = enabled;
    }
    void setFabricQueueStatEnabled(bool enabled)
    {
        m_fabricQueueStatEnabled = enabled;
    }
    void logRotate();

    // Two required API to support ring buffer feature
    /**
     * This method is used by a ring buffer consumer [Orchdaemon] to initialzie its ring,
     * and populate this ring's pointer to the producers [Orch, Consumer], to make sure that
     * they are connected to the same ring.
     */
    void enableRingBuffer();
    void disableRingBuffer();
    /**
     * This method describes how the ring consumer consumes this ring.
     */
    void popRingBuffer();

    std::shared_ptr<RingBuffer> gRingBuffer = nullptr;

    std::thread ring_thread;

protected:
    DBConnector *m_applDb;
    DBConnector *m_configDb;
    DBConnector *m_stateDb;
    DBConnector *m_chassisAppDb;
    ZmqServer *m_zmqServer;

    bool m_fabricEnabled = false;
    bool m_fabricPortStatEnabled = true;
    bool m_fabricQueueStatEnabled = true;

    std::vector<Orch *> m_orchList;
    Select *m_select;
    std::chrono::time_point<std::chrono::high_resolution_clock> m_lastHeartBeat;

    void flush();

    void heartBeat(std::chrono::time_point<std::chrono::high_resolution_clock> tcurrent, long interval);

    void freezeAndHeartBeat(unsigned int duration, long interval);
};

class FabricOrchDaemon : public OrchDaemon
{
public:
    FabricOrchDaemon(DBConnector *, DBConnector *, DBConnector *, DBConnector *, ZmqServer *);
    bool init() override;
private:
    DBConnector *m_applDb;
    DBConnector *m_configDb;
};


class DpuOrchDaemon : public OrchDaemon
{
public:
    DpuOrchDaemon(DBConnector *, DBConnector *, DBConnector *, DBConnector *, DBConnector *, DBConnector *, ZmqServer *);
    bool init() override;
private:
    DBConnector *m_dpu_appDb;
    DBConnector *m_dpu_appstateDb;
};
#endif /* SWSS_ORCHDAEMON_H */
