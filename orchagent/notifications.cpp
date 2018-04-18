#include <unordered_map>
#include <assert.h>

#include "portsorch.h"
#include "fdborch.h"

extern "C" {
#include "sai.h"
}

#include "logger.h"
#include "notifications.h"

extern PortsOrch *gPortsOrch;
extern FdbOrch *gFdbOrch;

void on_fdb_event(uint32_t count, sai_fdb_event_notification_data_t *data)
{
    SWSS_LOG_ENTER();

    if (!gFdbOrch)
    {
        SWSS_LOG_NOTICE("gFdbOrch is not initialized");
        return;
    }

    for (uint32_t i = 0; i < count; ++i)
    {
        sai_object_id_t oid = SAI_NULL_OBJECT_ID;

        for (uint32_t j = 0; j < data[i].attr_count; ++j)
        {
            if (data[i].attr[j].id == SAI_FDB_ENTRY_ATTR_BRIDGE_PORT_ID)
            {
                oid = data[i].attr[j].value.oid;
                break;
            }
        }

        gFdbOrch->update(data[i].event_type, &data[i].fdb_entry, oid);
    }
}

void on_port_state_change(uint32_t count, sai_port_oper_status_notification_t *data)
{
    SWSS_LOG_ENTER();

    if (!gPortsOrch)
    {
        SWSS_LOG_NOTICE("gPortsOrch is not initialized");
        return;
    }

    for (uint32_t i = 0; i < count; i++)
    {
        sai_object_id_t id = data[i].port_id;
        sai_port_oper_status_t status = data[i].port_state;

        SWSS_LOG_NOTICE("Get port state change notification id:%lx status:%d", id, status);

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
