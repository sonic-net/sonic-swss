#include <assert.h>
#include <iostream>
#include <vector>
#include <unordered_map>
#include <utility>

#include "logger.h"
#include "tokenize.h"
#include "fdborch.h"
#include "crmorch.h"
#include "notifier.h"
#include "sai_serialize.h"

extern sai_fdb_api_t    *sai_fdb_api;

extern sai_object_id_t  gSwitchId;
extern PortsOrch*       gPortsOrch;
extern CrmOrch *        gCrmOrch;

const int fdborch_pri = 20;

FdbOrch::FdbOrch(DBConnector *db, string tableName, PortsOrch *port) :
    Orch(db, tableName, fdborch_pri),
    m_portsOrch(port),
    m_table(Table(db, tableName))
{
    m_portsOrch->attach(this);
    auto consumer = new NotificationConsumer(db, "FLUSHFDBREQUEST");
    auto fdbNotification = new Notifier(consumer, this, "FLUSHFDBREQUEST");
    Orch::addExecutor("", fdbNotification);

    DBConnector *notification_db = new DBConnector(ASIC_DB, DBConnector::DEFAULT_UNIXSOCKET, 0);
    auto notification_consumer = new swss::NotificationConsumer(notification_db, "NOTIFICATIONS");
    auto fdb_notification = new Notifier(notification_consumer, this, "NOTIFICATIONS");
    Orch::addExecutor("FDB_NOTIFICATIONS", fdb_notification);
}

void FdbOrch::update(sai_fdb_event_t type, const sai_fdb_entry_t* entry, sai_object_id_t bridge_port_id)
{
    SWSS_LOG_ENTER();

    FdbUpdate update;
    update.entry.mac = entry->mac_address;
    update.entry.bv_id = entry->bv_id;

    switch (type)
    {
    case SAI_FDB_EVENT_LEARNED:
        if (!m_portsOrch->getPortByBridgePortId(bridge_port_id, update.port))
        {
            SWSS_LOG_ERROR("Failed to get port by bridge port ID 0x%lx", bridge_port_id);
            return;
        }

        update.add = true;

        {
            auto ret = m_entries.insert(update.entry);

            SWSS_LOG_DEBUG("FdbOrch notification: mac %s was inserted into bv_id 0x%lx",
                            update.entry.mac.to_string().c_str(), entry->bv_id);

            if (ret.second)
            {
                gCrmOrch->incCrmResUsedCounter(CrmResourceType::CRM_FDB_ENTRY);
            }
            else
            {
                SWSS_LOG_INFO("FdbOrch notification: mac %s is duplicate", update.entry.mac.to_string().c_str());
            }
        }

        for (auto observer: m_observers)
        {
            observer->update(SUBJECT_TYPE_FDB_CHANGE, &update);
        }

        break;

    case SAI_FDB_EVENT_AGED:
    case SAI_FDB_EVENT_MOVE:
        update.add = false;

        {
            auto ret = m_entries.erase(update.entry);
            SWSS_LOG_DEBUG("FdbOrch notification: mac %s was removed from bv_id 0x%lx", update.entry.mac.to_string().c_str(), entry->bv_id);

            if (ret)
            {
                gCrmOrch->decCrmResUsedCounter(CrmResourceType::CRM_FDB_ENTRY);
            }
        }

        for (auto observer: m_observers)
        {
            observer->update(SUBJECT_TYPE_FDB_CHANGE, &update);
        }

        break;

    case SAI_FDB_EVENT_FLUSHED:
        if (bridge_port_id == SAI_NULL_OBJECT_ID && entry->bv_id == SAI_NULL_OBJECT_ID)
        {
            for (auto itr = m_entries.begin(); itr != m_entries.end();)
            {
                /*
                   TODO: here should only delete the dynamic fdb entries,
                   but unfortunately in structure FdbEntry currently have
                   no member to indicate the fdb entry type,
                   if there is static mac added, here will have issue.
                */
                update.entry.mac = itr->mac;
                update.entry.bv_id = itr->bv_id;
                update.add = false;

                itr = m_entries.erase(itr);

                SWSS_LOG_DEBUG("FdbOrch notification: mac %s was removed", update.entry.mac.to_string().c_str());

                gCrmOrch->decCrmResUsedCounter(CrmResourceType::CRM_FDB_ENTRY);

                for (auto observer: m_observers)
                {
                    observer->update(SUBJECT_TYPE_FDB_CHANGE, &update);
                }
            }
        }
        else if (bridge_port_id && entry->bv_id == SAI_NULL_OBJECT_ID)
        {
            /*this is a placeholder for flush port fdb case, not supported yet.*/
            SWSS_LOG_ERROR("FdbOrch notification: not supported flush port fdb action, port_id = 0x%lx, bv_id = 0x%lx.", bridge_port_id, entry->bv_id);
        }
        else if (bridge_port_id == SAI_NULL_OBJECT_ID && entry->bv_id != SAI_NULL_OBJECT_ID)
        {
            /*this is a placeholder for flush vlan fdb case, not supported yet.*/
            SWSS_LOG_ERROR("FdbOrch notification: not supported flush vlan fdb action, port_id = 0x%lx, bv_id = 0x%lx.", bridge_port_id, entry->bv_id);
        }
        else
        {
            SWSS_LOG_ERROR("FdbOrch notification: not supported flush fdb action, port_id = 0x%lx, bv_id = 0x%lx.", bridge_port_id, entry->bv_id);
        }
        break;
    }

    return;
}

void FdbOrch::update(SubjectType type, void *cntx)
{
    SWSS_LOG_ENTER();

    assert(cntx);

    switch(type) {
        case SUBJECT_TYPE_VLAN_MEMBER_CHANGE:
        {
            VlanMemberUpdate *update = reinterpret_cast<VlanMemberUpdate *>(cntx);
            updateVlanMember(*update);
            break;
        }
        default:
            break;
    }

    return;
}

bool FdbOrch::getPort(const MacAddress& mac, uint16_t vlan, Port& port)
{
    SWSS_LOG_ENTER();

    if (!m_portsOrch->getVlanByVlanId(vlan, port))
    {
        SWSS_LOG_ERROR("Failed to get vlan by vlan ID %d", vlan);
        return false;
    }

    sai_fdb_entry_t entry;
    entry.switch_id = gSwitchId;
    memcpy(entry.mac_address, mac.getMac(), sizeof(sai_mac_t));
    entry.bv_id = port.m_vlan_info.vlan_oid;

    sai_attribute_t attr;
    attr.id = SAI_FDB_ENTRY_ATTR_BRIDGE_PORT_ID;

    sai_status_t status = sai_fdb_api->get_fdb_entry_attribute(&entry, 1, &attr);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to get bridge port ID for FDB entry %s, rv:%d",
            mac.to_string().c_str(), status);
        return false;
    }

    if (!m_portsOrch->getPortByBridgePortId(attr.value.oid, port))
    {
        SWSS_LOG_ERROR("Failed to get port by bridge port ID 0x%lx", attr.value.oid);
        return false;
    }

    return true;
}

void FdbOrch::doTask(Consumer& consumer)
{
    SWSS_LOG_ENTER();

    if (!gPortsOrch->isInitDone())
    {
        return;
    }

    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
        KeyOpFieldsValuesTuple t = it->second;

        /* format: <VLAN_name>:<MAC_address> */
        vector<string> keys = tokenize(kfvKey(t), ':', 1);
        string op = kfvOp(t);

        Port vlan;
        if (!m_portsOrch->getPort(keys[0], vlan))
        {
            SWSS_LOG_INFO("Failed to locate %s", keys[0].c_str());
            it++;
            continue;
        }

        FdbEntry entry;
        entry.mac = MacAddress(keys[1]);
        entry.bv_id = vlan.m_vlan_info.vlan_oid;

        if (op == SET_COMMAND)
        {
            string port;
            string type;

            for (auto i : kfvFieldsValues(t))
            {
                if (fvField(i) == "port")
                {
                    port = fvValue(i);
                }

                if (fvField(i) == "type")
                {
                    type = fvValue(i);
                }
            }

            /* FDB type is either dynamic or static */
            assert(type == "dynamic" || type == "static");

            if (addFdbEntry(entry, port, type))
                it = consumer.m_toSync.erase(it);
            else
                it++;

            /* Remove corresponding APP_DB entry if type is 'dynamic' */
            // FIXME: The modification of table is not thread safe.
            // Uncomment this after this issue is fixed.
            // if (type == "dynamic")
            // {
            //     m_table.del(kfvKey(t));
            // }
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

void FdbOrch::doTask(NotificationConsumer& consumer, const std::string &name)
{
    SWSS_LOG_ENTER();

    if (!gPortsOrch->isInitDone())
    {
        return;
    }

    sai_status_t status;
    std::string op;
    std::string data;
    std::vector<swss::FieldValueTuple> values;

    consumer.pop(op, data, values);

    if (name == "FLUSHFDBREQUEST")
    {
        if (op == "ALL")
        {
            /*
             * so far only support flush all the FDB entris
             * flush per port and flush per vlan will be added later.
             */
            status = sai_fdb_api->flush_fdb_entries(gSwitchId, 0, NULL);
            if (status != SAI_STATUS_SUCCESS)
            {
                SWSS_LOG_ERROR("Flush fdb failed, return code %x", status);
            }

            return;
        }
        else if (op == "PORT")
        {
            /*place holder for flush port fdb*/
            SWSS_LOG_ERROR("Received unsupported flush port fdb request");
    	    return;
        }
        else if (op == "VLAN")
        {
            /*place holder for flush vlan fdb*/
            SWSS_LOG_ERROR("Received unsupported flush vlan fdb request");
    	    return;
        }
        else
        {
            SWSS_LOG_ERROR("Received unknown flush fdb request");
    	    return;
        }
    }
    else if (name == "NOTIFICATIONS" && op == "fdb_event")
    {
        uint32_t count;
        sai_fdb_event_notification_data_t *fdbevent = nullptr;

        sai_deserialize_fdb_event_ntf(data, count, &fdbevent);

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

            this->update(fdbevent[i].event_type, &fdbevent[i].fdb_entry, oid);

            sai_deserialize_free_fdb_event_ntf(count, fdbevent);
        }
    }
}

void FdbOrch::updateVlanMember(const VlanMemberUpdate& update)
{
    SWSS_LOG_ENTER();

    if (!update.add)
    {
        return; // we need additions only
    }

    string port_name = update.member.m_alias;
    auto fdb_list = std::move(saved_fdb_entries[port_name]);
    if(!fdb_list.empty())
    {
        for (const auto& fdb: fdb_list)
        {
            // try to insert an FDB entry. If the FDB entry is not ready to be inserted yet,
            // it would be added back to the saved_fdb_entries structure by addFDBEntry()
            (void)addFdbEntry(fdb.entry, port_name, fdb.type);
        }
    }
}

bool FdbOrch::addFdbEntry(const FdbEntry& entry, const string& port_name, const string& type)
{
    SWSS_LOG_ENTER();

    if (m_entries.count(entry) != 0) // we already have such entries
    {
        // FIXME: should we check that the entry are moving to another port?
        // FIXME: should we check that the entry are changing its type?
        SWSS_LOG_ERROR("FDB entry already exists. mac=%s bv_id=0x%lx", entry.mac.to_string().c_str(), entry.bv_id);
        return true;
    }

    sai_fdb_entry_t fdb_entry;

    fdb_entry.switch_id = gSwitchId;
    memcpy(fdb_entry.mac_address, entry.mac.getMac(), sizeof(sai_mac_t));
    fdb_entry.bv_id = entry.bv_id;

    Port port;
    /* Retry until port is created */
    if (!m_portsOrch->getPort(port_name, port))
    {
        SWSS_LOG_DEBUG("Saving a fdb entry until port %s becomes active", port_name.c_str());
        saved_fdb_entries[port_name].push_back({entry, type});

        return true;
    }

    /* Retry until port is added to the VLAN */
    if (!port.m_bridge_port_id)
    {
        SWSS_LOG_DEBUG("Saving a fdb entry until port %s has got a bridge port ID", port_name.c_str());
        saved_fdb_entries[port_name].push_back({entry, type});

        return true;
    }

    sai_attribute_t attr;
    vector<sai_attribute_t> attrs;

    attr.id = SAI_FDB_ENTRY_ATTR_TYPE;
    attr.value.s32 = (type == "dynamic") ? SAI_FDB_ENTRY_TYPE_DYNAMIC : SAI_FDB_ENTRY_TYPE_STATIC;
    attrs.push_back(attr);

    attr.id = SAI_FDB_ENTRY_ATTR_BRIDGE_PORT_ID;
    attr.value.oid = port.m_bridge_port_id;
    attrs.push_back(attr);

    attr.id = SAI_FDB_ENTRY_ATTR_PACKET_ACTION;
    attr.value.s32 = SAI_PACKET_ACTION_FORWARD;
    attrs.push_back(attr);

    sai_status_t status = sai_fdb_api->create_fdb_entry(&fdb_entry, (uint32_t)attrs.size(), attrs.data());
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to create %s FDB %s on %s, rv:%d",
                type.c_str(), entry.mac.to_string().c_str(), port_name.c_str(), status);
        return false; //FIXME: it should be based on status. Some could be retried, some not
    }

    SWSS_LOG_NOTICE("Create %s FDB %s on %s", type.c_str(), entry.mac.to_string().c_str(), port_name.c_str());

    (void) m_entries.insert(entry);

    gCrmOrch->incCrmResUsedCounter(CrmResourceType::CRM_FDB_ENTRY);

    return true;
}

bool FdbOrch::removeFdbEntry(const FdbEntry& entry)
{
    SWSS_LOG_ENTER();

    if (m_entries.count(entry) == 0)
    {
        SWSS_LOG_ERROR("FDB entry isn't found. mac=%s bv_id=0x%lx", entry.mac.to_string().c_str(), entry.bv_id);
        return true;
    }

    sai_status_t status;
    sai_fdb_entry_t fdb_entry;
    fdb_entry.switch_id = gSwitchId;
    memcpy(fdb_entry.mac_address, entry.mac.getMac(), sizeof(sai_mac_t));
    fdb_entry.bv_id = entry.bv_id;

    status = sai_fdb_api->remove_fdb_entry(&fdb_entry);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to remove FDB entry. mac=%s, bv_id=0x%lx",
                       entry.mac.to_string().c_str(), entry.bv_id);
        return true; //FIXME: it should be based on status. Some could be retried. some not
    }

    (void)m_entries.erase(entry);

    gCrmOrch->decCrmResUsedCounter(CrmResourceType::CRM_FDB_ENTRY);

    return true;
}
