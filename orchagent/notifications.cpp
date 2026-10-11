extern "C" {
#include "sai.h"
}

#include "logger.h"
#include "notificationconsumerstatsorch.h"
#include "notifications.h"
#include "orch.h"
#include "sai_serialize.h"
#include "sainotificationorch.h"
#include "switchorch.h"

#include "json.h"

#include <algorithm>
#include <chrono>
#include <csignal>
#include <inttypes.h>
#include <utility>

extern SwitchOrch *gSwitchOrch;
extern sai_redis_communication_mode_t gRedisCommunicationMode;
volatile sig_atomic_t gOrchShutdownRequested = 0;

#ifdef ASAN_ENABLED
#include <sanitizer/lsan_interface.h>
#endif

namespace
{

constexpr size_t DEFAULT_SAI_NTF_FIFO_MAX_DEPTH = 4096;
constexpr std::chrono::seconds kFifoOverflowLogInterval{5};

std::chrono::steady_clock::time_point g_lastFifoOverflowLog{};
uint64_t g_lastLoggedFifoOverflowTotal = 0;

void maybeLogFifoOverflow(uint64_t totalOverflow, const std::string &consumerName)
{
    auto now = std::chrono::steady_clock::now();
    if (now - g_lastFifoOverflowLog < kFifoOverflowLogInterval &&
        totalOverflow == g_lastLoggedFifoOverflowTotal)
    {
        return;
    }

    SWSS_LOG_WARN("SaiNotificationQueue[%s]: FIFO overflow, dropped=%" PRIu64,
                  consumerName.c_str(), totalOverflow);

    g_lastFifoOverflowLog = now;
    g_lastLoggedFifoOverflowTotal = totalOverflow;
}

} // namespace

class SaiNotificationQueueSelectable : public swss::Selectable
{
public:
    explicit SaiNotificationQueueSelectable(SaiNotificationQueue *queue)
        : swss::Selectable(queue->getPri())
        , m_queue(queue)
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

SaiNotificationQueue::SaiNotificationQueue(const std::string &consumerName,
                                           swss::NotificationQueuePolicy policy,
                                           int pri,
                                           size_t popBatchSize)
    : swss::Selectable(pri)
    , m_consumerName(consumerName)
    , m_policy(policy)
    , m_selectableEvent(pri)
    , m_pri(pri)
    , m_popBatchSize(popBatchSize)
    , m_fifoMaxDepth(DEFAULT_SAI_NTF_FIFO_MAX_DEPTH)
{
    if (policy == swss::NotificationQueuePolicy::LruDedup)
    {
        m_queue = std::make_unique<swss::LruDedupNotificationQueue>(consumerName);
    }
    else
    {
        m_queue = std::make_unique<swss::FifoNotificationQueue>();
    }
}

std::string SaiNotificationQueue::buildWireMessage(
        const std::string &op,
        const std::string &data,
        const std::vector<swss::FieldValueTuple> &values) const
{
    std::vector<swss::FieldValueTuple> wireValues;
    wireValues.emplace_back(op, data);
    wireValues.insert(wireValues.end(), values.begin(), values.end());
    return swss::JSon::buildJson(wireValues);
}

void SaiNotificationQueue::wireToEntry(
        const std::string &wire,
        swss::KeyOpFieldsValuesTuple &entry) const
{
    std::vector<swss::FieldValueTuple> values;
    swss::JSon::readJson(wire, values);

    if (values.empty())
    {
        throw std::runtime_error("empty SAI notification wire message");
    }

    swss::FieldValueTuple opdata = values.front();
    values.erase(values.begin());
    entry = std::make_tuple(fvValue(opdata), fvField(opdata), values);
}

void SaiNotificationQueue::enqueue(const std::string &op,
                                     std::string data,
                                     std::vector<swss::FieldValueTuple> values)
{
    const auto wire = buildWireMessage(op, data, values);

    {
        std::lock_guard<std::mutex> lock(m_mutex);

        m_received.fetch_add(1, std::memory_order_relaxed);

        if (m_policy == swss::NotificationQueuePolicy::Fifo &&
            m_queue->size() >= m_fifoMaxDepth)
        {
            m_droppedOverflow.fetch_add(1, std::memory_order_relaxed);
            maybeLogFifoOverflow(m_droppedOverflow.load(std::memory_order_relaxed),
                                 m_consumerName);
            return;
        }

        m_queue->push(wire);

        if (m_queue->size() > m_highWatermark)
        {
            m_highWatermark = m_queue->size();
        }
    }

    m_selectableEvent.notify();
}

size_t SaiNotificationQueue::size() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_queue->size();
}

size_t SaiNotificationQueue::highWatermark() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_highWatermark;
}

bool SaiNotificationQueue::peekFrontOp(std::string &op) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_queue->empty())
    {
        return false;
    }

    op = swss::peekOp(m_queue->front());
    return !op.empty();
}

void SaiNotificationQueue::pops(std::deque<swss::KeyOpFieldsValuesTuple> &entries)
{
    entries.clear();

    std::lock_guard<std::mutex> lock(m_mutex);
    const auto count = std::min(m_queue->size(), m_popBatchSize);
    for (size_t i = 0; i < count; ++i)
    {
        swss::KeyOpFieldsValuesTuple entry;
        wireToEntry(m_queue->front(), entry);
        entries.push_back(std::move(entry));
        m_queue->pop();
    }
}

void SaiNotificationQueue::registerReadiness(ReadinessPredicate ready)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_ready = std::move(ready);
    m_handlerRegistered = true;
}

bool SaiNotificationQueue::isReady() const
{
    ReadinessPredicate ready;
    bool handlerRegistered = false;

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        handlerRegistered = m_handlerRegistered;
        ready = m_ready;
    }

    return handlerRegistered && (!ready || ready());
}

bool SaiNotificationQueue::isHandlerRegistered() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_handlerRegistered;
}

int SaiNotificationQueue::getPri() const
{
    return m_pri;
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
    return !m_queue->empty();
}

bool SaiNotificationQueue::hasCachedData()
{
    if (!isReady())
    {
        return false;
    }

    std::lock_guard<std::mutex> lock(m_mutex);
    return m_queue->size() > 1;
}

const std::string &SaiNotificationQueue::getConsumerName() const
{
    return m_consumerName;
}

const std::string &SaiNotificationQueue::getChannel() const
{
    static const std::string channel = "NOTIFICATIONS";
    return channel;
}

swss::NotificationQueuePolicy SaiNotificationQueue::getPolicy() const
{
    return m_policy;
}

SaiNotificationQueue::Stats SaiNotificationQueue::getStats() const
{
    Stats stats;
    stats.received = m_received.load(std::memory_order_relaxed);
    stats.dropped_allowlist = 0;
    stats.dropped_overflow = m_droppedOverflow.load(std::memory_order_relaxed);
    return stats;
}

swss::LruDedupNotificationQueue *SaiNotificationQueue::getLruDedupQueue() const
{
    return dynamic_cast<swss::LruDedupNotificationQueue *>(m_queue.get());
}

void SaiNotificationQueue::notifyPending()
{
    if (hasData())
    {
        m_selectableEvent.notify();
    }
}

void SaiNotificationDispatcher::registerHandler(const std::string &op, Handler handler)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_handlers.find(op) != m_handlers.end())
    {
        SWSS_LOG_WARN("Replacing SAI notification handler for op %s", op.c_str());
    }
    m_handlers[op] = std::move(handler);
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
        SWSS_LOG_WARN("No SAI notification handler registered for op %s", op.c_str());
    }
}

SaiNotificationQueueExecutor::SaiNotificationQueueExecutor(
        SaiNotificationQueue *queue,
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
    if (!m_queue->isReady() || !m_queue->hasData())
    {
        return;
    }

    std::deque<swss::KeyOpFieldsValuesTuple> entries;
    m_queue->pops(entries);

    for (auto &entry : entries)
    {
        m_dispatcher->dispatch(entry);
    }

    if (m_queue->hasData() && m_queue->isReady())
    {
        m_queue->notifyPending();
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

SaiNotificationDispatcher *getSaiNotificationDispatcher()
{
    static SaiNotificationDispatcher dispatcher;
    return &dispatcher;
}

void enqueueSaiNotification(const std::string &op,
                            std::string data,
                            std::vector<swss::FieldValueTuple> values)
{
    if (gOrchShutdownRequested != 0)
    {
        return;
    }

    if (gSaiNotificationOrch == nullptr)
    {
        SWSS_LOG_WARN("Dropping SAI notification op %s: SaiNotificationOrch not initialized",
                      op.c_str());
        return;
    }

    gSaiNotificationOrch->getSaiNotificationQueue(op)->enqueue(
        op, std::move(data), std::move(values));
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

        enqueueSaiNotification(SAI_SWITCH_NOTIFICATION_NAME_HA_SET_EVENT,
                               std::move(sdata),
                               std::move(values));
    }
}

void on_ha_scope_event(uint32_t count, sai_ha_scope_event_data_t *data)
{
    if (gRedisCommunicationMode == SAI_REDIS_COMMUNICATION_MODE_ZMQ_SYNC)
    {
        std::string sdata = sai_serialize_ha_scope_event_ntf(count, data);
        std::vector<swss::FieldValueTuple> values;

        enqueueSaiNotification(SAI_SWITCH_NOTIFICATION_NAME_HA_SCOPE_EVENT,
                               std::move(sdata),
                               std::move(values));
    }
}

void on_flow_bulk_get_session_event(sai_object_id_t flow_bulk_session_id,
                                    uint32_t count,
                                    sai_flow_bulk_get_session_event_data_t *data)
{
    if (gRedisCommunicationMode == SAI_REDIS_COMMUNICATION_MODE_ZMQ_SYNC)
    {
        std::string sdata = sai_serialize_flow_bulk_get_session_event_ntf(flow_bulk_session_id,
                                                                          count,
                                                                          data);
        std::vector<swss::FieldValueTuple> values;

        enqueueSaiNotification(SAI_SWITCH_NOTIFICATION_NAME_FLOW_BULK_GET_SESSION_EVENT,
                               std::move(sdata),
                               std::move(values));
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

#ifdef ASAN_ENABLED
    __lsan_do_leak_check();
#endif

    quick_exit(EXIT_FAILURE);
}

void on_port_host_tx_ready(sai_object_id_t switch_id,
                           sai_object_id_t port_id,
                           sai_port_host_tx_ready_status_t hostTxReadyStatus)
{
    if (gRedisCommunicationMode == SAI_REDIS_COMMUNICATION_MODE_ZMQ_SYNC)
    {
        std::string sdata = sai_serialize_port_host_tx_ready_ntf(switch_id,
                                                                port_id,
                                                                hostTxReadyStatus);
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

        enqueueSaiNotification(SAI_SWITCH_NOTIFICATION_NAME_TAM_TEL_TYPE_CONFIG_CHANGE,
                               std::move(sdata),
                               std::move(values));
    }
}

void on_switch_macsec_post_status_notify(sai_object_id_t switch_id,
                                         sai_switch_macsec_post_status_t switch_macsec_post_status)
{
    if (gRedisCommunicationMode == SAI_REDIS_COMMUNICATION_MODE_ZMQ_SYNC)
    {
        std::string sdata = sai_serialize_switch_macsec_post_status_ntf(switch_id,
                                                                        switch_macsec_post_status);
        std::vector<swss::FieldValueTuple> values;

        enqueueSaiNotification(SAI_SWITCH_NOTIFICATION_NAME_SWITCH_MACSEC_POST_STATUS,
                               std::move(sdata),
                               std::move(values));
    }
}

void on_macsec_post_status_notify(sai_object_id_t macsec_id,
                                  sai_macsec_post_status_t macsec_post_status)
{
    if (gRedisCommunicationMode == SAI_REDIS_COMMUNICATION_MODE_ZMQ_SYNC)
    {
        std::string sdata = sai_serialize_macsec_post_status_ntf(macsec_id, macsec_post_status);
        std::vector<swss::FieldValueTuple> values;

        enqueueSaiNotification(SAI_SWITCH_NOTIFICATION_NAME_MACSEC_POST_STATUS,
                               std::move(sdata),
                               std::move(values));
    }
}
