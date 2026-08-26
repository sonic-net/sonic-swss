#include "sainotificationorch.h"

#include "logger.h"
#include "notificationconsumerstatsorch.h"
#include "notifications.h"

extern "C" {
#include "sai.h"
}

SaiNotificationOrch *gSaiNotificationOrch = nullptr;

SaiNotificationOrch::SaiNotificationOrch()
    : Orch()
{
    SWSS_LOG_ENTER();

    initMetadata();
}

void SaiNotificationOrch::initMetadata()
{
    using swss::NotificationQueuePolicy;

    m_metadataByOp.emplace("fdb_event",
                           ConsumerMetadata{"FdbOrch:fdb_event", NotificationQueuePolicy::LruDedup});
    m_metadataByOp.emplace("port_state_change",
                           ConsumerMetadata{"PortsOrch:port_state_change",
                                              NotificationQueuePolicy::LruDedup});
    m_metadataByOp.emplace("port_host_tx_ready",
                           ConsumerMetadata{"PortsOrch:port_host_tx_ready",
                                              NotificationQueuePolicy::LruDedup});
    m_metadataByOp.emplace("bfd_session_state_change",
                           ConsumerMetadata{"BfdOrch:bfd_session_state_change",
                                              NotificationQueuePolicy::Fifo});
    m_metadataByOp.emplace("icmp_echo_session_state_change",
                           ConsumerMetadata{"IcmpOrch:icmp_echo_session_state_change",
                                              NotificationQueuePolicy::Fifo});
    m_metadataByOp.emplace("twamp_session_event",
                           ConsumerMetadata{"TwampOrch:twamp_session_event",
                                              NotificationQueuePolicy::Fifo});
    m_metadataByOp.emplace(SAI_SWITCH_NOTIFICATION_NAME_SWITCH_MACSEC_POST_STATUS,
                           ConsumerMetadata{"MacsecOrch:macsec_post_status",
                                              NotificationQueuePolicy::Fifo});
    m_metadataByOp.emplace(SAI_SWITCH_NOTIFICATION_NAME_MACSEC_POST_STATUS,
                           ConsumerMetadata{"MacsecOrch:macsec_post_status",
                                              NotificationQueuePolicy::Fifo});
    m_metadataByOp.emplace(SAI_SWITCH_NOTIFICATION_NAME_HA_SET_EVENT,
                           ConsumerMetadata{"DashHaOrch:ha_set_event",
                                              NotificationQueuePolicy::Fifo});
    m_metadataByOp.emplace(SAI_SWITCH_NOTIFICATION_NAME_HA_SCOPE_EVENT,
                           ConsumerMetadata{"DashHaOrch:ha_scope_event",
                                              NotificationQueuePolicy::Fifo});
    m_metadataByOp.emplace(SAI_SWITCH_NOTIFICATION_NAME_FLOW_BULK_GET_SESSION_EVENT,
                           ConsumerMetadata{"DashHaFlowOrch:flow_bulk_get_session_event",
                                              NotificationQueuePolicy::Fifo});
    m_metadataByOp.emplace(SAI_SWITCH_NOTIFICATION_NAME_TAM_TEL_TYPE_CONFIG_CHANGE,
                           ConsumerMetadata{"HFTelOrch:tam_tel_type_config_change",
                                              NotificationQueuePolicy::Fifo});
}

SaiNotificationQueue *SaiNotificationOrch::getOrCreateQueueForOp(const std::string &op)
{
    auto metaIt = m_metadataByOp.find(op);
    if (metaIt == m_metadataByOp.end())
    {
        SWSS_LOG_THROW("Unknown SAI notification op %s", op.c_str());
    }

    const auto &meta = metaIt->second;
    auto &entry = m_consumersByName[meta.consumerName];

    if (!entry.queue)
    {
        entry.queue = std::make_unique<SaiNotificationQueue>(
            meta.consumerName,
            meta.policy);

        entry.executor.reset(createSaiNotificationQueueExecutor(
            entry.queue.get(),
            this,
            &m_dispatcher,
            meta.consumerName));

        Orch::addExecutor(entry.executor.get());

        if (gNotifConsumerStatsOrch)
        {
            gNotifConsumerStatsOrch->registerSaiNotificationQueue(meta.consumerName,
                                                                entry.queue.get());
        }
    }

    return entry.queue.get();
}

SaiNotificationQueue *SaiNotificationOrch::getSaiNotificationQueue(const std::string &op)
{
    return getOrCreateQueueForOp(op);
}

void SaiNotificationOrch::registerHandler(const std::string &op,
                                          SaiNotificationDispatcher::Handler handler,
                                          SaiNotificationQueue::ReadinessPredicate ready)
{
    auto *queue = getOrCreateQueueForOp(op);

    m_dispatcher.registerHandler(op, std::move(handler));
    queue->registerReadiness(std::move(ready));
    queue->notifyPending();
}
