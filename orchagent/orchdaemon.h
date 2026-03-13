#ifndef SWSS_ORCHDAEMON_H
#define SWSS_ORCHDAEMON_H

#include "dbconnector.h"
#include "producerstatetable.h"
#include "consumertable.h"
#include "zmqserver.h"
#include "select.h"

#include "orchagent/portsorch.h"
#include "orchagent/fabricportsorch.h"
#include "orchagent/intfsorch.h"
#include "orchagent/neighorch.h"
#include "orchagent/routeorch.h"
#include "orchagent/flex_counter/flowcounterrouteorch.h"
#include "orchagent/nhgorch.h"
#include "orchagent/cbf/cbfnhgorch.h"
#include "orchagent/cbf/nhgmaporch.h"
#include "orchagent/copporch.h"
#include "orchagent/tunneldecaporch.h"
#include "orchagent/qosorch.h"
#include "orchagent/bufferorch.h"
#include "orchagent/mirrororch.h"
#include "orchagent/fdborch.h"
#include "orchagent/aclorch.h"
#include "orchagent/pbhorch.h"
#include "orchagent/pfcwdorch.h"
#include "orchagent/switchorch.h"
#include "orchagent/crmorch.h"
#include "orchagent/vrforch.h"
#include "orchagent/vxlanorch.h"
#include "orchagent/vnetorch.h"
#include "orchagent/countercheckorch.h"
#include "orchagent/flexcounterorch.h"
#include "orchagent/watermarkorch.h"
#include "orchagent/policerorch.h"
#include "orchagent/sfloworch.h"
#include "orchagent/debugcounterorch.h"
#include "orchagent/directory.h"
#include "natorch.h"
#include "orchagent/isolationgrouporch.h"
#include "orchagent/mlagorch.h"
#include "orchagent/muxorch.h"
#include "orchagent/macsecorch.h"
#include "orchagent/p4orch/p4orch.h"
#include "orchagent/bfdorch.h"
#include "icmporch.h"
#include "orchagent/srv6orch.h"
#include "orchagent/nvgreorch.h"
#include "orchagent/twamporch.h"
#include "orchagent/stporch.h"
#include "orchagent/dash/dashenifwdorch.h"
#include "dash/dashaclorch.h"
#include "dash/dashorch.h"
#include "dash/dashrouteorch.h"
#include "dash/dashtunnelorch.h"
#include "dash/dashvnetorch.h"
#include "orchagent/dash/dashhafloworch.h"
#include "orchagent/dash/dashhaorch.h"
#include "dash/dashmeterorch.h"
#include "dash/dashportmaporch.h"
#include "orchagent/high_frequency_telemetry/hftelorch.h"
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

    // Use a dedicated zmq server for p4Orch.
    const std::string m_p4OrchZmqServerEp = "ipc:///zmq_swss/p4orch_zmq_swss_ep";
    ZmqServer *m_p4OrchZmqServer = nullptr;

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
