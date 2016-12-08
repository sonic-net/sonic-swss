#include <assert.h>
#include <iostream>

#include "logger.h"
#include "fdborch.h"

extern sai_fdb_api_t *sai_fdb_api;

void FdbOrch::update(sai_fdb_event_t type, const sai_fdb_entry_t* entry, sai_object_id_t portOid)
{
    SWSS_LOG_ENTER();

    FdbUpdate update;
    update.entry.mac = entry->mac_address;
    update.entry.vlan = entry->vlan_id;

    switch (type)
    {
    case SAI_FDB_EVENT_LEARNED:
        if (!m_portsOrch->getPort(portOid, update.port))
        {
            SWSS_LOG_ERROR("Failed to get port for %lu OID\n", portOid);
            return;
        }

        update.add = true;
        break;
    case SAI_FDB_EVENT_AGED:
    case SAI_FDB_EVENT_FLUSHED:
        update.add = false;
        break;
    }

    for (auto observer: m_observers)
    {
        observer->update(SUBJECT_TYPE_FDB_CHANGE, static_cast<void *>(&update));
    }
}

bool FdbOrch::getPort(const MacAddress& mac, uint16_t vlan, Port& port)
{
    SWSS_LOG_ENTER();

    sai_fdb_entry_t entry;
    memcpy(entry.mac_address, mac.getMac(), sizeof(sai_mac_t));
    entry.vlan_id = vlan;

    sai_attribute_t attr;
    attr.id = SAI_FDB_ENTRY_ATTR_PORT_ID;

    sai_status_t status = sai_fdb_api->get_fdb_entry_attribute(&entry, 1, &attr);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_INFO("Failed to get port for FDB entry OID\n");
        return false;
    }

    if (!m_portsOrch->getPort(attr.value.oid, port))
    {
        SWSS_LOG_ERROR("Failed to get port for %llu OID\n", attr.value.oid);
        return false;
    }

    return true;
}
