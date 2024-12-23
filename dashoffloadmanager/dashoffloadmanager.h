#pragma once

#include "dbconnector.h"
#include "zmqserver.h"
#include "select.h"
#include "subscriberstatetable.h"
#include "orch.h"
#include <thread>

#include "dpuinfoprovider.h"

struct OffloadState
{
    bool pa_validation = false;
};

class DashOffloadManager
{
public:
    DashOffloadManager(const DpuInfo& dpuInfo, const std::string& zmq_server_addr, const std::string& zmq_proxy_endpoint);
    void start();
    void join();
    OffloadState getOffloadState() const;

private:
    DpuInfo m_dpuInfo;
    std::string m_dpuLogKey;
    std::string m_dpuStateKey;
    bool m_dpu_state_down = true;
    OffloadState m_offloadState;

    swss::Select m_select;
    std::shared_ptr<std::thread> m_thread;
    std::atomic<bool> m_stop_thread { false };
    std::unique_ptr<swss::ZmqServer> m_zmqProxy;
    std::unique_ptr<swss::DBConnector> m_applDb;
    std::unique_ptr<swss::DBConnector> m_dpuStateDb;
    std::vector<std::unique_ptr<Orch>> m_offloadOrchList;

    void offload_handle();
    void enableDefaultOffload();
    void disableOffload();
    void processDpuStateTableUpdate(swss::SubscriberStateTable *table);
    void processOffloadTablePaValidation(swss::KeyOpFieldsValuesTuple &entry);
    void processDpuStateUpdate(bool dpu_down);

    void enablePaValidationOffload();
};
