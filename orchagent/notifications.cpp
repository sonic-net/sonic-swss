#include "portsorch.h"

extern "C" {
#include "sai.h"
}

#include "logger.h"

extern PortsOrch *gPortsOrch;

void on_port_state_change(uint32_t count, sai_port_oper_status_notification_t *data)
{
    /* Wait until gPortsOrch is initialized */
    if (!gPortsOrch || !gPortsOrch->isInitDone())
        return;

    for (uint32_t i = 0; i < count; i++)
    {
        sai_object_id_t id = data[i].port_id;
        sai_port_oper_status_t status = data[i].port_state;

        gPortsOrch->updateDbPortOperStatus(id, status);
        gPortsOrch->setHostIntfsOperStatus(id, status == SAI_PORT_OPER_STATUS_UP);
    }
}

sai_switch_notification_t switch_notifications
{
    NULL,
    NULL,
    on_port_state_change,
    NULL,
    NULL,
    NULL
};
