#include <assert.h>
#include <iostream>
#include <vector>
#include <unordered_map>
#include <utility>
#include <inttypes.h>

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

FdbOrch::FdbOrch(TableConnector applDbConnector, TableConnector stateDbConnector, PortsOrch *port) :
    Orch(applDbConnector.first, applDbConnector.second, fdborch_pri),
    m_portsOrch(port),
    m_table(applDbConnector.first, applDbConnector.second),
    m_fdbStateTable(stateDbConnector.first, stateDbConnector.second)
{
    m_portsOrch->attach(this);
    m_flushNotificationsConsumer = new NotificationConsumer(applDbConnector.first, "FLUSHFDBREQUEST");
    auto flushNotifier = new Notifier(m_flushNotificationsConsumer, this, "FLUSHFDBREQUEST");
    Orch::addExecutor(flushNotifier);

    /* Add FDB notifications support from ASIC */
    DBConnector *notificationsDb = new DBConnector("ASIC_DB", 0);
    m_fdbNotificationConsumer = new swss::NotificationConsumer(notificationsDb, "NOTIFICATIONS");
    auto fdbNotifier = new Notifier(m_fdbNotificationConsumer, this, "FDB_NOTIFICATIONS");
    Orch::addExecutor(fdbNotifier);
}

bool FdbOrch::bake()
{
    Orch::bake();

    auto consumer = dynamic_cast<Consumer *>(getExecutor(APP_FDB_TABLE_NAME));
    if (consumer == NULL)
    {
        SWSS_LOG_ERROR("No consumer %s in Orch", APP_FDB_TABLE_NAME);
        return false;
    }

    size_t refilled = consumer->refillToSync(&m_fdbStateTable);
    SWSS_LOG_NOTICE("Add warm input FDB State: %s, %zd", APP_FDB_TABLE_NAME, refilled);
    return true;
}

bool FdbOrch::storeFdbEntryState(const FdbUpdate& update)
{
    const FdbEntry& entry = update.entry;
    const MacAddress& mac = entry.mac;
    Port vlan;

    if (!m_portsOrch->getPort(entry.bv_id, vlan))
    {
        SWSS_LOG_NOTICE("FdbOrch notification: Failed to locate \
                         vlan port from bv_id 0x%" PRIx64, entry.bv_id);
        return false;
    }

    // ref: https://github.com/Azure/sonic-swss/blob/master/doc/swss-schema.md#fdb_table
    string key = "Vlan" + to_string(vlan.m_vlan_info.vlan_id) + ":" + mac.to_string();

    if (update.add)
    {
        SWSS_LOG_INFO("Storing FDB entry: [%s, 0x%" PRIx64 "] [ port: %s ]",
                      entry.mac.to_string().c_str(),
                      entry.bv_id, entry.port_name.c_str());
        auto inserted = m_entries.insert(entry);

        SWSS_LOG_DEBUG("FdbOrch notification: mac %s was inserted into bv_id 0x%" PRIx64,
                        entry.mac.to_string().c_str(), entry.bv_id);

        if (!inserted.second)
        {
            SWSS_LOG_INFO("FdbOrch notification: mac %s is duplicate", entry.mac.to_string().c_str());
            return false;
        }

        // Write to StateDb
        std::vector<FieldValueTuple> fvs;
        fvs.push_back(FieldValueTuple("port", entry.port_name));
        fvs.push_back(FieldValueTuple("type", "dynamic"));
        m_fdbStateTable.set(key, fvs);

        gCrmOrch->incCrmResUsedCounter(CrmResourceType::CRM_FDB_ENTRY);
        return true;
    }
    else
    {
        size_t erased = m_entries.erase(entry);
        SWSS_LOG_DEBUG("FdbOrch notification: mac %s was removed from bv_id 0x%" PRIx64, entry.mac.to_string().c_str(), entry.bv_id);

        if (erased == 0)
        {
            return false;
        }

        // Remove in StateDb
        m_fdbStateTable.del(key);

        gCrmOrch->decCrmResUsedCounter(CrmResourceType::CRM_FDB_ENTRY);
        return true;
    }
}

void FdbOrch::update(sai_fdb_event_t        type, 
                     const sai_fdb_entry_t* entry, 
                     sai_object_id_t        bridge_port_id)
{
    SWSS_LOG_ENTER();

    FdbUpdate update;
    update.entry.mac = entry->mac_address;
    update.entry.bv_id = entry->bv_id;

    SWSS_LOG_INFO("FDB event:%d, MAC: %s , BVID: 0x%" PRIx64 " , \
                   bridge port ID: 0x%" PRIx64 ".", 
                   type, update.entry.mac.to_string().c_str(), 
                   entry->bv_id, bridge_port_id);

    if (bridge_port_id && 
        !m_portsOrch->getPortByBridgePortId(bridge_port_id, update.port))
    {
        SWSS_LOG_ERROR("Failed to get port by bridge port ID 0x%" PRIx64 ".",
                        bridge_port_id);
        return;
    }

    switch (type)
    {
    case SAI_FDB_EVENT_LEARNED:
        if (!m_portsOrch->getPortByBridgePortId(bridge_port_id, update.port))
        {
            SWSS_LOG_ERROR("Failed to get port by bridge port ID 0x%" PRIx64, bridge_port_id);
            return;
        }

        // we already have such entries
        if (m_entries.find(update.entry) != m_entries.end())
        {
             SWSS_LOG_INFO("FdbOrch notification: mac %s is already in bv_id 0x%" PRIx64,
                    update.entry.mac.to_string().c_str(), entry->bv_id);
             break;
        }

        update.add = true;
        update.entry.port_name = update.port.m_alias;
        storeFdbEntryState(update);

        SWSS_LOG_INFO("Notifying observers of FDB entry LEARN");
        for (auto observer: m_observers)
        {
            observer->update(SUBJECT_TYPE_FDB_CHANGE, &update);
        }

        break;

    case SAI_FDB_EVENT_AGED:
    case SAI_FDB_EVENT_MOVE:
        update.add = false;
        storeFdbEntryState(update);

        SWSS_LOG_INFO("Notifying observers of FDB entry removal on AGED/MOVED");
        for (auto observer: m_observers)
        {
            observer->update(SUBJECT_TYPE_FDB_CHANGE, &update);
        }

        break;

    case SAI_FDB_EVENT_FLUSHED:

        SWSS_LOG_INFO("FDB Flush event received: [ %s , 0x%" PRIx64 " ], \
                       bridge port ID: 0x%" PRIx64 ".",
                       update.entry.mac.to_string().c_str(), entry->bv_id,
                       bridge_port_id);

        string vlanName = "-";
        if (entry->bv_id) {
            Port vlan;

            if (!m_portsOrch->getPort(entry->bv_id, vlan))
            {
                SWSS_LOG_ERROR("FdbOrch notification: Failed to locate vlan\
                                port from bv_id 0x%" PRIx64, entry->bv_id);
                return;
            }
            vlanName = "Vlan" + to_string(vlan.m_vlan_info.vlan_id);
        }


        if (bridge_port_id == SAI_NULL_OBJECT_ID && 
            entry->bv_id == SAI_NULL_OBJECT_ID)
        {
            SWSS_LOG_INFO("FDB Flush: [ %s , %s ] = { port: - }",
                           update.entry.mac.to_string().c_str(), vlanName.c_str());
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
                itr++;

                storeFdbEntryState(update);

                for (auto observer: m_observers)
                {
                    observer->update(SUBJECT_TYPE_FDB_CHANGE, &update);
                }
            }
        }
        else if (entry->bv_id == SAI_NULL_OBJECT_ID)
        {
            /* FLUSH based on port */
            SWSS_LOG_INFO("FDB Flush: [ %s , %s ] = { port: %s }",
                           update.entry.mac.to_string().c_str(),
                           vlanName.c_str(), update.port.m_alias.c_str());

            for (auto itr = m_entries.begin(); itr != m_entries.end();)
            {
                auto next_item = std::next(itr);
                if (itr->port_name == update.port.m_alias)
                {
                    update.entry.mac = itr->mac;
                    update.entry.bv_id = itr->bv_id;
                    update.add = false;

                    storeFdbEntryState(update);

                    for (auto observer: m_observers)
                    {
                        observer->update(SUBJECT_TYPE_FDB_CHANGE, &update);
                    }
                }
                itr = next_item;
            }
        }
        else if (bridge_port_id == SAI_NULL_OBJECT_ID)
        {
            /* FLUSH based on VLAN - unsupported */
            SWSS_LOG_ERROR("Unsupported FDB Flush: [ %s , %s ] = { port: - }",
                           update.entry.mac.to_string().c_str(),
                           vlanName.c_str());

        }
        else
        {
            /* FLUSH based on port and VLAN - unsupported */
            SWSS_LOG_ERROR("Unsupported FDB Flush: [ %s , %s ] = { port: %s }",
                           update.entry.mac.to_string().c_str(),
                           vlanName.c_str(), update.port.m_alias.c_str());
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
        case SUBJECT_TYPE_PORT_OPER_STATE_CHANGE:
        {
            PortOperStateUpdate *update = reinterpret_cast<PortOperStateUpdate *>(cntx);
            updatePortOperState(*update);
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
        SWSS_LOG_ERROR("Failed to get port by bridge port ID 0x%" PRIx64, attr.value.oid);
        return false;
    }

    return true;
}

void FdbOrch::doTask(Consumer& consumer)
{
    SWSS_LOG_ENTER();

    if (!gPortsOrch->allPortsReady())
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

            entry.port_name = port;
            /* FDB type is either dynamic or static */
            assert(type == "dynamic" || type == "static");

            if (addFdbEntry(entry, type))
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

void FdbOrch::doTask(NotificationConsumer& consumer)
{
    SWSS_LOG_ENTER();

    if (!gPortsOrch->allPortsReady())
    {
        return;
    }

    sai_status_t status;
    std::string op;
    std::string data;
    std::vector<swss::FieldValueTuple> values;

    consumer.pop(op, data, values);

    if (&consumer == m_flushNotificationsConsumer)
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
    else if (&consumer == m_fdbNotificationConsumer && op == "fdb_event")
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
        }

        sai_deserialize_free_fdb_event_ntf(count, fdbevent);
    }
}

void FdbOrch::flushFDBEntries(sai_object_id_t bridge_port_oid,
                              sai_object_id_t vlan_oid)
{
    vector<sai_attribute_t>    attrs;
    sai_attribute_t            attr;
    sai_status_t               rv = SAI_STATUS_SUCCESS;

    SWSS_LOG_ENTER();

    if (SAI_NULL_OBJECT_ID == bridge_port_oid &&
        SAI_NULL_OBJECT_ID == vlan_oid)
    {
        SWSS_LOG_WARN("Couldn't flush FDB. Bridge port OID: 0x%" PRIx64 " bvid:%" PRIx64 ",",
                      bridge_port_oid, vlan_oid);
        return;
    }

    if (SAI_NULL_OBJECT_ID != bridge_port_oid)
    {
        attr.id = SAI_FDB_FLUSH_ATTR_BRIDGE_PORT_ID;
        attr.value.oid = bridge_port_oid;
        attrs.push_back(attr);
    }

    if (SAI_NULL_OBJECT_ID != vlan_oid)
    {
        attr.id = SAI_FDB_FLUSH_ATTR_BV_ID;
        attr.value.oid = vlan_oid;
        attrs.push_back(attr);
    }

    SWSS_LOG_INFO("Flushing FDB bridge_port_oid: 0x%" PRIx64 ", and bvid_oid:0x%" PRIx64 ".", bridge_port_oid, vlan_oid);

    rv = sai_fdb_api->flush_fdb_entries(gSwitchId, (uint32_t)attrs.size(), attrs.data());
    if (SAI_STATUS_SUCCESS != rv)
    {
        SWSS_LOG_ERROR("Flushing FDB failed. rv:%d", rv);
    }
}

void FdbOrch::updatePortOperState(const PortOperStateUpdate& update)
{
    SWSS_LOG_ENTER();
    if (update.operStatus == SAI_PORT_OPER_STATUS_DOWN)
    {
        swss::Port p = update.port;
        flushFDBEntries(p.m_bridge_port_id, SAI_NULL_OBJECT_ID);
    }
    return;
}

void FdbOrch::updateVlanMember(const VlanMemberUpdate& update)
{
    SWSS_LOG_ENTER();

    if (!update.add)
    {
        swss::Port vlan = update.vlan;
        swss::Port port = update.member;
        flushFDBEntries(port.m_bridge_port_id, vlan.m_vlan_info.vlan_oid);
        return;
    }

    string port_name = update.member.m_alias;
    auto fdb_list = std::move(saved_fdb_entries[port_name]);
    if(!fdb_list.empty())
    {
        for (const auto& fdb: fdb_list)
        {
            // try to insert an FDB entry. If the FDB entry is not ready to be inserted yet,
            // it would be added back to the saved_fdb_entries structure by addFDBEntry()
            (void)addFdbEntry(fdb.entry, fdb.type);
        }
    }
}

bool FdbOrch::addFdbEntry(const FdbEntry& entry, const string& type)
{
    SWSS_LOG_ENTER();

    sai_fdb_entry_t fdb_entry;

    fdb_entry.switch_id = gSwitchId;
    memcpy(fdb_entry.mac_address, entry.mac.getMac(), sizeof(sai_mac_t));
    fdb_entry.bv_id = entry.bv_id;

    Port port;
    /* Retry until port is created */
    if (!m_portsOrch->getPort(entry.port_name, port))
    {
        SWSS_LOG_DEBUG("Saving a fdb entry until port %s becomes active",
                        entry. port_name.c_str());
        saved_fdb_entries[entry.port_name].push_back({entry, type});

        return true;
    }

    /* Retry until port is added to the VLAN */
    if (!port.m_bridge_port_id)
    {
        SWSS_LOG_DEBUG("Saving a fdb entry until port %s has got a bridge port ID", 
                        entry.port_name.c_str());
        saved_fdb_entries[entry.port_name].push_back({entry, type});

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

    if (m_entries.count(entry) != 0) // we already have such entries
    {
        removeFdbEntry(entry);
    }

    sai_status_t status = sai_fdb_api->create_fdb_entry(&fdb_entry, (uint32_t)attrs.size(), attrs.data());
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to create %s FDB %s on %s, rv:%d",
                type.c_str(), entry.mac.to_string().c_str(),
                entry.port_name.c_str(), status);
        return false; //FIXME: it should be based on status. Some could be retried, some not
    }

    SWSS_LOG_NOTICE("Storing FDB entry: [%s, 0x%" PRIx64 "] [ port: %s , type: %s]",
                    entry.mac.to_string().c_str(),
                    entry.bv_id, entry.port_name.c_str(), type.c_str());

    (void) m_entries.insert(entry);

    gCrmOrch->incCrmResUsedCounter(CrmResourceType::CRM_FDB_ENTRY);

    FdbUpdate update = {entry, port, true};
    for (auto observer: m_observers)
    {
        observer->update(SUBJECT_TYPE_FDB_CHANGE, &update);
    }

    return true;
}

bool FdbOrch::removeFdbEntry(const FdbEntry& entry)
{
    SWSS_LOG_ENTER();

    if (m_entries.count(entry) == 0)
    {
        SWSS_LOG_ERROR("FDB entry isn't found. mac=%s bv_id=0x%" PRIx64 ".",
                        entry.mac.to_string().c_str(), entry.bv_id);
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
        SWSS_LOG_ERROR("Failed to remove FDB entry. mac=%s, bv_id=0x%" PRIx64,
                       entry.mac.to_string().c_str(), entry.bv_id);
        return true; //FIXME: it should be based on status. Some could be retried. some not
    }

    (void)m_entries.erase(entry);

    gCrmOrch->decCrmResUsedCounter(CrmResourceType::CRM_FDB_ENTRY);

    Port port;
    m_portsOrch->getPortByBridgePortId(entry.bv_id, port);

    SWSS_LOG_INFO("Notifying observers of FDB entry removal");
    FdbUpdate update = {entry, port, false};
    for (auto observer: m_observers)
    {
        observer->update(SUBJECT_TYPE_FDB_CHANGE, &update);
    }

    return true;
}
