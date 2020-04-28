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
#include "vxlanorch.h"
#include "directory.h"

extern sai_fdb_api_t    *sai_fdb_api;

extern sai_object_id_t  gSwitchId;
extern PortsOrch*       gPortsOrch;
extern CrmOrch *        gCrmOrch;
extern Directory<Orch*> gDirectory;

const int FdbOrch::fdborch_pri = 20;

FdbOrch::FdbOrch(DBConnector* applDbConnector, vector<table_name_with_pri_t> appFdbTables, TableConnector stateDbFdbConnector, PortsOrch *port) :
    Orch(applDbConnector, appFdbTables),
    m_portsOrch(port),
    m_fdbStateTable(stateDbFdbConnector.first, stateDbFdbConnector.second)
{
    for(auto it: appFdbTables)
    {
        m_appTables.push_back(new Table(applDbConnector, it.first));
    }

    m_portsOrch->attach(this);
    m_flushNotificationsConsumer = new NotificationConsumer(applDbConnector, "FLUSHFDBREQUEST");
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
    FdbData fdbdata;
    FdbData oldFdbData;
    const Port& port = update.port;
    const MacAddress& mac = entry.mac;
    string portName = port.m_alias;
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
        bool mac_move = false;
        auto it = m_entries.find(entry);
        if (it != m_entries.end())
        {
            /* This block is specifically added for MAC_MOVE event
               and not expected to be executed for LEARN event
             */
            if (port.m_bridge_port_id == it->second.bridge_port_id)
            {
                SWSS_LOG_INFO("FdbOrch notification: mac %s is duplicate", entry.mac.to_string().c_str());
                return false;
            }
            mac_move = true;
            oldFdbData = it->second;
        }

        fdbdata.bridge_port_id = update.port.m_bridge_port_id;
        fdbdata.type = update.type;
        fdbdata.origin = FDB_ORIGIN_LEARN;
        fdbdata.remote_ip = "";
        fdbdata.esi = "";
        fdbdata.vni = 0;

        m_entries[entry] = fdbdata;
        SWSS_LOG_INFO("FdbOrch notification: mac %s was inserted in port %s into bv_id 0x%" PRIx64,
                        entry.mac.to_string().c_str(), portName.c_str(), entry.bv_id);
        SWSS_LOG_INFO("m_entries size=%lu mac=%s port=0x%" PRIx64,
            m_entries.size(), entry.mac.to_string().c_str(),  m_entries[entry].bridge_port_id);

        // Write to StateDb
        std::vector<FieldValueTuple> fvs;
        fvs.push_back(FieldValueTuple("port", portName));
        fvs.push_back(FieldValueTuple("type", update.type));
        m_fdbStateTable.set(key, fvs);

        if (!mac_move)
        {
            gCrmOrch->incCrmResUsedCounter(CrmResourceType::CRM_FDB_ENTRY);
        }
        return true;
    }
    else
    {
        auto it= m_entries.find(entry);
        if(it != m_entries.end())
        {
            oldFdbData = it->second;
        }

        size_t erased = m_entries.erase(entry);
        SWSS_LOG_DEBUG("FdbOrch notification: mac %s was removed from bv_id 0x%" PRIx64, entry.mac.to_string().c_str(), entry.bv_id);

        if (erased == 0)
        {
            return false;
        }

        if (oldFdbData.origin != FDB_ORIGIN_VXLAN_ADVERTIZED)
        {
            // Remove in StateDb for non advertised mac addresses
            m_fdbStateTable.del(key);
        }

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
    update.type = "dynamic";
    Port vlan;

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
    {
        SWSS_LOG_INFO("Received LEARN event for bvid=0x%" PRIx64 "mac=%s port=0x%" PRIx64, entry->bv_id, update.entry.mac.to_string().c_str(), bridge_port_id);

        if (!m_portsOrch->getPort(entry->bv_id, vlan))
        {
            SWSS_LOG_ERROR("FdbOrch LEARN notification: Failed to locate vlan port from bv_id 0x%" PRIx64, entry->bv_id);
            return;
        }

        if (!m_portsOrch->getPortByBridgePortId(bridge_port_id, update.port))
        {
            SWSS_LOG_ERROR("FdbOrch LEARN notification: Failed to get port by bridge port ID 0x%" PRIx64, bridge_port_id);
            return;
        }

        // we already have such entries
        auto existing_entry = m_entries.find(update.entry);
        if (existing_entry != m_entries.end())
        {
             SWSS_LOG_INFO("FdbOrch LEARN notification: mac %s is already in bv_id 0x%"
                PRIx64 "existing-bp 0x%" PRIx64 "new-bp:0x%" PRIx64,
                update.entry.mac.to_string().c_str(), entry->bv_id, existing_entry->second.bridge_port_id, bridge_port_id);
             break;
        }

        update.add = true;
        update.type = "dynamic";
        update.port.m_fdb_count++;
        m_portsOrch->setPort(update.port.m_alias, update.port);
        vlan.m_fdb_count++;
        m_portsOrch->setPort(vlan.m_alias, vlan);

        storeFdbEntryState(update);
        notify(SUBJECT_TYPE_FDB_CHANGE, &update);

        break;
    }
    case SAI_FDB_EVENT_AGED:
    {
        SWSS_LOG_INFO("Received AGE event for bvid=%lx mac=%s port=%lx", entry->bv_id, update.entry.mac.to_string().c_str(), bridge_port_id);

        if (!m_portsOrch->getPort(entry->bv_id, vlan))
        {
            SWSS_LOG_NOTICE("FdbOrch AGE notification: Failed to locate vlan port from bv_id 0x%lx", entry->bv_id);
        }

        auto existing_entry = m_entries.find(update.entry);
        // we don't have such entries
        if (existing_entry == m_entries.end())
        {
             SWSS_LOG_INFO("FdbOrch AGE notification: mac %s is not present in bv_id 0x%lx bp 0x%lx",
                    update.entry.mac.to_string().c_str(), entry->bv_id, bridge_port_id);
             break;
        }

        if (!m_portsOrch->getPortByBridgePortId(bridge_port_id, update.port))
        {
            SWSS_LOG_NOTICE("FdbOrch AGE notification: Failed to get port by bridge port ID 0x%lx", bridge_port_id);
        }

        if (existing_entry->second.bridge_port_id != bridge_port_id)
        {
            SWSS_LOG_NOTICE("FdbOrch AGE notification: Stale aging event received for mac-bv_id %s-0x%lx with bp=0x%lx existing bp=0x%lx", update.entry.mac.to_string().c_str(), entry->bv_id, bridge_port_id, existing_entry->second.bridge_port_id);
            // We need to get the port for bridge-port in existing fdb
            if (!m_portsOrch->getPortByBridgePortId(existing_entry->second.bridge_port_id, update.port))
            {
                SWSS_LOG_NOTICE("FdbOrch AGE notification: Failed to get port by bridge port ID 0x%lx", existing_entry->second.bridge_port_id);
            }
            // dont return, let it delete just to bring SONiC and SAI in sync
            // return;
        }

        if (existing_entry->second.type == "static")
        {
            update.type = "static";

            if (vlan.m_members.find(update.port.m_alias) == vlan.m_members.end())
            {
        	    saved_fdb_entries[update.port.m_alias].push_back({existing_entry->first.mac,
                        vlan.m_vlan_info.vlan_id, update.type,
                        existing_entry->second.origin,
                        existing_entry->second.remote_ip,
                        existing_entry->second.esi,
                        existing_entry->second.vni});
            }
            else
            {
                /*port added back to vlan before we receive delete
                  notification for flush from SAI. Re-add entry to SAI
                 */ 
                sai_attribute_t attr;
                vector<sai_attribute_t> attrs;

                attr.id = SAI_FDB_ENTRY_ATTR_TYPE;
                attr.value.s32 = SAI_FDB_ENTRY_TYPE_STATIC;
                attrs.push_back(attr);
                attr.id = SAI_FDB_ENTRY_ATTR_BRIDGE_PORT_ID;
                attr.value.oid = bridge_port_id;
                attrs.push_back(attr);
                auto status = sai_fdb_api->create_fdb_entry(entry, (uint32_t)attrs.size(), attrs.data());
                if (status != SAI_STATUS_SUCCESS)
                {
                    SWSS_LOG_ERROR("Failed to create FDB %s on %s, rv:%d",
                        existing_entry->first.mac.to_string().c_str(), update.port.m_alias.c_str(), status);
                    return;
                }
                return;
            }
        }

        update.add = false;
        if (!update.port.m_alias.empty())
        { 
            update.port.m_fdb_count--;
            m_portsOrch->setPort(update.port.m_alias, update.port);
        }
        if (!vlan.m_alias.empty())
        {
            vlan.m_fdb_count--;
            m_portsOrch->setPort(vlan.m_alias, vlan);
        }

        storeFdbEntryState(update);

        notify(SUBJECT_TYPE_FDB_CHANGE, &update);

        notifyTunnelOrch(update.port);
        break;
    }
    case SAI_FDB_EVENT_MOVE:
    {
        Port port_old;
        auto existing_entry = m_entries.find(update.entry);

        SWSS_LOG_INFO("Received MOVE event for bvid=%lx mac=%s port=%lx", entry->bv_id, update.entry.mac.to_string().c_str(), bridge_port_id);

        if (!m_portsOrch->getPort(entry->bv_id, vlan))
        {
            SWSS_LOG_ERROR("FdbOrch MOVE notification: Failed to locate vlan port from bv_id 0x%lx", entry->bv_id);
            return;
        }

        if (!m_portsOrch->getPortByBridgePortId(bridge_port_id, update.port))
        {
            SWSS_LOG_ERROR("FdbOrch MOVE notification: Failed to get port by bridge port ID 0x%lx", bridge_port_id);
            return;
        }

        // We should already have such entry
        if (existing_entry == m_entries.end())
        {
             SWSS_LOG_WARN("FdbOrch MOVE notification: mac %s is not found in bv_id 0x%lx",
                    update.entry.mac.to_string().c_str(), entry->bv_id);
        }
        else if (!m_portsOrch->getPortByBridgePortId(existing_entry->second.bridge_port_id, port_old))
        {
            SWSS_LOG_ERROR("FdbOrch MOVE notification: Failed to get port by bridge port ID 0x%lx", existing_entry->second.bridge_port_id);
            return;
        }

        update.add = true;

        if (!port_old.m_alias.empty())
        {
            port_old.m_fdb_count--;
            m_portsOrch->setPort(port_old.m_alias, port_old);
        }
        update.port.m_fdb_count++;
        m_portsOrch->setPort(update.port.m_alias, update.port);

        storeFdbEntryState(update);

        notify(SUBJECT_TYPE_FDB_CHANGE, &update);

        notifyTunnelOrch(port_old);

        break;
    }
    case SAI_FDB_EVENT_FLUSHED:
        SWSS_LOG_INFO("Received FLUSH event for bvid=%lx mac=%s port=%lx", entry->bv_id, update.entry.mac.to_string().c_str(), bridge_port_id);
        for (auto itr = m_entries.begin(); itr != m_entries.end();)
        {
            if ((itr->second.type == "static") || (itr->second.origin == FDB_ORIGIN_VXLAN_ADVERTIZED))
            {
                itr++;
                continue;
            }

            if (((bridge_port_id == SAI_NULL_OBJECT_ID) && (entry->bv_id == SAI_NULL_OBJECT_ID)) // Flush all DYNAMIC
                || ((bridge_port_id == itr->second.bridge_port_id) && (entry->bv_id == SAI_NULL_OBJECT_ID)) // flush all DYN on a port
                || ((bridge_port_id == SAI_NULL_OBJECT_ID) && (entry->bv_id == itr->first.bv_id))) // flush all DYN on a vlan
            {

                if (!m_portsOrch->getPortByBridgePortId(itr->second.bridge_port_id, update.port))
                {
                    SWSS_LOG_ERROR("FdbOrch FLUSH notification: Failed to get port by bridge port ID 0x%lx", itr->second.bridge_port_id);
                }

                if (!m_portsOrch->getPort(itr->first.bv_id, vlan))
                {
                    SWSS_LOG_NOTICE("FdbOrch FLUSH notification: Failed to locate vlan port from bv_id 0x%lx", itr->first.bv_id);
                }

                update.entry.mac = itr->first.mac;
                update.entry.bv_id = itr->first.bv_id;
                update.add = false;
                itr++;

                if (!update.port.m_alias.empty())
                {
                    update.port.m_fdb_count--;
                    m_portsOrch->setPort(update.port.m_alias, update.port);
                }
                if (!vlan.m_alias.empty())
                {
                    vlan.m_fdb_count--;
                    m_portsOrch->setPort(vlan.m_alias, vlan);
                }

                /* This will invalidate the current iterator hence itr++ is done before */
                storeFdbEntryState(update);

                SWSS_LOG_DEBUG("FdbOrch FLUSH notification: mac %s was removed", update.entry.mac.to_string().c_str());

                notify(SUBJECT_TYPE_FDB_CHANGE, &update);
                notifyTunnelOrch(update.port);
            }
            else
            {
                itr++;
            }
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
    sai_object_id_t bridge_port_id;
    SWSS_LOG_ENTER();

    if (!m_portsOrch->getVlanByVlanId(vlan, port))
    {
        SWSS_LOG_ERROR("Failed to get vlan by vlan ID %d", vlan);
        return false;
    }

    FdbEntry entry;
    entry.bv_id = port.m_vlan_info.vlan_oid;
    entry.mac = mac;
    auto it= m_entries.find(entry);
    if(it != m_entries.end())
    {
        bridge_port_id = it->second.bridge_port_id;
    }
    else
    {
        return false;
    }

    if (!m_portsOrch->getPortByBridgePortId(bridge_port_id, port))
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

    FdbOrigin origin = FDB_ORIGIN_PROVISIONED;

    string table_name = consumer.getTableName();
    if(table_name == APP_VXLAN_FDB_TABLE_NAME)
    {
        origin = FDB_ORIGIN_VXLAN_ADVERTIZED;
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
            if(op == DEL_COMMAND)
            {
                /* Delete if it is in saved_fdb_entry */
                unsigned short vlan_id;
                try {
                    vlan_id = (unsigned short) stoi(keys[0].substr(4));
                } catch(exception &e) {
                    it = consumer.m_toSync.erase(it);
                    continue;
                }
                deleteFdbEntryFromSavedFDB(MacAddress(keys[1]), vlan_id, origin);

                it = consumer.m_toSync.erase(it);
            }
            else
            {
                it++;
            }
            continue;
        }

        FdbEntry entry;
        entry.mac = MacAddress(keys[1]);
        entry.bv_id = vlan.m_vlan_info.vlan_oid;

        if (op == SET_COMMAND)
        {
            string port = "";
            string type = "dynamic";
            string remote_ip = "";
            string esi = "";
            unsigned int vni = 0;
            string sticky = "";

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

                if(origin == FDB_ORIGIN_VXLAN_ADVERTIZED)
                {
                    if (fvField(i) == "remote_vtep")
                    {
                        remote_ip = fvValue(i);
                        // Creating an IpAddress object to validate if remote_ip is valid
                        // if invalid it will throw the exception and we will ignore the
                        // event
                        try {
                            IpAddress valid_ip = IpAddress(remote_ip);
                            (void)valid_ip; // To avoid g++ warning
                        } catch(exception &e) {
                            SWSS_LOG_NOTICE("Invalid IP address in remote MAC %s", remote_ip.c_str());
                            remote_ip = "";
                            break;
                        }
                    }

                    if (fvField(i) == "esi")
                    {
                        esi = fvValue(i);
                    }

                    if (fvField(i) == "vni")
                    {
                        try {
                            vni = (unsigned int) stoi(fvValue(i));
                        } catch(exception &e) {
                            SWSS_LOG_INFO("Invalid VNI in remote MAC %s", fvValue(i).c_str());
                            vni = 0;
                            break;
                        }
                    }
                }
            }

            /* FDB type is either dynamic or static */
            assert(type == "dynamic" || type == "static");

            if(origin == FDB_ORIGIN_VXLAN_ADVERTIZED)
            {
                VxlanTunnelOrch* tunnel_orch = gDirectory.get<VxlanTunnelOrch*>();

                if(!remote_ip.length())
                {
                    it = consumer.m_toSync.erase(it);
                    continue;
                }
                port = tunnel_orch->getTunnelPortName(remote_ip);
            }


            if (addFdbEntry(entry, port, type, origin, remote_ip, vni, esi))
                it = consumer.m_toSync.erase(it);
            else
                it++;
        }
        else if (op == DEL_COMMAND)
        {
            if (removeFdbEntry(entry, origin))
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

    std::string op;
    std::string data;
    std::vector<swss::FieldValueTuple> values;

    consumer.pop(op, data, values);

    if (&consumer == m_flushNotificationsConsumer)
    {
        if (op == "ALL")
        {
            flushFdbAll(0);
            return;
        }
        else if (op == "PORT")
        {
            flushFdbByPort(data, 0);
            return;
        }
        else if (op == "VLAN")
        {
            flushFdbByVlan(data, 0);
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

void FdbOrch::updatePortOperState(const PortOperStateUpdate& update)
{
    SWSS_LOG_ENTER();
    if (update.operStatus == SAI_PORT_OPER_STATUS_DOWN)
    {
        swss::Port p = update.port;
    	flushFdbByPort(p.m_alias, 0);
    }
    return;
}

void FdbOrch::updateVlanMember(const VlanMemberUpdate& update)
{
    Port port;

    string port_name = update.member.m_alias;
    string vlan_name = update.vlan.m_alias;

    SWSS_LOG_ENTER();

    if (!m_portsOrch->getPort(port_name, port))
    {
        SWSS_LOG_ERROR("could not locate port from alias %s", port_name.c_str());
        return;
    }

    if (!update.add)
    {
        if(port.m_type != Port::TUNNEL)
        {
            flushFdbByPortVlan(port_name, vlan_name, 1);
        }
        return;
    }

    auto fdb_list = std::move(saved_fdb_entries[port_name]);
    if(!fdb_list.empty())
    {
        for (const auto& fdb: fdb_list)
        {
            // try to insert an FDB entry. If the FDB entry is not ready to be inserted yet,
            // it would be added back to the saved_fdb_entries structure by addFDBEntry()
            if(fdb.vlanId == update.vlan.m_vlan_info.vlan_id)
            {
                FdbEntry entry;
                entry.mac = fdb.mac;
                entry.bv_id = update.vlan.m_vlan_info.vlan_oid;
                (void)addFdbEntry(entry, port_name, fdb.type, fdb.origin,
                        fdb.remote_ip, fdb.vni, fdb.esi);
            }
            else
            {
                saved_fdb_entries[port_name].push_back(fdb);
            }
        }
    }
}

bool FdbOrch::addFdbEntry(const FdbEntry& entry, const string& port_name, const string& type, FdbOrigin origin, const string& remote_ip, unsigned int vni, const string& esi)
{
    Port vlan;
    Port port;

    SWSS_LOG_ENTER();
    SWSS_LOG_NOTICE("mac=%s bv_id=0x%lx port_name=%s type=%s origin=%d", entry.mac.to_string().c_str(), entry.bv_id, port_name.c_str(), type.c_str(), origin);

    if (!m_portsOrch->getPort(entry.bv_id, vlan))
    {
        SWSS_LOG_NOTICE("addFdbEntry: Failed to locate vlan port from bv_id 0x%lx", entry.bv_id);
        return false;
    }

    /* Retry until port is created */
    if (!m_portsOrch->getPort(port_name, port) || (port.m_bridge_port_id == SAI_NULL_OBJECT_ID))
    {
        SWSS_LOG_INFO("Saving a fdb entry until port %s becomes active", port_name.c_str());
        saved_fdb_entries[port_name].push_back({entry.mac,
                vlan.m_vlan_info.vlan_id, type, origin, remote_ip, esi, vni});
        return true;
    }

    /* Retry until port is member of vlan*/
    if (vlan.m_members.find(port_name) == vlan.m_members.end())
    {
        SWSS_LOG_INFO("Saving a fdb entry until port %s becomes vlan %s member", port_name.c_str(), vlan.m_alias.c_str());
        saved_fdb_entries[port_name].push_back({entry.mac,
                vlan.m_vlan_info.vlan_id, type, origin, remote_ip, esi, vni});
        return true;
    }

    sai_status_t status;
    sai_fdb_entry_t fdb_entry;
    fdb_entry.switch_id = gSwitchId;
    memcpy(fdb_entry.mac_address, entry.mac.getMac(), sizeof(sai_mac_t));
    fdb_entry.bv_id = entry.bv_id;

    Port oldPort;
    string oldType;
    FdbOrigin oldOrigin = FDB_ORIGIN_INVALID ;
    bool macUpdate = false;
    auto it = m_entries.find(entry);
    if(it != m_entries.end())
    {
        /* get existing port and type */
        oldType = it->second.type;
        oldOrigin = it->second.origin;

        if (!m_portsOrch->getPortByBridgePortId(it->second.bridge_port_id, oldPort))
        {
            SWSS_LOG_ERROR("Existing port 0x%lx details not found", it->second.bridge_port_id);
            return false;
        }

        if((oldOrigin == origin) && (oldType == type) && (port.m_bridge_port_id == it->second.bridge_port_id))
        {
            /* Duplicate Mac */
            SWSS_LOG_NOTICE("FdbOrch: mac=%s %s port=%s type=%s origin=%d is duplicate", entry.mac.to_string().c_str(), vlan.m_alias.c_str(), port_name.c_str(), type.c_str(), origin);
            return true;
        }
        else if(origin != oldOrigin)
        {
            /* Mac origin has changed */
            if((oldType == "static") && (oldOrigin == FDB_ORIGIN_PROVISIONED))
            {
                /* old mac was static and provisioned, it can not be changed by Remote Mac */
                SWSS_LOG_NOTICE("Already existing static MAC:%s in Vlan:%d. "
                        "Received same MAC from peer:%s; "
                        "Peer mac ignored",
                        entry.mac.to_string().c_str(), vlan.m_vlan_info.vlan_id,
                        remote_ip.c_str());

                return true;
            }
            else if((oldType == "static") && (oldOrigin == FDB_ORIGIN_VXLAN_ADVERTIZED) && (type == "dynamic"))
            {
                /* old mac was static and received from remote, it can not be changed by dynamic locally provisioned Mac */
                SWSS_LOG_NOTICE("Already existing static MAC:%s in Vlan:%d "
                        "from Peer:%s. Now same is provisioned as dynamic; "
                        "Provisioned dynamic mac is ignored",
                        entry.mac.to_string().c_str(), vlan.m_vlan_info.vlan_id,
                        it->second.remote_ip.c_str());
                return true;
            }
            else if(oldOrigin == FDB_ORIGIN_VXLAN_ADVERTIZED)
            {
                if((oldType == "static") && (type == "static"))
                {
                    SWSS_LOG_WARN("You have just overwritten existing static MAC:%s "
                            "in Vlan:%d from Peer:%s, "
                            "If it is a mistake, it will result in inconsistent Traffic Forwarding",
                            entry.mac.to_string().c_str(),
                            vlan.m_vlan_info.vlan_id,
                            it->second.remote_ip.c_str());
                }
            }
        }
        else /* (origin == oldOrigin) */
        {
            /* Mac origin is same, all changes are allowed */
            /* Allowed
             * Bridge-port is changed or/and
             * Sticky bit from remote is modified or
             * provisioned mac is converted from static<-->dynamic
             */
        }

        macUpdate = true;
    }

    sai_attribute_t attr;
    vector<sai_attribute_t> attrs;

    attr.id = SAI_FDB_ENTRY_ATTR_TYPE;
    if (origin == FDB_ORIGIN_VXLAN_ADVERTIZED)
    {
        attr.value.s32 = (type == "dynamic") ? SAI_FDB_ENTRY_TYPE_STATIC_MACMOVE : SAI_FDB_ENTRY_TYPE_STATIC;
    }
    else
    {
        attr.value.s32 = (type == "dynamic") ? SAI_FDB_ENTRY_TYPE_DYNAMIC : SAI_FDB_ENTRY_TYPE_STATIC;
    }
    attrs.push_back(attr);

    attr.id = SAI_FDB_ENTRY_ATTR_BRIDGE_PORT_ID;
    attr.value.oid = port.m_bridge_port_id;
    attrs.push_back(attr);

    if(origin == FDB_ORIGIN_VXLAN_ADVERTIZED)
    {
        IpAddress remote = IpAddress(remote_ip);
        sai_ip_address_t ipaddr;
        if(remote.isV4())
        {
            ipaddr.addr_family = SAI_IP_ADDR_FAMILY_IPV4;
            ipaddr.addr.ip4 = remote.getV4Addr();
        }
        else
        {
            ipaddr.addr_family = SAI_IP_ADDR_FAMILY_IPV6;
            memcpy(ipaddr.addr.ip6, remote.getV6Addr(), sizeof(ipaddr.addr.ip6));
        }
        attr.id = SAI_FDB_ENTRY_ATTR_ENDPOINT_IP;
        attr.value.ipaddr = ipaddr;
        attrs.push_back(attr);
    }
    else if(macUpdate && (oldOrigin == FDB_ORIGIN_VXLAN_ADVERTIZED) && (origin != oldOrigin))
    {
        /* origin is changed from Remote-advertized to Local-provisioned
         * Remove the end-point ip attribute from fdb entry
         */
        sai_ip_address_t ipaddr;
        ipaddr.addr_family = SAI_IP_ADDR_FAMILY_IPV4;
        ipaddr.addr.ip4 = 0;
        attr.id = SAI_FDB_ENTRY_ATTR_ENDPOINT_IP;
        attr.value.ipaddr = ipaddr;
        attrs.push_back(attr);
    }

    string key = "Vlan" + to_string(vlan.m_vlan_info.vlan_id) + ":" + entry.mac.to_string();

    
    if(macUpdate)
    {
        SWSS_LOG_NOTICE("MAC-Update FDB %s in %s on from-%s:to-%s from-%s:to-%s origin-%d-to-%d",
                entry.mac.to_string().c_str(), vlan.m_alias.c_str(), oldPort.m_alias.c_str(),
                port_name.c_str(), oldType.c_str(), type.c_str(), oldOrigin, origin);
        for(auto itr : attrs)
        {
            status = sai_fdb_api->set_fdb_entry_attribute(&fdb_entry, &itr);
            if (status != SAI_STATUS_SUCCESS)
            {
                SWSS_LOG_ERROR("macUpdate-Failed for attr.id=0x%x for FDB %s in %s on %s, rv:%d",
                            itr.id, entry.mac.to_string().c_str(), vlan.m_alias.c_str(), port_name.c_str(), status);
                return false;
            }
        }
        if (oldPort.m_bridge_port_id != port.m_bridge_port_id)
        {
            oldPort.m_fdb_count--;
            m_portsOrch->setPort(oldPort.m_alias, oldPort);
            port.m_fdb_count++;
            m_portsOrch->setPort(port.m_alias, port);
        }
    }
    else
    {
        SWSS_LOG_NOTICE("MAC-Create %s FDB %s in %s on %s", type.c_str(), entry.mac.to_string().c_str(), vlan.m_alias.c_str(), port_name.c_str());

        status = sai_fdb_api->create_fdb_entry(&fdb_entry, (uint32_t)attrs.size(), attrs.data());
        if (status != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_ERROR("Failed to create %s FDB %s in %s on %s, rv:%d",
                    type.c_str(), entry.mac.to_string().c_str(),
                    vlan.m_alias.c_str(), port_name.c_str(), status);
            return false; //FIXME: it should be based on status. Some could be retried, some not
        }
        port.m_fdb_count++;
        m_portsOrch->setPort(port.m_alias, port);
        vlan.m_fdb_count++;
        m_portsOrch->setPort(vlan.m_alias, vlan);
    }

    FdbData fdbData;
    fdbData.bridge_port_id = port.m_bridge_port_id;
    fdbData.type = type;
    fdbData.origin = origin;
    fdbData.remote_ip = remote_ip;
    fdbData.esi = esi;
    fdbData.vni = vni;

    m_entries[entry] = fdbData;

    string key = "Vlan" + to_string(vlan.m_vlan_info.vlan_id) + ":" + entry.mac.to_string();

    if (origin != FDB_ORIGIN_VXLAN_ADVERTIZED)
    {
        /* State-DB is updated only for Local Mac addresses */
        // Write to StateDb
        std::vector<FieldValueTuple> fvs;
        fvs.push_back(FieldValueTuple("port", port_name));
        if (type == "dynamic_local")
            fvs.push_back(FieldValueTuple("type", "dynamic"));
        else
            fvs.push_back(FieldValueTuple("type", type));
        m_fdbStateTable.set(key, fvs);
    }
    else if (macUpdate && (oldOrigin != FDB_ORIGIN_VXLAN_ADVERTIZED))
    {
        /* origin is FDB_ORIGIN_ADVERTIZED and it is mac-update
         * so delete from StateDb since we only keep local fdbs
         * in state-db
         */
        m_fdbStateTable.del(key);
    }

    if(!macUpdate)
    {
        gCrmOrch->incCrmResUsedCounter(CrmResourceType::CRM_FDB_ENTRY);
    }

    FdbUpdate update;
    update.entry = entry;
    update.port = port;
    update.type = type;
    update.add = true;

    notify(SUBJECT_TYPE_FDB_CHANGE, &update);

    return true;
}

bool FdbOrch::removeFdbEntry(const FdbEntry& entry, FdbOrigin origin)
{
    Port vlan;
    Port port;

    SWSS_LOG_ENTER();

    SWSS_LOG_NOTICE("FdbOrch RemoveFDBEntry: mac=%s bv_id=0x%lx origin %d", entry.mac.to_string().c_str(), entry.bv_id, origin);

    if (!m_portsOrch->getPort(entry.bv_id, vlan))
    {
        SWSS_LOG_NOTICE("FdbOrch notification: Failed to locate vlan port from bv_id 0x%lx", entry.bv_id);
        return false;
    }

    auto it= m_entries.find(entry);
    if(it == m_entries.end())
    {
        SWSS_LOG_INFO("FdbOrch RemoveFDBEntry: FDB entry isn't found. mac=%s bv_id=0x%lx", entry.mac.to_string().c_str(), entry.bv_id);

        /* check whether the entry is in the saved fdb, if so delete it from there. */
        deleteFdbEntryFromSavedFDB(entry.mac, vlan.m_vlan_info.vlan_id, origin);
        return true;
    }

    FdbData fdbData = it->second;
    if (!m_portsOrch->getPortByBridgePortId(fdbData.bridge_port_id, port))
    {
        SWSS_LOG_NOTICE("FdbOrch RemoveFDBEntry: Failed to locate port from bridge_port_id 0x%lx", fdbData.bridge_port_id);
        return false;
    }

    if(fdbData.origin != origin)
    {
        /* When mac is moved from remote to local
         * BGP will delete the mac from vxlan_fdb_table
         * but we should not delete this mac here since now
         * mac in orchagent represents locally learnt
         */
        SWSS_LOG_NOTICE("FdbOrch RemoveFDBEntry: mac=%s fdb origin is different; found_origin:%d delete_origin:%d",
                entry.mac.to_string().c_str(), fdbData.origin, origin);

        /* We may still have the mac in saved-fdb probably due to unavailability
         * of bridge-port. check whether the entry is in the saved fdb,
         * if so delete it from there. */
        deleteFdbEntryFromSavedFDB(entry.mac, vlan.m_vlan_info.vlan_id, origin);

        return true;
    }

    string key = "Vlan" + to_string(vlan.m_vlan_info.vlan_id) + ":" + entry.mac.to_string();

    sai_status_t status;
    sai_fdb_entry_t fdb_entry;
    fdb_entry.switch_id = gSwitchId;
    memcpy(fdb_entry.mac_address, entry.mac.getMac(), sizeof(sai_mac_t));
    fdb_entry.bv_id = entry.bv_id;

    status = sai_fdb_api->remove_fdb_entry(&fdb_entry);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("FdbOrch RemoveFDBEntry: Failed to remove FDB entry. mac=%s, bv_id=0x%lx",
                       entry.mac.to_string().c_str(), entry.bv_id);
        return true; //FIXME: it should be based on status. Some could be retried. some not
    }

    SWSS_LOG_NOTICE("Removed mac=%s bv_id=0x%lx port:%s",
            entry.mac.to_string().c_str(), entry.bv_id, port.m_alias.c_str());

    port.m_fdb_count--;
    m_portsOrch->setPort(port.m_alias, port);
    vlan.m_fdb_count--;
    m_portsOrch->setPort(vlan.m_alias, vlan);
    (void)m_entries.erase(entry);

    // Remove in StateDb
    if (fdbData.origin != FDB_ORIGIN_VXLAN_ADVERTIZED) 
    {
        m_fdbStateTable.del(key);
    }

    gCrmOrch->decCrmResUsedCounter(CrmResourceType::CRM_FDB_ENTRY);

    FdbUpdate update;
    update.entry = entry;
    update.port = port;
    update.type = fdbData.type;
    update.add = false;

    notify(SUBJECT_TYPE_FDB_CHANGE, &update);

    notifyTunnelOrch(update.port);

    return true;
}

bool FdbOrch::flushFdbAll(bool flush_static)
{
    sai_status_t status;
    sai_attribute_t port_attr;

    if (!flush_static)
    {
        port_attr.id = SAI_FDB_FLUSH_ATTR_ENTRY_TYPE;
        port_attr.value.s32 = SAI_FDB_FLUSH_ENTRY_TYPE_DYNAMIC;
        status = sai_fdb_api->flush_fdb_entries(gSwitchId, 1, &port_attr);
    }
    else
    {
        status = sai_fdb_api->flush_fdb_entries(gSwitchId, 0, NULL);
    }
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Flush fdb failed, return code %x", status);
        return false;
    }
    return true;
}

bool FdbOrch::flushFdbByPort(const string &alias, bool flush_static)
{
    sai_status_t status;
    Port port;
    sai_attribute_t port_attr[2];

    if (!m_portsOrch->getPort(alias, port))
    {
        SWSS_LOG_ERROR("could not locate port from alias %s", alias.c_str());
        return false;
    }

    if ((port.m_bridge_port_id == SAI_NULL_OBJECT_ID) || !port.m_fdb_count)
    {
        /* port is not an L2 port or no macs to flush */
        return true;
    }

    SWSS_LOG_NOTICE("m_bridge_port_id 0x%lx flush_static %d m_fdb_count %u", port.m_bridge_port_id, flush_static, port.m_fdb_count);

    port_attr[0].id = SAI_FDB_FLUSH_ATTR_BRIDGE_PORT_ID;
    port_attr[0].value.oid = port.m_bridge_port_id;
    if (!flush_static)
    {
        port_attr[1].id = SAI_FDB_FLUSH_ATTR_ENTRY_TYPE;
        port_attr[1].value.s32 = SAI_FDB_FLUSH_ENTRY_TYPE_DYNAMIC;
        status = sai_fdb_api->flush_fdb_entries(gSwitchId, 2, port_attr);
    }
    else
    {
    	status = sai_fdb_api->flush_fdb_entries(gSwitchId, 1, port_attr);
    }
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Flush fdb failed, return code %x", status);
        return false;
    }
    return true;
}

bool FdbOrch::flushFdbByVlan(const string &alias, bool flush_static)
{
    sai_status_t status;
    Port vlan;
    sai_attribute_t vlan_attr[2];

    if (!m_portsOrch->getPort(alias, vlan))
    {
        SWSS_LOG_ERROR("could not locate vlan from alias %s", alias.c_str());
        return false;
    }
    SWSS_LOG_NOTICE("vlan_oid 0x%lx flush_static %d", vlan.m_vlan_info.vlan_oid, flush_static);

    vlan_attr[0].id = SAI_FDB_FLUSH_ATTR_BV_ID;
    vlan_attr[0].value.oid = vlan.m_vlan_info.vlan_oid;
    if (!flush_static)
    {
        vlan_attr[1].id = SAI_FDB_FLUSH_ATTR_ENTRY_TYPE;
        vlan_attr[1].value.s32 = SAI_FDB_FLUSH_ENTRY_TYPE_DYNAMIC;
        status = sai_fdb_api->flush_fdb_entries(gSwitchId, 2, vlan_attr);
    }
    else
    {
    	status = sai_fdb_api->flush_fdb_entries(gSwitchId, 1, vlan_attr);
    }
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Flush fdb failed, return code %x", status);
        return false;
    }

    return true;
}

bool FdbOrch::flushFdbByPortVlan(const string &port_alias, const string &vlan_alias, bool flush_static)
{

    sai_status_t status;
    Port vlan;
    Port port;
    sai_attribute_t port_vlan_attr[3];

    SWSS_LOG_NOTICE("port %s vlan %s", port_alias.c_str(), vlan_alias.c_str());

    if (!m_portsOrch->getPort(port_alias, port))
    {
        SWSS_LOG_ERROR("could not locate port from alias %s", port_alias.c_str());
        return false;
    }
    if (!m_portsOrch->getPort(vlan_alias, vlan))
    {
        SWSS_LOG_NOTICE("FdbOrch notification: Failed to locate vlan %s", vlan_alias.c_str());
        return false;
    }

    if ((port.m_bridge_port_id == SAI_NULL_OBJECT_ID) || !port.m_fdb_count)
    {
        /* port is not an L2 port or no macs to flush */
        return true;
    }

    SWSS_LOG_NOTICE("vlan_oid 0x%lx m_bridge_port_id 0x%lx flush_static %d m_fdb_count %u", vlan.m_vlan_info.vlan_oid, port.m_bridge_port_id, flush_static, port.m_fdb_count);

    port_vlan_attr[0].id = SAI_FDB_FLUSH_ATTR_BV_ID;
    port_vlan_attr[0].value.oid = vlan.m_vlan_info.vlan_oid;
    port_vlan_attr[1].id = SAI_FDB_FLUSH_ATTR_BRIDGE_PORT_ID;
    port_vlan_attr[1].value.oid = port.m_bridge_port_id;
    if (!flush_static)
    {
        port_vlan_attr[2].id = SAI_FDB_FLUSH_ATTR_ENTRY_TYPE;
        port_vlan_attr[2].value.s32 = SAI_FDB_FLUSH_ENTRY_TYPE_DYNAMIC;
        status = sai_fdb_api->flush_fdb_entries(gSwitchId, 3, port_vlan_attr);
    }
    else
    {
    	status = sai_fdb_api->flush_fdb_entries(gSwitchId, 2, port_vlan_attr);
    }
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Flush fdb failed, return code %x", status);
        return false;
    }

    return true;
}

void FdbOrch::deleteFdbEntryFromSavedFDB(const MacAddress &mac, 
        const unsigned short &vlanId, FdbOrigin origin, const string portName)
{
    bool found=false;
    SavedFdbEntry entry;
    entry.mac = mac;
    entry.vlanId = vlanId;
    entry.type = "static";
    /* Below members are unused during delete compare */
    entry.origin = origin;

    for (auto& itr: saved_fdb_entries)
    {
        if(portName.empty() || (portName == itr.first))
        {
            auto iter = saved_fdb_entries[itr.first].begin();
            while(iter != saved_fdb_entries[itr.first].end())
            {
                if (*iter == entry)
                {
                    if(iter->origin == origin)
                    {
                        SWSS_LOG_NOTICE("FDB entry found in saved fdb. deleting..."
                                "mac=%s vlan_id=0x%x origin:%d port:%s", 
                                mac.to_string().c_str(), vlanId, origin,
                                itr.first.c_str());
                        saved_fdb_entries[itr.first].erase(iter);

                        found=true;
                        break;
                    }
                    else
                    {
                        SWSS_LOG_NOTICE("FDB entry found in saved fdb, but Origin is "
                                "different mac=%s vlan_id=0x%x reqOrigin:%d "
                                "foundOrigin:%d port:%s, IGNORED", 
                                mac.to_string().c_str(), vlanId, origin,
                                iter->origin, itr.first.c_str());
                    }
                }
                iter++;
            }
        }
        if(found)
            break;
    }
}

// Notify Tunnel Orch when the number of MAC entries
void FdbOrch::notifyTunnelOrch(Port& port)
{
    VxlanTunnelOrch* tunnel_orch = gDirectory.get<VxlanTunnelOrch*>();

    if((port.m_type != Port::TUNNEL) ||
       (port.m_fdb_count != 0))
      return;

    tunnel_orch->deleteTunnelPort(port);
}

