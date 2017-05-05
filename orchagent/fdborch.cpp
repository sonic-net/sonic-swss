#include <assert.h>
#include <iostream>
#include <vector>

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
            SWSS_LOG_ERROR("Failed to get port for %lu OID", portOid);
            return;
        }

        update.add = true;

        (void)m_entries.insert(update.entry);
        SWSS_LOG_DEBUG("FdbOrch notification: mac %s was inserted into vlan %d", update.entry.mac.to_string().c_str(), entry->vlan_id);
        break;
    case SAI_FDB_EVENT_AGED:
    case SAI_FDB_EVENT_FLUSHED:
        update.add = false;

        (void)m_entries.erase(update.entry);
        SWSS_LOG_DEBUG("FdbOrch notification: mac %s was removed from vlan %d", update.entry.mac.to_string().c_str(), entry->vlan_id);
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
        SWSS_LOG_ERROR("Failed to get port for FDB entry OID");
        return false;
    }

    if (!m_portsOrch->getPort(attr.value.oid, port))
    {
        SWSS_LOG_ERROR("Failed to get port for %lu OID", attr.value.oid);
        return false;
    }

    return true;
}

void FdbOrch::doTask(Consumer& consumer)
{
    SWSS_LOG_ENTER();

    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
        KeyOpFieldsValuesTuple t = it->second;

        string key = kfvKey(t);
        string op = kfvOp(t);

        FdbEntry entry;
        if (!splitKey(key, entry))
        {
            it = consumer.m_toSync.erase(it);
            continue;
        }

        if (op == SET_COMMAND)
        {
            string port;
            string type;
            bool port_defined = false;
            bool type_defined = false;

            for (auto i : kfvFieldsValues(t))
            {
                if (fvField(i) == "port")
                {
                    port = fvValue(i);
                    port_defined = true;
                }

                if (fvField(i) == "type")
                {
                    type = fvValue(i);
                    type_defined = true;
                }
            }

            if (!port_defined)
            {
                SWSS_LOG_ERROR("FDB entry with key:'%s' must have 'port' attribute", key.c_str());
                it = consumer.m_toSync.erase(it);
                continue;
            }

            if (!type_defined)
            {
                SWSS_LOG_ERROR("FDB entry with key:'%s' must have 'type' attribute", key.c_str());
                it = consumer.m_toSync.erase(it);
                continue;
            }

            // check that type either static or dynamic
            if (type != "static" && type != "dynamic")
            {
                SWSS_LOG_ERROR("FDB entry with key: '%s' has type '%s'. But allowed only types: 'static' or 'dynamic'",
                               key.c_str(), type.c_str());
                it = consumer.m_toSync.erase(it);
                continue;
            }

            if (addFdbEntry(entry, port, type))
                it = consumer.m_toSync.erase(it);
            else
                it++;
        }
        else if (op == DEL_COMMAND)
        {
            if (removeFdbEntry(entry))
                it = consumer.m_toSync.erase(it);
            else
                it++;

        }
        else
        {
            SWSS_LOG_ERROR("Unknown operation type %s", op.c_str());
            it = consumer.m_toSync.erase(it);
        }
    }
}

bool FdbOrch::addFdbEntry(const FdbEntry& entry, const string& port_name, const string& type)
{
    SWSS_LOG_ENTER();

    SWSS_LOG_DEBUG("mac=%s, vlan=%d. port_name %s. type %s",
                   entry.mac.to_string().c_str(), entry.vlan, port_name.c_str(), type.c_str());

    if (m_entries.count(entry) != 0) // we already have such entries
    {
        // FIXME: should we check that the entry are moving to another port?
        // FIXME: should we check that the entry are changing its type?
        SWSS_LOG_ERROR("FDB entry already exists. mac=%s vlan=%d", entry.mac.to_string().c_str(), entry.vlan);
        return true;
    }

    sai_status_t status;

    sai_fdb_entry_t fdb_entry;
    memcpy(fdb_entry.mac_address, entry.mac.getMac(), sizeof(sai_mac_t));
    fdb_entry.vlan_id = entry.vlan;

    Port port;
    if (!m_portsOrch->getPort(port_name, port))
    {
        SWSS_LOG_ERROR("Failed to get port id for %s", port_name.c_str());
        return true;
    }

    sai_attribute_t attr;
    vector<sai_attribute_t> attrs;

    attr.id = SAI_FDB_ENTRY_ATTR_TYPE;
    attr.value.s32 = (type == "dynamic") ? SAI_FDB_ENTRY_DYNAMIC : SAI_FDB_ENTRY_STATIC;
    attrs.push_back(attr);

    attr.id = SAI_FDB_ENTRY_ATTR_PORT_ID;
    attr.value.oid = port.m_port_id;
    attrs.push_back(attr);

    attr.id = SAI_FDB_ENTRY_ATTR_PACKET_ACTION;
    attr.value.s32 = SAI_PACKET_ACTION_FORWARD;
    attrs.push_back(attr);

    status = sai_fdb_api->create_fdb_entry(&fdb_entry, attrs.size(), attrs.data());
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to add FDB entry. mac=%s, vlan=%d. port_name %s. type %s",
                   entry.mac.to_string().c_str(), entry.vlan, port_name.c_str(), type.c_str());
        return true;
    }

    (void)m_entries.insert(entry);

    return true;
}

bool FdbOrch::removeFdbEntry(const FdbEntry& entry)
{
    SWSS_LOG_ENTER();

    SWSS_LOG_DEBUG("mac=%s, vlan=%d", entry.mac.to_string().c_str(), entry.vlan);

    if (m_entries.count(entry) == 0)
    {
        SWSS_LOG_ERROR("FDB entry isn't found. mac=%s vlan=%d", entry.mac.to_string().c_str(), entry.vlan);
        return true;
    }

    sai_status_t status;
    sai_fdb_entry_t fdb_entry;
    memcpy(fdb_entry.mac_address, entry.mac.getMac(), sizeof(sai_mac_t));
    fdb_entry.vlan_id = entry.vlan;

    status = sai_fdb_api->remove_fdb_entry(&fdb_entry);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to remove FDB entry. mac=%s, vlan=%d",
                       entry.mac.to_string().c_str(), entry.vlan);
        return true;
    }

    (void)m_entries.erase(entry);

    return true;
}

bool FdbOrch::splitKey(const string& key, FdbEntry& entry)
{
    SWSS_LOG_ENTER();

    string mac_address_str;
    string vlan_str;

    size_t found = key.rfind(':');
    if (found == string::npos)
    {
        SWSS_LOG_ERROR("Failed to parse key: %s", key.c_str());
        return false;
    }

    mac_address_str = key.substr(0, found);
    vlan_str = key.substr(found + 1, string::npos);
    if (vlan_str.length() <= 4) // "Vlan"
    {
        SWSS_LOG_ERROR("Failed to extract vlan interface name from the key: %s", key.c_str());
        return false;
    }

    uint8_t mac_array[6];
    if (!MacAddress::parseMacString(mac_address_str, mac_array)) // FIXME: parseMacString consider 52:54:00:25::E9 and 52:54:00:25:E9 as good one
    {
        SWSS_LOG_ERROR("Failed to parse mac address: %s in key: %s", mac_address_str.c_str(), key.c_str());
        return false;
    }

    if (mac_array[0] & 0x01)
    {
        SWSS_LOG_ERROR("Mac address %s in key %s should be unicast", mac_address_str.c_str(), key.c_str());
        return false;
    }

    entry.mac = MacAddress(mac_array);

    Port port;
    if (!m_portsOrch->getPort(vlan_str, port))
    {
        SWSS_LOG_ERROR("Failed to get port for %s", vlan_str.c_str());
        return false;
    }

    entry.vlan = stoi(vlan_str.substr(4)); // FIXME: create swss-common function to extract vlan number from vlan name
                                           // Currently this code could raise an exception

    return true;
}
