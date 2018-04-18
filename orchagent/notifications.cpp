extern "C" {
#include "sai.h"
}

#include "logger.h"
#include "notifications.h"

void on_fdb_event(uint32_t count, sai_fdb_event_notification_data_t *data)
{
}

void on_port_state_change(uint32_t count, sai_port_oper_status_notification_t *data)
{
}

void on_switch_shutdown_request()
{
    SWSS_LOG_ENTER();

    /* TODO: Later a better restart story will be told here */
    SWSS_LOG_ERROR("Syncd stopped");

    exit(EXIT_FAILURE);
}
