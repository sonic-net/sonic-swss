#include <mutex>

#include "portsorch.h"

extern "C" {
#include "sai.h"
}

#include "logger.h"

extern mutex gDbMutex;
extern PortsOrch *gPortsOrch;

void on_port_state_change(uint32_t count, sai_port_oper_status_notification_t *data)
{
    SWSS_LOG_ENTER();

    lock_guard<mutex> lock(gDbMutex);

    if (!gPortsOrch)
    {
        SWSS_LOG_NOTICE("gPortsOrch is not initialized");
        return;
    }

    for (uint32_t i = 0; i < count; i++)
    {
        sai_object_id_t id = data[i].port_id;
        sai_port_oper_status_t status = data[i].port_state;

        SWSS_LOG_NOTICE("Get port state change notification id:%llx status:%d", id, status);

        gPortsOrch->updateDbPortOperStatus(id, status);
        gPortsOrch->setHostIntfsOperStatus(id, status == SAI_PORT_OPER_STATUS_UP);
    }
}

void on_switch_shutdown_request()
{
    SWSS_LOG_ENTER();

    /* TODO: Later a better restart story will be told here */
    SWSS_LOG_ERROR("Syncd stopped");

    exit(EXIT_FAILURE);
}

sai_switch_notification_t switch_notifications
{
    NULL,
    NULL,
    on_port_state_change,
    NULL,
    on_switch_shutdown_request,
    NULL
};
