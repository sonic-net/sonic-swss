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
#include "mlagorch.h"
#include "vxlanorch.h"
#include "directory.h"
#include "neighorch.h"

extern sai_fdb_api_t    *sai_fdb_api;

extern sai_object_id_t  gSwitchId;
extern CrmOrch *        gCrmOrch;
extern MlagOrch*        gMlagOrch;
extern Directory<Orch*> gDirectory;
extern NeighOrch*       gNeighOrch;

const int FdbOrch::fdborch_pri = 20;

FdbOrch::FdbOrch(DBConnector* applDbConnector, vector<table_name_with_pri_t> appFdbTables,
    TableConnector stateDbFdbConnector, TableConnector stateDbMclagFdbConnector, PortsOrch *port) :
    Orch(applDbConnector, appFdbTables),
    m_portsOrch(port),
    m_fdbStateTable(stateDbFdbConnector.first, stateDbFdbConnector.second),
    m_mclagFdbStateTable(stateDbMclagFdbConnector.first, stateDbMclagFdbConnector.second)
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
    m_notificationsDb = make_shared<DBConnector>("ASIC_DB", 0);
    m_fdbNotificationConsumer = new swss::NotificationConsumer(m_notificationsDb.get(), "NOTIFICATIONS");
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

    oldFdbData.origin = FDB_ORIGIN_INVALID;
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
                if (it->second.origin != FDB_ORIGIN_MCLAG_ADVERTIZED)
                {
                    SWSS_LOG_INFO("FdbOrch notification: mac %s is duplicate", entry.mac.to_string().c_str());
                    return false;
                }
            }
            mac_move = true;
            oldFdbData = it->second;

            //Remove the existing entry since its port_name may be changed
            (void)m_entries.erase(entry);
        }

        fdbdata.bridge_port_id = update.port.m_bridge_port_id;
        fdbdata.type = update.type;
        fdbdata.sai_fdb_type = update.sai_fdb_type;
        fdbdata.origin = FDB_ORIGIN_LEARN;
        fdbdata.dest_type = IFNAME;
        fdbdata.dest_value = portName;
        fdbdata.esi = "";
        fdbdata.vni = 0;

        m_entries[entry] = fdbdata;
        SWSS_LOG_INFO("FdbOrch notification: mac %s was inserted in port %s into bv_id 0x%" PRIx64,
                        entry.mac.to_string().c_str(), portName.c_str(), entry.bv_id);
        SWSS_LOG_INFO("m_entries size=%zu mac=%s port=0x%" PRIx64,
            m_entries.size(), entry.mac.to_string().c_str(),  m_entries[entry].bridge_port_id);

        if (mac_move && (oldFdbData.origin == FDB_ORIGIN_MCLAG_ADVERTIZED))
        {
            SWSS_LOG_NOTICE("fdbEvent: FdbOrch MCLAG remote to local move delete mac from state MCLAG remote fdb %s table:"
                    "bv_id 0x%" PRIx64, entry.mac.to_string().c_str(), entry.bv_id);

            m_mclagFdbStateTable.del(key);
        }
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

        if (oldFdbData.origin == FDB_ORIGIN_MCLAG_ADVERTIZED)
        {
            SWSS_LOG_NOTICE("fdbEvent: FdbOrch MCLAG remote mac %s deleted, remove from state mclag remote fdb table:"
                            "bv_id 0x%" PRIx64, entry.mac.to_string().c_str(), entry.bv_id);
            m_mclagFdbStateTable.del(key);
        }

        if ((oldFdbData.origin == FDB_ORIGIN_LEARN)  ||
                (oldFdbData.origin == FDB_ORIGIN_PROVISIONED))
        {
            // Remove in StateDb for non advertised mac addresses
            m_fdbStateTable.del(key);
        }

        gCrmOrch->decCrmResUsedCounter(CrmResourceType::CRM_FDB_ENTRY);
        return true;
    }
}

/*
clears stateDb and decrements corresponding internal fdb counters
*/
void FdbOrch::clearFdbEntry(const FdbEntry& entry, const FdbData& fdbData)
{
    FdbUpdate update;
    update.entry = entry;
    update.add = false;
    Port port;
    bool is_port_valid = false;
    sai_object_id_t bridge_port_id = fdbData.bridge_port_id;

    /* Fetch Vlan and decrement the counter */
    Port temp_vlan;
    if (m_portsOrch->getPort(entry.bv_id, temp_vlan))
    {
        m_portsOrch->decrFdbCount(temp_vlan.m_alias, 1);
    }

    if (m_portsOrch->getPort(entry.bv_id, temp_vlan))
    {
        SWSS_LOG_DEBUG("In clearFdbEntry, vlan %s, m_fdb_count %d", temp_vlan.m_alias.c_str(), temp_vlan.m_fdb_count);
    }

    /* Remove the FdbEntry from the internal cache, update state DB and CRM counter */
    storeFdbEntryState(update);

    if (m_portsOrch->getPortByBridgePortId(bridge_port_id, port))
    {
        is_port_valid = true;
    }
    else if (m_portsOrch->getPort(update.entry.port_name, port))
    {
        /* bridge port may be deleted,  try to get port by port_name */
        is_port_valid = true;
    }

    if (is_port_valid)
    {
        /* Decrement port fdb_counter */
        port.m_fdb_count--;
        m_portsOrch->setPort(port.m_alias, port);

        /* Must use the latest "port" since it's a copy, or else may fail to notify */
        SWSS_LOG_DEBUG("Try to notify tunnel port %s", port.m_alias.c_str());
        notifyTunnelOrch(port);
    }

    notify(SUBJECT_TYPE_FDB_CHANGE, &update);
    SWSS_LOG_INFO("FdbEntry removed from internal cache, MAC: %s , port: %s, BVID: 0x%" PRIx64,
                   update.entry.mac.to_string().c_str(), update.entry.port_name.c_str(), update.entry.bv_id);
}

/*
Handles the SAI_FDB_EVENT_FLUSHED notification recieved from syncd
*/
void FdbOrch::handleSyncdFlushNotif(const sai_object_id_t& bv_id,
                                    const sai_object_id_t& bridge_port_id,
                                    const MacAddress& mac,
                                    const sai_fdb_entry_type_t& sai_fdb_type)
{
    // Consolidated flush will have a zero mac
    MacAddress flush_mac("00:00:00:00:00:00");

    if (bridge_port_id == SAI_NULL_OBJECT_ID && bv_id == SAI_NULL_OBJECT_ID)
    {
        for (auto itr = m_entries.begin(); itr != m_entries.end();)
        {
            auto curr = itr++;
            if (curr->second.sai_fdb_type == sai_fdb_type &&
                (curr->first.mac == mac || mac == flush_mac) && curr->second.is_flush_pending)
            {
                clearFdbEntry(curr->first, curr->second);
            }
        }
    }
    else if (bv_id == SAI_NULL_OBJECT_ID)
    {
        /* FLUSH based on PORT */
        for (auto itr = m_entries.begin(); itr != m_entries.end();)
        {
            auto curr = itr++;
            if (curr->second.bridge_port_id == bridge_port_id)
            {
                if (curr->second.sai_fdb_type == sai_fdb_type &&
                    (curr->first.mac == mac || mac == flush_mac) && curr->second.is_flush_pending)
                {
                    clearFdbEntry(curr->first, curr->second);
                }
            }
        }
    }
    else if (bridge_port_id == SAI_NULL_OBJECT_ID)
    {
        /* FLUSH based on BV_ID */
        for (auto itr = m_entries.begin(); itr != m_entries.end();)
        {
            auto curr = itr++;
            if (curr->first.bv_id == bv_id)
            {
                if (curr->second.sai_fdb_type == sai_fdb_type &&
                    (curr->first.mac == mac || mac == flush_mac) && curr->second.is_flush_pending)
                {
                    clearFdbEntry(curr->first, curr->second);
                }
            }
        }
    }
    else
    {
        /* FLUSH based on port and VLAN */
        for (auto itr = m_entries.begin(); itr != m_entries.end();)
        {
            auto curr = itr++;
            if (curr->first.bv_id == bv_id && curr->second.bridge_port_id == bridge_port_id)
            {
                if (curr->second.sai_fdb_type == sai_fdb_type &&
                    (curr->first.mac == mac || mac == flush_mac) && curr->second.is_flush_pending)
                {
                    SWSS_LOG_DEBUG("Try to handle flush for FDB entry %s", mac.to_string().c_str());
                    clearFdbEntry(curr->first, curr->second);
                }
                else if (curr->first.mac == mac)
                {
                    /* Unexpected, leave a warning message for future to improve if we hit this case */
                    SWSS_LOG_WARN("Failed to handle flush for FDB entry %s", mac.to_string().c_str());
               }
            }
        }
    }
}

void FdbOrch::update(sai_fdb_event_t        type,
                     const sai_fdb_entry_t* entry,
                     sai_object_id_t        bridge_port_id,
                     const sai_fdb_entry_type_t   &sai_fdb_type)
{
    SWSS_LOG_ENTER();

    FdbUpdate update;
    update.entry.mac = entry->mac_address;
    update.entry.bv_id = entry->bv_id;
    update.type = "dynamic";
    Port vlan;

    SWSS_LOG_INFO("update: EVPN_MH_UC: FDB event:%d, MAC: %s , BVID: 0x%" PRIx64 " , \
                   bridge port ID: 0x%" PRIx64 ".",
                   type, update.entry.mac.to_string().c_str(),
                   entry->bv_id, bridge_port_id);

    if (bridge_port_id &&
        !m_portsOrch->getPortByBridgePortId(bridge_port_id, update.port))
    {
        if (type == SAI_FDB_EVENT_FLUSHED)
        {
            /* There are notifications about FDB FLUSH (syncd/sai_redis) on port,
               which was already removed by orchagent as a result of removeVlanMember
               action (removeBridgePort). But the internal cleanup of statedb and
               internal counters is yet to be performed, thus continue
            */
            SWSS_LOG_INFO("Flush event: Failed to get port by bridge port ID 0x%" PRIx64 ".",
                        bridge_port_id);
        } else {
            SWSS_LOG_ERROR("Failed to get port by bridge port ID 0x%" PRIx64 ".",
                        bridge_port_id);
            return;
        }
    }

    if (entry->bv_id &&
        !m_portsOrch->getPort(entry->bv_id, vlan))
    {
        SWSS_LOG_NOTICE("FdbOrch notification type %d: Failed to locate vlan port from bv_id 0x%" PRIx64, type, entry->bv_id);
        return;
    }

    switch (type)
    {
    case SAI_FDB_EVENT_LEARNED:
    {
        SWSS_LOG_INFO("Received LEARN event for bvid=0x%" PRIx64 "mac=%s port=0x%" PRIx64, entry->bv_id, update.entry.mac.to_string().c_str(), bridge_port_id);

        Port learn_port;
        /* Drop it if port is down */
        if (m_portsOrch->getPort(update.port.m_alias, learn_port))
        {
            if (learn_port.m_oper_status == SAI_PORT_OPER_STATUS_DOWN)
            {
                SWSS_LOG_NOTICE("update: Port %s is still down for the learnt mac. Flush to remove mac=%s bv_id=0x%" PRIx64,
						update.port.m_alias.c_str(), update.entry.mac.to_string().c_str(), entry->bv_id);

                /* since the interface is down, ignore this LEARN event and trigger a flush to flush all dynamic fdb at SDK and Meta layer */
                flushFDBEntries(learn_port.m_bridge_port_id, SAI_NULL_OBJECT_ID);

                return;
            }
        }

        Port port_old;
        bool mac_move_local = false;
        // we already have such entries
        auto existing_entry = m_entries.find(update.entry);
        if (existing_entry != m_entries.end())
        {
            if (existing_entry->second.origin == FDB_ORIGIN_MCLAG_ADVERTIZED)
            {
                // If the bp is different MOVE the MAC entry.
                if (existing_entry->second.bridge_port_id != bridge_port_id)
                {
                    Port port;
                    SWSS_LOG_NOTICE("FdbOrch LEARN notification: mac %s is already in bv_id 0x%" PRIx64 "with different existing-bp 0x%" PRIx64 " new-bp:0x%" PRIx64,
                            update.entry.mac.to_string().c_str(), entry->bv_id, existing_entry->second.bridge_port_id, bridge_port_id);
                    if (!m_portsOrch->getPortByBridgePortId(existing_entry->second.bridge_port_id, port))
                    {
                        SWSS_LOG_NOTICE("FdbOrch LEARN notification: Failed to get port by bridge port ID 0x%" PRIx64, existing_entry->second.bridge_port_id);
                        return;
                    }
                    else
                    {
                        port.m_fdb_count--;
                        m_portsOrch->setPort(port.m_alias, port);
                        vlan.m_fdb_count--;
                        m_portsOrch->setPort(vlan.m_alias, vlan);
                    }
                    // Continue to add (update/move) the MAC
                }
                else
                {
                    SWSS_LOG_NOTICE("FdbOrch LEARN notification: mac %s is already in bv_id 0x%" PRIx64 "with same bp 0x%" PRIx64,
                            update.entry.mac.to_string().c_str(), entry->bv_id, existing_entry->second.bridge_port_id);
                    // Continue to move the MAC as local.

                    // Existing MAC entry is on same VLAN, Port with Origin MCLAG(remote), its possible after the local learn MAC in
                    //the HW is updated to remote from FdbOrch, Update the MAC back to local in HW so that FdbOrch and HW is Sync and aging enabled.
                    sai_status_t status;
                    sai_fdb_entry_t fdb_entry;
                    fdb_entry.switch_id = gSwitchId;
                    memcpy(fdb_entry.mac_address, entry->mac_address, sizeof(sai_mac_t));
                    fdb_entry.bv_id = entry->bv_id;
                    sai_attribute_t attr;
                    vector<sai_attribute_t> attrs;

                    attr.id = SAI_FDB_ENTRY_ATTR_TYPE;
                    attr.value.s32 = SAI_FDB_ENTRY_TYPE_DYNAMIC;
                    update.sai_fdb_type = SAI_FDB_ENTRY_TYPE_DYNAMIC;
                    attrs.push_back(attr);

                    attr.id = SAI_FDB_ENTRY_ATTR_BRIDGE_PORT_ID;
                    attr.value.oid = existing_entry->second.bridge_port_id;
                    attrs.push_back(attr);

                    for(auto itr : attrs)
                    {
                        status = sai_fdb_api->set_fdb_entry_attribute(&fdb_entry, &itr);
                        if (status != SAI_STATUS_SUCCESS)
                        {
                            SWSS_LOG_ERROR("macUpdate-Failed for MCLAG mac attr.id=0x%x for FDB %s in 0x%" PRIx64 "on %s, rv:%d",
                                        itr.id, update.entry.mac.to_string().c_str(), entry->bv_id, update.port.m_alias.c_str(), status);
                        }
                    }
                    update.add = true;
                    update.type = "dynamic";
                    storeFdbEntryState(update);
                    notify(SUBJECT_TYPE_FDB_CHANGE, &update);

                    return;
                }
            }
            else if (existing_entry->second.origin == FDB_ORIGIN_VXLAN_ADVERTIZED)
            {
                SWSS_LOG_NOTICE("FdbOrch LEARN notification for original VXLAN entry: mac %s is already in vxlan bv_id 0x%"
                PRIx64 " existing-bp 0x%" PRIx64 "new-bp:0x%" PRIx64,
                update.entry.mac.to_string().c_str(), entry->bv_id, existing_entry->second.bridge_port_id, bridge_port_id);
 
                // Get the old port, same logic as MAC MOVE
                if (!m_portsOrch->getPortByBridgePortId(existing_entry->second.bridge_port_id, port_old))
                {
                    SWSS_LOG_ERROR("FdbOrch LEARN notification: Failed to get port by bridge port ID 0x%" PRIx64, existing_entry->second.bridge_port_id);
                    return;
                }

                sai_status_t status;
                sai_fdb_entry_t fdb_entry;
                fdb_entry.switch_id = gSwitchId;
                memcpy(fdb_entry.mac_address, entry->mac_address, sizeof(sai_mac_t));
                fdb_entry.bv_id = entry->bv_id;
                sai_attribute_t attr;
                sai_ip_address_t ipaddr;

                /* Since sai doesn't support remove one fdb attr, just set the ENDPOINT IP to 0 after move local */
                attr.id = SAI_FDB_ENTRY_ATTR_ENDPOINT_IP;
                ipaddr.addr_family = SAI_IP_ADDR_FAMILY_IPV6;
                memcpy(ipaddr.addr.ip6, swss::IpAddress("0:0:0::0").getV6Addr(), sizeof(ipaddr.addr.ip6));
                attr.value.ipaddr = ipaddr;
                status = sai_fdb_api->set_fdb_entry_attribute(&fdb_entry, &attr);
                if (status != SAI_STATUS_SUCCESS)
                {
                    SWSS_LOG_INFO("set fdb attr failed, mac attr.id=0x%x for FDB %s in 0x%" PRIx64 "on %s, rv:%d",
                                   attr.id, update.entry.mac.to_string().c_str(), entry->bv_id, update.port.m_alias.c_str(), status);
                }

                /* mac move from remote vxlan to local */
                mac_move_local = true;
            }
            else
            {
                SWSS_LOG_INFO("update: EVPN_MH_UC: remote mac entry exists for this Received LEARN event for bvid=0x%" PRIx64 "mac=%s port=0x%" PRIx64, entry->bv_id, update.entry.mac.to_string().c_str(), bridge_port_id);
                if (existing_entry->second.dest_type == IFNAME && existing_entry->second.type == "dynamic" && bridge_port_id == existing_entry->second.bridge_port_id) {
                    SWSS_LOG_NOTICE("update: EVPN_MH_UC: C -> (C+D) transition bvid=0x%" PRIx64 "mac=%s port=0x%" PRIx64, entry->bv_id, update.entry.mac.to_string().c_str(), bridge_port_id);
                    m_entries[update.entry].type = "dynamic";
                    return;
                }
                SWSS_LOG_INFO("FdbOrch LEARN notification: mac %s is already in bv_id 0x%"
                    PRIx64 "existing-bp 0x%" PRIx64 "new-bp:0x%" PRIx64,
                    update.entry.mac.to_string().c_str(), entry->bv_id, existing_entry->second.bridge_port_id, bridge_port_id);
            }

            if (!mac_move_local)
            {
                break;
            }
        }

        update.add = true;
        update.entry.port_name = update.port.m_alias;
        update.sai_fdb_type = SAI_FDB_ENTRY_TYPE_DYNAMIC;
        update.type = "dynamic";
        update.port.m_fdb_count++;
        m_portsOrch->setPort(update.port.m_alias, update.port);
        if (mac_move_local)
        {
            SWSS_LOG_DEBUG("Received LEARN event for mac move, vlan %s, m_fdb_count %d", vlan.m_alias.c_str(), vlan.m_fdb_count);
            if (!port_old.m_alias.empty())
            {
                port_old.m_fdb_count--;
                m_portsOrch->setPort(port_old.m_alias, port_old);
            }
        } else {
            vlan.m_fdb_count++;
            SWSS_LOG_DEBUG("Received LEARN event for new mac, vlan %s, m_fdb_count %d", vlan.m_alias.c_str(), vlan.m_fdb_count);
            m_portsOrch->setPort(vlan.m_alias, vlan);
        }

        storeFdbEntryState(update);
        notify(SUBJECT_TYPE_FDB_CHANGE, &update);
        if (mac_move_local)
        {
            /* Try to add local neighbor entry if exists
             * Since this mac is at the local side now
             */
            gNeighOrch->processFDBAdd(update.entry);
            notifyTunnelOrch(port_old);
        }

        break;
    }
    case SAI_FDB_EVENT_AGED:
    {
        SWSS_LOG_INFO("update: EVPN_MH_UC: Received AGE out event for bvid=0x%" PRIx64 " mac=%s port=0x%" PRIx64,
                       entry->bv_id, update.entry.mac.to_string().c_str(), bridge_port_id);

        auto status = sai_fdb_api->remove_fdb_entry(entry);
        if (status != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_INFO("FdbOrch ageout: Ignore the failure of removing FDB entry. mac=%s, bv_id=0x%" PRIx64,
                           update.entry.mac.to_string().c_str(), entry->bv_id);
        }
 
        auto existing_entry = m_entries.find(update.entry);
        // we don't have such entries
        if (existing_entry == m_entries.end())
        {
             SWSS_LOG_INFO("FdbOrch AGE notification: mac %s is not present in bv_id 0x%" PRIx64 " bp 0x%" PRIx64,
                    update.entry.mac.to_string().c_str(), entry->bv_id, bridge_port_id);
             break;
        }

        if (existing_entry->second.bridge_port_id != bridge_port_id)
        {
            SWSS_LOG_INFO("FdbOrch AGE notification: Stale aging event received for mac-bv_id %s-0x%" PRIx64 " with bp=0x%" PRIx64 " existing bp=0x%" PRIx64,
                           update.entry.mac.to_string().c_str(), entry->bv_id, bridge_port_id, existing_entry->second.bridge_port_id);
            // We need to get the port for bridge-port in existing fdb
            if (!m_portsOrch->getPortByBridgePortId(existing_entry->second.bridge_port_id, update.port))
            {
                SWSS_LOG_INFO("FdbOrch AGE notification: Failed to get port by bridge port ID 0x%" PRIx64, existing_entry->second.bridge_port_id);
            }
            // dont return, let it delete just to bring SONiC and SAI in sync
            // return;
        }

        if (existing_entry->second.type == "static")
        {
            update.type = "static";

            if (vlan.m_members.find(update.port.m_alias) == vlan.m_members.end())
            {
                FdbData fdbData;
                fdbData.bridge_port_id = SAI_NULL_OBJECT_ID;
                fdbData.type = update.type;
                fdbData.origin = existing_entry->second.origin;
                fdbData.dest_type = existing_entry->second.dest_type;
                fdbData.dest_value = existing_entry->second.dest_value;
                fdbData.esi = existing_entry->second.esi;
                fdbData.vni = existing_entry->second.vni;
                saved_fdb_entries[update.port.m_alias].push_back(
                        {existing_entry->first.mac, vlan.m_vlan_info.vlan_id, fdbData});
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
                    if (handleSaiCreateStatus(SAI_API_FDB, status) != task_success)
                    {
                        return;
                    }
                }
                return;
            }
        }

        // If MAC is MCLAG remote do not delete for age event, Add the MAC back..
        if ((existing_entry->second.origin == FDB_ORIGIN_MCLAG_ADVERTIZED) ||
            (existing_entry->second.origin == FDB_ORIGIN_VXLAN_ADVERTIZED))
        {
            sai_status_t status;
            sai_fdb_entry_t fdb_entry;

            fdb_entry.switch_id = gSwitchId;
            memcpy(fdb_entry.mac_address, entry->mac_address, sizeof(sai_mac_t));
            fdb_entry.bv_id = entry->bv_id;

            sai_attribute_t attr;
            vector<sai_attribute_t> attrs;

            attr.id = SAI_FDB_ENTRY_ATTR_TYPE;
            attr.value.s32 = SAI_FDB_ENTRY_TYPE_STATIC;
            attrs.push_back(attr);

            attr.id = SAI_FDB_ENTRY_ATTR_ALLOW_MAC_MOVE;
            attr.value.booldata = true;
            attrs.push_back(attr);
            existing_entry->second.allow_mac_move = true;

            attr.id = SAI_FDB_ENTRY_ATTR_BRIDGE_PORT_ID;
            attr.value.oid = existing_entry->second.bridge_port_id;
            attrs.push_back(attr);

            SWSS_LOG_NOTICE("fdbEvent: MAC age event received, MAC is %s, added back"
                "to HW type %s FDB %s in %s on %s",
                existing_entry->second.origin == FDB_ORIGIN_MCLAG_ADVERTIZED ? "MCLAG origin" : "VXLAN origin",
                existing_entry->second.type.c_str(),
                update.entry.mac.to_string().c_str(), vlan.m_alias.c_str(),
                update.port.m_alias.c_str());

            status = sai_fdb_api->create_fdb_entry(&fdb_entry, (uint32_t)attrs.size(), attrs.data());
            if (status != SAI_STATUS_SUCCESS)
            {
                SWSS_LOG_ERROR("Failed to create %s FDB %s in %s on %s, rv:%d",
                        existing_entry->second.type.c_str(), update.entry.mac.to_string().c_str(),
                        vlan.m_alias.c_str(), update.port.m_alias.c_str(), status);
            }
            return;
        }

        if (existing_entry->second.origin == FDB_ORIGIN_LEARN && existing_entry->second.dest_type == IFNAME) {
            if (existing_entry->second.type == "dynamic_control_learn") {
                SWSS_LOG_NOTICE("update: EVPN_MH_UC: ageout (C+D) -> C, type %s FDB %s in %s on %s",
                        existing_entry->second.type.c_str(), update.entry.mac.to_string().c_str(),
                        vlan.m_alias.c_str(), update.port.m_alias.c_str());
                m_entries[update.entry].type = "dynamic";
                m_entries[update.entry].origin = FDB_ORIGIN_VXLAN_ADVERTIZED;

                Port vlan;
                if (!m_portsOrch->getPort(update.entry.bv_id, vlan))
                {
                    SWSS_LOG_NOTICE("FdbOrch notification: Failed to locate \
                                    vlan port from bv_id 0x%" PRIx64, update.entry.bv_id);
                    return;
                }

                // ref: https://github.com/Azure/sonic-swss/blob/master/doc/swss-schema.md#fdb_table
                string key = "Vlan" + to_string(vlan.m_vlan_info.vlan_id) + ":" + update.entry.mac.to_string();
                m_fdbStateTable.del(key);

                sai_attribute_t attr;
                vector<sai_attribute_t> attrs;

                attr.id = SAI_FDB_ENTRY_ATTR_TYPE;
                attr.value.s32 = SAI_FDB_ENTRY_TYPE_STATIC;
                attrs.push_back(attr);
                attr.id = SAI_FDB_ENTRY_ATTR_BRIDGE_PORT_ID;
                attr.value.oid = bridge_port_id;
                attrs.push_back(attr);
                status = sai_fdb_api->create_fdb_entry(entry, (uint32_t)attrs.size(), attrs.data());
                if (status != SAI_STATUS_SUCCESS)
                {
                    SWSS_LOG_ERROR("Failed to create FDB %s on %s, rv:%d",
                        existing_entry->first.mac.to_string().c_str(), update.port.m_alias.c_str(), status);
                    if (handleSaiCreateStatus(SAI_API_FDB, status) != task_success)
                    {
                        return;
                    }
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
            SWSS_LOG_DEBUG("Received AGEOUT event, vlan %s, m_fdb_count %d", vlan.m_alias.c_str(), vlan.m_fdb_count);
            m_portsOrch->setPort(vlan.m_alias, vlan);
        }
        storeFdbEntryState(update);

        /* Remove local neighbor entry if exists
         */
        SWSS_LOG_INFO("Received mac age out for mac:%s vlan:0x%" PRIx64 "of type:%d",
				update.entry.mac.to_string().c_str(), update.entry.bv_id, existing_entry->second.dest_type);

        gNeighOrch->processFDBResolve(update.entry);

        notify(SUBJECT_TYPE_FDB_CHANGE, &update);

        notifyTunnelOrch(update.port);
        break;
    }
    case SAI_FDB_EVENT_MOVE:
    {
        Port port_old;
        bool mac_move_local = false;
        bool existing = true;
        auto existing_entry = m_entries.find(update.entry);

        SWSS_LOG_INFO("Received MOVE event for bvid=0x%" PRIx64 " mac=%s port=0x%" PRIx64,
                       entry->bv_id, update.entry.mac.to_string().c_str(), bridge_port_id);

        // We should already have such entry
        if (existing_entry == m_entries.end())
        {
             existing = false;
             SWSS_LOG_WARN("FdbOrch MOVE notification: mac %s is not found in bv_id 0x%" PRIx64,
                    update.entry.mac.to_string().c_str(), entry->bv_id);
        }
        else if (!m_portsOrch->getPortByBridgePortId(existing_entry->second.bridge_port_id, port_old))
        {
            SWSS_LOG_ERROR("FdbOrch MOVE notification: Failed to get port by bridge port ID 0x%" PRIx64, existing_entry->second.bridge_port_id);
            return;
        }
        else if (existing_entry->second.origin == FDB_ORIGIN_VXLAN_ADVERTIZED)
        {
           /* mac move from remote vxlan to local */
           SWSS_LOG_NOTICE("FdbOrch MOVE notification from remote vxlan to local: mac %s, bv_id 0x%" PRIx64,
                    update.entry.mac.to_string().c_str(), entry->bv_id);
           mac_move_local = true;
        }

        /* If the existing MAC is MCLAG remote, change its type to dynamic. */
        if (existing_entry->second.origin == FDB_ORIGIN_MCLAG_ADVERTIZED)
        {
            if (existing_entry->second.bridge_port_id != bridge_port_id)
            {
                sai_status_t status;
                sai_fdb_entry_t fdb_entry;
                fdb_entry.switch_id = gSwitchId;
                memcpy(fdb_entry.mac_address, entry->mac_address, sizeof(sai_mac_t));
                fdb_entry.bv_id = entry->bv_id;
                sai_attribute_t attr;
                vector<sai_attribute_t> attrs;

                attr.id = SAI_FDB_ENTRY_ATTR_ALLOW_MAC_MOVE;
                attr.value.booldata = false;
                attrs.push_back(attr);

                attr.id = SAI_FDB_ENTRY_ATTR_TYPE;
                attr.value.s32 = SAI_FDB_ENTRY_TYPE_DYNAMIC;
                attrs.push_back(attr);

                attr.id = SAI_FDB_ENTRY_ATTR_BRIDGE_PORT_ID;
                attr.value.oid = bridge_port_id;
                attrs.push_back(attr);

                for(auto itr : attrs)
                {
                    status = sai_fdb_api->set_fdb_entry_attribute(&fdb_entry, &itr);
                    if (status != SAI_STATUS_SUCCESS)
                    {
                        SWSS_LOG_ERROR("macUpdate-Failed for MCLAG mac attr.id=0x%x for FDB %s in 0x%" PRIx64 "on %s, rv:%d",
                                        itr.id, update.entry.mac.to_string().c_str(), entry->bv_id, update.port.m_alias.c_str(), status);
                    }
                }
            }
        }

        update.add = true;
        update.entry.port_name = update.port.m_alias;
        if (existing && !port_old.m_alias.empty())
        {
            port_old.m_fdb_count--;
            m_portsOrch->setPort(port_old.m_alias, port_old);
        }
        update.type = "dynamic";
        update.port.m_fdb_count++;
        m_portsOrch->setPort(update.port.m_alias, update.port);

        //update Vlan fdb count if no existing fdb
        if ((!existing) && (!vlan.m_alias.empty())) 
        {
            vlan.m_fdb_count++;
            SWSS_LOG_DEBUG("Received MOVE event without existing fdb, vlan %s, m_fdb_count %d", vlan.m_alias.c_str(), vlan.m_fdb_count);
            m_portsOrch->setPort(vlan.m_alias, vlan);
        }

        update.sai_fdb_type = SAI_FDB_ENTRY_TYPE_DYNAMIC;
        storeFdbEntryState(update);

        notify(SUBJECT_TYPE_FDB_CHANGE, &update);
 
        if (mac_move_local)
        {
            /* Try to add local neighbor entry if exists
             * Since this mac is at the local side now
             */
            gNeighOrch->processFDBAdd(update.entry);
        }

        if (existing) 
        {
            notifyTunnelOrch(port_old);
        }

        break;
    }
    case SAI_FDB_EVENT_FLUSHED:

        SWSS_LOG_INFO("FDB Flush event received: [ %s , 0x%" PRIx64 " ], \
                       bridge port ID: 0x%" PRIx64 ".",
                       update.entry.mac.to_string().c_str(), entry->bv_id,
                       bridge_port_id);

        string vlanName = "-";
        if (!vlan.m_alias.empty()) {
            vlanName = "Vlan" + to_string(vlan.m_vlan_info.vlan_id);
        }

        SWSS_LOG_INFO("FDB Flush: [ %s , %s ] = { port: %s }", update.entry.mac.to_string().c_str(),
                      vlanName.c_str(), update.port.m_alias.c_str());

        handleSyncdFlushNotif(entry->bv_id, bridge_port_id, update.entry.mac, sai_fdb_type);

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

    FdbEntry entry;
    entry.mac = mac;
    entry.bv_id = port.m_vlan_info.vlan_oid;

    auto it = m_entries.find(entry);
    if (it == m_entries.end())
    {
        // This message is now expected in many cases since orchagent will process events such as
        // learning new neighbor entries prior to updating the m_entries FDB cache.
        SWSS_LOG_INFO("Failed to get cached bridge port ID for FDB entry %s",
            mac.to_string().c_str());
        return false;
    }

    if (!m_portsOrch->getPortByBridgePortId(it->second.bridge_port_id, port))
    {
        SWSS_LOG_ERROR("Failed to get port by bridge port ID 0x%" PRIx64, it->second.bridge_port_id);
        return false;
    }

    return true;
}

bool FdbOrch::is_fdb_programmed_to_vxlan_tunnel(FdbEntry& entry)
{
    bool programmed_to_tunnel = false;

    auto it = m_entries.find(entry);
    if (it != m_entries.end())
    {
        Port port;
        if (FDB_ORIGIN_VXLAN_ADVERTIZED == it->second.origin &&
            m_portsOrch->getPortByBridgePortId(it->second.bridge_port_id, port))
        {
            SWSS_LOG_INFO("Cached fdb entry %s origin %d, port type %u", entry.mac.to_string().c_str(), it->second.origin, port.m_type);
            programmed_to_tunnel = (port.m_type == Port::TUNNEL);
        }
    }

	return programmed_to_tunnel;
}

void FdbOrch::doTask(Consumer& consumer)
{
    SWSS_LOG_ENTER();

    if (!m_portsOrch->allPortsReady())
    {
        return;
    }

    FdbOrigin origin = FDB_ORIGIN_PROVISIONED;

    string table_name = consumer.getTableName();
    if(table_name == APP_VXLAN_FDB_TABLE_NAME)
    {
        origin = FDB_ORIGIN_VXLAN_ADVERTIZED;
    }

    if (table_name == APP_MCLAG_FDB_TABLE_NAME)
    {
        origin = FDB_ORIGIN_MCLAG_ADVERTIZED;
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
            string esi = "";
            unsigned int vni = 0;
            string sticky = "";
            string discard = "false";
            NEXT_HOP_VALUE_TYPE dest_type = UNKNOWN;
            string dest_value;

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
                if (fvField(i) == "discard")
                {
                    discard = fvValue(i);
                }

                if(origin == FDB_ORIGIN_VXLAN_ADVERTIZED)
                {
                    if (fvField(i) == "remote_vtep") {
                        dest_type = VTEP;
                        dest_value = fvValue(i);
                        // Creating an IpAddress object to validate if remote_ip is valid
                        // if invalid it will throw the exception and we will ignore the
                        // event
                        try {
                            IpAddress valid_ip = IpAddress(dest_value);
                            (void)valid_ip; // To avoid g++ warning
                        } catch(exception &e) {
                            SWSS_LOG_NOTICE("Invalid IP address in remote MAC %s", dest_value.c_str());
                            dest_value = "";
                            break;
                        }
                    } else if (fvField(i) == "nexthop_group") {
                        dest_type = NEXTHOPGROUP;
                        dest_value = fvValue(i);
                    }  else if (fvField(i) == "ifname") {
                        dest_type = IFNAME;
                        dest_value = fvValue(i);
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
            assert(type == "dynamic" || type == "dynamic_local" || type == "dynamic_control_learn" || type == "static" );

            if(origin == FDB_ORIGIN_VXLAN_ADVERTIZED)
            {
                if (dest_type == VTEP) {
                    VxlanTunnelOrch* tunnel_orch = gDirectory.get<VxlanTunnelOrch*>();
                    if (tunnel_orch->isDipTunnelsSupported())
                    {
                        if(!dest_value.length())
                        {
                            it = consumer.m_toSync.erase(it);
                            continue;
                        }
                        port = tunnel_orch->getTunnelPortName(dest_value);
                    }
                    else
                    {
                        EvpnNvoOrch* evpn_nvo_orch = gDirectory.get<EvpnNvoOrch*>();
                        VxlanTunnel* sip_tunnel = evpn_nvo_orch->getEVPNVtep();
                        if (sip_tunnel == NULL)
                        {
                            it = consumer.m_toSync.erase(it);
                            continue;
                        }
                        port = tunnel_orch->getTunnelPortName(sip_tunnel->getSrcIP().to_string(), true);
                    }
                }
                else if (dest_type == IFNAME) {
                    /* For IFNAME case, use the dest_value as port name directly */
                    port = dest_value;
                }
            }

            FdbData fdbData;
            fdbData.bridge_port_id = SAI_NULL_OBJECT_ID;
            fdbData.type = type;
            fdbData.origin = origin;
            fdbData.dest_type = dest_type;
            fdbData.dest_value = dest_value;
            fdbData.esi = esi;
            fdbData.vni = vni;
            fdbData.is_flush_pending = false;
            fdbData.discard = discard;
            entry.port_name = port;
            if (addFdbEntry(entry, port, fdbData))
            {
                if (origin == FDB_ORIGIN_MCLAG_ADVERTIZED)
                {
                    string key = "Vlan" + to_string(vlan.m_vlan_info.vlan_id) + ":" + entry.mac.to_string();
                    if (type == "dynamic_local")
                    {
                        m_mclagFdbStateTable.del(key);
                    }
                }

                if(origin == FDB_ORIGIN_VXLAN_ADVERTIZED)
                {
                    if (dest_type == VTEP) {
                        VxlanTunnelOrch* tunnel_orch = gDirectory.get<VxlanTunnelOrch*>();

                        if(!dest_value.length())
                        {
                            it = consumer.m_toSync.erase(it);
                            continue;
                        }
                        port = tunnel_orch->getTunnelPortName(dest_value);
                    }
                }
                it = consumer.m_toSync.erase(it);
            }
            else
                it++;
        }
        else if (op == DEL_COMMAND)
        {
            if (removeFdbEntry(entry, origin))
            {
                if (origin == FDB_ORIGIN_MCLAG_ADVERTIZED)
                {
                    string key = "Vlan" + to_string(vlan.m_vlan_info.vlan_id) + ":" + entry.mac.to_string();
                    m_mclagFdbStateTable.del(key);
                    SWSS_LOG_NOTICE("fdbEvent: do Task Delete MCLAG FDB from state mclag remote fdb table: "
                            "Mac: %s Vlan: %d ",entry.mac.to_string().c_str(), vlan.m_vlan_info.vlan_id );
                }

                it = consumer.m_toSync.erase(it);
            }
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

    if (!m_portsOrch->allPortsReady())
    {
        return;
    }

    sai_status_t status;
    std::string op;
    std::string data;
    std::vector<swss::FieldValueTuple> values;
    string alias;
    string vlan;
    Port port;
    Port vlanPort;

    consumer.pop(op, data, values);

    if (&consumer == m_flushNotificationsConsumer)
    {
        if (op == "ALL")
        {
            vector<sai_attribute_t>    attrs;
            sai_attribute_t            attr;
            attr.id = SAI_FDB_FLUSH_ATTR_ENTRY_TYPE;
            attr.value.s32 = SAI_FDB_FLUSH_ENTRY_TYPE_DYNAMIC;
            attrs.push_back(attr);
            status = sai_fdb_api->flush_fdb_entries(gSwitchId, (uint32_t)attrs.size(), attrs.data());
            if (status != SAI_STATUS_SUCCESS)
            {
                SWSS_LOG_ERROR("Flush fdb failed, return code %x", status);
            }

            if (status == SAI_STATUS_SUCCESS) {
                for (map<FdbEntry, FdbData>::iterator it = m_entries.begin();
                        it != m_entries.end(); it++)
                {
                    it->second.is_flush_pending = true;
                }
            }

            return;
        }
        else if (op == "PORT")
        {
            alias = data;
            if (alias.empty())
            {
                SWSS_LOG_ERROR("Receive wrong port to flush fdb!");
                return;
            }
            if (!m_portsOrch->getPort(alias, port))
            {
                SWSS_LOG_ERROR("Get Port from port(%s) failed!", alias.c_str());
                return;
            }
            if (port.m_bridge_port_id == SAI_NULL_OBJECT_ID)
            {
                return;
            }
            flushFDBEntries(port.m_bridge_port_id, SAI_NULL_OBJECT_ID);
            SWSS_LOG_NOTICE("Clear fdb by port(%s)", alias.c_str());
            return;
        }
        else if (op == "VLAN")
        {
            vlan = data;
            if (vlan.empty())
            {
                SWSS_LOG_ERROR("Receive wrong vlan to flush fdb!");
                return;
            }
            if (!m_portsOrch->getPort(vlan, vlanPort))
            {
                SWSS_LOG_ERROR("Get Port from vlan(%s) failed!", vlan.c_str());
                return;
            }
            if (vlanPort.m_vlan_info.vlan_oid == SAI_NULL_OBJECT_ID)
            {
                return;
            }
            flushFDBEntries(SAI_NULL_OBJECT_ID, vlanPort.m_vlan_info.vlan_oid);
            SWSS_LOG_NOTICE("Clear fdb by vlan(%s)", vlan.c_str());
            return;
        }
        else if (op == "PORTVLAN")
        {
            size_t found = data.find('|');
            if (found != string::npos)
            {
                alias = data.substr(0, found);
                vlan = data.substr(found+1);
            }
            if (alias.empty() || vlan.empty())
            {
                SWSS_LOG_ERROR("Receive wrong port or vlan to flush fdb!");
                return;
            }
            if (!m_portsOrch->getPort(alias, port))
            {
                SWSS_LOG_ERROR("Get Port from port(%s) failed!", alias.c_str());
                return;
            }
            if (!m_portsOrch->getPort(vlan, vlanPort))
            {
                SWSS_LOG_ERROR("Get Port from vlan(%s) failed!", vlan.c_str());
                return;
            }
            if (port.m_bridge_port_id == SAI_NULL_OBJECT_ID ||
                vlanPort.m_vlan_info.vlan_oid == SAI_NULL_OBJECT_ID)
            {
                return;
            }
            flushFDBEntries(port.m_bridge_port_id, vlanPort.m_vlan_info.vlan_oid); 
            SWSS_LOG_NOTICE("Clear fdb by port(%s)+vlan(%s)", alias.c_str(), vlan.c_str());
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
        sai_fdb_entry_type_t sai_fdb_type = SAI_FDB_ENTRY_TYPE_DYNAMIC;

        sai_deserialize_fdb_event_ntf(data, count, &fdbevent);

        for (uint32_t i = 0; i < count; ++i)
        {
            sai_object_id_t oid = SAI_NULL_OBJECT_ID;

            for (uint32_t j = 0; j < fdbevent[i].attr_count; ++j)
            {
                if (fdbevent[i].attr[j].id == SAI_FDB_ENTRY_ATTR_BRIDGE_PORT_ID)
                {
                    oid = fdbevent[i].attr[j].value.oid;
                }
                else if (fdbevent[i].attr[j].id == SAI_FDB_ENTRY_ATTR_TYPE)
                {
                    sai_fdb_type = (sai_fdb_entry_type_t)fdbevent[i].attr[j].value.s32;
                }
            }

            this->update(fdbevent[i].event_type, &fdbevent[i].fdb_entry, oid, sai_fdb_type);
        }

        sai_deserialize_free_fdb_event_ntf(count, fdbevent);
    }
}

/*
 * Name: flushFDBEntries
 * Params:
 *     bridge_port_oid - SAI object ID of bridge port associated with the port
 *     vlan_oid - SAI object ID of the VLAN
 * Description:
 *     Flushes FDB entries based on bridge_port_oid, or vlan_oid or both.
 *     This function is called in three cases.
 *     1. Port is removed from VLAN (via SUBJECT_TYPE_VLAN_MEMBER_CHANGE)
 *     2. Bridge port OID is removed (Direct call)
 *     3. Port is shut down (via SUBJECT_TYPE_
 */
void FdbOrch::flushFDBEntries(sai_object_id_t bridge_port_oid,
                              sai_object_id_t vlan_oid)
{
    vector<sai_attribute_t>    attrs;
    sai_attribute_t            attr;
    sai_status_t               rv = SAI_STATUS_SUCCESS;

    bool bridge_port_exist = false;
    bool vlan_exist = false;

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
        bridge_port_exist = true;
    }

    if (SAI_NULL_OBJECT_ID != vlan_oid)
    {
        attr.id = SAI_FDB_FLUSH_ATTR_BV_ID;
        attr.value.oid = vlan_oid;
        attrs.push_back(attr);
        vlan_exist = true;
    }

    /* do not flush static mac */
    attr.id = SAI_FDB_FLUSH_ATTR_ENTRY_TYPE;
    attr.value.s32 = SAI_FDB_FLUSH_ENTRY_TYPE_DYNAMIC;
    attrs.push_back(attr);

    SWSS_LOG_INFO("Flushing FDB bridge_port_oid: 0x%" PRIx64 ", and bvid_oid:0x%" PRIx64 ".", bridge_port_oid, vlan_oid);

    rv = sai_fdb_api->flush_fdb_entries(gSwitchId, (uint32_t)attrs.size(), attrs.data());
    if (SAI_STATUS_SUCCESS != rv)
    {
        SWSS_LOG_ERROR("Flushing FDB failed. rv:%d", rv);
    }

    if (SAI_STATUS_SUCCESS == rv) {
        for (map<FdbEntry, FdbData>::iterator it = m_entries.begin();
                it != m_entries.end(); it++)
        {
            if ((!bridge_port_exist || it->second.bridge_port_id == bridge_port_oid) &&
                (!vlan_exist || it->first.bv_id == vlan_oid))
            {
                it->second.is_flush_pending = true;
            }
        }
    }
}

/*
 * Name: flushAllFDBEntries
 * Params:
 *     bridge_port_oid - SAI object ID of bridge port associated with the port
 *     vlan_oid - SAI object ID of the VLAN
 * Description:
 *     Flushes ALL FDB entries based on bridge_port_oid, or vlan_oid or both.
 *     This function is called in three cases.
 *     1. Port is removed from VLAN (via SUBJECT_TYPE_VLAN_MEMBER_CHANGE)
 *     2. Bridge port OID is removed (Direct call)
 *     3. Port is shut down (via SUBJECT_TYPE_
 */
void FdbOrch::flushAllFDBEntries(sai_object_id_t bridge_port_oid,
                                 sai_object_id_t vlan_oid)
{
    vector<sai_attribute_t>    attrs;
    sai_attribute_t            attr;
    sai_status_t               rv = SAI_STATUS_SUCCESS;

    bool bridge_port_exist = false;
    bool vlan_exist = false;

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
        bridge_port_exist = true;
    }

    if (SAI_NULL_OBJECT_ID != vlan_oid)
    {
        attr.id = SAI_FDB_FLUSH_ATTR_BV_ID;
        attr.value.oid = vlan_oid;
        attrs.push_back(attr);
        vlan_exist = true;
    }

    Port port;
    if (bridge_port_exist)
    {
        if (!m_portsOrch->getPortByBridgePortId(bridge_port_oid, port))
        {
            SWSS_LOG_WARN("FdbOrch flushAllFDB: Failed to locate port from bridge_port_id 0x%" PRIx64, bridge_port_oid);
            return;
        }

        /* Using a workaround to flush all mac under the tunnel port */
        if (port.m_type == Port::TUNNEL)
        {
            SWSS_LOG_NOTICE("Try to flushAllFDB for tunnel port %s bridge_port_id 0x%" PRIx64, port.m_alias.c_str(), bridge_port_oid);
            /* Try to remove all remote FDB under this tunnel port one by one */
            for (auto itr = m_entries.begin(); itr != m_entries.end();)
            {
                auto curr = itr++;
                if ((curr->second.bridge_port_id == bridge_port_oid) &&
                    (!vlan_exist || (curr->first.bv_id == vlan_oid)))
                {
                    SWSS_LOG_DEBUG("FdbOrch flush tunnel port: mac=%s bv_id=0x%" PRIx64 " origin %d", curr->first.mac.to_string().c_str(), curr->first.bv_id, curr->second.origin);

                    Port vlan;
                    sai_status_t status;
                    FdbEntry entry;
                    entry.mac = curr->first.mac;
                    entry.bv_id = curr->first.bv_id;
                    entry.port_name = curr->first.port_name;
                    auto type = curr->second.type;
                    sai_fdb_entry_t fdb_entry;
                    fdb_entry.switch_id = gSwitchId;
                    memcpy(fdb_entry.mac_address, entry.mac.getMac(), sizeof(sai_mac_t));
                    fdb_entry.bv_id = entry.bv_id;

                    status = sai_fdb_api->remove_fdb_entry(&fdb_entry);
                    if (status != SAI_STATUS_SUCCESS)
                    {
                        SWSS_LOG_ERROR("FdbOrch flushAllFDB: Failed to remove FDB entry. mac=%s, bv_id=0x%" PRIx64,
                                        entry.mac.to_string().c_str(), entry.bv_id);
                        task_process_status handle_status = handleSaiRemoveStatus(SAI_API_FDB, status);
                        if (handle_status != task_success)
                        {
                            parseHandleSaiStatusFailure(handle_status);
                            continue;
                        }
                    }

                    SWSS_LOG_INFO("Removed mac=%s bv_id=0x%" PRIx64 " port:%s",
                                   entry.mac.to_string().c_str(), entry.bv_id, port.m_alias.c_str());

                    port.m_fdb_count--;
                    if (!m_portsOrch->getPort(entry.bv_id, vlan))
                    {
                        SWSS_LOG_NOTICE("FdbOrch notification: Failed to locate vlan port from bv_id 0x%" PRIx64, entry.bv_id);
                    }
                    else
                    {
                        vlan.m_fdb_count--;
                        SWSS_LOG_INFO("after removing fdb, vlan %s, m_fdb_count %d", vlan.m_alias.c_str(), vlan.m_fdb_count);
                        m_portsOrch->setPort(vlan.m_alias, vlan);
                    }

                    (void)m_entries.erase(curr);
                    removeFdbEntryFromPortCache(entry, port);

                    gCrmOrch->decCrmResUsedCounter(CrmResourceType::CRM_FDB_ENTRY);

                    FdbUpdate update;
                    update.entry = entry;
                    update.port = port;
                    update.type = type;
                    update.add = false;

                    notify(SUBJECT_TYPE_FDB_CHANGE, &update);
                }
            }
            SWSS_LOG_NOTICE("flushAllFDB Done for tunnel bridge_port_id 0x%" PRIx64, bridge_port_oid);

            m_portsOrch->setPort(port.m_alias, port);
            return;
        }
    }

    /* As to non-tunnel port, flush both static and dynamic mac */
    attr.id = SAI_FDB_FLUSH_ATTR_ENTRY_TYPE;
    attr.value.s32 = SAI_FDB_FLUSH_ENTRY_TYPE_ALL;
    attrs.push_back(attr);

    SWSS_LOG_INFO("Flushing all FDB bridge_port_oid: 0x%" PRIx64 ", and bvid_oid:0x%" PRIx64 ".", bridge_port_oid, vlan_oid);

    rv = sai_fdb_api->flush_fdb_entries(gSwitchId, (uint32_t)attrs.size(), attrs.data());
    if (SAI_STATUS_SUCCESS != rv)
    {
        SWSS_LOG_ERROR("Flushing all FDB failed. rv:%d", rv);
    }

    if (SAI_STATUS_SUCCESS == rv) {
        for (map<FdbEntry, FdbData>::iterator it = m_entries.begin();
                it != m_entries.end(); it++)
        {
            if ((!bridge_port_exist || it->second.bridge_port_id == bridge_port_oid) &&
                (!vlan_exist || it->first.bv_id == vlan_oid))
            {
                it->second.is_flush_pending = true;
            }
        }
    }
}
void FdbOrch::flushFdbByVlan(const string &alias)
{
    sai_status_t status;
    swss::Port vlan;
    sai_attribute_t vlan_attr[2];

    if (!m_portsOrch->getPort(alias, vlan))
    {
        return;
    }

    vlan_attr[0].id = SAI_FDB_FLUSH_ATTR_BV_ID;
    vlan_attr[0].value.oid = vlan.m_vlan_info.vlan_oid;
    vlan_attr[1].id = SAI_FDB_FLUSH_ATTR_ENTRY_TYPE;
    vlan_attr[1].value.s32 = SAI_FDB_FLUSH_ENTRY_TYPE_DYNAMIC;
    status = sai_fdb_api->flush_fdb_entries(gSwitchId, 2, vlan_attr);
  
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Flush fdb failed, return code %x", status);
    }
    else
    {
        SWSS_LOG_INFO("Flush by vlan %s vlan_oid 0x%" PRIx64 "",
                    alias.c_str(), vlan.m_vlan_info.vlan_oid);
    }

    return;
}

void FdbOrch::notifyObserversFDBFlush(Port &port, sai_object_id_t& bvid)
{
    FdbFlushUpdate flushUpdate;
    flushUpdate.port = port;

    for (auto itr = m_entries.begin(); itr != m_entries.end(); ++itr)
    {
        if ((itr->first.port_name == port.m_alias) &&
            (itr->first.bv_id == bvid))
        {
            SWSS_LOG_INFO("Adding MAC learnt on [ port:%s , bvid:0x%" PRIx64 "]\
                           to ARP flush", port.m_alias.c_str(), bvid);
            FdbEntry entry;
            entry.mac = itr->first.mac;
            entry.bv_id = itr->first.bv_id;
            flushUpdate.entries.push_back(entry);
        }
    }

    if (!flushUpdate.entries.empty())
    {
        notify(SUBJECT_TYPE_FDB_FLUSH_CHANGE, &flushUpdate);
    }
}

void FdbOrch::updatePortOperState(const PortOperStateUpdate& update)
{
    SWSS_LOG_ENTER();
    if (update.operStatus == SAI_PORT_OPER_STATUS_DOWN)
    {
        swss::Port p = update.port;
        if (gMlagOrch->isMlagInterface(p.m_alias))
        {
            SWSS_LOG_NOTICE("Ignoring fdb flush on MCLAG port:%s", p.m_alias.c_str());
            return;
        }

        if (p.m_bridge_port_id != SAI_NULL_OBJECT_ID)
        {
            flushFDBEntries(p.m_bridge_port_id, SAI_NULL_OBJECT_ID);
        }

        // Get BVID of each VLAN that this port is a member of
        // and call notifyObserversFDBFlush
        vlan_members_t vlan_members;
        m_portsOrch->getPortVlanMembers(p, vlan_members);
        for (const auto& vlan_member: vlan_members)
        {
            swss::Port vlan;
            string vlan_alias = VLAN_PREFIX + to_string(vlan_member.first);
            if (!m_portsOrch->getPort(vlan_alias, vlan))
            {
                SWSS_LOG_INFO("Failed to locate VLAN %s", vlan_alias.c_str());
                continue;
            }
            notifyObserversFDBFlush(p, vlan.m_vlan_info.vlan_oid);
        }

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
        flushAllFDBEntries(port.m_bridge_port_id, vlan.m_vlan_info.vlan_oid);
        notifyObserversFDBFlush(port, vlan.m_vlan_info.vlan_oid);
        return;
    }

    string port_name = update.member.m_alias;
    auto fdb_list = std::move(saved_fdb_entries[port_name]);
    saved_fdb_entries[port_name].clear();
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
                entry.port_name = port_name;
                (void)addFdbEntry(entry, port_name, fdb.fdbData);
            }
            else
            {
                saved_fdb_entries[port_name].push_back(fdb);
            }
        }
    }
}

/**
 * @brief Remove an FDB entry from the port-specific cache
 *
 * This function removes a specific FDB entry from the port-based cache
 * (m_entries_by_port) which maintains a mapping of port names to their
 * associated FDB entries. This is typically called when an FDB entry
 * is deleted or moved from one port to another to keep the cache consistent.
 *
 * @param entry The FDB entry to remove from the cache
 * @param port The port from which to remove the FDB entry
 */
void FdbOrch::removeFdbEntryFromPortCache(const FdbEntry& entry, const Port& port)
{
    auto& entries_for_port = m_entries_by_port[port.m_alias];

    for (auto it = entries_for_port.begin(); it != entries_for_port.end(); it++) {
        if (*it == entry) {
            entries_for_port.erase(it);
            break;
        }
    }
}

bool FdbOrch::isDestinationSame(FdbData &oldFdbData, FdbData &newFdbData) {
    NEXT_HOP_VALUE_TYPE oldDestType = oldFdbData.dest_type;
    NEXT_HOP_VALUE_TYPE newDestType = newFdbData.dest_type;

    if (oldDestType != newDestType) {
        return false;
    }
    if (oldFdbData.dest_value.compare(newFdbData.dest_value) == 0) {
        return true;
    }
    return false;
}

bool FdbOrch::addFdbEntry(const FdbEntry& entry, const string& port_name,
        FdbData fdbData)
{
    Port vlan;
    Port port;
    string end_point_ip = "";

    VxlanTunnelOrch* tunnel_orch = gDirectory.get<VxlanTunnelOrch*>();

    SWSS_LOG_ENTER();
    SWSS_LOG_INFO("addFdbEntry: EVPN_MH_UC: mac=%s bv_id=0x%" PRIx64 " port_name=%s type=%s origin=%d dest_type=%s dest_value=%s",
            entry.mac.to_string().c_str(), entry.bv_id, port_name.c_str(),
            fdbData.type.c_str(), fdbData.origin, destTypeToString[fdbData.dest_type].c_str(),
            fdbData.dest_value.c_str());

    if (!m_portsOrch->getPort(entry.bv_id, vlan))
    {
        SWSS_LOG_NOTICE("addFdbEntry: Failed to locate vlan port from bv_id 0x%" PRIx64, entry.bv_id);
        return false;
    }

    /* Retry until port is created */
    if (!m_portsOrch->getPort(port_name, port) || (port.m_bridge_port_id == SAI_NULL_OBJECT_ID))
    {
        SWSS_LOG_INFO("Saving a fdb entry until port %s becomes active", port_name.c_str());
        saved_fdb_entries[port_name].push_back({entry.mac,
                vlan.m_vlan_info.vlan_id, fdbData});
        return true;
    }

    if (fdbData.dest_type == VTEP) {
        /* Assign end point IP only in SIP tunnel scenario since Port + IP address
        needed to uniquely identify Vlan member */
        if (!tunnel_orch->isDipTunnelsSupported())
        {
            end_point_ip = fdbData.dest_value;
        }

        /* Retry until port is member of vlan*/
        if (!m_portsOrch->isVlanMember(vlan, port, end_point_ip))
        {
            SWSS_LOG_INFO("Saving a fdb entry until port %s becomes vlan %s member", port_name.c_str(), vlan.m_alias.c_str());
            saved_fdb_entries[port_name].push_back({entry.mac,
                    vlan.m_vlan_info.vlan_id, fdbData});
            return true;
        }
    }

    sai_status_t status;
    sai_fdb_entry_t fdb_entry;
    fdb_entry.switch_id = gSwitchId;
    memcpy(fdb_entry.mac_address, entry.mac.getMac(), sizeof(sai_mac_t));
    fdb_entry.bv_id = entry.bv_id;

    Port oldPort;
    Port oldVlan;
    string oldType;
    FdbOrigin oldOrigin = FDB_ORIGIN_INVALID ;
    bool macUpdate = false;
    bool macMoveLocalToRemote = false;
    bool macMoveRemoteToLocal = false;
    bool macFlushPending = false;

    auto it = m_entries.find(entry);
    if (it != m_entries.end())
    {
        /* get existing port and type */
        oldType = it->second.type;
        oldOrigin = it->second.origin;

        if (it->second.is_flush_pending) 
        {
            macFlushPending = true;
        }

        if (!m_portsOrch->getPortByBridgePortId(it->second.bridge_port_id, oldPort))
        {
            SWSS_LOG_ERROR("Existing port 0x%" PRIx64 " details not found", it->second.bridge_port_id);
            return false;
        }

        if (!m_portsOrch->getPort(it->first.bv_id, oldVlan))
        {
            SWSS_LOG_NOTICE("addFdbEntry: Failed to locate existing vlan port from bv_id 0x%" PRIx64, it->first.bv_id);
            return false;
        }

        if ((oldOrigin == fdbData.origin) && (oldType == fdbData.type) && (port.m_bridge_port_id == it->second.bridge_port_id)
             && isDestinationSame(it->second, fdbData))
        {
            /* Duplicate Mac */
            SWSS_LOG_INFO("FdbOrch: mac=%s %s port=%s type=%s origin=%d dest_type=%s dest_value=%s is duplicate", entry.mac.to_string().c_str(),
                    vlan.m_alias.c_str(), port_name.c_str(),
                    fdbData.type.c_str(), fdbData.origin, destTypeToString[fdbData.dest_type].c_str(),
                    fdbData.dest_value.c_str());
            return true;
        }
        else if (fdbData.origin != oldOrigin)
        {
            /* Mac origin has changed */
            if ((oldType == "static") && (oldOrigin == FDB_ORIGIN_PROVISIONED))
            {
                /* old mac was static and provisioned, it can not be changed by Remote Mac */
                SWSS_LOG_NOTICE("Already existing static MAC:%s in Vlan:%d. "
                        "Received same MAC from peer, dest_type:%s, dest_value:%s; "
                        "Peer mac ignored",
                        entry.mac.to_string().c_str(), vlan.m_vlan_info.vlan_id,
                        destTypeToString[fdbData.dest_type].c_str(),
                        fdbData.dest_value.c_str());

                return true;
            }
            else if ((oldType == "static") && (oldOrigin ==
                        FDB_ORIGIN_VXLAN_ADVERTIZED) && (fdbData.type == "dynamic"))
            {
                /* old mac was static and received from remote, it can not be changed by dynamic locally provisioned Mac */
                SWSS_LOG_INFO("Already existing static MAC:%s in Vlan:%d "
                        "from Peer: dest_type:%s, dest_value:%s. Now same is provisioned as dynamic; "
                        "Provisioned dynamic mac is ignored",
                        entry.mac.to_string().c_str(), vlan.m_vlan_info.vlan_id,
                        destTypeToString[fdbData.dest_type].c_str(),
                        fdbData.dest_value.c_str());
                return true;
            }
            else if (oldOrigin == FDB_ORIGIN_VXLAN_ADVERTIZED)
            {
                if ((oldType == "static") && (fdbData.type == "static"))
                {
                    SWSS_LOG_WARN("You have just overwritten existing static MAC:%s "
                            "in Vlan:%d from Peer:dest_type:%s, dest_value:%s, "
                            "If it is a mistake, it will result in inconsistent Traffic Forwarding",
                            entry.mac.to_string().c_str(),
                            vlan.m_vlan_info.vlan_id,
                            destTypeToString[fdbData.dest_type].c_str(),
                            fdbData.dest_value.c_str());
                }
            }
            else if ((oldOrigin == FDB_ORIGIN_LEARN) && (fdbData.origin == FDB_ORIGIN_MCLAG_ADVERTIZED))
            {
                if ((port.m_bridge_port_id == it->second.bridge_port_id) && (oldType == "dynamic") && (fdbData.type == "dynamic_local"))
                {
                    SWSS_LOG_INFO("FdbOrch: mac=%s %s port=%s type=%s origin=%d old_origin=%d"
                        " old_type=%s local mac exists,"
                        " received dynamic_local from iccpd, ignore update",
                        entry.mac.to_string().c_str(), vlan.m_alias.c_str(), port_name.c_str(),
                        fdbData.type.c_str(), fdbData.origin, oldOrigin, oldType.c_str());

                    return true;
                }
            }
            else if ((oldOrigin == FDB_ORIGIN_LEARN) && (fdbData.origin == FDB_ORIGIN_VXLAN_ADVERTIZED))
            {
                SWSS_LOG_NOTICE("FdbOrch: mac=%s %s port=%s type=%s origin=%d old_origin=%d"
                        " old_type=%s local mac exists,"
                        " moved from local to remote vxlan vtep",
                        entry.mac.to_string().c_str(), vlan.m_alias.c_str(), port_name.c_str(),
                        fdbData.type.c_str(), fdbData.origin, oldOrigin, oldType.c_str());

                if (isDestinationSame(it->second, fdbData)) {
                    SWSS_LOG_NOTICE("addFdbEntry: EVPN_MH_UC: mac=%s %s port=%s type=%s origin=%d old_origin=%d"
                        " old_type=%s local mac exists, its D -> (C+D) state\n",
                        entry.mac.to_string().c_str(), vlan.m_alias.c_str(), port_name.c_str(),
                        fdbData.type.c_str(), fdbData.origin, oldOrigin, oldType.c_str());
                    m_entries[entry].type = "dynamic_control_learn";
                    return true;
                }
                macMoveLocalToRemote = true; 
            }
        }
        else /* (fdbData.origin == oldOrigin) */
        {
            /* Mac origin is same, all changes are allowed */
            /* Allowed
             * Bridge-port is changed or/and
             * Sticky bit from remote is modified or
             * provisioned mac is converted from static<-->dynamic
             */
            /*
             * when mac move happens for a multihomed host to remote leaf, the local leaf which initially learnt it as static,
             * it needs to be updated for the move since origin will be same again (VXLAN_ADVERTIZED)
             */
             if ((fdbData.origin == FDB_ORIGIN_VXLAN_ADVERTIZED) && (fdbData.origin == oldOrigin) &&
                 (port.m_bridge_port_id != it->second.bridge_port_id))  {
                 //Need to relax this if condition for local to remote move
                 SWSS_LOG_NOTICE("FdbOrch: mac=%s %s port=%s type=%s origin=%d old_origin=%d"
                        " old_type=%s old bridgeport 0x%" PRIx64 "  and new bridgeport 0x%" PRIx64 " "
                        " moved from remote vxlan vtep to local port",
                        entry.mac.to_string().c_str(), vlan.m_alias.c_str(), port_name.c_str(),
                        fdbData.type.c_str(), fdbData.origin, oldOrigin, oldType.c_str(),
                        it->second.bridge_port_id, port.m_bridge_port_id);
                 /* cover:  local static to remote static, or vice versa */
                 macMoveLocalToRemote = true;
             }
        }

        macUpdate = true;
    }

    sai_attribute_t attr;
    vector<sai_attribute_t> attrs;

    attr.id = SAI_FDB_ENTRY_ATTR_TYPE;
    if (fdbData.origin == FDB_ORIGIN_VXLAN_ADVERTIZED)
    {
        attr.value.s32 =  SAI_FDB_ENTRY_TYPE_STATIC;
    }
    else if (fdbData.origin == FDB_ORIGIN_MCLAG_ADVERTIZED)
    {
        attr.value.s32 = (fdbData.type == "dynamic_local") ? SAI_FDB_ENTRY_TYPE_DYNAMIC : SAI_FDB_ENTRY_TYPE_STATIC;
    }
    else
    {
        attr.value.s32 = (fdbData.type == "dynamic") ? SAI_FDB_ENTRY_TYPE_DYNAMIC : SAI_FDB_ENTRY_TYPE_STATIC;
    }
    fdbData.sai_fdb_type = (sai_fdb_entry_type_t)attr.value.s32;

    attrs.push_back(attr);

    if (((fdbData.origin == FDB_ORIGIN_VXLAN_ADVERTIZED) || (fdbData.origin == FDB_ORIGIN_MCLAG_ADVERTIZED))
            && (fdbData.type == "dynamic" || fdbData.type == "dynamic_control_learn"))
    {
        attr.id = SAI_FDB_ENTRY_ATTR_ALLOW_MAC_MOVE;
        attr.value.booldata = true;
        attrs.push_back(attr);
        fdbData.allow_mac_move = true;
    }

    attr.id = SAI_FDB_ENTRY_ATTR_BRIDGE_PORT_ID;
    attr.value.oid = port.m_bridge_port_id;
    attrs.push_back(attr);

    if (fdbData.origin == FDB_ORIGIN_VXLAN_ADVERTIZED)
    {
        // SingleHoming: MAC -> remote_vtep
        // Multihoming - Remote MAC with no local ESI: MAC -> NHGROUP
        // Multihoming - Remote MAC with local ESI:    MAC -> ifname
        // (just SAI_FDB_ENTRY_ATTR_BRIDGE_PORT_ID is enough, no need to set vtep or nhgroup attr)
        if (fdbData.dest_type == VTEP) {
            IpAddress remote = IpAddress(fdbData.dest_value);
            sai_ip_address_t ipaddr;
            if (remote.isV4())
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

        if (fdbData.dest_type == VTEP || fdbData.dest_type == NEXTHOPGROUP) {
            /* Try to remvoe local neighbor entry if exists
            * Since this mac is at the remote vxlan side now
            */
            gNeighOrch->processFDBDelete(entry);
        }
    }
    else if (macUpdate
            && (oldOrigin == FDB_ORIGIN_VXLAN_ADVERTIZED)
            && (fdbData.origin != oldOrigin))
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

    if (macUpdate && (oldOrigin == FDB_ORIGIN_VXLAN_ADVERTIZED))
    {
        if ((fdbData.origin != oldOrigin)
           || ((oldType == "dynamic") && (oldType != fdbData.type)))
        {
            attr.id = SAI_FDB_ENTRY_ATTR_ALLOW_MAC_MOVE;
            attr.value.booldata = false;
            attrs.push_back(attr);
        }
    }
    attr.id = SAI_FDB_ENTRY_ATTR_PACKET_ACTION;
    attr.value.s32 = (fdbData.discard == "true") ? SAI_PACKET_ACTION_DROP: SAI_PACKET_ACTION_FORWARD;
    attrs.push_back(attr);
    if (macUpdate && !macMoveLocalToRemote && !macMoveRemoteToLocal)
    {
        SWSS_LOG_INFO("MAC-Update FDB %s in %s on from-%s:to-%s from-%s:to-%s origin-%d-to-%d",
                entry.mac.to_string().c_str(), vlan.m_alias.c_str(), oldPort.m_alias.c_str(),
                port_name.c_str(), oldType.c_str(), fdbData.type.c_str(),
                oldOrigin, fdbData.origin);
        for (auto itr : attrs)
        {
            status = sai_fdb_api->set_fdb_entry_attribute(&fdb_entry, &itr);
            if (status != SAI_STATUS_SUCCESS)
            {
                SWSS_LOG_ERROR("macUpdate-Failed for attr.id=0x%x for FDB %s in %s on %s, rv:%d",
                            itr.id, entry.mac.to_string().c_str(), vlan.m_alias.c_str(), port_name.c_str(), status);
                task_process_status handle_status = handleSaiSetStatus(SAI_API_FDB, status);
                if (handle_status != task_success)
                {
                    return parseHandleSaiStatusFailure(handle_status);
                }
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
        if (macMoveLocalToRemote || macMoveRemoteToLocal)
        {
            status = sai_fdb_api->remove_fdb_entry(&fdb_entry);

            if ((macFlushPending == true) && (status == SAI_STATUS_ITEM_NOT_FOUND))
            {
                /* Since the fdb is in flusing, ignoring the NOT_FOUND status */
                SWSS_LOG_NOTICE("FdbOrch: fdb is flush pending, ignore NOT_FOUND. mac=%s, bv_id=0x%" PRIx64,
                                 entry.mac.to_string().c_str(), entry.bv_id);
            }
            else if (status != SAI_STATUS_SUCCESS)
            {
                SWSS_LOG_ERROR("FdbOrch RemoveFDBEntry: Failed to remove FDB entry. mac=%s, bv_id=0x%" PRIx64,
                               entry.mac.to_string().c_str(), entry.bv_id);
                task_process_status handle_status = handleSaiRemoveStatus(SAI_API_FDB, status); 
                if (handle_status != task_success)
                {
                    return parseHandleSaiStatusFailure(handle_status);
                }
            }

            if (!oldPort.m_alias.empty()) {
                oldPort.m_fdb_count--;
                m_portsOrch->setPort(oldPort.m_alias, oldPort);
            }
            if (!oldVlan.m_alias.empty()) {
                oldVlan.m_fdb_count--;
                SWSS_LOG_DEBUG("mac moved, oldvlan %s, m_fdb_count %d", oldVlan.m_alias.c_str(), oldVlan.m_fdb_count);
                m_portsOrch->setPort(oldVlan.m_alias, oldVlan);
            }

            // Remove the existing entry since its port_name is changed
            (void)m_entries.erase(entry);

            notifyTunnelOrch(oldPort);
            SWSS_LOG_INFO("FdbOrch Removed old entry, mac=%s bv_id=0x%" PRIx64 " port:%s",
                           entry.mac.to_string().c_str(), entry.bv_id, port.m_alias.c_str());
        }

        SWSS_LOG_INFO("FdbOrch MAC-Create %s FDB %s in %s on %s", fdbData.type.c_str(), entry.mac.to_string().c_str(), vlan.m_alias.c_str(), port_name.c_str());

        status = sai_fdb_api->create_fdb_entry(&fdb_entry, (uint32_t)attrs.size(), attrs.data());
        if (status != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_ERROR("Failed to create %s FDB %s in %s on %s, rv:%d",
                    fdbData.type.c_str(), entry.mac.to_string().c_str(),
                    vlan.m_alias.c_str(), port_name.c_str(), status);
            task_process_status handle_status = handleSaiCreateStatus(SAI_API_FDB, status); //FIXME: it should be based on status. Some could be retried, some not
            if (handle_status != task_success)
            {
                return parseHandleSaiStatusFailure(handle_status);
            }
        }
        port.m_fdb_count++;
        m_portsOrch->setPort(port.m_alias, port);

		// Re-get to vlan to get the latest fdb count
        if (m_portsOrch->getPort(entry.bv_id, vlan))
        {
            vlan.m_fdb_count++;
            m_portsOrch->setPort(vlan.m_alias, vlan);

            SWSS_LOG_DEBUG("Re-set count after creating static FDB, vlan %s, m_fdb_count %d", vlan.m_alias.c_str(), vlan.m_fdb_count);
        }
    }

    FdbData storeFdbData = fdbData;
    storeFdbData.bridge_port_id = port.m_bridge_port_id;
    // overwrite the type and origin
    if ((fdbData.origin == FDB_ORIGIN_MCLAG_ADVERTIZED) && (fdbData.type == "dynamic_local"))
    {
        //If the MAC is dynamic_local change the origin accordingly
        //MAC is added/updated as dynamic to allow aging.
        SWSS_LOG_INFO("MAC-Update Modify to dynamic FDB %s in %s on from-%s:to-%s from-%s:to-%s origin-%d-to-%d",
                entry.mac.to_string().c_str(), vlan.m_alias.c_str(), oldPort.m_alias.c_str(),
                port_name.c_str(), oldType.c_str(), fdbData.type.c_str(), 
                oldOrigin, fdbData.origin);

        storeFdbData.origin = FDB_ORIGIN_LEARN;
        storeFdbData.type = "dynamic";
    }

    if (oldPort.m_type != Port::UNKNOWN && port.m_bridge_port_id != oldPort.m_bridge_port_id) {
        removeFdbEntryFromPortCache(entry, oldPort);
    }

    m_entries[entry] = storeFdbData;

    m_entries_by_port[port.m_alias].push_back(entry);

    string key = "Vlan" + to_string(vlan.m_vlan_info.vlan_id) + ":" + entry.mac.to_string();

    if (((fdbData.origin != FDB_ORIGIN_MCLAG_ADVERTIZED) &&
         (fdbData.origin != FDB_ORIGIN_VXLAN_ADVERTIZED)) ||
        ((fdbData.origin == FDB_ORIGIN_MCLAG_ADVERTIZED) &&
          (fdbData.type == "dynamic_local")))
    {
        /* State-DB is updated only for Local Mac addresses */
        // Write to StateDb
        std::vector<FieldValueTuple> fvs;
        fvs.push_back(FieldValueTuple("port", port_name));
        if (fdbData.type == "dynamic_local")
            fvs.push_back(FieldValueTuple("type", "dynamic"));
        else
            fvs.push_back(FieldValueTuple("type", fdbData.type));
        m_fdbStateTable.set(key, fvs);
    }

    else if (macUpdate && (oldOrigin != FDB_ORIGIN_MCLAG_ADVERTIZED) &&
            (oldOrigin != FDB_ORIGIN_VXLAN_ADVERTIZED))
    {
        /* origin is FDB_ORIGIN_ADVERTIZED and it is mac-update
         * so delete from StateDb since we only keep local fdbs
         * in state-db
         */
        m_fdbStateTable.del(key);
    }

    if ((fdbData.origin == FDB_ORIGIN_MCLAG_ADVERTIZED) && (fdbData.type != "dynamic_local"))
    {
        std::vector<FieldValueTuple> fvs;
        fvs.push_back(FieldValueTuple("port", port_name));
        fvs.push_back(FieldValueTuple("type", fdbData.type));
        m_mclagFdbStateTable.set(key, fvs);

        SWSS_LOG_NOTICE("fdbEvent: AddFdbEntry: Add MCLAG MAC with state mclag remote fdb table "
              "Mac: %s Vlan: %d port:%s type:%s", entry.mac.to_string().c_str(),
              vlan.m_vlan_info.vlan_id, port_name.c_str(), fdbData.type.c_str());
    }
    else if (macUpdate && (oldOrigin == FDB_ORIGIN_MCLAG_ADVERTIZED) &&
            (fdbData.origin != FDB_ORIGIN_MCLAG_ADVERTIZED))
    {
        SWSS_LOG_NOTICE("fdbEvent: AddFdbEntry: del MCLAG MAC from state MCLAG remote fdb table "
                    "Mac: %s Vlan: %d port:%s type:%s", entry.mac.to_string().c_str(),
                    vlan.m_vlan_info.vlan_id, port_name.c_str(), fdbData.type.c_str());
        m_mclagFdbStateTable.del(key);
    }

    if (!macUpdate)
    {
        gCrmOrch->incCrmResUsedCounter(CrmResourceType::CRM_FDB_ENTRY);
    }

    FdbUpdate update;
    update.entry = entry;
    update.port = port;
    update.type = fdbData.type;
    update.add = true;

    notify(SUBJECT_TYPE_FDB_CHANGE, &update);

    return true;
}

bool FdbOrch::removeFdbEntry(const FdbEntry& entry, FdbOrigin origin)
{
    Port vlan;
    Port port;

    SWSS_LOG_ENTER();

    SWSS_LOG_INFO("FdbOrch: EVPN_MH_UC: RemoveFDBEntry: mac=%s bv_id=0x%" PRIx64 "origin %d", entry.mac.to_string().c_str(), entry.bv_id, origin);
    if (!m_portsOrch->getPort(entry.bv_id, vlan))
    {
        SWSS_LOG_INFO("FdbOrch notification: Failed to locate vlan port from bv_id 0x%" PRIx64, entry.bv_id);
        return false;
    }

    auto it= m_entries.find(entry);
    if (it == m_entries.end())
    {
        SWSS_LOG_INFO("FdbOrch RemoveFDBEntry: FDB entry isn't found. mac=%s bv_id=0x%" PRIx64, entry.mac.to_string().c_str(), entry.bv_id);

        /* check whether the entry is in the saved fdb, if so delete it from there. */
        deleteFdbEntryFromSavedFDB(entry.mac, vlan.m_vlan_info.vlan_id, origin);
        return true;
    }

    FdbData fdbData = it->second;
    if (!m_portsOrch->getPortByBridgePortId(fdbData.bridge_port_id, port))
    {
        SWSS_LOG_NOTICE("FdbOrch RemoveFDBEntry: Failed to locate port from bridge_port_id 0x%" PRIx64, fdbData.bridge_port_id);
        if (it->second.is_flush_pending) {
            /* when remove bridge port, we have triggered a FLUSH. 
               Here clear the fdb entry which is in flush pending to avoid the missing flush event case */
            SWSS_LOG_NOTICE("FdbOrch RemoveFDBEntry: FDB has been flushed, mac=%s bv_id=0x%" PRIx64, entry.mac.to_string().c_str(), entry.bv_id);
            clearFdbEntry(it->first, it->second);
            return true;
        }

        return false;
    }

    if (fdbData.origin != origin)
    {
        if ((origin == FDB_ORIGIN_VXLAN_ADVERTIZED) && (fdbData.origin == FDB_ORIGIN_LEARN)) {
            SWSS_LOG_NOTICE("RemoveFDBEntry: EVPN_MH_UC: (C+D) / D -> C : mac=%s fdb origin is different; found_origin:%d delete_origin:%d",
                    entry.mac.to_string().c_str(), origin, fdbData.origin);
            if (fdbData.type == "dynamic_control_learn") {
                if (fdbData.dest_type == IFNAME) {
                    m_entries[entry].type = "dynamic";
                    return true;
                } else {
                    SWSS_LOG_ERROR("RemoveFDBEntry: EVPN_MH_UC: invalid dest_type=%s for MAC=%s, bv_id=0x%" PRIx64, destTypeToString[fdbData.dest_type].c_str(), entry.mac.to_string().c_str(), entry.bv_id);
                }
            }
        }

        if ((origin == FDB_ORIGIN_MCLAG_ADVERTIZED) && (fdbData.origin == FDB_ORIGIN_LEARN) &&
                        (port.m_oper_status == SAI_PORT_OPER_STATUS_DOWN) && (gMlagOrch->isMlagInterface(port.m_alias)))
        {
            //check if the local MCLAG port is down, if yes then continue delete the local MAC
            origin = FDB_ORIGIN_LEARN;
            SWSS_LOG_INFO("FdbOrch RemoveFDBEntry: mac=%s fdb del origin is MCLAG; delete local mac as port %s is down",
                entry.mac.to_string().c_str(), port.m_alias.c_str());
        }
        else
        {

            /* When mac is moved from remote to local
             * BGP will delete the mac from vxlan_fdb_table
             * but we should not delete this mac here since now
             * mac in orchagent represents locally learnt
             */
            SWSS_LOG_INFO("FdbOrch RemoveFDBEntry: mac=%s fdb origin is different; found_origin:%d delete_origin:%d",
                    entry.mac.to_string().c_str(), origin, fdbData.origin);

            /* We may still have the mac in saved-fdb probably due to unavailability
             * of bridge-port. check whether the entry is in the saved fdb,
             * if so delete it from there. */
            deleteFdbEntryFromSavedFDB(entry.mac, vlan.m_vlan_info.vlan_id, origin);

            return true;
        }
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
        SWSS_LOG_ERROR("FdbOrch RemoveFDBEntry: Failed to remove FDB entry. mac=%s, bv_id=0x%" PRIx64,
                       entry.mac.to_string().c_str(), entry.bv_id);
        task_process_status handle_status = handleSaiRemoveStatus(SAI_API_FDB, status); //FIXME: it should be based on status. Some could be retried. some not
        if (handle_status != task_success)
        {
            return parseHandleSaiStatusFailure(handle_status);
        }
    }

    SWSS_LOG_INFO("Removed mac=%s bv_id=0x%" PRIx64 " port:%s",
            entry.mac.to_string().c_str(), entry.bv_id, port.m_alias.c_str());

    port.m_fdb_count--;
    m_portsOrch->setPort(port.m_alias, port);
    vlan.m_fdb_count--;
    SWSS_LOG_DEBUG("after removing fdb, vlan %s, m_fdb_count %d", vlan.m_alias.c_str(), vlan.m_fdb_count);
    m_portsOrch->setPort(vlan.m_alias, vlan);
    (void)m_entries.erase(entry);
    removeFdbEntryFromPortCache(entry, port);

    // Remove in StateDb
    if ((fdbData.origin != FDB_ORIGIN_VXLAN_ADVERTIZED) && (fdbData.origin != FDB_ORIGIN_MCLAG_ADVERTIZED))
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

void FdbOrch::deleteFdbEntryFromSavedFDB(const MacAddress &mac,
        const unsigned short &vlanId, FdbOrigin origin, const string portName)
{
    bool found=false;
    SavedFdbEntry entry;
    entry.mac = mac;
    entry.vlanId = vlanId;
    entry.fdbData.type = "static";
    /* Below members are unused during delete compare */
    entry.fdbData.origin = origin;

    for (auto& itr: saved_fdb_entries)
    {
        if (portName.empty() || (portName == itr.first))
        {
            auto iter = saved_fdb_entries[itr.first].begin();
            while(iter != saved_fdb_entries[itr.first].end())
            {
                if (*iter == entry)
                {
                    if (iter->fdbData.origin == origin)
                    {
                        SWSS_LOG_INFO("FDB entry found in saved fdb. deleting..."
                                "mac=%s vlan_id=0x%x origin:%d port:%s",
                                mac.to_string().c_str(), vlanId, origin,
                                itr.first.c_str());
                        saved_fdb_entries[itr.first].erase(iter);

                        found=true;
                        break;
                    }
                    else
                    {
                        SWSS_LOG_INFO("FDB entry found in saved fdb, but Origin is "
                                "different mac=%s vlan_id=0x%x reqOrigin:%d "
                                "foundOrigin:%d port:%s, IGNORED",
                                mac.to_string().c_str(), vlanId, origin,
                                iter->fdbData.origin, itr.first.c_str());
                    }
                }
                iter++;
            }
        }
        if (found)
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

    SWSS_LOG_NOTICE("Try to delete tunnel port %s",port.m_alias.c_str());
    tunnel_orch->deleteTunnelPort(port);
}
