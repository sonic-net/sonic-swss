extern "C" {
#include "sai.h"
}

#include "logger.h"
#include "notifications.h"
#include "switchorch.h"

#include <csignal>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "notificationproducer.h"

extern SwitchOrch *gSwitchOrch;
extern sai_redis_communication_mode_t gRedisCommunicationMode;

/*
 * Set by the orchagent signal handler.  Once it is set,
 * dispatchSaiNotification() returns without forwarding.  Graceful shutdown
 * exits without running destructors, so this stops new work that the main
 * loop will no longer drain.
 */
volatile sig_atomic_t gOrchShutdownRequested = 0;

#ifdef ASAN_ENABLED
#include <sanitizer/lsan_interface.h>
#endif

namespace
{

/*
 * op -> in-process NotificationConsumer.  Written from orch constructors on
 * the orchagent main thread and read from the libsairedis callback thread, so
 * every access takes gConsumerRegistryMutex.
 */
std::mutex gConsumerRegistryMutex;
std::unordered_map<std::string, swss::NotificationConsumer *> gConsumerRegistry;

} // namespace

/*
 * Hand a SAI notification to the orch that owns it.
 *
 * ZMQ mode: the owning orch registered an in-process NotificationConsumer for
 * this op, so push straight into it; its executor runs the usual
 * doTask(NotificationConsumer&) on the orchagent main thread.
 *
 * No in-process consumer registered for the op (or its orch does not exist):
 * publish on the Redis NOTIFICATIONS channel.  In ZMQ mode the owning orch's
 * consumer is in-process, so this fallback does not deliver the notification
 * to an orch.  It covers an op that arrives before that orch has registered.
 */
void dispatchSaiNotification(const std::string &op, const std::string &sdata)
{
    if (gOrchShutdownRequested != 0)
    {
        return;
    }

    std::vector<swss::FieldValueTuple> values;

    {
        std::lock_guard<std::mutex> lock(gConsumerRegistryMutex);
        auto it = gConsumerRegistry.find(op);
        if (it != gConsumerRegistry.end())
        {
            it->second->enqueue(op, sdata, values);
            return;
        }
    }

    static thread_local swss::DBConnector db("ASIC_DB", 0);
    static thread_local swss::NotificationProducer notifier(&db, "NOTIFICATIONS");
    notifier.send(op, sdata, values);
}

/*
 * Create the NotificationConsumer an orch uses to receive SAI switch
 * notifications on the "NOTIFICATIONS" channel.  The kind of consumer depends
 * on the SAI redis communication mode:
 *
 *   - ZMQ mode: an in-process consumer.  Notifications are pushed into it with
 *     NotificationConsumer::enqueue() from the SAI callbacks.  db is not used.
 *   - Any other mode: a consumer subscribed to the channel through db.
 *
 * policy selects the queue policy (Fifo or LruDedup).
 */
swss::NotificationConsumer *createSaiNotificationConsumer(swss::DBConnector *db,
                                                          swss::NotificationQueuePolicy policy)
{
    if (gRedisCommunicationMode == SAI_REDIS_COMMUNICATION_MODE_ZMQ_SYNC)
    {
        return new swss::NotificationConsumer("NOTIFICATIONS", policy);
    }

    if (policy == swss::NotificationQueuePolicy::Fifo)
    {
        return new swss::NotificationConsumer(db, "NOTIFICATIONS");
    }

    return new swss::NotificationConsumer(db, "NOTIFICATIONS",
                                          100,    // pri, same as the 2-arg ctor default
                                          swss::DEFAULT_NC_POP_BATCH_SIZE,
                                          policy);
}

void registerSaiNotificationConsumer(const std::string &op, swss::NotificationConsumer *consumer)
{
    if (consumer == nullptr || !consumer->isInProcessDelivery())
    {
        return;
    }

    std::lock_guard<std::mutex> lock(gConsumerRegistryMutex);
    gConsumerRegistry[op] = consumer;
    SWSS_LOG_NOTICE("Registered in-process SAI notification consumer for op %s", op.c_str());
}

void wakeSaiNotificationConsumers()
{
    std::lock_guard<std::mutex> lock(gConsumerRegistryMutex);
    for (auto &entry : gConsumerRegistry)
    {
        entry.second->notifyPending();
    }
}

void on_fdb_event(uint32_t count, sai_fdb_event_notification_data_t *data)
{
    if (gRedisCommunicationMode == SAI_REDIS_COMMUNICATION_MODE_ZMQ_SYNC)
    {
        dispatchSaiNotification("fdb_event", sai_serialize_fdb_event_ntf(count, data));
    }
}

/*
 * Don't perform DB operations within this event handler, because it runs by
 * libsairedis in a separate thread which causes concurrency issues.
 * For platforms which use zmq between orchagent and syncd, it is an acceptable
 * workaround to forward the notifications from the callback handler to the
 * owning orch's in-process NotificationConsumer, which is drained by the
 * orchagent main loop.
 */
void on_port_state_change(uint32_t count, sai_port_oper_status_notification_t *data)
{
    if (gRedisCommunicationMode == SAI_REDIS_COMMUNICATION_MODE_ZMQ_SYNC)
    {
        dispatchSaiNotification("port_state_change", sai_serialize_port_oper_status_ntf(count, data));
    }
}

void on_queue_pfc_deadlock(uint32_t count, sai_queue_deadlock_notification_data_t *data)
{
    if (gRedisCommunicationMode == SAI_REDIS_COMMUNICATION_MODE_ZMQ_SYNC)
    {
        dispatchSaiNotification(SAI_SWITCH_NOTIFICATION_NAME_QUEUE_PFC_DEADLOCK,
                                sai_serialize_queue_deadlock_ntf(count, data));
    }
}

void on_bfd_session_state_change(uint32_t count, sai_bfd_session_state_notification_t *data)
{
    if (gRedisCommunicationMode == SAI_REDIS_COMMUNICATION_MODE_ZMQ_SYNC)
    {
        dispatchSaiNotification("bfd_session_state_change", sai_serialize_bfd_session_state_ntf(count, data));
    }
}

void on_twamp_session_event(uint32_t count, sai_twamp_session_event_notification_data_t *data)
{
    if (gRedisCommunicationMode == SAI_REDIS_COMMUNICATION_MODE_ZMQ_SYNC)
    {
        dispatchSaiNotification("twamp_session_event", sai_serialize_twamp_session_event_ntf(count, data));
    }
}

void on_ha_set_event(uint32_t count, sai_ha_set_event_data_t *data)
{
    if (gRedisCommunicationMode == SAI_REDIS_COMMUNICATION_MODE_ZMQ_SYNC)
    {
        // Forward ha_set_event notification to be handled in dashhaorch doTask()
        dispatchSaiNotification(SAI_SWITCH_NOTIFICATION_NAME_HA_SET_EVENT,
                                sai_serialize_ha_set_event_ntf(count, data));
    }
}

void on_ha_scope_event(uint32_t count, sai_ha_scope_event_data_t *data)
{
    if (gRedisCommunicationMode == SAI_REDIS_COMMUNICATION_MODE_ZMQ_SYNC)
    {
        // Forward ha_scope_event notification to be handled in dashhaorch doTask()
        dispatchSaiNotification(SAI_SWITCH_NOTIFICATION_NAME_HA_SCOPE_EVENT,
                                sai_serialize_ha_scope_event_ntf(count, data));
    }
}

void on_flow_bulk_get_session_event(sai_object_id_t flow_bulk_session_id, uint32_t count, sai_flow_bulk_get_session_event_data_t *data)
{
    if (gRedisCommunicationMode == SAI_REDIS_COMMUNICATION_MODE_ZMQ_SYNC)
    {
        // Forward flow_bulk_get_session_event notification to be handled in orchagent doTask()
        dispatchSaiNotification(SAI_SWITCH_NOTIFICATION_NAME_FLOW_BULK_GET_SESSION_EVENT,
                                sai_serialize_flow_bulk_get_session_event_ntf(flow_bulk_session_id, count, data));
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

void on_port_host_tx_ready(sai_object_id_t switch_id, sai_object_id_t port_id, sai_port_host_tx_ready_status_t m_portHostTxReadyStatus)
{
    if (gRedisCommunicationMode == SAI_REDIS_COMMUNICATION_MODE_ZMQ_SYNC)
    {
        dispatchSaiNotification("port_host_tx_ready",
                                sai_serialize_port_host_tx_ready_ntf(switch_id, port_id, m_portHostTxReadyStatus));
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
        dispatchSaiNotification(SAI_SWITCH_NOTIFICATION_NAME_TAM_TEL_TYPE_CONFIG_CHANGE,
                                sai_serialize_object_id(tam_tel_id));
    }
}

void on_switch_macsec_post_status_notify(sai_object_id_t switch_id,
                                         sai_switch_macsec_post_status_t switch_macsec_post_status)
{
    if (gRedisCommunicationMode == SAI_REDIS_COMMUNICATION_MODE_ZMQ_SYNC)
    {
        // Forward switch_macsec_post_status notification to be handled in macsecorch doTask()
        dispatchSaiNotification("switch_macsec_post_status",
                                sai_serialize_switch_macsec_post_status_ntf(switch_id, switch_macsec_post_status));
    }
}

void on_macsec_post_status_notify(sai_object_id_t macsec_id,
                                  sai_macsec_post_status_t macsec_post_status)
{
    if (gRedisCommunicationMode == SAI_REDIS_COMMUNICATION_MODE_ZMQ_SYNC)
    {
        // Forward macsec_post_status notification to be handled in macsecorch doTask()
        dispatchSaiNotification("macsec_post_status",
                                sai_serialize_macsec_post_status_ntf(macsec_id, macsec_post_status));
    }
}
