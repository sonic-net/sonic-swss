extern "C" {
#include "sai.h"
}

#include "logger.h"
#include "notifications.h"
#include "switchorch.h"

extern SwitchOrch *gSwitchOrch;
extern sai_redis_communication_mode_t gRedisCommunicationMode;

#ifdef ASAN_ENABLED
#include <sanitizer/lsan_interface.h>
#endif

void on_fdb_event(uint32_t count, sai_fdb_event_notification_data_t *data)
{
    // don't use this event handler, because it runs by libsairedis in a separate thread
    // which causes concurrency access to the DB
}

/*
 * Don't perform DB operations within this event handler, because it runs by
 * libsairedis in a separate thread which causes concurrency issues.
 * For platforms which use zmq between orchagent and syncd, it is an acceptable
 * workaround to forward the notifications from the callback handler to the
 * redis notifications channel processed by portsorch.
 */
void on_port_state_change(uint32_t count, sai_port_oper_status_notification_t *data)
{
    if (gRedisCommunicationMode == SAI_REDIS_COMMUNICATION_MODE_ZMQ_SYNC)
    {
        swss::DBConnector db("ASIC_DB", 0);
        swss::NotificationProducer port_state_change(&db, "NOTIFICATIONS");
        std::string sdata = sai_serialize_port_oper_status_ntf(count, data);
        std::vector<swss::FieldValueTuple> values;

        // Forward port_state_change notification to be handled in portsorch doTask()
        port_state_change.send("port_state_change", sdata, values);
    }
}

void on_bfd_session_state_change(uint32_t count, sai_bfd_session_state_notification_t *data)
{
    // don't use this event handler, because it runs by libsairedis in a separate thread
    // which causes concurrency access to the DB
}

void on_twamp_session_event(uint32_t count, sai_twamp_session_event_notification_data_t *data)
{
    // don't use this event handler, because it runs by libsairedis in a separate thread
    // which causes concurrency access to the DB
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
    // don't use this event handler, because it runs by libsairedis in a separate thread
    // which causes concurrency access to the DB
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
