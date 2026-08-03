#pragma once

#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <queue>
#include <string>
#include <unordered_map>
#include <vector>

extern "C" {
#include "sai.h"
#include "saiextensions.h"
}

#include "selectableevent.h"
#include "table.h"

class Orch;
class Executor;

class SaiNotificationQueue : public swss::Selectable
{
public:
    SaiNotificationQueue(int pri = 100, size_t popBatchSize = 128);

    void enqueue(const std::string &op, std::string data, std::vector<swss::FieldValueTuple> values);
    bool peekFrontOp(std::string &op) const;
    void pops(std::deque<swss::KeyOpFieldsValuesTuple> &entries);
    size_t size() const;
    size_t highWatermark() const;

    int getFd() override;
    uint64_t readData() override;
    bool hasData() override;
    bool hasCachedData() override;

private:
    mutable std::mutex m_mutex;
    std::queue<swss::KeyOpFieldsValuesTuple> m_queue;
    swss::SelectableEvent m_selectableEvent;
    size_t m_popBatchSize;
    size_t m_highWatermark = 0;
};

class SaiNotificationDispatcher
{
public:
    using Handler = std::function<void(swss::KeyOpFieldsValuesTuple &)>;
    using ReadinessPredicate = std::function<bool()>;

    void registerHandler(const std::string &op, Handler handler,
                         ReadinessPredicate ready = nullptr);
    bool isReady(const std::string &op) const;
    void dispatch(swss::KeyOpFieldsValuesTuple &entry);

private:
    mutable std::mutex m_mutex;
    std::unordered_map<std::string, Handler> m_handlers;
    std::unordered_map<std::string, ReadinessPredicate> m_readiness;
};

SaiNotificationQueue *getSaiNotificationQueue();
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
