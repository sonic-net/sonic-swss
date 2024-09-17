#include "dashoffloadmanager.h"
#include "logger_utils.h"

#include "dashoffloadpavalidation.h"

#include <sstream>

using namespace std;
using namespace swss;

#define SELECT_TIMEOUT 60000

DashOffloadManager::DashOffloadManager(const DpuInfo& dpuInfo, const std::string& zmq_server_addr, const std::string& zmq_proxy_endpoint):
    m_dpuInfo(dpuInfo),
    m_dpuLogKey(makeDpuLogKey(dpuInfo.dpuId)),
    m_dpuStateKey("DPU" + to_string(dpuInfo.dpuId))
{
    SWSS_LOG_ENTER();

    DASH_MNG_LOG_NOTICE("Starting Dash Offload Manager (zmq proxy: %s -> %s)", zmq_server_addr.c_str(), zmq_proxy_endpoint.c_str());

    m_zmqProxy = make_unique<ZmqServer>(zmq_server_addr);

    m_zmqProxy->enableProxyMode(zmq_proxy_endpoint);

    m_applDb = make_unique<swss::DBConnector>("APPL_DB", 0);

    m_dpuStateDb = make_unique<swss::DBConnector>("CHASSIS_STATE_DB", 0);
};

void DashOffloadManager::start()
{
    SWSS_LOG_ENTER();

    m_thread = make_shared<thread>(&DashOffloadManager::offload_handle, this);
}

void DashOffloadManager::join()
{
    m_thread->join();
}

OffloadState DashOffloadManager::getOffloadState() const
{
    return m_offloadState;
}

void DashOffloadManager::enableDefaultOffload()
{
    SWSS_LOG_ENTER();

    enablePaValidationOffload();
}

void DashOffloadManager::disableOffload()
{
    SWSS_LOG_ENTER();

    for (auto &orch : m_offloadOrchList)
    {
        auto orch_selecatables = orch->getSelectables();
        for (auto &sel : orch_selecatables)
        {
            m_select.removeSelectable(sel);
        }
    }

    m_offloadOrchList.clear();
    m_offloadState.pa_validation = false;
}

void DashOffloadManager::processDpuStateUpdate(bool dpu_down)
{
    SWSS_LOG_ENTER();

    if (dpu_down == m_dpu_state_down)
    {
        return;
    }

    if (dpu_down)
    {
        DASH_MNG_LOG_NOTICE("DPU is down - stopping the offload");
        disableOffload();
    } else {
        enableDefaultOffload();
    }

    m_dpu_state_down = dpu_down;
}

void DashOffloadManager::processDpuStateTableUpdate(swss::SubscriberStateTable *table)
{
    SWSS_LOG_ENTER();

    std::deque<KeyOpFieldsValuesTuple> entries;
    table->pops(entries);

    for (auto &entry : entries)
    {
        auto key = kfvKey(entry);

        if (key == m_dpuStateKey)
        {
            for (auto i : kfvFieldsValues(entry))
            {
                if (fvField(i) == "dpu_control_plane_state")
                {
                    bool dpu_down = fvValue(i) == "down";
                    return processDpuStateUpdate(dpu_down);
                }
            }
        }
    }
}

void DashOffloadManager::enablePaValidationOffload()
{
    SWSS_LOG_ENTER();

    if (m_offloadState.pa_validation)
    {
        return;
    }

    Orch *orch = new DashPaVlidationOffloadOrch(m_dpuInfo, m_applDb.get(), m_zmqProxy.get());
    m_select.addSelectables(orch->getSelectables());

    m_offloadOrchList.push_back(unique_ptr<Orch>(orch));

    m_offloadState.pa_validation = true;
}

void DashOffloadManager::offload_handle()
{
    SWSS_LOG_ENTER();

    auto dpuStateTable = make_unique<SubscriberStateTable>(m_dpuStateDb.get(), CHASSIS_STATE_DPU_STATE_TABLE_NAME, TableConsumable::DEFAULT_POP_BATCH_SIZE, 0);
    m_select.addSelectable(dpuStateTable.get());

    while (!m_stop_thread)
    {
        Selectable *sel;
        int ret;

        ret = m_select.select(&sel, SELECT_TIMEOUT);
        if (ret == Select::ERROR)
        {
            DASH_MNG_LOG_ERROR("Error from select - %s",  strerror(errno));
            continue;
        }

        if (ret == Select::TIMEOUT)
        {
            for (auto &o : m_offloadOrchList)
            {
                o->doTask();
            }

            continue;
        }

        if (sel == dpuStateTable.get())
        {
            processDpuStateTableUpdate(dpuStateTable.get());
        } else
        {
            auto *c = (Executor *)sel;
            c->execute();
        }

        for (auto &o : m_offloadOrchList)
        {
            o->doTask();
        }
    }
}
