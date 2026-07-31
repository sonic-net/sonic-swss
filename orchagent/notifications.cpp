extern "C" {
#include "sai.h"
}

#include "logger.h"
#include "orch.h"
#include "notificationconsumer.h"
#include "notifications.h"
#include "sai_serialize.h"
#include "switchorch.h"

#include <algorithm>
#include <csignal>
#include <inttypes.h>
#include <utility>

extern SwitchOrch *gSwitchOrch;
extern sai_redis_communication_mode_t gRedisCommunicationMode;
volatile sig_atomic_t gOrchShutdownRequested = 0;

static SaiNotificationQueue *gSaiNotificationQueue = nullptr;
static SaiNotificationDispatcher *gSaiNotificationDispatcher = nullptr;

#ifdef ASAN_ENABLED
#include <sanitizer/lsan_interface.h>
#endif

class SaiNotificationQueueSelectable : public swss::Selectable
{
public:
    explicit SaiNotificationQueueSelectable(SaiNotificationQueue *queue)
        : m_queue(queue)
    {
    }

    int getFd() override
    {
        return m_queue->getFd();
    }

    uint64_t readData() override
    {
        return m_queue->readData();
    }

    bool hasData() override
    {
        return m_queue->hasData();
    }

    bool hasCachedData() override
    {
        return m_queue->hasCachedData();
    }

private:
    SaiNotificationQueue *m_queue;
};

class SaiNotificationQueueExecutor : public Executor
{
public:
    SaiNotificationQueueExecutor(SaiNotificationQueue *queue,
                                 Orch *orch,
                                 SaiNotificationDispatcher *dispatcher,
                                 const std::string &name);

    void execute() override;
    void drain() override;

private:
    SaiNotificationQueue *m_queue;
    SaiNotificationDispatcher *m_dispatcher;
};

SaiNotificationQueue::SaiNotificationQueue(int pri, size_t popBatchSize)
    : swss::Selectable(pri)
    , m_selectableEvent(pri)
    , m_popBatchSize(popBatchSize)
{
}

void SaiNotificationQueue::enqueue(const std::string &op, std::string data, std::vector<swss::FieldValueTuple> values)
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_queue.emplace(std::move(data), op, std::move(values));
        if (m_queue.size() > m_highWatermark)
        {
            m_highWatermark = m_queue.size();
        }
    }

    m_selectableEvent.notify();
}

size_t SaiNotificationQueue::size() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_queue.size();
}

size_t SaiNotificationQueue::highWatermark() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_highWatermark;
}

bool SaiNotificationQueue::peekFrontOp(std::string &op) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_queue.empty())
    {
        return false;
    }

    op = kfvOp(m_queue.front());
    return true;
}

void SaiNotificationQueue::pops(std::deque<swss::KeyOpFieldsValuesTuple> &entries)
{
    entries.clear();

    std::lock_guard<std::mutex> lock(m_mutex);
    const auto count = std::min(m_queue.size(), m_popBatchSize);
    for (size_t i = 0; i < count; ++i)
    {
        entries.push_back(std::move(m_queue.front()));
        m_queue.pop();
    }

    if (!m_queue.empty())
    {
        m_selectableEvent.notify();
    }
}

int SaiNotificationQueue::getFd()
{
    return m_selectableEvent.getFd();
}

uint64_t SaiNotificationQueue::readData()
{
    return m_selectableEvent.readData();
}

bool SaiNotificationQueue::hasData()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return !m_queue.empty();
}

bool SaiNotificationQueue::hasCachedData()
{
    return hasData();
}

void SaiNotificationDispatcher::registerHandler(const std::string &op, Handler handler,
                                                  ReadinessPredicate ready)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_handlers[op] = std::move(handler);
    m_readiness[op] = std::move(ready);
}

bool SaiNotificationDispatcher::isReady(const std::string &op) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    auto readyIt = m_readiness.find(op);
    if (readyIt == m_readiness.end() || !readyIt->second)
    {
        return true;
    }

    return readyIt->second();
}

void SaiNotificationDispatcher::dispatch(swss::KeyOpFieldsValuesTuple &entry)
{
    Handler handler;
    auto op = kfvOp(entry);

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto handlerIt = m_handlers.find(op);
        if (handlerIt != m_handlers.end())
        {
            handler = handlerIt->second;
        }
    }

    if (handler)
    {
        handler(entry);
    }
    else
    {
        SWSS_LOG_WARN("No handler registered for SAI notification op %s", op.c_str());
    }
}

SaiNotificationQueueExecutor::SaiNotificationQueueExecutor(SaiNotificationQueue *queue,
                                                           Orch *orch,
                                                           SaiNotificationDispatcher *dispatcher,
                                                           const std::string &name)
    : Executor(new SaiNotificationQueueSelectable(queue), orch, name)
    , m_queue(queue)
    , m_dispatcher(dispatcher)
{
}

void SaiNotificationQueueExecutor::execute()
{
    std::string frontOp;
    if (!m_queue->peekFrontOp(frontOp) || !m_dispatcher->isReady(frontOp))
    {
        return;
    }

    std::deque<swss::KeyOpFieldsValuesTuple> entries;
    m_queue->pops(entries);

    for (auto &entry : entries)
    {
        m_dispatcher->dispatch(entry);
    }
}

void SaiNotificationQueueExecutor::drain()
{
    execute();
}

Executor *createSaiNotificationQueueExecutor(SaiNotificationQueue *queue,
                                           Orch *orch,
                                           SaiNotificationDispatcher *dispatcher,
                                           const std::string &name)
{
    return new SaiNotificationQueueExecutor(queue, orch, dispatcher, name);
}

SaiNotificationQueue *getSaiNotificationQueue()
{
    static std::mutex queueMutex;

    std::lock_guard<std::mutex> lock(queueMutex);
    if (gSaiNotificationQueue == nullptr)
    {
        gSaiNotificationQueue = new SaiNotificationQueue(100, swss::DEFAULT_NC_POP_BATCH_SIZE);
    }

    return gSaiNotificationQueue;
}

SaiNotificationDispatcher *getSaiNotificationDispatcher()
{
    static std::mutex dispatcherMutex;

    std::lock_guard<std::mutex> lock(dispatcherMutex);
    if (gSaiNotificationDispatcher == nullptr)
    {
        gSaiNotificationDispatcher = new SaiNotificationDispatcher();
    }

    return gSaiNotificationDispatcher;
}

void enqueueSaiNotification(const std::string &op, std::string data, std::vector<swss::FieldValueTuple> values)
{
    if (gOrchShutdownRequested != 0)
    {
        return;
    }

    getSaiNotificationQueue()->enqueue(op, std::move(data), std::move(values));
}

void on_fdb_event(uint32_t count, sai_fdb_event_notification_data_t *data)
{
    if (gRedisCommunicationMode == SAI_REDIS_COMMUNICATION_MODE_ZMQ_SYNC)
    {
        std::string sdata = sai_serialize_fdb_event_ntf(count, data);
        std::vector<swss::FieldValueTuple> values;

        enqueueSaiNotification("fdb_event", std::move(sdata), std::move(values));
    }
}

/*
 * Don't perform DB operations within this event handler, because it runs by
 * libsairedis in a separate thread which causes concurrency issues.
 * In ZMQ mode, enqueue the notification so orchagent's main loop can process it.
 */
void on_port_state_change(uint32_t count, sai_port_oper_status_notification_t *data)
{
    if (gRedisCommunicationMode == SAI_REDIS_COMMUNICATION_MODE_ZMQ_SYNC)
    {
        std::string sdata = sai_serialize_port_oper_status_ntf(count, data);
        std::vector<swss::FieldValueTuple> values;

        enqueueSaiNotification("port_state_change", std::move(sdata), std::move(values));
    }
}

void on_bfd_session_state_change(uint32_t count, sai_bfd_session_state_notification_t *data)
{
    if (gRedisCommunicationMode == SAI_REDIS_COMMUNICATION_MODE_ZMQ_SYNC)
    {
        std::string sdata = sai_serialize_bfd_session_state_ntf(count, data);
        std::vector<swss::FieldValueTuple> values;

        enqueueSaiNotification("bfd_session_state_change", std::move(sdata), std::move(values));
    }
}

void on_twamp_session_event(uint32_t count, sai_twamp_session_event_notification_data_t *data)
{
    if (gRedisCommunicationMode == SAI_REDIS_COMMUNICATION_MODE_ZMQ_SYNC)
    {
        std::string sdata = sai_serialize_twamp_session_event_ntf(count, data);
        std::vector<swss::FieldValueTuple> values;

        enqueueSaiNotification("twamp_session_event", std::move(sdata), std::move(values));
    }
}

void on_ha_set_event(uint32_t count, sai_ha_set_event_data_t *data)
{
    if (gRedisCommunicationMode == SAI_REDIS_COMMUNICATION_MODE_ZMQ_SYNC)
    {
        std::string sdata = sai_serialize_ha_set_event_ntf(count, data);
        std::vector<swss::FieldValueTuple> values;

        enqueueSaiNotification(SAI_SWITCH_NOTIFICATION_NAME_HA_SET_EVENT, std::move(sdata), std::move(values));
    }
}

void on_ha_scope_event(uint32_t count, sai_ha_scope_event_data_t *data)
{
    if (gRedisCommunicationMode == SAI_REDIS_COMMUNICATION_MODE_ZMQ_SYNC)
    {
        std::string sdata = sai_serialize_ha_scope_event_ntf(count, data);
        std::vector<swss::FieldValueTuple> values;

        enqueueSaiNotification(SAI_SWITCH_NOTIFICATION_NAME_HA_SCOPE_EVENT, std::move(sdata), std::move(values));
    }
}

void on_flow_bulk_get_session_event(sai_object_id_t flow_bulk_session_id, uint32_t count, sai_flow_bulk_get_session_event_data_t *data)
{
    if (gRedisCommunicationMode == SAI_REDIS_COMMUNICATION_MODE_ZMQ_SYNC)
    {
        std::string sdata = sai_serialize_flow_bulk_get_session_event_ntf(flow_bulk_session_id, count, data);
        std::vector<swss::FieldValueTuple> values;

        enqueueSaiNotification(SAI_SWITCH_NOTIFICATION_NAME_FLOW_BULK_GET_SESSION_EVENT, std::move(sdata), std::move(values));
    }
}

void on_switch_shutdown_request(sai_object_id_t switch_id)
{
    SWSS_LOG_ENTER();

    /* TODO: Later a better restart story will be told here */
    SWSS_LOG_ERROR("Syncd stopped");

    if (gSwitchOrch->isFatalEventReceived())
    {
        SWSS_LOG_ERROR("Orchagent aborted due to fatal SAI error received");
        abort();
    }

    /*
        The quick_exit() is used instead of the exit() to avoid a following data race:
            * the exit() calls the destructors for global static variables (e.g.BufferOrch::m_buffer_type_maps)
            * in parallel to that, orchagent accesses the global static variables
        Since quick_exit doesn't call atexit() flows, the LSAN check is called explicitly via __lsan_do_leak_check()
    */

#ifdef ASAN_ENABLED
    __lsan_do_leak_check();
#endif

    quick_exit(EXIT_FAILURE);
}

void on_port_host_tx_ready(sai_object_id_t switch_id, sai_object_id_t port_id, sai_port_host_tx_ready_status_t hostTxReadyStatus)
{
    if (gRedisCommunicationMode == SAI_REDIS_COMMUNICATION_MODE_ZMQ_SYNC)
    {
        std::string sdata = sai_serialize_port_host_tx_ready_ntf(switch_id, port_id, hostTxReadyStatus);
        std::vector<swss::FieldValueTuple> values;

        enqueueSaiNotification("port_host_tx_ready", std::move(sdata), std::move(values));
    }
}

void on_switch_asic_sdk_health_event(sai_object_id_t switch_id,
                                     sai_switch_asic_sdk_health_severity_t severity,
                                     sai_timespec_t timestamp,
                                     sai_switch_asic_sdk_health_category_t category,
                                     sai_switch_health_data_t data,
                                     const sai_u8_list_t description)
{
    gSwitchOrch->onSwitchAsicSdkHealthEvent(switch_id,
                                            severity,
                                            timestamp,
                                            category,
                                            data,
                                            description);
}

void on_tam_tel_type_config_change(sai_object_id_t tam_tel_id)
{
    if (gRedisCommunicationMode == SAI_REDIS_COMMUNICATION_MODE_ZMQ_SYNC)
    {
        std::string sdata = sai_serialize_object_id(tam_tel_id);
        std::vector<swss::FieldValueTuple> values;

        enqueueSaiNotification(SAI_SWITCH_NOTIFICATION_NAME_TAM_TEL_TYPE_CONFIG_CHANGE, std::move(sdata), std::move(values));
    }
}

void on_switch_macsec_post_status_notify(sai_object_id_t switch_id,
                                         sai_switch_macsec_post_status_t switch_macsec_post_status)
{
    if (gRedisCommunicationMode == SAI_REDIS_COMMUNICATION_MODE_ZMQ_SYNC)
    {
        std::string sdata = sai_serialize_switch_macsec_post_status_ntf(switch_id, switch_macsec_post_status);
        std::vector<swss::FieldValueTuple> values;

        enqueueSaiNotification(SAI_SWITCH_NOTIFICATION_NAME_SWITCH_MACSEC_POST_STATUS, std::move(sdata), std::move(values));
    }
}

void on_macsec_post_status_notify(sai_object_id_t macsec_id,
                                  sai_macsec_post_status_t macsec_post_status)
{
    if (gRedisCommunicationMode == SAI_REDIS_COMMUNICATION_MODE_ZMQ_SYNC)
    {
        std::string sdata = sai_serialize_macsec_post_status_ntf(macsec_id, macsec_post_status);
        std::vector<swss::FieldValueTuple> values;

        enqueueSaiNotification(SAI_SWITCH_NOTIFICATION_NAME_MACSEC_POST_STATUS, std::move(sdata), std::move(values));
    }
}
