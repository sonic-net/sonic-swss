extern "C" {
#include "sai.h"
}

#include "logger.h"
#include "notifications.h"
#include "fdborch.h"

extern FdbOrch *gFdbOrch;

void on_fdb_event(uint32_t count, sai_fdb_event_notification_data_t *fdbevent)
{
    //std::lock_guard<std::mutex> lock(g_orchmutex);

    for (uint32_t i = 0; i < count; ++i)
    {
        sai_object_id_t oid = SAI_NULL_OBJECT_ID;

        for (uint32_t j = 0; j < fdbevent[i].attr_count; ++j)
        {
            if (fdbevent[i].attr[j].id == SAI_FDB_ENTRY_ATTR_BRIDGE_PORT_ID)
            {
                oid = fdbevent[i].attr[j].value.oid;
                break;
            }
        }

        gFdbOrch->update(fdbevent[i].event_type, &fdbevent[i].fdb_entry, oid);
    }
}

void on_port_state_change(uint32_t count, sai_port_oper_status_notification_t *data)
{
    // don't use this event handler, because it runs by libsairedis in a separate thread
    // which causes concurrency access to the DB
}

void on_switch_shutdown_request()
{
    SWSS_LOG_ENTER();

    /* TODO: Later a better restart story will be told here */
    SWSS_LOG_ERROR("Syncd stopped");

    exit(EXIT_FAILURE);
}
