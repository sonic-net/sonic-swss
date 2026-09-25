#pragma once

#include <atomic>
#include <cstddef>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

extern "C" {
#include "sai.h"
#include "saiextensions.h"
}

#include "notificationconsumer.h"
#include "selectableevent.h"
#include "table.h"

class Orch;
class Executor;

class SaiNotificationQueue : public swss::Selectable
{
public:
    using ReadinessPredicate = std::function<bool()>;

    struct Stats
    {
        uint64_t received = 0;
        uint64_t dropped_allowlist = 0;
        uint64_t dropped_overflow = 0;
    };

    SaiNotificationQueue(const std::string &consumerName,
                         swss::NotificationQueuePolicy policy,
                         int pri = 100,
                         size_t popBatchSize = swss::DEFAULT_NC_POP_BATCH_SIZE);

    void enqueue(const std::string &op, std::string data, std::vector<swss::FieldValueTuple> values);
    bool peekFrontOp(std::string &op) const;
    void pops(std::deque<swss::KeyOpFieldsValuesTuple> &entries);
    size_t size() const;
    size_t highWatermark() const;

    void registerReadiness(ReadinessPredicate ready = nullptr);
    bool isReady() const;
    bool isHandlerRegistered() const;

    int getPri() const;
    int getFd() override;
    uint64_t readData() override;
    bool hasData() override;
    bool hasCachedData() override;

    const std::string &getConsumerName() const;
    const std::string &getChannel() const;
    swss::NotificationQueuePolicy getPolicy() const;
    Stats getStats() const;
    swss::LruDedupNotificationQueue *getLruDedupQueue() const;

    void notifyPending();

private:
    std::string buildWireMessage(const std::string &op,
                                 const std::string &data,
                                 const std::vector<swss::FieldValueTuple> &values) const;

    void wireToEntry(const std::string &wire, swss::KeyOpFieldsValuesTuple &entry) const;

    mutable std::mutex m_mutex;
    std::string m_consumerName;
    swss::NotificationQueuePolicy m_policy;
    std::unique_ptr<swss::NotificationQueueBase> m_queue;
    swss::SelectableEvent m_selectableEvent;
    int m_pri;
    size_t m_popBatchSize;
    size_t m_highWatermark = 0;
    size_t m_fifoMaxDepth;
    ReadinessPredicate m_ready;
    bool m_handlerRegistered = false;

    std::atomic<uint64_t> m_received{0};
    std::atomic<uint64_t> m_droppedOverflow{0};
};

class SaiNotificationDispatcher
{
public:
    using Handler = std::function<void(swss::KeyOpFieldsValuesTuple &)>;

    void registerHandler(const std::string &op, Handler handler);
    void dispatch(swss::KeyOpFieldsValuesTuple &entry);

private:
    mutable std::mutex m_mutex;
    std::unordered_map<std::string, Handler> m_handlers;
};

SaiNotificationDispatcher *getSaiNotificationDispatcher();
void enqueueSaiNotification(const std::string &op, std::string data, std::vector<swss::FieldValueTuple> values);
Executor *createSaiNotificationQueueExecutor(SaiNotificationQueue *queue,
                                           Orch *orch,
                                           SaiNotificationDispatcher *dispatcher,
                                           const std::string &name);

void on_fdb_event(uint32_t count, sai_fdb_event_notification_data_t *data);
void on_port_state_change(uint32_t count, sai_port_oper_status_notification_t *data);
void on_bfd_session_state_change(uint32_t count, sai_bfd_session_state_notification_t *data);
void on_twamp_session_event(uint32_t count, sai_twamp_session_event_notification_data_t *data);
void on_ha_set_event(uint32_t count, sai_ha_set_event_data_t *data);
void on_ha_scope_event(uint32_t count, sai_ha_scope_event_data_t *data);
void on_flow_bulk_get_session_event(sai_object_id_t flow_bulk_session_id, uint32_t count, sai_flow_bulk_get_session_event_data_t *data);

// The function prototype information can be found here:
//      https://github.com/sonic-net/sonic-sairedis/blob/master/meta/NotificationSwitchShutdownRequest.cpp#L49
void on_switch_shutdown_request(sai_object_id_t switch_id);

void on_port_host_tx_ready(sai_object_id_t switch_id, sai_object_id_t port_id, sai_port_host_tx_ready_status_t m_portHostTxReadyStatus);

void on_switch_asic_sdk_health_event(sai_object_id_t switch_id,
                                     sai_switch_asic_sdk_health_severity_t severity,
                                     sai_timespec_t timestamp,
                                     sai_switch_asic_sdk_health_category_t category,
                                     sai_switch_health_data_t data,
                                     const sai_u8_list_t description);

void on_tam_tel_type_config_change(sai_object_id_t tam_tel_id);

void on_switch_macsec_post_status_notify(sai_object_id_t switch_id,
                                         sai_switch_macsec_post_status_t switch_macsec_post_status);
void on_macsec_post_status_notify(sai_object_id_t macsec_id,
                                  sai_macsec_post_status_t macsec_post_status);
